#include "YouTubeSourceModule.h"
#include "../graph/ModularSynthProcessor.h"

#ifndef AUDIO_ONLY_BUILD
#include "../../video/VideoFrameManager.h"
#include <opencv2/imgproc.hpp>
#endif

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

// ============================================================================
// Parameter Layout
// ============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout YouTubeSourceModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"loop", 1}, "Loop", true));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"volume", 1},
            "Volume",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.8f));

    return {params.begin(), params.end()};
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

YouTubeSourceModule::YouTubeSourceModule()
#ifndef AUDIO_ONLY_BUILD
    // Full build: Stereo Audio (channels 0,1) + Video ID (channel 2)
    // Audio first so auto-connect (O key) works correctly
    : ModuleProcessor(
          BusesProperties()
              .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)
              .withOutput("Video ID", juce::AudioChannelSet::mono(), true)),
#else
    // Audio-only: Just stereo audio
    : ModuleProcessor(
          BusesProperties().withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
#endif
      juce::Thread("YouTubeSourceThread"),
      apvts(*this, nullptr, "YOUTUBE_SOURCE_PARAMS", createParameterLayout())
{
    loopParam = apvts.getRawParameterValue("loop");
    volumeParam = apvts.getRawParameterValue("volume");

    // Initialize audio FIFO (stereo, lock-free like VideoFileLoaderModule)
    audioFifo.setSize(2, fifoSize, false, true, true);
    abstractFifo.setTotalSize(fifoSize);

    // Initialize download folder in exe directory
    juce::File exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    downloadFolder = exeFile.getParentDirectory().getChildFile("downloads");
    downloadFolder.createDirectory();
    juce::Logger::writeToLog(
        "[YouTubeSource] Download folder: " + downloadFolder.getFullPathName());

    // Check for yt-dlp
    checkForYtDlp();

    // Start background thread
    startThread(juce::Thread::Priority::normal);
}

YouTubeSourceModule::~YouTubeSourceModule()
{
    stopRequested = true;
    signalThreadShouldExit();
    stopThread(2000);

#ifndef AUDIO_ONLY_BUILD
    VideoFrameManager::getInstance().removeSource(getLogicalId());
#endif
}

// ============================================================================
// URL Cleaning
// ============================================================================

juce::String YouTubeSourceModule::cleanYouTubeUrl(const juce::String& url)
{
    juce::String cleaned = url.trim();

    // Extract video ID from various YouTube URL formats
    juce::String videoId;

    // Standard watch URL: youtube.com/watch?v=XXXXX
    if (cleaned.contains("watch?v="))
    {
        int start = cleaned.indexOf("watch?v=") + 8;
        int end = cleaned.indexOf(start, "&");
        if (end < 0)
            end = cleaned.length();
        videoId = cleaned.substring(start, end);
    }
    // Short URL: youtu.be/XXXXX
    else if (cleaned.contains("youtu.be/"))
    {
        int start = cleaned.indexOf("youtu.be/") + 9;
        int end = cleaned.indexOf(start, "?");
        if (end < 0)
            end = cleaned.indexOf(start, "&");
        if (end < 0)
            end = cleaned.length();
        videoId = cleaned.substring(start, end);
    }
    // Embed URL: youtube.com/embed/XXXXX
    else if (cleaned.contains("/embed/"))
    {
        int start = cleaned.indexOf("/embed/") + 7;
        int end = cleaned.indexOf(start, "?");
        if (end < 0)
            end = cleaned.length();
        videoId = cleaned.substring(start, end);
    }

    // If we extracted a video ID, reconstruct a clean URL
    if (videoId.isNotEmpty() && videoId.length() >= 11)
    {
        // Take only the first 11 characters (standard YouTube video ID length)
        videoId = videoId.substring(0, 11);
        return "https://www.youtube.com/watch?v=" + videoId;
    }

    // Return original if we couldn't parse it
    return cleaned;
}

// ============================================================================
// Tool Detection
// ============================================================================

void YouTubeSourceModule::checkForYtDlp()
{
    juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();

    // Check for yt-dlp
    juce::StringArray ytdlpPaths = {
        exeDir.getChildFile("yt-dlp.exe").getFullPathName(),
        exeDir.getChildFile("tools/yt-dlp.exe").getFullPathName(),
        "yt-dlp.exe",
        "yt-dlp"};

    for (const auto& path : ytdlpPaths)
    {
        juce::File exe(path);
        if (exe.existsAsFile())
        {
            ytdlpPath = exe;
            ytdlpFound = true;
            juce::Logger::writeToLog(
                "[YouTubeSource] Found yt-dlp: " + ytdlpPath.getFullPathName());
            break;
        }
    }

    // Try PATH
    if (!ytdlpFound)
    {
        juce::ChildProcess process;
        if (process.start("yt-dlp --version"))
        {
            process.waitForProcessToFinish(3000);
            juce::String output = process.readAllProcessOutput().trim();
            if (output.containsIgnoreCase("20"))
            {
                ytdlpPath = juce::File("yt-dlp");
                ytdlpFound = true;
                juce::Logger::writeToLog("[YouTubeSource] Found yt-dlp in PATH: " + output);
            }
        }
    }

    if (!ytdlpFound)
    {
        statusMessage = "yt-dlp not found";
        juce::Logger::writeToLog("[YouTubeSource] yt-dlp NOT found");
    }
}

// ============================================================================
// Video Info Fetching
// ============================================================================

bool YouTubeSourceModule::fetchVideoInfo(const juce::String& url)
{
    if (!ytdlpFound)
        return false;

    status = Status::Fetching;
    statusMessage = "Fetching info...";

    // Get video title
    juce::String cmd = "\"" + ytdlpPath.getFullPathName() + "\"";
    cmd += " --print title";
    cmd += " --no-playlist";
    cmd += " \"" + url + "\"";

    juce::Logger::writeToLog("[YouTubeSource] Fetching info: " + cmd);

    juce::ChildProcess process;
    if (!process.start(cmd))
        return false;

    process.waitForProcessToFinish(30000);
    juce::String output = process.readAllProcessOutput();

    if (process.getExitCode() == 0)
    {
        videoTitle = output.trim();
        // Truncate long titles
        if (videoTitle.length() > 60)
            videoTitle = videoTitle.substring(0, 57) + "...";

        juce::Logger::writeToLog("[YouTubeSource] Title: " + videoTitle);
        return true;
    }

    return false;
}

// ============================================================================
// Download Logic
// ============================================================================

bool YouTubeSourceModule::downloadVideo(const juce::String& url)
{
    if (!ytdlpFound)
    {
        statusMessage = "yt-dlp not found";
        status = Status::Error;
        return false;
    }

    status = Status::Downloading;
    statusMessage = "Downloading...";
    downloadProgress = 0.0f;

    // Generate filename from URL hash
    juce::String urlHash = juce::String(std::hash<std::string>{}(url.toStdString()));
    juce::File   outputFile = downloadFolder.getChildFile(urlHash + ".mp4");

    // Check cache
    if (outputFile.existsAsFile() && outputFile.getSize() > 10000)
    {
        juce::Logger::writeToLog("[YouTubeSource] Using cached: " + outputFile.getFullPathName());
        downloadedFile = outputFile;
        status = Status::Ready;
        statusMessage = "Cached";
        downloadProgress = 1.0f;
        return true;
    }

    // Build download command - use best single format to avoid needing ffmpeg merge
    juce::String cmd = "\"" + ytdlpPath.getFullPathName() + "\"";
    cmd += " -f \"best[ext=mp4]/best\"";
    cmd += " --newline";
    cmd += " --no-playlist";
    cmd += " --no-warnings"; // Suppress warnings in output
    cmd += " -o \"" + outputFile.getFullPathName() + "\"";
    cmd += " \"" + url + "\"";

    juce::Logger::writeToLog("[YouTubeSource] Download cmd: " + cmd);

    juce::ChildProcess process;
    if (!process.start(cmd))
    {
        statusMessage = "Failed to start download";
        status = Status::Error;
        return false;
    }

    // Monitor download progress
    const int timeoutMs = 600000; // 10 minutes
    double    startTime = juce::Time::getMillisecondCounterHiRes();

    while (process.isRunning())
    {
        if (threadShouldExit() || stopRequested)
        {
            process.kill();
            return false;
        }

        double now = juce::Time::getMillisecondCounterHiRes();
        if (now - startTime > timeoutMs)
        {
            process.kill();
            statusMessage = "Download timeout";
            status = Status::Error;
            return false;
        }

        // Read and parse progress
        char buffer[4096];
        int  bytesRead = process.readProcessOutput(buffer, sizeof(buffer) - 1);
        if (bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            juce::String output(buffer);

            // Parse [download] XX.X% ...
            int idx = output.indexOf("[download]");
            if (idx >= 0)
            {
                int pctIdx = output.indexOf(idx, "%");
                if (pctIdx > idx)
                {
                    int start = pctIdx - 1;
                    while (start > idx && (isdigit(output[start]) || output[start] == '.'))
                        start--;

                    juce::String pctStr = output.substring(start + 1, pctIdx);
                    float        pct = pctStr.getFloatValue();
                    if (pct > 0.0f)
                    {
                        downloadProgress = pct / 100.0f;
                        statusMessage = "Downloading " + juce::String((int)pct) + "%";
                    }
                }
            }
        }

        juce::Thread::sleep(100);
    }

    int exitCode = process.getExitCode();
    juce::Logger::writeToLog("[YouTubeSource] Exit code: " + juce::String(exitCode));

    if (exitCode != 0)
    {
        statusMessage = "Download failed";
        status = Status::Error;
        return false;
    }

    if (!outputFile.existsAsFile())
    {
        // yt-dlp might add format extension, search for the file
        juce::Array<juce::File> files;
        downloadFolder.findChildFiles(files, juce::File::findFiles, false, urlHash + ".*");
        if (files.size() > 0)
        {
            outputFile = files[0];
        }
        else
        {
            statusMessage = "File not found";
            status = Status::Error;
            return false;
        }
    }

    downloadedFile = outputFile;
    downloadProgress = 1.0f;
    status = Status::Ready;
    statusMessage = "Ready";

    juce::Logger::writeToLog("[YouTubeSource] Downloaded: " + downloadedFile.getFullPathName());
    return true;
}

// ============================================================================
// Media Loading
// ============================================================================

void YouTubeSourceModule::loadDownloadedMedia()
{
    if (!downloadedFile.existsAsFile())
    {
        statusMessage = "File not found";
        status = Status::Error;
        return;
    }

    juce::Logger::writeToLog("[YouTubeSource] Loading: " + downloadedFile.getFullPathName());

    // Load audio with resampling to device sample rate
    {
        const juce::ScopedLock lock(audioLock);

        // Get the device sample rate (from prepareToPlay or use default)
        double deviceRate = getSampleRate();
        if (deviceRate <= 0.0)
            deviceRate = 48000.0; // Common default

        // Create reader with target sample rate for automatic resampling
        audioReader =
            std::make_unique<FFmpegAudioReader>(downloadedFile.getFullPathName(), deviceRate);

        if (audioReader && audioReader->lengthInSamples > 0)
        {
            audioLoaded = true;
            audioReadPosition = 0;
            totalDurationSec = (double)audioReader->lengthInSamples / audioReader->sampleRate;

            juce::Logger::writeToLog(
                "[YouTubeSource] Audio: " + juce::String(audioReader->lengthInSamples) +
                " samples @ " + juce::String(audioReader->sampleRate) +
                " Hz (source: " + juce::String(audioReader->getSourceSampleRate()) + " Hz), " +
                juce::String(totalDurationSec.load(), 1) + " sec");

            // Pre-fill ring buffer
            fillAudioBuffer();
        }
        else
        {
            juce::Logger::writeToLog("[YouTubeSource] Failed to load audio");
        }
    }

#ifndef AUDIO_ONLY_BUILD
    // Load video
    {
        const juce::ScopedLock lock(videoLock);

        videoCapture.release();
        if (videoCapture.open(downloadedFile.getFullPathName().toStdString()))
        {
            videoLoaded = true;
            videoWidth = (int)videoCapture.get(cv::CAP_PROP_FRAME_WIDTH);
            videoHeight = (int)videoCapture.get(cv::CAP_PROP_FRAME_HEIGHT);
            videoFps = videoCapture.get(cv::CAP_PROP_FPS);
            if (videoFps <= 0.0)
                videoFps = 30.0;

            juce::Logger::writeToLog(
                "[YouTubeSource] Video: " + juce::String(videoWidth.load()) + "x" +
                juce::String(videoHeight.load()) + " @ " + juce::String(videoFps.load(), 1) +
                " fps, videoLoaded=" + juce::String(videoLoaded.load() ? 1 : 0));
        }
        else
        {
            juce::Logger::writeToLog(
                "[YouTubeSource] Failed to open video capture for: " +
                downloadedFile.getFullPathName());
        }
    }
#endif

    status = Status::Playing;
    statusMessage = "Playing";
    playing = true;

    juce::Logger::writeToLog(
        "[YouTubeSource] State after load: playing=" + juce::String(playing.load() ? 1 : 0) +
        " audioLoaded=" + juce::String(audioLoaded.load() ? 1 : 0) +
        " videoLoaded=" + juce::String(videoLoaded.load() ? 1 : 0));
}

void YouTubeSourceModule::stopPlayback()
{
    playing = false;

    {
        const juce::ScopedLock lock(audioLock);
        audioReader.reset();
        audioLoaded = false;
    }

#ifndef AUDIO_ONLY_BUILD
    {
        const juce::ScopedLock lock(videoLock);
        videoCapture.release();
        videoLoaded = false;
    }

    VideoFrameManager::getInstance().removeSource(getLogicalId());
#endif

    status = Status::Idle;
    statusMessage = "Stopped";
}

// ============================================================================
// Audio Buffer Management (Lock-free FIFO for smooth playback)
// ============================================================================

void YouTubeSourceModule::fillAudioBuffer()
{
    if (!audioLoaded || !audioReader)
        return;

    // Check if FIFO needs filling (lock-free check)
    const int freeSpace = abstractFifo.getFreeSpace();
    if (freeSpace < 4096)
        return; // Buffer is full enough

    // Determine how many samples to read
    const int samplesToRead = juce::jmin(freeSpace, 8192);

    // Read from FFmpeg into a temporary buffer
    juce::AudioBuffer<float> tempBuffer(2, samplesToRead);
    
    // FFmpegAudioReader expects float** when usesFloatingPointData is true
    float* destChannels[2] = {tempBuffer.getWritePointer(0), tempBuffer.getWritePointer(1)};
    
    bool ok = audioReader->readSamples(
        reinterpret_cast<int* const*>(destChannels), 2, 0, audioReadPosition, samplesToRead);

    if (ok)
    {
        // Write to FIFO using lock-free pattern (same as VideoFileLoaderModule)
        int start1, size1, start2, size2;
        abstractFifo.prepareToWrite(samplesToRead, start1, size1, start2, size2);
        
        if (size1 > 0)
        {
            audioFifo.copyFrom(0, start1, tempBuffer, 0, 0, size1);
            audioFifo.copyFrom(1, start1, tempBuffer, 1, 0, size1);
        }
        if (size2 > 0)
        {
            audioFifo.copyFrom(0, start2, tempBuffer, 0, size1, size2);
            audioFifo.copyFrom(1, start2, tempBuffer, 1, size1, size2);
        }
        abstractFifo.finishedWrite(size1 + size2);
        
        audioReadPosition += (size1 + size2);

        // Debug logging every ~1 second
        static int fillCallCount = 0;
        if (++fillCallCount % 50 == 0)
        {
            juce::Logger::writeToLog(
                "[YouTubeSource][Audio] FIFO: available=" +
                juce::String(abstractFifo.getNumReady()) + "/" + juce::String(fifoSize) +
                " filePos=" + juce::String(audioReadPosition) + "/" +
                juce::String(audioReader->lengthInSamples));
        }

        // Handle looping or end
        if (audioReadPosition >= audioReader->lengthInSamples)
        {
            if (loopParam && *loopParam > 0.5f)
            {
                audioReadPosition = 0;
                audioReader->resetPosition();
                juce::Logger::writeToLog("[YouTubeSource][Audio] Looping audio");

#ifndef AUDIO_ONLY_BUILD
                // Reset video too
                const juce::ScopedLock vlock(videoLock);
                if (videoCapture.isOpened())
                    videoCapture.set(cv::CAP_PROP_POS_FRAMES, 0);
#endif
            }
            else
            {
                playing = false;
                status = Status::Ready;
                statusMessage = "Finished";
                juce::Logger::writeToLog("[YouTubeSource][Audio] Playback finished");
            }
        }
    }
    else
    {
        juce::Logger::writeToLog(
            "[YouTubeSource][Audio] readSamples failed at pos " + juce::String(audioReadPosition));
    }
}

// ============================================================================
// Background Thread
// ============================================================================

void YouTubeSourceModule::run()
{
    while (!threadShouldExit())
    {
        // Check for URL change
        if (urlChanged.exchange(false))
        {
            stopPlayback();

            // Clean the URL
            currentUrl = cleanYouTubeUrl(pendingUrl);
            juce::Logger::writeToLog("[YouTubeSource] Cleaned URL: " + currentUrl);

            if (currentUrl.isNotEmpty())
            {
                // Fetch info first
                fetchVideoInfo(currentUrl);

                // Download
                bool success = downloadVideo(currentUrl);

                if (success)
                    loadDownloadedMedia();
            }
        }

        // Keep ring buffer filled while playing
        if (playing.load() && audioLoaded.load())
        {
            fillAudioBuffer();
        }

#ifndef AUDIO_ONLY_BUILD
        // Update video frame
        bool isPlaying = playing.load();
        bool isVideoLoaded = videoLoaded.load();

        // Debug every 2 seconds
        static double lastDebugTime = 0;
        double        debugNow = juce::Time::getMillisecondCounterHiRes();
        if (debugNow - lastDebugTime > 2000)
        {
            lastDebugTime = debugNow;
            juce::Logger::writeToLog(
                "[YouTubeSource][Debug] playing=" + juce::String(isPlaying ? 1 : 0) +
                " videoLoaded=" + juce::String(isVideoLoaded ? 1 : 0) +
                " audioLoaded=" + juce::String(audioLoaded.load() ? 1 : 0) +
                " storedLogicalId=" + juce::String(storedLogicalId) +
                " fifoReady=" + juce::String(abstractFifo.getNumReady()));
        }

        if (isPlaying && isVideoLoaded)
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            double frameInterval = 1000.0 / videoFps.load();

            if (now - lastFrameTimeMs >= frameInterval)
            {
                lastFrameTimeMs = now;

                cv::Mat frame;
                {
                    const juce::ScopedLock lock(videoLock);
                    if (videoCapture.isOpened())
                    {
                        if (!videoCapture.read(frame) || frame.empty())
                        {
                            juce::Logger::writeToLog(
                                "[YouTubeSource][Video] Frame read failed, looping...");
                            if (loopParam && *loopParam > 0.5f)
                            {
                                videoCapture.set(cv::CAP_PROP_POS_FRAMES, 0);
                                videoCapture.read(frame);
                            }
                        }
                    }
                    else
                    {
                        juce::Logger::writeToLog("[YouTubeSource][Video] VideoCapture not opened!");
                    }
                }

                if (!frame.empty())
                {
                    // Publish to VideoFrameManager
                    // Get logical ID, querying parentSynth if not cached
                    juce::uint32 myId = storedLogicalId;
                    if (myId == 0 && parentSynth != nullptr)
                    {
                        for (const auto& info : parentSynth->getModulesInfo())
                        {
                            if (parentSynth->getModuleForLogical(info.first) == this)
                            {
                                myId = info.first;
                                storedLogicalId = myId;
                                juce::Logger::writeToLog(
                                    "[YouTubeSource][Video] Got logical ID: " + juce::String(myId));
                                break;
                            }
                        }
                    }

                    if (myId != 0)
                    {
                        VideoFrameManager::getInstance().setFrame(myId, frame);

                        // Log first frame published
                        static bool firstFrameLogged = false;
                        if (!firstFrameLogged)
                        {
                            juce::Logger::writeToLog(
                                "[YouTubeSource][Video] Publishing frame to VideoFrameManager, "
                                "id=" +
                                juce::String(myId) + " size=" + juce::String(frame.cols) + "x" +
                                juce::String(frame.rows));
                            firstFrameLogged = true;
                        }
                    }
                    else
                    {
                        juce::Logger::writeToLog(
                            "[YouTubeSource][Video] Cannot publish - myId is 0!");
                    }

                    // Update UI preview
                    cv::Mat bgra;
                    if (frame.channels() == 4)
                        bgra = frame;
                    else if (frame.channels() == 3)
                        cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);

                    if (!bgra.empty())
                    {
                        const juce::ScopedLock lock(frameLock);
                        latestFrame = juce::Image(juce::Image::ARGB, bgra.cols, bgra.rows, true);
                        juce::Image::BitmapData destData(
                            latestFrame, juce::Image::BitmapData::writeOnly);
                        memcpy(destData.data, bgra.data, bgra.total() * bgra.elemSize());

                        // Log first UI frame
                        static bool uiFrameLogged = false;
                        if (!uiFrameLogged)
                        {
                            juce::Logger::writeToLog(
                                "[YouTubeSource][Video] UI frame updated: " +
                                juce::String(bgra.cols) + "x" + juce::String(bgra.rows));
                            uiFrameLogged = true;
                        }
                    }
                }
                else
                {
                    juce::Logger::writeToLog("[YouTubeSource][Video] Frame is empty after read!");
                }
            }
        }
#endif

        juce::Thread::sleep(5);
    }
}

// ============================================================================
// Audio Processing
// ============================================================================

void YouTubeSourceModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    double previousRate = audioSampleRate;
    audioSampleRate = sampleRate;

    // If sample rate changed significantly and we have audio loaded, reload with correct rate
    if (audioReader && audioReader->lengthInSamples > 0 &&
        std::abs(audioReader->sampleRate - sampleRate) > 1.0)
    {
        juce::Logger::writeToLog(
            "[YouTubeSource] Sample rate changed from " + juce::String(previousRate) + " to " +
            juce::String(sampleRate) + " Hz, reloading audio with resampling...");

        // Reload the audio file with the new sample rate
        if (downloadedFile.existsAsFile())
        {
            const juce::ScopedLock lock(audioLock);

            // Save current position
            double positionRatio = 0.0;
            if (audioReader->lengthInSamples > 0)
                positionRatio = (double)audioReadPosition / (double)audioReader->lengthInSamples;

            // Recreate reader with new sample rate
            audioReader =
                std::make_unique<FFmpegAudioReader>(downloadedFile.getFullPathName(), sampleRate);

            if (audioReader && audioReader->lengthInSamples > 0)
            {
                // Restore position (approximately)
                audioReadPosition = (juce::int64)(positionRatio * audioReader->lengthInSamples);
                totalDurationSec = (double)audioReader->lengthInSamples / audioReader->sampleRate;

                // Reset FIFO
                abstractFifo.reset();

                juce::Logger::writeToLog(
                    "[YouTubeSource] Audio reloaded: " + juce::String(audioReader->sampleRate) +
                    " Hz (source: " + juce::String(audioReader->getSourceSampleRate()) + " Hz)");
            }
        }
    }
}

void YouTubeSourceModule::releaseResources()
{
    // Ring buffer persists
}

void YouTubeSourceModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

#ifndef AUDIO_ONLY_BUILD
    // Full build: Bus 0 = Audio (stereo, channels 0-1), Bus 1 = Video ID (mono, channel 2)
    auto audioOutBus = getBusBuffer(buffer, false, 0);
    auto videoIdBus = getBusBuffer(buffer, false, 1);

    audioOutBus.clear();
    videoIdBus.clear();

    // Output Source ID on video ID bus (channel 2)
    juce::uint32 myId = storedLogicalId;
    if (myId == 0 && parentSynth != nullptr)
    {
        for (const auto& info : parentSynth->getModulesInfo())
        {
            if (parentSynth->getModuleForLogical(info.first) == this)
            {
                myId = info.first;
                storedLogicalId = myId;
                break;
            }
        }
    }

    if (videoIdBus.getNumChannels() > 0)
    {
        float sourceId = (float)myId;
        for (int i = 0; i < videoIdBus.getNumSamples(); ++i)
            videoIdBus.setSample(0, i, sourceId);

        // Debug: log the source ID being output
        static bool sourceIdLogged = false;
        if (!sourceIdLogged && myId != 0)
        {
            juce::Logger::writeToLog(
                "[YouTubeSource][Video] Outputting source ID=" + juce::String(myId) +
                " on Video bus (bus 1, channel 2)");
            sourceIdLogged = true;
        }
    }
    else
    {
        static bool busWarningLogged = false;
        if (!busWarningLogged)
        {
            juce::Logger::writeToLog("[YouTubeSource][Video] WARNING: videoIdBus has 0 channels!");
            busWarningLogged = true;
        }
    }

    const int   numSamples = audioOutBus.getNumSamples();
    const int   numChannels = juce::jmin(2, audioOutBus.getNumChannels());
    const float volume = volumeParam ? volumeParam->load() : 0.8f;

    if (!audioLoaded.load() || !playing.load() || numChannels == 0)
        return;

    // Read from FIFO using lock-free pattern (same as InternetRadioReceiverModule)
    int start1, size1, start2, size2;
    abstractFifo.prepareToRead(numSamples, start1, size1, start2, size2);
    
    const int totalRead = size1 + size2;
    
    if (totalRead > 0)
    {
        // Copy from FIFO to output
        if (size1 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audioOutBus.copyFrom(ch, 0, audioFifo, ch, start1, size1);
        }
        if (size2 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audioOutBus.copyFrom(ch, size1, audioFifo, ch, start2, size2);
        }
        abstractFifo.finishedRead(totalRead);
        
        // Zero remaining samples if we didn't get enough
        if (totalRead < numSamples)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audioOutBus.clear(ch, totalRead, numSamples - totalRead);
        }
        
        // Apply volume
        if (volume != 1.0f)
            audioOutBus.applyGain(volume);
    }
    else
    {
        // No data available - output silence
        audioOutBus.clear();
    }

    // Track underruns
    if (totalRead < numSamples && totalRead > 0)
    {
        static int underrunCount = 0;
        if (++underrunCount % 100 == 1)
        {
            juce::Logger::writeToLog(
                "[YouTubeSource][Audio] UNDERRUN #" + juce::String(underrunCount) +
                " needed=" + juce::String(numSamples) + " got=" + juce::String(totalRead));
        }
    }

#else
    // Audio-only build: Bus 0 = Audio (stereo)
    auto audioOutBus = getBusBuffer(buffer, false, 0);
    audioOutBus.clear();

    const int   numSamples = audioOutBus.getNumSamples();
    const int   numChannels = juce::jmin(2, audioOutBus.getNumChannels());
    const float volume = volumeParam ? volumeParam->load() : 0.8f;

    if (!audioLoaded.load() || !playing.load() || numChannels == 0)
        return;

    // Read from FIFO using lock-free pattern
    int start1, size1, start2, size2;
    abstractFifo.prepareToRead(numSamples, start1, size1, start2, size2);
    
    const int totalRead = size1 + size2;
    
    if (totalRead > 0)
    {
        if (size1 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audioOutBus.copyFrom(ch, 0, audioFifo, ch, start1, size1);
        }
        if (size2 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audioOutBus.copyFrom(ch, size1, audioFifo, ch, start2, size2);
        }
        abstractFifo.finishedRead(totalRead);
        
        if (totalRead < numSamples)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audioOutBus.clear(ch, totalRead, numSamples - totalRead);
        }
        
        if (volume != 1.0f)
            audioOutBus.applyGain(volume);
    }
    else
    {
        audioOutBus.clear();
    }

    // Track underruns
    if (totalRead < numSamples && totalRead > 0)
    {
        static int underrunCountAO = 0;
        if (++underrunCountAO % 100 == 1)
        {
            juce::Logger::writeToLog(
                "[YouTubeSource][Audio-AO] UNDERRUN #" + juce::String(underrunCountAO) +
                " needed=" + juce::String(numSamples) + " got=" + juce::String(totalRead));
        }
    }
#endif

    // Update position
    if (audioReader && audioReader->lengthInSamples > 0)
    {
        float pos = (float)audioReadPosition / (float)audioReader->lengthInSamples;
        lastKnownPosition = juce::jlimit(0.0f, 1.0f, pos);
    }

#if defined(PRESET_CREATOR_UI)
    // Visualization
#ifndef AUDIO_ONLY_BUILD
    auto& vizBus = audioOutBus;
#else
    auto& vizBus = audioOutBus;
#endif
    const int vizNumSamples = vizBus.getNumSamples();
    const int vizNumChannels = vizBus.getNumChannels();
    const int points = VizData::waveformPoints;
    const int stride = juce::jmax(1, vizNumSamples / points);
    float     peakL = 0.0f, peakR = 0.0f;

    for (int i = 0; i < points; ++i)
    {
        int   sampleIdx = juce::jmin(i * stride, vizNumSamples - 1);
        float sampleL = vizNumChannels > 0 ? vizBus.getSample(0, sampleIdx) : 0.0f;
        float sampleR = vizNumChannels > 1 ? vizBus.getSample(1, sampleIdx) : sampleL;

        vizData.waveformL[i].store(sampleL);
        vizData.waveformR[i].store(sampleR);

        peakL = juce::jmax(peakL, std::abs(sampleL));
        peakR = juce::jmax(peakR, std::abs(sampleR));
    }

    vizData.peakL.store(peakL);
    vizData.peakR.store(peakR);
#endif
}

// ============================================================================
// Video Output
// ============================================================================

#ifndef AUDIO_ONLY_BUILD
juce::Image YouTubeSourceModule::getLatestVideoFrame()
{
    const juce::ScopedLock lock(frameLock);
    return latestFrame.isValid() ? latestFrame.createCopy() : juce::Image();
}
#endif

// ============================================================================
// State Persistence
// ============================================================================

juce::ValueTree YouTubeSourceModule::getExtraStateTree() const
{
    juce::ValueTree tree("YouTubeSourceState");
    tree.setProperty("url", currentUrl, nullptr);
    return tree;
}

void YouTubeSourceModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("YouTubeSourceState"))
    {
        pendingUrl = tree.getProperty("url", "").toString();
        if (pendingUrl.isNotEmpty())
        {
            urlChanged = true;
            juce::Logger::writeToLog("[YouTubeSource] Restoring: " + pendingUrl);
        }
    }
}

// ============================================================================
// UI Drawing
// ============================================================================

#if defined(PRESET_CREATOR_UI)

ImVec2 YouTubeSourceModule::getCustomNodeSize() const
{
#ifndef AUDIO_ONLY_BUILD
    return ImVec2(320.0f, 0.0f);
#else
    return ImVec2(280.0f, 0.0f);
#endif
}

void YouTubeSourceModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // yt-dlp status
    if (!ytdlpFound)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "yt-dlp not found!");
        ImGui::TextWrapped("Place yt-dlp.exe next to the application");
        ImGui::PopItemWidth();
        return;
    }

#ifndef AUDIO_ONLY_BUILD
    // Video info (preview is rendered externally by ImGuiNodeEditorComponent)
    if (videoLoaded.load())
    {
        ImGui::Text("Video: %dx%d", videoWidth.load(), videoHeight.load());
    }
#endif

    // URL input
    ThemeText("YouTube URL", theme.text.section_header);

    if (urlInputBuffer[0] == 0)
    {
        if (currentUrl.isNotEmpty())
            strncpy(urlInputBuffer, currentUrl.toRawUTF8(), sizeof(urlInputBuffer) - 1);
        else if (pendingUrl.isNotEmpty())
            strncpy(urlInputBuffer, pendingUrl.toRawUTF8(), sizeof(urlInputBuffer) - 1);
    }

    float btnWidth = 50.0f;
    ImGui::PushItemWidth(itemWidth - btnWidth - 8.0f);

    // Safe input with paste protection - limit to reasonable URL length
    auto inputCallback = [](ImGuiInputTextCallbackData* data) -> int {
        // Protect against extremely long paste operations
        if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
        {
            // If buffer suddenly jumps to very large size, truncate
            if (data->BufTextLen > 1024)
            {
                data->DeleteChars(1024, data->BufTextLen - 1024);
                juce::Logger::writeToLog("[YouTubeSource] Clipboard paste truncated - too long");
            }
        }
        return 0;
    };

    bool enterPressed = ImGui::InputText(
        "##url",
        urlInputBuffer,
        sizeof(urlInputBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit,
        inputCallback);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    bool fetchPressed = ImGui::Button("Fetch", ImVec2(btnWidth, 0));

    if (enterPressed || fetchPressed)
    {
        pendingUrl = juce::String(urlInputBuffer);
        urlChanged = true;
        onModificationEnded();
    }

    // Video title
    if (videoTitle.isNotEmpty())
    {
        ImGui::TextWrapped("%s", videoTitle.toRawUTF8());
    }

    // Status
    Status currentStatus = status.load();
    ImVec4 statusColor;
    switch (currentStatus)
    {
    case Status::Idle:
        statusColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        break;
    case Status::Fetching:
        statusColor = ImVec4(1.0f, 1.0f, 0.3f, 1.0f);
        break;
    case Status::Downloading:
        statusColor = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
        break;
    case Status::Ready:
        statusColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        break;
    case Status::Playing:
        statusColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        break;
    case Status::Error:
        statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        break;
    }
    ImGui::TextColored(statusColor, "%s", statusMessage.toRawUTF8());

    // Progress bar
    if (currentStatus == Status::Downloading)
    {
        ImGui::ProgressBar(downloadProgress.load(), ImVec2(itemWidth, 0));
    }

    // Transport
    ThemeText("Transport", theme.text.section_header);

    float halfWidth = (itemWidth - 8.0f) / 2.0f;
    bool  isPlaying = playing.load();

    if (ImGui::Button(isPlaying ? "Pause" : "Play", ImVec2(halfWidth, 0)))
    {
        playing = !isPlaying;
        if (playing && status == Status::Ready)
            status = Status::Playing;
        onModificationEnded();
    }

    ImGui::SameLine();

    bool loop = loopParam && *loopParam > 0.5f;
    if (ImGui::Button(loop ? "Loop: ON" : "Loop: OFF", ImVec2(halfWidth, 0)))
    {
        if (auto* p = apvts.getParameter("loop"))
            p->setValueNotifyingHost(loop ? 0.0f : 1.0f);
        onModificationEnded();
    }

    // Volume
    float vol = volumeParam ? volumeParam->load() : 0.8f;
    if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f, "%.2f"))
    {
        if (auto* p = apvts.getParameter("volume"))
            p->setValueNotifyingHost(vol);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();

    // Position (read-only display)
    float pos = lastKnownPosition.load();
    ImGui::SliderFloat("Position", &pos, 0.0f, 1.0f, "%.2f");

    // Audio waveform
    ThemeText("Audio", theme.text.section_header);
    {
        float waveL[VizData::waveformPoints];
        float waveR[VizData::waveformPoints];
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            waveL[i] = vizData.waveformL[i].load();
            waveR[i] = vizData.waveformR[i].load();
        }

        const float  scopeHeight = 40.0f;
        const ImVec2 graphSize(itemWidth, scopeHeight);

        if (ImGui::BeginChild(
                "##scope",
                graphSize,
                false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);

            dl->AddRectFilled(p0, p1, IM_COL32(26, 26, 31, 255));
            dl->PushClipRect(p0, p1, true);

            float centerY = p0.y + graphSize.y * 0.5f;
            dl->AddLine(ImVec2(p0.x, centerY), ImVec2(p1.x, centerY), IM_COL32(60, 60, 60, 255));

            const float halfH = graphSize.y * 0.45f;
            const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

            for (int i = 0; i < VizData::waveformPoints - 1; ++i)
            {
                float x0 = p0.x + i * stepX;
                float x1 = p0.x + (i + 1) * stepX;

                dl->AddLine(
                    ImVec2(x0, centerY - waveL[i] * halfH),
                    ImVec2(x1, centerY - waveL[i + 1] * halfH),
                    IM_COL32(100, 255, 100, 200),
                    1.5f);

                dl->AddLine(
                    ImVec2(x0, centerY - waveR[i] * halfH),
                    ImVec2(x1, centerY - waveR[i + 1] * halfH),
                    IM_COL32(200, 100, 255, 200),
                    1.5f);
            }

            dl->PopClipRect();
        }
        ImGui::EndChild();
    }

    // Download folder info
    ImGui::TextDisabled("Downloads: %s", downloadFolder.getFileName().toRawUTF8());

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();
}

void YouTubeSourceModule::drawIoPins(const NodePinHelpers& helpers)
{
#ifndef AUDIO_ONLY_BUILD
    // Full build: Audio L (pin 0), Audio R (pin 1), Video (pin 2)
    // Audio first for auto-connect compatibility
    helpers.drawAudioOutputPin("Audio L", 0);
    helpers.drawAudioOutputPin("Audio R", 1);
    helpers.drawAudioOutputPin("Video", 2);
#else
    // Audio-only: Audio L (pin 0), Audio R (pin 1)
    helpers.drawAudioOutputPin("Audio L", 0);
    helpers.drawAudioOutputPin("Audio R", 1);
#endif
}

#endif // PRESET_CREATOR_UI
