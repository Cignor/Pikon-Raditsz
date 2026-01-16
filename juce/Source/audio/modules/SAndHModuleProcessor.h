#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class SAndHModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdThreshold = "threshold";
    static constexpr auto paramIdEdge = "edge";
    static constexpr auto paramIdSlew = "slew";
    static constexpr auto paramIdMode = "mode";
    static constexpr auto paramIdThresholdMod = "threshold_mod";
    static constexpr auto paramIdEdgeMod = "edge_mod";
    static constexpr auto paramIdSlewMod = "slew_mod";

    SAndHModuleProcessor();
    ~SAndHModuleProcessor() override = default;

    const juce::String getName() const override { return "s_and_h"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    bool usesCustomPinLayout() const override { return true; }
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    
    // Parameter pointers
    std::atomic<float>* thresholdParam{ nullptr };
    juce::AudioParameterChoice* edgeParam{ nullptr };
    std::atomic<float>* slewParam{ nullptr };
    juce::AudioParameterChoice* modeParam{ nullptr };
    
    // State variables
    float heldValue{ 0.0f };
    float smoothedValue{ 0.0f };
    float lastTriggerValue{ 0.0f };
    double currentSampleRate{ 44100.0 };
    
    // Slew limiter
    juce::LinearSmoothedValue<float> slewSmoother;
    float lastSlewTimeSec{ 0.0f }; // Track last slew time to avoid unnecessary resets
    
    // Edge detection state
    enum class EdgeType { Rising, Falling, Both };
    EdgeType currentEdgeType{ EdgeType::Rising };
    bool wasTriggerHigh{ false };

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputWaveform;
        std::array<std::atomic<float>, waveformPoints> outputWaveform;
        std::array<std::atomic<float>, waveformPoints> smoothedWaveform;
        std::array<std::atomic<float>, waveformPoints> triggerMarkers;
        std::atomic<float> currentThreshold{ 0.5f };
        std::atomic<int> currentEdge{ 0 };
        std::atomic<float> currentSlew{ 0.0f };
        std::atomic<int> currentMode{ 0 };
        std::atomic<int> sampleCount{ 0 };

        VizData()
        {
            for (auto& v : inputWaveform) v.store(0.0f);
            for (auto& v : outputWaveform) v.store(0.0f);
            for (auto& v : smoothedWaveform) v.store(0.0f);
            for (auto& v : triggerMarkers) v.store(0.0f);
        }
    };

    VizData vizData;
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
    juce::AudioBuffer<float> vizTriggerBuffer;
#endif
};

