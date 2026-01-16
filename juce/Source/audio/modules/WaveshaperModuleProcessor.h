#pragma once

#include "ModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include <array>
#include <atomic>
#include <vector>
#endif

class WaveshaperModuleProcessor : public ModuleProcessor
{
public:
    WaveshaperModuleProcessor();
    ~WaveshaperModuleProcessor() override = default;

    const juce::String getName() const override { return "waveshaper"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "In L";
            case 1: return "In R";
            case 2: return "Drive Mod";
            case 3: return "Type Mod";
            case 4: return "Mix Mod";
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
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    juce::AudioProcessorValueTreeState apvts;
    
    juce::AudioParameterFloat* driveParam { nullptr };
    juce::AudioParameterChoice* typeParam { nullptr };
    juce::AudioParameterFloat* mixParam { nullptr };
    
    // Relative modulation parameters
    std::atomic<float>* relativeDriveModParam { nullptr };
    
    // Smoothed values to prevent zipper noise
    juce::SmoothedValue<float> smoothedMix;

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int curvePoints = 128;
        static constexpr int historySize = 64;
        std::array<std::atomic<float>, curvePoints> transferCurve {};
        std::array<std::atomic<float>, historySize> driveHistory {};
        std::atomic<float> inputRms { 0.0f };
        std::atomic<float> outputRms { 0.0f };
        std::atomic<float> liveDrive { 1.0f };
        std::atomic<int> liveType { 0 };
        std::atomic<int> historyWriteIndex { 0 };
    };

    VizData vizData;
    juce::AudioBuffer<float> dryBlockTemp;
    std::vector<float> curveScratch;
    std::vector<float> driveHistoryScratch;
#endif

    int driveHistoryWriteIndex = 0;
};
