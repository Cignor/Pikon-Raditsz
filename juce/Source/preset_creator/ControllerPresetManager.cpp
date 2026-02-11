#include "ControllerPresetManager.h"

ControllerPresetManager::ControllerPresetManager()
{
    // 1. Find or create the root directory for all controller presets.
    rootDirectory = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                        .getParentDirectory()
                        .getChildFile("MidiControllerPresets");

    if (!rootDirectory.exists())
        rootDirectory.createDirectory();

    // 2. Ensure subdirectories exist for each module type.
    getDirectoryForType(ModuleType::Faders);
    getDirectoryForType(ModuleType::Knobs);
    getDirectoryForType(ModuleType::Buttons);
    getDirectoryForType(ModuleType::JogWheel);
    getDirectoryForType(ModuleType::StrokeSequencer);
    getDirectoryForType(ModuleType::GraphicEQ);
    getDirectoryForType(ModuleType::MultiBandShaper);
    getDirectoryForType(ModuleType::FunctionGenerator);
    getDirectoryForType(ModuleType::PolymetricSlicer);

    // 3. Perform an initial scan to populate the cache.
    scanAllPresets();
}

const juce::StringArray& ControllerPresetManager::getPresetNamesFor(ModuleType type) const
{
    auto it = presetCache.find(type);
    if (it != presetCache.end())
        return it->second;
    return emptyArray;
}

juce::ValueTree ControllerPresetManager::loadPreset(ModuleType type, const juce::String& presetName)
{
    juce::File presetFile = getDirectoryForType(type).getChildFile(presetName + ".xml");
    if (presetFile.existsAsFile())
    {
        if (auto xml = juce::XmlDocument::parse(presetFile))
        {
            return juce::ValueTree::fromXml(*xml);
        }
    }
    return {};
}

bool ControllerPresetManager::savePreset(
    ModuleType             type,
    const juce::String&    presetName,
    const juce::ValueTree& dataToSave)
{
    if (presetName.isEmpty())
        return false;

    juce::File presetFile = getDirectoryForType(type).getChildFile(presetName + ".xml");

    if (auto xml = dataToSave.createXml())
    {
        if (xml->writeTo(presetFile))
        {
            scanAllPresets(); // Re-scan to update the cache with the new file.
            return true;
        }
    }
    return false;
}

bool ControllerPresetManager::deletePreset(ModuleType type, const juce::String& presetName)
{
    juce::File presetFile = getDirectoryForType(type).getChildFile(presetName + ".xml");
    if (presetFile.deleteFile())
    {
        scanAllPresets(); // Re-scan to update the cache.
        return true;
    }
    return false;
}

juce::File ControllerPresetManager::getDirectoryForType(ModuleType type)
{
    juce::String subfolderName;
    switch (type)
    {
    case ModuleType::Faders:
        subfolderName = "MidiFaders";
        break;
    case ModuleType::Knobs:
        subfolderName = "MidiKnobs";
        break;
    case ModuleType::Buttons:
        subfolderName = "MidiButtons";
        break;
    case ModuleType::JogWheel:
        subfolderName = "MidiJogWheel";
        break;
    case ModuleType::StrokeSequencer:
        subfolderName = "StrokeSequencer";
        break;
    case ModuleType::GraphicEQ:
        subfolderName = "GraphicEQ";
        break;
    case ModuleType::MultiBandShaper:
        subfolderName = "MultiBandShaper";
        break;
    case ModuleType::FunctionGenerator:
        subfolderName = "FunctionGenerator";
        break;
    case ModuleType::PolymetricSlicer:
        subfolderName = "PolymetricSlicer";
        break;
    }

    auto dir = rootDirectory.getChildFile(subfolderName);
    if (!dir.exists())
        dir.createDirectory();

    return dir;
}

void ControllerPresetManager::scanAllPresets()
{
    presetCache.clear();
    for (int i = 0; i <= (int)ModuleType::PolymetricSlicer; ++i)
    {
        auto type = (ModuleType)i;
        auto dir = getDirectoryForType(type);

        // Find all .xml files and get them as juce::File objects
        juce::Array<juce::File> presetFiles;
        dir.findChildFiles(presetFiles, juce::File::findFiles, false, "*.xml");

        // Create a StringArray of just the filenames without the extension
        juce::StringArray names;
        for (const auto& file : presetFiles)
        {
            names.add(file.getFileNameWithoutExtension());
        }

        presetCache[type] = names;
    }
}
