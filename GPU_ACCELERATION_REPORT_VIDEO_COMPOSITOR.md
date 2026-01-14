# GPU Acceleration Report: VideoCompositorModule

## Executive Summary

This report analyzes the requirements and implementation plan for adding full GPU acceleration to `VideoCompositorModule`, following the pattern established in `VideoFXModule`.

**Current Status**: CPU-only implementation  
**Target**: Full GPU acceleration using CUDA/OpenCV GPU modules  
**Estimated Complexity**: High (multi-layer compositing with complex blend modes)  
**Performance Impact**: Significant (especially with 8 layers at high resolution)

---

## 1. Current Implementation Analysis

### 1.1 CPU Implementation Overview

The current `VideoCompositorModule` performs the following operations on CPU:

1. **Frame Fetching**: Retrieves frames from `VideoFrameManager` for each layer
2. **Transform Application**: 
   - Scaling (`cv::resize`)
   - Positioning (manual copy with clipping)
3. **Blend Mode Application**: 
   - 19 different blend modes
   - Alpha blending with opacity
   - Float precision calculations
4. **Layer Compositing**: Iterates through layers bottom-to-top, compositing each onto canvas

### 1.2 Key Functions Requiring GPU Port

| Function | Current Implementation | GPU Complexity |
|----------|----------------------|----------------|
| `applyTransforms()` | `cv::resize()` + manual copy with clipping | **Medium** - OpenCV CUDA has `cv::cuda::resize()` |
| `applyBlendMode()` | 19 blend modes using `cv::Mat` float operations | **High** - Complex math operations, some may need custom kernels |
| `compositeLayer()` | Orchestrates transform + blend | **Low** - Just calls GPU versions |
| `run()` | Main loop, frame fetching | **Low** - Upload/download management |

---

## 2. GPU Implementation Pattern (from VideoFXModule)

### 2.1 Architecture Pattern

```cpp
// 1. GPU toggle parameter
juce::AudioParameterBool* useGpuParam;

// 2. GPU buffers (member variables)
#if defined(WITH_CUDA_SUPPORT)
cv::cuda::GpuMat gpuFrame;
cv::cuda::GpuMat gpuTemp;
// ... additional buffers as needed
#endif

// 3. GPU function signatures
#if defined(WITH_CUDA_SUPPORT)
void applyBlendMode_gpu(cv::cuda::GpuMat& dst, const cv::cuda::GpuMat& src, 
                        BlendMode mode, float opacity);
cv::cuda::GpuMat applyTransforms_gpu(const cv::cuda::GpuMat& src, ...);
#endif

// 4. Main loop pattern
if (runOnGpu) {
    gpuFrame.upload(frame);
    // ... GPU processing
    gpuFrame.download(processedFrame);
} else {
    // CPU fallback
}
```

### 2.2 Required Includes

```cpp
#if defined(WITH_CUDA_SUPPORT)
#include <opencv2/cudaimgproc.hpp>  // Color conversions, resize
#include <opencv2/cudaarithm.hpp>   // Arithmetic operations
#include <opencv2/cudafilters.hpp>   // Filters (if needed)
#include <opencv2/cudawarping.hpp>   // Resize, transforms
#include "../../utils/CudaDeviceCountCache.h"
#endif
```

---

## 3. Implementation Requirements

### 3.1 Header File Changes (`VideoCompositorModule.h`)

**Add GPU Parameter:**
```cpp
juce::AudioParameterBool* useGpuParam = nullptr;
```

**Add GPU Buffers:**
```cpp
#if defined(WITH_CUDA_SUPPORT)
// Main canvas buffer
cv::cuda::GpuMat gpuCanvas;

// Temporary buffers for transforms
cv::cuda::GpuMat gpuTemp;           // 8-bit, 3-channel
cv::cuda::GpuMat gpuScaled;         // Scaled layer
cv::cuda::GpuMat gpuTransformed;    // Transformed layer

// Buffers for blend mode calculations
cv::cuda::GpuMat gpuDstF;           // Float destination
cv::cuda::GpuMat gpuSrcF;           // Float source
cv::cuda::GpuMat gpuResultF;        // Float result
cv::cuda::GpuMat gpuMask;           // Mask for conditional operations

// Channel split buffers
std::vector<cv::cuda::GpuMat> gpuChannels; // BGR channels

// Per-layer frame buffers (up to 8 layers)
std::vector<cv::cuda::GpuMat> gpuLayerFrames;
#endif
```

**Add GPU Function Declarations:**
```cpp
#if defined(WITH_CUDA_SUPPORT)
void compositeLayer_gpu(cv::cuda::GpuMat& canvas, const cv::cuda::GpuMat& layer, 
                       BlendMode mode, float opacity, float posX, float posY, 
                       float scaleX, float scaleY);
cv::cuda::GpuMat applyTransforms_gpu(const cv::cuda::GpuMat& src, float posX, 
                                     float posY, float scaleX, float scaleY, 
                                     int canvasWidth, int canvasHeight);
void applyBlendMode_gpu(cv::cuda::GpuMat& dst, const cv::cuda::GpuMat& src, 
                        BlendMode mode, float opacity);
#endif
```

### 3.2 Implementation File Changes (`VideoCompositorModule.cpp`)

#### 3.2.1 Parameter Layout

**Add GPU toggle parameter:**
```cpp
params.push_back(std::make_unique<juce::AudioParameterBool>("useGpu", "Use GPU (CUDA)", true));
```

#### 3.2.2 Constructor

**Initialize GPU parameter:**
```cpp
useGpuParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("useGpu"));
```

**Initialize GPU buffers:**
```cpp
#if defined(WITH_CUDA_SUPPORT)
gpuLayerFrames.resize(MAX_LAYERS);
gpuChannels.resize(3);
#endif
```

#### 3.2.3 Main Loop (`run()`)

**GPU/CPU path selection:**
```cpp
const bool useGpu = useGpuParam ? useGpuParam->get() : false;
#if defined(WITH_CUDA_SUPPORT)
const bool gpuAvailable = CudaDeviceCountCache::isAvailable();
#else
const bool gpuAvailable = false;
#endif

const bool runOnGpu = (useGpu && gpuAvailable);

if (runOnGpu) {
#if defined(WITH_CUDA_SUPPORT)
    try {
        // Upload all layer frames to GPU
        for (int i = 0; i < numLayersClamped; ++i) {
            if (!frames[i].empty()) {
                gpuLayerFrames[i].upload(frames[i]);
            }
        }
        
        // Initialize GPU canvas
        if (canvas.empty()) {
            // Determine canvas size from first layer
            int canvasW = frames[0].cols;
            int canvasH = frames[0].rows;
            gpuCanvas = cv::cuda::GpuMat::zeros(canvasH, canvasW, CV_8UC3);
        } else {
            gpuCanvas.upload(canvas);
        }
        
        // Composite layers on GPU
        for (int i = 0; i < numLayersClamped; ++i) {
            if (!gpuLayerFrames[i].empty()) {
                // Get parameters...
                compositeLayer_gpu(gpuCanvas, gpuLayerFrames[i], blendMode, 
                                  opacity, posX, posY, scaleX, scaleY);
            }
        }
        
        // Download result
        gpuCanvas.download(canvas);
    }
    catch (const cv::Exception& e) {
        juce::Logger::writeToLog("[VideoCompositor] GPU Error: " + 
                                 juce::String(e.what()) + ". Falling back to CPU.");
        // Fallback to CPU path
        goto cpu_path_label;
    }
#endif
} else {
cpu_path_label:
    // Existing CPU implementation
}
```

---

## 4. GPU Function Implementations

### 4.1 `applyTransforms_gpu()` - Complexity: **Medium**

**Challenges:**
- OpenCV CUDA has `cv::cuda::resize()` ✓
- Positioning/clipping requires manual implementation
- May need to use `cv::cuda::copyTo()` with masks

**Implementation Strategy:**
```cpp
cv::cuda::GpuMat VideoCompositorModule::applyTransforms_gpu(...)
{
    // 1. Resize on GPU
    cv::cuda::resize(src, gpuScaled, cv::Size(scaledWidth, scaledHeight), 
                     0, 0, cv::INTER_LINEAR);
    
    // 2. Create output canvas
    gpuTransformed = cv::cuda::GpuMat::zeros(canvasHeight, canvasWidth, src.type());
    
    // 3. Calculate source/dest regions (same clipping logic as CPU)
    
    // 4. Copy with clipping using cv::cuda::copyTo() or manual ROI
    // Note: May need custom kernel for complex clipping scenarios
    
    return gpuTransformed;
}
```

**OpenCV CUDA Functions Available:**
- `cv::cuda::resize()` ✓
- `cv::cuda::copyTo()` ✓ (with mask support)
- ROI operations via `cv::cuda::GpuMat(original, cv::Rect(...))` ✓

### 4.2 `applyBlendMode_gpu()` - Complexity: **High**

**Challenges:**
- 19 different blend modes
- Some require complex conditional logic (masks)
- Float precision operations
- Opacity blending

**Blend Mode Categories:**

| Category | Blend Modes | GPU Complexity |
|----------|-------------|----------------|
| **Simple Arithmetic** | Add, Multiply, Screen, LinearDodge, LinearBurn | **Low** - Direct CUDA arithmetic |
| **Conditional** | Overlay, SoftLight, HardLight, PinLight | **Medium** - Requires masks |
| **Division-based** | ColorDodge, ColorBurn, VividLight | **Medium** - `cv::cuda::divide()` |
| **Min/Max** | Darken, Lighten | **Low** - `cv::cuda::min()`, `cv::cuda::max()` |
| **Complex Math** | Difference, Exclusion, HardMix | **Medium** - Multiple operations |

**Implementation Pattern:**
```cpp
void VideoCompositorModule::applyBlendMode_gpu(cv::cuda::GpuMat& dst, 
                                               const cv::cuda::GpuMat& src, 
                                               BlendMode mode, float opacity)
{
    // Resize src to match dst if needed
    if (dst.size() != src.size()) {
        cv::cuda::resize(src, gpuTemp, dst.size(), 0, 0, cv::INTER_LINEAR);
        // Use gpuTemp instead of src
    }
    
    // Convert to float
    dst.convertTo(gpuDstF, CV_32FC3, 1.0 / 255.0);
    src.convertTo(gpuSrcF, CV_32FC3, 1.0 / 255.0);
    
    gpuResultF = gpuDstF.clone();
    
    switch (mode) {
        case BlendMode::Normal:
            cv::cuda::addWeighted(gpuDstF, 1.0f - opacity, 
                                  gpuSrcF, opacity, 0.0, gpuResultF);
            break;
            
        case BlendMode::Add:
            cv::cuda::add(gpuDstF, gpuSrcF * opacity, gpuResultF);
            cv::cuda::min(gpuResultF, 1.0f, gpuResultF);
            break;
            
        case BlendMode::Multiply:
            cv::cuda::multiply(gpuDstF, gpuSrcF * opacity + (1.0f - opacity), 
                             gpuResultF);
            break;
            
        // ... other blend modes
        
        case BlendMode::Overlay:
            // Requires mask for conditional logic
            cv::cuda::compare(gpuDstF, 0.5f, gpuMask, cv::CMP_LT);
            // Apply different formulas based on mask
            break;
    }
    
    // Convert back to 8-bit
    gpuResultF.convertTo(dst, CV_8UC3, 255.0);
}
```

**OpenCV CUDA Functions Available:**
- `cv::cuda::addWeighted()` ✓
- `cv::cuda::add()`, `cv::cuda::subtract()`, `cv::cuda::multiply()`, `cv::cuda::divide()` ✓
- `cv::cuda::min()`, `cv::cuda::max()` ✓
- `cv::cuda::compare()` ✓ (for masks)
- `cv::cuda::abs()` ✓
- `cv::cuda::bitwise_and()`, `cv::cuda::bitwise_not()` ✓

**Potential Issues:**
- Some blend modes (SoftLight, PinLight) have complex conditional logic that may require custom CUDA kernels
- Opacity blending needs to be integrated into each blend mode formula

### 4.3 `compositeLayer_gpu()` - Complexity: **Low**

**Implementation:**
```cpp
void VideoCompositorModule::compositeLayer_gpu(cv::cuda::GpuMat& canvas, ...)
{
    // Apply transforms
    cv::cuda::GpuMat transformed = applyTransforms_gpu(layer, ...);
    
    if (transformed.empty()) return;
    
    // Apply blend mode
    applyBlendMode_gpu(canvas, transformed, mode, opacity);
}
```

---

## 5. Performance Considerations

### 5.1 Memory Management

**Current CPU Implementation:**
- Frames stored as `cv::Mat` (CPU memory)
- Canvas created per frame
- ~30 FPS processing

**GPU Implementation:**
- Upload: CPU → GPU (PCIe bandwidth bottleneck)
- Processing: GPU (parallel, fast)
- Download: GPU → CPU (PCIe bandwidth bottleneck)

**Optimization Strategies:**
1. **Keep frames on GPU**: If source modules also use GPU, avoid upload/download
2. **Batch operations**: Process multiple layers in single GPU pass if possible
3. **Buffer reuse**: Reuse GPU buffers to avoid allocations
4. **Async operations**: Use CUDA streams for overlap (advanced)

### 5.2 Expected Performance Gains

| Scenario | CPU Time | GPU Time (Est.) | Speedup |
|----------|----------|-----------------|---------|
| 1 layer, 1080p | 33ms | ~5ms | **6.6x** |
| 4 layers, 1080p | 132ms | ~15ms | **8.8x** |
| 8 layers, 1080p | 264ms | ~25ms | **10.6x** |
| 8 layers, 4K | 1056ms | ~80ms | **13.2x** |

*Note: Estimates assume optimal GPU utilization. Actual performance depends on GPU model, PCIe bandwidth, and blend mode complexity.*

---

## 6. Implementation Checklist

### Phase 1: Infrastructure
- [ ] Add GPU toggle parameter to `createParameterLayout()`
- [ ] Initialize `useGpuParam` in constructor
- [ ] Add GPU buffer member variables to header
- [ ] Add GPU function declarations
- [ ] Add CUDA includes and CudaDeviceCountCache

### Phase 2: Transform Implementation
- [ ] Implement `applyTransforms_gpu()`
- [ ] Test scaling on GPU
- [ ] Test positioning/clipping on GPU
- [ ] Verify output matches CPU version

### Phase 3: Blend Mode Implementation (Incremental)
- [ ] Implement simple blend modes (Normal, Add, Multiply, Screen)
- [ ] Implement min/max blend modes (Darken, Lighten)
- [ ] Implement arithmetic blend modes (LinearDodge, LinearBurn, Difference, Exclusion)
- [ ] Implement division-based blend modes (ColorDodge, ColorBurn, VividLight)
- [ ] Implement conditional blend modes (Overlay, HardLight)
- [ ] Implement complex blend modes (SoftLight, PinLight, HardMix, LinearLight)
- [ ] Test each blend mode for correctness vs CPU

### Phase 4: Integration
- [ ] Implement `compositeLayer_gpu()`
- [ ] Integrate GPU path into `run()` loop
- [ ] Add error handling and CPU fallback
- [ ] Add UI checkbox for GPU toggle

### Phase 5: Testing & Optimization
- [ ] Performance benchmarking (CPU vs GPU)
- [ ] Memory leak testing
- [ ] Multi-layer stress testing
- [ ] Edge case testing (empty frames, size mismatches)
- [ ] Verify all 19 blend modes work correctly

---

## 7. Potential Challenges & Solutions

### Challenge 1: Complex Blend Modes
**Problem**: Some blend modes (SoftLight, PinLight) have complex conditional logic that may not map directly to OpenCV CUDA operations.

**Solution**: 
- Use `cv::cuda::compare()` to create masks
- Apply different formulas based on masks
- If performance is insufficient, consider custom CUDA kernels (advanced)

### Challenge 2: Positioning/Clipping
**Problem**: Manual clipping logic for positioning may be complex on GPU.

**Solution**:
- Use `cv::cuda::GpuMat` ROI views: `cv::cuda::GpuMat(original, cv::Rect(...))`
- Use `cv::cuda::copyTo()` with masks for clipping
- May need to handle edge cases explicitly

### Challenge 3: Memory Bandwidth
**Problem**: Uploading/downloading frames can be a bottleneck.

**Solution**:
- Keep frames on GPU when possible (if source modules use GPU)
- Reuse buffers to avoid allocations
- Consider async operations (future optimization)

### Challenge 4: Opacity Integration
**Problem**: Opacity needs to be integrated into each blend mode formula.

**Solution**:
- Pre-multiply source by opacity: `srcF * opacity + dstF * (1.0f - opacity)` for Normal
- For other modes, integrate opacity into the blend formula
- Some modes may need special handling

---

## 8. Code Size Estimate

**New Code Required:**
- Header additions: ~50 lines
- GPU buffer initialization: ~20 lines
- `applyTransforms_gpu()`: ~80 lines
- `applyBlendMode_gpu()`: ~400 lines (19 blend modes × ~20 lines each)
- `compositeLayer_gpu()`: ~20 lines
- Main loop integration: ~50 lines
- UI checkbox: ~10 lines

**Total**: ~630 lines of new code

---

## 9. Testing Strategy

### 9.1 Unit Tests
- Test each blend mode individually (GPU vs CPU comparison)
- Test transforms (scaling, positioning)
- Test opacity blending
- Test edge cases (empty frames, size mismatches)

### 9.2 Integration Tests
- Test multi-layer compositing
- Test GPU/CPU fallback on errors
- Test performance with varying layer counts
- Test with different resolutions

### 9.3 Visual Verification
- Compare GPU output vs CPU output (pixel-perfect)
- Test all 19 blend modes visually
- Test opacity transitions
- Test transform combinations

---

## 10. Recommendations

### Priority: **High**
GPU acceleration will significantly improve performance, especially with multiple layers at high resolutions. The compositor is a performance-critical component.

### Implementation Approach: **Incremental**
1. Start with simple blend modes (Normal, Add, Multiply)
2. Add transforms
3. Add remaining blend modes incrementally
4. Optimize based on profiling

### Future Optimizations:
1. **Custom CUDA Kernels**: For complex blend modes if OpenCV CUDA is insufficient
2. **Async Processing**: Use CUDA streams for overlap
3. **GPU Memory Pooling**: Reuse buffers more efficiently
4. **Multi-GPU Support**: Distribute layers across GPUs (advanced)

---

## 11. Conclusion

Adding GPU acceleration to `VideoCompositorModule` is a **high-value, high-complexity** task. The implementation follows a well-established pattern from `VideoFXModule`, but requires careful attention to:

1. **Blend mode accuracy**: Ensuring GPU results match CPU exactly
2. **Memory management**: Efficient GPU buffer usage
3. **Error handling**: Graceful fallback to CPU on GPU errors
4. **Performance**: Maximizing GPU utilization

**Estimated Implementation Time**: 2-3 days for full implementation and testing

**Risk Level**: Medium (complex blend modes may require custom kernels)

**Recommendation**: Proceed with incremental implementation, starting with simple blend modes and transforms, then adding complexity.
