#pragma once

#include "ModuleProcessor.h"
#include "FFmpegAudioReader.h"
#include "../dsp/TimePitchProcessor.h"
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_graphics/juce_graphics.h>
#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudacodec.hpp>
#endif

/**
 * Source node that loads a video file and publishes frames to VideoFrameManager.
 * Outputs its own logical ID as a CV signal for routing to processing nodes.
 */
class VideoFileLoaderModule : public ModuleProcessor, private juce::Thread
{
public:
    VideoFileLoaderModule();
    ~VideoFileLoaderModule() override;

    const juce::String getName() const override { return "video_file_loader"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void setTimingInfo(const TransportState& state) override;
    void forceStop() override; // Force stop (used after patch load)

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Dynamic pin definitions
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

    // State management for saving/loading video file path
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& state) override;

    // For UI
    juce::Image getLatestFrame();
    void        chooseVideoFile();

    // Timeline reporting interface (for Timeline Sync feature)
    bool   canProvideTimeline() const override;
    double getTimelinePositionSeconds() const override;
    double getTimelineDurationSeconds() const override;
    bool   isTimelineActive() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;

    // Override to specify custom node width. Height is calculated dynamically based on video aspect
    // ratio. Width changes based on zoom level (Small=240px, Normal=480px, Large=960px).
    ImVec2 getCustomNodeSize() const override;
#endif

private:
    void run() override;
    void updateGuiFrame(const cv::Mat& frame);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    std::atomic<float>* loopParam = nullptr;
    // 0 = Small (240), 1 = Normal (480), 2 = Large (960)
    std::atomic<float>* zoomLevelParam = nullptr;
    // Playback controls
    std::atomic<float>*         speedParam = nullptr;   // 0.25 .. 4.0 (1.0 default)
    std::atomic<float>*         inNormParam = nullptr;  // 0..1
    std::atomic<float>*         outNormParam = nullptr; // 0..1
    std::atomic<float>*         syncParam = nullptr;    // bool as float
    juce::AudioParameterChoice* engineParam = nullptr;

    std::atomic<bool> playing{true};
    std::atomic<bool> isStopped{
        true}; // true = stopped (reset to start), false = paused (resume from current)
    std::atomic<bool> syncToTransport{true};
    std::atomic<bool> lastTransportPlaying{false};
    std::atomic<bool> needPreviewFrame{false};
    std::atomic<bool> lastPlaying{false}; // for play-edge detection
    std::atomic<int>  lastFourcc{0};      // cached FOURCC
    TransportCommand  lastTransportCommand{
        TransportCommand::Stop}; // Track last command for transition detection
    std::atomic<double> pausedNormalizedPosition{
        -1.0}; // -1.0 = invalid, >= 0.0 = saved paused position
    std::atomic<int>    pendingSeekFrame{-1};
    std::atomic<int>    lastPosFrame{0};
    std::atomic<double> totalDurationMs{0.0};

    // Per-instance timing for frame-rate throttling (NOT static!)
    double lastFrameTimeMs{0.0};
    double lastGuiUpdateTimeMs{0.0};

    // Master clock: audio-driven synchronization for video sync
    std::atomic<juce::int64> currentAudioSamplePosition{
        0}; // The master clock - only advanced by audio thread
    std::atomic<double> sourceAudioSampleRate{44100.0}; // Sample rate of the loaded file
    std::atomic<double> audioReaderLengthSamples{0.0};
    std::atomic<float>  lastKnownNormalizedPosition{
        0.0f}; // Used for UI/reporting only, NOT for pause/resume

    // Unified, thread-safe seeking mechanism for both video and audio
    std::atomic<float> pendingSeekNormalized{-1.0f};

    cv::VideoCapture      videoCapture;
    juce::CriticalSection captureLock;

    static juce::String fourccToString(int fourcc)
    {
        if (fourcc == 0)
            return "unknown";

        char chars[5];
        chars[0] = (char)(fourcc & 0xFF);
        chars[1] = (char)((fourcc >> 8) & 0xFF);
        chars[2] = (char)((fourcc >> 16) & 0xFF);
        chars[3] = (char)((fourcc >> 24) & 0xFF);
        chars[4] = '\0';

        bool printable = true;
        for (int i = 0; i < 4; ++i)
        {
            const unsigned char ch = (unsigned char)chars[i];
            if (ch == 0 || ch < 32 || ch > 126)
            {
                printable = false;
                break;
            }
        }

        if (printable)
            return juce::String(chars);

        return juce::String::formatted("0x%08X", (unsigned int)fourcc);
    }

    static juce::String fourccFriendlyName(const juce::String& code)
    {
        if (code == "unknown" || code.startsWithIgnoreCase("0x"))
            return "unknown";

        const juce::String c = code.toLowerCase();
        if (c == "avc1" || c == "h264")
            return "H.264";
        if (c == "hvc1" || c == "hevc" || c == "hev1")
            return "H.265/HEVC";
        if (c == "mp4v" || c == "m4v")
            return "MPEG-4 Part 2";
        if (c == "mjpg" || c == "mjpa" || c == "jpeg")
            return "Motion JPEG";
        if (c == "xvid")
            return "MPEG-4 ASP (Xvid)";
        if (c == "vp09")
            return "VP9";
        if (c == "av01")
            return "AV1";
        if (c == "wmv3" || c == "wvc1")
            return "VC-1";
        if (c == "h263")
            return "H.263";
        return "unknown";
    }
    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;

    juce::File                         videoFileToLoad;
    juce::File                         currentVideoFile;
    std::unique_ptr<juce::FileChooser> fileChooser;

    // Cached metadata (atomic for cross-thread visibility)
    std::atomic<int> totalFrames{0};

#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudacodec.hpp>
#endif

    // ... existing code ...

    // Audio playback engine
    std::unique_ptr<FFmpegAudioReader> audioReader;
    TimePitchProcessor                 timePitch;
    double                             audioReadPosition = 0.0;
    double                             audioSampleRate = 44100.0;
    std::atomic<bool>                  audioLoaded{false};
    juce::CriticalSection              audioLock;

    // FIFO buffer for thread-safe audio streaming
    juce::AudioBuffer<float> audioFifo;
    juce::AbstractFifo       abstractFifo{0};
    int                      fifoSize{0};

    // GPU Video Reader (NVDEC)
#if defined(WITH_CUDA_SUPPORT)
    cv::Ptr<cv::cudacodec::VideoReader> gpuReader;
    cv::cuda::GpuMat                    gpuFrame;            // Raw decode (NV12 typically)
    cv::cuda::GpuMat                    gpuFrameRgba;        // Converted for pipeline
    std::atomic<bool>                   useGpuReader{false}; // Active status
    juce::String                        gpuErrorMsg;         // For UI diagnostics
#endif

    void loadAudioFromVideo();

    void updateLastKnownNormalizedFromSamples(juce::int64 samplePos);
};
