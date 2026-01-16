#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class ShapingOscillatorModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdFrequency    = "frequency";
    static constexpr auto paramIdWaveform     = "waveform";
    static constexpr auto paramIdDrive        = "drive";
    static constexpr auto paramIdDryWet       = "dryWet";
    // Virtual modulation target IDs (no APVTS parameters required)
    static constexpr auto paramIdFrequencyMod = "frequency_mod";
    static constexpr auto paramIdWaveformMod  = "waveform_mod";
    static constexpr auto paramIdDriveMod     = "drive_mod";
    static constexpr auto paramIdDryWetMod    = "dryWet_mod";

    ShapingOscillatorModuleProcessor();
    ~ShapingOscillatorModuleProcessor() override = default;

    const juce::String getName() const override { return "shaping_oscillator"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth,
                               const std::function<bool(const juce::String& paramId)>& isParamModulated,
                               const std::function<void()>& onModificationEnded,
                               const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
    bool usesCustomPinLayout() const override { return true; }
#endif

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    juce::dsp::Oscillator<float> oscillator;

    // Cached parameter pointers
    std::atomic<float>* frequencyParam { nullptr };
    std::atomic<float>* waveformParam  { nullptr };
    std::atomic<float>* driveParam     { nullptr };
    std::atomic<float>* dryWetParam    { nullptr };
    std::atomic<float>* relativeFreqModParam { nullptr };
    std::atomic<float>* relativeDriveModParam { nullptr };

    // Smoothed values to prevent zipper noise
    juce::SmoothedValue<float> smoothedFrequency;
    juce::SmoothedValue<float> smoothedDrive;
    juce::SmoothedValue<float> smoothedDryWet;

    int currentWaveform = -1;

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> rawOscWaveform;    // Before shaping
        std::array<std::atomic<float>, waveformPoints> shapedWaveform;    // After tanh shaping
        std::atomic<float> currentFrequency { 440.0f };
        std::atomic<float> currentDrive { 1.0f };
        std::atomic<int> currentWaveform { 0 };
        std::atomic<float> outputLevel { 0.0f };

        VizData()
        {
            for (auto& v : rawOscWaveform) v.store(0.0f);
            for (auto& v : shapedWaveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffers for waveform capture
    juce::AudioBuffer<float> vizRawOscBuffer;
    juce::AudioBuffer<float> vizShapedBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};

