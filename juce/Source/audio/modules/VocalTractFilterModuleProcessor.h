#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

// Simple descriptor for one formant band
struct FormantData { float frequency; float gain; float q; };

class VocalTractFilterModuleProcessor : public ModuleProcessor
{
public:
    VocalTractFilterModuleProcessor();
    ~VocalTractFilterModuleProcessor() override = default;

    const juce::String getName() const override { return "vocal_tract_filter"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Dynamic pins for editor rendering
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    bool usesCustomPinLayout() const override { return true; }

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Audio In L";
            case 1: return "Audio In R";
            case 2: return "Vowel Mod";
            case 3: return "Formant Mod";
            case 4: return "Instability Mod";
            case 5: return "Gain Mod";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Audio Out L";
            case 1: return "Audio Out R";
            default: return juce::String("Out ") + juce::String(channel + 1);
        }
    }

private:
    // Internal helpers
    void updateCoefficients(float vowelShape, float formantShift, float instability);
    void ensureWorkBuffers(int numChannels, int numSamples);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // State
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* vowelShapeParam { nullptr };
    std::atomic<float>* formantShiftParam { nullptr };
    std::atomic<float>* instabilityParam { nullptr };
    std::atomic<float>* outputGainParam { nullptr }; // dB

    // Formant tables
    static const FormantData VOWEL_A[4];
    static const FormantData VOWEL_E[4];
    static const FormantData VOWEL_I[4];
    static const FormantData VOWEL_O[4];
    static const FormantData VOWEL_U[4];

    // DSP - Use simple IIR filters for stereo processing
    using IIRFilter = juce::dsp::IIR::Filter<float>;
    struct Bands { IIRFilter b0, b1, b2, b3; };
    Bands bandsL;  // Left channel filters
    Bands bandsR;  // Right channel filters
    std::array<float, 4> bandGains { 1.0f, 0.5f, 0.2f, 0.15f };
    juce::dsp::Oscillator<float> wowOscillator, flutterOscillator;
    juce::dsp::ProcessSpec dspSpec { 0.0, 0, 0 };

    // Preallocated working buffers to avoid RT allocations
    juce::AudioBuffer<float> workBuffer;
    juce::AudioBuffer<float> sumBuffer;
    juce::AudioBuffer<float> tmpBuffer;

#if defined(PRESET_CREATOR_UI)
    // Visualization data shared with UI thread
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputWaveformR;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        std::array<std::atomic<float>, waveformPoints> formantEnergyHistory;
        std::array<std::atomic<float>, 4> bandEnergy;
        std::array<std::atomic<float>, 4> formantFrequency;
        std::array<std::atomic<float>, 4> formantGain;
        std::array<std::atomic<float>, 4> formantQ;
        std::atomic<float> currentVowelShape { 0.0f };
        std::atomic<float> currentFormantShift { 0.0f };
        std::atomic<float> currentInstability { 0.0f };
        std::atomic<float> currentGainDb { 0.0f };
        std::atomic<float> inputLevel { 0.0f };
        std::atomic<float> outputLevel { 0.0f };

        VizData()
        {
            for (auto& v : inputWaveformL) v.store(0.0f);
            for (auto& v : inputWaveformR) v.store(0.0f);
            for (auto& v : outputWaveformL) v.store(0.0f);
            for (auto& v : outputWaveformR) v.store(0.0f);
            for (auto& v : formantEnergyHistory) v.store(0.0f);
            for (auto& v : bandEnergy) v.store(0.0f);
            for (auto& v : formantFrequency) v.store(200.0f);
            for (auto& v : formantGain) v.store(0.0f);
            for (auto& v : formantQ) v.store(1.0f);
        }
    };

    VizData vizData;
    juce::AudioBuffer<float> vizInputBufferL;
    juce::AudioBuffer<float> vizInputBufferR;
    juce::AudioBuffer<float> vizOutputBufferL;
    juce::AudioBuffer<float> vizOutputBufferR;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalTractFilterModuleProcessor);
};
