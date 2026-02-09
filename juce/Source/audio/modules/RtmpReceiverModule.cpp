#include "RtmpReceiverModule.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h>
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

juce::AudioProcessorValueTreeState::ParameterLayout RtmpReceiverModule::createParameterLayout()
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
            juce::ParameterID{paramIdBufferSizeMs, 1}, "Buffer Size", 100, 5000, 500));

    return {params.begin(), params.end()};
}

RtmpReceiverModule::RtmpReceiverModule()
    : ModuleProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("RTMP Receiver Thread"),
      apvts(*this, nullptr, "RtmpReceiverParams", createParameterLayout())
{
    gainParam = apvts.getRawParameterValue(paramIdGain);
    bufferSizeMsParam = apvts.getRawParameterValue(paramIdBufferSizeMs);

    // Initialize FFmpeg network
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    avformat_network_init();

    connectionStatus = "Ready";

    // Set default example URL
    strncpy(urlInputBuffer, "rtmp://localhost:1935/live/stream", sizeof(urlInputBuffer) - 1);
}

RtmpReceiverModule::~RtmpReceiverModule()
{
    signalThreadShouldExit();
    stopThread(5000);
    closeStream();
    avformat_network_deinit();
}

void RtmpReceiverModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    if (currentUrl.isNotEmpty() || pendingUrl.isNotEmpty())
    {
        startThread(juce::Thread::Priority::normal);
    }
}

void RtmpReceiverModule::releaseResources() { signalThreadShouldExit(); }

bool RtmpReceiverModule::connectToStream(const juce::String& url)
{
    if (url.isEmpty())
        return false;

    // Validate RTMP URL
    if (!url.startsWithIgnoreCase("rtmp://") && !url.startsWithIgnoreCase("rtmps://") &&
        !url.startsWithIgnoreCase("rtmpt://") && !url.startsWithIgnoreCase("rtmpte://"))
    {
        connectionStatus = "Invalid RTMP URL";
        return false;
    }

    {
        const juce::ScopedLock lock(urlLock);
        pendingUrl = url;
    }

    connectionRequested = true;

    if (!isThreadRunning())
        startThread(juce::Thread::Priority::normal);

    return true;
}

void RtmpReceiverModule::disconnectFromStream() { disconnectRequested = true; }

bool RtmpReceiverModule::openStream(const juce::String& url)
{
    closeStream();

    connectionStatus = "Connecting...";
    juce::Logger::writeToLog("[RtmpReceiver] Connecting to: " + url);

    formatCtx = avformat_alloc_context();
    if (!formatCtx)
    {
        connectionStatus = "Memory allocation failed";
        return false;
    }

    // RTMP-specific options
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtmp_live", "live", 0);
    av_dict_set(&options, "timeout", "10000000", 0);        // 10s timeout
    av_dict_set(&options, "analyzeduration", "2000000", 0); // 2s analyze
    av_dict_set(&options, "probesize", "5000000", 0);       // 5MB probe

    int ret = avformat_open_input(&formatCtx, url.toRawUTF8(), nullptr, &options);
    av_dict_free(&options);

    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        connectionStatus = "Failed: " + juce::String(errbuf);
        juce::Logger::writeToLog("[RtmpReceiver] Open failed: " + juce::String(errbuf));
        closeStream();
        return false;
    }

    connectionStatus = "Finding stream info...";
    ret = avformat_find_stream_info(formatCtx, nullptr);
    if (ret < 0)
    {
        connectionStatus = "Could not find stream info";
        closeStream();
        return false;
    }

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
    {
        connectionStatus = "No audio stream found";
        closeStream();
        return false;
    }

    AVCodecParameters* codecPar = formatCtx->streams[audioStreamIndex]->codecpar;

    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec)
    {
        connectionStatus = "Unsupported audio codec";
        closeStream();
        return false;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        connectionStatus = "Could not allocate codec context";
        closeStream();
        return false;
    }

    ret = avcodec_parameters_to_context(codecCtx, codecPar);
    if (ret < 0)
    {
        connectionStatus = "Could not copy codec parameters";
        closeStream();
        return false;
    }

    ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0)
    {
        connectionStatus = "Could not open codec";
        closeStream();
        return false;
    }

    codecName = juce::String(codec->name);
    sourceSampleRate = codecCtx->sample_rate;
    sourceNumChannels = codecCtx->ch_layout.nb_channels;
    streamBitrate = codecPar->bit_rate / 1000;

    // Initialize resampler
    swrCtx = swr_alloc();
    if (!swrCtx)
    {
        connectionStatus = "Could not allocate resampler";
        closeStream();
        return false;
    }

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout;
    av_channel_layout_copy(&inLayout, &codecCtx->ch_layout);

    swr_alloc_set_opts2(
        &swrCtx,
        &outLayout,
        AV_SAMPLE_FMT_FLT,
        (int)currentSampleRate,
        &inLayout,
        codecCtx->sample_fmt,
        codecCtx->sample_rate,
        0,
        nullptr);

    av_channel_layout_uninit(&inLayout);

    ret = swr_init(swrCtx);
    if (ret < 0)
    {
        connectionStatus = "Could not initialize resampler";
        closeStream();
        return false;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame)
    {
        connectionStatus = "Could not allocate packet/frame";
        closeStream();
        return false;
    }

    currentUrl = url;
    streamConnected = true;
    reconnectAttempts = 0;
    connectionStatus = "Connected";

    juce::Logger::writeToLog(
        "[RtmpReceiver] Connected: " + codecName + " @ " + juce::String(sourceSampleRate.load()) +
        "Hz");

    return true;
}

void RtmpReceiverModule::closeStream()
{
    streamConnected = false;

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

    audioStreamIndex = -1;
}

void RtmpReceiverModule::decodeAndBuffer()
{
    if (!formatCtx || !codecCtx || !packet || !frame || !swrCtx)
        return;

    int ret = av_read_frame(formatCtx, packet);
    if (ret < 0)
    {
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
            return;

        connectionStatus = "Stream error";
        streamConnected = false;
        return;
    }

    if (packet->stream_index != audioStreamIndex)
    {
        av_packet_unref(packet);
        return;
    }

    ret = avcodec_send_packet(codecCtx, packet);
    av_packet_unref(packet);

    if (ret < 0)
        return;

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return;

        int outSamples = swr_get_out_samples(swrCtx, frame->nb_samples);
        if (outSamples <= 0)
        {
            av_frame_unref(frame);
            continue;
        }

        float* outBuffer[2];
        outBuffer[0] = new float[outSamples];
        outBuffer[1] = new float[outSamples];

        int convertedSamples = swr_convert(
            swrCtx,
            (uint8_t**)outBuffer,
            outSamples,
            (const uint8_t**)frame->extended_data,
            frame->nb_samples);

        if (convertedSamples > 0)
        {
            int start1, size1, start2, size2;
            audioFifo.prepareToWrite(convertedSamples, start1, size1, start2, size2);

            int totalWritten = size1 + size2;
            if (totalWritten > 0)
            {
                if (size1 > 0)
                {
                    audioRingBuffer.copyFrom(0, start1, outBuffer[0], size1);
                    audioRingBuffer.copyFrom(1, start1, outBuffer[1], size1);
                }
                if (size2 > 0)
                {
                    audioRingBuffer.copyFrom(0, start2, outBuffer[0] + size1, size2);
                    audioRingBuffer.copyFrom(1, start2, outBuffer[1] + size1, size2);
                }
                audioFifo.finishedWrite(totalWritten);
            }
        }

        delete[] outBuffer[0];
        delete[] outBuffer[1];
        av_frame_unref(frame);
    }
}

void RtmpReceiverModule::run()
{
    while (!threadShouldExit())
    {
        if (disconnectRequested.exchange(false))
        {
            closeStream();
            currentUrl.clear();
            connectionStatus = "Disconnected";
        }

        if (connectionRequested.exchange(false))
        {
            juce::String urlToConnect;
            {
                const juce::ScopedLock lock(urlLock);
                urlToConnect = pendingUrl;
                pendingUrl.clear();
            }

            if (urlToConnect.isNotEmpty())
            {
                openStream(urlToConnect);
            }
        }

        if (streamConnected)
        {
            decodeAndBuffer();
        }
        else if (autoReconnect && currentUrl.isNotEmpty() && !disconnectRequested)
        {
            if (reconnectAttempts < maxReconnectAttempts)
            {
                connectionStatus = "Reconnecting... (" + juce::String(reconnectAttempts + 1) + "/" +
                                   juce::String(maxReconnectAttempts) + ")";
                wait(3000);

                if (!threadShouldExit() && !disconnectRequested)
                {
                    reconnectAttempts++;
                    openStream(currentUrl);
                }
            }
            else
            {
                connectionStatus = "Reconnection failed";
                wait(1000);
            }
        }
        else
        {
            wait(100);
        }
    }

    closeStream();
}

void RtmpReceiverModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int   numSamples = buffer.getNumSamples();
    const int   numChannels = juce::jmin(2, buffer.getNumChannels());
    const float gain = gainParam->load();

    int start1, size1, start2, size2;
    audioFifo.prepareToRead(numSamples, start1, size1, start2, size2);

    int totalRead = size1 + size2;

    if (totalRead > 0)
    {
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

        if (totalRead < numSamples)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.clear(ch, totalRead, numSamples - totalRead);
        }

        if (gain != 1.0f)
            buffer.applyGain(gain);
    }
    else
    {
        buffer.clear();
    }

#if defined(PRESET_CREATOR_UI)
    {
        const int points = VizData::waveformPoints;
        const int stride = juce::jmax(1, numSamples / points);
        float     peakL = 0.0f, peakR = 0.0f;

        for (int i = 0; i < points; ++i)
        {
            int   sampleIdx = juce::jmin((i * stride) + (stride / 2), numSamples - 1);
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

juce::ValueTree RtmpReceiverModule::getExtraStateTree() const
{
    juce::ValueTree tree("RtmpReceiverState");
    tree.setProperty("url", currentUrl, nullptr);
    tree.setProperty("autoReconnect", autoReconnect.load(), nullptr);
    return tree;
}

void RtmpReceiverModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("RtmpReceiverState"))
    {
        juce::String savedUrl = tree.getProperty("url", "").toString();
        autoReconnect = tree.getProperty("autoReconnect", true);

        if (savedUrl.isNotEmpty())
        {
            strncpy(urlInputBuffer, savedUrl.toRawUTF8(), sizeof(urlInputBuffer) - 1);
            connectToStream(savedUrl);
        }
    }
}

#if defined(PRESET_CREATOR_UI)
void RtmpReceiverModule::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float  cursorScreenX = ImGui::GetCursorScreenPos().x;
    const float  nodeRightEdge = cursorScreenX + itemWidth;
    const float  savedWorkRectMaxX = window->WorkRect.Max.x;
    const float  savedContentRegionMaxX = window->ContentRegionRect.Max.x;
    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    ImGui::PushItemWidth(itemWidth);

    // URL input
    ImGui::Text("RTMP URL:");
    if (ImGui::InputText(
            "##url", urlInputBuffer, sizeof(urlInputBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        connectToStream(juce::String(urlInputBuffer));
        onModificationEnded();
    }

    // Help text
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "e.g., rtmp://localhost:1935/live/stream");

    // Connect/Disconnect
    if (streamConnected)
    {
        if (ImGui::Button("Disconnect", ImVec2(itemWidth, 0)))
        {
            disconnectFromStream();
            onModificationEnded();
        }
    }
    else
    {
        if (ImGui::Button("Connect", ImVec2(itemWidth, 0)))
        {
            connectToStream(juce::String(urlInputBuffer));
            onModificationEnded();
        }
    }

    // Status
    ImVec4 statusColor =
        streamConnected ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    ImGui::TextColored(statusColor, "%s", connectionStatus.toRawUTF8());

    // Stream info
    if (streamConnected)
    {
        if (codecName.isNotEmpty())
            ImGui::Text("Codec: %s @ %d kbps", codecName.toRawUTF8(), streamBitrate.load());
    }

    // Usage info
    if (ImGui::CollapsingHeader("Usage Tips"))
    {
        ImGui::TextWrapped(
            "1. Start an RTMP server (nginx-rtmp, SRS, etc.)\n"
            "2. Configure OBS to stream to that server\n"
            "3. Enter the RTMP URL here and connect\n\n"
            "Or connect directly to a live stream URL.");
    }

    // Gain slider
    bool gainModulated = isParamModulated(paramIdGain);
    if (gainModulated)
        ImGui::BeginDisabled();

    float gain = gainParam->load();
    if (ImGui::SliderFloat("Gain", &gain, 0.0f, 2.0f, "%.2f"))
    {
        apvts.getParameter(paramIdGain)->setValueNotifyingHost(gain / 2.0f);
        onModificationEnded();
    }

    if (gainModulated)
        ImGui::EndDisabled();

    // Waveform
    ImGui::Text("Audio:");
    ImVec2      waveformSize(itemWidth, 40.0f);
    ImVec2      pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(
        pos, ImVec2(pos.x + waveformSize.x, pos.y + waveformSize.y), IM_COL32(30, 30, 30, 255));

    float centerY = pos.y + waveformSize.y / 2.0f;
    float halfHeight = waveformSize.y / 2.0f - 2.0f;

    for (int i = 0; i < VizData::waveformPoints - 1; ++i)
    {
        float x1 = pos.x + (float)i / VizData::waveformPoints * waveformSize.x;
        float x2 = pos.x + (float)(i + 1) / VizData::waveformPoints * waveformSize.x;
        float y1 = centerY - vizData.waveformL[i].load() * halfHeight;
        float y2 = centerY - vizData.waveformL[i + 1].load() * halfHeight;

        drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 150, 100, 255));
    }

    ImGui::Dummy(waveformSize);

    ImGui::PopItemWidth();

    // Restore original values
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}

void RtmpReceiverModule::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioOutputPin("Out L", 0);
    helpers.drawAudioOutputPin("Out R", 1);
}
#endif
