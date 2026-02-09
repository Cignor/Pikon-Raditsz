/*
  ==============================================================================

    SdrReceiverModule.h
    Created: 3 Feb 2026
    Author:  Antigravity

  ==============================================================================
*/

#pragma once

#include "ModuleProcessor.h"
#include "SdrDemodulator.h"
#include <chrono>
#include <cstdio>

// Forward declare librtlsdr struct to keep header clean
struct rtlsdr_dev;

class SdrReceiverModule : public ModuleProcessor
{
public:
    SdrReceiverModule();
    ~SdrReceiverModule() override;

    // ModuleProcessor Override
    const juce::String                  getName() const override { return "sdr_receiver"; }
    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded,
        const NodePinHelpers*                                   pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;

    // Custom node size for waterfall display
    ImVec2 getCustomNodeSize() const override;
#endif

    // Parameters - Basic
    static constexpr auto paramIdFreq = "freq";           // MHz
    static constexpr auto paramIdGain = "gain";           // RF Gain (dB)
    static constexpr auto paramIdSquelch = "squelch";     // Squelch Threshold (dB)
    static constexpr auto paramIdMode = "mode";           // Demod Mode: 0=WFM, 1=NFM, 2=AM
    static constexpr auto paramIdWfMin = "wf_min";        // Waterfall Min dB
    static constexpr auto paramIdWfMax = "wf_max";        // Waterfall Max dB
    static constexpr auto paramIdDeEmphasis = "deemph";   // De-emphasis: 0=Off, 1=75µs, 2=50µs
    static constexpr auto paramIdAgcEnabled = "agc_on";   // AGC Enable: 0/1
    static constexpr auto paramIdAgcSpeed = "agc_speed";  // AGC Speed: 0=Fast, 1=Medium, 2=Slow
    
    // Parameters - Advanced Filtering
    static constexpr auto paramIdBandwidth = "bandwidth";       // kHz (1-200)
    static constexpr auto paramIdNotchEnabled = "notch_on";     // Notch filter enable
    static constexpr auto paramIdNotchFreq = "notch_freq";      // Notch frequency (Hz, 100-5000)
    static constexpr auto paramIdNbEnabled = "nb_on";           // Noise blanker enable
    static constexpr auto paramIdNbThreshold = "nb_thresh";     // Noise blanker threshold (0-1)
    
    // Parameters - Scanner/Auto-Tune
    static constexpr auto paramIdScanEnabled = "scan_on";       // Scanner enable
    static constexpr auto paramIdScanSpeed = "scan_speed";      // MHz/s (0.1-10)
    static constexpr auto paramIdDetectThreshold = "detect_db"; // dB above noise floor (3-20)
    static constexpr auto paramIdAfcEnabled = "afc_on";         // AFC enable
    static constexpr auto paramIdAfcRange = "afc_range";        // AFC range kHz (1-50)
    static constexpr auto paramIdScanMin = "scan_min";          // Scan range min MHz
    static constexpr auto paramIdScanMax = "scan_max";          // Scan range max MHz
    static constexpr auto paramIdHoldTime = "hold_time";        // Signal hold time (s)
    
    // Virtual modulation parameter IDs (for CV routing)
    static constexpr auto paramIdFreqMod = "freq_mod";
    static constexpr auto paramIdGainMod = "gain_mod";
    static constexpr auto paramIdSquelchMod = "squelch_mod";
    static constexpr auto paramIdBandwidthMod = "bandwidth_mod";
    
    // Live telemetry IDs (for UI feedback of modulated values)
    static constexpr auto paramIdFreqLive = "freq_live";
    static constexpr auto paramIdGainLive = "gain_live";
    static constexpr auto paramIdSquelchLive = "squelch_live";
    static constexpr auto paramIdBandwidthLive = "bandwidth_live";

    bool usesCustomPinLayout() const override { return true; }
    
    // CV modulation routing
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    // --- SDR Thread ---
    class SdrThread : public juce::Thread
    {
    public:
        SdrThread(SdrReceiverModule& owner);
        ~SdrThread() override;

        void run() override;
        void signalStop();

        void setFrequency(uint32_t freqHz);
        void setGain(int gainDb);
        void setSquelch(float thresholdDb);
        void setMode(int modeIndex);           // 0=WFM, 1=NFM, 2=AM
        void setDeEmphasis(int regionIndex);   // 0=Off, 1=75µs, 2=50µs
        void setAgcEnabled(bool enabled);
        void setAgcSpeed(int speedIndex);      // 0=Fast, 1=Medium, 2=Slow
        
        // Advanced filtering
        void setBandwidth(float bandwidthKhz);
        void setNotchEnabled(bool enabled);
        void setNotchFrequency(float freqHz);
        void setNoiseBlankerEnabled(bool enabled);
        void setNoiseBlankerThreshold(float threshold);

        bool isDeviceConnected() const { return deviceConnected.load(); }
        bool isSquelchOpen() const { return demodulator.isSquelchOpen(); }

    private:
        SdrReceiverModule& owner;
        rtlsdr_dev*        dev = nullptr;
        std::atomic<bool>  shouldStop{false};
        std::atomic<bool>  deviceConnected{false};

        // DSP Helper
        SdrDemodulator demodulator;

        // Cached settings to detect changes
        uint32_t currentFreq = 0;
        int      currentGain = -1;
        float    currentSquelch = -100.0f;
        int      currentMode = -1;
        int      currentDeEmphasis = -1;
        bool     currentAgcEnabled = true;
        int      currentAgcSpeed = -1;
        
        // Advanced filtering cached settings
        float    currentBandwidth = 150.0f;
        bool     currentNotchEnabled = false;
        float    currentNotchFreq = 1000.0f;
        bool     currentNbEnabled = false;
        float    currentNbThreshold = 0.5f;
    };

    std::unique_ptr<SdrThread> sdrThread;

    // --- Audio Buffering (SDR Thread -> Audio Thread) ---
    static constexpr int     audioFifoSize = 192000; // ~4 sec
    juce::AbstractFifo       audioFifo{audioFifoSize};
    juce::AudioBuffer<float> audioRingBuffer{2, audioFifoSize};

    // --- Visualization (SDR Thread -> GUI Thread) ---
    static constexpr int fftOrder = 10; // 1024 points
    static constexpr int fftSize = 1 << fftOrder;

    std::vector<float>              spectrumData; // Instantaneous power magnitude
    std::vector<std::vector<float>> waterfallHistory;
    std::mutex                      vizMutex;

    // FFT
    std::unique_ptr<juce::dsp::FFT>                      forwardFFT;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> windowing;
    std::vector<float>                                   fftData;     // Workspace
    std::vector<std::complex<float>>                     fftResponse; // Workspace

    // --- Parameters ---
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>*                freqParam = nullptr;
    std::atomic<float>*                gainParam = nullptr;
    std::atomic<float>*                squelchParam = nullptr;
    std::atomic<float>*                modeParam = nullptr;
    std::atomic<float>*                wfMinParam = nullptr;
    std::atomic<float>*                wfMaxParam = nullptr;
    std::atomic<float>*                deEmphasisParam = nullptr;
    std::atomic<float>*                agcEnabledParam = nullptr;
    std::atomic<float>*                agcSpeedParam = nullptr;
    
    // Advanced filtering parameters
    std::atomic<float>*                bandwidthParam = nullptr;
    std::atomic<float>*                notchEnabledParam = nullptr;
    std::atomic<float>*                notchFreqParam = nullptr;
    std::atomic<float>*                nbEnabledParam = nullptr;
    std::atomic<float>*                nbThresholdParam = nullptr;
    
    // Scanner parameters
    std::atomic<float>*                scanEnabledParam = nullptr;
    std::atomic<float>*                scanSpeedParam = nullptr;
    std::atomic<float>*                detectThresholdParam = nullptr;
    std::atomic<float>*                afcEnabledParam = nullptr;
    std::atomic<float>*                afcRangeParam = nullptr;
    std::atomic<float>*                scanMinParam = nullptr;
    std::atomic<float>*                scanMaxParam = nullptr;
    std::atomic<float>*                holdTimeParam = nullptr;

    // State
    double currentSampleRate = 48000.0;
    
    // --- Scanner/Auto-Tune State ---
    enum class ScannerState { Idle, Scanning, Locked, Holding };
    std::atomic<ScannerState> scannerState{ScannerState::Idle};
    std::atomic<float> currentNoiseFloor{-80.0f};
    std::atomic<float> currentSpectralFlatness{1.0f};
    std::atomic<float> currentSignalQuality{0.0f};
    std::atomic<float> detectedPeakFreq{0.0f};
    std::atomic<float> detectedPeakPower{-100.0f};
    std::chrono::steady_clock::time_point signalHoldStart;
    float scanDirection = 1.0f; // +1 = up, -1 = down

    // Helpers
    void pushAudioToFifo(const float* left, const float* right, int numSamples);
    void updateSpectrum(const std::vector<std::complex<float>>& iqData);
    
    // Signal analysis helpers
    float computeSpectralFlatness(const std::vector<float>& spectrum);
    float computeNoiseFloor(const std::vector<float>& spectrum);
    void findPeaks(const std::vector<float>& spectrum, float threshold,
                   std::vector<std::pair<int, float>>& peaks);
    void updateSignalAnalysis();
    void processScannerLogic(float deltaTime);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SdrReceiverModule)
};
