#include "VCOModuleProcessor.h"

VCOModuleProcessor::VCOModuleProcessor()
    : ModuleProcessor (BusesProperties()
                        .withInput("Inputs", juce::AudioChannelSet::discreteChannels(3), true) // ch0: Freq Mod, ch1: Wave Mod, ch2: Gate
                        .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts (*this, nullptr, "VCOParams", createParameterLayout())
{
    frequencyParam = apvts.getRawParameterValue(paramIdFrequency);
    waveformParam  = apvts.getRawParameterValue(paramIdWaveform);
    relativeFreqModParam = apvts.getRawParameterValue(paramIdRelativeFreqMod);
    portamentoParam = apvts.getRawParameterValue(paramIdPortamento);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));

    oscillator.initialise([](float x) { return std::sin(x); }, 128);
}

juce::AudioProcessorValueTreeState::ParameterLayout VCOModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdFrequency, "Frequency",
        juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.25f), 440.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdWaveform, "Waveform",
        juce::StringArray { "Sine", "Sawtooth", "Square" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        paramIdRelativeFreqMod, "Relative Freq Mod", true)); // Default: Relative mode
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdPortamento, "Portamento",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.001f, 0.5f), 0.0f)); // 0-2 seconds
    return { params.begin(), params.end() };
}

void VCOModuleProcessor::prepareToPlay(double sr, int samplesPerBlock)
{
    sampleRate = sr;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 1 };
    oscillator.prepare(spec);
    currentFrequency = frequencyParam != nullptr ? frequencyParam->load() : 440.0f;

#if defined(PRESET_CREATOR_UI)
    vizOutputBuffer.setSize(1, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.outputWaveform) v.store(0.0f);
    vizData.currentFrequency.store(440.0f);
    vizData.currentWaveform.store(0);
    vizData.gateLevel.store(0.0f);
    vizData.outputLevel.store(0.0f);
#endif
}

void VCOModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto outBus = getBusBuffer(buffer, false, 0);

    auto inBus = getBusBuffer(buffer, true, 0);
    const float* freqCV = (inBus.getNumChannels() > 0) ? inBus.getReadPointer(0) : nullptr;
    const float* waveCV = (inBus.getNumChannels() > 1) ? inBus.getReadPointer(1) : nullptr;
    const float* gateCV = (inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;

    const bool freqActive = isParamInputConnected(paramIdFrequency);
    const bool waveActive = isParamInputConnected(paramIdWaveformMod);
    const bool gateActive = isParamInputConnected("gate_mod");

#if defined(PRESET_CREATOR_UI)
    {
        static int dbgCounter = 0;
        if ((dbgCounter++ & 0x3F) == 0)
        {
            const float s0 = (freqCV && buffer.getNumSamples() > 0) ? freqCV[0] : 0.0f;
            const float s1 = (freqCV && buffer.getNumSamples() > 1) ? freqCV[1] : 0.0f;
            juce::Logger::writeToLog(
                juce::String("[VCO] inCh=") + juce::String(inBus.getNumChannels()) +
                " freqRMS=" + juce::String((inBus.getNumChannels()>0)?inBus.getRMSLevel(0,0,buffer.getNumSamples()):0.0f) +
                " s0=" + juce::String(s0) + " s1=" + juce::String(s1));
        }
    }
#endif

    const float baseFrequency = frequencyParam != nullptr ? frequencyParam->load() : 440.0f;
    const int baseWaveform = (int) (waveformParam != nullptr ? waveformParam->load() : 0.0f);
    const bool relativeMode = relativeFreqModParam != nullptr && relativeFreqModParam->load() > 0.5f;
    const float portamentoTime = portamentoParam != nullptr ? portamentoParam->load() : 0.0f;

    // DEBUG: Log relative mode status periodically
    static int vcoLogCounter = 0;
    if (freqActive && ++vcoLogCounter % 100 == 0)
    {
        juce::Logger::writeToLog("[VCO] Relative Freq Mod = " + juce::String(relativeMode ? "TRUE (around slider)" : "FALSE (absolute)"));
        juce::Logger::writeToLog("[VCO] Base Frequency = " + juce::String(baseFrequency, 1) + " Hz");
    }

    // Define smoothing factor for click-free gating
    constexpr float GATE_SMOOTHING_FACTOR = 0.002f;
    
    // Calculate portamento coefficient (time-based smoothing)
    // Using exponential smoothing: coefficient = 1 - exp(-1 / (time * sampleRate))
    float portamentoCoeff = 1.0f; // Instant (no smoothing)
    if (portamentoTime > 0.001f) // Only smooth if portamento time is meaningful
    {
        const double timeInSamples = portamentoTime * sampleRate;
        portamentoCoeff = static_cast<float>(1.0 - std::exp(-1.0 / timeInSamples));
    }

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        int waveform = baseWaveform;
        if (waveActive)
        {
            const float cvRaw = waveCV[i];
            const float cv01  = juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            waveform = (int) (cv01 * 2.99f);
        }

        float freq = baseFrequency;
        if (freqActive)
        {
            const float cvRaw = freqCV[i];
            // Normalize CV: prefer unipolar [0,1]; if outside, treat as bipolar [-1,1]
            const float cv01  = (cvRaw >= 0.0f && cvRaw <= 1.0f)
                                ? juce::jlimit(0.0f, 1.0f, cvRaw)
                                : juce::jlimit(0.0f, 1.0f, (cvRaw + 1.0f) * 0.5f);
            
            // Check relative mode setting
            const bool relativeModeNow = relativeFreqModParam != nullptr && relativeFreqModParam->load() > 0.5f;
            
            if (relativeModeNow)
            {
                // RELATIVE MODE: CV modulates around base frequency (±4 octaves from slider position)
                // cv01=0.5 means no change, cv01=0 means -4 octaves, cv01=1 means +4 octaves
                const float octaveOffset = (cv01 - 0.5f) * 8.0f; // ±4 octaves
                freq = baseFrequency * std::pow(2.0f, octaveOffset);
                
                // DEBUG: Log first sample calculation
                if (i == 0 && vcoLogCounter % 100 == 0)
                {
                    juce::Logger::writeToLog("[VCO Freq] RELATIVE mode: CV=" + juce::String(cv01, 3) + 
                                           ", baseFreq=" + juce::String(baseFrequency, 1) + 
                                           " Hz, octaveOffset=" + juce::String(octaveOffset, 2) +
                                           ", finalFreq=" + juce::String(freq, 1) + " Hz");
                }
            }
            else
            {
                // ABSOLUTE MODE: CV directly maps to full frequency range (20Hz - 20kHz)
                constexpr float fMin = 20.0f;
                constexpr float fMax = 20000.0f;
                const float spanOct = std::log2(fMax / fMin);
                freq = fMin * std::pow(2.0f, cv01 * spanOct);
                
                // DEBUG: Log first sample calculation
                if (i == 0 && vcoLogCounter % 100 == 0)
                {
                    juce::Logger::writeToLog("[VCO Freq] ABSOLUTE mode: CV=" + juce::String(cv01, 3) + 
                                           ", finalFreq=" + juce::String(freq, 1) + " Hz (ignores slider)");
                }
            }
        }
        freq = juce::jlimit(20.0f, 20000.0f, freq);
        
        // Apply portamento/glide smoothing to frequency
        if (portamentoTime > 0.001f)
        {
            currentFrequency += (freq - currentFrequency) * portamentoCoeff;
        }
        else
        {
            currentFrequency = freq; // Instant, no glide
        }

        if (currentWaveform != waveform)
        {
            if (waveform == 0)      oscillator.initialise([](float x){ return std::sin(x); }, 128);
            else if (waveform == 1) oscillator.initialise([](float x){ return (x / juce::MathConstants<float>::pi); }, 128);
            else                    oscillator.initialise([](float x){ return x < 0.0f ? -1.0f : 1.0f; }, 128);
            currentWaveform = waveform;
        }

        oscillator.setFrequency(currentFrequency, false);
        const float s = oscillator.processSample(0.0f);
        
        // Apply gate with click-free smoothing
        // CRITICAL FIX: If transport is stopped, force gate to 0 (silence VCO)
        // Otherwise, use gate input if connected, or default to 1.0 if no gate input
        float targetGate;
        if (!m_currentTransport.isPlaying)
        {
            // Transport is stopped - silence the VCO
            targetGate = 0.0f;
        }
        else
        {
            // Transport is playing - use gate input if connected, otherwise default to 1.0
            targetGate = gateActive ? gateCV[i] : 1.0f;
        }
        // Treat near-zero magnitudes as zero to avoid flutter from denormals or noise
        if (std::abs(targetGate) < 1.0e-4f) targetGate = 0.0f;
        if (targetGate > 1.0f) targetGate = 1.0f;
        smoothedGate += (targetGate - smoothedGate) * GATE_SMOOTHING_FACTOR;
        const float finalSample = s * smoothedGate;
        
        outBus.setSample(0, i, finalSample);

#if defined(PRESET_CREATOR_UI)
        // Capture output audio for visualization
        if (vizOutputBuffer.getNumSamples() > 0)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizOutputBuffer.setSample(0, writeIdx, finalSample);
        }

        // Track current state (use last sample for live display)
        if (i == buffer.getNumSamples() - 1)
        {
            vizData.currentFrequency.store(currentFrequency);
            vizData.currentWaveform.store(currentWaveform);
            vizData.gateLevel.store(smoothedGate);
            vizData.outputLevel.store(finalSample);
        }
#endif

        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("frequency_live", freq);
            setLiveParamValue("waveform_live", (float) waveform);
        }
    }
    
    // Update inspector value for visualization (peak magnitude)
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

bool VCOModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All inputs are on the same bus
    if (paramId == paramIdFrequency)   { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdWaveformMod) { outChannelIndexInBus = 1; return true; }
    if (paramId == "gate_mod")         { outChannelIndexInBus = 2; return true; }
    return false;
}

void VCOModuleProcessor::setTimingInfo(const TransportState& state)
{
    m_currentTransport = state;
}

void VCOModuleProcessor::forceStop()
{
    // Force gate to zero to silence the VCO
    smoothedGate = 0.0f;
}


