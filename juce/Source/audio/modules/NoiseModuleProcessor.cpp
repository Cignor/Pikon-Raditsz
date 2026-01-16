#include "NoiseModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

// --- Parameter Layout Definition ---
juce::AudioProcessorValueTreeState::ParameterLayout NoiseModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdColour, "Colour", juce::StringArray{ "White", "Pink", "Brown" }, 0));
        
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdLevel, "Level dB", juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f), -12.0f));

    auto rateRange = juce::NormalisableRange<float>(minRateHz, maxRateHz, 0.001f);
    rateRange.setSkewForCentre(4.0f);
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdRate, "Rate Hz", rateRange, 20.0f));

    return { params.begin(), params.end() };
}

// --- Constructor ---
NoiseModuleProcessor::NoiseModuleProcessor()
    : ModuleProcessor(BusesProperties()
        .withInput("Modulation", juce::AudioChannelSet::discreteChannels(3), true) // ch0: Level, ch1: Colour, ch2: Rate
        .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "NoiseParams", createParameterLayout())
{
    levelDbParam = apvts.getRawParameterValue(paramIdLevel);
    colourParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(paramIdColour));
    rateHzParam = apvts.getRawParameterValue(paramIdRate);
    slowNoiseState = 0.0f;

    // Initialize output value tracking for tooltips
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

// --- Audio Processing Setup ---
void NoiseModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = juce::jmax(1.0, sampleRate);
    slowNoiseState = 0.0f;

    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };

    // Pink noise is ~-3dB/octave. A simple 1-pole low-pass can approximate this.
    pinkFilter.coefficients = juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass(sampleRate, 1000.0);
    
    // Brown noise is ~-6dB/octave. A stronger low-pass.
    brownFilter.coefficients = juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass(sampleRate, 250.0);
    
    pinkFilter.prepare(spec);
    brownFilter.prepare(spec);
    pinkFilter.reset();
    brownFilter.reset();

#if defined(PRESET_CREATOR_UI)
    vizOutputBuffer.setSize(1, samplesPerBlock);
    vizOutputBuffer.clear();
    for (auto& v : vizData.outputWaveform) v.store(0.0f);
    vizData.outputRms.store(0.0f);
#endif
}

// --- Main Audio Processing Block ---
void NoiseModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto modInBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const int numSamples = buffer.getNumSamples();

    // --- Get Modulation CVs ---
    // CORRECTED: Use the _mod parameter IDs to check for connections
    const bool isLevelModulated = isParamInputConnected(paramIdLevelMod);
    const bool isColourModulated = isParamInputConnected(paramIdColourMod);
    const bool isRateModulated = isParamInputConnected(paramIdRateMod);

    const float* levelCV = isLevelModulated && modInBus.getNumChannels() > 0 ? modInBus.getReadPointer(0) : nullptr;
    const float* colourCV = isColourModulated && modInBus.getNumChannels() > 1 ? modInBus.getReadPointer(1) : nullptr;
    const float* rateCV = isRateModulated && modInBus.getNumChannels() > 2 ? modInBus.getReadPointer(2) : nullptr;

    // --- Get Base Parameter Values ---
    const float baseLevelDb = levelDbParam->load();
    const int baseColour = colourParam->getIndex();
    const float baseRateHz = rateHzParam ? rateHzParam->load() : 20.0f;

    // --- Per-Sample Processing for Responsive Modulation ---
    // CRITICAL FIX: Only generate noise when transport is playing
    const bool shouldGenerateNoise = m_currentTransport.isPlaying;
    
    for (int i = 0; i < numSamples; ++i)
    {
        // Declare variables outside if block for telemetry use
        float effectiveLevelDb = baseLevelDb;
        int effectiveColour = baseColour;
        float effectiveRateHz = baseRateHz;
        float sample = 0.0f; // Default to silence
        
        if (shouldGenerateNoise)
        {
            // 1. Calculate effective parameter values for this sample
            if (isLevelModulated && levelCV != nullptr) {
                // CV maps 0..1 to the full dB range
                effectiveLevelDb = juce::jmap(levelCV[i], 0.0f, 1.0f, -60.0f, 6.0f);
            }

            if (isColourModulated && colourCV != nullptr) {
                // CV maps 0..1 to the 3 choices
                effectiveColour = static_cast<int>(juce::jlimit(0.0f, 1.0f, colourCV[i]) * 2.99f);
            }

            if (isRateModulated && rateCV != nullptr) {
                effectiveRateHz = juce::jmap(rateCV[i], 0.0f, 1.0f, minRateHz, maxRateHz);
            }
            effectiveRateHz = juce::jlimit(minRateHz, maxRateHz, effectiveRateHz);

            // 2. Generate raw white noise
            sample = random.nextFloat() * 2.0f - 1.0f;

            // 3. Filter noise based on effective colour
            switch (effectiveColour)
            {
                case 0: /* White noise, no filter */ break;
                case 1: sample = pinkFilter.processSample(sample); break;
                case 2: sample = brownFilter.processSample(sample); break;
            }

            // 4. Apply gain
            sample *= juce::Decibels::decibelsToGain(effectiveLevelDb);

            // 5. Rate smoothing: higher rate -> faster tracking, lower rate -> slower movement
            const float smoothingAmount = juce::jlimit(0.0001f, 1.0f, effectiveRateHz / maxRateHz);
            slowNoiseState += smoothingAmount * (sample - slowNoiseState);
            sample = slowNoiseState;
        }
        else
        {
            // Transport is stopped - fade to silence smoothly
            const float fadeRate = 0.01f; // Fast fade
            slowNoiseState += fadeRate * (0.0f - slowNoiseState);
            sample = slowNoiseState;
        }

        // 6. Write to mono output
        outBus.setSample(0, i, sample);

        // 7. Update telemetry for UI (throttled)
        if ((i & 0x3F) == 0) // Every 64 samples
        {
            setLiveParamValue("level_live", effectiveLevelDb);
            setLiveParamValue("colour_live", (float)effectiveColour);
            setLiveParamValue("rate_live", effectiveRateHz);
        }
    }

    // --- Update Inspector Values (peak magnitude) ---
    updateOutputTelemetry(buffer);

#if defined(PRESET_CREATOR_UI)
    // Capture waveform for visualization
    if (numSamples > 0)
    {
        vizOutputBuffer.makeCopyOf(outBus);
        
        // Track last computed values for visualization
        float lastLevelDb = baseLevelDb;
        int lastColour = baseColour;
        float lastRate = baseRateHz;
        
        // Calculate RMS
        float rmsSum = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = outBus.getSample(0, i);
            rmsSum += sample * sample;
            
            // Track last effective values
            if (isLevelModulated && levelCV != nullptr) {
                lastLevelDb = juce::jmap(levelCV[i], 0.0f, 1.0f, -60.0f, 6.0f);
            }
            if (isColourModulated && colourCV != nullptr) {
                lastColour = static_cast<int>(juce::jlimit(0.0f, 1.0f, colourCV[i]) * 2.99f);
            }
            if (isRateModulated && rateCV != nullptr) {
                lastRate = juce::jmap(rateCV[i], 0.0f, 1.0f, minRateHz, maxRateHz);
            }
        }
        float rms = std::sqrt(rmsSum / (float)numSamples);
        vizData.outputRms.store(rms);
        
        // Down-sample waveform
        const int stride = juce::jmax(1, numSamples / VizData::waveformPoints);
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const int idx = juce::jmin(numSamples - 1, i * stride);
            float value = outBus.getSample(0, idx);
            vizData.outputWaveform[i].store(juce::jlimit(-1.0f, 1.0f, value));
        }
        
        // Update live parameter values
        vizData.currentLevelDb.store(lastLevelDb);
        vizData.currentColour.store(lastColour);
        vizData.currentRateHz.store(lastRate);
    }
#endif
}

bool NoiseModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation inputs are on the first bus
    
    // CORRECTED: Map the virtual _mod IDs to physical channels
    if (paramId == paramIdLevelMod)  { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdColourMod) { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdRateMod)   { outChannelIndexInBus = 2; return true; }
    
    return false;
}

void NoiseModuleProcessor::setTimingInfo(const TransportState& state)
{
    m_currentTransport = state;
}

void NoiseModuleProcessor::forceStop()
{
    // Force noise state to silence
    slowNoiseState = 0.0f;
}

#if defined(PRESET_CREATOR_UI)
// --- UI Drawing Logic ---

void NoiseModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushItemWidth(itemWidth);

    bool levelIsModulated = isParamModulated(paramIdLevelMod);
    float levelDb = levelIsModulated ? getLiveParamValueFor(paramIdLevelMod, "level_live", levelDbParam->load()) : levelDbParam->load();

    bool colourIsModulated = isParamModulated(paramIdColourMod);
    int colourIndex = colourIsModulated ? (int)getLiveParamValueFor(paramIdColourMod, "colour_live", (float)colourParam->getIndex()) : colourParam->getIndex();

    bool rateIsModulated = isParamModulated(paramIdRateMod);
    float rateHz = rateIsModulated ? getLiveParamValueFor(paramIdRateMod, "rate_live", rateHzParam ? rateHzParam->load() : 20.0f)
                                   : (rateHzParam ? rateHzParam->load() : 20.0f);

    // === SECTION: Noise Type ===
    ThemeText("NOISE TYPE", theme.text.section_header);

    // Inline pin for Colour Mod (Channel 1)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1))
            ImGui::SameLine();
    }

    if (colourIsModulated) ImGui::BeginDisabled();
    if (ImGui::Combo("Colour", &colourIndex, "White\0Pink\0Brown\0\0"))
    {
        if (!colourIsModulated) *colourParam = colourIndex;
    }
    if (!colourIsModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newIndex = juce::jlimit(0, 2, colourIndex + (wheel > 0.0f ? -1 : 1));
            if (newIndex != colourIndex)
            {
                colourIndex = newIndex;
                *colourParam = colourIndex;
                onModificationEnded();
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !colourIsModulated) { onModificationEnded(); }
    if (colourIsModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ThemeText("(mod)", theme.text.active); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("White=flat spectrum, Pink=-3dB/oct, Brown=-6dB/oct");

    ImGui::Spacing();
    ImGui::Spacing();

    // === SECTION: Rate ===
    ThemeText("RATE", theme.text.section_header);

    // Inline pin for Rate Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }

    if (rateIsModulated) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Rate", &rateHz, minRateHz, maxRateHz, "%.2f Hz", ImGuiSliderFlags_Logarithmic))
    {
        if (!rateIsModulated && rateHzParam != nullptr) *rateHzParam = rateHz;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !rateIsModulated) { onModificationEnded(); }
    if (!rateIsModulated && rateHzParam != nullptr) adjustParamOnWheel(ap.getParameter(paramIdRate), "rate", rateHz);
    if (rateIsModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ThemeText("(mod)", theme.text.active); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Controls how often the noise updates. Lower values slow it down.");

    ImGui::Spacing();
    ImGui::Spacing();

    // === SECTION: Output Level ===
    ThemeText("OUTPUT LEVEL", theme.text.section_header);

    // Inline pin for Level Mod (Channel 0)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(0))
            ImGui::SameLine();
    }

    if (levelIsModulated) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Level", &levelDb, -60.0f, 6.0f, "%.1f dB"))
    {
        if (!levelIsModulated) *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdLevel)) = levelDb;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !levelIsModulated) { onModificationEnded(); }
    if (!levelIsModulated) adjustParamOnWheel(ap.getParameter(paramIdLevel), "level", levelDb);
    if (levelIsModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ThemeText("(mod)", theme.text.active); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output amplitude in decibels");

    ImGui::Spacing();
    
    // === SECTION: Noise Visualizer ===
    ThemeText("Noise Visualizer", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushID(this); // Unique ID for this node's UI
    
    // Read visualization data (thread-safe)
    float outputWave[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        outputWave[i] = vizData.outputWaveform[i].load();
    }
    const float liveLevelDb = vizData.currentLevelDb.load();
    const float liveRms = vizData.outputRms.load();
    const int currentColour = vizData.currentColour.load();
    const float liveRateHz = vizData.currentRateHz.load();

    // Waveform visualization in child window
    const float waveHeight = 110.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);
    
    if (ImGui::BeginChild("NoiseViz", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);
        
        // Background
        const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        
        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);
        
        // Color based on noise type (using theme colors)
        ImU32 noiseColor;
        switch (currentColour)
        {
            case 1: // Pink
                noiseColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
                break;
            case 2: // Brown
                noiseColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.filter);
                break;
            default: // White
                noiseColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
                break;
        }

        // Draw output waveform
        const float scaleY = graphSize.y * 0.4f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);
        const float midY = p0.y + graphSize.y * 0.5f;
        
        // Draw center line (zero reference)
        const ImU32 centerLineColor = IM_COL32(150, 150, 150, 100);
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), centerLineColor, 1.0f);

        // Draw noise waveform
        float prevX = p0.x;
        float prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWave[i]);
            const float x = p0.x + i * stepX;
            const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), noiseColor, 1.5f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();
        
        // Live parameter values overlay
        const char* colourNames[] = { "White", "Pink", "Brown" };
        const char* currentColourName = (currentColour >= 0 && currentColour < 3) ? colourNames[currentColour] : "Unknown";
        
        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%s Noise  |  Rate: %.2f Hz  |  Level: %.1f dB  |  RMS: %.3f",
                           currentColourName, liveRateHz, liveLevelDb, liveRms);
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##noiseVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::PopID(); // End unique ID

    ImGui::Spacing();
    ImGui::Spacing();

    // === SECTION: Live Output ===
    ThemeText("LIVE OUTPUT", theme.text.section_header);

    float currentOut = 0.0f;
    if (lastOutputValues.size() >= 1 && lastOutputValues[0]) {
        currentOut = lastOutputValues[0]->load();
    }
    const float vizRateHz = vizData.currentRateHz.load();

    // Calculate fixed width for progress bar using actual text measurements
    const float labelTextWidth = ImGui::CalcTextSize("Level:").x;
    const float valueTextWidth = ImGui::CalcTextSize("-0.999").x;  // Max expected width
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float barWidth = itemWidth - labelTextWidth - valueTextWidth - (spacing * 2.0f);

    ImGui::Text("Level:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme.accent);
    ImGui::ProgressBar((currentOut + 1.0f) / 2.0f, ImVec2(barWidth, 0), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%.3f", currentOut);

    ImGui::Text("Rate:");
    ImGui::SameLine();
    ImGui::Text("%.2f Hz", vizRateHz);

    ImGui::PopItemWidth();
    ImGui::PopID();  // End unique ID
}

void NoiseModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // All modulation inputs (channels 0-2) are now inline, only draw output
    helpers.drawAudioOutputPin("Out", 0);
}

// --- Pin Label and Routing Definitions ---

juce::String NoiseModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Level Mod";
        case 1: return "Colour Mod";
        case 2: return "Rate Mod";
        default: return {};
    }
}

juce::String NoiseModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Out";
        default: return {};
    }
}
#endif