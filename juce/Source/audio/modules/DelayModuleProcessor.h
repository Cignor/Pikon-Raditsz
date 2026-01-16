#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

class DelayModuleProcessor : public ModuleProcessor
{
public:
    DelayModuleProcessor();
    ~DelayModuleProcessor() override = default;

    const juce::String getName() const override { return "delay"; }

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
        
        ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
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
        
        // === DELAY VISUALIZATION ===
        ImGui::Spacing();
        ImGui::Text("Waveform & Delay Taps");
        ImGui::Spacing();
        
        // Draw waveform visualization with delay tap markers
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float vizWidth = itemWidth;
        const float vizHeight = 120.0f;
        const ImVec2 rectMax = ImVec2(origin.x + vizWidth, origin.y + vizHeight);
        
        // Get theme colors
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
        
        // Input waveform: Use modulation.frequency (cyan) - brighter and more visible
        const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
        const ImU32 frequencyColor = ImGui::ColorConvertFloat4ToU32(ImVec4(frequencyColorVec4.x, frequencyColorVec4.y, frequencyColorVec4.z, 0.8f));
        const ImU32 inputWaveformColor = resolveColor(theme.modules.scope_plot_fg, frequencyColor, IM_COL32(100, 220, 255, 200));
        
        // Output waveform: Use modulation.timbre (orange/yellow) - very visible
        const ImVec4 timbreColorVec4 = theme.modulation.timbre;
        const ImU32 timbreColor = ImGui::ColorConvertFloat4ToU32(ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f));
        const ImU32 outputWaveformColor = (timbreColor != 0) ? timbreColor : IM_COL32(255, 180, 80, 255);
        
        // Delay tap markers: Use modulation.amplitude (magenta/pink) - brighter
        const ImVec4 amplitudeColorVec4 = theme.modulation.amplitude;
        const ImU32 amplitudeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(amplitudeColorVec4.x, amplitudeColorVec4.y, amplitudeColorVec4.z, 1.0f));
        const ImU32 tapMarkerColor = (amplitudeColor != 0) ? amplitudeColor : IM_COL32(255, 120, 220, 255);
        
        // Center line (thicker for visibility)
        const ImU32 centerLineColor = IM_COL32(150, 150, 150, 150);
        
        drawList->AddRectFilled(origin, rectMax, bgColor, 4.0f);
        ImGui::PushClipRect(origin, rectMax, true);
        
        // Read visualization data (thread-safe)
        float inputWaveform[VizData::waveformPoints];
        float outputWaveform[VizData::waveformPoints];
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            inputWaveform[i] = vizData.inputWaveformL[i].load();
            outputWaveform[i] = vizData.outputWaveformL[i].load();
        }
        const float currentTimeMs = vizData.currentTimeMs.load();
        const float currentFeedback = vizData.currentFeedback.load();
        const float currentMix = vizData.currentMix.load();
        const int activeTapCount = vizData.activeTapCount.load();
        float tapPositions[8], tapLevels[8];
        for (int i = 0; i < 8; ++i)
        {
            tapPositions[i] = vizData.tapPositions[i].load();
            tapLevels[i] = vizData.tapLevels[i].load();
        }
        
        const float midY = origin.y + vizHeight * 0.5f;
        const float scaleY = vizHeight * 0.4f;
        const float stepX = vizWidth / (float)(VizData::waveformPoints - 1);
        
        // Draw center line (thicker for visibility)
        drawList->AddLine(ImVec2(origin.x, midY), ImVec2(rectMax.x, midY), centerLineColor, 1.5f);
        
        // Draw input waveform (thicker, more visible)
        float prevX = origin.x, prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, inputWaveform[i]);
            const float x = origin.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0) drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), inputWaveformColor, 2.5f);
            prevX = x; prevY = y;
        }
        
        // Draw output waveform (thickest, shows delayed signal)
        prevX = origin.x; prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveform[i]);
            const float x = origin.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0) drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), outputWaveformColor, 3.5f);
            prevX = x; prevY = y;
        }
        
        // Draw delay tap markers (vertical lines with height proportional to level, thicker and more visible)
        for (int i = 0; i < activeTapCount; ++i)
        {
            const float tapPos = tapPositions[i];
            const float tapLevel = tapLevels[i];
            if (tapPos >= 0.0f && tapPos <= 1.0f && tapLevel > 0.01f)
            {
                const float tapX = origin.x + tapPos * vizWidth;
                const float tapHeight = tapLevel * scaleY * 0.8f;
                drawList->AddLine(ImVec2(tapX, midY - tapHeight), ImVec2(tapX, midY + tapHeight), 
                                tapMarkerColor, 3.0f);
                // Draw larger circle at top of tap marker for better visibility
                drawList->AddCircleFilled(ImVec2(tapX, midY - tapHeight), 4.5f, tapMarkerColor);
            }
        }
        
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(origin.x, rectMax.y));
        ImGui::Dummy(ImVec2(vizWidth, 0));
        
        // Parameter meters
        const ImVec4 accentVec4 = theme.accent;
        const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(ImVec4(accentVec4.x, accentVec4.y, accentVec4.z, 1.0f));
        
        ImGui::Text("Time: %.1f ms", currentTimeMs);
        float timeMeter = (currentTimeMs - 1.0f) / 1999.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
        ImGui::ProgressBar(timeMeter, ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", timeMeter * 100.0f);
        
        ImGui::Text("Feedback: %.2f", currentFeedback);
        float feedbackMeter = currentFeedback / 0.95f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
        ImGui::ProgressBar(feedbackMeter, ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", feedbackMeter * 100.0f);
        
        ImGui::Text("Mix: %.2f | Active Taps: %d", currentMix, activeTapCount);
        
        ImGui::Spacing();
        ImGui::Spacing();
        
        // === DELAY PARAMETERS SECTION ===
        ThemeText("Delay Parameters", theme.text.section_header);
        ImGui::Spacing();

        // Time
        bool isTimeModulated = isParamModulated("timeMs");
        float timeMs = isTimeModulated ? getLiveParamValueFor("timeMs", "timeMs_live", timeMsParam != nullptr ? timeMsParam->load() : 400.0f) 
                                       : (timeMsParam != nullptr ? timeMsParam->load() : 400.0f);
        if (isTimeModulated) {
            ImGui::BeginDisabled();
        }
        // Draw inline pin for Time Mod (channel 2)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(2))  // Channel 2 = Time Mod
                ImGui::SameLine();
        }
        if (ImGui::SliderFloat("Time (ms)", &timeMs, 1.0f, 2000.0f, "%.1f")) {
            if (!isTimeModulated) {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("timeMs"))) *p = timeMs;
            }
        }
        if (!isTimeModulated) adjustParamOnWheel(ap.getParameter("timeMs"), "timeMs", timeMs);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isTimeModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        ImGui::SameLine();
        HelpMarker("Delay time in milliseconds (1-2000 ms)");

        // Feedback
        bool isFbModulated = isParamModulated("feedback");
        float fb = isFbModulated ? getLiveParamValueFor("feedback", "feedback_live", feedbackParam != nullptr ? feedbackParam->load() : 0.4f)
                                 : (feedbackParam != nullptr ? feedbackParam->load() : 0.4f);
        if (isFbModulated) {
            ImGui::BeginDisabled();
        }
        // Draw inline pin for Feedback Mod (channel 3)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(3))  // Channel 3 = Feedback Mod
                ImGui::SameLine();
        }
        if (ImGui::SliderFloat("Feedback", &fb, 0.0f, 0.95f)) {
            if (!isFbModulated) {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("feedback"))) *p = fb;
            }
        }
        if (!isFbModulated) adjustParamOnWheel(ap.getParameter("feedback"), "feedback", fb);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isFbModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        ImGui::SameLine();
        HelpMarker("Feedback amount (0-95%)\nCreates repeating echoes");

        // Mix
        bool isMixModulated = isParamModulated("mix");
        float mix = isMixModulated ? getLiveParamValueFor("mix", "mix_live", mixParam != nullptr ? mixParam->load() : 0.3f)
                                   : (mixParam != nullptr ? mixParam->load() : 0.3f);
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
        HelpMarker("Dry/wet mix (0-100%)\n0% = dry signal only, 100% = delayed signal only");

        ImGui::Spacing();

        // === MODULATION MODE SECTION ===
        ThemeText("Modulation Mode", theme.text.section_header);
        ImGui::Spacing();
        
        bool relativeTimeMod = relativeTimeModParam ? (relativeTimeModParam->load() > 0.5f) : true;
        if (ImGui::Checkbox("Relative Time Mod", &relativeTimeMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeTimeMod")))
                *p = relativeTimeMod;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        ImGui::SameLine();
        HelpMarker("Relative: CV modulates around slider time (±3 octaves)\nAbsolute: CV directly controls time (1-2000ms, ignores slider)");

        bool relativeFeedbackMod = relativeFeedbackModParam ? (relativeFeedbackModParam->load() > 0.5f) : true;
        if (ImGui::Checkbox("Relative Feedback Mod", &relativeFeedbackMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeFeedbackMod")))
                *p = relativeFeedbackMod;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        ImGui::SameLine();
        HelpMarker("Relative: CV adds offset to slider feedback (±0.5)\nAbsolute: CV directly controls feedback (0-95%, ignores slider)");

        bool relativeMixMod = relativeMixModParam ? (relativeMixModParam->load() > 0.5f) : true;
        if (ImGui::Checkbox("Relative Mix Mod", &relativeMixMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMixMod")))
                *p = relativeMixMod;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        ImGui::SameLine();
        HelpMarker("Relative: CV adds offset to slider mix (±0.5)\nAbsolute: CV directly controls mix (0-100%, ignores slider)");

        ImGui::Spacing();
        
        ImGui::PopItemWidth();
        ImGui::PopID();
    }

    void drawIoPins(const NodePinHelpers& helpers) override
    {
        // Only draw audio pins - modulation pins (channels 2, 3, 4) are now inline
        helpers.drawParallelPins("In L", 0, "Out L", 0);
        helpers.drawParallelPins("In R", 1, "Out R", 1);
        // Time Mod (ch 2), Feedback Mod (ch 3), Mix Mod (ch 4) are drawn inline in drawParametersInNode
    }

    bool usesCustomPinLayout() const override { return true; }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "In L";
            case 1: return "In R";
            case 2: return "Time Mod";
            case 3: return "Feedback Mod";
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
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> dlL { 48000 }, dlR { 48000 };
    std::atomic<float>* timeMsParam { nullptr };
    std::atomic<float>* feedbackParam { nullptr };
    std::atomic<float>* mixParam { nullptr };
    std::atomic<float>* relativeTimeModParam { nullptr };
    std::atomic<float>* relativeFeedbackModParam { nullptr };
    std::atomic<float>* relativeMixModParam { nullptr };
    double sr { 48000.0 };
    int maxDelaySamples { 48000 };
    
    // Smoothed values to prevent clicks and zipper noise
    juce::SmoothedValue<float> timeSm;
    juce::SmoothedValue<float> feedbackSm;
    juce::SmoothedValue<float> mixSm;
    
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> inputWaveformL;
        std::array<std::atomic<float>, waveformPoints> inputWaveformR;
        std::array<std::atomic<float>, waveformPoints> outputWaveformL;
        std::array<std::atomic<float>, waveformPoints> outputWaveformR;
        
        // Delay tap positions and levels (normalized 0-1, -1 = inactive)
        // Track up to 8 delay taps (echoes) for visualization
        std::array<std::atomic<float>, 8> tapPositions;  // Position in waveform (0-1)
        std::array<std::atomic<float>, 8> tapLevels;      // Amplitude of each tap
        std::atomic<int> activeTapCount { 0 };
        
        // Current parameter state
        std::atomic<float> currentTimeMs { 400.0f };
        std::atomic<float> currentFeedback { 0.4f };
        std::atomic<float> currentMix { 0.3f };
    };
    VizData vizData;
    
    // Circular buffer for waveform snapshots
    juce::AudioBuffer<float> vizInputBuffer;
    juce::AudioBuffer<float> vizOutputBuffer;
    juce::AudioBuffer<float> vizDryBuffer;  // Store dry signal for comparison
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
};


