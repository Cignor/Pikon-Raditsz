#pragma once

#include "ModuleProcessor.h"
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class MapRangeModuleProcessor : public ModuleProcessor
{
public:
    MapRangeModuleProcessor();
    ~MapRangeModuleProcessor() override = default;

    const juce::String getName() const override { return "map_range"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    float getLastInputValue() const;
    float getLastOutputValue() const;
    float getLastCvOutputValue() const;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
        case 0:
            return "Input";
        default:
            return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
        case 0:
            return "Norm Out";
        case 1:
            return "Raw Out";
        case 2:
            return "CV Out";
        default:
            return juce::String("Out ") + juce::String(channel + 1);
        }
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>*                inMinParam{nullptr};
    std::atomic<float>*                inMaxParam{nullptr};
    std::atomic<float>*                outMinParam{nullptr};
    std::atomic<float>*                outMaxParam{nullptr};
    // Norm Out bipolar range [-1, 1]
    std::atomic<float>* normMinParam{nullptr};
    std::atomic<float>* normMaxParam{nullptr};

    // New CV parameters
    std::atomic<float>* cvMinParam{nullptr};
    std::atomic<float>* cvMaxParam{nullptr};

    std::atomic<float> lastInputValue{0.0f};
    std::atomic<float> lastOutputValue{0.0f};
    std::atomic<float> lastCvOutputValue{0.0f};

#if defined(PRESET_CREATOR_UI)
    struct VizData
    {
        static constexpr int                           waveformPoints = 128;
        std::array<std::atomic<float>, waveformPoints> inputWaveform;
        std::array<std::atomic<float>, waveformPoints> normWaveform;
        std::array<std::atomic<float>, waveformPoints> rawWaveform;
        std::array<std::atomic<float>, waveformPoints> cvWaveform;
        std::atomic<float>                             currentInMin{0.0f};
        std::atomic<float>                             currentInMax{1.0f};
        std::atomic<float>                             currentNormMin{-1.0f};
        std::atomic<float>                             currentNormMax{1.0f};
        std::atomic<float>                             currentOutMin{0.0f};
        std::atomic<float>                             currentOutMax{1.0f};
        std::atomic<float>                             currentCvMin{0.0f};
        std::atomic<float>                             currentCvMax{1.0f};

        VizData()
        {
            for (auto& v : inputWaveform)
                v.store(0.0f);
            for (auto& v : normWaveform)
                v.store(0.0f);
            for (auto& v : rawWaveform)
                v.store(0.0f);
            for (auto& v : cvWaveform)
                v.store(0.0f);

            // Explicitly zero all min/max tracking atomics
            currentInMin.store(0.0f);
            currentInMax.store(1.0f);
            currentNormMin.store(-1.0f);
            currentNormMax.store(1.0f);
            currentOutMin.store(0.0f);
            currentOutMax.store(1.0f);
            currentCvMin.store(0.0f);
            currentCvMax.store(1.0f);
        }
    };

    VizData                  vizData;
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
#endif
};
