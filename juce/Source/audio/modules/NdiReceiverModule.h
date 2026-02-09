#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>

// ============================================================================
// NDI Type Definitions (embedded to avoid SDK dependency)
// These match the official NDI SDK definitions for runtime loading
// ============================================================================

// Opaque handle types
typedef void* NDIlib_find_instance_t;
typedef void* NDIlib_recv_instance_t;

// Frame type enumeration
enum NDIlib_frame_type_e
{
    NDIlib_frame_type_none = 0,
    NDIlib_frame_type_video = 1,
    NDIlib_frame_type_audio = 2,
    NDIlib_frame_type_metadata = 3,
    NDIlib_frame_type_error = 4,
    NDIlib_frame_type_status_change = 100
};

// Color format enumeration
enum NDIlib_recv_color_format_e
{
    NDIlib_recv_color_format_BGRX_BGRA = 0,
    NDIlib_recv_color_format_UYVY_BGRA = 1,
    NDIlib_recv_color_format_RGBX_RGBA = 2,
    NDIlib_recv_color_format_UYVY_RGBA = 3,
    NDIlib_recv_color_format_fastest = 100,
    NDIlib_recv_color_format_best = 101
};

// Bandwidth enumeration
enum NDIlib_recv_bandwidth_e
{
    NDIlib_recv_bandwidth_metadata_only = -10,
    NDIlib_recv_bandwidth_audio_only = 10,
    NDIlib_recv_bandwidth_lowest = 0,
    NDIlib_recv_bandwidth_highest = 100
};

// Source structure
struct NDIlib_source_t
{
    const char* p_ndi_name;
    const char* p_url_address;
};

// Find create structure
struct NDIlib_find_create_t
{
    bool        show_local_sources;
    const char* p_groups;
    const char* p_extra_ips;
};

// Receiver create structure
struct NDIlib_recv_create_v3_t
{
    NDIlib_source_t            source_to_connect_to;
    NDIlib_recv_color_format_e color_format;
    NDIlib_recv_bandwidth_e    bandwidth;
    bool                       allow_video_fields;
    const char*                p_ndi_recv_name;
};

// Video frame structure
struct NDIlib_video_frame_v2_t
{
    int         xres, yres;
    int         FourCC;
    int         frame_rate_N, frame_rate_D;
    float       picture_aspect_ratio;
    int         frame_format_type;
    int64_t     timecode;
    uint8_t*    p_data;
    int         line_stride_in_bytes;
    const char* p_metadata;
    int64_t     timestamp;
};

// Audio frame structure
struct NDIlib_audio_frame_v2_t
{
    int         sample_rate;
    int         no_channels;
    int         no_samples;
    int64_t     timecode;
    float*      p_data;
    int         channel_stride_in_bytes;
    const char* p_metadata;
    int64_t     timestamp;
};

// Metadata frame structure
struct NDIlib_metadata_frame_t
{
    int     length;
    int64_t timecode;
    char*   p_data;
};

// ============================================================================
// Function pointer types for runtime loading
// ============================================================================
typedef bool (*pfn_NDIlib_initialize)(void);
typedef void (*pfn_NDIlib_destroy)(void);
typedef NDIlib_find_instance_t (*pfn_NDIlib_find_create_v2)(
    const NDIlib_find_create_t* p_create_settings);
typedef void (*pfn_NDIlib_find_destroy)(NDIlib_find_instance_t p_instance);
typedef const NDIlib_source_t* (*pfn_NDIlib_find_get_current_sources)(
    NDIlib_find_instance_t p_instance,
    uint32_t*              p_no_sources);
typedef bool (
    *pfn_NDIlib_find_wait_for_sources)(NDIlib_find_instance_t p_instance, uint32_t timeout_in_ms);
typedef NDIlib_recv_instance_t (*pfn_NDIlib_recv_create_v3)(
    const NDIlib_recv_create_v3_t* p_create_settings);
typedef void (*pfn_NDIlib_recv_destroy)(NDIlib_recv_instance_t p_instance);
typedef NDIlib_frame_type_e (*pfn_NDIlib_recv_capture_v2)(
    NDIlib_recv_instance_t   p_instance,
    NDIlib_video_frame_v2_t* p_video_data,
    NDIlib_audio_frame_v2_t* p_audio_data,
    NDIlib_metadata_frame_t* p_metadata,
    uint32_t                 timeout_in_ms);
typedef void (*pfn_NDIlib_recv_free_video_v2)(
    NDIlib_recv_instance_t         p_instance,
    const NDIlib_video_frame_v2_t* p_video_data);
typedef void (*pfn_NDIlib_recv_free_audio_v2)(
    NDIlib_recv_instance_t         p_instance,
    const NDIlib_audio_frame_v2_t* p_audio_data);
typedef void (*pfn_NDIlib_recv_free_metadata)(
    NDIlib_recv_instance_t         p_instance,
    const NDIlib_metadata_frame_t* p_metadata);

/**
 * Source node that receives NDI video streams over the network.
 * Outputs its own logical ID as a CV signal for routing to processing nodes.
 *
 * NDI (Network Device Interface) is a royalty-free video-over-IP protocol
 * that allows video sources to be shared across a local network.
 *
 * This implementation uses RUNTIME DLL LOADING - no NDI SDK required at build time.
 * Users just need NDI Runtime installed (free from ndi.video).
 */
class NdiReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    // Visualization data for UI (audio waveform scope)
    struct VizData
    {
        static constexpr int                           waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL{};
        std::array<std::atomic<float>, waveformPoints> waveformR{};
        std::atomic<float>                             peakL{0.0f};
        std::atomic<float>                             peakR{0.0f};
        std::atomic<int>                               audioFramesReceived{0};
        std::atomic<int>                               audioSamplesBuffered{0};
    };
    VizData vizData;

    NdiReceiverModule();
    ~NdiReceiverModule() override;

    const juce::String getName() const override { return "ndi_receiver"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // For UI: get latest frame for preview
    juce::Image getLatestFrame();

    // Check if NDI library is available
    bool isNdiAvailable() const { return ndiLibLoaded; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;

    // Override to specify custom node width. Height calculated by video aspect ratio.
    ImVec2 getCustomNodeSize() const override;
#endif

    // State persistence for selected source
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& tree) override;

private:
    void         run() override;
    void         updateGuiFrame(const cv::Mat& frame);
    juce::uint32 getMyLogicalId();
    void         refreshSourceList();
    bool         connectToSource(int sourceIndex);
    void         disconnectFromSource();
    bool         loadNdiLibrary();
    void         unloadNdiLibrary();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Parameters
    std::atomic<float>* sourceIndexParam = nullptr;
    // 0 = Small (240), 1 = Normal (480), 2 = Large (960)
    std::atomic<float>* zoomLevelParam = nullptr;

    // NDI Runtime Loading
    void* ndiLibHandle = nullptr;
    bool  ndiLibLoaded = false;

    // Function pointers (loaded at runtime)
    pfn_NDIlib_initialize               fn_initialize = nullptr;
    pfn_NDIlib_destroy                  fn_destroy = nullptr;
    pfn_NDIlib_find_create_v2           fn_find_create_v2 = nullptr;
    pfn_NDIlib_find_destroy             fn_find_destroy = nullptr;
    pfn_NDIlib_find_get_current_sources fn_find_get_current_sources = nullptr;
    pfn_NDIlib_find_wait_for_sources    fn_find_wait_for_sources = nullptr;
    pfn_NDIlib_recv_create_v3           fn_recv_create_v3 = nullptr;
    pfn_NDIlib_recv_destroy             fn_recv_destroy = nullptr;
    pfn_NDIlib_recv_capture_v2          fn_recv_capture_v2 = nullptr;
    pfn_NDIlib_recv_free_video_v2       fn_recv_free_video_v2 = nullptr;
    pfn_NDIlib_recv_free_audio_v2       fn_recv_free_audio_v2 = nullptr;
    pfn_NDIlib_recv_free_metadata       fn_recv_free_metadata = nullptr;

    // NDI handles (opaque pointers)
    NDIlib_find_instance_t ndiFinder = nullptr;
    NDIlib_recv_instance_t ndiReceiver = nullptr;

    // Discovered NDI sources
    juce::StringArray                                availableSources;
    std::vector<std::pair<std::string, std::string>> sourceDetails; // (name, url) pairs
    juce::CriticalSection                            sourcesLock;
    std::atomic<bool>                                refreshRequested{false};

    // Current connection state
    std::atomic<int>  currentSourceIndex{-1};
    std::atomic<bool> isConnected{false};
    juce::String      connectionStatus{"Not connected"};

    // Video frame storage
    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;
    cv::Mat               latestFrameBgr;

    // Live stream info
    std::atomic<int>   actualWidth{0};
    std::atomic<int>   actualHeight{0};
    std::atomic<float> actualFps{0.0f};

    // Persisted source name (for reconnection after load)
    juce::String lastConnectedSourceName;

    juce::uint32 storedLogicalId = 0;

    // Audio ring buffer (NDI thread writes, audio thread reads)
    static constexpr int     audioBufferSize = 48000; // ~1 second at 48kHz
    juce::AbstractFifo       audioFifo{audioBufferSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
    std::atomic<int>         ndiSampleRate{48000};
    std::atomic<int>         ndiNumChannels{2};
    double                   currentSampleRate = 44100.0;

    // Performance metrics
    float lastProcessTimeMs = 0.0f;
    bool  lastProcessWasGpu = false;
};
