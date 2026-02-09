/*
  ==============================================================================

    SdrReceiverModule.cpp
    Created: 3 Feb 2026
    Updated: 5 Feb 2026 (Complete DSP Redesign)
    Author:  Antigravity

    Major improvements:
    - Multi-stage FIR decimation with anti-aliasing
    - DC blocking for RTL-SDR offset removal
    - De-emphasis filter (75µs US / 50µs EU)
    - AGC (Automatic Gain Control) with adjustable speed
    - Smooth squelch with attack/release envelope

  ==============================================================================
*/

#include "SdrReceiverModule.h"
#include <rtl-sdr.h>
#include <cmath>
/#include <chrono>
#include <cstdio>

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#endif

// ==============================================================================
// SdrThread Implementation
// ==============================================================================

SdrReceiverModule::SdrThread::SdrThread(SdrReceiverModule& o) : juce::Thread("SdrThread"), owner(o)
{
}

SdrReceiverModule::SdrThread::~SdrThread()
{
    signalStop();
    stopThread(2000);
}

void SdrReceiverModule::SdrThread::signalStop()
{
    shouldStop.store(true);
    if (dev)
        rtlsdr_cancel_async(dev); // Breaks out of read loop
}

void SdrReceiverModule::SdrThread::setFrequency(uint32_t freqHz)
{
    if (dev && deviceConnected && freqHz != currentFreq)
    {
        rtlsdr_set_center_freq(dev, freqHz);
        currentFreq = freqHz;
    }
}

void SdrReceiverModule::SdrThread::setGain(int gainDb)
{
    if (dev && deviceConnected && gainDb != currentGain)
    {
        // Clamp minimum to 1 dB (no auto-gain mode)
        int clampedGain = std::max(1, gainDb);

        // Always use manual gain mode for consistent behavior
        rtlsdr_set_tuner_gain_mode(dev, 1);
        rtlsdr_set_tuner_gain(dev, clampedGain * 10); // tenths of dB
        currentGain = clampedGain;
    }
}

void SdrReceiverModule::SdrThread::setSquelch(float thresholdDb)
{
    if (thresholdDb != currentSquelch)
    {
        demodulator.setSquelchThreshold(thresholdDb);
        currentSquelch = thresholdDb;
    }
}

void SdrReceiverModule::SdrThread::setMode(int modeIndex)
{
    if (modeIndex != currentMode)
    {
        demodulator.setMode(static_cast<SdrDemodulator::Mode>(modeIndex));
        currentMode = modeIndex;
    }
}

void SdrReceiverModule::SdrThread::setDeEmphasis(int regionIndex)
{
    if (regionIndex != currentDeEmphasis)
    {
        DeEmphasisFilter::Region region;
        switch (regionIndex)
        {
        case 0:
            region = DeEmphasisFilter::Region::Off;
            break;
        case 1:
            region = DeEmphasisFilter::Region::US_75us;
            break;
        case 2:
            region = DeEmphasisFilter::Region::EU_50us;
            break;
        default:
            region = DeEmphasisFilter::Region::US_75us;
        }
        demodulator.setDeEmphasisRegion(region);
        currentDeEmphasis = regionIndex;
    }
}

void SdrReceiverModule::SdrThread::setAgcEnabled(bool enabled)
{
    if (enabled != currentAgcEnabled)
    {
        demodulator.setAgcEnabled(enabled);
        currentAgcEnabled = enabled;
    }
}

void SdrReceiverModule::SdrThread::setAgcSpeed(int speedIndex)
{
    if (speedIndex != currentAgcSpeed)
    {
        AutomaticGainControl::Speed speed;
        switch (speedIndex)
        {
        case 0:
            speed = AutomaticGainControl::Speed::Fast;
            break;
        case 1:
            speed = AutomaticGainControl::Speed::Medium;
            break;
        case 2:
            speed = AutomaticGainControl::Speed::Slow;
            break;
        default:
            speed = AutomaticGainControl::Speed::Medium;
        }
        demodulator.setAgcSpeed(speed);
        currentAgcSpeed = speedIndex;
    }
}

void SdrReceiverModule::SdrThread::setBandwidth(float bandwidthKhz)
{
    if (std::abs(bandwidthKhz - currentBandwidth) > 0.1f)
    {
        demodulator.setBandwidth(bandwidthKhz);
        currentBandwidth = bandwidthKhz;
    }
}

void SdrReceiverModule::SdrThread::setNotchEnabled(bool enabled)
{
    if (enabled != currentNotchEnabled)
    {
        demodulator.setNotchEnabled(enabled);
        currentNotchEnabled = enabled;
    }
}

void SdrReceiverModule::SdrThread::setNotchFrequency(float freqHz)
{
    if (std::abs(freqHz - currentNotchFreq) > 1.0f)
    {
        demodulator.setNotchFrequency(freqHz);
        currentNotchFreq = freqHz;
    }
}

void SdrReceiverModule::SdrThread::setNoiseBlankerEnabled(bool enabled)
{
    if (enabled != currentNbEnabled)
    {
        demodulator.setNoiseBlankerEnabled(enabled);
        currentNbEnabled = enabled;
    }
}

void SdrReceiverModule::SdrThread::setNoiseBlankerThreshold(float threshold)
{
    if (std::abs(threshold - currentNbThreshold) > 0.01f)
    {
        demodulator.setNoiseBlankerThreshold(threshold);
        currentNbThreshold = threshold;
    }
}

void SdrReceiverModule::SdrThread::run()
{
    // 1. Open Device
    int deviceCount = rtlsdr_get_device_count();
    if (deviceCount == 0)
    {
        deviceConnected.store(false);
        return;
    }

    if (rtlsdr_open(&dev, 0) < 0)
    {
        deviceConnected.store(false);
        return;
    }

    deviceConnected.store(true);

    // 2. Configure
    // 2.4 MHz sample rate provides proper bandwidth for WFM (200kHz)
    // Decimation chain: 2.4MHz -> 480kHz -> 96kHz -> 48kHz (50x total)
    const uint32_t sampleRate = 2400000;
    rtlsdr_set_sample_rate(dev, sampleRate);
    rtlsdr_set_center_freq(dev, 100000000); // Start at 100MHz
    rtlsdr_set_tuner_gain_mode(dev, 0);     // Auto gain start
    rtlsdr_reset_buffer(dev);

    // Prepare Demodulator
    demodulator.prepare((double)sampleRate, 48000.0);
    // Initial sync
    demodulator.setMode(static_cast<SdrDemodulator::Mode>(currentMode == -1 ? 0 : currentMode));

    // 3. Read Loop - larger buffer for 2.4 MHz throughput
    const int            bufferLen = 32 * 1024;
    std::vector<uint8_t> buffer(bufferLen);
    int                  nRead = 0;

    // Buffers for processing
    std::vector<std::complex<float>> iqSamples;
    iqSamples.reserve(bufferLen / 2);

    std::vector<float> outL, outR;

    while (!shouldStop.load())
    {
        int r = rtlsdr_read_sync(dev, buffer.data(), bufferLen, &nRead);
        if (r < 0 || nRead == 0)
            break;

        // Convert u8 [0-255] to float [-1.0, 1.0] complex
        // I = even, Q = odd
        int numIQ = nRead / 2;
        iqSamples.clear();
        for (int i = 0; i < numIQ; ++i)
        {
            float I = (buffer[2 * i] - 127.5f) / 127.5f;
            float Q = (buffer[2 * i + 1] - 127.5f) / 127.5f;
            iqSamples.push_back({I, Q});
        }

        // DSP Processing
        demodulator.process(iqSamples, outL, outR);

        // Push Audio to FIFO
        if (!outL.empty())
        {
            owner.pushAudioToFifo(outL.data(), outR.data(), (int)outL.size());
        }

        // Push Spectrum Data
        owner.updateSpectrum(iqSamples);
    }

    rtlsdr_close(dev);
    dev = nullptr;
    deviceConnected.store(false);
}

// ==============================================================================
// SdrReceiverModule Implementation
// ==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout SdrReceiverModule::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdFreq, 1}, "Frequency", 24.0f, 1700.0f, 100.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdGain, 1}, "RF Gain", 1.0f, 50.0f, 20.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdSquelch, 1}, "Squelch", -100.0f, 0.0f, -60.0f));

    // Mode: 0=WFM, 1=NFM, 2=AM
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdMode, 1}, "Demod Mode", 0.0f, 2.0f, 0.0f));

    // Waterfall Scaling
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdWfMin, 1}, "WF Min dB", -120.0f, 0.0f, -80.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdWfMax, 1}, "WF Max dB", -60.0f, 20.0f, -20.0f));

    // De-emphasis: 0=Off, 1=75µs (US), 2=50µs (EU)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdDeEmphasis, 1}, "De-emphasis", 0.0f, 2.0f, 1.0f));

    // AGC Enable
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdAgcEnabled, 1}, "AGC Enabled", 0.0f, 1.0f, 1.0f));

    // AGC Speed: 0=Fast, 1=Medium, 2=Slow
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdAgcSpeed, 1}, "AGC Speed", 0.0f, 2.0f, 1.0f));

    // Advanced Filtering Parameters
    // Bandwidth (kHz): 1-200, default depends on mode (WFM=150, NFM=12, AM=6)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdBandwidth, 1}, "Bandwidth", 1.0f, 200.0f, 150.0f));

    // Notch Filter
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdNotchEnabled, 1}, "Notch Enabled", 0.0f, 1.0f, 0.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdNotchFreq, 1}, "Notch Freq", 100.0f, 5000.0f, 1000.0f));

    // Noise Blanker
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdNbEnabled, 1}, "NB Enabled", 0.0f, 1.0f, 0.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdNbThreshold, 1}, "NB Threshold", 0.0f, 1.0f, 0.5f));

    // Scanner/Auto-Tune Parameters
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdScanEnabled, 1}, "Scan Enabled", 0.0f, 1.0f, 0.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdScanSpeed, 1}, "Scan Speed", 0.1f, 10.0f, 1.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdDetectThreshold, 1}, "Detect Threshold", 3.0f, 20.0f, 6.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdAfcEnabled, 1}, "AFC Enabled", 0.0f, 1.0f, 0.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdAfcRange, 1}, "AFC Range", 1.0f, 50.0f, 10.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdScanMin, 1}, "Scan Min", 24.0f, 1700.0f, 88.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdScanMax, 1}, "Scan Max", 24.0f, 1700.0f, 108.0f));

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramIdHoldTime, 1}, "Hold Time", 0.5f, 10.0f, 2.0f));

    return {params.begin(), params.end()};
}

SdrReceiverModule::SdrReceiverModule()
    : ModuleProcessor(
          BusesProperties()
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)
              .withInput(
                  "CV",
                  juce::AudioChannelSet::discreteChannels(4),
                  true)), // 4 CV inputs: Freq, Gain, Squelch, Bandwidth
      apvts(*this, nullptr, "SDR_PARAMS", createParameterLayout())
{
    // Retrieve pointers
    freqParam = apvts.getRawParameterValue(paramIdFreq);
    gainParam = apvts.getRawParameterValue(paramIdGain);
    squelchParam = apvts.getRawParameterValue(paramIdSquelch);
    modeParam = apvts.getRawParameterValue(paramIdMode);
    wfMinParam = apvts.getRawParameterValue(paramIdWfMin);
    wfMaxParam = apvts.getRawParameterValue(paramIdWfMax);
    deEmphasisParam = apvts.getRawParameterValue(paramIdDeEmphasis);
    agcEnabledParam = apvts.getRawParameterValue(paramIdAgcEnabled);
    agcSpeedParam = apvts.getRawParameterValue(paramIdAgcSpeed);

    // Advanced filtering parameters
    bandwidthParam = apvts.getRawParameterValue(paramIdBandwidth);
    notchEnabledParam = apvts.getRawParameterValue(paramIdNotchEnabled);
    notchFreqParam = apvts.getRawParameterValue(paramIdNotchFreq);
    nbEnabledParam = apvts.getRawParameterValue(paramIdNbEnabled);
    nbThresholdParam = apvts.getRawParameterValue(paramIdNbThreshold);

    // Scanner parameters
    scanEnabledParam = apvts.getRawParameterValue(paramIdScanEnabled);
    scanSpeedParam = apvts.getRawParameterValue(paramIdScanSpeed);
    detectThresholdParam = apvts.getRawParameterValue(paramIdDetectThreshold);
    afcEnabledParam = apvts.getRawParameterValue(paramIdAfcEnabled);
    afcRangeParam = apvts.getRawParameterValue(paramIdAfcRange);
    scanMinParam = apvts.getRawParameterValue(paramIdScanMin);
    scanMaxParam = apvts.getRawParameterValue(paramIdScanMax);
    holdTimeParam = apvts.getRawParameterValue(paramIdHoldTime);

    sdrThread = std::make_unique<SdrThread>(*this);

    // Initialize FFT
    forwardFFT = std::make_unique<juce::dsp::FFT>(fftOrder);
    windowing = std::make_unique<juce::dsp::WindowingFunction<float>>(
        fftSize, juce::dsp::WindowingFunction<float>::hann);

    fftResponse.resize(fftSize);
    waterfallHistory.resize(60, std::vector<float>(fftSize, 0.0f)); // 60 rows history
}

SdrReceiverModule::~SdrReceiverModule()
{
    if (sdrThread)
    {
        sdrThread->signalStop(); // Flag stop
        sdrThread->waitForThreadToExit(2000);
    }
}

void SdrReceiverModule::releaseResources()
{
    audioRingBuffer.clear();
    audioFifo.reset();
}

bool SdrReceiverModule::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation inputs on the single input bus

    if (paramId == paramIdFreqMod)
    {
        outChannelIndexInBus = 0;
        return true;
    }
    if (paramId == paramIdGainMod)
    {
        outChannelIndexInBus = 1;
        return true;
    }
    if (paramId == paramIdSquelchMod)
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdBandwidthMod)
    {
        outChannelIndexInBus = 3;
        return true;
    }

    return false;
}

void SdrReceiverModule::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    audioRingBuffer.setSize(2, audioFifoSize);
    audioRingBuffer.clear();
    audioFifo.reset();

    if (!sdrThread->isThreadRunning())
        sdrThread->startThread();
}

void SdrReceiverModule::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    int totalSamples = buffer.getNumSamples();
    int numReady = audioFifo.getNumReady();
    int samplesToRead = std::min(totalSamples, numReady);

    // --- 1. Check CV connections ---
    const bool isFreqMod = isParamInputConnected(paramIdFreqMod);
    const bool isGainMod = isParamInputConnected(paramIdGainMod);
    const bool isSquelchMod = isParamInputConnected(paramIdSquelchMod);
    const bool isBandwidthMod = isParamInputConnected(paramIdBandwidthMod);

    // --- 2. Read CV Inputs BEFORE overwriting buffer ---
    // CV modulation ranges: Freq=±10MHz, Gain=±25dB, Squelch=±30dB, Bandwidth=±50kHz
    float     freqCv = 0.0f, gainCv = 0.0f, squelchCv = 0.0f, bandwidthCv = 0.0f;
    const int numInputChannels = buffer.getNumChannels();

    if (isFreqMod && numInputChannels > 0)
        freqCv = buffer.getReadPointer(0)[0] * 10.0f; // ±10 MHz
    if (isGainMod && numInputChannels > 1)
        gainCv = buffer.getReadPointer(1)[0] * 25.0f; // ±25 dB
    if (isSquelchMod && numInputChannels > 2)
        squelchCv = buffer.getReadPointer(2)[0] * 30.0f; // ±30 dB
    if (isBandwidthMod && numInputChannels > 3)
        bandwidthCv = buffer.getReadPointer(3)[0] * 50.0f; // ±50 kHz

    if (samplesToRead > 0)
    {
        int start1, size1, start2, size2;
        audioFifo.prepareToRead(samplesToRead, start1, size1, start2, size2);

        // Read L
        buffer.copyFrom(0, 0, audioRingBuffer, 0, start1, size1);
        if (size2 > 0)
            buffer.copyFrom(0, size1, audioRingBuffer, 0, start2, size2);

        // Read R
        if (buffer.getNumChannels() > 1)
        {
            buffer.copyFrom(1, 0, audioRingBuffer, 1, start1, size1);
            if (size2 > 0)
                buffer.copyFrom(1, size1, audioRingBuffer, 1, start2, size2);
        }

        audioFifo.finishedRead(samplesToRead);
    }

    // Clear remainder if underrun
    if (samplesToRead < totalSamples)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, samplesToRead, totalSamples - samplesToRead);
    }

    // Apply tuning updates with CV modulation
    if (sdrThread)
    {
        // Frequency with CV modulation (clamp to valid range)
        float    baseFreq = freqParam->load();
        float    modulatedFreq = juce::jlimit(24.0f, 1700.0f, baseFreq + freqCv);
        uint32_t freqHz = static_cast<uint32_t>(modulatedFreq * 1000000.0);
        sdrThread->setFrequency(freqHz);

        // Gain with CV modulation
        float baseGain = gainParam->load();
        float modulatedGain = juce::jlimit(0.0f, 50.0f, baseGain + gainCv);
        sdrThread->setGain((int)modulatedGain);

        // Squelch with CV modulation
        float baseSquelch = squelchParam->load();
        float modulatedSquelch = juce::jlimit(-100.0f, 0.0f, baseSquelch + squelchCv);
        sdrThread->setSquelch(modulatedSquelch);

        sdrThread->setMode((int)modeParam->load());

        // DSP parameters
        sdrThread->setDeEmphasis((int)deEmphasisParam->load());
        sdrThread->setAgcEnabled(agcEnabledParam->load() > 0.5f);
        sdrThread->setAgcSpeed((int)agcSpeedParam->load());

        // Advanced filtering with CV modulation on bandwidth
        float baseBandwidth = bandwidthParam->load();
        float modulatedBandwidth = juce::jlimit(1.0f, 200.0f, baseBandwidth + bandwidthCv);
        sdrThread->setBandwidth(modulatedBandwidth);

        sdrThread->setNotchEnabled(notchEnabledParam->load() > 0.5f);
        sdrThread->setNotchFrequency(notchFreqParam->load());
        sdrThread->setNoiseBlankerEnabled(nbEnabledParam->load() > 0.5f);
        sdrThread->setNoiseBlankerThreshold(nbThresholdParam->load());

        // --- 3. Update live telemetry for modulated parameters ---
        if (isFreqMod)
            setLiveParamValue(paramIdFreqLive, modulatedFreq);
        if (isGainMod)
            setLiveParamValue(paramIdGainLive, modulatedGain);
        if (isSquelchMod)
            setLiveParamValue(paramIdSquelchLive, modulatedSquelch);
        if (isBandwidthMod)
            setLiveParamValue(paramIdBandwidthLive, modulatedBandwidth);
    }
}

void SdrReceiverModule::pushAudioToFifo(const float* left, const float* right, int numSamples)
{
    int freeSpace = audioFifo.getFreeSpace();
    if (freeSpace < numSamples)
        return; // Overrun

    int start1, size1, start2, size2;
    audioFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

    audioRingBuffer.copyFrom(0, start1, left, size1);
    if (size2 > 0)
        audioRingBuffer.copyFrom(0, start2, left + size1, size2);

    audioRingBuffer.copyFrom(1, start1, right, size1);
    if (size2 > 0)
        audioRingBuffer.copyFrom(1, start2, right + size1, size2);

    audioFifo.finishedWrite(numSamples);
}

void SdrReceiverModule::updateSpectrum(const std::vector<std::complex<float>>& iqData)
{
    float wfMin = wfMinParam ? wfMinParam->load() : -80.0f;
    float wfMax = wfMaxParam ? wfMaxParam->load() : -20.0f;

    // Safety checks
    if (wfMax <= wfMin + 0.1f)
        wfMax = wfMin + 1.0f;
    float wfRange = wfMax - wfMin;
    float invRange = 1.0f / wfRange;

    std::lock_guard<std::mutex> lock(vizMutex);

    // 1. Prepare FFT Data
    int limit = std::min((int)iqData.size(), fftSize);

    if (fftData.size() != fftSize)
        fftData.resize(fftSize);

    std::fill(fftData.begin(), fftData.end(), 1.0f);
    windowing->multiplyWithWindowingTable(fftData.data(), fftSize);

    for (int i = 0; i < limit; ++i)
    {
        fftResponse[i] = iqData[i] * fftData[i];
    }
    for (int i = limit; i < fftSize; ++i)
        fftResponse[i] = {0, 0};

    forwardFFT->perform(fftResponse.data(), fftResponse.data(), false);

    if (spectrumData.size() != fftSize)
        spectrumData.resize(fftSize);

    // FFT normalization factor: divide by fftSize to get proper magnitude scaling
    const float fftNorm = 1.0f / (float)fftSize;

    // #region agent log - RAW FFT VALUES
    {
        static int fftLogCounter = 0;
        if (fftLogCounter++ % 60 == 0)
        {
            // Find min/max raw FFT magnitude
            float       rawMagMin = 1e9f, rawMagMax = -1e9f;
            float       dbMinRaw = 1e9f, dbMaxRaw = -1e9f;
            const float testRefOffset = -100.0f;
            for (int i = 0; i < fftSize; ++i)
            {
                float rawMag = std::abs(fftResponse[i]);
                float mag = rawMag * fftNorm;
                float db = juce::Decibels::gainToDecibels(mag + 1e-9f);
                if (rawMag < rawMagMin)
                    rawMagMin = rawMag;
                if (rawMag > rawMagMax)
                    rawMagMax = rawMag;
                if (db < dbMinRaw)
                    dbMinRaw = db;
                if (db > dbMaxRaw)
                    dbMaxRaw = db;
            }
            float dbMinWithOffset = dbMinRaw + testRefOffset;
            float dbMaxWithOffset = dbMaxRaw + testRefOffset;
            FILE* f = fopen("H:/0000_CODE/01_collider_pyo/.cursor/debug.log", "a");
            if (f)
            {
                fprintf(
                    f,
                    "{\"hypothesisId\":\"I\",\"location\":\"SdrReceiverModule.cpp:fftRaw\","
                    "\"message\":\"Raw FFT values with "
                    "offset\",\"data\":{\"rawMagMin\":%.6f,\"rawMagMax\":%.6f,\"dbMinRaw\":%.2f,"
                    "\"dbMaxRaw\":%.2f,\"dbMinWithOffset\":%.2f,\"dbMaxWithOffset\":%.2f,\"wfMin\":"
                    "%.2f,\"wfMax\":%.2f,\"limit\":%d},\"timestamp\":%lld}\n",
                    rawMagMin,
                    rawMagMax,
                    dbMinRaw,
                    dbMaxRaw,
                    dbMinWithOffset,
                    dbMaxWithOffset,
                    wfMin,
                    wfMax,
                    limit,
                    (long long)std::chrono::system_clock::now().time_since_epoch().count());
                fclose(f);
            }
        }
    }
    // #endregion

    // Shift FFT (DC to center)
    int half = fftSize / 2;

    // Reference level offset: RTL-SDR FFT magnitudes are very high due to
    // raw sample range and FFT accumulation. Apply a -100 dB offset to bring
    // values into the typical -80 to -20 dB display range.
    const float refLevelOffset = -100.0f;

    for (int i = 0; i < fftSize; ++i)
    {
        int   srcIdx = (i + half) % fftSize;
        float mag = std::abs(fftResponse[srcIdx]) * fftNorm;

        // Convert to dB with reference level offset
        float db = juce::Decibels::gainToDecibels(mag + 1e-9f) + refLevelOffset;

        // Normalize to display range
        float norm = (db - wfMin) * invRange;
        spectrumData[i] = juce::jlimit(0.0f, 1.0f, norm);
    }

    // Update Waterfall
    if (waterfallHistory.empty())
        return;

    waterfallHistory.insert(waterfallHistory.begin(), spectrumData);
    if (waterfallHistory.size() > 120) // Increased history depth
        waterfallHistory.pop_back();

    // Update signal analysis for scanner (after spectrum is computed)
    updateSignalAnalysis();
}

// =============================================================================
// Signal Analysis Functions for Auto-Tune/Scanner
// =============================================================================

float SdrReceiverModule::computeSpectralFlatness(const std::vector<float>& spectrum)
{
    // Spectral Flatness Measure (SFM) = geometric_mean / arithmetic_mean
    // Pure noise → SFM ≈ 1.0 (flat/white spectrum)
    // Signal present → SFM < 0.7 (peaks stand out)

    if (spectrum.empty())
        return 1.0f;

    const int n = (int)spectrum.size();
    double    sumLog = 0.0;
    double    sumLin = 0.0;
    int       validCount = 0;

    for (int i = 0; i < n; ++i)
    {
        float val = spectrum[i];
        if (val > 0.001f) // Avoid log(0)
        {
            sumLog += std::log(val);
            sumLin += val;
            validCount++;
        }
    }

    if (validCount == 0 || sumLin < 1e-9)
        return 1.0f;

    double geometricMean = std::exp(sumLog / validCount);
    double arithmeticMean = sumLin / validCount;

    return (float)juce::jlimit(0.0, 1.0, geometricMean / arithmeticMean);
}

float SdrReceiverModule::computeNoiseFloor(const std::vector<float>& spectrum)
{
    // Estimate noise floor using median of lower values
    // This is robust against peaks from signals

    if (spectrum.empty())
        return 0.0f;

    std::vector<float> sorted = spectrum;
    std::sort(sorted.begin(), sorted.end());

    // Use 25th percentile as noise floor estimate
    int idx = (int)(sorted.size() * 0.25f);
    return sorted[idx];
}

void SdrReceiverModule::findPeaks(
    const std::vector<float>&           spectrum,
    float                               threshold,
    std::vector<std::pair<int, float>>& peaks)
{
    peaks.clear();
    const int n = (int)spectrum.size();
    if (n < 3)
        return;

    // Find local maxima above threshold
    for (int i = 2; i < n - 2; ++i)
    {
        float val = spectrum[i];
        if (val > threshold && val > spectrum[i - 1] && val > spectrum[i + 1] &&
            val > spectrum[i - 2] && val > spectrum[i + 2])
        {
            peaks.push_back({i, val});
        }
    }

    // Sort by magnitude (strongest first)
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
}

void SdrReceiverModule::updateSignalAnalysis()
{
    // Called from updateSpectrum, already under vizMutex lock
    if (spectrumData.empty())
        return;

    // Compute spectral flatness (0-1, lower = more signal-like)
    float sfm = computeSpectralFlatness(spectrumData);
    currentSpectralFlatness.store(sfm);

    // Compute noise floor
    float noiseFloor = computeNoiseFloor(spectrumData);
    currentNoiseFloor.store(noiseFloor);

    // Find peaks above threshold
    float detectThresh = detectThresholdParam ? detectThresholdParam->load() : 6.0f;
    float threshold = noiseFloor + (detectThresh / 60.0f); // Convert dB threshold to normalized

    std::vector<std::pair<int, float>> peaks;
    findPeaks(spectrumData, threshold, peaks);

    // Track strongest peak
    if (!peaks.empty())
    {
        int   peakBin = peaks[0].first;
        float peakPower = peaks[0].second;

        // Convert bin to frequency offset from center
        // Spectrum is centered (DC in middle), bin 0 = -Fs/2, bin N/2 = DC, bin N-1 = +Fs/2
        float binOffset = (float)(peakBin - fftSize / 2) / (float)fftSize;
        float freqOffsetMHz = binOffset * 0.96f; // 960 kHz sample rate = 0.96 MHz

        float centerFreq = freqParam ? freqParam->load() : 100.0f;
        detectedPeakFreq.store(centerFreq + freqOffsetMHz);
        detectedPeakPower.store(peakPower);

        // Signal quality = (peak - noise) * (1 - flatness)
        // Higher is better, combines SNR with tonality
        float snrNorm = (peakPower - noiseFloor) * 2.0f; // Scale up
        float tonality = 1.0f - sfm;
        currentSignalQuality.store(juce::jlimit(0.0f, 1.0f, snrNorm * tonality));
    }
    else
    {
        detectedPeakPower.store(0.0f);
        currentSignalQuality.store(0.0f);
    }
}

void SdrReceiverModule::processScannerLogic(float deltaTime)
{
    if (!scanEnabledParam || scanEnabledParam->load() < 0.5f)
    {
        scannerState.store(ScannerState::Idle);
        return;
    }

    float scanSpeed = scanSpeedParam ? scanSpeedParam->load() : 1.0f;
    float scanMin = scanMinParam ? scanMinParam->load() : 88.0f;
    float scanMax = scanMaxParam ? scanMaxParam->load() : 108.0f;
    float holdTime = holdTimeParam ? holdTimeParam->load() : 2.0f;
    float detectThresh = detectThresholdParam ? detectThresholdParam->load() : 6.0f;

    float currentFreq = freqParam ? freqParam->load() : 100.0f;
    float signalQuality = currentSignalQuality.load();
    float sfm = currentSpectralFlatness.load();

    ScannerState state = scannerState.load();

    switch (state)
    {
    case ScannerState::Idle:
        // Start scanning
        scannerState.store(ScannerState::Scanning);
        break;

    case ScannerState::Scanning:
    {
        // Check for signal detection
        // Criteria: spectral flatness < 0.7 AND signal quality > threshold
        bool signalDetected = (sfm < 0.7f) && (signalQuality > 0.3f);

        if (signalDetected)
        {
            // Lock onto this frequency
            float peakFreq = detectedPeakFreq.load();
            if (peakFreq > 24.0f && peakFreq < 1700.0f)
            {
                *freqParam = peakFreq;
            }
            signalHoldStart = std::chrono::steady_clock::now();
            scannerState.store(ScannerState::Locked);
        }
        else
        {
            // Continue scanning
            float newFreq = currentFreq + (scanDirection * scanSpeed * deltaTime);

            // Wrap around at boundaries
            if (newFreq > scanMax)
            {
                newFreq = scanMin;
            }
            else if (newFreq < scanMin)
            {
                newFreq = scanMax;
            }

            *freqParam = newFreq;
        }
        break;
    }

    case ScannerState::Locked:
    {
        // Check if signal is still present
        bool signalStillPresent = (sfm < 0.75f) && (signalQuality > 0.2f);

        if (!signalStillPresent)
        {
            // Signal lost, start hold timer
            scannerState.store(ScannerState::Holding);
            signalHoldStart = std::chrono::steady_clock::now();
        }
        else
        {
            // Apply AFC if enabled
            if (afcEnabledParam && afcEnabledParam->load() > 0.5f)
            {
                float afcRange = afcRangeParam ? afcRangeParam->load() : 10.0f;
                float peakFreq = detectedPeakFreq.load();
                float freqError = peakFreq - currentFreq;

                // Only correct if within AFC range
                if (std::abs(freqError) < (afcRange / 1000.0f) && std::abs(freqError) > 0.001f)
                {
                    // Gentle AFC correction (20% of error per update)
                    *freqParam = currentFreq + freqError * 0.2f;
                }
            }
        }
        break;
    }

    case ScannerState::Holding:
    {
        // Wait for hold time before resuming scan
        auto elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - signalHoldStart)
                .count();

        if (elapsed >= holdTime)
        {
            // Resume scanning
            scannerState.store(ScannerState::Scanning);
        }
        else
        {
            // Check if signal returned
            bool signalReturned = (sfm < 0.7f) && (signalQuality > 0.3f);
            if (signalReturned)
            {
                scannerState.store(ScannerState::Locked);
            }
        }
        break;
    }
    }
}

#if defined(PRESET_CREATOR_UI)

ImVec2 SdrReceiverModule::getCustomNodeSize() const
{
    // #region agent log
    {
        static int sizeLogCounter = 0;
        if (sizeLogCounter++ % 120 == 0)
        {
            FILE* f = fopen("H:/0000_CODE/01_collider_pyo/.cursor/debug.log", "a");
            if (f)
            {
                fprintf(
                    f,
                    "{\"hypothesisId\":\"G\",\"location\":\"SdrReceiverModule.cpp:"
                    "getCustomNodeSize\",\"message\":\"getCustomNodeSize "
                    "called\",\"data\":{\"returnWidth\":450.0,\"returnHeight\":0.0},\"timestamp\":%"
                    "lld}\n",
                    (long long)std::chrono::system_clock::now().time_since_epoch().count());
                fclose(f);
            }
        }
    }
    // #endregion
    // Wide node for SDR controls - auto height (0) lets ImNodes calculate
    // Content breakdown:
    // - Header & frequency display
    // - Tuning buttons
    // - Spectrum (80px) + Waterfall (160px)
    // - RF Gain + Squelch sliders
    // - Mode + De-emphasis combos
    // - AGC controls row
    // - Waterfall range controls
    return ImVec2(450.0f, 0.0f); // Wide canvas, auto-height
}

void SdrReceiverModule::drawParametersInNode(
    float                                           itemWidth,
    const std::function<bool(const juce::String&)>& isParamModulated,
    const std::function<void()>&                    onModificationEnded,
    const NodePinHelpers*                           pinHelpers)
{
    // Use proper child window pattern - NO WorkRect hacking!
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    // 1. Header: Frequency Display & Status with modulation support
    const bool freqIsMod = isParamModulated(paramIdFreqMod);
    float      freq = freqIsMod
                          ? getLiveParamValueFor(paramIdFreqMod, paramIdFreqLive, freqParam->load())
                          : freqParam->load();

    // Status indicator
    if (sdrThread && sdrThread->isDeviceConnected())
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "[ON]");
    else
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[OFF]");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            sdrThread && sdrThread->isDeviceConnected()
                ? "RTL-SDR device connected and receiving."
                : "No RTL-SDR device detected.\nConnect a compatible USB dongle.");

    ImGui::SameLine();

    // Inline input pin for Freq CV (Channel 0)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(0)) // Channel 0 = Freq CV
            ImGui::SameLine();
    }

    // Large frequency display with infinite drag tuning
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    ImGui::PushItemWidth(itemWidth - 70.0f);

    if (freqIsMod)
        ImGui::BeginDisabled();

    // Dynamic drag speed: faster at higher frequencies, with modifier keys
    float dragSpeed = freq * 0.0005f; // 0.05% per pixel base
    if (ImGui::GetIO().KeyShift)
        dragSpeed *= 0.1f; // Fine tune with Shift
    if (ImGui::GetIO().KeyCtrl)
        dragSpeed *= 10.0f;                    // Coarse tune with Ctrl
    dragSpeed = juce::jmax(0.001f, dragSpeed); // Minimum step

    if (ImGui::DragFloat("##FreqTune", &freq, dragSpeed, 24.0f, 1700.0f, "%.4f MHz"))
    {
        if (!freqIsMod)
            *freqParam = juce::jlimit(24.0f, 1700.0f, freq);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !freqIsMod)
        onModificationEnded();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Drag to tune frequency (24-1700 MHz)\n"
            "Shift: Fine tune (0.1x)\n"
            "Ctrl: Coarse tune (10x)\n"
            "Scroll wheel also works!");
        if (!freqIsMod)
        {
            // Scroll wheel tuning with same speed scaling
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                float scrollStep = freq * 0.005f; // 0.5% per scroll tick
                if (ImGui::GetIO().KeyShift)
                    scrollStep *= 0.1f;
                if (ImGui::GetIO().KeyCtrl)
                    scrollStep *= 10.0f;
                scrollStep = juce::jmax(0.01f, scrollStep);

                float newFreq = juce::jlimit(24.0f, 1700.0f, freq + wheel * scrollStep);
                *freqParam = newFreq;
                onModificationEnded();
            }
        }
    }
    if (freqIsMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(mod)");
    }

    ImGui::PopItemWidth();
    ImGui::PopFont();

    // Quick tune buttons row (kept for convenience)
    if (ImGui::Button("-1M"))
    {
        *freqParam = juce::jlimit(24.0f, 1700.0f, freq - 1.0f);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("-1 MHz");
    ImGui::SameLine();
    if (ImGui::Button("-.1"))
    {
        *freqParam = juce::jlimit(24.0f, 1700.0f, freq - 0.1f);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("-0.1 MHz");
    ImGui::SameLine();
    if (ImGui::Button("+.1"))
    {
        *freqParam = juce::jlimit(24.0f, 1700.0f, freq + 0.1f);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("+0.1 MHz");
    ImGui::SameLine();
    if (ImGui::Button("+1M"))
    {
        *freqParam = juce::jlimit(24.0f, 1700.0f, freq + 1.0f);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("+1 MHz");

    ImGui::Separator();

    // 2. Visualizations - Using proper child window pattern per NodeVisualizationGuide.md
    // Copy data BEFORE BeginChild (per ImGuiChildWindowVisualizationFix.md)
    std::vector<float>              localSpectrum;
    std::vector<std::vector<float>> localWaterfall;
    {
        std::lock_guard<std::mutex> lock(vizMutex);
        if (!spectrumData.empty())
        {
            localSpectrum = spectrumData;
            localWaterfall = waterfallHistory;
        }
    }

    // #region agent log - DISPLAY DATA RANGE
    {
        static int dispLogCounter = 0;
        if (!localSpectrum.empty() && dispLogCounter++ % 60 == 0)
        {
            float specMin = 1e9f, specMax = -1e9f;
            for (float v : localSpectrum)
            {
                if (v < specMin)
                    specMin = v;
                if (v > specMax)
                    specMax = v;
            }
            FILE* f = fopen("H:/0000_CODE/01_collider_pyo/.cursor/debug.log", "a");
            if (f)
            {
                fprintf(
                    f,
                    "{\"hypothesisId\":\"H\",\"location\":\"SdrReceiverModule.cpp:displayData\","
                    "\"message\":\"Display data range (0-1 "
                    "expected)\",\"data\":{\"spectrumSize\":%d,\"specMin\":%.4f,\"specMax\":%.4f,"
                    "\"wfRows\":%d},\"timestamp\":%lld}\n",
                    (int)localSpectrum.size(),
                    specMin,
                    specMax,
                    (int)localWaterfall.size(),
                    (long long)std::chrono::system_clock::now().time_since_epoch().count());
                fclose(f);
            }
        }
    }
    // #endregion

    // A. Spectrum display using PlotLines (limited width via child window)
    const float  spectrumHeight = 80.0f;
    const float  waterfallHeight = 160.0f;
    const ImVec2 spectrumSize(itemWidth, spectrumHeight);
    const ImVec2 waterfallSize(itemWidth, waterfallHeight);

    if (!localSpectrum.empty())
    {
        // Spectrum child window
        if (ImGui::BeginChild(
                "SdrSpectrum",
                spectrumSize,
                false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImDrawList*  drawList = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            const ImVec2 p1 = ImVec2(p0.x + spectrumSize.x, p0.y + spectrumSize.y);

            // Background
            drawList->AddRectFilled(p0, p1, IM_COL32(20, 20, 30, 255));
            drawList->PushClipRect(p0, p1, true);

            // --- Bandwidth Filter Indicator Overlay ---
            // RTL-SDR samples at 960kHz, so the spectrum spans ±480kHz around center
            // Bandwidth filter is in kHz, show as shaded region
            const float bwKhz = bandwidthParam->load();
            const float sampleRateKhz = 960.0f;               // SDR sample rate in kHz
            const float bwNormalized = bwKhz / sampleRateKhz; // Fraction of spectrum
            const float centerX = p0.x + spectrumSize.x * 0.5f;
            const float bwHalfWidth = (bwNormalized * spectrumSize.x) * 0.5f;

            // Draw bandwidth region (semi-transparent yellow)
            drawList->AddRectFilled(
                ImVec2(centerX - bwHalfWidth, p0.y),
                ImVec2(centerX + bwHalfWidth, p1.y),
                IM_COL32(255, 255, 0, 30)); // Light yellow tint

            // Draw bandwidth edges (yellow lines)
            drawList->AddLine(
                ImVec2(centerX - bwHalfWidth, p0.y),
                ImVec2(centerX - bwHalfWidth, p1.y),
                IM_COL32(255, 255, 0, 180),
                1.0f);
            drawList->AddLine(
                ImVec2(centerX + bwHalfWidth, p0.y),
                ImVec2(centerX + bwHalfWidth, p1.y),
                IM_COL32(255, 255, 0, 180),
                1.0f);

            // Draw spectrum line
            const int numPoints = (int)localSpectrum.size();
            if (numPoints > 1)
            {
                const float xStep = spectrumSize.x / (float)(numPoints - 1);
                for (int i = 0; i < numPoints - 1; ++i)
                {
                    float y0 = p1.y - localSpectrum[i] * spectrumSize.y;
                    float y1 = p1.y - localSpectrum[i + 1] * spectrumSize.y;
                    y0 = juce::jlimit(p0.y, p1.y, y0);
                    y1 = juce::jlimit(p0.y, p1.y, y1);
                    drawList->AddLine(
                        ImVec2(p0.x + i * xStep, y0),
                        ImVec2(p0.x + (i + 1) * xStep, y1),
                        IM_COL32(0, 255, 0, 255),
                        1.0f);
                }
            }

            drawList->PopClipRect();

            // Interactive tuning in spectrum view
            ImGui::SetCursorPos(ImVec2(0, 0));
            if (ImGui::InvisibleButton("##spectrumTune", spectrumSize))
            {
                // Click detected - tune to clicked frequency
                ImVec2      mousePos = ImGui::GetIO().MouseClickedPos[0];
                float       relX = (mousePos.x - p0.x) / spectrumSize.x;
                float       offsetNorm = relX - 0.5f;
                const float sampleRateKhz = 960.0f;
                float       offsetMhz = offsetNorm * sampleRateKhz * 0.001f;
                float       newFreq = freqParam->load() + offsetMhz;
                *freqParam = juce::jlimit(24.0f, 1700.0f, newFreq);
                onModificationEnded();
            }

            // Hover: show frequency tooltip
            if (ImGui::IsItemHovered())
            {
                ImVec2      mousePos = ImGui::GetIO().MousePos;
                float       relX = (mousePos.x - p0.x) / spectrumSize.x;
                float       offsetNorm = relX - 0.5f;
                const float sampleRateKhz = 960.0f;
                float       hoverFreqMhz = freqParam->load() + offsetNorm * sampleRateKhz * 0.001f;
                float       offsetKhz = offsetNorm * sampleRateKhz;
                ImGui::SetTooltip("%.4f MHz\n(%.1f kHz)\nClick to tune", hoverFreqMhz, offsetKhz);
            }
        }
        ImGui::EndChild(); // CRITICAL: Must be OUTSIDE the if block!

        // B. Waterfall child window
        if (ImGui::BeginChild(
                "SdrWaterfall",
                waterfallSize,
                false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImDrawList*  drawList = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            const ImVec2 p1 = ImVec2(p0.x + waterfallSize.x, p0.y + waterfallSize.y);

            // Background
            drawList->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
            drawList->PushClipRect(p0, p1, true);

            const int   rows = (int)localWaterfall.size();
            const float cellH = waterfallSize.y / (float)std::max(1, rows);
            const int   vizBins = 256;
            const float cellW = waterfallSize.x / (float)vizBins;
            const int   block = fftSize / vizBins;

            for (int r = 0; r < rows; ++r)
            {
                const auto& rowData = localWaterfall[r];
                const int   rowSize = (int)rowData.size();
                if (rowSize == 0)
                    continue;

                for (int c = 0; c < vizBins; ++c)
                {
                    // Average pool
                    float sum = 0;
                    int   count = 0;
                    for (int k = 0; k < block && (c * block + k) < rowSize; ++k)
                    {
                        sum += rowData[c * block + k];
                        count++;
                    }
                    float val = (count > 0) ? (sum / count) : 0.0f;

                    // Professional fire colormap: black → blue → cyan → green → yellow → red →
                    // white
                    float rV = 0, gV = 0, bV = 0;
                    val = juce::jlimit(0.0f, 1.0f, val);

                    if (val < 0.15f)
                    {
                        // Black to deep blue
                        float t = val / 0.15f;
                        rV = 0;
                        gV = 0;
                        bV = t * 0.6f;
                    }
                    else if (val < 0.30f)
                    {
                        // Deep blue to cyan
                        float t = (val - 0.15f) / 0.15f;
                        rV = 0;
                        gV = t * 0.9f;
                        bV = 0.6f + t * 0.4f;
                    }
                    else if (val < 0.45f)
                    {
                        // Cyan to green
                        float t = (val - 0.30f) / 0.15f;
                        rV = 0;
                        gV = 0.9f + t * 0.1f;
                        bV = 1.0f - t;
                    }
                    else if (val < 0.60f)
                    {
                        // Green to yellow
                        float t = (val - 0.45f) / 0.15f;
                        rV = t;
                        gV = 1.0f;
                        bV = 0;
                    }
                    else if (val < 0.80f)
                    {
                        // Yellow to red
                        float t = (val - 0.60f) / 0.20f;
                        rV = 1.0f;
                        gV = 1.0f - t * 0.9f;
                        bV = 0;
                    }
                    else
                    {
                        // Red to white (hot spots)
                        float t = (val - 0.80f) / 0.20f;
                        rV = 1.0f;
                        gV = 0.1f + t * 0.9f;
                        bV = t * 0.8f;
                    }

                    ImU32 col = IM_COL32((int)(rV * 255), (int)(gV * 255), (int)(bV * 255), 255);
                    drawList->AddRectFilled(
                        ImVec2(p0.x + c * cellW, p0.y + r * cellH),
                        ImVec2(p0.x + (c + 1) * cellW, p0.y + (r + 1) * cellH),
                        col);
                }
            }

            // --- Bandwidth Filter Indicator Overlay (on top of waterfall) ---
            const float bwKhz = bandwidthParam->load();
            const float sampleRateKhz = 960.0f;
            const float bwNormalized = bwKhz / sampleRateKhz;
            const float centerX = p0.x + waterfallSize.x * 0.5f;
            const float bwHalfWidth = (bwNormalized * waterfallSize.x) * 0.5f;

            // Draw bandwidth region (semi-transparent)
            drawList->AddRectFilled(
                ImVec2(centerX - bwHalfWidth, p0.y),
                ImVec2(centerX + bwHalfWidth, p1.y),
                IM_COL32(255, 255, 100, 20));

            // Draw bandwidth edges (yellow vertical lines)
            drawList->AddLine(
                ImVec2(centerX - bwHalfWidth, p0.y),
                ImVec2(centerX - bwHalfWidth, p1.y),
                IM_COL32(255, 255, 0, 200),
                1.5f);
            drawList->AddLine(
                ImVec2(centerX + bwHalfWidth, p0.y),
                ImVec2(centerX + bwHalfWidth, p1.y),
                IM_COL32(255, 255, 0, 200),
                1.5f);

            // Draw center frequency line (red)
            drawList->AddLine(
                ImVec2(centerX, p0.y), ImVec2(centerX, p1.y), IM_COL32(255, 50, 50, 220), 2.0f);

            drawList->PopClipRect();

            // Interactive tuning - click/drag to tune frequency
            ImGui::SetCursorPos(ImVec2(0, 0));
            if (ImGui::InvisibleButton("##waterfallTune", waterfallSize))
            {
                // Click detected - tune to clicked frequency
                ImVec2 mousePos = ImGui::GetIO().MouseClickedPos[0];
                float  relX = (mousePos.x - p0.x) / waterfallSize.x;    // 0-1
                float  offsetNorm = relX - 0.5f;                        // -0.5 to +0.5
                float  offsetMhz = offsetNorm * sampleRateKhz * 0.001f; // Convert to MHz
                float  newFreq = freqParam->load() + offsetMhz;
                *freqParam = juce::jlimit(24.0f, 1700.0f, newFreq);
                onModificationEnded();
            }

            // Drag-to-scroll frequency
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                if (std::abs(delta.x) > 1.0f)
                {
                    // Convert pixel drag to frequency offset
                    float freqDelta = -(delta.x / waterfallSize.x) * sampleRateKhz * 0.001f;
                    float newFreq =
                        freqParam->load() + freqDelta * 0.3f; // Dampened for smooth scrolling
                    *freqParam = juce::jlimit(24.0f, 1700.0f, newFreq);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                }
            }

            // Hover: show frequency tooltip + scroll wheel bandwidth zoom
            if (ImGui::IsItemHovered())
            {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                float  relX = (mousePos.x - p0.x) / waterfallSize.x;
                float  offsetNorm = relX - 0.5f;
                float  hoverFreqMhz = freqParam->load() + offsetNorm * sampleRateKhz * 0.001f;
                float  offsetKhz = offsetNorm * sampleRateKhz;

                ImGui::SetTooltip(
                    "%.4f MHz\n(%.1f kHz from center)\n\nClick to tune\nDrag to scroll\nScroll "
                    "wheel = bandwidth",
                    hoverFreqMhz,
                    offsetKhz);

                // Scroll wheel: adjust bandwidth
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f)
                {
                    float newBw = bandwidthParam->load() + wheel * 10.0f;
                    *bandwidthParam = juce::jlimit(10.0f, 200.0f, newBw);
                    onModificationEnded();
                }
            }
        }
        ImGui::EndChild(); // CRITICAL: Must be OUTSIDE the if block!
    }
    else
    {
        // Placeholder when no data
        ImGui::Dummy(ImVec2(itemWidth, spectrumHeight + waterfallHeight));
    }

    ImGui::Separator();

    // 3. Controls Layout - Use fixed column widths to stay within node bounds
    const float halfWidth = itemWidth * 0.48f;
    ImGui::Columns(2, "sdr_controls", false);
    ImGui::SetColumnWidth(0, halfWidth);
    ImGui::SetColumnWidth(1, halfWidth);

    // -- Column 1 --
    ImGui::PushItemWidth(halfWidth - 85.0f); // Leave room for label + inline pin

    // RF Gain with modulation support + inline input pin
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1)) // Channel 1 = Gain CV
            ImGui::SameLine();
    }
    const bool gainIsMod = isParamModulated(paramIdGainMod);
    float g = gainIsMod ? getLiveParamValueFor(paramIdGainMod, paramIdGainLive, gainParam->load())
                        : gainParam->load();

    if (gainIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("RF Gain", &g, 0.0f, 50.0f, "%.0f dB"))
    {
        if (!gainIsMod)
            *gainParam = g;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !gainIsMod)
        onModificationEnded();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Hardware tuner gain.\nHigher = boost weak signals.\nToo high may overload on strong "
            "signals.");
    if (!gainIsMod)
        adjustParamOnWheel(apvts.getParameter(paramIdGain), paramIdGain, g);
    if (gainIsMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(mod)");
    }

    // Squelch with modulation support + inline input pin
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2)) // Channel 2 = Squelch CV
            ImGui::SameLine();
    }
    const bool squelchIsMod = isParamModulated(paramIdSquelchMod);
    float      s =
        squelchIsMod
                 ? getLiveParamValueFor(paramIdSquelchMod, paramIdSquelchLive, squelchParam->load())
                 : squelchParam->load();

    if (squelchIsMod)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Squelch", &s, -100.f, 0.0f, "%.0f dB"))
    {
        if (!squelchIsMod)
            *squelchParam = s;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !squelchIsMod)
        onModificationEnded();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Mute audio below this signal level.\nLower = hear weaker signals.\nHigher = reduce "
            "noise.\n(Scroll wheel to adjust)");
        if (!squelchIsMod)
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const float step = 2.0f;
                float       newVal = juce::jlimit(-100.0f, 0.0f, s + wheel * step);
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                        apvts.getParameter(paramIdSquelch)))
                {
                    *p = newVal;
                    onModificationEnded();
                }
            }
        }
    }
    if (squelchIsMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(mod)");
    }

    ImGui::PopItemWidth();
    ImGui::NextColumn();

    // -- Column 2 --
    ImGui::PushItemWidth(halfWidth - 60.0f);

    int         m = (int)modeParam->load();
    const char* modes[] = {"WFM", "NFM", "AM"}; // Shortened labels
    if (ImGui::Combo("Mode", &m, modes, 3))
    {
        *modeParam = (float)m;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Demodulation mode:\nWFM: Wide FM for broadcast (88-108 MHz)\nNFM: Narrow FM for "
            "comms/ham\nAM: Aviation, shortwave");
        // Manual scroll handling for combo
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            int newMode = juce::jlimit(0, 2, m + (wheel > 0.0f ? -1 : 1));
            if (newMode != m)
            {
                *modeParam = (float)newMode;
                onModificationEnded();
            }
        }
    }

    // De-emphasis (only useful for WFM mode)
    int         deemph = (int)deEmphasisParam->load();
    const char* deEmphModes[] = {"Off", "75us", "50us"}; // Shortened labels
    if (ImGui::Combo("De-emph", &deemph, deEmphModes, 3))
    {
        *deEmphasisParam = (float)deemph;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "De-emphasis filter for WFM:\n75us = US stations\n50us = European stations\nReduces "
            "high-frequency hiss.");
        // Manual scroll handling for combo
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            int newDeemph = juce::jlimit(0, 2, deemph + (wheel > 0.0f ? -1 : 1));
            if (newDeemph != deemph)
            {
                *deEmphasisParam = (float)newDeemph;
                onModificationEnded();
            }
        }
    }

    ImGui::PopItemWidth();
    ImGui::Columns(1);

    ImGui::Separator();

    // 4. DSP Controls Row - Single row with SameLine
    bool agcOn = agcEnabledParam->load() > 0.5f;
    if (ImGui::Checkbox("AGC", &agcOn))
    {
        *agcEnabledParam = agcOn ? 1.0f : 0.0f;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Automatic Gain Control\nNormalizes audio level variations\nfrom signal fading.");

    ImGui::SameLine();

    // AGC Speed (only if AGC is enabled)
    if (agcOn)
    {
        int         agcSpd = (int)agcSpeedParam->load();
        const char* agcSpeeds[] = {"Fast", "Med", "Slow"};
        ImGui::PushItemWidth(65);
        if (ImGui::Combo("##AGCSpeed", &agcSpd, agcSpeeds, 3))
        {
            *agcSpeedParam = (float)agcSpd;
            onModificationEnded();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "AGC response speed:\nFast: Quick adaptation (speech)\nMed: Balanced\nSlow: Gentle "
                "(music)");
            // Manual scroll handling for combo
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                int newSpd = juce::jlimit(0, 2, agcSpd + (wheel > 0.0f ? -1 : 1));
                if (newSpd != agcSpd)
                {
                    *agcSpeedParam = (float)newSpd;
                    onModificationEnded();
                }
            }
        }
        ImGui::PopItemWidth();
    }
    else
    {
        ImGui::TextDisabled("(off)");
    }

    ImGui::SameLine();

    // Squelch indicator
    if (sdrThread && sdrThread->isDeviceConnected())
    {
        if (sdrThread->isSquelchOpen())
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "SQ:Open");
        else
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "SQ:Mute");
    }

    ImGui::Separator();

    // ========== 5. Advanced Filtering Section ==========
    ImGui::Text("Filtering");

    // Bandwidth Control with modulation support + inline input pin
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3)) // Channel 3 = BW CV
            ImGui::SameLine();
    }
    const bool bwIsMod = isParamModulated(paramIdBandwidthMod);
    float      bw = bwIsMod ? getLiveParamValueFor(
                             paramIdBandwidthMod, paramIdBandwidthLive, bandwidthParam->load())
                            : bandwidthParam->load();

    if (bwIsMod)
        ImGui::BeginDisabled();
    ImGui::PushItemWidth(70);
    if (ImGui::DragFloat("##BW", &bw, 1.0f, 1.0f, 200.0f, "%.0f kHz"))
    {
        if (!bwIsMod)
            *bandwidthParam = juce::jlimit(1.0f, 200.0f, bw);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !bwIsMod)
        onModificationEnded();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Signal bandwidth filter (kHz)\n"
            "WFM: ~150 kHz\n"
            "NFM: ~12 kHz\n"
            "AM: ~6 kHz\n"
            "Narrower = less noise, wider = more signal");
        if (!bwIsMod)
            adjustParamOnWheel(apvts.getParameter(paramIdBandwidth), paramIdBandwidth, bw);
    }
    ImGui::PopItemWidth();
    if (bwIsMod)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (bwIsMod)
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "BW(mod)");
    else
        ImGui::Text("BW");

    ImGui::SameLine();

    // Notch Filter Enable
    bool notchOn = notchEnabledParam->load() > 0.5f;
    if (ImGui::Checkbox("##NotchEn", &notchOn))
    {
        *notchEnabledParam = notchOn ? 1.0f : 0.0f;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Enable notch filter\nRemoves a specific interfering frequency\n(e.g., heterodyne "
            "whistle, powerline hum)");

    ImGui::SameLine();

    // Notch Frequency (only if enabled)
    if (notchOn)
    {
        float notchF = notchFreqParam->load();
        ImGui::PushItemWidth(60);
        if (ImGui::DragFloat("##NotchF", &notchF, 10.0f, 100.0f, 5000.0f, "%.0f"))
        {
            *notchFreqParam = juce::jlimit(100.0f, 5000.0f, notchF);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Notch frequency (Hz)\nSet to frequency of interference");
            adjustParamOnWheel(apvts.getParameter(paramIdNotchFreq), paramIdNotchFreq, notchF);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Text("Notch");
    }
    else
    {
        ImGui::TextDisabled("Notch");
    }

    ImGui::SameLine();

    // Noise Blanker Enable
    bool nbOn = nbEnabledParam->load() > 0.5f;
    if (ImGui::Checkbox("##NBEn", &nbOn))
    {
        *nbEnabledParam = nbOn ? 1.0f : 0.0f;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Enable noise blanker\nReduces impulse noise (clicks, pops)\nfrom electrical "
            "interference");

    ImGui::SameLine();

    // Noise Blanker Threshold (only if enabled)
    if (nbOn)
    {
        float nbT = nbThresholdParam->load();
        ImGui::PushItemWidth(50);
        if (ImGui::DragFloat("##NBT", &nbT, 0.01f, 0.0f, 1.0f, "%.2f"))
        {
            *nbThresholdParam = juce::jlimit(0.0f, 1.0f, nbT);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Noise blanker threshold\n0 = aggressive (may cut signal)\n1 = mild (less "
                "blanking)");
            adjustParamOnWheel(apvts.getParameter(paramIdNbThreshold), paramIdNbThreshold, nbT);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Text("NB");
    }
    else
    {
        ImGui::TextDisabled("NB");
    }

    ImGui::Separator();

    // ========== 6. Scanner / Auto-Tune Controls ==========
    ImGui::Text("Auto-Tune");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Auto-tune scans for signals and locks onto them.\n"
            "Uses spectral analysis to detect non-noise signals.");

    // Process scanner logic (60 fps approx)
    static auto lastFrameTime = std::chrono::steady_clock::now();
    auto        now = std::chrono::steady_clock::now();
    float       deltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
    lastFrameTime = now;
    processScannerLogic(deltaTime);

    // Scanner state indicator
    ScannerState state = scannerState.load();
    const char*  stateText = "IDLE";
    ImVec4       stateColor(0.5f, 0.5f, 0.5f, 1.0f); // Gray
    switch (state)
    {
    case ScannerState::Scanning:
        stateText = "SCAN";
        stateColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
        break;
    case ScannerState::Locked:
        stateText = "LOCK";
        stateColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
        break;
    case ScannerState::Holding:
        stateText = "HOLD";
        stateColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); // Orange
        break;
    default:
        break;
    }
    ImGui::SameLine();
    ImGui::TextColored(stateColor, "[%s]", stateText);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Scanner State:\n"
            "IDLE = Off\n"
            "SCAN = Searching for signals\n"
            "LOCK = Locked onto signal\n"
            "HOLD = Signal lost, waiting before resume");
    }

    // Signal Quality Meter (visual bar)
    float quality = currentSignalQuality.load();
    float sfm = currentSpectralFlatness.load();
    ImGui::SameLine();
    ImGui::PushItemWidth(60);
    char qualLabel[32];
    snprintf(qualLabel, sizeof(qualLabel), "Q:%.0f%%", quality * 100.0f);
    ImGui::ProgressBar(quality, ImVec2(60, 0), qualLabel);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Signal Quality: %.1f%%\n"
            "Spectral Flatness: %.2f\n"
            "(Lower flatness = more signal-like)\n\n"
            "Peak: %.4f MHz @ %.1f dB",
            quality * 100.0f,
            sfm,
            detectedPeakFreq.load(),
            detectedPeakPower.load() * 60.0f - 80.0f); // Convert normalized to dB
    }
    ImGui::PopItemWidth();

    // Row 2: Scan controls
    bool scanOn = scanEnabledParam->load() > 0.5f;
    if (ImGui::Checkbox("##ScanEn", &scanOn))
    {
        *scanEnabledParam = scanOn ? 1.0f : 0.0f;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Enable automatic scanning\nWill search for signals and lock onto them");
    ImGui::SameLine();
    ImGui::Text("Scan");

    ImGui::SameLine();

    // Seek buttons (manual step)
    if (ImGui::Button("<<"))
    {
        scanDirection = -1.0f;
        // Manual seek: jump to previous detected peak or step back
        float freq = freqParam->load();
        float scanMin = scanMinParam->load();
        float step = 0.1f; // 100 kHz step
        *freqParam = juce::jlimit(scanMin, scanMaxParam->load(), freq - step);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Seek down\n(-0.1 MHz)\nSets scan direction to DOWN");

    ImGui::SameLine();

    if (ImGui::Button(">>"))
    {
        scanDirection = 1.0f;
        // Manual seek: jump to next detected peak or step forward
        float freq = freqParam->load();
        float scanMax = scanMaxParam->load();
        float step = 0.1f; // 100 kHz step
        *freqParam = juce::jlimit(scanMinParam->load(), scanMax, freq + step);
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Seek up\n(+0.1 MHz)\nSets scan direction to UP");

    ImGui::SameLine();

    // AFC toggle
    bool afcOn = afcEnabledParam->load() > 0.5f;
    if (ImGui::Checkbox("##AFCEn", &afcOn))
    {
        *afcEnabledParam = afcOn ? 1.0f : 0.0f;
        onModificationEnded();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Automatic Frequency Control\n"
            "When locked, tracks signal drift\n"
            "to keep center frequency optimal");
    ImGui::SameLine();
    ImGui::Text("AFC");

    // Row 3: Scanner parameters (collapsible for cleaner UI)
    if (ImGui::TreeNode("Scanner Settings"))
    {
        ImGui::PushItemWidth(80);

        // Scan range
        float sMin = scanMinParam->load();
        float sMax = scanMaxParam->load();
        ImGui::Text("Range:");
        ImGui::SameLine();
        if (ImGui::DragFloat("##ScanMin", &sMin, 0.1f, 24.0f, sMax - 1.0f, "%.1f"))
            *scanMinParam = sMin;
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scan range minimum (MHz)");

        ImGui::SameLine();
        ImGui::Text("-");
        ImGui::SameLine();
        if (ImGui::DragFloat("##ScanMax", &sMax, 0.1f, sMin + 1.0f, 1700.0f, "%.1f"))
            *scanMaxParam = sMax;
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scan range maximum (MHz)");

        // Scan speed
        float speed = scanSpeedParam->load();
        if (ImGui::DragFloat("Speed", &speed, 0.1f, 0.1f, 10.0f, "%.1f MHz/s"))
            *scanSpeedParam = speed;
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Scan speed in MHz per second\n"
                "Slower = better detection\n"
                "Faster = quicker coverage");

        // Detection threshold
        float thresh = detectThresholdParam->load();
        if (ImGui::DragFloat("Threshold", &thresh, 0.5f, 3.0f, 20.0f, "%.1f dB"))
            *detectThresholdParam = thresh;
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Signal detection threshold\n"
                "dB above noise floor\n"
                "Lower = more sensitive (may false trigger)\n"
                "Higher = more selective");

        // AFC range
        float afcR = afcRangeParam->load();
        if (ImGui::DragFloat("AFC Range", &afcR, 1.0f, 1.0f, 50.0f, "%.0f kHz"))
            *afcRangeParam = afcR;
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "AFC correction range (kHz)\n"
                "Maximum frequency correction\n"
                "applied to track signal drift");

        // Hold time
        float hold = holdTimeParam->load();
        if (ImGui::DragFloat("Hold", &hold, 0.1f, 0.5f, 10.0f, "%.1f s"))
            *holdTimeParam = hold;
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Signal hold time (seconds)\n"
                "How long to wait after signal loss\n"
                "before resuming scan");

        ImGui::PopItemWidth();
        ImGui::TreePop();
    }

    ImGui::Separator();

    // ========== 7. Waterfall Range Controls ==========
    float wMin = wfMinParam->load();
    float wMax = wfMaxParam->load();
    ImGui::Text("WF Range");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Waterfall color mapping range.\nMin = cold colors (blue)\nMax = hot colors (red)");

    ImGui::SameLine();
    ImGui::PushItemWidth(60);
    if (ImGui::DragFloat("##wfMin", &wMin, 1.0f, -140.0f, wMax - 5.0f, "%.0f"))
    {
        *wfMinParam = wMin;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Min dB (blue/black threshold)");
    adjustParamOnWheel(apvts.getParameter("wfMin"), "wfMin", wMin);

    ImGui::SameLine();
    ImGui::Text("-");
    ImGui::SameLine();
    if (ImGui::DragFloat("##wfMax", &wMax, 1.0f, wMin + 5.0f, 40.0f, "%.0f"))
    {
        *wfMaxParam = wMax;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Max dB (red threshold)");
    adjustParamOnWheel(apvts.getParameter("wfMax"), "wfMax", wMax);
    ImGui::PopItemWidth();

    ImGui::PopItemWidth(); // Match the PushItemWidth at start
    ImGui::PopID();        // Match PushID(this) at start
}

void SdrReceiverModule::drawIoPins(const NodePinHelpers& helpers)
{
    // Output pins
    helpers.drawAudioOutputPin("Out L", 0);
    helpers.drawAudioOutputPin("Out R", 1);

    // CV Input pins for modulation
    helpers.drawAudioInputPin("Freq CV", 0);    // ±10 MHz modulation
    helpers.drawAudioInputPin("Gain CV", 1);    // ±25 dB modulation
    helpers.drawAudioInputPin("Squelch CV", 2); // ±30 dB modulation
    helpers.drawAudioInputPin("BW CV", 3);      // ±50 kHz modulation
}

#endif // PRESET_CREATOR_UI
