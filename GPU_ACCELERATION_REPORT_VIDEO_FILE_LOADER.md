# GPU Acceleration Report: VideoFileLoaderModule

## Executive Summary

This report analyzes the requirements and feasibility of adding GPU acceleration to `VideoFileLoaderModule`, following the pattern established in `VideoFXModule`.

**Current Status**: CPU-only implementation  
**Target**: Limited GPU acceleration (primarily color conversion)  
**Estimated Complexity**: Low-Medium  
**Performance Impact**: Minimal (I/O-bound operations dominate)

**Key Finding**: `VideoFileLoaderModule` is primarily an **I/O-bound source module** with minimal image processing. GPU acceleration would provide **limited benefits** compared to other modules.

---

## 1. Current Implementation Analysis

### 1.1 Module Purpose

`VideoFileLoaderModule` is a **source node** that:
- Loads video files from disk using `cv::VideoCapture`
- Decodes video frames using FFmpeg/OpenCV codecs
- Extracts and processes audio tracks
- Publishes video frames to `VideoFrameManager` for downstream processing
- Provides timeline synchronization and playback control

### 1.2 CPU Operations Performed

| Operation | Current Implementation | Frequency | GPU Benefit |
|-----------|----------------------|-----------|-------------|
| **Video Decoding** | `cv::VideoCapture.read()` | Every frame (~30 FPS) | **None** - Handled by codec (may already use hardware) |
| **Color Conversion** | `cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA)` | Every frame | **Low-Medium** - Simple conversion |
| **Frame Copying** | `memcpy()` to JUCE Image | Every frame | **None** - Memory copy, not compute-bound |
| **Audio Processing** | FFmpeg + TimePitchProcessor | Continuous | **None** - Separate audio pipeline |

### 1.3 Key Functions

**`run()` - Main Thread Loop:**
- Reads frames from `cv::VideoCapture` (I/O bound)
- Handles seeking, looping, synchronization
- Calls `updateGuiFrame()` for UI preview

**`updateGuiFrame()` - UI Frame Update:**
- Converts BGR → BGRA: `cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA)`
- Copies to JUCE Image: `memcpy()`

**`processBlock()` - Audio Thread:**
- Reads source IDs from input pins
- Outputs logical ID for routing
- Processes audio (separate from video)

---

## 2. GPU Acceleration Opportunities

### 2.1 Primary Opportunity: Color Conversion

**Current CPU Implementation:**
```cpp
void VideoFileLoaderModule::updateGuiFrame(const cv::Mat& frame)
{
    cv::Mat bgraFrame;
    cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);  // CPU conversion
    
    // ... copy to JUCE Image
}
```

**GPU Implementation:**
```cpp
void VideoFileLoaderModule::updateGuiFrame_gpu(const cv::Mat& frame)
{
#if defined(WITH_CUDA_SUPPORT)
    if (useGpu && gpuAvailable) {
        // Upload to GPU
        gpuFrame.upload(frame);
        
        // Convert on GPU
        cv::cuda::cvtColor(gpuFrame, gpuBgraFrame, cv::COLOR_BGR2BGRA);
        
        // Download result
        gpuBgraFrame.download(bgraFrame);
    } else {
        // CPU fallback
        cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);
    }
#else
    cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);
#endif
}
```

**Performance Impact**: 
- CPU: ~0.5-1ms per frame (1080p)
- GPU: ~0.1-0.2ms per frame (1080p) + upload/download overhead
- **Net Benefit**: ~0.3-0.7ms per frame (minimal)

### 2.2 Secondary Opportunity: Hardware-Accelerated Decoding

**Note**: Video decoding is handled by FFmpeg/OpenCV codecs, which may already use hardware acceleration if:
- GPU supports hardware video decoding (NVENC/NVDEC, QuickSync, VDPAU, etc.)
- Codec is configured to use hardware acceleration
- This is **outside the scope** of this module's code

**Current Status**: OpenCV's `cv::VideoCapture` may use hardware acceleration automatically if available, but this is codec-dependent and not controlled by the module.

---

## 3. Implementation Requirements

### 3.1 Header File Changes (`VideoFileLoaderModule.h`)

**Add GPU Parameter:**
```cpp
juce::AudioParameterBool* useGpuParam = nullptr;
```

**Add GPU Buffers:**
```cpp
#if defined(WITH_CUDA_SUPPORT)
cv::cuda::GpuMat gpuFrame;        // Input frame (BGR)
cv::cuda::GpuMat gpuBgraFrame;   // Output frame (BGRA)
#endif
```

**Add GPU Function Declaration:**
```cpp
#if defined(WITH_CUDA_SUPPORT)
void updateGuiFrame_gpu(const cv::Mat& frame);
#endif
```

### 3.2 Implementation File Changes (`VideoFileLoaderModule.cpp`)

#### 3.2.1 Parameter Layout

**Add GPU toggle parameter:**
```cpp
params.push_back(std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", false));
```

**Note**: Default to `false` because benefits are minimal, and GPU adds complexity/overhead.

#### 3.2.2 Constructor

**Initialize GPU parameter:**
```cpp
useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));
```

#### 3.2.3 Main Loop (`run()`)

**Minimal changes needed:**
```cpp
cv::Mat frame;
if (videoCapture.read(frame))
{
    if (myLogicalId != 0)
        VideoFrameManager::getInstance().setFrame(myLogicalId, frame);
    
    // Use GPU version if enabled
    const bool useGpu = useGpuParam ? useGpuParam->get() : false;
#if defined(WITH_CUDA_SUPPORT)
    const bool gpuAvailable = CudaDeviceCountCache::isAvailable();
    if (useGpu && gpuAvailable) {
        updateGuiFrame_gpu(frame);
    } else {
        updateGuiFrame(frame);
    }
#else
    updateGuiFrame(frame);
#endif
}
```

#### 3.2.4 GPU Color Conversion

**Implementation:**
```cpp
#if defined(WITH_CUDA_SUPPORT)
void VideoFileLoaderModule::updateGuiFrame_gpu(const cv::Mat& frame)
{
    try {
        // Upload to GPU
        gpuFrame.upload(frame);
        
        // Convert BGR to BGRA on GPU
        cv::cuda::cvtColor(gpuFrame, gpuBgraFrame, cv::COLOR_BGR2BGRA);
        
        // Download result
        cv::Mat bgraFrame;
        gpuBgraFrame.download(bgraFrame);
        
        // Copy to JUCE Image (still CPU operation)
        const juce::ScopedLock lock(imageLock);
        
        if (latestFrameForGui.isNull() || 
            latestFrameForGui.getWidth() != bgraFrame.cols ||
            latestFrameForGui.getHeight() != bgraFrame.rows)
        {
            latestFrameForGui = juce::Image(juce::Image::ARGB, bgraFrame.cols, bgraFrame.rows, true);
        }
        
        juce::Image::BitmapData destData(latestFrameForGui, juce::Image::BitmapData::writeOnly);
        memcpy(destData.data, bgraFrame.data, bgraFrame.total() * bgraFrame.elemSize());
    }
    catch (const cv::Exception& e)
    {
        juce::Logger::writeToLog("[VideoFileLoader] GPU Error: " + 
                                 juce::String(e.what()) + ". Falling back to CPU.");
        // Fallback to CPU
        updateGuiFrame(frame);
    }
}
#endif
```

#### 3.2.5 UI Integration

**Add GPU checkbox:**
```cpp
#if defined(PRESET_CREATOR_UI)
bool useGpu = useGpuParam ? useGpuParam->get() : false;
#if !defined(WITH_CUDA_SUPPORT)
ImGui::BeginDisabled();
useGpu = false;
#endif
if (ImGui::Checkbox("Use GPU (Color Conversion)", &useGpu))
{
    if (useGpuParam) *useGpuParam = useGpu;
    onModificationEnded();
}
#if !defined(WITH_CUDA_SUPPORT)
if (ImGui::IsItemHovered()) 
    ImGui::SetTooltip("CUDA support was not compiled.");
ImGui::EndDisabled();
#else
if (ImGui::IsItemHovered()) 
    ImGui::SetTooltip("Accelerates BGR→BGRA color conversion for UI preview.\nMinimal performance impact.");
#endif
#endif
```

---

## 4. Performance Analysis

### 4.1 Current Performance Profile

**Frame Processing Pipeline (1080p @ 30 FPS):**
1. **Video Decode**: ~25-30ms (I/O + codec) - **Dominant**
2. **Color Conversion**: ~0.5-1ms (CPU)
3. **Memory Copy**: ~0.1-0.2ms
4. **Frame Publish**: ~0.1ms

**Total per frame**: ~26-31ms (mostly I/O bound)

### 4.2 GPU Acceleration Impact

**With GPU Color Conversion:**
1. **Video Decode**: ~25-30ms (unchanged - I/O bound)
2. **Upload to GPU**: ~0.5-1ms (PCIe overhead)
3. **Color Conversion**: ~0.1-0.2ms (GPU)
4. **Download from GPU**: ~0.5-1ms (PCIe overhead)
5. **Memory Copy**: ~0.1-0.2ms

**Total per frame**: ~26-32ms (potentially **slower** due to upload/download overhead)

### 4.3 Performance Conclusion

**GPU acceleration provides minimal or negative benefit** because:
1. **I/O bound**: Video decoding dominates processing time
2. **Upload/download overhead**: PCIe transfer time may exceed conversion time
3. **Simple operation**: Color conversion is already fast on CPU
4. **Memory bandwidth**: Copying to JUCE Image still requires CPU memory

**Recommendation**: GPU acceleration is **not recommended** for this module unless:
- Future enhancements add more image processing (resizing, effects, etc.)
- Multiple color conversions are needed per frame
- Higher resolution processing (4K+) is required

---

## 5. Alternative Optimization Strategies

### 5.1 Hardware-Accelerated Video Decoding

**Better Approach**: Ensure FFmpeg/OpenCV uses hardware video decoding if available.

**Implementation**: This is typically handled automatically by the codec, but can be verified:
```cpp
// Check if hardware acceleration is available
cv::VideoCapture cap;
cap.open(filePath, cv::CAP_FFMPEG);
// Hardware acceleration is codec-dependent and may be automatic
```

**Benefit**: Could reduce decode time from 25-30ms to 5-10ms (if hardware decoder available).

### 5.2 Async Frame Processing

**Current**: Frame decode → convert → copy happens sequentially.

**Optimization**: Overlap operations using async processing:
- Decode next frame while converting current frame
- Use separate threads for decode vs. conversion

**Benefit**: Could reduce perceived latency, but adds complexity.

### 5.3 Direct GPU Memory Access

**Advanced**: If downstream modules use GPU, keep frames on GPU:
- Skip CPU download
- Publish GPU frames directly to `VideoFrameManager`
- Requires changes to `VideoFrameManager` API

**Benefit**: Eliminates upload/download overhead for GPU pipeline.

---

## 6. Implementation Checklist

### Phase 1: Basic GPU Support (Optional)
- [ ] Add GPU toggle parameter (default: false)
- [ ] Add GPU buffer member variables
- [ ] Implement `updateGuiFrame_gpu()`
- [ ] Add CUDA includes
- [ ] Add error handling and CPU fallback
- [ ] Add UI checkbox

### Phase 2: Testing
- [ ] Performance benchmarking (CPU vs GPU)
- [ ] Verify color conversion correctness
- [ ] Test error handling
- [ ] Verify no memory leaks

### Phase 3: Documentation
- [ ] Document minimal performance benefit
- [ ] Add comments explaining why GPU is optional
- [ ] Update user documentation

---

## 7. Code Size Estimate

**New Code Required:**
- Header additions: ~15 lines
- GPU buffer initialization: ~5 lines
- `updateGuiFrame_gpu()`: ~40 lines
- Main loop integration: ~10 lines
- UI checkbox: ~15 lines
- Parameter layout: ~2 lines

**Total**: ~87 lines of new code

---

## 8. Recommendations

### Priority: **Low**

GPU acceleration for `VideoFileLoaderModule` provides **minimal benefit** because:
1. Module is I/O-bound (video decoding dominates)
2. Only operation is simple color conversion (already fast)
3. Upload/download overhead may negate benefits
4. Memory copy to JUCE Image still requires CPU

### Implementation Approach: **Optional/Deferred**

1. **Don't implement GPU acceleration** unless:
   - Future enhancements add image processing (resizing, effects)
   - User specifically requests it
   - Downstream modules require GPU frames

2. **Focus optimization efforts on**:
   - Hardware-accelerated video decoding (codec-level)
   - Async frame processing
   - Direct GPU memory access (if downstream modules use GPU)

3. **If implementing**, keep it simple:
   - Single GPU function for color conversion
   - Default to CPU (set `useGpu` default to `false`)
   - Minimal UI presence (optional checkbox)

### Future Considerations

If `VideoFileLoaderModule` gains additional features:
- **Frame resizing** (for zoom levels): GPU would help
- **Frame effects** (preview filters): GPU would help
- **Multi-format conversion**: GPU would help
- **Frame caching**: GPU memory could be beneficial

---

## 9. Comparison with Other Modules

| Module | GPU Benefit | Complexity | Recommendation |
|--------|-------------|------------|---------------|
| **VideoFXModule** | **High** - Many effects | High | ✅ Implemented |
| **VideoCompositorModule** | **High** - Multi-layer blending | High | ✅ Recommended |
| **VideoFileLoaderModule** | **Low** - Simple conversion | Low | ❌ Not Recommended |

---

## 10. Conclusion

**GPU acceleration for `VideoFileLoaderModule` is not recommended** at this time because:

1. **Minimal benefit**: Only operation is simple color conversion
2. **I/O bound**: Video decoding dominates processing time
3. **Overhead**: Upload/download may negate benefits
4. **Complexity**: Adds code complexity for little gain

**Better optimization targets**:
- Hardware-accelerated video decoding (codec-level)
- Async frame processing
- Direct GPU memory access (if downstream uses GPU)

**If implementing anyway** (for consistency or future-proofing):
- Keep implementation simple
- Default to CPU (`useGpu = false`)
- Document minimal benefit
- Make it optional/experimental

**Estimated Implementation Time**: 2-3 hours (if proceeding)

**Risk Level**: Low (simple operation, easy to test)

**Recommendation**: **Defer implementation** unless specific use case requires it.
