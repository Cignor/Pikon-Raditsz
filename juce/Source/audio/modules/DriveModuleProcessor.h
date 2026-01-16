#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class DriveModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdDrive = "drive";
    static constexpr auto paramIdMix = "mix";

    DriveModuleProcessor();
    ~DriveModuleProcessor() override = default;

    const juce::String getName() const override { return "drive"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
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
    std::atomic<float>* driveParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    std::atomic<float>* relativeDriveModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };
    
    // Smoothed values to prevent zipper noise
    juce::SmoothedValue<float> smoothedDrive;
    juce::SmoothedValue<float> smoothedMix;

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> dryWaveform;
        std::array<std::atomic<float>, waveformPoints> wetWaveform;
        std::array<std::atomic<float>, waveformPoints> mixWaveform;
        std::atomic<float> currentDrive { 0.0f };
        std::atomic<float> currentMix { 1.0f };
        std::atomic<float> harmonicEnergy { 0.0f };
        std::atomic<float> outputLevelDb { -60.0f };

        VizData()
        {
            for (auto& v : dryWaveform) v.store(0.0f);
            for (auto& v : wetWaveform) v.store(0.0f);
            for (auto& v : mixWaveform) v.store(0.0f);
        }
    };

    VizData vizData;
    juce::AudioBuffer<float> vizDryBuffer;
    juce::AudioBuffer<float> vizWetBuffer;
#endif
};

