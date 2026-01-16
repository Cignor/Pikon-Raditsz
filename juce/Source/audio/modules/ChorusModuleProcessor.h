#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

class ChorusModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs for APVTS and modulation routing
    static constexpr auto paramIdRate = "rate";
    static constexpr auto paramIdDepth = "depth";
    static constexpr auto paramIdMix = "mix";
    // Virtual IDs for modulation inputs
    static constexpr auto paramIdRateMod = "rate_mod";
    static constexpr auto paramIdDepthMod = "depth_mod";
    static constexpr auto paramIdMixMod = "mix_mod";

    ChorusModuleProcessor();
    ~ChorusModuleProcessor() override = default;

    const juce::String getName() const override { return "chorus"; }

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

    // Dynamic pin interface - preserves exact modulation system
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // The core JUCE DSP chorus object
    juce::dsp::Chorus<float> chorus;

    // Cached atomic pointers to parameters for real-time access
    std::atomic<float>* rateParam { nullptr };
    std::atomic<float>* depthParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    
    // Relative modulation parameters
    std::atomic<float>* relativeRateModParam { nullptr };
    std::atomic<float>* relativeDepthModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };

    // --- Visualization Data ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        static constexpr int lfoPoints = 128;
        std::array<std::atomic<float>, waveformPoints> inputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputWaveformR;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        std::array<std::atomic<float>, lfoPoints> lfoWaveform;
        std::atomic<float> currentRate { 1.0f };
        std::atomic<float> currentDepth { 0.25f };
        std::atomic<float> currentMix { 0.5f };
    };
    VizData vizData;

    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
    std::vector<float> vizLfoBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048;
    float vizLfoPhase { 0.0f };
};

