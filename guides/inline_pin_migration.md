# Inline Pin Migration Guide

This guide documents the pattern for implementing inline modulation pins on module UI controls (sliders, etc.) instead of placing them as separate parallel pins on the left edge of the node.

## Overview

**Before (Parallel Pins):**
All modulation inputs appear as separate pins on the left side of the node.

**After (Inline Pins):**
Modulation pins are placed directly next to their corresponding UI controls, making the connection between the pin and the parameter it modulates visually obvious.

![sample_sfx example](/guides/sample_sfx_inline_pins_example.png)

## Key Components

### 1. `NodePinHelpers` Struct (ModuleProcessor.h)

```cpp
struct NodePinHelpers {
    std::function<void(const char* label, int channel)> drawAudioInputPin;
    std::function<void(const char* label, int channel)> drawAudioOutputPin;
    std::function<void(const char* inLabel, const char* outLabel, int inChannel, int outChannel)> drawParallelPins;
    std::function<bool(int channel)> drawInlineInputPin;  // Returns true if drawn
    std::function<void(ModuleProcessor* module)> drawIoPins;
};
```

### 2. `drawInlineInputPin` Lambda (ImGuiNodeEditorComponent.cpp)

This lambda draws just the pin circle without a text label. It's designed to be placed inline with `ImGui::SameLine()`:

```cpp
helpers.drawInlineInputPin = [&](int channel) -> bool {
    int attr = encodePinId({lid, channel, true});
    seenAttrs.insert(attr);
    availableAttrs.insert(attr);

    PinID        pinId = {lid, channel, true, false, ""};
    PinDataType  pinType = this->getPinDataTypeForPin(pinId);
    unsigned int pinColor = this->getImU32ForType(pinType);

    bool isConnected = connectedInputAttrs.count(attr) > 0;
    ImNodes::PushColorStyle(ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);

    ImNodes::BeginInputAttribute(attr);
    ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight()));  // Small dummy instead of text
    ImNodes::EndInputAttribute();

    // Cache pin position for link rendering
    // ... position caching code ...

    ImNodes::PopColorStyle();
    return true;
};
```

---

## Migration Steps for a Module

### Step 1: Update the Header Signature

Ensure `drawParametersInNode` accepts the `pinHelpers` parameter:

```cpp
// In YourModuleProcessor.h
void drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers = nullptr) override;
```

### Step 2: Update the Implementation

In `drawParametersInNode`, wrap the function body with `ImGui::PushID(this)` and `ImGui::PopID()`:

```cpp
void YourModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    // ... all your UI code ...
    
    ImGui::PopID();
}
```

### Step 3: Add Inline Pins Before Sliders

For each slider that has a corresponding modulation input, add the inline pin call:

```cpp
// Example: Pitch slider with channel 0 modulation input
if (pinHelpers && pinHelpers->drawInlineInputPin)
{
    if (pinHelpers->drawInlineInputPin(0))  // Channel 0 = Pitch Mod
        ImGui::SameLine();
}
if (ImGui::SliderFloat("Pitch", &pitch, -24.0f, 24.0f, "%.1f st"))
{
    // ... slider logic ...
}
```

### Step 4: Update `drawIoPins` to Exclude Inline Pins

If your module has a custom `drawIoPins` implementation, ensure you DON'T draw parallel pins for channels that are now inline:

```cpp
void YourModuleProcessor::drawIoPins(NodePinHelpers& helpers)
{
    // Only draw pins that DON'T have inline equivalents
    // For example, "Trigger Mod" often stays as a parallel pin
    // because it has no dedicated UI control
    helpers.drawAudioInputPin("Trigger", 3);  // Channel 3 = Trigger (no slider)
    
    // Draw outputs as normal
    helpers.drawAudioOutputPin("Out L", 0);
    helpers.drawAudioOutputPin("Out R", 1);
}
```

### Step 5: Ensure Caller Passes `&helpers`

In `ImGuiNodeEditorComponent.cpp`, make sure special-case module rendering passes `&helpers`:

```cpp
// WRONG - breaks inline pins!
sampleLoader->drawParametersInNode(
    nodeContentWidth, isParamModulated, onModificationEnded);

// CORRECT - inline pins work!
sampleLoader->drawParametersInNode(
    nodeContentWidth, isParamModulated, onModificationEnded, &helpers);
```

---

## Pin Channel Mapping

Each module defines its pins in `PinDatabase.cpp`. The channel numbers must match between:
1. The `PinDatabase.cpp` entry
2. The `drawInlineInputPin(channel)` calls
3. The `drawIoPins` implementation (for non-inline pins)

### Example: sample_loader (PinDatabase.cpp)

```cpp
db["sample_loader"] = ModulePinInfo(
    NodeWidth::Big,
    {AudioPin("Pitch Mod", 0, PinDataType::CV),      // Channel 0 -> inline
     AudioPin("Speed Mod", 1, PinDataType::CV),      // Channel 1 -> inline
     AudioPin("Gate Mod", 2, PinDataType::CV),       // Channel 2 -> inline
     AudioPin("Trigger Mod", 3, PinDataType::Gate),  // Channel 3 -> PARALLEL (no slider)
     AudioPin("Range Start Mod", 4, PinDataType::CV),// Channel 4 -> inline
     AudioPin("Range End Mod", 5, PinDataType::CV),  // Channel 5 -> inline
     AudioPin("Randomize Trig", 6, PinDataType::Gate),// Channel 6 -> inline (next to button)
     AudioPin("Position Mod", 7, PinDataType::CV)},  // Channel 7 -> inline
    {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
    {});
```

---

## Live Value Telemetry (Critical!)

For the slider to visually track the incoming CV value, you need BOTH sides of the telemetry system:

### The "Modulation Trinity" Pattern

Per `debuginput.md`, every modulated parameter needs:

1. **`_mod` suffix** - Virtual routing ID for `isParamModulated()` and `isParamInputConnected()`
2. **`_live` suffix** - Telemetry key for UI to read the actual computed value
3. **Base ID** - The APVTS parameter name

### Step A: Update `processBlock` to Set Live Values

**CRITICAL**: Calculate CV values and call `setLiveParamValue` **OUTSIDE** any conditional blocks like `if (isPlaying)`. The UI needs to see the CV value even when the module isn't actively processing audio.

```cpp
void YourModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, ...)
{
    auto controlBus = getBusBuffer(buffer, true, 1);  // Get CV input bus
    
    // --- Compute gate value BEFORE any conditional blocks ---
    // Calculate even when not playing so UI shows live values!
    float gateValue = gateParam ? gateParam->load() : 0.8f;
    const bool isGateMod = isParamInputConnected("gate_mod") && controlBus.getNumChannels() > 0;
    const float* gateCV = isGateMod ? controlBus.getReadPointer(0) : nullptr;
    if (isGateMod && gateCV)
    {
        gateValue = juce::jlimit(0.0f, 1.0f, gateCV[0]);
    }
    // CRITICAL: Update telemetry BEFORE any isPlaying checks!
    setLiveParamValue("gate_live", gateValue);

    // --- Audio Rendering (conditional) ---
    if (isPlaying)
    {
        // Apply gate to audio using the pre-computed gateCV pointer
        if (isGateMod && gateCV)
        {
            for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
            {
                float* channelData = outBus.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                    channelData[i] *= juce::jlimit(0.0f, 1.0f, gateCV[i]);
            }
        }
        else
        {
            outBus.applyGain(gateValue);
        }
    }
}
```

> [!CAUTION]
> **Common Bug**: Putting `setLiveParamValue` inside an `if (isPlaying)` block means the slider won't track CV when the sample isn't playing!

### Step B: Update UI to Read Live Values

In `drawParametersInNode`, use `getLiveParamValueFor` when modulated:

```cpp
// WRONG - slider doesn't track CV value!
float gate = gateParam ? gateParam->load() : 0.8f;

// CORRECT - slider shows live CV value!
float gate = gateModulated ? getLiveParamValueFor(
                                 "gate_mod",        // Routing check ID
                                 "gate_live",       // Telemetry key
                                 gateParam ? gateParam->load() : 0.8f)  // Fallback
                           : (gateParam ? gateParam->load() : 0.8f);
```

### Complete Example: Gate Slider with Live CV Tracking

```cpp
// In drawParametersInNode:
bool gateModulated = isParamModulated("gate_mod");
if (gateModulated)
{
    ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
}

// Use live value when modulated
float gate = gateModulated ? getLiveParamValueFor("gate_mod", "gate_live",
                                 gateParam ? gateParam->load() : 0.8f)
                           : (gateParam ? gateParam->load() : 0.8f);

// Draw inline pin
if (pinHelpers && pinHelpers->drawInlineInputPin)
{
    if (pinHelpers->drawInlineInputPin(1))  // Channel 1 = Gate
        ImGui::SameLine();
}

// Draw slider (shows live CV value when connected!)
if (ImGui::SliderFloat("Gate", &gate, 0.0f, 1.0f, "%.2f"))
{
    if (!gateModulated)
    {
        // Only allow edits when not modulated
        if (auto* p = apvts.getParameter("gate"))
        {
            p->setValueNotifyingHost(apvts.getParameterRange("gate").convertTo0to1(gate));
            onModificationEnded();
        }
    }
}

if (gateModulated)
{
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("(mod)");
}
```

---

## Common Mistakes

### 1. Forgetting `ImGui::PushID(this)`
**Symptom:** Pins don't appear or behave erratically when multiple instances exist.
**Fix:** Always wrap `drawParametersInNode` with `PushID(this)` / `PopID()`.

### 2. Not Passing `&helpers`
**Symptom:** `pinHelpers` is null, so inline pins never draw.
**Fix:** Ensure ALL call sites in `ImGuiNodeEditorComponent.cpp` pass `&helpers`.

### 3. Incorrect Channel Numbers
**Symptom:** Wrong pin colors or cables connect to wrong parameters.
**Fix:** Cross-reference channel numbers with `PinDatabase.cpp`.

### 4. Drawing Pin Both Inline AND Parallel
**Symptom:** Duplicate pins appear.
**Fix:** Update `drawIoPins` to skip channels that are now inline.

### 5. Missing `setLiveParamValue` in processBlock
**Symptom:** Slider shows "(mod)" but value doesn't track CV input.
**Fix:** Call `setLiveParamValue("paramName_live", computedValue)` in processBlock.

### 6. Not Using `getLiveParamValueFor` in UI
**Symptom:** Slider shows base parameter value instead of modulated value.
**Fix:** Use ternary with `getLiveParamValueFor` when `isParamModulated` is true.

### 7. Telemetry Inside `if (isPlaying)` Block
**Symptom:** Slider tracks CV only while sample is playing, freezes when stopped.
**Fix:** Move `setLiveParamValue()` call OUTSIDE any conditional blocks. Calculate CV values before checking play state.

---

## Modules Successfully Migrated

- [x] `SampleSfxModuleProcessor` - Working reference implementation
- [x] `SampleLoaderModuleProcessor` - Fixed by adding `&helpers` to caller

## Modules Needing Migration

Check modules with modulation inputs that could benefit from inline pins:
- [ ] `VCOModuleProcessor`
- [ ] `ADSRModuleProcessor`
- [ ] `LFOModuleProcessor`
- [ ] `VCFModuleProcessor`
- [ ] `DelayModuleProcessor`
- [ ] `ReverbModuleProcessor`
- [ ] `CompressorModuleProcessor`
- [ ] `GranulatorModuleProcessor`
- [ ] (... and more effects/modulators)

---

## Testing Checklist

After migration:
1. [ ] Inline pins appear next to their sliders
2. [ ] Pins have correct colors (CV=cyan, Gate=yellow, Audio=green)
3. [ ] Cables can connect to inline pins
4. [ ] Modulation works (slider shows "(mod)" and is disabled when connected)
5. [ ] **Slider value tracks live CV input** (visually moves with CV)
6. [ ] No duplicate pins (inline + parallel)
7. [ ] Multiple instances work correctly (no ID collisions)
8. [ ] Undo/redo works after connecting to inline pins

