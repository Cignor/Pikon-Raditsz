import re

filepath = r'h:\0000_CODE\01_collider_pyo\juce\Source\audio\modules\SdrDemodulator.h'

with open(filepath, 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Add AudioDecimator class after AudioLowpassFilter class (around line 670)
audio_decimator_class = '''
// Simple FIR decimator for real audio (mono to stereo handled externally)
class AudioDecimator
{
public:
    void prepare(int factor, float normalizedCutoff, int numTaps)
    {
        decimationFactor = factor;
        taps = numTaps;
        
        // Design lowpass FIR coefficients
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
            float w = 0.54f - 0.46f * std::cos(2.0f * juce::MathConstants<float>::pi * i / (taps - 1));
            coeffs[i] *= w;
            sum += coeffs[i];
        }
        // Normalize
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
                // Convolve
                float y = 0.0f;
                int idx = bufferIndex;
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
    int                  decimationFactor = 1;
    int                  taps = 1;
    std::vector<float>   coeffs;
    std::vector<float>   buffer;
    int                  bufferIndex = 0;
    int                  sampleCount = 0;
};

'''

# Find position to insert after AudioLowpassFilter class
# Looking for the end of AudioLowpassFilter class (after private: section ends with };)
audio_lowpass_end = content.find('// ==============================================================================\n// SdrDemodulator - Main demodulator')
if audio_lowpass_end == -1:
    # Try alternate search
    audio_lowpass_end = content.find('class SdrDemodulator')

content = content[:audio_lowpass_end] + audio_decimator_class + '\n' + content[audio_lowpass_end:]

# 2. Update prepare() to add audio decimators (10x total: 5x then 2x for LP filtering)
# Find and update the prepare method
old_prepare_decimators = '''        // Stage 2: 5x decimation (480kHz -> 96kHz)
        // Cutoff at ~38kHz (normalized = 38k/480k = 0.079)
        decimator2.prepare(5, 0.079f, 63);

        // Stage 3: 2x decimation (96kHz -> 48kHz)
        // Cutoff at ~19kHz (normalized = 19k/96k = 0.198)
        decimator3.prepare(2, 0.198f, 31);'''

new_prepare_decimators = '''        // Audio decimation: 480kHz -> 48kHz (10x total)
        // We demodulate at 480kHz, then decimate AUDIO (not IQ)
        // Stage 2: 5x audio decimation (480kHz -> 96kHz)
        // Cutoff at ~20kHz (normalized = 20k/480k = 0.042)
        audioDecimator1.prepare(5, 0.042f, 63);

        // Stage 3: 2x audio decimation (96kHz -> 48kHz)
        // Cutoff at ~20kHz (normalized = 20k/96k = 0.208)
        audioDecimator2.prepare(2, 0.208f, 31);'''

content = content.replace(old_prepare_decimators, new_prepare_decimators)

# 3. Update reset() to reset audio decimators instead of IQ decimators 2&3
old_reset = '''        decimator1.reset();
        decimator2.reset();
        decimator3.reset();'''

new_reset = '''        decimator1.reset();
        audioDecimator1.reset();
        audioDecimator2.reset();'''

content = content.replace(old_reset, new_reset)

# 4. Replace the process() function completely
old_process_start = '''    void process(
        const std::vector<std::complex<float>>& inputIq,
        std::vector<float>&                     outL,
        std::vector<float>&                     outR)
    {
        outL.clear();
        outR.clear();

        if (inputIq.empty())
            return;

        // 1. DC Blocking
        std::vector<std::complex<float>> dcBlocked;
        dcBlocked.reserve(inputIq.size());
        for (const auto& s : inputIq)
        {
            dcBlocked.push_back(dcBlocker.process(s));
        }

        // 2. First stage decimation (2.4MHz -> 480kHz)
        std::vector<std::complex<float>> stage1Out;
        decimator1.process(dcBlocked, stage1Out);

        // 3. Apply bandwidth filter at intermediate rate (480kHz)
        std::vector<std::complex<float>> bandwidthFiltered1;
        bandwidthFiltered1.reserve(stage1Out.size());
        for (const auto& s : stage1Out)
        {
            bandwidthFiltered1.push_back(bandwidthFilter.process(s));
        }

        // 4. Second stage decimation (480kHz -> 96kHz)
        std::vector<std::complex<float>> stage2Out;
        decimator2.process(bandwidthFiltered1, stage2Out);

        // 5. Third stage decimation (96kHz -> 48kHz)
        std::vector<std::complex<float>> stage3Out;
        decimator3.process(stage2Out, stage3Out);

        // Reserve output space
        outL.reserve(stage3Out.size());
        outR.reserve(stage3Out.size());

        // Apply final bandwidth filtering at output rate
        std::vector<std::complex<float>> bandwidthFiltered;
        bandwidthFiltered.reserve(stage3Out.size());
        for (const auto& s : stage3Out)
        {
            bandwidthFiltered.push_back(bandwidthFilter.process(s));
        }

        // 4. Demodulation and post-processing
        for (const auto& s : bandwidthFiltered)
        {
            // Measure signal level for squelch
            float magnitude = std::abs(s);
            squelch.updateSignalLevel(magnitude);

            // Demodulate based on mode
            float audioL = 0.0f;
            float audioR = 0.0f;

            switch (currentMode)
            {
            case Mode::WFM:
                demodulateWFM(s, audioL, audioR);
                break;
            case Mode::NFM:
                demodulateNFM(s, audioL, audioR);
                break;
            case Mode::AM:
                demodulateAM(s, audioL, audioR);
                break;
            }

            // Apply 15kHz audio lowpass filter (removes pilot tone and artifacts)
            audioLowpass.process(audioL, audioR);

            // Apply de-emphasis (only for WFM typically)
            if (currentMode == Mode::WFM)
            {
                deEmphasis.process(audioL, audioR);
            }

            // Apply notch filter for interference rejection
            notchFilter.process(audioL, audioR);

            // Apply noise blanker for impulse noise reduction
            noiseBlanker.process(audioL, audioR);

            // Apply AGC
            agc.process(audioL, audioR);

            // Apply squelch
            squelch.process(audioL, audioR);

            // Soft limiter to prevent clipping
            audioL = softLimit(audioL);
            audioR = softLimit(audioR);

            outL.push_back(audioL);
            outR.push_back(audioR);
        }

        // Store last sample for next block continuity
        if (!bandwidthFiltered.empty())
        {
            lastSample = bandwidthFiltered.back();
        }
    }'''

new_process = '''    void process(
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

        // ==== Stage 2: Decimate IQ 2.4MHz -> 480kHz ====
        std::vector<std::complex<float>> iqAt480k;
        decimator1.process(dcBlocked, iqAt480k);

        // ==== Stage 3: Channel filter at 480kHz ====
        std::vector<std::complex<float>> iqFiltered;
        iqFiltered.reserve(iqAt480k.size());
        for (const auto& s : iqAt480k)
        {
            iqFiltered.push_back(bandwidthFilter.process(s));
        }

        // ==== Stage 4: DEMODULATE AT 480kHz (not 48kHz!) ====
        // This is critical - WFM has ±75kHz deviation, so we need high sample rate
        std::vector<float> audioRaw;
        audioRaw.reserve(iqFiltered.size());

        for (const auto& s : iqFiltered)
        {
            // Measure signal level for squelch
            float magnitude = std::abs(s);
            squelch.updateSignalLevel(magnitude);

            float audioL = 0.0f;
            float audioR = 0.0f;

            switch (currentMode)
            {
            case Mode::WFM:
                demodulateWFM(s, audioL, audioR);
                break;
            case Mode::NFM:
                demodulateNFM(s, audioL, audioR);
                break;
            case Mode::AM:
                demodulateAM(s, audioL, audioR);
                break;
            }

            // Store mono audio (stereo is same for now)
            audioRaw.push_back(audioL);
        }

        // Store last IQ sample for next block
        if (!iqFiltered.empty())
        {
            lastSample = iqFiltered.back();
        }

        // ==== Stage 5: Decimate AUDIO 480kHz -> 96kHz ====
        std::vector<float> audioAt96k;
        audioDecimator1.process(audioRaw, audioAt96k);

        // ==== Stage 6: Decimate AUDIO 96kHz -> 48kHz ====
        std::vector<float> audioAt48k;
        audioDecimator2.process(audioAt96k, audioAt48k);

        // ==== Stage 7: Post-processing at 48kHz ====
        outL.reserve(audioAt48k.size());
        outR.reserve(audioAt48k.size());

        for (float audio : audioAt48k)
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
    }'''

content = content.replace(old_process_start, new_process)

# 5. Update demodulateWFM to use 480kHz sample rate
old_wfm_gain = '''        // Proper FM demodulation gain calculation:
        // FM deviation = ±75 kHz for WFM broadcast
        // At 48kHz output sample rate, max phase deviation per sample is:
        //   maxPhaseDev = 2π * (75000 / 48000) ≈ 9.82 radians
        // We want to normalize output to approximately ±1.0
        // Gain = 1 / maxPhaseDev = 48000 / (2π * 75000) ≈ 0.102
        constexpr float fmDeviation = 75000.0f; // Hz
        constexpr float outputRate = 48000.0f;  // Hz
        const float     fmGain = outputRate / (juce::MathConstants<float>::twoPi * fmDeviation);'''

new_wfm_gain = '''        // FM demodulation at 480kHz sample rate (NOT 48kHz!)
        // FM deviation = ±75 kHz for WFM broadcast
        // Gain = sampleRate / (2π * deviation) to normalize output to ±1.0
        constexpr float fmDeviation = 75000.0f;    // Hz
        constexpr float demodRate = 480000.0f;     // Sample rate for demodulation
        const float     fmGain = demodRate / (juce::MathConstants<float>::twoPi * fmDeviation);'''

content = content.replace(old_wfm_gain, new_wfm_gain)

# 6. Update member variables - replace decimator2/3 with audioDecimator1/2
old_members = '''    // DSP Components
    DcBlocker            dcBlocker;
    FirDecimator         decimator1; // IQ decimation stage 1
    FirDecimator         decimator2; // IQ decimation stage 2
    FirDecimator         decimator3; // IQ decimation stage 3'''

new_members = '''    // DSP Components
    DcBlocker            dcBlocker;
    FirDecimator         decimator1;      // IQ decimation: 2.4MHz -> 480kHz
    AudioDecimator       audioDecimator1; // Audio decimation: 480kHz -> 96kHz
    AudioDecimator       audioDecimator2; // Audio decimation: 96kHz -> 48kHz'''

content = content.replace(old_members, new_members)

with open(filepath, 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(content)

print('Done! Restructured FM demodulator to demodulate at 480kHz instead of 48kHz.')
print('Key changes:')
print('  1. Added AudioDecimator class for real-valued audio decimation')
print('  2. Changed process() to demodulate at 480kHz then decimate audio')
print('  3. Updated FM gain calculation to use 480kHz sample rate')
print('  4. Replaced IQ decimators 2&3 with audio decimators')
