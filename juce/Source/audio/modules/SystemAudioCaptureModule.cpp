#include "SystemAudioCaptureModule.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include "../../preset_creator/theme/ThemeManager.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <comdef.h>

// WASAPI constants
const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
#endif

juce::AudioProcessorValueTreeState::ParameterLayout SystemAudioCaptureModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{paramIdGain, 1}, "Gain",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));

    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{paramIdDeviceIndex, 1}, "Device",
        -1, 31, -1));  // -1 = default device

    return {params.begin(), params.end()};
}

SystemAudioCaptureModule::SystemAudioCaptureModule()
    : ModuleProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("System Audio Capture Thread"),
      apvts(*this, nullptr, "SystemAudioParams", createParameterLayout())
{
    gainParam = apvts.getRawParameterValue(paramIdGain);
    deviceIndexParam = apvts.getRawParameterValue(paramIdDeviceIndex);

#ifdef _WIN32
    // Initialize COM for this thread
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    if (initializeWasapi())
    {
        refreshDeviceList();
        captureStatus = "Ready";
    }
#else
    captureStatus = "Not supported on this platform";
#endif
}

SystemAudioCaptureModule::~SystemAudioCaptureModule()
{
    signalThreadShouldExit();
    stopThread(5000);
    
#ifdef _WIN32
    cleanupWasapi();
    CoUninitialize();
#endif
}

bool SystemAudioCaptureModule::isSupported() const
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

#ifdef _WIN32
bool SystemAudioCaptureModule::initializeWasapi()
{
    HRESULT hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
        IID_IMMDeviceEnumerator, (void**)&deviceEnumerator);
    
    if (FAILED(hr))
    {
        captureStatus = "Failed to create device enumerator";
        juce::Logger::writeToLog("[SystemAudio] CoCreateInstance failed: " + juce::String::toHexString((int)hr));
        return false;
    }
    
    return true;
}

void SystemAudioCaptureModule::cleanupWasapi()
{
    stopCapture();
    
    if (deviceEnumerator)
    {
        deviceEnumerator->Release();
        deviceEnumerator = nullptr;
    }
}

void SystemAudioCaptureModule::captureLoop()
{
    // This runs in the capture thread
    while (!threadShouldExit() && captureActive)
    {
        if (!captureClient || !audioClient)
            break;

        UINT32 packetLength = 0;
        HRESULT hr = captureClient->GetNextPacketSize(&packetLength);
        
        if (FAILED(hr))
        {
            captureStatus = "Capture error";
            captureActive = false;
            break;
        }

        while (packetLength > 0)
        {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr))
                break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data != nullptr)
            {
                // Convert to float and write to ring buffer
                float* floatData = reinterpret_cast<float*>(data);
                
                int start1, size1, start2, size2;
                audioFifo.prepareToWrite((int)numFramesAvailable, start1, size1, start2, size2);

                int totalWritten = size1 + size2;
                if (totalWritten > 0)
                {
                    // WASAPI provides interleaved stereo float data
                    if (size1 > 0)
                    {
                        for (int i = 0; i < size1; ++i)
                        {
                            audioRingBuffer.setSample(0, start1 + i, floatData[i * 2]);
                            audioRingBuffer.setSample(1, start1 + i, floatData[i * 2 + 1]);
                        }
                    }
                    if (size2 > 0)
                    {
                        int offset = size1 * 2;
                        for (int i = 0; i < size2; ++i)
                        {
                            audioRingBuffer.setSample(0, start2 + i, floatData[offset + i * 2]);
                            audioRingBuffer.setSample(1, start2 + i, floatData[offset + i * 2 + 1]);
                        }
                    }
                    audioFifo.finishedWrite(totalWritten);
                }
            }

            captureClient->ReleaseBuffer(numFramesAvailable);
            captureClient->GetNextPacketSize(&packetLength);
        }

        // Small sleep to prevent busy-waiting
        juce::Thread::sleep(2);
    }
}
#endif

void SystemAudioCaptureModule::refreshDeviceList()
{
#ifdef _WIN32
    const juce::ScopedLock lock(devicesLock);
    availableDevices.clear();
    availableDevices.add("Default Output Device");

    if (!deviceEnumerator)
        return;

    IMMDeviceCollection* devices = nullptr;
    HRESULT hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    
    if (SUCCEEDED(hr) && devices)
    {
        UINT count = 0;
        devices->GetCount(&count);

        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(devices->Item(i, &device)) && device)
            {
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props)
                {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)))
                    {
                        if (varName.vt == VT_LPWSTR && varName.pwszVal)
                        {
                            availableDevices.add(juce::String(varName.pwszVal));
                        }
                        PropVariantClear(&varName);
                    }
                    props->Release();
                }
                device->Release();
            }
        }
        devices->Release();
    }
#endif
}

juce::StringArray SystemAudioCaptureModule::getAvailableDevices() const
{
    const juce::ScopedLock lock(devicesLock);
    return availableDevices;
}

bool SystemAudioCaptureModule::selectDevice(int deviceIndex)
{
    pendingDeviceIndex = deviceIndex;
    deviceChangeRequested = true;
    return true;
}

bool SystemAudioCaptureModule::initializeCapture()
{
#ifdef _WIN32
    stopCapture();

    if (!deviceEnumerator)
        return false;

    HRESULT hr;
    
    // Get the device
    if (selectedDeviceIndex < 0)
    {
        // Use default render endpoint
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &captureDevice);
        selectedDeviceName = "Default Output";
    }
    else
    {
        // Get specific device
        IMMDeviceCollection* devices = nullptr;
        hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
        
        if (SUCCEEDED(hr) && devices)
        {
            hr = devices->Item((UINT)selectedDeviceIndex, &captureDevice);
            devices->Release();
        }
    }

    if (FAILED(hr) || !captureDevice)
    {
        captureStatus = "Failed to get audio device";
        return false;
    }

    // Get device name
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(captureDevice->OpenPropertyStore(STGM_READ, &props)) && props)
    {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)))
        {
            if (varName.vt == VT_LPWSTR && varName.pwszVal)
                selectedDeviceName = juce::String(varName.pwszVal);
            PropVariantClear(&varName);
        }
        props->Release();
    }

    // Activate audio client
    hr = captureDevice->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr) || !audioClient)
    {
        captureStatus = "Failed to activate audio client";
        stopCapture();
        return false;
    }

    // Get mix format
    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat)
    {
        captureStatus = "Failed to get mix format";
        stopCapture();
        return false;
    }

    captureSampleRate = mixFormat->nSamplesPerSec;
    captureChannels = mixFormat->nChannels;

    // Initialize audio client in loopback mode
    REFERENCE_TIME bufferDuration = 10000000;  // 1 second in 100-nanosecond units
    
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        bufferDuration,
        0,
        mixFormat,
        nullptr);

    CoTaskMemFree(mixFormat);

    if (FAILED(hr))
    {
        captureStatus = "Failed to initialize: " + juce::String::toHexString((int)hr);
        stopCapture();
        return false;
    }

    // Get capture client
    hr = audioClient->GetService(IID_IAudioCaptureClient, (void**)&captureClient);
    if (FAILED(hr) || !captureClient)
    {
        captureStatus = "Failed to get capture client";
        stopCapture();
        return false;
    }

    // Start capture
    hr = audioClient->Start();
    if (FAILED(hr))
    {
        captureStatus = "Failed to start capture";
        stopCapture();
        return false;
    }

    captureActive = true;
    captureStatus = "Capturing: " + selectedDeviceName;
    
    juce::Logger::writeToLog("[SystemAudio] Started capture on: " + selectedDeviceName + 
        " @ " + juce::String(captureSampleRate) + "Hz");

    return true;
#else
    captureStatus = "Not supported";
    return false;
#endif
}

void SystemAudioCaptureModule::stopCapture()
{
    captureActive = false;

#ifdef _WIN32
    if (audioClient)
    {
        audioClient->Stop();
    }
    
    if (captureClient)
    {
        captureClient->Release();
        captureClient = nullptr;
    }
    
    if (audioClient)
    {
        audioClient->Release();
        audioClient = nullptr;
    }
    
    if (captureDevice)
    {
        captureDevice->Release();
        captureDevice = nullptr;
    }
#endif
}

void SystemAudioCaptureModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    
    if (isSupported())
    {
        startThread(juce::Thread::Priority::high);
    }
}

void SystemAudioCaptureModule::releaseResources()
{
    signalThreadShouldExit();
}

void SystemAudioCaptureModule::run()
{
#ifdef _WIN32
    // COM must be initialized for this thread
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    // Start capture with default device
    if (initializeCapture())
    {
        while (!threadShouldExit())
        {
            // Check for device change request
            if (deviceChangeRequested.exchange(false))
            {
                selectedDeviceIndex = pendingDeviceIndex;
                initializeCapture();
            }
            
            if (captureActive)
            {
                captureLoop();
            }
            else
            {
                wait(100);
            }
        }
    }
    
    stopCapture();
    CoUninitialize();
#else
    // Non-Windows: just wait
    while (!threadShouldExit())
    {
        wait(1000);
    }
#endif
}

void SystemAudioCaptureModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(2, buffer.getNumChannels());
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
    // Capture waveform for visualization
    {
        const int points = VizData::waveformPoints;
        const int stride = juce::jmax(1, numSamples / points);
        float peakL = 0.0f, peakR = 0.0f;

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

juce::ValueTree SystemAudioCaptureModule::getExtraStateTree() const
{
    juce::ValueTree tree("SystemAudioState");
    tree.setProperty("deviceIndex", selectedDeviceIndex, nullptr);
    tree.setProperty("deviceName", selectedDeviceName, nullptr);
    return tree;
}

void SystemAudioCaptureModule::setExtraStateTree(const juce::ValueTree& tree)
{
    if (tree.hasType("SystemAudioState"))
    {
        int savedIndex = tree.getProperty("deviceIndex", -1);
        selectDevice(savedIndex);
    }
}

#if defined(PRESET_CREATOR_UI)
void SystemAudioCaptureModule::drawParametersInNode(
    float itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>& onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    if (!isSupported())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Not supported on this platform");
        ImGui::TextWrapped("WASAPI loopback is Windows-only");
        ImGui::PopItemWidth();
        return;
    }

    // Refresh device list button
    if (ImGui::Button("Refresh Devices", ImVec2(itemWidth, 0)))
    {
        refreshDeviceList();
    }

    // Device selector
    juce::StringArray devices = getAvailableDevices();
    int currentIndex = selectedDeviceIndex + 1;  // +1 because -1 is "default"
    
    const char* currentDevice = (currentIndex >= 0 && currentIndex < devices.size()) 
        ? devices[currentIndex].toRawUTF8() 
        : "Select device...";

    if (ImGui::BeginCombo("Device", currentDevice))
    {
        for (int i = 0; i < devices.size(); ++i)
        {
            bool isSelected = (currentIndex == i);
            if (ImGui::Selectable(devices[i].toRawUTF8(), isSelected))
            {
                selectDevice(i - 1);  // -1 to convert back to device index
                onModificationEnded();
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Status
    ImVec4 statusColor = captureActive ? 
        ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    ImGui::TextColored(statusColor, "%s", captureStatus.toRawUTF8());

    // Gain slider
    bool gainModulated = isParamModulated(paramIdGain);
    if (gainModulated) ImGui::BeginDisabled();
    
    float gain = gainParam->load();
    if (ImGui::SliderFloat("Gain", &gain, 0.0f, 2.0f, "%.2f"))
    {
        apvts.getParameter(paramIdGain)->setValueNotifyingHost(gain / 2.0f);
        onModificationEnded();
    }
    
    if (gainModulated) ImGui::EndDisabled();

    // Waveform display
    ImGui::Text("Audio:");
    ImVec2 waveformSize(itemWidth, 40.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    drawList->AddRectFilled(pos, ImVec2(pos.x + waveformSize.x, pos.y + waveformSize.y),
        IM_COL32(30, 30, 30, 255));
    
    float centerY = pos.y + waveformSize.y / 2.0f;
    float halfHeight = waveformSize.y / 2.0f - 2.0f;
    
    for (int i = 0; i < VizData::waveformPoints - 1; ++i)
    {
        float x1 = pos.x + (float)i / VizData::waveformPoints * waveformSize.x;
        float x2 = pos.x + (float)(i + 1) / VizData::waveformPoints * waveformSize.x;
        float y1 = centerY - vizData.waveformL[i].load() * halfHeight;
        float y2 = centerY - vizData.waveformL[i + 1].load() * halfHeight;
        
        drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(100, 200, 255, 255));
    }
    
    ImGui::Dummy(waveformSize);

    ImGui::PopItemWidth();
}

void SystemAudioCaptureModule::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioOutputPin("Out L", 0);
    helpers.drawAudioOutputPin("Out R", 1);
}
#endif
