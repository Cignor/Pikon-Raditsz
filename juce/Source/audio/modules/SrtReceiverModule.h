#pragma once

#include "ModuleProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <array>

#ifdef HAS_SRT
#include <srt/srt.h>
#endif

// Forward declarations for FFmpeg (used for decoding the MPEG-TS stream)
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVIOContext;

/**
 * SRT Receiver Module
 * 
 * Receives audio via SRT (Secure Reliable Transport) protocol.
 * SRT is a modern, low-latency streaming protocol widely used by broadcasters.
 * 
 * Features:
 * - Listener mode (wait for incoming connections)
 * - Caller mode (connect to remote SRT server)
 * - Optional AES encryption via passphrase
 * - Low latency with configurable buffer
 * - Connection statistics (RTT, packet loss, bandwidth)
 * 
 * Requires: libsrt (install via vcpkg: vcpkg install libsrt:x64-windows)
 */
class SrtReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    // Parameter IDs
    static constexpr auto paramIdGain = "gain";
    static constexpr auto paramIdMode = "mode";
    static constexpr auto paramIdPort = "port";
    static constexpr auto paramIdLatency = "latency";
    
    enum class Mode { Listener = 0, Caller = 1 };
    
    // Connection statistics
    struct Stats
    {
        std::atomic<int64_t> rtt{0};           // Round-trip time (ms)
        std::atomic<int> pktLoss{0};           // Packets lost
        std::atomic<double> bandwidth{0.0};    // Mbps
        std::atomic<int64_t> bytesRecv{0};     // Total bytes received
    };
    Stats stats;
    
    // Visualization data
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL{};
        std::array<std::atomic<float>, waveformPoints> waveformR{};
        std::atomic<float> peakL{0.0f};
        std::atomic<float> peakR{0.0f};
    };
    VizData vizData;

    SrtReceiverModule();
    ~SrtReceiverModule() override;

    const juce::String getName() const override { return "srt_receiver"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Connection control
    bool startListening(int port);
    bool connectTo(const juce::String& host, int port);
    void disconnect();
    bool isConnected() const { return srtConnected.load(); }
    bool isListening() const { return srtListening.load(); }
    bool isSrtAvailable() const;
    
    // Settings
    void setPassphrase(const juce::String& passphrase);
    void setStreamId(const juce::String& streamId);
    
    juce::String getConnectionStatus() const { return connectionStatus; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>& onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    // State persistence
    juce::ValueTree getExtraStateTree() const override;
    void setExtraStateTree(const juce::ValueTree& tree) override;

private:
    void run() override;
    void updateStats();
    void receiveLoop();
    bool decodeAudioPacket(const uint8_t* data, int size);
    
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Parameters
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* portParam = nullptr;
    std::atomic<float>* latencyParam = nullptr;

#ifdef HAS_SRT
    SRTSOCKET srtSocket = SRT_INVALID_SOCK;
    SRTSOCKET connectedSocket = SRT_INVALID_SOCK;
    bool initializeSrt();
    void cleanupSrt();
#endif

    // FFmpeg for MPEG-TS decoding
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    AVIOContext* avioCtx = nullptr;
    uint8_t* avioBuffer = nullptr;
    int audioStreamIndex = -1;

    // Connection state
    std::atomic<bool> srtConnected{false};
    std::atomic<bool> srtListening{false};
    std::atomic<bool> startRequested{false};
    std::atomic<bool> stopRequested{false};
    juce::String connectionStatus{"SRT Ready"};
    
    // Connection parameters
    juce::String hostAddress;
    int connectPort = 9000;
    juce::String passphrase;
    juce::String streamId;
    Mode currentMode = Mode::Listener;
    juce::CriticalSection settingsLock;

    // Receive buffer
    static constexpr int recvBufferSize = 1500 * 7;  // ~7 MTU packets
    uint8_t recvBuffer[recvBufferSize];

    // Audio ring buffer
    static constexpr int audioBufferSize = 48000;
    juce::AbstractFifo audioFifo{audioBufferSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
    double currentSampleRate = 44100.0;

    // UI input buffers
    char hostInputBuffer[256] = {0};
    char passphraseBuffer[128] = {0};
    char streamIdBuffer[256] = {0};
};
