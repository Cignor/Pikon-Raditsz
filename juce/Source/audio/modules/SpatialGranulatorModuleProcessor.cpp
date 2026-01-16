#include "SpatialGranulatorModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif
#include <cmath>

juce::AudioProcessorValueTreeState::ParameterLayout SpatialGranulatorModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdDryMix, "Dry Mix", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdPenMix, "Pen Mix", 0.0f, 1.0f, 0.5f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdSprayMix, "Spray Mix", 0.0f, 1.0f, 0.5f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdDensity,
            "Density (Hz)",
            juce::NormalisableRange<float>(0.1f, 100.0f, 0.01f, 0.3f),
            10.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdGrainSize,
            "Grain Size (ms)",
            juce::NormalisableRange<float>(5.0f, 500.0f, 0.01f, 0.4f),
            100.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdBufferLength,
            "Buffer Length (s)",
            juce::NormalisableRange<float>(1.0f, 10.0f, 0.1f),
            2.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdRedAmount, "Red Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdGreenAmount, "Green Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdBlueAmount, "Blue Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdYellowAmount, "Yellow Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdCyanAmount, "Cyan Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdMagentaAmount, "Magenta Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdOrangeAmount, "Orange Amount", 0.0f, 1.0f, 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdPurpleAmount, "Purple Amount", 0.0f, 1.0f, 1.0f));

    return {p.begin(), p.end()};
}

SpatialGranulatorModuleProcessor::SpatialGranulatorModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Inputs",
                  juce::AudioChannelSet::discreteChannels(7),
                  true) // ch0-1: audio, ch2-6: CV mods
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SpatialGranulatorParams", createParameterLayout())
{
    dryMixParam = apvts.getRawParameterValue(paramIdDryMix);
    penMixParam = apvts.getRawParameterValue(paramIdPenMix);
    sprayMixParam = apvts.getRawParameterValue(paramIdSprayMix);
    densityParam = apvts.getRawParameterValue(paramIdDensity);
    grainSizeParam = apvts.getRawParameterValue(paramIdGrainSize);
    bufferLengthParam = apvts.getRawParameterValue(paramIdBufferLength);
    redAmountParam = apvts.getRawParameterValue(paramIdRedAmount);
    greenAmountParam = apvts.getRawParameterValue(paramIdGreenAmount);
    blueAmountParam = apvts.getRawParameterValue(paramIdBlueAmount);
    yellowAmountParam = apvts.getRawParameterValue(paramIdYellowAmount);
    cyanAmountParam = apvts.getRawParameterValue(paramIdCyanAmount);
    magentaAmountParam = apvts.getRawParameterValue(paramIdMagentaAmount);
    orangeAmountParam = apvts.getRawParameterValue(paramIdOrangeAmount);
    purpleAmountParam = apvts.getRawParameterValue(paramIdPurpleAmount);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));

    // Initialize smoothed values with defaults (will be reset in prepareToPlay)
    smoothedDryMix.reset(44100.0, 0.05);
    smoothedPenMix.reset(44100.0, 0.05);
    smoothedSprayMix.reset(44100.0, 0.05);
    smoothedDensity.reset(44100.0, 0.05);
    smoothedGrainSize.reset(44100.0, 0.05);
    smoothedPenMix.setCurrentAndTargetValue(0.5f);
    smoothedSprayMix.setCurrentAndTargetValue(0.5f);
    smoothedDensity.setCurrentAndTargetValue(10.0f);
    smoothedGrainSize.setCurrentAndTargetValue(100.0f);

    // Initialize grain pool
    for (auto& grain : grainPool)
        grain.isActive = false;

    // Initialize voice pool
    for (auto& voice : voicePool)
    {
        voice.isActive = false;
        // Delay buffer will be resized in prepareToPlay based on actual sample rate
        // Max delay is 2000ms, so we need at least 2 seconds of buffer
        voice.delayBuffer.resize(96000, 0.0f); // Max 2 seconds at 48kHz (safe default)
        voice.pitchBuffer.resize(2048, 0.0f);  // Larger buffer for better pitch shifting quality
    }
}

void SpatialGranulatorModuleProcessor::prepareToPlay(double sampleRate, int)
{
    const float bufferLengthSeconds = bufferLengthParam ? bufferLengthParam->load() : 2.0f;
    const int   bufferSize = (int)(sampleRate * bufferLengthSeconds);

    sourceBuffer.setSize(2, bufferSize);
    sourceBuffer.clear();
    sourceWritePos = 0;
    samplesWritten = 0; // Reset sample counter

#if defined(PRESET_CREATOR_UI)
    // Initialize visualization buffer
    vizOutputBuffer.setSize(1, vizBufferSize);
    vizOutputBuffer.clear();
    vizWritePos = 0;
#endif
    smoothedDryMix.reset(sampleRate, 0.05);
    smoothedPenMix.reset(sampleRate, 0.05);
    smoothedSprayMix.reset(sampleRate, 0.05);
    smoothedDensity.reset(sampleRate, 0.05);
    smoothedGrainSize.reset(sampleRate, 0.05);

    dotDensityPhases.clear();

    // Reset all grains and voices
    for (auto& grain : grainPool)
        grain.isActive = false;

    // Resize delay buffers based on actual sample rate (max 2000ms delay)
    const int maxDelaySamples = (int)(sampleRate * 2.0); // 2 seconds max
    for (auto& voice : voicePool)
    {
        voice.isActive = false;
        voice.readPosition = 0.0;
        voice.delayWritePos = 0;
        voice.pitchPhase = 0.0;
        voice.delayBuffer.resize(maxDelaySamples, 0.0f);
        std::fill(voice.delayBuffer.begin(), voice.delayBuffer.end(), 0.0f);
        std::fill(voice.pitchBuffer.begin(), voice.pitchBuffer.end(), 0.0f);

        // Prepare filter for Green color
        juce::dsp::ProcessSpec filterSpec{sampleRate, 512, 1}; // Use 512 as default block size
        voice.filter.prepare(filterSpec);
        voice.filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        voice.filterCutoffHz = 20000.0f;
        voice.filterResonance = 0.707f;

        // Initialize Reverb buffers (Yellow color) - simple delay-based reverb
        const int reverbSize = (int)(sampleRate * 0.5f); // 500ms reverb buffer
        voice.reverbBufferL.resize(reverbSize, 0.0f);
        voice.reverbBufferR.resize(reverbSize, 0.0f);
        std::fill(voice.reverbBufferL.begin(), voice.reverbBufferL.end(), 0.0f);
        std::fill(voice.reverbBufferR.begin(), voice.reverbBufferR.end(), 0.0f);
        voice.reverbWritePos = 0;

        // Initialize Chorus buffers (Magenta color) - 50ms max delay
        const int chorusSize = (int)(sampleRate * 0.05f); // 50ms chorus buffer
        voice.chorusBufferL.resize(chorusSize, 0.0f);
        voice.chorusBufferR.resize(chorusSize, 0.0f);
        std::fill(voice.chorusBufferL.begin(), voice.chorusBufferL.end(), 0.0f);
        std::fill(voice.chorusBufferR.begin(), voice.chorusBufferR.end(), 0.0f);
        voice.chorusWritePos = 0;

        // Initialize Distortion tone filter (Cyan color) - reuse filterSpec
        voice.distortionToneFilter.prepare(filterSpec);
        voice.distortionToneFilter.coefficients =
            juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, 20000.0f);
        voice.distortionToneFilter.reset();
    }

    // Initialize grain effect buffers
    for (auto& grain : grainPool)
    {
        grain.isActive = false;
        // Initialize Reverb buffer (Yellow color) - simplified for grains
        const int reverbSize = (int)(sampleRate * 0.2f); // 200ms reverb buffer for grains
        grain.reverbBuffer.resize(reverbSize, 0.0f);
        std::fill(grain.reverbBuffer.begin(), grain.reverbBuffer.end(), 0.0f);
        grain.reverbWritePos = 0;

        // Initialize Chorus buffer (Magenta color) - 50ms max delay
        const int chorusSize = (int)(sampleRate * 0.05f); // 50ms chorus buffer
        grain.chorusBuffer.resize(chorusSize, 0.0f);
        std::fill(grain.chorusBuffer.begin(), grain.chorusBuffer.end(), 0.0f);
        grain.chorusWritePos = 0;
    }
}

void SpatialGranulatorModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&)
{
    static bool firstProcessBlock = true;
    if (firstProcessBlock)
    {
        firstProcessBlock = false;
    }

    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const int    numSamples = buffer.getNumSamples();
    const double sr = getSampleRate();

    // Safety check: if sample rate is 0 or buffer not initialized, just pass through
    if (sr <= 0.0 || numSamples == 0)
    {
        outBus.copyFrom(0, 0, inBus, 0, 0, numSamples);
        if (outBus.getNumChannels() > 1 && inBus.getNumChannels() > 1)
            outBus.copyFrom(1, 0, inBus, 1, 0, numSamples);
        return;
    }

    const float bufferLengthSeconds = bufferLengthParam ? bufferLengthParam->load() : 2.0f;
    int         currentSourceBufferSize = sourceBuffer.getNumSamples();

    // Update buffer size if needed
    const int requiredBufferSize = (int)(sr * bufferLengthSeconds);
    if (requiredBufferSize <= 0 || currentSourceBufferSize != requiredBufferSize)
    {
        if (requiredBufferSize > 0)
        {
            sourceBuffer.setSize(2, requiredBufferSize);
            currentSourceBufferSize = requiredBufferSize;
            if (sourceWritePos >= requiredBufferSize)
                sourceWritePos = 0;
            // Don't reset samplesWritten - keep it to track buffer fill
        }
        else
        {
            // Buffer not ready yet, just pass through
            outBus.copyFrom(0, 0, inBus, 0, 0, numSamples);
            if (outBus.getNumChannels() > 1 && inBus.getNumChannels() > 1)
                outBus.copyFrom(1, 0, inBus, 1, 0, numSamples);
            return;
        }
    }

    // Safety check: ensure source buffer is valid
    if (currentSourceBufferSize == 0)
    {
        outBus.copyFrom(0, 0, inBus, 0, 0, numSamples);
        if (outBus.getNumChannels() > 1 && inBus.getNumChannels() > 1)
            outBus.copyFrom(1, 0, inBus, 1, 0, numSamples);
        return;
    }

    // Read CV modulation inputs - Use _mod IDs for connection checks
    const bool dryMixModActive = isParamInputConnected(paramIdDryMixMod);
    const bool penMixModActive = isParamInputConnected(paramIdPenMixMod);
    const bool sprayMixModActive = isParamInputConnected(paramIdSprayMixMod);
    const bool densityModActive = isParamInputConnected(paramIdDensityMod);
    const bool grainSizeModActive = isParamInputConnected(paramIdGrainSizeMod);

    const float* dryMixCV =
        (dryMixModActive && inBus.getNumChannels() > 2) ? inBus.getReadPointer(2) : nullptr;
    const float* penMixCV =
        (penMixModActive && inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* sprayMixCV =
        (sprayMixModActive && inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;
    const float* densityCV =
        (densityModActive && inBus.getNumChannels() > 5) ? inBus.getReadPointer(5) : nullptr;
    const float* grainSizeCV =
        (grainSizeModActive && inBus.getNumChannels() > 6) ? inBus.getReadPointer(6) : nullptr;

    // Get parameters (base values)
    const float baseDryMix = dryMixParam ? dryMixParam->load() : 1.0f;
    const float basePenMix = penMixParam ? penMixParam->load() : 0.5f;
    const float baseSprayMix = sprayMixParam ? sprayMixParam->load() : 0.5f;
    const float baseDensity = densityParam ? densityParam->load() : 10.0f;
    const float baseGrainSizeMs = grainSizeParam ? grainSizeParam->load() : 100.0f;

    // Update smoothed values (safe even if not yet initialized)
    if (sr > 0.0)
    {
        smoothedDryMix.setTargetValue(baseDryMix);
        smoothedPenMix.setTargetValue(basePenMix);
        smoothedSprayMix.setTargetValue(baseSprayMix);
        smoothedDensity.setTargetValue(baseDensity);
        smoothedGrainSize.setTargetValue(baseGrainSizeMs);
    }

    // Clear output - REMOVED because it wipes the input buffer in in-place processing!
    // outBus.clear();

    // Get dots (thread-safe read)
    std::vector<Dot> currentDots;
    {
        const juce::ScopedReadLock lock(dotsLock);
        currentDots = dots;
    }

    // REMOVED early return for empty dots to ensure:
    // 1. Source buffer is always recorded (so we have history when dots appear)
    // 2. Dry/Wet mix is always respected (instead of forcing 100% dry)

    // Process each sample
    for (int i = 0; i < numSamples; ++i)
    {
        // 1. Record incoming audio to circular buffer
        if (currentSourceBufferSize > 0 && inBus.getNumChannels() >= 2)
        {
            sourceBuffer.setSample(0, sourceWritePos, inBus.getSample(0, i));
            sourceBuffer.setSample(1, sourceWritePos, inBus.getSample(1, i));
        }
        if (currentSourceBufferSize > 0)
        {
            sourceWritePos = (sourceWritePos + 1) % currentSourceBufferSize;
            samplesWritten++; // Track total samples written
        }

        float penSampleL = 0.0f;
        float penSampleR = 0.0f;
        float spraySampleL = 0.0f;
        float spraySampleR = 0.0f;

        // 2. Activate/update Pen tool voices based on dots (once per block, not per sample)
        // Match dots to voices (simple approach: one voice per dot, up to pool size)
        if (i == 0) // Only update once per block
        {
            int penDotIndex = 0;
            for (const auto& dot : currentDots)
            {
                if (dot.type == DotType::Pen && penDotIndex < (int)voicePool.size())
                {
                    auto& voice = voicePool[penDotIndex];
                    if (!voice.isActive)
                    {
                        // Activate new voice - but only if buffer has enough audio
                        // Wait for at least 1% of buffer to be filled before activating
                        const int minSamplesNeeded = juce::jmax(100, currentSourceBufferSize / 100);
                        if (samplesWritten >= minSamplesNeeded)
                        {
                            voice.isActive = true;
                            // Read position should be relative to write position (reading from the
                            // past) dot.y = 0 means read from recent past, dot.y = 1 means read
                            // from further past
                            const float bufferPos = dot.y; // 0 = recent, 1 = older
                            // Calculate read position behind the write position
                            // Ensure we only read from positions that have been written to
                            const int maxReadableOffset =
                                juce::jmin(samplesWritten - 1, currentSourceBufferSize - 1);
                            const int offset = maxReadableOffset > 0
                                                   ? (int)(bufferPos * maxReadableOffset * 0.9f)
                                                   : 0;
                            voice.readPosition =
                                (sourceWritePos - offset + currentSourceBufferSize) %
                                currentSourceBufferSize;
                            voice.delayWritePos = 0;
                            voice.pitchPhase = 0.0;
                            voice.delayFeedback = 0.0f;      // Initialize feedback
                            voice.filterCutoffHz = 20000.0f; // Initialize filter (no filtering)
                            voice.filterResonance = 0.707f;
                            voice.filter.reset(); // Reset filter state

                            // Initialize new effect states
                            voice.reverbRoomSize = 0.0f;
                            voice.reverbDecay = 0.0f;
                            voice.reverbWritePos = 0;
                            voice.distortionDrive = 0.0f;
                            voice.distortionTone = 0.5f;
                            voice.chorusDelayMs = 0.0f;
                            voice.chorusDepth = 0.0f;
                            voice.chorusLfoPhase = 0.0f;
                            voice.chorusWritePos = 0;
                            voice.bitcrusherBits = 16.0f;
                            voice.bitcrusherDownsample = 1.0f;
                            voice.bitcrusherLastSampleL = 0.0f;
                            voice.bitcrusherLastSampleR = 0.0f;
                            voice.bitcrusherCounter = 0;
                            voice.tremoloRate = 0.0f;
                            voice.tremoloDepth = 0.0f;
                            voice.tremoloPhase = 0.0f;
                        }
                    }

                    // Update voice parameters from dot
                    const float pan = dot.x * 2.0f - 1.0f; // -1 (left) to +1 (right)
                    voice.panL = std::cos((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
                    voice.panR = std::sin((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);

                    // Apply color parameters
                    const float colorValue = getColorParameterValue(dot.color, dot.size);
                    switch (dot.color)
                    {
                    case ColorID::Red: // Delay - grid-based: X = delay time, Y = feedback
                    {
                        // X-axis: Delay time (left = short delay, right = long delay)
                        // Map X (0-1) to delay time (0-2000ms)
                        const float maxDelayMs = 2000.0f;
                        voice.delayTimeMs = dot.x * maxDelayMs;

                        // Y-axis: Delay feedback (bottom = no feedback, top = high feedback)
                        // Map Y (0-1) to feedback amount (0.0 = no feedback, 0.95 = high feedback)
                        voice.delayFeedback =
                            dot.y * 0.95f; // Cap at 0.95 to prevent infinite feedback

                        // Apply red amount scaling to both parameters
                        const float redAmount = redAmountParam ? redAmountParam->load() : 1.0f;
                        voice.delayTimeMs *= redAmount;
                        voice.delayFeedback *= redAmount;
                    }
                        voice.volume = 1.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    case ColorID::Green: // Filter - grid-based: X = cutoff, Y = resonance
                    {
                        // X-axis: Cutoff frequency (left = low, right = high)
                        // Map X (0-1) to cutoff frequency (20 Hz to 20000 Hz, logarithmic)
                        const float minCutoffHz = 20.0f;
                        const float maxCutoffHz = 20000.0f;
                        // Use logarithmic mapping for more musical control
                        voice.filterCutoffHz =
                            minCutoffHz * std::pow(maxCutoffHz / minCutoffHz, dot.x);

                        // Y-axis: Resonance/Q (bottom = low, top = high)
                        // Map Y (0-1) to resonance (0.707 = no resonance, 10.0 = high resonance)
                        voice.filterResonance = juce::jmap(dot.y, 0.707f, 10.0f);

                        // Apply green amount scaling to both parameters
                        const float greenAmount =
                            greenAmountParam ? greenAmountParam->load() : 1.0f;
                        // For cutoff: interpolate between max (no filter) and calculated cutoff
                        voice.filterCutoffHz =
                            juce::jmap(greenAmount, maxCutoffHz, voice.filterCutoffHz);
                        // For resonance: interpolate between 0.707 (no resonance) and calculated
                        // resonance
                        voice.filterResonance =
                            juce::jmap(greenAmount, 0.707f, voice.filterResonance);

                        // Update filter parameters
                        voice.filter.setCutoffFrequency(voice.filterCutoffHz);
                        voice.filter.setResonance(voice.filterResonance);
                    }
                        voice.volume = 1.0f; // Volume is now controlled by dot.size only
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    case ColorID::Blue: // Pitch - grid-based: bottom-left = low, top-right = high
                    {
                        // Calculate pitch from position: (x + y) / 2 maps bottom-left (0,0) to 0.0
                        // and top-right (1,1) to 1.0
                        const float pitchPosition =
                            (dot.x + dot.y) * 0.5f; // 0.0 (bottom-left) to 1.0 (top-right)
                        // Map to semitones: -24 (low) to +24 (high)
                        float pitchSemitones = juce::jmap(pitchPosition, -24.0f, 24.0f);
                        // Apply blue amount scaling
                        const float blueAmount = blueAmountParam ? blueAmountParam->load() : 1.0f;
                        pitchSemitones *= blueAmount;
                        // Convert to pitch ratio
                        voice.pitchRatio = std::pow(2.0, pitchSemitones / 12.0);
                    }
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        break;
                    case ColorID::Yellow: // Reverb - grid-based: X = room size, Y = decay
                    {
                        // X-axis: Room size (left = small, right = large)
                        voice.reverbRoomSize = dot.x; // 0-1
                        // Y-axis: Decay time (bottom = short, top = long)
                        voice.reverbDecay = dot.y; // 0-1
                        // Apply yellow amount scaling
                        const float yellowAmount =
                            yellowAmountParam ? yellowAmountParam->load() : 1.0f;
                        voice.reverbRoomSize *= yellowAmount;
                        voice.reverbDecay *= yellowAmount;
                    }
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    case ColorID::Cyan: // Distortion - grid-based: X = drive, Y = tone
                    {
                        // X-axis: Drive amount (left = clean, right = distorted)
                        voice.distortionDrive = dot.x; // 0-1
                        // Y-axis: Tone (bottom = dark, top = bright)
                        voice.distortionTone = dot.y; // 0-1
                        // Apply cyan amount scaling
                        const float cyanAmount = cyanAmountParam ? cyanAmountParam->load() : 1.0f;
                        voice.distortionDrive *= cyanAmount;
                        voice.distortionTone *= cyanAmount;
                    }
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    case ColorID::Magenta: // Chorus - grid-based: X = delay time, Y = depth
                    {
                        // X-axis: Delay time (left = short, right = long)
                        voice.chorusDelayMs = dot.x * 50.0f; // 0-50ms
                        // Y-axis: Modulation depth (bottom = shallow, top = deep)
                        voice.chorusDepth = dot.y; // 0-1
                        // Apply magenta amount scaling
                        const float magentaAmount =
                            magentaAmountParam ? magentaAmountParam->load() : 1.0f;
                        voice.chorusDelayMs *= magentaAmount;
                        voice.chorusDepth *= magentaAmount;
                        voice.chorusLfoPhase = 0.0f; // Reset LFO phase
                    }
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    case ColorID::Orange: // Bitcrusher - grid-based: X = bit depth, Y = downsample
                    {
                        // X-axis: Bit depth (left = low bits, right = high bits)
                        voice.bitcrusherBits = juce::jmap(dot.x, 1.0f, 16.0f); // 1-16 bits
                        // Y-axis: Downsample factor (bottom = no downsampling, top = heavy
                        // downsampling)
                        voice.bitcrusherDownsample =
                            juce::jmap(dot.y, 1.0f, 16.0f); // 1-16x downsampling
                        // Apply orange amount scaling
                        const float orangeAmount =
                            orangeAmountParam ? orangeAmountParam->load() : 1.0f;
                        // Interpolate between full quality (16 bits, 1x) and calculated values
                        voice.bitcrusherBits =
                            juce::jmap(orangeAmount, 16.0f, voice.bitcrusherBits);
                        voice.bitcrusherDownsample =
                            juce::jmap(orangeAmount, 1.0f, voice.bitcrusherDownsample);
                        voice.bitcrusherLastSampleL = 0.0f;
                        voice.bitcrusherLastSampleR = 0.0f;
                        voice.bitcrusherCounter = 0;
                    }
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    case ColorID::Purple: // Tremolo - grid-based: X = rate, Y = depth
                    {
                        // X-axis: Modulation rate (left = slow, right = fast)
                        voice.tremoloRate = dot.x * 10.0f; // 0-10 Hz
                        // Y-axis: Modulation depth (bottom = shallow, top = deep)
                        voice.tremoloDepth = dot.y; // 0-1
                        // Apply purple amount scaling
                        const float purpleAmount =
                            purpleAmountParam ? purpleAmountParam->load() : 1.0f;
                        voice.tremoloRate *= purpleAmount;
                        voice.tremoloDepth *= purpleAmount;
                        voice.tremoloPhase = 0.0f; // Reset LFO phase
                    }
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    default:
                        voice.volume = 1.0f;
                        voice.delayTimeMs = 0.0f;
                        voice.pitchRatio = 1.0;
                        break;
                    }

                    // Apply size to volume (bigger = more voice reproduction)
                    voice.volume *= dot.size;

                    penDotIndex++;
                }
            }

            // Deactivate voices that don't have corresponding dots
            for (int v = penDotIndex; v < (int)voicePool.size(); ++v)
            {
                voicePool[v].isActive = false;
            }
        }

        // 3. Process Spray tool dots (grain spawners) - once per block
        if (i == 0)
        {
            // Apply CV modulation to density
            float densityValue = baseDensity;
            if (densityModActive && densityCV)
            {
                const float cv01 =
                    juce::jlimit(0.0f, 1.0f, (densityCV[i] + 1.0f) * 0.5f); // Normalize CV to 0-1
                // Map CV (0-1) to density range (0.1-100 Hz, logarithmic)
                densityValue = juce::jmap(cv01, 0.1f, 100.0f);
            }
            else
            {
                densityValue = (sr > 0.0) ? smoothedDensity.getNextValue() : baseDensity;
            }
            const float currentDensity = densityValue;

            // Apply CV modulation to grain size
            float grainSizeValue = baseGrainSizeMs;
            if (grainSizeModActive && grainSizeCV)
            {
                const float cv01 =
                    juce::jlimit(0.0f, 1.0f, (grainSizeCV[i] + 1.0f) * 0.5f); // Normalize CV to 0-1
                // Map CV (0-1) to grain size range (5-500 ms)
                grainSizeValue = juce::jmap(cv01, 5.0f, 500.0f);
            }
            const float currentGrainSizeMs = grainSizeValue;

            // Update live parameter values for UI display - Use _live keys
            if (densityModActive)
                setLiveParamValue("density_live", currentDensity);
            if (grainSizeModActive)
                setLiveParamValue("grainSize_live", currentGrainSizeMs);

            // Spawn grains from each Spray dot
            int sprayDotIndex = 0;
            for (const auto& dot : currentDots)
            {
                if (dot.type == DotType::Spray)
                {
                    // Each dot has its own density phase
                    double& phase = dotDensityPhases[sprayDotIndex];
                    phase += currentDensity / sr * numSamples; // Advance for entire block

                    while (phase >= 1.0)
                    {
                        phase -= 1.0;

                        // Find free grain slot
                        for (int j = 0; j < (int)grainPool.size(); ++j)
                        {
                            if (!grainPool[j].isActive)
                            {
                                launchGrain(
                                    j, dot, sr, sourceWritePos, samplesWritten, currentGrainSizeMs);
                                break;
                            }
                        }
                    }

                    sprayDotIndex++;
                }
            }

            // Clean up phases for dots that no longer exist
            if (dotDensityPhases.size() > (size_t)sprayDotIndex)
            {
                auto it = dotDensityPhases.begin();
                std::advance(it, sprayDotIndex);
                dotDensityPhases.erase(it, dotDensityPhases.end());
            }
        }

        // 4. Process active grains
        if (currentSourceBufferSize > 0)
        {
            for (auto& grain : grainPool)
            {
                if (grain.isActive)
                {
                    int   readPosInt = (int)grain.readPosition;
                    float fraction = (float)(grain.readPosition - readPosInt);

                    // Wrap read position
                    readPosInt = (readPosInt % currentSourceBufferSize + currentSourceBufferSize) %
                                 currentSourceBufferSize;
                    int readPosNext = (readPosInt + 1) % currentSourceBufferSize;

                    // Linear interpolation
                    float sL = sourceBuffer.getSample(0, readPosInt) * (1.0f - fraction) +
                               sourceBuffer.getSample(0, readPosNext) * fraction;
                    float sR = sourceBuffer.getSample(1, readPosInt) * (1.0f - fraction) +
                               sourceBuffer.getSample(1, readPosNext) * fraction;

                    // Apply filter if needed (Green color) - simple one-pole lowpass for grains
                    if (grain.filterCutoffHz < 20000.0f)
                    {
                        // Calculate filter coefficient from cutoff frequency
                        const float omega =
                            2.0f * juce::MathConstants<float>::pi * grain.filterCutoffHz / sr;
                        const float alpha = std::sin(omega) / (1.0f + std::cos(omega));
                        const float a0 = 1.0f + alpha;
                        const float b1 = alpha / a0;
                        const float a1 = (1.0f - alpha) / a0;

                        // Apply one-pole lowpass filter
                        float filteredL = a1 * sL + b1 * grain.filterState;
                        grain.filterState = filteredL;
                        sL = filteredL;
                        sR = filteredL; // Mono filter for simplicity
                    }

                    // Apply Reverb if needed (Yellow color) - simplified for grains
                    if (grain.reverbRoomSize > 0.0f && !grain.reverbBuffer.empty())
                    {
                        const int reverbSize = (int)grain.reverbBuffer.size();
                        const int delaySamples = juce::jlimit(
                            0, reverbSize - 1, (int)(grain.reverbRoomSize * reverbSize * 0.5f));
                        const int readPos =
                            (grain.reverbWritePos - delaySamples + reverbSize) % reverbSize;

                        float reverb = grain.reverbBuffer[readPos] * grain.reverbDecay;
                        grain.reverbBuffer[grain.reverbWritePos] = sL + reverb;
                        grain.reverbWritePos = (grain.reverbWritePos + 1) % reverbSize;

                        sL = sL * (1.0f - grain.reverbRoomSize) + reverb * grain.reverbRoomSize;
                        sR = sL; // Mono reverb for grains
                    }

                    // Apply Distortion if needed (Cyan color)
                    if (grain.distortionDrive > 0.0f)
                    {
                        const float drive = grain.distortionDrive * 10.0f;
                        sL *= drive;
                        sR *= drive;
                        sL = std::tanh(sL);
                        sR = std::tanh(sR);
                        // Simple tone control
                        if (grain.distortionTone < 1.0f)
                        {
                            const float toneCutoff =
                                juce::jmap(grain.distortionTone, 2000.0f, 20000.0f);
                            const float alpha =
                                1.0f /
                                (1.0f + sr / (toneCutoff * 2.0f * juce::MathConstants<float>::pi));
                            sL = sL * (1.0f - alpha) + grain.filterState * alpha;
                            sR = sL;
                            grain.filterState = sL; // Reuse filterState for tone
                        }
                    }

                    // Apply Chorus if needed (Magenta color) - simplified for grains
                    if (grain.chorusDepth > 0.0f && !grain.chorusBuffer.empty())
                    {
                        const float lfoRate = 1.5f;
                        grain.chorusLfoPhase += lfoRate / sr;
                        if (grain.chorusLfoPhase > 1.0f)
                            grain.chorusLfoPhase -= 1.0f;

                        const float lfo =
                            std::sin(grain.chorusLfoPhase * 2.0f * juce::MathConstants<float>::pi);
                        const int chorusSize = (int)grain.chorusBuffer.size();
                        const int baseDelay =
                            juce::jmin((int)(grain.chorusDelayMs * sr / 1000.0f), chorusSize - 1);
                        const int modDelay = juce::jlimit(
                            0,
                            chorusSize - 1,
                            baseDelay + (int)(lfo * grain.chorusDepth * baseDelay * 0.5f));
                        const int readPos =
                            (grain.chorusWritePos - modDelay + chorusSize) % chorusSize;

                        float chorus = grain.chorusBuffer[readPos];
                        grain.chorusBuffer[grain.chorusWritePos] = sL;
                        grain.chorusWritePos = (grain.chorusWritePos + 1) % chorusSize;

                        sL = sL * 0.5f + chorus * 0.5f;
                        sR = sL;
                    }

                    // Apply Bitcrusher if needed (Orange color)
                    if (grain.bitcrusherBits < 16.0f || grain.bitcrusherDownsample > 1.0f)
                    {
                        grain.bitcrusherCounter++;
                        if (grain.bitcrusherCounter >= (int)grain.bitcrusherDownsample)
                        {
                            grain.bitcrusherCounter = 0;
                            const float quantizeLevels = std::pow(2.0f, grain.bitcrusherBits);
                            sL = std::floor(sL * quantizeLevels + 0.5f) / quantizeLevels;
                            sR = sL;
                            grain.bitcrusherLastSample = sL;
                        }
                        else
                        {
                            sL = grain.bitcrusherLastSample;
                            sR = sL;
                        }
                    }

                    // Apply Tremolo if needed (Purple color)
                    if (grain.tremoloRate > 0.0f && grain.tremoloDepth > 0.0f)
                    {
                        grain.tremoloPhase += grain.tremoloRate / sr;
                        if (grain.tremoloPhase > 1.0f)
                            grain.tremoloPhase -= 1.0f;

                        const float lfo = 0.5f + 0.5f * std::sin(
                                                            grain.tremoloPhase * 2.0f *
                                                            juce::MathConstants<float>::pi);
                        const float modAmount =
                            1.0f - grain.tremoloDepth + (lfo * grain.tremoloDepth);
                        sL *= modAmount;
                        sR *= modAmount;
                    }

                    // Update envelope
                    grain.envelope += grain.envelopeIncrement;
                    if (grain.envelope > 1.0f)
                        grain.envelope = 1.0f;

                    // Hann window envelope
                    float env =
                        0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * grain.envelope));

                    // Apply dynamic movement (vibrant grains)
                    grain.movementOffset += grain.movementVelocity;
                    grain.movementVelocity +=
                        (random.nextFloat() - 0.5f) * 0.001f; // Random acceleration
                    grain.movementVelocity *= 0.95f;          // Damping

                    // Occasional "pop" effect (sudden position jump)
                    if (random.nextInt(1000) < 2) // 0.2% chance per sample
                    {
                        grain.readPosition +=
                            (random.nextFloat() - 0.5f) * currentSourceBufferSize * 0.1f;
                    }

                    // Apply color parameters to grain
                    float grainVolume = grain.volume;

                    // Note: Delay for grains would require per-grain delay buffers which is complex
                    // For now, delay is primarily a Pen tool feature (static voices)
                    // Grains are short-lived, so delay effect is less meaningful

                    // Apply pitch if Blue color (already in increment)
                    // Apply volume if Green color (already in grain.volume)

                    // Size affects overall volume (bigger = more voice reproduction)
                    grainVolume *= grain.size;

                    // Envelope affects volume
                    grainVolume *= 0.5f + grain.envelope * 0.5f;

                    spraySampleL += sL * env * grain.panL * grainVolume;
                    spraySampleR += sR * env * grain.panR * grainVolume;

                    grain.readPosition += grain.increment;
                    grain.readPosition += grain.movementOffset * 0.01; // Apply movement

                    // Wrap read position
                    while (grain.readPosition < 0.0)
                        grain.readPosition += currentSourceBufferSize;
                    while (grain.readPosition >= currentSourceBufferSize)
                        grain.readPosition -= currentSourceBufferSize;

                    grain.samplesRemaining--;
                    if (grain.samplesRemaining <= 0)
                    {
                        grain.isActive = false;
                    }
                }
            }
        }

        // 5. Process active voices (Pen tool)
        if (currentSourceBufferSize > 0)
        {
            for (auto& voice : voicePool)
            {
                if (voice.isActive)
                {
                    // Apply delay if needed (Red color)
                    float sL = 0.0f, sR = 0.0f;

                    if (voice.delayTimeMs > 0.0f && !voice.delayBuffer.empty())
                    {
                        // Simple delay line
                        const int delaySamples = (int)(voice.delayTimeMs * sr / 1000.0f);
                        const int delayBufferSize = (int)voice.delayBuffer.size();
                        const int delayReadPos =
                            (voice.delayWritePos - delaySamples + delayBufferSize) %
                            delayBufferSize;

                        // Read from delay buffer
                        sL = voice.delayBuffer[delayReadPos];
                        sR = sL; // Mono delay for simplicity

                        // Write current sample + feedback to delay buffer
                        int   readPosInt = (int)voice.readPosition;
                        float fraction = (float)(voice.readPosition - readPosInt);
                        readPosInt =
                            (readPosInt % currentSourceBufferSize + currentSourceBufferSize) %
                            currentSourceBufferSize;
                        int readPosNext = (readPosInt + 1) % currentSourceBufferSize;

                        float inputL = sourceBuffer.getSample(0, readPosInt) * (1.0f - fraction) +
                                       sourceBuffer.getSample(0, readPosNext) * fraction;
                        // Add feedback: mix input with delayed signal
                        voice.delayBuffer[voice.delayWritePos] =
                            inputL + (sL * voice.delayFeedback);
                        voice.delayWritePos = (voice.delayWritePos + 1) % delayBufferSize;
                    }
                    else
                    {
                        // No delay, read directly from buffer
                        int   readPosInt = (int)voice.readPosition;
                        float fraction = (float)(voice.readPosition - readPosInt);

                        readPosInt =
                            (readPosInt % currentSourceBufferSize + currentSourceBufferSize) %
                            currentSourceBufferSize;
                        int readPosNext = (readPosInt + 1) % currentSourceBufferSize;

                        sL = sourceBuffer.getSample(0, readPosInt) * (1.0f - fraction) +
                             sourceBuffer.getSample(0, readPosNext) * fraction;
                        sR = sourceBuffer.getSample(1, readPosInt) * (1.0f - fraction) +
                             sourceBuffer.getSample(1, readPosNext) * fraction;
                    }

                    // Apply filter if needed (Green color)
                    if (voice.filterCutoffHz < 20000.0f)
                    {
                        // Update filter parameters (they may have changed)
                        voice.filter.setCutoffFrequency(voice.filterCutoffHz);
                        voice.filter.setResonance(voice.filterResonance);

                        // Process through filter (mono processing for simplicity)
                        juce::AudioBuffer<float> filterBuffer(1, 1);
                        filterBuffer.setSample(0, 0, sL);
                        juce::dsp::AudioBlock<float>              block(filterBuffer);
                        juce::dsp::ProcessContextReplacing<float> context(block);
                        voice.filter.process(context);
                        sL = filterBuffer.getSample(0, 0);
                        sR = sL; // Mono filter for simplicity
                    }

                    // Apply pitch shift if needed (Blue color)
                    if (voice.pitchRatio != 1.0 && !voice.pitchBuffer.empty())
                    {
                        // Use pitch buffer as a simple resampling buffer
                        // Store current sample
                        const int pitchBufferSize = (int)voice.pitchBuffer.size();
                        voice.pitchBuffer[(int)voice.pitchPhase % pitchBufferSize] = sL;

                        // Read from pitch buffer at phase position
                        float phaseFrac = (float)(voice.pitchPhase - (int)voice.pitchPhase);
                        int   phaseInt = (int)voice.pitchPhase % pitchBufferSize;
                        int   phaseNext = (phaseInt + 1) % pitchBufferSize;

                        sL = voice.pitchBuffer[phaseInt] * (1.0f - phaseFrac) +
                             voice.pitchBuffer[phaseNext] * phaseFrac;
                        sR = sL; // Mono pitch shift for simplicity

                        // Advance phase
                        voice.pitchPhase += voice.pitchRatio;
                        if (voice.pitchPhase >= pitchBufferSize)
                            voice.pitchPhase -= pitchBufferSize;
                    }

                    // Apply Reverb if needed (Yellow color)
                    if (voice.reverbRoomSize > 0.0f && !voice.reverbBufferL.empty())
                    {
                        const int reverbSize = (int)voice.reverbBufferL.size();
                        const int delaySamples = juce::jlimit(
                            0, reverbSize - 1, (int)(voice.reverbRoomSize * reverbSize * 0.5f));
                        const int readPos =
                            (voice.reverbWritePos - delaySamples + reverbSize) % reverbSize;

                        float reverbL = voice.reverbBufferL[readPos] * voice.reverbDecay;
                        float reverbR = voice.reverbBufferR[readPos] * voice.reverbDecay;

                        voice.reverbBufferL[voice.reverbWritePos] = sL + reverbL;
                        voice.reverbBufferR[voice.reverbWritePos] = sR + reverbR;
                        voice.reverbWritePos = (voice.reverbWritePos + 1) % reverbSize;

                        sL = sL * (1.0f - voice.reverbRoomSize) + reverbL * voice.reverbRoomSize;
                        sR = sR * (1.0f - voice.reverbRoomSize) + reverbR * voice.reverbRoomSize;
                    }

                    // Apply Distortion if needed (Cyan color)
                    if (voice.distortionDrive > 0.0f)
                    {
                        // Soft clipping distortion
                        const float drive = voice.distortionDrive * 10.0f; // 0-10x gain
                        sL *= drive;
                        sR *= drive;
                        // Soft clip: tanh
                        sL = std::tanh(sL);
                        sR = std::tanh(sR);
                        // Tone control using IIR filter
                        if (voice.distortionTone < 1.0f)
                        {
                            const float toneCutoff =
                                juce::jmap(voice.distortionTone, 2000.0f, 20000.0f);
                            // Update filter coefficients if needed
                            voice.distortionToneFilter.coefficients =
                                juce::dsp::IIR::Coefficients<float>::makeLowPass(sr, toneCutoff);
                            // Process through filter
                            juce::AudioBuffer<float> filterBuf(1, 1);
                            filterBuf.setSample(0, 0, sL);
                            juce::dsp::AudioBlock<float>              block(filterBuf);
                            juce::dsp::ProcessContextReplacing<float> context(block);
                            voice.distortionToneFilter.process(context);
                            sL = filterBuf.getSample(0, 0);
                            // Process right channel
                            filterBuf.setSample(0, 0, sR);
                            voice.distortionToneFilter.process(context);
                            sR = filterBuf.getSample(0, 0);
                        }
                    }

                    // Apply Chorus if needed (Magenta color)
                    if (voice.chorusDepth > 0.0f && !voice.chorusBufferL.empty())
                    {
                        const float lfoRate = 1.5f; // Fixed LFO rate in Hz
                        voice.chorusLfoPhase += lfoRate / sr;
                        if (voice.chorusLfoPhase > 1.0f)
                            voice.chorusLfoPhase -= 1.0f;

                        const float lfo =
                            std::sin(voice.chorusLfoPhase * 2.0f * juce::MathConstants<float>::pi);
                        const int chorusSize = (int)voice.chorusBufferL.size();
                        const int baseDelay =
                            juce::jmin((int)(voice.chorusDelayMs * sr / 1000.0f), chorusSize - 1);
                        const int modDelay = juce::jlimit(
                            0,
                            chorusSize - 1,
                            baseDelay + (int)(lfo * voice.chorusDepth * baseDelay * 0.5f));
                        const int readPos =
                            (voice.chorusWritePos - modDelay + chorusSize) % chorusSize;

                        float chorusL = voice.chorusBufferL[readPos];
                        float chorusR = voice.chorusBufferR[readPos];

                        voice.chorusBufferL[voice.chorusWritePos] = sL;
                        voice.chorusBufferR[voice.chorusWritePos] = sR;
                        voice.chorusWritePos = (voice.chorusWritePos + 1) % chorusSize;

                        sL = sL * 0.5f + chorusL * 0.5f;
                        sR = sR * 0.5f + chorusR * 0.5f;
                    }

                    // Apply Bitcrusher if needed (Orange color)
                    if (voice.bitcrusherBits < 16.0f || voice.bitcrusherDownsample > 1.0f)
                    {
                        voice.bitcrusherCounter++;
                        if (voice.bitcrusherCounter >= (int)voice.bitcrusherDownsample)
                        {
                            voice.bitcrusherCounter = 0;
                            // Quantize
                            const float quantizeLevels = std::pow(2.0f, voice.bitcrusherBits);
                            sL = std::floor(sL * quantizeLevels + 0.5f) / quantizeLevels;
                            sR = std::floor(sR * quantizeLevels + 0.5f) / quantizeLevels;
                            voice.bitcrusherLastSampleL = sL;
                            voice.bitcrusherLastSampleR = sR;
                        }
                        else
                        {
                            // Hold last sample (downsampling)
                            sL = voice.bitcrusherLastSampleL;
                            sR = voice.bitcrusherLastSampleR;
                        }
                    }

                    // Apply Tremolo if needed (Purple color)
                    if (voice.tremoloRate > 0.0f && voice.tremoloDepth > 0.0f)
                    {
                        voice.tremoloPhase += voice.tremoloRate / sr;
                        if (voice.tremoloPhase > 1.0f)
                            voice.tremoloPhase -= 1.0f;

                        const float lfo = 0.5f + 0.5f * std::sin(
                                                            voice.tremoloPhase * 2.0f *
                                                            juce::MathConstants<float>::pi);
                        const float modAmount =
                            1.0f - voice.tremoloDepth + (lfo * voice.tremoloDepth);
                        sL *= modAmount;
                        sR *= modAmount;
                    }

                    // Apply volume and panning
                    sL *= voice.volume * voice.panL;
                    sR *= voice.volume * voice.panR;

                    penSampleL += sL;
                    penSampleR += sR;

                    // Advance read position (continuous playback with pitch ratio)
                    voice.readPosition += voice.pitchRatio;

                    // Wrap read position
                    while (voice.readPosition >= currentSourceBufferSize)
                        voice.readPosition -= currentSourceBufferSize;
                }
            }
        }

        // 6. Mix wet/dry separately for Pen and Spray, then combine
        // Apply CV modulation to dry mix
        float dryMixValue = baseDryMix;
        if (dryMixModActive && dryMixCV)
        {
            const float cv01 =
                juce::jlimit(0.0f, 1.0f, (dryMixCV[i] + 1.0f) * 0.5f); // Normalize CV to 0-1
            dryMixValue = cv01;
        }
        else
        {
            dryMixValue = (sr > 0.0 && dryMixParam) ? smoothedDryMix.getNextValue() : baseDryMix;
        }
        const float currentDryMix = dryMixValue;

        // Apply CV modulation to pen mix
        float penMixValue = basePenMix;
        if (penMixModActive && penMixCV)
        {
            const float cv01 =
                juce::jlimit(0.0f, 1.0f, (penMixCV[i] + 1.0f) * 0.5f); // Normalize CV to 0-1
            penMixValue = cv01;
        }
        else
        {
            penMixValue = (sr > 0.0 && penMixParam) ? smoothedPenMix.getNextValue() : basePenMix;
        }
        const float currentPenMix = penMixValue;

        // Apply CV modulation to spray mix
        float sprayMixValue = baseSprayMix;
        if (sprayMixModActive && sprayMixCV)
        {
            const float cv01 =
                juce::jlimit(0.0f, 1.0f, (sprayMixCV[i] + 1.0f) * 0.5f); // Normalize CV to 0-1
            sprayMixValue = cv01;
        }
        else
        {
            sprayMixValue =
                (sr > 0.0 && sprayMixParam) ? smoothedSprayMix.getNextValue() : baseSprayMix;
        }
        const float currentSprayMix = sprayMixValue;

        // Update live parameter values for UI display (throttled) - Use _live keys
        if ((i & 0x3F) == 0) // Every 64 samples
        {
            if (dryMixModActive)
                setLiveParamValue("dryMix_live", currentDryMix);
            if (penMixModActive)
                setLiveParamValue("penMix_live", currentPenMix);
            if (sprayMixModActive)
                setLiveParamValue("sprayMix_live", currentSprayMix);
        }

        const float dryL = inBus.getNumChannels() > 0 ? inBus.getSample(0, i) : 0.0f;
        const float dryR = inBus.getNumChannels() > 1 ? inBus.getSample(1, i) : 0.0f;

        // Mix: Dry signal * Dry Mix + Pen wet * Pen Mix + Spray wet * Spray Mix
        const float outL = (dryL * currentDryMix) + (penSampleL * currentPenMix) +
                           (spraySampleL * currentSprayMix);
        const float outR = (dryR * currentDryMix) + (penSampleR * currentPenMix) +
                           (spraySampleR * currentSprayMix);
        outBus.setSample(0, i, outL);
        outBus.setSample(1, i, outR);

#if defined(PRESET_CREATOR_UI)
        // Capture output audio for visualization
        if (vizOutputBuffer.getNumSamples() > 0)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            // Store mono mix of stereo output
            vizOutputBuffer.setSample(0, writeIdx, 0.5f * (outL + outR));
        }
#endif
    }

#if defined(PRESET_CREATOR_UI)
    // Update visualization data (thread-safe)
    vizWritePos = (vizWritePos + numSamples) % vizBufferSize;

    // Downsample waveform from circular buffer
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx =
            (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) %
            vizBufferSize;
        if (vizOutputBuffer.getNumSamples() > 0)
            vizData.outputWaveform[i].store(vizOutputBuffer.getSample(0, readIdx));
    }

    // Count active voices and grains
    int activeVoicesCount = 0;
    for (const auto& voice : voicePool)
    {
        if (voice.isActive)
            activeVoicesCount++;
    }
    int activeGrainsCount = 0;
    for (const auto& grain : grainPool)
    {
        if (grain.isActive)
            activeGrainsCount++;
    }
    vizData.activeVoices.store(activeVoicesCount);
    vizData.activeGrains.store(activeGrainsCount);

    // Calculate buffer fill level (0-1)
    const float bufferFill =
        currentSourceBufferSize > 0
            ? juce::jlimit(0.0f, 1.0f, (float)samplesWritten / (float)currentSourceBufferSize)
            : 0.0f;
    vizData.bufferFillLevel.store(bufferFill);

    // Calculate output level (RMS)
    float outputPeak = 0.0f;
    for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
        outputPeak = juce::jmax(outputPeak, outBus.getRMSLevel(ch, 0, numSamples));
    vizData.outputLevel.store(juce::Decibels::gainToDecibels(outputPeak, -60.0f));
#endif

    // Update output values for visualization
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(outBus.getSample(0, numSamples - 1));
        if (lastOutputValues[1])
            lastOutputValues[1]->store(outBus.getSample(1, numSamples - 1));
    }
}

void SpatialGranulatorModuleProcessor::processPenVoice(
    const Dot&                dot,
    juce::AudioBuffer<float>& buffer,
    int                       numSamples,
    double                    sampleRate)
{
    // Find or activate a voice for this dot
    // For simplicity, we'll process voices in processBlock directly
    // This is a placeholder - actual voice activation happens in processBlock
    juce::ignoreUnused(dot, buffer, numSamples, sampleRate);
}

void SpatialGranulatorModuleProcessor::processSprayGrains(
    const Dot&                dot,
    juce::AudioBuffer<float>& buffer,
    int                       numSamples,
    double                    sampleRate)
{
    // Grain spawning is handled in processBlock
    // This is a placeholder
    juce::ignoreUnused(dot, buffer, numSamples, sampleRate);
}

void SpatialGranulatorModuleProcessor::launchGrain(
    int        grainIndex,
    const Dot& dot,
    double     sampleRate,
    int        currentWritePos,
    int        currentSamplesWritten,
    float      grainSizeMs)
{
    if (grainIndex < 0 || grainIndex >= (int)grainPool.size())
    {
        return;
    }

    auto&     grain = grainPool[grainIndex];
    const int bufferSize = sourceBuffer.getNumSamples();

    // Safety check: don't launch grain if buffer not ready
    if (bufferSize <= 0 || sampleRate <= 0.0)
    {
        grain.isActive = false;
        return;
    }

    grain.totalLifetime = grain.samplesRemaining = (int)((grainSizeMs / 1000.0f) * sampleRate);
    if (grain.samplesRemaining == 0)
    {
        grain.isActive = false;
        return;
    }

    // Calculate read position from dot's Y position (buffer position)
    // Read position should be relative to write position (reading from the past)
    const float bufferPos = dot.y; // 0 = recent, 1 = older
    // Ensure we only read from positions that have been written to
    const int maxReadableOffset = juce::jmin(currentSamplesWritten - 1, bufferSize - 1);
    const int offset = maxReadableOffset > 0 ? (int)(bufferPos * maxReadableOffset * 0.9f) : 0;
    grain.readPosition = (currentWritePos - offset + bufferSize) % bufferSize;

    // Add some randomness for texture
    if (bufferSize > 20)
    {
        grain.readPosition += random.nextInt(bufferSize / 20) - (bufferSize / 40);
    }
    while (grain.readPosition < 0)
        grain.readPosition += bufferSize;
    while (grain.readPosition >= bufferSize)
        grain.readPosition -= bufferSize;

    // Calculate pan from dot's X position
    const float pan = dot.x * 2.0f - 1.0f; // -1 (left) to +1 (right)
    grain.panL = std::cos((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
    grain.panR = std::sin((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);

    // Store color parameters in grain
    grain.color = dot.color;
    grain.size = dot.size;

    // Calculate color parameter values
    const float colorValue = getColorParameterValue(dot.color, dot.size);
    switch (dot.color)
    {
    case ColorID::Red: // Delay - grid-based: X = delay time, Y = feedback
    {
        // X-axis: Delay time (left = short delay, right = long delay)
        // Map X (0-1) to delay time (0-2000ms)
        const float maxDelayMs = 2000.0f;
        grain.delayTimeMs = dot.x * maxDelayMs;

        // Y-axis: Delay feedback (bottom = no feedback, top = high feedback)
        // Map Y (0-1) to feedback amount (0.0 = no feedback, 0.95 = high feedback)
        grain.delayFeedback = dot.y * 0.95f; // Cap at 0.95 to prevent infinite feedback

        // Apply red amount scaling to both parameters
        const float redAmount = redAmountParam ? redAmountParam->load() : 1.0f;
        grain.delayTimeMs *= redAmount;
        grain.delayFeedback *= redAmount;
    }
        grain.volume = 1.0f;
        grain.pitchOffset = 0.0f;
        break;
    case ColorID::Green: // Filter - grid-based: X = cutoff, Y = resonance
    {
        // X-axis: Cutoff frequency (left = low, right = high)
        // Map X (0-1) to cutoff frequency (20 Hz to 20000 Hz, logarithmic)
        const float minCutoffHz = 20.0f;
        const float maxCutoffHz = 20000.0f;
        // Use logarithmic mapping for more musical control
        grain.filterCutoffHz = minCutoffHz * std::pow(maxCutoffHz / minCutoffHz, dot.x);

        // Y-axis: Resonance/Q (bottom = low, top = high)
        // Map Y (0-1) to resonance (0.707 = no resonance, 10.0 = high resonance)
        grain.filterResonance = juce::jmap(dot.y, 0.707f, 10.0f);

        // Apply green amount scaling to both parameters
        const float greenAmount = greenAmountParam ? greenAmountParam->load() : 1.0f;
        // For cutoff: interpolate between max (no filter) and calculated cutoff
        grain.filterCutoffHz = juce::jmap(greenAmount, maxCutoffHz, grain.filterCutoffHz);
        // For resonance: interpolate between 0.707 (no resonance) and calculated resonance
        grain.filterResonance = juce::jmap(greenAmount, 0.707f, grain.filterResonance);
        // Initialize filter state
        grain.filterState = 0.0f;
    }
        grain.volume = 1.0f; // Volume is now controlled by dot.size only
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    case ColorID::Blue: // Pitch - grid-based: bottom-left = low, top-right = high
    {
        // Calculate pitch from position: (x + y) / 2 maps bottom-left (0,0) to 0.0 and top-right
        // (1,1) to 1.0
        const float pitchPosition = (dot.x + dot.y) * 0.5f; // 0.0 (bottom-left) to 1.0 (top-right)
        // Map to semitones: -24 (low) to +24 (high)
        float pitchSemitones = juce::jmap(pitchPosition, -24.0f, 24.0f);
        // Apply blue amount scaling
        const float blueAmount = blueAmountParam ? blueAmountParam->load() : 1.0f;
        pitchSemitones *= blueAmount;
        grain.pitchOffset = pitchSemitones;
    }
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        break;
    case ColorID::Yellow: // Reverb - grid-based: X = room size, Y = decay
    {
        grain.reverbRoomSize = dot.x; // 0-1
        grain.reverbDecay = dot.y;    // 0-1
        const float yellowAmount = yellowAmountParam ? yellowAmountParam->load() : 1.0f;
        grain.reverbRoomSize *= yellowAmount;
        grain.reverbDecay *= yellowAmount;
    }
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    case ColorID::Cyan: // Distortion - grid-based: X = drive, Y = tone
    {
        grain.distortionDrive = dot.x; // 0-1
        grain.distortionTone = dot.y;  // 0-1
        const float cyanAmount = cyanAmountParam ? cyanAmountParam->load() : 1.0f;
        grain.distortionDrive *= cyanAmount;
        grain.distortionTone *= cyanAmount;
    }
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    case ColorID::Magenta: // Chorus - grid-based: X = delay time, Y = depth
    {
        grain.chorusDelayMs = dot.x * 50.0f; // 0-50ms
        grain.chorusDepth = dot.y;           // 0-1
        const float magentaAmount = magentaAmountParam ? magentaAmountParam->load() : 1.0f;
        grain.chorusDelayMs *= magentaAmount;
        grain.chorusDepth *= magentaAmount;
        grain.chorusLfoPhase = 0.0f;
    }
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    case ColorID::Orange: // Bitcrusher - grid-based: X = bit depth, Y = downsample
    {
        grain.bitcrusherBits = juce::jmap(dot.x, 1.0f, 16.0f);       // 1-16 bits
        grain.bitcrusherDownsample = juce::jmap(dot.y, 1.0f, 16.0f); // 1-16x
        const float orangeAmount = orangeAmountParam ? orangeAmountParam->load() : 1.0f;
        grain.bitcrusherBits = juce::jmap(orangeAmount, 16.0f, grain.bitcrusherBits);
        grain.bitcrusherDownsample = juce::jmap(orangeAmount, 1.0f, grain.bitcrusherDownsample);
        grain.bitcrusherLastSample = 0.0f;
        grain.bitcrusherCounter = 0;
    }
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    case ColorID::Purple: // Tremolo - grid-based: X = rate, Y = depth
    {
        grain.tremoloRate = dot.x * 10.0f; // 0-10 Hz
        grain.tremoloDepth = dot.y;        // 0-1
        const float purpleAmount = purpleAmountParam ? purpleAmountParam->load() : 1.0f;
        grain.tremoloRate *= purpleAmount;
        grain.tremoloDepth *= purpleAmount;
        grain.tremoloPhase = 0.0f;
    }
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    default:
        grain.volume = 1.0f;
        grain.delayTimeMs = 0.0f;
        grain.pitchOffset = 0.0f;
        break;
    }

    // Apply pitch offset
    grain.increment = std::pow(2.0, grain.pitchOffset / 12.0);

    // Envelope setup
    grain.envelope = 0.0f;
    grain.envelopeIncrement = 1.0f / (float)grain.totalLifetime;

    // Dynamic movement setup
    grain.movementOffset = 0.0f;
    grain.movementVelocity = (random.nextFloat() - 0.5f) * 0.1f;

    // Initialize new effect states
    grain.reverbRoomSize = 0.0f;
    grain.reverbDecay = 0.0f;
    grain.reverbWritePos = 0;
    grain.distortionDrive = 0.0f;
    grain.distortionTone = 0.5f;
    grain.chorusDelayMs = 0.0f;
    grain.chorusDepth = 0.0f;
    grain.chorusLfoPhase = 0.0f;
    grain.chorusWritePos = 0;
    grain.bitcrusherBits = 16.0f;
    grain.bitcrusherDownsample = 1.0f;
    grain.bitcrusherLastSample = 0.0f;
    grain.bitcrusherCounter = 0;
    grain.tremoloRate = 0.0f;
    grain.tremoloDepth = 0.0f;
    grain.tremoloPhase = 0.0f;

    grain.isActive = true;
}

float SpatialGranulatorModuleProcessor::getColorParameterValue(ColorID color, float size) const
{
    const auto mapping = ColorParameterMapping::getMapping(color);
    if (mapping.paramType == ColorParameterMapping::ParameterType::None)
        return 0.0f;

    // Size (0-1) maps to parameter range
    float baseValue = juce::jmap(size, mapping.minValue, mapping.maxValue);

    // Apply color amount scaling
    float amount = 1.0f;
    switch (color)
    {
    case ColorID::Red:
        amount = redAmountParam ? redAmountParam->load() : 1.0f;
        break;
    case ColorID::Green:
        amount = greenAmountParam ? greenAmountParam->load() : 1.0f;
        break;
    case ColorID::Blue:
        amount = blueAmountParam ? blueAmountParam->load() : 1.0f;
        break;
    case ColorID::Yellow:
        amount = yellowAmountParam ? yellowAmountParam->load() : 1.0f;
        break;
    case ColorID::Cyan:
        amount = cyanAmountParam ? cyanAmountParam->load() : 1.0f;
        break;
    case ColorID::Magenta:
        amount = magentaAmountParam ? magentaAmountParam->load() : 1.0f;
        break;
    case ColorID::Orange:
        amount = orangeAmountParam ? orangeAmountParam->load() : 1.0f;
        break;
    case ColorID::Purple:
        amount = purpleAmountParam ? purpleAmountParam->load() : 1.0f;
        break;
    default:
        break;
    }

    // Scale the parameter value by the amount (0 = no effect, 1 = full effect)
    return baseValue * amount;
}

bool SpatialGranulatorModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All inputs are on bus 0
    // ch0-1: Audio (stereo), ch2-6: CV modulations
    // Use _mod IDs for routing (virtual IDs, not APVTS parameters)
    if (paramId == paramIdDryMixMod)
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdPenMixMod)
    {
        outChannelIndexInBus = 3;
        return true;
    }
    if (paramId == paramIdSprayMixMod)
    {
        outChannelIndexInBus = 4;
        return true;
    }
    if (paramId == paramIdDensityMod)
    {
        outChannelIndexInBus = 5;
        return true;
    }
    if (paramId == paramIdGrainSizeMod)
    {
        outChannelIndexInBus = 6;
        return true;
    }
    return false;
}

std::vector<DynamicPinInfo> SpatialGranulatorModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});

    // CV modulation inputs (channels 2-6)
    pins.push_back({"Dry Mix Mod", 2, PinDataType::CV});
    pins.push_back({"Pen Mix Mod", 3, PinDataType::CV});
    pins.push_back({"Spray Mix Mod", 4, PinDataType::CV});
    pins.push_back({"Density Mod", 5, PinDataType::CV});
    pins.push_back({"Grain Size Mod", 6, PinDataType::CV});

    return pins;
}

juce::String SpatialGranulatorModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
    case 0:
        return "In L";
    case 1:
        return "In R";
    case 2:
        return "Dry Mix Mod";
    case 3:
        return "Pen Mix Mod";
    case 4:
        return "Spray Mix Mod";
    case 5:
        return "Density Mod";
    case 6:
        return "Grain Size Mod";
    default:
        return {};
    }
}

juce::String SpatialGranulatorModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel)
    {
    case 0:
        return "Out L";
    case 1:
        return "Out R";
    default:
        return {};
    }
}

#if defined(PRESET_CREATOR_UI)

// Helper function for tooltip with help marker
static void HelpMarker(const char* desc)
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
}

void SpatialGranulatorModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{

    auto&       ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // Global parameters
    const bool dryMixMod = isParamModulated(paramIdDryMixMod);
    float      dryMix =
        dryMixMod ? getLiveParamValueFor(
                        paramIdDryMixMod, "dryMix_live", dryMixParam ? dryMixParam->load() : 1.0f)
                       : (dryMixParam ? dryMixParam->load() : 1.0f);

    if (dryMixMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); // Cyan
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }

    if (dryMixMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Dry Mix", &dryMix, 0.0f, 1.0f, "%.2f"))
    {
        if (!dryMixMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDryMix)) = dryMix;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !dryMixMod)
        onModificationEnded();
    if (!dryMixMod)
        adjustParamOnWheel(ap.getParameter(paramIdDryMix), paramIdDryMix, dryMix);
    if (dryMixMod)
        ImGui::EndDisabled();

    if (dryMixMod)
    {
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ThemeText("(CV)", theme.text.active);
    }
    HelpMarker(
        "Controls the level of the original (dry) input signal.\n0 = no original signal, 1 = full "
        "original signal.\nUse this to reduce the dry signal when you want more processed sound.");

    const bool penMixMod = isParamModulated(paramIdPenMixMod);
    float      penMix =
        penMixMod ? getLiveParamValueFor(
                        paramIdPenMixMod, "penMix_live", penMixParam ? penMixParam->load() : 0.5f)
                       : (penMixParam ? penMixParam->load() : 0.5f);

    if (penMixMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); // Cyan
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }

    if (penMixMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Pen Mix", &penMix, 0.0f, 1.0f, "%.2f"))
    {
        if (!penMixMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPenMix)) = penMix;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !penMixMod)
        onModificationEnded();
    if (!penMixMod)
        adjustParamOnWheel(ap.getParameter(paramIdPenMix), paramIdPenMix, penMix);
    if (penMixMod)
        ImGui::EndDisabled();

    if (penMixMod)
    {
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ThemeText("(CV)", theme.text.active);
    }
    HelpMarker(
        "Wet/dry mix for Pen tool voices (chorus-like continuous playback).\n0 = dry, 1 = wet.");

    const bool sprayMixMod = isParamModulated(paramIdSprayMixMod);
    float      sprayMix =
        sprayMixMod
                 ? getLiveParamValueFor(
                  paramIdSprayMixMod, "sprayMix_live", sprayMixParam ? sprayMixParam->load() : 0.5f)
                 : (sprayMixParam ? sprayMixParam->load() : 0.5f);

    if (sprayMixMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); // Cyan
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }

    if (sprayMixMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Spray Mix", &sprayMix, 0.0f, 1.0f, "%.2f"))
    {
        if (!sprayMixMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdSprayMix)) = sprayMix;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !sprayMixMod)
        onModificationEnded();
    if (!sprayMixMod)
        adjustParamOnWheel(ap.getParameter(paramIdSprayMix), paramIdSprayMix, sprayMix);
    if (sprayMixMod)
        ImGui::EndDisabled();

    if (sprayMixMod)
    {
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ThemeText("(CV)", theme.text.active);
    }
    HelpMarker(
        "Wet/dry mix for Spray tool grains (dynamic granular synthesis).\n0 = dry, 1 = wet.");

    const bool densityMod = isParamModulated(paramIdDensityMod);
    float      density =
        densityMod
                 ? getLiveParamValueFor(
                  paramIdDensityMod, "density_live", densityParam ? densityParam->load() : 10.0f)
                 : (densityParam ? densityParam->load() : 10.0f);

    if (densityMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); // Cyan
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
    }

    if (densityMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat(
            "Density", &density, 0.1f, 100.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic))
    {
        if (!densityMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDensity)) = density;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !densityMod)
        onModificationEnded();
    if (!densityMod)
        adjustParamOnWheel(ap.getParameter(paramIdDensity), paramIdDensity, density);
    if (densityMod)
        ImGui::EndDisabled();

    if (densityMod)
    {
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ThemeText("(CV)", theme.text.active);
    }
    HelpMarker("Grain spawning rate for Spray tool dots.\nHigher = more grains per second.");

    const bool grainSizeMod = isParamModulated(paramIdGrainSizeMod);
    float      grainSize = grainSizeMod ? getLiveParamValueFor(
                                         paramIdGrainSizeMod,
                                         "grainSize_live",
                                         grainSizeParam ? grainSizeParam->load() : 100.0f)
                                        : (grainSizeParam ? grainSizeParam->load() : 100.0f);

    if (grainSizeMod)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); // Cyan
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 1.0f));
    }

    if (grainSizeMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Grain Size", &grainSize, 5.0f, 500.0f, "%.1f ms"))
    {
        if (!grainSizeMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdGrainSize)) =
                grainSize;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !grainSizeMod)
        onModificationEnded();
    if (!grainSizeMod)
        adjustParamOnWheel(ap.getParameter(paramIdGrainSize), paramIdGrainSize, grainSize);
    if (grainSizeMod)
        ImGui::EndDisabled();

    if (grainSizeMod)
    {
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ThemeText("(CV)", theme.text.active);
    }
    HelpMarker(
        "Length of each grain spawned by Spray tool dots.\nSmaller = rhythmic, larger = smooth "
        "textures.");

    float bufferLength = bufferLengthParam ? bufferLengthParam->load() : 2.0f;
    if (ImGui::SliderFloat("Buffer Length", &bufferLength, 1.0f, 10.0f, "%.1f s"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdBufferLength)) =
            bufferLength;
    }
    adjustParamOnWheel(ap.getParameter(paramIdBufferLength), paramIdBufferLength, bufferLength);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Length of the circular buffer for recording input audio.\nLonger = more history, more "
        "memory.");

    ImGui::Separator();
    ImGui::Text("Color Amounts");

    float redAmount = redAmountParam ? redAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.8f, 0.2f, 0.2f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.9f, 0.3f, 0.3f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 0.4f, 0.4f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
    if (ImGui::SliderFloat("Delay Amount", &redAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdRedAmount)) = redAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdRedAmount), paramIdRedAmount, redAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Delay effect.\n0 = no delay, 1 = full delay range (0-2000ms "
        "delay time, 0-0.95 feedback).");

    float greenAmount = greenAmountParam ? greenAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.8f, 0.2f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.3f, 0.9f, 0.3f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.4f, 1.0f, 0.4f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
    if (ImGui::SliderFloat("Filter Amount", &greenAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdGreenAmount)) =
            greenAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdGreenAmount), paramIdGreenAmount, greenAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Filter effect.\n0 = no filtering, 1 = full filter "
        "range.\nX-axis = Cutoff frequency (left=20Hz, right=20kHz)\nY-axis = Resonance "
        "(bottom=0.707, top=10.0)");

    float blueAmount = blueAmountParam ? blueAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.8f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.3f, 0.3f, 0.9f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.4f, 0.4f, 1.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.5f, 0.5f, 1.0f, 1.0f));
    if (ImGui::SliderFloat("Pitch Amount", &blueAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdBlueAmount)) = blueAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdBlueAmount), paramIdBlueAmount, blueAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Pitch effect.\n0 = no pitch shift, 1 = full pitch range (-24 to "
        "+24 semitones).");

    float yellowAmount = yellowAmountParam ? yellowAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.8f, 0.8f, 0.2f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.9f, 0.9f, 0.3f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 0.4f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
    if (ImGui::SliderFloat("Reverb Amount", &yellowAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdYellowAmount)) =
            yellowAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdYellowAmount), paramIdYellowAmount, yellowAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Reverb effect.\n0 = no reverb, 1 = full reverb range (room size "
        "0-1, decay 0-1).");

    float cyanAmount = cyanAmountParam ? cyanAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.8f, 0.8f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.3f, 0.9f, 0.9f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.4f, 1.0f, 1.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.5f, 1.0f, 1.0f, 1.0f));
    if (ImGui::SliderFloat("Distortion Amount", &cyanAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdCyanAmount)) = cyanAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdCyanAmount), paramIdCyanAmount, cyanAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Distortion effect.\n0 = no distortion, 1 = full distortion "
        "range (drive 0-1, tone 0-1).");

    float magentaAmount = magentaAmountParam ? magentaAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.8f, 0.2f, 0.8f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.9f, 0.3f, 0.9f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 0.4f, 1.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.5f, 1.0f, 1.0f));
    if (ImGui::SliderFloat("Chorus Amount", &magentaAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdMagentaAmount)) =
            magentaAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdMagentaAmount), paramIdMagentaAmount, magentaAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Chorus effect.\n0 = no chorus, 1 = full chorus range (delay "
        "0-50ms, depth 0-1).");

    float orangeAmount = orangeAmountParam ? orangeAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.8f, 0.5f, 0.2f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.9f, 0.6f, 0.3f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 0.7f, 0.4f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.8f, 0.5f, 1.0f));
    if (ImGui::SliderFloat("Bitcrusher Amount", &orangeAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdOrangeAmount)) =
            orangeAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdOrangeAmount), paramIdOrangeAmount, orangeAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Bitcrusher effect.\n0 = no bitcrushing, 1 = full bitcrush range "
        "(bits 1-16, downsample 1-16x).");

    float purpleAmount = purpleAmountParam ? purpleAmountParam->load() : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.8f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.9f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.4f, 1.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.9f, 0.5f, 1.0f, 1.0f));
    if (ImGui::SliderFloat("Tremolo Amount", &purpleAmount, 0.0f, 1.0f, "%.2f"))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPurpleAmount)) =
            purpleAmount;
    }
    ImGui::PopStyleColor(4);
    adjustParamOnWheel(ap.getParameter(paramIdPurpleAmount), paramIdPurpleAmount, purpleAmount);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    HelpMarker(
        "Controls the intensity of Tremolo effect.\n0 = no tremolo, 1 = full tremolo range (rate "
        "0-10 Hz, depth 0-1).");

    ImGui::Spacing();

    // Define colors for UI
    ImU32 activeColorBg = IM_COL32(100, 100, 100, 255);

    // Tool selection buttons
    ImGui::Text("Tools:");
    const bool isPenActive = (activeTool == DotType::Pen);
    if (isPenActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    if (ImGui::Button("Pen", ImVec2(60, 0)))
    {
        activeTool = DotType::Pen;
    }
    if (isPenActive)
        ImGui::PopStyleColor();
    ImGui::SameLine();
    const bool isSprayActive = (activeTool == DotType::Spray);
    if (isSprayActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    if (ImGui::Button("Spray", ImVec2(60, 0)))
    {
        activeTool = DotType::Spray;
    }
    if (isSprayActive)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 70, 70, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 100, 100, 255));
    if (ImGui::Button("Clear Canvas", ImVec2(100, 0)))
    {
        // Clear all dots with thread safety
        {
            const juce::ScopedWriteLock lock(dotsLock);
            dots.clear();
        }
        // Also clear density phases map
        dotDensityPhases.clear();

        // Deactivate all active voices and grains for clean reset
        for (auto& voice : voicePool)
        {
            voice.isActive = false;
        }
        for (auto& grain : grainPool)
        {
            grain.isActive = false;
        }

        onModificationEnded();
    }
    ImGui::PopStyleColor(3);
    HelpMarker("Clear all dots from the canvas and start fresh.");

    // Color selection buttons
    ImGui::Spacing();
    ImGui::Text("Color:");

    const bool isRedActive = (activeColor == ColorID::Red);
    if (isRedActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 100, 100, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255)); // Dark text for readability
    if (ImGui::Button("Delay", ImVec2(60, 0)))
    {
        activeColor = ColorID::Red;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isRedActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Delay. Controls delay time for each voice/grain.\nX-axis = Delay time (left=short, "
        "right=long, 0-2000ms)\nY-axis = Feedback (bottom=none, top=maximum, 0-0.95)\nLarger dots "
        "= more intensity.");

    ImGui::SameLine();
    const bool isGreenActive = (activeColor == ColorID::Green);
    if (isGreenActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 255, 100, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255)); // Dark text for readability
    if (ImGui::Button("Filter", ImVec2(60, 0)))
    {
        activeColor = ColorID::Green;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isGreenActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Filter. Controls lowpass filtering for each voice/grain.\nX-axis = Cutoff frequency "
        "(left=low, right=high, 20Hz-20kHz)\nY-axis = Resonance/Q (bottom=low, top=high, "
        "0.707-10.0)\nLarger dots = more intensity.");

    ImGui::SameLine();
    const bool isBlueActive = (activeColor == ColorID::Blue);
    if (isBlueActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 100, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255)); // White text for dark blue
    if (ImGui::Button("Pitch", ImVec2(60, 0)))
    {
        activeColor = ColorID::Blue;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isBlueActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Pitch. Controls pitch shift for each voice/grain.\nX+Y position = Pitch shift "
        "(bottom-left=-24st, top-right=+24st)\nLarger dots = more intensity.");

    ImGui::SameLine();
    const bool isYellowActive = (activeColor == ColorID::Yellow);
    if (isYellowActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 100, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255)); // Dark text for readability
    if (ImGui::Button("Reverb", ImVec2(60, 0)))
    {
        activeColor = ColorID::Yellow;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isYellowActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Reverb. Controls reverb/decay for each voice/grain.\nX-axis = Room size (left=small, "
        "right=large, 0-1)\nY-axis = Decay time (bottom=short, top=long, 0-1)\nLarger dots = more "
        "intensity.");

    ImGui::SameLine();
    const bool isCyanActive = (activeColor == ColorID::Cyan);
    if (isCyanActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255)); // Dark text for readability
    if (ImGui::Button("Distort", ImVec2(60, 0)))
    {
        activeColor = ColorID::Cyan;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isCyanActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Distortion. Controls drive and tone for each voice/grain.\nX-axis = Drive amount "
        "(left=clean, right=distorted, 0-1)\nY-axis = Tone (bottom=dark, top=bright, 0-1)\nLarger "
        "dots = more intensity.");

    ImGui::NewLine();
    const bool isMagentaActive = (activeColor == ColorID::Magenta);
    if (isMagentaActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 100, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255)); // Dark text for readability
    if (ImGui::Button("Chorus", ImVec2(60, 0)))
    {
        activeColor = ColorID::Magenta;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isMagentaActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Chorus. Controls modulation for each voice/grain.\nX-axis = Delay time (left=short, "
        "right=long, 0-50ms)\nY-axis = Modulation depth (bottom=shallow, top=deep, 0-1)\nLarger "
        "dots = more intensity.");

    ImGui::SameLine();
    const bool isOrangeActive = (activeColor == ColorID::Orange);
    if (isOrangeActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 165, 0, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255)); // Dark text for readability
    if (ImGui::Button("Crush", ImVec2(60, 0)))
    {
        activeColor = ColorID::Orange;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isOrangeActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Bitcrusher. Controls bit depth and downsampling.\nX-axis = Bit depth (left=low bits, "
        "right=high bits, 1-16)\nY-axis = Downsample factor (bottom=none, top=heavy, "
        "1-16x)\nLarger dots = more intensity.");

    ImGui::SameLine();
    const bool isPurpleActive = (activeColor == ColorID::Purple);
    if (isPurpleActive)
        ImGui::PushStyleColor(ImGuiCol_Button, activeColorBg);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 100, 255, 255));
    ImGui::PushStyleColor(
        ImGuiCol_Text, IM_COL32(255, 255, 255, 255)); // White text for dark purple
    if (ImGui::Button("Tremolo", ImVec2(60, 0)))
    {
        activeColor = ColorID::Purple;
    }
    ImGui::PopStyleColor(); // Text
    ImGui::PopStyleColor(); // Button
    if (isPurpleActive)
        ImGui::PopStyleColor();
    HelpMarker(
        "Tremolo. Controls amplitude modulation for each voice/grain.\nX-axis = Modulation rate "
        "(left=slow, right=fast, 0-10 Hz)\nY-axis = Modulation depth (bottom=shallow, top=deep, "
        "0-1)\nLarger dots = more intensity.");

    ImGui::Spacing();

    // Canvas with 16:9 aspect ratio
    // Use most of the available width, but maintain 16:9 ratio
    const float            canvasWidth = itemWidth * 0.95f;           // Use 95% of available width
    const float            canvasHeight = canvasWidth * 9.0f / 16.0f; // 16:9 aspect ratio
    const ImVec2           canvasSizeVec(canvasWidth, canvasHeight);
    const ImGuiWindowFlags childFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushID(this);
    if (ImGui::BeginChild("SpatialGranulatorCanvas", canvasSizeVec, false, childFlags))
    {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        if (!draw_list)
        {
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopItemWidth();
            return;
        }

        // Use GetCursorScreenPos() like StrokeSequencer does for accurate mouse position
        const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
        const ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvasWidth, canvas_p0.y + canvasHeight);

        // Get theme colors
        ImU32 bgColor = theme.canvas.canvas_background == 0 ? IM_COL32(30, 30, 30, 255)
                                                            : theme.canvas.canvas_background;
        ImU32 frameColor =
            theme.canvas.node_frame == 0 ? IM_COL32(150, 150, 150, 255) : theme.canvas.node_frame;

        draw_list->AddRectFilled(canvas_p0, canvas_p1, bgColor);
        draw_list->AddRect(canvas_p0, canvas_p1, frameColor);
        draw_list->PushClipRect(canvas_p0, canvas_p1, true);

        // Draw grid
        const int gridDivisions = 4;
        for (int i = 1; i < gridDivisions; ++i)
        {
            float pos = (float)i / (float)gridDivisions;
            // Vertical lines
            float x = canvas_p0.x + pos * canvasWidth;
            draw_list->AddLine(
                ImVec2(x, canvas_p0.y), ImVec2(x, canvas_p1.y), IM_COL32(60, 60, 60, 255), 1.0f);
            // Horizontal lines
            float y = canvas_p0.y + pos * canvasHeight;
            draw_list->AddLine(
                ImVec2(canvas_p0.x, y), ImVec2(canvas_p1.x, y), IM_COL32(60, 60, 60, 255), 1.0f);
        }

        // Draw center crosshair
        draw_list->AddLine(
            ImVec2(canvas_p0.x + canvasWidth * 0.5f, canvas_p0.y),
            ImVec2(canvas_p0.x + canvasWidth * 0.5f, canvas_p1.y),
            IM_COL32(100, 100, 100, 255),
            1.0f);
        draw_list->AddLine(
            ImVec2(canvas_p0.x, canvas_p0.y + canvasHeight * 0.5f),
            ImVec2(canvas_p1.x, canvas_p0.y + canvasHeight * 0.5f),
            IM_COL32(100, 100, 100, 255),
            1.0f);

        // Draw dots
        {
            const juce::ScopedReadLock lock(dotsLock);
            for (const auto& dot : dots)
            {
                float x = canvas_p0.x + dot.x * canvasWidth;
                float y = canvas_p0.y + (1.0f - dot.y) * canvasHeight; // Invert Y
                float radius = dot.size * juce::jmin(canvasWidth, canvasHeight) *
                               0.1f; // Scale size to reasonable radius

                ImU32 color = IM_COL32(128, 128, 128, 255);
                switch (dot.color)
                {
                case ColorID::Red:
                    color = IM_COL32(255, 0, 0, 255);
                    break;
                case ColorID::Green:
                    color = IM_COL32(0, 255, 0, 255);
                    break;
                case ColorID::Blue:
                    color = IM_COL32(0, 0, 255, 255);
                    break;
                case ColorID::Yellow:
                    color = IM_COL32(255, 255, 0, 255);
                    break;
                case ColorID::Cyan:
                    color = IM_COL32(0, 255, 255, 255);
                    break;
                case ColorID::Magenta:
                    color = IM_COL32(255, 0, 255, 255);
                    break;
                case ColorID::Orange:
                    color = IM_COL32(255, 165, 0, 255);
                    break;
                case ColorID::Purple:
                    color = IM_COL32(200, 100, 255, 255);
                    break;
                default:
                    break;
                }

                draw_list->AddCircleFilled(ImVec2(x, y), radius, color);
                draw_list->AddCircle(ImVec2(x, y), radius, IM_COL32(255, 255, 255, 200), 0, 2.0f);
            }
        }

        draw_list->PopClipRect();

        // Mouse interaction - must be after PopClipRect to get accurate hover state
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton(
            "##canvasDrag",
            canvasSizeVec,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool is_hovered = ImGui::IsItemHovered();
        ImVec2     mouse_pos_in_canvas = ImVec2(
            ImGui::GetIO().MousePos.x - canvas_p0.x, ImGui::GetIO().MousePos.y - canvas_p0.y);

        // Eraser Visual Feedback (Right Mouse Button) - show erase circle
        if (is_hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            const float eraseRadiusPixels = 15.0f;
            ImVec2      center =
                ImVec2(canvas_p0.x + mouse_pos_in_canvas.x, canvas_p0.y + mouse_pos_in_canvas.y);
            draw_list->AddCircleFilled(center, eraseRadiusPixels, IM_COL32(255, 0, 100, 80));
            draw_list->AddCircle(center, eraseRadiusPixels, IM_COL32(255, 0, 100, 255), 0, 2.5f);
        }

        // ERASER LOGIC (Right Mouse Button - Click or Drag) - like StrokeSequencer
        if (is_hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            const float eraseRadius = 0.08f; // Erase threshold in normalized coordinates (8% of
                                             // canvas - larger for easier erasing)
            float mouseX = juce::jlimit(0.0f, 1.0f, mouse_pos_in_canvas.x / canvasWidth);
            float mouseY = juce::jlimit(0.0f, 1.0f, 1.0f - mouse_pos_in_canvas.y / canvasHeight);

            const juce::ScopedWriteLock lock(dotsLock);
            const size_t                dotsBefore = dots.size();
            auto                        it = dots.begin();
            while (it != dots.end())
            {
                float dist = std::sqrt(
                    (it->x - mouseX) * (it->x - mouseX) + (it->y - mouseY) * (it->y - mouseY));
                if (dist < eraseRadius)
                {
                    it = dots.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            if (dots.size() != dotsBefore)
            {
                onModificationEnded();
            }
        }
        // DRAWING LOGIC (Left Mouse Button)
        else if (
            is_hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                           ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
        {
            // Use mouse_pos_in_canvas which is already calculated relative to canvas
            float x = juce::jlimit(0.0f, 1.0f, mouse_pos_in_canvas.x / canvasWidth);
            float y =
                juce::jlimit(0.0f, 1.0f, 1.0f - mouse_pos_in_canvas.y / canvasHeight); // Invert Y

            Dot newDot;
            newDot.x = x;
            newDot.y = y;
            newDot.size = defaultDotSize;
            newDot.color = activeColor;
            newDot.type = activeTool;

            // For Spray tool, add multiple dots while dragging (spray effect)
            if (activeTool == DotType::Spray)
            {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    // Add dots with randomness while dragging
                    if (random.nextInt(10) < 3) // 30% chance per frame
                    {
                        newDot.x += (random.nextFloat() - 0.5f) * 0.1f;
                        newDot.y += (random.nextFloat() - 0.5f) * 0.1f;
                        newDot.x = juce::jlimit(0.0f, 1.0f, newDot.x);
                        newDot.y = juce::jlimit(0.0f, 1.0f, newDot.y);
                        newDot.size =
                            defaultDotSize * (0.5f + random.nextFloat() * 0.5f); // Vary size

                        const juce::ScopedWriteLock lock(dotsLock);
                        dots.push_back(newDot);
                    }
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    // Add initial dot on click
                    const juce::ScopedWriteLock lock(dotsLock);
                    dots.push_back(newDot);
                    onModificationEnded();
                }
            }
            else if (activeTool == DotType::Pen)
            {
                // For Pen tool, add dots on both click and drag (for drawing lines of voices)
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    // Only add if not already a dot very close (avoid duplicates)
                    bool tooClose = false;
                    {
                        const juce::ScopedReadLock lock(dotsLock);
                        for (const auto& existingDot : dots)
                        {
                            if (existingDot.type == DotType::Pen)
                            {
                                float dist = std::sqrt(
                                    (existingDot.x - newDot.x) * (existingDot.x - newDot.x) +
                                    (existingDot.y - newDot.y) * (existingDot.y - newDot.y));
                                if (dist < 0.02f) // Threshold for duplicate detection
                                {
                                    tooClose = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (!tooClose)
                    {
                        const juce::ScopedWriteLock lock(dotsLock);
                        dots.push_back(newDot);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            onModificationEnded();
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    // === OUTPUT WAVEFORM VISUALIZATION ===
    ImGui::Spacing();
    ThemeText("Output Waveform", theme.text.section_header);
    ImGui::Spacing();

    // Read visualization data (thread-safe) - BEFORE BeginChild
    float outputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        outputWaveform[i] = vizData.outputWaveform[i].load();
    }
    const int   activeVoices = vizData.activeVoices.load();
    const int   activeGrains = vizData.activeGrains.load();
    const float bufferFill = vizData.bufferFillLevel.load();
    const float outputLevel = vizData.outputLevel.load();

    // Waveform visualization in child window
    const auto& freqColors = theme.modules.frequency_graph;
    const auto  resolveColor = [](ImU32 value, ImU32 fallback) {
        return value != 0 ? value : fallback;
    };
    const float  waveHeight = 120.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);

    if (ImGui::BeginChild(
            "SpatialGranulatorWaveform",
            graphSize,
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImDrawList*  drawList = ImGui::GetWindowDrawList();
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
        float       prevX = p0.x;
        float       prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveform[i]);
            const float x = p0.x + i * stepX;
            const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), waveformColor, 2.0f);
            prevX = x;
            prevY = y;
        }

        // Draw buffer fill indicator (horizontal bar at bottom)
        if (bufferFill > 0.0f)
        {
            const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);
            const float fillWidth = graphSize.x * bufferFill;
            const float barHeight = 4.0f;
            const float barY = p1.y - barHeight - 2.0f;
            drawList->AddRectFilled(
                ImVec2(p0.x, barY), ImVec2(p0.x + fillWidth, barY + barHeight), fillColor);
        }

        drawList->PopClipRect();

        // Info overlay
        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(
            ImVec4(1.0f, 1.0f, 1.0f, 0.9f),
            "Voices: %d | Grains: %d | Buffer: %.0f%% | %.1f dBFS",
            activeVoices,
            activeGrains,
            bufferFill * 100.0f,
            outputLevel);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##waveformDrag", graphSize);
    }
    ImGui::EndChild(); // CRITICAL: Must be OUTSIDE the if block!

    ImGui::PopID();

    ImGui::PopItemWidth();
}

void SpatialGranulatorModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Draw audio input/output pairs
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);

    // CV modulation inputs - ALWAYS draw them, use direct channel indices
    // Channels 2-6 are CV inputs as defined in BusesProperties
    helpers.drawParallelPins("Dry Mix Mod", 2, nullptr, -1);
    helpers.drawParallelPins("Pen Mix Mod", 3, nullptr, -1);
    helpers.drawParallelPins("Spray Mix Mod", 4, nullptr, -1);
    helpers.drawParallelPins("Density Mod", 5, nullptr, -1);
    helpers.drawParallelPins("Grain Size Mod", 6, nullptr, -1);
}
#endif

juce::ValueTree SpatialGranulatorModuleProcessor::getExtraStateTree() const
{
    juce::ValueTree extra("SpatialGranulatorExtra");
    juce::ValueTree dotsTree("dots");

    {
        const juce::ScopedReadLock lock(dotsLock);
        for (const auto& dot : dots)
        {
            juce::ValueTree dotTree("dot");
            dotTree.setProperty("x", dot.x, nullptr);
            dotTree.setProperty("y", dot.y, nullptr);
            dotTree.setProperty("size", dot.size, nullptr);
            dotTree.setProperty("color", (int)dot.color, nullptr);
            dotTree.setProperty("type", (int)dot.type, nullptr);
            dotsTree.addChild(dotTree, -1, nullptr);
        }
    }

    extra.addChild(dotsTree, -1, nullptr);
    return extra;
}

void SpatialGranulatorModuleProcessor::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.isValid() && tree.hasType("SpatialGranulatorExtra"))
    {
        auto dotsTree = tree.getChildWithName("dots");
        if (dotsTree.isValid())
        {
            const juce::ScopedWriteLock lock(dotsLock);
            dots.clear();

            for (int i = 0; i < dotsTree.getNumChildren(); ++i)
            {
                auto dotTree = dotsTree.getChild(i);
                if (dotTree.hasType("dot"))
                {
                    Dot dot;
                    dot.x = (float)dotTree.getProperty("x", 0.5);
                    dot.y = (float)dotTree.getProperty("y", 0.5);
                    dot.size = (float)dotTree.getProperty("size", 0.3);
                    dot.color = (ColorID)(int)dotTree.getProperty("color", 0);
                    dot.type = (DotType)(int)dotTree.getProperty("type", 0);
                    dots.push_back(dot);
                }
            }
        }
    }
}
