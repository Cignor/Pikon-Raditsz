#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>

/**
 * Video Draw Impact Node - Allows users to draw colored "impact" marks on video frames.
 * Drawings persist for a configurable number of frames, creating visual rhythms
 * that can be tracked by the Color Tracker node.
 */
class VideoDrawImpactModuleProcessor : public ModuleProcessor, private juce::Thread
{
public:
    VideoDrawImpactModuleProcessor();
    ~VideoDrawImpactModuleProcessor() override;

    const juce::String getName() const override { return "video_draw_impact"; }
    
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    
    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }
    juce::Image getLatestFrame();
    
    juce::ValueTree getExtraStateTree() const override;
    void setExtraStateTree(const juce::ValueTree& state) override;
    
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth,
                              const std::function<bool(const juce::String& paramId)>& isParamModulated,
                              const std::function<void()>& onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
    ImVec2 getCustomNodeSize() const override;
    bool usesCustomPinLayout() const override { return true; }
    void enqueueDrawPointFromUi(int x, int y, bool isErase);
    void endUiStroke();
#endif

private:
    struct SourceTimelineState
    {
        double positionSeconds = 0.0;
        double durationSeconds = 0.0;
        double rangeStartSeconds = 0.0;
        double rangeEndSeconds = 0.0;
        bool isActive = false;
        bool isValid = false;
        bool hasRange = false;
        bool loopEnabled = false;
    };

    void run() override;
    void updateGuiFrame(const cv::Mat& frame);
    void applySaturation(cv::Mat& frame, float saturation);
    void updateDrawColorFromParams();
    void processPendingDrawOps();
    void drawStrokesOnFrame(cv::Mat& frame,
                            const SourceTimelineState& timelineState,
                            double frameDurationSeconds,
                            int currentFrameIndex);
    void cleanupExpiredKeyframes();
    void addDrawPoint(const cv::Point2i& point, bool isErase);
    SourceTimelineState getSourceTimelineState() const;
    bool eraseKeyframesNear(double targetValue,
                            float normalizedY,
                            bool timelineMode,
                            double valueTolerance,
                            float yTolerance,
                            double wrapLength);
    void updateKeyframePosition(int keyframeIndex, double newTimeSeconds, float newNormalizedY);
    
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    // Drawing data structures
    struct DrawingStroke
    {
        std::vector<cv::Point2i> points;
        cv::Scalar color;                 // BGR color (0-255)
        int remainingFrames;
        int brushSize;
        int startFrameNumber;
        bool isErase;
    };
    
    struct TimelineKeyframe
    {
        int frameNumber { 0 };
        double timeSeconds { 0.0 };
        double persistenceSeconds { 0.0 };
        cv::Scalar color;
        int brushSize { 0 };
        bool isErase { false };
        float normalizedX { 0.5f };  // 0.0-1.0
        float normalizedY { 0.5f };  // 0.0-1.0
    };
    
    struct PendingDrawOperation
    {
        cv::Point2i point;
        cv::Scalar color;
        int brushSize;
        bool isNewStroke;
        bool isErase;
    };
    
    juce::AudioProcessorValueTreeState apvts;
    
    // Parameters
    std::atomic<float>* saturationParam = nullptr;
    std::atomic<float>* drawColorRParam = nullptr;
    std::atomic<float>* drawColorGParam = nullptr;
    std::atomic<float>* drawColorBParam = nullptr;
    juce::AudioParameterInt* framePersistenceParam = nullptr;
    juce::AudioParameterInt* brushSizeParam = nullptr;
    juce::AudioParameterBool* clearDrawingsParam = nullptr;
    
    // Source ID (read from input pin)
    std::atomic<juce::uint32> currentSourceId { 0 };
    juce::uint32 cachedResolvedSourceId { 0 }; // For XML load before processBlock runs
    
    // Frame tracking
    std::atomic<int> currentFrameNumber { 0 };
    
    // Active drawings (thread-safe)
    std::vector<DrawingStroke> activeDrawings;
    juce::CriticalSection drawingsLock;
    
    // Pending drawing operations (from UI thread)
    std::vector<PendingDrawOperation> pendingDrawOps;
    juce::CriticalSection pendingOpsLock;
    
    // Timeline keyframes
    std::vector<TimelineKeyframe> timelineKeyframes;
    juce::CriticalSection timelineLock;
    
    // Current drawing color (BGR, 0-255)
    cv::Scalar currentDrawColor { 0, 0, 255 };  // Default red
    
    // Color history palette (stores RGB as floats 0-1, most recent first)
    std::vector<ImVec4> usedColors;
    juce::CriticalSection colorHistoryLock;
    static constexpr int MAX_COLOR_HISTORY = 12;  // Maximum number of colors to remember
    
    // UI preview
    juce::Image latestFrameForGui;
    juce::CriticalSection imageLock;
    
    // Frame caching (for handling paused/loading scenarios, like ColorTrackerModule)
    cv::Mat lastFrameBgr;
    juce::CriticalSection frameLock;
    
    // Timeline tracking
    std::atomic<double> lastTimelinePositionSeconds { 0.0 };
    std::atomic<double> lastFrameDurationSeconds { 1.0 / 30.0 };
    
    // Drawing state (UI thread)
    bool isDrawing { false };
    bool lastWasErase { false };
    cv::Point2i lastDrawPoint { -1, -1 };
    
    // Cached logical ID
    juce::uint32 storedLogicalId { 0 };
    
    // Timeline zoom (pixels per second)
    float zoomPixelsPerSecond = 50.0f;
    float timelineScrollPixels = 0.0f;
    
    // Keyframe dragging state (UI thread only)
    int draggingKeyframeIndex { -1 };
    float dragOffsetX { 0.0f };
    float dragOffsetY { 0.0f };
    bool isDraggingKeyframe { false };
};

