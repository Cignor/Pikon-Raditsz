#include "InternetRadioReceiverModule.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

// FFmpeg includes
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

juce::AudioProcessorValueTreeState::ParameterLayout InternetRadioReceiverModule::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdGain, 1},
            "Gain",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f),
            1.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{paramIdBufferSizeMs, 1}, "Buffer Size", 100, 5000, 1000));

    params.push_back(
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{paramIdSyncToTransport, 1}, "Sync to Transport", true));

    return {params.begin(), params.end()};
}

InternetRadioReceiverModule::InternetRadioReceiverModule()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Out", juce::AudioChannelSet::canonicalChannelSet(3), true)),
      juce::Thread("Internet Radio Thread"),
      apvts(*this, nullptr, "InternetRadioParams", createParameterLayout())
{
    gainParam = apvts.getRawParameterValue(paramIdGain);
    bufferSizeMsParam = apvts.getRawParameterValue(paramIdBufferSizeMs);
    syncToTransportParam = apvts.getRawParameterValue(paramIdSyncToTransport);

    // Initialize FFmpeg network
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    avformat_network_init();

    // Initialize default stations
    stations = {
        {"SomaFM: Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3", "Ambient"},
        {"SomaFM: DEF CON", "http://ice1.somafm.com/defcon-128-mp3", "Techno"},
        {"SomaFM: Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3", "Drone"},
        {"SomaFM: Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3", "Downtempo"},
        {"SomaFM: Lush", "http://ice1.somafm.com/lush-128-mp3", "Vocals"},
        {"SomaFM: Underground 80s", "http://ice1.somafm.com/u80s-128-mp3", "80s"},
        {"SomaFM: The Trip", "http://ice1.somafm.com/thetrip-128-mp3", "Trance"},
        {"HBR1: Dream Factory", "http://radio.hbr1.com:19800/ambient.ogg", "Ambient"},
        {"BassDrive", "http://shoutcast.bassdrive.com:8000/stream", "DnB"},
        {"NTS Radio 1", "http://stream-relay-geo.ntslive.net/stream", "Eclectic"},
        {"NTS Radio 2", "http://stream-relay-geo.ntslive.net/stream2", "Eclectic"},
        {"BBC Radio 1", "http://stream.live.vc.bbcmedia.co.uk/bbc_radio_one", "Top 40"},
        {"BBC Radio 6 Music", "http://stream.live.vc.bbcmedia.co.uk/bbc_6music", "Alternative"},
        {"Resonance FM", "http://stream.resonance.fm:8000/resonance", "Experimental"},
        {"WFMU", "http://stream0.wfmu.org/freeform-128k", "Freeform"},
        {"Intergalactic FM", "http://radio.intergalactic.fm:80/1", "Disco/Italo"},
        {"Dublab", "http://dublab.out.airtime.pro:8000/dublab_128", "Eclectic"}};

    connectionStatus = "Ready";
}

InternetRadioReceiverModule::~InternetRadioReceiverModule()
{
    signalThreadShouldExit();
    stopThread(5000);
    activeStreams.clear();
    avformat_network_deinit();
}

void InternetRadioReceiverModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    // Always start the network thread, even if no URL is pending yet.
    // This connects the 'PendingStationSwitch' logic (trigger/next buttons) to the stream loader.
    // If we don't start it here, user inputs to change stations will be ignored until a manual URL
    // connection happens.
    if (!isThreadRunning())
    {
        startThread(juce::Thread::Priority::normal);
    }
}

void InternetRadioReceiverModule::releaseResources() { signalThreadShouldExit(); }

// ============================================================================
// InternetRadioReceiverModule::RadioStream Implementation
// ============================================================================

InternetRadioReceiverModule::RadioStream::RadioStream(const juce::String& url_, double hostRate)
    : juce::Thread("RadioStream_" + url_), url(url_), hostSampleRate(hostRate)
{
}

InternetRadioReceiverModule::RadioStream::~RadioStream() { stop(); }

void InternetRadioReceiverModule::RadioStream::start()
{
    startThread(juce::Thread::Priority::normal);
}

void InternetRadioReceiverModule::RadioStream::stop()
{
    stopRequested.store(true);
    signalThreadShouldExit();
    stopThread(4000);
    closeConnection();
}

void InternetRadioReceiverModule::RadioStream::run()
{
    while (!threadShouldExit())
    {
        if (!connected)
        {
            if (openConnection())
            {
                decodeLoop(); // Blocks until error or stop
                closeConnection();
            }
            else
            {
                // Retry delay
                wait(2000);
            }
        }
        else
        {
            wait(100);
        }
    }
}

bool InternetRadioReceiverModule::RadioStream::openConnection()
{
    juce::ScopedLock lock(ffmpegLock);

    // Initialize network (if not globally done, but we assume module did it)

    AVDictionary* options = nullptr;
    av_dict_set(&options, "icy", "1", 0);
    av_dict_set(&options, "timeout", "8000000", 0); // 8s timeout

    // Open input
    formatCtx = avformat_alloc_context();
    if (avformat_open_input(&formatCtx, url.toRawUTF8(), nullptr, &options) < 0)
    {
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);

    if (avformat_find_stream_info(formatCtx, nullptr) < 0)
        return false;

    // Find audio stream
    audioStreamIndex = -1;
    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i)
    {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioStreamIndex = i;
            break;
        }
    }
    if (audioStreamIndex < 0)
        return false;

    // Codec
    AVCodecParameters* codecPar = formatCtx->streams[audioStreamIndex]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec)
        return false;

    codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecCtx, codecPar) < 0)
        return false;
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
        return false;

    // Store Bitrate
    bitrate.store(codecPar->bit_rate / 1000);

    // Resampler
    swrCtx = swr_alloc();
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout;
    av_channel_layout_copy(&inLayout, &codecCtx->ch_layout);

    swr_alloc_set_opts2(
        &swrCtx,
        &outLayout,
        AV_SAMPLE_FMT_FLTP,
        (int)hostSampleRate,
        &inLayout,
        codecCtx->sample_fmt,
        codecCtx->sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&inLayout);

    if (swr_init(swrCtx) < 0)
        return false;

    packet = av_packet_alloc();
    frame = av_frame_alloc();

    connected.store(true);
    buffering.store(true);
    return true;
}

void InternetRadioReceiverModule::RadioStream::closeConnection()
{
    juce::ScopedLock lock(ffmpegLock);
    connected.store(false);

    if (frame)
    {
        av_frame_free(&frame);
        frame = nullptr;
    }
    if (packet)
    {
        av_packet_free(&packet);
        packet = nullptr;
    }
    if (swrCtx)
    {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }
    if (codecCtx)
    {
        avcodec_free_context(&codecCtx);
        codecCtx = nullptr;
    }
    if (formatCtx)
    {
        avformat_close_input(&formatCtx);
        formatCtx = nullptr;
    }
}

void InternetRadioReceiverModule::RadioStream::decodeLoop()
{
    while (!threadShouldExit() && !stopRequested.load())
    {
        // 1. Update Metadata
        updateMetadata();

        // 2. Read Frame
        {
            juce::ScopedLock lock(ffmpegLock);
            if (!formatCtx)
                break;

            int ret = av_read_frame(formatCtx, packet);
            if (ret < 0)
                break; // Error or EOF

            if (packet->stream_index == audioStreamIndex)
            {
                if (avcodec_send_packet(codecCtx, packet) >= 0)
                {
                    while (avcodec_receive_frame(codecCtx, frame) >= 0)
                    {
                        // Resample and FIFO push
                        int outSamplesEstim = swr_get_out_samples(swrCtx, frame->nb_samples);
                        if (outSamplesEstim > 0)
                        {
                            juce::AudioBuffer<float> temp(2, outSamplesEstim);
                            float* outData[2] = {temp.getWritePointer(0), temp.getWritePointer(1)};

                            int actualSamples = swr_convert(
                                swrCtx,
                                (uint8_t**)outData,
                                outSamplesEstim,
                                (const uint8_t**)frame->extended_data,
                                frame->nb_samples);

                            if (actualSamples > 0)
                            {
                                // Write to FIFO
                                int start1, size1, start2, size2;
                                fifo.prepareToWrite(actualSamples, start1, size1, start2, size2);
                                if (size1 > 0)
                                {
                                    ringBuffer.copyFrom(0, start1, temp, 0, 0, size1);
                                    ringBuffer.copyFrom(1, start1, temp, 1, 0, size1);
                                }
                                if (size2 > 0)
                                {
                                    ringBuffer.copyFrom(0, start2, temp, 0, size1, size2);
                                    ringBuffer.copyFrom(1, start2, temp, 1, size1, size2);
                                }
                                fifo.finishedWrite(size1 + size2);
                            }
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_unref(packet);
        }

        // 3. Manage Buffering State
        if (fifo.getNumReady() > bufferSize / 4)
            buffering.store(false);

        // Smart Throttle
        // If buffer is getting full, sleep a bit to avoid CPU spin
        if (fifo.getFreeSpace() < 4096)
            wait(5);
        else
            wait(1); // Small sleep to yield timeslice
    }
}

void InternetRadioReceiverModule::RadioStream::updateMetadata()
{
    juce::ScopedLock lock(ffmpegLock);
    if (!formatCtx)
        return;

    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(formatCtx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    {
        juce::String key = juce::String::fromUTF8(tag->key);
        juce::String value = juce::String::fromUTF8(tag->value);

        juce::ScopedLock metaLock(metadataLock);
        if (value != metadata[key])
            metadata[key] = value;
    }
}

juce::String InternetRadioReceiverModule::RadioStream::getMetadata(const juce::String& key) const
{
    juce::ScopedLock lock(metadataLock);
    auto             it = metadata.find(key);
    if (it != metadata.end())
        return it->second;
    return {};
}

juce::String InternetRadioReceiverModule::RadioStream::getFormatInfo() const
{
    return ""; // TODO
}

int InternetRadioReceiverModule::RadioStream::readAudio(
    juce::AudioBuffer<float>& dest,
    int                       numSamples)
{
    int start1, size1, start2, size2;
    fifo.prepareToRead(numSamples, start1, size1, start2, size2);

    int total = size1 + size2;
    if (total > 0)
    {
        if (size1 > 0)
        {
            dest.copyFrom(0, 0, ringBuffer, 0, start1, size1);
            dest.copyFrom(1, 0, ringBuffer, 1, start1, size1);
        }
        if (size2 > 0)
        {
            dest.copyFrom(0, size1, ringBuffer, 0, start2, size2);
            dest.copyFrom(1, size1, ringBuffer, 1, start2, size2);
        }
        fifo.finishedRead(total);
    }
    return total;
}

// ============================================================================
// InternetRadioReceiverModule Manager Implementation
// ============================================================================

void InternetRadioReceiverModule::run()
{
    while (!threadShouldExit())
    {
        // Handle Station Switch Request
        int pending = pendingStationSwitch.exchange(-1);
        if (pending >= 0)
        {
            currentStationIndex.store(pending);
        }

        updatePreCaches();
        wait(100);
    }
}

void InternetRadioReceiverModule::updatePreCaches()
{
    if (stations.empty())
        return;

    int           current = currentStationIndex.load();
    std::set<int> neededIndices = {current}; // Always keep current

    // Add neighbors if no manual prefetch (Legacy behavior)
    // Or maybe just ALWAYS add neighbors+prefetched? Let's do both.
    int prev = (current - 1 + stations.size()) % stations.size();
    int next = (current + 1) % stations.size();
    neededIndices.insert(prev);
    neededIndices.insert(next);

    // Add User Selected Prefetches
    {
        juce::ScopedLock lock(prefetchLock);
        neededIndices.insert(prefetchedIndices.begin(), prefetchedIndices.end());
    }

    // Limit max active streams to avoid thread explosion (max 6?)
    // This is a naive limit, but good for safety.
    if (neededIndices.size() > 6)
    {
        // Keep current, neighbors, and first 3 user prefs
        // TODO: Smarter LRU? For now, just hard limit or let it slide.
        // Let's rely on user not checking 20 boxes.
    }

    // Trash bin for deferred destruction to avoid blocking the lock
    std::vector<std::unique_ptr<RadioStream>> trash;

    {
        juce::ScopedLock lock(activeStreamsLock);

        // Remove unneeded streams
        for (auto it = activeStreams.begin(); it != activeStreams.end();)
        {
            if (neededIndices.find(it->first) == neededIndices.end())
            {
                trash.push_back(std::move(it->second));
                it = activeStreams.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Add missing streams
        for (int idx : neededIndices)
        {
            if (activeStreams.find(idx) == activeStreams.end())
            {
                if (idx >= 0 && idx < stations.size())
                {
                    auto url = stations[idx].url;
                    if (url.isNotEmpty())
                    {
                        auto stream = std::make_unique<RadioStream>(url, currentSampleRate);
                        stream->start();
                        activeStreams[idx] = std::move(stream);
                    }
                }
            }
        }

        // ... (Status update logic remains)
        juce::ScopedLock statusLockGuard(statusLock);
        if (activeStreams.find(current) != activeStreams.end())
        {
            if (activeStreams[current]->isStreamConnected())
                connectionStatus = "Connected: " + stations[current].name;
            else
                connectionStatus = "Connecting[" +
                                   juce::String(activeStreams[current]->getBufferReady() / 48000) +
                                   "s]: " + stations[current].name;
        }
    }
}

// Deprecated / Moved methods
bool InternetRadioReceiverModule::connectToStream(const juce::String& url)
{
    if (url.isNotEmpty())
    {
        // Create single-item manual list
        stations = {{"Manual Stream", url, "Manual"}};
        currentStationIndex = 0;
        return true;
    }
    return false;
}

void InternetRadioReceiverModule::disconnectFromStream() { activeStreams.clear(); }

bool InternetRadioReceiverModule::isConnected() const
{
    juce::ScopedLock lock(statusLock);
    return connectionStatus.startsWith("Connected");
}

juce::String InternetRadioReceiverModule::getCodecName() const { return "MP3/AAC"; }

void InternetRadioReceiverModule::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    const int   numSamples = buffer.getNumSamples();
    const float gain = gainParam->load();
    const bool  syncToTransport = syncToTransportParam->load() > 0.5f;

    // Gate output if transport is stopped and sync is enabled
    if (syncToTransport && !transportState.isPlaying)
    {
        buffer.clear();
    }

    // === PROCESS INPUTS (Trigger & Selection) ===
    auto totalNumInputChannels = getTotalNumInputChannels();

    bool  triggerActive = false;
    float triggerThreshold = 0.5f;

    // Check Trigger (Input 0)
    if (totalNumInputChannels > 0)
    {
        const float* trigIn = buffer.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            float val = trigIn[i];
            if (val > triggerThreshold && lastTriggerVal <= triggerThreshold)
            {
                triggerActive = true;
            }
            lastTriggerVal = val;
        }
    }

    // Check Selection CV (Input 1)
    if (triggerActive && !stations.empty())
    {
        int nextStationIdx = -1;

        if (totalNumInputChannels > 1)
        {
            // Simple sample-and-hold style: check last sample for index
            float selectVal = buffer.getSample(1, numSamples - 1); // 0.0 to 1.0

            if (selectVal > 0.01f)
            {
                int idx = (int)(selectVal * (stations.size() - 0.1f));
                nextStationIdx = juce::jlimit(0, (int)stations.size() - 1, idx);
            }
        }

        if (nextStationIdx == -1)
        {
            // Trigger only -> Next station
            nextStationIdx = (currentStationIndex + 1) % stations.size();
        }

        pendingStationSwitch.store(nextStationIdx);
    }

    // === PROCESS AUDIO OUTPUTS ===
    auto      audioOut = getBusBuffer(buffer, false, 0); // Audio Bus
    const int numOutChannels = audioOut.getNumChannels();
    const int numAudioChannels = juce::jmin(numOutChannels, 2);

    // Read from Active Stream
    int          samplesRead = 0;
    RadioStream* currentStream = nullptr;

    int idx = currentStationIndex.load();
    {
        const juce::ScopedTryLock lock(activeStreamsLock);
        if (lock.isLocked())
        {
            auto it = activeStreams.find(idx);
            if (it != activeStreams.end())
            {
                currentStream = it->second.get();
            }

            if (currentStream && currentStream->isStreamConnected())
            {
                samplesRead = currentStream->readAudio(audioOut, numSamples);
            }
        }
        else
        {
            // Failed to lock - silence (avoids blocking audio thread)
        }
    }

    if (samplesRead < numSamples)
    {
        // Clear remaining
        for (int ch = 0; ch < numAudioChannels; ++ch)
            audioOut.clear(ch, samplesRead, numSamples - samplesRead);
    }

    // Apply gain
    if (gain != 1.0f)
        audioOut.applyGain(gain);

    // Mute if sync required and stopped
    if (syncToTransport && !transportState.isPlaying)
    {
        audioOut.clear();
    }

    // === ENVELOPE FOLLOWER (Output 2) ===
    float rms = 0.0f;
    if (numOutChannels > 0)
        rms = audioOut.getRMSLevel(0, 0, numSamples);
    if (numOutChannels > 1)
        rms = (rms + audioOut.getRMSLevel(1, 0, numSamples)) * 0.5f;

    // Smooth envelope
    envelopeFollower = (rms > envelopeFollower) ? rms : (envelopeFollower * envelopeRelease);
    outputEnvelope.store(envelopeFollower);

    // If we have a 3rd channel (Envelope Pin), write to it
    auto totalOuts = getTotalNumOutputChannels();
    if (totalOuts > 2)
    {
        float* envOut = buffer.getWritePointer(2); // Output 2
        for (int i = 0; i < numSamples; ++i)
            envOut[i] = envelopeFollower;
    }

    // Capture visualization
#if defined(PRESET_CREATOR_UI)
    if (samplesRead > 0)
    {
        const int points = VizData::waveformPoints;
        const int stride = juce::jmax(1, numSamples / points);
        float     peakL = 0.0f, peakR = 0.0f;

        for (int i = 0; i < points; ++i)
        {
            int   sampleIdx = juce::jmin((i * stride) + (stride / 2), numSamples - 1);
            float sampleL = (numAudioChannels > 0) ? audioOut.getSample(0, sampleIdx) : 0.0f;
            float sampleR = (numAudioChannels > 1) ? audioOut.getSample(1, sampleIdx) : sampleL;

            // Clamp for Viz (Fixes UI Exploding)
            sampleL = juce::jlimit(-1.0f, 1.0f, sampleL);
            sampleR = juce::jlimit(-1.0f, 1.0f, sampleR);

            vizData.waveformL[i].store(sampleL);
            vizData.waveformR[i].store(sampleR);
            peakL = juce::jmax(peakL, std::abs(sampleL));
            peakR = juce::jmax(peakR, std::abs(sampleR));
        }
        vizData.peakL.store(peakL);
        vizData.peakR.store(peakR);
    }
    else
    {
        vizData.peakL.store(0);
        vizData.peakR.store(0);
    }
#endif
}

void InternetRadioReceiverModule::parseUserList(const juce::String& text)
{
    stations.clear();

    // Default stations if empty
    if (text.isEmpty())
    {
        stations = {
            {"SomaFM: Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3", "Ambient"},
            {"SomaFM: DEF CON Radio", "http://ice1.somafm.com/defcon-128-mp3", "Hacking"},
            {"SomaFM: Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3", "Drone"},
            // ... (add a few more basic ones to ensure fallback)
            {"BBC Radio 1", "http://stream.live.vc.bbcmedia.co.uk/bbc_radio_one", "Top 40"},
        };
        return;
    }

    juce::StringArray lines;
    lines.addTokens(text, "\n", ""); // Split by newline

    for (auto& line : lines)
    {
        // Format: Name | URL
        // Or just URL
        juce::String name, url;
        int          pipePos = line.indexOf("|");
        if (pipePos > 0)
        {
            name = line.substring(0, pipePos).trim();
            url = line.substring(pipePos + 1).trim();
        }
        else
        {
            url = line.trim();
            name = url; // fallback
        }

        if (url.isNotEmpty())
        {
            stations.push_back({name, url, "User"});
        }
    }

    if (stations.empty())
    {
        // Fallback if parsing failed completely
        stations.push_back({"User Radio (Empty)", "", "None"});
    }
}

juce::String InternetRadioReceiverModule::getConnectionStatus() const
{
    juce::ScopedLock lock(statusLock);
    return connectionStatus;
}

juce::String InternetRadioReceiverModule::getStationName() const
{
    int idx = currentStationIndex.load();
    if (idx >= 0 && idx < stations.size())
        return stations[idx].name;
    return "Unknown";
}

juce::String InternetRadioReceiverModule::getStreamUrl() const
{
    int idx = currentStationIndex.load();
    if (idx >= 0 && idx < stations.size())
        return stations[idx].url;
    return "";
}

juce::String InternetRadioReceiverModule::getCurrentSong() const
{
    // Need to get song title from Current Stream
    // activeStreams is NOT thread safe for reading without lock, but we returned raw pointer in
    // updatePreCaches? No, map. We can't lock the map in audio thread or UI thread easily if
    // manager is writing. But UI thread is fine to wait a bit.

    // Actually, let's just return "See UI" or implement proper locking for metadata.
    // For now, return empty or safe string.
    return "";
}

juce::ValueTree InternetRadioReceiverModule::getExtraStateTree() const
{
    juce::ValueTree tree("InternetRadioState");
    tree.setProperty("useUserList", useUserList, nullptr);
    tree.setProperty("userListText", userListText, nullptr);
    tree.setProperty("currentStationIndex", currentStationIndex.load(), nullptr);
    tree.setProperty("syncToTransport", syncToTransportParam->load() > 0.5f, nullptr);

    // Save Manual URL if present
    tree.setProperty("manualUrl", juce::String(urlInputBuffer), nullptr);
    return tree;
}

void InternetRadioReceiverModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("InternetRadioState"))
    {
        useUserList = tree.getProperty("useUserList", false);
        userListText = tree.getProperty("userListText", "");

        if (useUserList && userListText.isNotEmpty())
        {
            parseUserList(userListText);
        }

        int idx = tree.getProperty("currentStationIndex", 0);
        currentStationIndex.store(idx);

        if (tree.hasProperty("syncToTransport"))
            *syncToTransportParam = tree.getProperty("syncToTransport") ? 1.0f : 0.0f;

        juce::String manualUrl = tree.getProperty("manualUrl", "");
        if (manualUrl.isNotEmpty())
        {
            strncpy(urlInputBuffer, manualUrl.toRawUTF8(), sizeof(urlInputBuffer) - 1);
        }
    }
}

#if defined(PRESET_CREATOR_UI)
void InternetRadioReceiverModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // Create Tabs
    if (ImGui::BeginTabBar("RadioTabs"))
    {
        // === TAB 1: PLAYER ===
        if (ImGui::BeginTabItem("Player"))
        {
            // Current Station Display
            juce::String stationInfo = getStationName();
            if (stationInfo.isEmpty())
                stationInfo = "No metadata";

            ImGui::TextWrapped("NOW PLAYING:");
            // We don't have direct access to Fonts[1] safely without check, use default font with
            // color/scale
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 1, 0, 1));
            ImGui::TextWrapped("%s", stationInfo.toRawUTF8());
            ImGui::PopStyleColor();

            // Transport Status
            ImGui::Text("Status: %s", getConnectionStatus().toRawUTF8());

            // Buffer health
            int bufReady = 0;
            if (!stations.empty())
            {
                int idx = currentStationIndex.load();
                {
                    juce::ScopedTryLock lock(activeStreamsLock);
                    if (lock.isLocked() && activeStreams.find(idx) != activeStreams.end())
                        bufReady = activeStreams[idx]->getBufferReady();
                }
            }
            ImGui::ProgressBar((float)bufReady / 192000.0f, ImVec2(itemWidth, 0), "Buffer");

            ImGui::Separator();

            // Waveform
            {
                ImVec2      waveformSize(itemWidth, 60.0f);
                ImVec2      pos = ImGui::GetCursorScreenPos();
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                drawList->AddRectFilled(
                    pos,
                    ImVec2(pos.x + waveformSize.x, pos.y + waveformSize.y),
                    IM_COL32(30, 30, 30, 255));

                float centerY = pos.y + waveformSize.y / 2.0f;
                float halfHeight = waveformSize.y / 2.0f - 2.0f;

                for (int i = 0; i < VizData::waveformPoints - 1; ++i)
                {
                    float x1 = pos.x + (float)i / VizData::waveformPoints * waveformSize.x;
                    float x2 = pos.x + (float)(i + 1) / VizData::waveformPoints * waveformSize.x;
                    float y1 = centerY - vizData.waveformL[i].load() * halfHeight;
                    float y2 = centerY - vizData.waveformL[i + 1].load() * halfHeight;

                    drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(100, 200, 100, 255));
                }
                ImGui::Dummy(waveformSize);
            }

            // Controls
            float btnWidth = (itemWidth - 10.0f) / 2.0f;
            if (ImGui::Button("< Prev", ImVec2(btnWidth, 0)))
            {
                int c = currentStationIndex.load();
                pendingStationSwitch.store((c - 1 + stations.size()) % stations.size());
                onModificationEnded();
            }
            ImGui::SameLine();
            if (ImGui::Button("Next >", ImVec2(btnWidth, 0)))
            {
                int c = currentStationIndex.load();
                pendingStationSwitch.store((c + 1) % stations.size());
                onModificationEnded();
            }

            float gain = gainParam->load();
            if (ImGui::SliderFloat("Gain", &gain, 0.0f, 2.0f, "%.2f"))
            {
                if (auto* p = apvts.getParameter("gain"))
                    p->setValueNotifyingHost(gain);
                onModificationEnded();
            }

            bool sync = syncToTransportParam->load() > 0.5f;
            if (ImGui::Checkbox("Sync Transp.", &sync))
            {
                *syncToTransportParam = sync ? 1.0f : 0.0f;
                onModificationEnded();
            }

            ImGui::EndTabItem();
        }

        // === TAB 2: STATIONS ===
        if (ImGui::BeginTabItem("Stations"))
        {
            ImGui::Text("Check box to pre-cache (Instant Switch)");
            ImGui::BeginChild("StationList", ImVec2(itemWidth, 300), true);

            juce::ScopedLock pflock(prefetchLock);
            int              current = currentStationIndex.load();

            for (int i = 0; i < stations.size(); ++i)
            {
                bool isPrefetched = prefetchedIndices.count(i) > 0;
                bool isCurrent = (i == current);

                ImGui::PushID(i);

                // Pre-fetch checkbox
                if (ImGui::Checkbox("##pref", &isPrefetched))
                {
                    if (isPrefetched)
                        prefetchedIndices.insert(i);
                    else
                        prefetchedIndices.erase(i);
                }
                ImGui::SameLine();

                // Play Button / Selection
                juce::String label = juce::String(i + 1) + ". " + stations[i].name;
                if (isCurrent)
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "> %s", label.toRawUTF8());
                }
                else
                {
                    if (ImGui::Selectable(label.toRawUTF8(), false))
                    {
                        pendingStationSwitch.store(i);
                        onModificationEnded();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // === TAB 3: EDIT ===
        if (ImGui::BeginTabItem("Edit"))
        {
            ImGui::TextWrapped("Enter one station per line in the format:");
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Name | URL");
            ImGui::TextDisabled(
                "Example details:\nSomaFM | http://ice1.somafm.com/groovesalad-128-mp3");
            ImGui::Separator();
            if (ImGui::Checkbox("Use Custom List", &useUserList))
            {
                if (useUserList)
                    parseUserList(userListText);
                else
                    parseUserList("");
                onModificationEnded();
            }

            char buf[8192];
            strncpy(buf, userListText.toRawUTF8(), sizeof(buf) - 1);
            if (ImGui::InputTextMultiline("##editor", buf, sizeof(buf), ImVec2(itemWidth, 200)))
            {
                userListText = buf;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                if (useUserList)
                    parseUserList(userListText);
                onModificationEnded();
            }

            if (ImGui::Button("Manual URL Go"))
            {
                // Trigger manual override popup/logic if needed, or just leave it to "Edit"
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::PopItemWidth();
}

void InternetRadioReceiverModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Inputs (Trigger, Select)
    helpers.drawAudioInputPin("Trig", 0);
    helpers.drawAudioInputPin("Sel", 1);

    // Outputs (L, R, Envelope)
    helpers.drawAudioOutputPin("L", 0);
    helpers.drawAudioOutputPin("R", 1);
    helpers.drawAudioOutputPin("Env", 2);
}
#endif
