# ImNodes Widget Bleeding Fix - Collapsible Headers Guide

## The Problem

When using ImGui widgets like `CollapsingHeader`, `TreeNodeEx`, `SliderFloat`, `Combo`, and `Separator` inside ImNodes nodes, they "bleed" outside the node's visual boundaries. This happens because these widgets calculate their width based on `WorkRect.Max.x`, which in ImNodes is set to the entire canvas width (potentially thousands of pixels) rather than the node's content width.

![Widget Bleeding Example](The widgets extend far beyond the node boundary)

### Affected Widgets
- `ImGui::CollapsingHeader`
- `ImGui::TreeNodeEx`
- `ImGui::SliderFloat` / `SliderInt` (with labels)
- `ImGui::Combo` / `BeginCombo`
- `ImGui::Separator` / `SeparatorText`
- Most widgets that use `WorkRect.Max.x` for sizing

---

## The Solution: WorkRect Constraint Workaround

The fix involves temporarily modifying the internal ImGui window state to constrain widget drawing to the node's actual width.

### Required Include

```cpp
#include <imgui_internal.h> // For WorkRect workaround
```

### Implementation Pattern

At the **start** of your `drawParametersInNode` function:

```cpp
void MyModule::drawParametersInNode(
    float itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>& onModificationEnded)
{
    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    // Widgets like CollapsingHeader, TreeNodeEx, SliderFloat use WorkRect.Max.x
    // which is the entire canvas in ImNodes, causing them to extend beyond node bounds.
    // Solution: Temporarily constrain WorkRect and ContentRegionRect to node width.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float cursorX = ImGui::GetCursorPosX();
    const float nodeRightEdge = cursorX + itemWidth;
    
    // Save original values
    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;
    
    // Constrain to node width
    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    ImGui::PushItemWidth(itemWidth);
    
    // ... your widget drawing code ...
```

At the **end** of the function (before closing brace):

```cpp
    // ... your widget drawing code ...

    ImGui::PopItemWidth();
    
    // === RESTORE WORKRECT VALUES ===
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}
```

---

## Using Collapsible Headers (TreeNodeEx)

For collapsible sections that respect node width, use `ImGui::TreeNodeEx` with appropriate flags:

```cpp
// Use TreeNodeEx with SpanAvailWidth to fill node width
bool expanded = ImGui::TreeNodeEx(
    "Section Name",
    ImGuiTreeNodeFlags_SpanAvailWidth |  // Fill available width
    ImGuiTreeNodeFlags_Framed |          // Draw a frame around header
    ImGuiTreeNodeFlags_AllowOverlap      // Allow other items to overlap
);

if (expanded)
{
    // Draw your controls here
    ImGui::SliderFloat("Parameter", &value, 0.0f, 1.0f);
    
    ImGui::TreePop(); // MUST match TreeNodeEx
}
```

### Setting Default Open State

```cpp
// Make first item open by default (only on first frame)
ImGui::SetNextItemOpen(true, ImGuiCond_Once);

bool expanded = ImGui::TreeNodeEx("Layer 1", 
    ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed);
```

---

## Complete Example: VideoFXModule.cpp

Here's how VideoFXModule implements the pattern:

```cpp
#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#include <imgui_internal.h> // Required for WorkRect access
#include "../../preset_creator/theme/ThemeManager.h"
#endif

void VideoFXModule::drawParametersInNode(
    float itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>& onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float cursorX = ImGui::GetCursorPosX();
    const float nodeRightEdge = cursorX + itemWidth;
    
    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;
    
    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    ImGui::PushItemWidth(itemWidth);

    // Reset button
    if (ImGui::Button("Reset All Effects", ImVec2(itemWidth, 0)))
    {
        // Reset logic...
    }

    // Collapsible Color Adjustments section
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Color Adjustments",
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed))
    {
        ImGui::SliderFloat("Brightness", &brightness, -100.0f, 100.0f);
        ImGui::SliderFloat("Contrast", &contrast, 0.0f, 3.0f);
        ImGui::SliderFloat("Saturation", &saturation, 0.0f, 3.0f);
        
        ImGui::TreePop();
    }

    // ... more sections ...

    ImGui::PopItemWidth();
    
    // === RESTORE WORKRECT VALUES ===
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}
```

---

## Key Points

1. **Always include `<imgui_internal.h>`** - Required for `ImGuiWindow*` access

2. **Save values at start, restore at end** - Prevents affecting other nodes

3. **Use `juce::jmin()` not direct assignment** - In case node is smaller than canvas

4. **Match TreeNodeEx with TreePop** - Every expanded TreeNodeEx needs TreePop

5. **Use SpanAvailWidth flag** - Makes headers fill available width properly

6. **Don't use `GetContentRegionAvail().x`** - Returns canvas width, not node width

---

## References

- [ImNodes Issue #167: CollapsingHeader goes out of bounds](https://github.com/Nelarius/imnodes/issues/167)
- [imgui-node-editor: Widget clipping workarounds](https://github.com/thedmd/imgui-node-editor/issues/)
- Related files in this project:
  - `VideoCompositorModule.cpp` - Full implementation with Layer collapsibles
  - `VideoFXModule.cpp` - Implementation with WorkRect constraint

---

## Smart Collapsible Output Pins (Connection-Aware)

For modules with many output pins (e.g., `HandTrackerModule` with 50+ pins), use **smart collapsible sections** that:
- Group pins logically (by finger, channel group, etc.)
- Hide unconnected pins when collapsed
- **Show connected pins even when collapsed** (so cables remain visible)

### Extending NodePinHelpers

Add `isOutputPinConnected` callback to `NodePinHelpers` in `ModuleProcessor.h`:

```cpp
struct NodePinHelpers
{
    // ... existing callbacks ...
    
    // Check if an output pin has any connections (for smart collapsible sections)
    std::function<bool(int busIndex, int channel)> isOutputPinConnected;
};
```

### Implementation Pattern in drawIoPins

> [!WARNING]
> **Do NOT use `TreeNodeEx` in `drawIoPins`!** It causes glitches in ImNodes due to state conflicts. Use `Selectable` with manual toggle instead.

> [!IMPORTANT]
> You MUST apply the WorkRect constraint in `drawIoPins` to prevent Selectable from triggering when mouse is outside the node. Include `<imgui_internal.h>` for access to ImGuiWindow.

```cpp
#include <imgui_internal.h> // Required for WorkRect workaround

void MyModule::drawIoPins(const NodePinHelpers& helpers)
{
    // === WORKAROUND FOR IMNODES WIDGET BLEEDING ===
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float  cursorX = ImGui::GetCursorPosX();
    const float  nodeWidth = getCustomNodeSize().x > 0 ? getCustomNodeSize().x : 240.0f;
    const float  nodeRightEdge = cursorX + nodeWidth;

    const float savedWorkRectMaxX = window->WorkRect.Max.x;
    const float savedContentRegionMaxX = window->ContentRegionRect.Max.x;

    window->WorkRect.Max.x = juce::jmin(savedWorkRectMaxX, nodeRightEdge);
    window->ContentRegionRect.Max.x = juce::jmin(savedContentRegionMaxX, nodeRightEdge);

    // Define pin groups
    struct PinGroup {
        const char* name;
        int startChannel;
        int count;
    };
    
    const PinGroup groups[] = {
        {"Group A", 0, 4},
        {"Group B", 4, 4},
    };
    constexpr int numGroups = 2;
    
    // Static collapse state per node instance
    static std::map<juce::uint32, std::array<bool, numGroups>> collapseState;
    juce::uint32 nodeId = getLogicalId();
    if (collapseState.find(nodeId) == collapseState.end())
        collapseState[nodeId] = {true, true};
    
    auto& collapsed = collapseState[nodeId];
    
    for (int g = 0; g < numGroups; ++g)
    {
        const auto& group = groups[g];
        
        // Check if any pin in group is connected
        bool hasConnections = false;
        if (helpers.isOutputPinConnected)
        {
            for (int i = 0; i < group.count; ++i)
            {
                if (helpers.isOutputPinConnected(0, group.startChannel + i))
                {
                    hasConnections = true;
                    break;
                }
            }
        }
        
        ImGui::PushID(g);
        
        // Build header with arrow and connection indicator
        juce::String header;
        header << (collapsed[g] ? "\xE2\x96\xB6 " : "\xE2\x96\xBC "); // ▶ or ▼
        header << group.name;
        if (collapsed[g] && hasConnections)
            header << " [\xE2\x97\x8F]"; // ●
        
        // Use Selectable for toggle (doesn't bleed like TreeNodeEx)
        if (ImGui::Selectable(header.toRawUTF8(), false))
            collapsed[g] = !collapsed[g];
        
        if (!collapsed[g])
        {
            for (int i = 0; i < group.count; ++i)
                helpers.drawAudioOutputPin(pinLabels[i], group.startChannel + i);
        }
        else
        {
            if (helpers.isOutputPinConnected)
            {
                for (int i = 0; i < group.count; ++i)
                {
                    int ch = group.startChannel + i;
                    if (helpers.isOutputPinConnected(0, ch))
                        helpers.drawAudioOutputPin(pinLabels[i], ch);
                }
            }
        }
        ImGui::PopID();
    }

    // === RESTORE WORKRECT VALUES ===
    window->WorkRect.Max.x = savedWorkRectMaxX;
    window->ContentRegionRect.Max.x = savedContentRegionMaxX;
}
```

### Key Points

1. **Use `Selectable` not `TreeNodeEx`** - TreeNodeEx causes glitches in ImNodes due to state conflicts
2. **Apply WorkRect constraint** - Prevents widget hit areas from extending past node bounds
3. **Manual arrow indicators** - Use `▶` (collapsed) and `▼` (expanded) Unicode characters
4. **Connection indicator `[●]`** - Shows when collapsed section has active cables
5. **Static collapse state per node** - Uses `getLogicalId()` to track per-instance
6. **Override `usesCustomPinLayout()`** - Must return `true` for `drawIoPins` to be called

### Required Override

Add this to your module header:

```cpp
#if defined(PRESET_CREATOR_UI)
    void drawIoPins(const NodePinHelpers& helpers) override;
    bool usesCustomPinLayout() const override { return true; }
#endif
```
