#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

/**
 * CVMixerModuleProcessor - A dedicated mixer for control voltages (CV/modulation signals).
 * 
 * Features:
 * - Linear crossfading between two inputs (A and B) for precise morphing
 * - Additional summing inputs (C and D) with bipolar level controls
 * - Inverted output for signal polarity flipping
 * - Designed for CV signals with mathematically predictable linear operations
 */
class CVMixerModuleProcessor : public ModuleProcessor
{
public:
    CVMixerModuleProcessor();
    ~CVMixerModuleProcessor() override = default;

    const juce::String getName() const override { return "cv_mixer"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
#endif

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "In A";
            case 1: return "In B";
            case 2: return "In C";
            case 3: return "In D";
            case 4: return "Crossfade Mod";
            case 5: return "Level A Mod";
            case 6: return "Level C Mod";
            case 7: return "Level D Mod";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Mix Out";
            case 1: return "Inv Out";
            default: return juce::String("Out ") + juce::String(channel + 1);
        }
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* crossfadeParam { nullptr };  // -1..1 (A to B)
    std::atomic<float>* levelAParam { nullptr };     // 0..1 (master level for A/B crossfade)
    std::atomic<float>* levelCParam { nullptr };     // -1..1 (bipolar for C)
    std::atomic<float>* levelDParam { nullptr };     // -1..1 (bipolar for D)

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputAWaveform;
        std::array<std::atomic<float>, waveformPoints> inputBWaveform;
        std::array<std::atomic<float>, waveformPoints> inputCWaveform;
        std::array<std::atomic<float>, waveformPoints> inputDWaveform;
        std::array<std::atomic<float>, waveformPoints> mixOutputWaveform;
        std::array<std::atomic<float>, waveformPoints> invOutputWaveform;
        std::atomic<float> currentCrossfade { 0.0f };
        std::atomic<float> currentLevelA { 1.0f };
        std::atomic<float> currentLevelC { 0.0f };
        std::atomic<float> currentLevelD { 0.0f };
        std::atomic<float> inputALevel { 0.0f };
        std::atomic<float> inputBLevel { 0.0f };
        std::atomic<float> inputCLevel { 0.0f };
        std::atomic<float> inputDLevel { 0.0f };
        std::atomic<float> mixOutputLevel { 0.0f };
        std::atomic<float> invOutputLevel { 0.0f };

        VizData()
        {
            for (auto& v : inputAWaveform) v.store(0.0f);
            for (auto& v : inputBWaveform) v.store(0.0f);
            for (auto& v : inputCWaveform) v.store(0.0f);
            for (auto& v : inputDWaveform) v.store(0.0f);
            for (auto& v : mixOutputWaveform) v.store(0.0f);
            for (auto& v : invOutputWaveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffers for waveform capture
    juce::AudioBuffer<float> vizInputABuffer;
    juce::AudioBuffer<float> vizInputBBuffer;
    juce::AudioBuffer<float> vizInputCBuffer;
    juce::AudioBuffer<float> vizInputDBuffer;
    juce::AudioBuffer<float> vizMixOutputBuffer;
    juce::AudioBuffer<float> vizInvOutputBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};

