#include "ChromakeyModuleProcessor.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include "../../utils/CudaDeviceCountCache.h"
#include <opencv2/imgproc.hpp>
#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#endif

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <juce_opengl/juce_opengl.h>
#include "../../preset_creator/theme/ThemeManager.h"
#include <unordered_map>
#endif

juce::AudioProcessorValueTreeState::ParameterLayout ChromakeyModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // GPU toggle
    params.push_back(std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", true));

    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Zoom Level", juce::StringArray{"Small", "Normal", "Large"}, 1));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "spillSuppression", "Spill Suppression", 0.0f, 1.0f, 0.5f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "featherAmount", "Feather Amount", 0.0f, 20.0f, 2.0f));

    return {params.begin(), params.end()};
}

ChromakeyModuleProcessor::ChromakeyModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::mono(), true)
              .withOutput("RGBA Video", juce::AudioChannelSet::mono(), true)
              .withOutput("Mask", juce::AudioChannelSet::mono(), true)),
      juce::Thread("Chromakey Thread"),
      apvts(*this, nullptr, "ChromakeyParams", createParameterLayout())
{
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    spillSuppressionParam = apvts.getRawParameterValue("spillSuppression");
    featherAmountParam = apvts.getRawParameterValue("featherAmount");
}

ChromakeyModuleProcessor::~ChromakeyModuleProcessor()
{
    stopThread(5000);
    VideoFrameManager::getInstance().removeSource(getLogicalId());
    VideoFrameManager::getInstance().removeSource(getSecondaryLogicalId());
}

void ChromakeyModuleProcessor::prepareToPlay(double, int) { startThread(); }

void ChromakeyModuleProcessor::releaseResources()
{
    signalThreadShouldExit();
    stopThread(5000);
}

void ChromakeyModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    juce::ignoreUnused(midi);

    // Read the Source ID from our input pin
    auto inputBuffer = getBusBuffer(buffer, true, 0);
    if (inputBuffer.getNumSamples() > 0)
    {
        currentSourceId.store((juce::uint32)inputBuffer.getSample(0, 0));
    }

    buffer.clear();

    // Find our own ID if it's not set
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

    // Output primary Logical ID on bus 0 (RGBA Video)
    auto rgbaOutBus = getBusBuffer(buffer, false, 0);
    if (rgbaOutBus.getNumChannels() > 0 && rgbaOutBus.getNumSamples() > 0)
    {
        float primaryId = (float)myLogicalId;
        for (int sample = 0; sample < rgbaOutBus.getNumSamples(); ++sample)
        {
            rgbaOutBus.setSample(0, sample, primaryId);
        }
    }

    // Output secondary Logical ID on bus 1 (Mask/Alpha)
    auto maskOutBus = getBusBuffer(buffer, false, 1);
    if (maskOutBus.getNumChannels() > 0 && maskOutBus.getNumSamples() > 0)
    {
        float secondaryId = static_cast<float>(getSecondaryLogicalId());
        for (int sample = 0; sample < maskOutBus.getNumSamples(); ++sample)
        {
            maskOutBus.setSample(0, sample, secondaryId);
        }
    }
}

void ChromakeyModuleProcessor::run()
{
    cv::Mat processedFrame;
    cv::Mat rgbaOutput;
    cv::Mat alphaOutput;

    int frameCounter = 0;
    // GPU buffers are now class members (persistent)

    while (!threadShouldExit())
    {
        frameCounter++;
        // Get our logical ID first
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

        // Always check connection snapshot dynamically to respond to plug/unplug events
        juce::uint32 sourceId = 0;
        if (parentSynth != nullptr && myLogicalId != 0)
        {
            auto snapshot = parentSynth->getConnectionSnapshot();
            if (snapshot && !snapshot->empty())
            {
                // Look for input connection to our first input pin (channel 0)
                for (const auto& conn : *snapshot)
                {
                    if (conn.dstLogicalId == myLogicalId && conn.dstChan == 0)
                    {
                        sourceId = conn.srcLogicalId;
                        break;
                    }
                }
            }
        }

        // Also check the audio input pin (from processBlock)
        juce::uint32 pinSourceId = currentSourceId.load();
        if (pinSourceId != 0)
        {
            sourceId = pinSourceId;
        }

        // Update cached source ID
        if (sourceId != 0)
        {
            cachedResolvedSourceId = sourceId;
        }
        else
        {
            // Clear cache when no connection
            cachedResolvedSourceId = 0;
            const juce::ScopedLock lk(frameLock);
            lastFrameBgr = cv::Mat(); // Clear cached frame
        }

        // Only process if we have a valid input connection
        if (sourceId == 0)
        {
            // No input connected - clear outputs and wait
            updateGuiFrame(cv::Mat());
            wait(33);
            continue;
        }

        // Monitor performance
        auto startTime = juce::Time::getMillisecondCounterHiRes();

        // Get frame from connected source
        // Prefer GPU frame if we are running in GPU mode
        bool processingOnGpu = false;

#if defined(WITH_CUDA_SUPPORT)
        // Declare GPU outputs here locally so they persist for setGpuFrame call
        // Wrappers managed in outer scope for persistence

        // Check local caching/state to guess if we wanted GPU,
        // but dynamic check inside applyChromakey decides mostly.
        // To avoid download, we must peek/fetch GPU frame if possible.
        // We need to know if we intend to run on GPU.
        bool useGpuParamVal = useGpuParam ? useGpuParam->get() : false;
        bool hasCuda = CudaDeviceCountCache::isAvailable();

        if (useGpuParamVal && hasCuda)
        {
            cv::cuda::GpuMat gpuFrame = VideoFrameManager::getInstance().getGpuFrame(sourceId);
            if (!gpuFrame.empty())
            {
                processingOnGpu = true;

                // Store for eye dropper (download needed unfortunately if we want to sample on CPU)
                // But for eye dropper we can do it lazily or throttled.
                // For now, let's skip automatic download for eyedropper every frame to save speed.
                // Only download if UI requests or periodically?
                // Let's download for lastFrameBgr because that's used for UI preview
                // (updateGuiFrame usually takes CPU Mat or Image). updateGuiFrame takes cv::Mat. So
                // we need a CPU copy for UI anyway, BUT that shouldn't stall the pipeline for
                // downstream nodes. We will perform processing on GPU, then download specific
                // result for UI/Preview.

                applyChromakeyGpu(gpuFrame, gpuRgbaOutput, gpuAlphaOutput);

                // Publish outputs to manager (GPU)
                if (myLogicalId != 0)
                {
                    VideoFrameManager::getInstance().setGpuFrame(myLogicalId, gpuRgbaOutput);
                    VideoFrameManager::getInstance().setGpuFrame(
                        getSecondaryLogicalId(), gpuAlphaOutput);
                }

                // Download for UI preview (throttled or async?)
                // For now, sync download for UI to keep it working
                // Download for UI preview (throttled)
                if (frameCounter % 4 == 0)
                {
                    gpuRgbaOutput.download(processedFrame);
                }
                rgbaOutput = processedFrame; // alias (points to new or old data)
            }
        }
#endif

        if (!processingOnGpu)
        {
            // Original CPU path
            cv::Mat frame = VideoFrameManager::getInstance().getFrame(sourceId);
            if (frame.empty())
            {
                const juce::ScopedLock lk(frameLock);
                if (!lastFrameBgr.empty())
                    frame = lastFrameBgr.clone();
            }

            if (!frame.empty())
            {
                {
                    const juce::ScopedLock lk(frameLock);
                    frame.copyTo(lastFrameBgr);
                    const juce::ScopedLock lk2(originalFrameLock);
                    frame.copyTo(originalInputFrame);
                }

                applyChromakey(frame, rgbaOutput, alphaOutput);

                // Publish outputs (CPU)
                if (myLogicalId != 0)
                {
                    VideoFrameManager::getInstance().setFrame(myLogicalId, rgbaOutput);
                    VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), alphaOutput);
                }
            }
        }

        // GUI Updates and Logging
        // GUI Updates (throttled)
        if (frameCounter % 4 == 0)
            updateGuiFrame(rgbaOutput);

        auto elapsed = juce::Time::getMillisecondCounterHiRes() - startTime;
        lastProcessTimeMs = (float)elapsed;
        lastProcessWasGpu = processingOnGpu;

        static int logCounter = 0;
        if (++logCounter % 60 == 0) // Log once per second approx
        {
            juce::Logger::writeToLog(
                "[Perf] Chromakey (ID: " + juce::String(myLogicalId) + "): " +
                juce::String(elapsed, 2) + " ms (GPU: " + (processingOnGpu ? "ON" : "OFF") + ")");
        }

        wait(33); // ~30 FPS
    }
}

// CPU Implementation
void ChromakeyModuleProcessor::applyChromakey(
    cv::Mat& inputFrame,
    cv::Mat& outputRgba,
    cv::Mat& outputAlpha)
{
    if (inputFrame.empty())
    {
        outputRgba = cv::Mat();
        outputAlpha = cv::Mat();
        return;
    }

    std::vector<ChromakeyColor> colors;
    {
        const juce::ScopedLock lock(colorListLock);
        colors = selectedColors;
    }

    if (colors.empty())
    {
        cv::cvtColor(inputFrame, outputRgba, cv::COLOR_BGR2BGRA);
        outputAlpha = cv::Mat::ones(inputFrame.rows, inputFrame.cols, CV_8UC1) * 255;
        return;
    }

    cv::Mat combinedMask;
    cv::Mat alphaMask;
    cv::Mat bgrFrame;

    // Convert to HSV for color matching
    cv::Mat hsvFrame;
    cv::cvtColor(inputFrame, hsvFrame, cv::COLOR_BGR2HSV);

    // Create combined mask from all colors
    combinedMask = cv::Mat::zeros(hsvFrame.rows, hsvFrame.cols, CV_8UC1);

    for (const auto& color : colors)
    {
        cv::Mat colorMask = createColorMask(hsvFrame, color);
        cv::bitwise_or(combinedMask, colorMask, combinedMask);
    }

    // Invert mask
    cv::bitwise_not(combinedMask, alphaMask);
    float featherAmount = featherAmountParam ? featherAmountParam->load() : 2.0f;
    if (featherAmount > 0.1f)
    {
        applyFeathering(alphaMask, featherAmount);
    }

    // Apply spill suppression
    bgrFrame = inputFrame.clone();
    float spillAmount = spillSuppressionParam ? spillSuppressionParam->load() : 0.5f;
    if (spillAmount > 0.01f)
    {
        applySpillSuppression(bgrFrame, alphaMask, spillAmount);
    }

    // Create BGRA output
    std::vector<cv::Mat> channels;
    cv::split(bgrFrame, channels);
    channels.push_back(alphaMask);
    cv::merge(channels, outputRgba);

    cv::threshold(alphaMask, outputAlpha, 127, 255, cv::THRESH_BINARY);
}

#if defined(WITH_CUDA_SUPPORT)
void ChromakeyModuleProcessor::applyChromakeyGpu(
    const cv::cuda::GpuMat& inputFrame,
    cv::cuda::GpuMat&       outputRgba,
    cv::cuda::GpuMat&       outputAlpha)
{
    if (inputFrame.empty())
        return;

    // Get color list (thread-safe)
    std::vector<ChromakeyColor> colors;
    {
        const juce::ScopedLock lock(colorListLock);
        colors = selectedColors;
    }

    // If no colors selected, output opaque frame
    if (colors.empty())
    {
        cv::cuda::GpuMat bgraFrame;
        cv::cuda::cvtColor(inputFrame, bgraFrame, cv::COLOR_BGR2BGRA);
        outputRgba = bgraFrame;

        // White alpha
        cv::cuda::GpuMat alpha(inputFrame.size(), CV_8UC1, cv::Scalar(255));
        outputAlpha = alpha;
        return;
    }

    try
    {
        // 1. Convert to HSV on GPU (uses persistent buffer)
        cv::cuda::cvtColor(inputFrame, gpuHsv, cv::COLOR_BGR2HSV);

        // 2. Initialize combined mask (ensure size/type)
        if (gpuCombinedMask.size() != inputFrame.size() || gpuCombinedMask.type() != CV_8UC1)
        {
            gpuCombinedMask.create(inputFrame.size(), CV_8UC1);
        }
        gpuCombinedMask.setTo(cv::Scalar(0));

        // 3. Accumulate masks using persistent temp buffer
        for (const auto& color : colors)
        {
            // Inline mask creation using gpuTempMask
            // Calculate tolerance-adjusted bounds
            double centerH = 0.5 * (color.hsvLower[0] + color.hsvUpper[0]);
            double centerS = 0.5 * (color.hsvLower[1] + color.hsvUpper[1]);
            double centerV = 0.5 * (color.hsvLower[2] + color.hsvUpper[2]);
            double deltaH = 0.5 * (color.hsvUpper[0] - color.hsvLower[0]);
            double deltaS = 0.5 * (color.hsvUpper[1] - color.hsvLower[1]);
            double deltaV = 0.5 * (color.hsvUpper[2] - color.hsvLower[2]);
            double scale = juce::jlimit(0.1, 5.0, (double)color.tolerance);

            double lowH = juce::jlimit(0.0, 179.0, centerH - deltaH * scale);
            double highH = juce::jlimit(0.0, 179.0, centerH + deltaH * scale);
            double lowS = juce::jlimit(0.0, 255.0, centerS - deltaS * scale);
            double highS = juce::jlimit(0.0, 255.0, centerS + deltaS * scale);
            double lowV = juce::jlimit(0.0, 255.0, centerV - deltaV * scale);
            double highV = juce::jlimit(0.0, 255.0, centerV + deltaV * scale);

            cv::Scalar lower(lowH, lowS, lowV);
            cv::Scalar upper(highH, highS, highV);

            // Reuses gpuTempMask memory (reallocates only if size changed)
            cv::cuda::inRange(gpuHsv, lower, upper, gpuTempMask);

            if (color.inverted)
            {
                cv::cuda::bitwise_not(gpuTempMask, gpuTempMask);
            }

            cv::cuda::bitwise_or(gpuCombinedMask, gpuTempMask, gpuCombinedMask);
        }

        // 4. Invert combined mask -> Alpha
        // Use member gpuAlphaMask
        cv::cuda::bitwise_not(gpuCombinedMask, gpuAlphaMask);

        // 5. Feathering
        float featherAmount = featherAmountParam ? featherAmountParam->load() : 2.0f;
        if (featherAmount > 0.1f)
        {
            int kernelSize = (int)(featherAmount * 2.0f) | 1;
            if (kernelSize < 3)
                kernelSize = 3;
            auto gaussianFilter = cv::cuda::createGaussianFilter(
                CV_8UC1, CV_8UC1, cv::Size(kernelSize, kernelSize), featherAmount, featherAmount);
            // In-place filtering is allowed
            gaussianFilter->apply(gpuAlphaMask, gpuAlphaMask);
        }

        // 6. Merge BGRA
        std::vector<cv::cuda::GpuMat> gpuChannels;
        cv::cuda::split(inputFrame, gpuChannels);
        gpuChannels.push_back(gpuAlphaMask);

        // Output to persistent buffers
        cv::cuda::merge(gpuChannels, outputRgba);
        outputAlpha = gpuAlphaMask; // Shallow copy ref
    }
    catch (const cv::Exception& e)
    {
        juce::Logger::writeToLog("[Chromakey] GPU Error: " + juce::String(e.what()));
    }
}
#endif

cv::Mat ChromakeyModuleProcessor::createColorMask(
    const cv::Mat&        hsvFrame,
    const ChromakeyColor& color)
{
    cv::Mat mask = cv::Mat::zeros(hsvFrame.rows, hsvFrame.cols, CV_8UC1);

    // Calculate tolerance-adjusted bounds (ColorTracker-style)
    double centerH = 0.5 * (color.hsvLower[0] + color.hsvUpper[0]);
    double centerS = 0.5 * (color.hsvLower[1] + color.hsvUpper[1]);
    double centerV = 0.5 * (color.hsvLower[2] + color.hsvUpper[2]);
    double deltaH = 0.5 * (color.hsvUpper[0] - color.hsvLower[0]);
    double deltaS = 0.5 * (color.hsvUpper[1] - color.hsvLower[1]);
    double deltaV = 0.5 * (color.hsvUpper[2] - color.hsvLower[2]);
    double scale = juce::jlimit(0.1, 5.0, (double)color.tolerance);

    double lowH = juce::jlimit(0.0, 179.0, centerH - deltaH * scale);
    double highH = juce::jlimit(0.0, 179.0, centerH + deltaH * scale);
    double lowS = juce::jlimit(0.0, 255.0, centerS - deltaS * scale);
    double highS = juce::jlimit(0.0, 255.0, centerS + deltaS * scale);
    double lowV = juce::jlimit(0.0, 255.0, centerV - deltaV * scale);
    double highV = juce::jlimit(0.0, 255.0, centerV + deltaV * scale);

    cv::Scalar lower(lowH, lowS, lowV);
    cv::Scalar upper(highH, highS, highV);

    // Use inRange for efficient HSV matching
    cv::inRange(hsvFrame, lower, upper, mask);

    // Apply per-color inversion if set
    if (color.inverted)
    {
        cv::bitwise_not(mask, mask);
    }

    return mask;
}

// createColorMaskGpu removed (inlined)

void ChromakeyModuleProcessor::applySpillSuppression(
    cv::Mat&       bgrFrame,
    const cv::Mat& alphaMask,
    float          amount)
{
    if (bgrFrame.empty() || alphaMask.empty() || amount <= 0.01f)
        return;

    // Convert alpha mask to float for blending
    cv::Mat alphaFloat;
    alphaMask.convertTo(alphaFloat, CV_32F, 1.0f / 255.0f);

    // Create inverse alpha (where we want to suppress spill)
    cv::Mat inverseAlpha;
    cv::subtract(cv::Scalar::all(1.0f), alphaFloat, inverseAlpha);

    // Apply spill suppression: reduce color intensity in areas near the mask edges
    // This is a simplified approach - in production, you'd want more sophisticated spill detection
    std::vector<cv::Mat> channels;
    cv::split(bgrFrame, channels);

    for (auto& channel : channels)
    {
        channel.convertTo(channel, CV_32F);
        cv::multiply(channel, cv::Scalar::all(1.0f) - (inverseAlpha * amount), channel);
        channel.convertTo(channel, CV_8U);
    }

    cv::merge(channels, bgrFrame);
}

void ChromakeyModuleProcessor::applyFeathering(cv::Mat& alphaMask, float featherAmount)
{
    if (alphaMask.empty() || featherAmount <= 0.1f)
        return;

    // Convert to float for smoother blur
    cv::Mat alphaFloat;
    alphaMask.convertTo(alphaFloat, CV_32F, 1.0f / 255.0f);

    // Calculate kernel size (must be odd)
    int ksize = static_cast<int>(std::round(featherAmount * 2.0f));
    if (ksize % 2 == 0)
        ksize++;
    if (ksize < 3)
        ksize = 3;

    // Apply Gaussian blur
    cv::GaussianBlur(alphaFloat, alphaFloat, cv::Size(ksize, ksize), 0);

    // Convert back to 8-bit
    alphaFloat.convertTo(alphaMask, CV_8U, 255.0f);
}

void ChromakeyModuleProcessor::addColorAt(int x, int y, int radius)
{
    // Get frame for sampling
    cv::Mat frameCopy;
    {
        const juce::ScopedLock lk(originalFrameLock);
        if (!originalInputFrame.empty())
            frameCopy = originalInputFrame.clone();
    }
    if (frameCopy.empty())
        return;

    // Clamp coordinates
    const int mx = juce::jlimit(0, frameCopy.cols - 1, x);
    const int my = juce::jlimit(0, frameCopy.rows - 1, y);

    // Create ROI for median sampling
    cv::Rect roi(
        std::max(0, mx - radius), std::max(0, my - radius), radius * 2 + 1, radius * 2 + 1);
    roi &= cv::Rect(0, 0, frameCopy.cols, frameCopy.rows);

    if (roi.area() <= 0)
        return;

    // Calculate median color from ROI (ColorTracker-style)
    cv::Scalar avgBgr = cv::mean(frameCopy(roi));
    cv::Vec3b  bgr8((uchar)avgBgr[0], (uchar)avgBgr[1], (uchar)avgBgr[2]);

    // Convert to HSV
    cv::Mat onePix(1, 1, CV_8UC3);
    onePix.at<cv::Vec3b>(0, 0) = bgr8;
    cv::Mat onePixHsv;
    cv::cvtColor(onePix, onePixHsv, cv::COLOR_BGR2HSV);
    cv::Vec3b avgHsv = onePixHsv.at<cv::Vec3b>(0, 0);
    int       avgHue = (int)avgHsv[0];
    int       avgSat = (int)avgHsv[1];
    int       avgVal = (int)avgHsv[2];

    const juce::ScopedLock lock(colorListLock);
    int                    targetIdx = pickerTargetIndex.load();

    if (targetIdx < 0 || targetIdx >= (int)selectedColors.size())
    {
        // Add new color
        ChromakeyColor tc;
        tc.displayColour =
            juce::Colour((juce::uint8)bgr8[2], (juce::uint8)bgr8[1], (juce::uint8)bgr8[0]);
        tc.hsvLower = cv::Scalar(
            juce::jlimit(0, 179, avgHue - 10),
            juce::jlimit(0, 255, avgSat - 40),
            juce::jlimit(0, 255, avgVal - 40));
        tc.hsvUpper = cv::Scalar(
            juce::jlimit(0, 179, avgHue + 10),
            juce::jlimit(0, 255, avgSat + 40),
            juce::jlimit(0, 255, avgVal + 40));
        tc.tolerance = 1.0f;
        tc.inverted = false;
        selectedColors.push_back(tc);
    }
    else
    {
        // Update existing color
        auto& tc = selectedColors[(size_t)targetIdx];
        tc.displayColour =
            juce::Colour((juce::uint8)bgr8[2], (juce::uint8)bgr8[1], (juce::uint8)bgr8[0]);
        tc.hsvLower = cv::Scalar(
            juce::jlimit(0, 179, avgHue - 10),
            juce::jlimit(0, 255, avgSat - 40),
            juce::jlimit(0, 255, avgVal - 40));
        tc.hsvUpper = cv::Scalar(
            juce::jlimit(0, 179, avgHue + 10),
            juce::jlimit(0, 255, avgSat + 40),
            juce::jlimit(0, 255, avgVal + 40));
    }

    // Finalize picker state
    isColorPickerActive.store(false);
    pickerTargetIndex.store(-1);
}

void ChromakeyModuleProcessor::removeColor(int index)
{
    const juce::ScopedLock lock(colorListLock);
    if (index >= 0 && index < (int)selectedColors.size())
    {
        selectedColors.erase(selectedColors.begin() + index);
    }
}

void ChromakeyModuleProcessor::setColorInverted(int index, bool inverted)
{
    const juce::ScopedLock lock(colorListLock);
    if (index >= 0 && index < (int)selectedColors.size())
    {
        selectedColors[index].inverted = inverted;
    }
}

void ChromakeyModuleProcessor::setColorTolerance(int index, float tolerance)
{
    const juce::ScopedLock lock(colorListLock);
    if (index >= 0 && index < (int)selectedColors.size())
    {
        selectedColors[index].tolerance = juce::jlimit(0.1f, 5.0f, tolerance);
    }
}

void ChromakeyModuleProcessor::updateGuiFrame(const cv::Mat& frame)
{
    if (frame.empty())
        return;

    cv::Mat bgraFrame;
    if (frame.channels() == 4)
    {
        bgraFrame = frame;
    }
    else
    {
        cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);
    }

    const juce::ScopedLock lock(imageLock);

    if (latestFrameForGui.isNull() || latestFrameForGui.getWidth() != bgraFrame.cols ||
        latestFrameForGui.getHeight() != bgraFrame.rows)
    {
        latestFrameForGui = juce::Image(juce::Image::ARGB, bgraFrame.cols, bgraFrame.rows, true);
    }

    juce::Image::BitmapData destData(latestFrameForGui, juce::Image::BitmapData::writeOnly);
    memcpy(destData.data, bgraFrame.data, bgraFrame.total() * bgraFrame.elemSize());
}

juce::Image ChromakeyModuleProcessor::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

juce::ValueTree ChromakeyModuleProcessor::getExtraStateTree() const
{
    juce::ValueTree state("ChromakeyState");

    const juce::ScopedLock lock(colorListLock);
    for (size_t i = 0; i < selectedColors.size(); ++i)
    {
        const auto&     color = selectedColors[i];
        juce::ValueTree colorTree("Color");
        colorTree.setProperty("displayColour", color.displayColour.toString(), nullptr);
        // Save HSV bounds
        colorTree.setProperty("hsvLower0", (int)color.hsvLower[0], nullptr);
        colorTree.setProperty("hsvLower1", (int)color.hsvLower[1], nullptr);
        colorTree.setProperty("hsvLower2", (int)color.hsvLower[2], nullptr);
        colorTree.setProperty("hsvUpper0", (int)color.hsvUpper[0], nullptr);
        colorTree.setProperty("hsvUpper1", (int)color.hsvUpper[1], nullptr);
        colorTree.setProperty("hsvUpper2", (int)color.hsvUpper[2], nullptr);
        colorTree.setProperty("tolerance", color.tolerance, nullptr);
        colorTree.setProperty("inverted", color.inverted, nullptr);
        state.appendChild(colorTree, nullptr);
    }

    return state;
}

void ChromakeyModuleProcessor::setExtraStateTree(const juce::ValueTree& state)
{
    if (!state.hasType("ChromakeyState"))
        return;

    const juce::ScopedLock lock(colorListLock);
    selectedColors.clear();

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto colorTree = state.getChild(i);
        if (colorTree.hasType("Color"))
        {
            ChromakeyColor color;
            color.displayColour = juce::Colour::fromString(
                colorTree.getProperty("displayColour", "ff000000").toString());
            // Load HSV bounds
            int hL = (int)colorTree.getProperty("hsvLower0", 0);
            int sL = (int)colorTree.getProperty("hsvLower1", 100);
            int vL = (int)colorTree.getProperty("hsvLower2", 100);
            int hU = (int)colorTree.getProperty("hsvUpper0", 10);
            int sU = (int)colorTree.getProperty("hsvUpper1", 255);
            int vU = (int)colorTree.getProperty("hsvUpper2", 255);
            color.hsvLower = cv::Scalar(hL, sL, vL);
            color.hsvUpper = cv::Scalar(hU, sU, vU);
            color.tolerance = (float)(double)colorTree.getProperty("tolerance", 1.0);
            color.inverted = colorTree.getProperty("inverted", false);
            selectedColors.push_back(color);
        }
    }
}

std::vector<DynamicPinInfo> ChromakeyModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"Source In", 0, PinDataType::Video});
    return pins;
}

std::vector<DynamicPinInfo> ChromakeyModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"RGBA Video", 0, PinDataType::Video});
    pins.push_back({"Mask", 1, PinDataType::Video});
    return pins;
}

#if defined(PRESET_CREATOR_UI)
ImVec2 ChromakeyModuleProcessor::getCustomNodeSize() const
{
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void ChromakeyModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // GPU toggle
#if defined(WITH_CUDA_SUPPORT)
    bool useGpu = useGpuParam ? useGpuParam->get() : false;
    if (ImGui::Checkbox("Use GPU (CUDA)", &useGpu))
    {
        if (useGpuParam)
            useGpuParam->setValueNotifyingHost(useGpu ? 1.0f : 0.0f);
        onModificationEnded();
    }
    if (CudaDeviceCountCache::isAvailable())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(Available)");
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(Not Available)");
    }
#endif

    // Add Color button
    if (ImGui::Button("Add Color...", ImVec2(itemWidth, 0)))
    {
        pickerTargetIndex.store(-1); // -1 = add new
        isColorPickerActive.store(true);
    }

    if (isColorPickerActive.load())
    {
        ThemeText("Click on the video preview to pick a color", theme.text.warning);
    }

    // Get color list (thread-safe copy)
    std::vector<ChromakeyColor> colors;
    {
        const juce::ScopedLock lock(colorListLock);
        colors = selectedColors;
    }

    // Display color list (ColorTracker-style: swatch + tolerance + invert + remove)
    ThemeText("Selected Colors:", theme.text.section_header);

    for (size_t i = 0; i < colors.size(); ++i)
    {
        const auto& color = colors[i];
        ImGui::PushID((int)i);

        // Color swatch (click to repick)
        ImVec4 imc(
            color.displayColour.getFloatRed(),
            color.displayColour.getFloatGreen(),
            color.displayColour.getFloatBlue(),
            1.0f);
        if (ImGui::ColorButton(
                ("##swatch" + juce::String((int)i)).toRawUTF8(),
                imc,
                ImGuiColorEditFlags_NoTooltip,
                ImVec2(20, 20)))
        {
            pickerTargetIndex.store((int)i);
            isColorPickerActive.store(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to repick this color");

        ImGui::SameLine();

        // Single tolerance slider
        ImGui::SetNextItemWidth(120.0f);
        float tol = color.tolerance;
        if (ImGui::SliderFloat(
                ("Tol##" + juce::String((int)i)).toRawUTF8(), &tol, 0.1f, 5.0f, "%.2fx"))
        {
            setColorTolerance((int)i, tol);
            onModificationEnded();
        }
        // Scroll wheel support for tolerance slider
        if (ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const float step = 0.1f;
                const float delta = wheel > 0.0f ? step : -step;
                const float newTol = juce::jlimit(0.1f, 5.0f, tol + delta);
                if (std::abs(newTol - tol) > 0.01f)
                {
                    setColorTolerance((int)i, newTol);
                    onModificationEnded();
                }
            }
        }

        ImGui::SameLine();

        // Invert checkbox
        bool inv = color.inverted;
        if (ImGui::Checkbox(("Inv##" + juce::String((int)i)).toRawUTF8(), &inv))
        {
            setColorInverted((int)i, inv);
            onModificationEnded();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Invert: keep this color, remove everything else");

        ImGui::SameLine();

        // Remove button
        if (ImGui::SmallButton(("X##" + juce::String((int)i)).toRawUTF8()))
        {
            removeColor((int)i);
            onModificationEnded();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove this color");

        ImGui::PopID();
    }

    // Spill suppression
    bool        spillMod = isParamModulated("spillSuppression");
    const float spillDefault = spillSuppressionParam ? spillSuppressionParam->load() : 0.5f;
    float spill = spillMod ? getLiveParamValue("spillSuppression", spillDefault) : spillDefault;
    if (spillMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Spill Suppression", &spill, 0.0f, 1.0f))
    {
        if (!spillMod)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                    apvts.getParameter("spillSuppression")))
                *p = spill;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !spillMod)
        onModificationEnded();
    if (!spillMod)
        adjustParamOnWheel(apvts.getParameter("spillSuppression"), "spillSuppression", spill);
    if (spillMod)
        ImGui::EndDisabled();

    // Feather amount
    bool        featherMod = isParamModulated("featherAmount");
    const float featherDefault = featherAmountParam ? featherAmountParam->load() : 2.0f;
    float       feather =
        featherMod ? getLiveParamValue("featherAmount", featherDefault) : featherDefault;
    if (featherMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Feather Amount", &feather, 0.0f, 20.0f))
    {
        if (!featherMod)
        {
            if (auto* p =
                    dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("featherAmount")))
                *p = feather;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !featherMod)
        onModificationEnded();
    if (!featherMod)
        adjustParamOnWheel(apvts.getParameter("featherAmount"), "featherAmount", feather);
    if (featherMod)
        ImGui::EndDisabled();

    // Zoom buttons
    bool zoomModulated = isParamModulated("zoomLevel");
    int  level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    float      buttonWidth = (itemWidth / 2.0f) - 4.0f;
    const bool atMin = (level <= 0);
    const bool atMax = (level >= 2);

    if (zoomModulated)
        ImGui::BeginDisabled();

    // Store hover state for scroll wheel support
    bool anyButtonHovered = false;

    if (atMin)
        ImGui::BeginDisabled();
    if (ImGui::Button("-", ImVec2(buttonWidth, 0)))
    {
        int newLevel = juce::jmax(0, level - 1);
        if (auto* p = apvts.getParameter("zoomLevel"))
            p->setValueNotifyingHost((float)newLevel / 2.0f);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        anyButtonHovered = true;
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
    if (ImGui::IsItemHovered())
        anyButtonHovered = true;
    if (atMax)
        ImGui::EndDisabled();

    // Scroll wheel support for zoom level (works when hovering either button)
    if (!zoomModulated && anyButtonHovered)
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

    // Check connection status dynamically
    juce::uint32 connectedSourceId = 0;
    bool         hasConnection = false;

    if (parentSynth != nullptr)
    {
        juce::uint32 myLogicalId = storedLogicalId;
        if (myLogicalId == 0)
        {
            for (const auto& info : parentSynth->getModulesInfo())
            {
                if (parentSynth->getModuleForLogical(info.first) == this)
                {
                    myLogicalId = info.first;
                    break;
                }
            }
        }

        if (myLogicalId != 0)
        {
            auto snapshot = parentSynth->getConnectionSnapshot();
            if (snapshot && !snapshot->empty())
            {
                for (const auto& conn : *snapshot)
                {
                    if (conn.dstLogicalId == myLogicalId && conn.dstChan == 0)
                    {
                        connectedSourceId = conn.srcLogicalId;
                        hasConnection = true;
                        break;
                    }
                }
            }
        }
    }

    // Also check audio pin input
    juce::uint32 pinSourceId = currentSourceId.load();
    if (pinSourceId != 0)
    {
        connectedSourceId = pinSourceId;
        hasConnection = true;
    }

    // Display connection status
    if (hasConnection)
    {
        ThemeText(
            juce::String::formatted("Input: Connected (ID: %d)", (int)connectedSourceId)
                .toRawUTF8(),
            theme.text.section_header);
    }
    else
    {
        ThemeText("Input: Not Connected", theme.text.section_header);
    }

    ThemeText(
        juce::String::formatted("Output ID: %d", (int)getLogicalId()).toRawUTF8(),
        theme.text.section_header);
    ThemeText(
        juce::String::formatted("Alpha ID: %d", (int)getSecondaryLogicalId()).toRawUTF8(),
        theme.text.section_header);

    // Video preview with ColorTracker-style hover and scroll-wheel radius
    if (hasConnection)
    {
        juce::Image frame = getLatestFrame();
        if (!frame.isNull())
        {
            // Use static map for texture management (per-module-instance textures)
            static std::unordered_map<int, std::unique_ptr<juce::OpenGLTexture>> localTextures;
            static std::map<int, int> hoverRadiusByNode; // logicalId -> radius

            juce::uint32 nodeId = getLogicalId();
            if (localTextures.find((int)nodeId) == localTextures.end())
                localTextures[(int)nodeId] = std::make_unique<juce::OpenGLTexture>();

            // Initialize hover radius if not set
            int& rad = hoverRadiusByNode[(int)nodeId];
            if (rad <= 0)
                rad = 2;

            auto* texture = localTextures[(int)nodeId].get();
            texture->loadImage(frame);

            if (texture->getTextureID() != 0)
            {
                float  ar = (float)frame.getHeight() / juce::jmax(1.0f, (float)frame.getWidth());
                ImVec2 size(itemWidth, itemWidth * ar);
                ImGui::Image(
                    (void*)(intptr_t)texture->getTextureID(), size, ImVec2(0, 1), ImVec2(1, 0));

                // Get image screen coordinates and size for interaction
                ImVec2 imageRectMin = ImGui::GetItemRectMin();
                ImVec2 imageRectMax = ImGui::GetItemRectMax();
                ImVec2 imageSize = ImGui::GetItemRectSize();

                // Use InvisibleButton to capture mouse input
                ImGui::SetCursorScreenPos(imageRectMin);
                ImGui::InvisibleButton("##preview_interaction", imageSize);

                ImVec2 mousePos = ImGui::GetMousePos();

                // Handle color picker clicks
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    float mouseX = (mousePos.x - imageRectMin.x) / imageSize.x;
                    float mouseY = (mousePos.y - imageRectMin.y) / imageSize.y;

                    int frameX = juce::roundToInt(mouseX * frame.getWidth());
                    int frameY = juce::roundToInt(mouseY * frame.getHeight());

                    addColorAt(frameX, frameY, rad);
                    onModificationEnded();
                }

                // Hover preview with scroll-wheel radius (ColorTracker-style)
                if (ImGui::IsItemHovered())
                {
                    // Update radius by mouse wheel
                    float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.0f)
                    {
                        rad += (wheel > 0) ? 1 : -1;
                        rad = juce::jlimit(1, 30, rad); // (2*rad+1)^2 window, max 61x61
                    }

                    // Normalize mouse position
                    float nx = (mousePos.x - imageRectMin.x) / imageSize.x;
                    float ny = (mousePos.y - imageRectMin.y) / imageSize.y;
                    nx = juce::jlimit(0.0f, 1.0f, nx);
                    ny = juce::jlimit(0.0f, 1.0f, ny);
                    int cx = (int)juce::jlimit(
                        0.0f, (float)frame.getWidth() - 1.0f, nx * (float)frame.getWidth());
                    int cy = (int)juce::jlimit(
                        0.0f, (float)frame.getHeight() - 1.0f, ny * (float)frame.getHeight());

                    // Sample ROI from juce::Image for median color preview
                    std::vector<int> vr, vg, vb;
                    vr.reserve((2 * rad + 1) * (2 * rad + 1));
                    vg.reserve(vr.capacity());
                    vb.reserve(vr.capacity());

                    juce::Image::BitmapData bd(frame, juce::Image::BitmapData::readOnly);
                    auto                    clampi = [](int v, int lo, int hi) {
                        return (v < lo) ? lo : (v > hi ? hi : v);
                    };
                    for (int y = cy - rad; y <= cy + rad; ++y)
                    {
                        int                    yy = clampi(y, 0, frame.getHeight() - 1);
                        const juce::PixelARGB* row =
                            (const juce::PixelARGB*)(bd.getLinePointer(yy));
                        for (int x = cx - rad; x <= cx + rad; ++x)
                        {
                            int                    xx = clampi(x, 0, frame.getWidth() - 1);
                            const juce::PixelARGB& p = row[xx];
                            vr.push_back(p.getRed());
                            vg.push_back(p.getGreen());
                            vb.push_back(p.getBlue());
                        }
                    }
                    auto median = [](std::vector<int>& v) {
                        if (v.empty())
                            return 0;
                        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                        return v[v.size() / 2];
                    };
                    int          mr = median(vr), mg = median(vg), mb = median(vb);
                    juce::Colour mc((juce::uint8)mr, (juce::uint8)mg, (juce::uint8)mb);
                    float        h = mc.getHue(), s = mc.getSaturation(), b2 = mc.getBrightness();

                    // Tooltip with swatch and HSV values
                    ImGui::BeginTooltip();
                    ImGui::Text("(%d,%d) rad=%d", cx, cy, rad);
                    ImGui::ColorButton(
                        "##hoverSwatch",
                        ImVec4(mc.getFloatRed(), mc.getFloatGreen(), mc.getFloatBlue(), 1.0f),
                        0,
                        ImVec2(22, 22));
                    ImGui::SameLine();
                    ImGui::Text(
                        "RGB %d,%d,%d\nHSV %d,%d,%d",
                        mr,
                        mg,
                        mb,
                        (int)(h * 180.0f),
                        (int)(s * 255.0f),
                        (int)(b2 * 255.0f));
                    ImGui::TextDisabled("Scroll to adjust radius\nClick to pick color");
                    ImGui::EndTooltip();

                    // Text under image
                    ImGui::TextDisabled(
                        "Hover RGB %d,%d,%d  HSV %d,%d,%d  rad=%d",
                        mr,
                        mg,
                        mb,
                        (int)(h * 180.0f),
                        (int)(s * 255.0f),
                        (int)(b2 * 255.0f),
                        rad);

                    // Draw circle showing sample radius
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    float       radiusPx = (float)rad / (float)frame.getWidth() * imageSize.x;
                    ImU32       circleColor =
                        ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 0.0f, 0.8f));
                    drawList->AddCircle(mousePos, radiusPx, circleColor, 0, 2.0f);
                }
            }
        }
        else
        {
            ThemeText("Waiting for video input...", theme.text.warning);
        }
    }
    else
    {
        ThemeText("Connect a video source to the input", theme.text.warning);
    }

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();
}

void ChromakeyModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioInputPin("Source In", 0);
    helpers.drawAudioOutputPin("RGBA Video", 0);
    helpers.drawAudioOutputPin("Mask", 1);
}
#endif
