#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>

class PhaserModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdRate = "rate";
    static constexpr auto paramIdDepth = "depth";
    static constexpr auto paramIdCentreHz = "centreHz";
    static constexpr auto paramIdFeedback = "feedback";
    static constexpr auto paramIdMix = "mix";

    // Virtual IDs for modulation inputs
    static constexpr auto paramIdRateMod = "rate_mod";
    static constexpr auto paramIdDepthMod = "depth_mod";
    static constexpr auto paramIdCentreHzMod = "centreHz_mod";
    static constexpr auto paramIdFeedbackMod = "feedback_mod";
    static constexpr auto paramIdMixMod = "mix_mod";

    PhaserModuleProcessor();
    ~PhaserModuleProcessor() override = default;

    const juce::String getName() const override { return "phaser"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Rhythm introspection for BPM Monitor
    std::optional<RhythmInfo> getRhythmInfo() const override;

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

    // The core JUCE DSP phaser object
    juce::dsp::Phaser<float> phaser;

    // A temporary buffer for implementing the dry/wet mix, inspired by VoiceProcessor.cpp
    juce::AudioBuffer<float> tempBuffer;

    // Cached atomic pointers to parameters for real-time access
    std::atomic<float>* rateParam { nullptr };
    std::atomic<float>* depthParam { nullptr };
    std::atomic<float>* centreHzParam { nullptr };
    std::atomic<float>* feedbackParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    
    // Relative modulation parameters
    std::atomic<float>* relativeRateModParam { nullptr };
    std::atomic<float>* relativeDepthModParam { nullptr };
    std::atomic<float>* relativeCentreModParam { nullptr };
    std::atomic<float>* relativeFeedbackModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };
    
    // Visualization data (thread-safe for UI updates)
    struct VizData
    {
        static constexpr int frequencyPoints = 128;
        
        // LFO phase (0.0 to 1.0, represents one cycle)
        std::atomic<float> lfoPhase { 0.0f };
        
        // Current parameter values (for display)
        std::atomic<float> currentRate { 0.0f };
        std::atomic<float> currentDepth { 0.0f };
        std::atomic<float> currentCentre { 0.0f };
        std::atomic<float> currentFeedback { 0.0f };
        std::atomic<float> currentMix { 0.0f };
        
        // Frequency response visualization (magnitude at each frequency point)
        std::array<std::atomic<float>, frequencyPoints> frequencyResponse;
        
        VizData()
        {
            for (auto& val : frequencyResponse)
                val.store(0.0f);
        }
    } vizData;
    
    // LFO phase accumulator (for visualization)
    double lfoPhaseAccumulator { 0.0 };
};

