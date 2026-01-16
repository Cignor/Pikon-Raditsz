#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

/**
 * @class NoiseModuleProcessor
 * @brief Generates white, pink, or brown noise with controllable level.
 *
 * This module acts as a sound source, providing different colors of noise. Both the
 * noise color and the output level can be modulated via CV inputs.
 */
class NoiseModuleProcessor : public ModuleProcessor
{
public:
    // Parameter ID constants for APVTS and modulation routing
    static constexpr auto paramIdLevel = "level";
    static constexpr auto paramIdColour = "colour";
    static constexpr auto paramIdRate = "rate";
    // ADDED: Virtual parameter IDs for modulation inputs
    static constexpr auto paramIdLevelMod = "level_mod";
    static constexpr auto paramIdColourMod = "colour_mod";
    static constexpr auto paramIdRateMod = "rate_mod";

    static constexpr float minRateHz = 0.1f;
    static constexpr float maxRateHz = 200.0f;

    NoiseModuleProcessor();
    ~NoiseModuleProcessor() override = default;

    const juce::String getName() const override { return "noise"; }

    // --- Audio Processing ---
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setTimingInfo(const TransportState& state) override;
    void forceStop() override;

    // --- Required by ModuleProcessor ---
    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }
    
    // --- Parameter Modulation Routing ---
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    bool usesCustomPinLayout() const override { return true; }
    ImVec2 getCustomNodeSize() const override { return ImVec2(260.0f, 0.0f); }

#if defined(PRESET_CREATOR_UI)
    // --- UI Drawing and Pin Definitions ---
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;

    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    juce::AudioProcessorValueTreeState apvts;

    // --- Parameters ---
    std::atomic<float>* levelDbParam{ nullptr };
    juce::AudioParameterChoice* colourParam{ nullptr };
    std::atomic<float>* rateHzParam{ nullptr };

    // --- DSP State ---
    juce::Random random;
    juce::dsp::IIR::Filter<float> pinkFilter;  // Simple filter to approximate pink noise
    juce::dsp::IIR::Filter<float> brownFilter; // Simple filter to approximate brown noise
    double currentSampleRate { 44100.0 };
    float slowNoiseState { 0.0f };
    
    // Transport state tracking
    TransportState m_currentTransport;

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> outputWaveform;
        std::atomic<float> currentLevelDb { -12.0f };
        std::atomic<int> currentColour { 0 }; // 0=White, 1=Pink, 2=Brown
        std::atomic<float> outputRms { 0.0f };
        std::atomic<float> currentRateHz { 20.0f };

        VizData()
        {
            for (auto& v : outputWaveform) v.store(0.0f);
        }
    };

    VizData vizData;
    juce::AudioBuffer<float> vizOutputBuffer;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseModuleProcessor)
};