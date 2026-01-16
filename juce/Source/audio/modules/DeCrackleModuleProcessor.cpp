#include "DeCrackleModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

DeCrackleModuleProcessor::DeCrackleModuleProcessor()
    : ModuleProcessor (BusesProperties()
                        .withInput ("Input", juce::AudioChannelSet::discreteChannels(5), true) // ch0-1 audio, ch2-4 mods
                        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "DeCrackleParams", createParameterLayout())
{
    thresholdParam = apvts.getRawParameterValue("threshold");
    smoothingTimeMsParam = apvts.getRawParameterValue("smoothing_time");
    amountParam = apvts.getRawParameterValue("amount");
    
    // Initialize output value tracking for tooltips
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R

#if defined(PRESET_CREATOR_UI)
    for (auto& sample : vizData.dryWave) sample.store(0.0f);
    for (auto& sample : vizData.wetWave) sample.store(0.0f);
    for (auto& sample : vizData.crackleMask) sample.store(0.0f);
    for (auto& sample : vizData.crackleHistory) sample.store(0.0f);
    vizData.historyWriteIndex.store(0);
    crackleBinScratch.assign(VizData::waveformPoints, 0);
#endif
}

juce::AudioProcessorValueTreeState::ParameterLayout DeCrackleModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    // Threshold: 0.01 to 1.0
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "threshold", "Threshold",
        juce::NormalisableRange<float>(0.01f, 1.0f),
        0.1f));
    
    // Smoothing time: 0.1ms to 20.0ms (logarithmic)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "smoothing_time", "Smoothing Time",
        juce::NormalisableRange<float>(0.1f, 20.0f, 0.0f, 0.3f),
        5.0f));
    
    // Amount (dry/wet): 0.0 to 1.0
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "amount", "Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        1.0f));
    
    return { params.begin(), params.end() };
}

void DeCrackleModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;
    
    // Reset state
    for (int ch = 0; ch < 2; ++ch)
    {
        lastInputSample[ch] = 0.0f;
        lastOutputSample[ch] = 0.0f;
        smoothingSamplesRemaining[ch] = 0;
    }

#if defined(PRESET_CREATOR_UI)
    dryCapture.setSize(2, samplesPerBlock);
    dryCapture.clear();
    wetCapture.setSize(2, samplesPerBlock);
    wetCapture.clear();
    crackleBinScratch.assign(VizData::waveformPoints, 0);
#endif
}

void DeCrackleModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    
    auto in = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);
    
    const int nSamps = buffer.getNumSamples();
    if (nSamps <= 0)
        return;
    const int numChannels = juce::jmin(out.getNumChannels(), 2);
    
#if defined(PRESET_CREATOR_UI)
    const int captureChannels = juce::jmin(2, in.getNumChannels());
    if (dryCapture.getNumSamples() < nSamps)
        dryCapture.setSize(2, nSamps, false, false, true);
    if (wetCapture.getNumSamples() < nSamps)
        wetCapture.setSize(2, nSamps, false, false, true);
    dryCapture.clear();
    wetCapture.clear();
    for (int ch = 0; ch < captureChannels; ++ch)
        dryCapture.copyFrom(ch, 0, in, ch, 0, nSamps);
    if (captureChannels == 1 && dryCapture.getNumChannels() > 1)
        dryCapture.copyFrom(1, 0, dryCapture, 0, 0, nSamps);
    if ((int)crackleBinScratch.size() != VizData::waveformPoints)
        crackleBinScratch.assign(VizData::waveformPoints, 0);
    else
        std::fill(crackleBinScratch.begin(), crackleBinScratch.end(), 0);
    int smoothingActiveSamples = 0;
#endif
    int crackleEventsThisBlock = 0;
    
    // Check if modulation inputs are connected
    const bool isThresholdMod = isParamInputConnected("threshold_mod");
    const bool isSmoothingMod = isParamInputConnected("smoothing_time_mod");
    const bool isAmountMod = isParamInputConnected("amount_mod");
    
    // Get pointers to modulation CV inputs, if they are connected
    const float* thresholdCV = isThresholdMod && in.getNumChannels() > 2 ? in.getReadPointer(2) : nullptr;
    const float* smoothingCV = isSmoothingMod && in.getNumChannels() > 3 ? in.getReadPointer(3) : nullptr;
    const float* amountCV = isAmountMod && in.getNumChannels() > 4 ? in.getReadPointer(4) : nullptr;
    
    // Get base parameter values
    const float baseThreshold = thresholdParam != nullptr ? thresholdParam->load() : 0.1f;
    const float baseSmoothingMs = smoothingTimeMsParam != nullptr ? smoothingTimeMsParam->load() : 5.0f;
    const float baseAmount = amountParam != nullptr ? amountParam->load() : 1.0f;
    
    // Parameter ranges for modulation scaling
    const float thresholdMin = 0.01f;
    const float thresholdMax = 1.0f;
    const float smoothingMin = 0.1f;
    const float smoothingMax = 20.0f;
    const float amountMin = 0.0f;
    const float amountMax = 1.0f;
    
    // Calculate smoothing coefficient
    // Use a fixed, fast coefficient for the smoothing
    const float smoothingCoeff = 0.1f;
    
    // Track live values for UI
    float liveThreshold = baseThreshold;
    float liveSmoothingMs = baseSmoothingMs;
    float liveAmount = baseAmount;
    
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* input = in.getReadPointer(juce::jmin(ch, in.getNumChannels() - 1));
        float* output = out.getWritePointer(ch);
        
        for (int i = 0; i < nSamps; ++i)
        {
            // Calculate effective parameters FOR THIS SAMPLE
            float threshold = baseThreshold;
            if (isThresholdMod && thresholdCV != nullptr) {
                const float cv = juce::jlimit(0.0f, 1.0f, thresholdCV[i]);
                threshold = thresholdMin + cv * (thresholdMax - thresholdMin);
            }
            
            float smoothingMs = baseSmoothingMs;
            if (isSmoothingMod && smoothingCV != nullptr) {
                const float cv = juce::jlimit(0.0f, 1.0f, smoothingCV[i]);
                // Logarithmic scaling for smoothing time
                smoothingMs = smoothingMin * std::pow(smoothingMax / smoothingMin, cv);
            }
            
            float wet = baseAmount;
            if (isAmountMod && amountCV != nullptr) {
                wet = juce::jlimit(0.0f, 1.0f, amountCV[i]);
            }
            const float dry = 1.0f - wet;
            
            // Update live values (use last sample for UI display)
            if (i == nSamps - 1) {
                liveThreshold = threshold;
                liveSmoothingMs = smoothingMs;
                liveAmount = wet;
            }
            
            float inputSample = input[i];
            
            // 1. Detect Crackle (discontinuity)
            float delta = std::abs(inputSample - lastInputSample[ch]);
            if (delta > threshold)
            {
                // A crackle is detected. Activate smoothing for a short period.
                smoothingSamplesRemaining[ch] = static_cast<int>(smoothingMs * 0.001f * currentSampleRate);
                ++crackleEventsThisBlock;
#if defined(PRESET_CREATOR_UI)
                const float norm = (float)i / (float)juce::jmax(1, nSamps);
                int bin = (int)std::floor(norm * (float)VizData::waveformPoints);
                bin = juce::jlimit(0, VizData::waveformPoints - 1, bin);
                crackleBinScratch[(size_t)bin] += 1;
#endif
            }
            
            // 2. Apply Smoothing if Active
            float processedSample;
            if (smoothingSamplesRemaining[ch] > 0)
            {
                // Apply fast slew to smooth the transition
                lastOutputSample[ch] += (inputSample - lastOutputSample[ch]) * smoothingCoeff;
                processedSample = lastOutputSample[ch];
                smoothingSamplesRemaining[ch]--;
#if defined(PRESET_CREATOR_UI)
                ++smoothingActiveSamples;
#endif
            }
            else
            {
                // No smoothing needed, output is the same as input
                processedSample = inputSample;
                lastOutputSample[ch] = inputSample;
            }
            
            // 3. Apply Dry/Wet Mix
            output[i] = (inputSample * dry) + (processedSample * wet);
            
            // 4. Store last input sample for the next iteration's delta calculation
            lastInputSample[ch] = inputSample;
        }
    }
    
    // Update output values for tooltips
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0]) lastOutputValues[0]->store(out.getSample(0, nSamps - 1));
        if (lastOutputValues[1] && numChannels > 1) lastOutputValues[1]->store(out.getSample(1, nSamps - 1));
    }

#if defined(PRESET_CREATOR_UI)
    const int processedChannels = juce::jmin(2, out.getNumChannels());
    for (int ch = 0; ch < processedChannels; ++ch)
        wetCapture.copyFrom(ch, 0, out, ch, 0, nSamps);
    if (processedChannels == 1 && wetCapture.getNumChannels() > 1)
        wetCapture.copyFrom(1, 0, wetCapture, 0, 0, nSamps);

    auto downsampleBuffer = [&](const juce::AudioBuffer<float>& src, std::array<std::atomic<float>, VizData::waveformPoints>& dest)
    {
        if (src.getNumSamples() <= 0)
        {
            for (auto& sample : dest) sample.store(0.0f);
            return;
        }
        const int totalSamples = src.getNumSamples();
        const int channelsAvailable = juce::jmin(2, src.getNumChannels());
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float t = (float)i / (float)(VizData::waveformPoints - 1);
            const int sampleIndex = juce::jlimit(0, totalSamples - 1, (int)std::round(t * (float)(totalSamples - 1)));
            float left = src.getSample(0, sampleIndex);
            float right = channelsAvailable > 1 ? src.getSample(1, sampleIndex) : left;
            dest[(size_t)i].store((left + right) * 0.5f);
        }
    };

    downsampleBuffer(dryCapture, vizData.dryWave);
    downsampleBuffer(wetCapture, vizData.wetWave);
    for (int i = 0; i < VizData::waveformPoints; ++i)
        vizData.crackleMask[(size_t)i].store(crackleBinScratch[(size_t)i] > 0 ? 1.0f : 0.0f);

    const float blockDuration = (float)nSamps / (float)juce::jmax(1.0, currentSampleRate);
    const float cracklePerSec = blockDuration > 0.0f ? (float)crackleEventsThisBlock / blockDuration : 0.0f;
    vizData.crackleRatePerSec.store(cracklePerSec);
    vizData.smoothingMsLive.store(liveSmoothingMs);
    vizData.amountLive.store(liveAmount);
    
    // Store live values for UI telemetry
    setLiveParamValue("threshold_live", liveThreshold);
    setLiveParamValue("smoothing_time_live", liveSmoothingMs);
    setLiveParamValue("amount_live", liveAmount);
    const int totalSamplesConsidered = juce::jmax(1, nSamps * juce::jmax(1, numChannels));
    vizData.smoothingActiveRatio.store((float)smoothingActiveSamples / (float)totalSamplesConsidered);

    const float normalizedCrackle = juce::jlimit(0.0f, 1.0f, cracklePerSec / 200.0f);
    vizData.crackleHistory[(size_t)crackleHistoryWrite].store(normalizedCrackle);
    crackleHistoryWrite = (crackleHistoryWrite + 1) % VizData::historySize;
    vizData.historyWriteIndex.store(crackleHistoryWrite);
#endif
}

#if defined(PRESET_CREATOR_UI)
bool DeCrackleModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0;
    if (paramId == "threshold_mod")      { outChannelIndexInBus = 2; return true; }
    if (paramId == "smoothing_time_mod")  { outChannelIndexInBus = 3; return true; }
    if (paramId == "amount_mod")          { outChannelIndexInBus = 4; return true; }
    return false;
}

void DeCrackleModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    const auto& freqColors = theme.modules.frequency_graph;
    auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
    const ImU32 bgColor    = resolveColor(freqColors.background, IM_COL32(20, 22, 26, 255));
    const ImU32 gridColor  = resolveColor(freqColors.grid, IM_COL32(55, 60, 70, 255));
    const ImU32 dryColor   = resolveColor(freqColors.live_line, IM_COL32(120, 180, 255, 230));
    const ImU32 wetColor   = resolveColor(freqColors.peak_line, IM_COL32(255, 150, 90, 230));
    const ImU32 maskColor  = IM_COL32(255, 90, 120, 200);
    const ImU32 historyBg  = resolveColor(freqColors.background, IM_COL32(25, 27, 32, 255));
    const ImU32 historyLine = resolveColor(freqColors.live_line, IM_COL32(120, 200, 255, 200));
    const ImU32 historyPeak = resolveColor(freqColors.peak_line, IM_COL32(255, 150, 80, 200));
    const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);

    std::array<float, VizData::waveformPoints> dryWave {};
    std::array<float, VizData::waveformPoints> wetWave {};
    std::array<float, VizData::waveformPoints> crackMask {};
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        dryWave[(size_t)i] = vizData.dryWave[(size_t)i].load();
        wetWave[(size_t)i] = vizData.wetWave[(size_t)i].load();
        crackMask[(size_t)i] = vizData.crackleMask[(size_t)i].load();
    }
    std::array<float, VizData::historySize> crackleHistory {};
    for (int i = 0; i < VizData::historySize; ++i)
        crackleHistory[(size_t)i] = vizData.crackleHistory[(size_t)i].load();
    const int historyWrite = vizData.historyWriteIndex.load();
    const float crackleRate = vizData.crackleRatePerSec.load();
    const float smoothingMs = vizData.smoothingMsLive.load();
    const float amountLive = vizData.amountLive.load();
    const float smoothingRatio = vizData.smoothingActiveRatio.load();

    auto HelpMarker = [](const char* desc)
    {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginChild("DeCrackleWaveformViz", ImVec2(itemWidth, 160.0f), false, childFlags))
    {
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 childSize = ImGui::GetWindowSize();
        const ImVec2 p1 { p0.x + childSize.x, p0.y + childSize.y };
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        drawList->PushClipRect(p0, p1, true);

        auto xToScreen = [&](int index)
        {
            return p0.x + juce::jmap((float)index, 0.0f, (float)(VizData::waveformPoints - 1), 8.0f, childSize.x - 8.0f);
        };
        auto yToScreen = [&](float sample)
        {
            const float clamped = juce::jlimit(-1.2f, 1.2f, sample);
            return juce::jmap(clamped, 1.2f, -1.2f, p0.y + 10.0f, p1.y - 10.0f);
        };

        auto drawWave = [&](const std::array<float, VizData::waveformPoints>& data, ImU32 color, float thickness)
        {
            float prevX = xToScreen(0);
            float prevY = yToScreen(data[0]);
            for (int i = 1; i < VizData::waveformPoints; ++i)
            {
                const float x = xToScreen(i);
                const float y = yToScreen(data[(size_t)i]);
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), color, thickness);
                prevX = x;
                prevY = y;
            }
        };

        // Grid lines
        drawList->AddLine(ImVec2(p0.x, yToScreen(0.0f)), ImVec2(p1.x, yToScreen(0.0f)), gridColor);
        drawWave(dryWave, historyLine, 1.3f);
        drawWave(wetWave, historyPeak, 2.0f);

        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            if (crackMask[(size_t)i] > 0.0f)
            {
                const float x = xToScreen(i);
                drawList->AddLine(ImVec2(x, p0.y + 8.0f), ImVec2(x, p1.y - 8.0f), maskColor, 1.0f);
            }
        }

        drawList->PopClipRect();
        drawList->AddText(ImVec2(p0.x + 10.0f, p0.y + 8.0f), IM_COL32(220, 220, 230, 255),
                          (juce::String("Dry vs Processed  |  Crackle Rate ") + juce::String(crackleRate, 1) + " /s").toRawUTF8());

        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("WaveformDragBlocker", childSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::BeginChild("DeCrackleHistory", ImVec2(itemWidth, 70.0f), false, childFlags))
    {
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 childSize = ImGui::GetWindowSize();
        const ImVec2 p1 { p0.x + childSize.x, p0.y + childSize.y };
        drawList->AddRectFilled(p0, p1, historyBg, 3.0f);
        drawList->PushClipRect(p0, p1, true);

        auto idxToValue = [&](int visualIndex)
        {
            const int idx = (historyWrite + visualIndex) % VizData::historySize;
            return crackleHistory[(size_t)idx];
        };

        float prevX = p0.x + 6.0f;
        float prevY = p1.y - 8.0f;
        for (int i = 0; i < VizData::historySize; ++i)
        {
            const float normalized = idxToValue(i);
            const float x = juce::jmap((float)i, 0.0f, (float)(VizData::historySize - 1), p0.x + 6.0f, p1.x - 6.0f);
            const float y = juce::jmap(normalized, 0.0f, 1.0f, p1.y - 8.0f, p0.y + 10.0f);
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), historyLine, 2.0f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();
        drawList->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f), IM_COL32(210, 210, 220, 255), "Crackle Activity");

        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("HistoryDragBlocker", childSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::BeginChild("DeCrackleStats", ImVec2(itemWidth, 60.0f), false, childFlags))
    {
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 childSize = ImGui::GetWindowSize();
        const ImVec2 p1 { p0.x + childSize.x, p0.y + childSize.y };
        drawList->AddRectFilled(p0, p1, bgColor, 3.0f);
        const float barWidth = juce::jmax(20.0f, childSize.x - 20.0f);
        const float ratio = juce::jlimit(0.0f, 1.0f, smoothingRatio);
        drawList->AddRectFilled(ImVec2(p0.x + 10.0f, p0.y + 30.0f),
                                ImVec2(p0.x + 10.0f + barWidth * ratio, p0.y + 46.0f),
                                accentColor, 3.0f);
        drawList->AddRect(ImVec2(p0.x + 10.0f, p0.y + 30.0f),
                          ImVec2(p0.x + 10.0f + barWidth, p0.y + 46.0f),
                          IM_COL32(0, 0, 0, 100), 3.0f);
        drawList->AddText(ImVec2(p0.x + 12.0f, p0.y + 8.0f), IM_COL32(220, 220, 230, 255),
                          (juce::String("Smoothing active ") + juce::String(ratio * 100.0f, 1) + "% of block").toRawUTF8());
        drawList->AddText(ImVec2(p0.x + 12.0f, p0.y + 36.0f), IM_COL32(190, 190, 200, 255),
                          (juce::String("Live smoothing: ") + juce::String(smoothingMs, 2) + " ms    Mix: " + juce::String(amountLive * 100.0f, 0) + "%").toRawUTF8());

        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("StatsDragBlocker", childSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ThemeText("De-Crackler Controls", theme.text.section_header);
    ImGui::Spacing();

    // Threshold slider with inline pin
    bool isThresholdModulated = isParamModulated("threshold_mod");
    float threshold = thresholdParam != nullptr ? thresholdParam->load() : 0.1f;
    if (isThresholdModulated) {
        threshold = getLiveParamValueFor("threshold_mod", "threshold_live", threshold);
    }
    
    // Inline pin for Threshold Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }
    
    if (isThresholdModulated) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Threshold", &threshold, 0.01f, 1.0f, "%.3f"))
        if (!isThresholdModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("threshold")))
                *p = threshold;
    adjustParamOnWheel(ap.getParameter("threshold"), "threshold", threshold);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isThresholdModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarker("Crackle detection sensitivity.\nLower = more sensitive, higher = ignores smaller glitches.");

    // Smoothing Time slider with inline pin
    bool isSmoothingModulated = isParamModulated("smoothing_time_mod");
    float smoothingTime = smoothingTimeMsParam != nullptr ? smoothingTimeMsParam->load() : 5.0f;
    if (isSmoothingModulated) {
        smoothingTime = getLiveParamValueFor("smoothing_time_mod", "smoothing_time_live", smoothingTime);
    }
    
    // Inline pin for Smoothing Mod (Channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))
            ImGui::SameLine();
    }
    
    if (isSmoothingModulated) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Smoothing (ms)", &smoothingTime, 0.1f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
        if (!isSmoothingModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("smoothing_time")))
                *p = smoothingTime;
    adjustParamOnWheel(ap.getParameter("smoothing_time"), "smoothing_time", smoothingTime);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isSmoothingModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarker("Time window for the slewed repair.\nHigher values smooth longer clicks but can dull transients.");

    // Amount slider with inline pin
    bool isAmountModulated = isParamModulated("amount_mod");
    float amount = amountParam != nullptr ? amountParam->load() : 1.0f;
    if (isAmountModulated) {
        amount = getLiveParamValueFor("amount_mod", "amount_live", amount);
    }
    
    // Inline pin for Amount Mod (Channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))
            ImGui::SameLine();
    }
    
    if (isAmountModulated) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Amount", &amount, 0.0f, 1.0f, "%.2f"))
        if (!isAmountModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("amount")))
                *p = amount;
    adjustParamOnWheel(ap.getParameter("amount"), "amount", amount);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isAmountModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarker("Dry/Wet mix. 0 = original, 1 = fully repaired.");

    ImGui::PopItemWidth();
    ImGui::PopID();
}
#endif

