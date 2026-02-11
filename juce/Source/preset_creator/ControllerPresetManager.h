#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <map>

// A global singleton to manage saving/loading MIDI controller mapping presets.
class ControllerPresetManager
{
public:
    // Defines the types of modules that can have controller presets.
    enum class ModuleType
    {
        Faders,
        Knobs,
        Buttons,
        JogWheel,
        StrokeSequencer,
        GraphicEQ,
        MultiBandShaper,
        FunctionGenerator,
        PolymetricSlicer
    };

    // Get the singleton instance of the manager.
    static ControllerPresetManager& get()
    {
        static ControllerPresetManager instance;
        return instance;
    }

    // Get the names of all saved presets for a specific module type.
    const juce::StringArray& getPresetNamesFor(ModuleType type) const;

    // Load a preset's data as a ValueTree.
    juce::ValueTree loadPreset(ModuleType type, const juce::String& presetName);

    // Save a ValueTree of mapping data to a preset file.
    bool savePreset(
        ModuleType             type,
        const juce::String&    presetName,
        const juce::ValueTree& dataToSave);

    // Delete a preset file.
    bool deletePreset(ModuleType type, const juce::String& presetName);

private:
    // Private constructor for singleton pattern.
    ControllerPresetManager();
    ~ControllerPresetManager() = default;

    // Helper to get the correct subdirectory for a module type.
    juce::File getDirectoryForType(ModuleType type);

    // Scans all subdirectories and populates the cache.
    void scanAllPresets();

    juce::File                              rootDirectory;
    std::map<ModuleType, juce::StringArray> presetCache;
    juce::StringArray                       emptyArray; // Used as a safe fallback
};
