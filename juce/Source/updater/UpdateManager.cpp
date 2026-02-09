#include "UpdateManager.h"
#include "HashVerifier.h"
#include "ui/UpdateAvailableDialog.h"
#include "ui/DownloadProgressDialog.h"
#include <juce_events/juce_events.h>
#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace Updater
{

// Manifest URL - update this to your OVH server
const juce::String MANIFEST_URL = "https://pimpant.club/pikon-raditsz/manifest.json";

UpdateManager::UpdateManager()
{
    // Initialize core components
    versionManager = std::make_unique<VersionManager>();
    updateChecker = std::make_unique<UpdateChecker>(MANIFEST_URL, *versionManager);
    fileDownloader = std::make_unique<FileDownloader>();
    updateApplier = std::make_unique<UpdateApplier>(*versionManager);

    // Setup ImGui dialog callbacks
    updateDownloadDialog.onStartDownload = [this](const juce::Array<FileInfo>& selectedFiles) {
        startDownload(selectedFiles);
    };
    updateDownloadDialog.onCancelDownload = [this]() { cancelDownload(); };
    updateDownloadDialog.onSkipVersion = [this]() { skipVersion(); };
    updateDownloadDialog.setVersionManager(versionManager.get());

    loadPreferences();

    // Don't automatically register the running executable on startup
    // This avoids loading installed_files.json and calculating expensive hashes
    // Registration will happen when user actually checks for updates
    juce::Logger::writeToLog(
        "=== UpdateManager initialized (lazy registration on update check) ===");
    DBG("=== UpdateManager initialized (lazy registration on update check) ===");
}

UpdateManager::~UpdateManager()
{
    savePreferences();
    savePreferences();
    // closeUpdateAvailableDialog();
    // closeDownloadProgressDialog();
}

juce::String UpdateManager::getManifestUrl() { return MANIFEST_URL; }

void UpdateManager::render() { updateDownloadDialog.render(); }

void UpdateManager::checkForUpdatesManual()
{
    if (isCheckingForUpdates)
    {
        DBG("Update check already in progress");
        return;
    }

    isCheckingForUpdates = true;
    DBG("Manual update check started");

    // Ensure running executable is registered before checking
    // (in case registerRunningExecutable hasn't run yet or failed)
    registerRunningExecutable();

    // Show dialog immediately in "checking" state for UX feedback
    updateDownloadDialog.showChecking();

    updateChecker->checkForUpdatesAsync([this](UpdateInfo info) {
        isCheckingForUpdates = false;
        onUpdateCheckComplete(info);
    });
}

void UpdateManager::checkForUpdatesAutomatic(int delaySeconds)
{
    if (!getAutoCheckEnabled())
    {
        DBG("Automatic update check disabled");
        return;
    }

    if (isCheckingForUpdates)
    {
        DBG("Update check already in progress");
        return;
    }

    // Delay the check
    juce::Timer::callAfterDelay(delaySeconds * 1000, [this]() {
        isCheckingForUpdates = true;
        DBG("Automatic update check started");

        // Ensure running executable is registered before checking
        registerRunningExecutable();

        updateChecker->checkForUpdatesAsync([this](UpdateInfo info) {
            isCheckingForUpdates = false;

            // Only show dialog if update is available
            if (info.updateAvailable)
                onUpdateCheckComplete(info);
            else
                DBG("No updates available (automatic check)");
        });
    });
}

void UpdateManager::onUpdateCheckComplete(UpdateInfo info)
{
    currentUpdateInfo = info;

    // Always show the dialog so user can see status
    showUpdateAvailableDialog(info);

    // Now that the latest manifest has been fetched and cached by UpdateChecker,
    // register the running executable so VersionManager has an up-to-date record.
    // This allows the hash column in the update dialog to show the correct local
    // hash for Pikon Raditsz.exe even on first run after an update.
    registerRunningExecutable();
}

void UpdateManager::showUpdateAvailableDialog(const UpdateInfo& info)
{
    // Use ImGui dialog instead of native window
    updateDownloadDialog.open(info);

    /* Native dialog code disabled
    closeUpdateAvailableDialog();

    auto* dialog = new UpdateAvailableDialog(info);
    // ... (rest of native code)
    updateAvailableWindow->setResizable(false, false);
    */
}

void UpdateManager::showDownloadProgressDialog()
{
    // Disabled for ImGui
}

void UpdateManager::startDownload()
{
    // Legacy helper: start download for all pending files
    startDownload(currentUpdateInfo.filesToDownload);
}

void UpdateManager::startDownload(const juce::Array<FileInfo>& selectedFiles)
{
    if (isDownloading)
    {
        DBG("Download already in progress");
        return;
    }

    if (selectedFiles.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "No Files Selected",
            "No files were selected for update. Please select at least one file.",
            "OK");
        return;
    }

    isDownloading = true;
    updateDownloadDialog.setDownloading(true);

    // Remember selection for this run
    selectedFilesForCurrentRun = selectedFiles;
    requiresRestartForCurrentRun = false;
    for (const auto& f : selectedFilesForCurrentRun)
    {
        if (f.critical)
        {
            requiresRestartForCurrentRun = true;
            break;
        }
    }

    auto tempDir = getTempDirectory();
    tempDir.createDirectory();

    fileDownloader->downloadFiles(
        selectedFilesForCurrentRun,
        tempDir,
        [this](DownloadProgress progress) { onDownloadProgress(progress); },
        [this](bool success, juce::String error) { onDownloadComplete(success, error); });
}

void UpdateManager::onDownloadProgress(DownloadProgress progress)
{
    updateDownloadDialog.setDownloadProgress(progress);

    /* Native dialog code disabled
    if (downloadProgressWindow != nullptr)
    {
        if (auto* dialog = dynamic_cast<DownloadProgressDialog*>(
                downloadProgressWindow->getContentComponent()))
        {
            dialog->setProgress(progress);
        }
    }
    */
}

void UpdateManager::onDownloadComplete(bool success, juce::String error)
{
    isDownloading = false;
    updateDownloadDialog.setDownloading(false);

    // Get successfully downloaded files (even if some failed)
    auto successfulDownloads = fileDownloader->getSuccessfulFiles();

    if (successfulDownloads.isEmpty())
    {
        // No files downloaded successfully
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Download Failed",
            "Failed to download any files:\n\n" + error,
            "OK");
        return;
    }

    // Show warning if some files failed to download
    if (!success && !error.isEmpty())
    {
        juce::String warning = "Some files failed to download, but continuing with " +
                               juce::String(successfulDownloads.size()) +
                               " successful download(s):\n\n" + error;
        juce::Logger::writeToLog(warning);
    }

    // Apply only the successfully downloaded files
    auto tempDir = getTempDirectory();

    juce::Array<juce::String> failedApplies;
    juce::Array<FileInfo>     successfulApplies;

    bool applied = updateApplier->applyUpdates(
        successfulDownloads, // Only apply files that downloaded successfully
        tempDir,
        currentUpdateInfo.requiresRestart ? UpdateApplier::UpdateType::OnRestart
                                          : UpdateApplier::UpdateType::Immediate,
        &failedApplies,
        &successfulApplies);

    // Build summary message
    juce::String summary;
    const int    totalRequested = selectedFilesForCurrentRun.isEmpty()
                                      ? currentUpdateInfo.filesToDownload.size()
                                      : selectedFilesForCurrentRun.size();

    if (successfulApplies.size() == successfulDownloads.size() && failedApplies.isEmpty())
    {
        // All successful downloads were applied
        summary = "Update completed successfully!\n\n";
        summary += juce::String(successfulApplies.size()) + " file(s) updated.";
    }
    else
    {
        // Partial success
        summary = "Update completed with some issues:\n\n";
        summary += "Successfully updated: " + juce::String(successfulApplies.size()) + " file(s)\n";
        if (!failedApplies.isEmpty())
            summary += "Failed to apply: " + juce::String(failedApplies.size()) + " file(s)\n";
        if (successfulDownloads.size() < totalRequested)
            summary +=
                "Failed to download: " + juce::String(totalRequested - successfulDownloads.size()) +
                " file(s)\n";
    }

    if (!applied && successfulApplies.isEmpty())
    {
        // Nothing was applied
        DBG("Failed to apply any updates");
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Update Failed",
            "Downloaded files but failed to apply any updates:\n\n" + error,
            "OK");
        return;
    }

    // Update version info (only if at least one file was applied)
    if (!successfulApplies.isEmpty())
    {
        versionManager->setCurrentVersion(currentUpdateInfo.newVersion);
        DBG("Updates applied: " + juce::String(successfulApplies.size()) + " file(s)");
    }

    // If update requires restart (EXE or other critical file was updated), launch PikonUpdater.exe
    if (requiresRestartForCurrentRun && !successfulApplies.isEmpty())
    {
        DBG("Update requires restart - launching PikonUpdater.exe");

        // Create update manifest for PikonUpdater using only successfully downloaded files
        auto updateManifest = createUpdateManifest(successfulDownloads, tempDir);

        // Get path to PikonUpdater.exe (shipped with app)
        auto updaterPath = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                               .getParentDirectory()
                               .getChildFile("PikonUpdater.exe");

        if (!updaterPath.existsAsFile())
        {
            DBG("PikonUpdater.exe not found!");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Update Error",
                "Updater tool not found. Please reinstall the application.",
                "OK");
            return;
        }

        // Get current process ID
        auto currentPID = GetCurrentProcessId();

        // Build command line
        juce::String cmdLine;
        cmdLine << "\"" << updaterPath.getFullPathName() << "\" ";
        cmdLine << "--source \"" << tempDir.getFullPathName() << "\" ";
        cmdLine << "--dest \"" << getInstallDirectory().getFullPathName() << "\" ";
        cmdLine << "--manifest \"" << updateManifest.getFullPathName() << "\" ";
        cmdLine << "--relaunch \""
                << juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFileName()
                << "\" ";
        cmdLine << "--wait-pid " << juce::String(currentPID);

        DBG("Launching updater: " + cmdLine);

        // Show message with summary
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Update Complete - Restarting",
            summary + "\n\nThe application will now restart to complete the update.",
            "OK");

        // Launch updater and quit after short delay
        juce::Timer::callAfterDelay(1000, [this, updaterPath, cmdLine]() {
            if (updaterPath.startAsProcess(cmdLine))
            {
                DBG("PikonUpdater launched successfully");
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }
            else
            {
                DBG("Failed to launch PikonUpdater");
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Update Error",
                    "Failed to launch updater. Please restart manually.",
                    "OK");
            }
        });
    }
    else
    {
        // Non-critical files updated - show success message with summary
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "Update Complete", summary, "OK");
    }
}

void UpdateManager::cancelDownload()
{
    if (fileDownloader)
        fileDownloader->cancelDownload();

    isDownloading = false;
}

void UpdateManager::skipVersion()
{
    skippedVersion = currentUpdateInfo.newVersion;
    savePreferences();
    DBG("Skipped version: " + skippedVersion);
}

void UpdateManager::restartApplication()
{
    // Save preferences before restarting
    savePreferences();

    // Quit the application
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void UpdateManager::closeUpdateAvailableDialog() { /* updateAvailableWindow.reset(); */ }

void UpdateManager::closeDownloadProgressDialog() { /* downloadProgressWindow.reset(); */ }

juce::File UpdateManager::getTempDirectory()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("PikonRaditszUpdates");
}

bool UpdateManager::getAutoCheckEnabled() const
{
    auto* props = const_cast<UpdateManager*>(this)->getPropertiesFile();
    if (props != nullptr)
        return props->getBoolValue("autoCheckForUpdates", true);
    return true;
}

void UpdateManager::setAutoCheckEnabled(bool enabled)
{
    if (auto* props = getPropertiesFile())
    {
        props->setValue("autoCheckForUpdates", enabled);
        savePreferences();
    }
}

void UpdateManager::loadPreferences()
{
    if (auto* props = getPropertiesFile())
    {
        skippedVersion = props->getValue("skippedVersion", "");
    }
}

void UpdateManager::savePreferences()
{
    if (auto* props = getPropertiesFile())
    {
        props->setValue("skippedVersion", skippedVersion);
        props->saveIfNeeded();
    }
}

juce::PropertiesFile* UpdateManager::getPropertiesFile()
{
    // Get the application's properties file
    if (auto* app = juce::JUCEApplication::getInstance())
    {
        // Try to get properties from PresetCreatorApplication
        // This is a bit hacky but works for now
        juce::PropertiesFile::Options options;
        options.applicationName = app->getApplicationName();
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";

        static std::unique_ptr<juce::PropertiesFile> props;
        if (props == nullptr)
            props = std::make_unique<juce::PropertiesFile>(options);

        return props.get();
    }

    return nullptr;
}

// ============================================================================
// Phase 1: Hash Verification & Manifest Caching
// ============================================================================

juce::String UpdateManager::getCachedManifest()
{
    auto cacheFile =
        versionManager->getVersionFile().getParentDirectory().getChildFile("manifest_cache.json");

    if (cacheFile.existsAsFile())
    {
        // Check if cache is fresh (less than 1 hour old)
        auto age = juce::Time::getCurrentTime() - cacheFile.getLastModificationTime();
        if (age.inHours() < 1)
        {
            DBG("Using cached manifest (age: " + juce::String(age.inMinutes()) + " minutes)");
            return cacheFile.loadFileAsString();
        }
        else
        {
            DBG("Cached manifest too old (age: " + juce::String(age.inHours()) + " hours)");
        }
    }

    return juce::String();
}

void UpdateManager::cacheManifest(const juce::String& manifestJson)
{
    auto cacheFile =
        versionManager->getVersionFile().getParentDirectory().getChildFile("manifest_cache.json");

    cacheFile.getParentDirectory().createDirectory();
    cacheFile.replaceWithText(manifestJson);
    DBG("Manifest cached to: " + cacheFile.getFullPathName());
}

void UpdateManager::registerRunningExecutable()
{
    // OPTIMIZED: Use SIZE comparison instead of expensive hash calculation
    // This function is called on the UI thread, so we keep it fast

    auto exePath = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto exeName = exePath.getFileName();

    if (!exePath.existsAsFile())
        return;

    auto exeSize = exePath.getSize();

    // Check if already tracked
    if (versionManager->hasFile(exeName))
    {
        // Already tracked - no action needed
        DBG("registerRunningExecutable: " + exeName + " already tracked");
        return;
    }

    DBG("registerRunningExecutable: " + exeName + " not tracked, checking manifest...");

    // Try cached manifest to find EXE entry
    auto cachedManifest = getCachedManifest();

    if (cachedManifest.isEmpty())
    {
        // No cached manifest - will verify when manifest is fetched
        DBG("registerRunningExecutable: No cached manifest available");
        return;
    }

    try
    {
        auto json = juce::JSON::parse(cachedManifest);

        if (auto* obj = json.getDynamicObject())
        {
            auto currentVariant = versionManager->getCurrentVariant();
            auto variantsArray = obj->getProperty("variants");

            if (auto* variantsObj = variantsArray.getDynamicObject())
            {
                auto variantData = variantsObj->getProperty(currentVariant);

                if (auto* variantObj = variantData.getDynamicObject())
                {
                    auto filesObj = variantObj->getProperty("files");

                    if (auto* files = filesObj.getDynamicObject())
                    {
                        // Find EXE in manifest files
                        for (auto& prop : files->getProperties())
                        {
                            auto fileName = prop.name.toString();

                            // Check if this is the executable file (EXACT MATCH ONLY)
                            if (fileName.equalsIgnoreCase(exeName))
                            {
                                if (auto* fileObj = prop.value.getDynamicObject())
                                {
                                    juce::int64 manifestSize =
                                        (juce::int64)fileObj->getProperty("size");
                                    juce::String version =
                                        fileObj->getProperty("version").toString();
                                    juce::String manifestHash =
                                        fileObj->getProperty("sha256").toString();

                                    // Use SIZE comparison (fast) instead of hash
                                    if (exeSize == manifestSize)
                                    {
                                        // Size matches! Register as installed
                                        FileInfo info;
                                        info.relativePath = fileName;
                                        info.sha256 = manifestHash;
                                        info.version = version;
                                        info.size = manifestSize;
                                        info.critical = true;
                                        info.url = "";

                                        versionManager->updateFileRecord(fileName, info);
                                        versionManager->saveVersionInfo();
                                        DBG("registerRunningExecutable: Registered " + exeName +
                                            " (size match)");
                                        return;
                                    }
                                    else
                                    {
                                        DBG("registerRunningExecutable: Size mismatch - Local: " +
                                            juce::String(exeSize) +
                                            " Manifest: " + juce::String(manifestSize));
                                        return;
                                    }
                                }
                            }
                        }

                        DBG("registerRunningExecutable: EXE not found in manifest: " + exeName);
                    }
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        DBG("registerRunningExecutable: Error - " + juce::String(e.what()));
    }
}

juce::File UpdateManager::getInstallDirectory() const
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
}

juce::File UpdateManager::createUpdateManifest(
    const juce::Array<FileInfo>& files,
    const juce::File&            tempDir)
{
    auto manifestFile = tempDir.getChildFile("update_manifest.json");

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    juce::DynamicObject::Ptr filesObj = new juce::DynamicObject();

    for (const auto& file : files)
    {
        juce::DynamicObject::Ptr fileObj = new juce::DynamicObject();
        fileObj->setProperty("sha256", file.sha256);
        fileObj->setProperty("size", file.size);
        filesObj->setProperty(file.relativePath, juce::var(fileObj));
    }

    root->setProperty("files", juce::var(filesObj));

    juce::FileOutputStream output(manifestFile);
    if (output.openedOk())
    {
        juce::JSON::writeToStream(output, juce::var(root), true);
        DBG("Update manifest created: " + manifestFile.getFullPathName());
    }
    else
    {
        DBG("Failed to create update manifest");
    }

    return manifestFile;
}

} // namespace Updater
