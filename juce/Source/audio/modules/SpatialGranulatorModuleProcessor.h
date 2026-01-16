#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include <map>

class SpatialGranulatorModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs for APVTS
    static constexpr auto paramIdDryMix = "dryMix";
    static constexpr auto paramIdPenMix = "penMix";
    static constexpr auto paramIdSprayMix = "sprayMix";
    static constexpr auto paramIdDensity = "density";
    static constexpr auto paramIdGrainSize = "grainSize";
    static constexpr auto paramIdBufferLength = "bufferLength";

    // Color amount parameters
    static constexpr auto paramIdRedAmount = "redAmount";
    static constexpr auto paramIdGreenAmount = "greenAmount";
    static constexpr auto paramIdBlueAmount = "blueAmount";
    static constexpr auto paramIdYellowAmount = "yellowAmount";
    static constexpr auto paramIdCyanAmount = "cyanAmount";
    static constexpr auto paramIdMagentaAmount = "magentaAmount";
    static constexpr auto paramIdOrangeAmount = "orangeAmount";
    static constexpr auto paramIdPurpleAmount = "purpleAmount";

    // CV Modulation parameter IDs (for getParamRouting) - Virtual IDs, NOT in APVTS
    static constexpr auto paramIdDryMixMod = "dryMix_mod";
    static constexpr auto paramIdPenMixMod = "penMix_mod";
    static constexpr auto paramIdSprayMixMod = "sprayMix_mod";
    static constexpr auto paramIdDensityMod = "density_mod";
    static constexpr auto paramIdGrainSizeMod = "grainSize_mod";

    // Dot types
    enum class DotType
    {
        Pen,  // Static voice (chorus-like)
        Spray // Grain spawner (produces dynamic grains)
    };

    // Color IDs (8 total effects)
    enum class ColorID
    {
        Red,     // Delay
        Green,   // Filter
        Blue,    // Pitch shift
        Yellow,  // Reverb/Decay
        Cyan,    // Distortion/Drive
        Magenta, // Chorus/Modulation
        Orange,  // Bitcrusher/Downsampling
        Purple,  // Tremolo/Vibrato
        COUNT = 8
    };

    // Dot structure
    struct Dot
    {
        float   x;     // 0-1, pan position (static)
        float   y;     // 0-1, buffer position (static) - 0=start, 1=end
        float   size;  // 0-1, voice reproduction amount + color param intensity (static)
        ColorID color; // Fixed color set (Red, Green, Blue only)
        DotType type;  // Pen (voice) or Spray (grain spawner)
    };

    SpatialGranulatorModuleProcessor();
    ~SpatialGranulatorModuleProcessor() override = default;

    const juce::String getName() const override { return "spatial_granulator"; }

    // --- JUCE AudioProcessor Overrides ---
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // --- State Persistence ---
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& tree) override;

    // --- UI & Routing Overrides ---
#if defined(PRESET_CREATOR_UI)
    ImVec2 getCustomNodeSize() const override
    {
        return ImVec2(720.0f, 0.0f);
    } // 720px width for 16:9 canvas (10% smaller)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded,
        const NodePinHelpers*                                   pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus)
        const override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;

    std::vector<DynamicPinInfo> getDynamicInputPins() const override;

private:
    // --- Internal Implementation ---
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Audio processing helpers
    void processPenVoice(
        const Dot&                dot,
        juce::AudioBuffer<float>& buffer,
        int                       numSamples,
        double                    sampleRate);
    void processSprayGrains(
        const Dot&                dot,
        juce::AudioBuffer<float>& buffer,
        int                       numSamples,
        double                    sampleRate);
    void launchGrain(
        int        grainIndex,
        const Dot& dot,
        double     sampleRate,
        int        currentWritePos,
        int        currentSamplesWritten,
        float      grainSizeMs);
    float getColorParameterValue(ColorID color, float size) const;

    // Color parameter mapping
    struct ColorParameterMapping
    {
        enum class ParameterType
        {
            Delay,
            Volume,
            Pitch,
            Filter,
            Reverb,
            Distortion,
            Chorus,
            Bitcrusher,
            Tremolo,
            None
        };
        ParameterType paramType;
        float         minValue;
        float         maxValue;

        static ColorParameterMapping getMapping(ColorID color)
        {
            switch (color)
            {
            case ColorID::Red:
                return {ParameterType::Delay, 0.0f, 2000.0f}; // 0-2000ms
            case ColorID::Green:
                return {ParameterType::Filter, 20.0f, 20000.0f}; // 20-20000 Hz cutoff
            case ColorID::Blue:
                return {ParameterType::Pitch, -24.0f, 24.0f}; // -24 to +24 semitones
            case ColorID::Yellow:
                return {ParameterType::Reverb, 0.0f, 1.0f}; // 0-1 room size
            case ColorID::Cyan:
                return {ParameterType::Distortion, 0.0f, 1.0f}; // 0-1 drive amount
            case ColorID::Magenta:
                return {ParameterType::Chorus, 0.0f, 1.0f}; // 0-1 modulation depth
            case ColorID::Orange:
                return {ParameterType::Bitcrusher, 1.0f, 16.0f}; // 1-16 bit depth
            case ColorID::Purple:
                return {ParameterType::Tremolo, 0.0f, 10.0f}; // 0-10 Hz modulation rate
            default:
                return {ParameterType::None, 0.0f, 0.0f};
            }
        }
    };

    // --- APVTS & Parameters ---
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>*                dryMixParam{nullptr};
    std::atomic<float>*                penMixParam{nullptr};
    std::atomic<float>*                sprayMixParam{nullptr};
    std::atomic<float>*                densityParam{nullptr};
    std::atomic<float>*                grainSizeParam{nullptr};
    std::atomic<float>*                bufferLengthParam{nullptr};
    std::atomic<float>*                redAmountParam{nullptr};
    std::atomic<float>*                greenAmountParam{nullptr};
    std::atomic<float>*                blueAmountParam{nullptr};
    std::atomic<float>*                yellowAmountParam{nullptr};
    std::atomic<float>*                cyanAmountParam{nullptr};
    std::atomic<float>*                magentaAmountParam{nullptr};
    std::atomic<float>*                orangeAmountParam{nullptr};
    std::atomic<float>*                purpleAmountParam{nullptr};

    // --- Grain State (for Spray tool) ---
    struct Grain
    {
        bool   isActive{false};
        double readPosition{0.0};
        double increment{1.0};
        int    samplesRemaining{0};
        int    totalLifetime{0};
        float  panL{0.707f};
        float  panR{0.707f};
        float  envelope{0.0f};
        float  envelopeIncrement{0.0f};
        // Dynamic movement
        float movementOffset{0.0f};
        float movementVelocity{0.0f};
        // Color parameters (stored from dot)
        ColorID color{ColorID::Red};
        float   size{0.5f};
        float   delayTimeMs{0.0f};
        float   delayFeedback{0.0f}; // Feedback amount (0.0 = no feedback, 0.95 = high feedback)
        float   volume{1.0f};
        float   pitchOffset{0.0f};
        // Filter for Green color
        float filterCutoffHz{20000.0f}; // Cutoff frequency in Hz
        float filterResonance{
            0.707f}; // Resonance/Q (0.707 = no resonance, higher = more resonance)
        // Simple filter state for grains (one-pole lowpass)
        float filterState{0.0f}; // Filter state for per-grain filtering
        // Reverb for Yellow color
        float              reverbRoomSize{0.0f}; // 0-1 room size
        float              reverbDecay{0.0f};    // Decay time (0-1)
        std::vector<float> reverbBuffer;         // Simple reverb buffer
        int                reverbWritePos{0};
        // Distortion for Cyan color
        float distortionDrive{0.0f}; // 0-1 drive amount
        float distortionTone{0.5f};  // 0-1 tone (lowpass filter cutoff)
        // Chorus for Magenta color
        float              chorusDelayMs{0.0f};  // Delay time in ms
        float              chorusDepth{0.0f};    // Modulation depth (0-1)
        float              chorusLfoPhase{0.0f}; // LFO phase accumulator
        std::vector<float> chorusBuffer;         // Chorus delay buffer
        int                chorusWritePos{0};
        // Bitcrusher for Orange color
        float bitcrusherBits{16.0f};      // Bit depth (1-16)
        float bitcrusherDownsample{1.0f}; // Downsample factor (1 = no downsampling)
        float bitcrusherLastSample{0.0f}; // Last quantized sample
        int   bitcrusherCounter{0};       // Downsample counter
        // Tremolo for Purple color
        float tremoloRate{0.0f};  // Modulation rate in Hz (0-10)
        float tremoloDepth{0.5f}; // Modulation depth (0-1)
        float tremoloPhase{0.0f}; // LFO phase accumulator
    };
    std::array<Grain, 384>
                 grainPool; // Larger pool for multiple spray dots (3x increase: 128 -> 384)
    juce::Random random;

    // --- Voice State (for Pen tool) ---
    struct Voice
    {
        bool   isActive{false};
        double readPosition{0.0};
        float  panL{0.707f};
        float  panR{0.707f};
        float  volume{1.0f};
        // Delay line for Red color
        std::vector<float> delayBuffer;
        int                delayWritePos{0};
        float              delayTimeMs{0.0f};
        float delayFeedback{0.0f}; // Feedback amount (0.0 = no feedback, 1.0 = full feedback)
        // Pitch shifter for Blue color
        double             pitchRatio{1.0};
        double             pitchPhase{0.0};
        std::vector<float> pitchBuffer;
        // Filter for Green color
        juce::dsp::StateVariableTPTFilter<float> filter;
        float                                    filterCutoffHz{20000.0f}; // Cutoff frequency in Hz
        float                                    filterResonance{
            0.707f}; // Resonance/Q (0.707 = no resonance, higher = more resonance)
        // Reverb for Yellow color
        float              reverbRoomSize{0.0f}; // 0-1 room size
        float              reverbDecay{0.0f};    // Decay time (0-1)
        std::vector<float> reverbBufferL;        // Simple reverb buffer L
        std::vector<float> reverbBufferR;        // Simple reverb buffer R
        int                reverbWritePos{0};
        // Distortion for Cyan color
        float                         distortionDrive{0.0f}; // 0-1 drive amount
        float                         distortionTone{0.5f};  // 0-1 tone (lowpass filter cutoff)
        juce::dsp::IIR::Filter<float> distortionToneFilter;  // Tone filter
        // Chorus for Magenta color
        float              chorusDelayMs{0.0f};  // Delay time in ms
        float              chorusDepth{0.0f};    // Modulation depth (0-1)
        float              chorusLfoPhase{0.0f}; // LFO phase accumulator
        std::vector<float> chorusBufferL;        // Chorus delay buffer L
        std::vector<float> chorusBufferR;        // Chorus delay buffer R
        int                chorusWritePos{0};
        // Bitcrusher for Orange color
        float bitcrusherBits{16.0f};       // Bit depth (1-16)
        float bitcrusherDownsample{1.0f};  // Downsample factor (1 = no downsampling)
        float bitcrusherLastSampleL{0.0f}; // Last quantized sample L
        float bitcrusherLastSampleR{0.0f}; // Last quantized sample R
        int   bitcrusherCounter{0};        // Downsample counter
        // Tremolo for Purple color
        float tremoloRate{0.0f};  // Modulation rate in Hz (0-10)
        float tremoloDepth{0.5f}; // Modulation depth (0-1)
        float tremoloPhase{0.0f}; // LFO phase accumulator
    };
    std::array<Voice, 192> voicePool; // Pool for Pen tool voices (3x increase: 64 -> 192)

    // --- Audio Buffering ---
    juce::AudioBuffer<float> sourceBuffer;
    int                      sourceWritePos{0};
    int samplesWritten{0}; // Track how many samples have been written to the buffer
    std::map<int, double> dotDensityPhases; // Phase accumulator per dot (using dot index as key)

    // --- Thread-Safe Dot Storage ---
    mutable juce::ReadWriteLock dotsLock;
    std::vector<Dot>            dots;

    // --- UI State (only accessed from UI thread) ---
    DotType activeTool{DotType::Pen};
    ColorID activeColor{ColorID::Red};
    float   defaultDotSize{0.3f}; // Default size for new dots

    // --- Parameter Smoothing ---
    juce::SmoothedValue<float> smoothedDryMix, smoothedPenMix, smoothedSprayMix, smoothedDensity,
        smoothedGrainSize;

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int                           waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> outputWaveform;
        std::atomic<int>                               activeVoices{0};
        std::atomic<int>                               activeGrains{0};
        std::atomic<float> bufferFillLevel{0.0f}; // 0-1, how much of buffer is filled
        std::atomic<float> outputLevel{0.0f};

        VizData()
        {
            for (auto& v : outputWaveform)
                v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffer for waveform capture
    juce::AudioBuffer<float> vizOutputBuffer;
    int                      vizWritePos{0};
    static constexpr int     vizBufferSize = 2048; // ~43ms at 48kHz
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialGranulatorModuleProcessor)
};
