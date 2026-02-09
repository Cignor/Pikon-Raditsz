#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>
#include <cmath>

// ==============================================================================
// DC Blocker - Removes DC offset from IQ signal
// ==============================================================================
class DcBlocker
{
public:
    std::complex<float> process(std::complex<float> in)
    {
        std::complex<float> out = in - lastIn + alpha * lastOut;
        lastIn = in;
        lastOut = out;
        return out;
    }

    void reset()
    {
        lastIn = {0.0f, 0.0f};
        lastOut = {0.0f, 0.0f};
    }

private:
    std::complex<float> lastIn{0.0f, 0.0f};
    std::complex<float> lastOut{0.0f, 0.0f};
    float               alpha = 0.9999f;
};

// ==============================================================================
// FIR Decimator for complex IQ signals
// ==============================================================================
class FirDecimator
{
public:
    void prepare(int factor, float normalizedCutoff, int numTaps)
    {
        decimationFactor = factor;
        taps = numTaps;

        // Design lowpass FIR using sinc with Hamming window
        coeffs.resize(taps);
        float sum = 0.0f;
        for (int i = 0; i < taps; ++i)
        {
            int n = i - taps / 2;
            if (n == 0)
            {
                coeffs[i] = normalizedCutoff;
            }
            else
            {
                float x = juce::MathConstants<float>::pi * normalizedCutoff * n;
                coeffs[i] = std::sin(x) / (juce::MathConstants<float>::pi * n);
            }
            // Hamming window
            float w =
                0.54f - 0.46f * std::cos(2.0f * juce::MathConstants<float>::pi * i / (taps - 1));
            coeffs[i] *= w;
            sum += coeffs[i];
        }
        for (auto& c : coeffs)
            c /= sum;

        buffer.resize(taps, {0.0f, 0.0f});
        bufferIndex = 0;
        sampleCount = 0;
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), std::complex<float>{0.0f, 0.0f});
        bufferIndex = 0;
        sampleCount = 0;
    }

    void process(
        const std::vector<std::complex<float>>& input,
        std::vector<std::complex<float>>&       output)
    {
        output.clear();
        output.reserve((input.size() + decimationFactor - 1) / decimationFactor);

        for (const auto& sample : input)
        {
            buffer[bufferIndex] = sample;
            bufferIndex = (bufferIndex + 1) % taps;
            sampleCount++;

            if (sampleCount >= decimationFactor)
            {
                sampleCount = 0;
                std::complex<float> y{0.0f, 0.0f};
                int                 idx = bufferIndex;
                for (int i = 0; i < taps; ++i)
                {
                    idx = (idx == 0) ? (taps - 1) : (idx - 1);
                    y += buffer[idx] * coeffs[i];
                }
                output.push_back(y);
            }
        }
    }

private:
    int                              decimationFactor = 1;
    int                              taps = 1;
    std::vector<float>               coeffs;
    std::vector<std::complex<float>> buffer;
    int                              bufferIndex = 0;
    int                              sampleCount = 0;
};

// ==============================================================================
// Audio Decimator for real-valued audio signals
// ==============================================================================
class AudioDecimator
{
public:
    void prepare(int factor, float normalizedCutoff, int numTaps)
    {
        decimationFactor = factor;
        taps = numTaps;

        coeffs.resize(taps);
        float sum = 0.0f;
        for (int i = 0; i < taps; ++i)
        {
            int n = i - taps / 2;
            if (n == 0)
            {
                coeffs[i] = normalizedCutoff;
            }
            else
            {
                float x = juce::MathConstants<float>::pi * normalizedCutoff * n;
                coeffs[i] = std::sin(x) / (juce::MathConstants<float>::pi * n);
            }
            float w =
                0.54f - 0.46f * std::cos(2.0f * juce::MathConstants<float>::pi * i / (taps - 1));
            coeffs[i] *= w;
            sum += coeffs[i];
        }
        for (auto& c : coeffs)
            c /= sum;

        buffer.resize(taps, 0.0f);
        bufferIndex = 0;
        sampleCount = 0;
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        bufferIndex = 0;
        sampleCount = 0;
    }

    void process(const std::vector<float>& input, std::vector<float>& output)
    {
        output.clear();
        output.reserve((input.size() + decimationFactor - 1) / decimationFactor);

        for (float sample : input)
        {
            buffer[bufferIndex] = sample;
            bufferIndex = (bufferIndex + 1) % taps;
            sampleCount++;

            if (sampleCount >= decimationFactor)
            {
                sampleCount = 0;
                float y = 0.0f;
                int   idx = bufferIndex;
                for (int i = 0; i < taps; ++i)
                {
                    idx = (idx == 0) ? (taps - 1) : (idx - 1);
                    y += buffer[idx] * coeffs[i];
                }
                output.push_back(y);
            }
        }
    }

private:
    int                decimationFactor = 1;
    int                taps = 1;
    std::vector<float> coeffs;
    std::vector<float> buffer;
    int                bufferIndex = 0;
    int                sampleCount = 0;
};

// ==============================================================================
// De-Emphasis Filter (50µs EU / 75µs US)
// ==============================================================================
class DeEmphasisFilter
{
public:
    enum class Region
    {
        Off,
        US_75us,
        EU_50us
    };

    void prepare(double sampleRate, Region region)
    {
        setRegion(region);
        this->sampleRate = sampleRate;
        updateCoeffs();
    }

    void setRegion(Region newRegion)
    {
        region = newRegion;
        updateCoeffs();
    }

    void process(float& left, float& right)
    {
        if (region == Region::Off)
            return;

        float outL = b0 * left + b1 * lastInL - a1 * lastOutL;
        float outR = b0 * right + b1 * lastInR - a1 * lastOutR;

        lastInL = left;
        lastInR = right;
        lastOutL = outL;
        lastOutR = outR;

        left = outL;
        right = outR;
    }

    void reset() { lastInL = lastInR = lastOutL = lastOutR = 0.0f; }

private:
    void updateCoeffs()
    {
        if (region == Region::Off || sampleRate <= 0)
        {
            a1 = 0.0f;
            b0 = 1.0f;
            b1 = 0.0f;
            return;
        }

        double tau = (region == Region::US_75us) ? 75e-6 : 50e-6;
        double x = std::exp(-1.0 / (sampleRate * tau));
        a1 = static_cast<float>(-x);
        b0 = static_cast<float>(1.0 - x);
        b1 = 0.0f;
    }

    Region region = Region::EU_50us;
    double sampleRate = 48000.0;
    float  a1 = 0.0f, b0 = 1.0f, b1 = 0.0f;
    float  lastInL = 0.0f, lastInR = 0.0f;
    float  lastOutL = 0.0f, lastOutR = 0.0f;
};

// ==============================================================================
// Simple AGC
// ==============================================================================
class AutomaticGainControl
{
public:
    enum class Speed
    {
        Fast,
        Medium,
        Slow
    };

    void prepare(double sampleRate)
    {
        this->sampleRate = sampleRate;
        setSpeed(Speed::Medium);
    }

    void setSpeed(Speed s)
    {
        speed = s;
        switch (s)
        {
        case Speed::Fast:
            attackCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.001)));
            releaseCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.050)));
            break;
        case Speed::Medium:
            attackCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.005)));
            releaseCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.100)));
            break;
        case Speed::Slow:
            attackCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.020)));
            releaseCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.500)));
            break;
        }
    }

    void process(float& left, float& right)
    {
        if (!enabled)
            return;

        float peak = std::max(std::abs(left), std::abs(right));
        float coeff = (peak > envelope) ? attackCoeff : releaseCoeff;
        envelope = coeff * envelope + (1.0f - coeff) * peak;

        float targetGain = (envelope > 0.001f) ? (targetLevel / envelope) : 1.0f;
        targetGain = std::min(targetGain, maxGain);

        // Smooth gain changes
        currentGain += 0.01f * (targetGain - currentGain);

        left *= currentGain;
        right *= currentGain;
    }

    void setEnabled(bool e) { enabled = e; }
    void reset()
    {
        envelope = 0.001f;
        currentGain = 1.0f;
    }

private:
    double sampleRate = 48000.0;
    Speed  speed = Speed::Medium;
    bool   enabled = false;
    float  attackCoeff = 0.99f;
    float  releaseCoeff = 0.999f;
    float  envelope = 0.001f;
    float  currentGain = 1.0f;
    float  targetLevel = 0.5f;
    float  maxGain = 50.0f;
};

// ==============================================================================
// Smooth Squelch
// ==============================================================================
class SmoothSquelch
{
public:
    void prepare(double sampleRate)
    {
        // ~20ms attack/release for signal level smoothing
        signalCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.020)));
        // ~10ms for gate smoothing (sample-rate dependent)
        gateSmooth = static_cast<float>(1.0 / (sampleRate * 0.010));
    }

    void setThreshold(float db)
    {
        thresholdDb = db;
        thresholdLinear = std::pow(10.0f, db / 20.0f);
    }

    void updateSignalLevel(float magnitude)
    {
        signalLevel = signalCoeff * signalLevel + (1.0f - signalCoeff) * magnitude;
    }

    void process(float& left, float& right)
    {
        float targetGate = (signalLevel > thresholdLinear) ? 1.0f : 0.0f;
        gate += gateSmooth * (targetGate - gate);
        gate = std::max(0.0f, std::min(1.0f, gate));

        left *= gate;
        right *= gate;
    }

    bool  isOpen() const { return gate > 0.5f; }
    float getThresholdDb() const { return thresholdDb; }

    void reset()
    {
        signalLevel = 0.0f;
        gate = 0.0f;
    }

private:
    float signalCoeff = 0.99f;
    float gateSmooth = 0.05f;
    float thresholdDb = -60.0f;
    float thresholdLinear = 0.001f;
    float signalLevel = 0.0f;
    float gate = 0.0f;
};

// ==============================================================================
// Variable Bandwidth Filter (IIR approximation)
// ==============================================================================
class VariableBandwidthFilter
{
public:
    void prepare(double sampleRate)
    {
        this->sampleRate = sampleRate;
        setBandwidth(200.0f); // Default WFM: 200 kHz
    }

    void setBandwidth(float bandwidthKhz)
    {
        // Input is in kHz from UI, convert to Hz
        float hz = bandwidthKhz * 1000.0f;

        // Clamp to reasonable range (min 5kHz, max limited by sample rate)
        hz = std::max(5000.0f, hz);

        // Limit cutoff to 40% of sample rate (safe margin below Nyquist)
        // For WFM at 240kHz, this allows up to ~96kHz bandwidth (48kHz cutoff)
        float maxBandwidth = static_cast<float>(sampleRate) * 0.8f;
        bandwidth = std::min(hz, maxBandwidth);

        updateCoeffs();
    }

    std::complex<float> process(std::complex<float> in)
    {
        // Simple 2-pole lowpass
        float x = in.real();
        float y1 = a0 * x + a1 * prevX1 + a2 * prevX2 - b1 * prevY1 - b2 * prevY2;
        prevX2 = prevX1;
        prevX1 = x;
        prevY2 = prevY1;
        prevY1 = y1;

        float xi = in.imag();
        float y1i = a0 * xi + a1 * prevXi1 + a2 * prevXi2 - b1 * prevYi1 - b2 * prevYi2;
        prevXi2 = prevXi1;
        prevXi1 = xi;
        prevYi2 = prevYi1;
        prevYi1 = y1i;

        return {y1, y1i};
    }

    void reset()
    {
        prevX1 = prevX2 = prevY1 = prevY2 = 0.0f;
        prevXi1 = prevXi2 = prevYi1 = prevYi2 = 0.0f;
    }

private:
    void updateCoeffs()
    {
        // Butterworth 2-pole lowpass
        // Cutoff is half of bandwidth (bandwidth is -3dB total width)
        float fc = bandwidth / 2.0f;

        // CRITICAL: Clamp cutoff to 40% of Nyquist to prevent filter instability
        // At w0 > π/2, the bilinear transform produces unstable coefficients
        float maxFc = static_cast<float>(sampleRate) * 0.4f;
        fc = std::min(fc, maxFc);

        float w0 = 2.0f * juce::MathConstants<float>::pi * fc / static_cast<float>(sampleRate);
        float alpha = std::sin(w0) / (2.0f * 0.707f); // Q = sqrt(2)/2 for Butterworth

        float cosW0 = std::cos(w0);
        float norm = 1.0f / (1.0f + alpha);

        a0 = ((1.0f - cosW0) / 2.0f) * norm;
        a1 = (1.0f - cosW0) * norm;
        a2 = a0;
        b1 = (-2.0f * cosW0) * norm;
        b2 = (1.0f - alpha) * norm;
    }

    double sampleRate = 480000.0;
    float  bandwidth = 200000.0f;
    float  a0 = 1.0f, a1 = 0.0f, a2 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    float  prevX1 = 0.0f, prevX2 = 0.0f, prevY1 = 0.0f, prevY2 = 0.0f;
    float  prevXi1 = 0.0f, prevXi2 = 0.0f, prevYi1 = 0.0f, prevYi2 = 0.0f;
};

// ==============================================================================
// Notch Filter for interference
// ==============================================================================
class NotchFilter
{
public:
    void prepare(double sampleRate)
    {
        this->sampleRate = sampleRate;
        updateCoeffs();
    }
    void setFrequency(float hz)
    {
        frequency = hz;
        updateCoeffs();
    }
    void setEnabled(bool e) { enabled = e; }

    void process(float& left, float& right)
    {
        if (!enabled)
            return;
        left = processSample(left, stateL);
        right = processSample(right, stateR);
    }

    void reset() { stateL = stateR = {0, 0, 0, 0}; }

private:
    float processSample(float in, std::array<float, 4>& s)
    {
        float out = a0 * in + a1 * s[0] + a2 * s[1] - b1 * s[2] - b2 * s[3];
        s[1] = s[0];
        s[0] = in;
        s[3] = s[2];
        s[2] = out;
        return out;
    }

    void updateCoeffs()
    {
        float w0 =
            2.0f * juce::MathConstants<float>::pi * frequency / static_cast<float>(sampleRate);
        float bw = w0 / 30.0f;
        float cosW0 = std::cos(w0);
        float norm = 1.0f / (1.0f + bw);
        a0 = norm;
        a1 = -2.0f * cosW0 * norm;
        a2 = norm;
        b1 = -2.0f * cosW0 * norm;
        b2 = (1.0f - bw) * norm;
    }

    bool                 enabled = false;
    double               sampleRate = 48000.0;
    float                frequency = 1000.0f;
    float                a0 = 1.0f, a1 = 0.0f, a2 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    std::array<float, 4> stateL{}, stateR{};
};

// ==============================================================================
// Noise Blanker
// ==============================================================================
class NoiseBlanker
{
public:
    void prepare(double sampleRate)
    {
        attackCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.0001)));
        releaseCoeff = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.010)));
    }

    void setEnabled(bool e) { enabled = e; }
    void setThreshold(float t) { threshold = std::max(1.5f, t); }

    void process(float& left, float& right)
    {
        if (!enabled)
            return;

        float peak = std::max(std::abs(left), std::abs(right));
        float coeff = (peak > envelope) ? attackCoeff : releaseCoeff;
        envelope = coeff * envelope + (1.0f - coeff) * peak;

        // Smooth blanking gate instead of harsh attenuation
        float targetGate = (peak > envelope * threshold) ? 0.0f : 1.0f;
        blankingGate += 0.1f * (targetGate - blankingGate); // Smooth transition
        blankingGate = std::max(0.0f, std::min(1.0f, blankingGate));

        // Apply smooth gain reduction
        left *= blankingGate;
        right *= blankingGate;
    }

    void reset()
    {
        envelope = 0.0f;
        blankingGate = 1.0f;
    }

private:
    bool  enabled = false;
    float threshold = 5.0f;
    float attackCoeff = 0.9f, releaseCoeff = 0.999f;
    float envelope = 0.0f;
    float blankingGate = 1.0f;
};

// ==============================================================================
// Audio Lowpass Filter (15kHz Butterworth)
// ==============================================================================
class AudioLowpassFilter
{
public:
    void prepare(double sampleRate, float cutoffHz)
    {
        this->sampleRate = sampleRate;
        setCutoff(cutoffHz);
    }

    void setCutoff(float hz)
    {
        cutoff = hz;
        float w0 = 2.0f * juce::MathConstants<float>::pi * cutoff / static_cast<float>(sampleRate);
        float alpha = std::sin(w0) / (2.0f * 0.707f);
        float cosW0 = std::cos(w0);
        float norm = 1.0f / (1.0f + alpha);

        a0 = ((1.0f - cosW0) / 2.0f) * norm;
        a1 = (1.0f - cosW0) * norm;
        a2 = a0;
        b1 = (-2.0f * cosW0) * norm;
        b2 = (1.0f - alpha) * norm;
    }

    void process(float& left, float& right)
    {
        float outL = a0 * left + a1 * xL1 + a2 * xL2 - b1 * yL1 - b2 * yL2;
        xL2 = xL1;
        xL1 = left;
        yL2 = yL1;
        yL1 = outL;

        float outR = a0 * right + a1 * xR1 + a2 * xR2 - b1 * yR1 - b2 * yR2;
        xR2 = xR1;
        xR1 = right;
        yR2 = yR1;
        yR1 = outR;

        left = outL;
        right = outR;
    }

    void reset()
    {
        xL1 = xL2 = yL1 = yL2 = 0.0f;
        xR1 = xR2 = yR1 = yR2 = 0.0f;
    }

private:
    double sampleRate = 48000.0;
    float  cutoff = 15000.0f;
    float  a0 = 1.0f, a1 = 0.0f, a2 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    float  xL1 = 0.0f, xL2 = 0.0f, yL1 = 0.0f, yL2 = 0.0f;
    float  xR1 = 0.0f, xR2 = 0.0f, yR1 = 0.0f, yR2 = 0.0f;
};

// ==============================================================================
// SdrDemodulator - Main demodulator class
// ==============================================================================
class SdrDemodulator
{
public:
    enum class Mode
    {
        WFM = 0,
        NFM,
        AM
    };

    SdrDemodulator() { reset(); }

    void prepare(double inputRate, double outputRate)
    {
        this->inputSampleRate = inputRate;
        this->outputSampleRate = outputRate;

        // IQ decimation: 2.4MHz -> 240kHz (10x)
        // This provides adequate bandwidth for WFM while being manageable
        decimatorIQ.prepare(10, 0.04f, 127); // Cutoff at ~100kHz
        demodulationRate = inputRate / 10.0; // 240kHz

        // Audio decimation: 240kHz -> 48kHz (5x)
        audioDecimator.prepare(5, 0.083f, 63); // Cutoff at ~20kHz

        // Bandwidth filter at 240kHz
        bandwidthFilter.prepare(demodulationRate);

        // Post-processing at 48kHz
        deEmphasis.prepare(outputRate, DeEmphasisFilter::Region::EU_50us);
        agc.prepare(outputRate);
        squelch.prepare(outputRate);
        squelch.setThreshold(-60.0f);
        audioLowpass.prepare(outputRate, 15000.0f);
        notchFilter.prepare(outputRate);
        noiseBlanker.prepare(outputRate);

        dcBlocker.reset();
        setMode(currentMode);
    }

    void reset()
    {
        lastSample = {0.0f, 0.0f};
        dcBlocker.reset();
        decimatorIQ.reset();
        audioDecimator.reset();
        deEmphasis.reset();
        agc.reset();
        squelch.reset();
        audioLowpass.reset();
        bandwidthFilter.reset();
        notchFilter.reset();
        noiseBlanker.reset();
    }

    void setMode(Mode newMode)
    {
        if (currentMode != newMode)
        {
            currentMode = newMode;
            reset();
        }
    }

    void setSquelchThreshold(float db) { squelch.setThreshold(db); }
    void setDeEmphasisRegion(DeEmphasisFilter::Region region) { deEmphasis.setRegion(region); }
    void setAgcEnabled(bool e) { agc.setEnabled(e); }
    void setAgcSpeed(AutomaticGainControl::Speed s) { agc.setSpeed(s); }
    void setBandwidth(float hz) { bandwidthFilter.setBandwidth(hz); }
    void setNotchFrequency(float hz) { notchFilter.setFrequency(hz); }
    void setNotchEnabled(bool e) { notchFilter.setEnabled(e); }
    void setNoiseBlankerEnabled(bool e) { noiseBlanker.setEnabled(e); }
    void setNoiseBlankerThreshold(float t) { noiseBlanker.setThreshold(t); }
    bool isSquelchOpen() const { return squelch.isOpen(); }

    void process(
        const std::vector<std::complex<float>>& inputIq,
        std::vector<float>&                     outL,
        std::vector<float>&                     outR)
    {
        outL.clear();
        outR.clear();

        if (inputIq.empty())
            return;

        // ==== Stage 1: DC Blocking ====
        std::vector<std::complex<float>> dcBlocked;
        dcBlocked.reserve(inputIq.size());
        for (const auto& s : inputIq)
        {
            dcBlocked.push_back(dcBlocker.process(s));
        }

        // ==== Stage 2: Decimate IQ 2.4MHz -> 240kHz (10x) ====
        std::vector<std::complex<float>> iqDecimated;
        decimatorIQ.process(dcBlocked, iqDecimated);

        // ==== Stage 3: Channel filter at 240kHz ====
        std::vector<std::complex<float>> iqFiltered;
        iqFiltered.reserve(iqDecimated.size());
        for (const auto& s : iqDecimated)
        {
            iqFiltered.push_back(bandwidthFilter.process(s));
        }

        // ==== Stage 4: DEMODULATE AT 240kHz (high sample rate!) ====
        std::vector<float> audioRaw;
        audioRaw.reserve(iqFiltered.size());

        for (const auto& s : iqFiltered)
        {
            // Measure signal level for squelch
            float magnitude = std::abs(s);
            squelch.updateSignalLevel(magnitude);

            float audio = 0.0f;

            switch (currentMode)
            {
            case Mode::WFM:
                audio = demodulateWFM(s);
                break;
            case Mode::NFM:
                audio = demodulateNFM(s);
                break;
            case Mode::AM:
                audio = demodulateAM(s);
                break;
            }

            audioRaw.push_back(audio);
        }

        // Store last IQ sample for next block
        if (!iqFiltered.empty())
        {
            lastSample = iqFiltered.back();
        }

        // ==== Stage 5: Decimate AUDIO 240kHz -> 48kHz (5x) ====
        std::vector<float> audioDecimated;
        audioDecimator.process(audioRaw, audioDecimated);

        // ==== Stage 6: Post-processing at 48kHz ====
        outL.reserve(audioDecimated.size());
        outR.reserve(audioDecimated.size());

        for (float audio : audioDecimated)
        {
            float audioL = audio;
            float audioR = audio;

            // Apply 15kHz lowpass (removes pilot tone)
            audioLowpass.process(audioL, audioR);

            // Apply de-emphasis (WFM only)
            if (currentMode == Mode::WFM)
            {
                deEmphasis.process(audioL, audioR);
            }

            // Apply notch filter
            notchFilter.process(audioL, audioR);

            // Apply noise blanker
            noiseBlanker.process(audioL, audioR);

            // Apply AGC
            agc.process(audioL, audioR);

            // Apply squelch
            squelch.process(audioL, audioR);

            // Soft limit
            audioL = softLimit(audioL);
            audioR = softLimit(audioR);

            outL.push_back(audioL);
            outR.push_back(audioR);
        }
    }

private:
    double inputSampleRate = 2400000.0;
    double outputSampleRate = 48000.0;
    double demodulationRate = 240000.0;

    Mode currentMode = Mode::WFM;

    // State
    std::complex<float> lastSample{0.0f, 0.0f};
    float               avgCarrier = 0.001f;

    // DSP Components
    DcBlocker               dcBlocker;
    FirDecimator            decimatorIQ;
    AudioDecimator          audioDecimator;
    DeEmphasisFilter        deEmphasis;
    AutomaticGainControl    agc;
    SmoothSquelch           squelch;
    AudioLowpassFilter      audioLowpass;
    VariableBandwidthFilter bandwidthFilter;
    NotchFilter             notchFilter;
    NoiseBlanker            noiseBlanker;

    // --- Demodulators ---

    float demodulateWFM(std::complex<float> s)
    {
        // FM discriminator: phase difference between consecutive samples
        float phaseDiff = std::atan2(
            s.imag() * lastSample.real() - s.real() * lastSample.imag(),
            s.real() * lastSample.real() + s.imag() * lastSample.imag());

        lastSample = s;

        // FM demodulation at 240kHz sample rate
        // Gain = sampleRate / (2π * deviation) to normalize to ±1.0
        // For WFM: ±75kHz deviation
        constexpr float fmDeviation = 75000.0f;
        const float     demodRate = static_cast<float>(demodulationRate);
        const float     fmGain = demodRate / (juce::MathConstants<float>::twoPi * fmDeviation);

        return phaseDiff * fmGain;
    }

    float demodulateNFM(std::complex<float> s)
    {
        float phaseDiff = std::atan2(
            s.imag() * lastSample.real() - s.real() * lastSample.imag(),
            s.real() * lastSample.real() + s.imag() * lastSample.imag());

        lastSample = s;

        // NFM: ±5kHz deviation
        constexpr float fmDeviation = 5000.0f;
        const float     demodRate = static_cast<float>(demodulationRate);
        const float     fmGain = demodRate / (juce::MathConstants<float>::twoPi * fmDeviation);

        return phaseDiff * fmGain;
    }

    float demodulateAM(std::complex<float> s)
    {
        // Simple envelope detection
        float envelope = std::abs(s);

        // AGC for AM
        avgCarrier = 0.999f * avgCarrier + 0.001f * envelope;
        float normalized = (avgCarrier > 0.001f) ? (envelope / avgCarrier - 1.0f) : 0.0f;

        lastSample = s;
        return normalized;
    }

    float softLimit(float x)
    {
        // Soft clipper
        if (x > 1.0f)
            return 1.0f - 1.0f / (1.0f + (x - 1.0f));
        if (x < -1.0f)
            return -1.0f + 1.0f / (1.0f - (x + 1.0f));
        return x;
    }
};
