#include "NdiReceiverModule.h"
#include "../../video/VideoFrameManager.h"
#include "../graph/ModularSynthProcessor.h"
#include <opencv2/imgproc.hpp>

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

// Platform-specific DLL loading
#ifdef _WIN32
#include <windows.h>
#define NDI_LIB_NAME "Processing.NDI.Lib.x64.dll"
#else
#include <dlfcn.h>
#ifdef __APPLE__
#define NDI_LIB_NAME "libndi.dylib"
#else
#define NDI_LIB_NAME "libndi.so.6"
#endif
#endif

juce::AudioProcessorValueTreeState::ParameterLayout NdiReceiverModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterInt>("sourceIndex", "Source Index", -1, 31, -1));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "zoomLevel", "Node Size", juce::StringArray{"Small", "Normal", "Large"}, 1));

    return {params.begin(), params.end()};
}

bool NdiReceiverModule::loadNdiLibrary()
{
    if (ndiLibLoaded)
        return true;

#ifdef _WIN32
    // Try to load from system path first (NDI Runtime installs here)
    ndiLibHandle = LoadLibraryA(NDI_LIB_NAME);

    // If not found, try Program Files locations
    if (ndiLibHandle == nullptr)
    {
        // Try NDI Runtime location
        ndiLibHandle =
            LoadLibraryA("C:\\Program Files\\NDI\\NDI 6 Runtime\\v6\\Processing.NDI.Lib.x64.dll");
    }
    if (ndiLibHandle == nullptr)
    {
        // Try NDI SDK location
        ndiLibHandle =
            LoadLibraryA("C:\\Program Files\\NDI\\NDI 6 SDK\\Lib\\x64\\Processing.NDI.Lib.x64.dll");
    }
    if (ndiLibHandle == nullptr)
    {
        // Try NDI Tools Runtime location
        ndiLibHandle = LoadLibraryA(
            "C:\\Program Files\\NDI\\NDI 6 Tools\\Runtime\\Processing.NDI.Lib.x64.dll");
    }
    if (ndiLibHandle == nullptr)
    {
        juce::Logger::writeToLog(
            "[NdiReceiver] Failed to load NDI library - install NDI Runtime from ndi.video");
        return false;
    }

    // Load function pointers
    fn_initialize =
        (pfn_NDIlib_initialize)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_initialize");
    fn_destroy = (pfn_NDIlib_destroy)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_destroy");
    fn_find_create_v2 =
        (pfn_NDIlib_find_create_v2)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_find_create_v2");
    fn_find_destroy =
        (pfn_NDIlib_find_destroy)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_find_destroy");
    fn_find_get_current_sources = (pfn_NDIlib_find_get_current_sources)GetProcAddress(
        (HMODULE)ndiLibHandle, "NDIlib_find_get_current_sources");
    fn_find_wait_for_sources = (pfn_NDIlib_find_wait_for_sources)GetProcAddress(
        (HMODULE)ndiLibHandle, "NDIlib_find_wait_for_sources");
    fn_recv_create_v3 =
        (pfn_NDIlib_recv_create_v3)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_recv_create_v3");
    fn_recv_destroy =
        (pfn_NDIlib_recv_destroy)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_recv_destroy");
    fn_recv_capture_v2 =
        (pfn_NDIlib_recv_capture_v2)GetProcAddress((HMODULE)ndiLibHandle, "NDIlib_recv_capture_v2");
    fn_recv_free_video_v2 = (pfn_NDIlib_recv_free_video_v2)GetProcAddress(
        (HMODULE)ndiLibHandle, "NDIlib_recv_free_video_v2");
    fn_recv_free_audio_v2 = (pfn_NDIlib_recv_free_audio_v2)GetProcAddress(
        (HMODULE)ndiLibHandle, "NDIlib_recv_free_audio_v2");
    fn_recv_free_metadata = (pfn_NDIlib_recv_free_metadata)GetProcAddress(
        (HMODULE)ndiLibHandle, "NDIlib_recv_free_metadata");
#else
    ndiLibHandle = dlopen(NDI_LIB_NAME, RTLD_NOW);
    if (ndiLibHandle == nullptr)
    {
        juce::Logger::writeToLog(
            "[NdiReceiver] Failed to load NDI library: " + juce::String(dlerror()));
        return false;
    }

    fn_initialize = (pfn_NDIlib_initialize)dlsym(ndiLibHandle, "NDIlib_initialize");
    fn_destroy = (pfn_NDIlib_destroy)dlsym(ndiLibHandle, "NDIlib_destroy");
    fn_find_create_v2 = (pfn_NDIlib_find_create_v2)dlsym(ndiLibHandle, "NDIlib_find_create_v2");
    fn_find_destroy = (pfn_NDIlib_find_destroy)dlsym(ndiLibHandle, "NDIlib_find_destroy");
    fn_find_get_current_sources =
        (pfn_NDIlib_find_get_current_sources)dlsym(ndiLibHandle, "NDIlib_find_get_current_sources");
    fn_find_wait_for_sources =
        (pfn_NDIlib_find_wait_for_sources)dlsym(ndiLibHandle, "NDIlib_find_wait_for_sources");
    fn_recv_create_v3 = (pfn_NDIlib_recv_create_v3)dlsym(ndiLibHandle, "NDIlib_recv_create_v3");
    fn_recv_destroy = (pfn_NDIlib_recv_destroy)dlsym(ndiLibHandle, "NDIlib_recv_destroy");
    fn_recv_capture_v2 = (pfn_NDIlib_recv_capture_v2)dlsym(ndiLibHandle, "NDIlib_recv_capture_v2");
    fn_recv_free_video_v2 =
        (pfn_NDIlib_recv_free_video_v2)dlsym(ndiLibHandle, "NDIlib_recv_free_video_v2");
    fn_recv_free_audio_v2 =
        (pfn_NDIlib_recv_free_audio_v2)dlsym(ndiLibHandle, "NDIlib_recv_free_audio_v2");
    fn_recv_free_metadata =
        (pfn_NDIlib_recv_free_metadata)dlsym(ndiLibHandle, "NDIlib_recv_free_metadata");
#endif

    // Verify all functions loaded
    if (!fn_initialize || !fn_destroy || !fn_find_create_v2 || !fn_find_destroy ||
        !fn_find_get_current_sources || !fn_find_wait_for_sources || !fn_recv_create_v3 ||
        !fn_recv_destroy || !fn_recv_capture_v2 || !fn_recv_free_video_v2 ||
        !fn_recv_free_audio_v2 || !fn_recv_free_metadata)
    {
        juce::Logger::writeToLog("[NdiReceiver] Failed to load all NDI functions");
        unloadNdiLibrary();
        return false;
    }

    ndiLibLoaded = true;
    juce::Logger::writeToLog("[NdiReceiver] NDI library loaded successfully");
    return true;
}

void NdiReceiverModule::unloadNdiLibrary()
{
    if (ndiLibHandle != nullptr)
    {
#ifdef _WIN32
        FreeLibrary((HMODULE)ndiLibHandle);
#else
        dlclose(ndiLibHandle);
#endif
        ndiLibHandle = nullptr;
    }
    ndiLibLoaded = false;

    // Clear function pointers
    fn_initialize = nullptr;
    fn_destroy = nullptr;
    fn_find_create_v2 = nullptr;
    fn_find_destroy = nullptr;
    fn_find_get_current_sources = nullptr;
    fn_find_wait_for_sources = nullptr;
    fn_recv_create_v3 = nullptr;
    fn_recv_destroy = nullptr;
    fn_recv_capture_v2 = nullptr;
    fn_recv_free_video_v2 = nullptr;
    fn_recv_free_audio_v2 = nullptr;
    fn_recv_free_metadata = nullptr;
}

NdiReceiverModule::NdiReceiverModule()
    : ModuleProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("NDI Receiver Thread"),
      apvts(*this, nullptr, "NdiReceiverParams", createParameterLayout())
{
    sourceIndexParam = apvts.getRawParameterValue("sourceIndex");
    zoomLevelParam = apvts.getRawParameterValue("zoomLevel");

    // Try to load NDI library at runtime
    if (!loadNdiLibrary())
    {
        connectionStatus = "NDI not installed";
        juce::Logger::writeToLog("[NdiReceiver] NDI Runtime not found - install from ndi.video");
        return;
    }

    // Initialize NDI library
    if (!fn_initialize())
    {
        juce::Logger::writeToLog("[NdiReceiver] Failed to initialize NDI library!");
        connectionStatus = "NDI init failed";
        return;
    }

    // Create NDI finder for discovering sources
    NDIlib_find_create_t findDesc;
    findDesc.show_local_sources = true;
    findDesc.p_groups = nullptr;
    findDesc.p_extra_ips = nullptr;

    ndiFinder = fn_find_create_v2(&findDesc);
    if (ndiFinder == nullptr)
    {
        juce::Logger::writeToLog("[NdiReceiver] Failed to create NDI finder!");
        connectionStatus = "Finder init failed";
    }
    else
    {
        juce::Logger::writeToLog("[NdiReceiver] NDI library initialized successfully");
        connectionStatus = "Ready";
    }
}

NdiReceiverModule::~NdiReceiverModule()
{
    stopThread(5000);

    if (ndiReceiver != nullptr && fn_recv_destroy)
    {
        fn_recv_destroy(ndiReceiver);
        ndiReceiver = nullptr;
    }

    if (ndiFinder != nullptr && fn_find_destroy)
    {
        fn_find_destroy(ndiFinder);
        ndiFinder = nullptr;
    }

    // Note: We don't call fn_destroy() here because other modules might still be using NDI
    // The library will be cleaned up when the application exits

    VideoFrameManager::getInstance().removeSource(getLogicalId());
}

void NdiReceiverModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    if (ndiLibLoaded)
        startThread(juce::Thread::Priority::normal);
}

void NdiReceiverModule::releaseResources() { signalThreadShouldExit(); }

void NdiReceiverModule::refreshSourceList()
{
    if (ndiFinder == nullptr || !fn_find_get_current_sources)
        return;

    // Get current sources
    uint32_t               numSources = 0;
    const NDIlib_source_t* sources = fn_find_get_current_sources(ndiFinder, &numSources);

    const juce::ScopedLock lock(sourcesLock);
    availableSources.clear();
    sourceDetails.clear();

    if (numSources == 0)
    {
        availableSources.add("No NDI sources found");
        juce::Logger::writeToLog("[NdiReceiver] No NDI sources found on network");
    }
    else
    {
        for (uint32_t i = 0; i < numSources; ++i)
        {
            juce::String sourceName = juce::String::fromUTF8(sources[i].p_ndi_name);
            availableSources.add(sourceName);
            sourceDetails.push_back(
                {sources[i].p_ndi_name ? sources[i].p_ndi_name : "",
                 sources[i].p_url_address ? sources[i].p_url_address : ""});
            juce::Logger::writeToLog("[NdiReceiver] Found source: " + sourceName);
        }
    }
}

bool NdiReceiverModule::connectToSource(int sourceIndex)
{
    if (ndiFinder == nullptr || sourceIndex < 0 || !fn_find_get_current_sources ||
        !fn_recv_create_v3)
        return false;

    // Get source info
    uint32_t               numSources = 0;
    const NDIlib_source_t* sources = fn_find_get_current_sources(ndiFinder, &numSources);

    if (sourceIndex >= (int)numSources)
        return false;

    // Disconnect existing receiver
    disconnectFromSource();

    // Create receiver for the selected source
    NDIlib_recv_create_v3_t recvDesc;
    recvDesc.source_to_connect_to = sources[sourceIndex];
    recvDesc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvDesc.bandwidth = NDIlib_recv_bandwidth_highest;
    recvDesc.allow_video_fields = false;
    recvDesc.p_ndi_recv_name = "Collider NDI Receiver";

    ndiReceiver = fn_recv_create_v3(&recvDesc);
    if (ndiReceiver == nullptr)
    {
        juce::Logger::writeToLog(
            "[NdiReceiver] Failed to create receiver for: " +
            juce::String::fromUTF8(sources[sourceIndex].p_ndi_name));
        connectionStatus = "Connection failed";
        return false;
    }

    currentSourceIndex = sourceIndex;
    isConnected = true;
    lastConnectedSourceName = juce::String::fromUTF8(sources[sourceIndex].p_ndi_name);
    connectionStatus = "Connected: " + lastConnectedSourceName;

    juce::Logger::writeToLog("[NdiReceiver] Connected to: " + lastConnectedSourceName);
    return true;
}

void NdiReceiverModule::disconnectFromSource()
{
    if (ndiReceiver != nullptr && fn_recv_destroy)
    {
        fn_recv_destroy(ndiReceiver);
        ndiReceiver = nullptr;
    }

    isConnected = false;
    currentSourceIndex = -1;
    actualWidth = 0;
    actualHeight = 0;
    actualFps = 0.0f;
    connectionStatus = "Disconnected";
}

void NdiReceiverModule::run()
{
    if (!ndiLibLoaded)
    {
        juce::Logger::writeToLog("[NdiReceiver] Thread exiting - NDI not loaded");
        return;
    }

    // Use helper to resolve ID
    juce::uint32 myLogicalId = getMyLogicalId();

    // Initial source discovery
    refreshSourceList();

    while (!threadShouldExit())
    {
        // Check for refresh request
        if (refreshRequested.exchange(false))
        {
            // Wait a moment for NDI discovery to find sources
            if (fn_find_wait_for_sources)
                fn_find_wait_for_sources(ndiFinder, 1000);
            refreshSourceList();
        }

        // Check for source change request
        int requestedIndex = (int)sourceIndexParam->load();
        if (requestedIndex != currentSourceIndex.load())
        {
            if (requestedIndex >= 0)
            {
                connectToSource(requestedIndex);
            }
            else
            {
                disconnectFromSource();
            }
        }

        // Receive video frames if connected
        if (ndiReceiver != nullptr && isConnected && fn_recv_capture_v2)
        {
            NDIlib_video_frame_v2_t videoFrame;
            NDIlib_audio_frame_v2_t audioFrame;
            NDIlib_metadata_frame_t metadataFrame;

            // Initialize structures
            memset(&videoFrame, 0, sizeof(videoFrame));
            memset(&audioFrame, 0, sizeof(audioFrame));
            memset(&metadataFrame, 0, sizeof(metadataFrame));

            // Try to capture a frame (100ms timeout)
            NDIlib_frame_type_e frameType =
                fn_recv_capture_v2(ndiReceiver, &videoFrame, &audioFrame, &metadataFrame, 100);

            switch (frameType)
            {
            case NDIlib_frame_type_video:
            {
                auto t0 = juce::Time::getMillisecondCounterHiRes();

                // Update stream info
                actualWidth = videoFrame.xres;
                actualHeight = videoFrame.yres;
                if (videoFrame.frame_rate_D > 0)
                    actualFps = (float)videoFrame.frame_rate_N / (float)videoFrame.frame_rate_D;

                // Convert NDI frame to OpenCV Mat
                // NDI sends BGRA when we request NDIlib_recv_color_format_BGRX_BGRA
                cv::Mat bgraFrame(
                    videoFrame.yres,
                    videoFrame.xres,
                    CV_8UC4,
                    videoFrame.p_data,
                    videoFrame.line_stride_in_bytes);

                // Convert BGRA to BGR for VideoFrameManager
                cv::Mat bgrFrame;
                cv::cvtColor(bgraFrame, bgrFrame, cv::COLOR_BGRA2BGR);

                // Publish frame to central manager
                if (myLogicalId == 0)
                    myLogicalId = getMyLogicalId();

                VideoFrameManager::getInstance().setFrame(myLogicalId, bgrFrame);

                // Update local preview for UI
                updateGuiFrame(bgrFrame);

                auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
                lastProcessTimeMs = (float)elapsed;
                lastProcessWasGpu = false;

                // Free the video frame
                if (fn_recv_free_video_v2)
                    fn_recv_free_video_v2(ndiReceiver, &videoFrame);
                break;
            }

            case NDIlib_frame_type_audio:
            {
                // Store incoming audio in ring buffer
                ndiSampleRate.store(audioFrame.sample_rate);
                ndiNumChannels.store(audioFrame.no_channels);
                vizData.audioFramesReceived.fetch_add(1);

                int numSamples = audioFrame.no_samples;
                int numChannels = juce::jmin(2, audioFrame.no_channels); // Max stereo

                // Write to ring buffer (thread-safe)
                int start1, size1, start2, size2;
                audioFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

                int totalWritten = size1 + size2;
                if (totalWritten > 0)
                {
                    // NDI audio is planar: each channel is contiguous
                    // channel_stride_in_bytes is the stride between channels
                    const int channelStride = audioFrame.channel_stride_in_bytes / sizeof(float);

                    if (size1 > 0)
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            const float* src = audioFrame.p_data + (ch * channelStride);
                            audioRingBuffer.copyFrom(ch, start1, src, size1);
                        }
                    }
                    if (size2 > 0)
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            const float* src = audioFrame.p_data + (ch * channelStride) + size1;
                            audioRingBuffer.copyFrom(ch, start2, src, size2);
                        }
                    }
                    audioFifo.finishedWrite(totalWritten);
                    vizData.audioSamplesBuffered.fetch_add(totalWritten);
                }

                if (fn_recv_free_audio_v2)
                    fn_recv_free_audio_v2(ndiReceiver, &audioFrame);
                break;
            }

            case NDIlib_frame_type_metadata:
                if (fn_recv_free_metadata)
                    fn_recv_free_metadata(ndiReceiver, &metadataFrame);
                break;

            case NDIlib_frame_type_error:
                juce::Logger::writeToLog("[NdiReceiver] Connection error");
                connectionStatus = "Connection error";
                disconnectFromSource();
                break;

            case NDIlib_frame_type_none:
            default:
                // No frame available, just continue
                break;
            }
        }
        else
        {
            // Not connected, wait a bit before checking again
            wait(100);
        }
    }

    disconnectFromSource();
    if (myLogicalId != 0)
        VideoFrameManager::getInstance().removeSource(myLogicalId);
}

juce::uint32 NdiReceiverModule::getMyLogicalId()
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

void NdiReceiverModule::updateGuiFrame(const cv::Mat& frame)
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

juce::Image NdiReceiverModule::getLatestFrame()
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

void NdiReceiverModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(2, buffer.getNumChannels());

    // Read from ring buffer
    int start1, size1, start2, size2;
    audioFifo.prepareToRead(numSamples, start1, size1, start2, size2);

    int totalRead = size1 + size2;

    if (totalRead > 0)
    {
        // We have audio data
        if (size1 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom(ch, 0, audioRingBuffer, ch, start1, size1);
        }
        if (size2 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom(ch, size1, audioRingBuffer, ch, start2, size2);
        }
        audioFifo.finishedRead(totalRead);

        // Zero remaining samples if we didn't get enough
        if (totalRead < numSamples)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.clear(ch, totalRead, numSamples - totalRead);
        }

        // If mono NDI source, duplicate left to right
        if (ndiNumChannels.load() == 1 && numChannels >= 2)
        {
            buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
        }
    }
    else
    {
        // No audio available - output silence
        buffer.clear();
    }

#if defined(PRESET_CREATOR_UI)
    // Capture waveform for visualization
    {
        const int points = VizData::waveformPoints;
        const int stride = juce::jmax(1, numSamples / points);
        float     peakL = 0.0f, peakR = 0.0f;

        for (int i = 0; i < points; ++i)
        {
            int sampleIdx = juce::jmin((i * stride) + (stride / 2), numSamples - 1);

            float sampleL = (numChannels > 0) ? buffer.getSample(0, sampleIdx) : 0.0f;
            float sampleR = (numChannels > 1) ? buffer.getSample(1, sampleIdx) : sampleL;

            vizData.waveformL[i].store(sampleL);
            vizData.waveformR[i].store(sampleR);

            peakL = juce::jmax(peakL, std::abs(sampleL));
            peakR = juce::jmax(peakR, std::abs(sampleR));
        }

        vizData.peakL.store(peakL);
        vizData.peakR.store(peakR);
    }
#endif
}

juce::ValueTree NdiReceiverModule::getExtraStateTree() const
{
    juce::ValueTree tree("NdiReceiverState");
    tree.setProperty("lastSourceName", lastConnectedSourceName, nullptr);
    return tree;
}

void NdiReceiverModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("NdiReceiverState"))
    {
        lastConnectedSourceName = tree.getProperty("lastSourceName", "").toString();

        // Try to reconnect to the saved source on next refresh
        if (lastConnectedSourceName.isNotEmpty())
        {
            juce::Logger::writeToLog(
                "[NdiReceiver] Will attempt to reconnect to: " + lastConnectedSourceName);
        }
    }
}

#if defined(PRESET_CREATOR_UI)
ImVec2 NdiReceiverModule::getCustomNodeSize() const
{
    // Return different width based on zoom level (0=240, 1=480, 2=960)
    int level = zoomLevelParam ? (int)zoomLevelParam->load() : 1;
    level = juce::jlimit(0, 2, level);
    const float widths[3]{240.0f, 480.0f, 960.0f};
    return ImVec2(widths[level], 0.0f);
}

void NdiReceiverModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // Check if NDI is available
    if (!ndiLibLoaded)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "NDI Runtime not installed");
        ImGui::TextWrapped("Install from ndi.video");
        ImGui::PopItemWidth();
        return;
    }

    // Refresh button
    if (ImGui::Button("Refresh Sources"))
    {
        refreshRequested = true;
    }
    ImGui::SameLine();

    // Get current sources
    juce::StringArray currentSources;
    {
        const juce::ScopedLock lock(sourcesLock);
        currentSources = availableSources;
    }

    if (currentSources.isEmpty())
    {
        currentSources.add("No sources");
    }

    int currentIndex = (int)sourceIndexParam->load();
    currentIndex = juce::jlimit(-1, juce::jmax(0, currentSources.size() - 1), currentIndex);

    const char* currentSourceName = (currentIndex >= 0 && currentIndex < currentSources.size())
                                        ? currentSources[currentIndex].toRawUTF8()
                                        : "Select source...";

    bool sourceModulated = isParamModulated("sourceIndex");
    if (sourceModulated)
        ImGui::BeginDisabled();

    if (ImGui::BeginCombo("Source", currentSourceName))
    {
        // Add "None" option
        if (ImGui::Selectable("(None)", currentIndex < 0))
        {
            *dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("sourceIndex")) = -1;
            onModificationEnded();
        }

        for (int i = 0; i < currentSources.size(); ++i)
        {
            const bool          isSelected = (currentIndex == i);
            const juce::String& sourceName = currentSources[i];

            // Don't allow selecting placeholder text
            bool isSelectable = !sourceName.startsWith("No ");

            if (!isSelectable)
                ImGui::BeginDisabled();

            if (ImGui::Selectable(sourceName.toRawUTF8(), isSelected))
            {
                *dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("sourceIndex")) = i;
                onModificationEnded();
            }

            if (!isSelectable)
                ImGui::EndDisabled();

            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Scroll-edit for source combo (when not modulated)
    if (!sourceModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int maxIndex = juce::jmax(0, currentSources.size() - 1);
            // Scroll down = next source, scroll up = previous
            const int newIndex = juce::jlimit(-1, maxIndex, currentIndex + (wheel > 0.0f ? -1 : 1));
            if (newIndex != currentIndex)
            {
                if (auto* p =
                        dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("sourceIndex")))
                {
                    *p = newIndex;
                    onModificationEnded();
                }
            }
        }
    }

    if (sourceModulated)
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
    if (atMin)
        ImGui::BeginDisabled();
    if (ImGui::Button("-", ImVec2(buttonWidth, 0)))
    {
        int newLevel = juce::jmax(0, level - 1);
        if (auto* p = apvts.getParameter("zoomLevel"))
            p->setValueNotifyingHost((float)newLevel / 2.0f);
        onModificationEnded();
    }
    bool minusHovered = ImGui::IsItemHovered();
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
    bool plusHovered = ImGui::IsItemHovered();
    if (atMax)
        ImGui::EndDisabled();

    // Scroll-edit for zoom (when hovering either button)
    if (!zoomModulated && (minusHovered || plusHovered))
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            // Scroll up = zoom in (+), scroll down = zoom out (-)
            const int newLevel = juce::jlimit(0, 2, level + (wheel > 0.0f ? 1 : -1));
            if (newLevel != level)
            {
                if (auto* p = apvts.getParameter("zoomLevel"))
                {
                    p->setValueNotifyingHost((float)newLevel / 2.0f);
                    onModificationEnded();
                }
            }
        }
    }

    if (zoomModulated)
        ImGui::EndDisabled();

    // Connection status
    ImGui::TextColored(
        isConnected ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "%s",
        connectionStatus.toRawUTF8());

    // Source ID
    const juce::String sourceText = juce::String::formatted("Source ID: %d", (int)getLogicalId());
    ThemeText(sourceText.toRawUTF8(), theme.text.section_header);

    // Display stream info if connected
    if (actualWidth > 0)
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Info:");
        ImGui::Text("%dx%d @ %.1f FPS", actualWidth.load(), actualHeight.load(), actualFps.load());
    }

    // === AUDIO SCOPE VISUALIZATION ===
    {
        ThemeText("Audio", theme.text.section_header);

        // Read waveform data from VizData atomics
        float waveL[VizData::waveformPoints];
        float waveR[VizData::waveformPoints];
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            waveL[i] = vizData.waveformL[i].load();
            waveR[i] = vizData.waveformR[i].load();
        }
        float peakL = vizData.peakL.load();
        float peakR = vizData.peakR.load();
        int   framesReceived = vizData.audioFramesReceived.load();

        const float            scopeHeight = 50.0f;
        const ImVec2           graphSize(itemWidth, scopeHeight);
        const ImGuiWindowFlags childFlags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        if (ImGui::BeginChild("NdiAudioScope", graphSize, false, childFlags))
        {
            ImDrawList*  drawList = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);

            // Background
            const ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
            drawList->AddRectFilled(p0, p1, bgColor);

            // Clip rect
            drawList->PushClipRect(p0, p1, true);

            // Center line
            const float centerY = p0.y + graphSize.y * 0.5f;
            drawList->AddLine(
                ImVec2(p0.x, centerY), ImVec2(p1.x, centerY), IM_COL32(60, 60, 60, 255), 1.0f);

            // Draw waveforms (L=green, R=purple)
            const ImU32 colorL = IM_COL32(100, 255, 100, 200); // Green for left
            const ImU32 colorR = IM_COL32(200, 100, 255, 200); // Purple for right
            const float halfHeight = graphSize.y * 0.45f;
            const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

            // Left channel waveform
            for (int i = 0; i < VizData::waveformPoints - 1; ++i)
            {
                float x0 = p0.x + i * stepX;
                float x1 = p0.x + (i + 1) * stepX;
                float y0 = centerY - waveL[i] * halfHeight;
                float y1 = centerY - waveL[i + 1] * halfHeight;
                y0 = juce::jlimit(p0.y, p1.y, y0);
                y1 = juce::jlimit(p0.y, p1.y, y1);
                drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorL, 1.5f);
            }

            // Right channel waveform (offset slightly)
            for (int i = 0; i < VizData::waveformPoints - 1; ++i)
            {
                float x0 = p0.x + i * stepX;
                float x1 = p0.x + (i + 1) * stepX;
                float y0 = centerY - waveR[i] * halfHeight;
                float y1 = centerY - waveR[i + 1] * halfHeight;
                y0 = juce::jlimit(p0.y, p1.y, y0);
                y1 = juce::jlimit(p0.y, p1.y, y1);
                drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorR, 1.5f);
            }

            drawList->PopClipRect();

            // Peak meters on right edge
            const float meterWidth = 4.0f;
            const float meterHeight = graphSize.y - 4.0f;
            float       meterX = p1.x - meterWidth * 2 - 4.0f;

            // Left peak meter
            float peakLH = juce::jlimit(0.0f, 1.0f, peakL) * meterHeight;
            drawList->AddRectFilled(
                ImVec2(meterX, p1.y - 2.0f - peakLH),
                ImVec2(meterX + meterWidth, p1.y - 2.0f),
                colorL);

            // Right peak meter
            meterX += meterWidth + 2.0f;
            float peakRH = juce::jlimit(0.0f, 1.0f, peakR) * meterHeight;
            drawList->AddRectFilled(
                ImVec2(meterX, p1.y - 2.0f - peakRH),
                ImVec2(meterX + meterWidth, p1.y - 2.0f),
                colorR);

            // No audio indicator
            if (framesReceived == 0 || (peakL < 0.001f && peakR < 0.001f))
            {
                ImGui::SetCursorPos(ImVec2(4, 4));
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.2f, 1.0f), "No audio");
            }

            // Drag blocker
            ImGui::SetCursorPos(ImVec2(0, 0));
            ImGui::InvisibleButton("##ndiAudioScopeDrag", graphSize);
        }
        ImGui::EndChild();

        // Debug stats
        ImGui::TextColored(
            ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "Frames: %d  SR: %d  Ch: %d",
            framesReceived,
            ndiSampleRate.load(),
            ndiNumChannels.load());
    }

    drawPerformanceMetrics(itemWidth);
    ImGui::PopItemWidth();
}

void NdiReceiverModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Stereo audio outputs
    helpers.drawAudioOutputPin("Audio L", 0);
    helpers.drawAudioOutputPin("Audio R", 1);
}
#endif
