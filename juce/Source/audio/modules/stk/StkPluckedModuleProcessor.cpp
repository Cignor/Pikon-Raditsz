#include "StkPluckedModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>

// Compile-time check for STK_FOUND
#ifdef STK_FOUND
    #define STK_AVAILABLE_AT_COMPILE_TIME true
#else
    #define STK_AVAILABLE_AT_COMPILE_TIME false
#endif

StkPluckedModuleProcessor::StkPluckedModuleProcessor()
    : ModuleProcessor(BusesProperties()
                      .withInput("Inputs", juce::AudioChannelSet::discreteChannels(4), true) // ch0: Freq Mod, ch1: Gate, ch2: Damping Mod, ch3: Velocity Mod
                      .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "StkPluckedParams", createParameterLayout())
{
    frequencyParam = apvts.getRawParameterValue(paramIdFrequency);
    dampingParam = apvts.getRawParameterValue(paramIdDamping);
    pluckVelocityParam = apvts.getRawParameterValue(paramIdPluckVelocity);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout StkPluckedModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdFrequency, "Frequency",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.25f), 440.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdDamping, "Damping",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdPluckVelocity, "Pluck Velocity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.8f));
    
    return { params.begin(), params.end() };
}

void StkPluckedModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    
    juce::Logger::writeToLog("[STK Plucked] prepareToPlay: sampleRate=" + juce::String(sampleRate) + " blockSize=" + juce::String(samplesPerBlock));
    
    // Initialize STK wrapper
    StkWrapper::initializeStk(sampleRate);
    
#ifdef STK_FOUND
    try
    {
        instrument = std::make_unique<stk::Plucked>(0.5f); // lowestFrequency
        if (instrument)
        {
            instrument->setSampleRate(sampleRate);
            instrument->setFrequency(frequencyParam != nullptr ? frequencyParam->load() : 440.0f);
            juce::Logger::writeToLog("[STK Plucked] Instrument created and initialized at " + juce::String(sampleRate) + " Hz");
        }
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog("[STK Plucked] EXCEPTION creating instrument: " + juce::String(e.what()));
    }
#endif
    
    smoothedGate = 0.0f;
    wasGateHigh = false;
    pluckReTriggerCounter = 0;
    m_shouldAutoTrigger = true;
    
#if defined(PRESET_CREATOR_UI)
    // Initialize visualization buffer
    vizOutputBuffer.setSize(1, vizBufferSize, false, true, false);
    vizOutputBuffer.clear();
    vizWritePos = 0;
#endif
}

void StkPluckedModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);

#ifdef STK_FOUND
    if (!instrument)
    {
        buffer.clear();
        return;
    }
#endif

    auto outBus = getBusBuffer(buffer, false, 0);
    auto inBus = getBusBuffer(buffer, true, 0);
    
    const float* freqCV = (inBus.getNumChannels() > 0) ? inBus.getReadPointer(0) : nullptr;
    const float* gateCV = (inBus.getNumChannels() > 1) ? inBus.getReadPointer(1) : nullptr;
    const float* dampingCV = (inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;
    const float* velocityCV = (inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;

    const bool freqActive = isParamInputConnected(paramIdFreqMod);
    const bool gateActive = isParamInputConnected(paramIdGateMod);
    const bool dampingActive = isParamInputConnected(paramIdDampingMod);
    const bool velocityActive = isParamInputConnected(paramIdVelocityMod);

    const float baseFrequency = frequencyParam != nullptr ? frequencyParam->load() : 440.0f;
    const float baseDamping = dampingParam != nullptr ? dampingParam->load() : 0.5f;
    const float baseVelocity = pluckVelocityParam != nullptr ? pluckVelocityParam->load() : 0.8f;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        // Calculate frequency with CV modulation
        float freq = baseFrequency;
        if (freqActive && freqCV)
        {
            const float cvRaw = freqCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            
            const float octaveOffset = (cv01 - 0.5f) * 2.0f; // ±1 octave
            freq = baseFrequency * std::pow(2.0f, octaveOffset);
        }
        freq = juce::jlimit(20.0f, 2000.0f, freq);

        // Calculate damping with CV modulation (note: Plucked doesn't expose damping directly)
        float damping = baseDamping;
        if (dampingActive && dampingCV)
        {
            const float cvRaw = dampingCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            damping = cv01;
        }

        // Calculate velocity with CV modulation
        float velocity = baseVelocity;
        if (velocityActive && velocityCV)
        {
            const float cvRaw = velocityCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            velocity = cv01;
        }
        velocity = juce::jlimit(0.0f, 1.0f, velocity);

        // Handle gate/trigger
        float gateLevel = 1.0f;
        if (gateActive && gateCV)
        {
            const float cvRaw = gateCV[i];
            if (cvRaw >= 0.0f && cvRaw <= 1.0f)
                gateLevel = cvRaw;
            else
                gateLevel = juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
        }
        else if (!m_currentTransport.isPlaying)
        {
            gateLevel = 0.0f;
        }

        // Auto-trigger on first sample if no gate connected and transport just started
        if (m_shouldAutoTrigger && !gateActive && m_currentTransport.isPlaying && i == 0)
        {
            wasGateHigh = false;
            gateLevel = 1.0f;
            m_shouldAutoTrigger = false;
        }

        // Smooth gate
        smoothedGate += (gateLevel - smoothedGate) * 0.05f;
        const bool isGateHigh = smoothedGate > 0.3f;

        // Generate audio sample
        float sample = 0.0f;
#ifdef STK_FOUND
        if (instrument)
        {
            instrument->setFrequency(freq);
            
            // Detect gate edge for pluck trigger
            if (isGateHigh && !wasGateHigh)
            {
                // Gate edge - trigger pluck with CV-modulated velocity
                instrument->pluck(velocity);
            }
            
            wasGateHigh = isGateHigh;
            
            // Continuous re-triggering for plucked instruments while gate is high
            // (they decay quickly after initial pluck)
            if (isGateHigh)
            {
                // Re-trigger pluck periodically while gate is high (every ~20ms at 48kHz = ~960 samples)
                if (++pluckReTriggerCounter >= (int)(currentSampleRate * 0.02f))
                {
                    instrument->pluck(juce::jmax(0.3f, velocity)); // Use CV-modulated velocity
                    pluckReTriggerCounter = 0;
                }
            }
            else
            {
                pluckReTriggerCounter = 0; // Reset counter when gate is low
            }
            
            sample = instrument->tick();
            
            // Apply gain boost
            sample *= 10.0f;
        }
#endif

        // Apply gate smoothing
        sample *= smoothedGate;
        
        // Additional gain boost when gate is active
        if (smoothedGate > 0.1f)
        {
            sample *= 1.5f;
        }

        // Write output
        if (outBus.getNumChannels() > 0)
        {
            outBus.setSample(0, i, sample);
        }
        
#if defined(PRESET_CREATOR_UI)
        // Capture output audio for visualization
        if (vizOutputBuffer.getNumSamples() > 0)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizOutputBuffer.setSample(0, writeIdx, sample);
        }

        // Track current state
        if (i == buffer.getNumSamples() - 1)
        {
            vizData.currentFrequency.store(freq);
            vizData.gateLevel.store(smoothedGate);
            vizData.outputLevel.store(sample);
        }
#endif
        
        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("frequency_live", freq);
            setLiveParamValue("damping_live", damping);
            setLiveParamValue("pluckVelocity_live", velocity);
        }
    }

    updateOutputTelemetry(buffer);
    
#if defined(PRESET_CREATOR_UI)
    vizWritePos = (vizWritePos + buffer.getNumSamples()) % vizBufferSize;

    // Update visualization data (thread-safe)
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx = (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) % vizBufferSize;
        if (vizOutputBuffer.getNumSamples() > 0)
            vizData.outputWaveform[i].store(vizOutputBuffer.getSample(0, readIdx));
    }
#endif
}

void StkPluckedModuleProcessor::setTimingInfo(const TransportState& state)
{
    const bool wasPlaying = m_currentTransport.isPlaying;
    m_currentTransport = state;
    
    if (state.isPlaying && !wasPlaying)
    {
        m_shouldAutoTrigger = true;
    }
}

void StkPluckedModuleProcessor::forceStop()
{
#ifdef STK_FOUND
    if (instrument)
        instrument->noteOff(0.5f);
#endif
    smoothedGate = 0.0f;
    wasGateHigh = false;
    pluckReTriggerCounter = 0;
}

bool StkPluckedModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0;
    if (paramId == paramIdFreqMod)      { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdGateMod)      { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdDampingMod)   { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdVelocityMod)  { outChannelIndexInBus = 3; return true; }
    return false;
}

#if defined(PRESET_CREATOR_UI)
void StkPluckedModuleProcessor::drawParametersInNode(float itemWidth,
                              const std::function<bool(const juce::String& paramId)>& isParamModulated,
                              const std::function<void()>& onModificationEnded,
                              const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    auto& ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    
    auto HelpMarker = [](const char* desc)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    ImGui::PushItemWidth(itemWidth);

    // Read visualization data (thread-safe)
    float outputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
        outputWaveform[i] = vizData.outputWaveform[i].load();
    const float currentFreq = vizData.currentFrequency.load();
    const float gateLevel = vizData.gateLevel.load();
    const float outputLevel = vizData.outputLevel.load();

    // Waveform visualization in child window
    const auto& freqColors = theme.modules.frequency_graph;
    const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
    const float waveHeight = 140.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);

    if (ImGui::BeginChild("StkPluckedOscilloscope", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);

        // Background
        const ImU32 bgColor = resolveColor(freqColors.background, IM_COL32(18, 20, 24, 255));
        drawList->AddRectFilled(p0, p1, bgColor);

        // Grid lines
        const ImU32 gridColor = resolveColor(freqColors.grid, IM_COL32(50, 55, 65, 255));
        const float midY = p0.y + graphSize.y * 0.5f;
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), gridColor, 1.0f);

        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);

        // Draw output waveform
        const float scaleY = graphSize.y * 0.45f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        const ImU32 waveformColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
        float prevX = p0.x;
        float prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveform[i]);
            const float x = p0.x + i * stepX;
            const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), waveformColor, 2.5f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();

        // Frequency info overlay
        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%.1f Hz | Plucked", currentFreq);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##stkPluckedVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Frequency
    ThemeText("Frequency", theme.text.section_header);
    ImGui::Spacing();
    
    const bool freqMod = isParamModulated(paramIdFreqMod);
    if (freqMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Freq Mod (Channel 0)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(0))
            ImGui::SameLine();
    }
    
    if (freqMod) ImGui::BeginDisabled();
    float freq = frequencyParam != nullptr ? getLiveParamValueFor(paramIdFreqMod, "frequency_live", frequencyParam->load()) : 440.0f;
    if (ImGui::SliderFloat("##freq", &freq, 20.0f, 2000.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic))
    {
        if (!freqMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdFrequency)))
                *p = freq;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!freqMod) adjustParamOnWheel(ap.getParameter(paramIdFrequency), "frequencyHz", freq);
    if (freqMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (freqMod)
    {
        ThemeText("Frequency (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Frequency");
    }
    HelpMarker("Base frequency of the plucked string");

    ImGui::Spacing();
    ImGui::Spacing();

    // Damping
    ThemeText("Damping", theme.text.section_header);
    ImGui::Spacing();
    
    const bool dampingMod = isParamModulated(paramIdDampingMod);
    if (dampingMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Damping Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }
    
    if (dampingMod) ImGui::BeginDisabled();
    float damping = dampingParam != nullptr ? getLiveParamValueFor(paramIdDampingMod, "damping_live", dampingParam->load()) : 0.5f;
    if (ImGui::SliderFloat("##damping", &damping, 0.0f, 1.0f, "%.2f"))
    {
        if (!dampingMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDamping)))
                *p = damping;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!dampingMod) adjustParamOnWheel(ap.getParameter(paramIdDamping), "damping", damping);
    if (dampingMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (dampingMod)
    {
        ThemeText("Damping (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Damping");
    }
    HelpMarker("String damping (decay time)\nNote: Damping is controlled internally by STK\nThis parameter affects re-triggering behavior");

    ImGui::Spacing();
    ImGui::Spacing();

    // Pluck Velocity
    ThemeText("Pluck", theme.text.section_header);
    ImGui::Spacing();
    
    const bool velocityMod = isParamModulated(paramIdVelocityMod);
    if (velocityMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Velocity Mod (Channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))
            ImGui::SameLine();
    }
    
    if (velocityMod) ImGui::BeginDisabled();
    float velocity = pluckVelocityParam != nullptr ? getLiveParamValueFor(paramIdVelocityMod, "pluckVelocity_live", pluckVelocityParam->load()) : 0.8f;
    if (ImGui::SliderFloat("##velocity", &velocity, 0.0f, 1.0f, "%.2f"))
    {
        if (!velocityMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPluckVelocity)))
                *p = velocity;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!velocityMod) adjustParamOnWheel(ap.getParameter(paramIdPluckVelocity), "velocity", velocity);
    if (velocityMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (velocityMod)
    {
        ThemeText("Velocity (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Velocity");
    }
    HelpMarker("Pluck velocity/amplitude");

    ImGui::PopItemWidth();
    ImGui::PopID(); // End module instance ID
}

void StkPluckedModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Channels 0, 2, 3 are now inline (Frequency, Damping, Velocity)
    // Only draw Gate (ch1) and Output as parallel pins
    helpers.drawParallelPins("Gate", 1, nullptr, -1);
    helpers.drawAudioOutputPin("Out", 0);
}

juce::String StkPluckedModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Freq Mod";
        case 1: return "Gate";
        case 2: return "Damping";
        case 3: return "Velocity";
        default: return juce::String(channel);
    }
}

juce::String StkPluckedModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0) return "Out";
    return juce::String(channel);
}
#endif

