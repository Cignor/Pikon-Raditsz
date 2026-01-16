#include "StkWindModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>

// Compile-time check for STK_FOUND
#ifdef STK_FOUND
    #define STK_AVAILABLE_AT_COMPILE_TIME true
#else
    #define STK_AVAILABLE_AT_COMPILE_TIME false
#endif

StkWindModuleProcessor::StkWindModuleProcessor()
    : ModuleProcessor(BusesProperties()
                      .withInput("Inputs", juce::AudioChannelSet::discreteChannels(8), true) // ch0: Freq Mod, ch1: Gate, ch2: Breath, ch3: Vibrato Depth, ch4: Vibrato Rate, ch5: Reed Stiffness, ch6: Jet Delay, ch7: Lip Tension
                      .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "StkWindParams", createParameterLayout())
{
    frequencyParam = apvts.getRawParameterValue(paramIdFrequency);
    instrumentTypeParam = apvts.getRawParameterValue(paramIdInstrumentType);
    breathPressureParam = apvts.getRawParameterValue(paramIdBreathPressure);
    vibratoRateParam = apvts.getRawParameterValue(paramIdVibratoRate);
    vibratoDepthParam = apvts.getRawParameterValue(paramIdVibratoDepth);
    reedStiffnessParam = apvts.getRawParameterValue(paramIdReedStiffness);
    jetDelayParam = apvts.getRawParameterValue(paramIdJetDelay);
    lipTensionParam = apvts.getRawParameterValue(paramIdLipTension);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout StkWindModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdFrequency, "Frequency",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.25f), 440.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdInstrumentType, "Instrument Type",
        juce::StringArray { "Flute", "Clarinet", "Saxophone", "Brass" }, 0));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdBreathPressure, "Breath Pressure",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.7f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdVibratoRate, "Vibrato Rate",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.1f, 1.0f), 5.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdVibratoDepth, "Vibrato Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.2f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdReedStiffness, "Reed Stiffness",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdJetDelay, "Jet Delay",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdLipTension, "Lip Tension",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    return { params.begin(), params.end() };
}

void StkWindModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    
    juce::Logger::writeToLog("[STK Wind] prepareToPlay: sampleRate=" + juce::String(sampleRate) + " blockSize=" + juce::String(samplesPerBlock));
    
    // Initialize STK wrapper
    StkWrapper::initializeStk(sampleRate);
    
    // Create initial instrument
    updateInstrument();
    
#ifdef STK_FOUND
    if (instrument)
    {
        instrument->setSampleRate(sampleRate);
        juce::Logger::writeToLog("[STK Wind] Instrument created and initialized at " + juce::String(sampleRate) + " Hz");
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

void StkWindModuleProcessor::updateInstrument()
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
            case 0: // Flute
                instrument = std::make_unique<stk::Flute>(20.0);
                break;
            case 1: // Clarinet
                instrument = std::make_unique<stk::Clarinet>(20.0);
                break;
            case 2: // Saxophone
                instrument = std::make_unique<stk::Saxofony>(20.0);
                break;
            case 3: // Brass
                instrument = std::make_unique<stk::Brass>(20.0);
                break;
            default:
                instrument = std::make_unique<stk::Flute>(20.0);
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
        juce::Logger::writeToLog("[STK Wind] EXCEPTION creating instrument: " + juce::String(e.what()));
        try
        {
            instrument = std::make_unique<stk::Flute>(20.0);
            if (instrument)
            {
                instrument->setSampleRate(currentSampleRate);
            }
        }
        catch (const std::exception& e2)
        {
            juce::Logger::writeToLog("[STK Wind] EXCEPTION in fallback: " + juce::String(e2.what()));
        }
    }
#else
    juce::ignoreUnused(currentInstrumentType);
#endif
}

void StkWindModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
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
    const float* breathCV = (inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;
    const float* vibratoCV = (inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* vibratoRateCV = (inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;
    const float* reedStiffnessCV = (inBus.getNumChannels() > 5) ? inBus.getReadPointer(5) : nullptr;
    const float* jetDelayCV = (inBus.getNumChannels() > 6) ? inBus.getReadPointer(6) : nullptr;
    const float* lipTensionCV = (inBus.getNumChannels() > 7) ? inBus.getReadPointer(7) : nullptr;

    const bool freqActive = isParamInputConnected(paramIdFreqMod);
    const bool gateActive = isParamInputConnected(paramIdGateMod);
    const bool breathActive = isParamInputConnected(paramIdBreathMod);
    const bool vibratoActive = isParamInputConnected(paramIdVibratoMod);
    const bool vibratoRateActive = isParamInputConnected(paramIdVibratoRateMod);
    const bool reedStiffnessActive = isParamInputConnected(paramIdReedStiffnessMod);
    const bool jetDelayActive = isParamInputConnected(paramIdJetDelayMod);
    const bool lipTensionActive = isParamInputConnected(paramIdLipTensionMod);

    const float baseFrequency = frequencyParam != nullptr ? frequencyParam->load() : 440.0f;
    const float baseBreathPressure = breathPressureParam != nullptr ? breathPressureParam->load() : 0.7f;
    const float baseVibratoRate = vibratoRateParam != nullptr ? vibratoRateParam->load() : 5.0f;
    const float baseVibratoDepth = vibratoDepthParam != nullptr ? vibratoDepthParam->load() : 0.2f;

    // Check if instrument type changed (calculate once per block)
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

        // Calculate breath pressure with CV modulation
        float breathPressure = baseBreathPressure;
        if (breathActive && breathCV)
        {
            const float cvRaw = breathCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            breathPressure = cv01;
        }
        breathPressure = juce::jlimit(0.0f, 1.0f, breathPressure);

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
        const bool isGateHigh = smoothedGate > 0.1f;

        // Calculate vibrato with CV modulation
        float vibratoRate = baseVibratoRate;
        float vibratoDepth = baseVibratoDepth;
        if (vibratoActive && vibratoCV)
        {
            const float cvRaw = vibratoCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            vibratoDepth = juce::jlimit(0.0f, 1.0f, baseVibratoDepth + (cv01 - 0.5f) * 0.5f);
        }
        if (vibratoRateActive && vibratoRateCV)
        {
            const float cvRaw = vibratoRateCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            vibratoRate = juce::jlimit(0.0f, 20.0f, cv01 * 20.0f);
        }
        
        // Calculate instrument-specific parameters with CV modulation (for telemetry)
        const int instrumentType = (int)(instrumentTypeParam != nullptr ? instrumentTypeParam->load() : 0.0f);
        float reedStiffness = reedStiffnessParam != nullptr ? reedStiffnessParam->load() : 0.5f;
        float jetDelay = jetDelayParam != nullptr ? jetDelayParam->load() : 0.5f;
        float lipTension = lipTensionParam != nullptr ? lipTensionParam->load() : 0.5f;
        
        if (reedStiffnessActive && reedStiffnessCV)
        {
            const float cvRaw = reedStiffnessCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            reedStiffness = cv01;
        }
        reedStiffness = juce::jlimit(0.0f, 1.0f, reedStiffness);
        
        if (jetDelayActive && jetDelayCV)
        {
            const float cvRaw = jetDelayCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            jetDelay = cv01;
        }
        jetDelay = juce::jlimit(0.0f, 1.0f, jetDelay);
        
        if (lipTensionActive && lipTensionCV)
        {
            const float cvRaw = lipTensionCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            lipTension = cv01;
        }
        lipTension = juce::jlimit(0.0f, 1.0f, lipTension);

        // Generate audio sample
        float sample = 0.0f;
#ifdef STK_FOUND
        if (instrument)
        {
            instrument->setFrequency(freq);
            
            // Wind instruments use startBlowing/stopBlowing (need to cast to specific types)
            // Flute works better with noteOn() which adds a base amplitude
            if (isGateHigh && !wasGateHigh)
            {
                // Gate just went high - start blowing
                if (auto* flute = dynamic_cast<stk::Flute*>(instrument.get()))
                {
                    // Flute noteOn() adds base 1.1, so use breathPressure directly
                    // Ensure minimum breath pressure for audible sound
                    const float fluteAmplitude = juce::jmax(0.3f, breathPressure);
                    flute->noteOn(freq, fluteAmplitude);
                }
                else if (auto* clarinet = dynamic_cast<stk::Clarinet*>(instrument.get()))
                    clarinet->startBlowing(breathPressure, 0.5f);
                else if (auto* sax = dynamic_cast<stk::Saxofony*>(instrument.get()))
                    sax->startBlowing(breathPressure, 0.5f);
                else if (auto* brass = dynamic_cast<stk::Brass*>(instrument.get()))
                    brass->startBlowing(breathPressure, 0.5f);
            }
            else if (!isGateHigh && wasGateHigh)
            {
                // Gate just went low - stop blowing
                if (auto* flute = dynamic_cast<stk::Flute*>(instrument.get()))
                    flute->noteOff(0.1f);
                else if (auto* clarinet = dynamic_cast<stk::Clarinet*>(instrument.get()))
                    clarinet->stopBlowing(0.1f);
                else if (auto* sax = dynamic_cast<stk::Saxofony*>(instrument.get()))
                    sax->stopBlowing(0.1f);
                else if (auto* brass = dynamic_cast<stk::Brass*>(instrument.get()))
                    brass->stopBlowing(0.1f);
            }
            else if (isGateHigh)
            {
                // Continuous blowing - update breath pressure via controlChange
                // Control 128 = Breath Pressure (for most wind instruments)
                // For flute, use controlChange to update breath pressure while playing
                if (auto* flute = dynamic_cast<stk::Flute*>(instrument.get()))
                {
                    // Flute controlChange(128) sets ADSR target, use breathPressure directly
                    const float flutePressure = juce::jmax(0.3f, breathPressure);
                    flute->controlChange(128, flutePressure * 128.0f);
                }
                else
                {
                    instrument->controlChange(128, breathPressure * 128.0f);
                }
            }
            
            wasGateHigh = isGateHigh;
            
            // Update vibrato (common to all wind instruments)
            instrument->controlChange(11, vibratoRate * 128.0f / 20.0f); // Vibrato Frequency (0-128 maps to 0-20 Hz)
            instrument->controlChange(1, vibratoDepth * 128.0f); // Vibrato Gain
            
            // Instrument-specific parameters (use pre-calculated CV-modulated values)
            if (auto* clarinet = dynamic_cast<stk::Clarinet*>(instrument.get()))
            {
                clarinet->controlChange(2, reedStiffness * 128.0f); // Reed Stiffness
            }
            else if (auto* sax = dynamic_cast<stk::Saxofony*>(instrument.get()))
            {
                sax->controlChange(2, reedStiffness * 128.0f); // Reed Stiffness
            }
            else if (auto* flute = dynamic_cast<stk::Flute*>(instrument.get()))
            {
                flute->controlChange(2, jetDelay * 128.0f); // Jet Delay
            }
            else if (auto* brass = dynamic_cast<stk::Brass*>(instrument.get()))
            {
                brass->controlChange(2, lipTension * 128.0f); // Lip Tension
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
            vizData.currentInstrumentType.store(currentInstrumentType);
            vizData.gateLevel.store(smoothedGate);
            vizData.outputLevel.store(sample);
            vizData.breathPressure.store(breathPressure);
        }
#endif
        
        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("frequency_live", freq);
            setLiveParamValue("breathPressure_live", breathPressure);
            setLiveParamValue("vibratoRate_live", vibratoRate);
            setLiveParamValue("vibratoDepth_live", vibratoDepth);
            
            // Instrument-specific parameters (use pre-calculated CV-modulated values)
            if (instrumentType == 1 || instrumentType == 2) // Clarinet or Saxophone
            {
                setLiveParamValue("reedStiffness_live", reedStiffness);
            }
            else if (instrumentType == 0) // Flute
            {
                setLiveParamValue("jetDelay_live", jetDelay);
            }
            else if (instrumentType == 3) // Brass
            {
                setLiveParamValue("lipTension_live", lipTension);
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

void StkWindModuleProcessor::setTimingInfo(const TransportState& state)
{
    const bool wasPlaying = m_currentTransport.isPlaying;
    m_currentTransport = state;
    
    if (state.isPlaying && !wasPlaying)
    {
        m_shouldAutoTrigger = true;
    }
}

void StkWindModuleProcessor::forceStop()
{
#ifdef STK_FOUND
    if (instrument)
    {
        // Need to cast to specific wind instrument type
        if (auto* flute = dynamic_cast<stk::Flute*>(instrument.get()))
            flute->stopBlowing(0.1f);
        else if (auto* clarinet = dynamic_cast<stk::Clarinet*>(instrument.get()))
            clarinet->stopBlowing(0.1f);
        else if (auto* sax = dynamic_cast<stk::Saxofony*>(instrument.get()))
            sax->stopBlowing(0.1f);
        else if (auto* brass = dynamic_cast<stk::Brass*>(instrument.get()))
            brass->stopBlowing(0.1f);
    }
#endif
    smoothedGate = 0.0f;
    wasGateHigh = false;
}

bool StkWindModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0;
    if (paramId == paramIdFreqMod)          { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdGateMod)          { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdBreathMod)        { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdVibratoMod)       { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdVibratoRateMod)   { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdReedStiffnessMod) { outChannelIndexInBus = 5; return true; }
    if (paramId == paramIdJetDelayMod)      { outChannelIndexInBus = 6; return true; }
    if (paramId == paramIdLipTensionMod)    { outChannelIndexInBus = 7; return true; }
    return false;
}

#if defined(PRESET_CREATOR_UI)
void StkWindModuleProcessor::drawParametersInNode(float itemWidth,
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
    const float breathPressure = vizData.breathPressure.load();

    // Waveform visualization in child window
    const auto& freqColors = theme.modules.frequency_graph;
    const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
    const float waveHeight = 140.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);

    if (ImGui::BeginChild("StkWindOscilloscope", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
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

        // Draw breath pressure indicator
        if (breathPressure > 0.0f)
        {
            const ImU32 breathColor = IM_COL32(100, 200, 255, 255);
            const float breathY = p0.y + graphSize.y - (breathPressure * graphSize.y * 0.3f);
            const float clampedBreathY = juce::jlimit(p0.y + 2.0f, p1.y - 2.0f, breathY);
            drawList->AddLine(ImVec2(p0.x, clampedBreathY), ImVec2(p1.x, clampedBreathY), breathColor, 1.5f);
        }

        drawList->PopClipRect();

        // Frequency and instrument info overlay
        const char* instrumentNames[] = { "Flute", "Clarinet", "Saxophone", "Brass" };
        const char* instrumentName = (currentInstType >= 0 && currentInstType < 4) ? instrumentNames[currentInstType] : "Unknown";

        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%.1f Hz | %s", currentFreq, instrumentName);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##stkWindVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Instrument Type
    ThemeText("Instrument", theme.text.section_header);
    ImGui::Spacing();
    
    int instrumentType = 0;
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdInstrumentType)))
        instrumentType = p->getIndex();
    
    const char* instrumentNames[] = { "Flute", "Clarinet", "Saxophone", "Brass" };
    if (ImGui::Combo("##instrument", &instrumentType, instrumentNames, 4))
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
            const int maxIndex = 3;
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
    HelpMarker("Select wind instrument type");

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

    // Breath Pressure
    ThemeText("Breath", theme.text.section_header);
    ImGui::Spacing();
    
    const bool breathMod = isParamModulated(paramIdBreathMod);
    if (breathMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Breath Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }
    
    if (breathMod) ImGui::BeginDisabled();
    float breath = breathPressureParam != nullptr ? getLiveParamValueFor(paramIdBreathMod, "breathPressure_live", breathPressureParam->load()) : 0.7f;
    if (ImGui::SliderFloat("##breath", &breath, 0.0f, 1.0f, "%.2f"))
    {
        if (!breathMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdBreathPressure)))
                *p = breath;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!breathMod) adjustParamOnWheel(ap.getParameter(paramIdBreathPressure), "breath", breath);
    if (breathMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (breathMod)
    {
        ThemeText("Pressure (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Pressure");
    }
    HelpMarker("Breath pressure applied to the instrument");

    ImGui::Spacing();

    // Vibrato
    ThemeText("Vibrato", theme.text.section_header);
    ImGui::Spacing();
    
    const bool vibratoRateMod = isParamModulated(paramIdVibratoRateMod);
    if (vibratoRateMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Vibrato Rate Mod (Channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))
            ImGui::SameLine();
    }
    
    if (vibratoRateMod) ImGui::BeginDisabled();
    float vibratoRate = vibratoRateParam != nullptr ? getLiveParamValueFor(paramIdVibratoRateMod, "vibratoRate_live", vibratoRateParam->load()) : 5.0f;
    if (ImGui::SliderFloat("##vibratoRate", &vibratoRate, 0.0f, 20.0f, "%.1f Hz"))
    {
        if (!vibratoRateMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdVibratoRate)))
                *p = vibratoRate;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!vibratoRateMod) adjustParamOnWheel(ap.getParameter(paramIdVibratoRate), "vibratoRate", vibratoRate);
    if (vibratoRateMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (vibratoRateMod)
    {
        ThemeText("Rate (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Rate");
    }
    HelpMarker("Vibrato frequency");

    ImGui::Spacing();

    const bool vibratoMod = isParamModulated(paramIdVibratoMod);
    if (vibratoMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Vibrato Mod (Channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))
            ImGui::SameLine();
    }
    
    if (vibratoMod) ImGui::BeginDisabled();
    float vibratoDepth = vibratoDepthParam != nullptr ? getLiveParamValueFor(paramIdVibratoMod, "vibratoDepth_live", vibratoDepthParam->load()) : 0.2f;
    if (ImGui::SliderFloat("##vibratoDepth", &vibratoDepth, 0.0f, 1.0f, "%.2f"))
    {
        if (!vibratoMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdVibratoDepth)))
                *p = vibratoDepth;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!vibratoMod) adjustParamOnWheel(ap.getParameter(paramIdVibratoDepth), "vibratoDepth", vibratoDepth);
    if (vibratoMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (vibratoMod)
    {
        ThemeText("Depth (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Depth");
    }
    HelpMarker("Vibrato amplitude");

    ImGui::Spacing();

    // Instrument-specific parameters (only show relevant ones)
    if (currentInstType == 1 || currentInstType == 2) // Clarinet or Saxophone
    {
        ThemeText("Reed", theme.text.section_header);
        ImGui::Spacing();
        
        const bool reedStiffnessMod = isParamModulated(paramIdReedStiffnessMod);
        if (reedStiffnessMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Reed Stiffness Mod (Channel 5)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(5))
                ImGui::SameLine();
        }
        
        if (reedStiffnessMod) ImGui::BeginDisabled();
        float reedStiffness = reedStiffnessParam != nullptr ? getLiveParamValueFor(paramIdReedStiffnessMod, "reedStiffness_live", reedStiffnessParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##reed", &reedStiffness, 0.0f, 1.0f, "%.2f"))
        {
            if (!reedStiffnessMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdReedStiffness)))
                    *p = reedStiffness;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!reedStiffnessMod) adjustParamOnWheel(ap.getParameter(paramIdReedStiffness), "reedStiffness", reedStiffness);
        if (reedStiffnessMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (reedStiffnessMod)
        {
            ThemeText("Stiffness (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Stiffness");
        }
        HelpMarker("Reed stiffness (Clarinet/Saxophone)");
        
        ImGui::Spacing();
    }
    else if (currentInstType == 0) // Flute
    {
        ThemeText("Jet", theme.text.section_header);
        ImGui::Spacing();
        
        const bool jetDelayMod = isParamModulated(paramIdJetDelayMod);
        if (jetDelayMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Jet Delay Mod (Channel 6)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(6))
                ImGui::SameLine();
        }
        
        if (jetDelayMod) ImGui::BeginDisabled();
        float jetDelay = jetDelayParam != nullptr ? getLiveParamValueFor(paramIdJetDelayMod, "jetDelay_live", jetDelayParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##jet", &jetDelay, 0.0f, 1.0f, "%.2f"))
        {
            if (!jetDelayMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdJetDelay)))
                    *p = jetDelay;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!jetDelayMod) adjustParamOnWheel(ap.getParameter(paramIdJetDelay), "jetDelay", jetDelay);
        if (jetDelayMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (jetDelayMod)
        {
            ThemeText("Delay (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Delay");
        }
        HelpMarker("Jet delay (Flute)");
        
        ImGui::Spacing();
    }
    else if (currentInstType == 3) // Brass
    {
        ThemeText("Lip", theme.text.section_header);
        ImGui::Spacing();
        
        const bool lipTensionMod = isParamModulated(paramIdLipTensionMod);
        if (lipTensionMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Lip Tension Mod (Channel 7)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(7))
                ImGui::SameLine();
        }
        
        if (lipTensionMod) ImGui::BeginDisabled();
        float lipTension = lipTensionParam != nullptr ? getLiveParamValueFor(paramIdLipTensionMod, "lipTension_live", lipTensionParam->load()) : 0.5f;
        if (ImGui::SliderFloat("##lip", &lipTension, 0.0f, 1.0f, "%.2f"))
        {
            if (!lipTensionMod)
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdLipTension)))
                    *p = lipTension;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!lipTensionMod) adjustParamOnWheel(ap.getParameter(paramIdLipTension), "lipTension", lipTension);
        if (lipTensionMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (lipTensionMod)
        {
            ThemeText("Tension (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Tension");
        }
        HelpMarker("Lip tension (Brass)");
        
        ImGui::Spacing();
    }

    ImGui::PopItemWidth();
    ImGui::PopID(); // End module instance ID
}

void StkWindModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Channels 0, 2, 3, 4, 5, 6, 7 are now inline (all modulation inputs except Gate)
    // Only draw Gate (ch1) and Output as parallel pins
    helpers.drawParallelPins("Gate", 1, nullptr, -1);
    helpers.drawAudioOutputPin("Out", 0);
}

juce::String StkWindModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Freq Mod";
        case 1: return "Gate";
        case 2: return "Breath";
        case 3: return "Vibrato";
        case 4: return "Vibrato Rate";
        case 5: return "Reed Stiffness";
        case 6: return "Jet Delay";
        case 7: return "Lip Tension";
        default: return juce::String(channel);
    }
}

juce::String StkWindModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0) return "Out";
    return juce::String(channel);
}
#endif

