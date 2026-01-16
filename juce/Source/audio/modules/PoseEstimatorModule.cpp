#include "PoseEstimatorModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include "../../utils/CudaDeviceCountCache.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>
#if WITH_CUDA_SUPPORT
#include <opencv2/core/cuda.hpp>
#endif

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h> // For WorkRect workaround to fix widget bleeding in nodes
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include "../../preset_creator/theme/ThemeManager.h"
#include <juce_opengl/juce_opengl.h>
#include <map>
#include <array>
#include <unordered_map>
#endif

void PoseEstimatorModule::loadModel(int modelIndex)
{
    // Find assets next to executable (like ObjectDetectorModule)
    auto       exeFile = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    auto       appDir = exeFile.getParentDirectory();
    juce::File assetsDir = appDir.getChildFile("assets");

    auto poseModelsDir = assetsDir.getChildFile("openpose_models").getChildFile("pose");

    juce::String protoPath, modelPath;
    juce::String modelName;

    switch (modelIndex)
    {
    case 0: // BODY_25
        modelName = "BODY_25";
        protoPath = poseModelsDir.getChildFile("body_25/pose_deploy.prototxt").getFullPathName();
        modelPath =
            poseModelsDir.getChildFile("body_25/pose_iter_584000.caffemodel").getFullPathName();
        break;
    case 1: // COCO
        modelName = "COCO";
        protoPath =
            poseModelsDir.getChildFile("coco/pose_deploy_linevec.prototxt").getFullPathName();
        modelPath =
            poseModelsDir.getChildFile("coco/pose_iter_440000.caffemodel").getFullPathName();
        break;
    case 2: // MPI
        modelName = "MPI";
        protoPath =
            poseModelsDir.getChildFile("mpi/pose_deploy_linevec.prototxt").getFullPathName();
        modelPath = poseModelsDir.getChildFile("mpi/pose_iter_160000.caffemodel").getFullPathName();
        break;
    case 3: // MPI (Fast)
    default:
        modelName = "MPI (Fast)";
        protoPath = poseModelsDir.getChildFile("mpi/pose_deploy_linevec_faster_4_stages.prototxt")
                        .getFullPathName();
        modelPath = poseModelsDir.getChildFile("mpi/pose_iter_160000.caffemodel").getFullPathName();
        break;
    }

    juce::Logger::writeToLog("[PoseEstimator] Attempting to load " + modelName + " model...");
    juce::Logger::writeToLog("  - Prototxt: " + protoPath);
    juce::Logger::writeToLog("  - Caffemodel: " + modelPath);

    juce::File protoFile(protoPath);
    juce::File modelFile(modelPath);

    if (protoFile.existsAsFile() && modelFile.existsAsFile())
    {
        try
        {
            net = cv::dnn::readNetFromCaffe(protoPath.toStdString(), modelPath.toStdString());

            // Set backend immediately after loading model (like ObjectDetectorModule)
#if WITH_CUDA_SUPPORT
            bool useGpu = useGpuParam ? useGpuParam->get() : false;
            if (useGpu && CudaDeviceCountCache::isAvailable())
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                juce::Logger::writeToLog("[PoseEstimator] ✓ Model loaded with CUDA backend (GPU)");
            }
            else
            {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                juce::Logger::writeToLog("[PoseEstimator] Model loaded with CPU backend");
            }
#else
            net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            juce::Logger::writeToLog(
                "[PoseEstimator] Model loaded with CPU backend (CUDA not compiled)");
#endif

            modelLoaded = true;
            juce::Logger::writeToLog("[PoseEstimator] SUCCESS: Loaded model: " + modelName);
        }
        catch (const cv::Exception& e)
        {
            juce::Logger::writeToLog(
                "[PoseEstimator] FAILED: OpenCV exception while loading model: " +
                juce::String(e.what()));
            modelLoaded = false;
        }
    }
    else
    {
        juce::Logger::writeToLog(
            "[PoseEstimator] FAILED: Could not find model files at the specified paths.");
        if (!protoFile.existsAsFile())
            juce::Logger::writeToLog("  - Missing file: " + protoPath);
        if (!modelFile.existsAsFile())
            juce::Logger::writeToLog("  - Missing file: " + modelPath);
        modelLoaded = false;
    }
}

juce::ValueTree PoseEstimatorModule::getExtraStateTree() const
{
    juce::ValueTree state("PoseEstimatorState");
    state.setProperty("assetsPath", assetsPath, nullptr);
    return state;
}

void PoseEstimatorModule::setExtraStateTree(const juce::ValueTree& state)
{
    if (state.hasType("PoseEstimatorState"))
    {
        assetsPath = state.getProperty("assetsPath", "").toString();
        if (assetsPath.isNotEmpty())
        {
            requestedModelIndex = modelChoiceParam ? modelChoiceParam->getIndex() : 3;
        }
    }
}

// Network input size for MPI model
constexpr int POSE_NET_WIDTH = 368;
constexpr int POSE_NET_HEIGHT = 368;

juce::AudioProcessorValueTreeState::ParameterLayout PoseEstimatorModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Source ID input (which video source to connect to)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("sourceId", "Source ID", 0.0f, 1000.0f, 0.0f));

    // Model choice (BODY_25, COCO, MPI, MPI Fast)
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "model", "Model", juce::StringArray{"BODY_25", "COCO", "MPI", "MPI (Fast)"}, 3));

    // Model quality (affects blob size)
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "quality", "Quality", juce::StringArray{"Low (Fast)", "Medium (Default)"}, 1));

    // Note: assets path is stored via extra state, not as a parameter

    // Zoom level for UI: 0=Small(240),1=Normal(480),2=Large(960)
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Zoom Level", juce::StringArray{"Small", "Normal", "Large"}, 1));

    // Confidence threshold (0.0 - 1.0) - keypoints below this confidence will be ignored
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("confidence", "Confidence", 0.0f, 1.0f, 0.1f));

    // Toggle skeleton drawing on preview
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("drawSkeleton", "Draw Skeleton", true));

// GPU acceleration toggle - default from global setting
#if defined(PRESET_CREATOR_UI)
    bool defaultGpu = ImGuiNodeEditorComponent::getGlobalGpuEnabled();
#else
    bool defaultGpu = true; // Default to GPU for non-UI builds
#endif
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", defaultGpu));

    return {params.begin(), params.end()};
}

PoseEstimatorModule::PoseEstimatorModule()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Input",
                  juce::AudioChannelSet::discreteChannels(2),
                  true) // Source ID (ch 0) + Confidence Mod (ch 1)
              .withOutput(
                  "CV Out",
                  juce::AudioChannelSet::discreteChannels(34),
                  true) // 30 existing + 4 zone gates
              .withOutput("Video Out", juce::AudioChannelSet::mono(), true)     // PASSTHROUGH
              .withOutput("Cropped Out", juce::AudioChannelSet::mono(), true)), // CROPPED
      juce::Thread("Pose Estimator Thread"),
      apvts(*this, nullptr, "PoseEstimatorParams", createParameterLayout())
{
    // Get parameter pointers
    sourceIdParam = apvts.getRawParameterValue("sourceId");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    qualityParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("quality"));
    modelChoiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("model"));
    confidenceThresholdParam = apvts.getRawParameterValue("confidence");
    drawSkeletonParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("drawSkeleton"));
    useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));

    // Initialize FIFO buffer
    fifoBuffer.resize(16);

    // Defer initial model load to the thread (default to current selection or MPI Fast)
    requestedModelIndex = modelChoiceParam ? modelChoiceParam->getIndex() : 3;

    // Initialize confidence threshold
    currentConfidenceThreshold.store(
        confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f);
}

PoseEstimatorModule::~PoseEstimatorModule() { stopThread(5000); }

void PoseEstimatorModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    startThread(juce::Thread::Priority::normal);
}

void PoseEstimatorModule::releaseResources()
{
    // Just stop the thread - don't destroy resources here (they'll be reused on next prepareToPlay)
    // Only the destructor should do final CUDA cleanup
    signalThreadShouldExit();
    stopThread(5000);
}

void PoseEstimatorModule::run()
{
#if WITH_CUDA_SUPPORT
    bool lastGpuState = false;     // Track GPU state to minimize backend switches
    bool loggedGpuWarning = false; // Only warn once if no GPU available
#endif

    while (!threadShouldExit())
    {
        // Handle deferred model reload requests from UI
        int toLoad = requestedModelIndex.exchange(-1);
        if (toLoad != -1)
        {
            loadModel(toLoad);
        }

        // Load model on first run if not loaded yet
        if (!modelLoaded)
        {
            int defaultModel = modelChoiceParam ? modelChoiceParam->getIndex() : 3;
            loadModel(defaultModel);
        }

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

        if (!frame.empty())
        {
            bool useGpu = false;

#if WITH_CUDA_SUPPORT
            useGpu = useGpuParam ? useGpuParam->get() : false;
            if (useGpu && !CudaDeviceCountCache::isAvailable())
            {
                useGpu = false;
                if (!loggedGpuWarning)
                {
                    juce::Logger::writeToLog(
                        "[PoseEstimator] WARNING: GPU requested but no CUDA device found. Using "
                        "CPU.");
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
                    juce::Logger::writeToLog("[PoseEstimator] ✓ Switched to CUDA backend (GPU)");
                }
                else
                {
                    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                    juce::Logger::writeToLog("[PoseEstimator] Switched to CPU backend");
                }
                lastGpuState = useGpu;
            }
#endif

            // Check for exit before expensive operations
            if (threadShouldExit())
                break;

            // Prepare input blob
            int      q = qualityParam ? qualityParam->getIndex() : 1;
            cv::Size blobSize = (q == 0) ? cv::Size(224, 224) : cv::Size(368, 368);
            cv::Mat  inputBlob = cv::dnn::blobFromImage(
                frame, 1.0 / 255.0, blobSize, cv::Scalar(0, 0, 0), false, false);

            // Check for exit again before CUDA operation
            if (threadShouldExit())
                break;

            net.setInput(inputBlob);

            // Wrap forward() in try-catch to handle CUDA errors gracefully
            cv::Mat netOutput;
            try
            {
                netOutput = net.forward();
            }
            catch (const cv::Exception& e)
            {
                juce::Logger::writeToLog(
                    "[PoseEstimator] CUDA forward() failed: " + juce::String(e.what()) +
                    " - falling back to CPU");
                // Switch to CPU backend and retry once
                try
                {
                    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                    net.setInput(inputBlob);
                    netOutput = net.forward();
                }
                catch (...)
                {
                    wait(50);
                    continue; // Skip this frame if CPU also fails
                }
            }
            catch (...)
            {
                juce::Logger::writeToLog(
                    "[PoseEstimator] Unknown error in net.forward() - skipping frame");
                wait(50);
                continue; // Skip this frame
            }

            // 3. Parse the output to extract keypoint coordinates
            PoseResult result;
            // Initialize zone hits to false
            for (int z = 0; z < 4; ++z)
                result.zoneHits[z] = false;

            parsePoseOutput(netOutput, frame.cols, frame.rows, result);

            // Apply confidence threshold
            result.isValid =
                (result.detectedPoints > 5); // Need at least 5 keypoints for valid pose

            // Check zone hits: check if ANY detected keypoint is inside a zone
            if (result.isValid && result.detectedPoints > 0)
            {
                for (int colorIdx = 0; colorIdx < 4; ++colorIdx)
                {
                    std::vector<ZoneRect> rects;
                    loadZoneRects(colorIdx, rects);

                    bool hit = false;
                    // Check each detected keypoint
                    for (int i = 0; i < MPI_NUM_KEYPOINTS; ++i)
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

            // 4. Push result to the audio thread via lock-free FIFO (UPDATED API)
            if (fifo.getFreeSpace() >= 1)
            {
                auto writeScope = fifo.write(1);
                if (writeScope.blockSize1 > 0)
                {
                    fifoBuffer[writeScope.startIndex1] = result;
                }
            }

            // 5. Draw the skeleton on the frame for UI preview (if enabled)
            if (drawSkeletonParam->get())
            {
                // Draw skeleton lines
                for (const auto& pair : MPI_SKELETON_PAIRS)
                {
                    int idxA = pair.first;
                    int idxB = pair.second;

                    if (result.keypoints[idxA][0] >= 0 && result.keypoints[idxB][0] >= 0)
                    {
                        cv::Point ptA(
                            (int)result.keypoints[idxA][0], (int)result.keypoints[idxA][1]);
                        cv::Point ptB(
                            (int)result.keypoints[idxB][0], (int)result.keypoints[idxB][1]);
                        cv::line(frame, ptA, ptB, cv::Scalar(0, 255, 0), 3);
                    }
                }

                // Draw keypoint circles
                for (int i = 0; i < MPI_NUM_KEYPOINTS; ++i)
                {
                    if (result.keypoints[i][0] >= 0)
                    {
                        cv::Point pt((int)result.keypoints[i][0], (int)result.keypoints[i][1]);
                        cv::circle(frame, pt, 5, cv::Scalar(0, 0, 255), -1);
                    }
                }
            }

            // --- CROP LOGIC ---
            std::vector<cv::Point> validPoints;
            for (int i = 0; i < MPI_NUM_KEYPOINTS; ++i)
            {
                if (result.keypoints[i][0] >= 0)
                {
                    validPoints.emplace_back(
                        (int)result.keypoints[i][0], (int)result.keypoints[i][1]);
                }
            }
            if (validPoints.size() > 2)
            {
                cv::Rect box = cv::boundingRect(validPoints);
                box &= cv::Rect(0, 0, frame.cols, frame.rows);
                if (box.area() > 0)
                {
                    cv::Mat cropped = frame(box);
                    VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), cropped);
                }
            }
            else
            {
                cv::Mat emptyFrame;
                VideoFrameManager::getInstance().setFrame(getSecondaryLogicalId(), emptyFrame);
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

            // Passthrough logic (like ObjectDetectorModule)
            updateGuiFrame(frame);
            if (myLogicalId != 0)
                VideoFrameManager::getInstance().setFrame(myLogicalId, frame);
        }
        else
        {
            wait(50);
            continue;
        }

        // Run at ~15 FPS (pose estimation is computationally expensive)
        wait(66);
    }
}

void PoseEstimatorModule::parsePoseOutput(
    const cv::Mat& netOutput,
    int            frameWidth,
    int            frameHeight,
    PoseResult&    result)
{
    // OpenPose output format: [1, num_keypoints, height, width]
    // Each heatmap represents the probability of a keypoint at each location

    int H = netOutput.size[2]; // Heatmap height
    int W = netOutput.size[3]; // Heatmap width

    result.detectedPoints = 0;
    // Use modulated confidence value from audio thread (or fallback to parameter)
    float confidenceThreshold = currentConfidenceThreshold.load();

    int numHeatmaps = netOutput.size[1];
    int count = juce::jmin(MPI_NUM_KEYPOINTS, numHeatmaps);
    for (int i = 0; i < count; ++i)
    {
        cv::Mat heatMap(H, W, CV_32F, (void*)netOutput.ptr<float>(0, i));

        // Find the location of maximum confidence
        double    maxConfidence;
        cv::Point maxLoc;
        cv::minMaxLoc(heatMap, nullptr, &maxConfidence, nullptr, &maxLoc);

        if (maxConfidence > confidenceThreshold)
        {
            // Scale the heatmap coordinates back to the original frame size
            result.keypoints[i][0] = (float)maxLoc.x * frameWidth / W;
            result.keypoints[i][1] = (float)maxLoc.y * frameHeight / H;
            result.detectedPoints++;
        }
        else
        {
            // Mark as not detected
            result.keypoints[i][0] = -1.0f;
            result.keypoints[i][1] = -1.0f;
        }
    }
}

void PoseEstimatorModule::updateGuiFrame(const cv::Mat& frame)
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

juce::Image PoseEstimatorModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

// Serialize zone rectangles to string: "x1,y1,w1,h1;x2,y2,w2,h2;..."
juce::String PoseEstimatorModule::serializeZoneRects(const std::vector<ZoneRect>& rects)
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
std::vector<PoseEstimatorModule::ZoneRect> PoseEstimatorModule::deserializeZoneRects(
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
void PoseEstimatorModule::loadZoneRects(int colorIndex, std::vector<ZoneRect>& rects) const
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
void PoseEstimatorModule::saveZoneRects(int colorIndex, const std::vector<ZoneRect>& rects)
{
    juce::String key = "zone_color_" + juce::String(colorIndex) + "_rects";
    juce::String data = serializeZoneRects(rects);
    apvts.state.setProperty(key, data, nullptr);
}

std::vector<DynamicPinInfo> PoseEstimatorModule::getDynamicOutputPins() const
{
    // Bus 0: CV Out (34 channels - 30 existing + 4 zone gates)
    // Bus 1: Video Out (1 channel)
    // Bus 2: Cropped Out (1 channel)
    std::vector<DynamicPinInfo> pins;

    // Add all 15 keypoint pins
    const std::vector<std::string> keypointNames = {
        "Head",
        "Neck",
        "R Shoulder",
        "R Elbow",
        "R Wrist",
        "L Shoulder",
        "L Elbow",
        "L Wrist",
        "R Hip",
        "R Knee",
        "R Ankle",
        "L Hip",
        "L Knee",
        "L Ankle",
        "Chest"};

    for (size_t i = 0; i < keypointNames.size(); ++i)
    {
        pins.emplace_back(keypointNames[i] + " X", static_cast<int>(i * 2), PinDataType::CV);
        pins.emplace_back(keypointNames[i] + " Y", static_cast<int>(i * 2 + 1), PinDataType::CV);
    }

    // Add zone gate pins (channels 30-33)
    pins.emplace_back("Red Zone Gate", 30, PinDataType::Gate);
    pins.emplace_back("Green Zone Gate", 31, PinDataType::Gate);
    pins.emplace_back("Blue Zone Gate", 32, PinDataType::Gate);
    pins.emplace_back("Yellow Zone Gate", 33, PinDataType::Gate);

    // Add Video Out and Cropped Out pins
    const int videoOutStartChannel = 34;
    const int croppedOutStartChannel = videoOutStartChannel + 1;
    pins.emplace_back("Video Out", videoOutStartChannel, PinDataType::Video);
    pins.emplace_back("Cropped Out", croppedOutStartChannel, PinDataType::Video);

    return pins;
}

bool PoseEstimatorModule::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus
    if (paramId == paramIdConfidenceMod)
    {
        outChannelIndexInBus = 1; // Confidence CV is on channel 1
        return true;
    }
    return false;
}

void PoseEstimatorModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // ✅ CRITICAL: Read ALL inputs BEFORE clearing buffer (prevents buffer aliasing bug)
    auto inputBuffer = getBusBuffer(buffer, true, 0);

    // Read Source ID from input pin (channel 0)
    float sourceIdFloat = 0.0f;
    if (inputBuffer.getNumChannels() > 0 && inputBuffer.getNumSamples() > 0)
    {
        sourceIdFloat = inputBuffer.getSample(0, 0);
    }

    // Read Confidence CV modulation (channel 1) if connected
    const bool   isConfidenceModConnected = isParamInputConnected(paramIdConfidenceMod);
    const float* confidenceCV = nullptr;
    if (isConfidenceModConnected && inputBuffer.getNumChannels() > 1)
    {
        confidenceCV = inputBuffer.getReadPointer(1);
    }

    // Get base confidence value
    const float baseConfidence = confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f;

    // Process confidence modulation per-sample
    const int numSamples = buffer.getNumSamples();
    float     currentConfidence = baseConfidence;

    if (isConfidenceModConnected && confidenceCV)
    {
        // Use the last sample value (or average if needed)
        // For pose estimation, we use a single threshold per frame, so use the last sample
        const float cvValue = confidenceCV[numSamples - 1];
        // CV is 0-1, map directly to confidence range (0.0-1.0)
        currentConfidence = juce::jlimit(0.0f, 1.0f, cvValue);

        // Update telemetry for UI
        setLiveParamValue("confidence_live", currentConfidence);
    }
    else
    {
        // No modulation, use base value
        currentConfidence = baseConfidence;
    }

    // Store modulated confidence for processing thread
    currentConfidenceThreshold.store(currentConfidence);

    // Store source ID
    currentSourceId.store((juce::uint32)sourceIdFloat);

    // ✅ Now safe to clear the buffer for output
    buffer.clear();

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

    // Read ALL available results from FIFO to ensure latest result is used (like
    // ColorTrackerModule)
    while (fifo.getNumReady() > 0)
    {
        auto readScope = fifo.read(1);
        if (readScope.blockSize1 > 0)
            lastResultForAudio = fifoBuffer[readScope.startIndex1];
    }

    // Map keypoint coordinates to output channels (bus 0 - CV Out)
    auto cvOutBus = getBusBuffer(buffer, false, 0);
    // Channel layout: [Head X, Head Y, Neck X, Neck Y, R Shoulder X, R Shoulder Y, ...]
    for (int i = 0; i < MPI_NUM_KEYPOINTS; ++i)
    {
        int chX = i * 2;
        int chY = i * 2 + 1;

        if (chY < cvOutBus.getNumChannels())
        {
            // Normalize coordinates to 0-1 range based on typical video resolution (640x480 or
            // similar) If keypoint not detected (negative value), output 0
            float x_normalized =
                (lastResultForAudio.keypoints[i][0] >= 0)
                    ? juce::jlimit(0.0f, 1.0f, lastResultForAudio.keypoints[i][0] / 640.0f)
                    : 0.0f;

            float y_normalized =
                (lastResultForAudio.keypoints[i][1] >= 0)
                    ? juce::jlimit(0.0f, 1.0f, lastResultForAudio.keypoints[i][1] / 480.0f)
                    : 0.0f;

            // Fill the entire buffer with the current value (DC signal)
            for (int sample = 0; sample < cvOutBus.getNumSamples(); ++sample)
            {
                cvOutBus.setSample(chX, sample, x_normalized);
                cvOutBus.setSample(chY, sample, y_normalized);
            }
        }
    }

    // Output zone gates (channels 30-33)
    for (int z = 0; z < 4; ++z)
    {
        int ch = 30 + z;
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

    // Output Cropped Out ID on bus 2
    auto croppedOutBus = getBusBuffer(buffer, false, 2);
    if (croppedOutBus.getNumChannels() > 0)
    {
        float secondaryId = static_cast<float>(getSecondaryLogicalId());
        for (int s = 0; s < croppedOutBus.getNumSamples(); ++s)
            croppedOutBus.setSample(0, s, secondaryId);
    }
}

#if defined(PRESET_CREATOR_UI)
ImVec2 PoseEstimatorModule::getCustomNodeSize() const
{
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void PoseEstimatorModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
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
            "Enable GPU acceleration for pose detection.\nRequires CUDA-capable NVIDIA GPU.");
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

    // Model selection dropdown
    bool modelMod = isParamModulated("model");
    if (modelMod)
        ImGui::BeginDisabled();
    if (modelChoiceParam)
    {
        int m = modelChoiceParam->getIndex();
        if (ImGui::Combo(
                "Model",
                &m,
                "BODY_25 (25 pts)\0COCO (18 pts)\0MPI (15 pts)\0MPI Fast (15 pts)\0\0"))
        {
            if (!modelMod)
            {
                *modelChoiceParam = m;
                requestedModelIndex = m; // signal thread to reload
                onModificationEnded();
            }
        }
        // Scroll-edit for model combo
        if (!modelMod && ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const int newM = juce::jlimit(0, 3, m + (wheel > 0.0f ? -1 : 1));
                if (newM != m)
                {
                    *modelChoiceParam = newM;
                    requestedModelIndex = newM;
                    onModificationEnded();
                }
            }
        }
    }
    if (modelMod)
        ImGui::EndDisabled();

    // Blob size (maps to quality tiers)
    bool qualityMod = isParamModulated("quality");
    if (qualityMod)
        ImGui::BeginDisabled();
    if (qualityParam)
    {
        int blobSize = (qualityParam->getIndex() == 0) ? 224 : 368;
        if (ImGui::SliderInt("Blob Size", &blobSize, 224, 368))
        {
            if (!qualityMod)
            {
                int q = (blobSize <= 296) ? 0 : 1; // snap to Low/Medium
                *qualityParam = q;
                onModificationEnded();
            }
        }
        if (!qualityMod)
            adjustParamOnWheel(
                apvts.getParameter("quality"), "quality", (float)qualityParam->getIndex());
    }
    if (qualityMod)
        ImGui::EndDisabled();

    // Confidence threshold with CV modulation support
    bool  confidenceMod = isParamModulated(paramIdConfidenceMod);
    float confidenceFallback = confidenceThresholdParam ? confidenceThresholdParam->load() : 0.1f;
    float confidence =
        confidenceMod
            ? getLiveParamValueFor(paramIdConfidenceMod, "confidence_live", confidenceFallback)
            : confidenceFallback;
    if (confidenceMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Confidence", &confidence, 0.0f, 1.0f, "%.2f"))
    {
        if (!confidenceMod)
            *dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramIdConfidence)) =
                confidence;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !confidenceMod)
        onModificationEnded();
    if (!confidenceMod)
        adjustParamOnWheel(apvts.getParameter(paramIdConfidence), paramIdConfidence, confidence);
    if (confidenceMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    // Restore Zoom (-/+) controls
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    float      buttonWidth = (itemWidth / 2.0f) - 4.0f;
    const bool atMin = (level <= 0);
    const bool atMax = (level >= 2);
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

    // Status display
    if (modelLoaded)
    {
        ThemeText("Model: Loaded", theme.text.success);
        ThemeText(
            juce::String::formatted(
                "Keypoints: %d/%d", lastResultForAudio.detectedPoints, MPI_NUM_KEYPOINTS)
                .toRawUTF8(),
            theme.text.section_header);
    }
    else
    {
        ThemeText("Model: NOT LOADED", theme.text.error);
        ImGui::TextWrapped("Place model files in: assets/openpose_models/pose/mpi/");
    }

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
            const PoseResult& uiResult = lastResultForAudio;

            // Draw a small red dot at each detected keypoint
            ImU32 redColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            for (int i = 0; i < MPI_NUM_KEYPOINTS; ++i)
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

void PoseEstimatorModule::drawIoPins(const NodePinHelpers& helpers)
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

    // Input pins
    helpers.drawParallelPins("Source In", 0, nullptr, -1);
    helpers.drawParallelPins("Confidence Mod", 1, nullptr, -1);

    // Define body part groups for MPI 15-keypoint model
    struct BodyPartGroup
    {
        const char*      name;
        std::vector<int> keypointIndices;
    };

    const BodyPartGroup groups[] = {
        {"Head & Torso", {0, 1, 14}}, // Head, Neck, Chest
        {"Right Arm", {2, 3, 4}},     // R Shoulder, R Elbow, R Wrist
        {"Left Arm", {5, 6, 7}},      // L Shoulder, L Elbow, L Wrist
        {"Right Leg", {8, 9, 10}},    // R Hip, R Knee, R Ankle
        {"Left Leg", {11, 12, 13}}    // L Hip, L Knee, L Ankle
    };
    constexpr int numGroups = 5;

    static std::map<juce::uint32, std::array<bool, numGroups>> collapseState;
    juce::uint32                                               nodeId = getLogicalId();
    if (collapseState.find(nodeId) == collapseState.end())
        collapseState[nodeId] = {false, true, true, true, true};

    auto& collapsed = collapseState[nodeId];

    for (int g = 0; g < numGroups; ++g)
    {
        const auto& group = groups[g];

        bool hasConnectionsInGroup = false;
        if (helpers.isOutputPinConnected)
        {
            for (int kp : group.keypointIndices)
            {
                if (helpers.isOutputPinConnected(0, kp * 2) ||
                    helpers.isOutputPinConnected(0, kp * 2 + 1))
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
            for (int kp : group.keypointIndices)
            {
                const std::string& name = MPI_KEYPOINT_NAMES[kp];
                helpers.drawParallelPins(
                    nullptr, -1, (juce::String(name) + " X").toRawUTF8(), kp * 2);
                helpers.drawParallelPins(
                    nullptr, -1, (juce::String(name) + " Y").toRawUTF8(), kp * 2 + 1);
            }
        }
        else if (helpers.isOutputPinConnected)
        {
            for (int kp : group.keypointIndices)
            {
                if (helpers.isOutputPinConnected(0, kp * 2))
                    helpers.drawParallelPins(
                        nullptr,
                        -1,
                        (juce::String(MPI_KEYPOINT_NAMES[kp]) + " X").toRawUTF8(),
                        kp * 2);
                if (helpers.isOutputPinConnected(0, kp * 2 + 1))
                    helpers.drawParallelPins(
                        nullptr,
                        -1,
                        (juce::String(MPI_KEYPOINT_NAMES[kp]) + " Y").toRawUTF8(),
                        kp * 2 + 1);
            }
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    helpers.drawParallelPins(nullptr, -1, "Red Zone Gate", 30);
    helpers.drawParallelPins(nullptr, -1, "Green Zone Gate", 31);
    helpers.drawParallelPins(nullptr, -1, "Blue Zone Gate", 32);
    helpers.drawParallelPins(nullptr, -1, "Yellow Zone Gate", 33);

    ImGui::Spacing();
    helpers.drawParallelPins(nullptr, -1, "Video Out", 34);
    helpers.drawParallelPins(nullptr, -1, "Cropped Out", 35);

    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}
#endif
