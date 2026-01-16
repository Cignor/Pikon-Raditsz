#include "SequentialSwitchModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

SequentialSwitchModuleProcessor::SequentialSwitchModuleProcessor()
    : ModuleProcessor(
        BusesProperties()
            .withInput  ("Inputs",   juce::AudioChannelSet::discreteChannels(5), true)
            .withOutput ("Outputs",  juce::AudioChannelSet::discreteChannels(4), true)
      )
    , apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    // Cache parameter pointers
    threshold1Param = apvts.getRawParameterValue(paramIdThreshold1);
    threshold2Param = apvts.getRawParameterValue(paramIdThreshold2);
    threshold3Param = apvts.getRawParameterValue(paramIdThreshold3);
    threshold4Param = apvts.getRawParameterValue(paramIdThreshold4);
    
    // Initialize lastOutputValues for cable inspector (4 outputs)
    for (int i = 0; i < 4; ++i)
        lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout SequentialSwitchModuleProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{paramIdThreshold1, 1},
        "Threshold 1",
        0.0f,  // min
        1.0f,  // max
        0.5f   // default
    ));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{paramIdThreshold2, 1},
        "Threshold 2",
        0.0f,  // min
        1.0f,  // max
        0.5f   // default
    ));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{paramIdThreshold3, 1},
        "Threshold 3",
        0.0f,  // min
        1.0f,  // max
        0.5f   // default
    ));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{paramIdThreshold4, 1},
        "Threshold 4",
        0.0f,  // min
        1.0f,  // max
        0.5f   // default
    ));

    return layout;
}

void SequentialSwitchModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);

#if defined(PRESET_CREATOR_UI)
    vizInputBuffer.setSize(1, vizBufferSize, false, true, true);
    vizOutput1Buffer.setSize(1, vizBufferSize, false, true, true);
    vizOutput2Buffer.setSize(1, vizBufferSize, false, true, true);
    vizOutput3Buffer.setSize(1, vizBufferSize, false, true, true);
    vizOutput4Buffer.setSize(1, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.inputWaveform) v.store(0.0f);
    for (auto& v : vizData.output1Waveform) v.store(0.0f);
    for (auto& v : vizData.output2Waveform) v.store(0.0f);
    for (auto& v : vizData.output3Waveform) v.store(0.0f);
    for (auto& v : vizData.output4Waveform) v.store(0.0f);
    vizData.currentThreshold1.store(0.5f);
    vizData.currentThreshold2.store(0.5f);
    vizData.currentThreshold3.store(0.5f);
    vizData.currentThreshold4.store(0.5f);
    vizData.output1Active.store(false);
    vizData.output2Active.store(false);
    vizData.output3Active.store(false);
    vizData.output4Active.store(false);
#endif
}

void SequentialSwitchModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    // Get input bus
    auto inBus = getBusBuffer(buffer, true, 0);

    // Get output bus (4 channels)
    auto outBus = getBusBuffer(buffer, false, 0);

    // Safety check
    if (outBus.getNumChannels() == 0)
        return;

    const int numSamples = buffer.getNumSamples();

#if defined(PRESET_CREATOR_UI)
    // Capture input audio for visualization (before processing)
    const int samplesToCopy = juce::jmin(numSamples, vizBufferSize);
    if (vizInputBuffer.getNumSamples() > 0 && inBus.getNumChannels() > 0)
    {
        const float* inputData = inBus.getReadPointer(0);
        for (int i = 0; i < samplesToCopy; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputBuffer.setSample(0, writeIdx, inputData[i]);
        }
    }
#endif

    // Check which thresholds are modulated
    const bool isThresh1Mod = isParamInputConnected(paramIdThreshold1Mod);
    const bool isThresh2Mod = isParamInputConnected(paramIdThreshold2Mod);
    const bool isThresh3Mod = isParamInputConnected(paramIdThreshold3Mod);
    const bool isThresh4Mod = isParamInputConnected(paramIdThreshold4Mod);

    // Get input pointers
    const float* gateIn    = inBus.getNumChannels() > 0 ? inBus.getReadPointer(0) : nullptr;
    const float* thresh1CV = isThresh1Mod && inBus.getNumChannels() > 1 ? inBus.getReadPointer(1) : nullptr;
    const float* thresh2CV = isThresh2Mod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* thresh3CV = isThresh3Mod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;
    const float* thresh4CV = isThresh4Mod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;

    // Get base threshold values
    const float baseThreshold1 = threshold1Param->load();
    const float baseThreshold2 = threshold2Param->load();
    const float baseThreshold3 = threshold3Param->load();
    const float baseThreshold4 = threshold4Param->load();

    // Process each sample
    for (int i = 0; i < numSamples; ++i)
    {
        // Read input signal (default to 0.0 if not connected)
        const float inputSignal = gateIn ? gateIn[i] : 0.0f;

        // Calculate modulated thresholds
        float threshold1 = baseThreshold1;
        if (isThresh1Mod && thresh1CV)
            threshold1 = juce::jlimit(0.0f, 1.0f, thresh1CV[i]);

        float threshold2 = baseThreshold2;
        if (isThresh2Mod && thresh2CV)
            threshold2 = juce::jlimit(0.0f, 1.0f, thresh2CV[i]);

        float threshold3 = baseThreshold3;
        if (isThresh3Mod && thresh3CV)
            threshold3 = juce::jlimit(0.0f, 1.0f, thresh3CV[i]);

        float threshold4 = baseThreshold4;
        if (isThresh4Mod && thresh4CV)
            threshold4 = juce::jlimit(0.0f, 1.0f, thresh4CV[i]);

        // Output to each channel based on threshold comparison
        // If input >= threshold, pass the signal, otherwise output 0
        const float out1 = inputSignal >= threshold1 ? inputSignal : 0.0f;
        const float out2 = inputSignal >= threshold2 ? inputSignal : 0.0f;
        const float out3 = inputSignal >= threshold3 ? inputSignal : 0.0f;
        const float out4 = inputSignal >= threshold4 ? inputSignal : 0.0f;
        
        outBus.setSample(0, i, out1);
        outBus.setSample(1, i, out2);
        outBus.setSample(2, i, out3);
        outBus.setSample(3, i, out4);

#if defined(PRESET_CREATOR_UI)
        // Capture output audio for visualization (after processing)
        const int writeIdx = (vizWritePos + i) % vizBufferSize;
        if (vizOutput1Buffer.getNumSamples() > 0)
            vizOutput1Buffer.setSample(0, writeIdx, out1);
        if (vizOutput2Buffer.getNumSamples() > 0)
            vizOutput2Buffer.setSample(0, writeIdx, out2);
        if (vizOutput3Buffer.getNumSamples() > 0)
            vizOutput3Buffer.setSample(0, writeIdx, out3);
        if (vizOutput4Buffer.getNumSamples() > 0)
            vizOutput4Buffer.setSample(0, writeIdx, out4);

        // Track current state (use last sample for live display)
        if (i == numSamples - 1)
        {
            vizData.output1Active.store(out1 > 0.0f);
            vizData.output2Active.store(out2 > 0.0f);
            vizData.output3Active.store(out3 > 0.0f);
            vizData.output4Active.store(out4 > 0.0f);
        }
#endif

        // Update live values periodically for UI
        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("threshold1_live", threshold1);
            setLiveParamValue("threshold2_live", threshold2);
            setLiveParamValue("threshold3_live", threshold3);
            setLiveParamValue("threshold4_live", threshold4);
        }
    }

#if defined(PRESET_CREATOR_UI)
    vizWritePos = (vizWritePos + numSamples) % vizBufferSize;

    // Update visualization data (thread-safe)
    // Downsample waveforms from circular buffers
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx = (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) % vizBufferSize;
        if (vizInputBuffer.getNumSamples() > 0)
            vizData.inputWaveform[i].store(vizInputBuffer.getSample(0, readIdx));
        if (vizOutput1Buffer.getNumSamples() > 0)
            vizData.output1Waveform[i].store(vizOutput1Buffer.getSample(0, readIdx));
        if (vizOutput2Buffer.getNumSamples() > 0)
            vizData.output2Waveform[i].store(vizOutput2Buffer.getSample(0, readIdx));
        if (vizOutput3Buffer.getNumSamples() > 0)
            vizData.output3Waveform[i].store(vizOutput3Buffer.getSample(0, readIdx));
        if (vizOutput4Buffer.getNumSamples() > 0)
            vizData.output4Waveform[i].store(vizOutput4Buffer.getSample(0, readIdx));
    }

    // Update current thresholds (use last sample values)
    if (numSamples > 0)
    {
        float lastThresh1 = baseThreshold1;
        float lastThresh2 = baseThreshold2;
        float lastThresh3 = baseThreshold3;
        float lastThresh4 = baseThreshold4;
        
        if (isThresh1Mod && thresh1CV)
            lastThresh1 = juce::jlimit(0.0f, 1.0f, thresh1CV[numSamples - 1]);
        if (isThresh2Mod && thresh2CV)
            lastThresh2 = juce::jlimit(0.0f, 1.0f, thresh2CV[numSamples - 1]);
        if (isThresh3Mod && thresh3CV)
            lastThresh3 = juce::jlimit(0.0f, 1.0f, thresh3CV[numSamples - 1]);
        if (isThresh4Mod && thresh4CV)
            lastThresh4 = juce::jlimit(0.0f, 1.0f, thresh4CV[numSamples - 1]);
        
        vizData.currentThreshold1.store(lastThresh1);
        vizData.currentThreshold2.store(lastThresh2);
        vizData.currentThreshold3.store(lastThresh3);
        vizData.currentThreshold4.store(lastThresh4);
    }
#endif
}

#if defined(PRESET_CREATOR_UI)
void SequentialSwitchModuleProcessor::drawParametersInNode(
    float itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>& onModificationEnded,
    const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushItemWidth(itemWidth);

    // === SECTION: Sequential Switch Visualization ===
    ThemeText("Switch Activity", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushID(this); // Unique ID for this node's UI
    
    // Read visualization data (thread-safe) - BEFORE BeginChild
    float inputWaveform[VizData::waveformPoints];
    float output1Waveform[VizData::waveformPoints];
    float output2Waveform[VizData::waveformPoints];
    float output3Waveform[VizData::waveformPoints];
    float output4Waveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWaveform[i] = vizData.inputWaveform[i].load();
        output1Waveform[i] = vizData.output1Waveform[i].load();
        output2Waveform[i] = vizData.output2Waveform[i].load();
        output3Waveform[i] = vizData.output3Waveform[i].load();
        output4Waveform[i] = vizData.output4Waveform[i].load();
    }
    const float currentThreshold1 = vizData.currentThreshold1.load();
    const float currentThreshold2 = vizData.currentThreshold2.load();
    const float currentThreshold3 = vizData.currentThreshold3.load();
    const float currentThreshold4 = vizData.currentThreshold4.load();
    const bool output1Active = vizData.output1Active.load();
    const bool output2Active = vizData.output2Active.load();
    const bool output3Active = vizData.output3Active.load();
    const bool output4Active = vizData.output4Active.load();

    // Waveform visualization in child window
    const float waveHeight = 180.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("SequentialSwitchViz", graphSize, false, childFlags))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);
        
        // Background
        const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        
        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);
        
        const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);  // Cyan
        const ImU32 output1Color = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);      // Orange/Yellow
        const ImU32 output2Color = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);   // Magenta/Pink
        const ImU32 output3Color = ImGui::ColorConvertFloat4ToU32(theme.modulation.filter);     // Green
        const ImU32 output4Color = ImGui::ColorConvertFloat4ToU32(theme.accent);                 // Accent
        const ImU32 thresholdLineColor = IM_COL32(255, 255, 255, 150);
        const ImU32 centerLineColor = IM_COL32(150, 150, 150, 100);

        // Divide visualization into sections: Input (top), Outputs (bottom)
        const float midY = p0.y + graphSize.y * 0.5f;
        const float inputSectionHeight = graphSize.y * 0.35f;
        const float outputSectionHeight = graphSize.y * 0.6f;
        const float inputTopY = p0.y + 8.0f;
        const float inputBottomY = inputTopY + inputSectionHeight;
        const float outputTopY = midY + 4.0f;
        const float outputBottomY = outputTopY + outputSectionHeight;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

    // Draw center separator line
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), centerLineColor, 1.5f);

    // Draw threshold lines (in input section)
    auto drawThresholdLine = [&](float threshold, ImU32 color, const char* label)
    {
        const float thresholdY = inputTopY + (1.0f - threshold) * inputSectionHeight;
        const float clampedY = juce::jlimit(inputTopY + 2.0f, inputBottomY - 2.0f, thresholdY);
            drawList->AddLine(ImVec2(p0.x, clampedY), ImVec2(p1.x, clampedY), color, 1.5f);
            drawList->AddText(ImVec2(p0.x + 4.0f, clampedY - 12.0f), color, label);
    };

    drawThresholdLine(currentThreshold1, output1Color, "T1");
    drawThresholdLine(currentThreshold2, output2Color, "T2");
    drawThresholdLine(currentThreshold3, output3Color, "T3");
    drawThresholdLine(currentThreshold4, output4Color, "T4");

    // Draw input waveform (in input section, behind threshold lines)
        float prevX = p0.x;
    float prevY = inputBottomY;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const float sample = juce::jlimit(0.0f, 1.0f, inputWaveform[i]);
            const float x = p0.x + i * stepX;
        const float y = inputBottomY - sample * inputSectionHeight;
        if (i > 0)
        {
            ImVec4 colorVec4 = ImGui::ColorConvertU32ToFloat4(inputColor);
            colorVec4.w = 0.5f; // More transparent for background
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(colorVec4), 2.0f);
        }
        prevX = x;
        prevY = y;
    }

    // Draw output waveforms (in output section, stacked)
    const float outputRowHeight = outputSectionHeight / 4.0f;
    auto drawOutputWaveform = [&](const float* waveform, ImU32 color, float topY, float bottomY, float alpha)
    {
            float prevX = p0.x;
        float prevY = bottomY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(0.0f, 1.0f, waveform[i]);
                const float x = p0.x + i * stepX;
            const float y = bottomY - sample * (bottomY - topY);
            if (i > 0)
            {
                ImVec4 colorVec4 = ImGui::ColorConvertU32ToFloat4(color);
                colorVec4.w = alpha;
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(colorVec4), 2.5f);
            }
            prevX = x;
            prevY = y;
        }
    };

    drawOutputWaveform(output1Waveform, output1Color, outputTopY, outputTopY + outputRowHeight, 0.8f);
    drawOutputWaveform(output2Waveform, output2Color, outputTopY + outputRowHeight, outputTopY + outputRowHeight * 2.0f, 0.8f);
    drawOutputWaveform(output3Waveform, output3Color, outputTopY + outputRowHeight * 2.0f, outputTopY + outputRowHeight * 3.0f, 0.8f);
    drawOutputWaveform(output4Waveform, output4Color, outputTopY + outputRowHeight * 3.0f, outputBottomY, 0.8f);

    // Add labels for outputs
    const char* outputLabels[] = {"Out 1", "Out 2", "Out 3", "Out 4"};
    ImU32 outputColors[] = {output1Color, output2Color, output3Color, output4Color};
    for (int i = 0; i < 4; ++i)
    {
        const float labelY = outputTopY + outputRowHeight * i + outputRowHeight * 0.5f - 8.0f;
            drawList->AddText(ImVec2(p0.x + 4.0f, labelY), outputColors[i], outputLabels[i]);
    }

        drawList->PopClipRect();

        // Current state indicators overlay
        ImGui::SetCursorPos(ImVec2(4, waveHeight + 6));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "Active Outputs:");
        ImGui::SameLine();
        
        auto drawStateLED = [&](const char* label, bool state, ImU32 activeColor)
        {
            const ImVec2 pos = ImGui::GetCursorPos();
            const float radius = 5.0f;
            const ImU32 ledColor = state ? activeColor : IM_COL32(60, 60, 60, 200);
            drawList->AddCircleFilled(ImVec2(p0.x + pos.x + radius, p0.y + pos.y + radius), radius, ledColor, 16);
            ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        };

        drawStateLED("Out 1", output1Active, output1Color);
        ImGui::SameLine();
        drawStateLED("Out 2", output2Active, output2Color);
        ImGui::SameLine();
        drawStateLED("Out 3", output3Active, output3Color);
        ImGui::SameLine();
        drawStateLED("Out 4", output4Active, output4Color);

        ImGui::SetCursorPos(ImVec2(4, waveHeight + 28));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "Thresholds: T1=%.3f  T2=%.3f  T3=%.3f  T4=%.3f",
                    currentThreshold1, currentThreshold2, currentThreshold3, currentThreshold4);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##sequentialSwitchVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::PopID(); // End unique ID

    ImGui::Spacing();
    ImGui::Spacing();

    // Threshold 1
    {
        const bool thresh1IsMod = isParamModulated(paramIdThreshold1Mod);
        float thresh1 = thresh1IsMod ? getLiveParamValueFor(paramIdThreshold1Mod, "threshold1_live", threshold1Param->load())
                                     : threshold1Param->load();

        // Inline pin for Thresh 1 CV (Channel 1)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(1))
                ImGui::SameLine();
        }

        if (thresh1IsMod) ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Threshold 1", &thresh1, 0.0f, 1.0f, "%.3f"))
        {
            if (!thresh1IsMod)
                if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramIdThreshold1)))
                    *param = thresh1;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && onModificationEnded)
            onModificationEnded();
        if (thresh1IsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    }

    // Threshold 2
    {
        const bool thresh2IsMod = isParamModulated(paramIdThreshold2Mod);
        float thresh2 = thresh2IsMod ? getLiveParamValueFor(paramIdThreshold2Mod, "threshold2_live", threshold2Param->load())
                                     : threshold2Param->load();

        // Inline pin for Thresh 2 CV (Channel 2)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(2))
                ImGui::SameLine();
        }

        if (thresh2IsMod) ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Threshold 2", &thresh2, 0.0f, 1.0f, "%.3f"))
        {
            if (!thresh2IsMod)
                if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramIdThreshold2)))
                    *param = thresh2;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && onModificationEnded)
            onModificationEnded();
        if (thresh2IsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    }

    // Threshold 3
    {
        const bool thresh3IsMod = isParamModulated(paramIdThreshold3Mod);
        float thresh3 = thresh3IsMod ? getLiveParamValueFor(paramIdThreshold3Mod, "threshold3_live", threshold3Param->load())
                                     : threshold3Param->load();

        // Inline pin for Thresh 3 CV (Channel 3)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(3))
                ImGui::SameLine();
        }

        if (thresh3IsMod) ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Threshold 3", &thresh3, 0.0f, 1.0f, "%.3f"))
        {
            if (!thresh3IsMod)
                if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramIdThreshold3)))
                    *param = thresh3;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && onModificationEnded)
            onModificationEnded();
        if (thresh3IsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    }

    // Threshold 4
    {
        const bool thresh4IsMod = isParamModulated(paramIdThreshold4Mod);
        float thresh4 = thresh4IsMod ? getLiveParamValueFor(paramIdThreshold4Mod, "threshold4_live", threshold4Param->load())
                                     : threshold4Param->load();

        // Inline pin for Thresh 4 CV (Channel 4)
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(4))
                ImGui::SameLine();
        }

        if (thresh4IsMod) ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Threshold 4", &thresh4, 0.0f, 1.0f, "%.3f"))
        {
            if (!thresh4IsMod)
                if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(paramIdThreshold4)))
                    *param = thresh4;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && onModificationEnded)
            onModificationEnded();
        if (thresh4IsMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    }

    ImGui::PopItemWidth();
    ImGui::PopID(); // End module instance ID
}

void SequentialSwitchModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Draw input pins (left side)
    helpers.drawParallelPins("Gate In", 0, "Out 1", 0);
    helpers.drawParallelPins("Thresh 1 CV", 1, "Out 2", 1);
    helpers.drawParallelPins("Thresh 2 CV", 2, "Out 3", 2);
    helpers.drawParallelPins("Thresh 3 CV", 3, "Out 4", 3);
    helpers.drawParallelPins("Thresh 4 CV", 4, nullptr, -1);
}

juce::String SequentialSwitchModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Gate In";
        case 1: return "Thresh 1 CV";
        case 2: return "Thresh 2 CV";
        case 3: return "Thresh 3 CV";
        case 4: return "Thresh 4 CV";
        default: return "";
    }
}

juce::String SequentialSwitchModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Out 1";
        case 1: return "Out 2";
        case 2: return "Out 3";
        case 3: return "Out 4";
        default: return "";
    }
}
#endif

bool SequentialSwitchModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0;
    if (paramId == paramIdThreshold1Mod) { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdThreshold2Mod) { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdThreshold3Mod) { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdThreshold4Mod) { outChannelIndexInBus = 4; return true; }
    return false;
}
