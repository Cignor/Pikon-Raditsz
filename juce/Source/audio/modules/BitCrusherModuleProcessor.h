#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>

class BitCrusherModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs for APVTS
    static constexpr auto paramIdBitDepth = "bit_depth";
    static constexpr auto paramIdSampleRate = "sample_rate";
    static constexpr auto paramIdMix = "mix";
    static constexpr auto paramIdAntiAlias = "antiAlias";
    static constexpr auto paramIdQuantMode = "quant_mode";
    
    // Virtual modulation target IDs (no APVTS parameters required)
    static constexpr auto paramIdBitDepthMod = "bit_depth_mod";
    static constexpr auto paramIdSampleRateMod = "sample_rate_mod";
    static constexpr auto paramIdMixMod = "mix_mod";
    static constexpr auto paramIdAntiAliasMod = "antiAlias_mod";
    static constexpr auto paramIdQuantModeMod = "quant_mode_mod";

    BitCrusherModuleProcessor();
    ~BitCrusherModuleProcessor() override = default;

    const juce::String getName() const override { return "bit_crusher"; }

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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // A temporary buffer is needed to properly implement the dry/wet mix
    juce::AudioBuffer<float> tempBuffer;

    // Cached atomic pointers to parameters
    std::atomic<float>* bitDepthParam { nullptr };
    std::atomic<float>* sampleRateParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    std::atomic<float>* antiAliasParam { nullptr };
    std::atomic<float>* quantModeParam { nullptr };
    std::atomic<float>* relativeBitDepthModParam { nullptr };
    std::atomic<float>* relativeSampleRateModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };

    // Smoothed values to prevent zipper noise
    juce::SmoothedValue<float> mBitDepthSm;
    juce::SmoothedValue<float> mSampleRateSm;

    // Anti-aliasing filters (stereo)
    juce::dsp::StateVariableTPTFilter<float> mAntiAliasFilterL;
    juce::dsp::StateVariableTPTFilter<float> mAntiAliasFilterR;

    // Sample-and-hold decimator state
    float mSrCounterL = 0.0f;
    float mLastSampleL = 0.0f;
    float mSrCounterR = 0.0f;
    float mLastSampleR = 0.0f;
    
    // Random number generator for dithering
    juce::Random mRandom;
    
    // Noise shaping state (one per channel)
    float mQuantErrorL = 0.0f;
    float mQuantErrorR = 0.0f;
    
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputWaveformR;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        
        // Sample-and-hold regions (normalized positions 0-1, -1 = inactive)
        // Track actual hold positions from decimator state
        std::array<std::atomic<float>, 64> holdStartPositions; // Where holds begin (in waveform snapshot index)
        std::array<std::atomic<float>, 64> holdEndPositions;   // Where holds end (in waveform snapshot index)
        std::array<std::atomic<float>, 64> holdValues;         // The held sample value
        std::atomic<int> activeHoldCount { 0 };
        
        // Current quantization state
        std::atomic<float> currentBitDepth { 16.0f };
        std::atomic<float> currentSampleRate { 1.0f };
        std::atomic<int> currentQuantMode { 0 }; // 0=Linear, 1=Dither, 2=NoiseShape
        std::atomic<bool> currentAntiAlias { true };
    };
    VizData vizData;
    
    // Circular buffer for waveform snapshots
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
    juce::AudioBuffer<float> vizDecimatedBuffer; // Store decimated (before quantization) samples
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
    
    // Track hold regions for visualization (from decimator state)
    struct HoldRegion
    {
        int startIdx { -1 }; // Start index in waveform snapshot
        int endIdx { -1 };   // End index in waveform snapshot
        float value { 0.0f }; // The held sample value
    };
    std::array<HoldRegion, 64> trackedHolds;
    int trackedHoldCount { 0 };
    float lastHoldStartPos { -1.0f }; // Last position where we started a hold (for tracking)
};

