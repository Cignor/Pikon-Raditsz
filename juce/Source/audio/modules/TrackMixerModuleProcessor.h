#pragma once

#include "ModuleProcessor.h"

class TrackMixerModuleProcessor : public ModuleProcessor
{
public:
    static constexpr int MAX_TRACKS = 64;

    TrackMixerModuleProcessor();
    ~TrackMixerModuleProcessor() override = default;

    const juce::String getName() const override { return "track_mixer"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Per-channel label used by cable inspector and tooltips
    juce::String getAudioInputLabel(int channel) const override;
    void         drawIoPins(const NodePinHelpers& helpers) override;
    bool         usesCustomPinLayout() const override { return true; }
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus)
        const override;

    // Dynamic pin reporting (fixes color issue for tracks beyond 8)
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded,
        const NodePinHelpers*                                   pinHelpers = nullptr) override;
#endif

private:
    juce::AudioProcessorValueTreeState apvts;

    // Global controls
    juce::AudioParameterInt* numTracksParam{nullptr};
    juce::AudioParameterInt* numTracksMaxParam{nullptr}; // To bound modulation
    std::atomic<float>*      globalVolumeParam{nullptr}; // -60.0 to +6.0 dB

    // Per-track controls
    std::vector<std::atomic<float>*> trackGainParams;
    std::vector<std::atomic<float>*> trackPanParams;

    // Virtual modulation target IDs (no APVTS parameters required)
    static constexpr auto paramIdNumTracksMod = "numTracks_mod";
    static constexpr auto paramIdGainModPrefix = "track_gain_";
    static constexpr auto paramIdPanModPrefix = "track_pan_";

    // Modulation parameter pointers are no longer needed
    // std::vector<std::atomic<float>*> trackGainModParams;
    // std::vector<std::atomic<float>*> trackPanModParams;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    int                                                        getEffectiveNumTracks() const;

    mutable std::atomic<int> lastActiveTracks{2};
    static constexpr float   kNeutral = 0.5f;
    static constexpr float kDeadZone = 0.02f; // treat values within +/-2% around neutral as no-mod

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int                           waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        std::atomic<int>                               activeTracks{2};
        std::atomic<float>                             outputLevelDbL{-60.0f};
        std::atomic<float>                             outputLevelDbR{-60.0f};

        VizData()
        {
            for (auto& v : outputWaveformL)
                v.store(0.0f);
            for (auto& v : outputWaveformR)
                v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffer for waveform capture (stereo output)
    juce::AudioBuffer<float> vizOutputBuffer;
    int                      vizWritePos{0};
    static constexpr int     vizBufferSize = 2048; // ~43ms at 48kHz
#endif
};
