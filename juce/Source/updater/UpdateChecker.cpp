#include "UpdateChecker.h"
#include "HashVerifier.h"
#include <juce_core/juce_core.h>

namespace Updater
{

// Helper thread for async update checking
class UpdateCheckThread : public juce::Thread
{
public:
    UpdateCheckThread(UpdateChecker* checker, std::function<void(UpdateInfo)> callback)
        : juce::Thread("UpdateCheckThread"), checker(checker), callback(std::move(callback))
    {
    }

    void run() override
    {
        auto info = checker->checkForUpdates();

        // Call callback on message thread
        juce::MessageManager::callAsync([this, info]() {
            if (callback)
                callback(info);
        });
    }

private:
    UpdateChecker*                  checker;
    std::function<void(UpdateInfo)> callback;
};

UpdateChecker::UpdateChecker(const juce::String& manifestUrl, VersionManager& versionManager)
    : manifestUrl(manifestUrl), versionManager(versionManager)
{
}

UpdateChecker::~UpdateChecker() { cancelCheck(); }

void UpdateChecker::checkForUpdatesAsync(std::function<void(UpdateInfo)> callback)
{
    cancelCheck();

    shouldCancel = false;
    checkThread = std::make_unique<UpdateCheckThread>(this, std::move(callback));
    checkThread->startThread();
}

UpdateInfo UpdateChecker::checkForUpdates()
{
    UpdateInfo info;
    info.currentVersion = versionManager.getCurrentVersion();

    try
    {
        // Fetch manifest from server
        auto manifest = fetchManifest();

        if (shouldCancel)
            return info;

        // Compare versions
        info = compareVersions(manifest);
    }
    catch (const std::exception& e)
    {
        DBG("Update check failed: " + juce::String(e.what()));
        info.updateAvailable = false;
    }

    return info;
}

void UpdateChecker::cancelCheck()
{
    shouldCancel = true;

    if (checkThread != nullptr)
    {
        checkThread->stopThread(5000);
        checkThread.reset();
    }
}

UpdateManifest UpdateChecker::fetchManifest()
{
    DBG("Fetching manifest from: " + manifestUrl);

    juce::URL url(manifestUrl);

    // Set timeout
    url = url.withParameter("t", juce::String(juce::Time::getCurrentTime().toMilliseconds()));

    // Fetch with timeout
    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(10000)
            .withNumRedirectsToFollow(5));

    if (stream == nullptr)
    {
        throw std::runtime_error("Failed to connect to update server");
    }

    // Read response
    juce::String jsonString = stream->readEntireStreamAsString();

    if (jsonString.isEmpty())
    {
        throw std::runtime_error("Empty response from update server");
    }

    DBG("Manifest fetched successfully, size: " + juce::String(jsonString.length()) + " bytes");
    // Log first 500 chars of manifest for debugging
    DBG("Manifest content (start): " + jsonString.substring(0, 500));

    // Cache manifest for later use (e.g., registerRunningExecutable)
    cacheManifestLocally(jsonString);

    // Parse manifest
    auto manifest = UpdateManifest::fromJson(jsonString);
    DBG("Parsed updateUrl: " + manifest.updateUrl);

    if (manifest.updateUrl.isEmpty())
    {
        DBG("WARNING: updateUrl is empty in manifest!");
        juce::Logger::writeToLog("WARNING: updateUrl is empty in manifest!");
    }
    else
    {
        juce::Logger::writeToLog("Parsed updateUrl: " + manifest.updateUrl);
    }

    return manifest;
}

UpdateInfo UpdateChecker::compareVersions(const UpdateManifest& manifest)
{
    UpdateInfo info;
    info.currentVersion = versionManager.getCurrentVersion();
    info.newVersion = manifest.latestVersion;
    info.changelogUrl = manifest.changelogUrl;
    info.changelogSummary = manifest.changelogSummary;

    // Get variant info early to populate allRemoteFiles
    auto  currentVariant = versionManager.getCurrentVariant();
    auto* variant = manifest.getVariant(currentVariant);

    if (variant != nullptr)
    {
        info.allRemoteFiles = variant->files;
    }
    else
    {
        DBG("Variant not found in manifest: " + currentVariant);
    }

    // Compare versions
    int versionComparison = compareVersionStrings(info.currentVersion, info.newVersion);

    if (versionComparison > 0)
    {
        // Current version is newer (dev build?)
        DBG("Current version is newer than server. Current: " + info.currentVersion +
            ", Latest: " + info.newVersion);
        info.updateAvailable = false;
        return info;
    }

    // Update is available
    info.updateAvailable = true;
    DBG("Update available! Current: " + info.currentVersion + ", New: " + info.newVersion);

    if (variant == nullptr)
    {
        info.updateAvailable = false;
        return info;
    }

    // Get installed files
    const auto& installedFiles = versionManager.getInstalledFiles();

    // Get install directory
    auto installDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();

    juce::Logger::writeToLog("UpdateChecker: Install dir: " + installDir.getFullPathName());

    // Get running executable path for special handling
    auto runningExePath = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto runningExeName = runningExePath.getFileName();

    // Determine which files need updating
    for (const auto& fileInfo : variant->files)
    {
        bool needsUpdate = false;
        auto localFile = installDir.getChildFile(fileInfo.relativePath);
        bool isRunningExe =
            (fileInfo.relativePath.equalsIgnoreCase(runningExeName) ||
             (fileInfo.relativePath.endsWithIgnoreCase(".exe") && localFile == runningExePath));

        // Check if file exists locally
        if (!localFile.exists())
        {
            // File doesn't exist physically
            needsUpdate = true;
            // Only log first few missing files to avoid spam
            if (info.filesToDownload.size() < 5)
                juce::Logger::writeToLog(
                    "UpdateChecker: Missing file: " + localFile.getFullPathName());
        }
        else if (!installedFiles.contains(fileInfo.relativePath))
        {
            // File exists but NOT tracked - need to verify hash to be sure
            // Skip hash for running EXE (can't hash locked file)
            if (isRunningExe)
            {
                // For running EXE, use size as best-effort check
                auto localSize = localFile.getSize();
                if (localSize == fileInfo.size)
                {
                    // Size matches - register and assume correct for now
                    versionManager.updateFileRecord(fileInfo.relativePath, fileInfo);
                    needsUpdate = false;
                }
                else
                {
                    needsUpdate = true;
                    juce::Logger::writeToLog(
                        "UpdateChecker: Running EXE size mismatch: " + fileInfo.relativePath);
                }
            }
            else
            {
                // For other files, calculate hash to verify (this runs on background thread)
                auto localHash = HashVerifier::calculateSHA256(localFile);

                if (localHash.equalsIgnoreCase(fileInfo.sha256))
                {
                    // Hash matches - register and skip download
                    versionManager.updateFileRecord(fileInfo.relativePath, fileInfo);
                    needsUpdate = false;
                }
                else
                {
                    // Hash differs - needs update
                    needsUpdate = true;
                    if (info.filesToDownload.size() < 5)
                        juce::Logger::writeToLog(
                            "UpdateChecker: Hash mismatch for " + fileInfo.relativePath);
                }
            }
        }
        else
        {
            // File IS tracked - compare recorded hash with manifest hash
            const auto& installed = installedFiles[fileInfo.relativePath];

            if (installed.sha256.equalsIgnoreCase(fileInfo.sha256))
            {
                // Hash in our records matches manifest - file is up to date
                if (installed.version != fileInfo.version)
                {
                    // Version string changed but hash same - just update record
                    versionManager.updateFileRecord(fileInfo.relativePath, fileInfo);
                }
                needsUpdate = false;
            }
            else
            {
                // Hash in our records differs from manifest - needs update!
                needsUpdate = true;
                if (info.filesToDownload.size() < 5)
                    juce::Logger::writeToLog(
                        "UpdateChecker: File hash changed: " + fileInfo.relativePath +
                        " Recorded: " + installed.sha256.substring(0, 8) +
                        " Manifest: " + fileInfo.sha256.substring(0, 8));
            }
        }

        if (needsUpdate)
        {
            info.filesToDownload.add(fileInfo);
            info.totalDownloadSize += fileInfo.size;

            if (fileInfo.critical)
                info.requiresRestart = true;
        }
    }

    // Check for files that should be deleted (exist locally but not in manifest)
    for (auto it = installedFiles.begin(); it != installedFiles.end(); ++it)
    {
        bool foundInManifest = false;

        for (const auto& fileInfo : variant->files)
        {
            if (fileInfo.relativePath == it.getKey())
            {
                foundInManifest = true;
                break;
            }
        }

        if (!foundInManifest)
        {
            info.filesToDelete.add(it.getKey());
            DBG("File to delete: " + it.getKey());
        }
    }

    DBG("Files to download: " + juce::String(info.filesToDownload.size()));
    DBG("Files to delete: " + juce::String(info.filesToDelete.size()));
    DBG("Total download size: " + juce::String(info.totalDownloadSize / 1024 / 1024) + " MB");
    DBG("Requires restart: " + juce::String(info.requiresRestart ? "yes" : "no"));

    return info;
}

int UpdateChecker::compareVersionStrings(const juce::String& v1, const juce::String& v2)
{
    // Parse semantic version strings (e.g., "1.2.3")
    auto parts1 = juce::StringArray::fromTokens(v1, ".", "");
    auto parts2 = juce::StringArray::fromTokens(v2, ".", "");

    // Pad with zeros if needed
    while (parts1.size() < 3)
        parts1.add("0");
    while (parts2.size() < 3)
        parts2.add("0");

    // Compare each part
    for (int i = 0; i < 3; ++i)
    {
        int num1 = parts1[i].getIntValue();
        int num2 = parts2[i].getIntValue();

        if (num1 < num2)
            return -1; // v1 is older
        if (num1 > num2)
            return 1; // v1 is newer
    }

    return 0; // Versions are equal
}

void UpdateChecker::cacheManifestLocally(const juce::String& jsonString)
{
    auto cacheFile =
        versionManager.getVersionFile().getParentDirectory().getChildFile("manifest_cache.json");

    cacheFile.getParentDirectory().createDirectory();
    cacheFile.replaceWithText(jsonString);
    DBG("Manifest cached to: " + cacheFile.getFullPathName());
}

} // namespace Updater
