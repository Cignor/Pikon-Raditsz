#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class LagProcessorModuleProcessor : public ModuleProcessor
{
public:
    LagProcessorModuleProcessor();
    ~LagProcessorModuleProcessor() override = default;

    const juce::String getName() const override { return "lag_processor"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;

    void drawIoPins(const NodePinHelpers& helpers) override
    {
        // Only draw signal pin - modulation pins (channels 1-2) are now inline
        helpers.drawParallelPins("Signal In", 0, "Smoothed Out", 0);
        // Rise Mod (ch 1), Fall Mod (ch 2) are drawn inline in drawParametersInNode
    }

    bool usesCustomPinLayout() const override { return true; }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Signal In";
            case 1: return "Rise Mod";
            case 2: return "Fall Mod";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        if (channel == 0) return "Smoothed Out";
        return juce::String("Out ") + juce::String(channel + 1);
    }

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* riseTimeParam { nullptr };  // milliseconds
    std::atomic<float>* fallTimeParam { nullptr };  // milliseconds
    juce::AudioParameterChoice* modeParam { nullptr };
    
    // State variables for smoothing algorithm
    float currentOutput { 0.0f };
    double currentSampleRate { 44100.0 };

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputWaveform;
        std::array<std::atomic<float>, waveformPoints> outputWaveform;
        std::array<std::atomic<float>, waveformPoints> targetWaveform; // For envelope follower mode
        std::atomic<float> currentRiseMs { 10.0f };
        std::atomic<float> currentFallMs { 10.0f };
        std::atomic<int> currentMode { 0 }; // 0 = Slew Limiter, 1 = Envelope Follower

        VizData()
        {
            for (auto& v : inputWaveform) v.store(0.0f);
            for (auto& v : outputWaveform) v.store(0.0f);
            for (auto& v : targetWaveform) v.store(0.0f);
        }
    };

    VizData vizData;
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
#endif
};

