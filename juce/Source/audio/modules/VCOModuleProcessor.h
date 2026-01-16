#pragma once

#include "ModuleProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

class VCOModuleProcessor : public ModuleProcessor
{
public:
    // Parameter IDs
    static constexpr auto paramIdFrequency   = "frequency";
    static constexpr auto paramIdWaveform    = "waveform";
    // Virtual target only (no APVTS param needed) – used for routing to select waveform
    static constexpr auto paramIdWaveformMod = "waveform_mod";
    static constexpr auto paramIdRelativeFreqMod = "relative_freq_mod";
    static constexpr auto paramIdPortamento  = "portamento";

    VCOModuleProcessor();
    ~VCOModuleProcessor() override = default;

    const juce::String getName() const override { return "vco"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setTimingInfo(const TransportState& state) override;
    void forceStop() override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode (float itemWidth,
                               const std::function<bool(const juce::String& paramId)>& isParamModulated,
                               const std::function<void()>& onModificationEnded,
                               const NodePinHelpers* pinHelpers = nullptr) override
    {
        ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
        
        auto& ap = getAPVTS();
        const auto& theme = ThemeManager::getInstance().getCurrentTheme();
        
        const bool freqMod = isParamModulated(paramIdFrequency);
        const bool waveMod = isParamModulated(paramIdWaveformMod);
        
        float freq = freqMod ? getLiveParamValueFor(paramIdFrequency, "frequency_live", frequencyParam ? frequencyParam->load() : 440.0f)
                             : (frequencyParam ? frequencyParam->load() : 440.0f);
        int wave = 0;
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdWaveform)))
            wave = waveMod ? (int)getLiveParamValueFor(paramIdWaveformMod, "waveform_live", (float)p->getIndex())
                           : p->getIndex();

        // Helper for tooltips (imgui_demo.cpp pattern)
        auto HelpMarker = [](const char* desc)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted(desc);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        };

        ImGui::PushItemWidth (itemWidth);

        // === FREQUENCY SECTION ===
        ThemeText("Oscillator Control", theme.text.section_header);
        ImGui::Spacing();
        
        // Color-coded modulation indicator
        if (freqMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); // Cyan
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
        }
        
        // Inline pin for Frequency Mod (Channel 0)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(0))
                ImGui::SameLine();
        }
        
        if (freqMod) ImGui::BeginDisabled();
        if (ImGui::SliderFloat ("##freq", &freq, 20.0f, 20000.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic))
        {
            if (!freqMod) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdFrequency))) *p = freq;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (!freqMod) adjustParamOnWheel (ap.getParameter(paramIdFrequency), "frequencyHz", freq);
        if (freqMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (freqMod)
        {
            ThemeText("Frequency (CV)", theme.text.active);
            ImGui::PopStyleColor(3);
        }
        else
        {
            ImGui::Text("Frequency");
        }
        HelpMarker("Control voltage range: 0-1V = 20Hz to 20kHz (exponential)\nConnect LFO, Envelope, or Sequencer for modulation");

        // Note name display
        if (!freqMod)
        {
            auto getNoteFromFreq = [](float f) -> juce::String
            {
                if (f < 20.0f || f > 20000.0f) return "";
                float midiNote = 12.0f * std::log2(f / 440.0f) + 69.0f;
                int noteNum = (int)std::round(midiNote);
                const char* notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                int octave = (noteNum / 12) - 1;
                return juce::String(notes[noteNum % 12]) + juce::String(octave);
            };
            ImGui::TextDisabled("Note: %s", getNoteFromFreq(freq).toRawUTF8());
        }

        // Quick preset buttons
        if (!freqMod)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
            float btnWidth = (itemWidth - 12) / 4.0f;
            
            if (ImGui::Button("A4", ImVec2(btnWidth, 0)))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdFrequency))) *p = 440.0f;
                onModificationEnded();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("440 Hz (Concert A)");
            
            ImGui::SameLine();
            if (ImGui::Button("C4", ImVec2(btnWidth, 0)))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdFrequency))) *p = 261.63f;
                onModificationEnded();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("261.63 Hz (Middle C)");
            
            ImGui::SameLine();
            if (ImGui::Button("A3", ImVec2(btnWidth, 0)))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdFrequency))) *p = 220.0f;
                onModificationEnded();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("220 Hz");
            
            ImGui::SameLine();
            if (ImGui::Button("C3", ImVec2(btnWidth, 0)))
            {
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdFrequency))) *p = 130.81f;
                onModificationEnded();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("130.81 Hz");
            
            ImGui::PopStyleVar();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // === MODULATION MODE SECTION ===
        ThemeText("Frequency Modulation", theme.text.section_header);
        ImGui::Spacing();
        
        bool relativeFreqMod = true;
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativeFreqMod)))
            relativeFreqMod = p->get();
        
        if (ImGui::Checkbox("Relative Frequency Mod", &relativeFreqMod))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativeFreqMod)))
            {
                *p = relativeFreqMod;
                juce::Logger::writeToLog("[VCO UI] Relative Frequency Mod changed to: " + juce::String(relativeFreqMod ? "TRUE" : "FALSE"));
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
        HelpMarker("Relative: CV modulates around slider frequency (±4 octaves)\nAbsolute: CV directly controls frequency (20Hz-20kHz, ignores slider)\n\nExample with slider at 440Hz:\n- Relative: CV=0.5 → 440Hz, CV=0.625 → ~622Hz (+1 oct)\n- Absolute: CV=0.5 → ~632Hz, ignores slider position");

        ImGui::Spacing();
        ImGui::Spacing();

        // === PORTAMENTO SECTION ===
        ThemeText("Glide", theme.text.section_header);
        ImGui::Spacing();
        
        float portamentoTime = 0.0f;
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPortamento)))
            portamentoTime = p->get();
        
        if (ImGui::SliderFloat("##portamento", &portamentoTime, 0.0f, 2.0f, "%.3f s", ImGuiSliderFlags_Logarithmic))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPortamento)))
                *p = portamentoTime;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        adjustParamOnWheel(ap.getParameter(paramIdPortamento), "portamentoTime", portamentoTime);
        
        ImGui::SameLine();
        ImGui::Text("Portamento");
        HelpMarker("Pitch glide time between frequency changes\n0s = instant (no glide)\n0.1s = fast slide\n0.5s = smooth glide\n2s = slow portamento\nWorks with both CV modulation and manual changes");

        // Quick preset buttons
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        float btnWidth = (itemWidth - 12) / 4.0f;
        
        if (ImGui::Button("Off", ImVec2(btnWidth, 0)))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPortamento)))
            {
                *p = 0.0f;
                onModificationEnded();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("No glide (instant)");
        
        ImGui::SameLine();
        if (ImGui::Button("Fast", ImVec2(btnWidth, 0)))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPortamento)))
            {
                *p = 0.05f;
                onModificationEnded();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("50ms glide");
        
        ImGui::SameLine();
        if (ImGui::Button("Medium", ImVec2(btnWidth, 0)))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPortamento)))
            {
                *p = 0.2f;
                onModificationEnded();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("200ms glide");
        
        ImGui::SameLine();
        if (ImGui::Button("Slow", ImVec2(btnWidth, 0)))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdPortamento)))
            {
                *p = 0.5f;
                onModificationEnded();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("500ms glide");
        
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Spacing();

        // === WAVEFORM SECTION ===
        ThemeText("Waveform", theme.text.section_header);
        ImGui::Spacing();
        
        // Color-coded modulation indicator
        if (waveMod)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f)); // Orange
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.5f, 0.4f, 0.2f, 0.5f));
        }
        
        // Inline pin for Waveform Mod (Channel 1)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(1))
                ImGui::SameLine();
        }
        
        if (waveMod) ImGui::BeginDisabled();
        if (ImGui::Combo ("##wave", &wave, "Sine\0Sawtooth\0Square\0\0"))
        {
            if (!waveMod) if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdWaveform))) *p = wave;
        }
        if (!waveMod && ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                int newWave = juce::jlimit(0, 2, wave + (wheel > 0.0f ? -1 : 1));
                if (newWave != wave)
                {
                    wave = newWave;
                    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdWaveform)))
                    {
                        *p = wave;
                        onModificationEnded();
                    }
                }
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (waveMod) ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (waveMod)
        {
            ThemeText("Shape (CV)", theme.text.warning);
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::Text("Shape");
        }
        HelpMarker("Sine: Pure tone, no harmonics\nSawtooth: Bright, rich in harmonics\nSquare: Hollow, odd harmonics only\nCV modulation: 0V=Sine, 0.5V=Saw, 1V=Square");

        // Visual waveform preview (imgui_demo.cpp PlotLines pattern)
        float waveformPreview[128];
        for (int i = 0; i < 128; ++i)
        {
            float x = (float)i / 128.0f * 2.0f * juce::MathConstants<float>::pi;
            switch (wave)
            {
                case 0: waveformPreview[i] = std::sin(x); break; // Sine
                case 1: waveformPreview[i] = (x / juce::MathConstants<float>::pi) - 1.0f; break; // Sawtooth
                case 2: waveformPreview[i] = (x < juce::MathConstants<float>::pi) ? 1.0f : -1.0f; break; // Square
                default: waveformPreview[i] = 0.0f;
            }
        }
        ImGui::PlotLines("##wavepreview", waveformPreview, 128, 0, nullptr, -1.2f, 1.2f, ImVec2(itemWidth, 80));

        ImGui::Spacing();
        ImGui::Spacing();

        // === OSCILLOSCOPE VISUALIZATION ===
        ThemeText("Oscilloscope", theme.text.section_header);
        ImGui::Spacing();

        ImGui::PushID(this); // Unique ID for this node's UI
        
        // Read visualization data (thread-safe)
        float outputWaveform[VizData::waveformPoints];
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            outputWaveform[i] = vizData.outputWaveform[i].load();
        }
        const float currentFreq = vizData.currentFrequency.load();
        const int currentWave = vizData.currentWaveform.load();
        const float gateLevel = vizData.gateLevel.load();

        // Waveform visualization in child window
        const auto& freqColors = theme.modules.frequency_graph;
        const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
        const float waveHeight = 140.0f;
        const ImVec2 graphSize(itemWidth, waveHeight);
        
        if (ImGui::BeginChild("VCOOscilloscope", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);
            
            // Background
            const ImU32 bgColor = resolveColor(freqColors.background, IM_COL32(18, 20, 24, 255));
            drawList->AddRectFilled(p0, p1, bgColor);
            
            // Grid lines
            const ImU32 gridColor = resolveColor(freqColors.grid, IM_COL32(50, 55, 65, 255));
            const float midY = p0.y + graphSize.y * 0.5f;
            drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), gridColor, 1.0f);
            drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), gridColor, 1.0f);
            drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), gridColor, 1.0f);
            
            // Clip to graph area
            drawList->PushClipRect(p0, p1, true);
            
            // Draw output waveform
            const float scaleY = graphSize.y * 0.45f;
            const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);
            
            const ImU32 waveformColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
            float prevX = p0.x;
            float prevY = midY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveform[i]);
                const float x = p0.x + i * stepX;
                const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
                if (i > 0)
                    drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), waveformColor, 2.5f);
                prevX = x;
                prevY = y;
            }

            // Draw gate level indicator (horizontal line showing gate amount)
            if (gateLevel < 1.0f)
            {
                const ImU32 gateIndicatorColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);
                const float gateY = p0.y + graphSize.y - (gateLevel * graphSize.y * 0.3f);
                const float clampedGateY = juce::jlimit(p0.y + 2.0f, p1.y - 2.0f, gateY);
                drawList->AddLine(ImVec2(p0.x, clampedGateY), ImVec2(p1.x, clampedGateY), gateIndicatorColor, 1.5f);
                
                // Gate label using drawList
                const ImU32 textColor = gateIndicatorColor;
                drawList->AddText(ImVec2(p0.x + 4.0f, clampedGateY - 12.0f), textColor, "Gate");
            }
            
            drawList->PopClipRect();
            
            // Frequency and waveform info overlay
            const char* waveNames[] = { "Sine", "Sawtooth", "Square" };
            const char* waveName = (currentWave >= 0 && currentWave < 3) ? waveNames[currentWave] : "Unknown";
            
            ImGui::SetCursorPos(ImVec2(4, 4));
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%.1f Hz | %s", currentFreq, waveName);
            if (gateLevel < 1.0f)
            {
                ImGui::SetCursorPos(ImVec2(4, graphSize.y - 20));
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Gate: %.2f", gateLevel);
            }
            
            // Invisible drag blocker
            ImGui::SetCursorPos(ImVec2(0, 0));
            ImGui::InvisibleButton("##vcoOscilloscopeDrag", graphSize);
        }
        ImGui::EndChild();

        ImGui::PopID(); // End unique ID
        ImGui::Spacing();
        ImGui::Spacing();

        // === OUTPUT SECTION ===
        ThemeText("Output", theme.text.section_header);
        ImGui::Spacing();
        
        // Real-time output level meter
        float outputLevel = lastOutputValues[0]->load();
        float absLevel = std::abs(outputLevel);
        
        // Color-coded progress bar
        ImVec4 meterColor;
        if (absLevel < 0.7f)
            meterColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f); // Green
        else if (absLevel < 0.9f)
            meterColor = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
        else
            meterColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f); // Red
        
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, meterColor);
        ImGui::ProgressBar(absLevel, ImVec2(itemWidth, 0), "");
        ImGui::PopStyleColor();
        
        ImGui::SameLine(0, 5);
        ImGui::Text("%.3f", outputLevel);
        HelpMarker("Live output signal level\nConnect to VCA, Filter, or Audio Out\nUse Gate input to control amplitude");

        ImGui::PopItemWidth();
        ImGui::PopID();  // End unique ID
    }

    void drawIoPins(const NodePinHelpers& helpers) override
    {
        // Only draw Gate In as parallel pin (channels 0-1 are now inline)
        helpers.drawParallelPins("Gate In", 2, nullptr, -1);
        // Draw output
        helpers.drawAudioOutputPin("Output", 0);
    }

    juce::String getAudioInputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Frequency Mod";
            case 1: return "Waveform Mod";
            case 2: return "Gate";
            default: return juce::String("In ") + juce::String(channel + 1);
        }
    }

    juce::String getAudioOutputLabel(int channel) const override
    {
        switch (channel)
        {
            case 0: return "Out";
            default: return juce::String("Out ") + juce::String(channel + 1);
        }
    }
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    juce::dsp::Oscillator<float> oscillator;
    int currentWaveform = -1;

    // Cached parameter pointers
    std::atomic<float>* frequencyParam { nullptr };
    std::atomic<float>* waveformParam  { nullptr };
    std::atomic<float>* relativeFreqModParam { nullptr };
    std::atomic<float>* portamentoParam { nullptr };

    // Click-free gating
    float smoothedGate { 0.0f };
    
    // Portamento/glide
    float currentFrequency { 440.0f };
    double sampleRate { 44100.0 };
    
    // Transport state tracking
    TransportState m_currentTransport;

#if defined(PRESET_CREATOR_UI)
    // --- Visualization Data (thread-safe, updated from audio thread) ---
    struct VizData
    {
        static constexpr int waveformPoints = 256;
        std::array<std::atomic<float>, waveformPoints> outputWaveform;
        std::atomic<float> currentFrequency { 440.0f };
        std::atomic<int> currentWaveform { 0 };
        std::atomic<float> gateLevel { 0.0f };
        std::atomic<float> outputLevel { 0.0f };

        VizData()
        {
            for (auto& v : outputWaveform) v.store(0.0f);
        }
    };
    VizData vizData;

    // Circular buffers for waveform capture
    juce::AudioBuffer<float> vizOutputBuffer;
    int vizWritePos { 0 };
    static constexpr int vizBufferSize = 2048; // ~43ms at 48kHz
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VCOModuleProcessor)
};


