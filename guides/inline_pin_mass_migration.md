# Inline Pin Mass Migration Guide

This document provides a comprehensive, per-module migration plan for converting parallel modulation pins to inline pins. A less capable AI can follow these instructions precisely.

---

## Table of Contents
1. [Prerequisites](#prerequisites)
2. [Migration Checklist Template](#migration-checklist-template)
3. [Module Catalog by Family](#module-catalog-by-family)
4. [Per-Module Migration Instructions](#per-module-migration-instructions)

---

## Prerequisites

Before migrating ANY module, ensure you have read and understood:
- `guides/inline_pin_migration.md` - The pattern documentation
- `juce/Source/audio/modules/BestPracticeNodeProcessor.cpp` - Reference implementation
- `juce/Source/audio/modules/SampleSfxModuleProcessor.cpp` - Working inline pin example

---

## Migration Checklist Template

For EACH module, complete these steps in order:

### Step 1: Header File (ModuleName.h)
- [ ] Update `drawParametersInNode` signature to include `const NodePinHelpers* pinHelpers = nullptr`

### Step 2: Implementation File (ModuleName.cpp) - drawParametersInNode
- [ ] Add `ImGui::PushID(this);` at the start of `drawParametersInNode`
- [ ] Add `ImGui::PopID();` at the end of `drawParametersInNode`
- [ ] For EACH modulation parameter:
  - [ ] Add `if (pinHelpers && pinHelpers->drawInlineInputPin) { if (pinHelpers->drawInlineInputPin(CHANNEL)) ImGui::SameLine(); }` BEFORE the slider
  - [ ] Ensure slider uses `getLiveParamValueFor("paramName_mod", "paramName_live", fallback)` when modulated

### Step 3: Implementation File (ModuleName.cpp) - processBlock
- [ ] For EACH modulation parameter:
  - [ ] Ensure `setLiveParamValue("paramName_live", value)` is called OUTSIDE any `if (isPlaying)` blocks

### Step 4: Implementation File (ModuleName.cpp) - drawIoPins
- [ ] Remove parallel pin entries for channels that are now inline
- [ ] Keep parallel pins for:
  - Audio In/Out pins
  - Trigger/Gate pins without a corresponding slider
  - Any pin that doesn't have a dedicated UI control

### Step 5: Verification
- [ ] Build successfully
- [ ] Inline pins appear next to sliders
- [ ] Sliders track CV values in real-time
- [ ] No duplicate pins (inline + parallel)

---

## Module Catalog by Family

### ✅ Already Migrated
| Module | File | Status |
|--------|------|--------|
| sample_sfx | SampleSfxModuleProcessor.cpp | ✅ Complete |
| sample_loader | SampleLoaderModuleProcessor.cpp | ✅ Complete |

---

### 🔴 Priority 1: Effects (Audio In + CV Mods)
These modules have Audio In L/R + multiple CV modulation inputs. High visibility.

| Module Key | File | CV Mod Pins | Channels to Inline |
|------------|------|-------------|-------------------|
| vcf | VCFModuleProcessor.cpp | Cutoff, Resonance, Type | 2, 3, 4 |
| delay | DelayModuleProcessor.cpp | Time, Feedback, Mix | 2, 3, 4 |
| reverb | ReverbModuleProcessor.cpp | Size, Damp, Mix | 2, 3, 4 |
| chorus | ChorusModuleProcessor.cpp | Rate, Depth, Mix | 2, 3, 4 |
| phaser | PhaserModuleProcessor.cpp | Rate, Depth, Centre, Feedback, Mix | 2, 3, 4, 5, 6 |
| compressor | CompressorModuleProcessor.cpp | Thresh, Ratio, Attack, Release, Makeup, Mix | 2, 3, 4, 5, 6, 7 |
| limiter | LimiterModuleProcessor.cpp | Thresh, Release, Mix | 2, 3, 4 |
| drive | DriveModuleProcessor.cpp | Drive, Mix | 2, 3 |
| bit_crusher | BitCrusherModuleProcessor.cpp | Bit Depth, Sample Rate, Mix, Anti-Alias, Quant Mode | 2, 3, 4, 5, 6 |
| waveshaper | WaveshaperModuleProcessor.cpp | Drive, Type, Mix | 2, 3, 4 |
| timepitch | TimePitchModuleProcessor.cpp | Speed, Pitch, Mix | 2, 3, 4 |
| granulator | GranulatorModuleProcessor.cpp | Density, Size, Position, Pitch, Gate, Mix | 3, 4, 5, 6, 7, 8 |

---

### 🟠 Priority 2: Modulators
These modules generate CV/Gate and have modulation inputs for their parameters.

| Module Key | File | CV Mod Pins | Channels to Inline |
|------------|------|-------------|-------------------|
| lfo | LFOModuleProcessor.cpp | Rate, Depth, Wave | 0, 1, 2 |
| adsr | ADSRModuleProcessor.cpp | Attack, Decay, Sustain, Release | 2, 3, 4, 5 (keep Gate/Trigger as parallel) |
| random | RandomModuleProcessor.cpp | (No inputs) | N/A - Skip |
| function_generator | FunctionGeneratorModuleProcessor.cpp | Rate, Slew, Gate Thresh, Trig Thresh, Pitch Base, Value Mult, Curve Select | 3, 4, 5, 6, 7, 8, 9 (keep Gate/Trigger/Sync as parallel) |

---

### 🟡 Priority 3: Sources
These modules generate audio and have modulation inputs.

| Module Key | File | CV Mod Pins | Channels to Inline |
|------------|------|-------------|-------------------|
| vco | VCOModuleProcessor.cpp | Frequency, Waveform | 0, 1 (keep Gate as parallel at 2) |
| noise | NoiseModuleProcessor.cpp | Level, Colour, Rate | 0, 1, 2 |
| stk_string | StkStringModuleProcessor.cpp | Frequency, Velocity, Damping, Pickup Pos | 0, 2, 3, 4 (keep Pluck/Bow Gate as parallel at 1) |
| stk_wind | StkWindModuleProcessor.cpp | Freq, Breath, Vibrato, Vibrato Rate, Reed Stiffness, Jet Delay, Lip Tension | 0, 2, 3, 4, 5, 6, 7 (keep Gate as parallel at 1) |
| stk_percussion | StkPercussionModuleProcessor.cpp | Freq, Velocity, Stick Hardness, Strike Position, Decay, Resonance | 0, 2, 3, 4, 5, 6 (keep Strike Gate as parallel at 1) |
| stk_plucked | StkPluckedModuleProcessor.cpp | Freq, Damping, Velocity | 0, 2, 3 (keep Gate as parallel at 1) |

---

### 🟢 Priority 4: Utilities
These modules process CV/Audio and have modulation inputs.

| Module Key | File | CV Mod Pins | Channels to Inline |
|------------|------|-------------|-------------------|
| vca | VCAModuleProcessor.cpp | Gain | 2 |
| mixer | MixerModuleProcessor.cpp | Gain, Pan, X-Fade | 4, 5, 6 |
| cv_mixer | CVMixerModuleProcessor.cpp | Crossfade, Level A, Level C, Level D | 4, 5, 6, 7 |
| attenuverter | AttenuverterModuleProcessor.cpp | (Check file for CV inputs) | TBD |
| lag_processor | LagProcessorModuleProcessor.cpp | (Check file for CV inputs) | TBD |
| map_range | MapRangeModuleProcessor.cpp | (Check file for CV inputs) | TBD |

---

### 🔵 Priority 5: Sequencers (Complex)
These have MANY inputs. May require special handling.

| Module Key | File | Notes |
|------------|------|-------|
| sequencer | SequencerModuleProcessor.cpp | 54+ inputs - likely keep most as parallel |
| multi_sequencer | MultiSequencerModuleProcessor.cpp | 38+ inputs - likely keep most as parallel |

---

### ⚪ Skip/Special Cases
These modules should be skipped or handled specially:

| Module Key | Reason |
|------------|--------|
| comment | No processing, no pins |
| debug | Passthrough, no CV |
| input_debug | Passthrough, no CV |
| scope | Visualization only |
| recorder | No CV mods |
| reroute | Polymorphic passthrough |
| comparator | No CV mods, just threshold |
| audio_input | No CV mods |
| value | No inputs |

---

## Per-Module Migration Instructions

### VCF (vcf)

**File:** `juce/Source/audio/modules/VCFModuleProcessor.cpp`

**Pin Database Entry (PinDatabase.cpp line ~305):**
```cpp
db["vcf"] = ModulePinInfo(
    NodeWidth::Medium,
    {AudioPin("In L", 0, PinDataType::Audio),
     AudioPin("In R", 1, PinDataType::Audio),
     AudioPin("Cutoff Mod", 2, PinDataType::CV),
     AudioPin("Resonance Mod", 3, PinDataType::CV),
     AudioPin("Type Mod", 4, PinDataType::CV)},
    ...);
```

**Migration Steps:**

1. **Header (VCFModuleProcessor.h):**
   ```cpp
   void drawParametersInNode(
       float itemWidth,
       const std::function<bool(const juce::String& paramId)>& isParamModulated,
       const std::function<void()>& onModificationEnded,
       const NodePinHelpers* pinHelpers = nullptr) override;
   ```

2. **drawParametersInNode (VCFModuleProcessor.cpp):**
   ```cpp
   void VCFModuleProcessor::drawParametersInNode(..., const NodePinHelpers* pinHelpers)
   {
       ImGui::PushID(this);
       
       // For Cutoff slider:
       bool cutoffModulated = isParamModulated("cutoff_mod");
       float cutoff = cutoffModulated ? getLiveParamValueFor("cutoff_mod", "cutoff_live", cutoffParam->load())
                                      : cutoffParam->load();
       if (cutoffModulated) { ImGui::BeginDisabled(); ImGui::PushStyleColor(...); }
       
       if (pinHelpers && pinHelpers->drawInlineInputPin) {
           if (pinHelpers->drawInlineInputPin(2)) ImGui::SameLine();  // Channel 2
       }
       if (ImGui::SliderFloat("Cutoff", &cutoff, ...)) { ... }
       
       if (cutoffModulated) { ImGui::PopStyleColor(); ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
       
       // Repeat for Resonance (channel 3) and Type (channel 4)
       
       ImGui::PopID();
   }
   ```

3. **processBlock (VCFModuleProcessor.cpp):**
   ```cpp
   // BEFORE any if(isPlaying) or processing:
   float cutoffValue = cutoffParam->load();
   if (isParamInputConnected("cutoff_mod") && cvBus.getNumChannels() > 0) {
       cutoffValue = juce::jlimit(minCutoff, maxCutoff, cvBus.getReadPointer(0)[0]);
   }
   setLiveParamValue("cutoff_live", cutoffValue);
   // Repeat for resonance and type
   ```

4. **drawIoPins (VCFModuleProcessor.cpp):**
   ```cpp
   void VCFModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
   {
       // Only draw Audio In/Out - CV mods are now inline
       helpers.drawAudioInputPin("In L", 0);
       helpers.drawAudioInputPin("In R", 1);
       // REMOVE: helpers.drawAudioInputPin("Cutoff Mod", 2);
       // REMOVE: helpers.drawAudioInputPin("Resonance Mod", 3);
       // REMOVE: helpers.drawAudioInputPin("Type Mod", 4);
       helpers.drawAudioOutputPin("Out L", 0);
       helpers.drawAudioOutputPin("Out R", 1);
   }
   ```

---

### Delay (delay)

**File:** `juce/Source/audio/modules/DelayModuleProcessor.cpp`

**Inline Channels:** 2 (Time), 3 (Feedback), 4 (Mix)

**Pattern:** Same as VCF - Audio In L/R stay parallel, CV mods become inline.

---

### Reverb (reverb)

**File:** `juce/Source/audio/modules/ReverbModuleProcessor.cpp`

**Inline Channels:** 2 (Size), 3 (Damp), 4 (Mix)

**Pattern:** Same as VCF.

---

### LFO (lfo)

**File:** `juce/Source/audio/modules/LFOModuleProcessor.cpp`

**Inline Channels:** 0 (Rate), 1 (Depth), 2 (Wave)

**Special:** All inputs are CV mods (no Audio In). All become inline.

**drawIoPins after migration:**
```cpp
void LFOModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // ALL inputs are now inline - only draw output
    helpers.drawAudioOutputPin("Out", 0);
}
```

---

### ADSR (adsr)

**File:** `juce/Source/audio/modules/ADSRModuleProcessor.cpp`

**Inline Channels:** 2 (Attack), 3 (Decay), 4 (Sustain), 5 (Release)

**Keep Parallel:** 0 (Gate In), 1 (Trigger In) - these are primary triggers, not CV mods

**drawIoPins after migration:**
```cpp
void ADSRModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioInputPin("Gate In", 0);    // Keep parallel
    helpers.drawAudioInputPin("Trigger In", 1); // Keep parallel
    // REMOVE Attack, Decay, Sustain, Release - now inline
    helpers.drawAudioOutputPin("Env Out", 0);
    helpers.drawAudioOutputPin("Inv Out", 1);
    helpers.drawAudioOutputPin("EOR Gate", 2);
    helpers.drawAudioOutputPin("EOC Gate", 3);
}
```

---

### VCO (vco)

**File:** `juce/Source/audio/modules/VCOModuleProcessor.cpp`

**Inline Channels:** 0 (Frequency), 1 (Waveform)

**Keep Parallel:** 2 (Gate) - primary trigger

---

### Noise (noise)

**File:** `juce/Source/audio/modules/NoiseModuleProcessor.cpp`

**Inline Channels:** 0 (Level), 1 (Colour), 2 (Rate)

**Special:** All inputs are CV mods. All become inline.

---

## Critical Reminders for the Migrating AI

1. **ALWAYS check getParamRouting()** in the module to verify the correct channel numbers before using them in `drawInlineInputPin(channel)`.

2. **ALWAYS ensure setLiveParamValue is called OUTSIDE any conditional blocks** like `if (isPlaying)` or `if (currentProcessor != nullptr)`.

3. **NEVER duplicate pins** - if a pin becomes inline, REMOVE it from drawIoPins.

4. **VERIFY the _mod suffix convention** - the virtual routing ID uses `_mod` suffix, the telemetry key uses `_live` suffix.

5. **TEST each module individually** before moving to the next.

---

## File Locations Quick Reference

| File Type | Path Pattern |
|-----------|--------------|
| Header | `juce/Source/audio/modules/{ModuleName}ModuleProcessor.h` |
| Implementation | `juce/Source/audio/modules/{ModuleName}ModuleProcessor.cpp` |
| Pin Database | `juce/Source/preset_creator/PinDatabase.cpp` |
| Node Editor | `juce/Source/preset_creator/ImGuiNodeEditorComponent.cpp` |
| Migration Guide | `guides/inline_pin_migration.md` |
