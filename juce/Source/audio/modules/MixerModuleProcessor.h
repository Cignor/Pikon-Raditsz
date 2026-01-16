#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class MixerModuleProcessor : public ModuleProcessor
{
public:
    MixerModuleProcessor();
    ~MixerModuleProcessor() override = default;

    const juce::String getName() const override { return "mixer"; }

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
            case 0: return "In A L";
            case 1: return "In A R";
            case 2: return "In B L";
            case 3: return "In B R";
            case 4: return "Gain Mod";
            case 5: return "Pan Mod";
            case 6: return "X-Fade Mod";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Out L";
            case 1: return "Out R";
            default: return juce::String("Out ") + juce::String(channel + 1);
        }
    }
    
    // Parameter bus contract implementation

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* gainParam { nullptr };       // dB
    std::atomic<float>* panParam { nullptr };        // -1..1
    std::atomic<float>* crossfadeParam { nullptr };

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputAWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputAWaveformR;
        std::array<std::atomic<float>, waveformPoints> inputBWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputBWaveformR;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        std::atomic<float> currentCrossfade { 0.0f };
        std::atomic<float> currentGainDb { 0.0f };
        std::atomic<float> currentPan { 0.0f };
        std::atomic<float> inputALevelDb { -60.0f };
        std::atomic<float> inputBLevelDb { -60.0f };
        std::atomic<float> outputLevelDbL { -60.0f };
        std::atomic<float> outputLevelDbR { -60.0f };

        VizData()
        {
            for (auto& v : inputAWaveformL) v.store(0.0f);
            for (auto& v : inputAWaveformR) v.store(0.0f);
            for (auto& v : inputBWaveformL) v.store(0.0f);
            for (auto& v : inputBWaveformR) v.store(0.0f);
            for (auto& v : outputWaveformL) v.store(0.0f);
            for (auto& v : outputWaveformR) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffers for waveform capture
    juce::AudioBuffer<float> vizInputABuffer;
    juce::AudioBuffer<float> vizInputBBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};