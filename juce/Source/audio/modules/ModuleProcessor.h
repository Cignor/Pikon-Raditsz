#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional> // Required for std::function
#include <vector>
#include <map>
#include <unordered_map>
#include <atomic>

// OSC support - include full definition for nested type in virtual function signature
#include "../OscDeviceManager.h"

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <cmath>
#endif

// <<< TRANSPORT STATE FOR GLOBAL CLOCK >>>
// Transport commands for Play/Pause/Stop intent
enum class TransportCommand : int
{
    Play = 0,
    Pause = 1,
    Stop = 2
};

// Transport state struct shared by all modules
struct TransportState
{
    bool   isPlaying = false;
    double bpm = 120.0;
    double songPositionBeats = 0.0;
    double songPositionSeconds = 0.0;
    // Optional global division broadcast from a master tempo/clock (-1 means inactive)
    std::atomic<int> globalDivisionIndex{-1};
    // Flag to indicate if a Tempo Clock module is controlling the BPM (for UI feedback)
    std::atomic<bool> isTempoControlledByModule{false};
    // Last transport command issued (Play/Pause/Stop)
    std::atomic<TransportCommand> lastCommand{TransportCommand::Stop};

    // Global reset flag (Pulse)
    // When true, all time-based modules (LFOs, Sequencers) must reset phase to 0
    // This is set to true for one block when a Timeline Master (e.g., SampleLoader) loops
    std::atomic<bool> forceGlobalReset{false};

    // Custom copy constructor (atomics are not copyable by default)
    TransportState() = default;
    TransportState(const TransportState& other)
        : isPlaying(other.isPlaying), bpm(other.bpm), songPositionBeats(other.songPositionBeats),
          songPositionSeconds(other.songPositionSeconds),
          globalDivisionIndex(other.globalDivisionIndex.load()),
          isTempoControlledByModule(other.isTempoControlledByModule.load()),
          lastCommand(other.lastCommand.load()), forceGlobalReset(other.forceGlobalReset.load())
    {
    }

    // Custom copy assignment operator
    TransportState& operator=(const TransportState& other)
    {
        if (this != &other)
        {
            isPlaying = other.isPlaying;
            bpm = other.bpm;
            songPositionBeats = other.songPositionBeats;
            songPositionSeconds = other.songPositionSeconds;
            globalDivisionIndex.store(other.globalDivisionIndex.load());
            isTempoControlledByModule.store(other.isTempoControlledByModule.load());
            lastCommand.store(other.lastCommand.load());
            forceGlobalReset.store(other.forceGlobalReset.load());
        }
        return *this;
    }
};

// === RHYTHM REPORTING SYSTEM ===
// Allows modules to report their rhythmic timing for the BPM Monitor node

/**
 * Rhythm information reported by modules that produce rhythmic patterns
 */
struct RhythmInfo
{
    juce::String displayName; // e.g., "Sequencer #3", "Animation: Walk Cycle"
    float        bpm;         // Current BPM (can be modulated live value)
    bool         isActive;    // Is this source currently producing rhythm?
    bool         isSynced;    // Is it synced to global transport?
    juce::String sourceType;  // "sequencer", "animation", "physics", etc.

    RhythmInfo() : bpm(0.0f), isActive(false), isSynced(false) {}
    RhythmInfo(
        const juce::String& name,
        float               bpmValue,
        bool                active,
        bool                synced,
        const juce::String& type = "")
        : displayName(name), bpm(bpmValue), isActive(active), isSynced(synced), sourceType(type)
    {
    }
};

/**
 * Beat detection source (from audio input analysis)
 * Used by the BPM Monitor's tap tempo engine
 */
struct DetectedRhythmSource
{
    juce::String name;         // e.g., "Input 1 (Detected)"
    int          inputChannel; // Which input is being analyzed
    float        detectedBPM;  // Calculated BPM from beat detection
    float        confidence;   // 0.0-1.0 (how stable is the detection)
    bool         isActive;     // Currently detecting beats?

    DetectedRhythmSource() : inputChannel(-1), detectedBPM(0.0f), confidence(0.0f), isActive(false)
    {
    }
};

// <<< MULTI-MIDI DEVICE SUPPORT >>>
// MIDI message with device source information
// This struct allows modules to filter MIDI by device and channel
struct MidiMessageWithDevice
{
    juce::MidiMessage message;
    juce::String      deviceIdentifier;
    juce::String      deviceName;
    int               deviceIndex = -1;
};

// <<< OSC SUPPORT >>>
// OSC message with source information (defined in OscDeviceManager.h)
// Cannot use type alias here because OscDeviceManager is forward-declared
// Files using this must include OscDeviceManager.h and use OscDeviceManager::OscMessageWithSource

// <<< ALL PIN-RELATED DEFINITIONS ARE NOW CENTRALIZED HERE >>>

// Defines the data type of a modulation or audio signal
enum class PinDataType
{
    CV,
    Audio,
    Gate,
    Raw,
    Video
};

// Forward declare NodeWidth enum (defined in ImGuiNodeEditorComponent.h)
// This avoids circular dependency while allowing ModulePinInfo to store it
enum class NodeWidth;

// Describes a single audio/CV input or output pin
struct AudioPin
{
    juce::String name;
    int          channel;
    PinDataType  type;

    AudioPin(const juce::String& n, int ch, PinDataType t) : name(n), channel(ch), type(t) {}
};

// Renamed to avoid conflict with ImGuiNodeEditorComponent's PinInfo
struct DynamicPinInfo
{
    juce::String name;
    int          channel;
    PinDataType  type;

    // Constructor to allow brace-initialization
    DynamicPinInfo(const juce::String& n, int c, PinDataType t) : name(n), channel(c), type(t) {}
};

// Describes a single modulation input pin targeting a parameter
struct ModPin
{
    juce::String name;
    juce::String paramId;
    PinDataType  type;

    ModPin(const juce::String& n, const juce::String& p, PinDataType t)
        : name(n), paramId(p), type(t)
    {
    }
};

// A collection of all pins for a given module type
struct ModulePinInfo
{
    NodeWidth             defaultWidth; // Standardized node width category
    std::vector<AudioPin> audioIns;
    std::vector<AudioPin> audioOuts;
    std::vector<ModPin>   modIns;

    ModulePinInfo() : defaultWidth(static_cast<NodeWidth>(0)) {} // Default to Small (0)

    ModulePinInfo(
        NodeWidth                       width,
        std::initializer_list<AudioPin> ins,
        std::initializer_list<AudioPin> outs,
        std::initializer_list<ModPin>   mods)
        : defaultWidth(width), audioIns(ins), audioOuts(outs), modIns(mods)
    {
    }
};

// Forward declaration for NodePinHelpers
class ModuleProcessor;

// Helper struct passed to modules for drawing their pins
struct NodePinHelpers
{
    std::function<void(const char* label, int channel)> drawAudioInputPin;
    std::function<void(const char* label, int channel)> drawAudioOutputPin;
    std::function<void(const char* inLabel, int inChannel, const char* outLabel, int outChannel)>
                                                 drawParallelPins;
    std::function<void(ModuleProcessor* module)> drawIoPins;

    // Draw input pin inline (just the pin circle, no label) - used for inline modulation pins
    // Returns true if pin was drawn, false if callback not set
    std::function<bool(int channel)> drawInlineInputPin;

    // Check if an output pin has any connections (for smart collapsible sections)
    // Returns true if the output pin at the specified bus/channel has at least one cable connected
    std::function<bool(int busIndex, int channel)> isOutputPinConnected;
};

class ModularSynthProcessor; // forward declaration

/**
    An abstract base class for all modular synthesizer components.

    This class enforces a common interface for modules, ensuring they can be
    managed by the ModularSynthProcessor. The key requirement is providing access
    to the module's own parameter state via getAPVTS().
*/
class ModuleProcessor : public juce::AudioProcessor
{
public:
    ModuleProcessor(const BusesProperties& ioLayouts) : juce::AudioProcessor(ioLayouts) {}
    ~ModuleProcessor() override = default;

    // Parent container link (set by ModularSynthProcessor when node is created)
    void                   setParent(ModularSynthProcessor* parent) { parentSynth = parent; }
    ModularSynthProcessor* getParent() const { return parentSynth; }

    // Pure virtual method that all concrete modules MUST implement.
    // This is crucial for the parameter proxy system.
    virtual juce::AudioProcessorValueTreeState& getAPVTS() = 0;

    // Optional UI hook for drawing parameters inside nodes (used by Preset Creator)
    // pinHelpers is optional - when provided, modules can draw inline modulation pins
    // Optional UI hook for drawing parameters inside nodes (used by Preset Creator)
    // COMPATIBILITY: Restored 3-arg virtual for existing modules
    virtual void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded)
    {
        juce::ignoreUnused(itemWidth, isParamModulated, onModificationEnded);
    }

    // NEW: 4-arg virtual allowing modules to use inline pins.
    // Default implementation delegates to the 3-arg version.
    virtual void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded,
        const NodePinHelpers*                                   pinHelpers)
    {
        juce::ignoreUnused(pinHelpers);
        // Delegate to the legacy 3-arg version so modules that override THAT (and not this) still
        // get called
        drawParametersInNode(itemWidth, isParamModulated, onModificationEnded);
    }

#if defined(PRESET_CREATOR_UI)
    // Helper to draw standardized performance metrics
    void drawPerformanceMetrics(float width)
    {
        const float ms = lastProcessTimeMs.load(std::memory_order_relaxed);
        const bool  gpu = lastProcessWasGpu.load(std::memory_order_relaxed);

        if (ms < 0.0f)
            return; // Hide if invalid (uninitialized)

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("CPU: %.2f ms", ms);
        ImGui::SameLine();
        if (gpu)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "GPU: ON");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "GPU: OFF");

        // Gauge relative to 33ms (30fps)
        float progress = juce::jlimit(0.0f, 1.0f, ms / 33.3f);

        // Color code the bar
        ImVec4 color = ImVec4(0.2f, 0.8f, 0.2f, 1.0f); // Green
        if (progress > 0.5f)
            color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // Yellow
        if (progress > 0.9f)
            color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(progress, ImVec2(width, 0.0f));
        ImGui::PopStyleColor();
    }
#endif

    // Optional UI hook for drawing IO pins inside nodes
    virtual void drawIoPins(const NodePinHelpers& /*helpers*/) {}

    // Modules can override this to force Preset Creator to call drawIoPins()
    // even if dynamic pin info is available (needed for custom layouts).
    virtual bool usesCustomPinLayout() const { return false; }

#if defined(PRESET_CREATOR_UI)
    // Optional UI hook for modules that need custom node dimensions (Exception size category)
    // Return ImVec2(width, height) for custom size, or ImVec2(0, 0) to use default from PinDatabase
    // Height of 0 means auto-size to content (recommended for most cases)
    virtual ImVec2 getCustomNodeSize() const
    {
        return ImVec2(0.0f, 0.0f); // Default: use PinDatabase size
    }
#endif

    // Get the current output value for a channel (for visualization)
    virtual float getOutputChannelValue(int channel) const
    {
        if (juce::isPositiveAndBelow(channel, (int)lastOutputValues.size()) &&
            lastOutputValues[channel])
            return lastOutputValues[channel]->load();
        return 0.0f;
    }

    // Helper method to update output telemetry with peak magnitude
    // Call this at the end of processBlock to update visualization values
    void updateOutputTelemetry(const juce::AudioBuffer<float>& buffer)
    {
        const int numChannels = juce::jmin(buffer.getNumChannels(), (int)lastOutputValues.size());
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (lastOutputValues[ch])
            {
                // Use peak magnitude (max absolute value) for better visualization
                const float peak = buffer.getMagnitude(ch, 0, buffer.getNumSamples());
                lastOutputValues[ch]->store(peak, std::memory_order_relaxed);
            }
        }
    }

    // Standardized labels for module audio I/O channels (override per module if needed)
    virtual juce::String getAudioInputLabel(int channel) const
    {
        return juce::String("In ") + juce::String(channel + 1);
    }

    virtual juce::String getAudioOutputLabel(int channel) const
    {
        return juce::String("Out ") + juce::String(channel + 1);
    }

    // Stable logical ID assigned by ModularSynthProcessor upon node creation.
    void         setLogicalId(juce::uint32 id) { storedLogicalId = id; }
    juce::uint32 getLogicalId() const { return storedLogicalId; }

    // Secondary logical ID for extra outputs (like cropped video from detector nodes)
    void         setSecondaryLogicalId(juce::uint32 id) { storedSecondaryLogicalId = id; }
    juce::uint32 getSecondaryLogicalId() const { return storedSecondaryLogicalId; }

    // === COMPREHENSIVE DIAGNOSTICS SYSTEM ===

    // Get detailed connection information for debugging
    virtual juce::String getConnectionDiagnostics() const
    {
        juce::String result = "=== CONNECTION DIAGNOSTICS ===\n";

        // Bus layout info
        result += "Input Buses: " + juce::String(getBusCount(true)) + "\n";
        result += "Output Buses: " + juce::String(getBusCount(false)) + "\n";

        for (int bus = 0; bus < getBusCount(true); ++bus)
        {
            auto busName = getBus(true, bus)->getName();
            auto numChannels = getBus(true, bus)->getNumberOfChannels();
            result += "  Input Bus " + juce::String(bus) + ": \"" + busName + "\" (" +
                      juce::String(numChannels) + " channels)\n";
        }

        for (int bus = 0; bus < getBusCount(false); ++bus)
        {
            auto busName = getBus(false, bus)->getName();
            auto numChannels = getBus(false, bus)->getNumberOfChannels();
            result += "  Output Bus " + juce::String(bus) + ": \"" + busName + "\" (" +
                      juce::String(numChannels) + " channels)\n";
        }

        return result;
    }

    // Get parameter routing diagnostics
    virtual juce::String getParameterRoutingDiagnostics() const
    {
        juce::String result = "=== PARAMETER ROUTING DIAGNOSTICS ===\n";

        // Note: This method is const, so we can't access getAPVTS() directly
        // We'll return a placeholder for now
        result += "Parameter routing diagnostics require non-const access.\n";
        result += "Use getModuleDiagnostics() from ModularSynthProcessor instead.\n";

        return result;
    }

    // Get live parameter values for debugging
    virtual juce::String getLiveParameterDiagnostics() const
    {
        juce::String result = "=== LIVE PARAMETER VALUES ===\n";

        for (const auto& pair : paramLiveValues)
        {
            result += "  " + pair.first + ": " + juce::String(pair.second.load(), 4) + "\n";
        }

        return result;
    }

    // Get comprehensive module diagnostics
    virtual juce::String getAllDiagnostics() const
    {
        juce::String result = "=== MODULE DIAGNOSTICS ===\n";
        result += "Module Type: " + getName() + "\n\n";
        result += getConnectionDiagnostics() + "\n";
        result += getParameterRoutingDiagnostics() + "\n";
        result += getLiveParameterDiagnostics();
        return result;
    }

    /**
        Resolves a parameter's string ID to its modulation bus and channel.

        This is a virtual function that each module must override to declare which of its
        parameters can be modulated by an external signal. The function maps parameter IDs
        to their corresponding input bus and channel indices within that bus.

        @param paramId              The string ID of the parameter to query (e.g., "cutoff",
       "frequency").
        @param outBusIndex          Receives the index of the input bus used for modulation.
        @param outChannelIndexInBus Receives the channel index within that bus.
        @returns                    True if the parameter supports modulation, false otherwise.

        @see isParamInputConnected
    */
    virtual bool getParamRouting(
        const juce::String& paramId,
        int&                outBusIndex,
        int&                outChannelIndexInBus) const;

    /**
        Checks if a parameter's modulation input is connected in the synth graph.

        This is the single, reliable method for a module's audio thread to determine
        if it should use an incoming CV signal instead of its internal parameter value.
        The function internally uses getParamRouting() to resolve the parameter to its
        bus/channel location, then queries the parent synth's connection graph.

        @param paramId The string ID of the parameter to check (e.g., "cutoff", "frequency").
        @returns       True if a cable is connected to this parameter's modulation input.

        @see getParamRouting
    */
    bool isParamInputConnected(const juce::String& paramId) const;

    // --- Live telemetry for UI (thread-safe, lock-free) ---
    void setLiveParamValue(const juce::String& paramId, float value)
    {
        auto result = paramLiveValues.try_emplace(paramId, value);
        if (!result.second)
            result.first->second.store(value, std::memory_order_relaxed);
    }

    float getLiveParamValue(const juce::String& paramId, float fallback) const
    {
        // FIX: Only return the "live" (modulated) value if the corresponding
        // modulation input is actually connected. Otherwise, always return the
        // fallback, which is the base parameter's real value.
        if (isParamInputConnected(paramId))
        {
            if (auto it = paramLiveValues.find(paramId); it != paramLiveValues.end())
                return it->second.load(std::memory_order_relaxed);
        }
        return fallback;
    }

    // New helper: decouple the connectivity check (modParamId) from the live value key (liveKey).
    // This allows UI code to ask "is X_mod connected?" while reading the corresponding
    // live telemetry stored under a different key like "X_live".
    float getLiveParamValueFor(
        const juce::String& modParamId,
        const juce::String& liveKey,
        float               fallback) const
    {
        if (isParamInputConnected(modParamId))
        {
            if (auto it = paramLiveValues.find(liveKey); it != paramLiveValues.end())
                return it->second.load(std::memory_order_relaxed);
        }
        return fallback;
    }

    // Optional extra state hooks for modules that need to persist non-parameter data
    // Default: return invalid tree / ignore.
    virtual juce::ValueTree getExtraStateTree() const { return {}; }
    virtual void            setExtraStateTree(const juce::ValueTree&) {}

    // Optional timing info hook for modules that need global clock/transport
    // Default: ignore (modules that don't need timing can skip implementing this)
    virtual void setTimingInfo(const TransportState& state) { juce::ignoreUnused(state); }

    // Optional force stop hook for modules with playback state
    // Called after patch load to ensure all modules are stopped
    // Default: no-op (modules without playback state don't need this)
    virtual void forceStop() {}

    // Optional rhythm reporting hook for BPM Monitor node
    // Modules that produce rhythmic patterns can implement this to report their BPM
    // Default: return empty (module doesn't produce rhythm)
    virtual std::optional<RhythmInfo> getRhythmInfo() const { return std::nullopt; }

    // === TIMELINE REPORTING INTERFACE (for Timeline Sync feature) ===
    // Modules that can provide timeline position/duration (e.g., SampleLoader, VideoLoader)
    // should override these methods to report their state atomically.
    // Default: module does not provide timeline (returns false/0)

    virtual bool   canProvideTimeline() const { return false; }
    virtual double getTimelinePositionSeconds() const { return 0.0; }
    virtual double getTimelineDurationSeconds() const { return 0.0; }
    virtual bool   isTimelineActive() const { return false; }

    // Optional dynamic pin interface for modules with variable I/O (e.g., polyphonic modules)
    // Default: return empty vector (no dynamic pins)
    virtual std::vector<DynamicPinInfo> getDynamicInputPins() const { return {}; }
    virtual std::vector<DynamicPinInfo> getDynamicOutputPins() const { return {}; }

    /**
        Device-aware MIDI processing (MULTI-MIDI CONTROLLER SUPPORT)

        This method is called by ModularSynthProcessor BEFORE the standard graph processing
        begins. It provides MIDI modules with device-aware MIDI messages that include the
        source device information (name, identifier, index).

        MIDI modules should override this method to:
        - Filter messages by device (e.g., only respond to a specific controller)
        - Filter messages by MIDI channel
        - Update internal state based on filtered MIDI input

        The regular processBlock() can then use this updated state to generate CV outputs.

        @param midiMessages A vector of MIDI messages with device source information

        Default implementation: Does nothing (opt-in for MIDI modules only)

        @see MidiMessageWithDevice
    */
    virtual void handleDeviceSpecificMidi(const std::vector<MidiMessageWithDevice>& midiMessages)
    {
        juce::ignoreUnused(midiMessages);
        // Default: do nothing. MIDI-aware modules will override this method.
    }

    /**
        OSC signal processing (NETWORK-BASED CONTROL)

        This method is called by ModularSynthProcessor BEFORE the standard graph processing
        begins. It provides OSC modules with network-originated OSC messages that include
        source information (identifier, name, index).

        OSC modules should override this method to:
        - Filter messages by source (e.g., only respond to a specific OSC device)
        - Filter messages by OSC address pattern (e.g., "/cv/pitch", "/synth/note/on")
        - Update internal state based on filtered OSC input

        The regular processBlock() can then use this updated state to generate CV outputs.

        @param oscMessages A vector of OSC messages with source information

        Default implementation: Does nothing (opt-in for OSC modules only)

        @see OscMessageWithSource
    */
    virtual void handleOscSignal(
        const std::vector<OscDeviceManager::OscMessageWithSource>& oscMessages)
    {
        juce::ignoreUnused(oscMessages);
        // Default: do nothing. OSC-aware modules will override this method.
    }

public:
    // OPTION 9: Make public for TTS debugging
    // Live, modulated parameter values for UI feedback
    std::unordered_map<juce::String, std::atomic<float>> paramLiveValues;

    // Performance telemetry (CPU usage in ms, GPU active status)
    std::atomic<float> lastProcessTimeMs{0.0f};
    std::atomic<bool>  lastProcessWasGpu{false};

protected:
    // Thread-safe storage for last known output values (for tooltips)
    std::vector<std::unique_ptr<std::atomic<float>>> lastOutputValues;

#if defined(PRESET_CREATOR_UI)

    static void adjustParamOnWheel(
        juce::RangedAudioParameter* parameter,
        const juce::String&         idOrName,
        float                       displayedValue)
    {
        if (parameter == nullptr)
            return;
        if (!ImGui::IsItemHovered())
            return;
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel == 0.0f)
            return;

        if (auto* pf = dynamic_cast<juce::AudioParameterFloat*>(parameter))
        {
            // No right-click editing here; modules can add InputFloat next to sliders

            const auto&        range = pf->range;
            const float        span = range.end - range.start;
            const juce::String id = idOrName.toLowerCase();

            float step = span / 200.0f; // default ~0.5% of range
            if (span <= 1.0f)
                step = 0.01f;
            // Custom: fine tune for sequencer steps
            if (id.contains("step"))
            {
                step = 0.05f;
            }
            if (id.contains("hz") || id.contains("freq") || id.contains("cutoff") ||
                id.contains("rate"))
            {
                const float v = std::max(1.0f, std::abs(displayedValue));
                step = std::max(1.0f, std::pow(10.0f, std::floor(std::log10(v)) - 1.0f));
            }
            else if (id.contains("ms") || id.contains("time"))
            {
                const float v = std::max(1.0f, std::abs(displayedValue));
                step = std::max(0.1f, std::pow(10.0f, std::floor(std::log10(v)) - 1.0f));
            }
            else if (id.contains("db") || id.contains("gain"))
            {
                step = 0.5f;
            }
            else if (
                id.contains("mix") || id.contains("depth") || id.contains("amount") ||
                id.contains("resonance") || id.contains("q") || id.contains("size") ||
                id.contains("damp") || id.contains("pan") || id.contains("threshold"))
            {
                step = 0.01f;
            }

            float newVal = pf->get() + (wheel > 0 ? step : -step);
            newVal = juce::jlimit(range.start, range.end, newVal);
            *pf = newVal;
        }
        else if (auto* pc = dynamic_cast<juce::AudioParameterChoice*>(parameter))
        {
            int idx = pc->getIndex();
            idx += (ImGui::GetIO().MouseWheel > 0 ? 1 : -1);
            idx = juce::jlimit(0, pc->choices.size() - 1, idx);
            *pc = idx;
        }
        else if (auto* pi = dynamic_cast<juce::AudioParameterInt*>(parameter))
        {
            int         currentVal = pi->get();
            int         newVal = currentVal + (wheel > 0 ? 1 : -1);
            const auto& range = pi->getNormalisableRange();
            newVal = juce::jlimit((int)range.start, (int)range.end, newVal);
            *pi = newVal;
        }
        else if (auto* pb = dynamic_cast<juce::AudioParameterBool*>(parameter))
        {
            // Optional: toggle on strong scroll
            juce::ignoreUnused(pb);
        }
    }

#endif

public:
    //==============================================================================
    // Helper function to convert bus index and channel-in-bus to absolute channel index
    //==============================================================================
    int getChannelIndexInProcessBlockBuffer(bool isInput, int busIndex, int channelIndexInBus) const
    {
        int absoluteChannel = channelIndexInBus;
        if (busIndex > 0)
        {
            int       sum = 0;
            const int numBuses = getBusCount(isInput);
            for (int b = 0; b < numBuses && b < busIndex; ++b)
                sum += getChannelCountOfBus(isInput, b);
            absoluteChannel = sum + channelIndexInBus;
        }
        return absoluteChannel;
    }

    //==============================================================================
    // Provide default implementations for the pure virtuals to reduce boilerplate
    // in concrete module classes.
    //==============================================================================
    const juce::String          getName() const override { return "Module"; }
    bool                        acceptsMidi() const override { return false; }
    bool                        producesMidi() const override { return false; }
    double                      getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool                        hasEditor() const override { return false; }
    int                         getNumPrograms() override { return 1; }
    int                         getCurrentProgram() override { return 0; }
    void                        setCurrentProgram(int) override {}
    const juce::String          getProgramName(int) override { return {}; }
    void                        changeProgramName(int, const juce::String&) override {}
    void                        getStateInformation(juce::MemoryBlock&) override {}
    void                        setStateInformation(const void*, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModuleProcessor)

protected:
    ModularSynthProcessor* parentSynth{nullptr};
    juce::uint32           storedLogicalId{0};
    juce::uint32           storedSecondaryLogicalId{0};
};