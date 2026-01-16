#pragma once

#include "../graph/ModularSynthProcessor.h"
#include "../assets/SampleBank.h"
#include "../voices/SampleVoiceProcessor.h"
#include "../dsp/TimePitchProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

class SampleLoaderModuleProcessor : public ModuleProcessor
{
public:
    // --- Position Parameter IDs ---
    static constexpr auto paramIdPosition = "position";
    static constexpr auto paramIdPositionMod = "position_mod";
    static constexpr auto paramIdRelPosMod = "relativePositionMod";

    SampleLoaderModuleProcessor();
    ~SampleLoaderModuleProcessor() override = default;

    const juce::String getName() const override { return "sample_loader"; }

    // --- Audio Processing ---
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void reset() override;
    void forceStop(); // Force stop playback (used after patch load)

    // --- Transport Sync ---
    // Override to ignore transport when this module is the timeline master (prevents feedback
    // loops)
    void setTimingInfo(const TransportState& state) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- Required by ModuleProcessor ---
    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Rhythm introspection for BPM Monitor
    std::optional<RhythmInfo> getRhythmInfo() const override;

    // --- Sample Loading ---
    void         loadSample(const juce::File& file);
    void         loadSample(const juce::String& filePath);
    void         randomizeSample();
    juce::String getCurrentSampleName() const;
    bool         hasSampleLoaded() const;

    // (Removed SoundTouch controls; using Rubber Band via TimePitchProcessor)

    // --- Debug and Monitoring ---
    void setDebugOutput(bool enabled);
    void logCurrentSettings() const;

    // --- Parameter Layout ---
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded,
        const NodePinHelpers*                                   pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    // Parameter bus contract implementation (must be available in Collider too)
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus)
        const override;
    bool   usesCustomPinLayout() const override { return true; }
    ImVec2 getCustomNodeSize() const override { return ImVec2(360.0f, 0.0f); }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
        case 0:
            return "Pitch Mod";
        case 1:
            return "Speed Mod";
        case 2:
            return "Gate Mod";
        case 3:
            return "Trigger Mod";
        case 4:
            return "Range Start Mod";
        case 5:
            return "Range End Mod";
        case 6:
            return "Randomize Trig";
        case 7:
            return "Position Mod";
        default:
            return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
        case 0:
            return "Out L";
        case 1:
            return "Out R";
        default:
            return juce::String("Out ") + juce::String(channel + 1);
        }
    }

    std::vector<DynamicPinInfo> getDynamicOutputPins() const override
    {
        return {{"Out L", 0, PinDataType::Audio}, {"Out R", 1, PinDataType::Audio}};
    }

    // CRITICAL: Accept multi-bus layout (like TTS Performer)
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        // Accept any layout as long as we have at least the minimum channels
        if (layouts.getMainInputChannelSet().isDisabled())
            return false;
        if (layouts.getMainOutputChannelSet().isDisabled())
            return false;
        return true;
    }

    // --- Spectrogram Access ---
    juce::Image getSpectrogramImage()
    {
        const juce::ScopedLock lock(imageLock);
        return spectrogramImage;
    }

private:
    // --- ADD THIS STATE VARIABLE ---
    std::atomic<bool> isPlaying{false};

    // --- APVTS ---
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>*                engineParam{nullptr}; // 0: rubberband, 1: naive

    // --- Sample Management ---
    // CRITICAL: All sample-related data must be protected by processorSwapLock
    // This lock protects: currentSample, sampleProcessor, sampleSampleRate, sampleDurationSeconds
    std::shared_ptr<SampleBank::Sample>   currentSample;
    std::unique_ptr<SampleVoiceProcessor> sampleProcessor;
    std::atomic<SampleVoiceProcessor*>    newSampleProcessor{nullptr};
    juce::CriticalSection                 processorSwapLock;
    std::unique_ptr<SampleVoiceProcessor> processorToDelete;
    juce::String                          currentSampleName;
    juce::String                          currentSamplePath;

    // Thread-safe sample metadata (protected by processorSwapLock, but atomic for quick reads)
    std::atomic<double> sampleDurationSeconds{0.0};
    std::atomic<int>    sampleSampleRate{0};

    // Timeline reporting state (atomic for thread-safe access)
    std::atomic<double> reportPosition{0.0};
    std::atomic<double> reportDuration{0.0};
    std::atomic<bool>   reportActive{false};

    // Trigger edge detection for trigger_mod
    bool lastTriggerHigh{false};
    bool lastRandomizeTriggerHigh{false};

    // Deferred sample loading queue
    std::atomic<bool>     hasPendingRandomSample{false};
    juce::String          pendingRandomSamplePath;
    juce::CriticalSection pendingRandomLock;
    std::atomic<bool>     shouldResumeAfterRandomLoad{false};
    std::atomic<bool>     processorIsRendering{false};

    bool queueRandomSampleFromCurrentFolder(const juce::String& sourceTag);

#if defined(PRESET_CREATOR_UI)
    // Keep a persistent chooser so async callback remains valid
    std::unique_ptr<juce::FileChooser> fileChooser;
#endif

    // Rubber Band is configured in TimePitchProcessor; keep no per-node ST params

    // --- Debug ---
    bool debugOutput{false};

    // --- Spectrogram Data ---
    juce::Image           spectrogramImage;
    juce::CriticalSection imageLock;

    // --- Range Parameters ---
    std::atomic<float>* rangeStartParam{nullptr};
    std::atomic<float>* rangeEndParam{nullptr};
    std::atomic<float>* rangeStartModParam{nullptr};
    std::atomic<float>* rangeEndModParam{nullptr};
    double              readPosition{0.0};

    // --- Relative Modulation Parameters ---
    std::atomic<float>* relativeSpeedModParam{nullptr};
    std::atomic<float>* relativePitchModParam{nullptr};
    std::atomic<float>* relativeGateModParam{nullptr};
    std::atomic<float>* relativeRangeStartModParam{nullptr};
    std::atomic<float>* relativeRangeEndModParam{nullptr};

    // --- Position Parameters ---
    std::atomic<float>* positionParam{nullptr};
    std::atomic<float>* positionModParam{nullptr};
    std::atomic<float>* relativePositionModParam{nullptr};

    // --- Transport Sync ---
    std::atomic<float>*         syncParam{nullptr};
    juce::AudioParameterChoice* syncModeParam{
        nullptr}; // 0 = Relative (range-based), 1 = Absolute (1:1 time)
    std::atomic<bool> syncToTransport{true};

    // For detecting manual slider movement vs playback update
    float  lastUiPosition{0.0f};
    float  lastCvPosition{0.0f};              // Track last CV value to detect changes
    double lastReadPosition{0.0};             // To detect loops for global reset
    double lastSyncedTransportPosition{-1.0}; // Track last synced position to avoid constant seeks

    // Manual scrubbing state (for sync override)
    std::atomic<bool> manualScrubPending{
        false}; // True when user manually scrubbed and sync should be temporarily ignored
    std::atomic<int> manualScrubBlocksRemaining{
        0}; // Number of blocks to skip sync after manual scrub
    std::atomic<double> currentBpm{120.0};
    std::atomic<bool>   lastTransportPlaying{false};
    TransportCommand    lastTransportCommand{
        TransportCommand::Stop}; // Track last command to detect transitions
    double pausedPosition{-1.0};    // Track position when paused (for resume in unsynced mode)

    // --- Parameter References ---
    // Parameters are accessed directly via apvts.getRawParameterValue()

    // --- Internal Methods ---
    void updateSoundTouchSettings();
    void createSampleProcessor();
    void generateSpectrogram();
    void generateSpectrogram(
        std::shared_ptr<SampleBank::Sample>
            sample); // Thread-safe version that takes sample directly

    // Timeline reporting interface (for Timeline Sync feature)
    bool   canProvideTimeline() const override;
    double getTimelinePositionSeconds() const override;
    double getTimelineDurationSeconds() const override;
    bool   isTimelineActive() const override;
};
