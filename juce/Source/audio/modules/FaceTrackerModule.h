#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/objdetect.hpp>
#if WITH_CUDA_SUPPORT
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#endif
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>

constexpr int FACE_NUM_KEYPOINTS = 70;

struct FaceResult
{
    float keypoints[FACE_NUM_KEYPOINTS][2] = {{0}};
    float faceCenterX = -1.0f; // Face center X (absolute screen position)
    float faceCenterY = -1.0f; // Face center Y (absolute screen position)
    int   detectedPoints = 0;
    bool  zoneHits[4] = {false, false, false, false}; // Zone hit detection results
};

class FaceTrackerModule : public ModuleProcessor, private juce::Thread
{
public:
    FaceTrackerModule();
    ~FaceTrackerModule() override;

    const juce::String getName() const override { return "face_tracker"; }
    juce::Image        getLatestFrame();

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

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void   drawIoPins(const NodePinHelpers& helpers) override;
    bool   usesCustomPinLayout() const override { return true; }
    ImVec2 getCustomNodeSize() const override;
#endif

private:
    void run() override;
    void updateGuiFrame(const cv::Mat& frame);
    void parseFaceOutput(const cv::Mat& netOutput, const cv::Rect& faceBox, FaceResult& result);
    void loadModel();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    std::atomic<float>*       sourceIdParam = nullptr;
    std::atomic<float>*       confidenceThresholdParam = nullptr;
    std::atomic<float>*       zoomLevelParam = nullptr;
    juce::AudioParameterBool* useGpuParam = nullptr;

    cv::CascadeClassifier faceCascade;
    cv::dnn::Net          net;
    bool                  modelLoaded = false;

    std::atomic<juce::uint32> currentSourceId{0};
    juce::uint32              cachedResolvedSourceId{0};

    FaceResult              lastResultForAudio;
    juce::AbstractFifo      fifo{16};
    std::vector<FaceResult> fifoBuffer;

    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;

    cv::Mat               lastFrameBgr;
    juce::CriticalSection frameLock;

    juce::uint32 storedLogicalId{0};
};
