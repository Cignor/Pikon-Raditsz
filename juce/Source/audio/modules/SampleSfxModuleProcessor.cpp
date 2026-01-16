#include "SampleSfxModuleProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../../utils/RtLogger.h"
#include <cstring> // For std::strlen

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include "../../preset_creator/theme/ThemeManager.h"
#endif

SampleSfxModuleProcessor::SampleSfxModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Pitch Var Mod",
                  juce::AudioChannelSet::discreteChannels(1),
                  true) // Bus 0: Pitch Variation Mod (flat ch 0)
              .withInput(
                  "Control Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 1: Gate Mod, Trigger (flat ch 1-2)
              .withInput(
                  "Range Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 2: Range Start, Range End (flat ch 3-4)
              .withOutput("Audio Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SampleSfxParameters", createParameterLayout())
{
    // Initialize output value tracking for cable inspector (stereo)
    lastOutputValues.clear();
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));

    // Initialize parameter pointers
    pitchVariationParam = apvts.getRawParameterValue("pitchVariation");
    pitchVariationModParam = apvts.getRawParameterValue("pitchVariation_mod");
    gateParam = apvts.getRawParameterValue("gate");
    gateModParam = apvts.getRawParameterValue("gate_mod");
    selectionModeParam = apvts.getRawParameterValue("selectionMode");
    rangeStartParam = apvts.getRawParameterValue("rangeStart");
    rangeEndParam = apvts.getRawParameterValue("rangeEnd");
    rangeStartModParam = apvts.getRawParameterValue("rangeStart_mod");
    rangeEndModParam = apvts.getRawParameterValue("rangeEnd_mod");

#if defined(PRESET_CREATOR_UI)
    // Initialize visualization data
    for (auto& v : vizData.waveformPreview)
        v.store(0.0f);
#endif
}

juce::AudioProcessorValueTreeState::ParameterLayout SampleSfxModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> parameters;

    // --- Selection Mode ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "selectionMode",
            "Selection Mode",
            juce::StringArray{"Sequential", "Random", "Off"},
            0));

    // --- Pitch Variation (small range: ±2 semitones) ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "pitchVariation", "Pitch Variation", -2.0f, 2.0f, 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "pitchVariation_mod", "Pitch Variation Mod", -2.0f, 2.0f, 0.0f));

    // --- Gate ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("gate", "Gate", 0.0f, 1.0f, 0.8f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("gate_mod", "Gate Mod", 0.0f, 1.0f, 1.0f));

    // --- Range Control ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeStart", "Range Start", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeEnd", "Range End", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeStart_mod", "Range Start Mod", 0.0f, 1.0f, 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeEnd_mod", "Range End Mod", 0.0f, 1.0f, 1.0f));

    // --- Engine (for SampleVoiceProcessor) ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "engine", "Engine", juce::StringArray{"RubberBand", "Naive"}, 1));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("rbWindowShort", "RB Window Short", true));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("rbPhaseInd", "RB Phase Independent", true));

    return {parameters.begin(), parameters.end()};
}

void SampleSfxModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);
    juce::Logger::writeToLog(
        "[Sample SFX] prepareToPlay sr=" + juce::String(sampleRate) +
        ", block=" + juce::String(samplesPerBlock));

    // Force enable all input buses
    for (int i = 0; i < getBusCount(true); ++i)
    {
        if (auto* bus = getBus(true, i))
        {
            if (!bus->isEnabled())
            {
                enableAllBuses();
                break;
            }
        }
    }

    // Auto-load folder from saved state if available
    if (currentFolderPath.isEmpty())
    {
        const auto savedPath = apvts.state.getProperty("folderPath").toString();
        if (savedPath.isNotEmpty())
        {
            juce::File folder(savedPath);
            if (folder.isDirectory())
            {
                setSampleFolder(folder);
            }
        }
    }

    // Create sample processor if we have a sample loaded
    if (currentSample != nullptr)
    {
        createSampleProcessor();
    }

    const bool shouldResume = resumeAfterPrepare.exchange(false);
    if (shouldResume)
    {
        const float cached = lastKnownNormalizedPosition.load();
        if (cached >= 0.0f)
            pendingResumeNormalized.store(cached);
    }
}

void SampleSfxModuleProcessor::releaseResources()
{
    snapshotPlaybackState();

    bool wasPlaying = false;
    {
        const juce::ScopedLock lock(processorSwapLock);
        if (sampleProcessor)
            wasPlaying = sampleProcessor->isPlaying;
    }

    resumeShouldPlay.store(wasPlaying && !isStopped.load());
    resumeAfterPrepare.store(true);
}

void SampleSfxModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midiMessages)
{
    // Get OUTPUT bus
    auto outBus = getBusBuffer(buffer, false, 0);

    // --- Setup and Safety Checks ---
    auto refreshCurrentProcessor = [&]() -> SampleVoiceProcessor* {
        if (auto* pending = newSampleProcessor.exchange(nullptr))
        {
            const juce::ScopedLock lock(processorSwapLock);
            processorToDelete = std::move(sampleProcessor);
            sampleProcessor.reset(pending);
        }

        SampleVoiceProcessor* refreshed = nullptr;
        {
            const juce::ScopedLock lock(processorSwapLock);
            refreshed = sampleProcessor.get();
        }
        return refreshed;
    };

    SampleVoiceProcessor* currentProcessor = refreshCurrentProcessor();
    if (currentProcessor == nullptr || currentSample == nullptr)
    {
        outBus.clear();
        return;
    }

    const float resumeNorm = pendingResumeNormalized.exchange(-1.0f);
    if (resumeNorm >= 0.0f)
    {
        if (currentSample && currentSample->stereo.getNumSamples() > 0)
        {
            const double totalSamples = (double)currentSample->stereo.getNumSamples();
            const double clampedSample =
                juce::jlimit(0.0, totalSamples, (double)resumeNorm * totalSamples);
            currentProcessor->setCurrentPosition(clampedSample);
            updateCachedPosition(clampedSample, totalSamples);
            if (resumeShouldPlay.exchange(false))
                currentProcessor->isPlaying = true;
        }
        else
        {
            pendingResumeNormalized.store(resumeNorm);
        }
    }

    // Multi-bus input architecture
    auto pitchVarBus = getBusBuffer(buffer, true, 0); // Bus 0: Pitch Variation Mod (flat ch 0)
    auto controlBus = getBusBuffer(buffer, true, 1);  // Bus 1: Gate Mod, Trigger (flat ch 1-2)
    auto rangeBus = getBusBuffer(buffer, true, 2);    // Bus 2: Range Start, Range End (flat ch 3-4)

    const int numSamples = buffer.getNumSamples();

    // --- Calculate Range Values ---
    float startNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
    float endNorm = rangeEndParam ? rangeEndParam->load() : 1.0f;

    // Apply range modulation if connected
    if (isParamInputConnected("rangeStart_mod") && rangeBus.getNumChannels() > 0)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, rangeBus.getReadPointer(0)[0]);
        startNorm = cv;
    }

    if (isParamInputConnected("rangeEnd_mod") && rangeBus.getNumChannels() > 1)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, rangeBus.getReadPointer(1)[0]);
        endNorm = cv;
    }

    // Ensure valid range window
    {
        const float minGap = 0.001f;
        if (startNorm >= endNorm)
        {
            const float midpoint = (startNorm + endNorm) * 0.5f;
            startNorm = juce::jlimit(0.0f, 1.0f - minGap, midpoint - minGap * 0.5f);
            endNorm = juce::jlimit(minGap, 1.0f, startNorm + minGap);
        }
    }

    // Update live telemetry
    setLiveParamValue("rangeStart_live", startNorm);
    setLiveParamValue("rangeEnd_live", endNorm);

    // --- Trigger Detection ---
    if (isParamInputConnected("trigger_mod") && controlBus.getNumChannels() > 1)
    {
        const float* trigSignal = controlBus.getReadPointer(1); // Control Bus Channel 1 = Trigger
        for (int i = 0; i < numSamples; ++i)
        {
            const bool trigHigh = trigSignal[i] > 0.5f;
            if (trigHigh && !lastTriggerHigh)
            {
                // Ensure the queued sample (if any) is ready before playback
                processTriggerQueue();
                currentProcessor = refreshCurrentProcessor();
                if (currentProcessor == nullptr)
                {
                    lastTriggerHigh = trigHigh;
                    break;
                }

                // Start playback immediately
                reset();
                sampleEndDetected = false;

                // Prepare the next sample for the following trigger
                queueNextSample();
                break;
            }
            lastTriggerHigh = trigHigh;
        }
        if (numSamples > 0)
            lastTriggerHigh = (controlBus.getReadPointer(1)[numSamples - 1] > 0.5f);
    }

    // --- Compute pitch variation (for telemetry and audio) ---
    // Calculate even when not playing so UI shows live values
    float pitchVar = pitchVariationParam ? pitchVariationParam->load() : 0.0f;
    if (isParamInputConnected("pitchVariation_mod") && pitchVarBus.getNumChannels() > 0)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, pitchVarBus.getReadPointer(0)[0]);
        // Map CV 0-1 to -2 to +2 semitones
        const float cvPitch = juce::jmap(cv, -2.0f, 2.0f);
        pitchVar += cvPitch;
    }
    pitchVar = juce::jlimit(-2.0f, 2.0f, pitchVar);

    // Update live telemetry for UI (regardless of play state)
    setLiveParamValue("pitchVariation_live", pitchVar);

    // --- Compute gate value (for telemetry and audio) ---
    // Calculate even when not playing so UI shows live values
    float        gateValue = gateParam ? gateParam->load() : 0.8f;
    const bool   isGateMod = isParamInputConnected("gate_mod") && controlBus.getNumChannels() > 0;
    const float* gateCV = isGateMod ? controlBus.getReadPointer(0) : nullptr;
    if (isGateMod && gateCV)
    {
        // Use first sample of CV for telemetry display
        gateValue = juce::jlimit(0.0f, 1.0f, gateCV[0]);
    }
    // Update live telemetry for UI (regardless of play state)
    setLiveParamValue("gate_live", gateValue);

    // --- Audio Rendering ---
    if (currentProcessor->isPlaying)
    {
        // Apply pitch variation to audio engine
        currentProcessor->setBasePitchSemitones(pitchVar);

        // Apply playback range
        const int sourceLength = currentSample->stereo.getNumSamples();
        currentProcessor->setPlaybackRange(startNorm * sourceLength, endNorm * sourceLength);

        // Set engine parameters
        const int engineIdx = (int)apvts.getRawParameterValue("engine")->load();
        currentProcessor->setEngine(
            engineIdx == 0 ? SampleVoiceProcessor::Engine::RubberBand
                           : SampleVoiceProcessor::Engine::Naive);
        currentProcessor->setRubberBandOptions(
            apvts.getRawParameterValue("rbWindowShort")->load() > 0.5f,
            apvts.getRawParameterValue("rbPhaseInd")->load() > 0.5f);
        currentProcessor->setLooping(false); // SFX mode: play once, then switch

        // Track playing state before rendering
        const bool wasPlayingBeforeRender = currentProcessor->isPlaying;

        // Generate audio
        try
        {
            juce::AudioBuffer<float> outputBuffer(
                outBus.getArrayOfWritePointers(), outBus.getNumChannels(), outBus.getNumSamples());
            currentProcessor->renderBlock(outputBuffer, midiMessages);
        }
        catch (...)
        {
            RtLogger::postf("[Sample SFX][FATAL] renderBlock exception");
            outBus.clear();
        }

        // Check if sample finished during this block (isPlaying became false during renderBlock)
        const bool sampleFinished = wasPlayingBeforeRender && !currentProcessor->isPlaying;

        if (sampleFinished)
        {
            sampleEndDetected = true;
        }

        // --- Gate Application to Audio ---
        if (isGateMod && gateCV)
        {
            for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
            {
                float* channelData = outBus.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    const float cv = juce::jlimit(0.0f, 1.0f, gateCV[i]);
                    channelData[i] *= cv;
                }
            }
        }
        else
        {
            // Apply main gate knob (only when not CV modulated)
            outBus.applyGain(gateValue);
        }

        // Note: Trigger queue processing is handled above when sampleFinished is detected
    }
    else
    {
        // Not playing: clear output
        outBus.clear();

        // Reset flag when playback stops (allows next trigger to work)
        if (sampleEndDetected)
        {
            sampleEndDetected = false;
            queueNextSample(); // preload the upcoming sample after playback finishes
        }
    }

    // Update output values for cable inspector using block peak
    if (lastOutputValues.size() >= 2)
    {
        auto peakAbs = [&](int ch) {
            if (ch >= outBus.getNumChannels())
                return 0.0f;
            const float* p = outBus.getReadPointer(ch);
            float        m = 0.0f;
            for (int i = 0; i < outBus.getNumSamples(); ++i)
                m = juce::jmax(m, std::abs(p[i]));
            return m;
        };
        if (lastOutputValues[0])
            lastOutputValues[0]->store(peakAbs(0));
        if (lastOutputValues[1])
            lastOutputValues[1]->store(peakAbs(1));
    }

    if (currentSample)
        updateCachedPosition(
            currentProcessor->getCurrentPosition(), (double)currentSample->stereo.getNumSamples());
    moduleIsPlaying.store(currentProcessor->isPlaying);
}

void SampleSfxModuleProcessor::reset()
{
    if (sampleProcessor != nullptr)
    {
        sampleProcessor->reset();
    }
}

void SampleSfxModuleProcessor::forceStop() { handleStopRequest(); }

void SampleSfxModuleProcessor::updateCachedPosition(double samplePos, double totalSamples)
{
    lastKnownSamplePosition.store(samplePos);
    if (totalSamples > 0.0)
    {
        const double normalized = juce::jlimit(0.0, 1.0, samplePos / totalSamples);
        lastKnownNormalizedPosition.store((float)normalized);
    }
}

void SampleSfxModuleProcessor::snapshotPlaybackState()
{
    double samplePos = lastKnownSamplePosition.load();
    double totalSamples = 0.0;

    {
        const juce::ScopedLock lock(processorSwapLock);
        if (sampleProcessor)
            samplePos = sampleProcessor->getCurrentPosition();
        if (currentSample)
            totalSamples = (double)currentSample->stereo.getNumSamples();
    }

    updateCachedPosition(samplePos, totalSamples);
}

void SampleSfxModuleProcessor::handlePauseRequest()
{
    snapshotPlaybackState();
    resumeAfterPrepare.store(true);
    resumeShouldPlay.store(false);
    isStopped.store(false);

    const juce::ScopedLock lock(processorSwapLock);
    if (sampleProcessor)
        sampleProcessor->isPlaying = false;
}

void SampleSfxModuleProcessor::handleStopRequest()
{
    snapshotPlaybackState();

    {
        const juce::ScopedLock queueGuard(queueLock);
        triggerQueue.clear();
    }

    std::shared_ptr<SampleBank::Sample> safeSample;
    double                              totalSamples = 0.0;
    {
        const juce::ScopedLock lock(processorSwapLock);
        safeSample = currentSample;
        if (sampleProcessor)
            sampleProcessor->isPlaying = false;
        if (safeSample)
            totalSamples = (double)safeSample->stereo.getNumSamples();
        const float  startNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
        const double startSample =
            totalSamples > 0.0 ? juce::jlimit(0.0, totalSamples, (double)startNorm * totalSamples)
                               : 0.0;
        if (sampleProcessor && totalSamples > 0.0)
            sampleProcessor->setCurrentPosition(startSample);
        lastKnownSamplePosition.store(startSample);
        lastKnownNormalizedPosition.store(
            totalSamples > 0.0 ? (float)juce::jlimit(0.0, 1.0, startSample / totalSamples) : 0.0f);
    }

    pendingResumeNormalized.store(-1.0f);
    resumeAfterPrepare.store(false);
    resumeShouldPlay.store(false);
    isStopped.store(true);
}

void SampleSfxModuleProcessor::handlePlayRequest()
{
    resumeAfterPrepare.store(false);
    resumeShouldPlay.store(false);
    isStopped.store(false);

    const juce::ScopedLock lock(processorSwapLock);
    if (sampleProcessor)
        sampleProcessor->isPlaying = true;
}

void SampleSfxModuleProcessor::setSampleFolder(const juce::File& folder)
{
    if (!folder.isDirectory())
    {
        juce::Logger::writeToLog("[Sample SFX] Invalid folder: " + folder.getFullPathName());
        return;
    }

    bool loadedFirstSample = false;

    {
        const juce::ScopedLock lock(folderLock);

        currentFolderPath = folder.getFullPathName();

        // Scan folder for audio files
        folderSamples.clear();
        folder.findChildFiles(
            folderSamples, juce::File::findFiles, false, "*.wav;*.mp3;*.flac;*.aiff;*.ogg");

        // Sort alphabetically for sequential mode
        folderSamples.sort();

        // Reset index
        currentSampleIndex = 0;

        // Load first sample if available
        if (folderSamples.size() > 0)
        {
            loadSample(folderSamples[0]);
            loadedFirstSample = true;
        }
        else
        {
            juce::Logger::writeToLog(
                "[Sample SFX] No audio files found in folder: " + currentFolderPath);
        }

        // Save to APVTS state
        apvts.state.setProperty("folderPath", currentFolderPath, nullptr);
    }

    if (loadedFirstSample)
    {
        queueNextSample(); // Pre-select the next sample for the first trigger
    }
    else
    {
        const juce::ScopedLock queueLockGuard(queueLock);
        triggerQueue.clear();
    }
}

void SampleSfxModuleProcessor::loadSample(const juce::File& file)
{
    // Validate file
    if (!file.existsAsFile())
    {
        juce::Logger::writeToLog("[Sample SFX] File does not exist: " + file.getFullPathName());
        return;
    }

    // Clear any pending queue entries referencing old samples
    {
        const juce::ScopedLock lock(queueLock);
        triggerQueue.clear();
    }

    // Load the original shared sample from the bank
    SampleBank                          sampleBank;
    std::shared_ptr<SampleBank::Sample> original;
    try
    {
        original = sampleBank.getOrLoad(file);
    }
    catch (...)
    {
        juce::Logger::writeToLog("[Sample SFX][FATAL] Exception in SampleBank::getOrLoad");
        return;
    }

    if (original == nullptr || original->stereo.getNumSamples() <= 0)
    {
        juce::Logger::writeToLog(
            "[Sample SFX] Failed to load sample or empty: " + file.getFullPathName());
        return;
    }

    currentSampleName = file.getFileName();
    currentSamplePath = file.getFullPathName();

    // Store sample metadata
    sampleDurationSeconds = (double)original->stereo.getNumSamples() / original->sampleRate;
    sampleSampleRate = (int)original->sampleRate;

    // Create a private stereo copy (preserve stereo or duplicate mono)
    const int numSamples = original->stereo.getNumSamples();
    auto      privateCopy = std::make_shared<SampleBank::Sample>();
    privateCopy->sampleRate = original->sampleRate;

    try
    {
        privateCopy->stereo.setSize(2, numSamples); // Always stereo output
        if (original->stereo.getNumChannels() <= 1)
        {
            // Mono source: duplicate to both L and R channels
            privateCopy->stereo.copyFrom(0, 0, original->stereo, 0, 0, numSamples);
            privateCopy->stereo.copyFrom(1, 0, original->stereo, 0, 0, numSamples);
        }
        else
        {
            // Stereo source: copy L and R channels
            privateCopy->stereo.copyFrom(0, 0, original->stereo, 0, 0, numSamples);
            privateCopy->stereo.copyFrom(1, 0, original->stereo, 1, 0, numSamples);
        }
    }
    catch (...)
    {
        juce::Logger::writeToLog("[Sample SFX][FATAL] Exception copying audio data");
        return;
    }

    // Atomically assign our private copy
    {
        const juce::ScopedLock lock(processorSwapLock);
        currentSample = privateCopy;
    }

#if defined(PRESET_CREATOR_UI)
    // Generate waveform preview for visualization
    generateWaveformPreview();
#endif

    // If the module is prepared, stage a new processor
    if (getSampleRate() > 0.0 && getBlockSize() > 0)
    {
        createSampleProcessor();
    }
}

void SampleSfxModuleProcessor::queueNextSample()
{
    int nextIndex = -1;

    {
        const juce::ScopedLock lock(folderLock);

        if (folderSamples.isEmpty())
            return;

        const int mode = selectionModeParam ? ((int)selectionModeParam->load()) : 0;
        nextIndex = currentSampleIndex;

        if (mode == 0)
        {
            // Sequential: wrap around
            nextIndex = (currentSampleIndex + 1) % folderSamples.size();
        }
        else if (mode == 1)
        {
            // Random selection (exclude current if > 1 sample)
            juce::Random rng(juce::Time::getMillisecondCounterHiRes());
            if (folderSamples.size() > 1)
            {
                do
                {
                    nextIndex = rng.nextInt(folderSamples.size());
                } while (nextIndex == currentSampleIndex && folderSamples.size() > 2);
            }
        }
        else // mode == 2 (Off)
        {
            // Off mode: keep current sample (repeat itself)
            nextIndex = currentSampleIndex;
        }
    }

    if (nextIndex < 0)
        return;

    // Queue the next sample index (thread-safe, keep only one entry)
    {
        const juce::ScopedLock queueLockGuard(queueLock);
        triggerQueue.clear();
        triggerQueue.add(nextIndex);
    }
}

void SampleSfxModuleProcessor::processTriggerQueue()
{
    int sampleIndexToPlay = -1;

    // Get next sample from queue (thread-safe)
    {
        const juce::ScopedLock lock(queueLock);
        if (triggerQueue.isEmpty())
            return;

        sampleIndexToPlay = triggerQueue.removeAndReturn(0); // Remove first item
    }

    // Resolve file to load outside queue lock
    juce::File sampleToLoad;
    {
        const juce::ScopedLock folderLockGuard(folderLock);
        if (sampleIndexToPlay >= 0 && sampleIndexToPlay < folderSamples.size())
        {
            currentSampleIndex = sampleIndexToPlay;
            sampleToLoad = folderSamples[currentSampleIndex];
        }
        else
        {
            return;
        }
    }

    if (sampleToLoad.existsAsFile())
        loadSample(sampleToLoad);
}

void SampleSfxModuleProcessor::switchToNextSample()
{
    // Legacy method - now uses queue system
    queueNextSample();
    processTriggerQueue();
}

void SampleSfxModuleProcessor::createSampleProcessor()
{
    const juce::ScopedLock lock(processorSwapLock);

    if (currentSample == nullptr)
    {
        return;
    }

    // Guard against double-creation and race with audio thread
    auto newProcessor = std::make_unique<SampleVoiceProcessor>(currentSample);

    // Set up the sample processor
    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int    bs = getBlockSize() > 0 ? getBlockSize() : 512;
    newProcessor->prepareToPlay(sr, bs);

    // Set playback range from parameters (defaults to full sample)
    const int   sourceLength = currentSample->stereo.getNumSamples();
    const float startNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
    const float endNorm = rangeEndParam ? rangeEndParam->load() : 1.0f;
    newProcessor->setPlaybackRange(startNorm * sourceLength, endNorm * sourceLength);

    // Reset position without starting playback - wait for trigger
    newProcessor->resetPosition();
    lastKnownSamplePosition.store(startNorm * sourceLength);
    lastKnownNormalizedPosition.store(startNorm);
    pendingResumeNormalized.store(-1.0f);
    isStopped.store(true);

    // Set parameters from our APVTS
    newProcessor->setZoneTimeStretchRatio(1.0f); // No time stretch for SFX
    newProcessor->setBasePitchSemitones(pitchVariationParam ? pitchVariationParam->load() : 0.0f);

    newSampleProcessor.store(newProcessor.release());
    juce::Logger::writeToLog("[Sample SFX] Staged new sample processor for: " + currentSampleName);
}

juce::String SampleSfxModuleProcessor::getCurrentSampleName() const { return currentSampleName; }

bool SampleSfxModuleProcessor::hasSampleLoaded() const
{
    const juce::ScopedLock lock(processorSwapLock);
    return currentSample != nullptr;
}

juce::File SampleSfxModuleProcessor::getLastFolder() const
{
    if (currentFolderPath.isNotEmpty())
    {
        return juce::File(currentFolderPath);
    }

    // Try to find a default samples folder
    auto appFile = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    auto dir = appFile.getParentDirectory();
    for (int i = 0; i < 8 && dir.exists(); ++i)
    {
        auto candidate = dir.getSiblingFile("audio").getChildFile("samples");
        if (candidate.exists() && candidate.isDirectory())
            return candidate;
        dir = dir.getParentDirectory();
    }

    return juce::File();
}

void SampleSfxModuleProcessor::setTimingInfo(const TransportState& state)
{
    ModuleProcessor::setTimingInfo(state);

    const bool previousTransportPlaying = lastTransportPlaying.exchange(state.isPlaying);
    const bool transportPlayEdge = state.isPlaying && !previousTransportPlaying;
    const bool transportStopEdge = !state.isPlaying && previousTransportPlaying;
    const TransportCommand lastCommand = state.lastCommand.load();

    if (transportStopEdge)
    {
        if (lastCommand == TransportCommand::Stop)
            handleStopRequest();
        else
            handlePauseRequest();
    }
    else if (transportPlayEdge)
    {
        if (resumeShouldPlay.exchange(false) || state.isPlaying)
            handlePlayRequest();
    }
}

void SampleSfxModuleProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree vt("SampleSfx");
    vt.setProperty("folderPath", currentFolderPath, nullptr);
    vt.setProperty("currentIndex", currentSampleIndex, nullptr);
    vt.setProperty("gate", gateParam ? gateParam->load() : 0.8f, nullptr);
    vt.setProperty(
        "pitchVariation", pitchVariationParam ? pitchVariationParam->load() : 0.0f, nullptr);
    vt.setProperty(
        "selectionMode", selectionModeParam ? (int)selectionModeParam->load() : 0, nullptr);
    vt.setProperty("engine", (int)apvts.getRawParameterValue("engine")->load(), nullptr);
    vt.setProperty(
        "rbWindowShort", apvts.getRawParameterValue("rbWindowShort")->load() > 0.5f, nullptr);
    vt.setProperty("rbPhaseInd", apvts.getRawParameterValue("rbPhaseInd")->load() > 0.5f, nullptr);

    if (auto xml = vt.createXml())
        copyXmlToBinary(*xml, destData);
}

void SampleSfxModuleProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (!xml)
        return;

    juce::ValueTree vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid())
        return;

    // Restore folder path
    currentFolderPath = vt.getProperty("folderPath").toString();
    if (currentFolderPath.isNotEmpty())
    {
        juce::File folder(currentFolderPath);
        if (folder.isDirectory())
        {
            setSampleFolder(folder);
            // Restore index if valid
            const int savedIndex = (int)vt.getProperty("currentIndex", 0);
            if (savedIndex >= 0 && savedIndex < folderSamples.size())
            {
                currentSampleIndex = savedIndex;
                if (folderSamples.size() > 0)
                    loadSample(folderSamples[currentSampleIndex]);
            }
        }
    }

    // Restore parameters
    if (auto* p = apvts.getParameter("gate"))
        p->setValueNotifyingHost(
            apvts.getParameterRange("gate").convertTo0to1((float)vt.getProperty("gate", 0.8f)));
    if (auto* p = apvts.getParameter("pitchVariation"))
        p->setValueNotifyingHost(apvts.getParameterRange("pitchVariation")
                                     .convertTo0to1((float)vt.getProperty("pitchVariation", 0.0f)));
    if (auto* p = apvts.getParameter("selectionMode"))
        p->setValueNotifyingHost((float)(int)vt.getProperty("selectionMode", 0));
    if (auto* p = apvts.getParameter("engine"))
        p->setValueNotifyingHost((float)(int)vt.getProperty("engine", 1));
    if (auto* p = apvts.getParameter("rbWindowShort"))
        p->setValueNotifyingHost((bool)vt.getProperty("rbWindowShort", true) ? 1.0f : 0.0f);
    if (auto* p = apvts.getParameter("rbPhaseInd"))
        p->setValueNotifyingHost((bool)vt.getProperty("rbPhaseInd", true) ? 1.0f : 0.0f);
}

juce::ValueTree SampleSfxModuleProcessor::getExtraStateTree() const
{
    juce::ValueTree extra("SampleSfxExtra");
    extra.setProperty("folderPath", currentFolderPath, nullptr);
    extra.setProperty("currentIndex", currentSampleIndex, nullptr);
    return extra;
}

void SampleSfxModuleProcessor::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.isValid() && tree.hasType("SampleSfxExtra"))
    {
        const juce::String folderPath = tree.getProperty("folderPath").toString();
        if (folderPath.isNotEmpty())
        {
            juce::File folder(folderPath);
            if (folder.isDirectory())
            {
                setSampleFolder(folder);
                const int savedIndex = (int)tree.getProperty("currentIndex", 0);
                if (savedIndex >= 0 && savedIndex < folderSamples.size())
                {
                    currentSampleIndex = savedIndex;
                    if (folderSamples.size() > 0)
                        loadSample(folderSamples[currentSampleIndex]);
                }
            }
        }
    }
}

bool SampleSfxModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    // Bus 0: Pitch Variation Mod - flat channel 0
    if (paramId == "pitchVariation_mod")
    {
        outBusIndex = 0;
        outChannelIndexInBus = 0;
        return true;
    }

    // Bus 1: Control Mods - flat channels 1-2
    if (paramId == "gate_mod")
    {
        outBusIndex = 1;
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == "trigger_mod")
    {
        outBusIndex = 1;
        outChannelIndexInBus = 1;
        return true;
    }

    // Bus 2: Range Mods - flat channels 3-4
    if (paramId == "rangeStart_mod")
    {
        outBusIndex = 2;
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == "rangeEnd_mod")
    {
        outBusIndex = 2;
        outChannelIndexInBus = 1;
        return true;
    }

    return false;
}

#if defined(PRESET_CREATOR_UI)
void SampleSfxModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    // Protect global ID space (prevents conflicts when multiple instances exist)
    ImGui::PushID(this);

    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // Folder selection button
    if (ImGui::Button("Select Folder", ImVec2(itemWidth * 0.48f, 0)))
    {
        juce::File startDir = getLastFolder();
        if (!startDir.exists())
            startDir = juce::File();
        folderChooser = std::make_unique<juce::FileChooser>("Select Sample Folder", startDir, "*");
        auto chooserFlags =
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        folderChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& fc) {
            try
            {
                auto folder = fc.getResult();
                if (folder != juce::File{} && folder.isDirectory())
                {
                    juce::Logger::writeToLog(
                        "[Sample SFX] User selected folder: " + folder.getFullPathName());
                    setSampleFolder(folder);
                }
            }
            catch (...)
            {
                juce::Logger::writeToLog(
                    "[Sample SFX][FATAL] Exception during folder chooser callback");
            }
        });
    }
    ImGui::SameLine();

    // Current folder info
    if (currentFolderPath.isNotEmpty())
    {
        const juce::String folderName = juce::File(currentFolderPath).getFileName();
        ImGui::Text("%s", folderName.toRawUTF8());
    }

    ImGui::Spacing();

    // Selection mode (Sequential/Random/Off)
    auto* selectionModeChoice =
        dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("selectionMode"));
    int         mode = selectionModeChoice ? selectionModeChoice->getIndex() : 0;
    const char* items[] = {"Sequential", "Random", "Off"};
    if (ImGui::Combo("Selection Mode", &mode, items, 3))
    {
        if (selectionModeChoice)
        {
            *selectionModeChoice = mode;
            onModificationEnded();
        }
    }
    // Scroll wheel support for combo (matches pattern from other modules)
    if (selectionModeChoice && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newMode = juce::jlimit(0, 2, mode + (wheel > 0.0f ? -1 : 1));
            if (newMode != mode)
            {
                mode = newMode;
                *selectionModeChoice = mode;
                onModificationEnded();
            }
        }
    }

    ImGui::Spacing();

    // Pitch variation slider
    bool pitchModulated = isParamModulated("pitchVariation_mod");
    if (pitchModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    // Use live telemetry value if modulated, otherwise use parameter
    float pitchVar = pitchModulated ? getLiveParamValueFor(
                                          "pitchVariation_mod",
                                          "pitchVariation_live",
                                          pitchVariationParam ? pitchVariationParam->load() : 0.0f)
                                    : (pitchVariationParam ? pitchVariationParam->load() : 0.0f);

    // Inline pin for Pitch Mod (Channel 0)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(0))
            ImGui::SameLine();
    }

    if (ImGui::SliderFloat("Pitch Variation", &pitchVar, -2.0f, 2.0f, "%.2f st"))
    {
        if (auto* p = apvts.getParameter("pitchVariation"))
        {
            p->setValueNotifyingHost(
                apvts.getParameterRange("pitchVariation").convertTo0to1(pitchVar));
            onModificationEnded();
        }
    }
    if (!pitchModulated)
        ModuleProcessor::adjustParamOnWheel(
            apvts.getParameter("pitchVariation"), "pitchVariation", pitchVar);
    if (pitchModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    // Gate slider
    bool gateModulated = isParamModulated("gate_mod");
    if (gateModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    // Use live telemetry value if modulated, otherwise use parameter
    float gate =
        gateModulated
            ? getLiveParamValueFor("gate_mod", "gate_live", gateParam ? gateParam->load() : 0.8f)
            : (gateParam ? gateParam->load() : 0.8f);

    // Inline pin for Gate Mod (Channel 1)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1))
            ImGui::SameLine();
    }

    if (ImGui::SliderFloat("Gate", &gate, 0.0f, 1.0f, "%.2f"))
    {
        if (!gateModulated)
        {
            if (auto* p = apvts.getParameter("gate"))
            {
                p->setValueNotifyingHost(apvts.getParameterRange("gate").convertTo0to1(gate));
                onModificationEnded();
            }
        }
    }
    if (!gateModulated)
        ModuleProcessor::adjustParamOnWheel(apvts.getParameter("gate"), "gate", gate);
    if (gateModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    ImGui::Spacing();

    // Range parameters with live modulation feedback
    bool rangeStartModulated = isParamModulated("rangeStart_mod");
    if (rangeStartModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    float rangeStart = rangeStartModulated ? getLiveParamValueFor(
                                                 "rangeStart_mod",
                                                 "rangeStart_live",
                                                 rangeStartParam ? rangeStartParam->load() : 0.0f)
                                           : (rangeStartParam ? rangeStartParam->load() : 0.0f);
    float rangeEnd = rangeEndParam ? rangeEndParam->load() : 1.0f;

    // Inline pin for Range Start Mod (Channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))
            ImGui::SameLine();
    }

    if (ImGui::SliderFloat("Range Start", &rangeStart, 0.0f, 1.0f, "%.3f"))
    {
        // Ensure start doesn't exceed end (leave at least 0.001 gap)
        rangeStart = juce::jmin(rangeStart, rangeEnd - 0.001f);
        if (auto* p = apvts.getParameter("rangeStart"))
        {
            p->setValueNotifyingHost(
                apvts.getParameterRange("rangeStart").convertTo0to1(rangeStart));
            onModificationEnded();
        }
    }
    if (!rangeStartModulated)
        ModuleProcessor::adjustParamOnWheel(
            apvts.getParameter("rangeStart"), "rangeStart", rangeStart);
    if (rangeStartModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    bool rangeEndModulated = isParamModulated("rangeEnd_mod");
    if (rangeEndModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    rangeEnd =
        rangeEndModulated
            ? getLiveParamValueFor(
                  "rangeEnd_mod", "rangeEnd_live", rangeEndParam ? rangeEndParam->load() : 1.0f)
            : (rangeEndParam ? rangeEndParam->load() : 1.0f);
    rangeStart =
        rangeStartParam ? rangeStartParam->load() : 0.0f; // Refresh rangeStart for validation

    // Inline pin for Range End Mod (Channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))
            ImGui::SameLine();
    }

    if (ImGui::SliderFloat("Range End", &rangeEnd, 0.0f, 1.0f, "%.3f"))
    {
        // Ensure end doesn't go below start (leave at least 0.001 gap)
        rangeEnd = juce::jmax(rangeEnd, rangeStart + 0.001f);
        if (auto* p = apvts.getParameter("rangeEnd"))
        {
            p->setValueNotifyingHost(apvts.getParameterRange("rangeEnd").convertTo0to1(rangeEnd));
            onModificationEnded();
        }
    }
    if (!rangeEndModulated)
        ModuleProcessor::adjustParamOnWheel(apvts.getParameter("rangeEnd"), "rangeEnd", rangeEnd);
    if (rangeEndModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    ImGui::Spacing();

    // Current sample info
    if (hasSampleLoaded())
    {
        ImGui::Text("Sample: %s", currentSampleName.toRawUTF8());
        const juce::ScopedLock lock(folderLock);
        ImGui::Text("Index: %d/%d", currentSampleIndex + 1, folderSamples.size());
        ImGui::Text("Duration: %.2f s", sampleDurationSeconds.load());

        // === WAVEFORM VISUALIZATION ===
        ImGui::Spacing();

        // Read visualization data BEFORE BeginChild (per guide)
        float waveformPreview[VizData::waveformPoints];
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            waveformPreview[i] = vizData.waveformPreview[i].load();
        }

        // Read range values BEFORE BeginChild (use live telemetry if modulated)
        const bool rangeStartMod = isParamModulated("rangeStart_mod");
        const bool rangeEndMod = isParamModulated("rangeEnd_mod");
        float      rangeStart =
            rangeStartMod ? getLiveParamValue(
                                "rangeStart_live", rangeStartParam ? rangeStartParam->load() : 0.0f)
                               : (rangeStartParam ? rangeStartParam->load() : 0.0f);
        float rangeEnd =
            rangeEndMod
                ? getLiveParamValue("rangeEnd_live", rangeEndParam ? rangeEndParam->load() : 1.0f)
                : (rangeEndParam ? rangeEndParam->load() : 1.0f);

        // Waveform visualization in child window (following VCO pattern)
        const auto& freqColors = theme.modules.frequency_graph;
        const auto  resolveColor = [](ImU32 value, ImU32 fallback) {
            return value != 0 ? value : fallback;
        };
        const float  waveHeight = 120.0f;
        const ImVec2 graphSize(itemWidth, waveHeight);

        if (ImGui::BeginChild(
                "SampleSfxWaveform",
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

            // Clip to graph area (always execute, no conditionals)
            drawList->PushClipRect(p0, p1, true);

            // Draw waveform
            const float scaleY = graphSize.y * 0.45f;
            const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

            const ImU32 waveformColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
            float       prevX = p0.x;
            float       prevY = midY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float sample = juce::jlimit(-1.0f, 1.0f, waveformPreview[i]);
                const float x = p0.x + i * stepX;
                const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
                if (i > 0)
                    drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), waveformColor, 2.0f);
                prevX = x;
                prevY = y;
            }

            // Draw range indicators (vertical lines showing playback range)
            const float rangeStartX = p0.x + rangeStart * graphSize.x;
            const float rangeEndX = p0.x + rangeEnd * graphSize.x;

            const ImU32 rangeColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);
            const ImU32 rangeFillColor = IM_COL32(255, 255, 0, 30); // Semi-transparent yellow fill

            // Fill range area
            drawList->AddRectFilled(
                ImVec2(rangeStartX, p0.y), ImVec2(rangeEndX, p1.y), rangeFillColor);

            // Draw range boundary lines
            drawList->AddLine(
                ImVec2(rangeStartX, p0.y), ImVec2(rangeStartX, p1.y), rangeColor, 2.0f);
            drawList->AddLine(ImVec2(rangeEndX, p0.y), ImVec2(rangeEndX, p1.y), rangeColor, 2.0f);

            drawList->PopClipRect();

            // Sample name overlay
            ImGui::SetCursorPos(ImVec2(4, 4));
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%s", currentSampleName.toRawUTF8());

            // Invisible drag blocker
            ImGui::SetCursorPos(ImVec2(0, 0));
            ImGui::InvisibleButton("##sampleSfxWaveformDrag", graphSize);
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::TextDisabled("No sample loaded");
    }

    ImGui::Spacing();

    // Drag & drop zone
    ImVec2 dropZoneSize = ImVec2(itemWidth, 60.0f);
    bool   isDragging = ImGui::GetDragDropPayload() != nullptr;

    if (isDragging)
    {
        float time = (float)ImGui::GetTime();
        float pulse = (std::sin(time * 8.0f) * 0.5f + 0.5f);
        float glow = (std::sin(time * 3.0f) * 0.3f + 0.7f);
        ImU32 fillColor =
            IM_COL32(0, (int)(180 * glow), (int)(220 * glow), (int)(100 + pulse * 155));
        ImU32 borderColor =
            IM_COL32((int)(100 * glow), (int)(255 * pulse), (int)(255 * pulse), 255);
        ImGui::PushStyleColor(ImGuiCol_Button, fillColor);
        ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
        ImGui::Button("##dropzone_sfx", dropZoneSize);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 120));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::Button("##dropzone_sfx", dropZoneSize);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    const char* text = isDragging ? "Drop Here!" : "Drop Sample Here";
    ImVec2      textSize = ImGui::CalcTextSize(text);
    ImVec2      textPos = ImGui::GetItemRectMin();
    textPos.x += (dropZoneSize.x - textSize.x) * 0.5f;
    textPos.y += (dropZoneSize.y - textSize.y) * 0.5f;
    ImU32 textColor = isDragging ? IM_COL32(100, 255, 255, 255) : IM_COL32(150, 150, 150, 200);
    ImGui::GetWindowDrawList()->AddText(textPos, textColor, text);

    // Make drop target
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SAMPLE_PATH"))
        {
            if (payload->Data != nullptr && payload->DataSize > 0)
            {
                const char* path = (const char*)payload->Data;
                if (path[payload->DataSize - 1] == '\0')
                {
                    size_t pathLen = std::strlen(path);
                    if (pathLen > 0 && pathLen < payload->DataSize)
                    {
                        juce::String safePath(path, pathLen);
                        juce::File   file(safePath);
                        if (file.existsAsFile())
                        {
                            try
                            {
                                // Get the folder containing this sample
                                juce::File folder = file.getParentDirectory();
                                if (folder.isDirectory())
                                {
                                    // Set the folder first (this scans and loads the first sample)
                                    setSampleFolder(folder);

                                    // Now find and load the specific dropped sample
                                    const juce::ScopedLock lock(folderLock);
                                    for (int i = 0; i < folderSamples.size(); ++i)
                                    {
                                        if (folderSamples[i].getFullPathName() == safePath)
                                        {
                                            currentSampleIndex = i;
                                            loadSample(folderSamples[i]);
                                            break;
                                        }
                                    }
                                }
                                else
                                {
                                    // Fallback: just load the sample if folder detection fails
                                    loadSample(file);
                                }
                                onModificationEnded();
                            }
                            catch (...)
                            {
                                juce::Logger::writeToLog(
                                    "[Sample SFX][FATAL] Exception during drag-drop load: " +
                                    safePath);
                            }
                        }
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopItemWidth();
    ImGui::PopID(); // Match PushID(this) at function start
}

void SampleSfxModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Modulation inputs
    // Pitch Var Mod (0), Gate Mod (1), Range Start (3), Range End (4) are now drawn inline
    helpers.drawParallelPins("Trigger", 2, nullptr, -1);

    // Audio outputs (stereo)
    helpers.drawParallelPins(nullptr, -1, "Out L", 0);
    helpers.drawParallelPins(nullptr, -1, "Out R", 1);
}
#endif

#if defined(PRESET_CREATOR_UI)
void SampleSfxModuleProcessor::generateWaveformPreview()
{
    // Clear waveform preview
    for (auto& v : vizData.waveformPreview)
        v.store(0.0f);

    if (currentSample == nullptr || currentSample->stereo.getNumSamples() == 0)
        return;

    const int numSamples = currentSample->stereo.getNumSamples();
    const int numChannels = currentSample->stereo.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // Downsample to waveformPoints
    const int stride = juce::jmax(1, numSamples / VizData::waveformPoints);

    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int sampleIdx = juce::jmin(i * stride, numSamples - 1);

        // Average channels for mono preview, or use left channel
        float sample = 0.0f;
        if (numChannels == 1)
        {
            sample = currentSample->stereo.getSample(0, sampleIdx);
        }
        else
        {
            // Use left channel (channel 0) for preview
            sample = currentSample->stereo.getSample(0, sampleIdx);
        }

        // Clamp and store
        sample = juce::jlimit(-1.0f, 1.0f, sample);
        vizData.waveformPreview[i].store(sample);
    }
}
#endif
