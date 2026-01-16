#include "ThemeEditorComponent.h"
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <imgui.h>
#include <juce_opengl/juce_opengl.h>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif

namespace
{
static juce::File resolveFontPath(const juce::String& path)
{
    juce::File file(path);
    if (path.isNotEmpty() && !juce::File::isAbsolutePath(path))
    {
        auto exeDir =
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        file = exeDir.getChildFile(path);
    }
    return file;
}
} // namespace

ThemeEditorComponent::ThemeEditorComponent(ImGuiNodeEditorComponent* parent) : parentEditor(parent)
{
    // Initialize working copy with current theme
    m_workingCopy = ThemeManager::getInstance().getCurrentTheme();
    syncFontBuffersFromWorkingCopy();
    scanFontFolder();
    m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
}

void ThemeEditorComponent::open()
{
    m_isOpen = true;
    // Refresh working copy from current theme
    m_workingCopy = ThemeManager::getInstance().getCurrentTheme();
    m_currentThemeFilename =
        ThemeManager::getInstance().getCurrentThemeFilename(); // Get current theme filename
    m_hasChanges = false;
    m_currentTab = 0;
    m_showSaveDialog = false;
    memset(m_saveThemeName, 0, sizeof(m_saveThemeName));
    syncFontBuffersFromWorkingCopy();
    scanFontFolder();
    m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
}

void ThemeEditorComponent::close()
{
    if (m_hasChanges)
    {
        // TODO: Ask for confirmation if there are unsaved changes
        // For now, just discard
    }
    m_isOpen = false;
    m_showSaveDialog = false;
}

void ThemeEditorComponent::refreshThemeFromManager()
{
    m_workingCopy = ThemeManager::getInstance().getCurrentTheme();
    m_currentThemeFilename =
        ThemeManager::getInstance().getCurrentThemeFilename(); // Update current theme filename
    m_hasChanges = false;
    juce::Logger::writeToLog("[ThemeEditor] Refreshed working copy from ThemeManager");
    syncFontBuffersFromWorkingCopy();
    scanFontFolder();
    m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
}

void ThemeEditorComponent::render()
{
    if (!m_isOpen)
        return;

    // Window flags
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;

    // Set window size and position
    ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);

    // Build window title with current theme name
    juce::String windowTitle = "Theme Editor";
    if (m_currentThemeFilename.isNotEmpty())
    {
        // Remove .json extension and format nicely
        juce::String displayName = m_currentThemeFilename;
        if (displayName.endsWithIgnoreCase(".json"))
            displayName = displayName.substring(0, displayName.length() - 5);
        windowTitle += " - " + displayName;
    }
    else
    {
        windowTitle += " - Default Theme";
    }

    if (ImGui::Begin(windowTitle.toRawUTF8(), &m_isOpen, flags))
    {
        // Toolbar
        if (ImGui::Button("Apply Changes"))
        {
            applyChanges();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Tab"))
        {
            resetCurrentTab();
        }
        ImGui::SameLine();

        // Save button (only enabled if editing an existing theme)
        bool hasCurrentTheme = m_currentThemeFilename.isNotEmpty();
        if (!hasCurrentTheme)
            ImGui::BeginDisabled();
        if (ImGui::Button("Save"))
        {
            saveTheme();
        }
        if (!hasCurrentTheme)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Save As..."))
        {
            m_showSaveDialog = true;
            // Pre-fill with current theme name if available, otherwise use "CustomTheme"
            if (m_currentThemeFilename.isNotEmpty())
            {
                juce::String baseName = m_currentThemeFilename;
                if (baseName.endsWithIgnoreCase(".json"))
                    baseName = baseName.substring(0, baseName.length() - 5);
                strncpy(m_saveThemeName, baseName.toRawUTF8(), sizeof(m_saveThemeName) - 1);
            }
            else
            {
                strncpy(m_saveThemeName, "CustomTheme", sizeof(m_saveThemeName) - 1);
            }
        }
        ImGui::SameLine();
        if (m_hasChanges)
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Unsaved changes");
        }

        ImGui::Separator();

        // Tabs
        renderTabs();

        // Save dialog (modal)
        if (m_showSaveDialog)
        {
            renderSaveDialog();
        }

        // Eyedropper overlay if active
        renderPickerOverlay();
    }
    ImGui::End();

    // Close if window X clicked (m_isOpen was set to false by ImGui::Begin)
    if (!m_isOpen)
        close();
}

// ---- Eyedropper utilities ----
void ThemeEditorComponent::beginPickColor(ImU32* target)
{
    m_pickerActive = true;
    m_pickTargetU32 = target;
    m_pickTargetVec4 = nullptr;
}

void ThemeEditorComponent::beginPickColor(ImVec4* target)
{
    m_pickerActive = true;
    m_pickTargetU32 = nullptr;
    m_pickTargetVec4 = target;
}

bool ThemeEditorComponent::sampleScreenPixel(int, int, unsigned char outRGBA[4])
{
    // Disabled: pixel picking handled by ImGuiNodeEditorComponent.
    outRGBA[0] = outRGBA[1] = outRGBA[2] = 0;
    outRGBA[3] = 255;
    return false;
}

void ThemeEditorComponent::renderPickerOverlay()
{
    // Disabled: handled by node editor (we keep function for compatibility)
    juce::ignoreUnused(m_pickerActive);
}

void ThemeEditorComponent::renderTabs()
{
    if (ImGui::BeginTabBar("ThemeEditorTabs"))
    {
        // Tab buttons
        const char* tabNames[] = {
            "Style",
            "Colors",
            "Accent",
            "Text",
            "Status",
            "Headers",
            "ImNodes",
            "Links",
            "Canvas",
            "Layout",
            "Fonts",
            "Windows",
            "Modulation",
            "Meters",
            "Timeline",
            "Modules"};

        for (int i = 0; i < s_numTabs; ++i)
        {
            if (ImGui::BeginTabItem(tabNames[i]))
            {
                m_currentTab = i;

                // Render tab content
                switch (i)
                {
                case 0:
                    renderImGuiStyleTab();
                    break;
                case 1:
                    renderImGuiColorsTab();
                    break;
                case 2:
                    renderAccentTab();
                    break;
                case 3:
                    renderTextColorsTab();
                    break;
                case 4:
                    renderStatusColorsTab();
                    break;
                case 5:
                    renderHeaderColorsTab();
                    break;
                case 6:
                    renderImNodesTab();
                    break;
                case 7:
                    renderLinksTab();
                    break;
                case 8:
                    renderCanvasTab();
                    break;
                case 9:
                    renderLayoutTab();
                    break;
                case 10:
                    renderFontsTab();
                    break;
                case 11:
                    renderWindowsTab();
                    break;
                case 12:
                    renderModulationTab();
                    break;
                case 13:
                    renderMetersTab();
                    break;
                case 14:
                    renderTimelineTab();
                    break;
                case 15:
                    renderModulesTab();
                    break;
                }

                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

// Helper implementations
bool ThemeEditorComponent::colorEdit4(const char* label, ImVec4& color, ImGuiColorEditFlags flags)
{
    if (!label)
        return false;

    juce::Logger::writeToLog(
        juce::String("[ThemeColorPicker] Drawing (ImVec4) picker for: ") + label);

    bool valueChanged = false;
    try
    {
        ImGui::PushID(label);
        valueChanged = ImGui::ColorEdit4(
            "##picker",
            (float*)&color,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopID();

        if (valueChanged)
            m_hasChanges = true;
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            juce::String("[ThemeColorPicker] EXCEPTION (ImVec4) for label '") + label +
            "': " + e.what());
        ImGui::PopID();
        return false;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            juce::String("[ThemeColorPicker] UNKNOWN EXCEPTION (ImVec4) for label '") + label +
            "'");
        ImGui::PopID();
        return false;
    }

    return valueChanged;
}

bool ThemeEditorComponent::colorEditU32(const char* label, ImU32& color, ImGuiColorEditFlags flags)
{
    if (!label)
        return false;

    juce::Logger::writeToLog(
        juce::String("[ThemeColorPicker] Drawing (ImU32) picker for: ") + label);

    ImVec4 floatColor;
    bool   valueChanged = false;

    try
    {
        ImGui::PushID(label);

        floatColor = ImGui::ColorConvertU32ToFloat4(color);

        valueChanged = ImGui::ColorEdit4(
            "##picker",
            (float*)&floatColor,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

        if (valueChanged)
        {
            color = ImGui::ColorConvertFloat4ToU32(floatColor);
            m_hasChanges = true;
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(label);

        ImGui::PopID();
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            juce::String("[ThemeColorPicker] EXCEPTION (ImU32) for label '") + label +
            "': " + e.what());
        ImGui::PopID();
        return false;
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            juce::String("[ThemeColorPicker] UNKNOWN EXCEPTION (ImU32) for label '") + label + "'");
        ImGui::PopID();
        return false;
    }

    return valueChanged;
}

bool ThemeEditorComponent::dragFloat(
    const char* label,
    float&      value,
    float       speed,
    float       min,
    float       max,
    const char* format)
{
    bool changed = ImGui::DragFloat(label, &value, speed, min, max, format);
    if (changed)
        m_hasChanges = true;
    return changed;
}

bool ThemeEditorComponent::dragFloat2(
    const char* label,
    ImVec2&     value,
    float       speed,
    float       min,
    float       max,
    const char* format)
{
    bool changed = ImGui::DragFloat2(label, &value.x, speed, min, max, format);
    if (changed)
        m_hasChanges = true;
    return changed;
}

bool ThemeEditorComponent::triStateColorEdit(const char* label, TriStateColor& tsc)
{
    bool changed = false;
    if (ImGui::TreeNode(label))
    {
        changed |= colorEditU32("Base", tsc.base);
        changed |= colorEditU32("Hovered", tsc.hovered);
        changed |= colorEditU32("Active", tsc.active);
        ImGui::TreePop();
    }
    return changed;
}

// Tab implementations (stubs for now)
void ThemeEditorComponent::renderImGuiStyleTab()
{
    ImGui::Text("ImGui Style Settings");
    ImGui::Separator();

    // Split into two columns: controls on left, preview on right
    ImGui::Columns(2, "StyleColumns", true);

    // Left column: Controls
    if (ImGui::CollapsingHeader("Padding & Spacing"))
    {
        dragFloat2("Window Padding", m_workingCopy.style.WindowPadding, 1.0f, 0.0f, 50.0f);
        dragFloat2("Frame Padding", m_workingCopy.style.FramePadding, 1.0f, 0.0f, 50.0f);
        dragFloat2("Item Spacing", m_workingCopy.style.ItemSpacing, 1.0f, 0.0f, 50.0f);
        dragFloat2("Item Inner Spacing", m_workingCopy.style.ItemInnerSpacing, 1.0f, 0.0f, 50.0f);
    }

    if (ImGui::CollapsingHeader("Rounding"))
    {
        dragFloat("Window Rounding", m_workingCopy.style.WindowRounding, 0.5f, 0.0f, 20.0f);
        dragFloat("Child Rounding", m_workingCopy.style.ChildRounding, 0.5f, 0.0f, 20.0f);
        dragFloat("Frame Rounding", m_workingCopy.style.FrameRounding, 0.5f, 0.0f, 20.0f);
        dragFloat("Popup Rounding", m_workingCopy.style.PopupRounding, 0.5f, 0.0f, 20.0f);
        dragFloat("Scrollbar Rounding", m_workingCopy.style.ScrollbarRounding, 0.5f, 0.0f, 20.0f);
        dragFloat("Grab Rounding", m_workingCopy.style.GrabRounding, 0.5f, 0.0f, 20.0f);
        dragFloat("Tab Rounding", m_workingCopy.style.TabRounding, 0.5f, 0.0f, 20.0f);
    }

    if (ImGui::CollapsingHeader("Borders"))
    {
        dragFloat("Window Border Size", m_workingCopy.style.WindowBorderSize, 0.1f, 0.0f, 5.0f);
        dragFloat("Frame Border Size", m_workingCopy.style.FrameBorderSize, 0.1f, 0.0f, 5.0f);
        dragFloat("Popup Border Size", m_workingCopy.style.PopupBorderSize, 0.1f, 0.0f, 5.0f);
    }

    ImGui::NextColumn();

    // Right column: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    // Apply working copy style temporarily for preview
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiStyle  backup = style;

    style.WindowPadding = m_workingCopy.style.WindowPadding;
    style.FramePadding = m_workingCopy.style.FramePadding;
    style.ItemSpacing = m_workingCopy.style.ItemSpacing;
    style.ItemInnerSpacing = m_workingCopy.style.ItemInnerSpacing;
    style.WindowRounding = m_workingCopy.style.WindowRounding;
    style.ChildRounding = m_workingCopy.style.ChildRounding;
    style.FrameRounding = m_workingCopy.style.FrameRounding;
    style.PopupRounding = m_workingCopy.style.PopupRounding;
    style.ScrollbarRounding = m_workingCopy.style.ScrollbarRounding;
    style.GrabRounding = m_workingCopy.style.GrabRounding;
    style.TabRounding = m_workingCopy.style.TabRounding;
    style.WindowBorderSize = m_workingCopy.style.WindowBorderSize;
    style.FrameBorderSize = m_workingCopy.style.FrameBorderSize;
    style.PopupBorderSize = m_workingCopy.style.PopupBorderSize;

    // Preview window
    if (ImGui::BeginChild("StylePreview", ImVec2(0, 0), true))
    {
        ImGui::Text("Preview Window");
        ImGui::Separator();

        // Button preview
        if (ImGui::Button("Sample Button"))
        {
            // Button clicked
        }
        ImGui::SameLine();
        if (ImGui::Button("Another Button"))
        {
            // Button clicked
        }

        ImGui::Spacing();

        // Frame preview
        ImGui::Text("Frame with border:");
        ImGui::BeginChild(ImGui::GetID("preview_frame"), ImVec2(0, 60), true);
        ImGui::Text("Content inside frame");
        ImGui::Button("Button in Frame");
        ImGui::EndChild();

        ImGui::Spacing();

        // Tab preview
        if (ImGui::BeginTabBar("PreviewTabs"))
        {
            if (ImGui::BeginTabItem("Tab 1"))
            {
                ImGui::Text("Tab 1 content");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tab 2"))
            {
                ImGui::Text("Tab 2 content");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();

        // Slider preview
        static float sliderValue = 0.5f;
        ImGui::SliderFloat("Preview Slider", &sliderValue, 0.0f, 1.0f);

        ImGui::Spacing();

        // Checkbox preview
        static bool check1 = true, check2 = false;
        ImGui::Checkbox("Checkbox 1", &check1);
        ImGui::Checkbox("Checkbox 2", &check2);

        ImGui::Spacing();

        // Input preview
        static char text[64] = "Sample text";
        ImGui::InputText("Text Input", text, sizeof(text));
    }
    ImGui::EndChild();

    // Restore original style
    style = backup;

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderImGuiColorsTab()
{
    ImGui::Text("ImGui Colors");
    ImGui::Separator();
    ImGui::TextWrapped("Edit core ImGui colors. These affect all ImGui windows and widgets.");

    ImGui::Columns(2, "ImGuiColorsColumns", true);

    // Left: Controls organized by category
    if (ImGui::CollapsingHeader("Window Colors"))
    {
        ImVec4& windowBg = m_workingCopy.style.Colors[ImGuiCol_WindowBg];
        ImVec4& childBg = m_workingCopy.style.Colors[ImGuiCol_ChildBg];
        ImVec4& popupBg = m_workingCopy.style.Colors[ImGuiCol_PopupBg];
        ImVec4& titleBg = m_workingCopy.style.Colors[ImGuiCol_TitleBg];
        ImVec4& titleBgActive = m_workingCopy.style.Colors[ImGuiCol_TitleBgActive];
        ImVec4& titleBgCollapsed = m_workingCopy.style.Colors[ImGuiCol_TitleBgCollapsed];

        colorEdit4("Window Background", windowBg);
        colorEdit4("Node Background (ChildBg)", childBg);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Controls the background color used for node editors and child regions.");
        colorEdit4("Popup Background", popupBg);
        colorEdit4("Title Background", titleBg);
        colorEdit4("Title Active", titleBgActive);
        colorEdit4("Title Collapsed", titleBgCollapsed);
    }

    if (ImGui::CollapsingHeader("Text Colors"))
    {
        ImVec4& text = m_workingCopy.style.Colors[ImGuiCol_Text];
        ImVec4& textDisabled = m_workingCopy.style.Colors[ImGuiCol_TextDisabled];
        ImVec4& textSelectedBg = m_workingCopy.style.Colors[ImGuiCol_TextSelectedBg];

        colorEdit4("Text", text);
        colorEdit4("Text Disabled", textDisabled);
        colorEdit4("Text Selected Background", textSelectedBg);
    }

    if (ImGui::CollapsingHeader("Button & Frame Colors"))
    {
        ImVec4& button = m_workingCopy.style.Colors[ImGuiCol_Button];
        ImVec4& buttonHovered = m_workingCopy.style.Colors[ImGuiCol_ButtonHovered];
        ImVec4& buttonActive = m_workingCopy.style.Colors[ImGuiCol_ButtonActive];
        ImVec4& frameBg = m_workingCopy.style.Colors[ImGuiCol_FrameBg];
        ImVec4& frameBgHovered = m_workingCopy.style.Colors[ImGuiCol_FrameBgHovered];
        ImVec4& frameBgActive = m_workingCopy.style.Colors[ImGuiCol_FrameBgActive];

        colorEdit4("Button", button);
        colorEdit4("Button Hovered", buttonHovered);
        colorEdit4("Button Active", buttonActive);
        colorEdit4("Frame Background", frameBg);
        colorEdit4("Frame Hovered", frameBgHovered);
        colorEdit4("Frame Active", frameBgActive);
    }

    if (ImGui::CollapsingHeader("Slider & Scrollbar"))
    {
        ImVec4& sliderGrab = m_workingCopy.style.Colors[ImGuiCol_SliderGrab];
        ImVec4& sliderGrabActive = m_workingCopy.style.Colors[ImGuiCol_SliderGrabActive];
        ImVec4& scrollbarBg = m_workingCopy.style.Colors[ImGuiCol_ScrollbarBg];
        ImVec4& scrollbarGrab = m_workingCopy.style.Colors[ImGuiCol_ScrollbarGrab];
        ImVec4& scrollbarGrabHovered = m_workingCopy.style.Colors[ImGuiCol_ScrollbarGrabHovered];
        ImVec4& scrollbarGrabActive = m_workingCopy.style.Colors[ImGuiCol_ScrollbarGrabActive];

        colorEdit4("Slider Grab", sliderGrab);
        colorEdit4("Slider Grab Active", sliderGrabActive);
        colorEdit4("Scrollbar Background", scrollbarBg);
        colorEdit4("Scrollbar Grab", scrollbarGrab);
        colorEdit4("Scrollbar Grab Hovered", scrollbarGrabHovered);
        colorEdit4("Scrollbar Grab Active", scrollbarGrabActive);
    }

    if (ImGui::CollapsingHeader("Border & Separator"))
    {
        ImVec4& border = m_workingCopy.style.Colors[ImGuiCol_Border];
        ImVec4& borderShadow = m_workingCopy.style.Colors[ImGuiCol_BorderShadow];
        ImVec4& separator = m_workingCopy.style.Colors[ImGuiCol_Separator];
        ImVec4& separatorHovered = m_workingCopy.style.Colors[ImGuiCol_SeparatorHovered];
        ImVec4& separatorActive = m_workingCopy.style.Colors[ImGuiCol_SeparatorActive];

        colorEdit4("Border", border);
        colorEdit4("Border Shadow", borderShadow);
        colorEdit4("Separator", separator);
        colorEdit4("Separator Hovered", separatorHovered);
        colorEdit4("Separator Active", separatorActive);
    }

    if (ImGui::CollapsingHeader("Tab & Menu"))
    {
        ImVec4& tab = m_workingCopy.style.Colors[ImGuiCol_Tab];
        ImVec4& tabHovered = m_workingCopy.style.Colors[ImGuiCol_TabHovered];
        ImVec4& tabActive = m_workingCopy.style.Colors[ImGuiCol_TabActive];
        ImVec4& tabUnfocused = m_workingCopy.style.Colors[ImGuiCol_TabUnfocused];
        ImVec4& tabUnfocusedActive = m_workingCopy.style.Colors[ImGuiCol_TabUnfocusedActive];
        ImVec4& menuBarBg = m_workingCopy.style.Colors[ImGuiCol_MenuBarBg];

        colorEdit4("Tab", tab);
        colorEdit4("Tab Hovered", tabHovered);
        colorEdit4("Tab Active", tabActive);
        colorEdit4("Tab Unfocused", tabUnfocused);
        colorEdit4("Tab Unfocused Active", tabUnfocusedActive);
        colorEdit4("Menu Bar Background", menuBarBg);
    }

    if (ImGui::CollapsingHeader("Other"))
    {
        ImVec4& checkMark = m_workingCopy.style.Colors[ImGuiCol_CheckMark];
        ImVec4& dragDropTarget = m_workingCopy.style.Colors[ImGuiCol_DragDropTarget];
        ImVec4& header = m_workingCopy.style.Colors[ImGuiCol_Header];
        ImVec4& headerHovered = m_workingCopy.style.Colors[ImGuiCol_HeaderHovered];
        ImVec4& headerActive = m_workingCopy.style.Colors[ImGuiCol_HeaderActive];
        ImVec4& resizeGrip = m_workingCopy.style.Colors[ImGuiCol_ResizeGrip];
        ImVec4& resizeGripHovered = m_workingCopy.style.Colors[ImGuiCol_ResizeGripHovered];
        ImVec4& resizeGripActive = m_workingCopy.style.Colors[ImGuiCol_ResizeGripActive];

        colorEdit4("Check Mark", checkMark);
        colorEdit4("Drag Drop Target", dragDropTarget);
        colorEdit4("Header", header);
        colorEdit4("Header Hovered", headerHovered);
        colorEdit4("Header Active", headerActive);
        colorEdit4("Resize Grip", resizeGrip);
        colorEdit4("Resize Grip Hovered", resizeGripHovered);
        colorEdit4("Resize Grip Active", resizeGripActive);
    }

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    // Apply working copy colors temporarily for preview
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiStyle  backup = style;
    style = m_workingCopy.style;

    if (ImGui::BeginChild("ImGuiColorsPreview", ImVec2(0, 0), true))
    {
        // Window preview
        ImGui::Text("Window Preview");
        ImGui::Separator();

        if (ImGui::Button("Sample Button"))
        {
            // Button clicked
        }
        ImGui::SameLine();
        if (ImGui::Button("Another Button"))
        {
            // Button clicked
        }

        ImGui::Spacing();

        // Frame preview
        ImGui::Text("Frame with border:");
        ImGui::BeginChild(ImGui::GetID("preview_frame2"), ImVec2(0, 60), true);
        ImGui::Text("Content inside frame");
        ImGui::Button("Button in Frame");
        ImGui::EndChild();

        ImGui::Spacing();

        // Slider preview
        static float sliderValue = 0.5f;
        ImGui::SliderFloat("Preview Slider", &sliderValue, 0.0f, 1.0f);

        ImGui::Spacing();

        // Checkbox preview
        static bool check1 = true, check2 = false;
        ImGui::Checkbox("Checkbox 1", &check1);
        ImGui::Checkbox("Checkbox 2", &check2);

        ImGui::Spacing();

        // Input preview
        static char text[64] = "Sample text";
        ImGui::InputText("Text Input", text, sizeof(text));

        ImGui::Spacing();

        // Tab preview
        if (ImGui::BeginTabBar("PreviewTabs2"))
        {
            if (ImGui::BeginTabItem("Tab 1"))
            {
                ImGui::Text("Tab 1 content");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tab 2"))
            {
                ImGui::Text("Tab 2 content");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    // Restore original style
    style = backup;

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderAccentTab()
{
    ImGui::Text("Accent Color");
    ImGui::Separator();
    ImGui::TextWrapped(
        "The accent color is used throughout the UI for highlights and interactive elements.");

    ImGui::Columns(2, "AccentColumns", true);

    // Left: Color picker
    colorEdit4("Accent", m_workingCopy.accent);

    ImGui::Spacing();
    ImGui::Text("RGB Values:");
    ImGui::Text("R: %.3f", m_workingCopy.accent.x);
    ImGui::Text("G: %.3f", m_workingCopy.accent.y);
    ImGui::Text("B: %.3f", m_workingCopy.accent.z);
    ImGui::Text("A: %.3f", m_workingCopy.accent.w);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    // Large color swatch
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 100);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.accent));
    ImGui::SetCursorScreenPos(
        ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + ImGui::GetStyle().ItemSpacing.y));

    ImGui::Spacing();

    // Preview accent in UI elements
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, m_workingCopy.accent);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, m_workingCopy.accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, m_workingCopy.accent);
    ImGui::PushStyleColor(
        ImGuiCol_TabHovered,
        ImVec4(m_workingCopy.accent.x, m_workingCopy.accent.y, m_workingCopy.accent.z, 0.8f));

    if (ImGui::Button("Button (hovered color)"))
    {
        // Button clicked
    }

    static bool previewCheck = true;
    ImGui::Checkbox("Checkbox (checkmark color)", &previewCheck);

    static float previewSlider = 0.5f;
    ImGui::SliderFloat("Slider (grab color)", &previewSlider, 0.0f, 1.0f);

    if (ImGui::BeginTabBar("AccentTabs"))
    {
        if (ImGui::BeginTabItem("Tab (hover)"))
        {
            ImGui::Text("Tab content");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::PopStyleColor(4);

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderTextColorsTab()
{
    ImGui::Text("Text Colors");
    ImGui::Separator();

    ImGui::Columns(2, "TextColorsColumns", true);

    // Left: Controls
    if (ImGui::CollapsingHeader("Text Colors"))
    {
        colorEdit4("Section Header", m_workingCopy.text.section_header);
        colorEdit4("Warning", m_workingCopy.text.warning);
        colorEdit4("Success", m_workingCopy.text.success);
        colorEdit4("Error", m_workingCopy.text.error);
        colorEdit4("Disabled", m_workingCopy.text.disabled);
        colorEdit4("Active", m_workingCopy.text.active);
    }

    if (ImGui::CollapsingHeader("Tooltip Settings"))
    {
        dragFloat(
            "Tooltip Wrap (Standard)",
            m_workingCopy.text.tooltip_wrap_standard,
            1.0f,
            10.0f,
            100.0f);
        dragFloat(
            "Tooltip Wrap (Compact)", m_workingCopy.text.tooltip_wrap_compact, 1.0f, 10.0f, 100.0f);
    }

    if (ImGui::CollapsingHeader("Text Rendering"))
    {
        // Add the master toggle checkbox
        if (ImGui::Checkbox("Enable Text Glow / Shadow", &m_workingCopy.text.enable_text_glow))
        {
            m_hasChanges = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Enable a soft shadow/glow effect for all themed text");

        // Add the color picker for the glow
        colorEdit4("Glow / Shadow Color", m_workingCopy.text.text_glow_color);
    }

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImGui::TextColored(m_workingCopy.text.section_header, "Section Header Text");
    ImGui::Spacing();

    ImGui::TextColored(m_workingCopy.text.warning, "⚠ Warning Message");
    ImGui::Spacing();

    ImGui::TextColored(m_workingCopy.text.success, "✓ Success Message");
    ImGui::Spacing();

    ImGui::TextColored(m_workingCopy.text.error, "✗ Error Message");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, m_workingCopy.text.disabled);
    ImGui::Text("Disabled Text (grayed out)");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::TextColored(m_workingCopy.text.active, "● Active/Enabled Text");
    ImGui::Spacing();

    ImGui::Separator();
    ImGui::Text("Tooltip Preview:");
    ImGui::TextDisabled("(Hover over this text)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * m_workingCopy.text.tooltip_wrap_standard);
        ImGui::Text(
            "This is a tooltip with the wrap width you set. It demonstrates how tooltips will wrap "
            "at the specified character count.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderStatusColorsTab()
{
    ImGui::Text("Status Colors");
    ImGui::Separator();

    ImGui::Columns(2, "StatusColumns", true);

    // Left: Controls
    colorEdit4("Edited", m_workingCopy.status.edited);
    colorEdit4("Saved", m_workingCopy.status.saved);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImGui::Text("Status: ");
    ImGui::SameLine();
    ImGui::TextColored(m_workingCopy.status.edited, "EDITED");

    ImGui::Spacing();

    ImGui::Text("Status: ");
    ImGui::SameLine();
    ImGui::TextColored(m_workingCopy.status.saved, "SAVED");

    ImGui::Spacing();
    ImGui::Separator();

    // Preview as overlay-style status indicator
    ImGui::BeginChild("StatusPreview", ImVec2(0, 80), true);
    ImGui::SetCursorPos(ImVec2(10, 10));
    ImGui::TextColored(m_workingCopy.status.edited, "Status: EDITED");

    ImGui::SetCursorPos(ImVec2(10, 40));
    ImGui::TextColored(m_workingCopy.status.saved, "Status: SAVED");
    ImGui::EndChild();

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderHeaderColorsTab()
{
    ImGui::Text("Header Colors (TriState)");
    ImGui::Separator();

    ImGui::Columns(2, "HeaderColumns", true);

    // Left: Controls
    triStateColorEdit("Recent", m_workingCopy.headers.recent);
    triStateColorEdit("Samples", m_workingCopy.headers.samples);
    triStateColorEdit("Presets", m_workingCopy.headers.presets);
    triStateColorEdit("System", m_workingCopy.headers.system);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    // Preview Recent header
    ImVec2      pos = ImGui::GetCursorScreenPos();
    ImVec2      size = ImVec2(ImGui::GetContentRegionAvail().x, 30);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), m_workingCopy.headers.recent.base);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + 8));
    ImGui::Text("Recent (Base)");
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 5));

    // Preview Samples header
    pos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), m_workingCopy.headers.samples.base);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + 8));
    ImGui::Text("Samples (Base)");
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 5));

    // Preview Presets header
    pos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), m_workingCopy.headers.presets.base);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + 8));
    ImGui::Text("Presets (Base)");
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 5));

    // Preview System header
    pos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), m_workingCopy.headers.system.base);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + 8));
    ImGui::Text("System (Base)");
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 5));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Hover states:");

    // Hover preview
    pos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), m_workingCopy.headers.recent.hovered);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + 8));
    ImGui::Text("Recent (Hovered)");
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 5));

    pos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), m_workingCopy.headers.recent.active);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + 8));
    ImGui::Text("Recent (Active)");

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderImNodesTab()
{
    ImGui::Text("ImNodes Colors");
    ImGui::Separator();

    ImGui::Columns(2, "ImNodesColumns", true);

    // Left: Controls
    if (ImGui::CollapsingHeader("Category Colors"))
    {
        // Module categories
        const char* categoryNames[] = {
            "Source",
            "Effect",
            "Modulator",
            "Utility",
            "Seq",
            "MIDI",
            "Analysis",
            "TTS_Voice",
            "Special_Exp",
            "OpenCV",
            "Sys",
            "Comment",
            "Plugin",
            "Default"};

        ModuleCategory categories[] = {
            ModuleCategory::Source,
            ModuleCategory::Effect,
            ModuleCategory::Modulator,
            ModuleCategory::Utility,
            ModuleCategory::Seq,
            ModuleCategory::MIDI,
            ModuleCategory::Analysis,
            ModuleCategory::TTS_Voice,
            ModuleCategory::Special_Exp,
            ModuleCategory::OpenCV,
            ModuleCategory::Sys,
            ModuleCategory::Comment,
            ModuleCategory::Plugin,
            ModuleCategory::Default};

        for (int i = 0; i < 14; ++i)
        {
            ImU32& color = m_workingCopy.imnodes.category_colors[categories[i]];
            if (colorEditU32(categoryNames[i], color))
            {
                m_hasChanges = true;
            }
        }
    }

    if (ImGui::CollapsingHeader("Pin Colors"))
    {
        // Pin data types
        const char* pinTypeNames[] = {"CV", "Audio", "Gate", "Raw", "Video"};
        PinDataType pinTypes[] = {
            PinDataType::CV,
            PinDataType::Audio,
            PinDataType::Gate,
            PinDataType::Raw,
            PinDataType::Video};

        for (int i = 0; i < 5; ++i)
        {
            ImU32& color = m_workingCopy.imnodes.pin_colors[pinTypes[i]];
            if (colorEditU32(pinTypeNames[i], color))
            {
                m_hasChanges = true;
            }
        }

        ImGui::Separator();
        colorEditU32("Pin Connected", m_workingCopy.imnodes.pin_connected);
        colorEditU32("Pin Disconnected", m_workingCopy.imnodes.pin_disconnected);
    }

    if (ImGui::CollapsingHeader("Node States"))
    {
        colorEditU32("Node Muted", m_workingCopy.imnodes.node_muted);
        dragFloat("Node Muted Alpha", m_workingCopy.imnodes.node_muted_alpha, 0.01f, 0.0f, 1.0f);
        colorEditU32(
            "Node Hovered Link Highlight", m_workingCopy.imnodes.node_hovered_link_highlight);
    }

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 300);

    // Draw category color previews
    ImGui::Text("Category Colors:");
    float y = canvasPos.y + 20;
    float boxSize = 20.0f;
    float spacing = 5.0f;

    const char* categoryNames[] = {
        "Source",
        "Effect",
        "Modulator",
        "Utility",
        "Seq",
        "MIDI",
        "Analysis",
        "TTS_Voice",
        "Special_Exp",
        "OpenCV",
        "Sys",
        "Comment",
        "Plugin",
        "Default"};

    ModuleCategory categories[] = {
        ModuleCategory::Source,
        ModuleCategory::Effect,
        ModuleCategory::Modulator,
        ModuleCategory::Utility,
        ModuleCategory::Seq,
        ModuleCategory::MIDI,
        ModuleCategory::Analysis,
        ModuleCategory::TTS_Voice,
        ModuleCategory::Special_Exp,
        ModuleCategory::OpenCV,
        ModuleCategory::Sys,
        ModuleCategory::Comment,
        ModuleCategory::Plugin,
        ModuleCategory::Default};

    for (int i = 0; i < 14; ++i)
    {
        float x = canvasPos.x + (i % 7) * (boxSize + spacing + 60);
        float rowY = y + (i / 7) * (boxSize + spacing + 15);

        ImU32 color = m_workingCopy.imnodes.category_colors[categories[i]];
        drawList->AddRectFilled(ImVec2(x, rowY), ImVec2(x + boxSize, rowY + boxSize), color);
        drawList->AddRect(
            ImVec2(x, rowY), ImVec2(x + boxSize, rowY + boxSize), IM_COL32(100, 100, 100, 255));
        ImGui::SetCursorScreenPos(ImVec2(x + boxSize + 5, rowY));
        ImGui::Text("%s", categoryNames[i]);
    }

    y += 80;
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, y));
    ImGui::Text("Pin Colors:");
    y += 20;

    // Draw pin color previews
    const char* pinTypeNames[] = {"CV", "Audio", "Gate", "Raw", "Video"};
    PinDataType pinTypes[] = {
        PinDataType::CV,
        PinDataType::Audio,
        PinDataType::Gate,
        PinDataType::Raw,
        PinDataType::Video};

    for (int i = 0; i < 5; ++i)
    {
        float x = canvasPos.x + i * 80;
        ImU32 color = m_workingCopy.imnodes.pin_colors[pinTypes[i]];

        // Draw pin circle
        ImVec2 center = ImVec2(x + 15, y + 10);
        drawList->AddCircleFilled(center, 8.0f, color, 0);
        drawList->AddCircle(center, 8.0f, IM_COL32(100, 100, 100, 255), 0, 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(x, y + 25));
        ImGui::Text("%s", pinTypeNames[i]);
    }

    y += 60;
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, y));

    // Node muted preview
    ImGui::Text("Node Muted Preview:");
    ImVec2 nodePos = ImVec2(canvasPos.x, y + 20);
    ImVec2 nodeSize = ImVec2(150, 60);
    ImU32  nodeColor = m_workingCopy.imnodes.node_muted;
    ImU32  nodeColorAlpha = IM_COL32(
        (int)((nodeColor & 0xFF) * m_workingCopy.imnodes.node_muted_alpha),
        (int)(((nodeColor >> 8) & 0xFF) * m_workingCopy.imnodes.node_muted_alpha),
        (int)(((nodeColor >> 16) & 0xFF) * m_workingCopy.imnodes.node_muted_alpha),
        255);
    drawList->AddRectFilled(
        nodePos, ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y), nodeColorAlpha);
    drawList->AddRect(
        nodePos,
        ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
        IM_COL32(100, 100, 100, 255));
    ImGui::SetCursorScreenPos(ImVec2(nodePos.x + 5, nodePos.y + 20));
    ImGui::Text("Muted Node");

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderLinksTab()
{
    ImGui::Text("Link Colors");
    ImGui::Separator();

    ImGui::Columns(2, "LinksColumns", true);

    // Left: Controls
    colorEditU32("Link Hovered", m_workingCopy.links.link_hovered);
    colorEditU32("Link Selected", m_workingCopy.links.link_selected);
    colorEditU32("Link Highlighted", m_workingCopy.links.link_highlighted);
    colorEditU32("Preview Color", m_workingCopy.links.preview_color);
    dragFloat("Preview Width", m_workingCopy.links.preview_width, 0.1f, 1.0f, 10.0f);
    colorEditU32("Label Background", m_workingCopy.links.label_background);
    colorEditU32("Label Text", m_workingCopy.links.label_text);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 200);

    // Draw link previews
    float y = canvasPos.y;
    float x = canvasPos.x + 20;

    // Hovered link
    ImVec2 p1 = ImVec2(x, y + 20);
    ImVec2 p2 = ImVec2(x + 100, y + 40);
    drawList->AddBezierCubic(
        p1,
        ImVec2(p1.x + 30, p1.y),
        ImVec2(p2.x - 30, p2.y),
        p2,
        m_workingCopy.links.link_hovered,
        m_workingCopy.links.preview_width);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Text("Hovered Link");

    // Selected link
    y += 50;
    p1 = ImVec2(x, y + 20);
    p2 = ImVec2(x + 100, y + 40);
    drawList->AddBezierCubic(
        p1,
        ImVec2(p1.x + 30, p1.y),
        ImVec2(p2.x - 30, p2.y),
        p2,
        m_workingCopy.links.link_selected,
        m_workingCopy.links.preview_width);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Text("Selected Link");

    // Highlighted link
    y += 50;
    p1 = ImVec2(x, y + 20);
    p2 = ImVec2(x + 100, y + 40);
    drawList->AddBezierCubic(
        p1,
        ImVec2(p1.x + 30, p1.y),
        ImVec2(p2.x - 30, p2.y),
        p2,
        m_workingCopy.links.link_highlighted,
        m_workingCopy.links.preview_width);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Text("Highlighted Link");

    // Preview color
    y += 50;
    p1 = ImVec2(x, y + 20);
    p2 = ImVec2(x + 100, y + 40);
    drawList->AddBezierCubic(
        p1,
        ImVec2(p1.x + 30, p1.y),
        ImVec2(p2.x - 30, p2.y),
        p2,
        m_workingCopy.links.preview_color,
        m_workingCopy.links.preview_width);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Text("Preview Color");

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 10));

    // Label preview
    ImGui::Text("Link Label Preview:");
    ImVec2 labelPos = ImGui::GetCursorScreenPos();
    ImVec2 labelSize = ImVec2(120, 30);
    drawList->AddRectFilled(
        labelPos,
        ImVec2(labelPos.x + labelSize.x, labelPos.y + labelSize.y),
        m_workingCopy.links.label_background);
    ImGui::SetCursorScreenPos(ImVec2(labelPos.x + 5, labelPos.y + 8));
    ImGui::PushStyleColor(
        ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(m_workingCopy.links.label_text));
    ImGui::Text("Link Label");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(labelPos.x, labelPos.y + labelSize.y + 5));

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderCanvasTab()
{
    ImGui::Text("Canvas Colors");
    ImGui::Separator();

    ImGui::Columns(2, "CanvasColumns", true);

    // Left: Controls
    if (ImGui::CollapsingHeader("Canvas Background"))
    {
        colorEditU32("Canvas Background", m_workingCopy.canvas.canvas_background);
    }

    if (ImGui::CollapsingHeader("Grid Settings"))
    {
        colorEditU32("Grid Color", m_workingCopy.canvas.grid_color);
        colorEditU32("Grid Origin Color", m_workingCopy.canvas.grid_origin_color);
        dragFloat("Grid Size", m_workingCopy.canvas.grid_size, 1.0f, 10.0f, 200.0f);
        colorEditU32("Scale Text Color", m_workingCopy.canvas.scale_text_color);
        dragFloat("Scale Interval", m_workingCopy.canvas.scale_interval, 10.0f, 50.0f, 1000.0f);
    }

    if (ImGui::CollapsingHeader("Overlays & UI"))
    {
        colorEditU32("Drop Target Overlay", m_workingCopy.canvas.drop_target_overlay);
        colorEditU32("Mouse Position Text", m_workingCopy.canvas.mouse_position_text);
    }

    if (ImGui::CollapsingHeader("Node Styling"))
    {
        colorEditU32("Node Background", m_workingCopy.canvas.node_background);
        colorEditU32("Node Frame", m_workingCopy.canvas.node_frame);
        colorEditU32("Node Frame Hovered", m_workingCopy.canvas.node_frame_hovered);
        colorEditU32("Node Frame Selected", m_workingCopy.canvas.node_frame_selected);
        dragFloat("Node Rounding", m_workingCopy.canvas.node_rounding, 0.1f, 0.0f, 20.0f);
        dragFloat("Node Border Width", m_workingCopy.canvas.node_border_width, 0.1f, 0.0f, 10.0f);
    }

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 300);

    // Draw canvas background
    drawList->AddRectFilled(
        canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        m_workingCopy.canvas.canvas_background);

    // Draw grid preview
    float gridSize = m_workingCopy.canvas.grid_size;
    float scaleInterval = m_workingCopy.canvas.scale_interval;

    // Draw grid lines
    for (float x = canvasPos.x; x < canvasPos.x + canvasSize.x; x += gridSize)
    {
        drawList->AddLine(
            ImVec2(x, canvasPos.y),
            ImVec2(x, canvasPos.y + canvasSize.y),
            m_workingCopy.canvas.grid_color);
    }
    for (float y = canvasPos.y; y < canvasPos.y + canvasSize.y; y += gridSize)
    {
        drawList->AddLine(
            ImVec2(canvasPos.x, y),
            ImVec2(canvasPos.x + canvasSize.x, y),
            m_workingCopy.canvas.grid_color);
    }

    // Draw origin (center)
    ImVec2 center = ImVec2(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);
    drawList->AddCircle(center, 3.0f, m_workingCopy.canvas.grid_origin_color, 0, 2.0f);
    drawList->AddLine(
        ImVec2(center.x - 10, center.y),
        ImVec2(center.x + 10, center.y),
        m_workingCopy.canvas.grid_origin_color,
        2.0f);
    drawList->AddLine(
        ImVec2(center.x, center.y - 10),
        ImVec2(center.x, center.y + 10),
        m_workingCopy.canvas.grid_origin_color,
        2.0f);

    // Draw scale markers
    for (float x = canvasPos.x; x < canvasPos.x + canvasSize.x; x += scaleInterval)
    {
        ImVec2 textPos = ImVec2(x, canvasPos.y + 5);
        char   label[32];
        snprintf(label, sizeof(label), "%.0f", (x - canvasPos.x));
        drawList->AddText(textPos, m_workingCopy.canvas.scale_text_color, label);
    }

    // Draw drop target overlay preview
    ImVec2 dropPos = ImVec2(canvasPos.x + canvasSize.x * 0.3f, canvasPos.y + canvasSize.y * 0.3f);
    ImVec2 dropSize = ImVec2(80, 60);
    drawList->AddRectFilled(
        dropPos,
        ImVec2(dropPos.x + dropSize.x, dropPos.y + dropSize.y),
        m_workingCopy.canvas.drop_target_overlay);
    drawList->AddRect(
        dropPos,
        ImVec2(dropPos.x + dropSize.x, dropPos.y + dropSize.y),
        m_workingCopy.canvas.drop_target_overlay,
        0.0f,
        0,
        2.0f);

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 10));

    // Mouse position text preview
    ImGui::PushStyleColor(
        ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(m_workingCopy.canvas.mouse_position_text));
    ImGui::Text("Mouse: 1234, 567");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Node preview
    ImGui::Text("Node Preview");
    ImVec2 nodePreviewPos = ImGui::GetCursorScreenPos();
    ImVec2 nodeSize = ImVec2(150, 80);
    ImVec2 nodeMin = ImVec2(nodePreviewPos.x + 20, nodePreviewPos.y + 20);
    ImVec2 nodeMax = ImVec2(nodeMin.x + nodeSize.x, nodeMin.y + nodeSize.y);

    // Draw node background
    drawList->AddRectFilled(
        nodeMin, nodeMax, m_workingCopy.canvas.node_background, m_workingCopy.canvas.node_rounding);

    // Draw node frame (normal state)
    drawList->AddRect(
        nodeMin,
        nodeMax,
        m_workingCopy.canvas.node_frame,
        m_workingCopy.canvas.node_rounding,
        0,
        m_workingCopy.canvas.node_border_width);

    // Draw node title bar
    ImVec2 titleBarMin = nodeMin;
    ImVec2 titleBarMax = ImVec2(nodeMax.x, nodeMin.y + 25);
    drawList->AddRectFilled(
        titleBarMin,
        titleBarMax,
        m_workingCopy.canvas.node_frame,
        m_workingCopy.canvas.node_rounding);

    drawList->AddText(
        ImVec2(nodeMin.x + 8, nodeMin.y + 5), IM_COL32(255, 255, 255, 255), "Example Node");

    // Draw hovered state preview (second node)
    ImVec2 node2Min = ImVec2(nodePreviewPos.x + 200, nodePreviewPos.y + 20);
    ImVec2 node2Max = ImVec2(node2Min.x + nodeSize.x, node2Min.y + nodeSize.y);
    drawList->AddRectFilled(
        node2Min,
        node2Max,
        m_workingCopy.canvas.node_background,
        m_workingCopy.canvas.node_rounding);
    drawList->AddRect(
        node2Min,
        node2Max,
        m_workingCopy.canvas.node_frame_hovered,
        m_workingCopy.canvas.node_rounding,
        0,
        m_workingCopy.canvas.node_border_width);
    drawList->AddRectFilled(
        node2Min,
        ImVec2(node2Max.x, node2Min.y + 25),
        m_workingCopy.canvas.node_frame_hovered,
        m_workingCopy.canvas.node_rounding);
    drawList->AddText(
        ImVec2(node2Min.x + 8, node2Min.y + 5), IM_COL32(255, 255, 255, 255), "Hovered");

    // Draw selected state preview (third node)
    ImVec2 node3Min = ImVec2(nodePreviewPos.x + 380, nodePreviewPos.y + 20);
    ImVec2 node3Max = ImVec2(node3Min.x + nodeSize.x, node3Min.y + nodeSize.y);
    drawList->AddRectFilled(
        node3Min,
        node3Max,
        m_workingCopy.canvas.node_background,
        m_workingCopy.canvas.node_rounding);
    drawList->AddRect(
        node3Min,
        node3Max,
        m_workingCopy.canvas.node_frame_selected,
        m_workingCopy.canvas.node_rounding,
        0,
        m_workingCopy.canvas.node_border_width);
    drawList->AddRectFilled(
        node3Min,
        ImVec2(node3Max.x, node3Min.y + 25),
        m_workingCopy.canvas.node_frame_selected,
        m_workingCopy.canvas.node_rounding);
    drawList->AddText(
        ImVec2(node3Min.x + 8, node3Min.y + 5), IM_COL32(255, 255, 255, 255), "Selected");

    ImGui::SetCursorScreenPos(ImVec2(nodePreviewPos.x, nodePreviewPos.y + nodeSize.y + 40));

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderLayoutTab()
{
    ImGui::Text("Layout Settings");
    ImGui::Separator();

    ImGui::Columns(2, "LayoutColumns", true);

    // Left: Controls
    dragFloat("Sidebar Width", m_workingCopy.layout.sidebar_width, 1.0f, 100.0f, 500.0f);
    dragFloat("Window Padding", m_workingCopy.layout.window_padding, 1.0f, 0.0f, 50.0f);
    dragFloat(
        "Node Vertical Padding", m_workingCopy.layout.node_vertical_padding, 1.0f, 0.0f, 200.0f);
    dragFloat(
        "Preset Vertical Padding",
        m_workingCopy.layout.preset_vertical_padding,
        1.0f,
        0.0f,
        300.0f);
    dragFloat("Node Default Width", m_workingCopy.layout.node_default_width, 1.0f, 100.0f, 1000.0f);
    dragFloat2(
        "Node Default Padding", m_workingCopy.layout.node_default_padding, 1.0f, 0.0f, 50.0f);
    dragFloat2("Node Muted Padding", m_workingCopy.layout.node_muted_padding, 1.0f, 0.0f, 50.0f);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 250);

    // Draw sidebar
    float sidebarWidth = m_workingCopy.layout.sidebar_width;
    if (sidebarWidth > canvasSize.x * 0.4f)
        sidebarWidth = canvasSize.x * 0.4f; // Limit for preview
    drawList->AddRectFilled(
        canvasPos,
        ImVec2(canvasPos.x + sidebarWidth, canvasPos.y + canvasSize.y),
        IM_COL32(40, 40, 40, 255));
    drawList->AddLine(
        ImVec2(canvasPos.x + sidebarWidth, canvasPos.y),
        ImVec2(canvasPos.x + sidebarWidth, canvasPos.y + canvasSize.y),
        IM_COL32(60, 60, 60, 255),
        1.0f);

    // Draw main area with padding
    float  padding = m_workingCopy.layout.window_padding;
    ImVec2 mainAreaStart = ImVec2(canvasPos.x + sidebarWidth + padding, canvasPos.y + padding);
    ImVec2 mainAreaSize =
        ImVec2(canvasSize.x - sidebarWidth - padding * 2, canvasSize.y - padding * 2);

    // Draw node preview
    float nodeWidth = m_workingCopy.layout.node_default_width;
    if (nodeWidth > mainAreaSize.x * 0.8f)
        nodeWidth = mainAreaSize.x * 0.8f;
    float  nodeHeight = 60.0f;
    ImVec2 nodePos = ImVec2(
        mainAreaStart.x + m_workingCopy.layout.node_default_padding.x,
        mainAreaStart.y + m_workingCopy.layout.node_default_padding.y);
    ImVec2 nodeSize = ImVec2(nodeWidth, nodeHeight);

    drawList->AddRectFilled(
        nodePos, ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y), IM_COL32(50, 50, 50, 255));
    drawList->AddRect(
        nodePos,
        ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
        IM_COL32(100, 100, 100, 255),
        0.0f,
        0,
        1.0f);

    // Draw second node with vertical padding
    ImVec2 node2Pos =
        ImVec2(nodePos.x, nodePos.y + nodeHeight + m_workingCopy.layout.node_vertical_padding);
    drawList->AddRectFilled(
        node2Pos,
        ImVec2(node2Pos.x + nodeSize.x, node2Pos.y + nodeSize.y),
        IM_COL32(50, 50, 50, 255));
    drawList->AddRect(
        node2Pos,
        ImVec2(node2Pos.x + nodeSize.x, node2Pos.y + nodeSize.y),
        IM_COL32(100, 100, 100, 255),
        0.0f,
        0,
        1.0f);

    // Labels
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x + 5, canvasPos.y + 5));
    ImGui::Text("Sidebar");

    ImGui::SetCursorScreenPos(ImVec2(nodePos.x + 5, nodePos.y + 5));
    ImGui::Text("Node");

    ImGui::SetCursorScreenPos(ImVec2(node2Pos.x + 5, node2Pos.y + 5));
    ImGui::Text("Node");

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 10));

    ImGui::Text(
        "Sidebar: %.0fpx | Node Width: %.0fpx | Padding: %.0fpx",
        m_workingCopy.layout.sidebar_width,
        m_workingCopy.layout.node_default_width,
        m_workingCopy.layout.window_padding);

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderFontsTab()
{
    ImGui::Text("Font Settings");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Select a font from the bundled 'fonts' folder or browse for a custom font. Click 'Apply "
        "Changes' to rebuild the font atlas.");

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Default Font", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Scanned Fonts (from 'fonts' folder')");

        juce::String       selectionLabel = "<None (ImGui Default)>";
        const juce::String currentPath = m_workingCopy.fonts.default_path;

        if (m_selectedFontIndex >= 0 && m_selectedFontIndex < m_scannedFontFiles.size())
        {
            selectionLabel = juce::File(m_scannedFontFiles[m_selectedFontIndex]).getFileName();
        }
        else if (currentPath.isNotEmpty())
        {
            selectionLabel = juce::File(currentPath).getFileName();
        }

        float availableWidth = ImGui::GetContentRegionAvail().x;
        float comboWidth = availableWidth - 70.0f;
        if (comboWidth < 150.0f)
            comboWidth = availableWidth;

        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##DefaultFontCombo", selectionLabel.toRawUTF8()))
        {
            const bool defaultSelected = (m_selectedFontIndex == -1 && currentPath.isEmpty());
            if (ImGui::Selectable("<None (ImGui Default)>", defaultSelected))
            {
                m_selectedFontIndex = -1;
                m_workingCopy.fonts.default_path.clear();
                m_hasChanges = true;
                previewFontChanges();
            }
            if (defaultSelected)
                ImGui::SetItemDefaultFocus();

            for (int i = 0; i < m_scannedFontFiles.size(); ++i)
            {
                const bool   isSelected = (m_selectedFontIndex == i);
                juce::String filename = juce::File(m_scannedFontFiles[i]).getFileName();

                if (ImGui::Selectable(filename.toRawUTF8(), isSelected))
                {
                    m_selectedFontIndex = i;
                    m_workingCopy.fonts.default_path = m_scannedFontFiles[i];
                    m_hasChanges = true;
                    previewFontChanges();
                }

                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Scan"))
        {
            scanFontFolder();
            m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
        }

        ImGui::Spacing();
        if (ImGui::Button("Browse for other font..."))
        {
            auto fontsFolder = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                   .getParentDirectory()
                                   .getChildFile("fonts");
            juce::File initialDir = fontsFolder.exists() ? fontsFolder : juce::File();
            m_fontChooser = std::make_unique<juce::FileChooser>(
                "Select custom font", initialDir, "*.ttf;*.otf;*.ttc");

            auto chooserFlags =
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
            m_fontChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file.existsAsFile())
                {
                    m_workingCopy.fonts.default_path = file.getFullPathName();
                    m_hasChanges = true;
                    m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
                    previewFontChanges();
                }
            });
        }

        ImGui::Spacing();
        if (dragFloat("Default Font Size", m_workingCopy.fonts.default_size, 0.5f, 8.0f, 72.0f))
        {
            if (ImGui::IsItemDeactivatedAfterEdit())
                previewFontChanges();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Preview");
    ImGui::BeginChild("FontPreview", ImVec2(0, 200), true);

    const float baseSize = ImGui::GetFontSize();
    const float desiredSize = m_workingCopy.fonts.default_size;
    const float scale = (baseSize > 0.0f) ? (desiredSize / baseSize) : 1.0f;
    ImGui::SetWindowFontScale(scale);

    ImGui::Text("Default Font Size (%.1f):", m_workingCopy.fonts.default_size);
    ImGui::Text("The quick brown fox jumps over the lazy dog.");
    ImGui::Text("0123456789 !@#$%%^&*()");

    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
}

void ThemeEditorComponent::renderWindowsTab()
{
    ImGui::Text("Window Settings");
    ImGui::Separator();

    ImGui::Columns(2, "WindowsColumns", true);

    // Left: Controls
    if (ImGui::CollapsingHeader("Window Transparency"))
    {
        // Slider for the main Status Overlay
        if (ImGui::SliderFloat(
                "Status Overlay", &m_workingCopy.windows.status_overlay_alpha, 0.0f, 1.0f, "%.2f"))
        {
            m_hasChanges = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transparency for the 'Status: EDITED' overlay");

        // Slider for the Probe Scope
        if (ImGui::SliderFloat(
                "Probe Scope", &m_workingCopy.windows.probe_scope_alpha, 0.0f, 1.0f, "%.2f"))
        {
            m_hasChanges = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transparency for the 🔬 Probe Scope window");

        // Slider for Notifications
        if (ImGui::SliderFloat(
                "Notifications", &m_workingCopy.windows.notifications_alpha, 0.0f, 1.0f, "%.2f"))
        {
            m_hasChanges = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transparency for pop-up notifications (Success, Error, etc.)");

        // Slider for Preset Status (kept for backward compatibility)
        if (ImGui::SliderFloat(
                "Preset Status", &m_workingCopy.windows.preset_status_alpha, 0.0f, 1.0f, "%.2f"))
        {
            m_hasChanges = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transparency for the Preset Status overlay");
    }

    if (ImGui::CollapsingHeader("Probe Scope Size"))
    {
        dragFloat(
            "Probe Scope Width", m_workingCopy.windows.probe_scope_width, 1.0f, 100.0f, 500.0f);
        dragFloat(
            "Probe Scope Height", m_workingCopy.windows.probe_scope_height, 1.0f, 50.0f, 500.0f);
    }

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 250);

    // Draw background
    drawList->AddRectFilled(
        canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        IM_COL32(10, 10, 10, 255));

    // Status overlay preview
    ImVec2 statusPos = ImVec2(canvasPos.x + 10, canvasPos.y + 10);
    ImVec2 statusSize = ImVec2(150, 40);
    ImU32  statusColor =
        IM_COL32(255, 255, 255, (int)(m_workingCopy.windows.status_overlay_alpha * 255));
    drawList->AddRectFilled(
        statusPos, ImVec2(statusPos.x + statusSize.x, statusPos.y + statusSize.y), statusColor);
    drawList->AddRect(
        statusPos,
        ImVec2(statusPos.x + statusSize.x, statusPos.y + statusSize.y),
        IM_COL32(200, 200, 200, 255),
        0.0f,
        0,
        1.0f);
    ImGui::SetCursorScreenPos(ImVec2(statusPos.x + 5, statusPos.y + 12));
    ImGui::Text("Status Overlay");
    ImGui::SetCursorScreenPos(ImVec2(statusPos.x, statusPos.y + statusSize.y + 5));

    // Probe scope preview
    float scopeWidth = m_workingCopy.windows.probe_scope_width;
    float scopeHeight = m_workingCopy.windows.probe_scope_height;
    if (scopeWidth > canvasSize.x * 0.8f)
        scopeWidth = canvasSize.x * 0.8f;
    if (scopeHeight > canvasSize.y * 0.5f)
        scopeHeight = canvasSize.y * 0.5f;

    ImVec2 scopePos = ImVec2(canvasPos.x + 10, ImGui::GetCursorScreenPos().y);
    ImVec2 scopeSize = ImVec2(scopeWidth, scopeHeight);
    ImU32  scopeColor = IM_COL32(0, 200, 255, (int)(m_workingCopy.windows.probe_scope_alpha * 255));
    drawList->AddRectFilled(
        scopePos,
        ImVec2(scopePos.x + scopeSize.x, scopePos.y + scopeSize.y),
        IM_COL32(20, 20, 20, 255));
    drawList->AddRect(
        scopePos,
        ImVec2(scopePos.x + scopeSize.x, scopePos.y + scopeSize.y),
        scopeColor,
        0.0f,
        0,
        2.0f);

    // Draw scope waveform
    for (int i = 0; i < (int)scopeSize.x - 4; i += 2)
    {
        float x = scopePos.x + 2 + i;
        float y = scopePos.y + scopeSize.y * 0.5f + sinf(i * 0.1f) * scopeSize.y * 0.3f;
        drawList->AddLine(
            ImVec2(x, y),
            ImVec2(x + 2, y + sinf((i + 2) * 0.1f) * scopeSize.y * 0.3f),
            scopeColor,
            1.5f);
    }

    ImGui::SetCursorScreenPos(ImVec2(scopePos.x + 5, scopePos.y + 5));
    ImGui::Text("Probe Scope");

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 10));

    ImGui::Text(
        "Scope: %.0fx%.0fpx | Alpha: %.2f",
        m_workingCopy.windows.probe_scope_width,
        m_workingCopy.windows.probe_scope_height,
        m_workingCopy.windows.probe_scope_alpha);

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderModulationTab()
{
    ImGui::Text("Modulation Colors");
    ImGui::Separator();

    ImGui::Columns(2, "ModulationColumns", true);

    // Left: Controls
    colorEdit4("Frequency", m_workingCopy.modulation.frequency);
    colorEdit4("Timbre", m_workingCopy.modulation.timbre);
    colorEdit4("Amplitude", m_workingCopy.modulation.amplitude);
    colorEdit4("Filter", m_workingCopy.modulation.filter);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 200);

    // Draw waveform previews for each modulation type
    float waveHeight = 30.0f;
    float ySpacing = 40.0f;

    // Frequency
    ImVec2 freqStart = ImVec2(canvasPos.x, canvasPos.y);
    ImGui::SetCursorScreenPos(freqStart);
    ImGui::Text("Frequency:");
    for (int i = 0; i < 50; ++i)
    {
        float x = freqStart.x + i * 5.0f;
        float y = freqStart.y + 20.0f + sinf(i * 0.2f) * waveHeight * 0.5f;
        drawList->AddCircleFilled(
            ImVec2(x, y),
            2.0f,
            ImGui::ColorConvertFloat4ToU32(m_workingCopy.modulation.frequency),
            0);
    }

    // Timbre
    ImVec2 timbreStart = ImVec2(canvasPos.x, canvasPos.y + ySpacing);
    ImGui::SetCursorScreenPos(timbreStart);
    ImGui::Text("Timbre:");
    for (int i = 0; i < 50; ++i)
    {
        float x = timbreStart.x + i * 5.0f;
        float y = timbreStart.y + 20.0f + sinf(i * 0.15f) * waveHeight * 0.5f;
        drawList->AddCircleFilled(
            ImVec2(x, y), 2.0f, ImGui::ColorConvertFloat4ToU32(m_workingCopy.modulation.timbre), 0);
    }

    // Amplitude
    ImVec2 ampStart = ImVec2(canvasPos.x, canvasPos.y + ySpacing * 2);
    ImGui::SetCursorScreenPos(ampStart);
    ImGui::Text("Amplitude:");
    for (int i = 0; i < 50; ++i)
    {
        float x = ampStart.x + i * 5.0f;
        float y = ampStart.y + 20.0f + sinf(i * 0.3f) * waveHeight * 0.5f;
        drawList->AddCircleFilled(
            ImVec2(x, y),
            2.0f,
            ImGui::ColorConvertFloat4ToU32(m_workingCopy.modulation.amplitude),
            0);
    }

    // Filter
    ImVec2 filterStart = ImVec2(canvasPos.x, canvasPos.y + ySpacing * 3);
    ImGui::SetCursorScreenPos(filterStart);
    ImGui::Text("Filter:");
    for (int i = 0; i < 50; ++i)
    {
        float x = filterStart.x + i * 5.0f;
        float y = filterStart.y + 20.0f + sinf(i * 0.1f) * waveHeight * 0.5f;
        drawList->AddCircleFilled(
            ImVec2(x, y), 2.0f, ImGui::ColorConvertFloat4ToU32(m_workingCopy.modulation.filter), 0);
    }

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y));

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderMetersTab()
{
    ImGui::Text("Meter Colors");
    ImGui::Separator();

    ImGui::Columns(2, "MetersColumns", true);

    // Left: Controls
    colorEdit4("Safe", m_workingCopy.meters.safe);
    colorEdit4("Warning", m_workingCopy.meters.warning);
    colorEdit4("Clipping", m_workingCopy.meters.clipping);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();

    // Draw meter bars
    float meterWidth = ImGui::GetContentRegionAvail().x;
    float meterHeight = 20.0f;
    float spacing = 5.0f;

    // Safe level meter
    ImVec2 safePos = canvasPos;
    float  safeLevel = 0.6f; // 60% full
    drawList->AddRectFilled(
        safePos,
        ImVec2(safePos.x + meterWidth, safePos.y + meterHeight),
        IM_COL32(30, 30, 30, 255));
    drawList->AddRectFilled(
        safePos,
        ImVec2(safePos.x + meterWidth * safeLevel, safePos.y + meterHeight),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.meters.safe));
    ImGui::SetCursorScreenPos(ImVec2(safePos.x, safePos.y));
    ImGui::Text("Safe (60%):");
    ImGui::SetCursorScreenPos(ImVec2(safePos.x, safePos.y + meterHeight + spacing));

    // Warning level meter
    ImVec2 warnPos = ImGui::GetCursorScreenPos();
    float  warnLevel = 0.85f; // 85% full
    drawList->AddRectFilled(
        warnPos,
        ImVec2(warnPos.x + meterWidth, warnPos.y + meterHeight),
        IM_COL32(30, 30, 30, 255));
    drawList->AddRectFilled(
        warnPos,
        ImVec2(warnPos.x + meterWidth * 0.8f, warnPos.y + meterHeight),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.meters.safe));
    drawList->AddRectFilled(
        ImVec2(warnPos.x + meterWidth * 0.8f, warnPos.y),
        ImVec2(warnPos.x + meterWidth * warnLevel, warnPos.y + meterHeight),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.meters.warning));
    ImGui::SetCursorScreenPos(ImVec2(warnPos.x, warnPos.y));
    ImGui::Text("Warning (85%):");
    ImGui::SetCursorScreenPos(ImVec2(warnPos.x, warnPos.y + meterHeight + spacing));

    // Clipping level meter
    ImVec2 clipPos = ImGui::GetCursorScreenPos();
    float  clipLevel = 1.0f; // 100% full (clipping)
    drawList->AddRectFilled(
        clipPos,
        ImVec2(clipPos.x + meterWidth, clipPos.y + meterHeight),
        IM_COL32(30, 30, 30, 255));
    drawList->AddRectFilled(
        clipPos,
        ImVec2(clipPos.x + meterWidth * 0.8f, clipPos.y + meterHeight),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.meters.safe));
    drawList->AddRectFilled(
        ImVec2(clipPos.x + meterWidth * 0.8f, clipPos.y),
        ImVec2(clipPos.x + meterWidth * 0.95f, clipPos.y + meterHeight),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.meters.warning));
    drawList->AddRectFilled(
        ImVec2(clipPos.x + meterWidth * 0.95f, clipPos.y),
        ImVec2(clipPos.x + meterWidth * clipLevel, clipPos.y + meterHeight),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.meters.clipping));
    ImGui::SetCursorScreenPos(ImVec2(clipPos.x, clipPos.y));
    ImGui::Text("Clipping (100%):");

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderTimelineTab()
{
    ImGui::Text("Timeline Colors");
    ImGui::Separator();

    ImGui::Columns(2, "TimelineColumns", true);

    // Left: Controls
    colorEditU32("Marker Start/End", m_workingCopy.timeline.marker_start_end);
    colorEditU32("Marker Gate", m_workingCopy.timeline.marker_gate);
    colorEditU32("Marker Trigger", m_workingCopy.timeline.marker_trigger);

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();
    ImVec2      canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 150);

    // Draw timeline preview
    float  timelineHeight = 40.0f;
    ImVec2 timelineStart = ImVec2(canvasPos.x, canvasPos.y + 20);

    // Draw timeline background
    drawList->AddRectFilled(
        timelineStart,
        ImVec2(timelineStart.x + canvasSize.x, timelineStart.y + timelineHeight),
        IM_COL32(20, 20, 20, 255));

    // Draw markers
    // Start marker
    ImVec2 startMarker = ImVec2(timelineStart.x + 20, timelineStart.y);
    drawList->AddLine(
        ImVec2(startMarker.x, startMarker.y),
        ImVec2(startMarker.x, startMarker.y + timelineHeight),
        m_workingCopy.timeline.marker_start_end,
        3.0f);
    drawList->AddTriangleFilled(
        ImVec2(startMarker.x, startMarker.y),
        ImVec2(startMarker.x - 5, startMarker.y - 8),
        ImVec2(startMarker.x + 5, startMarker.y - 8),
        m_workingCopy.timeline.marker_start_end);
    ImGui::SetCursorScreenPos(ImVec2(startMarker.x - 10, startMarker.y - 20));
    ImGui::Text("Start");

    // End marker
    ImVec2 endMarker = ImVec2(timelineStart.x + canvasSize.x - 20, timelineStart.y);
    drawList->AddLine(
        ImVec2(endMarker.x, endMarker.y),
        ImVec2(endMarker.x, endMarker.y + timelineHeight),
        m_workingCopy.timeline.marker_start_end,
        3.0f);
    drawList->AddTriangleFilled(
        ImVec2(endMarker.x, endMarker.y),
        ImVec2(endMarker.x - 5, endMarker.y - 8),
        ImVec2(endMarker.x + 5, endMarker.y - 8),
        m_workingCopy.timeline.marker_start_end);
    ImGui::SetCursorScreenPos(ImVec2(endMarker.x - 10, endMarker.y - 20));
    ImGui::Text("End");

    // Gate markers
    float gateY = timelineStart.y + timelineHeight * 0.3f;
    for (int i = 0; i < 3; ++i)
    {
        float x = timelineStart.x + 60 + i * 40;
        drawList->AddRectFilled(
            ImVec2(x, gateY), ImVec2(x + 20, gateY + 15), m_workingCopy.timeline.marker_gate);
    }
    ImGui::SetCursorScreenPos(ImVec2(timelineStart.x + 60, gateY - 15));
    ImGui::Text("Gates");

    // Trigger markers
    float triggerY = timelineStart.y + timelineHeight * 0.7f;
    for (int i = 0; i < 5; ++i)
    {
        float x = timelineStart.x + 80 + i * 25;
        drawList->AddLine(
            ImVec2(x, triggerY),
            ImVec2(x, triggerY + 10),
            m_workingCopy.timeline.marker_trigger,
            2.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(timelineStart.x + 80, triggerY - 15));
    ImGui::Text("Triggers");

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y));

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderModulesTab()
{
    ImGui::Text("Module-Specific Colors");
    ImGui::Separator();

    ImGui::Columns(2, "ModulesColumns", true);

    // Left: Controls
    if (ImGui::CollapsingHeader("VideoFX Module"))
    {
        colorEdit4("Section Header", m_workingCopy.modules.videofx_section_header);
        colorEdit4("Section Subheader", m_workingCopy.modules.videofx_section_subheader);
    }

    if (ImGui::CollapsingHeader("Scope Module"))
    {
        colorEdit4("Section Header", m_workingCopy.modules.scope_section_header);
        colorEditU32("Plot Background", m_workingCopy.modules.scope_plot_bg);
        colorEditU32("Plot Foreground", m_workingCopy.modules.scope_plot_fg);
        colorEditU32("Plot Max", m_workingCopy.modules.scope_plot_max);
        colorEditU32("Plot Min", m_workingCopy.modules.scope_plot_min);
        colorEdit4("Text Max", m_workingCopy.modules.scope_text_max);
        colorEdit4("Text Min", m_workingCopy.modules.scope_text_min);
    }

    if (ImGui::CollapsingHeader("Stroke Sequencer"))
    {
        colorEditU32("Border", m_workingCopy.modules.stroke_seq_border);
        colorEditU32("Canvas Background", m_workingCopy.modules.stroke_seq_canvas_bg);
        colorEditU32("Line Inactive", m_workingCopy.modules.stroke_seq_line_inactive);
        colorEditU32("Line Active", m_workingCopy.modules.stroke_seq_line_active);
        colorEditU32("Playhead", m_workingCopy.modules.stroke_seq_playhead);
        colorEditU32("Threshold Floor", m_workingCopy.modules.stroke_seq_thresh_floor);
        colorEditU32("Threshold Mid", m_workingCopy.modules.stroke_seq_thresh_mid);
        colorEditU32("Threshold Ceil", m_workingCopy.modules.stroke_seq_thresh_ceil);
        colorEdit4("Frame Background", m_workingCopy.modules.stroke_seq_frame_bg);
        colorEdit4("Frame Hovered", m_workingCopy.modules.stroke_seq_frame_bg_hovered);
        colorEdit4("Frame Active", m_workingCopy.modules.stroke_seq_frame_bg_active);
    }

    if (ImGui::CollapsingHeader("PanVol Module"))
    {
        dragFloat("Node Width", m_workingCopy.modules.panvol_node_width, 1.0f, 120.0f, 400.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Custom width for PanVol node (default: 180px)");
        colorEditU32("Grid Background", m_workingCopy.modules.panvol_grid_background);
        colorEditU32("Grid Border", m_workingCopy.modules.panvol_grid_border);
        colorEditU32("Grid Lines", m_workingCopy.modules.panvol_grid_lines);
        colorEditU32("Crosshair", m_workingCopy.modules.panvol_crosshair);
        colorEditU32("Circle (Manual)", m_workingCopy.modules.panvol_circle_manual);
        colorEditU32("Circle (Modulated)", m_workingCopy.modules.panvol_circle_modulated);
        colorEditU32("Label Text", m_workingCopy.modules.panvol_label_text);
        colorEditU32("Value Text", m_workingCopy.modules.panvol_value_text);
    }

    ImGui::NextColumn();

    // Right: Live Preview
    ImGui::Text("Live Preview");
    ImGui::Separator();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2      canvasPos = ImGui::GetCursorScreenPos();

    // VideoFX Preview
    ImGui::TextColored(m_workingCopy.modules.videofx_section_header, "VideoFX Section Header");
    ImGui::TextColored(
        m_workingCopy.modules.videofx_section_subheader, "VideoFX Section Subheader");
    ImGui::Spacing();

    // Scope Preview
    ImGui::TextColored(m_workingCopy.modules.scope_section_header, "Scope Section Header");
    ImVec2 scopePos = ImVec2(canvasPos.x, ImGui::GetCursorScreenPos().y + 10);
    ImVec2 scopeSize = ImVec2(200, 100);

    // Draw scope background
    drawList->AddRectFilled(
        scopePos,
        ImVec2(scopePos.x + scopeSize.x, scopePos.y + scopeSize.y),
        m_workingCopy.modules.scope_plot_bg);
    drawList->AddRect(
        scopePos,
        ImVec2(scopePos.x + scopeSize.x, scopePos.y + scopeSize.y),
        IM_COL32(100, 100, 100, 255));

    // Draw scope waveform
    for (int i = 0; i < (int)scopeSize.x - 4; i += 2)
    {
        float x = scopePos.x + 2 + i;
        float y = scopePos.y + scopeSize.y * 0.5f + sinf(i * 0.1f) * scopeSize.y * 0.3f;
        drawList->AddLine(
            ImVec2(x, y),
            ImVec2(x + 2, y + sinf((i + 2) * 0.1f) * scopeSize.y * 0.3f),
            m_workingCopy.modules.scope_plot_fg,
            1.5f);
    }

    // Draw max/min markers
    drawList->AddLine(
        ImVec2(scopePos.x + 5, scopePos.y + 5),
        ImVec2(scopePos.x + scopeSize.x - 5, scopePos.y + 5),
        m_workingCopy.modules.scope_plot_max,
        2.0f);
    drawList->AddLine(
        ImVec2(scopePos.x + 5, scopePos.y + scopeSize.y - 5),
        ImVec2(scopePos.x + scopeSize.x - 5, scopePos.y + scopeSize.y - 5),
        m_workingCopy.modules.scope_plot_min,
        2.0f);

    ImGui::SetCursorScreenPos(ImVec2(scopePos.x + 5, scopePos.y + 5));
    ImGui::TextColored(m_workingCopy.modules.scope_text_max, "MAX");
    ImGui::SetCursorScreenPos(ImVec2(scopePos.x + 5, scopePos.y + scopeSize.y - 20));
    ImGui::TextColored(m_workingCopy.modules.scope_text_min, "MIN");

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, scopePos.y + scopeSize.y + 20));
    ImGui::Spacing();

    // Stroke Sequencer Preview
    ImGui::Text("Stroke Sequencer Preview:");
    ImVec2 seqPos = ImVec2(canvasPos.x, ImGui::GetCursorScreenPos().y + 5);
    ImVec2 seqSize = ImVec2(250, 120);

    // Draw canvas background
    drawList->AddRectFilled(
        seqPos,
        ImVec2(seqPos.x + seqSize.x, seqPos.y + seqSize.y),
        m_workingCopy.modules.stroke_seq_canvas_bg);
    drawList->AddRect(
        seqPos,
        ImVec2(seqPos.x + seqSize.x, seqPos.y + seqSize.y),
        m_workingCopy.modules.stroke_seq_border,
        0.0f,
        0,
        2.0f);

    // Draw lines
    float lineY = seqPos.y + 20;
    for (int i = 0; i < 5; ++i)
    {
        float x1 = seqPos.x + 10 + i * 20;
        float x2 = seqPos.x + 10 + (i + 1) * 20;
        ImU32 lineColor = (i == 2) ? m_workingCopy.modules.stroke_seq_line_active
                                   : m_workingCopy.modules.stroke_seq_line_inactive;
        drawList->AddLine(ImVec2(x1, lineY + i * 15), ImVec2(x2, lineY + i * 15), lineColor, 2.0f);
    }

    // Draw playhead
    float playheadX = seqPos.x + 100;
    drawList->AddLine(
        ImVec2(playheadX, seqPos.y),
        ImVec2(playheadX, seqPos.y + seqSize.y),
        m_workingCopy.modules.stroke_seq_playhead,
        2.0f);

    // Draw threshold markers
    float threshY1 = seqPos.y + seqSize.y * 0.3f;
    float threshY2 = seqPos.y + seqSize.y * 0.5f;
    float threshY3 = seqPos.y + seqSize.y * 0.7f;
    drawList->AddLine(
        ImVec2(seqPos.x + 5, threshY1),
        ImVec2(seqPos.x + seqSize.x - 5, threshY1),
        m_workingCopy.modules.stroke_seq_thresh_floor,
        1.0f);
    drawList->AddLine(
        ImVec2(seqPos.x + 5, threshY2),
        ImVec2(seqPos.x + seqSize.x - 5, threshY2),
        m_workingCopy.modules.stroke_seq_thresh_mid,
        1.0f);
    drawList->AddLine(
        ImVec2(seqPos.x + 5, threshY3),
        ImVec2(seqPos.x + seqSize.x - 5, threshY3),
        m_workingCopy.modules.stroke_seq_thresh_ceil,
        1.0f);

    // Draw frame preview
    ImVec2 framePos = ImVec2(seqPos.x + seqSize.x - 60, seqPos.y + 10);
    ImVec2 frameSize = ImVec2(50, 30);
    drawList->AddRectFilled(
        framePos,
        ImVec2(framePos.x + frameSize.x, framePos.y + frameSize.y),
        ImGui::ColorConvertFloat4ToU32(m_workingCopy.modules.stroke_seq_frame_bg));
    drawList->AddRect(
        framePos,
        ImVec2(framePos.x + frameSize.x, framePos.y + frameSize.y),
        IM_COL32(100, 100, 100, 255));

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, seqPos.y + seqSize.y + 20));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // PanVol Preview
    ImGui::Text("PanVol Preview:");
    ImVec2 panvolPos = ImVec2(canvasPos.x, ImGui::GetCursorScreenPos().y + 5);
    float  panvolWidth = m_workingCopy.modules.panvol_node_width;
    if (panvolWidth > 200.0f)
        panvolWidth = 200.0f; // Limit for preview
    float  gridSize = juce::jmin(panvolWidth - 20.0f, 100.0f);
    ImVec2 gridMin = ImVec2(panvolPos.x + (panvolWidth - gridSize) * 0.5f, panvolPos.y + 10);
    ImVec2 gridMax = ImVec2(gridMin.x + gridSize, gridMin.y + gridSize);

    // Draw grid
    drawList->AddRectFilled(gridMin, gridMax, m_workingCopy.modules.panvol_grid_background);
    drawList->AddRect(gridMin, gridMax, m_workingCopy.modules.panvol_grid_border, 0.0f, 0, 2.0f);

    // Draw grid lines
    for (int i = 1; i < 4; ++i)
    {
        float t = (float)i / 4.0f;
        float x = gridMin.x + t * gridSize;
        float y = gridMin.y + t * gridSize;
        drawList->AddLine(
            ImVec2(x, gridMin.y),
            ImVec2(x, gridMax.y),
            m_workingCopy.modules.panvol_grid_lines,
            1.0f);
        drawList->AddLine(
            ImVec2(gridMin.x, y),
            ImVec2(gridMax.x, y),
            m_workingCopy.modules.panvol_grid_lines,
            1.0f);
    }

    // Draw crosshair
    ImVec2 center = ImVec2(gridMin.x + gridSize * 0.5f, gridMin.y + gridSize * 0.5f);
    drawList->AddLine(
        ImVec2(center.x, gridMin.y),
        ImVec2(center.x, gridMax.y),
        m_workingCopy.modules.panvol_crosshair,
        1.0f);
    drawList->AddLine(
        ImVec2(gridMin.x, center.y),
        ImVec2(gridMax.x, center.y),
        m_workingCopy.modules.panvol_crosshair,
        1.0f);

    // Draw circle (manual state)
    drawList->AddCircleFilled(center, 6.0f, m_workingCopy.modules.panvol_circle_manual, 16);
    drawList->AddCircle(center, 6.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);

    // Draw labels
    drawList->AddText(
        ImVec2(gridMin.x + 2, gridMin.y + 2), m_workingCopy.modules.panvol_label_text, "Vol");
    ImVec2 panTextSize = ImGui::CalcTextSize("Pan");
    drawList->AddText(
        ImVec2(gridMax.x - panTextSize.x - 2, gridMin.y + 2),
        m_workingCopy.modules.panvol_label_text,
        "Pan");

    // Draw value readouts
    drawList->AddText(
        ImVec2(gridMin.x + 2, gridMin.y + 2 + ImGui::GetFontSize() + 2),
        m_workingCopy.modules.panvol_value_text,
        "-55.1dB");
    ImVec2 panValSize = ImGui::CalcTextSize("0.76");
    drawList->AddText(
        ImVec2(gridMax.x - panValSize.x - 2, gridMin.y + 2 + ImGui::GetFontSize() + 2),
        m_workingCopy.modules.panvol_value_text,
        "0.76");

    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, gridMax.y + 20));
    ImGui::Text("Node Width: %.0fpx", m_workingCopy.modules.panvol_node_width);

    ImGui::Columns(1);
}

void ThemeEditorComponent::renderSaveDialog()
{
    ImGui::OpenPopup("Save Theme");
    if (ImGui::BeginPopupModal("Save Theme", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Enter theme name:");
        ImGui::InputText("##ThemeName", m_saveThemeName, sizeof(m_saveThemeName));

        ImGui::Separator();

        if (ImGui::Button("Save"))
        {
            saveThemeAs();
            m_showSaveDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_showSaveDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void ThemeEditorComponent::saveTheme()
{
    // Save to current theme file (if one exists)
    if (m_currentThemeFilename.isEmpty())
    {
        // No current theme file, should use Save As instead
        juce::Logger::writeToLog(
            "[ThemeEditor] Cannot save: no current theme file. Use 'Save As...' instead.");
        return;
    }

    // Find the current theme file
    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto themesDir = exeDir.getChildFile("themes");
    auto themeFile = themesDir.getChildFile(m_currentThemeFilename);

    // Also check source tree as fallback
    if (!themeFile.existsAsFile())
    {
        auto sourceThemesDir = exeDir.getParentDirectory()
                                   .getParentDirectory()
                                   .getChildFile("Source")
                                   .getChildFile("preset_creator")
                                   .getChildFile("theme")
                                   .getChildFile("presets");
        themeFile = sourceThemesDir.getChildFile(m_currentThemeFilename);
    }

    if (!themeFile.existsAsFile())
    {
        juce::Logger::writeToLog(
            "[ThemeEditor] ERROR: Current theme file not found: " + m_currentThemeFilename);
        return;
    }

    ThemeManager::getInstance().getEditableTheme() = m_workingCopy;
    if (ThemeManager::getInstance().saveTheme(themeFile))
    {
        juce::Logger::writeToLog("[ThemeEditor] Saved theme to: " + themeFile.getFullPathName());
        m_hasChanges = false;
    }
    else
    {
        juce::Logger::writeToLog(
            "[ThemeEditor] ERROR saving theme: " + themeFile.getFullPathName());
    }
}

void ThemeEditorComponent::saveThemeAs()
{
    juce::String themeName(m_saveThemeName);
    if (themeName.isEmpty())
    {
        return;
    }

    themeName = themeName.replaceCharacter(' ', '_');

    // Save to exe/themes folder
    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto themesDir = exeDir.getChildFile("themes");
    themesDir.createDirectory();
    auto themeFile = themesDir.getChildFile(themeName + ".json");

    ThemeManager::getInstance().getEditableTheme() = m_workingCopy;
    if (ThemeManager::getInstance().saveTheme(themeFile))
    {
        juce::Logger::writeToLog("[ThemeEditor] Saved theme to: " + themeFile.getFullPathName());
        // Update current theme filename
        m_currentThemeFilename = themeFile.getFileName();
        // Persist as last-used
        ThemeManager::getInstance().saveUserThemePreference(themeFile.getFileName());
        m_hasChanges = false;
    }
    else
    {
        juce::Logger::writeToLog(
            "[ThemeEditor] ERROR saving theme: " + themeFile.getFullPathName());
    }
}

void ThemeEditorComponent::resetCurrentTab()
{
    // TODO: Reset current tab to default values
    // For now, just reload from current theme
    m_workingCopy = ThemeManager::getInstance().getCurrentTheme();
    m_hasChanges = false;
}

void ThemeEditorComponent::applyChanges()
{
    // Apply working copy to ThemeManager
    ThemeManager::getInstance().getEditableTheme() = m_workingCopy;
    ThemeManager::getInstance().applyTheme();
    ThemeManager::getInstance().requestFontReload();
    m_hasChanges = false;
    juce::Logger::writeToLog("[ThemeEditor] Applied theme changes");

    // Also save user preference if they want persistence
    // (Theme preference is saved when selecting from menu, not when editing)
}

void ThemeEditorComponent::syncFontBuffersFromWorkingCopy()
{
    const std::string defaultPath = m_workingCopy.fonts.default_path.toStdString();
    std::snprintf(
        m_defaultFontPathBuffer.data(), m_defaultFontPathBuffer.size(), "%s", defaultPath.c_str());
    m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
}

void ThemeEditorComponent::previewFontChanges()
{
    auto& liveFonts = ThemeManager::getInstance().getEditableTheme().fonts;
    liveFonts.default_path = m_workingCopy.fonts.default_path;
    liveFonts.default_size = m_workingCopy.fonts.default_size;
    ThemeManager::getInstance().requestFontReload();
    m_selectedFontIndex = findScannedFontIndex(m_workingCopy.fonts.default_path);
}

void ThemeEditorComponent::scanFontFolder()
{
    m_scannedFontFiles.clear();

    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto fontsDir = exeDir.getChildFile("fonts");

    juce::Logger::writeToLog("[ThemeEditor] Scanning for fonts in: " + fontsDir.getFullPathName());

    if (fontsDir.isDirectory())
    {
        auto addFontsForPattern = [this, &fontsDir, &exeDir](const juce::String& pattern) {
            juce::Array<juce::File> files;
            fontsDir.findChildFiles(files, juce::File::findFiles, false, pattern);
            for (const auto& file : files)
            {
                const juce::String relativePath = file.getRelativePathFrom(exeDir);
                if (!m_scannedFontFiles.contains(relativePath))
                    m_scannedFontFiles.add(relativePath);
            }
        };

        addFontsForPattern("*.ttf");
        addFontsForPattern("*.otf");
        addFontsForPattern("*.ttc");
    }

    m_scannedFontFiles.sort(true);
    juce::Logger::writeToLog(
        "[ThemeEditor] Found " + juce::String(m_scannedFontFiles.size()) + " font files.");
}

int ThemeEditorComponent::findScannedFontIndex(const juce::String& path) const
{
    if (path.isEmpty())
        return -1;

    const juce::String normalised = resolveFontPath(path).getFullPathName();

    for (int i = 0; i < m_scannedFontFiles.size(); ++i)
    {
        if (resolveFontPath(m_scannedFontFiles[i]).getFullPathName() == normalised)
            return i;
    }
    return -1;
}
