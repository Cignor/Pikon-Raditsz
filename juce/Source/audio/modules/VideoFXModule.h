#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/core/cuda.hpp>
#endif

/**
 * A "Swiss Army knife" video processing node.
 * Takes a source ID as input, applies a chain of effects, and outputs a new
 * source ID for the processed video stream, allowing for effect chaining.
 */
class VideoFXModule : public ModuleProcessor, private juce::Thread
{
public:
    VideoFXModule();
    ~VideoFXModule() override;

    const juce::String getName() const override { return "video_fx"; }

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

    // --- CPU EFFECT FUNCTIONS ---
    void applyBrightnessContrast(cv::Mat& ioFrame, float brightness, float contrast);
    void applyGamma(cv::Mat& ioFrame, float gamma);
    void applyTemperature(cv::Mat& ioFrame, float temperature);
    void applySepia(cv::Mat& ioFrame, bool sepia);
    void applySaturationHue(cv::Mat& ioFrame, float saturation, float hueShift);
    void applyRgbGain(cv::Mat& ioFrame, float gainR, float gainG, float gainB);
    void applyLevels(cv::Mat& ioFrame, float black, float white, float gamma);
    void applyChannelMixer(
        cv::Mat& ioFrame,
        float    rr,
        float    rg,
        float    rb,
        float    gr,
        float    gg,
        float    gb,
        float    br,
        float    bg,
        float    bb);
    void applyBlendColor(cv::Mat& ioFrame, float amount, float r, float g, float b, int blendMode);
    void applyPosterize(cv::Mat& ioFrame, int levels);
    void applyGrayscale(cv::Mat& ioFrame, bool grayscale);
    void applyCanny(cv::Mat& ioFrame, float thresh1, float thresh2);
    void applyThreshold(cv::Mat& ioFrame, float level);
    void applyInvert(cv::Mat& ioFrame, bool invert);
    void applySolarize(cv::Mat& ioFrame, float threshold);
    void applyEmboss(cv::Mat& ioFrame, float strength);
    void applyNoise(cv::Mat& ioFrame, float amount);
    void applyEdgeGlow(cv::Mat& ioFrame, float amount, float r, float g, float b);
    void applyBlur(cv::Mat& ioFrame, float blur);
    void applySharpen(cv::Mat& ioFrame, float sharpen);
    void applyFlip(cv::Mat& ioFrame, bool flipH, bool flipV);
    void applyMirror(cv::Mat& ioFrame, int mode);
    void applyRotation(cv::Mat& ioFrame, int mode);
    void applyVignette(cv::Mat& ioFrame, float amount, float size);
    void applyPixelate(cv::Mat& ioFrame, int pixelSize);
    void applyKaleidoscope(cv::Mat& ioFrame, int mode);
    void applyScanlines(cv::Mat& ioFrame, float amount, int spacing);
    void applyDithering(cv::Mat& ioFrame, int levels);
    void applyChromaAberration(cv::Mat& ioFrame, float amount);
    void applyZoomBlur(cv::Mat& ioFrame, float amount);

#if defined(WITH_CUDA_SUPPORT)
    // --- Reusable GPU Buffers ---
    cv::cuda::GpuMat              gpuTemp;     // 8-bit, 3-channel
    cv::cuda::GpuMat              gpuGray;     // 8-bit, 1-channel
    std::vector<cv::cuda::GpuMat> gpuChannels; // 8-bit, 1-channel (x3)

    // --- NEW BUFFERS FOR PHASE 2C ---

    // Buffers for Sharpen (16-bit signed)
    cv::cuda::GpuMat gpuTemp16S;
    cv::cuda::GpuMat gpuBlurred16S;

    // Buffers for Sat/Hue (32-bit float)
    cv::cuda::GpuMat gpuTempF1;
    cv::cuda::GpuMat gpuTempF2;
    cv::cuda::GpuMat gpuMask; // Mask for comparisons

    // Buffers for Kaleidoscope (8-bit, 3-channel)
    cv::cuda::GpuMat gpuQuadrant;
    cv::cuda::GpuMat gpuFlipH;
    cv::cuda::GpuMat gpuFlipV;
    cv::cuda::GpuMat gpuFlipHV;

    // Buffers for Vignette (caching)
    cv::Mat          cpuVignetteMask; // 32-bit, 1-channel (CPU cache)
    cv::cuda::GpuMat gpuVignetteMask; // 32-bit, 1-channel (GPU cache)
    int              lastVignetteW = 0, lastVignetteH = 0;
    float            lastVignetteAmount = -1.f, lastVignetteSize = -1.f;

    // Buffers for Wet/Dry Blending
    cv::cuda::GpuMat gpuPreColor;
    cv::cuda::GpuMat gpuPreFilters;
    cv::cuda::GpuMat gpuPreMoreFilters;
    cv::cuda::GpuMat gpuPreAdvanced;
    cv::cuda::GpuMat gpuPreNewEffects;
    cv::cuda::GpuMat gpuPreDistortion;

    // --- End of new buffers ---

    // --- GPU EFFECT FUNCTIONS ---
    void applyBrightnessContrast_gpu(cv::cuda::GpuMat& ioFrame, float brightness, float contrast);
    void applyGamma_gpu(cv::cuda::GpuMat& ioFrame, float gamma);
    void applyTemperature_gpu(cv::cuda::GpuMat& ioFrame, float temperature);
    void applySepia_gpu(cv::cuda::GpuMat& ioFrame, bool sepia);
    void applySaturationHue_gpu(cv::cuda::GpuMat& ioFrame, float saturation, float hueShift);
    void applyRgbGain_gpu(cv::cuda::GpuMat& ioFrame, float gainR, float gainG, float gainB);
    void applyLevels_gpu(cv::cuda::GpuMat& ioFrame, float black, float white, float gamma);
    void applyChannelMixer_gpu(
        cv::cuda::GpuMat& ioFrame,
        float             rr,
        float             rg,
        float             rb,
        float             gr,
        float             gg,
        float             gb,
        float             br,
        float             bg,
        float             bb);
    void applyBlendColor_gpu(
        cv::cuda::GpuMat& ioFrame,
        float             amount,
        float             r,
        float             g,
        float             b,
        int               blendMode);
    void applyPosterize_gpu(cv::cuda::GpuMat& ioFrame, int levels);
    void applyGrayscale_gpu(cv::cuda::GpuMat& ioFrame, bool grayscale);
    void applyCanny_gpu(cv::cuda::GpuMat& ioFrame, float thresh1, float thresh2);
    void applyThreshold_gpu(cv::cuda::GpuMat& ioFrame, float level);
    void applyInvert_gpu(cv::cuda::GpuMat& ioFrame, bool invert);
    void applySolarize_gpu(cv::cuda::GpuMat& ioFrame, float threshold);
    void applyEmboss_gpu(cv::cuda::GpuMat& ioFrame, float strength);
    void applyNoise_gpu(cv::cuda::GpuMat& ioFrame, float amount);
    void applyEdgeGlow_gpu(cv::cuda::GpuMat& ioFrame, float amount, float r, float g, float b);
    void applyBlur_gpu(cv::cuda::GpuMat& ioFrame, float blur);
    void applySharpen_gpu(cv::cuda::GpuMat& ioFrame, float sharpen);
    void applyFlip_gpu(cv::cuda::GpuMat& ioFrame, bool flipH, bool flipV);
    void applyMirror_gpu(cv::cuda::GpuMat& ioFrame, int mode);
    void applyRotation_gpu(cv::cuda::GpuMat& ioFrame, int mode);
    void applyVignette_gpu(cv::cuda::GpuMat& ioFrame, float amount, float size);
    void applyPixelate_gpu(cv::cuda::GpuMat& ioFrame, int pixelSize);
    void applyKaleidoscope_gpu(cv::cuda::GpuMat& ioFrame, int mode);
    void applyScanlines_gpu(cv::cuda::GpuMat& ioFrame, float amount, int spacing);
    void applyDithering_gpu(cv::cuda::GpuMat& ioFrame, int levels);
    void applyChromaAberration_gpu(cv::cuda::GpuMat& ioFrame, float amount);
    void applyZoomBlur_gpu(cv::cuda::GpuMat& ioFrame, float amount);
#endif

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // === ARCHITECTURE ===
    std::atomic<float>*       zoomLevelParam = nullptr;
    juce::AudioParameterBool* useGpuParam = nullptr;
    std::atomic<float>*       wetDryMixParam = nullptr;

    // === COLOR ===
    std::atomic<float>*         brightnessParam = nullptr;
    std::atomic<float>*         contrastParam = nullptr;
    std::atomic<float>*         gammaParam = nullptr;
    std::atomic<float>*         saturationParam = nullptr;
    std::atomic<float>*         hueShiftParam = nullptr;
    std::atomic<float>*         gainRedParam = nullptr;
    std::atomic<float>*         gainGreenParam = nullptr;
    std::atomic<float>*         gainBlueParam = nullptr;
    juce::AudioParameterBool*   sepiaParam = nullptr;
    std::atomic<float>*         temperatureParam = nullptr;
    std::atomic<float>*         levelsBlackParam = nullptr;
    std::atomic<float>*         levelsWhiteParam = nullptr;
    std::atomic<float>*         levelsGammaParam = nullptr;
    juce::AudioParameterBool*   channelMixerEnableParam = nullptr;
    std::atomic<float>*         mixRRParam = nullptr;
    std::atomic<float>*         mixRGParam = nullptr;
    std::atomic<float>*         mixRBParam = nullptr;
    std::atomic<float>*         mixGRParam = nullptr;
    std::atomic<float>*         mixGGParam = nullptr;
    std::atomic<float>*         mixGBParam = nullptr;
    std::atomic<float>*         mixBRParam = nullptr;
    std::atomic<float>*         mixBGParam = nullptr;
    std::atomic<float>*         mixBBParam = nullptr;
    std::atomic<float>*         blendColorAmountParam = nullptr;
    std::atomic<float>*         blendColorRParam = nullptr;
    std::atomic<float>*         blendColorGParam = nullptr;
    std::atomic<float>*         blendColorBParam = nullptr;
    juce::AudioParameterChoice* blendModeParam = nullptr;

    // === FILTERS ===
    std::atomic<float>*       sharpenParam = nullptr;
    std::atomic<float>*       blurParam = nullptr;
    juce::AudioParameterBool* embossParam = nullptr;
    std::atomic<float>*       embossStrengthParam = nullptr;
    std::atomic<float>*       noiseAmountParam = nullptr;
    juce::AudioParameterBool* grayscaleParam = nullptr;
    juce::AudioParameterBool* invertParam = nullptr;
    juce::AudioParameterBool* solarizeParam = nullptr;
    std::atomic<float>*       solarizeThresholdParam = nullptr;

    // === GEOMETRIC ===
    juce::AudioParameterBool*   flipHorizontalParam = nullptr;
    juce::AudioParameterBool*   flipVerticalParam = nullptr;
    juce::AudioParameterChoice* mirrorModeParam = nullptr;
    juce::AudioParameterChoice* rotationModeParam = nullptr;

    // === THRESHOLD & EDGE ===
    juce::AudioParameterBool* thresholdEnableParam = nullptr;
    std::atomic<float>*       thresholdLevelParam = nullptr;
    juce::AudioParameterBool* cannyEnableParam = nullptr;
    std::atomic<float>*       cannyThresh1Param = nullptr;
    std::atomic<float>*       cannyThresh2Param = nullptr;
    std::atomic<float>*       edgeGlowAmountParam = nullptr;
    std::atomic<float>*       edgeGlowRParam = nullptr;
    std::atomic<float>*       edgeGlowGParam = nullptr;
    std::atomic<float>*       edgeGlowBParam = nullptr;

    // === STYLIZE ===
    juce::AudioParameterInt*    posterizeLevelsParam = nullptr;
    std::atomic<float>*         vignetteAmountParam = nullptr;
    std::atomic<float>*         vignetteSizeParam = nullptr;
    juce::AudioParameterInt*    pixelateBlockSizeParam = nullptr;
    juce::AudioParameterChoice* kaleidoscopeModeParam = nullptr;
    std::atomic<float>*         scanlineAmountParam = nullptr;
    juce::AudioParameterInt*    scanlineSpacingParam = nullptr;
    juce::AudioParameterBool*   ditherEnableParam = nullptr;
    juce::AudioParameterInt*    ditherLevelsParam = nullptr;

    // === DISTORTION ===
    std::atomic<float>* chromaAberrationParam = nullptr;
    std::atomic<float>* zoomBlurAmountParam = nullptr;

    // === WET/DRY MIXERS ===
    juce::AudioParameterFloat* wetDryColorParam = nullptr;
    juce::AudioParameterFloat* wetDryFiltersParam = nullptr;
    juce::AudioParameterFloat* wetDryMoreFiltersParam = nullptr;
    juce::AudioParameterFloat* wetDryAdvancedParam = nullptr;
    juce::AudioParameterFloat* wetDryNewEffectsParam = nullptr;
    juce::AudioParameterFloat* wetDryDistortionParam = nullptr;

    // Source ID (read from input pin)
    std::atomic<juce::uint32> currentSourceId{0};
    juce::uint32              cachedResolvedSourceId{0};

    // UI Preview
    juce::Image           latestFrameForGui;
    juce::CriticalSection imageLock;

    cv::Mat               lastFrameBgr;
    juce::CriticalSection frameLock;

    // Buffers for Wet/Dry Blending (CPU)
    cv::Mat cpuPreColor;
    cv::Mat cpuPreFilters;
    cv::Mat cpuPreMoreFilters;
    cv::Mat cpuPreAdvanced;
    cv::Mat cpuPreNewEffects;
    cv::Mat cpuPreDistortion;

    juce::uint32 storedLogicalId{0};
};
