#include "HarmonicShaperModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

bool HarmonicShaperModuleProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // We require exactly 1 input bus and 1 output bus
    if (layouts.inputBuses.size() != 1 || layouts.outputBuses.size() != 1)
        return false;

    // Input MUST be exactly 8 discrete channels (Audio L/R + 6 CVs)
    // This strict check prevents the host from aliasing channels
    if (layouts.inputBuses[0] != juce::AudioChannelSet::discreteChannels(8))
        return false;

    // Output MUST be Stereo
    if (layouts.outputBuses[0] != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

juce::AudioProcessorValueTreeState::ParameterLayout HarmonicShaperModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // --- Global Parameters ---
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMasterFreq, "Master Frequency",
        juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.25f), 440.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMasterDrive, "Master Drive", 0.0f, 1.0f, 0.2f)); // Lower default
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdOutputGain, "Output Gain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 0.5f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdCharacter, "Character",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 0.5f), 0.3f)); // Controls modulation intensity
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdSmoothness, "Smoothness",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 0.5f), 0.5f)); // Carrier smoothing

    // --- Per-Oscillator Parameters ---
    for (int i = 0; i < NUM_OSCILLATORS; ++i)
    {
        auto idx = juce::String(i + 1);
        params.push_back(std::make_unique<juce::AudioParameterFloat>("ratio_" + idx, "Ratio " + idx,
            juce::NormalisableRange<float>(0.125f, 16.0f, 0.001f, 0.25f), (float)(i + 1)));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("detune_" + idx, "Detune " + idx, -100.0f, 100.0f, 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterChoice>("waveform_" + idx, "Waveform " + idx,
            juce::StringArray{ "Sine", "Saw", "Square", "Triangle" }, 0));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("drive_" + idx, "Drive " + idx, 0.0f, 1.0f, 0.5f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>("level_" + idx, "Level " + idx, 0.0f, 1.0f, i == 0 ? 1.0f : 0.0f));
    }
    
    // Relative modulation parameters
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeFreqMod", "Relative Freq Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeDriveMod", "Relative Drive Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeGainMod", "Relative Gain Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeMixMod", "Relative Mix Mod", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeCharacterMod", "Relative Character Mod", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeSmoothnessMod", "Relative Smoothness Mod", false));

    return { params.begin(), params.end() };
}

HarmonicShaperModuleProcessor::HarmonicShaperModuleProcessor()
    : ModuleProcessor(BusesProperties()
        .withInput("Inputs", juce::AudioChannelSet::discreteChannels(8), true) // Audio L/R, Freq Mod, Drive Mod, Gain Mod, Mix Mod, Character Mod, Smoothness Mod
        .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "HarmonicShaperParams", createParameterLayout())
{
    // Cache global parameter pointers
    masterFreqParam = apvts.getRawParameterValue(paramIdMasterFreq);
    masterDriveParam = apvts.getRawParameterValue(paramIdMasterDrive);
    outputGainParam = apvts.getRawParameterValue(paramIdOutputGain);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    characterParam = apvts.getRawParameterValue(paramIdCharacter);
    smoothnessParam = apvts.getRawParameterValue(paramIdSmoothness);

    // Initialize oscillators and cache per-oscillator parameter pointers
    for (int i = 0; i < NUM_OSCILLATORS; ++i)
    {
        oscillators[i].initialise([](float x) { return std::sin(x); }, 128);
        currentWaveforms[i] = -1; // Force initial waveform setup

        auto idx = juce::String(i + 1);
        ratioParams[i] = apvts.getRawParameterValue("ratio_" + idx);
        detuneParams[i] = apvts.getRawParameterValue("detune_" + idx);
        waveformParams[i] = apvts.getRawParameterValue("waveform_" + idx);
        driveParams[i] = apvts.getRawParameterValue("drive_" + idx);
        levelParams[i] = apvts.getRawParameterValue("level_" + idx);
    }
    
    relativeFreqModParam = apvts.getRawParameterValue("relativeFreqMod");
    relativeDriveModParam = apvts.getRawParameterValue("relativeDriveMod");
    relativeGainModParam = apvts.getRawParameterValue("relativeGainMod");
    relativeMixModParam = apvts.getRawParameterValue("relativeMixMod");
    relativeCharacterModParam = apvts.getRawParameterValue("relativeCharacterMod");
    relativeSmoothnessModParam = apvts.getRawParameterValue("relativeSmoothnessMod");

    // Initialize visualization data
    for (auto& level : vizData.oscillatorLevels) level.store(0.0f);
    for (auto& freq : vizData.oscillatorFrequencies) freq.store(0.0f);
    for (auto& wave : vizData.oscillatorWaveforms) wave.store(0);
    for (auto& wf : vizData.combinedWaveform) wf.store(0.0f);
}

void HarmonicShaperModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 1 };
    for (auto& osc : oscillators)
    {
        osc.prepare(spec);
    }
    smoothedMasterFreq.reset(sampleRate, 0.02);
    smoothedMasterDrive.reset(sampleRate, 0.02);
    smoothedCarrier.reset(sampleRate, 0.01); // Fast smoothing for carrier
}

void HarmonicShaperModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const int numSamples = buffer.getNumSamples();

    // Get modulation CVs
    // IMPORTANT: Acquire input pointers BEFORE any output operations (aliasing-safe)
    const bool isFreqMod = isParamInputConnected(paramIdMasterFreqMod);
    const bool isDriveMod = isParamInputConnected(paramIdMasterDriveMod);
    const bool isGainMod = isParamInputConnected(paramIdOutputGainMod);
    const bool isMixMod = isParamInputConnected(paramIdMixMod);
    const bool isCharacterMod = isParamInputConnected(paramIdCharacterMod);
    const bool isSmoothnessMod = isParamInputConnected(paramIdSmoothnessMod);

    const float* freqCVPtr = (isFreqMod && inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;
    const float* driveCVPtr = (isDriveMod && inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* gainCVPtr = (isGainMod && inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;
    const float* mixCVPtr = (isMixMod && inBus.getNumChannels() > 5) ? inBus.getReadPointer(5) : nullptr;
    const float* characterCVPtr = (isCharacterMod && inBus.getNumChannels() > 6) ? inBus.getReadPointer(6) : nullptr;
    const float* smoothnessCVPtr = (isSmoothnessMod && inBus.getNumChannels() > 7) ? inBus.getReadPointer(7) : nullptr;

    // Debug: Always log first call to see what's happening
    {
        static bool firstCall = true;
        if (firstCall) {
            firstCall = false;
            auto ptrToHex = [](const void* ptr) -> juce::String {
                if (!ptr) return "NULL";
                char buf[32];
                snprintf(buf, sizeof(buf), "%p", ptr);
                return juce::String(buf);
            };
            
            juce::Logger::writeToLog("[HarmonicShaper][FIRST CALL] inBusChannels=" + juce::String(inBus.getNumChannels()));
            
            juce::String ptrMsg = "[HarmonicShaper][POINTERS] ";
            ptrMsg += "freqPtr=" + ptrToHex(freqCVPtr) + " ";
            ptrMsg += "drivePtr=" + ptrToHex(driveCVPtr) + " ";
            ptrMsg += "gainPtr=" + ptrToHex(gainCVPtr) + " ";
            ptrMsg += "mixPtr=" + ptrToHex(mixCVPtr) + " ";
            ptrMsg += "charPtr=" + ptrToHex(characterCVPtr) + " ";
            ptrMsg += "smoothPtr=" + ptrToHex(smoothnessCVPtr);
            juce::Logger::writeToLog(ptrMsg);
            
            // Check raw channel pointers and values
            juce::String rawMsg = "[HarmonicShaper][RAW CH] ";
            for (int ch = 2; ch < inBus.getNumChannels() && ch < 8; ++ch) {
                const float* rawPtr = inBus.getReadPointer(ch);
                rawMsg += "ch" + juce::String(ch) + ":ptr=" + ptrToHex(rawPtr);
                if (rawPtr) rawMsg += ":val=" + juce::String(rawPtr[0], 3);
                rawMsg += " ";
            }
            juce::Logger::writeToLog(rawMsg);
        }
    }

    // EXTRA SAFETY: Copy CV channels we use into local buffers BEFORE any output writes,
    // so later writes cannot affect reads even if buffers alias (see DEBUG_INPUT_IMPORTANT.md).
    juce::HeapBlock<float> freqCV, driveCV, gainCV, mixCV, characterCV, smoothnessCV;
    if (freqCVPtr)      { freqCV.malloc(numSamples);      std::memcpy(freqCV.get(),      freqCVPtr,      sizeof(float) * (size_t)numSamples); }
    if (driveCVPtr)     { driveCV.malloc(numSamples);     std::memcpy(driveCV.get(),     driveCVPtr,     sizeof(float) * (size_t)numSamples); }
    if (gainCVPtr)      { gainCV.malloc(numSamples);       std::memcpy(gainCV.get(),      gainCVPtr,      sizeof(float) * (size_t)numSamples); }
    if (mixCVPtr)       { mixCV.malloc(numSamples);        std::memcpy(mixCV.get(),       mixCVPtr,       sizeof(float) * (size_t)numSamples); }
    if (characterCVPtr) { characterCV.malloc(numSamples);  std::memcpy(characterCV.get(), characterCVPtr,  sizeof(float) * (size_t)numSamples); }
    if (smoothnessCVPtr){ smoothnessCV.malloc(numSamples); std::memcpy(smoothnessCV.get(), smoothnessCVPtr, sizeof(float) * (size_t)numSamples); }

    // Debug: Log copied values to verify they're different
    {
        static bool firstCopy = true;
        if (firstCopy) {
            firstCopy = false;
            juce::String copyMsg = "[HarmonicShaper][COPIED VALUES] ";
            if (freqCV.get())      copyMsg += "freq=" + juce::String(freqCV[0], 3) + " ";
            if (driveCV.get())     copyMsg += "drive=" + juce::String(driveCV[0], 3) + " ";
            if (gainCV.get())      copyMsg += "gain=" + juce::String(gainCV[0], 3) + " ";
            if (mixCV.get())       copyMsg += "mix=" + juce::String(mixCV[0], 3) + " ";
            if (characterCV.get()) copyMsg += "char=" + juce::String(characterCV[0], 3) + " ";
            if (smoothnessCV.get()) copyMsg += "smooth=" + juce::String(smoothnessCV[0], 3) + " ";
            juce::Logger::writeToLog(copyMsg);
        }
    }

    // ✅ NOW safe to get output write pointers (CV data is safely copied)
    auto* outL = outBus.getWritePointer(0);
    auto* outR = outBus.getNumChannels() > 1 ? outBus.getWritePointer(1) : outL;
    
    const float* inL = inBus.getReadPointer(0);
    const float* inR = inBus.getNumChannels() > 1 ? inBus.getReadPointer(1) : inL;

    const float baseFrequency = masterFreqParam->load();
    const float baseMasterDrive = masterDriveParam->load();
    const float baseOutputGain = outputGainParam->load();
    const float baseMix = mixParam->load();
    const float baseCharacter = characterParam->load();
    const float baseSmoothness = smoothnessParam->load();
    
    const bool relativeFreqMode = relativeFreqModParam && relativeFreqModParam->load() > 0.5f;
    const bool relativeDriveMode = relativeDriveModParam && relativeDriveModParam->load() > 0.5f;
    const bool relativeGainMode = relativeGainModParam && relativeGainModParam->load() > 0.5f;
    const bool relativeMixMode = relativeMixModParam && relativeMixModParam->load() > 0.5f;
    const bool relativeCharacterMode = relativeCharacterModParam && relativeCharacterModParam->load() > 0.5f;
    const bool relativeSmoothnessMode = relativeSmoothnessModParam && relativeSmoothnessModParam->load() > 0.5f;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        // === 1. Calculate Global Modulated Parameters (per-sample) ===
        float currentMasterFreq = baseFrequency;
        if (freqCV.get()) {
            const float cv = juce::jlimit(0.0f, 1.0f, freqCV[i]);
            if (relativeFreqMode) {
                // RELATIVE: ±4 octaves around base frequency
                const float octaveOffset = (cv - 0.5f) * 8.0f;
                currentMasterFreq = baseFrequency * std::pow(2.0f, octaveOffset);
            } else {
                // ABSOLUTE: CV directly sets frequency (20-20000 Hz)
                const float spanOct = std::log2(20000.0f / 20.0f);
                currentMasterFreq = 20.0f * std::pow(2.0f, cv * spanOct);
            }
            currentMasterFreq = juce::jlimit(20.0f, 20000.0f, currentMasterFreq);
        }
        smoothedMasterFreq.setTargetValue(currentMasterFreq);

        float currentMasterDrive = baseMasterDrive;
        if (driveCV.get()) {
            const float cv = juce::jlimit(0.0f, 1.0f, driveCV[i]);
            if (relativeDriveMode) {
                // RELATIVE: ±0.5 offset
                const float offset = (cv - 0.5f) * 1.0f;
                currentMasterDrive = baseMasterDrive + offset;
            } else {
                // ABSOLUTE: CV directly sets drive
                currentMasterDrive = cv;
            }
            currentMasterDrive = juce::jlimit(0.0f, 1.0f, currentMasterDrive);
        }
        smoothedMasterDrive.setTargetValue(currentMasterDrive);
        
        // === 2. Generate and Sum the 8 Oscillators ===
        float carrierSample = 0.0f;
        const float smoothedFreq = smoothedMasterFreq.getNextValue();
        const float smoothedDrive = smoothedMasterDrive.getNextValue();

        for (int osc = 0; osc < NUM_OSCILLATORS; ++osc)
        {
            const float level = levelParams[osc]->load();
            if (level <= 0.001f) continue; // Skip silent oscillators

            const int waveform = (int)waveformParams[osc]->load();
            if (currentWaveforms[osc] != waveform)
            {
                if (waveform == 0)      oscillators[osc].initialise([](float x) { return std::sin(x); });
                else if (waveform == 1) oscillators[osc].initialise([](float x) { return x / juce::MathConstants<float>::pi; });
                else if (waveform == 2) oscillators[osc].initialise([](float x) { return x < 0.0f ? -1.0f : 1.0f; });
                else                    oscillators[osc].initialise([](float x) { return 2.0f / juce::MathConstants<float>::pi * std::asin(std::sin(x)); });
                currentWaveforms[osc] = waveform;
            }

            const float frequency = smoothedFreq * ratioParams[osc]->load() + detuneParams[osc]->load();
            oscillators[osc].setFrequency(juce::jlimit(1.0f, (float)getSampleRate() * 0.5f, frequency), true);

            const float oscSample = oscillators[osc].processSample(0.0f);
            const float drive = driveParams[osc]->load() * smoothedDrive;
            // Gentler drive curve: reduce max amplification from 10x to 4x, use smoother curve
            const float driveAmount = 1.0f + drive * 3.0f; // Max 4x instead of 10x
            // Use a gentler saturation curve
            const float driven = oscSample * driveAmount;
            const float shapedSample = driven / (1.0f + std::abs(driven)); // Softer saturation than tanh
            carrierSample += shapedSample * level;
        }

        // Gentle normalization instead of hard clipping
        const float carrierNorm = carrierSample / (1.0f + std::abs(carrierSample) * 0.5f);
        
        // === 2.5. Calculate Modulated Parameters (per-sample) - Smoothness first as it's needed for carrier smoothing ===
        float currentSmoothness = baseSmoothness;
        if (smoothnessCV.get()) {
            const float cv = juce::jlimit(0.0f, 1.0f, smoothnessCV[i]);
            if (relativeSmoothnessMode) {
                // RELATIVE: ±0.5 around base smoothness
                const float offset = (cv - 0.5f) * 1.0f;
                currentSmoothness = baseSmoothness + offset;
            } else {
                // ABSOLUTE: CV directly sets smoothness (0-1)
                currentSmoothness = cv;
            }
            currentSmoothness = juce::jlimit(0.0f, 1.0f, currentSmoothness);
        }
        
        // Apply smoothing to carrier to reduce harshness
        const float smoothness = currentSmoothness;
        // Update smoothing time only when smoothness changes (not every sample)
        static float lastSmoothness = -1.0f;
        if (std::abs(smoothness - lastSmoothness) > 0.01f)
        {
            const float smoothingTime = 0.001f + smoothness * 0.01f; // 1-11ms based on smoothness
            smoothedCarrier.reset(getSampleRate(), smoothingTime);
            lastSmoothness = smoothness;
        }
        smoothedCarrier.setTargetValue(carrierNorm);
        const float smoothedCarrierValue = smoothedCarrier.getNextValue();
        
        // Calculate other modulated parameters
        float currentOutputGain = baseOutputGain;
        if (gainCV.get()) {
            const float cv = juce::jlimit(0.0f, 1.0f, gainCV[i]);
            if (relativeGainMode) {
                // RELATIVE: ±0.5 around base gain
                const float offset = (cv - 0.5f) * 1.0f;
                currentOutputGain = baseOutputGain + offset;
            } else {
                // ABSOLUTE: CV directly sets gain (0-1)
                currentOutputGain = cv;
            }
            currentOutputGain = juce::jlimit(0.0f, 1.0f, currentOutputGain);
        }

        float currentMix = baseMix;
        if (mixCV.get()) {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
            if (relativeMixMode) {
                // RELATIVE: ±0.5 around base mix
                const float offset = (cv - 0.5f) * 1.0f;
                currentMix = baseMix + offset;
            } else {
                // ABSOLUTE: CV directly sets mix (0-1)
                currentMix = cv;
            }
            currentMix = juce::jlimit(0.0f, 1.0f, currentMix);
        }

        float currentCharacter = baseCharacter;
        if (characterCV.get()) {
            const float cv = juce::jlimit(0.0f, 1.0f, characterCV[i]);
            if (relativeCharacterMode) {
                // RELATIVE: ±0.5 around base character
                const float offset = (cv - 0.5f) * 1.0f;
                currentCharacter = baseCharacter + offset;
            } else {
                // ABSOLUTE: CV directly sets character (0-1)
                currentCharacter = cv;
            }
            currentCharacter = juce::jlimit(0.0f, 1.0f, currentCharacter);
        }

        // === 3. Modulate Input with Carrier and Apply Gain ===
        const float mix = currentMix;
        const float character = currentCharacter;
        
        // Gentler modulation: blend between ring mod and amplitude mod
        // Character=0: pure amplitude modulation (gentler)
        // Character=1: pure ring modulation (more aggressive)
        const float carrierForMod = 0.5f + smoothedCarrierValue * 0.5f; // Center around 0.5 for AM
        const float ringMod = inL[i] * smoothedCarrierValue;
        const float ampMod = inL[i] * carrierForMod;
        const float wetL = (ringMod * character + ampMod * (1.0f - character)) * currentOutputGain;
        
        const float carrierForModR = 0.5f + smoothedCarrierValue * 0.5f;
        const float ringModR = inR[i] * smoothedCarrierValue;
        const float ampModR = inR[i] * carrierForModR;
        const float wetR = (ringModR * character + ampModR * (1.0f - character)) * currentOutputGain;
        const float dryL = inL[i];
        const float dryR = inR[i];
        
        // Apply dry/wet mix
        outL[i] = dryL * (1.0f - mix) + wetL * mix;
        outR[i] = dryR * (1.0f - mix) + wetR * mix;

        // === 4. Update Visualization Data (Throttled - every 64 samples) ===
        if ((i & 63) == 0) {
            setLiveParamValue("masterFrequency_live", smoothedFreq);
            setLiveParamValue("masterDrive_live", smoothedDrive);
            setLiveParamValue("outputGain_live", currentOutputGain);
            setLiveParamValue("mix_live", currentMix);
            setLiveParamValue("character_live", currentCharacter);
            setLiveParamValue("smoothness_live", currentSmoothness);
            
            // Update oscillator visualization data
            for (int osc = 0; osc < NUM_OSCILLATORS; ++osc)
            {
                const float level = levelParams[osc]->load();
                vizData.oscillatorLevels[osc].store(level);
                
                const float frequency = smoothedFreq * ratioParams[osc]->load() + detuneParams[osc]->load();
                vizData.oscillatorFrequencies[osc].store(frequency);
                
                const int waveform = (int)waveformParams[osc]->load();
                vizData.oscillatorWaveforms[osc].store(waveform);
            }
            
            // Update master parameters
            vizData.masterFrequency.store(smoothedFreq);
            vizData.masterDrive.store(smoothedDrive);
            
            // Update combined waveform preview (generate one cycle)
            if (i == 0) // Only update once per block
            {
                for (int p = 0; p < VizData::waveformPoints; ++p)
                {
                    float combined = 0.0f;
                    const float phase = (float)p / (float)VizData::waveformPoints * 2.0f * juce::MathConstants<float>::pi;
                    
                    for (int osc = 0; osc < NUM_OSCILLATORS; ++osc)
                    {
                        const float level = levelParams[osc]->load();
                        if (level <= 0.001f) continue;
                        
                        const float ratio = ratioParams[osc]->load();
                        const float detune = detuneParams[osc]->load();
                        const float oscFreq = smoothedFreq * ratio + detune;
                        const float oscPhase = phase * (oscFreq / smoothedFreq);
                        
                        const int waveform = (int)waveformParams[osc]->load();
                        float oscSample = 0.0f;
                        if (waveform == 0)      oscSample = std::sin(oscPhase);
                        else if (waveform == 1) oscSample = oscPhase / juce::MathConstants<float>::pi;
                        else if (waveform == 2) oscSample = (oscPhase < 0.0f) ? -1.0f : 1.0f;
                        else                    oscSample = 2.0f / juce::MathConstants<float>::pi * std::asin(std::sin(oscPhase));
                        
                        const float drive = driveParams[osc]->load() * smoothedDrive;
                        const float shaped = std::tanh(oscSample * (1.0f + drive * 9.0f));
                        combined += shaped * level;
                    }
                    combined = std::tanh(combined);
                    vizData.combinedWaveform[p].store(combined);
                }
            }
        }
    }
}

#if defined(PRESET_CREATOR_UI)

void HarmonicShaperModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    
    auto HelpMarker = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f); ImGui::TextUnformatted(desc); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    };

    // === HARMONIC SPECTRUM VISUALIZATION ===
    ImGui::Spacing();
    ThemeText("Harmonic Spectrum", theme.text.section_header);
    ImGui::Spacing();

    auto* drawList = ImGui::GetWindowDrawList();
    const ImVec2 specOrigin = ImGui::GetCursorScreenPos();
    const float specWidth = itemWidth;
    const float specHeight = 80.0f;
    const ImVec2 specRectMax = ImVec2(specOrigin.x + specWidth, specOrigin.y + specHeight);
    
    // Get theme colors (with fallbacks like GranulatorModuleProcessor)
    auto& themeMgr = ThemeManager::getInstance();
    auto resolveColor = [&](ImU32 primary, ImU32 secondary, ImU32 tertiary) -> ImU32 {
        if (primary != 0) return primary;
        if (secondary != 0) return secondary;
        return tertiary;
    };
    
    const ImU32 canvasBg = themeMgr.getCanvasBackground();
    const ImVec4 childBgVec4 = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
    const ImU32 childBg = ImGui::ColorConvertFloat4ToU32(childBgVec4);
    const ImU32 bgColor = resolveColor(theme.modules.scope_plot_bg, canvasBg, childBg);
    
    const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
    const ImU32 barColor = ImGui::ColorConvertFloat4ToU32(frequencyColorVec4);
    const ImVec4 accentVec4 = theme.accent;
    const ImU32 barColorActive = ImGui::ColorConvertFloat4ToU32(accentVec4);
    const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(theme.text.section_header);
    
    drawList->AddRectFilled(specOrigin, specRectMax, bgColor, 4.0f);
    ImGui::PushClipRect(specOrigin, specRectMax, true);
    
    // Read visualization data
    float levels[8];
    int waveforms[8];
    for (int i = 0; i < 8; ++i)
    {
        levels[i] = vizData.oscillatorLevels[i].load();
        waveforms[i] = vizData.oscillatorWaveforms[i].load();
    }
    
    // Draw 8 harmonic bars (clickable)
    const float barWidth = specWidth / 8.0f - 4.0f;
    const float barSpacing = 4.0f;
    const float maxBarHeight = specHeight - 20.0f; // Leave room for labels
    
    for (int i = 0; i < 8; ++i)
    {
        const float barX = specOrigin.x + i * (barWidth + barSpacing) + barSpacing;
        const float barHeight = levels[i] * maxBarHeight;
        const float barY = specOrigin.y + maxBarHeight - barHeight;
        
        // Color based on waveform
        ImU32 barCol = barColor;
        if (levels[i] > 0.001f)
        {
            switch (waveforms[i])
            {
                case 0: barCol = IM_COL32(100, 200, 255, 255); break; // Sine - blue
                case 1: barCol = IM_COL32(255, 150, 100, 255); break; // Saw - orange
                case 2: barCol = IM_COL32(255, 100, 150, 255); break; // Square - pink
                case 3: barCol = IM_COL32(150, 255, 150, 255); break; // Triangle - green
            }
        }
        else
        {
            barCol = IM_COL32(60, 60, 60, 255); // Dimmed
        }
        
        // Draw bar
        drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barWidth, specOrigin.y + maxBarHeight), barCol, 2.0f);
        
        // Draw label
        const char* labels[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
        const ImVec2 labelPos(barX + barWidth * 0.5f, specOrigin.y + maxBarHeight + 2.0f);
        drawList->AddText(labelPos, textColor, labels[i]);
        
        // Make bar clickable to adjust level, Ctrl+click to change waveform
        ImGui::SetCursorScreenPos(ImVec2(barX, specOrigin.y));
        ImGui::InvisibleButton(("##bar" + juce::String(i)).toRawUTF8(), ImVec2(barWidth, specHeight));
        
        // Ctrl+Click to cycle waveform
        if (ImGui::IsItemClicked(0) && ImGui::GetIO().KeyCtrl)
        {
            auto idx = juce::String(i + 1);
            int currentWave = (int)waveformParams[i]->load();
            int newWave = (currentWave + 1) % 4;
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("waveform_" + idx)))
                *p = newWave;
            onModificationEnded();
        }
        // Drag to adjust level
        else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
        {
            const float dragDelta = -ImGui::GetIO().MouseDelta.y / maxBarHeight;
            auto idx = juce::String(i + 1);
            float newLevel = juce::jlimit(0.0f, 1.0f, levels[i] + dragDelta);
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("level_" + idx)))
                *p = newLevel;
            onModificationEnded();
        }
    }
    
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(specOrigin.x, specRectMax.y));
    ImGui::Dummy(ImVec2(specWidth, 0));
    ImGui::Spacing();

    // Hint about Ctrl+click
    ImGui::TextDisabled("Drag bars to adjust level | Ctrl+Click to change waveform");
    ImGui::Spacing();

    // === MASTER CONTROLS (COMPACT) ===
    ThemeText("Master Controls", theme.text.section_header);
    ImGui::Spacing();

    const bool freqIsMod = isParamModulated(paramIdMasterFreqMod);
    float freq = freqIsMod ? getLiveParamValueFor(paramIdMasterFreqMod, "masterFrequency_live", masterFreqParam->load()) : masterFreqParam->load();

    const bool driveIsMod = isParamModulated(paramIdMasterDriveMod);
    float drive = driveIsMod ? getLiveParamValueFor(paramIdMasterDriveMod, "masterDrive_live", masterDriveParam->load()) : masterDriveParam->load();

    const bool gainIsMod = isParamModulated(paramIdOutputGainMod);
    float gain = gainIsMod ? getLiveParamValueFor(paramIdOutputGainMod, "outputGain_live", outputGainParam->load()) : outputGainParam->load();

    // Master Frequency - on its own line with full width
    ImGui::PushItemWidth(itemWidth);
    // Draw inline pin for Freq Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))  // Channel 2 = Freq Mod
            ImGui::SameLine();
    }
    if (freqIsMod) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Freq", &freq, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic)) {
        if (!freqIsMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdMasterFreq))) *p = freq;
    }
    if (!freqIsMod) adjustParamOnWheel(ap.getParameter(paramIdMasterFreq), "masterFreqHz", freq);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (freqIsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }

    // Master Drive - on its own line
    // Draw inline pin for Drive Mod (channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))  // Channel 3 = Drive Mod
            ImGui::SameLine();
    }
    if (driveIsMod) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Drive", &drive, 0.0f, 1.0f, "%.2f")) {
        if (!driveIsMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdMasterDrive))) *p = drive;
    }
    if (!driveIsMod) adjustParamOnWheel(ap.getParameter(paramIdMasterDrive), "masterDrive", drive);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (driveIsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }

    // Output Gain - on its own line
    // Draw inline pin for Gain Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))  // Channel 4 = Gain Mod
            ImGui::SameLine();
    }
    if (gainIsMod) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Gain", &gain, 0.0f, 1.0f, "%.2f")) {
        if (!gainIsMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdOutputGain))) *p = gain;
    }
    if (!gainIsMod) adjustParamOnWheel(ap.getParameter(paramIdOutputGain), "outputGain", gain);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (gainIsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::PopItemWidth();

    // Mix (Dry/Wet)
    const bool mixIsMod = isParamModulated(paramIdMixMod);
    float mix = mixIsMod ? getLiveParamValueFor(paramIdMixMod, "mix_live", mixParam->load()) : mixParam->load();
    ImGui::PushItemWidth(itemWidth);
    if (mixIsMod) ImGui::BeginDisabled();
    // Draw inline pin for Mix Mod (channel 5)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(5))  // Channel 5 = Mix Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) {
        if (!mixIsMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdMix))) *p = mix;
    }
    if (!mixIsMod) adjustParamOnWheel(ap.getParameter(paramIdMix), "mix", mix);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (mixIsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::PopItemWidth();

    // Character (modulation intensity)
    const bool charIsMod = isParamModulated(paramIdCharacterMod);
    float character = charIsMod ? getLiveParamValueFor(paramIdCharacterMod, "character_live", characterParam->load()) : characterParam->load();
    ImGui::PushItemWidth(itemWidth);
    if (charIsMod) ImGui::BeginDisabled();
    // Draw inline pin for Character Mod (channel 6)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(6))  // Channel 6 = Character Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Character", &character, 0.0f, 1.0f, "%.2f")) {
        if (!charIsMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdCharacter))) *p = character;
    }
    if (!charIsMod) adjustParamOnWheel(ap.getParameter(paramIdCharacter), "character", character);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (charIsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0.0 = Gentle (AM), 1.0 = Aggressive (Ring Mod)");
    ImGui::PopItemWidth();

    // Smoothness
    const bool smoothIsMod = isParamModulated(paramIdSmoothnessMod);
    float smoothness = smoothIsMod ? getLiveParamValueFor(paramIdSmoothnessMod, "smoothness_live", smoothnessParam->load()) : smoothnessParam->load();
    ImGui::PushItemWidth(itemWidth);
    if (smoothIsMod) ImGui::BeginDisabled();
    // Draw inline pin for Smoothness Mod (channel 7)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(7))  // Channel 7 = Smoothness Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Smoothness", &smoothness, 0.0f, 1.0f, "%.2f")) {
        if (!smoothIsMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdSmoothness))) *p = smoothness;
    }
    if (!smoothIsMod) adjustParamOnWheel(ap.getParameter(paramIdSmoothness), "smoothness", smoothness);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (smoothIsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooths carrier transitions to reduce harshness");
    ImGui::PopItemWidth();

    // Relative modulation checkboxes
    ImGui::Spacing();
    ImGui::Text("CV Modulation Modes:");
    ImGui::Spacing();
    
    bool relativeFreqMod = relativeFreqModParam != nullptr && relativeFreqModParam->load() > 0.5f;
    if (ImGui::Checkbox("Rel Freq", &relativeFreqMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeFreqMod")))
            *p = relativeFreqMod;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: CV modulates around slider (±4 octaves)\nOFF: CV directly sets freq (20-20000 Hz)");
    
    ImGui::SameLine();
    bool relativeDriveMod = relativeDriveModParam != nullptr && relativeDriveModParam->load() > 0.5f;
    if (ImGui::Checkbox("Rel Drive", &relativeDriveMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeDriveMod")))
            *p = relativeDriveMod;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets drive (0-1)");
    
    ImGui::SameLine();
    bool relativeGainMod = relativeGainModParam != nullptr && relativeGainModParam->load() > 0.5f;
    if (ImGui::Checkbox("Rel Gain", &relativeGainMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeGainMod")))
            *p = relativeGainMod;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets gain (0-1)");
    
    bool relativeMixMod = relativeMixModParam != nullptr && relativeMixModParam->load() > 0.5f;
    if (ImGui::Checkbox("Rel Mix", &relativeMixMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMixMod")))
            *p = relativeMixMod;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets mix (0-1)");
    
    ImGui::SameLine();
    bool relativeCharacterMod = relativeCharacterModParam != nullptr && relativeCharacterModParam->load() > 0.5f;
    if (ImGui::Checkbox("Rel Character", &relativeCharacterMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeCharacterMod")))
            *p = relativeCharacterMod;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets character (0-1)");
    
    ImGui::SameLine();
    bool relativeSmoothnessMod = relativeSmoothnessModParam != nullptr && relativeSmoothnessModParam->load() > 0.5f;
    if (ImGui::Checkbox("Rel Smoothness", &relativeSmoothnessMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeSmoothnessMod")))
            *p = relativeSmoothnessMod;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets smoothness (0-1)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // === ADVANCED CONTROLS (COLLAPSIBLE) ===
    static bool showAdvanced = false;
    if (ImGui::CollapsingHeader("Advanced Settings", &showAdvanced))
    {
        ImGui::Spacing();
        
        // Compact oscillator matrix (only when expanded)
        ThemeText("Oscillator Details", theme.text.section_header);
        ImGui::Spacing();
        
        // Table header
        ImGui::Columns(5, "osc_matrix", false);
        ImGui::SetColumnWidth(0, 30.0f);  // Harmonic number
        ImGui::SetColumnWidth(1, (itemWidth - 30.0f) * 0.3f); // Ratio
        ImGui::SetColumnWidth(2, (itemWidth - 30.0f) * 0.2f); // Detune
        ImGui::SetColumnWidth(3, (itemWidth - 30.0f) * 0.25f); // Waveform
        ImGui::SetColumnWidth(4, (itemWidth - 30.0f) * 0.25f);  // Drive
        ImGui::Text("H"); ImGui::NextColumn();
        ImGui::Text("Ratio"); ImGui::NextColumn();
        ImGui::Text("Detune"); ImGui::NextColumn();
        ImGui::Text("Wave"); ImGui::NextColumn();
        ImGui::Text("Drive"); ImGui::NextColumn();
        ImGui::Separator();
        
        for (int i = 0; i < NUM_OSCILLATORS; ++i)
        {
            auto idx = juce::String(i + 1);
            ImGui::PushID(i);
            
            // Harmonic number
            ImGui::Text("%d", i + 1);
            ImGui::NextColumn();
            
            // Ratio
            float ratio = ratioParams[i]->load();
            ImGui::PushItemWidth(-1);
            if (ImGui::DragFloat("##ratio", &ratio, 0.01f, 0.125f, 16.0f, "%.2fx"))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("ratio_" + idx))) *p = ratio;
                onModificationEnded();
            }
            ImGui::PopItemWidth();
            ImGui::NextColumn();
            
            // Detune
            float detune = detuneParams[i]->load();
            ImGui::PushItemWidth(-1);
            if (ImGui::DragFloat("##detune", &detune, 1.0f, -100.0f, 100.0f, "%.0f"))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("detune_" + idx))) *p = detune;
                onModificationEnded();
            }
            ImGui::PopItemWidth();
            ImGui::NextColumn();
            
            // Waveform (shows current, but Ctrl+click on bar is primary)
            int wave = (int)waveformParams[i]->load();
            const char* waveLabels[] = { "Sine", "Saw", "Square", "Triangle" };
            if (ImGui::Button(waveLabels[wave], ImVec2(-1, 0)))
            {
                wave = (wave + 1) % 4;
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("waveform_" + idx))) *p = wave;
                onModificationEnded();
            }
            ImGui::NextColumn();
            
            // Drive
            float oscDrive = driveParams[i]->load();
            ImGui::PushItemWidth(-1);
            if (ImGui::SliderFloat("##drive", &oscDrive, 0.0f, 1.0f, "%.2f"))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("drive_" + idx))) *p = oscDrive;
                onModificationEnded();
            }
            ImGui::PopItemWidth();
            ImGui::NextColumn();
            
            ImGui::PopID();
        }
        ImGui::Columns(1);
    }
    ImGui::PopID();
}

void HarmonicShaperModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-7) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Freq Mod (ch 2), Drive Mod (ch 3), Gain Mod (ch 4), Mix Mod (ch 5), Character Mod (ch 6), Smoothness Mod (ch 7) are drawn inline in drawParametersInNode
}

juce::String HarmonicShaperModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel) {
        case 0: return "In L";
        case 1: return "In R";
        case 2: return "Freq Mod";
        case 3: return "Drive Mod";
        case 4: return "Gain Mod";
        case 5: return "Mix Mod";
        case 6: return "Character Mod";
        case 7: return "Smoothness Mod";
        default: return {};
    }
}

juce::String HarmonicShaperModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel) {
        case 0: return "Out L";
        case 1: return "Out R";
        default: return {};
    }
}
#endif

std::vector<DynamicPinInfo> HarmonicShaperModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio inputs (bus 0, channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});
    
    // Modulation inputs (bus 1, channels 0-5)
    pins.push_back({"Freq Mod", 2, PinDataType::CV});
    pins.push_back({"Drive Mod", 3, PinDataType::CV});
    pins.push_back({"Gain Mod", 4, PinDataType::CV});
    pins.push_back({"Mix Mod", 5, PinDataType::CV});
    pins.push_back({"Character Mod", 6, PinDataType::CV});
    pins.push_back({"Smoothness Mod", 7, PinDataType::CV});
    
    return pins;
}

std::vector<DynamicPinInfo> HarmonicShaperModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});
    
    return pins;
}

bool HarmonicShaperModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    // Debug: Log routing requests to verify parameter IDs are recognized
    static juce::StringArray loggedParams;
    if (!loggedParams.contains(paramId)) {
        loggedParams.add(paramId);
        juce::Logger::writeToLog("[HarmonicShaper][ROUTING REQ] paramId=" + paramId);
    }
    // All modulation is on the single input bus (like Granulator and BestPracticeNodeProcessor)
    outBusIndex = 0;
    if (paramId == paramIdMasterFreqMod) { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdMasterDriveMod) { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdOutputGainMod) { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdMixMod) { outChannelIndexInBus = 5; return true; }
    if (paramId == paramIdCharacterMod) { outChannelIndexInBus = 6; return true; }
    if (paramId == paramIdSmoothnessMod) { outChannelIndexInBus = 7; return true; }
    return false;
}

