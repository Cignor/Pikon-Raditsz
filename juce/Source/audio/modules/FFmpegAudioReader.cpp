#include "FFmpegAudioReader.h"
#include <mutex>
#include <cmath>
#include <algorithm>

#if defined(_WIN32) || defined(_WIN64)
    #pragma warning(push)
    #pragma warning(disable: 4244 4996)
#endif

// Include FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#if defined(_WIN32) || defined(_WIN64)
    #pragma warning(pop)
#endif

FFmpegAudioReader::FFmpegAudioReader(const juce::String& path)
    : juce::AudioFormatReader(nullptr, "FFmpeg"), filePath(path)
{
    initializeWithSampleRate(0.0); // 0 = use file's native sample rate
}

FFmpegAudioReader::FFmpegAudioReader(const juce::String& path, double targetSampleRate)
    : juce::AudioFormatReader(nullptr, "FFmpeg"), filePath(path)
{
    initializeWithSampleRate(targetSampleRate);
}

void FFmpegAudioReader::initializeWithSampleRate(double targetRate)
{
    // Initialize FFmpeg networking capabilities once.
    static std::once_flag ffmpegInitialized;
    std::call_once(ffmpegInitialized, []() { avformat_network_init(); });

    if (avformat_open_input(&formatContext, filePath.toUTF8(), nullptr, nullptr) != 0) return;
    if (avformat_find_stream_info(formatContext, nullptr) < 0) { cleanup(); return; }

    streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) { cleanup(); return; }

    audioStream = formatContext->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
    if (!codec) { cleanup(); return; }

    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) { cleanup(); return; }
    
    if (avcodec_parameters_to_context(codecContext, audioStream->codecpar) < 0) { cleanup(); return; }
    if (avcodec_open2(codecContext, codec, nullptr) < 0) { cleanup(); return; }

    // Store the source (file) sample rate
    sourceSampleRate = (double)codecContext->sample_rate;
    
    // Determine output sample rate: use target if specified, otherwise use source
    double outputSampleRate = (targetRate > 0.0) ? targetRate : sourceSampleRate;
    
    // Set up the JUCE AudioFormatReader properties
    this->sampleRate = outputSampleRate;  // This is what we'll output
    this->numChannels = (unsigned int)codecContext->ch_layout.nb_channels;
    this->bitsPerSample = 32;
    this->usesFloatingPointData = true; // We will provide float data directly

    // Calculate total audio duration in samples (at OUTPUT sample rate)
    if (audioStream->duration != AV_NOPTS_VALUE) {
        double durationInSeconds = (double)audioStream->duration * av_q2d(audioStream->time_base);
        this->lengthInSamples = (juce::int64)(durationInSeconds * outputSampleRate);
    }

    // Configure the resampler to convert FFmpeg's decoded audio format to 32-bit float
    // AND resample from source rate to output rate
    resamplerContext = swr_alloc();
    if (!resamplerContext) { cleanup(); return; }
    
    av_opt_set_chlayout(resamplerContext, "in_chlayout", &codecContext->ch_layout, 0);
    av_opt_set_int(resamplerContext, "in_sample_rate", codecContext->sample_rate, 0);
    av_opt_set_sample_fmt(resamplerContext, "in_sample_fmt", codecContext->sample_fmt, 0);
    av_opt_set_chlayout(resamplerContext, "out_chlayout", &codecContext->ch_layout, 0);
    av_opt_set_int(resamplerContext, "out_sample_rate", (int)outputSampleRate, 0);  // Output at target rate
    av_opt_set_sample_fmt(resamplerContext, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);  // Planar float for JUCE
    
    if (swr_init(resamplerContext) < 0) { cleanup(); return; }
    
    // Log if resampling is active
    if (std::abs(sourceSampleRate - outputSampleRate) > 1.0)
    {
        juce::Logger::writeToLog("[FFmpegAudioReader] Resampling: " + 
                                 juce::String(sourceSampleRate) + "Hz -> " + 
                                 juce::String(outputSampleRate) + "Hz");
    }

    decodedFrame = av_frame_alloc();
    packet = av_packet_alloc();
    currentPositionInSamples = 0; // Initialize position tracker
    leftoverSamples = 0;
    leftoverBuffer.setSize((int)this->numChannels, 8192); // Initial size for leftover samples
    isInitialized = (decodedFrame && packet);
}

FFmpegAudioReader::~FFmpegAudioReader() {
    cleanup();
}

void FFmpegAudioReader::cleanup() {
    if (resamplerContext) swr_free(&resamplerContext);
    if (decodedFrame) av_frame_free(&decodedFrame);
    if (packet) av_packet_free(&packet);
    if (codecContext) avcodec_free_context(&codecContext);
    if (formatContext) avformat_close_input(&formatContext);
}

bool FFmpegAudioReader::readSamples(int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer, juce::int64 startSampleInFile, int numSamples)
{
    if (!isInitialized || numSamples <= 0) return true;

    // Correctly interpret the destination buffer as float** since usesFloatingPointData is true.
    auto* floatDestSamples = reinterpret_cast<float* const*>(destSamples);
    
    // Clear destination buffers to ensure silence if we fail to read
    for (int i = 0; i < numDestChannels; ++i) {
        if (floatDestSamples[i] != nullptr) {
            juce::FloatVectorOperations::clear(floatDestSamples[i] + startOffsetInDestBuffer, numSamples);
        }
    }

    // Only seek if the requested position differs significantly from current position
    // This allows sequential reading without expensive seeks on every call
    const juce::int64 seekThreshold = 1000; // Only seek if difference is > 1000 samples
    bool didSeek = false;
    if (std::abs(startSampleInFile - currentPositionInSamples) > seekThreshold)
    {
        // Convert output sample position to source sample position for seeking
        // (startSampleInFile is in output rate, but we need to seek in source rate)
        double resampleRatio = (sourceSampleRate > 0.0) ? (sourceSampleRate / sampleRate) : 1.0;
        juce::int64 sourceSamplePos = (juce::int64)(startSampleInFile * resampleRatio);
        
        int64_t targetTimestamp = (int64_t)((double)sourceSamplePos / sourceSampleRate / av_q2d(audioStream->time_base));
        if (av_seek_frame(formatContext, streamIndex, targetTimestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codecContext);
        
        // Clear leftover buffer on seek (old samples are no longer valid)
        leftoverSamples = 0;
        didSeek = true;
    }
    currentPositionInSamples = startSampleInFile;

    int samplesWritten = 0;
    
    // First, use any leftover samples from previous call
    if (leftoverSamples > 0 && !didSeek)
    {
        int samplesToUse = std::min(leftoverSamples, numSamples);
        for (int ch = 0; ch < std::min((int)this->numChannels, numDestChannels); ++ch) {
            if (floatDestSamples[ch] != nullptr) {
                juce::FloatVectorOperations::copy(floatDestSamples[ch] + startOffsetInDestBuffer,
                                                  leftoverBuffer.getReadPointer(ch),
                                                  samplesToUse);
            }
        }
        samplesWritten = samplesToUse;
        
        // Shift remaining leftover samples to beginning of buffer
        if (leftoverSamples > samplesToUse)
        {
            int remaining = leftoverSamples - samplesToUse;
            for (int ch = 0; ch < (int)this->numChannels; ++ch) {
                juce::FloatVectorOperations::copy(leftoverBuffer.getWritePointer(ch),
                                                  leftoverBuffer.getReadPointer(ch) + samplesToUse,
                                                  remaining);
            }
            leftoverSamples = remaining;
        }
        else
        {
            leftoverSamples = 0;
        }
    }
    
    // Now decode more samples if needed
    while (samplesWritten < numSamples)
    {
        if (av_read_frame(formatContext, packet) < 0) break; // End of file

        if (packet->stream_index == streamIndex)
        {
            if (avcodec_send_packet(codecContext, packet) == 0)
            {
                while (avcodec_receive_frame(codecContext, decodedFrame) == 0)
                {
                    int maxOutSamples = (int)av_rescale_rnd(decodedFrame->nb_samples, (int)this->sampleRate, codecContext->sample_rate, AV_ROUND_UP);
                    tempResampledBuffer.setSize((int)this->numChannels, maxOutSamples, false, false, true);
                    
                    // Prepare output pointers for planar format (one pointer per channel)
                    uint8_t* outData[64];  // FFmpeg supports up to 64 channels
                    for (int ch = 0; ch < (int)this->numChannels; ++ch) {
                        outData[ch] = (uint8_t*)tempResampledBuffer.getWritePointer(ch);
                    }
                    
                    int samplesConverted = swr_convert(resamplerContext, outData, maxOutSamples, (const uint8_t**)decodedFrame->extended_data, decodedFrame->nb_samples);
                    
                    if (samplesConverted > 0)
                    {
                        int samplesNeeded = numSamples - samplesWritten;
                        int samplesToCopy = std::min(samplesConverted, samplesNeeded);
                        
                        // Copy what we need to destination
                        for (int ch = 0; ch < std::min((int)this->numChannels, numDestChannels); ++ch) {
                            if (floatDestSamples[ch] != nullptr) {
                                juce::FloatVectorOperations::copy(floatDestSamples[ch] + startOffsetInDestBuffer + samplesWritten,
                                                                  tempResampledBuffer.getReadPointer(ch),
                                                                  samplesToCopy);
                            }
                        }
                        samplesWritten += samplesToCopy;
                        
                        // Save any extra samples to leftover buffer for next call
                        int extraSamples = samplesConverted - samplesToCopy;
                        if (extraSamples > 0)
                        {
                            // Ensure leftover buffer is big enough
                            if (leftoverBuffer.getNumSamples() < extraSamples + leftoverSamples)
                            {
                                leftoverBuffer.setSize((int)this->numChannels, extraSamples + leftoverSamples + 1024, true, true, true);
                            }
                            
                            // Append extra samples to leftover buffer
                            for (int ch = 0; ch < (int)this->numChannels; ++ch) {
                                juce::FloatVectorOperations::copy(leftoverBuffer.getWritePointer(ch) + leftoverSamples,
                                                                  tempResampledBuffer.getReadPointer(ch) + samplesToCopy,
                                                                  extraSamples);
                            }
                            leftoverSamples += extraSamples;
                        }
                    }
                }
            }
        }
        av_packet_unref(packet);
        if (samplesWritten >= numSamples) break;
    }
    
    // Update current position after successful read
    currentPositionInSamples = startSampleInFile + samplesWritten;

    return true;
}
