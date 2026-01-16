#include "BitCrusherModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif
#include <cmath> // For std::exp2, std::floor

juce::AudioProcessorValueTreeState::ParameterLayout BitCrusherModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Bit Depth: 1.0f to 24.0f with logarithmic scaling (skew factor 0.3 like Waveshaper's drive)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdBitDepth,
            "Bit Depth",
            juce::NormalisableRange<float>(1.0f, 24.0f, 0.01f, 0.3f),
            16.0f));

    // Sample Rate: 0.1f to 1.0f with logarithmic scaling (skew factor 0.3)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdSampleRate,
            "Sample Rate",
            juce::NormalisableRange<float>(0.1f, 1.0f, 0.001f, 0.3f),
            1.0f));

    // Mix: 0.0f to 1.0f (linear, like DriveModuleProcessor)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 1.0f));

    // Anti-aliasing filter
    params.push_back(
        std::make_unique<juce::AudioParameterBool>(paramIdAntiAlias, "Anti-Aliasing", true));

    // Quantization mode
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            paramIdQuantMode,
            "Quant Mode",
            juce::StringArray{"Linear", "Dither (TPDF)", "Noise Shaping"},
            0));

    // Relative modulation parameters
    params.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "relativeBitDepthMod", "Relative Bit Depth Mod", true));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "relativeSampleRateMod", "Relative Sample Rate Mod", true));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeMixMod", "Relative Mix Mod", false));

    return {params.begin(), params.end()};
}

BitCrusherModuleProcessor::BitCrusherModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Audio In",
                  juce::AudioChannelSet::discreteChannels(7),
                  true) // 0-1: Audio, 2: BitDepth Mod, 3: SampleRate Mod, 4: Mix Mod, 5: AntiAlias
                        // Mod, 6: QuantMode Mod
              .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "BitCrusherParams", createParameterLayout())
{
    bitDepthParam = apvts.getRawParameterValue(paramIdBitDepth);
    sampleRateParam = apvts.getRawParameterValue(paramIdSampleRate);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    antiAliasParam = apvts.getRawParameterValue(paramIdAntiAlias);
    quantModeParam = apvts.getRawParameterValue(paramIdQuantMode);
    relativeBitDepthModParam = apvts.getRawParameterValue("relativeBitDepthMod");
    relativeSampleRateModParam = apvts.getRawParameterValue("relativeSampleRateMod");
    relativeMixModParam = apvts.getRawParameterValue("relativeMixMod");

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R

    // Initialize smoothed values
    mBitDepthSm.reset(16.0f);
    mSampleRateSm.reset(1.0f);

    // Initialize visualization data
    for (auto& w : vizData.inputWaveformL)
        w.store(0.0f);
    for (auto& w : vizData.inputWaveformR)
        w.store(0.0f);
    for (auto& w : vizData.outputWaveformL)
        w.store(0.0f);
    for (auto& w : vizData.outputWaveformR)
        w.store(0.0f);
    for (auto& p : vizData.holdStartPositions)
        p.store(-1.0f); // -1 = inactive
    for (auto& p : vizData.holdEndPositions)
        p.store(-1.0f);
}

void BitCrusherModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    tempBuffer.setSize(2, samplesPerBlock);

    // Initialize visualization buffers
    vizInputBuffer.setSize(2, vizBufferSize);
    vizOutputBuffer.setSize(2, vizBufferSize);
    vizDecimatedBuffer.setSize(2, vizBufferSize);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
    vizDecimatedBuffer.clear();
    vizWritePos = 0;

    // Reset hold tracking
    trackedHoldCount = 0;
    lastHoldStartPos = -1.0f;
    for (auto& hold : trackedHolds)
    {
        hold.startIdx = -1;
        hold.endIdx = -1;
        hold.value = 0.0f;
    }

    // Set smoothing time for parameters (10ms)
    mBitDepthSm.reset(sampleRate, 0.01);
    mSampleRateSm.reset(sampleRate, 0.01);

    // Prepare anti-aliasing filters
    juce::dsp::ProcessSpec specL{sampleRate, (juce::uint32)samplesPerBlock, 1};
    juce::dsp::ProcessSpec specR{sampleRate, (juce::uint32)samplesPerBlock, 1};
    mAntiAliasFilterL.prepare(specL);
    mAntiAliasFilterR.prepare(specR);
    mAntiAliasFilterL.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    mAntiAliasFilterR.setType(juce::dsp::StateVariableTPTFilterType::lowpass);

    // Reset quantization errors
    mQuantErrorL = 0.0f;
    mQuantErrorR = 0.0f;
}

void BitCrusherModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    juce::ignoreUnused(midi);

    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const float baseBitDepth = bitDepthParam != nullptr ? bitDepthParam->load() : 16.0f;
    const float baseSampleRate = sampleRateParam != nullptr ? sampleRateParam->load() : 1.0f;
    const float mixAmount = mixParam != nullptr ? mixParam->load() : 1.0f;

    const int numInputChannels = inBus.getNumChannels();
    const int numOutputChannels = outBus.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(numInputChannels, numOutputChannels);

    // ✅ CRITICAL FIX: Read CV inputs BEFORE any output operations to avoid buffer aliasing issues
    // According to DEBUG_INPUT_IMPORTANT.md: "Read all inputs BEFORE clearing any outputs"
    // Get pointers to modulation CV inputs from unified input bus FIRST
    // Use virtual _mod IDs as per BestPracticeNodeProcessor.md
    const bool   isBitDepthMod = isParamInputConnected(paramIdBitDepthMod);
    const bool   isSampleRateMod = isParamInputConnected(paramIdSampleRateMod);
    const bool   isMixMod = isParamInputConnected(paramIdMixMod);
    const bool   isAntiAliasMod = isParamInputConnected(paramIdAntiAliasMod);
    const bool   isQuantModeMod = isParamInputConnected(paramIdQuantModeMod);
    const float* bitDepthCV =
        isBitDepthMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* sampleRateCV =
        isSampleRateMod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;
    const float* mixCV = isMixMod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;
    const float* antiAliasCV =
        isAntiAliasMod && inBus.getNumChannels() > 5 ? inBus.getReadPointer(5) : nullptr;
    const float* quantModeCV =
        isQuantModeMod && inBus.getNumChannels() > 6 ? inBus.getReadPointer(6) : nullptr;

    const bool baseAntiAlias = antiAliasParam != nullptr && antiAliasParam->load() > 0.5f;
    const int  baseQuantMode =
        quantModeParam != nullptr ? static_cast<int>(quantModeParam->load()) : 0;
    const bool relativeBitDepthMode =
        relativeBitDepthModParam != nullptr && relativeBitDepthModParam->load() > 0.5f;
    const bool relativeSampleRateMode =
        relativeSampleRateModParam != nullptr && relativeSampleRateModParam->load() > 0.5f;
    const bool relativeMixMode =
        relativeMixModParam != nullptr && relativeMixModParam->load() > 0.5f;

    // ✅ Now safe to copy input to output (CV pointers already obtained)
    if (numInputChannels > 0)
    {
        // If input is mono, copy it to both left and right outputs.
        if (numInputChannels == 1 && numOutputChannels > 1)
        {
            outBus.copyFrom(0, 0, inBus, 0, 0, numSamples);
            outBus.copyFrom(1, 0, inBus, 0, 0, numSamples);
        }
        // Otherwise, perform a standard stereo copy.
        else
        {
            const int channelsToCopy = juce::jmin(numInputChannels, numOutputChannels);
            for (int ch = 0; ch < channelsToCopy; ++ch)
            {
                outBus.copyFrom(ch, 0, inBus, ch, 0, numSamples);
            }
        }
    }
    else
    {
        // If no input is connected, ensure the output is silent.
        // ✅ Safe to clear here - we've already read CV inputs above
        outBus.clear();
    }

    // If bit depth is at maximum and sample rate is at maximum and mix is fully dry, we can skip
    // processing entirely.
    if (baseBitDepth >= 23.99f && baseSampleRate >= 0.999f && mixAmount <= 0.001f)
    {
        // Update output values for tooltips
        if (lastOutputValues.size() >= 2)
        {
            if (lastOutputValues[0])
                lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
            if (lastOutputValues[1] && numChannels > 1)
                lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
        }
        return;
    }

    // --- Dry/Wet Mix Implementation (inspired by DriveModuleProcessor) ---
    // 1. Make a copy of the original (dry) signal.
    tempBuffer.makeCopyOf(outBus);

    // Create temporary single-sample buffers for anti-aliasing filter processing (per channel)
    juce::AudioBuffer<float>                  sampleBufferL(1, 1);
    juce::AudioBuffer<float>                  sampleBufferR(1, 1);
    juce::dsp::AudioBlock<float>              blockL(sampleBufferL);
    juce::dsp::AudioBlock<float>              blockR(sampleBufferR);
    juce::dsp::ProcessContextReplacing<float> contextL(blockL);
    juce::dsp::ProcessContextReplacing<float> contextR(blockR);

    // Process each channel
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float*       data = outBus.getWritePointer(ch);
        const float* dryData = tempBuffer.getReadPointer(ch);

        // Per-channel decimator state
        float& srCounter = (ch == 0) ? mSrCounterL : mSrCounterR;
        float& lastSample = (ch == 0) ? mLastSampleL : mLastSampleR;

        // Per-channel quantization error state
        float& quantError = (ch == 0) ? mQuantErrorL : mQuantErrorR;

        // Select the correct anti-aliasing filter and context for this channel
        auto& antiAliasFilter = (ch == 0) ? mAntiAliasFilterL : mAntiAliasFilterR;
        auto& sampleBuffer = (ch == 0) ? sampleBufferL : sampleBufferR;
        auto& context = (ch == 0) ? contextL : contextR;

        for (int i = 0; i < numSamples; ++i)
        {
            // PER-SAMPLE FIX: Calculate effective bit depth FOR THIS SAMPLE
            float bitDepth = baseBitDepth;
            if (isBitDepthMod && bitDepthCV != nullptr)
            {
                const float cv = juce::jlimit(0.0f, 1.0f, bitDepthCV[i]);
                if (relativeBitDepthMode)
                {
                    // RELATIVE: ±12 bits around base (e.g., base 16 -> range 4-28, clamped to 1-24)
                    const float bitRange = 12.0f;
                    const float bitOffset = (cv - 0.5f) * (bitRange * 2.0f);
                    bitDepth = baseBitDepth + bitOffset;
                }
                else
                {
                    // ABSOLUTE: CV directly sets bit depth (1-24)
                    bitDepth = juce::jmap(cv, 1.0f, 24.0f);
                }
                bitDepth = juce::jlimit(1.0f, 24.0f, bitDepth);
            }

            // Apply smoothing to bit depth to prevent zipper noise
            mBitDepthSm.setTargetValue(bitDepth);
            bitDepth = mBitDepthSm.getNextValue();

            // PER-SAMPLE FIX: Calculate effective sample rate FOR THIS SAMPLE
            float sampleRate = baseSampleRate;
            if (isSampleRateMod && sampleRateCV != nullptr)
            {
                const float cv = juce::jlimit(0.0f, 1.0f, sampleRateCV[i]);
                if (relativeSampleRateMode)
                {
                    // RELATIVE: ±3 octaves (0.125x to 8x, clamped to 0.1x-1.0x)
                    const float octaveRange = 3.0f;
                    const float octaveOffset = (cv - 0.5f) * (octaveRange * 2.0f);
                    sampleRate = baseSampleRate * std::pow(2.0f, octaveOffset);
                }
                else
                {
                    // ABSOLUTE: CV directly sets sample rate (0.1-1.0)
                    sampleRate = juce::jmap(cv, 0.1f, 1.0f);
                }
                sampleRate = juce::jlimit(0.1f, 1.0f, sampleRate);
            }

            // Apply smoothing to sample rate to prevent clicks
            mSampleRateSm.setTargetValue(sampleRate);
            sampleRate = mSampleRateSm.getNextValue();

            // Determine effective anti-aliasing state for this sample
            bool isAntiAliasOn = baseAntiAlias;
            if (isAntiAliasMod && antiAliasCV != nullptr)
            {
                isAntiAliasOn = (antiAliasCV[i] > 0.5f);
            }

            // Determine effective quantization mode for this sample
            int quantMode = baseQuantMode;
            if (isQuantModeMod && quantModeCV != nullptr)
            {
                quantMode = static_cast<int>(
                    juce::jmap(juce::jlimit(0.0f, 1.0f, quantModeCV[i]), 0.0f, 2.99f));
            }

            // PER-SAMPLE FIX: Calculate effective mix FOR THIS SAMPLE
            float mixAmountPerSample = mixAmount;
            if (isMixMod && mixCV != nullptr)
            {
                const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
                if (relativeMixMode)
                {
                    // RELATIVE: ±0.5 around base mix (e.g., base 0.5 -> range 0.0-1.0, clamped to
                    // 0-1)
                    const float mixRange = 0.5f;
                    const float mixOffset = (cv - 0.5f) * (mixRange * 2.0f);
                    mixAmountPerSample = mixAmount + mixOffset;
                }
                else
                {
                    // ABSOLUTE: CV directly sets mix (0-1)
                    mixAmountPerSample = cv;
                }
                mixAmountPerSample = juce::jlimit(0.0f, 1.0f, mixAmountPerSample);
            }

            // Get the sample *before* decimation
            float sampleToDecimate = data[i];

            // Update the anti-aliasing filter cutoff based on the *current* smoothed sample rate
            antiAliasFilter.setCutoffFrequency(sampleRate * (getSampleRate() * 0.45f));

            // Conditionally filter the sample using ProcessContextReplacing (like
            // VCFModuleProcessor)
            if (isAntiAliasOn)
            {
                sampleBuffer.setSample(0, 0, sampleToDecimate);
                antiAliasFilter.process(context);
                sampleToDecimate = sampleBuffer.getSample(0, 0);
            }

            // Now, perform the sample-and-hold on the (potentially filtered) sample
            // Track hold regions for visualization
            const bool wasHolding = (srCounter > 0.0f && srCounter < 1.0f);
            srCounter += sampleRate;
            const bool holdJustStarted = (srCounter >= 1.0f) && wasHolding;

            if (srCounter >= 1.0f)
            {
                srCounter -= 1.0f;
                lastSample = sampleToDecimate;

                // Track hold start for visualization (throttled, every 64 samples)
                if (ch == 0 && (i & 0x3F) == 0) // Only track left channel, throttled
                {
                    // Calculate normalized position in waveform snapshot
                    const float posInBuffer = (float)vizWritePos / (float)vizBufferSize;
                    const int   snapshotIdx = (int)(posInBuffer * (float)VizData::waveformPoints) %
                                            VizData::waveformPoints;

                    // Record that a new hold started here
                    lastHoldStartPos = snapshotIdx;
                }
            }
            float decimatedSample = lastSample;

            // Record decimated sample (before quantization) for visualization
            if (ch == 0) // Only record once per sample (left channel)
            {
                vizDecimatedBuffer.setSample(0, vizWritePos, decimatedSample);
            }

            // --- New Quantization Logic ---
            float numLevels = std::exp2(bitDepth);
            float step = 2.0f / (numLevels - 1.0f); // Step size for [-1, 1] range

            float dither = 0.0f;
            float sampleToQuantize = decimatedSample;

            switch (quantMode)
            {
            case 1: // Dither (TPDF)
                dither = (mRandom.nextFloat() - mRandom.nextFloat()) * 0.5f * step;
                quantError = 0.0f; // Reset error if switching
                break;
            case 2: // Noise Shaping
                dither = (mRandom.nextFloat() - mRandom.nextFloat()) * 0.5f * step;
                sampleToQuantize += quantError; // Add previous sample's error
                break;
            case 0: // Linear
            default:
                quantError = 0.0f; // Reset error
                break;
            }

            // Quantize using floor-rounding
            float quantizedSample = std::floor((sampleToQuantize + dither) / step + 0.5f) * step;
            quantizedSample = juce::jlimit(-1.0f, 1.0f, quantizedSample);

            // If noise shaping, calculate and store the error for the *next* sample
            if (quantMode == 2)
            {
                float error = sampleToQuantize - quantizedSample;
                quantError = error * 0.95f; // Simple 1-pole high-pass on error
            }
            // --- End of New Quantization Logic ---

            // Apply dry/wet mix (using per-sample modulated mix amount)
            const float dryLevel = 1.0f - mixAmountPerSample;
            const float wetLevel = mixAmountPerSample;
            data[i] = dryData[i] * dryLevel + quantizedSample * wetLevel;

            // Record to visualization buffers (circular)
            if (ch == 0) // Only record once per sample (left channel)
            {
                vizInputBuffer.setSample(0, vizWritePos, dryData[i]);
                vizOutputBuffer.setSample(0, vizWritePos, data[i]);
                if (vizInputBuffer.getNumChannels() > 1 && vizOutputBuffer.getNumChannels() > 1)
                {
                    vizInputBuffer.setSample(1, vizWritePos, tempBuffer.getSample(1, i));
                    vizOutputBuffer.setSample(1, vizWritePos, outBus.getSample(1, i));
                }
                vizWritePos = (vizWritePos + 1) % vizBufferSize;

                // Update visualization data (throttled - every 64 samples, like Granulator)
                if ((i & 0x3F) == 0)
                {
                    // Update current quantization state
                    vizData.currentBitDepth.store(mBitDepthSm.getCurrentValue());
                    vizData.currentSampleRate.store(mSampleRateSm.getCurrentValue());
                    vizData.currentQuantMode.store(baseQuantMode);
                    vizData.currentAntiAlias.store(baseAntiAlias);

                    // Update waveform snapshots (downsampled from circular buffer)
                    const int step = vizBufferSize / VizData::waveformPoints;
                    for (int j = 0; j < VizData::waveformPoints; ++j)
                    {
                        int idx =
                            (vizWritePos - (VizData::waveformPoints - j) * step + vizBufferSize) %
                            vizBufferSize;
                        vizData.inputWaveformL[j].store(vizInputBuffer.getSample(0, idx));
                        vizData.outputWaveformL[j].store(vizOutputBuffer.getSample(0, idx));
                        if (vizInputBuffer.getNumChannels() > 1 &&
                            vizOutputBuffer.getNumChannels() > 1)
                        {
                            vizData.inputWaveformR[j].store(vizInputBuffer.getSample(1, idx));
                            vizData.outputWaveformR[j].store(vizOutputBuffer.getSample(1, idx));
                        }
                    }

                    // Track sample-and-hold regions from actual decimator state
                    // Find holds by looking for flat regions in the decimated buffer
                    int         activeHoldIdx = 0;
                    const float sampleRateValue = mSampleRateSm.getCurrentValue();
                    const float tolerance =
                        0.01f; // Samples within this range are considered "held"

                    if (sampleRateValue < 0.999f &&
                        VizData::waveformPoints > 1) // Only track holds if sample rate is reduced
                    {
                        // Scan the decimated waveform for flat regions (holds)
                        // A hold is a region where consecutive samples are approximately equal
                        float currentHoldStart = -1.0f;
                        float currentHoldValue = 0.0f;

                        for (int k = 0; k < VizData::waveformPoints && activeHoldIdx < 64; ++k)
                        {
                            const float decimatedVal = vizDecimatedBuffer.getSample(
                                0,
                                (vizWritePos - (VizData::waveformPoints - k) + vizBufferSize) %
                                    vizBufferSize);
                            const float normalizedPos =
                                (float)k / (float)(VizData::waveformPoints - 1);

                            if (currentHoldStart < 0.0f)
                            {
                                // Not currently in a hold - check if we should start one
                                // Look ahead to see if next samples are similar
                                if (k < VizData::waveformPoints - 1)
                                {
                                    const int nextIdx =
                                        (vizWritePos - (VizData::waveformPoints - (k + 1)) +
                                         vizBufferSize) %
                                        vizBufferSize;
                                    const float nextVal = vizDecimatedBuffer.getSample(0, nextIdx);
                                    if (std::abs(decimatedVal - nextVal) < tolerance)
                                    {
                                        // Start a hold
                                        currentHoldStart = normalizedPos;
                                        currentHoldValue = decimatedVal;
                                    }
                                }
                            }
                            else
                            {
                                // Currently in a hold - check if it continues
                                if (k == VizData::waveformPoints - 1 ||
                                    std::abs(decimatedVal - currentHoldValue) > tolerance)
                                {
                                    // End the hold
                                    const float holdEnd =
                                        (k == VizData::waveformPoints - 1)
                                            ? 1.0f
                                            : ((float)k - 1.0f) /
                                                  (float)(VizData::waveformPoints - 1);

                                    // Only record holds that are long enough (at least 2% of
                                    // waveform width)
                                    if (holdEnd - currentHoldStart > 0.02f)
                                    {
                                        vizData.holdStartPositions[activeHoldIdx].store(
                                            currentHoldStart);
                                        vizData.holdEndPositions[activeHoldIdx].store(holdEnd);
                                        vizData.holdValues[activeHoldIdx].store(currentHoldValue);
                                        activeHoldIdx++;
                                    }

                                    // Check if we should start a new hold
                                    currentHoldStart = -1.0f;
                                    if (k < VizData::waveformPoints - 1)
                                    {
                                        const int nextIdx =
                                            (vizWritePos - (VizData::waveformPoints - (k + 1)) +
                                             vizBufferSize) %
                                            vizBufferSize;
                                        const float nextVal =
                                            vizDecimatedBuffer.getSample(0, nextIdx);
                                        if (std::abs(decimatedVal - nextVal) < tolerance)
                                        {
                                            currentHoldStart = normalizedPos;
                                            currentHoldValue = decimatedVal;
                                        }
                                    }
                                }
                            }
                        }

                        // Close any remaining hold
                        if (currentHoldStart >= 0.0f && activeHoldIdx < 64)
                        {
                            const float holdEnd = 1.0f;
                            if (holdEnd - currentHoldStart > 0.02f)
                            {
                                vizData.holdStartPositions[activeHoldIdx].store(currentHoldStart);
                                vizData.holdEndPositions[activeHoldIdx].store(holdEnd);
                                vizData.holdValues[activeHoldIdx].store(currentHoldValue);
                                activeHoldIdx++;
                            }
                        }
                    }

                    vizData.activeHoldCount.store(activeHoldIdx);

                    // Clear inactive hold slots
                    for (int j = activeHoldIdx; j < 64; ++j)
                    {
                        vizData.holdStartPositions[j].store(-1.0f);
                        vizData.holdEndPositions[j].store(-1.0f);
                        vizData.holdValues[j].store(0.0f);
                    }
                }

                // Update telemetry (throttled - every 64 samples as per
                // BestPracticeNodeProcessor.md) Only update once per sample (on left channel) to
                // avoid redundant updates
                if (ch == 0 && (i & 0x3F) == 0)
                {
                    setLiveParamValue("bit_depth_live", bitDepth);
                    setLiveParamValue("sample_rate_live", sampleRate);
                    setLiveParamValue("mix_live", mixAmountPerSample);
                    setLiveParamValue("antiAlias_live", isAntiAliasOn ? 1.0f : 0.0f);
                    setLiveParamValue("quant_mode_live", static_cast<float>(quantMode));
                }
            }
        }
    }

    // Update output values for tooltips
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1] && numChannels > 1)
            lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
    }
}

bool BitCrusherModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus

    // Use virtual _mod IDs as per BestPracticeNodeProcessor.md
    if (paramId == paramIdBitDepthMod)
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdSampleRateMod)
    {
        outChannelIndexInBus = 3;
        return true;
    }
    if (paramId == paramIdMixMod)
    {
        outChannelIndexInBus = 4;
        return true;
    }
    if (paramId == paramIdAntiAliasMod)
    {
        outChannelIndexInBus = 5;
        return true;
    }
    if (paramId == paramIdQuantModeMod)
    {
        outChannelIndexInBus = 6;
        return true;
    }
    return false;
}

juce::String BitCrusherModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
    case 0:
        return "In L";
    case 1:
        return "In R";
    case 2:
        return "Bit Depth Mod";
    case 3:
        return "Sample Rate Mod";
    case 4:
        return "Mix Mod";
    case 5:
        return "Anti-Alias Mod";
    case 6:
        return "Quant Mode Mod";
    default:
        return juce::String("In ") + juce::String(channel + 1);
    }
}

juce::String BitCrusherModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0)
        return "Out L";
    if (channel == 1)
        return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void BitCrusherModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto&       ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    auto HelpMarker = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    ThemeText("Bit Crusher Parameters", theme.text.section_header);
    ImGui::Spacing();

    // === BIT CRUSHER VISUALIZATION ===
    ImGui::Spacing();
    ImGui::Text("Waveform Visualization");
    ImGui::Spacing();

    // Draw waveform visualization with pixelated output
    auto*        drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  vizWidth = itemWidth;
    const float  vizHeight = 120.0f;
    const ImVec2 rectMax = ImVec2(origin.x + vizWidth, origin.y + vizHeight);

    // Get theme colors for visualization with nice variation
    auto& themeMgr = ThemeManager::getInstance();
    auto  resolveColor = [](ImU32 primary, ImU32 secondary, ImU32 tertiary) -> ImU32 {
        if (primary != 0)
            return primary;
        if (secondary != 0)
            return secondary;
        return tertiary;
    };

    // Background: scope_plot_bg -> canvas_background -> ChildBg -> fallback
    const ImU32  canvasBg = themeMgr.getCanvasBackground();
    const ImVec4 childBgVec4 = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
    const ImU32  childBg = ImGui::ColorConvertFloat4ToU32(childBgVec4);
    const ImU32  bgColor = resolveColor(theme.modules.scope_plot_bg, canvasBg, childBg);

    // Input waveform: Use modulation.frequency (cyan) for distinct input color
    const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
    const ImU32  frequencyColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(frequencyColorVec4.x, frequencyColorVec4.y, frequencyColorVec4.z, 0.6f));
    const ImU32 inputWaveformColor =
        resolveColor(theme.modules.scope_plot_fg, frequencyColor, IM_COL32(100, 200, 255, 150));

    // Output waveform: Use modulation.timbre (orange/yellow) for distinct output color
    const ImVec4 timbreColorVec4 = theme.modulation.timbre;
    const ImU32  timbreColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f));
    const ImVec4 accentVec4 = theme.accent;
    const ImU32  accentColor =
        ImGui::ColorConvertFloat4ToU32(ImVec4(accentVec4.x, accentVec4.y, accentVec4.z, 1.0f));
    const ImU32 outputWaveformColor =
        (timbreColor != 0) ? timbreColor : IM_COL32(255, 150, 100, 255);

    // Quant grid: Use muted scope_plot_fg or frequency color
    const ImU32 scopePlotFg = theme.modules.scope_plot_fg;
    const ImU32 quantGridColorBase =
        resolveColor(scopePlotFg, frequencyColor, IM_COL32(150, 150, 150, 80));
    const ImVec4 quantGridVec4 = ImGui::ColorConvertU32ToFloat4(quantGridColorBase);
    const ImU32  quantGridColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(quantGridVec4.x, quantGridVec4.y, quantGridVec4.z, 0.3f));

    // Hold regions: Use modulation.amplitude (magenta/pink) for distinct hold color
    const ImVec4 amplitudeColorVec4 = theme.modulation.amplitude;
    const ImU32  amplitudeColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(amplitudeColorVec4.x, amplitudeColorVec4.y, amplitudeColorVec4.z, 0.4f));
    const ImU32 holdRegionColor =
        (amplitudeColor != 0) ? amplitudeColor : IM_COL32(255, 200, 100, 100);

    // Hold region background: Same color but more transparent
    const ImVec4 holdRegionVec4 = ImGui::ColorConvertU32ToFloat4(holdRegionColor);
    const ImU32  holdRegionBgColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(holdRegionVec4.x, holdRegionVec4.y, holdRegionVec4.z, 0.12f));

    drawList->AddRectFilled(origin, rectMax, bgColor, 4.0f);
    ImGui::PushClipRect(origin, rectMax, true);

    // Read visualization data (thread-safe)
    float inputWaveform[VizData::waveformPoints];
    float outputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWaveform[i] = vizData.inputWaveformL[i].load();
        outputWaveform[i] = vizData.outputWaveformL[i].load();
    }
    const float currentBitDepth = vizData.currentBitDepth.load();
    const float currentSampleRate = vizData.currentSampleRate.load();
    const int   currentQuantMode = vizData.currentQuantMode.load();
    const bool  currentAntiAlias = vizData.currentAntiAlias.load();
    const int   activeHoldCount = vizData.activeHoldCount.load();
    float       holdStarts[64], holdEnds[64], holdVals[64];
    for (int i = 0; i < 64; ++i)
    {
        holdStarts[i] = vizData.holdStartPositions[i].load();
        holdEnds[i] = vizData.holdEndPositions[i].load();
        holdVals[i] = vizData.holdValues[i].load();
    }

    const float midY = origin.y + vizHeight * 0.5f;
    const float scaleY = vizHeight * 0.4f;
    const float stepX = vizWidth / (float)(VizData::waveformPoints - 1);

    // Calculate quantization grid (horizontal lines)
    const float numLevels = std::exp2(currentBitDepth);
    const int   maxGridLines = 32; // Limit grid lines for visibility
    const int   numGridLines = juce::jmin(static_cast<int>(numLevels), maxGridLines);
    const float gridStep = 2.0f / (numLevels - 1.0f);

    // Draw quantization grid (horizontal lines) - behind everything
    if (currentBitDepth < 16.0f) // Only show grid for lower bit depths
    {
        for (int i = 0; i <= numGridLines; ++i)
        {
            const float level =
                -1.0f + (float)i * gridStep * (numLevels - 1.0f) / (float)numGridLines;
            const float y = midY - juce::jlimit(-1.0f, 1.0f, level) * scaleY;
            drawList->AddLine(
                ImVec2(origin.x, y),
                ImVec2(rectMax.x, y),
                quantGridColor,
                currentBitDepth < 8.0f ? 1.5f : 0.5f);
        }
    }

    // Draw sample-and-hold background regions (subtle highlight, behind waveforms)
    for (int i = 0; i < activeHoldCount; ++i)
    {
        const float startNorm = holdStarts[i];
        const float endNorm = holdEnds[i];

        if (startNorm >= 0.0f && endNorm >= 0.0f && startNorm <= 1.0f && endNorm <= 1.0f &&
            endNorm > startNorm)
        {
            const float startX = origin.x + startNorm * vizWidth;
            const float endX = origin.x + endNorm * vizWidth;

            // Draw subtle background region to highlight hold area (behind waveforms)
            drawList->AddRectFilled(
                ImVec2(startX, origin.y), ImVec2(endX, rectMax.y), holdRegionBgColor, 0.0f);
        }
    }

    // Calculate pixel size based on bit depth (fewer bits = larger pixels)
    float pixelHeight = 1.0f;
    if (currentBitDepth <= 4.0f)
        pixelHeight = 8.0f;
    else if (currentBitDepth <= 8.0f)
        pixelHeight = 4.0f;
    else if (currentBitDepth <= 12.0f)
        pixelHeight = 2.0f;
    else if (currentBitDepth <= 16.0f)
        pixelHeight = 1.0f;
    else
        pixelHeight = 0.5f;

    // Draw input waveform (smooth, faded)
    float prevX = origin.x, prevY = midY;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const float sample = juce::jlimit(-1.0f, 1.0f, inputWaveform[i]);
        const float x = origin.x + i * stepX;
        const float y = midY - sample * scaleY;
        if (i > 0)
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), inputWaveformColor, 1.5f);
        prevX = x;
        prevY = y;
    }

    // Draw output waveform (pixelated based on bit depth only, NOT sample rate)
    // Pixelation represents quantization (vertical steps), not decimation (horizontal holds)
    // Sample rate is already shown as horizontal flat segments above

    // Group samples into small segments for pixelation effect (based on visualization resolution)
    const int samplesPerSegment =
        juce::jmax(1, VizData::waveformPoints / 128); // ~2 samples per pixel

    for (int i = 0; i < VizData::waveformPoints; i += samplesPerSegment)
    {
        // Average samples in this segment
        float sum = 0.0f;
        int   count = 0;
        for (int j = 0; j < samplesPerSegment && (i + j) < VizData::waveformPoints; ++j)
        {
            sum += outputWaveform[i + j];
            count++;
        }
        if (count > 0)
        {
            float avgSample = juce::jlimit(-1.0f, 1.0f, sum / (float)count);

            // Quantize to pixel height (bit depth dependent only)
            const float quantizedLevel = std::floor(avgSample / gridStep + 0.5f) * gridStep;
            const float quantizedY = midY - juce::jlimit(-1.0f, 1.0f, quantizedLevel) * scaleY;

            const float x1 = origin.x + i * stepX;
            const float x2 =
                origin.x +
                juce::jmin((float)VizData::waveformPoints - 1, (float)(i + samplesPerSegment)) *
                    stepX;

            // Draw pixel rectangle (height represents quantization, width is just for smoothness)
            const float y1 = quantizedY - pixelHeight * 0.5f * scaleY / 16.0f;
            const float y2 = quantizedY + pixelHeight * 0.5f * scaleY / 16.0f;

            // Mode-specific styling with theme color variations
            ImU32 pixelColor = outputWaveformColor;
            if (currentQuantMode == 1) // Dither - slightly lighter/more transparent
            {
                const ImVec4 timbreVec4 = ImGui::ColorConvertU32ToFloat4(timbreColor);
                pixelColor = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(timbreVec4.x, timbreVec4.y, timbreVec4.z, 0.85f));
            }
            else if (currentQuantMode == 2) // Noise Shaping - slightly brighter
            {
                const ImVec4 timbreVec4 = ImGui::ColorConvertU32ToFloat4(timbreColor);
                // Brighten slightly for noise shaping
                pixelColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    juce::jmin(1.0f, timbreVec4.x * 1.1f),
                    juce::jmin(1.0f, timbreVec4.y * 1.1f),
                    juce::jmin(1.0f, timbreVec4.z * 1.1f),
                    0.95f));
            }

            drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), pixelColor, 0.0f);
        }
    }

    // Draw sample-and-hold regions as horizontal flat segments ON TOP (shows sample rate reduction)
    // These appear AFTER quantization pixelation so they're clearly visible
    for (int i = 0; i < activeHoldCount; ++i)
    {
        const float startNorm = holdStarts[i];
        const float endNorm = holdEnds[i];
        const float holdValue = holdVals[i];

        if (startNorm >= 0.0f && endNorm >= 0.0f && startNorm <= 1.0f && endNorm <= 1.0f &&
            endNorm > startNorm)
        {
            const float startX = origin.x + startNorm * vizWidth;
            const float endX = origin.x + endNorm * vizWidth;
            const float holdY = midY - juce::jlimit(-1.0f, 1.0f, holdValue) * scaleY;

            // Draw horizontal flat line for the hold (thick, on top for visibility)
            drawList->AddLine(
                ImVec2(startX, holdY),
                ImVec2(endX, holdY),
                holdRegionColor,
                3.0f); // Thick line to clearly show hold

            // Draw subtle outline around hold for extra visibility
            const ImVec4 holdOutlineVec4 = ImGui::ColorConvertU32ToFloat4(holdRegionColor);
            const ImU32  holdOutlineColor = ImGui::ColorConvertFloat4ToU32(
                ImVec4(holdOutlineVec4.x, holdOutlineVec4.y, holdOutlineVec4.z, 0.6f));
            drawList->AddLine(
                ImVec2(startX, holdY - 1.0f), ImVec2(endX, holdY - 1.0f), holdOutlineColor, 1.0f);
            drawList->AddLine(
                ImVec2(startX, holdY + 1.0f), ImVec2(endX, holdY + 1.0f), holdOutlineColor, 1.0f);
        }
    }

    // Draw center line - use muted quant grid color
    const ImVec4 centerLineVec4 = ImGui::ColorConvertU32ToFloat4(quantGridColorBase);
    const ImU32  centerLineColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(centerLineVec4.x, centerLineVec4.y, centerLineVec4.z, 0.4f));
    drawList->AddLine(ImVec2(origin.x, midY), ImVec2(rectMax.x, midY), centerLineColor, 1.0f);

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, rectMax.y));
    ImGui::Dummy(ImVec2(vizWidth, 0));

    // Parameter meters - use accent color for progress bars
    ImGui::Text("Bit Depth: %.1f bits", currentBitDepth);
    float bitDepthMeter = (currentBitDepth - 1.0f) / 23.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
    ImGui::ProgressBar(bitDepthMeter, ImVec2(itemWidth * 0.5f, 0), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%.0f%%", bitDepthMeter * 100.0f);

    ImGui::Text("Sample Rate: %.3fx", currentSampleRate);
    float sampleRateMeter = (currentSampleRate - 0.1f) / 0.9f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
    ImGui::ProgressBar(sampleRateMeter, ImVec2(itemWidth * 0.5f, 0), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%.0f%%", sampleRateMeter * 100.0f);

    // Quantization info
    const char* modeNames[] = {"Linear", "Dither (TPDF)", "Noise Shaping"};
    ImGui::Text(
        "Quant Mode: %s | AA: %s", modeNames[currentQuantMode], currentAntiAlias ? "ON" : "OFF");

    ImGui::Spacing();
    ImGui::Spacing();

    // Bit Depth
    // Use virtual _mod ID to check for modulation as per BestPracticeNodeProcessor.md
    bool isBitDepthModulated = isParamModulated(paramIdBitDepthMod);
    // Get live value if modulated, otherwise use base parameter value
    float bitDepth = isBitDepthModulated
                         ? getLiveParamValueFor(
                               paramIdBitDepthMod,
                               "bit_depth_live",
                               bitDepthParam != nullptr ? bitDepthParam->load() : 16.0f)
                         : (bitDepthParam != nullptr ? bitDepthParam->load() : 16.0f);
    if (isBitDepthModulated)
    {
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Bit Depth Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2)) // Channel 2 = Bit Depth Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat(
            "Bit Depth", &bitDepth, 1.0f, 24.0f, "%.2f bits", ImGuiSliderFlags_Logarithmic))
    {
        if (!isBitDepthModulated)
        {
            if (auto* p =
                    dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdBitDepth)))
                *p = bitDepth;
        }
    }
    if (!isBitDepthModulated)
        adjustParamOnWheel(ap.getParameter(paramIdBitDepth), paramIdBitDepth, bitDepth);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isBitDepthModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker(
        "Bit depth reduction (1-24 bits)\nLower values create more quantization "
        "artifacts\nLogarithmic scale for fine control");

    // Sample Rate
    // Use virtual _mod ID to check for modulation as per BestPracticeNodeProcessor.md
    bool isSampleRateModulated = isParamModulated(paramIdSampleRateMod);
    // Get live value if modulated, otherwise use base parameter value
    float sampleRate = isSampleRateModulated
                           ? getLiveParamValueFor(
                                 paramIdSampleRateMod,
                                 "sample_rate_live",
                                 sampleRateParam != nullptr ? sampleRateParam->load() : 1.0f)
                           : (sampleRateParam != nullptr ? sampleRateParam->load() : 1.0f);
    if (isSampleRateModulated)
    {
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Sample Rate Mod (channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3)) // Channel 3 = Sample Rate Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat(
            "Sample Rate", &sampleRate, 0.1f, 1.0f, "%.3fx", ImGuiSliderFlags_Logarithmic))
    {
        if (!isSampleRateModulated)
        {
            if (auto* p =
                    dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdSampleRate)))
                *p = sampleRate;
        }
    }
    if (!isSampleRateModulated)
        adjustParamOnWheel(ap.getParameter(paramIdSampleRate), paramIdSampleRate, sampleRate);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isSampleRateModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker(
        "Sample rate reduction (0.1x-1.0x)\nLower values create more aliasing and stuttering\n1.0x "
        "= full rate, 0.1x = 10% of original rate");

    // Mix
    // Use virtual _mod ID to check for modulation as per BestPracticeNodeProcessor.md
    bool isMixModulated = isParamModulated(paramIdMixMod);
    // Get live value if modulated, otherwise use base parameter value
    float mix = isMixModulated
                    ? getLiveParamValueFor(
                          paramIdMixMod, "mix_live", mixParam != nullptr ? mixParam->load() : 1.0f)
                    : (mixParam != nullptr ? mixParam->load() : 1.0f);
    if (isMixModulated)
    {
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Mix Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4)) // Channel 4 = Mix Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f"))
    {
        if (!isMixModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdMix)))
                *p = mix;
        }
    }
    if (!isMixModulated)
        adjustParamOnWheel(ap.getParameter(paramIdMix), paramIdMix, mix);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isMixModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker("Dry/wet mix (0-1)\n0 = clean, 1 = fully crushed");

    ImGui::Spacing();

    // Anti-Aliasing
    // Use virtual _mod ID to check for modulation as per BestPracticeNodeProcessor.md
    bool isAntiAliasModulated = isParamModulated(paramIdAntiAliasMod);
    // Get live value if modulated, otherwise use base parameter value
    bool antiAlias =
        isAntiAliasModulated
            ? (getLiveParamValueFor(
                   paramIdAntiAliasMod,
                   "antiAlias_live",
                   antiAliasParam != nullptr && antiAliasParam->load() > 0.5f ? 1.0f : 0.0f) > 0.5f)
            : (antiAliasParam != nullptr && antiAliasParam->load() > 0.5f);
    if (isAntiAliasModulated)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Anti-Aliasing", &antiAlias))
    {
        if (!isAntiAliasModulated)
        {
            if (auto* p =
                    dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdAntiAlias)))
                *p = antiAlias;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isAntiAliasModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker(
        "ON: Applies a low-pass filter before decimation to reduce aliasing.\nOFF: Disables the "
        "filter for a harsher, classic aliased sound.");

    // Quant Mode
    // Use virtual _mod ID to check for modulation as per BestPracticeNodeProcessor.md
    bool isQuantModeModulated = isParamModulated(paramIdQuantModeMod);
    // Get live value if modulated, otherwise use base parameter value
    int quantMode =
        isQuantModeModulated
            ? static_cast<int>(getLiveParamValueFor(
                  paramIdQuantModeMod,
                  "quant_mode_live",
                  quantModeParam != nullptr ? quantModeParam->load() : 0.0f))
            : (quantModeParam != nullptr ? static_cast<int>(quantModeParam->load()) : 0);
    if (isQuantModeModulated)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Combo("Quant Mode", &quantMode, "Linear\0Dither (TPDF)\0Noise Shaping\0\0"))
    {
        if (!isQuantModeModulated)
        {
            if (auto* p =
                    dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdQuantMode)))
                *p = quantMode;
        }
    }
    if (!isQuantModeModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newMode = juce::jlimit(0, 2, quantMode + (wheel > 0.0f ? -1 : 1));
            if (newMode != quantMode)
            {
                quantMode = newMode;
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(
                        ap.getParameter(paramIdQuantMode)))
                    *p = quantMode;
                onModificationEnded();
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isQuantModeModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker(
        "Quantization Algorithm:\nLinear: Basic, harsh quantization.\nDither: Adds noise to reduce "
        "artifacts.\nNoise Shaping: Pushes quantization noise into higher, less audible "
        "frequencies.");

    ImGui::Spacing();
    ImGui::Spacing();

    // === RELATIVE MODULATION SECTION ===
    ThemeText("CV Input Modes", theme.modulation.frequency);
    ImGui::Spacing();

    // Relative Bit Depth Mod checkbox
    bool relativeBitDepthMod =
        relativeBitDepthModParam != nullptr && relativeBitDepthModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Bit Depth Mod", &relativeBitDepthMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeBitDepthMod")))
            *p = relativeBitDepthMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±12 bits)\nOFF: CV directly sets bit depth (1-24)");
    }

    // Relative Sample Rate Mod checkbox
    bool relativeSampleRateMod =
        relativeSampleRateModParam != nullptr && relativeSampleRateModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Sample Rate Mod", &relativeSampleRateMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeSampleRateMod")))
            *p = relativeSampleRateMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±3 octaves)\nOFF: CV directly sets sample rate "
            "(0.1x-1.0x)");
    }

    // Relative Mix Mod checkbox
    bool relativeMixMod = relativeMixModParam != nullptr && relativeMixModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Mix Mod", &relativeMixMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMixMod")))
            *p = relativeMixMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets mix (0-1)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void BitCrusherModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-6) are now inline in drawParametersInNode
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Bit Depth Mod (ch 2), Sample Rate Mod (ch 3), Mix Mod (ch 4), Anti-Alias Mod (ch 5), Quant
    // Mode Mod (ch 6) are drawn inline
}
#endif
