#pragma once

#include "ModuleProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <array>

/**
 * Bluetooth Audio Receiver Module
 * 
 * Receives audio from Bluetooth devices using the system's Bluetooth A2DP support.
 * 
 * On Windows 10 2004+, this uses the AudioPlaybackConnection WinRT API to enable
 * A2DP Sink mode, allowing the PC to receive audio from phones, tablets, etc.
 * 
 * For older Windows or other platforms, it falls back to showing Bluetooth audio
 * devices as selectable input sources (if they appear as audio devices).
 * 
 * Features:
 * - Scan for paired Bluetooth devices
 * - Enable A2DP Sink (receive audio from phone)
 * - Show connection status
 * - Display device battery level (if available)
 */
class BluetoothAudioReceiverModule : public ModuleProcessor, private juce::Thread
{
public:
    // Parameter IDs
    static constexpr auto paramIdGain = "gain";
    static constexpr auto paramIdDeviceIndex = "deviceIndex";
    
    // Device info structure
    struct BluetoothDevice
    {
        juce::String name;
        juce::String address;
        bool isPaired = false;
        bool isConnected = false;
        int batteryLevel = -1;  // -1 = unknown
    };
    
    // Visualization data
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> waveformL{};
        std::array<std::atomic<float>, waveformPoints> waveformR{};
        std::atomic<float> peakL{0.0f};
        std::atomic<float> peakR{0.0f};
    };
    VizData vizData;

    BluetoothAudioReceiverModule();
    ~BluetoothAudioReceiverModule() override;

    const juce::String getName() const override { return "bluetooth_audio"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Device management
    void scanForDevices();
    std::vector<BluetoothDevice> getDiscoveredDevices() const;
    bool connectToDevice(int deviceIndex);
    void disconnectDevice();
    bool isConnected() const { return deviceConnected.load(); }
    bool isScanning() const { return scanningActive.load(); }
    bool isA2dpSinkSupported() const;
    
    juce::String getStatus() const { return connectionStatus; }
    juce::String getConnectedDeviceName() const { return connectedDeviceName; }

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(float itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>& onModificationEnded) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    // State persistence
    juce::ValueTree getExtraStateTree() const override;
    void setExtraStateTree(const juce::ValueTree& tree) override;

private:
    void run() override;
    void scanBluetoothDevices();
    void connectToBluetoothDevice(int index);
    void setupAudioInput();
    void stopAudioInput();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Parameters
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* deviceIndexParam = nullptr;

    // Device list
    std::vector<BluetoothDevice> discoveredDevices;
    mutable juce::CriticalSection devicesLock;
    
    // Connection state
    std::atomic<bool> deviceConnected{false};
    std::atomic<bool> scanningActive{false};
    std::atomic<bool> scanRequested{false};
    std::atomic<bool> connectRequested{false};
    std::atomic<bool> disconnectRequested{false};
    int pendingDeviceIndex = -1;
    juce::String connectionStatus{"Ready"};
    juce::String connectedDeviceName;

    // Audio capture (fallback using JUCE audio devices)
    std::unique_ptr<juce::AudioDeviceManager> audioDeviceManager;
    juce::String selectedAudioDeviceName;
    
    // Audio ring buffer
    static constexpr int audioBufferSize = 48000;
    juce::AbstractFifo audioFifo{audioBufferSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
    double currentSampleRate = 44100.0;

#ifdef HAS_WINRT_AUDIO
    // WinRT handles for A2DP Sink (Windows 10 2004+)
    void* audioPlaybackConnection = nullptr;  // Windows::Media::Audio::AudioPlaybackConnection
    bool initializeWinRtBluetooth();
    void cleanupWinRtBluetooth();
#endif
};
