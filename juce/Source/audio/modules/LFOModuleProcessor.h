#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class LFOModuleProcessor : public ModuleProcessor
{
public:
    // Parameter ID constants
    static constexpr auto paramIdRate = "rate";
    static constexpr auto paramIdDepth = "depth";
    static constexpr auto paramIdWave = "wave";
    static constexpr auto paramIdBipolar = "bipolar";
    static constexpr auto paramIdRateMod = "rate_mod";
    static constexpr auto paramIdDepthMod = "depth_mod";
    static constexpr auto paramIdWaveMod = "wave_mod";
    static constexpr auto paramIdSync = "sync";
    static constexpr auto paramIdRateDivision = "rate_division";
    static constexpr auto paramIdRelativeMode = "relative_mode";

    LFOModuleProcessor();
    ~LFOModuleProcessor() override = default;

    const juce::String getName() const override { return "lfo"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    
    // Transport sync support
    void setTimingInfo(const TransportState& state) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Rhythm introspection for BPM Monitor
    std::optional<RhythmInfo> getRhythmInfo() const override;
    
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }

    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    juce::dsp::Oscillator<float> osc;
    
    // Cached parameter pointers
    std::atomic<float>* rateParam{ nullptr };
    std::atomic<float>* depthParam{ nullptr };
    std::atomic<float>* bipolarParam{ nullptr };
    std::atomic<float>* waveParam{ nullptr };
    std::atomic<float>* syncParam{ nullptr };
    std::atomic<float>* rateDivisionParam{ nullptr };
    std::atomic<float>* relativeModeParam{ nullptr };
    
    int currentWaveform = -1;
    TransportState m_currentTransport;

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> lfoWaveform;
        std::atomic<float> currentValue { 0.0f };
        std::atomic<float> currentRate { 1.0f };
        std::atomic<float> currentDepth { 0.5f };
        std::atomic<int> currentWave { 0 };
        std::atomic<bool> isBipolar { true };
        std::atomic<bool> isSynced { false };

        VizData()
        {
            for (auto& v : lfoWaveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffer for LFO waveform capture
    juce::AudioBuffer<float> vizLfoBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};