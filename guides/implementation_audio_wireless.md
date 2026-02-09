# Wireless & Streaming Audio Receiver Nodes - Implementation Plan

## Overview

This document outlines the implementation plan for adding wireless and streaming audio receiver nodes to Collider. These nodes enable capturing audio from various external sources including internet radio streams, live streaming protocols, system audio, and Bluetooth devices.

**Priority Order (by implementation complexity and user value):**

| Priority | Module | Complexity | Dependencies | User Value |
|----------|--------|------------|--------------|------------|
| 1 | Internet Radio Receiver | Low | FFmpeg (existing) | High - Huge content library |
| 2 | Virtual Audio Cable Input | Low | WASAPI Loopback | High - Capture any system audio |
| 3 | SRT Receiver | Medium | libsrt | High - Modern, low-latency streaming |
| 4 | RTMP Receiver | Medium | FFmpeg (existing) | High - OBS/Twitch/YouTube Live |
| 5 | Bluetooth Audio Receiver | Medium | WinRT (Windows SDK) | Medium - Wireless audio |

---

## CRITICAL: Dependencies to Install BEFORE Building

### Summary Table

| Library | Required For | Install Method | New Dependency? |
|---------|--------------|----------------|-----------------|
| FFmpeg | Internet Radio, RTMP | Already in vendor/ | **NO** |
| WASAPI | System Audio Capture | Windows SDK (built-in) | **NO** |
| libsrt | SRT Receiver | vcpkg | **YES** |
| C++/WinRT | Bluetooth Audio | Windows SDK 10.0.19041+ | **NO** (use SDK) |

### New Dependencies to Install

#### 1. libsrt (for SRT Receiver)

**Install via vcpkg:**
```powershell
# If you don't have vcpkg installed:
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# Install libsrt (includes OpenSSL dependency)
vcpkg install libsrt:x64-windows

# Integrate with system (makes libraries available to CMake)
vcpkg integrate install
```

**Expected installation location:** `C:\vcpkg\installed\x64-windows\`
- Headers: `include/srt/srt.h`
- Library: `lib/srt.lib`
- DLL: `bin/srt.dll`

**Dependencies pulled automatically:**
- OpenSSL (required by SRT for encryption)
- pthreads (Windows port)

#### 2. C++/WinRT (for Bluetooth Audio)

**Option A: Use Windows SDK (Recommended - No install needed)**
Windows SDK 10.0.19041.0 (Windows 10 2004) or later includes C++/WinRT headers.
You likely already have this if you're building for Windows 10/11.

**Verify your SDK version:**
```powershell
# Check installed SDKs
dir "C:\Program Files (x86)\Windows Kits\10\Include\"
# You need 10.0.19041.0 or higher for AudioPlaybackConnection
```

**Option B: Install via vcpkg (Alternative)**
```powershell
vcpkg install cppwinrt:x64-windows
```

### Libraries Already Available (No Action Needed)

| Library | Location | Used For |
|---------|----------|----------|
| FFmpeg | `vendor/ffmpeg/` | Internet Radio, RTMP decoding |
| WASAPI | Windows SDK | System audio capture (loopback) |
| OpenCV | `opencv_cuda_install/` | Video processing (if needed) |

### Windows SDK Requirements

For **Bluetooth Audio Receiver** (AudioPlaybackConnection API):
- **Minimum SDK:** Windows 10 SDK 10.0.19041.0 (May 2020 Update / Version 2004)
- **Minimum Runtime:** Windows 10 Version 2004 or later

### vcpkg Triplet

All vcpkg packages should use the `x64-windows` triplet for consistency with the rest of the project.

---

## CMakeLists.txt Modifications (Single Update)

Add the following section to `juce/CMakeLists.txt` AFTER the NDI section (~line 871):

```cmake
# ==============================================================================
# SRT (Secure Reliable Transport) - Low-latency streaming protocol
# ==============================================================================
# Install via vcpkg: vcpkg install libsrt:x64-windows
set(SRT_FOUND FALSE)

# Try to find SRT via vcpkg or system
find_package(SRT QUIET CONFIG)
if(SRT_FOUND)
    message(STATUS "✓ SRT found via CMake config")
    set(SRT_LIBRARIES srt::srt)
else()
    # Fallback: Try pkg-config on Linux/Mac
    if(NOT WIN32)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SRT srt)
        endif()
    endif()
    
    # Fallback: Manual search
    if(NOT SRT_FOUND)
        find_path(SRT_INCLUDE_DIR NAMES srt/srt.h
            PATHS 
                "${CMAKE_SOURCE_DIR}/../vendor/srt/include"
                "C:/vcpkg/installed/x64-windows/include"
                "/usr/include"
                "/usr/local/include"
        )
        find_library(SRT_LIBRARY NAMES srt
            PATHS
                "${CMAKE_SOURCE_DIR}/../vendor/srt/lib"
                "C:/vcpkg/installed/x64-windows/lib"
                "/usr/lib"
                "/usr/local/lib"
        )
        
        if(SRT_INCLUDE_DIR AND SRT_LIBRARY)
            set(SRT_FOUND TRUE)
            set(SRT_INCLUDE_DIRS ${SRT_INCLUDE_DIR})
            set(SRT_LIBRARIES ${SRT_LIBRARY})
            message(STATUS "✓ SRT found manually")
            message(STATUS "  - Include: ${SRT_INCLUDE_DIR}")
            message(STATUS "  - Library: ${SRT_LIBRARY}")
        endif()
    endif()
endif()

if(NOT SRT_FOUND)
    message(STATUS "SRT not found. SRT Receiver module will be disabled.")
    message(STATUS "  Install via: vcpkg install libsrt:x64-windows")
endif()

# ==============================================================================
# C++/WinRT for Bluetooth Audio (Windows only)
# ==============================================================================
set(CPPWINRT_AVAILABLE FALSE)

if(WIN32)
    # Check for Windows SDK with C++/WinRT support (10.0.19041.0+)
    # AudioPlaybackConnection requires Windows 10 2004 (build 19041)
    if(CMAKE_SYSTEM_VERSION VERSION_GREATER_EQUAL "10.0.19041")
        set(CPPWINRT_AVAILABLE TRUE)
        message(STATUS "✓ C++/WinRT available (Windows SDK ${CMAKE_SYSTEM_VERSION})")
    else()
        message(STATUS "C++/WinRT requires Windows SDK 10.0.19041.0 or later")
        message(STATUS "  Current: ${CMAKE_SYSTEM_VERSION}")
    endif()
endif()
```

Add to `target_link_libraries` for PresetCreatorApp and ColliderApp:

```cmake
# Link SRT if found
if(SRT_FOUND)
    target_link_libraries(${TARGET_NAME} PRIVATE ${SRT_LIBRARIES})
    target_include_directories(${TARGET_NAME} PRIVATE ${SRT_INCLUDE_DIRS})
    target_compile_definitions(${TARGET_NAME} PRIVATE HAS_SRT=1)
endif()

# Link WinRT for Bluetooth Audio (Windows only)
if(WIN32 AND CPPWINRT_AVAILABLE)
    target_link_libraries(${TARGET_NAME} PRIVATE WindowsApp.lib)
    target_compile_definitions(${TARGET_NAME} PRIVATE HAS_WINRT_BLUETOOTH=1)
endif()
```

---

## Post-Build: DLL Copy Requirements

Add to the DLL copy section for SRT:

```cmake
# Copy SRT DLL if found
if(SRT_FOUND AND WIN32)
    find_file(SRT_DLL srt.dll
        PATHS "C:/vcpkg/installed/x64-windows/bin"
        NO_DEFAULT_PATH
    )
    if(SRT_DLL)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SRT_DLL}"
                    "$<TARGET_FILE_DIR:${TARGET_NAME}>"
            COMMENT "Copying SRT DLL"
        )
    endif()
endif()
```

---

## Common Architecture Patterns

All streaming receiver modules share a common architecture derived from the existing `NdiReceiverModule`:

### Audio Ring Buffer Pattern
```cpp
// Thread-safe audio transfer between network/capture thread and audio thread
static constexpr int     audioBufferSize = 48000; // ~1 second at 48kHz
juce::AbstractFifo       audioFifo{audioBufferSize};
juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
std::atomic<int>         sourceSampleRate{48000};
std::atomic<int>         sourceNumChannels{2};
double                   currentSampleRate = 44100.0;
```

### Receiver Thread Pattern
```cpp
class StreamReceiverModule : public ModuleProcessor, private juce::Thread
{
    void run() override
    {
        while (!threadShouldExit())
        {
            // 1. Check connection state
            // 2. Receive audio data
            // 3. Write to ring buffer
            // 4. Update visualization data
        }
    }
};
```

### ProcessBlock Pattern (consuming from ring buffer)
```cpp
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(2, buffer.getNumChannels());
    
    int start1, size1, start2, size2;
    audioFifo.prepareToRead(numSamples, start1, size1, start2, size2);
    int totalRead = size1 + size2;
    
    if (totalRead > 0) {
        if (size1 > 0) {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom(ch, 0, audioRingBuffer, ch, start1, size1);
        }
        if (size2 > 0) {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom(ch, size1, audioRingBuffer, ch, start2, size2);
        }
        audioFifo.finishedRead(totalRead);
        
        if (totalRead < numSamples) {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.clear(ch, totalRead, numSamples - totalRead);
        }
    } else {
        buffer.clear();
    }
}
```

---

## Module 1: Internet Radio Receiver (Icecast/Shoutcast)

### Overview
Receives audio from internet radio streams using HTTP streaming protocols. Supports Icecast, Shoutcast, and direct MP3/AAC/OGG streams.

### Internal Name
`internet_radio_receiver`

### Display Name
"Internet Radio"

### Category
Sources

### Dependencies
- **FFmpeg** (already integrated in project for video decoding)
- Uses `libavformat` for stream demuxing
- Uses `libavcodec` for audio decoding
- Uses `libswresample` for sample rate conversion

### Files to Create
- `juce/Source/audio/modules/InternetRadioReceiverModule.h`
- `juce/Source/audio/modules/InternetRadioReceiverModule.cpp`

### Parameters
| Parameter ID | Type | Range | Default | Description |
|--------------|------|-------|---------|-------------|
| `streamUrl` | String | - | "" | Stream URL (http://...) |
| `gain` | Float | 0.0-2.0 | 1.0 | Output volume |
| `reconnectOnFail` | Bool | - | true | Auto-reconnect on disconnect |
| `bufferSizeMs` | Int | 100-5000 | 1000 | Network buffer size in ms |

### Pin Configuration
```cpp
db["internet_radio_receiver"] = ModulePinInfo(
    NodeWidth::Medium,
    {},  // No inputs - source module
    {
        AudioPin("Out L", 0, PinDataType::Audio),
        AudioPin("Out R", 1, PinDataType::Audio)
    },
    {}
);
```

### UI Elements
1. **URL Text Input** - Enter stream URL
2. **Preset Dropdown** - Popular radio presets (optional)
3. **Connect/Disconnect Button**
4. **Status Display** - "Connecting...", "Buffering...", "Playing", "Error"
5. **Stream Info** - Bitrate, codec, station name (from ICY metadata)
6. **Gain Slider**
7. **Waveform Scope** - Visual feedback of audio

### Implementation Details

#### Header Structure
```cpp
#pragma once
#include "ModuleProcessor.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>

// Forward declarations for FFmpeg (avoid including headers)
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVPacket;
struct AVFrame;

class InternetRadioReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    // Visualization data
    struct VizData {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL{};
        std::array<std::atomic<float>, waveformPoints> waveformR{};
        std::atomic<float> peakL{0.0f};
        std::atomic<float> peakR{0.0f};
    };
    VizData vizData;

    InternetRadioReceiverModule();
    ~InternetRadioReceiverModule() override;

    const juce::String getName() const override { return "internet_radio_receiver"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Stream control
    bool connectToStream(const juce::String& url);
    void disconnectFromStream();
    bool isConnected() const { return streamConnected.load(); }

    // ICY metadata
    juce::String getStationName() const;
    juce::String getCurrentSong() const;
    int getBitrate() const { return streamBitrate.load(); }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>& onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    juce::ValueTree getExtraStateTree() const override;
    void setExtraStateTree(const juce::ValueTree& tree) override;

private:
    void run() override;  // Network thread
    bool openStream(const juce::String& url);
    void closeStream();
    void processAudioPacket();
    void decodeAndBuffer(AVFrame* frame);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Parameters
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* bufferSizeMsParam = nullptr;

    // FFmpeg contexts
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    int audioStreamIndex = -1;

    // Connection state
    juce::String currentUrl;
    std::atomic<bool> streamConnected{false};
    std::atomic<bool> connectionRequested{false};
    std::atomic<bool> disconnectRequested{false};
    juce::String connectionStatus{"Not connected"};

    // ICY metadata
    juce::String stationName;
    juce::String currentSong;
    std::atomic<int> streamBitrate{0};
    juce::CriticalSection metadataLock;

    // Audio ring buffer (network thread writes, audio thread reads)
    static constexpr int audioBufferSize = 96000; // ~2 seconds at 48kHz
    juce::AbstractFifo audioFifo{audioBufferSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
    std::atomic<int> sourceSampleRate{44100};
    std::atomic<int> sourceNumChannels{2};
    double currentSampleRate = 44100.0;

    // Reconnection logic
    std::atomic<bool> autoReconnect{true};
    int reconnectAttempts = 0;
    static constexpr int maxReconnectAttempts = 5;
};
```

#### Key Implementation Points

1. **FFmpeg Initialization** (in constructor or first connect)
```cpp
// Register network protocols
avformat_network_init();

// Open stream with ICY metadata parsing
AVDictionary* options = nullptr;
av_dict_set(&options, "icy", "1", 0);  // Request ICY metadata
av_dict_set(&options, "timeout", "5000000", 0);  // 5s timeout

int ret = avformat_open_input(&formatCtx, url.toRawUTF8(), nullptr, &options);
av_dict_free(&options);
```

2. **ICY Metadata Parsing**
```cpp
// In the receive loop, check for metadata updates
AVDictionaryEntry* tag = nullptr;
while ((tag = av_dict_get(formatCtx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
    if (juce::String(tag->key).equalsIgnoreCase("icy-name"))
        stationName = juce::String::fromUTF8(tag->value);
    else if (juce::String(tag->key).equalsIgnoreCase("StreamTitle"))
        currentSong = juce::String::fromUTF8(tag->value);
}
```

3. **Sample Rate Conversion**
```cpp
// Initialize SwrContext for resampling to DAW sample rate
swrCtx = swr_alloc_set_opts(nullptr,
    AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, (int)currentSampleRate,
    codecCtx->channel_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
    0, nullptr);
swr_init(swrCtx);
```

### Registration Checklist
- [ ] Factory registration in `ModularSynthProcessor.cpp`
- [ ] Pin database in `PinDatabase.cpp`
- [ ] Description in `PinDatabase.cpp`
- [ ] Left panel menu (Sources category)
- [ ] Right-click context menu
- [ ] Top bar Insert Between menu (not applicable - source)
- [ ] Insert on cable menu (not applicable - source)
- [ ] Search database entry
- [ ] Category matcher (Source)

---

## Module 2: Virtual Audio Cable Input (System Audio Capture)

### Overview
Captures system audio output using WASAPI loopback on Windows, or equivalent APIs on macOS/Linux. This allows routing any system audio (Spotify, YouTube, games, etc.) into Collider.

### Internal Name
`system_audio_capture`

### Display Name
"System Audio"

### Category
Sources

### Dependencies
- **Windows**: WASAPI Loopback API (built into Windows)
- **macOS**: CoreAudio (with virtual audio device or BlackHole)
- **Linux**: PulseAudio monitor sources

### Files to Create
- `juce/Source/audio/modules/SystemAudioCaptureModule.h`
- `juce/Source/audio/modules/SystemAudioCaptureModule.cpp`

### Parameters
| Parameter ID | Type | Range | Default | Description |
|--------------|------|-------|---------|-------------|
| `deviceIndex` | Int | -1-31 | -1 | Audio device to capture |
| `gain` | Float | 0.0-2.0 | 1.0 | Output volume |
| `latencyMs` | Int | 10-500 | 50 | Capture latency |

### Pin Configuration
```cpp
db["system_audio_capture"] = ModulePinInfo(
    NodeWidth::Small,
    {},  // No inputs - source module
    {
        AudioPin("Out L", 0, PinDataType::Audio),
        AudioPin("Out R", 1, PinDataType::Audio)
    },
    {}
);
```

### UI Elements
1. **Device Dropdown** - List of audio output devices (for loopback capture)
2. **Gain Slider**
3. **Status Display** - "Capturing", "Device not available"
4. **Level Meters** - Visual feedback

### Implementation Details

#### Windows WASAPI Loopback
```cpp
#ifdef _WIN32
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

class SystemAudioCaptureModule : public ModuleProcessor, private juce::Thread
{
private:
    // WASAPI objects
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* captureDevice = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    
    bool initializeWasapiLoopback(int deviceIndex)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        
        // Create device enumerator
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&deviceEnumerator);
        
        // Get default render endpoint (output device) for loopback
        if (deviceIndex < 0) {
            deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &captureDevice);
        } else {
            IMMDeviceCollection* devices;
            deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
            devices->Item(deviceIndex, &captureDevice);
            devices->Release();
        }
        
        // Initialize audio client in loopback mode
        captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        
        WAVEFORMATEX* mixFormat;
        audioClient->GetMixFormat(&mixFormat);
        
        // Initialize with AUDCLNT_STREAMFLAGS_LOOPBACK for system audio capture
        audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            10000000,  // 1 second buffer
            0,
            mixFormat,
            nullptr);
        
        audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
        audioClient->Start();
        
        CoTaskMemFree(mixFormat);
        return true;
    }
    
    void captureLoop()
    {
        while (!threadShouldExit())
        {
            UINT32 packetLength = 0;
            captureClient->GetNextPacketSize(&packetLength);
            
            while (packetLength > 0)
            {
                BYTE* data;
                UINT32 numFramesAvailable;
                DWORD flags;
                
                captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
                
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                {
                    // Write audio to ring buffer
                    writeToRingBuffer((float*)data, numFramesAvailable);
                }
                
                captureClient->ReleaseBuffer(numFramesAvailable);
                captureClient->GetNextPacketSize(&packetLength);
            }
            
            juce::Thread::sleep(5);  // Small sleep to prevent busy-waiting
        }
    }
};
#endif
```

#### macOS CoreAudio
On macOS, loopback requires either:
1. A virtual audio device (like BlackHole or Loopback)
2. Screen recording permissions for system audio

```cpp
#ifdef __APPLE__
// Use existing AudioInputModuleProcessor pattern but with device selection
// for BlackHole or other virtual audio cable devices
#endif
```

### Registration Checklist
- [ ] Factory registration in `ModularSynthProcessor.cpp`
- [ ] Pin database in `PinDatabase.cpp`
- [ ] Description in `PinDatabase.cpp`
- [ ] Left panel menu (Sources category)
- [ ] Right-click context menu
- [ ] Search database entry
- [ ] Category matcher (Source)

---

## Module 3: SRT Receiver

### Overview
Receives audio (and optionally video) using the Secure Reliable Transport (SRT) protocol. SRT is designed for low-latency live streaming and is widely used in professional broadcasting.

### Internal Name
`srt_receiver`

### Display Name
"SRT Receiver"

### Category
Sources

### Dependencies
- **libsrt** - Open source SRT library
  - Windows: Build from source or use vcpkg
  - Website: https://github.com/Haivision/srt
  - License: MPL-2.0

### CMake Integration
```cmake
# In juce/CMakeLists.txt
find_package(SRT QUIET)
if(SRT_FOUND)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE HAS_SRT=1)
    target_link_libraries(${PLUGIN_NAME} PRIVATE srt::srt)
endif()
```

### Files to Create
- `juce/Source/audio/modules/SrtReceiverModule.h`
- `juce/Source/audio/modules/SrtReceiverModule.cpp`

### Parameters
| Parameter ID | Type | Range | Default | Description |
|--------------|------|-------|---------|-------------|
| `mode` | Choice | Listener/Caller | Listener | Connection mode |
| `port` | Int | 1024-65535 | 9000 | Listen/connect port |
| `host` | String | - | "" | Host address (for Caller mode) |
| `streamId` | String | - | "" | SRT stream ID |
| `passphrase` | String | - | "" | Encryption passphrase |
| `latencyMs` | Int | 20-8000 | 120 | SRT latency setting |
| `gain` | Float | 0.0-2.0 | 1.0 | Output volume |

### Pin Configuration
```cpp
db["srt_receiver"] = ModulePinInfo(
    NodeWidth::Medium,
    {},  // No inputs - source module
    {
        AudioPin("Out L", 0, PinDataType::Audio),
        AudioPin("Out R", 1, PinDataType::Audio)
    },
    {}
);
```

### UI Elements
1. **Mode Selector** - Listener (wait for connection) / Caller (connect to remote)
2. **Host Input** - Remote host address (Caller mode)
3. **Port Input**
4. **Stream ID Input** (optional)
5. **Passphrase Input** (optional, for encryption)
6. **Latency Slider**
7. **Connect/Listen Button**
8. **Status Display** - Connection state, statistics
9. **Stream Statistics** - RTT, packet loss, bandwidth

### Implementation Details

#### Header Structure
```cpp
#pragma once
#include "ModuleProcessor.h"

#ifdef HAS_SRT
#include <srt/srt.h>
#endif

class SrtReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    enum class Mode { Listener, Caller };
    
    SrtReceiverModule();
    ~SrtReceiverModule() override;

    const juce::String getName() const override { return "srt_receiver"; }

    // Connection control
    bool startListening(int port);
    bool connectTo(const juce::String& host, int port);
    void disconnect();

    // Statistics
    struct Stats {
        int64_t rtt = 0;           // Round-trip time (ms)
        int pktLoss = 0;           // Packets lost
        double bandwidth = 0.0;    // Mbps
        int64_t byteRecv = 0;      // Total bytes received
    };
    Stats getStats() const;

#ifdef HAS_SRT
private:
    SRTSOCKET srtSocket = SRT_INVALID_SOCK;
    SRTSOCKET connectedSocket = SRT_INVALID_SOCK;
    
    bool initializeSrt();
    void cleanupSrt();
    void receiveLoop();
    void parseMediaPacket(const uint8_t* data, int size);
#endif
};
```

#### SRT Listener Mode
```cpp
bool SrtReceiverModule::startListening(int port)
{
#ifdef HAS_SRT
    srt_startup();
    
    srtSocket = srt_create_socket();
    if (srtSocket == SRT_INVALID_SOCK) return false;
    
    // Set socket options
    int latency = latencyMsParam->load();
    srt_setsockopt(srtSocket, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));
    
    bool blocking = false;
    srt_setsockopt(srtSocket, 0, SRTO_RCVSYN, &blocking, sizeof(blocking));
    
    // Bind and listen
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (srt_bind(srtSocket, (sockaddr*)&addr, sizeof(addr)) == SRT_ERROR)
        return false;
    
    if (srt_listen(srtSocket, 1) == SRT_ERROR)
        return false;
    
    isListening = true;
    startThread();
    return true;
#else
    connectionStatus = "SRT not available";
    return false;
#endif
}
```

#### SRT Caller Mode
```cpp
bool SrtReceiverModule::connectTo(const juce::String& host, int port)
{
#ifdef HAS_SRT
    srtSocket = srt_create_socket();
    
    // Set latency
    int latency = latencyMsParam->load();
    srt_setsockopt(srtSocket, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));
    
    // Set passphrase if provided
    juce::String passphrase = getExtraStateTree().getProperty("passphrase", "").toString();
    if (passphrase.isNotEmpty()) {
        srt_setsockopt(srtSocket, 0, SRTO_PASSPHRASE, 
            passphrase.toRawUTF8(), passphrase.length());
    }
    
    // Connect
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.toRawUTF8(), &addr.sin_addr);
    
    if (srt_connect(srtSocket, (sockaddr*)&addr, sizeof(addr)) == SRT_ERROR)
        return false;
    
    connectedSocket = srtSocket;
    isConnected = true;
    startThread();
    return true;
#else
    return false;
#endif
}
```

### Registration Checklist
- [ ] Factory registration (with `#ifdef HAS_SRT` guard)
- [ ] Pin database
- [ ] Description
- [ ] Left panel menu
- [ ] Right-click context menu
- [ ] Search database entry
- [ ] Category matcher (Source)

---

## Module 4: RTMP Receiver

### Overview
Receives RTMP streams, commonly used for live streaming from OBS Studio, Twitch re-streams, and similar applications. Uses FFmpeg's libavformat which already supports RTMP.

### Internal Name
`rtmp_receiver`

### Display Name
"RTMP Receiver"

### Category
Sources

### Dependencies
- **FFmpeg** (already integrated)
- Uses `libavformat` with RTMP protocol support

### Files to Create
- `juce/Source/audio/modules/RtmpReceiverModule.h`
- `juce/Source/audio/modules/RtmpReceiverModule.cpp`

### Parameters
| Parameter ID | Type | Range | Default | Description |
|--------------|------|-------|---------|-------------|
| `listenPort` | Int | 1024-65535 | 1935 | RTMP listen port |
| `streamKey` | String | - | "" | Expected stream key |
| `gain` | Float | 0.0-2.0 | 1.0 | Output volume |
| `bufferSizeMs` | Int | 100-5000 | 500 | Receive buffer |

### Pin Configuration
```cpp
db["rtmp_receiver"] = ModulePinInfo(
    NodeWidth::Medium,
    {},  // No inputs - source module
    {
        AudioPin("Out L", 0, PinDataType::Audio),
        AudioPin("Out R", 1, PinDataType::Audio),
        AudioPin("Video", 2, PinDataType::Video)  // Optional video output
    },
    {}
);
```

### UI Elements
1. **Listen Port Input**
2. **Stream Key Input** (for security)
3. **Start/Stop Server Button**
4. **Status Display** - "Listening on port 1935", "Stream connected: OBS"
5. **Stream Info** - Encoder, bitrate, resolution
6. **Gain Slider**
7. **Waveform Scope**

### Implementation Details

#### RTMP Server Setup
Since FFmpeg's avformat primarily supports RTMP as a client, for server functionality we have two options:

**Option A: Use FFmpeg as RTMP server (simpler)**
```cpp
// FFmpeg can receive RTMP via TCP server mode
// rtmp://0.0.0.0:1935/live/stream_key

bool RtmpReceiverModule::startServer(int port, const juce::String& streamKey)
{
    juce::String url = juce::String::formatted("rtmp://0.0.0.0:%d/live/%s", 
        port, streamKey.toRawUTF8());
    
    // This requires special FFmpeg build or custom RTMP server
    // Most common approach is to run a simple RTMP server (nginx-rtmp, SRS)
    // and have the module connect to it as a client
    
    return openAsClient(url);
}
```

**Option B: Connect to external RTMP server**
```cpp
// More practical: Connect to RTMP URL as client (for re-streaming scenarios)
bool RtmpReceiverModule::connectToStream(const juce::String& rtmpUrl)
{
    // rtmp://server.com/live/streamkey
    // rtmp://localhost:1935/live/stream
    
    AVDictionary* options = nullptr;
    av_dict_set(&options, "timeout", "5000000", 0);
    av_dict_set(&options, "rtmp_live", "live", 0);
    
    int ret = avformat_open_input(&formatCtx, rtmpUrl.toRawUTF8(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) return false;
    
    // Find audio stream
    avformat_find_stream_info(formatCtx, nullptr);
    
    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }
    
    // Initialize decoder and resampler (same as Internet Radio)
    return initializeDecoder();
}
```

### Usage Scenarios
1. **OBS Studio** → Stream to `rtmp://collider-pc:1935/live/mykey`
2. **Twitch Re-stream** → Connect to Twitch RTMP URL
3. **Local Server** → Run nginx-rtmp, point module to `rtmp://localhost:1935/live/stream`

### Registration Checklist
- [ ] Factory registration
- [ ] Pin database
- [ ] Description
- [ ] Left panel menu
- [ ] Right-click context menu
- [ ] Search database entry
- [ ] Category matcher (Source)

---

## Module 5: Bluetooth Audio Receiver

### Overview
Receives audio from Bluetooth devices. This is the most complex module due to platform-specific Bluetooth APIs.

### Internal Name
`bluetooth_audio_receiver`

### Display Name
"Bluetooth Audio"

### Category
Sources

### Dependencies
- **Windows**: Windows.Devices.Bluetooth API (WinRT)
- **macOS**: CoreBluetooth / AVAudioSession
- **Linux**: BlueZ D-Bus API + PulseAudio

### Files to Create
- `juce/Source/audio/modules/BluetoothAudioReceiverModule.h`
- `juce/Source/audio/modules/BluetoothAudioReceiverModule.cpp`
- `juce/Source/audio/modules/platform/BluetoothWindows.cpp`
- `juce/Source/audio/modules/platform/BluetoothMacOS.mm`
- `juce/Source/audio/modules/platform/BluetoothLinux.cpp`

### Parameters
| Parameter ID | Type | Range | Default | Description |
|--------------|------|-------|---------|-------------|
| `deviceIndex` | Int | -1-31 | -1 | Selected Bluetooth device |
| `gain` | Float | 0.0-2.0 | 1.0 | Output volume |
| `codec` | Choice | SBC/AAC/aptX | SBC | Preferred codec |

### Pin Configuration
```cpp
db["bluetooth_audio_receiver"] = ModulePinInfo(
    NodeWidth::Medium,
    {},  // No inputs - source module
    {
        AudioPin("Out L", 0, PinDataType::Audio),
        AudioPin("Out R", 1, PinDataType::Audio)
    },
    {}
);
```

### UI Elements
1. **Scan Button** - Scan for Bluetooth devices
2. **Device List** - Available paired devices
3. **Connect/Disconnect Button**
4. **Status Display** - "Scanning...", "Connected to AirPods Pro"
5. **Codec Info** - Active codec (SBC, AAC, aptX, etc.)
6. **Battery Level** (if available)
7. **Gain Slider**

### Implementation Details

#### Platform Abstraction
```cpp
// BluetoothAudioProvider.h - Abstract interface
class BluetoothAudioProvider
{
public:
    virtual ~BluetoothAudioProvider() = default;
    
    struct DeviceInfo {
        juce::String name;
        juce::String address;
        bool isPaired;
        bool isConnected;
        int batteryLevel;  // -1 if unknown
    };
    
    virtual bool startScanning() = 0;
    virtual void stopScanning() = 0;
    virtual std::vector<DeviceInfo> getDiscoveredDevices() = 0;
    
    virtual bool connectToDevice(const juce::String& address) = 0;
    virtual void disconnectDevice() = 0;
    virtual bool isConnected() const = 0;
    
    // Audio callback - called from Bluetooth thread
    virtual void setAudioCallback(std::function<void(const float*, int, int)> callback) = 0;
    
    static std::unique_ptr<BluetoothAudioProvider> create();
};
```

#### Windows Implementation (WinRT)
```cpp
#ifdef _WIN32
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Media.Audio.h>

class BluetoothAudioProviderWindows : public BluetoothAudioProvider
{
    // Uses Windows.Devices.Bluetooth.BluetoothDevice
    // Audio routing through Windows audio APIs
};
#endif
```

#### Simplified Approach (Recommended for v1)
Instead of implementing full Bluetooth A2DP sink, leverage the system's existing Bluetooth audio support:

```cpp
// On Windows, Bluetooth audio devices appear as audio endpoints
// Use the same approach as SystemAudioCaptureModule but filter for Bluetooth devices

class BluetoothAudioReceiverModule : public ModuleProcessor
{
    // List audio input devices, filter by name containing "Bluetooth" or device type
    juce::StringArray getBluetoothAudioDevices()
    {
        juce::StringArray devices;
        auto& dm = juce::AudioDeviceManager();
        
        for (auto& type : dm.getAvailableDeviceTypes()) {
            for (auto& name : type->getDeviceNames(true)) {  // Input devices
                // Heuristic: Bluetooth devices often have specific names
                if (name.containsIgnoreCase("bluetooth") ||
                    name.containsIgnoreCase("airpods") ||
                    name.containsIgnoreCase("buds") ||
                    isBluetoothDevice(name)) {
                    devices.add(name);
                }
            }
        }
        return devices;
    }
};
```

### Registration Checklist
- [ ] Factory registration (with platform guards)
- [ ] Pin database
- [ ] Description
- [ ] Left panel menu
- [ ] Right-click context menu
- [ ] Search database entry
- [ ] Category matcher (Source)

---

## Implementation Status

### All Modules Implemented ✓

| Module | Status | Files Created |
|--------|--------|---------------|
| Internet Radio | ✅ Complete | `InternetRadioReceiverModule.h/.cpp` |
| System Audio | ✅ Complete | `SystemAudioCaptureModule.h/.cpp` |
| SRT Receiver | ✅ Complete | `SrtReceiverModule.h/.cpp` |
| RTMP Receiver | ✅ Complete | `RtmpReceiverModule.h/.cpp` |
| Bluetooth Audio | ✅ Complete | `BluetoothAudioReceiverModule.h/.cpp` |

### Registration Complete ✓

All modules registered in:
- ✅ Factory (`ModularSynthProcessor.cpp`)
- ✅ Pin Database (`PinDatabase.cpp`)
- ✅ Module Descriptions (`PinDatabase.cpp`)
- ✅ Left Panel Menu (`ImGuiNodeEditorComponent.cpp`)
- ✅ Right-Click Context Menu (`ImGuiNodeEditorComponent.cpp`)
- ✅ Search Database (`ImGuiNodeEditorComponent.cpp`)
- ✅ Category Matcher (`ImGuiNodeEditorComponent.cpp`)
- ✅ CMakeLists.txt (source files added)

### Test URLs for Internet Radio

- `http://ice1.somafm.com/groovesalad-128-mp3` (SomaFM Groove Salad)
- `http://ice1.somafm.com/defcon-128-mp3` (SomaFM DEF CON)
- `http://streams.kqed.org/kqedradio` (KQED)

---

## Testing Checklist

### For Each Module:
- [ ] Module appears in Left Panel menu (Sources category)
- [ ] Module appears in Right-click context menu
- [ ] Module appears in Search results
- [ ] Module has correct category color (green for Sources)
- [ ] Tooltip/description displays correctly
- [ ] Audio output works at different sample rates (44.1k, 48k, 96k)
- [ ] No audio glitches during normal operation
- [ ] Graceful handling of connection loss
- [ ] State saves and loads correctly in presets
- [ ] No memory leaks (check with Valgrind/Dr. Memory)
- [ ] Thread safety verified (no race conditions)

---

## References

- **NdiReceiverModule** - Reference implementation for network streaming with video+audio
- **AudioInputModuleProcessor** - Reference for audio device handling
- **VideoFileLoaderModule** - Reference for FFmpeg integration
- **ADD_NEW_NODE_COMPREHENSIVE_GUIDE.md** - Complete registration checklist

---

**Last Updated:** February 2026  
**Author:** Collider Modular Synthesizer Project
