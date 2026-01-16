# Plan: Disable Video Modules in Audio-Only Build

## Overview
Ensure all video-related modules are properly disabled when building the Pikon Raditsz audio-only app (`AUDIO_ONLY_BUILD=1`). This prevents compilation errors and ensures video functionality is completely excluded from the audio-only variant.

## Current State Analysis

### ✅ Already Properly Guarded

1. **Module Registration** (`ModularSynthProcessor.cpp` lines 1134-1150)
   - All video modules are wrapped in `#ifndef AUDIO_ONLY_BUILD`
   - Includes: webcam_loader, video_file_loader, video_fx, chromakey, video_compositor, video_draw_impact, movement_detector, pose_estimator, hand_tracker, face_tracker, object_detector, color_tracker, contour_detector, crop_video, **video_viewer**

2. **Most Includes** (`ImGuiNodeEditorComponent.cpp` lines 128-142)
   - Video module headers are wrapped in `#ifndef AUDIO_ONLY_BUILD`
   - **ISSUE**: `VideoViewerModuleProcessor.h` is NOT included here (but it's included in ModularSynthProcessor.cpp)

### ❌ Issues Found

#### 1. Missing Include Guard
- **File**: `juce/Source/audio/graph/ModularSynthProcessor.cpp` line 106
- **Issue**: `#include "../modules/VideoViewerModuleProcessor.h"` is NOT wrapped in `#ifndef AUDIO_ONLY_BUILD`
- **Impact**: Will cause compilation error in audio-only build

#### 2. VideoViewer Auto-Creation Code (3 locations)
- **Location 1**: `renderImGui()` function (lines 1102-1133)
  - Auto-creates VideoViewer on empty canvas at startup
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

- **Location 2**: `applyUiValueTree()` function (lines 8991-9005)
  - Auto-creates VideoViewer when loading empty preset
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

- **Location 3**: `newCanvas()` function (lines 9715-9726)
  - Auto-creates VideoViewer when creating new canvas
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

#### 3. VideoViewer Menu Items
- **Location**: `ImGuiNodeEditorComponent.cpp` line 7592-7593
  - Menu item "Video Viewer" in context menu
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

#### 4. VideoViewer in Module Lists
- **Location 1**: `ImGuiNodeEditorComponent.cpp` line 12773
  - In module name mapping array
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

- **Location 2**: `ImGuiNodeEditorComponent.cpp` line 14590
  - In module type checking (contains "video_viewer")
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

- **Location 3**: `ImGuiNodeEditorComponent.cpp` lines 14688-14689
  - In module help/description mapping
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

- **Location 4**: `ImGuiNodeEditorComponent.cpp` line 15665
  - In `populatePinDatabase()` - `addInputModule(PinDataType::Video, "video_viewer")`
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

#### 5. PinDatabase.cpp
- **Location 1**: Line 174-176
  - Module description for "video_viewer"
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

- **Location 2**: Lines 1764-1769
  - Module pin database entry for "video_viewer"
  - **Needs**: `#ifndef AUDIO_ONLY_BUILD` guard

#### 6. Other Video Module References (Already Guarded)
- Most other video modules (Video FX, Video Compositor, etc.) appear to be properly guarded in menu items
- However, should verify all video-related menu sections are wrapped

## Implementation Plan

### Phase 1: Fix Critical Compilation Issues
1. **Add include guard** in `ModularSynthProcessor.cpp`
   - Wrap `#include "../modules/VideoViewerModuleProcessor.h"` with `#ifndef AUDIO_ONLY_BUILD`

### Phase 2: Fix Auto-Creation Code
2. **Guard VideoViewer auto-creation in `renderImGui()`**
   - Wrap lines 1102-1133 with `#ifndef AUDIO_ONLY_BUILD`

3. **Guard VideoViewer auto-creation in `applyUiValueTree()`**
   - Wrap lines 8991-9005 with `#ifndef AUDIO_ONLY_BUILD`

4. **Guard VideoViewer auto-creation in `newCanvas()`**
   - Wrap lines 9715-9726 with `#ifndef AUDIO_ONLY_BUILD`

### Phase 3: Fix UI References
5. **Guard VideoViewer menu item**
   - Wrap line 7592-7593 with `#ifndef AUDIO_ONLY_BUILD`

6. **Guard VideoViewer in module arrays**
   - Wrap line 12773 in module name mapping
   - Wrap "video_viewer" check in line 14590
   - Wrap lines 14688-14689 in module help mapping
   - Wrap line 15665 in `populatePinDatabase()`

### Phase 4: Fix PinDatabase
7. **Guard VideoViewer in PinDatabase.cpp**
   - Wrap lines 174-176 (description)
   - Wrap lines 1764-1769 (pin database entry)

### Phase 5: Verification
8. **Build verification**
   - Compile audio-only build to ensure no errors
   - Verify video modules don't appear in UI
   - Verify VideoViewer auto-creation doesn't run

## Risk Assessment

### Risk Level: **MEDIUM**

**Risks:**
- Missing a reference could cause compilation errors
- Auto-creation code running in audio-only build could cause runtime errors
- UI references could cause crashes if modules aren't available

**Mitigation:**
- Systematic review of all video module references
- Test compilation of audio-only build after changes
- Verify UI doesn't show video modules in audio-only build

## Confidence Rating: **HIGH (95%)**

**Strong Points:**
- Clear pattern already established with `#ifndef AUDIO_ONLY_BUILD`
- Most video modules already properly guarded
- Systematic approach to finding all references

**Weak Points:**
- Need to verify no other hidden references
- Some code might be in conditional blocks that need careful placement

## Testing Checklist

- [ ] Audio-only build compiles without errors
- [ ] Video modules don't appear in module menus
- [ ] VideoViewer doesn't auto-create on new canvas
- [ ] VideoViewer doesn't auto-create on empty preset load
- [ ] VideoViewer doesn't auto-create on startup
- [ ] No video-related UI elements visible in audio-only build
- [ ] Pin database doesn't include video modules
- [ ] Module descriptions don't include video modules

## Files to Modify

1. `juce/Source/audio/graph/ModularSynthProcessor.cpp`
2. `juce/Source/preset_creator/ImGuiNodeEditorComponent.cpp` (multiple locations)
3. `juce/Source/preset_creator/PinDatabase.cpp`

## Estimated Effort

- **Time**: 30-45 minutes
- **Complexity**: Low (mostly adding preprocessor guards)
- **Risk**: Low (well-defined changes)
