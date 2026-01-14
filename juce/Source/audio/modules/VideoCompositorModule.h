#pragma once

#include "ModuleProcessor.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>

#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/cudafilters.hpp>
#endif

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

/**
 * Video compositor that stacks multiple video layers with blend modes and transforms.
 * Supports up to 8 layers with independent opacity, blend mode, position, and scale controls.
 */
class VideoCompositorModule : public ModuleProcessor, private juce::Thread
{
public:
    VideoCompositorModule();
    ~VideoCompositorModule() override;

    const juce::String getName() const override { return "video_compositor"; }

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

    // Blend mode enum
    enum class BlendMode
    {
        Normal = 0,
        Add,
        Multiply,
        Screen,
        Overlay,
        SoftLight,
        HardLight,
        Darken,
        Lighten,
        Difference,
        Exclusion,
        ColorDodge,
        ColorBurn,
        LinearDodge,
        LinearBurn,
        VividLight,
        LinearLight,
        PinLight,
        HardMix
    };

    // Compositing functions
    // Compositing functions
    void compositeLayer(
        cv::Mat&       canvas,
        const cv::Mat& layer,
        BlendMode      mode,
        float          opacity,
        float          posX,
        float          posY,
        float          scaleX,
        float          scaleY);
    cv::Mat applyTransforms(
        const cv::Mat& src,
        float          posX,
        float          posY,
        float          scaleX,
        float          scaleY,
        int            canvasWidth,
        int            canvasHeight);
    void applyBlendMode(cv::Mat& dst, const cv::Mat& src, BlendMode mode, float opacity);

#if defined(WITH_CUDA_SUPPORT)
    // GPU Compositing functions
    void compositeLayer_gpu(
        cv::cuda::GpuMat&       canvas,
        const cv::cuda::GpuMat& layer,
        BlendMode               mode,
        float                   opacity,
        float                   posX,
        float                   posY,
        float                   scaleX,
        float                   scaleY);
    cv::cuda::GpuMat applyTransforms_gpu(
        const cv::cuda::GpuMat& src,
        float                   posX,
        float                   posY,
        float                   scaleX,
        float                   scaleY,
        int                     canvasWidth,
        int                     canvasHeight);
    void applyBlendMode_gpu(
        cv::cuda::GpuMat&       dst,
        const cv::cuda::GpuMat& src,
        BlendMode               mode,
        float                   opacity);
#endif

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Global parameters
    juce::AudioParameterInt*  numLayersParam = nullptr;
    juce::AudioParameterBool* useGpuParam = nullptr;

    // Per-layer parameters (arrays for 8 layers)
    static constexpr int        MAX_LAYERS = 8;
    std::atomic<float>*         layerOpacityParams[MAX_LAYERS] = {nullptr};
    juce::AudioParameterChoice* layerBlendModeParams[MAX_LAYERS] = {nullptr};
    std::atomic<float>*         layerPosXParams[MAX_LAYERS] = {nullptr};
    std::atomic<float>*         layerPosYParams[MAX_LAYERS] = {nullptr};
    std::atomic<float>*         layerScaleXParams[MAX_LAYERS] = {nullptr};
    std::atomic<float>*         layerScaleYParams[MAX_LAYERS] = {nullptr};

    // Source IDs for each layer (read from input pins)
    // Using fixed-size array since std::atomic is not copyable/movable
    std::atomic<juce::uint32> layerSourceIds[MAX_LAYERS];

    // Cached frames per layer (for preview when disconnected)
    std::vector<cv::Mat>  lastLayerFrames;
    juce::CriticalSection framesLock;

#if defined(WITH_CUDA_SUPPORT)
    // GPU Buffers
    cv::cuda::GpuMat gpuCanvas;
    cv::cuda::GpuMat gpuTemp;
    cv::cuda::GpuMat gpuScaled;
    cv::cuda::GpuMat gpuTransformed;

    // Buffers for blend mode calculations
    cv::cuda::GpuMat gpuDstF;
    cv::cuda::GpuMat gpuSrcF;
    cv::cuda::GpuMat gpuResultF;
    cv::cuda::GpuMat gpuMask;

    // Layer frames (upload buffer)
    std::vector<cv::cuda::GpuMat> gpuLayerFrames;
#endif

    std::atomic<bool> gpuHasFailed{false};

    // UI Preview
    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;

    juce::uint32 storedLogicalId{0};
};
