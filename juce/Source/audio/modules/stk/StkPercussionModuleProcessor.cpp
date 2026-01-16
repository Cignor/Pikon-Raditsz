#include "StkPercussionModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>

// Compile-time check for STK_FOUND
#ifdef STK_FOUND
    #define STK_AVAILABLE_AT_COMPILE_TIME true
#else
    #define STK_AVAILABLE_AT_COMPILE_TIME false
#endif

StkPercussionModuleProcessor::StkPercussionModuleProcessor()
    : ModuleProcessor(BusesProperties()
                      .withInput("Inputs", juce::AudioChannelSet::discreteChannels(7), true) // ch0: Freq Mod, ch1: Gate/Strike, ch2: Velocity, ch3: Stick Hardness Mod, ch4: Strike Position Mod, ch5: Decay Mod, ch6: Resonance Mod
                      .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "StkPercussionParams", createParameterLayout())
{
    frequencyParam = apvts.getRawParameterValue(paramIdFrequency);
    instrumentTypeParam = apvts.getRawParameterValue(paramIdInstrumentType);
    strikeVelocityParam = apvts.getRawParameterValue(paramIdStrikeVelocity);
    strikePositionParam = apvts.getRawParameterValue(paramIdStrikePosition);
    stickHardnessParam = apvts.getRawParameterValue(paramIdStickHardness);
    presetParam = apvts.getRawParameterValue(paramIdPreset);
    decayParam = apvts.getRawParameterValue(paramIdDecay);
    resonanceParam = apvts.getRawParameterValue(paramIdResonance);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout StkPercussionModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdFrequency, "Frequency",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.25f), 440.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdInstrumentType, "Instrument Type",
        juce::StringArray { "ModalBar", "BandedWG", "Shakers" }, 0));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdStrikeVelocity, "Strike Velocity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.8f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdStrikePosition, "Strike Position",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdStickHardness, "Stick Hardness",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        paramIdPreset, "Preset",
        0, 22, 0)); // Range depends on instrument type (0-8 for ModalBar, 0-3 for BandedWG, 0-22 for Shakers)
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdDecay, "Decay",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdResonance, "Resonance",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    return { params.begin(), params.end() };
}

void StkPercussionModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    
    juce::Logger::writeToLog("[STK Percussion] prepareToPlay: sampleRate=" + juce::String(sampleRate) + " blockSize=" + juce::String(samplesPerBlock));
    
    // Initialize STK wrapper
    StkWrapper::initializeStk(sampleRate);
    
    // Create initial instrument
    updateInstrument();
    
#ifdef STK_FOUND
    if (instrument)
    {
        instrument->setSampleRate(sampleRate);
        juce::Logger::writeToLog("[STK Percussion] Instrument created and initialized at " + juce::String(sampleRate) + " Hz");
    }
#endif
    
    smoothedGate = 0.0f;
    wasGateHigh = false;
    m_shouldAutoTrigger = true;
    
#if defined(PRESET_CREATOR_UI)
    // Initialize visualization buffer
    vizOutputBuffer.setSize(1, vizBufferSize, false, true, false);
    vizOutputBuffer.clear();
    vizWritePos = 0;
#endif
}

void StkPercussionModuleProcessor::updateInstrument()
{
#ifdef STK_FOUND
    const int instrumentType = (int)(instrumentTypeParam != nullptr ? instrumentTypeParam->load() : 0.0f);
    
    if (instrumentType == currentInstrumentType && instrument != nullptr)
        return; // No change needed
    
    currentInstrumentType = instrumentType;
    
    try
    {
        switch (instrumentType)
        {
            case 0: // ModalBar
                instrument = std::make_unique<stk::ModalBar>();
                break;
            case 1: // BandedWG
                instrument = std::make_unique<stk::BandedWG>();
                break;
            case 2: // Shakers
                instrument = std::make_unique<stk::Shakers>(0); // Default to Maraca
                break;
            default:
                instrument = std::make_unique<stk::ModalBar>();
                break;
        }
        
        if (instrument)
        {
            instrument->setSampleRate(currentSampleRate);
            instrument->setFrequency(frequencyParam != nullptr ? frequencyParam->load() : 440.0f);
        }
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog("[STK Percussion] EXCEPTION creating instrument: " + juce::String(e.what()));
        try
        {
            instrument = std::make_unique<stk::ModalBar>();
            if (instrument)
            {
                instrument->setSampleRate(currentSampleRate);
            }
        }
        catch (const std::exception& e2)
        {
            juce::Logger::writeToLog("[STK Percussion] EXCEPTION in fallback: " + juce::String(e2.what()));
        }
    }
#else
    juce::ignoreUnused(currentInstrumentType);
#endif
}

void StkPercussionModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
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
    const float* velocityCV = (inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;
    const float* stickHardnessCV = (inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* strikePositionCV = (inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;
    const float* decayCV = (inBus.getNumChannels() > 5) ? inBus.getReadPointer(5) : nullptr;
    const float* resonanceCV = (inBus.getNumChannels() > 6) ? inBus.getReadPointer(6) : nullptr;

    const bool freqActive = isParamInputConnected(paramIdFreqMod);
    const bool gateActive = isParamInputConnected(paramIdGateMod);
    const bool velocityActive = isParamInputConnected(paramIdVelocityMod);
    const bool stickHardnessActive = isParamInputConnected(paramIdStickHardnessMod);
    const bool strikePositionActive = isParamInputConnected(paramIdStrikePositionMod);
    const bool decayActive = isParamInputConnected(paramIdDecayMod);
    const bool resonanceActive = isParamInputConnected(paramIdResonanceMod);

    const float baseFrequency = frequencyParam != nullptr ? frequencyParam->load() : 440.0f;
    const float baseVelocity = strikeVelocityParam != nullptr ? strikeVelocityParam->load() : 0.8f;

    // Check if instrument type changed
    const int instrumentType = (int)(instrumentTypeParam != nullptr ? instrumentTypeParam->load() : 0.0f);
    if (instrumentType != currentInstrumentType)
    {
        updateInstrument();
    }

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

        // Calculate strike velocity with CV modulation
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
        
        // Calculate Shakers parameters with CV modulation (for telemetry)
        float decay = decayParam != nullptr ? decayParam->load() : 0.5f;
        float resonance = resonanceParam != nullptr ? resonanceParam->load() : 0.5f;
        
        if (decayActive && decayCV)
        {
            const float cvRaw = decayCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            decay = cv01;
        }
        decay = juce::jlimit(0.0f, 1.0f, decay);
        
        if (resonanceActive && resonanceCV)
        {
            const float cvRaw = resonanceCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            resonance = cv01;
        }
        resonance = juce::jlimit(0.0f, 1.0f, resonance);

        // Handle gate/strike trigger
        float gateLevel = 0.0f;
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
            
            // Update instrument-specific parameters BEFORE triggering (important for BandedWG)
            if (auto* modalBar = dynamic_cast<stk::ModalBar*>(instrument.get()))
            {
                int preset = (int)(presetParam != nullptr ? presetParam->load() : 0.0f);
                preset = juce::jlimit(0, 8, preset);
                modalBar->setPreset(preset);
                
                // Calculate stick hardness with CV modulation
                float stickHardness = stickHardnessParam != nullptr ? stickHardnessParam->load() : 0.5f;
                if (stickHardnessActive && stickHardnessCV)
                {
                    const float cvRaw = stickHardnessCV[i];
                    const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                                     ? juce::jlimit(0.0f, 1.0f, cvRaw)
                                     : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
                    stickHardness = cv01;
                }
                stickHardness = juce::jlimit(0.0f, 1.0f, stickHardness);
                modalBar->setStickHardness(stickHardness);
                
                // Calculate strike position with CV modulation
                float strikePos = strikePositionParam != nullptr ? strikePositionParam->load() : 0.5f;
                if (strikePositionActive && strikePositionCV)
                {
                    const float cvRaw = strikePositionCV[i];
                    const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                                     ? juce::jlimit(0.0f, 1.0f, cvRaw)
                                     : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
                    strikePos = cv01;
                }
                strikePos = juce::jlimit(0.0f, 1.0f, strikePos);
                modalBar->setStrikePosition(strikePos);
            }
            else if (auto* bandedWG = dynamic_cast<stk::BandedWG*>(instrument.get()))
            {
                // Set preset and strike position BEFORE triggering
                int preset = (int)(presetParam != nullptr ? presetParam->load() : 0.0f);
                preset = juce::jlimit(0, 3, preset);
                bandedWG->setPreset(preset);
                
                // Calculate strike position with CV modulation
                float strikePos = strikePositionParam != nullptr ? strikePositionParam->load() : 0.5f;
                if (strikePositionActive && strikePositionCV)
                {
                    const float cvRaw = strikePositionCV[i];
                    const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                                     ? juce::jlimit(0.0f, 1.0f, cvRaw)
                                     : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
                    strikePos = cv01;
                }
                strikePos = juce::jlimit(0.0f, 1.0f, strikePos);
                bandedWG->setStrikePosition(strikePos);
            }
            
            // Detect gate edge for strike trigger (AFTER parameters are set)
            if (isGateHigh && !wasGateHigh)
            {
                // Gate edge - trigger strike
                if (auto* modalBar = dynamic_cast<stk::ModalBar*>(instrument.get()))
                {
                    modalBar->noteOn(freq, velocity);
                }
                else if (auto* bandedWG = dynamic_cast<stk::BandedWG*>(instrument.get()))
                {
                    // BandedWG works better with pluck() for percussion strikes
                    bandedWG->pluck(velocity);
                }
                else if (auto* shakers = dynamic_cast<stk::Shakers*>(instrument.get()))
                {
                    // Shakers uses noteOn(instrument, amplitude) where instrument is type selector
                    int shakerType = (int)(presetParam != nullptr ? presetParam->load() : 0.0f);
                    shakerType = juce::jlimit(0, 22, shakerType);
                    shakers->noteOn(shakerType, velocity);
                }
            }
            
            wasGateHigh = isGateHigh;
            
            // Update instrument-specific parameters (Shakers) - use pre-calculated CV-modulated values
            if (auto* shakers = dynamic_cast<stk::Shakers*>(instrument.get()))
            {
                shakers->controlChange(4, decay * 128.0f); // System Decay
                shakers->controlChange(1, resonance * 128.0f); // Resonance Frequency
            }
            
            sample = instrument->tick();
            
            // Apply gain boost - Shakers need significantly more gain
            if (auto* shakers = dynamic_cast<stk::Shakers*>(instrument.get()))
            {
                sample *= 25.0f; // Shakers need way more gain
            }
            else
            {
                sample *= 8.0f; // Other percussion instruments
            }
        }
#endif

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
            vizData.currentInstrumentType.store(currentInstrumentType);
            vizData.gateLevel.store(smoothedGate);
            vizData.outputLevel.store(sample);
            vizData.strikeVelocity.store(velocity);
        }
#endif
        
        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("frequency_live", freq);
            setLiveParamValue("strikeVelocity_live", velocity);
            
            // Shakers-specific parameters (only set if instrument type is Shakers)
            const int instrumentType = (int)(instrumentTypeParam != nullptr ? instrumentTypeParam->load() : 0.0f);
            if (instrumentType == 2) // Shakers
            {
                setLiveParamValue("decay_live", decay);
                setLiveParamValue("resonance_live", resonance);
            }
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

void StkPercussionModuleProcessor::setTimingInfo(const TransportState& state)
{
    const bool wasPlaying = m_currentTransport.isPlaying;
    m_currentTransport = state;
    
    if (state.isPlaying && !wasPlaying)
    {
        m_shouldAutoTrigger = true;
    }
}

void StkPercussionModuleProcessor::forceStop()
{
#ifdef STK_FOUND
    if (instrument)
        instrument->noteOff(0.5f);
#endif
    smoothedGate = 0.0f;
    wasGateHigh = false;
}

bool StkPercussionModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0;
    if (paramId == paramIdFreqMod)              { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdGateMod)              { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdVelocityMod)          { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdStickHardnessMod)     { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdStrikePositionMod)    { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdDecayMod)             { outChannelIndexInBus = 5; return true; }
    if (paramId == paramIdResonanceMod)         { outChannelIndexInBus = 6; return true; }
    return false;
}

#if defined(PRESET_CREATOR_UI)
void StkPercussionModuleProcessor::drawParametersInNode(float itemWidth,
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
    ImGui::PushID(this);

    // Read visualization data (thread-safe)
    float outputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
        outputWaveform[i] = vizData.outputWaveform[i].load();
    const float currentFreq = vizData.currentFrequency.load();
    const int currentInstType = vizData.currentInstrumentType.load();
    const float gateLevel = vizData.gateLevel.load();
    const float outputLevel = vizData.outputLevel.load();
    const float strikeVelocity = vizData.strikeVelocity.load();

    // Waveform visualization in child window
    const auto& freqColors = theme.modules.frequency_graph;
    const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
    const float waveHeight = 140.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);

    if (ImGui::BeginChild("StkPercussionOscilloscope", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
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

        // Draw strike velocity indicator
        if (strikeVelocity > 0.0f)
        {
            const ImU32 velocityColor = IM_COL32(255, 200, 100, 255);
            const float velocityY = p0.y + graphSize.y - (strikeVelocity * graphSize.y * 0.3f);
            const float clampedVelocityY = juce::jlimit(p0.y + 2.0f, p1.y - 2.0f, velocityY);
            drawList->AddLine(ImVec2(p0.x, clampedVelocityY), ImVec2(p1.x, clampedVelocityY), velocityColor, 1.5f);
        }

        drawList->PopClipRect();

        // Frequency and instrument info overlay
        const char* instrumentNames[] = { "ModalBar", "BandedWG", "Shakers" };
        const char* instrumentName = (currentInstType >= 0 && currentInstType < 3) ? instrumentNames[currentInstType] : "Unknown";

        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%.1f Hz | %s", currentFreq, instrumentName);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##stkPercussionVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Instrument Type
    ThemeText("Instrument", theme.text.section_header);
    ImGui::Spacing();
    
    int instrumentType = 0;
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdInstrumentType)))
        instrumentType = p->getIndex();
    
    const char* instrumentNames[] = { "ModalBar", "BandedWG", "Shakers" };
    if (ImGui::Combo("##instrument", &instrumentType, instrumentNames, 3))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdInstrumentType)))
            *p = instrumentType;
        onModificationEnded();
    }
    
    // Scroll wheel support
    if (ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int maxIndex = 2;
            const int newIndex = juce::jlimit(0, maxIndex, instrumentType + (wheel > 0.0f ? -1 : 1));
            if (newIndex != instrumentType)
            {
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdInstrumentType)))
                {
                    *p = newIndex;
                    onModificationEnded();
                }
            }
        }
    }
    
    ImGui::SameLine();
    ImGui::Text("Type");
    HelpMarker("Select percussion instrument type");

    ImGui::Spacing();
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
    HelpMarker("Base frequency of the instrument");

    ImGui::Spacing();

    // Strike Velocity
    ThemeText("Strike", theme.text.section_header);
    ImGui::Spacing();
    
    const bool velocityMod = isParamModulated(paramIdVelocityMod);
    if (velocityMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Velocity Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }
    
    if (velocityMod) ImGui::BeginDisabled();
    float velocity = strikeVelocityParam != nullptr ? getLiveParamValueFor(paramIdVelocityMod, "strikeVelocity_live", strikeVelocityParam->load()) : 0.8f;
    if (ImGui::SliderFloat("##velocity", &velocity, 0.0f, 1.0f, "%.2f"))
    {
        if (!velocityMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdStrikeVelocity)))
                *p = velocity;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!velocityMod) adjustParamOnWheel(ap.getParameter(paramIdStrikeVelocity), "velocity", velocity);
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
    HelpMarker("Strike velocity/amplitude");

    ImGui::Spacing();

    // Instrument-specific parameters
    if (currentInstType == 0) // ModalBar
    {
        // Preset
        int preset = (int)(presetParam != nullptr ? presetParam->load() : 0.0f);
        preset = juce::jlimit(0, 8, preset);
        const char* presetNames[] = { "Marimba", "Vibraphone", "Agogo", "Wood1", "Reso", "Wood2", "Beats", "Two Fixed", "Clump" };
        if (ImGui::Combo("##preset", &preset, presetNames, 9))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(paramIdPreset)))
                *p = preset;
            onModificationEnded();
        }
        
        // Scroll wheel support
        if (ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int maxIndex = 8;
                const int newIndex = juce::jlimit(0, maxIndex, preset + (wheel > 0.0f ? -1 : 1));
                if (newIndex != preset)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(paramIdPreset)))
                    {
                        *p = newIndex;
                        onModificationEnded();
                    }
                }
            }
        }
        
        ImGui::SameLine();
        ImGui::Text("Preset");
        HelpMarker("ModalBar preset type");

        ImGui::Spacing();

        // Stick Hardness
        const bool stickHardnessMod = isParamModulated(paramIdStickHardnessMod);
        if (stickHardnessMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Stick Hardness Mod (Channel 3)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(3))
                ImGui::SameLine();
        }
        
        if (stickHardnessMod) ImGui::BeginDisabled();
        float stickHardness = stickHardnessParam != nullptr ? getLiveParamValueFor(paramIdStickHardnessMod, "stickHardness_live", stickHardnessParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##stick", &stickHardness, 0.0f, 1.0f, "%.2f"))
        {
            if (!stickHardnessMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdStickHardness)))
                    *p = stickHardness;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!stickHardnessMod) adjustParamOnWheel(ap.getParameter(paramIdStickHardness), "stickHardness", stickHardness);
        if (stickHardnessMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (stickHardnessMod)
        {
            ThemeText("Stick Hardness (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Stick Hardness");
        }
        HelpMarker("Stick hardness (ModalBar)");

        ImGui::Spacing();

        // Strike Position
        const bool strikePosMod = isParamModulated(paramIdStrikePositionMod);
        if (strikePosMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Strike Position Mod (Channel 4)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(4))
                ImGui::SameLine();
        }
        
        if (strikePosMod) ImGui::BeginDisabled();
        float strikePos = strikePositionParam != nullptr ? getLiveParamValueFor(paramIdStrikePositionMod, "strikePosition_live", strikePositionParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##strikePos", &strikePos, 0.0f, 1.0f, "%.2f"))
        {
            if (!strikePosMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdStrikePosition)))
                    *p = strikePos;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!strikePosMod) adjustParamOnWheel(ap.getParameter(paramIdStrikePosition), "strikePos", strikePos);
        if (strikePosMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (strikePosMod)
        {
            ThemeText("Strike Position (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Strike Position");
        }
        HelpMarker("Strike position (ModalBar)");
    }
    else if (currentInstType == 1) // BandedWG
    {
        // Preset
        int preset = (int)(presetParam != nullptr ? presetParam->load() : 0.0f);
        preset = juce::jlimit(0, 3, preset);
        const char* presetNames[] = { "Uniform Bar", "Tuned Bar", "Glass Harmonica", "Tibetan Bowl" };
        if (ImGui::Combo("##preset", &preset, presetNames, 4))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(paramIdPreset)))
                *p = preset;
            onModificationEnded();
        }
        
        // Scroll wheel support
        if (ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int maxIndex = 3;
                const int newIndex = juce::jlimit(0, maxIndex, preset + (wheel > 0.0f ? -1 : 1));
                if (newIndex != preset)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(paramIdPreset)))
                    {
                        *p = newIndex;
                        onModificationEnded();
                    }
                }
            }
        }
        
        ImGui::SameLine();
        ImGui::Text("Preset");
        HelpMarker("BandedWG preset type");

        ImGui::Spacing();

        // Strike Position
        const bool strikePosMod = isParamModulated(paramIdStrikePositionMod);
        if (strikePosMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Strike Position Mod (Channel 4)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(4))
                ImGui::SameLine();
        }
        
        if (strikePosMod) ImGui::BeginDisabled();
        float strikePos = strikePositionParam != nullptr ? getLiveParamValueFor(paramIdStrikePositionMod, "strikePosition_live", strikePositionParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##strikePos", &strikePos, 0.0f, 1.0f, "%.2f"))
        {
            if (!strikePosMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdStrikePosition)))
                    *p = strikePos;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!strikePosMod) adjustParamOnWheel(ap.getParameter(paramIdStrikePosition), "strikePos", strikePos);
        if (strikePosMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (strikePosMod)
        {
            ThemeText("Strike Position (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Strike Position");
        }
        HelpMarker("Strike position (BandedWG)");
    }
    else if (currentInstType == 2) // Shakers
    {
        // Preset (instrument type for Shakers)
        int preset = (int)(presetParam != nullptr ? presetParam->load() : 0.0f);
        preset = juce::jlimit(0, 22, preset);
        const char* presetNames[] = { "Maraca", "Cabasa", "Sekere", "Tambourine", "Sleigh Bells", "Bamboo Chimes", 
                                     "Sand Paper", "Coke Can", "Sticks", "Crunch", "Big Rocks", "Little Rocks",
                                     "Next Mug", "Penny+Mug", "Nickle+Mug", "Dime+Mug", "Quarter+Mug", "Franc+Mug",
                                     "Peso+Mug", "Guiro", "Wrench", "Water Drops", "Tuned Bamboo" };
        if (ImGui::Combo("##preset", &preset, presetNames, 23))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(paramIdPreset)))
                *p = preset;
            onModificationEnded();
        }
        
        // Scroll wheel support
        if (ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int maxIndex = 22;
                const int newIndex = juce::jlimit(0, maxIndex, preset + (wheel > 0.0f ? -1 : 1));
                if (newIndex != preset)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(paramIdPreset)))
                    {
                        *p = newIndex;
                        onModificationEnded();
                    }
                }
            }
        }
        
        ImGui::SameLine();
        ImGui::Text("Type");
        HelpMarker("Shaker instrument type");

        ImGui::Spacing();

        const bool decayMod = isParamModulated(paramIdDecayMod);
        if (decayMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Decay Mod (Channel 5)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(5))
                ImGui::SameLine();
        }
        
        if (decayMod) ImGui::BeginDisabled();
        float decay = decayParam != nullptr ? getLiveParamValueFor(paramIdDecayMod, "decay_live", decayParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##decay", &decay, 0.0f, 1.0f, "%.2f"))
        {
            if (!decayMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDecay)))
                    *p = decay;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!decayMod) adjustParamOnWheel(ap.getParameter(paramIdDecay), "decay", decay);
        if (decayMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (decayMod)
        {
            ThemeText("Decay (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Decay");
        }
        HelpMarker("System decay (Shakers)");

        ImGui::Spacing();

        const bool resonanceMod = isParamModulated(paramIdResonanceMod);
        if (resonanceMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Resonance Mod (Channel 6)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(6))
                ImGui::SameLine();
        }
        
        if (resonanceMod) ImGui::BeginDisabled();
        float resonance = resonanceParam != nullptr ? getLiveParamValueFor(paramIdResonanceMod, "resonance_live", resonanceParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##resonance", &resonance, 0.0f, 1.0f, "%.2f"))
        {
            if (!resonanceMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdResonance)))
                    *p = resonance;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!resonanceMod) adjustParamOnWheel(ap.getParameter(paramIdResonance), "resonance", resonance);
        if (resonanceMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (resonanceMod)
        {
            ThemeText("Resonance (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Resonance");
        }
        HelpMarker("Resonance frequency (Shakers)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID(); // End module instance ID
}

void StkPercussionModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Channels 0, 2, 3, 4, 5, 6 are now inline (all modulation inputs except Strike)
    // Only draw Strike (ch1) and Output as parallel pins
    helpers.drawParallelPins("Strike", 1, nullptr, -1);
    helpers.drawAudioOutputPin("Out", 0);
}

juce::String StkPercussionModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Freq Mod";
        case 1: return "Strike";
        case 2: return "Velocity";
        case 3: return "Stick Hardness";
        case 4: return "Strike Position";
        case 5: return "Decay";
        case 6: return "Resonance";
        default: return juce::String(channel);
    }
}

juce::String StkPercussionModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0) return "Out";
    return juce::String(channel);
}
#endif

