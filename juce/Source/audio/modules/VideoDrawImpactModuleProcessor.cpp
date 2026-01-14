#include "VideoDrawImpactModuleProcessor.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <algorithm>

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout VideoDrawImpactModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Saturation control (0.0 = grayscale, 1.0 = full color, up to 3.0 for oversaturation)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>("saturation", "Saturation", 0.0f, 3.0f, 1.0f));

    // Drawing color (RGB stored as normalized floats)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "drawColorR", "Draw Color R", 0.0f, 1.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "drawColorG", "Draw Color G", 0.0f, 1.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "drawColorB", "Draw Color B", 0.0f, 1.0f, 0.0f));

    // Frame persistence (how many frames a drawing stays visible)
    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            "framePersistence", "Frame Persistence", 1, 60, 3));

    // Brush size (radius in pixels)
    params.push_back(
        std::make_unique<juce::AudioParameterInt>("brushSize", "Brush Size", 1, 50, 5));

    // Clear all drawings button (trigger, not persistent)
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("clearDrawings", "Clear Drawings", false));

    return {params.begin(), params.end()};
}

VideoDrawImpactModuleProcessor::VideoDrawImpactModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::mono(), true)
              .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      juce::Thread("VideoDrawImpact Thread"),
      apvts(*this, nullptr, "VideoDrawImpactParams", createParameterLayout())
{
    // Initialize parameter pointers
    saturationParam = apvts.getRawParameterValue("saturation");
    drawColorRParam = apvts.getRawParameterValue("drawColorR");
    drawColorGParam = apvts.getRawParameterValue("drawColorG");
    drawColorBParam = apvts.getRawParameterValue("drawColorB");
    framePersistenceParam =
        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("framePersistence"));
    brushSizeParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("brushSize"));
    clearDrawingsParam =
        dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("clearDrawings"));

    // Initialize current draw color from parameters
    updateDrawColorFromParams();
}

VideoDrawImpactModuleProcessor::~VideoDrawImpactModuleProcessor()
{
    stopThread(5000);
    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void VideoDrawImpactModuleProcessor::prepareToPlay(double, int) { startThread(); }

void VideoDrawImpactModuleProcessor::releaseResources()
{
    signalThreadShouldExit();
    stopThread(5000);
}

void VideoDrawImpactModuleProcessor::processBlock(
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

    // Output our own Logical ID on the output pin, so we can be chained
    if (buffer.getNumChannels() > 0 && buffer.getNumSamples() > 0)
    {
        float sourceId = (float)myLogicalId;
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            buffer.setSample(0, sample, sourceId);
        }
    }
}

void VideoDrawImpactModuleProcessor::run()
{
    cv::Mat processedFrame;

    while (!threadShouldExit())
    {
        // Resolve source ID as early as possible (handles XML load before processBlock runs)
        juce::uint32 sourceId = currentSourceId.load();
        cv::Mat      prefetchedFrame;

        if (sourceId == 0)
        {
            // Try cached resolved ID first
            if (cachedResolvedSourceId != 0)
            {
                sourceId = cachedResolvedSourceId;
            }
            // Otherwise resolve via connection graph / fallback search
            else if (parentSynth != nullptr)
            {
                auto snapshot = parentSynth->getConnectionSnapshot();
                if (snapshot && !snapshot->empty())
                {
                    // Resolve our logical ID so we can find the upstream connection
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

                // FINAL FALLBACK: search any video source that already has frames
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

        // Fetch frame from the VideoFrameManager (unless fallback already grabbed one)
        cv::Mat frame = !prefetchedFrame.empty()
                            ? prefetchedFrame
                            : VideoFrameManager::getInstance().getFrame(sourceId);

        // Cache last good frame for paused/no-signal scenarios (like ColorTrackerModule)
        if (!frame.empty())
        {
            const juce::ScopedLock lk(frameLock);
            frame.copyTo(lastFrameBgr);
        }
        else
        {
            // Use cached frame when no fresh frames are available (e.g., transport paused, loading)
            const juce::ScopedLock lk(frameLock);
            if (!lastFrameBgr.empty())
                frame = lastFrameBgr.clone();
        }

        if (frame.empty())
        {
            // Wait longer when no frame is available (video might still be loading)
            wait(100);
            continue;
        }

        // Monitor performance
        auto t0 = juce::Time::getMillisecondCounterHiRes();

        // Increment frame number for timeline tracking
        int currentFrameIndex = currentFrameNumber.fetch_add(1) + 1;

        // Query timeline state from source
        SourceTimelineState timelineState = getSourceTimelineState();
        double              timelinePos = timelineState.isValid ? timelineState.positionSeconds
                                                                : static_cast<double>(currentFrameIndex);
        double              prevPos = lastTimelinePositionSeconds.load();
        double              deltaTime = 0.0;
        if (timelineState.isValid)
        {
            double duration = juce::jmax(1e-6, timelineState.durationSeconds);
            deltaTime = timelinePos - prevPos;
            if (deltaTime < 0.0)
                deltaTime += duration;
            if (!timelineState.isActive)
                deltaTime = 0.0;
        }
        else
        {
            deltaTime = 1.0 / 30.0; // Fallback frame duration
        }
        if (deltaTime <= 0.0)
            deltaTime = lastFrameDurationSeconds.load();
        lastTimelinePositionSeconds.store(timelinePos);
        lastFrameDurationSeconds.store(deltaTime);

        // Clone frame for processing
        processedFrame = frame.clone();

        // Apply saturation adjustment
        float saturation = saturationParam ? saturationParam->load() : 0.0f;
        if (saturation < 1.0f)
        {
            applySaturation(processedFrame, saturation);
        }

        // Process pending draw operations from UI thread
        processPendingDrawOps();

        // Draw all active strokes onto frame
        drawStrokesOnFrame(processedFrame, timelineState, deltaTime, currentFrameIndex);

        // Clean up expired keyframes (when not timeline-driven)
        cleanupExpiredKeyframes();

        // Resolve logical ID continuously (parentSynth may become available later)
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

        // Update GUI preview regardless of passthrough state
        updateGuiFrame(processedFrame);

        // Publish processed frame once we have a valid logical ID
        if (myLogicalId != 0)
            VideoFrameManager::getInstance().setFrame(myLogicalId, processedFrame);

        // Handle clear drawings button
        if (clearDrawingsParam && clearDrawingsParam->get())
        {
            {
                const juce::ScopedLock lock(drawingsLock);
                activeDrawings.clear();
            }
            {
                const juce::ScopedLock lock(timelineLock);
                timelineKeyframes.clear();
            }
            {
                const juce::ScopedLock lock(pendingOpsLock);
                pendingDrawOps.clear();
            }
            *clearDrawingsParam = false; // Reset button
        }

        auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
        lastProcessTimeMs = (float)elapsed;
        lastProcessWasGpu = false;

        wait(33); // ~30 FPS
    }
}

void VideoDrawImpactModuleProcessor::applySaturation(cv::Mat& frame, float saturation)
{
    if (saturation >= 1.0f)
        return; // No change needed

    cv::Mat hsv;
    cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Mat> hsvChannels;
    cv::split(hsv, hsvChannels);

    // Apply saturation
    hsvChannels[1].convertTo(hsvChannels[1], CV_32F);
    hsvChannels[1] *= saturation;
    hsvChannels[1].convertTo(hsvChannels[1], CV_8U);

    cv::merge(hsvChannels, hsv);
    cv::cvtColor(hsv, frame, cv::COLOR_HSV2BGR);
}

void VideoDrawImpactModuleProcessor::updateDrawColorFromParams()
{
    float r = drawColorRParam ? drawColorRParam->load() : 1.0f;
    float g = drawColorGParam ? drawColorGParam->load() : 0.0f;
    float b = drawColorBParam ? drawColorBParam->load() : 0.0f;

    // Convert RGB (0-1) to BGR (0-255) for OpenCV
    currentDrawColor = cv::Scalar(
        (int)(b * 255.0f), // B
        (int)(g * 255.0f), // G
        (int)(r * 255.0f)  // R
    );
}

void VideoDrawImpactModuleProcessor::addDrawPoint(const cv::Point2i& point, bool isErase)
{
    const juce::ScopedLock lock(pendingOpsLock);

    // Update draw color from parameters if not erasing
    if (!isErase)
    {
        updateDrawColorFromParams();
    }

    PendingDrawOperation op;
    op.point = point;
    op.color = isErase ? cv::Scalar(255, 255, 255) : currentDrawColor; // White for erase
    op.brushSize = brushSizeParam ? brushSizeParam->get() : 5;
    op.isNewStroke = !isDrawing || (isErase != lastWasErase); // New stroke if mode changed
    op.isErase = isErase;

    pendingDrawOps.push_back(op);
    isDrawing = true;
    lastDrawPoint = point;
    lastWasErase = isErase;
}

VideoDrawImpactModuleProcessor::SourceTimelineState VideoDrawImpactModuleProcessor::
    getSourceTimelineState() const
{
    SourceTimelineState state;

    const juce::uint32 sourceId = currentSourceId.load();
    if (sourceId == 0 || parentSynth == nullptr)
        return state;

    if (auto* module = parentSynth->getModuleForLogical(sourceId))
    {
        if (module->canProvideTimeline())
        {
            state.positionSeconds = juce::jmax(0.0, module->getTimelinePositionSeconds());
            state.durationSeconds = juce::jmax(0.0, module->getTimelineDurationSeconds());
            state.isActive = module->isTimelineActive();
            state.isValid = state.durationSeconds > 0.0;
        }
    }

    return state;
}

bool VideoDrawImpactModuleProcessor::eraseKeyframesNear(
    double targetValue,
    float  normalizedY,
    bool   timelineMode,
    double valueTolerance,
    float  yTolerance,
    double wrapLength)
{
    const juce::ScopedLock lock(timelineLock);
    if (timelineKeyframes.empty())
        return false;

    const double clampedValueTol = juce::jmax(0.0, valueTolerance);
    const float  clampedYTol = juce::jmax(0.0f, yTolerance);
    bool         removed = false;

    const double baseFrameDuration = juce::jmax(1e-4, lastFrameDurationSeconds.load());
    const double fallbackPersistenceSeconds =
        (framePersistenceParam ? juce::jmax(1, framePersistenceParam->get()) : 3) *
        baseFrameDuration;

    auto intervalDelta = [&](double relative, double length, double wrap) -> double {
        auto single = [&](double rel) -> double {
            if (rel < 0.0)
                return -rel;
            if (rel > length)
                return rel - length;
            return 0.0;
        };

        double best = single(relative);
        if (wrap > 0.0)
        {
            best = juce::jmin(best, single(relative + wrap));
            best = juce::jmin(best, single(relative - wrap));
        }
        return best;
    };

    auto newEnd = std::remove_if(
        timelineKeyframes.begin(), timelineKeyframes.end(), [&](const TimelineKeyframe& kf) {
            double startValue = timelineMode ? kf.timeSeconds : static_cast<double>(kf.frameNumber);
            double durationSeconds =
                kf.persistenceSeconds > 0.0 ? kf.persistenceSeconds : fallbackPersistenceSeconds;
            if (durationSeconds <= 1e-6)
                durationSeconds = fallbackPersistenceSeconds;

            double lengthValue = timelineMode
                                     ? durationSeconds
                                     : juce::jmax(1.0, durationSeconds / baseFrameDuration);
            double relative = targetValue - startValue;
            double delta = intervalDelta(relative, lengthValue, timelineMode ? wrapLength : 0.0);

            if (delta <= clampedValueTol && std::abs(kf.normalizedY - normalizedY) <= clampedYTol)
            {
                removed = true;
                return true;
            }

            return false;
        });

    if (removed)
        timelineKeyframes.erase(newEnd, timelineKeyframes.end());
    return removed;
}

void VideoDrawImpactModuleProcessor::updateKeyframePosition(
    int    keyframeIndex,
    double newTimeSeconds,
    float  newNormalizedY)
{
    const juce::ScopedLock lock(timelineLock);

    if (keyframeIndex < 0 || keyframeIndex >= (int)timelineKeyframes.size())
        return;

    TimelineKeyframe& kf = timelineKeyframes[(size_t)keyframeIndex];

    // Update time
    kf.timeSeconds = newTimeSeconds;

    // Update normalized Y position
    kf.normalizedY = juce::jlimit(0.0f, 1.0f, newNormalizedY);

    // If not timeline-driven, also update frame number (assuming 30 FPS)
    if (!getSourceTimelineState().isValid)
    {
        kf.frameNumber = (int)std::round(newTimeSeconds * 30.0);
    }
}

void VideoDrawImpactModuleProcessor::processPendingDrawOps()
{
    const juce::ScopedLock pendingLock(pendingOpsLock);
    const juce::ScopedLock drawingsLockGuard(drawingsLock);

    DrawingStroke*            currentStroke = nullptr;
    int                       currentFrame = currentFrameNumber.load();
    const SourceTimelineState timelineState = getSourceTimelineState();
    const double              timelineDuration =
        timelineState.isValid ? juce::jmax(1e-6, timelineState.durationSeconds) : 1.0;

    for (const auto& op : pendingDrawOps)
    {
        if (op.isNewStroke || currentStroke == nullptr)
        {
            // Start new stroke
            DrawingStroke newStroke;
            newStroke.color = op.color;
            newStroke.brushSize = op.brushSize;
            newStroke.remainingFrames = framePersistenceParam ? framePersistenceParam->get() : 3;
            newStroke.startFrameNumber = currentFrame;
            newStroke.isErase = op.isErase;
            newStroke.points.push_back(op.point);
            activeDrawings.push_back(newStroke);
            currentStroke = &activeDrawings.back();

            // Add color to history if it's a draw operation (not erase)
            if (!op.isErase)
            {
                // Convert BGR (0-255) to RGB (0-1) for ImGui
                ImVec4 colorHistoryEntry(
                    op.color[2] / 255.0f, // R
                    op.color[1] / 255.0f, // G
                    op.color[0] / 255.0f, // B
                    1.0f                  // A
                );

                // Add to history (most recent first)
                const juce::ScopedLock colorHistoryGuard(colorHistoryLock);
                // Remove if already exists (to move to front)
                usedColors.erase(
                    std::remove_if(
                        usedColors.begin(),
                        usedColors.end(),
                        [&colorHistoryEntry](const ImVec4& c) {
                            return std::abs(c.x - colorHistoryEntry.x) < 0.01f &&
                                   std::abs(c.y - colorHistoryEntry.y) < 0.01f &&
                                   std::abs(c.z - colorHistoryEntry.z) < 0.01f;
                        }),
                    usedColors.end());
                // Add to front
                usedColors.insert(usedColors.begin(), colorHistoryEntry);
                // Limit to MAX_COLOR_HISTORY
                if (usedColors.size() > MAX_COLOR_HISTORY)
                    usedColors.resize(MAX_COLOR_HISTORY);

                // Create timeline keyframe ONLY for draw operations (not erase)
                // Erase operations should only remove existing drawings, not create markers
                TimelineKeyframe keyframe;
                keyframe.frameNumber = currentFrame;
                if (timelineState.isValid)
                {
                    double timeSeconds = timelineState.positionSeconds;
                    if (timelineDuration > 0.0)
                        timeSeconds = std::fmod(juce::jmax(0.0, timeSeconds), timelineDuration);
                    keyframe.timeSeconds = timeSeconds;
                }
                else
                {
                    keyframe.timeSeconds = static_cast<double>(currentFrame);
                }
                double frameDur = lastFrameDurationSeconds.load();
                if (frameDur <= 0.0)
                    frameDur = 1.0 / 30.0;
                const int persistenceFrames =
                    framePersistenceParam ? framePersistenceParam->get() : 3;
                keyframe.persistenceSeconds =
                    juce::jmax(1e-4, frameDur * juce::jmax(1, persistenceFrames));
                keyframe.color = op.color;
                keyframe.brushSize = op.brushSize;
                keyframe.isErase = false; // Draw operations are never erase markers

                // Get frame dimensions for normalization
                juce::Image latest = getLatestFrame();
                if (!latest.isNull())
                {
                    keyframe.normalizedX = (float)op.point.x / latest.getWidth();
                    keyframe.normalizedY = (float)op.point.y / latest.getHeight();
                }
                else
                {
                    keyframe.normalizedX = 0.5f;
                    keyframe.normalizedY = 0.5f;
                }

                const juce::ScopedLock timelineLockGuard(timelineLock);
                timelineKeyframes.push_back(keyframe);
            }
            // Note: Erase operations do NOT create timeline keyframes
            // They only affect activeDrawings for immediate visual erasing on the frame
        }
        else
        {
            // Continue current stroke
            currentStroke->points.push_back(op.point);
        }
    }

    pendingDrawOps.clear();
}

void VideoDrawImpactModuleProcessor::drawStrokesOnFrame(
    cv::Mat&                   frame,
    const SourceTimelineState& timelineState,
    double                     frameDurationSeconds,
    int                        currentFrameIndex)
{
    {
        const juce::ScopedLock lock(drawingsLock);

        for (auto& stroke : activeDrawings)
        {
            if (stroke.isErase)
            {
                cv::Scalar eraseColor(0, 0, 0);

                if (stroke.points.size() < 2)
                {
                    cv::circle(frame, stroke.points[0], stroke.brushSize * 2, eraseColor, -1);
                }
                else
                {
                    for (size_t i = 1; i < stroke.points.size(); ++i)
                    {
                        cv::line(
                            frame,
                            stroke.points[i - 1],
                            stroke.points[i],
                            eraseColor,
                            stroke.brushSize * 2,
                            cv::LINE_AA);
                    }
                }
            }
            else
            {
                if (stroke.points.size() < 2)
                {
                    cv::circle(frame, stroke.points[0], stroke.brushSize, stroke.color, -1);
                }
                else
                {
                    for (size_t i = 1; i < stroke.points.size(); ++i)
                    {
                        cv::line(
                            frame,
                            stroke.points[i - 1],
                            stroke.points[i],
                            stroke.color,
                            stroke.brushSize,
                            cv::LINE_AA);
                    }
                }
            }

            stroke.remainingFrames--;
        }

        activeDrawings.erase(
            std::remove_if(
                activeDrawings.begin(),
                activeDrawings.end(),
                [](const DrawingStroke& s) { return s.remainingFrames <= 0; }),
            activeDrawings.end());
    }

    const double duration =
        timelineState.isValid ? juce::jmax(1e-6, timelineState.durationSeconds) : 0.0;
    const double currentTime = timelineState.isValid
                                   ? timelineState.positionSeconds
                                   : static_cast<double>(currentFrameIndex) * frameDurationSeconds;
    const int    persistenceFrames = framePersistenceParam ? framePersistenceParam->get() : 3;
    const double defaultPersistenceSeconds =
        juce::jmax(frameDurationSeconds * juce::jmax(1, persistenceFrames), 1e-4);

    const juce::ScopedLock timelineGuard(timelineLock);
    for (const auto& kf : timelineKeyframes)
    {
        double persistenceSeconds =
            kf.persistenceSeconds > 0.0 ? kf.persistenceSeconds : defaultPersistenceSeconds;
        bool shouldDraw = false;
        if (timelineState.isValid)
        {
            double dt = currentTime - kf.timeSeconds;
            if (dt < 0.0 && duration > 0.0)
                dt += duration;
            if (dt >= 0.0 && dt <= persistenceSeconds)
                shouldDraw = true;
        }
        else
        {
            int frameOffset = currentFrameIndex - kf.frameNumber;
            if (frameOffset >= 0)
            {
                double dtSeconds = frameOffset * frameDurationSeconds;
                if (dtSeconds <= persistenceSeconds)
                    shouldDraw = true;
            }
        }

        if (!shouldDraw)
            continue;

        int         x = frame.cols > 1
                            ? juce::jlimit(
                          0,
                          frame.cols - 1,
                          static_cast<int>(std::round(kf.normalizedX * (frame.cols - 1))))
                            : 0;
        int         y = frame.rows > 1
                            ? juce::jlimit(
                          0,
                          frame.rows - 1,
                          static_cast<int>(std::round(kf.normalizedY * (frame.rows - 1))))
                            : 0;
        cv::Point2i pt(x, y);

        if (kf.isErase)
        {
            cv::Scalar eraseColor(0, 0, 0);
            cv::circle(frame, pt, juce::jmax(1, kf.brushSize * 2), eraseColor, -1);
        }
        else
        {
            cv::circle(frame, pt, juce::jmax(1, kf.brushSize), kf.color, -1);
        }
    }
}

void VideoDrawImpactModuleProcessor::cleanupExpiredKeyframes()
{
    if (getSourceTimelineState().isValid)
        return; // Preserve keyframes when synced to a video timeline

    const juce::ScopedLock lock(timelineLock);
    int                    currentFrame = currentFrameNumber.load();
    int maxPersistence = framePersistenceParam ? framePersistenceParam->get() : 3;

    timelineKeyframes.erase(
        std::remove_if(
            timelineKeyframes.begin(),
            timelineKeyframes.end(),
            [currentFrame, maxPersistence](const TimelineKeyframe& kf) {
                return (currentFrame - kf.frameNumber) > maxPersistence;
            }),
        timelineKeyframes.end());
}

void VideoDrawImpactModuleProcessor::updateGuiFrame(const cv::Mat& frame)
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

juce::Image VideoDrawImpactModuleProcessor::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

juce::ValueTree VideoDrawImpactModuleProcessor::getExtraStateTree() const
{
    juce::ValueTree vt("VideoDrawImpactState");
    // Save current draw color, frame persistence, brush size, zoom
    if (drawColorRParam)
        vt.setProperty("drawColorR", drawColorRParam->load(), nullptr);
    if (drawColorGParam)
        vt.setProperty("drawColorG", drawColorGParam->load(), nullptr);
    if (drawColorBParam)
        vt.setProperty("drawColorB", drawColorBParam->load(), nullptr);
    vt.setProperty("zoomPixelsPerSecond", zoomPixelsPerSecond, nullptr);
    vt.setProperty("timelineScrollPixels", timelineScrollPixels, nullptr);

    // For integer parameters, convert from normalized value (0-1) to actual integer range
    if (auto* param = apvts.getParameter("framePersistence"))
    {
        auto  range = apvts.getParameterRange("framePersistence");
        float normalizedValue = param->getValue();
        int   actualValue = (int)range.convertFrom0to1(normalizedValue);
        vt.setProperty("framePersistence", actualValue, nullptr);
    }
    if (auto* param = apvts.getParameter("brushSize"))
    {
        auto  range = apvts.getParameterRange("brushSize");
        float normalizedValue = param->getValue();
        int   actualValue = (int)range.convertFrom0to1(normalizedValue);
        vt.setProperty("brushSize", actualValue, nullptr);
    }

    {
        const juce::ScopedLock lock(timelineLock);
        if (!timelineKeyframes.empty())
        {
            juce::ValueTree timelineNode("Keyframes");
            for (const auto& kf : timelineKeyframes)
            {
                juce::ValueTree kfNode("Keyframe");
                kfNode.setProperty("frame", kf.frameNumber, nullptr);
                kfNode.setProperty("timeSeconds", kf.timeSeconds, nullptr);
                kfNode.setProperty("persistenceSeconds", kf.persistenceSeconds, nullptr);
                kfNode.setProperty("brushSize", kf.brushSize, nullptr);
                kfNode.setProperty("isErase", kf.isErase, nullptr);
                kfNode.setProperty("normalizedX", kf.normalizedX, nullptr);
                kfNode.setProperty("normalizedY", kf.normalizedY, nullptr);
                kfNode.setProperty("colorB", (int)kf.color[0], nullptr);
                kfNode.setProperty("colorG", (int)kf.color[1], nullptr);
                kfNode.setProperty("colorR", (int)kf.color[2], nullptr);
                timelineNode.addChild(kfNode, -1, nullptr);
            }
            vt.addChild(timelineNode, -1, nullptr);
        }
    }

    // Save used colors palette
    {
        const juce::ScopedLock colorHistoryGuard(colorHistoryLock);
        if (!usedColors.empty())
        {
            juce::ValueTree colorsNode("UsedColors");
            for (const auto& color : usedColors)
            {
                juce::ValueTree colorNode("Color");
                colorNode.setProperty("r", color.x, nullptr); // RGB as floats 0-1
                colorNode.setProperty("g", color.y, nullptr);
                colorNode.setProperty("b", color.z, nullptr);
                colorsNode.addChild(colorNode, -1, nullptr);
            }
            vt.addChild(colorsNode, -1, nullptr);
        }
    }

    return vt;
}

void VideoDrawImpactModuleProcessor::setExtraStateTree(const juce::ValueTree& state)
{
    if (!state.isValid() || !state.hasType("VideoDrawImpactState"))
        return;

    // Restore parameters
    if (auto* param = apvts.getParameter("drawColorR"))
        param->setValueNotifyingHost(state.getProperty("drawColorR", 1.0f));
    if (auto* param = apvts.getParameter("drawColorG"))
        param->setValueNotifyingHost(state.getProperty("drawColorG", 0.0f));
    if (auto* param = apvts.getParameter("drawColorB"))
        param->setValueNotifyingHost(state.getProperty("drawColorB", 0.0f));
    if (auto* param = apvts.getParameter("framePersistence"))
        param->setValueNotifyingHost(state.getProperty("framePersistence", 3.0f));
    if (auto* param = apvts.getParameter("brushSize"))
        param->setValueNotifyingHost(state.getProperty("brushSize", 5.0f));

    // Restore zoom
    zoomPixelsPerSecond = (float)state.getProperty("zoomPixelsPerSecond", 50.0);
    zoomPixelsPerSecond = juce::jlimit(10.0f, 500.0f, zoomPixelsPerSecond); // Clamp to valid range
    timelineScrollPixels = (float)state.getProperty("timelineScrollPixels", 0.0f);
    if (!std::isfinite(timelineScrollPixels) || timelineScrollPixels < 0.0f)
        timelineScrollPixels = 0.0f;

    updateDrawColorFromParams();

    // Restore used colors palette
    if (auto colorsNode = state.getChildWithName("UsedColors"); colorsNode.isValid())
    {
        const juce::ScopedLock colorHistoryGuard(colorHistoryLock);
        usedColors.clear();
        for (int i = 0; i < colorsNode.getNumChildren(); ++i)
        {
            auto colorChild = colorsNode.getChild(i);
            if (!colorChild.hasType("Color"))
                continue;

            ImVec4 color;
            color.x = (float)colorChild.getProperty("r", 1.0f); // R
            color.y = (float)colorChild.getProperty("g", 0.0f); // G
            color.z = (float)colorChild.getProperty("b", 0.0f); // B
            color.w = 1.0f;                                     // Alpha always 1.0

            // Clamp values to valid range
            color.x = juce::jlimit(0.0f, 1.0f, color.x);
            color.y = juce::jlimit(0.0f, 1.0f, color.y);
            color.z = juce::jlimit(0.0f, 1.0f, color.z);

            usedColors.push_back(color);
        }
        // Limit to MAX_COLOR_HISTORY (in case saved preset has more)
        if (usedColors.size() > MAX_COLOR_HISTORY)
            usedColors.resize(MAX_COLOR_HISTORY);
    }

    if (auto keyframesNode = state.getChildWithName("Keyframes"); keyframesNode.isValid())
    {
        const juce::ScopedLock lock(timelineLock);
        timelineKeyframes.clear();
        for (int i = 0; i < keyframesNode.getNumChildren(); ++i)
        {
            auto child = keyframesNode.getChild(i);
            if (!child.hasType("Keyframe"))
                continue;

            TimelineKeyframe kf;
            kf.frameNumber = (int)child.getProperty("frame", 0);
            kf.timeSeconds = (double)child.getProperty("timeSeconds", 0.0);
            kf.persistenceSeconds = (double)child.getProperty("persistenceSeconds", 0.0);
            kf.brushSize = (int)child.getProperty("brushSize", 5);
            kf.isErase = (bool)child.getProperty("isErase", false);
            kf.normalizedX = (float)child.getProperty("normalizedX", 0.5f);
            kf.normalizedY = (float)child.getProperty("normalizedY", 0.5f);
            int b = (int)child.getProperty("colorB", 0);
            int g = (int)child.getProperty("colorG", 0);
            int r = (int)child.getProperty("colorR", 0);
            kf.color = cv::Scalar(b, g, r);
            timelineKeyframes.push_back(kf);
        }
    }
}

std::vector<DynamicPinInfo> VideoDrawImpactModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"Source In", 0, PinDataType::Video});
    return pins;
}

std::vector<DynamicPinInfo> VideoDrawImpactModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    pins.push_back({"Output", 0, PinDataType::Video});
    return pins;
}

#if defined(PRESET_CREATOR_UI)
ImVec2 VideoDrawImpactModuleProcessor::getCustomNodeSize() const
{
    // Return a reasonable default size (can be adjusted based on UI needs)
    return ImVec2(480.0f, 0.0f);
}

void VideoDrawImpactModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    // Protect global ID space (prevents conflicts when multiple instances exist)
    ImGui::PushID(this);

    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    const float baseNodeWidth = getCustomNodeSize().x > 0.0f ? getCustomNodeSize().x : 480.0f;
    const float baseNodeHeight = 700.0f; // keep overall footprint stable and avoid clipping
    const float paddingX = ImGui::GetStyle().WindowPadding.x;
    const float contentWidth = juce::jmax(150.0f, baseNodeWidth - paddingX * 2.0f);
    const ImU32 sectionBorderColor = ImGui::ColorConvertFloat4ToU32(theme.text.section_header);
    const ImU32 disabledColor = ImGui::ColorConvertFloat4ToU32(theme.text.disabled);
    const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
    ImGui::BeginChild(
        "VideoDrawImpactContent",
        ImVec2(baseNodeWidth, baseNodeHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushItemWidth(contentWidth);

    // Read data before any child windows (following visualization guide pattern)
    // Note: Video preview is handled centrally by ImGuiNodeEditorComponent
    juce::Image latestFrame = getLatestFrame();
    int         frameWidth = latestFrame.getWidth();
    int         frameHeight = latestFrame.getHeight();

    // Get current parameter values
    float saturation = saturationParam ? saturationParam->load() : 1.0f;
    float colorR = drawColorRParam ? drawColorRParam->load() : 1.0f;
    float colorG = drawColorGParam ? drawColorGParam->load() : 0.0f;
    float colorB = drawColorBParam ? drawColorBParam->load() : 0.0f;
    int   framePersistence = framePersistenceParam ? framePersistenceParam->get() : 3;
    int   brushSize = brushSizeParam ? brushSizeParam->get() : 5;
    bool  clearDrawings = clearDrawingsParam ? clearDrawingsParam->get() : false;

    // Saturation slider
    bool saturationMod = isParamModulated("saturation");
    if (saturationMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Saturation", &saturation, 0.0f, 3.0f, "%.2f"))
    {
        if (!saturationMod)
        {
            // Update parameter through APVTS to ensure it's saved properly
            if (auto* param = apvts.getParameter("saturation"))
            {
                // Convert to normalized 0-1 range and set through APVTS
                float normalizedValue =
                    apvts.getParameterRange("saturation").convertTo0to1(saturation);
                param->setValueNotifyingHost(normalizedValue);
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !saturationMod)
        onModificationEnded();
    if (!saturationMod)
        adjustParamOnWheel(apvts.getParameter("saturation"), "saturation", saturation);
    if (saturationMod)
        ImGui::EndDisabled();

    ImGui::Spacing();

    // Color picker and history palette side by side for better space usage
    // Get color history (thread-safe copy) - read before layout calculations
    std::vector<ImVec4> colorsCopy;
    {
        const juce::ScopedLock colorHistoryGuard(colorHistoryLock);
        colorsCopy = usedColors;
    }

    // Calculate layout: color picker on left, palette on right
    const float colorPickerWidth = contentWidth * 0.65f; // 65% for color picker
    const float paletteWidth =
        contentWidth - colorPickerWidth - 8.0f; // Remaining space minus spacing
    const float swatchSize = 20.0f;             // Smaller swatches for compact layout
    const float spacing = 3.0f;
    const int   cols = 3; // 3 columns for better fit in narrow space

    // Color picker on the left
    ImGui::PushItemWidth(colorPickerWidth);
    ImVec4 colorVec4(colorR, colorG, colorB, 1.0f);
    bool   colorRMod = isParamModulated("drawColorR");
    bool   colorGMod = isParamModulated("drawColorG");
    bool   colorBMod = isParamModulated("drawColorB");
    bool   anyColorMod = colorRMod || colorGMod || colorBMod;

    if (anyColorMod)
        ImGui::BeginDisabled();
    if (ImGui::ColorPicker4(
            "Draw Color",
            (float*)&colorVec4,
            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs |
                ImGuiColorEditFlags_NoLabel))
    {
        if (!anyColorMod)
        {
            if (drawColorRParam)
                drawColorRParam->store(colorVec4.x);
            if (drawColorGParam)
                drawColorGParam->store(colorVec4.y);
            if (drawColorBParam)
                drawColorBParam->store(colorVec4.z);
            updateDrawColorFromParams();
            onModificationEnded();
        }
    }
    // Scroll wheel support for color picker (manual handler since adjustParamOnWheel can't hook
    // into ColorPicker4)
    if (!anyColorMod && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            // Adjust color components by small steps (0.01 per wheel tick)
            const float step = 0.01f;
            const float delta = wheel > 0.0f ? step : -step;

            float newR = juce::jlimit(0.0f, 1.0f, colorVec4.x + delta);
            float newG = juce::jlimit(0.0f, 1.0f, colorVec4.y + delta);
            float newB = juce::jlimit(0.0f, 1.0f, colorVec4.z + delta);

            // Only update if values changed
            if (newR != colorVec4.x || newG != colorVec4.y || newB != colorVec4.z)
            {
                if (drawColorRParam)
                    drawColorRParam->store(newR);
                if (drawColorGParam)
                    drawColorGParam->store(newG);
                if (drawColorBParam)
                    drawColorBParam->store(newB);
                updateDrawColorFromParams();
                onModificationEnded();
            }
        }
    }
    if (anyColorMod)
        ImGui::EndDisabled();
    ImGui::PopItemWidth();

    // Color history palette on the right - use SameLine to place next to color picker
    ImGui::SameLine(0, 8.0f); // 8px spacing from color picker

    // Create a group for the palette to keep it organized
    ImGui::BeginGroup();

    ThemeText("Used Colors", theme.text.section_header);

    // Display color swatches in a compact grid (3 columns)
    for (int i = 0; i < MAX_COLOR_HISTORY; ++i)
    {
        if (i > 0 && i % cols == 0)
            ImGui::NewLine();
        else if (i > 0)
            ImGui::SameLine(0, spacing);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 size(swatchSize, swatchSize);

        if (i < (int)colorsCopy.size())
        {
            // Draw filled color rectangle
            ImVec4 color = colorsCopy[i];
            ImU32  colorU32 = ImGui::ColorConvertFloat4ToU32(color);

            ImGui::GetWindowDrawList()->AddRectFilled(
                pos, ImVec2(pos.x + size.x, pos.y + size.y), colorU32);
            ImGui::GetWindowDrawList()->AddRect(
                pos, ImVec2(pos.x + size.x, pos.y + size.y), sectionBorderColor, 0.0f, 0, 1.0f);

            // Make it clickable
            ImGui::InvisibleButton(("##colorSwatch" + juce::String(i)).toRawUTF8(), size);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !anyColorMod)
            {
                // Select this color
                if (drawColorRParam)
                    drawColorRParam->store(color.x);
                if (drawColorGParam)
                    drawColorGParam->store(color.y);
                if (drawColorBParam)
                    drawColorBParam->store(color.z);
                updateDrawColorFromParams();
                onModificationEnded();
            }

            // Tooltip
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("R: %.2f G: %.2f B: %.2f", color.x, color.y, color.z);
                ImGui::Text("Click to select");
                ImGui::EndTooltip();
            }
        }
        else
        {
            // Draw empty rectangle placeholder (dashed border to show it's available)
            ImGui::GetWindowDrawList()->AddRect(
                pos, ImVec2(pos.x + size.x, pos.y + size.y), disabledColor, 0.0f, 0, 1.0f);
            // Draw diagonal line to indicate empty
            ImGui::GetWindowDrawList()->AddLine(
                pos, ImVec2(pos.x + size.x, pos.y + size.y), disabledColor, 1.0f);
            ImGui::Dummy(size); // Reserve space
        }
    }

    ImGui::EndGroup();

    ImGui::Spacing();
    ThemeText(
        "Left-click to draw, right-click to erase on the video preview.", theme.text.disabled);

    ImGui::Spacing();

    // Frame persistence slider
    bool framePersistenceMod = isParamModulated("framePersistence");
    if (framePersistenceMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderInt("Frame Persistence", &framePersistence, 1, 60, "%d frames"))
    {
        if (!framePersistenceMod && framePersistenceParam)
            *framePersistenceParam = framePersistence;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !framePersistenceMod)
        onModificationEnded();
    if (!framePersistenceMod)
        adjustParamOnWheel(
            apvts.getParameter("framePersistence"), "framePersistence", (float)framePersistence);
    if (framePersistenceMod)
        ImGui::EndDisabled();

    // Brush size slider
    bool brushSizeMod = isParamModulated("brushSize");
    if (brushSizeMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderInt("Brush Size", &brushSize, 1, 50, "%d px"))
    {
        if (!brushSizeMod && brushSizeParam)
            *brushSizeParam = brushSize;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !brushSizeMod)
        onModificationEnded();
    if (!brushSizeMod)
        adjustParamOnWheel(apvts.getParameter("brushSize"), "brushSize", (float)brushSize);
    if (brushSizeMod)
        ImGui::EndDisabled();

    ImGui::Spacing();

    // Clear drawings button
    if (ImGui::Button("Clear All Drawings", ImVec2(contentWidth, 0)))
    {
        if (clearDrawingsParam)
            *clearDrawingsParam = true;
        {
            const juce::ScopedLock lock(drawingsLock);
            activeDrawings.clear();
        }
        {
            const juce::ScopedLock lock(timelineLock);
            timelineKeyframes.clear();
        }
        {
            const juce::ScopedLock lock(pendingOpsLock);
            pendingDrawOps.clear();
        }
        onModificationEnded();
    }

    ImGui::Spacing();

    const SourceTimelineState timelineStateUi = getSourceTimelineState();

    // Video preview area (will be handled by central texture system)
    // For now, show frame info
    if (!latestFrame.isNull())
    {
        ThemeText("Frame Info", theme.text.section_header);
        ImGui::Text("Frame: %dx%d", frameWidth, frameHeight);
        ImGui::Text("Frame #: %d", currentFrameNumber.load());
        if (timelineStateUi.isValid)
            ImGui::Text(
                "Time: %.2fs / %.2fs",
                timelineStateUi.positionSeconds,
                timelineStateUi.durationSeconds);
    }
    else
    {
        ThemeText("No video input", theme.text.warning);
    }

    ImGui::Spacing();

    // === TIMELINE ZOOM SECTION ===
    ThemeText("Timeline Zoom:", theme.text.section_header);
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    if (ImGui::SliderFloat("##zoom", &zoomPixelsPerSecond, 10.0f, 500.0f, "%.0f px/s"))
    {
        // Zoom changed - no action needed, just update the variable
    }
    // Scroll-edit for zoom slider (manual handling since zoomPixelsPerSecond is not a JUCE
    // parameter)
    if (ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const float zoomStep = 10.0f; // 10 pixels per second per scroll step
            const float newZoom = juce::jlimit(
                10.0f, 500.0f, zoomPixelsPerSecond + (wheel > 0.0f ? zoomStep : -zoomStep));
            if (newZoom != zoomPixelsPerSecond)
            {
                zoomPixelsPerSecond = newZoom;
            }
        }
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();

    // === TIMELINE VIEW ===
    // Read keyframes data BEFORE BeginChild (following VCO pattern)
    std::vector<TimelineKeyframe> keyframesCopy;
    int                           currentFrame = currentFrameNumber.load();
    int maxPersistence = framePersistenceParam ? framePersistenceParam->get() : 3;
    int minFrameForDisplay = 0;
    int maxFrameForDisplay = currentFrame;

    {
        const juce::ScopedLock lock(timelineLock);
        keyframesCopy = timelineKeyframes; // Copy for UI thread
    }

    const float            timelineHeight = 90.0f;
    const float            sliderHeight = ImGui::GetFrameHeightWithSpacing();
    const ImGuiWindowFlags timelineFlags = ImGuiWindowFlags_NoScrollbar;

    if (ImGui::BeginChild(
            "TimelineView",
            ImVec2(contentWidth, timelineHeight + sliderHeight),
            false,
            timelineFlags))
    {
        const float timelineWidth = ImGui::GetContentRegionAvail().x;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        const auto resolveColor = [](ImU32 value, ImU32 fallback) {
            return value != 0 ? value : fallback;
        };
        ImU32 bgColor = resolveColor(theme.canvas.canvas_background, IM_COL32(30, 30, 30, 255));
        ImU32 gridColor = resolveColor(theme.canvas.grid_color, IM_COL32(60, 60, 60, 255));
        ImU32 playheadColor = IM_COL32(255, 200, 0, 255);

        const bool hasTimeline = timelineStateUi.isValid;
        double     totalDuration =
            hasTimeline ? juce::jmax(1e-3, timelineStateUi.durationSeconds) : 1.0;
        int minFrame = 0;
        int maxFrame = currentFrame;
        if (!hasTimeline && !keyframesCopy.empty())
        {
            minFrame = keyframesCopy[0].frameNumber;
            maxFrame = currentFrame;
            for (const auto& kf : keyframesCopy)
            {
                minFrame = juce::jmin(minFrame, kf.frameNumber);
                maxFrame = juce::jmax(maxFrame, kf.frameNumber);
            }
            if (currentFrame > maxPersistence)
                minFrame = juce::jmax(0, currentFrame - maxPersistence * 2);

            totalDuration = juce::jmax(1.0, (maxFrame - minFrame) / 30.0);
        }
        minFrameForDisplay = minFrame;
        maxFrameForDisplay = maxFrame;

        const float totalWidth =
            juce::jmax(timelineWidth, (float)(totalDuration * zoomPixelsPerSecond));
        const float maxScrollPixels = std::max(0.0f, totalWidth - timelineWidth);
        timelineScrollPixels = juce::jlimit(0.0f, maxScrollPixels, timelineScrollPixels);

        if (ImGui::IsWindowHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f && !ImGui::IsAnyItemActive())
            {
                const bool panRequested = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyAlt;
                if (panRequested)
                {
                    const float panStep = 30.0f;
                    timelineScrollPixels =
                        juce::jlimit(0.0f, maxScrollPixels, timelineScrollPixels - wheel * panStep);
                }
                else
                {
                    const double playheadTime =
                        hasTimeline ? timelineStateUi.positionSeconds : (currentFrame / 30.0);
                    const float playheadXContent = (float)(playheadTime * zoomPixelsPerSecond);
                    const float playheadVisible = playheadXContent - timelineScrollPixels;

                    const float zoomStep = 10.0f;
                    const float newZoom = juce::jlimit(
                        10.0f, 500.0f, zoomPixelsPerSecond + (wheel > 0.0f ? zoomStep : -zoomStep));
                    if (newZoom != zoomPixelsPerSecond)
                    {
                        zoomPixelsPerSecond = newZoom;
                        const float newTotalWidth =
                            juce::jmax(timelineWidth, (float)(totalDuration * newZoom));
                        const float newMaxScroll = std::max(0.0f, newTotalWidth - timelineWidth);
                        const float newPlayheadX = (float)(playheadTime * newZoom);
                        timelineScrollPixels =
                            juce::jlimit(0.0f, newMaxScroll, newPlayheadX - playheadVisible);
                    }
                }
            }
        }

        const ImVec2 canvasStart = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##timelineCanvas", ImVec2(timelineWidth, timelineHeight));
        const ImVec2 canvasMin = ImGui::GetItemRectMin();
        const ImVec2 canvasMax = ImVec2(canvasMin.x + timelineWidth, canvasMin.y + timelineHeight);

        drawList->AddRectFilled(canvasMin, canvasMax, bgColor);
        drawList->PushClipRect(canvasMin, canvasMax, true);

        const double displayStart = 0.0;
        const double displayEnd = totalDuration;
        const double displayRange = juce::jmax(1e-6, displayEnd - displayStart);
        const double visibleStartTime = timelineScrollPixels / zoomPixelsPerSecond;
        const double visibleEndTime = (timelineScrollPixels + timelineWidth) / zoomPixelsPerSecond;

        if (displayRange > 0.0)
        {
            const double gridStep = 1.0;
            const double firstGridLine = std::floor(visibleStartTime / gridStep) * gridStep;
            const double lastGridLine = std::ceil(visibleEndTime / gridStep) * gridStep;

            for (double t = firstGridLine; t <= lastGridLine + 1e-6; t += gridStep)
            {
                if (t < displayStart || t > displayEnd)
                    continue;

                const float x =
                    canvasMin.x + (float)(t * zoomPixelsPerSecond - timelineScrollPixels);
                drawList->AddLine(ImVec2(x, canvasMin.y), ImVec2(x, canvasMax.y), gridColor, 1.0f);
            }
        }

        if (displayRange > 0.0 && ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            if (mousePos.x >= canvasMin.x && mousePos.x <= canvasMax.x &&
                mousePos.y >= canvasMin.y && mousePos.y <= canvasMax.y)
            {
                const float mouseXContent = (mousePos.x - canvasMin.x) + timelineScrollPixels;
                double      targetTime = juce::jlimit<double>(
                    displayStart,
                    displayEnd,
                    static_cast<double>(mouseXContent) / static_cast<double>(zoomPixelsPerSecond));
                double targetValue = hasTimeline ? targetTime : targetTime * 30.0;
                float  targetNormY =
                    juce::jlimit(0.0f, 1.0f, (mousePos.y - canvasMin.y) / timelineHeight);

                const double valueTolerance =
                    hasTimeline ? juce::jmax(0.1, 10.0 / zoomPixelsPerSecond)
                                : juce::jmax(1.0, 10.0 / zoomPixelsPerSecond * 30.0);
                const float yTolerance = 0.08f;
                eraseKeyframesNear(
                    targetValue,
                    targetNormY,
                    hasTimeline,
                    valueTolerance,
                    yTolerance,
                    hasTimeline ? displayRange : 0.0);
            }
        }

        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        const bool   mouseInTimeline = mousePos.x >= canvasMin.x && mousePos.x <= canvasMax.x &&
                                     mousePos.y >= canvasMin.y && mousePos.y <= canvasMax.y;
        if (mouseInTimeline && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            timelineScrollPixels = juce::jlimit(
                0.0f, maxScrollPixels, timelineScrollPixels - ImGui::GetIO().MouseDelta.x);
        }
        const float hitRadius = 8.0f;

        const double baseFrameDuration = juce::jmax(1e-4, lastFrameDurationSeconds.load());
        const double fallbackPersistenceSeconds =
            (framePersistenceParam ? juce::jmax(1, framePersistenceParam->get()) : 3) *
            baseFrameDuration;

        auto computeMarkerRect = [&](const TimelineKeyframe& keyframe,
                                     float                   startX,
                                     float                   centerY,
                                     ImVec2&                 outMin,
                                     ImVec2&                 outMax) -> bool {
            double durationSeconds = keyframe.persistenceSeconds > 0.0 ? keyframe.persistenceSeconds
                                                                       : fallbackPersistenceSeconds;
            float  markerWidth = juce::jmax(3.0f, (float)(durationSeconds * zoomPixelsPerSecond));
            float  halfHeight = juce::jmax(3.0f, keyframe.brushSize * 0.4f);

            float clampedStart = juce::jmax(canvasMin.x, startX);
            float clampedEnd = juce::jmin(canvasMax.x, startX + markerWidth);
            if (clampedEnd <= clampedStart)
                return false;

            outMin = ImVec2(clampedStart, juce::jmax(canvasMin.y, centerY - halfHeight));
            outMax = ImVec2(clampedEnd, juce::jmin(canvasMax.y, centerY + halfHeight));
            return true;
        };

        if (!isDraggingKeyframe && mouseInTimeline && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            for (size_t i = 0; i < keyframesCopy.size(); ++i)
            {
                const auto& kf = keyframesCopy[i];
                double      keyTime = hasTimeline ? kf.timeSeconds : (kf.frameNumber / 30.0);
                if (keyTime < visibleStartTime - 0.1 || keyTime > visibleEndTime + 0.1)
                    continue;

                const float x =
                    canvasMin.x + (float)(keyTime * zoomPixelsPerSecond - timelineScrollPixels);
                const float y =
                    canvasMin.y + juce::jlimit(0.0f, 1.0f, kf.normalizedY) * timelineHeight;
                ImVec2 rectMin, rectMax;
                bool   hasRect = computeMarkerRect(kf, x, y, rectMin, rectMax);

                bool insideRect = hasRect && mousePos.x >= rectMin.x - hitRadius &&
                                  mousePos.x <= rectMax.x + hitRadius &&
                                  mousePos.y >= rectMin.y - hitRadius &&
                                  mousePos.y <= rectMax.y + hitRadius;

                const float dx = mousePos.x - x;
                const float dy = mousePos.y - y;
                const bool  insideCircle = std::sqrt(dx * dx + dy * dy) <= hitRadius;

                if (insideRect || insideCircle)
                {
                    isDraggingKeyframe = true;
                    draggingKeyframeIndex = (int)i;
                    dragOffsetX = dx;
                    dragOffsetY = dy;
                    break;
                }
            }
        }

        if (isDraggingKeyframe && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (draggingKeyframeIndex >= 0 && draggingKeyframeIndex < (int)keyframesCopy.size())
            {
                double newTime =
                    (mousePos.x - canvasMin.x + timelineScrollPixels) / zoomPixelsPerSecond;
                newTime = juce::jlimit<double>(displayStart, displayEnd, newTime);
                float newNormY =
                    juce::jlimit(0.0f, 1.0f, (mousePos.y - canvasMin.y) / timelineHeight);
                updateKeyframePosition(draggingKeyframeIndex, newTime, newNormY);

                keyframesCopy[(size_t)draggingKeyframeIndex].timeSeconds = newTime;
                keyframesCopy[(size_t)draggingKeyframeIndex].normalizedY = newNormY;
                if (!hasTimeline)
                    keyframesCopy[(size_t)draggingKeyframeIndex].frameNumber =
                        (int)std::round(newTime * 30.0);
            }
        }
        else if (isDraggingKeyframe && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            onModificationEnded();
            isDraggingKeyframe = false;
            draggingKeyframeIndex = -1;
        }

        for (size_t i = 0; i < keyframesCopy.size(); ++i)
        {
            const auto& kf = keyframesCopy[i];
            double      keyTime = hasTimeline ? kf.timeSeconds : (kf.frameNumber / 30.0);
            if (keyTime < visibleStartTime - 0.1 || keyTime > visibleEndTime + 0.1)
                continue;

            const float x =
                canvasMin.x + (float)(keyTime * zoomPixelsPerSecond - timelineScrollPixels);
            const float y = canvasMin.y + juce::jlimit(0.0f, 1.0f, kf.normalizedY) * timelineHeight;

            ImVec2 rectMin, rectMax;
            bool   hasRect = computeMarkerRect(kf, x, y, rectMin, rectMax);
            if (!hasRect)
                continue;

            ImU32 keyframeColor =
                IM_COL32((int)kf.color[2], (int)kf.color[1], (int)kf.color[0], 255);
            const bool  draggingThis = isDraggingKeyframe && draggingKeyframeIndex == (int)i;
            const ImU32 drawColor = draggingThis ? accentColor : keyframeColor;

            if (kf.isErase)
            {
                drawList->AddRect(rectMin, rectMax, drawColor, 3.0f, 0, 1.5f);
                drawList->AddLine(rectMin, rectMax, drawColor, 1.5f);
                drawList->AddLine(
                    ImVec2(rectMin.x, rectMax.y), ImVec2(rectMax.x, rectMin.y), drawColor, 1.5f);
            }
            else
            {
                drawList->AddRectFilled(rectMin, rectMax, drawColor, 3.0f);
                drawList->AddRect(rectMin, rectMax, sectionBorderColor, 3.0f, 0, 1.0f);
            }

            if (!isDraggingKeyframe && mouseInTimeline && mousePos.x >= rectMin.x - hitRadius &&
                mousePos.x <= rectMax.x + hitRadius && mousePos.y >= rectMin.y - hitRadius &&
                mousePos.y <= rectMax.y + hitRadius)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }

        if (displayRange > 0.0)
        {
            double playheadTime =
                hasTimeline ? timelineStateUi.positionSeconds : (currentFrame / 30.0);
            playheadTime = juce::jlimit<double>(displayStart, displayEnd, playheadTime);
            const float playheadX =
                canvasMin.x + (float)(playheadTime * zoomPixelsPerSecond - timelineScrollPixels);
            drawList->AddLine(
                ImVec2(playheadX, canvasMin.y),
                ImVec2(playheadX, canvasMax.y),
                playheadColor,
                2.0f);
            const float triangleSize = 6.0f;
            drawList->AddTriangleFilled(
                ImVec2(playheadX, canvasMin.y - triangleSize),
                ImVec2(playheadX - triangleSize * 0.5f, canvasMin.y),
                ImVec2(playheadX + triangleSize * 0.5f, canvasMin.y),
                playheadColor);
        }

        drawList->PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(
            canvasStart.x, canvasMin.y + timelineHeight + ImGui::GetStyle().ItemSpacing.y * 0.5f));
        ImGui::PushItemWidth(timelineWidth);
        float scrollNorm = maxScrollPixels > 0.0f ? timelineScrollPixels / maxScrollPixels : 0.0f;
        if (ImGui::SliderFloat(
                "##timelineScroll", &scrollNorm, 0.0f, 1.0f, "", ImGuiSliderFlags_NoInput))
        {
            timelineScrollPixels = scrollNorm * maxScrollPixels;
        }
        ImGui::PopItemWidth();
    }
    ImGui::EndChild();

    if (timelineStateUi.isValid)
    {
        ThemeText(
            juce::String::formatted(
                "Time %.2fs / %.2fs",
                timelineStateUi.positionSeconds,
                timelineStateUi.durationSeconds)
                .toRawUTF8(),
            theme.text.section_header);
    }
    else
    {
        ThemeText(
            juce::String::formatted("Frame %d-%d", minFrameForDisplay, maxFrameForDisplay)
                .toRawUTF8(),
            theme.text.section_header);
    }
    ThemeText("Right-click the timeline to remove impact markers.", theme.text.disabled);

    ImGui::PopItemWidth();
    drawPerformanceMetrics(contentWidth);
    ImGui::EndChild();
    ImGui::PopID();
}

void VideoDrawImpactModuleProcessor::enqueueDrawPointFromUi(int x, int y, bool isErase)
{
    addDrawPoint(cv::Point2i(x, y), isErase);
}

void VideoDrawImpactModuleProcessor::endUiStroke()
{
    const juce::ScopedLock lock(pendingOpsLock);
    isDrawing = false;
    lastWasErase = false;
    lastDrawPoint = {-1, -1};
}

void VideoDrawImpactModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Use drawParallelPins for proper parallel layout (following ParallelPinsGuide)
    helpers.drawParallelPins("Source In", 0, "Output", 0);
}
#endif
