#include "HandTrackerModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include "../../utils/CudaDeviceCountCache.h"
#include <opencv2/imgproc.hpp>

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include <juce_opengl/juce_opengl.h>
#include <imgui_internal.h> // For WorkRect workaround to fix widget bleeding in nodes
#include <map>
#include <array>
#include <unordered_map>
#endif

// Thumb/Index/Middle/Ring/Pinky chains plus wrist
static const std::vector<std::pair<int, int>> HAND_SKELETON_PAIRS = {
    {0, 1},  {1, 2},   {2, 3},   {3, 4},   // Thumb
    {0, 5},  {5, 6},   {6, 7},   {7, 8},   // Index
    {0, 9},  {9, 10},  {10, 11}, {11, 12}, // Middle
    {0, 13}, {13, 14}, {14, 15}, {15, 16}, // Ring
    {0, 17}, {17, 18}, {18, 19}, {19, 20}  // Pinky
};

juce::AudioProcessorValueTreeState::ParameterLayout HandTrackerModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("sourceId", "Source ID", 0.0f, 1000.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("confidence", "Confidence", 0.0f, 1.0f, 0.1f));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Zoom Level", juce::StringArray{"Small", "Normal", "Large"}, 1));

// GPU acceleration toggle - default from global setting
#if defined(PRESET_CREATOR_UI)
    bool defaultGpu = ImGuiNodeEditorComponent::getGlobalGpuEnabled();
#else
    bool defaultGpu = true;
#endif
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", defaultGpu));
    return {params.begin(), params.end()};
}

HandTrackerModule::HandTrackerModule()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::mono(), true)
              .withOutput(
                  "CV Out",
                  juce::AudioChannelSet::discreteChannels(46),
                  true) // 42 existing + 4 zone gates
              .withOutput("Video Out", juce::AudioChannelSet::mono(), true)     // PASSTHROUGH
              .withOutput("Cropped Out", juce::AudioChannelSet::mono(), true)), // CROPPED
      juce::Thread("Hand Tracker Thread"),
      apvts(*this, nullptr, "HandTrackerParams", createParameterLayout())
{
    sourceIdParam = apvts.getRawParameterValue("sourceId");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    confidenceThresholdParam = apvts.getRawParameterValue("confidence");
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));
    fifoBuffer.resize(16);
}

HandTrackerModule::~HandTrackerModule() { stopThread(5000); }

void HandTrackerModule::prepareToPlay(double, int) { startThread(juce::Thread::Priority::normal); }

void HandTrackerModule::releaseResources()
{
    signalThreadShouldExit();
    stopThread(5000);
}

void HandTrackerModule::loadModel()
{
    auto appDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
    auto assetsDir = appDir.getChildFile("assets");
    auto handDir = assetsDir.getChildFile("openpose_models").getChildFile("hand");
    auto protoPath = handDir.getChildFile("pose_deploy.prototxt").getFullPathName();
    auto modelPath = handDir.getChildFile("pose_iter_102000.caffemodel").getFullPathName();
    if (juce::File(protoPath).existsAsFile() && juce::File(modelPath).existsAsFile())
    {
        try
        {
            net = cv::dnn::readNetFromCaffe(protoPath.toStdString(), modelPath.toStdString());

// CRITICAL: Set backend immediately after loading model
#if WITH_CUDA_SUPPORT
            bool useGpu = useGpuParam ? useGpuParam->get() : false;
            if (useGpu && CudaDeviceCountCache::isAvailable())
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                juce::Logger::writeToLog("[HandTracker] ✓ Model loaded with CUDA backend (GPU)");
            }
            else
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                juce::Logger::writeToLog("[HandTracker] Model loaded with CPU backend");
            }
#else
            net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            juce::Logger::writeToLog(
                "[HandTracker] Model loaded with CPU backend (CUDA not compiled)");
#endif

            modelLoaded = true;
        }
        catch (...)
        {
            modelLoaded = false;
        }
    }
}

void HandTrackerModule::run()
{
    if (!modelLoaded)
        loadModel();

#if WITH_CUDA_SUPPORT
    bool lastGpuState = false;     // Track GPU state to minimize backend switches
    bool loggedGpuWarning = false; // Only warn once if no GPU available
#endif

    while (!threadShouldExit())
    {
        if (!modelLoaded)
            loadModel();

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

        cv::Mat frame = prefetchedFrame.empty()
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
        if (frame.empty())
        {
            wait(50);
            continue;
        }

        bool useGpu = false;

#if WITH_CUDA_SUPPORT
        // Check if user wants GPU and if CUDA device is available
        useGpu = useGpuParam ? useGpuParam->get() : false;
        if (useGpu && !CudaDeviceCountCache::isAvailable())
        {
            useGpu = false; // Fallback to CPU
            if (!loggedGpuWarning)
            {
                juce::Logger::writeToLog(
                    "[HandTracker] WARNING: GPU requested but no CUDA device found. Using CPU.");
                loggedGpuWarning = true;
            }
        }

        // Set DNN backend only when state changes (expensive operation)
        if (useGpu != lastGpuState)
        {
            if (useGpu)
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                juce::Logger::writeToLog("[HandTracker] ✓ Switched to CUDA backend (GPU)");
            }
            else
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                juce::Logger::writeToLog("[HandTracker] Switched to CPU backend");
            }
            lastGpuState = useGpu;
        }
#endif

        // Save a clean copy for cropping (before drawing annotations)
        cv::Mat originalFrame;
        frame.copyTo(originalFrame);

        // NOTE: For DNN models, blobFromImage works on CPU
        // The GPU acceleration happens in net.forward() when backend is set to CUDA
        cv::Mat blob = cv::dnn::blobFromImage(
            frame, 1.0 / 255.0, cv::Size(368, 368), cv::Scalar(), false, false);
        net.setInput(blob);
        // Forward pass (GPU-accelerated if backend is CUDA)
        cv::Mat    out = net.forward();
        HandResult result{};
        // Initialize zone hits to false
        for (int z = 0; z < 4; ++z)
            result.zoneHits[z] = false;

        parseHandOutput(out, frame.cols, frame.rows, result);

        // Check zone hits: check if ANY detected keypoint is inside a zone
        if (result.detectedPoints > 0)
        {
            for (int colorIdx = 0; colorIdx < 4; ++colorIdx)
            {
                std::vector<ZoneRect> rects;
                loadZoneRects(colorIdx, rects);

                bool hit = false;
                // Check each detected keypoint
                for (int i = 0; i < HAND_NUM_KEYPOINTS; ++i)
                {
                    // Skip if keypoint not detected
                    if (result.keypoints[i][0] < 0 || result.keypoints[i][1] < 0)
                        continue;

                    // Normalize keypoint position to 0-1 range
                    float posX = result.keypoints[i][0] / (float)frame.cols;
                    float posY = result.keypoints[i][1] / (float)frame.rows;

                    // Check if this keypoint is inside any rectangle of this color zone
                    for (const auto& rect : rects)
                    {
                        if (posX >= rect.x && posX <= rect.x + rect.width && posY >= rect.y &&
                            posY <= rect.y + rect.height)
                        {
                            hit = true;
                            break; // Found a keypoint in this zone
                        }
                    }

                    if (hit)
                        break; // Found a hit for this zone, check next zone
                }
                result.zoneHits[colorIdx] = hit;
            }
        }

        if (fifo.getFreeSpace() >= 1)
        {
            auto w = fifo.write(1);
            if (w.blockSize1 > 0)
                fifoBuffer[w.startIndex1] = result;
        }

        // --- CROPPED OUTPUT LOGIC ---
        // Calculate bounding box from detected keypoints
        int minX = INT_MAX, minY = INT_MAX, maxX = 0, maxY = 0;
        int validPoints = 0;
        for (int i = 0; i < HAND_NUM_KEYPOINTS; ++i)
        {
            if (result.keypoints[i][0] >= 0 && result.keypoints[i][1] >= 0)
            {
                int x = (int)result.keypoints[i][0];
                int y = (int)result.keypoints[i][1];
                minX = juce::jmin(minX, x);
                minY = juce::jmin(minY, y);
                maxX = juce::jmax(maxX, x);
                maxY = juce::jmax(maxY, y);
                validPoints++;
            }
        }

        if (validPoints > 0)
        {
            // Add padding around the bounding box
            int      padding = 20;
            int      boxX = juce::jmax(0, minX - padding);
            int      boxY = juce::jmax(0, minY - padding);
            int      boxW = juce::jmin(originalFrame.cols - boxX, maxX - minX + padding * 2);
            int      boxH = juce::jmin(originalFrame.rows - boxY, maxY - minY + padding * 2);
            cv::Rect box(boxX, boxY, boxW, boxH);
            box &= cv::Rect(0, 0, originalFrame.cols, originalFrame.rows);

            if (box.area() > 0)
            {
                cv::Mat cropped = originalFrame(box);
                VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), cropped);
            }
        }
        else
        {
            // Clear cropped output when no hand detected
            cv::Mat emptyFrame;
            VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), emptyFrame);
        }

        // Draw minimal skeleton
        for (const auto& p : HAND_SKELETON_PAIRS)
        {
            int a = p.first, b = p.second;
            if (result.keypoints[a][0] >= 0 && result.keypoints[b][0] >= 0)
                cv::line(
                    frame,
                    {(int)result.keypoints[a][0], (int)result.keypoints[a][1]},
                    {(int)result.keypoints[b][0], (int)result.keypoints[b][1]},
                    {0, 255, 0},
                    2);
        }
        for (int i = 0; i < HAND_NUM_KEYPOINTS; ++i)
            if (result.keypoints[i][0] >= 0)
                cv::circle(
                    frame,
                    {(int)result.keypoints[i][0], (int)result.keypoints[i][1]},
                    3,
                    {0, 0, 255},
                    -1);

        // --- PASSTHROUGH LOGIC ---
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
        updateGuiFrame(frame);
        if (myLogicalId != 0)
            VideoFrameManager::getInstance().setFrame(myLogicalId, frame);
        wait(66);
    }
}

void HandTrackerModule::parseHandOutput(
    const cv::Mat& netOutput,
    int            frameWidth,
    int            frameHeight,
    HandResult&    result)
{
    int   H = netOutput.size[2];
    int   W = netOutput.size[3];
    float thresh = confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f;
    result.detectedPoints = 0;
    int count = juce::jmin(HAND_NUM_KEYPOINTS, netOutput.size[1]);
    for (int i = 0; i < count; ++i)
    {
        cv::Mat   heat(H, W, CV_32F, (void*)netOutput.ptr<float>(0, i));
        double    maxVal;
        cv::Point maxLoc;
        cv::minMaxLoc(heat, nullptr, &maxVal, nullptr, &maxLoc);
        if (maxVal > thresh)
        {
            result.keypoints[i][0] = (float)maxLoc.x * frameWidth / W;
            result.keypoints[i][1] = (float)maxLoc.y * frameHeight / H;
            result.detectedPoints++;
        }
        else
        {
            result.keypoints[i][0] = -1.0f;
            result.keypoints[i][1] = -1.0f;
        }
    }
}

void HandTrackerModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    // Read Source ID from input pin
    auto in = getBusBuffer(buffer, true, 0);
    if (in.getNumChannels() > 0 && in.getNumSamples() > 0)
        currentSourceId.store((juce::uint32)in.getSample(0, 0));

    // --- BEGIN FIX: Find our own ID if it's not set ---
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

    // Read ALL available results from FIFO to ensure latest result is used (like
    // ColorTrackerModule)
    while (fifo.getNumReady() > 0)
    {
        auto readScope = fifo.read(1);
        if (readScope.blockSize1 > 0)
            lastResultForAudio = fifoBuffer[readScope.startIndex1];
    }

    // Output CV on bus 0
    auto cvOutBus = getBusBuffer(buffer, false, 0);

    // Get wrist position (keypoint 0) - this is our reference point
    float wristX =
        (lastResultForAudio.keypoints[0][0] >= 0) ? lastResultForAudio.keypoints[0][0] : -1.0f;
    float wristY =
        (lastResultForAudio.keypoints[0][1] >= 0) ? lastResultForAudio.keypoints[0][1] : -1.0f;

    // Normalization factors (using typical frame size - actual frame size could vary)
    // These are used to normalize relative offsets to a reasonable 0.0-1.0 range
    const float normScaleX = 1.0f / 640.0f; // Normalize by typical frame width
    const float normScaleY = 1.0f / 480.0f; // Normalize by typical frame height

    // Output wrist absolute position (channels 0, 1)
    float wristX_norm = (wristX >= 0) ? juce::jlimit(0.0f, 1.0f, wristX * normScaleX) : 0.5f;
    float wristY_norm = (wristY >= 0) ? juce::jlimit(0.0f, 1.0f, wristY * normScaleY) : 0.5f;

    for (int s = 0; s < cvOutBus.getNumSamples(); ++s)
    {
        cvOutBus.setSample(0, s, wristX_norm); // Wrist X (absolute)
        cvOutBus.setSample(1, s, wristY_norm); // Wrist Y (absolute)
    }

    // Output all other keypoints relative to wrist (channels 2-41)
    for (int i = 1; i < HAND_NUM_KEYPOINTS; ++i)
    {
        int chX = i * 2;     // Channel for X (e.g., i=1 -> ch 2, i=2 -> ch 4, etc.)
        int chY = i * 2 + 1; // Channel for Y (e.g., i=1 -> ch 3, i=2 -> ch 5, etc.)

        if (chY >= cvOutBus.getNumChannels())
            break;

        if (wristX >= 0 && wristY >= 0 && lastResultForAudio.keypoints[i][0] >= 0 &&
            lastResultForAudio.keypoints[i][1] >= 0)
        {
            // Calculate relative offset from wrist
            float relX = (lastResultForAudio.keypoints[i][0] - wristX) * normScaleX;
            float relY = (lastResultForAudio.keypoints[i][1] - wristY) * normScaleY;

            // Map relative offset to 0.0-1.0 range where 0.5 = no offset (centered at wrist)
            // Typical hand spans ~0.1-0.2 in normalized frame units, so we scale by 2-3x to fill
            // range
            const float relScale = 2.5f; // Scale factor to make relative positions more usable
            float       relX_norm = juce::jlimit(0.0f, 1.0f, 0.5f + relX * relScale);
            float       relY_norm = juce::jlimit(0.0f, 1.0f, 0.5f + relY * relScale);

            for (int s = 0; s < cvOutBus.getNumSamples(); ++s)
            {
                cvOutBus.setSample(chX, s, relX_norm);
                cvOutBus.setSample(chY, s, relY_norm);
            }
        }
        else
        {
            // No wrist or keypoint not detected - output center value (0.5 = no offset)
            for (int s = 0; s < cvOutBus.getNumSamples(); ++s)
            {
                cvOutBus.setSample(chX, s, 0.5f);
                cvOutBus.setSample(chY, s, 0.5f);
            }
        }
    }

    // Output zone gates (channels 42-45)
    for (int z = 0; z < 4; ++z)
    {
        int ch = 42 + z;
        if (ch < cvOutBus.getNumChannels())
        {
            float gateValue = lastResultForAudio.zoneHits[z] ? 1.0f : 0.0f;
            // Fill entire buffer with same value (gates should be steady-state)
            for (int s = 0; s < cvOutBus.getNumSamples(); ++s)
                cvOutBus.setSample(ch, s, gateValue);
        }
    }

    // Passthrough Video ID on bus 1
    auto videoOutBus = getBusBuffer(buffer, false, 1);
    if (videoOutBus.getNumChannels() > 0)
    {
        float primaryId = static_cast<float>(myLogicalId); // Use the resolved ID
        for (int s = 0; s < videoOutBus.getNumSamples(); ++s)
            videoOutBus.setSample(0, s, primaryId);
    }

    // Cropped Video ID on bus 2
    auto croppedOutBus = getBusBuffer(buffer, false, 2);
    if (croppedOutBus.getNumChannels() > 0)
    {
        float secondaryId = static_cast<float>(getSecondaryLogicalId());
        for (int s = 0; s < croppedOutBus.getNumSamples(); ++s)
            croppedOutBus.setSample(0, s, secondaryId);
    }
}

#if defined(PRESET_CREATOR_UI)
ImVec2 HandTrackerModule::getCustomNodeSize() const
{
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

static const char* HAND_NAMES[HAND_NUM_KEYPOINTS] = {
    "Wrist",  "Thumb1", "Thumb2",  "Thumb3",  "Thumb4",  "Index1",  "Index2",
    "Index3", "Index4", "Middle1", "Middle2", "Middle3", "Middle4", "Ring1",
    "Ring2",  "Ring3",  "Ring4",   "Pinky1",  "Pinky2",  "Pinky3",  "Pinky4"};

void HandTrackerModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    ImGui::PushItemWidth(itemWidth);

// GPU ACCELERATION TOGGLE
#if WITH_CUDA_SUPPORT
    bool cudaAvailable = CudaDeviceCountCache::isAvailable();

    if (!cudaAvailable)
    {
        ImGui::BeginDisabled();
    }

    bool useGpu = useGpuParam->get();
    if (ImGui::Checkbox("⚡ Use GPU (CUDA)", &useGpu))
    {
        *useGpuParam = useGpu;
        onModificationEnded();
    }

    if (!cudaAvailable)
    {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                "No CUDA-enabled GPU detected.\nCheck that your GPU supports CUDA and drivers are "
                "installed.");
        }
    }
    else if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Enable GPU acceleration for hand tracking.\nRequires CUDA-capable NVIDIA GPU.");
    }
#else
    ImGui::TextDisabled("🚫 GPU support not compiled");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "OpenCV was built without CUDA support.\nRebuild with WITH_CUDA=ON to enable GPU "
            "acceleration.");
    }
#endif

    // Confidence
    bool  confMod = isParamModulated("confidence");
    float confFallback = confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f;
    float conf = confMod ? getLiveParamValue("confidence", confFallback) : confFallback;
    if (confMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Confidence", &conf, 0.0f, 1.0f, "%.2f"))
    {
        if (!confMod)
            *dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("confidence")) = conf;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !confMod)
        onModificationEnded();
    if (!confMod)
        adjustParamOnWheel(apvts.getParameter("confidence"), "confidence", conf);
    if (confMod)
        ImGui::EndDisabled();
    // Zoom -/+
    bool zoomMod = isParamModulated("zoomLevel");
    int  level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    float bw = (itemWidth / 2.0f) - 4.0f;
    bool  atMin = (level <= 0), atMax = (level >= 2);
    if (zoomMod)
        ImGui::BeginDisabled();
    if (atMin)
        ImGui::BeginDisabled();
    if (ImGui::Button("-", ImVec2(bw, 0)))
    {
        int nl = juce::jmax(0, level - 1);
        if (auto* p = apvts.getParameter("zoomLevel"))
            p->setValueNotifyingHost((float)nl / 2.0f);
        onModificationEnded();
    }
    if (atMin)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (atMax)
        ImGui::BeginDisabled();
    if (ImGui::Button("+", ImVec2(bw, 0)))
    {
        int nl = juce::jmin(2, level + 1);
        if (auto* p = apvts.getParameter("zoomLevel"))
            p->setValueNotifyingHost((float)nl / 2.0f);
        onModificationEnded();
    }
    if (atMax)
        ImGui::EndDisabled();
    // Scroll-edit for zoom level
    if (!zoomMod && ImGui::IsItemHovered())
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
    if (zoomMod)
        ImGui::EndDisabled();

    // Zone color palette (4 colors)
    static const ImVec4 ZONE_COLORS[4] = {
        ImVec4(1.0f, 0.0f, 0.0f, 0.3f), // Red - 30% opacity
        ImVec4(0.0f, 1.0f, 0.0f, 0.3f), // Green - 30% opacity
        ImVec4(0.0f, 0.0f, 1.0f, 0.3f), // Blue - 30% opacity
        ImVec4(1.0f, 1.0f, 0.0f, 0.3f)  // Yellow - 30% opacity
    };

    // Static state for mouse interaction (per-instance using logical ID)
    static std::map<int, int>   activeZoneColorIndexByNode;
    static std::map<int, int>   drawingZoneIndexByNode;
    static std::map<int, float> dragStartXByNode;
    static std::map<int, float> dragStartYByNode;

    int nodeId = (int)getLogicalId();

    // Initialize active color index if not set
    if (activeZoneColorIndexByNode.find(nodeId) == activeZoneColorIndexByNode.end())
        activeZoneColorIndexByNode[nodeId] = 0;

    // Initialize drawingZoneIndex to -1 (not drawing) if not set - MUST do before accessing
    // reference!
    if (drawingZoneIndexByNode.find(nodeId) == drawingZoneIndexByNode.end())
        drawingZoneIndexByNode[nodeId] = -1;

    // Now safe to get references
    int&   activeZoneColorIndex = activeZoneColorIndexByNode[nodeId];
    int&   drawingZoneIndex = drawingZoneIndexByNode[nodeId];
    float& dragStartX = dragStartXByNode[nodeId];
    float& dragStartY = dragStartYByNode[nodeId];

    // Color picker boxes
    ImGui::Text("Zone Colors:");
    ImGui::SameLine();
    for (int c = 0; c < 4; ++c)
    {
        ImGui::PushID(c);
        ImVec4 color = ZONE_COLORS[c];
        color.w = 1.0f; // Full opacity for picker button
        if (ImGui::ColorButton(
                "##ZoneColor",
                color,
                ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                ImVec2(20, 20)))
        {
            activeZoneColorIndex = c;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Click to select color %d", c + 1);
        }
        ImGui::PopID();
        if (c < 3)
            ImGui::SameLine();
    }

    // Video preview with zone overlays
    juce::Image frame = getLatestFrame();
    if (!frame.isNull())
    {
        // Use static map for texture management (per-module-instance textures)
        static std::unordered_map<int, std::unique_ptr<juce::OpenGLTexture>> localTextures;

        if (localTextures.find(nodeId) == localTextures.end())
            localTextures[nodeId] = std::make_unique<juce::OpenGLTexture>();

        auto* texture = localTextures[nodeId].get();
        texture->loadImage(frame);

        if (texture->getTextureID() != 0)
        {
            float  ar = (float)frame.getHeight() / juce::jmax(1.0f, (float)frame.getWidth());
            ImVec2 size(itemWidth, itemWidth * ar);
            ImGui::Image(
                (void*)(intptr_t)texture->getTextureID(), size, ImVec2(0, 1), ImVec2(1, 0));

            // Get image screen coordinates and size for interaction
            ImVec2      imageRectMin = ImGui::GetItemRectMin();
            ImVec2      imageRectMax = ImGui::GetItemRectMax();
            ImVec2      imageSize = ImGui::GetItemRectSize();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Use InvisibleButton to capture mouse input and prevent node movement (like
            // CropVideoModule)
            ImGui::SetCursorScreenPos(imageRectMin);
            ImGui::InvisibleButton("##zone_interaction", imageSize);

            ImVec2 mousePos = ImGui::GetMousePos();

            // Draw zones - each color zone can have multiple rectangles
            for (int colorIdx = 0; colorIdx < 4; ++colorIdx)
            {
                std::vector<ZoneRect> rects;
                loadZoneRects(colorIdx, rects);

                ImVec4 color = ZONE_COLORS[colorIdx];
                ImU32  fillColor = ImGui::ColorConvertFloat4ToU32(color);
                ImU32  borderColor =
                    ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 1.0f));

                for (const auto& rect : rects)
                {
                    ImVec2 zoneMin(
                        imageRectMin.x + rect.x * imageSize.x,
                        imageRectMin.y + rect.y * imageSize.y);
                    ImVec2 zoneMax(
                        imageRectMin.x + (rect.x + rect.width) * imageSize.x,
                        imageRectMin.y + (rect.y + rect.height) * imageSize.y);

                    drawList->AddRectFilled(zoneMin, zoneMax, fillColor);
                    drawList->AddRect(zoneMin, zoneMax, borderColor, 0.0f, 0, 2.0f);
                }
            }

            // Draw keypoint positions (the points being checked for zone hits) - small red dots
            // Use lastResultForAudio which is updated in processBlock() (safe to read from UI
            // thread)
            const HandResult& uiResult = lastResultForAudio;

            // Draw a small red dot at each detected keypoint
            ImU32 redColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            for (int i = 0; i < HAND_NUM_KEYPOINTS; ++i)
            {
                // Only draw if keypoint was actually detected
                if (uiResult.keypoints[i][0] >= 0 && uiResult.keypoints[i][1] >= 0)
                {
                    // Normalize keypoint position to 0-1 range (match what we do in zone detection)
                    float posX = uiResult.keypoints[i][0] / (float)frame.getWidth();
                    float posY = uiResult.keypoints[i][1] / (float)frame.getHeight();

                    // Convert normalized coordinates to screen coordinates
                    float  centerX = imageRectMin.x + posX * imageSize.x;
                    float  centerY = imageRectMin.y + posY * imageSize.y;
                    ImVec2 center(centerX, centerY);

                    // Draw small red dot (radius 3 pixels)
                    drawList->AddCircleFilled(center, 3.0f, redColor);
                }
            }

            // Mouse interaction - use InvisibleButton's hover state
            if (ImGui::IsItemHovered())
            {
                // Normalize mouse position (0.0-1.0)
                float mouseX = (mousePos.x - imageRectMin.x) / imageSize.x;
                float mouseY = (mousePos.y - imageRectMin.y) / imageSize.y;

                // Check if Ctrl key is held
                bool ctrlHeld = ImGui::GetIO().KeyCtrl;

                // Zone drawing: Only with Ctrl+Left-click
                if (ctrlHeld)
                {
                    // Ctrl+Left-click: Start drawing a new rectangle for the selected color zone
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    {
                        dragStartX = mouseX;
                        dragStartY = mouseY;
                        drawingZoneIndex = activeZoneColorIndex; // Drawing for the selected color
                    }

                    // Ctrl+Left-drag: Update rectangle being drawn and show preview
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && drawingZoneIndex >= 0 &&
                        ctrlHeld)
                    {
                        float dragEndX = mouseX;
                        float dragEndY = mouseY;

                        // Calculate rectangle from drag start to current position
                        float zx = juce::jmin(dragStartX, dragEndX);
                        float zy = juce::jmin(dragStartY, dragEndY);
                        float zw = std::abs(dragEndX - dragStartX);
                        float zh = std::abs(dragEndY - dragStartY);

                        // Clamp to image bounds
                        zx = juce::jlimit(0.0f, 1.0f, zx);
                        zy = juce::jlimit(0.0f, 1.0f, zy);
                        zw = juce::jlimit(0.01f, 1.0f - zx, zw);
                        zh = juce::jlimit(0.01f, 1.0f - zy, zh);

                        // Draw preview rectangle
                        ImVec2 previewMin(
                            imageRectMin.x + zx * imageSize.x, imageRectMin.y + zy * imageSize.y);
                        ImVec2 previewMax(
                            imageRectMin.x + (zx + zw) * imageSize.x,
                            imageRectMin.y + (zy + zh) * imageSize.y);

                        ImVec4 previewColor = ZONE_COLORS[drawingZoneIndex];
                        ImU32  previewFillColor = ImGui::ColorConvertFloat4ToU32(previewColor);
                        ImU32  previewBorderColor = ImGui::ColorConvertFloat4ToU32(
                            ImVec4(previewColor.x, previewColor.y, previewColor.z, 1.0f));

                        drawList->AddRectFilled(previewMin, previewMax, previewFillColor);
                        drawList->AddRect(
                            previewMin, previewMax, previewBorderColor, 0.0f, 0, 2.0f);
                    }

                    // Ctrl+Left-release: Finish drawing - add rectangle to the color zone
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && drawingZoneIndex >= 0)
                    {
                        float dragEndX = mouseX;
                        float dragEndY = mouseY;

                        // Calculate final rectangle
                        float zx = juce::jmin(dragStartX, dragEndX);
                        float zy = juce::jmin(dragStartY, dragEndY);
                        float zw = std::abs(dragEndX - dragStartX);
                        float zh = std::abs(dragEndY - dragStartY);

                        // Only add if rectangle is large enough
                        if (zw > 0.01f && zh > 0.01f)
                        {
                            // Clamp to image bounds
                            zx = juce::jlimit(0.0f, 1.0f, zx);
                            zy = juce::jlimit(0.0f, 1.0f, zy);
                            zw = juce::jlimit(0.01f, 1.0f - zx, zw);
                            zh = juce::jlimit(0.01f, 1.0f - zy, zh);

                            // Load existing rectangles for this color
                            std::vector<ZoneRect> rects;
                            loadZoneRects(drawingZoneIndex, rects);

                            // Add new rectangle
                            ZoneRect newRect;
                            newRect.x = zx;
                            newRect.y = zy;
                            newRect.width = zw;
                            newRect.height = zh;
                            rects.push_back(newRect);

                            // Save back to APVTS
                            saveZoneRects(drawingZoneIndex, rects);
                            onModificationEnded();
                        }

                        drawingZoneIndex = -1;
                    }
                }

                // Right-drag: Eraser mode (delete rectangles from color zones) - works regardless
                // of Ctrl
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                {
                    // Check if mouse is inside any rectangle of any color zone
                    for (int colorIdx = 0; colorIdx < 4; ++colorIdx)
                    {
                        std::vector<ZoneRect> rects;
                        loadZoneRects(colorIdx, rects);

                        // Check each rectangle and remove if mouse is inside
                        bool modified = false;
                        for (auto it = rects.begin(); it != rects.end();)
                        {
                            bool inside =
                                (mouseX >= it->x && mouseX <= it->x + it->width &&
                                 mouseY >= it->y && mouseY <= it->y + it->height);

                            if (inside)
                            {
                                it = rects.erase(it);
                                modified = true;
                            }
                            else
                            {
                                ++it;
                            }
                        }

                        if (modified)
                        {
                            saveZoneRects(colorIdx, rects);
                            onModificationEnded();
                        }
                    }
                }

                // Show tooltip for zone drawing hints
                ImGui::BeginTooltip();
                ImGui::TextDisabled("Ctrl+Left-drag: Draw zone\nRight-drag: Erase zone");
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::PopItemWidth();
}

void HandTrackerModule::drawIoPins(const NodePinHelpers& helpers)
{
    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    // Widgets like Selectable use WorkRect.Max.x which is the entire canvas in ImNodes,
    // causing them to extend beyond node bounds and triggering when mouse is outside node.
    // Solution: Temporarily constrain WorkRect and ContentRegionRect to node width.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float  cursorX = ImGui::GetCursorPosX();
    const float  nodeWidth = getCustomNodeSize().x > 0 ? getCustomNodeSize().x : 240.0f;
    const float  nodeRightEdge = cursorX + nodeWidth;

    // Save original values
    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;

    // Constrain to node width
    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    // Input pin (always shown)
    helpers.drawAudioInputPin("Source In", 0);

    // Define finger groups with their keypoint ranges
    // Keypoint 0 = Wrist, 1-4 = Thumb, 5-8 = Index, 9-12 = Middle, 13-16 = Ring, 17-20 = Pinky
    struct FingerGroup
    {
        const char* name;
        int         startKeypoint; // Index in HAND_NAMES (0=Wrist, 1-4=Thumb, etc.)
        int         count;         // Number of keypoints in this group
    };

    const FingerGroup groups[] = {
        {"Wrist", 0, 1},
        {"Thumb", 1, 4},
        {"Index", 5, 4},
        {"Middle", 9, 4},
        {"Ring", 13, 4},
        {"Pinky", 17, 4}};
    constexpr int numGroups = 6;

    // Static collapse state per node instance - use logical ID as key
    static std::map<juce::uint32, std::array<bool, numGroups>> collapseState;
    juce::uint32                                               nodeId = getLogicalId();
    if (collapseState.find(nodeId) == collapseState.end())
        collapseState[nodeId] = {
            false, true, true, true, true, true}; // Wrist expanded, others collapsed

    auto& collapsed = collapseState[nodeId];

    for (int g = 0; g < numGroups; ++g)
    {
        const auto& group = groups[g];

        // Check if any pin in this group has connections
        bool hasConnectionsInGroup = false;
        if (helpers.isOutputPinConnected)
        {
            for (int i = 0; i < group.count; ++i)
            {
                int kp = group.startKeypoint + i;
                int chX = kp * 2;
                int chY = kp * 2 + 1;
                if (helpers.isOutputPinConnected(0, chX) || helpers.isOutputPinConnected(0, chY))
                {
                    hasConnectionsInGroup = true;
                    break;
                }
            }
        }

        ImGui::PushID(g);

        // Build header label with arrow indicator and connection indicator
        juce::String headerLabel;
        headerLabel << (collapsed[g] ? "\xE2\x96\xB6 " : "\xE2\x96\xBC "); // ▶ or ▼
        headerLabel << group.name;
        if (collapsed[g] && hasConnectionsInGroup)
            headerLabel << " [\xE2\x97\x8F]"; // ●

        // Simple selectable button for toggle (doesn't bleed like TreeNodeEx)
        if (ImGui::Selectable(headerLabel.toRawUTF8(), false, ImGuiSelectableFlags_None))
        {
            collapsed[g] = !collapsed[g];
        }

        if (!collapsed[g])
        {
            // Draw ALL pins in this group when expanded
            for (int i = 0; i < group.count; ++i)
            {
                int          kp = group.startKeypoint + i;
                const char*  suffix = (kp == 0) ? " (Abs)" : " (Rel)";
                juce::String xLabel = juce::String(HAND_NAMES[kp]) + " X" + suffix;
                juce::String yLabel = juce::String(HAND_NAMES[kp]) + " Y" + suffix;

                int chX = kp * 2;
                int chY = kp * 2 + 1;

                helpers.drawAudioOutputPin(xLabel.toRawUTF8(), chX);
                helpers.drawAudioOutputPin(yLabel.toRawUTF8(), chY);
            }
        }
        else
        {
            // Draw ONLY connected pins when collapsed (smart masking)
            if (helpers.isOutputPinConnected)
            {
                for (int i = 0; i < group.count; ++i)
                {
                    int         kp = group.startKeypoint + i;
                    const char* suffix = (kp == 0) ? " (Abs)" : " (Rel)";

                    int chX = kp * 2;
                    int chY = kp * 2 + 1;

                    if (helpers.isOutputPinConnected(0, chX))
                    {
                        juce::String xLabel = juce::String(HAND_NAMES[kp]) + " X" + suffix;
                        helpers.drawAudioOutputPin(xLabel.toRawUTF8(), chX);
                    }
                    if (helpers.isOutputPinConnected(0, chY))
                    {
                        juce::String yLabel = juce::String(HAND_NAMES[kp]) + " Y" + suffix;
                        helpers.drawAudioOutputPin(yLabel.toRawUTF8(), chY);
                    }
                }
            }
        }
        ImGui::PopID();
    }

    // Zone gates (always visible, no collapse needed - only 4 pins)
    ImGui::Spacing();
    helpers.drawAudioOutputPin("Red Zone Gate", 42);
    helpers.drawAudioOutputPin("Green Zone Gate", 43);
    helpers.drawAudioOutputPin("Blue Zone Gate", 44);
    helpers.drawAudioOutputPin("Yellow Zone Gate", 45);

    // Video outputs (always visible)
    ImGui::Spacing();
    helpers.drawAudioOutputPin("Video Out", 46);   // Using channel 46 to avoid conflict
    helpers.drawAudioOutputPin("Cropped Out", 47); // Using channel 47 to avoid conflict

    // === RESTORE WORKRECT VALUES ===
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}

#endif

void HandTrackerModule::updateGuiFrame(const cv::Mat& frame)
{
    cv::Mat bgra;
    cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);
    const juce::ScopedLock lock(imageLock);
    if (latestFrameForGui.isNull() || latestFrameForGui.getWidth() != bgra.cols ||
        latestFrameForGui.getHeight() != bgra.rows)
        latestFrameForGui = juce::Image(juce::Image::ARGB, bgra.cols, bgra.rows, true);
    juce::Image::BitmapData dest(latestFrameForGui, juce::Image::BitmapData::writeOnly);
    memcpy(dest.data, bgra.data, bgra.total() * bgra.elemSize());
}

juce::Image HandTrackerModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

// Serialize zone rectangles to string: "x1,y1,w1,h1;x2,y2,w2,h2;..."
juce::String HandTrackerModule::serializeZoneRects(const std::vector<ZoneRect>& rects)
{
    juce::String result;
    for (size_t i = 0; i < rects.size(); ++i)
    {
        if (i > 0)
            result += ";";
        result += juce::String(rects[i].x, 4) + "," + juce::String(rects[i].y, 4) + "," +
                  juce::String(rects[i].width, 4) + "," + juce::String(rects[i].height, 4);
    }
    return result;
}

// Deserialize zone rectangles from string
std::vector<HandTrackerModule::ZoneRect> HandTrackerModule::deserializeZoneRects(
    const juce::String& data)
{
    std::vector<ZoneRect> rects;
    if (data.isEmpty())
        return rects;

    juce::StringArray rectStrings;
    rectStrings.addTokens(data, ";", "");

    for (const auto& rectStr : rectStrings)
    {
        juce::StringArray coords;
        coords.addTokens(rectStr, ",", "");
        if (coords.size() == 4)
        {
            ZoneRect rect;
            rect.x = coords[0].getFloatValue();
            rect.y = coords[1].getFloatValue();
            rect.width = coords[2].getFloatValue();
            rect.height = coords[3].getFloatValue();
            rects.push_back(rect);
        }
    }
    return rects;
}

// Load zone rectangles for a color from APVTS state tree
void HandTrackerModule::loadZoneRects(int colorIndex, std::vector<ZoneRect>& rects) const
{
    juce::String key = "zone_color_" + juce::String(colorIndex) + "_rects";
    juce::var    value = apvts.state.getProperty(key);
    if (value.isString())
    {
        rects = deserializeZoneRects(value.toString());
    }
    else
    {
        rects.clear();
    }
}

// Save zone rectangles for a color to APVTS state tree
void HandTrackerModule::saveZoneRects(int colorIndex, const std::vector<ZoneRect>& rects)
{
    juce::String key = "zone_color_" + juce::String(colorIndex) + "_rects";
    juce::String data = serializeZoneRects(rects);
    apvts.state.setProperty(key, data, nullptr);
}

std::vector<DynamicPinInfo> HandTrackerModule::getDynamicOutputPins() const
{
    // Bus 0: CV Out (46 channels - 42 existing + 4 zone gates)
    //   Channels 0-1: Wrist X/Y (absolute screen position)
    //   Channels 2-41: Other keypoints X/Y (relative to wrist)
    //   Channels 42-45: Zone gates
    // Bus 1: Video Out (1 channel)
    // Bus 2: Cropped Out (1 channel)
    std::vector<DynamicPinInfo> pins;

    // Hand keypoint names matching PinDatabase
    const char* handNames[21] = {
        "Wrist",   "Thumb 1", "Thumb 2",  "Thumb 3",  "Thumb 4",  "Index 1",  "Index 2",
        "Index 3", "Index 4", "Middle 1", "Middle 2", "Middle 3", "Middle 4", "Ring 1",
        "Ring 2",  "Ring 3",  "Ring 4",   "Pinky 1",  "Pinky 2",  "Pinky 3",  "Pinky 4"};

    // Add wrist pins (absolute position)
    pins.emplace_back("Wrist X (Abs)", 0, PinDataType::CV);
    pins.emplace_back("Wrist Y (Abs)", 1, PinDataType::CV);

    // Add all other keypoint pins (relative to wrist)
    for (int i = 1; i < 21; ++i)
    {
        pins.emplace_back(std::string(handNames[i]) + " X (Rel)", i * 2, PinDataType::CV);
        pins.emplace_back(std::string(handNames[i]) + " Y (Rel)", i * 2 + 1, PinDataType::CV);
    }

    // Add zone gate pins (channels 42-45)
    pins.emplace_back("Red Zone Gate", 42, PinDataType::Gate);
    pins.emplace_back("Green Zone Gate", 43, PinDataType::Gate);
    pins.emplace_back("Blue Zone Gate", 44, PinDataType::Gate);
    pins.emplace_back("Yellow Zone Gate", 45, PinDataType::Gate);

    // Add Video Out and Cropped Out pins (bus 1 and 2, not CV channels)
    pins.emplace_back("Video Out", 0, PinDataType::Video);   // Bus 1, channel 0
    pins.emplace_back("Cropped Out", 1, PinDataType::Video); // Bus 2, channel 1

    return pins;
}
