#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#endif

/**
 * Chromakey video processing node.
 * Removes selected colors from video frames and converts them to alpha transparency.
 * Supports multiple color selection with ColorTracker-style HSV range matching,
 * median sampling, spill suppression, and edge feathering.
 */
class ChromakeyModuleProcessor : public ModuleProcessor, private juce::Thread
{
public:
    ChromakeyModuleProcessor();
    ~ChromakeyModuleProcessor() override;

    const juce::String getName() const override { return "chromakey"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }
    juce::Image                         getLatestFrame();

    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& state) override;

    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
    void   drawIoPins(const NodePinHelpers& helpers) override;
    ImVec2 getCustomNodeSize() const override;
#endif

private:
    void run() override;
    void updateGuiFrame(const cv::Mat& frame);

    // Color management structure (ColorTracker-style with HSV range bounds)
    struct ChromakeyColor
    {
        juce::Colour displayColour;    // For UI swatch display
        cv::Scalar   hsvLower;         // HSV lower bounds (H: 0-179, S: 0-255, V: 0-255)
        cv::Scalar   hsvUpper;         // HSV upper bounds
        float        tolerance = 1.0f; // Multiplier that scales the HSV window
        bool         inverted = false; // Invert this color's mask

        ChromakeyColor()
            : displayColour(juce::Colours::black), hsvLower(0, 100, 100), hsvUpper(10, 255, 255),
              tolerance(1.0f), inverted(false)
        {
        }
    };

    // Chromakey processing functions
    void    applyChromakey(cv::Mat& inputFrame, cv::Mat& outputRgba, cv::Mat& outputAlpha);
    cv::Mat createColorMask(const cv::Mat& hsvFrame, const ChromakeyColor& color);
    void    applySpillSuppression(cv::Mat& bgrFrame, const cv::Mat& alphaMask, float amount);
    void    applyFeathering(cv::Mat& alphaMask, float featherAmount);

#if defined(WITH_CUDA_SUPPORT)
    // GPU-accelerated mask creation using cuda::inRange
    // Full GPU pipeline
    void applyChromakeyGpu(
        const cv::cuda::GpuMat& inputFrame,
        cv::cuda::GpuMat&       outputRgba,
        cv::cuda::GpuMat&       outputAlpha);

    // Persistent GPU buffers to avoid thrashing (cudaMalloc is slow)
    cv::cuda::GpuMat gpuHsv;
    cv::cuda::GpuMat gpuCombinedMask;
    cv::cuda::GpuMat gpuTempMask;
    cv::cuda::GpuMat gpuAlphaMask;

    // Output buffers (persistent)
    cv::cuda::GpuMat gpuRgbaOutput;
    cv::cuda::GpuMat gpuAlphaOutput;
#endif

    // Color management
    void addColorAt(int x, int y, int radius); // Add/update color with median sampling
    void removeColor(int index);
    void setColorInverted(int index, bool inverted);
    void setColorTolerance(int index, float tolerance);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Parameters
    juce::AudioParameterBool* useGpuParam = nullptr;
    std::atomic<float>*       zoomLevelParam = nullptr;
    std::atomic<float>*       spillSuppressionParam = nullptr; // 0.0 to 1.0
    std::atomic<float>*       featherAmountParam = nullptr;    // 0.0 to 20.0

    // Color list (thread-safe access required)
    std::vector<ChromakeyColor>   selectedColors;
    mutable juce::CriticalSection colorListLock;

    // Picker state (ColorTracker-style)
    std::atomic<bool> isColorPickerActive{false};
    std::atomic<int>  pickerTargetIndex{-1}; // -1 = add new, >=0 = update existing

    // Source ID (read from input pin)
    std::atomic<juce::uint32> currentSourceId{0};
    juce::uint32              cachedResolvedSourceId{0};

    // UI Preview
    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;

    cv::Mat               lastFrameBgr;
    juce::CriticalSection frameLock;

    // Original input frame for accurate eyedropper sampling (before chroma processing)
    cv::Mat               originalInputFrame;
    juce::CriticalSection originalFrameLock;

    juce::uint32 storedLogicalId{0};
};
