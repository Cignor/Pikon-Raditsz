#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>

class GranulatorModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs for APVTS
    static constexpr auto paramIdDensity      = "density";
    static constexpr auto paramIdSize         = "size";
    static constexpr auto paramIdPosition     = "position";
    static constexpr auto paramIdSpread       = "spread";
    static constexpr auto paramIdPitch        = "pitch";
    static constexpr auto paramIdPitchRandom  = "pitchRandom";
    static constexpr auto paramIdPanRandom    = "panRandom";
    static constexpr auto paramIdGate         = "gate";
    static constexpr auto paramIdMix          = "mix";

    // Virtual IDs for modulation inputs, used for routing
    static constexpr auto paramIdTriggerIn    = "trigger_in_mod";
    static constexpr auto paramIdDensityMod   = "density_mod";
    static constexpr auto paramIdSizeMod      = "size_mod";
    static constexpr auto paramIdPositionMod  = "position_mod";
    static constexpr auto paramIdPitchMod     = "pitch_mod";
    static constexpr auto paramIdGateMod      = "gate_mod";
    static constexpr auto paramIdMixMod       = "mix_mod";

    // Relative modulation mode parameters
    static constexpr auto paramIdRelativeDensityMod  = "relativeDensityMod";
    static constexpr auto paramIdRelativeSizeMod     = "relativeSizeMod";
    static constexpr auto paramIdRelativePositionMod = "relativePositionMod";
    static constexpr auto paramIdRelativePitchMod    = "relativePitchMod";

    GranulatorModuleProcessor();
    ~GranulatorModuleProcessor() override = default;

    const juce::String getName() const override { return "granulator"; }

    // --- JUCE AudioProcessor Overrides ---
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // --- UI & Routing Overrides ---
#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    juce::String getAudioInputLabel(int channel) const override;
    
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

private:
    // --- Internal Implementation ---
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void launchGrain(int grainIndex, float density, float size, float position, float spread, float pitch, float pitchRandom, float panRandom);

    // --- APVTS & Parameters ---
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* densityParam      { nullptr };
    std::atomic<float>* sizeParam         { nullptr };
    std::atomic<float>* positionParam     { nullptr };
    std::atomic<float>* spreadParam       { nullptr };
    std::atomic<float>* pitchParam        { nullptr };
    std::atomic<float>* pitchRandomParam  { nullptr };
    std::atomic<float>* panRandomParam    { nullptr };
    std::atomic<float>* gateParam         { nullptr };
    std::atomic<float>* mixParam          { nullptr };
    
    // Relative modulation mode parameters
    std::atomic<float>* relativeDensityModParam  { nullptr };
    std::atomic<float>* relativeSizeModParam     { nullptr };
    std::atomic<float>* relativePositionModParam { nullptr };
    std::atomic<float>* relativePitchModParam    { nullptr };

    // --- Grain State ---
    struct Grain
    {
        bool isActive { false };
        double readPosition { 0.0 };
        double increment { 1.0 };
        int samplesRemaining { 0 };
        int totalLifetime { 0 };
        float panL { 0.707f };
        float panR { 0.707f };
    };
    std::array<Grain, 64> grainPool;
    juce::Random random;

    // --- Audio Buffering ---
    juce::AudioBuffer<float> sourceBuffer;
    int sourceWritePos { 0 };
    double densityPhase { 0.0 }; // Phase accumulator for density-based grain spawning

    // --- Parameter Smoothing ---
    juce::SmoothedValue<float> smoothedDensity, smoothedSize, smoothedPosition, smoothedPitch, smoothedGate, smoothedMix;

    // --- CV De-stepping State (for modules that are block-constant) ---
    float prevDensityCv { std::numeric_limits<float>::quiet_NaN() };
    float prevSizeCv    { std::numeric_limits<float>::quiet_NaN() };
    float prevPositionCv{ std::numeric_limits<float>::quiet_NaN() };
    float prevPitchCv   { std::numeric_limits<float>::quiet_NaN() };
    float prevGateCv    { std::numeric_limits<float>::quiet_NaN() };
    float prevMixCv     { std::numeric_limits<float>::quiet_NaN() };

    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL;
        std::array<std::atomic<float>, waveformPoints> waveformR;
        std::atomic<float> writePosNormalized { 0.0f }; // 0-1 position in buffer
        std::atomic<float> positionParamNormalized { 0.5f }; // Current position param (0-1)
        std::atomic<int> activeGrainCount { 0 };
        std::array<std::atomic<float>, 64> activeGrainPositions; // Normalized read positions (0-1)
        std::array<std::atomic<float>, 64> activeGrainEnvelopes; // Current envelope values (0-1)
    };
    VizData vizData;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GranulatorModuleProcessor)
};
