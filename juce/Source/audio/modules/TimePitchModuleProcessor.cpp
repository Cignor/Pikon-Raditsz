#include "TimePitchModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

static inline void ensureCapacity (juce::HeapBlock<float>& block, int frames, int channels, int& capacityFrames)
{
    if (frames > capacityFrames)
    {
        capacityFrames = juce::jmax (frames, capacityFrames * 2 + 128);
        block.allocate ((size_t) (capacityFrames * channels), true);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout TimePitchModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdSpeed, "Speed", juce::NormalisableRange<float> (0.25f, 4.0f, 0.0001f, 0.5f), 1.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdPitch, "Pitch (st)", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (paramIdEngine, "Engine", juce::StringArray { "RubberBand", "Naive" }, 0));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdSpeedMod, "Speed Mod", juce::NormalisableRange<float> (0.25f, 4.0f, 0.0001f, 0.5f), 1.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdPitchMod, "Pitch Mod", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdMix, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdBufferSeconds, "Buffer Headroom", juce::NormalisableRange<float> (0.25f, 8.0f, 0.01f), 5.0f));
    return { p.begin(), p.end() };
}

TimePitchModuleProcessor::TimePitchModuleProcessor()
    : ModuleProcessor (BusesProperties()
        .withInput ("Inputs", juce::AudioChannelSet::discreteChannels(5), true) // ch0 L in, ch1 R in, ch2 Speed Mod, ch3 Pitch Mod, ch4 Mix Mod
        .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TimePitchParams", createParameterLayout())
{
    speedParam     = apvts.getRawParameterValue (paramIdSpeed);
    pitchParam     = apvts.getRawParameterValue (paramIdPitch);
    speedModParam  = apvts.getRawParameterValue (paramIdSpeedMod);
    pitchModParam  = apvts.getRawParameterValue (paramIdPitchMod);
    mixParam       = apvts.getRawParameterValue (paramIdMix);
    bufferSecondsParam = apvts.getRawParameterValue (paramIdBufferSeconds);
    engineParam    = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter (paramIdEngine));

    lastOutputValues.clear();
    lastOutputValues.push_back (std::make_unique<std::atomic<float>> (0.0f));
    lastOutputValues.push_back (std::make_unique<std::atomic<float>> (0.0f));
    
    // Initialize smoothed values
    speedSm.reset(1.0f);
    pitchSm.reset(0.0f);
    smoothedMix.reset(1.0f);

#if defined(PRESET_CREATOR_UI)
    for (auto& v : vizData.speedHistory) v.store (1.0f);
    for (auto& v : vizData.pitchHistory) v.store (0.0f);
    for (auto& v : vizData.fifoHistory) v.store (0.0f);
    vizData.currentSpeed.store (1.0f);
    vizData.currentPitch.store (0.0f);
    vizData.fifoFill.store (0.0f);
    vizData.engineMode.store (0);
    vizData.historyHead.store (0);
    vizSampleAccumulator = 0;
    vizHistoryWrite = 0;
    vizUpdateSamples = 512;
#endif
}

void TimePitchModuleProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    timePitch.prepare (sampleRate, 2, samplesPerBlock);

    // Initialize FIFO based on user-adjustable buffer headroom
    const double bufferSecs = bufferSecondsParam ? bufferSecondsParam->load() : 5.0;
    lastBufferSeconds = bufferSecs;
    fifoSize = (int) (sampleRate * bufferSecs);
    if (fifoSize < samplesPerBlock * 4) fifoSize = samplesPerBlock * 4; // safety minimum
    inputFifo.setSize (2, fifoSize);
    abstractFifo.setTotalSize (fifoSize);
    abstractFifo.reset();

    interleavedInputCapacityFrames = 0;
    interleavedOutputCapacityFrames = 0;
    ensureCapacity (interleavedInput, samplesPerBlock, 2, interleavedInputCapacityFrames);
    ensureCapacity (interleavedOutput, samplesPerBlock * 2, 2, interleavedOutputCapacityFrames); // some headroom
    timePitch.reset();
    
    smoothedMix.reset(sampleRate, 0.01);
    
    // Initialize auto-drop cooldown (200ms) and overlap window (15ms for crossfade)
    autoDropCooldownSamples = (int) (sampleRate * 0.2);
    autoDropCooldownRemaining = 0;
    autoDropOverlapSamples = juce::jmax (64, (int) (sampleRate * 0.015)); // 15ms overlap
    pendingAutoDrop = false;
    pendingAutoDropAmount = 0;
    overlapBefore.allocate ((size_t) (autoDropOverlapSamples * 2), true);
    overlapAfter.allocate ((size_t) (autoDropOverlapSamples * 2), true);

#if defined(PRESET_CREATOR_UI)
    vizSampleAccumulator = 0;
    vizHistoryWrite = 0;
    vizUpdateSamples = sampleRate > 0.0
        ? juce::jmax (1, (int) std::round (sampleRate / 60.0))
        : samplesPerBlock;
    vizData.autoflushActive.store (0);
#endif
}

void TimePitchModuleProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    auto inBus = getBusBuffer(buffer, true, 0);  // Single bus
    auto outBus = getBusBuffer(buffer, false, 0);
    
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    // Check for manual flush request
    if (flushRequested.exchange (false))
    {
        abstractFifo.reset();
        inputFifo.clear();
        timePitch.reset();
    }

    // Check if buffer size parameter changed and resize if needed
    const double bufferSecs = bufferSecondsParam ? bufferSecondsParam->load() : 5.0;
    if (std::abs (bufferSecs - lastBufferSeconds) > 0.01)
    {
        const int newFifoSize = juce::jmax (numSamples * 4, (int) (sr * bufferSecs));
        if (newFifoSize != fifoSize)
        {
            fifoSize = newFifoSize;
            inputFifo.setSize (2, fifoSize);
            abstractFifo.setTotalSize (fifoSize);
            abstractFifo.reset();
            timePitch.reset();
        }
        lastBufferSeconds = bufferSecs;
    }

    // 1) Write incoming audio into FIFO with back-pressure
    const int freeSpace = abstractFifo.getFreeSpace();
    const int samplesToWrite = juce::jmin (numSamples, freeSpace);
    
    int start1=0,size1=0,start2=0,size2=0;
    if (samplesToWrite > 0)
    {
        abstractFifo.prepareToWrite (samplesToWrite, start1, size1, start2, size2);
        if (size1 > 0)
        {
            inputFifo.copyFrom (0, start1, inBus, 0, 0, size1);
            inputFifo.copyFrom (1, start1, inBus, 1, 0, size1);
        }
        if (size2 > 0)
        {
            inputFifo.copyFrom (0, start2, inBus, 0, size1, size2);
            inputFifo.copyFrom (1, start2, inBus, 1, size1, size2);
        }
        const int written = size1 + size2;
        abstractFifo.finishedWrite (written);
    }
    // If we couldn't write all samples, we've hit back-pressure (buffer full)

    // 2) Read params and configure engine
    const int engineIdx = engineParam != nullptr ? engineParam->getIndex() : 0;
    const auto requestedMode = (engineIdx == 0 ? TimePitchProcessor::Mode::RubberBand
                                               : TimePitchProcessor::Mode::Fifo);
    if (requestedMode != lastMode)
    {
        timePitch.reset();
        lastMode = requestedMode;
    }
    timePitch.setMode (requestedMode);

    // Get pointers to modulation CV inputs
    const bool isSpeedMod = isParamInputConnected(paramIdSpeedMod);
    const bool isPitchMod = isParamInputConnected(paramIdPitchMod);
    const bool isMixMod = isParamInputConnected(paramIdMixMod);
    
    // SAFE: Read input pointers BEFORE any output operations
    const float* speedCV = isSpeedMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* pitchCV = isPitchMod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;
    const float* mixCV = isMixMod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;
    
    // Get base parameter values ONCE
    const float baseMix = mixParam != nullptr ? mixParam->load() : 1.0f;
    
    // Store dry signal for mixing
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.setSize(2, numSamples, false, false, true);
    for (int ch = 0; ch < juce::jmin(2, inBus.getNumChannels()); ++ch)
        dryBuffer.copyFrom(ch, 0, inBus, ch, 0, numSamples);
    
    // Process in slices to reduce engine reconfig cost
    const int sliceSize = 32;
    double maxConsumptionRatio = 0.0;
    double lastPlaybackSpeed = juce::jlimit (0.25, 4.0, (double) speedSm.getCurrentValue());
    for (int sliceStart = 0; sliceStart < numSamples; sliceStart += sliceSize)
    {
        const int sliceEnd = juce::jmin(sliceStart + sliceSize, numSamples);
        const int sliceSamples = sliceEnd - sliceStart;
        
        // Calculate target values from CV (use middle of slice)
        const int midSample = sliceStart + sliceSamples / 2;
        
        float targetSpeed = speedParam->load();
        if (isSpeedMod && speedCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, speedCV[midSample]);
            const float minSpeed = 0.25f;
            const float maxSpeed = 4.0f;
            targetSpeed = minSpeed * std::pow(maxSpeed / minSpeed, cv);
        }
        
        float targetPitch = pitchParam->load();
        if (isPitchMod && pitchCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, pitchCV[midSample]);
            targetPitch = -24.0f + cv * 48.0f; // -24 to +24 semitones
        }
        
        // Set targets and advance smoothing
        speedSm.setTargetValue(juce::jlimit(0.25f, 4.0f, targetSpeed));
        pitchSm.setTargetValue(juce::jlimit(-24.0f, 24.0f, targetPitch));
        
        for (int i = 0; i < sliceSamples; ++i) {
            speedSm.skip(1);
            pitchSm.skip(1);
        }
        
        const double playbackSpeed = juce::jlimit (0.25, 4.0, (double) speedSm.getCurrentValue());
        const double pitchSemis = juce::jlimit (-24.0, 24.0, (double) pitchSm.getCurrentValue());
        const double ratioForEngine = (requestedMode == TimePitchProcessor::Mode::RubberBand)
                                        ? (1.0 / juce::jmax (0.01, playbackSpeed))
                                        : playbackSpeed;
        
        timePitch.setTimeStretchRatio(ratioForEngine);
        timePitch.setPitchSemitones(pitchSemis);
        
        const double pitchFactor = std::pow (2.0, pitchSemis / 12.0);
        double consumptionCandidate = playbackSpeed;
        if (requestedMode == TimePitchProcessor::Mode::Fifo)
            consumptionCandidate *= pitchFactor;
        maxConsumptionRatio = juce::jmax (maxConsumptionRatio, consumptionCandidate);
        lastPlaybackSpeed = playbackSpeed;
        
        const float playbackSpeedF = static_cast<float> (playbackSpeed);
        const float pitchSemisF = static_cast<float> (pitchSemis);
        setLiveParamValue("speed_live", playbackSpeedF);
        setLiveParamValue("pitch_live", pitchSemisF);
        setLiveParamValue("speed", playbackSpeedF);
        setLiveParamValue("pitch", pitchSemisF);
    }

    // 3) Compute frames needed to fill this block
    if (maxConsumptionRatio <= 0.0)
        maxConsumptionRatio = juce::jlimit (0.25, 4.0, lastPlaybackSpeed);
    double adjustedConsumption = maxConsumptionRatio;
    int availableFrames = abstractFifo.getNumReady();
    
    // Autoflush: if buffer is >85% full, drop samples to prevent overflow
    const float fillRatio = fifoSize > 0 ? (float) availableFrames / (float) fifoSize : 0.0f;
    const double currentBufferSecs = juce::jmax (0.25, lastBufferSeconds);
    const float minFillSeconds = juce::jlimit (0.05f, 0.5f, (float) currentBufferSecs * 0.2f);
    const float minFillRatio = fifoSize > 0
        ? juce::jlimit (0.05f, 0.3f, minFillSeconds / (float) currentBufferSecs)
        : 0.2f;
    const float cautionRatio = juce::jlimit (minFillRatio + 0.05f, 0.6f, minFillRatio + 0.15f);
    const float highFillRatio = 0.80f;
    const bool buffering = fillRatio < minFillRatio;
    int autoflushState = 0; // 0=stable, 1=dropping, -1=draining, -2=buffering
    bool didAutoDrop = false;
    
    // Update cooldown
    autoDropCooldownRemaining = juce::jmax (0, autoDropCooldownRemaining - numSamples);
    const bool canAutoDrop = autoDropCooldownRemaining <= 0;
    
    // Auto-drop: only when buffer is >80% full and cooldown allows
    if (fillRatio > highFillRatio && canAutoDrop && availableFrames > numSamples + autoDropOverlapSamples)
    {
        const int targetFill = (int) (fifoSize * 0.65f);
        const int desiredDrop = juce::jmin (availableFrames - targetFill, (int) (sr * 0.3)); // Max 300ms
        if (desiredDrop > 0 && availableFrames >= autoDropOverlapSamples * 2)
        {
            // Read overlap window from BEFORE the drop point
            int ov1=0, ovSize1=0, ov2=0, ovSize2=0;
            abstractFifo.prepareToRead (autoDropOverlapSamples, ov1, ovSize1, ov2, ovSize2);
            auto* inL = inputFifo.getReadPointer (0);
            auto* inR = inputFifo.getReadPointer (1);
            float* ovBefore = overlapBefore.getData();
            for (int i = 0; i < ovSize1; ++i)
            {
                ovBefore[2*i+0] = inL[ov1 + i];
                ovBefore[2*i+1] = inR[ov1 + i];
            }
            if (ovSize2 > 0)
            {
                for (int i = 0; i < ovSize2; ++i)
                {
                    ovBefore[2*(ovSize1+i)+0] = inL[ov2 + i];
                    ovBefore[2*(ovSize1+i)+1] = inR[ov2 + i];
                }
            }
            abstractFifo.finishedRead (ovSize1 + ovSize2);
            
            // Advance past the drop
            const int dropAmount = desiredDrop;
            int drop1=0, dropSize1=0, drop2=0, dropSize2=0;
            abstractFifo.prepareToRead (dropAmount, drop1, dropSize1, drop2, dropSize2);
            abstractFifo.finishedRead (dropSize1 + dropSize2);
            
            // Read overlap window from AFTER the drop point
            abstractFifo.prepareToRead (autoDropOverlapSamples, ov1, ovSize1, ov2, ovSize2);
            float* ovAfter = overlapAfter.getData();
            for (int i = 0; i < ovSize1; ++i)
            {
                ovAfter[2*i+0] = inL[ov1 + i];
                ovAfter[2*i+1] = inR[ov1 + i];
            }
            if (ovSize2 > 0)
            {
                for (int i = 0; i < ovSize2; ++i)
                {
                    ovAfter[2*(ovSize1+i)+0] = inL[ov2 + i];
                    ovAfter[2*(ovSize1+i)+1] = inR[ov2 + i];
                }
            }
            abstractFifo.finishedRead (ovSize1 + ovSize2);
            
            // Crossfade the overlap windows
            for (int i = 0; i < autoDropOverlapSamples; ++i)
            {
                const float t = (float) i / (float) (autoDropOverlapSamples - 1);
                const float fadeOut = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * t));
                const float fadeIn = 1.0f - fadeOut;
                const int idx = 2 * i;
                ovBefore[idx] = ovBefore[idx] * fadeOut + ovAfter[idx] * fadeIn;
                ovBefore[idx + 1] = ovBefore[idx + 1] * fadeOut + ovAfter[idx + 1] * fadeIn;
            }
            
            // Feed the crossfaded overlap to the engine
            try { timePitch.putInterleaved (ovBefore, autoDropOverlapSamples); }
            catch (...) { /* swallow */ }
            
            availableFrames = abstractFifo.getNumReady();
            autoflushState = 1; // dropping
            autoDropCooldownRemaining = autoDropCooldownSamples;
            didAutoDrop = true;
        }
    }
    else if (buffering)
    {
        autoflushState = -2;
    }
    else if (fillRatio < cautionRatio)
    {
        autoflushState = -1; // draining (low fill)
    }
    
    if (buffering)
    {
        adjustedConsumption = 0.0;
    }
    else if (fillRatio < cautionRatio)
    {
        const double span = juce::jmax (0.001f, cautionRatio - minFillRatio);
        const double t = juce::jlimit (0.0, 1.0, (fillRatio - minFillRatio) / span);
        const double softLimit = 1.0 + t * (maxConsumptionRatio - 1.0);
        adjustedConsumption = juce::jmin (maxConsumptionRatio, softLimit);
    }
    
    const int framesRequired = adjustedConsumption > 0.0
        ? juce::jmax (1, (int) std::ceil ((double) numSamples * adjustedConsumption))
        : 0;
    // If we did an auto-drop, we already fed the overlap, so continue with normal processing
    // but account for the overlap we already consumed
    const int framesToFeed = framesRequired > 0 ? juce::jmin (framesRequired, availableFrames) : 0;

    outBus.clear();
    int outFramesWritten = 0;
    // Continue normal processing even after auto-drop (overlap was already fed)
    if (framesToFeed > 0)
    {
        // 4) Read from FIFO and interleave
        ensureCapacity (interleavedInput, framesToFeed, 2, interleavedInputCapacityFrames);
        abstractFifo.prepareToRead (framesToFeed, start1, size1, start2, size2);
        auto* inL = inputFifo.getReadPointer (0);
        auto* inR = inputFifo.getReadPointer (1);
        float* inLR = interleavedInput.getData();
        for (int i = 0; i < size1; ++i) { inLR[2*i+0] = inL[start1 + i]; inLR[2*i+1] = inR[start1 + i]; }
        if (size2 > 0)
            for (int i = 0; i < size2; ++i) { inLR[2*(size1+i)+0] = inL[start2 + i]; inLR[2*(size1+i)+1] = inR[start2 + i]; }
        const int readCount = size1 + size2;
        abstractFifo.finishedRead (readCount);

        // 5) Process and copy back
        // Guard against engine internal errors with try/catch (non-RT critical path)
        try { timePitch.putInterleaved (inLR, framesToFeed); }
        catch (...) { /* swallow to avoid crash; output will be silence */ }
        ensureCapacity (interleavedOutput, numSamples, 2, interleavedOutputCapacityFrames);
        int produced = 0;
        try { produced = timePitch.receiveInterleaved (interleavedOutput.getData(), numSamples); }
        catch (...) { produced = 0; }
        if (produced > 0)
        {
            const int outFrames = juce::jmin (numSamples, produced);
            outFramesWritten = outFrames;
            const float* outLR = interleavedOutput.getData();
            float* L = outBus.getNumChannels() > 0 ? outBus.getWritePointer (0) : buffer.getWritePointer(0);
            float* R = outBus.getNumChannels() > 1 ? outBus.getWritePointer (1) : L;
            
            // Apply dry/wet mix
            for (int i = 0; i < outFrames; ++i)
            {
                // Calculate mix with CV modulation
                float currentMix = baseMix;
                if (isMixMod && mixCV != nullptr) {
                    const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
                    currentMix = cv;
                }
                smoothedMix.setTargetValue(currentMix);
                const float mix = smoothedMix.getNextValue();
                const float dry = 1.0f - mix;
                
                const float wetL = outLR[2*i+0];
                const float wetR = outLR[2*i+1];
                const float dryL = dryBuffer.getSample(0, i);
                const float dryR = dryBuffer.getSample(1, i);
                
                L[i] = dry * dryL + mix * wetL;
                if (R) R[i] = dry * dryR + mix * wetR;
            }
        }
    }
    
    // Update live parameter value for telemetry
    if (numSamples > 0) {
        float finalMix = baseMix;
        if (isMixMod && mixCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[numSamples - 1]);
            finalMix = cv;
        }
        setLiveParamValue("mix_live", finalMix);
    }

#if defined(PRESET_CREATOR_UI)
    vizSampleAccumulator += numSamples;
    if (vizUpdateSamples <= 0)
        vizUpdateSamples = numSamples;

    const float fifoRatio = fifoSize > 0 ? juce::jlimit (0.0f, 1.0f, (float) abstractFifo.getNumReady() / (float) fifoSize) : 0.0f;
    while (vizSampleAccumulator >= vizUpdateSamples)
    {
        vizSampleAccumulator -= vizUpdateSamples;
        const float speedNow = juce::jlimit (0.25f, 4.0f, speedSm.getCurrentValue());
        const float pitchNow = juce::jlimit (-24.0f, 24.0f, pitchSm.getCurrentValue());

        vizData.currentSpeed.store (speedNow);
        vizData.currentPitch.store (pitchNow);
        vizData.fifoFill.store (fifoRatio);
        vizData.engineMode.store (requestedMode == TimePitchProcessor::Mode::RubberBand ? 0 : 1);
        vizData.autoflushActive.store (autoflushState);

        vizHistoryWrite = (vizHistoryWrite + 1) % VizData::historyPoints;
        vizData.speedHistory[vizHistoryWrite].store (speedNow);
        vizData.pitchHistory[vizHistoryWrite].store (pitchNow);
        vizData.fifoHistory[vizHistoryWrite].store (fifoRatio);
        vizData.historyHead.store (vizHistoryWrite);
    }
#endif

    // Update lastOutputValues
    if (lastOutputValues.size() >= 2)
    {
        lastOutputValues[0]->store (buffer.getMagnitude (0, 0, numSamples));
        lastOutputValues[1]->store (buffer.getNumChannels() > 1 ? buffer.getMagnitude (1, 0, numSamples) : 0.0f);
    }
}

// Parameter bus contract implementation
bool TimePitchModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0;
    if (paramId == paramIdSpeedMod) { outChannelIndexInBus = 2; return true; }  // Speed Mod
    if (paramId == paramIdPitchMod) { outChannelIndexInBus = 3; return true; }  // Pitch Mod
    if (paramId == paramIdMixMod) { outChannelIndexInBus = 4; return true; }  // Mix Mod
    return false;
}

#if defined(PRESET_CREATOR_UI)
void TimePitchModuleProcessor::drawParametersInNode (float itemWidth,
                                                    const std::function<bool(const juce::String& paramId)>& isParamModulated,
                                                    const std::function<void()>& onModificationEnded,
                                                    const NodePinHelpers* pinHelpers)
{
    ImGui::PushID (this);
    ImGui::PushItemWidth (itemWidth);
    auto& ap = getAPVTS();

    auto& themeMgr = ThemeManager::getInstance();
    const auto& theme = themeMgr.getCurrentTheme();
    const auto resolveColor = [] (ImU32 primary, ImU32 secondary, ImU32 fallback)
    {
        if (primary != 0) return primary;
        if (secondary != 0) return secondary;
        return fallback;
    };

    const ImU32 canvasBg = themeMgr.getCanvasBackground();
    const ImVec4 childBgVec4 = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
    const ImU32 defaultBg = ImGui::ColorConvertFloat4ToU32 (childBgVec4);
    const ImU32 panelBg = resolveColor (theme.modules.scope_plot_bg, canvasBg, defaultBg);
    const ImVec4 freqColorVec4 = theme.modulation.frequency;
    const ImVec4 timbreColorVec4 = theme.modulation.timbre;
    const ImVec4 accentVec4 = theme.accent;
    const ImU32 speedColor = resolveColor (0,
                                           ImGui::ColorConvertFloat4ToU32 (ImVec4 (freqColorVec4.x, freqColorVec4.y, freqColorVec4.z, 1.0f)),
                                           IM_COL32 (120, 200, 255, 255));
    const ImU32 pitchColor = resolveColor (0,
                                           ImGui::ColorConvertFloat4ToU32 (ImVec4 (timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f)),
                                           IM_COL32 (255, 140, 90, 255));
    const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32 (ImVec4 (accentVec4.x, accentVec4.y, accentVec4.z, 1.0f));
    const ImU32 textColor = ImGui::GetColorU32 (ImGuiCol_Text);

    ImGui::TextUnformatted ("Time / Pitch Monitor");
    const float vizHeight = 190.0f;
    const ImGuiWindowFlags vizFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild ("TimePitchViz", ImVec2 (itemWidth, vizHeight), false, vizFlags))
    {
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x <= 0.0f) canvasSize.x = itemWidth;
        if (canvasSize.y <= 0.0f) canvasSize.y = vizHeight;
        const ImVec2 canvasMax = ImVec2 (canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
        auto* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled (canvasPos, canvasMax, panelBg, 6.0f);
        drawList->AddRect (canvasPos, canvasMax, IM_COL32 (255, 255, 255, 20), 6.0f);

        ImGui::PushClipRect (canvasPos, canvasMax, true);

        std::array<float, VizData::historyPoints> speedHistory {};
        std::array<float, VizData::historyPoints> pitchHistory {};
        std::array<float, VizData::historyPoints> fifoHistory {};

        const int head = vizData.historyHead.load();
        for (int i = 0; i < VizData::historyPoints; ++i)
        {
            const int idx = (head + 1 + i) % VizData::historyPoints;
            speedHistory[i] = vizData.speedHistory[idx].load();
            pitchHistory[i] = vizData.pitchHistory[idx].load();
            fifoHistory[i] = vizData.fifoHistory[idx].load();
        }

        const float topHeight = canvasSize.y * 0.55f;
        const float ribbonHeight = canvasSize.y * 0.28f;
        const float barHeight = canvasSize.y - topHeight - ribbonHeight - 14.0f;

        const ImVec2 timeBoxMin = ImVec2 (canvasPos.x + 8.0f, canvasPos.y + 8.0f);
        const ImVec2 timeBoxMax = ImVec2 (canvasMax.x - 8.0f, canvasPos.y + topHeight);
        const ImVec2 pitchBoxMin = ImVec2 (canvasPos.x + 8.0f, timeBoxMax.y + 6.0f);
        const ImVec2 pitchBoxMax = ImVec2 (canvasMax.x - 8.0f, pitchBoxMin.y + ribbonHeight);
        const ImVec2 barBoxMin = ImVec2 (canvasPos.x + 8.0f, pitchBoxMax.y + 6.0f);
        const ImVec2 barBoxMax = ImVec2 (canvasMax.x - 8.0f, barBoxMin.y + juce::jmax (12.0f, barHeight));

        const ImU32 timeBg = ImGui::GetColorU32 (ImVec4 (childBgVec4.x, childBgVec4.y, childBgVec4.z, 0.9f));
        const ImU32 pitchBg = ImGui::GetColorU32 (ImVec4 (childBgVec4.x, childBgVec4.y, childBgVec4.z, 0.7f));
        const ImU32 barBg = IM_COL32 (20, 20, 25, 150);
        drawList->AddRectFilled (timeBoxMin, timeBoxMax, timeBg, 4.0f);
        drawList->AddRectFilled (pitchBoxMin, pitchBoxMax, pitchBg, 4.0f);
        drawList->AddRectFilled (barBoxMin, barBoxMax, barBg, 4.0f);

        const float speedMin = 0.25f;
        const float speedMax = 4.0f;
        auto drawHistory = [&drawList] (const std::array<float, VizData::historyPoints>& src,
                                        float minValue,
                                        float maxValue,
                                        ImVec2 boxMin,
                                        ImVec2 boxMax,
                                        ImU32 color,
                                        float thickness)
        {
            ImVec2 prevPoint {};
            for (int i = 0; i < VizData::historyPoints; ++i)
            {
                const float normX = (float) i / (float) (VizData::historyPoints - 1);
                const float width = boxMax.x - boxMin.x;
                const float height = boxMax.y - boxMin.y;
                const float value = juce::jlimit (minValue, maxValue, src[i]);
                const float ratio = (value - minValue) / (maxValue - minValue);
                float x = boxMin.x + normX * width;
                float y = boxMax.y - ratio * height;
                y = juce::jlimit (boxMin.y, boxMax.y, y);
                ImVec2 point (x, y);
                if (i > 0)
                    drawList->AddLine (prevPoint, point, color, thickness);
                prevPoint = point;
            }
        };

        // Grid lines inside time box
        const int gridLines = 3;
        for (int g = 0; g <= gridLines; ++g)
        {
            const float t = (float) g / (float) gridLines;
            const float y = timeBoxMin.y + t * (timeBoxMax.y - timeBoxMin.y);
            drawList->AddLine (ImVec2 (timeBoxMin.x, y), ImVec2 (timeBoxMax.x, y), IM_COL32 (255, 255, 255, 25));
        }

        drawHistory (speedHistory, speedMin, speedMax, timeBoxMin, timeBoxMax, speedColor, 2.5f);

        const float speedNow = vizData.currentSpeed.load();
        juce::String speedLabel = juce::String (speedNow, 2) + "x";
        drawList->AddText (ImVec2 (timeBoxMin.x + 4.0f, timeBoxMin.y + 2.0f), textColor, "Time Stretch");
        drawList->AddText (ImVec2 (timeBoxMax.x - 60.0f, timeBoxMin.y + 2.0f), speedColor, speedLabel.toRawUTF8());

        // Pitch ribbon
        const float pitchMin = -24.0f;
        const float pitchMax = 24.0f;
        const float midY = pitchBoxMin.y + (pitchBoxMax.y - pitchBoxMin.y) * 0.5f;
        drawList->AddLine (ImVec2 (pitchBoxMin.x, midY), ImVec2 (pitchBoxMax.x, midY), IM_COL32 (255, 255, 255, 30), 1.0f);

        ImVec2 prevTop = ImVec2 (pitchBoxMin.x, midY);
        for (int i = 0; i < VizData::historyPoints; ++i)
        {
            const float normX = (float) i / (float) (VizData::historyPoints - 1);
            const float width = pitchBoxMax.x - pitchBoxMin.x;
            const float span = pitchBoxMax.y - pitchBoxMin.y;
            const float value = juce::jlimit (pitchMin, pitchMax, pitchHistory[i]);
            const float ratio = (value - pitchMin) / (pitchMax - pitchMin);
            const float x = pitchBoxMin.x + normX * width;
            float y = pitchBoxMax.y - ratio * span;
            y = juce::jlimit (pitchBoxMin.y, pitchBoxMax.y, y);
            ImVec2 point (x, y);
            if (i > 0)
            {
                ImVec2 quad[4] = {
                    ImVec2 (prevTop.x, midY),
                    prevTop,
                    point,
                    ImVec2 (x, midY)
                };
                drawList->AddConvexPolyFilled (quad, 4, ImGui::GetColorU32 (ImVec4 (timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 0.25f)));
                drawList->AddLine (prevTop, point, pitchColor, 2.0f);
            }
            prevTop = point;
        }

        const float pitchNow = vizData.currentPitch.load();
        juce::String pitchLabel;
        pitchLabel << (pitchNow >= 0.0f ? "+" : "");
        pitchLabel << juce::String (pitchNow, 1) << " st";
        drawList->AddText (ImVec2 (pitchBoxMin.x + 4.0f, pitchBoxMin.y + 2.0f), textColor, "Pitch Offset");
        drawList->AddText (ImVec2 (pitchBoxMax.x - 70.0f, pitchBoxMin.y + 2.0f), pitchColor, pitchLabel.toRawUTF8());

        // FIFO fill
        const float fifoNow = juce::jlimit (0.0f, 1.0f, vizData.fifoFill.load());
        drawHistory (fifoHistory, 0.0f, 1.0f, barBoxMin, barBoxMax, ImGui::GetColorU32 (ImVec4 (1.0f, 1.0f, 1.0f, 0.25f)), 1.0f);
        const float fillWidth = (barBoxMax.x - barBoxMin.x) * fifoNow;
        drawList->AddRectFilled (barBoxMin, ImVec2 (barBoxMin.x + fillWidth, barBoxMax.y),
                                 fifoNow > 0.5f ? accentColor : speedColor, 3.0f);
        drawList->AddRect (barBoxMin, barBoxMax, IM_COL32 (255, 255, 255, 30), 3.0f);

        const bool isRubberBand = vizData.engineMode.load() == 0;
        const int autoflushState = vizData.autoflushActive.load();
        juce::String barLabel = (isRubberBand ? "RubberBand" : "Naive");
        barLabel << "  ";
        barLabel << juce::String (fifoNow * 100.0f, 1) << "% buffer";
        if (autoflushState == 1)
            barLabel << " [Auto-dropping]";
        else if (autoflushState == -1)
            barLabel << " [Low]";
        else if (autoflushState == -2)
            barLabel << " [Buffering]";
        drawList->AddText (ImVec2 (barBoxMin.x + 6.0f, barBoxMin.y + 2.0f), textColor, barLabel.toRawUTF8());

        ImGui::PopClipRect();
        ImGui::Dummy (canvasSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Parameters ---
    bool spMod = isParamModulated (paramIdSpeedMod);
    if (spMod) {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor (ImGuiCol_FrameBg, ImVec4 (1, 1, 0, 0.3f));
    }
    float speed = ap.getRawParameterValue (paramIdSpeed)->load();
    if (spMod) {
        speed = getLiveParamValueFor (paramIdSpeedMod, "speed_live", speed);
    }
    // Draw inline pin for Speed Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))  // Channel 2 = Speed Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat ("Speed", &speed, 0.25f, 4.0f, "%.2fx")) {
        if (!spMod) {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter (paramIdSpeed))) *p = speed;
        }
    }
    if (!spMod) adjustParamOnWheel (ap.getParameter (paramIdSpeed), paramIdSpeed, speed);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (spMod) { ImGui::PopStyleColor(); ImGui::EndDisabled(); }

    bool piMod = isParamModulated (paramIdPitchMod);
    if (piMod) {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor (ImGuiCol_FrameBg, ImVec4 (1, 1, 0, 0.3f));
    }
    float pitch = ap.getRawParameterValue (paramIdPitch)->load();
    if (piMod) {
        pitch = getLiveParamValueFor (paramIdPitchMod, "pitch_live", pitch);
    }
    // Draw inline pin for Pitch Mod (channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))  // Channel 3 = Pitch Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat ("Pitch", &pitch, -24.0f, 24.0f, "%.1f st"))
        if (!piMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter (paramIdPitch))) *p = pitch;
    if (!piMod) adjustParamOnWheel (ap.getParameter (paramIdPitch), paramIdPitch, pitch);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (piMod) { ImGui::PopStyleColor(); ImGui::EndDisabled(); }

    // Mix slider
    bool mixMod = isParamModulated (paramIdMixMod);
    if (mixMod) {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor (ImGuiCol_FrameBg, ImVec4 (1, 1, 0, 0.3f));
    }
    float mix = mixParam ? mixParam->load() : 1.0f;
    if (mixMod) {
        mix = getLiveParamValueFor (paramIdMixMod, "mix_live", mix);
    }
    // Draw inline pin for Mix Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))  // Channel 4 = Mix Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat ("Mix", &mix, 0.0f, 1.0f, "%.2f"))
        if (!mixMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter (paramIdMix))) *p = mix;
    if (!mixMod) adjustParamOnWheel (ap.getParameter (paramIdMix), paramIdMix, mix);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (mixMod) { ImGui::PopStyleColor(); ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    auto HelpMarker = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f); ImGui::TextUnformatted(desc); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    };
    HelpMarker("Wet/dry balance (0=dry, 1=wet)");

    int engineIdx = engineParam != nullptr ? engineParam->getIndex() : 0;
    const char* items[] = { "RubberBand", "Naive" };
    if (ImGui::Combo ("Engine", &engineIdx, items, 2))
        if (engineParam) *engineParam = engineIdx;
    if (engineParam && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newIdx = juce::jlimit(0, 1, engineIdx + (wheel > 0.0f ? -1 : 1));
            if (newIdx != engineIdx)
            {
                engineIdx = newIdx;
                *engineParam = engineIdx;
                onModificationEnded();
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Buffer management controls
    ThemeText ("Buffer Management", theme.text.section_header);
    ImGui::Spacing();

    float bufferSecs = bufferSecondsParam ? bufferSecondsParam->load() : 5.0f;
    if (ImGui::SliderFloat ("Buffer Headroom", &bufferSecs, 0.25f, 8.0f, "%.2f s"))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter (paramIdBufferSeconds)))
            *p = bufferSecs;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip ("Higher = safer slowdowns, adds latency\nLower = less latency, may drop samples");
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();

    ImGui::Spacing();
    if (ImGui::Button ("Flush Buffer", ImVec2 (itemWidth, 0)))
    {
        flushRequested.store (true);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip ("Clear buffer and reset playback (resets audio)");

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void TimePitchModuleProcessor::drawIoPins (const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-4) are now inline
    helpers.drawParallelPins ("In L", 0, "Out L", 0);
    helpers.drawParallelPins ("In R", 1, "Out R", 1);
    // Speed Mod (ch 2), Pitch Mod (ch 3), Mix Mod (ch 4) are drawn inline in drawParametersInNode
}
#endif

std::vector<DynamicPinInfo> TimePitchModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});
    
    // Modulation inputs (channels 2-4)
    pins.push_back({"Speed Mod", 2, PinDataType::CV});
    pins.push_back({"Pitch Mod", 3, PinDataType::CV});
    pins.push_back({"Mix Mod", 4, PinDataType::CV});
    
    return pins;
}

std::vector<DynamicPinInfo> TimePitchModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});
    
    return pins;
}



