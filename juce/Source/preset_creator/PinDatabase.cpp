#include "PinDatabase.h"
#include "ImGuiNodeEditorComponent.h" // For NodeWidth enum

// Module Descriptions - populated on first use
void populateModuleDescriptions()
{
    auto& descriptions = getModuleDescriptions();
    if (!descriptions.empty())
        return; // Only run once

    // Sources
    descriptions["audio_input"] = "Brings hardware audio into the patch.";
    descriptions["vco"] = "A standard Voltage-Controlled Oscillator.";
    descriptions["stk_string"] =
        "Physical modeling string synthesizer (guitar, violin, cello, sitar, banjo)";
    descriptions["stk_wind"] =
        "Physical modeling wind instruments (flute, clarinet, saxophone, brass)";
    descriptions["stk_percussion"] = "Modal synthesis percussion (marimba, cymbal, shakers, etc.)";
    descriptions["stk_plucked"] = "Karplus-Strong plucked string synthesis";
    descriptions["essentia_onset_detector"] =
        "Detects note onsets (attacks) in audio, outputs gate triggers and velocity";
    descriptions["polyvco"] = "A multi-voice oscillator bank for polyphony.";
    descriptions["noise"] = "Generates white, pink, or brown noise.";
    descriptions["sequencer"] = "A classic 16-step CV and Gate sequencer.";
    descriptions["multi_sequencer"] = "Advanced sequencer with parallel per-step outputs.";
    descriptions["midi_player"] = "Plays MIDI files and outputs CV/Gate for each track.";
    descriptions["midi_cv"] =
        "Converts MIDI Note/CC messages to CV signals. 8-voice polyphonic with voice stealing. "
        "Compatible with MidiLogger for recording/export.";
    descriptions["osc_cv"] =
        "Converts OSC (Open Sound Control) messages to CV/Gate signals. Supports address pattern "
        "matching and source filtering.";
    descriptions["cv_osc_sender"] =
        "Converts CV/Audio/Gate signals to OSC messages. Send internal signals over the network "
        "with configurable addresses.";
    descriptions["midi_control_center"] =
        "A powerful MIDI learn interface to map any MIDI CC to CV/Gate outputs.";
    descriptions["midi_faders"] = "1-16 MIDI-learnable faders with customizable output ranges.";
    descriptions["midi_knobs"] = "1-16 MIDI-learnable knobs with customizable output ranges.";
    descriptions["midi_buttons"] = "1-32 MIDI-learnable buttons with Gate/Toggle/Trigger modes.";
    descriptions["midi_jog_wheel"] =
        "A single MIDI-learnable jog wheel control for expressive modulation.";
    descriptions["value"] = "Outputs a constant, adjustable numerical value.";
    descriptions["sample_loader"] = "Loads and plays audio samples with pitch/time control.";
    descriptions["sample_sfx"] = "Plays sample variations from a folder with automatic switching.";
    // TTS Family
    descriptions["tts_performer"] = "Advanced Text-to-Speech engine with word-level sequencing.";
    descriptions["vocal_tract_filter"] = "A formant filter that simulates human vowel sounds.";
    // Effects
    descriptions["vcf"] = "A Voltage-Controlled Filter (LP, HP, BP).";
    descriptions["delay"] = "A stereo delay effect with modulation.";
    descriptions["reverb"] = "A stereo reverb effect.";
    descriptions["chorus"] = "A stereo chorus effect.";
    descriptions["spatial_granulator"] =
        "Visual canvas granulator/chorus with color-coded parameters (Red=Delay, Green=Volume, "
        "Blue=Pitch).";
    descriptions["phaser"] = "A stereo phaser effect.";
    descriptions["compressor"] = "Reduces the dynamic range of a signal.";
    descriptions["limiter"] = "Prevents a signal from exceeding a set level.";
    descriptions["gate"] = "A stereo noise gate to silence signals below a threshold.";
    descriptions["drive"] = "A waveshaping distortion effect.";
    descriptions["bit_crusher"] =
        "A bit depth and sample rate reduction effect for lo-fi textures.";
    descriptions["panvol"] = "A 2D control surface for simultaneous volume and panning adjustment.";
    descriptions["graphic_eq"] = "An 8-band graphic equalizer.";
    descriptions["frequency_graph"] = "A high-resolution, real-time spectrum analyzer.";
    descriptions["waveshaper"] = "A distortion effect with multiple shaping algorithms.";
    descriptions["8bandshaper"] = "A multi-band waveshaper for frequency-specific distortion.";
    descriptions["granulator"] =
        "A granular synthesizer/effect that plays small grains of a sample.";
    descriptions["harmonic_shaper"] = "Shapes the harmonic content of a signal.";
    descriptions["timepitch"] = "Real-time pitch and time manipulation using RubberBand.";
    descriptions["de_crackle"] = "A utility to reduce clicks from discontinuous signals.";
    descriptions["recorder"] = "Records incoming audio to a WAV, AIFF, or FLAC file.";
    descriptions["tempo_clock"] =
        "Global clock generator with BPM control, transport, and clock outputs.";
    descriptions["bpm_monitor"] =
        "Monitors and reports BPM from rhythm-producing modules (sequencers, animations).";
    descriptions["timeline"] =
        "Transport-synchronized automation recorder with sample-accurate timing for CV, Gate, "
        "Trigger, and Raw signals.";
    // Modulators
    descriptions["lfo"] = "A Low-Frequency Oscillator for modulation.";
    descriptions["adsr"] = "An Attack-Decay-Sustain-Release envelope generator.";
    descriptions["random"] = "A random value generator with internal sample & hold.";
    descriptions["s_and_h"] =
        "Professional Sample & Hold module with edge detection, threshold control, slew limiting, "
        "and multiple modes. Features real-time visualization and CV modulation.";
    descriptions["function_generator"] = "A complex, drawable envelope/LFO generator.";
    descriptions["automation_lane"] =
        "Draw automation curves on an infinitely scrolling timeline with fixed center playhead. "
        "Create complex hand-drawn modulation with precise timing control.";
    descriptions["automato"] =
        "Record user gestures on a 2D grid and replay them with transport sync. Features "
        "Record/Edit modes, 7 CV outputs, and time-based sample storage.";
    descriptions["shaping_oscillator"] = "An oscillator with a built-in waveshaper.";
    // Utilities & Logic
    descriptions["vca"] = "A Voltage-Controlled Amplifier to control signal level.";
    descriptions["mixer"] = "A stereo audio mixer with crossfading and panning.";
    descriptions["cv_mixer"] = "A mixer specifically for control voltage signals.";
    descriptions["track_mixer"] = "A multi-channel mixer for polyphonic sources.";
    descriptions["attenuverter"] = "Attenuates (reduces) and/or inverts signals.";
    descriptions["lag_processor"] = "Smooths out abrupt changes in a signal (slew limiter).";
    descriptions["math"] = "Performs mathematical operations on signals.";
    descriptions["map_range"] = "Remaps a signal from one numerical range to another.";
    descriptions["quantizer"] = "Snaps a continuous signal to a musical scale.";
    descriptions["rate"] = "Converts a control signal into a normalized rate value.";
    descriptions["comparator"] = "Outputs a high signal if an input is above a threshold.";
    descriptions["logic"] = "Performs boolean logic (AND, OR, XOR, NOT) on gate signals.";
    descriptions["clock_divider"] = "Divides and multiplies clock signals.";
    descriptions["sequential_switch"] = "A signal router with multiple thresholds.";
    descriptions["reroute"] =
        "A polymorphic passthrough node. Pin color adapts to the input signal.";
    descriptions["comment"] = "A plain text comment node for documentation.";
    descriptions["snapshot_sequencer"] =
        "A sequencer that stores and recalls complete patch states.";
    descriptions["chord_arp"] =
        "Harmony brain node that generates chords and simple arpeggios from CV inputs.";
    // Analysis
    descriptions["scope"] = "Visualizes an audio or CV signal.";
    descriptions["debug"] = "A tool for logging signal value changes.";
    descriptions["input_debug"] =
        "A passthrough version of the Debug node for inspecting signals on a cable.";

    // Physics
    descriptions["physics"] = "A 2D physics simulation that outputs collision and contact data.";
    descriptions["animation"] =
        "Loads and plays 3D animations, outputs joint positions and velocities.";
    descriptions["stroke_sequencer"] =
        "Gesture-based sequencer that records and plays back drawn patterns.";

    // OpenCV (Computer Vision)
    descriptions["webcam_loader"] =
        "Captures video from a webcam and publishes it as a source for vision processing modules.";
    descriptions["video_file_loader"] =
        "Loads and plays a video file, publishes it as a source for vision processing modules.";
    descriptions["video_fx"] =
        "Applies real-time video effects (brightness, contrast, saturation, blur, sharpen, etc.) "
        "to video sources, chainable.";
    descriptions["chromakey"] =
        "Removes selected colors from video and converts them to alpha transparency. Supports "
        "multiple colors, HSV tolerance, spill suppression, and edge feathering.";
    descriptions["video_compositor"] =
        "Composites multiple video layers with blend modes and transforms.";
    descriptions["video_draw_impact"] =
        "Allows drawing colored impact marks on video frames. Drawings persist for a configurable "
        "number of frames, creating visual rhythms that can be tracked by the Color Tracker node.";
    descriptions["movement_detector"] =
        "Analyzes video source for motion via optical flow or background subtraction, outputs "
        "motion data as CV, and 4 zone gates as Gate.";
    descriptions["pose_estimator"] =
        "Uses OpenPose to detect 15 body keypoints, outputs their positions as CV, and 4 zone "
        "gates as Gate.";
    descriptions["hand_tracker"] =
        "Detects 21 hand keypoints. Wrist outputs absolute screen position; all other keypoints "
        "output relative positions to wrist, making the hand an instrument independent of screen "
        "location. Outputs 4 zone gates as Gate.";
    descriptions["face_tracker"] =
        "Detects facial landmarks. Outputs face center (absolute), plus simplified set of "
        "expressive points: nose, eyes, mouth, eyebrows (all relative to face center). 36 CV "
        "outputs + 4 zone gates as Gate + 2 video outputs.";
    descriptions["object_detector"] =
        "Uses YOLOv3 to detect objects, outputs bounding box position/size as CV, and 4 zone gates "
        "as Gate.";
    descriptions["color_tracker"] =
        "Tracks multiple colors in video, outputs their positions and sizes as CV, and 4 zone "
        "gates as Gate.";
    descriptions["contour_detector"] =
        "Detects shapes via background subtraction and outputs area, complexity, aspect ratio, and "
        "4 zone gates as CV.";
    descriptions["crop_video"] =
        "Crops a video stream based on CV signals (X, Y, Width, Height). Perfect for following "
        "detected objects or regions.";
}

void populatePinDatabase()
{
    // Populate both databases
    populateModuleDescriptions();

    auto& db = getModulePinDatabase();
    if (!db.empty())
        return; // Only run once

    // --- Sources ---
    // Audio Input: Up to 16 audio outputs (dynamically shown based on channel count) + 3 CV outputs
    {
        ModulePinInfo audioInputPins(NodeWidth::Small, {}, {}, {});
        // Add all 16 possible audio outputs
        for (int i = 0; i < 16; ++i)
            audioInputPins.audioOuts.emplace_back(
                "Out " + juce::String(i + 1), i, PinDataType::Audio);
        // Add CV outputs (Gate, Trigger, EOP) at channels 16, 17, 18
        audioInputPins.audioOuts.emplace_back("Gate", 16, PinDataType::Gate);
        audioInputPins.audioOuts.emplace_back("Trigger", 17, PinDataType::Gate);
        audioInputPins.audioOuts.emplace_back("EOP", 18, PinDataType::Gate);
        db["audio_input"] = audioInputPins;
    }
    db["vco"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Frequency", 0, PinDataType::CV),
         AudioPin("Waveform", 1, PinDataType::CV),
         AudioPin("Gate", 2, PinDataType::Gate)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["stk_string"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Frequency", 0, PinDataType::CV),
         AudioPin("Pluck/Bow", 1, PinDataType::Gate),
         AudioPin("Velocity", 2, PinDataType::CV),
         AudioPin("Damping", 3, PinDataType::CV),
         AudioPin("Pickup Pos", 4, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["stk_wind"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Freq Mod", 0, PinDataType::CV),
         AudioPin("Gate", 1, PinDataType::Gate),
         AudioPin("Breath", 2, PinDataType::CV),
         AudioPin("Vibrato", 3, PinDataType::CV),
         AudioPin("Vibrato Rate", 4, PinDataType::CV),
         AudioPin("Reed Stiffness", 5, PinDataType::CV),
         AudioPin("Jet Delay", 6, PinDataType::CV),
         AudioPin("Lip Tension", 7, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["stk_percussion"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Freq Mod", 0, PinDataType::CV),
         AudioPin("Strike", 1, PinDataType::Gate),
         AudioPin("Velocity", 2, PinDataType::CV),
         AudioPin("Stick Hardness", 3, PinDataType::CV),
         AudioPin("Strike Position", 4, PinDataType::CV),
         AudioPin("Decay", 5, PinDataType::CV),
         AudioPin("Resonance", 6, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["stk_plucked"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Freq Mod", 0, PinDataType::CV),
         AudioPin("Gate", 1, PinDataType::Gate),
         AudioPin("Damping", 2, PinDataType::CV),
         AudioPin("Velocity", 3, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["essentia_onset_detector"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Audio In", 0, PinDataType::Audio)},
        {AudioPin("Onset", 0, PinDataType::Gate),
         AudioPin("Velocity", 1, PinDataType::CV),
         AudioPin("Confidence", 2, PinDataType::CV)},
        {});
    db["essentia_pitch_tracker"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Audio In", 0, PinDataType::Audio),
         AudioPin("Min Freq Mod", 1, PinDataType::CV),
         AudioPin("Max Freq Mod", 2, PinDataType::CV)},
        {AudioPin("Pitch CV", 0, PinDataType::CV), AudioPin("Confidence", 1, PinDataType::CV)},
        {});
    db["noise"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Level Mod", 0, PinDataType::CV),
         AudioPin("Colour Mod", 1, PinDataType::CV),
         AudioPin("Rate Mod", 2, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio),
         AudioPin("Out R", 1, PinDataType::Audio)}, // Stereo output to match actual implementation
        {});
    db["value"] = ModulePinInfo(
        NodeWidth::Small,
        {},
        {AudioPin("Raw", 0, PinDataType::Raw),
         AudioPin("Normalized", 1, PinDataType::CV),
         AudioPin("Inverted", 2, PinDataType::Raw),
         AudioPin("Integer", 3, PinDataType::Raw),
         AudioPin("CV Out", 4, PinDataType::CV)},
        {});
    db["sample_loader"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("Pitch Mod", 0, PinDataType::CV),
         AudioPin("Speed Mod", 1, PinDataType::CV),
         AudioPin("Gate Mod", 2, PinDataType::CV),
         AudioPin("Trigger Mod", 3, PinDataType::Gate),
         AudioPin("Range Start Mod", 4, PinDataType::CV),
         AudioPin("Range End Mod", 5, PinDataType::CV),
         AudioPin("Randomize Trig", 6, PinDataType::Gate),
         AudioPin("Position Mod", 7, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["sample_sfx"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Pitch Var Mod", 0, PinDataType::CV),
         AudioPin("Gate Mod", 1, PinDataType::CV),
         AudioPin("Trigger", 2, PinDataType::Gate),
         AudioPin("Range Start Mod", 3, PinDataType::CV),
         AudioPin("Range End Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});

    // --- Effects ---
    db["vcf"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Cutoff Mod", 2, PinDataType::CV),
         AudioPin("Resonance Mod", 3, PinDataType::CV),
         AudioPin("Type Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["delay"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Time Mod", 2, PinDataType::CV),
         AudioPin("Feedback Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["reverb"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Size Mod", 2, PinDataType::CV),
         AudioPin("Damp Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["compressor"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Thresh Mod", 2, PinDataType::CV),
         AudioPin("Ratio Mod", 3, PinDataType::CV),
         AudioPin("Attack Mod", 4, PinDataType::CV),
         AudioPin("Release Mod", 5, PinDataType::CV),
         AudioPin("Makeup Mod", 6, PinDataType::CV),
         AudioPin("Mix Mod", 7, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Mix", "mix_mod", PinDataType::CV)});

    // --- Modulators ---
    db["lfo"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Rate Mod", 0, PinDataType::CV),
         AudioPin("Depth Mod", 1, PinDataType::CV),
         AudioPin("Wave Mod", 2, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::CV)},
        {});
    db["adsr"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Gate In", 0, PinDataType::Gate),
         AudioPin("Trigger In", 1, PinDataType::Gate),
         AudioPin("Attack Mod", 2, PinDataType::CV),
         AudioPin("Decay Mod", 3, PinDataType::CV),
         AudioPin("Sustain Mod", 4, PinDataType::CV),
         AudioPin("Release Mod", 5, PinDataType::CV)},
        {AudioPin("Env Out", 0, PinDataType::CV),
         AudioPin("Inv Out", 1, PinDataType::CV),
         AudioPin("EOR Gate", 2, PinDataType::Gate),
         AudioPin("EOC Gate", 3, PinDataType::Gate)},
        {});
    db["random"] = ModulePinInfo(
        NodeWidth::Small,
        {}, // No inputs - self-contained random generator
        {AudioPin("Norm Out", 0, PinDataType::CV),
         AudioPin("Raw Out", 1, PinDataType::Raw),
         AudioPin("CV Out", 2, PinDataType::CV),
         AudioPin("Bool Out", 3, PinDataType::Gate),
         AudioPin("Trig Out", 4, PinDataType::Gate)},
        {});

    // --- Utilities ---
    db["vca"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Gain Mod", 2, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["mixer"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In A L", 0, PinDataType::Audio),
         AudioPin("In A R", 1, PinDataType::Audio),
         AudioPin("In B L", 2, PinDataType::Audio),
         AudioPin("In B R", 3, PinDataType::Audio),
         AudioPin("Gain Mod", 4, PinDataType::CV),
         AudioPin("Pan Mod", 5, PinDataType::CV),
         AudioPin("X-Fade Mod", 6, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["cv_mixer"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In A", 0, PinDataType::CV),
         AudioPin("In B", 1, PinDataType::CV),
         AudioPin("In C", 2, PinDataType::CV),
         AudioPin("In D", 3, PinDataType::CV),
         AudioPin("Crossfade Mod", 4, PinDataType::CV),
         AudioPin("Level A Mod", 5, PinDataType::CV),
         AudioPin("Level C Mod", 6, PinDataType::CV),
         AudioPin("Level D Mod", 7, PinDataType::CV)},
        {AudioPin("Mix Out", 0, PinDataType::CV), AudioPin("Inv Out", 1, PinDataType::CV)},
        {ModPin("Crossfade", "crossfade", PinDataType::CV),
         ModPin("Level A", "levelA", PinDataType::CV),
         ModPin("Level C", "levelC", PinDataType::CV),
         ModPin("Level D", "levelD", PinDataType::CV)});
    db["reroute"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In", 0, PinDataType::Audio)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["scope"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In", 0, PinDataType::Audio)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});
    db["graphic_eq"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Band 1 Mod", 2, PinDataType::CV),
         AudioPin("Band 2 Mod", 3, PinDataType::CV),
         AudioPin("Band 3 Mod", 4, PinDataType::CV),
         AudioPin("Band 4 Mod", 5, PinDataType::CV),
         AudioPin("Band 5 Mod", 6, PinDataType::CV),
         AudioPin("Band 6 Mod", 7, PinDataType::CV),
         AudioPin("Band 7 Mod", 8, PinDataType::CV),
         AudioPin("Band 8 Mod", 9, PinDataType::CV),
         AudioPin("Gate Thresh Mod", 10, PinDataType::CV),
         AudioPin("Trig Thresh Mod", 11, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio),
         AudioPin("Out R", 1, PinDataType::Audio),
         AudioPin("Gate Out", 2, PinDataType::Gate),
         AudioPin("Trig Out", 3, PinDataType::Gate)},
        {});
    db["frequency_graph"] = ModulePinInfo(
        NodeWidth::ExtraWide,
        {AudioPin("In L", 0, PinDataType::Audio), AudioPin("In R", 1, PinDataType::Audio)},
        {// Outputs: Stereo audio pass-through + 8 Gate/Trigger outputs
         AudioPin("Out L", 0, PinDataType::Audio),
         AudioPin("Out R", 1, PinDataType::Audio),
         AudioPin("Sub Gate", 2, PinDataType::Gate),
         AudioPin("Sub Trig", 3, PinDataType::Gate),
         AudioPin("Bass Gate", 4, PinDataType::Gate),
         AudioPin("Bass Trig", 5, PinDataType::Gate),
         AudioPin("Mid Gate", 6, PinDataType::Gate),
         AudioPin("Mid Trig", 7, PinDataType::Gate),
         AudioPin("High Gate", 8, PinDataType::Gate),
         AudioPin("High Trig", 9, PinDataType::Gate)},
        {} // No modulation inputs
    );
    db["chorus"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Rate Mod", 2, PinDataType::CV),
         AudioPin("Depth Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["spatial_granulator"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for 16:9 canvas
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Dry Mix Mod", 2, PinDataType::CV),
         AudioPin("Pen Mix Mod", 3, PinDataType::CV),
         AudioPin("Spray Mix Mod", 4, PinDataType::CV),
         AudioPin("Density Mod", 5, PinDataType::CV),
         AudioPin("Grain Size Mod", 6, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Dry Mix", "dryMix_mod", PinDataType::CV),
         ModPin("Pen Mix", "penMix_mod", PinDataType::CV),
         ModPin("Spray Mix", "sprayMix_mod", PinDataType::CV),
         ModPin("Density", "density_mod", PinDataType::CV),
         ModPin("Grain Size", "grainSize_mod", PinDataType::CV)});
    db["phaser"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Rate Mod", 2, PinDataType::CV),
         AudioPin("Depth Mod", 3, PinDataType::CV),
         AudioPin("Centre Mod", 4, PinDataType::CV),
         AudioPin("Feedback Mod", 5, PinDataType::CV),
         AudioPin("Mix Mod", 6, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["compressor"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Thresh Mod", 2, PinDataType::CV),
         AudioPin("Ratio Mod", 3, PinDataType::CV),
         AudioPin("Attack Mod", 4, PinDataType::CV),
         AudioPin("Release Mod", 5, PinDataType::CV),
         AudioPin("Makeup Mod", 6, PinDataType::CV),
         AudioPin("Mix Mod", 7, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Mix", "mix_mod", PinDataType::CV)});
    db["recorder"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio), AudioPin("In R", 1, PinDataType::Audio)},
        {}, // No outputs
        {});
    db["limiter"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Thresh Mod", 2, PinDataType::CV),
         AudioPin("Release Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Mix", "mix_mod", PinDataType::CV)});
    db["gate"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In L", 0, PinDataType::Audio), AudioPin("In R", 1, PinDataType::Audio)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["drive"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Drive Mod", 2, PinDataType::CV),
         AudioPin("Mix Mod", 3, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Drive", "drive", PinDataType::CV), ModPin("Mix", "mix", PinDataType::CV)});
    db["bit_crusher"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Bit Depth Mod", 2, PinDataType::CV),
         AudioPin("Sample Rate Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV),
         AudioPin("Anti-Alias Mod", 5, PinDataType::Gate),
         AudioPin("Quant Mode Mod", 6, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["panvol"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Pan Mod", 0, PinDataType::CV), AudioPin("Vol Mod", 1, PinDataType::CV)},
        {AudioPin("Pan Out", 0, PinDataType::CV), AudioPin("Vol Out", 1, PinDataType::CV)},
        {});
    db["timepitch"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Speed Mod", 2, PinDataType::CV),
         AudioPin("Pitch Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Mix", "mix_mod", PinDataType::CV)});
    db["waveshaper"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Drive Mod", 2, PinDataType::CV),
         AudioPin("Type Mod", 3, PinDataType::CV),
         AudioPin("Mix Mod", 4, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Drive", "drive", PinDataType::CV),
         ModPin("Type", "type", PinDataType::CV),
         ModPin("Mix", "mix", PinDataType::CV)});
    db["8bandshaper"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Drive 1 Mod", 2, PinDataType::CV),
         AudioPin("Drive 2 Mod", 3, PinDataType::CV),
         AudioPin("Drive 3 Mod", 4, PinDataType::CV),
         AudioPin("Drive 4 Mod", 5, PinDataType::CV),
         AudioPin("Drive 5 Mod", 6, PinDataType::CV),
         AudioPin("Drive 6 Mod", 7, PinDataType::CV),
         AudioPin("Drive 7 Mod", 8, PinDataType::CV),
         AudioPin("Drive 8 Mod", 9, PinDataType::CV),
         AudioPin("Gain Mod", 10, PinDataType::CV),
         AudioPin("Mix Mod", 11, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Drive 1", "drive_1", PinDataType::CV),
         ModPin("Drive 2", "drive_2", PinDataType::CV),
         ModPin("Drive 3", "drive_3", PinDataType::CV),
         ModPin("Drive 4", "drive_4", PinDataType::CV),
         ModPin("Drive 5", "drive_5", PinDataType::CV),
         ModPin("Drive 6", "drive_6", PinDataType::CV),
         ModPin("Drive 7", "drive_7", PinDataType::CV),
         ModPin("Drive 8", "drive_8", PinDataType::CV),
         ModPin("Output Gain", "outputGain", PinDataType::CV),
         ModPin("Mix", "mix", PinDataType::CV)});
    db["old_8bandshaper"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Drive 1 Mod", 2, PinDataType::CV),
         AudioPin("Drive 2 Mod", 3, PinDataType::CV),
         AudioPin("Drive 3 Mod", 4, PinDataType::CV),
         AudioPin("Drive 4 Mod", 5, PinDataType::CV),
         AudioPin("Drive 5 Mod", 6, PinDataType::CV),
         AudioPin("Drive 6 Mod", 7, PinDataType::CV),
         AudioPin("Drive 7 Mod", 8, PinDataType::CV),
         AudioPin("Drive 8 Mod", 9, PinDataType::CV),
         AudioPin("Gain Mod", 10, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["granulator"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Trigger In", 2, PinDataType::Gate),
         AudioPin("Density Mod", 3, PinDataType::CV),
         AudioPin("Size Mod", 4, PinDataType::CV),
         AudioPin("Position Mod", 5, PinDataType::CV),
         AudioPin("Pitch Mod", 6, PinDataType::CV),
         AudioPin("Gate Mod", 7, PinDataType::CV),
         AudioPin("Mix Mod", 8, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Mix", "mix_mod", PinDataType::CV)});
    db["mixer"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In A L", 0, PinDataType::Audio),
         AudioPin("In A R", 1, PinDataType::Audio),
         AudioPin("In B L", 2, PinDataType::Audio),
         AudioPin("In B R", 3, PinDataType::Audio),
         AudioPin("Gain Mod", 4, PinDataType::CV),
         AudioPin("Pan Mod", 5, PinDataType::CV),
         AudioPin("X-Fade Mod", 6, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});
    db["sequencer"] = ModulePinInfo(
        NodeWidth::ExtraWide,
        {AudioPin("Mod In L", 0, PinDataType::Audio),
         AudioPin("Mod In R", 1, PinDataType::Audio),
         AudioPin("Rate Mod", 2, PinDataType::CV),
         AudioPin("Gate Mod", 3, PinDataType::CV),
         AudioPin("Steps Mod", 4, PinDataType::CV),
         AudioPin("Gate Thr Mod", 5, PinDataType::CV),
         // Per-step value mods absolute 6..21 (Step1..Step16)
         AudioPin("Step 1 Mod", 6, PinDataType::CV),
         AudioPin("Step 2 Mod", 7, PinDataType::CV),
         AudioPin("Step 3 Mod", 8, PinDataType::CV),
         AudioPin("Step 4 Mod", 9, PinDataType::CV),
         AudioPin("Step 5 Mod", 10, PinDataType::CV),
         AudioPin("Step 6 Mod", 11, PinDataType::CV),
         AudioPin("Step 7 Mod", 12, PinDataType::CV),
         AudioPin("Step 8 Mod", 13, PinDataType::CV),
         AudioPin("Step 9 Mod", 14, PinDataType::CV),
         AudioPin("Step 10 Mod", 15, PinDataType::CV),
         AudioPin("Step 11 Mod", 16, PinDataType::CV),
         AudioPin("Step 12 Mod", 17, PinDataType::CV),
         AudioPin("Step 13 Mod", 18, PinDataType::CV),
         AudioPin("Step 14 Mod", 19, PinDataType::CV),
         AudioPin("Step 15 Mod", 20, PinDataType::CV),
         AudioPin("Step 16 Mod", 21, PinDataType::CV),
         // Per-step trig mods absolute 22..37 (Step1..Step16) â€” these are Gates
         AudioPin("Step 1 Trig Mod", 22, PinDataType::Gate),
         AudioPin("Step 2 Trig Mod", 23, PinDataType::Gate),
         AudioPin("Step 3 Trig Mod", 24, PinDataType::Gate),
         AudioPin("Step 4 Trig Mod", 25, PinDataType::Gate),
         AudioPin("Step 5 Trig Mod", 26, PinDataType::Gate),
         AudioPin("Step 6 Trig Mod", 27, PinDataType::Gate),
         AudioPin("Step 7 Trig Mod", 28, PinDataType::Gate),
         AudioPin("Step 8 Trig Mod", 29, PinDataType::Gate),
         AudioPin("Step 9 Trig Mod", 30, PinDataType::Gate),
         AudioPin("Step 10 Trig Mod", 31, PinDataType::Gate),
         AudioPin("Step 11 Trig Mod", 32, PinDataType::Gate),
         AudioPin("Step 12 Trig Mod", 33, PinDataType::Gate),
         AudioPin("Step 13 Trig Mod", 34, PinDataType::Gate),
         AudioPin("Step 14 Trig Mod", 35, PinDataType::Gate),
         AudioPin("Step 15 Trig Mod", 36, PinDataType::Gate),
         AudioPin("Step 16 Trig Mod", 37, PinDataType::Gate),
         // Per-step gate level mods absolute 38..53
         AudioPin("Step 1 Gate Mod", 38, PinDataType::CV),
         AudioPin("Step 2 Gate Mod", 39, PinDataType::CV),
         AudioPin("Step 3 Gate Mod", 40, PinDataType::CV),
         AudioPin("Step 4 Gate Mod", 41, PinDataType::CV),
         AudioPin("Step 5 Gate Mod", 42, PinDataType::CV),
         AudioPin("Step 6 Gate Mod", 43, PinDataType::CV),
         AudioPin("Step 7 Gate Mod", 44, PinDataType::CV),
         AudioPin("Step 8 Gate Mod", 45, PinDataType::CV),
         AudioPin("Step 9 Gate Mod", 46, PinDataType::CV),
         AudioPin("Step 10 Gate Mod", 47, PinDataType::CV),
         AudioPin("Step 11 Gate Mod", 48, PinDataType::CV),
         AudioPin("Step 12 Gate Mod", 49, PinDataType::CV),
         AudioPin("Step 13 Gate Mod", 50, PinDataType::CV),
         AudioPin("Step 14 Gate Mod", 51, PinDataType::CV),
         AudioPin("Step 15 Gate Mod", 52, PinDataType::CV),
         AudioPin("Step 16 Gate Mod", 53, PinDataType::CV)},
        {AudioPin("Pitch", 0, PinDataType::CV),
         AudioPin("Gate", 1, PinDataType::Gate),
         AudioPin("Gate Nuanced", 2, PinDataType::CV),
         AudioPin("Velocity", 3, PinDataType::CV),
         AudioPin("Mod", 4, PinDataType::CV),
         AudioPin("Trigger", 5, PinDataType::Gate)},
        {});

    db["value"] = ModulePinInfo(
        NodeWidth::Small,
        {},
        {AudioPin("Raw", 0, PinDataType::Raw),
         AudioPin("Normalized", 1, PinDataType::CV),
         AudioPin("Inverted", 2, PinDataType::Raw),
         AudioPin("Integer", 3, PinDataType::Raw),
         AudioPin("CV Out", 4, PinDataType::CV)},
        {});

    db["random"] = ModulePinInfo(
        NodeWidth::Small,
        {}, // No inputs - self-contained random generator
        {AudioPin("Norm Out", 0, PinDataType::CV),
         AudioPin("Raw Out", 1, PinDataType::Raw),
         AudioPin("CV Out", 2, PinDataType::CV),
         AudioPin("Bool Out", 3, PinDataType::Gate),
         AudioPin("Trig Out", 4, PinDataType::Gate)},
        {} // No modulation inputs
    );

    db["tts_performer"] = ModulePinInfo(
        NodeWidth::Big,
        {// Inputs (absolute channels based on bus structure)
         AudioPin("Rate Mod", 0, PinDataType::CV),
         AudioPin("Gate Mod", 1, PinDataType::CV),
         AudioPin("Trigger", 2, PinDataType::Gate),
         AudioPin("Reset", 3, PinDataType::Gate),
         AudioPin("Randomize Trig", 4, PinDataType::Gate),
         AudioPin("Trim Start Mod", 5, PinDataType::CV),
         AudioPin("Trim End Mod", 6, PinDataType::CV),
         AudioPin("Speed Mod", 7, PinDataType::CV),
         AudioPin("Pitch Mod", 8, PinDataType::CV),
         // Word Triggers (Channels 9-24)
         AudioPin("Word 1 Trig", 9, PinDataType::Gate),
         AudioPin("Word 2 Trig", 10, PinDataType::Gate),
         AudioPin("Word 3 Trig", 11, PinDataType::Gate),
         AudioPin("Word 4 Trig", 12, PinDataType::Gate),
         AudioPin("Word 5 Trig", 13, PinDataType::Gate),
         AudioPin("Word 6 Trig", 14, PinDataType::Gate),
         AudioPin("Word 7 Trig", 15, PinDataType::Gate),
         AudioPin("Word 8 Trig", 16, PinDataType::Gate),
         AudioPin("Word 9 Trig", 17, PinDataType::Gate),
         AudioPin("Word 10 Trig", 18, PinDataType::Gate),
         AudioPin("Word 11 Trig", 19, PinDataType::Gate),
         AudioPin("Word 12 Trig", 20, PinDataType::Gate),
         AudioPin("Word 13 Trig", 21, PinDataType::Gate),
         AudioPin("Word 14 Trig", 22, PinDataType::Gate),
         AudioPin("Word 15 Trig", 23, PinDataType::Gate),
         AudioPin("Word 16 Trig", 24, PinDataType::Gate)},
        {// Outputs
         AudioPin("Audio", 0, PinDataType::Audio),
         AudioPin("Word Gate", 1, PinDataType::Gate),
         AudioPin("EOP Gate", 2, PinDataType::Gate),
         // Per-Word Gates (Channels 3-18)
         AudioPin("Word 1 Gate", 3, PinDataType::Gate),
         AudioPin("Word 2 Gate", 4, PinDataType::Gate),
         AudioPin("Word 3 Gate", 5, PinDataType::Gate),
         AudioPin("Word 4 Gate", 6, PinDataType::Gate),
         AudioPin("Word 5 Gate", 7, PinDataType::Gate),
         AudioPin("Word 6 Gate", 8, PinDataType::Gate),
         AudioPin("Word 7 Gate", 9, PinDataType::Gate),
         AudioPin("Word 8 Gate", 10, PinDataType::Gate),
         AudioPin("Word 9 Gate", 11, PinDataType::Gate),
         AudioPin("Word 10 Gate", 12, PinDataType::Gate),
         AudioPin("Word 11 Gate", 13, PinDataType::Gate),
         AudioPin("Word 12 Gate", 14, PinDataType::Gate),
         AudioPin("Word 13 Gate", 15, PinDataType::Gate),
         AudioPin("Word 14 Gate", 16, PinDataType::Gate),
         AudioPin("Word 15 Gate", 17, PinDataType::Gate),
         AudioPin("Word 16 Gate", 18, PinDataType::Gate),
         // Per-Word Triggers (Channels 19-34)
         AudioPin("Word 1 Trig", 19, PinDataType::Gate),
         AudioPin("Word 2 Trig", 20, PinDataType::Gate),
         AudioPin("Word 3 Trig", 21, PinDataType::Gate),
         AudioPin("Word 4 Trig", 22, PinDataType::Gate),
         AudioPin("Word 5 Trig", 23, PinDataType::Gate),
         AudioPin("Word 6 Trig", 24, PinDataType::Gate),
         AudioPin("Word 7 Trig", 25, PinDataType::Gate),
         AudioPin("Word 8 Trig", 26, PinDataType::Gate),
         AudioPin("Word 9 Trig", 27, PinDataType::Gate),
         AudioPin("Word 10 Trig", 28, PinDataType::Gate),
         AudioPin("Word 11 Trig", 29, PinDataType::Gate),
         AudioPin("Word 12 Trig", 30, PinDataType::Gate),
         AudioPin("Word 13 Trig", 31, PinDataType::Gate),
         AudioPin("Word 14 Trig", 32, PinDataType::Gate),
         AudioPin("Word 15 Trig", 33, PinDataType::Gate),
         AudioPin("Word 16 Trig", 34, PinDataType::Gate)},
        {// Modulation Pins (for UI parameter disabling)
         ModPin("Rate", "rate_mod", PinDataType::CV),
         ModPin("Gate", "gate_mod", PinDataType::CV),
         ModPin("Trim Start", "trimStart_mod", PinDataType::CV),
         ModPin("Trim End", "trimEnd_mod", PinDataType::CV),
         ModPin("Speed", "speed_mod", PinDataType::CV),
         ModPin("Pitch", "pitch_mod", PinDataType::CV)});
    db["vocal_tract_filter"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Audio In L", 0, PinDataType::Audio),
         AudioPin("Audio In R", 1, PinDataType::Audio)},
        {AudioPin("Audio Out L", 0, PinDataType::Audio),
         AudioPin("Audio Out R", 1, PinDataType::Audio)},
        {ModPin("Vowel", "vowelShape", PinDataType::CV),
         ModPin("Formant", "formantShift", PinDataType::CV),
         ModPin("Instability", "instability", PinDataType::CV),
         ModPin("Gain", "formantGain", PinDataType::CV)});
    db["shaping_oscillator"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Freq Mod", 2, PinDataType::CV),
         AudioPin("Wave Mod", 3, PinDataType::CV),
         AudioPin("Drive Mod", 4, PinDataType::CV),
         AudioPin("Dry/Wet Mod", 5, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Frequency", "frequency_mod", PinDataType::CV),
         ModPin("Waveform", "waveform_mod", PinDataType::CV),
         ModPin("Drive", "drive_mod", PinDataType::CV),
         ModPin("Dry/Wet", "dryWet_mod", PinDataType::CV)});
    db["harmonic_shaper"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Freq Mod", 2, PinDataType::CV),
         AudioPin("Drive Mod", 3, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {ModPin("Master Frequency", "masterFrequency_mod", PinDataType::CV),
         ModPin("Master Drive", "masterDrive_mod", PinDataType::CV)});
    db["function_generator"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("Gate In", 0, PinDataType::Gate),
         AudioPin("Trigger In", 1, PinDataType::Gate),
         AudioPin("Sync In", 2, PinDataType::Gate),
         AudioPin("Rate Mod", 3, PinDataType::CV),
         AudioPin("Slew Mod", 4, PinDataType::CV),
         AudioPin("Gate Thresh Mod", 5, PinDataType::CV),
         AudioPin("Trig Thresh Mod", 6, PinDataType::CV),
         AudioPin("Pitch Base Mod", 7, PinDataType::CV),
         AudioPin("Value Mult Mod", 8, PinDataType::CV),
         AudioPin("Curve Select Mod", 9, PinDataType::CV)},
        {AudioPin("Value", 0, PinDataType::CV),
         AudioPin("Inverted", 1, PinDataType::CV),
         AudioPin("Bipolar", 2, PinDataType::CV),
         AudioPin("Pitch", 3, PinDataType::CV),
         AudioPin("Gate", 4, PinDataType::Gate),
         AudioPin("Trigger", 5, PinDataType::Gate),
         AudioPin("End of Cycle", 6, PinDataType::Gate),
         // New dedicated outputs
         AudioPin("Blue Value", 7, PinDataType::CV),
         AudioPin("Blue Pitch", 8, PinDataType::CV),
         AudioPin("Red Value", 9, PinDataType::CV),
         AudioPin("Red Pitch", 10, PinDataType::CV),
         AudioPin("Green Value", 11, PinDataType::CV),
         AudioPin("Green Pitch", 12, PinDataType::CV)},
        {ModPin("Rate", "rate_mod", PinDataType::CV),
         ModPin("Slew", "slew_mod", PinDataType::CV),
         ModPin("Gate Thresh", "gateThresh_mod", PinDataType::CV),
         ModPin("Trig Thresh", "trigThresh_mod", PinDataType::CV),
         ModPin("Pitch Base", "pitchBase_mod", PinDataType::CV),
         ModPin("Value Mult", "valueMult_mod", PinDataType::CV),
         ModPin("Curve Select", "curveSelect_mod", PinDataType::CV)});

    db["automation_lane"] = ModulePinInfo(
        NodeWidth::Big,
        {}, // No inputs
        {AudioPin("Value", 0, PinDataType::CV),
         AudioPin("Inverted", 1, PinDataType::CV),
         AudioPin("Bipolar", 2, PinDataType::CV),
         AudioPin("Pitch", 3, PinDataType::CV),
         AudioPin("Trigger", 4, PinDataType::Gate)},
        {});

    db["automato"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("X Mod", 0, PinDataType::CV), AudioPin("Y Mod", 1, PinDataType::CV)},
        {AudioPin("X", 0, PinDataType::CV),
         AudioPin("Y", 1, PinDataType::CV),
         AudioPin("Combined", 2, PinDataType::CV),
         AudioPin("Value", 3, PinDataType::CV),
         AudioPin("Inverted", 4, PinDataType::CV),
         AudioPin("Bipolar", 5, PinDataType::CV),
         AudioPin("Pitch", 6, PinDataType::CV)},
        {});

    ModulePinInfo multiSequencerPins(
        NodeWidth::ExtraWide,
        {// Inputs: Mod In L, Mod In R, Rate Mod, Gate Mod, Steps Mod, Gate Thr Mod, plus per-step
         // mods and triggers
         AudioPin("Mod In L", 0, PinDataType::Audio),
         AudioPin("Mod In R", 1, PinDataType::Audio),
         AudioPin("Rate Mod", 2, PinDataType::CV),
         AudioPin("Gate Mod", 3, PinDataType::CV),
         AudioPin("Steps Mod", 4, PinDataType::CV),
         AudioPin("Gate Thr Mod", 5, PinDataType::CV),
         // Per-step mods (channels 6-21)
         AudioPin("Step 1 Mod", 6, PinDataType::CV),
         AudioPin("Step 2 Mod", 7, PinDataType::CV),
         AudioPin("Step 3 Mod", 8, PinDataType::CV),
         AudioPin("Step 4 Mod", 9, PinDataType::CV),
         AudioPin("Step 5 Mod", 10, PinDataType::CV),
         AudioPin("Step 6 Mod", 11, PinDataType::CV),
         AudioPin("Step 7 Mod", 12, PinDataType::CV),
         AudioPin("Step 8 Mod", 13, PinDataType::CV),
         AudioPin("Step 9 Mod", 14, PinDataType::CV),
         AudioPin("Step 10 Mod", 15, PinDataType::CV),
         AudioPin("Step 11 Mod", 16, PinDataType::CV),
         AudioPin("Step 12 Mod", 17, PinDataType::CV),
         AudioPin("Step 13 Mod", 18, PinDataType::CV),
         AudioPin("Step 14 Mod", 19, PinDataType::CV),
         AudioPin("Step 15 Mod", 20, PinDataType::CV),
         AudioPin("Step 16 Mod", 21, PinDataType::CV),
         // Per-step trigger mods (channels 22-37)
         AudioPin("Step 1 Trig Mod", 22, PinDataType::Gate),
         AudioPin("Step 2 Trig Mod", 23, PinDataType::Gate),
         AudioPin("Step 3 Trig Mod", 24, PinDataType::Gate),
         AudioPin("Step 4 Trig Mod", 25, PinDataType::Gate),
         AudioPin("Step 5 Trig Mod", 26, PinDataType::Gate),
         AudioPin("Step 6 Trig Mod", 27, PinDataType::Gate),
         AudioPin("Step 7 Trig Mod", 28, PinDataType::Gate),
         AudioPin("Step 8 Trig Mod", 29, PinDataType::Gate),
         AudioPin("Step 9 Trig Mod", 30, PinDataType::Gate),
         AudioPin("Step 10 Trig Mod", 31, PinDataType::Gate),
         AudioPin("Step 11 Trig Mod", 32, PinDataType::Gate),
         AudioPin("Step 12 Trig Mod", 33, PinDataType::Gate),
         AudioPin("Step 13 Trig Mod", 34, PinDataType::Gate),
         AudioPin("Step 14 Trig Mod", 35, PinDataType::Gate),
         AudioPin("Step 15 Trig Mod", 36, PinDataType::Gate),
         AudioPin("Step 16 Trig Mod", 37, PinDataType::Gate)},
        {// Outputs: Live outputs (0-6) + Parallel step outputs (7+)
         // Live Outputs
         AudioPin("Pitch", 0, PinDataType::CV),
         AudioPin("Gate", 1, PinDataType::Gate),
         AudioPin("Gate Nuanced", 2, PinDataType::CV),
         AudioPin("Velocity", 3, PinDataType::CV),
         AudioPin("Mod", 4, PinDataType::CV),
         AudioPin("Trigger", 5, PinDataType::Gate),
         AudioPin("Num Steps", 6, PinDataType::Raw),
         // Parallel Step Outputs (Corrected Names and Channels, shifted by +1 after Num Steps)
         AudioPin("Pitch 1", 7, PinDataType::CV),
         AudioPin("Gate 1", 8, PinDataType::Gate),
         AudioPin("Trig 1", 9, PinDataType::Gate),
         AudioPin("Pitch 2", 10, PinDataType::CV),
         AudioPin("Gate 2", 11, PinDataType::Gate),
         AudioPin("Trig 2", 12, PinDataType::Gate),
         AudioPin("Pitch 3", 13, PinDataType::CV),
         AudioPin("Gate 3", 14, PinDataType::Gate),
         AudioPin("Trig 3", 15, PinDataType::Gate),
         AudioPin("Pitch 4", 16, PinDataType::CV),
         AudioPin("Gate 4", 17, PinDataType::Gate),
         AudioPin("Trig 4", 18, PinDataType::Gate),
         AudioPin("Pitch 5", 19, PinDataType::CV),
         AudioPin("Gate 5", 20, PinDataType::Gate),
         AudioPin("Trig 5", 21, PinDataType::Gate),
         AudioPin("Pitch 6", 22, PinDataType::CV),
         AudioPin("Gate 6", 23, PinDataType::Gate),
         AudioPin("Trig 6", 24, PinDataType::Gate),
         AudioPin("Pitch 7", 25, PinDataType::CV),
         AudioPin("Gate 7", 26, PinDataType::Gate),
         AudioPin("Trig 7", 27, PinDataType::Gate),
         AudioPin("Pitch 8", 28, PinDataType::CV),
         AudioPin("Gate 8", 29, PinDataType::Gate),
         AudioPin("Trig 8", 30, PinDataType::Gate),
         AudioPin("Pitch 9", 31, PinDataType::CV),
         AudioPin("Gate 9", 32, PinDataType::Gate),
         AudioPin("Trig 9", 33, PinDataType::Gate),
         AudioPin("Pitch 10", 34, PinDataType::CV),
         AudioPin("Gate 10", 35, PinDataType::Gate),
         AudioPin("Trig 10", 36, PinDataType::Gate),
         AudioPin("Pitch 11", 37, PinDataType::CV),
         AudioPin("Gate 11", 38, PinDataType::Gate),
         AudioPin("Trig 11", 39, PinDataType::Gate),
         AudioPin("Pitch 12", 40, PinDataType::CV),
         AudioPin("Gate 12", 41, PinDataType::Gate),
         AudioPin("Trig 12", 42, PinDataType::Gate),
         AudioPin("Pitch 13", 43, PinDataType::CV),
         AudioPin("Gate 13", 44, PinDataType::Gate),
         AudioPin("Trig 13", 45, PinDataType::Gate),
         AudioPin("Pitch 14", 46, PinDataType::CV),
         AudioPin("Gate 14", 47, PinDataType::Gate),
         AudioPin("Trig 14", 48, PinDataType::Gate),
         AudioPin("Pitch 15", 49, PinDataType::CV),
         AudioPin("Gate 15", 50, PinDataType::Gate),
         AudioPin("Trig 15", 51, PinDataType::Gate),
         AudioPin("Pitch 16", 52, PinDataType::CV),
         AudioPin("Gate 16", 53, PinDataType::Gate),
         AudioPin("Trig 16", 54, PinDataType::Gate)},
        {});
    db["multi_sequencer"] = multiSequencerPins;
    db["comparator"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In", 0, PinDataType::Audio)},
        {AudioPin("Out", 0, PinDataType::Gate)},
        {});

    db["sample_loader"] = ModulePinInfo(
        NodeWidth::Big,
        {AudioPin("Pitch Mod", 0, PinDataType::CV),
         AudioPin("Speed Mod", 1, PinDataType::CV),
         AudioPin("Gate Mod", 2, PinDataType::CV),
         AudioPin("Trigger Mod", 3, PinDataType::Gate),
         AudioPin("Range Start Mod", 4, PinDataType::CV),
         AudioPin("Range End Mod", 5, PinDataType::CV),
         AudioPin("Randomize Trig", 6, PinDataType::Gate),
         AudioPin("Position Mod", 7, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});

    // Track Mixer - first 8 tracks UI definition (mono per track + gain/pan CV) and a Tracks Mod
    // pin
    db["track_mixer"] = ModulePinInfo(
        NodeWidth::Big,
        {// Mono audio inputs for first 8 tracks (absolute channels 0..7)
         AudioPin("In 1", 0, PinDataType::Audio),
         AudioPin("In 2", 1, PinDataType::Audio),
         AudioPin("In 3", 2, PinDataType::Audio),
         AudioPin("In 4", 3, PinDataType::Audio),
         AudioPin("In 5", 4, PinDataType::Audio),
         AudioPin("In 6", 5, PinDataType::Audio),
         AudioPin("In 7", 6, PinDataType::Audio),
         AudioPin("In 8", 7, PinDataType::Audio),

         // Num Tracks modulation CV at absolute channel 64 (start of Mod bus)
         AudioPin("Num Tracks Mod", 64, PinDataType::Raw),

         // Per-track CV inputs on Mod bus: Gain at 65,67,... Pan at 66,68,...
         AudioPin("Gain 1 Mod", 65, PinDataType::CV),
         AudioPin("Pan 1 Mod", 66, PinDataType::CV),
         AudioPin("Gain 2 Mod", 67, PinDataType::CV),
         AudioPin("Pan 2 Mod", 68, PinDataType::CV),
         AudioPin("Gain 3 Mod", 69, PinDataType::CV),
         AudioPin("Pan 3 Mod", 70, PinDataType::CV),
         AudioPin("Gain 4 Mod", 71, PinDataType::CV),
         AudioPin("Pan 4 Mod", 72, PinDataType::CV),
         AudioPin("Gain 5 Mod", 73, PinDataType::CV),
         AudioPin("Pan 5 Mod", 74, PinDataType::CV),
         AudioPin("Gain 6 Mod", 75, PinDataType::CV),
         AudioPin("Pan 6 Mod", 76, PinDataType::CV),
         AudioPin("Gain 7 Mod", 77, PinDataType::CV),
         AudioPin("Pan 7 Mod", 78, PinDataType::CV),
         AudioPin("Gain 8 Mod", 79, PinDataType::CV),
         AudioPin("Pan 8 Mod", 80, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});

    // Add PolyVCO module - Build the pin lists directly in initializer list
    db["polyvco"] = ModulePinInfo(
        NodeWidth::Big,
        {// Num Voices modulation input
         AudioPin("Num Voices Mod", 0, PinDataType::Raw),

         // Frequency modulation inputs (channels 1-32)
         AudioPin("Freq 1 Mod", 1, PinDataType::CV),
         AudioPin("Freq 2 Mod", 2, PinDataType::CV),
         AudioPin("Freq 3 Mod", 3, PinDataType::CV),
         AudioPin("Freq 4 Mod", 4, PinDataType::CV),
         AudioPin("Freq 5 Mod", 5, PinDataType::CV),
         AudioPin("Freq 6 Mod", 6, PinDataType::CV),
         AudioPin("Freq 7 Mod", 7, PinDataType::CV),
         AudioPin("Freq 8 Mod", 8, PinDataType::CV),
         AudioPin("Freq 9 Mod", 9, PinDataType::CV),
         AudioPin("Freq 10 Mod", 10, PinDataType::CV),
         AudioPin("Freq 11 Mod", 11, PinDataType::CV),
         AudioPin("Freq 12 Mod", 12, PinDataType::CV),
         AudioPin("Freq 13 Mod", 13, PinDataType::CV),
         AudioPin("Freq 14 Mod", 14, PinDataType::CV),
         AudioPin("Freq 15 Mod", 15, PinDataType::CV),
         AudioPin("Freq 16 Mod", 16, PinDataType::CV),
         AudioPin("Freq 17 Mod", 17, PinDataType::CV),
         AudioPin("Freq 18 Mod", 18, PinDataType::CV),
         AudioPin("Freq 19 Mod", 19, PinDataType::CV),
         AudioPin("Freq 20 Mod", 20, PinDataType::CV),
         AudioPin("Freq 21 Mod", 21, PinDataType::CV),
         AudioPin("Freq 22 Mod", 22, PinDataType::CV),
         AudioPin("Freq 23 Mod", 23, PinDataType::CV),
         AudioPin("Freq 24 Mod", 24, PinDataType::CV),
         AudioPin("Freq 25 Mod", 25, PinDataType::CV),
         AudioPin("Freq 26 Mod", 26, PinDataType::CV),
         AudioPin("Freq 27 Mod", 27, PinDataType::CV),
         AudioPin("Freq 28 Mod", 28, PinDataType::CV),
         AudioPin("Freq 29 Mod", 29, PinDataType::CV),
         AudioPin("Freq 30 Mod", 30, PinDataType::CV),
         AudioPin("Freq 31 Mod", 31, PinDataType::CV),
         AudioPin("Freq 32 Mod", 32, PinDataType::CV),

         // Waveform modulation inputs (channels 33-64)
         AudioPin("Wave 1 Mod", 33, PinDataType::CV),
         AudioPin("Wave 2 Mod", 34, PinDataType::CV),
         AudioPin("Wave 3 Mod", 35, PinDataType::CV),
         AudioPin("Wave 4 Mod", 36, PinDataType::CV),
         AudioPin("Wave 5 Mod", 37, PinDataType::CV),
         AudioPin("Wave 6 Mod", 38, PinDataType::CV),
         AudioPin("Wave 7 Mod", 39, PinDataType::CV),
         AudioPin("Wave 8 Mod", 40, PinDataType::CV),
         AudioPin("Wave 9 Mod", 41, PinDataType::CV),
         AudioPin("Wave 10 Mod", 42, PinDataType::CV),
         AudioPin("Wave 11 Mod", 43, PinDataType::CV),
         AudioPin("Wave 12 Mod", 44, PinDataType::CV),
         AudioPin("Wave 13 Mod", 45, PinDataType::CV),
         AudioPin("Wave 14 Mod", 46, PinDataType::CV),
         AudioPin("Wave 15 Mod", 47, PinDataType::CV),
         AudioPin("Wave 16 Mod", 48, PinDataType::CV),
         AudioPin("Wave 17 Mod", 49, PinDataType::CV),
         AudioPin("Wave 18 Mod", 50, PinDataType::CV),
         AudioPin("Wave 19 Mod", 51, PinDataType::CV),
         AudioPin("Wave 20 Mod", 52, PinDataType::CV),
         AudioPin("Wave 21 Mod", 53, PinDataType::CV),
         AudioPin("Wave 22 Mod", 54, PinDataType::CV),
         AudioPin("Wave 23 Mod", 55, PinDataType::CV),
         AudioPin("Wave 24 Mod", 56, PinDataType::CV),
         AudioPin("Wave 25 Mod", 57, PinDataType::CV),
         AudioPin("Wave 26 Mod", 58, PinDataType::CV),
         AudioPin("Wave 27 Mod", 59, PinDataType::CV),
         AudioPin("Wave 28 Mod", 60, PinDataType::CV),
         AudioPin("Wave 29 Mod", 61, PinDataType::CV),
         AudioPin("Wave 30 Mod", 62, PinDataType::CV),
         AudioPin("Wave 31 Mod", 63, PinDataType::CV),
         AudioPin("Wave 32 Mod", 64, PinDataType::CV),

         // Gate modulation inputs (channels 65-96)
         AudioPin("Gate 1 Mod", 65, PinDataType::Gate),
         AudioPin("Gate 2 Mod", 66, PinDataType::Gate),
         AudioPin("Gate 3 Mod", 67, PinDataType::Gate),
         AudioPin("Gate 4 Mod", 68, PinDataType::Gate),
         AudioPin("Gate 5 Mod", 69, PinDataType::Gate),
         AudioPin("Gate 6 Mod", 70, PinDataType::Gate),
         AudioPin("Gate 7 Mod", 71, PinDataType::Gate),
         AudioPin("Gate 8 Mod", 72, PinDataType::Gate),
         AudioPin("Gate 9 Mod", 73, PinDataType::Gate),
         AudioPin("Gate 10 Mod", 74, PinDataType::Gate),
         AudioPin("Gate 11 Mod", 75, PinDataType::Gate),
         AudioPin("Gate 12 Mod", 76, PinDataType::Gate),
         AudioPin("Gate 13 Mod", 77, PinDataType::Gate),
         AudioPin("Gate 14 Mod", 78, PinDataType::Gate),
         AudioPin("Gate 15 Mod", 79, PinDataType::Gate),
         AudioPin("Gate 16 Mod", 80, PinDataType::Gate),
         AudioPin("Gate 17 Mod", 81, PinDataType::Gate),
         AudioPin("Gate 18 Mod", 82, PinDataType::Gate),
         AudioPin("Gate 19 Mod", 83, PinDataType::Gate),
         AudioPin("Gate 20 Mod", 84, PinDataType::Gate),
         AudioPin("Gate 21 Mod", 85, PinDataType::Gate),
         AudioPin("Gate 22 Mod", 86, PinDataType::Gate),
         AudioPin("Gate 23 Mod", 87, PinDataType::Gate),
         AudioPin("Gate 24 Mod", 88, PinDataType::Gate),
         AudioPin("Gate 25 Mod", 89, PinDataType::Gate),
         AudioPin("Gate 26 Mod", 90, PinDataType::Gate),
         AudioPin("Gate 27 Mod", 91, PinDataType::Gate),
         AudioPin("Gate 28 Mod", 92, PinDataType::Gate),
         AudioPin("Gate 29 Mod", 93, PinDataType::Gate),
         AudioPin("Gate 30 Mod", 94, PinDataType::Gate),
         AudioPin("Gate 31 Mod", 95, PinDataType::Gate),
         AudioPin("Gate 32 Mod", 96, PinDataType::Gate)},
        {// Audio outputs (channels 0-31)
         AudioPin("Out 1", 0, PinDataType::Audio),   AudioPin("Out 2", 1, PinDataType::Audio),
         AudioPin("Out 3", 2, PinDataType::Audio),   AudioPin("Out 4", 3, PinDataType::Audio),
         AudioPin("Out 5", 4, PinDataType::Audio),   AudioPin("Out 6", 5, PinDataType::Audio),
         AudioPin("Out 7", 6, PinDataType::Audio),   AudioPin("Out 8", 7, PinDataType::Audio),
         AudioPin("Out 9", 8, PinDataType::Audio),   AudioPin("Out 10", 9, PinDataType::Audio),
         AudioPin("Out 11", 10, PinDataType::Audio), AudioPin("Out 12", 11, PinDataType::Audio),
         AudioPin("Out 13", 12, PinDataType::Audio), AudioPin("Out 14", 13, PinDataType::Audio),
         AudioPin("Out 15", 14, PinDataType::Audio), AudioPin("Out 16", 15, PinDataType::Audio),
         AudioPin("Out 17", 16, PinDataType::Audio), AudioPin("Out 18", 17, PinDataType::Audio),
         AudioPin("Out 19", 18, PinDataType::Audio), AudioPin("Out 20", 19, PinDataType::Audio),
         AudioPin("Out 21", 20, PinDataType::Audio), AudioPin("Out 22", 21, PinDataType::Audio),
         AudioPin("Out 23", 22, PinDataType::Audio), AudioPin("Out 24", 23, PinDataType::Audio),
         AudioPin("Out 25", 24, PinDataType::Audio), AudioPin("Out 26", 25, PinDataType::Audio),
         AudioPin("Out 27", 26, PinDataType::Audio), AudioPin("Out 28", 27, PinDataType::Audio),
         AudioPin("Out 29", 28, PinDataType::Audio), AudioPin("Out 30", 29, PinDataType::Audio),
         AudioPin("Out 31", 30, PinDataType::Audio), AudioPin("Out 32", 31, PinDataType::Audio)},
        {});

    // Add missing modules
    db["quantizer"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("CV In", 0, PinDataType::CV),
         AudioPin("Scale Mod", 1, PinDataType::CV),
         AudioPin("Root Mod", 2, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::CV)},
        {});

    db["timepitch"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("Audio In", 0, PinDataType::Audio),
         AudioPin("Speed Mod", 1, PinDataType::CV),
         AudioPin("Pitch Mod", 2, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});

    // Note: TTS Performer pin database is defined earlier in this function (around line 378)
    // Duplicate entry removed to avoid conflicts

    // Removed alias: enforce canonical name "track_mixer"

    // Add MIDI Player module
    db["midi_player"] = ModulePinInfo(NodeWidth::ExtraWide, {}, {}, {});

    // Add converter modules
    db["attenuverter"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Amount Mod", 2, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});

    // Add lowercase alias for Attenuverter
    // Add Sample & Hold module
    db["s_and_h"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In L", 0, PinDataType::Audio),
         AudioPin("In R", 1, PinDataType::Audio),
         AudioPin("Trigger In", 2, PinDataType::Gate),
         AudioPin("Threshold Mod", 3, PinDataType::CV),
         AudioPin("Edge Mod", 4, PinDataType::CV),
         AudioPin("Slew Mod", 5, PinDataType::CV)},
        {AudioPin("Out L", 0, PinDataType::Audio),
         AudioPin("Out R", 1, PinDataType::Audio),
         AudioPin("Smoothed Out", 2, PinDataType::Audio),
         AudioPin("Trigger Out", 3, PinDataType::Gate)},
        {ModPin("Threshold", "threshold_mod", PinDataType::CV),
         ModPin("Edge", "edge_mod", PinDataType::CV),
         ModPin("Slew", "slew_mod", PinDataType::CV)});

    db["map_range"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Raw In", 0, PinDataType::Raw)},
        {AudioPin("CV Out", 0, PinDataType::CV), AudioPin("Audio Out", 1, PinDataType::Audio)},
        {ModPin("Min In", "minIn", PinDataType::Raw),
         ModPin("Max In", "maxIn", PinDataType::Raw),
         ModPin("Min Out", "minOut", PinDataType::Raw),
         ModPin("Max Out", "maxOut", PinDataType::Raw)});

    db["lag_processor"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Signal In", 0, PinDataType::CV),
         AudioPin("Rise Mod", 1, PinDataType::CV),
         AudioPin("Fall Mod", 2, PinDataType::CV)},
        {AudioPin("Smoothed Out", 0, PinDataType::CV)},
        {});

    db["de_crackle"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In L", 0, PinDataType::Audio), AudioPin("In R", 1, PinDataType::Audio)},
        {AudioPin("Out L", 0, PinDataType::Audio), AudioPin("Out R", 1, PinDataType::Audio)},
        {});

    // ADD MISSING MODULES FOR COLOR-CODED CHAINING

    db["scope"] = ModulePinInfo(
        NodeWidth::Medium,
        {AudioPin("In", 0, PinDataType::Audio)},
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});

    db["logic"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In A", 0, PinDataType::Gate), AudioPin("In B", 1, PinDataType::Gate)},
        {AudioPin("AND", 0, PinDataType::Gate),
         AudioPin("OR", 1, PinDataType::Gate),
         AudioPin("XOR", 2, PinDataType::Gate),
         AudioPin("NOT A", 3, PinDataType::Gate)},
        {});

    db["clock_divider"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Clock In", 0, PinDataType::Gate), AudioPin("Reset", 1, PinDataType::Gate)},
        {AudioPin("/2", 0, PinDataType::Gate),
         AudioPin("/4", 1, PinDataType::Gate),
         AudioPin("/8", 2, PinDataType::Gate),
         AudioPin("x2", 3, PinDataType::Gate),
         AudioPin("x3", 4, PinDataType::Gate),
         AudioPin("x4", 5, PinDataType::Gate)},
        {});

    db["rate"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Rate Mod", 0, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::CV)},
        {});

    // ADD REMAINING MISSING MODULES FROM CMAKE LISTS

    db["math"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In A", 0, PinDataType::CV),
         AudioPin("In B", 1, PinDataType::CV),
         AudioPin("Op Mod", 2, PinDataType::CV)},
        {AudioPin("Out", 0, PinDataType::CV)},
        {ModPin("Operation", "operation_mod", PinDataType::CV)});

    db["sequential_switch"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("Gate In", 0, PinDataType::Audio),
         AudioPin("Thresh 1 CV", 1, PinDataType::CV),
         AudioPin("Thresh 2 CV", 2, PinDataType::CV),
         AudioPin("Thresh 3 CV", 3, PinDataType::CV),
         AudioPin("Thresh 4 CV", 4, PinDataType::CV)},
        {AudioPin("Out 1", 0, PinDataType::Audio),
         AudioPin("Out 2", 1, PinDataType::Audio),
         AudioPin("Out 3", 2, PinDataType::Audio),
         AudioPin("Out 4", 3, PinDataType::Audio)},
        {});

    {
        ModulePinInfo inletPins(NodeWidth::Small, {}, {}, {});
        for (int ch = 0; ch < 16; ++ch)
            inletPins.audioOuts.emplace_back(
                juce::String("Out ") + juce::String(ch + 1), ch, PinDataType::Audio);
        db["inlet"] = inletPins;
    }

    {
        ModulePinInfo outletPins(NodeWidth::Small, {}, {}, {});
        for (int ch = 0; ch < 16; ++ch)
            outletPins.audioIns.emplace_back(
                juce::String("In ") + juce::String(ch + 1), ch, PinDataType::Audio);
        db["outlet"] = outletPins;
    }

    db["meta_module"] = ModulePinInfo(NodeWidth::Medium, {}, {}, {});
    db["meta"] = ModulePinInfo(NodeWidth::Medium, {}, {}, {});

    db["snapshot_sequencer"] = ModulePinInfo(
        NodeWidth::ExtraWide,
        {AudioPin("Clock", 0, PinDataType::Gate), AudioPin("Reset", 1, PinDataType::Gate)},
        {}, // No audio outputs
        {});

    // Chord / Arp harmony brain node
    db["chord_arp"] = ModulePinInfo(
        NodeWidth::Medium,
        {// Single unified input bus (bus 0) – indices match ChordArpModuleProcessor constructor
         AudioPin("Degree In", 0, PinDataType::CV),
         AudioPin("Root CV In", 1, PinDataType::CV),
         AudioPin("Chord Mode Mod", 2, PinDataType::CV),
         AudioPin("Arp Rate Mod", 3, PinDataType::CV),
         AudioPin("Scale Mod", 4, PinDataType::CV),
         AudioPin("Key Mod", 5, PinDataType::CV),
         AudioPin("Voicing Mod", 6, PinDataType::CV),
         AudioPin("Arp Mode Mod", 7, PinDataType::CV),
         AudioPin("Range Mode Mod", 8, PinDataType::CV),
         AudioPin("Voices Mod", 9, PinDataType::CV)},
        {// Outputs: 4 voices (pitch/gate pairs) + arp pitch/gate
         AudioPin("Pitch 1", 0, PinDataType::CV),
         AudioPin("Gate 1", 1, PinDataType::Gate),
         AudioPin("Pitch 2", 2, PinDataType::CV),
         AudioPin("Gate 2", 3, PinDataType::Gate),
         AudioPin("Pitch 3", 4, PinDataType::CV),
         AudioPin("Gate 3", 5, PinDataType::Gate),
         AudioPin("Pitch 4", 6, PinDataType::CV),
         AudioPin("Gate 4", 7, PinDataType::Gate),
         AudioPin("Arp Pitch", 8, PinDataType::CV),
         AudioPin("Arp Gate", 9, PinDataType::Gate)},
        {// Modulation pins for UI parameter disabling / highlighting
         ModPin("Degree", "degree_mod", PinDataType::CV),
         ModPin("Root CV", "root_cv_mod", PinDataType::CV),
         ModPin("Chord Mode", "chordMode_mod", PinDataType::CV),
         ModPin("Arp Rate", "arpRate_mod", PinDataType::CV),
         ModPin("Scale", "scale_mod", PinDataType::CV),
         ModPin("Key", "key_mod", PinDataType::CV),
         ModPin("Voicing", "voicing_mod", PinDataType::CV),
         ModPin("Arp Mode", "arpMode_mod", PinDataType::CV),
         ModPin("Range Mode", "rangeMode_mod", PinDataType::CV),
         ModPin("Voices", "numVoices_mod", PinDataType::CV)});

    // OSC CV - Dynamic OSC to CV converter (outputs are dynamic based on mapped addresses)
    db["osc_cv"] = ModulePinInfo(
        NodeWidth::Big, // Big width to accommodate monitor addresses section
        {},             // No inputs - receives OSC messages via handleOscSignal
        {},             // Outputs are dynamic - generated via getDynamicOutputPins()
        {});

    // CV OSC Sender - Dynamic CV to OSC converter (inputs are dynamic based on mappings)
    db["cv_osc_sender"] = ModulePinInfo(
        NodeWidth::ExtraWide, // Extra wide for large input mappings list
        {},                   // Inputs are dynamic - generated via getDynamicInputPins()
        {},                   // No outputs - sends OSC messages
        {});

    // MIDI CV - Polyphonic 8-voice converter
    db["midi_cv"] = ModulePinInfo(
        NodeWidth::Big,
        {}, // No inputs - receives MIDI messages via handleDeviceSpecificMidi
        {   // Voice 1 outputs (channels 0-2)
         AudioPin("V1 Gate", 0, PinDataType::Gate),
         AudioPin("V1 Pitch", 1, PinDataType::CV),
         AudioPin("V1 Vel", 2, PinDataType::CV),
         // Voice 2 outputs (channels 3-5)
         AudioPin("V2 Gate", 3, PinDataType::Gate),
         AudioPin("V2 Pitch", 4, PinDataType::CV),
         AudioPin("V2 Vel", 5, PinDataType::CV),
         // Voice 3 outputs (channels 6-8)
         AudioPin("V3 Gate", 6, PinDataType::Gate),
         AudioPin("V3 Pitch", 7, PinDataType::CV),
         AudioPin("V3 Vel", 8, PinDataType::CV),
         // Voice 4 outputs (channels 9-11)
         AudioPin("V4 Gate", 9, PinDataType::Gate),
         AudioPin("V4 Pitch", 10, PinDataType::CV),
         AudioPin("V4 Vel", 11, PinDataType::CV),
         // Voice 5 outputs (channels 12-14)
         AudioPin("V5 Gate", 12, PinDataType::Gate),
         AudioPin("V5 Pitch", 13, PinDataType::CV),
         AudioPin("V5 Vel", 14, PinDataType::CV),
         // Voice 6 outputs (channels 15-17)
         AudioPin("V6 Gate", 15, PinDataType::Gate),
         AudioPin("V6 Pitch", 16, PinDataType::CV),
         AudioPin("V6 Vel", 17, PinDataType::CV),
         // Voice 7 outputs (channels 18-20)
         AudioPin("V7 Gate", 18, PinDataType::Gate),
         AudioPin("V7 Pitch", 19, PinDataType::CV),
         AudioPin("V7 Vel", 20, PinDataType::CV),
         // Voice 8 outputs (channels 21-23)
         AudioPin("V8 Gate", 21, PinDataType::Gate),
         AudioPin("V8 Pitch", 22, PinDataType::CV),
         AudioPin("V8 Vel", 23, PinDataType::CV),
         // Global controller outputs (channels 24-26)
         AudioPin("Mod Wheel", 24, PinDataType::CV),
         AudioPin("Pitch Bend", 25, PinDataType::CV),
         AudioPin("Aftertouch", 26, PinDataType::CV)},
        {});

    // MIDI Family - New Modules with Correct Pin Types
    {
        // MIDI Faders: All outputs are CV (blue)
        db["midi_faders"] = ModulePinInfo();
        db["midi_faders"].defaultWidth = NodeWidth::Big;
        for (int i = 0; i < 16; ++i)
            db["midi_faders"].audioOuts.emplace_back(
                "Fader " + juce::String(i + 1), i, PinDataType::CV);

        // MIDI Knobs: All outputs are CV (blue)
        db["midi_knobs"] = ModulePinInfo();
        db["midi_knobs"].defaultWidth = NodeWidth::Big;
        for (int i = 0; i < 16; ++i)
            db["midi_knobs"].audioOuts.emplace_back(
                "Knob " + juce::String(i + 1), i, PinDataType::CV);

        // MIDI Buttons: All outputs are Gate/Trigger (yellow)
        db["midi_buttons"] = ModulePinInfo();
        db["midi_buttons"].defaultWidth = NodeWidth::Big;
        for (int i = 0; i < 32; ++i)
            db["midi_buttons"].audioOuts.emplace_back(
                "Button " + juce::String(i + 1), i, PinDataType::Gate);

        // MIDI Jog Wheel: Output is CV (blue)
        db["midi_jog_wheel"] =
            ModulePinInfo(NodeWidth::Small, {}, {AudioPin("Value", 0, PinDataType::CV)}, {});
    }

    db["debug"] = ModulePinInfo(
        NodeWidth::Small,
        {AudioPin("In", 0, PinDataType::Audio)},
        {}, // No outputs
        {});

    db["input_debug"] = ModulePinInfo(
        NodeWidth::Small,
        {}, // No inputs
        {AudioPin("Out", 0, PinDataType::Audio)},
        {});

    // Tempo Clock
    db["tempo_clock"] = ModulePinInfo(
        NodeWidth::ExtraWide,
        {AudioPin("BPM Mod", 0, PinDataType::CV),
         AudioPin("Tap", 1, PinDataType::Gate),
         AudioPin("Nudge+", 2, PinDataType::Gate),
         AudioPin("Nudge-", 3, PinDataType::Gate),
         AudioPin("Play", 4, PinDataType::Gate),
         AudioPin("Stop", 5, PinDataType::Gate),
         AudioPin("Reset", 6, PinDataType::Gate),
         AudioPin("Swing Mod", 7, PinDataType::CV)},
        {AudioPin("Clock", 0, PinDataType::Gate),
         AudioPin("Beat Trig", 1, PinDataType::Gate),
         AudioPin("Bar Trig", 2, PinDataType::Gate),
         AudioPin("Beat Gate", 3, PinDataType::Gate),
         AudioPin("Phase", 4, PinDataType::CV),
         AudioPin("BPM CV", 5, PinDataType::CV),
         AudioPin("Downbeat", 6, PinDataType::Gate)},
        {ModPin("BPM", "bpm_mod", PinDataType::CV),
         ModPin("Tap", "tap_mod", PinDataType::Gate),
         ModPin("Nudge+", "nudge_up_mod", PinDataType::Gate),
         ModPin("Nudge-", "nudge_down_mod", PinDataType::Gate),
         ModPin("Play", "play_mod", PinDataType::Gate),
         ModPin("Stop", "stop_mod", PinDataType::Gate),
         ModPin("Reset", "reset_mod", PinDataType::Gate),
         ModPin("Swing", "swing_mod", PinDataType::CV)});

    // Timeline - Uses dynamic pins based on automation channels
    db["timeline"] = ModulePinInfo(
        NodeWidth::Big,
        {}, // Dynamic inputs defined by module (one per automation channel)
        {}, // Dynamic outputs defined by module (one per automation channel)
        {});

    // BPM Monitor - Uses dynamic pins based on detected rhythm sources
    db["bpm_monitor"] = ModulePinInfo(
        NodeWidth::Big,
        {}, // Dynamic inputs defined by module (beat detection inputs)
        {}, // Dynamic outputs defined by module (per-source BPM/CV/Active)
        {});

    // Physics Module - Exception size (custom dimensions defined by module)
    db["physics"] = ModulePinInfo(
        NodeWidth::Exception,
        {}, // Dynamic inputs defined by module
        {}, // Dynamic outputs defined by module
        {});

    db["webcam_loader"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for video display
        {},                   // No inputs
        {AudioPin("Source ID", 0, PinDataType::Video)},
        {});

    db["video_file_loader"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for video display
        {},                   // No inputs
        {AudioPin("Source ID", 0, PinDataType::Video)},
        {});

    db["movement_detector"] = ModulePinInfo(
        NodeWidth::Exception,
        {AudioPin("Source In", 0, PinDataType::Video)},
        {
            AudioPin("Motion X", 0, PinDataType::CV),
            AudioPin("Motion Y", 1, PinDataType::CV),
            AudioPin("Amount", 2, PinDataType::CV),
            AudioPin("Trigger", 3, PinDataType::Gate),
            AudioPin("Red Zone Gate", 4, PinDataType::Gate),
            AudioPin("Green Zone Gate", 5, PinDataType::Gate),
            AudioPin("Blue Zone Gate", 6, PinDataType::Gate),
            AudioPin("Yellow Zone Gate", 7, PinDataType::Gate),
            AudioPin("Video Out", 0, PinDataType::Video) // Bus 1
        },
        {});

    // Object Detector (YOLOv3) - 1 input (Source ID) and 7 outputs (X,Y,Width,Height,Gate,Video
    // Out,Cropped Out)
    db["object_detector"] = ModulePinInfo(
        NodeWidth::Exception,
        {AudioPin("Source In", 0, PinDataType::Video)},
        {
            AudioPin("X", 0, PinDataType::CV),
            AudioPin("Y", 1, PinDataType::CV),
            AudioPin("Width", 2, PinDataType::CV),
            AudioPin("Height", 3, PinDataType::CV),
            AudioPin("Gate", 4, PinDataType::Gate),
            AudioPin("Red Zone Gate", 5, PinDataType::Gate),
            AudioPin("Green Zone Gate", 6, PinDataType::Gate),
            AudioPin("Blue Zone Gate", 7, PinDataType::Gate),
            AudioPin("Yellow Zone Gate", 8, PinDataType::Gate),
            AudioPin("Video Out", 0, PinDataType::Video),  // Bus 1
            AudioPin("Cropped Out", 1, PinDataType::Video) // Bus 2
        },
        {});

    // Color Tracker: dynamic outputs (3 per color) + 4 zone gates. Only declare input here.
    db["color_tracker"] = ModulePinInfo(
        NodeWidth::Exception, // custom node width with zoom
        {AudioPin("Source In", 0, PinDataType::Video)},
        {
            AudioPin("Video Out", 0, PinDataType::Video) // Bus 1 - dynamic color pins and zone
                                                         // gates are added programmatically
        },
        {});

    // Pose Estimator: 15 keypoints x 2 coordinates = 30 output pins + Video Out
    db["pose_estimator"] = ModulePinInfo();
    db["pose_estimator"].defaultWidth = NodeWidth::Exception; // Custom size with zoom support
    db["pose_estimator"].audioIns.emplace_back("Source In", 0, PinDataType::Video);
    db["pose_estimator"].audioIns.emplace_back("Confidence Mod", 1, PinDataType::CV);
    // Programmatically add all 30 output pins (15 keypoints x 2 coordinates)
    const std::vector<std::string> keypointNames = {
        "Head",
        "Neck",
        "R Shoulder",
        "R Elbow",
        "R Wrist",
        "L Shoulder",
        "L Elbow",
        "L Wrist",
        "R Hip",
        "R Knee",
        "R Ankle",
        "L Hip",
        "L Knee",
        "L Ankle",
        "Chest"};
    for (size_t i = 0; i < keypointNames.size(); ++i)
    {
        db["pose_estimator"].audioOuts.emplace_back(
            keypointNames[i] + " X", static_cast<int>(i * 2), PinDataType::CV);
        db["pose_estimator"].audioOuts.emplace_back(
            keypointNames[i] + " Y", static_cast<int>(i * 2 + 1), PinDataType::CV);
    }
    // Add zone gate pins (channels 30-33)
    db["pose_estimator"].audioOuts.emplace_back("Red Zone Gate", 30, PinDataType::Gate);
    db["pose_estimator"].audioOuts.emplace_back("Green Zone Gate", 31, PinDataType::Gate);
    db["pose_estimator"].audioOuts.emplace_back("Blue Zone Gate", 32, PinDataType::Gate);
    db["pose_estimator"].audioOuts.emplace_back("Yellow Zone Gate", 33, PinDataType::Gate);
    // Add Video Out and Cropped Out pins (bus 1 and 2)
    db["pose_estimator"].audioOuts.emplace_back("Video Out", 0, PinDataType::Video);
    db["pose_estimator"].audioOuts.emplace_back("Cropped Out", 1, PinDataType::Video);
    // Add modulation pin for confidence CV input
    db["pose_estimator"].modIns.emplace_back("Confidence", "confidence_mod", PinDataType::CV);

    // Hand Tracker: 21 keypoints x 2 = 42 outs
    //   Channels 0-1: Wrist X/Y (absolute screen position)
    //   Channels 2-41: Other keypoints X/Y (relative to wrist)
    db["hand_tracker"] = ModulePinInfo();
    db["hand_tracker"].defaultWidth = NodeWidth::Exception;
    db["hand_tracker"].audioIns.emplace_back("Source In", 0, PinDataType::Video);
    const char* handNames[21] = {
        "Wrist",   "Thumb 1", "Thumb 2",  "Thumb 3",  "Thumb 4",  "Index 1",  "Index 2",
        "Index 3", "Index 4", "Middle 1", "Middle 2", "Middle 3", "Middle 4", "Ring 1",
        "Ring 2",  "Ring 3",  "Ring 4",   "Pinky 1",  "Pinky 2",  "Pinky 3",  "Pinky 4"};
    // Add wrist pins (absolute position)
    db["hand_tracker"].audioOuts.emplace_back("Wrist X (Abs)", 0, PinDataType::CV);
    db["hand_tracker"].audioOuts.emplace_back("Wrist Y (Abs)", 1, PinDataType::CV);
    // Add all other keypoint pins (relative to wrist)
    for (int i = 1; i < 21; ++i)
    {
        db["hand_tracker"].audioOuts.emplace_back(
            std::string(handNames[i]) + " X (Rel)", i * 2, PinDataType::CV);
        db["hand_tracker"].audioOuts.emplace_back(
            std::string(handNames[i]) + " Y (Rel)", i * 2 + 1, PinDataType::CV);
    }
    // Add zone gate pins (channels 42-45)
    db["hand_tracker"].audioOuts.emplace_back("Red Zone Gate", 42, PinDataType::Gate);
    db["hand_tracker"].audioOuts.emplace_back("Green Zone Gate", 43, PinDataType::Gate);
    db["hand_tracker"].audioOuts.emplace_back("Blue Zone Gate", 44, PinDataType::Gate);
    db["hand_tracker"].audioOuts.emplace_back("Yellow Zone Gate", 45, PinDataType::Gate);
    // Add Video Out and Cropped Out pins (bus 1 and 2)
    db["hand_tracker"].audioOuts.emplace_back("Video Out", 0, PinDataType::Video);
    db["hand_tracker"].audioOuts.emplace_back("Cropped Out", 1, PinDataType::Video);

    // Face Tracker: Simplified to 36 CV outputs + 2 Video outputs
    //   Channels 0-1: Face Center X/Y (absolute screen position)
    //   Channels 2-3: Nose Base (relative to face center)
    //   Channels 4-11: Right Eye (4 points: Outer, Top, Inner, Bottom)
    //   Channels 12-19: Left Eye (4 points: Inner, Top, Outer, Bottom)
    //   Channels 20-27: Mouth (4 points: Corner R, Top Center, Corner L, Bottom Center)
    //   Channels 28-35: Eyebrows (4 points: R Outer, R Inner, L Inner, L Outer)
    //   Bus 1: Video Out (passthrough)
    //   Bus 2: Cropped Out (cropped face region)
    db["face_tracker"] = ModulePinInfo();
    db["face_tracker"].defaultWidth = NodeWidth::Exception;
    db["face_tracker"].audioIns.emplace_back("Source In", 0, PinDataType::Video);

    // Face Center (absolute position)
    db["face_tracker"].audioOuts.emplace_back("Face Center X (Abs)", 0, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Face Center Y (Abs)", 1, PinDataType::CV);

    // Nose Base (channels 2, 3)
    db["face_tracker"].audioOuts.emplace_back("Nose Base X (Rel)", 2, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Nose Base Y (Rel)", 3, PinDataType::CV);

    // Right Eye (channels 4-11)
    db["face_tracker"].audioOuts.emplace_back("R Eye Outer X (Rel)", 4, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Outer Y (Rel)", 5, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Top X (Rel)", 6, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Top Y (Rel)", 7, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Inner X (Rel)", 8, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Inner Y (Rel)", 9, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Bottom X (Rel)", 10, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eye Bottom Y (Rel)", 11, PinDataType::CV);

    // Left Eye (channels 12-19)
    db["face_tracker"].audioOuts.emplace_back("L Eye Inner X (Rel)", 12, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Inner Y (Rel)", 13, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Top X (Rel)", 14, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Top Y (Rel)", 15, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Outer X (Rel)", 16, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Outer Y (Rel)", 17, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Bottom X (Rel)", 18, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eye Bottom Y (Rel)", 19, PinDataType::CV);

    // Mouth (channels 20-27)
    db["face_tracker"].audioOuts.emplace_back("Mouth Corner R X (Rel)", 20, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Corner R Y (Rel)", 21, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Top Center X (Rel)", 22, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Top Center Y (Rel)", 23, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Corner L X (Rel)", 24, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Corner L Y (Rel)", 25, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Bottom Center X (Rel)", 26, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("Mouth Bottom Center Y (Rel)", 27, PinDataType::CV);

    // Eyebrows (channels 28-35)
    db["face_tracker"].audioOuts.emplace_back("R Eyebrow Outer X (Rel)", 28, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eyebrow Outer Y (Rel)", 29, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eyebrow Inner X (Rel)", 30, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("R Eyebrow Inner Y (Rel)", 31, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eyebrow Inner X (Rel)", 32, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eyebrow Inner Y (Rel)", 33, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eyebrow Outer X (Rel)", 34, PinDataType::CV);
    db["face_tracker"].audioOuts.emplace_back("L Eyebrow Outer Y (Rel)", 35, PinDataType::CV);

    // Add zone gate pins (channels 36-39)
    db["face_tracker"].audioOuts.emplace_back("Red Zone Gate", 36, PinDataType::Gate);
    db["face_tracker"].audioOuts.emplace_back("Green Zone Gate", 37, PinDataType::Gate);
    db["face_tracker"].audioOuts.emplace_back("Blue Zone Gate", 38, PinDataType::Gate);
    db["face_tracker"].audioOuts.emplace_back("Yellow Zone Gate", 39, PinDataType::Gate);

    // Add Video Out and Cropped Out pins (bus 1 and 2, not CV channels)
    db["face_tracker"].audioOuts.emplace_back(
        "Video Out", 0, PinDataType::Video); // Bus 1, channel 0
    db["face_tracker"].audioOuts.emplace_back(
        "Cropped Out", 1, PinDataType::Video); // Bus 2, channel 1

    // Contour Detector: 1 input, 3 CV outputs + 4 zone gates + Video Out
    // Outputs are dynamic (defined by getDynamicOutputPins()), but we list them here for
    // documentation
    db["contour_detector"] = ModulePinInfo(
        NodeWidth::Exception,
        {AudioPin("Source In", 0, PinDataType::Video)},
        {
            AudioPin("Area", 0, PinDataType::CV),
            AudioPin("Complexity", 1, PinDataType::CV),
            AudioPin("Aspect Ratio", 2, PinDataType::CV),
            AudioPin("Red Zone Gate", 3, PinDataType::Gate),
            AudioPin("Green Zone Gate", 4, PinDataType::Gate),
            AudioPin("Blue Zone Gate", 5, PinDataType::Gate),
            AudioPin("Yellow Zone Gate", 6, PinDataType::Gate),
            AudioPin("Video Out", 0, PinDataType::Video) // Bus 1 (channel 0 on bus 1)
        },
        {});

    // Video FX Module - Uses dynamic pins based on video source
    db["video_fx"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for video preview
        {}, // Dynamic inputs defined by module (video source + optional CV parameters)
        {}, // Dynamic outputs defined by module (processed video output)
        {});

    // Chromakey Module - Uses dynamic pins for video input and dual outputs (RGBA + Alpha)
    db["chromakey"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for video preview
        {},                   // Dynamic inputs defined by module (video source)
        {},                   // Dynamic outputs defined by module (RGBA video + mask channel)
        {});

    // Video Compositor Module - Uses dynamic pins for multiple video layers
    db["video_compositor"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for video preview
        {},                   // Dynamic inputs defined by module (1-8 video layer inputs)
        {},                   // Dynamic outputs defined by module (composited video output)
        {});

    // Video Draw Impact Module - Uses dynamic pins for video I/O
    db["video_draw_impact"] = ModulePinInfo(
        NodeWidth::Exception, // Custom size for video preview and drawing interface
        {AudioPin("Source In", 0, PinDataType::Video)},
        {AudioPin("Output", 0, PinDataType::Video)},
        {});

    // Animation Module - dynamic outputs (bone velocities/triggers) defined at runtime
    // Keep empty pins here so validation recognizes the module; connections will be validated as
    // warnings if channels don't match
    db["animation"] = ModulePinInfo(NodeWidth::Exception, {}, {}, {});

    // Crop Video Module - takes source ID and CV modulation signals (X, Y, W, H) to crop a video
    // stream
    db["crop_video"] = ModulePinInfo(
        NodeWidth::Exception, // Uses custom size for video preview
        {AudioPin("Source In", 0, PinDataType::Video)},
        {AudioPin("Output ID", 0, PinDataType::Video)},
        {ModPin("Center X", "cropX_mod", PinDataType::CV),
         ModPin("Center Y", "cropY_mod", PinDataType::CV),
         ModPin("Width", "cropW_mod", PinDataType::CV),
         ModPin("Height", "cropH_mod", PinDataType::CV)});
}
