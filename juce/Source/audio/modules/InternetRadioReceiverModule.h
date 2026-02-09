#pragma once

#include "ModuleProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <array>
#include <memory>

// Forward declarations for FFmpeg (avoid including heavy headers)
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVPacket;
struct AVFrame;

/**
 * Internet Radio Receiver Module
 *
 * Receives audio from internet radio streams (Icecast, Shoutcast, direct HTTP streams).
 * Supports MP3, AAC, OGG/Vorbis, and other common streaming formats via FFmpeg.
 *
 * Features:
 * - ICY metadata parsing (station name, current song)
 * - Automatic reconnection on stream drop
 * - Configurable buffer size for network jitter
 * - Sample rate conversion to match DAW
 */
class InternetRadioReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    // Parameter IDs
    static constexpr auto paramIdGain = "gain";
    static constexpr auto paramIdBufferSizeMs = "bufferSizeMs";
    static constexpr auto paramIdSyncToTransport = "syncToTransport";

    // Visualization data for UI (audio waveform scope)
    struct VizData
    {
        static constexpr int                           waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL{};
        std::array<std::atomic<float>, waveformPoints> waveformR{};
        std::atomic<float>                             peakL{0.0f};
        std::atomic<float>                             peakR{0.0f};
        std::atomic<int>                               bytesReceived{0};
    };
    VizData vizData;

    InternetRadioReceiverModule();
    ~InternetRadioReceiverModule() override;

    const juce::String getName() const override { return "internet_radio"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Stream control
    bool connectToStream(const juce::String& url);
    void disconnectFromStream();
    bool isConnected() const;

    // Stream info
    juce::String getStreamUrl() const;
    juce::String getStationName() const;
    juce::String getCurrentSong() const;
    juce::String getConnectionStatus() const;
    int          getBitrate() const;
    juce::String getCodecName() const;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    void setTimingInfo(const TransportState& state) override { transportState = state; }

    // State persistence
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& tree) override;

private:
    void run() override; // Main manager thread (not stream thread)

    // Station Library
    struct StationEntry
    {
        juce::String name;
        juce::String url;
        juce::String genre;
    };
    std::vector<StationEntry> stations;
    std::atomic<int>          currentStationIndex{0};

    // --- RadioStream Class (Nested) ---
    class RadioStream : public juce::Thread
    {
    public:
        RadioStream(const juce::String& url, double hostSampleRate);
        ~RadioStream() override;

        void start();
        void stop();

        // Read audio into destination buffer (returns samples read)
        int readAudio(juce::AudioBuffer<float>& dest, int numSamples);

        bool isStreamConnected() const { return connected.load(); }
        bool isBuffering() const { return buffering.load(); }

        juce::String getMetadata(const juce::String& key) const;
        juce::String getFormatInfo() const;
        int          getStreamBitrate() const { return bitrate.load(); }

    private:
        void run() override;
        bool openConnection();
        void closeConnection();
        void decodeLoop();
        void updateMetadata();

        juce::String url;
        double       hostSampleRate;

        // FFmpeg state
        AVFormatContext*      formatCtx = nullptr;
        AVCodecContext*       codecCtx = nullptr;
        SwrContext*           swrCtx = nullptr;
        AVPacket*             packet = nullptr;
        AVFrame*              frame = nullptr;
        int                   audioStreamIndex = -1;
        juce::CriticalSection ffmpegLock;

        // Audio Buffer
        static constexpr int     bufferSize = 192000; // Increased: ~4 sec @ 48k
        juce::AbstractFifo       fifo{bufferSize};
        juce::AudioBuffer<float> ringBuffer{2, bufferSize};

    public:
        // Public access to buffering state for UI
        int getBufferFreeSpace() const { return fifo.getFreeSpace(); }
        int getBufferReady() const { return fifo.getNumReady(); }

    private:
        std::atomic<bool> connected{false};
        std::atomic<bool> buffering{true};
        std::atomic<bool> stopRequested{false};
        std::atomic<int>  bitrate{0};

        std::map<juce::String, juce::String> metadata;
        mutable juce::CriticalSection        metadataLock;
    };

    // --- Stream Management ---
    std::map<int, std::unique_ptr<RadioStream>> activeStreams;
    juce::CriticalSection                       activeStreamsLock;
    void                                        updatePreCaches();

    // Set of station indices to keep active (User + Neighbors)
    std::set<int>         prefetchedIndices;
    juce::CriticalSection prefetchLock;

    // UI State
    int activeTab = 0;

    // Metadata (Module level, if needed, or per stream)
    // Note: RadioStream has its own metadata. This might be legacy or for current display.
    std::map<juce::String, juce::String> metadata;
    mutable juce::CriticalSection        metadataLock;
    std::atomic<int>                     bitrate{0};

    // Connection State (Module level summary)
    juce::String          connectionStatus{"Ready"};
    juce::CriticalSection statusLock;

    // Parameters
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* bufferSizeMsParam = nullptr;
    std::atomic<float>* syncToTransportParam = nullptr;

    TransportState                                             transportState;
    juce::AudioProcessorValueTreeState                         apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Trigger & Selection State
    float            lastTriggerVal = 0.0f;
    std::atomic<int> pendingStationSwitch{-1};

    // User List Management
    juce::String userListText;
    void         parseUserList(const juce::String& text);
    bool         useUserList = false;

    // Manual URL
    char              urlInputBuffer[512] = {0};
    std::atomic<bool> autoReconnect{true};

    // Envelope Follower
    std::atomic<float> outputEnvelope{0.0f};
    float              envelopeFollower = 0.0f;
    const float        envelopeRelease = 0.9995f;

    // State
    double currentSampleRate = 48000.0;
};
