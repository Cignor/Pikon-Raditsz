#include "StkStringModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>

// Compile-time check for STK_FOUND
#ifdef STK_FOUND
    #define STK_AVAILABLE_AT_COMPILE_TIME true
#else
    #define STK_AVAILABLE_AT_COMPILE_TIME false
#endif

StkStringModuleProcessor::StkStringModuleProcessor()
    : ModuleProcessor(BusesProperties()
                      .withInput("Inputs", juce::AudioChannelSet::discreteChannels(5), true) // ch0: Freq Mod, ch1: Gate, ch2: Velocity, ch3: Damping, ch4: Pickup Pos
                      .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "StkStringParams", createParameterLayout())
{
    frequencyParam = apvts.getRawParameterValue(paramIdFrequency);
    instrumentTypeParam = apvts.getRawParameterValue(paramIdInstrumentType);
    dampingParam = apvts.getRawParameterValue(paramIdDamping);
    pickupPosParam = apvts.getRawParameterValue(paramIdPickupPos);
    brightnessParam = apvts.getRawParameterValue(paramIdBrightness);
    bodySizeParam = apvts.getRawParameterValue(paramIdBodySize);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout StkStringModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdFrequency, "Frequency",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.25f), 440.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdInstrumentType, "Instrument Type",
        juce::StringArray { "Guitar", "Violin", "Cello", "Sitar", "Banjo" }, 0));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdDamping, "Damping",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdPickupPos, "Pickup Position",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdBrightness, "Brightness",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdBodySize, "Body Size",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f), 0.5f));
    
    return { params.begin(), params.end() };
}

void StkStringModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    
    juce::Logger::writeToLog("[STK String] prepareToPlay: sampleRate=" + juce::String(sampleRate) + " blockSize=" + juce::String(samplesPerBlock));
    
    // Log compile-time STK availability
    juce::Logger::writeToLog("[STK String] Compile-time check: STK_AVAILABLE_AT_COMPILE_TIME=" + 
        juce::String(STK_AVAILABLE_AT_COMPILE_TIME ? "true" : "false"));
    
    // Initialize STK wrapper
    StkWrapper::initializeStk(sampleRate);
    
    juce::Logger::writeToLog("[STK String] StkWrapper initialized. Runtime check: isInitialized()=" + 
        juce::String(StkWrapper::isInitialized() ? "true" : "false"));
    
    // Create initial instrument
    updateInstrument();
    
#ifdef STK_FOUND
    if (instrument)
    {
        instrument->setSampleRate(sampleRate);
        juce::Logger::writeToLog("[STK String] Instrument created and initialized at " + juce::String(sampleRate) + " Hz");
        
        // Immediately trigger a test note to verify it works
        instrument->noteOn(440.0f, 1.0f);
        juce::Logger::writeToLog("[STK String] Test noteOn(440Hz, 1.0) called in prepareToPlay");
    }
    else
    {
        juce::Logger::writeToLog("[STK String] ERROR: Instrument is null after updateInstrument()");
    }
#else
    juce::Logger::writeToLog("[STK String] WARNING: STK_FOUND not defined - STK library not available!");
#endif
    
    smoothedGate = 0.0f;
    wasGateHigh = false;
    pluckReTriggerCounter = 0;
    
    // Auto-trigger initial note if no gate is connected (for testing/continuous sound)
    m_shouldAutoTrigger = true;
    juce::Logger::writeToLog("[STK String] Auto-trigger flag set to true");
    
#if defined(PRESET_CREATOR_UI)
    // Initialize visualization buffer
    vizOutputBuffer.setSize(1, vizBufferSize, false, true, false);
    vizOutputBuffer.clear();
    vizWritePos = 0;
#endif
}

void StkStringModuleProcessor::updateInstrument()
{
#ifdef STK_FOUND
    const int instrumentType = (int)(instrumentTypeParam != nullptr ? instrumentTypeParam->load() : 0.0f);
    
    if (instrumentType == currentInstrumentType && instrument != nullptr)
        return; // No change needed
    
    currentInstrumentType = instrumentType;
    
    juce::Logger::writeToLog("[STK String] Creating instrument type " + juce::String(instrumentType) + " at sample rate " + juce::String(currentSampleRate));
    
    try
    {
        switch (instrumentType)
        {
            case 0: // Guitar
                instrument = std::make_unique<stk::Plucked>(0.5f);
                juce::Logger::writeToLog("[STK String] Created Plucked (Guitar)");
                break;
            case 1: // Violin
                instrument = std::make_unique<stk::Bowed>();
                juce::Logger::writeToLog("[STK String] Created Bowed (Violin)");
                break;
            case 2: // Cello
                instrument = std::make_unique<stk::Bowed>();
                juce::Logger::writeToLog("[STK String] Created Bowed (Cello)");
                break;
            case 3: // Sitar
                instrument = std::make_unique<stk::Sitar>();
                juce::Logger::writeToLog("[STK String] Created Sitar");
                break;
            case 4: // Banjo
                instrument = std::make_unique<stk::Plucked>(0.3f);
                juce::Logger::writeToLog("[STK String] Created Plucked (Banjo)");
                break;
            default:
                instrument = std::make_unique<stk::Plucked>(0.5f);
                juce::Logger::writeToLog("[STK String] Created Plucked (default)");
                break;
        }
        
        if (instrument)
        {
            instrument->setSampleRate(currentSampleRate);
            juce::Logger::writeToLog("[STK String] Instrument sample rate set to " + juce::String(currentSampleRate));
            
            // Immediately trigger a note to test
            instrument->noteOn(440.0f, 1.0f);
            juce::Logger::writeToLog("[STK String] Test noteOn(440Hz, 1.0) called");
        }
        else
        {
            juce::Logger::writeToLog("[STK String] ERROR: Instrument creation returned null!");
        }
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog("[STK String] EXCEPTION creating instrument: " + juce::String(e.what()));
        // Fallback to Plucked if creation fails
        try
        {
            instrument = std::make_unique<stk::Plucked>(0.5f);
            if (instrument)
            {
                instrument->setSampleRate(currentSampleRate);
                juce::Logger::writeToLog("[STK String] Fallback Plucked created");
            }
        }
        catch (const std::exception& e2)
        {
            juce::Logger::writeToLog("[STK String] EXCEPTION in fallback: " + juce::String(e2.what()));
        }
    }
#else
    juce::ignoreUnused(currentInstrumentType);
    // Only log once to avoid spam - STK_FOUND check happens at compile time
    static bool loggedOnce = false;
    if (!loggedOnce)
    {
        juce::Logger::writeToLog("[STK String] ERROR: updateInstrument called but STK_FOUND not defined at compile time. CMake needs to be reconfigured!");
        loggedOnce = true;
    }
#endif
}

void StkStringModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
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
    const float* dampingCV = (inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* pickupCV = (inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;

    const bool freqActive = isParamInputConnected(paramIdFreqMod);
    const bool gateActive = isParamInputConnected(paramIdGateMod);
    const bool velocityActive = isParamInputConnected(paramIdVelocityMod);
    const bool dampingActive = isParamInputConnected(paramIdDampingMod);
    const bool pickupActive = isParamInputConnected(paramIdPickupMod);

    const float baseFrequency = frequencyParam != nullptr ? frequencyParam->load() : 440.0f;
    const float baseDamping = dampingParam != nullptr ? dampingParam->load() : 0.5f;
    const float basePickupPos = pickupPosParam != nullptr ? pickupPosParam->load() : 0.5f;
    const float baseBrightness = brightnessParam != nullptr ? brightnessParam->load() : 0.5f;
    const float baseBodySize = bodySizeParam != nullptr ? bodySizeParam->load() : 0.5f;

    // Check if instrument type changed
    const int instrumentType = (int)(instrumentTypeParam != nullptr ? instrumentTypeParam->load() : 0.0f);
    if (instrumentType != currentInstrumentType)
    {
        updateInstrument();
    }

    constexpr float GATE_SMOOTHING_FACTOR = 0.05f; // Faster smoothing for better response

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
            
            // Relative modulation: ±1 octave around base frequency
            const float octaveOffset = (cv01 - 0.5f) * 2.0f; // ±1 octave
            freq = baseFrequency * std::pow(2.0f, octaveOffset);
        }
        freq = juce::jlimit(20.0f, 2000.0f, freq);

        // Calculate damping with CV modulation
        float damping = baseDamping;
        if (dampingActive && dampingCV)
        {
            const float cvRaw = dampingCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            damping = cv01;
        }

        // Calculate pickup position with CV modulation
        float pickupPos = basePickupPos;
        if (pickupActive && pickupCV)
        {
            const float cvRaw = pickupCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            pickupPos = cv01;
        }

        // Handle gate/trigger
        float gateLevel = 1.0f; // Default to high when no gate connected and transport playing
        if (gateActive && gateCV)
        {
            // Normalize CV: assume bipolar [-1, 1] or unipolar [0, 1]
            const float cvRaw = gateCV[i];
            if (cvRaw >= 0.0f && cvRaw <= 1.0f)
                gateLevel = cvRaw; // Already unipolar
            else
                gateLevel = juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f); // Bipolar to unipolar
            
            // Debug logging
            if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0)
            {
                juce::Logger::writeToLog("[STK String] Gate CV: raw=" + juce::String(cvRaw, 3) + " normalized=" + juce::String(gateLevel, 3));
            }
        }
        else if (!m_currentTransport.isPlaying)
        {
            // Transport stopped - silence output
            gateLevel = 0.0f;
            // Debug logging
            if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0)
            {
                juce::Logger::writeToLog("[STK String] Gate=0 (transport not playing) gateActive=" + juce::String(gateActive ? "yes" : "no"));
            }
        }
        else
        {
            // No gate connected, transport playing - keep gate high for continuous sound
            gateLevel = 1.0f;
            if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0)
            {
                juce::Logger::writeToLog("[STK String] Gate=1.0 (default, no gate connected, transport playing)");
            }
        }

        // Auto-trigger on first sample if no gate connected and transport just started
        if (m_shouldAutoTrigger && !gateActive && m_currentTransport.isPlaying && i == 0)
        {
            wasGateHigh = false; // Force rising edge detection
            gateLevel = 1.0f; // Set gate high to trigger note
            m_shouldAutoTrigger = false; // Only trigger once
            juce::Logger::writeToLog("[STK String] Auto-triggering note (no gate connected, transport playing)");
        }

        // Calculate velocity for this sample (needed for noteOn and re-triggering)
        float velocity = 1.0f;
        if (velocityActive && velocityCV)
        {
            const float cvRaw = velocityCV[i];
            const float cv01 = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                             ? juce::jlimit(0.0f, 1.0f, cvRaw)
                             : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            velocity = cv01;
        }
        
        // Detect gate rising edge for noteOn
        const bool isGateHigh = gateLevel > 0.3f; // Lower threshold for better triggering
        if (isGateHigh && !wasGateHigh)
        {
            // Rising edge - trigger note
            
#ifdef STK_FOUND
            if (instrument)
            {
                // Set frequency before triggering
                instrument->setFrequency(freq);
                
                // Apply physical modeling parameters
                // Note: Plucked and Sitar don't expose pluck position or loop gain as public APIs
                // These are internal implementation details in STK
                if (auto* plucked = dynamic_cast<stk::Plucked*>(instrument.get()))
                {
                    // Plucked only supports setFrequency() - other parameters are internal
                    juce::Logger::writeToLog("[STK String] Plucked: freq=" + juce::String(freq, 1) + " (pickupPos and damping not available via STK API)");
                }
                else if (auto* bowed = dynamic_cast<stk::Bowed*>(instrument.get()))
                {
                    bowed->setFrequency(freq);
                    bowed->setVibrato(0.0f);  // setVibrato() not setVibratoGain()
                    // Use controlChange for bow pressure (2) and bow position (4)
                    // Values are in 0-128 range, so scale from 0-1
                    bowed->controlChange(2, velocity * 128.0f);  // Bow Pressure
                    bowed->controlChange(4, pickupPos * 128.0f); // Bow Position
                    juce::Logger::writeToLog("[STK String] Bowed: freq=" + juce::String(freq, 1) + " velocity=" + juce::String(velocity, 3) + " pickupPos=" + juce::String(pickupPos, 3));
                }
                else if (auto* sitar = dynamic_cast<stk::Sitar*>(instrument.get()))
                {
                    // Sitar only supports setFrequency() - pluck position is internal
                    juce::Logger::writeToLog("[STK String] Sitar: freq=" + juce::String(freq, 1) + " (pickupPos not available via STK API)");
                }
                
                instrument->noteOn(freq, velocity);
                juce::Logger::writeToLog("[STK String] noteOn triggered: freq=" + juce::String(freq, 1) + " Hz, velocity=" + juce::String(velocity, 2) + 
                    " instrument=" + juce::String(instrumentType) + " gateActive=" + juce::String(gateActive ? "yes" : "no"));
            }
            else
            {
                juce::Logger::writeToLog("[STK String] ERROR: noteOn called but instrument is null!");
            }
#else
            // Only log once to avoid spam
            static bool noteOnLoggedOnce = false;
            if (!noteOnLoggedOnce)
            {
                juce::Logger::writeToLog("[STK String] ERROR: noteOn attempted but STK_FOUND not defined at compile time. CMake needs to be reconfigured!");
                noteOnLoggedOnce = true;
            }
#endif
        }
        else if (!isGateHigh && wasGateHigh)
        {
            // Falling edge - trigger noteOff
#ifdef STK_FOUND
            if (instrument)
                instrument->noteOff(0.5f);
#endif
        }
        wasGateHigh = isGateHigh;

        // For Plucked instruments, continuously re-trigger while gate is high
        // (they decay quickly after initial pluck)
#ifdef STK_FOUND
        if (instrument && isGateHigh)
        {
            if (auto* plucked = dynamic_cast<stk::Plucked*>(instrument.get()))
            {
                // Re-trigger pluck very frequently (every ~20ms at 48kHz = ~960 samples)
                // Plucked instruments decay very quickly, so we need frequent re-triggering
                if (++pluckReTriggerCounter >= 960)
                {
                    plucked->pluck(juce::jmax(0.3f, velocity)); // Ensure minimum velocity for audibility
                    pluckReTriggerCounter = 0;
                }
            }
            else
            {
                // Reset counter for non-plucked instruments
                pluckReTriggerCounter = 0;
            }
        }
        else
        {
            // Reset counter when gate is low
            pluckReTriggerCounter = 0;
        }
#endif

        // Generate audio sample
        float sample = 0.0f;
#ifdef STK_FOUND
        if (instrument)
        {
            sample = instrument->tick();
            
            // Immediate logging for first 10 samples of first block to diagnose
            static int logCounter = 0;
            if (logCounter < 10)
            {
                juce::Logger::writeToLog("[STK String] tick[" + juce::String(logCounter) + "] sample=" + juce::String(sample, 6) + 
                    " freq=" + juce::String(freq, 1) + " gate=" + juce::String(gateLevel, 3) + " isGateHigh=" + juce::String(isGateHigh ? "true" : "false"));
                logCounter++;
            }
            
            // Debug logging (first sample of block only)
            if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0) // Every ~512 samples
            {
                juce::Logger::writeToLog("[STK String] tick() sample=" + juce::String(sample, 6) + 
                    " freq=" + juce::String(freq, 1) + " gate=" + juce::String(gateLevel, 3));
            }
            
            // Apply significant gain boost (STK instruments can be very quiet)
            sample *= 10.0f; // Boost by ~20dB to make it clearly audible
            
            // Apply brightness and body size (simple filtering)
            // Brightness: high-pass effect
            static float lastSample = 0.0f;
            const float brightnessAmount = baseBrightness;
            sample = sample * (1.0f - brightnessAmount) + (sample - lastSample) * brightnessAmount;
            lastSample = sample;
            
            // Body size: low-pass effect (simplified)
            static float bodyFilterState = 0.0f;
            const float bodyAmount = baseBodySize;
            bodyFilterState += (sample - bodyFilterState) * (1.0f - bodyAmount * 0.1f);
            sample = sample * (1.0f - bodyAmount * 0.3f) + bodyFilterState * bodyAmount * 0.3f;
        }
        else
        {
            // Log if instrument is null
            if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0)
            {
                juce::Logger::writeToLog("[STK String] WARNING: instrument is null!");
            }
        }
#else
        // Log when STK is not available
        if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0)
        {
            juce::Logger::writeToLog("[STK String] STK_FOUND not defined - no audio generation");
        }
#endif

        // Apply gate smoothing (faster response for better triggering)
        const float GATE_SMOOTHING_FAST = 0.1f; // Faster smoothing for better response
        smoothedGate += (gateLevel - smoothedGate) * GATE_SMOOTHING_FAST;
        sample *= smoothedGate;
        
        // Additional gain boost when gate is active to compensate for smoothing
        if (smoothedGate > 0.1f)
        {
            sample *= 1.5f; // Extra boost when gate is active
        }
        
        // Debug logging for output (first 10 samples)
        static int outputLogCounter = 0;
        if (outputLogCounter < 10)
        {
            juce::Logger::writeToLog("[STK String] Output[" + juce::String(outputLogCounter) + "] sample=" + juce::String(sample, 6) + 
                " gateLevel=" + juce::String(gateLevel, 3) + " smoothedGate=" + juce::String(smoothedGate, 3) +
                " wasGateHigh=" + juce::String(wasGateHigh ? "true" : "false") + " outBus.channels=" + juce::String(outBus.getNumChannels()));
            outputLogCounter++;
        }
        
        // Debug logging for output (periodic)
        if (i == 0 && (buffer.getNumSamples() & 0x1FF) == 0)
        {
            juce::Logger::writeToLog("[STK String] Output: sample=" + juce::String(sample, 6) + 
                " gateLevel=" + juce::String(gateLevel, 3) + " smoothedGate=" + juce::String(smoothedGate, 3) +
                " wasGateHigh=" + juce::String(wasGateHigh ? "true" : "false"));
        }

        // Ensure output bus is valid
        if (outBus.getNumChannels() > 0)
        {
            outBus.setSample(0, i, sample);
        }
        else
        {
            if (i == 0)
            {
                juce::Logger::writeToLog("[STK String] ERROR: Output bus has no channels! Cannot write audio.");
            }
        }
        
#if defined(PRESET_CREATOR_UI)
        // Capture output audio for visualization
        if (vizOutputBuffer.getNumSamples() > 0)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizOutputBuffer.setSample(0, writeIdx, sample);
        }

        // Track current state (use last sample for live display)
        if (i == buffer.getNumSamples() - 1)
        {
            vizData.currentFrequency.store(freq);
            vizData.currentInstrumentType.store(currentInstrumentType);
            vizData.gateLevel.store(smoothedGate);
            vizData.outputLevel.store(sample);
        }
#endif
        
        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("frequency_live", freq);
            setLiveParamValue("damping_live", damping);
            setLiveParamValue("pickupPos_live", pickupPos);
        }
    }

    updateOutputTelemetry(buffer);
    
#if defined(PRESET_CREATOR_UI)
    vizWritePos = (vizWritePos + buffer.getNumSamples()) % vizBufferSize;

    // Update visualization data (thread-safe)
    // Downsample waveform from circular buffer
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx = (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) % vizBufferSize;
        if (vizOutputBuffer.getNumSamples() > 0)
            vizData.outputWaveform[i].store(vizOutputBuffer.getSample(0, readIdx));
    }
#endif
}

void StkStringModuleProcessor::setTimingInfo(const TransportState& state)
{
    const bool wasPlaying = m_currentTransport.isPlaying;
    m_currentTransport = state;
    
    // Auto-trigger when transport starts playing (if no gate connected)
    if (state.isPlaying && !wasPlaying)
    {
        m_shouldAutoTrigger = true;
    }
}

void StkStringModuleProcessor::forceStop()
{
#ifdef STK_FOUND
    if (instrument)
        instrument->noteOff(0.5f);
#endif
    smoothedGate = 0.0f;
    wasGateHigh = false;
}

bool StkStringModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All inputs are on the same bus
    if (paramId == paramIdFreqMod)      { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdGateMod)      { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdVelocityMod)   { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdDampingMod)   { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdPickupMod)    { outChannelIndexInBus = 4; return true; }
    return false;
}

#if defined(PRESET_CREATOR_UI)
void StkStringModuleProcessor::drawParametersInNode(float itemWidth,
                                                      const std::function<bool(const juce::String& paramId)>& isParamModulated,
                                                      const std::function<void()>& onModificationEnded,
                                                      const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    auto& ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    
    // Helper for tooltips
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

    // === INSTRUMENT TYPE ===
    ThemeText("Instrument", theme.text.section_header);
    ImGui::Spacing();
    
    int instrumentType = 0;
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdInstrumentType)))
        instrumentType = p->getIndex();
    
    const char* instrumentNames[] = { "Guitar", "Violin", "Cello", "Sitar", "Banjo" };
    if (ImGui::Combo("##instrument", &instrumentType, instrumentNames, 5))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdInstrumentType)))
            *p = instrumentType;
        onModificationEnded();
    }
    
    // Scroll wheel support for instrument type combo
    if (ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int maxIndex = 4; // 0-4 (5 instruments)
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
    HelpMarker("Select physical modeling instrument type\nGuitar: Plucked string\nViolin: Bowed string\nCello: Lower bowed string\nSitar: Indian plucked string\nBanjo: Bright plucked string");

    ImGui::Spacing();
    ImGui::Spacing();

    // === FREQUENCY ===
    ThemeText("Frequency", theme.text.section_header);
    ImGui::Spacing();
    
    const bool freqMod = isParamModulated(paramIdFreqMod);
    if (freqMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Frequency Mod (Channel 0)
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
    HelpMarker("Fundamental frequency of the string\nCV modulation: ±1 octave around slider value\nConnect LFO or Sequencer for pitch modulation");

    ImGui::Spacing();
    ImGui::Spacing();

    // === DAMPING ===
    ThemeText("Damping", theme.text.section_header);
    ImGui::Spacing();
    
    const bool dampingMod = isParamModulated(paramIdDampingMod);
    if (dampingMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Damping Mod (Channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))
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
    HelpMarker("String damping (decay time)\n0.0 = Long sustain\n1.0 = Short decay\nCV modulation: 0-1V maps to 0-1 damping");

    ImGui::Spacing();
    ImGui::Spacing();

    // === PICKUP POSITION ===
    ThemeText("Pickup Position", theme.text.section_header);
    ImGui::Spacing();
    
    const bool pickupMod = isParamModulated(paramIdPickupMod);
    if (pickupMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }
    
    // Inline pin for Pickup Mod (Channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))
            ImGui::SameLine();
    }
    
    if (pickupMod) ImGui::BeginDisabled();
    float pickupPos = pickupPosParam != nullptr ? getLiveParamValueFor(paramIdPickupMod, "pickupPos_live", pickupPosParam->load()) : 0.5f;
    if (ImGui::SliderFloat("##pickup", &pickupPos, 0.0f, 1.0f, "%.2f"))
    {
        if (!pickupMod)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPickupPos)))
                *p = pickupPos;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (!pickupMod) adjustParamOnWheel(ap.getParameter(paramIdPickupPos), "pickupPos", pickupPos);
    if (pickupMod) ImGui::EndDisabled();
    
    ImGui::SameLine();
    if (pickupMod)
    {
        ThemeText("Pickup Pos (CV)", theme.text.active);
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::Text("Pickup Position");
    }
    HelpMarker("Pickup/pluck position along string\n0.0 = Near bridge (bright)\n1.0 = Near nut (warm)\nCV modulation: 0-1V maps to 0-1 position");

    ImGui::Spacing();
    ImGui::Spacing();

    // === BRIGHTNESS ===
    ThemeText("Brightness", theme.text.section_header);
    ImGui::Spacing();
    
    float brightness = brightnessParam != nullptr ? brightnessParam->load() : 0.5f;
    if (ImGui::SliderFloat("##brightness", &brightness, 0.0f, 1.0f, "%.2f"))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdBrightness)))
            *p = brightness;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    adjustParamOnWheel(ap.getParameter(paramIdBrightness), "brightness", brightness);
    
    ImGui::SameLine();
    ImGui::Text("Brightness");
    HelpMarker("High-frequency emphasis\n0.0 = Dark\n1.0 = Bright");

    ImGui::Spacing();
    ImGui::Spacing();

    // === BODY SIZE ===
    ThemeText("Body Size", theme.text.section_header);
    ImGui::Spacing();
    
    float bodySize = bodySizeParam != nullptr ? bodySizeParam->load() : 0.5f;
    if (ImGui::SliderFloat("##bodysize", &bodySize, 0.0f, 1.0f, "%.2f"))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdBodySize)))
            *p = bodySize;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    adjustParamOnWheel(ap.getParameter(paramIdBodySize), "bodySize", bodySize);
    
    ImGui::SameLine();
    ImGui::Text("Body Size");
    HelpMarker("Resonance body size (low-frequency emphasis)\n0.0 = Small\n1.0 = Large");

    ImGui::Spacing();
    ImGui::Spacing();

    // === OUTPUT ===
    ThemeText("Output", theme.text.section_header);
    ImGui::Spacing();
    
    float outputLevel = lastOutputValues[0]->load();
    float absLevel = std::abs(outputLevel);
    
    ImVec4 meterColor;
    if (absLevel < 0.7f)
        meterColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    else if (absLevel < 0.9f)
        meterColor = ImVec4(0.9f, 0.7f, 0.0f, 1.0f);
    else
        meterColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
    
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, meterColor);
    ImGui::ProgressBar(absLevel, ImVec2(itemWidth, 0), "");
    ImGui::PopStyleColor();
    
    ImGui::SameLine(0, 5);
    ImGui::Text("%.3f", outputLevel);
    HelpMarker("Live output signal level\nConnect to VCA, Filter, or Audio Out\nUse Gate input to trigger notes");

    ImGui::Spacing();
    ImGui::Spacing();

    // === WAVEFORM VISUALIZATION ===
    ThemeText("Waveform", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushID(this); // Unique ID for this node's UI
    
    // Read visualization data (thread-safe) - BEFORE BeginChild
    float outputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        outputWaveform[i] = vizData.outputWaveform[i].load();
    }
    const float currentFreq = vizData.currentFrequency.load();
    const int currentInstrument = vizData.currentInstrumentType.load();
    const float gateLevel = vizData.gateLevel.load();

    // Waveform visualization in child window
    const auto& freqColors = theme.modules.frequency_graph;
    const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
    const float waveHeight = 140.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);
    
    if (ImGui::BeginChild("STKStringWaveform", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
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
        drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), gridColor, 1.0f);
        
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

        // Draw gate level indicator (horizontal line showing gate amount)
        if (gateLevel < 1.0f && gateLevel > 0.0f)
        {
            const ImU32 gateIndicatorColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);
            const float gateY = p0.y + graphSize.y - (gateLevel * graphSize.y * 0.3f);
            const float clampedGateY = juce::jlimit(p0.y + 2.0f, p1.y - 2.0f, gateY);
            drawList->AddLine(ImVec2(p0.x, clampedGateY), ImVec2(p1.x, clampedGateY), gateIndicatorColor, 1.5f);
            
            // Gate label using drawList
            const ImU32 textColor = gateIndicatorColor;
            drawList->AddText(ImVec2(p0.x + 4.0f, clampedGateY - 12.0f), textColor, "Gate");
        }
        
        drawList->PopClipRect();
        
        // Frequency and instrument info overlay
        const char* instrumentNames[] = { "Guitar", "Violin", "Cello", "Sitar", "Banjo" };
        const char* instrumentName = (currentInstrument >= 0 && currentInstrument < 5) ? instrumentNames[currentInstrument] : "Unknown";
        
        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%.1f Hz | %s", currentFreq, instrumentName);
        
        // Invisible button to block node dragging over the visualization
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##stkStringVizDrag", graphSize);
    }
    ImGui::EndChild(); // CRITICAL: Must be OUTSIDE the if block!
    
    ImGui::PopID(); // End visualization child window ID

    ImGui::PopItemWidth();
    ImGui::PopID(); // End module instance ID
}

void StkStringModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Channels 0, 3, 4 are now inline (Frequency, Damping, Pickup)
    // Only draw Gate (ch1), Velocity (ch2), and Output as parallel pins
    helpers.drawParallelPins("Gate", 1, nullptr, -1);
    helpers.drawParallelPins("Velocity", 2, nullptr, -1);
    helpers.drawAudioOutputPin("Output", 0);
}

juce::String StkStringModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Frequency Mod";
        case 1: return "Gate";
        case 2: return "Velocity";
        case 3: return "Damping Mod";
        case 4: return "Pickup Mod";
        default: return juce::String("In ") + juce::String(channel + 1);
    }
}

juce::String StkStringModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Out";
        default: return juce::String("Out ") + juce::String(channel + 1);
    }
}
#endif

