#include "WebcamLoaderModule.h"
#include "../../video/VideoFrameManager.h"
#include "../../video/CameraEnumerator.h"
#include "../graph/ModularSynthProcessor.h"
#include <opencv2/imgproc.hpp>

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout WebcamLoaderModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterInt>("cameraIndex", "Camera Index", 0, 3, 0));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Node Size", juce::StringArray{"Small", "Normal", "Large"}, 1));

    // New explicit controls
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "resolution",
            "Resolution",
            juce::StringArray{"320x240", "640x480", "1280x720", "1920x1080"},
            1)); // Default 640x480

    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "fps", "FPS", juce::StringArray{"15", "24", "30", "60"}, 2)); // Default 30

    // Phase 3: Advanced Controls
    // Auto Exposure: 0=Manual, 1=Auto
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "autoExposure", "Auto Exposure", juce::StringArray{"Manual", "Auto"}, 1));

    // Exposure: -13 to -1 (log2 seconds)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("exposure", "Exposure", -13.0f, -1.0f, -5.0f));

    // Auto Focus: 0=Manual, 1=Auto
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "autoFocus", "Auto Focus", juce::StringArray{"Manual", "Auto"}, 1));

    // Focus: 0 to 255
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("focus", "Focus", 0.0f, 255.0f, 0.0f));

    // Gain: 0 to 255
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("gain", "Gain", 0.0f, 255.0f, 0.0f));

    // Auto WB: 0=Manual, 1=Auto
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "autoWB", "Auto WB", juce::StringArray{"Manual", "Auto"}, 1));

    // WB Temperature: 2000 to 10000
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "wbTemperature", "WB Temp", 2000.0f, 10000.0f, 4000.0f));

    return {params.begin(), params.end()};
}

WebcamLoaderModule::WebcamLoaderModule()
    : ModuleProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::mono(), true)),
      juce::Thread("Webcam Loader Thread"),
      apvts(*this, nullptr, "WebcamLoaderParams", createParameterLayout())
{
    cameraIndexParam = apvts.getRawParameterValue("cameraIndex");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    resolutionParam = apvts.getRawParameterValue("resolution");
    fpsParam = apvts.getRawParameterValue("fps");

    autoExposureParam = apvts.getRawParameterValue("autoExposure");
    exposureParam = apvts.getRawParameterValue("exposure");
    autoFocusParam = apvts.getRawParameterValue("autoFocus");
    focusParam = apvts.getRawParameterValue("focus");
    gainParam = apvts.getRawParameterValue("gain");
    autoWBParam = apvts.getRawParameterValue("autoWB");
    wbTemperatureParam = apvts.getRawParameterValue("wbTemperature");

    // Camera enumeration is now done by CameraEnumerator singleton on a background thread
    // This constructor is now instant!
}

WebcamLoaderModule::~WebcamLoaderModule()
{
    stopThread(5000);
    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void WebcamLoaderModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    startThread(juce::Thread::Priority::normal);
}

void WebcamLoaderModule::releaseResources() { signalThreadShouldExit(); }

void WebcamLoaderModule::run()
{
    int currentCameraIndex = -1;
    int currentResIndex = -1;
    int currentFpsIndex = -1;

    int       retryCount = 0;
    const int maxRetries = 3;
    int       lastFailedIndex = -1;

    // Use helper to resolve ID
    juce::uint32 myLogicalId = getMyLogicalId();

    while (!threadShouldExit())
    {
        int requestedIndex = (int)cameraIndexParam->load();
        int requestedResIndex = resolutionParam ? (int)resolutionParam->load() : 1;
        int requestedFpsIndex = fpsParam ? (int)fpsParam->load() : 2;

        // Reset retry count if camera index changed
        if (requestedIndex != lastFailedIndex)
        {
            retryCount = 0;
            lastFailedIndex = -1;
        }

        // If camera changed, settings changed, or is not open, try to open it
        if (requestedIndex != currentCameraIndex || requestedResIndex != currentResIndex ||
            requestedFpsIndex != currentFpsIndex || !videoCapture.isOpened())
        {
            if (videoCapture.isOpened())
            {
                videoCapture.release();
            }

            // Check exit before blocking open() call
            if (threadShouldExit())
                break;

            // Measure initialization time
            auto startTime = juce::Time::getMillisecondCounter();

            // Try to open with timeout
            if (openCameraWithTimeout(requestedIndex, 3000))
            {
                // Configure properties immediately
                videoCapture.set(cv::CAP_PROP_BUFFERSIZE, 1);

                // Set resolution based on explicit parameter
                int resIndex = requestedResIndex;
                int widths[] = {320, 640, 1280, 1920};
                int heights[] = {240, 480, 720, 1080};

                // Safety check
                resIndex = juce::jlimit(0, 3, resIndex);

                videoCapture.set(cv::CAP_PROP_FRAME_WIDTH, widths[resIndex]);
                videoCapture.set(cv::CAP_PROP_FRAME_HEIGHT, heights[resIndex]);

                // Set FPS based on explicit parameter
                int fpsIndex = requestedFpsIndex;
                int fpsValues[] = {15, 24, 30, 60};

                // Safety check
                fpsIndex = juce::jlimit(0, 3, fpsIndex);

                videoCapture.set(cv::CAP_PROP_FPS, fpsValues[fpsIndex]);

                // Store actual negotiated values
                actualWidth = (int)videoCapture.get(cv::CAP_PROP_FRAME_WIDTH);
                actualHeight = (int)videoCapture.get(cv::CAP_PROP_FRAME_HEIGHT);
                actualFps = (float)videoCapture.get(cv::CAP_PROP_FPS);

                auto elapsed = juce::Time::getMillisecondCounter() - startTime;
                juce::Logger::writeToLog(
                    "[WebcamLoader] Opened camera " + juce::String(requestedIndex) + " (" +
                    juce::String(actualWidth) + "x" + juce::String(actualHeight) + " @ " +
                    juce::String(actualFps, 1) + "fps) in " + juce::String(elapsed) + "ms");

                currentCameraIndex = requestedIndex;
                currentResIndex = requestedResIndex;
                currentFpsIndex = requestedFpsIndex;
                retryCount = 0;
                lastFailedIndex = -1;
            }
            else
            {
                // Failed - retry with exponential backoff
                retryCount++;
                lastFailedIndex = requestedIndex;

                if (retryCount <= maxRetries)
                {
                    int backoffMs = 1000 * retryCount;
                    juce::Logger::writeToLog(
                        "[WebcamLoader] Camera open failed, retrying in " +
                        juce::String(backoffMs) + "ms (attempt " + juce::String(retryCount) + "/" +
                        juce::String(maxRetries) + ")");
                    wait(backoffMs);
                }
                else
                {
                    juce::Logger::writeToLog(
                        "[WebcamLoader] Camera open failed after " + juce::String(maxRetries) +
                        " attempts");
                    wait(5000);     // Wait longer before next cycle
                    retryCount = 0; // Reset for next cycle
                }
                continue;
            }
        }

        // --- Phase 3: Apply Advanced Controls ---
        if (videoCapture.isOpened())
        {
            // Auto Exposure
            bool autoExp = (autoExposureParam && autoExposureParam->load() > 0.5f);

            // For many webcams, setting CAP_PROP_EXPOSURE automatically disables auto-exposure.
            // But explicit control is better.
            videoCapture.set(cv::CAP_PROP_AUTO_EXPOSURE, autoExp ? 3.0 : 1.0);

            if (!autoExp && exposureParam)
            {
                videoCapture.set(cv::CAP_PROP_EXPOSURE, exposureParam->load());
            }

            // Auto Focus
            bool autoFocus = (autoFocusParam && autoFocusParam->load() > 0.5f);
            videoCapture.set(cv::CAP_PROP_AUTOFOCUS, autoFocus ? 1.0 : 0.0);

            if (!autoFocus && focusParam)
            {
                videoCapture.set(cv::CAP_PROP_FOCUS, focusParam->load());
            }

            // Gain (Manual only usually)
            if (gainParam)
            {
                videoCapture.set(cv::CAP_PROP_GAIN, gainParam->load());
            }

            // White Balance
            bool autoWB = (autoWBParam && autoWBParam->load() > 0.5f);
            videoCapture.set(cv::CAP_PROP_AUTO_WB, autoWB ? 1.0 : 0.0);

            if (!autoWB && wbTemperatureParam)
            {
                videoCapture.set(cv::CAP_PROP_WB_TEMPERATURE, wbTemperatureParam->load());
            }
        }

        // Check exit before blocking read() call
        if (threadShouldExit())
            break;

        cv::Mat frame;
        if (videoCapture.read(frame) && !frame.empty())
        {
            auto t0 = juce::Time::getMillisecondCounterHiRes();

            // Publish frame to central manager using this module's logical ID
            // Re-check ID in case it wasn't ready at start
            if (myLogicalId == 0)
                myLogicalId = getMyLogicalId();

            VideoFrameManager::getInstance().setFrame(myLogicalId, frame);

            // Update local preview for UI (lazy conversion)
            updateGuiFrame(frame);

            auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
            lastProcessTimeMs = (float)elapsed;
            lastProcessWasGpu = false;
        }
        else
        {
            // Lost camera connection
            videoCapture.release();
            currentCameraIndex = -1;
        }

        wait(33); // ~30 FPS
    }

    videoCapture.release();
    if (myLogicalId != 0)
        VideoFrameManager::getInstance().removeSource(myLogicalId);
}

bool WebcamLoaderModule::openCameraWithTimeout(int index, int timeoutMs)
{
    auto startTime = juce::Time::getMillisecondCounter();

// Try to open camera
#if JUCE_WINDOWS
    if (!videoCapture.open(index, cv::CAP_DSHOW))
    {
        // Fallback to default backend
        if (!videoCapture.open(index))
            return false;
    }
#else
    if (!videoCapture.open(index))
        return false;
#endif

    // Wait for first valid frame (proves camera is ready)
    while (juce::Time::getMillisecondCounter() - startTime < timeoutMs)
    {
        if (threadShouldExit())
        {
            videoCapture.release();
            return false;
        }

        cv::Mat testFrame;
        if (videoCapture.read(testFrame) && !testFrame.empty())
        {
            return true;
        }

        wait(100);
    }

    // Timeout
    videoCapture.release();
    return false;
}

juce::uint32 WebcamLoaderModule::getMyLogicalId()
{
    if (storedLogicalId == 0 && parentSynth != nullptr)
    {
        for (const auto& info : parentSynth->getModulesInfo())
        {
            if (parentSynth->getModuleForLogical(info.first) == this)
            {
                storedLogicalId = info.first;
                break;
            }
        }
    }
    return storedLogicalId;
}

void WebcamLoaderModule::updateGuiFrame(const cv::Mat& frame)
{
    const juce::ScopedLock lock(imageLock);

    // Store BGR frame directly (lazy conversion)
    if (latestFrameBgr.empty() || latestFrameBgr.cols != frame.cols ||
        latestFrameBgr.rows != frame.rows)
    {
        latestFrameBgr = cv::Mat(frame.rows, frame.cols, CV_8UC3);
    }
    frame.copyTo(latestFrameBgr);
}

juce::Image WebcamLoaderModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);

    if (latestFrameBgr.empty())
        return juce::Image();

    // Convert BGR -> BGRA only when GUI requests it
    if (latestFrameForGui.isNull() || latestFrameForGui.getWidth() != latestFrameBgr.cols ||
        latestFrameForGui.getHeight() != latestFrameBgr.rows)
    {
        latestFrameForGui =
            juce::Image(juce::Image::ARGB, latestFrameBgr.cols, latestFrameBgr.rows, true);
    }

    cv::Mat bgraFrame;
    cv::cvtColor(latestFrameBgr, bgraFrame, cv::COLOR_BGR2BGRA);

    juce::Image::BitmapData destData(latestFrameForGui, juce::Image::BitmapData::writeOnly);
    memcpy(destData.data, bgraFrame.data, bgraFrame.total() * bgraFrame.elemSize());

    return latestFrameForGui.createCopy();
}

void WebcamLoaderModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();

    // --- BEGIN FIX: Find our own ID if it's not set ---
    juce::uint32 myLogicalId = getMyLogicalId();
    // --- END FIX ---

    // Output this module's logical ID on the "Source ID" pin
    if (buffer.getNumChannels() > 0)
    {
        float sourceId = (float)myLogicalId; // Use the resolved ID
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            buffer.setSample(0, sample, sourceId);
        }
    }
}

#if defined(PRESET_CREATOR_UI)
ImVec2 WebcamLoaderModule::getCustomNodeSize() const
{
    // Return different width based on zoom level (0=240,1=480,2=960)
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void WebcamLoaderModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // Get the latest list from the fast, cached singleton
    auto availableCameraNames = CameraEnumerator::getInstance().getAvailableCameraNames();

    // Add a refresh button to re-scan for cameras if needed
    if (ImGui::Button("Refresh List"))
    {
        CameraEnumerator::getInstance().rescan();
    }
    ImGui::SameLine();

    int currentIndex = (int)cameraIndexParam->load();
    currentIndex = juce::jlimit(0, juce::jmax(0, availableCameraNames.size() - 1), currentIndex);

    const char* currentCameraName = availableCameraNames[currentIndex].toRawUTF8();

    // Check if we're in a scanning state or no cameras found
    bool isScanning =
        (availableCameraNames.size() == 1 && availableCameraNames[0].startsWith("Scanning"));
    bool noCameras =
        (availableCameraNames.size() == 1 && availableCameraNames[0].startsWith("No cameras"));

    if (isScanning || noCameras)
    {
        ImGui::BeginDisabled();
    }

    bool cameraModulated = isParamModulated("cameraIndex");
    if (cameraModulated)
        ImGui::BeginDisabled();
    if (ImGui::BeginCombo("Camera", currentCameraName))
    {
        for (int i = 0; i < availableCameraNames.size(); ++i)
        {
            const bool          isSelected = (currentIndex == i);
            const juce::String& cameraName = availableCameraNames[i];

            // Don't allow selecting "Scanning..." or "No cameras found"
            bool isSelectable =
                !cameraName.startsWith("Scanning") && !cameraName.startsWith("No cameras");

            if (!isSelectable)
            {
                ImGui::BeginDisabled();
            }

            if (ImGui::Selectable(cameraName.toRawUTF8(), isSelected))
            {
                *dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("cameraIndex")) = i;
                onModificationEnded();
            }

            if (!isSelectable)
            {
                ImGui::EndDisabled();
            }

            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    // Scroll-edit for camera combo
    if (!cameraModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int maxIndex = juce::jmax(0, (int)availableCameraNames.size() - 1);
            const int newIndex = juce::jlimit(0, maxIndex, currentIndex + (wheel > 0.0f ? -1 : 1));
            if (newIndex != currentIndex)
            {
                *dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("cameraIndex")) =
                    newIndex;
                onModificationEnded();
            }
        }
    }
    if (cameraModulated)
        ImGui::EndDisabled();

    if (isScanning || noCameras)
    {
        ImGui::EndDisabled();
    }

    // Zoom buttons (+ to increase, - to decrease) across 3 levels
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

    // --- Resolution Dropdown ---
    {
        bool resModulated = isParamModulated("resolution");
        if (resModulated)
            ImGui::BeginDisabled();
        const char* resNames[] = {"320x240", "640x480", "1280x720", "1920x1080"};
        int         currentRes = resolutionParam ? (int)resolutionParam->load() : 1;
        currentRes = juce::jlimit(0, 3, currentRes);

        if (ImGui::Combo("Resolution", &currentRes, resNames, 4))
        {
            if (!resModulated)
            {
                if (auto* p = apvts.getParameter("resolution"))
                    p->setValueNotifyingHost((float)currentRes / 3.0f);
                onModificationEnded();
            }
        }
        // Scroll-edit for resolution combo
        if (!resModulated && ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int newRes = juce::jlimit(0, 3, currentRes + (wheel > 0.0f ? -1 : 1));
                if (newRes != currentRes)
                {
                    if (auto* p = apvts.getParameter("resolution"))
                        p->setValueNotifyingHost((float)newRes / 3.0f);
                    onModificationEnded();
                }
            }
        }
        if (resModulated)
            ImGui::EndDisabled();
    }

    // --- FPS Dropdown ---
    {
        bool fpsModulated = isParamModulated("fps");
        if (fpsModulated)
            ImGui::BeginDisabled();
        const char* fpsNames[] = {"15", "24", "30", "60"};
        int         currentFps = fpsParam ? (int)fpsParam->load() : 2;
        currentFps = juce::jlimit(0, 3, currentFps);

        if (ImGui::Combo("FPS", &currentFps, fpsNames, 4))
        {
            if (!fpsModulated)
            {
                if (auto* p = apvts.getParameter("fps"))
                    p->setValueNotifyingHost((float)currentFps / 3.0f);
                onModificationEnded();
            }
        }
        // Scroll-edit for FPS combo
        if (!fpsModulated && ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int newFps = juce::jlimit(0, 3, currentFps + (wheel > 0.0f ? -1 : 1));
                if (newFps != currentFps)
                {
                    if (auto* p = apvts.getParameter("fps"))
                        p->setValueNotifyingHost((float)newFps / 3.0f);
                    onModificationEnded();
                }
            }
        }
        if (fpsModulated)
            ImGui::EndDisabled();
    }

    // --- Phase 3: Advanced Controls ---
    ImGui::Separator();
    ImGui::Text("Image Settings");

    // Exposure
    if (autoExposureParam && exposureParam)
    {
        bool autoExp = (autoExposureParam->load() > 0.5f);
        if (ImGui::Checkbox("Auto Exposure", &autoExp))
        {
            if (auto* p = apvts.getParameter("autoExposure"))
                p->setValueNotifyingHost(autoExp ? 1.0f : 0.0f);
            onModificationEnded();
        }

        if (!autoExp)
        {
            bool  expModulated = isParamModulated("exposure");
            float exp = exposureParam->load();
            if (expModulated)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Exposure", &exp, -13.0f, -1.0f))
            {
                if (!expModulated)
                {
                    if (auto* p = apvts.getParameter("exposure"))
                        p->setValueNotifyingHost((exp + 13.0f) / 12.0f); // Normalize for host
                    onModificationEnded();
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !expModulated)
                onModificationEnded();
            if (!expModulated)
                adjustParamOnWheel(apvts.getParameter("exposure"), "exposure", exp);
            if (expModulated)
                ImGui::EndDisabled();
        }
    }

    // Focus
    if (autoFocusParam && focusParam)
    {
        bool autoFocus = (autoFocusParam->load() > 0.5f);
        if (ImGui::Checkbox("Auto Focus", &autoFocus))
        {
            if (auto* p = apvts.getParameter("autoFocus"))
                p->setValueNotifyingHost(autoFocus ? 1.0f : 0.0f);
            onModificationEnded();
        }

        if (!autoFocus)
        {
            bool  focusModulated = isParamModulated("focus");
            float focus = focusParam->load();
            if (focusModulated)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Focus", &focus, 0.0f, 255.0f))
            {
                if (!focusModulated)
                {
                    if (auto* p = apvts.getParameter("focus"))
                        p->setValueNotifyingHost(focus / 255.0f);
                    onModificationEnded();
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !focusModulated)
                onModificationEnded();
            if (!focusModulated)
                adjustParamOnWheel(apvts.getParameter("focus"), "focus", focus);
            if (focusModulated)
                ImGui::EndDisabled();
        }
    }

    // Gain
    if (gainParam)
    {
        bool  gainModulated = isParamModulated("gain");
        float gain = gainParam->load();
        if (gainModulated)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Gain", &gain, 0.0f, 255.0f))
        {
            if (!gainModulated)
            {
                if (auto* p = apvts.getParameter("gain"))
                    p->setValueNotifyingHost(gain / 255.0f);
                onModificationEnded();
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !gainModulated)
            onModificationEnded();
        if (!gainModulated)
            adjustParamOnWheel(apvts.getParameter("gain"), "gain", gain);
        if (gainModulated)
            ImGui::EndDisabled();
    }

    // White Balance
    if (autoWBParam && wbTemperatureParam)
    {
        bool autoWB = (autoWBParam->load() > 0.5f);
        if (ImGui::Checkbox("Auto WB", &autoWB))
        {
            if (auto* p = apvts.getParameter("autoWB"))
                p->setValueNotifyingHost(autoWB ? 1.0f : 0.0f);
            onModificationEnded();
        }

        if (!autoWB)
        {
            bool  wbModulated = isParamModulated("wbTemperature");
            float wb = wbTemperatureParam->load();
            if (wbModulated)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("WB Temp", &wb, 2000.0f, 10000.0f))
            {
                if (!wbModulated)
                {
                    if (auto* p = apvts.getParameter("wbTemperature"))
                        p->setValueNotifyingHost((wb - 2000.0f) / 8000.0f);
                    onModificationEnded();
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && !wbModulated)
                onModificationEnded();
            if (!wbModulated)
                adjustParamOnWheel(apvts.getParameter("wbTemperature"), "wbTemperature", wb);
            if (wbModulated)
                ImGui::EndDisabled();
        }
    }

    const juce::String sourceText = juce::String::formatted("Source ID: %d", (int)getLogicalId());
    ThemeText(sourceText.toRawUTF8(), theme.text.section_header);

    // Display actual camera info
    if (actualWidth > 0)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Info:");
        ImGui::Text("%dx%d @ %.1f FPS", actualWidth.load(), actualHeight.load(), actualFps.load());
    }

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();
}

void WebcamLoaderModule::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioOutputPin("Source ID", 0);
}
#endif
