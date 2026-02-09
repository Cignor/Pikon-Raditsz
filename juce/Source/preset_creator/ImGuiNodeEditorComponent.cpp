#include "ImGuiNodeEditorComponent.h"
#include "PinDatabase.h"
#include "SavePresetJob.h"
#include "NotificationManager.h"
#include "PresetValidator.h"
#include "PresetAutoHealer.h"
#include "../utils/VersionInfo.h"
#include "../utils/CudaDeviceCountCache.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <backends/imgui_impl_opengl2.h>
#include <cmath>
#include <cstring>
#include <juce_core/juce_core.h>
#if JUCE_WINDOWS && WITH_CUDA_SUPPORT
#include <windows.h>
#include <excpt.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <optional>
#include "theme/ThemeManager.h"
#include "../audio/modules/ChordArpModuleProcessor.h"
#include "PatchGenerator.h"

namespace
{
using collider::KeyChord;

[[nodiscard]] bool chordsEqual(const KeyChord& a, const KeyChord& b) noexcept
{
    return a.key == b.key && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt &&
           a.superKey == b.superKey;
}

[[nodiscard]] juce::String contextDisplayName(const juce::Identifier& contextId)
{
    if (contextId == collider::ShortcutManager::getGlobalContextIdentifier())
        return "Global";
    if (contextId == ImGuiNodeEditorComponent::nodeEditorContextId)
        return "Node Editor";
    return contextId.toString();
}
} // namespace

#if JUCE_DEBUG
namespace
{
struct ImGuiStackBalanceChecker
{
    ImGuiContext* ctx{ImGui::GetCurrentContext()};
    ImGuiWindow*  window{ctx ? ctx->CurrentWindow : nullptr};
    float         indent{window ? window->DC.Indent.x : 0.0f};
    float         groupOffset{window ? window->DC.GroupOffset.x : 0.0f};
    float         columnsOffset{window ? window->DC.ColumnsOffset.x : 0.0f};

    void validate(const juce::String& label)
    {
        if (ctx == nullptr)
            return;

        if (window != nullptr)
        {
            constexpr float epsilon = 1.0e-4f;
            auto            approxEqual = [](float a, float b, float eps) noexcept {
                return std::abs(a - b) <= eps;
            };

            if (!approxEqual(window->DC.Indent.x, indent, epsilon) ||
                !approxEqual(window->DC.GroupOffset.x, groupOffset, epsilon) ||
                !approxEqual(window->DC.ColumnsOffset.x, columnsOffset, epsilon))
            {
                juce::Logger::writeToLog(
                    "[ImGui][IndentLeak] " + label + " indent=" +
                    juce::String(window->DC.Indent.x) + " expected=" + juce::String(indent));
                jassertfalse;
                window->DC.Indent.x = indent;
                window->DC.GroupOffset.x = groupOffset;
                window->DC.ColumnsOffset.x = columnsOffset;
            }
        }
    }
};
} // namespace
#else
struct ImGuiStackBalanceChecker
{
    ImGuiStackBalanceChecker() = default;
    void validate(const juce::String&) const {}
};
#endif

// Lightweight theme change toast state
static double       s_themeToastEndTime = 0.0;
static juce::String s_themeToastText;

// ============================================================================
// Global GPU/CPU Settings (default: GPU enabled for best performance)
// ============================================================================
bool ImGuiNodeEditorComponent::s_globalGpuEnabled = true;
#include "../audio/graph/ModularSynthProcessor.h"
#include "../audio/modules/ModuleProcessor.h"
#include "../audio/modules/RerouteModuleProcessor.h"
#include "../audio/modules/AudioInputModuleProcessor.h"
#include "../audio/modules/AttenuverterModuleProcessor.h"
#include "../audio/modules/MapRangeModuleProcessor.h"
#include "../audio/modules/RandomModuleProcessor.h"
#include "../audio/modules/ValueModuleProcessor.h"
#include "../audio/modules/SampleLoaderModuleProcessor.h"
#include "../audio/modules/MIDIPlayerModuleProcessor.h"
#include "../audio/modules/MIDICVModuleProcessor.h"
#include "../audio/modules/PolyVCOModuleProcessor.h"
#include "../audio/modules/TrackMixerModuleProcessor.h"
#include "../audio/modules/MathModuleProcessor.h"
#include "../audio/modules/StepSequencerModuleProcessor.h"
#include "../audio/modules/MultiSequencerModuleProcessor.h"
#include "../audio/modules/StrokeSequencerModuleProcessor.h"
#include "../audio/modules/AnimationModuleProcessor.h"
#include "../audio/modules/TempoClockModuleProcessor.h"
#ifndef AUDIO_ONLY_BUILD
#include "../audio/modules/WebcamLoaderModule.h"
#include "../audio/modules/VideoFileLoaderModule.h"
#include "../audio/modules/MovementDetectorModule.h"
#include "../audio/modules/PoseEstimatorModule.h"
#include "../audio/modules/ColorTrackerModule.h"
#include "../audio/modules/ContourDetectorModule.h"
#include "../audio/modules/ObjectDetectorModule.h"
#include "../audio/modules/HandTrackerModule.h"
#include "../audio/modules/FaceTrackerModule.h"
#include "../audio/modules/VideoFXModule.h"
#include "../audio/modules/VideoCompositorModule.h"
#include "../audio/modules/VideoDrawImpactModuleProcessor.h"
#include "../audio/modules/CropVideoModule.h"
#include "../audio/modules/NdiReceiverModule.h"
#include "../audio/modules/YouTubeSourceModule.h"
#endif
#include "../audio/modules/MapRangeModuleProcessor.h"
#include "../audio/modules/LagProcessorModuleProcessor.h"
#include "../audio/modules/DeCrackleModuleProcessor.h"
#include "../audio/modules/GraphicEQModuleProcessor.h"
#include "../audio/modules/FrequencyGraphModuleProcessor.h"
#include "../audio/modules/ChorusModuleProcessor.h"
#include "../audio/modules/PhaserModuleProcessor.h"
#include "../audio/modules/CompressorModuleProcessor.h"
#include "../audio/modules/RecordModuleProcessor.h"
#include "../audio/modules/CommentModuleProcessor.h"
#include "../audio/modules/LimiterModuleProcessor.h"
#include "../audio/modules/GateModuleProcessor.h"
#include "../audio/modules/DriveModuleProcessor.h"
#include "../audio/modules/VstHostModuleProcessor.h"
// #include "../audio/modules/SnapshotSequencerModuleProcessor.h"  // Commented out - causing build
// errors
#include "../audio/modules/MIDICVModuleProcessor.h"
#include "../audio/modules/ScopeModuleProcessor.h"
#include "../audio/modules/MetaModuleProcessor.h"
#include "../audio/modules/InletModuleProcessor.h"
#include "../audio/modules/OutletModuleProcessor.h"
#include "PresetCreatorApplication.h"
#include "PresetCreatorComponent.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <imgui_impl_juce/imgui_impl_juce.h>
#include <backends/imgui_impl_opengl2.h>
#include <juce_opengl/juce_opengl.h>

#if JUCE_DEBUG
static int          gImNodesNodeDepth = 0;
static int          gImNodesInputDepth = 0;
static int          gImNodesOutputDepth = 0;
static juce::String gLastRenderedNodeLabel;

struct ImNodesDepthSnapshot
{
    int          node;
    int          input;
    int          output;
    juce::String label;

    ImNodesDepthSnapshot(const juce::String& lbl)
        : node(gImNodesNodeDepth), input(gImNodesInputDepth), output(gImNodesOutputDepth),
          label(lbl)
    {
    }

    ~ImNodesDepthSnapshot()
    {
        if (gImNodesNodeDepth != node || gImNodesInputDepth != input ||
            gImNodesOutputDepth != output)
        {
            juce::Logger::writeToLog(
                "[ImNodes][DepthLeak] " + label + " node=" + juce::String(gImNodesNodeDepth) +
                " (expected " + juce::String(node) + ")" +
                " input=" + juce::String(gImNodesInputDepth) + " (expected " + juce::String(input) +
                ")" + " output=" + juce::String(gImNodesOutputDepth) + " (expected " +
                juce::String(output) + ")");
            jassertfalse;

            // Reset to avoid cascading logs
            gImNodesNodeDepth = node;
            gImNodesInputDepth = input;
            gImNodesOutputDepth = output;
        }
    }
};
#endif

#define NODE_DEBUG 0

// --- Module Descriptions for Tooltips ---
static const char* toString(PinDataType t)
{
    switch (t)
    {
    case PinDataType::Audio:
        return "Audio";
    case PinDataType::CV:
        return "CV";
    case PinDataType::Gate:
        return "Gate";
    case PinDataType::Raw:
        return "Raw";
    case PinDataType::Video:
        return "Video";
    default:
        return "Unknown";
    }
}

#define LOG_LINK(msg)                                                                              \
    do                                                                                             \
    {                                                                                              \
        if (NODE_DEBUG)                                                                            \
            juce::Logger::writeToLog("[LINK] " + juce::String(msg));                               \
    } while (0)

struct Range
{
    float min;
    float max;
};

// Forward declarations
class ModularSynthProcessor;
class RandomModuleProcessor;
class ValueModuleProcessor;
class StepSequencerModuleProcessor;
class MapRangeModuleProcessor;

// Helper methods for MapRange configuration
ImGuiNodeEditorComponent::Range getSourceRange(
    const ImGuiNodeEditorComponent::PinID& srcPin,
    ModularSynthProcessor*                 synth)
{
    if (synth == nullptr)
        return {0.0f, 1.0f};

    auto* module = synth->getModuleForLogical(srcPin.logicalId);
    if (auto* random = dynamic_cast<RandomModuleProcessor*>(module))
    {
        auto& ap = random->getAPVTS();
        float min = 0.0f, max = 1.0f;
        if (auto* minParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("min")))
            min = minParam->get();
        if (auto* maxParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("max")))
            max = maxParam->get();
        return {min, max};
    }
    else if (auto* value = dynamic_cast<ValueModuleProcessor*>(module))
    {
        auto& ap = value->getAPVTS();
        float min = 0.0f, max = 1.0f;
        if (auto* minParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("min")))
            min = minParam->get();
        if (auto* maxParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("max")))
            max = maxParam->get();
        return {min, max};
    }
    else if (auto* stepSeq = dynamic_cast<StepSequencerModuleProcessor*>(module))
    {
        // StepSequencer outputs CV range
        return {0.0f, 1.0f};
    }
    // Fallback: estimate from source's lastOutputValues
    // TODO: implement fallback estimation
    return {0.0f, 1.0f};
}

void configureMapRangeFor(
    PinDataType                     srcType,
    PinDataType                     dstType,
    MapRangeModuleProcessor&        m,
    ImGuiNodeEditorComponent::Range inRange)
{
    auto& ap = m.getAPVTS();

    // Set input range
    if (auto* inMinParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("inMin")))
        *inMinParam = inRange.min;
    if (auto* inMaxParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("inMax")))
        *inMaxParam = inRange.max;

    // Set output range based on destination type
    if (dstType == PinDataType::Audio)
    {
        if (auto* outMinParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMin")))
            *outMinParam = -1.0f;
        if (auto* outMaxParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMax")))
            *outMaxParam = 1.0f;
    }
    else // CV or Gate
    {
        if (auto* outMinParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMin")))
            *outMinParam = 0.0f;
        if (auto* outMaxParam = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMax")))
            *outMaxParam = 1.0f;
    }
}

ImGuiNodeEditorComponent::ImGuiNodeEditorComponent(juce::AudioDeviceManager& dm) : deviceManager(dm)
{
    juce::Logger::writeToLog("ImGuiNodeEditorComponent constructor starting...");

    // --- THIS WILL BE THE SMOKING GUN ---
    juce::Logger::writeToLog("About to populate pin database...");
    populatePinDatabase(); // Initialize the pin database for color coding
    populateDragInsertSuggestions();
    juce::Logger::writeToLog("Pin database populated.");

    glContext.setRenderer(this);
    glContext.setContinuousRepainting(true);
    glContext.setComponentPaintingEnabled(false);
    glContext.attachTo(*this);
    setWantsKeyboardFocus(true);
    registerShortcuts();

    // Wire Theme Editor to use framebuffer-based eyedropper
    themeEditor.setStartPicker([this](std::function<void(ImU32)> onPicked) {
        this->startColorPicking(std::move(onPicked));
    });

    // Initialize browser paths (load from saved settings or use defaults)
    // Use currentExecutableFile and default to exe/presets, exe/samples, exe/midi
    // Cache exe directory to avoid slow path resolution on every button click
    try
    {
        auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        if (exeFile.getFullPathName().isNotEmpty())
        {
            m_cachedExeDir = exeFile.getParentDirectory();
        }
    }
    catch (...)
    {
        juce::Logger::writeToLog("[UI] Failed to resolve executable path, using fallback");
        m_cachedExeDir = juce::File();
    }

    juce::File exeDir = m_cachedExeDir;

    if (auto* props = PresetCreatorApplication::getApp().getProperties())
    {
        // Default paths: exe/presets, exe/samples, exe/midi
        juce::File defaultPresetPath = exeDir.getChildFile("presets");
        juce::File defaultSamplePath = exeDir.getChildFile("samples");
        juce::File defaultMidiPath = exeDir.getChildFile("midi");

        // Load saved paths
        juce::String savedPresetPath = props->getValue("presetScanPath", "");
        juce::String savedSamplePath = props->getValue("sampleScanPath", "");
        juce::String savedMidiPath = props->getValue("midiScanPath", "");

        // Migration: Check if saved paths are using old defaults and update them
        // Check for old project directory pattern (H:\0000_CODE\01_collider_pyo) or old default
        // locations
        bool presetNeedsMigration = savedPresetPath.isEmpty();
        bool sampleNeedsMigration = savedSamplePath.isEmpty();
        bool midiNeedsMigration = savedMidiPath.isEmpty();

        if (!presetNeedsMigration && exeDir.getFullPathName().isNotEmpty())
        {
            juce::String presetPathLower = savedPresetPath.toLowerCase();
            juce::File   savedPresetFile(savedPresetPath);
            juce::File   oldDefaultPreset = exeDir.getChildFile("Presets");

            // Migrate if: pointing to old project directory, old default location, or empty
            if (presetPathLower.contains("0000_code") ||
                presetPathLower.contains("01_collider_pyo") ||
                savedPresetFile == oldDefaultPreset || !savedPresetFile.exists())
            {
                presetNeedsMigration = true;
            }
        }

        if (!sampleNeedsMigration && exeDir.getFullPathName().isNotEmpty())
        {
            juce::String samplePathLower = savedSamplePath.toLowerCase();
            juce::File   savedSampleFile(savedSamplePath);
            juce::File   oldDefaultSample = exeDir.getChildFile("Samples");

            // Migrate if: pointing to old project directory, old default location, or empty
            if (samplePathLower.contains("0000_code") ||
                samplePathLower.contains("01_collider_pyo") ||
                savedSampleFile == oldDefaultSample || !savedSampleFile.exists())
            {
                sampleNeedsMigration = true;
            }
        }

        if (!midiNeedsMigration && exeDir.getFullPathName().isNotEmpty())
        {
            juce::String midiPathLower = savedMidiPath.toLowerCase();
            juce::File   savedMidiFile(savedMidiPath);

            // Migrate if: pointing to old project directory, old audio/MIDI structure, or empty
            if (midiPathLower.contains("0000_code") || midiPathLower.contains("01_collider_pyo") ||
                midiPathLower.contains("/audio/midi") ||
                (midiPathLower.contains("midi") && midiPathLower.contains("audio")) ||
                !savedMidiFile.exists())
            {
                midiNeedsMigration = true;
            }
        }

        // Apply migration if needed
        if (presetNeedsMigration)
        {
            m_presetScanPath = defaultPresetPath;
            props->setValue("presetScanPath", defaultPresetPath.getFullPathName());
            juce::Logger::writeToLog(
                "[UI] Migrated preset path to: " + defaultPresetPath.getFullPathName());
        }
        else
        {
            m_presetScanPath = juce::File(savedPresetPath);
        }

        if (sampleNeedsMigration)
        {
            m_sampleScanPath = defaultSamplePath;
            props->setValue("sampleScanPath", defaultSamplePath.getFullPathName());
            juce::Logger::writeToLog(
                "[UI] Migrated sample path to: " + defaultSamplePath.getFullPathName());
        }
        else
        {
            m_sampleScanPath = juce::File(savedSamplePath);
        }

        if (midiNeedsMigration)
        {
            m_midiScanPath = defaultMidiPath;
            props->setValue("midiScanPath", defaultMidiPath.getFullPathName());
            juce::Logger::writeToLog(
                "[UI] Migrated MIDI path to: " + defaultMidiPath.getFullPathName());
        }
        else
        {
            m_midiScanPath = juce::File(savedMidiPath);
        }
    }

    // Create these directories if they don't already exist
    if (m_presetScanPath.getFullPathName().isNotEmpty() && !m_presetScanPath.exists())
        m_presetScanPath.createDirectory();
    if (m_sampleScanPath.getFullPathName().isNotEmpty() && !m_sampleScanPath.exists())
        m_sampleScanPath.createDirectory();
    if (m_midiScanPath.getFullPathName().isNotEmpty() && !m_midiScanPath.exists())
        m_midiScanPath.createDirectory();

    // --- VST BROWSER PATH INITIALIZATION ---
    if (auto* props = PresetCreatorApplication::getApp().getProperties())
    {
        // Default path: exe/vst (lowercase, matching other browsers)
        juce::File defaultVstPath = exeDir.getChildFile("vst");

        // Load saved path
        juce::String savedVstPath = props->getValue("vstScanPath", "");

        // Migration: Check if saved path is using old defaults
        bool vstNeedsMigration = savedVstPath.isEmpty();

        if (!vstNeedsMigration && exeDir.getFullPathName().isNotEmpty())
        {
            juce::String vstPathLower = savedVstPath.toLowerCase();
            juce::File   savedVstFile(savedVstPath);
            juce::File   oldDefaultVst = exeDir.getChildFile("VST"); // Old capital VST

            // Migrate if: pointing to old project directory, old default location (capital VST), or
            // empty
            if (vstPathLower.contains("0000_code") || vstPathLower.contains("01_collider_pyo") ||
                savedVstFile == oldDefaultVst || !savedVstFile.exists())
            {
                vstNeedsMigration = true;
            }
        }

        // Apply migration if needed
        if (vstNeedsMigration)
        {
            m_vstScanPath = defaultVstPath;
            props->setValue("vstScanPath", defaultVstPath.getFullPathName());
            juce::Logger::writeToLog(
                "[UI] Migrated VST path to: " + defaultVstPath.getFullPathName());
        }
        else
        {
            m_vstScanPath = juce::File(savedVstPath);
        }
    }

    // Create VST directory if it doesn't exist
    if (m_vstScanPath.getFullPathName().isNotEmpty() && !m_vstScanPath.exists())
        m_vstScanPath.createDirectory();

    juce::Logger::writeToLog("[UI] Preset path set to: " + m_presetScanPath.getFullPathName());
    juce::Logger::writeToLog("[UI] Sample path set to: " + m_sampleScanPath.getFullPathName());
    juce::Logger::writeToLog("[UI] MIDI path set to: " + m_midiScanPath.getFullPathName());
    juce::Logger::writeToLog("[UI] VST path set to: " + m_vstScanPath.getFullPathName());

    // Build initial VST tree from existing KnownPluginList if plugins are already loaded
    // (This happens if known_plugins.xml was loaded on startup)
    if (m_vstScanPath.getFullPathName().isNotEmpty() && m_vstScanPath.exists())
    {
        auto& app = PresetCreatorApplication::getApp();
        auto& knownPluginList = app.getKnownPluginList();
        if (knownPluginList.getNumTypes() > 0)
        {
            // Build tree from existing plugin list without re-scanning
            m_vstManager.buildTreeFromPluginList(m_vstScanPath, knownPluginList);
            juce::Logger::writeToLog(
                "[UI] Built VST tree from existing plugin list (" +
                juce::String(knownPluginList.getNumTypes()) + " plugins)");
        }
    }
    // --- END OF MIDI INITIALIZATION ---
}

ImGuiNodeEditorComponent::~ImGuiNodeEditorComponent()
{
    unregisterShortcuts();
    glContext.detach();
}

void ImGuiNodeEditorComponent::registerShortcuts()
{
    auto registerAction = [this](
                              const juce::Identifier&   id,
                              const char*               name,
                              const char*               description,
                              const char*               category,
                              const collider::KeyChord& chord,
                              std::atomic<bool>&        flag) {
        collider::ShortcutAction action{
            id, juce::String(name), juce::String(description), juce::String(category)};
        shortcutManager.registerAction(
            action, [this, &flag]() { flag.store(true, std::memory_order_release); });
        shortcutManager.setDefaultBinding(id, nodeEditorContextId, chord);
    };

    registerAction(
        ShortcutActionIds::fileSave,
        "Save Preset",
        "Save the current patch to its file.",
        "File",
        {ImGuiKey_S, true, false, false, false},
        shortcutFileSaveRequested);

    registerAction(
        ShortcutActionIds::fileSaveAs,
        "Save Preset As...",
        "Save the current patch to a new file.",
        "File",
        {ImGuiKey_S, true, false, true, false},
        shortcutFileSaveAsRequested);

    registerAction(
        ShortcutActionIds::fileNewCanvas,
        "New Canvas",
        "Start with a clean canvas, clearing any loaded preset.",
        "File",
        {ImGuiKey_N, true, true, false, false},
        shortcutNewCanvasRequested);

    registerAction(
        ShortcutActionIds::fileOpen,
        "Load Preset",
        "Open a preset from disk.",
        "File",
        {ImGuiKey_O, true, false, false, false},
        shortcutFileOpenRequested);

    registerAction(
        ShortcutActionIds::fileRandomizePatch,
        "Randomize Patch",
        "Randomize the entire patch.",
        "File",
        {ImGuiKey_P, true, false, false, false},
        shortcutRandomizePatchRequested);

    registerAction(
        ShortcutActionIds::fileRandomizeConnections,
        "Randomize Connections",
        "Randomize node connections.",
        "File",
        {ImGuiKey_M, true, false, false, false},
        shortcutRandomizeConnectionsRequested);

    registerAction(
        ShortcutActionIds::fileBeautifyLayout,
        "Beautify Layout",
        "Automatically tidy the node layout.",
        "File",
        {ImGuiKey_B, true, false, false, false},
        shortcutBeautifyLayoutRequested);

    registerAction(
        ShortcutActionIds::editRecordOutput,
        "Record Output",
        "Record the main output to a file.",
        "Edit",
        {ImGuiKey_R, true, false, false, false},
        shortcutRecordOutputRequested);

    registerAction(
        ShortcutActionIds::editResetNode,
        "Reset Node",
        "Reset selected nodes to their default parameter values.",
        "Edit",
        {ImGuiKey_R, false, false, true, false}, // Alt+R
        shortcutResetNodeRequested);

    registerAction(
        ShortcutActionIds::editSelectAll,
        "Select All",
        "Select every node in the graph.",
        "Edit",
        {ImGuiKey_A, true, false, false, false},
        shortcutSelectAllRequested);

    registerAction(
        ShortcutActionIds::editMuteSelection,
        "Toggle Mute",
        "Mute or bypass the selected nodes.",
        "Edit",
        {ImGuiKey_M, false, false, false, false},
        shortcutMuteSelectionRequested);

    registerAction(
        ShortcutActionIds::editConnectOutput,
        "Connect to Output",
        "Wire the selected node to the main output.",
        "Edit",
        {ImGuiKey_O, false, false, false, false},
        shortcutConnectOutputRequested);

    registerAction(
        ShortcutActionIds::editDisconnectSelection,
        "Disconnect Selection",
        "Remove all connections from selected nodes.",
        "Edit",
        {ImGuiKey_D, false, false, true, false},
        shortcutDisconnectRequested);

    registerAction(
        ShortcutActionIds::editDuplicate,
        "Duplicate Selection",
        "Duplicate selected nodes.",
        "Edit",
        {ImGuiKey_D, true, false, false, false},
        shortcutDuplicateRequested);

    registerAction(
        ShortcutActionIds::editDuplicateWithRouting,
        "Duplicate Selection (With Routing)",
        "Duplicate selected nodes and replicate their connections.",
        "Edit",
        {ImGuiKey_D, false, true, false, false},
        shortcutDuplicateWithRoutingRequested);

    registerAction(
        ShortcutActionIds::editDelete,
        "Delete Selection",
        "Delete selected nodes or links.",
        "Edit",
        {ImGuiKey_Delete, false, false, false, false},
        shortcutDeleteRequested);

    registerAction(
        ShortcutActionIds::editBypassDelete,
        "Bypass Delete",
        "Delete selected nodes while preserving signal flow.",
        "Edit",
        {ImGuiKey_Delete, false, true, false, false},
        shortcutBypassDeleteRequested);

    registerAction(
        ShortcutActionIds::viewFrameSelection,
        "Frame Selection",
        "Frame the currently selected nodes.",
        "View",
        {ImGuiKey_F, false, false, false, false},
        shortcutFrameSelectionRequested);

    registerAction(
        ShortcutActionIds::viewFrameAll,
        "Frame All",
        "Frame the entire graph.",
        "View",
        {ImGuiKey_Home, false, false, false, false},
        shortcutFrameAllRequested);

    registerAction(
        ShortcutActionIds::viewResetOrigin,
        "Reset View Origin",
        "Reset the editor panning to the origin.",
        "View",
        {ImGuiKey_Home, true, false, false, false},
        shortcutResetOriginRequested);

    registerAction(
        ShortcutActionIds::viewToggleMinimap,
        "Toggle Minimap Zoom",
        "Temporarily enlarge the minimap.",
        "View",
        {ImGuiKey_Comma, false, false, false, false},
        shortcutToggleMinimapRequested);

    // This action now opens the new Help Manager to the Shortcuts tab
    // If a node is selected, opens to that node's dictionary entry instead
    shortcutManager.registerAction(
        {ShortcutActionIds::viewToggleShortcutsWindow,
         "Help Manager",
         "Show the Help Manager window.",
         "Help"},
        [this]() {
            // Check if a node is selected
            int numSelected = ImNodes::NumSelectedNodes();
            if (numSelected > 0)
            {
                // Get the first selected node
                std::vector<int> selectedNodeIds(numSelected);
                ImNodes::GetSelectedNodes(selectedNodeIds.data());
                if (!selectedNodeIds.empty())
                {
                    juce::uint32 logicalId = (juce::uint32)selectedNodeIds[0];
                    if (synth != nullptr)
                    {
                        juce::String moduleType = synth->getModuleTypeForLogical(logicalId);

                        if (moduleType.isNotEmpty())
                        {
                            // Open Help Manager to this node's dictionary entry
                            m_helpManager.openToNode(moduleType);
                            return;
                        }
                    }
                }
            }

            // Default behavior: open to Shortcuts tab
            m_helpManager.open();
            m_helpManager.setActiveTab(0); // 0 = Shortcuts tab
        });
    // F1 now shows About dialog instead of Help Manager
    // Help Manager is accessible via Settings menu or Help menu
    // shortcutManager.setDefaultBinding(ShortcutActionIds::viewToggleShortcutsWindow,
    // nodeEditorContextId, { ImGuiKey_F1, false, false, false, false });

    registerAction(
        ShortcutActionIds::historyUndo,
        "Undo",
        "Revert the last action.",
        "History",
        {ImGuiKey_Z, true, false, false, false},
        shortcutUndoRequested);

    registerAction(
        ShortcutActionIds::historyRedo,
        "Redo",
        "Redo the last undone action.",
        "History",
        {ImGuiKey_Y, true, false, false, false},
        shortcutRedoRequested);

    registerAction(
        ShortcutActionIds::debugToggleOverlay,
        "Toggle Debug Menu",
        "Show or hide the diagnostics window.",
        "Debug",
        {ImGuiKey_D, true, true, false, false},
        shortcutToggleDebugRequested);

    registerAction(
        ShortcutActionIds::graphInsertMixer,
        "Insert Mixer",
        "Insert a mixer after the selected node.",
        "Graph",
        {ImGuiKey_None,
         false,
         false,
         false,
         false}, // No default shortcut - user can assign if needed
        shortcutInsertMixerRequested);

    registerAction(
        ShortcutActionIds::graphConnectSelectedToTrackMixer,
        "Connect Selected to Track Mixer",
        "Connect selected nodes to a new Track Mixer with automatic routing.",
        "Graph",
        {ImGuiKey_T, true, false, false, false}, // Ctrl+T default
        shortcutConnectSelectedToTrackMixerRequested);

    registerAction(
        ShortcutActionIds::graphConnectSelectedToRecorder,
        "Connect Selected to Recorder",
        "Connect selected nodes to a new Recorder for multi-phase recording.",
        "Graph",
        {ImGuiKey_R, true, true, false, false}, // Ctrl+Shift+R
        shortcutConnectSelectedToRecorderRequested);

    registerAction(
        ShortcutActionIds::graphShowInsertPopup,
        "Open Insert Node Popup",
        "Open the insert node popup for the selected node.",
        "Graph",
        {ImGuiKey_I, true, false, false, false},
        shortcutShowInsertPopupRequested);

    registerAction(
        ShortcutActionIds::graphInsertOnLink,
        "Insert Node On Link",
        "Insert a node on the hovered link.",
        "Graph",
        {ImGuiKey_I, false, false, false, false},
        shortcutInsertOnLinkRequested);

    registerAction(
        ShortcutActionIds::graphChainSequential,
        "Chain Selection (Stereo)",
        "Connect selected nodes sequentially using stereo outputs.",
        "Graph",
        {ImGuiKey_C, false, false, false, false},
        shortcutChainSequentialRequested);

    registerAction(
        ShortcutActionIds::graphChainAudio,
        "Chain Audio Pins",
        "Connect matching audio pins between selected nodes.",
        "Graph",
        {ImGuiKey_G, false, false, false, false},
        shortcutChainAudioRequested);

    registerAction(
        ShortcutActionIds::graphChainCv,
        "Chain CV Pins",
        "Connect matching CV pins between selected nodes.",
        "Graph",
        {ImGuiKey_B, false, false, false, false},
        shortcutChainCvRequested);

    registerAction(
        ShortcutActionIds::graphChainGate,
        "Chain Gate Pins",
        "Connect matching gate pins between selected nodes.",
        "Graph",
        {ImGuiKey_Y, false, false, false, false},
        shortcutChainGateRequested);

    registerAction(
        ShortcutActionIds::graphChainRaw,
        "Chain Raw Pins",
        "Connect matching raw pins between selected nodes.",
        "Graph",
        {ImGuiKey_R, false, false, false, false},
        shortcutChainRawRequested);

    registerAction(
        ShortcutActionIds::graphChainVideo,
        "Chain Video Pins",
        "Connect matching video pins between selected nodes.",
        "Graph",
        {ImGuiKey_V, false, false, false, false},
        shortcutChainVideoRequested);
}

void ImGuiNodeEditorComponent::unregisterShortcuts()
{
    shortcutManager.unregisterAction(ShortcutActionIds::graphInsertOnLink);
    shortcutManager.unregisterAction(ShortcutActionIds::graphShowInsertPopup);
    shortcutManager.unregisterAction(ShortcutActionIds::graphConnectSelectedToTrackMixer);
    shortcutManager.unregisterAction(ShortcutActionIds::graphConnectSelectedToRecorder);
    shortcutManager.unregisterAction(ShortcutActionIds::graphInsertMixer);
    shortcutManager.unregisterAction(ShortcutActionIds::debugToggleOverlay);
    shortcutManager.unregisterAction(ShortcutActionIds::historyRedo);
    shortcutManager.unregisterAction(ShortcutActionIds::historyUndo);
    shortcutManager.unregisterAction(ShortcutActionIds::graphChainVideo);
    shortcutManager.unregisterAction(ShortcutActionIds::graphChainRaw);
    shortcutManager.unregisterAction(ShortcutActionIds::graphChainGate);
    shortcutManager.unregisterAction(ShortcutActionIds::graphChainCv);
    shortcutManager.unregisterAction(ShortcutActionIds::graphChainAudio);
    shortcutManager.unregisterAction(ShortcutActionIds::graphChainSequential);
    shortcutManager.unregisterAction(ShortcutActionIds::viewToggleShortcutsWindow);
    shortcutManager.unregisterAction(ShortcutActionIds::viewToggleMinimap);
    shortcutManager.unregisterAction(ShortcutActionIds::viewResetOrigin);
    shortcutManager.unregisterAction(ShortcutActionIds::viewFrameAll);
    shortcutManager.unregisterAction(ShortcutActionIds::viewFrameSelection);
    shortcutManager.unregisterAction(ShortcutActionIds::editBypassDelete);
    shortcutManager.unregisterAction(ShortcutActionIds::editDelete);
    shortcutManager.unregisterAction(ShortcutActionIds::editDuplicateWithRouting);
    shortcutManager.unregisterAction(ShortcutActionIds::editDuplicate);
    shortcutManager.unregisterAction(ShortcutActionIds::editDisconnectSelection);
    shortcutManager.unregisterAction(ShortcutActionIds::editConnectOutput);
    shortcutManager.unregisterAction(ShortcutActionIds::editMuteSelection);
    shortcutManager.unregisterAction(ShortcutActionIds::editSelectAll);
    shortcutManager.unregisterAction(ShortcutActionIds::editRecordOutput);
    shortcutManager.unregisterAction(ShortcutActionIds::editResetNode);
    shortcutManager.unregisterAction(ShortcutActionIds::fileBeautifyLayout);
    shortcutManager.unregisterAction(ShortcutActionIds::fileRandomizeConnections);
    shortcutManager.unregisterAction(ShortcutActionIds::fileRandomizePatch);
    shortcutManager.unregisterAction(ShortcutActionIds::fileOpen);
    shortcutManager.unregisterAction(ShortcutActionIds::fileNewCanvas);
    shortcutManager.unregisterAction(ShortcutActionIds::fileSaveAs);
    shortcutManager.unregisterAction(ShortcutActionIds::fileSave);
}

void ImGuiNodeEditorComponent::paint(juce::Graphics& g) { juce::ignoreUnused(g); }

void ImGuiNodeEditorComponent::resized()
{
    juce::Logger::writeToLog(
        "resized: " + juce::String(getWidth()) + "x" + juce::String(getHeight()));
}
// Input handled by imgui_juce backend
void ImGuiNodeEditorComponent::newOpenGLContextCreated()
{
    juce::Logger::writeToLog("ImGuiNodeEditor: newOpenGLContextCreated()");
    // Create ImGui context
    imguiContext = ImGui::CreateContext();
    imguiIO = &ImGui::GetIO();

    // Try to load user's saved theme preference, otherwise use default
    if (!ThemeManager::getInstance().loadUserThemePreference())
    {
        // No preference found or failed to load, apply default theme
        ThemeManager::getInstance().applyTheme();
    }
    // If preference was loaded successfully, loadUserThemePreference() already called applyTheme()

    // imgui_juce backend handles key mapping internally (new IO API)

    // Setup JUCE platform backend and OpenGL2 renderer backend
    ImGui_ImplJuce_Init(*this, glContext);
    ImGui_ImplOpenGL2_Init();

    // Setup imnodes
    ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
    editorContext = ImNodes::CreateContext();

    // Enable grid snapping
    ImNodes::GetStyle().GridSpacing = 64.0f;

    // Optional ergonomics: Alt = pan, Ctrl = detach link
    {
        auto& ioNodes = ImNodes::GetIO();
        auto& ioImgui = ImGui::GetIO();
        ioNodes.EmulateThreeButtonMouse.Modifier = &ioImgui.KeyAlt;
        ioNodes.LinkDetachWithModifierClick.Modifier = &ioImgui.KeyCtrl;

        // Log setup of modifiers
        juce::Logger::writeToLog(
            "ImGuiNodeEditor: Modifiers configured. Alt=Emulate3Btn, Ctrl=LinkDetach");
    }
    juce::Logger::writeToLog("ImGuiNodeEditor: ImNodes context created");
}

void ImGuiNodeEditorComponent::openGLContextClosing()
{
    juce::Logger::writeToLog("ImGuiNodeEditor: openGLContextClosing()");
    ImNodes::DestroyContext(editorContext);
    editorContext = nullptr;
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplJuce_Shutdown();
    ImGui::DestroyContext(imguiContext);
    imguiContext = nullptr;
    imguiIO = nullptr;
}

void ImGuiNodeEditorComponent::renderOpenGL()
{
    if (imguiContext == nullptr)
        return;

    ImGui::SetCurrentContext(imguiContext);

    // Clear background
    juce::OpenGLHelpers::clear(juce::Colours::darkgrey);

    // ======================================================
    // === 💡 FONT REBUILD DEFERRED EXECUTION ===============
    // ======================================================
    if (fontAtlasNeedsRebuild.exchange(false, std::memory_order_acq_rel) ||
        ThemeManager::getInstance().consumeFontReloadRequest())
    {
        rebuildFontAtlas();
    }

    // Ensure IO is valid and configured each frame (size, delta time, DPI scale, fonts)
    ImGuiIO&    io = ImGui::GetIO();
    const float scale = (float)glContext.getRenderingScale();
    io.DisplaySize = ImVec2((float)getWidth(), (float)getHeight());
    io.DisplayFramebufferScale = ImVec2(scale, scale);

    // imgui_juce will queue and apply key/mouse events; avoid manual KeysDown edits that break
    // internal asserts
    io.MouseDrawCursor = false;

    // Mouse input comes via backend listeners; avoid overriding io.MousePos here

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (lastTime <= 0.0)
        lastTime = nowMs;
    const double dtMs = nowMs - lastTime;
    lastTime = nowMs;
    io.DeltaTime = (dtMs > 0.0 ? (float)(dtMs / 1000.0) : 1.0f / 60.0f);

    // Zoom/pan disabled: use default font scale and editor panning

    // Start a new frame for both backends
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplJuce_NewFrame();

    ImGui::NewFrame();
    // Demo is hidden by default; toggle can be added later if needed
    renderImGui();
    themeEditor.render();         // Render theme editor if open
    m_helpManager.render();       // Render help manager if open
    voiceDownloadDialog.render(); // Render voice download dialog if open
    if (onRenderUpdateDialog)
        onRenderUpdateDialog(); // Render update dialog if open
    ImGui::Render();
    auto* dd = ImGui::GetDrawData();
    // Render via OpenGL2 backend
    ImGui_ImplOpenGL2_RenderDrawData(dd);

    // --- Eyedropper sampling after rendering (framebuffer has ImGui drawn) ---
    if (m_isPickingColor)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2   mousePos = ImGui::GetMousePos();

        // Convert to framebuffer Y
        const int fbH = (int)io.DisplaySize.y;
        const int px = juce::jlimit(0, (int)io.DisplaySize.x - 1, (int)mousePos.x);
        const int py = juce::jlimit(0, fbH - 1, fbH - (int)mousePos.y - 1);

        unsigned char rgba[4]{0, 0, 0, 255};
        juce::gl::glReadPixels(px, py, 1, 1, juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE, rgba);
        ImU32 picked = IM_COL32(rgba[0], rgba[1], rgba[2], 255);

        // Draw cursor overlay
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float s = 16.0f;
        ImVec2      tl(mousePos.x + 12, mousePos.y + 12);
        ImVec2      br(tl.x + s, tl.y + s);
        fg->AddRectFilled(tl, br, picked, 3.0f);
        fg->AddRect(tl, br, IM_COL32(0, 0, 0, 255), 3.0f, 0, 1.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (m_onColorPicked)
                m_onColorPicked(picked);
            m_isPickingColor = false;
            m_onColorPicked = nullptr;
        }
        else if (
            ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_isPickingColor = false;
            m_onColorPicked = nullptr;
        }
    }
}
void ImGuiNodeEditorComponent::renderImGui()
{
    // Ensure the synth always has the creation notification hook registered
    if (synth != nullptr)
    {
        synth->setOnModuleCreated([](const juce::String& pretty) {
            NotificationManager::post(
                NotificationManager::Type::Info, "Created " + pretty + " node");
        });
    }
    static int frameCounter = 0;
    frameCounter++;

#ifndef AUDIO_ONLY_BUILD
    // Auto-create VideoViewer module if canvas is empty (only output node exists)
    // Check after a few frames to allow initialization, and only once per session
    static bool hasCheckedForDefaultVideoViewer = false;
    if (!hasCheckedForDefaultVideoViewer && synth != nullptr && frameCounter >= 10)
    {
        auto modules = synth->getModulesInfo();
        bool hasVideoViewer = false;
        for (const auto& modInfo : modules)
        {
            if (modInfo.second.toLowerCase() == "video_viewer")
            {
                hasVideoViewer = true;
                break;
            }
        }

        if (modules.empty() && !hasVideoViewer)
        {
            // Canvas is empty - create VideoViewer module by default
            auto videoViewerNodeId = synth->addModule("video_viewer", false);
            if (videoViewerNodeId.uid != 0)
            {
                auto videoViewerLogicalId = synth->getLogicalIdForNode(videoViewerNodeId);
                // Position VideoViewer directly below the output node (same X, small Y offset)
                // Output node is at (1250.0, 500.0), place VideoViewer at (1250.0, 650.0) - 150px
                // below
                pendingNodePositions[(int)videoViewerLogicalId] = ImVec2(1250.0f, 650.0f);
                synth->commitChanges();
                juce::Logger::writeToLog(
                    "[AutoCreate] Auto-created VideoViewer module (logicalId=" +
                    juce::String((int)videoViewerLogicalId) +
                    ") at default position (1250.0, 650.0)");
            }
        }
        hasCheckedForDefaultVideoViewer = true;
    }
#endif

    // --- Apply PatchGenerator Positions ---
    // If the PatchGenerator has created a new layout, apply it now.
    auto generatedPositions = PatchGenerator::getNodePositions();
    if (!generatedPositions.empty())
    {
        for (const auto& pair : generatedPositions)
        {
            // Store positions in pendingNodePositions, which will be applied during the next render
            // This is safer than directly calling SetNodeGridSpacePos which might fail if nodes
            // aren't ready
            pendingNodePositions[(int)pair.first] = ImVec2(pair.second.x, pair.second.y);
        }
        PatchGenerator::clearNodePositions();
        juce::Logger::writeToLog(
            "[ImGuiNodeEditor] Applied " + juce::String(generatedPositions.size()) +
            " generated node positions.");
    }

    // ========================= THE DEFINITIVE FIX =========================
    //
    // Rebuild the audio graph at the START of the frame if a change is pending.
    // This ensures that the synth model is in a consistent state BEFORE we try
    // to draw the UI, eliminating the "lost frame" that caused nodes to jump.
    //
    if (graphNeedsRebuild.load())
    {
        juce::Logger::writeToLog("[GraphSync] Rebuild flag is set. Committing changes now...");
        if (synth)
        {
            synth->commitChanges();
        }
        graphNeedsRebuild = false; // Reset the flag immediately after committing.

        // CRITICAL: Invalidate hover state to prevent cable inspector from accessing
        // modules that were just deleted/recreated during commitChanges()
        lastHoveredLinkId = -1;
        lastHoveredNodeId = -1;
        hoveredLinkSrcId = 0;
        hoveredLinkDstId = 0;

        juce::Logger::writeToLog("[GraphSync] Graph rebuild complete.");
    }
    // ========================== END OF FIX ==========================

    // Frame start

    // --- Stateless Frame Rendering ---
    // Clear link registries at start of each frame for fully stateless rendering.
    // Pin IDs are now generated directly via bitmasking, no maps needed.
    linkIdToAttrs.clear();
    linkToId.clear();
    nextLinkId = 1000;

    collider::ScopedShortcutContext contextGuard(shortcutManager, nodeEditorContextId);

    if (imguiIO != nullptr)
        shortcutManager.processImGuiIO(*imguiIO);

    // --- ZOOM CONTROL HANDLER (requires imnodes zoom-enabled build) ---
#if defined(IMNODES_ZOOM_ENABLED)
    if (ImNodes::GetCurrentContext())
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float    currentZoom = ImNodes::EditorContextGetZoom();
        lastZoom = currentZoom; // Cache for use in other methods

        if (io.KeyCtrl && io.MouseWheel != 0.0f)
        {
            const float zoomFactor = 1.0f + (io.MouseWheel * 0.1f);
            const float newZoom = currentZoom * zoomFactor;
            ImNodes::EditorContextSetZoom(newZoom, ImGui::GetMousePos());
            juce::Logger::writeToLog("[Zoom] New Zoom: " + juce::String(newZoom, 2) + "x");
        }
    }
#else
    lastZoom = 1.0f;
#endif
    // --- END ZOOM CONTROL HANDLER ---

    // === FIX DOUBLE CANVAS RENDERING ===
    // Make the parent window's background transparent.
    // This ensures that only the ImNodes canvas background (which
    // your theme controls) is the only one visible.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    // === END OF FIX ===

    // Basic docking-like two-panels layout
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)getWidth(), (float)getHeight()), ImGuiCond_Always);
    ImGui::Begin(
        VersionInfo::APPLICATION_NAME,
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    const float sidebarWidth = theme.layout.sidebar_width;
    const float menuBarHeight = ImGui::GetFrameHeight();
    const float padding = theme.layout.window_padding;

    // === PROBE SCOPE OVERLAY ===
    if (synth != nullptr && showProbeScope)
    {
        if (auto* scope = synth->getProbeScopeProcessor())
        {
            const float scopeWidth = theme.windows.probe_scope_width;
            const float scopeHeight = theme.windows.probe_scope_height;
            const float scopePosX = (float)getWidth() - (scopeWidth + padding);
            ImGui::SetNextWindowPos(
                ImVec2(scopePosX, menuBarHeight + padding), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(scopeWidth, scopeHeight), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(theme.windows.probe_scope_alpha);

            if (ImGui::Begin(
                    "🔬 Probe Scope", &showProbeScope, ImGuiWindowFlags_NoFocusOnAppearing))
            {
                ImGui::Text("Signal Probe");
                ImGui::Separator();

                // Get scope buffer
                const auto& buffer = scope->getScopeBuffer();

                // Get statistics
                float minVal = 0.0f, maxVal = 0.0f;
                scope->getStatistics(minVal, maxVal);

                if (buffer.getNumSamples() > 0 && maxVal - minVal > 0.0001f)
                {
                    // Display stats
                    ImGui::Text("Min: %.3f  Max: %.3f", minVal, maxVal);
                    ImGui::Text("Peak: %.3f", juce::jmax(std::abs(minVal), std::abs(maxVal)));

                    // Draw waveform with explicit width to avoid node expansion feedback
                    ImVec2    plotSize = ImVec2(ImGui::GetContentRegionAvail().x, 100);
                    const int numSamples = buffer.getNumSamples();
                    if (buffer.getNumChannels() > 0)
                    {
                        const float* samples = buffer.getReadPointer(0);
                        ImGui::PlotLines(
                            "##Waveform", samples, numSamples, 0, nullptr, -1.0f, 1.0f, plotSize);
                    }

                    // Button to clear probe connection
                    if (ImGui::Button("Clear Probe"))
                    {
                        synth->clearProbeConnection();
                    }
                }
                else
                {
                    ThemeText("No signal probed", theme.text.disabled);
                    ImGui::Text("Right-click > Probe Signal");
                    ImGui::Text("Then click any output pin");
                }
            }
            ImGui::End();
        }
    }
    // === END OF PROBE SCOPE OVERLAY ===

    // Clean up textures for deleted sample loaders
    if (synth != nullptr)
    {
        auto                    infos = synth->getModulesInfo();
        std::unordered_set<int> activeSampleLoaderIds;
        for (const auto& info : infos)
        {
            if (info.second.equalsIgnoreCase("sample_loader"))
            {
                activeSampleLoaderIds.insert((int)info.first);
            }
        }

        for (auto it = sampleLoaderTextureIds.begin(); it != sampleLoaderTextureIds.end();)
        {
            if (activeSampleLoaderIds.find(it->first) == activeSampleLoaderIds.end())
            {
                if (it->second)
                    it->second.reset();
                it = sampleLoaderTextureIds.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    // ADD THIS BLOCK:
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Canvas", "Ctrl+Shift+N"))
            {
                newCanvas();
            }
            if (ImGui::MenuItem("Save Preset", "Ctrl+S"))
            {
                if (currentPresetFile.existsAsFile())
                {
                    savePresetToFile(currentPresetFile);
                }
                else
                {
                    startSaveDialog();
                }
            }
            if (ImGui::MenuItem("Save Preset As...", "Ctrl+Alt+S"))
            {
                startSaveDialog();
            }
            if (ImGui::MenuItem("Load Preset", "Ctrl+O"))
            {
                startLoadDialog();
            }

            ImGui::Separator();

            // Set startup default preset
            if (ImGui::MenuItem("Set Startup Default Preset..."))
            {
                auto presetsDir = findPresetsDirectory();
                startupPresetChooser = std::make_unique<juce::FileChooser>(
                    "Choose Default Startup Preset", presetsDir, "*.xml");
                startupPresetChooser->launchAsync(
                    juce::FileBrowserComponent::openMode |
                        juce::FileBrowserComponent::canSelectFiles,
                    [this](const juce::FileChooser& fc) {
                        auto file = fc.getResult();
                        if (file.existsAsFile())
                        {
                            auto& app = PresetCreatorApplication::getApp();
                            if (auto* props = app.getProperties())
                            {
                                props->setValue("startupDefaultPreset", file.getFullPathName());
                                props->saveIfNeeded();
                                NotificationManager::post(
                                    NotificationManager::Type::Success,
                                    "Startup default preset set to: " +
                                        file.getFileNameWithoutExtension());
                                juce::Logger::writeToLog(
                                    "[Settings] Startup default preset set to: " +
                                    file.getFullPathName());
                            }
                        }
                        startupPresetChooser.reset(); // Clean up after use
                    });
            }

            // Clear startup default preset
            if (ImGui::MenuItem("Clear Startup Default Preset"))
            {
                auto& app = PresetCreatorApplication::getApp();
                if (auto* props = app.getProperties())
                {
                    props->setValue("startupDefaultPreset", "");
                    props->saveIfNeeded();
                    NotificationManager::post(
                        NotificationManager::Type::Info, "Startup default preset cleared");
                    juce::Logger::writeToLog("[Settings] Startup default preset cleared");
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Generate"))
        {
            if (ImGui::MenuItem("East Coast (Subtractive)"))
                PatchGenerator::generate(synth, PatchArchetype::EastCoast);
            if (ImGui::MenuItem("West Coast (Buchla)"))
                PatchGenerator::generate(synth, PatchArchetype::WestCoast);
            if (ImGui::MenuItem("Ambient Drone"))
                PatchGenerator::generate(synth, PatchArchetype::AmbientDrone);
            if (ImGui::MenuItem("Techno Bass"))
                PatchGenerator::generate(synth, PatchArchetype::TechnoBass);
            if (ImGui::MenuItem("Glitch Machine"))
                PatchGenerator::generate(synth, PatchArchetype::Glitch);
            if (ImGui::MenuItem("Ethereal Pad"))
                PatchGenerator::generate(synth, PatchArchetype::Ethereal);
            ImGui::Separator();
            if (ImGui::BeginMenu("Leads"))
            {
                if (ImGui::MenuItem("Acid Lead"))
                    PatchGenerator::generate(synth, PatchArchetype::AcidLead);
                if (ImGui::MenuItem("Bright Lead"))
                    PatchGenerator::generate(synth, PatchArchetype::BrightLead);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Bass"))
            {
                if (ImGui::MenuItem("Deep Bass"))
                    PatchGenerator::generate(synth, PatchArchetype::DeepBass);
                if (ImGui::MenuItem("Wobble Bass"))
                    PatchGenerator::generate(synth, PatchArchetype::WobbleBass);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Pads & Textures"))
            {
                if (ImGui::MenuItem("Warm Pad"))
                    PatchGenerator::generate(synth, PatchArchetype::WarmPad);
                if (ImGui::MenuItem("Reverb Wash"))
                    PatchGenerator::generate(synth, PatchArchetype::ReverbWash);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Rhythmic"))
            {
                if (ImGui::MenuItem("Arpeggio"))
                    PatchGenerator::generate(synth, PatchArchetype::Arpeggio);
                if (ImGui::MenuItem("Percussion"))
                    PatchGenerator::generate(synth, PatchArchetype::Percussion);
                if (ImGui::MenuItem("Stutter"))
                    PatchGenerator::generate(synth, PatchArchetype::Stutter);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Effects"))
            {
                if (ImGui::MenuItem("Delay Loop"))
                    PatchGenerator::generate(synth, PatchArchetype::DelayLoop);
                if (ImGui::MenuItem("Distorted"))
                    PatchGenerator::generate(synth, PatchArchetype::Distorted);
                if (ImGui::MenuItem("Noise Sweep"))
                    PatchGenerator::generate(synth, PatchArchetype::NoiseSweep);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Advanced"))
            {
                if (ImGui::MenuItem("FM Synthesis"))
                    PatchGenerator::generate(synth, PatchArchetype::FM);
                if (ImGui::MenuItem("Granular"))
                    PatchGenerator::generate(synth, PatchArchetype::Granular);
                if (ImGui::MenuItem("Harmonic"))
                    PatchGenerator::generate(synth, PatchArchetype::Harmonic);
                if (ImGui::MenuItem("Complex"))
                    PatchGenerator::generate(synth, PatchArchetype::Complex);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Other"))
            {
                if (ImGui::MenuItem("Pluck"))
                    PatchGenerator::generate(synth, PatchArchetype::Pluck);
                if (ImGui::MenuItem("Chord Progression"))
                    PatchGenerator::generate(synth, PatchArchetype::ChordProg);
                if (ImGui::MenuItem("Minimal"))
                    PatchGenerator::generate(synth, PatchArchetype::Minimal);
                if (ImGui::MenuItem("Experimental"))
                    PatchGenerator::generate(synth, PatchArchetype::Experimental);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Surprise Me (Random)"))
                PatchGenerator::generate(synth, PatchArchetype::Random);

            ImGui::EndMenu();
        }

        // <<< ADD THIS ENTIRE "Edit" MENU BLOCK >>>
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Clear Output Connections"))
            {
                if (synth != nullptr)
                {
                    synth->clearOutputConnections();
                    pushSnapshot(); // Make the action undoable
                }
            }

            // <<< ADD THIS ENTIRE BLOCK >>>
            bool isNodeSelected = (ImNodes::NumSelectedNodes() > 0);
            if (ImGui::MenuItem("Clear Selected Node Connections", nullptr, false, isNodeSelected))
            {
                if (synth != nullptr)
                {
                    std::vector<int> selectedNodeIds(ImNodes::NumSelectedNodes());
                    ImNodes::GetSelectedNodes(selectedNodeIds.data());
                    if (!selectedNodeIds.empty())
                    {
                        // Act on the first selected node
                        juce::uint32 logicalId = (juce::uint32)selectedNodeIds[0];
                        auto         nodeId = synth->getNodeIdForLogical(logicalId);
                        if (nodeId.uid != 0)
                        {
                            synth->clearConnectionsForNode(nodeId);
                            pushSnapshot(); // Make the action undoable
                        }
                    }
                }
            }
            // <<< END OF BLOCK >>>

            ImGui::EndMenu();
        }

        // ========================================================================
        // SETTINGS MENU - Global GPU/CPU Configuration, Audio, and MIDI
        // ========================================================================
        if (ImGui::BeginMenu("Settings"))
        {
            // CRITICAL: Wrap entire Settings menu in try-catch to prevent crashes
            // This catches any exceptions that might occur during menu rendering
            try
            {
                // Audio Settings
                if (ImGui::MenuItem("Audio Settings..."))
                {
                    if (onShowAudioSettings)
                        onShowAudioSettings();
                }

                // MIDI Device Manager
                if (ImGui::MenuItem("MIDI Device Manager..."))
                {
                    showMidiDeviceManager = !showMidiDeviceManager;
                }

                // OSC Device Manager
                if (ImGui::MenuItem("OSC Device Manager..."))
                {
                    showOscDeviceManager = !showOscDeviceManager;
                }

                if (ImGui::MenuItem("Help Manager..."))
                {
                    m_helpManager.open();
                    m_helpManager.setActiveTab(0); // Open to Shortcuts tab
                }

                if (ImGui::MenuItem("Download Piper Voices..."))
                {
                    voiceDownloadDialog.open();
                }

                if (ImGui::MenuItem("Check for Updates..."))
                {
                    if (onCheckForUpdates)
                        onCheckForUpdates();
                }

                ImGui::Separator();

                if (ImGui::BeginMenu("Theme"))
                {
                    if (ImGui::MenuItem("Edit Current Theme..."))
                    {
                        themeEditor.open();
                    }
                    ImGui::Separator();

                    // Dynamic theme scanning - refreshes each time menu is opened
                    auto loadThemePreset = [&](const char* label, const juce::String& filename) {
                        if (ImGui::MenuItem(label))
                        {
                            juce::File presetFile;
                            auto       exeFile =
                                juce::File::getSpecialLocation(juce::File::currentExecutableFile);
                            auto exeDir = exeFile.getParentDirectory();
                            auto themesDir = exeDir.getChildFile("themes");
                            auto candidate = themesDir.getChildFile(filename);

                            if (candidate.existsAsFile())
                            {
                                if (ThemeManager::getInstance().loadTheme(candidate))
                                {
                                    // Save user preference
                                    ThemeManager::getInstance().saveUserThemePreference(filename);
                                    themeEditor.refreshThemeFromManager();
                                    juce::Logger::writeToLog(
                                        "[Theme] Loaded: " + juce::String(label));
                                    s_themeToastText = "Theme Loaded: " + juce::String(label);
                                    s_themeToastEndTime = ImGui::GetTime() + 2.0;
                                }
                            }
                        }
                    };

                    // Helper to convert filename to display name
                    auto filenameToDisplayName = [](const juce::String& filename) -> juce::String {
                        juce::String name = filename;
                        // Remove .json extension
                        if (name.endsWithIgnoreCase(".json"))
                            name = name.substring(0, name.length() - 5);

                        // Special case for MoofyDark
                        if (name.equalsIgnoreCase("MoofyDark"))
                            return "Moofy Dark (Default)";

                        // Convert camelCase/PascalCase to Title Case with spaces
                        // e.g., "AtomOneLight" -> "Atom One Light", "ClassicTheme" -> "Classic
                        // Theme"
                        juce::String result;
                        for (int i = 0; i < name.length(); ++i)
                        {
                            juce::juce_wchar c = name[i];
                            // Insert space before uppercase letters (except first character)
                            if (i > 0 && juce::CharacterFunctions::isUpperCase(c))
                                result += " ";
                            result += c;
                        }

                        // Handle special cases
                        result = result.replace("Synthwave 84", "Synthwave '84");
                        result = result.replace("Rosé Pine", "Rosé Pine Moon");
                        result = result.replace("Night Owl", "Night Owl Neo");
                        result = result.replace("Everforest", "Everforest Night");
                        result = result.replace("Dracula Midnight", "Dracula Midnight");

                        return result;
                    };

                    // Scan themes directory for all .json files
                    std::vector<std::pair<juce::String, juce::String>>
                        foundThemes; // {displayName, filename}

                    try
                    {
                        auto exeFile =
                            juce::File::getSpecialLocation(juce::File::currentExecutableFile);
                        auto exeDir = exeFile.getParentDirectory();
                        auto themesDir = exeDir.getChildFile("themes");

                        if (themesDir.exists() && themesDir.isDirectory())
                        {
                            juce::Array<juce::File> themeFiles;
                            themesDir.findChildFiles(
                                themeFiles, juce::File::findFiles, false, "*.json");

                            for (const auto& themeFile : themeFiles)
                            {
                                juce::String filename = themeFile.getFileName();
                                // Skip hidden/system files
                                if (filename.startsWithChar('.'))
                                    continue;

                                juce::String displayName = filenameToDisplayName(filename);
                                foundThemes.push_back({displayName, filename});
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        juce::Logger::writeToLog(
                            "[Settings] Theme scan exception: " + juce::String(e.what()));
                        // Continue - will show "No themes found" message
                    }
                    catch (...)
                    {
                        juce::Logger::writeToLog("[Settings] Theme scan unknown exception");
                        // Continue - will show "No themes found" message
                    }

                    // Also check source tree for development (fallback)
                    try
                    {
                        auto exeFile =
                            juce::File::getSpecialLocation(juce::File::currentExecutableFile);
                        auto exeDir = exeFile.getParentDirectory();
                        auto sourceThemesDir = exeDir.getParentDirectory()
                                                   .getParentDirectory()
                                                   .getChildFile("Source")
                                                   .getChildFile("preset_creator")
                                                   .getChildFile("theme")
                                                   .getChildFile("presets");

                        if (sourceThemesDir.exists() && sourceThemesDir.isDirectory())
                        {
                            juce::Array<juce::File> sourceThemeFiles;
                            sourceThemesDir.findChildFiles(
                                sourceThemeFiles, juce::File::findFiles, false, "*.json");

                            for (const auto& themeFile : sourceThemeFiles)
                            {
                                juce::String filename = themeFile.getFileName();
                                if (filename.startsWithChar('.'))
                                    continue;

                                // Check if we already have this theme from exe/themes
                                bool alreadyFound = false;
                                for (const auto& existing : foundThemes)
                                {
                                    if (existing.second.equalsIgnoreCase(filename))
                                    {
                                        alreadyFound = true;
                                        break;
                                    }
                                }

                                if (!alreadyFound)
                                {
                                    juce::String displayName = filenameToDisplayName(filename);
                                    foundThemes.push_back({displayName, filename});
                                }
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        juce::Logger::writeToLog(
                            "[Settings] Source theme scan exception: " + juce::String(e.what()));
                        // Continue - only affects development fallback
                    }
                    catch (...)
                    {
                        juce::Logger::writeToLog("[Settings] Source theme scan unknown exception");
                        // Continue - only affects development fallback
                    }

                    // Sort themes alphabetically by display name
                    std::sort(
                        foundThemes.begin(), foundThemes.end(), [](const auto& a, const auto& b) {
                            return a.first.compareIgnoreCase(b.first) < 0;
                        });

                    // Render menu items from dynamically found themes
                    for (const auto& theme : foundThemes)
                    {
                        loadThemePreset(theme.first.toRawUTF8(), theme.second);
                    }

                    // If no themes found, show a message
                    if (foundThemes.empty())
                    {
                        ImGui::TextDisabled("No themes found in themes/ directory");
                    }

                    ImGui::EndMenu();
                }
            }
#ifndef AUDIO_ONLY_BUILD
            catch (const cv::Exception& e)
            {
                juce::Logger::writeToLog(
                    "[Settings] OpenCV exception in Settings menu: " + juce::String(e.what()));
                ImGui::TextColored(
                    ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Settings menu failed to load");
                ImGui::TextDisabled("Check log file for details");
            }
#endif
            catch (const std::exception& e)
            {
                juce::Logger::writeToLog(
                    "[Settings] Exception in Settings menu: " + juce::String(e.what()));
                ImGui::TextColored(
                    ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Settings menu failed to load");
                ImGui::TextDisabled("Check log file for details");
            }
            catch (...)
            {
                juce::Logger::writeToLog("[Settings] Unknown exception in Settings menu");
                ImGui::TextColored(
                    ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Settings menu failed to load");
                ImGui::TextDisabled("Check log file for details");
            }

            ImGui::EndMenu();
        }

        // Video GPU menu
        if (ImGui::BeginMenu("Video GPU"))
        {
            try
            {
#if WITH_CUDA_SUPPORT
                bool gpuEnabled = getGlobalGpuEnabled();
                if (ImGui::Checkbox("Enable GPU Acceleration (CUDA)", &gpuEnabled))
                {
                    setGlobalGpuEnabled(gpuEnabled);
                    juce::Logger::writeToLog(
                        "[Video GPU] Global GPU: " +
                        juce::String(gpuEnabled ? "ENABLED" : "DISABLED"));
                }

                ImGui::TextDisabled("Computer vision nodes require GPU");

                ImGui::Separator();

                // Show CUDA device info - use global cache (queried once at app startup)
                int  deviceCount = CudaDeviceCountCache::getDeviceCount();
                bool querySucceeded = CudaDeviceCountCache::querySucceeded();
                bool cudaAvailable = CudaDeviceCountCache::isAvailable();

                // Safe theme text rendering with fallback
                try
                {
                    if (!querySucceeded)
                    {
                        // Query failed - CUDA not compiled or runtime error
                        ThemeText("CUDA: Query failed", theme.text.warning);
                        ImGui::TextDisabled("CUDA runtime libraries not found or not compiled");
                    }
                    else if (cudaAvailable)
                    {
                        // Query succeeded and devices are available
                        ThemeText("CUDA Available", theme.text.success);
                        ImGui::Text("GPU Devices: %d", deviceCount);
                    }
                    else
                    {
                        // Query succeeded but no devices found
                        ThemeText("CUDA compiled but no devices found", theme.text.warning);
                        ImGui::TextDisabled("Check NVIDIA GPU drivers and CUDA installation");
                    }
                }
                catch (...)
                {
                    // Fallback if ThemeText fails
                    ImGui::TextColored(
                        ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "CUDA: Unable to query status");
                }
#else
                ImGui::TextDisabled("GPU Acceleration: Not Compiled");
                ImGui::TextDisabled("Rebuild with CUDA support to enable");
#endif
            }
#ifndef AUDIO_ONLY_BUILD
            catch (const cv::Exception& e)
            {
                juce::Logger::writeToLog("[Video GPU] OpenCV exception: " + juce::String(e.what()));
                ImGui::TextColored(
                    ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Video GPU menu failed to load");
                ImGui::TextDisabled("Check log file for details");
            }
#endif
            catch (const std::exception& e)
            {
                juce::Logger::writeToLog("[Video GPU] Exception: " + juce::String(e.what()));
                ImGui::TextColored(
                    ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Video GPU menu failed to load");
                ImGui::TextDisabled("Check log file for details");
            }
            catch (...)
            {
                juce::Logger::writeToLog("[Video GPU] Unknown exception");
                ImGui::TextColored(
                    ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Video GPU menu failed to load");
                ImGui::TextDisabled("Check log file for details");
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Actions"))
        {
            // This item should only be enabled if at least one node is selected
            bool anyNodesSelected = ImNodes::NumSelectedNodes() > 0;
            bool multipleNodesSelected = ImNodes::NumSelectedNodes() > 1;

            // Get the shortcut string for "Connect Selected to Track Mixer"
            juce::String shortcutLabel;
            {
                const auto& context = nodeEditorContextId;
                auto        userBinding = shortcutManager.getUserBinding(
                    ShortcutActionIds::graphConnectSelectedToTrackMixer, context);
                if (userBinding.hasValue() && userBinding->isValid())
                {
                    shortcutLabel = userBinding->toString();
                }
                else
                {
                    auto defaultBinding = shortcutManager.getDefaultBinding(
                        ShortcutActionIds::graphConnectSelectedToTrackMixer, context);
                    if (defaultBinding.hasValue() && defaultBinding->isValid())
                    {
                        shortcutLabel = defaultBinding->toString();
                    }
                }
            }

            if (ImGui::MenuItem(
                    "Connect Selected to Track Mixer",
                    shortcutLabel.isEmpty() ? nullptr : shortcutLabel.toRawUTF8(),
                    false,
                    anyNodesSelected))
            {
                handleConnectSelectedToTrackMixer();
            }

            // Get the shortcut string for "Connect Selected to Recorder"
            juce::String recorderShortcutLabel;
            {
                const auto& context = nodeEditorContextId;
                auto        userBinding = shortcutManager.getUserBinding(
                    ShortcutActionIds::graphConnectSelectedToRecorder, context);
                if (userBinding.hasValue() && userBinding->isValid())
                {
                    recorderShortcutLabel = userBinding->toString();
                }
                else
                {
                    auto defaultBinding = shortcutManager.getDefaultBinding(
                        ShortcutActionIds::graphConnectSelectedToRecorder, context);
                    if (defaultBinding.hasValue() && defaultBinding->isValid())
                    {
                        recorderShortcutLabel = defaultBinding->toString();
                    }
                }
            }

            if (ImGui::MenuItem(
                    "Connect Selected to Recorder",
                    recorderShortcutLabel.isEmpty() ? nullptr : recorderShortcutLabel.toRawUTF8(),
                    false,
                    anyNodesSelected))
            {
                handleConnectSelectedToRecorder();
            }

            // Meta Module: Collapse selected nodes into a reusable sub-patch
            // if (ImGui::MenuItem("Collapse to Meta Module", "Ctrl+Shift+M", false,
            // multipleNodesSelected))
            // {
            //     handleCollapseToMetaModule();
            // }

            if (ImGui::MenuItem("Record Output", "Ctrl+R"))
            {
                handleRecordOutput();
            }

            // Get the shortcut string for "Reset Node"
            juce::String resetNodeShortcutLabel;
            {
                const auto& context = nodeEditorContextId;
                auto        userBinding =
                    shortcutManager.getUserBinding(ShortcutActionIds::editResetNode, context);
                if (userBinding.hasValue() && userBinding->isValid())
                {
                    resetNodeShortcutLabel = userBinding->toString();
                }
                else
                {
                    auto defaultBinding = shortcutManager.getDefaultBinding(
                        ShortcutActionIds::editResetNode, context);
                    if (defaultBinding.hasValue() && defaultBinding->isValid())
                    {
                        resetNodeShortcutLabel = defaultBinding->toString();
                    }
                }
            }

            if (ImGui::MenuItem(
                    "Reset Node",
                    resetNodeShortcutLabel.isEmpty() ? nullptr : resetNodeShortcutLabel.toRawUTF8(),
                    false,
                    ImNodes::NumSelectedNodes() > 0))
            {
                const int numSelected = ImNodes::NumSelectedNodes();
                if (numSelected > 0 && synth != nullptr)
                {
                    pushSnapshot();

                    std::vector<int> selectedNodeIds(numSelected);
                    ImNodes::GetSelectedNodes(selectedNodeIds.data());

                    for (int lid : selectedNodeIds)
                    {
                        if (auto* module = synth->getModuleForLogical((juce::uint32)lid))
                        {
                            auto& params = module->getParameters();
                            for (auto* paramBase : params)
                            {
                                if (auto* param =
                                        dynamic_cast<juce::RangedAudioParameter*>(paramBase))
                                    param->setValueNotifyingHost(param->getDefaultValue());
                            }
                            juce::Logger::writeToLog(
                                "[Reset] Reset parameters for node " + juce::String(lid));
                        }
                    }
                }
            }

            if (ImGui::MenuItem("Beautify Layout", "Ctrl+B"))
            {
                handleBeautifyLayout();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Recording"))
        {
            if (synth != nullptr)
            {
                bool        isAnyRecording = synth->isAnyModuleRecording();
                const char* label = isAnyRecording ? "Stop All Recordings" : "Start All Recordings";
                if (ImGui::MenuItem(label))
                {
                    if (isAnyRecording)
                    {
                        synth->stopAllRecorders();
                    }
                    else
                    {
                        synth->startAllRecorders();
                    }
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Generate"))
        {
            if (ImGui::MenuItem("Randomize Patch", "Ctrl+P"))
            {
                handleRandomizePatch();
            }
            if (ImGui::MenuItem("Randomize Connections", "Ctrl+M"))
            {
                handleRandomizeConnections();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Insert Node"))
        {
            bool isNodeSelected = (selectedLogicalId != 0);

            if (ImGui::BeginMenu("Effects", isNodeSelected))
            {
                if (ImGui::MenuItem("VCF"))
                {
                    insertNodeAfterSelection("vcf");
                }
                if (ImGui::MenuItem("Delay"))
                {
                    insertNodeAfterSelection("delay");
                }
                if (ImGui::MenuItem("Reverb"))
                {
                    insertNodeAfterSelection("reverb");
                }
                if (ImGui::MenuItem("Chorus"))
                {
                    insertNodeAfterSelection("chorus");
                }
                if (ImGui::MenuItem("Phaser"))
                {
                    insertNodeAfterSelection("phaser");
                }
                if (ImGui::MenuItem("Compressor"))
                {
                    insertNodeAfterSelection("compressor");
                }
                if (ImGui::MenuItem("Limiter"))
                {
                    insertNodeAfterSelection("limiter");
                }
                if (ImGui::MenuItem("Noise Gate"))
                {
                    insertNodeAfterSelection("gate");
                }
                if (ImGui::MenuItem("Reroute"))
                {
                    insertNodeAfterSelection("reroute");
                }
                if (ImGui::MenuItem("Drive"))
                {
                    insertNodeAfterSelection("drive");
                }
                if (ImGui::MenuItem("Bit Crusher"))
                {
                    insertNodeAfterSelection("bit_crusher");
                }
                if (ImGui::MenuItem("Graphic EQ"))
                {
                    insertNodeAfterSelection("graphic_eq");
                }
                if (ImGui::MenuItem("Waveshaper"))
                {
                    insertNodeAfterSelection("waveshaper");
                }
                if (ImGui::MenuItem("8-Band Shaper"))
                {
                    insertNodeAfterSelection("8bandshaper");
                }
                if (ImGui::MenuItem("Granulator"))
                {
                    insertNodeAfterSelection("granulator");
                }
                if (ImGui::MenuItem("Harmonic Shaper"))
                {
                    insertNodeAfterSelection("harmonic_shaper");
                }
                if (ImGui::MenuItem("Time/Pitch Shifter"))
                {
                    insertNodeAfterSelection("timepitch");
                }
                if (ImGui::MenuItem("De-Crackle"))
                {
                    insertNodeAfterSelection("de_crackle");
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Modulators", isNodeSelected))
            {
                if (ImGui::MenuItem("LFO"))
                {
                    insertNodeAfterSelection("lfo");
                }
                if (ImGui::MenuItem("ADSR"))
                {
                    insertNodeAfterSelection("adsr");
                }
                if (ImGui::MenuItem("Random"))
                {
                    insertNodeAfterSelection("random");
                }
                if (ImGui::MenuItem("S&H"))
                {
                    insertNodeAfterSelection("s_and_h");
                }
                if (ImGui::MenuItem("Function Generator"))
                {
                    insertNodeAfterSelection("function_generator");
                }
                if (ImGui::MenuItem("Shaping Oscillator"))
                {
                    insertNodeAfterSelection("shaping_oscillator");
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Utilities & Logic", isNodeSelected))
            {
                if (ImGui::MenuItem("VCA"))
                {
                    insertNodeAfterSelection("vca");
                }
                if (ImGui::MenuItem("Mixer"))
                {
                    insertNodeAfterSelection("mixer");
                }
                if (ImGui::MenuItem("CV Mixer"))
                {
                    insertNodeAfterSelection("cv_mixer");
                }
                if (ImGui::MenuItem("Track Mixer"))
                {
                    insertNodeAfterSelection("track_mixer");
                }
                if (ImGui::MenuItem("PanVol"))
                {
                    insertNodeAfterSelection("panvol");
                }
                if (ImGui::MenuItem("Attenuverter"))
                {
                    insertNodeAfterSelection("attenuverter");
                }
                if (ImGui::MenuItem("Lag Processor"))
                {
                    insertNodeAfterSelection("lag_processor");
                }
                if (ImGui::MenuItem("Math"))
                {
                    insertNodeAfterSelection("math");
                }
                if (ImGui::MenuItem("Map Range"))
                {
                    insertNodeAfterSelection("map_range");
                }
                if (ImGui::MenuItem("Quantizer"))
                {
                    insertNodeAfterSelection("quantizer");
                }
                if (ImGui::MenuItem("Rate"))
                {
                    insertNodeAfterSelection("rate");
                }
                if (ImGui::MenuItem("Comparator"))
                {
                    insertNodeAfterSelection("comparator");
                }
                if (ImGui::MenuItem("Logic"))
                {
                    insertNodeAfterSelection("logic");
                }
                if (ImGui::MenuItem("Reroute"))
                {
                    insertNodeAfterSelection("reroute");
                }
                if (ImGui::MenuItem("Sequential Switch"))
                {
                    insertNodeAfterSelection("sequential_switch");
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Computer Vision", isNodeSelected))
            {
                if (ImGui::MenuItem("Video FX"))
                {
                    insertNodeAfterSelection("video_fx");
                }
                if (ImGui::MenuItem("Chromakey"))
                {
                    insertNodeAfterSelection("chromakey");
                }
                if (ImGui::MenuItem("Video Compositor"))
                {
                    insertNodeAfterSelection("video_compositor");
                }
                if (ImGui::MenuItem("Video Draw Impact"))
                {
                    insertNodeAfterSelection("video_draw_impact");
                }
                if (ImGui::MenuItem("Crop Video"))
                {
                    insertNodeAfterSelection("crop_video");
                }
                if (ImGui::MenuItem("Video Viewer"))
                {
                    insertNodeAfterSelection("video_viewer");
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("TTS", isNodeSelected))
            {
                if (ImGui::MenuItem("TTS Performer"))
                {
                    insertNodeAfterSelection("tts_performer");
                }
                if (ImGui::MenuItem("Vocal Tract Filter"))
                {
                    insertNodeAfterSelection("vocal_tract_filter");
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Analysis", isNodeSelected))
            {
                if (ImGui::MenuItem("Scope"))
                {
                    insertNodeAfterSelection("scope");
                }
                if (ImGui::MenuItem("Frequency Graph"))
                {
                    insertNodeAfterSelection("frequency_graph");
                }
                if (ImGui::MenuItem("BPM Monitor"))
                {
                    insertNodeAfterSelection("bpm_monitor");
                }
                if (ImGui::MenuItem("Onset Detector"))
                {
                    insertNodeAfterSelection("essentia_onset_detector");
                }
                if (ImGui::MenuItem("Pitch Tracker"))
                {
                    insertNodeAfterSelection("essentia_pitch_tracker");
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }
        // === DEBUG MENU ===
        if (ImGui::BeginMenu("Debug"))
        {
            if (ImGui::MenuItem("Show System Diagnostics", "Ctrl+Shift+D"))
            {
                showDebugMenu = !showDebugMenu;
            }

            if (ImGui::MenuItem("Log System State"))
            {
                if (synth != nullptr)
                {
                    juce::Logger::writeToLog("=== SYSTEM DIAGNOSTICS ===");
                    juce::Logger::writeToLog(synth->getSystemDiagnostics());
                }
            }

            if (ImGui::MenuItem("Log Selected Module Diagnostics"))
            {
                if (synth != nullptr && selectedLogicalId != 0)
                {
                    juce::Logger::writeToLog("=== MODULE DIAGNOSTICS ===");
                    juce::Logger::writeToLog(synth->getModuleDiagnostics(selectedLogicalId));
                }
            }

            bool requestedLogViewerToggle = false;
            if (ImGui::MenuItem("Show Log Viewer", nullptr, showLogViewer))
            {
                showLogViewer = !showLogViewer;
                requestedLogViewerToggle = showLogViewer;
            }
            if (requestedLogViewerToggle)
            {
                refreshLogViewerContent();
                logViewerAutoScroll = true;
            }

            ImGui::EndMenu();
        }

        // === TRANSPORT CONTROLS ===
        if (synth != nullptr)
        {
            // Get current transport state
            auto transportState = synth->getTransportState();

            // Add some spacing before transport controls
            ImGui::Separator();
            ImGui::Spacing();

            // Access PresetCreatorComponent to use unified setMasterPlayState()
            auto* presetCreator = dynamic_cast<PresetCreatorComponent*>(getParentComponent());

            // Play/Pause button
            if (transportState.isPlaying)
            {
                if (ImGui::Button("Pause"))
                {
                    // Use unified setMasterPlayState() to control both audio callback and transport
                    if (presetCreator != nullptr)
                        presetCreator->setMasterPlayState(false, TransportCommand::Pause);
                    else
                        synth->applyTransportCommand(
                            TransportCommand::Pause); // Fallback if parent not available
                }
            }
            else
            {
                if (ImGui::Button("Play"))
                {
                    // Use unified setMasterPlayState() to control both audio callback and transport
                    if (presetCreator != nullptr)
                        presetCreator->setMasterPlayState(true, TransportCommand::Play);
                    else
                        synth->applyTransportCommand(
                            TransportCommand::Play); // Fallback if parent not available
                }
            }

            ImGui::SameLine();

            // Stop button (resets position)
            if (ImGui::Button("Stop"))
            {
                // Use unified setMasterPlayState() to control both audio callback and transport
                if (presetCreator != nullptr)
                    presetCreator->setMasterPlayState(false, TransportCommand::Stop);
                else
                    synth->applyTransportCommand(
                        TransportCommand::Stop); // Fallback if parent not available

                // Reset transport position (same behavior as before)
                synth->resetTransportPosition();
            }

            ImGui::SameLine();

            // BPM control (greyed out if controlled by Tempo Clock module)
            float bpm = static_cast<float>(transportState.bpm);
            ImGui::SetNextItemWidth(80.0f);

            bool isControlled = transportState.isTempoControlledByModule.load();
            if (isControlled)
                ImGui::BeginDisabled();

            if (ImGui::DragFloat("BPM", &bpm, 0.1f, 20.0f, 999.0f, "%.1f"))
                synth->setBPM(static_cast<double>(bpm));

            if (isControlled)
            {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
                    ThemeText("Tempo Clock Module Active", theme.text.warning);
                    ImGui::TextUnformatted(
                        "A Tempo Clock node with 'Sync to Host' disabled is controlling the global "
                        "BPM.");
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }

            ImGui::SameLine();

            // Position display
            ImGui::Text("%.2f beats", transportState.songPositionBeats);
        }
        // === MULTI-MIDI DEVICE ACTIVITY INDICATOR ===
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        if (synth != nullptr)
        {
            auto activityState = synth->getMidiActivityState();

            if (activityState.deviceNames.empty())
            {
                // No MIDI devices connected
                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
                ImGui::Text("MIDI: No Devices");
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::Text("MIDI:");
                ImGui::SameLine();

                // Display each device with active channels
                for (const auto& [deviceIndex, deviceName] : activityState.deviceNames)
                {
                    ImGui::SameLine();

                    bool hasActivity = false;
                    if (activityState.deviceChannelActivity.count(deviceIndex) > 0)
                    {
                        const auto& channels = activityState.deviceChannelActivity.at(deviceIndex);
                        for (bool active : channels)
                        {
                            if (active)
                            {
                                hasActivity = true;
                                break;
                            }
                        }
                    }

                    // Abbreviated device name (max 12 chars)
                    juce::String abbrevName = deviceName;
                    if (abbrevName.length() > 12)
                        abbrevName = abbrevName.substring(0, 12) + "...";

                    // Color: bright green if active, dim gray if inactive
                    if (hasActivity)
                        ImGui::PushStyleColor(ImGuiCol_Text, theme.text.active);
                    else
                        ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);

                    ImGui::Text("[%s]", abbrevName.toRawUTF8());
                    ImGui::PopStyleColor();

                    // Tooltip with full name and active channels
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("%s", deviceName.toRawUTF8());
                        ImGui::Separator();

                        if (activityState.deviceChannelActivity.count(deviceIndex) > 0)
                        {
                            const auto& channels =
                                activityState.deviceChannelActivity.at(deviceIndex);
                            ImGui::Text("Active Channels:");
                            juce::String activeChannels;
                            for (int ch = 0; ch < 16; ++ch)
                            {
                                if (channels[ch])
                                {
                                    if (activeChannels.isNotEmpty())
                                        activeChannels += ", ";
                                    activeChannels += juce::String(ch + 1);
                                }
                            }
                            if (activeChannels.isEmpty())
                                activeChannels = "None";
                            ImGui::Text("%s", activeChannels.toRawUTF8());
                        }

                        ImGui::EndTooltip();
                    }
                }
            }
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
            ImGui::Text("MIDI: ---");
            ImGui::PopStyleColor();
        }
        // === END OF MULTI-MIDI INDICATOR ===

        // === OSC DEVICE ACTIVITY INDICATOR ===
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();

        // Get OSC device manager from parent component
        auto* presetCreator = dynamic_cast<PresetCreatorComponent*>(getParentComponent());
        if (presetCreator && presetCreator->oscDeviceManager && synth != nullptr)
        {
            auto& oscMgr = *presetCreator->oscDeviceManager;
            auto  enabledDevices = oscMgr.getEnabledDevices();
            auto  oscActivityState = synth->getOscActivityState();

            // Check last message times from OscDeviceManager activity info
            auto activitySnapshot = oscMgr.getActivitySnapshot();

            if (enabledDevices.empty())
            {
                // No OSC devices enabled
                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
                ImGui::Text("OSC: No Devices");
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::Text("OSC:");
                ImGui::SameLine();

                // Display each enabled OSC device
                for (const auto& device : enabledDevices)
                {
                    ImGui::SameLine();

                    // Check if device has recent activity
                    bool         hasRecentActivity = false;
                    juce::String lastAddress;

                    auto activityIt = activitySnapshot.find(device.deviceIndex);
                    if (activityIt != activitySnapshot.end())
                    {
                        const auto&  activity = activityIt->second;
                        juce::uint64 now = juce::Time::getMillisecondCounter();
                        juce::uint64 timeSinceMessage = now - activity.lastMessageTime;
                        hasRecentActivity =
                            (timeSinceMessage < 1000); // Active if message within 1 second
                        lastAddress = activity.lastAddress;
                    }

                    // Abbreviated device name (max 12 chars)
                    juce::String abbrevName = device.name;
                    if (abbrevName.length() > 12)
                        abbrevName = abbrevName.substring(0, 12) + "...";

                    // Color: bright green if active, dim gray if inactive but enabled
                    if (hasRecentActivity)
                        ImGui::PushStyleColor(ImGuiCol_Text, theme.text.active);
                    else
                        ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);

                    ImGui::Text("[%s]", abbrevName.toRawUTF8());
                    ImGui::PopStyleColor();

                    // Tooltip with full name, port, and last address received
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("%s", device.name.toRawUTF8());
                        ImGui::Text("Port: %d", device.port);
                        ImGui::Separator();

                        if (hasRecentActivity && lastAddress.isNotEmpty())
                        {
                            ImGui::Text("Last Address:");
                            ImGui::Text("%s", lastAddress.toRawUTF8());
                        }
                        else
                        {
                            ImGui::TextDisabled("Waiting for messages...");
                            ImGui::Text("Send OSC to: localhost:%d", device.port);
                        }

                        ImGui::EndTooltip();
                    }
                }
            }
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
            ImGui::Text("OSC: ---");
            ImGui::PopStyleColor();
        }
        // === END OF OSC INDICATOR ===

        // --- ZOOM DISPLAY (menu bar, right side) ---
#if defined(IMNODES_ZOOM_ENABLED)
        if (ImNodes::GetCurrentContext())
        {
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();
            ImGui::Text("Zoom: %.2fx", ImNodes::EditorContextGetZoom());
        }
#endif
        // --- END ZOOM DISPLAY ---

        // === HELP MENU ===
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
            {
                showAboutDialog = true;
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Help Manager...", "F1"))
            {
                m_helpManager.open();
                m_helpManager.setActiveTab(0); // Open to Shortcuts tab
            }

            // === DEBUG ZOOM TEST ===
#if defined(IMNODES_ZOOM_ENABLED)
            ImGui::Separator();
            if (ImGui::BeginMenu("Debug Zoom"))
            {
                const float currentZoom = ImNodes::EditorContextGetZoom();
                ImGui::Text("Current Zoom: %.3f", currentZoom);
                ImGui::Separator();

                if (ImGui::MenuItem("Zoom In (1.5x)"))
                {
                    ImNodes::EditorContextSetZoom(
                        1.5f, ImVec2((float)getWidth() / 2, (float)getHeight() / 2));
                    juce::Logger::writeToLog("[ZoomTest] Set zoom to 1.5x centered");
                }
                if (ImGui::MenuItem("Zoom Out (0.5x)"))
                {
                    ImNodes::EditorContextSetZoom(
                        0.5f, ImVec2((float)getWidth() / 2, (float)getHeight() / 2));
                    juce::Logger::writeToLog("[ZoomTest] Set zoom to 0.5x centered");
                }
                if (ImGui::MenuItem("Reset Zoom (1.0x)"))
                {
                    ImNodes::EditorContextSetZoom(
                        1.0f, ImVec2((float)getWidth() / 2, (float)getHeight() / 2));
                    juce::Logger::writeToLog("[ZoomTest] Reset zoom to 1.0x");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Zoom 2.0x"))
                {
                    ImNodes::EditorContextSetZoom(
                        2.0f, ImVec2((float)getWidth() / 2, (float)getHeight() / 2));
                    juce::Logger::writeToLog("[ZoomTest] Set zoom to 2.0x centered");
                }
                if (ImGui::MenuItem("Zoom 0.25x"))
                {
                    ImNodes::EditorContextSetZoom(
                        0.25f, ImVec2((float)getWidth() / 2, (float)getHeight() / 2));
                    juce::Logger::writeToLog("[ZoomTest] Set zoom to 0.25x centered");
                }
                ImGui::EndMenu();
            }
#endif
            // === END DEBUG ZOOM TEST ===

            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    // === ABOUT DIALOG ===
    if (showAboutDialog)
    {
        ImGui::OpenPopup("About");
    }

    // Track if popup is open to handle F1 key correctly
    bool aboutPopupOpen = ImGui::BeginPopupModal(
        "About", &showAboutDialog, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);

    if (aboutPopupOpen)
    {
        ImGui::Spacing();

        // Application name (large, centered)
        ImGui::SetWindowFontScale(1.5f);
        ImGui::Text("%s", VersionInfo::APPLICATION_NAME);
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Version info
        ImGui::Text("Version %s", VersionInfo::VERSION_FULL);
        ImGui::Text("%s", VersionInfo::BUILD_TYPE);

        ImGui::Spacing();

        // Author
        ImGui::Text("By %s", VersionInfo::AUTHOR);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Close button
        bool shouldClose = false;
        if (ImGui::Button("Close", ImVec2(120, 0)))
        {
            shouldClose = true;
        }

        // Handle escape/enter to close, but not F1 when dialog is already open
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        {
            shouldClose = true;
        }

        if (shouldClose)
        {
            showAboutDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
    {
        m_helpManager.open();
        m_helpManager.setActiveTab(0); // Shortcuts tab
    }

    // --- PRESET STATUS OVERLAY ---
    ImGui::SetNextWindowPos(ImVec2(sidebarWidth + padding, menuBarHeight + padding));
    ImGui::SetNextWindowBgAlpha(
        ThemeManager::getInstance().getCurrentTheme().windows.preset_status_alpha);
    ImGui::Begin(
        "Preset Status",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_AlwaysAutoResize);

    if (currentPresetFile.existsAsFile())
    {
        ImGui::Text("Preset: %s", currentPresetFile.getFileName().toRawUTF8());
    }
    else
    {
        ImGui::Text("Preset: Unsaved Patch");
    }

    // Get the theme colors
    if (isPatchDirty)
    {
        ThemeText("Status: EDITED", theme.status.edited);
    }
    else
    {
        ThemeText("Status: SAVED", theme.status.saved);
    }
    ImGui::End();
    // --- END OF PRESET STATUS OVERLAY ---
    ImGui::Columns(2, nullptr, true);
    ImGui::SetColumnWidth(0, sidebarWidth);
    // Zoom removed
    // ADD THIS BLOCK:
    ImGui::Text("Browser");
    // Create a scrolling child window to contain the entire browser
    ImGui::BeginChild("BrowserScrollRegion", ImVec2(0, 0), true);
    // Helper lambda to recursively draw the directory tree for presets
    std::function<void(const PresetManager::DirectoryNode*)> drawPresetTree =
        [&](const PresetManager::DirectoryNode* node) {
            if (!node || (node->presets.empty() && node->subdirectories.empty()))
                return;

            // Draw subdirectories first
            for (const auto& subdir : node->subdirectories)
            {
                if (ImGui::TreeNode(subdir->name.toRawUTF8()))
                {
                    drawPresetTree(subdir.get());
                    ImGui::TreePop();
                }
            }

            // Then draw presets in this directory with drag-and-drop support
            for (const auto& preset : node->presets)
            {
                if (m_presetSearchTerm.isEmpty() ||
                    preset.name.containsIgnoreCase(m_presetSearchTerm))
                {
                    // Draw the selectable item and capture its return value
                    bool clicked = ImGui::Selectable(preset.name.toRawUTF8());

                    // --- THIS IS THE FIX ---
                    // Check if this item is the source of a drag operation
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
                    {
                        // Set the payload type and data (the preset's file path)
                        const juce::String path = preset.file.getFullPathName();
                        const std::string  pathStr = path.toStdString();
                        ImGui::SetDragDropPayload(
                            "DND_PRESET_PATH", pathStr.c_str(), pathStr.length() + 1);

                        // Provide visual feedback while dragging
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::Text("Merge Preset: %s", preset.name.toRawUTF8());

                        ImGui::EndDragDropSource();
                    }
                    // If a drag did NOT occur, and the item was clicked, load the preset
                    else if (clicked)
                    {
                        loadPresetFromFile(preset.file);
                    }
                    // --- END OF FIX ---

                    // Tooltip (only shown when hovering, not dragging)
                    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0) &&
                        preset.description.isNotEmpty())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(preset.description.toRawUTF8());
                        if (!preset.tags.isEmpty())
                            ImGui::Text("Tags: %s", preset.tags.joinIntoString(", ").toRawUTF8());
                        ImGui::EndTooltip();
                    }
                }
            }
        };

    // Helper to push category colors (used for all module category headers)
    auto pushCategoryColor = [&](ModuleCategory cat) {
        ImU32  color = getImU32ForCategory(cat);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
        ImGui::PushStyleColor(ImGuiCol_Header, color);
        ImGui::PushStyleColor(
            ImGuiCol_HeaderHovered,
            ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 1.2f, c.y * 1.2f, c.z * 1.2f, 1.0f)));
        ImGui::PushStyleColor(
            ImGuiCol_HeaderActive,
            ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 1.4f, c.y * 1.4f, c.z * 1.4f, 1.0f)));

        // Automatically adjust text color based on background contrast for optimal legibility
        ImU32 optimalTextColor = ThemeUtils::getOptimalTextColor(color);
        ImGui::PushStyleColor(ImGuiCol_Text, optimalTextColor);
    };
    auto pushHeaderColors = [&](const TriStateColor& tri) {
        const ImGuiStyle& styleRef = ImGui::GetStyle();
        auto              toVec4 = [&](ImU32 value, ImGuiCol fallback) -> ImVec4 {
            if (value != 0)
                return ImGui::ColorConvertU32ToFloat4(value);
            return styleRef.Colors[fallback];
        };
        ImGui::PushStyleColor(ImGuiCol_Header, toVec4(tri.base, ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, toVec4(tri.hovered, ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, toVec4(tri.active, ImGuiCol_HeaderActive));

        // Automatically adjust text color based on background luminance for optimal legibility
        // Use the base background color to determine text color (Option B: single text color)
        ImU32 baseBgColor = tri.base != 0
                                ? tri.base
                                : ImGui::ColorConvertFloat4ToU32(styleRef.Colors[ImGuiCol_Header]);
        ImU32 optimalTextColor = ThemeUtils::getOptimalTextColor(baseBgColor);
        ImGui::PushStyleColor(ImGuiCol_Text, optimalTextColor);
    };
    // === PRESET BROWSER ===
    pushHeaderColors(theme.headers.presets);
    bool presetsExpanded = ImGui::CollapsingHeader("Presets");
    ImGui::PopStyleColor(4); // 3 background colors + 1 text color
    if (presetsExpanded)
    {
        // 1. Path Display (read-only)
        char pathBuf[1024];
        strncpy(pathBuf, m_presetScanPath.getFullPathName().toRawUTF8(), sizeof(pathBuf) - 1);
        ImGui::InputText("##presetpath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);

        // 2. "Change Path" Button
        if (ImGui::Button("Change Path##preset"))
        {
            // Construct File from path string directly - no validation, no blocking
            // If path is empty, use empty File() which opens at system default (fastest)
            juce::File startDir = m_presetScanPath.getFullPathName().isNotEmpty()
                                      ? juce::File(m_presetScanPath.getFullPathName())
                                      : juce::File();
            presetPathChooser =
                std::make_unique<juce::FileChooser>("Select Preset Directory", startDir);
            presetPathChooser->launchAsync(
                juce::FileBrowserComponent::openMode |
                    juce::FileBrowserComponent::canSelectDirectories,
                [this](const juce::FileChooser& fc) {
                    auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        m_presetScanPath = dir;
                        // Save the new path to the properties file
                        if (auto* props = PresetCreatorApplication::getApp().getProperties())
                        {
                            props->setValue("presetScanPath", m_presetScanPath.getFullPathName());
                        }
                        // Rescan the directory after path change
                        m_presetManager.clearCache();
                        m_presetManager.scanDirectory(m_presetScanPath);
                    }
                });
        }
        ImGui::SameLine();

        // 3. "Scan" Button
        if (ImGui::Button("Scan##preset"))
        {
            m_presetManager.clearCache();
            m_presetManager.scanDirectory(m_presetScanPath);
        }

        // 4. Search bar for filtering results
        char searchBuf[256] = {};
        strncpy(searchBuf, m_presetSearchTerm.toRawUTF8(), sizeof(searchBuf) - 1);
        if (ImGui::InputText("Search##preset", searchBuf, sizeof(searchBuf)))
            m_presetSearchTerm = juce::String(searchBuf);

        ImGui::Separator();

        // 5. Display hierarchical preset tree
        drawPresetTree(m_presetManager.getRootNode());
    }

    // Helper lambda to recursively draw the directory tree for samples
    std::function<void(const SampleManager::DirectoryNode*)> drawSampleTree =
        [&](const SampleManager::DirectoryNode* node) {
            if (!node || (node->samples.empty() && node->subdirectories.empty()))
                return;

            // Draw subdirectories first
            for (const auto& subdir : node->subdirectories)
            {
                if (ImGui::TreeNode(subdir->name.toRawUTF8()))
                {
                    drawSampleTree(subdir.get());
                    ImGui::TreePop();
                }
            }

            // Then draw samples in this directory with drag-and-drop support
            for (const auto& sample : node->samples)
            {
                if (m_sampleSearchTerm.isEmpty() ||
                    sample.name.containsIgnoreCase(m_sampleSearchTerm))
                {
                    // --- THIS IS THE HEROIC FIX ---

                    // A. Draw the selectable item and capture its return value (which is true on
                    // mouse release).
                    bool clicked = ImGui::Selectable(sample.name.toRawUTF8());

                    // B. Check if this item is the source of a drag operation. This takes priority.
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
                    {
                        // Set the payload (the data we are transferring is the sample's file path).
                        const juce::String path = sample.file.getFullPathName();
                        const std::string  pathStr = path.toStdString();
                        ImGui::SetDragDropPayload(
                            "DND_SAMPLE_PATH", pathStr.c_str(), pathStr.length() + 1);

                        // Provide visual feedback during the drag.
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::Text("Dragging: %s", sample.name.toRawUTF8());

                        ImGui::EndDragDropSource();
                    }
                    // C. If a drag did NOT occur, and the item was clicked (mouse released on it),
                    // then create the node.
                    else if (clicked)
                    {
                        if (synth != nullptr)
                        {
                            auto newNodeId = synth->addModule("sample_loader");
                            auto newLogicalId = synth->getLogicalIdForNode(newNodeId);
                            pendingNodeScreenPositions[(int)newLogicalId] = ImGui::GetMousePos();
                            if (auto* sampleLoader = dynamic_cast<SampleLoaderModuleProcessor*>(
                                    synth->getModuleForLogical(newLogicalId)))
                            {
                                sampleLoader->loadSample(sample.file);
                            }
                            snapshotAfterEditor = true;
                        }
                    }

                    // --- END OF FIX ---

                    // (Existing tooltip for sample info remains the same)
                    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0))
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("Duration: %.2f s", sample.durationSeconds);
                        ImGui::Text("Rate: %d Hz", sample.sampleRate);
                        ImGui::EndTooltip();
                    }
                }
            }
        };

    // === SAMPLE BROWSER ===
    pushHeaderColors(theme.headers.samples);
    bool samplesExpanded = ImGui::CollapsingHeader("Samples");
    ImGui::PopStyleColor(4); // 3 background colors + 1 text color

    if (samplesExpanded)
    {
        // 1. Path Display (read-only)
        char pathBuf[1024];
        strncpy(pathBuf, m_sampleScanPath.getFullPathName().toRawUTF8(), sizeof(pathBuf) - 1);
        ImGui::InputText("##samplepath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);

        // 2. "Change Path" Button
        if (ImGui::Button("Change Path##sample"))
        {
            // Construct File from path string directly - no validation, no blocking
            // If path is empty, use empty File() which opens at system default (fastest)
            juce::File startDir = m_sampleScanPath.getFullPathName().isNotEmpty()
                                      ? juce::File(m_sampleScanPath.getFullPathName())
                                      : juce::File();
            samplePathChooser =
                std::make_unique<juce::FileChooser>("Select Sample Directory", startDir);
            samplePathChooser->launchAsync(
                juce::FileBrowserComponent::openMode |
                    juce::FileBrowserComponent::canSelectDirectories,
                [this](const juce::FileChooser& fc) {
                    auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        m_sampleScanPath = dir;
                        // Save the new path to the properties file
                        if (auto* props = PresetCreatorApplication::getApp().getProperties())
                        {
                            props->setValue("sampleScanPath", m_sampleScanPath.getFullPathName());
                        }
                        // Rescan the directory after path change
                        m_sampleManager.clearCache();
                        m_sampleManager.scanDirectory(m_sampleScanPath);
                    }
                });
        }
        ImGui::SameLine();

        // 3. "Scan" Button
        if (ImGui::Button("Scan##sample"))
        {
            m_sampleManager.clearCache();
            m_sampleManager.scanDirectory(m_sampleScanPath);
        }

        // 4. Search bar for filtering results
        char searchBuf[256] = {};
        strncpy(searchBuf, m_sampleSearchTerm.toRawUTF8(), sizeof(searchBuf) - 1);
        if (ImGui::InputText("Search##sample", searchBuf, sizeof(searchBuf)))
            m_sampleSearchTerm = juce::String(searchBuf);

        ImGui::Separator();

        // 5. Display hierarchical sample tree
        drawSampleTree(m_sampleManager.getRootNode());
    }

    ImGui::Separator();

    // === MIDI BROWSER ===
    pushHeaderColors(theme.headers.recent);
    bool midiExpanded = ImGui::CollapsingHeader("MIDI Files");
    ImGui::PopStyleColor(4); // 3 background colors + 1 text color

    if (midiExpanded)
    {
        // 1. Path Display (read-only)
        char pathBuf[1024];
        strncpy(pathBuf, m_midiScanPath.getFullPathName().toRawUTF8(), sizeof(pathBuf) - 1);
        ImGui::InputText("##midipath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);

        // 2. "Change Path" Button
        if (ImGui::Button("Change Path##midi"))
        {
            // Construct File from path string directly - no validation, no blocking
            // If path is empty, use empty File() which opens at system default (fastest)
            juce::File startDir = m_midiScanPath.getFullPathName().isNotEmpty()
                                      ? juce::File(m_midiScanPath.getFullPathName())
                                      : juce::File();
            midiPathChooser =
                std::make_unique<juce::FileChooser>("Select MIDI Directory", startDir);
            midiPathChooser->launchAsync(
                juce::FileBrowserComponent::openMode |
                    juce::FileBrowserComponent::canSelectDirectories,
                [this](const juce::FileChooser& fc) {
                    auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        m_midiScanPath = dir;
                        // Save the new path to the properties file
                        if (auto* props = PresetCreatorApplication::getApp().getProperties())
                        {
                            props->setValue("midiScanPath", m_midiScanPath.getFullPathName());
                        }
                        // Rescan the directory after path change
                        m_midiManager.clearCache();
                        m_midiManager.scanDirectory(m_midiScanPath);
                    }
                });
        }
        ImGui::SameLine();

        // 3. "Scan" Button
        if (ImGui::Button("Scan##midi"))
        {
            m_midiManager.clearCache();
            m_midiManager.scanDirectory(m_midiScanPath);
        }

        // 4. Search bar for filtering results
        char searchBuf[256] = {};
        strncpy(searchBuf, m_midiSearchTerm.toRawUTF8(), sizeof(searchBuf) - 1);
        if (ImGui::InputText("Search##midi", searchBuf, sizeof(searchBuf)))
            m_midiSearchTerm = juce::String(searchBuf);
        ImGui::Separator();
        // 5. Display hierarchical MIDI tree
        std::function<void(const MidiManager::DirectoryNode*)> drawMidiTree =
            [&](const MidiManager::DirectoryNode* node) {
                if (!node || (node->midiFiles.empty() && node->subdirectories.empty()))
                    return;

                // Draw subdirectories first
                for (const auto& subdir : node->subdirectories)
                {
                    if (ImGui::TreeNode(subdir->name.toRawUTF8()))
                    {
                        drawMidiTree(subdir.get());
                        ImGui::TreePop();
                    }
                }

                // Then draw MIDI files in this directory with drag-and-drop support
                for (const auto& midi : node->midiFiles)
                {
                    if (m_midiSearchTerm.isEmpty() ||
                        midi.name.containsIgnoreCase(m_midiSearchTerm))
                    {
                        // Draw the selectable item and capture its return value
                        bool clicked = ImGui::Selectable(midi.name.toRawUTF8());

                        // Check if this item is the source of a drag operation
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
                        {
                            // Set the payload (the MIDI file path)
                            const juce::String path = midi.file.getFullPathName();
                            const std::string  pathStr = path.toStdString();
                            ImGui::SetDragDropPayload(
                                "DND_MIDI_PATH", pathStr.c_str(), pathStr.length() + 1);

                            // Provide visual feedback during the drag
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::Text("Dragging: %s", midi.name.toRawUTF8());

                            ImGui::EndDragDropSource();
                        }
                        // If a drag did NOT occur, and the item was clicked, create a new MIDI
                        // Player node
                        else if (clicked)
                        {
                            if (synth != nullptr)
                            {
                                auto newNodeId = synth->addModule("midi_player");
                                auto newLogicalId = synth->getLogicalIdForNode(newNodeId);
                                pendingNodeScreenPositions[(int)newLogicalId] =
                                    ImGui::GetMousePos();

                                // Load the MIDI file into the new player
                                if (auto* midiPlayer = dynamic_cast<MIDIPlayerModuleProcessor*>(
                                        synth->getModuleForLogical(newLogicalId)))
                                {
                                    midiPlayer->loadMIDIFile(midi.file);
                                }

                                snapshotAfterEditor = true;
                            }
                        }

                        // Tooltip for MIDI info (only shown when hovering, not dragging)
                        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0))
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("MIDI File: %s", midi.file.getFileName().toRawUTF8());
                            ImGui::EndTooltip();
                        }
                    }
                }
            };

        drawMidiTree(m_midiManager.getRootNode());
    }

    ImGui::Separator();

    // === VST BROWSER ===
    pushHeaderColors(
        theme.headers.recent); // Use recent header color for VST (can add dedicated color later)
    bool vstExpanded = ImGui::CollapsingHeader("VST Plugins");
    ImGui::PopStyleColor(4); // 3 background colors + 1 text color

    if (vstExpanded)
    {
        // 1. Path Display (read-only)
        char pathBuf[1024];
        strncpy(pathBuf, m_vstScanPath.getFullPathName().toRawUTF8(), sizeof(pathBuf) - 1);
        ImGui::InputText("##vstpath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);

        // 2. "Change Path" Button
        if (ImGui::Button("Change Path##vst"))
        {
            // Construct File from path string directly - no validation, no blocking
            // If path is empty, use empty File() which opens at system default (fastest)
            juce::File startDir = m_vstScanPath.getFullPathName().isNotEmpty()
                                      ? juce::File(m_vstScanPath.getFullPathName())
                                      : juce::File();
            vstPathChooser = std::make_unique<juce::FileChooser>("Select VST Directory", startDir);
            vstPathChooser->launchAsync(
                juce::FileBrowserComponent::openMode |
                    juce::FileBrowserComponent::canSelectDirectories,
                [this](const juce::FileChooser& fc) {
                    auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        m_vstScanPath = dir;
                        // Save the new path to the properties file
                        if (auto* props = PresetCreatorApplication::getApp().getProperties())
                        {
                            props->setValue("vstScanPath", m_vstScanPath.getFullPathName());
                        }
                        // Rescan the directory after path change
                        auto& app = PresetCreatorApplication::getApp();
                        m_vstManager.clearCache();
                        m_vstManager.scanDirectory(
                            m_vstScanPath, app.getPluginFormatManager(), app.getKnownPluginList());
                    }
                });
        }
        ImGui::SameLine();

        // 3. "Scan" Button
        if (ImGui::Button("Scan##vst"))
        {
            auto& app = PresetCreatorApplication::getApp();
            m_vstManager.clearCache();
            m_vstManager.scanDirectory(
                m_vstScanPath, app.getPluginFormatManager(), app.getKnownPluginList());
        }

        // 4. Search bar for filtering results
        char searchBuf[256] = {};
        strncpy(searchBuf, m_vstSearchTerm.toRawUTF8(), sizeof(searchBuf) - 1);
        if (ImGui::InputText("Search##vst", searchBuf, sizeof(searchBuf)))
            m_vstSearchTerm = juce::String(searchBuf);

        ImGui::Separator();

        // 5. Display hierarchical VST tree
        std::function<void(const VstManager::DirectoryNode*)> drawVstTree =
            [&](const VstManager::DirectoryNode* node) {
                if (!node || (node->plugins.empty() && node->subdirectories.empty()))
                    return;

                // Draw subdirectories (manufacturers) first
                for (const auto& subdir : node->subdirectories)
                {
                    if (ImGui::TreeNode(subdir->name.toRawUTF8()))
                    {
                        drawVstTree(subdir.get());
                        ImGui::TreePop();
                    }
                }

                // Draw plugins in this node
                for (const auto& plugin : node->plugins)
                {
                    // Apply search filter
                    if (m_vstSearchTerm.isNotEmpty() &&
                        !plugin.name.containsIgnoreCase(m_vstSearchTerm) &&
                        !plugin.manufacturer.containsIgnoreCase(m_vstSearchTerm))
                        continue;

                    juce::String displayName = plugin.name;
                    if (plugin.manufacturer.isNotEmpty())
                        displayName += " (" + plugin.manufacturer + ")";

                    bool clicked = ImGui::Selectable(displayName.toRawUTF8());

                    // Drag and drop support
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
                    {
                        // Store plugin description as payload
                        const std::string pluginId =
                            plugin.description.createIdentifierString().toStdString();
                        ImGui::SetDragDropPayload(
                            "DND_VST_PLUGIN", pluginId.c_str(), pluginId.length() + 1);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::Text("Dragging: %s", displayName.toRawUTF8());
                        ImGui::EndDragDropSource();
                    }

                    // If clicked (not dragged), add plugin as node
                    if (clicked && synth != nullptr)
                    {
                        auto& app = PresetCreatorApplication::getApp();
                        auto  nodeId =
                            synth->addVstModule(app.getPluginFormatManager(), plugin.description);
                        if (nodeId.uid != 0)
                        {
                            const ImVec2 mouse = ImGui::GetMousePos();
                            const auto   logicalId = synth->getLogicalIdForNode(nodeId);
                            pendingNodeScreenPositions[(int)logicalId] = mouse;
                            snapshotAfterEditor = true;
                            juce::Logger::writeToLog("[VST] Added plugin: " + plugin.name);
                        }
                        else
                        {
                            juce::Logger::writeToLog(
                                "[VST] ERROR: Failed to add plugin: " + plugin.name);
                        }
                    }

                    // Tooltip with plugin info
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("Name: %s", plugin.name.toRawUTF8());
                        ImGui::Text("Manufacturer: %s", plugin.manufacturer.toRawUTF8());
                        ImGui::Text("Version: %s", plugin.version.toRawUTF8());
                        ImGui::Text("Type: %s", plugin.isInstrument ? "Instrument" : "Effect");
                        ImGui::Text("Inputs: %d, Outputs: %d", plugin.numInputs, plugin.numOutputs);
                        ImGui::EndTooltip();
                    }
                }
            };

        drawVstTree(m_vstManager.getRootNode());
    }

    ImGui::Separator();

    // === MODULE BROWSER ===
    pushHeaderColors(theme.headers.system);
    bool modulesExpanded = ImGui::CollapsingHeader("Modules", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(4); // 3 background colors + 1 text color

    if (modulesExpanded)
    {

        auto addModuleButton = [this](const char* label, const char* type) {
            if (ImGui::Selectable(label, false))
            {
                if (synth != nullptr)
                {
                    auto         nodeId = synth->addModule(type);
                    const ImVec2 mouse = ImGui::GetMousePos();
                    // queue screen-space placement after node is drawn to avoid assertions
                    const int logicalId = (int)synth->getLogicalIdForNode(nodeId);
                    pendingNodeScreenPositions[logicalId] = mouse;
                    // Defer snapshot until after EndNodeEditor so the node exists in this frame
                    snapshotAfterEditor = true;
                }
            }

            // --- FIX: Show tooltip with module description on hover ---
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();

                // Find the description in our list using the module's internal 'type'
                bool found = false;
                for (const auto& pair : getModuleDescriptions())
                {
                    if (pair.first.equalsIgnoreCase(type))
                    {
                        // If found, display it
                        ImGui::TextUnformatted(pair.second);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    // Fallback text if a description is missing
                    ImGui::TextUnformatted("No description available.");
                }

                ImGui::EndTooltip();
            }
        };

        // ═══════════════════════════════════════════════════════════════════════════════
        // MODULE NAMING CONVENTION:
        // ───────────────────────────────────────────────────────────────────────────────
        // ALL module type names MUST follow this strict naming convention:
        //   • Use ONLY lowercase letters (a-z)
        //   • Use ONLY numbers (0-9) where appropriate
        //   • Replace ALL spaces with underscores (_)
        //   • NO capital letters allowed
        //   • NO hyphens or other special characters
        //
        // Examples:
        //   ✓ CORRECT:   "midi_player", "sample_loader", "graphic_eq", "vco"
        //   ✗ INCORRECT: "MIDI Player", "Sample Loader", "Graphic EQ", "VCO"
        //
        // This ensures consistent module identification across the system.
        // ═══════════════════════════════════════════════════════════════════════════════

        // ═══════════════════════════════════════════════════════════════════════════════
        // 1. SOURCES - Signal generators and inputs
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Source);
        bool sourcesExpanded = ImGui::CollapsingHeader("Sources", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (sourcesExpanded)
        {
            addModuleButton("VCO", "vco");
            addModuleButton("Polyphonic VCO", "polyvco");
            addModuleButton("STK String", "stk_string");
            addModuleButton("STK Wind", "stk_wind");
            addModuleButton("STK Percussion", "stk_percussion");
            addModuleButton("STK Plucked", "stk_plucked");
            addModuleButton("Noise", "noise");
            addModuleButton("Audio Input", "audio_input");
            addModuleButton("Sample Loader", "sample_loader");
            addModuleButton("Sample SFX", "sample_sfx");
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Streaming:");
            addModuleButton("Internet Radio", "internet_radio");
            addModuleButton("SDR Receiver", "sdr_receiver");
            addModuleButton("System Audio", "system_audio");
            addModuleButton("SRT Receiver", "srt_receiver");
            addModuleButton("RTMP Receiver", "rtmp_receiver");
            addModuleButton("Bluetooth Audio", "bluetooth_audio");
            ImGui::Separator();
            addModuleButton("Value", "value");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 2. EFFECTS - Audio processing and tone shaping
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Effect);
        bool effectsExpanded = ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (effectsExpanded)
        {
            addModuleButton("VCF", "vcf");
            addModuleButton("Delay", "delay");
            addModuleButton("Reverb", "reverb");
            addModuleButton("Chorus", "chorus");
            addModuleButton("Spatial Granulator", "spatial_granulator");
            addModuleButton("Phaser", "phaser");
            addModuleButton("Compressor", "compressor");
            addModuleButton("Limiter", "limiter");
            addModuleButton("Noise Gate", "gate");
            addModuleButton("Drive", "drive");
            addModuleButton("Bit Crusher", "bit_crusher");
            addModuleButton("Graphic EQ", "graphic_eq");
            addModuleButton("Waveshaper", "waveshaper");
            addModuleButton("8-Band Shaper", "8bandshaper");
            addModuleButton("Granulator", "granulator");
            addModuleButton("Harmonic Shaper", "harmonic_shaper");
            addModuleButton("Time/Pitch Shifter", "timepitch");
            addModuleButton("De-Crackle", "de_crackle");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 3. MODULATORS - CV generation and modulation sources
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Modulator);
        bool modulatorsExpanded =
            ImGui::CollapsingHeader("Modulators", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (modulatorsExpanded)
        {
            addModuleButton("LFO", "lfo");
            addModuleButton("ADSR", "adsr");
            addModuleButton("Random", "random");
            addModuleButton("S&H", "s_and_h");
            addModuleButton("Function Generator", "function_generator");
            addModuleButton("Shaping Oscillator", "shaping_oscillator");
        }
        // ═══════════════════════════════════════════════════════════════════════════════
        // 4. UTILITIES & LOGIC - Signal processing and routing
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Utility);
        bool utilitiesExpanded =
            ImGui::CollapsingHeader("Utilities & Logic", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (utilitiesExpanded)
        {
            addModuleButton("VCA", "vca");
            addModuleButton("Mixer", "mixer");
            addModuleButton("CV Mixer", "cv_mixer");
            addModuleButton("Track Mixer", "track_mixer");
            addModuleButton("Attenuverter", "attenuverter");
            addModuleButton("Reroute", "reroute");
            addModuleButton("Lag Processor", "lag_processor");
            addModuleButton("Math", "math");
            addModuleButton("Map Range", "map_range");
            addModuleButton("Quantizer", "quantizer");
            addModuleButton("Rate", "rate");
            addModuleButton("Comparator", "comparator");
            addModuleButton("Logic", "logic");
            addModuleButton("Clock Divider", "clock_divider");
            addModuleButton("Sequential Switch", "sequential_switch");
            addModuleButton("PanVol", "panvol");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 5. SEQUENCERS - Pattern and rhythm generation
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Seq);
        bool sequencersExpanded =
            ImGui::CollapsingHeader("Sequencers", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (sequencersExpanded)
        {
            addModuleButton("Sequencer", "sequencer");
            addModuleButton("Multi Sequencer", "multi_sequencer");
            addModuleButton("Tempo Clock", "tempo_clock");
            addModuleButton("Snapshot Sequencer", "snapshot_sequencer");
            addModuleButton("Stroke Sequencer", "stroke_sequencer");
            addModuleButton("Chord Arp", "chord_arp");
            addModuleButton("Timeline", "timeline");
            addModuleButton("Automation Lane", "automation_lane");
            addModuleButton("Automato", "automato");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 6. MIDI - MIDI input/output and controllers
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::MIDI);
        bool midiExpanded = ImGui::CollapsingHeader("MIDI", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (midiExpanded)
        {
            ThemeText("Players / Editors:", theme.text.section_header);
            addModuleButton("MIDI CV", "midi_cv");
            addModuleButton("OSC CV", "osc_cv");
            addModuleButton("CV -> OSC", "cv_osc_sender");
            addModuleButton("MIDI Player", "midi_player");
            addModuleButton("MIDI Logger", "midi_logger");
            ImGui::Spacing();
            ThemeText("Controllers:", theme.text.section_header);
            addModuleButton("MIDI Faders", "midi_faders");
            addModuleButton("MIDI Knobs", "midi_knobs");
            addModuleButton("MIDI Buttons", "midi_buttons");
            addModuleButton("MIDI Jog Wheel", "midi_jog_wheel");
            addModuleButton("MIDI Pads", "midi_pads");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 7. ANALYSIS - Signal visualization and debugging
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Analysis);
        bool analysisExpanded = ImGui::CollapsingHeader("Analysis", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (analysisExpanded)
        {
            addModuleButton("Scope", "scope");
            addModuleButton("Debug", "debug");
            addModuleButton("Input Debug", "input_debug");
            addModuleButton("Onset Detector", "essentia_onset_detector");
            addModuleButton("Pitch Tracker", "essentia_pitch_tracker");
            addModuleButton("Frequency Graph", "frequency_graph");
            addModuleButton("BPM Monitor", "bpm_monitor");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 8. TTS - Text-to-Speech and vocal synthesis
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::TTS_Voice);
        bool ttsExpanded = ImGui::CollapsingHeader("TTS", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (ttsExpanded)
        {
            addModuleButton("TTS Performer", "tts_performer");
            addModuleButton("Vocal Tract Filter", "vocal_tract_filter");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 9. SPECIAL - Physics, animation, and experimental
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Special_Exp);
        bool specialExpanded = ImGui::CollapsingHeader("Special", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (specialExpanded)
        {
            addModuleButton("Physics", "physics");
            addModuleButton("Animation", "animation");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 10. COMPUTER VISION - Video processing and analysis
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::OpenCV);
        bool openCVExpanded =
            ImGui::CollapsingHeader("Computer Vision", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (openCVExpanded)
        {
            ThemeText("Sources:", theme.text.section_header);
            addModuleButton("Webcam Loader", "webcam_loader");
            addModuleButton("Video File Loader", "video_file_loader");
            addModuleButton("NDI Receiver", "ndi_receiver");
            addModuleButton("YouTube Source", "youtube_source");
            ImGui::Spacing();
            ThemeText("Processors:", theme.text.section_header);
            addModuleButton("Video FX", "video_fx");
            addModuleButton("Chromakey", "chromakey");
            addModuleButton("Video Compositor", "video_compositor");
            addModuleButton("Video Draw Impact", "video_draw_impact");
            addModuleButton("Video Viewer", "video_viewer");
            addModuleButton("Movement Detector", "movement_detector");
            addModuleButton("Object Detector", "object_detector");
            addModuleButton("Pose Estimator", "pose_estimator");
            addModuleButton("Hand Tracker", "hand_tracker");
            addModuleButton("Face Tracker", "face_tracker");
            addModuleButton("Color Tracker", "color_tracker");
            addModuleButton("Contour Detector", "contour_detector");
        }

        // ═══════════════════════════════════════════════════════════════════════════════
        // 11. SYSTEM - Patch organization and system utilities
        // ═══════════════════════════════════════════════════════════════════════════════
        pushCategoryColor(ModuleCategory::Sys);
        bool systemExpanded = ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4); // 3 background colors + 1 text color
        if (systemExpanded)
        {
            // addModuleButton("Meta", "meta");
            // addModuleButton("Inlet", "inlet");
            // addModuleButton("Outlet", "outlet");
            addModuleButton("Comment", "comment");
            addModuleButton("Recorder", "recorder");
        }

    } // End of Modules collapsing header

    // End the scrolling region
    ImGui::EndChild();
    ImGui::NextColumn();
    // --- DEFINITIVE FIX FOR PRESET DRAG-AND-DROP WITH VISUAL FEEDBACK ---
    // Step 1: Define canvas dimensions first (needed for the drop target)
    // Get grid/canvas colors from theme
    auto&       themeMgr = ThemeManager::getInstance();
    const ImU32 GRID_COLOR = themeMgr.getGridColor();
    const ImU32 GRID_ORIGIN_COLOR = themeMgr.getGridOriginColor();
    const float GRID_SIZE = themeMgr.getGridSize();
    ImVec2      canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2      canvas_sz = ImGui::GetContentRegionAvail();
    ImVec2      canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    // Cache canvas dimensions for modal pan logic later
    lastCanvasP0 = canvas_p0;
    lastCanvasSize = canvas_sz;

    // Step 2: Create a full-canvas invisible button to act as our drop area
    ImGui::SetCursorScreenPos(canvas_p0);
    ImGui::InvisibleButton("##canvas_drop_target", canvas_sz);

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImU32 nodeBackground = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_ChildBg]);
    const ImU32 nodeBackgroundHover =
        ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBgHovered]);
    const ImU32 nodeBackgroundSel =
        ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBgActive]);
    const ImU32 nodeOutline = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Border]);

    auto& imnodesStyle = ImNodes::GetStyle();
    imnodesStyle.NodeCornerRounding = style.ChildRounding;
    imnodesStyle.NodeBorderThickness = style.FrameBorderSize;

    ImNodes::PushColorStyle(ImNodesCol_NodeBackground, nodeBackground);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, nodeBackgroundHover);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, nodeBackgroundSel);
    ImNodes::PushColorStyle(ImNodesCol_NodeOutline, nodeOutline);

    // Step 3: Make this area a drop target with visual feedback
    if (ImGui::BeginDragDropTarget())
    {
        // Check if a preset payload is being hovered over the canvas
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "DND_PRESET_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery))
        {
            // Draw a semi-transparent overlay to show the canvas is a valid drop zone
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            drawList->AddRectFilled(canvas_p0, canvas_p1, themeMgr.getDropTargetOverlay());

            // Check if the mouse button was released to complete the drop
            if (payload->IsDelivery())
            {
                const char* path = (const char*)payload->Data;
                ImVec2      dropPos = ImGui::GetMousePos(); // Get the exact drop position
                mergePresetFromFile(juce::File(path), dropPos);
            }
        }
        ImGui::EndDragDropTarget();
    }
    // --- END OF DEFINITIVE FIX ---

    // Reset cursor position for subsequent drawing
    ImGui::SetCursorScreenPos(canvas_p0);

    // <<< ADD THIS ENTIRE BLOCK TO CACHE CONNECTION STATUS >>>
    std::unordered_set<int> connectedInputAttrs;
    std::unordered_set<int> connectedOutputAttrs;
    if (synth != nullptr)
    {
        for (const auto& c : synth->getConnectionsInfo())
        {
            int srcAttr = encodePinId({c.srcLogicalId, c.srcChan, false});
            connectedOutputAttrs.insert(srcAttr);

            int dstAttr = c.dstIsOutput ? encodePinId({0, c.dstChan, true})
                                        : encodePinId({c.dstLogicalId, c.dstChan, true});
            connectedInputAttrs.insert(dstAttr);
        }
    }
    // <<< END OF BLOCK >>>

    // <<< ADD THIS BLOCK TO DEFINE COLORS >>>
    const ImU32 colPin = themeMgr.getPinDisconnectedColor();
    const ImU32 colPinConnected = themeMgr.getPinConnectedColor();
    // <<< END OF BLOCK >>>

    // Pre-register is no longer needed - stateless encoding generates IDs on-the-fly
    // (Removed the old pre-registration loop)

    // --- BACKGROUND GRID AND COORDINATE DISPLAY ---
    // (Canvas dimensions already defined above in the drop target code)
    // Draw into the window draw list so colors aren't obscured by window bg
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    // Note: EditorContextGetPanning() can only be called AFTER BeginNodeEditor()
    // Since we draw the grid before BeginNodeEditor, we use zero panning here
    // The grid will be drawn correctly after BeginNodeEditor is called
    ImVec2 panning = lastEditorPanning;

    // Node canvas bound to the underlying model if available
    // Hide ImNodes' own grid so we only render the custom one above.
    ImNodes::PushColorStyle(ImNodesCol_GridBackground, IM_COL32(0, 0, 0, 0));
    ImNodes::PushColorStyle(ImNodesCol_GridLine, IM_COL32(0, 0, 0, 0));
    ImNodes::PushColorStyle(ImNodesCol_GridLinePrimary, IM_COL32(0, 0, 0, 0));

    // Draw canvas background (behind everything)
    draw_list->AddRectFilled(canvas_p0, canvas_p1, themeMgr.getCanvasBackground());

    // Draw grid lines
    for (float x = fmodf(panning.x, GRID_SIZE); x < canvas_sz.x; x += GRID_SIZE)
        draw_list->AddLine(
            ImVec2(canvas_p0.x + x, canvas_p0.y),
            ImVec2(canvas_p0.x + x, canvas_p0.y + canvas_sz.y),
            GRID_COLOR);
    for (float y = fmodf(panning.y, GRID_SIZE); y < canvas_sz.y; y += GRID_SIZE)
        draw_list->AddLine(
            ImVec2(canvas_p0.x, canvas_p0.y + y),
            ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + y),
            GRID_SIZE);
    // Draw thicker lines for the origin (0,0)
    ImVec2 origin_on_screen = ImVec2(canvas_p0.x + panning.x, canvas_p0.y + panning.y);
    draw_list->AddLine(
        ImVec2(origin_on_screen.x, canvas_p0.y),
        ImVec2(origin_on_screen.x, canvas_p1.y),
        GRID_ORIGIN_COLOR,
        2.0f);
    draw_list->AddLine(
        ImVec2(canvas_p0.x, origin_on_screen.y),
        ImVec2(canvas_p1.x, origin_on_screen.y),
        GRID_ORIGIN_COLOR,
        2.0f);
    // Draw scale markers every SCALE_INTERVAL grid units as a grid (not a cross)
    const float SCALE_INTERVAL = themeMgr.getScaleInterval();
    const ImU32 SCALE_TEXT_COLOR = themeMgr.getScaleTextColor();
    ImDrawList* fg_draw_list = ImGui::GetForegroundDrawList();
    // X-axis scale markers - always at the bottom edge
    float gridLeft = -panning.x;
    float gridRight = canvas_sz.x - panning.x;
    int   startX = (int)std::floor(gridLeft / SCALE_INTERVAL);
    int   endX = (int)std::ceil(gridRight / SCALE_INTERVAL);
    for (int i = startX; i <= endX; ++i)
    {
        float gridX = i * SCALE_INTERVAL;
        float screenX = canvas_p0.x + panning.x + gridX;

        // Only draw if visible on screen
        if (screenX >= canvas_p0.x && screenX <= canvas_p1.x)
        {
            char label[16];
            snprintf(label, sizeof(label), "%.0f", gridX);
            // Always draw at bottom edge
            fg_draw_list->AddText(ImVec2(screenX + 2, canvas_p1.y - 45), SCALE_TEXT_COLOR, label);
        }
    }

    // Y-axis scale markers - always at the left edge
    float gridTop = -panning.y;
    float gridBottom = canvas_sz.y - panning.y;
    int   startY = (int)std::floor(gridTop / SCALE_INTERVAL);
    int   endY = (int)std::ceil(gridBottom / SCALE_INTERVAL);

    for (int i = startY; i <= endY; ++i)
    {
        float gridY = i * SCALE_INTERVAL;
        float screenY = canvas_p0.y + panning.y + gridY;

        // Only draw if visible on screen
        if (screenY >= canvas_p0.y && screenY <= canvas_p1.y)
        {
            char label[16];
            snprintf(label, sizeof(label), "%.0f", gridY);
            // Always draw at left edge
            fg_draw_list->AddText(ImVec2(canvas_p0.x + 5, screenY + 2), SCALE_TEXT_COLOR, label);
        }
    }
    // Mouse coordinate display overlay (bottom-left)
    ImVec2 mouseScreenPos = ImGui::GetMousePos();
    float  currentZoom = 1.0f;
    ImVec2 mouseGridPos = ImVec2(
        mouseScreenPos.x - canvas_p0.x - panning.x, mouseScreenPos.y - canvas_p0.y - panning.y);
    char posStr[32];
    snprintf(posStr, sizeof(posStr), "%.0f, %.0f", mouseGridPos.x, mouseGridPos.y);
    // Use the foreground draw list to ensure text is on top of everything
    // Position at bottom-left: canvas_p1.y is bottom edge, subtract text height plus padding
    ImGui::GetForegroundDrawList()->AddText(
        ImVec2(canvas_p0.x + 10, canvas_p1.y - 25), themeMgr.getMousePositionText(), posStr);

    // --- Cut-by-line gesture (preview) ---
    {
        const bool altDown = ImGui::GetIO().KeyAlt;
        const bool rmbPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        const bool rmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);

        // Contextual helper near cursor
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
        {
            ImGuiWindowFlags hintFlags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
            // If a link is hovered (probe tooltip likely shown), bias the hint to top-left
            const bool   linkLikelyHovered = (lastHoveredLinkId != -1);
            const ImVec2 basePos = ImVec2(mouseScreenPos.x, mouseScreenPos.y);
            const ImVec2 hintOffset =
                linkLikelyHovered ? ImVec2(-180.0f, -16.0f) : ImVec2(14.0f, 16.0f);
            const ImVec2 hintPos = ImVec2(basePos.x + hintOffset.x, basePos.y + hintOffset.y);
            if (altDown && !rmbDown && !cutModeActive)
            {
                ImGui::SetNextWindowPos(hintPos);
                ImGui::Begin("##CutHintIdle", nullptr, hintFlags);
                ImGui::TextUnformatted("Alt + Right-drag: cut cables");
                ImGui::End();
            }
            else if (cutModeActive && rmbDown)
            {
                ImGui::SetNextWindowPos(hintPos);
                ImGui::Begin("##CutHintActive", nullptr, hintFlags);
                ImGui::TextUnformatted("Release to split with reroutes");
                ImGui::End();
            }
            else if (ImGui::GetIO().KeyCtrl)
            {
                ImGui::SetNextWindowPos(hintPos);
                ImGui::Begin("##CtrlHint", nullptr, hintFlags);
                ImGui::TextUnformatted(
                    "Ctrl + Click link: detach\nCtrl + Drag: move cable\nCtrl + Mid-click cable: "
                    "duplicate");
                ImGui::End();
            }
        }

        if (!cutModeActive && altDown && rmbPressed)
        {
            cutModeActive = true;
            cutStartGrid = mouseGridPos;
            cutEndGrid = mouseGridPos;
            juce::Logger::writeToLog(
                "[CutGesture] STARTED at Grid(" + juce::String(cutStartGrid.x) + ", " +
                juce::String(cutStartGrid.y) + ")");
        }
        if (cutModeActive && rmbDown)
        {
            cutEndGrid = mouseGridPos;
            // Draw preview in screen space (foreground to guarantee visibility)
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2      a = ImVec2(
                canvas_p0.x + panning.x + cutStartGrid.x, canvas_p0.y + panning.y + cutStartGrid.y);
            ImVec2 b = ImVec2(
                canvas_p0.x + panning.x + cutEndGrid.x, canvas_p0.y + panning.y + cutEndGrid.y);
            dl->AddLine(a, b, IM_COL32(255, 200, 0, 255), 2.0f);
        }
    }
    // --- END OF BACKGROUND GRID AND COORDINATE DISPLAY ---
    // Node canvas bound to the underlying model if available
    // Keep ImNodes' background/panning grid visible, but colour-match to theme overrides.
    ImNodes::PushColorStyle(ImNodesCol_GridBackground, themeMgr.getCanvasBackground());
    ImNodes::PushColorStyle(ImNodesCol_GridLine, GRID_COLOR);
    ImNodes::PushColorStyle(ImNodesCol_GridLinePrimary, GRID_ORIGIN_COLOR);
    ImNodes::PushColorStyle(ImNodesCol_BoxSelector, themeMgr.getSelectionRect());
    ImNodes::PushColorStyle(ImNodesCol_BoxSelectorOutline, themeMgr.getSelectionRectOutline());
    // === END OF FIX ===
    ImNodes::BeginNodeEditor();
    lastEditorPanning = ImNodes::EditorContextGetPanning();
    // Now we can safely get the actual panning for any future use
    // (Grid is already drawn with zero panning above, which is fine for background)
    // Begin the editor
    // +++ ADD THIS LINE AT THE START OF THE RENDER LOOP +++
    // attrPositions.clear(); // REMOVED: Persist cache for off-screen pins
    auto cancelDragInsert = [this]() {
        dragInsertActive = false;
        dragInsertStartAttrId = -1;
        dragInsertStartPin = PinID{};
        shouldOpenDragInsertPopup = false;
    };
    // Rebuild mod attribute mapping from currently drawn nodes only
    // modAttrToParam.clear(); // TODO: Remove when fully migrated
    // Track which attribute IDs were actually registered this frame
    std::unordered_set<int> availableAttrs;
    // Track duplicates to diagnose disappearing pins
    std::unordered_set<int> seenAttrs;
    auto linkIdOf = [this](int srcAttr, int dstAttr) -> int { return getLinkId(srcAttr, dstAttr); };
    if (synth != nullptr)
    {
        // Apply any pending UI state restore (first frame after load)
        if (uiPending.isValid())
        {
            // Cache target positions to ensure they stick even if nodes are created later this
            // frame
            auto nodes = uiPending;
            for (int i = 0; i < nodes.getNumChildren(); ++i)
            {
                auto n = nodes.getChild(i);
                if (!n.hasType("node"))
                    continue;
                const int   nid = (int)n.getProperty("id", 0);
                const float x = (float)n.getProperty("x", 0.0f);
                const float y = (float)n.getProperty("y", 0.0f);
                if (!(x == 0.0f && y == 0.0f))
                    pendingNodePositions[nid] = ImVec2(x, y);
            }
            uiPending = {};
        }

        // Draw module nodes (exactly once per logical module)
        // Graph is now always in consistent state since we rebuild at frame start
        std::unordered_set<int> drawnNodes;
        for (const auto& mod : synth->getModulesInfo())
        {
            const juce::uint32  lid = mod.first;
            const juce::String& type = mod.second;
            juce::String        moduleLabel = type + " [lid=" + juce::String((int)lid) + "]";
            NodePinHelpers      helpers;

            // Color-code modules by category (base colors)
            const auto moduleCategory = getModuleCategory(type);
            ImU32      baseTitleBarColor = getImU32ForCategory(moduleCategory);
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, baseTitleBarColor);
            ImNodes::PushColorStyle(
                ImNodesCol_TitleBarHovered, getImU32ForCategory(moduleCategory, true));
            ImNodes::PushColorStyle(
                ImNodesCol_TitleBarSelected, getImU32ForCategory(moduleCategory, true));

            // Determine the actual title bar color that will be used (checking overrides in order)
            ImU32 actualTitleBarColor = baseTitleBarColor;

            // Highlight nodes participating in the hovered link (overrides category color)
            const bool isHoveredSource =
                (hoveredLinkSrcId != 0 && hoveredLinkSrcId == (juce::uint32)lid);
            const bool isHoveredDest =
                (hoveredLinkDstId != 0 && hoveredLinkDstId == (juce::uint32)lid);
            if (isHoveredSource || isHoveredDest)
            {
                actualTitleBarColor = IM_COL32(255, 220, 0, 255);
                ImNodes::PushColorStyle(ImNodesCol_TitleBar, actualTitleBarColor);
            }

            // Visual feedback for muted nodes (overrides category color and hover)
            const bool isMuted = mutedNodeStates.count(lid) > 0;
            if (isMuted)
            {
                actualTitleBarColor = IM_COL32(80, 80, 80, 255);
                ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(8, 8));
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
                ImNodes::PushColorStyle(ImNodesCol_TitleBar, actualTitleBarColor);
            }

#if JUCE_DEBUG
            gLastRenderedNodeLabel = moduleLabel;
#endif
            ImNodes::BeginNode((int)lid);
#if JUCE_DEBUG
            ++gImNodesNodeDepth;
#endif
            ImNodes::BeginNodeTitleBar();

            // Calculate optimal text color based on title bar background color
            ImU32 optimalTextColor = ThemeUtils::getOptimalTextColor(actualTitleBarColor);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(optimalTextColor));

            // Special handling for reroute nodes: show dynamic type only (no 'Reroute' prefix)
            if (type.equalsIgnoreCase("reroute"))
            {
                if (auto* reroute =
                        dynamic_cast<RerouteModuleProcessor*>(synth->getModuleForLogical(lid)))
                {
                    PinDataType  passthroughType = reroute->getPassthroughType();
                    juce::String typeName;
                    switch (passthroughType)
                    {
                    case PinDataType::CV:
                        typeName = "CV";
                        break;
                    case PinDataType::Audio:
                        typeName = "Audio";
                        break;
                    case PinDataType::Gate:
                        typeName = "Gate";
                        break;
                    case PinDataType::Raw:
                        typeName = "Raw";
                        break;
                    case PinDataType::Video:
                        typeName = "Video";
                        break;
                    default:
                        typeName = "Audio";
                        break;
                    }
                    ImGui::TextUnformatted(typeName.toRawUTF8());
                }
                else
                {
                    ImGui::TextUnformatted(type.toRawUTF8());
                }
            }
            else
            {
                ImGui::TextUnformatted(type.toRawUTF8());
            }

            // Pop the text color we pushed
            ImGui::PopStyleColor();

            ImNodes::EndNodeTitleBar();

            // Get node content width - check if module has custom size, otherwise use default
            float nodeContentWidth = 240.0f; // Default width
            if (auto* mp = synth->getModuleForLogical(lid))
            {
                ImVec2 customSize = mp->getCustomNodeSize();
                if (customSize.x > 0.0f) // Module specified a custom width
                {
                    nodeContentWidth = customSize.x;
                }
            }

            if (synth != nullptr)
            {

                if (auto* mp = synth->getModuleForLogical(lid))
                {

                    // Helper to draw right-aligned text within a node's content width
                    // From imnodes examples (color_node_editor.cpp:353, save_load.cpp:77,
                    // multi_editor.cpp:73): Use ImGui::Indent() for right-alignment - this is the
                    // CORRECT ImNodes pattern!
                    auto rightLabelWithinWidth = [&](const char* txt, float nodeContentWidth) {
                        const ImVec2 textSize = ImGui::CalcTextSize(txt);

                        // Indent by (nodeWidth - textWidth) to right-align the text
                        // CRITICAL: Must call Unindent() to prevent indent from persisting!
                        const float indentAmount = juce::jmax(0.0f, nodeContentWidth - textSize.x);
                        ImGui::Indent(indentAmount);
                        ImGui::TextUnformatted(txt);
                        ImGui::Unindent(indentAmount); // Reset indent!
                    };
                    helpers.drawAudioInputPin = [&](const char* label, int channel) {
                        int attr = encodePinId({lid, channel, true});
                        seenAttrs.insert(attr);
                        availableAttrs.insert(attr);

                        // Get pin data type for color coding
                        PinID        pinId = {lid, channel, true, false, ""};
                        PinDataType  pinType = this->getPinDataTypeForPin(pinId);
                        unsigned int pinColor = this->getImU32ForType(pinType);

                        bool isConnected = connectedInputAttrs.count(attr) > 0;
                        ImNodes::PushColorStyle(
                            ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);

                        ImNodes::BeginInputAttribute(attr);
#if JUCE_DEBUG
                        ++gImNodesInputDepth;
#endif
                        ImGui::TextUnformatted(label);
                        ImNodes::EndInputAttribute();
#if JUCE_DEBUG
                        --gImNodesInputDepth;
                        jassert(gImNodesInputDepth >= 0);
#endif

                        // --- THIS IS THE DEFINITIVE FIX ---
                        // Get the bounding box of the pin circle that was just drawn.
                        ImVec2 pinMin = ImGui::GetItemRectMin();
                        ImVec2 pinMax = ImGui::GetItemRectMax();
                        // Calculate the exact center and cache it.
                        float centerX = (pinMin.x + pinMax.x) * 0.5f;
                        float centerY = (pinMin.y + pinMax.y) * 0.5f;
                        // Cache pin position in GRID SPACE
                        attrPositions[attr] = ImVec2(
                            centerX - lastCanvasP0.x - lastEditorPanning.x,
                            centerY - lastCanvasP0.y - lastEditorPanning.y);
                        // --- END OF FIX ---

                        ImNodes::PopColorStyle(); // Restore default color

                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            if (isConnected)
                            {
                                ThemeText("Connected", theme.text.active);
                                // Find which output this input is connected to and show source info
                                for (const auto& c : synth->getConnectionsInfo())
                                {
                                    bool isConnectedToThisPin =
                                        (!c.dstIsOutput && c.dstLogicalId == lid &&
                                         c.dstChan == channel) ||
                                        (c.dstIsOutput && lid == 0 && c.dstChan == channel);
                                    if (isConnectedToThisPin)
                                    {
                                        // CRASH FIX: Verify module exists before accessing it
                                        if (c.srcLogicalId != 0 && synth != nullptr)
                                        {
                                            bool moduleExists = false;
                                            for (const auto& modInfo : synth->getModulesInfo())
                                            {
                                                if (modInfo.first == c.srcLogicalId)
                                                {
                                                    moduleExists = true;
                                                    break;
                                                }
                                            }

                                            if (moduleExists)
                                            {
                                                if (auto* srcMod =
                                                        synth->getModuleForLogical(c.srcLogicalId))
                                                {
                                                    float value =
                                                        srcMod->getOutputChannelValue(c.srcChan);
                                                    ImGui::Text(
                                                        "From %u:%d", c.srcLogicalId, c.srcChan);
                                                    ImGui::Text("Value: %.3f", value);
                                                }
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                ThemeText("Not Connected", theme.text.disabled);
                            }
                            // Show pin data type
                            ImGui::Text("Type: %s", this->pinDataTypeToString(pinType));
                            ImGui::EndTooltip();
                        }
                    };

                    // NEW CLEAN OUTPUT PIN TEXT FUNCTION - FIXED SPACING
                    helpers.drawAudioOutputPin = [&](const char* label, int channel) {
                        const int attr = encodePinId({(juce::uint32)lid, channel, false});
                        seenAttrs.insert(attr);
                        availableAttrs.insert(attr);

                        PinID        pinId = {(juce::uint32)lid, channel, false, false, ""};
                        PinDataType  pinType = this->getPinDataTypeForPin(pinId);
                        unsigned int pinColor = this->getImU32ForType(pinType);
                        bool         isConnected = connectedOutputAttrs.count(attr) > 0;

                        ImNodes::PushColorStyle(
                            ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);

                        // EXACT OFFICIAL PATTERN: Text right-aligned, pin touches text edge
                        ImNodes::BeginOutputAttribute(attr);
#if JUCE_DEBUG
                        ++gImNodesOutputDepth;
#endif
                        const float label_width = ImGui::CalcTextSize(label).x;
                        ImGui::Indent(
                            nodeContentWidth - label_width); // Right-align to content width
                        ImGui::TextUnformatted(label);
                        ImGui::Unindent(nodeContentWidth - label_width);
                        ImNodes::EndOutputAttribute();
#if JUCE_DEBUG
                        --gImNodesOutputDepth;
                        jassert(gImNodesOutputDepth >= 0);
#endif

                        // Cache pin center
                        {
                            ImVec2 pinMin = ImGui::GetItemRectMin();
                            ImVec2 pinMax = ImGui::GetItemRectMax();
                            float  centerY = (pinMin.y + pinMax.y) * 0.5f;
                            float  x_pos = pinMax.x;
                            // Cache pin position in GRID SPACE
                            attrPositions[attr] = ImVec2(
                                x_pos - lastCanvasP0.x - lastEditorPanning.x,
                                centerY - lastCanvasP0.y - lastEditorPanning.y);
                        }

                        ImNodes::PopColorStyle();

                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            if (isConnected)
                            {
                                ThemeText("Connected", theme.text.active);
                            }
                            else
                            {
                                ThemeText("Not Connected", theme.text.disabled);
                            }
                            ImGui::Text("Type: %s", this->pinDataTypeToString(pinType));
                            if (auto* mp = synth->getModuleForLogical(lid))
                            {
                                float value = mp->getOutputChannelValue(channel);
                                ImGui::Text("Value: %.3f", value);
                            }
                            ImGui::EndTooltip();
                        }
                    };

                    // Manual layout: align output labels with pins using SameLine
                    helpers.drawParallelPins = [&](const char* inLabel,
                                                   int         inChannel,
                                                   const char* outLabel,
                                                   int         outChannel) {
                        ImGui::PushID((inChannel << 16) ^ outChannel ^ lid);

                        const float spacing = ImGui::GetStyle().ItemSpacing.x;
                        const float labelToPinGap = spacing * 0.3f;
                        const float rowStartX = ImGui::GetCursorPosX();

                        bool hasItemOnLine = false;

                        if (inLabel != nullptr)
                        {
                            int inAttr = encodePinId({lid, inChannel, true});
                            seenAttrs.insert(inAttr);
                            availableAttrs.insert(inAttr);
                            PinID        pinId = {lid, inChannel, true, false, ""};
                            PinDataType  pinType = this->getPinDataTypeForPin(pinId);
                            unsigned int pinColor = this->getImU32ForType(pinType);
                            bool         isConnected = connectedInputAttrs.count(inAttr) > 0;
                            ImNodes::PushColorStyle(
                                ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);
                            ImNodes::BeginInputAttribute(inAttr);
#if JUCE_DEBUG
                            ++gImNodesInputDepth;
#endif
                            ImGui::TextUnformatted(inLabel);
                            ImNodes::EndInputAttribute();
#if JUCE_DEBUG
                            --gImNodesInputDepth;
                            jassert(gImNodesInputDepth >= 0);
#endif
                            ImNodes::PopColorStyle();

                            // --- CACHE PIN POSITION FOR CUT GESTURE ---
                            {
                                ImVec2 pinMin = ImGui::GetItemRectMin();
                                ImVec2 pinMax = ImGui::GetItemRectMax();
                                float  centerY = (pinMin.y + pinMax.y) * 0.5f;
                                // Input pins are on the left
                                // Cache pin position in GRID SPACE
                                attrPositions[inAttr] = ImVec2(
                                    pinMin.x - lastCanvasP0.x - lastEditorPanning.x,
                                    centerY - lastCanvasP0.y - lastEditorPanning.y);
                            }

                            hasItemOnLine = true;
                        }

                        if (!hasItemOnLine && outLabel != nullptr)
                        {
                            ImGui::Dummy(ImVec2(0.0f, 0.0f));
                            hasItemOnLine = true;
                        }

                        if (outLabel != nullptr)
                        {
                            const float textW = ImGui::CalcTextSize(outLabel).x;
                            const float desiredStart =
                                rowStartX +
                                juce::jmax(0.0f, nodeContentWidth - textW - labelToPinGap);
                            if (hasItemOnLine)
                                ImGui::SameLine(0.0f, spacing);
                            ImGui::SetCursorPosX(desiredStart);

                            int outAttr = encodePinId({lid, outChannel, false});
                            seenAttrs.insert(outAttr);
                            availableAttrs.insert(outAttr);
                            PinID        pinId = {lid, outChannel, false, false, ""};
                            PinDataType  pinType = this->getPinDataTypeForPin(pinId);
                            unsigned int pinColor = this->getImU32ForType(pinType);
                            bool         isConnected = connectedOutputAttrs.count(outAttr) > 0;
                            ImNodes::PushColorStyle(
                                ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);
#if JUCE_DEBUG
                            ++gImNodesOutputDepth;
#endif
                            ImNodes::BeginOutputAttribute(outAttr);
                            ImGui::TextUnformatted(outLabel);
                            ImNodes::EndOutputAttribute();
#if JUCE_DEBUG
                            --gImNodesOutputDepth;
                            jassert(gImNodesOutputDepth >= 0);
#endif
                            ImNodes::PopColorStyle();

                            ImVec2      pinMin = ImGui::GetItemRectMin();
                            ImVec2      pinMax = ImGui::GetItemRectMax();
                            const float yCenter = pinMin.y + (pinMax.y - pinMin.y) * 0.5f;
                            const float xPos = pinMax.x;
                            // Cache pin position in GRID SPACE
                            attrPositions[outAttr] = ImVec2(
                                xPos - lastCanvasP0.x - lastEditorPanning.x,
                                yCenter - lastCanvasP0.y - lastEditorPanning.y);
                        }

                        if (inLabel == nullptr && outLabel == nullptr)
                            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));

                        ImGui::PopID();
                    };

                    // Draw input pin inline (just the pin circle) - used for inline modulation pins
                    // This allows modules to draw pins next to their sliders with SameLine()
                    helpers.drawInlineInputPin = [&](int channel) -> bool {
                        int attr = encodePinId({lid, channel, true});
                        seenAttrs.insert(attr);
                        availableAttrs.insert(attr);

                        // Get pin data type for color coding
                        PinID        pinId = {lid, channel, true, false, ""};
                        PinDataType  pinType = this->getPinDataTypeForPin(pinId);
                        unsigned int pinColor = this->getImU32ForType(pinType);

                        bool isConnected = connectedInputAttrs.count(attr) > 0;
                        ImNodes::PushColorStyle(
                            ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);

                        ImNodes::BeginInputAttribute(attr);
#if JUCE_DEBUG
                        ++gImNodesInputDepth;
#endif
                        // Use small dummy instead of text - just want the pin circle
                        ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight()));
                        ImNodes::EndInputAttribute();
#if JUCE_DEBUG
                        --gImNodesInputDepth;
                        jassert(gImNodesInputDepth >= 0);
#endif

                        // Cache pin position in GRID SPACE
                        ImVec2 pinMin = ImGui::GetItemRectMin();
                        ImVec2 pinMax = ImGui::GetItemRectMax();
                        float  centerX = (pinMin.x + pinMax.x) * 0.5f;
                        float  centerY = (pinMin.y + pinMax.y) * 0.5f;
                        attrPositions[attr] = ImVec2(
                            centerX - lastCanvasP0.x - lastEditorPanning.x,
                            centerY - lastCanvasP0.y - lastEditorPanning.y);

                        ImNodes::PopColorStyle();
                        return true;
                    };

                    // Check if an output pin has any connections (for smart collapsible sections)
                    helpers.isOutputPinConnected = [&](int busIndex, int channel) -> bool {
                        juce::ignoreUnused(busIndex); // Currently busIndex maps to channel directly
                        const int attr = encodePinId({(juce::uint32)lid, channel, false});
                        return connectedOutputAttrs.count(attr) > 0;
                    };

                    // --- DYNAMIC PIN FIX ---
                    // Add a new helper that uses dynamic pin information from modules
                    helpers.drawIoPins = [&](ModuleProcessor* module) {
                        if (!module)
                            return;
                        const auto logicalId = module->getLogicalId();
                        const auto moduleType = synth->getModuleTypeForLogical(logicalId);

                        if (module->usesCustomPinLayout())
                        {
                            module->drawIoPins(helpers);
                            return;
                        }

                        // 1. Get dynamic pins from the module itself.
                        auto dynamicInputs = module->getDynamicInputPins();
                        auto dynamicOutputs = module->getDynamicOutputPins();

                        // 2. Get static pins from the database as a fallback.
                        const auto& pinDb = getModulePinDatabase();
                        auto        pinInfoIt = pinDb.find(moduleType.toLowerCase());
                        const bool  hasStaticPinInfo = (pinInfoIt != pinDb.end());
                        const auto& staticPinInfo =
                            hasStaticPinInfo ? pinInfoIt->second : ModulePinInfo{};

                        // 3. If the module has dynamic pins, use the new system
                        const bool hasDynamicPins =
                            !dynamicInputs.empty() || !dynamicOutputs.empty();

                        if (hasDynamicPins)
                        {
                            // Draw inputs (dynamic if available, otherwise static)
                            if (!dynamicInputs.empty())
                            {
                                for (const auto& pin : dynamicInputs)
                                {
                                    helpers.drawAudioInputPin(pin.name.toRawUTF8(), pin.channel);
                                }
                            }
                            else
                            {
                                for (const auto& pin : staticPinInfo.audioIns)
                                {
                                    helpers.drawAudioInputPin(pin.name.toRawUTF8(), pin.channel);
                                }
                            }

                            // Draw outputs (dynamic if available, otherwise static)
                            if (!dynamicOutputs.empty())
                            {
                                for (const auto& pin : dynamicOutputs)
                                {
                                    helpers.drawAudioOutputPin(pin.name.toRawUTF8(), pin.channel);
                                }
                            }
                            else
                            {
                                for (const auto& pin : staticPinInfo.audioOuts)
                                {
                                    helpers.drawAudioOutputPin(pin.name.toRawUTF8(), pin.channel);
                                }
                            }
                        }
                        else
                        {
                            // 4. Otherwise, fall back to the module's custom drawIoPins
                            // implementation
                            module->drawIoPins(helpers);
                        }
                    };
                    // --- END OF DYNAMIC PIN FIX ---
#ifndef AUDIO_ONLY_BUILD
                    // Debug logging for ObjectDetectorModule (only once per second to reduce
                    // flooding)
                    if (auto* objDet = dynamic_cast<ObjectDetectorModule*>(mp))
                    {
                        static std::atomic<juce::int64>             lastLogTime{0};
                        static std::atomic<juce::pointer_sized_int> lastLoggedPtr{0};
                        juce::int64 currentTime = juce::Time::currentTimeMillis();
                        if (lastLoggedPtr.load() != (juce::pointer_sized_int)objDet ||
                            (currentTime - lastLogTime.load() > 1000))
                        {
                            juce::Logger::writeToLog(
                                "[UI][drawParametersInNode] About to call drawParametersInNode() "
                                "on ObjectDetectorModule (ptr=0x" +
                                juce::String::toHexString((juce::pointer_sized_int)objDet) +
                                ") logicalId=" + juce::String(lid));
                            lastLogTime.store(currentTime);
                            lastLoggedPtr.store((juce::pointer_sized_int)objDet);
                        }
                    }
#endif
                    ImGui::PushID((int)lid);
#if JUCE_DEBUG
                    ImGuiStackBalanceChecker parameterStackGuard;
                    ImNodesDepthSnapshot     depthSnapshot(moduleLabel + "::drawParametersInNode");
#endif

                    // This new lambda function checks if a parameter is being modulated
                    auto isParamModulated = [&](const juce::String& paramId) -> bool {
                        if (!synth)
                            return false;
                        if (auto* mp = synth->getModuleForLogical(lid))
                        {
                            int busIdx = -1, chInBus = -1;
                            // Use the new standardized routing API on the module itself
                            if (!mp->getParamRouting(paramId, busIdx, chInBus))
                                return false;

                            // Calculate the absolute channel index that the graph uses for this
                            // bus/channel pair
                            const int absoluteChannelIndex =
                                mp->getChannelIndexInProcessBlockBuffer(true, busIdx, chInBus);
                            if (absoluteChannelIndex < 0)
                                return false;

                            // Scan the simple graph connections for a match
                            for (const auto& c : synth->getConnectionsInfo())
                            {
                                if (c.dstLogicalId == lid && c.dstChan == absoluteChannelIndex)
                                    return true;
                            }
                        }
                        return false;
                    };

                    // Helper to read a live, modulated value if available (respects _mod alias)
                    auto getLiveValueOr = [&](const juce::String& paramId,
                                              float               fallback) -> float {
                        if (!synth)
                            return fallback;
                        if (auto* mp = synth->getModuleForLogical(lid))
                            return mp->getLiveParamValueFor(
                                paramId + "_mod", paramId + "_live", fallback);
                        return fallback;
                    };
                    // Create a new function that calls pushSnapshot
                    auto onModificationEnded = [&]() { this->pushSnapshot(); };
                    // --- SPECIAL RENDERING FOR SAMPLE LOADER ---
                    if (auto* sampleLoader = dynamic_cast<SampleLoaderModuleProcessor*>(mp))
                    {
                        // First, draw the standard parameters (buttons, sliders, etc.)
                        // We pass a modified onModificationEnded to avoid creating undo states
                        // while dragging.
                        sampleLoader->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded, &helpers);

                        // Now, handle the spectrogram texture and drawing
                        juce::OpenGLTexture* texturePtr = nullptr;
                        if (auto it = sampleLoaderTextureIds.find((int)lid);
                            it != sampleLoaderTextureIds.end())
                            texturePtr = it->second.get();

                        juce::Image spectrogram = sampleLoader->getSpectrogramImage();
                        if (spectrogram.isValid())
                        {
                            if (texturePtr == nullptr)
                            {
                                auto tex = std::make_unique<juce::OpenGLTexture>();
                                texturePtr = tex.get();
                                sampleLoaderTextureIds[(int)lid] = std::move(tex);
                            }
                            // Upload or update texture from JUCE image (handles format & parameters
                            // internally)
                            texturePtr->loadImage(spectrogram);

                            ImGui::Image(
                                (void*)(intptr_t)texturePtr->getTextureID(),
                                ImVec2(nodeContentWidth, 100.0f));

                            // Drag state is tracked per Sample Loader node to avoid cross-node
                            // interference
                            static std::unordered_map<int, int>
                                 draggedHandleByNode; // lid -> -1,0,1
                            int& draggedHandle = draggedHandleByNode[(int)lid];
                            if (draggedHandle != 0 && draggedHandle != 1)
                                draggedHandle = -1;
                            ImGui::SetCursorScreenPos(ImGui::GetItemRectMin());
                            ImGui::InvisibleButton(
                                "##spectrogram_interaction", ImVec2(nodeContentWidth, 100.0f));

                            auto*        drawList = ImGui::GetWindowDrawList();
                            const ImVec2 rectMin = ImGui::GetItemRectMin();
                            const ImVec2 rectMax = ImGui::GetItemRectMax();

                            float startNorm =
                                sampleLoader->getAPVTS().getRawParameterValue("rangeStart")->load();
                            float endNorm =
                                sampleLoader->getAPVTS().getRawParameterValue("rangeEnd")->load();

                            // Use live telemetry values when modulated
                            startNorm = sampleLoader->getLiveParamValueFor(
                                "rangeStart_mod", "rangeStart_live", startNorm);
                            endNorm = sampleLoader->getLiveParamValueFor(
                                "rangeEnd_mod", "rangeEnd_live", endNorm);

                            // Visual guard even when modulated
                            const float kMinGap = 0.001f;
                            startNorm = juce::jlimit(0.0f, 1.0f, startNorm);
                            endNorm = juce::jlimit(0.0f, 1.0f, endNorm);
                            if (startNorm >= endNorm)
                            {
                                if (startNorm <= 1.0f - kMinGap)
                                    endNorm = juce::jmin(1.0f, startNorm + kMinGap);
                                else
                                    startNorm = juce::jmax(0.0f, endNorm - kMinGap);
                            }

                            // --- FIX FOR BUG 1: Separate modulation checks for each handle ---
                            bool startIsModulated = isParamModulated("rangeStart_mod");
                            bool endIsModulated = isParamModulated("rangeEnd_mod");

                            const bool itemHovered = ImGui::IsItemHovered();
                            const bool itemActive = ImGui::IsItemActive();
                            if (itemHovered)
                            {
                                ImVec2 mousePos = ImGui::GetMousePos();
                                float  startHandleX = rectMin.x + startNorm * nodeContentWidth;
                                float  endHandleX = rectMin.x + endNorm * nodeContentWidth;

                                bool canDragStart =
                                    !startIsModulated && (std::abs(mousePos.x - startHandleX) < 5);
                                bool canDragEnd =
                                    !endIsModulated && (std::abs(mousePos.x - endHandleX) < 5);

                                if (draggedHandle == -1 && (canDragStart || canDragEnd))
                                {
                                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                                }

                                if (ImGui::IsItemClicked())
                                {
                                    // Only allow dragging to start if the corresponding handle is
                                    // not modulated
                                    if (canDragStart && canDragEnd)
                                        draggedHandle = (std::abs(mousePos.x - startHandleX) <
                                                         std::abs(mousePos.x - endHandleX))
                                                            ? 0
                                                            : 1;
                                    else if (canDragStart)
                                        draggedHandle = 0;
                                    else if (canDragEnd)
                                        draggedHandle = 1;
                                }
                            }

                            if (itemActive && ImGui::IsMouseReleased(0))
                            {
                                if (draggedHandle != -1)
                                    onModificationEnded();
                                draggedHandle = -1;
                            }

                            // Handle the drag update, checking the specific modulation flag for the
                            // active handle
                            if (itemActive && draggedHandle != -1 && ImGui::IsMouseDragging(0))
                            {
                                float newNormX = juce::jlimit(
                                    0.0f,
                                    1.0f,
                                    (ImGui::GetMousePos().x - rectMin.x) / nodeContentWidth);
                                if (draggedHandle == 0 && !startIsModulated)
                                {
                                    // Guard: start cannot be >= end
                                    startNorm = juce::jmin(newNormX, endNorm - 0.001f);
                                    sampleLoader->getAPVTS()
                                        .getParameter("rangeStart")
                                        ->setValueNotifyingHost(startNorm);
                                }
                                else if (draggedHandle == 1 && !endIsModulated)
                                {
                                    // Guard: end cannot be <= start
                                    endNorm = juce::jmax(newNormX, startNorm + 0.001f);
                                    sampleLoader->getAPVTS()
                                        .getParameter("rangeEnd")
                                        ->setValueNotifyingHost(endNorm);
                                }
                            }

                            float startX = rectMin.x + startNorm * nodeContentWidth;
                            float endX = rectMin.x + endNorm * nodeContentWidth;
                            drawList->AddRectFilled(
                                rectMin, ImVec2(startX, rectMax.y), IM_COL32(0, 0, 0, 120));
                            drawList->AddRectFilled(
                                ImVec2(endX, rectMin.y), rectMax, IM_COL32(0, 0, 0, 120));

                            // Draw range handles (yellow lines)
                            drawList->AddLine(
                                ImVec2(startX, rectMin.y),
                                ImVec2(startX, rectMax.y),
                                IM_COL32(255, 255, 0, 255),
                                3.0f);
                            drawList->AddLine(
                                ImVec2(endX, rectMin.y),
                                ImVec2(endX, rectMax.y),
                                IM_COL32(255, 255, 0, 255),
                                3.0f);

                            // === PLAYHEAD INDICATOR (red line) ===
                            // Get current playhead position (absolute 0-1 across full sample)
                            float positionAbs =
                                sampleLoader->getAPVTS().getRawParameterValue("position")->load();

                            // Use live telemetry value if available (shows real-time playback
                            // position)
                            positionAbs = sampleLoader->getLiveParamValueFor(
                                "position_mod", "position_live", positionAbs);

                            // CRITICAL: Playhead must ALWAYS be clamped to range boundaries
                            // [startNorm, endNorm] Position is stored as absolute (0-1 across full
                            // sample), but playhead cannot be outside range
                            positionAbs = juce::jlimit(startNorm, endNorm, positionAbs);

                            // Convert absolute position to spectrogram position
                            float positionInSpectrogram =
                                positionAbs; // Direct mapping: absolute position in sample =
                                             // position in spectrogram

                            // Calculate playhead X position
                            float playheadX = rectMin.x + positionInSpectrogram * nodeContentWidth;

                            // Draw red playhead line (2px width for visibility)
                            drawList->AddLine(
                                ImVec2(playheadX, rectMin.y),
                                ImVec2(playheadX, rectMax.y),
                                IM_COL32(255, 0, 0, 255),
                                2.0f);
                        }
                    }
                    // --- SPECIAL RENDERING FOR AUDIO INPUT (MULTI-CHANNEL) ---
                    else if (auto* audioIn = dynamic_cast<AudioInputModuleProcessor*>(mp))
                    {
                        auto& apvts = audioIn->getAPVTS();

                        // --- Device Type Selection (ASIO, Windows Audio, etc.) ---
                        const auto& deviceTypes = deviceManager.getAvailableDeviceTypes();

                        // Get per-instance device type (not global)
                        juce::String instanceDeviceType = audioIn->getSelectedDeviceType();
                        juce::String globalDeviceType = deviceManager.getCurrentAudioDeviceType();

                        // Use instance type if set, otherwise use global as default
                        juce::String currentDeviceType =
                            instanceDeviceType.isEmpty() ? globalDeviceType : instanceDeviceType;

                        // Build device type list
                        juce::StringArray deviceTypeNames;
                        for (int i = 0; i < deviceTypes.size(); ++i)
                        {
                            if (auto* type = deviceTypes[i])
                                deviceTypeNames.add(type->getTypeName());
                        }

                        std::vector<const char*> deviceTypeItems;
                        for (const auto& name : deviceTypeNames)
                            deviceTypeItems.push_back(name.toRawUTF8());

                        int currentDeviceTypeIndex = deviceTypeNames.indexOf(currentDeviceType);
                        if (currentDeviceTypeIndex < 0)
                            currentDeviceTypeIndex = 0;

                        ImGui::PushItemWidth(nodeContentWidth);

                        // Get the current global device type (user sets this in Audio Settings)
                        juce::String currentGlobalDeviceType =
                            deviceManager.getCurrentAudioDeviceType();
                        if (currentGlobalDeviceType.isEmpty() && deviceTypes.size() > 0)
                        {
                            if (auto* firstType = deviceTypes[0])
                                currentGlobalDeviceType = firstType->getTypeName();
                        }

                        // CRITICAL: Check if we're using ASIO
                        bool isASIO = (currentGlobalDeviceType == "ASIO");

                        if (isASIO)
                        {
                            // ASIO: Cannot select separate device - uses global input
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "ASIO Mode - Using Global Input");
                            ImGui::TextWrapped(
                                "ASIO drivers don't allow multiple connections. This node uses the "
                                "global audio input from Audio Settings.");

                            if (auto* globalDevice = deviceManager.getCurrentAudioDevice())
                            {
                                ImGui::Text(
                                    "Global Device: %s", globalDevice->getName().toRawUTF8());
                                ImGui::Text(
                                    "Sample Rate: %.0f Hz", globalDevice->getCurrentSampleRate());
                                ImGui::Text(
                                    "Buffer Size: %d samples",
                                    globalDevice->getCurrentBufferSizeSamples());
                            }
                            else
                            {
                                ImGui::TextColored(
                                    ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No global ASIO device open!");
                                ImGui::TextWrapped(
                                    "Please open an ASIO device in Audio Settings first.");
                            }

                            // Clear any stored device name to prevent conflicts
                            if (!audioIn->getSelectedDeviceName().isEmpty())
                            {
                                audioIn->setSelectedDeviceName("");
                            }

                            // Still allow selecting which input channels to use from the global
                            // device (This is handled by the channel mapping below)
                        }
                        else
                        {
                            // Windows Audio: Can select separate input devices
                            // Find the device type object for the current global type
                            juce::AudioIODeviceType* currentDeviceTypeObj = nullptr;
                            for (int i = 0; i < deviceTypes.size(); ++i)
                            {
                                if (auto* type = deviceTypes[i])
                                {
                                    if (type->getTypeName() == currentGlobalDeviceType)
                                    {
                                        currentDeviceTypeObj = type;
                                        break;
                                    }
                                }
                            }

                            // If node has a stored device type, use that (for backward
                            // compatibility) Otherwise use the global device type
                            juce::String nodeDeviceType = audioIn->getSelectedDeviceType();
                            if (nodeDeviceType.isEmpty())
                            {
                                nodeDeviceType = currentGlobalDeviceType;
                                audioIn->setSelectedDeviceType(nodeDeviceType);
                            }

                            // If node's device type doesn't match global, update it to match global
                            if (nodeDeviceType != currentGlobalDeviceType)
                            {
                                nodeDeviceType = currentGlobalDeviceType;
                                audioIn->setSelectedDeviceType(nodeDeviceType);

                                // Re-find device type object
                                currentDeviceTypeObj = nullptr;
                                for (int i = 0; i < deviceTypes.size(); ++i)
                                {
                                    if (auto* type = deviceTypes[i])
                                    {
                                        if (type->getTypeName() == nodeDeviceType)
                                        {
                                            currentDeviceTypeObj = type;
                                            break;
                                        }
                                    }
                                }
                            }

                            // Scan for devices in current type
                            if (currentDeviceTypeObj)
                                currentDeviceTypeObj->scanForDevices();

                            // --- Input Device Selection (Windows Audio only) ---
                            juce::StringArray availableInputDevices;
                            if (currentDeviceTypeObj)
                            {
                                availableInputDevices = currentDeviceTypeObj->getDeviceNames(true);
                            }
                            std::vector<const char*> inputDeviceItems;
                            for (const auto& name : availableInputDevices)
                                inputDeviceItems.push_back(name.toRawUTF8());

                            // Get the per-instance device name
                            juce::String instanceDeviceName = audioIn->getSelectedDeviceName();

                            int currentInputDeviceIndex =
                                availableInputDevices.indexOf(instanceDeviceName);
                            if (currentInputDeviceIndex < 0)
                                currentInputDeviceIndex = 0;

                            if (ImGui::Combo(
                                    "Input Device",
                                    &currentInputDeviceIndex,
                                    inputDeviceItems.data(),
                                    (int)inputDeviceItems.size()))
                            {
                                if (currentInputDeviceIndex < availableInputDevices.size())
                                {
                                    // Store per-instance device name and type
                                    juce::String selectedDevice =
                                        availableInputDevices[currentInputDeviceIndex];
                                    audioIn->setSelectedDeviceType(
                                        nodeDeviceType); // Use current global type
                                    audioIn->setSelectedDeviceName(
                                        selectedDevice); // This will call updateDevice()

                                    onModificationEnded();
                                }
                            }
                            // Scroll-edit for Input Device combo
                            if (!availableInputDevices.isEmpty() && ImGui::IsItemHovered())
                            {
                                const float wheel = ImGui::GetIO().MouseWheel;
                                if (wheel != 0.0f)
                                {
                                    const int maxIndex = (int)availableInputDevices.size() - 1;
                                    int       newIndex = juce::jlimit(
                                        0,
                                        maxIndex,
                                        currentInputDeviceIndex + (wheel > 0.0f ? -1 : 1));
                                    if (newIndex != currentInputDeviceIndex)
                                    {
                                        currentInputDeviceIndex = newIndex;
                                        juce::String selectedDevice =
                                            availableInputDevices[newIndex];
                                        audioIn->setSelectedDeviceType(nodeDeviceType);
                                        audioIn->setSelectedDeviceName(selectedDevice);
                                        onModificationEnded();
                                    }
                                }
                            }
                        }

                        // --- Channel Count ---
                        auto* numChannelsParam = static_cast<juce::AudioParameterInt*>(
                            apvts.getParameter("numChannels"));
                        int numChannels = numChannelsParam->get();
                        if (ImGui::SliderInt(
                                "Channels",
                                &numChannels,
                                1,
                                AudioInputModuleProcessor::MAX_CHANNELS))
                        {
                            *numChannelsParam = numChannels;
                            onModificationEnded();
                        }
                        // Scroll-edit support for Channels (integer parameter)
                        if (ImGui::IsItemHovered())
                        {
                            const float wheel = ImGui::GetIO().MouseWheel;
                            if (wheel != 0.0f)
                            {
                                int newVal = numChannelsParam->get() + (wheel > 0.0f ? 1 : -1);
                                newVal = juce::jlimit(
                                    1, AudioInputModuleProcessor::MAX_CHANNELS, newVal);
                                if (newVal != numChannelsParam->get())
                                {
                                    *numChannelsParam = newVal;
                                    onModificationEnded();
                                }
                            }
                        }

                        // --- Threshold Sliders ---
                        auto* gateThreshParam = static_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("gateThreshold"));
                        float gateThresh = gateThreshParam->get();
                        if (ImGui::SliderFloat("Gate Threshold", &gateThresh, 0.0f, 1.0f, "%.3f"))
                        {
                            *gateThreshParam = gateThresh;
                            onModificationEnded();
                        }
                        // Scroll-edit support for Gate Threshold
                        if (ImGui::IsItemHovered())
                        {
                            const float wheel = ImGui::GetIO().MouseWheel;
                            if (wheel != 0.0f)
                            {
                                const float step = 0.01f;
                                float       newVal = juce::jlimit(
                                    0.0f,
                                    1.0f,
                                    gateThreshParam->get() + (wheel > 0.0f ? step : -step));
                                if (newVal != gateThreshParam->get())
                                {
                                    *gateThreshParam = newVal;
                                    onModificationEnded();
                                }
                            }
                        }

                        auto* trigThreshParam = static_cast<juce::AudioParameterFloat*>(
                            apvts.getParameter("triggerThreshold"));
                        float trigThresh = trigThreshParam->get();
                        if (ImGui::SliderFloat(
                                "Trigger Threshold", &trigThresh, 0.0f, 1.0f, "%.3f"))
                        {
                            *trigThreshParam = trigThresh;
                            onModificationEnded();
                        }
                        // Scroll-edit support for Trigger Threshold
                        if (ImGui::IsItemHovered())
                        {
                            const float wheel = ImGui::GetIO().MouseWheel;
                            if (wheel != 0.0f)
                            {
                                const float step = 0.01f;
                                float       newVal = juce::jlimit(
                                    0.0f,
                                    1.0f,
                                    trigThreshParam->get() + (wheel > 0.0f ? step : -step));
                                if (newVal != trigThreshParam->get())
                                {
                                    *trigThreshParam = newVal;
                                    onModificationEnded();
                                }
                            }
                        }

                        ImGui::PopItemWidth();

                        // --- Dynamic Channel Selectors & VU Meters ---
                        auto hardwareChannels =
                            deviceManager.getCurrentAudioDevice()
                                ? deviceManager.getCurrentAudioDevice()->getInputChannelNames()
                                : juce::StringArray{};
                        if (!hardwareChannels.isEmpty())
                        {
                            std::vector<const char*> hwChannelItems;
                            for (const auto& name : hardwareChannels)
                                hwChannelItems.push_back(name.toRawUTF8());

                            for (int i = 0; i < numChannels; ++i)
                            {
                                auto* mappingParam = static_cast<juce::AudioParameterInt*>(
                                    apvts.getParameter("channelMap" + juce::String(i)));
                                int selectedHwChannel = mappingParam->get();
                                selectedHwChannel = juce::jlimit(
                                    0, (int)hwChannelItems.size() - 1, selectedHwChannel);

                                ImGui::PushID(i);
                                ImGui::PushItemWidth(nodeContentWidth * 0.6f);
                                if (ImGui::Combo(
                                        ("Input for Out " + juce::String(i + 1)).toRawUTF8(),
                                        &selectedHwChannel,
                                        hwChannelItems.data(),
                                        (int)hwChannelItems.size()))
                                {
                                    *mappingParam = selectedHwChannel;
                                    std::vector<int> newMapping(numChannels);
                                    for (int j = 0; j < numChannels; ++j)
                                    {
                                        auto* p = static_cast<juce::AudioParameterInt*>(
                                            apvts.getParameter("channelMap" + juce::String(j)));
                                        newMapping[j] = p->get();
                                    }
                                    synth->setAudioInputChannelMapping(
                                        synth->getNodeIdForLogical(lid), newMapping);
                                    onModificationEnded();
                                }
                                // Scroll-edit support for channel mapping combo
                                if (ImGui::IsItemHovered())
                                {
                                    const float wheel = ImGui::GetIO().MouseWheel;
                                    if (wheel != 0.0f)
                                    {
                                        const int maxIndex = (int)hwChannelItems.size() - 1;
                                        int       newIndex = juce::jlimit(
                                            0,
                                            maxIndex,
                                            selectedHwChannel + (wheel > 0.0f ? -1 : 1));
                                        if (newIndex != selectedHwChannel)
                                        {
                                            selectedHwChannel = newIndex;
                                            *mappingParam = selectedHwChannel;

                                            std::vector<int> newMapping(numChannels);
                                            for (int j = 0; j < numChannels; ++j)
                                            {
                                                auto* p = static_cast<juce::AudioParameterInt*>(
                                                    apvts.getParameter(
                                                        "channelMap" + juce::String(j)));
                                                newMapping[j] = p->get();
                                            }
                                            synth->setAudioInputChannelMapping(
                                                synth->getNodeIdForLogical(lid), newMapping);
                                            onModificationEnded();
                                        }
                                    }
                                }
                                ImGui::PopItemWidth();

                                ImGui::SameLine();

                                // --- VU Meter with Threshold Lines ---
                                float  level = (i < (int)audioIn->channelLevels.size() &&
                                               audioIn->channelLevels[i])
                                                   ? audioIn->channelLevels[i]->load()
                                                   : 0.0f;
                                ImVec2 meterSize(
                                    nodeContentWidth * 0.38f,
                                    ImGui::GetTextLineHeightWithSpacing() * 0.8f);
                                ImGui::ProgressBar(level, meterSize, "");

                                // Draw threshold lines on top of the progress bar
                                ImVec2      p_min = ImGui::GetItemRectMin();
                                ImVec2      p_max = ImGui::GetItemRectMax();
                                ImDrawList* draw_list = ImGui::GetWindowDrawList();

                                // Gate Threshold (Yellow)
                                float gateLineX = p_min.x + gateThresh * (p_max.x - p_min.x);
                                draw_list->AddLine(
                                    ImVec2(gateLineX, p_min.y),
                                    ImVec2(gateLineX, p_max.y),
                                    IM_COL32(255, 255, 0, 200),
                                    2.0f);

                                // Trigger Threshold (Orange)
                                float trigLineX = p_min.x + trigThresh * (p_max.x - p_min.x);
                                draw_list->AddLine(
                                    ImVec2(trigLineX, p_min.y),
                                    ImVec2(trigLineX, p_max.y),
                                    IM_COL32(255, 165, 0, 200),
                                    2.0f);

                                ImGui::PopID();
                            }
                        }
                    }
#ifndef AUDIO_ONLY_BUILD
                    // --- SPECIAL RENDERING FOR OPENCV MODULES (WITH VIDEO FEED) ---
                    else if (auto* webcamModule = dynamic_cast<WebcamLoaderModule*>(mp))
                    {
                        juce::Image frame = webcamModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                // Calculate aspect ratio dynamically from the actual frame
                                // dimensions
                                float nativeWidth = (float)frame.getWidth();
                                float nativeHeight = (float)frame.getHeight();

                                // Preserve the video's native aspect ratio (handles portrait,
                                // landscape, square, etc.)
                                float aspectRatio = (nativeWidth > 0.0f)
                                                        ? nativeHeight / nativeWidth
                                                        : 0.75f; // Default to 4:3

                                // Width is fixed at itemWidth (480px for video modules), height
                                // scales proportionally
                                ImVec2 renderSize =
                                    ImVec2(nodeContentWidth, nodeContentWidth * aspectRatio);

                                // Flip Y-coordinates to fix upside-down video (OpenCV uses top-left
                                // origin, OpenGL uses bottom-left)
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        webcamModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* videoFileModule = dynamic_cast<VideoFileLoaderModule*>(mp))
                    {
                        juce::Image frame = videoFileModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                // Calculate aspect ratio dynamically from the actual frame
                                // dimensions
                                float nativeWidth = (float)frame.getWidth();
                                float nativeHeight = (float)frame.getHeight();

                                // Preserve the video's native aspect ratio (handles portrait,
                                // landscape, square, etc.)
                                float aspectRatio = (nativeWidth > 0.0f)
                                                        ? nativeHeight / nativeWidth
                                                        : 0.75f; // Default to 4:3

                                // Width is fixed at itemWidth (480px for video modules), height
                                // scales proportionally
                                ImVec2 renderSize =
                                    ImVec2(nodeContentWidth, nodeContentWidth * aspectRatio);

                                // Flip Y-coordinates to fix upside-down video (OpenCV uses top-left
                                // origin, OpenGL uses bottom-left)
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        videoFileModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* ndiModule = dynamic_cast<NdiReceiverModule*>(mp))
                    {
                        juce::Image frame = ndiModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                // Calculate aspect ratio dynamically from the actual frame
                                // dimensions
                                float nativeWidth = (float)frame.getWidth();
                                float nativeHeight = (float)frame.getHeight();

                                // Preserve the video's native aspect ratio
                                float aspectRatio = (nativeWidth > 0.0f)
                                                        ? nativeHeight / nativeWidth
                                                        : 0.5625f; // Default to 16:9

                                // Width is fixed at itemWidth, height scales proportionally
                                ImVec2 renderSize =
                                    ImVec2(nodeContentWidth, nodeContentWidth * aspectRatio);

                                // Flip Y-coordinates to fix upside-down video
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        ndiModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* youtubeModule = dynamic_cast<YouTubeSourceModule*>(mp))
                    {
                        juce::Image frame = youtubeModule->getLatestVideoFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                float  nativeWidth = (float)frame.getWidth();
                                float  nativeHeight = (float)frame.getHeight();
                                float  aspectRatio = (nativeWidth > 0.0f)
                                                         ? nativeHeight / nativeWidth
                                                         : 0.5625f; // Default to 16:9
                                ImVec2 renderSize =
                                    ImVec2(nodeContentWidth, nodeContentWidth * aspectRatio);
                                // Flip Y for OpenGL
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        youtubeModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* movementModule = dynamic_cast<MovementDetectorModule*>(mp))
                    {
                        // MovementDetectorModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        movementModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* poseModule = dynamic_cast<PoseEstimatorModule*>(mp))
                    {
                        // PoseEstimatorModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        poseModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* colorModule = dynamic_cast<ColorTrackerModule*>(mp))
                    {
                        // ColorTrackerModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        colorModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* contourModule = dynamic_cast<ContourDetectorModule*>(mp))
                    {
                        // ContourDetectorModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        contourModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* objModule = dynamic_cast<ObjectDetectorModule*>(mp))
                    {
                        // ObjectDetectorModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        // (Logging moved inside drawParametersInNode to reduce flooding)
                        objModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (false) // Keep old code commented below for reference
                    {
                        juce::Image frame = objModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            auto* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                float ar = (float)frame.getHeight() /
                                           juce::jmax(1.0f, (float)frame.getWidth());
                                ImVec2 size(nodeContentWidth, nodeContentWidth * ar);
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    size,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        objModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* handModule = dynamic_cast<HandTrackerModule*>(mp))
                    {
                        // HandTrackerModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        handModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* faceModule = dynamic_cast<FaceTrackerModule*>(mp))
                    {
                        // FaceTrackerModule handles its own video preview rendering with zone
                        // interaction in drawParametersInNode
                        faceModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* fxModule = dynamic_cast<VideoFXModule*>(mp))
                    {
                        juce::Image frame = fxModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                float  nativeWidth = (float)frame.getWidth();
                                float  nativeHeight = (float)frame.getHeight();
                                float  aspectRatio = (nativeWidth > 0.0f)
                                                         ? nativeHeight / nativeWidth
                                                         : 0.75f; // Default to 4:3
                                ImVec2 renderSize =
                                    ImVec2(nodeContentWidth, nodeContentWidth * aspectRatio);
                                // Flip Y-coords for correct orientation
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        // Now draw the regular parameters below the video
                        fxModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* compositorModule = dynamic_cast<VideoCompositorModule*>(mp))
                    {
                        juce::Image frame = compositorModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                float  nativeWidth = (float)frame.getWidth();
                                float  nativeHeight = (float)frame.getHeight();
                                float  aspectRatio = (nativeWidth > 0.0f)
                                                         ? nativeHeight / nativeWidth
                                                         : 0.75f; // Default to 4:3
                                ImVec2 renderSize =
                                    ImVec2(nodeContentWidth, nodeContentWidth * aspectRatio);
                                // Flip Y-coords for correct orientation
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));
                            }
                        }
                        // Now draw the regular parameters below the video
                        compositorModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (
                        auto* drawImpactModule = dynamic_cast<VideoDrawImpactModuleProcessor*>(mp))
                    {
                        juce::Image frame = drawImpactModule->getLatestFrame();
                        if (!frame.isNull())
                        {
                            if (visionModuleTextures.find((int)lid) == visionModuleTextures.end())
                            {
                                visionModuleTextures[(int)lid] =
                                    std::make_unique<juce::OpenGLTexture>();
                            }
                            juce::OpenGLTexture* texture = visionModuleTextures[(int)lid].get();
                            texture->loadImage(frame);
                            if (texture->getTextureID() != 0)
                            {
                                float nativeWidth = (float)frame.getWidth();
                                float nativeHeight = (float)frame.getHeight();
                                float aspectRatio = (nativeWidth > 0.0f)
                                                        ? nativeHeight / nativeWidth
                                                        : 0.75f; // Default to 4:3

                                const float baseWidth =
                                    (drawImpactModule->getCustomNodeSize().x > 0.0f)
                                        ? drawImpactModule->getCustomNodeSize().x
                                        : nodeContentWidth;
                                ImVec2 renderSize(baseWidth, baseWidth * aspectRatio);

                                const float maxPreviewHeight = 260.0f;
                                if (renderSize.y > maxPreviewHeight)
                                {
                                    float scale = maxPreviewHeight / renderSize.y;
                                    renderSize.y = maxPreviewHeight;
                                    renderSize.x *= scale;
                                }

                                if (renderSize.x > nodeContentWidth)
                                {
                                    float scale = nodeContentWidth / renderSize.x;
                                    renderSize.x = nodeContentWidth;
                                    renderSize.y *= scale;
                                }

                                const ImVec2 previewTopLeft = ImGui::GetCursorScreenPos();
                                const float  xOffset =
                                    juce::jmax(0.0f, (nodeContentWidth - renderSize.x) * 0.5f);
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + xOffset);

                                // Flip Y-coords for correct orientation
                                ImGui::Image(
                                    (void*)(intptr_t)texture->getTextureID(),
                                    renderSize,
                                    ImVec2(0, 1),
                                    ImVec2(1, 0));

                                // Overlay invisible button to capture mouse (prevents node dragging
                                // while drawing)
                                const ImVec2 imageMin = ImGui::GetItemRectMin();
                                const ImVec2 imageMax = ImGui::GetItemRectMax();
                                const float  imageWidth = imageMax.x - imageMin.x;
                                const float  imageHeight = imageMax.y - imageMin.y;
                                const ImVec2 cursorAfterImage = ImGui::GetCursorScreenPos();
                                ImGui::SetCursorScreenPos(imageMin);
                                ImGui::PushID("video_draw_impact_canvas");
                                ImGui::InvisibleButton(
                                    "canvas",
                                    renderSize,
                                    ImGuiButtonFlags_MouseButtonLeft |
                                        ImGuiButtonFlags_MouseButtonRight);
                                ImGui::PopID();
                                const bool isHovered = ImGui::IsItemHovered();
                                const bool isActive = ImGui::IsItemActive();

                                ImVec2 nextPos = cursorAfterImage;
                                nextPos.x = previewTopLeft.x;
                                ImGui::SetCursorScreenPos(nextPos);
                                const bool leftDown =
                                    isActive && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                                const bool rightDown =
                                    isActive && ImGui::IsMouseDown(ImGuiMouseButton_Right);
                                const bool eitherDown = leftDown || rightDown;

                                if (imageWidth > 0.0f && imageHeight > 0.0f &&
                                    frame.getWidth() > 0 && frame.getHeight() > 0)
                                {
                                    if (isHovered)
                                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                                    if (eitherDown)
                                    {
                                        const ImVec2 mousePos = ImGui::GetIO().MousePos;
                                        float        normX = juce::jlimit(
                                            0.0f, 1.0f, (mousePos.x - imageMin.x) / imageWidth);
                                        float normY = juce::jlimit(
                                            0.0f, 1.0f, (mousePos.y - imageMin.y) / imageHeight);

                                        const int pixelX = juce::jlimit(
                                            0,
                                            frame.getWidth() - 1,
                                            juce::roundToInt(normX * (frame.getWidth() - 1)));
                                        const int pixelY = juce::jlimit(
                                            0,
                                            frame.getHeight() - 1,
                                            juce::roundToInt(normY * (frame.getHeight() - 1)));

                                        drawImpactModule->enqueueDrawPointFromUi(
                                            pixelX, pixelY, rightDown);
                                    }
                                    else if (!isActive)
                                    {
                                        drawImpactModule->endUiStroke();
                                    }
                                }
                                else if (!isActive)
                                {
                                    drawImpactModule->endUiStroke();
                                }
                            }
                        }
                        // Now draw the regular parameters below the video
                        drawImpactModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
                    else if (auto* cropVideoModule = dynamic_cast<CropVideoModule*>(mp))
                    {
                        // CropVideoModule handles its own preview rendering with interaction in
                        // drawParametersInNode
                        cropVideoModule->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded);
                    }
#endif
                    else
                    {
                        mp->drawParametersInNode(
                            nodeContentWidth, isParamModulated, onModificationEnded, &helpers);
                    }
#if JUCE_DEBUG
                    parameterStackGuard.validate(moduleLabel + "::drawParametersInNode");
#endif
                    ImGui::Spacing();
                    ImGui::PopID();
                }
            }

            // IO per module type via helpers

            // Delegate per-module IO pin drawing
            if (synth != nullptr)
                if (auto* mp = synth->getModuleForLogical(lid))
                {
#if JUCE_DEBUG
                    ImGuiStackBalanceChecker ioStackGuard;
                    ImNodesDepthSnapshot     ioDepthSnapshot(moduleLabel + "::drawIoPins");
#endif
                    helpers.drawIoPins(mp);
#if JUCE_DEBUG
                    ioStackGuard.validate(
                        type + " [lid=" + juce::String((int)lid) + "]::drawIoPins");
#endif
                }

            // Optional per-node right-click popup
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            {
                selectedLogicalId = (int)lid;
                ImGui::OpenPopup("NodeActionPopup");
            }

            // Legacy per-type IO drawing removed; delegated to module implementations via helpers

            ImNodes::EndNode();
#if JUCE_DEBUG
            --gImNodesNodeDepth;
            jassert(gImNodesNodeDepth >= 0);
#endif

            // Cache position for snapshot safety
            // Graph is always in consistent state since we rebuild at frame start
            lastKnownNodePositions[(int)lid] = ImNodes::GetNodeGridSpacePos((int)lid);

            // Pop muted node styles (in reverse order of push)
            if (isMuted)
            {
                ImNodes::PopColorStyle(); // Mute TitleBar
                ImGui::PopStyleVar();     // Alpha
                ImNodes::PopStyleVar();   // NodePadding
            }

            // Pop hover highlight color
            if (isHoveredSource || isHoveredDest)
                ImNodes::PopColorStyle(); // Hover TitleBar

            // Pop category colors (in reverse order of push)
            ImNodes::PopColorStyle(); // TitleBarSelected
            ImNodes::PopColorStyle(); // TitleBarHovered
            ImNodes::PopColorStyle(); // TitleBar

            // Apply pending placement if queued
            if (auto itS = pendingNodeScreenPositions.find((int)lid);
                itS != pendingNodeScreenPositions.end())
            {
                ImNodes::SetNodeScreenSpacePos((int)lid, itS->second);
                pendingNodeScreenPositions.erase(itS);
            }
            if (auto it = pendingNodePositions.find((int)lid); it != pendingNodePositions.end())
            {
                // Apply saved position once; do not write (0,0) defaults
                const ImVec2 p = it->second;
                if (!(p.x == 0.0f && p.y == 0.0f))
                {
                    ImNodes::SetNodeGridSpacePos((int)lid, p);
                    juce::Logger::writeToLog(
                        "[PositionRestore] Applied pending position for node " +
                        juce::String((int)lid) + ": (" + juce::String(p.x) + ", " +
                        juce::String(p.y) + ")");
                }
                pendingNodePositions.erase(it);
            }
            // Apply pending size if queued (for Comment nodes to prevent feedback loop)
            if (auto itSize = pendingNodeSizes.find((int)lid); itSize != pendingNodeSizes.end())
            {
                // Store the desired size in the Comment module itself
                if (auto* comment = dynamic_cast<CommentModuleProcessor*>(
                        synth->getModuleForLogical((juce::uint32)lid)))
                {
                    comment->nodeWidth = itSize->second.x;
                    comment->nodeHeight = itSize->second.y;
                }
                pendingNodeSizes.erase(itSize);
            }
            drawnNodes.insert((int)lid);
        }

        // Node action popup (Delete / Duplicate)
        bool triggerInsertMixer = false;
        if (ImGui::BeginPopup("NodeActionPopup"))
        {
            const juce::String selectedType =
                selectedLogicalId != 0 ? getTypeForLogical((juce::uint32)selectedLogicalId)
                                       : juce::String();
            const bool selectedIsMeta = selectedType.equalsIgnoreCase("meta_module") ||
                                        selectedType.equalsIgnoreCase("meta");

            if (ImGui::MenuItem("Delete") && selectedLogicalId != 0)
            {
                mutedNodeStates.erase(
                    (juce::uint32)selectedLogicalId); // Clean up muted state if exists
                synth->removeModule(synth->getNodeIdForLogical((juce::uint32)selectedLogicalId));
                graphNeedsRebuild = true;
                pushSnapshot();
                NotificationManager::post(
                    NotificationManager::Type::Info, "Deleted " + juce::String(1) + " node(s)");
                selectedLogicalId = 0;
            }
            if (ImGui::MenuItem("Duplicate") && selectedLogicalId != 0)
            {
                const juce::String type = getTypeForLogical((juce::uint32)selectedLogicalId);
                if (!type.isEmpty())
                {
                    auto newNodeId = synth->addModule(type);
                    graphNeedsRebuild = true;
                    if (auto* src = synth->getModuleForLogical((juce::uint32)selectedLogicalId))
                        if (auto* dst =
                                synth->getModuleForLogical(synth->getLogicalIdForNode(newNodeId)))
                            dst->getAPVTS().replaceState(src->getAPVTS().copyState());
                    ImVec2 pos = ImNodes::GetNodeGridSpacePos(selectedLogicalId);
                    ImNodes::SetNodeGridSpacePos(
                        (int)synth->getLogicalIdForNode(newNodeId),
                        ImVec2(pos.x + 40.0f, pos.y + 40.0f));
                    pushSnapshot();
                }
            }
            if (ImGui::MenuItem("Expand Meta Module", nullptr, false, selectedIsMeta))
            {
                expandMetaModule((juce::uint32)selectedLogicalId);
            }
            if (ImGui::MenuItem("Insert Mixer", "Ctrl+T") && selectedLogicalId != 0)
            {
                triggerInsertMixer = true;
            }
            ImGui::EndPopup();
        }

        // Shortcut: Ctrl+T to insert a Mixer after selected node and reroute
        // Debounced Ctrl+T
        const bool ctrlDown = ImGui::GetIO().KeyCtrl;
        if (!ctrlDown)
        {
            mixerShortcutCooldown = false;
            insertNodeShortcutCooldown = false;
        }

        const bool insertMixerShortcut = consumeShortcutFlag(shortcutInsertMixerRequested);
        const bool connectToTrackMixerShortcut =
            consumeShortcutFlag(shortcutConnectSelectedToTrackMixerRequested);
        const bool connectToRecorderShortcut =
            consumeShortcutFlag(shortcutConnectSelectedToRecorderRequested);

        // Handle "Connect Selected to Track Mixer" shortcut
        if (connectToTrackMixerShortcut && ImNodes::NumSelectedNodes() > 0)
        {
            handleConnectSelectedToTrackMixer();
        }

        // Handle "Connect Selected to Recorder" shortcut
        if (connectToRecorderShortcut && ImNodes::NumSelectedNodes() > 0)
        {
            handleConnectSelectedToRecorder();
        }

        if ((triggerInsertMixer || (selectedLogicalId != 0 && insertMixerShortcut)) &&
            !mixerShortcutCooldown)
        {
            mixerShortcutCooldown = true; // Prevent re-triggering in the same frame
            const juce::uint32 srcLid = (juce::uint32)selectedLogicalId;

            juce::Logger::writeToLog("--- [InsertMixer] Start ---");
            juce::Logger::writeToLog(
                "[InsertMixer] Selected Node Logical ID: " + juce::String(srcLid));

            auto srcNodeId = synth->getNodeIdForLogical(srcLid);
            if (srcNodeId.uid == 0)
            {
                juce::Logger::writeToLog(
                    "[InsertMixer] ABORT: Source node with logical ID " + juce::String(srcLid) +
                    " is invalid or could not be found.");
            }
            else
            {
                // 1. Collect all outgoing connections from the selected node
                std::vector<ModularSynthProcessor::ConnectionInfo> outgoingConnections;
                for (const auto& c : synth->getConnectionsInfo())
                {
                    if (c.srcLogicalId == srcLid)
                    {
                        outgoingConnections.push_back(c);
                    }
                }
                juce::Logger::writeToLog(
                    "[InsertMixer] Found " + juce::String(outgoingConnections.size()) +
                    " outgoing connections to reroute.");
                for (const auto& c : outgoingConnections)
                {
                    juce::String destStr =
                        c.dstIsOutput ? "Main Output" : "Node " + juce::String(c.dstLogicalId);
                    juce::Logger::writeToLog(
                        "  - Stored connection: [Src: " + juce::String(c.srcLogicalId) + ":" +
                        juce::String(c.srcChan) + "] -> [Dst: " + destStr + ":" +
                        juce::String(c.dstChan) + "]");
                }

                // 2. Create and position the new mixer node intelligently
                auto               mixNodeIdGraph = synth->addModule("mixer");
                const juce::uint32 mixLid = synth->getLogicalIdForNode(mixNodeIdGraph);

                ImVec2 srcPos = ImNodes::GetNodeGridSpacePos(selectedLogicalId);
                ImVec2 avgDestPos = srcPos; // Default to source pos if no outputs

                if (!outgoingConnections.empty())
                {
                    float totalX = 0.0f, totalY = 0.0f;
                    for (const auto& c : outgoingConnections)
                    {
                        int    destId = c.dstIsOutput ? 0 : (int)c.dstLogicalId;
                        ImVec2 pos = ImNodes::GetNodeGridSpacePos(destId);
                        totalX += pos.x;
                        totalY += pos.y;
                    }
                    avgDestPos = ImVec2(
                        totalX / outgoingConnections.size(), totalY / outgoingConnections.size());
                }
                else
                {
                    // If there are no outgoing connections, place it to the right
                    avgDestPos.x += 600.0f;
                }

                // Place the new mixer halfway between the source and the average destination
                pendingNodePositions[(int)mixLid] =
                    ImVec2((srcPos.x + avgDestPos.x) * 0.5f, (srcPos.y + avgDestPos.y) * 0.5f);
                juce::Logger::writeToLog(
                    "[InsertMixer] Added new Mixer. Logical ID: " + juce::String(mixLid) +
                    ", Node ID: " + juce::String(mixNodeIdGraph.uid));

                // 3. Disconnect all original outgoing links
                juce::Logger::writeToLog("[InsertMixer] Disconnecting original links...");
                for (const auto& c : outgoingConnections)
                {
                    auto currentSrcNodeId = synth->getNodeIdForLogical(c.srcLogicalId);
                    auto dstNodeId = c.dstIsOutput ? synth->getOutputNodeID()
                                                   : synth->getNodeIdForLogical(c.dstLogicalId);

                    if (currentSrcNodeId.uid != 0 && dstNodeId.uid != 0)
                    {
                        bool success =
                            synth->disconnect(currentSrcNodeId, c.srcChan, dstNodeId, c.dstChan);
                        juce::Logger::writeToLog(
                            "  - Disconnecting [" + juce::String(currentSrcNodeId.uid) + ":" +
                            juce::String(c.srcChan) + "] -> [" + juce::String(dstNodeId.uid) + ":" +
                            juce::String(c.dstChan) + "]... " + (success ? "SUCCESS" : "FAILED"));
                    }
                    else
                    {
                        juce::Logger::writeToLog("  - SKIPPING Disconnect due to invalid node ID.");
                    }
                }

                // 4. Connect the source node to the new mixer's first input
                juce::Logger::writeToLog("[InsertMixer] Connecting source node to new mixer...");
                bool c1 = synth->connect(srcNodeId, 0, mixNodeIdGraph, 0); // L to In A L
                juce::Logger::writeToLog(
                    "  - Connecting [" + juce::String(srcNodeId.uid) + ":0] -> [" +
                    juce::String(mixNodeIdGraph.uid) + ":0]... " + (c1 ? "SUCCESS" : "FAILED"));
                bool c2 = synth->connect(srcNodeId, 1, mixNodeIdGraph, 1); // R to In A R
                juce::Logger::writeToLog(
                    "  - Connecting [" + juce::String(srcNodeId.uid) + ":1] -> [" +
                    juce::String(mixNodeIdGraph.uid) + ":1]... " + (c2 ? "SUCCESS" : "FAILED"));

                // 5. Connect the mixer's output to all the original destinations (maintaining the
                // chain)
                juce::Logger::writeToLog(
                    "[InsertMixer] Connecting mixer to original destinations to maintain chain...");
                if (outgoingConnections.empty())
                {
                    juce::Logger::writeToLog(
                        "  - No original outgoing connections. Connecting mixer to Main Output by "
                        "default.");
                    auto outNode = synth->getOutputNodeID();
                    if (outNode.uid != 0)
                    {
                        bool o1 = synth->connect(mixNodeIdGraph, 0, outNode, 0);
                        juce::Logger::writeToLog(
                            "  - Connecting [" + juce::String(mixNodeIdGraph.uid) +
                            ":0] -> [Output:0]... " + (o1 ? "SUCCESS" : "FAILED"));
                        bool o2 = synth->connect(mixNodeIdGraph, 1, outNode, 1);
                        juce::Logger::writeToLog(
                            "  - Connecting [" + juce::String(mixNodeIdGraph.uid) +
                            ":1] -> [Output:1]... " + (o2 ? "SUCCESS" : "FAILED"));
                    }
                }
                else
                {
                    for (const auto& c : outgoingConnections)
                    {
                        auto dstNodeId = c.dstIsOutput ? synth->getOutputNodeID()
                                                       : synth->getNodeIdForLogical(c.dstLogicalId);
                        if (dstNodeId.uid != 0)
                        {
                            // Connect mixer output to the same destination the original node was
                            // connected to This maintains the chain: original -> mixer ->
                            // destination
                            bool success =
                                synth->connect(mixNodeIdGraph, c.srcChan, dstNodeId, c.dstChan);
                            juce::String destStr = c.dstIsOutput
                                                       ? "Main Output"
                                                       : "Node " + juce::String(c.dstLogicalId);
                            juce::Logger::writeToLog(
                                "  - Maintaining chain: Mixer [" +
                                juce::String(mixNodeIdGraph.uid) + ":" + juce::String(c.srcChan) +
                                "] -> " + destStr + "[" + juce::String(dstNodeId.uid) + ":" +
                                juce::String(c.dstChan) + "]... " +
                                (success ? "SUCCESS" : "FAILED"));
                        }
                        else
                        {
                            juce::Logger::writeToLog(
                                "  - SKIPPING Reconnect due to invalid destination node ID for "
                                "original logical ID " +
                                juce::String(c.dstLogicalId));
                        }
                    }
                }

                graphNeedsRebuild = true;
                pushSnapshot(); // Make the entire operation undoable
                juce::Logger::writeToLog(
                    "[InsertMixer] Rerouting complete. Flagging for graph rebuild.");
            }
            juce::Logger::writeToLog("--- [InsertMixer] End ---");
        }

        // Shortcut: Ctrl+I to show Insert Node popup menu
        const bool showInsertPopupShortcut = consumeShortcutFlag(shortcutShowInsertPopupRequested);
        if (selectedLogicalId != 0 && showInsertPopupShortcut && !insertNodeShortcutCooldown)
        {
            insertNodeShortcutCooldown = true;
            showInsertNodePopup = true;
        }

        // Insert Node popup menu
        if (showInsertNodePopup)
        {
            ImGui::OpenPopup("InsertNodePopup");
            showInsertNodePopup = false;
        }

        if (ImGui::BeginPopup("InsertNodePopup"))
        {
            ImGui::Text("Insert Node Between Connections");

            // Audio Path
            if (ImGui::MenuItem("VCF"))
            {
                insertNodeBetween("vcf");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("VCA"))
            {
                insertNodeBetween("vca");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Delay"))
            {
                insertNodeBetween("delay");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Reverb"))
            {
                insertNodeBetween("reverb");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Mixer"))
            {
                insertNodeBetween("mixer");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Recorder"))
            {
                insertNodeBetween("recorder");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Shaping Oscillator"))
            {
                insertNodeBetween("shaping_oscillator");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("8-Band Shaper"))
            {
                insertNodeBetween("8bandshaper");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Granulator"))
            {
                insertNodeBetween("granulator");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Spatial Granulator"))
            {
                insertNodeBetween("spatial_granulator");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Harmonic Shaper"))
            {
                insertNodeBetween("harmonic_shaper");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Vocal Tract Filter"))
            {
                insertNodeBetween("vocal_tract_filter");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Scope"))
            {
                insertNodeBetween("scope");
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();

            // Modulation Path
            if (ImGui::MenuItem("MIDI CV"))
            {
                insertNodeBetween("midi_cv");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("OSC CV"))
            {
                insertNodeBetween("osc_cv");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Attenuverter"))
            {
                insertNodeBetween("attenuverter");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Math"))
            {
                insertNodeBetween("math");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Comparator"))
            {
                insertNodeBetween("comparator");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("CV Mixer"))
            {
                insertNodeBetween("cv_mixer");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Sequential Switch"))
            {
                insertNodeBetween("sequential_switch");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Automato"))
            {
                insertNodeBetween("automato");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Automation Lane"))
            {
                insertNodeBetween("automation_lane");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Timeline"))
            {
                insertNodeBetween("timeline");
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // Output sink node with stereo inputs (single, fixed ID 0)
        const bool isOutputHovered = (hoveredLinkDstId == kOutputHighlightId);
        ImU32      outputTitleBarColor =
            IM_COL32(80, 80, 80, 255); // Default output node color (dark grey)
        if (isOutputHovered)
        {
            outputTitleBarColor = IM_COL32(255, 220, 0, 255); // Yellow when hovered
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, outputTitleBarColor);
        }
        ImNodes::BeginNode(0);
        ImNodes::BeginNodeTitleBar();

        // Calculate optimal text color based on title bar background color
        ImU32 optimalTextColor = ThemeUtils::getOptimalTextColor(outputTitleBarColor);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(optimalTextColor));
        ImGui::TextUnformatted("Output");
        ImGui::PopStyleColor();

        ImNodes::EndNodeTitleBar();
        if (isOutputHovered)
            ImNodes::PopColorStyle();

        // In L pin with proper Audio type coloring (green)
        {
            int a = encodePinId({0, 0, true});
            seenAttrs.insert(a);
            availableAttrs.insert(a);
            bool         isConnected = connectedInputAttrs.count(a) > 0;
            PinID        pinId = {0, 0, true, false, ""};
            PinDataType  pinType = getPinDataTypeForPin(pinId);
            unsigned int pinColor = getImU32ForType(pinType);
            ImNodes::PushColorStyle(ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);
            ImNodes::BeginInputAttribute(a);
#if JUCE_DEBUG
            ++gImNodesInputDepth;
#endif
            ImGui::Text("In L");
            ImNodes::EndInputAttribute();

            // Cache pin position for cut gesture (Input pins are to the left of text)
            {
                ImVec2 pinMin = ImGui::GetItemRectMin();
                ImVec2 pinMax = ImGui::GetItemRectMax();
                float  centerY = (pinMin.y + pinMax.y) * 0.5f;
                attrPositions[a] = ImVec2(pinMin.x, centerY);
            }
#if JUCE_DEBUG
            --gImNodesInputDepth;
            jassert(gImNodesInputDepth >= 0);
#endif
            ImNodes::PopColorStyle();
        }

        // In R pin with proper Audio type coloring (green)
        {
            int a = encodePinId({0, 1, true});
            seenAttrs.insert(a);
            availableAttrs.insert(a);
            bool         isConnected = connectedInputAttrs.count(a) > 0;
            PinID        pinId = {0, 1, true, false, ""};
            PinDataType  pinType = getPinDataTypeForPin(pinId);
            unsigned int pinColor = getImU32ForType(pinType);
            ImNodes::PushColorStyle(ImNodesCol_Pin, isConnected ? colPinConnected : pinColor);
            ImNodes::BeginInputAttribute(a);
#if JUCE_DEBUG
            ++gImNodesInputDepth;
#endif
            ImGui::Text("In R");
            ImNodes::EndInputAttribute();

            // Cache pin position for cut gesture (Input pins are to the left of text)
            {
                ImVec2 pinMin = ImGui::GetItemRectMin();
                ImVec2 pinMax = ImGui::GetItemRectMax();
                float  centerY = (pinMin.y + pinMax.y) * 0.5f;
                attrPositions[a] = ImVec2(pinMin.x, centerY);
            }
#if JUCE_DEBUG
            --gImNodesInputDepth;
            jassert(gImNodesInputDepth >= 0);
#endif
            ImNodes::PopColorStyle();
        }

        ImNodes::EndNode();

        // Cache output node position for snapshot safety
        // Graph is always in consistent state since we rebuild at frame start
        lastKnownNodePositions[0] = ImNodes::GetNodeGridSpacePos(0);

        if (auto it = pendingNodePositions.find(0); it != pendingNodePositions.end())
        {
            ImNodes::SetNodeGridSpacePos(0, it->second);
            juce::Logger::writeToLog(
                "[PositionRestore] Applied pending position for output node 0: (" +
                juce::String(it->second.x) + ", " + juce::String(it->second.y) + ")");
            pendingNodePositions.erase(it);
        }
        else
        {
            // Output node (0): set default position if not in pending positions
            ImVec2 currentPos = ImNodes::GetNodeGridSpacePos(0);
            if (currentPos.x == 0.0f && currentPos.y == 0.0f)
            {
                ImNodes::SetNodeGridSpacePos(0, ImVec2(1250.0f, 500.0f));
                juce::Logger::writeToLog(
                    "[PositionRestore] Set default position for output node: (2000.0, 500.0)");
            }
        }
        drawnNodes.insert(0);

        // Use last frame's hovered node id for highlighting (queried after EndNodeEditor)
        int hoveredNodeId = lastHoveredNodeId;

        // Draw existing audio connections (IDs stable via bitmasking)
        int cableIdx = 0;
        for (const auto& c : synth->getConnectionsInfo())
        {

            // Skip links whose nodes weren't drawn this frame (e.g., just deleted)
            if (c.srcLogicalId != 0 && !drawnNodes.count((int)c.srcLogicalId))
            {
                continue;
            }
            if (!c.dstIsOutput && c.dstLogicalId != 0 && !drawnNodes.count((int)c.dstLogicalId))
            {
                continue;
            }

            const int srcAttr = encodePinId({c.srcLogicalId, c.srcChan, false});
            const int dstAttr = c.dstIsOutput ? encodePinId({0, c.dstChan, true})
                                              : encodePinId({c.dstLogicalId, c.dstChan, true});

            // CRITICAL FIX: Always add connections to linkIdToAttrs, even if pins aren't in
            // availableAttrs This allows cutting CV cables, dynamic pins, and connections that
            // exist but pins weren't drawn The availableAttrs check was preventing valid
            // connections from being cuttable
            if (!availableAttrs.count(srcAttr) || !availableAttrs.count(dstAttr))
            {
                static std::unordered_set<std::string> warnOnce;
                const std::string                      key =
                    std::to_string((int)c.srcLogicalId) + ":" + std::to_string(c.srcChan) + "->" +
                    (c.dstIsOutput ? std::string("0") : std::to_string((int)c.dstLogicalId)) + ":" +
                    std::to_string(c.dstChan);
                if (warnOnce.insert(key).second)
                {
                    juce::Logger::writeToLog(
                        juce::String(
                            "[ImNodes][WARN] Connection pins not in availableAttrs (may be "
                            "dynamic/CV): srcPresent=") +
                        (availableAttrs.count(srcAttr) ? "1" : "0") +
                        " dstPresent=" + (availableAttrs.count(dstAttr) ? "1" : "0") +
                        " srcKey=(lid=" + juce::String((int)c.srcLogicalId) +
                        ",ch=" + juce::String(c.srcChan) + ")" +
                        " dstKey=(lid=" + juce::String(c.dstIsOutput ? 0 : (int)c.dstLogicalId) +
                        ",ch=" + juce::String(c.dstChan) + ",in=1) id(s)=" + juce::String(srcAttr) +
                        "," + juce::String(dstAttr) +
                        " - Adding to linkIdToAttrs anyway for cut gesture");
                }
                // Continue to add connection - don't skip it!
            }

            const int linkId = linkIdOf(srcAttr, dstAttr);
            linkIdToAttrs[linkId] = {srcAttr, dstAttr};

            // --- THIS IS THE DEFINITIVE FIX ---
            // 1. Determine the base color and check for signal activity.
            auto        srcPin = decodePinId(srcAttr);
            PinDataType linkDataType = getPinDataTypeForPin(srcPin);
            ImU32       linkColor = getImU32ForType(linkDataType);
            float       magnitude = 0.0f;
            bool        hasThicknessModification = false;

            // CRASH FIX: Verify module still exists before accessing it.
            // During preset loading, modules may be destroyed while we're iterating connections.
            if (synth != nullptr && srcPin.logicalId != 0)
            {
                // First verify the module exists in the current module list
                bool moduleExists = false;
                for (const auto& modInfo : synth->getModulesInfo())
                {
                    if (modInfo.first == srcPin.logicalId)
                    {
                        moduleExists = true;
                        break;
                    }
                }

                // Only access if module still exists
                if (moduleExists)
                {
                    if (auto* srcModule = synth->getModuleForLogical(srcPin.logicalId))
                    {
                        magnitude = srcModule->getOutputChannelValue(srcPin.channel);
                    }
                }
            }

            // 2. If the signal is active, calculate a glowing/blinking color.
            if (magnitude > 0.01f)
            {
                const float blinkSpeed = 8.0f;
                float blinkFactor = (std::sin((float)ImGui::GetTime() * blinkSpeed) + 1.0f) * 0.5f;
                float glowIntensity = juce::jlimit(0.0f, 1.0f, blinkFactor * magnitude * 2.0f);

                // Brighten the base color and modulate alpha for glow effect
                ImVec4 colorVec = ImGui::ColorConvertU32ToFloat4(linkColor);
                colorVec.x = juce::jmin(1.0f, colorVec.x + glowIntensity * 0.4f);
                colorVec.y = juce::jmin(1.0f, colorVec.y + glowIntensity * 0.4f);
                colorVec.z = juce::jmin(1.0f, colorVec.z + glowIntensity * 0.4f);
                colorVec.w = juce::jlimit(0.5f, 1.0f, 0.5f + glowIntensity * 0.5f);
                linkColor = ImGui::ColorConvertFloat4ToU32(colorVec);

                // Make active cables slightly thicker (keep constant in screen space under zoom)
                {
                    float currentZoom = 1.0f;
#if defined(IMNODES_ZOOM_ENABLED)
                    if (ImNodes::GetCurrentContext())
                        currentZoom = ImNodes::EditorContextGetZoom();
#endif
                    ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness, 3.0f / currentZoom);
                }
                hasThicknessModification = true;
            }

            // 3. Push the chosen color (either normal or glowing) to the style stack.
            const auto& theme = ThemeManager::getInstance().getCurrentTheme();
            ImNodes::PushColorStyle(ImNodesCol_Link, linkColor);
            ImNodes::PushColorStyle(ImNodesCol_LinkHovered, theme.links.link_hovered);
            ImNodes::PushColorStyle(ImNodesCol_LinkSelected, theme.links.link_selected);

            // 4. Check for node hover highlight (this should override the glow).
            const bool hl = (hoveredNodeId != -1) &&
                            ((int)c.srcLogicalId == hoveredNodeId ||
                             (!c.dstIsOutput && (int)c.dstLogicalId == hoveredNodeId) ||
                             (c.dstIsOutput && hoveredNodeId == 0));
            if (hl)
            {
                ImNodes::PushColorStyle(ImNodesCol_Link, theme.links.link_highlighted);
            }

            // 5. Tell imnodes to draw the link. It will use the color we just pushed.
            ImNodes::Link(linkId, srcAttr, dstAttr);

            // 6. Pop ALL style modifications to restore the defaults for the next link.
            if (hl)
                ImNodes::PopColorStyle();
            ImNodes::PopColorStyle(); // LinkSelected
            ImNodes::PopColorStyle(); // LinkHovered
            ImNodes::PopColorStyle(); // Link
            if (hasThicknessModification)
                ImNodes::PopStyleVar(); // LinkThickness

            // --- END OF FIX ---
        }

        // Drag detection for node movement: snapshot once on mouse release (post-state)
        const bool hoveringNode = (lastHoveredNodeId != -1);
        if (hoveringNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            isDraggingNode = true;
        }
        if (isDraggingNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            isDraggingNode = false;
            // Capture positions after a move so subsequent operations (e.g. delete) undo to the
            // moved location
            pushSnapshot();
        }
    }

    // --- Handle Auto-Connect Requests from MIDI Players ---
    for (const auto& modInfo : synth->getModulesInfo())
    {
        if (auto* midiPlayer =
                dynamic_cast<MIDIPlayerModuleProcessor*>(synth->getModuleForLogical(modInfo.first)))
        {
            // Check for initial button presses
            if (midiPlayer->autoConnectTriggered.exchange(false))
            {
                midiPlayer->lastAutoConnectState =
                    MIDIPlayerModuleProcessor::AutoConnectState::Samplers;
                handleMidiPlayerAutoConnect(midiPlayer, modInfo.first);
                pushSnapshot();
            }
            else if (midiPlayer->autoConnectVCOTriggered.exchange(false))
            {
                midiPlayer->lastAutoConnectState =
                    MIDIPlayerModuleProcessor::AutoConnectState::PolyVCO;
                handleMidiPlayerAutoConnectVCO(midiPlayer, modInfo.first);
                pushSnapshot();
            }
            else if (midiPlayer->autoConnectHybridTriggered.exchange(false))
            {
                midiPlayer->lastAutoConnectState =
                    MIDIPlayerModuleProcessor::AutoConnectState::Hybrid;
                handleMidiPlayerAutoConnectHybrid(midiPlayer, modInfo.first);
                pushSnapshot();
            }
            // --- THIS IS THE NEW LOGIC ---
            // Check if an update was requested after a new file was loaded
            else if (midiPlayer->connectionUpdateRequested.exchange(false))
            {
                // Re-run the correct handler based on the saved state
                switch (midiPlayer->lastAutoConnectState.load())
                {
                case MIDIPlayerModuleProcessor::AutoConnectState::Samplers:
                    handleMidiPlayerAutoConnect(midiPlayer, modInfo.first);
                    pushSnapshot();
                    break;
                case MIDIPlayerModuleProcessor::AutoConnectState::PolyVCO:
                    handleMidiPlayerAutoConnectVCO(midiPlayer, modInfo.first);
                    pushSnapshot();
                    break;
                case MIDIPlayerModuleProcessor::AutoConnectState::Hybrid:
                    handleMidiPlayerAutoConnectHybrid(midiPlayer, modInfo.first);
                    pushSnapshot();
                    break;
                case MIDIPlayerModuleProcessor::AutoConnectState::None:
                default:
                    // Do nothing if it wasn't auto-connected before
                    break;
                }
            }
            // --- END OF NEW LOGIC ---
        }
    }
    // --- Handle Auto-Connect Requests using new intelligent system ---
    handleAutoConnectionRequests();

    // Capture hover state for drag/drop logic before we leave the node editor scope
    int  hoverPinIdForDrop = -1;
    int  hoverNodeIdForDrop = -1;
    int  hoverLinkIdForDrop = -1;
    bool pinHoveredDuringEditor = false;
    bool nodeHoveredDuringEditor = false;
    bool linkHoveredDuringEditor = false;

    // ======================================================
    // === 💡 MODAL MINIMAP (v13 - Scale-on-Press) ==========
    // ======================================================
    if (isMinimapEnlarged.load())
    {
        ImNodes::MiniMap(modalMinimapScale, ImNodesMiniMapLocation_BottomRight);
    }
    else
    {
        ImNodes::MiniMap(0.2f, ImNodesMiniMapLocation_BottomRight);
    }
    // ======================================================
    // === 💡 END MODAL MINIMAP =============================
    // ======================================================

    ImNodes::EndNodeEditor();

    // --- Cut-by-line gesture (finalize on release) ---
    {
        const bool rmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Right);
        if (cutModeActive && rmbReleased)
        {
            cutModeActive = false;
            cutJustPerformed = true;
            juce::Logger::writeToLog(
                "[CutGesture] ENDED at Grid(" + juce::String(cutEndGrid.x) + ", " +
                juce::String(cutEndGrid.y) + ")");

            struct Hit
            {
                int      linkId;
                float    t;
                ImVec2   posGrid;
                LinkInfo link;
            };
            std::vector<Hit> hits;
            hits.reserve(linkIdToAttrs.size());

            // More robust segment intersection with point-to-segment distance fallback
            auto segmentIntersect = [](const ImVec2& p,
                                       const ImVec2& p2,
                                       const ImVec2& q,
                                       const ImVec2& q2,
                                       float&        tOut,
                                       ImVec2&       ptOut) -> bool {
                ImVec2      r{p2.x - p.x, p2.y - p.y};
                ImVec2      s{q2.x - q.x, q2.y - q.y};
                const float rxs = r.x * s.y - r.y * s.x;
                const float qmpx = q.x - p.x;
                const float qmpy = q.y - p.y;
                const float qmpxr = qmpx * r.y - qmpy * r.x;

                // More lenient epsilon for near-parallel/colinear cases
                const float epsilon = 1e-4f;

                if (std::abs(rxs) < epsilon)
                {
                    // Parallel or colinear - use point-to-segment distance check
                    // Check if cut line endpoints are close to cable segment
                    auto pointToSegmentDistSq = [](const ImVec2& pt,
                                                   const ImVec2& segStart,
                                                   const ImVec2& segEnd) -> float {
                        ImVec2 seg = ImVec2(segEnd.x - segStart.x, segEnd.y - segStart.y);
                        float  segLenSq = seg.x * seg.x + seg.y * seg.y;
                        if (segLenSq < 1e-6f)
                        {
                            // Degenerate segment - just distance to point
                            float dx = pt.x - segStart.x;
                            float dy = pt.y - segStart.y;
                            return dx * dx + dy * dy;
                        }
                        float t = juce::jlimit(
                            0.0f,
                            1.0f,
                            ((pt.x - segStart.x) * seg.x + (pt.y - segStart.y) * seg.y) / segLenSq);
                        ImVec2 closest = ImVec2(segStart.x + t * seg.x, segStart.y + t * seg.y);
                        float  dx = pt.x - closest.x;
                        float  dy = pt.y - closest.y;
                        return dx * dx + dy * dy;
                    };

                    // Check if cut line is close to cable (within 10 pixels)
                    const float thresholdSq = 100.0f; // 10 pixels squared
                    float       dist1Sq = pointToSegmentDistSq(q, p, p2);
                    float       dist2Sq = pointToSegmentDistSq(q2, p, p2);
                    if (dist1Sq < thresholdSq || dist2Sq < thresholdSq)
                    {
                        // Use midpoint of cable segment as intersection point
                        tOut = 0.5f;
                        ptOut = ImVec2((p.x + p2.x) * 0.5f, (p.y + p2.y) * 0.5f);
                        return true;
                    }
                    return false;
                }

                const float t = (qmpx * s.y - qmpy * s.x) / rxs;
                const float u = (qmpx * r.y - qmpy * r.x) / rxs;

                // More lenient bounds check - allow slight overshoot
                const float margin = 0.01f;
                if (t >= -margin && t <= (1.0f + margin) && u >= -margin && u <= (1.0f + margin))
                {
                    // Clamp to valid range
                    float tClamped = juce::jlimit(0.0f, 1.0f, t);
                    tOut = tClamped;
                    ptOut = ImVec2(p.x + tClamped * r.x, p.y + tClamped * r.y);
                    return true;
                }
                return false;
            };

            auto minf = [](float x, float y) { return x < y ? x : y; };
            auto maxf = [](float x, float y) { return x > y ? x : y; };

            // Build hits
            juce::Logger::writeToLog(
                "[CutGesture] Starting cut detection. linkIdToAttrs.size()=" +
                juce::String(linkIdToAttrs.size()) +
                " attrPositions.size()=" + juce::String(attrPositions.size()));
            int checkedCount = 0;
            int skippedInvalidAttr = 0;
            int skippedNoPositions = 0;
            int skippedBBox = 0;
            int skippedIntersect = 0;
            int skippedEndpoint = 0;
            for (const auto& kv : linkIdToAttrs)
            {
                const int linkId = kv.first;
                const int srcAttr = kv.second.first;
                const int dstAttr = kv.second.second;
                if (srcAttr == 0 || dstAttr == 0)
                {
                    skippedInvalidAttr++;
                    continue;
                }

                LinkInfo li;
                li.linkId = linkId;
                li.srcPin = decodePinId(srcAttr);
                li.dstPin = decodePinId(dstAttr);
                li.isMod = li.srcPin.isMod || li.dstPin.isMod;

                // Prefer actual pin attribute positions if available; fallback to node centers
                ImVec2 a, b;
                bool   usingPinPositions = false;
                auto   attrToGrid = [this](int attr) -> ImVec2 {
                    auto it = attrPositions.find(attr);
                    if (it != attrPositions.end())
                    {
                        // attrPositions now stores GRID-SPACE, so return directly
                        return it->second;
                    }
                    return ImVec2(FLT_MAX, FLT_MAX);
                };
                ImVec2 aGrid = attrToGrid(srcAttr);
                ImVec2 bGrid = attrToGrid(dstAttr);
                if (aGrid.x != FLT_MAX && bGrid.x != FLT_MAX)
                {
                    a = aGrid;
                    b = bGrid;
                    usingPinPositions = true;
                }
                else
                {
                    skippedNoPositions++;
                    // Fallback to node centers - but this might fail for output node (logicalId 0)
                    juce::Logger::writeToLog(
                        "[CutGesture] WARNING: Link " + juce::String(linkId) +
                        " missing pin positions. Falling back to node centers. " +
                        "Src: " + juce::String((int)li.srcPin.logicalId) + ":" +
                        juce::String(li.srcPin.channel) +
                        " Dst: " + juce::String((int)li.dstPin.logicalId) + ":" +
                        juce::String(li.dstPin.channel));

                    a = ImNodes::GetNodeGridSpacePos((int)li.srcPin.logicalId);
                    b = ImNodes::GetNodeGridSpacePos((int)li.dstPin.logicalId);

                    juce::Logger::writeToLog(
                        "[CutGesture] Fallback Coords: A(" + juce::String(a.x) + "," +
                        juce::String(a.y) + ") B(" + juce::String(b.x) + "," + juce::String(b.y) +
                        ")");
                }
                ImVec2 c = cutStartGrid;
                ImVec2 d = cutEndGrid;

                // More lenient bounding box check - add padding to account for cable thickness and
                // imprecision
                const float bboxPadding = 20.0f; // pixels of padding
                ImVec2      abMin{minf(a.x, b.x) - bboxPadding, minf(a.y, b.y) - bboxPadding};
                ImVec2      abMax{maxf(a.x, b.x) + bboxPadding, maxf(a.y, b.y) + bboxPadding};
                ImVec2      cdMin{minf(c.x, d.x), minf(c.y, d.y)};
                ImVec2      cdMax{maxf(c.x, d.x), maxf(c.y, d.y)};
                if (abMax.x < cdMin.x || cdMax.x < abMin.x || abMax.y < cdMin.y ||
                    cdMax.y < abMin.y)
                {
                    skippedBBox++;
                    // Optional: log why we skipped bbox for debugging specific cables
                    // juce::Logger::writeToLog("[CutGesture] Skip BBox: Link " +
                    // juce::String(linkId));
                    continue;
                }

                juce::Logger::writeToLog(
                    "[CutGesture] Checking Link " + juce::String(linkId) + " A(" +
                    juce::String(a.x) + "," + juce::String(a.y) + ") " + " B(" + juce::String(b.x) +
                    "," + juce::String(b.y) + ") " + " Cut(" + juce::String(c.x) + "," +
                    juce::String(c.y) + "->" + juce::String(d.x) + "," + juce::String(d.y) + ")");

                float  t = 0.0f;
                ImVec2 pt{};
                bool   hit = segmentIntersect(a, b, c, d, t, pt);

                // Fallback: if intersection fails, check if cut line is close to cable using
                // point-to-segment distance
                if (!hit)
                {
                    auto pointToSegmentDistSq =
                        [](const ImVec2& pt,
                           const ImVec2& segStart,
                           const ImVec2& segEnd) -> std::pair<float, float> {
                        ImVec2 seg = ImVec2(segEnd.x - segStart.x, segEnd.y - segStart.y);
                        float  segLenSq = seg.x * seg.x + seg.y * seg.y;
                        if (segLenSq < 1e-6f)
                        {
                            float dx = pt.x - segStart.x;
                            float dy = pt.y - segStart.y;
                            return {dx * dx + dy * dy, 0.5f};
                        }
                        float t = juce::jlimit(
                            0.0f,
                            1.0f,
                            ((pt.x - segStart.x) * seg.x + (pt.y - segStart.y) * seg.y) / segLenSq);
                        ImVec2 closest = ImVec2(segStart.x + t * seg.x, segStart.y + t * seg.y);
                        float  dx = pt.x - closest.x;
                        float  dy = pt.y - closest.y;
                        return {dx * dx + dy * dy, t};
                    };

                    // Check distance from cut line midpoint to cable segment
                    ImVec2 cutMid = ImVec2((c.x + d.x) * 0.5f, (c.y + d.y) * 0.5f);
                    auto [distSq, tOnCable] = pointToSegmentDistSq(cutMid, a, b);

                    // If cut line is within 15 pixels of cable, consider it a hit
                    const float thresholdSq = 225.0f; // 15 pixels squared
                    if (distSq < thresholdSq && tOnCable > 0.005f && tOnCable < 0.995f)
                    {
                        hit = true;
                        t = tOnCable;
                        pt = ImVec2(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y));
                        juce::Logger::writeToLog(
                            "[CutGesture] FALLBACK HIT (distance-based): linkId=" +
                            juce::String(linkId) + " dist=" + juce::String(std::sqrt(distSq), 1) +
                            "px");
                    }
                }

                if (hit)
                {
                    // Only skip if intersection is extremely close to endpoints (within 0.5% of
                    // cable length) This prevents cutting right at the pins but allows cutting very
                    // close to them
                    if (t <= 0.005f || t >= 0.995f)
                    {
                        skippedEndpoint++;
                        continue;
                    }
                    checkedCount++;
                    PinDataType srcType = getPinDataTypeForPin(li.srcPin);
                    juce::Logger::writeToLog(
                        "[CutGesture] HIT: linkId=" + juce::String(linkId) +
                        " srcLid=" + juce::String((int)li.srcPin.logicalId) +
                        " ch=" + juce::String(li.srcPin.channel) +
                        " dstLid=" + juce::String((int)li.dstPin.logicalId) +
                        " ch=" + juce::String(li.dstPin.channel) +
                        " type=" + juce::String(static_cast<int>(srcType)) + " usingPinPos=" +
                        juce::String(usingPinPositions ? 1 : 0) + " t=" + juce::String(t, 3));
                    hits.push_back(Hit{linkId, t, pt, li});
                }
                else
                {
                    skippedIntersect++;
                }
            }
            juce::Logger::writeToLog(
                "[CutGesture] Summary: checked=" + juce::String(checkedCount) +
                " skippedInvalidAttr=" + juce::String(skippedInvalidAttr) + " skippedNoPositions=" +
                juce::String(skippedNoPositions) + " skippedBBox=" + juce::String(skippedBBox) +
                " skippedIntersect=" + juce::String(skippedIntersect) + " skippedEndpoint=" +
                juce::String(skippedEndpoint) + " totalHits=" + juce::String(hits.size()));

            // Merge per-link
            std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
                if (x.linkId != y.linkId)
                    return x.linkId < y.linkId;
                return x.t < y.t;
            });
            std::vector<Hit> merged;
            for (size_t i = 0; i < hits.size(); ++i)
            {
                if (merged.empty())
                {
                    merged.push_back(hits[i]);
                    continue;
                }
                const Hit& prev = merged.back();
                if (prev.linkId == hits[i].linkId)
                {
                    const float dx = hits[i].posGrid.x - prev.posGrid.x;
                    const float dy = hits[i].posGrid.y - prev.posGrid.y;
                    const float distSq = dx * dx + dy * dy;
                    if (distSq < (cutMergeEpsilonPx * cutMergeEpsilonPx))
                        continue;
                }
                merged.push_back(hits[i]);
            }

            if (!merged.empty())
            {
                pushSnapshot();
                for (const auto& h : merged)
                {
                    // Ultra-simple positioning: use midpoint of cut line (where user dragged)
                    // This is the most reliable - user's cut gesture defines the position
                    ImVec2 cutMidpointGrid = ImVec2(
                        (cutStartGrid.x + cutEndGrid.x) * 0.5f,
                        (cutStartGrid.y + cutEndGrid.y) * 0.5f);

                    // Convert to screen space
                    ImVec2 screenPos = ImVec2(
                        lastCanvasP0.x + lastEditorPanning.x + cutMidpointGrid.x,
                        lastCanvasP0.y + lastEditorPanning.y + cutMidpointGrid.y);

                    juce::Logger::writeToLog(
                        "[CutGesture] Inserting reroute at cut midpoint: grid=(" +
                        juce::String(cutMidpointGrid.x, 1) + "," +
                        juce::String(cutMidpointGrid.y, 1) + ") screen=(" +
                        juce::String(screenPos.x, 1) + "," + juce::String(screenPos.y, 1) + ")");

                    insertNodeOnLink("reroute", h.link, screenPos);
                }
                graphNeedsRebuild = true;
            }
        }
    }
#if JUCE_DEBUG
    if (gImNodesNodeDepth != 0 || gImNodesInputDepth != 0 || gImNodesOutputDepth != 0)
    {
        juce::Logger::writeToLog(
            "[ImNodes][DepthLeak][Frame] nodeDepth=" + juce::String(gImNodesNodeDepth) +
            " inputDepth=" + juce::String(gImNodesInputDepth) + " outputDepth=" +
            juce::String(gImNodesOutputDepth) + " lastNode=" + gLastRenderedNodeLabel);
        jassertfalse;
        gImNodesNodeDepth = 0;
        gImNodesInputDepth = 0;
        gImNodesOutputDepth = 0;
    }
#endif
    pinHoveredDuringEditor = ImNodes::IsPinHovered(&hoverPinIdForDrop);
    nodeHoveredDuringEditor = ImNodes::IsNodeHovered(&hoverNodeIdForDrop);
    linkHoveredDuringEditor = ImNodes::IsLinkHovered(&hoverLinkIdForDrop);
    juce::ignoreUnused(hoverPinIdForDrop, hoverNodeIdForDrop, hoverLinkIdForDrop);
    int linkStartAttr = -1;
    if (ImNodes::IsLinkStarted(&linkStartAttr))
    {
        dragInsertActive = true;
        dragInsertStartAttrId = linkStartAttr;
        dragInsertStartPin = decodePinId(linkStartAttr);
        shouldOpenDragInsertPopup = false;
        juce::Logger::writeToLog(
            "[DragInsert] Started drag from attr " + juce::String(linkStartAttr));
    }
    if (dragInsertActive)
    {
        const bool cancelRequested = ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                     ImGui::IsMouseReleased(ImGuiMouseButton_Right);
        if (cancelRequested)
        {
            juce::Logger::writeToLog("[DragInsert] Drag cancelled.");
            cancelDragInsert();
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const bool editorHovered = ImGui::IsWindowHovered(
                ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup);
            if (!pinHoveredDuringEditor && !nodeHoveredDuringEditor && !linkHoveredDuringEditor &&
                editorHovered)
            {
                dragInsertDropPos = ImGui::GetMousePos();
                shouldOpenDragInsertPopup = true;
                juce::Logger::writeToLog(
                    "[DragInsert] Drop captured on canvas (logicalId=" +
                    juce::String((int)dragInsertStartPin.logicalId) +
                    ", channel=" + juce::String(dragInsertStartPin.channel) + ").");
            }
            else
            {
                dragInsertStartAttrId = -1;
                dragInsertStartPin = PinID{};
                shouldOpenDragInsertPopup = false;
            }
            dragInsertActive = false;
        }
    }
    else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        shouldOpenDragInsertPopup = false;
    }
    // === POP THE TRANSPARENT GRID BACKGROUND STYLE ===
    ImNodes::PopColorStyle(); // Pop BoxSelectorOutline
    ImNodes::PopColorStyle(); // Pop BoxSelector
    ImNodes::PopColorStyle(); // Pop GridLinePrimary
    ImNodes::PopColorStyle(); // Pop GridLine
    ImNodes::PopColorStyle(); // Pop GridBackground
    ImNodes::PopColorStyle(); // Pop NodeOutline
    ImNodes::PopColorStyle(); // Pop NodeBackgroundSelected
    ImNodes::PopColorStyle(); // Pop NodeBackgroundHovered
    ImNodes::PopColorStyle(); // Pop NodeBackground
    // === END OF FIX ===
    hasRenderedAtLeastOnce = true;

    if (shouldOpenDragInsertPopup)
    {
        shouldOpenDragInsertPopup = false;
        ImGui::SetNextWindowPos(dragInsertDropPos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("DragInsertPopup");
    }
    if (ImGui::BeginPopup("DragInsertPopup"))
    {
        const PinDataType displayType =
            dragInsertStartPin.isMod ? PinDataType::CV : getPinDataTypeForPin(dragInsertStartPin);
        const auto& suggestions = getDragInsertSuggestionsFor(dragInsertStartPin);

        if (suggestions.empty())
        {
            ImGui::TextDisabled("No compatible modules found.");
            if (ImGui::MenuItem("Close"))
            {
                dragInsertStartAttrId = -1;
                dragInsertStartPin = PinID{};
                ImGui::CloseCurrentPopup();
            }
        }
        else
        {
            ImGui::Text("Insert node for %s", pinDataTypeToString(displayType));
            ImGui::Separator();

            for (const auto& moduleType : suggestions)
            {
                if (ImGui::MenuItem(moduleType.toRawUTF8()))
                {
                    insertNodeFromDragSelection(moduleType);
                    ImGui::CloseCurrentPopup();
                    break;
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Cancel"))
            {
                dragInsertStartAttrId = -1;
                dragInsertStartPin = PinID{};
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    // ======================================================
    // === 💡 MODAL MINIMAP LOGIC (v13 - Scale-on-Press) ====
    // ======================================================
    ImGuiIO& io = ImGui::GetIO();
    bool     isEditorHovered = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    if (consumeShortcutFlag(shortcutToggleMinimapRequested) && !isMinimapEnlarged.load() &&
        isEditorHovered)
    {
        isMinimapEnlarged.store(true);

        ImVec2 minimapCorner =
            ImVec2(lastCanvasP0.x + lastCanvasSize.x, lastCanvasP0.y + lastCanvasSize.y);
        ImVec2 mousePos = io.MousePos;
        float  dist_x = minimapCorner.x - mousePos.x;
        float  dist_y = minimapCorner.y - mousePos.y;
        float  distance = std::sqrt(dist_x * dist_x + dist_y * dist_y);
        float  max_dist =
            std::sqrt(lastCanvasSize.x * lastCanvasSize.x + lastCanvasSize.y * lastCanvasSize.y);

        float norm_dist = 0.0f;
        if (max_dist > 0.0f)
            norm_dist = juce::jlimit(0.0f, 1.0f, distance / max_dist);

        modalMinimapScale = 0.2f + (norm_dist * 0.6f);
    }

    if (ImGui::IsKeyReleased(ImGuiKey_Comma))
    {
        isMinimapEnlarged.store(false);
        modalMinimapScale = 0.2f;
    }

    if (isMinimapEnlarged.load() && !ImGui::IsWindowFocused(ImGuiHoveredFlags_RootAndChildWindows))
    {
        isMinimapEnlarged.store(false);
        modalMinimapScale = 0.2f;
    }
    // ======================================================
    // === 💡 END MODAL MINIMAP LOGIC =======================
    // ======================================================

    // ================== MIDI PLAYER QUICK CONNECT LOGIC ==================
    // Poll all MIDI Player modules for connection requests
    if (synth != nullptr)
    {
        for (const auto& modInfo : synth->getModulesInfo())
        {
            if (auto* midiPlayer = dynamic_cast<MIDIPlayerModuleProcessor*>(
                    synth->getModuleForLogical(modInfo.first)))
            {
                int requestType = midiPlayer->getAndClearConnectionRequest();
                if (requestType > 0)
                {
                    handleMIDIPlayerConnectionRequest(modInfo.first, midiPlayer, requestType);
                    break; // Only handle one request per frame
                }
            }
        }
    }
    // ================== END MIDI PLAYER QUICK CONNECT ==================

    // ================== MIDI CV QUICK CONNECT LOGIC ==================
    // Poll all MIDI CV modules for connection requests
    if (synth != nullptr)
    {
        for (const auto& modInfo : synth->getModulesInfo())
        {
            if (auto* midiCV =
                    dynamic_cast<MIDICVModuleProcessor*>(synth->getModuleForLogical(modInfo.first)))
            {
                int requestType = midiCV->getAndClearConnectionRequest();
                if (requestType > 0)
                {
                    handleMIDICVConnectionRequest(modInfo.first, midiCV, requestType);
                    break; // Only handle one request per frame
                }
            }
        }
    }
    // ================== END MIDI CV QUICK CONNECT ==================
    // ================== META MODULE EDITING LOGIC ==================
    // Check if any Meta Module has requested to be edited
    if (synth != nullptr && metaModuleToEditLid == 0) // Only check if not already editing one
    {
        for (const auto& modInfo : synth->getModulesInfo())
        {
            if (auto* metaModule =
                    dynamic_cast<MetaModuleProcessor*>(synth->getModuleForLogical(modInfo.first)))
            {
                // Atomically check and reset the flag
                if (metaModule->editRequested.exchange(false))
                {
                    metaModuleToEditLid = modInfo.first;
                    juce::Logger::writeToLog(
                        "[MetaEdit] Opening editor for Meta Module L-ID " +
                        juce::String((int)metaModuleToEditLid));
                    ImGui::OpenPopup("Edit Meta Module");
                    break; // Only handle one request per frame
                }
            }
        }
    }

    // Draw the modal popup for the internal editor if one is selected
    if (metaModuleToEditLid != 0)
    {
        ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Edit Meta Module", nullptr, ImGuiWindowFlags_MenuBar))
        {
            ImGui::PushID((int)metaModuleToEditLid);
            auto* metaModule =
                dynamic_cast<MetaModuleProcessor*>(synth->getModuleForLogical(metaModuleToEditLid));
            if (metaModule && metaModule->getInternalGraph())
            {
                if (!metaEditorSession || metaEditorSession->metaLogicalId != metaModuleToEditLid)
                    openMetaModuleEditor(metaModule, metaModuleToEditLid);

                if (metaEditorSession)
                    renderMetaModuleEditor(*metaEditorSession);

                ImGui::Separator();
                if (ImGui::Button("Apply Changes"))
                {
                    if (metaEditorSession && metaEditorSession->dirty)
                    {
                        metaModule->refreshCachedLayout();
                        graphNeedsRebuild = true;
                        snapshotAfterEditor = true;
                    }
                    closeMetaModuleEditor();
                    metaModuleToEditLid = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Close"))
                {
                    closeMetaModuleEditor();
                    metaModuleToEditLid = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ImGui::Text(
                    "Meta module %d has no internal graph to edit.", (int)metaModuleToEditLid);
                if (ImGui::Button("Close"))
                {
                    closeMetaModuleEditor();
                    metaModuleToEditLid = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopID();
            ImGui::EndPopup();
        }
        else
        {
            closeMetaModuleEditor();
            metaModuleToEditLid = 0;
        }
    }
    // ======================= END OF META MODULE LOGIC =======================

    // --- CONSOLIDATED HOVERED LINK DETECTION ---
    // Declare these variables ONCE, immediately after the editor has ended.
    // All subsequent features that need to know about hovered links can now
    // safely reuse these results without causing redefinition or scope errors.
    // Graph is always in consistent state since we rebuild at frame start
    int  hoveredLinkId = -1;
    bool isLinkHovered = ImNodes::IsLinkHovered(&hoveredLinkId);
    // --- END OF CONSOLIDATED DECLARATION ---

    // Smart cable visualization is now integrated directly into the link drawing loop above.
    // No separate overlay needed - cables glow by modifying their own color.

    // === PROBE TOOL MODE HANDLING ===
    if (isProbeModeActive)
    {
        // Change cursor to indicate probe mode is active
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Draw "PROBE ACTIVE" indicator at mouse position
        auto*       drawList = ImGui::GetForegroundDrawList();
        ImVec2      mousePos = ImGui::GetMousePos();
        const char* text = "PROBE MODE: Click output pin";
        auto        textSize = ImGui::CalcTextSize(text);
        ImVec2      textPos = ImVec2(mousePos.x + 20, mousePos.y - 20);
        const auto& theme = ThemeManager::getInstance().getCurrentTheme();
        drawList->AddRectFilled(
            ImVec2(textPos.x - 5, textPos.y - 2),
            ImVec2(textPos.x + textSize.x + 5, textPos.y + textSize.y + 2),
            theme.links.label_background);
        drawList->AddText(textPos, theme.links.label_text, text);

        // Check for pin clicks
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int hoveredPinId = -1;
            if (ImNodes::IsPinHovered(&hoveredPinId) && hoveredPinId != -1)
            {
                auto pinId = decodePinId(hoveredPinId);
                // Check if it's an output pin (not input, not mod)
                if (!pinId.isInput && !pinId.isMod && pinId.logicalId != 0)
                {
                    juce::Logger::writeToLog(
                        "[PROBE_UI] Probe clicked on valid output pin. LogicalID: " +
                        juce::String(pinId.logicalId) +
                        ", Channel: " + juce::String(pinId.channel));
                    auto nodeId = synth->getNodeIdForLogical(pinId.logicalId);
                    synth->setProbeConnection(nodeId, pinId.channel);
                    isProbeModeActive = false; // Deactivate after probing
                }
                else
                {
                    juce::Logger::writeToLog(
                        "[PROBE_UI] Probe clicked on an invalid pin (input or output node). "
                        "Cancelling.");
                    isProbeModeActive = false;
                }
            }
            else
            {
                // Clicked on empty space, cancel probe mode
                juce::Logger::writeToLog("[PROBE_UI] Probe clicked on empty space. Cancelling.");
                isProbeModeActive = false;
            }
        }

        // Allow ESC to cancel probe mode
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            isProbeModeActive = false;
            juce::Logger::writeToLog("[PROBE_UI] Cancelled with ESC");
        }
    }
    // --- CONTEXTUAL RIGHT-CLICK HANDLER ---
    // A cable was right-clicked. Store its info and open the insert popup.
    if (isLinkHovered && hoveredLinkId != -1)
    {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            // User initiated a split. Find the source pin of the hovered link.
            if (auto it = linkIdToAttrs.find(hoveredLinkId); it != linkIdToAttrs.end())
            {
                splittingFromAttrId = it->second.first; // The source attribute ID
                juce::Logger::writeToLog(
                    "[CableSplit] Starting split from attr ID: " +
                    juce::String(splittingFromAttrId));
            }
        }
    }
    // --- END OF CABLE SPLITTING ---

    // 2. If a split-drag is active, handle drawing and completion.
    if (splittingFromAttrId != -1)
    {
        // Draw a line from the source pin to the mouse cursor for visual feedback.
        if (auto it = attrPositions.find(splittingFromAttrId); it != attrPositions.end())
        {
            ImVec2      sourcePos = it->second;
            ImVec2      mousePos = ImGui::GetMousePos();
            const auto& theme = ThemeManager::getInstance().getCurrentTheme();
            ImGui::GetForegroundDrawList()->AddLine(
                sourcePos, mousePos, theme.links.preview_color, theme.links.preview_width);
        }

        // 3. Handle completion or cancellation of the drag.
        // We use Left-click to complete the link, matching ImNodes' default behavior.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            int hoveredPinId = -1;
            if (ImNodes::IsPinHovered(&hoveredPinId) && hoveredPinId != -1)
            {
                // User dropped the link on a pin.
                auto srcPin = decodePinId(splittingFromAttrId);
                auto dstPin = decodePinId(hoveredPinId);

                // Ensure the connection is valid (Output -> Input).
                if (!srcPin.isInput && dstPin.isInput)
                {
                    auto srcNode = synth->getNodeIdForLogical(srcPin.logicalId);
                    auto dstNode = (dstPin.logicalId == 0)
                                       ? synth->getOutputNodeID()
                                       : synth->getNodeIdForLogical(dstPin.logicalId);

                    synth->connect(srcNode, srcPin.channel, dstNode, dstPin.channel);
                    graphNeedsRebuild = true;
                    pushSnapshot(); // Make it undoable
                }
            }

            // ALWAYS reset the state, whether the connection was successful or not.
            splittingFromAttrId = -1;
        }
        // Also allow cancellation with a right-click.
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            splittingFromAttrId = -1; // Cancel the operation.
        }
    }
    // --- END OF NEW LOGIC ---

    // Open popup now (outside editor) if requested this frame
    if (showInsertNodePopup)
    {
        showInsertNodePopup = false;
        // Validate the link still exists
        if (pendingInsertLinkId != -1)
        {
            bool stillValid =
                (/* modLinkIdToRoute.count(pendingInsertLinkId) || */ linkIdToAttrs.count(
                    pendingInsertLinkId));
            if (!stillValid)
            {
                juce::Logger::writeToLog(
                    "[InsertNode] Skipping popup: link disappeared this frame");
                pendingInsertLinkId = -1;
            }
        }
        if (pendingInsertLinkId != -1)
        {
            ImGui::OpenPopup("InsertNodeOnLinkPopup");
            // Consume the mouse release/click so the popup stays open
            ImGui::GetIO().WantCaptureMouse = true;
            juce::Logger::writeToLog("[InsertNode] Opened popup (post-editor)");
        }
        else
        {
            linkToInsertOn = {}; // safety
        }
        pendingInsertLinkId = -1;
    }
    // Fallback: If user right-clicked and a link was hovered this frame, open popup using cached
    // hover
    // Exclude Alt+Right-click to allow cut-by-line gesture to work
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && lastHoveredLinkId != -1 &&
        !ImGui::IsPopupOpen("InsertNodeOnLinkPopup") && !ImGui::GetIO().KeyAlt)
    {
        int id = lastHoveredLinkId;
        linkToInsertOn = {};
        linkToInsertOn.linkId = id;
        bool captured = false;
        // TODO: Handle modulation link deletion for new bus-based system
        // if (auto itM = modLinkIdToRoute.find(id); itM != modLinkIdToRoute.end())
        // {
        //     linkToInsertOn.isMod = true;
        //     int sL, sC, dL; juce::String paramId; std::tie(sL, sC, dL, paramId) = itM->second;
        //     linkToInsertOn.srcLogicalId = (juce::uint32) sL;
        //     linkToInsertOn.srcChan = sC;
        //     linkToInsertOn.dstLogicalId = (juce::uint32) dL;
        //     linkToInsertOn.paramId = paramId;
        //     captured = true;
        //     juce::Logger::writeToLog("[InsertNode][RC-Fallback] Mod link captured id=" +
        //     juce::String(id));
        // }
        // else
        if (auto it = linkIdToAttrs.find(id); it != linkIdToAttrs.end())
        {
            linkToInsertOn.isMod = false;
            linkToInsertOn.srcPin = decodePinId(it->second.first);
            linkToInsertOn.dstPin = decodePinId(it->second.second);
            // Infer modulation vs audio vs video list from pin data types
            const PinDataType srcType = getPinDataTypeForPin(linkToInsertOn.srcPin);
            const PinDataType dstType = getPinDataTypeForPin(linkToInsertOn.dstPin);
            // Check if this is a Video cable (both src and dst are Video)
            if (srcType == PinDataType::Video && dstType == PinDataType::Video)
            {
                linkToInsertOn.isMod = false; // Video cables get their own menu, not audio or mod
            }
            // Check if this is CV/Gate/Raw (but not Video - handled above)
            else if (
                srcType == PinDataType::CV || dstType == PinDataType::CV ||
                srcType == PinDataType::Gate || dstType == PinDataType::Gate ||
                srcType == PinDataType::Raw || dstType == PinDataType::Raw)
            {
                linkToInsertOn.isMod = true;
            }
            captured = true;
            juce::Logger::writeToLog(
                "[InsertNode][RC-Fallback] Link captured id=" + juce::String(id));
        }
        if (captured)
        {
            ImGui::OpenPopup("InsertNodeOnLinkPopup");
            ImGui::GetIO().WantCaptureMouse = true;
            juce::Logger::writeToLog("[InsertNode][RC-Fallback] Opened popup");
        }
        else
        {
            linkToInsertOn.linkId = -1;
        }
    }
    // This function draws the popup if the popup is open.
    drawInsertNodeOnLinkPopup();

    // --- Cable Inspector: Stateless, rebuild-safe implementation ---
    hoveredLinkSrcId = 0;
    hoveredLinkDstId = 0;

    // Skip inspector if popups are open (graph is always in consistent state now)
    const bool anyPopupOpen =
        ImGui::IsPopupOpen("InsertNodeOnLinkPopup") || ImGui::IsPopupOpen("AddModulePopup");
    // Do not early-return here; we still need to finish the frame and close any ImGui scopes.

    if (!anyPopupOpen && synth != nullptr)
    {
        if (isLinkHovered && hoveredLinkId != -1)
        {
            // Safety: Re-verify link still exists in our mapping
            if (auto it = linkIdToAttrs.find(hoveredLinkId); it != linkIdToAttrs.end())
            {
                auto srcPin = decodePinId(it->second.first);
                auto dstPin = decodePinId(it->second.second);

                // Set highlight IDs for this frame only
                hoveredLinkSrcId = srcPin.logicalId;
                hoveredLinkDstId = (dstPin.logicalId == 0) ? kOutputHighlightId : dstPin.logicalId;

                if (auto* srcModule = synth->getModuleForLogical(srcPin.logicalId))
                {
                    const int numOutputs = srcModule->getTotalNumOutputChannels();
                    if (srcPin.channel >= 0 && srcPin.channel < numOutputs)
                    {
                        if (hoveredLinkId != m_currentlyProbedLinkId)
                        {
                            auto sourceNodeId = synth->getNodeIdForLogical(srcPin.logicalId);
                            synth->setProbeConnection(sourceNodeId, srcPin.channel);
                            m_currentlyProbedLinkId = hoveredLinkId;
                        }

                        // Create link info for the tooltip
                        LinkInfo linkInfo;
                        linkInfo.srcLogicalNodeId = srcPin.logicalId;
                        linkInfo.srcNodeId = srcPin.logicalId;
                        linkInfo.srcChannel = srcPin.channel;
                        linkInfo.sourceNodeName = srcModule->getName();
                        linkInfo.pinName = srcModule->getAudioOutputLabel(srcPin.channel);
                        if (linkInfo.pinName.isEmpty())
                            linkInfo.pinName = "Channel " + juce::String(srcPin.channel);

                        ImGui::BeginTooltip();
                        drawLinkInspectorTooltip(linkInfo);
                        ImGui::EndTooltip();
                    }
                    else if (
                        m_currentlyProbedLinkId != -1 && m_currentlyProbedLinkId != hoveredLinkId)
                    {
                        synth->clearProbeConnection();
                        m_currentlyProbedLinkId = -1;
                    }
                }
            }
        }
        else if (m_currentlyProbedLinkId != -1)
        {
            int  hoveredNodeId = -1;
            bool isNodeHovered = ImNodes::IsNodeHovered(&hoveredNodeId);

            int  hoveredPinId = -1;
            bool isPinHovered = ImNodes::IsPinHovered(&hoveredPinId);

            if (!isLinkHovered && !isNodeHovered && !isPinHovered)
            {
                synth->clearProbeConnection();
                m_currentlyProbedLinkId = -1;
            }
        }
    }
    else if (m_currentlyProbedLinkId != -1 && synth != nullptr)
    {
        synth->clearProbeConnection();
        m_currentlyProbedLinkId = -1;
    }
    // Update hovered node/link id for next frame (must be called outside editor scope)
    // Graph is always in consistent state since we rebuild at frame start
    int hv = -1;
    if (ImNodes::IsNodeHovered(&hv))
        lastHoveredNodeId = hv;
    else
        lastHoveredNodeId = -1;
    int hl = -1;
    if (ImNodes::IsLinkHovered(&hl))
        lastHoveredLinkId = hl;
    else
        lastHoveredLinkId = -1;
    // Shortcut: press 'I' while hovering a link to open Insert-on-Link popup (bypasses mouse
    // handling)
    if (consumeShortcutFlag(shortcutInsertOnLinkRequested) && lastHoveredLinkId != -1 &&
        !ImGui::IsPopupOpen("InsertNodeOnLinkPopup"))
    {
        linkToInsertOn = {}; // reset
        linkToInsertOn.linkId = lastHoveredLinkId;
        bool captured = false;
        // TODO: Handle modulation link hover end for new bus-based system
        // if (auto itM = modLinkIdToRoute.find(lastHoveredLinkId); itM != modLinkIdToRoute.end())
        // {
        //     linkToInsertOn.isMod = true;
        //     int sL, sC, dL; juce::String paramId; std::tie(sL, sC, dL, paramId) = itM->second;
        //     linkToInsertOn.srcLogicalId = (juce::uint32) sL;
        //     linkToInsertOn.srcChan = sC;
        //     linkToInsertOn.dstLogicalId = (juce::uint32) dL;
        //     linkToInsertOn.paramId = paramId;
        //     captured = true;
        //     juce::Logger::writeToLog("[InsertNode][KeyI] Mod link captured id=" +
        //     juce::String(lastHoveredLinkId));
        // }
        // else
        if (auto it = linkIdToAttrs.find(lastHoveredLinkId); it != linkIdToAttrs.end())
        {
            linkToInsertOn.isMod = false;
            linkToInsertOn.srcPin = decodePinId(it->second.first);
            linkToInsertOn.dstPin = decodePinId(it->second.second);
            const PinDataType srcType = getPinDataTypeForPin(linkToInsertOn.srcPin);
            const PinDataType dstType = getPinDataTypeForPin(linkToInsertOn.dstPin);
            // Check if this is a Video cable (both src and dst are Video)
            if (srcType == PinDataType::Video && dstType == PinDataType::Video)
            {
                linkToInsertOn.isMod = false; // Video cables get their own menu, not audio or mod
            }
            // Check if this is CV/Gate/Raw (but not Video - handled above)
            else if (
                srcType == PinDataType::CV || dstType == PinDataType::CV ||
                srcType == PinDataType::Gate || dstType == PinDataType::Gate ||
                srcType == PinDataType::Raw || dstType == PinDataType::Raw)
            {
                linkToInsertOn.isMod = true;
            }
            captured = true;
            juce::Logger::writeToLog(
                "[InsertNode][KeyI] Link captured id=" + juce::String(lastHoveredLinkId));
        }
        if (captured)
        {
            pendingInsertLinkId = lastHoveredLinkId;
            showInsertNodePopup = true; // will open next lines
        }
        else
        {
            linkToInsertOn.linkId = -1;
            juce::Logger::writeToLog(
                "[InsertNode][KeyI] No link data found for id=" + juce::String(lastHoveredLinkId));
        }
    }

    // After editor pass, if we added/duplicated a node, take snapshot now that nodes exist
    if (snapshotAfterEditor)
    {
        snapshotAfterEditor = false;
        pushSnapshot();
    }
    if (synth != nullptr)
    {
        // No persistent panning state when zoom is disabled

        // Right-click on empty canvas -> Add module popup
        // Avoid passing nullptr to ImNodes::IsLinkHovered; some builds may write to the pointer
        int        dummyHoveredLinkId = -1;
        const bool anyLinkHovered = ImNodes::IsLinkHovered(&dummyHoveredLinkId);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
            !ImGui::IsAnyItemHovered() && !anyLinkHovered &&
            !ImGui::IsPopupOpen("InsertNodeOnLinkPopup") && linkToInsertOn.linkId == -1 &&
            !ImGui::GetIO().KeyAlt && !cutJustPerformed) // suppress when cut gesture used
        {
            ImGui::OpenPopup("AddModulePopup");
        }
        // Reset suppression flag at end of popup decision
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
            cutJustPerformed = false;
        // --- REVISED AND IMPROVED "QUICK ADD" POPUP ---
        if (ImGui::BeginPopup("AddModulePopup"))
        {
            static char searchQuery[128] = "";
            static int  selectedIndex = 0; // Track keyboard navigation

            // Auto-focus the search bar when the popup opens and clear any previous search
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetWindowFocus();

                if (dragInsertStartAttrId != -1)
                {
                    PinID displayPin = dragInsertStartPin;
                    auto  type =
                        displayPin.isMod ? PinDataType::CV : getPinDataTypeForPin(displayPin);

                    juce::String seed = ":" + juce::String(pinDataTypeToString(type));
                    juce::String modules;
                    const auto&  suggestions = getDragInsertSuggestionsFor(displayPin);
                    for (size_t i = 0; i < suggestions.size(); ++i)
                        modules += ":" + suggestions[i];

                    juce::String tokenized = seed + modules;
                    auto         truncated = tokenized.substring(
                        0, juce::jmin((int)tokenized.length(), (int)sizeof(searchQuery) - 1));
                    std::memset(searchQuery, 0, sizeof(searchQuery));
                    std::memcpy(
                        searchQuery, truncated.toRawUTF8(), (size_t)truncated.getNumBytesAsUTF8());
                }
                else
                {
                    searchQuery[0] = '\0';
                }

                ImGui::SetKeyboardFocusHere(0);
                selectedIndex = 0;
            }

            ImGui::Text("Add Module");
            ImGui::PushItemWidth(250.0f);

            // Enable Enter key detection for instant module creation
            bool enterPressed = ImGui::InputText(
                "Search##addmodule",
                searchQuery,
                sizeof(searchQuery),
                ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::PopItemWidth();
            ImGui::Separator();

            // --- PROBE TOOL ---
            if (ImGui::MenuItem("🔬 Probe Signal (Click any output pin)"))
            {
                isProbeModeActive = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Activate probe mode to instantly visualize any signal without manual "
                    "patching.\nClick on any output pin to route it to the probe scope.");
            }
            ImGui::Separator();

            auto addAtMouse = [this](const char* type) {
                auto         nodeId = synth->addModule(type);
                juce::String nodeName = juce::String(type).replaceCharacter('_', ' ');
                // Capitalize first letter of each word
                nodeName = nodeName.toLowerCase();
                bool capitalizeNext = true;
                for (int i = 0; i < nodeName.length(); ++i)
                {
                    if (capitalizeNext && juce::CharacterFunctions::isLetter(nodeName[i]))
                    {
                        nodeName = nodeName.substring(0, i) +
                                   juce::String::charToString(nodeName[i]).toUpperCase() +
                                   nodeName.substring(i + 1);
                        capitalizeNext = false;
                    }
                    else if (nodeName[i] == ' ')
                        capitalizeNext = true;
                }
                const int logicalId = (int)synth->getLogicalIdForNode(nodeId);
                // This places the new node exactly where the user right-clicked
                pendingNodeScreenPositions[logicalId] = ImGui::GetMousePosOnOpeningCurrentPopup();

                // Special handling for recorder module
                if (juce::String(type).equalsIgnoreCase("recorder"))
                {
                    if (auto* recorder = dynamic_cast<RecordModuleProcessor*>(
                            synth->getModuleForLogical((juce::uint32)logicalId)))
                    {
                        recorder->setPropertiesFile(
                            PresetCreatorApplication::getApp().getProperties());
                    }
                }

                // Give comment nodes a default size to prevent feedback loop
                if (juce::String(type).equalsIgnoreCase("comment"))
                {
                    pendingNodeSizes[logicalId] = ImVec2(250.f, 150.f);
                }

                snapshotAfterEditor = true;
                ImGui::CloseCurrentPopup();
            };

            juce::String filter(searchQuery);

            ImGui::BeginChild("ModuleList", ImVec2(280, 350), true);

            if (filter.isEmpty())
            {
                // --- BROWSE MODE (No text in search bar) ---
                // Reorganized to match the new category structure

                if (ImGui::BeginMenu("Sources"))
                {
                    if (ImGui::MenuItem("VCO"))
                        addAtMouse("vco");
                    if (ImGui::MenuItem("STK String"))
                        addAtMouse("stk_string");
                    if (ImGui::MenuItem("STK Wind"))
                        addAtMouse("stk_wind");
                    if (ImGui::MenuItem("STK Percussion"))
                        addAtMouse("stk_percussion");
                    if (ImGui::MenuItem("STK Plucked"))
                        addAtMouse("stk_plucked");
                    if (ImGui::MenuItem("Polyphonic VCO"))
                        addAtMouse("polyvco");
                    if (ImGui::MenuItem("Noise"))
                        addAtMouse("noise");
                    if (ImGui::MenuItem("Audio Input"))
                        addAtMouse("audio_input");
                    if (ImGui::MenuItem("Sample Loader"))
                        addAtMouse("sample_loader");
                    if (ImGui::MenuItem("Sample SFX"))
                        addAtMouse("sample_sfx");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Internet Radio"))
                        addAtMouse("internet_radio");
                    if (ImGui::MenuItem("System Audio"))
                        addAtMouse("system_audio");
                    if (ImGui::MenuItem("SRT Receiver"))
                        addAtMouse("srt_receiver");
                    if (ImGui::MenuItem("RTMP Receiver"))
                        addAtMouse("rtmp_receiver");
                    if (ImGui::MenuItem("Bluetooth Audio"))
                        addAtMouse("bluetooth_audio");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Value"))
                        addAtMouse("value");
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Effects"))
                {
                    if (ImGui::MenuItem("VCF"))
                        addAtMouse("vcf");
                    if (ImGui::MenuItem("Delay"))
                        addAtMouse("delay");
                    if (ImGui::MenuItem("Reverb"))
                        addAtMouse("reverb");
                    if (ImGui::MenuItem("Chorus"))
                        addAtMouse("chorus");
                    if (ImGui::MenuItem("Spatial Granulator"))
                        addAtMouse("spatial_granulator");
                    if (ImGui::MenuItem("Phaser"))
                        addAtMouse("phaser");
                    if (ImGui::MenuItem("Compressor"))
                        addAtMouse("compressor");
                    if (ImGui::MenuItem("Limiter"))
                        addAtMouse("limiter");
                    if (ImGui::MenuItem("Noise Gate"))
                        addAtMouse("gate");
                    if (ImGui::MenuItem("Reroute"))
                        addAtMouse("reroute");
                    if (ImGui::MenuItem("Drive"))
                        addAtMouse("drive");
                    if (ImGui::MenuItem("Bit Crusher"))
                        addAtMouse("bit_crusher");
                    if (ImGui::MenuItem("Graphic EQ"))
                        addAtMouse("graphic_eq");
                    if (ImGui::MenuItem("Waveshaper"))
                        addAtMouse("waveshaper");
                    if (ImGui::MenuItem("8-Band Shaper"))
                        addAtMouse("8bandshaper");
                    if (ImGui::MenuItem("Granulator"))
                        addAtMouse("granulator");
                    if (ImGui::MenuItem("Harmonic Shaper"))
                        addAtMouse("harmonic_shaper");
                    if (ImGui::MenuItem("Time/Pitch Shifter"))
                        addAtMouse("timepitch");
                    if (ImGui::MenuItem("De-Crackle"))
                        addAtMouse("de_crackle");
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Modulators"))
                {
                    if (ImGui::MenuItem("LFO"))
                        addAtMouse("lfo");
                    if (ImGui::MenuItem("ADSR"))
                        addAtMouse("adsr");
                    if (ImGui::MenuItem("Random"))
                        addAtMouse("random");
                    if (ImGui::MenuItem("S&H"))
                        addAtMouse("s_and_h");
                    if (ImGui::MenuItem("Function Generator"))
                        addAtMouse("function_generator");
                    if (ImGui::MenuItem("Shaping Oscillator"))
                        addAtMouse("shaping_oscillator");
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Utilities & Logic"))
                {
                    if (ImGui::MenuItem("VCA"))
                        addAtMouse("vca");
                    if (ImGui::MenuItem("Mixer"))
                        addAtMouse("mixer");
                    if (ImGui::MenuItem("CV Mixer"))
                        addAtMouse("cv_mixer");
                    if (ImGui::MenuItem("Track Mixer"))
                        addAtMouse("track_mixer");
                    if (ImGui::MenuItem("PanVol"))
                        addAtMouse("panvol");
                    if (ImGui::MenuItem("Attenuverter"))
                        addAtMouse("attenuverter");
                    if (ImGui::MenuItem("Lag Processor"))
                        addAtMouse("lag_processor");
                    if (ImGui::MenuItem("Math"))
                        addAtMouse("math");
                    if (ImGui::MenuItem("Map Range"))
                        addAtMouse("map_range");
                    if (ImGui::MenuItem("Quantizer"))
                        addAtMouse("quantizer");
                    if (ImGui::MenuItem("Rate"))
                        addAtMouse("rate");
                    if (ImGui::MenuItem("Comparator"))
                        addAtMouse("comparator");
                    if (ImGui::MenuItem("Logic"))
                        addAtMouse("logic");
                    if (ImGui::MenuItem("Reroute"))
                        addAtMouse("reroute");
                    if (ImGui::MenuItem("Clock Divider"))
                        addAtMouse("clock_divider");
                    if (ImGui::MenuItem("Sequential Switch"))
                        addAtMouse("sequential_switch");
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Sequencers"))
                {
                    if (ImGui::MenuItem("Sequencer"))
                        addAtMouse("sequencer");
                    if (ImGui::MenuItem("Multi Sequencer"))
                        addAtMouse("multi_sequencer");
                    if (ImGui::MenuItem("Tempo Clock"))
                        addAtMouse("tempo_clock");
                    if (ImGui::MenuItem("Snapshot Sequencer"))
                        addAtMouse("snapshot_sequencer");
                    if (ImGui::MenuItem("Stroke Sequencer"))
                        addAtMouse("stroke_sequencer");
                    if (ImGui::MenuItem("Chord Arp"))
                        addAtMouse("chord_arp");
                    if (ImGui::MenuItem("Timeline"))
                        addAtMouse("timeline");
                    if (ImGui::MenuItem("Automation Lane"))
                        addAtMouse("automation_lane");
                    if (ImGui::MenuItem("Automato"))
                        addAtMouse("automato");
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("MIDI"))
                {
                    if (ImGui::BeginMenu("Players / Editors"))
                    {
                        if (ImGui::MenuItem("MIDI CV"))
                            addAtMouse("midi_cv");
                        if (ImGui::MenuItem("OSC CV"))
                            addAtMouse("osc_cv");
                        if (ImGui::MenuItem("CV -> OSC"))
                            addAtMouse("cv_osc_sender");
                        if (ImGui::MenuItem("MIDI Player"))
                            addAtMouse("midi_player");
                        if (ImGui::MenuItem("MIDI Logger"))
                            addAtMouse("midi_logger");
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Controllers"))
                    {
                        if (ImGui::MenuItem("MIDI Faders"))
                            addAtMouse("midi_faders");
                        if (ImGui::MenuItem("MIDI Knobs"))
                            addAtMouse("midi_knobs");
                        if (ImGui::MenuItem("MIDI Buttons"))
                            addAtMouse("midi_buttons");
                        if (ImGui::MenuItem("MIDI Jog Wheel"))
                            addAtMouse("midi_jog_wheel");
                        if (ImGui::MenuItem("MIDI Pads"))
                            addAtMouse("midi_pads");
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Analysis"))
                {
                    if (ImGui::MenuItem("Scope"))
                        addAtMouse("scope");
                    if (ImGui::MenuItem("Debug"))
                        addAtMouse("debug");
                    if (ImGui::MenuItem("Onset Detector"))
                        addAtMouse("essentia_onset_detector");
                    if (ImGui::MenuItem("Pitch Tracker"))
                        addAtMouse("essentia_pitch_tracker");
                    if (ImGui::MenuItem("Input Debug"))
                        addAtMouse("input_debug");
                    if (ImGui::MenuItem("Frequency Graph"))
                        addAtMouse("frequency_graph");
                    if (ImGui::MenuItem("BPM Monitor"))
                        addAtMouse("bpm_monitor");
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("TTS"))
                {
                    if (ImGui::MenuItem("TTS Performer"))
                        addAtMouse("tts_performer");
                    if (ImGui::MenuItem("Vocal Tract Filter"))
                        addAtMouse("vocal_tract_filter");
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Special"))
                {
                    if (ImGui::MenuItem("Physics"))
                        addAtMouse("physics");
                    if (ImGui::MenuItem("Animation"))
                        addAtMouse("animation");
                    ImGui::EndMenu();
                }
#ifndef AUDIO_ONLY_BUILD
                if (ImGui::BeginMenu("Computer Vision"))
                {
                    if (ImGui::MenuItem("Webcam Loader"))
                        addAtMouse("webcam_loader");
                    if (ImGui::MenuItem("Video File Loader"))
                        addAtMouse("video_file_loader");
                    if (ImGui::MenuItem("NDI Receiver"))
                        addAtMouse("ndi_receiver");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Video FX"))
                        addAtMouse("video_fx");
                    if (ImGui::MenuItem("Chromakey"))
                        addAtMouse("chromakey");
                    if (ImGui::MenuItem("Video Compositor"))
                        addAtMouse("video_compositor");
                    if (ImGui::MenuItem("Video Draw Impact"))
                        addAtMouse("video_draw_impact");
                    if (ImGui::MenuItem("Crop Video"))
                        addAtMouse("crop_video");
                    if (ImGui::MenuItem("Video Viewer"))
                        addAtMouse("video_viewer");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Movement Detector"))
                        addAtMouse("movement_detector");
                    if (ImGui::MenuItem("Object Detector"))
                        addAtMouse("object_detector");
                    if (ImGui::MenuItem("Pose Estimator"))
                        addAtMouse("pose_estimator");
                    if (ImGui::MenuItem("Hand Tracker"))
                        addAtMouse("hand_tracker");
                    if (ImGui::MenuItem("Face Tracker"))
                        addAtMouse("face_tracker");
                    if (ImGui::MenuItem("Color Tracker"))
                        addAtMouse("color_tracker");
                    if (ImGui::MenuItem("Contour Detector"))
                        addAtMouse("contour_detector");
                    ImGui::EndMenu();
                }
#endif

                if (ImGui::BeginMenu("Plugins / VST"))
                {
                    drawVstMenuByManufacturerForAddModule();
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("System"))
                {
                    // if (ImGui::MenuItem("Meta")) addAtMouse("meta");
                    // if (ImGui::MenuItem("Inlet")) addAtMouse("inlet");
                    // if (ImGui::MenuItem("Outlet")) addAtMouse("outlet");
                    if (ImGui::MenuItem("Comment"))
                        addAtMouse("comment");
                    if (ImGui::MenuItem("Recorder"))
                        addAtMouse("recorder");
                    ImGui::EndMenu();
                }
            }
            else
            {
                // --- SEARCH MODE (Text has been entered) ---
                struct MatchedModule
                {
                    juce::String displayName;
                    const char*  internalType;
                    const char*  description;
                };
                std::vector<MatchedModule> matches;

                auto                             registry = getModuleRegistry();
                std::unordered_set<juce::String> matchedInternals;

                auto addMatchByInternal = [&](const juce::String& internal) {
                    if (!matchedInternals.insert(internal).second)
                        return;
                    for (const auto& entry : registry)
                    {
                        if (juce::String(entry.second.first).equalsIgnoreCase(internal))
                        {
                            matches.push_back(
                                {entry.first, entry.second.first, entry.second.second});
                            break;
                        }
                    }
                };

                bool         usingTokenFilter = false;
                juce::String tokenType;

                if (filter.startsWithChar(':'))
                {
                    juce::StringArray tokens;
                    tokens.addTokens(filter, ":", "");
                    tokens.removeEmptyStrings();

                    if (!tokens.isEmpty())
                    {
                        usingTokenFilter = true;
                        tokenType = tokens[0];

                        auto parsePinTypeToken = [](const juce::String& token,
                                                    PinDataType&        outType) -> bool {
                            if (token.equalsIgnoreCase("audio"))
                            {
                                outType = PinDataType::Audio;
                                return true;
                            }
                            if (token.equalsIgnoreCase("cv") || token.equalsIgnoreCase("mod"))
                            {
                                outType = PinDataType::CV;
                                return true;
                            }
                            if (token.equalsIgnoreCase("gate") || token.equalsIgnoreCase("trigger"))
                            {
                                outType = PinDataType::Gate;
                                return true;
                            }
                            if (token.equalsIgnoreCase("raw"))
                            {
                                outType = PinDataType::Raw;
                                return true;
                            }
                            if (token.equalsIgnoreCase("video"))
                            {
                                outType = PinDataType::Video;
                                return true;
                            }
                            return false;
                        };

                        PinDataType parsedType = PinDataType::Raw;
                        const bool  typeParsed = parsePinTypeToken(tokenType, parsedType);

                        for (int i = 1; i < tokens.size(); ++i)
                        {
                            juce::String internal = tokens[i].trim();
                            if (internal.isNotEmpty())
                                addMatchByInternal(internal);
                        }

                        if (matches.empty() && typeParsed)
                        {
                            auto appendFromMap =
                                [&](const std::map<PinDataType, std::vector<juce::String>>&
                                        source) {
                                    if (auto it = source.find(parsedType); it != source.end())
                                    {
                                        for (const auto& internal : it->second)
                                            addMatchByInternal(internal);
                                    }
                                };
                            appendFromMap(dragInsertSuggestionsInputs);
                            appendFromMap(dragInsertSuggestionsOutputs);
                        }

                        if (matches.empty())
                        {
                            usingTokenFilter = false;
                        }
                        else
                        {
                            juce::String label = tokenType.isNotEmpty() ? tokenType : "signal";
                            ImGui::TextDisabled("Suggestions for %s", label.toRawUTF8());
                            ImGui::Separator();
                        }
                    }
                }

                if (!usingTokenFilter)
                {
                    for (const auto& entry : registry)
                    {
                        const juce::String& displayName = entry.first;
                        const char*         internalType = entry.second.first;
                        const char*         description = entry.second.second;

                        if (displayName.containsIgnoreCase(filter) ||
                            juce::String(internalType).containsIgnoreCase(filter))
                        {
                            if (matchedInternals.insert(juce::String(internalType)).second)
                                matches.push_back({displayName, internalType, description});
                        }
                    }
                }

                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                {
                    selectedIndex++;
                    if (selectedIndex >= (int)matches.size())
                        selectedIndex = (int)matches.size() - 1;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                {
                    selectedIndex--;
                    if (selectedIndex < 0)
                        selectedIndex = 0;
                }
                if (enterPressed && !matches.empty())
                {
                    if (selectedIndex >= 0 && selectedIndex < (int)matches.size())
                    {
                        addAtMouse(matches[selectedIndex].internalType);
                    }
                }

                for (int i = 0; i < (int)matches.size(); ++i)
                {
                    const auto& match = matches[i];
                    bool        isSelected = (i == selectedIndex);

                    if (ImGui::Selectable(match.displayName.toRawUTF8(), isSelected))
                    {
                        addAtMouse(match.internalType);
                    }

                    if (isSelected && !ImGui::IsItemVisible())
                        ImGui::SetScrollHereY(0.5f);

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(match.description);
                        ImGui::EndTooltip();
                    }
                }

                if (selectedIndex >= (int)matches.size())
                    selectedIndex = (int)matches.size() - 1;
                if (selectedIndex < 0 && !matches.empty())
                    selectedIndex = 0;
            }

            ImGui::EndChild();
            ImGui::EndPopup();
        }

        // Helper functions are now class methods

        // Handle user-created links (must be called after EndNodeEditor)
        int startAttr = 0, endAttr = 0;
        if (ImNodes::IsLinkCreated(&startAttr, &endAttr))
        {
            auto startPin = decodePinId(startAttr);
            auto endPin = decodePinId(endAttr);
            auto srcPin = startPin.isInput ? endPin : startPin;
            auto dstPin = startPin.isInput ? startPin : endPin;

            // Ensure connection is always Output -> Input
            if (!srcPin.isInput && dstPin.isInput)
            {
                PinDataType srcType = getPinDataTypeForPin(srcPin);

                // CRITICAL: Update reroute node type BEFORE checking compatibility
                // This ensures reroute nodes adopt the source type before type checks
                if (dstPin.logicalId != 0 &&
                    getTypeForLogical(dstPin.logicalId).equalsIgnoreCase("reroute"))
                {
                    if (auto* reroute = dynamic_cast<RerouteModuleProcessor*>(
                            synth->getModuleForLogical(dstPin.logicalId)))
                    {
                        reroute->setPassthroughType(srcType);
                    }
                }

                // Re-query dstType after potential reroute update
                PinDataType dstType = getPinDataTypeForPin(dstPin);

                bool conversionHandled = false;

                // Determine if a converter is needed based on pin types
                if (srcType == PinDataType::Audio && dstType == PinDataType::CV)
                {
                    insertNodeBetween("attenuverter", srcPin, dstPin);
                    conversionHandled = true;
                }
                else if (srcType == PinDataType::CV && dstType == PinDataType::Gate)
                {
                    insertNodeBetween("comparator", srcPin, dstPin);
                    conversionHandled = true;
                }
                else if (srcType == PinDataType::Audio && dstType == PinDataType::Gate)
                {
                    insertNodeBetween("comparator", srcPin, dstPin);
                    conversionHandled = true;
                }
                else if (srcType == PinDataType::Raw && dstType != PinDataType::Raw)
                {
                    insertNodeBetween("map_range", srcPin, dstPin);
                    conversionHandled = true;
                }

                if (conversionHandled)
                {
                    pushSnapshot();
                }
                else
                {
                    // All other combinations are considered directly compatible.
                    if (!dstPin.isMod && dstPin.isInput && dstPin.logicalId != 0)
                    {
                        if (getTypeForLogical(dstPin.logicalId).equalsIgnoreCase("reroute"))
                            updateRerouteTypeFromConnections(dstPin.logicalId);
                    }

                    auto srcNode = synth->getNodeIdForLogical(srcPin.logicalId);
                    auto dstNode = (dstPin.logicalId == 0)
                                       ? synth->getOutputNodeID()
                                       : synth->getNodeIdForLogical(dstPin.logicalId);

                    synth->connect(srcNode, srcPin.channel, dstNode, dstPin.channel);
                    // Immediate commit for RecordModuleProcessor filename update
                    synth->commitChanges();

                    if (auto* dstModule = synth->getModuleForLogical(dstPin.logicalId))
                    {
                        if (auto* recorder = dynamic_cast<RecordModuleProcessor*>(dstModule))
                        {
                            juce::String sourceName;
                            if (auto* srcModule = synth->getModuleForLogical(srcPin.logicalId))
                            {
                                sourceName = srcModule->getName();
                            }
                            recorder->updateSuggestedFilename(sourceName);
                        }
                    }

                    auto updateRerouteForPin = [&](const PinID& pin) {
                        if (pin.logicalId != 0 &&
                            getTypeForLogical(pin.logicalId).equalsIgnoreCase("reroute"))
                            updateRerouteTypeFromConnections(pin.logicalId);
                    };
                    updateRerouteForPin(srcPin);
                    updateRerouteForPin(dstPin);

                    pushSnapshot();
                }
            }
        }

        // Handle link deletion (single)
        int linkId = 0;
        if (ImNodes::IsLinkDestroyed(&linkId))
        {
            if (auto it = linkIdToAttrs.find(linkId); it != linkIdToAttrs.end())
            {
                auto srcPin = decodePinId(it->second.first);
                auto dstPin = decodePinId(it->second.second);

                auto srcNode = synth->getNodeIdForLogical(srcPin.logicalId);
                auto dstNode = (dstPin.logicalId == 0)
                                   ? synth->getOutputNodeID()
                                   : synth->getNodeIdForLogical(dstPin.logicalId);

                // Debug log disconnect intent
                juce::Logger::writeToLog(
                    juce::String("[LinkDelete] src(lid=") + juce::String((int)srcPin.logicalId) +
                    ",ch=" + juce::String(srcPin.channel) +
                    ") -> dst(lid=" + juce::String((int)dstPin.logicalId) +
                    ",ch=" + juce::String(dstPin.channel) + ")");

                synth->disconnect(srcNode, srcPin.channel, dstNode, dstPin.channel);

                // Immediate commit for RecordModuleProcessor filename update
                synth->commitChanges();

                auto updateRerouteForPin = [&](const PinID& pin) {
                    if (pin.logicalId != 0 &&
                        getTypeForLogical(pin.logicalId).equalsIgnoreCase("reroute"))
                        updateRerouteTypeFromConnections(pin.logicalId);
                };
                updateRerouteForPin(srcPin);
                updateRerouteForPin(dstPin);

                // After disconnecting, tell the recorder to update (pass empty string for
                // unconnected)
                if (auto* dstModule = synth->getModuleForLogical(dstPin.logicalId))
                {
                    if (auto* recorder = dynamic_cast<RecordModuleProcessor*>(dstModule))
                    {
                        recorder->updateSuggestedFilename(""); // Empty = unconnected
                    }
                }

                pushSnapshot();
                linkIdToAttrs.erase(it);
            }
        }
        // Handle link deletion (multi-select via Delete)
        // Keyboard shortcuts
        // Only process global keyboard shortcuts if no ImGui widget wants the keyboard
        if (!ImGui::GetIO().WantCaptureKeyboard)
        {
            if (consumeShortcutFlag(shortcutFileSaveAsRequested))
            {
                startSaveDialog();
            }
            if (consumeShortcutFlag(shortcutFileSaveRequested))
            {
                if (currentPresetFile.existsAsFile())
                    savePresetToFile(currentPresetFile);
                else
                    startSaveDialog();
            }
            if (consumeShortcutFlag(shortcutNewCanvasRequested))
            {
                newCanvas();
            }
            if (consumeShortcutFlag(shortcutFileOpenRequested))
            {
                startLoadDialog();
            }
            if (consumeShortcutFlag(shortcutRandomizePatchRequested))
            {
                handleRandomizePatch();
            }
            if (consumeShortcutFlag(shortcutRandomizeConnectionsRequested))
            {
                handleRandomizeConnections();
            }
            if (consumeShortcutFlag(shortcutBeautifyLayoutRequested))
            {
                handleBeautifyLayout();
            }

            if (consumeShortcutFlag(shortcutMuteSelectionRequested) &&
                ImNodes::NumSelectedNodes() > 0)
            {
                handleMuteToggle();
            }

            if (consumeShortcutFlag(shortcutSelectAllRequested))
            {
                if (synth != nullptr)
                {
                    const auto&      modules = synth->getModulesInfo();
                    std::vector<int> allNodeIds;
                    allNodeIds.push_back(0);
                    for (const auto& mod : modules)
                        allNodeIds.push_back((int)mod.first);

                    ImNodes::ClearNodeSelection();
                    for (int id : allNodeIds)
                        ImNodes::SelectNode(id);
                }
            }

            if (consumeShortcutFlag(shortcutChainSequentialRequested) &&
                ImNodes::NumSelectedNodes() > 1)
            {
                handleNodeChaining();
            }

            if (consumeShortcutFlag(shortcutChainAudioRequested) && ImNodes::NumSelectedNodes() > 1)
            {
                handleColorCodedChaining(PinDataType::Audio);
            }

            if (consumeShortcutFlag(shortcutChainCvRequested) && ImNodes::NumSelectedNodes() > 1)
            {
                handleColorCodedChaining(PinDataType::CV);
            }

            if (consumeShortcutFlag(shortcutChainGateRequested) && ImNodes::NumSelectedNodes() > 1)
            {
                handleColorCodedChaining(PinDataType::Gate);
            }

            if (consumeShortcutFlag(shortcutChainRawRequested) && ImNodes::NumSelectedNodes() > 1)
            {
                handleColorCodedChaining(PinDataType::Raw);
            }

            if (consumeShortcutFlag(shortcutChainVideoRequested) && ImNodes::NumSelectedNodes() > 1)
            {
                handleColorCodedChaining(PinDataType::Video);
            }

            if (consumeShortcutFlag(shortcutRecordOutputRequested))
            {
                handleRecordOutput();
            }

            if (consumeShortcutFlag(shortcutResetNodeRequested))
            {
                const int numSelected = ImNodes::NumSelectedNodes();
                if (numSelected > 0 && synth != nullptr)
                {
                    pushSnapshot();

                    std::vector<int> selectedNodeIds(numSelected);
                    ImNodes::GetSelectedNodes(selectedNodeIds.data());

                    for (int lid : selectedNodeIds)
                    {
                        if (auto* module = synth->getModuleForLogical((juce::uint32)lid))
                        {
                            auto& params = module->getParameters();
                            for (auto* paramBase : params)
                            {
                                if (auto* param =
                                        dynamic_cast<juce::RangedAudioParameter*>(paramBase))
                                    param->setValueNotifyingHost(param->getDefaultValue());
                            }
                            juce::Logger::writeToLog(
                                "[Reset] Reset parameters for node " + juce::String(lid));
                        }
                    }
                }
            }
            if (consumeShortcutFlag(shortcutConnectOutputRequested) &&
                ImNodes::NumSelectedNodes() == 1)
            {
                if (synth != nullptr)
                {
                    int selectedId = 0;
                    ImNodes::GetSelectedNodes(&selectedId);
                    if (selectedId != 0)
                    {
                        synth->connect(
                            synth->getNodeIdForLogical(selectedId), 0, synth->getOutputNodeID(), 0);
                        synth->connect(
                            synth->getNodeIdForLogical(selectedId), 1, synth->getOutputNodeID(), 1);
                        graphNeedsRebuild = true;
                        pushSnapshot();
                    }
                }
            }

            if (consumeShortcutFlag(shortcutDisconnectRequested) && ImNodes::NumSelectedNodes() > 0)
            {
                if (synth != nullptr)
                {
                    std::vector<int> selectedNodeIds(ImNodes::NumSelectedNodes());
                    ImNodes::GetSelectedNodes(selectedNodeIds.data());
                    for (int id : selectedNodeIds)
                        synth->clearConnectionsForNode(synth->getNodeIdForLogical(id));

                    graphNeedsRebuild = true;
                    pushSnapshot();
                }
            }
            auto frameNodes = [&](const std::vector<int>& nodeIds) {
                if (nodeIds.empty() || synth == nullptr)
                    return;

                juce::Rectangle<float> bounds;
                bool                   foundAny = false;

                std::unordered_set<int> validNodes;
                validNodes.insert(0);
                for (const auto& mod : synth->getModulesInfo())
                    validNodes.insert((int)mod.first);

                for (int nodeId : nodeIds)
                {
                    if (validNodes.find(nodeId) != validNodes.end())
                    {
                        ImVec2 pos = ImNodes::GetNodeGridSpacePos(nodeId);
                        if (!foundAny)
                        {
                            bounds = juce::Rectangle<float>(pos.x, pos.y, 1, 1);
                            foundAny = true;
                        }
                        else
                        {
                            bounds = bounds.getUnion(juce::Rectangle<float>(pos.x, pos.y, 1, 1));
                        }
                    }
                }

                if (!foundAny)
                    return;

                if (!nodeIds.empty() && validNodes.find(nodeIds[0]) != validNodes.end())
                    bounds = bounds.expanded(
                        ImNodes::GetNodeDimensions(nodeIds[0]).x,
                        ImNodes::GetNodeDimensions(nodeIds[0]).y);

                ImVec2 center(
                    (bounds.getX() + bounds.getRight()) * 0.5f,
                    (bounds.getY() + bounds.getBottom()) * 0.5f);
                ImNodes::EditorContextResetPanning(center);
            };

            if (consumeShortcutFlag(shortcutFrameSelectionRequested))
            {
                const int numSelected = ImNodes::NumSelectedNodes();
                if (numSelected > 0)
                {
                    std::vector<int> selectedNodeIds(numSelected);
                    ImNodes::GetSelectedNodes(selectedNodeIds.data());
                    frameNodes(selectedNodeIds);
                }
            }

            if (consumeShortcutFlag(shortcutFrameAllRequested))
            {
                if (synth != nullptr)
                {
                    auto             modules = synth->getModulesInfo();
                    std::vector<int> allNodeIds;
                    allNodeIds.push_back(0);
                    for (const auto& mod : modules)
                        allNodeIds.push_back((int)mod.first);
                    frameNodes(allNodeIds);
                }
            }

            if (consumeShortcutFlag(shortcutResetOriginRequested))
            {
                ImNodes::EditorContextResetPanning(ImVec2(0, 0));
            }

            if (consumeShortcutFlag(shortcutToggleDebugRequested))
            {
                showDebugMenu = !showDebugMenu;
            }

            if (consumeShortcutFlag(shortcutUndoRequested))
            {
                if (undoStack.size() > 1)
                {
                    Snapshot current = undoStack.back();
                    redoStack.push_back(current);
                    undoStack.pop_back();
                    restoreSnapshot(undoStack.back());
                    linkIdToAttrs.clear();
                    NotificationManager::post(NotificationManager::Type::Info, "Undo");
                }
            }

            if (consumeShortcutFlag(shortcutRedoRequested))
            {
                if (!redoStack.empty())
                {
                    Snapshot s = redoStack.back();
                    redoStack.pop_back();
                    restoreSnapshot(s);
                    undoStack.push_back(s);
                    linkIdToAttrs.clear();
                    NotificationManager::post(NotificationManager::Type::Info, "Redo");
                }
            }

            const bool duplicateRequested = consumeShortcutFlag(shortcutDuplicateRequested);
            const bool duplicateWithRoutingRequested =
                consumeShortcutFlag(shortcutDuplicateWithRoutingRequested);
            if (duplicateRequested || duplicateWithRoutingRequested)
            {
                const bool copyConnections = duplicateWithRoutingRequested;
                const int  n = ImNodes::NumSelectedNodes();
                if (n > 0)
                {
                    std::vector<int> sel((size_t)n);
                    ImNodes::GetSelectedNodes(sel.data());
                    for (int oldId : sel)
                    {
                        if (oldId == 0)
                            continue;

                        const juce::String type = getTypeForLogical((juce::uint32)oldId);
                        if (type.isEmpty())
                            continue;

                        auto newNodeId = synth->addModule(type);
                        graphNeedsRebuild = true;
                        const juce::uint32 newLogical = synth->getLogicalIdForNode(newNodeId);
                        if (newLogical != 0)
                        {
                            if (auto* src = synth->getModuleForLogical((juce::uint32)oldId))
                                if (auto* dst = synth->getModuleForLogical(newLogical))
                                    dst->getAPVTS().replaceState(src->getAPVTS().copyState());

                            ImVec2 pos = ImNodes::GetNodeGridSpacePos(oldId);
                            pendingNodePositions[(int)newLogical] =
                                ImVec2(pos.x + 40.0f, pos.y + 40.0f);

                            if (copyConnections)
                            {
                                const auto oldNode =
                                    synth->getNodeIdForLogical((juce::uint32)oldId);
                                const auto newNode = newNodeId;
                                for (const auto& c : synth->getConnectionsInfo())
                                {
                                    if ((int)c.srcLogicalId == oldId)
                                    {
                                        auto dstNode =
                                            (c.dstLogicalId == 0)
                                                ? synth->getOutputNodeID()
                                                : synth->getNodeIdForLogical(c.dstLogicalId);
                                        synth->connect(newNode, c.srcChan, dstNode, c.dstChan);
                                    }
                                    if ((int)c.dstLogicalId == oldId)
                                    {
                                        auto srcNode = synth->getNodeIdForLogical(c.srcLogicalId);
                                        synth->connect(srcNode, c.srcChan, newNode, c.dstChan);
                                    }
                                }
                                // TODO: Implement modulation route duplication for new bus-based
                                // system
                            }
                        }
                    }

                    pushSnapshot();
                    NotificationManager::post(
                        NotificationManager::Type::Info,
                        "Duplicated " + juce::String(n) + " node(s)");
                }
            }

        } // End of keyboard shortcuts (WantCaptureKeyboard check)

        // Update selection for parameter panel
        {
            int selCount = ImNodes::NumSelectedNodes();
            if (selCount > 0)
            {
                std::vector<int> ids((size_t)selCount);
                ImNodes::GetSelectedNodes(ids.data());
                selectedLogicalId = ids.back();
            }
            else
            {
                selectedLogicalId = 0;
            }
        }

        handleDeletion();
    }
    // === MIDI DEVICE MANAGER WINDOW ===
    if (showMidiDeviceManager)
    {
        if (ImGui::Begin(
                "MIDI Device Manager", &showMidiDeviceManager, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ThemeText("MIDI Input Devices", theme.text.section_header);
            ImGui::Separator();

            // Access MidiDeviceManager from PresetCreatorComponent
            auto* presetCreator = dynamic_cast<PresetCreatorComponent*>(getParentComponent());
            if (presetCreator && presetCreator->midiDeviceManager)
            {
                auto&       midiMgr = *presetCreator->midiDeviceManager;
                const auto& devices = midiMgr.getDevices();

                if (devices.empty())
                {
                    ImGui::TextDisabled("No MIDI devices found");
                }
                else
                {
                    ImGui::Text("Found %d device(s):", (int)devices.size());
                    ImGui::Spacing();

                    // Display each device
                    for (const auto& device : devices)
                    {
                        ImGui::PushID(device.identifier.toRawUTF8());

                        // Checkbox to enable/disable device
                        bool enabled = device.enabled;
                        if (ImGui::Checkbox("##enabled", &enabled))
                        {
                            if (enabled)
                                midiMgr.enableDevice(device.identifier);
                            else
                                midiMgr.disableDevice(device.identifier);
                        }

                        ImGui::SameLine();

                        // Device name
                        ImGui::Text("%s", device.name.toRawUTF8());

                        // Activity indicator
                        auto activity = midiMgr.getDeviceActivity(device.identifier);
                        if (activity.lastMessageTime > 0)
                        {
                            ImGui::SameLine();
                            float timeSinceMessage =
                                (juce::Time::getMillisecondCounter() - activity.lastMessageTime) /
                                1000.0f;
                            if (timeSinceMessage < 1.0f)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.active);
                                ImGui::Text("ACTIVE");
                                ImGui::PopStyleColor();
                            }
                            else
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
                                ImGui::Text("idle");
                                ImGui::PopStyleColor();
                            }
                        }

                        ImGui::PopID();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Rescan button
                if (ImGui::Button("Rescan Devices"))
                {
                    midiMgr.scanDevices();
                }

                ImGui::SameLine();

                // Enable/Disable all buttons
                if (ImGui::Button("Enable All"))
                {
                    midiMgr.enableAllDevices();
                }

                ImGui::SameLine();

                if (ImGui::Button("Disable All"))
                {
                    midiMgr.disableAllDevices();
                }
            }
            else
            {
                ImGui::TextDisabled("MIDI Manager not available");
            }
        }
        ImGui::End();
    }

    // === OSC DEVICE MANAGER WINDOW ===
    if (showOscDeviceManager)
    {
        if (ImGui::Begin(
                "OSC Device Manager", &showOscDeviceManager, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ThemeText("OSC Input Devices", theme.text.section_header);
            ImGui::Separator();

            // Access OscDeviceManager from PresetCreatorComponent
            auto* presetCreator = dynamic_cast<PresetCreatorComponent*>(getParentComponent());
            if (presetCreator && presetCreator->oscDeviceManager)
            {
                auto&       oscMgr = *presetCreator->oscDeviceManager;
                const auto& devices = oscMgr.getDevices();

                if (devices.empty())
                {
                    ImGui::TextDisabled("No OSC devices configured");
                    ImGui::Spacing();
                    ImGui::TextDisabled("Click 'Add Device' to create a new OSC receiver");
                }
                else
                {
                    ImGui::Text("Configured %d device(s):", (int)devices.size());
                    ImGui::Spacing();

                    // Display each device
                    for (const auto& device : devices)
                    {
                        ImGui::PushID(device.identifier.toRawUTF8());

                        // Checkbox to enable/disable device
                        bool enabled = device.enabled;
                        if (ImGui::Checkbox("##enabled", &enabled))
                        {
                            if (enabled)
                                oscMgr.enableDevice(device.identifier);
                            else
                                oscMgr.disableDevice(device.identifier);
                        }

                        ImGui::SameLine();

                        // Device name
                        ImGui::Text("%s", device.name.toRawUTF8());

                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
                        ImGui::Text("(%s : %d)", "localhost", device.port);
                        ImGui::PopStyleColor();

                        // Activity indicator
                        auto activity = oscMgr.getDeviceActivity(device.identifier);
                        if (activity.lastMessageTime > 0)
                        {
                            ImGui::SameLine();
                            float timeSinceMessage =
                                (juce::Time::getMillisecondCounter() - activity.lastMessageTime) /
                                1000.0f;
                            if (timeSinceMessage < 1.0f)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.active);
                                ImGui::Text("ACTIVE");
                                ImGui::PopStyleColor();

                                // Show last address received
                                if (!activity.lastAddress.isEmpty())
                                {
                                    ImGui::SameLine();
                                    ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
                                    ImGui::Text(": %s", activity.lastAddress.toRawUTF8());
                                    ImGui::PopStyleColor();
                                }
                            }
                            else
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.disabled);
                                ImGui::Text("idle");
                                ImGui::PopStyleColor();
                            }
                        }

                        // Remove button
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                        ImGui::PushStyleColor(
                            ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                        if (ImGui::Button("Remove##remove"))
                        {
                            oscMgr.removeDevice(device.identifier);
                        }
                        ImGui::PopStyleColor(2);

                        ImGui::PopID();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Add Device button (opens a simple dialog)
                static bool showAddDeviceDialog = false;
                static char deviceNameBuf[256] = "OSC Device";
                static int  devicePort = 57120;

                if (ImGui::Button("Add Device..."))
                {
                    showAddDeviceDialog = true;
                    juce::String("OSC Device").copyToUTF8(deviceNameBuf, 256);
                    devicePort = 57120;
                }

                // Add Device Dialog
                if (showAddDeviceDialog)
                {
                    ImGui::OpenPopup("Add OSC Device");
                }

                if (ImGui::BeginPopupModal(
                        "Add OSC Device", &showAddDeviceDialog, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::Text("Add New OSC Device");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Device Name:");
                    ImGui::InputText("##name", deviceNameBuf, 256);

                    ImGui::Spacing();
                    ImGui::Text("Port:");
                    ImGui::InputInt("##port", &devicePort, 1, 100);
                    devicePort = juce::jlimit(1024, 65535, devicePort); // Valid port range

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("Add", ImVec2(120, 0)))
                    {
                        juce::String name(deviceNameBuf);
                        if (name.isEmpty())
                            name = "OSC Device";

                        juce::String identifier = oscMgr.addDevice(name, devicePort);
                        if (identifier.isNotEmpty())
                        {
                            // Auto-enable the new device
                            oscMgr.enableDevice(identifier);
                            showAddDeviceDialog = false;
                        }
                        else
                        {
                            // Port might be in use - show error (could be improved with a proper
                            // error dialog)
                            juce::Logger::writeToLog(
                                "[OSC] Failed to add device - port " + juce::String(devicePort) +
                                " may be in use");
                        }
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0)))
                    {
                        showAddDeviceDialog = false;
                    }

                    ImGui::EndPopup();
                }
            }
            else
            {
                ImGui::TextDisabled("OSC Manager not available");
            }
        }
        ImGui::End();
    }

    // === DEBUG WINDOW ===
    if (showDebugMenu)
    {
        if (ImGui::Begin("System Diagnostics", &showDebugMenu))
        {
            if (synth != nullptr)
            {
                ImGui::Text("=== SYSTEM OVERVIEW ===");
                if (ImGui::Button("Refresh"))
                {
                    // Force refresh of diagnostics
                }

                // System diagnostics
                ImGui::Text("System State:");
                juce::String systemDiag = synth->getSystemDiagnostics();
                ImGui::TextWrapped("%s", systemDiag.toUTF8());

                // Module selector
                ImGui::Text("Module Diagnostics:");
                auto modules = synth->getModulesInfo();
                if (!modules.empty())
                {
                    static int selectedModuleIndex = 0;
                    if (selectedModuleIndex >= (int)modules.size())
                        selectedModuleIndex = 0;

                    juce::String moduleList;
                    for (size_t i = 0; i < modules.size(); ++i)
                    {
                        if (i > 0)
                            moduleList += "\0";
                        moduleList += "Logical " + juce::String((int)modules[i].first) + ": " +
                                      modules[i].second;
                    }
                    moduleList += "\0";

                    ImGui::Combo("Select Module", &selectedModuleIndex, moduleList.toUTF8());

                    if (selectedModuleIndex >= 0 && selectedModuleIndex < (int)modules.size())
                    {
                        juce::String moduleDiag =
                            synth->getModuleDiagnostics(modules[selectedModuleIndex].first);
                        if (moduleDiag.isNotEmpty())
                            ImGui::TextWrapped("%s", moduleDiag.toUTF8());
                        else
                            ImGui::TextDisabled("No diagnostics available for this module.");
                    }
                }
                else
                {
                    ImGui::Text("No modules found.");
                }
            }
            else
            {
                ImGui::Text("No synth processor available.");
            }
        }
        ImGui::End();
    }

    if (showLogViewer)
    {
        if (ImGui::Begin("Log Viewer", &showLogViewer))
        {
            if (ImGui::Button("Refresh"))
            {
                refreshLogViewerContent();
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy All"))
            {
                ImGui::SetClipboardText(logViewerContent.toRawUTF8());
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &logViewerAutoScroll);

            ImGui::Separator();

            ImGui::BeginChild(
                "LogViewerScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(logViewerContent.toRawUTF8());
            if (logViewerAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f)
            {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    ImGui::End();

    // === POP THE TRANSPARENT BACKGROUND STYLE ===
    ImGui::PopStyleColor();
    // === END OF FIX ===

    // Render notification system (must be called at the end to appear on top)
    NotificationManager::render();
    // --- Phase 5: Periodic Stale History Cleanup ---
    static double lastCleanupTime = 0.0;
    const double  currentTimeSec = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (currentTimeSec - lastCleanupTime > 10.0) // Run every 10 seconds
    {
        lastCleanupTime = currentTimeSec;
        // Set cutoff for "stale" entries (2x the max window size = 40 seconds)
        const double staleCutoffTime = currentTimeSec - (20.0 * 2.0);

        for (auto it = inspectorHistory.begin(); it != inspectorHistory.end(); /* no increment */)
        {
            if (it->second.lastAccessTime < staleCutoffTime)
            {
                it = inspectorHistory.erase(it); // Erase stale entry
            }
            else
            {
                ++it;
            }
        }
    }

    // No deferred snapshots; unified pre-state strategy
}

void ImGuiNodeEditorComponent::refreshLogViewerContent()
{
    logViewerContent = "File logger is not active.";

    if (auto* currentLogger = juce::Logger::getCurrentLogger())
    {
        if (auto* fileLogger = dynamic_cast<juce::FileLogger*>(currentLogger))
        {
            auto logFile = fileLogger->getLogFile();
            if (logFile.existsAsFile())
            {
                auto          logText = logFile.loadFileAsString();
                constexpr int maxChars = 200000;
                if (logText.length() > maxChars)
                    logText = logText.substring(logText.length() - maxChars);
                logViewerContent = logText;
            }
            else
            {
                logViewerContent = "Log file not found:\n" + logFile.getFullPathName();
            }
        }
    }
}

void ImGuiNodeEditorComponent::rebuildFontAtlas()
{
    ImGuiIO& io = ImGui::GetIO();
    ThemeManager::getInstance().applyFonts(io);
    ImGui_ImplOpenGL2_DestroyDeviceObjects();
    ImGui_ImplOpenGL2_CreateDeviceObjects();
}

void ImGuiNodeEditorComponent::pushSnapshot()
{
    // Ensure any newly scheduled positions are flushed into the current UI state
    // by applying them immediately before capturing.
    if (!pendingNodePositions.empty())
    {
        // Temporarily mask rebuild flag to avoid ImNodes queries during capture
        const bool rebuilding = graphNeedsRebuild.load();
        if (rebuilding)
        {
            // getUiValueTree will still avoid ImNodes now, but assert safety
        }
        juce::ValueTree applied = getUiValueTree();
        for (const auto& kv : pendingNodePositions)
        {
            // Overwrite the entry for this node if present
            for (int i = 0; i < applied.getNumChildren(); ++i)
            {
                auto n = applied.getChild(i);
                if (n.hasType("node") && (int)n.getProperty("id", -1) == kv.first)
                {
                    n.setProperty("x", kv.second.x, nullptr);
                    n.setProperty("y", kv.second.y, nullptr);
                    break;
                }
            }
        }
        // Do not commit pending positions of (0,0) which are placeholders
        for (int i = 0; i < applied.getNumChildren(); ++i)
        {
            auto n = applied.getChild(i);
            if (!n.hasType("node"))
                continue;
            const float x = (float)n.getProperty("x", 0.0f);
            const float y = (float)n.getProperty("y", 0.0f);
            if (x == 0.0f && y == 0.0f)
            {
                // Try to recover from last-known or pending
                const int nid = (int)n.getProperty("id", -1);
                auto      itL = lastKnownNodePositions.find(nid);
                if (itL != lastKnownNodePositions.end())
                {
                    n.setProperty("x", itL->second.x, nullptr);
                    n.setProperty("y", itL->second.y, nullptr);
                }
                else if (auto itP = pendingNodePositions.find(nid);
                         itP != pendingNodePositions.end())
                {
                    n.setProperty("x", itP->second.x, nullptr);
                    n.setProperty("y", itP->second.y, nullptr);
                }
            }
        }
        Snapshot s;
        s.uiState = applied;
        if (synth != nullptr)
            synth->getStateInformation(s.synthState);
        undoStack.push_back(std::move(s));
        redoStack.clear();
        isPatchDirty = true; // Mark patch as dirty
        return;
    }
    Snapshot s;
    s.uiState = getUiValueTree();
    if (synth != nullptr)
        synth->getStateInformation(s.synthState);
    undoStack.push_back(std::move(s));
    redoStack.clear();

    // Mark patch as dirty whenever a change is made
    isPatchDirty = true;
}

void ImGuiNodeEditorComponent::restoreSnapshot(const Snapshot& s)
{
    if (synth != nullptr && s.synthState.getSize() > 0)
        synth->setStateInformation(s.synthState.getData(), (int)s.synthState.getSize());
    // Restore UI positions exactly as saved
    applyUiValueTreeNow(s.uiState);
}

juce::String ImGuiNodeEditorComponent::getTypeForLogical(juce::uint32 logicalId) const
{
    if (synth == nullptr)
        return {};
    for (const auto& p : synth->getModulesInfo())
        if (p.first == logicalId)
            return p.second;
    return {};
}

// Parameters are now drawn inline within each node; side panel removed

juce::ValueTree ImGuiNodeEditorComponent::getUiValueTree() const
{
    juce::ValueTree ui("NodeEditorUI");
    if (synth == nullptr)
        return ui;
    // Save node positions
    for (const auto& mod : synth->getModulesInfo())
    {
        const int nid = (int)mod.first;

        // Prefer cached position if available; never query ImNodes while rebuilding
        ImVec2 pos;
        if (lastKnownNodePositions.count(nid) > 0)
        {
            pos = lastKnownNodePositions.at(nid);
        }
        else if (auto it = pendingNodePositions.find(nid); it != pendingNodePositions.end())
        {
            pos = it->second;
        }
        else if (!graphNeedsRebuild.load() && hasRenderedAtLeastOnce)
        {
            pos = ImNodes::GetNodeGridSpacePos(nid);
        }
        else
        {
            pos = ImVec2(0.0f, 0.0f);
        }

        juce::ValueTree n("node");
        n.setProperty("id", nid, nullptr);
        n.setProperty("x", pos.x, nullptr);
        n.setProperty("y", pos.y, nullptr);

        // --- FIX: Save muted/bypassed state ---
        // If this node's ID is in our map of muted nodes, add the property to the XML
        if (mutedNodeStates.count(nid) > 0)
        {
            n.setProperty("muted", true, nullptr);
        }

        ui.addChild(n, -1, nullptr);
    }

    // --- FIX: Explicitly save the output node position (ID 0) ---
    // The main output node is not part of getModulesInfo(), so we need to save it separately

    // Prefer cached output position; avoid ImNodes when rebuilding
    ImVec2 outputPos;
    if (lastKnownNodePositions.count(0) > 0)
    {
        outputPos = lastKnownNodePositions.at(0);
    }
    else if (auto it0 = pendingNodePositions.find(0); it0 != pendingNodePositions.end())
    {
        outputPos = it0->second;
    }
    else if (!graphNeedsRebuild.load() && hasRenderedAtLeastOnce)
    {
        outputPos = ImNodes::GetNodeGridSpacePos(0);
    }
    else
    {
        outputPos = ImVec2(0.0f, 0.0f);
    }

    juce::ValueTree outputNode("node");
    outputNode.setProperty("id", 0, nullptr);
    outputNode.setProperty("x", outputPos.x, nullptr);
    outputNode.setProperty("y", outputPos.y, nullptr);
    ui.addChild(outputNode, -1, nullptr);
    // --- END OF FIX ---

    return ui;
}
void ImGuiNodeEditorComponent::applyUiValueTreeNow(const juce::ValueTree& uiState)
{
    if (!uiState.isValid() || synth == nullptr)
        return;

    juce::Logger::writeToLog("[UI_RESTORE] Applying UI ValueTree now...");

    // This is the core of the crash: the synth graph has already been rebuilt by
    // setStateInformation. We must clear our stale UI data (like muted nodes) before applying the
    // new state from the preset.
    mutedNodeStates.clear();

    auto nodes = uiState; // expect tag NodeEditorUI
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        auto n = nodes.getChild(i);

        if (!n.hasType("node"))
            continue;
        const int nid = (int)n.getProperty("id", 0);

        // ========================= THE FIX STARTS HERE =========================
        //
        // Before applying any property, VERIFY that this node ID actually exists
        // in the synth. This prevents crashes when loading presets that contain
        // modules which are not available in the current build.
        //
        bool nodeExistsInSynth = (nid == 0); // Node 0 is always the output node.
        if (!nodeExistsInSynth)
        {
            for (const auto& modInfo : synth->getModulesInfo())
            {
                if ((int)modInfo.first == nid)
                {
                    nodeExistsInSynth = true;
                    break;
                }
            }
        }

        if (!nodeExistsInSynth)
        {
            juce::Logger::writeToLog(
                "[UI_RESTORE] WARNING: Skipping UI properties for non-existent node ID " +
                juce::String(nid) + ". The module may be missing or failed to load.");
            continue; // Skip to the next node in the preset.
        }
        // ========================== END OF FIX ==========================

        const float x = (float)n.getProperty("x", 0.0f);
        const float y = (float)n.getProperty("y", 0.0f);
        if (!(x == 0.0f && y == 0.0f))
        {
            pendingNodePositions[nid] = ImVec2(x, y);
            juce::Logger::writeToLog(
                "[UI_RESTORE] Queued position for node " + juce::String(nid) + ": (" +
                juce::String(x) + ", " + juce::String(y) + ")");
        }

        // Read and apply muted state from preset for existing nodes.
        if ((bool)n.getProperty("muted", false))
        {
            // Use muteNodeSilent to store the original connections first,
            // then apply the mute (which creates bypass connections)
            muteNodeSilent(nid);
            muteNode(nid);
        }
    }

    // Set default positions for output node and BPM monitor node if they weren't loaded from preset
    // Output node (ID 0): right of center
    auto outputIt = pendingNodePositions.find(0);
    if (outputIt == pendingNodePositions.end())
    {
        // Position output node on right side, far enough to avoid splash screen overlap
        pendingNodePositions[0] = ImVec2(1250.0f, 500.0f);
        juce::Logger::writeToLog(
            "[UI_RESTORE] Set default position for output node: (2000.0, 500.0)");
    }

#ifndef AUDIO_ONLY_BUILD
    // Auto-create VideoViewer module if canvas is empty (only output node exists)
    auto modules = synth->getModulesInfo();
    if (modules.empty())
    {
        // Canvas is empty - create VideoViewer module by default
        auto videoViewerNodeId = synth->addModule("video_viewer", false);
        if (videoViewerNodeId.uid != 0)
        {
            auto videoViewerLogicalId = synth->getLogicalIdForNode(videoViewerNodeId);
            // Position VideoViewer directly below the output node (same X, small Y offset)
            // Output node is at (1250.0, 500.0), place VideoViewer at (1250.0, 650.0) - 150px below
            pendingNodePositions[(int)videoViewerLogicalId] = ImVec2(1250.0f, 650.0f);
            synth->commitChanges();
            juce::Logger::writeToLog(
                "[UI_RESTORE] Auto-created VideoViewer module (logicalId=" +
                juce::String((int)videoViewerLogicalId) + ") at default position (1250.0, 650.0)");
        }
    }
#endif

    // Muting/unmuting modifies graph connections, so we must tell the
    // synth to rebuild its processing order.
    graphNeedsRebuild = true;
    juce::Logger::writeToLog("[UI_RESTORE] UI state applied. Flagging for graph rebuild.");
}

void ImGuiNodeEditorComponent::applyUiValueTree(const juce::ValueTree& uiState)
{
    // Queue for next frame to avoid calling imnodes setters before editor is begun
    uiPending = uiState;
}

void ImGuiNodeEditorComponent::handleDeletion()
{
    if (synth == nullptr)
        return;

    const bool bypassRequested = consumeShortcutFlag(shortcutBypassDeleteRequested);
    const bool deleteRequested = consumeShortcutFlag(shortcutDeleteRequested);

    if (!bypassRequested && !deleteRequested)
        return;

    if (bypassRequested)
    {
        bypassDeleteSelectedNodes();
        return;
    }

    // If a drag was in progress, capture positions before we mutate the graph
    if (isDraggingNode || ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        isDraggingNode = false;
        pushSnapshot();
    }

    // Early out if nothing selected
    const int numSelLinks = ImNodes::NumSelectedLinks();
    const int numSelNodes = ImNodes::NumSelectedNodes();

    if (numSelLinks <= 0 && numSelNodes <= 0)
        return;

    // Perform batch delete; snapshot after commit

    // Disconnect selected links
    if (numSelLinks > 0)
    {
        std::vector<int> ids((size_t)numSelLinks);
        ImNodes::GetSelectedLinks(ids.data());
        for (int id : ids)
        {
            // TODO: Handle modulation link deletion for new bus-based system
            // if (auto itM = modLinkIdToRoute.find (id); itM != modLinkIdToRoute.end())
            // {
            //     int sL, sC, dL; juce::String paramId; std::tie(sL, sC, dL, paramId) =
            //     itM->second;
            //     // TODO: Handle modulation route removal
            //     // if (paramId.isNotEmpty())
            //     //     synth->removeModulationRoute (synth->getNodeIdForLogical ((juce::uint32)
            //     sL), sC, (juce::uint32) dL, paramId);
            //     // else
            //     //     synth->removeModulationRoute (synth->getNodeIdForLogical ((juce::uint32)
            //     sL), sC, (juce::uint32) dL);
            // }
            // else
            if (auto it = linkIdToAttrs.find(id); it != linkIdToAttrs.end())
            {
                auto srcPin = decodePinId(it->second.first);
                auto dstPin = decodePinId(it->second.second);

                auto srcNode = synth->getNodeIdForLogical(srcPin.logicalId);
                auto dstNode = (dstPin.logicalId == 0)
                                   ? synth->getOutputNodeID()
                                   : synth->getNodeIdForLogical(dstPin.logicalId);
                synth->disconnect(srcNode, srcPin.channel, dstNode, dstPin.channel);
            }
        }
    }

    if (numSelNodes > 0)
    {
        std::vector<int> nodeIds((size_t)numSelNodes);
        ImNodes::GetSelectedNodes(nodeIds.data());
        // Build a set for quick lookup when removing connections
        std::unordered_map<int, bool> toDelete;
        for (int nid : nodeIds)
            toDelete[nid] = true;
        // Disconnect all connections touching any selected node
        for (const auto& c : synth->getConnectionsInfo())
        {
            if (toDelete.count((int)c.srcLogicalId) ||
                (!c.dstIsOutput && toDelete.count((int)c.dstLogicalId)))
            {
                auto srcNode = synth->getNodeIdForLogical(c.srcLogicalId);
                auto dstNode = c.dstIsOutput ? synth->getOutputNodeID()
                                             : synth->getNodeIdForLogical(c.dstLogicalId);
                synth->disconnect(srcNode, c.srcChan, dstNode, c.dstChan);
            }
        }
        // Remove nodes
        for (int nid : nodeIds)
        {
            if (nid == 0)
                continue; // don't delete output sink

            // Clean up vision module textures if exists
            if (visionModuleTextures.count(nid))
            {
                visionModuleTextures.erase(nid);
            }

            // Clean up sample loader textures if exists
            if (sampleLoaderTextureIds.count(nid))
            {
                sampleLoaderTextureIds.erase(nid);
            }

            mutedNodeStates.erase((juce::uint32)nid); // Clean up muted state if exists
            lastKnownNodePositions.erase(nid);        // Clean up position cache
            synth->removeModule(synth->getNodeIdForLogical((juce::uint32)nid));
        }
    }
    graphNeedsRebuild = true;
    pushSnapshot();
    NotificationManager::post(
        NotificationManager::Type::Info, "Deleted " + juce::String(numSelNodes) + " node(s)");
}
void ImGuiNodeEditorComponent::bypassDeleteSelectedNodes()
{
    const int numSelNodes = ImNodes::NumSelectedNodes();
    if (numSelNodes <= 0 || synth == nullptr)
        return;

    if (isDraggingNode || ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        isDraggingNode = false;
        pushSnapshot();
    }

    std::vector<int> nodeIds((size_t)numSelNodes);
    ImNodes::GetSelectedNodes(nodeIds.data());

    for (int nid : nodeIds)
    {
        if (nid == 0)
            continue;

        bypassDeleteNode((juce::uint32)nid);
    }

    graphNeedsRebuild = true;
    pushSnapshot();
    NotificationManager::post(
        NotificationManager::Type::Info, "Deleted " + juce::String(numSelNodes) + " node(s)");
}

void ImGuiNodeEditorComponent::bypassDeleteNode(juce::uint32 logicalId)
{
    // Collect all incoming/outgoing audio links for this node
    std::vector<decltype(synth->getConnectionsInfo())::value_type> inputs, outputs;
    for (const auto& c : synth->getConnectionsInfo())
    {
        if (!c.dstIsOutput && c.dstLogicalId == logicalId)
            inputs.push_back(c);
        if (c.srcLogicalId == logicalId)
            outputs.push_back(c);
    }

    // For each output channel, find matching input channel to splice
    for (const auto& out : outputs)
    {
        // Try to find input with same channel index, else fallback to first input
        const auto* inPtr = (const decltype(inputs)::value_type*)nullptr;
        for (const auto& in : inputs)
        {
            if (in.dstChan == out.srcChan)
            {
                inPtr = &in;
                break;
            }
        }
        if (inPtr == nullptr && !inputs.empty())
            inPtr = &inputs.front();

        // Disconnect out link first
        auto srcNode = synth->getNodeIdForLogical(out.srcLogicalId);
        auto dstNode = out.dstIsOutput ? synth->getOutputNodeID()
                                       : synth->getNodeIdForLogical(out.dstLogicalId);
        synth->disconnect(srcNode, out.srcChan, dstNode, out.dstChan);

        if (inPtr != nullptr)
        {
            // Disconnect incoming link from the node
            auto inSrcNode = synth->getNodeIdForLogical(inPtr->srcLogicalId);
            auto inDstNode = synth->getNodeIdForLogical(inPtr->dstLogicalId);
            synth->disconnect(inSrcNode, inPtr->srcChan, inDstNode, inPtr->dstChan);

            // Connect source of incoming directly to destination of outgoing
            auto finalDstNode = out.dstIsOutput ? synth->getOutputNodeID()
                                                : synth->getNodeIdForLogical(out.dstLogicalId);
            synth->connect(inSrcNode, inPtr->srcChan, finalDstNode, out.dstChan);
        }
    }

    // TODO: Remove modulation routes targeting or originating this node using new bus-based system

    // Finally remove the node itself
    mutedNodeStates.erase(logicalId); // Clean up muted state if exists
    synth->removeModule(synth->getNodeIdForLogical(logicalId));
}

// === Non-Destructive Mute/Bypass Implementation ===

void ImGuiNodeEditorComponent::muteNodeSilent(juce::uint32 logicalId)
{
    // This function is used when loading presets. It records the connections that were
    // loaded from the XML without modifying the graph or creating bypass connections.
    // This preserves the original "unmuted" connections for later use.

    if (!synth)
        return;

    MutedNodeState state;
    auto           allConnections = synth->getConnectionsInfo();

    // Store all connections attached to this node
    for (const auto& c : allConnections)
    {
        if (!c.dstIsOutput && c.dstLogicalId == logicalId)
        {
            state.incomingConnections.push_back(c);
        }
        if (c.srcLogicalId == logicalId)
        {
            state.outgoingConnections.push_back(c);
        }
    }

    // Store the state, but DON'T modify the graph or create bypass connections
    mutedNodeStates[logicalId] = state;
    juce::Logger::writeToLog(
        "[MuteSilent] Node " + juce::String(logicalId) + " marked as muted, stored " +
        juce::String(state.incomingConnections.size()) + " incoming and " +
        juce::String(state.outgoingConnections.size()) + " outgoing connections.");
}
void ImGuiNodeEditorComponent::muteNode(juce::uint32 logicalId)
{
    if (!synth)
        return;

    MutedNodeState state;
    auto           allConnections = synth->getConnectionsInfo();

    // 1. Find and store all connections attached to this node.
    for (const auto& c : allConnections)
    {
        if (!c.dstIsOutput && c.dstLogicalId == logicalId)
        {
            state.incomingConnections.push_back(c);
        }
        if (c.srcLogicalId == logicalId)
        {
            state.outgoingConnections.push_back(c);
        }
    }

    // 2. Disconnect all of them.
    for (const auto& c : state.incomingConnections)
    {
        synth->disconnect(
            synth->getNodeIdForLogical(c.srcLogicalId),
            c.srcChan,
            synth->getNodeIdForLogical(c.dstLogicalId),
            c.dstChan);
    }
    for (const auto& c : state.outgoingConnections)
    {
        auto dstNodeId =
            c.dstIsOutput ? synth->getOutputNodeID() : synth->getNodeIdForLogical(c.dstLogicalId);
        synth->disconnect(
            synth->getNodeIdForLogical(c.srcLogicalId), c.srcChan, dstNodeId, c.dstChan);
    }

    // --- FIX: More robust bypass splicing logic ---
    // 3. Splice the connections to bypass the node.
    // Connect the FIRST input source to ALL output destinations.
    // This correctly handles cases where input channel != output channel (e.g., Mixer input 3 →
    // output 0).
    if (!state.incomingConnections.empty() && !state.outgoingConnections.empty())
    {
        const auto& primary_input = state.incomingConnections[0];
        auto        srcNodeId = synth->getNodeIdForLogical(primary_input.srcLogicalId);

        for (const auto& out_conn : state.outgoingConnections)
        {
            auto dstNodeId = out_conn.dstIsOutput
                                 ? synth->getOutputNodeID()
                                 : synth->getNodeIdForLogical(out_conn.dstLogicalId);
            // Connect the primary input's source directly to the original output's destination
            synth->connect(srcNodeId, primary_input.srcChan, dstNodeId, out_conn.dstChan);
            juce::Logger::writeToLog(
                "[Mute] Splicing bypass: [" + juce::String(primary_input.srcLogicalId) + ":" +
                juce::String(primary_input.srcChan) + "] -> [" +
                (out_conn.dstIsOutput ? "Output" : juce::String(out_conn.dstLogicalId)) + ":" +
                juce::String(out_conn.dstChan) + "]");
        }
    }

    // 4. Store the original state.
    mutedNodeStates[logicalId] = state;
    juce::Logger::writeToLog("[Mute] Node " + juce::String(logicalId) + " muted and bypassed.");
}

void ImGuiNodeEditorComponent::unmuteNode(juce::uint32 logicalId)
{
    if (!synth || mutedNodeStates.find(logicalId) == mutedNodeStates.end())
        return;

    MutedNodeState state = mutedNodeStates[logicalId];

    // --- FIX: Remove bypass connections matching the new mute logic ---
    // 1. Find and remove the bypass connections.
    // The bypass connected the first input source to all output destinations.
    if (!state.incomingConnections.empty() && !state.outgoingConnections.empty())
    {
        const auto& primary_input = state.incomingConnections[0];
        auto        srcNodeId = synth->getNodeIdForLogical(primary_input.srcLogicalId);

        for (const auto& out_conn : state.outgoingConnections)
        {
            auto dstNodeId = out_conn.dstIsOutput
                                 ? synth->getOutputNodeID()
                                 : synth->getNodeIdForLogical(out_conn.dstLogicalId);
            // Disconnect the bypass connection
            synth->disconnect(srcNodeId, primary_input.srcChan, dstNodeId, out_conn.dstChan);
            juce::Logger::writeToLog(
                "[Unmute] Removing bypass: [" + juce::String(primary_input.srcLogicalId) + ":" +
                juce::String(primary_input.srcChan) + "] -> [" +
                (out_conn.dstIsOutput ? "Output" : juce::String(out_conn.dstLogicalId)) + ":" +
                juce::String(out_conn.dstChan) + "]");
        }
    }

    // 2. Restore the original connections.
    for (const auto& c : state.incomingConnections)
    {
        synth->connect(
            synth->getNodeIdForLogical(c.srcLogicalId),
            c.srcChan,
            synth->getNodeIdForLogical(c.dstLogicalId),
            c.dstChan);
    }
    for (const auto& c : state.outgoingConnections)
    {
        auto dstNodeId =
            c.dstIsOutput ? synth->getOutputNodeID() : synth->getNodeIdForLogical(c.dstLogicalId);
        synth->connect(synth->getNodeIdForLogical(c.srcLogicalId), c.srcChan, dstNodeId, c.dstChan);
    }

    // 3. Remove from muted state.
    mutedNodeStates.erase(logicalId);
    juce::Logger::writeToLog("[Mute] Node " + juce::String(logicalId) + " unmuted.");
}
void ImGuiNodeEditorComponent::handleMuteToggle()
{
    const int numSelected = ImNodes::NumSelectedNodes();
    if (numSelected == 0)
        return;

    pushSnapshot(); // Create a single undo state for the whole operation.

    std::vector<int> selectedNodeIds(numSelected);
    ImNodes::GetSelectedNodes(selectedNodeIds.data());

    for (int lid : selectedNodeIds)
    {
        if (mutedNodeStates.count(lid))
        {
            unmuteNode(lid);
        }
        else
        {
            muteNode(lid);
        }
    }

    graphNeedsRebuild = true;
}
void ImGuiNodeEditorComponent::savePresetToFile(const juce::File& file)
{
    bool wasAlreadyInProgress = isSaveInProgress.exchange(true); // Atomically check and set
    if (wasAlreadyInProgress)
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow] Save action ignored (already in progress). Current flag state: " +
            juce::String(isSaveInProgress.load() ? "TRUE" : "FALSE"));
        return;
    }

    juce::Logger::writeToLog(
        "[SaveWorkflow] Flag set to TRUE. Starting save workflow for: " + file.getFullPathName());

    if (synth == nullptr)
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow] ERROR: Synth is null! Resetting flag and aborting.");
        NotificationManager::post(NotificationManager::Type::Error, "ERROR: Synth not ready!");
        isSaveInProgress.store(false);
        juce::Logger::writeToLog("[SaveWorkflow] Flag reset to FALSE after synth null check.");
        return;
    }

    juce::Logger::writeToLog(
        "--- [Save Workflow] Initiated for: " + file.getFullPathName() + " ---");

    // Post status notification (long duration since it will be replaced by Success/Error when
    // complete)
    NotificationManager::post(
        NotificationManager::Type::Status,
        "Saving: " + file.getFileNameWithoutExtension(),
        1000.0f);

    // --- All fast operations now happen on the UI thread BEFORE the job is launched ---

    juce::Logger::writeToLog("[SaveWorkflow][UI_THREAD] Capturing state...");
    auto mutedNodeIDs = getMutedNodeIds();
    juce::Logger::writeToLog(
        "[SaveWorkflow][UI_THREAD] Found " + juce::String((int)mutedNodeIDs.size()) +
        " muted nodes.");

    // Temporarily unmute to get correct connections
    juce::Logger::writeToLog(
        "[SaveWorkflow][UI_THREAD] Temporarily unmuting nodes for state capture...");
    for (auto lid : mutedNodeIDs)
        unmuteNode(lid);
    synth->commitChanges();

    // Capture state while unmuted
    juce::Logger::writeToLog("[SaveWorkflow][UI_THREAD] Calling synth->getStateInformation()...");
    juce::MemoryBlock synthState;
    try
    {
        synth->getStateInformation(synthState);
        juce::Logger::writeToLog(
            "[SaveWorkflow][UI_THREAD] Synth state captured (" +
            juce::String(synthState.getSize()) + " bytes).");
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow][UI_THREAD] EXCEPTION in getStateInformation: " +
            juce::String(e.what()));
        isSaveInProgress.store(false);
        juce::Logger::writeToLog("[SaveWorkflow] Flag reset to FALSE after exception.");
        NotificationManager::post(
            NotificationManager::Type::Error, "Failed to capture synth state!");
        return;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow][UI_THREAD] UNKNOWN EXCEPTION in getStateInformation");
        isSaveInProgress.store(false);
        juce::Logger::writeToLog("[SaveWorkflow] Flag reset to FALSE after unknown exception.");
        NotificationManager::post(
            NotificationManager::Type::Error, "Failed to capture synth state!");
        return;
    }

    juce::Logger::writeToLog("[SaveWorkflow][UI_THREAD] Calling editor->getUiValueTree()...");
    juce::ValueTree uiState = getUiValueTree();
    juce::Logger::writeToLog(
        "[SaveWorkflow][UI_THREAD] UI state captured (valid: " +
        juce::String(uiState.isValid() ? 1 : 0) + ").");

    // Immediately re-mute to restore visual state
    juce::Logger::writeToLog(
        "[SaveWorkflow][UI_THREAD] Re-muting nodes to restore visual state...");
    for (auto lid : mutedNodeIDs)
        muteNode(lid);
    synth->commitChanges();
    juce::Logger::writeToLog(
        "[SaveWorkflow][UI_THREAD] State captured. Offloading to background thread.");

    // Launch the background job with the captured data
    juce::Logger::writeToLog("[SaveWorkflow][UI_THREAD] Creating SavePresetJob...");
    auto* job = new SavePresetJob(synthState, uiState, file);

    job->onSaveComplete = [this, filePath = file.getFullPathName()](
                              const juce::File& savedFile, bool success) {
        juce::Logger::writeToLog(
            "[SaveWorkflow] onSaveComplete callback called (success: " +
            juce::String(success ? 1 : 0) + ") for: " + savedFile.getFullPathName());

        if (success)
        {
            NotificationManager::post(
                NotificationManager::Type::Success,
                "Saved: " + savedFile.getFileNameWithoutExtension());
            isPatchDirty = false;
            currentPresetFile = savedFile;
            juce::Logger::writeToLog(
                "[SaveWorkflow] Save completed successfully. Flag will be reset.");
        }
        else
        {
            juce::Logger::writeToLog("[SaveWorkflow] Save FAILED. Flag will be reset.");
            NotificationManager::post(NotificationManager::Type::Error, "Failed to save preset!");
        }

        juce::Logger::writeToLog("[SaveWorkflow] Resetting isSaveInProgress flag to FALSE.");
        isSaveInProgress.store(false);
        juce::Logger::writeToLog(
            "[SaveWorkflow] Flag reset complete. Current state: " +
            juce::String(isSaveInProgress.load() ? "TRUE" : "FALSE"));
    };

    juce::Logger::writeToLog("[SaveWorkflow][UI_THREAD] Adding job to thread pool...");
    try
    {
        threadPool.addJob(job, true);
        juce::Logger::writeToLog(
            "[SaveWorkflow][UI_THREAD] Job added to thread pool successfully.");
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow][UI_THREAD] EXCEPTION adding job to thread pool: " +
            juce::String(e.what()));
        isSaveInProgress.store(false);
        juce::Logger::writeToLog("[SaveWorkflow] Flag reset to FALSE after thread pool exception.");
        NotificationManager::post(NotificationManager::Type::Error, "Failed to start save job!");
        delete job; // Clean up the job we created
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow][UI_THREAD] UNKNOWN EXCEPTION adding job to thread pool");
        isSaveInProgress.store(false);
        juce::Logger::writeToLog(
            "[SaveWorkflow] Flag reset to FALSE after unknown thread pool exception.");
        NotificationManager::post(NotificationManager::Type::Error, "Failed to start save job!");
        delete job; // Clean up the job we created
    }
}
void ImGuiNodeEditorComponent::startSaveDialog()
{
    juce::Logger::writeToLog(
        "[SaveWorkflow] startSaveDialog() called. isSaveInProgress: " +
        juce::String(isSaveInProgress.load() ? "TRUE" : "FALSE"));

    // Check if a save is already in progress to avoid opening multiple dialogs
    if (isSaveInProgress.load())
    {
        juce::Logger::writeToLog(
            "[SaveWorkflow] 'Save As' action ignored (a save is already in progress).");
        NotificationManager::post(
            NotificationManager::Type::Warning,
            "A save operation is already in progress. Please wait...",
            3.0f);
        return;
    }

    juce::Logger::writeToLog("[SaveWorkflow] Opening file chooser dialog...");
    auto presetsDir = findPresetsDirectory();
    juce::Logger::writeToLog("[SaveWorkflow] Presets directory: " + presetsDir.getFullPathName());

    saveChooser = std::make_unique<juce::FileChooser>("Save Preset As...", presetsDir, "*.xml");
    saveChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            juce::Logger::writeToLog("[SaveWorkflow] File chooser callback invoked.");
            auto fileToSave = fc.getResult();

            if (fileToSave != juce::File{}) // Check if user selected a file and didn't cancel
            {
                juce::Logger::writeToLog(
                    "[SaveWorkflow] User selected file: " + fileToSave.getFullPathName());
                // Call the unified, asynchronous save function
                savePresetToFile(fileToSave);
            }
            else
            {
                juce::Logger::writeToLog("[SaveWorkflow] User cancelled file chooser dialog.");
            }
        });
    juce::Logger::writeToLog("[SaveWorkflow] File chooser launched (async).");
}

std::vector<juce::uint32> ImGuiNodeEditorComponent::getMutedNodeIds() const
{
    std::vector<juce::uint32> ids;
    // MutedNodeState is a map, so we don't need a lock if we're just reading keys
    for (const auto& pair : mutedNodeStates)
    {
        ids.push_back(pair.first);
    }
    return ids;
}

void ImGuiNodeEditorComponent::startLoadDialog()
{
    NotificationManager::post(
        NotificationManager::Type::Info, "Opening Load Preset dialog...", 3.0f);
    loadChooser =
        std::make_unique<juce::FileChooser>("Load preset", findPresetsDirectory(), "*.xml");
    loadChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (!file.existsAsFile())
                return;
            NotificationManager::post(
                NotificationManager::Type::Info, "Loading: " + file.getFileName(), 5.0f);

            auto xml = juce::XmlDocument::parse(file);
            if (!xml)
            {
                NotificationManager::post(
                    NotificationManager::Type::Error,
                    "Preset failed to load: Not a valid XML file.");
                return;
            }
            juce::ValueTree presetVT = juce::ValueTree::fromXml(*xml);

            // === STAGE 1 & 2: HEAL THE PRESET (RULE-BASED) ===
            PresetAutoHealer healer;
            auto             healingMessages = healer.heal(presetVT);

            // === STAGE 3: VALIDATE THE (NOW HEALED) PRESET ===
            PresetValidator validator;
            auto            issues = validator.validate(presetVT);
            int             errorCount = 0;
            int             warningCount = 0;
            for (const auto& issue : issues)
            {
                if (issue.severity == PresetValidator::Issue::Error)
                    errorCount++;
                else
                    warningCount++;
            }

            // Report errors/warnings but proceed to load to match built-in loader behavior
            if (errorCount > 0)
            {
                juce::String summary = "Validation found " + juce::String(errorCount) +
                                       " error(s). Attempting load anyway.";
                if (!healingMessages.empty())
                    summary += " (" + juce::String((int)healingMessages.size()) +
                               " issue(s) auto-healed).";
                NotificationManager::post(NotificationManager::Type::Warning, summary, 12.0f);
                for (const auto& issue : issues)
                {
                    if (issue.severity == PresetValidator::Issue::Error)
                        NotificationManager::post(
                            NotificationManager::Type::Warning, issue.message, 12.0f);
                }
            }

            // === STAGE 4: LOAD THE HEALED DATA ===
            juce::MemoryBlock        mb;
            juce::MemoryOutputStream mos(mb, false);
            if (auto healedXml = presetVT.createXml())
                healedXml->writeTo(mos);
            synth->setStateInformation(mb.getData(), (int)mb.getSize());
            auto uiState = presetVT.getChildWithName("NodeEditorUI");
            if (uiState.isValid())
                applyUiValueTree(uiState);
            isPatchDirty = false;
            currentPresetFile = file;
            pushSnapshot();

            // === STAGE 5: NOTIFY ===
            if (!healingMessages.empty() || warningCount > 0 || errorCount > 0)
            {
                juce::String summary =
                    "Loaded with " + juce::String(warningCount + errorCount) + " issue(s).";
                NotificationManager::post(NotificationManager::Type::Warning, summary, 8.0f);
                for (const auto& msg : healingMessages)
                    NotificationManager::post(NotificationManager::Type::Info, msg, 8.0f);
                for (const auto& issue : issues)
                    NotificationManager::post(
                        issue.severity == PresetValidator::Issue::Warning
                            ? NotificationManager::Type::Warning
                            : NotificationManager::Type::Warning,
                        issue.message,
                        8.0f);
            }
            else
            {
                NotificationManager::post(
                    NotificationManager::Type::Success,
                    "Loaded: " + file.getFileNameWithoutExtension());
            }
        });
}

void ImGuiNodeEditorComponent::newCanvas()
{
    if (synth == nullptr)
        return;

    // Clear the synth state (removes all modules and connections)
    synth->clearAll();

    // Set output node position first
    pendingNodePositions[0] = ImVec2(1250.0f, 500.0f);

#ifndef AUDIO_ONLY_BUILD
    // Auto-create VideoViewer module on new canvas (like default audio output)
    auto videoViewerNodeId = synth->addModule("video_viewer", false);
    if (videoViewerNodeId.uid != 0)
    {
        auto videoViewerLogicalId = synth->getLogicalIdForNode(videoViewerNodeId);
        // Position VideoViewer directly below the output node (same X, small Y offset)
        // Output node is at (1250.0, 500.0), place VideoViewer at (1250.0, 650.0) - 150px below
        pendingNodePositions[(int)videoViewerLogicalId] = ImVec2(1250.0f, 650.0f);
        synth->commitChanges();
        juce::Logger::writeToLog(
            "[NewCanvas] Auto-created VideoViewer module (logicalId=" +
            juce::String((int)videoViewerLogicalId) + ") at default position (1250.0, 650.0)");
    }
#endif

    // Clear undo/redo stacks
    undoStack.clear();
    redoStack.clear();

    // Clear the current preset file reference
    currentPresetFile = juce::File();

    // Reset patch dirty flag
    isPatchDirty = false;

    // Push a snapshot of the empty state for undo/redo
    pushSnapshot();

    // Notify the user
    NotificationManager::post(
        NotificationManager::Type::Info, "New canvas created - ready to start fresh");

    juce::Logger::writeToLog("[NewCanvas] Cleared synth state and started fresh canvas");
}

void ImGuiNodeEditorComponent::handleRandomizePatch()
{
    if (synth == nullptr)
        return;

    populatePinDatabase();

    // 1. --- SETUP ---
    synth->clearAll();
    juce::Random rng(juce::Time::getMillisecondCounterHiRes());

    // 2. --- ADD A "CLOUD" OF RANDOM MODULES ---
    std::vector<juce::String> modulePool = {
        "vco",
        "noise",
        "sequencer",
        "vcf",
        "delay",
        "reverb",
        "waveshaper",
        "lfo",
        "adsr",
        "random",
        "s_and_h",
        "math",
        "map_range",
        "quantizer",
        "clock_divider"};
    int numModules = 6 + rng.nextInt(7); // 6 to 12 modules
    std::vector<std::pair<juce::uint32, juce::String>> addedModules;

    for (int i = 0; i < numModules; ++i)
    {
        auto type = modulePool[rng.nextInt(modulePool.size())];
        auto newId = synth->getLogicalIdForNode(synth->addModule(type));
        addedModules.push_back({newId, type});
    }

    // 3. --- ESTABLISH AN OBSERVATION POINT ---
    // Always add a Mixer and Scope. This is our window into the chaos.
    auto mixerId = synth->getLogicalIdForNode(synth->addModule("mixer"));
    addedModules.push_back({mixerId, "mixer"});
    auto scopeId = synth->getLogicalIdForNode(synth->addModule("scope"));
    addedModules.push_back({scopeId, "scope"});

    // Connect the observation path: Mixer -> Scope -> Output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(synth->getNodeIdForLogical(mixerId), 0, synth->getNodeIdForLogical(scopeId), 0);
    synth->connect(synth->getNodeIdForLogical(scopeId), 0, outputNodeId, 0);
    synth->connect(synth->getNodeIdForLogical(scopeId), 1, outputNodeId, 1);

    // 4. --- CREATE CHAOTIC CONNECTIONS ---
    std::vector<std::pair<juce::uint32, AudioPin>> allAudioOuts;
    std::vector<std::pair<juce::uint32, AudioPin>> allAudioIns;
    std::vector<std::pair<juce::uint32, ModPin>>   allModIns;

    for (const auto& mod : addedModules)
    {
        auto it = getModulePinDatabase().find(mod.second);
        if (it != getModulePinDatabase().end())
        {
            for (const auto& pin : it->second.audioOuts)
                allAudioOuts.push_back({mod.first, pin});
            for (const auto& pin : it->second.audioIns)
                allAudioIns.push_back({mod.first, pin});
            for (const auto& pin : it->second.modIns)
                allModIns.push_back({mod.first, pin});
        }
    }

    // Connect a few random audio sources to the Mixer to make sound likely
    int numMixerInputs = 2 + rng.nextInt(3); // 2 to 4 mixer inputs
    if (!allAudioOuts.empty())
    {
        for (int i = 0; i < numMixerInputs; ++i)
        {
            auto& source = allAudioOuts[rng.nextInt(allAudioOuts.size())];
            // Connect to mixer inputs 0, 1, 2, 3
            synth->connect(
                synth->getNodeIdForLogical(source.first),
                source.second.channel,
                synth->getNodeIdForLogical(mixerId),
                i);
        }
    }

    // Make a large number of fully random connections
    int numRandomConnections = numModules + rng.nextInt(numModules);
    for (int i = 0; i < numRandomConnections; ++i)
    {
        float choice = rng.nextFloat();
        // 70% chance of making a CV modulation connection
        if (choice < 0.7f && !allAudioOuts.empty() && !allModIns.empty())
        {
            auto& source = allAudioOuts[rng.nextInt(allAudioOuts.size())];
            auto& target = allModIns[rng.nextInt(allModIns.size())];
            // TODO: synth->addModulationRouteByLogical(source.first, source.second.channel,
            // target.first, target.second.paramId);
        }
        // 30% chance of making an audio-path or gate connection
        else if (!allAudioOuts.empty() && !allAudioIns.empty())
        {
            auto& source = allAudioOuts[rng.nextInt(allAudioOuts.size())];
            auto& target = allAudioIns[rng.nextInt(allAudioIns.size())];
            // Allow self-connection for feedback
            if (source.first != target.first || rng.nextFloat() < 0.2f)
            {
                synth->connect(
                    synth->getNodeIdForLogical(source.first),
                    source.second.channel,
                    synth->getNodeIdForLogical(target.first),
                    target.second.channel);
            }
        }
    }

    // 5. --- LAYOUT AND FINALIZE ---
    // Arrange nodes in a neat grid to prevent overlap.
    const float startX = 50.0f;
    const float startY = 50.0f;
    const float cellWidth = 300.0f;
    const float cellHeight = 400.0f;
    const int   numColumns = 4;
    int         col = 0;
    int         row = 0;

    juce::uint32 finalMixerId = 0, finalScopeId = 0;
    for (const auto& mod : addedModules)
    {
        if (mod.second == "mixer")
            finalMixerId = mod.first;
        if (mod.second == "scope")
            finalScopeId = mod.first;
    }

    for (const auto& mod : addedModules)
    {
        // Skip the special output-chain nodes; we will place them manually.
        if (mod.first == finalMixerId || mod.first == finalScopeId)
            continue;

        float x = startX + col * cellWidth;
        float y = startY + row * cellHeight;
        pendingNodePositions[(int)mod.first] = ImVec2(x, y);

        col++;
        if (col >= numColumns)
        {
            col = 0;
            row++;
        }
    }

    // Manually place the Mixer and Scope on the far right for a clean, readable signal flow.
    float finalX = startX + numColumns * cellWidth;
    if (finalMixerId != 0)
        pendingNodePositions[(int)finalMixerId] = ImVec2(finalX, startY);
    if (finalScopeId != 0)
        pendingNodePositions[(int)finalScopeId] = ImVec2(finalX, startY + cellHeight);

    synth->commitChanges();
    pushSnapshot();
}

void ImGuiNodeEditorComponent::handleRandomizeConnections()
{
    if (synth == nullptr)
        return;
    auto currentModules = synth->getModulesInfo();
    if (currentModules.empty())
        return;

    // 1. --- SETUP AND CLEAR ---
    synth->clearAllConnections();
    juce::Random rng(juce::Time::getMillisecondCounterHiRes());

    // 2. --- ESTABLISH AN OBSERVATION POINT ---
    juce::uint32 mixerId = 0, scopeId = 0;
    for (const auto& mod : currentModules)
    {
        if (mod.second == "mixer")
            mixerId = mod.first;
        if (mod.second == "scope")
            scopeId = mod.first;
    }
    // Add Mixer/Scope if they don't exist, as they are crucial for listening
    if (mixerId == 0)
        mixerId = synth->getLogicalIdForNode(synth->addModule("mixer"));
    if (scopeId == 0)
        scopeId = synth->getLogicalIdForNode(synth->addModule("scope"));

    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(synth->getNodeIdForLogical(mixerId), 0, synth->getNodeIdForLogical(scopeId), 0);
    synth->connect(synth->getNodeIdForLogical(scopeId), 0, outputNodeId, 0);

    // 3. --- CREATE CHAOTIC CONNECTIONS ---
    std::vector<std::pair<juce::uint32, AudioPin>> allAudioOuts;
    std::vector<std::pair<juce::uint32, AudioPin>> allAudioIns;
    std::vector<std::pair<juce::uint32, ModPin>>   allModIns;

    // Refresh module list in case we added a Mixer/Scope
    auto updatedModules = synth->getModulesInfo();
    for (const auto& mod : updatedModules)
    {
        auto it = getModulePinDatabase().find(mod.second);
        if (it != getModulePinDatabase().end())
        {
            for (const auto& pin : it->second.audioOuts)
                allAudioOuts.push_back({mod.first, pin});
            for (const auto& pin : it->second.audioIns)
                allAudioIns.push_back({mod.first, pin});
            for (const auto& pin : it->second.modIns)
                allModIns.push_back({mod.first, pin});
        }
    }

    // Connect random sources to the Mixer
    int numMixerInputs = 2 + rng.nextInt(3);
    if (!allAudioOuts.empty())
    {
        for (int i = 0; i < numMixerInputs; ++i)
        {
            auto& source = allAudioOuts[rng.nextInt(allAudioOuts.size())];
            if (source.first != mixerId) // Don't connect mixer to itself here
                synth->connect(
                    synth->getNodeIdForLogical(source.first),
                    source.second.channel,
                    synth->getNodeIdForLogical(mixerId),
                    i);
        }
    }

    // Make a large number of fully random connections
    int numRandomConnections = (int)updatedModules.size() + rng.nextInt((int)updatedModules.size());
    for (int i = 0; i < numRandomConnections; ++i)
    {
        float choice = rng.nextFloat();
        if (choice < 0.7f && !allAudioOuts.empty() && !allModIns.empty())
        {
            auto& source = allAudioOuts[rng.nextInt(allAudioOuts.size())];
            auto& target = allModIns[rng.nextInt(allModIns.size())];
            // TODO: synth->addModulationRouteByLogical(source.first, source.second.channel,
            // target.first, target.second.paramId);
        }
        else if (!allAudioOuts.empty() && !allAudioIns.empty())
        {
            auto& source = allAudioOuts[rng.nextInt(allAudioOuts.size())];
            auto& target = allAudioIns[rng.nextInt(allAudioIns.size())];
            if (source.first != target.first || rng.nextFloat() < 0.2f)
            { // Allow feedback
                synth->connect(
                    synth->getNodeIdForLogical(source.first),
                    source.second.channel,
                    synth->getNodeIdForLogical(target.first),
                    target.second.channel);
            }
        }
    }

    // 4. --- FINALIZE ---
    synth->commitChanges();
    pushSnapshot();
}
void ImGuiNodeEditorComponent::handleBeautifyLayout()
{
    if (synth == nullptr)
        return;

    // Graph is always in consistent state since we rebuild at frame start
    // Create an undo state so the action can be reversed
    pushSnapshot();
    juce::Logger::writeToLog("--- [Beautify Layout] Starting ---");

    // --- STEP 1: Build Graph Representation ---
    // Adjacency list: map<source_lid, vector<destination_lid>>
    std::map<juce::uint32, std::vector<juce::uint32>> adjacencyList;
    std::map<juce::uint32, int> inDegree; // Counts incoming connections for each node
    std::vector<juce::uint32>   sourceNodes;

    auto modules = synth->getModulesInfo();
    for (const auto& mod : modules)
    {
        inDegree[mod.first] = 0;
        adjacencyList[mod.first] = {};
    }
    // Include the output node in the graph
    inDegree[0] = 0;       // Output node ID is 0
    adjacencyList[0] = {}; // Output node has no outgoing connections

    for (const auto& conn : synth->getConnectionsInfo())
    {
        if (conn.dstIsOutput)
        {
            adjacencyList[conn.srcLogicalId].push_back(0); // Connect to output node
            inDegree[0]++;
        }
        else
        {
            adjacencyList[conn.srcLogicalId].push_back(conn.dstLogicalId);
            inDegree[conn.dstLogicalId]++;
        }
    }

    // Debug: Log all connections to identify cycles
    juce::Logger::writeToLog("[Beautify] Graph connections:");
    for (const auto& pair : adjacencyList)
    {
        if (!pair.second.empty())
        {
            juce::String connStr = "[Beautify] Node " + juce::String(pair.first) + " -> ";
            for (juce::uint32 dst : pair.second)
            {
                connStr += juce::String(dst) + " ";
            }
            juce::Logger::writeToLog(connStr);
        }
    }

    for (const auto& mod : modules)
    {
        if (inDegree[mod.first] == 0)
        {
            sourceNodes.push_back(mod.first);
        }
    }

    juce::Logger::writeToLog(
        "[Beautify] Found " + juce::String(sourceNodes.size()) + " source nodes");

    // --- STEP 2: Assign Nodes to Columns (Topological Sort with Cycle Handling) ---
    juce::Logger::writeToLog("[Beautify] Starting topological sort...");
    std::map<juce::uint32, int>            nodeColumn;
    std::vector<std::vector<juce::uint32>> columns;
    int                                    maxColumn = 0;

    // Initialize source nodes in column 0
    for (juce::uint32 nodeId : sourceNodes)
    {
        nodeColumn[nodeId] = 0;
    }
    columns.push_back(sourceNodes);
    juce::Logger::writeToLog("[Beautify] Initialized source nodes in column 0");

    // Process each column and assign children to appropriate columns
    // Use topological sort with cycle detection: track visited nodes to prevent infinite loops
    std::queue<juce::uint32> processQueue;
    for (juce::uint32 srcNode : sourceNodes)
        processQueue.push(srcNode);

    const int                   MAX_COLUMNS = 50; // Maximum columns to prevent excessive spacing
    std::map<juce::uint32, int> visitCount;       // Track how many times each node is visited
    const int MAX_VISITS = 3; // Allow a node to be visited up to 3 times (handles some cycles)

    while (!processQueue.empty())
    {
        juce::uint32 u = processQueue.front();
        processQueue.pop();

        visitCount[u]++;

        // Safety check: if node visited too many times, cap its column and skip
        if (visitCount[u] > MAX_VISITS)
        {
            // Cap the column assignment for this node if not already set
            if (nodeColumn.count(u) == 0)
            {
                nodeColumn[u] = MAX_COLUMNS / 2; // Place in middle
            }
            juce::Logger::writeToLog(
                "[Beautify] WARNING: Node " + juce::String(u) + " visited " +
                juce::String(visitCount[u]) + " times (cycle detected), capping column");
            continue;
        }

        // Safety check: ensure adjacencyList has this node
        if (adjacencyList.find(u) == adjacencyList.end())
        {
            continue;
        }

        // Get current column of node u (default to 0 if not set)
        int uColumn = nodeColumn.count(u) > 0 ? nodeColumn[u] : 0;

        for (juce::uint32 v : adjacencyList[u])
        {
            // The column for node 'v' is the maximum of its predecessors' columns + 1
            int newColumn = uColumn + 1;

            // Cap the column to prevent excessive spacing
            if (newColumn > MAX_COLUMNS)
            {
                newColumn = MAX_COLUMNS;
            }

            // Only update if this gives a higher column (or if not set yet)
            // This allows nodes to be placed in the rightmost column they need
            if (nodeColumn.count(v) == 0 || newColumn > nodeColumn[v])
            {
                nodeColumn[v] = newColumn;
                maxColumn = std::max(maxColumn, newColumn);

                // Only push to queue if we haven't visited it too many times
                if (visitCount.count(v) == 0 || visitCount[v] < MAX_VISITS)
                {
                    processQueue.push(v);
                }
            }
        }
    }

    // Handle unvisited nodes (disconnected or part of cycles that weren't reached)
    for (const auto& mod : modules)
    {
        if (nodeColumn.count(mod.first) == 0)
        {
            // Assign to a reasonable column (middle)
            nodeColumn[mod.first] = MAX_COLUMNS / 2;
            juce::Logger::writeToLog(
                "[Beautify] Unvisited node " + juce::String(mod.first) + " assigned to column " +
                juce::String(MAX_COLUMNS / 2));
        }
    }

    // Ensure output node is assigned (rightmost)
    if (nodeColumn.count(0) == 0)
    {
        nodeColumn[0] = maxColumn + 1;
        maxColumn++;
    }
    else
    {
        // Output should be rightmost
        if (nodeColumn[0] <= maxColumn)
        {
            nodeColumn[0] = maxColumn + 1;
            maxColumn++;
        }
    }

    // Cap maxColumn to MAX_COLUMNS
    if (maxColumn > MAX_COLUMNS)
    {
        maxColumn = MAX_COLUMNS;
        juce::Logger::writeToLog("[Beautify] Capped maxColumn to " + juce::String(MAX_COLUMNS));
    }

    juce::Logger::writeToLog(
        "[Beautify] Topological sort complete, maxColumn=" + juce::String(maxColumn));

    // Re-populate columns based on assignments
    juce::Logger::writeToLog("[Beautify] Re-populating columns...");
    if (maxColumn < 0)
    {
        juce::Logger::writeToLog("[Beautify] ERROR: maxColumn is negative, setting to 0");
        maxColumn = 0;
    }
    columns.assign(maxColumn + 1, {});
    for (const auto& pair : nodeColumn)
    {
        if (pair.second >= 0 && pair.second < (int)columns.size())
        {
            columns[pair.second].push_back(pair.first);
        }
        else
        {
            juce::Logger::writeToLog(
                "[Beautify] WARNING: Node " + juce::String(pair.first) + " has invalid column " +
                juce::String(pair.second));
        }
    }

    juce::Logger::writeToLog(
        "[Beautify] Arranged nodes into " + juce::String(maxColumn + 1) + " columns");

    // --- STEP 3: Optimize Node Ordering Within Columns ---
    juce::Logger::writeToLog("[Beautify] Optimizing node ordering within columns...");
    // Sort nodes in each column based on median position of their parents
    for (int c = 1; c <= maxColumn; ++c)
    {
        std::map<juce::uint32, float> medianPositions;

        for (juce::uint32 nodeId : columns[c])
        {
            std::vector<float> parentPositions;

            // Find all parents in previous columns
            for (const auto& pair : adjacencyList)
            {
                for (juce::uint32 dest : pair.second)
                {
                    if (dest == nodeId)
                    {
                        // Find the vertical index of the parent node
                        int parentColumn = nodeColumn[pair.first];
                        if (parentColumn >= 0 && parentColumn < (int)columns.size())
                        {
                            auto& parentColVec = columns[parentColumn];
                            auto  it =
                                std::find(parentColVec.begin(), parentColVec.end(), pair.first);
                            if (it != parentColVec.end())
                            {
                                parentPositions.push_back(
                                    (float)std::distance(parentColVec.begin(), it));
                            }
                        }
                    }
                }
            }

            if (!parentPositions.empty())
            {
                std::sort(parentPositions.begin(), parentPositions.end());
                medianPositions[nodeId] = parentPositions[parentPositions.size() / 2];
            }
            else
            {
                medianPositions[nodeId] = 0.0f;
            }
        }

        // Sort the column based on median positions
        std::sort(columns[c].begin(), columns[c].end(), [&](juce::uint32 a, juce::uint32 b) {
            return medianPositions[a] < medianPositions[b];
        });
    }
    juce::Logger::writeToLog("[Beautify] Node ordering optimization complete");

    // --- STEP 4: Calculate Final Coordinates ---
    juce::Logger::writeToLog("[Beautify] Calculating final coordinates...");
    // NOTE: We intentionally size columns based on the *actual* node widths so
    // that wide nodes (e.g. timeline / sampler / sequencer) do not overlap
    // adjacent columns.
    const float COLUMN_HORIZONTAL_PADDING = 80.0f;
    const float NODE_VERTICAL_PADDING = 50.0f;

    // Compute per-column maximum width based on the current node sizes
    // Use fallback dimensions if GetNodeDimensions returns zero (node not yet rendered)
    auto&       themeMgr = ThemeManager::getInstance();
    const float DEFAULT_NODE_WIDTH = themeMgr.getNodeDefaultWidth();
    const float DEFAULT_NODE_HEIGHT = 150.0f; // Standard height for most modules

    // Cache node dimensions to avoid repeated lookups
    std::map<juce::uint32, ImVec2> nodeDimensionCache;

    auto getCachedNodeDimensions = [&](juce::uint32 lid) -> ImVec2 {
        auto it = nodeDimensionCache.find(lid);
        if (it != nodeDimensionCache.end())
        {
            return it->second;
        }

        ImVec2 nodeSize = ImVec2(0.0f, 0.0f);

        // Try to get actual rendered dimensions first
        // Use a timeout/check to avoid blocking on function_generator
        try
        {
            ImVec2 actualSize = ImNodes::GetNodeDimensions(lid);
            if (actualSize.x > 0.0f && actualSize.y > 0.0f)
            {
                nodeSize = actualSize;
                juce::Logger::writeToLog(
                    "[Beautify] Node " + juce::String(lid) + " dimensions from ImNodes: " +
                    juce::String(actualSize.x, 1) + "x" + juce::String(actualSize.y, 1));
            }
        }
        catch (...)
        {
            juce::Logger::writeToLog(
                "[Beautify] Exception getting dimensions for node " + juce::String(lid) +
                ", using fallback");
        }

        // Fallback: Use PinDatabase defaultWidth if dimensions are invalid
        if (nodeSize.x <= 0.0f || nodeSize.y <= 0.0f)
        {
            float fallbackWidth = DEFAULT_NODE_WIDTH;
            float fallbackHeight = DEFAULT_NODE_HEIGHT;

            // Try to get module type and look up in PinDatabase
            try
            {
                if (synth != nullptr)
                {
                    juce::String moduleType = synth->getModuleTypeForLogical(lid);
                    if (moduleType.isNotEmpty())
                    {
                        const auto& pinDb = getModulePinDatabase();
                        auto        pinIt = pinDb.find(moduleType.toLowerCase());
                        if (pinIt != pinDb.end())
                        {
                            NodeWidth widthCategory = pinIt->second.defaultWidth;
                            float     categoryWidth = getWidthForCategory(widthCategory);

                            // If category width is valid (not Exception), use it
                            if (categoryWidth > 0.0f)
                            {
                                fallbackWidth = categoryWidth;
                            }
                            else if (widthCategory == NodeWidth::Exception)
                            {
                                // Exception nodes might have custom size, use wider default
                                fallbackWidth = DEFAULT_NODE_WIDTH * 1.5f;
                                juce::Logger::writeToLog(
                                    "[Beautify] Node " + juce::String(lid) + " (" + moduleType +
                                    ") is Exception size, using " + juce::String(fallbackWidth, 1) +
                                    "px");
                            }

                            juce::Logger::writeToLog(
                                "[Beautify] Node " + juce::String(lid) + " (" + moduleType +
                                ") fallback width: " + juce::String(fallbackWidth, 1) + "px");
                        }
                        else
                        {
                            juce::Logger::writeToLog(
                                "[Beautify] Node " + juce::String(lid) + " (" + moduleType +
                                ") not found in PinDatabase, using default");
                        }
                    }
                    else if (lid == 0)
                    {
                        // Output node - use default width
                        fallbackWidth = DEFAULT_NODE_WIDTH;
                    }
                }
            }
            catch (...)
            {
                juce::Logger::writeToLog(
                    "[Beautify] Exception getting module type for node " + juce::String(lid) +
                    ", using default dimensions");
            }

            nodeSize = ImVec2(fallbackWidth, fallbackHeight);
        }

        nodeDimensionCache[lid] = nodeSize;
        return nodeSize;
    };

    juce::Logger::writeToLog("[Beautify] Computing column widths...");
    std::vector<float> columnWidths(maxColumn + 1, 0.0f);
    for (int c = 0; c <= maxColumn; ++c)
    {
        float maxWidth = 0.0f;
        for (juce::uint32 lid : columns[c])
        {
            ImVec2 nodeSize = getCachedNodeDimensions(lid);
            maxWidth = std::max(maxWidth, nodeSize.x);
        }
        columnWidths[c] = maxWidth;
    }
    juce::Logger::writeToLog("[Beautify] Column widths computed");

    // Compute column X positions as cumulative sum of widths + padding
    std::vector<float> columnX(maxColumn + 1, 0.0f);
    float              accumulatedX = 0.0f;
    for (int c = 0; c <= maxColumn; ++c)
    {
        columnX[c] = accumulatedX;
        // Use minimum width for empty columns to prevent layout issues
        float colWidth = columnWidths[c] > 0.0f ? columnWidths[c] : DEFAULT_NODE_WIDTH;
        accumulatedX += colWidth + COLUMN_HORIZONTAL_PADDING;
    }

    // Find the tallest column to center shorter ones
    float tallestColumnHeight = 0.0f;
    for (const auto& col : columns)
    {
        float height = 0.0f;
        for (juce::uint32 lid : col)
        {
            ImVec2 nodeSize = getCachedNodeDimensions(lid);
            height += nodeSize.y + NODE_VERTICAL_PADDING;
        }
        tallestColumnHeight = std::max(tallestColumnHeight, height);
    }

    // --- STEP 5: Apply Positions ---
    for (int c = 0; c <= maxColumn; ++c)
    {
        // Calculate column height for centering
        float columnHeight = 0.0f;
        for (juce::uint32 lid : columns[c])
        {
            ImVec2 nodeSize = getCachedNodeDimensions(lid);
            columnHeight += nodeSize.y + NODE_VERTICAL_PADDING;
        }

        // Start Y position (centered vertically)
        float currentY = (tallestColumnHeight - columnHeight) / 2.0f;

        for (juce::uint32 lid : columns[c])
        {
            float x = columnX[c];
            pendingNodePositions[(int)lid] = ImVec2(x, currentY);

            ImVec2 nodeSize = getCachedNodeDimensions(lid);
            currentY += nodeSize.y + NODE_VERTICAL_PADDING;
        }
    }

    // Position the output node to the right of all other modules, respecting its width
    float  finalX = accumulatedX;
    ImVec2 outputNodeSize = getCachedNodeDimensions(0);
    float  outputNodeY = (tallestColumnHeight - outputNodeSize.y) / 2.0f;
    pendingNodePositions[0] = ImVec2(finalX, outputNodeY);
    juce::Logger::writeToLog("[Beautify] Applied position to Output Node");

    juce::Logger::writeToLog(
        "[Beautify] Applied positions to " + juce::String(modules.size()) + " nodes");
    juce::Logger::writeToLog("--- [Beautify Layout] Complete ---");
}
void ImGuiNodeEditorComponent::handleConnectSelectedToTrackMixer()
{
    if (synth == nullptr || ImNodes::NumSelectedNodes() <= 0)
    {
        juce::Logger::writeToLog("[AutoConnect] Aborted: No synth or no nodes selected.");
        return;
    }

    // This is a significant action, so create an undo state first.
    pushSnapshot();
    juce::Logger::writeToLog("--- [Connect to Mixer] Starting routine ---");

    // 1. Get all selected node IDs.
    const int        numSelectedNodes = ImNodes::NumSelectedNodes();
    std::vector<int> selectedNodeLids(numSelectedNodes);
    ImNodes::GetSelectedNodes(selectedNodeLids.data());

    // 2. Find the geometric center of the selected nodes to position our new modules.
    float totalX = 0.0f, maxX = 0.0f, totalY = 0.0f;
    bool  anyValidPos = false;
    for (int lid : selectedNodeLids)
    {
        ImVec2 pos = ImNodes::GetNodeGridSpacePos(lid);
        if (pos.x != 0.0f || pos.y != 0.0f)
            anyValidPos = true;

        totalX += pos.x;
        totalY += pos.y;
        if (pos.x > maxX)
        {
            maxX = pos.x;
        }
    }
    ImVec2 centerPos = ImVec2(totalX / numSelectedNodes, totalY / numSelectedNodes);

    // FIX: If positions are all (0,0) (e.g. not yet rendered), fallback to visible screen center
    if (!anyValidPos || (centerPos.x == 0.0f && centerPos.y == 0.0f))
    {
        // Calculate center of visible area in grid space
        // Center = (-panning + canvasSize/2) / zoom
        ImVec2 visibleCenter;
        visibleCenter.x = (-lastEditorPanning.x + (lastCanvasSize.x * 0.5f)) / lastZoom;
        visibleCenter.y = (-lastEditorPanning.y + (lastCanvasSize.y * 0.5f)) / lastZoom;

        centerPos = visibleCenter;
        maxX = visibleCenter.x; // Place mixer relative to this fallback

        juce::Logger::writeToLog(
            "[AutoConnect] Nodes have invalid positions (0,0). Fallback to screen center: " +
            juce::String(centerPos.x) + ", " + juce::String(centerPos.y));
    }

    // 3. Compute the TOTAL number of Audio outputs across ALL selected nodes.
    //    This defines how many mixer tracks we need.
    struct NodeAudioOut
    {
        juce::uint32 logicalId;
        int          numAudioOuts;
    };
    std::vector<NodeAudioOut> nodesWithAudio;
    nodesWithAudio.reserve(selectedNodeLids.size());

    int totalAudioOutputs = 0;
    for (int lid : selectedNodeLids)
    {
        if (auto* mp = synth->getModuleForLogical((juce::uint32)lid))
        {
            // AudioProcessor::getTotalNumOutputChannels() returns AUDIO channel count
            int audioCh = mp->getTotalNumOutputChannels();

            // --- SPECIAL CASE FIXES ---
            // Some modules (like tts_performer) expose control signals as audio outputs.
            // We only want to connect the actual audio output (usually ch 0).
            if (mp->getName().equalsIgnoreCase("tts_performer"))
            {
                audioCh = 1; // Force to 1 channel (Audio Out)
                juce::Logger::writeToLog(
                    "[AutoConnect] Limiting tts_performer to 1 output channel.");
            }
            else if (mp->getName().equalsIgnoreCase("polyvco"))
            {
                // Dynamic PolyVCO logic: use the "numVoices" parameter to determine active outputs
                if (auto* poly = dynamic_cast<PolyVCOModuleProcessor*>(mp))
                {
                    if (auto* param = dynamic_cast<juce::AudioParameterInt*>(
                            poly->getAPVTS().getParameter("numVoices")))
                    {
                        audioCh = param->get();
                        juce::Logger::writeToLog(
                            "[AutoConnect] PolyVCO detected. Active voices: " +
                            juce::String(audioCh));
                    }
                    else
                    {
                        audioCh = 1; // Fallback
                        juce::Logger::writeToLog(
                            "[AutoConnect] PolyVCO detected but numVoices param not found. "
                            "Defaulting to 1.");
                    }
                }
            }
            else if (mp->getName().equalsIgnoreCase("physics"))
            {
                audioCh = 2; // Limit to L/R audio outputs, ignoring triggers/CV
                juce::Logger::writeToLog("[AutoConnect] Limiting physics to 2 output channels.");
            }

            // --- EXCLUSIONS (CV/Control modules that shouldn't connect to audio mixer) ---
            static const std::vector<juce::String> excludedModules = {
                "midi_player",
                "multi_sequencer",
                "step_sequencer",
                "stroke_sequencer",
                "snapshot_sequencer",
                "pose_estimator",
                "hand_tracker",
                "face_tracker",
                "object_detector",
                "movement_detector",
                "contour_detector",
                "color_tracker",
                "midi_buttons",
                "midi_faders",
                "midi_knobs",
                "midi_pads"};

            for (const auto& excluded : excludedModules)
            {
                if (mp->getName().equalsIgnoreCase(excluded))
                {
                    audioCh = 0;
                    juce::Logger::writeToLog(
                        "[AutoConnect] Skipping " + mp->getName() + " (CV/Control source).");
                    break;
                }
            }
            // --------------------------

            if (audioCh > 0)
            {
                nodesWithAudio.push_back({(juce::uint32)lid, audioCh});
                totalAudioOutputs += audioCh;
            }
        }
    }

    if (totalAudioOutputs <= 0)
    {
        juce::Logger::writeToLog("[AutoConnect] No audio outputs found on selected nodes.");
        return;
    }

    // 4. Create the Value node and set it to the TOTAL number of audio outputs (tracks).
    auto valueNodeId = synth->addModule("value");
    auto valueLid = synth->getLogicalIdForNode(valueNodeId);
    if (auto* valueProc = dynamic_cast<ValueModuleProcessor*>(synth->getModuleForLogical(valueLid)))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                valueProc->getAPVTS().getParameter("value")))
        {
            *p = (float)totalAudioOutputs;
            juce::Logger::writeToLog(
                "[AutoConnect] Created Value node " + juce::String(valueLid) +
                " and set its value to total audio outputs = " + juce::String(totalAudioOutputs));
        }
    }
    // Position it slightly to the right of the center of the selection.
    pendingNodePositions[(int)valueLid] = ImVec2(maxX + 200.0f, centerPos.y - 100.0f);

    // 4. Create the Track Mixer node.
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    // Position it to the right of the right-most selected node for a clean signal flow.
    pendingNodePositions[(int)mixerLid] = ImVec2(maxX + 600.0f, centerPos.y);
    juce::Logger::writeToLog(
        "[AutoConnect] Created Track Mixer with logical ID " + juce::String(mixerLid));

    // 6. Connect the Value node to the Track Mixer's "Num Tracks Mod" input.
    // The Value module's "Raw" output is channel 0 (provides the exact value entered by the user).
    // The Track Mixer's "Num Tracks Mod" is on Bus 1, Channel 0, which is absolute channel 64.
    synth->connect(valueNodeId, 0, mixerNodeId, TrackMixerModuleProcessor::MAX_TRACKS);
    juce::Logger::writeToLog(
        "[AutoConnect] Connected Value node 'Raw' output to Track Mixer's Num Tracks Mod input.");

    // 7. Connect ALL audio outputs to sequential mixer inputs in a stable order.
    //    Maintain selection order, and within each node, preserve channel order 0..N-1.
    int mixerInputChannel = 0;
    for (const auto& entry : nodesWithAudio)
    {
        auto sourceNodeId = synth->getNodeIdForLogical(entry.logicalId);
        for (int ch = 0; ch < entry.numAudioOuts; ++ch)
        {
            if (mixerInputChannel >= TrackMixerModuleProcessor::MAX_TRACKS)
            {
                juce::Logger::writeToLog(
                    "[AutoConnect] Reached mixer max tracks while wiring; remaining outputs "
                    "skipped.");
                break;
            }
            // Skip if this mixer input is already connected (idempotency)
            bool inputAlreadyConnected = false;
            for (const auto& c : synth->getConnectionsInfo())
            {
                if (c.dstLogicalId == (juce::uint32)mixerLid && c.dstChan == mixerInputChannel)
                {
                    inputAlreadyConnected = true;
                    break;
                }
            }
            if (!inputAlreadyConnected)
            {
                synth->connect(sourceNodeId, ch, mixerNodeId, mixerInputChannel);
                juce::Logger::writeToLog(
                    "[AutoConnect] Connected node " + juce::String((int)entry.logicalId) +
                    " (Out " + juce::String(ch) + ") -> Mixer In " +
                    juce::String(mixerInputChannel + 1));
            }
            mixerInputChannel++;
            if (mixerInputChannel >= TrackMixerModuleProcessor::MAX_TRACKS)
                break;
        }
        if (mixerInputChannel >= TrackMixerModuleProcessor::MAX_TRACKS)
            break;
    }

    // 8. Flag the graph for a rebuild to apply all changes.
    graphNeedsRebuild = true;
    juce::Logger::writeToLog("--- [Connect to Mixer] Routine complete. ---");
}

void ImGuiNodeEditorComponent::handleConnectSelectedToRecorder()
{
    if (synth == nullptr || ImNodes::NumSelectedNodes() <= 0)
    {
        juce::Logger::writeToLog("[AutoConnect] Aborted: No synth or no nodes selected.");
        return;
    }

    // This is a significant action, so create an undo state first.
    pushSnapshot();
    juce::Logger::writeToLog("--- [Connect to Recorder] Starting routine ---");

    // 1. Get all selected node IDs.
    const int        numSelectedNodes = ImNodes::NumSelectedNodes();
    std::vector<int> selectedNodeLids(numSelectedNodes);
    ImNodes::GetSelectedNodes(selectedNodeLids.data());

    // 2. Find the rightmost position of the selected nodes to position recorders.
    float maxX = 0.0f;
    for (int lid : selectedNodeLids)
    {
        ImVec2 pos = ImNodes::GetNodeGridSpacePos(lid);
        if (pos.x > maxX)
        {
            maxX = pos.x;
        }
    }

    // 3. Create a recorder for each selected node that has audio outputs.
    int   recorderCount = 0;
    float verticalSpacing = 200.0f; // Vertical spacing between recorders
    float startY = 0.0f;

    // Calculate starting Y position (center of selected nodes vertically)
    float totalY = 0.0f;
    int   validNodeCount = 0;
    for (int lid : selectedNodeLids)
    {
        if (auto* mp = synth->getModuleForLogical((juce::uint32)lid))
        {
            const int audioCh = mp->getTotalNumOutputChannels();
            if (audioCh > 0)
            {
                ImVec2 pos = ImNodes::GetNodeGridSpacePos(lid);
                totalY += pos.y;
                validNodeCount++;
            }
        }
    }

    if (validNodeCount == 0)
    {
        juce::Logger::writeToLog("[AutoConnect] No audio outputs found on selected nodes.");
        return;
    }

    startY = totalY / validNodeCount - (validNodeCount - 1) * verticalSpacing / 2.0f;

    // 4. Create a recorder for each selected node with audio outputs.
    for (int lid : selectedNodeLids)
    {
        if (auto* mp = synth->getModuleForLogical((juce::uint32)lid))
        {
            const int audioCh = mp->getTotalNumOutputChannels();
            if (audioCh == 0)
            {
                juce::Logger::writeToLog(
                    "[AutoConnect] Skipping node " + juce::String(lid) + " (no audio outputs)");
                continue;
            }

            // Create a recorder for this node
            auto recorderNodeId = synth->addModule("recorder");
            auto recorderLid = synth->getLogicalIdForNode(recorderNodeId);

            // Position the recorder to the right of the source node
            ImVec2 sourcePos = ImNodes::GetNodeGridSpacePos(lid);
            float  recorderX = maxX + 800.0f;
            float  recorderY = startY + recorderCount * verticalSpacing;
            pendingNodePositions[(int)recorderLid] = ImVec2(recorderX, recorderY);

            // Get the source node ID
            auto sourceNodeId = synth->getNodeIdForLogical((juce::uint32)lid);

            // Connect the source to the recorder
            if (audioCh == 1)
            {
                // Mono source: connect to left channel
                synth->connect(sourceNodeId, 0, recorderNodeId, 0);
                juce::Logger::writeToLog(
                    "[AutoConnect] Connected mono node " + juce::String(lid) +
                    " (Out 0) -> Recorder " + juce::String(recorderLid) + " In L (0)");
            }
            else if (audioCh >= 2)
            {
                // Stereo source: connect to both channels
                synth->connect(sourceNodeId, 0, recorderNodeId, 0);
                synth->connect(sourceNodeId, 1, recorderNodeId, 1);
                juce::Logger::writeToLog(
                    "[AutoConnect] Connected stereo node " + juce::String(lid) +
                    " (Out 0,1) -> Recorder " + juce::String(recorderLid) + " In L,R (0,1)");
            }

            // Set suggested filename for the recorder
            if (auto* recorder =
                    dynamic_cast<RecordModuleProcessor*>(synth->getModuleForLogical(recorderLid)))
            {
                recorder->setPropertiesFile(PresetCreatorApplication::getApp().getProperties());
                if (auto* sourceModule = synth->getModuleForLogical((juce::uint32)lid))
                {
                    recorder->updateSuggestedFilename(sourceModule->getName());
                }
            }

            recorderCount++;
            juce::Logger::writeToLog(
                "[AutoConnect] Created Recorder " + juce::String(recorderLid) + " for node " +
                juce::String(lid));
        }
    }

    // 5. Flag the graph for a rebuild to apply all changes.
    graphNeedsRebuild = true;
    juce::Logger::writeToLog(
        "--- [Connect to Recorder] Routine complete. Created " + juce::String(recorderCount) +
        " recorder(s). ---");
}

void ImGuiNodeEditorComponent::handleMidiPlayerAutoConnect(
    MIDIPlayerModuleProcessor* midiPlayer,
    juce::uint32               midiPlayerLid)
{
    if (!synth || !midiPlayer || midiPlayerLid == 0 || !midiPlayer->hasMIDIFileLoaded())
    {
        juce::Logger::writeToLog("[AutoConnect] Aborted: MIDI Player not ready.");
        return;
    }

    juce::Logger::writeToLog(
        "--- [AutoConnect to Samplers] Starting routine for MIDI Player " +
        juce::String(midiPlayerLid) + " ---");

    // 1. Get initial positions and clear existing connections from the MIDI Player.
    auto   midiPlayerNodeId = synth->getNodeIdForLogical(midiPlayerLid);
    ImVec2 midiPlayerPos = ImNodes::GetNodeGridSpacePos((int)midiPlayerLid);
    synth->clearConnectionsForNode(midiPlayerNodeId);

    // --- FIX: Create and position the Track Mixer first ---
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(midiPlayerPos.x + 1200.0f, midiPlayerPos.y);
    juce::Logger::writeToLog(
        "[AutoConnect] Created Track Mixer with logical ID " + juce::String(mixerLid));

    // --- FIX: Connect MIDI Player "Num Tracks" output to Track Mixer "Num Tracks Mod" input ---
    // This ensures the Track Mixer automatically adjusts its track count based on the MIDI file
    // content
    synth->connect(
        midiPlayerNodeId,
        MIDIPlayerModuleProcessor::kNumTracksChannelIndex,
        mixerNodeId,
        TrackMixerModuleProcessor::MAX_TRACKS);
    juce::Logger::writeToLog(
        "[AutoConnect] Connected MIDI Player Num Tracks to Track Mixer Num Tracks Mod");

    // 2. Create and connect a Sample Loader for each active MIDI track.
    const auto& activeTrackIndices = midiPlayer->getActiveTrackIndices();
    juce::Logger::writeToLog(
        "[AutoConnect] MIDI file has " + juce::String(activeTrackIndices.size()) +
        " active tracks.");

    for (int i = 0; i < (int)activeTrackIndices.size(); ++i)
    {
        if (i >= MIDIPlayerModuleProcessor::kMaxTracks)
            break;

        // A. Create and position the new modules.
        auto samplerNodeId = synth->addModule("sample_loader");
        auto samplerLid = synth->getLogicalIdForNode(samplerNodeId);
        pendingNodePositions[(int)samplerLid] =
            ImVec2(midiPlayerPos.x + 800.0f, midiPlayerPos.y + (i * 350.0f));

        auto mapRangeNodeId = synth->addModule("map_range");
        auto mapRangeLid = synth->getLogicalIdForNode(mapRangeNodeId);
        pendingNodePositions[(int)mapRangeLid] =
            ImVec2(midiPlayerPos.x + 400.0f, midiPlayerPos.y + (i * 350.0f));

        // B. Configure the MapRange module for Pitch CV conversion.
        if (auto* mapRangeProc =
                dynamic_cast<MapRangeModuleProcessor*>(synth->getModuleForLogical(mapRangeLid)))
        {
            auto& ap = mapRangeProc->getAPVTS();
            // MIDI Player Pitch Out (0..1) -> Sample Loader Pitch Mod (-24..+24 semitones)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("inMin")))
                *p = 0.0f;
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("inMax")))
                *p = 1.0f;
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMin")))
                *p = -24.0f;
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMax")))
                *p = 24.0f;
        }

        // C. Connect the outputs for this track.
        const int pitchChan = i * MIDIPlayerModuleProcessor::kOutputsPerTrack + 0;
        const int gateChan = i * MIDIPlayerModuleProcessor::kOutputsPerTrack + 1;
        const int trigChan = i * MIDIPlayerModuleProcessor::kOutputsPerTrack + 3;

        // Pitch: MIDI Player -> MapRange -> Sample Loader
        synth->connect(midiPlayerNodeId, pitchChan, mapRangeNodeId, 0); // Pitch Out -> MapRange In
        synth->connect(
            mapRangeNodeId, 1, samplerNodeId, 0); // MapRange Raw Out -> SampleLoader Pitch Mod In

        // Gate: MIDI Player -> Sample Loader
        synth->connect(
            midiPlayerNodeId, gateChan, samplerNodeId, 2); // Gate Out -> SampleLoader Gate Mod In

        // Trigger: MIDI Player -> Sample Loader
        synth->connect(
            midiPlayerNodeId,
            trigChan,
            samplerNodeId,
            3); // Trigger Out -> SampleLoader Trigger Mod In

        // --- FIX: Connect the Sample Loader's audio output to the Track Mixer ---
        // The Sample Loader's main audio output is channel 0.
        // The Track Mixer's inputs are mono channels 0, 1, 2...
        synth->connect(samplerNodeId, 0, mixerNodeId, i);
    }

    // --- FIX: Connect the mixer to the main output so you can hear it! ---
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Mixer Out L -> Main Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Mixer Out R -> Main Out R

    // 3. Flag the graph for a rebuild to apply all changes.
    graphNeedsRebuild = true;
    juce::Logger::writeToLog("--- [AutoConnect to Samplers] Routine complete. ---");
}
void ImGuiNodeEditorComponent::handleMidiPlayerAutoConnectVCO(
    MIDIPlayerModuleProcessor* midiPlayer,
    juce::uint32               midiPlayerLid)
{
    if (!synth || !midiPlayer || midiPlayerLid == 0 || !midiPlayer->hasMIDIFileLoaded())
    {
        juce::Logger::writeToLog("[AutoConnectVCO] Aborted: MIDI Player not ready.");
        return;
    }

    juce::Logger::writeToLog(
        "--- [AutoConnectVCO] Starting routine for MIDI Player " + juce::String(midiPlayerLid) +
        " ---");

    // 1. Get initial positions and clear all existing connections from the MIDI Player.
    auto   midiPlayerNodeId = synth->getNodeIdForLogical(midiPlayerLid);
    ImVec2 midiPlayerPos = ImNodes::GetNodeGridSpacePos((int)midiPlayerLid);
    synth->clearConnectionsForNode(midiPlayerNodeId);

    // 2. Create and position the PolyVCO and Track Mixer.
    auto polyVcoNodeId = synth->addModule("polyvco");
    auto polyVcoLid = synth->getLogicalIdForNode(polyVcoNodeId);
    pendingNodePositions[(int)polyVcoLid] = ImVec2(midiPlayerPos.x + 400.0f, midiPlayerPos.y);
    juce::Logger::writeToLog(
        "[AutoConnectVCO] Created PolyVCO with logical ID " + juce::String(polyVcoLid));

    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(midiPlayerPos.x + 800.0f, midiPlayerPos.y);
    juce::Logger::writeToLog(
        "[AutoConnectVCO] Created Track Mixer with logical ID " + juce::String(mixerLid));

    // 3. Connect the track count outputs to control both new modules.
    synth->connect(
        midiPlayerNodeId,
        MIDIPlayerModuleProcessor::kRawNumTracksChannelIndex,
        polyVcoNodeId,
        0); // Raw Num Tracks -> PolyVCO Num Voices Mod
    synth->connect(
        midiPlayerNodeId,
        MIDIPlayerModuleProcessor::kRawNumTracksChannelIndex,
        mixerNodeId,
        TrackMixerModuleProcessor::MAX_TRACKS); // Raw Num Tracks -> Mixer Num Tracks Mod
    juce::Logger::writeToLog(
        "[AutoConnectVCO] Connected MIDI Player raw track counts to PolyVCO and Track Mixer "
        "modulation inputs.");

    // 4. Loop through active MIDI tracks to connect CV routes and audio.
    const auto& activeTrackIndices = midiPlayer->getActiveTrackIndices();
    juce::Logger::writeToLog(
        "[AutoConnectVCO] MIDI file has " + juce::String(activeTrackIndices.size()) +
        " active tracks. Patching voices...");

    for (int i = 0; i < (int)activeTrackIndices.size(); ++i)
    {
        if (i >= PolyVCOModuleProcessor::MAX_VOICES)
            break; // Don't try to connect more voices than the PolyVCO has

        int sourceTrackIndex = activeTrackIndices[i];

        // A. Connect CV modulation routes from MIDI Player to the corresponding PolyVCO voice.
        int pitchChan = i * MIDIPlayerModuleProcessor::kOutputsPerTrack + 0;
        int velChan = i * MIDIPlayerModuleProcessor::kOutputsPerTrack + 2;

        // Connect MIDI CV to the corresponding PolyVCO voice inputs
        synth->connect(midiPlayerNodeId, pitchChan, polyVcoNodeId, 1 + i); // Pitch -> Freq Mod
        synth->connect(
            midiPlayerNodeId,
            velChan,
            polyVcoNodeId,
            1 + PolyVCOModuleProcessor::MAX_VOICES * 2 + i); // Velocity -> Gate Mod

        // B. Connect the PolyVCO voice's audio output to the Track Mixer's input.
        synth->connect(polyVcoNodeId, i, mixerNodeId, i * 2);
        synth->connect(polyVcoNodeId, i, mixerNodeId, i * 2 + 1);
    }

    // 5. Connect the Track Mixer to the main audio output.
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Mixer Out L -> Main Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Mixer Out R -> Main Out R

    // 6. Flag the graph for a rebuild.
    graphNeedsRebuild = true;
    juce::Logger::writeToLog("--- [AutoConnectVCO] Routine complete. ---");
}
void ImGuiNodeEditorComponent::handleMidiPlayerAutoConnectHybrid(
    MIDIPlayerModuleProcessor* midiPlayer,
    juce::uint32               midiPlayerLid)
{
    if (!synth || !midiPlayer)
        return;

    pushSnapshot();

    const int numTracks = midiPlayer->getNumTracks();
    if (numTracks == 0)
        return;

    auto   midiPlayerNodeId = synth->getNodeIdForLogical(midiPlayerLid);
    ImVec2 midiPos = ImNodes::GetNodeGridSpacePos((int)midiPlayerLid);

    // --- THIS IS THE NEW "FIND-BY-TRACING" LOGIC ---

    juce::uint32 polyVcoLid = 0;
    juce::uint32 trackMixerLid = 0;

    // 1. Scan existing connections to find modules to reuse by tracing backwards.
    // First, find a TrackMixer connected to the output.
    for (const auto& conn : synth->getConnectionsInfo())
    {
        if (conn.dstIsOutput &&
            synth->getModuleTypeForLogical(conn.srcLogicalId).equalsIgnoreCase("track_mixer"))
        {
            trackMixerLid = conn.srcLogicalId; // Found a TrackMixer to reuse!
            break;
        }
    }
    // If we found a TrackMixer, now find a PolyVCO connected to it.
    if (trackMixerLid != 0)
    {
        for (const auto& conn : synth->getConnectionsInfo())
        {
            if (conn.dstLogicalId == trackMixerLid &&
                synth->getModuleTypeForLogical(conn.srcLogicalId).equalsIgnoreCase("polyvco"))
            {
                polyVcoLid = conn.srcLogicalId; // Found a PolyVCO to reuse!
                break;
            }
        }
    }

    // 2. Clear all old Pitch/Gate/Velocity connections from the MIDI Player.
    std::vector<ModularSynthProcessor::ConnectionInfo> oldConnections;
    for (const auto& conn : synth->getConnectionsInfo())
    {
        if (conn.srcLogicalId == midiPlayerLid && conn.srcChan < 16 * 3)
            oldConnections.push_back(conn);
    }
    for (const auto& conn : oldConnections)
    {
        synth->disconnect(
            synth->getNodeIdForLogical(conn.srcLogicalId),
            conn.srcChan,
            synth->getNodeIdForLogical(conn.dstLogicalId),
            conn.dstChan);
    }

    // 3. If we didn't find a PolyVCO to reuse after tracing, create a new one.
    if (polyVcoLid == 0)
    {
        auto polyVcoNodeId = synth->addModule("polyvco", false);
        polyVcoLid = synth->getLogicalIdForNode(polyVcoNodeId);
        pendingNodePositions[(int)polyVcoLid] = ImVec2(midiPos.x + 400.0f, midiPos.y);
    }

    // 4. If we didn't find a TrackMixer to reuse after tracing, create a new one.
    if (trackMixerLid == 0)
    {
        auto trackMixerNodeId = synth->addModule("track_mixer", false);
        trackMixerLid = synth->getLogicalIdForNode(trackMixerNodeId);
        pendingNodePositions[(int)trackMixerLid] = ImVec2(midiPos.x + 800.0f, midiPos.y);
    }
    // --- END OF NEW LOGIC ---

    auto polyVcoNodeId = synth->getNodeIdForLogical(polyVcoLid);
    auto trackMixerNodeId = synth->getNodeIdForLogical(trackMixerLid);

    if (auto* vco = dynamic_cast<PolyVCOModuleProcessor*>(synth->getModuleForLogical(polyVcoLid)))
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(vco->getAPVTS().getParameter("numVoices")))
            *p = numTracks;
    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(trackMixerLid)))
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")))
            *p = numTracks;

    int voicesToConnect = std::min({numTracks, PolyVCOModuleProcessor::MAX_VOICES, 64});
    for (int i = 0; i < voicesToConnect; ++i)
    {
        synth->connect(midiPlayerNodeId, i, polyVcoNodeId, 1 + i);
        synth->connect(
            midiPlayerNodeId,
            i + 16,
            polyVcoNodeId,
            1 + PolyVCOModuleProcessor::MAX_VOICES * 2 + i);
        synth->connect(polyVcoNodeId, i, trackMixerNodeId, i * 2);
        synth->connect(polyVcoNodeId, i, trackMixerNodeId, i * 2 + 1);
    }

    synth->connect(trackMixerNodeId, 0, synth->getOutputNodeID(), 0);
    synth->connect(trackMixerNodeId, 1, synth->getOutputNodeID(), 1);

    synth->commitChanges();
}

void ImGuiNodeEditorComponent::handleStrokeSeqBuildDrumKit(
    StrokeSequencerModuleProcessor* strokeSeq,
    juce::uint32                    strokeSeqLid)
{
    if (!synth || !strokeSeq)
        return;

    juce::Logger::writeToLog("🥁 BUILD DRUM KIT handler called! Creating modules...");

    // 1. Get Stroke Sequencer position
    auto   seqNodeId = synth->getNodeIdForLogical(strokeSeqLid);
    ImVec2 seqPos = ImNodes::GetNodeGridSpacePos((int)strokeSeqLid);

    // 2. Create 3 Sample Loaders (for Floor, Mid, Ceiling triggers)
    auto sampler1NodeId = synth->addModule("sample_loader");
    auto sampler2NodeId = synth->addModule("sample_loader");
    auto sampler3NodeId = synth->addModule("sample_loader");

    auto sampler1Lid = synth->getLogicalIdForNode(sampler1NodeId);
    auto sampler2Lid = synth->getLogicalIdForNode(sampler2NodeId);
    auto sampler3Lid = synth->getLogicalIdForNode(sampler3NodeId);

    // Position samplers in a vertical stack to the right
    pendingNodePositions[(int)sampler1Lid] = ImVec2(seqPos.x + 400.0f, seqPos.y);
    pendingNodePositions[(int)sampler2Lid] = ImVec2(seqPos.x + 400.0f, seqPos.y + 220.0f);
    pendingNodePositions[(int)sampler3Lid] = ImVec2(seqPos.x + 400.0f, seqPos.y + 440.0f);

    // 3. Create Track Mixer (will be set to 6 tracks by Value node)
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(seqPos.x + 800.0f, seqPos.y + 200.0f);

    // 4. Create Value node set to 6.0 (for 3 stereo tracks = 6 channels)
    auto valueNodeId = synth->addModule("value");
    auto valueLid = synth->getLogicalIdForNode(valueNodeId);
    pendingNodePositions[(int)valueLid] = ImVec2(seqPos.x + 600.0f, seqPos.y + 550.0f);

    if (auto* valueNode = dynamic_cast<ValueModuleProcessor*>(synth->getModuleForLogical(valueLid)))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(valueNode->getAPVTS().getParameter("value")) =
            6.0f;
    }

    // 5. Connect Stroke Sequencer TRIGGERS to Sample Loader TRIGGER MOD inputs (channel 3)
    synth->connect(seqNodeId, 0, sampler1NodeId, 3); // Floor Trig   -> Sampler 1 Trigger Mod
    synth->connect(seqNodeId, 1, sampler2NodeId, 3); // Mid Trig     -> Sampler 2 Trigger Mod
    synth->connect(seqNodeId, 2, sampler3NodeId, 3); // Ceiling Trig -> Sampler 3 Trigger Mod

    // 6. Connect Sample Loader AUDIO OUTPUTS to Track Mixer AUDIO INPUTS (stereo pairs)
    // Sampler 1 (L+R) -> Mixer Audio 1+2
    synth->connect(sampler1NodeId, 0, mixerNodeId, 0); // Sampler 1 L -> Mixer Audio 1
    synth->connect(sampler1NodeId, 1, mixerNodeId, 1); // Sampler 1 R -> Mixer Audio 2

    // Sampler 2 (L+R) -> Mixer Audio 3+4
    synth->connect(sampler2NodeId, 0, mixerNodeId, 2); // Sampler 2 L -> Mixer Audio 3
    synth->connect(sampler2NodeId, 1, mixerNodeId, 3); // Sampler 2 R -> Mixer Audio 4

    // Sampler 3 (L+R) -> Mixer Audio 5+6
    synth->connect(sampler3NodeId, 0, mixerNodeId, 4); // Sampler 3 L -> Mixer Audio 5
    synth->connect(sampler3NodeId, 1, mixerNodeId, 5); // Sampler 3 R -> Mixer Audio 6

    // 7. Connect Value node (6.0) to Track Mixer's "Num Tracks" input
    synth->connect(valueNodeId, 0, mixerNodeId, 64); // Value (6) -> Num Tracks Mod

    // 8. Connect Track Mixer output to global output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Mixer Out L -> Global Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Mixer Out R -> Global Out R

    synth->commitChanges();
    graphNeedsRebuild = true;
}

void ImGuiNodeEditorComponent::handleAnimationBuildTriggersAudio(
    AnimationModuleProcessor* animModule,
    juce::uint32              animModuleLid)
{
    if (!synth || !animModule)
        return;

    // Query the dynamic output pins to determine how many bones are tracked
    auto dynamicPins = animModule->getDynamicOutputPins();

    // Each bone has 3 outputs: Vel X, Vel Y, Hit
    // So number of bones = number of pins / 3
    int numTrackedBones = (int)dynamicPins.size() / 3;

    if (numTrackedBones == 0)
    {
        juce::Logger::writeToLog("🦶 BUILD TRIGGERS AUDIO: No tracked bones! Add bones first.");
        return;
    }

    juce::Logger::writeToLog(
        "🦶 BUILD TRIGGERS AUDIO handler called! Creating modules for " +
        juce::String(numTrackedBones) + " tracked bones...");

    // 1. Get Animation Module position
    auto   animNodeId = synth->getNodeIdForLogical(animModuleLid);
    ImVec2 animPos = ImNodes::GetNodeGridSpacePos((int)animModuleLid);

    // 2. Create one Sample Loader per tracked bone
    std::vector<juce::AudioProcessorGraph::NodeID> samplerNodeIds;
    std::vector<juce::uint32>                      samplerLids;

    for (int i = 0; i < numTrackedBones; ++i)
    {
        auto samplerNodeId = synth->addModule("sample_loader");
        samplerNodeIds.push_back(samplerNodeId);
        samplerLids.push_back(synth->getLogicalIdForNode(samplerNodeId));

        // Position samplers in a vertical stack to the right
        pendingNodePositions[(int)samplerLids[i]] =
            ImVec2(animPos.x + 400.0f, animPos.y + i * 220.0f);
    }

    // 3. Create Track Mixer (num_bones * 2 for stereo pairs)
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] =
        ImVec2(animPos.x + 800.0f, animPos.y + (numTrackedBones * 110.0f));

    // 4. Create Value node for mixer track count
    int  numMixerTracks = numTrackedBones * 2; // 2 channels per sampler (stereo)
    auto valueNodeId = synth->addModule("value");
    auto valueLid = synth->getLogicalIdForNode(valueNodeId);
    pendingNodePositions[(int)valueLid] =
        ImVec2(animPos.x + 600.0f, animPos.y + (numTrackedBones * 220.0f));

    if (auto* valueNode = dynamic_cast<ValueModuleProcessor*>(synth->getModuleForLogical(valueLid)))
    {
        *dynamic_cast<juce::AudioParameterFloat*>(valueNode->getAPVTS().getParameter("value")) =
            (float)numMixerTracks;
    }

    // 5. Connect Animation Module TRIGGERS to Sample Loader TRIGGER MOD inputs
    // Animation Module Output Channels (per bone):
    //   i*3 + 0: Bone Vel X
    //   i*3 + 1: Bone Vel Y
    //   i*3 + 2: Bone Hit (trigger) ← Connect this to sampler
    for (int i = 0; i < numTrackedBones; ++i)
    {
        int triggerChannel = i * 3 + 2; // Every 3rd channel starting at 2 (2, 5, 8, 11, ...)
        synth->connect(
            animNodeId, triggerChannel, samplerNodeIds[i], 3); // Bone Hit -> Sampler Trigger Mod
    }

    // 6. Connect Sample Loader AUDIO OUTPUTS to Track Mixer AUDIO INPUTS (stereo pairs)
    for (int i = 0; i < numTrackedBones; ++i)
    {
        int mixerChannelL = i * 2;     // 0, 2, 4, 6, ...
        int mixerChannelR = i * 2 + 1; // 1, 3, 5, 7, ...

        synth->connect(
            samplerNodeIds[i], 0, mixerNodeId, mixerChannelL); // Sampler L -> Mixer Audio L
        synth->connect(
            samplerNodeIds[i], 1, mixerNodeId, mixerChannelR); // Sampler R -> Mixer Audio R
    }

    // 7. Connect Value node to Track Mixer's "Num Tracks" input
    synth->connect(valueNodeId, 0, mixerNodeId, 64); // Value -> Num Tracks Mod

    // 8. Connect Track Mixer output to global output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Mixer Out L -> Global Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Mixer Out R -> Global Out R

    synth->commitChanges();
    graphNeedsRebuild = true;

    juce::Logger::writeToLog(
        "🦶 BUILD TRIGGERS AUDIO complete! " + juce::String(numTrackedBones) +
        " samplers + mixer + wiring created.");
}
void ImGuiNodeEditorComponent::handleMultiSequencerAutoConnectSamplers(
    MultiSequencerModuleProcessor* sequencer,
    juce::uint32                   sequencerLid)
{
    if (!synth || !sequencer)
        return;

    // 1. Get Sequencer info and clear its old connections
    auto      seqNodeId = synth->getNodeIdForLogical(sequencerLid);
    ImVec2    seqPos = ImNodes::GetNodeGridSpacePos((int)sequencerLid);
    const int numSteps =
        static_cast<int>(sequencer->getAPVTS().getRawParameterValue("numSteps")->load());
    synth->clearConnectionsForNode(seqNodeId);

    // 2. Create the necessary Mixer
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(seqPos.x + 800.0f, seqPos.y + 100.0f);
    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(mixerLid)))
    {
        *dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")) =
            numSteps;
    }

    // 3. CREATE a Sample Loader for each step and connect its audio to the mixer
    for (int i = 0; i < numSteps; ++i)
    {
        auto samplerNodeId = synth->addModule("sample_loader");
        auto samplerLid = synth->getLogicalIdForNode(samplerNodeId);
        pendingNodePositions[(int)samplerLid] = ImVec2(seqPos.x + 400.0f, seqPos.y + (i * 220.0f));

        // Connect this sampler's audio output to the mixer's input
        synth->connect(samplerNodeId, 0 /*Audio Output*/, mixerNodeId, i);

        // Connect the Sequencer's CV/Trig for this step directly to the new sampler
        synth->connect(seqNodeId, 7 + i * 3 + 0, samplerNodeId, 0); // Pitch N -> Pitch Mod
        synth->connect(seqNodeId, 1, samplerNodeId, 2);             // Main Gate -> Gate Mod
        synth->connect(seqNodeId, 7 + i * 3 + 2, samplerNodeId, 3); // Trig N  -> Trigger Mod
    }

    // Connect Num Steps output (channel 6) to Track Mixer's Num Tracks Mod input (channel 64)
    synth->connect(seqNodeId, 6, mixerNodeId, 64); // Num Steps -> Num Tracks Mod

    // 4. Connect the mixer to the main output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Out R

    graphNeedsRebuild = true;
}

void ImGuiNodeEditorComponent::handleMultiSequencerAutoConnectVCO(
    MultiSequencerModuleProcessor* sequencer,
    juce::uint32                   sequencerLid)
{
    if (!synth || !sequencer)
        return;

    // 1. Get Sequencer info and clear its old connections
    auto      seqNodeId = synth->getNodeIdForLogical(sequencerLid);
    ImVec2    seqPos = ImNodes::GetNodeGridSpacePos((int)sequencerLid);
    const int numSteps =
        static_cast<int>(sequencer->getAPVTS().getRawParameterValue("numSteps")->load());
    synth->clearConnectionsForNode(seqNodeId);

    // 2. CREATE the PolyVCO and Track Mixer
    auto polyVcoNodeId = synth->addModule("polyvco");
    auto polyVcoLid = synth->getLogicalIdForNode(polyVcoNodeId);
    pendingNodePositions[(int)polyVcoLid] = ImVec2(seqPos.x + 400.0f, seqPos.y);
    if (auto* vco = dynamic_cast<PolyVCOModuleProcessor*>(synth->getModuleForLogical(polyVcoLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(vco->getAPVTS().getParameter("numVoices")))
            *p = numSteps;
    }

    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(seqPos.x + 800.0f, seqPos.y);
    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(mixerLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")))
            *p = numSteps;
    }

    // 3. Connect CV, Audio, and Main Output
    for (int i = 0; i < numSteps; ++i)
    {
        // Connect CV: Sequencer -> PolyVCO
        synth->connect(seqNodeId, 7 + i * 3 + 0, polyVcoNodeId, 1 + i); // Pitch N -> Freq N Mod
        synth->connect(
            seqNodeId,
            1,
            polyVcoNodeId,
            1 + PolyVCOModuleProcessor::MAX_VOICES * 2 + i); // Main Gate -> Gate N Mod

        // Connect Audio: PolyVCO -> Mixer
        synth->connect(polyVcoNodeId, i, mixerNodeId, i);
    }

    // Connect Num Steps output (channel 6) to PolyVCO's Num Voices Mod input (channel 0)
    synth->connect(seqNodeId, 6, polyVcoNodeId, 0); // Num Steps -> Num Voices Mod

    // Connect Num Steps output (channel 6) to Track Mixer's Num Tracks Mod input (channel 64)
    synth->connect(seqNodeId, 6, mixerNodeId, 64); // Num Steps -> Num Tracks Mod

    // Connect Mixer -> Main Output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Out R

    graphNeedsRebuild = true;
}

void ImGuiNodeEditorComponent::handlePolyVCOAutoConnectTrackMixer(
    PolyVCOModuleProcessor* polyVco,
    juce::uint32            polyVcoLid)
{
    if (!synth || !polyVco)
        return;

    // 1. Get PolyVCO info
    auto   vcoNodeId = synth->getNodeIdForLogical(polyVcoLid);
    ImVec2 vcoPos = ImNodes::GetNodeGridSpacePos((int)polyVcoLid);
    int    numVoices = 8;
    if (auto* param =
            dynamic_cast<juce::AudioParameterInt*>(polyVco->getAPVTS().getParameter("numVoices")))
    {
        numVoices = param->get();
    }

    // 2. CREATE the Track Mixer
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(vcoPos.x + 400.0f, vcoPos.y);
    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(mixerLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")))
            p->setValueNotifyingHost(p->convertTo0to1((float)numVoices));
    }

    // 3. Connect Audio: PolyVCO -> Mixer (connect all active voices)
    for (int i = 0; i < numVoices; ++i)
    {
        synth->connect(vcoNodeId, i, mixerNodeId, i); // Freq N -> Audio N
    }

    // 4. Connect Mixer -> Main Output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Out R

    graphNeedsRebuild = true;
}

void ImGuiNodeEditorComponent::openMetaModuleEditor(
    MetaModuleProcessor* metaModule,
    juce::uint32         metaLogicalId)
{
    closeMetaModuleEditor();

    if (metaModule == nullptr)
        return;

    auto session = std::make_unique<MetaModuleEditorSession>();
    session->context.reset(ImNodes::CreateContext());
    if (session->context == nullptr)
        return;

    ImNodes::SetCurrentContext(session->context.get());
    ImNodes::StyleColorsDark();
    ImNodesIO& io = ImNodes::GetIO();
    io.LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyAlt;

    session->meta = metaModule;
    session->metaLogicalId = metaLogicalId;
    session->graph = metaModule->getInternalGraph();

    if (session->graph != nullptr)
    {
        auto modules = session->graph->getModulesInfo();
        int  index = 0;
        for (const auto& mod : modules)
        {
            const int logicalId = (int)mod.first;
            const int row = index / 5;
            const int col = index % 5;
            session->nodePositions.emplace(logicalId, ImVec2(220.0f * col, 140.0f * row));
            ++index;
        }
    }

    ImNodes::SetCurrentContext(editorContext);
    metaEditorSession = std::move(session);
}

void ImGuiNodeEditorComponent::closeMetaModuleEditor()
{
    if (metaEditorSession)
    {
        if (metaEditorSession->context)
        {
            ImNodes::SetCurrentContext(metaEditorSession->context.get());
            ImNodes::DestroyContext(metaEditorSession->context.release());
        }
        metaEditorSession.reset();
    }
    if (editorContext != nullptr)
        ImNodes::SetCurrentContext(editorContext);
}

void ImGuiNodeEditorComponent::renderMetaModuleEditor(MetaModuleEditorSession& session)
{
    if (session.context == nullptr || session.meta == nullptr || session.graph == nullptr)
    {
        ImGui::TextUnformatted("Internal graph is unavailable.");
        return;
    }

    ImNodes::SetCurrentContext(session.context.get());

    auto modules = session.graph->getModulesInfo();

    if (session.nodePositions.empty())
    {
        int index = 0;
        for (const auto& mod : modules)
        {
            const int logicalId = (int)mod.first;
            const int row = index / 5;
            const int col = index % 5;
            session.nodePositions.emplace(logicalId, ImVec2(220.0f * col, 140.0f * row));
            ++index;
        }
    }

    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = juce::jmax(canvasSize.x, 640.0f);
    canvasSize.y = juce::jmax(canvasSize.y, 360.0f);

    ImGui::BeginChild("MetaEditorCanvas", canvasSize, true, ImGuiWindowFlags_NoScrollWithMouse);
    ImNodes::BeginNodeEditor();

    const auto& pinDb = getModulePinDatabase();

    auto drawPinsForModule = [&](ModuleProcessor* module, const ModulePinInfo* info) {
        std::vector<AudioPin> audioIns;
        std::vector<AudioPin> audioOuts;
        std::vector<ModPin>   modIns;

        if (info != nullptr)
        {
            audioIns.assign(info->audioIns.begin(), info->audioIns.end());
            audioOuts.assign(info->audioOuts.begin(), info->audioOuts.end());
            modIns.assign(info->modIns.begin(), info->modIns.end());
        }

        if (module != nullptr)
        {
            auto dynamicInputs = module->getDynamicInputPins();
            auto dynamicOutputs = module->getDynamicOutputPins();

            for (const auto& dyn : dynamicInputs)
                audioIns.emplace_back(dyn.name, dyn.channel, dyn.type);
            for (const auto& dyn : dynamicOutputs)
                audioOuts.emplace_back(dyn.name, dyn.channel, dyn.type);
        }

        for (const auto& pin : audioIns)
        {
            PinID pinId;
            pinId.logicalId = module != nullptr ? module->getLogicalId() : 0;
            pinId.channel = pin.channel;
            pinId.isInput = true;
            pinId.isMod = (pin.type != PinDataType::Audio);

            ImNodes::BeginInputAttribute(encodePinId(pinId));
            ImGui::Text("%s", pin.name.toRawUTF8());
            ImNodes::EndInputAttribute();
        }

        for (const auto& pin : modIns)
        {
            PinID pinId;
            pinId.logicalId = module != nullptr ? module->getLogicalId() : 0;
            pinId.channel = (int)pin.paramId.hashCode();
            pinId.isInput = true;
            pinId.isMod = true;

            ImNodes::BeginInputAttribute(encodePinId(pinId));
            ImGui::Text("%s", pin.name.toRawUTF8());
            ImNodes::EndInputAttribute();
        }

        for (const auto& pin : audioOuts)
        {
            PinID pinId;
            pinId.logicalId = module != nullptr ? module->getLogicalId() : 0;
            pinId.channel = pin.channel;
            pinId.isInput = false;
            pinId.isMod = (pin.type != PinDataType::Audio);

            ImNodes::BeginOutputAttribute(encodePinId(pinId));
            ImGui::Text("%s", pin.name.toRawUTF8());
            ImNodes::EndOutputAttribute();
        }
    };

    for (const auto& mod : modules)
    {
        const int          logicalId = (int)mod.first;
        const juce::String type = mod.second;
        ModuleProcessor*   module = session.graph->getModuleForLogical(mod.first);

        ImNodes::BeginNode(logicalId);
        ImGui::Text("%s", type.toRawUTF8());

        const ModulePinInfo* info = nullptr;
        auto                 it = pinDb.find(type);
        if (it != pinDb.end())
            info = &it->second;

        drawPinsForModule(module, info);

        ImNodes::EndNode();

        const auto posIt = session.nodePositions.find(logicalId);
        if (posIt != session.nodePositions.end())
            ImNodes::SetNodeGridSpacePos(logicalId, posIt->second);
    }

    session.linkIdToAttrs.clear();
    const auto connections = session.graph->getConnectionsInfo();
    for (const auto& conn : connections)
    {
        PinID srcPin;
        srcPin.logicalId = conn.srcLogicalId;
        srcPin.channel = conn.srcChan;
        srcPin.isInput = false;
        srcPin.isMod = false;

        PinID dstPin;
        if (conn.dstIsOutput)
        {
            dstPin.logicalId = 0;
        }
        else
        {
            dstPin.logicalId = conn.dstLogicalId;
        }
        dstPin.channel = conn.dstChan;
        dstPin.isInput = true;
        dstPin.isMod = false;

        const int srcAttr = encodePinId(srcPin);
        const int dstAttr = encodePinId(dstPin);

        const int linkId =
            (int)(((conn.srcLogicalId & 0xFFFF) << 16) ^ ((conn.dstLogicalId & 0xFFFF) << 1) ^
                  ((conn.srcChan & 0xFF) << 8) ^ (conn.dstChan & 0xFF) ^
                  (conn.dstIsOutput ? 0x4000 : 0x0));

        session.linkIdToAttrs[linkId] = {srcAttr, dstAttr};
        ImNodes::Link(linkId, srcAttr, dstAttr);
    }

    if (ImGui::BeginPopupContextWindow("MetaNodeEditorContext", ImGuiPopupFlags_MouseButtonRight))
    {
        if (ImGui::MenuItem("Delete Selected"))
        {
            const int numSelected = ImNodes::NumSelectedNodes();
            if (numSelected > 0)
            {
                std::vector<int> selected(numSelected);
                ImNodes::GetSelectedNodes(selected.data());
                for (int nodeId : selected)
                {
                    auto node = session.graph->getNodeIdForLogical((juce::uint32)nodeId);
                    if (node.uid != 0)
                        session.graph->removeModule(node);
                }
                session.graph->commitChanges();
                session.meta->refreshCachedLayout();
                session.dirty = true;
                ImNodes::ClearNodeSelection();
            }
        }
        ImGui::EndPopup();
    }

    ImNodes::MiniMap(0.2f);
    ImNodes::EndNodeEditor();

    int startAttr = 0;
    int endAttr = 0;
    if (ImNodes::IsLinkCreated(&startAttr, &endAttr))
    {
        PinID a = decodePinId(startAttr);
        PinID b = decodePinId(endAttr);

        PinID src = a;
        PinID dst = b;
        if (src.isInput && !dst.isInput)
            std::swap(src, dst);

        if (!src.isInput && dst.isInput)
        {
            auto srcNodeId = session.graph->getNodeIdForLogical(src.logicalId);
            juce::AudioProcessorGraph::NodeID dstNodeId;
            if (dst.logicalId == 0)
                dstNodeId = session.graph->getOutputNodeID();
            else
                dstNodeId = session.graph->getNodeIdForLogical(dst.logicalId);

            if (srcNodeId.uid != 0 && dstNodeId.uid != 0)
            {
                if (session.graph->connect(srcNodeId, src.channel, dstNodeId, dst.channel))
                {
                    session.graph->commitChanges();
                    session.meta->refreshCachedLayout();
                    session.dirty = true;
                }
            }
        }
    }

    int destroyedLink = 0;
    if (ImNodes::IsLinkDestroyed(&destroyedLink))
    {
        auto linkIt = session.linkIdToAttrs.find(destroyedLink);
        if (linkIt != session.linkIdToAttrs.end())
        {
            PinID src = decodePinId(linkIt->second.first);
            PinID dst = decodePinId(linkIt->second.second);

            auto srcNodeId = session.graph->getNodeIdForLogical(src.logicalId);
            juce::AudioProcessorGraph::NodeID dstNodeId;
            if (dst.logicalId == 0)
                dstNodeId = session.graph->getOutputNodeID();
            else
                dstNodeId = session.graph->getNodeIdForLogical(dst.logicalId);

            if (srcNodeId.uid != 0 && dstNodeId.uid != 0)
            {
                if (session.graph->disconnect(srcNodeId, src.channel, dstNodeId, dst.channel))
                {
                    session.graph->commitChanges();
                    session.meta->refreshCachedLayout();
                    session.dirty = true;
                }
            }
        }
    }

    for (const auto& mod : modules)
    {
        const int logicalId = (int)mod.first;
        session.nodePositions[logicalId] = ImNodes::GetNodeGridSpacePos(logicalId);
    }

    ImGui::EndChild();

    char searchBuffer[128];
    std::memset(searchBuffer, 0, sizeof(searchBuffer));
    std::strncpy(searchBuffer, session.moduleSearchTerm.toRawUTF8(), sizeof(searchBuffer) - 1);
    if (ImGui::InputTextWithHint(
            "##MetaModuleSearch", "Module type (e.g. vco)", searchBuffer, sizeof(searchBuffer)))
    {
        session.moduleSearchTerm = juce::String(searchBuffer);
    }

    if (ImGui::Button("Create Module"))
    {
        juce::String moduleType = session.moduleSearchTerm.trim();
        if (moduleType.isNotEmpty())
        {
            auto nodeId = session.graph->addModule(moduleType);
            if (nodeId.uid != 0)
            {
                auto logicalId = session.graph->getLogicalIdForNode(nodeId);
                session.nodePositions[(int)logicalId] =
                    ImVec2(40.0f * (float)session.nodePositions.size(), 40.0f);
                session.graph->commitChanges();
                session.meta->refreshCachedLayout();
                session.dirty = true;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Inlet"))
    {
        auto nodeId = session.graph->addModule("inlet");
        if (nodeId.uid != 0)
        {
            auto logicalId = session.graph->getLogicalIdForNode(nodeId);
            session.nodePositions[(int)logicalId] =
                ImVec2(40.0f * (float)session.nodePositions.size(), 40.0f);
            session.graph->commitChanges();
            session.meta->refreshCachedLayout();
            session.dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Outlet"))
    {
        auto nodeId = session.graph->addModule("outlet");
        if (nodeId.uid != 0)
        {
            auto logicalId = session.graph->getLogicalIdForNode(nodeId);
            session.nodePositions[(int)logicalId] =
                ImVec2(40.0f * (float)session.nodePositions.size(), 40.0f);
            session.graph->commitChanges();
            session.meta->refreshCachedLayout();
            session.dirty = true;
        }
    }

    ImNodes::SetCurrentContext(editorContext);
}
#ifndef AUDIO_ONLY_BUILD
void ImGuiNodeEditorComponent::handleColorTrackerAutoConnectPolyVCO(
    ColorTrackerModule* colorTracker,
    juce::uint32        colorTrackerLid)
{
    if (!synth || !colorTracker)
        return;

    // 1. Get ColorTracker info and get number of tracked colors
    auto   colorTrackerNodeId = synth->getNodeIdForLogical(colorTrackerLid);
    ImVec2 colorTrackerPos = ImNodes::GetNodeGridSpacePos((int)colorTrackerLid);

    // Get tracked colors count using the new helper method
    int numColors = colorTracker->getTrackedColorsCount();

    if (numColors == 0)
    {
        juce::Logger::writeToLog("[ColorTracker Auto-Connect] No colors tracked, aborting.");
        return;
    }

    // 2. Create PolyVCO with matching number of voices
    auto polyVcoNodeId = synth->addModule("polyvco");
    auto polyVcoLid = synth->getLogicalIdForNode(polyVcoNodeId);
    pendingNodePositions[(int)polyVcoLid] = ImVec2(colorTrackerPos.x + 400.0f, colorTrackerPos.y);

    if (auto* vco = dynamic_cast<PolyVCOModuleProcessor*>(synth->getModuleForLogical(polyVcoLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(vco->getAPVTS().getParameter("numVoices")))
            *p = numColors;
    }

    // 3. Create Track Mixer
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(colorTrackerPos.x + 800.0f, colorTrackerPos.y);

    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(mixerLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")))
            *p = numColors;
    }

    // 4. Connect Num Colors output to PolyVCO's NumVoices Mod and TrackMixer's Num Tracks Mod
    synth->connect(colorTrackerNodeId, 72, polyVcoNodeId, 0); // Num Colors -> NumVoices Mod
    synth->connect(colorTrackerNodeId, 72, mixerNodeId, 64);  // Num Colors -> Num Tracks Mod

    // 5. Connect ColorTracker outputs to PolyVCO inputs
    for (int i = 0; i < numColors; ++i)
    {
        // Map X position to pitch/frequency for voice i
        synth->connect(colorTrackerNodeId, i * 3 + 0, polyVcoNodeId, 1 + i); // X -> Freq Mod

        // Map Area to gate level for voice i
        const int gateModChannel = 1 + PolyVCOModuleProcessor::MAX_VOICES * 2 + i;
        synth->connect(
            colorTrackerNodeId, i * 3 + 2, polyVcoNodeId, gateModChannel); // Area -> Gate Mod
    }

    // 6. Connect PolyVCO audio outputs to Track Mixer inputs
    for (int i = 0; i < numColors; ++i)
    {
        synth->connect(polyVcoNodeId, i, mixerNodeId, i); // Voice i -> Mixer Track i
    }

    // 7. Connect Track Mixer to Main Output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Out R

    graphNeedsRebuild = true;
    juce::Logger::writeToLog(
        "[ColorTracker Auto-Connect] Connected " + juce::String(numColors) + " colors to PolyVCO.");
}
#endif

#ifndef AUDIO_ONLY_BUILD
void ImGuiNodeEditorComponent::handleColorTrackerAutoConnectSamplers(
    ColorTrackerModule* colorTracker,
    juce::uint32        colorTrackerLid)
{
    if (!synth || !colorTracker)
        return;

    // 1. Get ColorTracker info
    auto   colorTrackerNodeId = synth->getNodeIdForLogical(colorTrackerLid);
    ImVec2 colorTrackerPos = ImNodes::GetNodeGridSpacePos((int)colorTrackerLid);

    // Get tracked colors count
    int numColors = colorTracker->getTrackedColorsCount();

    if (numColors == 0)
    {
        juce::Logger::writeToLog("[ColorTracker Auto-Connect] No colors tracked, aborting.");
        return;
    }

    // 2. Create Track Mixer
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] =
        ImVec2(colorTrackerPos.x + 800.0f, colorTrackerPos.y + 100.0f);

    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(mixerLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")))
            *p = numColors;
    }

    // 3. Connect Num Colors output to TrackMixer's Num Tracks Mod
    synth->connect(colorTrackerNodeId, 72, mixerNodeId, 64); // Num Colors -> Num Tracks Mod

    // 4. Create a Sample Loader for each tracked color
    for (int i = 0; i < numColors; ++i)
    {
        auto samplerNodeId = synth->addModule("sample_loader");
        auto samplerLid = synth->getLogicalIdForNode(samplerNodeId);
        pendingNodePositions[(int)samplerLid] =
            ImVec2(colorTrackerPos.x + 400.0f, colorTrackerPos.y + (i * 220.0f));

        // Connect Sample Loader audio output to mixer
        synth->connect(samplerNodeId, 0, mixerNodeId, i); // Audio -> Mixer Track i

        // Connect ColorTracker CV outputs to Sample Loader modulation inputs
        synth->connect(colorTrackerNodeId, i * 3 + 0, samplerNodeId, 0); // X -> Pitch Mod
        synth->connect(colorTrackerNodeId, i * 3 + 2, samplerNodeId, 2); // Area -> Gate Mod
    }

    // 5. Connect Track Mixer to Main Output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0); // Out L
    synth->connect(mixerNodeId, 1, outputNodeId, 1); // Out R

    graphNeedsRebuild = true;
    juce::Logger::writeToLog(
        "[ColorTracker Auto-Connect] Connected " + juce::String(numColors) +
        " colors to Sample Loaders.");
}
#endif

void ImGuiNodeEditorComponent::handleChordArpAutoConnectPolyVCO(
    ChordArpModuleProcessor* chordArp,
    juce::uint32             chordArpLid)
{
    if (!synth || !chordArp)
        return;

    // 1. Get ChordArp info
    auto   arpNodeId = synth->getNodeIdForLogical(chordArpLid);
    ImVec2 arpPos = ImNodes::GetNodeGridSpacePos((int)chordArpLid);

    // 2. Create PolyVCO (4 voices default)
    auto polyVcoNodeId = synth->addModule("polyvco");
    auto polyVcoLid = synth->getLogicalIdForNode(polyVcoNodeId);
    pendingNodePositions[(int)polyVcoLid] = ImVec2(arpPos.x + 400.0f, arpPos.y);

    // Set PolyVCO voices to 4
    if (auto* vco = dynamic_cast<PolyVCOModuleProcessor*>(synth->getModuleForLogical(polyVcoLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(vco->getAPVTS().getParameter("numVoices")))
            *p = 4;
    }

    // 3. Create Track Mixer (4 tracks)
    auto mixerNodeId = synth->addModule("track_mixer");
    auto mixerLid = synth->getLogicalIdForNode(mixerNodeId);
    pendingNodePositions[(int)mixerLid] = ImVec2(arpPos.x + 800.0f, arpPos.y);

    if (auto* mixer =
            dynamic_cast<TrackMixerModuleProcessor*>(synth->getModuleForLogical(mixerLid)))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterInt*>(mixer->getAPVTS().getParameter("numTracks")))
            *p = 4;
    }

    // 4. Connect ChordArp -> PolyVCO
    // ChordArp outputs: Pitch 1-4 (0, 2, 4, 6), Gate 1-4 (1, 3, 5, 7)
    for (int i = 0; i < 4; ++i)
    {
        // Pitch
        synth->connect(arpNodeId, i * 2, polyVcoNodeId, 1 + i);

        // Gate
        // Gate Mod index: 1 + MAX_VOICES*2 + i
        int gateModIdx = 1 + PolyVCOModuleProcessor::MAX_VOICES * 2 + i;
        synth->connect(arpNodeId, i * 2 + 1, polyVcoNodeId, gateModIdx);
    }

    // 5. Connect PolyVCO -> Mixer
    for (int i = 0; i < 4; ++i)
    {
        synth->connect(polyVcoNodeId, i, mixerNodeId, i);
    }

    // 6. Connect Mixer -> Output
    auto outputNodeId = synth->getOutputNodeID();
    synth->connect(mixerNodeId, 0, outputNodeId, 0);
    synth->connect(mixerNodeId, 1, outputNodeId, 1);

    graphNeedsRebuild = true;
    juce::Logger::writeToLog("[ChordArp Auto-Connect] Connected 4 voices to PolyVCO.");
}

// Add this exact helper function to the class
void ImGuiNodeEditorComponent::parsePinName(
    const juce::String& fullName,
    juce::String&       outType,
    int&                outIndex)
{
    outIndex = -1; // Default to no index
    outType = fullName;

    if (fullName.contains(" "))
    {
        const juce::String lastWord = fullName.substring(fullName.lastIndexOfChar(' ') + 1);
        if (lastWord.containsOnly("0123456789"))
        {
            outIndex = lastWord.getIntValue();
            outType = fullName.substring(0, fullName.lastIndexOfChar(' '));
        }
    }
}

void ImGuiNodeEditorComponent::updateRerouteTypeFromConnections(juce::uint32 rerouteLogicalId)
{
    if (synth == nullptr)
        return;

    auto* reroute =
        dynamic_cast<RerouteModuleProcessor*>(synth->getModuleForLogical(rerouteLogicalId));
    if (reroute == nullptr)
        return;

    std::optional<PinDataType> resolvedType;
    const auto                 connections = synth->getConnectionsInfo();

    for (const auto& conn : connections)
    {
        if (!conn.dstIsOutput && conn.dstLogicalId == rerouteLogicalId)
        {
            PinID srcPin{conn.srcLogicalId, conn.srcChan, false, false, {}};
            resolvedType = getPinDataTypeForPin(srcPin);
            break;
        }
    }

    if (!resolvedType.has_value())
    {
        for (const auto& conn : connections)
        {
            if (conn.srcLogicalId == rerouteLogicalId && !conn.dstIsOutput)
            {
                PinID dstPin{conn.dstLogicalId, conn.dstChan, true, false, {}};
                resolvedType = getPinDataTypeForPin(dstPin);
                break;
            }
        }
    }

    if (resolvedType.has_value())
        reroute->setPassthroughType(*resolvedType);
    else
        reroute->setPassthroughType(PinDataType::Audio);
}

// Helper functions to get pins from modules
std::vector<AudioPin> ImGuiNodeEditorComponent::getOutputPins(const juce::String& moduleType)
{
    auto it = getModulePinDatabase().find(moduleType);
    if (it != getModulePinDatabase().end())
        return it->second.audioOuts;
    return {};
}

std::vector<AudioPin> ImGuiNodeEditorComponent::getInputPins(const juce::String& moduleType)
{
    auto it = getModulePinDatabase().find(moduleType);
    if (it != getModulePinDatabase().end())
        return it->second.audioIns;
    return {};
}

AudioPin* ImGuiNodeEditorComponent::findInputPin(
    const juce::String& moduleType,
    const juce::String& pinName)
{
    auto pins = getInputPins(moduleType);
    for (auto& pin : pins)
    {
        if (pin.name == pinName)
            return &pin;
    }
    return nullptr;
}

AudioPin* ImGuiNodeEditorComponent::findOutputPin(
    const juce::String& moduleType,
    const juce::String& pinName)
{
    auto pins = getOutputPins(moduleType);
    for (auto& pin : pins)
    {
        if (pin.name == pinName)
            return &pin;
    }
    return nullptr;
}

std::vector<juce::uint32> ImGuiNodeEditorComponent::findNodesOfType(const juce::String& moduleType)
{
    std::vector<juce::uint32> result;
    if (!synth)
        return result;

    for (const auto& modInfo : synth->getModulesInfo())
    {
        if (synth->getModuleTypeForLogical(modInfo.first) == moduleType)
        {
            result.push_back(modInfo.first);
        }
    }
    return result;
}

// New dynamic pin-fetching helper
std::vector<PinInfo> ImGuiNodeEditorComponent::getDynamicOutputPins(ModuleProcessor* module)
{
    std::vector<PinInfo> pins;
    if (!module)
        return pins;

    const int numOutputChannels = module->getBus(false, 0)->getNumberOfChannels();
    for (int i = 0; i < numOutputChannels; ++i)
    {
        juce::String pinName = module->getAudioOutputLabel(i);
        if (pinName.isNotEmpty())
        {
            pins.push_back({(uint32_t)i, pinName}); // Store the full pin name in the type field
        }
    }
    return pins;
}
// Template function implementations
template<typename TargetProcessorType>
void ImGuiNodeEditorComponent::connectToMonophonicTargets(
    ModuleProcessor*                            sourceNode,
    const std::map<juce::String, juce::String>& pinNameMapping,
    const std::vector<juce::uint32>&            targetLids)
{
    if (!synth || !sourceNode || targetLids.empty())
        return;

    juce::Logger::writeToLog(
        "[AutoConnect] connectToMonophonicTargets called for " + sourceNode->getName());

    // Get the source module type
    juce::String sourceModuleType;
    for (const auto& modInfo : synth->getModulesInfo())
    {
        if (synth->getModuleForLogical(modInfo.first) == sourceNode)
        {
            sourceModuleType = synth->getModuleTypeForLogical(modInfo.first);
            break;
        }
    }

    if (sourceModuleType.isEmpty())
        return;

    // Use provided target logical IDs explicitly
    auto targetNodes = targetLids;

    int currentTargetIndex = 0;

    // First, group all of the source node's output pins by their index number.
    // For example, "Pitch 1" and "Trig 1" will both be in the group for index 1.
    std::map<int, std::vector<PinInfo>> pinsByIndex;

    // THE FIX: Get pins directly from the module instance.
    auto outputPins = getDynamicOutputPins(sourceNode);

    for (const auto& pin : outputPins)
    {
        juce::String type;
        int          index = -1;
        parsePinName(pin.type, type, index); // Use pin.type instead of pin.name
        if (index != -1)
        {
            // Store channel ID as the pin's ID
            pinsByIndex[index].push_back({(uint32_t)pin.id, type});
        }
    }

    // Now, loop through each group of pins (each voice).
    for (auto const& [index, pinsInGroup] : pinsByIndex)
    {
        if (currentTargetIndex >= (int)targetNodes.size())
            break; // Stop if we run out of targets
        auto targetNodeId = targetNodes[currentTargetIndex];

        // For each pin in the group (e.g., for "Pitch 1" and "Trig 1")...
        for (const auto& pinInfo : pinsInGroup)
        {
            // Check if we have a connection rule for this pin type (e.g., "Pitch").
            if (pinNameMapping.count(pinInfo.type))
            {
                juce::String targetPinName = pinNameMapping.at(pinInfo.type);
                auto*        targetPin = findInputPin("sample_loader", targetPinName);

                // If the target pin exists, create the connection.
                if (targetPin)
                {
                    juce::uint32 sourceLogicalId = 0;
                    for (const auto& modInfo : synth->getModulesInfo())
                    {
                        if (synth->getModuleForLogical(modInfo.first) == sourceNode)
                        {
                            sourceLogicalId = modInfo.first;
                            break;
                        }
                    }
                    auto sourceNodeId = synth->getNodeIdForLogical(sourceLogicalId);
                    synth->connect(
                        sourceNodeId,
                        pinInfo.id,
                        synth->getNodeIdForLogical(targetNodeId),
                        targetPin->channel);
                }
            }
        }
        // IMPORTANT: Move to the next target module for the next voice.
        currentTargetIndex++;
    }
}
template<typename TargetProcessorType>
void ImGuiNodeEditorComponent::connectToPolyphonicTarget(
    ModuleProcessor*                            sourceNode,
    const std::map<juce::String, juce::String>& pinNameMapping)
{
    if (!synth || !sourceNode)
        return;

    juce::Logger::writeToLog(
        "[AutoConnect] connectToPolyphonicTarget called for " + sourceNode->getName());

    // Get the source module type
    juce::String sourceModuleType;
    juce::uint32 sourceLogicalId = 0;
    for (const auto& modInfo : synth->getModulesInfo())
    {
        if (synth->getModuleForLogical(modInfo.first) == sourceNode)
        {
            sourceModuleType = synth->getModuleTypeForLogical(modInfo.first);
            sourceLogicalId = modInfo.first;
            break;
        }
    }

    if (sourceModuleType.isEmpty())
        return;

    auto targetNodes = findNodesOfType("polyvco");
    if (targetNodes.empty())
        return;
    auto targetNodeId = targetNodes[0]; // Use the first available PolyVCO

    auto sourceNodeId = synth->getNodeIdForLogical(sourceLogicalId);

    // THE FIX: Get pins directly from the module instance, not the database.
    auto outputPins = getDynamicOutputPins(sourceNode);

    // Loop through every output pin on the source module.
    for (const auto& sourcePin : outputPins)
    {
        // Parse the source pin's name to get its type and index.
        juce::String sourceType;
        int          sourceIndex = -1;
        parsePinName(sourcePin.type, sourceType, sourceIndex); // Use pin.type instead of pin.name

        if (sourceIndex == -1)
            continue; // Skip pins that aren't numbered.

        // Check if we have a rule for this pin type (e.g., "Pitch" maps to "Freq").
        if (pinNameMapping.count(sourceType))
        {
            juce::String targetType = pinNameMapping.at(sourceType);
            // PolyVCO inputs use the format "Freq 1 Mod", "Gate 1 Mod", etc.
            juce::String targetPinName = targetType + " " + juce::String(sourceIndex) + " Mod";

            // Find that pin on the target and connect it if available.
            auto* targetPin = findInputPin("polyvco", targetPinName);
            if (targetPin)
            {
                synth->connect(
                    sourceNodeId,
                    sourcePin.id,
                    synth->getNodeIdForLogical(targetNodeId),
                    targetPin->channel);
            }
        }
    }
}
void ImGuiNodeEditorComponent::handleAutoConnectionRequests()
{
    if (!synth)
        return;

    for (const auto& modInfo : synth->getModulesInfo())
    {
        auto* module = synth->getModuleForLogical(modInfo.first);
        if (!module)
            continue;

        // --- Check MultiSequencer Flags ---
        if (auto* multiSeq = dynamic_cast<MultiSequencerModuleProcessor*>(module))
        {
            if (multiSeq->autoConnectSamplersTriggered.exchange(false))
            {
                handleMultiSequencerAutoConnectSamplers(
                    multiSeq, modInfo.first); // Call the new specific handler
                pushSnapshot();
                return;
            }
            if (multiSeq->autoConnectVCOTriggered.exchange(false))
            {
                handleMultiSequencerAutoConnectVCO(
                    multiSeq, modInfo.first); // Call the new specific handler
                pushSnapshot();
                return;
            }
        }

        // --- Check ColorTracker Flags ---
#ifndef AUDIO_ONLY_BUILD
        if (auto* colorTracker = dynamic_cast<ColorTrackerModule*>(module))
        {
            if (colorTracker->autoConnectPolyVCOTriggered.exchange(false))
            {
                handleColorTrackerAutoConnectPolyVCO(colorTracker, modInfo.first);
                pushSnapshot();
                return;
            }
            if (colorTracker->autoConnectSamplersTriggered.exchange(false))
            {
                handleColorTrackerAutoConnectSamplers(colorTracker, modInfo.first);
                pushSnapshot();
                return;
            }
        }
#endif

        // --- Check ChordArp Flags ---
        if (auto* chordArp = dynamic_cast<ChordArpModuleProcessor*>(module))
        {
            if (chordArp->autoConnectVCOTriggered.exchange(false))
            {
                handleChordArpAutoConnectPolyVCO(chordArp, modInfo.first);
                pushSnapshot();
                return;
            }
        }

        // --- Check PolyVCO Flags ---
        if (auto* polyVco = dynamic_cast<PolyVCOModuleProcessor*>(module))
        {
            if (polyVco->autoConnectTrackMixerTriggered.exchange(false))
            {
                handlePolyVCOAutoConnectTrackMixer(polyVco, modInfo.first);
                pushSnapshot();
                return;
            }
        }

        // --- Check StrokeSequencer Flags ---
        if (auto* strokeSeq = dynamic_cast<StrokeSequencerModuleProcessor*>(module))
        {
            if (strokeSeq->autoBuildDrumKitTriggered.exchange(false))
            {
                handleStrokeSeqBuildDrumKit(strokeSeq, modInfo.first);
                pushSnapshot();
                return;
            }
        }

        // --- Check AnimationModule Flags ---
        if (auto* animModule = dynamic_cast<AnimationModuleProcessor*>(module))
        {
            if (animModule->autoBuildTriggersAudioTriggered.exchange(false))
            {
                handleAnimationBuildTriggersAudio(animModule, modInfo.first);
                pushSnapshot();
                return;
            }
        }

        // --- Check MIDIPlayer Flags ---
        if (auto* midiPlayer = dynamic_cast<MIDIPlayerModuleProcessor*>(module))
        {
            if (midiPlayer->autoConnectTriggered.exchange(false)) // Samplers
            {
                handleMidiPlayerAutoConnect(
                    midiPlayer, modInfo.first); // Reuse old detailed handler
                pushSnapshot();
                return;
            }
            if (midiPlayer->autoConnectVCOTriggered.exchange(false))
            {
                handleMidiPlayerAutoConnectVCO(
                    midiPlayer, modInfo.first); // Reuse old detailed handler
                pushSnapshot();
                return;
            }
            if (midiPlayer->autoConnectHybridTriggered.exchange(false))
            {
                handleMidiPlayerAutoConnectHybrid(
                    midiPlayer, modInfo.first); // Reuse old detailed handler
                pushSnapshot();
                return;
            }
        }
    }
}

void ImGuiNodeEditorComponent::handleMIDICVConnectionRequest(
    juce::uint32           midiCVLid,
    MIDICVModuleProcessor* midiCV,
    int                    requestType)
{
    if (!synth || !midiCV)
        return;

    juce::Logger::writeToLog("[MIDI CV Quick Connect] Request type: " + juce::String(requestType));

    if (requestType == 1) // MidiLogger
    {
        // Get MIDI CV position for positioning new node
        ImVec2 midiCVPos = ImNodes::GetNodeEditorSpacePos(static_cast<int>(midiCVLid));
        auto   midiCVNodeId = synth->getNodeIdForLogical(midiCVLid);

        // 1. Create MidiLogger
        auto         midiLoggerNodeId = synth->addModule("midi_logger");
        juce::uint32 midiLoggerLid = synth->getLogicalIdForNode(midiLoggerNodeId);
        pendingNodeScreenPositions[(int)midiLoggerLid] = ImVec2(midiCVPos.x + 400.0f, midiCVPos.y);
        juce::Logger::writeToLog(
            "[MIDI CV Quick Connect] Created MidiLogger at LID " +
            juce::String((int)midiLoggerLid));

        // 2. Connect all 8 voices from MIDI CV to MidiLogger
        // Each voice has 3 outputs: Gate (baseChannel), Pitch (baseChannel+1), Vel (baseChannel+2)
        // MidiLogger expects: Gate (trackIdx*3+0), Pitch (trackIdx*3+1), Velo (trackIdx*3+2)
        // Since MIDI CV outputs match MidiLogger input layout, we can connect directly
        for (int voice = 0; voice < MIDICVModuleProcessor::NUM_VOICES; ++voice)
        {
            const int baseChannel = voice * 3;

            // Connect Gate, Pitch, and Vel for this voice
            synth->connect(
                midiCVNodeId, baseChannel + 0, midiLoggerNodeId, baseChannel + 0); // Gate
            synth->connect(
                midiCVNodeId, baseChannel + 1, midiLoggerNodeId, baseChannel + 1); // Pitch
            synth->connect(midiCVNodeId, baseChannel + 2, midiLoggerNodeId, baseChannel + 2); // Vel
        }

        juce::Logger::writeToLog(
            "[MIDI CV Quick Connect] Connected " + juce::String(MIDICVModuleProcessor::NUM_VOICES) +
            " voices: MIDI CV → MidiLogger");

        // Request graph rebuild to update connections
        graphNeedsRebuild = true;
    }
}

void ImGuiNodeEditorComponent::handleMIDIPlayerConnectionRequest(
    juce::uint32               midiPlayerLid,
    MIDIPlayerModuleProcessor* midiPlayer,
    int                        requestType)
{
    if (!synth || !midiPlayer)
        return;

    juce::Logger::writeToLog(
        "[MIDI Player Quick Connect] Request type: " + juce::String(requestType));

    // Get ALL tracks (don't filter by whether they have notes)
    const auto& notesByTrack = midiPlayer->getNotesByTrack();
    int         numTracks = (int)notesByTrack.size();

    if (numTracks == 0)
    {
        juce::Logger::writeToLog("[MIDI Player Quick Connect] No tracks in MIDI file");
        return;
    }

    // Get MIDI Player position for positioning new nodes
    ImVec2 playerPos = ImNodes::GetNodeEditorSpacePos(static_cast<int>(midiPlayerLid));
    auto   midiPlayerNodeId = synth->getNodeIdForLogical(midiPlayerLid);

    // Request Type: 1=PolyVCO, 2=Samplers, 3=Both
    juce::uint32 polyVCOLid = 0;
    juce::uint32 mixerLid = 0;

    if (requestType == 1 || requestType == 3) // PolyVCO or Both
    {
        // 1. Create PolyVCO
        auto polyVCONodeId = synth->addModule("polyvco");
        polyVCOLid = synth->getLogicalIdForNode(polyVCONodeId);
        pendingNodeScreenPositions[(int)polyVCOLid] = ImVec2(playerPos.x + 400.0f, playerPos.y);
        juce::Logger::writeToLog(
            "[MIDI Player Quick Connect] Created PolyVCO at LID " + juce::String((int)polyVCOLid));

        // 2. Create Track Mixer
        auto mixerNodeId = synth->addModule("track_mixer");
        mixerLid = synth->getLogicalIdForNode(mixerNodeId);
        pendingNodeScreenPositions[(int)mixerLid] = ImVec2(playerPos.x + 700.0f, playerPos.y);
        juce::Logger::writeToLog(
            "[MIDI Player Quick Connect] Created Track Mixer at LID " +
            juce::String((int)mixerLid));

        // 3. Connect MIDI Player tracks to PolyVCO
        // Connect ALL tracks, regardless of whether they have notes
        int trackIdx = 0;
        for (size_t i = 0; i < notesByTrack.size() && trackIdx < 32; ++i)
        {
            const int midiPitchPin = trackIdx * 4 + 1;
            const int midiGatePin = trackIdx * 4 + 0;
            const int midiVeloPin = trackIdx * 4 + 2;

            const int vcoFreqPin = trackIdx + 1;
            const int vcoWavePin = 32 + trackIdx + 1;
            const int vcoGatePin = 64 + trackIdx + 1;

            synth->connect(midiPlayerNodeId, midiPitchPin, polyVCONodeId, vcoFreqPin);
            synth->connect(midiPlayerNodeId, midiGatePin, polyVCONodeId, vcoGatePin);
            synth->connect(midiPlayerNodeId, midiVeloPin, polyVCONodeId, vcoWavePin);
            trackIdx++;
        }

        // 4. Connect Num Tracks to PolyVCO (Num Voices Mod on channel 0)
        synth->connect(
            midiPlayerNodeId,
            MIDIPlayerModuleProcessor::kRawNumTracksChannelIndex,
            polyVCONodeId,
            0);

        // 5. Connect PolyVCO outputs to Track Mixer inputs
        for (int i = 0; i < trackIdx; ++i)
        {
            synth->connect(polyVCONodeId, i, mixerNodeId, i);
        }

        // 6. Connect Num Tracks output to mixer's Num Tracks Mod input
        synth->connect(
            midiPlayerNodeId,
            MIDIPlayerModuleProcessor::kRawNumTracksChannelIndex,
            mixerNodeId,
            TrackMixerModuleProcessor::MAX_TRACKS);

        // 7. Connect Track Mixer to main output
        auto outputNodeId = synth->getOutputNodeID();
        synth->connect(mixerNodeId, 0, outputNodeId, 0); // L
        synth->connect(mixerNodeId, 1, outputNodeId, 1); // R

        juce::Logger::writeToLog(
            "[MIDI Player Quick Connect] Connected " + juce::String(trackIdx) +
            " tracks: MIDI Player → PolyVCO → Track Mixer → Output");
    }
    if (requestType == 2 || requestType == 3) // Samplers or Both
    {
        float samplerX = playerPos.x + 400.0f;
        float mixerX = playerPos.x + 700.0f;

        // If PolyVCO mode (Both), offset samplers and use same mixer
        if (requestType == 3)
        {
            samplerX += 300.0f; // Offset samplers if PolyVCO exists
            // Reuse existing mixer created in PolyVCO section
        }
        else
        {
            // 1. Create Track Mixer for Samplers-only mode
            auto mixerNodeId = synth->addModule("track_mixer");
            mixerLid = synth->getLogicalIdForNode(mixerNodeId);
            pendingNodeScreenPositions[(int)mixerLid] = ImVec2(mixerX, playerPos.y);
            juce::Logger::writeToLog(
                "[MIDI Player Quick Connect] Created Track Mixer at LID " +
                juce::String((int)mixerLid));
        }

        // 2. Create samplers and connect
        // Connect ALL tracks, regardless of whether they have notes
        auto mixerNodeId = synth->getNodeIdForLogical(mixerLid);
        int  trackIdx = 0;
        int  totalTracks = (int)notesByTrack.size();
        int  mixerStartChannel = (requestType == 3) ? totalTracks : 0; // Offset for "Both" mode

        for (size_t i = 0; i < notesByTrack.size(); ++i)
        {
            // Create SampleLoader
            float        samplerY = playerPos.y + (trackIdx * 150.0f);
            auto         samplerNodeId = synth->addModule("sample_loader");
            juce::uint32 samplerLid = synth->getLogicalIdForNode(samplerNodeId);
            pendingNodeScreenPositions[(int)samplerLid] = ImVec2(samplerX, samplerY);

            const int midiPitchPin = trackIdx * 4 + 1;
            const int midiGatePin = trackIdx * 4 + 0;
            const int midiTrigPin = trackIdx * 4 + 3;

            // Connect MIDI Player to Sampler
            synth->connect(midiPlayerNodeId, midiPitchPin, samplerNodeId, 0);
            synth->connect(midiPlayerNodeId, midiGatePin, samplerNodeId, 2);
            synth->connect(midiPlayerNodeId, midiTrigPin, samplerNodeId, 3);

            // Connect Sampler output to Track Mixer input
            synth->connect(samplerNodeId, 0, mixerNodeId, mixerStartChannel + trackIdx);

            trackIdx++;
        }

        // 3. Connect Num Tracks to mixer and route to output (only if not already done in PolyVCO
        // mode)
        if (requestType != 3)
        {
            synth->connect(
                midiPlayerNodeId,
                MIDIPlayerModuleProcessor::kRawNumTracksChannelIndex,
                mixerNodeId,
                TrackMixerModuleProcessor::MAX_TRACKS);

            // 4. Connect Track Mixer to output
            auto outputNodeId = synth->getOutputNodeID();
            synth->connect(mixerNodeId, 0, outputNodeId, 0);
            synth->connect(mixerNodeId, 1, outputNodeId, 1);

            juce::Logger::writeToLog(
                "[MIDI Player Quick Connect] Complete chain: " + juce::String(trackIdx) +
                " SampleLoaders → Track Mixer (with Num Tracks) → Stereo Output");
        }
        else
        {
            juce::Logger::writeToLog(
                "[MIDI Player Quick Connect] Connected " + juce::String(trackIdx) +
                " SampleLoaders → Track Mixer (channels " + juce::String(mixerStartChannel) + "-" +
                juce::String(mixerStartChannel + trackIdx - 1) +
                ") [Mixer already connected in PolyVCO section]");
        }
    }

    // Commit changes
    if (synth)
    {
        synth->commitChanges();
        graphNeedsRebuild = true;
    }

    pushSnapshot();
}

void ImGuiNodeEditorComponent::drawInsertNodeOnLinkPopup()
{
    if (ImGui::BeginPopup("InsertNodeOnLinkPopup"))
    {
        const int  numSelected = ImNodes::NumSelectedLinks();
        const bool isMultiInsert = numSelected > 1;

        // --- Module Insertion on Cables (Organized by Category) ---
        // Map format: {Display Name, Internal Type}
        // Internal types use lowercase with underscores for spaces
        const std::map<const char*, const char*> audioInsertable = {
            // Sources
            {"Sample Loader", "sample_loader"},
            {"Sample SFX", "sample_sfx"},
            // Effects
            {"VCF", "vcf"},
            {"Delay", "delay"},
            {"Reverb", "reverb"},
            {"Chorus", "chorus"},
            {"Phaser", "phaser"},
            {"Compressor", "compressor"},
            {"Limiter", "limiter"},
            {"Noise Gate", "gate"},
            {"Drive", "drive"},
            {"Spatial Granulator", "spatial_granulator"},
            {"Bit Crusher", "bit_crusher"},
            {"Graphic EQ", "graphic_eq"},
            {"Waveshaper", "waveshaper"},
            {"8-Band Shaper", "8bandshaper"},
            {"Granulator", "granulator"},
            {"Spatial Granulator", "spatial_granulator"},
            {"Harmonic Shaper", "harmonic_shaper"},
            {"Time/Pitch Shifter", "timepitch"},
            {"De-Crackle", "de_crackle"},
            // Utilities
            {"VCA", "vca"},
            {"Mixer", "mixer"},
            {"Attenuverter", "attenuverter"},
            {"Reroute", "reroute"},
            // Modulators
            {"Function Generator", "function_generator"},
            {"Shaping Oscillator", "shaping_oscillator"},
            // TTS
            {"Vocal Tract Filter", "vocal_tract_filter"},
            // Analysis
            {"Scope", "scope"},
            {"Frequency Graph", "frequency_graph"}};
        const std::map<const char*, const char*> modInsertable = {
            // Utilities
            {"Attenuverter", "attenuverter"},
            {"Lag Processor", "lag_processor"},
            {"Math", "math"},
            {"Map Range", "map_range"},
            {"Quantizer", "quantizer"},
            {"Rate", "rate"},
            {"Comparator", "comparator"},
            {"Logic", "logic"},
            {"Reroute", "reroute"},
            {"CV Mixer", "cv_mixer"},
            {"MIDI CV", "midi_cv"},
            {"OSC CV", "osc_cv"},
            {"CV OSC", "cv_osc_sender"},
            {"CV -> OSC", "cv_osc_sender"},
            {"PanVol", "panvol"},
            {"Sequential Switch", "sequential_switch"},
            // Modulators
            {"S&H", "s_and_h"},
            {"Function Generator", "function_generator"},
            {"Chord Arp", "chord_arp"},
            // Sequencers
            {"Timeline", "timeline"},
            {"Automation Lane", "automation_lane"},
            {"Automato", "automato"},
            // Analysis (CV outputs)
            {"BPM Monitor", "bpm_monitor"}};
#ifndef AUDIO_ONLY_BUILD
        const std::map<const char*, const char*> videoInsertable = {
            // Computer Vision (Video processing)
            // Passthrough nodes (Video In → Video Out)
            {"Video FX", "video_fx"},
            {"Chromakey", "chromakey"},
            {"Video Compositor", "video_compositor"},
            {"Video Draw Impact", "video_draw_impact"},
            {"Crop Video", "crop_video"},
            {"Video Viewer", "video_viewer"},
            {"Reroute", "reroute"},
            {"Movement Detector", "movement_detector"},
            {"Object Detector", "object_detector"},
            {"Pose Estimator", "pose_estimator"},
            {"Hand Tracker", "hand_tracker"},
            {"Face Tracker", "face_tracker"},
            {"Color Tracker", "color_tracker"},
            {"Contour Detector", "contour_detector"}};
#endif

        // Determine which list to show based on cable type
        const PinDataType srcType = getPinDataTypeForPin(linkToInsertOn.srcPin);
        const PinDataType dstType = getPinDataTypeForPin(linkToInsertOn.dstPin);
        const bool isVideoCable = (srcType == PinDataType::Video && dstType == PinDataType::Video);
#ifndef AUDIO_ONLY_BUILD
        const auto& listToShow = isVideoCable
                                     ? videoInsertable
                                     : (linkToInsertOn.isMod ? modInsertable : audioInsertable);
#else
        const auto& listToShow = (linkToInsertOn.isMod ? modInsertable : audioInsertable);
#endif

        if (isMultiInsert)
            ImGui::Text("Insert Node on %d Cables", numSelected);
        else
            ImGui::Text("Insert Node on Cable");

        // --- FIX: Iterate over map pairs instead of simple strings ---
        for (const auto& pair : listToShow)
        {
            // pair.first = display label, pair.second = internal type
            if (ImGui::MenuItem(pair.first))
            {
                if (isMultiInsert)
                {
                    handleInsertNodeOnSelectedLinks(pair.second);
                }
                else
                {
                    insertNodeBetween(pair.second);
                }
                ImGui::CloseCurrentPopup();
            }
        }

        // VST Plugins submenu (only for audio cables, not video cables)
        if (!linkToInsertOn.isMod && !isVideoCable)
        {
            ImGui::Separator();
            if (ImGui::BeginMenu("VST"))
            {
                drawVstMenuByManufacturer(isMultiInsert, isVideoCable);
                ImGui::EndMenu();
            }
        }

        ImGui::EndPopup();
    }
    else
    {
        // --- FIX: Reset state when popup is closed ---
        // If the popup is not open (i.e., it was closed or the user clicked away),
        // we must reset the state variable. This ensures that the application
        // is no longer "stuck" in the insert-on-link mode and right-click on
        // empty canvas will work again.
        linkToInsertOn.linkId = -1;
    }
}
void ImGuiNodeEditorComponent::drawLinkInspectorTooltip(const LinkInfo& link)
{
    if (synth == nullptr)
        return;
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    // Get the probe scope processor
    auto* scope = synth->getProbeScopeProcessor();
    if (scope == nullptr)
        return;

    // Get the statistics from the scope module
    float minVal, maxVal;
    scope->getStatistics(minVal, maxVal);

    // Get the scope buffer for waveform
    const auto& scopeBuffer = scope->getScopeBuffer();

    // Draw the text info
    ImGui::Text("Inspecting: %s", link.pinName.toRawUTF8());
    ImGui::Text("From: %s (ID %d)", link.sourceNodeName.toRawUTF8(), (int)link.srcNodeId);
    ImGui::Text("Pin: %s", link.pinName.toRawUTF8());

    ImGui::Separator();

    juce::String peakMaxText = juce::String::formatted("Peak Max: %.3f", maxVal);
    juce::String peakMinText = juce::String::formatted("Peak Min: %.3f", minVal);
    ThemeText(peakMaxText.toRawUTF8(), theme.modules.scope_text_max);
    ThemeText(peakMinText.toRawUTF8(), theme.modules.scope_text_min);

    float peakToPeak = maxVal - minVal;
    ImGui::Text("P-P: %.3f", peakToPeak);

    float dBMax = maxVal > 0.0001f ? 20.0f * std::log10(maxVal) : -100.0f;
    ImGui::Text("Max dBFS: %.1f", dBMax);

    ImGui::Separator();

    // Draw the waveform using ImGui PlotLines
    const int numSamples = scopeBuffer.getNumSamples();
    if (scopeBuffer.getNumChannels() > 0 && numSamples > 0)
    {
        const float* samples = scopeBuffer.getReadPointer(0);
        ImVec2       plotSize = ImVec2(ImGui::GetContentRegionAvail().x, 80.0f);
        ImGui::PlotLines("##Waveform", samples, numSamples, 0, nullptr, -1.0f, 1.0f, plotSize);
    }
}
// --- NEW HELPER FUNCTION ---
void ImGuiNodeEditorComponent::insertNodeOnLink(
    const juce::String& nodeType,
    const LinkInfo&     linkInfo,
    const ImVec2&       position)
{
    if (synth == nullptr)
    {
        juce::Logger::writeToLog("[InsertNodeOnLink] ERROR: synth is nullptr");
        return;
    }

    PinDataType srcType = getPinDataTypeForPin(linkInfo.srcPin);
    PinDataType dstType = getPinDataTypeForPin(linkInfo.dstPin);
    juce::Logger::writeToLog(
        "[InsertNodeOnLink] Inserting " + nodeType + " on link " + juce::String(linkInfo.linkId) +
        " srcLid=" + juce::String((int)linkInfo.srcPin.logicalId) +
        " srcCh=" + juce::String(linkInfo.srcPin.channel) +
        " dstLid=" + juce::String((int)linkInfo.dstPin.logicalId) +
        " dstCh=" + juce::String(linkInfo.dstPin.channel) +
        " srcType=" + juce::String(static_cast<int>(srcType)) +
        " dstType=" + juce::String(static_cast<int>(dstType)));

    // 1. Create and Position the New Node
    // Check if this is a VST plugin by checking against known plugins
    juce::AudioProcessorGraph::NodeID newNodeId;
    auto&                             app = PresetCreatorApplication::getApp();
    auto&                             knownPluginList = app.getKnownPluginList();
    bool                              isVst = false;

    // Get the VST folder at exe position for filtering
    juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
    juce::File vstFolder = exeDir.getChildFile("VST");

    for (const auto& desc : knownPluginList.getTypes())
    {
        if (desc.name == nodeType)
        {
            // Check if plugin is in the VST folder at exe position
            juce::File pluginFile(desc.fileOrIdentifier);
            if (pluginFile.existsAsFile())
            {
                juce::File pluginDir = pluginFile.getParentDirectory();
                if (pluginDir.isAChildOf(vstFolder) || pluginDir == vstFolder)
                {
                    // This is a VST plugin - use addVstModule
                    newNodeId = synth->addVstModule(app.getPluginFormatManager(), desc);
                    if (newNodeId.uid == 0)
                    {
                        juce::Logger::writeToLog(
                            "[InsertNode] ERROR: Failed to create VST module: " + desc.name);
                        return; // Don't disconnect if node creation failed
                    }
                    isVst = true;
                    break;
                }
            }
        }
    }

    if (!isVst)
    {
        // Regular module - use addModule
        newNodeId = synth->addModule(nodeType);
        if (newNodeId.uid == 0)
        {
            juce::Logger::writeToLog(
                "[InsertNodeOnLink] ERROR: Failed to create module: " + nodeType);
            return; // Don't disconnect if node creation failed
        }
        juce::Logger::writeToLog(
            "[InsertNodeOnLink] Created module " + nodeType +
            " with nodeId=" + juce::String(newNodeId.uid));
    }

    juce::String nodeName = isVst ? nodeType : juce::String(nodeType).replaceCharacter('_', ' ');
    if (!isVst)
    {
        nodeName = nodeName.toLowerCase();
        bool capitalizeNext = true;
        for (int i = 0; i < nodeName.length(); ++i)
        {
            if (capitalizeNext && juce::CharacterFunctions::isLetter(nodeName[i]))
            {
                nodeName = nodeName.substring(0, i) +
                           juce::String::charToString(nodeName[i]).toUpperCase() +
                           nodeName.substring(i + 1);
                capitalizeNext = false;
            }
            else if (nodeName[i] == ' ')
                capitalizeNext = true;
        }
    }

    auto newNodeLid = synth->getLogicalIdForNode(newNodeId);
    if (newNodeLid == 0)
    {
        juce::Logger::writeToLog("[InsertNode] ERROR: Failed to get logical ID for new node");
        return; // Don't disconnect if we can't get logical ID
    }

    pendingNodeScreenPositions[(int)newNodeLid] = position;

    // Always set passthrough type for reroute nodes based on source pin data type
    // (isMod flag is unreliable, so we use the actual pin data type instead)
    if (auto* reroute =
            dynamic_cast<RerouteModuleProcessor*>(synth->getModuleForLogical(newNodeLid)))
        reroute->setPassthroughType(srcType);

    // 2. Get Original Connection Points
    auto originalSrcNodeId = synth->getNodeIdForLogical(linkInfo.srcPin.logicalId);
    auto originalDstNodeId = (linkInfo.dstPin.logicalId == 0)
                                 ? synth->getOutputNodeID()
                                 : synth->getNodeIdForLogical(linkInfo.dstPin.logicalId);

    // 3. Disconnect the Original Link (only after node is confirmed created)
    bool disconnectSuccess = synth->disconnect(
        originalSrcNodeId, linkInfo.srcPin.channel, originalDstNodeId, linkInfo.dstPin.channel);
    if (!disconnectSuccess)
    {
        juce::Logger::writeToLog("[InsertNodeOnLink] WARNING: Failed to disconnect original link");
    }

    // 4. Configure newly inserted node if necessary (e.g., MapRange)
    int newNodeOutputChannel = 0;
    if (nodeType == "map_range")
    {
        if (auto* mapRange =
                dynamic_cast<MapRangeModuleProcessor*>(synth->getModuleForLogical(newNodeLid)))
        {
            Range inRange = getSourceRange(linkInfo.srcPin, synth);
            configureMapRangeFor(srcType, dstType, *mapRange, inRange);
            newNodeOutputChannel = (dstType == PinDataType::Audio) ? 1 : 0;
        }
    }

    // 5. Reconnect through the New Node
    bool connect1Success = synth->connect(originalSrcNodeId, linkInfo.srcPin.channel, newNodeId, 0);
    bool connect2Success =
        synth->connect(newNodeId, newNodeOutputChannel, originalDstNodeId, linkInfo.dstPin.channel);

    if (!connect1Success || !connect2Success)
    {
        juce::Logger::writeToLog(
            "[InsertNodeOnLink] ERROR: Failed to reconnect. connect1=" +
            juce::String(connect1Success ? "OK" : "FAIL") +
            " connect2=" + juce::String(connect2Success ? "OK" : "FAIL"));
    }
    else
    {
        juce::Logger::writeToLog("[InsertNodeOnLink] SUCCESS: Node inserted and reconnected");
    }

    if (getTypeForLogical(newNodeLid).equalsIgnoreCase("reroute"))
        updateRerouteTypeFromConnections((juce::uint32)newNodeLid);
}
void ImGuiNodeEditorComponent::insertNodeOnLinkStereo(
    const juce::String& nodeType,
    const LinkInfo&     linkLeft,
    const LinkInfo&     linkRight,
    const ImVec2&       position)
{
    if (synth == nullptr)
        return;

    juce::Logger::writeToLog("[InsertStereo] Inserting stereo node: " + nodeType);
    juce::Logger::writeToLog(
        "[InsertStereo] Left cable: " + juce::String(linkLeft.srcPin.logicalId) + " ch" +
        juce::String(linkLeft.srcPin.channel) + " -> " + juce::String(linkLeft.dstPin.logicalId) +
        " ch" + juce::String(linkLeft.dstPin.channel));
    juce::Logger::writeToLog(
        "[InsertStereo] Right cable: " + juce::String(linkRight.srcPin.logicalId) + " ch" +
        juce::String(linkRight.srcPin.channel) + " -> " + juce::String(linkRight.dstPin.logicalId) +
        " ch" + juce::String(linkRight.dstPin.channel));

    // 1. Create ONE node for both channels
    juce::AudioProcessorGraph::NodeID newNodeId;
    auto&                             app = PresetCreatorApplication::getApp();
    auto&                             knownPluginList = app.getKnownPluginList();
    bool                              isVst = false;

    // Get the VST folder at exe position for filtering
    juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
    juce::File vstFolder = exeDir.getChildFile("VST");

    for (const auto& desc : knownPluginList.getTypes())
    {
        if (desc.name == nodeType)
        {
            // Check if plugin is in the VST folder at exe position
            juce::File pluginFile(desc.fileOrIdentifier);
            if (pluginFile.existsAsFile())
            {
                juce::File pluginDir = pluginFile.getParentDirectory();
                if (pluginDir.isAChildOf(vstFolder) || pluginDir == vstFolder)
                {
                    newNodeId = synth->addVstModule(app.getPluginFormatManager(), desc);
                    if (newNodeId.uid == 0)
                    {
                        juce::Logger::writeToLog(
                            "[InsertNode] ERROR: Failed to create VST module: " + desc.name);
                        return; // Don't disconnect if node creation failed
                    }
                    isVst = true;
                    break;
                }
            }
        }
    }

    if (!isVst)
    {
        newNodeId = synth->addModule(nodeType);
        if (newNodeId.uid == 0)
        {
            juce::Logger::writeToLog("[InsertNode] ERROR: Failed to create module: " + nodeType);
            return; // Don't disconnect if node creation failed
        }
    }

    juce::String nodeName = isVst ? nodeType : juce::String(nodeType).replaceCharacter('_', ' ');
    if (!isVst)
    {
        nodeName = nodeName.toLowerCase();
        bool capitalizeNext = true;
        for (int i = 0; i < nodeName.length(); ++i)
        {
            if (capitalizeNext && juce::CharacterFunctions::isLetter(nodeName[i]))
            {
                nodeName = nodeName.substring(0, i) +
                           juce::String::charToString(nodeName[i]).toUpperCase() +
                           nodeName.substring(i + 1);
                capitalizeNext = false;
            }
            else if (nodeName[i] == ' ')
                capitalizeNext = true;
        }
    }

    auto newNodeLid = synth->getLogicalIdForNode(newNodeId);
    if (newNodeLid == 0)
    {
        juce::Logger::writeToLog(
            "[InsertNode] ERROR: Failed to get logical ID for new stereo node");
        return; // Don't disconnect if we can't get logical ID
    }

    pendingNodeScreenPositions[(int)newNodeLid] = position;

    // 2. Get Original Connection Points for LEFT cable (first cable)
    auto leftSrcNodeId = synth->getNodeIdForLogical(linkLeft.srcPin.logicalId);
    auto leftDstNodeId = (linkLeft.dstPin.logicalId == 0)
                             ? synth->getOutputNodeID()
                             : synth->getNodeIdForLogical(linkLeft.dstPin.logicalId);

    // 3. Get Original Connection Points for RIGHT cable (second cable)
    auto rightSrcNodeId = synth->getNodeIdForLogical(linkRight.srcPin.logicalId);
    auto rightDstNodeId = (linkRight.dstPin.logicalId == 0)
                              ? synth->getOutputNodeID()
                              : synth->getNodeIdForLogical(linkRight.dstPin.logicalId);

    // 4. Disconnect BOTH Original Links (only after node is confirmed created)
    synth->disconnect(
        leftSrcNodeId, linkLeft.srcPin.channel, leftDstNodeId, linkLeft.dstPin.channel);
    synth->disconnect(
        rightSrcNodeId, linkRight.srcPin.channel, rightDstNodeId, linkRight.dstPin.channel);

    // 5. Reconnect Through the New Node
    // Left cable -> new node's LEFT input (ch0)
    bool leftInConnected = synth->connect(leftSrcNodeId, linkLeft.srcPin.channel, newNodeId, 0);

    // Right cable -> new node's RIGHT input (ch1)
    bool rightInConnected = synth->connect(rightSrcNodeId, linkRight.srcPin.channel, newNodeId, 1);

    // New node's outputs -> original destinations
    // Note: We'll connect both outputs to their respective destinations
    bool leftOutConnected = synth->connect(newNodeId, 0, leftDstNodeId, linkLeft.dstPin.channel);
    bool rightOutConnected = synth->connect(newNodeId, 1, rightDstNodeId, linkRight.dstPin.channel);

    if (leftInConnected && rightInConnected && leftOutConnected && rightOutConnected)
    {
        juce::Logger::writeToLog(
            "[InsertStereo] Successfully inserted stereo node: both channels connected");
    }
    else
    {
        juce::Logger::writeToLog(
            "[InsertStereo] WARNING: Some connections failed - leftIn=" +
            juce::String(leftInConnected ? 1 : 0) +
            ", rightIn=" + juce::String(rightInConnected ? 1 : 0) +
            ", leftOut=" + juce::String(leftOutConnected ? 1 : 0) +
            ", rightOut=" + juce::String(rightOutConnected ? 1 : 0));
    }
}
void ImGuiNodeEditorComponent::insertNodeBetween(
    const juce::String& nodeType,
    const PinID&        srcPin,
    const PinID&        dstPin,
    bool                createUndoSnapshot)
{
    if (synth == nullptr)
        return;

    PinDataType srcType = getPinDataTypeForPin(srcPin);
    PinDataType dstType = getPinDataTypeForPin(dstPin);

    ImVec2 srcPos = ImNodes::GetNodeGridSpacePos(srcPin.logicalId);
    ImVec2 dstPos = ImNodes::GetNodeGridSpacePos(dstPin.logicalId == 0 ? 0 : dstPin.logicalId);
    ImVec2 newNodePos = ImVec2((srcPos.x + dstPos.x) * 0.5f, (srcPos.y + dstPos.y) * 0.5f);

    juce::AudioProcessorGraph::NodeID newNodeId;
    auto&                             app = PresetCreatorApplication::getApp();
    auto&                             knownPluginList = app.getKnownPluginList();
    bool                              isVst = false;

    // Get the VST folder at exe position for filtering
    juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
    juce::File vstFolder = exeDir.getChildFile("VST");

    for (const auto& desc : knownPluginList.getTypes())
    {
        if (desc.name == nodeType)
        {
            // Check if plugin is in the VST folder at exe position
            juce::File pluginFile(desc.fileOrIdentifier);
            if (pluginFile.existsAsFile())
            {
                juce::File pluginDir = pluginFile.getParentDirectory();
                if (pluginDir.isAChildOf(vstFolder) || pluginDir == vstFolder)
                {
                    newNodeId = synth->addVstModule(app.getPluginFormatManager(), desc);
                    if (newNodeId.uid == 0)
                    {
                        juce::Logger::writeToLog(
                            "[InsertNode] ERROR: Failed to create VST module: " + desc.name);
                        return; // Don't disconnect if node creation failed
                    }
                    isVst = true;
                    break;
                }
            }
        }
    }

    if (!isVst)
    {
        newNodeId = synth->addModule(nodeType);
        if (newNodeId.uid == 0)
        {
            juce::Logger::writeToLog("[InsertNode] ERROR: Failed to create module: " + nodeType);
            return; // Don't disconnect if node creation failed
        }
    }

    auto newNodeLid = synth->getLogicalIdForNode(newNodeId);
    if (newNodeLid == 0)
    {
        juce::Logger::writeToLog("[InsertNode] ERROR: Failed to get logical ID for new node");
        return; // Don't disconnect if we can't get logical ID
    }

    pendingNodePositions[(int)newNodeLid] = newNodePos;

    // Always set passthrough type for reroute nodes based on source pin data type
    // (isMod flag is unreliable, so we use the actual pin data type instead)
    if (auto* reroute =
            dynamic_cast<RerouteModuleProcessor*>(synth->getModuleForLogical(newNodeLid)))
        reroute->setPassthroughType(srcType);

    auto originalSrcNodeId = synth->getNodeIdForLogical(srcPin.logicalId);
    auto originalDstNodeId = (dstPin.logicalId == 0) ? synth->getOutputNodeID()
                                                     : synth->getNodeIdForLogical(dstPin.logicalId);

    int newNodeOutputChannel = 0;
    if (nodeType == "map_range")
    {
        if (auto* mapRange =
                dynamic_cast<MapRangeModuleProcessor*>(synth->getModuleForLogical(newNodeLid)))
        {
            Range inRange = getSourceRange(srcPin, synth);
            configureMapRangeFor(srcType, dstType, *mapRange, inRange);
            newNodeOutputChannel = (dstType == PinDataType::Audio) ? 1 : 0;
        }
    }

    synth->connect(originalSrcNodeId, srcPin.channel, newNodeId, 0);
    synth->connect(newNodeId, newNodeOutputChannel, originalDstNodeId, dstPin.channel);

    if (getTypeForLogical(newNodeLid).equalsIgnoreCase("reroute"))
        updateRerouteTypeFromConnections((juce::uint32)newNodeLid);

    juce::Logger::writeToLog(
        "[AutoConvert] Inserted '" + nodeType + "' between " + juce::String(srcPin.logicalId) +
        " and " + juce::String(dstPin.logicalId));

    if (createUndoSnapshot)
    {
        pushSnapshot();
        graphNeedsRebuild = true;
    }
}

void ImGuiNodeEditorComponent::insertNodeAfterSelection(const juce::String& nodeType)
{
    if (synth == nullptr || selectedLogicalId == 0)
        return;

    const juce::uint32 sourceLid = (juce::uint32)selectedLogicalId;
    auto               sourceNodeId = synth->getNodeIdForLogical(sourceLid);
    if (sourceNodeId == juce::AudioProcessorGraph::NodeID{})
        return;

    auto                                               connections = synth->getConnectionsInfo();
    std::vector<ModularSynthProcessor::ConnectionInfo> outgoing;
    outgoing.reserve(connections.size());
    for (const auto& c : connections)
    {
        if (c.srcLogicalId == sourceLid)
            outgoing.push_back(c);
    }

    if (outgoing.empty())
    {
        NotificationManager::post(
            NotificationManager::Type::Info,
            "Selected node has no outgoing connections to intercept.");
        return;
    }

    auto newNodeId = synth->addModule(nodeType);
    if (newNodeId == juce::AudioProcessorGraph::NodeID{})
    {
        juce::Logger::writeToLog("[InsertNodeAfterSelection] Failed to create module: " + nodeType);
        return;
    }

    auto newLogicalId = synth->getLogicalIdForNode(newNodeId);
    if (newLogicalId == 0)
    {
        juce::Logger::writeToLog(
            "[InsertNodeAfterSelection] Failed to get logical ID for new node");
        return;
    }

    ImVec2 srcPos = ImNodes::GetNodeGridSpacePos(selectedLogicalId);
    pendingNodePositions[(int)newLogicalId] = ImVec2(srcPos.x + 160.0f, srcPos.y);

    auto          outputNodeId = synth->getOutputNodeID();
    std::set<int> connectedInputChannels;

    for (const auto& conn : outgoing)
    {
        auto dstNodeId =
            conn.dstLogicalId == 0 ? outputNodeId : synth->getNodeIdForLogical(conn.dstLogicalId);
        if (dstNodeId.uid == 0)
            continue;

        synth->disconnect(sourceNodeId, conn.srcChan, dstNodeId, conn.dstChan);

        if (connectedInputChannels.insert(conn.srcChan).second)
        {
            if (!synth->connect(sourceNodeId, conn.srcChan, newNodeId, conn.srcChan))
            {
                juce::Logger::writeToLog(
                    "[InsertNodeAfterSelection] Failed to connect source ch " +
                    juce::String(conn.srcChan) + " to new node.");
            }
        }

        if (!synth->connect(newNodeId, conn.srcChan, dstNodeId, conn.dstChan))
        {
            juce::Logger::writeToLog(
                "[InsertNodeAfterSelection] Failed to connect new node output ch " +
                juce::String(conn.srcChan) + " to logical " + juce::String((int)conn.dstLogicalId) +
                " ch " + juce::String(conn.dstChan));
        }
    }

    if (getTypeForLogical(newLogicalId).equalsIgnoreCase("reroute"))
        updateRerouteTypeFromConnections(newLogicalId);

    selectedLogicalId = (int)newLogicalId;
    graphNeedsRebuild = true;
    pushSnapshot();
}

void ImGuiNodeEditorComponent::insertNodeBetween(const juce::String& nodeType)
{
    if (linkToInsertOn.linkId != -1)
    {
        insertNodeOnLink(nodeType, linkToInsertOn, ImGui::GetMousePos());
        graphNeedsRebuild = true;
        pushSnapshot();
        linkToInsertOn.linkId = -1;
    }
}
void ImGuiNodeEditorComponent::handleInsertNodeOnSelectedLinks(const juce::String& nodeType)
{
    if (synth == nullptr || ImNodes::NumSelectedLinks() == 0)
        return;

    pushSnapshot();

    const int        numSelectedLinks = ImNodes::NumSelectedLinks();
    std::vector<int> selectedLinkIds(numSelectedLinks);
    ImNodes::GetSelectedLinks(selectedLinkIds.data());

    ImVec2 basePosition = ImGui::GetMousePos();
    float  xOffset = 0.0f;

    if (numSelectedLinks == 2)
    {
        auto it0 = linkIdToAttrs.find(selectedLinkIds[0]);
        auto it1 = linkIdToAttrs.find(selectedLinkIds[1]);

        if (it0 != linkIdToAttrs.end() && it1 != linkIdToAttrs.end())
        {
            LinkInfo firstLink;
            firstLink.linkId = selectedLinkIds[0];
            firstLink.srcPin = decodePinId(it0->second.first);
            firstLink.dstPin = decodePinId(it0->second.second);
            firstLink.isMod = firstLink.srcPin.isMod || firstLink.dstPin.isMod;

            LinkInfo secondLink;
            secondLink.linkId = selectedLinkIds[1];
            secondLink.srcPin = decodePinId(it1->second.first);
            secondLink.dstPin = decodePinId(it1->second.second);
            secondLink.isMod = secondLink.srcPin.isMod || secondLink.dstPin.isMod;

            if (!firstLink.isMod && !secondLink.isMod)
            {
                auto isStereoCandidate = [&]() -> bool {
                    // Both links must be from the same source node
                    if (firstLink.srcPin.logicalId != secondLink.srcPin.logicalId)
                    {
                        juce::Logger::writeToLog("[InsertNode] Not stereo: different source nodes");
                        return false;
                    }

                    // Both links must go to the same destination (or both to main output)
                    const bool bothToMainOutput =
                        (firstLink.dstPin.logicalId == 0 && secondLink.dstPin.logicalId == 0);
                    if (!bothToMainOutput &&
                        firstLink.dstPin.logicalId != secondLink.dstPin.logicalId)
                    {
                        juce::Logger::writeToLog(
                            "[InsertNode] Not stereo: different destination nodes");
                        return false;
                    }

                    // Source channels should be consecutive (0-1, 1-2, etc.) for stereo
                    const int srcDelta =
                        std::abs(firstLink.srcPin.channel - secondLink.srcPin.channel);
                    const int dstDelta =
                        std::abs(firstLink.dstPin.channel - secondLink.dstPin.channel);

                    // For stereo, we expect channels 0 and 1, but allow other consecutive pairs
                    if (srcDelta != 1)
                    {
                        juce::Logger::writeToLog(
                            "[InsertNode] Not stereo: source channels not consecutive (delta=" +
                            juce::String(srcDelta) + ")");
                        return false;
                    }

                    // Destination channels should also be consecutive
                    if (dstDelta != 1)
                    {
                        juce::Logger::writeToLog(
                            "[InsertNode] Not stereo: destination channels not consecutive "
                            "(delta=" +
                            juce::String(dstDelta) + ")");
                        return false;
                    }

                    // All pins must be audio type
                    const auto srcTypeA = getPinDataTypeForPin(firstLink.srcPin);
                    const auto srcTypeB = getPinDataTypeForPin(secondLink.srcPin);
                    const auto dstTypeA = getPinDataTypeForPin(firstLink.dstPin);
                    const auto dstTypeB = getPinDataTypeForPin(secondLink.dstPin);
                    const bool allAudio =
                        srcTypeA == PinDataType::Audio && srcTypeB == PinDataType::Audio &&
                        dstTypeA == PinDataType::Audio && dstTypeB == PinDataType::Audio;

                    if (!allAudio)
                    {
                        juce::Logger::writeToLog("[InsertNode] Not stereo: not all audio pins");
                        return false;
                    }

                    juce::Logger::writeToLog(
                        "[InsertNode] Detected stereo pair: ch" +
                        juce::String(firstLink.srcPin.channel) + " and ch" +
                        juce::String(secondLink.srcPin.channel));
                    return true;
                };

                if (isStereoCandidate())
                {
                    LinkInfo leftLink = firstLink;
                    LinkInfo rightLink = secondLink;

                    // Ensure left link has the lower channel number
                    if (rightLink.srcPin.channel < leftLink.srcPin.channel)
                        std::swap(leftLink, rightLink);

                    juce::Logger::writeToLog(
                        "[InsertNode] Inserting STEREO node: left=ch" +
                        juce::String(leftLink.srcPin.channel) + ", right=ch" +
                        juce::String(rightLink.srcPin.channel));
                    insertNodeOnLinkStereo(nodeType, leftLink, rightLink, basePosition);
                    juce::Logger::writeToLog(
                        "[InsertNode] Successfully inserted STEREO node for 2 selected audio "
                        "cables");
                    graphNeedsRebuild = true;
                    return;
                }
            }
        }
    }

    std::set<int> processedLinks;

    for (int linkId : selectedLinkIds)
    {
        if (processedLinks.count(linkId) != 0)
            continue;

        auto it = linkIdToAttrs.find(linkId);
        if (it == linkIdToAttrs.end())
            continue;

        LinkInfo link;
        link.linkId = linkId;
        link.srcPin = decodePinId(it->second.first);
        link.dstPin = decodePinId(it->second.second);
        link.isMod = link.srcPin.isMod || link.dstPin.isMod;

        ImVec2 newPosition(basePosition.x + xOffset, basePosition.y);
        insertNodeOnLink(nodeType, link, newPosition);
        processedLinks.insert(linkId);
        juce::Logger::writeToLog(
            "[InsertNode] Inserted MONO node for link " + juce::String(linkId));

        xOffset += 40.0f;
    }

    graphNeedsRebuild = true;
}

void ImGuiNodeEditorComponent::expandMetaModule(juce::uint32 metaLogicalId)
{
    if (!synth)
        return;

    const auto metaNodeId = synth->getNodeIdForLogical(metaLogicalId);
    if (metaNodeId.uid == 0)
        return;

    auto* metaModule =
        dynamic_cast<MetaModuleProcessor*>(synth->getModuleForLogical(metaLogicalId));
    if (metaModule == nullptr)
        return;

    const auto         metaState = metaModule->getExtraStateTree();
    const juce::String encoded = metaState.getProperty("internalGraphState").toString();
    if (encoded.isEmpty())
    {
        NotificationManager::post(
            NotificationManager::Type::Warning, "Meta module has no internal patch to expand.");
        return;
    }

    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, encoded))
    {
        NotificationManager::post(
            NotificationManager::Type::Warning, "Failed to decode meta module state.");
        return;
    }

    std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(decoded.toString()));
    if (xml == nullptr)
        return;

    juce::ValueTree internalRoot = juce::ValueTree::fromXml(*xml);
    auto            modulesVT = internalRoot.getChildWithName("modules");
    auto            connsVT = internalRoot.getChildWithName("connections");
    if (!modulesVT.isValid() || !connsVT.isValid())
        return;

    pushSnapshot();

    struct CollapsedInlet
    {
        int          oldId{};
        int          pinIndex{};
        int          channelCount{1};
        juce::uint32 externalLogicalId{};
        int          externalChannel{};
        int          metaChannel{};
    };
    struct CollapsedOutlet
    {
        int          oldId{};
        int          pinIndex{};
        int          channelCount{1};
        juce::uint32 externalLogicalId{};
        int          externalChannel{};
        bool         externalIsOutput{};
        int          metaChannel{};
    };
    struct InternalConnection
    {
        int srcId;
        int srcChan;
        int dstId;
        int dstChan;
    };
    struct InboundConnection
    {
        int inletOldId;
        int dstId;
        int dstChan;
    };
    struct OutboundConnection
    {
        int srcId;
        int srcChan;
        int outletOldId;
    };

    std::vector<CollapsedInlet>     collapsedInlets;
    std::vector<CollapsedOutlet>    collapsedOutlets;
    std::vector<InternalConnection> internalConnections;
    std::vector<InboundConnection>  inboundConnections;
    std::vector<OutboundConnection> outboundConnections;
    std::map<int, juce::uint32>     oldToNew;
    std::vector<juce::uint32>       createdLogicalIds;

    auto readChannelCount = [](const juce::ValueTree&  moduleVT,
                               const juce::Identifier& paramId) -> int {
        if (auto paramsWrapper = moduleVT.getChildWithName("params"); paramsWrapper.isValid())
        {
            if (paramsWrapper.getNumChildren() > 0)
            {
                auto params = paramsWrapper.getChild(0);
                for (int i = 0; i < params.getNumChildren(); ++i)
                {
                    auto paramNode = params.getChild(i);
                    if (paramNode.getProperty("id").toString().equalsIgnoreCase(paramId.toString()))
                        return (int)paramNode.getProperty("value", 1.0);
                }
            }
        }
        return 1;
    };

    std::unordered_set<int> inletIds;
    std::unordered_set<int> outletIds;

    for (int i = 0; i < modulesVT.getNumChildren(); ++i)
    {
        auto moduleVT = modulesVT.getChild(i);
        if (!moduleVT.hasType("module"))
            continue;

        const int          oldId = (int)moduleVT.getProperty("logicalId", 0);
        const juce::String type = moduleVT.getProperty("type").toString();

        auto extraWrapper = moduleVT.getChildWithName("extra");
        auto extraState = (extraWrapper.isValid() && extraWrapper.getNumChildren() > 0)
                              ? extraWrapper.getChild(0)
                              : juce::ValueTree();

        if (type.equalsIgnoreCase("inlet"))
        {
            CollapsedInlet inlet;
            inlet.oldId = oldId;
            inlet.pinIndex = (int)extraState.getProperty("pinIndex", (int)collapsedInlets.size());
            inlet.channelCount =
                readChannelCount(moduleVT, InletModuleProcessor::paramIdChannelCount);
            inlet.externalLogicalId =
                (juce::uint32)(int)extraState.getProperty("externalLogicalId", 0);
            inlet.externalChannel = (int)extraState.getProperty("externalChannel", 0);
            collapsedInlets.push_back(inlet);
            inletIds.insert(oldId);
            continue;
        }

        if (type.equalsIgnoreCase("outlet"))
        {
            CollapsedOutlet outlet;
            outlet.oldId = oldId;
            outlet.pinIndex = (int)extraState.getProperty("pinIndex", (int)collapsedOutlets.size());
            outlet.channelCount =
                readChannelCount(moduleVT, OutletModuleProcessor::paramIdChannelCount);
            outlet.externalLogicalId =
                (juce::uint32)(int)extraState.getProperty("externalLogicalId", 0);
            outlet.externalChannel = (int)extraState.getProperty("externalChannel", 0);
            outlet.externalIsOutput = (bool)(int)extraState.getProperty(
                "externalIsOutput", outlet.externalLogicalId == 0 ? 1 : 0);
            collapsedOutlets.push_back(outlet);
            outletIds.insert(oldId);
            continue;
        }

        const auto         nodeId = synth->addModule(type);
        const juce::uint32 newLogical = synth->getLogicalIdForNode(nodeId);
        oldToNew[oldId] = newLogical;
        createdLogicalIds.push_back(newLogical);

        if (auto* module = synth->getModuleForLogical(newLogical))
        {
            if (auto paramsWrapper = moduleVT.getChildWithName("params"); paramsWrapper.isValid())
                if (paramsWrapper.getNumChildren() > 0)
                    module->getAPVTS().replaceState(paramsWrapper.getChild(0));
            if (extraState.isValid())
                module->setExtraStateTree(extraState);
        }
    }

    for (int i = 0; i < connsVT.getNumChildren(); ++i)
    {
        auto cv = connsVT.getChild(i);
        if (!cv.hasType("connection"))
            continue;

        const int srcId = (int)cv.getProperty("srcId", 0);
        const int dstId = (int)cv.getProperty("dstId", 0);
        const int srcChan = (int)cv.getProperty("srcChan", 0);
        const int dstChan = (int)cv.getProperty("dstChan", 0);

        const bool srcIsInlet = inletIds.count(srcId) > 0;
        const bool dstIsOutlet = outletIds.count(dstId) > 0;

        if (srcIsInlet && !dstIsOutlet)
            inboundConnections.push_back({srcId, dstId, dstChan});
        else if (!srcIsInlet && dstIsOutlet)
            outboundConnections.push_back({srcId, srcChan, dstId});
        else if (!srcIsInlet && !dstIsOutlet)
            internalConnections.push_back({srcId, srcChan, dstId, dstChan});
    }

    std::sort(
        collapsedInlets.begin(),
        collapsedInlets.end(),
        [](const CollapsedInlet& a, const CollapsedInlet& b) {
            if (a.pinIndex != b.pinIndex)
                return a.pinIndex < b.pinIndex;
            return a.oldId < b.oldId;
        });

    int runningChannel = 0;
    for (auto& inlet : collapsedInlets)
    {
        inlet.metaChannel = runningChannel;
        runningChannel += inlet.channelCount;
    }

    std::sort(
        collapsedOutlets.begin(),
        collapsedOutlets.end(),
        [](const CollapsedOutlet& a, const CollapsedOutlet& b) {
            if (a.pinIndex != b.pinIndex)
                return a.pinIndex < b.pinIndex;
            return a.oldId < b.oldId;
        });

    runningChannel = 0;
    for (auto& outlet : collapsedOutlets)
    {
        outlet.metaChannel = runningChannel;
        runningChannel += outlet.channelCount;
    }

    std::unordered_map<int, std::pair<juce::uint32, int>>        metaInputs;
    std::unordered_map<int, std::tuple<juce::uint32, int, bool>> metaOutputs;

    for (const auto& c : synth->getConnectionsInfo())
    {
        if (c.dstLogicalId == metaLogicalId && !c.dstIsOutput)
            metaInputs.emplace(c.dstChan, std::make_pair(c.srcLogicalId, c.srcChan));
        if (c.srcLogicalId == metaLogicalId)
            metaOutputs.emplace(
                c.srcChan, std::make_tuple(c.dstLogicalId, c.dstChan, c.dstIsOutput));
    }

    for (auto& inlet : collapsedInlets)
    {
        if (inlet.externalLogicalId == 0 && metaInputs.count(inlet.metaChannel) > 0)
        {
            auto external = metaInputs[inlet.metaChannel];
            inlet.externalLogicalId = external.first;
            inlet.externalChannel = external.second;
        }
    }

    for (auto& outlet : collapsedOutlets)
    {
        if (metaOutputs.count(outlet.metaChannel) > 0)
        {
            auto external = metaOutputs[outlet.metaChannel];
            if ((outlet.externalLogicalId == 0 || std::get<0>(external) != 0))
                outlet.externalLogicalId = std::get<0>(external);
            outlet.externalChannel = std::get<1>(external);
            outlet.externalIsOutput = std::get<2>(external) || outlet.externalLogicalId == 0;
        }
    }

    std::unordered_map<int, CollapsedInlet> inletLookup;
    for (const auto& inlet : collapsedInlets)
        inletLookup.emplace(inlet.oldId, inlet);

    std::unordered_map<int, CollapsedOutlet> outletLookup;
    for (const auto& outlet : collapsedOutlets)
        outletLookup.emplace(outlet.oldId, outlet);

    for (const auto& conn : internalConnections)
    {
        auto srcIt = oldToNew.find(conn.srcId);
        auto dstIt = oldToNew.find(conn.dstId);
        if (srcIt == oldToNew.end() || dstIt == oldToNew.end())
            continue;

        auto srcNode = synth->getNodeIdForLogical(srcIt->second);
        auto dstNode = synth->getNodeIdForLogical(dstIt->second);
        if (srcNode.uid == 0 || dstNode.uid == 0)
            continue;

        synth->connect(srcNode, conn.srcChan, dstNode, conn.dstChan);
    }

    for (const auto& inbound : inboundConnections)
    {
        auto inletIt = inletLookup.find(inbound.inletOldId);
        auto dstIt = oldToNew.find(inbound.dstId);
        if (inletIt == inletLookup.end() || dstIt == oldToNew.end())
            continue;

        const auto& inlet = inletIt->second;
        if (inlet.externalLogicalId == 0)
            continue;

        auto srcNode = synth->getNodeIdForLogical(inlet.externalLogicalId);
        auto dstNode = synth->getNodeIdForLogical(dstIt->second);
        if (srcNode.uid == 0 || dstNode.uid == 0)
            continue;

        synth->connect(srcNode, inlet.externalChannel, dstNode, inbound.dstChan);
    }

    for (const auto& outbound : outboundConnections)
    {
        auto outletIt = outletLookup.find(outbound.outletOldId);
        auto srcIt = oldToNew.find(outbound.srcId);
        if (outletIt == outletLookup.end() || srcIt == oldToNew.end())
            continue;

        const auto& outlet = outletIt->second;
        auto        srcNode = synth->getNodeIdForLogical(srcIt->second);
        if (srcNode.uid == 0)
            continue;

        juce::AudioProcessorGraph::NodeID dstNode;
        if (outlet.externalIsOutput || outlet.externalLogicalId == 0)
            dstNode = synth->getOutputNodeID();
        else
            dstNode = synth->getNodeIdForLogical(outlet.externalLogicalId);

        if (dstNode.uid == 0)
            continue;

        synth->connect(srcNode, outbound.srcChan, dstNode, outlet.externalChannel);
    }

    const ImVec2 metaPos = ImNodes::GetNodeGridSpacePos((int)metaLogicalId);
    synth->removeModule(metaNodeId);

    const float spacing = 160.0f;
    for (std::size_t idx = 0; idx < createdLogicalIds.size(); ++idx)
    {
        const auto lid = createdLogicalIds[idx];
        const int  ix = (int)(idx % 4);
        const int  iy = (int)(idx / 4);
        pendingNodePositions[(int)lid] = ImVec2(metaPos.x + ix * spacing, metaPos.y + iy * spacing);
    }

    selectedLogicalId = 0;
    graphNeedsRebuild = true;
    synth->commitChanges();

    NotificationManager::post(NotificationManager::Type::Info, "Expanded Meta Module");
}

juce::File ImGuiNodeEditorComponent::findPresetsDirectory()
{
    // Try to get executable directory with error handling to prevent blocking
    // Use a quick check to avoid slow filesystem operations
    juce::File   exeDir;
    juce::String exePath;

    try
    {
        auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        exePath = exeFile.getFullPathName();

        // Quick validation: check if path is non-empty before doing expensive operations
        if (exePath.isNotEmpty())
        {
            exeDir = exeFile.getParentDirectory();
            // Quick check: if parent directory path is empty or clearly invalid, skip it
            if (exeDir.getFullPathName().isEmpty())
            {
                exeDir = juce::File();
            }
        }
    }
    catch (...)
    {
        // If executable path resolution fails, fall back to user documents
        juce::Logger::writeToLog(
            "[PresetLoader] Failed to resolve executable path, using fallback");
        exeDir = juce::File();
    }

    // If we successfully got the exe directory, try exe/presets/ first
    // Only do filesystem checks if we have a valid path
    if (exeDir.getFullPathName().isNotEmpty())
    {
        auto presetsDir = exeDir.getChildFile("presets");
        // Quick check: only call exists() if path looks valid
        if (presetsDir.getFullPathName().isNotEmpty())
        {
            try
            {
                if (presetsDir.exists() && presetsDir.isDirectory())
                {
                    return presetsDir;
                }
                // Create exe/presets/ if it doesn't exist (but don't block if it fails)
                if (presetsDir.createDirectory())
                {
                    return presetsDir;
                }
            }
            catch (...)
            {
                // If filesystem operations fail, continue to fallback
                juce::Logger::writeToLog(
                    "[PresetLoader] Filesystem check failed for exe/presets, using fallback");
            }
        }

        // Fallback: Search upwards from the executable's location for a sibling directory
        // named "Synth_presets". This is robust to different build configurations.
        // Limit iterations and add error handling to prevent blocking
        juce::File dir = exeDir;
        for (int i = 0; i < 8; ++i) // Limit search depth to 8 levels
        {
            try
            {
                dir = dir.getParentDirectory();
                auto dirPath = dir.getFullPathName();
                if (dirPath.isEmpty() || !dir.exists())
                    break;

                juce::File candidate = dir.getSiblingFile("Synth_presets");
                if (candidate.getFullPathName().isNotEmpty() && candidate.isDirectory())
                {
                    return candidate;
                }
            }
            catch (...)
            {
                // If filesystem operations fail during search, break out
                break;
            }
        }

        // If exe directory is valid, return it as final fallback
        if (exeDir.getFullPathName().isNotEmpty())
        {
            return exeDir;
        }
    }

    // Ultimate fallback: Use user documents directory if executable path resolution failed
    try
    {
        auto userDocs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        if (userDocs.getFullPathName().isNotEmpty())
        {
            auto presetsDir = userDocs.getChildFile("Presets");
            if (presetsDir.getFullPathName().isNotEmpty())
            {
                try
                {
                    if (presetsDir.exists() && presetsDir.isDirectory())
                    {
                        return presetsDir;
                    }
                    if (presetsDir.createDirectory())
                    {
                        return presetsDir;
                    }
                }
                catch (...)
                {
                    // Continue to return userDocs as fallback
                }
            }
            return userDocs; // Last resort: return user documents directory
        }
    }
    catch (...)
    {
        // If everything fails, return an empty file (FileChooser will use system default)
        juce::Logger::writeToLog("[PresetLoader] All fallbacks failed, using system default");
    }

    // Return empty file - FileChooser will handle this gracefully by using system default location
    return juce::File();
}
// Helper function implementations
PinDataType ImGuiNodeEditorComponent::getPinDataTypeForPin(const PinID& pin)
{
    if (synth == nullptr)
        return PinDataType::Raw;

    // Handle the main output node as a special case
    if (pin.logicalId == 0)
    {
        return PinDataType::Audio;
    }

    juce::String moduleType = getTypeForLogical(pin.logicalId);
    if (moduleType.isEmpty())
        return PinDataType::Raw;

    // *** NEW: Check dynamic pins FIRST ***
    if (auto* module = synth->getModuleForLogical(pin.logicalId))
    {
        // Check dynamic input pins
        if (pin.isInput && !pin.isMod)
        {
            auto dynamicInputs = module->getDynamicInputPins();
            for (const auto& dynPin : dynamicInputs)
            {
                if (dynPin.channel == pin.channel)
                {
                    return dynPin.type;
                }
            }
        }
        // Check dynamic output pins
        else if (!pin.isInput && !pin.isMod)
        {
            auto dynamicOutputs = module->getDynamicOutputPins();
            for (const auto& dynPin : dynamicOutputs)
            {
                if (dynPin.channel == pin.channel)
                {
                    return dynPin.type;
                }
            }
        }
    }
    // *** END NEW CODE ***

    auto it = getModulePinDatabase().find(moduleType);
    if (it == getModulePinDatabase().end())
    {
        // Fallback: case-insensitive lookup (module registry may use different casing)
        juce::String moduleTypeLower = moduleType.toLowerCase();
        for (const auto& kv : getModulePinDatabase())
        {
            if (kv.first.compareIgnoreCase(moduleType) == 0 ||
                kv.first.toLowerCase() == moduleTypeLower)
            {
                it = getModulePinDatabase().find(kv.first);
                break;
            }
        }
        if (it == getModulePinDatabase().end())
        {
            // If the module type is not in our static database, it's likely a VST plugin.
            // A safe assumption is that its pins are for audio.
            if (auto* module = synth->getModuleForLogical(pin.logicalId))
            {
                if (dynamic_cast<VstHostModuleProcessor*>(module))
                {
                    return PinDataType::Audio; // Green for VST pins
                }
            }
            return PinDataType::Raw;
        }
    }

    const auto& pinInfo = it->second;

    if (pin.isMod)
    {
        for (const auto& modPin : pinInfo.modIns)
        {
            if (modPin.paramId == pin.paramId)
            {
                return modPin.type;
            }
        }
    }
    else // It's an audio pin
    {
        const auto& pins = pin.isInput ? pinInfo.audioIns : pinInfo.audioOuts;
        for (const auto& audioPin : pins)
        {
            if (audioPin.channel == pin.channel)
            {
                return audioPin.type;
            }
        }
    }
    return PinDataType::Raw; // Fallback
}

unsigned int ImGuiNodeEditorComponent::getImU32ForType(PinDataType type)
{
    const ImU32 themedColor = ThemeManager::getInstance().getPinColor(type);
    if (themedColor != 0)
        return themedColor;

    switch (type)
    {
    case PinDataType::CV:
        return IM_COL32(100, 150, 255, 255); // Blue
    case PinDataType::Audio:
        return IM_COL32(100, 255, 150, 255); // Green
    case PinDataType::Gate:
        return IM_COL32(255, 220, 100, 255); // Yellow
    case PinDataType::Raw:
        return IM_COL32(255, 100, 100, 255); // Red
    case PinDataType::Video:
        return IM_COL32(0, 200, 255, 255); // Cyan
    default:
        return IM_COL32(150, 150, 150, 255); // Grey
    }
}

const char* ImGuiNodeEditorComponent::pinDataTypeToString(PinDataType type)
{
    switch (type)
    {
    case PinDataType::CV:
        return "CV (0 to 1)";
    case PinDataType::Audio:
        return "Audio (-1 to 1)";
    case PinDataType::Gate:
        return "Gate/Trigger";
    case PinDataType::Raw:
        return "Raw";
    case PinDataType::Video:
        return "Video Source";
    default:
        return "Unknown";
    }
}
std::vector<AudioPin> ImGuiNodeEditorComponent::getPinsOfType(
    juce::uint32 logicalId,
    bool         isInput,
    PinDataType  targetType)
{
    std::vector<AudioPin> matchingPins;
    juce::String          moduleType = getTypeForLogical(logicalId);

    if (moduleType.isEmpty())
    {
        return matchingPins;
    }

    // *** PRIORITIZE DYNAMIC PINS OVER STATIC PINS ***
    // Dynamic pins are more accurate and up-to-date for modules that provide them
    if (auto* module = synth->getModuleForLogical(logicalId))
    {
        // --- DYNAMIC PATH FOR MODULES WITH getDynamicInputPins/getDynamicOutputPins ---
        auto dynamicPins = isInput ? module->getDynamicInputPins() : module->getDynamicOutputPins();

        if (!dynamicPins.empty())
        {
            // Module provides dynamic pins - filter by type
            for (const auto& pin : dynamicPins)
            {
                if (pin.type == targetType)
                {
                    matchingPins.emplace_back(pin.name, pin.channel, pin.type);
                }
            }
        }
        else if (auto* vst = dynamic_cast<VstHostModuleProcessor*>(module))
        {
            // For VSTs without dynamic pins, assume all pins are 'Audio' type for chaining.
            if (targetType == PinDataType::Audio)
            {
                const int numChannels =
                    isInput ? vst->getTotalNumInputChannels() : vst->getTotalNumOutputChannels();
                for (int i = 0; i < numChannels; ++i)
                {
                    juce::String pinName =
                        isInput ? vst->getAudioInputLabel(i) : vst->getAudioOutputLabel(i);
                    if (pinName.isNotEmpty())
                    {
                        matchingPins.emplace_back(pinName, i, PinDataType::Audio);
                    }
                }
            }
        }
    }

    // If no dynamic pins matched, fall back to static pins from the database
    if (matchingPins.empty())
    {
        auto it = getModulePinDatabase().find(moduleType);

        // --- CASE-INSENSITIVE LOOKUP ---
        if (it == getModulePinDatabase().end())
        {
            for (const auto& kv : getModulePinDatabase())
            {
                if (kv.first.compareIgnoreCase(moduleType) == 0)
                {
                    it = getModulePinDatabase().find(kv.first);
                    break;
                }
            }
        }

        if (it != getModulePinDatabase().end())
        {
            // --- Standard path for built-in modules ---
            const auto& pins = isInput ? it->second.audioIns : it->second.audioOuts;
            for (const auto& pin : pins)
            {
                if (pin.type == targetType)
                {
                    matchingPins.push_back(pin);
                }
            }
        }
    }

    return matchingPins;
}

bool ImGuiNodeEditorComponent::isInputConnected(juce::uint32 destLogicalId, int destChannel) const
{
    if (synth == nullptr)
        return false;

    // Get all current connections
    const auto connections = synth->getConnectionsInfo();

    // Check if any connection targets this input
    for (const auto& conn : connections)
    {
        // Check if this connection targets our destination input
        // dstIsOutput == false means it's an input (not the main output node)
        if (!conn.dstIsOutput && conn.dstLogicalId == destLogicalId && conn.dstChan == destChannel)
        {
            return true; // This input is already connected
        }
    }

    return false; // Input is available
}

void ImGuiNodeEditorComponent::handleNodeChaining()
{
    if (synth == nullptr)
        return;

    const int numSelected = ImNodes::NumSelectedNodes();
    if (numSelected <= 1)
        return;

    std::vector<int> selectedNodeIds(numSelected);
    ImNodes::GetSelectedNodes(selectedNodeIds.data());

    std::vector<std::pair<float, int>> sortedNodes;
    sortedNodes.reserve(selectedNodeIds.size());

    for (int nodeId : selectedNodeIds)
    {
        if (nodeId == 0)
            continue;
        ImVec2 pos = ImNodes::GetNodeGridSpacePos(nodeId);
        sortedNodes.emplace_back(pos.x, nodeId);
    }

    if (sortedNodes.size() <= 1)
        return;

    std::sort(sortedNodes.begin(), sortedNodes.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    pushSnapshot();

    for (size_t i = 0; i + 1 < sortedNodes.size(); ++i)
    {
        juce::uint32 sourceLid = sortedNodes[i].second;
        juce::uint32 destLid = sortedNodes[i + 1].second;

        auto sourceNodeId = synth->getNodeIdForLogical(sourceLid);
        auto destNodeId = synth->getNodeIdForLogical(destLid);

        if (sourceNodeId.uid == 0 || destNodeId.uid == 0)
            continue;

        synth->connect(sourceNodeId, 0, destNodeId, 0);
        synth->connect(sourceNodeId, 1, destNodeId, 1);

        if (auto* destModule = synth->getModuleForLogical(destLid))
        {
            if (auto* recorder = dynamic_cast<RecordModuleProcessor*>(destModule))
            {
                if (auto* sourceModule = synth->getModuleForLogical(sourceLid))
                    recorder->updateSuggestedFilename(sourceModule->getName());
            }
        }
    }

    graphNeedsRebuild = true;
}

void ImGuiNodeEditorComponent::handleRecordOutput()
{
    if (!synth)
        return;

    pushSnapshot();
    juce::Logger::writeToLog("[Record Output] Initiated.");

    // 1. Find connections going to the main output node.
    std::vector<ModularSynthProcessor::ConnectionInfo> outputFeeds;
    for (const auto& c : synth->getConnectionsInfo())
    {
        if (c.dstIsOutput)
        {
            outputFeeds.push_back(c);
        }
    }

    if (outputFeeds.empty())
    {
        juce::Logger::writeToLog("[Record Output] No connections to main output found.");
        return;
    }

    // 2. Create and position the recorder.
    auto   recorderNodeId = synth->addModule("recorder");
    auto   recorderLid = synth->getLogicalIdForNode(recorderNodeId);
    ImVec2 outPos = ImNodes::GetNodeGridSpacePos(0);
    pendingNodePositions[(int)recorderLid] = ImVec2(outPos.x - 400.0f, outPos.y);

    auto* recorder = dynamic_cast<RecordModuleProcessor*>(synth->getModuleForLogical(recorderLid));
    if (recorder)
    {
        recorder->setPropertiesFile(PresetCreatorApplication::getApp().getProperties());
    }

    // 3. "Tap" the signals by connecting the original sources to the recorder.
    juce::String sourceName;
    for (const auto& feed : outputFeeds)
    {
        auto srcNodeId = synth->getNodeIdForLogical(feed.srcLogicalId);
        synth->connect(
            srcNodeId, feed.srcChan, recorderNodeId, feed.dstChan); // dstChan will be 0 or 1

        // Get the name of the first source for the filename prefix
        if (sourceName.isEmpty())
        {
            if (auto* srcModule = synth->getModuleForLogical(feed.srcLogicalId))
            {
                sourceName = srcModule->getName();
            }
        }
    }

    if (recorder)
    {
        recorder->updateSuggestedFilename(sourceName);
    }

    graphNeedsRebuild = true;
    juce::Logger::writeToLog("[Record Output] Recorder added and connected.");
}
void ImGuiNodeEditorComponent::handleColorCodedChaining(PinDataType targetType)
{
    if (synth == nullptr)
    {
        juce::Logger::writeToLog("[Color Chaining] ERROR: synth is nullptr");
        return;
    }

    const int numSelected = ImNodes::NumSelectedNodes();
    if (numSelected <= 1)
    {
        juce::Logger::writeToLog(
            "[Color Chaining] ERROR: numSelected <= 1 (" + juce::String(numSelected) + ")");
        return;
    }

    juce::Logger::writeToLog(
        "[Color Chaining] Started for " + juce::String(toString(targetType)) + " with " +
        juce::String(numSelected) + " nodes");

    // 1. Get and sort selected nodes by their horizontal position.
    std::vector<int> selectedNodeIds(numSelected);
    ImNodes::GetSelectedNodes(selectedNodeIds.data());

    std::vector<std::pair<float, int>> sortedNodes;
    for (int nodeId : selectedNodeIds)
    {
        if (nodeId == 0)
            continue; // Exclude the output node.
        ImVec2 pos = ImNodes::GetNodeGridSpacePos(nodeId);
        sortedNodes.push_back({pos.x, nodeId});
    }

    if (sortedNodes.empty())
    {
        juce::Logger::writeToLog("[Color Chaining] ERROR: No valid nodes after filtering");
        return;
    }

    std::sort(sortedNodes.begin(), sortedNodes.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    // Create a single undo action for the entire operation.
    pushSnapshot();

    int totalConnectionsMade = 0;
    int totalConnectionAttempts = 0;

    // 2. Iterate through sorted nodes and connect matching pins.
    for (size_t i = 0; i < sortedNodes.size() - 1; ++i)
    {
        juce::uint32 sourceLid = sortedNodes[i].second;
        juce::uint32 destLid = sortedNodes[i + 1].second;

        auto sourceNodeId = synth->getNodeIdForLogical(sourceLid);
        auto destNodeId = synth->getNodeIdForLogical(destLid);

        if (sourceNodeId.uid == 0 || destNodeId.uid == 0)
        {
            juce::Logger::writeToLog(
                "[Color Chaining] Skipping invalid node pair: " + juce::String(sourceLid) + " -> " +
                juce::String(destLid));
            continue;
        }

        // Find all matching output pins on the source and input pins on the destination.
        auto sourcePins = getPinsOfType(sourceLid, false, targetType);
        auto destPins = getPinsOfType(destLid, true, targetType);

        if (sourcePins.empty() || destPins.empty())
        {
            juce::Logger::writeToLog(
                "[Color Chaining] No matching pins: " + juce::String(sourcePins.size()) + " src, " +
                juce::String(destPins.size()) + " dst");
            continue;
        }

        // Connect source pins to destination pins, skipping already-connected inputs.
        // For each source pin, find the next available destination input.
        for (size_t srcIdx = 0; srcIdx < sourcePins.size(); ++srcIdx)
        {
            // Find the next available destination input pin
            bool connected = false;
            for (size_t dstIdx = 0; dstIdx < destPins.size(); ++dstIdx)
            {
                // Check if this destination input is already connected
                if (isInputConnected(destLid, destPins[dstIdx].channel))
                {
                    juce::Logger::writeToLog(
                        "[Color Chaining] Skipping already-connected input: " +
                        getTypeForLogical(destLid) + " channel " +
                        juce::String(destPins[dstIdx].channel));
                    continue; // Try next destination pin
                }

                // Attempt connection to this available input
                totalConnectionAttempts++;
                bool connectResult = synth->connect(
                    sourceNodeId, sourcePins[srcIdx].channel, destNodeId, destPins[dstIdx].channel);

                if (connectResult)
                {
                    totalConnectionsMade++;
                    connected = true;
                    juce::Logger::writeToLog(
                        "[Color Chaining] Connected " + getTypeForLogical(sourceLid) + ":" +
                        juce::String(sourcePins[srcIdx].channel) + " -> " +
                        getTypeForLogical(destLid) + ":" + juce::String(destPins[dstIdx].channel));

                    // Check if the destination is a recorder and update its filename
                    if (auto* destModule = synth->getModuleForLogical(destLid))
                    {
                        if (auto* recorder = dynamic_cast<RecordModuleProcessor*>(destModule))
                        {
                            if (auto* sourceModule = synth->getModuleForLogical(sourceLid))
                            {
                                recorder->updateSuggestedFilename(sourceModule->getName());
                            }
                        }
                    }
                    break; // Successfully connected, move to next source pin
                }
            }

            if (!connected)
            {
                juce::Logger::writeToLog(
                    "[Color Chaining] No available input for " + getTypeForLogical(sourceLid) +
                    ":" + juce::String(sourcePins[srcIdx].channel) + " -> " +
                    getTypeForLogical(destLid) + " (all inputs connected or incompatible)");
            }
        }
    }

    juce::Logger::writeToLog(
        "[Color Chaining] Completed: " + juce::String(totalConnectionsMade) + "/" +
        juce::String(totalConnectionAttempts) + " connections made");

    // 3. Apply all new connections to the audio graph.
    graphNeedsRebuild = true;
}

// Module Category Color Coding
ImGuiNodeEditorComponent::ModuleCategory ImGuiNodeEditorComponent::getModuleCategory(
    const juce::String& moduleType)
{
    juce::String lower = moduleType.toLowerCase();

    // === CATEGORY CLASSIFICATION (Following Dictionary Structure) ===

    // --- 1. SOURCES (Green) ---
    if (lower.contains("vco") || lower.contains("polyvco") || lower.contains("stk_string") ||
        lower.contains("stk") || lower.contains("noise") || lower == "audio_input" ||
        lower.contains("sample") || lower == "value" ||
        // Streaming audio sources
        lower == "internet_radio" || lower == "system_audio" || lower == "srt_receiver" ||
        lower == "rtmp_receiver" || lower == "bluetooth_audio" || lower == "sdr_receiver")
        return ModuleCategory::Source;

    // --- 2. EFFECTS (Red) ---
    // Note: Recorder moved to System, Vocal Tract Filter moved to TTS
    if (lower.contains("vcf") || lower.contains("delay") || lower.contains("reverb") ||
        lower.contains("chorus") || lower.contains("phaser") || lower.contains("compressor") ||
        lower.contains("limiter") || lower == "gate" || lower.contains("drive") ||
        lower.contains("bit_crusher") || lower.contains("crusher") || lower.contains("eq") ||
        lower.contains("waveshaper") || lower.contains("8bandshaper") ||
        lower.contains("granulator") || lower.contains("spatial_granulator") ||
        lower.contains("harmonic_shaper") || lower.contains("timepitch") ||
        lower.contains("crackle"))
        return ModuleCategory::Effect;

    // --- 3. MODULATORS (Blue) ---
    if (lower.contains("lfo") || lower.contains("adsr") || lower.contains("random") ||
        lower.contains("s&h") || lower.contains("function_generator") ||
        lower.contains("shaping_oscillator"))
        return ModuleCategory::Modulator;

    // --- 4. UTILITIES & LOGIC (Orange) ---
    if (lower.contains("vca") || lower.contains("mixer") || lower.contains("attenuverter") ||
        lower.contains("lag_processor") || lower.contains("math") || lower.contains("map_range") ||
        lower.contains("quantizer") || lower.contains("rate") || lower.contains("comparator") ||
        lower.contains("logic") || lower.contains("reroute") || lower.contains("panvol") ||
        lower.contains("clock_divider") || lower.contains("sequential_switch"))
        return ModuleCategory::Utility;

    // --- 5. SEQUENCERS (Light Green) ---
    if (lower.contains("sequencer") || lower.contains("tempo_clock") || lower == "timeline" ||
        lower == "chord_arp" || lower == "automation_lane" || lower == "automato")
        return ModuleCategory::Seq;

    // --- 6. MIDI (Vibrant Purple) ---
    if (lower.contains("midi") || lower.contains("osc"))
        return ModuleCategory::MIDI;

    // --- 7. ANALYSIS (Purple) ---
    if (lower.contains("scope") || lower.contains("debug") || lower.contains("frequency_graph") ||
        lower.contains("onset_detector") || lower.contains("essentia"))
        return ModuleCategory::Analysis;

    // --- 8. TTS (Peach/Coral) ---
    if (lower.contains("tts") || lower.contains("vocal_tract"))
        return ModuleCategory::TTS_Voice;

    // --- 9. SPECIAL (Cyan) - Physics & Animation ---
    if (lower.contains("physics") || lower.contains("animation"))
        return ModuleCategory::Special_Exp;

    // --- 10. COMPUTER VISION (Bright Orange) ---
#ifndef AUDIO_ONLY_BUILD
    if (lower.contains("webcam") || lower.contains("video_file") || lower == "video_fx" ||
        lower == "chromakey" || lower == "video_compositor" || lower == "video_draw_impact" ||
        lower == "crop_video" || lower == "video_viewer" || lower.contains("movement") ||
        lower.contains("detector") || lower.contains("opencv") || lower.contains("vision") ||
        lower.contains("tracker") || lower.contains("segmentation") ||
        lower.contains("pose_estimator") || lower == "youtube_source" || lower == "ndi_receiver")
        return ModuleCategory::OpenCV;
#endif

    // --- 11. SYSTEM (Lavender) ---
    if (lower.contains("meta") || lower.contains("inlet") || lower.contains("outlet") ||
        lower.contains("comment") || lower.contains("recorder") || lower.contains("vst_host") ||
        lower == "bpm_monitor" || lower.contains("bpm monitor"))
        return ModuleCategory::Sys;

    // --- 12. PLUGINS (Teal) ---
    if (lower.contains("vst") || lower.contains("plugin"))
        return ModuleCategory::Plugin;

    // --- Default: Utility ---
    return ModuleCategory::Utility;
}

unsigned int ImGuiNodeEditorComponent::getImU32ForCategory(ModuleCategory category, bool hovered)
{
    ImU32 color =
        ThemeManager::getInstance().getCategoryColor(static_cast<::ModuleCategory>(category));

    if (hovered)
    {
        // Brighten on hover
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
        c.x = juce::jmin(c.x * 1.3f, 1.0f);
        c.y = juce::jmin(c.y * 1.3f, 1.0f);
        c.z = juce::jmin(c.z * 1.3f, 1.0f);
        return ImGui::ColorConvertFloat4ToU32(c);
    }
    return color;
}

// Quick Add Menu - Module Registry - Dictionary
// Maps Display Name -> { Internal Type, Description }
std::map<juce::String, std::pair<const char*, const char*>> ImGuiNodeEditorComponent::
    getModuleRegistry()
{
    return {
        // Sources
        {"Audio Input", {"audio_input", "Records audio from your audio interface"}},
        {"VCO", {"vco", "Voltage Controlled Oscillator - generates waveforms"}},
        {"STK String",
         {"stk_string",
          "Physical modeling string synthesizer (guitar, violin, cello, sitar, banjo)"}},
        {"STK Wind",
         {"stk_wind", "Physical modeling wind instruments (flute, clarinet, saxophone, brass)"}},
        {"STK Percussion",
         {"stk_percussion", "Modal synthesis percussion (marimba, cymbal, shakers, etc.)"}},
        {"STK Plucked", {"stk_plucked", "Karplus-Strong plucked string synthesis"}},
        {"Polyphonic VCO", {"polyvco", "Polyphonic VCO with multiple voices"}},
        {"Onset Detector", {"essentia_onset_detector", "Detects note onsets (attacks) in audio"}},
        {"Pitch Tracker",
         {"essentia_pitch_tracker", "Detects pitch (fundamental frequency) in audio"}},
        {"Noise", {"noise", "White, pink, or brown noise generator"}},
        {"Sequencer", {"sequencer", "Step sequencer for creating patterns"}},
        {"Multi Sequencer", {"multi_sequencer", "Multi-track step sequencer"}},
        {"Stroke Sequencer", {"stroke_sequencer", "Freeform visual rhythmic and CV generator"}},
        {"Chord Arp",
         {"chord_arp", "Harmony brain that generates chords and arpeggios from CV inputs"}},
        {"MIDI Player", {"midi_player", "Plays MIDI files"}},
        {"MIDI CV", {"midi_cv", "Converts MIDI Note/CC messages to CV signals. (Monophonic)"}},
        {"OSC CV", {"osc_cv", "Converts OSC (Open Sound Control) messages to CV/Gate signals"}},
        {"CV OSC",
         {"cv_osc_sender",
          "Converts CV/Audio/Gate signals to OSC messages. Send internal signals over the network "
          "with configurable addresses."}},
        {"CV -> OSC",
         {"cv_osc_sender",
          "Converts CV/Audio/Gate signals to OSC messages. Send internal signals over the network "
          "with configurable addresses."}},
        {"MIDI Faders", {"midi_faders", "Up to 16 MIDI faders with CC learning"}},
        {"MIDI Knobs", {"midi_knobs", "Up to 16 MIDI knobs/rotary encoders with CC learning"}},
        {"MIDI Buttons", {"midi_buttons", "Up to 32 MIDI buttons with Gate/Toggle/Trigger modes"}},
        {"MIDI Jog Wheel", {"midi_jog_wheel", "Single MIDI jog wheel/rotary encoder"}},
        {"MIDI Pads",
         {"midi_pads", "16-pad MIDI controller with polyphonic triggers and velocity outputs"}},
        {"MIDI Logger",
         {"midi_logger", "Records CV/Gate to MIDI events with piano roll editor and .mid export"}},
        {"Value", {"value", "Constant CV value output"}},
        {"Sample Loader", {"sample_loader", "Loads and plays audio samples"}},
        {"Sample SFX",
         {"sample_sfx", "Plays sample variations from a folder with automatic switching"}},

        // Wireless/Streaming Audio
        {"Internet Radio",
         {"internet_radio",
          "Receives audio from internet radio streams (Icecast, Shoutcast). "
          "Supports MP3, AAC, OGG with ICY metadata."}},
        {"System Audio",
         {"system_audio",
          "Captures system audio using WASAPI loopback (Windows). "
          "Record Spotify, YouTube, games, etc."}},
        {"SRT Receiver",
         {"srt_receiver",
          "Receives low-latency audio via SRT (Secure Reliable Transport) protocol. "
          "Supports encryption and both Listener/Caller modes."}},
        {"RTMP Receiver",
         {"rtmp_receiver",
          "Receives audio from RTMP streams. "
          "Compatible with OBS Studio and streaming servers."}},
        {"Bluetooth Audio",
         {"bluetooth_audio",
          "Receives audio from Bluetooth devices. "
          "Connect to phones, tablets, and other Bluetooth sources."}},
        {"SDR Receiver",
         {"sdr_receiver",
          "Receives radio signals (FM/AM/SSB) from RTL-SDR USB dongles. Visualizes spectrum and "
          "demodulates audio."}},

        // TTS
        {"TTS Performer", {"tts_performer", "Text-to-speech synthesizer"}},
        {"Vocal Tract Filter", {"vocal_tract_filter", "Physical model vocal tract filter"}},

        // Physics & Animation
        {"Physics", {"physics", "2D physics simulation for audio modulation"}},
        {"Animation", {"animation", "Skeletal animation system with glTF file support"}},

#ifndef AUDIO_ONLY_BUILD
        // OpenCV (Computer Vision)
        {"Video Viewer",
         {"video_viewer",
          "Displays video in a separate resizable window for viewing or OBS capture. The window "
          "can be positioned on any monitor."}},
        {"Webcam Loader",
         {"webcam_loader",
          "Captures video from a webcam and publishes it as a source for vision processing "
          "modules"}},
        {"Video File Loader",
         {"video_file_loader",
          "Loads and plays a video file, publishes it as a source for vision processing modules"}},
        {"NDI Receiver",
         {"ndi_receiver",
          "Receives NDI video streams over the network from cameras, production software, and "
          "other NDI-enabled devices"}},
        {"YouTube Source",
         {"youtube_source",
          "Plays audio/video from YouTube URLs. Supports VOD downloads and live streams. "
          "Downloads cached in 'downloads' folder. Video output in full build only."}},
        {"Video FX",
         {"video_fx",
          "Applies real-time video effects (brightness, contrast, saturation, blur, sharpen, etc.) "
          "to video sources, chainable"}},
        {"Chromakey",
         {"chromakey",
          "Removes selected colors from video and converts them to alpha transparency, with spill "
          "suppression and feathering"}},
        {"Video Compositor",
         {"video_compositor", "Composites multiple video layers with blend modes and transforms"}},
        {"Video Draw Impact",
         {"video_draw_impact",
          "Allows drawing colored impact marks on video frames. Drawings persist for a "
          "configurable number of frames, creating visual rhythms that can be tracked by the Color "
          "Tracker node."}},
        {"Crop Video",
         {"crop_video",
          "Crops and resizes video frames to a specified region, chainable video processor"}},
        {"Movement Detector",
         {"movement_detector",
          "Analyzes video source for motion via optical flow or background subtraction, outputs "
          "motion data as CV"}},
        {"Object Detector",
         {"object_detector",
          "Uses YOLOv3 to detect objects (person, car, etc.) and outputs bounding box "
          "position/size as CV"}},
        {"Pose Estimator",
         {"pose_estimator",
          "Uses OpenPose to detect 15 body keypoints (head, shoulders, elbows, wrists, hips, "
          "knees, ankles) and outputs their positions as CV signals"}},
        {"Hand Tracker",
         {"hand_tracker",
          "Detects 21 hand keypoints and outputs their X/Y positions as CV (42 channels)"}},
        {"Face Tracker",
         {"face_tracker",
          "Detects 70 facial landmarks and outputs X/Y positions as CV (140 channels)"}},
        {"Color Tracker",
         {"color_tracker",
          "Tracks multiple colors in video and outputs their positions and sizes as CV"}},
        {"Contour Detector",
         {"contour_detector",
          "Detects shapes via background subtraction and outputs area, complexity, and aspect "
          "ratio as CV"}},
        {"Semantic Segmentation",
         {"semantic_segmentmentation",
          "Uses deep learning to segment video into semantic regions and outputs detected areas as "
          "CV"}},
#endif

        // Effects
        {"VCF", {"vcf", "Voltage Controlled Filter"}},
        {"Delay", {"delay", "Echo/delay effect"}},
        {"Reverb", {"reverb", "Reverb effect"}},
        {"Chorus", {"chorus", "Chorus effect for thickening sound"}},
        {"Phaser", {"phaser", "Phaser modulation effect"}},
        {"Compressor", {"compressor", "Dynamic range compressor"}},
        {"Recorder", {"recorder", "Records audio to disk"}},
        {"Limiter", {"limiter", "Peak limiter"}},
        {"Noise Gate", {"gate", "Noise gate"}},
        {"Drive", {"drive", "Distortion/overdrive"}},
        {"Bit Crusher", {"bit_crusher", "Bit depth and sample rate reduction"}},
        {"PanVol", {"panvol", "2D control surface for volume and panning"}},
        {"Graphic EQ", {"graphic_eq", "Graphic equalizer"}},
        {"Waveshaper", {"waveshaper", "Waveshaping distortion"}},
        {"8-Band Shaper", {"8bandshaper", "8-band spectral shaper"}},
        {"Granulator", {"granulator", "Granular synthesis effect"}},
        {"Spatial Granulator",
         {"spatial_granulator", "Visual canvas granulator/chorus with color-coded parameters"}},
        {"Harmonic Shaper", {"harmonic_shaper", "Harmonic content shaper"}},
        {"Time/Pitch Shifter", {"timepitch", "Time stretching and pitch shifting"}},
        {"De-Crackle", {"de_crackle", "Removes clicks and pops"}},

        // Modulators
        {"LFO", {"lfo", "Low Frequency Oscillator for modulation"}},
        {"ADSR", {"adsr", "Attack Decay Sustain Release envelope"}},
        {"Random", {"random", "Random value generator"}},
        {"S&H", {"s_and_h", "Sample and Hold"}},
        {"Tempo Clock",
         {"tempo_clock",
          "Global clock with BPM control, transport (play/stop/reset), division, swing, and "
          "clock/gate outputs. Use External Takeover to drive the master transport."}},
        {"Function Generator", {"function_generator", "Custom function curves"}},
        {"Automation Lane", {"automation_lane", "Draw automation curves on scrolling timeline"}},
        {"Automato", {"automato", "Record and replay 2D gestures with transport sync"}},
        {"Shaping Oscillator", {"shaping_oscillator", "Oscillator with waveshaping"}},

        // Utilities
        {"VCA", {"vca", "Voltage Controlled Amplifier"}},
        {"Mixer", {"mixer", "Audio/CV mixer"}},
        {"CV Mixer", {"cv_mixer", "CV signal mixer"}},
        {"Track Mixer", {"track_mixer", "Multi-track mixer with panning"}},
        {"Attenuverter", {"attenuverter", "Attenuate and invert signals"}},
        {"Reroute",
         {"reroute", "A polymorphic passthrough node. Pin color adapts to the input signal."}},
        {"Lag Processor", {"lag_processor", "Slew rate limiter/smoother"}},
        {"Math", {"math", "Mathematical operations"}},
        {"Map Range", {"map_range", "Map values from one range to another"}},
        {"Quantizer", {"quantizer", "Quantize CV to scales"}},
        {"Rate", {"rate", "Rate/frequency divider"}},
        {"Comparator", {"comparator", "Compare and threshold signals"}},
        {"Logic", {"logic", "Boolean logic operations"}},
        {"Clock Divider", {"clock_divider", "Clock division and multiplication"}},
        {"Sequential Switch", {"sequential_switch", "Sequential signal router"}},
        {"Comment", {"comment", "Text comment box"}},
        {"Snapshot Sequencer",
         {"snapshot_sequencer", "Snapshot sequencer for parameter automation"}},
        {"Timeline",
         {"timeline",
          "Transport-synchronized automation recorder for CV, Gate, Trigger, and Raw signals"}},
        {"BPM Monitor",
         {"bpm_monitor",
          "Hybrid rhythm detection and BPM reporting from sequencers and audio inputs"}},

        // Analysis
        {"Scope", {"scope", "Oscilloscope display"}},
        {"Debug", {"debug", "Debug value display"}},
        {"Input Debug", {"input_debug", "Input signal debugger"}},
        {"Frequency Graph", {"frequency_graph", "Spectrum analyzer display"}}};
}

// Legacy function for backwards compatibility with tooltip display
std::vector<std::pair<juce::String, const char*>> ImGuiNodeEditorComponent::getModuleDescriptions()
{
    std::vector<std::pair<juce::String, const char*>> result;
    for (const auto& entry : getModuleRegistry())
    {
        // Return {internal type, description} for compatibility
        result.push_back({entry.second.first, entry.second.second});
    }
    return result;
}

// VST Plugin Support
void ImGuiNodeEditorComponent::addPluginModules()
{
    if (synth == nullptr)
        return;

    auto& app = PresetCreatorApplication::getApp();
    auto& knownPluginList = app.getKnownPluginList();
    auto& formatManager = app.getPluginFormatManager();

    // Set the plugin format manager and known plugin list on the synth if not already set
    synth->setPluginFormatManager(&formatManager);
    synth->setKnownPluginList(&knownPluginList);

    // Get the VST folder at exe position
    juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
    juce::File vstFolder = exeDir.getChildFile("VST");

    // Get all plugins and filter/deduplicate
    const auto& allPlugins = knownPluginList.getTypes();

    if (allPlugins.isEmpty())
    {
        ImGui::TextDisabled("No plugins found.");
        ImGui::TextDisabled("Use 'Scan for Plugins...' in the File menu.");
        return;
    }

    // Filter to only plugins in the VST folder and deduplicate
    std::vector<juce::PluginDescription> filteredPlugins;
    std::set<juce::String>               seenPlugins; // Use name + manufacturer as unique key

    for (const auto& desc : allPlugins)
    {
        // Check if plugin is in the VST folder at exe position
        juce::File pluginFile(desc.fileOrIdentifier);
        if (!pluginFile.existsAsFile())
            continue;

        juce::File pluginDir = pluginFile.getParentDirectory();
        if (!pluginDir.isAChildOf(vstFolder) && pluginDir != vstFolder)
            continue;

        // Create unique key for deduplication (name + manufacturer)
        juce::String uniqueKey = desc.name + "|" + desc.manufacturerName;
        if (seenPlugins.find(uniqueKey) != seenPlugins.end())
            continue; // Skip duplicate

        seenPlugins.insert(uniqueKey);
        filteredPlugins.push_back(desc);
    }

    if (filteredPlugins.empty())
    {
        ImGui::TextDisabled("No plugins found in VST folder.");
        ImGui::TextDisabled(("Place VST plugins in: " + vstFolder.getFullPathName()).toRawUTF8());
        return;
    }

    // Use PushID to create unique IDs for each plugin to avoid conflicts when called from multiple
    // menus
    ImGui::PushID("PluginList");
    int pluginIndex = 0;
    for (const auto& desc : filteredPlugins)
    {
        ImGui::PushID(pluginIndex++);
        juce::String buttonLabel = desc.name;
        if (desc.manufacturerName.isNotEmpty())
        {
            buttonLabel += " (" + desc.manufacturerName + ")";
        }

        if (ImGui::Selectable(buttonLabel.toRawUTF8()))
        {
            auto nodeId = synth->addVstModule(formatManager, desc);
            if (nodeId.uid != 0)
            {
                const ImVec2 mouse = ImGui::GetMousePos();
                const auto   logicalId = synth->getLogicalIdForNode(nodeId);
                pendingNodeScreenPositions[(int)logicalId] = mouse;
                snapshotAfterEditor = true;
                juce::Logger::writeToLog("[VST] Added plugin: " + desc.name);
                // Close popup if we're in a popup context (safe to call even if not in popup)
                ImGui::CloseCurrentPopup();
            }
            else
            {
                juce::Logger::writeToLog("[VST] ERROR: Failed to add plugin: " + desc.name);
            }
        }

        // Show tooltip with plugin info on hover
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Name: %s", desc.name.toRawUTF8());
            ImGui::Text("Manufacturer: %s", desc.manufacturerName.toRawUTF8());
            ImGui::Text("Version: %s", desc.version.toRawUTF8());
            ImGui::Text("Format: %s", desc.pluginFormatName.toRawUTF8());
            ImGui::Text("Type: %s", desc.isInstrument ? "Instrument" : "Effect");
            ImGui::Text("Inputs: %d", desc.numInputChannels);
            ImGui::Text("Outputs: %d", desc.numOutputChannels);
            ImGui::EndTooltip();
        }

        ImGui::PopID(); // Pop plugin index ID
    }
    ImGui::PopID(); // Pop PluginList ID
}

void ImGuiNodeEditorComponent::drawVstMenuByManufacturer(bool isMultiInsert, bool isVideoCable)
{
    if (isVideoCable)
        return; // VST plugins are audio-only

    auto& app = PresetCreatorApplication::getApp();
    auto& knownPluginList = app.getKnownPluginList();

    // Use VstManager to get plugins organized by manufacturer
    auto* rootNode = m_vstManager.getRootNode();
    if (!rootNode || rootNode->subdirectories.empty())
    {
        ImGui::TextDisabled("No plugins found.");
        return;
    }

    // Iterate through manufacturer nodes (subdirectories)
    for (const auto& manufacturerNode : rootNode->subdirectories)
    {
        if (manufacturerNode->plugins.empty())
            continue;

        // Create collapsible tree node for each manufacturer
        if (ImGui::TreeNode(manufacturerNode->name.toRawUTF8()))
        {
            // List all plugins for this manufacturer
            for (const auto& plugin : manufacturerNode->plugins)
            {
                juce::String menuLabel = plugin.name;

                if (ImGui::MenuItem(menuLabel.toRawUTF8()))
                {
                    if (isMultiInsert)
                    {
                        handleInsertNodeOnSelectedLinks(plugin.description.name);
                    }
                    else
                    {
                        insertNodeBetween(plugin.description.name);
                    }
                    ImGui::CloseCurrentPopup();
                }

                // Show tooltip with plugin info
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("Manufacturer: %s", plugin.manufacturer.toRawUTF8());
                    ImGui::Text("Version: %s", plugin.version.toRawUTF8());
                    ImGui::Text("Type: %s", plugin.isInstrument ? "Instrument" : "Effect");
                    ImGui::Text("Inputs: %d, Outputs: %d", plugin.numInputs, plugin.numOutputs);
                    ImGui::EndTooltip();
                }
            }

            ImGui::TreePop();
        }
    }
}

void ImGuiNodeEditorComponent::drawVstMenuByManufacturerForAddModule()
{
    if (synth == nullptr)
        return;

    auto& app = PresetCreatorApplication::getApp();
    auto& formatManager = app.getPluginFormatManager();

    // Use VstManager to get plugins organized by manufacturer
    auto* rootNode = m_vstManager.getRootNode();
    if (!rootNode || rootNode->subdirectories.empty())
    {
        ImGui::TextDisabled("No plugins found.");
        return;
    }

    // Iterate through manufacturer nodes (subdirectories)
    for (const auto& manufacturerNode : rootNode->subdirectories)
    {
        if (manufacturerNode->plugins.empty())
            continue;

        // Create collapsible tree node for each manufacturer
        if (ImGui::TreeNode(manufacturerNode->name.toRawUTF8()))
        {
            // List all plugins for this manufacturer
            for (const auto& plugin : manufacturerNode->plugins)
            {
                juce::String menuLabel = plugin.name;

                if (ImGui::MenuItem(menuLabel.toRawUTF8()))
                {
                    auto nodeId = synth->addVstModule(formatManager, plugin.description);
                    if (nodeId.uid != 0)
                    {
                        const ImVec2 mouse = ImGui::GetMousePos();
                        const auto   logicalId = synth->getLogicalIdForNode(nodeId);
                        pendingNodeScreenPositions[(int)logicalId] = mouse;
                        snapshotAfterEditor = true;
                        juce::Logger::writeToLog("[VST] Added plugin: " + plugin.name);
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        juce::Logger::writeToLog(
                            "[VST] ERROR: Failed to add plugin: " + plugin.name);
                    }
                }

                // Show tooltip with plugin info
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("Manufacturer: %s", plugin.manufacturer.toRawUTF8());
                    ImGui::Text("Version: %s", plugin.version.toRawUTF8());
                    ImGui::Text("Type: %s", plugin.isInstrument ? "Instrument" : "Effect");
                    ImGui::Text("Inputs: %d, Outputs: %d", plugin.numInputs, plugin.numOutputs);
                    ImGui::EndTooltip();
                }
            }

            ImGui::TreePop();
        }
    }
}

void ImGuiNodeEditorComponent::handleCollapseToMetaModule()
{
    if (!synth)
        return;

    juce::Logger::writeToLog("[Meta Module] Starting collapse operation...");

    // 1. Get selected nodes
    const int numSelected = ImNodes::NumSelectedNodes();
    if (numSelected < 2)
    {
        juce::Logger::writeToLog("[Meta Module] ERROR: Need at least 2 nodes selected");
        return;
    }

    std::vector<int> selectedNodeIds(numSelected);
    ImNodes::GetSelectedNodes(selectedNodeIds.data());

    // Convert to logical IDs
    std::set<juce::uint32> selectedLogicalIds;
    for (int nodeId : selectedNodeIds)
    {
        selectedLogicalIds.insert((juce::uint32)nodeId);
    }

    juce::Logger::writeToLog("[Meta Module] Selected " + juce::String(numSelected) + " nodes");

    // 2. Analyze boundary connections
    struct BoundaryConnection
    {
        juce::uint32 externalLogicalId;
        int          externalChannel;
        juce::uint32 internalLogicalId;
        int          internalChannel;
        bool         isInput; // true = external -> internal, false = internal -> external
    };
    std::vector<BoundaryConnection> boundaries;
    using InletKey = std::pair<juce::uint32, int>;
    using OutletKey = std::pair<juce::uint32, int>;
    struct InletInfo
    {
        juce::uint32 logicalId;
        int          pinIndex;
        int          channelCount;
        juce::uint32 externalLogicalId;
        int          externalChannel;
    };
    struct OutletInfo
    {
        juce::uint32 logicalId;
        int          pinIndex;
        int          channelCount;
        juce::uint32 externalLogicalId;
        int          externalChannel;
        bool         externalIsOutput;
    };
    std::map<InletKey, InletInfo>                inletInfoMap;
    std::map<OutletKey, OutletInfo>              outletInfoMap;
    std::unordered_map<juce::uint32, InletInfo>  inletInfoByLogical;
    std::unordered_map<juce::uint32, OutletInfo> outletInfoByLogical;
    int                                          inletPinIndexCounter = 0;
    int                                          outletPinIndexCounter = 0;
    auto                                         allConnections = synth->getConnectionsInfo();
    for (const auto& conn : allConnections)
    {
        bool srcIsSelected = selectedLogicalIds.count(conn.srcLogicalId) > 0;
        bool dstIsSelected = selectedLogicalIds.count(conn.dstLogicalId) > 0 && !conn.dstIsOutput;
        bool dstIsOutput = conn.dstIsOutput;

        // Inlet: external -> selected
        if (!srcIsSelected && dstIsSelected)
        {
            BoundaryConnection bc;
            bc.externalLogicalId = conn.srcLogicalId;
            bc.externalChannel = conn.srcChan;
            bc.internalLogicalId = conn.dstLogicalId;
            bc.internalChannel = conn.dstChan;
            bc.isInput = true;
            boundaries.push_back(bc);
            juce::Logger::writeToLog(
                "[Meta Module] Found inlet: " + juce::String(bc.externalLogicalId) + " -> " +
                juce::String(bc.internalLogicalId));
        }
        // Outlet: selected -> external or output
        else if (srcIsSelected && (!dstIsSelected || dstIsOutput))
        {
            BoundaryConnection bc;
            bc.externalLogicalId = dstIsOutput ? 0 : conn.dstLogicalId;
            bc.externalChannel = conn.dstChan;
            bc.internalLogicalId = conn.srcLogicalId;
            bc.internalChannel = conn.srcChan;
            bc.isInput = false;
            boundaries.push_back(bc);
            juce::Logger::writeToLog(
                "[Meta Module] Found outlet: " + juce::String(bc.internalLogicalId) + " -> " +
                (dstIsOutput ? "OUTPUT" : juce::String(bc.externalLogicalId)));
        }
    }

    // Count inlets and outlets
    int numInlets = 0;
    int numOutlets = 0;
    for (const auto& bc : boundaries)
    {
        if (bc.isInput)
            numInlets++;
        else
            numOutlets++;
    }

    juce::Logger::writeToLog(
        "[META] Boundary Detection: Found " + juce::String(numInlets) + " inlets and " +
        juce::String(numOutlets) + " outlets.");
    juce::Logger::writeToLog(
        "[META] Found " + juce::String(boundaries.size()) + " boundary connections");

    if (boundaries.empty())
    {
        juce::Logger::writeToLog(
            "[META] WARNING: No boundary connections - creating isolated meta module");
    }

    // 3. Create the internal graph state
    pushSnapshot(); // Make undoable

    // Save the state of selected nodes
    juce::MemoryBlock internalState;
    {
        // Create a temporary state containing only selected nodes
        juce::ValueTree internalRoot("ModularSynthPreset");
        internalRoot.setProperty("version", 1, nullptr);

        juce::ValueTree modsVT("modules");
        juce::ValueTree connsVT("connections");

        // Add selected modules
        std::map<juce::uint32, juce::uint32> oldToNewLogicalId;
        juce::uint32                         newLogicalId = 1;

        for (juce::uint32 oldId : selectedLogicalIds)
        {
            oldToNewLogicalId[oldId] = newLogicalId++;

            auto* module = synth->getModuleForLogical(oldId);
            if (!module)
                continue;

            juce::String moduleType = synth->getModuleTypeForLogical(oldId);

            juce::ValueTree mv("module");
            mv.setProperty("logicalId", (int)oldToNewLogicalId[oldId], nullptr);
            mv.setProperty("type", moduleType, nullptr);

            // Save parameters
            juce::ValueTree params = module->getAPVTS().copyState();
            juce::ValueTree paramsWrapper("params");
            paramsWrapper.addChild(params, -1, nullptr);
            mv.addChild(paramsWrapper, -1, nullptr);

            // Save extra state
            if (auto extra = module->getExtraStateTree(); extra.isValid())
            {
                juce::ValueTree extraWrapper("extra");
                extraWrapper.addChild(extra, -1, nullptr);
                mv.addChild(extraWrapper, -1, nullptr);
            }

            modsVT.addChild(mv, -1, nullptr);
        }

        auto createParameterState = [](const juce::String& paramId, int value) {
            juce::ValueTree params("Parameters");
            juce::ValueTree paramNode("Parameter");
            paramNode.setProperty("id", paramId, nullptr);
            paramNode.setProperty("value", (double)value, nullptr);
            params.addChild(paramNode, -1, nullptr);
            return params;
        };

        // Add inlet modules for each unique input
        for (const auto& bc : boundaries)
        {
            if (!bc.isInput)
                continue;

            InletKey key{bc.externalLogicalId, bc.externalChannel};
            if (inletInfoMap.find(key) != inletInfoMap.end())
                continue;

            const juce::uint32 inletId = newLogicalId++;
            const int          pinIndex = inletPinIndexCounter++;
            const int          channelCount = 1;

            InletInfo info{
                inletId, pinIndex, channelCount, bc.externalLogicalId, bc.externalChannel};
            inletInfoMap[key] = info;
            inletInfoByLogical.emplace(inletId, info);

            juce::ValueTree mv("module");
            mv.setProperty("logicalId", (int)inletId, nullptr);
            mv.setProperty("type", "inlet", nullptr);

            juce::ValueTree paramsWrapper("params");
            paramsWrapper.addChild(
                createParameterState(InletModuleProcessor::paramIdChannelCount, channelCount),
                -1,
                nullptr);
            mv.addChild(paramsWrapper, -1, nullptr);

            juce::ValueTree extra("InletState");
            juce::String    inletLabel;
            if (auto* srcModule = synth->getModuleForLogical(bc.externalLogicalId))
            {
                inletLabel = srcModule->getName();
                const juce::String channelLabel =
                    srcModule->getAudioOutputLabel(bc.externalChannel);
                if (channelLabel.isNotEmpty())
                    inletLabel += " :: " + channelLabel;
                else
                    inletLabel += " :: Out " + juce::String(bc.externalChannel + 1);
            }
            else
            {
                inletLabel = "In " + juce::String(pinIndex + 1);
            }
            extra.setProperty("customLabel", inletLabel, nullptr);
            extra.setProperty("pinIndex", pinIndex, nullptr);
            extra.setProperty("externalLogicalId", (int)bc.externalLogicalId, nullptr);
            extra.setProperty("externalChannel", bc.externalChannel, nullptr);
            juce::ValueTree extraWrapper("extra");
            extraWrapper.addChild(extra, -1, nullptr);
            mv.addChild(extraWrapper, -1, nullptr);

            modsVT.addChild(mv, -1, nullptr);
            juce::Logger::writeToLog(
                "[Meta Module] Created inlet node ID=" + juce::String(inletId));
        }

        // Add outlet modules for each unique output
        for (const auto& bc : boundaries)
        {
            if (bc.isInput)
                continue;

            OutletKey key{bc.internalLogicalId, bc.internalChannel};
            if (outletInfoMap.find(key) != outletInfoMap.end())
                continue;

            const juce::uint32 outletId = newLogicalId++;
            const int          pinIndex = outletPinIndexCounter++;
            const int          channelCount = 1;

            OutletInfo info{
                outletId,
                pinIndex,
                channelCount,
                bc.externalLogicalId,
                bc.externalChannel,
                bc.externalLogicalId == 0};
            outletInfoMap[key] = info;
            outletInfoByLogical.emplace(outletId, info);

            juce::ValueTree mv("module");
            mv.setProperty("logicalId", (int)outletId, nullptr);
            mv.setProperty("type", "outlet", nullptr);

            juce::ValueTree paramsWrapper("params");
            paramsWrapper.addChild(
                createParameterState(OutletModuleProcessor::paramIdChannelCount, channelCount),
                -1,
                nullptr);
            mv.addChild(paramsWrapper, -1, nullptr);

            juce::ValueTree extra("OutletState");
            juce::String    outletLabel;
            if (bc.externalLogicalId == 0)
            {
                outletLabel = "Main Output :: Ch " + juce::String(bc.externalChannel + 1);
            }
            else if (auto* dstModule = synth->getModuleForLogical(bc.externalLogicalId))
            {
                outletLabel = dstModule->getName();
                const juce::String channelLabel = dstModule->getAudioInputLabel(bc.externalChannel);
                if (channelLabel.isNotEmpty())
                    outletLabel += " :: " + channelLabel;
                else
                    outletLabel += " :: In " + juce::String(bc.externalChannel + 1);
            }
            else
            {
                outletLabel = "Out " + juce::String(pinIndex + 1);
            }
            extra.setProperty("customLabel", outletLabel, nullptr);
            extra.setProperty("pinIndex", pinIndex, nullptr);
            extra.setProperty("externalLogicalId", (int)bc.externalLogicalId, nullptr);
            extra.setProperty("externalChannel", bc.externalChannel, nullptr);
            extra.setProperty("externalIsOutput", bc.externalLogicalId == 0, nullptr);
            juce::ValueTree extraWrapper("extra");
            extraWrapper.addChild(extra, -1, nullptr);
            mv.addChild(extraWrapper, -1, nullptr);

            modsVT.addChild(mv, -1, nullptr);
            juce::Logger::writeToLog(
                "[Meta Module] Created outlet node ID=" + juce::String(outletId));
        }

        // Add internal connections (between selected nodes)
        for (const auto& conn : allConnections)
        {
            bool srcIsSelected = selectedLogicalIds.count(conn.srcLogicalId) > 0;
            bool dstIsSelected = selectedLogicalIds.count(conn.dstLogicalId) > 0;

            if (srcIsSelected && dstIsSelected)
            {
                juce::ValueTree cv("connection");
                cv.setProperty("srcId", (int)oldToNewLogicalId[conn.srcLogicalId], nullptr);
                cv.setProperty("srcChan", conn.srcChan, nullptr);
                cv.setProperty("dstId", (int)oldToNewLogicalId[conn.dstLogicalId], nullptr);
                cv.setProperty("dstChan", conn.dstChan, nullptr);
                connsVT.addChild(cv, -1, nullptr);
            }
        }

        // Add connections from inlets to internal nodes
        for (const auto& bc : boundaries)
        {
            if (bc.isInput)
            {
                InletKey key{bc.externalLogicalId, bc.externalChannel};
                auto     it = inletInfoMap.find(key);
                if (it == inletInfoMap.end())
                    continue;
                juce::uint32 inletId = it->second.logicalId;

                juce::ValueTree cv("connection");
                cv.setProperty("srcId", (int)inletId, nullptr);
                cv.setProperty("srcChan", 0, nullptr); // Inlets output on channel 0
                cv.setProperty("dstId", (int)oldToNewLogicalId[bc.internalLogicalId], nullptr);
                cv.setProperty("dstChan", bc.internalChannel, nullptr);
                connsVT.addChild(cv, -1, nullptr);
            }
        }

        // Add connections from internal nodes to outlets
        for (const auto& bc : boundaries)
        {
            if (!bc.isInput)
            {
                OutletKey key{bc.internalLogicalId, bc.internalChannel};
                auto      it = outletInfoMap.find(key);
                if (it == outletInfoMap.end())
                    continue;
                juce::uint32 outletId = it->second.logicalId;

                juce::ValueTree cv("connection");
                cv.setProperty("srcId", (int)oldToNewLogicalId[bc.internalLogicalId], nullptr);
                cv.setProperty("srcChan", bc.internalChannel, nullptr);
                cv.setProperty("dstId", (int)outletId, nullptr);
                cv.setProperty("dstChan", 0, nullptr); // Outlets input on channel 0
                connsVT.addChild(cv, -1, nullptr);
            }
        }

        internalRoot.addChild(modsVT, -1, nullptr);
        internalRoot.addChild(connsVT, -1, nullptr);

        // Serialize to memory block
        if (auto xml = internalRoot.createXml())
        {
            juce::MemoryOutputStream mos(internalState, false);
            xml->writeTo(mos);
            juce::Logger::writeToLog("[META] Generated state for sub-patch.");
        }
    }

    // 4. Calculate average position for the meta module
    ImVec2 avgPos(0.0f, 0.0f);
    int    posCount = 0;
    for (juce::uint32 logicalId : selectedLogicalIds)
    {
        ImVec2 pos = ImNodes::GetNodeGridSpacePos((int)logicalId);
        avgPos.x += pos.x;
        avgPos.y += pos.y;
        posCount++;
    }
    if (posCount > 0)
    {
        avgPos.x /= posCount;
        avgPos.y /= posCount;
    }

    // 5. Delete selected nodes
    for (juce::uint32 logicalId : selectedLogicalIds)
    {
        auto nodeId = synth->getNodeIdForLogical(logicalId);
        synth->removeModule(nodeId);
    }

    // 6. Create meta module
    auto metaNodeId = synth->addModule("meta_module");
    auto metaLogicalId = synth->getLogicalIdForNode(metaNodeId);
    pendingNodePositions[(int)metaLogicalId] = avgPos;

    juce::Logger::writeToLog(
        "[META] Created new MetaModule with logical ID: " + juce::String((int)metaLogicalId));
    auto* metaModule =
        dynamic_cast<MetaModuleProcessor*>(synth->getModuleForLogical(metaLogicalId));
    if (metaModule)
    {
        juce::ValueTree metaState("MetaModuleState");
        metaState.setProperty("label", "Meta Module", nullptr);

        if (internalState.getSize() > 0)
        {
            juce::MemoryOutputStream base64Stream;
            juce::Base64::convertToBase64(
                base64Stream, internalState.getData(), internalState.getSize());
            metaState.setProperty("internalGraphState", base64Stream.toString(), nullptr);
        }

        metaModule->setExtraStateTree(metaState);
        juce::Logger::writeToLog("[META] Loaded internal state into meta module");
    }
    else
    {
        juce::Logger::writeToLog("[META] ERROR: Failed to create meta module");
        return;
    }
    // 7. Reconnect external connections
    auto sortedInlets = metaModule->getInletNodes();
    std::sort(sortedInlets.begin(), sortedInlets.end(), [](auto* a, auto* b) {
        if (a->getPinIndex() != b->getPinIndex())
            return a->getPinIndex() < b->getPinIndex();
        return a->getLogicalId() < b->getLogicalId();
    });
    std::unordered_map<int, int> inletBaseChannels;
    std::unordered_map<int, int> inletChannelCounts;
    int                          runningInputChannel = 0;
    for (auto* inlet : sortedInlets)
    {
        const int pinIndex = inlet->getPinIndex();
        int       channelCount = 1;
        if (auto* param = dynamic_cast<juce::AudioParameterInt*>(
                inlet->getAPVTS().getParameter(InletModuleProcessor::paramIdChannelCount)))
        {
            channelCount = juce::jmax(1, param->get());
        }
        if (auto logicalIt = inletInfoByLogical.find(inlet->getLogicalId());
            logicalIt != inletInfoByLogical.end())
        {
            inlet->setExternalMapping(
                logicalIt->second.externalLogicalId, logicalIt->second.externalChannel);
        }
        inletBaseChannels[pinIndex] = runningInputChannel;
        inletChannelCounts[pinIndex] = channelCount;
        runningInputChannel += channelCount;
    }

    auto sortedOutlets = metaModule->getOutletNodes();
    std::sort(sortedOutlets.begin(), sortedOutlets.end(), [](auto* a, auto* b) {
        if (a->getPinIndex() != b->getPinIndex())
            return a->getPinIndex() < b->getPinIndex();
        return a->getLogicalId() < b->getLogicalId();
    });

    std::unordered_map<int, int> outletBaseChannels;
    std::unordered_map<int, int> outletChannelCounts;
    int                          runningOutputChannel = 0;
    for (auto* outlet : sortedOutlets)
    {
        const int pinIndex = outlet->getPinIndex();
        int       channelCount = 1;
        if (auto* param = dynamic_cast<juce::AudioParameterInt*>(
                outlet->getAPVTS().getParameter(OutletModuleProcessor::paramIdChannelCount)))
        {
            channelCount = juce::jmax(1, param->get());
        }
        if (auto logicalIt = outletInfoByLogical.find(outlet->getLogicalId());
            logicalIt != outletInfoByLogical.end())
        {
            outlet->setExternalMapping(
                logicalIt->second.externalLogicalId,
                logicalIt->second.externalChannel,
                logicalIt->second.externalIsOutput);
        }
        outletBaseChannels[pinIndex] = runningOutputChannel;
        outletChannelCounts[pinIndex] = channelCount;
        runningOutputChannel += channelCount;
    }

    // Connect unique external sources to meta inputs
    for (const auto& entry : inletInfoMap)
    {
        const InletKey&  key = entry.first;
        const InletInfo& info = entry.second;

        auto extNodeId = synth->getNodeIdForLogical(key.first);
        if (extNodeId.uid == 0)
            continue;

        auto baseIt = inletBaseChannels.find(info.pinIndex);
        auto countIt = inletChannelCounts.find(info.pinIndex);
        if (baseIt == inletBaseChannels.end() || countIt == inletChannelCounts.end())
            continue;

        const int baseChannel = baseIt->second;
        const int channelCount = countIt->second;

        for (int ch = 0; ch < channelCount; ++ch)
        {
            synth->connect(extNodeId, key.second + ch, metaNodeId, baseChannel + ch);
        }
    }

    const auto outputNodeId = synth->getOutputNodeID();

    // Reconnect meta outputs to their original destinations
    for (const auto& bc : boundaries)
    {
        if (bc.isInput)
            continue;

        OutletKey key{bc.internalLogicalId, bc.internalChannel};
        auto      infoIt = outletInfoMap.find(key);
        if (infoIt == outletInfoMap.end())
            continue;

        const OutletInfo& info = infoIt->second;
        auto              destNodeId = (bc.externalLogicalId == 0)
                                           ? outputNodeId
                                           : synth->getNodeIdForLogical(bc.externalLogicalId);

        if (destNodeId.uid == 0)
            continue;

        auto baseIt = outletBaseChannels.find(info.pinIndex);
        auto countIt = outletChannelCounts.find(info.pinIndex);
        if (baseIt == outletBaseChannels.end() || countIt == outletChannelCounts.end())
            continue;

        const int baseChannel = baseIt->second;
        const int channelCount = countIt->second;

        for (int ch = 0; ch < channelCount; ++ch)
        {
            synth->connect(metaNodeId, baseChannel + ch, destNodeId, bc.externalChannel + ch);
        }
    }

    graphNeedsRebuild = true;
    synth->commitChanges();

    juce::Logger::writeToLog("[META] Reconnected external cables. Collapse complete!");
    NotificationManager::post(NotificationManager::Type::Info, "Collapsed to Meta Module");
}

void ImGuiNodeEditorComponent::populateDragInsertSuggestions()
{
    dragInsertSuggestionsInputs.clear();
    dragInsertSuggestionsOutputs.clear();

    const auto& pinDb = getModulePinDatabase();

    auto addUnique = [](auto& mapRef, PinDataType type, const juce::String& moduleType) {
        auto& modules = mapRef[type];
        if (std::find(modules.begin(), modules.end(), moduleType) == modules.end())
            modules.push_back(moduleType);
    };

    auto addInputModule = [&](PinDataType type, const juce::String& moduleType) {
        addUnique(dragInsertSuggestionsInputs, type, moduleType);
    };

    auto addOutputModule = [&](PinDataType type, const juce::String& moduleType) {
        addUnique(dragInsertSuggestionsOutputs, type, moduleType);
    };

    // Seed curated utilities for fast access when connecting FROM outputs (needs inputs).
    addInputModule(PinDataType::Audio, "attenuverter");
    addInputModule(PinDataType::Audio, "comparator");
    addInputModule(PinDataType::Audio, "mixer");

    addInputModule(PinDataType::CV, "attenuverter");
    addInputModule(PinDataType::CV, "lag_processor");
    addInputModule(PinDataType::CV, "math");

    addInputModule(PinDataType::Gate, "comparator");
    addInputModule(PinDataType::Gate, "logic");
    addInputModule(PinDataType::Gate, "sequential_switch");

    addInputModule(PinDataType::Raw, "map_range");
    addInputModule(PinDataType::Raw, "scope");

#ifndef AUDIO_ONLY_BUILD
    addInputModule(PinDataType::Video, "video_fx");
    addInputModule(PinDataType::Video, "video_draw_impact");
    addInputModule(PinDataType::Video, "crop_video");
    addInputModule(PinDataType::Video, "video_viewer");
#endif

    // Seed curated sources for fast access when connecting INTO inputs (needs outputs).
    addOutputModule(PinDataType::Audio, "vco");
    addOutputModule(PinDataType::Audio, "polyvco");
    addOutputModule(PinDataType::Audio, "noise");
    addOutputModule(PinDataType::Audio, "sample_loader");
    addOutputModule(PinDataType::Audio, "midi_player");

    addOutputModule(PinDataType::CV, "lfo");
    addOutputModule(PinDataType::CV, "adsr");
    addOutputModule(PinDataType::CV, "function_generator");
    addOutputModule(PinDataType::CV, "value");

    addOutputModule(PinDataType::Gate, "adsr");
    addOutputModule(PinDataType::Gate, "random");

    addOutputModule(PinDataType::Raw, "value");

#ifndef AUDIO_ONLY_BUILD
    addOutputModule(PinDataType::Video, "webcam_loader");
    addOutputModule(PinDataType::Video, "video_file_loader");
    addOutputModule(PinDataType::Video, "chromakey");
#endif

    std::vector<PinDataType> types = {
        PinDataType::Audio, PinDataType::CV, PinDataType::Gate, PinDataType::Raw};
#ifndef AUDIO_ONLY_BUILD
    types.push_back(PinDataType::Video);
#endif
    for (auto type : types)
    {
        addInputModule(type, "reroute");
        addOutputModule(type, "reroute");
    }

    for (const auto& entry : pinDb)
    {
        const juce::String& moduleType = entry.first;
        const auto&         info = entry.second;

        for (const auto& pin : info.audioIns)
            addInputModule(pin.type, moduleType);

        for (const auto& pin : info.modIns)
            addInputModule(pin.type, moduleType);

        for (const auto& pin : info.audioOuts)
            addOutputModule(pin.type, moduleType);
    }

    auto sortMapVectors = [](auto& mapRef) {
        for (auto& entry : mapRef)
        {
            auto& modules = entry.second;
            std::sort(
                modules.begin(), modules.end(), [](const juce::String& a, const juce::String& b) {
                    return a.compareIgnoreCase(b) < 0;
                });
        }
    };

    sortMapVectors(dragInsertSuggestionsInputs);
    sortMapVectors(dragInsertSuggestionsOutputs);
}

const std::vector<juce::String>& ImGuiNodeEditorComponent::getDragInsertSuggestionsFor(
    const PinID& pin) const
{
    PinID       localPin = pin;
    PinDataType type =
        localPin.isMod
            ? PinDataType::CV
            : const_cast<ImGuiNodeEditorComponent*>(this)->getPinDataTypeForPin(localPin);

    const auto& sourceMap =
        localPin.isInput ? dragInsertSuggestionsOutputs : dragInsertSuggestionsInputs;
    if (auto it = sourceMap.find(type); it != sourceMap.end())
        return it->second;

    static const std::vector<juce::String> empty;
    return empty;
}
void ImGuiNodeEditorComponent::insertNodeFromDragSelection(const juce::String& moduleType)
{
    if (synth == nullptr || dragInsertStartAttrId == -1)
        return;

    auto newNodeId = synth->addModule(moduleType);
    auto newLogicalId = synth->getLogicalIdForNode(newNodeId);

    pendingNodeScreenPositions[(int)newLogicalId] = dragInsertDropPos;

    const PinDataType primaryType =
        dragInsertStartPin.isMod ? PinDataType::CV : getPinDataTypeForPin(dragInsertStartPin);

    juce::Logger::writeToLog(
        "[DragInsert] primaryType=" + juce::String(toString(primaryType)) +
        ", startPin: lid=" + juce::String((int)dragInsertStartPin.logicalId) +
        ", channel=" + juce::String(dragInsertStartPin.channel) +
        ", isInput=" + juce::String(dragInsertStartPin.isInput ? 1 : 0) +
        ", isMod=" + juce::String(dragInsertStartPin.isMod ? 1 : 0));

    auto getSortedPinsForType = [&](juce::uint32 logicalId, bool isInput) -> std::vector<AudioPin> {
        std::vector<AudioPin> pins;

        if (logicalId == 0)
        {
            if (primaryType == PinDataType::Audio)
            {
                pins.emplace_back("Main L", 0, PinDataType::Audio);
                pins.emplace_back("Main R", 1, PinDataType::Audio);
            }
            return pins;
        }

        pins = getPinsOfType(logicalId, isInput, primaryType);
        std::sort(pins.begin(), pins.end(), [](const AudioPin& a, const AudioPin& b) {
            return a.channel < b.channel;
        });
        return pins;
    };

    auto findChannelIndex = [](const std::vector<AudioPin>& pins, int channel) -> int {
        for (int i = 0; i < (int)pins.size(); ++i)
        {
            if (pins[(size_t)i].channel == channel)
                return i;
        }
        return -1;
    };

    auto logNoCompatiblePins = [&](const char* role) {
        juce::Logger::writeToLog(
            "[DragInsert] No compatible " + juce::String(toString(primaryType)) + " " +
            juce::String(role) + " found for '" + moduleType + "', skipping auto-wire.");
    };

    bool connected = false;
    if (!dragInsertStartPin.isMod)
    {
        if (!dragInsertStartPin.isInput)
        {
            auto srcNodeId = synth->getNodeIdForLogical(dragInsertStartPin.logicalId);
            if (srcNodeId.uid != 0)
            {
                const auto sourcePins = getSortedPinsForType(dragInsertStartPin.logicalId, false);
                const auto targetPins = getSortedPinsForType((juce::uint32)newLogicalId, true);

                juce::Logger::writeToLog(
                    "[DragInsert] sourcePins count=" + juce::String(sourcePins.size()));
                for (const auto& pin : sourcePins)
                {
                    juce::Logger::writeToLog(
                        "  source: " + pin.name + " ch=" + juce::String(pin.channel) +
                        " type=" + juce::String(toString(pin.type)));
                }
                juce::Logger::writeToLog(
                    "[DragInsert] targetPins count=" + juce::String(targetPins.size()));
                for (const auto& pin : targetPins)
                {
                    juce::Logger::writeToLog(
                        "  target: " + pin.name + " ch=" + juce::String(pin.channel) +
                        " type=" + juce::String(toString(pin.type)));
                }

                if (!sourcePins.empty() && !targetPins.empty())
                {
                    if (primaryType == PinDataType::Audio)
                    {
                        std::vector<int> sourceChannels;
                        sourceChannels.reserve(sourcePins.size());
                        for (const auto& pin : sourcePins)
                            sourceChannels.push_back(pin.channel);
                        if (sourceChannels.empty())
                            sourceChannels.push_back(dragInsertStartPin.channel);
                        if (sourceChannels.size() > 2)
                            sourceChannels.resize(2);

                        std::vector<int> targetChannels;
                        targetChannels.reserve(targetPins.size());
                        for (const auto& pin : targetPins)
                            targetChannels.push_back(pin.channel);
                        if (targetChannels.size() > 2)
                            targetChannels.resize(2);

                        std::set<std::pair<int, int>> madeConnections;
                        auto connectAudioPair = [&](int srcChan, int dstChan) {
                            if (srcChan < 0 || dstChan < 0)
                                return;
                            std::pair<int, int> key{srcChan, dstChan};
                            if (madeConnections.insert(key).second)
                            {
                                synth->connect(srcNodeId, srcChan, newNodeId, dstChan);
                                connected = true;
                            }
                        };

                        if (!sourceChannels.empty() && !targetChannels.empty())
                        {
                            const bool sourceStereo = sourceChannels.size() >= 2;
                            const bool targetStereo = targetChannels.size() >= 2;

                            if (!sourceStereo && targetStereo)
                            {
                                connectAudioPair(sourceChannels[0], targetChannels[0]);
                                connectAudioPair(sourceChannels[0], targetChannels[1]);
                            }
                            else if (sourceStereo && !targetStereo)
                            {
                                connectAudioPair(sourceChannels[0], targetChannels[0]);
                            }
                            else if (sourceStereo && targetStereo)
                            {
                                connectAudioPair(sourceChannels[0], targetChannels[0]);
                                connectAudioPair(sourceChannels[1], targetChannels[1]);
                            }
                            else
                            {
                                connectAudioPair(sourceChannels[0], targetChannels[0]);
                            }
                        }
                        if (!connected)
                            logNoCompatiblePins("input");
                    }
                    else
                    {
                        // For non-Audio types (Video, CV, Gate, etc.), match by actual channel
                        // number First, find the source pin that matches the drag start channel
                        int       sourceChannel = dragInsertStartPin.channel;
                        const int sourceIndex = findChannelIndex(sourcePins, sourceChannel);

                        if (sourceIndex >= 0 && sourceIndex < (int)sourcePins.size())
                        {
                            sourceChannel = sourcePins[(size_t)sourceIndex].channel;

                            // Try to find a target pin at the same channel number
                            int       targetChannel = -1;
                            const int targetIndexByChannel =
                                findChannelIndex(targetPins, sourceChannel);

                            if (targetIndexByChannel >= 0 &&
                                targetIndexByChannel < (int)targetPins.size())
                            {
                                // Found exact channel match
                                targetChannel = targetPins[(size_t)targetIndexByChannel].channel;
                            }
                            else if (!targetPins.empty())
                            {
                                // No exact match, use first available pin of the matching type
                                targetChannel = targetPins[0].channel;
                            }

                            if (targetChannel >= 0)
                            {
                                juce::Logger::writeToLog(
                                    "[DragInsert] Connecting: srcNodeId=" +
                                    juce::String((int)srcNodeId.uid) +
                                    " srcChannel=" + juce::String(sourceChannel) +
                                    " -> newNodeId=" + juce::String((int)newNodeId.uid) +
                                    " targetChannel=" + juce::String(targetChannel));
                                synth->connect(srcNodeId, sourceChannel, newNodeId, targetChannel);
                                connected = true;
                            }
                            else
                            {
                                juce::Logger::writeToLog(
                                    "[DragInsert] ERROR: targetChannel < 0, cannot connect");
                            }
                        }
                        else if (!sourcePins.empty() && !targetPins.empty())
                        {
                            // Fallback: use first available pins if channel lookup failed
                            synth->connect(
                                srcNodeId, sourcePins[0].channel, newNodeId, targetPins[0].channel);
                            connected = true;
                        }
                    }
                }
                else
                {
                    logNoCompatiblePins("input");
                }
            }
        }
        else
        {
            auto dstNodeId = dragInsertStartPin.logicalId == 0
                                 ? synth->getOutputNodeID()
                                 : synth->getNodeIdForLogical(dragInsertStartPin.logicalId);
            if (dstNodeId.uid != 0)
            {
                const auto sourcePins = getSortedPinsForType((juce::uint32)newLogicalId, false);
                const auto destinationPins =
                    getSortedPinsForType(dragInsertStartPin.logicalId, true);

                if (!sourcePins.empty() && !destinationPins.empty())
                {
                    const int destinationIndex =
                        findChannelIndex(destinationPins, dragInsertStartPin.channel);
                    if (destinationIndex >= 0 && destinationIndex < (int)destinationPins.size())
                    {
                        const int destinationChannel =
                            destinationPins[(size_t)destinationIndex].channel;

                        if (primaryType == PinDataType::Audio)
                        {
                            // Audio path: use array indices for stereo pairs
                            const int sourceIndex =
                                juce::jlimit(0, (int)sourcePins.size() - 1, destinationIndex);

                            synth->connect(
                                newNodeId,
                                sourcePins[(size_t)sourceIndex].channel,
                                dstNodeId,
                                destinationChannel);
                            connected = true;

                            const int stereoSourceIndex = sourceIndex + 1;
                            const int stereoDestinationIndex = destinationIndex + 1;

                            if (stereoSourceIndex < (int)sourcePins.size() &&
                                stereoDestinationIndex < (int)destinationPins.size())
                            {
                                synth->connect(
                                    newNodeId,
                                    sourcePins[(size_t)stereoSourceIndex].channel,
                                    dstNodeId,
                                    destinationPins[(size_t)stereoDestinationIndex].channel);
                            }
                        }
                        else
                        {
                            // Non-Audio path: match by actual channel number
                            int       sourceChannel = -1;
                            const int sourceIndexByChannel =
                                findChannelIndex(sourcePins, destinationChannel);

                            if (sourceIndexByChannel >= 0 &&
                                sourceIndexByChannel < (int)sourcePins.size())
                            {
                                // Found exact channel match
                                sourceChannel = sourcePins[(size_t)sourceIndexByChannel].channel;
                            }
                            else if (!sourcePins.empty())
                            {
                                // No exact match, use first available pin of the matching type
                                sourceChannel = sourcePins[0].channel;
                            }

                            if (sourceChannel >= 0)
                            {
                                synth->connect(
                                    newNodeId, sourceChannel, dstNodeId, destinationChannel);
                                connected = true;
                            }
                        }
                    }
                }
                else
                {
                    logNoCompatiblePins("output");
                }
            }
        }
    }

    synth->commitChanges();

    graphNeedsRebuild = true;
    pushSnapshot();

    juce::Logger::writeToLog(
        "[DragInsert] Added '" + moduleType + "' (LID " + juce::String((int)newLogicalId) + ")" +
        (connected ? " and auto-wired input." : "."));

    dragInsertStartAttrId = -1;
    dragInsertStartPin = PinID{};
    shouldOpenDragInsertPopup = false;
}

void ImGuiNodeEditorComponent::loadPresetFromFile(const juce::File& file)
{
    if (!file.existsAsFile() || synth == nullptr)
        return;

    // 1. Load the file content.
    juce::MemoryBlock mb;
    file.loadFileAsData(mb);

    // 2. Set the synthesizer's state. This rebuilds the audio graph.
    synth->setStateInformation(mb.getData(), (int)mb.getSize());

    // 3. Parse the XML to find the UI state.
    juce::ValueTree uiState;
    if (auto xml = juce::XmlDocument::parse(mb.toString()))
    {
        auto vt = juce::ValueTree::fromXml(*xml);
        uiState = vt.getChildWithName("NodeEditorUI");
        if (uiState.isValid())
        {
            // 4. Apply the UI state (node positions, muted status, etc.).
            // This queues the changes to be applied on the next frame.
            applyUiValueTree(uiState);
        }
    }

    // 5. Create an undo snapshot for this action.
    Snapshot s;
    synth->getStateInformation(s.synthState);
    s.uiState = uiState.isValid() ? uiState : getUiValueTree();
    undoStack.push_back(std::move(s));
    redoStack.clear();

    // 5. Update the UI status trackers.
    isPatchDirty = false;
    currentPresetFile = file; // Store full file path

    // No notification here; the calling function will handle it.
}
void ImGuiNodeEditorComponent::mergePresetFromFile(const juce::File& file, ImVec2 dropPosition)
{
    if (!file.existsAsFile() || synth == nullptr)
        return;

    auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr)
        return;

    juce::ValueTree preset = juce::ValueTree::fromXml(*xml);
    auto            modulesVT = preset.getChildWithName("modules");
    auto            connectionsVT = preset.getChildWithName("connections");
    auto            uiVT = preset.getChildWithName("NodeEditorUI");

    if (!modulesVT.isValid())
        return;

    pushSnapshot(); // Create an undo state before we start merging.

    // --- THIS IS THE NEW LOGIC ---
    // 1. Find the top-most Y coordinate of all existing nodes on the canvas.
    float topMostY = FLT_MAX;
    auto  currentUiState = getUiValueTree();
    bool  canvasHasNodes = false;
    for (int i = 0; i < currentUiState.getNumChildren(); ++i)
    {
        auto nodePosVT = currentUiState.getChild(i);
        if (nodePosVT.hasType("node"))
        {
            canvasHasNodes = true;
            float y = (float)nodePosVT.getProperty("y");
            if (y < topMostY)
            {
                topMostY = y;
            }
        }
    }
    // If the canvas is empty, use the drop position as the reference.
    if (!canvasHasNodes)
    {
        topMostY = dropPosition.y;
    }

    // 2. Find the bounding box of the nodes within the preset we are dropping.
    float presetMinX = FLT_MAX;
    float presetMaxY = -FLT_MAX;
    if (uiVT.isValid())
    {
        for (int i = 0; i < uiVT.getNumChildren(); ++i)
        {
            auto nodePosVT = uiVT.getChild(i);
            if (nodePosVT.hasType("node"))
            {
                float x = (float)nodePosVT.getProperty("x");
                float y = (float)nodePosVT.getProperty("y");
                if (x < presetMinX)
                    presetMinX = x;
                if (y > presetMaxY)
                    presetMaxY = y; // We need the lowest point (max Y) of the preset group.
            }
        }
    }

    // 3. Calculate the necessary offsets.
    const float verticalPadding = 100.0f;
    const float yOffset = topMostY - presetMaxY - verticalPadding;
    const float xOffset = dropPosition.x - presetMinX;
    // --- END OF NEW LOGIC ---

    // This map will track how we remap old IDs from the file to new, unique IDs on the canvas.
    std::map<juce::uint32, juce::uint32> oldIdToNewId;

    // First pass: create all the new modules from the preset.
    for (int i = 0; i < modulesVT.getNumChildren(); ++i)
    {
        auto moduleNode = modulesVT.getChild(i);
        if (moduleNode.hasType("module"))
        {
            juce::uint32 oldLogicalId = (juce::uint32)(int)moduleNode.getProperty("logicalId");
            juce::String type = moduleNode.getProperty("type").toString();

            // Add the module without committing the graph changes yet.
            auto         newNodeId = synth->addModule(type, false);
            juce::uint32 newLogicalId = synth->getLogicalIdForNode(newNodeId);

            oldIdToNewId[oldLogicalId] = newLogicalId; // Store the mapping

            // Restore the new module's parameters and extra state.
            if (auto* proc = synth->getModuleForLogical(newLogicalId))
            {
                auto paramsWrapper = moduleNode.getChildWithName("params");
                if (paramsWrapper.isValid())
                    proc->getAPVTS().replaceState(paramsWrapper.getChild(0));

                auto extraWrapper = moduleNode.getChildWithName("extra");
                if (extraWrapper.isValid())
                    proc->setExtraStateTree(extraWrapper.getChild(0));
            }
        }
    }

    // Second pass: recreate the internal connections between the new modules.
    if (connectionsVT.isValid())
    {
        for (int i = 0; i < connectionsVT.getNumChildren(); ++i)
        {
            auto connNode = connectionsVT.getChild(i);
            if (connNode.hasType("connection"))
            {
                juce::uint32 oldSrcId = (juce::uint32)(int)connNode.getProperty("srcId");
                int          srcChan = (int)connNode.getProperty("srcChan");
                juce::uint32 oldDstId = (juce::uint32)(int)connNode.getProperty("dstId");
                int          dstChan = (int)connNode.getProperty("dstChan");

                // Only connect if both source and destination are part of the preset we're merging.
                if (oldIdToNewId.count(oldSrcId) && oldIdToNewId.count(oldDstId))
                {
                    auto newSrcNodeId = synth->getNodeIdForLogical(oldIdToNewId[oldSrcId]);
                    auto newDstNodeId = synth->getNodeIdForLogical(oldIdToNewId[oldDstId]);
                    synth->connect(newSrcNodeId, srcChan, newDstNodeId, dstChan);
                }
            }
        }
    }

    // Third pass: Apply UI positions using our new calculated offsets.
    if (uiVT.isValid())
    {
        for (int i = 0; i < uiVT.getNumChildren(); ++i)
        {
            auto nodePosVT = uiVT.getChild(i);
            if (nodePosVT.hasType("node"))
            {
                juce::uint32 oldId = (juce::uint32)(int)nodePosVT.getProperty("id");
                if (oldIdToNewId.count(oldId)) // Check if it's one of our new nodes
                {
                    ImVec2 pos = ImVec2(
                        (float)nodePosVT.getProperty("x"), (float)nodePosVT.getProperty("y"));

                    // Apply the smart offsets
                    ImVec2 newPos = ImVec2(pos.x + xOffset, pos.y + yOffset);

                    pendingNodeScreenPositions[(int)oldIdToNewId[oldId]] = newPos;
                }
            }
        }
    }

    // Finally, commit all the changes to the audio graph at once.
    synth->commitChanges();
    isPatchDirty = true; // Mark the patch as edited.

    juce::Logger::writeToLog(
        "[Preset] Successfully merged preset: " + file.getFullPathName() +
        " above existing nodes with offsets (" + juce::String(xOffset) + ", " +
        juce::String(yOffset) + ")");
}
