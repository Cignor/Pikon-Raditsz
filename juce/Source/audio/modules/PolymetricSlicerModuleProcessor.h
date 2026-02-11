#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class PolymetricSlicerModuleProcessor : public ModuleProcessor
{
public:
    static constexpr int NUM_TIMELINES = 4;
    static constexpr int MAX_STEPS = 32;
    static constexpr int MAX_ROWS = 12;

    // Output channel layout:
    //   0..7  = 4 stereo pairs of sliced audio (Timeline 1 L/R, Timeline 2 L/R, ...)
    //   8..19 = 4 × 3 CV outputs (per timeline: Pitch, Gate, Trigger)
    static constexpr int AUDIO_OUT_CHANNELS = NUM_TIMELINES * 2; // 8
    static constexpr int CV_OUT_PER_TIMELINE = 3;                // pitch, gate, trigger
    static constexpr int CV_OUT_CHANNELS = NUM_TIMELINES * CV_OUT_PER_TIMELINE;     // 12
    static constexpr int TOTAL_OUT_CHANNELS = AUDIO_OUT_CHANNELS + CV_OUT_CHANNELS; // 20

    // Input channel layout:
    //   0..7  = 4 stereo pairs of audio input (one per timeline)
    //   8..11 = 4 modulation inputs (bars mod per timeline — future use)
    static constexpr int AUDIO_IN_CHANNELS = NUM_TIMELINES * 2;                   // 8
    static constexpr int MOD_IN_CHANNELS = NUM_TIMELINES;                         // 4
    static constexpr int TOTAL_IN_CHANNELS = AUDIO_IN_CHANNELS + MOD_IN_CHANNELS; // 12

    PolymetricSlicerModuleProcessor();
    ~PolymetricSlicerModuleProcessor() override = default;

    const juce::String getName() const override { return "polymetric_slicer"; }

    void prepareToPlay(double newSampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // State management
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree&) override;

    // Pin labels
    juce::String getAudioOutputLabel(int channel) const override;
    juce::String getAudioInputLabel(int channel) const override;

#if defined(PRESET_CREATOR_UI)
    ImVec2 getCustomNodeSize() const override { return ImVec2(560.0f, 0.0f); }

    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;

    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif

protected:
    void setTimingInfo(const TransportState& state) override;
    void forceStop() override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // --- Grid State (NOT in APVTS — stored in extra state tree) ---
    // gridState[timeline][row][step] = on/off
    std::array<std::array<std::array<bool, MAX_STEPS>, MAX_ROWS>, NUM_TIMELINES> gridState{};

    // --- Per-Timeline Playback ---
    std::array<std::atomic<int>, NUM_TIMELINES> currentStep{};
    std::array<double, NUM_TIMELINES>           phase{};

    // --- Transport ---
    double           sampleRate{44100.0};
    TransportState   m_currentTransport;
    bool             wasPlaying{false};
    TransportCommand lastTransportCommand{TransportCommand::Stop};

    // --- Trigger pulse state (per timeline) ---
    std::array<int, NUM_TIMELINES> pendingTriggerSamples{};

    // --- Gate fade state (per timeline) ---
    std::array<bool, NUM_TIMELINES>  previousGateOn{};
    std::array<float, NUM_TIMELINES> gateFadeProgress{};
    static constexpr float           GATE_FADE_TIME_MS = 5.0f;

#if defined(PRESET_CREATOR_UI)
    // --- UI State ---
    int activeTimeline{0}; // Which timeline tab is selected (0-3)

    // --- Preset Management (mirrors MultiBandShaperModuleProcessor pattern) ---
    juce::String activePresetName;
    int          selectedPresetIndex{-1};
    int          selectedStandardPresetIndex{0};
    char         presetNameBuffer[128] = "";
#endif
};
