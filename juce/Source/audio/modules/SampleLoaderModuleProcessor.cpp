#include "SampleLoaderModuleProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../../utils/RtLogger.h"
#include <cmath>   // For std::fmod
#include <cstring> // For std::strlen

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include "../../preset_creator/theme/ThemeManager.h"
#endif

SampleLoaderModuleProcessor::SampleLoaderModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Playback Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 0: Pitch, Speed (flat ch 0-1)
              .withInput(
                  "Control Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 1: Gate, Trigger (flat ch 2-3)
              .withInput(
                  "Range Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 2: Range Start, Range End (flat ch 4-5)
              .withInput(
                  "Randomize",
                  juce::AudioChannelSet::discreteChannels(1),
                  true) // Bus 3: Randomize (flat ch 6)
              .withInput(
                  "Position Mod",
                  juce::AudioChannelSet::discreteChannels(1),
                  true) // Bus 4: Position Mod (flat ch 7)
              .withOutput("Audio Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SampleLoaderParameters", createParameterLayout())
{
    // Parameter references will be obtained when needed
    // Initialize output value tracking for cable inspector (stereo)
    lastOutputValues.clear();
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));

    // Initialize parameter pointers
    rangeStartParam = apvts.getRawParameterValue("rangeStart");
    rangeEndParam = apvts.getRawParameterValue("rangeEnd");
    rangeStartModParam = apvts.getRawParameterValue("rangeStart_mod");
    rangeEndModParam = apvts.getRawParameterValue("rangeEnd_mod");

    // Initialize relative modulation parameter pointers
    relativeSpeedModParam = apvts.getRawParameterValue("relativeSpeedMod");
    relativePitchModParam = apvts.getRawParameterValue("relativePitchMod");
    relativeGateModParam = apvts.getRawParameterValue("relativeGateMod");
    relativeRangeStartModParam = apvts.getRawParameterValue("relativeRangeStartMod");
    relativeRangeEndModParam = apvts.getRawParameterValue("relativeRangeEndMod");

    // Initialize position parameter pointers
    positionParam = apvts.getRawParameterValue(paramIdPosition);
    positionModParam = apvts.getRawParameterValue(paramIdPositionMod);
    relativePositionModParam = apvts.getRawParameterValue(paramIdRelPosMod);

    // Initialize sync parameters
    syncParam = apvts.getRawParameterValue("sync");
    syncModeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("syncMode"));
    syncToTransport.store(syncParam && (*syncParam > 0.5f));
}

juce::AudioProcessorValueTreeState::ParameterLayout SampleLoaderModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> parameters;

    // --- Basic Playback Parameters ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("speed", "Speed", 0.25f, 4.0f, 1.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "pitch", "Pitch (semitones)", -24.0f, 24.0f, 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("gate", "Gate", 0.0f, 1.0f, 0.8f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "engine", "Engine", juce::StringArray{"RubberBand", "Naive"}, 1));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("rbWindowShort", "RB Window Short", true));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("rbPhaseInd", "RB Phase Independent", true));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "loop", "Loop", true)); // Default to true for continuous playback
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "sync", "Sync to Transport", false)); // Default to false - manual control by default
    parameters.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "syncMode",
            "Sync Mode",
            juce::StringArray{"Relative", "Absolute"},
            0)); // Default to Relative (range-based)

    // (Removed legacy SoundTouch tuning parameters)

    // --- New Modulation Inputs (absolute control) ---
    // These live in APVTS and are fed by modulation cables; they override UI when connected.
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("pitch_mod", "Pitch Mod", -24.0f, 24.0f, 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("speed_mod", "Speed Mod", 0.25f, 4.0f, 1.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>("gate_mod", "Gate Mod", 0.0f, 1.0f, 1.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "trigger_mod", "Trigger Mod", 0.0f, 1.0f, 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "randomize_mod", "Randomize Mod", 0.0f, 1.0f, 0.0f));

    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeStart_mod", "Range Start Mod", 0.0f, 1.0f, 0.0f));

    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeEnd_mod", "Range End Mod", 0.0f, 1.0f, 1.0f));

    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeStart", "Range Start", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));

    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rangeEnd", "Range End", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // --- Position Parameter ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdPosition, "Position", 0.0f, 1.0f, 0.0f));

    // --- Position Parameters ---
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdPosition, "Position", 0.0f, 1.0f, 0.0f));
    parameters.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdPositionMod, "Position Mod", 0.0f, 1.0f, 0.0f));

    // Relative modulation parameters
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeSpeedMod", "Relative Speed Mod", true));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("relativePitchMod", "Relative Pitch Mod", true));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeGateMod", "Relative Gate Mod", false));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "relativeRangeStartMod", "Relative Range Start Mod", false));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "relativeRangeEndMod", "Relative Range End Mod", false));
    parameters.push_back(
        std::make_unique<juce::AudioParameterBool>(
            paramIdRelPosMod, "Relative Position Mod", false));

    return {parameters.begin(), parameters.end()};
}

void SampleLoaderModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);
    juce::Logger::writeToLog(
        "[Sample Loader] prepareToPlay sr=" + juce::String(sampleRate) +
        ", block=" + juce::String(samplesPerBlock));

    // DEBUG: Check bus enablement status BEFORE forcing
    juce::String busStatusBefore = "[Sample Loader] Bus Status BEFORE: ";
    for (int i = 0; i < getBusCount(true); ++i)
    {
        auto* bus = getBus(true, i);
        if (bus)
            busStatusBefore += "In" + juce::String(i) + "=" + (bus->isEnabled() ? "ON" : "OFF") +
                               "(" + juce::String(bus->getNumberOfChannels()) + "ch) ";
    }
    juce::Logger::writeToLog(busStatusBefore);

    // FORCE ENABLE ALL INPUT BUSES (AudioProcessorGraph might disable them)
    for (int i = 0; i < getBusCount(true); ++i)
    {
        if (auto* bus = getBus(true, i))
        {
            if (!bus->isEnabled())
            {
                enableAllBuses(); // Try to enable all
                juce::Logger::writeToLog("[Sample Loader] Forced all buses ON!");
                break;
            }
        }
    }

    // DEBUG: Check bus enablement status AFTER forcing
    juce::String busStatusAfter = "[Sample Loader] Bus Status AFTER: ";
    for (int i = 0; i < getBusCount(true); ++i)
    {
        auto* bus = getBus(true, i);
        if (bus)
            busStatusAfter += "In" + juce::String(i) + "=" + (bus->isEnabled() ? "ON" : "OFF") +
                              "(" + juce::String(bus->getNumberOfChannels()) + "ch) ";
    }
    juce::Logger::writeToLog(busStatusAfter);

    // Auto-load sample from saved state if available
    if (currentSample == nullptr)
    {
        const auto savedPath = apvts.state.getProperty("samplePath").toString();
        if (savedPath.isNotEmpty())
        {
            currentSamplePath = savedPath;
            loadSample(juce::File(currentSamplePath));
        }
    }
    // Create sample processor if we have a sample loaded
    if (currentSample != nullptr)
    {
        createSampleProcessor();
    }
}

void SampleLoaderModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midiMessages)
{
    processorIsRendering.store(true, std::memory_order_relaxed);

    // === DEFERRED SAMPLE LOADING ===
    bool         shouldLoadAfterBlock = false;
    juce::String deferredRandomPath;

    if (hasPendingRandomSample.load())
    {
        const juce::ScopedLock lock(pendingRandomLock);
        if (hasPendingRandomSample.load())
        {
            deferredRandomPath = pendingRandomSamplePath;
            hasPendingRandomSample.store(false);
            pendingRandomSamplePath.clear();
            shouldLoadAfterBlock = !deferredRandomPath.isEmpty();
        }
    }

    // Get OUTPUT bus, but do NOT clear here.
    // Clearing at the start can zero input buses when buffers are aliased in AudioProcessorGraph.
    auto outBus = getBusBuffer(buffer, false, 0);

    // --- Setup and Safety Checks ---
    if (auto* pending = newSampleProcessor.exchange(nullptr))
    {
        const juce::ScopedLock lock(processorSwapLock);
        processorToDelete = std::move(sampleProcessor);
        sampleProcessor.reset(pending);
    }
    SampleVoiceProcessor* currentProcessor = nullptr;
    {
        const juce::ScopedLock lock(processorSwapLock);
        currentProcessor = sampleProcessor.get();
    }
    if (currentProcessor == nullptr || currentSample == nullptr)
    {
        outBus.clear();
        processorIsRendering.store(false, std::memory_order_relaxed);
        return;
    }

    // DEBUG: Check MAIN buffer first (before splitting into buses)
    static int rawDebugCounter = 0;
    if (rawDebugCounter == 0 || rawDebugCounter % 240 == 0)
    {
        juce::String mainMsg = "[Sample MAIN Buffer #" + juce::String(rawDebugCounter) + "] ";
        mainMsg += "totalCh=" + juce::String(buffer.getNumChannels()) +
                   " samples=" + juce::String(buffer.getNumSamples()) + " | ";

        // Check if buffer channels have valid pointers
        bool hasData = false;
        for (int ch = 0; ch < juce::jmin(7, buffer.getNumChannels()); ++ch)
        {
            float val = buffer.getSample(ch, 0);
            mainMsg += "ch" + juce::String(ch) + "=" + juce::String(val, 3) + " ";
            if (std::abs(val) > 0.001f)
                hasData = true;
        }

        mainMsg += "| hasData=" + juce::String(hasData ? "YES" : "NO");

        // Check channel pointer validity
        mainMsg += " | ptrs: ";
        for (int ch = 0; ch < juce::jmin(3, buffer.getNumChannels()); ++ch)
        {
            const float* ptr = buffer.getReadPointer(ch);
            mainMsg += "ch" + juce::String(ch) + "=" + (ptr ? "OK" : "NULL") + " ";
        }
        juce::Logger::writeToLog(mainMsg);
    }

    // Multi-bus input architecture (like TTS Performer)
    auto playbackBus = getBusBuffer(buffer, true, 0); // Bus 0: Pitch, Speed (flat ch 0-1)
    auto controlBus = getBusBuffer(buffer, true, 1);  // Bus 1: Gate, Trigger (flat ch 2-3)
    auto rangeBus = getBusBuffer(buffer, true, 2);    // Bus 2: Range Start, Range End (flat ch 4-5)
    auto randomizeBus = getBusBuffer(buffer, true, 3);   // Bus 3: Randomize (flat ch 6)
    auto positionModBus = getBusBuffer(buffer, true, 4); // Bus 4: Position Mod (flat ch 7)

    // DEBUG: Check buses after splitting
    if (rawDebugCounter == 0 || rawDebugCounter % 240 == 0)
    {
        juce::String busMsg = "[Sample Buses #" + juce::String(rawDebugCounter) + "] ";
        busMsg += "playback=" + juce::String(playbackBus.getNumChannels()) + " ";
        busMsg += "control=" + juce::String(controlBus.getNumChannels()) + " ";
        busMsg += "range=" + juce::String(rangeBus.getNumChannels()) + " ";
        busMsg += "randomize=" + juce::String(randomizeBus.getNumChannels()) + " | ";
        if (playbackBus.getNumChannels() > 0)
            busMsg += "pitch=" + juce::String(playbackBus.getSample(0, 0), 3) + " ";
        if (playbackBus.getNumChannels() > 1)
            busMsg += "speed=" + juce::String(playbackBus.getSample(1, 0), 3) + " ";
        if (controlBus.getNumChannels() > 0)
            busMsg += "gate=" + juce::String(controlBus.getSample(0, 0), 3) + " ";
        juce::Logger::writeToLog(busMsg);
    }
    rawDebugCounter++;

    const int numSamples = buffer.getNumSamples();

    // Keep syncToTransport in sync with syncParam (for preset loading)
    if (syncParam)
    {
        syncToTransport.store(*syncParam > 0.5f);
    }

    // --- 0. CALCULATE RANGE VALUES (needed for position scrubbing) ---
    // Calculate range start/end early so we can use them for position scrubbing
    float startNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
    float endNorm = rangeEndParam ? rangeEndParam->load() : 1.0f;

    // Apply range modulation if connected
    const bool relativeRangeStartMode =
        relativeRangeStartModParam != nullptr && relativeRangeStartModParam->load() > 0.5f;
    const bool relativeRangeEndMode =
        relativeRangeEndModParam != nullptr && relativeRangeEndModParam->load() > 0.5f;

    if (isParamInputConnected("rangeStart_mod") && rangeBus.getNumChannels() > 0)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, rangeBus.getReadPointer(0)[0]);
        if (relativeRangeStartMode)
        {
            const float rangeRange = 0.25f;
            const float rangeOffset = (cv - 0.5f) * (rangeRange * 2.0f);
            startNorm = juce::jlimit(0.0f, 1.0f, startNorm + rangeOffset);
        }
        else
        {
            startNorm = cv;
        }
    }

    if (isParamInputConnected("rangeEnd_mod") && rangeBus.getNumChannels() > 1)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, rangeBus.getReadPointer(1)[0]);
        if (relativeRangeEndMode)
        {
            const float rangeRange = 0.25f;
            const float rangeOffset = (cv - 0.5f) * (rangeRange * 2.0f);
            endNorm = juce::jlimit(0.0f, 1.0f, endNorm + rangeOffset);
        }
        else
        {
            endNorm = cv;
        }
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

    // --- 1. POSITION LOGIC (ANTI-FREEZE FIX) ---
    float currentPosParam = positionParam ? positionParam->load() : 0.0f;
    float targetPosNorm = currentPosParam;
    bool  isScrubbing = false;

    // A. Check for CV Modulation (Highest Priority)
    if (isParamInputConnected(paramIdPositionMod) && positionModBus.getNumChannels() > 0)
    {
        float cv = juce::jlimit(0.0f, 1.0f, positionModBus.getReadPointer(0)[0]);
        if (relativePositionModParam && relativePositionModParam->load() > 0.5f)
        {
            // Relative Mode: CV adds to slider value
            targetPosNorm = juce::jlimit(0.0f, 1.0f, currentPosParam + (cv - 0.5f));
        }
        else
        {
            // Absolute Mode: CV controls position directly
            targetPosNorm = cv;
        }
        // CRITICAL: Don't clamp to range here - allow position to be outside range
        // Range should only cap/limit playback boundaries, not restrict where playhead can be
        // positioned The setPlaybackRange() call later will handle limiting actual playback to the
        // range

        // Only scrub if CV-derived position changed significantly from last CV position
        // This prevents constant time stretcher resets while allowing smooth CV control
        // Use a threshold that allows smooth movement but prevents micro-movements from resetting
        if (std::abs(targetPosNorm - lastCvPosition) > 0.001f)
        {
            isScrubbing = true;
            lastCvPosition = targetPosNorm; // Track CV-derived position
        }
    }
    // B. Check for Manual Slider Movement (User Dragging UI)
    // We detect this by checking if the parameter value changed significantly
    // from what we *last set it to* in the audio thread.
    // Use a more lenient threshold to catch all user interactions
    else if (positionParam && std::abs(currentPosParam - lastUiPosition) > 0.00001f)
    {
        targetPosNorm = currentPosParam;
        // CRITICAL: Don't clamp to range here - allow position to be outside range
        // Range should only cap/limit playback boundaries, not restrict where playhead can be
        // positioned The setPlaybackRange() call later will handle limiting actual playback to the
        // range
        isScrubbing = true;
    }

    // Apply Position to Engine
    if (currentSample && sampleProcessor && positionParam)
    {
        const double totalSamples = (double)currentSample->stereo.getNumSamples();

        if (totalSamples > 0)
        {
            if (isScrubbing)
            {
                // FORCE read head to target position (scrubbing)
                // Map normalized position (0-1) to sample range (startNorm-endNorm)
                // Position slider 0.0 = rangeStart, Position slider 1.0 = rangeEnd
                double rangeStartSamples = startNorm * totalSamples;
                double rangeEndSamples = endNorm * totalSamples;
                double rangeLength = rangeEndSamples - rangeStartSamples;

                // CRITICAL: Position parameter is absolute (0-1 across full sample)
                // Map absolute position (0-1 across full sample) directly to sample position
                double newSamplePos = targetPosNorm * totalSamples;

                // CRITICAL: Playhead must ALWAYS be clamped to range boundaries [rangeStart,
                // rangeEnd] Range defines the valid playback zone - playhead can never be outside
                // it This ensures that when range changes, if playhead is outside new range, it
                // gets clamped Note: rangeStartSamples and rangeEndSamples already declared above
                newSamplePos = juce::jlimit(rangeStartSamples, rangeEndSamples, newSamplePos);

                // CRITICAL: Preserve playback state when scrubbing
                // If playing, continue playing from new position. If stopped, remain stopped.
                bool wasPlaying = sampleProcessor->isPlaying;

                // Move the playhead to the new position (this resets time stretcher buffers)
                sampleProcessor->setCurrentPosition(newSamplePos);

                // Ensure playback state is preserved (setCurrentPosition doesn't affect isPlaying,
                // but be explicit) If it was playing, it should continue playing from the new
                // position
                if (wasPlaying)
                {
                    // Ensure playback continues from the new position
                    sampleProcessor->isPlaying = true;

                    // CRITICAL: If synced to transport, temporarily disable sync position updates
                    // This allows playback to continue from the scrubbed position for a short time
                    // (~250ms = ~11 blocks at 44.1kHz, ~12 blocks at 48kHz)
                    if (syncToTransport.load())
                    {
                        manualScrubPending.store(true);
                        manualScrubBlocksRemaining.store(12); // Skip sync for ~12 blocks (~250ms)
                    }
                }
                // If it was stopped, leave it stopped (isPlaying remains false)

                // Sync tracking vars to this new forced position
                lastReadPosition = newSamplePos;
                readPosition = newSamplePos; // Keep readPosition in sync for timeline reporting
                lastUiPosition = targetPosNorm;

                // If we're the timeline master and was playing, update transport immediately
                // This ensures transport syncs immediately, not waiting for TempoClock to read
                if (wasPlaying)
                {
                    auto* parentSynth = getParent();
                    if (parentSynth && parentSynth->isModuleTimelineMaster(getLogicalId()))
                    {
                        double newPosSeconds = newSamplePos / sampleSampleRate;
                        parentSynth->setTransportPositionSeconds(newPosSeconds);
                    }
                }

                // Update the parameter so the UI slider snaps to the CV if CV driven
                // (We don't notify host here to avoid automation loops)
                positionParam->store(targetPosNorm);
            }
            else
            {
                // NORMAL PLAYBACK
                // We must UPDATE the position parameter to match the audio engine.
                // This prevents the "Manual Slider Movement" check from triggering falsely in the
                // next block.

                double currentSamplePos = sampleProcessor->getCurrentPosition();

                // CRITICAL: Position is stored as absolute (0-1 across full sample)
                // But playhead must ALWAYS be clamped to range boundaries [rangeStart, rangeEnd]
                // This ensures that when range changes, if playhead is outside new range, it gets
                // clamped
                double rangeStartSamples = startNorm * totalSamples;
                double rangeEndSamples = endNorm * totalSamples;

                // Clamp playhead position to range boundaries if it's outside
                if (currentSamplePos < rangeStartSamples || currentSamplePos > rangeEndSamples)
                {
                    currentSamplePos =
                        juce::jlimit(rangeStartSamples, rangeEndSamples, currentSamplePos);
                    sampleProcessor->setCurrentPosition(currentSamplePos);
                }

                // Convert clamped sample position to absolute position (0-1 across full sample)
                targetPosNorm = (float)(currentSamplePos / totalSamples);

                // CRITICAL: When synced to transport, setTimingInfo() handles position updates
                // Only update position parameter if NOT synced to avoid conflicts
                if (!syncToTransport.load())
                {
                    // 1. Update the atomic parameter so the UI draws the correct playhead
                    // Store as absolute position (0-1 across full sample)
                    positionParam->store(targetPosNorm);

                    // 2. Update our tracker so we know this value came from US, not the user
                    lastUiPosition = targetPosNorm;
                }
                // If synced, setTimingInfo() will update positionParam and lastUiPosition

                // --- GLOBAL RESET LOGIC ---
                if (sampleProcessor->isPlaying && currentSamplePos < lastReadPosition)
                {
                    // If jumped back significantly (loop wrap), trigger global reset
                    if (lastReadPosition > totalSamples * 0.5)
                    {
                        auto* parentSynth = getParent();
                        // Only trigger if this module is the timeline master (driving transport)
                        if (parentSynth && parentSynth->isModuleTimelineMaster(getLogicalId()))
                        {
                            parentSynth->triggerGlobalReset();
                        }
                    }
                }
                lastReadPosition = currentSamplePos;
                readPosition = currentSamplePos; // Keep readPosition in sync for any other uses
            }
        }
    }

    // Telemetry for UI (Standard practice)
    setLiveParamValue("position_live", targetPosNorm);

    // === TIMELINE REPORTING (at END of block, after position updates) ===
    // Update atomic state for timeline sync feature
    // CRITICAL: Use actual playhead position, not stale readPosition variable
    if (currentSample && sampleProcessor && sampleSampleRate > 0)
    {
        // Get actual playhead position from the audio engine (reflects scrubbing immediately)
        double currentSamplePos = sampleProcessor->getCurrentPosition();
        double currentPosSeconds = currentSamplePos / sampleSampleRate;
        double durationSeconds = sampleDurationSeconds;

        // Edge case: Validate duration > 0 before reporting
        if (durationSeconds <= 0.0)
        {
            reportActive.store(false, std::memory_order_relaxed);
        }
        else
        {
            // Edge case: Clamp position to valid range
            currentPosSeconds = juce::jlimit(0.0, durationSeconds, currentPosSeconds);

            // Check if playing and within duration
            bool isActive = sampleProcessor->isPlaying &&
                            (currentSamplePos < durationSeconds * sampleSampleRate);

            // Store atomically (using relaxed memory order - sufficient for this use case)
            reportPosition.store(currentPosSeconds, std::memory_order_relaxed);
            reportDuration.store(durationSeconds, std::memory_order_relaxed);
            reportActive.store(isActive, std::memory_order_relaxed);
        }
    }
    else
    {
        // No sample loaded or invalid state
        reportActive.store(false, std::memory_order_relaxed);
    }

    // Read relative modulation mode flags
    // Note: relativeRangeStartMode and relativeRangeEndMode are already declared earlier for range
    // calculation
    const bool relativeSpeedMode =
        relativeSpeedModParam != nullptr && relativeSpeedModParam->load() > 0.5f;
    const bool relativePitchMode =
        relativePitchModParam != nullptr && relativePitchModParam->load() > 0.5f;
    const bool relativeGateMode =
        relativeGateModParam != nullptr && relativeGateModParam->load() > 0.5f;

    // --- Compute block-rate CV-mapped values for telemetry (even when not playing) ---
    const float baseSpeed = apvts.getRawParameterValue("speed")->load();
    float       speedNow = baseSpeed;
    if (isParamInputConnected("speed_mod") && playbackBus.getNumChannels() > 1)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, playbackBus.getReadPointer(1)[0]);
        if (relativeSpeedMode)
        {
            // RELATIVE: ±4 octaves around base (CV at 0.5 = no change)
            const float octaveRange = 4.0f;
            const float octaveOffset = (cv - 0.5f) * octaveRange;
            speedNow = juce::jlimit(0.25f, 4.0f, baseSpeed * std::pow(2.0f, octaveOffset));
        }
        else
        {
            // ABSOLUTE: CV directly maps to 0.25-4.0 range
            speedNow = juce::jmap(cv, 0.25f, 4.0f);
        }
    }

    // VARISPEED LOGIC when synced to transport.
    // TimePitchProcessor expects a unified SPEED multiplier for all engines:
    // 2.0 = double speed (fast), 0.5 = half speed (slow).
    if (syncToTransport.load() && currentSample && sampleSampleRate > 0)
    {
        double bpm = currentBpm.load();
        if (bpm < 1.0)
            bpm = 120.0;

        // 1. Speed factor relative to 120 BPM
        // High BPM (240) -> 240/120 = 2.0 (fast)
        // Low  BPM (60)  ->  60/120 = 0.5 (slow)
        const float speedFactor = (float)(bpm / 120.0);

        // 2. Apply Speed knob as an additional multiplier
        speedNow = speedFactor * baseSpeed;
    }

    const float basePitch = apvts.getRawParameterValue("pitch")->load();
    float       pitchNow = basePitch;
    if (isParamInputConnected("pitch_mod") && playbackBus.getNumChannels() > 0)
    {
        const float rawCV = playbackBus.getReadPointer(0)[0];
        const float cv = juce::jlimit(0.0f, 1.0f, rawCV);
        if (relativePitchMode)
        {
            // RELATIVE: ±24 semitones around base (CV treated as bipolar: 0.5 = no change)
            const float bipolarCV = cv * 2.0f - 1.0f; // 0-1 -> -1 to 1
            const float pitchModulationRange = 24.0f;
            pitchNow = juce::jlimit(-24.0f, 24.0f, basePitch + (bipolarCV * pitchModulationRange));
        }
        else
        {
            // ABSOLUTE: CV directly maps to -24 to +24 semitones (CV 0.0 = -24, CV 1.0 = +24)
            pitchNow = juce::jmap(cv, -24.0f, 24.0f);
        }
    }

    // Range values already calculated earlier for position scrubbing - reuse them here
    // (startNorm and endNorm are already set above)

    // Update live telemetry regardless of play state (matches TTS pattern)
    setLiveParamValue("speed_live", speedNow);
    setLiveParamValue("pitch_live", pitchNow);
    setLiveParamValue("rangeStart_live", startNorm);
    setLiveParamValue("rangeEnd_live", endNorm);
    // Gate live (use first sample if CV present, otherwise knob)
    if (isParamInputConnected("gate_mod") && controlBus.getNumChannels() > 0)
    {
        const float g = juce::jlimit(0.0f, 1.0f, controlBus.getReadPointer(0)[0]);
        setLiveParamValue("gate_live", g);
    }
    else
    {
        setLiveParamValue("gate_live", apvts.getRawParameterValue("gate")->load());
    }

    // --- 1. TRIGGER DETECTION ---
    const bool looping = apvts.getRawParameterValue("loop")->load() > 0.5f;
    // (Auto-start logic removed to prevent fighting with setTimingInfo)

    // Check for a rising edge on the trigger input to start playback.
    if (isParamInputConnected("trigger_mod") && controlBus.getNumChannels() > 1)
    {
        const float* trigSignal = controlBus.getReadPointer(1); // Control Bus Channel 1 = Trigger
        for (int i = 0; i < numSamples; ++i)
        {
            const bool trigHigh = trigSignal[i] > 0.5f;
            if (trigHigh && !lastTriggerHigh)
            {
                reset(); // This now sets the internal voice's isPlaying to true
                break;
            }
            lastTriggerHigh = trigHigh;
        }
        if (numSamples > 0)
            lastTriggerHigh = (controlBus.getReadPointer(1)[numSamples - 1] > 0.5f);
    }

    // --- Randomize Trigger ---
    if (isParamInputConnected("randomize_mod") && randomizeBus.getNumChannels() > 0)
    {
        const float* randTrigSignal = randomizeBus.getReadPointer(0); // Randomize Bus Channel 0
        for (int i = 0; i < numSamples; ++i)
        {
            const bool trigHigh = randTrigSignal[i] > 0.5f;
            if (trigHigh && !lastRandomizeTriggerHigh)
            {
                queueRandomSampleFromCurrentFolder("CV trigger");
                break; // Only randomize once per block
            }
            lastRandomizeTriggerHigh = trigHigh;
        }
        if (numSamples > 0)
            lastRandomizeTriggerHigh = (randomizeBus.getReadPointer(0)[numSamples - 1] > 0.5f);
    }

    // --- 2. CONDITIONAL AUDIO RENDERING ---
    // Only generate audio if the internal voice is in a playing state.
    if (currentProcessor->isPlaying)
    {
        // DEBUG: Log CV values from buses (like TTS)
        static int debugFrameCounter = 0;
        if (debugFrameCounter == 0 || debugFrameCounter % 240 == 0)
        {
            juce::String dbgMsg = "[Sample CV Debug #" + juce::String(debugFrameCounter) + "] ";
            if (playbackBus.getNumChannels() > 0)
                dbgMsg += "pitch_cv=" + juce::String(playbackBus.getReadPointer(0)[0], 3) + " ";
            if (playbackBus.getNumChannels() > 1)
                dbgMsg += "speed_cv=" + juce::String(playbackBus.getReadPointer(1)[0], 3) + " ";
            if (controlBus.getNumChannels() > 0)
                dbgMsg += "gate_cv=" + juce::String(controlBus.getReadPointer(0)[0], 3) + " ";
            juce::Logger::writeToLog(dbgMsg);
        }
        debugFrameCounter++;

        currentProcessor->setZoneTimeStretchRatio(speedNow);
        currentProcessor->setBasePitchSemitones(pitchNow);
        const int sourceLength = currentSample->stereo.getNumSamples();
        currentProcessor->setPlaybackRange(startNorm * sourceLength, endNorm * sourceLength);

        // Update APVTS parameters for UI feedback (especially spectrogram handles)
        *rangeStartParam = startNorm;
        apvts.getParameter("rangeStart")->sendValueChangedMessageToListeners(startNorm);
        *rangeEndParam = endNorm;
        apvts.getParameter("rangeEnd")->sendValueChangedMessageToListeners(endNorm);

        const int engineIdx = (int)apvts.getRawParameterValue("engine")->load();
        currentProcessor->setEngine(
            engineIdx == 0 ? SampleVoiceProcessor::Engine::RubberBand
                           : SampleVoiceProcessor::Engine::Naive);
        currentProcessor->setRubberBandOptions(
            apvts.getRawParameterValue("rbWindowShort")->load() > 0.5f,
            apvts.getRawParameterValue("rbPhaseInd")->load() > 0.5f);
        currentProcessor->setLooping(apvts.getRawParameterValue("loop")->load() > 0.5f);

        // Generate the sample's audio into the OUTPUT buffer. This might set isPlaying to false if
        // the sample ends.
        try
        {
            // Create a temporary buffer view for just the output bus
            juce::AudioBuffer<float> outputBuffer(
                outBus.getArrayOfWritePointers(), outBus.getNumChannels(), outBus.getNumSamples());
            currentProcessor->renderBlock(outputBuffer, midiMessages);
        }
        catch (...)
        {
            RtLogger::postf("[SampleLoader][FATAL] renderBlock exception");
            outBus.clear();
        }

        // --- 3. GATE (VCA) APPLICATION ---
        // If a gate is connected, use it to shape the volume of the audio we just generated.
        const float baseGate = apvts.getRawParameterValue("gate")->load();
        float       lastGateValue = 1.0f;
        if (isParamInputConnected("gate_mod") && controlBus.getNumChannels() > 0)
        {
            const float* gateCV = controlBus.getReadPointer(0); // Control Bus Channel 0 = Gate
            for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
            {
                float* channelData = outBus.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    const float cv = juce::jlimit(0.0f, 1.0f, gateCV[i]);
                    float       gateValue;
                    if (relativeGateMode)
                    {
                        // RELATIVE: ±0.5 around base gate (CV at 0.5 = no change, clamped to 0-1)
                        const float gateRange = 0.5f;
                        const float gateOffset = (cv - 0.5f) * (gateRange * 2.0f);
                        gateValue = juce::jlimit(0.0f, 1.0f, baseGate + gateOffset);
                    }
                    else
                    {
                        // ABSOLUTE: CV directly sets gate (0-1)
                        gateValue = cv;
                    }
                    channelData[i] *= gateValue;

                    // Update telemetry (throttled every 64 samples, only once per channel)
                    if (ch == 0 && (i & 0x3F) == 0)
                    {
                        setLiveParamValue("gate_live", gateValue);
                        lastGateValue = gateValue;
                    }
                }
            }
        }
        else
        {
            // No gate modulation - use static gate knob value
            lastGateValue = apvts.getRawParameterValue("gate")->load();
            setLiveParamValue("gate_live", lastGateValue);
        }

        // Apply main gate knob last
        outBus.applyGain(apvts.getRawParameterValue("gate")->load());
    }
    else
    {
        // Not playing: explicitly clear output now (safe after input analysis)
        outBus.clear();
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

    if (shouldLoadAfterBlock && deferredRandomPath.isNotEmpty())
    {
        juce::Logger::writeToLog(
            "[Sample Loader] Loading queued random sample: " + deferredRandomPath);
        loadSample(juce::File(deferredRandomPath));

        if (shouldResumeAfterRandomLoad.exchange(false))
        {
            const juce::ScopedLock lock(processorSwapLock);
            if (sampleProcessor != nullptr)
            {
                sampleProcessor->reset();
                sampleProcessor->isPlaying = true;
                juce::Logger::writeToLog("[Sample Loader] Auto-resumed playback after random swap");
            }
        }
    }

    processorIsRendering.store(false, std::memory_order_relaxed);
}

void SampleLoaderModuleProcessor::reset()
{
    // Clear any pending random queue on reset
    {
        const juce::ScopedLock lock(pendingRandomLock);
        hasPendingRandomSample.store(false);
        pendingRandomSamplePath.clear();
    }

    if (sampleProcessor != nullptr)
    {
        sampleProcessor->reset();
    }

    if (currentSample != nullptr && rangeStartParam != nullptr)
    {
        readPosition = rangeStartParam->load() * currentSample->stereo.getNumSamples();
    }
    else
    {
        readPosition = 0.0;
    }
}

void SampleLoaderModuleProcessor::forceStop()
{
    // Clear any pending random queue on force stop
    {
        const juce::ScopedLock lock(pendingRandomLock);
        hasPendingRandomSample.store(false);
        pendingRandomSamplePath.clear();
    }

    // Force stop the module's playback state
    isPlaying.store(false);

    // Also stop the internal voice if it exists
    if (sampleProcessor != nullptr)
    {
        sampleProcessor->isPlaying = false;
    }
}

void SampleLoaderModuleProcessor::setTimingInfo(const TransportState& state)
{
    auto* parentSynth = getParent();
    if (parentSynth && parentSynth->isModuleTimelineMaster(getLogicalId()))
    {
        return;
    }

    ModuleProcessor::setTimingInfo(state);

    // Detect edges for Stop/Pause command handling
    bool wasPlaying = lastTransportPlaying.exchange(state.isPlaying);
    bool risingEdge = state.isPlaying && !wasPlaying;  // Play pressed (resume)
    bool fallingEdge = !state.isPlaying && wasPlaying; // Pause or Stop pressed

    SampleVoiceProcessor* safeProcessor = nullptr;
    {
        const juce::ScopedLock lock(processorSwapLock);
        safeProcessor = sampleProcessor.get();
    }

    if (!safeProcessor)
        return;

    // Always update BPM for the audio engine
    currentBpm.store(state.bpm);

    // === SYNCED MODE ===
    // The sample follows the global timeline cursor.
    if (syncToTransport.load())
    {
        // Play/Pause is handled automatically because the timeline itself runs or freezes.
        safeProcessor->isPlaying = state.isPlaying;

        if (state.isPlaying && currentSample && sampleSampleRate > 0)
        {
            const double totalSamples = (double)currentSample->stereo.getNumSamples();
            double       targetSamplePos = 0.0;

            // Varispeed calculations for UI positioning
            double bpm = state.bpm;
            if (bpm < 1.0)
                bpm = 120.0;
            float knobVal = apvts.getRawParameterValue("speed")->load();

            // Note: This logic matches the UI/Playhead visualization
            double varispeedSpeed = (bpm / 120.0) * knobVal;

            // 1. Calculate Target Position
            const bool isAbsoluteMode = syncModeParam && (syncModeParam->getIndex() == 1);

            // Calculate loop points
            float sNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
            float eNorm = rangeEndParam ? rangeEndParam->load() : 1.0f;
            if (sNorm >= eNorm)
            {
                float m = (sNorm + eNorm) * 0.5f;
                sNorm = m - 0.001f;
                eNorm = m + 0.001f;
            }
            double sSamp = sNorm * totalSamples;
            double lSamp = (eNorm * totalSamples) - sSamp;

            if (isAbsoluteMode)
            {
                targetSamplePos =
                    state.songPositionSeconds * varispeedSpeed * (double)sampleSampleRate;
            }
            else
            {
                // Relative: Beats -> Loop Progress
                double loopDurationSec = (lSamp / sampleSampleRate) / varispeedSpeed;
                double progress = 0.0;
                if (loopDurationSec > 0.0)
                {
                    bool loop = apvts.getRawParameterValue("loop")
                                    ? (apvts.getRawParameterValue("loop")->load() > 0.5f)
                                    : true;
                    if (loop)
                        progress =
                            std::fmod(state.songPositionSeconds, loopDurationSec) / loopDurationSec;
                    else
                        progress =
                            juce::jlimit(0.0, 1.0, state.songPositionSeconds / loopDurationSec);
                }
                targetSamplePos = sSamp + (progress * lSamp);
            }

            targetSamplePos = juce::jlimit(0.0, totalSamples, targetSamplePos);

            // 2. Circular Sync Check (Anti-Poc)
            double currentEnginePos = safeProcessor->getCurrentPosition();
            double linearDiff = std::abs(targetSamplePos - currentEnginePos);
            double circularDiff = linearDiff;

            if (lSamp > 0)
            {
                double relTarget = targetSamplePos - sSamp;
                double relEngine = currentEnginePos - sSamp;
                if (relTarget < 0)
                    relTarget += lSamp;
                if (relEngine < 0)
                    relEngine += lSamp;
                double diffInLoop = std::abs(relTarget - relEngine);
                double wrapDiff = lSamp - diffInLoop;
                circularDiff = std::min(diffInLoop, wrapDiff);
            }

            // 3. Detect Jump (Discontinuity)
            double currentPos =
                isAbsoluteMode ? state.songPositionSeconds : state.songPositionBeats;
            double prevPos = lastSyncedTransportPosition;
            double delta = std::abs(currentPos - prevPos);
            bool   transportJumped = (delta > 0.1) || (prevPos < 0.0);

            // 4. ACTION: Seek only if Transport Jumped AND Audio is Out of Sync
            // Threshold ~100ms (4800 samples)
            if (transportJumped && circularDiff > 4800.0)
            {
                safeProcessor->setCurrentPosition(targetSamplePos);
                readPosition = targetSamplePos;
                lastReadPosition = targetSamplePos;
            }

            // 5. Update UI
            if (positionParam)
            {
                float normPos = (float)(targetSamplePos / totalSamples);
                positionParam->store(normPos);
                lastUiPosition = normPos;
            }

            lastSyncedTransportPosition = currentPos;
        }
        else
        {
            lastSyncedTransportPosition = -1.0;
        }
    }
    // === UNSYNCED MODE ===
    else
    {
        // 3. Handle RISING EDGE FIRST (Resume from Pause) - CRITICAL: Restore saved position BEFORE
        // setting isPlaying When resuming from pause, restore the position we saved when pausing
        // This MUST happen before we set isPlaying to true to prevent any race conditions
        if (risingEdge)
        {
            // Rising edge: Resume playback from saved position
            // CRITICAL: Restore position if we have a saved paused position (even if it's 0, it
            // might be valid)
            if (currentSample && pausedPosition >= 0.0)
            {
                const double totalSamples = (double)currentSample->stereo.getNumSamples();
                // Clamp saved position to valid range before restoring
                // Use totalSamples (not totalSamples - 1) to allow position at the very end
                double posToRestore = juce::jlimit(0.0, totalSamples, pausedPosition);

                // CRITICAL: Restore the position from when we paused - do this BEFORE setting
                // isPlaying This ensures the position is set before any audio processing starts
                safeProcessor->setCurrentPosition(posToRestore);

                // Also update our internal readPosition to match
                readPosition = posToRestore;
                lastReadPosition = posToRestore;

                // Update UI to reflect restored position
                if (positionParam)
                {
                    float normPos = (float)(posToRestore / totalSamples);
                    positionParam->store(normPos);
                    lastUiPosition = normPos;
                }

                // Clear saved position AFTER restoring (we've restored it)
                pausedPosition = -1.0;
            }
            // If no saved position (pausedPosition < 0), just resume from wherever the voice
            // processor currently is (This handles the case where pause didn't save position for
            // some reason)

            // Update tracked command
            lastTransportCommand = state.lastCommand.load();
        }

        // 1. Always mirror Transport Play State (This fixes "Stuck at Zero")
        // If transport is playing, we play. If transport is paused, we pause.
        // Do this AFTER restoring position on rising edge
        safeProcessor->isPlaying = state.isPlaying;

        // 2. Handle STOP Command (Rewind) - Only on explicit Stop transition, NEVER on Pause
        // CRITICAL: If command is Pause, NEVER reset - just freeze position
        // Only reset on explicit transition TO Stop command
        if (fallingEdge)
        {
            const TransportCommand currentCommand = state.lastCommand.load();

            // FIRST CHECK: If it's Pause, SAVE current position and freeze
            if (currentCommand == TransportCommand::Pause)
            {
                // Pause: Save current position for resume, do not reset
                // CRITICAL: Get position from voice processor directly
                double currentPos = safeProcessor->getCurrentPosition();
                // Always save the position, even if it's 0 (it might be valid at the start of
                // sample) We'll validate it when we restore
                pausedPosition = currentPos;
                lastTransportCommand = currentCommand;
            }
            // SECOND CHECK: Only reset if command TRANSITIONS to Stop (and not Pause)
            else
            {
                const TransportCommand previousCommand = lastTransportCommand;
                // Only reset if command TRANSITIONS to Stop
                if (currentCommand == TransportCommand::Stop &&
                    previousCommand != TransportCommand::Stop)
                {
                    // Explicit transition to Stop: Rewind to range start
                    if (currentSample)
                    {
                        const double totalSamples = (double)currentSample->stereo.getNumSamples();
                        float        sNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
                        double       sSamp = sNorm * totalSamples;

                        safeProcessor->setCurrentPosition(sSamp);

                        // Clear any saved paused position (we're stopping, not pausing)
                        pausedPosition = -1.0;

                        // Update UI to show reset
                        if (positionParam)
                        {
                            positionParam->store(sNorm);
                            lastUiPosition = sNorm;
                        }
                    }
                }
                // For any other case (Play, unknown, or Stop that persists): DO NOTHING - just
                // freeze

                // Update tracked command for next comparison
                lastTransportCommand = currentCommand;
            }
        }
        // (Rising edge handling moved to top of function, before setting isPlaying)
        else if (!risingEdge)
        {
            // Not a falling edge and not a rising edge - just update tracked command
            lastTransportCommand = state.lastCommand.load();
        }
        {
            // Update tracked command even when not on any edge (for next time)
            lastTransportCommand = state.lastCommand.load();
        }
    }
}
void SampleLoaderModuleProcessor::loadSample(const juce::File& file)
{
    // Clear any pending random queue when manually loading a sample
    {
        const juce::ScopedLock lock(pendingRandomLock);
        hasPendingRandomSample.store(false);
        pendingRandomSamplePath.clear();
    }

    // CRASH PROTECTION: Validate file path safely
    juce::String filePath;
    try
    {
        filePath = file.getFullPathName();
        // CRASH PROTECTION: Validate path string is not empty
        if (filePath.isEmpty())
        {
            juce::Logger::writeToLog("[Sample Loader] Empty file path");
            return;
        }
    }
    catch (...)
    {
        juce::Logger::writeToLog("[Sample Loader][FATAL] Exception getting file path");
        return;
    }

    // CRASH PROTECTION: Validate file exists before processing
    try
    {
        if (!file.existsAsFile())
        {
            DBG("[Sample Loader] File does not exist: " + filePath);
            return;
        }
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception checking file existence: " + filePath);
        return;
    }

    // 1) Load the original shared sample from the bank
    SampleBank                          sampleBank;
    std::shared_ptr<SampleBank::Sample> original;
    try
    {
        juce::Logger::writeToLog(
            "[Sample Loader] Attempting to load: " + filePath + " (" +
            juce::String(file.getSize() / 1024) + " KB)");
        original = sampleBank.getOrLoad(file);
    }
    catch (const std::bad_alloc& e)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Memory allocation failed in SampleBank::getOrLoad: " +
            juce::String(e.what()) + " (file: " + filePath + ")");
        return;
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception in SampleBank::getOrLoad: " + juce::String(e.what()) +
            " (file: " + filePath + ")");
        return;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Unknown exception in SampleBank::getOrLoad (file: " + filePath +
            ")");
        return;
    }
    if (original == nullptr || original->stereo.getNumSamples() <= 0)
    {
        juce::Logger::writeToLog(
            "[Sample Loader] Failed to load sample or empty: " + filePath +
            (original == nullptr
                 ? " (nullptr)"
                 : " (samples: " + juce::String(original->stereo.getNumSamples()) + ")"));
        return;
    }

    // CRASH PROTECTION: Log sample info for debugging
    const int    numChannels = original->stereo.getNumChannels();
    const int    numSamples = original->stereo.getNumSamples();
    const double sampleRate = original->sampleRate;
    const double duration = numSamples / sampleRate;
    juce::Logger::writeToLog(
        "[Sample Loader] Loaded sample: " + file.getFileName() +
        " (channels: " + juce::String(numChannels) + ", samples: " + juce::String(numSamples) +
        ", rate: " + juce::String(sampleRate, 1) + " Hz" +
        ", duration: " + juce::String(duration, 2) + " s)");

    currentSampleName = file.getFileName();
    currentSamplePath = file.getFullPathName();
    apvts.state.setProperty("samplePath", currentSamplePath, nullptr);

    // --- THIS IS THE FIX ---
    // Store the sample's metadata in our new member variables.
    sampleDurationSeconds = (double)original->stereo.getNumSamples() / original->sampleRate;
    sampleSampleRate = (int)original->sampleRate;
    // --- END OF FIX ---

    // 2) Create a private STEREO copy (preserve stereo or duplicate mono)
    // CRASH PROTECTION: Validate sample size before copying
    // Note: numSamples already defined above at line 943
    const juce::int64 totalSizeBytes =
        static_cast<juce::int64>(numSamples) * 2 * sizeof(float); // Stereo = 2 channels
    const juce::int64 maxSizeBytes = 1LL * 1024 * 1024 * 1024;    // 1 GB limit

    if (totalSizeBytes > maxSizeBytes)
    {
        juce::Logger::writeToLog(
            "[Sample Loader] Sample too large to copy (" +
            juce::String(totalSizeBytes / 1024 / 1024) + " MB): " + filePath);
        return;
    }

    auto privateCopy = std::make_shared<SampleBank::Sample>();
    privateCopy->sampleRate = original->sampleRate;

    try
    {
        privateCopy->stereo.setSize(2, numSamples); // Always stereo output
        if (privateCopy->stereo.getNumSamples() != numSamples ||
            privateCopy->stereo.getNumChannels() != 2)
        {
            juce::Logger::writeToLog(
                "[Sample Loader] Failed to allocate stereo buffer (" + juce::String(numSamples) +
                " samples)");
            return;
        }
    }
    catch (const std::bad_alloc& e)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Memory allocation failed for stereo copy: " +
            juce::String(e.what()) + " (samples: " + juce::String(numSamples) + ")");
        return;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception allocating stereo buffer (samples: " +
            juce::String(numSamples) + ")");
        return;
    }

    try
    {
        if (original->stereo.getNumChannels() <= 1)
        {
            // Mono source: duplicate to both L and R channels
            privateCopy->stereo.copyFrom(0, 0, original->stereo, 0, 0, numSamples); // L = Mono
            privateCopy->stereo.copyFrom(1, 0, original->stereo, 0, 0, numSamples); // R = Mono
            juce::Logger::writeToLog(
                "[Sample Loader] Loaded mono sample and duplicated to stereo: " +
                file.getFileName());
        }
        else
        {
            // Stereo (or multi-channel) source: copy L and R channels
            privateCopy->stereo.copyFrom(0, 0, original->stereo, 0, 0, numSamples); // L channel
            privateCopy->stereo.copyFrom(1, 0, original->stereo, 1, 0, numSamples); // R channel
            juce::Logger::writeToLog("[Sample Loader] Loaded stereo sample: " + file.getFileName());
        }
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception copying audio data (samples: " +
            juce::String(numSamples) +
            ", channels: " + juce::String(original->stereo.getNumChannels()) + ")");
        return;
    }

    // 3) Atomically assign our private copy for this module
    currentSample = privateCopy;

    // CRASH PROTECTION: Generate spectrogram with exception handling
    try
    {
        generateSpectrogram();
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception in generateSpectrogram: " + juce::String(e.what()));
        // Continue without spectrogram - sample is still loaded
    }
    catch (...)
    {
        juce::Logger::writeToLog("[Sample Loader][FATAL] Unknown exception in generateSpectrogram");
        // Continue without spectrogram - sample is still loaded
    }

    // 4) If the module is prepared, stage a new processor
    if (getSampleRate() > 0.0 && getBlockSize() > 0)
    {
        createSampleProcessor();
    }
    else
    {
        DBG("[Sample Loader][Defer] Module not prepared yet; will create processor in "
            "prepareToPlay");
    }
}
void SampleLoaderModuleProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree vt("SampleLoader");
    vt.setProperty("samplePath", currentSamplePath, nullptr);
    vt.setProperty("speed", apvts.getRawParameterValue("speed")->load(), nullptr);
    vt.setProperty("pitch", apvts.getRawParameterValue("pitch")->load(), nullptr);
    vt.setProperty("gate", apvts.getRawParameterValue("gate")->load(), nullptr);
    vt.setProperty("engine", (int)apvts.getRawParameterValue("engine")->load(), nullptr);
    vt.setProperty(
        "rbWindowShort", apvts.getRawParameterValue("rbWindowShort")->load() > 0.5f, nullptr);
    vt.setProperty("rbPhaseInd", apvts.getRawParameterValue("rbPhaseInd")->load() > 0.5f, nullptr);
    vt.setProperty("loop", apvts.getRawParameterValue("loop")->load() > 0.5f, nullptr);
    if (auto xml = vt.createXml())
        copyXmlToBinary(*xml, destData);
}

void SampleLoaderModuleProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (!xml)
        return;
    juce::ValueTree vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid())
        return;
    currentSamplePath = vt.getProperty("samplePath").toString();
    if (currentSamplePath.isNotEmpty())
        loadSample(juce::File(currentSamplePath));
    if (auto* p = apvts.getParameter("speed"))
        p->setValueNotifyingHost(
            apvts.getParameterRange("speed").convertTo0to1((float)vt.getProperty("speed", 1.0f)));
    if (auto* p = apvts.getParameter("pitch"))
        p->setValueNotifyingHost(
            apvts.getParameterRange("pitch").convertTo0to1((float)vt.getProperty("pitch", 0.0f)));
    if (auto* p = apvts.getParameter("gate"))
        p->setValueNotifyingHost(
            apvts.getParameterRange("gate").convertTo0to1((float)vt.getProperty("gate", 0.8f)));
    if (auto* p = apvts.getParameter("engine"))
        p->setValueNotifyingHost((float)(int)vt.getProperty("engine", 0));
    if (auto* p = apvts.getParameter("rbWindowShort"))
        p->setValueNotifyingHost((bool)vt.getProperty("rbWindowShort", true) ? 1.0f : 0.0f);
    if (auto* p = apvts.getParameter("rbPhaseInd"))
        p->setValueNotifyingHost((bool)vt.getProperty("rbPhaseInd", true) ? 1.0f : 0.0f);
    if (auto* p = apvts.getParameter("loop"))
        p->setValueNotifyingHost((bool)vt.getProperty("loop", false) ? 1.0f : 0.0f);
}

void SampleLoaderModuleProcessor::loadSample(const juce::String& filePath)
{
    loadSample(juce::File(filePath));
}

juce::String SampleLoaderModuleProcessor::getCurrentSampleName() const { return currentSampleName; }

bool SampleLoaderModuleProcessor::hasSampleLoaded() const { return currentSample != nullptr; }

// Legacy SoundTouch setters removed

void SampleLoaderModuleProcessor::setDebugOutput(bool enabled) { debugOutput = enabled; }

void SampleLoaderModuleProcessor::logCurrentSettings() const
{
    if (debugOutput)
    {
        DBG("[Sample Loader] Current Settings:");
        DBG("  Sample: " + currentSampleName);
        DBG("  Speed: " + juce::String(apvts.getRawParameterValue("speed")->load()));
        DBG("  Pitch: " + juce::String(apvts.getRawParameterValue("pitch")->load()));
    }
}

void SampleLoaderModuleProcessor::updateSoundTouchSettings() {}

void SampleLoaderModuleProcessor::randomizeSample() { queueRandomSampleFromCurrentFolder("UI"); }

bool SampleLoaderModuleProcessor::queueRandomSampleFromCurrentFolder(const juce::String& sourceTag)
{
    if (currentSamplePath.isEmpty())
        return false;

    juce::File currentFile(currentSamplePath);
    juce::File parentDir = currentFile.getParentDirectory();

    if (!parentDir.exists() || !parentDir.isDirectory())
        return false;

    juce::Array<juce::File> audioFiles;
    parentDir.findChildFiles(
        audioFiles, juce::File::findFiles, true, "*.wav;*.mp3;*.flac;*.aiff;*.ogg");

    if (audioFiles.size() <= 1)
        return false;

    for (int i = audioFiles.size() - 1; i >= 0; --i)
    {
        if (audioFiles[i].getFullPathName() == currentSamplePath)
        {
            audioFiles.remove(i);
            break;
        }
    }

    if (audioFiles.isEmpty())
        return false;

    juce::Random rng(juce::Time::getMillisecondCounterHiRes());
    juce::File   randomFile = audioFiles[rng.nextInt(audioFiles.size())];

    {
        const juce::ScopedLock lock(pendingRandomLock);
        pendingRandomSamplePath = randomFile.getFullPathName();
        hasPendingRandomSample.store(true);
    }

    bool wasActivelyUsed = false;
    {
        const juce::ScopedLock lock(processorSwapLock);
        if (sampleProcessor != nullptr &&
            (sampleProcessor->isPlaying || processorIsRendering.load(std::memory_order_relaxed)))
        {
            wasActivelyUsed = true;
            sampleProcessor->isPlaying = false;
            shouldResumeAfterRandomLoad.store(true);
        }
    }

    if (wasActivelyUsed)
    {
        juce::Logger::writeToLog(
            "[Sample Loader] " + sourceTag +
            " stopped playback for random swap: " + randomFile.getFileName());
    }
    else
    {
        juce::Logger::writeToLog(
            "[Sample Loader] " + sourceTag + " queued random sample: " + randomFile.getFileName());
    }

    return true;
}

void SampleLoaderModuleProcessor::createSampleProcessor()
{
    if (currentSample == nullptr)
    {
        return;
    }
    // Guard against double-creation and race with audio thread: build new then swap under lock
    auto newProcessor = std::make_unique<SampleVoiceProcessor>(currentSample);

    // Set up the sample processor
    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int    bs = getBlockSize() > 0 ? getBlockSize() : 512;
    newProcessor->prepareToPlay(sr, bs);

    // --- Set initial playback range ---
    const float  startNorm = rangeStartParam->load();
    const float  endNorm = rangeEndParam->load();
    const double startSample = startNorm * currentSample->stereo.getNumSamples();
    const double endSample = endNorm * currentSample->stereo.getNumSamples();
    newProcessor->setPlaybackRange(startSample, endSample);
    newProcessor->resetPosition();

    // CRITICAL FIX: Initialize position based on sync mode
    if (pausedPosition >= 0.0 && !syncToTransport.load())
    {
        // UNSYNCED: Use saved paused position, but CLAMP to new sample bounds
        const double maxValidPosition = (double)currentSample->stereo.getNumSamples() - 1.0;
        const double clampedPos = juce::jlimit(0.0, maxValidPosition, pausedPosition);
        newProcessor->setCurrentPosition(clampedPos);

        // Log if we had to clamp (indicates sample switch)
        if (pausedPosition > maxValidPosition)
        {
            juce::Logger::writeToLog(
                "[Sample Loader] Clamped position from " + juce::String(pausedPosition) + " to " +
                juce::String(clampedPos) + " (new sample length: " +
                juce::String(currentSample->stereo.getNumSamples()) + ")");
        }
    }
    else if (syncToTransport.load() && getParent())
    {
        // SYNCED: Initialize from current transport position
        TransportState transport = getParent()->getTransportState();
        const double   totalSamples = (double)currentSample->stereo.getNumSamples();
        double         targetSamplePos = 0.0;

        // Use the same calculation logic as setTimingInfo for consistency
        double bpm = transport.bpm;
        if (bpm < 1.0)
            bpm = 120.0;
        float  knobVal = apvts.getRawParameterValue("speed")->load();
        double varispeedSpeed = (bpm / 120.0) * knobVal;

        const bool isAbsoluteMode = syncModeParam && (syncModeParam->getIndex() == 1);
        float      sNorm = rangeStartParam ? rangeStartParam->load() : 0.0f;
        float      eNorm = rangeEndParam ? rangeEndParam->load() : 1.0f;
        if (sNorm >= eNorm)
        {
            float m = (sNorm + eNorm) * 0.5f;
            sNorm = m - 0.001f;
            eNorm = m + 0.001f;
        }
        double sSamp = sNorm * totalSamples;
        double lSamp = (eNorm * totalSamples) - sSamp;

        if (isAbsoluteMode)
        {
            targetSamplePos =
                transport.songPositionSeconds * varispeedSpeed * (double)sampleSampleRate.load();
        }
        else
        {
            // Relative: Beats -> Loop Progress
            double loopDurationSec = (lSamp / (double)sampleSampleRate.load()) / varispeedSpeed;
            double progress = 0.0;
            if (loopDurationSec > 0.0)
            {
                bool loop = apvts.getRawParameterValue("loop")
                                ? (apvts.getRawParameterValue("loop")->load() > 0.5f)
                                : true;
                if (loop)
                    progress =
                        std::fmod(transport.songPositionSeconds, loopDurationSec) / loopDurationSec;
                else
                    progress =
                        juce::jlimit(0.0, 1.0, transport.songPositionSeconds / loopDurationSec);
            }
            targetSamplePos = sSamp + (progress * lSamp);
        }

        const double maxValidPosition = totalSamples - 1.0;
        targetSamplePos = juce::jlimit(0.0, maxValidPosition, targetSamplePos);
        newProcessor->setCurrentPosition(targetSamplePos);
    }
    else
    {
        // Default: reset to range start
        newProcessor
            ->resetPosition(); // Reset position without starting playback - wait for trigger
    }

    // Set parameters from our APVTS
    newProcessor->setZoneTimeStretchRatio(apvts.getRawParameterValue("speed")->load());
    newProcessor->setBasePitchSemitones(apvts.getRawParameterValue("pitch")->load());
    newSampleProcessor.store(newProcessor.release());
    DBG("[Sample Loader] Staged new sample processor for: " << currentSampleName);

    DBG("[Sample Loader] Created sample processor for: " + currentSampleName);
}

// === TIMELINE REPORTING INTERFACE IMPLEMENTATION ===

bool SampleLoaderModuleProcessor::canProvideTimeline() const { return hasSampleLoaded(); }

double SampleLoaderModuleProcessor::getTimelinePositionSeconds() const
{
    return reportPosition.load(std::memory_order_relaxed);
}

double SampleLoaderModuleProcessor::getTimelineDurationSeconds() const
{
    return reportDuration.load(std::memory_order_relaxed);
}

bool SampleLoaderModuleProcessor::isTimelineActive() const
{
    return reportActive.load(std::memory_order_relaxed);
}

void SampleLoaderModuleProcessor::generateSpectrogram()
{
    const juce::ScopedLock lock(imageLock);
    spectrogramImage = juce::Image(); // Clear previous image

    // CRASH PROTECTION: Validate sample data before processing
    if (currentSample == nullptr)
        return;

    // Get a safe reference to the sample (may be swapped during processing)
    std::shared_ptr<SampleBank::Sample> safeSample = currentSample;
    if (safeSample == nullptr || safeSample->stereo.getNumSamples() == 0)
        return;

    const int fftOrder = 10;
    const int fftSize = 1 << fftOrder;
    const int hopSize = fftSize / 4;
    const int totalSamples = safeSample->stereo.getNumSamples();

    // CRASH PROTECTION: Check for integer overflow in numHops calculation
    if (totalSamples < fftSize)
        return;

    // CRASH PROTECTION: Safe numHops calculation with overflow protection
    // Use int64 to prevent overflow for very large files
    const juce::int64 totalSamples64 = static_cast<juce::int64>(totalSamples);
    const juce::int64 fftSize64 = static_cast<juce::int64>(fftSize);
    const juce::int64 hopSize64 = static_cast<juce::int64>(hopSize);
    const juce::int64 numHops64 = (totalSamples64 - fftSize64) / hopSize64;

    // CRASH PROTECTION: Limit spectrogram size to prevent memory issues (max 32k width)
    const int maxHops = 32768;
    const int numHops =
        static_cast<int>(juce::jlimit(1LL, static_cast<juce::int64>(maxHops), numHops64));

    if (numHops <= 0)
    {
        juce::Logger::writeToLog(
            "[Sample Loader] Spectrogram: Invalid numHops (" + juce::String(numHops64) +
            ") for file with " + juce::String(totalSamples) + " samples");
        return;
    }

    // CRASH PROTECTION: Validate image dimensions won't cause memory issues
    const int         imageHeight = fftSize / 2;
    const juce::int64 imageSizeBytes = static_cast<juce::int64>(numHops) *
                                       static_cast<juce::int64>(imageHeight) *
                                       3LL;                  // RGB = 3 bytes per pixel
    const juce::int64 maxImageSizeBytes = 100 * 1024 * 1024; // 100 MB limit

    if (imageSizeBytes > maxImageSizeBytes)
    {
        juce::Logger::writeToLog(
            "[Sample Loader] Spectrogram: Image too large (" +
            juce::String(imageSizeBytes / 1024 / 1024) + " MB), skipping generation");
        return;
    }

    // CRASH PROTECTION: Validate buffer access
    if (safeSample->stereo.getNumChannels() == 0)
        return;

    // Create a mono version for analysis if necessary
    juce::AudioBuffer<float> monoBuffer;
    const float*             audioData = nullptr;

    try
    {
        // CRASH PROTECTION: Validate total samples won't cause memory issues
        const juce::int64 monoBufferSizeBytes =
            static_cast<juce::int64>(totalSamples) * sizeof(float);
        const juce::int64 maxMonoBufferSizeBytes = 500 * 1024 * 1024; // 500 MB limit

        if (monoBufferSizeBytes > maxMonoBufferSizeBytes)
        {
            juce::Logger::writeToLog(
                "[Sample Loader] Spectrogram: Sample too large (" +
                juce::String(monoBufferSizeBytes / 1024 / 1024) + " MB), skipping generation");
            return;
        }

        if (safeSample->stereo.getNumChannels() > 1)
        {
            monoBuffer.setSize(1, totalSamples);
            if (monoBuffer.getNumSamples() != totalSamples)
            {
                juce::Logger::writeToLog(
                    "[Sample Loader] Spectrogram: Failed to allocate mono buffer (" +
                    juce::String(totalSamples) + " samples)");
                return; // Allocation failed
            }

            // CRASH PROTECTION: Validate source pointers before copy
            const float* srcL = safeSample->stereo.getReadPointer(0);
            const float* srcR = safeSample->stereo.getReadPointer(1);
            if (srcL == nullptr || srcR == nullptr)
            {
                juce::Logger::writeToLog("[Sample Loader] Spectrogram: Invalid source pointers");
                return;
            }

            monoBuffer.copyFrom(0, 0, safeSample->stereo, 0, 0, totalSamples);
            monoBuffer.addFrom(0, 0, safeSample->stereo, 1, 0, totalSamples, 0.5f);
            monoBuffer.applyGain(0.5f);
            audioData = monoBuffer.getReadPointer(0);
        }
        else
        {
            audioData = safeSample->stereo.getReadPointer(0);
        }

        // CRASH PROTECTION: Validate audio data pointer
        if (audioData == nullptr)
        {
            juce::Logger::writeToLog("[Sample Loader] Spectrogram: Audio data pointer is null");
            return;
        }
    }
    catch (const std::bad_alloc& e)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Memory allocation failed in spectrogram generation: " +
            juce::String(e.what()) + " (file: " + currentSampleName +
            ", samples: " + juce::String(totalSamples) + ")");
        return;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception preparing spectrogram audio data (file: " +
            currentSampleName + ", samples: " + juce::String(totalSamples) + ")");
        return;
    }

    // Use RGB so JUCE's OpenGLTexture uploads with expected format
    try
    {
        const int imageWidth = numHops;
        const int imageHeight = fftSize / 2;

        // CRASH PROTECTION: Final validation of image dimensions
        if (imageWidth <= 0 || imageHeight <= 0 || imageWidth > maxHops || imageHeight > 10000)
        {
            juce::Logger::writeToLog(
                "[Sample Loader] Spectrogram: Invalid image dimensions (" +
                juce::String(imageWidth) + "x" + juce::String(imageHeight) + ")");
            return;
        }

        spectrogramImage = juce::Image(juce::Image::RGB, imageWidth, imageHeight, true);
        if (spectrogramImage.isNull())
        {
            juce::Logger::writeToLog(
                "[Sample Loader] Spectrogram: Image allocation failed (" +
                juce::String(imageWidth) + "x" + juce::String(imageHeight) + ")");
            return; // Image allocation failed
        }

        juce::Logger::writeToLog(
            "[Sample Loader] Spectrogram: Allocated " + juce::String(imageWidth) + "x" +
            juce::String(imageHeight) + " for " + currentSampleName + " (" +
            juce::String(totalSamples) + " samples)");
    }
    catch (const std::bad_alloc& e)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Memory allocation failed for spectrogram image: " +
            juce::String(e.what()) + " (file: " + currentSampleName + ")");
        return;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[Sample Loader][FATAL] Exception allocating spectrogram image (file: " +
            currentSampleName + ", numHops: " + juce::String(numHops) + ")");
        return;
    }

    juce::dsp::FFT                      fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(fftSize, juce::dsp::WindowingFunction<float>::hann);
    std::vector<float>                  fftData(fftSize * 2);

    for (int i = 0; i < numHops; ++i)
    {
        // CRASH PROTECTION: Bounds checking before memcpy
        const int sourceOffset = i * hopSize;
        if (sourceOffset < 0 || sourceOffset + fftSize > totalSamples)
            continue; // Skip invalid hop

        std::fill(fftData.begin(), fftData.end(), 0.0f);

        // CRASH PROTECTION: Safe memcpy with bounds validation
        const float* sourcePtr = audioData + sourceOffset;
        if (sourcePtr != nullptr)
        {
            memcpy(fftData.data(), sourcePtr, fftSize * sizeof(float));
        }
        else
        {
            continue; // Skip if pointer is invalid
        }

        try
        {
            window.multiplyWithWindowingTable(fftData.data(), fftSize);
            fft.performFrequencyOnlyForwardTransform(fftData.data());
        }
        catch (...)
        {
            juce::Logger::writeToLog(
                "[Sample Loader][FATAL] Exception in FFT processing at hop " + juce::String(i));
            continue; // Skip this hop, continue with next
        }

        for (int j = 0; j < fftSize / 2; ++j)
        {
            try
            {
                const float db =
                    juce::Decibels::gainToDecibels(juce::jmax(fftData[j], 1.0e-9f), -100.0f);
                float level = juce::jmap(db, -100.0f, 0.0f, 0.0f, 1.0f);
                level = juce::jlimit(0.0f, 1.0f, level);

                // CRASH PROTECTION: Validate pixel coordinates
                const int x = i;
                const int y = (fftSize / 2) - 1 - j;
                if (x >= 0 && x < numHops && y >= 0 && y < (fftSize / 2))
                {
                    spectrogramImage.setPixelAt(
                        x, y, juce::Colour::fromFloatRGBA(level, level, level, 1.0f));
                }
            }
            catch (...)
            {
                // Skip this pixel, continue with next
                continue;
            }
        }
    }
}

#if defined(PRESET_CREATOR_UI)
void SampleLoaderModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    // Protect global ID space (prevents conflicts when multiple instances exist)
    ImGui::PushID(this);

    // --- THIS IS THE DEFINITIVE FIX ---
    // 1. Draw all the parameter sliders and buttons FIRST.
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    if (ImGui::Button("Load Sample", ImVec2(itemWidth * 0.48f, 0)))
    {
        juce::File startDir;
        {
            auto appFile = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
            auto dir = appFile.getParentDirectory();
            for (int i = 0; i < 8 && dir.exists(); ++i)
            {
                auto candidate = dir.getSiblingFile("audio").getChildFile("samples");
                if (candidate.exists() && candidate.isDirectory())
                {
                    startDir = candidate;
                    break;
                }
                dir = dir.getParentDirectory();
            }
        }
        if (!startDir.exists())
            startDir = juce::File();
        fileChooser = std::make_unique<juce::FileChooser>(
            "Select Audio Sample", startDir, "*.wav;*.mp3;*.flac;*.aiff;*.ogg");
        auto chooserFlags =
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        fileChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& fc) {
            try
            {
                auto file = fc.getResult();
                if (file != juce::File{})
                {
                    juce::Logger::writeToLog(
                        "[Sample Loader] User selected file: " + file.getFullPathName());
                    loadSample(file);
                }
            }
            catch (...)
            {
                juce::Logger::writeToLog(
                    "[Sample Loader][FATAL] Exception during file chooser callback");
            }
        });
    }
    ImGui::SameLine();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(6)) // Randomize Trig
            ImGui::SameLine();
    }
    if (ImGui::Button("Random", ImVec2(itemWidth * 0.48f, 0)))
    {
        randomizeSample();
    }
    if (hasPendingRandomSample.load())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Queued");
    }

    // Range selection is now handled by the interactive spectrogram in the UI component

    ImGui::Spacing();
    // Main parameters in compact layout
    bool speedModulated = isParamModulated("speed_mod");

    // Lock speed control only when modulated (but not when merely synced,
    // since in sync mode the speed knob defines musical loop length).
    const bool speedLocked = speedModulated;
    if (speedLocked)
    {
        ImGui::BeginDisabled();
        if (speedModulated)
            ImGui::PushStyleColor(
                ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f)); // Yellow tint for CV control
    }

    float speed = speedModulated
                      ? getLiveParamValueFor(
                            "speed_mod", "speed_live", apvts.getRawParameterValue("speed")->load())
                      : apvts.getRawParameterValue("speed")->load();

    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1)) // Speed Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Speed", &speed, 0.25f, 4.0f, "%.2fx"))
    {
        // Only allow edits when not transport-locked and not CV-modulated
        if (!speedLocked)
        {
            apvts.getParameter("speed")->setValueNotifyingHost(
                apvts.getParameterRange("speed").convertTo0to1(speed));
            onModificationEnded();
        }
    }

    if (!speedLocked)
        ModuleProcessor::adjustParamOnWheel(apvts.getParameter("speed"), "speed", speed);

    if (speedLocked)
    {
        if (speedModulated)
            ImGui::PopStyleColor();
        ImGui::EndDisabled();

        if (speedModulated)
        {
            ImGui::SameLine();
            ImGui::Text("(mod)");
        }
    }

    // Tooltip: explain role of speed when synced
    if (syncToTransport.load() && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Relative speed multiplier vs 120 BPM reference:\n"
            "1.00x = match BPM (120 reference)\n"
            "2.00x = double time\n"
            "0.50x = half time");
    }

    bool pitchModulated = isParamModulated("pitch_mod");
    if (pitchModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    float pitch = pitchModulated
                      ? getLiveParamValueFor(
                            "pitch_mod", "pitch_live", apvts.getRawParameterValue("pitch")->load())
                      : apvts.getRawParameterValue("pitch")->load();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(0)) // Pitch Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Pitch", &pitch, -24.0f, 24.0f, "%.1f st"))
    {
        apvts.getParameter("pitch")->setValueNotifyingHost(
            apvts.getParameterRange("pitch").convertTo0to1(pitch));
        onModificationEnded();
    }
    if (!pitchModulated)
        ModuleProcessor::adjustParamOnWheel(apvts.getParameter("pitch"), "pitch", pitch);
    if (pitchModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    }

    // --- Gate slider (formerly volume) ---
    bool gateModulated = isParamModulated("gate_mod");
    if (gateModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    float gate = gateModulated
                     ? getLiveParamValueFor(
                           "gate_mod", "gate_live", apvts.getRawParameterValue("gate")->load())
                     : apvts.getRawParameterValue("gate")->load();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2)) // Gate Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Gate", &gate, 0.0f, 1.0f, "%.2f"))
    {
        if (!gateModulated)
        {
            apvts.getParameter("gate")->setValueNotifyingHost(
                apvts.getParameterRange("gate").convertTo0to1(gate));
            onModificationEnded();
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

    // Range parameters with live modulation feedback
    bool rangeStartModulated = isParamModulated("rangeStart_mod");
    if (rangeStartModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    float rangeStart =
        rangeStartModulated
            ? getLiveParamValueFor("rangeStart_mod", "rangeStart_live", rangeStartParam->load())
            : rangeStartParam->load();
    float rangeEnd = rangeEndParam->load();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4)) // Range Start Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Range Start", &rangeStart, 0.0f, 1.0f, "%.3f"))
    {
        // Ensure start doesn't exceed end (leave at least 0.001 gap)
        rangeStart = juce::jmin(rangeStart, rangeEnd - 0.001f);
        apvts.getParameter("rangeStart")
            ->setValueNotifyingHost(
                apvts.getParameterRange("rangeStart").convertTo0to1(rangeStart));
        onModificationEnded();
    }
    if (!rangeStartModulated)
        ModuleProcessor::adjustParamOnWheel(
            apvts.getParameter("rangeStart"), "rangeStart", rangeStart);
    if (rangeStartModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    }

    bool rangeEndModulated = isParamModulated("rangeEnd_mod");
    if (rangeEndModulated)
    {
        ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    }
    rangeEnd = rangeEndModulated
                   ? getLiveParamValueFor("rangeEnd_mod", "rangeEnd_live", rangeEndParam->load())
                   : rangeEndParam->load();
    rangeStart = rangeStartParam->load(); // Refresh rangeStart for validation
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(5)) // Range End Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Range End", &rangeEnd, 0.0f, 1.0f, "%.3f"))
    {
        // Ensure end doesn't go below start (leave at least 0.001 gap)
        rangeEnd = juce::jmax(rangeEnd, rangeStart + 0.001f);
        apvts.getParameter("rangeEnd")
            ->setValueNotifyingHost(apvts.getParameterRange("rangeEnd").convertTo0to1(rangeEnd));
        onModificationEnded();
    }
    if (!rangeEndModulated)
        ModuleProcessor::adjustParamOnWheel(apvts.getParameter("rangeEnd"), "rangeEnd", rangeEnd);
    if (rangeEndModulated)
    {
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // === SYNC TO TRANSPORT CHECKBOX ===
    bool sync = syncParam ? (*syncParam > 0.5f) : false;
    if (ImGui::Checkbox("Sync to Transport", &sync))
    {
        syncToTransport.store(sync);
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("sync")))
            *p = sync;
        onModificationEnded();
    }

    // === SYNC MODE SELECTOR (only shown when synced) ===
    if (sync)
    {
        int         syncModeIdx = syncModeParam ? syncModeParam->getIndex() : 0;
        const char* syncModeItems[] = {"Relative (Range-Based)", "Absolute (1:1 Time)"};
        if (ImGui::Combo("Sync Mode", &syncModeIdx, syncModeItems, 2))
        {
            if (syncModeParam)
                *syncModeParam = syncModeIdx;
            onModificationEnded();
        }
        if (syncModeParam && ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int newIdx = juce::jlimit(0, 1, syncModeIdx + (wheel > 0.0f ? -1 : 1));
                if (newIdx != syncModeIdx)
                {
                    syncModeIdx = newIdx;
                    *syncModeParam = syncModeIdx;
                    onModificationEnded();
                }
            }
        }
    }
    ImGui::Spacing();

    // === POSITION SLIDER ===
    // Use _mod suffix for virtual routing ID check (per debuginput.md)
    bool posMod = isParamModulated(paramIdPositionMod);

    // Get value: Always use _live telemetry if available (shows playback position moving)
    // If modulated, use live value. If not, use parameter but prefer live for visual feedback
    float posVal = getLiveParamValue("position_live", positionParam ? positionParam->load() : 0.0f);

    // Grey out position slider if CV connected OR synced to transport
    bool positionLocked = posMod || syncToTransport.load();

    if (positionLocked)
    {
        ImGui::BeginDisabled(); // Lock if CV controlled or synced to transport
        if (posMod)
            ImGui::PushStyleColor(
                ImGuiCol_FrameBg, ImVec4(0.2f, 0.6f, 0.2f, 0.3f)); // Green tint for CV control
        else if (syncToTransport.load())
            ImGui::PushStyleColor(
                ImGuiCol_FrameBg, ImVec4(0.4f, 0.4f, 0.4f, 0.3f)); // Grey tint for transport sync
    }

    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(7)) // Position Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Position", &posVal, 0.0f, 1.0f, "%.3f"))
    {
        // Only allow updates if not modulated AND not synced to transport
        if (!positionLocked && positionParam)
        {
            posVal = juce::jlimit(0.0f, 1.0f, posVal);
            // Update parameter using setValueNotifyingHost so audio thread detects the change
            // The audio thread will detect this change and scrub the playhead
            if (auto* p = apvts.getParameter(paramIdPosition))
            {
                p->setValueNotifyingHost(
                    apvts.getParameterRange(paramIdPosition).convertTo0to1(posVal));
            }
            onModificationEnded();
        }
    }

    // Don't use adjustParamOnWheel here - it would fight with playback updates
    // The slider shows live position during playback, and allows scrubbing when not playing

    if (positionLocked)
    {
        if (posMod || syncToTransport.load()) // Pop style color if it was pushed
            ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (posMod)
        {
            ImGui::SameLine();
            ImGui::Text("(mod)");
        }
        else if (syncToTransport.load())
        {
            ImGui::SameLine();
            ImGui::Text("(synced)");
        }
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Sample playback position (0.0 = start, 1.0 = end)\n"
            "Moves automatically during playback\n"
            "Drag to scrub/seek manually\n"
            "CV modulation overrides when connected");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // === CV INPUT MODES SECTION ===
    ThemeText("CV Input Modes", theme.text.section_header);
    ImGui::Spacing();

    // Relative Speed Mod checkbox
    bool relativeSpeedMod =
        relativeSpeedModParam != nullptr && relativeSpeedModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Speed Mod", &relativeSpeedMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("relativeSpeedMod")))
            *p = relativeSpeedMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±4 octaves)\nOFF: CV directly sets speed "
            "(0.25x-4.0x)");
    }

    // Relative Pitch Mod checkbox
    bool relativePitchMod =
        relativePitchModParam != nullptr && relativePitchModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Pitch Mod", &relativePitchMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("relativePitchMod")))
            *p = relativePitchMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±24 semitones)\nOFF: CV directly sets pitch (-24 to "
            "+24 st)");
    }

    // Relative Gate Mod checkbox
    bool relativeGateMod = relativeGateModParam != nullptr && relativeGateModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Gate Mod", &relativeGateMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("relativeGateMod")))
            *p = relativeGateMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±0.5)\nOFF: CV directly sets gate (0-1)");
    }

    // Relative Range Start Mod checkbox
    bool relativeRangeStartMod =
        relativeRangeStartModParam != nullptr && relativeRangeStartModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Range Start Mod", &relativeRangeStartMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(
                apvts.getParameter("relativeRangeStartMod")))
            *p = relativeRangeStartMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±0.25)\nOFF: CV directly sets range start (0-1)");
    }

    // Relative Range End Mod checkbox
    bool relativeRangeEndMod =
        relativeRangeEndModParam != nullptr && relativeRangeEndModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Range End Mod", &relativeRangeEndMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("relativeRangeEndMod")))
            *p = relativeRangeEndMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±0.25)\nOFF: CV directly sets range end (0-1)");
    }

    // Relative Position Mod checkbox
    bool relativePosMod =
        relativePositionModParam != nullptr && relativePositionModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Position Mod", &relativePosMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter(paramIdRelPosMod)))
            *p = relativePosMod;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (bipolar: 0.5 = no change)\nOFF: CV directly sets "
            "position (0-1)");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    bool loop = apvts.getRawParameterValue("loop")->load() > 0.5f;
    if (ImGui::Checkbox("Loop", &loop))
    {
        apvts.getParameter("loop")->setValueNotifyingHost(loop ? 1.0f : 0.0f);
        onModificationEnded();
    }

    int         engineIdx = (int)apvts.getRawParameterValue("engine")->load();
    const char* items[] = {"RubberBand", "Naive"};
    if (ImGui::Combo("Engine", &engineIdx, items, 2))
    {
        apvts.getParameter("engine")->setValueNotifyingHost((float)engineIdx);
        if (sampleProcessor)
            sampleProcessor->setEngine(
                engineIdx == 0 ? SampleVoiceProcessor::Engine::RubberBand
                               : SampleVoiceProcessor::Engine::Naive);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newIdx = juce::jlimit(0, 1, engineIdx + (wheel > 0.0f ? -1 : 1));
            if (newIdx != engineIdx)
            {
                engineIdx = newIdx;
                apvts.getParameter("engine")->setValueNotifyingHost((float)engineIdx);
                if (sampleProcessor)
                    sampleProcessor->setEngine(
                        engineIdx == 0 ? SampleVoiceProcessor::Engine::RubberBand
                                       : SampleVoiceProcessor::Engine::Naive);
                onModificationEnded();
            }
        }
    }

    if (engineIdx == 0)
    {
        bool winShort = apvts.getRawParameterValue("rbWindowShort")->load() > 0.5f;
        if (ImGui::Checkbox("RB Window Short", &winShort))
        {
            apvts.getParameter("rbWindowShort")->setValueNotifyingHost(winShort ? 1.0f : 0.0f);
            if (sampleProcessor)
                sampleProcessor->setRubberBandOptions(
                    winShort, apvts.getRawParameterValue("rbPhaseInd")->load() > 0.5f);
            onModificationEnded();
        }
        bool phaseInd = apvts.getRawParameterValue("rbPhaseInd")->load() > 0.5f;
        if (ImGui::Checkbox("RB Phase Independent", &phaseInd))
        {
            apvts.getParameter("rbPhaseInd")->setValueNotifyingHost(phaseInd ? 1.0f : 0.0f);
            if (sampleProcessor)
                sampleProcessor->setRubberBandOptions(
                    apvts.getRawParameterValue("rbWindowShort")->load() > 0.5f, phaseInd);
            onModificationEnded();
        }
    }

    ImGui::PopItemWidth();

    // 2. Now, draw the sample information and visual display AT THE END.
    if (hasSampleLoaded())
    {
        ImGui::Text("Sample: %s", currentSampleName.toRawUTF8());
        ImGui::Text("Duration: %.2f s", sampleDurationSeconds.load());
        ImGui::Text("Rate: %d Hz", sampleSampleRate.load());

        // Draw a drop zone for hot-swapping with visual feedback
        ImVec2 swapZoneSize = ImVec2(itemWidth, 100.0f);

        // Check if a drag-drop operation is in progress
        bool isDragging = ImGui::GetDragDropPayload() != nullptr;

        if (isDragging)
        {
            // Beautiful blinking animation during drag-drop
            float time = (float)ImGui::GetTime();
            float pulse = (std::sin(time * 8.0f) * 0.5f + 0.5f); // Fast blink
            float glow = (std::sin(time * 3.0f) * 0.3f + 0.7f);  // Slower glow

            // Vibrant cyan with pulsing alpha
            ImU32 fillColor =
                IM_COL32(0, (int)(180 * glow), (int)(220 * glow), (int)(100 + pulse * 155));
            ImU32 borderColor =
                IM_COL32((int)(100 * glow), (int)(255 * pulse), (int)(255 * pulse), 255);

            ImGui::PushStyleColor(ImGuiCol_Button, fillColor);
            ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
            ImGui::Button("##dropzone_sample_swap", swapZoneSize);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
        else
        {
            // Discrete outline only when idle
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0)); // Transparent fill
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 120)); // Gray outline
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::Button("##dropzone_sample_swap", swapZoneSize);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }

        // Draw text centered on the button
        const char* text = isDragging ? "Drop to Swap!" : "Drop to Swap Sample";
        ImVec2      textSize = ImGui::CalcTextSize(text);
        ImVec2      textPos = ImGui::GetItemRectMin();
        textPos.x += (swapZoneSize.x - textSize.x) * 0.5f;
        textPos.y += (swapZoneSize.y - textSize.y) * 0.5f;
        ImU32 textColor = isDragging ? IM_COL32(100, 255, 255, 255) : IM_COL32(150, 150, 150, 200);
        ImGui::GetWindowDrawList()->AddText(textPos, textColor, text);

        // 3. Make this button the drop target for hot-swapping.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SAMPLE_PATH"))
            {
                // CRASH PROTECTION: Validate payload data before use
                if (payload->Data != nullptr && payload->DataSize > 0)
                {
                    // ImGui guarantees null-terminated strings, so we can safely use the data
                    const char* path = (const char*)payload->Data;

                    // Verify null-termination exists (should be at end)
                    if (path[payload->DataSize - 1] == '\0')
                    {
                        // Create safe string copy (handles special characters)
                        // Use strlen to get actual string length (stops at first null)
                        size_t pathLen = std::strlen(path);

                        if (pathLen > 0 && pathLen < payload->DataSize)
                        {
                            juce::String safePath(path, pathLen);
                            juce::File   file(safePath);

                            // Validate file exists before loading
                            if (file.existsAsFile())
                            {
                                try
                                {
                                    loadSample(file);
                                    onModificationEnded();
                                }
                                catch (...)
                                {
                                    juce::Logger::writeToLog(
                                        "[Sample Loader][FATAL] Exception during drag-drop load: " +
                                        safePath);
                                }
                            }
                            else
                            {
                                juce::Logger::writeToLog(
                                    "[Sample Loader] Drag-drop file does not exist: " + safePath);
                            }
                        }
                        else
                        {
                            juce::Logger::writeToLog(
                                "[Sample Loader] Invalid drag-drop payload: invalid string length");
                        }
                    }
                    else
                    {
                        juce::Logger::writeToLog(
                            "[Sample Loader] Invalid drag-drop payload: not null-terminated");
                    }
                }
                else
                {
                    juce::Logger::writeToLog(
                        "[Sample Loader] Invalid drag-drop payload: null or empty data");
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    else
    {
        // If NO sample is loaded, draw a dedicated dropzone with visual feedback
        ImVec2 dropZoneSize = ImVec2(itemWidth, 60.0f);

        // Check if a drag-drop operation is in progress
        bool isDragging = ImGui::GetDragDropPayload() != nullptr;

        if (isDragging)
        {
            // Beautiful blinking animation during drag-drop
            float time = (float)ImGui::GetTime();
            float pulse = (std::sin(time * 8.0f) * 0.5f + 0.5f); // Fast blink
            float glow = (std::sin(time * 3.0f) * 0.3f + 0.7f);  // Slower glow

            // Vibrant cyan with pulsing alpha
            ImU32 fillColor =
                IM_COL32(0, (int)(180 * glow), (int)(220 * glow), (int)(100 + pulse * 155));
            ImU32 borderColor =
                IM_COL32((int)(100 * glow), (int)(255 * pulse), (int)(255 * pulse), 255);

            ImGui::PushStyleColor(ImGuiCol_Button, fillColor);
            ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
            ImGui::Button("##dropzone_sample", dropZoneSize);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
        else
        {
            // Discrete outline only when idle
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0)); // Transparent fill
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 120)); // Gray outline
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::Button("##dropzone_sample", dropZoneSize);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }

        // Draw text centered on top of the button
        const char* text = isDragging ? "Drop Here!" : "Drop Sample Here";
        ImVec2      textSize = ImGui::CalcTextSize(text);
        ImVec2      textPos = ImGui::GetItemRectMin();
        textPos.x += (dropZoneSize.x - textSize.x) * 0.5f;
        textPos.y += (dropZoneSize.y - textSize.y) * 0.5f;
        ImU32 textColor = isDragging ? IM_COL32(100, 255, 255, 255) : IM_COL32(150, 150, 150, 200);
        ImGui::GetWindowDrawList()->AddText(textPos, textColor, text);

        // Make THIS BUTTON the drop target.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SAMPLE_PATH"))
            {
                // CRASH PROTECTION: Validate payload data before use
                if (payload->Data != nullptr && payload->DataSize > 0)
                {
                    // ImGui guarantees null-terminated strings, so we can safely use the data
                    const char* path = (const char*)payload->Data;

                    // Verify null-termination exists (should be at end)
                    if (path[payload->DataSize - 1] == '\0')
                    {
                        // Create safe string copy (handles special characters)
                        // Use strlen to get actual string length (stops at first null)
                        size_t pathLen = std::strlen(path);

                        if (pathLen > 0 && pathLen < payload->DataSize)
                        {
                            juce::String safePath(path, pathLen);
                            juce::File   file(safePath);

                            // Validate file exists before loading
                            if (file.existsAsFile())
                            {
                                try
                                {
                                    loadSample(file);
                                    onModificationEnded();
                                }
                                catch (...)
                                {
                                    juce::Logger::writeToLog(
                                        "[Sample Loader][FATAL] Exception during drag-drop load: " +
                                        safePath);
                                }
                            }
                            else
                            {
                                juce::Logger::writeToLog(
                                    "[Sample Loader] Drag-drop file does not exist: " + safePath);
                            }
                        }
                        else
                        {
                            juce::Logger::writeToLog(
                                "[Sample Loader] Invalid drag-drop payload: invalid string length");
                        }
                    }
                    else
                    {
                        juce::Logger::writeToLog(
                            "[Sample Loader] Invalid drag-drop payload: not null-terminated");
                    }
                }
                else
                {
                    juce::Logger::writeToLog(
                        "[Sample Loader] Invalid drag-drop payload: null or empty data");
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    // --- END OF FIX ---
    ImGui::PopID();
}

void SampleLoaderModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Modulation inputs
    // Channels 0, 1, 2, 4, 5, 6, 7 are drawn inline
    helpers.drawParallelPins("Trigger Mod", 3, nullptr, -1); // Keep Trigger Mod as parallel for now

    // Audio outputs (stereo)
    helpers.drawParallelPins(nullptr, -1, "Out L", 0);
    helpers.drawParallelPins(nullptr, -1, "Out R", 1);
}
#endif

// Parameter bus contract implementation (multi-bus architecture like TTS Performer)
bool SampleLoaderModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    // Bus 0: Playback Mods (Pitch, Speed) - flat channels 0-1
    if (paramId == "pitch_mod")
    {
        outBusIndex = 0;
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == "speed_mod")
    {
        outBusIndex = 0;
        outChannelIndexInBus = 1;
        return true;
    }

    // Bus 1: Control Mods (Gate, Trigger) - flat channels 2-3
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

    // Bus 2: Range Mods (Range Start, Range End) - flat channels 4-5
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

    // Bus 3: Randomize - flat channel 6
    if (paramId == "randomize_mod")
    {
        outBusIndex = 3;
        outChannelIndexInBus = 0;
        return true;
    }

    // Bus 4: Position Mod - flat channel 7
    if (paramId == paramIdPositionMod)
    {
        outBusIndex = 4;
        outChannelIndexInBus = 0;
        return true;
    }

    return false;
}

std::optional<RhythmInfo> SampleLoaderModuleProcessor::getRhythmInfo() const
{
    RhythmInfo info;

    // Build display name with logical ID
    info.displayName = "Sample Loader #" + juce::String(getLogicalId());
    info.sourceType = "sample_loader";

    // Check if synced to transport
    const bool syncEnabled = syncParam && syncParam->load() > 0.5f;
    info.isSynced = syncEnabled;

    // Check if looping (BPM only meaningful when looping)
    const bool isLooping =
        apvts.getRawParameterValue("loop") && apvts.getRawParameterValue("loop")->load() > 0.5f;

    // Read LIVE transport state from parent (not cached copy)
    TransportState transport;
    bool           hasTransport = false;
    if (getParent())
    {
        transport = getParent()->getTransportState();
        hasTransport = true;
    }

    // Sample Loader is active when playing and sample is loaded
    const bool sampleLoaded = hasSampleLoaded();
    info.isActive = sampleLoaded && isPlaying.load();

    // Calculate effective BPM
    if (syncEnabled && isLooping && info.isActive && hasTransport && transport.isPlaying)
    {
        // In sync mode with looping: effective BPM = transport BPM * speed multiplier
        const float speed = apvts.getRawParameterValue("speed")
                                ? apvts.getRawParameterValue("speed")->load()
                                : 1.0f;
        info.bpm = static_cast<float>(transport.bpm * speed);
    }
    else if (!syncEnabled && isLooping && info.isActive)
    {
        // Free-running with looping: calculate from sample duration and speed
        // This would require sample duration, which might not be easily accessible
        // For now, return 0.0f (unknown) - could be enhanced to calculate from sample length
        info.bpm = 0.0f; // Unknown - would need sample duration to calculate
    }
    else
    {
        // Not looping, not synced, or not active
        info.bpm = 0.0f;
    }

    // Validate BPM before returning
    if (!std::isfinite(info.bpm))
        info.bpm = 0.0f;

    return info;
}