#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class SequentialSwitchModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdThreshold1 = "threshold1";
    static constexpr auto paramIdThreshold2 = "threshold2";
    static constexpr auto paramIdThreshold3 = "threshold3";
    static constexpr auto paramIdThreshold4 = "threshold4";
    
    // Virtual modulation target IDs (no APVTS parameters required)
    static constexpr auto paramIdThreshold1Mod = "threshold1_mod";
    static constexpr auto paramIdThreshold2Mod = "threshold2_mod";
    static constexpr auto paramIdThreshold3Mod = "threshold3_mod";
    static constexpr auto paramIdThreshold4Mod = "threshold4_mod";

    SequentialSwitchModuleProcessor();
    ~SequentialSwitchModuleProcessor() override = default;

    const juce::String getName() const override { return "sequential_switch"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth,
                               const std::function<bool(const juce::String& paramId)>& isParamModulated,
                               const std::function<void()>& onModificationEnded,
                               const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
    bool usesCustomPinLayout() const override { return true; }
#endif

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // Cached parameter pointers
    std::atomic<float>* threshold1Param { nullptr };
    std::atomic<float>* threshold2Param { nullptr };
    std::atomic<float>* threshold3Param { nullptr };
    std::atomic<float>* threshold4Param { nullptr };

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputWaveform;
        std::array<std::atomic<float>, waveformPoints> output1Waveform;
        std::array<std::atomic<float>, waveformPoints> output2Waveform;
        std::array<std::atomic<float>, waveformPoints> output3Waveform;
        std::array<std::atomic<float>, waveformPoints> output4Waveform;
        std::atomic<float> currentThreshold1 { 0.5f };
        std::atomic<float> currentThreshold2 { 0.5f };
        std::atomic<float> currentThreshold3 { 0.5f };
        std::atomic<float> currentThreshold4 { 0.5f };
        std::atomic<bool> output1Active { false };
        std::atomic<bool> output2Active { false };
        std::atomic<bool> output3Active { false };
        std::atomic<bool> output4Active { false };

        VizData()
        {
            for (auto& v : inputWaveform) v.store(0.0f);
            for (auto& v : output1Waveform) v.store(0.0f);
            for (auto& v : output2Waveform) v.store(0.0f);
            for (auto& v : output3Waveform) v.store(0.0f);
            for (auto& v : output4Waveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffers for waveform capture
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutput1Buffer;
    juce::AudioBuffer<float> vizOutput2Buffer;
    juce::AudioBuffer<float> vizOutput3Buffer;
    juce::AudioBuffer<float> vizOutput4Buffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};
