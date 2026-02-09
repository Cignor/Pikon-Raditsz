#pragma once

#include "ModuleProcessor.h"
#include "FFmpegAudioReader.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>

#ifndef AUDIO_ONLY_BUILD
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#endif

/**
 * YouTube Source Module - Streams or downloads audio/video from YouTube
 *
 * Features:
 * - Downloads VOD content to local cache for offline playback
 * - Streams live content in real-time
 * - Audio output via stereo pins (always available)
 * - Video output via VideoFrameManager (PresetCreatorApp only)
 *
 * Requirements:
 * - yt-dlp.exe must be available (bundled with app or in PATH)
 *
 * Build Modes:
 * - Full build: Video ID + Audio L/R outputs
 * - AUDIO_ONLY_BUILD: Audio L/R outputs only
 */
class YouTubeSourceModule : public ModuleProcessor, private juce::Thread
{
public:
    // Download/stream status
    enum class Status
    {
        Idle,        // Waiting for URL
        Fetching,    // Getting video info via yt-dlp
        Downloading, // Downloading content
        Ready,       // Downloaded and ready to play
        Playing,     // Currently playing
        Error        // Error occurred
    };

    // Visualization data for audio scope
    struct VizData
    {
        static constexpr int                           waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL{};
        std::array<std::atomic<float>, waveformPoints> waveformR{};
        std::atomic<float>                             peakL{0.0f};
        std::atomic<float>                             peakR{0.0f};
    };
    VizData vizData;

    YouTubeSourceModule();
    ~YouTubeSourceModule() override;

    const juce::String getName() const override { return "youtube_source"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // State management
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& state) override;

    // Public accessors for UI
    Status       getStatus() const { return status.load(); }
    juce::String getStatusMessage() const { return statusMessage; }
    float        getDownloadProgress() const { return downloadProgress.load(); }
    bool         isYtDlpAvailable() const { return ytdlpFound; }
    juce::String getVideoTitle() const { return videoTitle; }

#ifndef AUDIO_ONLY_BUILD
    // Video frame for UI preview
    juce::Image getLatestVideoFrame();
#endif

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void   drawIoPins(const NodePinHelpers& helpers) override;
    ImVec2 getCustomNodeSize() const override;
#endif

private:
    void run() override; // Background thread for download/streaming

    // yt-dlp integration
    void checkForYtDlp();
    bool fetchVideoInfo(const juce::String& url);
    bool downloadVideo(const juce::String& url);

    // URL cleaning
    static juce::String cleanYouTubeUrl(const juce::String& url);

    // Media loading
    void loadDownloadedMedia();
    void stopPlayback();

    // Audio buffer management
    void fillAudioBuffer();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Parameters
    std::atomic<float>* loopParam = nullptr;
    std::atomic<float>* volumeParam = nullptr;

    // URL and state
    juce::String      currentUrl;
    juce::String      pendingUrl;
    std::atomic<bool> urlChanged{false};
    std::atomic<bool> stopRequested{false};

    // yt-dlp
    bool       ytdlpFound = false;
    juce::File ytdlpPath;

    // Status
    std::atomic<Status> status{Status::Idle};
    juce::String        statusMessage{"Enter YouTube URL"};
    std::atomic<float>  downloadProgress{0.0f};

    // Video metadata
    juce::String videoTitle;

    // Download folder
    juce::File downloadFolder;
    juce::File downloadedFile;

    // Audio playback
    std::unique_ptr<FFmpegAudioReader> audioReader;
    juce::int64                        audioReadPosition = 0;
    std::atomic<double>                audioSampleRate{48000.0};
    std::atomic<bool>                  audioLoaded{false};
    juce::CriticalSection              audioLock;

    // Audio FIFO buffer for smooth playback (lock-free, like VideoFileLoaderModule)
    static constexpr int      fifoSize = 131072; // ~2.7 seconds at 48kHz
    juce::AudioBuffer<float>  audioFifo;         // Stereo buffer
    juce::AbstractFifo        abstractFifo{fifoSize};

    // Playback state
    std::atomic<bool>   playing{false};
    std::atomic<double> totalDurationSec{0.0};
    std::atomic<float>  lastKnownPosition{0.0f};

#ifndef AUDIO_ONLY_BUILD
    // Video playback (PresetCreatorApp only)
    cv::VideoCapture      videoCapture;
    juce::CriticalSection videoLock;
    std::atomic<bool>     videoLoaded{false};
    std::atomic<int>      videoWidth{0};
    std::atomic<int>      videoHeight{0};
    std::atomic<double>   videoFps{30.0};

    // Video frame for UI preview
    juce::Image           latestFrame;
    juce::CriticalSection frameLock;
    double                lastFrameTimeMs = 0.0;
#endif

    // UI state
    char urlInputBuffer[2048] = {0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(YouTubeSourceModule)
};
