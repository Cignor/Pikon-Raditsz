#pragma once

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <juce_core/juce_core.h>
#include <map>

/**
 * Thread-safe singleton for sharing video frames between source and processing nodes.
 * Source nodes publish frames, processing nodes consume them.
 */
class VideoFrameManager
{
public:
    static VideoFrameManager& getInstance()
    {
        static VideoFrameManager instance;
        return instance;
    }

    // Called by source node's background thread to publish a frame
    void setFrame(juce::uint32 sourceId, const cv::Mat& frame)
    {
        const juce::ScopedLock lock(frameMapLock);
        if (!frame.empty())
        {
            frame.copyTo(frameMap[sourceId]);
#if defined(WITH_CUDA_SUPPORT)
            // Invalidate GPU cache for this ID because CPU frame is newer
            gpuFrameMap.erase(sourceId);
#endif
        }
    }

#if defined(WITH_CUDA_SUPPORT)
    // Called by GPU-enabled source nodes (e.g. VideoFX, Chromakey output)
    void setGpuFrame(juce::uint32 sourceId, const cv::cuda::GpuMat& frame)
    {
        const juce::ScopedLock lock(frameMapLock);
        if (!frame.empty())
        {
            try
            {
                // Use copyTo to avoid reallocation if possible
                if (gpuFrameMap[sourceId].size() != frame.size() ||
                    gpuFrameMap[sourceId].type() != frame.type())
                    gpuFrameMap[sourceId] = cv::cuda::GpuMat(frame.size(), frame.type());

                frame.copyTo(gpuFrameMap[sourceId]);

                // Invalidate CPU cache because GPU frame is newer
                // (Lazy download: we only download if getFrame is called)
                frameMap.erase(sourceId);
            }
            catch (const cv::Exception&)
            {
                // Silently ignore GPU errors - fall back to CPU path
            }
        }
    }

    // Called by GPU-enabled processing nodes to get input
    cv::cuda::GpuMat getGpuFrame(juce::uint32 sourceId)
    {
        const juce::ScopedLock lock(frameMapLock);

        try
        {
            // 1. Try to find existing GPU frame
            auto itGpu = gpuFrameMap.find(sourceId);
            if (itGpu != gpuFrameMap.end() && !itGpu->second.empty())
            {
                return itGpu->second.clone();
            }

            // 2. If no GPU frame, check CPU frame map and upload
            auto itCpu = frameMap.find(sourceId);
            if (itCpu != frameMap.end() && !itCpu->second.empty())
            {
                // Upload to GPU cache
                gpuFrameMap[sourceId].upload(itCpu->second);
                return gpuFrameMap[sourceId].clone();
            }
        }
        catch (const cv::Exception&)
        {
            // Silently ignore GPU errors
        }

        return cv::cuda::GpuMat();
    }
#endif

    // Called by processing node's background thread to retrieve a frame (CPU)
    cv::Mat getFrame(juce::uint32 sourceId)
    {
        const juce::ScopedLock lock(frameMapLock);

        // 1. Try to find CPU frame
        auto it = frameMap.find(sourceId);
        if (it != frameMap.end() && !it->second.empty())
        {
            return it->second.clone();
        }

#if defined(WITH_CUDA_SUPPORT)
        // 2. If no CPU frame, check GPU and download
        try
        {
            auto itGpu = gpuFrameMap.find(sourceId);
            if (itGpu != gpuFrameMap.end() && !itGpu->second.empty())
            {
                // Download to CPU cache
                cv::Mat cpuFrame;
                itGpu->second.download(cpuFrame);
                frameMap[sourceId] = cpuFrame;
                return cpuFrame.clone();
            }
        }
        catch (const cv::Exception&)
        {
            // Silently ignore GPU errors
        }
#endif

        return cv::Mat();
    }

    // Called when a source node is deleted
    void removeSource(juce::uint32 sourceId)
    {
        const juce::ScopedLock lock(frameMapLock);
        frameMap.erase(sourceId);
#if defined(WITH_CUDA_SUPPORT)
        gpuFrameMap.erase(sourceId);
#endif
    }

    // For UI: get list of active sources
    juce::StringArray getAvailableSources()
    {
        const juce::ScopedLock lock(frameMapLock);
        juce::StringArray      sources;
        // Merge keys from both maps
        for (const auto& pair : frameMap)
            sources.add(juce::String((int)pair.first));

#if defined(WITH_CUDA_SUPPORT)
        for (const auto& pair : gpuFrameMap)
        {
            juce::String idStr((int)pair.first);
            if (!sources.contains(idStr))
                sources.add(idStr);
        }
#endif
        return sources;
    }

private:
    VideoFrameManager() = default;
    ~VideoFrameManager() = default;
    VideoFrameManager(const VideoFrameManager&) = delete;
    VideoFrameManager& operator=(const VideoFrameManager&) = delete;

    std::map<juce::uint32, cv::Mat> frameMap;
#if defined(WITH_CUDA_SUPPORT)
    // Add include for cuda if needed, but header includes opencv2/core.hpp.
    // GpuMat is in core/cuda_types.hpp or similar.
    // We already included <opencv2/core.hpp> which might not have GpuMat fwd decl.
    // We need <opencv2/core/cuda.hpp> usually.
    std::map<juce::uint32, cv::cuda::GpuMat> gpuFrameMap;
#endif
    juce::CriticalSection frameMapLock;
};
