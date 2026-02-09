#pragma once

#include "ModuleProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <array>

// Forward declarations for FFmpeg
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVPacket;
struct AVFrame;

/**
 * RTMP Receiver Module
 * 
 * Receives audio from RTMP streams. RTMP is commonly used for:
 * - OBS Studio streaming
 * - Twitch/YouTube Live re-streams
 * - Local streaming servers (nginx-rtmp, SRS)
 * 
 * This module connects as an RTMP client to receive streams.
 * Common use: Start an RTMP server (like nginx-rtmp), point OBS to it,
 * then connect this module to receive the audio.
 * 
 * Features:
 * - Connect to any RTMP URL
 * - Supports rtmp://, rtmps://, rtmpt:// protocols
 * - Automatic audio codec detection
 * - Low-latency playback
 */
class RtmpReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    // Parameter IDs
    static constexpr auto paramIdGain = "gain";
    static constexpr auto paramIdBufferSizeMs = "bufferSizeMs";
    
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

    RtmpReceiverModule();
    ~RtmpReceiverModule() override;

    const juce::String getName() const override { return "rtmp_receiver"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Connection control
    bool connectToStream(const juce::String& rtmpUrl);
    void disconnectFromStream();
    bool isConnected() const { return streamConnected.load(); }
    
    // Stream info
    juce::String getStreamUrl() const { return currentUrl; }
    juce::String getCodecName() const { return codecName; }
    juce::String getConnectionStatus() const { return connectionStatus; }
    int getBitrate() const { return streamBitrate.load(); }

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
    bool openStream(const juce::String& url);
    void closeStream();
    void decodeAndBuffer();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Parameters
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* bufferSizeMsParam = nullptr;

    // FFmpeg contexts
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int audioStreamIndex = -1;

    // Connection state
    juce::String currentUrl;
    std::atomic<bool> streamConnected{false};
    std::atomic<bool> connectionRequested{false};
    std::atomic<bool> disconnectRequested{false};
    juce::String connectionStatus{"Ready"};
    juce::String pendingUrl;
    juce::CriticalSection urlLock;

    // Stream info
    juce::String codecName;
    std::atomic<int> streamBitrate{0};
    std::atomic<int> sourceSampleRate{44100};
    std::atomic<int> sourceNumChannels{2};

    // Audio ring buffer
    static constexpr int audioBufferSize = 96000;
    juce::AbstractFifo audioFifo{audioBufferSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
    double currentSampleRate = 44100.0;

    // Reconnection
    std::atomic<bool> autoReconnect{true};
    int reconnectAttempts = 0;
    static constexpr int maxReconnectAttempts = 5;

    // UI
    char urlInputBuffer[512] = {0};
};
