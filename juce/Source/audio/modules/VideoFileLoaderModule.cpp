#include "VideoFileLoaderModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include <opencv2/imgproc.hpp>

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout VideoFileLoaderModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterBool>("loop", "Loop", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("sync", "Sync to Transport", true));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Zoom Level", juce::StringArray{"Small", "Normal", "Large"}, 1));
    // Playback controls
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "speed", "Speed", juce::NormalisableRange<float>(0.25f, 4.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("in", "Start", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("out", "End", 0.0f, 1.0f, 1.0f));

    // Add the engine selection parameter, defaulting to Naive for performance
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "engine", "Engine", juce::StringArray{"RubberBand", "Naive"}, 1));

    return {params.begin(), params.end()};
}

VideoFileLoaderModule::VideoFileLoaderModule()
    : ModuleProcessor(
          BusesProperties()
              .withOutput("CV Out", juce::AudioChannelSet::mono(), true)
              .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("Video File Loader Thread"),
      apvts(*this, nullptr, "VideoFileLoaderParams", createParameterLayout())
{
    loopParam = apvts.getRawParameterValue("loop");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");
    speedParam = apvts.getRawParameterValue("speed");
    inNormParam = apvts.getRawParameterValue("in");
    outNormParam = apvts.getRawParameterValue("out");
    syncParam = apvts.getRawParameterValue("sync");
    engineParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("engine"));
    syncToTransport.store(syncParam && (*syncParam > 0.5f));
}

VideoFileLoaderModule::~VideoFileLoaderModule()
{
    stopThread(5000);
    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void VideoFileLoaderModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock lock(audioLock);
    audioSampleRate = sampleRate;
    timePitch.prepare(sampleRate, 2, samplesPerBlock); // Prepare for stereo

    // Initialize FIFO to ~5 seconds of stereo audio
    fifoSize = (int)(sampleRate * 5.0);
    audioFifo.setSize(2, fifoSize, false, true, true); // 2 channels
    abstractFifo.setTotalSize(fifoSize);

    startThread(juce::Thread::Priority::normal);
    // If we already had a file open previously, request re-open on thread start
    // Also check videoFileToLoad (set by setExtraStateTree when loading from XML)
    if (currentVideoFile.existsAsFile())
        videoFileToLoad = currentVideoFile;
    else if (videoFileToLoad.existsAsFile() && !currentVideoFile.existsAsFile())
    {
        // When loading from XML, videoFileToLoad is set but currentVideoFile is empty
        // Ensure the file will be opened by the thread
        // (The thread will check videoFileToLoad.existsAsFile() and open it)
    }

    // NOTE: prepareToPlay() does NOT participate in pause/resume - that's setTimingInfo()'s
    // responsibility This only handles initial position restoration if audio device is restarted
    // while paused
    const double savedPausedPos = pausedNormalizedPosition.load();
    if (savedPausedPos >= 0.0)
    {
        // Queue seek to saved paused position (will be processed by run() thread)
        pendingSeekNormalized.store((float)savedPausedPos);
        needPreviewFrame.store(true);

        // Also update master clock if audio is loaded
        const juce::ScopedLock audioLk(audioLock);
        if (audioReader && audioReader->lengthInSamples > 0)
        {
            const double      clampedPos = juce::jlimit(0.0, 1.0, savedPausedPos);
            const juce::int64 targetSamplePos =
                (juce::int64)(clampedPos * audioReader->lengthInSamples);
            currentAudioSamplePosition.store(targetSamplePos);
            audioReadPosition = (double)targetSamplePos;
            updateLastKnownNormalizedFromSamples(targetSamplePos);
        }
    }
}

void VideoFileLoaderModule::releaseResources()
{
    // NOTE: pause/resume state is handled by setTimingInfo() using pausedNormalizedPosition
    signalThreadShouldExit();
    stopThread(5000);

    const juce::ScopedLock lock(audioLock);
    timePitch.reset();
}

void VideoFileLoaderModule::chooseVideoFile()
{
    // Default to exe/video/ directory, create if it doesn't exist
    juce::File startDir;
    auto       exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto videoDir = exeDir.getChildFile("video");
    if (videoDir.exists() && videoDir.isDirectory())
        startDir = videoDir;
    else if (videoDir.createDirectory())
        startDir = videoDir;
    else
        startDir = exeDir; // Fallback to exe folder if video directory can't be created

    fileChooser = std::make_unique<juce::FileChooser>(
        "Select a video file...", startDir, "*.mp4;*.mov;*.avi;*.mkv;*.wmv");
    auto chooserFlags =
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file.existsAsFile())
        {
            videoFileToLoad = file;
        }
    });
}

void VideoFileLoaderModule::run()
{
    // Resolve our logical ID once at the start
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

    // One-time OpenCV build summary: detect if FFMPEG is integrated
    {
        static std::atomic<bool> buildInfoLogged{false};
        if (!buildInfoLogged.exchange(true))
        {
            juce::String info(cv::getBuildInformation().c_str());
            bool         ffmpegYes = false;
            juce::String ffmpegLine;
            {
                juce::StringArray lines;
                lines.addLines(info);
                for (const auto& ln : lines)
                {
                    if (ln.containsIgnoreCase("FFMPEG:"))
                    {
                        ffmpegLine = ln.trim();
                        if (ln.containsIgnoreCase("YES"))
                            ffmpegYes = true;
                        break;
                    }
                }
            }
            juce::Logger::writeToLog(
                "[OpenCV Build] FFMPEG integrated: " + juce::String(ffmpegYes ? "YES" : "NO") +
                (ffmpegLine.isNotEmpty() ? juce::String(" | ") + ffmpegLine : juce::String()));
        }
    }

    bool   sourceIsOpen = false;
    double videoFps = 30.0;        // Default FPS
    double frameDurationMs = 33.0; // Default to ~30fps

    while (!threadShouldExit())
    {
        // Check if user requested a new file OR we need to (re)open the same file after restart
        if (videoFileToLoad.existsAsFile() &&
            (!videoCapture.isOpened() || videoFileToLoad != currentVideoFile))
        {
            if (videoCapture.isOpened())
            {
                videoCapture.release();
            }

            bool opened =
                videoCapture.open(videoFileToLoad.getFullPathName().toStdString(), cv::CAP_FFMPEG);
            if (!opened)
            {
                juce::Logger::writeToLog(
                    "[VideoFileLoader] FFmpeg backend open failed, retrying default backend: " +
                    videoFileToLoad.getFullPathName());
                opened = videoCapture.open(videoFileToLoad.getFullPathName().toStdString());
            }
            if (opened)
            {
                currentVideoFile = videoFileToLoad;
                videoFileToLoad = juce::File{}; // Clear request immediately after processing
                sourceIsOpen = true;
                needPreviewFrame.store(true);
                // Log backend name (helps diagnose FFmpeg vs MSMF at runtime)
#if (CV_VERSION_MAJOR >= 4)
                juce::String backendName = videoCapture.getBackendName().c_str();
                juce::Logger::writeToLog("[VideoFileLoader] Backend: " + backendName);
#endif
                // Reset state for new media, but keep user-defined in/out ranges
                totalFrames.store(0); // force re-evaluation
                lastPosFrame.store(0);
                // CRITICAL FIX: Don't set pendingSeekFrame to 0 here!
                // When the video file is reloaded (e.g., during resume from pause), setting this to
                // 0 would reset the playback position to the beginning, even though we want to
                // preserve the position. Set to -1 (no seek) instead.
                pendingSeekFrame.store(-1);

                // Get the video's native FPS and codec
                videoFps = videoCapture.get(cv::CAP_PROP_FPS);
                lastFourcc.store((int)videoCapture.get(cv::CAP_PROP_FOURCC));
                {
// Backend and raw FOURCC diagnostics
#if CV_VERSION_MAJOR >= 4
                    const std::string backendName = videoCapture.getBackendName();
                    juce::Logger::writeToLog(
                        "[VideoFileLoader] Backend: " + juce::String(backendName.c_str()));
#endif
                    const int fourccRaw = lastFourcc.load();
                    juce::Logger::writeToLog(
                        "[VideoFileLoader] Metadata: FPS=" + juce::String(videoFps, 2) +
                        ", Raw FOURCC=" + juce::String(fourccRaw) + " ('" +
                        fourccToString(fourccRaw) + "')");
                }
                {
                    int tf = (int)videoCapture.get(cv::CAP_PROP_FRAME_COUNT);
                    if (tf <= 1)
                        tf = 0; // treat unknown/invalid as 0 so UI uses normalized seeks
                    totalFrames.store(tf);
                    juce::Logger::writeToLog(
                        "[VideoFileLoader] Opened '" + currentVideoFile.getFileName() +
                        "' frames=" + juce::String(tf) + ", fps=" + juce::String(videoFps, 2) +
                        ", fourcc='" + fourccToString(lastFourcc.load()) + "'");
                    if (tf > 1 && videoFps > 0.0)
                        totalDurationMs.store((double)tf * (1000.0 / videoFps));
                    else
                        totalDurationMs.store(0.0);
                }
                if (videoFps > 0.0 && videoFps < 1000.0) // Sanity check
                {
                    frameDurationMs = 1000.0 / videoFps;
                    juce::Logger::writeToLog(
                        "[VideoFileLoader] Opened: " + currentVideoFile.getFileName() +
                        " (FPS: " + juce::String(videoFps, 2) + ")");
                }
                else
                {
                    frameDurationMs = 33.0; // Fallback to 30fps
                    juce::Logger::writeToLog(
                        "[VideoFileLoader] Opened: " + currentVideoFile.getFileName() +
                        " (FPS unknown, using 30fps)");
                }

                // Load audio from the video file
                loadAudioFromVideo();
            }
            else
            {
                juce::Logger::writeToLog(
                    "[VideoFileLoader] Failed to open: " + videoFileToLoad.getFullPathName());
                videoFileToLoad = juce::File{};
            }
        }

        if (!sourceIsOpen)
        {
            wait(500);
            continue;
        }

        // --- UNIFIED SEEK LOGIC ---
        // This is now the single point of control for seeking, triggered by UI.
        float normSeekPos = pendingSeekNormalized.exchange(-1.0f);
        if (normSeekPos >= 0.0f)
        {
            const double durMs = totalDurationMs.load();
            if (durMs > 0.0)
            {
                double seekToMs = juce::jlimit(0.0, durMs, (double)normSeekPos * durMs);

                // Seek video
                const juce::ScopedLock capLock(captureLock);
                if (videoCapture.isOpened())
                    videoCapture.set(cv::CAP_PROP_POS_MSEC, seekToMs);

                // Seek audio by updating the read position - clear FIFO and reset
                const juce::ScopedLock audioLk(audioLock);
                if (audioReader)
                {
                    audioReadPosition = normSeekPos * audioReader->lengthInSamples;
                    const juce::int64 newSamplePos = (juce::int64)audioReadPosition;
                    currentAudioSamplePosition.store(newSamplePos); // SET THE MASTER CLOCK
                    updateLastKnownNormalizedFromSamples(newSamplePos);
                    audioReader->resetPosition(); // Force seek on next read
                    abstractFifo.reset();         // Clear FIFO on seek
                    timePitch.reset();            // Reset time stretcher state after seek

                    // If we're the timeline master and playing, update transport immediately
                    // This ensures transport syncs immediately, not waiting for TempoClock to read
                    if (playing.load())
                    {
                        auto* parentSynth = getParent();
                        if (parentSynth && parentSynth->isModuleTimelineMaster(getLogicalId()))
                        {
                            const double sourceRate = sourceAudioSampleRate.load();
                            if (sourceRate > 0.0)
                            {
                                double newPosSeconds = (double)audioReadPosition / sourceRate;
                                parentSynth->setTransportPositionSeconds(newPosSeconds);
                            }
                        }
                    }
                }
            }
            // Fallback to ratio seek if duration is not known
            else
            {
                const juce::ScopedLock capLock(captureLock);
                if (videoCapture.isOpened())
                    videoCapture.set(cv::CAP_PROP_POS_AVI_RATIO, (double)normSeekPos);

                // Seek audio by ratio - clear FIFO and reset
                const juce::ScopedLock audioLk(audioLock);
                if (audioReader)
                {
                    audioReadPosition = normSeekPos * audioReader->lengthInSamples;
                    const juce::int64 newSamplePos = (juce::int64)audioReadPosition;
                    currentAudioSamplePosition.store(newSamplePos); // SET THE MASTER CLOCK
                    updateLastKnownNormalizedFromSamples(newSamplePos);
                    audioReader->resetPosition(); // Force seek on next read
                    abstractFifo.reset();         // Clear FIFO on seek
                    timePitch.reset();
                }
            }
            needPreviewFrame.store(true);
        }

        // Legacy frame-based seek support (for compatibility)
        int seekTo = pendingSeekFrame.exchange(-1);
        if (seekTo >= 0)
        {
            const juce::ScopedLock capLock(captureLock);
            if (videoCapture.isOpened())
                videoCapture.set(cv::CAP_PROP_POS_FRAMES, (double)seekTo);
            needPreviewFrame.store(true);

            // Also seek audio - clear FIFO and reset position
            const juce::ScopedLock audioLk(audioLock);
            if (audioReader)
            {
                int tfLocal = totalFrames.load();
                if (tfLocal > 1)
                {
                    float normPos = (float)seekTo / (float)(tfLocal - 1);
                    audioReadPosition = normPos * audioReader->lengthInSamples;
                    const juce::int64 newSamplePos = (juce::int64)audioReadPosition;

                    // DIAGNOSTIC: Log the seek operation
                    juce::Logger::writeToLog(
                        "[VideoLoader run()] Processing pendingSeekFrame: seekTo=" +
                        juce::String(seekTo) + " tfLocal=" + juce::String(tfLocal) + " normPos=" +
                        juce::String(normPos, 6) + " newSamplePos=" + juce::String(newSamplePos) +
                        " (BEFORE masterClock=" + juce::String(currentAudioSamplePosition.load()) +
                        ")");

                    currentAudioSamplePosition.store(newSamplePos); // SET THE MASTER CLOCK
                    updateLastKnownNormalizedFromSamples(newSamplePos);
                    audioReader->resetPosition(); // Force seek on next read
                    abstractFifo.reset();         // Clear FIFO on seek
                    timePitch.reset();
                }
            }
        }

        // NOTE: Play edge detection and pause/resume logic is handled by setTimingInfo()
        // The run() thread only processes pendingSeekNormalized requests
        // No play edge handling here - setTimingInfo() controls all pause/resume/stop behavior

        // Respect play/pause
        if (!playing.load())
        {
            // If paused, show a single preview frame after (re)open
            if (needPreviewFrame.exchange(false))
            {
                cv::Mat preview;
                if (videoCapture.isOpened())
                {
                    videoCapture.read(preview);
                }
                {
                    if (!preview.empty())
                    {
                        if (myLogicalId != 0)
                            VideoFrameManager::getInstance().setFrame(myLogicalId, preview);
                        updateGuiFrame(preview);
                        juce::Logger::writeToLog(
                            "[VideoFileLoader][Preview] Published paused preview frame");
                        lastPosFrame.store((int)videoCapture.get(cv::CAP_PROP_POS_FRAMES));
                        if (lastFourcc.load() == 0)
                            lastFourcc.store((int)videoCapture.get(cv::CAP_PROP_FOURCC));
                        // Also try to refresh total frames after the first paused read
                        if (totalFrames.load() <= 1)
                        {
                            int tf = (int)videoCapture.get(cv::CAP_PROP_FRAME_COUNT);
                            if (tf > 1)
                            {
                                totalFrames.store(tf);
                                juce::Logger::writeToLog(
                                    "[VideoFileLoader] Frame count acquired after paused read: " +
                                    juce::String(tf));
                            }
                        }
                    }
                }
            }
            wait(40);
            continue;
        }

        // Get current UI settings for this iteration
        const bool  isLoopingEnabled = loopParam && loopParam->load() > 0.5f;
        const float startNormalized = inNormParam ? inNormParam->load() : 0.0f;
        const float endNormalized = outNormParam ? outNormParam->load() : 1.0f;

        // ==============================================================================
        //  SECTION 1: AUDIO DECODER (The "Producer")
        //  This thread's only job is to decode audio and keep the FIFO buffer full,
        //  respecting the start/end points and looping its read position.
        // ==============================================================================
        if (playing.load() && audioLoaded.load() && audioReader &&
            abstractFifo.getFreeSpace() > 8192)
        {
            const juce::ScopedLock audioLk(audioLock);
            if (audioReader)
            {
                const juce::int64 startSample =
                    (juce::int64)(startNormalized * audioReader->lengthInSamples);
                juce::int64 endSample = (juce::int64)(endNormalized * audioReader->lengthInSamples);
                if (endSample <= startSample)
                    endSample = audioReader->lengthInSamples;

                // If the decoder's read-head is past the end point, loop it back to the start.
                if (audioReadPosition >= endSample)
                {
                    if (isLoopingEnabled)
                    {
                        audioReadPosition = (double)startSample;
                    }
                }

                // If the read-head is within the valid range, decode a chunk of audio.
                if (audioReadPosition < endSample)
                {
                    const int samplesToRead = 4096;
                    const int numSamplesAvailable = (int)(endSample - audioReadPosition);
                    const int numSamplesToReadNow = juce::jmin(samplesToRead, numSamplesAvailable);

                    if (numSamplesToReadNow > 0)
                    {
                        juce::AudioBuffer<float> tempReadBuffer(2, numSamplesToReadNow);
                        float*                   channelPointers[] = {
                            tempReadBuffer.getWritePointer(0), tempReadBuffer.getWritePointer(1)};
                        if (audioReader->readSamples(
                                reinterpret_cast<int* const*>(channelPointers),
                                2,
                                0,
                                (juce::int64)audioReadPosition,
                                numSamplesToReadNow))
                        {
                            audioReadPosition += numSamplesToReadNow;

                            // Write decoded audio into the FIFO buffer for the audio thread
                            int s1, z1, s2, z2;
                            abstractFifo.prepareToWrite(numSamplesToReadNow, s1, z1, s2, z2);
                            if (z1 > 0)
                                audioFifo.copyFrom(0, s1, tempReadBuffer, 0, 0, z1);
                            if (z1 > 0)
                                audioFifo.copyFrom(1, s1, tempReadBuffer, 1, 0, z1);
                            if (z2 > 0)
                                audioFifo.copyFrom(0, s2, tempReadBuffer, 0, z1, z2);
                            if (z2 > 0)
                                audioFifo.copyFrom(1, s2, tempReadBuffer, 1, z1, z2);
                            abstractFifo.finishedWrite(z1 + z2);
                        }
                    }
                }
            }
        }

        // ==============================================================================
        //  SECTION 2: VIDEO DISPLAY (The "Slave")
        //  This thread syncs the video to the master audio clock and handles the actual loop event.
        // ==============================================================================
        if (playing.load() && sourceIsOpen && totalFrames.load() > 1 && videoFps > 0)
        {
            // Get the current time from the master audio clock
            const juce::int64 audioMasterPosition = currentAudioSamplePosition.load();
            const double      sourceRate = sourceAudioSampleRate.load();
            const double      currentTimeInSeconds = (double)audioMasterPosition / sourceRate;

            // Calculate the target video frame that corresponds to the audio time
            // CRITICAL FIX: Guard against division by zero or invalid sample rate
            // If sourceRate is 0, currentTimeInSeconds becomes infinite/NaN, causing
            // targetVideoFrame to be huge which falsely triggers the loop condition immediately.
            int targetVideoFrame = 0;
            if (sourceRate > 1.0)
            {
                targetVideoFrame = (int)(currentTimeInSeconds * videoFps);
            }
            else
            {
                // Fallback if sample rate invalid - use video frame count if available
                targetVideoFrame = (int)videoCapture.get(cv::CAP_PROP_POS_FRAMES);
            }

            const int totalVideoFrames = totalFrames.load();
            const int startFrame = (int)(startNormalized * totalVideoFrames);
            const int endFrame = (int)(endNormalized * totalVideoFrames);

            // **THE CRITICAL FIX**: Check if the MASTER AUDIO CLOCK has passed the end point.
            // IMPORTANT: Only trigger loop if we've CROSSED the boundary (not just starting from
            // mid-point) This prevents false loop resets after resuming from pause
            const int  lastFrame = lastPosFrame.load();
            const bool crossedBoundary = (lastFrame < endFrame) && (targetVideoFrame >= endFrame);

            // DIAGNOSTIC LOGGING: Track frame calculations to diagnose loop issues
            static int logCounter = 0;
            if (logCounter++ % 100 == 0) // Log every 100 iterations to avoid spam
            {
                juce::Logger::writeToLog(
                    "[VideoLoader run()] Frame tracking: lastFrame=" + juce::String(lastFrame) +
                    " targetFrame=" + juce::String(targetVideoFrame) + " endFrame=" +
                    juce::String(endFrame) + " startFrame=" + juce::String(startFrame) +
                    " crossedBoundary=" + juce::String(crossedBoundary ? "TRUE" : "FALSE") +
                    " audioPos=" + juce::String(audioMasterPosition) +
                    " sourceRate=" + juce::String(sourceRate));
            }

            if (crossedBoundary)
            {
                juce::Logger::writeToLog(
                    "[VideoLoader run()] Loop boundary crossed: lastFrame=" +
                    juce::String(lastFrame) + " targetFrame=" + juce::String(targetVideoFrame) +
                    " endFrame=" + juce::String(endFrame) + " sourceRate=" +
                    juce::String(sourceRate) + " audioPos=" + juce::String(audioMasterPosition));

                if (isLoopingEnabled)
                {
                    // The loop is triggered HERE. Atomically reset all playback state for both
                    // audio and video.
                    const juce::ScopedLock audioLk(audioLock);
                    const juce::ScopedLock capLock(captureLock);

                    const juce::int64 startSample =
                        (juce::int64)(startNormalized * audioReader->lengthInSamples);

                    // Reset master clock to the start position
                    currentAudioSamplePosition.store(startSample);
                    updateLastKnownNormalizedFromSamples(startSample);
                    // Reset audio decoder's read position
                    audioReadPosition = (double)startSample;
                    // Reset video to the start frame
                    if (videoCapture.isOpened())
                        videoCapture.set(cv::CAP_PROP_POS_FRAMES, startFrame);
                    // Flush all buffers
                    abstractFifo.reset();
                    timePitch.reset();

                    // The new target is now the start frame
                    targetVideoFrame = startFrame;

                    // CRITICAL: If synced to transport, ensure playing state is maintained
                    // This prevents setTimingInfo() from stopping playback after loop
                    if (syncToTransport.load() && !playing.load())
                    {
                        // If we're synced but playing was false, restore it from transport state
                        playing.store(lastTransportPlaying.load());
                    }
                }
                else
                {
                    // Not looping, so stop playback and mark as stopped
                    juce::Logger::writeToLog(
                        "[VideoLoader run()] End of video reached (not looping) - stopping and "
                        "marking as stopped");
                    playing.store(false);
                    isStopped.store(true);
                    pausedNormalizedPosition.store(-1.0); // Clear saved position on new file load
                }
            }

            // With the correct target frame determined, seek the video capture directly to that
            // frame.
            if (playing.load() && videoCapture.isOpened())
            {
                const juce::ScopedLock capLock(captureLock);
                int currentVideoFrame = (int)videoCapture.get(cv::CAP_PROP_POS_FRAMES);

                // Only update the video if the target frame is different from the current one.
                if (currentVideoFrame != targetVideoFrame)
                {
                    videoCapture.set(cv::CAP_PROP_POS_FRAMES, targetVideoFrame);
                }

                // Read and display the single, correct frame for this point in time.
                cv::Mat frame;

                // Measure decode time
                double startTime = juce::Time::getMillisecondCounterHiRes();

                if (videoCapture.read(frame))
                {
                    if (myLogicalId != 0)
                        VideoFrameManager::getInstance().setFrame(myLogicalId, frame);
                    updateGuiFrame(frame);
                    lastPosFrame.store((int)videoCapture.get(cv::CAP_PROP_POS_FRAMES));
                    if (lastFourcc.load() == 0)
                        lastFourcc.store((int)videoCapture.get(cv::CAP_PROP_FOURCC));
                }

                double elapsed = juce::Time::getMillisecondCounterHiRes() - startTime;
                lastProcessTimeMs = (float)elapsed;
                lastProcessWasGpu = false; // CPU decode (FFMPEG)
            }
        }

        // A short sleep to prevent this thread from consuming 100% CPU
        wait(5);
    }

    videoCapture.release();
    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void VideoFileLoaderModule::updateGuiFrame(const cv::Mat& frame)
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

juce::Image VideoFileLoaderModule::getLatestFrame()
{
    const juce::ScopedLock lock(imageLock);
    return latestFrameForGui.createCopy();
}

void VideoFileLoaderModule::updateLastKnownNormalizedFromSamples(juce::int64 samplePos)
{
    const double lengthSamples = audioReaderLengthSamples.load();
    if (lengthSamples > 0.0)
    {
        const double normalized = juce::jlimit(0.0, 1.0, (double)samplePos / lengthSamples);
        lastKnownNormalizedPosition.store((float)normalized);

        const int tf = totalFrames.load();
        if (tf > 1)
        {
            const int frame = juce::jlimit(0, tf - 1, (int)std::round(normalized * (tf - 1)));
            lastPosFrame.store(frame);
        }
    }
}

void VideoFileLoaderModule::loadAudioFromVideo()
{
    const juce::ScopedLock lock(audioLock);

    audioReader.reset();
    timePitch.reset(); // Reset the processing engine

    // CRITICAL FIX: Preserve position if we're currently playing (resuming from pause)
    // Only reset master clock if we're stopped or loading a new file
    const bool        isCurrentlyPlaying = playing.load();
    const bool        isCurrentlyStopped = isStopped.load();
    const juce::int64 currentMasterClock = currentAudioSamplePosition.load();

    if (isCurrentlyPlaying && !isCurrentlyStopped && currentMasterClock > 0)
    {
        // We're playing and have a valid position - preserve it (likely resuming from pause)
        // Don't reset the master clock - it was already restored correctly by setTimingInfo()
        // Keep currentMasterClock and sync audioReadPosition
        // CRITICAL: Don't call updateLastKnownNormalizedFromSamples() here because
        // audioReaderLengthSamples is not yet set (it will be 0, causing division by zero).
        // It will be called AFTER the audio is loaded and audioReaderLengthSamples is set (line
        // 724).
        audioReadPosition = (double)currentMasterClock;
        // Don't reset audioReaderLengthSamples here - it will be set when audio loads
        // Don't reset lastKnownNormalizedPosition - it will be recalculated after load

        // CRITICAL FIX: Clear pendingSeekFrame to prevent run() thread from resetting position
        // When video file is reloaded, pendingSeekFrame gets set to 0 (line 227), which would
        // reset the master clock. By clearing it here, we prevent that.
        pendingSeekFrame.store(-1);
    }
    else
    {
        // New file load or explicit stop - reset to beginning
        audioReadPosition = 0.0;
        currentAudioSamplePosition.store(0); // Reset master clock
        updateLastKnownNormalizedFromSamples(0);

        // CRITICAL FIX: Only reset these when actually starting fresh, not when resuming
        audioReaderLengthSamples.store(0.0);
        lastKnownNormalizedPosition.store(0.0f);
        isStopped.store(true);                // New file loaded, start from beginning
        pausedNormalizedPosition.store(-1.0); // Clear saved position on new file load
    }

    abstractFifo.reset(); // Clear FIFO
    audioLoaded.store(false);

    if (!currentVideoFile.existsAsFile())
        return;

    try
    {
        audioReader = std::make_unique<FFmpegAudioReader>(currentVideoFile.getFullPathName());

        if (audioReader != nullptr && audioReader->lengthInSamples > 0)
        {
            audioLoaded.store(true);
            sourceAudioSampleRate.store(audioReader->sampleRate); // Store the source sample rate
            audioReaderLengthSamples.store((double)audioReader->lengthInSamples);

            // CRITICAL: Now that audioReaderLengthSamples is set, update normalized position
            // This is safe because audioReaderLengthSamples is now valid (not 0)
            const juce::int64 preservedPosition = currentAudioSamplePosition.load();
            updateLastKnownNormalizedFromSamples(preservedPosition);

            juce::Logger::writeToLog(
                "[VideoFileLoader] Audio loaded via FFmpeg. SampleRate=" +
                juce::String(audioReader->sampleRate) +
                " Length=" + juce::String(audioReader->lengthInSamples) +
                " PreservedPosition=" + juce::String(preservedPosition) +
                " Normalized=" + juce::String(lastKnownNormalizedPosition.load()));

            // DIAGNOSTIC: Log position immediately after loading to see if it changes
            juce::Logger::writeToLog(
                "[VideoFileLoader] Position immediately after load: masterClock=" +
                juce::String(currentAudioSamplePosition.load()) +
                " audioReadPosition=" + juce::String(audioReadPosition) +
                " normalized=" + juce::String(lastKnownNormalizedPosition.load()));
        }
        else
        {
            juce::Logger::writeToLog(
                "[VideoFileLoader] Could not extract audio stream via FFmpeg.");
        }
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            "[VideoFileLoader] Exception loading audio: " + juce::String(e.what()));
    }
}

void VideoFileLoaderModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto cvOutBus = getBusBuffer(buffer, false, 0);
    auto audioOutBus = getBusBuffer(buffer, false, 1);

    cvOutBus.clear();
    audioOutBus.clear();

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

    // Output Source ID on CV bus
    if (cvOutBus.getNumChannels() > 0)
    {
        float sourceId = (float)myLogicalId; // Use the correct ID
        cvOutBus.setSample(0, 0, sourceId);
        for (int ch = 0; ch < cvOutBus.getNumChannels(); ++ch)
            cvOutBus.copyFrom(ch, 1, cvOutBus, ch, 0, cvOutBus.getNumSamples() - 1);
    }

    // Return early if not playing
    if (!audioLoaded.load() || !playing.load())
    {
        return;
    }

    // Ensure sync state mirrors parameter value even if changed externally
    // CRITICAL FIX: Default to FALSE to prevent spacebar from inadvertently enabling sync mode
    // (which would reset position to 0). Sync mode should only activate when explicitly enabled by
    // user.
    const bool paramSync = syncParam ? (*syncParam > 0.5f) : false;
    const bool storedSync = syncToTransport.load();
    if (paramSync != storedSync)
    {
        syncToTransport.store(paramSync);
        if (paramSync)
        {
            playing.store(lastTransportPlaying.load());
            if (currentVideoFile.existsAsFile())
                videoFileToLoad = currentVideoFile;
        }
    }

    // --- NEW FIFO-BASED AUDIO PROCESSING ---
    const juce::ScopedLock lock(audioLock);
    const int              numSamples = audioOutBus.getNumSamples();

    // 1. Configure the time-stretcher
    const float speed = juce::jlimit(0.25f, 4.0f, speedParam ? speedParam->load() : 1.0f);
    const int   engineIdx = engineParam ? engineParam->getIndex() : 1;
    auto        requestedMode =
        (engineIdx == 0) ? TimePitchProcessor::Mode::RubberBand : TimePitchProcessor::Mode::Fifo;

    timePitch.setMode(requestedMode);

    // THIS IS THE FIX: Calculate the correct ratio based on which engine is active.
    double ratioForEngine = (double)speed;
    if (requestedMode == TimePitchProcessor::Mode::RubberBand)
    {
        // RubberBand expects a time-stretch ratio, which is the inverse of playback speed.
        // A speed of 2.0x requires a stretch ratio of 0.5 to play twice as fast.
        ratioForEngine = 1.0 / juce::jmax(0.01, (double)speed);
    }

    // The Naive engine expects a direct playback speed, so `ratioForEngine` is unchanged.
    timePitch.setTimeStretchRatio(ratioForEngine);

    // 2. Determine how many source frames we need to pull from the FIFO
    const int framesToReadFromFifo = (int)std::ceil((double)numSamples * speed);

    if (abstractFifo.getNumReady() >= framesToReadFromFifo)
    {
        // Use a temporary buffer for interleaving
        juce::AudioBuffer<float> interleavedBuffer(1, framesToReadFromFifo * 2);
        float*                   interleavedData = interleavedBuffer.getWritePointer(0);

        // 3. Read from FIFO and interleave the data
        int start1, size1, start2, size2;
        abstractFifo.prepareToRead(framesToReadFromFifo, start1, size1, start2, size2);

        const float* fifoDataL = audioFifo.getReadPointer(0);
        const float* fifoDataR = audioFifo.getReadPointer(1);

        for (int i = 0; i < size1; ++i)
        {
            interleavedData[2 * i] = fifoDataL[start1 + i];
            interleavedData[2 * i + 1] = fifoDataR[start1 + i];
        }
        if (size2 > 0)
        {
            for (int i = 0; i < size2; ++i)
            {
                interleavedData[2 * (size1 + i)] = fifoDataL[start2 + i];
                interleavedData[2 * (size1 + i) + 1] = fifoDataR[start2 + i];
            }
        }
        abstractFifo.finishedRead(size1 + size2);
        const int readCount = size1 + size2; // How many samples we actually read from FIFO

        // 4. Feed the time-stretcher
        timePitch.putInterleaved(interleavedData, framesToReadFromFifo);

        // 5. Retrieve processed audio
        juce::AudioBuffer<float> tempOutput(1, numSamples * 2);
        int produced = timePitch.receiveInterleaved(tempOutput.getWritePointer(0), numSamples);

        // 6. De-interleave into the output bus
        if (produced > 0)
        {
            const float* processedData = tempOutput.getReadPointer(0);
            for (int ch = 0; ch < audioOutBus.getNumChannels(); ++ch)
            {
                float* dest = audioOutBus.getWritePointer(ch);
                for (int i = 0; i < produced; ++i)
                {
                    dest[i] = (ch == 0) ? processedData[i * 2] : processedData[i * 2 + 1];
                }
            }

            // THIS IS THE FIX: Advance the master clock by the number of samples consumed
            const juce::int64 updatedSamplePos = currentAudioSamplePosition.load() + readCount;
            currentAudioSamplePosition.store(updatedSamplePos);
            updateLastKnownNormalizedFromSamples(updatedSamplePos);

            // If we're the timeline master and playing, update transport position continuously
            // This ensures transport stays in sync regardless of processing order
            if (playing.load())
            {
                auto* parentSynth = getParent();
                if (parentSynth && parentSynth->isModuleTimelineMaster(getLogicalId()))
                {
                    const double sourceRate = sourceAudioSampleRate.load();
                    if (sourceRate > 0.0)
                    {
                        const juce::int64 currentPos = currentAudioSamplePosition.load();
                        double            newPosSeconds = (double)currentPos / sourceRate;
                        parentSynth->setTransportPositionSeconds(newPosSeconds);
                    }
                }
            }
        }
    }
}

juce::ValueTree VideoFileLoaderModule::getExtraStateTree() const
{
    juce::ValueTree state("VideoFileLoaderState");
    // Save the absolute path of the currently loaded video file
    if (currentVideoFile.existsAsFile())
    {
        state.setProperty("videoFilePath", currentVideoFile.getFullPathName(), nullptr);
    }
    return state;
}

void VideoFileLoaderModule::setExtraStateTree(const juce::ValueTree& state)
{
    if (!state.hasType("VideoFileLoaderState"))
        return;

    juce::String filePath = state.getProperty("videoFilePath", "").toString();
    if (filePath.isNotEmpty())
    {
        juce::File restoredFile(filePath);
        if (restoredFile.existsAsFile())
        {
            videoFileToLoad = restoredFile;
            // Also set currentVideoFile so prepareToPlay() will trigger reload
            // This ensures the video opens immediately when play is clicked after loading
            currentVideoFile = restoredFile;
            juce::Logger::writeToLog(
                "[VideoFileLoader] Restored video file from preset: " + filePath);
        }
        else
        {
            juce::Logger::writeToLog(
                "[VideoFileLoader] Warning: Saved video file not found: " + filePath);
        }
    }
}

#if defined(PRESET_CREATOR_UI)
ImVec2 VideoFileLoaderModule::getCustomNodeSize() const
{
    // Return different width based on zoom level (0=240,1=480,2=960)
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void VideoFileLoaderModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    if (ImGui::Button("Load Video File...", ImVec2(itemWidth, 0)))
    {
        chooseVideoFile();
    }

    if (currentVideoFile.existsAsFile())
    {
        const juce::String fileName = currentVideoFile.getFileName();
        ThemeText(fileName.toRawUTF8(), theme.text.success);
    }
    else
    {
        ThemeText("No file loaded", theme.text.disabled);
    }

    bool loop = loopParam->load() > 0.5f;
    if (ImGui::Checkbox("Loop", &loop))
    {
        *dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("loop")) = loop;
        onModificationEnded();
    }

    // Transport sync and play/stop controls
    bool sync = syncParam ? (*syncParam > 0.5f) : true;
    if (ImGui::Checkbox("Sync to Transport", &sync))
    {
        syncToTransport.store(sync);
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("sync")))
            *p = sync;
        if (sync)
        {
            playing.store(lastTransportPlaying.load());
        }
    }

    ImGui::SameLine();
    bool localPlaying = playing.load();
    bool localStopped = isStopped.load();
    if (sync)
        ImGui::BeginDisabled();

    // Play/Pause button: shows "Pause" when playing, "Play" when paused or stopped
    const char* playPauseBtn = localPlaying ? "Pause" : "Play";
    if (sync)
    {
        // Mirror transport state in label when synced
        playPauseBtn = lastTransportPlaying.load() ? "Pause" : "Play";
    }
    if (ImGui::Button(playPauseBtn))
    {
        if (localPlaying)
        {
            // PAUSE: Save current position for resume
            juce::Logger::writeToLog("[VideoLoader UI] Play/Pause button - Pausing");
            const juce::ScopedLock audioLk(audioLock);
            if (audioReader && audioReader->lengthInSamples > 0)
            {
                const juce::int64 currentSamplePos = currentAudioSamplePosition.load();
                const double normalized = (double)currentSamplePos / audioReader->lengthInSamples;
                pausedNormalizedPosition.store(normalized);
                juce::Logger::writeToLog(
                    "[VideoLoader UI] Saved paused position: " + juce::String(normalized, 3));
            }
            isStopped.store(false); // Paused, not stopped
            playing.store(false);
        }
        else
        {
            // PLAY: Restore from paused position if available
            juce::Logger::writeToLog(
                "[VideoLoader UI] Play/Pause button - Playing, isStopped=" +
                juce::String(isStopped.load() ? "true" : "false"));

            const double savedPos = pausedNormalizedPosition.load();
            if (savedPos >= 0.0 && !isStopped.load())
            {
                // Resume from paused position
                const juce::ScopedLock audioLk(audioLock);
                if (audioReader && audioReader->lengthInSamples > 0)
                {
                    const double      clampedPos = juce::jlimit(0.0, 1.0, savedPos);
                    const juce::int64 targetSamplePos =
                        (juce::int64)(clampedPos * audioReader->lengthInSamples);
                    currentAudioSamplePosition.store(targetSamplePos);
                    audioReadPosition = (double)targetSamplePos;
                    updateLastKnownNormalizedFromSamples(targetSamplePos);
                    pendingSeekNormalized.store((float)clampedPos);
                    pausedNormalizedPosition.store(-1.0); // Clear after restoring
                    juce::Logger::writeToLog(
                        "[VideoLoader UI] Resumed from paused position: " +
                        juce::String(clampedPos, 3));
                }
            }
            else if (isStopped.load())
            {
                // Start from IN point
                float inN = inNormParam ? inNormParam->load() : 0.0f;
                pendingSeekNormalized.store(inN);
                const juce::ScopedLock audioLk(audioLock);
                if (audioReader)
                {
                    audioReadPosition = inN * audioReader->lengthInSamples;
                    const juce::int64 newSamplePos = (juce::int64)audioReadPosition;
                    currentAudioSamplePosition.store(newSamplePos);
                    updateLastKnownNormalizedFromSamples(newSamplePos);
                }
            }
            isStopped.store(false);
            playing.store(true);
        }
    }

    // Stop button: reset to start position
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        // Stop: reset position to start and mark as stopped
        juce::Logger::writeToLog("[VideoLoader UI] Stop button pressed");
        float inN = inNormParam ? inNormParam->load() : 0.0f;
        pendingSeekNormalized.store(inN);
        needPreviewFrame.store(true);

        // Also seek audio - clear FIFO and reset position
        const juce::ScopedLock audioLk(audioLock);
        if (audioReader)
        {
            audioReadPosition = inN * audioReader->lengthInSamples;
            const juce::int64 newSamplePos = (juce::int64)audioReadPosition;
            currentAudioSamplePosition.store(newSamplePos);
            updateLastKnownNormalizedFromSamples(newSamplePos);
            audioReader->resetPosition();
            abstractFifo.reset();
            timePitch.reset();
        }

        // Clear saved paused position and mark as stopped
        pausedNormalizedPosition.store(-1.0);
        isStopped.store(true);
    }

    if (sync)
        ImGui::EndDisabled();

    // Zoom buttons (+/-) across 3 levels
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

    const juce::String sourceIdText = juce::String::formatted("Source ID: %d", (int)getLogicalId());
    ThemeText(sourceIdText.toRawUTF8(), theme.text.section_header);
    {
        int          fcc = lastFourcc.load();
        juce::String codec = fourccToString(fcc);
        juce::String friendly = fourccFriendlyName(codec);
        juce::String ext = currentVideoFile.getFileExtension();
        if (ext.startsWithChar('.'))
            ext = ext.substring(1);
        if (ext.isEmpty())
            ext = "unknown";

        juce::String codecLine = "Codec: " + codec + " (" + friendly + ")   Container: " + ext;
        ThemeText(codecLine.toRawUTF8(), theme.text.active);
        if (totalFrames.load() <= 1)
            ThemeText("Length unknown yet (ratio seeks)", theme.text.warning);
    }

    // ADD ENGINE SELECTION COMBO BOX
    bool engineModulated = isParamModulated("engine");
    if (engineModulated)
        ImGui::BeginDisabled();
    int         engineIdx = engineParam ? engineParam->getIndex() : 1;
    const char* items[] = {"RubberBand (High Quality)", "Naive (Low CPU)"};
    if (ImGui::Combo("Engine", &engineIdx, items, 2))
    {
        if (engineParam)
            *engineParam = engineIdx;
        onModificationEnded();
    }
    // Scroll-edit for engine combo
    if (!engineModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newIdx = juce::jlimit(0, 1, engineIdx + (wheel > 0.0f ? -1 : 1));
            if (newIdx != engineIdx && engineParam)
            {
                *engineParam = newIdx;
                onModificationEnded();
            }
        }
    }
    if (engineModulated)
        ImGui::EndDisabled();

    // --- Playback speed (slider only) ---
    bool        speedModulated = isParamModulated("speed");
    const float speedFallback = speedParam ? speedParam->load() : 1.0f;
    float       spd = speedModulated ? getLiveParamValue("speed", speedFallback) : speedFallback;
    if (speedModulated)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Speed", &spd, 0.25f, 4.0f, "%.2fx"))
    {
        if (!speedModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("speed")))
                *p = spd;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !speedModulated)
        onModificationEnded();
    if (!speedModulated)
        adjustParamOnWheel(apvts.getParameter("speed"), "speed", spd);
    if (speedModulated)
        ImGui::EndDisabled();

    // --- Trim / Timeline ---
    {
        bool      inModulated = isParamModulated("in");
        bool      outModulated = isParamModulated("out");
        const int tf = juce::jmax(1, totalFrames.load());
        float inN = inModulated ? getLiveParamValue("in", inNormParam ? inNormParam->load() : 0.0f)
                                : (inNormParam ? inNormParam->load() : 0.0f);
        float outN = outModulated
                         ? getLiveParamValue("out", outNormParam ? outNormParam->load() : 1.0f)
                         : (outNormParam ? outNormParam->load() : 1.0f);

        if (inModulated)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Start", &inN, 0.0f, 1.0f, "%.3f"))
        {
            if (!inModulated)
            {
                inN = juce::jlimit(0.0f, outN - 0.01f, inN);
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("in")))
                    *p = inN;
                pendingSeekNormalized.store(inN); // Use unified seek
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !inModulated)
            onModificationEnded();
        if (!inModulated)
            adjustParamOnWheel(apvts.getParameter("in"), "in", inN);
        if (inModulated)
            ImGui::EndDisabled();

        if (outModulated)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("End", &outN, 0.0f, 1.0f, "%.3f"))
        {
            if (!outModulated)
            {
                outN = juce::jlimit(inN + 0.01f, 1.0f, outN);
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("out")))
                    *p = outN;
                onModificationEnded();
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !outModulated)
            onModificationEnded();
        if (!outModulated)
            adjustParamOnWheel(apvts.getParameter("out"), "out", outN);
        if (outModulated)
            ImGui::EndDisabled();

        float pos = (tf > 1) ? ((float)lastPosFrame.load() / (float)tf) : 0.0f;
        // Clamp position UI between in/out
        float minPos = juce::jlimit(0.0f, 1.0f, inN);
        float maxPos = juce::jlimit(minPos, 1.0f, outN);
        pos = juce::jlimit(minPos, maxPos, pos);

        // Position slider (manual scrub when not synced to transport)
        bool sliderChanged = false;
        if (sync)
            ImGui::BeginDisabled();
        sliderChanged = ImGui::SliderFloat("Position", &pos, minPos, maxPos, "%.3f");
        if (sync)
            ImGui::EndDisabled();
        if (!sync)
        {
            if (sliderChanged)
            {
                pendingSeekNormalized.store(pos);
                if (tf > 1)
                {
                    const int newFrame =
                        (int)juce::jlimit(0, tf - 1, (int)std::round(pos * (float)(tf - 1)));
                    lastPosFrame.store(newFrame);
                }
            }
            if (ImGui::IsItemHovered())
            {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f)
                {
                    const float step = 0.01f;
                    const float newPos =
                        juce::jlimit(minPos, maxPos, pos + (wheel > 0.0f ? step : -step));
                    pendingSeekNormalized.store(newPos);
                }
            }
        }
    }

    ImGui::PopItemWidth();
    drawPerformanceMetrics(itemWidth);
}

// === TIMELINE REPORTING INTERFACE IMPLEMENTATION ===

bool VideoFileLoaderModule::canProvideTimeline() const
{
    // Check if video file is loaded (sourceIsOpen would be better, but it's private)
    // We can check if totalDurationMs > 0 as a proxy
    return totalDurationMs.load() > 0.0;
}

double VideoFileLoaderModule::getTimelinePositionSeconds() const
{
    const juce::int64 audioPos = currentAudioSamplePosition.load();
    const double      sourceRate = sourceAudioSampleRate.load();
    if (sourceRate > 0.0)
        return (double)audioPos / sourceRate;
    return 0.0;
}

double VideoFileLoaderModule::getTimelineDurationSeconds() const
{
    return totalDurationMs.load() / 1000.0;
}

bool VideoFileLoaderModule::isTimelineActive() const { return playing.load(); }

void VideoFileLoaderModule::forceStop()
{
    // Force stop playback regardless of sync settings
    playing.store(false);
    isStopped.store(true);
    pausedNormalizedPosition.store(-1.0); // Clear saved position
    lastTransportPlaying.store(false);
}

void VideoFileLoaderModule::setTimingInfo(const TransportState& state)
{
    // CRITICAL: If this module is the timeline master, ignore transport updates
    // This prevents feedback loops where:
    // 1. VideoLoader scrubs → updates transport
    // 2. Transport broadcasts → VideoLoader receives update
    // 3. VideoLoader reacts → creates feedback loop
    auto*      parentSynth = getParent();
    const bool isTimelineMaster =
        parentSynth && parentSynth->isModuleTimelineMaster(getLogicalId());

    // Not the timeline master - accept transport updates normally
    const bool previousTransportPlaying = lastTransportPlaying.exchange(state.isPlaying);
    const bool transportPlayEdge = state.isPlaying && !previousTransportPlaying;
    const bool transportStopEdge = !state.isPlaying && previousTransportPlaying;
    const TransportCommand lastCommand = state.lastCommand.load();

    // === UNSYNCED MODE ===
    // Free-running, but controlled by Transport Buttons
    if (!syncToTransport.load())
    {
        // Detect edges for proper pause/resume handling
        const bool risingEdge = transportPlayEdge;  // stop → play
        const bool fallingEdge = transportStopEdge; // play → stop/pause

        // 3. Handle RISING EDGE FIRST (Resume from Pause) - CRITICAL: Restore saved position BEFORE
        // setting playing When resuming from pause, restore the position we saved when pausing
        // This MUST happen before we set playing to true to prevent any race conditions
        if (risingEdge)
        {
            // Rising edge: Resume playback from saved position
            // CRITICAL: Restore position if we have a saved paused position
            double savedPos = pausedNormalizedPosition.load();
            if (savedPos >= 0.0)
            {
                const juce::ScopedLock audioLk(audioLock);
                if (audioReader && audioReader->lengthInSamples > 0)
                {
                    // Clamp saved position to valid range before restoring
                    savedPos = juce::jlimit(0.0, 1.0, savedPos);
                    const juce::int64 targetSamplePos =
                        (juce::int64)(savedPos * audioReader->lengthInSamples);

                    // CRITICAL: Restore the position from when we paused - do this BEFORE setting
                    // playing This ensures the position is set before any audio processing starts
                    currentAudioSamplePosition.store(targetSamplePos);
                    audioReadPosition = (double)targetSamplePos;
                    updateLastKnownNormalizedFromSamples(targetSamplePos);

                    // Reset processing to prevent stale audio
                    audioReader->resetPosition();
                    abstractFifo.reset();
                    timePitch.reset();

                    // CRITICAL FIX: Don't set pendingSeekFrame here!
                    // Setting both pendingSeekFrame and pendingSeekNormalized causes conflicts
                    // The run() thread processes pendingSeekNormalized first, which correctly
                    // restores the position. But then it processes pendingSeekFrame, which
                    // recalculates the position from the frame number and can produce a different
                    // (incorrect) result due to rounding or timing issues.
                    //
                    // Only set pendingSeekNormalized - it's sufficient and more accurate.
                    // The video frame will be synced automatically by the run() thread based on
                    // the audio position.

                    // Update lastPosFrame for loop detection
                    const int tf = totalFrames.load();
                    if (tf > 1)
                    {
                        const int targetFrame =
                            juce::jlimit(0, tf - 1, (int)std::round(savedPos * (tf - 1)));
                        lastPosFrame.store(targetFrame);
                    }

                    // Set pendingSeekNormalized for unified seek system
                    pendingSeekNormalized.store((float)savedPos);

                    // CRITICAL FIX: Clear pendingSeekFrame to prevent run() thread from resetting
                    // position When video file is reloaded during prepareToPlay(), pendingSeekFrame
                    // gets set to 0 which would reset the master clock. By clearing it here, we
                    // prevent that.
                    pendingSeekFrame.store(-1);

                    const double sourceRate = sourceAudioSampleRate.load();
                    if (sourceRate > 0.0)
                    {
                        const double posSeconds = (double)targetSamplePos / sourceRate;
                        juce::Logger::writeToLog(
                            "[VideoLoader Resume] Restored from paused position: " +
                            juce::String(targetSamplePos) + " samples (" +
                            juce::String(posSeconds, 3) +
                            "s, normalized: " + juce::String(savedPos, 3) + ")");
                    }
                }
                // Clear saved position AFTER restoring (we've restored it)
                pausedNormalizedPosition.store(-1.0);
            }
            else
            {
                // If no saved position (pausedNormalizedPosition < 0), just resume from wherever we
                // currently are
                juce::Logger::writeToLog(
                    "[VideoLoader Resume] Playing from current position (no saved pause)");
            }

            // Update tracked command
            lastTransportCommand = state.lastCommand.load();

            // CRITICAL FIX: Update playing flag on rising edge (spacebar/global transport)
            // This ensures the local playing flag matches the transport state when using spacebar
            playing.store(true);
            isStopped.store(false); // No longer stopped when playing
        }

        // 2. Handle FALLING EDGE - Save on Pause, Reset on Stop transition
        // CRITICAL: If command is Pause, NEVER reset - just freeze position
        // Only reset on explicit transition TO Stop command
        if (fallingEdge)
        {
            const TransportCommand currentCommand = state.lastCommand.load();

            // FIRST CHECK: If it's Pause, SAVE current position and freeze
            if (currentCommand == TransportCommand::Pause)
            {
                // Pause: Save current position for resume, do not reset
                // CRITICAL: Get position from master clock directly
                const juce::ScopedLock audioLk(audioLock);
                if (audioReader && audioReader->lengthInSamples > 0)
                {
                    const juce::int64 currentSamplePos = currentAudioSamplePosition.load();
                    const double      normalized =
                        (double)currentSamplePos / audioReader->lengthInSamples;
                    // Always save the position, even if it's 0 (it might be valid at the start)
                    pausedNormalizedPosition.store(normalized);

                    const double sourceRate = sourceAudioSampleRate.load();
                    if (sourceRate > 0.0)
                    {
                        const double posSeconds = (double)currentSamplePos / sourceRate;
                        juce::Logger::writeToLog(
                            "[VideoLoader Pause] Saved position: " +
                            juce::String(currentSamplePos) + " samples (" +
                            juce::String(posSeconds, 3) +
                            "s, normalized: " + juce::String(normalized, 3) + ")");
                    }
                }

                isStopped.store(false); // Paused, not stopped
                lastTransportCommand = currentCommand;
            }
            // SECOND CHECK: Only reset if command TRANSITIONS to Stop (and not Pause)
            else
            {
                const TransportCommand previousCommand = lastTransportCommand;
                // Only reset if command TRANSITIONS to Stop
                if (currentCommand == TransportCommand::Stop &&
                    previousCommand != TransportCommand::Stop)
                {
                    // Explicit transition to Stop: Rewind to range start
                    const juce::ScopedLock audioLk(audioLock);
                    if (audioReader && audioReader->lengthInSamples > 0)
                    {
                        const float       inN = inNormParam ? inNormParam->load() : 0.0f;
                        const juce::int64 startSample =
                            (juce::int64)(inN * audioReader->lengthInSamples);

                        currentAudioSamplePosition.store(startSample);
                        audioReadPosition = (double)startSample;
                        updateLastKnownNormalizedFromSamples(startSample);

                        // Reset processing
                        audioReader->resetPosition();
                        abstractFifo.reset();
                        timePitch.reset();

                        // Seek video to match
                        pendingSeekNormalized.store(inN);
                        const int tf = totalFrames.load();
                        if (tf > 1)
                        {
                            const int startFrame =
                                juce::jlimit(0, tf - 1, (int)std::round(inN * (tf - 1)));
                            pendingSeekFrame.store(startFrame);
                            lastPosFrame.store(startFrame);
                        }

                        // Clear any saved paused position (we're stopping, not pausing)
                        pausedNormalizedPosition.store(-1.0);

                        juce::Logger::writeToLog(
                            "[VideoLoader Stop] Reset to range start: normalized=" +
                            juce::String(inN, 3) + ", sample=" + juce::String(startSample));
                    }

                    isStopped.store(true);
                }
                // For any other case (Play, unknown, or Stop that persists): DO NOTHING - just
                // freeze

                // Update tracked command for next comparison
                lastTransportCommand = currentCommand;
            }

            // CRITICAL FIX: Update playing flag on falling edge (spacebar/global transport)
            // This ensures the local playing flag matches the transport state when using spacebar
            playing.store(false);
        }
        // Not a falling edge and not a rising edge - just update tracked command
        else if (!risingEdge)
        {
            // Update tracked command even when not on any edge (for next time)
            lastTransportCommand = state.lastCommand.load();
        }

        return; // Exit early - don't process synced logic
    }

    // === SYNCED MODE ===
    // Handle stop edge in synced mode
    if (transportStopEdge)
    {
        const TransportCommand currentCommand = lastCommand;
        if (currentCommand == TransportCommand::Stop)
        {
            // In synced mode, stop means reset to range start
            const juce::ScopedLock audioLk(audioLock);
            if (audioReader && audioReader->lengthInSamples > 0)
            {
                const float       inN = inNormParam ? inNormParam->load() : 0.0f;
                const juce::int64 startSample = (juce::int64)(inN * audioReader->lengthInSamples);
                currentAudioSamplePosition.store(startSample);
                audioReadPosition = (double)startSample;
                updateLastKnownNormalizedFromSamples(startSample);
                pendingSeekNormalized.store(inN);

                const int tf = totalFrames.load();
                if (tf > 1)
                {
                    const int startFrame = juce::jlimit(0, tf - 1, (int)std::round(inN * (tf - 1)));
                    pendingSeekFrame.store(startFrame);
                    lastPosFrame.store(startFrame);
                }
            }
            pausedNormalizedPosition.store(-1.0); // Clear saved position
            isStopped.store(true);
        }
        // Pause in synced mode is handled by just setting playing = false
    }

    playing.store(state.isPlaying);

    // CRITICAL: Sync playback position to transport when following timeline
    // This ensures VideoFileLoaderModule stays in sync with TempoClock
    // BUT: Don't override position if we just looped (let loop reset take priority)
    if (state.isPlaying && audioLoaded.load() && audioReader)
    {
        const double sourceRate = sourceAudioSampleRate.load();
        if (sourceRate > 0.0 && state.songPositionSeconds >= 0.0)
        {
            // VARISPEED CALCULATION: Adapt playback speed to tempo/BPM
            // 120 BPM is the reference (1.0x speed)
            // Higher BPM = Faster Playback. Lower BPM = Slower Playback.
            double bpm = state.bpm;
            if (bpm < 1.0)
                bpm = 120.0;
            float  knobVal = speedParam ? speedParam->load() : 1.0f;
            double varispeedSpeed = (bpm / 120.0) * knobVal; // THIS IS THE TEMPO ADAPTATION!

            // Calculate target sample position from transport with varispeed
            // At 2.0x speed, 1 second of transport = 2 seconds of audio covered
            const juce::int64 targetSamplePos =
                (juce::int64)(state.songPositionSeconds * varispeedSpeed * sourceRate);

            // Only update if position has changed significantly (avoid constant seeks)
            const juce::int64 currentPos = currentAudioSamplePosition.load();
            const juce::int64 diff = std::abs(targetSamplePos - currentPos);

            // CRITICAL FIX: Don't sync position if transport position is past the end
            // This prevents transport from overriding loop reset
            // Calculate end position to check bounds
            const float       startNormalized = inNormParam ? inNormParam->load() : 0.0f;
            const float       endNormalized = outNormParam ? outNormParam->load() : 1.0f;
            const juce::int64 startSample =
                (juce::int64)(startNormalized * audioReader->lengthInSamples);
            const juce::int64 endSample =
                (juce::int64)(endNormalized * audioReader->lengthInSamples);

            // Only sync if transport position is within valid range
            // If transport is past the end, let the loop detection handle it
            if (targetSamplePos >= startSample && targetSamplePos < endSample)
            {
                // Update if difference is more than 1 block of samples (prevents micro-seeks)
                if (diff > 512) // ~10ms at 48kHz
                {
                    const juce::ScopedLock audioLk(audioLock);
                    if (audioReader)
                    {
                        // Update master clock to match transport
                        currentAudioSamplePosition.store(targetSamplePos);
                        updateLastKnownNormalizedFromSamples(targetSamplePos);
                        // Update decoder read position
                        audioReadPosition = (double)targetSamplePos;
                        // Reset position for next read
                        audioReader->resetPosition();
                        // Clear FIFO to prevent stale audio
                        abstractFifo.reset();
                        // Reset time stretcher to prevent artifacts
                        timePitch.reset();
                    }
                }
            }
            // If transport position is past the end, don't sync - let loop detection handle it
        }
    }

    if (isTimelineMaster)
    {
        // Timeline masters ignore transport-controlled position updates beyond play/pause mirroring
        return;
    }
}

void VideoFileLoaderModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Although getDynamicOutputPins takes precedence, this ensures correctness if it's ever used as
    // a fallback.
    helpers.drawAudioOutputPin("Source ID", 0);
    helpers.drawAudioOutputPin("Audio L", 1);
    helpers.drawAudioOutputPin("Audio R", 2);
}
#endif

std::vector<DynamicPinInfo> VideoFileLoaderModule::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // CV Output: Bus 0, Channel 0 (mono - contains logical ID for video routing)
    pins.push_back({"Source ID", 0, PinDataType::Video});

    // Audio Outputs: Bus 1, Channels 0-1 (stereo)
    // Note: Channel indices are absolute, so bus 1 channel 0 = absolute channel 1
    // (bus 0 has 1 channel, so bus 1 starts at absolute index 1)
    int bus1StartChannel = 1; // After bus 0's 1 channel
    pins.push_back({"Audio L", bus1StartChannel + 0, PinDataType::Audio});
    pins.push_back({"Audio R", bus1StartChannel + 1, PinDataType::Audio});

    return pins;
}
