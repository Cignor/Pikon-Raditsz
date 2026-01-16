#pragma once

#include "../ModuleProcessor.h"
#include "StkWrapper.h"
#include <memory>
#include <atomic>
#include <array>

#ifdef STK_FOUND
#include <Instrmnt.h>
#include <ModalBar.h>
#include <BandedWG.h>
#include <Shakers.h>
#endif

#if defined(PRESET_CREATOR_UI)
#include "../../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

class StkPercussionModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdFrequency = "frequency";
    static constexpr auto paramIdInstrumentType = "instrument_type";
    static constexpr auto paramIdStrikeVelocity = "strike_velocity";
    static constexpr auto paramIdStrikePosition = "strike_position";
    static constexpr auto paramIdStickHardness = "stick_hardness";
    static constexpr auto paramIdPreset = "preset";
    static constexpr auto paramIdDecay = "decay";
    static constexpr auto paramIdResonance = "resonance";
    
    // CV modulation inputs (virtual targets for routing)
    static constexpr auto paramIdFreqMod = "freq_mod";
    static constexpr auto paramIdVelocityMod = "velocity_mod";
    static constexpr auto paramIdGateMod = "gate_mod";
    static constexpr auto paramIdStickHardnessMod = "stick_hardness_mod";
    static constexpr auto paramIdStrikePositionMod = "strike_position_mod";
    static constexpr auto paramIdDecayMod = "decay_mod";
    static constexpr auto paramIdResonanceMod = "resonance_mod";

    StkPercussionModuleProcessor();
    ~StkPercussionModuleProcessor() override = default;

    const juce::String getName() const override { return "stk_percussion"; }

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
    
    void updateInstrument();
    
    juce::AudioProcessorValueTreeState apvts;
    
#ifdef STK_FOUND
    std::unique_ptr<stk::Instrmnt> instrument;
#endif
    
    double currentSampleRate = 44100.0;
    int currentInstrumentType = -1;
    
    // Cached parameter pointers
    std::atomic<float>* frequencyParam { nullptr };
    std::atomic<float>* instrumentTypeParam { nullptr };
    std::atomic<float>* strikeVelocityParam { nullptr };
    std::atomic<float>* strikePositionParam { nullptr };
    std::atomic<float>* stickHardnessParam { nullptr };
    std::atomic<float>* presetParam { nullptr };
    std::atomic<float>* decayParam { nullptr };
    std::atomic<float>* resonanceParam { nullptr };
    
    // Gate handling
    float smoothedGate { 0.0f };
    bool wasGateHigh { false };
    bool m_shouldAutoTrigger { false };
    
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
        std::atomic<int> currentInstrumentType { 0 };
        std::atomic<float> gateLevel { 0.0f };
        std::atomic<float> outputLevel { 0.0f };
        std::atomic<float> strikeVelocity { 0.0f };

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StkPercussionModuleProcessor)
};

