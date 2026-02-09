#include "VideoFXModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include "../../utils/CudaDeviceCountCache.h"
#include <opencv2/imgproc.hpp>
#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#include <opencv2/cudawarping.hpp>
#endif

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h> // For WorkRect workaround to fix widget bleeding in nodes
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout VideoFXModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // === ARCHITECTURE ===
    params.push_back(std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", true));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("wetDryMix", "Wet/Dry Mix", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Zoom Level", juce::StringArray{"Small", "Normal", "Large"}, 1));

    // === COLOR ADJUSTMENTS ===
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "brightness", "Brightness", -100.0f, 100.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("contrast", "Contrast", 0.0f, 3.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("gamma", "Gamma", 0.1f, 3.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("saturation", "Saturation", 0.0f, 3.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "hueShift", "Hue Shift", -180.0f, 180.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("gainRed", "Red Gain", 0.0f, 2.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("gainGreen", "Green Gain", 0.0f, 2.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("gainBlue", "Blue Gain", 0.0f, 2.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>("sepia", "Sepia", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "temperature", "Temperature", -1.0f, 1.0f, 0.0f));

    // Levels (Black/White Point)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "levelsBlack", "Levels Black", 0.0f, 255.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "levelsWhite", "Levels White", 0.0f, 255.0f, 255.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "levelsGamma", "Levels Gamma", 0.1f, 3.0f, 1.0f));

    // Channel Mixer (9 coefficients for 3x3 matrix)
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("channelMixerEnable", "Channel Mixer", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixRR", "R->R", -2.0f, 2.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixRG", "G->R", -2.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixRB", "B->R", -2.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixGR", "R->G", -2.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixGG", "G->G", -2.0f, 2.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixGB", "B->G", -2.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixBR", "R->B", -2.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixBG", "G->B", -2.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("mixBB", "B->B", -2.0f, 2.0f, 1.0f));

    // Blend with Solid Color
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "blendColorAmount", "Color Blend", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("blendColorR", "Blend Red", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "blendColorG", "Blend Green", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("blendColorB", "Blend Blue", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "blendMode",
            "Blend Mode",
            juce::StringArray{"Normal", "Multiply", "Screen", "Overlay", "Add"},
            0));

    // === FILTERS & EFFECTS ===
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("sharpen", "Sharpen", 0.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("blur", "Blur", 0.0f, 20.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>("emboss", "Emboss", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "embossStrength", "Emboss Strength", 0.5f, 3.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "noiseAmount", "Noise/Grain", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>("grayscale", "Grayscale", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("invert", "Invert Colors", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("solarize", "Solarize", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "solarizeThreshold", "Solarize Thresh", 0.0f, 255.0f, 128.0f));

    // === GEOMETRIC ===
    params.push_back(std::make_unique<juce::AudioParameterBool>("flipH", "Flip Horizontal", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("flipV", "Flip Vertical", false));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "mirror",
            "Mirror",
            juce::StringArray{"None", "Left-Right", "Right-Left", "Top-Bottom", "Bottom-Top"},
            0));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "rotation", "Rotation", juce::StringArray{"0", "90", "180", "270"}, 0));

    // Threshold & Edge
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("thresholdEnable", "Enable Threshold", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "thresholdLevel", "Threshold Level", 0.0f, 255.0f, 127.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("cannyEnable", "Edge Detect", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "cannyThresh1", "Canny Thresh 1", 0.0f, 255.0f, 50.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "cannyThresh2", "Canny Thresh 2", 0.0f, 255.0f, 150.0f));

    // Edge Glow
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "edgeGlowAmount", "Edge Glow", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("edgeGlowR", "Glow Red", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("edgeGlowG", "Glow Green", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("edgeGlowB", "Glow Blue", 0.0f, 1.0f, 1.0f));

    // === STYLIZE ===
    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            "posterizeLevels", "Posterize Levels", 2, 16, 16));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "vignetteAmount", "Vignette Amount", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "vignetteSize", "Vignette Size", 0.1f, 2.0f, 0.5f));
    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            "pixelateSize", "Pixelate Block Size", 1, 128, 1));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "kaleidoscope", "Kaleidoscope", juce::StringArray{"None", "4-Way", "8-Way"}, 0));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "scanlineAmount", "Scanlines", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterInt>("scanlineSpacing", "Scanline Spacing", 1, 10, 2));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("ditherEnable", "Dithering", false));
    params.push_back(
        std::make_unique<juce::AudioParameterInt>("ditherLevels", "Dither Levels", 2, 8, 4));

    // === DISTORTION ===
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "chromaAberration", "Chromatic Aberration", 0.0f, 20.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "zoomBlurAmount", "Zoom Blur", 0.0f, 1.0f, 0.0f));

    // === WET/DRY MIXERS ===
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wetDryColor", "Color Wet/Dry", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wetDryFilters", "Filters Wet/Dry", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wetDryMoreFilters", "More Filters Wet/Dry", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wetDryAdvanced", "Advanced Wet/Dry", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wetDryNewEffects", "New Effects Wet/Dry", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wetDryDistortion", "Distortion Wet/Dry", 0.0f, 1.0f, 1.0f));

    return {params.begin(), params.end()};
}

VideoFXModule::VideoFXModule()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::mono(), true)
              .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      juce::Thread("VideoFX Thread"),
      apvts(*this, nullptr, "VideoFXParams", createParameterLayout())
{
    // === ARCHITECTURE ===
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));
    wetDryMixParam = apvts.getRawParameterValue("wetDryMix");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");

    // === COLOR ===
    brightnessParam = apvts.getRawParameterValue("brightness");
    contrastParam = apvts.getRawParameterValue("contrast");
    gammaParam = apvts.getRawParameterValue("gamma");
    saturationParam = apvts.getRawParameterValue("saturation");
    hueShiftParam = apvts.getRawParameterValue("hueShift");
    gainRedParam = apvts.getRawParameterValue("gainRed");
    gainGreenParam = apvts.getRawParameterValue("gainGreen");
    gainBlueParam = apvts.getRawParameterValue("gainBlue");
    sepiaParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("sepia"));
    temperatureParam = apvts.getRawParameterValue("temperature");

    // Levels
    levelsBlackParam = apvts.getRawParameterValue("levelsBlack");
    levelsWhiteParam = apvts.getRawParameterValue("levelsWhite");
    levelsGammaParam = apvts.getRawParameterValue("levelsGamma");

    // Channel Mixer
    channelMixerEnableParam =
        dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("channelMixerEnable"));
    mixRRParam = apvts.getRawParameterValue("mixRR");
    mixRGParam = apvts.getRawParameterValue("mixRG");
    mixRBParam = apvts.getRawParameterValue("mixRB");
    mixGRParam = apvts.getRawParameterValue("mixGR");
    mixGGParam = apvts.getRawParameterValue("mixGG");
    mixGBParam = apvts.getRawParameterValue("mixGB");
    mixBRParam = apvts.getRawParameterValue("mixBR");
    mixBGParam = apvts.getRawParameterValue("mixBG");
    mixBBParam = apvts.getRawParameterValue("mixBB");

    // Blend Color
    blendColorAmountParam = apvts.getRawParameterValue("blendColorAmount");
    blendColorRParam = apvts.getRawParameterValue("blendColorR");
    blendColorGParam = apvts.getRawParameterValue("blendColorG");
    blendColorBParam = apvts.getRawParameterValue("blendColorB");
    blendModeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("blendMode"));

    // === FILTERS ===
    sharpenParam = apvts.getRawParameterValue("sharpen");
    blurParam = apvts.getRawParameterValue("blur");
    embossParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("emboss"));
    embossStrengthParam = apvts.getRawParameterValue("embossStrength");
    noiseAmountParam = apvts.getRawParameterValue("noiseAmount");
    grayscaleParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("grayscale"));
    invertParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("invert"));
    solarizeParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("solarize"));
    solarizeThresholdParam = apvts.getRawParameterValue("solarizeThreshold");

    // === GEOMETRIC ===
    flipHorizontalParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("flipH"));
    flipVerticalParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("flipV"));
    mirrorModeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("mirror"));
    rotationModeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("rotation"));

    // === THRESHOLD & EDGE ===
    thresholdEnableParam =
        dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("thresholdEnable"));
    thresholdLevelParam = apvts.getRawParameterValue("thresholdLevel");
    cannyEnableParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("cannyEnable"));
    cannyThresh1Param = apvts.getRawParameterValue("cannyThresh1");
    cannyThresh2Param = apvts.getRawParameterValue("cannyThresh2");
    edgeGlowAmountParam = apvts.getRawParameterValue("edgeGlowAmount");
    edgeGlowRParam = apvts.getRawParameterValue("edgeGlowR");
    edgeGlowGParam = apvts.getRawParameterValue("edgeGlowG");
    edgeGlowBParam = apvts.getRawParameterValue("edgeGlowB");

    // === STYLIZE ===
    posterizeLevelsParam =
        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("posterizeLevels"));
    vignetteAmountParam = apvts.getRawParameterValue("vignetteAmount");
    vignetteSizeParam = apvts.getRawParameterValue("vignetteSize");
    pixelateBlockSizeParam =
        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("pixelateSize"));
    kaleidoscopeModeParam =
        dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("kaleidoscope"));
    scanlineAmountParam = apvts.getRawParameterValue("scanlineAmount");
    scanlineSpacingParam =
        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("scanlineSpacing"));
    ditherEnableParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("ditherEnable"));
    ditherLevelsParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("ditherLevels"));

    // === DISTORTION ===
    chromaAberrationParam = apvts.getRawParameterValue("chromaAberration");
    zoomBlurAmountParam = apvts.getRawParameterValue("zoomBlurAmount");

    // === WET/DRY MIXERS ===
    wetDryColorParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("wetDryColor"));
    wetDryFiltersParam =
        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("wetDryFilters"));
    wetDryMoreFiltersParam =
        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("wetDryMoreFilters"));
    wetDryAdvancedParam =
        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("wetDryAdvanced"));
    wetDryNewEffectsParam =
        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("wetDryNewEffects"));
    wetDryDistortionParam =
        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("wetDryDistortion"));
}

VideoFXModule::~VideoFXModule()
{
    stopThread(5000);
    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void VideoFXModule::prepareToPlay(double, int) { startThread(); }
void VideoFXModule::releaseResources()
{
    signalThreadShouldExit();
    stopThread(5000);
}

// ==============================================================================
// === PRIVATE EFFECT HELPER FUNCTIONS ==========================================
// ==============================================================================

void VideoFXModule::applyBrightnessContrast(cv::Mat& ioFrame, float brightness, float contrast)
{
    if (brightness != 0.0f || contrast != 1.0f)
    {
        ioFrame.convertTo(ioFrame, -1, contrast, brightness);
    }
}

void VideoFXModule::applyTemperature(cv::Mat& ioFrame, float temperature)
{
    if (temperature == 0.0f)
        return;

    std::vector<cv::Mat> bgr;
    cv::split(ioFrame, bgr);
    float factor = temperature;

    // Original logic was wrong (applied same effect for warm/cool)
    // This is corrected:
    if (factor < 0.0f)
    {                                      // Cool (add blue, remove red)
        bgr[0] = bgr[0] * (1.0f - factor); // Add Blue
        bgr[2] = bgr[2] * (1.0f + factor); // Remove Red
    }
    else
    {                                      // Warm (add red, remove blue)
        bgr[0] = bgr[0] * (1.0f - factor); // Remove Blue
        bgr[2] = bgr[2] * (1.0f + factor); // Add Red
    }

    cv::merge(bgr, ioFrame);
}

void VideoFXModule::applySepia(cv::Mat& ioFrame, bool sepia)
{
    if (!sepia)
        return;
    cv::Mat sepiaKernel =
        (cv::Mat_<float>(3, 3) << 0.272, 0.534, 0.131, 0.349, 0.686, 0.168, 0.393, 0.769, 0.189);
    cv::transform(ioFrame, ioFrame, sepiaKernel);
}

void VideoFXModule::applySaturationHue(cv::Mat& ioFrame, float saturation, float hueShift)
{
    if (saturation == 1.0f && hueShift == 0.0f)
        return;

    cv::Mat hsv;
    cv::cvtColor(ioFrame, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Mat> hsvChannels;
    cv::split(hsv, hsvChannels);

    if (hueShift != 0.0f)
    {
        hsvChannels[0].convertTo(hsvChannels[0], CV_32F);
        hsvChannels[0] += (hueShift / 2.0f);
        cv::Mat mask = hsvChannels[0] < 0;
        cv::add(hsvChannels[0], 180, hsvChannels[0], mask);
        mask = hsvChannels[0] >= 180;
        cv::subtract(hsvChannels[0], 180, hsvChannels[0], mask);
        hsvChannels[0].convertTo(hsvChannels[0], CV_8U);
    }
    if (saturation != 1.0f)
    {
        hsvChannels[1].convertTo(hsvChannels[1], CV_32F);
        hsvChannels[1] *= saturation;
        hsvChannels[1].convertTo(hsvChannels[1], CV_8U);
    }
    cv::merge(hsvChannels, hsv);
    cv::cvtColor(hsv, ioFrame, cv::COLOR_HSV2BGR);
}

void VideoFXModule::applyRgbGain(cv::Mat& ioFrame, float gainR, float gainG, float gainB)
{
    if (gainR == 1.0f && gainG == 1.0f && gainB == 1.0f)
        return;

    std::vector<cv::Mat> bgr;
    cv::split(ioFrame, bgr);
    if (gainB != 1.0f)
        bgr[0] *= gainB;
    if (gainG != 1.0f)
        bgr[1] *= gainG;
    if (gainR != 1.0f)
        bgr[2] *= gainR;
    cv::merge(bgr, ioFrame);
}

void VideoFXModule::applyPosterize(cv::Mat& ioFrame, int levels)
{
    // levels is 2-16. 16 is "off".
    if (levels >= 16)
        return;
    if (levels < 2)
        levels = 2;

    // --- NEW, ROBUST LOGIC ---
    // This math correctly maps the 0-255 range to 'levels' number of steps.
    // e.g., if levels = 2, it maps to 0 and 255.
    // e.g., if levels = 3, it maps to 0, 127, 255.

    // 1. Calculate the divider
    const int divider = 255 / (levels - 1); // e.g., 255 / (2-1) = 255

    // 2. Convert to 16-bit to prevent overflow during math
    ioFrame.convertTo(ioFrame, CV_16U);

    // 3. Scale down, round, and scale back up
    // This is an integer-math version of:
    // ioFrame = round(ioFrame / divider) * divider;
    ioFrame = (ioFrame + (divider / 2)) / divider;
    ioFrame = ioFrame * divider;

    // 4. Convert back to 8-bit
    ioFrame.convertTo(ioFrame, CV_8U);
}

void VideoFXModule::applyGrayscale(cv::Mat& ioFrame, bool grayscale)
{
    if (!grayscale)
        return;
    cv::cvtColor(ioFrame, ioFrame, cv::COLOR_BGR2GRAY);
    cv::cvtColor(ioFrame, ioFrame, cv::COLOR_GRAY2BGR);
}

void VideoFXModule::applyCanny(cv::Mat& ioFrame, float thresh1, float thresh2)
{
    // Canny needs a grayscale image
    cv::Mat gray;
    cv::cvtColor(ioFrame, gray, cv::COLOR_BGR2GRAY);
    cv::Canny(gray, gray, thresh1, thresh2);
    cv::cvtColor(gray, ioFrame, cv::COLOR_GRAY2BGR);
}

void VideoFXModule::applyThreshold(cv::Mat& ioFrame, float level)
{
    cv::Mat gray;
    cv::cvtColor(ioFrame, gray, cv::COLOR_BGR2GRAY);
    cv::threshold(gray, gray, level, 255, cv::THRESH_BINARY);
    cv::cvtColor(gray, ioFrame, cv::COLOR_GRAY2BGR);
}

void VideoFXModule::applyInvert(cv::Mat& ioFrame, bool invert)
{
    if (!invert)
        return;
    cv::bitwise_not(ioFrame, ioFrame);
}

void VideoFXModule::applyFlip(cv::Mat& ioFrame, bool flipH, bool flipV)
{
    if (!flipH && !flipV)
        return;
    int flipCode = flipH && flipV ? -1 : (flipH ? 1 : 0);
    cv::flip(ioFrame, ioFrame, flipCode);
}

void VideoFXModule::applyVignette(cv::Mat& ioFrame, float amount, float size)
{
    if (amount <= 0.0f)
        return;

    cv::Mat vignette = cv::Mat::zeros(ioFrame.size(), CV_32F);
    int     centerX = ioFrame.cols / 2;
    int     centerY = ioFrame.rows / 2;
    float   maxDist = std::sqrt((float)centerX * centerX + (float)centerY * centerY) * size;
    if (maxDist <= 0.0f)
        maxDist = 1.0f; // Avoid divide by zero

    for (int y = 0; y < ioFrame.rows; y++)
    {
        for (int x = 0; x < ioFrame.cols; x++)
        {
            float dist = std::sqrt(std::pow(x - centerX, 2) + std::pow(y - centerY, 2));
            float v = 1.0f - (dist / maxDist) * amount;
            vignette.at<float>(y, x) = juce::jlimit(0.0f, 1.0f, v);
        }
    }

    std::vector<cv::Mat> bgr;
    cv::split(ioFrame, bgr);
    for (size_t i = 0; i < bgr.size(); i++)
    {
        bgr[i].convertTo(bgr[i], CV_32F);
        cv::multiply(bgr[i], vignette, bgr[i]);
        bgr[i].convertTo(bgr[i], CV_8U);
    }
    cv::merge(bgr, ioFrame);
}

void VideoFXModule::applyPixelate(cv::Mat& ioFrame, int pixelSize)
{
    if (pixelSize <= 1)
        return;

    int     w = ioFrame.cols;
    int     h = ioFrame.rows;
    cv::Mat temp;
    cv::resize(ioFrame, temp, cv::Size(w / pixelSize, h / pixelSize), 0, 0, cv::INTER_NEAREST);
    cv::resize(temp, ioFrame, cv::Size(w, h), 0, 0, cv::INTER_NEAREST);
}

void VideoFXModule::applyBlur(cv::Mat& ioFrame, float blur)
{
    // *** THIS IS THE BUG FIX ***
    // We use a small threshold and ensure the kernel size is an odd number > 1.

    if (blur <= 0.1f)
        return; // No blur

    // Round to nearest integer, not truncate
    int ksize = static_cast<int>(std::round(blur));

    // Ensure kernel size is ODD
    if (ksize % 2 == 0)
        ksize++;

    // Ensure kernel size is at least 3
    if (ksize < 3)
        ksize = 3;

    cv::GaussianBlur(ioFrame, ioFrame, cv::Size(ksize, ksize), 0);
}

void VideoFXModule::applySharpen(cv::Mat& ioFrame, float sharpen)
{
    if (sharpen <= 0.0f)
        return;

    cv::Mat temp;
    ioFrame.convertTo(temp, CV_16SC3);
    cv::Mat blurred;
    cv::GaussianBlur(temp, blurred, cv::Size(0, 0), 3);
    cv::addWeighted(temp, 1.0 + sharpen, blurred, -sharpen, 0, temp);
    temp.convertTo(ioFrame, CV_8UC3);
}

void VideoFXModule::applyKaleidoscope(cv::Mat& ioFrame, int mode)
{
    if (mode == 0)
        return; // "None"

    int w = ioFrame.cols;
    int h = ioFrame.rows;
    int halfW = w / 2;
    int halfH = h / 2;

    // Ensure we have at least 2x2 pixels
    if (halfW < 1 || halfH < 1)
        return;

    cv::Mat quadrant = ioFrame(cv::Rect(0, 0, halfW, halfH)).clone();

    if (mode == 1)
    { // 4-Way
        cv::Mat flippedH, flippedV, flippedBoth;
        cv::flip(quadrant, flippedH, 1);
        cv::flip(quadrant, flippedV, 0);
        cv::flip(quadrant, flippedBoth, -1);
        quadrant.copyTo(ioFrame(cv::Rect(0, 0, halfW, halfH)));
        flippedH.copyTo(ioFrame(cv::Rect(halfW, 0, halfW, halfH)));
        flippedV.copyTo(ioFrame(cv::Rect(0, halfH, halfW, halfH)));
        flippedBoth.copyTo(ioFrame(cv::Rect(halfW, halfH, halfW, halfH)));
    }
    else if (mode == 2)
    { // 8-Way
        cv::Mat                symmQuadrant = quadrant.clone();
        cv::Mat                mask = cv::Mat::zeros(quadrant.size(), CV_8U);
        std::vector<cv::Point> triangle_pts = {
            cv::Point(0, 0), cv::Point(halfW, 0), cv::Point(0, halfH)};
        cv::fillConvexPoly(mask, triangle_pts, cv::Scalar(255));

        cv::Mat tri;
        quadrant.copyTo(tri, mask);
        cv::Mat tri_flipped_h;
        cv::flip(tri, tri_flipped_h, 1);
        tri_flipped_h.copyTo(symmQuadrant, ~mask);

        cv::Mat flippedH, flippedV, flippedBoth;
        cv::flip(symmQuadrant, flippedH, 1);
        cv::flip(symmQuadrant, flippedV, 0);
        cv::flip(symmQuadrant, flippedBoth, -1);
        symmQuadrant.copyTo(ioFrame(cv::Rect(0, 0, halfW, halfH)));
        flippedH.copyTo(ioFrame(cv::Rect(halfW, 0, halfW, halfH)));
        flippedV.copyTo(ioFrame(cv::Rect(0, halfH, halfW, halfH)));
        flippedBoth.copyTo(ioFrame(cv::Rect(halfW, halfH, halfW, halfH)));
    }
}

// ==============================================================================
// === NEW CPU EFFECT FUNCTIONS =================================================
// ==============================================================================

void VideoFXModule::applyGamma(cv::Mat& ioFrame, float gamma)
{
    if (gamma == 1.0f)
        return;
    cv::Mat lut(1, 256, CV_8U);
    for (int i = 0; i < 256; i++)
        lut.at<uchar>(i) = cv::saturate_cast<uchar>(std::pow(i / 255.0f, gamma) * 255.0f);
    cv::LUT(ioFrame, lut, ioFrame);
}

void VideoFXModule::applyLevels(cv::Mat& ioFrame, float black, float white, float gamma)
{
    if (black == 0.0f && white == 255.0f && gamma == 1.0f)
        return;
    float range = white - black;
    if (range <= 0)
        range = 1.0f;
    cv::Mat lut(1, 256, CV_8U);
    for (int i = 0; i < 256; i++)
    {
        float normalized = (i - black) / range;
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        if (gamma != 1.0f)
            normalized = std::pow(normalized, gamma);
        lut.at<uchar>(i) = cv::saturate_cast<uchar>(normalized * 255.0f);
    }
    cv::LUT(ioFrame, lut, ioFrame);
}

void VideoFXModule::applyChannelMixer(
    cv::Mat& ioFrame,
    float    rr,
    float    rg,
    float    rb,
    float    gr,
    float    gg,
    float    gb,
    float    br,
    float    bg,
    float    bb)
{
    if (rr == 1.0f && rg == 0.0f && rb == 0.0f && gr == 0.0f && gg == 1.0f && gb == 0.0f &&
        br == 0.0f && bg == 0.0f && bb == 1.0f)
        return;
    cv::Mat kernel = (cv::Mat_<float>(3, 3) << bb, bg, br, gb, gg, gr, rb, rg, rr);
    cv::transform(ioFrame, ioFrame, kernel);
}

void VideoFXModule::applyBlendColor(
    cv::Mat& ioFrame,
    float    amount,
    float    r,
    float    g,
    float    b,
    int      blendMode)
{
    if (amount <= 0.0f)
        return;
    cv::Mat overlay(ioFrame.size(), ioFrame.type(), cv::Scalar(b * 255, g * 255, r * 255));
    cv::Mat blended;
    switch (blendMode)
    {
    case 0:
        cv::addWeighted(ioFrame, 1.0f - amount, overlay, amount, 0, blended);
        break;
    case 1:
        cv::multiply(ioFrame, overlay, blended, 1.0 / 255.0);
        cv::addWeighted(ioFrame, 1.0f - amount, blended, amount, 0, blended);
        break;
    case 4:
        cv::add(ioFrame, overlay * amount, blended);
        break;
    default:
        cv::addWeighted(ioFrame, 1.0f - amount, overlay, amount, 0, blended);
        break;
    }
    blended.copyTo(ioFrame);
}

void VideoFXModule::applySolarize(cv::Mat& ioFrame, float threshold)
{
    cv::Mat lut(1, 256, CV_8U);
    for (int i = 0; i < 256; i++)
        lut.at<uchar>(i) = (i > threshold) ? (255 - i) : i;
    cv::LUT(ioFrame, lut, ioFrame);
}

void VideoFXModule::applyEmboss(cv::Mat& ioFrame, float strength)
{
    cv::Mat kernel =
        (cv::Mat_<float>(3, 3) << -2 * strength,
         -strength,
         0,
         -strength,
         1,
         strength,
         0,
         strength,
         2 * strength);
    cv::filter2D(ioFrame, ioFrame, -1, kernel);
    ioFrame += cv::Scalar(128, 128, 128);
}

void VideoFXModule::applyNoise(cv::Mat& ioFrame, float amount)
{
    if (amount <= 0.0f)
        return;
    cv::Mat noise(ioFrame.size(), ioFrame.type());
    cv::randn(noise, 0, amount * 50);
    cv::add(ioFrame, noise, ioFrame);
}

void VideoFXModule::applyMirror(cv::Mat& ioFrame, int mode)
{
    if (mode == 0)
        return;
    int     w = ioFrame.cols, h = ioFrame.rows, halfW = w / 2, halfH = h / 2;
    cv::Mat half, flipped;
    switch (mode)
    {
    case 1:
        half = ioFrame(cv::Rect(0, 0, halfW, h));
        cv::flip(half, flipped, 1);
        flipped.copyTo(ioFrame(cv::Rect(halfW, 0, halfW, h)));
        break;
    case 2:
        half = ioFrame(cv::Rect(halfW, 0, halfW, h));
        cv::flip(half, flipped, 1);
        flipped.copyTo(ioFrame(cv::Rect(0, 0, halfW, h)));
        break;
    case 3:
        half = ioFrame(cv::Rect(0, 0, w, halfH));
        cv::flip(half, flipped, 0);
        flipped.copyTo(ioFrame(cv::Rect(0, halfH, w, halfH)));
        break;
    case 4:
        half = ioFrame(cv::Rect(0, halfH, w, halfH));
        cv::flip(half, flipped, 0);
        flipped.copyTo(ioFrame(cv::Rect(0, 0, w, halfH)));
        break;
    }
}

void VideoFXModule::applyRotation(cv::Mat& ioFrame, int mode)
{
    if (mode == 0)
        return;
    switch (mode)
    {
    case 1:
        cv::rotate(ioFrame, ioFrame, cv::ROTATE_90_CLOCKWISE);
        break;
    case 2:
        cv::rotate(ioFrame, ioFrame, cv::ROTATE_180);
        break;
    case 3:
        cv::rotate(ioFrame, ioFrame, cv::ROTATE_90_COUNTERCLOCKWISE);
        break;
    }
}

void VideoFXModule::applyScanlines(cv::Mat& ioFrame, float amount, int spacing)
{
    if (amount <= 0.0f)
        return;
    float darken = 1.0f - amount;
    for (int y = 0; y < ioFrame.rows; y += spacing)
        ioFrame.row(y) *= darken;
}

void VideoFXModule::applyDithering(cv::Mat& ioFrame, int levels)
{
    if (levels >= 256)
        return;
    float   step = 255.0f / (levels - 1);
    cv::Mat lut(1, 256, CV_8U);
    for (int i = 0; i < 256; i++)
        lut.at<uchar>(i) = cv::saturate_cast<uchar>(std::round(i / step) * step);
    cv::LUT(ioFrame, lut, ioFrame);
}

void VideoFXModule::applyChromaAberration(cv::Mat& ioFrame, float amount)
{
    if (amount <= 0.0f)
        return;

    // Handle both BGR (3 channel) and BGRA (4 channel) frames
    cv::Mat alpha;
    cv::Mat bgrFrame;
    if (ioFrame.channels() == 4)
    {
        // Extract alpha channel and convert to BGR for processing
        std::vector<cv::Mat> channels;
        cv::split(ioFrame, channels);
        alpha = channels[3].clone();
        cv::merge(std::vector<cv::Mat>{channels[0], channels[1], channels[2]}, bgrFrame);
    }
    else if (ioFrame.channels() == 3)
    {
        bgrFrame = ioFrame;
    }
    else
    {
        // Unsupported format - skip
        return;
    }

    std::vector<cv::Mat> channels;
    cv::split(bgrFrame, channels);
    int     shift = static_cast<int>(amount);
    cv::Mat M_r = (cv::Mat_<float>(2, 3) << 1, 0, shift, 0, 1, 0);
    cv::Mat M_b = (cv::Mat_<float>(2, 3) << 1, 0, -shift, 0, 1, 0);
    cv::warpAffine(channels[2], channels[2], M_r, bgrFrame.size());
    cv::warpAffine(channels[0], channels[0], M_b, bgrFrame.size());
    cv::merge(channels, bgrFrame);

    // Restore alpha channel if present
    if (!alpha.empty())
    {
        std::vector<cv::Mat> finalChannels;
        cv::split(bgrFrame, finalChannels);
        finalChannels.push_back(alpha);
        cv::merge(finalChannels, ioFrame);
    }
    else
    {
        ioFrame = bgrFrame;
    }
}

void VideoFXModule::applyEdgeGlow(cv::Mat& ioFrame, float amount, float r, float g, float b)
{
    if (amount <= 0.0f)
        return;
    cv::Mat gray, edges;
    cv::cvtColor(ioFrame, gray, cv::COLOR_BGR2GRAY);
    cv::Canny(gray, edges, 50, 150);
    cv::dilate(edges, edges, cv::Mat(), cv::Point(-1, -1), 2);
    cv::Mat glow(ioFrame.size(), ioFrame.type(), cv::Scalar(b * 255, g * 255, r * 255));
    cv::Mat mask;
    cv::cvtColor(edges, mask, cv::COLOR_GRAY2BGR);
    mask.convertTo(mask, CV_32F, 1.0 / 255.0);
    cv::Mat glowBlended;
    cv::multiply(glow, mask, glowBlended, 1.0, CV_8U);
    cv::addWeighted(ioFrame, 1.0, glowBlended, amount, 0, ioFrame);
}

void VideoFXModule::applyZoomBlur(cv::Mat& ioFrame, float amount)
{
    if (amount <= 0.0f)
        return;
    cv::Mat blurred, accumulated = ioFrame.clone();
    accumulated.convertTo(accumulated, CV_32F);
    int   steps = static_cast<int>(amount * 10) + 1;
    float cx = ioFrame.cols / 2.0f, cy = ioFrame.rows / 2.0f;
    for (int i = 1; i <= steps; i++)
    {
        float   scale = 1.0f + (amount * 0.02f * i);
        cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(cx, cy), 0, scale);
        cv::warpAffine(ioFrame, blurred, M, ioFrame.size());
        cv::Mat temp;
        blurred.convertTo(temp, CV_32F);
        accumulated += temp;
    }
    accumulated /= (steps + 1);
    accumulated.convertTo(ioFrame, CV_8U);
}

void VideoFXModule::run()
{
    cv::Mat processedFrame;
#if defined(WITH_CUDA_SUPPORT)
    cv::cuda::GpuMat gpuFrame;
#endif

    while (!threadShouldExit())
    {
        try
        {
            juce::uint32 sourceId = currentSourceId.load();
            cv::Mat      prefetchedFrame;

            if (sourceId == 0)
            {
                if (cachedResolvedSourceId != 0)
                {
                    sourceId = cachedResolvedSourceId;
                }
                else if (parentSynth != nullptr)
                {
                    auto snapshot = parentSynth->getConnectionSnapshot();
                    if (snapshot && !snapshot->empty())
                    {
                        juce::uint32 myLogicalId = storedLogicalId;
                        if (myLogicalId == 0)
                        {
                            for (const auto& info : parentSynth->getModulesInfo())
                            {
                                if (parentSynth->getModuleForLogical(info.first) == this)
                                {
                                    myLogicalId = info.first;
                                    storedLogicalId = myLogicalId;
                                    break;
                                }
                            }
                        }

                        if (myLogicalId != 0)
                        {
                            for (const auto& conn : *snapshot)
                            {
                                if (conn.dstLogicalId == myLogicalId && conn.dstChan == 0)
                                {
                                    sourceId = conn.srcLogicalId;
                                    cachedResolvedSourceId = sourceId;
                                    break;
                                }
                            }
                        }
                    }

                    if (sourceId == 0)
                    {
                        for (const auto& info : parentSynth->getModulesInfo())
                        {
                            juce::String moduleType = info.second.toLowerCase();
                            if (moduleType.contains("video") || moduleType.contains("webcam") ||
                                moduleType == "video_file_loader")
                            {
                                cv::Mat testFrame =
                                    VideoFrameManager::getInstance().getFrame(info.first);
                                if (!testFrame.empty())
                                {
                                    sourceId = info.first;
                                    cachedResolvedSourceId = sourceId;
                                    prefetchedFrame = testFrame;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                if (cachedResolvedSourceId != 0 && cachedResolvedSourceId != sourceId)
                    cachedResolvedSourceId = 0;
            }

            // --- 1. Get all parameter values first (to decide GPU vs CPU) ---
            float brightness = brightnessParam ? brightnessParam->load() : 0.0f;
            float contrast = contrastParam ? contrastParam->load() : 1.0f;
            float saturation = saturationParam ? saturationParam->load() : 1.0f;
            float hueShift = hueShiftParam ? hueShiftParam->load() : 0.0f;
            float gainR = gainRedParam ? gainRedParam->load() : 1.0f;
            float gainG = gainGreenParam ? gainGreenParam->load() : 1.0f;
            float gainB = gainBlueParam ? gainBlueParam->load() : 1.0f;
            bool  sepia = sepiaParam ? sepiaParam->get() : false;
            float temperature = temperatureParam ? temperatureParam->load() : 0.0f;
            float sharpen = sharpenParam ? sharpenParam->load() : 0.0f;
            float blur = blurParam ? blurParam->load() : 0.0f;
            bool  grayscale = grayscaleParam ? grayscaleParam->get() : false;
            bool  invert = invertParam ? invertParam->get() : false;
            bool  flipH = flipHorizontalParam ? flipHorizontalParam->get() : false;
            bool  flipV = flipVerticalParam ? flipVerticalParam->get() : false;
            bool  thresholdEnable = thresholdEnableParam ? thresholdEnableParam->get() : false;
            float thresholdLevel = thresholdLevelParam ? thresholdLevelParam->load() : 127.0f;
            int   posterizeLevels = posterizeLevelsParam ? posterizeLevelsParam->get() : 16;
            float vignetteAmount = vignetteAmountParam ? vignetteAmountParam->load() : 0.0f;
            float vignetteSize = vignetteSizeParam ? vignetteSizeParam->load() : 0.5f;
            int   pixelateSize = pixelateBlockSizeParam ? pixelateBlockSizeParam->get() : 1;
            bool  cannyEnable = cannyEnableParam ? cannyEnableParam->get() : false;
            float cannyThresh1 = cannyThresh1Param ? cannyThresh1Param->load() : 50.0f;
            float cannyThresh2 = cannyThresh2Param ? cannyThresh2Param->load() : 150.0f;
            int   kaleidoscopeMode = kaleidoscopeModeParam ? kaleidoscopeModeParam->getIndex() : 0;

            // === NEW PARAMETERS ===
            float wetDryMix = wetDryMixParam ? wetDryMixParam->load() : 1.0f;
            float gamma = gammaParam ? gammaParam->load() : 1.0f;
            float levelsBlack = levelsBlackParam ? levelsBlackParam->load() : 0.0f;
            float levelsWhite = levelsWhiteParam ? levelsWhiteParam->load() : 255.0f;
            float levelsGamma = levelsGammaParam ? levelsGammaParam->load() : 1.0f;
            bool  channelMixerEnable =
                channelMixerEnableParam ? channelMixerEnableParam->get() : false;
            float mixRR = mixRRParam ? mixRRParam->load() : 1.0f;
            float mixRG = mixRGParam ? mixRGParam->load() : 0.0f;
            float mixRB = mixRBParam ? mixRBParam->load() : 0.0f;
            float mixGR = mixGRParam ? mixGRParam->load() : 0.0f;
            float mixGG = mixGGParam ? mixGGParam->load() : 1.0f;
            float mixGB = mixGBParam ? mixGBParam->load() : 0.0f;
            float mixBR = mixBRParam ? mixBRParam->load() : 0.0f;
            float mixBG = mixBGParam ? mixBGParam->load() : 0.0f;
            float mixBB = mixBBParam ? mixBBParam->load() : 1.0f;
            float blendColorAmount = blendColorAmountParam ? blendColorAmountParam->load() : 0.0f;
            float blendColorR = blendColorRParam ? blendColorRParam->load() : 1.0f;
            float blendColorG = blendColorGParam ? blendColorGParam->load() : 0.0f;
            float blendColorB = blendColorBParam ? blendColorBParam->load() : 0.0f;
            int   blendMode = blendModeParam ? blendModeParam->getIndex() : 0;
            bool  emboss = embossParam ? embossParam->get() : false;
            float embossStrength = embossStrengthParam ? embossStrengthParam->load() : 1.0f;
            float noiseAmount = noiseAmountParam ? noiseAmountParam->load() : 0.0f;
            bool  solarize = solarizeParam ? solarizeParam->get() : false;
            float solarizeThreshold =
                solarizeThresholdParam ? solarizeThresholdParam->load() : 128.0f;
            int   mirrorMode = mirrorModeParam ? mirrorModeParam->getIndex() : 0;
            int   rotationMode = rotationModeParam ? rotationModeParam->getIndex() : 0;
            float edgeGlowAmount = edgeGlowAmountParam ? edgeGlowAmountParam->load() : 0.0f;
            float edgeGlowR = edgeGlowRParam ? edgeGlowRParam->load() : 0.0f;
            float edgeGlowG = edgeGlowGParam ? edgeGlowGParam->load() : 1.0f;
            float edgeGlowB = edgeGlowBParam ? edgeGlowBParam->load() : 1.0f;
            float scanlineAmount = scanlineAmountParam ? scanlineAmountParam->load() : 0.0f;
            int   scanlineSpacing = scanlineSpacingParam ? scanlineSpacingParam->get() : 2;
            bool  ditherEnable = ditherEnableParam ? ditherEnableParam->get() : false;
            int   ditherLevels = ditherLevelsParam ? ditherLevelsParam->get() : 4;
            float chromaAberration = chromaAberrationParam ? chromaAberrationParam->load() : 0.0f;
            float zoomBlurAmount = zoomBlurAmountParam ? zoomBlurAmountParam->load() : 0.0f;

            const bool useGpu = useGpuParam ? useGpuParam->get() : false;
#if defined(WITH_CUDA_SUPPORT)
            const bool gpuAvailable = CudaDeviceCountCache::isAvailable();
#else
            const bool gpuAvailable = false;
#endif

            // Force CPU path when Canny is enabled to avoid hybrid GPU/CPU stalls
            bool runOnGpu = (useGpu && gpuAvailable && !cannyEnable);

            cv::Mat frame;
            bool    gpuInputReady = false;

            if (runOnGpu)
            {
#if defined(WITH_CUDA_SUPPORT)
                // Try to fetch GPU frame first
                if (sourceId != 0)
                {
                    cv::cuda::GpuMat gFrame =
                        VideoFrameManager::getInstance().getGpuFrame(sourceId);
                    if (!gFrame.empty())
                    {
                        gpuFrame = gFrame; // Use the reference or copy? GpuMat is ref-counted.
                        // But we need a writable copy if we modify it in place?
                        // The apply functions function in-place usually or take src/dst.
                        // Let's ensure we work on a copy if we want to preserve input (though we
                        // are consumimg it). Actually VideoFX usually acts as a filter. We should
                        // clone if we don't own it. But GpuMat clone is deep copy. We can just rely
                        // on the fact that we output to a NEW gpuFrame or modify in place if safe.
                        // But we set `gpuFrame` to it.
                        gpuInputReady = true;
                    }
                }
#endif
            }

            if (!gpuInputReady)
            {
                // CPU Fetch Fallback
                frame = prefetchedFrame.empty()
                            ? VideoFrameManager::getInstance().getFrame(sourceId)
                            : prefetchedFrame;

                if (!frame.empty())
                {
                    const juce::ScopedLock lk(frameLock);
                    frame.copyTo(lastFrameBgr);
                }
                else
                {
                    const juce::ScopedLock lk(frameLock);
                    if (!lastFrameBgr.empty())
                        frame = lastFrameBgr.clone();
                }
            }

            // If both failed
            if (!gpuInputReady && frame.empty())
            {
                wait(33);
                continue;
            }

            // Monitor start time
            auto startTime = juce::Time::getMillisecondCounterHiRes();

            if (runOnGpu)
            {
#if defined(WITH_CUDA_SUPPORT)
                try
                {
                    // Read Wet/Dry Params
                    float wetDryColor = wetDryColorParam ? wetDryColorParam->get() : 1.0f;
                    float wetDryFilters = wetDryFiltersParam ? wetDryFiltersParam->get() : 1.0f;
                    float wetDryMoreFilters =
                        wetDryMoreFiltersParam ? wetDryMoreFiltersParam->get() : 1.0f;
                    float wetDryAdvanced = wetDryAdvancedParam ? wetDryAdvancedParam->get() : 1.0f;
                    float wetDryNewEffects =
                        wetDryNewEffectsParam ? wetDryNewEffectsParam->get() : 1.0f;
                    float wetDryDistortion =
                        wetDryDistortionParam ? wetDryDistortionParam->get() : 1.0f;

                    if (!gpuInputReady)
                        gpuFrame.upload(frame);

                    // === 1. COLOR ADJUSTMENTS ===
                    if (wetDryColor < 1.0f)
                        gpuFrame.copyTo(gpuPreColor);

                    applyBrightnessContrast_gpu(gpuFrame, brightness, contrast);
                    applyGamma_gpu(gpuFrame, gamma);
                    applyTemperature_gpu(gpuFrame, temperature);
                    applySepia_gpu(gpuFrame, sepia);
                    applySaturationHue_gpu(gpuFrame, saturation, hueShift);
                    applyRgbGain_gpu(gpuFrame, gainR, gainG, gainB);

                    if (wetDryColor < 1.0f)
                        cv::cuda::addWeighted(
                            gpuPreColor, 1.0f - wetDryColor, gpuFrame, wetDryColor, 0.0, gpuFrame);

                    // === 2. FILTERS & EFFECTS ===
                    if (wetDryFilters < 1.0f)
                        gpuFrame.copyTo(gpuPreFilters);

                    applySharpen_gpu(gpuFrame, sharpen);
                    applyBlur_gpu(gpuFrame, blur);
                    applyGrayscale_gpu(gpuFrame, grayscale);
                    applyInvert_gpu(gpuFrame, invert);
                    applyFlip_gpu(gpuFrame, flipH, flipV);

                    if (wetDryFilters < 1.0f)
                        cv::cuda::addWeighted(
                            gpuPreFilters,
                            1.0f - wetDryFilters,
                            gpuFrame,
                            wetDryFilters,
                            0.0,
                            gpuFrame);

                    // === 3. MORE FILTERS ===
                    if (wetDryMoreFilters < 1.0f)
                        gpuFrame.copyTo(gpuPreMoreFilters);

                    if (thresholdEnable)
                        applyThreshold_gpu(gpuFrame, thresholdLevel);
                    applyPosterize_gpu(gpuFrame, posterizeLevels);
                    applyPixelate_gpu(gpuFrame, pixelateSize);
                    if (cannyEnable)
                        applyCanny_gpu(
                            gpuFrame, cannyThresh1, cannyThresh2); // Now supported on GPU!

                    if (wetDryMoreFilters < 1.0f)
                        cv::cuda::addWeighted(
                            gpuPreMoreFilters,
                            1.0f - wetDryMoreFilters,
                            gpuFrame,
                            wetDryMoreFilters,
                            0.0,
                            gpuFrame);

                    // === 4. ADVANCED EFFECTS ===
                    if (wetDryAdvanced < 1.0f)
                        gpuFrame.copyTo(gpuPreAdvanced);

                    applyVignette_gpu(gpuFrame, vignetteAmount, vignetteSize);
                    applyKaleidoscope_gpu(gpuFrame, kaleidoscopeMode);
                    if (channelMixerEnable)
                        applyChannelMixer_gpu(
                            gpuFrame,
                            mixRR,
                            mixRG,
                            mixRB,
                            mixGR,
                            mixGG,
                            mixGB,
                            mixBR,
                            mixBG,
                            mixBB);
                    applyBlendColor_gpu(
                        gpuFrame,
                        blendColorAmount,
                        blendColorR,
                        blendColorG,
                        blendColorB,
                        blendMode);

                    if (wetDryAdvanced < 1.0f)
                        cv::cuda::addWeighted(
                            gpuPreAdvanced,
                            1.0f - wetDryAdvanced,
                            gpuFrame,
                            wetDryAdvanced,
                            0.0,
                            gpuFrame);

                    // === 5. NEW EFFECTS ===
                    if (wetDryNewEffects < 1.0f)
                        gpuFrame.copyTo(gpuPreNewEffects);

                    applyGamma_gpu(gpuFrame, gamma);
                    applyLevels_gpu(gpuFrame, levelsBlack, levelsWhite, levelsGamma);
                    applyNoise_gpu(gpuFrame, noiseAmount);
                    if (solarize)
                        applySolarize_gpu(gpuFrame, solarizeThreshold);
                    if (emboss)
                        applyEmboss_gpu(gpuFrame, embossStrength);
                    applyMirror_gpu(gpuFrame, mirrorMode);
                    applyRotation_gpu(gpuFrame, rotationMode);
                    applyScanlines_gpu(gpuFrame, scanlineAmount, scanlineSpacing);
                    if (ditherEnable)
                        applyDithering_gpu(gpuFrame, ditherLevels);

                    if (wetDryNewEffects < 1.0f)
                        cv::cuda::addWeighted(
                            gpuPreNewEffects,
                            1.0f - wetDryNewEffects,
                            gpuFrame,
                            wetDryNewEffects,
                            0.0,
                            gpuFrame);

                    // === 6. DISTORTION ===
                    if (wetDryDistortion < 1.0f)
                        gpuFrame.copyTo(gpuPreDistortion);

                    applyEdgeGlow_gpu(gpuFrame, edgeGlowAmount, edgeGlowR, edgeGlowG, edgeGlowB);
                    applyChromaAberration_gpu(gpuFrame, chromaAberration);
                    applyZoomBlur_gpu(gpuFrame, zoomBlurAmount);

                    if (wetDryDistortion < 1.0f)
                        cv::cuda::addWeighted(
                            gpuPreDistortion,
                            1.0f - wetDryDistortion,
                            gpuFrame,
                            wetDryDistortion,
                            0.0,
                            gpuFrame);

                    // Publish result via GPU manager
                    juce::uint32 myId = storedLogicalId;
                    if (myId != 0)
                    {
                        VideoFrameManager::getInstance().setGpuFrame(myId, gpuFrame);
                    }

                    gpuFrame.download(processedFrame);
                }
                catch (const cv::Exception& e)
                {
                    juce::Logger::writeToLog(
                        "[VideoFX] GPU Error: " + juce::String(e.what()) +
                        ". Falling back to CPU.");
                    // If frame is empty (gpuInputReady was true), download from GPU
                    if (frame.empty() && !gpuFrame.empty())
                        gpuFrame.download(frame);
                    if (!frame.empty())
                        processedFrame = frame.clone();
                    goto cpu_path_label;
                }
#endif
            }
            else
            {
            cpu_path_label:
                // Skip CPU processing if frame is empty (guards against GPU fallback with empty
                // frame)
                if (frame.empty())
                {
                    wait(33);
                    continue;
                }
                processedFrame = frame.clone();

                // Read Wet/Dry Params
                float wetDryColor = wetDryColorParam ? wetDryColorParam->get() : 1.0f;
                float wetDryFilters = wetDryFiltersParam ? wetDryFiltersParam->get() : 1.0f;
                float wetDryMoreFilters =
                    wetDryMoreFiltersParam ? wetDryMoreFiltersParam->get() : 1.0f;
                float wetDryAdvanced = wetDryAdvancedParam ? wetDryAdvancedParam->get() : 1.0f;
                float wetDryNewEffects =
                    wetDryNewEffectsParam ? wetDryNewEffectsParam->get() : 1.0f;
                float wetDryDistortion =
                    wetDryDistortionParam ? wetDryDistortionParam->get() : 1.0f;

                // === 1. COLOR ADJUSTMENTS ===
                if (wetDryColor < 1.0f)
                    cpuPreColor = processedFrame.clone();

                applyBrightnessContrast(processedFrame, brightness, contrast);
                applyTemperature(processedFrame, temperature);
                applySepia(processedFrame, sepia);
                applySaturationHue(processedFrame, saturation, hueShift);
                applyRgbGain(processedFrame, gainR, gainG, gainB);

                if (wetDryColor < 1.0f)
                    cv::addWeighted(
                        cpuPreColor,
                        1.0f - wetDryColor,
                        processedFrame,
                        wetDryColor,
                        0.0,
                        processedFrame);

                // === 2. FILTERS & EFFECTS ===
                if (wetDryFilters < 1.0f)
                    cpuPreFilters = processedFrame.clone();

                applySharpen(processedFrame, sharpen);
                applyBlur(processedFrame, blur);
                applyGrayscale(processedFrame, grayscale);
                applyInvert(processedFrame, invert);
                applyFlip(processedFrame, flipH, flipV);

                if (wetDryFilters < 1.0f)
                    cv::addWeighted(
                        cpuPreFilters,
                        1.0f - wetDryFilters,
                        processedFrame,
                        wetDryFilters,
                        0.0,
                        processedFrame);

                // === 3. MORE FILTERS ===
                if (wetDryMoreFilters < 1.0f)
                    cpuPreMoreFilters = processedFrame.clone();

                if (thresholdEnable)
                    applyThreshold(processedFrame, thresholdLevel);
                applyPosterize(processedFrame, posterizeLevels);
                applyPixelate(processedFrame, pixelateSize);
                if (cannyEnable)
                    applyCanny(processedFrame, cannyThresh1, cannyThresh2);

                if (wetDryMoreFilters < 1.0f)
                    cv::addWeighted(
                        cpuPreMoreFilters,
                        1.0f - wetDryMoreFilters,
                        processedFrame,
                        wetDryMoreFilters,
                        0.0,
                        processedFrame);

                // === 4. ADVANCED EFFECTS ===
                if (wetDryAdvanced < 1.0f)
                    cpuPreAdvanced = processedFrame.clone();

                applyVignette(processedFrame, vignetteAmount, vignetteSize);
                applyKaleidoscope(processedFrame, kaleidoscopeMode);
                if (channelMixerEnable)
                    applyChannelMixer(
                        processedFrame,
                        mixRR,
                        mixRG,
                        mixRB,
                        mixGR,
                        mixGG,
                        mixGB,
                        mixBR,
                        mixBG,
                        mixBB);
                applyBlendColor(
                    processedFrame,
                    blendColorAmount,
                    blendColorR,
                    blendColorG,
                    blendColorB,
                    blendMode);

                if (wetDryAdvanced < 1.0f)
                    cv::addWeighted(
                        cpuPreAdvanced,
                        1.0f - wetDryAdvanced,
                        processedFrame,
                        wetDryAdvanced,
                        0.0,
                        processedFrame);

                // === 5. NEW EFFECTS ===
                if (wetDryNewEffects < 1.0f)
                    cpuPreNewEffects = processedFrame.clone();

                applyGamma(processedFrame, gamma);
                applyLevels(processedFrame, levelsBlack, levelsWhite, levelsGamma);
                applyNoise(processedFrame, noiseAmount);
                if (solarize)
                    applySolarize(processedFrame, solarizeThreshold);
                if (emboss)
                    applyEmboss(processedFrame, embossStrength);
                applyMirror(processedFrame, mirrorMode);
                applyRotation(processedFrame, rotationMode);
                applyScanlines(processedFrame, scanlineAmount, scanlineSpacing);
                if (ditherEnable)
                    applyDithering(processedFrame, ditherLevels);

                if (wetDryNewEffects < 1.0f)
                    cv::addWeighted(
                        cpuPreNewEffects,
                        1.0f - wetDryNewEffects,
                        processedFrame,
                        wetDryNewEffects,
                        0.0,
                        processedFrame);

                // === 6. DISTORTION ===
                if (wetDryDistortion < 1.0f)
                    cpuPreDistortion = processedFrame.clone();

                applyEdgeGlow(processedFrame, edgeGlowAmount, edgeGlowR, edgeGlowG, edgeGlowB);
                applyChromaAberration(processedFrame, chromaAberration);
                applyZoomBlur(processedFrame, zoomBlurAmount);

                if (wetDryDistortion < 1.0f)
                    cv::addWeighted(
                        cpuPreDistortion,
                        1.0f - wetDryDistortion,
                        processedFrame,
                        wetDryDistortion,
                        0.0,
                        processedFrame);
            }

            juce::uint32 myLogicalId = storedLogicalId;
            if (myLogicalId == 0 && parentSynth != nullptr)
            {
                for (const auto& info : parentSynth->getModulesInfo())
                {
                    if (parentSynth->getModuleForLogical(info.first) == this)
                    {
                        myLogicalId = info.first;
                        storedLogicalId = myLogicalId;
                        break;
                    }
                }
            }

            // --- 4. Publish and update UI ---
            // If we processed on GPU, we downloaded to processedFrame, so this works for both.
            // We publish to CPU manager for compatibility (though GPU manager has it too).
            // This ensures downstream CPU nodes still work.
            updateGuiFrame(processedFrame);
            if (myLogicalId != 0)
                VideoFrameManager::getInstance().setFrame(myLogicalId, processedFrame);

            auto elapsed = juce::Time::getMillisecondCounterHiRes() - startTime;
            lastProcessTimeMs = (float)elapsed;
            lastProcessWasGpu = runOnGpu;

            static int logCounter = 0;
            if (++logCounter % 60 == 0) // Log once per second approx
            {
                juce::Logger::writeToLog(
                    "[Perf] VideoFX: " + juce::String(elapsed, 2) +
                    " ms (GPU: " + (runOnGpu ? "ON" : "OFF") + ")");
            }

            wait(33); // ~30 FPS
        }
        catch (const cv::Exception& e)
        {
            juce::Logger::writeToLog(
                "[VideoFX] OpenCV Exception in run loop: " + juce::String(e.what()));
            wait(100); // Brief pause before retry
        }
        catch (const std::exception& e)
        {
            juce::Logger::writeToLog("[VideoFX] Exception in run loop: " + juce::String(e.what()));
            wait(100);
        }
        catch (...)
        {
            juce::Logger::writeToLog("[VideoFX] Unknown exception in run loop");
            wait(100);
        }
    }
}

void VideoFXModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // Read the Source ID from our input pin
    auto inputBuffer = getBusBuffer(buffer, true, 0);
    if (inputBuffer.getNumSamples() > 0)
    {
        currentSourceId.store((juce::uint32)inputBuffer.getSample(0, 0));
    }

    buffer.clear();

    // --- BEGIN FIX ---
    // Find our own ID if it's not set
    juce::uint32 myLogicalId = storedLogicalId;
    if (myLogicalId == 0 && parentSynth != nullptr)
    {
        for (const auto& info : parentSynth->getModulesInfo())
        {
            if (parentSynth->getModuleForLogical(info.first) == this)
            {
                myLogicalId = info.first;
                storedLogicalId = myLogicalId; // Cache it
                break;
            }
        }
    }
    // --- END FIX ---

    // Output our own Logical ID on the output pin, so we can be chained
    if (buffer.getNumChannels() > 0 && buffer.getNumSamples() > 0)
    {
        float sourceId = (float)myLogicalId; // Use the correct ID
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            buffer.setSample(0, sample, sourceId);
        }
    }
}

void VideoFXModule::updateGuiFrame(const cv::Mat& frame)
{
    cv::Mat bgraFrame;
    cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);

    const juce::ScopedLock lock(imageLock);

    if (latestFrameForGui.isNull() || latestFrameForGui.getWidth() != bgraFrame.cols ||
        latestFrameForGui.getHeight() != bgraFrame.rows)
    {
        latestFrameForGui = juce::Image(juce::Image::ARGB, bgraFrame.cols, bgraFrame.rows, true);
    }

    juce::Image::BitmapData destData(latestFrameForGui, juce::Image::BitmapData::writeOnly);
    memcpy(destData.data, bgraFrame.data, bgraFrame.total() * bgraFrame.elemSize());
}

juce::Image VideoFXModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

juce::ValueTree VideoFXModule::getExtraStateTree() const
{
    // No special state to save for VideoFX module
    return juce::ValueTree("VideoFXState");
}

void VideoFXModule::setExtraStateTree(const juce::ValueTree& state)
{
    // No special state to restore for VideoFX module
    juce::ignoreUnused(state);
}

std::vector<DynamicPinInfo> VideoFXModule::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"Source In", 0, PinDataType::Video});
    return pins;
}

std::vector<DynamicPinInfo> VideoFXModule::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"Output", 0, PinDataType::Video});
    return pins;
}

#if defined(PRESET_CREATOR_UI)
ImVec2 VideoFXModule::getCustomNodeSize() const
{
    // Return different width based on zoom level (0=240,1=480,2=960)
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void VideoFXModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto        themeText = [](const juce::String& text, const ImVec4& colour) {
        ThemeText(text.toRawUTF8(), colour);
    };

    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    // Widgets like CollapsingHeader, TreeNodeEx, SliderFloat use WorkRect.Max.x
    // which is the entire canvas in ImNodes, causing them to extend beyond node bounds.
    // Solution: Temporarily constrain WorkRect and ContentRegionRect to node width.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float  cursorScreenX = ImGui::GetCursorScreenPos().x;
    const float  nodeRightEdge = cursorScreenX + itemWidth;

    // Save original values
    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;

    // Constrain to node width
    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    ImGui::PushItemWidth(itemWidth);

    // --- FEATURE: RESET BUTTON ---
    if (ImGui::Button("Reset All Effects", ImVec2(itemWidth, 0)))
    {
        // Reset all parameters to their default values
        const char* paramIds[] = {
            // Original
            "useGpu",
            "zoomLevel",
            "brightness",
            "contrast",
            "saturation",
            "hueShift",
            "gainRed",
            "gainGreen",
            "gainBlue",
            "sepia",
            "temperature",
            "sharpen",
            "blur",
            "grayscale",
            "invert",
            "flipH",
            "flipV",
            "thresholdEnable",
            "thresholdLevel",
            "posterizeLevels",
            "vignetteAmount",
            "vignetteSize",
            "pixelateSize",
            "cannyEnable",
            "cannyThresh1",
            "cannyThresh2",
            "kaleidoscope",
            // NEW PARAMETERS
            "wetDryMix",
            "gamma",
            "levelsBlack",
            "levelsWhite",
            "levelsGamma",
            "channelMixerEnable",
            "mixRR",
            "mixRG",
            "mixRB",
            "mixGR",
            "mixGG",
            "mixGB",
            "mixBR",
            "mixBG",
            "mixBB",
            "blendColorAmount",
            "blendColorR",
            "blendColorG",
            "blendColorB",
            "blendMode",
            "emboss",
            "embossStrength",
            "noiseAmount",
            "solarize",
            "solarizeThreshold",
            "mirror",
            "rotation",
            "edgeGlowAmount",
            "edgeGlowR",
            "edgeGlowG",
            "edgeGlowB",
            "scanlineAmount",
            "scanlineSpacing",
            "ditherEnable",
            "ditherLevels",
            "chromaAberration",
            "zoomBlurAmount"};

        for (const char* paramId : paramIds)
        {
            if (auto* param = apvts.getParameter(paramId))
            {
                if (auto* rangedParam = dynamic_cast<juce::RangedAudioParameter*>(param))
                {
                    rangedParam->setValueNotifyingHost(rangedParam->getDefaultValue());
                }
            }
        }
        onModificationEnded(); // Create an undo state for the reset
    }

    // GPU checkbox
    bool useGpu = useGpuParam ? useGpuParam->get() : true;
#if !defined(WITH_CUDA_SUPPORT)
    ImGui::BeginDisabled();
    useGpu = false;
#endif
    if (ImGui::Checkbox("Use GPU", &useGpu))
    {
        if (useGpuParam)
            *useGpuParam = useGpu;
        onModificationEnded();
    }
#if !defined(WITH_CUDA_SUPPORT)
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("CUDA support was not compiled.\nCheck CMake and CUDA installation.");
    ImGui::EndDisabled();
#else
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggles all video processing between CPU and GPU (CUDA).");
#endif

    // Zoom buttons
    bool zoomModulated = isParamModulated("zoomLevel");
    int  level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    float      buttonWidth = (itemWidth / 2.0f) - 4.0f;
    const bool atMin = (level <= 0);
    const bool atMax = (level >= 2);

    if (zoomModulated)
        ImGui::BeginDisabled();
    if (atMin)
        ImGui::BeginDisabled();
    if (ImGui::Button("-", ImVec2(buttonWidth, 0)))
    {
        int newLevel = juce::jmax(0, level - 1);
        if (auto* p = apvts.getParameter("zoomLevel"))
            p->setValueNotifyingHost((float)newLevel / 2.0f);
        onModificationEnded();
    }
    if (atMin)
        ImGui::EndDisabled();

    ImGui::SameLine();

    if (atMax)
        ImGui::BeginDisabled();
    if (ImGui::Button("+", ImVec2(buttonWidth, 0)))
    {
        int newLevel = juce::jmin(2, level + 1);
        if (auto* p = apvts.getParameter("zoomLevel"))
            p->setValueNotifyingHost((float)newLevel / 2.0f);
        onModificationEnded();
    }
    if (atMax)
        ImGui::EndDisabled();
    // Scroll-edit for zoom level
    if (!zoomModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newLevel = juce::jlimit(0, 2, level + (wheel > 0.0f ? 1 : -1));
            if (newLevel != level)
            {
                if (auto* p = apvts.getParameter("zoomLevel"))
                    p->setValueNotifyingHost((float)newLevel / 2.0f);
                onModificationEnded();
            }
        }
    }
    if (zoomModulated)
        ImGui::EndDisabled();

    themeText(
        juce::String::formatted("Source ID In: %d", (int)currentSourceId.load()),
        theme.modules.videofx_section_header);
    themeText(
        juce::String::formatted("Output ID: %d", (int)getLogicalId()),
        theme.modules.videofx_section_header);

    // === COLOR ADJUSTMENTS (Collapsible) ===
    ImGui::SetNextItemOpen(true, ImGuiCond_Once); // Start expanded
    if (ImGui::TreeNodeEx(
            "Color Adjustments", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        // Color sliders
        bool        brightnessMod = isParamModulated("brightness");
        const float brightnessDefault = brightnessParam ? brightnessParam->load() : 0.0f;
        float       brightness =
            brightnessMod ? getLiveParamValue("brightness", brightnessDefault) : brightnessDefault;
        if (brightnessMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Brightness", &brightness, -100.0f, 100.0f))
        {
            if (!brightnessMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("brightness")))
                    *p = brightness;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !brightnessMod)
            onModificationEnded();
        if (!brightnessMod)
            adjustParamOnWheel(apvts.getParameter("brightness"), "brightness", brightness);
        if (brightnessMod)
            ImGui::EndDisabled();

        bool        contrastMod = isParamModulated("contrast");
        const float contrastDefault = contrastParam ? contrastParam->load() : 1.0f;
        float       contrast =
            contrastMod ? getLiveParamValue("contrast", contrastDefault) : contrastDefault;
        if (contrastMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Contrast", &contrast, 0.0f, 3.0f))
        {
            if (!contrastMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("contrast")))
                    *p = contrast;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !contrastMod)
            onModificationEnded();
        if (!contrastMod)
            adjustParamOnWheel(apvts.getParameter("contrast"), "contrast", contrast);
        if (contrastMod)
            ImGui::EndDisabled();

        bool        saturationMod = isParamModulated("saturation");
        const float saturationDefault = saturationParam ? saturationParam->load() : 1.0f;
        float       saturation =
            saturationMod ? getLiveParamValue("saturation", saturationDefault) : saturationDefault;
        if (saturationMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Saturation", &saturation, 0.0f, 3.0f))
        {
            if (!saturationMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("saturation")))
                    *p = saturation;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !saturationMod)
            onModificationEnded();
        if (!saturationMod)
            adjustParamOnWheel(apvts.getParameter("saturation"), "saturation", saturation);
        if (saturationMod)
            ImGui::EndDisabled();

        bool        hueShiftMod = isParamModulated("hueShift");
        const float hueDefault = hueShiftParam ? hueShiftParam->load() : 0.0f;
        float       hueShift = hueShiftMod ? getLiveParamValue("hueShift", hueDefault) : hueDefault;
        if (hueShiftMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Hue Shift", &hueShift, -180.0f, 180.0f))
        {
            if (!hueShiftMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("hueShift")))
                    *p = hueShift;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !hueShiftMod)
            onModificationEnded();
        if (!hueShiftMod)
            adjustParamOnWheel(apvts.getParameter("hueShift"), "hueShift", hueShift);
        if (hueShiftMod)
            ImGui::EndDisabled();

        bool        gainRedMod = isParamModulated("gainRed");
        const float gainRDefault = gainRedParam ? gainRedParam->load() : 1.0f;
        float       gainR = gainRedMod ? getLiveParamValue("gainRed", gainRDefault) : gainRDefault;
        if (gainRedMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Red Gain", &gainR, 0.0f, 2.0f))
        {
            if (!gainRedMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gainRed")))
                    *p = gainR;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !gainRedMod)
            onModificationEnded();
        if (!gainRedMod)
            adjustParamOnWheel(apvts.getParameter("gainRed"), "gainRed", gainR);
        if (gainRedMod)
            ImGui::EndDisabled();

        bool        gainGreenMod = isParamModulated("gainGreen");
        const float gainGDefault = gainGreenParam ? gainGreenParam->load() : 1.0f;
        float gainG = gainGreenMod ? getLiveParamValue("gainGreen", gainGDefault) : gainGDefault;
        if (gainGreenMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Green Gain", &gainG, 0.0f, 2.0f))
        {
            if (!gainGreenMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gainGreen")))
                    *p = gainG;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !gainGreenMod)
            onModificationEnded();
        if (!gainGreenMod)
            adjustParamOnWheel(apvts.getParameter("gainGreen"), "gainGreen", gainG);
        if (gainGreenMod)
            ImGui::EndDisabled();

        bool        gainBlueMod = isParamModulated("gainBlue");
        const float gainBDefault = gainBlueParam ? gainBlueParam->load() : 1.0f;
        float gainB = gainBlueMod ? getLiveParamValue("gainBlue", gainBDefault) : gainBDefault;
        if (gainBlueMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Blue Gain", &gainB, 0.0f, 2.0f))
        {
            if (!gainBlueMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gainBlue")))
                    *p = gainB;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !gainBlueMod)
            onModificationEnded();
        if (!gainBlueMod)
            adjustParamOnWheel(apvts.getParameter("gainBlue"), "gainBlue", gainB);
        if (gainBlueMod)
            ImGui::EndDisabled();

        bool sepiaMod = isParamModulated("sepia");
        if (sepiaMod)
            ImGui::BeginDisabled();
        bool sepia = sepiaParam ? sepiaParam->get() : false;
        if (ImGui::Checkbox("Sepia", &sepia))
        {
            if (!sepiaMod && sepiaParam)
                *sepiaParam = sepia;
            onModificationEnded();
        }
        if (sepiaMod)
            ImGui::EndDisabled();

        bool        temperatureMod = isParamModulated("temperature");
        const float temperatureDefault = temperatureParam ? temperatureParam->load() : 0.0f;
        float temperature = temperatureMod ? getLiveParamValue("temperature", temperatureDefault)
                                           : temperatureDefault;
        if (temperatureMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Temperature", &temperature, -1.0f, 1.0f))
        {
            if (!temperatureMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("temperature")))
                    *p = temperature;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !temperatureMod)
            onModificationEnded();
        if (!temperatureMod)
            adjustParamOnWheel(apvts.getParameter("temperature"), "temperature", temperature);
        if (temperatureMod)
            ImGui::EndDisabled();

        // Wet/Dry Mix
        {
            float wetDry = wetDryColorParam ? wetDryColorParam->get() : 1.0f;
            if (ImGui::SliderFloat("Color Wet/Dry", &wetDry, 0.0f, 1.0f))
                if (wetDryColorParam)
                    *wetDryColorParam = wetDry;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        ImGui::TreePop(); // End Color Adjustments
    }

    // === FILTERS & EFFECTS (Collapsible) ===
    ImGui::SetNextItemOpen(false, ImGuiCond_Once); // Start collapsed
    if (ImGui::TreeNodeEx(
            "Filters & Effects", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        // Filter sliders
        bool        sharpenMod = isParamModulated("sharpen");
        const float sharpenDefault = sharpenParam ? sharpenParam->load() : 0.0f;
        float sharpen = sharpenMod ? getLiveParamValue("sharpen", sharpenDefault) : sharpenDefault;
        if (sharpenMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Sharpen", &sharpen, 0.0f, 2.0f))
        {
            if (!sharpenMod)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("sharpen")))
                    *p = sharpen;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !sharpenMod)
            onModificationEnded();
        if (!sharpenMod)
            adjustParamOnWheel(apvts.getParameter("sharpen"), "sharpen", sharpen);
        if (sharpenMod)
            ImGui::EndDisabled();

        bool        blurMod = isParamModulated("blur");
        const float blurDefault = blurParam ? blurParam->load() : 0.0f;
        float       blur = blurMod ? getLiveParamValue("blur", blurDefault) : blurDefault;
        if (blurMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Blur", &blur, 0.0f, 20.0f))
        {
            if (!blurMod)
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("blur")))
                    *p = blur;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !blurMod)
            onModificationEnded();
        if (!blurMod)
            adjustParamOnWheel(apvts.getParameter("blur"), "blur", blur);
        if (blurMod)
            ImGui::EndDisabled();

        // Effect checkboxes
        bool grayscale = grayscaleParam ? grayscaleParam->get() : false;
        if (ImGui::Checkbox("Grayscale", &grayscale))
        {
            if (grayscaleParam)
                *grayscaleParam = grayscale;
            onModificationEnded();
        }

        bool invert = invertParam ? invertParam->get() : false;
        if (ImGui::Checkbox("Invert", &invert))
        {
            if (invertParam)
                *invertParam = invert;
            onModificationEnded();
        }

        bool flipH = flipHorizontalParam ? flipHorizontalParam->get() : false;
        if (ImGui::Checkbox("Flip H", &flipH))
        {
            if (flipHorizontalParam)
                *flipHorizontalParam = flipH;
            onModificationEnded();
        }

        bool flipV = flipVerticalParam ? flipVerticalParam->get() : false;
        if (ImGui::Checkbox("Flip V", &flipV))
        {
            if (flipVerticalParam)
                *flipVerticalParam = flipV;
            onModificationEnded();
        }

        // Wet/Dry Mix
        {
            float wetDry = wetDryFiltersParam ? wetDryFiltersParam->get() : 1.0f;
            if (ImGui::SliderFloat("Filters Wet/Dry", &wetDry, 0.0f, 1.0f))
                if (wetDryFiltersParam)
                    *wetDryFiltersParam = wetDry;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        ImGui::TreePop(); // End Filters & Effects
    }

    // === MORE FILTERS (Collapsible) ===
    ImGui::SetNextItemOpen(false, ImGuiCond_Once); // Start collapsed
    if (ImGui::TreeNodeEx(
            "More Filters", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        // Threshold Effect
        bool threshEnable = thresholdEnableParam ? thresholdEnableParam->get() : false;
        if (ImGui::Checkbox("Threshold", &threshEnable))
        {
            if (thresholdEnableParam)
                *thresholdEnableParam = threshEnable;
            onModificationEnded();
        }

        if (threshEnable)
        {
            ImGui::SameLine();
            bool  threshLevelMod = isParamModulated("thresholdLevel");
            float threshLevel =
                threshLevelMod ? getLiveParamValue(
                                     "thresholdLevel",
                                     thresholdLevelParam ? thresholdLevelParam->load() : 127.0f)
                               : (thresholdLevelParam ? thresholdLevelParam->load() : 127.0f);
            if (threshLevelMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("##level", &threshLevel, 0.0f, 255.0f, "%.0f"))
            {
                if (!threshLevelMod)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("thresholdLevel")))
                        *p = threshLevel;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !threshLevelMod)
                onModificationEnded();
            if (!threshLevelMod)
                adjustParamOnWheel(
                    apvts.getParameter("thresholdLevel"), "thresholdLevel", threshLevel);
            if (threshLevelMod)
                ImGui::EndDisabled();
        }

        // Posterize
        bool posterizeMod = isParamModulated("posterizeLevels");
        int  posterizeLevels = posterizeLevelsParam ? posterizeLevelsParam->get() : 16;
        if (posterizeMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderInt("Posterize", &posterizeLevels, 2, 16))
        {
            if (!posterizeMod && posterizeLevelsParam)
                *posterizeLevelsParam = posterizeLevels;
            onModificationEnded();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reduces the number of colors.\nLower values = stronger effect.");
        if (!posterizeMod)
            adjustParamOnWheel(
                apvts.getParameter("posterizeLevels"), "posterizeLevels", (float)posterizeLevels);
        if (posterizeMod)
            ImGui::EndDisabled();

        // Pixelate
        bool pixelateMod = isParamModulated("pixelateSize");
        int  pixelateSize = pixelateBlockSizeParam ? pixelateBlockSizeParam->get() : 1;
        if (pixelateMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderInt("Pixelate", &pixelateSize, 1, 128))
        {
            if (!pixelateMod && pixelateBlockSizeParam)
                *pixelateBlockSizeParam = pixelateSize;
            onModificationEnded();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Creates a mosaic effect.\nHigher values = larger blocks.");
        if (!pixelateMod)
            adjustParamOnWheel(
                apvts.getParameter("pixelateSize"), "pixelateSize", (float)pixelateSize);
        if (pixelateMod)
            ImGui::EndDisabled();

        // Edge Detection (Canny)
        bool cannyEnable = cannyEnableParam ? cannyEnableParam->get() : false;
        if (ImGui::Checkbox("Edge Detect", &cannyEnable))
        {
            if (cannyEnableParam)
                *cannyEnableParam = cannyEnable;
            onModificationEnded();
        }

        if (cannyEnable)
        {
            bool  cannyTh1Mod = isParamModulated("cannyThresh1");
            float cannyTh1 =
                cannyTh1Mod
                    ? getLiveParamValue(
                          "cannyThresh1", cannyThresh1Param ? cannyThresh1Param->load() : 50.0f)
                    : (cannyThresh1Param ? cannyThresh1Param->load() : 50.0f);
            if (cannyTh1Mod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Canny Thresh 1", &cannyTh1, 0.0f, 255.0f))
            {
                if (!cannyTh1Mod)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("cannyThresh1")))
                        *p = cannyTh1;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !cannyTh1Mod)
                onModificationEnded();
            if (!cannyTh1Mod)
                adjustParamOnWheel(apvts.getParameter("cannyThresh1"), "cannyThresh1", cannyTh1);
            if (cannyTh1Mod)
                ImGui::EndDisabled();

            bool  cannyTh2Mod = isParamModulated("cannyThresh2");
            float cannyTh2 =
                cannyTh2Mod
                    ? getLiveParamValue(
                          "cannyThresh2", cannyThresh2Param ? cannyThresh2Param->load() : 150.0f)
                    : (cannyThresh2Param ? cannyThresh2Param->load() : 150.0f);
            if (cannyTh2Mod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Canny Thresh 2", &cannyTh2, 0.0f, 255.0f))
            {
                if (!cannyTh2Mod)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("cannyThresh2")))
                        *p = cannyTh2;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !cannyTh2Mod)
                onModificationEnded();
            if (!cannyTh2Mod)
                adjustParamOnWheel(apvts.getParameter("cannyThresh2"), "cannyThresh2", cannyTh2);
            if (cannyTh2Mod)
                ImGui::EndDisabled();
        }

        // Wet/Dry Mix
        {
            float wetDry = wetDryMoreFiltersParam ? wetDryMoreFiltersParam->get() : 1.0f;
            if (ImGui::SliderFloat("More Filters Wet/Dry", &wetDry, 0.0f, 1.0f))
                if (wetDryMoreFiltersParam)
                    *wetDryMoreFiltersParam = wetDry;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        ImGui::TreePop(); // End More Filters
    }

    // === ADVANCED EFFECTS (Collapsible) ===
    ImGui::SetNextItemOpen(false, ImGuiCond_Once); // Start collapsed
    if (ImGui::TreeNodeEx(
            "Advanced Effects", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        // Vignette
        bool  vignetteAmountMod = isParamModulated("vignetteAmount");
        float vignetteAmount =
            vignetteAmountMod
                ? getLiveParamValue(
                      "vignetteAmount", vignetteAmountParam ? vignetteAmountParam->load() : 0.0f)
                : (vignetteAmountParam ? vignetteAmountParam->load() : 0.0f);
        if (vignetteAmountMod)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Vignette Amount", &vignetteAmount, 0.0f, 1.0f))
        {
            if (!vignetteAmountMod)
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                        apvts.getParameter("vignetteAmount")))
                    *p = vignetteAmount;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !vignetteAmountMod)
            onModificationEnded();
        if (!vignetteAmountMod)
            adjustParamOnWheel(
                apvts.getParameter("vignetteAmount"), "vignetteAmount", vignetteAmount);
        if (vignetteAmountMod)
            ImGui::EndDisabled();

        if (vignetteAmount > 0.0f)
        {
            bool  vignetteSizeMod = isParamModulated("vignetteSize");
            float vignetteSize =
                vignetteSizeMod
                    ? getLiveParamValue(
                          "vignetteSize", vignetteSizeParam ? vignetteSizeParam->load() : 0.5f)
                    : (vignetteSizeParam ? vignetteSizeParam->load() : 0.5f);
            if (vignetteSizeMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Vignette Size", &vignetteSize, 0.1f, 2.0f))
            {
                if (!vignetteSizeMod)
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("vignetteSize")))
                        *p = vignetteSize;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !vignetteSizeMod)
                onModificationEnded();
            if (!vignetteSizeMod)
                adjustParamOnWheel(
                    apvts.getParameter("vignetteSize"), "vignetteSize", vignetteSize);
            if (vignetteSizeMod)
                ImGui::EndDisabled();
        }

        // Kaleidoscope
        bool kaleidoscopeMod = isParamModulated("kaleidoscope");
        if (kaleidoscopeMod)
            ImGui::BeginDisabled();
        int kaleidoscopeMode = kaleidoscopeModeParam ? kaleidoscopeModeParam->getIndex() : 0;
        const char* kaleidoscopeModes[] = {"None", "4-Way", "8-Way"};
        if (ImGui::Combo("Kaleidoscope", &kaleidoscopeMode, kaleidoscopeModes, 3))
        {
            if (!kaleidoscopeMod && kaleidoscopeModeParam)
                kaleidoscopeModeParam->setValueNotifyingHost((float)kaleidoscopeMode / 2.0f);
            onModificationEnded();
        }
        // Scroll-edit for kaleidoscope combo
        if (!kaleidoscopeMod && ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int newMode = juce::jlimit(0, 2, kaleidoscopeMode + (wheel > 0.0f ? -1 : 1));
                if (newMode != kaleidoscopeMode && kaleidoscopeModeParam)
                {
                    kaleidoscopeModeParam->setValueNotifyingHost((float)newMode / 2.0f);
                    onModificationEnded();
                }
            }
        }
        if (kaleidoscopeMod)
            ImGui::EndDisabled();

        // Wet/Dry Mix
        {
            float wetDry = wetDryAdvancedParam ? wetDryAdvancedParam->get() : 1.0f;
            if (ImGui::SliderFloat("Advanced Wet/Dry", &wetDry, 0.0f, 1.0f))
                if (wetDryAdvancedParam)
                    *wetDryAdvancedParam = wetDry;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        ImGui::TreePop(); // End Advanced Effects
    }

    // === NEW EFFECTS (Collapsible) ===
    ImGui::SetNextItemOpen(false, ImGuiCond_Once); // Start collapsed
    if (ImGui::TreeNodeEx(
            "New Effects", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        // Gamma
        {
            float gamma = gammaParam ? gammaParam->load() : 1.0f;
            if (ImGui::SliderFloat("Gamma", &gamma, 0.1f, 3.0f))
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gamma")))
                    *p = gamma;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        // Levels
        {
            float black = levelsBlackParam ? levelsBlackParam->load() : 0.0f;
            float white = levelsWhiteParam ? levelsWhiteParam->load() : 255.0f;
            float gamma = levelsGammaParam ? levelsGammaParam->load() : 1.0f;
            if (ImGui::SliderFloat("Levels Black", &black, 0.0f, 255.0f))
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("levelsBlack")))
                    *p = black;
            if (ImGui::SliderFloat("Levels White", &white, 0.0f, 255.0f))
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("levelsWhite")))
                    *p = white;
            if (ImGui::SliderFloat("Levels Gamma", &gamma, 0.1f, 3.0f))
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("levelsGamma")))
                    *p = gamma;
        }

        // Noise
        {
            float noise = noiseAmountParam ? noiseAmountParam->load() : 0.0f;
            if (ImGui::SliderFloat("Noise/Grain", &noise, 0.0f, 1.0f))
                if (auto* p =
                        dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("noiseAmount")))
                    *p = noise;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        // Solarize
        {
            bool solarize = solarizeParam ? solarizeParam->get() : false;
            if (ImGui::Checkbox("Solarize", &solarize))
            {
                if (solarizeParam)
                    solarizeParam->setValueNotifyingHost(solarize ? 1.0f : 0.0f);
                onModificationEnded();
            }
            if (solarize)
            {
                float thresh = solarizeThresholdParam ? solarizeThresholdParam->load() : 128.0f;
                if (ImGui::SliderFloat("Solarize Threshold", &thresh, 0.0f, 255.0f))
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("solarizeThreshold")))
                        *p = thresh;
            }
        }

        // Emboss
        {
            bool emboss = embossParam ? embossParam->get() : false;
            if (ImGui::Checkbox("Emboss", &emboss))
            {
                if (embossParam)
                    embossParam->setValueNotifyingHost(emboss ? 1.0f : 0.0f);
                onModificationEnded();
            }
            if (emboss)
            {
                float strength = embossStrengthParam ? embossStrengthParam->load() : 1.0f;
                if (ImGui::SliderFloat("Emboss Strength", &strength, 0.5f, 3.0f))
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("embossStrength")))
                        *p = strength;
            }
        }

        // Mirror
        {
            int         mirrorMode = mirrorModeParam ? mirrorModeParam->getIndex() : 0;
            const char* mirrorModes[] = {
                "None", "Left-Right", "Right-Left", "Top-Bottom", "Bottom-Top"};
            if (ImGui::Combo("Mirror", &mirrorMode, mirrorModes, 5))
            {
                if (mirrorModeParam)
                    mirrorModeParam->setValueNotifyingHost((float)mirrorMode / 4.0f);
                onModificationEnded();
            }
        }

        // Rotation
        {
            int         rotMode = rotationModeParam ? rotationModeParam->getIndex() : 0;
            const char* rotModes[] = {"0", "90", "180", "270"};
            if (ImGui::Combo("Rotation", &rotMode, rotModes, 4))
            {
                if (rotationModeParam)
                    rotationModeParam->setValueNotifyingHost((float)rotMode / 3.0f);
                onModificationEnded();
            }
        }

        // Scanlines
        {
            float scanlines = scanlineAmountParam ? scanlineAmountParam->load() : 0.0f;
            if (ImGui::SliderFloat("Scanlines", &scanlines, 0.0f, 1.0f))
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                        apvts.getParameter("scanlineAmount")))
                    *p = scanlines;
            if (scanlines > 0.0f)
            {
                int spacing = scanlineSpacingParam ? scanlineSpacingParam->get() : 2;
                if (ImGui::SliderInt("Scanline Spacing", &spacing, 1, 10))
                    if (scanlineSpacingParam)
                        scanlineSpacingParam->setValueNotifyingHost((float)(spacing - 1) / 9.0f);
            }
        }

        // Dithering
        {
            bool dither = ditherEnableParam ? ditherEnableParam->get() : false;
            if (ImGui::Checkbox("Dithering", &dither))
            {
                if (ditherEnableParam)
                    ditherEnableParam->setValueNotifyingHost(dither ? 1.0f : 0.0f);
                onModificationEnded();
            }
            if (dither)
            {
                int levels = ditherLevelsParam ? ditherLevelsParam->get() : 4;
                if (ImGui::SliderInt("Dither Levels", &levels, 2, 8))
                    if (ditherLevelsParam)
                        ditherLevelsParam->setValueNotifyingHost((float)(levels - 2) / 6.0f);
            }
        }

        // Wet/Dry Mix
        {
            float wetDry = wetDryNewEffectsParam ? wetDryNewEffectsParam->get() : 1.0f;
            if (ImGui::SliderFloat("New Effects Wet/Dry", &wetDry, 0.0f, 1.0f))
                if (wetDryNewEffectsParam)
                    *wetDryNewEffectsParam = wetDry;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        ImGui::TreePop(); // End New Effects
    }

    // === DISTORTION (Collapsible) ===
    ImGui::SetNextItemOpen(false, ImGuiCond_Once); // Start collapsed
    if (ImGui::TreeNodeEx(
            "Distortion", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        // Chromatic Aberration
        {
            float chroma = chromaAberrationParam ? chromaAberrationParam->load() : 0.0f;
            if (ImGui::SliderFloat("Chromatic Aberration", &chroma, 0.0f, 20.0f))
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                        apvts.getParameter("chromaAberration")))
                    *p = chroma;
        }

        // Zoom Blur
        {
            float zoom = zoomBlurAmountParam ? zoomBlurAmountParam->load() : 0.0f;
            if (ImGui::SliderFloat("Zoom Blur", &zoom, 0.0f, 1.0f))
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                        apvts.getParameter("zoomBlurAmount")))
                    *p = zoom;
        }

        // Edge Glow
        {
            float glow = edgeGlowAmountParam ? edgeGlowAmountParam->load() : 0.0f;
            if (ImGui::SliderFloat("Edge Glow", &glow, 0.0f, 1.0f))
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                        apvts.getParameter("edgeGlowAmount")))
                    *p = glow;
            if (glow > 0.0f)
            {
                float r = edgeGlowRParam ? edgeGlowRParam->load() : 0.0f;
                float g = edgeGlowGParam ? edgeGlowGParam->load() : 1.0f;
                float b = edgeGlowBParam ? edgeGlowBParam->load() : 1.0f;
                if (ImGui::SliderFloat("Glow Red", &r, 0.0f, 1.0f))
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("edgeGlowR")))
                        *p = r;
                if (ImGui::SliderFloat("Glow Green", &g, 0.0f, 1.0f))
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("edgeGlowG")))
                        *p = g;
                if (ImGui::SliderFloat("Glow Blue", &b, 0.0f, 1.0f))
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("edgeGlowB")))
                        *p = b;
            }
        }

        // Wet/Dry Mix
        {
            float wetDry = wetDryDistortionParam ? wetDryDistortionParam->get() : 1.0f;
            if (ImGui::SliderFloat("Distortion Wet/Dry", &wetDry, 0.0f, 1.0f))
                if (wetDryDistortionParam)
                    *wetDryDistortionParam = wetDry;
            if (ImGui::IsItemDeactivatedAfterEdit())
                onModificationEnded();
        }

        ImGui::TreePop(); // End Distortion
    }

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();

    // === RESTORE WORKRECT VALUES ===
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}

void VideoFXModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Pins are handled via getDynamicInputPins/getDynamicOutputPins for proper Video type
    // coloring This method is called but dynamic pins take precedence
    helpers.drawAudioInputPin("Source In", 0);
    helpers.drawAudioOutputPin("Output", 0);
}
#endif

// ==============================================================================
// === GPU EFFECT HELPER FUNCTIONS (Phase 2A) ===================================
// ==============================================================================

#if defined(WITH_CUDA_SUPPORT)

void VideoFXModule::applyBrightnessContrast_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             brightness,
    float             contrast)
{
    // 1. Brightness / Contrast
    if (brightness == 0.0f && contrast == 1.0f)
        return;

    // Use the member 'gpuTemp' as the destination
    ioFrame.convertTo(gpuTemp, -1, contrast, brightness);

    // Copy the result back to ioFrame
    gpuTemp.copyTo(ioFrame);
}

void VideoFXModule::applyGrayscale_gpu(cv::cuda::GpuMat& ioFrame, bool grayscale)
{
    // 2. Grayscale
    if (!grayscale)
        return;

    // Convert to single-channel gray
    cv::cuda::cvtColor(ioFrame, gpuTemp, cv::COLOR_BGR2GRAY);

    // Convert back to 3-channel BGR for compatibility with subsequent filters
    cv::cuda::cvtColor(gpuTemp, ioFrame, cv::COLOR_GRAY2BGR);
}

void VideoFXModule::applyInvert_gpu(cv::cuda::GpuMat& ioFrame, bool invert)
{
    // 3. Invert
    if (!invert)
        return;

    // Invert all bits
    cv::cuda::bitwise_not(ioFrame, gpuTemp);
    gpuTemp.copyTo(ioFrame);
}

void VideoFXModule::applyFlip_gpu(cv::cuda::GpuMat& ioFrame, bool flipH, bool flipV)
{
    // 4. Flip
    if (!flipH && !flipV)
        return;

    // -1 = both, 1 = H, 0 = V
    int flipCode = flipH && flipV ? -1 : (flipH ? 1 : 0);
    cv::cuda::flip(ioFrame, gpuTemp, flipCode);
    gpuTemp.copyTo(ioFrame);
}

void VideoFXModule::applyBlur_gpu(cv::cuda::GpuMat& ioFrame, float blur)
{
    // 5. Blur (with bug fix)
    if (blur <= 0.1f)
        return;

    // Use the same fixed logic as the CPU path
    int ksize = static_cast<int>(std::round(blur));
    if (ksize % 2 == 0)
        ksize++;
    if (ksize < 3)
        ksize = 3;

    // Create a filter object
    // This is relatively lightweight and can be created on the fly
    auto gaussian = cv::cuda::createGaussianFilter(ioFrame.type(), -1, cv::Size(ksize, ksize), 0);

    // Apply the filter
    gaussian->apply(ioFrame, gpuTemp);
    gpuTemp.copyTo(ioFrame);
}

// ==============================================================================
// === GPU EFFECT HELPER FUNCTIONS (Phase 2B) ===================================
// ==============================================================================

void VideoFXModule::applyTemperature_gpu(cv::cuda::GpuMat& ioFrame, float temperature)
{
    // 6. Temperature
    if (temperature == 0.0f)
        return;

    // We must split the BGR channels to apply different gains
    cv::cuda::split(ioFrame, gpuChannels);
    // gpuChannels[0] = Blue
    // gpuChannels[1] = Green
    // gpuChannels[2] = Red

    float factor = temperature;

    // Corrected logic from the CPU version
    if (factor < 0.0f)
    { // Cool (add blue, remove red)
        cv::cuda::multiply(gpuChannels[0], (1.0f - factor), gpuChannels[0]); // Add Blue
        cv::cuda::multiply(gpuChannels[2], (1.0f + factor), gpuChannels[2]); // Remove Red
    }
    else
    { // Warm (add red, remove blue)
        cv::cuda::multiply(gpuChannels[0], (1.0f - factor), gpuChannels[0]); // Remove Blue
        cv::cuda::multiply(gpuChannels[2], (1.0f + factor), gpuChannels[2]); // Add Red
    }

    // Merge the channels back together
    cv::cuda::merge(gpuChannels, ioFrame);
}

void VideoFXModule::applySepia_gpu(cv::cuda::GpuMat& ioFrame, bool sepia)
{
    // 7. Sepia
    if (!sepia)
        return;

    // Sepia matrix coefficients (BGR order):
    // B' = 0.131*B + 0.534*G + 0.272*R
    // G' = 0.168*B + 0.686*G + 0.349*R
    // R' = 0.189*B + 0.769*G + 0.393*R

    // Split into BGR channels
    cv::cuda::split(ioFrame, gpuChannels);

    // Convert channels to float for weighted math
    cv::cuda::GpuMat bF, gF, rF;
    gpuChannels[0].convertTo(bF, CV_32F);
    gpuChannels[1].convertTo(gF, CV_32F);
    gpuChannels[2].convertTo(rF, CV_32F);

    // Calculate new B channel: 0.131*B + 0.534*G + 0.272*R
    cv::cuda::GpuMat newB, newG, newR, tmpAdd;
    cv::cuda::multiply(bF, 0.131, gpuTempF1);
    cv::cuda::multiply(gF, 0.534, gpuTempF2);
    cv::cuda::add(gpuTempF1, gpuTempF2, newB);
    cv::cuda::multiply(rF, 0.272, gpuTempF1);
    cv::cuda::add(newB, gpuTempF1, newB);

    // Calculate new G channel: 0.168*B + 0.686*G + 0.349*R
    cv::cuda::multiply(bF, 0.168, gpuTempF1);
    cv::cuda::multiply(gF, 0.686, gpuTempF2);
    cv::cuda::add(gpuTempF1, gpuTempF2, newG);
    cv::cuda::multiply(rF, 0.349, gpuTempF1);
    cv::cuda::add(newG, gpuTempF1, newG);

    // Calculate new R channel: 0.189*B + 0.769*G + 0.393*R
    cv::cuda::multiply(bF, 0.189, gpuTempF1);
    cv::cuda::multiply(gF, 0.769, gpuTempF2);
    cv::cuda::add(gpuTempF1, gpuTempF2, newR);
    cv::cuda::multiply(rF, 0.393, gpuTempF1);
    cv::cuda::add(newR, gpuTempF1, newR);

    // Convert back to 8U and clamp
    newB.convertTo(gpuChannels[0], CV_8U);
    newG.convertTo(gpuChannels[1], CV_8U);
    newR.convertTo(gpuChannels[2], CV_8U);

    // Merge channels
    cv::cuda::merge(gpuChannels, ioFrame);
}

void VideoFXModule::applyRgbGain_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             gainR,
    float             gainG,
    float             gainB)
{
    // 8. RGB Gain
    if (gainR == 1.0f && gainG == 1.0f && gainB == 1.0f)
        return;

    // Split BGR channels
    cv::cuda::split(ioFrame, gpuChannels);

    // Apply gain (multiply) to each channel
    // gpuChannels[0] = Blue
    if (gainB != 1.0f)
        cv::cuda::multiply(gpuChannels[0], gainB, gpuChannels[0]);
    // gpuChannels[1] = Green
    if (gainG != 1.0f)
        cv::cuda::multiply(gpuChannels[1], gainG, gpuChannels[1]);
    // gpuChannels[2] = Red
    if (gainR != 1.0f)
        cv::cuda::multiply(gpuChannels[2], gainR, gpuChannels[2]);

    // Merge channels back
    cv::cuda::merge(gpuChannels, ioFrame);
}

void VideoFXModule::applyCanny_gpu(cv::cuda::GpuMat& ioFrame, float thresh1, float thresh2)
{
    static cv::Ptr<cv::cuda::CannyEdgeDetector> canny;
    static float                                lastT1 = -1, lastT2 = -1;

    if (!canny || thresh1 != lastT1 || thresh2 != lastT2)
    {
        canny = cv::cuda::createCannyEdgeDetector(thresh1, thresh2);
        lastT1 = thresh1;
        lastT2 = thresh2;
    }

    if (ioFrame.channels() != 1)
        cv::cuda::cvtColor(ioFrame, gpuGray, cv::COLOR_BGR2GRAY);
    else
        ioFrame.copyTo(gpuGray);

    cv::cuda::GpuMat edges;
    canny->detect(gpuGray, edges);
    cv::cuda::cvtColor(edges, ioFrame, cv::COLOR_GRAY2BGR);
}

void VideoFXModule::applyThreshold_gpu(cv::cuda::GpuMat& ioFrame, float level)
{
    // 10. Threshold using LUT to match CPU THRESH_BINARY semantics exactly
    // Build once per level and reuse (fast and stable)

    // 1) Grayscale 8U
    cv::cuda::cvtColor(ioFrame, gpuGray, cv::COLOR_BGR2GRAY);

    // 2) Build/refresh LUT for this level
    static int              lastLevel = -1;
    static cv::cuda::GpuMat lutGpu;
    int                     t = juce::jlimit(0, 255, static_cast<int>(std::round(level)));
    if (t != lastLevel || lutGpu.empty())
    {
        cv::Mat lutCpu(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
            lutCpu.at<uchar>(i) = (i > t) ? 255 : 0; // THRESH_BINARY: src > thresh ? maxVal : 0
        lutGpu.upload(lutCpu);
        lastLevel = t;
    }

    // 3) Apply LUT on GPU
    {
        cv::Ptr<cv::cuda::LookUpTable> lut = cv::cuda::createLookUpTable(lutGpu);
        lut->transform(gpuGray, gpuTemp);
    }

    // 4) Back to 3-channel BGR
    cv::cuda::cvtColor(gpuTemp, ioFrame, cv::COLOR_GRAY2BGR);
}

// ==============================================================================
// === GPU EFFECT HELPER FUNCTIONS (Phase 2C - Hard) ============================
// ==============================================================================

void VideoFXModule::applySaturationHue_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             saturation,
    float             hueShift)
{
    // 11. Saturation / Hue
    if (saturation == 1.0f && hueShift == 0.0f)
        return;

    // Convert to HSV
    cv::cuda::cvtColor(ioFrame, gpuTemp, cv::COLOR_BGR2HSV);

    // Split into H, S, V channels
    cv::cuda::split(gpuTemp, gpuChannels);
    // gpuChannels[0] = H, gpuChannels[1] = S, gpuChannels[2] = V

    // --- Hue Shift ---
    if (hueShift != 0.0f)
    {
        // Convert Hue to 32-bit float
        gpuChannels[0].convertTo(gpuTempF1, CV_32F);

        // Add shift (e.g., 30 degrees = 15 in OpenCV H space)
        cv::cuda::add(gpuTempF1, hueShift / 2.0f, gpuTempF1);

        // --- GPU-based Hue Wrapping (0-180) ---
        // 1. mask = (H < 0)
        cv::cuda::compare(gpuTempF1, 0.0f, gpuMask, cv::CMP_LT);
        // 2. H[mask] = H[mask] + 180
        cv::cuda::add(gpuTempF1, 180.0f, gpuTempF1, gpuMask);

        // 3. mask = (H >= 180)
        cv::cuda::compare(gpuTempF1, 180.0f, gpuMask, cv::CMP_GE);
        // 4. H[mask] = H[mask] - 180
        cv::cuda::subtract(gpuTempF1, 180.0f, gpuTempF1, gpuMask);

        // Convert back to 8-bit
        gpuTempF1.convertTo(gpuChannels[0], CV_8U);
    }

    // --- Saturation ---
    if (saturation != 1.0f)
    {
        // Convert Saturation to 32-bit float
        gpuChannels[1].convertTo(gpuTempF2, CV_32F);
        // Multiply
        cv::cuda::multiply(gpuTempF2, saturation, gpuTempF2);
        // Convert back to 8-bit
        gpuTempF2.convertTo(gpuChannels[1], CV_8U);
    }

    // Merge H, S, V back together
    cv::cuda::merge(gpuChannels, gpuTemp);

    // Convert back to BGR
    cv::cuda::cvtColor(gpuTemp, ioFrame, cv::COLOR_HSV2BGR);
}

void VideoFXModule::applyPosterize_gpu(cv::cuda::GpuMat& ioFrame, int levels)
{
    // 12. Posterize via per-channel LUT (matches CPU exactly, no color bias)
    if (levels >= 16)
        return;
    if (levels < 2)
        levels = 2;

    // Cache LUT by levels
    static int              lastLevels = -1;
    static cv::cuda::GpuMat lutGpu; // 1x256 CV_8UC1

    if (levels != lastLevels || lutGpu.empty())
    {
        const int divider = 255 / (levels - 1);
        const int halfDiv = divider / 2;

        cv::Mat lutCpu(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
        {
            int v = (i + halfDiv) / divider; // integer division floors
            v = v * divider;
            if (v > 255)
                v = 255;
            lutCpu.at<uchar>(i) = static_cast<uchar>(v);
        }
        lutGpu.upload(lutCpu);
        lastLevels = levels;
    }

    // Apply LUT to each channel (GPU handles per-channel LUT)
    cv::Ptr<cv::cuda::LookUpTable> lut = cv::cuda::createLookUpTable(lutGpu);
    lut->transform(ioFrame, ioFrame);
}

void VideoFXModule::applyVignette_gpu(cv::cuda::GpuMat& ioFrame, float amount, float size)
{
    // 13. Vignette
    if (amount <= 0.0f)
        return;

    int w = ioFrame.cols;
    int h = ioFrame.rows;

    // --- Caching ---
    // Regenerate the mask ONLY if settings or frame size change
    if (w != lastVignetteW || h != lastVignetteH || amount != lastVignetteAmount ||
        size != lastVignetteSize)
    {
        // Create mask on CPU
        cpuVignetteMask.create(h, w, CV_32FC1);
        int   centerX = w / 2;
        int   centerY = h / 2;
        float maxDist = std::sqrt((float)centerX * centerX + (float)centerY * centerY) * size;
        if (maxDist <= 0.0f)
            maxDist = 1.0f;

        for (int y = 0; y < h; y++)
        {
            float* p = cpuVignetteMask.ptr<float>(y);
            for (int x = 0; x < w; x++)
            {
                float dist = std::sqrt(std::pow(x - centerX, 2) + std::pow(y - centerY, 2));
                float v = 1.0f - (dist / maxDist) * amount;
                p[x] = juce::jlimit(0.0f, 1.0f, v);
            }
        }

        // Upload to GPU cache
        gpuVignetteMask.upload(cpuVignetteMask);

        // Store last used settings
        lastVignetteW = w;
        lastVignetteH = h;
        lastVignetteAmount = amount;
        lastVignetteSize = size;
    }

    // Apply the mask to each channel individually (channel broadcast fix)
    ioFrame.convertTo(gpuTemp, CV_32F);
    cv::cuda::split(gpuTemp, gpuChannels);
    for (int i = 0; i < 3; i++)
        cv::cuda::multiply(gpuChannels[i], gpuVignetteMask, gpuChannels[i]);
    cv::cuda::merge(gpuChannels, gpuTemp);
    gpuTemp.convertTo(ioFrame, CV_8U);
}

void VideoFXModule::applyPixelate_gpu(cv::cuda::GpuMat& ioFrame, int pixelSize)
{
    // 14. Pixelate
    if (pixelSize <= 1)
        return;

    int w = ioFrame.cols;
    int h = ioFrame.rows;

    // 1. Resize down (Nearest Neighbor)
    cv::cuda::resize(
        ioFrame, gpuTemp, cv::Size(w / pixelSize, h / pixelSize), 0, 0, cv::INTER_NEAREST);

    // 2. Resize back up (Nearest Neighbor)
    cv::cuda::resize(gpuTemp, ioFrame, cv::Size(w, h), 0, 0, cv::INTER_NEAREST);
}

void VideoFXModule::applySharpen_gpu(cv::cuda::GpuMat& ioFrame, float sharpen)
{
    // 15. Sharpen
    if (sharpen <= 0.0f)
        return;

    // 1. Convert to 16-bit signed
    ioFrame.convertTo(gpuTemp16S, CV_16SC3);

    // 2. Create blurred version
    auto blurFilter = cv::cuda::createGaussianFilter(gpuTemp16S.type(), -1, cv::Size(0, 0), 3);
    blurFilter->apply(gpuTemp16S, gpuBlurred16S);

    // 3. Add weighted: (original * (1+s)) + (blurred * -s)
    cv::cuda::addWeighted(gpuTemp16S, 1.0 + sharpen, gpuBlurred16S, -sharpen, 0, gpuTemp16S);

    // 4. Convert back to 8-bit
    gpuTemp16S.convertTo(ioFrame, CV_8UC3);
}

void VideoFXModule::applyKaleidoscope_gpu(cv::cuda::GpuMat& ioFrame, int mode)
{
    // 16. Kaleidoscope
    if (mode == 0)
        return; // "None"

    int w = ioFrame.cols;
    int h = ioFrame.rows;
    int halfW = w / 2;
    int halfH = h / 2;
    if (halfW < 1 || halfH < 1)
        return;

    // Get a read-only view of the top-left quadrant
    const cv::cuda::GpuMat quadrantView(ioFrame, cv::Rect(0, 0, halfW, halfH));

    // Make a copy of it into our member buffer
    quadrantView.copyTo(gpuQuadrant);

    if (mode == 1) // 4-Way
    {
        // 1. Flip the quadrant 3 ways
        cv::cuda::flip(gpuQuadrant, gpuFlipH, 1);   // Horizontal
        cv::cuda::flip(gpuQuadrant, gpuFlipV, 0);   // Vertical
        cv::cuda::flip(gpuQuadrant, gpuFlipHV, -1); // Both

        // 2. Copy all 4 back into the main frame
        // (Top-Left)
        gpuQuadrant.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(0, 0, halfW, halfH)));
        // (Top-Right)
        gpuFlipH.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(halfW, 0, halfW, halfH)));
        // (Bottom-Left)
        gpuFlipV.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(0, halfH, halfW, halfH)));
        // (Bottom-Right)
        gpuFlipHV.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(halfW, halfH, halfW, halfH)));
    }
    else if (mode == 2) // 8-Way
    {
        // The 8-way logic is extremely complex (pixel-level masking)
        // and not feasible without a custom CUDA kernel.
        // For this phase, we will just apply the 4-way as a fallback.
        // TODO: Implement 8-way with a custom kernel or fallback to CPU.
        applyKaleidoscope_gpu(ioFrame, 1); // Just do 4-way for now
    }
}

// ==============================================================================
// === NEW GPU EFFECT FUNCTIONS (Native GPU) ====================================
// ==============================================================================

void VideoFXModule::applyGamma_gpu(cv::cuda::GpuMat& ioFrame, float gamma)
{
    if (gamma == 1.0f)
        return;
    // Use LUT for GPU gamma correction
    static float            lastGamma = -1.0f;
    static cv::cuda::GpuMat gammaLutGpu;

    if (gamma != lastGamma || gammaLutGpu.empty())
    {
        cv::Mat lut(1, 256, CV_8UC1);
        for (int i = 0; i < 256; i++)
            lut.at<uchar>(i) = cv::saturate_cast<uchar>(std::pow(i / 255.0f, gamma) * 255.0f);
        gammaLutGpu.upload(lut);
        lastGamma = gamma;
    }
    cv::Ptr<cv::cuda::LookUpTable> lutObj = cv::cuda::createLookUpTable(gammaLutGpu);
    lutObj->transform(ioFrame, ioFrame);
}

void VideoFXModule::applyLevels_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             black,
    float             white,
    float             gamma)
{
    if (black == 0.0f && white == 255.0f && gamma == 1.0f)
        return;
    // Use LUT for GPU levels adjustment
    static float            lastBlack = -1.0f, lastWhite = -1.0f, lastGamma = -1.0f;
    static cv::cuda::GpuMat levelsLutGpu;

    if (black != lastBlack || white != lastWhite || gamma != lastGamma || levelsLutGpu.empty())
    {
        float range = white - black;
        if (range <= 0)
            range = 1.0f;
        cv::Mat lut(1, 256, CV_8UC1);
        for (int i = 0; i < 256; i++)
        {
            float normalized = (i - black) / range;
            normalized = std::clamp(normalized, 0.0f, 1.0f);
            if (gamma != 1.0f)
                normalized = std::pow(normalized, gamma);
            lut.at<uchar>(i) = cv::saturate_cast<uchar>(normalized * 255.0f);
        }
        levelsLutGpu.upload(lut);
        lastBlack = black;
        lastWhite = white;
        lastGamma = gamma;
    }
    cv::Ptr<cv::cuda::LookUpTable> lutObj = cv::cuda::createLookUpTable(levelsLutGpu);
    lutObj->transform(ioFrame, ioFrame);
}

void VideoFXModule::applyChannelMixer_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             rr,
    float             rg,
    float             rb,
    float             gr,
    float             gg,
    float             gb,
    float             br,
    float             bg,
    float             bb)
{
    // Pure GPU implementation using channel split and per-channel math
    if (rr == 1.0f && rg == 0.0f && rb == 0.0f && gr == 0.0f && gg == 1.0f && gb == 0.0f &&
        br == 0.0f && bg == 0.0f && bb == 1.0f)
        return;

    // Split into B, G, R channels
    std::vector<cv::cuda::GpuMat> channels(3);
    cv::cuda::split(ioFrame, channels);

    // Convert to float for accurate math
    cv::cuda::GpuMat bFloat, gFloat, rFloat;
    channels[0].convertTo(bFloat, CV_32F);
    channels[1].convertTo(gFloat, CV_32F);
    channels[2].convertTo(rFloat, CV_32F);

    // Apply channel mixing matrix:
    // newR = rr*R + rg*G + rb*B
    // newG = gr*R + gg*G + gb*B
    // newB = br*R + bg*G + bb*B
    cv::cuda::GpuMat newR, newG, newB, temp1, temp2;

    // New Red channel
    cv::cuda::multiply(rFloat, cv::Scalar(rr), newR);
    cv::cuda::multiply(gFloat, cv::Scalar(rg), temp1);
    cv::cuda::add(newR, temp1, newR);
    cv::cuda::multiply(bFloat, cv::Scalar(rb), temp1);
    cv::cuda::add(newR, temp1, newR);

    // New Green channel
    cv::cuda::multiply(rFloat, cv::Scalar(gr), newG);
    cv::cuda::multiply(gFloat, cv::Scalar(gg), temp1);
    cv::cuda::add(newG, temp1, newG);
    cv::cuda::multiply(bFloat, cv::Scalar(gb), temp1);
    cv::cuda::add(newG, temp1, newG);

    // New Blue channel
    cv::cuda::multiply(rFloat, cv::Scalar(br), newB);
    cv::cuda::multiply(gFloat, cv::Scalar(bg), temp1);
    cv::cuda::add(newB, temp1, newB);
    cv::cuda::multiply(bFloat, cv::Scalar(bb), temp1);
    cv::cuda::add(newB, temp1, newB);

    // Convert back to 8-bit and merge
    newB.convertTo(channels[0], CV_8U);
    newG.convertTo(channels[1], CV_8U);
    newR.convertTo(channels[2], CV_8U);

    cv::cuda::merge(channels, ioFrame);
}

void VideoFXModule::applyBlendColor_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             amount,
    float             r,
    float             g,
    float             b,
    int               blendMode)
{
    // Pure GPU implementation
    if (amount <= 0.0f)
        return;

    // Create solid color overlay on GPU (cached)
    static cv::cuda::GpuMat overlayGpu;
    static int              lastW = 0, lastH = 0;
    static float            lastR = -1, lastG = -1, lastB = -1;

    if (ioFrame.cols != lastW || ioFrame.rows != lastH || r != lastR || g != lastG || b != lastB)
    {
        cv::Mat overlayCpu(
            ioFrame.rows, ioFrame.cols, ioFrame.type(), cv::Scalar(b * 255, g * 255, r * 255));
        overlayGpu.upload(overlayCpu);
        lastW = ioFrame.cols;
        lastH = ioFrame.rows;
        lastR = r;
        lastG = g;
        lastB = b;
    }

    switch (blendMode)
    {
    case 0: // Normal blend
        cv::cuda::addWeighted(ioFrame, 1.0 - amount, overlayGpu, amount, 0, ioFrame);
        break;
    case 1: // Multiply blend
    {
        cv::cuda::GpuMat multiplied;
        cv::cuda::multiply(ioFrame, overlayGpu, multiplied, 1.0 / 255.0);
        cv::cuda::addWeighted(ioFrame, 1.0 - amount, multiplied, amount, 0, ioFrame);
        break;
    }
    case 4: // Add blend
    {
        cv::cuda::GpuMat scaled;
        cv::cuda::multiply(overlayGpu, cv::Scalar(amount, amount, amount), scaled);
        cv::cuda::add(ioFrame, scaled, ioFrame);
        break;
    }
    default:
        cv::cuda::addWeighted(ioFrame, 1.0 - amount, overlayGpu, amount, 0, ioFrame);
        break;
    }
}

void VideoFXModule::applySolarize_gpu(cv::cuda::GpuMat& ioFrame, float threshold)
{
    // Pure GPU implementation using cached LUT
    static cv::cuda::GpuMat               solarizeLutGpu;
    static cv::Ptr<cv::cuda::LookUpTable> solarizeLutObj;
    static float                          lastThreshold = -1.0f;

    if (threshold != lastThreshold || solarizeLutGpu.empty())
    {
        cv::Mat lutCpu(1, 256, CV_8UC1);
        for (int i = 0; i < 256; i++)
            lutCpu.at<uchar>(i) = (i > threshold) ? (255 - i) : static_cast<uchar>(i);
        solarizeLutGpu.upload(lutCpu);
        solarizeLutObj = cv::cuda::createLookUpTable(solarizeLutGpu);
        lastThreshold = threshold;
    }
    solarizeLutObj->transform(ioFrame, ioFrame);
}

void VideoFXModule::applyEmboss_gpu(cv::cuda::GpuMat& ioFrame, float strength)
{
    // Pure GPU implementation using cached convolution filter
    static cv::Ptr<cv::cuda::Filter> embossFilter;
    static float                     lastStrength = -1.0f;

    if (strength != lastStrength || !embossFilter)
    {
        cv::Mat kernel =
            (cv::Mat_<float>(3, 3) << -2 * strength,
             -strength,
             0,
             -strength,
             1,
             strength,
             0,
             strength,
             2 * strength);
        embossFilter = cv::cuda::createLinearFilter(ioFrame.type(), ioFrame.type(), kernel);
        lastStrength = strength;
    }
    embossFilter->apply(ioFrame, ioFrame);
    cv::cuda::add(ioFrame, cv::Scalar(128, 128, 128), ioFrame);
}

void VideoFXModule::applyNoise_gpu(cv::cuda::GpuMat& ioFrame, float amount)
{
    // GPU implementation with cached noise texture
    // cv::cuda::randu doesn't exist in all OpenCV versions, so we generate noise on CPU
    // and cache/upload it to GPU. Noise is regenerated periodically for temporal variation.
    if (amount <= 0.0f)
        return;

    static cv::cuda::GpuMat noiseGpu;
    static int              lastW = 0, lastH = 0;
    static float            lastAmount = -1.0f;
    static int              frameCounter = 0;

    // Regenerate noise if size/amount changed or every 2 frames for temporal variation
    if (ioFrame.cols != lastW || ioFrame.rows != lastH || amount != lastAmount ||
        (frameCounter++ % 2 == 0))
    {
        cv::Mat noiseCpu(ioFrame.rows, ioFrame.cols, ioFrame.type());
        cv::randn(noiseCpu, 0, amount * 50);
        noiseGpu.upload(noiseCpu);
        lastW = ioFrame.cols;
        lastH = ioFrame.rows;
        lastAmount = amount;
    }
    cv::cuda::add(ioFrame, noiseGpu, ioFrame, cv::noArray(), ioFrame.type());
}

void VideoFXModule::applyMirror_gpu(cv::cuda::GpuMat& ioFrame, int mode)
{
    if (mode == 0)
        return;
    int w = ioFrame.cols, h = ioFrame.rows, halfW = w / 2, halfH = h / 2;
    switch (mode)
    {
    case 1:
    {
        cv::cuda::GpuMat half(ioFrame, cv::Rect(0, 0, halfW, h));
        cv::cuda::flip(half, gpuTemp, 1);
        gpuTemp.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(halfW, 0, halfW, h)));
        break;
    }
    case 2:
    {
        cv::cuda::GpuMat half(ioFrame, cv::Rect(halfW, 0, halfW, h));
        cv::cuda::flip(half, gpuTemp, 1);
        gpuTemp.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(0, 0, halfW, h)));
        break;
    }
    case 3:
    {
        cv::cuda::GpuMat half(ioFrame, cv::Rect(0, 0, w, halfH));
        cv::cuda::flip(half, gpuTemp, 0);
        gpuTemp.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(0, halfH, w, halfH)));
        break;
    }
    case 4:
    {
        cv::cuda::GpuMat half(ioFrame, cv::Rect(0, halfH, w, halfH));
        cv::cuda::flip(half, gpuTemp, 0);
        gpuTemp.copyTo(cv::cuda::GpuMat(ioFrame, cv::Rect(0, 0, w, halfH)));
        break;
    }
    }
}

void VideoFXModule::applyRotation_gpu(cv::cuda::GpuMat& ioFrame, int mode)
{
    if (mode == 0)
        return;
    // OpenCV CUDA rotate function
    switch (mode)
    {
    case 1:
        cv::cuda::rotate(ioFrame, gpuTemp, ioFrame.size(), 90, ioFrame.cols - 1, 0);
        gpuTemp.copyTo(ioFrame);
        break;
    case 2:
        cv::cuda::rotate(ioFrame, gpuTemp, ioFrame.size(), 180, ioFrame.cols - 1, ioFrame.rows - 1);
        gpuTemp.copyTo(ioFrame);
        break;
    case 3:
        cv::cuda::rotate(ioFrame, gpuTemp, ioFrame.size(), 270, 0, ioFrame.rows - 1);
        gpuTemp.copyTo(ioFrame);
        break;
    }
}

void VideoFXModule::applyScanlines_gpu(cv::cuda::GpuMat& ioFrame, float amount, int spacing)
{
    if (amount <= 0.0f)
        return;
    // Create scanline mask in GPU memory (cached)
    static cv::cuda::GpuMat scanMask;
    static int              lastH = 0, lastSpacing = 0;
    static float            lastAmount = -1.0f;

    if (ioFrame.rows != lastH || spacing != lastSpacing || amount != lastAmount)
    {
        cv::Mat cpuMask(ioFrame.rows, ioFrame.cols, CV_8UC3, cv::Scalar(255, 255, 255));
        uchar   darken = static_cast<uchar>((1.0f - amount) * 255);
        for (int y = 0; y < cpuMask.rows; y += spacing)
            cpuMask.row(y).setTo(cv::Scalar(darken, darken, darken));
        scanMask.upload(cpuMask);
        lastH = ioFrame.rows;
        lastSpacing = spacing;
        lastAmount = amount;
    }
    cv::cuda::multiply(ioFrame, scanMask, ioFrame, 1.0 / 255.0);
}

void VideoFXModule::applyDithering_gpu(cv::cuda::GpuMat& ioFrame, int levels)
{
    if (levels >= 256 || levels < 2)
        return;
    // Use LUT similar to posterize - fully GPU based
    static int              lastLevels = -1;
    static cv::cuda::GpuMat lutGpu;

    if (levels != lastLevels || lutGpu.empty())
    {
        float   step = 255.0f / (levels - 1);
        cv::Mat lutCpu(1, 256, CV_8UC1);
        for (int i = 0; i < 256; i++)
            lutCpu.at<uchar>(i) = cv::saturate_cast<uchar>(std::round(i / step) * step);
        lutGpu.upload(lutCpu);
        lastLevels = levels;
    }
    cv::Ptr<cv::cuda::LookUpTable> lut = cv::cuda::createLookUpTable(lutGpu);
    lut->transform(ioFrame, ioFrame);
}

void VideoFXModule::applyChromaAberration_gpu(cv::cuda::GpuMat& ioFrame, float amount)
{
    if (amount < 1.0f)
        return; // Skip if less than 1 pixel shift

    int shift = static_cast<int>(amount);

    // Cache transformation matrices based on shift value
    static int     lastShift = 0;
    static cv::Mat M_r, M_b;
    if (shift != lastShift)
    {
        M_r = (cv::Mat_<float>(2, 3) << 1, 0, shift, 0, 1, 0);
        M_b = (cv::Mat_<float>(2, 3) << 1, 0, -shift, 0, 1, 0);
        lastShift = shift;
    }

    // Use member buffer for channels
    cv::cuda::split(ioFrame, gpuChannels);

    cv::cuda::warpAffine(gpuChannels[2], gpuChannels[2], M_r, ioFrame.size());
    cv::cuda::warpAffine(gpuChannels[0], gpuChannels[0], M_b, ioFrame.size());
    cv::cuda::merge(gpuChannels, ioFrame);
}

void VideoFXModule::applyZoomBlur_gpu(cv::cuda::GpuMat& ioFrame, float amount)
{
    if (amount < 0.1f)
        return; // Skip for very low values

    // Limit to 2 steps max for better performance
    int   steps = std::min(static_cast<int>(amount * 3) + 1, 2);
    float cx = ioFrame.cols / 2.0f, cy = ioFrame.rows / 2.0f;

    // Pre-calculate scale for single step if only 1 step
    float   scale = 1.0f + (amount * 0.03f);
    cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(cx, cy), 0, scale);
    cv::cuda::warpAffine(ioFrame, gpuTemp, M, ioFrame.size());

    if (steps == 1)
    {
        // Simple blend for 1 step - faster
        cv::cuda::addWeighted(ioFrame, 0.5, gpuTemp, 0.5, 0, ioFrame);
    }
    else
    {
        // 2 step blend
        ioFrame.convertTo(gpuTempF1, CV_32FC3);
        gpuTemp.convertTo(gpuTempF2, CV_32FC3);
        cv::cuda::add(gpuTempF1, gpuTempF2, gpuTempF1);

        float   scale2 = 1.0f + (amount * 0.06f);
        cv::Mat M2 = cv::getRotationMatrix2D(cv::Point2f(cx, cy), 0, scale2);
        cv::cuda::warpAffine(ioFrame, gpuTemp, M2, ioFrame.size());
        gpuTemp.convertTo(gpuTempF2, CV_32FC3);
        cv::cuda::add(gpuTempF1, gpuTempF2, gpuTempF1);

        cv::cuda::divide(gpuTempF1, cv::Scalar(3, 3, 3), gpuTempF1);
        gpuTempF1.convertTo(ioFrame, CV_8UC3);
    }
}

void VideoFXModule::applyEdgeGlow_gpu(
    cv::cuda::GpuMat& ioFrame,
    float             amount,
    float             r,
    float             g,
    float             b)
{
    if (amount < 0.05f)
        return; // Skip for very low glow values

    // Cache the Canny detector and dilate filter to avoid recreation each frame
    static cv::Ptr<cv::cuda::CannyEdgeDetector> canny;
    static cv::Ptr<cv::cuda::Filter>            dilateFilter;
    if (!canny)
        canny = cv::cuda::createCannyEdgeDetector(50, 150);
    if (!dilateFilter)
        dilateFilter =
            cv::cuda::createMorphologyFilter(cv::MORPH_DILATE, CV_8UC1, cv::Mat::ones(3, 3, CV_8U));

    cv::cuda::cvtColor(ioFrame, gpuGray, cv::COLOR_BGR2GRAY);

    canny->detect(gpuGray, gpuTemp); // Use gpuTemp for edges instead of allocating

    dilateFilter->apply(gpuTemp, gpuTemp); // Single dilate pass (was 2)

    // Optimized: use gpuTempF1 for glow color, gpuTempF2 for edge mask
    cv::cuda::cvtColor(gpuTemp, gpuTemp, cv::COLOR_GRAY2BGR);
    gpuTemp.convertTo(gpuTempF1, CV_32FC3, 1.0 / 255.0);

    // Create color glow and multiply
    cv::Scalar glowColor(b * 255, g * 255, r * 255);
    gpuTempF2.create(ioFrame.size(), CV_32FC3);
    gpuTempF2.setTo(glowColor);
    cv::cuda::multiply(gpuTempF2, gpuTempF1, gpuTempF1);

    gpuTempF1.convertTo(gpuTemp, CV_8UC3);
    cv::cuda::addWeighted(ioFrame, 1.0, gpuTemp, amount, 0, ioFrame);
}

#endif
