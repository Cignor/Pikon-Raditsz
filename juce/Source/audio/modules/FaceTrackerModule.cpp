#include "FaceTrackerModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include "../../utils/CudaDeviceCountCache.h"
#include <opencv2/imgproc.hpp>

#if defined(PRESET_CREATOR_UI)
#include <imgui_internal.h> // For WorkRect workaround to fix widget bleeding in nodes
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include <juce_opengl/juce_opengl.h>
#include <map>
#include <array>
#include <unordered_map>
#endif

juce::AudioProcessorValueTreeState::ParameterLayout FaceTrackerModule::createParameterLayout()
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

FaceTrackerModule::FaceTrackerModule()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::mono(), true)
              .withOutput(
                  "CV Out",
                  juce::AudioChannelSet::discreteChannels(40),
                  true) // 36 existing + 4 zone gates
              .withOutput("Video Out", juce::AudioChannelSet::mono(), true)     // PASSTHROUGH
              .withOutput("Cropped Out", juce::AudioChannelSet::mono(), true)), // CROPPED
      juce::Thread("Face Tracker Thread"),
      apvts(*this, nullptr, "FaceTrackerParams", createParameterLayout())
{
    sourceIdParam = apvts.getRawParameterValue("sourceId");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    confidenceThresholdParam = apvts.getRawParameterValue("confidence");
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));
    fifoBuffer.resize(16);
}

FaceTrackerModule::~FaceTrackerModule() { stopThread(5000); }

void FaceTrackerModule::prepareToPlay(double, int) { startThread(juce::Thread::Priority::normal); }

void FaceTrackerModule::releaseResources()
{
    signalThreadShouldExit();
    stopThread(5000);
}

void FaceTrackerModule::loadModel()
{
    auto appDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
    auto assetsDir = appDir.getChildFile("assets");
    auto faceDir = assetsDir.getChildFile("openpose_models").getChildFile("face");
    auto haarPath = faceDir.getChildFile("haarcascade_frontalface_alt.xml").getFullPathName();
    auto protoPath = faceDir.getChildFile("pose_deploy.prototxt").getFullPathName();
    auto modelPath = faceDir.getChildFile("pose_iter_116000.caffemodel").getFullPathName();
    bool ok = faceCascade.load(haarPath.toStdString());
    if (ok && juce::File(protoPath).existsAsFile() && juce::File(modelPath).existsAsFile())
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
                juce::Logger::writeToLog("[FaceTracker] ✓ Model loaded with CUDA backend (GPU)");
            }
            else
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                juce::Logger::writeToLog("[FaceTracker] Model loaded with CPU backend");
            }
#else
            net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            juce::Logger::writeToLog(
                "[FaceTracker] Model loaded with CPU backend (CUDA not compiled)");
#endif

            modelLoaded = true;
        }
        catch (...)
        {
            modelLoaded = false;
        }
    }
}

void FaceTrackerModule::run()
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
                    "[FaceTracker] WARNING: GPU requested but no CUDA device found. Using CPU.");
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
                juce::Logger::writeToLog("[FaceTracker] ✓ Switched to CUDA backend (GPU)");
            }
            else
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                juce::Logger::writeToLog("[FaceTracker] Switched to CPU backend");
            }
            lastGpuState = useGpu;
        }
#endif

        // Save a clean copy for cropping (before drawing annotations)
        cv::Mat originalFrame;
        frame.copyTo(originalFrame);

        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        std::vector<cv::Rect> faces;
        faceCascade.detectMultiScale(gray, faces);
        FaceResult result{};
        // Initialize zone hits to false
        for (int z = 0; z < 4; ++z)
            result.zoneHits[z] = false;

        if (!faces.empty())
        {
            cv::Rect box = faces[0];

            // --- CROPPED OUTPUT LOGIC ---
            cv::Rect validBox = box & cv::Rect(0, 0, originalFrame.cols, originalFrame.rows);
            if (validBox.area() > 0)
            {
                cv::Mat cropped = originalFrame(validBox);
                VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), cropped);
            }

            cv::Mat roi = frame(box);
            // NOTE: For DNN models, blobFromImage works on CPU
            // The GPU acceleration happens in net.forward() when backend is set to CUDA
            cv::Mat blob = cv::dnn::blobFromImage(
                roi, 1.0 / 255.0, cv::Size(368, 368), cv::Scalar(), false, false);
            net.setInput(blob);
            // Forward pass (GPU-accelerated if backend is CUDA)
            cv::Mat out = net.forward();
            parseFaceOutput(out, box, result);

            // Check zone hits: check if face center OR ANY detected keypoint is inside a zone
            if (result.detectedPoints > 0 || (result.faceCenterX >= 0 && result.faceCenterY >= 0))
            {
                for (int colorIdx = 0; colorIdx < 4; ++colorIdx)
                {
                    std::vector<ZoneRect> rects;
                    loadZoneRects(colorIdx, rects);

                    bool hit = false;

                    // First check face center (if valid)
                    if (result.faceCenterX >= 0 && result.faceCenterY >= 0)
                    {
                        float posX = result.faceCenterX / (float)frame.cols;
                        float posY = result.faceCenterY / (float)frame.rows;

                        for (const auto& rect : rects)
                        {
                            if (posX >= rect.x && posX <= rect.x + rect.width && posY >= rect.y &&
                                posY <= rect.y + rect.height)
                            {
                                hit = true;
                                break;
                            }
                        }
                    }

                    // Also check each detected keypoint
                    if (!hit)
                    {
                        for (int i = 0; i < FACE_NUM_KEYPOINTS; ++i)
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
                                if (posX >= rect.x && posX <= rect.x + rect.width &&
                                    posY >= rect.y && posY <= rect.y + rect.height)
                                {
                                    hit = true;
                                    break; // Found a keypoint in this zone
                                }
                            }

                            if (hit)
                                break; // Found a hit for this zone, check next zone
                        }
                    }
                    result.zoneHits[colorIdx] = hit;
                }
            }

            // Draw box
            cv::rectangle(frame, box, {0, 255, 0}, 2);
            for (int i = 0; i < FACE_NUM_KEYPOINTS; ++i)
                if (result.keypoints[i][0] >= 0)
                    cv::circle(
                        frame,
                        {(int)result.keypoints[i][0], (int)result.keypoints[i][1]},
                        2,
                        {0, 0, 255},
                        -1);
        }
        else
        {
            // Clear cropped output when no face detected
            cv::Mat emptyFrame;
            VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), emptyFrame);
            // Mark face center as invalid when no face detected
            result.faceCenterX = -1.0f;
            result.faceCenterY = -1.0f;
        }

        if (fifo.getFreeSpace() >= 1)
        {
            auto w = fifo.write(1);
            if (w.blockSize1 > 0)
                fifoBuffer[w.startIndex1] = result;
        }

        // Resolve logical ID continuously (handles XML load timing)
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

        // Update GUI preview regardless of passthrough availability
        updateGuiFrame(frame);

        // --- PASSTHROUGH LOGIC ---
        if (myLogicalId != 0)
            VideoFrameManager::getInstance().setFrame(myLogicalId, frame);
        wait(66);
    }
}

void FaceTrackerModule::parseFaceOutput(
    const cv::Mat&  netOutput,
    const cv::Rect& faceBox,
    FaceResult&     result)
{
    int   H = netOutput.size[2], W = netOutput.size[3];
    float thresh = confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f;
    result.detectedPoints = 0;
    int count = juce::jmin(FACE_NUM_KEYPOINTS, netOutput.size[1]);

    // Calculate face center from bounding box (this is our reference point)
    result.faceCenterX = faceBox.x + faceBox.width * 0.5f;
    result.faceCenterY = faceBox.y + faceBox.height * 0.5f;

    for (int i = 0; i < count; ++i)
    {
        cv::Mat   heat(H, W, CV_32F, (void*)netOutput.ptr<float>(0, i));
        double    mv;
        cv::Point ml;
        cv::minMaxLoc(heat, nullptr, &mv, nullptr, &ml);
        if (mv > thresh)
        {
            result.keypoints[i][0] = (float)faceBox.x + (float)ml.x * faceBox.width / W;
            result.keypoints[i][1] = (float)faceBox.y + (float)ml.y * faceBox.height / H;
            result.detectedPoints++;
        }
        else
        {
            result.keypoints[i][0] = -1.0f;
            result.keypoints[i][1] = -1.0f;
        }
    }
}

void FaceTrackerModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
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

    // Get face center position (our reference point)
    float faceCenterX = lastResultForAudio.faceCenterX;
    float faceCenterY = lastResultForAudio.faceCenterY;

    // Normalization factors (using typical frame size - actual frame size could vary)
    // These are used to normalize relative offsets to a reasonable 0.0-1.0 range
    const float normScaleX = 1.0f / 640.0f; // Normalize by typical frame width
    const float normScaleY = 1.0f / 480.0f; // Normalize by typical frame height

    // Helper function to output a keypoint relative to face center
    auto outputKeypoint = [&](int keypointIndex, int channelStart, int numSamples) {
        int chX = channelStart;
        int chY = channelStart + 1;

        if (faceCenterX >= 0 && faceCenterY >= 0 && keypointIndex >= 0 &&
            keypointIndex < FACE_NUM_KEYPOINTS &&
            lastResultForAudio.keypoints[keypointIndex][0] >= 0 &&
            lastResultForAudio.keypoints[keypointIndex][1] >= 0)
        {
            // Calculate relative offset from face center
            float relX =
                (lastResultForAudio.keypoints[keypointIndex][0] - faceCenterX) * normScaleX;
            float relY =
                (lastResultForAudio.keypoints[keypointIndex][1] - faceCenterY) * normScaleY;

            // Map relative offset to 0.0-1.0 range where 0.5 = no offset (centered at face center)
            const float relScale = 2.5f; // Scale factor to make relative positions more usable
            float       relX_norm = juce::jlimit(0.0f, 1.0f, 0.5f + relX * relScale);
            float       relY_norm = juce::jlimit(0.0f, 1.0f, 0.5f + relY * relScale);

            for (int s = 0; s < numSamples; ++s)
            {
                if (chX < cvOutBus.getNumChannels())
                    cvOutBus.setSample(chX, s, relX_norm);
                if (chY < cvOutBus.getNumChannels())
                    cvOutBus.setSample(chY, s, relY_norm);
            }
        }
        else
        {
            // No face center or keypoint not detected - output center value (0.5 = no offset)
            for (int s = 0; s < numSamples; ++s)
            {
                if (chX < cvOutBus.getNumChannels())
                    cvOutBus.setSample(chX, s, 0.5f);
                if (chY < cvOutBus.getNumChannels())
                    cvOutBus.setSample(chY, s, 0.5f);
            }
        }
    };

    int numSamples = cvOutBus.getNumSamples();

    // Output face center absolute position (channels 0, 1)
    float faceCenterX_norm =
        (faceCenterX >= 0) ? juce::jlimit(0.0f, 1.0f, faceCenterX * normScaleX) : 0.5f;
    float faceCenterY_norm =
        (faceCenterY >= 0) ? juce::jlimit(0.0f, 1.0f, faceCenterY * normScaleY) : 0.5f;
    for (int s = 0; s < numSamples; ++s)
    {
        cvOutBus.setSample(0, s, faceCenterX_norm); // Face Center X (absolute)
        cvOutBus.setSample(1, s, faceCenterY_norm); // Face Center Y (absolute)
    }

    // Output Nose Base (index 32) - channels 2, 3
    outputKeypoint(32, 2, numSamples);

    // Output Right Eye: Outer(36), Top(37), Inner(38), Bottom(39) - channels 4-11
    outputKeypoint(36, 4, numSamples);  // R Eye Outer
    outputKeypoint(37, 6, numSamples);  // R Eye Top
    outputKeypoint(38, 8, numSamples);  // R Eye Inner
    outputKeypoint(39, 10, numSamples); // R Eye Bottom

    // Output Left Eye: Inner(42), Top(43), Outer(44), Bottom(45) - channels 12-19
    outputKeypoint(42, 12, numSamples); // L Eye Inner
    outputKeypoint(43, 14, numSamples); // L Eye Top
    outputKeypoint(44, 16, numSamples); // L Eye Outer
    outputKeypoint(45, 18, numSamples); // L Eye Bottom

    // Output Mouth: Corner R(48), Top Center(51), Corner L(54), Bottom Center(57) - channels 20-27
    outputKeypoint(48, 20, numSamples); // Mouth Corner R
    outputKeypoint(51, 22, numSamples); // Mouth Top Center
    outputKeypoint(54, 24, numSamples); // Mouth Corner L
    outputKeypoint(57, 26, numSamples); // Mouth Bottom Center

    // Output Eyebrows: R Outer(17), R Inner(21), L Inner(22), L Outer(26) - channels 28-35
    outputKeypoint(17, 28, numSamples); // R Eyebrow Outer
    outputKeypoint(21, 30, numSamples); // R Eyebrow Inner
    outputKeypoint(22, 32, numSamples); // L Eyebrow Inner
    outputKeypoint(26, 34, numSamples); // L Eyebrow Outer

    // Output zone gates (channels 36-39)
    for (int z = 0; z < 4; ++z)
    {
        int ch = 36 + z;
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
ImVec2 FaceTrackerModule::getCustomNodeSize() const
{
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void FaceTrackerModule::drawParametersInNode(
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
            "Enable GPU acceleration for face tracking.\nRequires CUDA-capable NVIDIA GPU.");
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

    bool        confMod = isParamModulated("confidence");
    const float confFallback = confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f;
    float       conf = confMod ? getLiveParamValue("confidence", confFallback) : confFallback;
    if (confMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Confidence", &conf, 0.0f, 1.0f, "%.2f"))
    {
        if (!confMod)
        {
            if (auto* param =
                    dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("confidence")))
                *param = conf;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !confMod)
        onModificationEnded();
    if (!confMod)
        adjustParamOnWheel(apvts.getParameter("confidence"), "confidence", conf);
    if (confMod)
        ImGui::EndDisabled();
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
            const FaceResult& uiResult = lastResultForAudio;

            // Draw a small red dot at face center (if valid)
            ImU32 redColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            if (uiResult.faceCenterX >= 0 && uiResult.faceCenterY >= 0)
            {
                float posX = uiResult.faceCenterX / (float)frame.getWidth();
                float posY = uiResult.faceCenterY / (float)frame.getHeight();

                float  centerX = imageRectMin.x + posX * imageSize.x;
                float  centerY = imageRectMin.y + posY * imageSize.y;
                ImVec2 center(centerX, centerY);

                // Draw face center (slightly larger dot, 4 pixels)
                drawList->AddCircleFilled(center, 4.0f, redColor);
            }

            // Draw a small red dot at each detected keypoint
            for (int i = 0; i < FACE_NUM_KEYPOINTS; ++i)
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

                    // Draw small red dot (radius 2 pixels)
                    drawList->AddCircleFilled(center, 2.0f, redColor);
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

void FaceTrackerModule::drawIoPins(const NodePinHelpers& helpers)
{
    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float  cursorX = ImGui::GetCursorPosX();
    const float  nodeWidth = getCustomNodeSize().x > 0 ? getCustomNodeSize().x : 240.0f;
    const float  nodeRightEdge = cursorX + nodeWidth;

    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;

    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    // Input pin
    helpers.drawAudioInputPin("Source In", 0);

    // Define face feature groups based on getDynamicOutputPins channel layout
    struct FaceFeatureGroup
    {
        const char*                              name;
        std::vector<std::pair<const char*, int>> pins; // {label, channel}
    };

    const FaceFeatureGroup groups[] = {
        {"Face Center", {{"Face Center X (Abs)", 0}, {"Face Center Y (Abs)", 1}}},
        {"Nose", {{"Nose Base X (Rel)", 2}, {"Nose Base Y (Rel)", 3}}},
        {"Right Eye",
         {{"R Eye Outer X (Rel)", 4},
          {"R Eye Outer Y (Rel)", 5},
          {"R Eye Top X (Rel)", 6},
          {"R Eye Top Y (Rel)", 7},
          {"R Eye Inner X (Rel)", 8},
          {"R Eye Inner Y (Rel)", 9},
          {"R Eye Bottom X (Rel)", 10},
          {"R Eye Bottom Y (Rel)", 11}}},
        {"Left Eye",
         {{"L Eye Inner X (Rel)", 12},
          {"L Eye Inner Y (Rel)", 13},
          {"L Eye Top X (Rel)", 14},
          {"L Eye Top Y (Rel)", 15},
          {"L Eye Outer X (Rel)", 16},
          {"L Eye Outer Y (Rel)", 17},
          {"L Eye Bottom X (Rel)", 18},
          {"L Eye Bottom Y (Rel)", 19}}},
        {"Mouth",
         {{"Mouth Corner R X (Rel)", 20},
          {"Mouth Corner R Y (Rel)", 21},
          {"Mouth Top Center X (Rel)", 22},
          {"Mouth Top Center Y (Rel)", 23},
          {"Mouth Corner L X (Rel)", 24},
          {"Mouth Corner L Y (Rel)", 25},
          {"Mouth Bottom Center X (Rel)", 26},
          {"Mouth Bottom Center Y (Rel)", 27}}},
        {"Eyebrows",
         {{"R Eyebrow Outer X (Rel)", 28},
          {"R Eyebrow Outer Y (Rel)", 29},
          {"R Eyebrow Inner X (Rel)", 30},
          {"R Eyebrow Inner Y (Rel)", 31},
          {"L Eyebrow Inner X (Rel)", 32},
          {"L Eyebrow Inner Y (Rel)", 33},
          {"L Eyebrow Outer X (Rel)", 34},
          {"L Eyebrow Outer Y (Rel)", 35}}}};
    constexpr int numGroups = 6;

    static std::map<juce::uint32, std::array<bool, numGroups>> collapseState;
    juce::uint32                                               nodeId = getLogicalId();
    if (collapseState.find(nodeId) == collapseState.end())
        collapseState[nodeId] = {false, true, true, true, true, true}; // Face Center expanded

    auto& collapsed = collapseState[nodeId];

    for (int g = 0; g < numGroups; ++g)
    {
        const auto& group = groups[g];

        bool hasConnectionsInGroup = false;
        if (helpers.isOutputPinConnected)
        {
            for (const auto& pin : group.pins)
            {
                if (helpers.isOutputPinConnected(0, pin.second))
                {
                    hasConnectionsInGroup = true;
                    break;
                }
            }
        }

        ImGui::PushID(g);

        juce::String headerLabel;
        headerLabel << (collapsed[g] ? "\xE2\x96\xB6 " : "\xE2\x96\xBC ");
        headerLabel << group.name;
        if (collapsed[g] && hasConnectionsInGroup)
            headerLabel << " [\xE2\x97\x8F]";

        if (ImGui::Selectable(headerLabel.toRawUTF8(), false))
            collapsed[g] = !collapsed[g];

        if (!collapsed[g])
        {
            for (const auto& pin : group.pins)
                helpers.drawAudioOutputPin(pin.first, pin.second);
        }
        else if (helpers.isOutputPinConnected)
        {
            for (const auto& pin : group.pins)
            {
                if (helpers.isOutputPinConnected(0, pin.second))
                    helpers.drawAudioOutputPin(pin.first, pin.second);
            }
        }
        ImGui::PopID();
    }

    // Zone gates (always visible)
    ImGui::Spacing();
    helpers.drawAudioOutputPin("Red Zone Gate", 36);
    helpers.drawAudioOutputPin("Green Zone Gate", 37);
    helpers.drawAudioOutputPin("Blue Zone Gate", 38);
    helpers.drawAudioOutputPin("Yellow Zone Gate", 39);

    // Video outputs (always visible)
    ImGui::Spacing();
    helpers.drawAudioOutputPin("Video Out", 40);
    helpers.drawAudioOutputPin("Cropped Out", 41);

    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}

#endif

void FaceTrackerModule::updateGuiFrame(const cv::Mat& frame)
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

juce::Image FaceTrackerModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

// Serialize zone rectangles to string: "x1,y1,w1,h1;x2,y2,w2,h2;..."
juce::String FaceTrackerModule::serializeZoneRects(const std::vector<ZoneRect>& rects)
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
std::vector<FaceTrackerModule::ZoneRect> FaceTrackerModule::deserializeZoneRects(
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
void FaceTrackerModule::loadZoneRects(int colorIndex, std::vector<ZoneRect>& rects) const
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
void FaceTrackerModule::saveZoneRects(int colorIndex, const std::vector<ZoneRect>& rects)
{
    juce::String key = "zone_color_" + juce::String(colorIndex) + "_rects";
    juce::String data = serializeZoneRects(rects);
    apvts.state.setProperty(key, data, nullptr);
}

std::vector<DynamicPinInfo> FaceTrackerModule::getDynamicOutputPins() const
{
    // Bus 0: CV Out (40 channels - 36 existing + 4 zone gates)
    //   Channels 0-1: Face Center X/Y (absolute screen position)
    //   Channels 2-3: Nose Base (relative to face center)
    //   Channels 4-11: Right Eye (4 points: Outer, Top, Inner, Bottom)
    //   Channels 12-19: Left Eye (4 points: Inner, Top, Outer, Bottom)
    //   Channels 20-27: Mouth (4 points: Corner R, Top Center, Corner L, Bottom Center)
    //   Channels 28-35: Eyebrows (4 points: R Outer, R Inner, L Inner, L Outer)
    //   Channels 36-39: Zone gates
    // Bus 1: Video Out (1 channel) - PASSTHROUGH
    // Bus 2: Cropped Out (1 channel) - CROPPED
    std::vector<DynamicPinInfo> pins;

    // Add face center pins (absolute position)
    pins.emplace_back("Face Center X (Abs)", 0, PinDataType::CV);
    pins.emplace_back("Face Center Y (Abs)", 1, PinDataType::CV);

    // Nose Base (channels 2, 3)
    pins.emplace_back("Nose Base X (Rel)", 2, PinDataType::CV);
    pins.emplace_back("Nose Base Y (Rel)", 3, PinDataType::CV);

    // Right Eye (channels 4-11)
    pins.emplace_back("R Eye Outer X (Rel)", 4, PinDataType::CV);
    pins.emplace_back("R Eye Outer Y (Rel)", 5, PinDataType::CV);
    pins.emplace_back("R Eye Top X (Rel)", 6, PinDataType::CV);
    pins.emplace_back("R Eye Top Y (Rel)", 7, PinDataType::CV);
    pins.emplace_back("R Eye Inner X (Rel)", 8, PinDataType::CV);
    pins.emplace_back("R Eye Inner Y (Rel)", 9, PinDataType::CV);
    pins.emplace_back("R Eye Bottom X (Rel)", 10, PinDataType::CV);
    pins.emplace_back("R Eye Bottom Y (Rel)", 11, PinDataType::CV);

    // Left Eye (channels 12-19)
    pins.emplace_back("L Eye Inner X (Rel)", 12, PinDataType::CV);
    pins.emplace_back("L Eye Inner Y (Rel)", 13, PinDataType::CV);
    pins.emplace_back("L Eye Top X (Rel)", 14, PinDataType::CV);
    pins.emplace_back("L Eye Top Y (Rel)", 15, PinDataType::CV);
    pins.emplace_back("L Eye Outer X (Rel)", 16, PinDataType::CV);
    pins.emplace_back("L Eye Outer Y (Rel)", 17, PinDataType::CV);
    pins.emplace_back("L Eye Bottom X (Rel)", 18, PinDataType::CV);
    pins.emplace_back("L Eye Bottom Y (Rel)", 19, PinDataType::CV);

    // Mouth (channels 20-27)
    pins.emplace_back("Mouth Corner R X (Rel)", 20, PinDataType::CV);
    pins.emplace_back("Mouth Corner R Y (Rel)", 21, PinDataType::CV);
    pins.emplace_back("Mouth Top Center X (Rel)", 22, PinDataType::CV);
    pins.emplace_back("Mouth Top Center Y (Rel)", 23, PinDataType::CV);
    pins.emplace_back("Mouth Corner L X (Rel)", 24, PinDataType::CV);
    pins.emplace_back("Mouth Corner L Y (Rel)", 25, PinDataType::CV);
    pins.emplace_back("Mouth Bottom Center X (Rel)", 26, PinDataType::CV);
    pins.emplace_back("Mouth Bottom Center Y (Rel)", 27, PinDataType::CV);

    // Eyebrows (channels 28-35)
    pins.emplace_back("R Eyebrow Outer X (Rel)", 28, PinDataType::CV);
    pins.emplace_back("R Eyebrow Outer Y (Rel)", 29, PinDataType::CV);
    pins.emplace_back("R Eyebrow Inner X (Rel)", 30, PinDataType::CV);
    pins.emplace_back("R Eyebrow Inner Y (Rel)", 31, PinDataType::CV);
    pins.emplace_back("L Eyebrow Inner X (Rel)", 32, PinDataType::CV);
    pins.emplace_back("L Eyebrow Inner Y (Rel)", 33, PinDataType::CV);
    pins.emplace_back("L Eyebrow Outer X (Rel)", 34, PinDataType::CV);
    pins.emplace_back("L Eyebrow Outer Y (Rel)", 35, PinDataType::CV);

    // Add zone gate pins (channels 36-39)
    pins.emplace_back("Red Zone Gate", 36, PinDataType::Gate);
    pins.emplace_back("Green Zone Gate", 37, PinDataType::Gate);
    pins.emplace_back("Blue Zone Gate", 38, PinDataType::Gate);
    pins.emplace_back("Yellow Zone Gate", 39, PinDataType::Gate);

    // Add Video Out and Cropped Out pins (bus 1 and 2, not CV channels)
    pins.emplace_back("Video Out", 0, PinDataType::Video);   // Bus 1, channel 0
    pins.emplace_back("Cropped Out", 1, PinDataType::Video); // Bus 2, channel 1

    return pins;
}
