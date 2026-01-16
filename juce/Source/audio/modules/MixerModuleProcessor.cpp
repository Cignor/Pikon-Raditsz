#include "MixerModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

// Corrected constructor with two separate stereo inputs
MixerModuleProcessor::MixerModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("In A", juce::AudioChannelSet::stereo(), true) // Bus 0
              .withInput("In B", juce::AudioChannelSet::stereo(), true) // Bus 1
              .withInput("Gain Mod", juce::AudioChannelSet::mono(), true)
              .withInput("Pan Mod", juce::AudioChannelSet::mono(), true)
              .withInput("X-Fade Mod", juce::AudioChannelSet::mono(), true)
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "MixerParams", createParameterLayout())
{
    gainParam = apvts.getRawParameterValue("gain");
    panParam = apvts.getRawParameterValue("pan");
    crossfadeParam = apvts.getRawParameterValue("crossfade"); // Get the new parameter

    // Initialize value tooltips for the stereo output
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

// Updated parameter layout with the new crossfade slider
juce::AudioProcessorValueTreeState::ParameterLayout MixerModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "gain", "Gain", juce::NormalisableRange<float>(-60.0f, 6.0f, 0.01f), 0.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "pan", "Pan", juce::NormalisableRange<float>(-1.0f, 1.0f), 0.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "crossfade",
            "Crossfade",
            juce::NormalisableRange<float>(-1.0f, 1.0f),
            0.0f)); // A <-> B
    return {p.begin(), p.end()};
}

void MixerModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);

#if defined(PRESET_CREATOR_UI)
    vizInputABuffer.setSize(2, vizBufferSize, false, true, true);
    vizInputBBuffer.setSize(2, vizBufferSize, false, true, true);
    vizOutputBuffer.setSize(2, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.inputAWaveformL)
        v.store(0.0f);
    for (auto& v : vizData.inputAWaveformR)
        v.store(0.0f);
    for (auto& v : vizData.inputBWaveformL)
        v.store(0.0f);
    for (auto& v : vizData.inputBWaveformR)
        v.store(0.0f);
    for (auto& v : vizData.outputWaveformL)
        v.store(0.0f);
    for (auto& v : vizData.outputWaveformR)
        v.store(0.0f);
    vizData.currentCrossfade.store(0.0f);
    vizData.currentGainDb.store(0.0f);
    vizData.currentPan.store(0.0f);
    vizData.inputALevelDb.store(-60.0f);
    vizData.inputBLevelDb.store(-60.0f);
    vizData.outputLevelDbL.store(-60.0f);
    vizData.outputLevelDbR.store(-60.0f);
#endif
}

// Completely rewritten processBlock for crossfading
void MixerModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto inA = getBusBuffer(buffer, true, 0);
    auto inB = getBusBuffer(buffer, true, 1);
    auto out = getBusBuffer(buffer, false, 0);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = out.getNumChannels();

#if defined(PRESET_CREATOR_UI)
    // Capture input audio for visualization (before processing)
    if (vizInputABuffer.getNumSamples() > 0 && inA.getNumChannels() >= 2)
    {
        const int samplesToCopy = juce::jmin(numSamples, vizBufferSize);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (inA.getNumChannels() > ch)
            {
                const float* inputData = inA.getReadPointer(ch);
                for (int i = 0; i < samplesToCopy; ++i)
                {
                    const int writeIdx = (vizWritePos + i) % vizBufferSize;
                    vizInputABuffer.setSample(ch, writeIdx, inputData[i]);
                }
            }
        }
    }
    if (vizInputBBuffer.getNumSamples() > 0 && inB.getNumChannels() >= 2)
    {
        const int samplesToCopy = juce::jmin(numSamples, vizBufferSize);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (inB.getNumChannels() > ch)
            {
                const float* inputData = inB.getReadPointer(ch);
                for (int i = 0; i < samplesToCopy; ++i)
                {
                    const int writeIdx = (vizWritePos + i) % vizBufferSize;
                    vizInputBBuffer.setSample(ch, writeIdx, inputData[i]);
                }
            }
        }
    }
#endif

    // Read CV from input buses (if connected)
    float gainModCV = 0.0f;
    float panModCV = 0.0f;
    float crossfadeModCV = 0.0f;

    // Check if gain mod bus is connected and read CV
    if (isParamInputConnected("gain"))
    {
        const auto& gainModBus = getBusBuffer(buffer, true, 2);
        if (gainModBus.getNumChannels() > 0)
            gainModCV = gainModBus.getReadPointer(0)[0]; // Read first sample
    }

    // Check if pan mod bus is connected and read CV
    if (isParamInputConnected("pan"))
    {
        const auto& panModBus = getBusBuffer(buffer, true, 3);
        if (panModBus.getNumChannels() > 0)
            panModCV = panModBus.getReadPointer(0)[0]; // Read first sample
    }

    // Check if crossfade mod bus is connected and read CV
    if (isParamInputConnected("x-fade"))
    {
        const auto& crossfadeModBus = getBusBuffer(buffer, true, 4);
        if (crossfadeModBus.getNumChannels() > 0)
            crossfadeModCV = crossfadeModBus.getReadPointer(0)[0]; // Read first sample
    }

    // Apply modulation or use parameter values
    float crossfade = 0.0f;
    if (isParamInputConnected("x-fade"))
    {
        // Map CV [0,1] to crossfade [-1, 1]
        crossfade = -1.0f + crossfadeModCV * (1.0f - (-1.0f));
    }
    else
    {
        crossfade = crossfadeParam != nullptr ? crossfadeParam->load() : 0.0f;
    }

    // Use a constant power law for a smooth crossfade without volume dips
    const float mixAngle = (crossfade * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
    const float gainA = std::cos(mixAngle);
    const float gainB = std::sin(mixAngle);

    // Perform the crossfade into the output buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* srcA = (ch < inA.getNumChannels()) ? inA.getReadPointer(ch) : nullptr;
        const float* srcB = (ch < inB.getNumChannels()) ? inB.getReadPointer(ch) : nullptr;
        float*       dst = out.getWritePointer(ch);

        for (int i = 0; i < numSamples; ++i)
        {
            const float a = srcA ? srcA[i] : 0.0f;
            const float b = srcB ? srcB[i] : 0.0f;
            dst[i] = a * gainA + b * gainB;
        }
    }

    // Now, apply the master gain and pan to the mixed signal in the output buffer
    float masterGain = 0.0f;
    if (isParamInputConnected("gain"))
    {
        // Map CV [0,1] to gain [-60, 6] dB
        float gainDb = -60.0f + gainModCV * (6.0f - (-60.0f));
        masterGain = juce::Decibels::decibelsToGain(gainDb);
    }
    else
    {
        masterGain =
            juce::Decibels::decibelsToGain(gainParam != nullptr ? gainParam->load() : 0.0f);
    }

    float pan = 0.0f;
    if (isParamInputConnected("pan"))
    {
        // Map CV [0,1] to pan [-1, 1]
        pan = -1.0f + panModCV * (1.0f - (-1.0f));
    }
    else
    {
        pan = panParam != nullptr ? panParam->load() : 0.0f;
    }
    const float panAngleMaster = (pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
    const float lGain = masterGain * std::cos(panAngleMaster);
    const float rGain = masterGain * std::sin(panAngleMaster);

    out.applyGain(0, 0, numSamples, lGain);
    if (numChannels > 1)
        out.applyGain(1, 0, numSamples, rGain);

    // Store live modulated values for UI display
    setLiveParamValue("crossfade_live", crossfade);
    setLiveParamValue(
        "gain_live",
        isParamInputConnected("gain") ? (-60.0f + gainModCV * 66.0f)
                                      : (gainParam != nullptr ? gainParam->load() : 0.0f));
    setLiveParamValue("pan_live", pan);

    // Update tooltips
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(out.getSample(0, numSamples - 1));
        if (lastOutputValues[1])
            lastOutputValues[1]->store(out.getSample(1, numSamples - 1));
    }

#if defined(PRESET_CREATOR_UI)
    // Capture output audio for visualization (after processing)
    if (vizOutputBuffer.getNumSamples() > 0 && out.getNumChannels() >= 2)
    {
        const int samplesToCopy = juce::jmin(numSamples, vizBufferSize);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (out.getNumChannels() > ch)
            {
                const float* outputData = out.getReadPointer(ch);
                for (int i = 0; i < samplesToCopy; ++i)
                {
                    const int writeIdx = (vizWritePos + i) % vizBufferSize;
                    vizOutputBuffer.setSample(ch, writeIdx, outputData[i]);
                }
            }
        }
        vizWritePos = (vizWritePos + samplesToCopy) % vizBufferSize;
    }

    // Update visualization data (thread-safe)
    // Downsample waveforms from circular buffers
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx =
            (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) %
            vizBufferSize;
        if (vizInputABuffer.getNumChannels() > 0 && vizInputBBuffer.getNumChannels() > 0 &&
            vizOutputBuffer.getNumChannels() > 0)
        {
            vizData.inputAWaveformL[i].store(vizInputABuffer.getSample(0, readIdx));
            vizData.inputAWaveformR[i].store(
                vizInputABuffer.getNumChannels() > 1 ? vizInputABuffer.getSample(1, readIdx)
                                                     : vizInputABuffer.getSample(0, readIdx));
            vizData.inputBWaveformL[i].store(vizInputBBuffer.getSample(0, readIdx));
            vizData.inputBWaveformR[i].store(
                vizInputBBuffer.getNumChannels() > 1 ? vizInputBBuffer.getSample(1, readIdx)
                                                     : vizInputBBuffer.getSample(0, readIdx));
            vizData.outputWaveformL[i].store(vizOutputBuffer.getSample(0, readIdx));
            vizData.outputWaveformR[i].store(
                vizOutputBuffer.getNumChannels() > 1 ? vizOutputBuffer.getSample(1, readIdx)
                                                     : vizOutputBuffer.getSample(0, readIdx));
        }
    }

    // Calculate input/output levels (RMS)
    float inputARms = 0.0f;
    float inputBRms = 0.0f;
    float outputRmsL = 0.0f;
    float outputRmsR = 0.0f;
    if (numSamples > 0)
    {
        if (inA.getNumChannels() > 0)
            inputARms = inA.getRMSLevel(0, 0, numSamples);
        if (inB.getNumChannels() > 0)
            inputBRms = inB.getRMSLevel(0, 0, numSamples);
        if (out.getNumChannels() > 0)
            outputRmsL = out.getRMSLevel(0, 0, numSamples);
        if (out.getNumChannels() > 1)
            outputRmsR = out.getRMSLevel(1, 0, numSamples);
    }
    vizData.inputALevelDb.store(juce::Decibels::gainToDecibels(inputARms, -60.0f));
    vizData.inputBLevelDb.store(juce::Decibels::gainToDecibels(inputBRms, -60.0f));
    vizData.outputLevelDbL.store(juce::Decibels::gainToDecibels(outputRmsL, -60.0f));
    vizData.outputLevelDbR.store(juce::Decibels::gainToDecibels(outputRmsR, -60.0f));
    vizData.currentCrossfade.store(crossfade);
    vizData.currentGainDb.store(
        isParamInputConnected("gain") ? (-60.0f + gainModCV * 66.0f)
                                      : (gainParam != nullptr ? gainParam->load() : 0.0f));
    vizData.currentPan.store(pan);
#endif
}

// Updated UI drawing code
#if defined(PRESET_CREATOR_UI)
void MixerModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    auto&       ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    ImGui::PushID(this); // Unique ID for this node's UI - must be at start

    // Helper for tooltips
    auto HelpMarkerMixer = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    float gainDb = gainParam != nullptr ? gainParam->load() : 0.0f;
    float pan = panParam != nullptr ? panParam->load() : 0.0f;
    float crossfade = crossfadeParam != nullptr ? crossfadeParam->load() : 0.0f;

    ImGui::PushItemWidth(itemWidth);

    // === CROSSFADE SECTION ===
    ThemeText("Crossfade", theme.text.section_header);
    ImGui::Spacing();

    // Crossfade Slider
    bool isXfModulated = isParamModulated("x-fade");
    if (isXfModulated)
    {
        crossfade = getLiveParamValueFor("x-fade", "crossfade_live", crossfade);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for X-Fade Mod (channel 6)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(6)) // Channel 6 = X-Fade Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("A <-> B", &crossfade, -1.0f, 1.0f))
        if (!isXfModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("crossfade")))
                *p = crossfade;
    if (!isXfModulated)
        adjustParamOnWheel(ap.getParameter("crossfade"), "crossfade", crossfade);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isXfModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ThemeText("(mod)", theme.text.active);
    }
    ImGui::SameLine();
    HelpMarkerMixer("Crossfade between inputs A and B\n-1 = A only, 0 = equal mix, +1 = B only");

    // Visual crossfade indicator
    float aLevel = (1.0f - crossfade) / 2.0f;
    float bLevel = (1.0f + crossfade) / 2.0f;
    ImGui::Text("A: %.1f%%", aLevel * 100.0f);
    ImGui::SameLine(itemWidth * 0.5f);
    ImGui::Text("B: %.1f%%", bLevel * 100.0f);

    ImGui::Spacing();
    ImGui::Spacing();

    // === SECTION: Mixer Visualization ===
    ThemeText("Mixer Activity", theme.text.section_header);
    ImGui::Spacing();

    // Read visualization data (thread-safe) - BEFORE BeginChild
    float inputAWaveform[VizData::waveformPoints];
    float inputBWaveform[VizData::waveformPoints];
    float outputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputAWaveform[i] = vizData.inputAWaveformL[i].load();
        inputBWaveform[i] = vizData.inputBWaveformL[i].load();
        outputWaveform[i] = vizData.outputWaveformL[i].load();
    }
    const float currentCrossfade = vizData.currentCrossfade.load();
    const float currentGainDb = vizData.currentGainDb.load();
    const float currentPan = vizData.currentPan.load();
    const float inputALevelDb = vizData.inputALevelDb.load();
    const float inputBLevelDb = vizData.inputBLevelDb.load();
    const float outputLevelDbL = vizData.outputLevelDbL.load();
    const float outputLevelDbR = vizData.outputLevelDbR.load();

    // Waveform visualization in child window
    const float            waveHeight = 140.0f;
    const ImVec2           graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("MixerViz", graphSize, false, childFlags))
    {
        ImDrawList*  drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);

        // Background
        const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);

        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);

        const ImU32 inputAColor =
            ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency); // Cyan
        const ImU32 inputBColor =
            ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre); // Orange/Yellow
        const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(
            theme.modulation.amplitude); // Magenta/Pink - distinct from both inputs
        const ImU32 centerLineColor = IM_COL32(150, 150, 150, 100);

        // Draw output waveforms
        const float midY = p0.y + graphSize.y * 0.5f;
        const float scaleY = graphSize.y * 0.45f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        // Draw center line (zero reference)
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), centerLineColor, 1.0f);

        // Draw input A waveform (background, most subtle)
        float prevX = p0.x;
        float prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, inputAWaveform[i]);
            const float x = p0.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0)
            {
                ImVec4 colorVec4 = ImGui::ColorConvertU32ToFloat4(inputAColor);
                colorVec4.w = 0.3f; // Very transparent for background
                drawList->AddLine(
                    ImVec2(prevX, prevY),
                    ImVec2(x, y),
                    ImGui::ColorConvertFloat4ToU32(colorVec4),
                    1.5f);
            }
            prevX = x;
            prevY = y;
        }

        // Draw input B waveform (middle layer)
        prevX = p0.x;
        prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, inputBWaveform[i]);
            const float x = p0.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0)
            {
                ImVec4 colorVec4 = ImGui::ColorConvertU32ToFloat4(inputBColor);
                colorVec4.w = 0.35f; // Slightly more visible than A
                drawList->AddLine(
                    ImVec2(prevX, prevY),
                    ImVec2(x, y),
                    ImGui::ColorConvertFloat4ToU32(colorVec4),
                    1.8f);
            }
            prevX = x;
            prevY = y;
        }

        // Draw output waveform (foreground, shows crossfade result)
        prevX = p0.x;
        prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveform[i]);
            const float x = p0.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), outputColor, 2.8f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();

        // Level meters overlay
        ImGui::SetCursorPos(ImVec2(4, waveHeight + 4));
        const float inputANorm = juce::jlimit(0.0f, 1.0f, (inputALevelDb + 60.0f) / 60.0f);
        const float inputBNorm = juce::jlimit(0.0f, 1.0f, (inputBLevelDb + 60.0f) / 60.0f);
        const float outputLNorm = juce::jlimit(0.0f, 1.0f, (outputLevelDbL + 60.0f) / 60.0f);
        const float outputRNorm = juce::jlimit(0.0f, 1.0f, (outputLevelDbR + 60.0f) / 60.0f);

        ImGui::Text("In A: %.1f dB", inputALevelDb);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, inputAColor);
        ImGui::ProgressBar(inputANorm, ImVec2(graphSize.x * 0.4f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", inputANorm * 100.0f);

        ImGui::Text("In B: %.1f dB", inputBLevelDb);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, inputBColor);
        ImGui::ProgressBar(inputBNorm, ImVec2(graphSize.x * 0.4f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", inputBNorm * 100.0f);

        ImGui::Text("Out L: %.1f dB", outputLevelDbL);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, outputColor);
        ImGui::ProgressBar(outputLNorm, ImVec2(graphSize.x * 0.4f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", outputLNorm * 100.0f);

        ImGui::Text("Out R: %.1f dB", outputLevelDbR);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, outputColor);
        ImGui::ProgressBar(outputRNorm, ImVec2(graphSize.x * 0.4f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", outputRNorm * 100.0f);

        // Parameter readouts
        ImGui::Text(
            "Crossfade: %.2f  |  Gain: %.1f dB  |  Pan: %.2f",
            currentCrossfade,
            currentGainDb,
            currentPan);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##mixerVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Spacing();

    // === MASTER CONTROLS SECTION ===
    ThemeText("Master Controls", theme.text.section_header);
    ImGui::Spacing();

    // Gain Slider
    bool isGainModulated = isParamModulated("gain");
    if (isGainModulated)
    {
        gainDb = getLiveParamValueFor("gain", "gain_live", gainDb);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Gain Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4)) // Channel 4 = Gain Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Gain dB", &gainDb, -60.0f, 6.0f))
        if (!isGainModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("gain")))
                *p = gainDb;
    if (!isGainModulated)
        adjustParamOnWheel(ap.getParameter("gain"), "gainDb", gainDb);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isGainModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ThemeText("(mod)", theme.text.active);
    }
    ImGui::SameLine();
    HelpMarkerMixer("Master output gain (-60 to +6 dB)");

    // Pan Slider
    bool isPanModulated = isParamModulated("pan");
    if (isPanModulated)
    {
        pan = getLiveParamValueFor("pan", "pan_live", pan);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Pan Mod (channel 5)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(5)) // Channel 5 = Pan Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Pan", &pan, -1.0f, 1.0f))
        if (!isPanModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("pan")))
                *p = pan;
    if (!isPanModulated)
        adjustParamOnWheel(ap.getParameter("pan"), "pan", pan);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isPanModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ThemeText("(mod)", theme.text.active);
    }
    ImGui::SameLine();
    HelpMarkerMixer("Stereo panning\n-1 = full left, 0 = center, +1 = full right");

    // Visual pan indicator
    const char* panLabel = (pan < -0.3f) ? "L" : (pan > 0.3f) ? "R" : "C";
    ImGui::Text("Position: %s (%.2f)", panLabel, pan);

    ImGui::PopItemWidth();
    ImGui::PopID(); // End unique ID
}
#endif

void MixerModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 4-6) are now inline
    helpers.drawParallelPins("In A L", 0, "Out L", 0);
    helpers.drawParallelPins("In A R", 1, "Out R", 1);
    helpers.drawParallelPins("In B L", 2, nullptr, -1);
    helpers.drawParallelPins("In B R", 3, nullptr, -1);
    // Gain Mod (ch 4), Pan Mod (ch 5), X-Fade Mod (ch 6) are drawn inline in drawParametersInNode
}

bool MixerModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outChannelIndexInBus = 0;
    if (paramId == "gain")
    {
        outBusIndex = 2;
        return true;
    }
    if (paramId == "pan")
    {
        outBusIndex = 3;
        return true;
    }
    if (paramId == "crossfade" || paramId == "x-fade")
    {
        outBusIndex = 4;
        return true;
    }
    return false;
}