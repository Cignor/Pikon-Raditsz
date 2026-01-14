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
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout VideoFXModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // GPU toggle
    params.push_back(std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", true));

    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Zoom Level", juce::StringArray{"Small", "Normal", "Large"}, 1));

    // Color
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "brightness", "Brightness", -100.0f, 100.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("contrast", "Contrast", 0.0f, 3.0f, 1.0f));
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

    // Filters & Effects
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("sharpen", "Sharpen", 0.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("blur", "Blur", 0.0f, 20.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>("grayscale", "Grayscale", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("invert", "Invert Colors", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("flipH", "Flip Horizontal", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("flipV", "Flip Vertical", false));

    // Threshold Effect
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("thresholdEnable", "Enable Threshold", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "thresholdLevel", "Threshold Level", 0.0f, 255.0f, 127.0f));

    // New Effects
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
        std::make_unique<juce::AudioParameterBool>("cannyEnable", "Edge Detect", false));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "cannyThresh1", "Canny Thresh 1", 0.0f, 255.0f, 50.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "cannyThresh2", "Canny Thresh 2", 0.0f, 255.0f, 150.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "kaleidoscope", "Kaleidoscope", juce::StringArray{"None", "4-Way", "8-Way"}, 0));

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
    // GPU toggle
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));

    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    brightnessParam = apvts.getRawParameterValue("brightness");
    contrastParam = apvts.getRawParameterValue("contrast");
    saturationParam = apvts.getRawParameterValue("saturation");
    hueShiftParam = apvts.getRawParameterValue("hueShift");
    gainRedParam = apvts.getRawParameterValue("gainRed");
    gainGreenParam = apvts.getRawParameterValue("gainGreen");
    gainBlueParam = apvts.getRawParameterValue("gainBlue");
    sharpenParam = apvts.getRawParameterValue("sharpen");
    blurParam = apvts.getRawParameterValue("blur");
    grayscaleParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("grayscale"));
    invertParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("invert"));
    flipHorizontalParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("flipH"));
    flipVerticalParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("flipV"));

    // Initialize threshold parameters
    thresholdEnableParam =
        dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("thresholdEnable"));
    thresholdLevelParam = apvts.getRawParameterValue("thresholdLevel");

    // Initialize new effect parameters
    sepiaParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("sepia"));
    temperatureParam = apvts.getRawParameterValue("temperature");
    posterizeLevelsParam =
        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("posterizeLevels"));
    vignetteAmountParam = apvts.getRawParameterValue("vignetteAmount");
    vignetteSizeParam = apvts.getRawParameterValue("vignetteSize");
    pixelateBlockSizeParam =
        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("pixelateSize"));
    cannyEnableParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("cannyEnable"));
    cannyThresh1Param = apvts.getRawParameterValue("cannyThresh1");
    cannyThresh2Param = apvts.getRawParameterValue("cannyThresh2");
    kaleidoscopeModeParam =
        dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("kaleidoscope"));
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

void VideoFXModule::run()
{
    cv::Mat processedFrame;
#if defined(WITH_CUDA_SUPPORT)
    cv::cuda::GpuMat gpuFrame;
#endif

    while (!threadShouldExit())
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
                cv::cuda::GpuMat gFrame = VideoFrameManager::getInstance().getGpuFrame(sourceId);
                if (!gFrame.empty())
                {
                    gpuFrame = gFrame; // Use the reference or copy? GpuMat is ref-counted.
                    // But we need a writable copy if we modify it in place?
                    // The apply functions function in-place usually or take src/dst.
                    // Let's ensure we work on a copy if we want to preserve input (though we are
                    // consumimg it). Actually VideoFX usually acts as a filter. We should clone if
                    // we don't own it. But GpuMat clone is deep copy. We can just rely on the fact
                    // that we output to a NEW gpuFrame or modify in place if safe. But we set
                    // `gpuFrame` to it.
                    gpuInputReady = true;
                }
            }
#endif
        }

        if (!gpuInputReady)
        {
            // CPU Fetch Fallback
            frame = prefetchedFrame.empty() ? VideoFrameManager::getInstance().getFrame(sourceId)
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
                if (!gpuInputReady)
                    gpuFrame.upload(frame);

                // Color Adjustments
                applyBrightnessContrast_gpu(gpuFrame, brightness, contrast);
                applyTemperature_gpu(gpuFrame, temperature);
                applySepia_gpu(gpuFrame, sepia);
                applySaturationHue_gpu(gpuFrame, saturation, hueShift);
                applyRgbGain_gpu(gpuFrame, gainR, gainG, gainB);
                applyPosterize_gpu(gpuFrame, posterizeLevels);

                // Monochrome & Edge Effects
                applyGrayscale_gpu(gpuFrame, grayscale);
                // Canny is CPU-only fallback; skip it in GPU mode
                if (thresholdEnable)
                {
                    applyThreshold_gpu(gpuFrame, thresholdLevel);
                }
                applyInvert_gpu(gpuFrame, invert);

                // Geometric & Spatial Filters
                applyFlip_gpu(gpuFrame, flipH, flipV);
                applyVignette_gpu(gpuFrame, vignetteAmount, vignetteSize);
                applyPixelate_gpu(gpuFrame, pixelateSize);
                applyBlur_gpu(gpuFrame, blur);
                applySharpen_gpu(gpuFrame, sharpen);
                applyKaleidoscope_gpu(gpuFrame, kaleidoscopeMode);

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
                    "[VideoFX] GPU Error: " + juce::String(e.what()) + ". Falling back to CPU.");
                processedFrame = frame.clone();
                goto cpu_path_label;
            }
#endif
        }
        else
        {
        cpu_path_label:
            processedFrame = frame.clone();

            // Color Adjustments
            applyBrightnessContrast(processedFrame, brightness, contrast);
            applyTemperature(processedFrame, temperature);
            applySepia(processedFrame, sepia);
            applySaturationHue(processedFrame, saturation, hueShift);
            applyRgbGain(processedFrame, gainR, gainG, gainB);
            applyPosterize(processedFrame, posterizeLevels);

            // Monochrome & Edge Effects
            applyGrayscale(processedFrame, grayscale);
            if (cannyEnable)
            {
                applyCanny(processedFrame, cannyThresh1, cannyThresh2);
            }
            else if (thresholdEnable)
            {
                applyThreshold(processedFrame, thresholdLevel);
            }
            applyInvert(processedFrame, invert);

            // Geometric & Spatial Filters
            applyFlip(processedFrame, flipH, flipV);
            applyVignette(processedFrame, vignetteAmount, vignetteSize);
            applyPixelate(processedFrame, pixelateSize);
            applyBlur(processedFrame, blur);
            applySharpen(processedFrame, sharpen);
            applyKaleidoscope(processedFrame, kaleidoscopeMode);
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

    ImGui::PushItemWidth(itemWidth);

    // --- FEATURE: RESET BUTTON ---
    if (ImGui::Button("Reset All Effects", ImVec2(itemWidth, 0)))
    {
        // Reset all parameters to their default values
        const char* paramIds[] = {
            "useGpu",         "zoomLevel",       "brightness",     "contrast",
            "saturation",     "hueShift",        "gainRed",        "gainGreen",
            "gainBlue",       "sepia",           "temperature",    "sharpen",
            "blur",           "grayscale",       "invert",         "flipH",
            "flipV",          "thresholdEnable", "thresholdLevel", "posterizeLevels",
            "vignetteAmount", "vignetteSize",    "pixelateSize",   "cannyEnable",
            "cannyThresh1",   "cannyThresh2",    "kaleidoscope"};

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

    themeText("Color Adjustments", theme.modules.videofx_section_subheader);

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
    float contrast = contrastMod ? getLiveParamValue("contrast", contrastDefault) : contrastDefault;
    if (contrastMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Contrast", &contrast, 0.0f, 3.0f))
    {
        if (!contrastMod)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("contrast")))
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
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("hueShift")))
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
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gainRed")))
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
    float       gainG = gainGreenMod ? getLiveParamValue("gainGreen", gainGDefault) : gainGDefault;
    if (gainGreenMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Green Gain", &gainG, 0.0f, 2.0f))
    {
        if (!gainGreenMod)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gainGreen")))
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
    float       gainB = gainBlueMod ? getLiveParamValue("gainBlue", gainBDefault) : gainBDefault;
    if (gainBlueMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Blue Gain", &gainB, 0.0f, 2.0f))
    {
        if (!gainBlueMod)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("gainBlue")))
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
    float       temperature =
        temperatureMod ? getLiveParamValue("temperature", temperatureDefault) : temperatureDefault;
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

    themeText("Filters & Effects", theme.modules.videofx_section_subheader);

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
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("sharpen")))
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

    themeText("More Filters", theme.modules.videofx_section_subheader);

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
            threshLevelMod
                ? getLiveParamValue(
                      "thresholdLevel", thresholdLevelParam ? thresholdLevelParam->load() : 127.0f)
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
            adjustParamOnWheel(apvts.getParameter("thresholdLevel"), "thresholdLevel", threshLevel);
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
        adjustParamOnWheel(apvts.getParameter("pixelateSize"), "pixelateSize", (float)pixelateSize);
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
            cannyTh1Mod ? getLiveParamValue(
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
        float cannyTh2 = cannyTh2Mod ? getLiveParamValue(
                                           "cannyThresh2",
                                           cannyThresh2Param ? cannyThresh2Param->load() : 150.0f)
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

    themeText("Advanced Effects", theme.modules.videofx_section_subheader);

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
            if (auto* p =
                    dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("vignetteAmount")))
                *p = vignetteAmount;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !vignetteAmountMod)
        onModificationEnded();
    if (!vignetteAmountMod)
        adjustParamOnWheel(apvts.getParameter("vignetteAmount"), "vignetteAmount", vignetteAmount);
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
            adjustParamOnWheel(apvts.getParameter("vignetteSize"), "vignetteSize", vignetteSize);
        if (vignetteSizeMod)
            ImGui::EndDisabled();
    }

    // Kaleidoscope
    bool kaleidoscopeMod = isParamModulated("kaleidoscope");
    if (kaleidoscopeMod)
        ImGui::BeginDisabled();
    int         kaleidoscopeMode = kaleidoscopeModeParam ? kaleidoscopeModeParam->getIndex() : 0;
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

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();
}

void VideoFXModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Pins are handled via getDynamicInputPins/getDynamicOutputPins for proper Video type coloring
    // This method is called but dynamic pins take precedence
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

    // Efficient GPU implementation using a single GEMM (matrix multiply):
    // Flatten (W*H, 3) x (3,3) = (W*H,3), then reshape back to (H,W,3).

    // 3x3 kernel in BGR order (rows: output B,G,R; cols: input B,G,R)
    static cv::Mat sepiaKernelCpu =
        (cv::Mat_<float>(3, 3) << 0.131f,
         0.534f,
         0.272f, // B' = 0.131*B + 0.534*G + 0.272*R
         0.168f,
         0.686f,
         0.349f, // G'
         0.189f,
         0.769f,
         0.393f // R'
        );

    // Cache the kernel on GPU
    static cv::cuda::GpuMat sepiaKernelGpu;
    if (sepiaKernelGpu.empty())
        sepiaKernelGpu.upload(sepiaKernelCpu);

    // 1) Reshape to (W*H, 3) with single channel
    cv::cuda::GpuMat flat1C = ioFrame.reshape(1, ioFrame.rows * ioFrame.cols);

    // 2) Convert to float32
    flat1C.convertTo(gpuTempF1, CV_32F);

    // 3) GEMM: (W*H,3) * (3,3) -> (W*H,3)
    cv::cuda::gemm(gpuTempF1, sepiaKernelGpu, 1.0, cv::cuda::GpuMat(), 0.0, gpuTempF2);

    // 4) Back to 8U
    gpuTempF2.convertTo(gpuTemp, CV_8U);

    // 5) Reshape back to 3 channels (H,W,3)
    cv::cuda::GpuMat reshaped = gpuTemp.reshape(3, ioFrame.rows);
    reshaped.copyTo(ioFrame);
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
    // 9. Canny Edge Detect
    // Note: cv::cuda::Canny() and cv::cuda::createCannyDetector() are not available
    // in standard OpenCV CUDA. We fall back to CPU for Canny edge detection.
    // This is a known limitation - Canny requires complex edge detection algorithms
    // that may not be available in the CUDA module.

    // 1. Convert to grayscale on GPU
    cv::cuda::cvtColor(ioFrame, gpuGray, cv::COLOR_BGR2GRAY);

    // 2. Download to CPU for Canny processing
    cv::Mat grayCpu;
    gpuGray.download(grayCpu);

    // 3. Apply Canny on CPU
    cv::Mat edgesCpu;
    cv::Canny(grayCpu, edgesCpu, thresh1, thresh2);

    // 4. Upload back to GPU
    gpuTemp.upload(edgesCpu);

    // 5. Convert the single-channel edge map back to 3-channel BGR
    cv::cuda::cvtColor(gpuTemp, ioFrame, cv::COLOR_GRAY2BGR);
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

    // Apply the mask (3-channel 8U * 1-channel 32F = 3-channel 32F)
    ioFrame.convertTo(gpuTemp, CV_32F);
    cv::cuda::multiply(gpuTemp, gpuVignetteMask, gpuTemp);
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

#endif
