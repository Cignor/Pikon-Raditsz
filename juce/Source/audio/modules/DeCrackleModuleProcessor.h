#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#if defined(PRESET_CREATOR_UI)
#include <array>
#include <atomic>
#include <vector>
#endif

class DeCrackleModuleProcessor : public ModuleProcessor
{
public:
    DeCrackleModuleProcessor();
    ~DeCrackleModuleProcessor() override = default;

    const juce::String getName() const override { return "de_crackle"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;

    void drawIoPins(const NodePinHelpers& helpers) override
    {
        helpers.drawParallelPins("In L", 0, "Out L", 0);
        helpers.drawParallelPins("In R", 1, "Out R", 1);
        // Modulation pins are now inline, not drawn here
    }
    
    bool usesCustomPinLayout() const override { return true; }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "In L";
            case 1: return "In R";
            case 2: return "Threshold Mod";
            case 3: return "Smoothing Mod";
            case 4: return "Amount Mod";
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

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* thresholdParam { nullptr };
    std::atomic<float>* smoothingTimeMsParam { nullptr };
    std::atomic<float>* amountParam { nullptr };
    
    // State variables for discontinuity detection (per channel)
    float lastInputSample[2] { 0.0f, 0.0f };
    float lastOutputSample[2] { 0.0f, 0.0f };
    int smoothingSamplesRemaining[2] { 0, 0 };
    
    double currentSampleRate { 44100.0 };

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        static constexpr int historySize = 64;
        std::array<std::atomic<float>, waveformPoints> dryWave {};
        std::array<std::atomic<float>, waveformPoints> wetWave {};
        std::array<std::atomic<float>, waveformPoints> crackleMask {};
        std::array<std::atomic<float>, historySize> crackleHistory {};
        std::atomic<int> historyWriteIndex { 0 };
        std::atomic<float> crackleRatePerSec { 0.0f };
        std::atomic<float> smoothingMsLive { 0.0f };
        std::atomic<float> amountLive { 0.0f };
        std::atomic<float> smoothingActiveRatio { 0.0f };
    };

    VizData vizData;
    juce::AudioBuffer<float> dryCapture;
    juce::AudioBuffer<float> wetCapture;
    std::vector<int> crackleBinScratch;
    int crackleHistoryWrite = 0;
#endif
};

