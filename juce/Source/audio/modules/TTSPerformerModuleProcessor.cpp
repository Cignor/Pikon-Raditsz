#include "TTSPerformerModuleProcessor.h"
#include "../graph/ModularSynthProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <map>

// --- Audio-based Word Detection Implementation ---
// This function is used when JSON timing data is not available from piper.exe
std::vector<WordTiming> TTSPerformerModuleProcessor::detectWordsFromAudio(
    const juce::AudioBuffer<float>& audio,
    double                          sr)
{
    std::vector<WordTiming> timings;
    if (audio.getNumSamples() == 0 || sr <= 0.0)
        return timings;

    // --- MORE AGGRESSIVE PARAMETERS FOR FINER SLICING ---
    const int   windowSize = 256; // Analyze in smaller chunks (was 1024).
    const float energyThreshold =
        0.01f; // Lower the volume needed to be considered sound (was 0.02f).
    const float silenceThreshold =
        0.008f; // Require audio to be quieter to be considered silence (was 0.01f).
    const double minSilenceSec = 0.04; // Require only 40ms of silence to split words (was 0.1s).
    const double minWordSec = 0.05;    // 50ms minimum duration for a sound to be a "word".

    enum class State
    {
        IN_SILENCE,
        IN_WORD
    };
    State state = State::IN_SILENCE;

    double    wordStartTime = 0.0;
    int       silenceCounter = 0;
    const int minSilenceSamples = (int)(minSilenceSec * sr);

    for (int i = 0; i < audio.getNumSamples(); i += windowSize)
    {
        int    numSamplesInWindow = juce::jmin(windowSize, audio.getNumSamples() - i);
        float  rms = audio.getRMSLevel(0, i, numSamplesInWindow);
        double currentTime = (double)i / sr;

        if (state == State::IN_SILENCE)
        {
            if (rms > energyThreshold)
            {
                state = State::IN_WORD;
                wordStartTime = currentTime;
                silenceCounter = 0;
            }
        }
        else // state == IN_WORD
        {
            if (rms < silenceThreshold)
            {
                silenceCounter += numSamplesInWindow;
                if (silenceCounter >= minSilenceSamples)
                {
                    state = State::IN_SILENCE;
                    double wordEndTime = currentTime - minSilenceSec;
                    if (wordEndTime > wordStartTime + minWordSec)
                    {
                        juce::String wordName = juce::String(timings.size() + 1);
                        timings.emplace_back(wordName, wordStartTime, wordEndTime);
                    }
                }
            }
            else
            {
                silenceCounter = 0;
            }
        }
    }

    if (state == State::IN_WORD)
    {
        double wordEndTime = (double)audio.getNumSamples() / sr;
        if (wordEndTime > wordStartTime + minWordSec)
        {
            juce::String wordName = juce::String(timings.size() + 1);
            timings.emplace_back(wordName, wordStartTime, wordEndTime);
        }
    }

    return timings;
}

#if defined(PRESET_CREATOR_UI)
void TTSPerformerModuleProcessor::playSelectedClipFromTrim()
{
    if (!(selectedClip && selectedClip->audio.getNumSamples() > 0))
        return;
    const juce::ScopedLock lock(audioBufferLock);
    float                  trimStartNorm = apvts.getRawParameterValue("trimStart")->load();
    int trimStart = (int)std::floor(trimStartNorm * selectedClip->audio.getNumSamples());
    readPosition = (double)juce::jlimit(0, selectedClip->audio.getNumSamples() - 1, trimStart);
    isPlaying = true;
}

void TTSPerformerModuleProcessor::stopPlayback() { isPlaying = false; }

void TTSPerformerModuleProcessor::forceStop() { isPlaying = false; }

bool TTSPerformerModuleProcessor::deleteSelectedClipFromDisk()
{
    if (!selectedClip)
        return false;
    juce::File dir = getClipsRootDir().getChildFile(selectedClip->clipId);
    bool       ok = dir.deleteRecursively();
    {
        const juce::ScopedLock c(clipCacheLock);
        clipCache.erase(selectedClip->clipId);
        selectedClip.reset();
    }
    return ok;
}

bool TTSPerformerModuleProcessor::renameSelectedClipOnDisk(const juce::String& newName)
{
    if (!selectedClip || newName.isEmpty())
        return false;

    // --- FIX: Don't rename directory, update metadata instead ---
    // The directory name is the hash ID and must remain unchanged
    juce::File dir = getClipsRootDir().getChildFile(selectedClip->clipId);
    if (!dir.exists())
        return false;

    // Update the info.xml metadata with the new name
    juce::File       metaFile = dir.getChildFile("info.xml");
    juce::XmlElement meta("ClipInfo");
    meta.setAttribute("name", newName.substring(0, 48));
    meta.setAttribute("text", selectedClip->text);      // Keep original text
    meta.setAttribute("model", selectedClip->modelKey); // Keep model info

    bool ok = metaFile.replaceWithText(meta.toString());
    if (ok)
    {
        const juce::ScopedLock c(clipCacheLock);
        // Update the clip in memory (ID stays the same)
        selectedClip->name = newName;
    }
    return ok;
}
#endif

// Using a simplified parameter layout for this example.
// You can merge this with your more detailed layout.
juce::AudioProcessorValueTreeState::ParameterLayout TTSPerformerModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Core parameters
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("volume", "Volume", 0.0f, 1.0f, 0.8f));

    // Transport & Sequencer (following SampleLoader pattern)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rate", "Rate (Hz)", juce::NormalisableRange<float>(0.1f, 20.0f, 0.01f, 0.5f), 2.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>("gate", "Gate", 0.0f, 1.0f, 0.8f));

    // Trim range parameters
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "trimStart", "Trim Start", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "trimEnd", "Trim End", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Speed/Pitch playback parameters (SampleLoader-style)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "speed", "Speed", juce::NormalisableRange<float>(0.25f, 4.0f, 0.01f), 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "pitch",
            "Pitch (semitones)",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f),
            0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "engine", "Engine", juce::StringArray{"RubberBand", "Naive"}, 1));

    // Transport sync parameters
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("sync", "Sync to Transport", false));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "rate_division",
            "Division",
            juce::StringArray{"1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8"},
            3));

    // NOTE: Do NOT create APVTS parameters for modulation inputs. They are CV buses only.

    return {params.begin(), params.end()};
}

// Helper function to find the correct word index for a given time in seconds
int TTSPerformerModuleProcessor::findWordIndexForTime(float timeSeconds) const
{
    if (!selectedClip)
        return 0;

    const auto& timings = getActiveTimings();
    if (timings.empty())
        return 0;

    for (int i = 0; i < (int)timings.size(); ++i)
    {
        if (timings[i].startTimeSeconds >= timeSeconds)
        {
            return i; // Return the index of the first word at or after the time
        }
    }

    return (int)timings.size() - 1; // Not found, return the last word
}

TTSPerformerModuleProcessor::TTSPerformerModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              // CORRECTED BUS LAYOUT:
              .withInput(
                  "Global Mods",
                  juce::AudioChannelSet::discreteChannels(5),
                  true) // Bus 0: Rate, Gate, Trigger, Reset, Randomize
              .withInput(
                  "Trim Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 1: Trim Start, Trim End
              .withInput(
                  "Playback Mods",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Bus 2: Speed, Pitch
              .withInput(
                  "Word Triggers",
                  juce::AudioChannelSet::discreteChannels(16),
                  true) // Bus 3: Word 1-16 Triggers
              // Output bus: 1 mono audio + 1 word gate + 1 EOP + 16 per-word gates + 16 per-word
              // triggers = 35 channels
              .withOutput("Outputs", juce::AudioChannelSet::discreteChannels(35), true)),
      apvts(*this, nullptr, "TTSPerformerParams", createParameterLayout()), textFifo(64),
      textFifoBuffer(64), synthesisThread(*this)
{
    juce::Logger::writeToLog(
        "[TTS][Ctor] instance=" + juce::String((juce::uint64)(uintptr_t)this) +
        " storedLogicalId=" + juce::String((int)getLogicalId()));
    volumeParam = apvts.getRawParameterValue("volume");
    rateParam = apvts.getRawParameterValue("rate");
    gateParam = apvts.getRawParameterValue("gate");

    synthesisThread.startThread();

    // Load clips from disk on startup
    loadClipsFromDisk();
}

TTSPerformerModuleProcessor::~TTSPerformerModuleProcessor() { synthesisThread.stopThread(5000); }

void TTSPerformerModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    timePitch.prepare(sampleRate, 2, samplesPerBlock);
    interleavedCapacityFrames = samplesPerBlock; // keep equal to block by default
    interleavedInput.allocate((size_t)(interleavedCapacityFrames * 2), true);
    interleavedOutput.allocate((size_t)(interleavedCapacityFrames * 2), true);
    readPosition = 0.0;
    stepAccumulatorSec = 0.0;
    lastScaledBeats_tts = 0.0;
    juce::Logger::writeToLog(
        "[TTS][Prepare] instance=" + juce::String((juce::uint64)(uintptr_t)this) +
        " storedLogicalId=" + juce::String((int)getLogicalId()));
}

void TTSPerformerModuleProcessor::setTimingInfo(const TransportState& state)
{
    // --- THIS IS THE DEFINITIVE FIX ---

    // 1. Set the module's internal play state directly from the master transport.
    // This is what "emulating the spacebar" means.
    isPlaying = state.isPlaying;

    // 2. Check if the transport has just started playing from a stopped state.
    if (state.isPlaying && !wasPlaying)
    {
        juce::Logger::writeToLog("[TTS FIX] Play Toggled ON. Resetting playheads.");
        if (selectedClip && getSampleRate() > 0)
        {
            // Calculate start time in seconds
            const double clipDurationSeconds =
                selectedClip->audio.getNumSamples() / getSampleRate();
            const double trimStartSeconds =
                apvts.getRawParameterValue("trimStart")->load() * clipDurationSeconds;

            // Find the correct starting word and reset both playheads
            currentWordIndex = findWordIndexForTime((float)trimStartSeconds);
            readPosition = trimStartSeconds * getSampleRate();

            juce::Logger::writeToLog(
                "[TTS FIX] Reset complete. Start Word: " + juce::String(currentWordIndex) +
                ", Read Position: " + juce::String(readPosition));
        }

        // Reset all internal schedulers and clocks.
        stepAccumulatorSec = 0.0;
        lastScaledBeats_tts = 0.0;
    }
    else if (!state.isPlaying && wasPlaying)
    {
        juce::Logger::writeToLog("[TTS FIX] Play Toggled OFF.");
    }

    wasPlaying = state.isPlaying;
    m_currentTransport = state;
    // --- END OF FIX ---
}

void TTSPerformerModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    static bool once = false;
    if (!once)
    {
        once = true;
        juce::Logger::writeToLog(
            "[TTS][Audio] instance=" + juce::String((juce::uint64)(uintptr_t)this));
        juce::Logger::writeToLog("[TTS][Audio] logicalId=" + juce::String((int)getLogicalId()));
        const int inBuses = getBusCount(true);
        juce::Logger::writeToLog("[TTS][Audio] inputBuses=" + juce::String(inBuses));
        if (inBuses > 0 && getBus(true, 0) != nullptr)
            juce::Logger::writeToLog(
                "[TTS][Audio] bus0 channels=" +
                juce::String(getBus(true, 0)->getNumberOfChannels()));
    }
    // CRITICAL FIX: Read from multiple input buses (AudioProcessorGraph routing fix)
    // Bus 0: Global Mods (4 channels: Rate, Gate, Trigger, Reset)
    // Bus 1: Trim Mods (2 channels: Trim Start, Trim End)
    // Bus 2: Playback Mods (2 channels: Speed, Pitch)
    // Bus 3: Word Triggers (16 channels: Word 1-16)
    auto globalBus = getBusBuffer(buffer, true, 0);
    auto trimBus = getBusBuffer(buffer, true, 1);
    auto playbackBus = getBusBuffer(buffer, true, 2);
    auto wordTrigBus = getBusBuffer(buffer, true, 3);

    // DO NOT clear output bus - it may share memory with input buses in AudioProcessorGraph
    // The existing logic already handles silent output sample-by-sample in the main loop

    const int    numSamples = buffer.getNumSamples();
    const double sr = juce::jmax(1.0, getSampleRate());

    // Get base parameters (ALWAYS)
    float baseRate = rateParam->load();
    float baseGate = gateParam->load();

    // Check modulation (like BestPractice line 54-56)
    const bool isRateMod = isParamInputConnected("rate_mod");
    const bool isGateMod = isParamInputConnected("gate_mod");
    const bool isTrigMod = isParamInputConnected("trigger_mod");
    const bool isResetMod = isParamInputConnected("reset_mod");
    const bool isRandomizeMod = isParamInputConnected("randomize_mod"); // <-- NEW
    const bool isTrimStartMod = isParamInputConnected("trimStart_mod");
    const bool isTrimEndMod = isParamInputConnected("trimEnd_mod");

    // Check for speed/pitch modulation
    const bool isSpeedMod = isParamInputConnected("speed_mod");
    const bool isPitchMod = isParamInputConnected("pitch_mod");

    // Get base speed/pitch parameters
    float baseSpeed = apvts.getRawParameterValue("speed")->load();
    float basePitch = apvts.getRawParameterValue("pitch")->load();

    // CORRECTED: Read CV pointers from the correct buses according to getParamRouting()
    // Bus 0: Global Mods (Rate, Gate, Trigger, Reset, Randomize) - channels 0,1,2,3,4
    const float* rateCV =
        isRateMod && globalBus.getNumChannels() > 0 ? globalBus.getReadPointer(0) : nullptr;
    const float* gateCV =
        isGateMod && globalBus.getNumChannels() > 1 ? globalBus.getReadPointer(1) : nullptr;
    const float* trigCV =
        isTrigMod && globalBus.getNumChannels() > 2 ? globalBus.getReadPointer(2) : nullptr;
    const float* resetCV =
        isResetMod && globalBus.getNumChannels() > 3 ? globalBus.getReadPointer(3) : nullptr;
    const float* randomizeCV = isRandomizeMod && globalBus.getNumChannels() > 4
                                   ? globalBus.getReadPointer(4)
                                   : nullptr; // <-- NEW

    // Bus 1: Trim Mods (Trim Start, Trim End) - channels 0,1
    const float* trimStartCV =
        isTrimStartMod && trimBus.getNumChannels() > 0 ? trimBus.getReadPointer(0) : nullptr;
    const float* trimEndCV =
        isTrimEndMod && trimBus.getNumChannels() > 1 ? trimBus.getReadPointer(1) : nullptr;

    // Bus 2: Playback Mods (Speed, Pitch) - channels 0,1
    const float* speedCV =
        isSpeedMod && playbackBus.getNumChannels() > 0 ? playbackBus.getReadPointer(0) : nullptr;
    const float* pitchCV =
        isPitchMod && playbackBus.getNumChannels() > 1 ? playbackBus.getReadPointer(1) : nullptr;

    // DEBUG: Log multi-bus state and CV values (first call + every ~5s)
    static int debugFrameCounter = 0;
    if (debugFrameCounter == 0 || debugFrameCounter % 240 == 0)
    {
        juce::String dbgMsg = "[TTS CV Debug #" + juce::String(debugFrameCounter) + "] ";
        dbgMsg += "buses: global=" + juce::String(globalBus.getNumChannels()) + " ";
        dbgMsg += "trim=" + juce::String(trimBus.getNumChannels()) + " ";
        dbgMsg += "playback=" + juce::String(playbackBus.getNumChannels()) + " ";
        dbgMsg += "words=" + juce::String(wordTrigBus.getNumChannels()) + " | ";
        if (rateCV)
            dbgMsg += "rate=" + juce::String(numSamples > 0 ? rateCV[0] : -999.0f, 3) + " ";
        else
            dbgMsg += "rate=null ";
        if (gateCV)
            dbgMsg += "gate=" + juce::String(numSamples > 0 ? gateCV[0] : -999.0f, 3) + " ";
        else
            dbgMsg += "gate=null ";
        if (trimStartCV)
            dbgMsg +=
                "trimStart=" + juce::String(numSamples > 0 ? trimStartCV[0] : -999.0f, 3) + " ";
        else
            dbgMsg += "trimStart=null ";
        if (trimEndCV)
            dbgMsg += "trimEnd=" + juce::String(numSamples > 0 ? trimEndCV[0] : -999.0f, 3) + " ";
        else
            dbgMsg += "trimEnd=null ";
        if (speedCV)
            dbgMsg += "speed=" + juce::String(numSamples > 0 ? speedCV[0] : -999.0f, 3) + " ";
        else
            dbgMsg += "speed=null ";
        if (pitchCV)
            dbgMsg += "pitch=" + juce::String(numSamples > 0 ? pitchCV[0] : -999.0f, 3);
        else
            dbgMsg += "pitch=null";
        juce::Logger::writeToLog(dbgMsg);

        // Also log modulation states
        juce::String modStates = "[TTS MOD STATES] ";
        modStates += "rateMod=" + juce::String(isRateMod ? "ON" : "OFF") + " ";
        modStates += "gateMod=" + juce::String(isGateMod ? "ON" : "OFF") + " ";
        modStates += "speedMod=" + juce::String(isSpeedMod ? "ON" : "OFF") + " ";
        modStates += "pitchMod=" + juce::String(isPitchMod ? "ON" : "OFF");
        juce::Logger::writeToLog(modStates);
    }
    debugFrameCounter++;

    // CRITICAL FIX: Get output pointers directly from buffer (AudioProcessorGraph routing
    // requirement)
    auto* audioOut = buffer.getNumChannels() > 0 ? buffer.getWritePointer(0) : nullptr;
    auto* wordGateOut = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    auto* eopGateOut = buffer.getNumChannels() > 2 ? buffer.getWritePointer(2) : nullptr;

    bool wasPlayingLastBlock = isPlaying;
    bool hasValidClip = (selectedClip && selectedClip->audio.getNumSamples() > 0);

    // Precompute trim boundaries in samples for this block
    int clipLen = hasValidClip ? selectedClip->audio.getNumSamples() : 0;
    int trimStartSample = 0, trimEndSample = 0;
    if (hasValidClip)
    {
        float trimStartNormB = apvts.getRawParameterValue("trimStart")->load();
        float trimEndNormB = apvts.getRawParameterValue("trimEnd")->load();
        trimStartSample = (int)(trimStartNormB * clipLen);
        trimEndSample = (int)(trimEndNormB * clipLen);
        trimStartSample = juce::jlimit(0, clipLen, trimStartSample);
        trimEndSample = juce::jlimit(trimStartSample, clipLen, trimEndSample);
        startSamplePos = (double)trimStartSample;
        endSamplePos = (double)juce::jmax(trimStartSample + 1, trimEndSample);
    }

    // Word stepping setup (using last/selected timings)
    auto&        timingsForClip = getActiveTimings();
    const bool   haveTimings = hasValidClip && !timingsForClip.empty();
    const double srD = sr;

    for (int i = 0; i < numSamples; ++i)
    {
        // Per-sample rate/gate (ALWAYS read CV) - Use CV to modulate base rate exponentially
        float currentRate = baseRate; // Start with the slider's value
        if (rateCV)
        {
            // Use the incoming CV to modulate the base rate by +/- 2 octaves
            const float cv = juce::jlimit(0.0f, 1.0f, rateCV[i]);
            const float octaveRange = 4.0f; // Total modulation range in octaves (-2 to +2)
            const float octaveOffset = (cv - 0.5f) * octaveRange; // Map CV to [-2.0, 2.0]

            currentRate = baseRate * std::pow(2.0f, octaveOffset);
        }

        float currentGate = baseGate; // The slider's value (e.g., 0.8)

        if (gateCV)
        {
            // === START OF TRACING LOGIC ===

            // Point A: The raw value read directly from the input buffer.
            float rawCVValue = gateCV[i];

            // Point B: The value after your juce::jlimit logic is applied.
            float processedCVValue = juce::jlimit(0.0f, 1.0f, rawCVValue);

            // This is the original logic line. We'll use our traced value instead.
            currentGate = processedCVValue;

            // Point C: The final value of currentGate before it's used.
            float finalGateValue = currentGate;

            // Log all three points for the first 5 samples to see the transformation.
            if (i < 5)
            {
                juce::Logger::writeToLog(
                    "[GATE TRACE] Sample " + juce::String(i) +
                    ": [A] Raw=" + juce::String(rawCVValue, 3) +
                    " -> [B] Processed=" + juce::String(processedCVValue, 3) +
                    " -> [C] Final=" + juce::String(finalGateValue, 3));
            }
            // === END OF TRACING LOGIC ===
        }

        // Trim range handling (per-sample; CV modulates slider values)
        float trimStartNorm = apvts.getRawParameterValue("trimStart")->load();
        if (trimStartCV)
        {
            // Remap incoming CV [0, 1] to an offset of [-0.5, +0.5]
            const float cvOffset = trimStartCV[i] - 0.5f;
            // Add the offset to the base value from the slider
            trimStartNorm += cvOffset;
        }
        // Clamp the final result to the valid [0, 1] range
        trimStartNorm = juce::jlimit(0.0f, 1.0f, trimStartNorm);

        float trimEndNorm = apvts.getRawParameterValue("trimEnd")->load();
        if (trimEndCV)
        {
            // Remap incoming CV [0, 1] to an offset of [-0.5, +0.5]
            const float cvOffset = trimEndCV[i] - 0.5f;
            // Add the offset to the base value from the slider
            trimEndNorm += cvOffset;
        }
        // Clamp the final result to the valid [0, 1] range
        trimEndNorm = juce::jlimit(0.0f, 1.0f, trimEndNorm);
        if (trimStartNorm >= trimEndNorm)
            trimStartNorm = juce::jmax(0.0f, trimEndNorm - 0.001f);

        // Update per-sample trim and apply to engine loop bounds
        clipLen = hasValidClip ? selectedClip->audio.getNumSamples() : 0;
        trimStartSample = (int)(trimStartNorm * clipLen);
        trimEndSample = (int)(trimEndNorm * clipLen);
        startSamplePos = (double)trimStartSample;
        endSamplePos = (double)juce::jmax(trimStartSample + 1, trimEndSample);

        // Process triggers even without clip loaded
        if (resetCV)
        {
            bool resetHigh = resetCV[i] > 0.5f;
            if (resetHigh && !lastResetHigh)
            {
                // Reset to trim start
                currentWordIndex = findFirstWordIndexAtOrAfter(
                    trimStartNorm * (hasValidClip ? selectedClip->durationSeconds : 0.0));
                readPosition = (double)trimStartSample;
                if (hasValidClip)
                    isPlaying = true;
                phase = 0.0;
            }
            lastResetHigh = resetHigh;
        }

        if (trigCV)
        {
            bool trigHigh = trigCV[i] > 0.5f;
            if (trigHigh && !lastTrigHigh)
            {
                // Trigger starts at trim start
                readPosition = (double)trimStartSample;
                currentWordIndex = findFirstWordIndexAtOrAfter(
                    trimStartNorm * (hasValidClip ? selectedClip->durationSeconds : 0.0));
                if (hasValidClip)
                    isPlaying = true;
                phase = 0.0;
            }
            lastTrigHigh = trigHigh;
        }

        // --- Randomize Trigger ---
        if (randomizeCV != nullptr) // Check if the pointer is valid
        {
            const bool trigHigh = randomizeCV[i] > 0.5f;
            if (trigHigh && !lastRandomizeTriggerHigh)
            {
                const juce::ScopedLock c(clipCacheLock);
                if (clipCache.size() > 1 && selectedClip)
                {
                    std::vector<juce::String> otherKeys;
                    for (const auto& pair : clipCache)
                    {
                        if (pair.first != selectedClip->clipId)
                        {
                            otherKeys.push_back(pair.first);
                        }
                    }
                    if (!otherKeys.empty())
                    {
                        juce::Random rng;
                        juce::String randomKey = otherKeys[rng.nextInt((int)otherKeys.size())];
                        selectClipByKey(randomKey);
                    }
                }
                lastRandomizeTriggerHigh =
                    true; // Prevent re-triggering within the same high signal
            }
            else if (!trigHigh)
            {
                lastRandomizeTriggerHigh = false;
            }
        }

        // Per-word trigger inputs (Bus 3, channels 0-15)
        if (hasValidClip)
        {
            for (int w = 0; w < juce::jmin(16, (int)selectedClip->timings.size()); ++w)
            {
                if (wordTrigBus.getNumChannels() > w)
                {
                    const float* wordTrigCV = wordTrigBus.getReadPointer(w);
                    bool         wordTrigHigh = wordTrigCV[i] > 0.5f;
                    if (wordTrigHigh && !lastWordTrigHigh[w])
                    {
                        // Jump to this word
                        currentWordIndex = w;
                        const auto& wordTiming = selectedClip->timings[w];
                        double      jumpPos = juce::jlimit(
                            (double)trimStartSample,
                            (double)trimEndSample,
                            wordTiming.startTimeSeconds * sr);
                        readPosition = jumpPos;
                        isPlaying = true;
                        phase = 0.0;
                    }
                    lastWordTrigHigh[w] = wordTrigHigh;
                }
            }
        }

        // Rate-based stepping scheduler (jump to word starts)
        const bool syncEnabled = apvts.getRawParameterValue("sync")->load() > 0.5f;
        if (hasValidClip && haveTimings)
        {
            bool advanceStep = false;
            if (syncEnabled && m_currentTransport.isPlaying)
            {
                // SYNC MODE
                int divisionIndex = (int)apvts.getRawParameterValue("rate_division")->load();
                // Use global division if a Tempo Clock has override enabled
                // IMPORTANT: Read from parent's LIVE transport state, not cached copy (which is
                // stale)
                if (getParent())
                {
                    int globalDiv = getParent()->getTransportState().globalDivisionIndex.load();
                    if (globalDiv >= 0)
                        divisionIndex = globalDiv;
                }
                static const double divisions[] = {
                    1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0, 2.0, 4.0, 8.0};
                const double beatDivision = divisions[juce::jlimit(0, 8, divisionIndex)];

                double beatsNow = m_currentTransport.songPositionBeats +
                                  (i / srD / 60.0 * m_currentTransport.bpm);
                double scaledBeats = beatsNow * beatDivision;

                if (static_cast<long long>(scaledBeats) >
                    static_cast<long long>(lastScaledBeats_tts))
                {
                    advanceStep = true;
                }
                lastScaledBeats_tts = scaledBeats;
            }
            else if (currentRate > 0.0f)
            {
                // FREE-RUNNING MODE
                if (stepAccumulatorSec <= 0.0)
                {
                    advanceStep = true;
                    stepAccumulatorSec += (1.0 / (double)currentRate);
                }
                stepAccumulatorSec -= (1.0 / srD);
            }

            if (advanceStep)
            {
                clampWordIndexToTrim();
                crossfadeStartPosition = readPosition;
                const auto& w = getActiveTimings()[(size_t)juce::jlimit(
                    0, (int)getActiveTimings().size() - 1, currentWordIndex)];
                crossfadeEndPosition =
                    juce::jlimit(startSamplePos, endSamplePos - 1.0, w.startTimeSeconds * srD);
                crossfadeSamplesTotal = (int)(srD * 0.020); // 20ms crossfade
                crossfadeSamplesRemaining = crossfadeSamplesTotal;

                if (currentWordIndex < 16)
                    wordTriggerPending[currentWordIndex] = (int)std::ceil(0.001 * srD);

                // Advance to next word
                const auto& t = getActiveTimings();
                if (!t.empty())
                {
                    currentWordIndex++;
                    if (currentWordIndex >= (int)t.size())
                        currentWordIndex = 0;
                }
            }
        }

        // Read speed/pitch for this sample
        float currentSpeed = baseSpeed; // Start with the base value from the slider
        if (speedCV)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, speedCV[i]);

            // Use CV to modulate speed by +/- 2 octaves (0.25x to 4x) around the base speed
            const float octaveRange = 4.0f; // Total modulation range in octaves
            const float octaveOffset =
                (cv - 0.5f) * octaveRange; // Remaps CV from [0, 1] to [-2, +2]

            currentSpeed = baseSpeed * std::pow(2.0f, octaveOffset);
        }

        // Clamp the final result to the parameter's valid range
        currentSpeed = juce::jlimit(0.25f, 4.0f, currentSpeed);

        float currentPitch = basePitch; // Start with the base value from the slider
        if (pitchCV)
        {
            // Remap incoming CV to a bipolar [-1, 1] range if it isn't already
            const float rawCV = pitchCV[i];
            const float bipolarCV =
                (rawCV >= 0.0f && rawCV <= 1.0f) ? (rawCV * 2.0f - 1.0f) : rawCV;

            // Use the bipolar CV to modulate by a defined range, e.g., +/- 12 semitones (one
            // octave)
            const float pitchModulationRange = 12.0f;
            currentPitch += bipolarCV * pitchModulationRange;

            // DEBUG: Log pitch modulation values (occasionally)
            static int pitchLogCounter = 0;
            if ((pitchLogCounter++ % 4800) == 0) // Log every ~100ms
            {
                juce::Logger::writeToLog(
                    "[TTS PITCH] basePitch=" + juce::String(basePitch, 2) + " bipolarCV=" +
                    juce::String(bipolarCV, 3) + " currentPitch=" + juce::String(currentPitch, 2));
            }
        }

        // Clamp the final result to the parameter's valid range
        currentPitch = juce::jlimit(-24.0f, 24.0f, currentPitch);

        // Generate audio ONLY if clip loaded
        if (isPlaying && hasValidClip)
        {
            const juce::ScopedLock lock(audioBufferLock);

            // --- NEW: CROSSFADE LOGIC ---
            if (crossfadeSamplesRemaining > 0)
            {
                // We are in a crossfade.
                const float fadeProgress =
                    1.0f - ((float)crossfadeSamplesRemaining / (float)crossfadeSamplesTotal);
                const float fadeInGain = fadeProgress;
                const float fadeOutGain = 1.0f - fadeProgress;

                // Get sample from the OLD position
                int   oldPos = (int)crossfadeStartPosition;
                float oldSample =
                    (oldPos < clipLen) ? selectedClip->audio.getSample(0, oldPos) : 0.0f;

                // Get sample from the NEW position
                int   newPos = (int)crossfadeEndPosition;
                float newSample =
                    (newPos < clipLen) ? selectedClip->audio.getSample(0, newPos) : 0.0f;

                // Blend them
                float finalSample = (oldSample * fadeOutGain) + (newSample * fadeInGain);
                if (audioOut)
                    audioOut[i] = finalSample * currentGate * volumeParam->load();

                // Advance both read heads for the next sample in the fade
                crossfadeStartPosition += 1.0;
                crossfadeEndPosition += 1.0;

                // When the fade is done, snap the main readPosition to the correct new location
                if (--crossfadeSamplesRemaining == 0)
                {
                    readPosition = crossfadeEndPosition;
                }
            }
            else // --- ORIGINAL PLAYBACK LOGIC (when not crossfading) ---
            {
                // Compute effective time/pitch like SampleVoiceProcessor
                const float effectiveTime = juce::jlimit(0.25f, 4.0f, currentSpeed);
                const float effectivePitchSemis = currentPitch;

                // Select engine: 0=RubberBand,1=Naive (default Naive if param absent)
                int engineIdx = 1; // Naive default
                if (auto* p = apvts.getParameter("engine"))
                    engineIdx = (int)p->getValue();

                if (engineIdx == 1) // Naive
                {
                    // Linear interpolation over mono buffer
                    float        sample = 0.0f;
                    const int    srcLen = selectedClip->audio.getNumSamples();
                    const double pitchScale = std::pow(2.0, (double)effectivePitchSemis / 12.0);
                    const double step =
                        (double)pitchScale / (double)juce::jmax(0.0001f, effectiveTime);

                    // Wrap within trim window
                    if (readPosition >= endSamplePos)
                        readPosition = startSamplePos + (readPosition - endSamplePos);
                    int base = (int)readPosition;
                    base = juce::jlimit(0, srcLen - 1, base);
                    const int   next = juce::jmin(srcLen - 1, base + 1);
                    const float frac = (float)(readPosition - (double)base);
                    const float s0 = selectedClip->audio.getSample(0, base);
                    const float s1 = selectedClip->audio.getSample(0, next);
                    sample = s0 + frac * (s1 - s0);
                    if (audioOut)
                        audioOut[i] = sample * currentGate * volumeParam->load();
                    readPosition += step;
                    if (readPosition >= endSamplePos)
                        readPosition = startSamplePos + (readPosition - endSamplePos);
                }
                else // RubberBand via TimePitchProcessor
                {
                    // Ensure interleaved buffers large enough
                    if (1 > interleavedCapacityFrames)
                    {
                        interleavedCapacityFrames = 1;
                        interleavedInput.allocate((size_t)(interleavedCapacityFrames * 2), true);
                        interleavedOutput.allocate((size_t)(interleavedCapacityFrames * 2), true);
                    }

                    // Feed one frame (mono duplicated) from current readPosition
                    int pos = (int)readPosition;
                    if (readPosition >= endSamplePos)
                        readPosition = startSamplePos + (readPosition - endSamplePos),
                        pos = (int)readPosition;
                    pos = juce::jlimit(0, selectedClip->audio.getNumSamples() - 1, pos);
                    float  s = selectedClip->audio.getSample(0, pos);
                    float* inLR = interleavedInput.getData();
                    inLR[0] = s;
                    inLR[1] = s;
                    // Force immediate parameter application by resetting processor when values
                    // change
                    if (lastEffectiveTime != effectiveTime ||
                        lastEffectivePitch != effectivePitchSemis)
                    {
                        timePitch.reset();
                        lastEffectiveTime = effectiveTime;
                        lastEffectivePitch = effectivePitchSemis;
                        // PRIME: push a burst of frames so RubberBand has material to output
                        // immediately
                        const int primeFramesDesired = 64;
                        const int availableWindow =
                            (int)juce::jmax(1.0, endSamplePos - startSamplePos);
                        const int primeFrames =
                            juce::jlimit(1, primeFramesDesired, availableWindow - 1);
                        if (interleavedCapacityFrames < primeFrames)
                        {
                            interleavedCapacityFrames = primeFrames;
                            interleavedInput.allocate(
                                (size_t)(interleavedCapacityFrames * 2), true);
                            interleavedOutput.allocate(
                                (size_t)(interleavedCapacityFrames * 2), true);
                            inLR = interleavedInput.getData();
                        }
                        // Fill prime frames from current readPosition
                        double posPrime = readPosition;
                        for (int pf = 0; pf < primeFrames; ++pf)
                        {
                            if (posPrime >= endSamplePos)
                                posPrime = startSamplePos + (posPrime - endSamplePos);
                            int ip = juce::jlimit(
                                0, selectedClip->audio.getNumSamples() - 1, (int)posPrime);
                            const float v = selectedClip->audio.getSample(0, ip);
                            inLR[2 * pf + 0] = v;
                            inLR[2 * pf + 1] = v;
                            posPrime += 1.0;
                        }
                        // Apply parameters before prime
                        timePitch.setTimeStretchRatio(effectiveTime);
                        timePitch.setPitchSemitones(effectivePitchSemis);
                        timePitch.putInterleaved(inLR, primeFrames);
                        // Advance read head by the frames we just fed
                        readPosition += (double)primeFrames;
                        if (readPosition >= endSamplePos)
                            readPosition = startSamplePos + (readPosition - endSamplePos);
                        // Start a short ramp to suppress de-clicks
                        rbFadeSamplesTotal = 32;
                        rbFadeSamplesRemaining = rbFadeSamplesTotal;
                    }
                    timePitch.setTimeStretchRatio(effectiveTime);
                    timePitch.setPitchSemitones(effectivePitchSemis);
                    timePitch.putInterleaved(inLR, 1);
                    float* outLR = interleavedOutput.getData();
                    // Drain a small burst to minimize latency
                    int produced = 0;
                    {
                        int       drained = 0;
                        const int maxDrain =
                            4; // small burst keeps latency low without starving input
                        while (drained < maxDrain)
                        {
                            const int got = timePitch.receiveInterleaved(outLR + (drained * 2), 1);
                            if (got <= 0)
                                break;
                            drained += got;
                        }
                        produced = drained;
                    }
                    if (produced > 0)
                    {
                        float outSample = outLR[(produced - 1) * 2 + 0];
                        // Apply short fade-in after parameter changes to soften clicks
                        if (rbFadeSamplesRemaining > 0 && rbFadeSamplesTotal > 0)
                        {
                            const float fade =
                                1.0f - (float)rbFadeSamplesRemaining / (float)rbFadeSamplesTotal;
                            outSample *= fade;
                            rbFadeSamplesRemaining--;
                        }
                        if (audioOut)
                            audioOut[i] = outSample * currentGate * volumeParam->load();
                        // Advance read head by the number of frames FED (1), not produced
                        // RubberBand can output >1 frames from 1 input frame; advancing by produced
                        // would starve input
                        readPosition += 1.0;
                    }
                    else
                    {
                        // AGGRESSIVE FALLBACK: produce immediate output via naive interpolation
                        const int    srcLen = selectedClip->audio.getNumSamples();
                        const double pitchScaleFB =
                            std::pow(2.0, (double)effectivePitchSemis / 12.0);
                        const double stepFB =
                            (double)pitchScaleFB / (double)juce::jmax(0.0001f, effectiveTime);
                        int         baseFB = juce::jlimit(0, srcLen - 1, (int)readPosition);
                        const int   nextFB = juce::jmin(srcLen - 1, baseFB + 1);
                        const float fracFB = (float)(readPosition - (double)baseFB);
                        const float s0FB = selectedClip->audio.getSample(0, baseFB);
                        const float s1FB = selectedClip->audio.getSample(0, nextFB);
                        float       sampleFB = s0FB + fracFB * (s1FB - s0FB);
                        if (rbFadeSamplesRemaining > 0 && rbFadeSamplesTotal > 0)
                        {
                            const float fade =
                                1.0f - (float)rbFadeSamplesRemaining / (float)rbFadeSamplesTotal;
                            sampleFB *= fade;
                            rbFadeSamplesRemaining--;
                        }
                        if (audioOut)
                            audioOut[i] = sampleFB * currentGate * volumeParam->load();
                        readPosition += stepFB;
                    }
                    if (readPosition >= endSamplePos)
                        readPosition = startSamplePos + (readPosition - endSamplePos);
                }
            } // End of crossfade else block
        }
        else
        {
            if (audioOut)
                audioOut[i] = 0.0f;
        }

        // Mid-block debug of mapped live values (throttled)
        {
            static int midDbg = 0;
            if (i == (numSamples >> 1) && ((midDbg++ % 240) == 0))
            {
                juce::String msg = "[TTS Live Mid] rateHz=" + juce::String(currentRate, 3) +
                                   " gate=" + juce::String(currentGate, 3) +
                                   " speed=" + juce::String(currentSpeed, 3) +
                                   " pitchSemis=" + juce::String(currentPitch, 3);
                juce::Logger::writeToLog(msg);
            }
        }

        // Update live telemetry (like BestPractice line 121-126)
        if ((i & 0x07) == 0) // Every 8 samples instead of 64 for better responsiveness
        {
            setLiveParamValue("rate_live", currentRate);
            setLiveParamValue("gate_live", currentGate);
            setLiveParamValue("trimStart_live", trimStartNorm);
            setLiveParamValue("trimEnd_live", trimEndNorm);
            setLiveParamValue("speed_live", currentSpeed);
            setLiveParamValue("pitch_live", currentPitch);
        }

        // Word gates/triggers (skip if no clip)
        if (hasValidClip)
        {
            // Word gate output
            if (wordGateOut)
            {
                double curTime = readPosition / sr;
                wordGateOut[i] = isWordActiveAtTime(curTime) ? 1.0f : 0.0f;
            }

            // Per-word gates (direct buffer access for AudioProcessorGraph)
            for (int w = 0; w < juce::jmin(16, (int)selectedClip->timings.size()); ++w)
            {
                if (buffer.getNumChannels() > 3 + w)
                {
                    float* wordGate = buffer.getWritePointer(3 + w);
                    wordGate[i] = (w == currentWordIndex && isPlaying) ? 1.0f : 0.0f;
                }
            }

            // Per-word triggers (direct buffer access for AudioProcessorGraph)
            for (int w = 0; w < juce::jmin(16, (int)selectedClip->timings.size()); ++w)
            {
                if (buffer.getNumChannels() > 19 + w)
                {
                    float* wordTrig = buffer.getWritePointer(19 + w);
                    if (wordTriggerPending[w] > 0)
                    {
                        wordTrig[i] = 1.0f;
                        wordTriggerPending[w]--;
                    }
                    else
                    {
                        wordTrig[i] = 0.0f;
                    }
                }
            }
        }
    }

    // EOP gate
    if (eopGateOut && !isPlaying && wasPlayingLastBlock)
    {
        int pulseSamples = (int)std::ceil(0.005 * sr);
        for (int i = 0; i < juce::jmin(numSamples, pulseSamples); ++i)
            eopGateOut[i] = 1.0f;
    }
}

void TTSPerformerModuleProcessor::startSynthesis(const juce::String& text)
{
    DBG("[TTS Performer] startSynthesis called with text: " + text);
    juce::Logger::writeToLog("[TTS Performer] startSynthesis called with text: " + text);

    if (currentStatus == Status::Synthesizing || text.trim().isEmpty())
    {
        DBG("[TTS Performer] startSynthesis early return - already synthesizing or empty text");
        return;
    }

    // --- Phase 3: Reset sequencer state for new synthesis ---
    resetSequencer();

    const juce::ScopedLock lock(textBufferLock);
    if (textFifo.getFreeSpace() > 0)
    {
        int start1, size1, start2, size2;
        textFifo.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 > 0)
        {
            textFifoBuffer[start1] = text;
            textFifo.finishedWrite(1);
            synthesisThread.notify();

            DBG("[TTS Performer] Text queued for synthesis at position " + juce::String(start1));
            juce::Logger::writeToLog(
                "[TTS Performer] Text queued for synthesis at position " + juce::String(start1));
            DBG("[TTS Performer] Synthesis thread notified");
            juce::Logger::writeToLog("[TTS Performer] Synthesis thread notified");
        }
    }
    else
    {
        DBG("[TTS Performer] Text FIFO is full, cannot queue text");
        juce::Logger::writeToLog("[TTS Performer] Text FIFO is full, cannot queue text");
    }
}

// --- Synthesis Thread Implementation ---

TTSPerformerModuleProcessor::SynthesisThread::SynthesisThread(TTSPerformerModuleProcessor& o)
    : juce::Thread("Piper Synthesis Thread"), owner(o)
{
}
TTSPerformerModuleProcessor::SynthesisThread::~SynthesisThread() { stopThread(5000); }

void TTSPerformerModuleProcessor::SynthesisThread::run()
{
    DBG("[TTS Performer] SynthesisThread::run() started");
    juce::Logger::writeToLog("[TTS Performer] SynthesisThread::run() started");

    // --- FIX #1: ROBUST THREAD WAIT LOOP ---
    // This pattern prevents "lost wakeups" by checking the condition
    // before waiting and looping until there's work to do.
    while (!threadShouldExit())
    {
        if (owner.textFifo.getNumReady() == 0)
        {
            wait(-1);
            continue; // Loop back and check condition again after waking up
        }

        DBG("[TTS Performer] Found " + juce::String(owner.textFifo.getNumReady()) +
            " text items in queue");
        juce::Logger::writeToLog(
            "[TTS Performer] Found " + juce::String(owner.textFifo.getNumReady()) +
            " text items in queue");

        owner.currentStatus = Status::Synthesizing;

        // Dequeue text safely
        juce::String textToSynthesize;
        {
            const juce::ScopedLock lock(owner.textBufferLock);
            int                    start1, size1, start2, size2;
            owner.textFifo.prepareToRead(1, start1, size1, start2, size2);
            if (size1 > 0)
                textToSynthesize = owner.textFifoBuffer[start1];
            owner.textFifo.finishedRead(1);
        }

        if (textToSynthesize.isEmpty() || threadShouldExit())
        {
            owner.currentStatus = Status::Idle;
            continue;
        }

        DBG("[TTS Performer] About to start Piper synthesis for text: " + textToSynthesize);
        juce::Logger::writeToLog(
            "[TTS Performer] About to start Piper synthesis for text: " + textToSynthesize);

        try
        {
            // --- FIND EXECUTABLE AND MODELS ---
            auto appDir = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                              .getParentDirectory();
            piperExecutable = appDir.getChildFile("piper.exe");

            DBG("[TTS Performer] Looking for piper.exe at: " + piperExecutable.getFullPathName());
            juce::Logger::writeToLog(
                "[TTS Performer] Looking for piper.exe at: " + piperExecutable.getFullPathName());

            if (!piperExecutable.existsAsFile())
                throw std::runtime_error("piper.exe not found next to application");

            // --- THIS IS THE FIX ---
            // Look for the 'models' directory in the same folder as the executable.
            juce::File modelsDir = appDir.getChildFile("models");

            DBG("[TTS Performer] Looking for models directory at: " + modelsDir.getFullPathName());
            juce::Logger::writeToLog(
                "[TTS Performer] Looking for models directory at: " + modelsDir.getFullPathName());

            if (!modelsDir.isDirectory())
            {
                // Throw an error that shows the path we actually checked
                throw std::runtime_error(
                    "Models directory not found at: " + modelsDir.getFullPathName().toStdString());
            }

            // --- Model selection via registry ---
            juce::File modelFile = owner.resolveSelectedModelFile();
            juce::File configFile = modelFile.withFileExtension(".onnx.json");

            // --- FIX: Update thread state so computeClipKey() uses correct model ---
            // This ensures the clip ID hash includes the correct voice model filename
            this->currentModelFile = modelFile;

            juce::String modelPath = modelFile.getFullPathName();
            juce::String configPath = configFile.getFullPathName();

            DBG("[TTS Performer] Looking for model file: " + modelPath);
            DBG("[TTS Performer] Looking for config file: " + configPath);
            juce::Logger::writeToLog("[TTS Performer] Looking for model file: " + modelPath);
            juce::Logger::writeToLog("[TTS Performer] Looking for config file: " + configPath);

            if (!modelFile.existsAsFile() || !configFile.existsAsFile())
                throw std::runtime_error(
                    "Model .onnx and/or .onnx.json not found in models folder.");

            // Verify model file is valid (not empty/corrupted)
            // ONNX model files should be at least 1 MB (valid models are typically 60-120 MB)
            const juce::int64 MIN_MODEL_SIZE = 1024 * 1024; // 1 MB minimum
            const juce::int64 MIN_CONFIG_SIZE = 1000;       // 1 KB minimum for JSON

            juce::int64 modelSize = modelFile.getSize();
            juce::int64 configSize = configFile.getSize();

            juce::Logger::writeToLog(
                "[TTS Performer] Model file size: " + juce::String(modelSize) + " bytes");
            juce::Logger::writeToLog(
                "[TTS Performer] Config file size: " + juce::String(configSize) + " bytes");

            if (modelSize == 0)
            {
                juce::Logger::writeToLog(
                    "[TTS Performer] ERROR: Model file is empty: " + modelPath);
                throw std::runtime_error(
                    "Model file is empty or corrupted: " + modelFile.getFileName().toStdString() +
                    " (0 bytes). Please re-download this voice.");
            }

            if (modelSize < MIN_MODEL_SIZE)
            {
                juce::Logger::writeToLog(
                    "[TTS Performer] ERROR: Model file is too small (corrupted): " + modelPath +
                    " (" + juce::String(modelSize) + " bytes, expected at least " +
                    juce::String(MIN_MODEL_SIZE) + " bytes)");
                throw std::runtime_error(
                    "Model file is corrupted or incomplete: " +
                    modelFile.getFileName().toStdString() + " (only " + std::to_string(modelSize) +
                    " bytes, expected at least " + std::to_string(MIN_MODEL_SIZE) +
                    " bytes). Please re-download this voice from the download dialog.");
            }

            if (configSize == 0)
            {
                juce::Logger::writeToLog(
                    "[TTS Performer] ERROR: Config file is empty: " + configPath);
                throw std::runtime_error(
                    "Config file is empty or corrupted: " + configFile.getFileName().toStdString() +
                    ". Please re-download this voice.");
            }

            if (configSize < MIN_CONFIG_SIZE)
            {
                juce::Logger::writeToLog(
                    "[TTS Performer] ERROR: Config file is too small (corrupted): " + configPath +
                    " (" + juce::String(configSize) + " bytes)");
                throw std::runtime_error(
                    "Config file is corrupted or incomplete: " +
                    configFile.getFileName().toStdString() + ". Please re-download this voice.");
            }

            // --- Phase 4: Check Cache and Update Usage Time ---
            juce::String cacheKey = getCacheKey(modelPath);
            bool         wasCached = isVoiceCached(modelPath);

            if (wasCached)
            {
                DBG("[TTS Performer] Voice found in cache: " + cacheKey + " (Instant access!)");
                // Update last used time
                {
                    const juce::ScopedLock lock(cacheLock);
                    auto                   it = voiceCache.find(cacheKey);
                    if (it != voiceCache.end())
                    {
                        it->second.lastUsed = std::chrono::steady_clock::now();
                    }
                }
            }
            else
            {
                DBG("[TTS Performer] Voice not in cache: " + cacheKey + " (Loading from disk...)");

                // Update cache size limit from parameter before adding
                updateMaxCachedVoicesFromParameter();
                addVoiceToCache(modelPath, configPath);
            }

            // --- CREATE TEMP FILES FOR OUTPUT ---
            juce::File tempWavFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                         .getNonexistentChildFile("piper_out", ".wav");
            juce::File tempJsonFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                          .getNonexistentChildFile("piper_timing", ".json");

            // --- FIX #2: ROBUST ChildProcess with input file ---
            // Create temporary input file for text
            juce::File tempInputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                           .getNonexistentChildFile("piper_input", ".txt");
            tempInputFile.replaceWithText(textToSynthesize);

            // Build command using Windows cmd to pipe input to piper
            juce::ChildProcess piperProcess;
            // Build command with working directory change to ensure piper can find its dependencies
            auto workingDirectory = piperExecutable.getParentDirectory();

            // Create temp file to capture stderr/stdout for debugging crashes
            juce::File tempErrorFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                           .getNonexistentChildFile("piper_error", ".txt");

            // Build command with stdout/stderr redirected to error file for debugging
            // This helps us see what piper.exe is saying before it crashes
            // Using parentheses to group the pipe operation before redirecting output
            juce::String command =
                "cmd /c \"cd /d \"" + workingDirectory.getFullPathName() + "\" && (type \"" +
                tempInputFile.getFullPathName() + "\" | \"" + piperExecutable.getFullPathName() +
                "\" --model \"" + modelFile.getFullPathName() + "\"" + " --espeak_data \"" +
                workingDirectory.getChildFile("espeak-ng-data").getFullPathName() + "\"" +
                " --output_file \"" + tempWavFile.getFullPathName() + "\") > \"" +
                tempErrorFile.getFullPathName() + "\" 2>&1\"";

            DBG("[TTS Performer] Starting Piper process with command: " + command);
            juce::Logger::writeToLog(
                "[TTS Performer] Starting Piper process with command: " + command);
            juce::Logger::writeToLog(
                "[TTS Performer] Error output will be logged to: " +
                tempErrorFile.getFullPathName());

            if (piperProcess.start(command))
            {
                DBG("[TTS Performer] Piper process started successfully, waiting for "
                    "completion...");
                juce::Logger::writeToLog(
                    "[TTS Performer] Piper process started successfully, waiting for "
                    "completion...");

                // Wait for the process to finish
                if (!piperProcess.waitForProcessToFinish(30000)) // 30s timeout
                    throw std::runtime_error("Piper process timed out.");

                // --- DIAGNOSTIC: Check if WAV file was created ---
                if (tempWavFile.existsAsFile())
                {
                    juce::Logger::writeToLog("--- PIPER WAV OUTPUT SUCCESS ---");
                    juce::Logger::writeToLog("WAV file created: " + tempWavFile.getFullPathName());
                    juce::Logger::writeToLog(
                        "File size: " + juce::String(tempWavFile.getSize()) + " bytes");
                }
                else
                {
                    juce::Logger::writeToLog("--- PIPER WAV OUTPUT FAILED: FILE NOT CREATED ---");
                }
                // --- END OF DIAGNOSTIC BLOCK ---

                int exitCode = piperProcess.getExitCode();
                DBG("[TTS Performer] Piper process finished with exit code: " +
                    juce::String(exitCode));
                juce::Logger::writeToLog(
                    "[TTS Performer] Piper process finished with exit code: " +
                    juce::String(exitCode));

                // Read process output (stdout/stderr) from temp file
                juce::String outputText;
                if (tempErrorFile.existsAsFile() && tempErrorFile.getSize() > 0)
                {
                    outputText = tempErrorFile.loadFileAsString();
                    if (outputText.isNotEmpty())
                    {
                        juce::Logger::writeToLog("[TTS Performer] Piper process output:");
                        juce::StringArray lines;
                        lines.addTokens(outputText, "\r\n", "\"");
                        for (const auto& line : lines)
                        {
                            if (line.trim().isNotEmpty())
                                juce::Logger::writeToLog("  > " + line);
                        }
                    }
                    // Clean up error file after reading
                    tempErrorFile.deleteFile();
                }

                if (exitCode != 0)
                {
                    // Enhanced error message with exit code interpretation
                    juce::String errorMsg =
                        "Piper process failed with exit code: " + juce::String(exitCode);
                    if (exitCode == -1073740791) // 0xC0000409 STATUS_STACK_BUFFER_OVERRUN
                    {
                        errorMsg += " (STATUS_STACK_BUFFER_OVERRUN - piper.exe crashed). ";
                        errorMsg +=
                            "Possible causes: corrupted model file, missing DLL dependencies (ONNX "
                            "Runtime), or model incompatibility. ";
                        errorMsg += "Model: " + modelFile.getFileName();
                        errorMsg += " (Size: " + juce::String(modelFile.getSize()) + " bytes)";

                        // Suggest checking model file integrity
                        juce::Logger::writeToLog(
                            "[TTS Performer] WARNING: Model file may be corrupted or "
                            "incompatible.");
                        juce::Logger::writeToLog(
                            "[TTS Performer] Try re-downloading this voice model from the download "
                            "dialog.");
                    }
                    else if (exitCode < 0)
                    {
                        errorMsg += " (Process crashed). ";
                    }

                    if (outputText.isNotEmpty())
                    {
                        juce::String shortOutput =
                            outputText.substring(0, 500).replace("\r", " ").replace("\n", " ");
                        errorMsg += " Error output: " + shortOutput;
                    }
                    else
                    {
                        errorMsg +=
                            " (No error output captured - process may have crashed before writing "
                            "to stderr).";
                    }

                    juce::Logger::writeToLog("[TTS Performer] ERROR: " + errorMsg);
                    throw std::runtime_error(errorMsg.toStdString());
                }

                // Clean up input file
                tempInputFile.deleteFile();

                // --- LOAD AND RESAMPLE GENERATED AUDIO (THE CRITICAL FIX) ---
                if (!tempWavFile.existsAsFile())
                    throw std::runtime_error("Piper did not create an output WAV file.");

                juce::AudioFormatManager formatManager;
                formatManager.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader(
                    formatManager.createReaderFor(tempWavFile));

                if (reader == nullptr)
                    throw std::runtime_error("Could not read generated WAV file.");

                DBG("[TTS Performer] Original audio sample rate: " +
                    juce::String(reader->sampleRate) + " Hz");
                juce::Logger::writeToLog(
                    "[TTS Performer] Original audio sample rate: " +
                    juce::String(reader->sampleRate) + " Hz");
                DBG("[TTS Performer] Target sample rate: " + juce::String(owner.getSampleRate()) +
                    " Hz");
                juce::Logger::writeToLog(
                    "[TTS Performer] Target sample rate: " + juce::String(owner.getSampleRate()) +
                    " Hz");

                // Load the entire file into a temporary buffer at its original sample rate
                const int                originalNumSamples = (int)reader->lengthInSamples;
                juce::AudioBuffer<float> originalAudio(1, originalNumSamples);
                reader->read(&originalAudio, 0, originalNumSamples, 0, true, false);

                // CRITICAL FIX: Ensure valid sample rate before resampling
                double targetSR = owner.getSampleRate();
                if (targetSR <= 0.0)
                {
                    DBG("[TTS Performer] ERROR: Invalid target sample rate (" +
                        juce::String(targetSR) + "), using 48000 Hz as fallback");
                    targetSR = 48000.0;
                }

                // Calculate resampling ratio
                double resampleRatio = reader->sampleRate / targetSR;
                int    resampledNumSamples = (int)(originalNumSamples / resampleRatio);

                DBG("[TTS Performer] Resampling ratio: " + juce::String(resampleRatio, 4));
                DBG("[TTS Performer] Original samples: " + juce::String(originalNumSamples));
                DBG("[TTS Performer] Resampled samples: " + juce::String(resampledNumSamples));
                juce::Logger::writeToLog(
                    "[TTS Performer] Resampling from " + juce::String(originalNumSamples) + " to " +
                    juce::String(resampledNumSamples) +
                    " samples (target SR: " + juce::String(targetSR) + ")");

                // Prepare a resampling source
                juce::MemoryAudioSource     tempSource(originalAudio, false);
                juce::ResamplingAudioSource resampledSource(&tempSource, false, 1);
                resampledSource.setResamplingRatio(resampleRatio);

                // Prepare a buffer at the destination sample rate
                juce::AudioBuffer<float> finalAudio(1, resampledNumSamples);
                resampledSource.prepareToPlay(512, targetSR);

                // Perform the resampling
                juce::AudioSourceChannelInfo info(finalAudio);
                resampledSource.getNextAudioBlock(info);

                // --- Create/Store Clip and select it ---
                {
                    auto clip = std::make_shared<TTSClip>();
                    clip->clipId = owner.computeClipKey(textToSynthesize);
                    clip->name = textToSynthesize.substring(0, 48);
                    clip->text = textToSynthesize;
                    clip->modelKey = owner.synthesisThread.currentModelFile.getFileName();
                    clip->audio.makeCopyOf(finalAudio);
                    clip->sampleRate = targetSR;
                    clip->durationSeconds = (double)clip->audio.getNumSamples() / clip->sampleRate;
                    // Persist to disk (wav saved now, timing after parse below)
                    owner.persistClipToDisk(textToSynthesize, modelFile, finalAudio, {});
                    {
                        const juce::ScopedLock c(owner.clipCacheLock);
                        owner.addClipToCache(clip);
                        owner.selectedClip = clip;
                    }
                    // keep legacy baked buffer path for waveform until UI uses selectedClip
                    // exclusively
                    const juce::ScopedLock lock(owner.audioBufferLock);
                    owner.bakedAudioBuffer.makeCopyOf(finalAudio);
                    DBG("[TTS Performer] Audio copied to bakedAudioBuffer: " +
                        juce::String(owner.bakedAudioBuffer.getNumSamples()) + " samples");
                    DBG("[TTS Performer] selectedClip audio: " +
                        juce::String(clip->audio.getNumSamples()) + " samples");
                }
                owner.readPosition = 0.0;
                owner.isPlaying = false; // Don't auto-play, wait for trigger
                DBG("[TTS Performer] Clip ready: " +
                    juce::String(
                        owner.selectedClip ? owner.selectedClip->audio.getNumSamples() : 0) +
                    " samples");

                DBG("[TTS Performer] Audio resampling complete, ready for playback");
                juce::Logger::writeToLog(
                    "[TTS Performer] Audio resampling complete, ready for playback");

                // --- PARSE JSON TIMING DATA (Phase 2.3) ---
                bool hasTimingData = false;
                if (tempJsonFile.existsAsFile())
                {
                    DBG("[TTS Performer] JSON timing file generated: " +
                        tempJsonFile.getFullPathName());
                    juce::Logger::writeToLog(
                        "[TTS Performer] JSON timing file generated: " +
                        tempJsonFile.getFullPathName());

                    try
                    {
                        // Read and parse the JSON file
                        juce::String jsonContent = tempJsonFile.loadFileAsString();
                        auto         jsonData = nlohmann::json::parse(jsonContent.toStdString());

                        DBG("[TTS Performer] JSON parsing successful, extracting timing data...");
                        juce::Logger::writeToLog(
                            "[TTS Performer] JSON parsing successful, extracting timing data...");

                        // Parse the timing data
                        std::vector<WordTiming> newTimings;

                        // Piper JSON structure typically contains:
                        // - "words": array of word objects with timing
                        // - Each word has "text", "start_time", "end_time", and "phonemes"
                        if (jsonData.contains("words") && jsonData["words"].is_array())
                        {
                            for (const auto& wordData : jsonData["words"])
                            {
                                if (wordData.contains("text") && wordData.contains("start_time") &&
                                    wordData.contains("end_time"))
                                {
                                    juce::String wordText = wordData["text"].get<std::string>();
                                    double       startTime = wordData["start_time"].get<double>();
                                    double       endTime = wordData["end_time"].get<double>();

                                    WordTiming wordTiming(wordText, startTime, endTime);

                                    // Parse phonemes if available
                                    if (wordData.contains("phonemes") &&
                                        wordData["phonemes"].is_array())
                                    {
                                        for (const auto& phonemeData : wordData["phonemes"])
                                        {
                                            if (phonemeData.contains("phoneme") &&
                                                phonemeData.contains("start_time") &&
                                                phonemeData.contains("end_time"))
                                            {
                                                juce::String phoneme =
                                                    phonemeData["phoneme"].get<std::string>();
                                                double phonemeStart =
                                                    phonemeData["start_time"].get<double>();
                                                double phonemeEnd =
                                                    phonemeData["end_time"].get<double>();

                                                wordTiming.phonemes.emplace_back(
                                                    phoneme, phonemeStart, phonemeEnd);
                                            }
                                        }
                                    }

                                    newTimings.push_back(wordTiming);

                                    DBG("[TTS Performer] Parsed word: \"" + wordText + "\" (" +
                                        juce::String(startTime, 3) + "s - " +
                                        juce::String(endTime, 3) + "s, " +
                                        juce::String(wordTiming.phonemes.size()) + " phonemes)");
                                }
                            }
                        }

                        // Store the parsed timing data (thread-safe) and attach to the selected
                        // clip
                        {
                            const juce::ScopedLock lock(owner.audioBufferLock);
                            owner.lastSynthesisTimings = newTimings;
                        }
                        if (owner.selectedClip)
                        {
                            const juce::ScopedLock c(owner.clipCacheLock);
                            owner.selectedClip->timings = newTimings;
                        }
                        // Persist timing JSON and XML (uses lastSynthesisTimings which was just
                        // updated)
                        owner.persistClipToDisk(textToSynthesize, modelFile, {}, jsonContent);
                        hasTimingData = true;

                        DBG("[TTS Performer] Timing data parsed successfully: " +
                            juce::String(newTimings.size()) + " words");
                        juce::Logger::writeToLog(
                            "[TTS Performer] Timing data parsed successfully: " +
                            juce::String(newTimings.size()) + " words");

                        // Log summary of timing data
                        for (size_t i = 0; i < newTimings.size(); ++i)
                        {
                            const auto& word = newTimings[i];
                            DBG("[TTS Performer] Word " + juce::String(i + 1) + ": \"" + word.word +
                                "\" (" + juce::String(word.startTimeSeconds, 3) + "s - " +
                                juce::String(word.endTimeSeconds, 3) + "s)");
                        }
                    }
                    catch (const std::exception& e)
                    {
                        DBG("[TTS Performer] ERROR: Failed to parse JSON timing data: " +
                            juce::String(e.what()));
                        juce::Logger::writeToLog(
                            "[TTS Performer] ERROR: Failed to parse JSON timing data: " +
                            juce::String(e.what()));

                        // Clear timing data on error
                        {
                            const juce::ScopedLock lock(owner.audioBufferLock);
                            owner.lastSynthesisTimings.clear();
                        }
                    }
                }
                else
                {
                    DBG("[TTS Performer] WARNING: JSON timing file was not created - using onset "
                        "detection to find words.");
                    juce::Logger::writeToLog(
                        "[TTS Performer] WARNING: JSON timing file was not created - using onset "
                        "detection to find words.");

                    // Call the new onset detection function to get precise timings from the audio
                    std::vector<WordTiming> detectedTimings =
                        owner.detectWordsFromAudio(finalAudio, targetSR);

                    if (!detectedTimings.empty())
                    {
                        // Store the new precise timings
                        {
                            const juce::ScopedLock lock(owner.audioBufferLock);
                            owner.lastSynthesisTimings = detectedTimings;
                        }
                        if (owner.selectedClip)
                        {
                            const juce::ScopedLock c(owner.clipCacheLock);
                            owner.selectedClip->timings = detectedTimings;
                        }
                        // Persist the detected timings to an XML file for this clip
                        owner.persistClipToDisk(textToSynthesize, modelFile, {}, {});
                        hasTimingData = true;
                        DBG("[TTS Performer] Onset detection found " +
                            juce::String(detectedTimings.size()) + " words");
                        juce::Logger::writeToLog(
                            "[TTS Performer] Onset detection found " +
                            juce::String(detectedTimings.size()) + " words");
                    }
                }
            }
            else
            {
                throw std::runtime_error("Failed to start piper.exe process.");
            }
            tempWavFile.deleteFile();  // Clean up WAV file
            tempJsonFile.deleteFile(); // Clean up JSON file
        }
        catch (const std::exception& e)
        {
            const juce::ScopedLock lock(owner.messageLock);
            owner.errorMessage = e.what();
            owner.currentStatus = Status::Error;
        }
        owner.currentStatus = Status::Idle;
    }
}

void TTSPerformerModuleProcessor::cancelSynthesis()
{
    synthesisThread.stopThread(5000);
    synthesisThread.startThread();
    currentStatus = Status::Idle;
}

juce::File TTSPerformerModuleProcessor::getClipsRootDir() const
{
    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto ttsDir = exeDir.getChildFile("TTSPERFORMER");

    if (!ttsDir.isDirectory())
        ttsDir.createDirectory();

    DBG("[TTS Performer] Clips root: " + ttsDir.getFullPathName());
    return ttsDir;
}

juce::String TTSPerformerModuleProcessor::sanitizeForDir(const juce::String& text) const
{
    juce::String s = text;
    const char*  bad[] = {"\\", "/", ":", "*", "?", "\"", "<", ">", "|"};
    for (auto b : bad)
        s = s.replace(b, "_");
    if (s.length() > 64)
        s = s.substring(0, 64);
    return s.trim();
}

void TTSPerformerModuleProcessor::persistClipToDisk(
    const juce::String&             text,
    const juce::File&               modelFile,
    const juce::AudioBuffer<float>& audioBuffer,
    const juce::String&             jsonContent)
{
    // --- FIX: Use hash-based ID as directory name for consistency ---
    // Compute the same hash-based ID that the clip object will have
    juce::String model = modelFile.getFileName();
    auto         key = text + "|" + model;
    juce::String clipId =
        juce::String(juce::String::toHexString(juce::DefaultHashFunctions::generateHash(key, 0)));

    // Use the clip ID for the directory name
    juce::File dir = getClipsRootDir().getChildFile(clipId);
    if (!dir.exists())
        dir.createDirectory();

    // Save metadata file containing original text and model info
    juce::XmlElement meta("ClipInfo");
    meta.setAttribute("name", text.substring(0, 48));
    meta.setAttribute("text", text);
    meta.setAttribute("model", modelFile.getFileName());
    dir.getChildFile("info.xml").replaceWithText(meta.toString());

    // Build unique file stem including voice + params
    juce::String base = modelFile.getFileNameWithoutExtension();
    auto*        sp = apvts.getRawParameterValue("speed");
    auto*        pt = apvts.getRawParameterValue("pitch");
    float        spv = sp ? sp->load() : 0.0f;
    float        ptv = pt ? pt->load() : 0.0f;
    juce::String speedStr = juce::String(spv, 2);
    juce::String pitchStr = juce::String(ptv, 2);
    juce::String stem = base + "_spd" + speedStr.replaceCharacter('.', '_') + "_pit" +
                        pitchStr.replaceCharacter('.', '_');

    // Save model name tag (keeping for backward compatibility)
    dir.getChildFile("model.txt").replaceWithText(modelFile.getFileName());

    // Save JSON timing if provided
    if (jsonContent.isNotEmpty())
        dir.getChildFile(stem + ".json").replaceWithText(jsonContent);

    // Save XML timing if we have parsed timings (matching WAV filename)
    {
        const juce::ScopedLock lock(audioBufferLock);
        if (!lastSynthesisTimings.empty())
        {
            DBG("[TTS Performer] Saving XML timing with " +
                juce::String(lastSynthesisTimings.size()) + " words");
            juce::XmlElement root("timings");
            for (const auto& word : lastSynthesisTimings)
            {
                auto* wordEl = root.createNewChildElement("word");
                wordEl->setAttribute("text", word.word);
                wordEl->setAttribute("start", word.startTimeSeconds);
                wordEl->setAttribute("end", word.endTimeSeconds);
                for (const auto& ph : word.phonemes)
                {
                    auto* phEl = wordEl->createNewChildElement("phoneme");
                    phEl->setAttribute("text", ph.phoneme);
                    phEl->setAttribute("start", ph.startTimeSeconds);
                    phEl->setAttribute("end", ph.endTimeSeconds);
                }
            }
            auto xmlFile = dir.getChildFile(stem + ".xml");
            bool saved = xmlFile.replaceWithText(root.toString());
            DBG("[TTS Performer] XML timing saved: " + xmlFile.getFullPathName() +
                " (success: " + juce::String(saved ? "YES" : "NO") + ")");
        }
        else
        {
            DBG("[TTS Performer] WARNING: No timing data available for XML export");
        }
    }

    // Save WAV if provided
    if (audioBuffer.getNumSamples() > 0)
    {
        juce::WavAudioFormat                    wav;
        juce::String                            fname = stem + ".wav";
        std::unique_ptr<juce::FileOutputStream> out(dir.getChildFile(fname).createOutputStream());
        if (out && out->openedOk())
        {
            std::unique_ptr<juce::AudioFormatWriter> writer(
                wav.createWriterFor(out.release(), getSampleRate(), 1, 16, {}, 0));
            if (writer)
            {
                writer->writeFromAudioSampleBuffer(audioBuffer, 0, audioBuffer.getNumSamples());
                DBG("[TTS Performer] WAV saved: " + dir.getChildFile(fname).getFullPathName());
            }
        }
        else
        {
            DBG("[TTS Performer] ERROR: Failed to create WAV output stream");
        }
    }
}

// --- Clip cache helpers ---
juce::String TTSPerformerModuleProcessor::computeClipKey(const juce::String& text) const
{
    // Simple key: hash(text + model)
    juce::String model = synthesisThread.currentModelFile.getFileName();
    auto         key = text + "|" + model;
    return juce::String(
        juce::String::toHexString(juce::DefaultHashFunctions::generateHash(key, 0)));
}

void TTSPerformerModuleProcessor::addClipToCache(const std::shared_ptr<TTSClip>& clip)
{
    if (!clip)
        return;
    // LRU eviction
    if ((int)clipCache.size() >= clipCacheMax)
    {
        juce::String oldestKey;
        auto         oldestTime = std::chrono::steady_clock::now();
        for (auto& kv : clipCache)
        {
            if (kv.second && kv.second->lastUsed <= oldestTime)
            {
                oldestTime = kv.second->lastUsed;
                oldestKey = kv.first;
            }
        }
        if (oldestKey.isNotEmpty())
            clipCache.erase(oldestKey);
    }
    clipCache[clip->clipId] = clip;
}

std::shared_ptr<TTSPerformerModuleProcessor::TTSClip> TTSPerformerModuleProcessor::findClipInCache(
    const juce::String& key) const
{
    auto it = clipCache.find(key);
    if (it != clipCache.end())
        return it->second;
    return nullptr;
}

void TTSPerformerModuleProcessor::selectClipByKey(const juce::String& key)
{
    const juce::ScopedLock c(clipCacheLock);
    auto                   clip = findClipInCache(key);
    if (clip)
    {
        selectedClip = clip;
        readPosition = 0.0;
        isPlaying = false;
        {
            const juce::ScopedLock lock(audioBufferLock);
            lastSynthesisTimings = clip->timings;
            bakedAudioBuffer.makeCopyOf(clip->audio);
        }
        // Reset trim range parameters to full clip duration
        auto* trimStartParam =
            dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("trimStart"));
        auto* trimEndParam =
            dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("trimEnd"));
        if (trimStartParam)
            trimStartParam->setValueNotifyingHost(0.0f);
        if (trimEndParam)
            trimEndParam->setValueNotifyingHost(1.0f);
    }
}

void TTSPerformerModuleProcessor::selectClipByIndex(int index)
{
    const juce::ScopedLock c(clipCacheLock);
    if (clipCache.empty())
        return;
    index = juce::jlimit(0, (int)clipCache.size() - 1, index);
    int i = 0;
    for (auto& kv : clipCache)
    {
        if (i++ == index)
        {
            selectClipByKey(kv.first);
            break;
        }
    }
}

void TTSPerformerModuleProcessor::loadClipsFromDisk()
{
    juce::File              root = getClipsRootDir();
    juce::Array<juce::File> dirs;
    root.findChildFiles(dirs, juce::File::findDirectories, false);

    DBG("[TTS Performer] Scanning TTSPERFORMER: found " + juce::String(dirs.size()) +
        " clip folders");

    // Clear existing cache before reload
    {
        const juce::ScopedLock c(clipCacheLock);
        clipCache.clear();
    }

    for (auto dir : dirs)
    {
        // --- FIX: The directory name IS the unique clip ID now ---
        juce::String clipId = dir.getFileName();

        // Load metadata from info.xml
        juce::File   metaFile = dir.getChildFile("info.xml");
        juce::String clipName = clipId; // Fallback to ID if no metadata
        juce::String clipText = clipId;
        juce::String clipModel = "";

        if (metaFile.existsAsFile())
        {
            std::unique_ptr<juce::XmlElement> metaXml(juce::XmlDocument::parse(metaFile));
            if (metaXml && metaXml->hasTagName("ClipInfo"))
            {
                clipName = metaXml->getStringAttribute("name", clipId);
                clipText = metaXml->getStringAttribute("text", clipId);
                clipModel = metaXml->getStringAttribute("model", "");
            }
        }

        // --- THIS IS THE FIX ---
        // 1. Find all .wav files in the directory.
        juce::Array<juce::File> wavs;
        dir.findChildFiles(wavs, juce::File::findFiles, false, "*.wav");

        // 2. If no .wav files are found, we cannot load this clip, so skip to the next directory.
        if (wavs.isEmpty())
            continue;

        // 3. Find the newest .wav file in the directory.
        juce::File wavToLoad = wavs.getFirst();
        for (const auto& f : wavs)
        {
            if (f.getLastModificationTime() > wavToLoad.getLastModificationTime())
            {
                wavToLoad = f;
            }
        }
        // --- END OF FIX ---

        // Look for XML/JSON timing files matching WAV stem
        juce::String wavStem = wavToLoad.getFileNameWithoutExtension();
        auto         timingXml = dir.getChildFile(wavStem + ".xml");
        auto         timingJson = dir.getChildFile(wavStem + ".json");
        // Fallback to old naming scheme
        if (!timingXml.existsAsFile())
            timingXml = dir.getChildFile("timing.xml");
        if (!timingJson.existsAsFile())
            timingJson = dir.getChildFile("timing.json");

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(wavToLoad));
        if (!r)
            continue;
        juce::AudioBuffer<float> buf(1, (int)r->lengthInSamples);
        r->read(&buf, 0, buf.getNumSamples(), 0, true, false);
        std::vector<WordTiming> timings;
        // Prefer XML timing if available
        if (timingXml.existsAsFile())
        {
            std::unique_ptr<juce::XmlElement> root(juce::XmlDocument::parse(timingXml));
            if (root && root->hasTagName("timings"))
            {
                for (auto* wordEl : root->getChildWithTagNameIterator("word"))
                {
                    juce::String word = wordEl->getStringAttribute("text");
                    double       start = wordEl->getDoubleAttribute("start");
                    double       end = wordEl->getDoubleAttribute("end");
                    WordTiming   wt(word, start, end);
                    for (auto* phEl : wordEl->getChildWithTagNameIterator("phoneme"))
                    {
                        juce::String ph = phEl->getStringAttribute("text");
                        double       phStart = phEl->getDoubleAttribute("start");
                        double       phEnd = phEl->getDoubleAttribute("end");
                        wt.phonemes.emplace_back(ph, phStart, phEnd);
                    }
                    timings.push_back(wt);
                }
            }
        }
        // Fallback to JSON timing if XML not found
        else if (timingJson.existsAsFile())
        {
            try
            {
                auto jsonData = nlohmann::json::parse(timingJson.loadFileAsString().toStdString());
                if (jsonData.contains("words") && jsonData["words"].is_array())
                {
                    for (const auto& wordData : jsonData["words"])
                    {
                        if (wordData.contains("text") && wordData.contains("start_time") &&
                            wordData.contains("end_time"))
                        {
                            WordTiming wt(
                                wordData["text"].get<std::string>(),
                                wordData["start_time"].get<double>(),
                                wordData["end_time"].get<double>());
                            if (wordData.contains("phonemes") && wordData["phonemes"].is_array())
                            {
                                for (const auto& p : wordData["phonemes"])
                                {
                                    if (p.contains("phoneme") && p.contains("start_time") &&
                                        p.contains("end_time"))
                                        wt.phonemes.emplace_back(
                                            p["phoneme"].get<std::string>(),
                                            p["start_time"].get<double>(),
                                            p["end_time"].get<double>());
                                }
                            }
                            timings.push_back(wt);
                        }
                    }
                }
            }
            catch (...)
            {
            }
        }
        auto clip = std::make_shared<TTSClip>();
        clip->clipId = clipId;      // Use the hash-based ID from directory name
        clip->name = clipName;      // Use the name from metadata
        clip->text = clipText;      // Use the original text from metadata
        clip->modelKey = clipModel; // Use the model from metadata
        clip->audio.makeCopyOf(buf);
        clip->sampleRate = getSampleRate();
        clip->durationSeconds = (double)buf.getNumSamples() / juce::jmax(1.0, getSampleRate());
        clip->timings = std::move(timings);
        const juce::ScopedLock c(clipCacheLock);
        addClipToCache(clip);
        DBG("[TTS Performer] Loaded clip: " + clipName + " (ID: " + clipId +
            ") from: " + dir.getFullPathName());
    }
    // Set flag AFTER successful load
    clipsLoadedFromDisk = true;
}

void TTSPerformerModuleProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto                              state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    // Persist selected clip (trim parameters are already in APVTS)
    if (selectedClip)
        xml->setAttribute("selectedClipId", selectedClip->clipId);
    copyXmlToBinary(*xml, destData);
}

void TTSPerformerModuleProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName(apvts.state.getType()))
        {
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
            // Restore selected clip (trim parameters restored via APVTS)
            selectedClipId = xmlState->getStringAttribute("selectedClipId");
            if (selectedClipId.isNotEmpty())
                selectClipByKey(selectedClipId);
        }
}

juce::ValueTree TTSPerformerModuleProcessor::getExtraStateTree() const
{
    juce::ValueTree vt("TTSPerformerState");
    // Only save the unique ID of the selected clip.
    // Trim parameters are handled automatically by the main APVTS save system.
    if (selectedClip)
    {
        vt.setProperty("selectedClipId", selectedClip->clipId, nullptr);
    }
    return vt;
}

void TTSPerformerModuleProcessor::setExtraStateTree(const juce::ValueTree& vt)
{
    if (vt.hasType("TTSPerformerState"))
    {
        // Only load the unique ID of the clip to select.
        // Trim parameters are restored automatically by the main APVTS system.
        juce::String clipIdToSelect = vt.getProperty("selectedClipId", "").toString();
        if (clipIdToSelect.isNotEmpty())
        {
            selectClipByKey(clipIdToSelect);
        }
    }
}

#if defined(PRESET_CREATOR_UI)
void TTSPerformerModuleProcessor::drawClipsPanel(float itemWidth)
{
    ImGui::Text("Clips (%d cached)", getNumCachedClips());

    // --- FIX: Set fixed width to match waveform canvas (600.0f) ---
    const float dropdownWidth = 600.0f;
    ImGui::PushItemWidth(dropdownWidth);

    if (!clipsLoadedFromDisk)
        loadClipsFromDisk();
    juce::String currentName = selectedClip ? selectedClip->name : juce::String("(none)");
    if (ImGui::BeginCombo("##clipsCombo", currentName.toRawUTF8()))
    {
        const juce::ScopedLock lock(clipCacheLock);
        for (auto& kv : clipCache)
        {
            const auto& clip = kv.second;
            if (!clip)
                continue;

            // --- FIX: Include modelKey to show which voice was used ---
            juce::String label = clip->name + " (" + juce::String(clip->durationSeconds, 1) +
                                 "s, " + juce::String(clip->timings.size()) + " words) [" +
                                 clip->modelKey + "]";

            // Append unique hidden ID to prevent ImGui ID conflicts
            // The "##" tells ImGui to use the following text for the ID but not display it
            label += "##" + clip->clipId;

            bool sel = (selectedClip && selectedClip->clipId == clip->clipId);
            if (ImGui::Selectable(label.toRawUTF8(), sel))
            {
                selectClipByKey(clip->clipId);
                // Reset trim parameters on clip switch (handled in selectClipByKey)
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    // Transport removed: use spacebar (PresetCreator) or audio engine auto-play (Collider)
    if (ImGui::Button("Rename##clip", ImVec2(itemWidth * 0.2f, 0)))
    {
        if (selectedClip)
        {
            // Show rename popup
            strncpy_s(
                renameBuffer, sizeof(renameBuffer), selectedClip->name.toRawUTF8(), _TRUNCATE);
            showRenamePopup = true;
            ImGui::OpenPopup("Rename Clip##renamepopup");
        }
    }

    // Rename popup modal
    if (ImGui::BeginPopupModal(
            "Rename Clip##renamepopup", &showRenamePopup, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Enter new name:");
        ImGui::InputText("##renameinput", renameBuffer, IM_ARRAYSIZE(renameBuffer));
        if (ImGui::Button("OK##renameok", ImVec2(120, 0)))
        {
            if (selectedClip && renameBuffer[0] != '\0')
            {
                renameSelectedClipOnDisk(juce::String(renameBuffer));
            }
            showRenamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##renamecancel", ImVec2(120, 0)))
        {
            showRenamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete##clip", ImVec2(itemWidth * 0.2f, 0)))
    {
        if (selectedClip)
        {
            showDeleteConfirm = true;
            ImGui::OpenPopup("Confirm Delete##deletepopup");
        }
    }

    // Delete confirmation modal
    if (ImGui::BeginPopupModal(
            "Confirm Delete##deletepopup", &showDeleteConfirm, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete clip \"%s\"?", selectedClip ? selectedClip->name.toRawUTF8() : "");
        ImGui::Text("This will remove it from disk permanently.");
        if (ImGui::Button("Yes##deleteyes", ImVec2(120, 0)))
        {
            deleteSelectedClipFromDisk();
            showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No##deleteno", ImVec2(120, 0)))
        {
            showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Reload##clip", ImVec2(itemWidth * 0.2f, 0)))
    {
        clipsLoadedFromDisk = false;
        loadClipsFromDisk();
    }
}
#endif

bool TTSPerformerModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    // Bus 0: Global Mods (Rate, Gate, Trigger, Reset, Randomize)
    if (paramId == paramIdRateMod)
    {
        outBusIndex = 0;
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == paramIdGateMod)
    {
        outBusIndex = 0;
        outChannelIndexInBus = 1;
        return true;
    }
    if (paramId == paramIdTriggerMod)
    {
        outBusIndex = 0;
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdResetMod)
    {
        outBusIndex = 0;
        outChannelIndexInBus = 3;
        return true;
    }
    if (paramId == paramIdRandomizeMod)
    {
        outBusIndex = 0;
        outChannelIndexInBus = 4;
        return true;
    } // <-- NEW

    // Bus 1: Trim Mods (Trim Start, Trim End)
    if (paramId == paramIdTrimStartMod)
    {
        outBusIndex = 1;
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == paramIdTrimEndMod)
    {
        outBusIndex = 1;
        outChannelIndexInBus = 1;
        return true;
    }

    // Bus 2: Playback Mods (Speed, Pitch)
    if (paramId == paramIdSpeedMod)
    {
        outBusIndex = 2;
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == paramIdPitchMod)
    {
        outBusIndex = 2;
        outChannelIndexInBus = 1;
        return true;
    }

    // Bus 3: Word Triggers (Word 1-16)
    if (paramId.startsWith("word") && paramId.endsWith("_trig_mod"))
    {
        int wordNum = paramId.fromFirstOccurrenceOf("word", false, false)
                          .upToFirstOccurrenceOf("_trig_mod", false, false)
                          .getIntValue();
        if (wordNum > 0 && wordNum <= 16)
        {
            outBusIndex = 3;
            outChannelIndexInBus = wordNum - 1; // 0-indexed within bus
            return true;
        }
    }

    return false;
}

juce::String TTSPerformerModuleProcessor::getAudioInputLabel(int channel) const
{
    // Multi-bus absolute channel mapping (flattened in bus order):
    // Bus 0 (Global Mods): 0..4
    // Bus 1 (Trim Mods):   5..6
    // Bus 2 (Playback):    7..8
    // Bus 3 (Word Trigs):  9..24
    switch (channel)
    {
    case 0:
        return "Rate Mod";
    case 1:
        return "Gate Mod";
    case 2:
        return "Trigger";
    case 3:
        return "Reset";
    case 4:
        return "Randomize Trig"; // <-- NEW
    case 5:
        return "Trim Start Mod";
    case 6:
        return "Trim End Mod";
    case 7:
        return "Speed Mod";
    case 8:
        return "Pitch Mod";
    default:
        if (channel >= 9 && channel < 25)
            return "Word " + juce::String(channel - 8) + " Trig"; // 9->Word1, 24->Word16
        return {};
    }
}

juce::String TTSPerformerModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0)
        return "Audio";
    if (channel == 1)
        return "Word Gate";
    if (channel == 2)
        return "EOP Gate";
    if (channel >= 3 && channel < 19)
        return "Word " + juce::String(channel - 2) + " Gate";
    if (channel >= 19 && channel < 35)
        return "Word " + juce::String(channel - 18) + " Trig";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void TTSPerformerModuleProcessor::refreshModelChoices()
{
    const juce::ScopedLock lock(modelLock);
    modelEntries.clear();
    // Prefer mapping file if present
    auto mapFile = resolveModelsBaseDir().getChildFile("piper_voices_map.md");
    if (!loadVoicesFromMapFile(mapFile))
    {
        // Scan models directory recursively for .onnx files under piper-voices
        juce::File base = resolveModelsBaseDir().getChildFile("piper-voices");
        if (base.isDirectory())
        {
            juce::Array<juce::File> files;
            base.findChildFiles(files, juce::File::findFiles, true, "*.onnx");
            for (auto f : files)
            {
                auto rel = f.getRelativePathFrom(resolveModelsBaseDir());
                auto parts = juce::StringArray::fromTokens(rel, "\\/", "");
                // Expect: piper-voices/<lang>/<locale>/<voice>/<quality>/<file>
                if (parts.size() >= 6 && parts[0].equalsIgnoreCase("piper-voices"))
                {
                    ModelEntry e;
                    e.language = parts[1];
                    e.locale = parts[2];
                    e.voice = parts[3];
                    e.quality = parts[4];
                    e.relativeOnnx = rel.replaceCharacter('\\', '/');
                    modelEntries.push_back(e);
                }
            }
        }
    }
    if (modelEntries.empty())
    {
        // Fallback seed
        ModelEntry e{
            "en",
            "en_US",
            "lessac",
            "medium",
            "piper-voices/en/en_US/lessac/medium/en_US-lessac-medium.onnx"};
        modelEntries.push_back(e);
    }
}
#endif

juce::File TTSPerformerModuleProcessor::resolveModelsBaseDir() const
{
    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto candidate = exeDir.getChildFile("models");
    if (candidate.isDirectory())
        return candidate;
    auto parent = exeDir.getParentDirectory();
    auto parentModels = parent.getChildFile("models");
    if (parentModels.isDirectory())
        return parentModels;
    return exeDir;
}

juce::File TTSPerformerModuleProcessor::resolveSelectedModelFile() const
{
    const juce::ScopedLock lock(modelLock);
    for (const auto& e : modelEntries)
    {
        if (e.locale == selectedLocale && e.voice == selectedVoice && e.quality == selectedQuality)
            return resolveModelsBaseDir().getChildFile(e.relativeOnnx);
    }
    for (const auto& e : modelEntries)
    {
        if (e.locale == selectedLocale && e.voice == selectedVoice)
            return resolveModelsBaseDir().getChildFile(e.relativeOnnx);
    }
    return resolveModelsBaseDir().getChildFile(
        "piper-voices/en/en_US/lessac/medium/en_US-lessac-medium.onnx");
}

// --- Voice Manifest and Status Checking Implementation ---

std::vector<TTSPerformerModuleProcessor::VoiceEntry> TTSPerformerModuleProcessor::
    getAllAvailableVoices()
{
    // This is the comprehensive list from the PowerShell script
    // Mark 3-4 voices as isIncluded=true for default distribution
    std::vector<VoiceEntry> voices;

    // English (US) - Various accents and styles
    voices.emplace_back(
        "en_US-lessac-medium",
        "English (US)",
        "General American",
        "Female",
        "Medium",
        true); // DEFAULT
    voices.emplace_back(
        "en_US-lessac-high", "English (US)", "General American", "Female", "High", false);
    voices.emplace_back(
        "en_US-lessac-low", "English (US)", "General American", "Female", "Low", false);
    voices.emplace_back(
        "en_US-libritts-high", "English (US)", "General American", "Male", "High", false);
    voices.emplace_back(
        "en_US-libritts-medium",
        "English (US)",
        "General American",
        "Male",
        "Medium",
        true); // DEFAULT
    voices.emplace_back(
        "en_US-libritts-low", "English (US)", "General American", "Male", "Low", false);
    voices.emplace_back("en_US-vctk-medium", "English (US)", "Various", "Mixed", "Medium", false);

    // English (UK) - British accents
    voices.emplace_back("en_GB-alan-medium", "English (UK)", "British", "Male", "Medium", false);
    voices.emplace_back("en_GB-alan-high", "English (UK)", "British", "Male", "High", false);
    voices.emplace_back(
        "en_GB-southern_english_female-medium",
        "English (UK)",
        "Southern British",
        "Female",
        "Medium",
        false);

    // English (AU) - Australian accent
    voices.emplace_back(
        "en_AU-shmale-medium", "English (AU)", "Australian", "Male", "Medium", false);

    // German voices
    voices.emplace_back(
        "de_DE-thorsten-medium", "German", "Standard German", "Male", "Medium", true); // DEFAULT
    voices.emplace_back("de_DE-thorsten-high", "German", "Standard German", "Male", "High", false);
    voices.emplace_back("de_DE-thorsten-low", "German", "Standard German", "Male", "Low", false);
    voices.emplace_back(
        "de_DE-ramona-medium", "German", "Standard German", "Female", "Medium", false);
    voices.emplace_back("de_DE-ramona-low", "German", "Standard German", "Female", "Low", false);
    voices.emplace_back("de_DE-pavoque-low", "German", "Standard German", "Female", "Low", false);
    voices.emplace_back("de_DE-eva_k-x_low", "German", "Standard German", "Female", "x_low", false);
    voices.emplace_back("de_DE-karlsson-low", "German", "Standard German", "Male", "Low", false);
    voices.emplace_back("de_DE-kerstin-low", "German", "Standard German", "Female", "Low", false);
    voices.emplace_back("de_DE-mls-medium", "German", "Standard German", "Mixed", "Medium", false);
    voices.emplace_back(
        "de_DE-thorsten_emotional-medium", "German", "Standard German", "Male", "Medium", false);

    // Spanish voices
    voices.emplace_back(
        "es_ES-davefx-medium", "Spanish (Spain)", "Castilian", "Male", "Medium", false);
    voices.emplace_back("es_ES-davefx-high", "Spanish (Spain)", "Castilian", "Male", "High", false);
    voices.emplace_back(
        "es_MX-claudio-medium", "Spanish (Mexico)", "Mexican", "Male", "Medium", false);

    // French voices
    voices.emplace_back(
        "fr_FR-siwis-medium", "French", "Standard French", "Male", "Medium", true); // DEFAULT
    voices.emplace_back("fr_FR-siwis-high", "French", "Standard French", "Male", "High", false);
    voices.emplace_back("fr_FR-siwis-low", "French", "Standard French", "Male", "Low", false);
    voices.emplace_back(
        "fr_FR-siwis_female-medium", "French", "Standard French", "Female", "Medium", false);

    // Italian voices
    voices.emplace_back(
        "it_IT-riccardo-medium", "Italian", "Standard Italian", "Male", "Medium", false);
    voices.emplace_back(
        "it_IT-riccardo-high", "Italian", "Standard Italian", "Male", "High", false);

    // Portuguese voices
    voices.emplace_back(
        "pt_BR-faber-medium", "Portuguese (Brazil)", "Brazilian", "Male", "Medium", false);
    voices.emplace_back(
        "pt_BR-faber-high", "Portuguese (Brazil)", "Brazilian", "Male", "High", false);

    // Dutch voices
    voices.emplace_back("nl_NL-mls-medium", "Dutch", "Standard Dutch", "Male", "Medium", false);
    voices.emplace_back("nl_NL-mls-high", "Dutch", "Standard Dutch", "Male", "High", false);

    // Russian voices
    voices.emplace_back(
        "ru_RU-dmitri-medium", "Russian", "Standard Russian", "Male", "Medium", false);
    voices.emplace_back("ru_RU-dmitri-high", "Russian", "Standard Russian", "Male", "High", false);

    // Chinese voices
    voices.emplace_back(
        "zh_CN-huayan-medium",
        "Chinese (Mandarin)",
        "Standard Mandarin",
        "Female",
        "Medium",
        false);
    voices.emplace_back(
        "zh_CN-huayan-high", "Chinese (Mandarin)", "Standard Mandarin", "Female", "High", false);

    // Japanese voices
    voices.emplace_back(
        "ja_JP-ljspeech-medium", "Japanese", "Standard Japanese", "Female", "Medium", false);
    voices.emplace_back(
        "ja_JP-ljspeech-high", "Japanese", "Standard Japanese", "Female", "High", false);

    // Korean voices
    voices.emplace_back("ko_KR-kss-medium", "Korean", "Standard Korean", "Female", "Medium", false);

    // Polish voices
    voices.emplace_back(
        "pl_PL-darkman-medium", "Polish", "Standard Polish", "Male", "Medium", false);

    // Czech voices
    voices.emplace_back("cs_CZ-jirka-medium", "Czech", "Standard Czech", "Male", "Medium", false);

    // Greek voices
    voices.emplace_back(
        "el_GR-rapunzelina-medium", "Greek", "Standard Greek", "Female", "Medium", false);

    // Finnish voices
    voices.emplace_back(
        "fi_FI-harri-medium", "Finnish", "Standard Finnish", "Male", "Medium", false);

    // Swedish voices
    voices.emplace_back("sv_SE-nst-medium", "Swedish", "Standard Swedish", "Male", "Medium", false);

    // Norwegian voices
    voices.emplace_back(
        "nb_NO-talesyntese-medium", "Norwegian", "Standard Norwegian", "Male", "Medium", false);

    // Danish voices
    voices.emplace_back(
        "da_DK-talesyntese-medium", "Danish", "Standard Danish", "Male", "Medium", false);

    // Additional voices from voices.json
    // Arabic voices
    voices.emplace_back("ar_JO-kareem-low", "Arabic", "Standard Arabic", "Unknown", "Low", false);
    voices.emplace_back(
        "ar_JO-kareem-medium", "Arabic", "Standard Arabic", "Unknown", "Medium", false);

    // Catalan voices
    voices.emplace_back(
        "ca_ES-upc_ona-medium", "Catalan", "Standard Catalan", "Unknown", "Medium", false);
    voices.emplace_back(
        "ca_ES-upc_ona-x_low", "Catalan", "Standard Catalan", "Unknown", "x_low", false);
    voices.emplace_back(
        "ca_ES-upc_pau-x_low", "Catalan", "Standard Catalan", "Unknown", "x_low", false);

    // Chinese voices
    voices.emplace_back(
        "zh_CN-huayan-x_low", "Chinese (Mandarin)", "Standard Mandarin", "Unknown", "x_low", false);

    // Czech voices
    voices.emplace_back("cs_CZ-jirka-low", "Czech", "Standard Czech", "Male", "Low", false);

    // Dutch voices (additional)
    voices.emplace_back("nl_BE-nathalie-medium", "Dutch", "Belgian", "Female", "Medium", false);
    voices.emplace_back("nl_BE-nathalie-x_low", "Dutch", "Belgian", "Female", "x_low", false);
    voices.emplace_back("nl_BE-rdh-medium", "Dutch", "Belgian", "Unknown", "Medium", false);
    voices.emplace_back("nl_BE-rdh-x_low", "Dutch", "Belgian", "Unknown", "x_low", false);
    voices.emplace_back("nl_NL-mls_5809-low", "Dutch", "Standard Dutch", "Unknown", "Low", false);
    voices.emplace_back("nl_NL-mls_7432-low", "Dutch", "Standard Dutch", "Unknown", "Low", false);
    voices.emplace_back("nl_NL-pim-medium", "Dutch", "Standard Dutch", "Unknown", "Medium", false);
    voices.emplace_back(
        "nl_NL-ronnie-medium", "Dutch", "Standard Dutch", "Unknown", "Medium", false);

    // English voices (additional)
    voices.emplace_back("en_GB-alan-low", "English (UK)", "British", "Male", "Low", false);
    voices.emplace_back("en_GB-alba-medium", "English (UK)", "British", "Female", "Medium", false);
    voices.emplace_back("en_GB-aru-medium", "English (UK)", "British", "Mixed", "Medium", false);
    voices.emplace_back("en_GB-cori-high", "English (UK)", "British", "Unknown", "High", false);
    voices.emplace_back("en_GB-cori-medium", "English (UK)", "British", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_GB-jenny_dioco-medium", "English (UK)", "British", "Female", "Medium", false);
    voices.emplace_back(
        "en_GB-northern_english_male-medium", "English (UK)", "British", "Male", "Medium", false);
    voices.emplace_back(
        "en_GB-semaine-medium", "English (UK)", "British", "Mixed", "Medium", false);
    voices.emplace_back(
        "en_GB-southern_english_female-low",
        "English (UK)",
        "Southern British",
        "Female",
        "Low",
        false);
    voices.emplace_back("en_GB-vctk-medium", "English (UK)", "British", "Mixed", "Medium", false);
    voices.emplace_back(
        "en_US-amy-low", "English (US)", "General American", "Female", "Low", false);
    voices.emplace_back(
        "en_US-amy-medium", "English (US)", "General American", "Female", "Medium", false);
    voices.emplace_back(
        "en_US-arctic-medium", "English (US)", "General American", "Mixed", "Medium", false);
    voices.emplace_back(
        "en_US-bryce-medium", "English (US)", "General American", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_US-danny-low", "English (US)", "General American", "Unknown", "Low", false);
    voices.emplace_back(
        "en_US-hfc_female-medium", "English (US)", "General American", "Female", "Medium", false);
    voices.emplace_back(
        "en_US-hfc_male-medium", "English (US)", "General American", "Male", "Medium", false);
    voices.emplace_back(
        "en_US-joe-medium", "English (US)", "General American", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_US-john-medium", "English (US)", "General American", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_US-kathleen-low", "English (US)", "General American", "Female", "Low", false);
    voices.emplace_back(
        "en_US-kristin-medium", "English (US)", "General American", "Female", "Medium", false);
    voices.emplace_back(
        "en_US-kusal-medium", "English (US)", "General American", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_US-l2arctic-medium", "English (US)", "General American", "Mixed", "Medium", false);
    voices.emplace_back(
        "en_US-libritts_r-medium", "English (US)", "General American", "Mixed", "Medium", false);
    voices.emplace_back(
        "en_US-ljspeech-high", "English (US)", "General American", "Unknown", "High", false);
    voices.emplace_back(
        "en_US-ljspeech-medium", "English (US)", "General American", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_US-norman-medium", "English (US)", "General American", "Male", "Medium", false);
    voices.emplace_back(
        "en_US-reza_ibrahim-medium",
        "English (US)",
        "General American",
        "Unknown",
        "Medium",
        false);
    voices.emplace_back(
        "en_US-ryan-high", "English (US)", "General American", "Unknown", "High", false);
    voices.emplace_back(
        "en_US-ryan-low", "English (US)", "General American", "Unknown", "Low", false);
    voices.emplace_back(
        "en_US-ryan-medium", "English (US)", "General American", "Unknown", "Medium", false);
    voices.emplace_back(
        "en_US-sam-medium", "English (US)", "General American", "Unknown", "Medium", false);

    // Farsi voices
    voices.emplace_back("fa_IR-amir-medium", "Farsi", "Standard Farsi", "Unknown", "Medium", false);
    voices.emplace_back(
        "fa_IR-ganji-medium", "Farsi", "Standard Farsi", "Unknown", "Medium", false);
    voices.emplace_back(
        "fa_IR-ganji_adabi-medium", "Farsi", "Standard Farsi", "Unknown", "Medium", false);
    voices.emplace_back("fa_IR-gyro-medium", "Farsi", "Standard Farsi", "Unknown", "Medium", false);
    voices.emplace_back(
        "fa_IR-reza_ibrahim-medium", "Farsi", "Standard Farsi", "Unknown", "Medium", false);

    // Finnish voices (additional)
    voices.emplace_back("fi_FI-harri-low", "Finnish", "Standard Finnish", "Male", "Low", false);

    // French voices (additional)
    voices.emplace_back("fr_FR-gilles-low", "French", "Standard French", "Unknown", "Low", false);
    voices.emplace_back("fr_FR-mls-medium", "French", "Standard French", "Mixed", "Medium", false);
    voices.emplace_back("fr_FR-mls_1840-low", "French", "Standard French", "Unknown", "Low", false);
    voices.emplace_back(
        "fr_FR-tom-medium", "French", "Standard French", "Unknown", "Medium", false);
    voices.emplace_back("fr_FR-upmc-medium", "French", "Standard French", "Mixed", "Medium", false);

    // Georgian voices
    voices.emplace_back(
        "ka_GE-natia-medium", "Georgian", "Standard Georgian", "Female", "Medium", false);

    // Greek voices (additional)
    voices.emplace_back("el_GR-rapunzelina-low", "Greek", "Standard Greek", "Female", "Low", false);

    // Hebrew voices
    voices.emplace_back(
        "he_IL-motek-medium", "Hebrew", "Standard Hebrew", "Unknown", "Medium", false);

    // Hindi voices
    voices.emplace_back("hi_IN-pratham-medium", "Hindi", "Standard Hindi", "Male", "Medium", false);
    voices.emplace_back(
        "hi_IN-priyamvada-medium", "Hindi", "Standard Hindi", "Female", "Medium", false);
    voices.emplace_back("hi_IN-rohan-medium", "Hindi", "Standard Hindi", "Male", "Medium", false);

    // Hungarian voices
    voices.emplace_back(
        "hu_HU-anna-medium", "Hungarian", "Standard Hungarian", "Unknown", "Medium", false);
    voices.emplace_back(
        "hu_HU-berta-medium", "Hungarian", "Standard Hungarian", "Unknown", "Medium", false);
    voices.emplace_back(
        "hu_HU-imre-medium", "Hungarian", "Standard Hungarian", "Unknown", "Medium", false);

    // Icelandic voices
    voices.emplace_back(
        "is_IS-bui-medium", "Icelandic", "Standard Icelandic", "Unknown", "Medium", false);
    voices.emplace_back(
        "is_IS-salka-medium", "Icelandic", "Standard Icelandic", "Unknown", "Medium", false);
    voices.emplace_back(
        "is_IS-steinn-medium", "Icelandic", "Standard Icelandic", "Unknown", "Medium", false);
    voices.emplace_back(
        "is_IS-ugla-medium", "Icelandic", "Standard Icelandic", "Unknown", "Medium", false);

    // Indonesian voices
    voices.emplace_back(
        "id_ID-news_tts-medium", "Indonesian", "Standard Indonesian", "Unknown", "Medium", false);

    // Italian voices (additional)
    voices.emplace_back(
        "it_IT-paola-medium", "Italian", "Standard Italian", "Unknown", "Medium", false);
    voices.emplace_back(
        "it_IT-riccardo-x_low", "Italian", "Standard Italian", "Male", "x_low", false);

    // Kazakh voices
    voices.emplace_back(
        "kk_KZ-iseke-x_low", "Kazakh", "Standard Kazakh", "Unknown", "x_low", false);
    voices.emplace_back("kk_KZ-issai-high", "Kazakh", "Standard Kazakh", "Mixed", "High", false);
    voices.emplace_back("kk_KZ-raya-x_low", "Kazakh", "Standard Kazakh", "Unknown", "x_low", false);

    // Latvian voices
    voices.emplace_back(
        "lv_LV-aivars-medium", "Latvian", "Standard Latvian", "Male", "Medium", false);

    // Luxembourgish voices
    voices.emplace_back(
        "lb_LU-marylux-medium",
        "Luxembourgish",
        "Standard Luxembourgish",
        "Female",
        "Medium",
        false);

    // Malayalam voices
    voices.emplace_back(
        "ml_IN-arjun-medium", "Malayalam", "Standard Malayalam", "Male", "Medium", false);
    voices.emplace_back(
        "ml_IN-meera-medium", "Malayalam", "Standard Malayalam", "Female", "Medium", false);

    // Nepali voices
    voices.emplace_back(
        "ne_NP-chitwan-medium", "Nepali", "Standard Nepali", "Unknown", "Medium", false);
    voices.emplace_back(
        "ne_NP-google-medium", "Nepali", "Standard Nepali", "Mixed", "Medium", false);
    voices.emplace_back("ne_NP-google-x_low", "Nepali", "Standard Nepali", "Mixed", "x_low", false);

    // Norwegian voices (additional)
    voices.emplace_back(
        "no_NO-talesyntese-medium", "Norwegian", "Standard Norwegian", "Unknown", "Medium", false);

    // Polish voices (additional)
    voices.emplace_back(
        "pl_PL-gosia-medium", "Polish", "Standard Polish", "Female", "Medium", false);
    voices.emplace_back(
        "pl_PL-mc_speech-medium", "Polish", "Standard Polish", "Unknown", "Medium", false);
    voices.emplace_back("pl_PL-mls_6892-low", "Polish", "Standard Polish", "Unknown", "Low", false);

    // Portuguese voices (additional)
    voices.emplace_back(
        "pt_BR-cadu-medium", "Portuguese (Brazil)", "Brazilian", "Unknown", "Medium", false);
    voices.emplace_back(
        "pt_BR-edresson-low", "Portuguese (Brazil)", "Brazilian", "Unknown", "Low", false);
    voices.emplace_back(
        "pt_BR-jeff-medium", "Portuguese (Brazil)", "Brazilian", "Unknown", "Medium", false);
    voices.emplace_back(
        "pt_PT-tugao-medium",
        "Portuguese (Portugal)",
        "European Portuguese",
        "Unknown",
        "Medium",
        false);

    // Romanian voices
    voices.emplace_back(
        "ro_RO-mihai-medium", "Romanian", "Standard Romanian", "Male", "Medium", false);

    // Russian voices (additional)
    voices.emplace_back(
        "ru_RU-denis-medium", "Russian", "Standard Russian", "Male", "Medium", false);
    voices.emplace_back(
        "ru_RU-irina-medium", "Russian", "Standard Russian", "Female", "Medium", false);
    voices.emplace_back(
        "ru_RU-ruslan-medium", "Russian", "Standard Russian", "Male", "Medium", false);

    // Serbian voices
    voices.emplace_back(
        "sr_RS-serbski_institut-medium", "Serbian", "Standard Serbian", "Mixed", "Medium", false);

    // Slovak voices
    voices.emplace_back(
        "sk_SK-lili-medium", "Slovak", "Standard Slovak", "Unknown", "Medium", false);

    // Slovenian voices
    voices.emplace_back(
        "sl_SI-artur-medium", "Slovenian", "Standard Slovenian", "Male", "Medium", false);

    // Spanish voices (additional)
    voices.emplace_back(
        "es_AR-daniela-high", "Spanish (Argentina)", "Argentinian", "Female", "High", false);
    voices.emplace_back(
        "es_ES-carlfm-x_low", "Spanish (Spain)", "Castilian", "Unknown", "x_low", false);
    voices.emplace_back(
        "es_ES-mls_10246-low", "Spanish (Spain)", "Castilian", "Unknown", "Low", false);
    voices.emplace_back(
        "es_ES-mls_9972-low", "Spanish (Spain)", "Castilian", "Unknown", "Low", false);
    voices.emplace_back(
        "es_ES-sharvard-medium", "Spanish (Spain)", "Castilian", "Mixed", "Medium", false);
    voices.emplace_back(
        "es_MX-ald-medium", "Spanish (Mexico)", "Mexican", "Unknown", "Medium", false);
    voices.emplace_back(
        "es_MX-claude-high", "Spanish (Mexico)", "Mexican", "Unknown", "High", false);

    // Swahili voices
    voices.emplace_back(
        "sw_CD-lanfrica-medium", "Swahili", "Standard Swahili", "Unknown", "Medium", false);

    // Swedish voices (additional)
    voices.emplace_back(
        "sv_SE-lisa-medium", "Swedish", "Standard Swedish", "Female", "Medium", false);

    // Telugu voices
    voices.emplace_back(
        "te_IN-maya-medium", "Telugu", "Standard Telugu", "Female", "Medium", false);
    voices.emplace_back(
        "te_IN-padmavathi-medium", "Telugu", "Standard Telugu", "Female", "Medium", false);
    voices.emplace_back(
        "te_IN-venkatesh-medium", "Telugu", "Standard Telugu", "Male", "Medium", false);

    // Turkish voices
    voices.emplace_back(
        "tr_TR-dfki-medium", "Turkish", "Standard Turkish", "Unknown", "Medium", false);
    voices.emplace_back(
        "tr_TR-fahrettin-medium", "Turkish", "Standard Turkish", "Male", "Medium", false);
    voices.emplace_back(
        "tr_TR-fettah-medium", "Turkish", "Standard Turkish", "Male", "Medium", false);

    // Ukrainian voices
    voices.emplace_back(
        "uk_UA-lada-x_low", "Ukrainian", "Standard Ukrainian", "Female", "x_low", false);
    voices.emplace_back(
        "uk_UA-ukrainian_tts-medium", "Ukrainian", "Standard Ukrainian", "Mixed", "Medium", false);

    // Vietnamese voices
    voices.emplace_back(
        "vi_VN-25hours_single-low", "Vietnamese", "Standard Vietnamese", "Unknown", "Low", false);
    voices.emplace_back(
        "vi_VN-vais1000-medium", "Vietnamese", "Standard Vietnamese", "Unknown", "Medium", false);
    voices.emplace_back(
        "vi_VN-vivos-x_low", "Vietnamese", "Standard Vietnamese", "Mixed", "x_low", false);

    // Welsh voices
    voices.emplace_back("cy_GB-bu_tts-medium", "Welsh", "British", "Mixed", "Medium", false);
    voices.emplace_back(
        "cy_GB-gwryw_gogleddol-medium", "Welsh", "British", "Unknown", "Medium", false);

    return voices;
}

TTSPerformerModuleProcessor::VoiceStatus TTSPerformerModuleProcessor::checkVoiceStatus(
    const juce::String& voiceName) const
{
    // Parse voice name to construct expected path
    // Format: "en_US-lessac-medium" ->
    // "piper-voices/en/en_US/lessac/medium/en_US-lessac-medium.onnx" Voice names follow pattern:
    // <locale>-<voice>-<quality>

    // Find the last dash to separate quality from the rest
    int lastDash = voiceName.lastIndexOfChar('-');
    if (lastDash < 0)
    {
        DBG("[Voice Status] Invalid voice name format (no dashes): " + voiceName);
        return VoiceStatus::Error;
    }

    // Get everything before the last dash
    juce::String beforeLastDash = voiceName.substring(0, lastDash);

    // Find the second-to-last dash in the substring
    int secondLastDash = beforeLastDash.lastIndexOfChar('-');
    if (secondLastDash < 0)
    {
        DBG("[Voice Status] Invalid voice name format (need at least 2 dashes): " + voiceName);
        return VoiceStatus::Error;
    }

    juce::String locale = voiceName.substring(0, secondLastDash);           // "en_US"
    juce::String voice = voiceName.substring(secondLastDash + 1, lastDash); // "lessac"
    juce::String quality = voiceName.substring(lastDash + 1);               // "medium"

    // Extract language code from locale (e.g., "en_US" -> "en")
    juce::String lang = locale.substring(0, locale.indexOfChar('_'));
    if (lang.isEmpty())
        lang = locale; // Fallback if no underscore

    // Build expected file paths
    juce::File modelsDir = resolveModelsBaseDir();
    juce::File onnxFile = modelsDir.getChildFile("piper-voices")
                              .getChildFile(lang)
                              .getChildFile(locale)
                              .getChildFile(voice)
                              .getChildFile(quality)
                              .getChildFile(voiceName + ".onnx");
    juce::File jsonFile = onnxFile.withFileExtension(".onnx.json");

    bool onnxExists = onnxFile.existsAsFile() && onnxFile.getSize() > 0;
    bool jsonExists = jsonFile.existsAsFile() && jsonFile.getSize() > 0;

    // Check if files are corrupted (too small to be valid)
    const juce::int64 MIN_MODEL_SIZE = 1024 * 1024; // 1 MB minimum (valid models are 60-120 MB)
    const juce::int64 MIN_CONFIG_SIZE = 1000;       // 1 KB minimum

    if (onnxExists && jsonExists)
    {
        // Verify files are not corrupted (too small)
        if (onnxFile.getSize() < MIN_MODEL_SIZE || jsonFile.getSize() < MIN_CONFIG_SIZE)
        {
            DBG("[Voice Status] Files exist but are too small (corrupted): " + voiceName);
            return VoiceStatus::Error; // Mark as error if corrupted
        }
        return VoiceStatus::Installed;
    }
    else if (onnxExists || jsonExists)
        return VoiceStatus::Partial; // One file missing
    else
        return VoiceStatus::NotInstalled;
}

std::map<juce::String, TTSPerformerModuleProcessor::VoiceStatus> TTSPerformerModuleProcessor::
    checkAllVoiceStatuses() const
{
    std::map<juce::String, VoiceStatus> statuses;
    auto                                availableVoices = getAllAvailableVoices();

    for (const auto& voice : availableVoices)
    {
        statuses[voice.name] = checkVoiceStatus(voice.name);
    }

    return statuses;
}

bool TTSPerformerModuleProcessor::loadVoicesFromMapFile(const juce::File& mapFile)
{
    if (!mapFile.existsAsFile())
        return false;
    juce::String content = mapFile.loadFileAsString();
    auto         lines = juce::StringArray::fromLines(content);
    for (auto& line : lines)
    {
        auto l = line.trim();
        if (l.isEmpty() || !l.endsWithIgnoreCase(".onnx"))
            continue;
        juce::String p = l;
        // Lines may start with leading backslash; normalize
        while (p.startsWithChar('\\') || p.startsWithChar('/'))
            p = p.substring(1);
        p = p.replaceCharacter('\\', '/');
        auto parts = juce::StringArray::fromTokens(p, "/", "");
        // Expect: piper-voices/<lang>/<locale>/<voice>/<quality>/<file>
        int idx = parts.indexOf("piper-voices");
        if (idx < 0)
            continue;
        if (parts.size() >= idx + 6)
        {
            ModelEntry e;
            e.language = parts[idx + 1];
            e.locale = parts[idx + 2];
            e.voice = parts[idx + 3];
            e.quality = parts[idx + 4];
            juce::String rel;
            for (int i = idx; i < parts.size(); ++i)
            {
                if (i > idx)
                    rel += "/";
                rel += parts[i];
            }
            e.relativeOnnx = rel;
            modelEntries.push_back(e);
        }
    }
    return !modelEntries.empty();
}

const std::vector<WordTiming>& TTSPerformerModuleProcessor::getActiveTimings() const
{
    return (selectedClip && !selectedClip->timings.empty()) ? selectedClip->timings
                                                            : lastSynthesisTimings;
}

int TTSPerformerModuleProcessor::findFirstWordIndexAtOrAfter(double timeSec) const
{
    const auto& t = getActiveTimings();
    for (int i = 0; i < (int)t.size(); ++i)
        if (t[(size_t)i].endTimeSeconds >= timeSec)
            return i;
    return juce::jmax(0, (int)t.size() - 1);
}

int TTSPerformerModuleProcessor::findLastWordIndexAtOrBefore(double timeSec) const
{
    const auto& t = getActiveTimings();
    for (int i = (int)t.size() - 1; i >= 0; --i)
        if (t[(size_t)i].startTimeSeconds <= timeSec)
            return i;
    return 0;
}

void TTSPerformerModuleProcessor::clampWordIndexToTrim()
{
    const auto& t = getActiveTimings();
    if (t.empty())
        return;
    // Read trim from parameters (normalized 0-1)
    float  trimStartNorm = apvts.getRawParameterValue("trimStart")->load();
    float  trimEndNorm = apvts.getRawParameterValue("trimEnd")->load();
    double totalDur = selectedClip ? selectedClip->durationSeconds : t.back().endTimeSeconds;
    double t0 = trimStartNorm * totalDur;
    double t1 = trimEndNorm * totalDur;
    int    minIdx = findFirstWordIndexAtOrAfter(t0);
    int    maxIdx = findLastWordIndexAtOrBefore(t1);
    currentWordIndex = juce::jlimit(minIdx, maxIdx, currentWordIndex);
}

void TTSPerformerModuleProcessor::normalizeModelSelection()
{
    const juce::ScopedLock lock(modelLock);
    if (modelEntries.empty())
        return;
    // Ensure selectedLanguage exists; otherwise pick first
    auto languageExists =
        std::find_if(modelEntries.begin(), modelEntries.end(), [&](const ModelEntry& e) {
            return e.language == selectedLanguage;
        }) != modelEntries.end();
    if (!languageExists)
        selectedLanguage = modelEntries.front().language;
    // Limit locale set to language
    juce::StringArray locales;
    for (const auto& e : modelEntries)
        if (e.language == selectedLanguage && !locales.contains(e.locale))
            locales.add(e.locale);
    if (!locales.contains(selectedLocale) && locales.size() > 0)
        selectedLocale = locales[0];
    // Limit voices to locale
    juce::StringArray voices;
    for (const auto& e : modelEntries)
        if (e.locale == selectedLocale && !voices.contains(e.voice))
            voices.add(e.voice);
    if (!voices.contains(selectedVoice) && voices.size() > 0)
        selectedVoice = voices[0];
    // Limit qualities to voice
    juce::StringArray qualities;
    for (const auto& e : modelEntries)
        if (e.locale == selectedLocale && e.voice == selectedVoice &&
            !qualities.contains(e.quality))
            qualities.add(e.quality);
    if (!qualities.contains(selectedQuality) && qualities.size() > 0)
        selectedQuality = qualities[0];
}

#if defined(PRESET_CREATOR_UI)
float TTSPerformerModuleProcessor::getLiveNoGate(const juce::String& liveKey, float fallback) const
{
    // Read live telemetry directly without engine connectivity gating
    if (auto it = paramLiveValues.find(liveKey); it != paramLiveValues.end())
        return it->second.load(std::memory_order_relaxed);
    return fallback;
}

void TTSPerformerModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);
    
    // Status indicator
    Status status = currentStatus.load();
    switch (status)
    {
    case Status::Idle:
        ImGui::Text("Status: Ready");
        break;
    case Status::Synthesizing:
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Status: Synthesizing...");
        break;
    case Status::Playing:
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Playing");
        break;
    case Status::Error:
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Status: Error");
        break;
    }

    // Error message
    if (status == Status::Error)
    {
        const juce::ScopedLock lock(messageLock);
        if (errorMessage.isNotEmpty())
        {
            ImGui::TextWrapped("Error: %s", errorMessage.toRawUTF8());
        }
    }

    // Text input (compact, use itemWidth directly)
    ImGui::PushItemWidth(itemWidth);
    ImGui::InputTextMultiline(
        "##TextInput",
        uiTextBuffer,
        sizeof(uiTextBuffer),
        ImVec2(itemWidth, 45),
        ImGuiInputTextFlags_None);
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(itemWidth);

    // --- SYNC CONTROLS ---
    bool sync = apvts.getRawParameterValue("sync")->load() > 0.5f;
    if (ImGui::Checkbox("Sync to Transport", &sync))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("sync")))
            *p = sync;
        onModificationEnded();
    }

    if (sync)
    {
        // Check if global division is active (Tempo Clock override)
        // IMPORTANT: Read from parent's LIVE transport state, not cached copy
        int globalDiv =
            getParent() ? getParent()->getTransportState().globalDivisionIndex.load() : -1;
        bool isGlobalDivisionActive = globalDiv >= 0;
        int  division = isGlobalDivisionActive
                            ? globalDiv
                            : (int)apvts.getRawParameterValue("rate_division")->load();

        // Grey out if controlled by Tempo Clock
        if (isGlobalDivisionActive)
            ImGui::BeginDisabled();

        if (ImGui::Combo(
                "Division",
                &division,
                "1/32\0"
                "1/16\0"
                "1/8\0"
                "1/4\0"
                "1/2\0"
                "1\0"
                "2\0"
                "4\0"
                "8\0\0"))
        {
            if (!isGlobalDivisionActive)
            {
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(
                        apvts.getParameter("rate_division")))
                    *p = division;
                onModificationEnded();
            }
        }

        if (isGlobalDivisionActive)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
                ImGui::TextColored(
                    ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Tempo Clock Division Override Active");
                ImGui::TextUnformatted(
                    "A Tempo Clock node with 'Division Override' enabled is controlling the global "
                    "division.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }
    else
    {
        // Rate slider (only shown in free-running mode, with modulation feedback + proper
        // undo/redo)
        const bool rateIsMod = isParamModulated("rate_mod");
        float rate = rateIsMod ? getLiveNoGate("rate_live", rateParam->load()) : rateParam->load();

        // Inline pin for Rate Mod
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            int busIdx, chanInBus;
            if (getParamRouting(paramIdRateMod, busIdx, chanInBus))
            {
                int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
                if (pinHelpers->drawInlineInputPin(channel))
                    ImGui::SameLine();
            }
        }

        if (rateIsMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat(
                "Rate (Hz)", &rate, 0.1f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
        {
            if (!rateIsMod)
            {
                auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("rate"));
                if (param)
                    param->setValueNotifyingHost(
                        apvts.getParameterRange("rate").convertTo0to1(rate));
            }
        }
        if (!rateIsMod)
            adjustParamOnWheel(apvts.getParameter("rate"), "rate", rate);
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (rateIsMod)
        {
            ImGui::EndDisabled();
            const float baseRateDisp = rateParam->load();
            const float liveRateDisp = getLiveNoGate("rate_live", baseRateDisp);
            ImGui::SameLine();
            ImGui::Text("%.2f Hz -> %.2f Hz (mod)", baseRateDisp, liveRateDisp);
        }
    }

    // Gate slider
    const bool gateIsMod = isParamModulated("gate_mod");
    float      gate = gateIsMod ? getLiveNoGate("gate_live", gateParam->load()) : gateParam->load();

    // Inline pin for Gate Mod
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting(paramIdGateMod, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }

    if (gateIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Gate", &gate, 0.0f, 1.0f, "%.3f"))
    {
        if (!gateIsMod)
        {
            auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gate"));
            if (param)
                param->setValueNotifyingHost(apvts.getParameterRange("gate").convertTo0to1(gate));
        }
    }
    if (!gateIsMod)
        adjustParamOnWheel(apvts.getParameter("gate"), "gate", gate);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (gateIsMod)
    {
        ImGui::EndDisabled();
        const float baseGateDisp = gateParam->load();
        const float liveGateDisp = getLiveNoGate("gate_live", baseGateDisp);
        ImGui::SameLine();
        ImGui::Text("%.0f%% -> %.0f%% (mod)", baseGateDisp * 100.0f, liveGateDisp * 100.0f);
    }

    // Volume slider (with proper undo/redo)
    float volume = volumeParam->load();
    if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f"))
    {
        auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("volume"));
        if (param)
            param->setValueNotifyingHost(apvts.getParameterRange("volume").convertTo0to1(volume));
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();

    // Engine UI (minimal) - default to Naive if param missing
    {
        int engineIdx = 1;
        if (auto* p = apvts.getParameter("engine"))
            engineIdx = (int)p->getValue();
        const char* items[] = {"RubberBand", "Naive"};
        if (ImGui::Combo("Engine", &engineIdx, items, 2))
        {
            if (auto* p = apvts.getParameter("engine"))
                p->setValueNotifyingHost((float)engineIdx);
            onModificationEnded();
        }
    }

    // Speed slider (with modulation feedback)
    const bool speedIsMod = isParamModulated("speed_mod");
    float      speed = speedIsMod
                           ? getLiveNoGate("speed_live", apvts.getRawParameterValue("speed")->load())
                           : apvts.getRawParameterValue("speed")->load();

    // Inline pin for Speed Mod
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting(paramIdSpeedMod, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }

    if (speedIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Speed", &speed, 0.25f, 4.0f, "%.2fx"))
    {
        if (!speedIsMod)
        {
            auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("speed"));
            if (param)
                param->setValueNotifyingHost(apvts.getParameterRange("speed").convertTo0to1(speed));
        }
    }
    if (!speedIsMod)
        adjustParamOnWheel(apvts.getParameter("speed"), "speed", speed);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (speedIsMod)
    {
        ImGui::EndDisabled();
        const float baseSpeedDisp = apvts.getRawParameterValue("speed")->load();
        const float liveSpeedDisp = getLiveNoGate("speed_live", baseSpeedDisp);
        ImGui::SameLine();
        ImGui::Text("%.2fx -> %.2fx (mod)", baseSpeedDisp, liveSpeedDisp);
    }

    // Pitch slider (with modulation feedback)
    const bool pitchIsMod = isParamModulated("pitch_mod");
    float      pitch = pitchIsMod
                           ? getLiveNoGate("pitch_live", apvts.getRawParameterValue("pitch")->load())
                           : apvts.getRawParameterValue("pitch")->load();

    // Inline pin for Pitch Mod
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting(paramIdPitchMod, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }

    if (pitchIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Pitch", &pitch, -24.0f, 24.0f, "%.1f st"))
    {
        if (!pitchIsMod)
        {
            auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitch"));
            if (param)
                param->setValueNotifyingHost(apvts.getParameterRange("pitch").convertTo0to1(pitch));
        }
    }
    if (!pitchIsMod)
        adjustParamOnWheel(apvts.getParameter("pitch"), "pitch", pitch);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (pitchIsMod)
    {
        ImGui::EndDisabled();
        const float basePitchDisp = apvts.getRawParameterValue("pitch")->load();
        const float livePitchDisp = getLiveNoGate("pitch_live", basePitchDisp);
        ImGui::SameLine();
        ImGui::Text("%.1f st -> %.1f st (mod)", basePitchDisp, livePitchDisp);
    }

    // Trim sliders (with modulation feedback)
    const bool trimStartIsMod = isParamModulated("trimStart_mod");
    float      trimStart =
        trimStartIsMod
                 ? getLiveNoGate("trimStart_live", apvts.getRawParameterValue("trimStart")->load())
                 : apvts.getRawParameterValue("trimStart")->load();
    
    // Inline pin for Trim Start Mod
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting(paramIdTrimStartMod, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }
    
    if (trimStartIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Trim Start", &trimStart, 0.0f, 1.0f, "%.3f"))
    {
        if (!trimStartIsMod)
        {
            auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("trimStart"));
            if (param)
                param->setValueNotifyingHost(
                    apvts.getParameterRange("trimStart").convertTo0to1(trimStart));
        }
    }
    if (!trimStartIsMod)
        adjustParamOnWheel(apvts.getParameter("trimStart"), "trimStart", trimStart);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (trimStartIsMod)
    {
        ImGui::EndDisabled();
        const float  baseTrimStartDisp = apvts.getRawParameterValue("trimStart")->load();
        const float  liveTrimStartDisp = getLiveNoGate("trimStart_live", baseTrimStartDisp);
        const double durSec = (selectedClip ? selectedClip->durationSeconds : 0.0);
        ImGui::SameLine();
        if (durSec > 0.0)
            ImGui::Text(
                "%.3f -> %.3f (%.2fs -> %.2fs) (mod)",
                baseTrimStartDisp,
                liveTrimStartDisp,
                baseTrimStartDisp * (float)durSec,
                liveTrimStartDisp * (float)durSec);
        else
            ImGui::Text("%.3f -> %.3f (mod)", baseTrimStartDisp, liveTrimStartDisp);
    }

    const bool trimEndIsMod = isParamModulated("trimEnd_mod");
    float      trimEnd =
        trimEndIsMod ? getLiveNoGate("trimEnd_live", apvts.getRawParameterValue("trimEnd")->load())
                          : apvts.getRawParameterValue("trimEnd")->load();
    
    // Inline pin for Trim End Mod
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting(paramIdTrimEndMod, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }
    
    if (trimEndIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Trim End", &trimEnd, 0.0f, 1.0f, "%.3f"))
    {
        if (!trimEndIsMod)
        {
            auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("trimEnd"));
            if (param)
                param->setValueNotifyingHost(
                    apvts.getParameterRange("trimEnd").convertTo0to1(trimEnd));
        }
    }
    if (!trimEndIsMod)
        adjustParamOnWheel(apvts.getParameter("trimEnd"), "trimEnd", trimEnd);
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (trimEndIsMod)
    {
        ImGui::EndDisabled();
        const float  baseTrimEndDisp = apvts.getRawParameterValue("trimEnd")->load();
        const float  liveTrimEndDisp = getLiveNoGate("trimEnd_live", baseTrimEndDisp);
        const double durSec = (selectedClip ? selectedClip->durationSeconds : 0.0);
        ImGui::SameLine();
        if (durSec > 0.0)
            ImGui::Text(
                "%.3f -> %.3f (%.2fs -> %.2fs) (mod)",
                baseTrimEndDisp,
                liveTrimEndDisp,
                baseTrimEndDisp * (float)durSec,
                liveTrimEndDisp * (float)durSec);
        else
            ImGui::Text("%.3f -> %.3f (mod)", baseTrimEndDisp, liveTrimEndDisp);
    }

    ImGui::PopItemWidth();

    // Model selection (compact)
    {
        if (modelEntries.empty())
        {
            refreshModelChoices();
            loadClipsFromDisk();
        }
        normalizeModelSelection();
        const juce::ScopedLock lock(modelLock);
        // Build unique sets
        juce::StringArray languages, locales, voices, qualities;
        for (const auto& e : modelEntries)
            if (!languages.contains(e.language))
                languages.add(e.language);
        // Language
        juce::String langShown = selectedLanguage;
        ImGui::PushItemWidth(itemWidth * 0.4f); // Constrain dropdown width
        if (ImGui::BeginCombo("Language", langShown.toRawUTF8()))
        {
            for (auto& l : languages)
            {
                bool sel = (l == selectedLanguage);
                if (ImGui::Selectable(l.toRawUTF8(), sel))
                {
                    selectedLanguage = l;
                    selectedLocale.clear();
                    selectedVoice.clear();
                    selectedQuality.clear();
                    normalizeModelSelection();
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        // Locales for language
        for (const auto& e : modelEntries)
            if (e.language == selectedLanguage && !locales.contains(e.locale))
                locales.add(e.locale);
        juce::String locShown = selectedLocale;
        ImGui::PushItemWidth(itemWidth * 0.4f);
        if (ImGui::BeginCombo("Locale", locShown.toRawUTF8()))
        {
            for (auto& l : locales)
            {
                bool sel = (l == selectedLocale);
                if (ImGui::Selectable(l.toRawUTF8(), sel))
                {
                    selectedLocale = l;
                    selectedVoice.clear();
                    selectedQuality.clear();
                    normalizeModelSelection();
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        // Voices for locale
        for (const auto& e : modelEntries)
            if (e.locale == selectedLocale && !voices.contains(e.voice))
                voices.add(e.voice);
        juce::String voiceShown = selectedVoice;
        ImGui::PushItemWidth(itemWidth * 0.4f);
        if (ImGui::BeginCombo("Voice", voiceShown.toRawUTF8()))
        {
            for (auto& v : voices)
            {
                bool sel = (v == selectedVoice);
                if (ImGui::Selectable(v.toRawUTF8(), sel))
                {
                    selectedVoice = v;
                    selectedQuality.clear();
                    normalizeModelSelection();
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        // Quality for voice
        for (const auto& e : modelEntries)
            if (e.locale == selectedLocale && e.voice == selectedVoice &&
                !qualities.contains(e.quality))
                qualities.add(e.quality);
        juce::String qualShown = selectedQuality;
        ImGui::PushItemWidth(itemWidth * 0.4f);
        if (ImGui::BeginCombo("Quality", qualShown.toRawUTF8()))
        {
            for (auto& q : qualities)
            {
                bool sel = (q == selectedQuality);
                if (ImGui::Selectable(q.toRawUTF8(), sel))
                {
                    selectedQuality = q;
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
    }

    // Phase 5: Clips panel (list/select)
    drawClipsPanel(itemWidth);

    // SINGLE UNIFIED TIMELINE (after Clips panel, before BAKE)
    if (selectedClip && !selectedClip->timings.empty())
    {
        ImGui::Text("Timeline");

        const float canvasWidth = 600.0f;  // Fixed width for better horizontal space
        const float canvasHeight = 200.0f; // Compact height to reduce vertical space
        ImVec2      canvas_p0 = ImGui::GetCursorScreenPos();
        ImVec2      canvas_p1(canvas_p0.x + canvasWidth, canvas_p0.y + canvasHeight);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(20, 20, 20, 255));

        double totalDur =
            selectedClip->durationSeconds > 0.0
                ? selectedClip->durationSeconds
                : ((double)selectedClip->audio.getNumSamples() / juce::jmax(1.0, getSampleRate()));

        // Get the live, modulated trim values to correctly position the UI handles
        const bool trimStartIsMod = isParamModulated("trimStart_mod");
        float      trimStartNorm = trimStartIsMod ? getLiveParamValueFor(
                                                   "trimStart_mod",
                                                   "trimStart_live",
                                                   apvts.getRawParameterValue("trimStart")->load())
                                                  : apvts.getRawParameterValue("trimStart")->load();

        const bool trimEndIsMod = isParamModulated("trimEnd_mod");
        float      trimEndNorm =
            trimEndIsMod
                     ? getLiveParamValueFor(
                      "trimEnd_mod", "trimEnd_live", apvts.getRawParameterValue("trimEnd")->load())
                     : apvts.getRawParameterValue("trimEnd")->load();

        // Ensure start is always before end for drawing stability
        if (trimStartNorm >= trimEndNorm)
        {
            trimStartNorm = juce::jmax(0.0f, trimEndNorm - 0.001f);
        }
        double trimStartSec = trimStartNorm * totalDur;
        double trimEndSec = trimEndNorm * totalDur;

        // Draw waveform (semi-transparent)
        const juce::ScopedLock lock(audioBufferLock);
        if (selectedClip->audio.getNumSamples() > 0)
        {
            const float midY = canvas_p0.y + canvasHeight * 0.5f;
            for (int x = 0; x < (int)canvasWidth; ++x)
            {
                float startSampleF = ((float)x / canvasWidth) * selectedClip->audio.getNumSamples();
                float endSampleF =
                    ((float)(x + 1) / canvasWidth) * selectedClip->audio.getNumSamples();
                int startSample = (int)startSampleF;
                int endSample = (int)endSampleF;
                if (startSample >= endSample)
                    continue;

                juce::Range<float> minMax =
                    selectedClip->audio.findMinMax(0, startSample, endSample - startSample);
                float y1 = midY - minMax.getStart() * (canvasHeight * 0.4f);
                float y2 = midY - minMax.getEnd() * (canvasHeight * 0.4f);
                dl->AddLine(
                    ImVec2(canvas_p0.x + x, y1),
                    ImVec2(canvas_p0.x + x, y2),
                    IM_COL32(60, 80, 100, 180),
                    1.0f);
            }
        }

        // Draw word bars (colored, labeled)
        for (size_t i = 0; i < selectedClip->timings.size(); ++i)
        {
            const auto& w = selectedClip->timings[i];
            float       x0 = canvas_p0.x + (float)(w.startTimeSeconds / totalDur) * canvasWidth;
            float       x1 = canvas_p0.x + (float)(w.endTimeSeconds / totalDur) * canvasWidth;

            bool active = false;
            if (isPlaying)
            {
                double curSec = readPosition / juce::jmax(1.0, getSampleRate());
                active = (curSec >= w.startTimeSeconds && curSec < w.endTimeSeconds);
            }

            ImU32 bg = active ? IM_COL32(255, 180, 80, 100) : IM_COL32(80, 120, 160, 80);
            dl->AddRectFilled(ImVec2(x0, canvas_p0.y), ImVec2(x1, canvas_p1.y), bg);
            dl->AddLine(
                ImVec2(x0, canvas_p0.y),
                ImVec2(x0, canvas_p1.y),
                IM_COL32(200, 200, 200, 120),
                1.0f);

            // Word label
            ImGui::PushClipRect(ImVec2(x0, canvas_p0.y), ImVec2(x1, canvas_p1.y), true);
            dl->AddText(ImVec2(x0 + 2, canvas_p0.y + 2), IM_COL32_WHITE, w.word.toRawUTF8());
            ImGui::PopClipRect();
        }

        // Draw trim handles (10px wide, draggable)
        float trimX0 = canvas_p0.x + (float)(trimStartSec / totalDur) * canvasWidth;
        float trimX1 = canvas_p0.x + (float)(trimEndSec / totalDur) * canvasWidth;

        dl->AddRectFilled(
            ImVec2(trimX0 - 5, canvas_p0.y),
            ImVec2(trimX0 + 5, canvas_p1.y),
            IM_COL32(255, 255, 100, 180));
        dl->AddRectFilled(
            ImVec2(trimX1 - 5, canvas_p0.y),
            ImVec2(trimX1 + 5, canvas_p1.y),
            IM_COL32(255, 100, 100, 180));

        // Draw playhead (red line if playing)
        if (isPlaying)
        {
            double curTime = readPosition / juce::jmax(1.0, getSampleRate());
            float  playX = canvas_p0.x + (float)(curTime / totalDur) * canvasWidth;
            dl->AddLine(
                ImVec2(playX, canvas_p0.y),
                ImVec2(playX, canvas_p1.y),
                IM_COL32(255, 50, 50, 255),
                2.0f);
        }

        // Border
        dl->AddRect(canvas_p0, canvas_p1, IM_COL32(100, 100, 100, 255));

        // Interaction (trim handles + scrubbing)
        ImGui::SetCursorScreenPos(canvas_p0);
        ImGui::InvisibleButton("##timeline", ImVec2(canvasWidth, canvasHeight));

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
        {
            float mouseX = ImGui::GetIO().MousePos.x;
            float normalizedX = (mouseX - canvas_p0.x) / canvasWidth;
            normalizedX = juce::jlimit(0.0f, 1.0f, normalizedX);
            double t = normalizedX * totalDur;

            // Determine what to drag
            if (std::abs(mouseX - trimX0) < 10.0f && !draggingTrimEnd && !draggingScrub)
                draggingTrimStart = true;
            else if (std::abs(mouseX - trimX1) < 10.0f && !draggingTrimStart && !draggingScrub)
                draggingTrimEnd = true;
            else if (!draggingTrimStart && !draggingTrimEnd)
                draggingScrub = true;

            if (draggingTrimStart)
            {
                // Update parameter directly for bidirectional sync
                float newTrimStartNorm =
                    (float)juce::jlimit(0.0, (double)trimEndNorm, t / totalDur);
                auto* param =
                    dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("trimStart"));
                if (param)
                    param->setValueNotifyingHost(newTrimStartNorm);
            }
            else if (draggingTrimEnd)
            {
                // Update parameter directly for bidirectional sync
                float newTrimEndNorm =
                    (float)juce::jlimit((double)trimStartNorm, 1.0, t / totalDur);
                auto* param =
                    dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("trimEnd"));
                if (param)
                    param->setValueNotifyingHost(newTrimEndNorm);
            }
            else if (draggingScrub)
            {
                const juce::ScopedLock lock2(audioBufferLock);
                readPosition = juce::jlimit(
                    0.0, (double)(selectedClip->audio.getNumSamples() - 1), t * getSampleRate());
            }
        }

        if (ImGui::IsItemDeactivated())
        {
            draggingTrimStart = draggingTrimEnd = draggingScrub = false;
            onModificationEnded();
        }

        ImGui::Dummy(ImVec2(canvasWidth, canvasHeight));
    }

    // Action buttons (compact layout, no extra spacing)
    bool isBusy = (status == Status::Synthesizing);
    if (isBusy)
        ImGui::BeginDisabled();

    if (ImGui::Button("BAKE", ImVec2(itemWidth * 0.30f, 18)))
    {
        DBG("[TTS Performer] BAKE AUDIO button clicked!");
        juce::Logger::writeToLog("[TTS Performer] BAKE AUDIO button clicked!");

        juce::String textToSpeak = juce::String(uiTextBuffer);
        if (textToSpeak.isNotEmpty())
        {
            startSynthesis(textToSpeak);
        }
    }

    if (isBusy)
        ImGui::EndDisabled();

    // Live input telemetry block (compact, minimal spacing)

    // Current clip and playback state
    if (selectedClip)
    {
        juce::String clipInfo = "Clip: " + selectedClip->name + " (" +
                                juce::String(selectedClip->durationSeconds, 2) + "s, " +
                                juce::String(selectedClip->timings.size()) + " words)";
        ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "%s", clipInfo.toRawUTF8());
        ImGui::Text(
            "Playback: %s | Word: %d/%d",
            isPlaying ? "PLAYING" : "STOPPED",
            currentWordIndex + 1,
            (int)getActiveTimings().size());
    }
    else
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No clip selected");
    }
    
    ImGui::PopItemWidth();
    ImGui::PopID();
}

void TTSPerformerModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Helper lambda to draw a pin by its virtual parameter ID
    auto drawInputPin = [&](const char* paramId, const char* label) {
        int busIdx, chanInBus;
        if (getParamRouting(paramId, busIdx, chanInBus))
        {
            helpers.drawAudioInputPin(
                label, getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus));
        }
    };

    // Helper lambda for parallel pins
    auto drawParallelPin =
        [&](const char* inParamId, const char* inLabel, const char* outLabel, int outChannel) {
            int busIdx = -1, chanInBus = -1;
            int inChannel = -1;
            if (inParamId != nullptr && getParamRouting(inParamId, busIdx, chanInBus))
            {
                inChannel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            }
            helpers.drawParallelPins(inLabel, inChannel, outLabel, outChannel);
        };

    // --- Draw Global Inputs & Outputs ---
    // Rate Mod and Gate Mod are now inline, not drawn here
    drawParallelPin(paramIdTriggerMod, "Trigger", "EOP Gate", 2);
    drawInputPin(paramIdResetMod, "Reset");              // No parallel output
    drawInputPin(paramIdRandomizeMod, "Randomize Trig"); // No parallel output

    ImGui::Spacing(); // Add visual separation

    // --- Draw Playback Control Inputs ---
    // Trim Start Mod, Trim End Mod, Speed Mod, and Pitch Mod are now inline, not drawn here

    // --- Draw Per-Word Inputs & Outputs (Dynamically) ---
    int wordCount = selectedClip ? juce::jmin((int)selectedClip->timings.size(), 16) : 0;
    if (wordCount > 0)
    {
        ImGui::Spacing(); // Add visual separation
    }

    for (int i = 0; i < wordCount; ++i)
    {
        juce::String word = selectedClip->timings[i].word.substring(0, 8);

        juce::String inParamId = "word" + juce::String(i + 1) + "_trig_mod";
        juce::String inLabel = "Word " + juce::String(i + 1) + " Trig";

        juce::String outGateLabel = word + " Gate";
        juce::String outTrigLabel = word + " Trig";

        // Draw Word Trigger input on the left, and its corresponding Gate output on the right
        drawParallelPin(
            inParamId.toRawUTF8(), inLabel.toRawUTF8(), outGateLabel.toRawUTF8(), 3 + i);

        // Draw the per-word Trigger output on its own line below
        drawParallelPin(nullptr, nullptr, outTrigLabel.toRawUTF8(), 19 + i);
    }
}
#endif

// --- Timing Data Access Method Implementations ---

bool TTSPerformerModuleProcessor::isWordActiveAtTime(double timeInSeconds) const
{
    const juce::ScopedLock lock(audioBufferLock);

    const auto& timingsA = getActiveTimings();
    for (const auto& word : timingsA)
    {
        if (timeInSeconds >= word.startTimeSeconds && timeInSeconds <= word.endTimeSeconds)
        {
            return true;
        }
    }
    return false;
}

const WordTiming* TTSPerformerModuleProcessor::getCurrentWordAtTime(double timeInSeconds) const
{
    const juce::ScopedLock lock(audioBufferLock);

    const auto& timingsB = getActiveTimings();
    for (const auto& word : timingsB)
    {
        if (timeInSeconds >= word.startTimeSeconds && timeInSeconds <= word.endTimeSeconds)
        {
            return &word;
        }
    }
    return nullptr;
}

const PhonemeTiming* TTSPerformerModuleProcessor::getCurrentPhonemeAtTime(
    double timeInSeconds) const
{
    const juce::ScopedLock lock(audioBufferLock);

    const auto& timingsC = getActiveTimings();
    for (const auto& word : timingsC)
    {
        for (const auto& phoneme : word.phonemes)
        {
            if (timeInSeconds >= phoneme.startTimeSeconds &&
                timeInSeconds <= phoneme.endTimeSeconds)
            {
                return &phoneme;
            }
        }
    }
    return nullptr;
}

// --- Phase 3: Sequencer Helper Method Implementations ---

// processSequencerInputs method removed - functionality integrated into processBlock

void TTSPerformerModuleProcessor::advanceSequencerStep()
{
    // Simplified: just advance word index (used by rate-based auto-advance in processBlock)
    // This function is now a stub since auto-advance logic is in processBlock
    currentWordIndex++;
    if (selectedClip && currentWordIndex >= (int)selectedClip->timings.size())
    {
        currentWordIndex = 0; // Always loop
    }
}

void TTSPerformerModuleProcessor::resetSequencer()
{
    currentWordIndex = 0;
    currentPhonemeIndex = 0;
    sequencerStartTime = 0.0;
    sequencerActive = false;

    DBG("[TTS Performer] Sequencer reset to beginning");
}

int TTSPerformerModuleProcessor::getSequencerCurrentIndex() const
{
    // Simplified: just return current word index
    return currentWordIndex;
}

double TTSPerformerModuleProcessor::getSequencerCurrentDuration() const
{
    const auto& timingsE = getActiveTimings();
    if (timingsE.empty())
        return 0.0;
    if (currentWordIndex >= (int)timingsE.size())
        return 0.0;

    const auto& currentWord = timingsE[(size_t)currentWordIndex];
    return currentWord.durationSeconds;
}

// processSequencerPlayback method removed - functionality integrated into processBlock

// processNormalPlayback method removed - functionality integrated into processBlock

void TTSPerformerModuleProcessor::handleLoopMode()
{
    // Stub - loop mode is now handled by loopParam (bool) in processBlock
}

// --- Phase 4: SynthesisThread Cache Management Implementation ---

void TTSPerformerModuleProcessor::SynthesisThread::clearVoiceCache()
{
    const juce::ScopedLock lock(cacheLock);
    voiceCache.clear();
    DBG("[TTS Performer] Voice cache cleared - " + juce::String(voiceCache.size()) +
        " voices removed");
}

void TTSPerformerModuleProcessor::SynthesisThread::setMaxCachedVoices(int maxVoices)
{
    const juce::ScopedLock lock(cacheLock);
    maxCachedVoices = juce::jlimit(1, 10, maxVoices);
    DBG("[TTS Performer] Max cached voices set to: " + juce::String(maxCachedVoices));

    // Remove excess voices if needed
    while (voiceCache.size() > maxCachedVoices)
    {
        removeOldestVoice();
    }
}

void TTSPerformerModuleProcessor::SynthesisThread::updateMaxCachedVoicesFromParameter()
{
    // Stub - max cached voices is now a fixed constant (no UI control)
}

int TTSPerformerModuleProcessor::SynthesisThread::getCacheSize() const
{
    const juce::ScopedLock lock(cacheLock);
    return (int)voiceCache.size();
}

bool TTSPerformerModuleProcessor::SynthesisThread::isVoiceCached(
    const juce::String& modelPath) const
{
    const juce::ScopedLock lock(cacheLock);
    juce::String           key = getCacheKey(modelPath);
    auto                   it = voiceCache.find(key);
    return (it != voiceCache.end() && it->second.isValid);
}

void TTSPerformerModuleProcessor::SynthesisThread::addVoiceToCache(
    const juce::String& modelPath,
    const juce::String& configPath)
{
    const juce::ScopedLock lock(cacheLock);
    juce::String           key = getCacheKey(modelPath);

    // Check if we need to remove old voices
    while (voiceCache.size() >= maxCachedVoices)
    {
        removeOldestVoice();
    }

    // Add new voice to cache
    voiceCache[key] = CachedVoice(modelPath, configPath);

    DBG("[TTS Performer] Voice added to cache: " + modelPath +
        " (Cache size: " + juce::String(voiceCache.size()) + ")");
}

void TTSPerformerModuleProcessor::SynthesisThread::removeOldestVoice()
{
    if (voiceCache.empty())
        return;

    auto oldest = voiceCache.begin();
    for (auto it = voiceCache.begin(); it != voiceCache.end(); ++it)
    {
        if (it->second.lastUsed < oldest->second.lastUsed)
        {
            oldest = it;
        }
    }

    DBG("[TTS Performer] Removing oldest voice from cache: " + oldest->first);
    voiceCache.erase(oldest);
}

juce::String TTSPerformerModuleProcessor::SynthesisThread::getCacheKey(
    const juce::String& modelPath) const
{
    // Use the filename as the cache key (without path)
    return juce::File(modelPath).getFileNameWithoutExtension();
}

#if defined(PRESET_CREATOR_UI)
// --- Phase 5: Waveform Visualization Implementation ---

void TTSPerformerModuleProcessor::drawWaveform(
    void*         drawListPtr,
    const ImVec2& pos,
    const ImVec2& size)
{
    ImDrawList* drawList = static_cast<ImDrawList*>(drawListPtr);

    if (bakedAudioBuffer.getNumSamples() == 0)
        return;

    const int    numSamples = bakedAudioBuffer.getNumSamples();
    const float* audioData = bakedAudioBuffer.getReadPointer(0);

    // Calculate samples per pixel for downsampling
    const int samplesPerPixel = juce::jmax(1, numSamples / (int)size.x);
    const int numPixels = (int)size.x;

    // Center line for zero crossing
    const float centerY = pos.y + size.y * 0.5f;

    // Draw waveform as connected lines
    std::vector<ImVec2> waveformPoints;
    waveformPoints.reserve(numPixels * 2);

    for (int x = 0; x < numPixels; ++x)
    {
        // Calculate sample range for this pixel
        const int startSample = x * samplesPerPixel;
        const int endSample = juce::jmin(startSample + samplesPerPixel, numSamples);

        // Find min/max in this range
        float minVal = 0.0f;
        float maxVal = 0.0f;

        for (int s = startSample; s < endSample; ++s)
        {
            const float sample = audioData[s];
            minVal = juce::jmin(minVal, sample);
            maxVal = juce::jmax(maxVal, sample);
        }

        // Convert to screen coordinates
        const float screenX = pos.x + x;
        const float minY = centerY - (minVal * size.y * 0.5f);
        const float maxY = centerY - (maxVal * size.y * 0.5f);

        // Add points for min and max
        waveformPoints.push_back(ImVec2(screenX, minY));
        waveformPoints.push_back(ImVec2(screenX, maxY));
    }

    // Draw the waveform
    if (waveformPoints.size() >= 2)
    {
        // Draw as a filled shape for better visibility
        std::vector<ImVec2> fillPoints;
        fillPoints.reserve(waveformPoints.size() + 2);

        // Add bottom edge
        fillPoints.push_back(ImVec2(pos.x, centerY));

        // Add waveform points
        for (size_t i = 0; i < waveformPoints.size(); i += 2)
        {
            fillPoints.push_back(waveformPoints[i]); // min point
        }

        // Add top edge (reverse order)
        for (int i = (int)waveformPoints.size() - 1; i >= 1; i -= 2)
        {
            fillPoints.push_back(waveformPoints[i]); // max point
        }

        // Close the shape
        fillPoints.push_back(ImVec2(pos.x + size.x, centerY));

        // Draw filled waveform
        drawList->AddConvexPolyFilled(
            fillPoints.data(),
            (int)fillPoints.size(),
            IM_COL32(100, 150, 255, 200)); // Blue with transparency

        // Draw center line
        drawList->AddLine(
            ImVec2(pos.x, centerY),
            ImVec2(pos.x + size.x, centerY),
            IM_COL32(100, 100, 100, 100),
            1.0f);

        // Phase 5.2: Draw word and phoneme boundaries
        drawWordBoundaries(drawList, pos, size, numSamples, centerY);
    }
}

void TTSPerformerModuleProcessor::drawWordBoundaries(
    void*         drawListPtr,
    const ImVec2& pos,
    const ImVec2& size,
    int           numSamples,
    float         centerY)
{
    ImDrawList* drawList = static_cast<ImDrawList*>(drawListPtr);

    const auto& timingsF = getActiveTimings();
    if (timingsF.empty() || numSamples == 0)
        return;

    // Calculate time per sample (assuming standard sample rate)
    const double sampleRate = getSampleRate();
    const double durationSeconds = numSamples / sampleRate;

    // Define colors for different words (cycle through a palette)
    const ImU32 wordColors[] = {
        IM_COL32(255, 100, 100, 120), // Red
        IM_COL32(100, 255, 100, 120), // Green
        IM_COL32(100, 100, 255, 120), // Blue
        IM_COL32(255, 255, 100, 120), // Yellow
        IM_COL32(255, 100, 255, 120), // Magenta
        IM_COL32(100, 255, 255, 120), // Cyan
    };
    const int numColors = sizeof(wordColors) / sizeof(wordColors[0]);

    // Draw word boundaries
    for (size_t i = 0; i < timingsF.size(); ++i)
    {
        const auto& word = timingsF[i];

        // Calculate pixel positions for this word
        const float startX = pos.x + (word.startTimeSeconds / durationSeconds) * size.x;
        const float endX = pos.x + (word.endTimeSeconds / durationSeconds) * size.x;
        const float wordWidth = endX - startX;

        // Skip words that are too small to see
        if (wordWidth < 2.0f)
            continue;

        // Choose color for this word
        const ImU32 wordColor = wordColors[i % numColors];

        // Draw word background rectangle
        drawList->AddRectFilled(ImVec2(startX, pos.y + 2), ImVec2(endX, centerY - 2), wordColor);

        // Draw word border
        drawList->AddRect(
            ImVec2(startX, pos.y + 2),
            ImVec2(endX, centerY - 2),
            IM_COL32(255, 255, 255, 200),
            1.0f);

        // Draw word text if there's enough space
        if (wordWidth > 20.0f)
        {
            const char*  wordText = word.word.toRawUTF8();
            const ImVec2 textSize = ImGui::CalcTextSize(wordText);

            // Center text horizontally and vertically in the word area
            const float textX = startX + (wordWidth - textSize.x) * 0.5f;
            const float textY = pos.y + 2 + (centerY - pos.y - 4 - textSize.y) * 0.5f;

            // Only draw text if it fits
            if (textX >= startX && textX + textSize.x <= endX)
            {
                drawList->AddText(ImVec2(textX, textY), IM_COL32(255, 255, 255, 255), wordText);
            }
        }

        // Phase 5.2: Draw phoneme boundaries within this word
        if (wordWidth > 40.0f && !word.phonemes.empty()) // Only if word is large enough
        {
            drawPhonemeBoundaries(drawList, pos, size, word, startX, wordWidth, centerY);
        }
    }

    // Phase 5.3: Draw playhead indicator
    drawPlayheadIndicator(drawList, pos, size, numSamples, centerY);
}

void TTSPerformerModuleProcessor::drawPhonemeBoundaries(
    void*             drawListPtr,
    const ImVec2&     pos,
    const ImVec2&     size,
    const WordTiming& word,
    float             wordStartX,
    float             wordWidth,
    float             centerY)
{
    ImDrawList* drawList = static_cast<ImDrawList*>(drawListPtr);

    if (word.phonemes.empty())
        return;

    const double wordDuration = word.endTimeSeconds - word.startTimeSeconds;
    const ImU32  phonemeColor = IM_COL32(255, 255, 255, 60); // Semi-transparent white

    // Draw vertical lines for phoneme boundaries
    for (size_t i = 0; i < word.phonemes.size(); ++i)
    {
        const auto& phoneme = word.phonemes[i];

        // Calculate relative position within the word
        const double relativeStart =
            (phoneme.startTimeSeconds - word.startTimeSeconds) / wordDuration;
        const float phonemeX = wordStartX + relativeStart * wordWidth;

        // Draw vertical line for phoneme boundary
        drawList->AddLine(
            ImVec2(phonemeX, centerY - 10), ImVec2(phonemeX, centerY + 10), phonemeColor, 1.0f);

        // Draw phoneme symbol if there's enough space
        if (wordWidth > 60.0f) // Only for larger words
        {
            const char*  phonemeText = phoneme.phoneme.toRawUTF8();
            const ImVec2 textSize = ImGui::CalcTextSize(phonemeText);

            // Position phoneme text above the boundary line
            const float textX = phonemeX - textSize.x * 0.5f;
            const float textY = centerY - 15;

            // Only draw if text fits within the word bounds
            if (textX >= wordStartX && textX + textSize.x <= wordStartX + wordWidth)
            {
                drawList->AddText(ImVec2(textX, textY), IM_COL32(200, 200, 200, 180), phonemeText);
            }
        }
    }
}

void TTSPerformerModuleProcessor::drawPlayheadIndicator(
    void*         drawListPtr,
    const ImVec2& pos,
    const ImVec2& size,
    int           numSamples,
    float         centerY)
{
    ImDrawList* drawList = static_cast<ImDrawList*>(drawListPtr);

    if (numSamples == 0 || !isPlaying)
        return;

    // Calculate playhead position in pixels
    const float playheadRatio = (float)readPosition / (float)numSamples;
    const float playheadX = pos.x + playheadRatio * size.x;

    // Only draw if playhead is within the visible area
    if (playheadX >= pos.x && playheadX <= pos.x + size.x)
    {
        // Draw main playhead line (bright yellow/orange)
        drawList->AddLine(
            ImVec2(playheadX, pos.y),
            ImVec2(playheadX, pos.y + size.y),
            IM_COL32(255, 200, 0, 255), // Bright yellow-orange
            2.0f                        // Thick line for visibility
        );

        // Draw playhead shadow for depth
        drawList->AddLine(
            ImVec2(playheadX - 1, pos.y),
            ImVec2(playheadX - 1, pos.y + size.y),
            IM_COL32(255, 255, 255, 100), // White shadow
            1.0f);

        // Draw playhead indicator triangle at the top
        const float  triangleSize = 8.0f;
        const ImVec2 triangleTop(playheadX, pos.y - triangleSize);
        const ImVec2 triangleLeft(playheadX - triangleSize * 0.5f, pos.y);
        const ImVec2 triangleRight(playheadX + triangleSize * 0.5f, pos.y);

        drawList->AddTriangleFilled(
            triangleTop, triangleLeft, triangleRight, IM_COL32(255, 200, 0, 255));
        drawList->AddTriangle(
            triangleTop, triangleLeft, triangleRight, IM_COL32(255, 255, 255, 200), 1.0f);

        // Draw time indicator text above the playhead
        const double       currentTimeSeconds = readPosition / getSampleRate();
        const juce::String timeText = juce::String::formatted("%.2fs", currentTimeSeconds);
        const char*        timeCStr = timeText.toRawUTF8();
        const ImVec2       textSize = ImGui::CalcTextSize(timeCStr);

        // Position text above the triangle
        const float textX = playheadX - textSize.x * 0.5f;
        const float textY = pos.y - triangleSize - textSize.y - 4.0f;

        // Draw text background
        drawList->AddRectFilled(
            ImVec2(textX - 2, textY - 1),
            ImVec2(textX + textSize.x + 2, textY + textSize.y + 1),
            IM_COL32(0, 0, 0, 180) // Semi-transparent black background
        );

        // Draw time text
        drawList->AddText(ImVec2(textX, textY), IM_COL32(255, 255, 255, 255), timeCStr);

        // Phase 5.3: Highlight current word/phoneme based on playhead position
        highlightCurrentWord(drawList, pos, size, centerY, currentTimeSeconds);
    }
}

void TTSPerformerModuleProcessor::highlightCurrentWord(
    void*         drawListPtr,
    const ImVec2& pos,
    const ImVec2& size,
    float         centerY,
    double        currentTimeSeconds)
{
    ImDrawList* drawList = static_cast<ImDrawList*>(drawListPtr);

    // Find the current word being played
    const auto& timingsG = getActiveTimings();
    for (size_t i = 0; i < timingsG.size(); ++i)
    {
        const auto& word = timingsG[i];

        if (currentTimeSeconds >= word.startTimeSeconds &&
            currentTimeSeconds <= word.endTimeSeconds)
        {
            // Calculate word position
            const double durationSeconds =
                (double)bakedAudioBuffer.getNumSamples() / getSampleRate();
            const float startX = pos.x + (word.startTimeSeconds / durationSeconds) * size.x;
            const float endX = pos.x + (word.endTimeSeconds / durationSeconds) * size.x;

            // Draw highlight overlay for current word
            drawList->AddRectFilled(
                ImVec2(startX, pos.y),
                ImVec2(endX, pos.y + size.y),
                IM_COL32(255, 255, 0, 30) // Yellow highlight with low opacity
            );

            // Draw thicker border around current word
            drawList->AddRect(
                ImVec2(startX, pos.y),
                ImVec2(endX, pos.y + size.y),
                IM_COL32(255, 255, 0, 150), // Yellow border
                2.0f                        // Thick border
            );

            // Find current phoneme within the word
            for (const auto& phoneme : word.phonemes)
            {
                if (currentTimeSeconds >= phoneme.startTimeSeconds &&
                    currentTimeSeconds <= phoneme.endTimeSeconds)
                {
                    // Calculate phoneme position
                    const double wordDuration = word.endTimeSeconds - word.startTimeSeconds;
                    const double relativeStart =
                        (phoneme.startTimeSeconds - word.startTimeSeconds) / wordDuration;
                    const double relativeEnd =
                        (phoneme.endTimeSeconds - word.startTimeSeconds) / wordDuration;

                    const float phonemeStartX = startX + relativeStart * (endX - startX);
                    const float phonemeEndX = startX + relativeEnd * (endX - startX);

                    // Draw phoneme highlight
                    drawList->AddRectFilled(
                        ImVec2(phonemeStartX, centerY - 15),
                        ImVec2(phonemeEndX, centerY + 15),
                        IM_COL32(255, 255, 255, 40) // White highlight for phoneme
                    );

                    break; // Only highlight the current phoneme
                }
            }
            break; // Only highlight the current word
        }
    }
}

bool TTSPerformerModuleProcessor::handleWaveformInteraction(
    const ImVec2& pos,
    const ImVec2& size,
    int           numSamples)
{
    if (numSamples == 0)
        return false;

    // Get mouse position and check if it's over the waveform area
    const ImVec2 mousePos = ImGui::GetMousePos();
    const bool   isMouseOverWaveform =
        (mousePos.x >= pos.x && mousePos.x <= pos.x + size.x && mousePos.y >= pos.y &&
         mousePos.y <= pos.y + size.y);

    // Track interaction state
    static bool isDragging = false;
    static bool wasPlayingBeforeDrag = false;

    // Handle mouse click and drag
    if (isMouseOverWaveform)
    {
        // Show hover cursor
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Display hover tooltip with time information
        if (!isDragging && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y)))
        {
            const float  normalizedPos = (mousePos.x - pos.x) / size.x;
            const int    hoverSample = (int)(normalizedPos * numSamples);
            const double hoverTime = (double)hoverSample / getSampleRate();

            // Find word at hover position
            const WordTiming* hoverWord = nullptr;
            const auto&       timingsH = getActiveTimings();
            for (const auto& word : timingsH)
            {
                if (hoverTime >= word.startTimeSeconds && hoverTime <= word.endTimeSeconds)
                {
                    hoverWord = &word;
                    break;
                }
            }

            // Create tooltip text
            juce::String tooltipText = juce::String::formatted("Time: %.2fs", hoverTime);
            if (hoverWord != nullptr)
            {
                tooltipText += "\nWord: \"" + hoverWord->word + "\"";
            }

            ImGui::BeginTooltip();
            ImGui::TextUnformatted(tooltipText.toRawUTF8());
            ImGui::EndTooltip();
        }

        // Start dragging on mouse down
        if (ImGui::IsMouseClicked(0))
        {
            isDragging = true;
            wasPlayingBeforeDrag = isPlaying;

            // Pause playback during scrubbing
            if (isPlaying)
            {
                isPlaying = false;
            }
        }
    }

    // Handle dragging
    if (isDragging)
    {
        // Calculate new playback position based on mouse X
        const float normalizedPos = juce::jlimit(0.0f, 1.0f, (mousePos.x - pos.x) / size.x);
        const int   newPosition = (int)(normalizedPos * numSamples);

        // Update the AUDIO playhead directly
        readPosition = (double)newPosition;

        // Release drag on mouse up
        if (ImGui::IsMouseReleased(0))
        {
            isDragging = false;

            // Resume playback if it was playing before
            if (wasPlayingBeforeDrag)
            {
                isPlaying = true;
            }

            return true; // Position was changed
        }
    }

    // Reset dragging if mouse is released outside
    if (!ImGui::IsMouseDown(0))
    {
        if (isDragging)
        {
            isDragging = false;

            // Resume playback if it was playing before
            if (wasPlayingBeforeDrag)
            {
                isPlaying = true;
            }
        }
    }

    return false;
}

#endif