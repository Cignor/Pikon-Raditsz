#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath> // For std::abs
#include <cstdio> // For snprintf
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class ReverbModuleProcessor : public ModuleProcessor
{
public:
    ReverbModuleProcessor();
    ~ReverbModuleProcessor() override = default;

    const juce::String getName() const override { return "reverb"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }
    
    // Parameter bus contract implementation
    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;
    
    std::vector<DynamicPinInfo> getDynamicInputPins() const override;
    std::vector<DynamicPinInfo> getDynamicOutputPins() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers = nullptr) override
    {
        auto& ap = getAPVTS();
        const auto& theme = ThemeManager::getInstance().getCurrentTheme();
        
        ImGui::PushID(this);
        ImGui::PushItemWidth(itemWidth);
        
        // Helper for tooltips
        auto HelpMarker = [](const char* desc) {
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted(desc);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        };
        
        // === REVERB VISUALIZATION ===
        ImGui::Spacing();
        ImGui::Text("Waveform & Reverb Tail");
        ImGui::Spacing();
        
        const float vizWidth = itemWidth;
        const float vizHeight = 120.0f;
        const float currentSize = vizData.currentSize.load();
        const float currentDamp = vizData.currentDamp.load();
        const float currentMix = vizData.currentMix.load();
        const float reverbActivity = vizData.reverbActivity.load();
        auto& themeMgr = ThemeManager::getInstance();
        auto resolveColor = [](ImU32 primary, ImU32 secondary, ImU32 tertiary) -> ImU32 {
            if (primary != 0) return primary;
            if (secondary != 0) return secondary;
            return tertiary;
        };
        const ImU32 canvasBg = themeMgr.getCanvasBackground();
        const ImVec4 childBgVec4 = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
        const ImU32 childBg = ImGui::ColorConvertFloat4ToU32(childBgVec4);
        const ImU32 bgColor = resolveColor(theme.modules.scope_plot_bg, canvasBg, childBg);
        const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
        const ImU32 frequencyColor = ImGui::ColorConvertFloat4ToU32(ImVec4(frequencyColorVec4.x, frequencyColorVec4.y, frequencyColorVec4.z, 0.8f));
        const ImU32 inputWaveformColor = resolveColor(theme.modules.scope_plot_fg, frequencyColor, IM_COL32(100, 220, 255, 200));
        const ImVec4 timbreColorVec4 = theme.modulation.timbre;
        const ImU32 timbreColor = ImGui::ColorConvertFloat4ToU32(ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f));
        const ImU32 outputWaveformColor = (timbreColor != 0) ? timbreColor : IM_COL32(255, 180, 80, 255);
        const ImU32 scopePlotFg = theme.modules.scope_plot_fg;
        const ImU32 centerLineColorBase = resolveColor(scopePlotFg, frequencyColor, IM_COL32(150, 150, 150, 100));
        const ImVec4 centerLineVec4 = ImGui::ColorConvertU32ToFloat4(centerLineColorBase);
        const ImU32 centerLineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(centerLineVec4.x, centerLineVec4.y, centerLineVec4.z, 0.4f));
        ImGui::PushID("ReverbWaveViz");
        if (ImGui::BeginChild("ReverbWaveViz", ImVec2(vizWidth, vizHeight), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetWindowPos();
            const ImVec2 childSize = ImGui::GetWindowSize();
            const ImVec2 rectMax = ImVec2(origin.x + childSize.x, origin.y + childSize.y);
            ImGui::PushClipRect(origin, rectMax, true);
        
        // Read visualization data (thread-safe)
        float inputWaveform[VizData::waveformPoints];
        float outputWaveform[VizData::waveformPoints];
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            inputWaveform[i] = vizData.inputWaveformL[i].load();
            outputWaveform[i] = vizData.outputWaveformL[i].load();
        }
        
        const float midY = origin.y + childSize.y * 0.5f;
        const float scaleY = childSize.y * 0.4f;
        const float stepX = childSize.x / (float)(VizData::waveformPoints - 1);
        float prevX = origin.x;
        float prevY = midY;
        
        // Draw center line (thicker for visibility)
        drawList->AddLine(ImVec2(origin.x, midY), ImVec2(rectMax.x, midY), centerLineColor, 1.5f);
        
        // Detect where input stops to mark reverb tail start
        float inputEnergy = 0.0f;
        float maxInputEnergy = 0.0f;
        int reverbTailStartIdx = -1;
        
        // First pass: find where input energy drops significantly (reverb tail starts)
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float inputSample = juce::jlimit(-1.0f, 1.0f, inputWaveform[i]);
            inputEnergy += std::abs(inputSample);
            if (i > 0 && (i % 32) == 0) // Check every 32 samples
            {
                const float avgEnergy = inputEnergy / 32.0f;
                if (avgEnergy > maxInputEnergy)
                    maxInputEnergy = avgEnergy;
                if (reverbTailStartIdx == -1 && avgEnergy < maxInputEnergy * 0.1f && maxInputEnergy > 0.05f)
                {
                    reverbTailStartIdx = i - 16; // Mark slightly before the drop
                }
                inputEnergy = 0.0f;
            }
        }
        
        // Draw output waveform FIRST (as background, shows reverb tail extending)
        // Make it more subtle/faded so input waveform is more prominent
        prevX = origin.x; prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveform[i]);
            const float x = origin.x + i * stepX;
            const float y = midY - sample * scaleY;
            
            if (i > 0)
            {
                // Make reverb tail more subtle - lower opacity overall
                const ImVec4 timbreVec4 = ImGui::ColorConvertU32ToFloat4(outputWaveformColor);
                
                // Base opacity is lower for background effect
                float baseAlpha = 0.25f;
                
                // Further fade the reverb tail region
                if (reverbTailStartIdx >= 0 && i > reverbTailStartIdx)
                {
                    const float tailProgress = (float)(i - reverbTailStartIdx) / (float)(VizData::waveformPoints - reverbTailStartIdx);
                    baseAlpha *= (1.0f - tailProgress * 0.6f); // Fade even more in tail region
                }
                
                const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    timbreVec4.x, timbreVec4.y, timbreVec4.z, 
                    baseAlpha));
                
                // Thinner line for background effect
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), lineColor, 1.8f);
            }
            prevX = x; prevY = y;
        }
        
        // Draw reverb tail start marker (vertical line where input stops)
        if (reverbTailStartIdx >= 0 && reverbTailStartIdx < VizData::waveformPoints)
        {
            const float tailStartX = origin.x + reverbTailStartIdx * stepX;
            const ImU32 tailMarkerColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
                timbreColorVec4.x * 0.7f, timbreColorVec4.y * 0.7f, timbreColorVec4.z * 0.7f, 0.6f));
            drawList->AddLine(ImVec2(tailStartX, origin.y), ImVec2(tailStartX, rectMax.y), tailMarkerColor, 2.0f);
            // Add label
            const char* tailLabel = "Reverb Tail";
            const ImVec2 textSize = ImGui::CalcTextSize(tailLabel);
            drawList->AddText(ImVec2(tailStartX + 4.0f, origin.y + 2.0f), tailMarkerColor, tailLabel);
        }
        
        // Draw reverb activity halo (outline only to avoid heavy fill batches)
        if (reverbActivity > 0.01f)
        {
            const float intensity = juce::jlimit(0.0f, 1.0f, reverbActivity * 1.6f);
            const float haloHalfHeight = scaleY * (0.15f + intensity * 0.55f);
            const ImVec4 haloVec4(
                timbreColorVec4.x,
                timbreColorVec4.y,
                timbreColorVec4.z,
                juce::jlimit(0.08f, 0.28f, intensity * 0.22f + 0.08f));
            const ImU32 haloColor = ImGui::ColorConvertFloat4ToU32(haloVec4);
            
            const ImVec2 haloMin(origin.x, juce::jmax(origin.y, midY - haloHalfHeight));
            const ImVec2 haloMax(rectMax.x, juce::jmin(rectMax.y, midY + haloHalfHeight));
            
            // Outer halo outline
            drawList->AddRect(haloMin, haloMax, haloColor, 6.0f, 0, 2.5f);
            
            // Inner dashed guides to suggest diffusion without filling outside clip
            const int dashCount = 12;
            for (int d = 0; d < dashCount; ++d)
            {
                const float t = (float)d / (float)(dashCount - 1);
                const float y = haloMin.y + t * (haloMax.y - haloMin.y);
                const float dashWidth = juce::jmap(std::abs(t - 0.5f), 0.0f, 0.5f, haloMax.x - haloMin.x, (haloMax.x - haloMin.x) * 0.5f);
                const float xStart = origin.x + (vizWidth - dashWidth) * 0.5f;
                const float xEnd = xStart + dashWidth;
                drawList->AddLine(ImVec2(xStart, y), ImVec2(xEnd, y), haloColor, 1.2f);
            }
        }
        
        // Draw input waveform ON TOP (more prominent, shows original sound)
        prevX = origin.x; prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, inputWaveform[i]);
            const float x = origin.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0) drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), inputWaveformColor, 3.6f);
            prevX = x; prevY = y;
        }
        
            ImGui::PopClipRect();
        }
        ImGui::EndChild();
        ImGui::PopID();
        
        // Parameter meters
        const ImVec4 accentVec4 = theme.accent;
        const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentVec4.x, accentVec4.y, accentVec4.z, 1.0f));
        
        const ImVec4 meterTimbreColor = theme.modulation.timbre;
        ImGui::Text("Size: %.2f", currentSize);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
        ImGui::ProgressBar(currentSize, ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", currentSize * 100.0f);
        
        ImGui::Text("Damp: %.2f", currentDamp);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
        ImGui::ProgressBar(currentDamp, ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", currentDamp * 100.0f);
        
        // Calculate RT60 and room type
        const float rt60 = currentSize * 3.0f + 0.5f;
        const float dampFactor = 1.0f - (currentDamp * 0.7f);
        const char* roomType = (currentSize < 0.3f) ? "Small Room" : (currentSize < 0.7f) ? "Medium Hall" : "Large Cathedral";
        
        // Enhanced parameter display with visual indicators
        ImGui::Text("Mix: %.2f", currentMix);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
        ImGui::ProgressBar(currentMix, ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", currentMix * 100.0f);
        
        // Room type and RT60 with visual indicator
        ImGui::Text("%s | RT60: %.2f s", roomType, rt60 * dampFactor);
        
        // Reverb activity meter (visual feedback)
        ImGui::Text("Activity: %.2f", reverbActivity);
        const float activityMeter = juce::jlimit(0.0f, 1.0f, reverbActivity * 2.0f); // Scale for visibility
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertFloat4ToU32(ImVec4(
            meterTimbreColor.x, meterTimbreColor.y, meterTimbreColor.z, 0.8f)));
        ImGui::ProgressBar(activityMeter, ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", activityMeter * 100.0f);
        
        ImGui::Spacing();
        
        // === DECAY CURVE VISUALIZATION ===
        ImGui::Text("Decay Envelope");
        ImGui::Spacing();
        
        const float decayWidth = itemWidth;
        const float decayHeight = 60.0f;
        ImGui::PushID("ReverbDecayViz");
        if (ImGui::BeginChild("ReverbDecayViz", ImVec2(decayWidth, decayHeight), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 decayOrigin = ImGui::GetWindowPos();
            const ImVec2 decayRectMax = ImVec2(decayOrigin.x + decayWidth, decayOrigin.y + decayHeight);
            
            drawList->AddRectFilled(decayOrigin, decayRectMax, bgColor, 4.0f);
            ImGui::PushClipRect(decayOrigin, decayRectMax, true);
        
        // Read decay curve
        float decayCurve[VizData::decayCurvePoints];
        for (int i = 0; i < VizData::decayCurvePoints; ++i)
        {
            decayCurve[i] = vizData.decayCurve[i].load();
        }
        
        const float decayMidY = decayOrigin.y + decayHeight * 0.5f;
        const float decayScaleY = decayHeight * 0.4f;
        const float decayStepX = decayWidth / (float)(VizData::decayCurvePoints - 1);
        
        // Draw decay curve with enhanced visual feedback
        // Use modulation.amplitude (magenta/pink) for distinct decay curve color
        const ImVec4 amplitudeColorVec4 = theme.modulation.amplitude;
        const ImU32 amplitudeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(amplitudeColorVec4.x, amplitudeColorVec4.y, amplitudeColorVec4.z, 1.0f));
        const ImU32 decayColorBase = (amplitudeColor != 0) ? amplitudeColor : timbreColor;
        
        // Draw decay curve line (simplified - no complex polygon fill to avoid glitches)
        float prevDecayX = decayOrigin.x;
        float prevDecayY = decayOrigin.y + decayHeight;
        for (int i = 0; i < VizData::decayCurvePoints; ++i)
        {
            const float decay = decayCurve[i];
            const float x = decayOrigin.x + i * decayStepX;
            const float y = decayOrigin.y + decayHeight - decay * decayScaleY;
            
            // Clamp y to bounds to prevent drawing outside clip rect
            const float clampedY = juce::jlimit(decayOrigin.y, decayOrigin.y + decayHeight, y);
            
            // Dynamic color variation: brightness based on damping, alpha based on decay value
            const float brightness = 1.0f - (currentDamp * 0.4f);
            const float decayAlpha = 0.4f + decay * 0.4f;  // 0.4 to 0.8 alpha
            
            const ImVec4 decayColorVec4 = ImGui::ColorConvertU32ToFloat4(decayColorBase);
            const ImU32 decayColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
                decayColorVec4.x * brightness,
                decayColorVec4.y * brightness,
                decayColorVec4.z * brightness,
                decayAlpha));
            
            if (i > 0) drawList->AddLine(ImVec2(prevDecayX, prevDecayY), ImVec2(x, clampedY), decayColor, 2.5f);
            prevDecayX = x;
            prevDecayY = clampedY;
        }
        
        // Draw RT60 marker (vertical line at 60% decay point)
        const float rt60Time = rt60 * dampFactor;
        const float rt60Normalized = 0.6f; // RT60 is when decay reaches ~0.6 (60% of original)
        const float rt60X = decayOrigin.x + rt60Normalized * decayWidth;
        const ImU32 rt60MarkerColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            amplitudeColorVec4.x, amplitudeColorVec4.y, amplitudeColorVec4.z, 0.5f));
        drawList->AddLine(ImVec2(rt60X, decayOrigin.y), ImVec2(rt60X, decayOrigin.y + decayHeight), 
                         rt60MarkerColor, 1.5f);
        // Add RT60 label
        char rt60Label[32];
        snprintf(rt60Label, sizeof(rt60Label), "RT60: %.1fs", rt60Time);
        const ImVec2 rt60TextSize = ImGui::CalcTextSize(rt60Label);
        drawList->AddText(ImVec2(rt60X - rt60TextSize.x * 0.5f, decayOrigin.y + 2.0f), rt60MarkerColor, rt60Label);
        
            ImGui::PopClipRect();
        }
        ImGui::EndChild();
        ImGui::PopID();
        
        ImGui::Spacing();
        ImGui::Spacing();
        
        // === REVERB PARAMETERS SECTION ===
        ThemeText("Reverb Parameters", theme.text.section_header);
        ImGui::Spacing();
        
        // Get live modulated values for display
        bool isSizeModulated = isParamModulated("size");
        bool isDampModulated = isParamModulated("damp");
        bool isMixModulated = isParamModulated("mix");
        
        float size = isSizeModulated ? getLiveParamValueFor("size", "size_live", sizeParam != nullptr ? sizeParam->load() : 0.5f) 
                                     : (sizeParam != nullptr ? sizeParam->load() : 0.5f);
        float damp = isDampModulated ? getLiveParamValueFor("damp", "damp_live", dampParam != nullptr ? dampParam->load() : 0.3f)
                                     : (dampParam != nullptr ? dampParam->load() : 0.3f);
        float mix = isMixModulated ? getLiveParamValueFor("mix", "mix_live", mixParam != nullptr ? mixParam->load() : 0.8f)
                                   : (mixParam != nullptr ? mixParam->load() : 0.8f);

        // Size
        if (isSizeModulated) {
            ImGui::BeginDisabled();
        }
        // Draw inline pin for Size Mod (channel 2)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(2))  // Channel 2 = Size Mod
                ImGui::SameLine();
        }
        if (ImGui::SliderFloat("Size", &size, 0.0f, 1.0f)) {
            if (!isSizeModulated) {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("size"))) *p = size;
            }
        }
        if (!isSizeModulated) adjustParamOnWheel(ap.getParameter("size"), "size", size);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isSizeModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        ImGui::SameLine();
        HelpMarker("Room size (0-1)\n0 = small room, 1 = large hall");
        
        // Damp
        if (isDampModulated) {
            ImGui::BeginDisabled();
        }
        // Draw inline pin for Damp Mod (channel 3)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(3))  // Channel 3 = Damp Mod
                ImGui::SameLine();
        }
        if (ImGui::SliderFloat("Damp", &damp, 0.0f, 1.0f)) {
            if (!isDampModulated) {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("damp"))) *p = damp;
            }
        }
        if (!isDampModulated) adjustParamOnWheel(ap.getParameter("damp"), "damp", damp);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isDampModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        ImGui::SameLine();
        HelpMarker("High frequency damping (0-1)\n0 = bright, 1 = dark/muffled");
        
        // Mix
        if (isMixModulated) {
            ImGui::BeginDisabled();
        }
        // Draw inline pin for Mix Mod (channel 4)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(4))  // Channel 4 = Mix Mod
                ImGui::SameLine();
        }
        if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f)) {
            if (!isMixModulated) {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("mix"))) *p = mix;
            }
        }
        if (!isMixModulated) adjustParamOnWheel(ap.getParameter("mix"), "mix", mix);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isMixModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        ImGui::SameLine();
        HelpMarker("Dry/wet mix (0-1)\n0 = dry only, 1 = wet only");

        ImGui::Spacing();
        ImGui::Spacing();

        // === RELATIVE MODULATION SECTION ===
        ThemeText("CV Input Modes", theme.text.section_header);
        ImGui::Spacing();
        
        // Relative Size Mod checkbox
        bool relativeSizeMod = relativeSizeModParam != nullptr && relativeSizeModParam->load() > 0.5f;
        if (ImGui::Checkbox("Relative Size Mod", &relativeSizeMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeSizeMod")))
                *p = relativeSizeMod;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("ON: CV modulates around slider value (±0.5)\nOFF: CV directly sets size (0-1)");
        }
        
        // Relative Damp Mod checkbox
        bool relativeDampMod = relativeDampModParam != nullptr && relativeDampModParam->load() > 0.5f;
        if (ImGui::Checkbox("Relative Damp Mod", &relativeDampMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeDampMod")))
                *p = relativeDampMod;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("ON: CV modulates around slider value (±0.5)\nOFF: CV directly sets damp (0-1)");
        }
        
        // Relative Mix Mod checkbox
        bool relativeMixMod = relativeMixModParam != nullptr && relativeMixModParam->load() > 0.5f;
        if (ImGui::Checkbox("Relative Mix Mod", &relativeMixMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMixMod")))
                *p = relativeMixMod;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("ON: CV modulates around slider value (±0.5)\nOFF: CV directly sets mix (0-1)");
        }
        
        ImGui::PopItemWidth();
        ImGui::PopID();
    }

    void drawIoPins(const NodePinHelpers& helpers) override
    {
        // Only draw audio pins - modulation pins (channels 2, 3, 4) are now inline
        helpers.drawParallelPins("In L", 0, "Out L", 0);
        helpers.drawParallelPins("In R", 1, "Out R", 1);
        // Size Mod (ch 2), Damp Mod (ch 3), Mix Mod (ch 4) are drawn inline in drawParametersInNode
    }

    bool usesCustomPinLayout() const override { return true; }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "In L";
            case 1: return "In R";
            case 2: return "Size Mod";
            case 3: return "Damp Mod";
            case 4: return "Mix Mod";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Out L";
            case 1: return "Out R";
            default: return juce::String("Out ") + juce::String(channel + 1);
        }
    }
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    juce::dsp::Reverb reverb;
    std::atomic<float>* sizeParam { nullptr };
    std::atomic<float>* dampParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    std::atomic<float>* relativeSizeModParam { nullptr };
    std::atomic<float>* relativeDampModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };
    
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        static constexpr int decayCurvePoints = 128;
        static constexpr int spectrumPoints = 64;
        
        // Waveform snapshots
        std::array<std::atomic<float>, waveformPoints> inputWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputWaveformR;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        
        // Reverb tail decay curve (precomputed for visualization)
        std::array<std::atomic<float>, decayCurvePoints> decayCurve;
        
        // Frequency spectrum (for damping visualization)
        std::array<std::atomic<float>, spectrumPoints> frequencySpectrum;
        
        // Current parameter state
        std::atomic<float> currentSize { 0.5f };
        std::atomic<float> currentDamp { 0.3f };
        std::atomic<float> currentMix { 0.8f };
        std::atomic<float> reverbActivity { 0.0f }; // For density visualization
    };
    VizData vizData;
    
    // Circular buffer for waveform snapshots (longer buffer for reverb tail)
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
    juce::AudioBuffer<float> vizDryBuffer;  // Store dry signal for comparison
    juce::AudioBuffer<float> dryBlockTemp;  // Reused per-block dry copy
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 4096; // ~85ms at 48kHz (longer for reverb tail)
};


