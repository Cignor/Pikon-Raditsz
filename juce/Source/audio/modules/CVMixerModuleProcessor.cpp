#include "CVMixerModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

CVMixerModuleProcessor::CVMixerModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "CV Inputs",
                  juce::AudioChannelSet::discreteChannels(4),
                  true)                                                        // Bus 0: A, B, C, D
              .withInput("Crossfade Mod", juce::AudioChannelSet::mono(), true) // Bus 1
              .withInput("Level A Mod", juce::AudioChannelSet::mono(), true)   // Bus 2
              .withInput("Level C Mod", juce::AudioChannelSet::mono(), true)   // Bus 3
              .withInput("Level D Mod", juce::AudioChannelSet::mono(), true)   // Bus 4
              .withOutput(
                  "Outputs",
                  juce::AudioChannelSet::discreteChannels(2),
                  true)), // Bus 0: Mix Out, Inv Out
      apvts(*this, nullptr, "CVMixerParams", createParameterLayout())
{
    crossfadeParam = apvts.getRawParameterValue("crossfade");
    levelAParam = apvts.getRawParameterValue("levelA");
    levelCParam = apvts.getRawParameterValue("levelC");
    levelDParam = apvts.getRawParameterValue("levelD");

    // Initialize value tooltips for the two outputs
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Mix Out
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Inv Out
}

juce::AudioProcessorValueTreeState::ParameterLayout CVMixerModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    // Crossfade: -1 = full A, 0 = equal mix, +1 = full B
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "crossfade",
            "Crossfade A/B",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.001f),
            0.0f));

    // Level A: master level for the A/B crossfade section (0..1)
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "levelA", "Level A/B", juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));

    // Level C: bipolar for adding/subtracting input C
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "levelC", "Level C", juce::NormalisableRange<float>(-1.0f, 1.0f, 0.001f), 0.0f));

    // Level D: bipolar for adding/subtracting input D
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "levelD", "Level D", juce::NormalisableRange<float>(-1.0f, 1.0f, 0.001f), 0.0f));

    return {p.begin(), p.end()};
}

void CVMixerModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);

#if defined(PRESET_CREATOR_UI)
    vizInputABuffer.setSize(1, vizBufferSize, false, true, true);
    vizInputBBuffer.setSize(1, vizBufferSize, false, true, true);
    vizInputCBuffer.setSize(1, vizBufferSize, false, true, true);
    vizInputDBuffer.setSize(1, vizBufferSize, false, true, true);
    vizMixOutputBuffer.setSize(1, vizBufferSize, false, true, true);
    vizInvOutputBuffer.setSize(1, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.inputAWaveform)
        v.store(0.0f);
    for (auto& v : vizData.inputBWaveform)
        v.store(0.0f);
    for (auto& v : vizData.inputCWaveform)
        v.store(0.0f);
    for (auto& v : vizData.inputDWaveform)
        v.store(0.0f);
    for (auto& v : vizData.mixOutputWaveform)
        v.store(0.0f);
    for (auto& v : vizData.invOutputWaveform)
        v.store(0.0f);
    vizData.currentCrossfade.store(0.0f);
    vizData.currentLevelA.store(1.0f);
    vizData.currentLevelC.store(0.0f);
    vizData.currentLevelD.store(0.0f);
    vizData.inputALevel.store(0.0f);
    vizData.inputBLevel.store(0.0f);
    vizData.inputCLevel.store(0.0f);
    vizData.inputDLevel.store(0.0f);
    vizData.mixOutputLevel.store(0.0f);
    vizData.invOutputLevel.store(0.0f);
#endif
}

void CVMixerModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // Get input and output buses
    auto cvInputs = getBusBuffer(buffer, true, 0); // 4 discrete channels: A, B, C, D
    auto outputs = getBusBuffer(buffer, false, 0); // 2 discrete channels: Mix Out, Inv Out

    const int numSamples = buffer.getNumSamples();

#if defined(PRESET_CREATOR_UI)
    // Capture input audio for visualization (before processing)
    const int samplesToCopy = juce::jmin(numSamples, vizBufferSize);
    if (vizInputABuffer.getNumSamples() > 0 && cvInputs.getNumChannels() > 0)
    {
        const float* inputData = cvInputs.getReadPointer(0);
        for (int i = 0; i < samplesToCopy; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputABuffer.setSample(0, writeIdx, inputData[i]);
        }
    }
    if (vizInputBBuffer.getNumSamples() > 0 && cvInputs.getNumChannels() > 1)
    {
        const float* inputData = cvInputs.getReadPointer(1);
        for (int i = 0; i < samplesToCopy; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputBBuffer.setSample(0, writeIdx, inputData[i]);
        }
    }
    if (vizInputCBuffer.getNumSamples() > 0 && cvInputs.getNumChannels() > 2)
    {
        const float* inputData = cvInputs.getReadPointer(2);
        for (int i = 0; i < samplesToCopy; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputCBuffer.setSample(0, writeIdx, inputData[i]);
        }
    }
    if (vizInputDBuffer.getNumSamples() > 0 && cvInputs.getNumChannels() > 3)
    {
        const float* inputData = cvInputs.getReadPointer(3);
        for (int i = 0; i < samplesToCopy; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputDBuffer.setSample(0, writeIdx, inputData[i]);
        }
    }
#endif

    // Read modulation CV values (first sample of each mod bus)
    float crossfadeMod = 0.0f;
    float levelAMod = 0.0f;
    float levelCMod = 0.0f;
    float levelDMod = 0.0f;

    if (isParamInputConnected("crossfade"))
    {
        const auto& modBus = getBusBuffer(buffer, true, 1);
        if (modBus.getNumChannels() > 0)
            crossfadeMod = modBus.getReadPointer(0)[0];
    }

    if (isParamInputConnected("levelA"))
    {
        const auto& modBus = getBusBuffer(buffer, true, 2);
        if (modBus.getNumChannels() > 0)
            levelAMod = modBus.getReadPointer(0)[0];
    }

    if (isParamInputConnected("levelC"))
    {
        const auto& modBus = getBusBuffer(buffer, true, 3);
        if (modBus.getNumChannels() > 0)
            levelCMod = modBus.getReadPointer(0)[0];
    }

    if (isParamInputConnected("levelD"))
    {
        const auto& modBus = getBusBuffer(buffer, true, 4);
        if (modBus.getNumChannels() > 0)
            levelDMod = modBus.getReadPointer(0)[0];
    }

    // Determine final parameter values (modulated or from parameters)
    float crossfade = 0.0f;
    if (isParamInputConnected("crossfade"))
    {
        // Map CV [0,1] to crossfade [-1, 1]
        crossfade = -1.0f + crossfadeMod * 2.0f;
    }
    else
    {
        crossfade = crossfadeParam != nullptr ? crossfadeParam->load() : 0.0f;
    }

    float levelA = 0.0f;
    if (isParamInputConnected("levelA"))
    {
        // Map CV [0,1] to levelA [0, 1]
        levelA = levelAMod;
    }
    else
    {
        levelA = levelAParam != nullptr ? levelAParam->load() : 1.0f;
    }

    float levelC = 0.0f;
    if (isParamInputConnected("levelC"))
    {
        // Map CV [0,1] to levelC [-1, 1]
        levelC = -1.0f + levelCMod * 2.0f;
    }
    else
    {
        levelC = levelCParam != nullptr ? levelCParam->load() : 0.0f;
    }

    float levelD = 0.0f;
    if (isParamInputConnected("levelD"))
    {
        // Map CV [0,1] to levelD [-1, 1]
        levelD = -1.0f + levelDMod * 2.0f;
    }
    else
    {
        levelD = levelDParam != nullptr ? levelDParam->load() : 0.0f;
    }

    // Get read pointers for all inputs (may be null if not connected)
    const float* inA = cvInputs.getNumChannels() > 0 ? cvInputs.getReadPointer(0) : nullptr;
    const float* inB = cvInputs.getNumChannels() > 1 ? cvInputs.getReadPointer(1) : nullptr;
    const float* inC = cvInputs.getNumChannels() > 2 ? cvInputs.getReadPointer(2) : nullptr;
    const float* inD = cvInputs.getNumChannels() > 3 ? cvInputs.getReadPointer(3) : nullptr;

    // Get write pointers for outputs
    float* mixOut = outputs.getNumChannels() > 0 ? outputs.getWritePointer(0) : nullptr;
    float* invOut = outputs.getNumChannels() > 1 ? outputs.getWritePointer(1) : nullptr;

    // Process each sample
    for (int i = 0; i < numSamples; ++i)
    {
        // Read input samples (0.0 if not connected)
        const float a = inA ? inA[i] : 0.0f;
        const float b = inB ? inB[i] : 0.0f;
        const float c = inC ? inC[i] : 0.0f;
        const float d = inD ? inD[i] : 0.0f;

        // 1. Linear crossfade between A and B
        // Convert crossfade from [-1, 1] to mix amount [0, 1]
        const float mixAmount = (crossfade + 1.0f) * 0.5f;
        const float crossfaded_AB = (a * (1.0f - mixAmount)) + (b * mixAmount);

        // 2. Apply master level for the A/B section
        const float final_AB = crossfaded_AB * levelA;

        // 3. Sum all inputs with their respective levels
        const float finalMix = final_AB + (c * levelC) + (d * levelD);

        // 4. Write to outputs
        if (mixOut)
            mixOut[i] = finalMix;
        if (invOut)
            invOut[i] = -finalMix;

#if defined(PRESET_CREATOR_UI)
        // Capture output audio for visualization (after processing)
        if (vizMixOutputBuffer.getNumSamples() > 0 && mixOut)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizMixOutputBuffer.setSample(0, writeIdx, mixOut[i]);
        }
        if (vizInvOutputBuffer.getNumSamples() > 0 && invOut)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInvOutputBuffer.setSample(0, writeIdx, invOut[i]);
        }
#endif
    }

    // Store live modulated values for UI display
    setLiveParamValue("crossfade_live", crossfade);
    setLiveParamValue("levelA_live", levelA);
    setLiveParamValue("levelC_live", levelC);
    setLiveParamValue("levelD_live", levelD);

    // Update tooltips with last sample values
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0] && mixOut)
            lastOutputValues[0]->store(mixOut[numSamples - 1]);
        if (lastOutputValues[1] && invOut)
            lastOutputValues[1]->store(invOut[numSamples - 1]);
    }

#if defined(PRESET_CREATOR_UI)
    vizWritePos = (vizWritePos + numSamples) % vizBufferSize;

    // Update visualization data (thread-safe)
    // Downsample waveforms from circular buffers
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx =
            (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) %
            vizBufferSize;
        if (vizInputABuffer.getNumSamples() > 0)
            vizData.inputAWaveform[i].store(vizInputABuffer.getSample(0, readIdx));
        if (vizInputBBuffer.getNumSamples() > 0)
            vizData.inputBWaveform[i].store(vizInputBBuffer.getSample(0, readIdx));
        if (vizInputCBuffer.getNumSamples() > 0)
            vizData.inputCWaveform[i].store(vizInputCBuffer.getSample(0, readIdx));
        if (vizInputDBuffer.getNumSamples() > 0)
            vizData.inputDWaveform[i].store(vizInputDBuffer.getSample(0, readIdx));
        if (vizMixOutputBuffer.getNumSamples() > 0)
            vizData.mixOutputWaveform[i].store(vizMixOutputBuffer.getSample(0, readIdx));
        if (vizInvOutputBuffer.getNumSamples() > 0)
            vizData.invOutputWaveform[i].store(vizInvOutputBuffer.getSample(0, readIdx));
    }

    // Calculate input/output levels (RMS)
    float inputARms = 0.0f;
    float inputBRms = 0.0f;
    float inputCRms = 0.0f;
    float inputDRms = 0.0f;
    float mixOutputRms = 0.0f;
    float invOutputRms = 0.0f;
    if (numSamples > 0)
    {
        if (cvInputs.getNumChannels() > 0)
            inputARms = cvInputs.getRMSLevel(0, 0, numSamples);
        if (cvInputs.getNumChannels() > 1)
            inputBRms = cvInputs.getRMSLevel(1, 0, numSamples);
        if (cvInputs.getNumChannels() > 2)
            inputCRms = cvInputs.getRMSLevel(2, 0, numSamples);
        if (cvInputs.getNumChannels() > 3)
            inputDRms = cvInputs.getRMSLevel(3, 0, numSamples);
        if (outputs.getNumChannels() > 0 && mixOut)
            mixOutputRms = outputs.getRMSLevel(0, 0, numSamples);
        if (outputs.getNumChannels() > 1 && invOut)
            invOutputRms = outputs.getRMSLevel(1, 0, numSamples);
    }
    vizData.inputALevel.store(inputARms);
    vizData.inputBLevel.store(inputBRms);
    vizData.inputCLevel.store(inputCRms);
    vizData.inputDLevel.store(inputDRms);
    vizData.mixOutputLevel.store(mixOutputRms);
    vizData.invOutputLevel.store(invOutputRms);
    vizData.currentCrossfade.store(crossfade);
    vizData.currentLevelA.store(levelA);
    vizData.currentLevelC.store(levelC);
    vizData.currentLevelD.store(levelD);
#endif
}

#if defined(PRESET_CREATOR_UI)
void CVMixerModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    auto&       ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    float       crossfade = crossfadeParam != nullptr ? crossfadeParam->load() : 0.0f;
    float       levelA = levelAParam != nullptr ? levelAParam->load() : 1.0f;
    float       levelC = levelCParam != nullptr ? levelCParam->load() : 0.0f;
    float       levelD = levelDParam != nullptr ? levelDParam->load() : 0.0f;

    ImGui::PushID(this); // Unique ID for this node's UI - must be at start
    ImGui::PushItemWidth(itemWidth);

    // === SECTION: CV Mixer Visualization ===
    ThemeText("CV Mixer Activity", theme.text.section_header);
    ImGui::Spacing();

    // Read visualization data (thread-safe) - BEFORE BeginChild
    float inputAWaveform[VizData::waveformPoints];
    float inputBWaveform[VizData::waveformPoints];
    float inputCWaveform[VizData::waveformPoints];
    float inputDWaveform[VizData::waveformPoints];
    float mixOutputWaveform[VizData::waveformPoints];
    float invOutputWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputAWaveform[i] = vizData.inputAWaveform[i].load();
        inputBWaveform[i] = vizData.inputBWaveform[i].load();
        inputCWaveform[i] = vizData.inputCWaveform[i].load();
        inputDWaveform[i] = vizData.inputDWaveform[i].load();
        mixOutputWaveform[i] = vizData.mixOutputWaveform[i].load();
        invOutputWaveform[i] = vizData.invOutputWaveform[i].load();
    }
    const float currentCrossfade = vizData.currentCrossfade.load();
    const float currentLevelA = vizData.currentLevelA.load();
    const float currentLevelC = vizData.currentLevelC.load();
    const float currentLevelD = vizData.currentLevelD.load();
    const float inputALevel = vizData.inputALevel.load();
    const float inputBLevel = vizData.inputBLevel.load();
    const float inputCLevel = vizData.inputCLevel.load();
    const float inputDLevel = vizData.inputDLevel.load();
    const float mixOutputLevel = vizData.mixOutputLevel.load();
    const float invOutputLevel = vizData.invOutputLevel.load();

    // Waveform visualization in child window
    const float            waveHeight = 140.0f;
    const ImVec2           graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("CVMixerViz", graphSize, false, childFlags))
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
        const ImU32 inputCColor =
            ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude); // Magenta/Pink
        const ImU32 inputDColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.filter); // Green
        const ImU32 mixOutputColor = ImGui::ColorConvertFloat4ToU32(theme.accent); // Accent color
        const ImU32 invOutputColor = IM_COL32(180, 180, 255, 255); // Light blue for inverted output
        const ImU32 centerLineColor = IM_COL32(150, 150, 150, 100);

        // Draw output waveforms
        const float midY = p0.y + graphSize.y * 0.5f;
        const float scaleY = graphSize.y * 0.45f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        // Draw center line (zero reference)
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), centerLineColor, 1.0f);

        // Draw input waveforms (background layers, more subtle)
        auto drawWaveform = [&](const float* waveform, ImU32 color, float alpha, float thickness) {
            float prevX = p0.x;
            float prevY = midY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float sample = juce::jlimit(-1.0f, 1.0f, waveform[i]);
                const float x = p0.x + i * stepX;
                const float y = midY - sample * scaleY;
                if (i > 0)
                {
                    ImVec4 colorVec4 = ImGui::ColorConvertU32ToFloat4(color);
                    colorVec4.w = alpha;
                    drawList->AddLine(
                        ImVec2(prevX, prevY),
                        ImVec2(x, y),
                        ImGui::ColorConvertFloat4ToU32(colorVec4),
                        thickness);
                }
                prevX = x;
                prevY = y;
            }
        };

        // Draw inputs in order (most subtle first)
        drawWaveform(inputAWaveform, inputAColor, 0.25f, 1.2f);
        drawWaveform(inputBWaveform, inputBColor, 0.3f, 1.4f);
        drawWaveform(inputCWaveform, inputCColor, 0.25f, 1.2f);
        drawWaveform(inputDWaveform, inputDColor, 0.25f, 1.2f);

        // Draw inverted output (middle layer, shows polarity flip)
        drawWaveform(invOutputWaveform, invOutputColor, 0.4f, 1.6f);

        // Draw mix output (foreground, most prominent - shows final result)
        float prevX = p0.x;
        float prevY = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, mixOutputWaveform[i]);
            const float x = p0.x + i * stepX;
            const float y = midY - sample * scaleY;
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), mixOutputColor, 2.8f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();

        // Level meters overlay
        ImGui::SetCursorPos(ImVec2(4, waveHeight + 4));
        auto drawLevelMeter = [&](const char* label, float level, ImU32 color, float maxWidth) {
            const float norm = juce::jlimit(0.0f, 1.0f, level);
            ImGui::Text("%s: %.3f", label, level);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar(norm, ImVec2(maxWidth, 0), "");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%.0f%%", norm * 100.0f);
        };

        const float meterWidth = graphSize.x * 0.35f;
        drawLevelMeter("In A", inputALevel, inputAColor, meterWidth);
        drawLevelMeter("In B", inputBLevel, inputBColor, meterWidth);
        drawLevelMeter("In C", inputCLevel, inputCColor, meterWidth);
        drawLevelMeter("In D", inputDLevel, inputDColor, meterWidth);
        drawLevelMeter("Mix", mixOutputLevel, mixOutputColor, meterWidth);
        drawLevelMeter("Inv", invOutputLevel, invOutputColor, meterWidth);

        // Parameter readouts
        ImGui::Text(
            "Crossfade: %.2f  |  Level A: %.2f  |  C: %.2f  |  D: %.2f",
            currentCrossfade,
            currentLevelA,
            currentLevelC,
            currentLevelD);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##cvMixerVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Spacing();

    // Crossfade A/B (horizontal slider)
    bool isCrossfadeModulated = isParamModulated("crossfade");
    if (isCrossfadeModulated)
    {
        crossfade = getLiveParamValueFor("crossfade", "crossfade_live", crossfade);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Crossfade Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4)) // Channel 4 = Crossfade Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("A <-> B", &crossfade, -1.0f, 1.0f))
    {
        if (!isCrossfadeModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("crossfade")))
                *p = crossfade;
        }
    }
    if (!isCrossfadeModulated)
        adjustParamOnWheel(ap.getParameter("crossfade"), "crossfade", crossfade);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isCrossfadeModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    ImGui::Spacing();

    // Level A (master level for A/B section)
    bool isLevelAModulated = isParamModulated("levelA");
    if (isLevelAModulated)
    {
        levelA = getLiveParamValueFor("levelA", "levelA_live", levelA);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Level A Mod (channel 5)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(5)) // Channel 5 = Level A Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Level A/B", &levelA, 0.0f, 1.0f))
    {
        if (!isLevelAModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("levelA")))
                *p = levelA;
        }
    }
    if (!isLevelAModulated)
        adjustParamOnWheel(ap.getParameter("levelA"), "levelA", levelA);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isLevelAModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    // Level C (bipolar)
    bool isLevelCModulated = isParamModulated("levelC");
    if (isLevelCModulated)
    {
        levelC = getLiveParamValueFor("levelC", "levelC_live", levelC);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Level C Mod (channel 6)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(6)) // Channel 6 = Level C Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Level C", &levelC, -1.0f, 1.0f))
    {
        if (!isLevelCModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("levelC")))
                *p = levelC;
        }
    }
    if (!isLevelCModulated)
        adjustParamOnWheel(ap.getParameter("levelC"), "levelC", levelC);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isLevelCModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    // Level D (bipolar)
    bool isLevelDModulated = isParamModulated("levelD");
    if (isLevelDModulated)
    {
        levelD = getLiveParamValueFor("levelD", "levelD_live", levelD);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Level D Mod (channel 7)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(7)) // Channel 7 = Level D Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Level D", &levelD, -1.0f, 1.0f))
    {
        if (!isLevelDModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("levelD")))
                *p = levelD;
        }
    }
    if (!isLevelDModulated)
        adjustParamOnWheel(ap.getParameter("levelD"), "levelD", levelD);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isLevelDModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID(); // End unique ID
}
#endif

void CVMixerModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw CV input pins - modulation pins (channels 4-7) are now inline
    helpers.drawParallelPins("In A", 0, "Mix Out", 0);
    helpers.drawParallelPins("In B", 1, "Inv Out", 1);
    helpers.drawParallelPins("In C", 2, nullptr, -1);
    helpers.drawParallelPins("In D", 3, nullptr, -1);
    // Crossfade Mod (ch 4), Level A Mod (ch 5), Level C Mod (ch 6), Level D Mod (ch 7) are drawn
    // inline in drawParametersInNode
}

bool CVMixerModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outChannelIndexInBus = 0;
    if (paramId == "crossfade")
    {
        outBusIndex = 1;
        return true;
    }
    if (paramId == "levelA")
    {
        outBusIndex = 2;
        return true;
    }
    if (paramId == "levelC")
    {
        outBusIndex = 3;
        return true;
    }
    if (paramId == "levelD")
    {
        outBusIndex = 4;
        return true;
    }
    return false;
}
