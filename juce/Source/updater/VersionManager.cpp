#include "VersionManager.h"
#include "../utils/VersionInfo.h"

namespace Updater
{

VersionManager::VersionManager()
    : currentVersion("0.9.0") // Default version
#if AUDIO_ONLY_BUILD
      ,
      currentVariant("audio") // Audio-only variant
#else
      ,
      currentVariant("cuda") // Default variant
#endif
      ,
      versionInfoLoaded(false) // Lazy load flag
{
    // Don't load version info during construction - lazy load it when first needed
    // This prevents blocking startup with file I/O
}

VersionManager::~VersionManager() { saveVersionInfo(); }

juce::String VersionManager::getCurrentVersion() const { return currentVersion; }

juce::String VersionManager::getCurrentVariant() const { return currentVariant; }

void VersionManager::ensureVersionInfoLoaded() const
{
    if (!versionInfoLoaded)
    {
        // Cast away const to load (we're just loading data, not modifying logical state)
        const_cast<VersionManager*>(this)->loadVersionInfo();
        versionInfoLoaded = true;
    }
}

const juce::HashMap<juce::String, InstalledFileInfo>& VersionManager::getInstalledFiles() const
{
    ensureVersionInfoLoaded();
    return installedFiles;
}

InstalledFileInfo VersionManager::getFileInfo(const juce::String& relativePath) const
{
    ensureVersionInfoLoaded();
    if (installedFiles.contains(relativePath))
        return installedFiles[relativePath];

    return InstalledFileInfo();
}

bool VersionManager::hasFile(const juce::String& relativePath) const
{
    ensureVersionInfoLoaded();
    return installedFiles.contains(relativePath);
}

void VersionManager::updateFileRecord(const juce::String& relativePath, const FileInfo& info)
{
    InstalledFileInfo installed;
    installed.version = info.version;
    installed.sha256 = info.sha256;
    installed.installedDate = juce::Time::getCurrentTime();

    installedFiles.set(relativePath, installed);
}

void VersionManager::removeFileRecord(const juce::String& relativePath)
{
    installedFiles.remove(relativePath);
}

void VersionManager::setCurrentVersion(const juce::String& version) { currentVersion = version; }

void VersionManager::setCurrentVariant(const juce::String& variant) { currentVariant = variant; }

juce::File VersionManager::getAppDataDirectory() const
{
    // Use JUCE's application data directory
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Pikon Raditsz");
}

juce::File VersionManager::getVersionFile() const
{
    return getAppDataDirectory().getChildFile("installed_files.json");
}

bool VersionManager::saveVersionInfo()
{
    auto versionFile = getVersionFile();

    // Create directory if it doesn't exist
    versionFile.getParentDirectory().createDirectory();

    // Build JSON structure
    auto* root = new juce::DynamicObject();
    root->setProperty("appVersion", currentVersion);
    root->setProperty("variant", currentVariant);
    root->setProperty("lastUpdateCheck", lastUpdateCheck.toISO8601(true));

    // Add installed files
    auto* filesObj = new juce::DynamicObject();
    for (auto it = installedFiles.begin(); it != installedFiles.end(); ++it)
    {
        filesObj->setProperty(it.getKey(), it.getValue().toJson());
    }
    root->setProperty("files", juce::var(filesObj));

    // Write to file
    juce::var json(root);
    auto      jsonString = juce::JSON::toString(json, true);

    return versionFile.replaceWithText(jsonString);
}

bool VersionManager::loadVersionInfo()
{
    // If already loaded, skip
    if (versionInfoLoaded)
        return true;

    auto versionFile = getVersionFile();

    if (!versionFile.existsAsFile())
    {
        juce::Logger::writeToLog(
            "VersionManager: installed_files.json doesn't exist yet: " +
            versionFile.getFullPathName());
        juce::Logger::writeToLog(
            "  This is normal for first run - file will be created when files are registered");
        currentVersion = VersionInfo::getFullVersionString(); // Show actual binary version in UI
        versionInfoLoaded = true; // Mark as loaded even if file doesn't exist
        return false;
    }

    juce::Logger::writeToLog(
        "VersionManager: Loading installed_files.json from: " + versionFile.getFullPathName());
    juce::Logger::writeToLog("  File size: " + juce::String(versionFile.getSize()) + " bytes");
    juce::Logger::writeToLog(
        "  Modified: " + versionFile.getLastModificationTime().toString(true, true, true, true));

    auto jsonString = versionFile.loadFileAsString();
    auto json = juce::JSON::parse(jsonString);

    if (auto* obj = json.getDynamicObject())
    {
        currentVersion = obj->getProperty("appVersion").toString();
        // Override with actual binary version so updater UI always shows running app version
        currentVersion = VersionInfo::getFullVersionString();
        // currentVariant = obj->getProperty("variant").toString(); // Do NOT overwrite variant from
        // file - trust the build config!

        auto dateStr = obj->getProperty("lastUpdateCheck").toString();
        if (dateStr.isNotEmpty())
            lastUpdateCheck = juce::Time::fromISO8601(dateStr);

        juce::Logger::writeToLog("  Loaded version: " + currentVersion);
        juce::Logger::writeToLog("  Loaded variant: " + currentVariant);

        // Load installed files
        if (auto filesObj = obj->getProperty("files"))
        {
            if (auto* filesDict = filesObj.getDynamicObject())
            {
                installedFiles.clear();

                for (auto& prop : filesDict->getProperties())
                {
                    auto fileInfo = InstalledFileInfo::fromJson(prop.value);
                    installedFiles.set(prop.name.toString(), fileInfo);
                }

                juce::Logger::writeToLog(
                    "  Loaded " + juce::String(installedFiles.size()) + " tracked files");

                // Log EXE if present
                auto exePath = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
                auto exeName = exePath.getFileName();
                if (installedFiles.contains(exeName))
                {
                    auto exeInfo = installedFiles[exeName];
                    juce::Logger::writeToLog("  EXE tracked: " + exeName);
                    juce::Logger::writeToLog("    Recorded hash: " + exeInfo.sha256);
                    juce::Logger::writeToLog("    Recorded version: " + exeInfo.version);
                    juce::Logger::writeToLog(
                        "    Installed date: " +
                        exeInfo.installedDate.toString(true, true, true, true));
                }
                else
                {
                    juce::Logger::writeToLog("  EXE NOT tracked: " + exeName);
                }
            }
        }

        versionInfoLoaded = true;
        return true;
    }

    juce::Logger::writeToLog("  ❌ Failed to parse installed_files.json");
    versionInfoLoaded = true; // Mark as loaded even on error to prevent retry loops
    return false;
}

} // namespace Updater
