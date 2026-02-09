#include "SrtReceiverModule.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

// FFmpeg for decoding (SRT typically carries MPEG-TS)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

juce::AudioProcessorValueTreeState::ParameterLayout SrtReceiverModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdGain, 1},
            "Gain",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f),
            1.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{paramIdMode, 1}, "Mode", juce::StringArray{"Listener", "Caller"}, 0));

    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{paramIdPort, 1}, "Port", 1024, 65535, 9000));

    params.push_back(
        std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{paramIdLatency, 1}, "Latency (ms)", 20, 8000, 120));

    return {params.begin(), params.end()};
}

SrtReceiverModule::SrtReceiverModule()
    : ModuleProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("SRT Receiver Thread"),
      apvts(*this, nullptr, "SrtReceiverParams", createParameterLayout())
{
    gainParam = apvts.getRawParameterValue(paramIdGain);
    modeParam = apvts.getRawParameterValue(paramIdMode);
    portParam = apvts.getRawParameterValue(paramIdPort);
    latencyParam = apvts.getRawParameterValue(paramIdLatency);

#ifdef HAS_SRT
    if (initializeSrt())
    {
        connectionStatus = "SRT Ready";
    }
    else
    {
        connectionStatus = "SRT init failed";
    }
#else
    connectionStatus = "SRT not available";
    juce::Logger::writeToLog(
        "[SrtReceiver] SRT library not available. Install via: vcpkg install libsrt:x64-windows");
#endif
}

SrtReceiverModule::~SrtReceiverModule()
{
    signalThreadShouldExit();
    stopThread(5000);

#ifdef HAS_SRT
    cleanupSrt();
#endif
}

bool SrtReceiverModule::isSrtAvailable() const
{
#ifdef HAS_SRT
    return true;
#else
    return false;
#endif
}

#ifdef HAS_SRT
bool SrtReceiverModule::initializeSrt()
{
    int result = srt_startup();
    if (result != 0)
    {
        juce::Logger::writeToLog("[SrtReceiver] srt_startup() failed");
        return false;
    }

    juce::Logger::writeToLog("[SrtReceiver] SRT library initialized");
    return true;
}

void SrtReceiverModule::cleanupSrt()
{
    disconnect();
    srt_cleanup();
}
#endif

void SrtReceiverModule::setPassphrase(const juce::String& pass)
{
    const juce::ScopedLock lock(settingsLock);
    passphrase = pass;
    strncpy(passphraseBuffer, pass.toRawUTF8(), sizeof(passphraseBuffer) - 1);
}

void SrtReceiverModule::setStreamId(const juce::String& id)
{
    const juce::ScopedLock lock(settingsLock);
    streamId = id;
    strncpy(streamIdBuffer, id.toRawUTF8(), sizeof(streamIdBuffer) - 1);
}

bool SrtReceiverModule::startListening(int port)
{
#ifdef HAS_SRT
    disconnect();

    connectPort = port;
    currentMode = Mode::Listener;
    startRequested = true;

    if (!isThreadRunning())
        startThread(juce::Thread::Priority::normal);

    return true;
#else
    connectionStatus = "SRT not available";
    return false;
#endif
}

bool SrtReceiverModule::connectTo(const juce::String& host, int port)
{
#ifdef HAS_SRT
    disconnect();

    hostAddress = host;
    connectPort = port;
    currentMode = Mode::Caller;
    startRequested = true;

    if (!isThreadRunning())
        startThread(juce::Thread::Priority::normal);

    return true;
#else
    connectionStatus = "SRT not available";
    return false;
#endif
}

void SrtReceiverModule::disconnect()
{
    stopRequested = true;

#ifdef HAS_SRT
    if (connectedSocket != SRT_INVALID_SOCK)
    {
        srt_close(connectedSocket);
        connectedSocket = SRT_INVALID_SOCK;
    }
    if (srtSocket != SRT_INVALID_SOCK)
    {
        srt_close(srtSocket);
        srtSocket = SRT_INVALID_SOCK;
    }
#endif

    srtConnected = false;
    srtListening = false;
}

void SrtReceiverModule::updateStats()
{
#ifdef HAS_SRT
    if (connectedSocket == SRT_INVALID_SOCK)
        return;

    SRT_TRACEBSTATS perf;
    if (srt_bstats(connectedSocket, &perf, 1) == 0)
    {
        stats.rtt.store((int64_t)perf.msRTT);
        stats.pktLoss.store(perf.pktRcvLoss);
        stats.bandwidth.store(perf.mbpsBandwidth);
        stats.bytesRecv.store(perf.byteRecv);
    }
#endif
}

void SrtReceiverModule::receiveLoop()
{
#ifdef HAS_SRT
    while (!threadShouldExit() && srtConnected && !stopRequested)
    {
        // Check for incoming data
        SRT_EPOLL_EVENT events[1];
        int             epollId = srt_epoll_create();
        srt_epoll_add_usock(epollId, connectedSocket, nullptr);

        int numEvents = srt_epoll_uwait(epollId, events, 1, 100); // 100ms timeout
        srt_epoll_release(epollId);

        if (numEvents > 0)
        {
            int received = srt_recvmsg(connectedSocket, (char*)recvBuffer, recvBufferSize);

            if (received > 0)
            {
                stats.bytesRecv.fetch_add(received);

                // TODO: Decode MPEG-TS packet and extract audio
                // For now, we'll handle raw PCM or simple packet format
                // Real implementation would use FFmpeg to demux MPEG-TS

                // Simple raw audio handling (assumes 48kHz stereo float)
                int numSamples = received / (2 * sizeof(float));
                if (numSamples > 0)
                {
                    float* audioData = reinterpret_cast<float*>(recvBuffer);

                    int start1, size1, start2, size2;
                    audioFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

                    int totalWritten = size1 + size2;
                    if (totalWritten > 0)
                    {
                        if (size1 > 0)
                        {
                            for (int i = 0; i < size1; ++i)
                            {
                                audioRingBuffer.setSample(0, start1 + i, audioData[i * 2]);
                                audioRingBuffer.setSample(1, start1 + i, audioData[i * 2 + 1]);
                            }
                        }
                        if (size2 > 0)
                        {
                            int offset = size1 * 2;
                            for (int i = 0; i < size2; ++i)
                            {
                                audioRingBuffer.setSample(0, start2 + i, audioData[offset + i * 2]);
                                audioRingBuffer.setSample(
                                    1, start2 + i, audioData[offset + i * 2 + 1]);
                            }
                        }
                        audioFifo.finishedWrite(totalWritten);
                    }
                }
            }
            else if (received == 0 || received == SRT_ERROR)
            {
                // Connection closed or error
                connectionStatus = "Connection lost";
                srtConnected = false;
                break;
            }
        }

        // Update statistics periodically
        static int statsCounter = 0;
        if (++statsCounter > 10)
        {
            updateStats();
            statsCounter = 0;
        }
    }
#endif
}

void SrtReceiverModule::run()
{
#ifdef HAS_SRT
    while (!threadShouldExit())
    {
        // Handle stop request
        if (stopRequested.exchange(false))
        {
            disconnect();
            connectionStatus = "Disconnected";
        }

        // Handle start request
        if (startRequested.exchange(false))
        {
            int latency = (int)latencyParam->load();

            if (currentMode == Mode::Listener)
            {
                // Listener mode
                connectionStatus = "Creating listener...";

                srtSocket = srt_create_socket();
                if (srtSocket == SRT_INVALID_SOCK)
                {
                    connectionStatus = "Failed to create socket";
                    continue;
                }

                // Set options
                srt_setsockopt(srtSocket, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));

                // Set passphrase if provided
                {
                    const juce::ScopedLock lock(settingsLock);
                    if (passphrase.isNotEmpty())
                    {
                        srt_setsockopt(
                            srtSocket,
                            0,
                            SRTO_PASSPHRASE,
                            passphrase.toRawUTF8(),
                            (int)passphrase.length());
                    }
                }

                // Bind
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons((uint16_t)connectPort);
                addr.sin_addr.s_addr = INADDR_ANY;

                if (srt_bind(srtSocket, (sockaddr*)&addr, sizeof(addr)) == SRT_ERROR)
                {
                    connectionStatus = "Bind failed: " + juce::String(srt_getlasterror_str());
                    srt_close(srtSocket);
                    srtSocket = SRT_INVALID_SOCK;
                    continue;
                }

                // Listen
                if (srt_listen(srtSocket, 1) == SRT_ERROR)
                {
                    connectionStatus = "Listen failed";
                    srt_close(srtSocket);
                    srtSocket = SRT_INVALID_SOCK;
                    continue;
                }

                srtListening = true;
                connectionStatus = "Listening on port " + juce::String(connectPort);
                juce::Logger::writeToLog(
                    "[SrtReceiver] Listening on port " + juce::String(connectPort));

                // Wait for connection
                while (!threadShouldExit() && srtListening && !stopRequested)
                {
                    sockaddr_in clientAddr{};
                    int         addrLen = sizeof(clientAddr);

                    // Use polling to allow checking for exit
                    SRT_EPOLL_EVENT events[1];
                    int             epollId = srt_epoll_create();
                    srt_epoll_add_usock(epollId, srtSocket, nullptr);

                    int numEvents = srt_epoll_uwait(epollId, events, 1, 1000); // 1s timeout
                    srt_epoll_release(epollId);

                    if (numEvents > 0)
                    {
                        connectedSocket = srt_accept(srtSocket, (sockaddr*)&clientAddr, &addrLen);
                        if (connectedSocket != SRT_INVALID_SOCK)
                        {
                            srtConnected = true;
                            srtListening = false;
                            connectionStatus = "Client connected";
                            juce::Logger::writeToLog("[SrtReceiver] Client connected");

                            receiveLoop();
                        }
                    }
                }
            }
            else
            {
                // Caller mode
                connectionStatus = "Connecting...";

                srtSocket = srt_create_socket();
                if (srtSocket == SRT_INVALID_SOCK)
                {
                    connectionStatus = "Failed to create socket";
                    continue;
                }

                // Set options
                srt_setsockopt(srtSocket, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));

                // Set passphrase if provided
                {
                    const juce::ScopedLock lock(settingsLock);
                    if (passphrase.isNotEmpty())
                    {
                        srt_setsockopt(
                            srtSocket,
                            0,
                            SRTO_PASSPHRASE,
                            passphrase.toRawUTF8(),
                            (int)passphrase.length());
                    }
                    if (streamId.isNotEmpty())
                    {
                        srt_setsockopt(
                            srtSocket,
                            0,
                            SRTO_STREAMID,
                            streamId.toRawUTF8(),
                            (int)streamId.length());
                    }
                }

                // Connect
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons((uint16_t)connectPort);

                // Parse host address
                if (inet_pton(AF_INET, hostAddress.toRawUTF8(), &addr.sin_addr) != 1)
                {
                    // Try hostname resolution
                    struct hostent* he = gethostbyname(hostAddress.toRawUTF8());
                    if (he)
                        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
                    else
                    {
                        connectionStatus = "Invalid host";
                        srt_close(srtSocket);
                        srtSocket = SRT_INVALID_SOCK;
                        continue;
                    }
                }

                if (srt_connect(srtSocket, (sockaddr*)&addr, sizeof(addr)) == SRT_ERROR)
                {
                    connectionStatus = "Connect failed: " + juce::String(srt_getlasterror_str());
                    srt_close(srtSocket);
                    srtSocket = SRT_INVALID_SOCK;
                    continue;
                }

                connectedSocket = srtSocket;
                srtConnected = true;
                connectionStatus = "Connected to " + hostAddress;
                juce::Logger::writeToLog("[SrtReceiver] Connected to " + hostAddress);

                receiveLoop();
            }
        }

        // Wait if not doing anything
        if (!srtConnected && !srtListening)
        {
            wait(100);
        }
    }

    disconnect();
#else
    // SRT not available - just wait
    while (!threadShouldExit())
    {
        wait(1000);
    }
#endif
}

void SrtReceiverModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
}

void SrtReceiverModule::releaseResources() { signalThreadShouldExit(); }

void SrtReceiverModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int   numSamples = buffer.getNumSamples();
    const int   numChannels = juce::jmin(2, buffer.getNumChannels());
    const float gain = gainParam->load();

    // Read from ring buffer
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
    // Capture waveform
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

juce::ValueTree SrtReceiverModule::getExtraStateTree() const
{
    juce::ValueTree tree("SrtReceiverState");
    tree.setProperty("host", hostAddress, nullptr);
    tree.setProperty("port", connectPort, nullptr);
    tree.setProperty("mode", (int)currentMode, nullptr);
    tree.setProperty("streamId", streamId, nullptr);
    // Note: Don't save passphrase for security
    return tree;
}

void SrtReceiverModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("SrtReceiverState"))
    {
        hostAddress = tree.getProperty("host", "").toString();
        connectPort = tree.getProperty("port", 9000);
        currentMode = (Mode)(int)tree.getProperty("mode", 0);
        streamId = tree.getProperty("streamId", "").toString();

        strncpy(hostInputBuffer, hostAddress.toRawUTF8(), sizeof(hostInputBuffer) - 1);
        strncpy(streamIdBuffer, streamId.toRawUTF8(), sizeof(streamIdBuffer) - 1);
    }
}

#if defined(PRESET_CREATOR_UI)
void SrtReceiverModule::drawParametersInNode(
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

    if (!isSrtAvailable())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "SRT not available");
        ImGui::TextWrapped("Install via: vcpkg install libsrt:x64-windows");
        ImGui::PopItemWidth();
        return;
    }

    // Mode selector
    bool modeModulated = isParamModulated(paramIdMode);
    if (modeModulated)
        ImGui::BeginDisabled();

    int mode = (int)modeParam->load();
    if (ImGui::Combo("Mode", &mode, "Listener\0Caller\0"))
    {
        apvts.getParameter(paramIdMode)->setValueNotifyingHost(mode == 0 ? 0.0f : 1.0f);
        currentMode = (Mode)mode;
        onModificationEnded();
    }

    if (modeModulated)
        ImGui::EndDisabled();

    // Port input
    int port = (int)portParam->load();
    if (ImGui::InputInt("Port", &port))
    {
        port = juce::jlimit(1024, 65535, port);
        apvts.getParameter(paramIdPort)
            ->setValueNotifyingHost((float)(port - 1024) / (65535 - 1024));
        connectPort = port;
        onModificationEnded();
    }

    // Host input (Caller mode only)
    if (currentMode == Mode::Caller)
    {
        ImGui::Text("Host:");
        if (ImGui::InputText("##host", hostInputBuffer, sizeof(hostInputBuffer)))
        {
            hostAddress = juce::String(hostInputBuffer);
            onModificationEnded();
        }
    }

    // Optional settings
    if (ImGui::CollapsingHeader("Advanced"))
    {
        // Latency
        int latency = (int)latencyParam->load();
        if (ImGui::SliderInt("Latency (ms)", &latency, 20, 8000))
        {
            apvts.getParameter(paramIdLatency)
                ->setValueNotifyingHost((float)(latency - 20) / (8000 - 20));
            onModificationEnded();
        }

        // Passphrase
        ImGui::Text("Passphrase:");
        if (ImGui::InputText(
                "##pass", passphraseBuffer, sizeof(passphraseBuffer), ImGuiInputTextFlags_Password))
        {
            setPassphrase(juce::String(passphraseBuffer));
            onModificationEnded();
        }

        // Stream ID
        ImGui::Text("Stream ID:");
        if (ImGui::InputText("##streamid", streamIdBuffer, sizeof(streamIdBuffer)))
        {
            setStreamId(juce::String(streamIdBuffer));
            onModificationEnded();
        }
    }

    // Connect/Disconnect buttons
    if (srtConnected || srtListening)
    {
        if (ImGui::Button("Disconnect", ImVec2(itemWidth, 0)))
        {
            disconnect();
            onModificationEnded();
        }
    }
    else
    {
        const char* buttonText = (currentMode == Mode::Listener) ? "Start Listening" : "Connect";
        if (ImGui::Button(buttonText, ImVec2(itemWidth, 0)))
        {
            if (currentMode == Mode::Listener)
                startListening(connectPort);
            else
                connectTo(hostAddress, connectPort);
            onModificationEnded();
        }
    }

    // Status
    ImVec4 statusColor = srtConnected ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                      : (srtListening ? ImVec4(1.0f, 1.0f, 0.3f, 1.0f)
                                                      : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextColored(statusColor, "%s", connectionStatus.toRawUTF8());

    // Statistics (when connected)
    if (srtConnected)
    {
        ImGui::Text("RTT: %lld ms", (long long)stats.rtt.load());
        ImGui::Text("Loss: %d pkts", stats.pktLoss.load());
        ImGui::Text("BW: %.2f Mbps", stats.bandwidth.load());
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

    ImGui::PopItemWidth();

    // Restore original values
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}

void SrtReceiverModule::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioOutputPin("Out L", 0);
    helpers.drawAudioOutputPin("Out R", 1);
}
#endif
