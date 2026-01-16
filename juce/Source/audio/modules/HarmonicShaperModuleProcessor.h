#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>

class HarmonicShaperModuleProcessor : public ModuleProcessor
{
public:
    static constexpr int NUM_OSCILLATORS = 8;

    // --- Parameter IDs ---
    static constexpr auto paramIdMasterFreq   = "masterFrequency";
    static constexpr auto paramIdMasterDrive  = "masterDrive";
    static constexpr auto paramIdOutputGain   = "outputGain";
    static constexpr auto paramIdMix          = "mix";
    static constexpr auto paramIdCharacter    = "character"; // Controls modulation intensity
    static constexpr auto paramIdSmoothness   = "smoothness"; // Carrier smoothing
    // Modulation targets
    static constexpr auto paramIdMasterFreqMod = "masterFrequency_mod";
    static constexpr auto paramIdMasterDriveMod = "masterDrive_mod";
    static constexpr auto paramIdOutputGainMod = "outputGain_mod";
    static constexpr auto paramIdMixMod = "mix_mod";
    static constexpr auto paramIdCharacterMod = "character_mod";
    static constexpr auto paramIdSmoothnessMod = "smoothness_mod";

    HarmonicShaperModuleProcessor();
    ~HarmonicShaperModuleProcessor() override = default;

    const juce::String getName() const override { return "harmonic_shaper"; }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
    bool usesCustomPinLayout() const override { return true; }
#endif

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    std::array<juce::dsp::Oscillator<float>, NUM_OSCILLATORS> oscillators;
    std::array<int, NUM_OSCILLATORS> currentWaveforms;

    // --- Cached Parameter Pointers ---
    std::atomic<float>* masterFreqParam { nullptr };
    std::atomic<float>* masterDriveParam { nullptr };
    std::atomic<float>* outputGainParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    std::atomic<float>* characterParam { nullptr };
    std::atomic<float>* smoothnessParam { nullptr };
    std::array<std::atomic<float>*, NUM_OSCILLATORS> ratioParams;
    std::array<std::atomic<float>*, NUM_OSCILLATORS> detuneParams;
    std::array<std::atomic<float>*, NUM_OSCILLATORS> waveformParams;
    std::array<std::atomic<float>*, NUM_OSCILLATORS> driveParams;
    std::array<std::atomic<float>*, NUM_OSCILLATORS> levelParams;

    // --- Smoothed Values for Zipper-Free Modulation ---
    juce::SmoothedValue<float> smoothedMasterFreq;
    juce::SmoothedValue<float> smoothedMasterDrive;
    juce::SmoothedValue<float> smoothedCarrier; // Smooth carrier to reduce harshness
    
    // Relative modulation parameters
    std::atomic<float>* relativeFreqModParam { nullptr };
    std::atomic<float>* relativeDriveModParam { nullptr };
    std::atomic<float>* relativeGainModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };
    std::atomic<float>* relativeCharacterModParam { nullptr };
    std::atomic<float>* relativeSmoothnessModParam { nullptr };

    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 128;
        std::array<std::atomic<float>, NUM_OSCILLATORS> oscillatorLevels; // Current level (0-1)
        std::array<std::atomic<float>, NUM_OSCILLATORS> oscillatorFrequencies; // Current frequency in Hz
        std::array<std::atomic<int>, NUM_OSCILLATORS> oscillatorWaveforms; // Current waveform (0-3)
        std::array<std::atomic<float>, waveformPoints> combinedWaveform; // Combined carrier waveform
        std::atomic<float> masterFrequency { 440.0f };
        std::atomic<float> masterDrive { 0.5f };
    };
    VizData vizData;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmonicShaperModuleProcessor);
};

