#pragma once

#include "../ModuleProcessor.h"
#include "StkWrapper.h"
#include <memory>
#include <atomic>
#include <array>

#ifdef STK_FOUND
#include <Instrmnt.h>
#include <Plucked.h>
#endif

#if defined(PRESET_CREATOR_UI)
#include "../../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

class StkPluckedModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdFrequency = "frequency";
    static constexpr auto paramIdDamping = "damping";
    static constexpr auto paramIdPluckVelocity = "pluck_velocity";
    
    // CV modulation inputs (virtual targets for routing)
    static constexpr auto paramIdFreqMod = "freq_mod";
    static constexpr auto paramIdDampingMod = "damping_mod";
    static constexpr auto paramIdVelocityMod = "velocity_mod";
    static constexpr auto paramIdGateMod = "gate_mod";

    StkPluckedModuleProcessor();
    ~StkPluckedModuleProcessor() override = default;

    const juce::String getName() const override { return "stk_plucked"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void setTimingInfo(const TransportState& state) override;
    void forceStop() override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth,
                              const std::function<bool(const juce::String& paramId)>& isParamModulated,
                              const std::function<void()>& onModificationEnded,
                              const NodePinHelpers* pinHelpers = nullptr) override;
    
    void drawIoPins(const NodePinHelpers& helpers) override;
    
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    juce::AudioProcessorValueTreeState apvts;
    
#ifdef STK_FOUND
    std::unique_ptr<stk::Plucked> instrument;
#endif
    
    double currentSampleRate = 44100.0;
    
    // Cached parameter pointers
    std::atomic<float>* frequencyParam { nullptr };
    std::atomic<float>* dampingParam { nullptr };
    std::atomic<float>* pluckVelocityParam { nullptr };
    
    // Gate handling
    float smoothedGate { 0.0f };
    bool wasGateHigh { false };
    bool m_shouldAutoTrigger { false };
    
    // Re-trigger counter for continuous plucking
    int pluckReTriggerCounter = 0;
    
    // Transport state
    TransportState m_currentTransport;
    
    // Output telemetry
    std::vector<std::unique_ptr<std::atomic<float>>> lastOutputValues;

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> outputWaveform;
        std::atomic<float> currentFrequency { 440.0f };
        std::atomic<float> gateLevel { 0.0f };
        std::atomic<float> outputLevel { 0.0f };

        VizData()
        {
            for (auto& v : outputWaveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffer for waveform capture
    juce::AudioBuffer<float> vizOutputBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StkPluckedModuleProcessor)
};

