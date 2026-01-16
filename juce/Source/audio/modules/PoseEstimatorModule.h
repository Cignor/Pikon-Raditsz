#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#if WITH_CUDA_SUPPORT
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#endif
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#if defined(PRESET_CREATOR_UI)
#include <juce_gui_basics/juce_gui_basics.h>
#endif

// The MPI model detects 15 keypoints per person
inline constexpr int MPI_NUM_KEYPOINTS = 15;

/**
 * A real-time safe struct to hold the (x, y) coordinates of each keypoint.
 * Used to pass pose data from the processing thread to the audio thread via a lock-free FIFO.
 */
struct PoseResult
{
    float keypoints[MPI_NUM_KEYPOINTS][2] = {{0}}; // [point_index][x or y]
    int   detectedPoints = 0;
    bool  isValid = false;
    bool  zoneHits[4] = {false, false, false, false}; // Zone hit detection results
};

/**
 * Pose Estimator Module
 * Uses OpenPose MPI model to detect human body keypoints in real-time video.
 * Connects to a video source (webcam or video file) and outputs 30 CV signals
 * (x,y coordinates for 15 body keypoints: head, shoulders, elbows, wrists, hips, knees, ankles,
 * etc.)
 */
class PoseEstimatorModule : public ModuleProcessor, private juce::Thread
{
public:
    PoseEstimatorModule();
    ~PoseEstimatorModule() override;

    const juce::String getName() const override { return "pose_estimator"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Parameter routing for CV modulation
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus)
        const override;

    // Persist extra state (e.g., assets path)
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& state) override;

    // For UI: get latest frame with skeleton overlay for preview
    juce::Image getLatestFrame();

    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

    // Zone rectangles structure: each color zone can have multiple rectangles
    struct ZoneRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    // Helper functions to serialize/deserialize zone rectangles
    static juce::String          serializeZoneRects(const std::vector<ZoneRect>& rects);
    static std::vector<ZoneRect> deserializeZoneRects(const juce::String& data);
    void                         loadZoneRects(int colorIndex, std::vector<ZoneRect>& rects) const;
    void                         saveZoneRects(int colorIndex, const std::vector<ZoneRect>& rects);

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }

    // Override to specify custom node width based on zoom level (Small/Normal/Large)
    ImVec2 getCustomNodeSize() const override;
#endif

private:
    void run() override; // Thread entry point - processes video frames
    void updateGuiFrame(const cv::Mat& frame);
    void parsePoseOutput(
        const cv::Mat& netOutput,
        int            frameWidth,
        int            frameHeight,
        PoseResult&    result);
    void loadModel(int modelIndex);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Parameters
    std::atomic<float>* sourceIdParam = nullptr;
    // 0 = Small (240), 1 = Normal (480), 2 = Large (960)
    std::atomic<float>*       zoomLevelParam = nullptr;
    std::atomic<float>*       confidenceThresholdParam = nullptr;
    juce::AudioParameterBool* drawSkeletonParam = nullptr;
    juce::AudioParameterBool* useGpuParam = nullptr;
    // Store custom assets directory as plain string (saved via extra state)
    juce::String assetsPath;
#if defined(PRESET_CREATOR_UI)
    std::unique_ptr<juce::FileChooser> pathChooser;
#endif

    // Deep Neural Network for pose estimation
    cv::dnn::Net                net;
    bool                        modelLoaded = false;
    juce::AudioParameterChoice* qualityParam = nullptr;
    juce::AudioParameterChoice* modelChoiceParam = nullptr;

    // Signal for the background thread to reload the model
    std::atomic<int> requestedModelIndex{-1};

    // Parameter IDs for modulation
    static constexpr auto paramIdConfidence = "confidence";
    static constexpr auto paramIdConfidenceMod = "confidence_mod";

    // Source ID (read from input cable in audio thread, used by processing thread)
    std::atomic<juce::uint32> currentSourceId{0};

    // Cached resolved source ID from connection graph (for XML load before processBlock runs)
    juce::uint32 cachedResolvedSourceId{0};

    // Modulated confidence value (set by audio thread, read by processing thread)
    std::atomic<float> currentConfidenceThreshold{0.1f};

    // Lock-free FIFO for passing results from processing thread to audio thread
    PoseResult              lastResultForAudio;
    juce::AbstractFifo      fifo{16};
    std::vector<PoseResult> fifoBuffer;

    // UI preview
    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;

    // Cached last BGR frame for operations while source is paused/no new frames
    cv::Mat               lastFrameBgr;
    juce::CriticalSection frameLock;
};

// Keypoint names for the MPI model (for UI labels and debugging)
const std::vector<std::string> MPI_KEYPOINT_NAMES = {
    "Head",
    "Neck",
    "R Shoulder",
    "R Elbow",
    "R Wrist",
    "L Shoulder",
    "L Elbow",
    "L Wrist",
    "R Hip",
    "R Knee",
    "R Ankle",
    "L Hip",
    "L Knee",
    "L Ankle",
    "Chest"};

// Skeleton connections (pairs of keypoint indices to draw as lines)
const std::vector<std::pair<int, int>> MPI_SKELETON_PAIRS = {
    {0, 1},   // Head -> Neck
    {1, 14},  // Neck -> Chest
    {1, 2},   // Neck -> R Shoulder
    {2, 3},   // R Shoulder -> R Elbow
    {3, 4},   // R Elbow -> R Wrist
    {1, 5},   // Neck -> L Shoulder
    {5, 6},   // L Shoulder -> L Elbow
    {6, 7},   // L Elbow -> L Wrist
    {14, 8},  // Chest -> R Hip
    {8, 9},   // R Hip -> R Knee
    {9, 10},  // R Knee -> R Ankle
    {14, 11}, // Chest -> L Hip
    {11, 12}, // L Hip -> L Knee
    {12, 13}  // L Knee -> L Ankle
};
