#include "LagProcessorModuleProcessor.h"

LagProcessorModuleProcessor::LagProcessorModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Signal In", juce::AudioChannelSet::mono(), true)
              .withInput("Rise Mod", juce::AudioChannelSet::mono(), true)
              .withInput("Fall Mod", juce::AudioChannelSet::mono(), true)
              .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "LagProcessorParams", createParameterLayout())
{
    riseTimeParam = apvts.getRawParameterValue("rise_time");
    fallTimeParam = apvts.getRawParameterValue("fall_time");
    modeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("mode"));

    // Initialize output value tracking for tooltips
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout LagProcessorModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Rise time: 0.1ms to 4000ms (logarithmic)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rise_time",
            "Rise Time",
            juce::NormalisableRange<float>(0.1f, 4000.0f, 0.0f, 0.3f),
            10.0f));

    // Fall time: 0.1ms to 4000ms (logarithmic)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "fall_time",
            "Fall Time",
            juce::NormalisableRange<float>(0.1f, 4000.0f, 0.0f, 0.3f),
            10.0f));

    // Mode: Slew Limiter or Envelope Follower
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "mode", "Mode", juce::StringArray{"Slew Limiter", "Envelope Follower"}, 0));

    return {params.begin(), params.end()};
}

void LagProcessorModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;
    currentOutput = 0.0f;
#if defined(PRESET_CREATOR_UI)
    vizInputBuffer.setSize(1, samplesPerBlock);
    vizOutputBuffer.setSize(1, samplesPerBlock);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
#endif
}

void LagProcessorModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    juce::ignoreUnused(midi);

    auto signalIn = getBusBuffer(buffer, true, 0);
    auto riseModIn = getBusBuffer(buffer, true, 1);
    auto fallModIn = getBusBuffer(buffer, true, 2);
    auto out = getBusBuffer(buffer, false, 0);

    const int nSamps = buffer.getNumSamples();

    // Get base parameter values
    float     baseRiseMs = riseTimeParam != nullptr ? riseTimeParam->load() : 10.0f;
    float     baseFallMs = fallTimeParam != nullptr ? fallTimeParam->load() : 10.0f;
    const int mode = modeParam != nullptr ? modeParam->getIndex() : 0;

    // Check for modulation
    const bool isRiseModulated = isParamInputConnected("rise_time_mod");
    const bool isFallModulated = isParamInputConnected("fall_time_mod");

    const float* riseModSignal = isRiseModulated ? riseModIn.getReadPointer(0) : nullptr;
    const float* fallModSignal = isFallModulated ? fallModIn.getReadPointer(0) : nullptr;

    const float* input = signalIn.getReadPointer(0);
    float*       output = out.getWritePointer(0);

    // Track last computed rise/fall times for visualization
    float lastRiseMs = baseRiseMs;
    float lastFallMs = baseFallMs;

    for (int i = 0; i < nSamps; ++i)
    {
        // Get modulated rise/fall times if connected
        float riseMs = baseRiseMs;
        float fallMs = baseFallMs;

        if (isRiseModulated && riseModSignal)
        {
            // Map CV (0..1) to time range (0.1..4000ms) logarithmically
            riseMs = 0.1f * std::pow(40000.0f, riseModSignal[i]);
            riseMs = juce::jlimit(0.1f, 4000.0f, riseMs);
        }

        if (isFallModulated && fallModSignal)
        {
            fallMs = 0.1f * std::pow(40000.0f, fallModSignal[i]);
            fallMs = juce::jlimit(0.1f, 4000.0f, fallMs);
        }

        // Track for visualization
        lastRiseMs = riseMs;
        lastFallMs = fallMs;

        // Update telemetry (throttled)
        if ((i & 0x3F) == 0)
        {
            setLiveParamValue("rise_time_live", riseMs);
            setLiveParamValue("fall_time_live", fallMs);
        }

        // Calculate smoothing coefficients
        // Formula: coeff = 1.0 - exp(-1.0 / (time_in_seconds * sampleRate))
        float riseCoeff =
            1.0f - std::exp(-1.0f / (riseMs * 0.001f * static_cast<float>(currentSampleRate)));
        float fallCoeff =
            1.0f - std::exp(-1.0f / (fallMs * 0.001f * static_cast<float>(currentSampleRate)));

        // Get target value based on mode
        float inputSample = input[i];
        float targetValue = inputSample;

        if (mode == 1) // Envelope Follower
        {
            // Rectify the signal to extract amplitude envelope
            targetValue = std::abs(inputSample);
        }

        // Apply smoothing
        if (targetValue > currentOutput) // Rising
        {
            currentOutput += (targetValue - currentOutput) * riseCoeff;
        }
        else // Falling
        {
            currentOutput += (targetValue - currentOutput) * fallCoeff;
        }

        output[i] = currentOutput;
    }

    // Update output values for tooltips
    if (lastOutputValues.size() >= 1 && lastOutputValues[0])
    {
        lastOutputValues[0]->store(currentOutput);
    }

#if defined(PRESET_CREATOR_UI)
    // Capture waveforms for visualization
    if (nSamps > 0)
    {
        vizInputBuffer.makeCopyOf(signalIn);
        vizOutputBuffer.makeCopyOf(out);

        auto captureWaveform = [&](const juce::AudioBuffer<float>&                          source,
                                   std::array<std::atomic<float>, VizData::waveformPoints>& dest) {
            const int samples = juce::jmin(source.getNumSamples(), nSamps);
            if (samples <= 0)
                return;
            const int stride = juce::jmax(1, samples / VizData::waveformPoints);
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const int idx = juce::jmin(samples - 1, i * stride);
                float     value = source.getSample(0, idx);
                dest[i].store(juce::jlimit(-1.0f, 1.0f, value));
            }
        };

        captureWaveform(vizInputBuffer, vizData.inputWaveform);
        captureWaveform(vizOutputBuffer, vizData.outputWaveform);

        // Capture target waveform (for envelope follower mode)
        if (mode == 1)
        {
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const int idx = juce::jmin(nSamps - 1, (i * nSamps) / VizData::waveformPoints);
                float     targetVal = std::abs(input[idx]);
                vizData.targetWaveform[i].store(juce::jlimit(0.0f, 1.0f, targetVal));
            }
        }
        else
        {
            // In slew limiter mode, target is same as input
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                vizData.targetWaveform[i].store(vizData.inputWaveform[i].load());
            }
        }

        // Update live parameter values
        vizData.currentRiseMs.store(lastRiseMs);
        vizData.currentFallMs.store(lastFallMs);
        vizData.currentMode.store(mode);
    }
#endif
}

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"

void LagProcessorModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto&       ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    // Get current parameter values
    float riseMs = riseTimeParam != nullptr ? riseTimeParam->load() : 10.0f;
    float fallMs = fallTimeParam != nullptr ? fallTimeParam->load() : 10.0f;
    int   modeIdx = modeParam != nullptr ? modeParam->getIndex() : 0;

    // Check for modulation
    bool isRiseModulated = isParamModulated("rise_time_mod");
    bool isFallModulated = isParamModulated("fall_time_mod");

    if (isRiseModulated)
    {
        riseMs = getLiveParamValueFor("rise_time_mod", "rise_time_live", riseMs);
    }
    if (isFallModulated)
    {
        fallMs = getLiveParamValueFor("fall_time_mod", "fall_time_live", fallMs);
    }

    // Draw visualization
    ImGui::Spacing();
    ImGui::Text("Lag Visualizer");
    ImGui::Spacing();

    // Read waveform data from atomics - BEFORE BeginChild
    float inputWave[VizData::waveformPoints];
    float outputWave[VizData::waveformPoints];
    float targetWave[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWave[i] = vizData.inputWaveform[i].load();
        outputWave[i] = vizData.outputWaveform[i].load();
        targetWave[i] = vizData.targetWaveform[i].load();
    }
    const float liveRiseMs = vizData.currentRiseMs.load();
    const float liveFallMs = vizData.currentFallMs.load();

    // Waveform visualization in child window
    const float            waveHeight = 110.0f;
    const ImVec2           graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("LagViz", graphSize, false, childFlags))
    {
        ImDrawList*  drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);

        // Background
        const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);

        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);

        const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
        const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
        const ImU32 targetColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);

        // Draw output waveforms
        const float midY = p0.y + graphSize.y * 0.5f;
        const float scaleY = graphSize.y * 0.4f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        auto drawWave = [&](float* data, ImU32 color, float thickness) {
            float px = p0.x;
            float py = midY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float x = p0.x + i * stepX;
                const float clampedY =
                    juce::jlimit(p0.y, p1.y, midY - juce::jlimit(-1.0f, 1.0f, data[i]) * scaleY);
                const float y = clampedY;
                if (i > 0)
                    drawList->AddLine(ImVec2(px, py), ImVec2(x, y), color, thickness);
                px = x;
                py = y;
            }
        };

        // Draw waveforms
        if (modeIdx == 1) // Envelope Follower mode
        {
            // Show target (rectified input) as a filled area
            for (int i = 0; i < VizData::waveformPoints - 1; ++i)
            {
                const float x1 = p0.x + i * stepX;
                const float x2 = p0.x + (i + 1) * stepX;
                const float y1 = juce::jlimit(p0.y, p1.y, midY - targetWave[i] * scaleY);
                const float y2 = juce::jlimit(p0.y, p1.y, midY - targetWave[i + 1] * scaleY);
                const float yBase = juce::jlimit(p0.y, p1.y, midY);
                drawList->AddQuadFilled(
                    ImVec2(x1, yBase),
                    ImVec2(x2, yBase),
                    ImVec2(x2, y2),
                    ImVec2(x1, y1),
                    (targetColor & 0x00FFFFFF) | 0x40000000); // Semi-transparent
            }
            drawWave(targetWave, targetColor, 1.5f);
        }

        drawWave(inputWave, inputColor, 1.2f);
        drawWave(outputWave, outputColor, 2.0f);

        drawList->PopClipRect();

        // Live parameter values overlay
        ImGui::SetCursorPos(ImVec2(4, waveHeight + 4));
        ImGui::TextColored(
            ImVec4(1.0f, 1.0f, 1.0f, 0.9f),
            "Rise: %.2f ms  |  Fall: %.2f ms",
            liveRiseMs,
            liveFallMs);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##lagVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ThemeText("Lag Parameters", theme.text.section_header);
    ImGui::Spacing();

    // Mode selector
    const char* modeNames[] = {"Slew Limiter", "Envelope Follower"};
    if (ImGui::Combo("Mode", &modeIdx, modeNames, 2))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("mode")))
        {
            *p = modeIdx;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }

    // Rise Time (or Attack in Envelope Follower mode)
    const char* riseLabel = (modeIdx == 0) ? "Rise Time (ms)" : "Attack (ms)";
    if (isRiseModulated)
        ImGui::BeginDisabled();
    // Draw inline pin for Rise Mod (channel 1)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1)) // Channel 1 = Rise Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat(riseLabel, &riseMs, 0.1f, 4000.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
    {
        if (!isRiseModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("rise_time")))
                *p = riseMs;
        }
    }
    if (!isRiseModulated)
        adjustParamOnWheel(ap.getParameter("rise_time"), "rise_time", riseMs);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isRiseModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    // Fall Time (or Release in Envelope Follower mode)
    const char* fallLabel = (modeIdx == 0) ? "Fall Time (ms)" : "Release (ms)";
    if (isFallModulated)
        ImGui::BeginDisabled();
    // Draw inline pin for Fall Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2)) // Channel 2 = Fall Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat(fallLabel, &fallMs, 0.1f, 4000.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
    {
        if (!isFallModulated)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("fall_time")))
                *p = fallMs;
        }
    }
    if (!isFallModulated)
        adjustParamOnWheel(ap.getParameter("fall_time"), "fall_time", fallMs);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isFallModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
}

bool LagProcessorModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    if (paramId == "rise_time_mod")
    {
        outBusIndex = 1; // "Rise Mod" is Bus 1
        outChannelIndexInBus = 0;
        return true;
    }
    else if (paramId == "fall_time_mod")
    {
        outBusIndex = 2; // "Fall Mod" is Bus 2
        outChannelIndexInBus = 0;
        return true;
    }
    return false;
}
#endif
