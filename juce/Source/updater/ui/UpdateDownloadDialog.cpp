#include "UpdateDownloadDialog.h"
#include "../VersionManager.h"
#include <algorithm>
#include <vector>

namespace Updater
{

UpdateDownloadDialog::UpdateDownloadDialog() {}

void UpdateDownloadDialog::open(const UpdateInfo& info)
{
    updateInfo = info;
    isOpen = true;
    isDownloading = false;
    isChecking = false; // Done checking, show results
    // Reset filter
    searchFilter[0] = 0;

    // Initialise selection: all files that need an update are selected by default
    fileSelected.clear();
    fileSelected.resize((size_t)updateInfo.filesToDownload.size(), true);
}

void UpdateDownloadDialog::showChecking()
{
    isOpen = true;
    isChecking = true;
    isDownloading = false;
}

void UpdateDownloadDialog::setDownloadProgress(const DownloadProgress& progress)
{
    currentProgress = progress;
}

void UpdateDownloadDialog::render()
{
    if (!isOpen)
        return;

    // Set window size and position
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

    const char* windowTitle = isChecking ? "Checking for Updates..." : "Software Update Available";
    if (!ImGui::Begin(windowTitle, &isOpen, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    // Show checking state with centered message
    if (isChecking)
    {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 300) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.4f);

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Checking for updates, please wait...");

        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 150) * 0.5f);
        ImGui::TextDisabled("This may take a few seconds");

        ImGui::End();
        return;
    }

    // Header info
    if (updateInfo.updateAvailable)
    {
        ImGui::TextColored(
            ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "New Version Available: %s",
            updateInfo.newVersion.toRawUTF8());
        ImGui::SameLine();
        ImGui::TextDisabled("(Current: %s)", updateInfo.currentVersion.toRawUTF8());

        if (updateInfo.requiresRestart)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[Requires Restart]");
        }
    }
    else
    {
        ImGui::TextColored(
            ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
            "You are up to date! (Version %s)",
            updateInfo.currentVersion.toRawUTF8());
    }

    ImGui::Separator();

    // Search filter
    ImGui::Text("Search Files:");
    ImGui::SameLine();
    ImGui::PushItemWidth(300.0f);
    ImGui::InputText("##search", searchFilter, sizeof(searchFilter));
    ImGui::PopItemWidth();

    ImGui::Separator();

    // File list and controls split
    float footerHeight = 150.0f;
    ImGui::BeginChild("FileList", ImVec2(0, -footerHeight), false);
    renderFileList();
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("Controls", ImVec2(0, 0), false);
    renderControls();
    ImGui::EndChild();

    ImGui::End();
}

void UpdateDownloadDialog::renderFileList()
{
    const auto& filesToShow = updateInfo.allRemoteFiles;

    if (filesToShow.isEmpty())
    {
        ImGui::Text("No files found on server.");
        return;
    }

    // Filter logic
    std::vector<int> filteredIndices;
    juce::String     search(searchFilter);
    search = search.toLowerCase();

    for (int i = 0; i < filesToShow.size(); ++i)
    {
        const auto& file = filesToShow.getReference(i);
        if (search.isEmpty() || file.relativePath.toLowerCase().contains(search))
        {
            filteredIndices.push_back(i);
        }
    }

    // Sort: Pending files first, then installed files
    // Also prioritize critical files within each group
    std::sort(filteredIndices.begin(), filteredIndices.end(), [this, &filesToShow](int a, int b) {
        const auto& fileA = filesToShow.getReference(a);
        const auto& fileB = filesToShow.getReference(b);

        // Check if files need updating
        bool needsUpdateA = false;
        bool needsUpdateB = false;

        for (const auto& f : updateInfo.filesToDownload)
        {
            if (f.relativePath == fileA.relativePath)
                needsUpdateA = true;
            if (f.relativePath == fileB.relativePath)
                needsUpdateB = true;
        }

        // Pending files come first
        if (needsUpdateA != needsUpdateB)
            return needsUpdateA; // true (pending) comes before false (installed)

        // Within same group, critical files first
        if (fileA.critical != fileB.critical)
            return fileA.critical; // true (critical) comes before false (non-critical)

        // Otherwise, alphabetical
        return fileA.relativePath < fileB.relativePath;
    });

    if (ImGui::BeginTable(
            "UpdateFilesTable",
            5,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Hash (Local | Remote)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int idx : filteredIndices)
        {
            const auto& file = filesToShow.getReference(idx);

            ImGui::TableNextRow();

            // Determine if this file needs an update and its index in filesToDownload
            bool needsUpdate = false;
            int  downloadIndex = -1;
            for (int i = 0; i < updateInfo.filesToDownload.size(); ++i)
            {
                const auto& f = updateInfo.filesToDownload.getReference(i);
                if (f.relativePath == file.relativePath)
                {
                    needsUpdate = true;
                    downloadIndex = i;
                    break;
                }
            }

            // File Name + small checkbox on the left (single line per item)
            ImGui::TableSetColumnIndex(0);
            bool isSelected =
                (needsUpdate && downloadIndex >= 0 && downloadIndex < (int)fileSelected.size() &&
                 fileSelected[(size_t)downloadIndex]);

            juce::String checkboxId = "##select_" + file.relativePath;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f)); // smaller checkbox
            if (!needsUpdate || isDownloading)
            {
                ImGui::BeginDisabled();
                ImGui::Checkbox(checkboxId.toRawUTF8(), &isSelected);
                ImGui::EndDisabled();
            }
            else
            {
                if (ImGui::Checkbox(checkboxId.toRawUTF8(), &isSelected))
                {
                    if (downloadIndex >= 0 && downloadIndex < (int)fileSelected.size())
                        fileSelected[(size_t)downloadIndex] = isSelected;
                }
            }
            ImGui::PopStyleVar();

            ImGui::SameLine();
            ImGui::Text("%s", file.relativePath.toRawUTF8());
            if (file.critical)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(Critical)");
            }

            // Type
            ImGui::TableSetColumnIndex(1);
            juce::String ext = file.relativePath.fromLastOccurrenceOf(".", false, false);
            ImGui::Text("%s", ext.toRawUTF8());

            // Size
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", getFormattedFileSize(file.size).toRawUTF8());

            // Hash comparison
            ImGui::TableSetColumnIndex(3);
            juce::String localHash = getLocalHash(file.relativePath);
            juce::String remoteHash = file.sha256;

            if (localHash.isEmpty())
            {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "N/A");
                ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();
                ImGui::TextColored(
                    ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", remoteHash.substring(0, 16).toRawUTF8());
            }
            else
            {
                bool hashMatch = localHash.equalsIgnoreCase(remoteHash);
                ImGui::TextColored(
                    hashMatch ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "%s",
                    localHash.substring(0, 16).toRawUTF8());
                ImGui::SameLine();
                ImGui::Text("|");
                ImGui::SameLine();
                ImGui::TextColored(
                    hashMatch ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "%s",
                    remoteHash.substring(0, 16).toRawUTF8());

                // Show full hash on hover
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("Local:  %s", localHash.toRawUTF8());
                    ImGui::Text("Remote: %s", remoteHash.toRawUTF8());
                    ImGui::EndTooltip();
                }
            }

            // Status
            ImGui::TableSetColumnIndex(4);

            if (isDownloading && currentProgress.currentFile == file.relativePath)
            {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Downloading...");
            }
            else if (needsUpdate)
            {
                ImGui::Text("Pending");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Installed");
            }
        }
        ImGui::EndTable();
    }
}

void UpdateDownloadDialog::renderControls()
{
    // Summary stats based on current selection
    juce::int64 totalPendingSize = 0;
    juce::int64 totalSelectedSize = 0;
    int         pendingCount = updateInfo.filesToDownload.size();
    int         selectedCount = 0;

    for (int i = 0; i < updateInfo.filesToDownload.size(); ++i)
    {
        const auto& f = updateInfo.filesToDownload.getReference(i);
        totalPendingSize += f.size;

        bool selected = (i >= 0 && i < (int)fileSelected.size() && fileSelected[(size_t)i]);
        if (selected)
        {
            selectedCount++;
            totalSelectedSize += f.size;
        }
    }

    if (updateInfo.updateAvailable)
    {
        ImGui::Text("Summary: %d selected of %d pending", selectedCount, pendingCount);
        ImGui::SameLine();
        ImGui::Text(
            "| Selected Download Size: %s", getFormattedFileSize(totalSelectedSize).toRawUTF8());
    }
    else
    {
        ImGui::Text("Summary: %d files verified", updateInfo.allRemoteFiles.size());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "| System is up to date");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (isDownloading)
    {
        // Progress Bar
        float progress = (float)currentProgress.getProgress();
        ImGui::ProgressBar(progress, ImVec2(-1, 0));

        ImGui::Text("Downloading: %s", currentProgress.currentFile.toRawUTF8());
        ImGui::Text("Speed: %.2f MB/s", currentProgress.speedBytesPerSec / (1024.0 * 1024.0));
        ImGui::SameLine();
        ImGui::Text(
            "| Downloaded: %s / %s",
            getFormattedFileSize(currentProgress.bytesDownloaded).toRawUTF8(),
            getFormattedFileSize(currentProgress.totalBytes).toRawUTF8());

        if (ImGui::Button("Cancel", ImVec2(120, 30)))
        {
            if (onCancelDownload)
                onCancelDownload();
        }
    }
    else
    {
        // Selection helper buttons (only when not downloading)
        if (updateInfo.updateAvailable)
        {
            if (ImGui::Button("Select All Pending", ImVec2(150, 30)))
            {
                fileSelected.assign(fileSelected.size(), true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Deselect All", ImVec2(150, 30)))
            {
                fileSelected.assign(fileSelected.size(), false);
            }
        }

        ImGui::Spacing();

        // Action Buttons
        if (!updateInfo.updateAvailable)
            ImGui::BeginDisabled();

        if (ImGui::Button("Update Now", ImVec2(150, 40)))
        {
            if (onStartDownload)
            {
                auto selected = getSelectedFiles();
                onStartDownload(selected);
            }
        }

        if (!updateInfo.updateAvailable)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Remind Me Later", ImVec2(150, 40)))
        {
            close();
        }

        ImGui::SameLine();
        if (ImGui::Button("Skip This Version", ImVec2(150, 40)))
        {
            if (onSkipVersion)
                onSkipVersion();
            close();
        }
    }

    // Changelog link/summary
    ImGui::Spacing();
    if (updateInfo.changelogSummary.isNotEmpty())
    {
        ImGui::TextWrapped("What's New: %s", updateInfo.changelogSummary.toRawUTF8());
    }
}

juce::String UpdateDownloadDialog::getFormattedFileSize(juce::int64 size) const
{
    if (size >= 1024 * 1024 * 1024)
        return juce::String::formatted("%.2f GB", size / (1024.0 * 1024.0 * 1024.0));
    else if (size >= 1024 * 1024)
        return juce::String::formatted("%.1f MB", size / (1024.0 * 1024.0));
    else if (size >= 1024)
        return juce::String::formatted("%.1f KB", size / 1024.0);
    else
        return juce::String::formatted("%d B", (int)size);
}

juce::String UpdateDownloadDialog::getLocalHash(const juce::String& relativePath) const
{
    if (versionManager == nullptr)
        return {};

    // Only return hash from VersionManager (installed_files.json)
    // DO NOT calculate hash here - this is called during render loop!
    if (versionManager->hasFile(relativePath))
    {
        auto fileInfo = versionManager->getFileInfo(relativePath);
        return fileInfo.sha256;
    }

    // If not tracked, just return empty - don't freeze UI calculating hash
    return {};
}

juce::Array<FileInfo> UpdateDownloadDialog::getSelectedFiles() const
{
    juce::Array<FileInfo> result;

    // fileSelected is parallel to updateInfo.filesToDownload
    int numToDownload = updateInfo.filesToDownload.size();
    for (int i = 0; i < numToDownload; ++i)
    {
        bool selected = (i >= 0 && i < (int)fileSelected.size() && fileSelected[(size_t)i]);
        if (selected)
            result.add(updateInfo.filesToDownload.getReference(i));
    }

    return result;
}

} // namespace Updater
