#pragma once

#include "ModuleProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <array>

#ifdef _WIN32
// Forward declarations for Windows audio APIs
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;
#endif

/**
 * System Audio Capture Module (WASAPI Loopback)
 * 
 * Captures audio from the system's audio output using WASAPI loopback mode.
 * This allows capturing audio from any application (Spotify, YouTube, games, etc.)
 * 
 * Windows-specific implementation using WASAPI loopback API.
 * On other platforms, this module will show "Not supported" message.
 * 
 * Features:
 * - Capture any system audio
 * - Select specific audio output device
 * - Low-latency capture
 * - Automatic sample rate matching
 */
class SystemAudioCaptureModule : public ModuleProcessor, private juce::Thread
{
public:
    // Parameter IDs
    static constexpr auto paramIdGain = "gain";
    static constexpr auto paramIdDeviceIndex = "deviceIndex";
    
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

    SystemAudioCaptureModule();
    ~SystemAudioCaptureModule() override;

    const juce::String getName() const override { return "system_audio"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    // Device control
    juce::StringArray getAvailableDevices() const;
    bool selectDevice(int deviceIndex);
    bool isCapturing() const { return captureActive.load(); }
    juce::String getSelectedDeviceName() const { return selectedDeviceName; }
    juce::String getStatus() const { return captureStatus; }
    bool isSupported() const;

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
    void run() override;  // Capture thread
    bool initializeCapture();
    void stopCapture();
    void refreshDeviceList();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Parameters
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* deviceIndexParam = nullptr;

#ifdef _WIN32
    // Windows WASAPI objects
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* captureDevice = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    
    bool initializeWasapi();
    void cleanupWasapi();
    void captureLoop();
#endif

    // Device list
    juce::StringArray availableDevices;
    juce::CriticalSection devicesLock;
    int selectedDeviceIndex = -1;  // -1 = default device
    juce::String selectedDeviceName;
    std::atomic<bool> deviceChangeRequested{false};
    int pendingDeviceIndex = -1;

    // Capture state
    std::atomic<bool> captureActive{false};
    juce::String captureStatus{"Not initialized"};
    int captureSampleRate = 48000;
    int captureChannels = 2;

    // Audio ring buffer
    static constexpr int audioBufferSize = 48000;  // ~1 second at 48kHz
    juce::AbstractFifo audioFifo{audioBufferSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioBufferSize};
    double currentSampleRate = 44100.0;
};
