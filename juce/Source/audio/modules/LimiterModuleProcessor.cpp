#include "LimiterModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout LimiterModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdThreshold, "Threshold", -20.0f, 0.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            paramIdRelease, "Release", 1.0f, 200.0f, 10.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 1.0f));

    // Relative modulation parameters
    params.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "relativeThresholdMod", "Relative Threshold Mod", true));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>(
            "relativeReleaseMod", "Relative Release Mod", true));

    return {params.begin(), params.end()};
}

LimiterModuleProcessor::LimiterModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Inputs",
                  juce::AudioChannelSet::discreteChannels(5),
                  true) // 0-1: Audio In, 2: Threshold Mod, 3: Release Mod, 4: Mix Mod
              .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "LimiterParams", createParameterLayout())
{
    thresholdParam = apvts.getRawParameterValue(paramIdThreshold);
    releaseParam = apvts.getRawParameterValue(paramIdRelease);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    relativeThresholdModParam = apvts.getRawParameterValue("relativeThresholdMod");
    relativeReleaseModParam = apvts.getRawParameterValue("relativeReleaseMod");

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R

    for (auto& v : vizData.inputHistory)
        v.store(-60.0f);
    for (auto& v : vizData.outputHistory)
        v.store(-60.0f);
}

void LimiterModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = 2;

    limiter.prepare(spec);
    limiter.reset();
    smoothedMix.reset(sampleRate, 0.01);
    dryBuffer.setSize(2, samplesPerBlock);

    vizHistoryIndex = 0;
    for (auto& v : vizData.inputHistory)
        v.store(-60.0f);
    for (auto& v : vizData.outputHistory)
        v.store(-60.0f);
    vizData.currentReduction.store(0.0f);
    vizData.currentThreshold.store(thresholdParam ? thresholdParam->load() : 0.0f);
    vizData.currentRelease.store(releaseParam ? releaseParam->load() : 10.0f);
    vizData.currentInputDb.store(-60.0f);
    vizData.currentOutputDb.store(-60.0f);
}

void LimiterModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // Get pointers to modulation CV inputs from unified input bus
    const bool isMixMod = isParamInputConnected(paramIdMixMod);
    auto       inBus = getBusBuffer(buffer, true, 0);
    auto       outBus = getBusBuffer(buffer, false, 0);

    // SAFE: Read input pointers BEFORE any output operations
    const float* mixCV = isMixMod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;

    // Get base parameter values ONCE
    const float baseMix = mixParam != nullptr ? mixParam->load() : 1.0f;

    // Store dry signal for mixing
    if (buffer.getNumSamples() > dryBuffer.getNumSamples())
        dryBuffer.setSize(2, buffer.getNumSamples(), false, false, true);
    dryBuffer.clear();
    for (int ch = 0; ch < juce::jmin(2, inBus.getNumChannels()); ++ch)
        dryBuffer.copyFrom(ch, 0, inBus, ch, 0, buffer.getNumSamples());

    // Copy input to output for in-place processing
    const int numInputChannels = inBus.getNumChannels();
    const int numOutputChannels = outBus.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numInputChannels > 0)
    {
        // If input is mono, copy it to both left and right outputs.
        if (numInputChannels == 1 && numOutputChannels > 1)
        {
            outBus.copyFrom(0, 0, inBus, 0, 0, numSamples);
            outBus.copyFrom(1, 0, inBus, 0, 0, numSamples);
        }
        // Otherwise, perform a standard stereo copy.
        else
        {
            const int channelsToCopy = juce::jmin(numInputChannels, numOutputChannels);
            for (int ch = 0; ch < channelsToCopy; ++ch)
            {
                outBus.copyFrom(ch, 0, inBus, ch, 0, numSamples);
            }
        }
    }
    else
    {
        // If no input is connected, ensure the output is silent.
        outBus.clear();
    }

    // --- Get base parameter values and relative modes ---
    const float baseThreshold = thresholdParam != nullptr ? thresholdParam->load() : 0.0f;
    const float baseRelease = releaseParam != nullptr ? releaseParam->load() : 10.0f;
    const bool  relativeThresholdMode =
        relativeThresholdModParam && relativeThresholdModParam->load() > 0.5f;
    const bool relativeReleaseMode =
        relativeReleaseModParam && relativeReleaseModParam->load() > 0.5f;

    // --- Update DSP Parameters from unified input bus (once per block) ---
    float finalThreshold = baseThreshold;
    if (isParamInputConnected(paramIdThresholdMod) && inBus.getNumChannels() > 2)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(2, 0));
        if (relativeThresholdMode)
        {
            // RELATIVE: ±10dB offset
            const float offset = (cv - 0.5f) * 20.0f;
            finalThreshold = baseThreshold + offset;
        }
        else
        {
            // ABSOLUTE: CV directly sets threshold
            finalThreshold = juce::jmap(cv, -20.0f, 0.0f);
        }
        finalThreshold = juce::jlimit(-20.0f, 0.0f, finalThreshold);
    }

    float finalRelease = baseRelease;
    if (isParamInputConnected(paramIdReleaseMod) && inBus.getNumChannels() > 3)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(3, 0));
        if (relativeReleaseMode)
        {
            // RELATIVE: 0.25x to 4x time scaling
            const float octaveOffset = (cv - 0.5f) * 4.0f;
            finalRelease = baseRelease * std::pow(2.0f, octaveOffset);
        }
        else
        {
            // ABSOLUTE: CV directly sets release
            finalRelease = juce::jmap(cv, 1.0f, 200.0f);
        }
        finalRelease = juce::jlimit(1.0f, 200.0f, finalRelease);
    }

    limiter.setThreshold(finalThreshold);
    limiter.setRelease(finalRelease);

    // --- Process the Audio ---
    // First, create a limited version for the wet signal
    juce::dsp::AudioBlock<float>              block(outBus);
    juce::dsp::ProcessContextReplacing<float> context(block);
    limiter.process(context);

    // Store the limited (wet) signal
    juce::AudioBuffer<float> wetBuffer(2, numSamples);
    for (int ch = 0; ch < juce::jmin(2, outBus.getNumChannels()); ++ch)
        wetBuffer.copyFrom(ch, 0, outBus, ch, 0, numSamples);

    // Apply dry/wet mix
    for (int i = 0; i < numSamples; ++i)
    {
        float currentMix = baseMix;
        if (isMixMod && mixCV != nullptr)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
            currentMix = cv;
        }
        smoothedMix.setTargetValue(currentMix);
        const float mix = smoothedMix.getNextValue();
        const float dry = 1.0f - mix;

        for (int ch = 0; ch < juce::jmin(2, outBus.getNumChannels()); ++ch)
        {
            const float wetSample = wetBuffer.getSample(ch, i);
            const float drySample = dryBuffer.getSample(ch, i);
            outBus.setSample(ch, i, dry * drySample + mix * wetSample);
        }
    }

    // SAFETY: Apply limiting to the final mixed output to ensure it never exceeds threshold
    // This guarantees safety even when mix < 1.0 (dry signal mixed in)
    juce::dsp::AudioBlock<float>              finalBlock(outBus);
    juce::dsp::ProcessContextReplacing<float> finalContext(finalBlock);
    limiter.process(finalContext);

    // FINAL SAFETY: Hard clip at 0dB as absolute safety net (should never trigger if limiter works
    // correctly)
    for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
    {
        auto* channelData = outBus.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            channelData[i] = juce::jlimit(-1.0f, 1.0f, channelData[i]);
        }
    }

    // Visualization capture (simple peak levels and gain reduction)
    float inputPeak = -60.0f;
    float outputPeak = -60.0f;
    for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
    {
        inputPeak = juce::jmax(inputPeak, inBus.getRMSLevel(ch, 0, numSamples));
        outputPeak = juce::jmax(outputPeak, outBus.getRMSLevel(ch, 0, numSamples));
    }
    auto        toDb = [](float v) { return juce::Decibels::gainToDecibels(v, -60.0f); };
    const float inputDb = toDb(inputPeak);
    const float outputDb = toDb(outputPeak);
    const float reduction = juce::jmax(0.0f, inputDb - outputDb);

    vizData.inputHistory[vizHistoryIndex].store(inputDb);
    vizData.outputHistory[vizHistoryIndex].store(outputDb);
    const float normalizedReduction = juce::jlimit(0.0f, 1.0f, reduction / 24.0f);
    vizData.reductionHistory[vizHistoryIndex].store(normalizedReduction);
    vizData.currentReduction.store(reduction);
    vizData.currentThreshold.store(finalThreshold);
    vizData.currentRelease.store(finalRelease);
    vizData.currentInputDb.store(inputDb);
    vizData.currentOutputDb.store(outputDb);
    vizHistoryIndex = (vizHistoryIndex + 1) % VizData::historyPoints;
    vizData.historyWriteIndex.store(vizHistoryIndex);

    // --- Update UI Telemetry & Tooltips ---
    setLiveParamValue("threshold_live", finalThreshold);
    setLiveParamValue("release_live", finalRelease);

    // Update live mix value for telemetry
    if (numSamples > 0)
    {
        float finalMix = baseMix;
        if (isMixMod && mixCV != nullptr)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[numSamples - 1]);
            finalMix = cv;
        }
        setLiveParamValue("mix_live", finalMix);
    }

    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1])
            lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
    }
}

bool LimiterModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus

    if (paramId == paramIdThresholdMod)
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdReleaseMod)
    {
        outChannelIndexInBus = 3;
        return true;
    }
    if (paramId == paramIdMixMod)
    {
        outChannelIndexInBus = 4;
        return true;
    }
    return false;
}

juce::String LimiterModuleProcessor::getAudioInputLabel(int channel) const
{
    if (channel == 0)
        return "In L";
    if (channel == 1)
        return "In R";
    if (channel == 2)
        return "Thresh Mod";
    if (channel == 3)
        return "Release Mod";
    if (channel == 4)
        return "Mix Mod";
    return {};
}

juce::String LimiterModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0)
        return "Out L";
    if (channel == 1)
        return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void LimiterModuleProcessor::drawParametersInNode(
    float                                           itemWidth,
    const std::function<bool(const juce::String&)>& isParamModulated,
    const std::function<void()>&                    onModificationEnded,
    const NodePinHelpers*                           pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto&       ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    auto HelpMarker = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    auto drawSlider = [&](const char*         label,
                          const juce::String& paramId,
                          const juce::String& modId,
                          float               min,
                          float               max,
                          const char*         format,
                          const char*         tooltip,
                          int                 channel = -1) {
        bool  isMod = isParamModulated(modId);
        float value = isMod
                          ? getLiveParamValueFor(
                                modId, paramId + "_live", ap.getRawParameterValue(paramId)->load())
                          : ap.getRawParameterValue(paramId)->load();

        if (isMod)
            ImGui::BeginDisabled();

        // Draw inline pin if channel is specified and pinHelpers is available
        if (channel >= 0 && pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }

        if (ImGui::SliderFloat(label, &value, min, max, format))
            if (!isMod)
                *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramId)) = value;
        if (!isMod)
            adjustParamOnWheel(ap.getParameter(paramId), paramId, value);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            onModificationEnded();
        }
        if (isMod)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted("(mod)");
        }
        if (tooltip)
        {
            ImGui::SameLine();
            HelpMarker(tooltip);
        }
    };

    // Visualization section
    ImGui::Spacing();
    ImGui::Text("Limiter Activity");
    ImGui::Spacing();

    const int    writeIdx = vizData.historyWriteIndex.load();
    const ImU32  freqColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32  timbreColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
    const ImU32  accentColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
    const float  currentThresholdDb = vizData.currentThreshold.load();
    const float  currentReleaseMs = vizData.currentRelease.load();
    const float  currentReductionDb = vizData.currentReduction.load();
    const float  currentInputDb = vizData.currentInputDb.load();
    const float  currentOutputDb = vizData.currentOutputDb.load();
    const ImVec4 childBgVec4 =
        ImGui::ColorConvertU32ToFloat4(ThemeManager::getInstance().getCanvasBackground());

    auto drawHistoryChild =
        [&](const char*                                                                  childId,
            float                                                                        height,
            const std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float)>& drawFn) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, childBgVec4);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
            if (ImGui::BeginChild(
                    childId,
                    ImVec2(itemWidth, height),
                    false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                auto*        childDrawList = ImGui::GetWindowDrawList();
                const ImVec2 childPos = ImGui::GetWindowPos();
                const ImVec2 childSize = ImGui::GetWindowSize();
                const ImVec2 pad = ImGui::GetStyle().WindowPadding;
                const ImVec2 contentOrigin(childPos.x + pad.x, childPos.y + pad.y);
                const ImVec2 contentMax(
                    childPos.x + childSize.x - pad.x, childPos.y + childSize.y - pad.y);
                const float contentWidth = contentMax.x - contentOrigin.x;
                drawFn(childDrawList, contentOrigin, contentMax, contentWidth);
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };

    drawHistoryChild(
        "LimiterLevelHistory",
        70.0f,
        [&](ImDrawList* dl, const ImVec2& origin, const ImVec2& max, float width) {
            const float plotHeight = max.y - origin.y;
            const float stepX = width / (float)(VizData::historyPoints - 1);

            auto drawLine =
                [&](const std::array<std::atomic<float>, VizData::historyPoints>& history,
                    ImU32                                                         color) {
                    float prevX = origin.x;
                    float prevY = max.y;
                    for (int i = 0; i < VizData::historyPoints; ++i)
                    {
                        const int   idx = (writeIdx + i) % VizData::historyPoints;
                        const float val = juce::jlimit(-60.0f, 0.0f, history[idx].load());
                        const float normalized = juce::jmap(val, -60.0f, 0.0f, 0.0f, 1.0f);
                        const float x = origin.x + i * stepX;
                        const float y = max.y - normalized * (plotHeight - 4.0f) - 2.0f;
                        if (i > 0)
                            dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), color, 2.0f);
                        prevX = x;
                        prevY = y;
                    }
                };

            drawLine(vizData.inputHistory, freqColor);
            drawLine(vizData.outputHistory, timbreColor);

            // Threshold line overlay (shows slider feedback even without signal)
            const float thresholdNorm = juce::jmap(currentThresholdDb, -60.0f, 0.0f, 0.0f, 1.0f);
            const float threshY =
                juce::jlimit(origin.y, max.y, max.y - thresholdNorm * (plotHeight - 4.0f) - 2.0f);
            const ImU32 threshColor = IM_COL32(255, 255, 255, 120);
            dl->AddLine(ImVec2(origin.x, threshY), ImVec2(max.x, threshY), threshColor, 1.5f);

            const juce::String threshLabel = juce::String(currentThresholdDb, 1) + " dB";
            dl->AddText(
                ImVec2(origin.x + 4.0f, threshY - ImGui::GetTextLineHeight()),
                threshColor,
                threshLabel.toRawUTF8());
        });

    ImGui::Spacing();

    drawHistoryChild(
        "LimiterReductionHistory",
        55.0f,
        [&](ImDrawList* dl, const ImVec2& origin, const ImVec2& max, float width) {
            const float plotHeight = max.y - origin.y;
            const float stepX = width / (float)(VizData::historyPoints - 1);
            float       prevX = origin.x;
            float       prevY = max.y - 2.0f;
            for (int i = 0; i < VizData::historyPoints; ++i)
            {
                const int   idx = (writeIdx + i) % VizData::historyPoints;
                const float val = juce::jlimit(0.0f, 1.0f, vizData.reductionHistory[idx].load());
                const float x = origin.x + i * stepX;
                const float y = max.y - val * (plotHeight - 4.0f) - 2.0f;
                if (i > 0)
                    dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), accentColor, 2.0f);
                prevX = x;
                prevY = y;
            }

            const juce::String text =
                juce::String("Reduction: ") + juce::String(currentReductionDb, 1) + " dB";
            dl->AddText(
                ImVec2(origin.x + 2.0f, origin.y + 2.0f),
                IM_COL32(220, 220, 220, 255),
                text.toRawUTF8());

            const juce::String releaseText =
                juce::String("Release: ") + juce::String(currentReleaseMs, 0) + " ms";
            dl->AddText(
                ImVec2(origin.x + 2.0f, origin.y + 18.0f),
                IM_COL32(200, 200, 200, 200),
                releaseText.toRawUTF8());
        });

    ImGui::Spacing();

    auto levelToNorm = [](float db) -> float {
        return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    };

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, freqColor);
    ImGui::Text("Input Level");
    ImGui::ProgressBar(
        levelToNorm(currentInputDb),
        ImVec2(itemWidth * 0.5f, 0),
        juce::String(currentInputDb, 1).toRawUTF8());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, timbreColor);
    ImGui::Text("Output Level");
    ImGui::ProgressBar(
        levelToNorm(currentOutputDb),
        ImVec2(itemWidth * 0.5f, 0),
        juce::String(currentOutputDb, 1).toRawUTF8());
    ImGui::PopStyleColor();

    ImGui::Spacing();

    ThemeText("Limiter Parameters", theme.text.section_header);
    ImGui::Spacing();

    drawSlider(
        "Threshold",
        paramIdThreshold,
        paramIdThresholdMod,
        -20.0f,
        0.0f,
        "%.1f dB",
        "Maximum output level (-20 to 0 dB)\nSignal peaks above this are limited",
        2); // Channel 2 = Thresh Mod
    drawSlider(
        "Release",
        paramIdRelease,
        paramIdReleaseMod,
        1.0f,
        200.0f,
        "%.0f ms",
        "Release time (1-200 ms)\nHow fast the limiter recovers",
        3); // Channel 3 = Release Mod
    drawSlider(
        "Mix",
        paramIdMix,
        paramIdMixMod,
        0.0f,
        1.0f,
        "%.2f",
        "Wet/dry balance (0=dry, 1=wet)",
        4); // Channel 4 = Mix Mod

    ImGui::Spacing();
    ImGui::Spacing();

    // === RELATIVE MODULATION SECTION ===
    ThemeText("CV Input Modes", theme.modulation.frequency);
    ImGui::Spacing();

    // Relative Threshold Mod checkbox
    bool relativeThresholdMod =
        relativeThresholdModParam != nullptr && relativeThresholdModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Threshold Mod", &relativeThresholdMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeThresholdMod")))
            *p = relativeThresholdMod;
        juce::Logger::writeToLog(
            "[Limiter UI] Relative Threshold Mod: " +
            juce::String(relativeThresholdMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±10dB)\nOFF: CV directly sets threshold (-20dB to "
            "0dB)");
    }

    // Relative Release Mod checkbox
    bool relativeReleaseMod =
        relativeReleaseModParam != nullptr && relativeReleaseModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Release Mod", &relativeReleaseMod))
    {
        if (auto* p =
                dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeReleaseMod")))
            *p = relativeReleaseMod;
        juce::Logger::writeToLog(
            "[Limiter UI] Relative Release Mod: " +
            juce::String(relativeReleaseMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (0.25x to 4x)\nOFF: CV directly sets release "
            "(1-200ms)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void LimiterModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-4) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Thresh Mod (ch 2), Release Mod (ch 3), Mix Mod (ch 4) are drawn inline in
    // drawParametersInNode
}
#endif

std::vector<DynamicPinInfo> LimiterModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});

    // Modulation inputs (channels 2-4)
    pins.push_back({"Thresh Mod", 2, PinDataType::CV});
    pins.push_back({"Release Mod", 3, PinDataType::CV});
    pins.push_back({"Mix Mod", 4, PinDataType::CV});

    return pins;
}

std::vector<DynamicPinInfo> LimiterModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});

    return pins;
}
