#pragma once

#include "ModuleProcessor.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class ADSRModuleProcessor : public ModuleProcessor
{
public:
    // Parameter ID constants
    static constexpr auto paramIdAttack = "attack";
    static constexpr auto paramIdDecay = "decay";
    static constexpr auto paramIdSustain = "sustain";
    static constexpr auto paramIdRelease = "release";
    static constexpr auto paramIdAttackMod = "attack_mod";
    static constexpr auto paramIdDecayMod = "decay_mod";
    static constexpr auto paramIdSustainMod = "sustain_mod";
    static constexpr auto paramIdReleaseMod = "release_mod";

    ADSRModuleProcessor();
    ~ADSRModuleProcessor() override = default;

    const juce::String getName() const override { return "adsr"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
private:
    static void HelpMarkerADSR(const char* desc);
public:

    void drawIoPins(const NodePinHelpers& helpers) override
    {
        // Gate and Trigger inputs stay as parallel pins (no UI controls)
        helpers.drawParallelPins("Gate In", 0, "Env Out", 0);
        helpers.drawParallelPins("Trigger In", 1, "Inv Out", 1);
        // Attack/Decay/Sustain/Release mods are now inline pins
        // Draw remaining outputs
        helpers.drawAudioOutputPin("EOR Gate", 2);
        helpers.drawAudioOutputPin("EOC Gate", 3);
    }

    bool usesCustomPinLayout() const override { return true; }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Gate In";
            case 1: return "Trigger In";
            case 2: return "Attack Mod";
            case 3: return "Decay Mod";
            case 4: return "Sustain Mod";
            case 5: return "Release Mod";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Env Out";
            case 1: return "Inv Out";
            case 2: return "EOR Gate";
            case 3: return "EOC Gate";
            default: return juce::String("Out ") + juce::String(channel + 1);
        }
    }
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* attackParam { nullptr };
    std::atomic<float>* decayParam { nullptr };
    std::atomic<float>* sustainParam { nullptr };
    std::atomic<float>* releaseParam { nullptr };
    std::atomic<float>* attackModParam { nullptr };
    std::atomic<float>* decayModParam { nullptr };
    std::atomic<float>* sustainModParam { nullptr };
    std::atomic<float>* releaseModParam { nullptr };
    std::atomic<float>* relativeAttackModParam { nullptr };
    std::atomic<float>* relativeDecayModParam { nullptr };
    std::atomic<float>* relativeSustainModParam { nullptr };
    std::atomic<float>* relativeReleaseModParam { nullptr };
    // Simple RT-safe envelope state (custom, replaces juce::ADSR to avoid surprises)
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage { Stage::Idle };
    float envLevel { 0.0f };
    bool lastGate { false };
    bool lastTrigger { false };
    int eorPending { 0 };
    int eocPending { 0 };
    double sr { 44100.0 };

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> envelopeWaveform;
        std::atomic<int> currentStage { 0 }; // 0=Idle, 1=Attack, 2=Decay, 3=Sustain, 4=Release
        std::atomic<float> currentEnvelope { 0.0f };
        std::atomic<bool> gateActive { false };
        std::atomic<bool> triggerActive { false };
        std::atomic<float> currentAttack { 0.01f };
        std::atomic<float> currentDecay { 0.1f };
        std::atomic<float> currentSustain { 0.7f };
        std::atomic<float> currentRelease { 0.2f };

        VizData()
        {
            for (auto& v : envelopeWaveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffer for envelope waveform capture
    juce::AudioBuffer<float> vizEnvelopeBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};


