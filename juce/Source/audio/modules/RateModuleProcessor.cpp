#include "RateModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

RateModuleProcessor::RateModuleProcessor()
    : ModuleProcessor(BusesProperties()
                        .withInput("Rate Mod", juce::AudioChannelSet::mono(), true)
                        .withOutput("Out", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "RateParams", createParameterLayout())
{
    baseRateParam = apvts.getRawParameterValue("baseRate");
    multiplierParam = apvts.getRawParameterValue("multiplier");
    
    // Initialize output value tracking for tooltips
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // For Out
}

juce::AudioProcessorValueTreeState::ParameterLayout RateModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterFloat>("baseRate", "Base Rate", juce::NormalisableRange<float>(0.1f, 20.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("multiplier", "Multiplier", juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 1.0f));
    return { params.begin(), params.end() };
}

void RateModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);
#if defined(PRESET_CREATOR_UI)
    vizInputBuffer.setSize(1, samplesPerBlock);
    vizOutputBuffer.setSize(1, samplesPerBlock);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
#endif
}

void RateModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    
    auto in = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);
    
    const float* src = in.getNumChannels() > 0 ? in.getReadPointer(0) : nullptr;
    float* dst = out.getWritePointer(0);
    
    const float baseRate = baseRateParam->load();
    const float multiplier = multiplierParam->load();
    
    float sumOutput = 0.0f;
    
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        float modulation = 0.0f;
        if (src != nullptr)
        {
            // Convert audio input (-1 to 1) to modulation range (-0.5 to +0.5)
            modulation = src[i] * 0.5f;
        }
        
        // Calculate final rate: baseRate * multiplier * (1 + modulation)
        float finalRate = baseRate * multiplier * (1.0f + modulation);
        finalRate = juce::jlimit(0.01f, 50.0f, finalRate); // Clamp to reasonable range
        
        // Update telemetry for live UI feedback (throttled to every 64 samples)
        if ((i & 0x3F) == 0) {
            setLiveParamValue("baseRate_live", baseRate);
            setLiveParamValue("multiplier_live", multiplier);
        }
        
        // Normalize the rate to 0.0..1.0 for modulation routing
        // Map 0.01..50.0 Hz -> 0.0..1.0
        const float normalizedRate = juce::jlimit (0.0f, 1.0f, (finalRate - 0.01f) / (50.0f - 0.01f));
        dst[i] = normalizedRate;
        sumOutput += finalRate;
    }
    
    lastOutputValue.store(sumOutput / (float) buffer.getNumSamples());
    
    // Update output values for tooltips
    if (!lastOutputValues.empty() && lastOutputValues[0])
    {
        lastOutputValues[0]->store(out.getSample(0, buffer.getNumSamples() - 1));
    }

#if defined(PRESET_CREATOR_UI)
    // Capture waveforms for visualization
    const int nSamps = buffer.getNumSamples();
    if (nSamps > 0)
    {
        vizInputBuffer.makeCopyOf(in);
        vizOutputBuffer.makeCopyOf(out);
        
        auto captureWaveform = [&](const juce::AudioBuffer<float>& source, std::array<std::atomic<float>, VizData::waveformPoints>& dest)
        {
            const int samples = juce::jmin(source.getNumSamples(), nSamps);
            if (samples <= 0) return;
            const int stride = juce::jmax(1, samples / VizData::waveformPoints);
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const int idx = juce::jmin(samples - 1, i * stride);
                float value = source.getSample(0, idx);
                dest[i].store(juce::jlimit(-1.0f, 1.0f, value));
            }
        };
        
        captureWaveform(vizInputBuffer, vizData.inputWaveform);
        captureWaveform(vizOutputBuffer, vizData.outputWaveform);
        
        // Capture final rate waveform (in Hz, normalized to 0..1 for display)
        const float rateMin = 0.01f;
        const float rateMax = 50.0f;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const int idx = juce::jmin(nSamps - 1, (i * nSamps) / VizData::waveformPoints);
            float modulation = 0.0f;
            if (src != nullptr)
            {
                modulation = src[idx] * 0.5f;
            }
            float finalRate = baseRate * multiplier * (1.0f + modulation);
            finalRate = juce::jlimit(rateMin, rateMax, finalRate);
            // Normalize to 0..1 for visualization
            float normalizedRate = (finalRate - rateMin) / (rateMax - rateMin);
            vizData.finalRateWaveform[i].store(juce::jlimit(0.0f, 1.0f, normalizedRate));
        }
        
        // Update live parameter values
        vizData.currentBaseRate.store(baseRate);
        vizData.currentMultiplier.store(multiplier);
        // Calculate average final rate for display
        float avgFinalRate = sumOutput / (float)nSamps;
        vizData.currentFinalRate.store(avgFinalRate);
    }
#endif
}

#if defined(PRESET_CREATOR_UI)
void RateModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);
    
    float baseRate = baseRateParam->load();
    float multiplier = multiplierParam->load();

    // Draw visualization
    ImGui::Spacing();
    ThemeText("Rate Visualizer", theme.text.section_header);
    ImGui::Spacing();

    // Read waveform data from atomics - BEFORE BeginChild
    float inputWave[VizData::waveformPoints];
    float outputWave[VizData::waveformPoints];
    float rateWave[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWave[i] = vizData.inputWaveform[i].load();
        outputWave[i] = vizData.outputWaveform[i].load();
        rateWave[i] = vizData.finalRateWaveform[i].load();
    }
    const float liveBaseRate = vizData.currentBaseRate.load();
    const float liveMultiplier = vizData.currentMultiplier.load();
    const float liveFinalRate = vizData.currentFinalRate.load();

    // Waveform visualization in child window
    const float waveHeight = 110.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("RateViz", graphSize, false, childFlags))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);
        
        // Background
        const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        
        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);
        
        const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
        const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
        const ImU32 rateColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);

        // Draw output waveforms
        const float midY = p0.y + graphSize.y * 0.5f;
        const float scaleY = graphSize.y * 0.4f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        // Draw center line (zero reference)
        const ImU32 centerLineColor = IM_COL32(150, 150, 150, 100);
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), centerLineColor, 1.0f);

        auto drawWave = [&](float* data, ImU32 color, float thickness, float offsetY = 0.0f)
        {
            float px = p0.x;
            float py = midY + offsetY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float x = p0.x + i * stepX;
                const float clampedY = juce::jlimit(p0.y, p1.y, midY + offsetY - juce::jlimit(-1.0f, 1.0f, data[i]) * scaleY);
                const float y = clampedY;
                if (i > 0)
                    drawList->AddLine(ImVec2(px, py), ImVec2(x, y), color, thickness);
                px = x;
                py = y;
            }
        };

        // Draw input modulation (if present) - show as bipolar waveform
        drawWave(inputWave, inputColor, 1.2f);
        
        // Draw output (normalized rate 0..1) - show as unipolar from center
        drawWave(outputWave, outputColor, 2.0f);
        
        // Draw final rate (normalized 0..1) - show as unipolar from bottom
        const float rateBaseY = p1.y;
        float prevX = p0.x;
        float prevY = rateBaseY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float x = p0.x + i * stepX;
            const float rateValue = juce::jlimit(0.0f, 1.0f, rateWave[i]);
            const float y = juce::jlimit(p0.y, p1.y, rateBaseY - rateValue * scaleY * 2.0f);
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), rateColor, 1.8f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();
        
        // Live parameter values overlay
        ImGui::SetCursorPos(ImVec2(4, waveHeight + 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "Base: %.2f Hz  |  Mult: %.2fx  |  Final: %.2f Hz", liveBaseRate, liveMultiplier, liveFinalRate);
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##rateVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ThemeText("RATE CONTROL", theme.text.section_header);
    ImGui::Spacing();
    
    bool isBaseRateModulated = isParamModulated("baseRate");
    if (isBaseRateModulated) {
        baseRate = getLiveParamValueFor("baseRate", "baseRate_live", baseRate);
        ImGui::BeginDisabled();
    }
    if (ImGui::SliderFloat("Base Rate", &baseRate, 0.1f, 20.0f, "%.2f Hz")) {
        if (!isBaseRateModulated) {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("baseRate"))) *p = baseRate;
        }
    }
    if (!isBaseRateModulated) adjustParamOnWheel(ap.getParameter("baseRate"), "baseRate", baseRate);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (isBaseRateModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ThemeText("(mod)", theme.text.active); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Base frequency in Hz");

    bool isMultiplierModulated = isParamModulated("multiplier");
    if (isMultiplierModulated) {
        multiplier = getLiveParamValueFor("multiplier", "multiplier_live", multiplier);
        ImGui::BeginDisabled();
    }
    if (ImGui::SliderFloat("Multiplier", &multiplier, 0.1f, 10.0f, "%.2fx")) {
        if (!isMultiplierModulated) {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("multiplier"))) *p = multiplier;
        }
    }
    if (!isMultiplierModulated) adjustParamOnWheel(ap.getParameter("multiplier"), "multiplier", multiplier);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (isMultiplierModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ThemeText("(mod)", theme.text.active); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rate multiplier");

    ImGui::Spacing();
    ThemeText("OUTPUT", theme.text.section_header);
    
    float outputHz = getLastOutputValue();
    ImGui::Text("Frequency: %.2f Hz", outputHz);
    
    ImGui::PopItemWidth();
    ImGui::PopID();
}
#endif

void RateModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawParallelPins("Mod In", 0, "Out", 0);
}

float RateModuleProcessor::getLastOutputValue() const
{
    return lastOutputValue.load();
}

// Parameter bus contract implementation
bool RateModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    if (paramId == "baseRate") { outBusIndex = 1; outChannelIndexInBus = 0; return true; }
    return false;
}