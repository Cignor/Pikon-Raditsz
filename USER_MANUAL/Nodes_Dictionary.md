# Collider Modular Synthesizer - Node Dictionary

**Last Updated:** December 18, 2024  
**Version:** 1.2

---

## Node Sizes and Layout

Nodes in Collider have standardized width categories to ensure consistent layout and prevent overlapping in the node editor. The "Beautify Layout" function uses these sizes to arrange nodes optimally.

### Node Width Categories

Nodes are categorized into five width categories:

- **Small** (240px): Basic modules with minimal UI elements
  - Examples: VCO, VCA, LFO, ADSR, VCF, Mixer, Comparator, Logic, Math
  
- **Medium** (360px): Modules with moderate UI complexity
  - Examples: Delay, Reverb, Chorus, Phaser, Compressor, Scope, Frequency Graph
  
- **Big** (480px): Complex modules with extensive controls
  - Examples: PolyVCO, Sample Loader, Function Generator, Granulator, TTS Performer, Automation Lane
  
- **ExtraWide** (840px): Timeline/grid-based modules requiring wide display
  - Examples: Sequencer, Multi Sequencer, MIDI Player, Tempo Clock, Timeline
  
- **Exception**: Custom-sized modules that define their own dimensions
  - Examples: Spatial Granulator (16:9 canvas), Video modules (webcam_loader, video_file_loader), Computer Vision modules (pose_estimator, hand_tracker, face_tracker), Physics, Animation

### Size Determination

The node editor determines node sizes using the following priority:

1. **Rendered Dimensions**: If a node has been rendered, `ImNodes::GetNodeDimensions()` provides the actual rendered size
2. **PinDatabase Lookup**: For unrendered nodes, the system looks up the module type in `PinDatabase` and uses the `defaultWidth` category
3. **Fallback**: If neither is available, uses the theme's default node width

### Exception Nodes

Exception-sized nodes (like video modules and computer vision nodes) have custom dimensions defined by the module itself. These are typically much larger than standard nodes to accommodate video previews, canvas interfaces, or complex visualizations.

The "Beautify Layout" function handles exception nodes by:
- Using a wider default (1.5x theme default) when dimensions aren't available
- Respecting actual rendered dimensions when available
- Ensuring proper spacing to prevent overlap

---

## Table of Contents

### Quick Reference Index

#### 1. SOURCE NODES
- [vco](#vco) - Voltage-Controlled Oscillator
- [polyvco](#polyvco) - Multi-Voice Oscillator Bank
- [noise](#noise) - Noise Generator
- [audio_input](#audio_input) - Hardware Audio Input
- [sample_loader](#sample_loader) - Audio Sample Player
- [value](#value) - Constant Value Generator

#### 2. EFFECT NODES
- [vcf](#vcf) - Voltage-Controlled Filter
- [delay](#delay) - Stereo Delay Effect
- [reverb](#reverb) - Stereo Reverb Effect
- [chorus](#chorus) - Stereo Chorus Effect
- [phaser](#phaser) - Stereo Phaser Effect
- [compressor](#compressor) - Dynamic Range Compressor
- [limiter](#limiter) - Audio Limiter
- [gate](#gate) - Noise Gate
- [drive](#drive) - Waveshaping Distortion
- [graphic_eq](#graphic_eq) - 8-Band Graphic Equalizer
- [waveshaper](#waveshaper) - Multi-Algorithm Waveshaper
- [8bandshaper](#8bandshaper) - Multi-Band Waveshaper
- [granulator](#granulator) - Granular Synthesizer/Effect
- [spatial_granulator](#spatial_granulator) - Visual Canvas Granulator/Chorus
- [harmonic_shaper](#harmonic_shaper) - Harmonic Content Shaper
- [timepitch](#timepitch) - Time/Pitch Manipulation
- [de_crackle](#de_crackle) - Click/Pop Reducer
- [vocal_tract_filter](#vocal_tract_filter) - Formant Filter

#### 3. MODULATOR NODES
- [lfo](#lfo) - Low-Frequency Oscillator
- [adsr](#adsr) - Envelope Generator
- [random](#random) - Random Value Generator
- [s_and_h](#s_and_h) - Sample & Hold
- [function_generator](#function_generator) - Drawable Envelope/LFO
- [shaping_oscillator](#shaping_oscillator) - Oscillator with Built-in Waveshaper

#### 4. UTILITY & LOGIC NODES
- [vca](#vca) - Voltage-Controlled Amplifier
- [mixer](#mixer) - Stereo Audio Mixer
- [cv_mixer](#cv_mixer) - Control Voltage Mixer
- [track_mixer](#track_mixer) - Multi-Channel Mixer
- [panvol](#panvol) - 2D Volume and Panning Control
- [attenuverter](#attenuverter) - Attenuate/Invert Signal
- [lag_processor](#lag_processor) - Slew Limiter/Smoother
- [math](#math) - Mathematical Operations
- [map_range](#map_range) - Value Range Mapper
- [quantizer](#quantizer) - Musical Scale Quantizer
- [rate](#rate) - Rate Value Converter
- [comparator](#comparator) - Threshold Comparator
- [logic](#logic) - Boolean Logic Operations
- [clock_divider](#clock_divider) - Clock Division/Multiplication
- [sequential_switch](#sequential_switch) - Signal Router

#### 5. SEQUENCER NODES
- [sequencer](#sequencer) - 16-Step CV/Gate Sequencer
- [multi_sequencer](#multi_sequencer) - Advanced Multi-Output Sequencer
- [snapshot_sequencer](#snapshot_sequencer) - Patch State Sequencer
- [stroke_sequencer](#stroke_sequencer) - Gesture-Based Sequencer
- [chord_arp](#chord_arp) - Chord & Arpeggiator Harmony Brain
- [tempo_clock](#tempo_clock) - Global Clock Generator
- [timeline](#timeline) - Automation Recorder and Playback
- [automation_lane](#automation_lane) - Drawable Automation Curve Lane
- [automato](#automato) - Gesture Recorder and Playback

#### 6. MIDI NODES
- [midi_cv](#midi_cv) - MIDI to CV Converter
- [midi_player](#midi_player) - MIDI File Player
- [midi_faders](#midi_faders) - MIDI-Learnable Faders (1-16)
- [midi_knobs](#midi_knobs) - MIDI-Learnable Knobs (1-16)
- [midi_buttons](#midi_buttons) - MIDI-Learnable Buttons (1-32)
- [midi_jog_wheel](#midi_jog_wheel) - MIDI Jog Wheel Control

#### 7. ANALYSIS NODES
- [scope](#scope) - Oscilloscope
- [debug](#debug) - Signal Value Logger
- [input_debug](#input_debug) - Passthrough Debug Logger
- [frequency_graph](#frequency_graph) - Spectrum Analyzer

#### 8. TTS (TEXT-TO-SPEECH) NODES
- [tts_performer](#tts_performer) - Text-to-Speech Engine
- [vocal_tract_filter](#vocal_tract_filter) - Formant Filter

#### 9. SPECIAL NODES
- [physics](#physics) - 2D Physics Simulation
- [animation](#animation) - 3D Animation Player

#### 10. COMPUTER VISION NODES
- [webcam_loader](#webcam_loader) - Webcam Video Source
- [video_file_loader](#video_file_loader) - Video File Source
- [movement_detector](#movement_detector) - Motion Detection
- [object_detector](#object_detector) - Object Detection (YOLOv3)
- [pose_estimator](#pose_estimator) - Body Keypoint Detection
- [hand_tracker](#hand_tracker) - Hand Keypoint Tracking
- [face_tracker](#face_tracker) - Facial Landmark Tracking
- [chromakey](#chromakey) - Chromakey Video Processor
- [color_tracker](#color_tracker) - Multi-Color Tracking
- [contour_detector](#contour_detector) - Shape Detection
- [video_fx](#video_fx) - Real-Time Video Effects
- [crop_video](#crop_video) - Video Cropping and Tracking

#### 11. SYSTEM NODES
- [meta](#meta) - Meta Module Container
- [inlet](#inlet) - Meta Module Input
- [outlet](#outlet) - Meta Module Output
- [comment](#comment) - Documentation Node
- [recorder](#recorder) - Audio Recording to File
- [vst_host](#vst_host) - VST Plugin Host
- [bpm_monitor](#bpm_monitor) - Rhythm Detection and BPM Reporting

---

## Auto-Connect Shortcuts

When multiple nodes are selected, you can use keyboard shortcuts to automatically chain them by data type. These shortcuts intelligently connect compatible outputs to inputs based on signal types.

| Key | Data Type | Color | Use Case |
|-----|-----------|-------|----------|
| **C** | Standard Chaining | White | Stereo audio chain (channels 0→0, 1→1) |
| **G** | Audio | Green | Audio effects chain |
| **B** | CV (Control Voltage) | Blue | CV signal chain (modulation inputs) |
| **Y** | Gate | Yellow | Gate/trigger chain |
| **R** | Raw | Red | Raw value chain |
| **V** | Video | Cyan | Video source chain (webcam/video → vision processing modules) |

**How It Works:**
- Select 2 or more nodes
- Press the appropriate key for the data type you want to chain
- The system automatically connects compatible outputs to inputs
- Works with most node types that have matching signal types

**Examples:**
- **Video Processing:** Select Webcam Loader → Movement Detector, press **V** to connect `Source ID` → `Source In`
- **CV Modulation:** Select LFO → VCF, press **B** to connect LFO output → Cutoff Mod input
- **Audio Chain:** Select VCO → VCF → VCA, press **G** to chain audio outputs to inputs
- **Gate Triggers:** Select Sequencer → ADSR, press **Y** to connect Gate output → Gate In

**Note:** Auto-connection shortcuts only work when 2+ nodes are selected.

---

## 1. SOURCE NODES

Source nodes generate or input signals into your patch.

### vco
**Voltage-Controlled Oscillator**

A standard analog-style oscillator that generates periodic waveforms.

#### Inputs
- `Frequency` (CV) - Frequency modulation input
- `Waveform` (CV) - Waveform selection modulation
- `Gate` (Gate) - Gate input for amplitude control

#### Outputs
- `Out` (Audio) - Mono audio output

#### Parameters
- `Frequency` (20 Hz - 20 kHz) - Base oscillator frequency
- `Waveform` (Choice) - Sine, Sawtooth, or Square wave
- `Relative Freq Mod` (Bool) - When enabled, CV modulates ±4 octaves around slider position. When disabled, CV directly maps to 20 Hz - 20 kHz
- `Portamento` (0-2 seconds) - Frequency glide/smoothing time

#### How to Use
1. Connect the audio output to an effect or VCA
2. Set the frequency slider to your desired pitch
3. Optionally connect CV (from sequencer, LFO, or ADSR) to modulate frequency
4. Use the Relative Freq Mod toggle to choose between relative (musical) or absolute (full range) modulation
5. Connect a gate signal for amplitude gating if needed
6. Adjust portamento for smooth frequency transitions

---

### polyvco
**Multi-Voice Oscillator Bank**

A polyphonic oscillator module with up to 32 independent voices, ideal for creating rich, layered sounds or building polyphonic synthesizers.

#### Inputs
- `Num Voices Mod` (Raw) - Control number of active voices (1-32)
- `Freq 1-32 Mod` (CV) - Individual frequency modulation for each voice
- `Wave 1-32 Mod` (CV) - Individual waveform modulation for each voice
- `Gate 1-32 Mod` (Gate) - Individual gate inputs for each voice

#### Outputs
- `Out 1-32` (Audio) - 32 independent audio outputs (one per voice)

#### Parameters
- `Num Voices` (1-32) - Number of active voices
- `Base Frequency` (20 Hz - 20 kHz) - Base frequency for all voices
- `Detune Amount` (0-100 cents) - Amount of random detuning between voices
- `Spread` (0-100%) - Frequency spread between voices
- `Waveform` (Choice) - Base waveform for all voices (Sine, Sawtooth, Square)

#### How to Use
1. Set the number of voices you want active
2. Connect the voice outputs to a Track Mixer or individual effects
3. Use the detune parameter to create a chorus-like effect
4. Connect a Multi Sequencer's parallel outputs to the individual frequency and gate inputs for polyphonic melodies
5. Adjust spread to create harmonic stacks

---

### noise
**Noise Generator**

Generates white, pink, or brown noise for percussion, ambience, or modulation.

#### Inputs
- `Level Mod` (CV) - Level modulation input
- `Colour Mod` (CV) - Noise color modulation

#### Outputs
- `Out L` (Audio) - Left channel output
- `Out R` (Audio) - Right channel output

#### Parameters
- `Colour` (Choice) - White (flat spectrum), Pink (-3 dB/octave), or Brown (-6 dB/octave)
- `Level dB` (-60 to +6 dB) - Output level in decibels

#### How to Use
1. Select the noise color (white for hi-hats, pink for general noise, brown for low rumble)
2. Adjust the level to taste
3. Optionally modulate the color with CV for dynamic timbral changes
4. Great for percussion synthesis when combined with envelopes and filters
5. Use as a modulation source for subtle random variations

---

### audio_input
**Hardware Audio Input**

Brings external audio from your audio interface into the patch.

#### Outputs
- `Out 1` (Audio) - Input channel 1
- `Out 2` (Audio) - Input channel 2
- `Gate` (Gate) - Gate signal when audio exceeds threshold
- `Trigger` (Gate) - Trigger signal on transients
- `EOP` (Gate) - End of phrase detection

#### Parameters
- `Input Gain` (-60 to +20 dB) - Input gain control
- `Gate Threshold` (-60 to 0 dB) - Threshold for gate output
- `Trigger Sensitivity` (Low/Medium/High) - Transient detection sensitivity

#### How to Use
1. Connect your external audio source (microphone, instrument, etc.) to your audio interface
2. Adjust input gain to get a healthy signal level
3. Use the gate and trigger outputs to create envelope followers or rhythm detection
4. Process the audio through effects or use it as a modulation source via envelope following

---

### sample_loader
**Audio Sample Player**

Loads and plays audio samples with extensive playback control and modulation options.

#### Inputs
- `Pitch Mod` (CV) - Pitch modulation in semitones
- `Speed Mod` (CV) - Playback speed modulation
- `Gate Mod` (CV) - Gate/trigger modulation
- `Trigger Mod` (Gate) - Retrigger the sample
- `Range Start Mod` (CV) - Modulate sample start point
- `Range End Mod` (CV) - Modulate sample end point
- `Randomize Trig` (Gate) - Randomize sample settings on trigger

#### Outputs
- `Out L` (Audio) - Left channel output
- `Out R` (Audio) - Right channel output

#### Parameters
- `File` (Button) - Load audio file (WAV, AIFF, FLAC, MP3)
- `Pitch` (-48 to +48 semitones) - Pitch shift amount
- `Speed` (0.1x to 4x) - Playback speed multiplier
- `Loop Mode` (Choice) - Off, Forward, Ping-Pong
- `Gate Mode` (Choice) - Trigger (one-shot), Gate (held), Free (always play)
- `Range Start` (0-100%) - Sample start position
- `Range End` (0-100%) - Sample end position
- `Reverse` (Bool) - Play sample in reverse
- `Randomize Range` (Bool) - Randomize start/end on each trigger

#### How to Use
1. Click the "Load File" button and select an audio file
2. Set the pitch and speed for your desired sound
3. Choose a loop mode (Off for one-shots, Forward for sustained sounds)
4. Use Gate Mode: Trigger for drums, Gate for sustained tones, Free for continuous playback
5. Adjust Range Start/End to isolate specific portions of the sample
6. Connect CV to Pitch Mod for melodic playing
7. Use Trigger Mod to retrigger the sample rhythmically
8. Enable Randomize Range for variation on each hit

---

### value
**Constant Value Generator**

Outputs a constant, adjustable numerical value in multiple formats.

#### Outputs
- `Raw` (Raw) - Unprocessed value as-is
- `Normalized` (CV) - Value normalized to 0-1 range
- `Inverted` (Raw) - Negative of raw value
- `Integer` (Raw) - Truncated integer value
- `CV Out` (CV) - Scaled CV output (0-1 range)

#### Parameters
- `Value` (-100 to +100) - The constant value to output

#### How to Use
1. Adjust the value slider to your desired number
2. Connect the appropriate output to the destination:
   - Use `CV Out` for standard 0-1 modulation
   - Use `Raw` for custom ranges or mathematical operations with the Math node
   - Use `Integer` for step/index control
3. Great for setting static modulation amounts, offsets, or reference values
4. Combine multiple Value nodes with Math nodes for complex calculations

---

## 2. EFFECT NODES

Effect nodes process audio signals to shape tone, add space, or create sonic textures.

### vcf
**Voltage-Controlled Filter**

A resonant multi-mode filter for subtractive synthesis and tone shaping.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Cutoff Mod` (CV) - Cutoff frequency modulation
- `Resonance Mod` (CV) - Resonance amount modulation
- `Type Mod` (CV) - Filter type modulation

#### Outputs
- `Out L` (Audio) - Left filtered output
- `Out R` (Audio) - Right filtered output

#### Parameters
- `Cutoff` (20 Hz - 20 kHz) - Filter cutoff frequency
- `Resonance` (0.1 - 10.0) - Resonance/Q factor
- `Type` (Choice) - Low-pass, High-pass, or Band-pass
- `Relative Cutoff Mod` (Bool) - When enabled, CV modulates ±5 octaves around slider. When disabled, CV maps to full 20 Hz - 20 kHz range
- `Relative Resonance Mod` (Bool) - When enabled, CV scales resonance 0.25x-4x. When disabled, CV maps to full 0.1-10.0 range

#### How to Use
1. Connect audio through the filter
2. Adjust cutoff to set the frequency where filtering occurs
3. Increase resonance for emphasis around the cutoff (be careful, high values can self-oscillate!)
4. Choose filter type: Low-pass removes highs, High-pass removes lows, Band-pass keeps only around cutoff
5. Modulate cutoff with envelopes or LFOs for classic synth sounds
6. Use Relative mode for musical modulation around a set position

---

### delay
**Stereo Delay Effect**

A stereo delay effect with modulation and tempo sync capabilities.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Time Mod` (CV) - Delay time modulation
- `Feedback Mod` (CV) - Feedback amount modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left delayed output
- `Out R` (Audio) - Right delayed output

#### Parameters
- `Time (ms)` (1-2000 ms) - Delay time in milliseconds
- `Feedback` (0-0.95) - Amount of delayed signal fed back into the delay
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Relative Time Mod` (Bool) - Enable relative time modulation around slider position
- `Relative Feedback Mod` (Bool) - Enable relative feedback modulation
- `Relative Mix Mod` (Bool) - Enable relative mix modulation

#### How to Use
1. Send audio through the delay
2. Set delay time to taste (short for slapback, long for echoes)
3. Adjust feedback for the number of repeats (be careful, high values can self-oscillate!)
4. Use mix to blend delayed signal with dry signal
5. Modulate time with LFOs for chorus-like effects
6. Connect to Tempo Clock's clock outputs and use short delay times for rhythmic effects

---

### reverb
**Stereo Reverb Effect**

A stereo reverb effect that simulates acoustic spaces.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Size Mod` (CV) - Room size modulation
- `Damp Mod` (CV) - Damping modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left reverb output
- `Out R` (Audio) - Right reverb output

#### Parameters
- `Size` (0-1) - Room size (0=small, 1=large)
- `Damping` (0-1) - High frequency damping (0=bright, 1=dark)
- `Width` (0-1) - Stereo width
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)

#### How to Use
1. Send audio through the reverb
2. Adjust size to set the perceived space (small room to large hall)
3. Use damping to control brightness (low damping=reflective surfaces, high damping=absorptive)
4. Adjust mix to blend reverb with dry signal
5. Great for adding depth and space to sounds
6. Use sparingly on bass-heavy sounds to avoid muddiness

---

### chorus
**Stereo Chorus Effect**

A stereo chorus effect that creates thick, shimmering textures by layering slightly detuned copies of the signal.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Rate Mod` (CV) - LFO rate modulation
- `Depth Mod` (CV) - Effect depth modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left chorus output
- `Out R` (Audio) - Right chorus output

#### Parameters
- `Rate` (0.1-10 Hz) - LFO speed
- `Depth` (0-1) - Modulation depth
- `Mix` (0-1) - Wet/dry balance

#### How to Use
1. Send audio through the chorus
2. Set rate for the speed of the sweeping effect (slow=gentle, fast=vibrato)
3. Adjust depth for intensity of detuning
4. Use mix to blend with dry signal
5. Great for thickening synth pads and leads
6. Use on clean guitars for classic 80s sounds

---

### phaser
**Stereo Phaser Effect**

A stereo phaser effect that creates sweeping notches in the frequency spectrum.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Rate Mod` (CV) - LFO rate modulation
- `Depth Mod` (CV) - Effect depth modulation
- `Centre Mod` (CV) - Center frequency modulation
- `Feedback Mod` (CV) - Feedback amount modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left phaser output
- `Out R` (Audio) - Right phaser output

#### Parameters
- `Rate` (0.1-10 Hz) - LFO speed
- `Depth` (0-1) - Modulation depth
- `Centre Freq` (200-2000 Hz) - Center frequency of the sweep
- `Feedback` (0-0.95) - Amount of feedback (increases resonance)
- `Mix` (0-1) - Wet/dry balance

#### How to Use
1. Send audio through the phaser
2. Adjust rate for sweep speed (slow=subtle, fast=intense)
3. Set centre frequency to target specific frequency ranges
4. Increase feedback for more pronounced notches
5. Great for adding movement to static sounds
6. Classic effect for electric pianos and guitars

---

### compressor
**Dynamic Range Compressor**

Reduces the dynamic range of audio signals, making quiet parts louder and loud parts quieter.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Thresh Mod` (CV) - Threshold modulation
- `Ratio Mod` (CV) - Ratio modulation
- `Attack Mod` (CV) - Attack time modulation
- `Release Mod` (CV) - Release time modulation
- `Makeup Mod` (CV) - Makeup gain modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left compressed output
- `Out R` (Audio) - Right compressed output

#### Parameters
- `Threshold` (-60 to 0 dB) - Level above which compression starts
- `Ratio` (1:1 to 20:1) - Amount of compression
- `Attack` (0.1-200 ms) - How quickly compression engages
- `Release` (5-1000 ms) - How quickly compression disengages
- `Makeup Gain` (-12 to +12 dB) - Output gain to compensate for level reduction
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Relative Threshold Mod` (Bool) - When enabled, CV modulates ±30 dB around slider. When disabled, CV directly maps to -60 to 0 dB range
- `Relative Ratio Mod` (Bool) - When enabled, CV modulates around slider (0.25x-4x). When disabled, CV directly maps to 1:1 to 20:1 range
- `Relative Attack Mod` (Bool) - When enabled, CV modulates around slider (0.25x-4x). When disabled, CV directly maps to 0.1-200 ms range
- `Relative Release Mod` (Bool) - When enabled, CV modulates around slider (0.25x-4x). When disabled, CV directly maps to 5-1000 ms range
- `Relative Makeup Mod` (Bool) - When enabled, CV modulates ±12 dB around slider. When disabled, CV directly maps to -12 to +12 dB range

#### How to Use
1. Set threshold to the level where you want compression to start
2. Adjust ratio (2:1 for gentle, 10:1+ for heavy compression)
3. Use fast attack to catch transients, slow attack to preserve punch
4. Set release to taste (fast for pumping effects, slow for smooth)
5. Adjust makeup gain to match the output level to the input
6. Adjust mix to blend processed signal with dry input (0.0 = 100% dry, 1.0 = 100% wet)
7. Use Relative Mod toggles to choose between musical (relative) or full-range (absolute) CV modulation
8. Great for controlling dynamics, adding sustain, and gluing mixes together

---

### limiter
**Audio Limiter**

Prevents audio from exceeding a set level, acting as a "brick wall" for peaks.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Thresh Mod` (CV) - Threshold modulation
- `Release Mod` (CV) - Release time modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left limited output
- `Out R` (Audio) - Right limited output

#### Parameters
- `Threshold` (-20 to 0 dB) - Maximum allowed level
- `Release` (1-200 ms) - Recovery time
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Relative Threshold Mod` (Bool) - When enabled, CV modulates ±10 dB around slider. When disabled, CV directly maps to -20 to 0 dB range
- `Relative Release Mod` (Bool) - When enabled, CV modulates around slider (0.25x-4x). When disabled, CV directly maps to 1-200 ms range

#### How to Use
1. Set threshold to the maximum level you want to allow
2. Adjust mix to blend processed signal with dry input (0.0 = 100% dry, 1.0 = 100% wet)
3. Use Relative Mod toggles to choose between musical (relative) or full-range (absolute) CV modulation
2. Adjust release time (fast for transparent, slow for smoother)
3. Use at the end of your signal chain to prevent clipping
4. Essential for mastering and protecting speakers
5. Can add punch and loudness when used aggressively

---

### gate
**Noise Gate**

Silences signals below a threshold, useful for removing background noise or creating rhythmic effects.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input

#### Outputs
- `Out L` (Audio) - Left gated output
- `Out R` (Audio) - Right gated output

#### Parameters
- `Threshold` (-60 to 0 dB) - Level below which the gate closes
- `Attack` (0.1-100 ms) - How quickly the gate opens
- `Release` (10-1000 ms) - How quickly the gate closes
- `Range` (-60 to 0 dB) - Amount of attenuation when gate is closed

#### How to Use
1. Set threshold just above your noise floor
2. Adjust attack and release for smooth or rhythmic gating
3. Use range to set how much the signal is reduced (not necessarily to silence)
4. Great for cleaning up noisy recordings
5. Use creatively with short releases for rhythmic chopping effects

---

### drive
**Waveshaping Distortion**

A simple waveshaping distortion effect using tanh saturation.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Drive Mod` (CV) - Drive amount modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left distorted output
- `Out R` (Audio) - Right distorted output

#### Parameters
- `Drive` (0-2) - Amount of distortion
- `Mix` (0-1) - Dry/wet mix (0=dry, 1=wet)
- `Relative Drive Mod` (Bool) - When enabled, CV modulates around slider. When disabled, CV directly maps to 0-2 range
- `Relative Mix Mod` (Bool) - When enabled, CV modulates around slider. When disabled, CV directly maps to 0-1 range

#### How to Use
1. Increase drive to add saturation
2. Adjust mix to blend processed signal with dry input (0.0 = 100% dry, 1.0 = 100% wet)
3. Use Relative Mod toggles to choose between musical (relative) or full-range (absolute) CV modulation
4. Great for adding warmth and character

---

### graphic_eq
**8-Band Graphic Equalizer**

An 8-band graphic equalizer with CV outputs for frequency-based triggering.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Band 1-8 Mod` (CV) - Individual band gain modulation
- `Gate Thresh Mod` (CV) - Gate threshold modulation
- `Trig Thresh Mod` (CV) - Trigger threshold modulation

#### Outputs
- `Out L` (Audio) - Left EQ output
- `Out R` (Audio) - Right EQ output
- `Gate Out` (Gate) - Gates when signal exceeds gate threshold
- `Trig Out` (Gate) - Triggers on transients above trigger threshold

#### Parameters
- `Gain Band 1-8` (-60 to +12 dB) - Gain for each frequency band (centered at: 60, 170, 310, 600, 1000, 3000, 6000, 12000 Hz)
- `Output Level` (-24 to +24 dB) - Overall output level
- `Gate Threshold` (-60 to 0 dB) - Threshold for gate output
- `Trigger Threshold` (-60 to 0 dB) - Threshold for trigger detection

#### How to Use
1. Boost or cut specific frequency bands to shape your sound
2. Use negative gain to remove unwanted frequencies
3. Use the gate and trigger outputs for frequency-responsive triggering (great for kick/bass triggering)
4. Combine with other modules for frequency-dependent effects

---

### waveshaper
**Multi-Algorithm Waveshaper**

A distortion effect with multiple waveshaping algorithms for varied saturation and distortion effects.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Drive Mod` (CV) - Drive amount modulation
- `Type Mod` (CV) - Algorithm selection modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left waveshaped output
- `Out R` (Audio) - Right waveshaped output

#### Parameters
- `Drive` (1-100) - Amount of waveshaping
- `Type` (Choice) - Waveshaping algorithm (Soft Clip, Hard Clip, Foldback)
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Relative Drive Mod` (Bool) - When enabled, CV modulates ±3 octaves around slider. When disabled, CV directly maps to 1-100 range

#### How to Use
1. Choose a waveshaping algorithm
2. Gradually increase drive to add saturation
3. Try different algorithms for different characters
4. Adjust mix to blend processed signal with dry input (0.0 = 100% dry, 1.0 = 100% wet)
5. Use Relative Drive Mod toggle to choose between musical (relative) or full-range (absolute) CV modulation
6. Great for parallel processing or subtle saturation effects

---

### 8bandshaper
**Multi-Band Waveshaper**

A multi-band waveshaper that applies frequency-specific distortion across 8 bands.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Drive 1-8 Mod` (CV) - Per-band drive modulation
- `Gain Mod` (CV) - Output gain modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left processed output
- `Out R` (Audio) - Right processed output

#### Parameters
- `Drive Band 1-8` (0-100) - Drive amount for each frequency band
- `Output Gain` (-24 to +24 dB) - Overall output level
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Relative Drive Mod 1-8` (Bool) - When enabled, CV modulates ±3 octaves around slider. When disabled, CV directly maps to 0-100 range
- `Relative Gain Mod` (Bool) - When enabled, CV modulates ±24 dB around slider. When disabled, CV directly maps to -24 to +24 dB range

#### How to Use
1. Adjust individual band drives to add selective distortion
2. Drive bass frequencies differently than highs for balanced distortion
3. Adjust mix to blend processed signal with dry input (0.0 = 100% dry, 1.0 = 100% wet)
4. Use Relative Mod toggles to choose between musical (relative) or full-range (absolute) CV modulation
5. Great for adding harmonics to specific frequency ranges
6. Use sparingly for subtle enhancement or aggressively for heavy distortion

---

### granulator
**Granular Synthesizer/Effect**

A granular processor that plays small grains of audio for textural and rhythmic effects.

#### Inputs
- `In L` (Audio) - Left audio input (recorded to internal buffer)
- `In R` (Audio) - Right audio input (recorded to internal buffer)
- `Trigger In` (Gate) - Manual grain triggering
- `Density Mod` (CV) - Grain density modulation
- `Size Mod` (CV) - Grain size modulation
- `Position Mod` (CV) - Playback position modulation
- `Pitch Mod` (CV) - Pitch modulation
- `Gate Mod` (CV) - Gate amount modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left granulated output
- `Out R` (Audio) - Right granulated output

#### Parameters
- `Density` (0.1-100 Hz) - How often grains are triggered
- `Size` (5-500 ms) - Length of each grain
- `Position` (0-1) - Where in the buffer to read grains
- `Spread` (0-1) - Random variation in grain position
- `Pitch` (-24 to +24 semitones) - Pitch shift of grains
- `Pitch Random` (0-12 semitones) - Random pitch variation per grain
- `Pan Random` (0-1) - Random stereo placement per grain
- `Gate` (0-1) - Overall output level/gate
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Relative Density Mod` (Bool) - When enabled, CV modulates around slider (0.5x-2x). When disabled, CV directly maps to 0.1-100 Hz range
- `Relative Size Mod` (Bool) - When enabled, CV modulates around slider (0.1x-2x). When disabled, CV directly maps to 5-500 ms range
- `Relative Position Mod` (Bool) - When enabled, CV adds offset to slider (±0.5 range). When disabled, CV directly maps to 0-1 range
- `Relative Pitch Mod` (Bool) - When enabled, CV adds offset to slider (±12 st). When disabled, CV directly maps to -24 to +24 st range

#### How to Use
1. Audio is continuously recorded to a 2-second buffer
2. Adjust density for grain triggering rate (low=sparse, high=dense cloud)
3. Set grain size (small=rhythmic, large=smooth textures)
4. Use position to read from different parts of the buffer
5. Add spread for more random, evolving textures
6. Modulate position with LFOs for scanning effects
7. Great for creating ambient textures from any sound source

---

### spatial_granulator
**Visual Canvas Granulator/Chorus**

A hybrid granular synthesizer and chorus effect with a visual canvas interface. Paint dots on a square canvas where each dot represents a voice or grain spawner. Combines the textural qualities of granular synthesis with the spatial richness of chorus effects.

#### Inputs
- `In L` (Audio) - Left audio input (recorded to internal buffer)
- `In R` (Audio) - Right audio input (recorded to internal buffer)
- `Dry Mix Mod` (CV) - Dry mix level modulation
- `Pen Mix Mod` (CV) - Pen voices mix level modulation
- `Spray Mix Mod` (CV) - Spray grains mix level modulation
- `Density Mod` (CV) - Grain density modulation
- `Grain Size Mod` (CV) - Grain size modulation

#### Outputs
- `Out L` (Audio) - Left processed output
- `Out R` (Audio) - Right processed output

#### Parameters
- `Dry Mix` (0-1) - Original input signal level (0=silent, 1=full)
- `Pen Mix` (0-1) - Pen tool voices mix level (0=silent, 1=full)
- `Spray Mix` (0-1) - Spray tool grains mix level (0=silent, 1=full)
- `Density` (0.1-100 Hz) - Grain spawning rate for Spray tool dots
- `Grain Size` (5-500 ms) - Length of each grain spawned by Spray tool
- `Buffer Length` (1-10 seconds) - Size of the internal recording buffer
- `Red Amount` (0-1) - Delay effect intensity multiplier
- `Green Amount` (0-1) - Filter effect intensity multiplier
- `Blue Amount` (0-1) - Pitch effect intensity multiplier

#### Canvas Controls
- **Tools:**
  - **Pen** - Draws static voices (chorus-like continuous playback)
  - **Spray** - Spawns dynamic grains (granular synthesis with movement and "pop" effects)
- **Colors (Color-Coded Parameters):**
  - **Red (Delay)** - Grid-based delay control:
    - X-axis: Delay time (0-2000ms, left=short, right=long)
    - Y-axis: Feedback amount (0.0-0.95, bottom=none, top=maximum)
    - Dot size: Controls delay intensity (scaled by Red Amount slider)
  - **Green (Filter)** - Grid-based filter control:
    - X-axis: Cutoff frequency (20Hz-20kHz logarithmic, left=low, right=high)
    - Y-axis: Resonance/Q (0.707-10.0, bottom=smooth, top=sharp)
    - Dot size: Controls filter intensity (scaled by Green Amount slider)
  - **Blue (Pitch)** - Grid-based pitch control:
    - X+Y position: Pitch shift (-24 to +24 semitones)
      - Bottom-left (0,0) = -24 semitones (low)
      - Top-right (1,1) = +24 semitones (high)
    - Dot size: Controls pitch shift intensity (scaled by Blue Amount slider)
- **Canvas Axes:**
  - **X-axis** - Panning (left = left, right = right) + Color-specific grid control
  - **Y-axis** - Buffer position (low = start of buffer, high = end of buffer) + Color-specific grid control
  - **Dot Size** - Controls both voice reproduction amount and color parameter intensity

#### How to Use
1. **Recording:** Audio is continuously recorded to the internal buffer (size set by Buffer Length parameter)
2. **Pen Tool (Static Voices):**
   - Click and drag to place dots on the canvas
   - Each dot creates a continuous voice playing from the buffer
   - Y-position determines where in the buffer to read from
   - X-position controls stereo panning
   - Color determines which parameter is active (Delay, Filter, or Pitch)
   - Dot size controls voice amplitude and parameter intensity
   - Great for creating thick, chorus-like textures
3. **Spray Tool (Dynamic Grains):**
   - Click and drag to place multiple dots (spray effect)
   - Each dot spawns grains periodically based on Density parameter
   - Grains have dynamic movement and "pop" effects for vibrant sound
   - Y-position determines grain read position in buffer
   - X-position controls grain panning
   - Color determines which parameter is active per grain
   - Great for creating evolving, granular textures
4. **Color Selection and Grid Control:**
   - **Red (Delay):** Place dots horizontally to control delay time, vertically to control feedback. Bottom-left = short delay, no feedback. Top-right = long delay, maximum feedback.
   - **Green (Filter):** Place dots horizontally to control cutoff frequency, vertically to control resonance. Bottom-left = low cutoff, smooth. Top-right = high cutoff, sharp resonance.
   - **Blue (Pitch):** Place dots diagonally to control pitch. Bottom-left = -24 semitones (low), top-right = +24 semitones (high). The average of X and Y positions determines pitch.
   - Use the color amount sliders (Red Amount, Green Amount, Blue Amount) to scale the intensity of each effect
   - Larger dots increase the intensity of the color-coded parameter
5. **Mix Controls:**
   - Adjust `Dry Mix` to blend the original input signal
   - Adjust `Pen Mix` to control the level of Pen tool voices
   - Adjust `Spray Mix` to control the level of Spray tool grains
   - All three can be mixed independently for flexible sound design
6. **CV Modulation:**
   - Connect CV sources to modulation inputs for automated control
   - `Dry Mix Mod`, `Pen Mix Mod`, `Spray Mix Mod` control respective mix levels
   - `Density Mod` and `Grain Size Mod` control Spray tool behavior
   - Modulation indicators show when parameters are being modulated
7. **Erasing:** Right-click and drag on the canvas to erase dots
8. **Canvas Interaction:**
   - Left-click/drag: Paint dots
   - Right-click/drag: Erase dots
   - Dots are static (no animation) - they represent fixed voice positions
9. **Tips:**
   - Use Pen tool with multiple dots for rich chorus effects
   - Use Spray tool with high density for dense granular clouds
   - Combine both tools on the same canvas for hybrid textures
   - Experiment with dot placement for spatial effects
   - Use grid-based color control to create musical intervals and harmonies
   - Larger dots = more prominent voices/grains
   - Color selection dramatically changes the character of the effect
   - Adjust color amount sliders to fine-tune effect intensity

#### Visual Feedback
- Dots are rendered as colored circles on the canvas
- Dot size visually represents the size parameter
- Color matches the selected color (Red/Green/Blue)
- Grid and crosshair help with precise placement
- Real-time visualization shows output waveform, active voice/grain counts, buffer fill level, and output level

#### Performance & Limits
- **Pen Tool Voices:** Up to 192 simultaneous active voices (each Pen dot can have one active voice)
- **Spray Tool Grains:** Up to 384 simultaneous active grains (shared across all Spray dots)
- When limits are reached, new voices/grains will not activate until existing ones finish
- CPU usage increases with more active voices/grains, especially when effects are enabled
- Monitor performance when using many dots with color-coded effects active

---

### harmonic_shaper
**Harmonic Content Shaper**

Shapes the harmonic content of a signal using frequency-specific waveshaping.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Freq Mod` (CV) - Frequency modulation
- `Drive Mod` (CV) - Drive modulation

#### Outputs
- `Out L` (Audio) - Left shaped output
- `Out R` (Audio) - Right shaped output

#### Parameters
- `Master Frequency` (20 Hz - 20 kHz) - Center frequency for harmonic shaping
- `Master Drive` (0-10) - Amount of harmonic emphasis

#### How to Use
1. Set the master frequency to target specific harmonics
2. Increase drive to emphasize those harmonics
3. Great for adding presence and character to sounds
4. Use on bass for sub-harmonic generation

---

### timepitch
**Time/Pitch Manipulation**

Real-time pitch and time manipulation using the RubberBand library for high-quality time stretching and pitch shifting.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Speed Mod` (CV) - Playback speed modulation
- `Pitch Mod` (CV) - Pitch shift modulation
- `Mix Mod` (CV) - Wet/dry mix modulation

#### Outputs
- `Out L` (Audio) - Left processed output
- `Out R` (Audio) - Right processed output

#### Parameters
- `Speed` (0.25x to 4x) - Playback speed without affecting pitch
- `Pitch` (-24 to +24 semitones) - Pitch shift without affecting tempo
- `Mix` (0-1) - Wet/dry balance (0=dry, 1=wet)
- `Engine` (Choice) - Processing engine (RubberBand or Naive)
- `Buffer Headroom` (0.25-8 s) - Internal buffer size for time stretching

#### How to Use
1. Adjust speed to time-stretch audio (0.5x=half speed, 2x=double speed)
2. Adjust pitch to transpose audio independently
3. Adjust mix to blend processed signal with dry input (0.0 = 100% dry, 1.0 = 100% wet)
4. Increase buffer headroom for safer slowdowns (adds latency)
5. Use Flush Buffer button to reset playback
6. Great for creative effects and sound design

---

### de_crackle
**Click/Pop Reducer**

A utility to reduce clicks and pops caused by discontinuous CV or audio signals.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input

#### Outputs
- `Out L` (Audio) - Left de-clicked output
- `Out R` (Audio) - Right de-clicked output

#### Parameters
- `Sensitivity` (Low/Medium/High) - How aggressively to detect and reduce clicks

#### How to Use
1. Insert after modules that produce discontinuous signals
2. Adjust sensitivity based on the severity of clicks
3. Essential for smoothing abrupt parameter changes
4. Use on CV signals as well as audio

---

## 3. MODULATOR NODES

Modulator nodes generate control voltages for animating parameters over time.

### lfo
**Low-Frequency Oscillator**

A versatile LFO for modulating parameters with periodic waveforms.

#### Inputs
- `Rate Mod` (CV) - Rate modulation input
- `Depth Mod` (CV) - Depth modulation input
- `Wave Mod` (CV) - Waveform selection modulation

#### Outputs
- `Out` (CV) - CV modulation output

#### Parameters
- `Rate` (0.05-20 Hz) - LFO frequency
- `Depth` (0-1) - Modulation amount
- `Bipolar` (Bool) - Output range: On = -1 to +1, Off = 0 to 1
- `Wave` (Choice) - Sine, Triangle, or Sawtooth
- `Sync` (Bool) - Sync to global tempo
- `Division` (Choice) - Note division when synced (1/32 to 8 bars)
- `Relative Mod` (Bool) - When enabled, rate CV is additive around slider position

#### How to Use
1. Set rate to desired modulation speed
2. Choose bipolar for modulation around a center point, unipolar for one-directional
3. Select waveform based on desired modulation shape
4. Enable Sync and set Division for tempo-locked modulation
5. Connect output to any CV modulation input
6. Use multiple LFOs at different rates for complex modulation

---

### adsr
**Attack-Decay-Sustain-Release Envelope Generator**

A classic ADSR envelope generator for shaping sounds over time.

#### Inputs
- `Gate In` (Gate) - Gate signal to trigger and hold envelope
- `Trigger In` (Gate) - Trigger signal to retrigger envelope
- `Attack Mod` (CV) - Attack time modulation
- `Decay Mod` (CV) - Decay time modulation
- `Sustain Mod` (CV) - Sustain level modulation
- `Release Mod` (CV) - Release time modulation

#### Outputs
- `Env Out` (CV) - Main envelope output (0-1)
- `Inv Out` (CV) - Inverted envelope output (1-0)
- `EOR Gate` (Gate) - End of Release gate
- `EOC Gate` (Gate) - End of Cycle gate

#### Parameters
- `Attack` (0.001-5 seconds) - Rise time from 0 to 1
- `Decay` (0.001-5 seconds) - Fall time from 1 to sustain level
- `Sustain` (0-1) - Held level while gate is high
- `Release` (0.001-5 seconds) - Fall time from sustain to 0 after gate goes low
- `Relative Attack/Decay/Sustain/Release Mod` (Bool) - Enable relative modulation modes

#### How to Use
1. Connect a gate source (sequencer, MIDI CV, etc.) to Gate In
2. Adjust Attack for how quickly sound reaches full volume
3. Set Decay for how quickly it falls to the sustain level
4. Sustain sets the held level while key/gate is pressed
5. Release controls fade-out time after gate is released
6. Connect Env Out to VCA gain for amplitude shaping
7. Use Inv Out for inverted modulation
8. EOR and EOC gates useful for triggering events at envelope completion

---

### random
**Random Value Generator**

Generates random values at a specified rate with multiple output formats and tempo sync.

#### Outputs
- `Norm Out` (CV) - Normalized random values (0-1 range)
- `Raw Out` (Raw) - Raw random values (custom range)
- `CV Out` (CV) - CV random values (custom CV range)
- `Bool Out` (Gate) - Random boolean (on/off)
- `Trig Out` (Gate) - Trigger pulse on each new random value

#### Parameters
- `Rate` (0.1-50 Hz) - How often new random values are generated
- `Min` (-100 to 100) - Minimum value for Raw output
- `Max` (-100 to 100) - Maximum value for Raw output
- `CV Min` (0-1) - Minimum value for CV output
- `CV Max` (0-1) - Maximum value for CV output
- `Norm Min` (0-1) - Minimum value for Norm output
- `Norm Max` (0-1) - Maximum value for Norm output
- `Slew` (0-1) - Smoothing between random values
- `Trig Threshold` (0-1) - Threshold for Bool output
- `Sync` (Bool) - Sync to global tempo
- `Division` (Choice) - Note division when synced

#### How to Use
1. Set rate for how often random values change
2. Adjust min/max ranges for each output type as needed
3. Use slew to smooth transitions between random values (0=stepped, 1=smooth)
4. Connect different outputs for different modulation needs
5. Use Bool Out for random gates/triggers
6. Enable sync for tempo-locked randomness
7. Great for adding unpredictability and variation to patches

---

### s_and_h
**Sample & Hold**

Samples and holds an input signal when triggered.

#### Inputs
- `Signal In L` (Audio) - Left signal to sample
- `Signal In R` (Audio) - Right signal to sample
- `Trig In L` (Gate) - Trigger for left channel
- `Trig In R` (Gate) - Trigger for right channel
- `Threshold Mod` (CV) - Trigger threshold modulation
- `Edge Mod` (CV) - Trigger edge selection modulation
- `Slew Mod` (CV) - Slew limiting modulation

#### Outputs
- `Out L` (Audio) - Left sampled & held output
- `Out R` (Audio) - Right sampled & held output

#### Parameters
- `Threshold` (0-1) - Trigger threshold level
- `Edge` (Choice) - Rising, Falling, or Both edges
- `Slew` (0-1) - Slew limiting between sampled values

#### How to Use
1. Connect a signal to sample (CV, audio, etc.)
2. Connect a trigger source (LFO, clock, gate)
3. Each trigger samples the current input value and holds it
4. Adjust threshold if using audio-rate triggers
5. Use slew to smooth transitions between held values
6. Classic for creating stepped random modulation (LFO → S&H → destination)

---

### function_generator
**Drawable Envelope/LFO Generator**

A complex, drawable envelope and LFO generator with multiple curve slots and extensive modulation options.

#### Inputs
- `Gate In` (Gate) - Gate input for envelope triggering
- `Trigger In` (Gate) - Trigger input for envelope
- `Sync In` (Gate) - Sync input for phase reset
- `Rate Mod` (CV) - Rate modulation
- `Slew Mod` (CV) - Slew limiting modulation
- `Gate Thresh Mod` (CV) - Gate threshold modulation
- `Trig Thresh Mod` (CV) - Trigger threshold modulation
- `Pitch Base Mod` (CV) - Pitch base modulation
- `Value Mult Mod` (CV) - Value multiplier modulation
- `Curve Select Mod` (CV) - Curve selection modulation

#### Outputs
- `Value` (CV) - Main output value
- `Inverted` (CV) - Inverted output
- `Bipolar` (CV) - Bipolar output (-1 to +1)
- `Pitch` (CV) - Pitch CV output (V/Oct)
- `Gate` (Gate) - Gate output based on threshold
- `Trigger` (Gate) - Trigger output
- `End of Cycle` (Gate) - Trigger at cycle end
- `Blue/Red/Green Value` (CV) - Per-curve outputs
- `Blue/Red/Green Pitch` (CV) - Per-curve pitch outputs

#### Parameters
- `Rate` (0.05-20 Hz) - Cycle speed
- `Slew` (0-1) - Smoothing between points
- `Gate Threshold` (0-1) - Threshold for gate output
- `Trig Threshold` (0-1) - Threshold for trigger output
- `Pitch Base` (-4 to +4 octaves) - Base pitch offset
- `Value Mult` (0-10) - Value scaling multiplier
- `Curve Select` (0-2) - Choose active curve (Blue, Red, or Green)
- Drawing Interface - Click and drag to draw curves

#### How to Use
1. Click "Draw" to enter drawing mode
2. Draw up to 3 different curves (Blue, Red, Green tabs)
3. Set rate for cycle speed
4. Use as LFO (free-running) or envelope (gate-triggered)
5. Adjust slew for smooth or stepped transitions
6. Multiple outputs allow simultaneous different modulations
7. Per-curve outputs let you route different curves to different destinations
8. Extremely versatile for complex modulation shapes

---

### shaping_oscillator
**Oscillator with Built-in Waveshaper**

An oscillator with integrated waveshaping for generating harmonically rich tones.

#### Inputs
- `In L` (Audio) - External audio input (optional, can shape external audio)
- `In R` (Audio) - External audio input right channel
- `Freq Mod` (CV) - Frequency modulation
- `Wave Mod` (CV) - Waveform modulation
- `Drive Mod` (CV) - Drive modulation
- `Dry/Wet Mod` (CV) - Dry/wet mix modulation

#### Outputs
- `Out L` (Audio) - Shaped oscillator output left channel
- `Out R` (Audio) - Shaped oscillator output right channel

#### Parameters
- `Frequency` (20 Hz - 20 kHz) - Oscillator frequency
- `Waveform` (Choice) - Base waveform (Sine, Saw, Square)
- `Drive` (1-50) - Waveshaping amount (1=clean, 50=extreme)
- `Dry/Wet` (0-1) - Mix between dry input (0.0) and wet processed signal (1.0)
- `Relative Freq Mod` (Bool) - When enabled, CV modulates ±4 octaves around slider. When disabled, CV directly maps to 20 Hz - 20 kHz range
- `Relative Drive Mod` (Bool) - When enabled, CV modulates around slider drive. When disabled, CV directly maps to full drive range

#### How to Use
1. Set frequency for desired pitch
2. Choose base waveform
3. Increase drive to add harmonics via waveshaping
4. Adjust dry/wet to blend between input and processed signal (0.0 = 100% dry, 1.0 = 100% wet)
5. Use Relative Freq Mod and Relative Drive Mod toggles to choose between musical (relative) or full-range (absolute) CV modulation
6. Can also process external audio through the shaper
7. Great for thick, harmonically rich tones
8. Use dry/wet for parallel processing or subtle effect blending

---

## 4. UTILITY & LOGIC NODES

Utility nodes provide essential signal processing, routing, and logic operations.

### vca
**Voltage-Controlled Amplifier**

A basic amp module for controlling audio levels with CV.

#### Inputs
- `In L` (Audio) - Left audio input
- `In R` (Audio) - Right audio input
- `Gain Mod` (CV) - Gain modulation

#### Outputs
- `Out L` (Audio) - Left amplified output
- `Out R` (Audio) - Right amplified output

#### Parameters
- `Gain` (-60 to +12 dB) - Base gain level

#### How to Use
1. Send audio through the VCA
2. Connect an envelope or LFO to Gain Mod for amplitude control
3. Essential for creating dynamic amplitude envelopes
4. The core of any subtractive synthesis voice

---

### mixer
**Stereo Audio Mixer**

A two-input stereo mixer with gain, pan, and crossfade controls.

#### Inputs
- `In A L/R` (Audio) - Input A stereo pair
- `In B L/R` (Audio) - Input B stereo pair
- `Gain Mod` (CV) - Gain modulation
- `Pan Mod` (CV) - Pan modulation
- `X-Fade Mod` (CV) - Crossfade modulation

#### Outputs
- `Out L/R` (Audio) - Mixed stereo output

#### Parameters
- `Gain` (-60 to +12 dB) - Overall output gain
- `Pan` (-1 to +1) - Stereo panning
- `Crossfade` (0-1) - Blend between input A (0) and input B (1)

#### How to Use
1. Connect two stereo sources
2. Use crossfade to blend between them
3. Adjust gain and pan for final mix
4. Modulate crossfade with LFOs or envelopes for dynamic mixing

---

### cv_mixer
**Control Voltage Mixer**

A mixer specifically designed for mixing CV signals.

#### Inputs
- `In A/B` (CV) - CV inputs
- `Gain Mod` (CV) - Gain modulation

#### Outputs
- `Out` (CV) - Mixed CV output

#### Parameters
- `Gain A/B` (-2 to +2) - Gain for each input

#### How to Use
1. Mix multiple CV sources together
2. Use negative gain to invert signals
3. Create complex modulation by combining LFOs and envelopes
4. Essential for additive CV processing

---

### track_mixer
**Multi-Channel Mixer**

A mixer for up to 8 monophonic tracks with individual gain and pan controls.

#### Inputs
- `In 1-8` (Audio) - 8 mono audio inputs
- `Num Tracks Mod` (Raw) - Number of active tracks modulation
- `Gain 1-8 Mod` (CV) - Per-track gain modulation
- `Pan 1-8 Mod` (CV) - Per-track pan modulation

#### Outputs
- `Out L/R` (Audio) - Stereo mixed output

#### Parameters
- `Num Tracks` (1-8) - Number of active tracks
- `Gain 1-8` (-60 to +12 dB) - Per-track gain
- `Pan 1-8` (-1 to +1) - Per-track stereo panning

#### How to Use
1. Connect multiple mono sources (great with PolyVCO outputs)
2. Adjust per-track gain and pan
3. Set Num Tracks to control how many inputs are active
4. Perfect for mixing polyphonic voices

---

### panvol
**2D Volume and Panning Control**

A compact, intuitive control surface that provides simultaneous volume and panning adjustment via a draggable circle on a 2D grid.

#### Inputs
- `Pan Mod` (CV) - Panning modulation input
- `Vol Mod` (CV) - Volume modulation input

#### Outputs
- `Pan Out` (CV) - Panning CV output (normalized 0.0 to 1.0, maps to -1.0 to +1.0 in mixers)
- `Vol Out` (CV) - Volume CV output (normalized 0.0 to 1.0, maps to -60dB to +6dB in mixers)

#### Parameters
- `Pan` (-1.0 to +1.0) - Panning position (-1.0 = Full Left, 0.0 = Center, +1.0 = Full Right)
- `Volume` (-60.0 to +6.0 dB) - Volume level (0.0 dB = Unity gain)

#### How to Use
1. **Interactive Control:**
   - Click and drag the circle on the grid to adjust both volume and panning simultaneously
   - Vertical movement controls volume (up = louder, down = quieter)
   - Horizontal movement controls panning (left = left, right = right)
   - Click anywhere on the grid to jump the circle to that position

2. **Connecting to Mixers:**
   - Connect `Pan Out` → Mixer's `Pan Mod` input
   - Connect `Vol Out` → Mixer's `Gain Mod` input
   - Or connect to Track Mixer's per-track pan and gain modulation inputs

3. **CV Modulation:**
   - Connect CV sources to `Pan Mod` and `Vol Mod` inputs for automated control
   - When CV is connected, manual control is disabled (indicated by cyan circle color)
   - Perfect for LFO-driven spatial effects or envelope-controlled volume sweeps

4. **Reset Button:**
   - Click "Reset to Center" to instantly return to center pan and unity gain

5. **Creative Applications:**
   - Live performance: Quick volume and pan adjustments during performance
   - Spatial mixing: Precise positioning of sounds in stereo field
   - Automation: Connect to sequencers or LFOs for automated movement
   - Gesture control: Connect to motion sensors or touch controllers for hands-free control

**Visual Feedback:**
- Orange circle = Manual control active
- Cyan circle = CV modulation active
- Grid lines and center crosshair for visual reference
- Axis labels (↑ Vol, ← Pan →) for orientation

---

### attenuverter
**Attenuate/Invert Signal**

Attenuates (reduces) and/or inverts CV or audio signals.

#### Inputs
- `In L/R` (Audio) - Stereo inputs
- `Amount Mod` (CV) - Amount modulation

#### Outputs
- `Out L/R` (Audio) - Processed outputs

#### Parameters
- `Amount` (-1 to +1) - Attenuation/inversion amount (0=silent, 0.5=half, 1=full, negative=inverted)

#### How to Use
1. Use positive values (0-1) to reduce signal levels
2. Use negative values (-1-0) to invert and reduce
3. Set to 0 for silence
4. Essential for scaling modulation amounts
5. Use to create inverted versions of CV for opposite modulation

---

### lag_processor
**Slew Limiter/Smoother**

Smooths abrupt changes in signals using independent rise and fall times.

#### Inputs
- `Signal In` (CV) - CV input to smooth
- `Rise Mod` (CV) - Rise time modulation
- `Fall Mod` (CV) - Fall time modulation

#### Outputs
- `Smoothed Out` (CV) - Smoothed CV output

#### Parameters
- `Rise Time` (0.1-1000 ms) - Time to reach rising values
- `Fall Time` (0.1-1000 ms) - Time to reach falling values

#### How to Use
1. Insert between a CV source and destination to smooth transitions
2. Use equal rise/fall times for symmetrical smoothing
3. Use different rise/fall for attack/release character
4. Great for portamento effects and smoothing stepped sequences
5. Can turn hard gates into smooth envelopes

---

### math
**Mathematical Operations**

Performs mathematical operations on two input signals with 17 different operation modes. Supports CV modulation of the operation selection and scroll-edit functionality.

#### Inputs
- `In A` (CV) - First operand
- `In B` (CV) - Second operand
- `Op Mod` (CV) - Operation selection modulation (0-1 maps to 17 operations)

#### Outputs
- `Out` (CV) - Result of the selected operation

#### Parameters
- `Value A` (-100 to 100) - Default value for A (used if not patched)
- `Value B` (-100 to 100) - Default value for B (used if not patched)
- `Operation` (Choice) - Mathematical operation to perform:
  - **Arithmetic:** Add, Subtract, Multiply, Divide
  - **Comparison:** Min, Max, A>B, A<B
  - **Power & Roots:** Power, Sqrt(A)
  - **Trigonometric:** Sin(A), Cos(A), Tan(A)
  - **Other:** Abs(A), Modulo, Fract(A), Int(A)

#### CV Modulation
- **Operation Mod (`Op Mod`):** Connect a CV source (0-1 range) to modulate the operation selection in real-time
  - CV 0.0 → Operation 0 (Add)
  - CV 1.0 → Operation 16 (A<B)
  - Evenly distributed across all 17 operations
  - When CV is connected, the operation dropdown is disabled and shows "(mod)" indicator

#### Scroll-Edit Support
- **Operation Dropdown:** Hover over the operation dropdown and use the mouse wheel to cycle through operations
  - Scroll up: Previous operation
  - Scroll down: Next operation
  - Only works when operation is not being modulated via CV
  - Provides quick, intuitive operation selection

#### How to Use
1. **Basic Operation:**
   - Connect CV sources to `In A` and `In B`, or use internal `Value A` and `Value B` sliders
   - Select the desired operation from the dropdown
   - The result appears at the `Out` output

2. **CV Modulation:**
   - Connect a CV source to `Op Mod` to dynamically change operations
   - Use LFOs, sequencers, or envelopes to create evolving mathematical transformations
   - CV range 0-1 maps evenly to all 17 operations

3. **Scroll-Edit:**
   - Hover over the operation dropdown
   - Use mouse wheel to quickly cycle through operations
   - Perfect for live performance and rapid experimentation

4. **Creative Applications:**
   - Use comparison operations (A>B, A<B) for logic gates and conditional routing
   - Combine trigonometric functions (Sin, Cos, Tan) with oscillators for complex waveforms
   - Use Modulo for creating repeating patterns and wraparound effects
   - Fract(A) extracts fractional parts for smooth, continuous modulation
   - Int(A) quantizes to integers for stepped sequences

5. **Operation Examples:**
   - **Add/Subtract:** Combine or offset CV signals
   - **Multiply:** Amplitude modulation, scaling
   - **Power:** Exponential curves, distortion shaping
   - **Min/Max:** Limiting, gating, signal selection
   - **Trigonometric:** Waveform generation, phase modulation
   - **Modulo:** Creating repeating cycles, wraparound effects

---

### map_range
**Value Range Mapper**

Remaps values from one range to another.

#### Inputs
- `Raw In` (Raw) - Input value to remap

#### Outputs
- `CV Out` (CV) - Remapped to 0-1 range
- `Audio Out` (Audio) - Remapped to audio-rate

#### Parameters
- `Min In` (-1000 to 1000) - Input range minimum
- `Max In` (-1000 to 1000) - Input range maximum
- `Min Out` (-1000 to 1000) - Output range minimum
- `Max Out` (-1000 to 1000) - Output range maximum

#### How to Use
1. Define your input range (min/max in)
2. Define your desired output range (min/max out)
3. Connect input signal
4. Output is linearly scaled to new range
5. Great for converting between different parameter ranges

---

### quantizer
**Musical Scale Quantizer**

Snaps continuous CV to musical scales.

#### Inputs
- `CV In` (CV) - Continuous pitch CV
- `Scale Mod` (CV) - Scale selection modulation
- `Root Mod` (CV) - Root note modulation

#### Outputs
- `Out` (CV) - Quantized pitch CV

#### Parameters
- `Scale` (Choice) - Musical scale (Major, Minor, Chromatic, Pentatonic, etc.)
- `Root` (Choice) - Root note (C, C#, D, etc.)

#### How to Use
1. Connect a continuous CV source (LFO, random, etc.)
2. Choose a musical scale
3. Set the root note
4. Output will snap to nearest note in the scale
5. Great for creating melodic sequences from random sources

---

### rate
**Rate Value Converter**

Converts raw values to normalized rate values for tempo-related modulation.

#### Inputs
- `Rate Mod` (CV) - Rate modulation input

#### Outputs
- `Out` (CV) - Normalized rate output

#### Parameters
- `Rate` (0.1-20) - Rate multiplier

#### How to Use
1. Use to convert between different rate representations
2. Useful for tempo-syncing external modulators
3. Provides standardized rate output for consistent timing

---

### comparator
**Threshold Comparator**

Outputs a gate signal when input exceeds a threshold.

#### Inputs
- `In` (CV) - CV input to compare

#### Outputs
- `Out` (Gate) - Gate output (high when input > threshold)

#### Parameters
- `Threshold` (0-1) - Comparison threshold

#### How to Use
1. Connect a CV source
2. Set threshold
3. Output goes high when input exceeds threshold
4. Great for converting CV to gates
5. Create rhythm from slow LFOs

---

### logic
**Boolean Logic Operations**

Performs boolean logic operations on gate signals.

#### Inputs
- `In A` (Gate) - First input
- `In B` (Gate) - Second input

#### Outputs
- `AND` (Gate) - High when both inputs are high
- `OR` (Gate) - High when either input is high
- `XOR` (Gate) - High when inputs differ
- `NOT A` (Gate) - Inverted A

#### How to Use
1. Connect two gate sources
2. Use outputs for different logic combinations
3. AND: Both gates must be high (good for requiring multiple conditions)
4. OR: Either gate can be high (good for combining triggers)
5. XOR: Only one gate high (good for alternating patterns)
6. NOT A: Invert a gate signal

---

### clock_divider
**Clock Division/Multiplication**

Divides and multiplies clock signals for polyrhythmic patterns.

#### Inputs
- `Clock In` (Gate) - Clock input to divide/multiply
- `Reset` (Gate) - Reset all divisions to sync

#### Outputs
- `/2, /4, /8` (Gate) - Divided clocks (half, quarter, eighth speed)
- `x2, x3, x4` (Gate) - Multiplied clocks (double, triple, quadruple speed)

#### How to Use
1. Connect a clock source
2. Use divided outputs for slower rhythms
3. Use multiplied outputs for faster rhythms
4. Create polyrhythmic patterns by using multiple outputs
5. Use reset to synchronize all divisions

---

### sequential_switch
**Signal Router**

Routes an input signal to one of four outputs based on CV thresholds.

#### Inputs
- `Gate In` (Audio) - Signal to route
- `Thresh 1-4 CV` (CV) - Threshold values for each output

#### Outputs
- `Out 1-4` (Audio) - Four possible output destinations

#### Parameters
- `Threshold 1-4` (0-1) - Threshold levels

#### How to Use
1. Connect a signal to Gate In
2. Set thresholds for each output
3. As input CV changes, signal routes to different outputs
4. Use with sequencers or LFOs for rhythmic switching
5. Great for creating evolving patterns

---

## 5. SEQUENCER NODES

Sequencer nodes generate rhythmic and melodic patterns.

### chord_arp
**Chord & Arpeggiator Harmony Brain**

Generates chords and arpeggios from simple CV inputs, acting as a compact “harmony brain” that can drive oscillators, samplers, or filters with musical pitch/gate patterns.

#### Inputs
- `Degree In` (CV) - Normalized degree control (0-1). Intended to select scale degree or chord position (behavior will expand in future versions).
- `Root CV In` (CV) - Base pitch CV for the chord (0-1 range in the initial implementation).
- `Chord Mode Mod` (CV) - Modulation input for chord mode selection.
- `Arp Rate Mod` (CV) - Modulation input for arpeggiator rate.

#### Outputs
- `Pitch 1-4` (CV) - Four chord voice pitch outputs (normalized 0-1, typically mapped to V/Oct inputs via downstream modules).
- `Gate 1-4` (Gate) - Per-voice gates, high while the node considers the chord “active”.
- `Arp Pitch` (CV) - Single-note arp pitch output, cycling across active voices.
- `Arp Gate` (Gate) - Short pulses when the arp steps to a new note.

#### Parameters
- `Scale` (Choice) - Musical scale (Major, Natural Minor, Harmonic Minor, Dorian, Mixolydian, Pent Maj, Pent Min).
- `Key` (Choice) - Root key (C, C#, D, …, B).
- `Chord Mode` (Choice) - Chord quality type (Triad, Seventh).
- `Voicing` (Choice) - Voicing style (Close, Spread). Designed for richer voicing logic in future updates.
- `Arp Mode` (Choice) - Arpeggiator behavior (Off, Up, Down, UpDown, Random).
- `Arp Division` (Choice) - Musical rate division for arp steps (1/1 to 1/16).
- `Use External Clock` (Bool) - When enabled, lets external clocking drive arp timing (planned; current skeleton uses internal timing derived from CV and transport).
- `Voices` (1-4) - Number of active chord voices.

#### Visualization
- Embedded mini-visualizer at the top of the node shows:
  - Text strip: `Degree` and `Root CV` values from the input CVs.
  - Four voice bars: current chord voice pitches (1–4).
  - Rightmost bar: current arpeggiated pitch.
  - Colored border flash: `Arp Gate` activity (brief pulses when a new arp step fires).

#### How to Use
1. **Basic Chord Source**
   - Connect `Pitch 1-4` to oscillator pitch CV inputs (e.g., `vco`, `polyvco`, or multiple voices via mixers).
   - Connect `Gate 1-4` to envelope or VCA gate inputs for each voice.
   - Set `Scale`, `Key`, `Chord Mode`, and `Voicing` to define the harmonic flavor.
2. **Driving the Arp**
   - Feed a steady or sequenced pitch into `Root CV In` (from `sequencer`, `multi_sequencer`, or `midi_cv`).
   - Use `Arp Mode` and `Arp Division` to choose arp movement and speed.
   - Use `Arp Pitch` / `Arp Gate` when you need a single melodic line instead of full chords.
3. **Modulation & Performance**
   - Patch slow CV into `Degree In` to sweep through harmonic positions.
   - Use `Chord Mode Mod` and `Arp Rate Mod` with LFOs or envelopes to morph between chord types and arp rates over time.
   - Combine with `tempo_clock` and `bpm_monitor` for transport-aware rhythmic patches.

> Note: The initial implementation focuses on safe, musical scaffolding (normalized CV and basic chord offsets). Later updates will deepen theory-aware degree handling, voicing rules, and external clock integration.

---

### sequencer
**16-Step CV/Gate Sequencer**

A classic 16-step sequencer for creating melodies and rhythms.

#### Inputs
- Extensive per-step modulation inputs for values, triggers, and gates

#### Outputs
- `Pitch` (CV) - Current step pitch value
- `Gate` (Gate) - Gate output
- `Gate Nuanced` (CV) - Gate with velocity
- `Velocity` (CV) - Velocity value
- `Mod` (CV) - Modulation output
- `Trigger` (Gate) - Trigger on each step

#### Parameters
- `Rate` (0.1-20 Hz) - Sequence speed
- `Num Steps` (1-16) - Number of active steps
- `Gate Length` (0-1) - Duration of gates
- Per-step: Pitch, Gate, Velocity, Modulation values

#### How to Use
1. Set number of steps and rate
2. Program pitch values for each step
3. Set gates on/off for rhythm
4. Adjust gate length for articulation
5. Can sync to Tempo Clock for musical timing

---

### multi_sequencer
**Advanced Multi-Output Sequencer**

An advanced sequencer with parallel per-step outputs for polyphonic sequencing.

#### Outputs
- Live outputs: Pitch, Gate, Trigger, Velocity, Mod
- Parallel outputs: Pitch 1-16, Gate 1-16, Trig 1-16 (all steps output simultaneously)

#### How to Use
1. Similar to Sequencer but with simultaneous output of all 16 steps
2. Connect parallel outputs to PolyVCO for poly synth
3. Create complex polyphonic arrangements
4. Each step can trigger independently

---

### tempo_clock
**Global Clock Generator**

Master tempo/clock source with transport controls.

#### Inputs
- `BPM Mod` (CV) - BPM modulation
- `Tap` (Gate) - Tap tempo input
- `Nudge+/-` (Gate) - Fine tempo adjustment
- `Play/Stop/Reset` (Gate) - Transport controls
- `Swing Mod` (CV) - Swing amount modulation

#### Outputs
- `Clock` (Gate) - Main clock pulse
- `Beat Trig` (Gate) - Trigger on each beat
- `Bar Trig` (Gate) - Trigger on each bar
- `Beat Gate` (Gate) - Gate for beat duration
- `Phase` (CV) - Clock phase (0-1)
- `BPM CV` (CV) - BPM as CV
- `Downbeat` (Gate) - First beat of bar

#### Parameters
- `BPM` (20-300) - Tempo in beats per minute
- `Time Signature` (Choice) - 4/4, 3/4, 6/8, etc.
- `Swing` (0-100%) - Swing amount
- `Global Division` (Bool) - Override all synced modules' divisions

#### How to Use
1. Set BPM for your project
2. Use clock outputs to drive sequencers and LFOs
3. Enable Global Division to control all synced modules at once
4. Use transport controls for performance

---

### snapshot_sequencer
**Patch State Sequencer**

Sequences complete patch states, recalling all parameter values.

#### How to Use
1. Create snapshots of your entire patch at different states
2. Sequence through snapshots for dramatic changes
3. Great for live performance and automation

---

### stroke_sequencer
**Gesture-Based Beat/Stroke Sequencer**

Draw strokes on a canvas and generate timing/gate events when the playhead crosses user-defined threshold lines. Functions as a beat-maker: three independent trigger lanes (Floor/Mid/Ceiling) can fire per cycle and can auto-wire to samplers/mixers via the in-module quick-connect button. Also produces continuous and held pitch CV derived from the stroke under the playhead.

#### How to Use
1. Draw with Left-click (adds points); Erase with Right-click (cuts segments)
2. Set threshold lines (Floor/Mid/Ceiling) to define where triggers should occur
3. Run free (Rate) or sync to Transport with musical Division
4. Use 3 trigger outputs as drum lanes, and the quick-connect button to build a drum kit
5. Map Continuous/Held Pitch CV to tonal parameters (oscillator, filter, etc.)

#### Inputs
- `Reset In` (Gate) - Resets playhead to start (immediate, sample-accurate)
- `Rate Mod In` (CV) - Modulates Rate in free-run mode (0-1 maps to full range)
- `Floor Mod In` (CV) - Modulates Floor threshold (0-1)
- `Mid Mod In` (CV) - Modulates Mid threshold (0-1)
- `Ceiling Mod In` (CV) - Modulates Ceiling threshold (0-1)

#### Outputs
- `Floor Trig Out` (Gate) - Trigger when stroke crosses Floor
- `Mid Trig Out` (Gate) - Trigger when stroke crosses Mid
- `Ceiling Trig Out` (Gate) - Trigger when stroke crosses Ceiling
- `Continuous Pitch` (CV) - Current stroke Y under playhead (0-1), live
- `Floor Pitch` (CV) - Held pitch while on a stroke that crosses Floor
- `Mid Pitch` (CV) - Held pitch while on a stroke that crosses Mid
- `Ceiling Pitch` (CV) - Held pitch while on a stroke that crosses Ceiling

#### Parameters
- `Sync to Transport` (Bool) - When on, playhead follows global transport
- `Division` (Choice) - Musical division for sync mode (1/32 … 8). May be overridden by Tempo Clock’s Global Division
- `Rate` (0.1-20 Hz) - Free-run speed (log slider). Active only when Sync is off
- `Floor Y` (0-1) - Floor threshold height
- `Mid Y` (0-1) - Mid threshold height
- `Ceiling Y` (0-1) - Ceiling threshold height
- `Playhead` (0-1) - Manual scrub control (UI slider; takes temporary priority over auto-advance while dragging)

#### Behavior Details
- Triggers fire on any crossing of a horizontal threshold (upwards or downwards) by the line segment swept between consecutive playhead samples
- The sequencer is “primed” only while on a stroke; crossings are ignored in gaps
- In sync mode, playhead is computed from global song position × selected Division; wraps at 1.0
- In free-run mode, Rate sets per-sample increment; wraps at 1.0
- Reset gate immediately zeros playhead and phase

#### Beat Maker / Quick-Connect
- Button: “BUILD DRUM KIT” auto-creates a 3-pad drum kit (3 sample players + mixer) and wires:
  - `Floor Trig Out` → Kick sampler trigger
  - `Mid Trig Out` → Snare sampler trigger
  - `Ceiling Trig Out` → Hi-hat sampler trigger
  - Mixer output routed appropriately
- Use Auto-Connect shortcuts with multiple nodes selected:
  - Press `Y` to chain Gate outputs to Gate inputs
  - Press `B` to chain CV outputs (e.g., Pitch) to CV inputs

#### UI Notes
- Canvas: 840×360 area with retro visual theme; active strokes under the playhead highlight thicker and brighter
- Threshold lines show subtle gradients; vertical slider controls to the right match their colors
- L-Click: draw; R-Click: erase (eraser radius around mouse)
- “CLEAR” removes all strokes
- Manual playhead slider shows the live position at all times; dragging temporarily takes control

#### Presets
- Stroke preset dropdown with Save/Delete management:
  - Save current strokes and transport settings as a named preset
  - Load to replace current strokes
  - Presets persist with patches

---

### timeline
**Automation Recorder and Playback**

A transport-synchronized automation recorder that captures and plays back CV, Gate, Trigger, and Raw signals with sample-accurate precision. The Timeline Node serves as the single source of truth for temporal automation in the modular synthesizer.

**Inputs (Dynamic):**
- `[Channel Name] In` (CV) - One input per automation channel (up to 32 channels)

**Outputs (Dynamic):**
- `[Channel Name] Out` (CV) - One output per automation channel (up to 32 channels)

#### Parameters
- `Record` (Bool) - Enable recording mode (mutually exclusive with Play)
- `Play` (Bool) - Enable playback mode (mutually exclusive with Record)
- `Add Channel` (Button) - Create a new automation channel
- `Remove Channel` (Button) - Remove the last automation channel

#### How to Use
1. **Setup Channels:**
   - Click "Add Channel" to create automation channels
   - Each channel can record a separate signal (CV, Gate, Trigger, or Raw)
   - Channels are automatically named (e.g., "Channel 1", "Channel 2")
   
2. **Recording:**
   - Connect signals you want to record to the Timeline's input pins
   - Click the "● REC" button to enable recording
   - Start the global transport (Tempo Clock)
   - The Timeline records keyframes with sample-accurate timing
   - Recording automatically detects value changes (only stores new keyframes when values change)
   - Click "● REC" again to stop recording
   
3. **Playback:**
   - Click the "▶ PLAY" button to enable playback
   - Start the global transport
   - The Timeline plays back recorded automation, interpolating between keyframes
   - Playback is sample-accurate and synchronized to the global tempo
   
4. **Visualization:**
   - Select a channel from the list to view its keyframes
   - A waveform plot shows the recorded automation curve
   - The UI displays the current playback position in Bar:Beat:Tick format
   
5. **Persistence:**
   - Automation data is automatically saved with presets
   - All channels and keyframes are preserved when saving/loading patches
   
6. **Tips:**
   - Use multiple channels to record different parameters simultaneously
   - Record is mutually exclusive with Play - switch modes as needed
   - The Timeline passes through signals when neither Record nor Play is active (zero-latency monitoring)
   - Great for creating complex automation that syncs perfectly to tempo

**Technical Details:**
- Synchronized with global `TransportState` (from Tempo Clock)
- Sample-accurate keyframe recording and playback
- Linear interpolation between keyframes during playback
- Thread-safe data access (audio thread + UI thread)
- XML persistence via `getExtraStateTree()` / `setExtraStateTree()`
- Dynamic I/O pins based on number of automation channels

---

### automation_lane
**Drawable Automation Curve Lane**

A drawable automation lane that allows you to draw and edit automation curves directly on an infinitely scrolling timeline. Features a fixed center playhead with the timeline scrolling underneath, similar to a traditional DAW automation lane. Ideal for creating complex, hand-drawn modulation curves with precise timing control. Includes a trigger output that fires when the automation curve crosses a user-defined threshold line.

#### Outputs
- `Value` (CV) - Main automation output (0-1 range)
- `Inverted` (CV) - Inverted output (1-0 range)
- `Bipolar` (CV) - Bipolar output (-1 to +1 range)
- `Pitch` (CV) - Pitch CV output (0-10V range, V/Oct compatible)
- `Trigger` (Gate) - Trigger output that fires when the automation curve crosses the threshold line

#### Parameters
- `Rate` (0.01-20 Hz) - Playback rate in free mode
- `Mode` (Choice) - Free (Hz) or Sync (tempo-synchronized)
- `Division` (Choice) - Musical division when synced (1/32, 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars, 8 Bars)
- `Loop` (Bool) - Enable looping playback
- `Record/Edit` (Choice) - Toggle between recording mode (auto-scroll) and edit mode (manual scroll)
- `Duration Mode` (Choice) - User Choice, 1 Bar, 2 Bars, 4 Bars, 8 Bars, 16 Bars, 32 Bars (loop duration when sync is enabled)
- `Custom Duration` (1-256 beats) - Custom loop duration (only active when Duration Mode is "User Choice")
- `Zoom` (10-200 pixels/beat) - Timeline zoom level (UI only)
- `Trigger Threshold` (0.0-1.0) - Threshold level for trigger output (shown as horizontal line on timeline)
- `Trigger Edge` (Choice) - Trigger on Rising edge (below to above), Falling edge (above to below), or Both edges

#### How to Use
1. **Drawing Automation:**
   - Click and drag on the timeline canvas to draw automation curves
   - Vertical position controls the output value (top = 1.0, bottom = 0.0)
   - The timeline scrolls infinitely - draw as long as you need

2. **Playback Modes:**
   - **Free Mode:** Set rate in Hz for independent playback speed
   - **Sync Mode:** Sync to global transport with musical divisions
   - Enable Loop to repeat the automation pattern

3. **Record vs Edit Mode:**
   - **Record Mode:** Timeline auto-scrolls during playback, follow the playhead
   - **Edit Mode:** Manual scrolling for precise editing of existing curves

4. **Multiple Output Formats:**
   - Use `Value` for standard 0-1 modulation
   - Use `Inverted` for reverse modulation curves
   - Use `Bipolar` for modulation centered around zero
   - Use `Pitch` for V/Oct pitch control
   - Use `Trigger` for event-based triggering when the curve crosses the threshold

5. **Trigger Output:**
   - Adjust the `Trigger Threshold` slider to set the threshold line position (visible on the timeline)
   - Select `Trigger Edge` mode: Rising (crosses upward), Falling (crosses downward), or Both (any crossing)
   - The trigger fires a 1ms pulse when the automation curve crosses the threshold line
   - Perfect for triggering events based on automation curve shape (e.g., trigger envelope when curve rises above 0.7)
   - Connect to sequencer triggers, envelope gates, or any trigger input

6. **Timeline Navigation:**
   - Scroll horizontally to navigate the timeline
   - Adjust zoom to see more or less detail
   - Grid lines show beat divisions for reference
   - Fixed center playhead makes it easy to see current position

7. **Example Patches:**
   - **Filter Sweeps:** Draw filter cutoff automation and connect to VCF
   - **Volume Automation:** Use Value output to control VCA gain
   - **Pitch Sequences:** Draw melodic lines and use Pitch output to VCO
   - **Complex Modulation:** Combine with other modulators for layered automation
   - **Trigger-Based Events:** Use Trigger output to fire envelopes or advance sequencers when automation crosses a threshold
   - **Dynamic Gating:** Draw volume automation with trigger points to create rhythmic gating patterns

8. **Tips:**
   - Draw smooth curves by dragging slowly
   - Use sync mode for tempo-locked automation
   - Enable loop for repeating patterns
   - Zoom out to see the big picture, zoom in for fine editing
   - Perfect for creating evolving, non-repetitive modulation

**Visual Features:**
- Fixed center playhead (doesn't move, timeline scrolls underneath)
- Infinite scrolling timeline (draw as long as needed)
- Beat and bar grid lines for musical reference
- Real-time curve visualization with bold, visible lines
- Trigger threshold line displayed on the timeline
- Sample-accurate playback

**Technical Details:**
- Chunk-based storage for efficient handling of long automation data
- Thread-safe data access between audio and UI threads
- Sample-accurate interpolation during playback
- Transport-synchronized when in Sync mode
- XML persistence via `getExtraStateTree()` / `setExtraStateTree()`

---

### automato
**Gesture Recorder and Playback**

A compact module that records user gestures on a 2D grid and replays them with transport synchronization. Features Record/Edit mode toggle, 7 CV outputs, and time-based sample storage. Perfect for capturing expressive movements and creating evolving modulation patterns.

#### Outputs
- `X` (CV) - X coordinate output (0-1 range)
- `Y` (CV) - Y coordinate output (0-1 range)
- `Combined` (CV) - Combined X&Y output (0-1 range)
- `Value` (CV) - Combined value output (0-1 range)
- `Inverted` (CV) - Inverted output (1-0 range)
- `Bipolar` (CV) - Bipolar output (-1 to +1 range)
- `Pitch` (CV) - Pitch CV output (0-10V range, V/Oct compatible)

#### Parameters
- `Record/Edit` (Choice) - Toggle between recording mode and edit mode
- `Sync` (Bool) - Sync to global transport
- `Division` (Choice) - Musical division when synced (1/32, 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars, 8 Bars)
- `Loop` (Bool) - Enable looping playback
- `Rate` (0.01-20 Hz) - Playback rate in free mode (when sync is off)

#### How to Use
1. **Recording Gestures:**
   - Click the "REC" button to enter recording mode
   - Click and drag on the 2D grid to record your gesture
   - The module captures X, Y coordinates over time as you move the mouse
   - Recording continues until you click "REC" again or switch to Edit mode
   - Gestures are stored with time-based samples for accurate playback

2. **Playback Modes:**
   - **Sync Mode:** Sync to global transport with musical divisions
   - **Free Mode:** Independent playback speed controlled by Rate parameter
   - Enable Loop to repeat the recorded gesture continuously

3. **Record vs Edit Mode:**
   - **Record Mode:** Click and drag on the grid to record new gestures
   - **Edit Mode:** View and edit existing recorded gestures (editing features may be expanded in future updates)

4. **Multiple Output Formats:**
   - Use `X` and `Y` for independent coordinate control
   - Use `Combined` for averaged X&Y values
   - Use `Value` for standard 0-1 modulation
   - Use `Inverted` for reverse modulation curves
   - Use `Bipolar` for modulation centered around zero
   - Use `Pitch` for V/Oct pitch control

5. **Example Patches:**
   - **2D Filter Control:** Connect `X` → VCF Cutoff, `Y` → VCF Resonance
   - **Pan/Volume Automation:** Connect `X` → Pan Mod, `Y` → Volume Mod
   - **Dual Oscillator Control:** Connect `X` → VCO 1 Frequency, `Y` → VCO 2 Frequency
   - **Complex Modulation:** Use different outputs to control multiple parameters simultaneously

6. **Tips:**
   - Record smooth gestures by dragging slowly and steadily
   - Use sync mode for tempo-locked playback
   - Enable loop for repeating gesture patterns
   - Combine with other modulators for layered automation
   - Perfect for capturing expressive, hand-drawn modulation curves

**Visual Features:**
- 2D grid interface (similar to PanVol module)
- Recorded gesture path visualization
- Playhead indicator showing current playback position
- Simple Record/Edit button toggle

**Technical Details:**
- Time-based sample storage (X, Y pairs)
- Chunk-based storage for efficient handling of long recordings
- Thread-safe data access between audio and UI threads
- Sample-accurate interpolation during playback
- Transport-synchronized when in Sync mode
- XML persistence via `getExtraStateTree()` / `setExtraStateTree()`
- Supports unlimited recording duration

---

## 6. MIDI NODES

MIDI nodes handle MIDI input/output and conversion to CV.

### midi_cv
**MIDI to CV Converter**

Converts incoming MIDI notes to CV/Gate signals (monophonic).

#### Outputs
- `Pitch` (CV) - Note pitch as CV (V/Oct)
- `Gate` (Gate) - Note on/off gate
- `Velocity` (CV) - Note velocity
- `Mod Wheel` (CV) - CC1 modulation wheel
- `Pitch Bend` (CV) - Pitch bend wheel
- `Aftertouch` (CV) - Channel aftertouch

#### How to Use
1. Connect MIDI controller or use virtual MIDI
2. Play notes → outputs CV/Gate
3. Use with VCO + VCA + ADSR for classic synth voice
4. Monophonic (last note priority)

---

### midi_player
**MIDI File Player**

Plays MIDI files with per-track CV/Gate outputs.

#### How to Use
1. Load a MIDI file
2. Outputs CV/Gate for each MIDI track
3. Great for backing tracks or complex sequences

---

### midi_faders
**MIDI-Learnable Faders (1-16)**

1-16 MIDI-learnable faders with customizable output ranges.

#### Outputs
- `Fader 1-16` (CV) - CV outputs (0-1 range)

#### Parameters
- MIDI Learn for each fader
- Min/Max output range per fader

#### How to Use
1. Click MIDI Learn
2. Move a fader on your controller
3. Fader is now linked
4. Adjust output ranges as needed

---

### midi_knobs
**MIDI-Learnable Knobs (1-16)**

Similar to MIDI Faders but optimized for rotary controls.

---

### midi_buttons
**MIDI-Learnable Buttons (1-32)**

1-32 MIDI-learnable buttons with Gate/Toggle/Trigger modes.

#### Outputs
- `Button 1-32` (Gate) - Gate/trigger outputs

**Modes:**
- Gate: High while pressed
- Toggle: Alternates on/off each press
- Trigger: Brief pulse on press

---

### midi_jog_wheel
**MIDI Jog Wheel Control**

A single MIDI-learnable jog wheel for expressive modulation.

**Output:**
- `Value` (CV) - Wheel position/velocity

---

## 7. ANALYSIS NODES

Analysis nodes visualize and inspect signals.

### scope
**Oscilloscope**

Visualizes audio or CV signals over time.

**Inputs/Outputs:**
- `In/Out` (Audio) - Pass-through with visualization

#### Parameters
- `Window Size` (0.5-20 seconds) - Time window to display
- `Trigger Mode` (Choice) - Free-run, Rising Edge, Falling Edge
- `Trigger Level` (0-1) - Trigger threshold

#### How to Use
1. Insert in signal path (pass-through)
2. Adjust window size to see desired time range
3. Use trigger modes for stable waveform display
4. Great for debugging and sound design

---

### debug
**Signal Value Logger**

Logs signal value changes to the console.

#### How to Use
1. Insert in CV path
2. Logs values to console when they change
3. Great for troubleshooting CV routing
4. No audio output (endpoint)

---

### input_debug
**Passthrough Debug Logger**

Like Debug but with pass-through output.

---

### frequency_graph
**Spectrum Analyzer**

High-resolution real-time spectrum analyzer with frequency-based gate outputs.

#### Inputs
- `In` (Audio) - Mono audio to analyze

#### Outputs
- `Out L/R` (Audio) - Stereo pass-through
- `Sub/Bass/Mid/High Gate` (Gate) - Per-band gate outputs
- `Sub/Bass/Mid/High Trig` (Gate) - Per-band trigger outputs

#### Parameters
- `Gate Threshold` per band - Threshold for gate outputs

#### How to Use
1. Send audio through for visualization
2. Displays frequency spectrum in real-time
3. Use gate/trigger outputs for frequency-reactive triggering
4. Great for kick drum detection, bass triggering, etc.

---

## 8. SPECIAL NODES

Special nodes provide unique functionality beyond traditional synthesis.

### tts_performer
**Text-to-Speech Engine**

Advanced text-to-speech with word-level sequencing.

#### Inputs
- Per-word trigger inputs (1-16)
- Rate, Gate, Speed, Pitch modulation

#### Outputs
- `Audio` - Speech audio output
- `Word Gate` - Gate while speaking
- `EOP Gate` - End of phrase gate
- Per-word gates and triggers (1-16)

#### Parameters
- Text input field
- Voice selection
- Rate, pitch, speed controls

#### How to Use
1. Type text into text field
2. Choose voice
3. Trigger individual words or play entire phrase
4. Use per-word gates for word-synced events
5. Modulate pitch/speed for effects

---

### vocal_tract_filter
**Formant Filter**

Simulates human vowel sounds through formant filtering.

#### Parameters
- `Vowel Shape` - Continuous blend between vowels (A, E, I, O, U)
- `Formant Shift` - Shift formant frequencies up/down
- `Instability` - Add human-like variation
- `Gain` - Formant emphasis

#### How to Use
1. Send audio through (great with sawtooth waves)
2. Adjust vowel shape to morph between vowels
3. Modulate with LFOs for talking/singing effects
4. Use with TTS Performer for enhanced vocal synthesis

---

### physics
**2D Physics Simulation**

A 2D physics engine that outputs collision and contact data as CV.

#### How to Use
1. Create physics objects in the UI
2. Set gravity, friction, elasticity
3. Objects collide and interact
4. Outputs include position, velocity, collision events
5. Use outputs to drive synthesis parameters
6. Experimental and creative

---

### animation
**3D Animation Player**

Loads and plays 3D animations, outputs joint positions and velocities.

#### How to Use
1. Load 3D animation file (FBX, etc.)
2. Play animation
3. Outputs joint positions as CV
4. Drive synthesis from motion capture data

---

## 9. COMPUTER VISION NODES

Computer vision nodes process video for audio/CV generation.

### webcam_loader
**Webcam Video Source**

Captures video from webcam and publishes a `Source ID` for vision processing modules.

#### Outputs
- `Source ID` (Video) - Video source identifier

---

### video_file_loader
**Video File Source**

Loads and plays video files; publishes a `Source ID` for vision processing modules.

#### Outputs
- `Source ID` (Video) - Video source identifier

---

### movement_detector
**Motion Detection**

Analyzes video for motion via optical flow or background subtraction. Detects feature points (displayed as blue circles) and tracks their movement between frames.

#### Inputs
- `Source In` (Video) - Video source ID

#### Outputs
- `Motion X` (CV) - Horizontal motion component (-1 to +1)
- `Motion Y` (CV) - Vertical motion component (-1 to +1)
- `Amount` (CV) - Total motion magnitude (0-1)
- `Trigger` (Gate) - Trigger pulse on significant motion (above sensitivity threshold)
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High while movement is detected inside any rectangle of the corresponding zone color
- `Video Out` (Video) - Passthrough video output with motion visualization (blue feature points and green motion vectors)

#### Parameters
- `Mode` (Choice) - Optical Flow or Background Subtraction
- `Sensitivity` (0.01-1.0) - Motion detection threshold (higher = less sensitive)
- `Max Features` (20-500) - Maximum number of feature points to track (Optical Flow mode only)
  - Lower values (20-100): Fewer, higher-quality feature points
  - Higher values (200-500): More feature points, including weaker corners
  - Feature points are displayed as blue circles on the video preview
- `Noise Reduction` (Bool) - Enable morphological filtering to reduce noise (Background Subtraction mode only)
- `Use GPU (CUDA)` (Bool) - Enable GPU acceleration for optical flow (requires CUDA-capable GPU)
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Connect Video Source:** Connect Webcam Loader or Video File Loader's `Source ID` to `Source In`
2. **Choose Detection Mode:**
   - **Optical Flow:** Tracks individual feature points (blue circles) and their motion vectors (green lines)
   - **Background Subtraction:** Detects moving regions by comparing to learned background
3. **Adjust Max Features (Optical Flow):**
   - Increase to track more points for detailed motion analysis
   - Decrease for faster processing and cleaner tracking
   - Feature points appear as blue circles on the video preview
4. **Set Sensitivity:** Adjust threshold for motion trigger output (higher = less sensitive)
5. **Define Zones (UI Overlay):**
   - Hold Ctrl and Left-drag on the video to draw rectangles for the active zone color
   - Right-drag to erase rectangles under the cursor
   - Click the color swatches to choose the active zone (Red/Green/Blue/Yellow)
   - Semi-transparent overlays show current zones
   - Tooltips remind: “Ctrl+Left-drag: Draw zone / Right-drag: Erase zone”
6. **Map Motion to Synthesis:**
   - Connect `Motion X/Y` to panning, filter cutoff, or oscillator frequency
   - Use `Amount` to control effect intensity based on overall motion
   - Connect any Zone Gate to sequencers or envelopes for region-based gating
7. **Chain Video Processing:** Connect `Video Out` to other video modules (Video FX, etc.) for multi-stage processing
8. **Example Patches:**
   - **Gesture Control:** Map hand/body motion to synthesis parameters
   - **Motion-Triggered Sequences:** Use a Zone Gate to advance sequencers when movement enters a region
   - **Interactive Installations:** Create soundscapes that respond to movement
   - **Dance Performance:** Full-body motion drives multiple synthesis parameters

**Visual Feedback:**
- **Blue circles:** Detected feature points (Optical Flow mode)
- **Green lines:** Motion vectors showing direction and magnitude of movement
- **Centroid circle (Background Subtraction):** Center point of detected motion region

**Performance Tips:**
- GPU acceleration significantly improves optical flow performance (if available)
- Lower Max Features values process faster but track fewer points
- Good lighting and contrast improve feature detection accuracy
- Video output updates continuously at ~30 FPS for smooth passthrough

---

### pose_estimator
**Body Keypoint Detection**

Uses OpenPose MPI model to detect 15 body keypoints. Outputs 30 CV pins (X/Y per keypoint) plus 4 zone gates. Zone gates go high if any detected keypoint is inside the corresponding zone color.

#### Inputs
- `Source In` (Video) - Video source ID from webcam or video file loader

**Outputs (dynamic/programmatic):**
- `Head X/Y`, `Neck X/Y`, `R Shoulder X/Y`, `R Elbow X/Y`, `R Wrist X/Y`, `L Shoulder X/Y`, `L Elbow X/Y`, `L Wrist X/Y`, `R Hip X/Y`, `R Knee X/Y`, `R Ankle X/Y`, `L Hip X/Y`, `L Knee X/Y`, `L Ankle X/Y`, `Chest X/Y` (all CV)
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High if any detected keypoint is inside any rectangle of the zone color
- `Video Out` (Video) - Passthrough video output for chaining
- `Cropped Out` (Video) - Cropped region around detected body

#### Parameters
- `Confidence` (0.0-1.0) - Detection confidence threshold (default: 0.1). Lower values detect more keypoints but may include false positives
- `Draw Skeleton` (Bool) - Toggle skeleton overlay on video preview
- `Zoom` (+/-) - Toggle between normal (480px) and zoomed (960px) video preview

#### How to Use
1. **Setup:** Download OpenPose MPI model files (see `guides/POSE_ESTIMATOR_SETUP.md`)
2. **Connect Video Source:** Connect a Webcam Loader or Video File Loader's `Source ID` output to `Source In`
3. **Adjust Confidence:** Lower threshold for more sensitive detection, higher for more reliable detection
4. **Map Keypoints:** Connect individual keypoint X/Y outputs to any CV modulation input
5. **Define Zones (UI Overlay):**
   - Hold Ctrl and Left-drag to draw zone rectangles for the active color; Right-drag to erase
   - Click color swatches to pick the active zone (Red/Green/Blue/Yellow)
   - Zone gates are high if any detected keypoint is inside a zone area
5. **Example Patches:**
   - **Hand-Controlled Oscillator:** Connect `R Wrist X` → VCO Frequency, `R Wrist Y` → VCF Cutoff
   - **Body-Driven Rhythm:** Connect `R Knee Y` → Sequencer Rate, `L Knee Y` → Gate threshold
   - **Dance Performance:** Map multiple keypoints to different parameters for full-body control
6. **Performance Tips:**
   - Good lighting improves detection accuracy
   - Stand 1-3 meters from camera for best results
   - Keep full body in frame for all keypoints to be detected
   - Simple backgrounds work best

**Technical Details:**
- Uses OpenPose MPI (faster) model with 15 keypoints
- Runs at ~15 FPS on CPU (computationally intensive)
- Outputs normalized coordinates (0-1 range)
- Real-time safe: Processing runs on separate thread, lock-free FIFO to audio thread
- Video preview shows skeleton overlay when enabled

**Creative Applications:**
- **Interactive Installations:** Create body-responsive soundscapes
- **Live Performance:** Control synthesis with gestures and movement
- **Accessibility:** Hands-free instrument control for performers with mobility constraints
- **Dance + Music:** Choreography-driven composition
- **Fitness Apps:** Exercise-triggered sound design
- **Game Controllers:** Full-body game audio integration

**Requirements:**
- OpenPose MPI model files (~200 MB download)
- Webcam or video file source
- OpenCV with DNN module (included in build)

---

### hand_tracker
**Hand Keypoint Detection**

Uses OpenPose hand model to detect 21 hand keypoints. Outputs make the hand behave like an instrument independent of screen position: the wrist is absolute, all other keypoints are relative to the wrist. Also includes 4 zone gates.

#### Inputs
- `Source In` (Video) - Video source ID

**Outputs (dynamic/programmatic):**
- `Wrist X/Y` (CV, absolute), all other keypoints are relative to the wrist:
  - `Thumb 1-4 X/Y (Rel)`, `Index 1-4 X/Y (Rel)`, `Middle 1-4 X/Y (Rel)`, `Ring 1-4 X/Y (Rel)`, `Pinky 1-4 X/Y (Rel)`
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High if any detected keypoint is inside any rectangle of the zone color
- `Video Out` (Video) - Passthrough video output for chaining
- `Cropped Out` (Video) - Cropped region around detected hand

#### Parameters
- `Confidence` (0.0-1.0) - Detection confidence threshold (default: 0.1)
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Setup:** Requires OpenPose hand model files in `assets/openpose_models/hand/`
2. **Connect Video Source:** Connect Webcam or Video File Loader's `Source ID` to `Source In`
3. **Adjust Confidence:** Lower values for sensitive detection, higher for reliable detection
4. **Define Zones (UI Overlay):**
   - Hold Ctrl and Left-drag to draw zone rectangles for the active color; Right-drag to erase
   - Click color swatches to pick the active zone (Red/Green/Blue/Yellow)
   - Zone gates are high if any detected keypoint is inside a zone area
5. **Map Gestures:** Connect finger joint positions to synthesis parameters
5. **Example Patches:**
   - **Gesture Control:** Map thumb position to filter cutoff, index position to VCO frequency
   - **Finger Tracking:** Use individual finger tips for multi-parameter control
   - **Hand Size:** Use wrist to fingertip distances for amplitude or volume control
6. **Performance Tips:**
   - Works best with hands clearly visible against contrasting background
   - Keep hands ~30-80cm from camera
   - Good lighting improves detection accuracy

**Technical Details:**
- Uses OpenPose hand detection model
- Detects 21 keypoints per hand
- Runs at ~15 FPS on CPU
- Outputs normalized coordinates (0-1 range)

---

### face_tracker
**Facial Landmark Detection**

Uses OpenPose face model to detect 70 facial landmarks, but the module outputs a simplified, expressive set of 36 CV pins plus 4 zone gates. The face center is absolute; expressive points (nose base, eyes, mouth, eyebrows) are relative to the face center for stable, musically useful control.

#### Inputs
- `Source In` (Video) - Video source ID

**Outputs (dynamic/programmatic):**
- `Face Center X/Y (Abs)` (CV)
- `Nose Base X/Y (Rel)` (CV)
- Right Eye (Rel): `Outer X/Y`, `Top X/Y`, `Inner X/Y`, `Bottom X/Y`
- Left Eye (Rel): `Inner X/Y`, `Top X/Y`, `Outer X/Y`, `Bottom X/Y`
- Mouth (Rel): `Corner R X/Y`, `Top Center X/Y`, `Corner L X/Y`, `Bottom Center X/Y`
- Eyebrows (Rel): `R Outer X/Y`, `R Inner X/Y`, `L Inner X/Y`, `L Outer X/Y`
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High if the face center or any detected keypoint is inside any rectangle of the zone color
- `Video Out` (Video) - Passthrough video output for chaining
- `Cropped Out` (Video) - Cropped region around detected face

#### Parameters
- `Confidence` (0.0-1.0) - Detection confidence threshold (default: 0.1)
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Setup:** Requires OpenPose face model files in `assets/openpose_models/face/`
2. **Connect Video Source:** Connect Webcam or Video File Loader's `Source ID` to `Source In`
3. **Adjust Confidence:** Lower values for sensitive detection, higher for reliable detection
4. **Define Zones (UI Overlay):**
   - Hold Ctrl and Left-drag to draw zone rectangles for the active color; Right-drag to erase
   - Click color swatches to pick the active zone (Red/Green/Blue/Yellow)
   - Zone gates are high if the face center or any detected keypoint is inside a zone area
5. **Map Landmarks:** Connect expressive outputs to synthesis parameters
5. **Example Patches:**
   - **Expression Control:** Map mouth width to effect parameters, eyebrow position to filter resonance
   - **Head Tracking:** Use face center for spatial panning
   - **Lip Sync:** Use mouth landmarks for vocoder or formant filtering
6. **Performance Tips:**
   - Face-front camera position works best
   - Keep face well-lit
   - Maintain 50-150cm distance from camera

**Technical Details:**
- Uses Haar Cascade for face detection, OpenPose DNN for landmark estimation
- Detects 70 landmarks per face
- Runs at ~15 FPS on CPU
- Outputs normalized coordinates (0-1 range)

---

### object_detector
**YOLOv3 Object Detection**

Uses YOLOv3 deep learning model to detect objects from 80 COCO classes (person, car, bottle, etc.) in real-time video. Includes zone drawing UI and 4 zone gates; draws a small red dot at the detected object center on the preview.

#### Inputs
- `Source In` (Video) - Video source ID

#### Outputs
- `X` (CV) - Center X position (0-1)
- `Y` (CV) - Center Y position (0-1)
- `Width` (CV) - Width (0-1)
- `Height` (CV) - Height (0-1)
- `Gate` (Gate) - High when target detected
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High if the detected object center is inside any rectangle of the zone color
- `Video Out` (Video) - Passthrough video output for chaining
- `Cropped Out` (Video) - Cropped region around detected object

#### Parameters
- `Target Class` (Choice) - Object class to detect (person, car, bicycle, etc.)
- `Confidence` (0.0-1.0) - Detection confidence threshold (default: 0.5)
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Setup:** Requires YOLOv3 model files (`yolov3.cfg`, `yolov3.weights`, `coco.names`) in `assets/`
2. **Connect Video Source:** Connect Webcam or Video File Loader's `Source ID` to `Source In`
3. **Select Target Class:** Choose which object type to detect
4. **Adjust Confidence:** Lower for more detections (may include false positives), higher for reliable detection
5. **Define Zones (UI Overlay):**
   - Hold Ctrl and Left-drag to draw zone rectangles for the active color; Right-drag to erase
   - Click color swatches to pick the active zone (Red/Green/Blue/Yellow)
6. **Map Coordinates:** Connect X/Y/Width/Height outputs to synthesis parameters
6. **Example Patches:**
   - **Person Tracking:** Use person bounding box to trigger events when person enters/exits frame
   - **Zone Logic:** Use Zone Gates to trigger when detected object enters regions
   - **Object Size Control:** Use Width × Height to control effect amount
   - **Position-Based Effects:** Map center X to panning, center Y to filter cutoff
7. **Performance Tips:**
   - YOLO is computationally intensive (~10 FPS on CPU)
   - Good lighting and contrast improve detection
   - Larger objects are detected more reliably

**Technical Details:**
- Uses YOLOv3 (You Only Look Once v3) detection model
- 80 COCO object classes supported
- Runs at ~10 FPS on CPU
- Outputs normalized bounding box coordinates
- Falls back to YOLOv3-tiny if standard model not available

---

### chromakey
**Chromakey Video Processor**

Removes selected colors from video frames and converts them to alpha transparency (Green Screen effect). Supports multiple color selection with ColorTracker-style HSV range matching, median sampling, spill suppression, and edge feathering.

#### Inputs
- `Source In` (Video) - Video source ID

#### Outputs
- `RGBA Video` (Video) - Processed video with alpha transparency
- `Mask` (Video) - Black and white binary mask (White = Opaque, Black = Transparent)

#### Parameters
- `Add Color...` (Button) - Click to pick a color from the video preview
- `Spill Support` (0.0-1.0) - Tint reduction for green/blue spill on edges
- `Feather` (0.0-20.0) - Softness of the mask edges
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px)

#### How to Use
1. **Connect Video Source:** Connect Webcam or Video File Loader's `Source ID` to `Source In`
2. **Pick Background Color:** Click "Add Color..." and click on the green/blue screen background in the preview
3. **Refine Mask:** Add more color samples if lighting is uneven
4. **Feather Edges:** Increase Feather amount to soften the transition
5. **Remove Spill:** Increase Spill Suppression if you see green/blue halos on the subject
6. **Use Outputs:**
   - Connect `RGBA Video` to `Video Compositor` to layer over other backgrounds
   - Connect `Mask` to `Video Compositor` or other effects for luma keying

**Technical Details:**
- GPU-accelerated when available (CUDA)
- Supports multiple background colors
- Median sampling for robust color selection
- Real-time processing

---

### color_tracker
**Multi-Color HSV Tracking**

Tracks multiple custom colors in video using HSV color space. Outputs are dynamic: each added color creates three CV outputs, plus 4 zone gates that go high if any tracked color’s centroid is inside the corresponding zone color.

#### Inputs
- `Source In` (Video) - Video source ID

**Outputs (dynamic):**
- For each tracked color:
  - `[Color] X` (CV) - Center X (0-1)
  - `[Color] Y` (CV) - Center Y (0-1)
  - `[Color] Area` (CV) - Covered area (0-1)
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High if any tracked color centroid is inside any rectangle of the zone color
- `Video Out` (Video) - Passthrough video output for chaining

#### Parameters
- `Add Color...` (Button) - Click to pick a color from the video preview
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Connect Video Source:** Connect Webcam or Video File Loader's `Source ID` to `Source In`
2. **Pick Colors:** Click "Add Color..." and click on the video preview to sample a color (Left-click)
3. **Track Multiple:** Add up to 8 different colors to track simultaneously
4. **Remove Colors:** Click "Remove" button next to each tracked color
5. **Define Zones (UI Overlay):**
   - Hovering shows a live color preview square (always on while hovering)
   - Hold Ctrl and Left-drag to draw zone rectangles for the active color; Right-drag to erase
   - Click color swatches to pick the active zone (Red/Green/Blue/Yellow)
5. **Map Coordinates:** Connect individual color outputs to synthesis parameters
6. **Example Patches:**
   - **Two-Object Control:** Track two colored objects for stereo panning or dual oscillator control
   - **Area-Based Effects:** Use Area output to control effect wet/dry mix
   - **Position Automation:** Map color X/Y to sequencer position or filter sweeps
7. **Performance Tips:**
   - Works best with saturated, distinct colors
   - Avoid overlapping colors in similar hues
   - Good lighting maintains consistent HSV values

**Technical Details:**
- Uses HSV color space for robust color detection
- Tracks up to 8 colors simultaneously
- Automatic morphological cleanup for noise reduction
- Runs at ~30 FPS on CPU
- Outputs normalized coordinates and area

---

### contour_detector
**Shape Detection via Background Subtraction**

Detects shapes and their properties using background subtraction and contour analysis. Includes zone drawing UI and 4 zone gates that go high when detected contour centroids enter the corresponding zones.

#### Inputs
- `Source In` (Video) - Video source ID

#### Outputs
- `Area` (CV) - Detected shape area (0-1)
- `Complexity` (CV) - Polygon complexity (0-1)
- `Aspect Ratio` (CV) - Width/height ratio
- `Red/Green/Blue/Yellow Zone Gate` (Gate) - High when the detected contour centroid is inside any rectangle of the zone color
- `Video Out` (Video) - Passthrough video output for chaining

#### Parameters
- `Threshold` (0-255) - Threshold for foreground/background separation (default: 128)
- `Noise Reduction` (Bool) - Enable morphological filtering to reduce noise (default: On)
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Connect Video Source:** Connect Webcam or Video File Loader's `Source ID` to `Source In`
2. **Adjust Threshold:** Set threshold to separate foreground from background
3. **Enable Noise Reduction:** Reduce detection of small, noisy artifacts
4. **Define Zones (UI Overlay):**
   - Hold Ctrl and Left-drag to draw zone rectangles for the active color; Right-drag to erase
   - Click color swatches to pick the active zone (Red/Green/Blue/Yellow)
4. **Map Shape Properties:** Connect Area, Complexity, and Aspect Ratio to synthesis parameters
5. **Example Patches:**
   - **Size-Based Filtering:** Use Area to control low-pass filter cutoff
   - **Shape Recognition:** Use Complexity to detect simple vs complex shapes
   - **Orientation Control:** Use Aspect Ratio to determine if object is horizontal or vertical
6. **Performance Tips:**
   - Requires relatively static background for best results
   - Good contrast between foreground and background
   - Use noise reduction for clean signal

**Technical Details:**
- Uses MOG2 background subtraction
- Polygon approximation for complexity calculation
- Runs at ~25 FPS on CPU
- Outputs normalized shape properties

---

---

### video_fx
**Real-Time Video Effects Processor**

A comprehensive video processing node that applies real-time effects to video streams. Supports chaining multiple effects for complex video transformations. All parameters can be modulated via CV inputs.

#### Inputs
- `Source In` (Video) - Video source ID from webcam, video file, or other video processing nodes

#### Outputs
- `Output ID` (Video) - Processed video source ID for chaining to other video modules

**Parameters (All CV-Modulatable):**

**Color Adjustments:**
- `Brightness` (-100 to +100) - Brightness adjustment
- `Contrast` (0.0-3.0) - Contrast multiplier
- `Saturation` (0.0-3.0) - Color saturation (0=grayscale, 1=normal, >1=enhanced)
- `Hue Shift` (-180 to +180) - Hue rotation in degrees
- `Red/Green/Blue Gain` (0.0-2.0) - Per-channel gain control
- `Temperature` (-1.0 to +1.0) - Color temperature (cold to warm)
- `Sepia` (Bool) - Apply sepia tone effect

**Filters & Effects:**
- `Sharpen` (0.0-2.0) - Sharpening amount
- `Blur` (0-20) - Blur radius
- `Grayscale` (Bool) - Convert to grayscale
- `Invert Colors` (Bool) - Invert color values
- `Flip Horizontal/Vertical` (Bool) - Mirror frames

**Advanced Effects:**
- `Threshold Enable` (Bool) - Enable threshold effect
- `Threshold Level` (0-255) - Threshold cutoff
- `Posterize Levels` (2-32) - Color quantization (lower = fewer colors)
- `Vignette Amount` (0.0-1.0) - Vignette darkening intensity
- `Vignette Size` (0.1-2.0) - Vignette radius
- `Pixelate Block Size` (1-128) - Pixelation block size
- `Edge Detect (Canny)` (Bool) - Enable Canny edge detection
- `Canny Threshold 1/2` (0-255) - Edge detection thresholds
- `Kaleidoscope` (Choice) - None, 4-Way, or 8-Way mirroring

**System:**
- `Use GPU (CUDA)` (Bool) - Enable GPU acceleration (if available)
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

#### How to Use
1. **Connect Video Source:** Connect Webcam, Video File Loader, or another Video FX node to `Source In`
2. **Chain Effects:** Connect `Output ID` to another Video FX or CV processing module
3. **Adjust Parameters:** Use sliders for real-time preview
4. **Modulate with CV:** Connect LFOs, envelopes, or sequencers to CV inputs for animated effects
5. **Example Chains:**
   - **Color Grading Chain:** Video FX (saturation + temperature) → Video FX (contrast + brightness)
   - **Stylization:** Video FX (posterize + vignette) → Video FX (sepia + blur)
   - **Motion Effects:** LFO → Brightness CV, ADSR → Blur CV for dynamic effects
6. **Performance Tips:**
   - GPU acceleration significantly improves performance (enable if available)
   - Chain multiple Video FX nodes for complex effect combinations
   - Use CV modulation for automated video transformations
   - Preview size affects UI performance (use Small for many nodes)

**Technical Details:**
- Passthrough video processing (zero latency)
- Supports GPU acceleration via CUDA (when compiled with CUDA support)
- Real-time processing at video frame rate
- All effects are composable and can be combined
- Dynamic CV modulation inputs for all parameters
- Creates new video source ID for processed output (enables chaining)

---

### crop_video
**Video Cropping with Automatic Tracking**

Crops video frames to a specified region. Supports three modes: manual cropping, automatic face tracking, and automatic object tracking (YOLOv3). Perfect for following detected objects or isolating regions of interest.

#### Inputs
- `Source In` (Video) - Video source ID
- `Center X Mod` (CV) - Center X position modulation (0-1)
- `Center Y Mod` (CV) - Center Y position modulation (0-1)
- `Width Mod` (CV) - Crop width modulation (0-1)
- `Height Mod` (CV) - Crop height modulation (0-1)

#### Outputs
- `Output ID` (Video) - Cropped video source ID for chaining

#### Parameters
- `Tracking Mode` (Choice) - Manual, Track Face, or Track Object
- `Target Class` (Choice) - Object class when tracking objects (person, car, etc.)
- `Confidence` (0.0-1.0) - Detection confidence threshold
- `Padding` (0.0-2.0) - Padding around tracked region (0.1 = 10% padding)
- `Aspect Ratio` (Choice) - Stretch or Preserve (Fit)
- `Use GPU (CUDA)` (Bool) - Enable GPU acceleration for tracking
- `Zoom` (+/-) - Adjust preview size: Small (240px), Normal (480px), Large (960px)

**Manual Crop Controls:**
- `Center X/Y` (0-1) - Crop region center position
- `Width/Height` (0-1) - Crop region size

#### How to Use
1. **Manual Mode:**
   - Connect video source to `Source In`
   - Adjust Center X/Y and Width/Height sliders
   - Use Aspect Ratio "Preserve" to maintain original proportions
   
2. **Track Face Mode:**
   - Select "Track Face" from Tracking Mode
   - Automatically detects and tracks faces using Haar Cascade
   - Adjust Padding to add space around face
   - GPU acceleration recommended for better performance

3. **Track Object Mode:**
   - Select "Track Object" from Tracking Mode
   - Choose Target Class (e.g., "person", "car", "bottle")
   - Requires YOLOv3 model files in `assets/`
   - Automatically tracks and crops to detected object bounding box
   - Adjust Confidence threshold to filter detections

4. **CV Modulation:**
   - Connect Object Detector X/Y/Width/Height outputs to Crop Video modulation inputs
   - Connect Pose Estimator keypoint positions for dynamic cropping
   - Use sequencers or LFOs for automated crop animations

5. **Example Patches:**
   - **Face Isolation:** Track Face mode → Cropped output to Face Tracker for detailed analysis
   - **Person Following:** Object Detector (person) → Crop Video CV inputs → Cropped region to Pose Estimator
   - **Dynamic Cropping:** LFO → Center X Mod, ADSR → Width Mod for animated crops
   - **Multi-Stage Processing:** Crop Video → Video FX (stylize cropped region) → Further processing

6. **Performance Tips:**
   - GPU acceleration improves tracking performance (especially for YOLOv3)
   - Lower confidence thresholds detect more objects but may include false positives
   - Padding helps maintain context around tracked objects
   - Use Preserve aspect ratio to avoid distortion

**Technical Details:**
- Three tracking modes: Manual (slider-based), Face (Haar Cascade), Object (YOLOv3)
- YOLOv3 tracking requires model files (`yolov3.cfg`, `yolov3.weights`, `coco.names`)
- Falls back to YOLOv3-tiny if standard model not available
- GPU acceleration via CUDA (optional, improves performance)
- Real-time processing at video frame rate
- Passthrough video processing with zero latency
- CV modulation allows dynamic crop region control

---

## 11. SYSTEM NODES

System nodes provide special functionality for patch organization.

### meta
**Meta Module Container**

A container for creating custom reusable modules from sub-patches.

#### How to Use
1. Create a patch inside the Meta module
2. Use Inlet/Outlet nodes to define interface
3. Save as reusable module
4. Collapse complex patches into single nodes

---

### inlet
**Meta Module Input**

Defines an input for a Meta module.

---

### outlet
**Meta Module Output**

Defines an output for a Meta module.

---

### comment
**Documentation Node**

A text comment node for documenting patches.

#### How to Use
1. Add comment node
2. Type documentation text
3. Helps explain complex patches
4. No audio/CV functionality

---

### recorder
**Audio Recording to File**

Records incoming audio to WAV, AIFF, or FLAC files.

#### Inputs
- `In L/R` (Audio) - Stereo audio to record

#### Parameters
- File path/name
- Format (WAV, AIFF, FLAC)
- Bit depth (16/24/32)
- Record button

#### How to Use
1. Set file path and format
2. Connect audio source
3. Click Record to start
4. Click Stop to finish and save

---

### vst_host
**VST Plugin Host**

Hosts VST2/VST3 plugins within the modular environment.

#### How to Use
1. Load VST plugin
2. Audio routed through plugin
3. Use external effects and instruments
4. Combine modular with traditional plugins

---

### bpm_monitor
**Rhythm Detection and BPM Reporting**

A hybrid smart system that automatically detects and reports BPM from rhythm-producing modules and audio inputs. This node is always present in patches (like the output node) and cannot be deleted. It dynamically generates output pins for each detected rhythm source.

**Inputs (Dynamic):**
- `In 1-16` (Audio) - Audio inputs for beat detection (up to 16 channels)

**Outputs (Dynamic):**
- For each detected rhythm source:
  - `[Source Name] BPM` (Raw) - Absolute BPM value
  - `[Source Name] CV` (CV) - Normalized BPM (0-1 range for modulation)
  - `[Source Name] Active` (Gate) - High when source is active (for introspected sources)
  - `[Source Name] Confidence` (CV) - Detection confidence (0-1, for detected sources)

#### Parameters
- `Operation Mode` (Choice) - Auto (both methods), Introspection Only, or Detection Only
- `Min BPM` (20-120) - Minimum BPM for normalization (default: 60)
- `Max BPM` (120-300) - Maximum BPM for normalization (default: 240)

#### How to Use
1. **Operation Modes:**
   - **Auto**: Uses both introspection (fast) and beat detection (universal)
   - **Introspection Only**: Only scans modules that report rhythm info (sequencers, animations)
   - **Detection Only**: Only analyzes audio inputs using tap tempo algorithm
   
2. **Introspection (Recommended):**
   - Automatically detects modules that implement `getRhythmInfo()`
   - Works with: Tempo Clock, Sequencers (Step, Stroke, Snapshot), Timeline, Clock Divider, LFOs, Function Generator, Random, Phaser, Chorus, Sample Loader, and other rhythm-producing modules
   - Provides instant, accurate BPM reporting
   - Scans the graph periodically (every 128 audio blocks for efficiency)
   
3. **Beat Detection (Fallback):**
   - Connect any rhythmic audio signal to the inputs
   - Uses tap tempo algorithm with rolling average
   - Works with external audio, VST plugins, or any rhythmic source
   - Outputs confidence level indicating detection stability
   
4. **Using Outputs:**
   - Each detected source generates three outputs:
     - **BPM (Raw)**: Absolute BPM value (e.g., 120.0)
     - **CV**: Normalized 0-1 range for modulation (scaled by Min/Max BPM parameters)
     - **Active/Confidence**: Gate for introspected sources, confidence CV for detected sources
   
5. **Example Patches:**
   - **Sync Effects to Sequencer**: Connect Sequencer → BPM Monitor, use `CV` output to modulate delay time
   - **Beat-Synchronized LFO**: Use `CV` output to sync LFO rate to detected BPM
   - **Multi-Source Tempo**: Monitor multiple sequencers and select the most appropriate one
   - **External Tempo Sync**: Connect audio input from external source, use detected BPM to sync your patch
   
6. **Tips:**
   - Introspection mode is fastest and most accurate for compatible modules
   - Beat detection works with any rhythmic source but may have latency
   - Use Min/Max BPM parameters to set the CV output range for your modulation needs
   - The node automatically updates when new rhythm sources appear or disappear

**Technical Details:**
- Hybrid detection: introspection (via `getRhythmInfo()`) + audio analysis (TapTempo algorithm)
- Graph scanning runs every 128 audio blocks (~2.9ms at 44.1kHz) for efficiency
- Thread-safe access to detected sources (protects dynamic pin queries)
- Dynamic output pins generated based on detected sources
- Always present (logical ID 999, undeletable)
- Supports up to 16 audio detection inputs

**Supported Introspection Sources:**
- **Tempo Clock** - Master tempo source (always synced to transport)
- **Stroke Sequencer** - Gesture-based sequencer (sync or free-run)
- **Snapshot Sequencer** - Clock-driven patch state sequencer
- **Timeline** - Automation recorder/playback (always synced)
- **Clock Divider** - Clock division/multiplication (reports detected BPM)
- **LFO** - Low-frequency oscillator (sync or free-run rate)
- **Function Generator** - Drawable envelope/LFO (sync or free-run rate)
- **Random** - Random value generator (sync or free-run rate)
- **Phaser** - Phaser effect LFO rate
- **Chorus** - Chorus effect LFO rate
- **Sample Loader** - Sample playback (when synced and looping)
- Other modules implementing `getRhythmInfo()`

---


## Glossary

**CV (Control Voltage):** A signal (typically 0-1 or -1 to +1) used to modulate parameters.

**Gate:** A binary signal (high/low, on/off) used for triggering and timing.

**Trigger:** A brief pulse signal, typically used to initiate events.

**Audio:** Full-rate audio signals (~44.1kHz or higher).

**Raw:** Unscaled numerical values for custom ranges.

**Video:** Video source identifier for computer vision processing.

**V/Oct:** Volt-per-octave pitch CV standard (1 semitone = 1/12 V).

**Relative Modulation:** CV modulates around a slider position (musical/proportional).

**Absolute Modulation:** CV directly maps to full parameter range.

**Bipolar:** Signal range from -1 to +1 (centered at 0).

**Unipolar:** Signal range from 0 to 1.

---

## Tips for Using the Dictionary

1. **Search by Function:** Use Ctrl+F to find nodes by keyword (e.g., "distortion", "filter", "envelope")
2. **Follow the Signal Flow:** Start with Sources → Effects → Output
3. **Modulation is Key:** Most parameters can be modulated - experiment!
4. **Save Your Patches:** Use the preset system to save and recall configurations
5. **Start Simple:** Build complexity gradually by adding one module at a time

---

**End of Nodes Dictionary**

*For more information, see the main user manual and individual module guides.*


