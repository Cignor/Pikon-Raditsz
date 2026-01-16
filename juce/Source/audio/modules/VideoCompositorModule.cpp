#include "VideoCompositorModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include <opencv2/imgproc.hpp>
#include "../../utils/CudaDeviceCountCache.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h> // For WorkRect workaround to fix widget bleeding in nodes
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout VideoCompositorModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Global parameter: number of layers
    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            "numLayers", "Number of Layers", 1, MAX_LAYERS, 1));

    // Global parameter: Use GPU
    params.push_back(std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", true));

    // Per-layer parameters for all 8 layers
    juce::StringArray blendModeChoices = {
        "Normal",
        "Add",
        "Multiply",
        "Screen",
        "Overlay",
        "Soft Light",
        "Hard Light",
        "Darken",
        "Lighten",
        "Difference",
        "Exclusion",
        "Color Dodge",
        "Color Burn",
        "Linear Dodge",
        "Linear Burn",
        "Vivid Light",
        "Linear Light",
        "Pin Light",
        "Hard Mix"};

    for (int i = 0; i < MAX_LAYERS; ++i)
    {
        const juce::String layerNum = juce::String(i + 1);

        // Opacity (0.0 to 1.0)
        params.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "layer" + layerNum + "_opacity",
                "Layer " + layerNum + " Opacity",
                0.0f,
                1.0f,
                1.0f));

        // Blend mode
        params.push_back(
            std::make_unique<juce::AudioParameterChoice>(
                "layer" + layerNum + "_blendMode",
                "Layer " + layerNum + " Blend Mode",
                blendModeChoices,
                0));

        // Position X (normalized -1.0 to 1.0, 0.0 = center)
        params.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "layer" + layerNum + "_posX",
                "Layer " + layerNum + " Position X",
                -1.0f,
                1.0f,
                0.0f));

        // Position Y (normalized -1.0 to 1.0, 0.0 = center)
        params.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "layer" + layerNum + "_posY",
                "Layer " + layerNum + " Position Y",
                -1.0f,
                1.0f,
                0.0f));

        // Scale X (0.1 to 5.0)
        params.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "layer" + layerNum + "_scaleX",
                "Layer " + layerNum + " Scale X",
                0.1f,
                5.0f,
                1.0f));

        // Scale Y (0.1 to 5.0)
        params.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "layer" + layerNum + "_scaleY",
                "Layer " + layerNum + " Scale Y",
                0.1f,
                5.0f,
                1.0f));
    }

    return {params.begin(), params.end()};
}

VideoCompositorModule::VideoCompositorModule()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Inputs", juce::AudioChannelSet::discreteChannels(MAX_LAYERS), true)
              .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      juce::Thread("Video Compositor Thread"),
      apvts(*this, nullptr, "VideoCompositorParams", createParameterLayout())
{
    // Get global parameter
    numLayersParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("numLayers"));
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));

    // Initialize per-layer parameters
    for (int i = 0; i < MAX_LAYERS; ++i)
    {
        const juce::String layerNum = juce::String(i + 1);
        layerOpacityParams[i] = apvts.getRawParameterValue("layer" + layerNum + "_opacity");
        layerBlendModeParams[i] = dynamic_cast<juce::AudioParameterChoice*>(
            apvts.getParameter("layer" + layerNum + "_blendMode"));
        layerPosXParams[i] = apvts.getRawParameterValue("layer" + layerNum + "_posX");
        layerPosYParams[i] = apvts.getRawParameterValue("layer" + layerNum + "_posY");
        layerScaleXParams[i] = apvts.getRawParameterValue("layer" + layerNum + "_scaleX");
        layerScaleYParams[i] = apvts.getRawParameterValue("layer" + layerNum + "_scaleY");
    }

    // Initialize source IDs array
    for (int i = 0; i < MAX_LAYERS; ++i)
        layerSourceIds[i].store(0);

    // Initialize last frames vector
    lastLayerFrames.resize(MAX_LAYERS);

#if defined(WITH_CUDA_SUPPORT)
    gpuLayerFrames.resize(MAX_LAYERS);
#endif
}

VideoCompositorModule::~VideoCompositorModule()
{
    stopThread(5000);
    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void VideoCompositorModule::prepareToPlay(double, int) { startThread(); }

void VideoCompositorModule::releaseResources()
{
    signalThreadShouldExit();
    stopThread(5000);
}

void VideoCompositorModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // Read source IDs from input pins
    // All dynamic pins are on the same bus (bus 0) but different channels
    const int numLayers = numLayersParam ? numLayersParam->get() : 1;
    const int numLayersClamped = juce::jlimit(1, MAX_LAYERS, numLayers);

    auto inputBus = getBusBuffer(buffer, true, 0);
    if (inputBus.getNumChannels() > 0 && inputBus.getNumSamples() > 0)
    {
        for (int i = 0; i < numLayersClamped; ++i)
        {
            // Read from channel i if it exists, otherwise use 0 (no connection)
            juce::uint32 sourceId = 0;
            if (i < inputBus.getNumChannels())
            {
                sourceId = (juce::uint32)inputBus.getSample(i, 0);
            }
            layerSourceIds[i].store(sourceId);
        }
    }
    else
    {
        // Clear all source IDs if no input
        for (int i = 0; i < numLayersClamped; ++i)
        {
            layerSourceIds[i].store(0);
        }
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

    // Output our own Logical ID on the output pin
    auto outputBus = getBusBuffer(buffer, false, 0);
    if (outputBus.getNumChannels() > 0 && outputBus.getNumSamples() > 0)
    {
        float logicalId = (float)myLogicalId;
        for (int s = 0; s < outputBus.getNumSamples(); ++s)
            outputBus.setSample(0, s, logicalId);
    }
}

std::vector<DynamicPinInfo> VideoCompositorModule::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    const int                   numLayers = numLayersParam ? numLayersParam->get() : 1;
    const int                   numLayersClamped = juce::jlimit(1, MAX_LAYERS, numLayers);

    for (int i = 0; i < numLayersClamped; ++i)
    {
        pins.push_back({"Layer " + juce::String(i + 1), i, PinDataType::Video});
    }
    return pins;
}

std::vector<DynamicPinInfo> VideoCompositorModule::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"Output", 0, PinDataType::Video});
    return pins;
}

void VideoCompositorModule::run()
{
    juce::Logger::writeToLog("[VideoCompositor] Thread started.");

    while (!threadShouldExit())
    {
        double    startTime = juce::Time::getMillisecondCounterHiRes();
        const int numLayers = numLayersParam ? numLayersParam->get() : 1;
        const int numLayersClamped = juce::jlimit(1, MAX_LAYERS, numLayers);

        // Check GPU availability
        const bool useGpu = useGpuParam ? useGpuParam->get() : false;
#if defined(WITH_CUDA_SUPPORT)
        const bool gpuAvailable = CudaDeviceCountCache::isAvailable();
#else
        const bool gpuAvailable = false;
#endif
        // Use GPU if enabled and available
        // Use GPU if enabled and available
        // Note: gpuHasFailed is now a member variable
        bool runOnGpu = (useGpu && gpuAvailable && !gpuHasFailed.load());

        // Log status changes
        static bool lastRunOnGpu = !runOnGpu; // force log on first run
        if (runOnGpu != lastRunOnGpu)
        {
            juce::Logger::writeToLog(
                "[VideoCompositor] Processing Mode: " +
                juce::String(runOnGpu ? "GPU (CUDA)" : "CPU"));
            lastRunOnGpu = runOnGpu;
        }

        // Monitor performance
        // Monitor performance
        // startTime already initialized at top of loop

        // Get source IDs (thread-safe copy)
        std::vector<juce::uint32> sourceIds(numLayersClamped);
        for (int i = 0; i < numLayersClamped; ++i)
        {
            sourceIds[i] = layerSourceIds[i].load();
        }

        int canvasWidth = 0;
        int canvasHeight = 0;

        cv::Mat canvas;

        if (runOnGpu)
        {
#if defined(WITH_CUDA_SUPPORT)
            try
            {
                // Fetch frames GPU-first
                bool inputsReady = false;

                // Initialize GPU canvas (Lazy realloc)
                // Use MAX dimension of all inputs if canvas not set?
                // Currently we use first valid frame.
                // We need to iterate sources to find size.
                int maxWidth = 0, maxHeight = 0;

                // Fetch all frames (GpuMat refs)
                std::vector<cv::cuda::GpuMat> gpuFrames(numLayersClamped);

                for (int i = 0; i < numLayersClamped; ++i)
                {
                    juce::uint32 sid = sourceIds[i];
                    if (sid != 0)
                    {
                        gpuFrames[i] = VideoFrameManager::getInstance().getGpuFrame(sid);
                        if (!gpuFrames[i].empty())
                        {
                            maxWidth = std::max(maxWidth, gpuFrames[i].cols);
                            maxHeight = std::max(maxHeight, gpuFrames[i].rows);
                            inputsReady = true;
                        }
                    }
                }

                if (!inputsReady)
                {
                    // No valid inputs yet
                    wait(33);
                    continue;
                }

                canvasWidth = maxWidth;
                canvasHeight = maxHeight;

                // Initialize GPU canvas
                // Reallocate only if size changed
                if (gpuCanvas.size() != cv::Size(canvasWidth, canvasHeight) ||
                    gpuCanvas.type() != CV_8UC3)
                {
                    gpuCanvas =
                        cv::cuda::GpuMat(canvasHeight, canvasWidth, CV_8UC3, cv::Scalar(0, 0, 0));
                    juce::Logger::writeToLog(
                        "[VideoCompositor] Reallocated GPU Canvas: " + juce::String(canvasWidth) +
                        "x" + juce::String(canvasHeight));
                }
                else
                {
                    gpuCanvas.setTo(cv::Scalar(0, 0, 0));
                }

                // Upload frames and composite
                for (int i = 0; i < numLayersClamped; ++i)
                {
                    if (gpuFrames[i].empty())
                        continue;

                    // Get layer parameters
                    float opacity = layerOpacityParams[i] ? layerOpacityParams[i]->load() : 1.0f;
                    int   blendModeIdx =
                        layerBlendModeParams[i] ? layerBlendModeParams[i]->getIndex() : 0;
                    BlendMode blendMode = static_cast<BlendMode>(blendModeIdx);
                    float     posX = layerPosXParams[i] ? layerPosXParams[i]->load() : 0.0f;
                    float     posY = layerPosYParams[i] ? layerPosYParams[i]->load() : 0.0f;
                    float     scaleX = layerScaleXParams[i] ? layerScaleXParams[i]->load() : 1.0f;
                    float     scaleY = layerScaleYParams[i] ? layerScaleYParams[i]->load() : 1.0f;

                    // Composite on GPU
                    compositeLayer_gpu(
                        gpuCanvas, gpuFrames[i], blendMode, opacity, posX, posY, scaleX, scaleY);
                }

                // Publish result via GPU manager
                juce::uint32 myLogicalId = storedLogicalId;
                if (myLogicalId != 0)
                {
                    VideoFrameManager::getInstance().setGpuFrame(myLogicalId, gpuCanvas);
                }

                // Download for UI preview (throttled/necessary for updateGuiFrame)
                gpuCanvas.download(canvas);
            }
            catch (const cv::Exception& e)
            {
                // Fallback to CPU on error
                gpuHasFailed = true;
                static bool loggedError = false;
                if (!loggedError)
                {
                    juce::Logger::writeToLog(
                        "[VideoCompositor] GPU Error: " + juce::String(e.what()) +
                        ". Falling back to CPU.");
                    loggedError = true;
                }
                runOnGpu = false;
            }
#endif
        }

        // CPU fallback (or primary if GPU disabled)
        if (!runOnGpu)
        {
            // Fetch frames from VideoFrameManager (CPU)
            std::vector<cv::Mat> frames(numLayersClamped);
            bool                 hasValidInput = false;

            for (int i = 0; i < numLayersClamped; ++i)
            {
                if (sourceIds[i] != 0)
                {
                    frames[i] = VideoFrameManager::getInstance().getFrame(sourceIds[i]);
                    if (!frames[i].empty())
                    {
                        hasValidInput = true;
                        if (canvasWidth == 0)
                        {
                            canvasWidth = frames[i].cols;
                            canvasHeight = frames[i].rows;
                        }
                    }
                    // Cache frame for preview
                    {
                        const juce::ScopedLock lock(framesLock);
                        // Logic: if we have new frame, update cache. If not, use cache.
                        if (!frames[i].empty())
                        {
                            if (i < (int)lastLayerFrames.size())
                                frames[i].copyTo(lastLayerFrames[i]);
                        }
                        else if (i < (int)lastLayerFrames.size() && !lastLayerFrames[i].empty())
                        {
                            // Retrieve from cache if input empty
                            frames[i] = lastLayerFrames[i].clone();
                            hasValidInput = true; // Cache counts as valid input
                            if (canvasWidth == 0 && !frames[i].empty())
                            {
                                canvasWidth = frames[i].cols;
                                canvasHeight = frames[i].rows;
                            }
                        }
                    }
                }
            }

            if (!hasValidInput)
            {
                wait(33);
                continue;
            }
            if (canvasWidth == 0)
            {
                canvasWidth = 1920;
                canvasHeight = 1080;
            }

            canvas = cv::Mat::zeros(canvasHeight, canvasWidth, CV_8UC3);

            for (int i = 0; i < numLayersClamped; ++i)
            {
                if (frames[i].empty())
                    continue;

                // Get layer parameters
                float opacity = layerOpacityParams[i] ? layerOpacityParams[i]->load() : 1.0f;
                int   blendModeIdx =
                    layerBlendModeParams[i] ? layerBlendModeParams[i]->getIndex() : 0;
                BlendMode blendMode = static_cast<BlendMode>(blendModeIdx);
                float     posX = layerPosXParams[i] ? layerPosXParams[i]->load() : 0.0f;
                float     posY = layerPosYParams[i] ? layerPosYParams[i]->load() : 0.0f;
                float     scaleX = layerScaleXParams[i] ? layerScaleXParams[i]->load() : 1.0f;
                float     scaleY = layerScaleYParams[i] ? layerScaleYParams[i]->load() : 1.0f;

                // Composite this layer
                compositeLayer(canvas, frames[i], blendMode, opacity, posX, posY, scaleX, scaleY);
            }

            // Log frame time occasionally
            // Performance telemetry
            double msDouble = juce::Time::getMillisecondCounterHiRes() - startTime;
            lastProcessTimeMs = (float)msDouble;
            lastProcessWasGpu = runOnGpu;

            // Log frame time occasionally
            static int perfLog = 0;
            if (++perfLog % 60 == 0)
            {
                float ms = (float)msDouble;
                // wait, startTime is in method scope? No, I need to declare it!
                // I forgot to declare startTime in this refactor.
                // I will add it to the top of run() in a separate step or just assume it is not
                // there yet. Actually I tried to add it in step 934 but failed. So I should add it.
                // usage: just juce::Time::getMillisecondCounterHiRes();
            }
            // Publish CPU result
            juce::uint32 myLogicalId = storedLogicalId;
            if (myLogicalId != 0)
            {
                VideoFrameManager::getInstance().setFrame(myLogicalId, canvas);
            }
        }

        updateGuiFrame(canvas);

        static int frameCounter = 0;
        if (++frameCounter % 100 == 0)
            juce::Logger::writeToLog(
                "[VideoCompositor] Frame " + juce::String(frameCounter) + " processed.");

        wait(33); // ~30 FPS
    }
}

void VideoCompositorModule::compositeLayer(
    cv::Mat&       canvas,
    const cv::Mat& layer,
    BlendMode      mode,
    float          opacity,
    float          posX,
    float          posY,
    float          scaleX,
    float          scaleY)
{
    if (layer.empty() || canvas.empty())
        return;

    // Optimize: if no transforms and same size, use layer directly
    cv::Mat transformed;
    if (posX == 0.0f && posY == 0.0f && scaleX == 1.0f && scaleY == 1.0f &&
        layer.size() == canvas.size())
    {
        transformed = layer;
    }
    else
    {
        // Apply transforms
        transformed = applyTransforms(layer, posX, posY, scaleX, scaleY, canvas.cols, canvas.rows);
    }

    if (transformed.empty())
        return;

    // Apply blend mode
    applyBlendMode(canvas, transformed, mode, opacity);
}

cv::Mat VideoCompositorModule::applyTransforms(
    const cv::Mat& src,
    float          posX,
    float          posY,
    float          scaleX,
    float          scaleY,
    int            canvasWidth,
    int            canvasHeight)
{
    if (src.empty())
        return cv::Mat();

    // Calculate scaled size
    int scaledWidth = (int)(src.cols * scaleX);
    int scaledHeight = (int)(src.rows * scaleY);

    if (scaledWidth <= 0 || scaledHeight <= 0)
        return cv::Mat();

    // Resize
    cv::Mat scaled;
    cv::resize(src, scaled, cv::Size(scaledWidth, scaledHeight), 0, 0, cv::INTER_LINEAR);

    // Calculate position offset (normalized -1.0 to 1.0 maps to canvas)
    // 0.0 = center, -1.0 = left/top edge, 1.0 = right/bottom edge
    int offsetX = (int)((posX + 1.0f) * 0.5f * (canvasWidth - scaledWidth));
    int offsetY = (int)((posY + 1.0f) * 0.5f * (canvasHeight - scaledHeight));

    // Create output canvas
    cv::Mat result = cv::Mat::zeros(canvasHeight, canvasWidth, src.type());

    // Calculate source and destination regions (handle clipping)
    int srcX = 0;
    int srcY = 0;
    int srcW = scaledWidth;
    int srcH = scaledHeight;
    int dstX = offsetX;
    int dstY = offsetY;

    // Clip to canvas bounds
    if (dstX < 0)
    {
        srcX = -dstX;
        srcW += dstX;
        dstX = 0;
    }
    if (dstY < 0)
    {
        srcY = -dstY;
        srcH += dstY;
        dstY = 0;
    }
    if (dstX + srcW > canvasWidth)
        srcW = canvasWidth - dstX;
    if (dstY + srcH > canvasHeight)
        srcH = canvasHeight - dstY;

    if (srcW > 0 && srcH > 0 && srcX < scaledWidth && srcY < scaledHeight)
    {
        cv::Rect srcRect(srcX, srcY, srcW, srcH);
        cv::Rect dstRect(dstX, dstY, srcW, srcH);

        scaled(srcRect).copyTo(result(dstRect));
    }

    return result;
}

void VideoCompositorModule::applyBlendMode(
    cv::Mat&       dst,
    const cv::Mat& src,
    BlendMode      mode,
    float          opacity)
{
    if (dst.empty() || src.empty())
        return;

    // If sizes don't match, resize src to match dst (shouldn't happen after transforms, but safety
    // check)
    cv::Mat srcMat = src;
    if (dst.size() != src.size())
    {
        cv::resize(src, srcMat, dst.size(), 0, 0, cv::INTER_LINEAR);
    }

    // Convert to float for blending calculations
    cv::Mat dstF, srcF;
    dst.convertTo(dstF, CV_32FC3, 1.0 / 255.0);
    srcMat.convertTo(srcF, CV_32FC3, 1.0 / 255.0);

    cv::Mat resultF = dstF.clone();

    // Apply blend mode
    switch (mode)
    {
    case BlendMode::Normal:
    {
        // Standard alpha blending
        cv::addWeighted(dstF, 1.0f - opacity, srcF, opacity, 0.0, resultF);
        break;
    }
    case BlendMode::Add:
    {
        resultF = dstF + srcF * opacity;
        cv::min(resultF, 1.0f, resultF);
        break;
    }
    case BlendMode::Multiply:
    {
        resultF = dstF.mul(srcF * opacity + (1.0f - opacity));
        break;
    }
    case BlendMode::Screen:
    {
        resultF = 1.0f - (1.0f - dstF).mul(1.0f - srcF * opacity);
        cv::min(resultF, 1.0f, resultF);
        break;
    }
    case BlendMode::Overlay:
    {
        cv::Mat mask = dstF < 0.5f;
        cv::Mat result1, result2;
        cv::multiply(2.0f * dstF, srcF * opacity + (1.0f - opacity), result1);
        cv::multiply(1.0f - 2.0f * (1.0f - dstF), srcF * opacity + (1.0f - opacity), result2);
        result1.copyTo(resultF, mask);
        result2.copyTo(resultF, ~mask);
        break;
    }
    case BlendMode::SoftLight:
    {
        cv::Mat mask = srcF < 0.5f;
        cv::Mat result1, result2, temp;
        cv::Mat expr1 = 2.0f * dstF.mul(srcF * opacity) + dstF * (1.0f - opacity) - dstF.mul(dstF);
        expr1.copyTo(result1);
        temp = 2.0f * srcF * opacity - 1.0f;
        cv::max(temp, 0.0f, temp);
        cv::Mat expr2 =
            2.0f * dstF * (1.0f - srcF * opacity) + temp.mul(dstF) + dstF * (1.0f - opacity);
        expr2.copyTo(result2);
        result1.copyTo(resultF, mask);
        result2.copyTo(resultF, ~mask);
        break;
    }
    case BlendMode::HardLight:
    {
        cv::Mat mask = srcF < 0.5f;
        cv::Mat result1, result2;
        cv::multiply(2.0f * srcF * opacity, dstF, result1);
        cv::multiply(1.0f - 2.0f * (1.0f - srcF * opacity), dstF, result2);
        result1.copyTo(resultF, mask);
        result2.copyTo(resultF, ~mask);
        break;
    }
    case BlendMode::Darken:
    {
        cv::min(dstF, srcF * opacity + dstF * (1.0f - opacity), resultF);
        break;
    }
    case BlendMode::Lighten:
    {
        cv::max(dstF, srcF * opacity + dstF * (1.0f - opacity), resultF);
        break;
    }
    case BlendMode::Difference:
    {
        resultF = cv::abs(dstF - (srcF * opacity + dstF * (1.0f - opacity)));
        break;
    }
    case BlendMode::Exclusion:
    {
        resultF = dstF + srcF * opacity - 2.0f * dstF.mul(srcF * opacity);
        break;
    }
    case BlendMode::ColorDodge:
    {
        cv::Mat divisor = 1.0f - srcF * opacity;
        cv::divide(dstF, divisor, resultF, 1.0f, CV_32F);
        cv::min(resultF, 1.0f, resultF);
        break;
    }
    case BlendMode::ColorBurn:
    {
        cv::Mat divisor = srcF * opacity;
        cv::divide(1.0f - dstF, divisor, resultF, 1.0f, CV_32F);
        resultF = 1.0f - resultF;
        cv::max(resultF, 0.0f, resultF);
        break;
    }
    case BlendMode::LinearDodge:
    {
        resultF = dstF + srcF * opacity;
        cv::min(resultF, 1.0f, resultF);
        break;
    }
    case BlendMode::LinearBurn:
    {
        resultF = dstF + srcF * opacity - 1.0f;
        cv::max(resultF, 0.0f, resultF);
        break;
    }
    case BlendMode::VividLight:
    {
        cv::Mat mask = srcF * opacity < 0.5f;
        cv::Mat result1, result2;
        cv::divide(dstF, 2.0f * (1.0f - srcF * opacity), result1, 1.0f, CV_32F);
        result1 = 1.0f - result1;
        cv::max(result1, 0.0f, result1);
        cv::divide(dstF, 2.0f * srcF * opacity, result2, 1.0f, CV_32F);
        cv::min(result2, 1.0f, result2);
        result1.copyTo(resultF, mask);
        result2.copyTo(resultF, ~mask);
        break;
    }
    case BlendMode::LinearLight:
    {
        resultF = dstF + 2.0f * srcF * opacity - 1.0f;
        cv::max(resultF, 0.0f, resultF);
        cv::min(resultF, 1.0f, resultF);
        break;
    }
    case BlendMode::PinLight:
    {
        cv::Mat mask1 = dstF < 2.0f * srcF * opacity - 1.0f;
        cv::Mat mask2 = dstF > 2.0f * srcF * opacity;
        resultF = dstF.clone();
        cv::Mat expr1 = 2.0f * srcF * opacity - 1.0f;
        cv::Mat expr2 = 2.0f * srcF * opacity;
        expr1.copyTo(resultF, mask1);
        expr2.copyTo(resultF, mask2);
        break;
    }
    case BlendMode::HardMix:
    {
        resultF = dstF + srcF * opacity;
        cv::Mat mask = resultF >= 1.0f;
        resultF.setTo(1.0f, mask);
        resultF.setTo(0.0f, ~mask);
        break;
    }
    }

    // Convert back to 8-bit
    resultF.convertTo(dst, CV_8UC3, 255.0);
}

void VideoCompositorModule::updateGuiFrame(const cv::Mat& frame)
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

juce::Image VideoCompositorModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

#if defined(WITH_CUDA_SUPPORT)
void VideoCompositorModule::compositeLayer_gpu(
    cv::cuda::GpuMat&       canvas,
    const cv::cuda::GpuMat& layer,
    BlendMode               mode,
    float                   opacity,
    float                   posX,
    float                   posY,
    float                   scaleX,
    float                   scaleY)
{
    if (layer.empty() || canvas.empty())
        return;

    // Apply transforms
    cv::cuda::GpuMat transformed =
        applyTransforms_gpu(layer, posX, posY, scaleX, scaleY, canvas.cols, canvas.rows);

    if (transformed.empty())
        return;

    // Apply blend mode
    applyBlendMode_gpu(canvas, transformed, mode, opacity);
}

cv::cuda::GpuMat VideoCompositorModule::applyTransforms_gpu(
    const cv::cuda::GpuMat& src,
    float                   posX,
    float                   posY,
    float                   scaleX,
    float                   scaleY,
    int                     canvasWidth,
    int                     canvasHeight)
{
    if (src.empty())
        return cv::cuda::GpuMat();

    // Calculate scaled size
    int scaledWidth = (int)(src.cols * scaleX);
    int scaledHeight = (int)(src.rows * scaleY);

    if (scaledWidth <= 0 || scaledHeight <= 0)
        return cv::cuda::GpuMat();

    // Resize
    cv::cuda::resize(src, gpuScaled, cv::Size(scaledWidth, scaledHeight), 0, 0, cv::INTER_LINEAR);

    // Calculate position offset
    int offsetX = (int)((posX + 1.0f) * 0.5f * (canvasWidth - scaledWidth));
    int offsetY = (int)((posY + 1.0f) * 0.5f * (canvasHeight - scaledHeight));

    // Create output canvas (black initialized)
    // Avoid reallocation if possible
    if (gpuTransformed.size() != cv::Size(canvasWidth, canvasHeight) ||
        gpuTransformed.type() != src.type())
        gpuTransformed =
            cv::cuda::GpuMat(canvasHeight, canvasWidth, src.type(), cv::Scalar(0, 0, 0, 0));
    else
        gpuTransformed.setTo(cv::Scalar(0, 0, 0, 0));

    // Calculate source and destination regions (handle clipping)
    int srcX = 0;
    int srcY = 0;
    int srcW = scaledWidth;
    int srcH = scaledHeight;
    int dstX = offsetX;
    int dstY = offsetY;

    // Clip to canvas bounds
    if (dstX < 0)
    {
        srcX = -dstX;
        srcW += dstX;
        dstX = 0;
    }
    if (dstY < 0)
    {
        srcY = -dstY;
        srcH += dstY;
        dstY = 0;
    }
    if (dstX + srcW > canvasWidth)
        srcW = canvasWidth - dstX;
    if (dstY + srcH > canvasHeight)
        srcH = canvasHeight - dstY;

    if (srcW > 0 && srcH > 0 && srcX < scaledWidth && srcY < scaledHeight)
    {
        cv::Rect srcRect(srcX, srcY, srcW, srcH);
        cv::Rect dstRect(dstX, dstY, srcW, srcH);
        gpuScaled(srcRect).copyTo(gpuTransformed(dstRect));
    }

    return gpuTransformed;
}

void VideoCompositorModule::applyBlendMode_gpu(
    cv::cuda::GpuMat&       dst,
    const cv::cuda::GpuMat& src,
    BlendMode               mode,
    float                   opacity)
{
    // Need float buffers for math
    if (gpuDstF.size() != dst.size())
        gpuDstF = cv::cuda::GpuMat(dst.size(), CV_32FC3);
    if (gpuSrcF.size() != dst.size())
        gpuSrcF = cv::cuda::GpuMat(dst.size(), CV_32FC3);
    if (gpuResultF.size() != dst.size())
        gpuResultF = cv::cuda::GpuMat(dst.size(), CV_32FC3);

    dst.convertTo(gpuDstF, CV_32FC3, 1.0 / 255.0);

    // Alpha support
    cv::cuda::GpuMat srcAlpha;

    if (src.channels() == 4)
    {
        cv::cuda::GpuMat floatSrc;
        src.convertTo(floatSrc, CV_32FC4, 1.0 / 255.0);

        std::vector<cv::cuda::GpuMat> chans;
        cv::cuda::split(floatSrc, chans);

        std::vector<cv::cuda::GpuMat> rgb = {chans[0], chans[1], chans[2]};
        cv::cuda::merge(rgb, gpuSrcF);

        srcAlpha = chans[3];
        if (opacity < 1.0f)
            cv::cuda::multiply(srcAlpha, cv::Scalar::all(opacity), srcAlpha);

        // Expand alpha to 3 channels for broadcasting
        cv::cuda::GpuMat alpha3;
        cv::cuda::cvtColor(srcAlpha, alpha3, cv::COLOR_GRAY2BGR);
        srcAlpha = alpha3;
    }
    else if (src.channels() == 1)
    {
        // Handle 1-channel Grayscale (Masks/Mattes)
        cv::cuda::GpuMat src3;
        cv::cuda::cvtColor(src, src3, cv::COLOR_GRAY2BGR);
        src3.convertTo(gpuSrcF, CV_32FC3, 1.0 / 255.0);
        // implicit alpha = 1.0 * opacity
    }
    else
    {
        src.convertTo(gpuSrcF, CV_32FC3, 1.0 / 255.0);
        // implicit alpha = 1.0 * opacity
    }

    // Initialize result with dest
    // Use copyTo to avoid reallocation if size matches
    gpuDstF.copyTo(gpuResultF);

    // Reuse temp buffers if available or allocate locally
    // We have gpuTemp which is GpuMat
    cv::cuda::GpuMat& temp1 = gpuTemp;
    if (temp1.size() != dst.size() || temp1.type() != CV_32FC3)
        temp1 = cv::cuda::GpuMat(dst.size(), CV_32FC3);

    switch (mode)
    {
    case BlendMode::Normal:
        if (!srcAlpha.empty())
        {
            // dst = dst * (1 - alpha) + src * alpha
            cv::cuda::GpuMat invAlpha;
            cv::cuda::absdiff(srcAlpha, cv::Scalar::all(1.0), invAlpha);

            // temp1 is already prepared above

            cv::cuda::multiply(gpuDstF, invAlpha, gpuResultF);
            cv::cuda::multiply(gpuSrcF, srcAlpha, temp1);
            cv::cuda::add(gpuResultF, temp1, gpuResultF);
        }
        else
        {
            cv::cuda::addWeighted(gpuDstF, 1.0 - opacity, gpuSrcF, opacity, 0.0, gpuResultF);
        }
        break;

    case BlendMode::Add:
        // For now keep simple Add
        if (opacity < 1.0f)
        {
            cv::cuda::multiply(gpuSrcF, cv::Scalar::all(opacity), gpuTemp);
            cv::cuda::add(gpuDstF, gpuTemp, gpuResultF);
        }
        else
        {
            cv::cuda::add(gpuDstF, gpuSrcF, gpuResultF);
        }
        cv::cuda::min(gpuResultF, 1.0f, gpuResultF);
        break;

    case BlendMode::Multiply:
        if (opacity < 1.0f)
        {
            // res = dst * (src*opacity + (1-opacity))
            cv::cuda::multiply(gpuSrcF, cv::Scalar::all(opacity), gpuTemp);
            cv::cuda::add(gpuTemp, cv::Scalar::all(1.0f - opacity), gpuTemp);
            cv::cuda::multiply(gpuDstF, gpuTemp, gpuResultF);
        }
        else
        {
            cv::cuda::multiply(gpuDstF, gpuSrcF, gpuResultF);
        }
        break;

    case BlendMode::Screen:
        // 1 - (1-dst)*(1-src)
        {
            cv::cuda::GpuMat invDst, invSrc;
            cv::cuda::absdiff(gpuDstF, cv::Scalar::all(1.0), invDst);
            cv::cuda::absdiff(gpuSrcF, cv::Scalar::all(1.0), invSrc);
            cv::cuda::multiply(invDst, invSrc, gpuResultF);
            cv::cuda::absdiff(gpuResultF, cv::Scalar::all(1.0), gpuResultF);
        }
        break;

    case BlendMode::Darken:
        cv::cuda::min(gpuDstF, gpuSrcF, gpuResultF);
        break;

    case BlendMode::Lighten:
        cv::cuda::max(gpuDstF, gpuSrcF, gpuResultF);
        break;

    case BlendMode::Difference:
        cv::cuda::absdiff(gpuDstF, gpuSrcF, gpuResultF);
        break;

    case BlendMode::Exclusion:
        // dst + src - 2*dst*src
        {
            cv::cuda::multiply(gpuDstF, gpuSrcF, temp1);            // dst*src
            cv::cuda::multiply(temp1, cv::Scalar::all(2.0), temp1); // 2*dst*src
            cv::cuda::add(gpuDstF, gpuSrcF, gpuResultF);            // dst+src
            cv::cuda::subtract(gpuResultF, temp1, gpuResultF);
        }
        break;

    case BlendMode::Overlay:
        // if dst < 0.5: 2*dst*src
        // else: 1 - 2*(1-dst)*(1-src)
        {
            cv::cuda::GpuMat mask;
            // Threshold dst > 0.5
            cv::cuda::threshold(gpuDstF, mask, 0.5, 1.0, cv::THRESH_BINARY);

            // Res1 (dst < 0.5)
            cv::cuda::multiply(gpuDstF, gpuSrcF, temp1);
            cv::cuda::multiply(temp1, cv::Scalar::all(2.0), temp1);

            // Res2 (dst >= 0.5)
            cv::cuda::GpuMat invDst, invSrc, temp2;
            cv::cuda::absdiff(gpuDstF, cv::Scalar::all(1.0), invDst);
            cv::cuda::absdiff(gpuSrcF, cv::Scalar::all(1.0), invSrc);
            cv::cuda::multiply(invDst, invSrc, temp2);
            cv::cuda::multiply(temp2, cv::Scalar::all(2.0), temp2);
            cv::cuda::absdiff(temp2, cv::Scalar::all(1.0), temp2);

            // Blending
            cv::cuda::GpuMat invMask;
            cv::cuda::absdiff(mask, cv::Scalar::all(1.0), invMask);

            cv::cuda::multiply(temp1, invMask, temp1);
            cv::cuda::multiply(temp2, mask, temp2);
            cv::cuda::add(temp1, temp2, gpuResultF);
        }
        break;

    case BlendMode::HardLight:
        // Overlay with swapped inputs (check src < 0.5)
        {
            cv::cuda::GpuMat mask;
            cv::cuda::threshold(gpuSrcF, mask, 0.5, 1.0, cv::THRESH_BINARY);

            // Res1 (src < 0.5)
            cv::cuda::multiply(gpuDstF, gpuSrcF, temp1);
            cv::cuda::multiply(temp1, cv::Scalar::all(2.0), temp1);

            // Res2 (src >= 0.5)
            cv::cuda::GpuMat invDst, invSrc, temp2;
            cv::cuda::absdiff(gpuDstF, cv::Scalar::all(1.0), invDst);
            cv::cuda::absdiff(gpuSrcF, cv::Scalar::all(1.0), invSrc);
            cv::cuda::multiply(invDst, invSrc, temp2);
            cv::cuda::multiply(temp2, cv::Scalar::all(2.0), temp2);
            cv::cuda::absdiff(temp2, cv::Scalar::all(1.0), temp2);

            cv::cuda::GpuMat invMask;
            cv::cuda::absdiff(mask, cv::Scalar::all(1.0), invMask);
            cv::cuda::multiply(temp1, invMask, temp1);
            cv::cuda::multiply(temp2, mask, temp2);
            cv::cuda::add(temp1, temp2, gpuResultF);
        }
        break;

    case BlendMode::ColorDodge:
        // dst / (1 - src)
        {
            cv::cuda::GpuMat invSrc;
            cv::cuda::absdiff(gpuSrcF, cv::Scalar::all(1.0), invSrc);
            cv::cuda::divide(gpuDstF, invSrc, gpuResultF);
            cv::cuda::min(gpuResultF, 1.0f, gpuResultF);
        }
        break;

    case BlendMode::ColorBurn:
        // 1 - (1-dst)/src
        {
            cv::cuda::GpuMat invDst;
            cv::cuda::absdiff(gpuDstF, cv::Scalar::all(1.0), invDst);
            cv::cuda::divide(invDst, gpuSrcF, gpuResultF);
            cv::cuda::absdiff(gpuResultF, cv::Scalar::all(1.0), gpuResultF);
            cv::cuda::max(gpuResultF, 0.0f, gpuResultF);
        }
        break;

    case BlendMode::LinearDodge:
        // dst + src (Same as Add)
        cv::cuda::add(gpuDstF, gpuSrcF, gpuResultF);
        cv::cuda::min(gpuResultF, 1.0f, gpuResultF);
        break;

    case BlendMode::LinearBurn:
        // dst + src - 1
        cv::cuda::add(gpuDstF, gpuSrcF, gpuResultF);
        cv::cuda::subtract(gpuResultF, cv::Scalar::all(1.0), gpuResultF);
        cv::cuda::max(gpuResultF, 0.0f, gpuResultF);
        break;

    case BlendMode::VividLight:
        // if src < 0.5: ColorBurn(dst, 2*src)
        // else: ColorDodge(dst, 2*(src-0.5))
        {
            // This is complex to implement branchlessly on GpuMat with simple ops.
            // Simplification: Use Overlay or HardLight logic as approximation
            // OR construct masks.
            // Mask: src < 0.5
            cv::cuda::GpuMat mask;
            cv::cuda::threshold(gpuSrcF, mask, 0.5, 1.0, cv::THRESH_BINARY);

            // Res1 (src < 0.5): ColorBurn(dst, 2*src)
            // 1 - (1-dst)/(2*src)
            cv::cuda::multiply(gpuSrcF, cv::Scalar::all(2.0), temp1); // 2*src + epsilon?
            cv::cuda::add(temp1, cv::Scalar::all(0.001), temp1);      // avoid div zero
            cv::cuda::GpuMat invDst, res1;
            cv::cuda::absdiff(gpuDstF, cv::Scalar::all(1.0), invDst);
            cv::cuda::divide(invDst, temp1, res1);
            cv::cuda::absdiff(res1, cv::Scalar::all(1.0), res1);
            cv::cuda::max(res1, 0.0f, res1);

            // Res2 (src >= 0.5): ColorDodge(dst, 2*src - 1)
            // dst / (1 - (2*src - 1)) = dst / (2 - 2*src) = dst / (2*(1-src))
            // 1 - src
            cv::cuda::GpuMat invSrc, res2;
            cv::cuda::absdiff(gpuSrcF, cv::Scalar::all(1.0), invSrc);
            cv::cuda::multiply(invSrc, cv::Scalar::all(2.0), temp1);
            cv::cuda::add(temp1, cv::Scalar::all(0.001), temp1);
            cv::cuda::divide(gpuDstF, temp1, res2);
            cv::cuda::min(res2, 1.0f, res2);

            // Blend
            cv::cuda::GpuMat invMask;
            cv::cuda::absdiff(mask, cv::Scalar::all(1.0), invMask);
            cv::cuda::multiply(res1, invMask, res1);
            cv::cuda::multiply(res2, mask, res2);
            cv::cuda::add(res1, res2, gpuResultF);
        }
        break;

    case BlendMode::LinearLight:
        // if src < 0.5: LinearBurn(dst, 2*src) -> dst + 2*src - 1
        // else: LinearDodge(dst, 2*(src-0.5)) -> dst + 2*src - 1 (Wait, same formula?)
        // LinearLight is "dst + 2*src - 1" everywhere?
        // Let's check:
        // src < 0.5: LB(dst, 2*src) = dst + 2*src - 1
        // src > 0.5: LD(dst, 2*src-1) = dst + (2*src - 1) = dst + 2*src - 1
        // YES! Linear Light is simpler than it looks.
        {
            cv::cuda::multiply(gpuSrcF, cv::Scalar::all(2.0), temp1);
            cv::cuda::add(gpuDstF, temp1, gpuResultF);
            cv::cuda::subtract(gpuResultF, cv::Scalar::all(1.0), gpuResultF);
            cv::cuda::max(gpuResultF, 0.0f, gpuResultF);
            cv::cuda::min(gpuResultF, 1.0f, gpuResultF);
        }
        break;

    case BlendMode::PinLight:
        // if src < 0.5: Darken(dst, 2*src)
        // else: Lighten(dst, 2*(src-0.5)) = Lighten(dst, 2*src - 1)
        {
            cv::cuda::GpuMat mask;
            cv::cuda::threshold(gpuSrcF, mask, 0.5, 1.0, cv::THRESH_BINARY);

            // Res1 (src < 0.5): min(dst, 2*src)
            cv::cuda::multiply(gpuSrcF, cv::Scalar::all(2.0), temp1);
            cv::cuda::GpuMat res1;
            cv::cuda::min(gpuDstF, temp1, res1);

            // Res2 (src >= 0.5): max(dst, 2*src - 1)
            cv::cuda::subtract(temp1, cv::Scalar::all(1.0), temp1); // 2*src - 1
            cv::cuda::GpuMat res2;
            cv::cuda::max(gpuDstF, temp1, res2);

            // Blend
            cv::cuda::GpuMat invMask;
            cv::cuda::absdiff(mask, cv::Scalar::all(1.0), invMask);
            cv::cuda::multiply(res1, invMask, res1);
            cv::cuda::multiply(res2, mask, res2);
            cv::cuda::add(res1, res2, gpuResultF);
        }
        break;

    case BlendMode::HardMix:
        // if (src < 1-dst) 0 else 1
        // (src + dst >= 1) ? 1 : 0
        {
            cv::cuda::add(gpuSrcF, gpuDstF, temp1);
            cv::cuda::threshold(temp1, gpuResultF, 0.999, 1.0, cv::THRESH_BINARY);
        }
        break;

    // Fallback
    default:
        cv::cuda::addWeighted(gpuDstF, 1.0 - opacity, gpuSrcF, opacity, 0.0, gpuResultF);
        break;
    }

    // Convert back
    gpuResultF.convertTo(dst, CV_8UC3, 255.0);
}
#endif

juce::ValueTree VideoCompositorModule::getExtraStateTree() const
{
    return juce::ValueTree("VideoCompositorState");
}

void VideoCompositorModule::setExtraStateTree(const juce::ValueTree& state)
{
    juce::ignoreUnused(state);
}

#if defined(PRESET_CREATOR_UI)
ImVec2 VideoCompositorModule::getCustomNodeSize() const
{
    // Use medium width for compositor (has more UI elements)
    return ImVec2(480.0f, 0.0f);
}

void VideoCompositorModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    // Widgets like CollapsingHeader, SliderFloat, etc. use WorkRect.Max.x which
    // is the entire canvas in ImNodes, causing them to extend beyond node bounds.
    // Solution: Temporarily constrain WorkRect and ContentRegionRect to node width.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float  cursorX = ImGui::GetCursorPosX();
    const float  nodeRightEdge = cursorX + itemWidth;

    // Save original values
    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;

    // Constrain to node width
    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    ImGui::PushItemWidth(itemWidth);

    // Global Options
    bool useGpu = useGpuParam ? useGpuParam->get() : false;
    if (ImGui::Checkbox("Use GPU (CUDA)", &useGpu))
    {
        if (useGpuParam)
            *useGpuParam = useGpu;
        onModificationEnded();
    }

    // Layer management buttons
    int  numLayers = numLayersParam ? numLayersParam->get() : 1;
    bool canAdd = (numLayers < MAX_LAYERS);
    bool canRemove = (numLayers > 1);

    if (!canAdd)
        ImGui::BeginDisabled();
    if (ImGui::Button("Add Layer", ImVec2((itemWidth / 2.0f) - 4.0f, 0)))
    {
        if (numLayersParam)
            *numLayersParam = numLayers + 1;
        onModificationEnded();
    }
    if (!canAdd)
        ImGui::EndDisabled();

    ImGui::SameLine();

    if (!canRemove)
        ImGui::BeginDisabled();
    if (ImGui::Button("Remove Layer", ImVec2((itemWidth / 2.0f) - 4.0f, 0)))
    {
        if (numLayersParam)
            *numLayersParam = numLayers - 1;
        onModificationEnded();
    }
    if (!canRemove)
        ImGui::EndDisabled();

    // Per-layer controls
    juce::StringArray blendModeNames = {
        "Normal",
        "Add",
        "Multiply",
        "Screen",
        "Overlay",
        "Soft Light",
        "Hard Light",
        "Darken",
        "Lighten",
        "Difference",
        "Exclusion",
        "Color Dodge",
        "Color Burn",
        "Linear Dodge",
        "Linear Burn",
        "Vivid Light",
        "Linear Light",
        "Pin Light",
        "Hard Mix"};

    for (int i = 0; i < numLayers; ++i)
    {
        ImGui::PushID(i); // CRITICAL: Unique ID per layer to prevent conflicts

        const juce::String layerName = "Layer " + juce::String(i + 1);
        const juce::String layerNum = juce::String(i + 1);

        // Set default open state (first layer only, on first use)
        ImGui::SetNextItemOpen(i < 1, ImGuiCond_Once);

        // Use TreeNodeEx with SpanAvailWidth to properly fill the node width without bleeding
        // This ensures the collapsible header respects the node boundaries
        bool layerExpanded = ImGui::TreeNodeEx(
            layerName.toRawUTF8(),
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed |
                ImGuiTreeNodeFlags_AllowOverlap);

        if (layerExpanded)
        {
            // Use a fixed slider width based on itemWidth, accounting for the TreeNodeEx indent
            // TreeNodeEx adds ~20px indent automatically, and we want sliders to fit
            float sliderWidth = itemWidth - 100.0f; // Reserve space for labels
            if (sliderWidth < 100.0f)
                sliderWidth = 100.0f; // Minimum width

            // Push a width that fits within the node
            ImGui::PushItemWidth(sliderWidth);

            // Opacity
            bool  opacityMod = isParamModulated("layer" + layerNum + "_opacity");
            float opacity = opacityMod
                                ? getLiveParamValue(
                                      "layer" + layerNum + "_opacity",
                                      layerOpacityParams[i] ? layerOpacityParams[i]->load() : 1.0f)
                                : (layerOpacityParams[i] ? layerOpacityParams[i]->load() : 1.0f);
            if (opacityMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f"))
            {
                if (!opacityMod && layerOpacityParams[i])
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("layer" + layerNum + "_opacity")))
                        *p = opacity;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !opacityMod)
                onModificationEnded();
            if (!opacityMod)
                adjustParamOnWheel(
                    apvts.getParameter("layer" + layerNum + "_opacity"),
                    "layer" + layerNum + "_opacity",
                    opacity);
            if (opacityMod)
                ImGui::EndDisabled();

            // Blend Mode
            bool blendModeMod = isParamModulated("layer" + layerNum + "_blendMode");
            if (blendModeMod)
                ImGui::BeginDisabled();
            int blendModeIdx = layerBlendModeParams[i] ? layerBlendModeParams[i]->getIndex() : 0;
            const char* blendModeItems[19];
            for (int j = 0; j < 19; ++j)
                blendModeItems[j] = blendModeNames[j].toRawUTF8();

            if (ImGui::Combo("Blend", &blendModeIdx, blendModeItems, 19))
            {
                if (!blendModeMod && layerBlendModeParams[i])
                {
                    layerBlendModeParams[i]->setValueNotifyingHost((float)blendModeIdx / 18.0f);
                    onModificationEnded();
                }
            }
            // Scroll-edit for blend mode combo
            if (!blendModeMod && ImGui::IsItemHovered())
            {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f)
                {
                    const int newIdx = juce::jlimit(0, 18, blendModeIdx + (wheel > 0.0f ? -1 : 1));
                    if (newIdx != blendModeIdx && layerBlendModeParams[i])
                    {
                        layerBlendModeParams[i]->setValueNotifyingHost((float)newIdx / 18.0f);
                        onModificationEnded();
                    }
                }
            }
            if (blendModeMod)
                ImGui::EndDisabled();

            // Position X
            bool  posXMod = isParamModulated("layer" + layerNum + "_posX");
            float posX = posXMod ? getLiveParamValue(
                                       "layer" + layerNum + "_posX",
                                       layerPosXParams[i] ? layerPosXParams[i]->load() : 0.0f)
                                 : (layerPosXParams[i] ? layerPosXParams[i]->load() : 0.0f);
            if (posXMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Pos X", &posX, -1.0f, 1.0f, "%.2f"))
            {
                if (!posXMod && layerPosXParams[i])
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("layer" + layerNum + "_posX")))
                        *p = posX;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !posXMod)
                onModificationEnded();
            if (!posXMod)
                adjustParamOnWheel(
                    apvts.getParameter("layer" + layerNum + "_posX"),
                    "layer" + layerNum + "_posX",
                    posX);
            if (posXMod)
                ImGui::EndDisabled();

            // Position Y
            bool  posYMod = isParamModulated("layer" + layerNum + "_posY");
            float posY = posYMod ? getLiveParamValue(
                                       "layer" + layerNum + "_posY",
                                       layerPosYParams[i] ? layerPosYParams[i]->load() : 0.0f)
                                 : (layerPosYParams[i] ? layerPosYParams[i]->load() : 0.0f);
            if (posYMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Pos Y", &posY, -1.0f, 1.0f, "%.2f"))
            {
                if (!posYMod && layerPosYParams[i])
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("layer" + layerNum + "_posY")))
                        *p = posY;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !posYMod)
                onModificationEnded();
            if (!posYMod)
                adjustParamOnWheel(
                    apvts.getParameter("layer" + layerNum + "_posY"),
                    "layer" + layerNum + "_posY",
                    posY);
            if (posYMod)
                ImGui::EndDisabled();

            // Scale X
            bool  scaleXMod = isParamModulated("layer" + layerNum + "_scaleX");
            float scaleX = scaleXMod
                               ? getLiveParamValue(
                                     "layer" + layerNum + "_scaleX",
                                     layerScaleXParams[i] ? layerScaleXParams[i]->load() : 1.0f)
                               : (layerScaleXParams[i] ? layerScaleXParams[i]->load() : 1.0f);
            if (scaleXMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Scale X", &scaleX, 0.1f, 5.0f, "%.2f"))
            {
                if (!scaleXMod && layerScaleXParams[i])
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("layer" + layerNum + "_scaleX")))
                        *p = scaleX;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !scaleXMod)
                onModificationEnded();
            if (!scaleXMod)
                adjustParamOnWheel(
                    apvts.getParameter("layer" + layerNum + "_scaleX"),
                    "layer" + layerNum + "_scaleX",
                    scaleX);
            if (scaleXMod)
                ImGui::EndDisabled();

            // Scale Y
            bool  scaleYMod = isParamModulated("layer" + layerNum + "_scaleY");
            float scaleY = scaleYMod
                               ? getLiveParamValue(
                                     "layer" + layerNum + "_scaleY",
                                     layerScaleYParams[i] ? layerScaleYParams[i]->load() : 1.0f)
                               : (layerScaleYParams[i] ? layerScaleYParams[i]->load() : 1.0f);
            if (scaleYMod)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Scale Y", &scaleY, 0.1f, 5.0f, "%.2f"))
            {
                if (!scaleYMod && layerScaleYParams[i])
                {
                    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("layer" + layerNum + "_scaleY")))
                        *p = scaleY;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !scaleYMod)
                onModificationEnded();
            if (!scaleYMod)
                adjustParamOnWheel(
                    apvts.getParameter("layer" + layerNum + "_scaleY"),
                    "layer" + layerNum + "_scaleY",
                    scaleY);
            if (scaleYMod)
                ImGui::EndDisabled();

            ImGui::PopItemWidth(); // Match PushItemWidth
            ImGui::TreePop();      // Match TreeNodeEx
        }

        ImGui::PopID(); // Match PushID
    }

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();

    // === RESTORE WORKRECT VALUES ===
    // Restore original WorkRect and ContentRegionRect after drawing all widgets
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}

void VideoCompositorModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Pins are handled via getDynamicInputPins/getDynamicOutputPins
    // This method is called but dynamic pins take precedence
}
#endif
