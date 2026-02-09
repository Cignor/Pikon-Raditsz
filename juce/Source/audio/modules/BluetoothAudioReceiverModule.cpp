#include "BluetoothAudioReceiverModule.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

#ifdef HAS_WINRT_AUDIO
// WinRT includes for Bluetooth audio (Windows 10 2004+)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Media::Audio;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::Bluetooth;
#endif

juce::AudioProcessorValueTreeState::ParameterLayout BluetoothAudioReceiverModule::
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
            juce::ParameterID{paramIdDeviceIndex, 1}, "Device", -1, 31, -1));

    return {params.begin(), params.end()};
}

BluetoothAudioReceiverModule::BluetoothAudioReceiverModule()
    : ModuleProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("Bluetooth Audio Thread"),
      apvts(*this, nullptr, "BluetoothAudioParams", createParameterLayout())
{
    gainParam = apvts.getRawParameterValue(paramIdGain);
    deviceIndexParam = apvts.getRawParameterValue(paramIdDeviceIndex);

    // Initialize audio device manager for fallback mode
    audioDeviceManager = std::make_unique<juce::AudioDeviceManager>();

#ifdef HAS_WINRT_AUDIO
    initializeWinRtBluetooth();
#endif

    connectionStatus = "Ready - Scan for devices";
}

BluetoothAudioReceiverModule::~BluetoothAudioReceiverModule()
{
    signalThreadShouldExit();
    stopThread(5000);
    stopAudioInput();

#ifdef HAS_WINRT_AUDIO
    cleanupWinRtBluetooth();
#endif
}

bool BluetoothAudioReceiverModule::isA2dpSinkSupported() const
{
#ifdef HAS_WINRT_AUDIO
    // A2DP Sink requires Windows 10 2004 (build 19041) or later
    return true;
#else
    return false;
#endif
}

#ifdef HAS_WINRT_AUDIO
bool BluetoothAudioReceiverModule::initializeWinRtBluetooth()
{
    try
    {
        winrt::init_apartment();
        juce::Logger::writeToLog("[BluetoothAudio] WinRT initialized for Bluetooth");
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        juce::Logger::writeToLog(
            "[BluetoothAudio] WinRT init failed: " +
            juce::String(winrt::to_string(e.message()).c_str()));
        return false;
    }
}

void BluetoothAudioReceiverModule::cleanupWinRtBluetooth()
{
    // WinRT cleanup happens automatically
}
#endif

void BluetoothAudioReceiverModule::scanForDevices()
{
    scanRequested = true;

    if (!isThreadRunning())
        startThread(juce::Thread::Priority::normal);
}

std::vector<BluetoothAudioReceiverModule::BluetoothDevice> BluetoothAudioReceiverModule::
    getDiscoveredDevices() const
{
    const juce::ScopedLock lock(devicesLock);
    return discoveredDevices;
}

void BluetoothAudioReceiverModule::scanBluetoothDevices()
{
    scanningActive = true;
    connectionStatus = "Scanning...";

    {
        const juce::ScopedLock lock(devicesLock);
        discoveredDevices.clear();
    }

#ifdef HAS_WINRT_AUDIO
    try
    {
        // Use WinRT to enumerate Bluetooth audio devices
        // Note: Must use fully qualified name because local BluetoothDevice struct shadows WinRT
        // type
        auto selector =
            winrt::Windows::Devices::Bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(
                true);
        auto devices = DeviceInformation::FindAllAsync(winrt::hstring(selector)).get();

        const juce::ScopedLock lock(devicesLock);

        for (const auto& device : devices)
        {
            BluetoothDevice btDevice;
            btDevice.name = juce::String(winrt::to_string(device.Name()).c_str());
            btDevice.address = juce::String(winrt::to_string(device.Id()).c_str());
            btDevice.isPaired = true;
            btDevice.isConnected = false; // Will check when connecting

            discoveredDevices.push_back(btDevice);

            juce::Logger::writeToLog("[BluetoothAudio] Found: " + btDevice.name);
        }

        connectionStatus = "Found " + juce::String(discoveredDevices.size()) + " devices";
    }
    catch (const winrt::hresult_error& e)
    {
        connectionStatus = "Scan failed: " + juce::String(winrt::to_string(e.message()).c_str());
    }
#else
    // Fallback: Look for Bluetooth devices in JUCE audio device list
    // Bluetooth audio devices typically appear as audio input devices when connected
    juce::StringArray inputDevices;

    auto& types = audioDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types)
    {
        auto deviceNames = type->getDeviceNames(true); // Input devices
        for (const auto& name : deviceNames)
        {
            // Heuristic: Look for common Bluetooth device names
            if (name.containsIgnoreCase("bluetooth") || name.containsIgnoreCase("airpods") ||
                name.containsIgnoreCase("buds") || name.containsIgnoreCase("headphones") ||
                name.containsIgnoreCase("headset") || name.containsIgnoreCase("speaker") ||
                name.containsIgnoreCase("bose") || name.containsIgnoreCase("sony") ||
                name.containsIgnoreCase("jbl") || name.containsIgnoreCase("beats"))
            {
                BluetoothDevice btDevice;
                btDevice.name = name;
                btDevice.address = name; // Use name as ID for audio device
                btDevice.isPaired = true;
                btDevice.isConnected = true; // If it appears as audio device, it's connected

                const juce::ScopedLock lock(devicesLock);
                discoveredDevices.push_back(btDevice);
            }
        }
    }

    {
        const juce::ScopedLock lock(devicesLock);
        connectionStatus = "Found " + juce::String(discoveredDevices.size()) + " audio devices";
    }
#endif

    scanningActive = false;
}

bool BluetoothAudioReceiverModule::connectToDevice(int deviceIndex)
{
    pendingDeviceIndex = deviceIndex;
    connectRequested = true;

    if (!isThreadRunning())
        startThread(juce::Thread::Priority::normal);

    return true;
}

void BluetoothAudioReceiverModule::disconnectDevice() { disconnectRequested = true; }

void BluetoothAudioReceiverModule::connectToBluetoothDevice(int index)
{
    BluetoothDevice device;

    {
        const juce::ScopedLock lock(devicesLock);
        if (index < 0 || index >= (int)discoveredDevices.size())
        {
            connectionStatus = "Invalid device index";
            return;
        }
        device = discoveredDevices[index];
    }

    connectionStatus = "Connecting to " + device.name + "...";

#ifdef HAS_WINRT_AUDIO
    try
    {
        // Try to create AudioPlaybackConnection for A2DP Sink
        // This requires Windows 10 2004+ and appropriate capabilities
        auto connection = AudioPlaybackConnection::TryCreateFromId(
            winrt::to_hstring(device.address.toStdString()));

        if (connection)
        {
            connection.Open();
            connection.Start();

            // Store the connection using WinRT's IUnknown COM interface
            // WinRT types are reference counted, so we detach and store the raw ABI pointer
            audioPlaybackConnection = winrt::detach_abi(connection);
            deviceConnected = true;
            connectedDeviceName = device.name;
            connectionStatus = "Connected: " + device.name;

            juce::Logger::writeToLog("[BluetoothAudio] A2DP Sink connected to: " + device.name);
        }
        else
        {
            // Fall back to audio device approach
            connectionStatus = "A2DP not available, using audio device";
            setupAudioInput();
        }
    }
    catch (const winrt::hresult_error& e)
    {
        connectionStatus = "Connect failed, trying audio device...";
        setupAudioInput();
    }
#else
    // Non-WinRT: Use JUCE audio device
    selectedAudioDeviceName = device.name;
    setupAudioInput();
#endif
}

void BluetoothAudioReceiverModule::setupAudioInput()
{
    // Set up audio capture from the Bluetooth device
    // This uses the device as an audio input source

    juce::String error;

    // Try to find and enable the Bluetooth device as an input
    auto& types = audioDeviceManager->getAvailableDeviceTypes();

    for (auto* type : types)
    {
        auto deviceNames = type->getDeviceNames(true);

        for (int i = 0; i < deviceNames.size(); ++i)
        {
            if (deviceNames[i].containsIgnoreCase(selectedAudioDeviceName) ||
                selectedAudioDeviceName.containsIgnoreCase(deviceNames[i]))
            {
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                setup.inputDeviceName = deviceNames[i];
                setup.useDefaultInputChannels = true;
                setup.sampleRate = currentSampleRate;
                setup.bufferSize = 512;

                error = audioDeviceManager->setAudioDeviceSetup(setup, true);

                if (error.isEmpty())
                {
                    deviceConnected = true;
                    connectedDeviceName = deviceNames[i];
                    connectionStatus = "Connected: " + connectedDeviceName;
                    juce::Logger::writeToLog(
                        "[BluetoothAudio] Audio input connected: " + connectedDeviceName);
                    return;
                }
            }
        }
    }

    connectionStatus = "Could not connect audio input";
    if (error.isNotEmpty())
        juce::Logger::writeToLog("[BluetoothAudio] Setup error: " + error);
}

void BluetoothAudioReceiverModule::stopAudioInput()
{
    deviceConnected = false;

#ifdef HAS_WINRT_AUDIO
    if (audioPlaybackConnection)
    {
        // Re-attach the ABI pointer to a WinRT object so it gets properly released
        AudioPlaybackConnection conn{nullptr};
        winrt::attach_abi(conn, audioPlaybackConnection);
        // conn destructor will decrement the COM reference count
        audioPlaybackConnection = nullptr;
    }
#endif

    audioDeviceManager->closeAudioDevice();
    connectedDeviceName.clear();
}

void BluetoothAudioReceiverModule::run()
{
    while (!threadShouldExit())
    {
        // Handle scan request
        if (scanRequested.exchange(false))
        {
            scanBluetoothDevices();
        }

        // Handle connect request
        if (connectRequested.exchange(false))
        {
            connectToBluetoothDevice(pendingDeviceIndex);
        }

        // Handle disconnect request
        if (disconnectRequested.exchange(false))
        {
            stopAudioInput();
            connectionStatus = "Disconnected";
        }

        // If connected, read audio from device
        if (deviceConnected && audioDeviceManager->getCurrentAudioDevice())
        {
            // Audio comes through the audio device callback
            // We would need to set up a callback to capture it
            // For now, this is handled by the system audio routing
        }

        wait(100);
    }
}

void BluetoothAudioReceiverModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
}

void BluetoothAudioReceiverModule::releaseResources() { signalThreadShouldExit(); }

void BluetoothAudioReceiverModule::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    const int   numSamples = buffer.getNumSamples();
    const int   numChannels = juce::jmin(2, buffer.getNumChannels());
    const float gain = gainParam->load();

    // Read from ring buffer (if audio is being captured)
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
        // No audio in buffer - output silence
        // Note: With Bluetooth, audio typically routes through system
        // and may need separate audio input node
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

juce::ValueTree BluetoothAudioReceiverModule::getExtraStateTree() const
{
    juce::ValueTree tree("BluetoothAudioState");
    tree.setProperty("deviceName", connectedDeviceName, nullptr);
    return tree;
}

void BluetoothAudioReceiverModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("BluetoothAudioState"))
    {
        juce::String savedDevice = tree.getProperty("deviceName", "").toString();
        if (savedDevice.isNotEmpty())
        {
            selectedAudioDeviceName = savedDevice;
            // Will attempt to reconnect when devices are scanned
        }
    }
}

#if defined(PRESET_CREATOR_UI)
void BluetoothAudioReceiverModule::drawParametersInNode(
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

    // Scan button
    if (scanningActive)
    {
        ImGui::BeginDisabled();
        ImGui::Button("Scanning...", ImVec2(itemWidth, 0));
        ImGui::EndDisabled();
    }
    else
    {
        if (ImGui::Button("Scan for Devices", ImVec2(itemWidth, 0)))
        {
            scanForDevices();
        }
    }

    // Device list
    auto devices = getDiscoveredDevices();

    if (devices.empty())
    {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No devices found");
        ImGui::TextWrapped(
            "Click 'Scan for Devices' to search for paired Bluetooth audio devices.");
    }
    else
    {
        ImGui::Text("Devices:");

        for (int i = 0; i < (int)devices.size(); ++i)
        {
            const auto& device = devices[i];

            juce::String label = device.name;
            if (device.isConnected)
                label += " (connected)";
            if (device.batteryLevel >= 0)
                label += " [" + juce::String(device.batteryLevel) + "%]";

            bool isSelected = (connectedDeviceName == device.name);

            if (ImGui::Selectable(label.toRawUTF8(), isSelected))
            {
                connectToDevice(i);
                onModificationEnded();
            }
        }
    }

    // Disconnect button
    if (deviceConnected)
    {
        if (ImGui::Button("Disconnect", ImVec2(itemWidth, 0)))
        {
            disconnectDevice();
            onModificationEnded();
        }
    }

    // Status
    ImVec4 statusColor =
        deviceConnected ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    ImGui::TextColored(statusColor, "%s", connectionStatus.toRawUTF8());

    // A2DP Sink info
    if (ImGui::CollapsingHeader("Info"))
    {
#ifdef HAS_WINRT_AUDIO
        ImGui::TextWrapped("A2DP Sink mode available (Windows 10 2004+)");
        ImGui::TextWrapped("Your PC can receive audio from phones and tablets.");
#else
        ImGui::TextWrapped("Basic Bluetooth audio support.");
        ImGui::TextWrapped(
            "Connect Bluetooth headphones/speakers first, then they will appear here.");
#endif
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

void BluetoothAudioReceiverModule::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioOutputPin("Out L", 0);
    helpers.drawAudioOutputPin("Out R", 1);
}
#endif
