#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>

class LimiterModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdThreshold = "threshold";
    static constexpr auto paramIdRelease = "release";
    static constexpr auto paramIdMix = "mix";

    // Virtual IDs for modulation inputs
    static constexpr auto paramIdThresholdMod = "threshold_mod";
    static constexpr auto paramIdReleaseMod = "release_mod";
    static constexpr auto paramIdMixMod = "mix_mod";

    LimiterModuleProcessor();
    ~LimiterModuleProcessor() override = default;

    const juce::String getName() const override { return "limiter"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
    
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // The core JUCE DSP Limiter object
    juce::dsp::Limiter<float> limiter;
    juce::AudioBuffer<float> dryBuffer;
    
    // Smoothed values to prevent zipper noise
    juce::SmoothedValue<float> smoothedMix;

    // Cached atomic pointers to parameters
    std::atomic<float>* thresholdParam { nullptr };
    std::atomic<float>* releaseParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    
    // Relative modulation parameters
    std::atomic<float>* relativeThresholdModParam { nullptr };
    std::atomic<float>* relativeReleaseModParam { nullptr };

    struct VizData
    {
        static constexpr int historyPoints = 128;
        std::array<std::atomic<float>, historyPoints> inputHistory;
        std::array<std::atomic<float>, historyPoints> outputHistory;
        std::array<std::atomic<float>, historyPoints> reductionHistory;
        std::atomic<int> historyWriteIndex { 0 };
        std::atomic<float> currentReduction { 0.0f };
        std::atomic<float> currentThreshold { 0.0f };
        std::atomic<float> currentRelease { 10.0f };
        std::atomic<float> currentInputDb { -60.0f };
        std::atomic<float> currentOutputDb { -60.0f };

        VizData()
        {
            for (auto& v : inputHistory) v.store(-60.0f);
            for (auto& v : outputHistory) v.store(-60.0f);
            for (auto& v : reductionHistory) v.store(0.0f);
        }
    };
    VizData vizData;
    int vizHistoryIndex { 0 };
};

