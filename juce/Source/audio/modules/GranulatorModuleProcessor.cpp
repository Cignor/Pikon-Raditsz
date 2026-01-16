#include "GranulatorModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif
#include <cstring> // for std::memcpy

juce::AudioProcessorValueTreeState::ParameterLayout GranulatorModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdDensity, "Density (Hz)", juce::NormalisableRange<float>(0.1f, 100.0f, 0.01f, 0.3f), 10.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdSize, "Size (ms)", juce::NormalisableRange<float>(5.0f, 500.0f, 0.01f, 0.4f), 100.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdPosition, "Position", 0.0f, 1.0f, 0.5f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdSpread, "Spread", 0.0f, 1.0f, 0.1f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdPitch, "Pitch (st)", -24.0f, 24.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdPitchRandom, "Pitch Rand", 0.0f, 12.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdPanRandom, "Pan Rand", 0.0f, 1.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdGate, "Gate", 0.0f, 1.0f, 1.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 1.0f));
    
    // Relative modulation mode parameters (default: true = relative mode)
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdRelativeDensityMod, "Relative Density Mod", true));
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdRelativeSizeMod, "Relative Size Mod", true));
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdRelativePositionMod, "Relative Position Mod", true));
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdRelativePitchMod, "Relative Pitch Mod", true));
    
    return { p.begin(), p.end() };
}

GranulatorModuleProcessor::GranulatorModuleProcessor()
    : ModuleProcessor(BusesProperties()
          .withInput("Inputs", juce::AudioChannelSet::discreteChannels(12), true) // Audio L/R, Trig, Density, Size, Position, Spread, Pitch, Pitch Rand, Pan Rand, Gate, Mix
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "GranulatorParams", createParameterLayout())
{
    densityParam      = apvts.getRawParameterValue(paramIdDensity);
    sizeParam         = apvts.getRawParameterValue(paramIdSize);
    positionParam     = apvts.getRawParameterValue(paramIdPosition);
    spreadParam       = apvts.getRawParameterValue(paramIdSpread);
    pitchParam        = apvts.getRawParameterValue(paramIdPitch);
    pitchRandomParam  = apvts.getRawParameterValue(paramIdPitchRandom);
    panRandomParam    = apvts.getRawParameterValue(paramIdPanRandom);
    gateParam         = apvts.getRawParameterValue(paramIdGate);
    mixParam          = apvts.getRawParameterValue(paramIdMix);
    
    relativeDensityModParam  = apvts.getRawParameterValue(paramIdRelativeDensityMod);
    relativeSizeModParam     = apvts.getRawParameterValue(paramIdRelativeSizeMod);
    relativePositionModParam = apvts.getRawParameterValue(paramIdRelativePositionMod);
    relativePitchModParam    = apvts.getRawParameterValue(paramIdRelativePitchMod);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));

    // Initialize visualization data
    for (auto& w : vizData.waveformL) w.store(0.0f);
    for (auto& w : vizData.waveformR) w.store(0.0f);
    for (auto& p : vizData.activeGrainPositions) p.store(-1.0f); // -1 = inactive
    for (auto& e : vizData.activeGrainEnvelopes) e.store(0.0f);
}

void GranulatorModuleProcessor::prepareToPlay(double sampleRate, int)
{
    const int bufferSeconds = 2;
    sourceBuffer.setSize(2, (int)(sampleRate * bufferSeconds));
    sourceBuffer.clear();
    sourceWritePos = 0;

    smoothedDensity.reset(sampleRate, 0.05);
    smoothedSize.reset(sampleRate, 0.05);
    smoothedPosition.reset(sampleRate, 0.05);
    smoothedPitch.reset(sampleRate, 0.05);
    smoothedGate.reset(sampleRate, 0.002);
    smoothedMix.reset(sampleRate, 0.01);

    densityPhase = 0.0;

    for (auto& grain : grainPool)
        grain.isActive = false;

    // Emit a startup log and warn about potential aliasing (enabled in all builds, throttled by prepareToPlay frequency)
    juce::Logger::writeToLog("[Granulator] prepareToPlay; inputs=" + juce::String(getTotalNumInputChannels()) +
                             " outputs=" + juce::String(getTotalNumOutputChannels()));
    if (getTotalNumInputChannels() >= getTotalNumOutputChannels())
        juce::Logger::writeToLog("[Granulator] [WARNING] Potential buffer aliasing: " +
                                 juce::String(getTotalNumInputChannels()) + " inputs, " +
                                 juce::String(getTotalNumOutputChannels()) + " outputs");
}

void GranulatorModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const int numSamples = buffer.getNumSamples();
    const double sr = getSampleRate();

    // Get modulation CVs
    // IMPORTANT: Acquire input pointers BEFORE any output operations (aliasing-safe)
    const bool isTriggerConnected = isParamInputConnected(paramIdTriggerIn);
    const bool hasDensityMod = isParamInputConnected(paramIdDensityMod);
    const bool hasSizeMod = isParamInputConnected(paramIdSizeMod);
    const bool hasPositionMod = isParamInputConnected(paramIdPositionMod);
    const bool hasSpreadMod = isParamInputConnected(paramIdSpreadMod);
    const bool hasPitchMod = isParamInputConnected(paramIdPitchMod);
    const bool hasPitchRandomMod = isParamInputConnected(paramIdPitchRandomMod);
    const bool hasPanRandomMod = isParamInputConnected(paramIdPanRandomMod);
    const bool hasGateMod = isParamInputConnected(paramIdGateMod);
    const bool hasMixMod = isParamInputConnected(paramIdMixMod);

    // SAFE: Read input pointers BEFORE any output operations
    const float* trigCVPtr      = (isTriggerConnected && inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;
    const float* densityPtr    = (hasDensityMod   && inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* sizePtr       = (hasSizeMod      && inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;
    const float* posPtr         = (hasPositionMod  && inBus.getNumChannels() > 5) ? inBus.getReadPointer(5) : nullptr;
    const float* spreadPtr      = (hasSpreadMod    && inBus.getNumChannels() > 6) ? inBus.getReadPointer(6) : nullptr;
    const float* pitchPtr       = (hasPitchMod     && inBus.getNumChannels() > 7) ? inBus.getReadPointer(7) : nullptr;
    const float* pitchRandomPtr = (hasPitchRandomMod && inBus.getNumChannels() > 8) ? inBus.getReadPointer(8) : nullptr;
    const float* panRandomPtr   = (hasPanRandomMod && inBus.getNumChannels() > 9) ? inBus.getReadPointer(9) : nullptr;
    const float* gatePtr        = (hasGateMod      && inBus.getNumChannels() > 10) ? inBus.getReadPointer(10) : nullptr;
    const float* mixPtr         = (hasMixMod       && inBus.getNumChannels() > 11) ? inBus.getReadPointer(11) : nullptr;
    
    // Get base parameter values ONCE
    const float baseMix = mixParam != nullptr ? mixParam->load() : 1.0f;
    
    // Store dry signal for mixing
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.setSize(2, numSamples, false, false, true);
    for (int ch = 0; ch < juce::jmin(2, inBus.getNumChannels()); ++ch)
        dryBuffer.copyFrom(ch, 0, inBus, ch, 0, numSamples);

    // EXTRA SAFETY: Copy CV channels we use into local buffers BEFORE any output writes,
    // so later writes cannot affect reads even if buffers alias (see DEBUG_INPUT_IMPORTANT.md).
    juce::HeapBlock<float> trigCV, densityCV, sizeCV, posCV, spreadCV, pitchCV, pitchRandomCV, panRandomCV, gateCV, mixCV;
    if (trigCVPtr)      { trigCV.malloc(numSamples);        std::memcpy(trigCV.get(),        trigCVPtr,      sizeof(float) * (size_t)numSamples); }
    if (densityPtr)     { densityCV.malloc(numSamples);     std::memcpy(densityCV.get(),     densityPtr,     sizeof(float) * (size_t)numSamples); }
    if (sizePtr)        { sizeCV.malloc(numSamples);        std::memcpy(sizeCV.get(),        sizePtr,        sizeof(float) * (size_t)numSamples); }
    if (posPtr)         { posCV.malloc(numSamples);         std::memcpy(posCV.get(),         posPtr,         sizeof(float) * (size_t)numSamples); }
    if (spreadPtr)      { spreadCV.malloc(numSamples);       std::memcpy(spreadCV.get(),      spreadPtr,      sizeof(float) * (size_t)numSamples); }
    if (pitchPtr)       { pitchCV.malloc(numSamples);        std::memcpy(pitchCV.get(),       pitchPtr,       sizeof(float) * (size_t)numSamples); }
    if (pitchRandomPtr) { pitchRandomCV.malloc(numSamples);  std::memcpy(pitchRandomCV.get(), pitchRandomPtr, sizeof(float) * (size_t)numSamples); }
    if (panRandomPtr)   { panRandomCV.malloc(numSamples);    std::memcpy(panRandomCV.get(),   panRandomPtr,   sizeof(float) * (size_t)numSamples); }
    if (gatePtr)        { gateCV.malloc(numSamples);         std::memcpy(gateCV.get(),         gatePtr,        sizeof(float) * (size_t)numSamples); }
    if (mixPtr)         { mixCV.malloc(numSamples);          std::memcpy(mixCV.get(),          mixPtr,         sizeof(float) * (size_t)numSamples); }

    // Get base parameters
    const float baseDensity = densityParam->load();
    const float baseSize = sizeParam->load();
    const float basePos = positionParam->load();
    const float baseSpread = spreadParam->load();
    const float basePitch = pitchParam->load();
    const float basePitchRandom = pitchRandomParam->load();
    const float basePanRandom = panRandomParam->load();
    const float baseGate = gateParam->load();

    // Diagnostic: log CV connection state and first-sample values periodically (enabled in all builds)
    {
        static int dbgCounter = 0;
        if ((++dbgCounter & 0x7F) == 0) // every 128 calls to reduce spam
        {
            juce::String msg = "[Granulator][CV DEBUG] inCh=" + juce::String(inBus.getNumChannels()) +
                                " outCh=" + juce::String(outBus.getNumChannels()) +
                                " N=" + juce::String(numSamples) + " | ";
            msg += "TrigConn=" + juce::String(isTriggerConnected ? 1 : 0) +
                   " DenConn=" + juce::String(hasDensityMod ? 1 : 0) +
                   " SizeConn=" + juce::String(hasSizeMod ? 1 : 0) +
                   " PosConn=" + juce::String(hasPositionMod ? 1 : 0) +
                   " PitchConn=" + juce::String(hasPitchMod ? 1 : 0) +
                   " GateConn=" + juce::String(hasGateMod ? 1 : 0) + " | ";
            auto fmt = [](const juce::HeapBlock<float>& b)->juce::String {
                return b.get() ? juce::String(b[0], 3) : juce::String("---");
            };
            msg += "v0(Trig)=" + fmt(trigCV) + " v3(Den)=" + fmt(densityCV) +
                   " v4(Size)=" + fmt(sizeCV) + " v5(Pos)=" + fmt(posCV) +
                   " v6(Pitch)=" + fmt(pitchCV) + " v7(Gate)=" + fmt(gateCV);
            juce::Logger::writeToLog(msg);

            // Extra: min/max snapshots to see if CVs are changing across the block
            auto rangeFmt = [&](const juce::HeapBlock<float>& b)->juce::String {
                if (!b.get()) return juce::String("---");
                float mn = b[0], mx = b[0];
                const int step = juce::jmax(1, numSamples / 64);
                for (int i = 0; i < numSamples; i += step) { mn = juce::jmin(mn, b[i]); mx = juce::jmax(mx, b[i]); }
                return juce::String(mn, 3) + ".." + juce::String(mx, 3);
            };
            juce::Logger::writeToLog("[Granulator][CV RANGE] Trig=" + rangeFmt(trigCV) +
                                     " Den=" + rangeFmt(densityCV) +
                                     " Size=" + rangeFmt(sizeCV) +
                                     " Pos=" + rangeFmt(posCV) +
                                     " Pitch=" + rangeFmt(pitchCV) +
                                     " Gate=" + rangeFmt(gateCV));

            // Routing check: confirm channels used by getParamRouting
            int busIdx, ch;
            juce::String mapMsg = "[Granulator][ROUTING] ";
            if (getParamRouting(paramIdTriggerIn, busIdx, ch)) mapMsg += "Trig->" + juce::String(ch) + " ";
            if (getParamRouting(paramIdDensityMod, busIdx, ch)) mapMsg += "Den->" + juce::String(ch) + " ";
            if (getParamRouting(paramIdSizeMod, busIdx, ch)) mapMsg += "Size->" + juce::String(ch) + " ";
            if (getParamRouting(paramIdPositionMod, busIdx, ch)) mapMsg += "Pos->" + juce::String(ch) + " ";
            if (getParamRouting(paramIdPitchMod, busIdx, ch)) mapMsg += "Pitch->" + juce::String(ch) + " ";
            if (getParamRouting(paramIdGateMod, busIdx, ch)) mapMsg += "Gate->" + juce::String(ch) + " ";
            juce::Logger::writeToLog(mapMsg);
        }
    }

    // Helper lambdas to fetch possibly block-constant CVs with intra-block interpolation
    auto sampleCvOrRamp = [&](const juce::HeapBlock<float>& buf, float& prev, int i)->float
    {
        if (!buf.get()) return std::numeric_limits<float>::quiet_NaN();
        const float first = buf[0];
        const float last  = buf[numSamples - 1];
        // If block is flat (or nearly), linearly ramp from previous block value to current
        const bool flat = std::abs(last - first) < 1e-6f;
        if (flat)
        {
            const float start = std::isfinite(prev) ? prev : first;
            const float t = (numSamples > 1) ? (float)i / (float)(numSamples - 1) : 1.0f;
            return start + (first - start) * t;
        }
        // Otherwise, use per-sample value
        return buf[i];
    };

    // Normalize CV to [0,1] range (handle both unipolar and bipolar)
    auto normalizeCV = [](float cv) -> float {
        return (cv >= 0.0f && cv <= 1.0f) 
            ? juce::jlimit(0.0f, 1.0f, cv)
            : juce::jlimit(0.0f, 1.0f, (cv + 1.0f) * 0.5f);
    };

    for (int i = 0; i < numSamples; ++i)
    {
        // 1. Record incoming audio to circular buffer
        sourceBuffer.setSample(0, sourceWritePos, inBus.getSample(0, i));
        sourceBuffer.setSample(1, sourceWritePos, inBus.getSample(1, i));

        // 2. Handle triggers
        // Default to ON if the trigger input is not connected.
        bool isGenerating = !isTriggerConnected;
        if (isTriggerConnected && trigCV.get() != nullptr) {
            // If connected, follow the gate signal.
            isGenerating = trigCV[i] > 0.5f;
        }
        
        // 3. Update smoothed parameters (with intra-block ramping if source is block-constant)
        float densityCvSample   = densityCV.get()      ? sampleCvOrRamp(densityCV,      prevDensityCv,   i) : std::numeric_limits<float>::quiet_NaN();
        float sizeCvSample      = sizeCV.get()         ? sampleCvOrRamp(sizeCV,         prevSizeCv,      i) : std::numeric_limits<float>::quiet_NaN();
        float positionCvSample  = posCV.get()          ? sampleCvOrRamp(posCV,          prevPositionCv,  i) : std::numeric_limits<float>::quiet_NaN();
        float spreadCvSample    = spreadCV.get()       ? sampleCvOrRamp(spreadCV,       prevSpreadCv,    i) : std::numeric_limits<float>::quiet_NaN();
        float pitchCvSample      = pitchCV.get()        ? sampleCvOrRamp(pitchCV,        prevPitchCv,     i) : std::numeric_limits<float>::quiet_NaN();
        float pitchRandomCvSample = pitchRandomCV.get() ? sampleCvOrRamp(pitchRandomCV, prevPitchRandomCv, i) : std::numeric_limits<float>::quiet_NaN();
        float panRandomCvSample = panRandomCV.get()    ? sampleCvOrRamp(panRandomCV,    prevPanRandomCv, i) : std::numeric_limits<float>::quiet_NaN();
        float gateCvSample       = gateCV.get()         ? sampleCvOrRamp(gateCV,         prevGateCv,      i) : std::numeric_limits<float>::quiet_NaN();

        // Apply CV with relative/absolute mode
        float density = baseDensity;
        if (std::isfinite(densityCvSample))
        {
            const float cv01 = normalizeCV(densityCvSample);
            const bool relativeMode = relativeDensityModParam && relativeDensityModParam->load() > 0.5f;
            if (relativeMode)
            {
                // RELATIVE: CV modulates around base (0.5x to 2x range)
                density = baseDensity * juce::jmap(cv01, 0.0f, 1.0f, 0.5f, 2.0f);
            }
            else
            {
                // ABSOLUTE: CV directly sets density (0.1 to 100 Hz)
                density = juce::jmap(cv01, 0.1f, 100.0f);
            }
        }

        float sizeMs = baseSize;
        if (std::isfinite(sizeCvSample))
        {
            const float cv01 = normalizeCV(sizeCvSample);
            const bool relativeMode = relativeSizeModParam && relativeSizeModParam->load() > 0.5f;
            if (relativeMode)
            {
                // RELATIVE: CV modulates around base (0.1x to 2x range)
                sizeMs = baseSize * juce::jmap(cv01, 0.0f, 1.0f, 0.1f, 2.0f);
            }
            else
            {
                // ABSOLUTE: CV directly sets size (5 to 500 ms)
                sizeMs = juce::jmap(cv01, 5.0f, 500.0f);
            }
        }

        float position = basePos;
        if (std::isfinite(positionCvSample))
        {
            const float cv01 = normalizeCV(positionCvSample);
            const bool relativeMode = relativePositionModParam && relativePositionModParam->load() > 0.5f;
            if (relativeMode)
            {
                // RELATIVE: CV adds offset to base position (±0.5 range)
                position = basePos + juce::jlimit(-0.5f, 0.5f, cv01 - 0.5f);
            }
            else
            {
                // ABSOLUTE: CV directly sets position (0 to 1)
                position = cv01;
            }
        }

        float pitch = basePitch;
        if (std::isfinite(pitchCvSample))
        {
            const float cv01 = normalizeCV(pitchCvSample);
            const bool relativeMode = relativePitchModParam && relativePitchModParam->load() > 0.5f;
            if (relativeMode)
            {
                // RELATIVE: CV adds offset to base pitch (±12 semitones)
                pitch = basePitch + juce::jmap(cv01, 0.0f, 1.0f, -12.0f, 12.0f);
            }
            else
            {
                // ABSOLUTE: CV directly sets pitch (-24 to +24 semitones)
                pitch = juce::jmap(cv01, -24.0f, 24.0f);
            }
        }

        float gate = std::isfinite(gateCvSample) ? juce::jlimit(0.0f, 1.0f, normalizeCV(gateCvSample)) : baseGate;
        
        // Calculate spread with CV modulation
        float spread = baseSpread;
        if (std::isfinite(spreadCvSample))
        {
            const float cv01 = normalizeCV(spreadCvSample);
            spread = juce::jlimit(0.0f, 1.0f, cv01); // CV directly maps to spread (0-1)
        }
        
        // Calculate pitchRandom with CV modulation
        float pitchRandom = basePitchRandom;
        if (std::isfinite(pitchRandomCvSample))
        {
            const float cv01 = normalizeCV(pitchRandomCvSample);
            pitchRandom = juce::jlimit(0.0f, 12.0f, cv01 * 12.0f); // CV maps to 0-12 semitones
        }
        
        // Calculate panRandom with CV modulation
        float panRandom = basePanRandom;
        if (std::isfinite(panRandomCvSample))
        {
            const float cv01 = normalizeCV(panRandomCvSample);
            panRandom = juce::jlimit(0.0f, 1.0f, cv01); // CV directly maps to panRandom (0-1)
        }
        
        // Calculate mix with CV modulation
        float mixCvSample = mixCV.get() ? sampleCvOrRamp(mixCV, prevMixCv, i) : std::numeric_limits<float>::quiet_NaN();
        float currentMix = baseMix;
        if (std::isfinite(mixCvSample)) {
            const float cv01 = normalizeCV(mixCvSample);
            currentMix = juce::jlimit(0.0f, 1.0f, cv01); // CV directly maps to mix (0-1)
        }
        smoothedMix.setTargetValue(currentMix);
        
        // ✅ CRITICAL FIX: Advance ALL smoothed values every sample (like Gate does)
        // This makes all CV inputs respond continuously, not just when grains spawn
        smoothedDensity.setTargetValue(density);
        smoothedSize.setTargetValue(sizeMs);
        smoothedPosition.setTargetValue(position);
        smoothedPitch.setTargetValue(pitch);
        smoothedGate.setTargetValue(gate);
        
        // Get continuously-updated smoothed values (like Gate does)
        float currentDensity = smoothedDensity.getNextValue();
        float currentSize = smoothedSize.getNextValue();
        float currentPosition = smoothedPosition.getNextValue();
        float currentPitch = smoothedPitch.getNextValue();
        float currentGate = smoothedGate.getNextValue();

        // 4. Spawn new grains using phase accumulator (makes density changes immediate)
        if (isGenerating && currentDensity > 0.1f) {
            // Phase accumulator: density in Hz means grains per second
            densityPhase += currentDensity / sr;
            
            // Spawn grains as phase accumulates
            while (densityPhase >= 1.0) {
                densityPhase -= 1.0;
                
                // Find free grain slot
                for (int j = 0; j < (int)grainPool.size(); ++j) {
                    if (!grainPool[j].isActive) {
                        launchGrain(j, currentDensity, currentSize, currentPosition, 
                                    spread, currentPitch, 
                                    pitchRandom, panRandom);
                        break;
                    }
                }
            }
        } else {
            // If not generating or density too low, reset phase
            densityPhase = 0.0;
        }

        // 5. Process active grains
        float sampleL = 0.0f, sampleR = 0.0f;
        for (auto& grain : grainPool) {
            if (grain.isActive) {
                int readPosInt = (int)grain.readPosition;
                float fraction = (float)(grain.readPosition - readPosInt);
                
                // Linear interpolation
                float sL = sourceBuffer.getSample(0, readPosInt) * (1.0f - fraction) + sourceBuffer.getSample(0, (readPosInt + 1) % sourceBuffer.getNumSamples()) * fraction;
                float sR = sourceBuffer.getSample(1, readPosInt) * (1.0f - fraction) + sourceBuffer.getSample(1, (readPosInt + 1) % sourceBuffer.getNumSamples()) * fraction;

                // Hann window envelope
                float envelope = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * (float)(grain.totalLifetime - grain.samplesRemaining) / (float)grain.totalLifetime));
                
                sampleL += sL * envelope * grain.panL;
                sampleR += sR * envelope * grain.panR;

                grain.readPosition += grain.increment;
                if (grain.readPosition >= sourceBuffer.getNumSamples())
                    grain.readPosition -= sourceBuffer.getNumSamples();
                
                if (--grain.samplesRemaining <= 0)
                    grain.isActive = false;
            }
        }
        
        // 6. Apply gate and mix, then write to output
        const float mix = smoothedMix.getNextValue();
        const float dry = 1.0f - mix;
        const float wetL = sampleL * currentGate;
        const float wetR = sampleR * currentGate;
        const float dryL = dryBuffer.getSample(0, i);
        const float dryR = dryBuffer.getSample(1, i);
        outBus.setSample(0, i, dry * dryL + mix * wetL);
        outBus.setSample(1, i, dry * dryR + mix * wetR);

        sourceWritePos = (sourceWritePos + 1) % sourceBuffer.getNumSamples();

        // Update visualization data (throttled - every 64 samples)
        if ((i & 0x3F) == 0)
        {
            const int bufferSize = sourceBuffer.getNumSamples();
            const float writePosNorm = (float)sourceWritePos / (float)bufferSize;
            vizData.writePosNormalized.store(writePosNorm);
            vizData.positionParamNormalized.store(currentPosition);

            // Update waveform snapshot (downsampled)
            const int step = bufferSize / VizData::waveformPoints;
            for (int j = 0; j < VizData::waveformPoints; ++j)
            {
                int idx = (sourceWritePos - (VizData::waveformPoints - j) * step + bufferSize) % bufferSize;
                vizData.waveformL[j].store(sourceBuffer.getSample(0, idx));
                vizData.waveformR[j].store(sourceBuffer.getSample(1, idx));
            }

            // Update active grain positions
            int activeCount = 0;
            for (int g = 0; g < (int)grainPool.size(); ++g)
            {
                const auto& grain = grainPool[g];
                if (grain.isActive)
                {
                    const float grainPosNorm = (float)grain.readPosition / (float)bufferSize;
                    vizData.activeGrainPositions[activeCount].store(grainPosNorm);
                    
                    // Calculate current envelope value
                    const float env = (grain.totalLifetime > 0) 
                        ? 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * 
                            (float)(grain.totalLifetime - grain.samplesRemaining) / (float)grain.totalLifetime))
                        : 0.0f;
                    vizData.activeGrainEnvelopes[activeCount].store(env);
                    ++activeCount;
                }
            }
            vizData.activeGrainCount.store(activeCount);
            
            // Clear inactive grain slots
            for (int g = activeCount; g < 64; ++g)
            {
                vizData.activeGrainPositions[g].store(-1.0f);
                vizData.activeGrainEnvelopes[g].store(0.0f);
            }
        }
    }

    // Update previous block CV anchors for de-stepping
    if (densityCV.get())      prevDensityCv      = densityCV[numSamples - 1];
    if (sizeCV.get())         prevSizeCv         = sizeCV[numSamples - 1];
    if (posCV.get())          prevPositionCv     = posCV[numSamples - 1];
    if (spreadCV.get())       prevSpreadCv       = spreadCV[numSamples - 1];
    if (pitchCV.get())        prevPitchCv        = pitchCV[numSamples - 1];
    if (pitchRandomCV.get())  prevPitchRandomCv  = pitchRandomCV[numSamples - 1];
    if (panRandomCV.get())    prevPanRandomCv    = panRandomCV[numSamples - 1];
    if (gateCV.get())         prevGateCv         = gateCV[numSamples - 1];
    if (mixCV.get())          prevMixCv           = mixCV[numSamples - 1];
    
    // Update live parameter values for telemetry
    if (numSamples > 0) {
        float finalSpread = baseSpread;
        if (spreadCV.get() != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, normalizeCV(spreadCV[numSamples - 1]));
            finalSpread = cv;
        }
        setLiveParamValue("spread_live", finalSpread);
        
        float finalPitchRandom = basePitchRandom;
        if (pitchRandomCV.get() != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, normalizeCV(pitchRandomCV[numSamples - 1]));
            finalPitchRandom = cv * 12.0f;
        }
        setLiveParamValue("pitchRandom_live", finalPitchRandom);
        
        float finalPanRandom = basePanRandom;
        if (panRandomCV.get() != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, normalizeCV(panRandomCV[numSamples - 1]));
            finalPanRandom = cv;
        }
        setLiveParamValue("panRandom_live", finalPanRandom);
        
        float finalMix = baseMix;
        if (mixCV.get() != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, normalizeCV(mixCV[numSamples - 1]));
            finalMix = cv;
        }
        setLiveParamValue("mix_live", finalMix);
    }
    
    // Update telemetry (use getCurrentValue since we've already advanced all smoothed values)
    setLiveParamValue("density_live", smoothedDensity.getCurrentValue());
    setLiveParamValue("size_live", smoothedSize.getCurrentValue());
    setLiveParamValue("position_live", smoothedPosition.getCurrentValue());
    setLiveParamValue("pitch_live", smoothedPitch.getCurrentValue());
    setLiveParamValue("gate_live", smoothedGate.getCurrentValue());
    
    if (lastOutputValues[0]) lastOutputValues[0]->store(outBus.getSample(0, numSamples - 1));
    if (lastOutputValues[1]) lastOutputValues[1]->store(outBus.getSample(1, numSamples - 1));
}

void GranulatorModuleProcessor::launchGrain(int grainIndex, float density, float size, float position, float spread, float pitch, float pitchRandom, float panRandom)
{
    auto& grain = grainPool[grainIndex];
    const double sr = getSampleRate();

    grain.totalLifetime = grain.samplesRemaining = (int)((size / 1000.0f) * sr);
    if (grain.samplesRemaining == 0) return;

    float posOffset = (random.nextFloat() - 0.5f) * spread;
    grain.readPosition = (sourceWritePos - (int)(juce::jlimit(0.0f, 1.0f, position + posOffset) * sourceBuffer.getNumSamples()) + sourceBuffer.getNumSamples()) % sourceBuffer.getNumSamples();

    float pitchOffset = (random.nextFloat() - 0.5f) * pitchRandom;
    grain.increment = std::pow(2.0, (pitch + pitchOffset) / 12.0);

    float pan = (random.nextFloat() - 0.5f) * panRandom;
    grain.panL = std::cos((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
    grain.panR = std::sin((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);

    grain.isActive = true;
}

#if defined(PRESET_CREATOR_UI)
void GranulatorModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    auto HelpMarker = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    auto drawSlider = [&](const char* label, const juce::String& paramId, const juce::String& modId, float min, float max, const char* format, int flags = 0, int channel = -1) {
        bool isMod = isParamModulated(modId);
        float value = isMod ? getLiveParamValueFor(modId, paramId + "_live", ap.getRawParameterValue(paramId)->load())
                            : ap.getRawParameterValue(paramId)->load();
        
        if (isMod) ImGui::BeginDisabled();
        // Draw inline pin if channel is specified and pinHelpers is available
        if (channel >= 0 && pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
        if (ImGui::SliderFloat(label, &value, min, max, format, flags))
            if (!isMod) *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramId)) = value;
        if (!isMod) adjustParamOnWheel(ap.getParameter(paramId), paramId, value);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    };

    // === GRANULATOR VISUALIZATION ===
    ImGui::Spacing();
    ImGui::Text("Buffer & Grains");
    ImGui::Spacing();

    // Draw waveform visualization with grain markers
    auto* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float vizWidth = itemWidth;
    const float vizHeight = 100.0f;
    const ImVec2 rectMax = ImVec2(origin.x + vizWidth, origin.y + vizHeight);
    
    // Get theme colors for visualization
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& themeMgr = ThemeManager::getInstance();
    
    // Helper to resolve color with multiple fallbacks
    auto resolveColor = [&](ImU32 primary, ImU32 secondary, ImU32 tertiary) -> ImU32 {
        if (primary != 0) return primary;
        if (secondary != 0) return secondary;
        return tertiary;
    };
    
    // Background: scope_plot_bg -> canvas_background -> ChildBg -> fallback
    const ImU32 canvasBg = themeMgr.getCanvasBackground();
    const ImVec4 childBgVec4 = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
    const ImU32 childBg = ImGui::ColorConvertFloat4ToU32(childBgVec4);
    const ImU32 bgColor = resolveColor(theme.modules.scope_plot_bg, canvasBg, childBg);
    
    // Waveform: Use modulation.frequency (cyan) for distinct waveform color
    const ImVec4 accentVec4 = theme.accent;
    const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentVec4.x, accentVec4.y, accentVec4.z, 0.78f));
    const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
    const ImU32 frequencyColor = ImGui::ColorConvertFloat4ToU32(ImVec4(frequencyColorVec4.x, frequencyColorVec4.y, frequencyColorVec4.z, 0.85f));
    const ImU32 waveformColor = resolveColor(theme.modules.scope_plot_fg, frequencyColor, IM_COL32(100, 200, 255, 220));
    
    // Write position: Use modulation.timbre (orange/yellow) for warm write position
    const ImU32 scopeTextMax = ImGui::ColorConvertFloat4ToU32(theme.modules.scope_text_max);
    const ImVec4 timbreColorVec4 = theme.modulation.timbre;
    const ImU32 timbreColor = ImGui::ColorConvertFloat4ToU32(ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f));
    const ImU32 writePosColor = resolveColor(theme.modules.scope_plot_max, timbreColor, IM_COL32(255, 200, 100, 255));
    
    // Position marker: Use modulation.amplitude (magenta/pink) for distinct position marker
    const ImU32 scopeTextMin = ImGui::ColorConvertFloat4ToU32(theme.modules.scope_text_min);
    const ImVec4 amplitudeColorVec4 = theme.modulation.amplitude;
    const ImU32 amplitudeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(amplitudeColorVec4.x, amplitudeColorVec4.y, amplitudeColorVec4.z, 0.85f));
    const ImU32 warningColor = ImGui::ColorConvertFloat4ToU32(theme.text.warning);
    const ImU32 positionMarkerColor = resolveColor(theme.modules.scope_plot_min, amplitudeColor, 
                                                   resolveColor(warningColor, IM_COL32(255, 100, 100, 200), IM_COL32(255, 100, 100, 200)));
    
    // Grains: Use modulation.filter (green) or text.success for distinct grain color
    const ImVec4 filterColorVec4 = theme.modulation.filter;
    const ImU32 filterColor = ImGui::ColorConvertFloat4ToU32(ImVec4(filterColorVec4.x, filterColorVec4.y, filterColorVec4.z, 1.0f));
    const ImVec4 successColorVec4 = theme.text.success;
    const ImU32 successColor = ImGui::ColorConvertFloat4ToU32(successColorVec4);
    const ImU32 grainColorBase = (filterColor != 0) ? filterColor : 
                                 (successColor != 0) ? successColor : IM_COL32(100, 255, 100, 255);
    const ImU32 grainColor = grainColorBase;
    
    // Grain envelope: Use a lighter, more transparent version of grain color
    const ImVec4 grainColorVec4 = ImGui::ColorConvertU32ToFloat4(grainColor);
    const ImU32 grainEnvelopeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
        grainColorVec4.x, grainColorVec4.y, grainColorVec4.z, 0.5f));
    
    drawList->AddRectFilled(origin, rectMax, bgColor, 4.0f);
    ImGui::PushClipRect(origin, rectMax, true);
    
    // Read visualization data (thread-safe)
    float waveformL[VizData::waveformPoints];
    float waveformR[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        waveformL[i] = vizData.waveformL[i].load();
        waveformR[i] = vizData.waveformR[i].load();
    }
    const float writePosNorm = vizData.writePosNormalized.load();
    const float positionNorm = vizData.positionParamNormalized.load();
    const int activeGrainCount = vizData.activeGrainCount.load();
    float grainPositions[64];
    float grainEnvelopes[64];
    for (int i = 0; i < 64; ++i)
    {
        grainPositions[i] = vizData.activeGrainPositions[i].load();
        grainEnvelopes[i] = vizData.activeGrainEnvelopes[i].load();
    }
    
    const float midY = origin.y + vizHeight * 0.5f;
    const float scaleY = vizHeight * 0.4f;
    const float stepX = vizWidth / (float)(VizData::waveformPoints - 1);
    
    // Draw waveform (stereo average)
    float prevX = origin.x, prevY = midY;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const float sample = (waveformL[i] + waveformR[i]) * 0.5f;
        const float x = origin.x + i * stepX;
        const float y = midY - juce::jlimit(-1.0f, 1.0f, sample) * scaleY;
        if (i > 0) drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), waveformColor, 1.5f);
        prevX = x; prevY = y;
    }
    
    // Draw write position indicator (vertical line)
    const float writeX = origin.x + writePosNorm * vizWidth;
    drawList->AddLine(ImVec2(writeX, origin.y), ImVec2(writeX, rectMax.y), writePosColor, 2.0f);
    
    // Draw position parameter marker (vertical line)
    const float posX = origin.x + positionNorm * vizWidth;
    drawList->AddLine(ImVec2(posX, origin.y), ImVec2(posX, rectMax.y), positionMarkerColor, 1.5f);
    
    // Draw active grains as markers with envelope visualization
    for (int i = 0; i < activeGrainCount; ++i)
    {
        const float grainPos = grainPositions[i];
        if (grainPos >= 0.0f && grainPos <= 1.0f)
        {
            const float grainX = origin.x + grainPos * vizWidth;
            const float env = grainEnvelopes[i];
            const float envHeight = env * scaleY * 0.5f;
            
            // Vary grain color based on envelope strength for visual interest
            // Stronger grains are brighter, weaker grains are more transparent
            const float envAlpha = 0.3f + env * 0.5f;  // 0.3 to 0.8 alpha based on envelope
            const ImVec4 grainColorWithEnv = ImVec4(
                grainColorVec4.x,
                grainColorVec4.y,
                grainColorVec4.z,
                envAlpha);
            const ImU32 grainEnvelopeColorVaried = ImGui::ColorConvertFloat4ToU32(grainColorWithEnv);
            
            // Envelope visualization (vertical bar) with varied color
            drawList->AddLine(ImVec2(grainX, midY - envHeight), ImVec2(grainX, midY + envHeight), 
                            grainEnvelopeColorVaried, 2.0f);
            
            // Grain position marker (small circle) - brighter for stronger grains
            const float markerAlpha = 0.6f + env * 0.4f;  // 0.6 to 1.0 alpha
            const ImVec4 grainMarkerColor = ImVec4(
                grainColorVec4.x,
                grainColorVec4.y,
                grainColorVec4.z,
                markerAlpha);
            const ImU32 grainMarkerColorVaried = ImGui::ColorConvertFloat4ToU32(grainMarkerColor);
            drawList->AddCircleFilled(ImVec2(grainX, midY), 3.0f, grainMarkerColorVaried);
        }
    }
    
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, rectMax.y));
    ImGui::Dummy(ImVec2(vizWidth, 0));
    
    // Active grain count indicator
    ImGui::Text("Active Grains: %d / 64", activeGrainCount);
    float grainMeter = activeGrainCount / 64.0f;
    
    // Theme the progress bar color - use accent color directly
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
    ImGui::ProgressBar(grainMeter, ImVec2(itemWidth * 0.5f, 0), "");
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    ImGui::Text("%.0f%%", grainMeter * 100.0f);
    
    ImGui::Spacing();
    ImGui::Spacing();

    // === PARAMETERS ===
    drawSlider("Density", paramIdDensity, paramIdDensityMod, 0.1f, 100.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic, 3); // Channel 3 = Density Mod
    bool relDens = relativeDensityModParam && relativeDensityModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Density Mod", &relDens))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativeDensityMod)))
            *p = relDens;
        onModificationEnded();
    }
    ImGui::SameLine();
    HelpMarker("Relative: CV modulates around slider (0.5x-2x). Absolute: CV sets density directly (0.1-100 Hz).");

    drawSlider("Size", paramIdSize, paramIdSizeMod, 5.0f, 500.0f, "%.0f ms", ImGuiSliderFlags_Logarithmic, 4); // Channel 4 = Size Mod
    bool relSize = relativeSizeModParam && relativeSizeModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Size Mod", &relSize))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativeSizeMod)))
            *p = relSize;
        onModificationEnded();
    }
    ImGui::SameLine();
    HelpMarker("Relative: CV modulates around slider (0.1x-2x). Absolute: CV sets size directly (5-500 ms).");

    drawSlider("Position", paramIdPosition, paramIdPositionMod, 0.0f, 1.0f, "%.2f", 0, 5); // Channel 5 = Position Mod
    bool relPos = relativePositionModParam && relativePositionModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Position Mod", &relPos))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativePositionMod)))
            *p = relPos;
        onModificationEnded();
    }
    ImGui::SameLine();
    HelpMarker("Relative: CV adds offset to slider (±0.5). Absolute: CV sets position directly (0-1).");

    drawSlider("Spread", paramIdSpread, paramIdSpreadMod, 0.0f, 1.0f, "%.2f", 0, 6); // Channel 6 = Spread Mod
    drawSlider("Pitch", paramIdPitch, paramIdPitchMod, -24.0f, 24.0f, "%.1f st", 0, 7); // Channel 7 = Pitch Mod
    bool relPitch = relativePitchModParam && relativePitchModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Pitch Mod", &relPitch))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativePitchMod)))
            *p = relPitch;
        onModificationEnded();
    }
    ImGui::SameLine();
    HelpMarker("Relative: CV adds offset to slider (±12 st). Absolute: CV sets pitch directly (-24 to +24 st).");

    drawSlider("Pitch Rand", paramIdPitchRandom, paramIdPitchRandomMod, 0.0f, 12.0f, "%.1f st", 0, 8); // Channel 8 = Pitch Rand Mod
    drawSlider("Pan Rand", paramIdPanRandom, paramIdPanRandomMod, 0.0f, 1.0f, "%.2f", 0, 9); // Channel 9 = Pan Rand Mod
    drawSlider("Gate", paramIdGate, paramIdGateMod, 0.0f, 1.0f, "%.2f", 0, 10); // Channel 10 = Gate Mod
    drawSlider("Mix", paramIdMix, paramIdMixMod, 0.0f, 1.0f, "%.2f", 0, 11); // Channel 11 = Mix Mod
    ImGui::SameLine();
    HelpMarker("Wet/dry balance (0=dry, 1=wet)");

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void GranulatorModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio and trigger pins - modulation pins (channels 3-11) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    helpers.drawParallelPins("Trigger In", 2, nullptr, -1);
    // Density Mod (ch 3), Size Mod (ch 4), Position Mod (ch 5), Spread Mod (ch 6), Pitch Mod (ch 7), 
    // Pitch Rand Mod (ch 8), Pan Rand Mod (ch 9), Gate Mod (ch 10), Mix Mod (ch 11) are drawn inline in drawParametersInNode
}
#endif

std::vector<DynamicPinInfo> GranulatorModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});
    
    // Modulation/trigger inputs (channels 2-11)
    pins.push_back({"Trigger In", 2, PinDataType::Gate});
    pins.push_back({"Density Mod", 3, PinDataType::CV});
    pins.push_back({"Size Mod", 4, PinDataType::CV});
    pins.push_back({"Position Mod", 5, PinDataType::CV});
    pins.push_back({"Spread Mod", 6, PinDataType::CV});
    pins.push_back({"Pitch Mod", 7, PinDataType::CV});
    pins.push_back({"Pitch Rand Mod", 8, PinDataType::CV});
    pins.push_back({"Pan Rand Mod", 9, PinDataType::CV});
    pins.push_back({"Gate Mod", 10, PinDataType::CV});
    pins.push_back({"Mix Mod", 11, PinDataType::CV});
    
    return pins;
}

std::vector<DynamicPinInfo> GranulatorModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});
    
    return pins;
}

bool GranulatorModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus.
    if (paramId == paramIdTriggerIn)      { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdDensityMod)     { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdSizeMod)        { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdPositionMod)    { outChannelIndexInBus = 5; return true; }
    if (paramId == paramIdSpreadMod)      { outChannelIndexInBus = 6; return true; }
    if (paramId == paramIdPitchMod)       { outChannelIndexInBus = 7; return true; }
    if (paramId == paramIdPitchRandomMod) { outChannelIndexInBus = 8; return true; }
    if (paramId == paramIdPanRandomMod)   { outChannelIndexInBus = 9; return true; }
    if (paramId == paramIdGateMod)        { outChannelIndexInBus = 10; return true; }
    if (paramId == paramIdMixMod)         { outChannelIndexInBus = 11; return true; }
    return false;
}

juce::String GranulatorModuleProcessor::getAudioInputLabel(int channel) const
{
    switch(channel) {
        case 0: return "In L";
        case 1: return "In R";
        case 2: return "Trigger In";
        case 3: return "Density Mod";
        case 4: return "Size Mod";
        case 5: return "Position Mod";
        case 6: return "Spread Mod";
        case 7: return "Pitch Mod";
        case 8: return "Pitch Rand Mod";
        case 9: return "Pan Rand Mod";
        case 10: return "Gate Mod";
        case 11: return "Mix Mod";
        default: return {};
    }
}