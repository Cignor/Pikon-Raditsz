#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class GateModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdThreshold = "threshold";
    static constexpr auto paramIdAttack = "attack";
    static constexpr auto paramIdRelease = "release";
    static constexpr auto paramIdThresholdMod = "threshold_mod";
    static constexpr auto paramIdAttackMod = "attack_mod";
    static constexpr auto paramIdReleaseMod = "release_mod";

    GateModuleProcessor();
    ~GateModuleProcessor() override = default;

    const juce::String getName() const override { return "gate"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // Cached atomic pointers to parameters
    std::atomic<float>* thresholdParam { nullptr };
    std::atomic<float>* attackParam { nullptr };
    std::atomic<float>* releaseParam { nullptr };

    // DSP state for the envelope follower
    float envelope { 0.0f };
    double currentSampleRate { 48000.0 };

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int historyPoints = 128;
        std::array<std::atomic<float>, historyPoints> inputHistory;
        std::array<std::atomic<float>, historyPoints> envelopeHistory;
        std::array<std::atomic<float>, historyPoints> gateHistory;
        std::atomic<int> writeIndex { 0 };
        std::atomic<float> currentThresholdDb { -40.0f };
        std::atomic<float> currentAttackMs { 1.0f };
        std::atomic<float> currentReleaseMs { 50.0f };
        std::atomic<float> gateAmount { 0.0f };

        VizData()
        {
            for (auto& v : inputHistory) v.store(-80.0f);
            for (auto& v : envelopeHistory) v.store(-80.0f);
            for (auto& v : gateHistory) v.store(0.0f);
        }
    };

    VizData vizData;
#endif
};

