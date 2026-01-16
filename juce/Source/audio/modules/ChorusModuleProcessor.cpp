#include "ChorusModuleProcessor.h"
#include <cmath>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout ChorusModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdRate, "Rate", 0.05f, 5.0f, 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdDepth, "Depth", 0.0f, 1.0f, 0.25f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 0.5f));

    // Relative modulation parameters
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeRateMod", "Relative Rate Mod", true));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeDepthMod", "Relative Depth Mod", true));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeMixMod", "Relative Mix Mod", true));

    return {params.begin(), params.end()};
}

ChorusModuleProcessor::ChorusModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Inputs",
                  juce::AudioChannelSet::discreteChannels(5),
                  true) // 0-1: Audio In, 2: Rate Mod, 3: Depth Mod, 4: Mix Mod
              .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "ChorusParams", createParameterLayout())
{
    rateParam = apvts.getRawParameterValue(paramIdRate);
    depthParam = apvts.getRawParameterValue(paramIdDepth);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    relativeRateModParam = apvts.getRawParameterValue("relativeRateMod");
    relativeDepthModParam = apvts.getRawParameterValue("relativeDepthMod");
    relativeMixModParam = apvts.getRawParameterValue("relativeMixMod");

    // Initialize output value tracking for tooltips
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R

    // Initialize visualization data
    for (auto& v : vizData.inputWaveformL)
        v.store(0.0f);
    for (auto& v : vizData.inputWaveformR)
        v.store(0.0f);
    for (auto& v : vizData.outputWaveformL)
        v.store(0.0f);
    for (auto& v : vizData.outputWaveformR)
        v.store(0.0f);
    for (auto& v : vizData.lfoWaveform)
        v.store(0.0f);
}

void ChorusModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = 2; // Process in stereo

    chorus.prepare(spec);
    chorus.reset();

    // Visualization buffers
    vizInputBuffer.setSize(2, vizBufferSize);
    vizOutputBuffer.setSize(2, vizBufferSize);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
    vizLfoBuffer.assign(vizBufferSize, 0.0f);
    vizWritePos = 0;
    vizLfoPhase = 0.0f;
}

void ChorusModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // Get handles to the input and output buses
    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    // The Chorus DSP object works in-place, so we first copy the dry input to the output.
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

    // --- Get Modulation CVs from unified input bus ---
    const bool isRateMod = isParamInputConnected(paramIdRateMod);
    const bool isDepthMod = isParamInputConnected(paramIdDepthMod);
    const bool isMixMod = isParamInputConnected(paramIdMixMod);

    const float* rateCV =
        isRateMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* depthCV =
        isDepthMod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;
    const float* mixCV = isMixMod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;

    // --- Get Base Parameter Values ---
    const float baseRate = rateParam->load();
    const float baseDepth = depthParam->load();
    const float baseMix = mixParam->load();
    const bool  relativeRateMode = relativeRateModParam && relativeRateModParam->load() > 0.5f;
    const bool  relativeDepthMode = relativeDepthModParam && relativeDepthModParam->load() > 0.5f;
    const bool  relativeMixMode = relativeMixModParam && relativeMixModParam->load() > 0.5f;

    // Apply CV modulation with relative/absolute modes
    float finalRate = baseRate;
    if (isRateMod && rateCV)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, rateCV[0]);
        if (relativeRateMode)
        {
            // RELATIVE: Modulate ±2 octaves (0.25x to 4x)
            const float octaveOffset = (cv - 0.5f) * 4.0f; // ±2 octaves
            finalRate = baseRate * std::pow(2.0f, octaveOffset);
        }
        else
        {
            // ABSOLUTE: CV directly sets rate (0.05-5 Hz)
            finalRate = juce::jmap(cv, 0.05f, 5.0f);
        }
    }

    float finalDepth = baseDepth;
    if (isDepthMod && depthCV)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, depthCV[0]);
        if (relativeDepthMode)
        {
            // RELATIVE: CV adds offset to base depth (±0.5)
            const float offset = (cv - 0.5f) * 1.0f;
            finalDepth = baseDepth + offset;
        }
        else
        {
            // ABSOLUTE: CV directly sets depth
            finalDepth = cv;
        }
        finalDepth = juce::jlimit(0.0f, 1.0f, finalDepth);
    }

    float finalMix = baseMix;
    if (isMixMod && mixCV)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, mixCV[0]);
        if (relativeMixMode)
        {
            // RELATIVE: CV adds offset to base mix (±0.5)
            const float offset = (cv - 0.5f) * 1.0f;
            finalMix = baseMix + offset;
        }
        else
        {
            // ABSOLUTE: CV directly sets mix
            finalMix = cv;
        }
        finalMix = juce::jlimit(0.0f, 1.0f, finalMix);
    }

    // --- Update the DSP Object ---
    chorus.setRate(juce::jlimit(0.05f, 5.0f, finalRate));
    chorus.setDepth(juce::jlimit(0.0f, 1.0f, finalDepth));
    chorus.setMix(juce::jlimit(0.0f, 1.0f, finalMix));

    // Snapshot dry signal for visualization before processing
    juce::AudioBuffer<float> drySnapshot;
    drySnapshot.makeCopyOf(outBus);

    // --- Process the Audio ---
    juce::dsp::AudioBlock<float>              block(outBus);
    juce::dsp::ProcessContextReplacing<float> context(block);
    chorus.process(context);

    // --- Visualization capture ---
    const float sampleRate = static_cast<float>(getSampleRate());
    const float lfoDelta = sampleRate > 0.0f ? juce::MathConstants<float>::twoPi *
                                                   juce::jlimit(0.05f, 5.0f, finalRate) / sampleRate
                                             : 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float dryL = drySnapshot.getNumChannels() > 0 ? drySnapshot.getSample(0, i) : 0.0f;
        const float wetL = outBus.getNumChannels() > 0 ? outBus.getSample(0, i) : 0.0f;
        vizInputBuffer.setSample(0, vizWritePos, dryL);
        vizOutputBuffer.setSample(0, vizWritePos, wetL);

        if (vizInputBuffer.getNumChannels() > 1)
        {
            const float dryR =
                drySnapshot.getNumChannels() > 1 ? drySnapshot.getSample(1, i) : dryL;
            const float wetR = outBus.getNumChannels() > 1 ? outBus.getSample(1, i) : wetL;
            vizInputBuffer.setSample(1, vizWritePos, dryR);
            vizOutputBuffer.setSample(1, vizWritePos, wetR);
        }

        vizLfoPhase += lfoDelta;
        if (vizLfoPhase > juce::MathConstants<float>::twoPi)
            vizLfoPhase -= juce::MathConstants<float>::twoPi;
        const float lfoValue = std::sin(vizLfoPhase);
        if (vizLfoBuffer.size() == vizBufferSize)
            vizLfoBuffer[vizWritePos] = lfoValue;

        vizWritePos = (vizWritePos + 1) % vizBufferSize;

        if ((i & 0x3F) == 0)
        {
            vizData.currentRate.store(finalRate);
            vizData.currentDepth.store(finalDepth);
            vizData.currentMix.store(finalMix);

            const int step = juce::jmax(1, vizBufferSize / VizData::waveformPoints);
            for (int j = 0; j < VizData::waveformPoints; ++j)
            {
                const int idx =
                    (vizWritePos - (VizData::waveformPoints - j) * step + vizBufferSize) %
                    vizBufferSize;
                vizData.inputWaveformL[j].store(vizInputBuffer.getSample(0, idx));
                vizData.outputWaveformL[j].store(vizOutputBuffer.getSample(0, idx));
                if (vizInputBuffer.getNumChannels() > 1)
                {
                    vizData.inputWaveformR[j].store(vizInputBuffer.getSample(1, idx));
                    vizData.outputWaveformR[j].store(vizOutputBuffer.getSample(1, idx));
                }
            }

            const int lfoStep = juce::jmax(1, vizBufferSize / VizData::lfoPoints);
            for (int j = 0; j < VizData::lfoPoints; ++j)
            {
                const int idx = (vizWritePos - (VizData::lfoPoints - j) * lfoStep + vizBufferSize) %
                                vizBufferSize;
                const float value =
                    (vizLfoBuffer.size() == vizBufferSize) ? vizLfoBuffer[idx] : 0.0f;
                vizData.lfoWaveform[j].store(value);
            }
        }
    }

    // --- Update UI Telemetry ---
    setLiveParamValue("rate_live", finalRate);
    setLiveParamValue("depth_live", finalDepth);
    setLiveParamValue("mix_live", finalMix);

    // --- Update Tooltips ---
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1])
            lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
    }
}

bool ChorusModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus

    if (paramId == paramIdRateMod)
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdDepthMod)
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

// Dynamic pin interface - preserves exact same modulation system
std::vector<DynamicPinInfo> ChorusModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio inputs (always present)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});

    // Modulation inputs - exactly matching current static layout
    pins.push_back({"Rate Mod", 2, PinDataType::CV});
    pins.push_back({"Depth Mod", 3, PinDataType::CV});
    pins.push_back({"Mix Mod", 4, PinDataType::CV});

    return pins;
}

std::vector<DynamicPinInfo> ChorusModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio outputs - exactly matching current static layout
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});

    return pins;
}

juce::String ChorusModuleProcessor::getAudioInputLabel(int channel) const
{
    // Bus 0 (Stereo Audio In)
    if (channel == 0)
        return "In L";
    if (channel == 1)
        return "In R";
    // Bus 1-3 (Mono Mod In) - We need to calculate the absolute channel index
    if (channel == 2)
        return "Rate Mod";
    if (channel == 3)
        return "Depth Mod";
    if (channel == 4)
        return "Mix Mod";
    return {};
}

juce::String ChorusModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0)
        return "Out L";
    if (channel == 1)
        return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void ChorusModuleProcessor::drawParametersInNode(
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

    // === VISUALIZATION ===
    auto& themeMgr = ThemeManager::getInstance();
    auto  resolveColor = [](ImU32 primary, ImU32 secondary, ImU32 fallback) {
        if (primary != 0)
            return primary;
        if (secondary != 0)
            return secondary;
        return fallback;
    };

    const ImU32  canvasBg = themeMgr.getCanvasBackground();
    const ImVec4 childBgVec4 = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
    const ImU32  childBg = ImGui::ColorConvertFloat4ToU32(childBgVec4);
    const ImU32  bgColor = resolveColor(theme.modules.scope_plot_bg, canvasBg, childBg);
    const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
    const ImVec4 timbreColorVec4 = theme.modulation.timbre;
    const ImU32  inputColor = resolveColor(
        theme.modules.scope_plot_fg,
        ImGui::ColorConvertFloat4ToU32(
            ImVec4(frequencyColorVec4.x, frequencyColorVec4.y, frequencyColorVec4.z, 1.0f)),
        IM_COL32(110, 220, 255, 255));
    const ImU32 outputColor = resolveColor(
        0,
        ImGui::ColorConvertFloat4ToU32(
            ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f)),
        IM_COL32(255, 190, 120, 255));

    // --- Dual Path Visualization ---
    ImGui::Spacing();
    ImGui::Text("Stereo Modulation");
    ImGui::Spacing();

    const float            vizHeightStereo = 140.0f;
    const ImGuiWindowFlags childFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("ChorusStereoViz", ImVec2(itemWidth, vizHeightStereo), false, childFlags))
    {
        auto*        drawList = ImGui::GetWindowDrawList();
        const ImVec2 stereoOrigin = ImGui::GetWindowPos();
        const ImVec2 stereoSize = ImGui::GetWindowSize();
        const ImVec2 stereoRectMax =
            ImVec2(stereoOrigin.x + stereoSize.x, stereoOrigin.y + stereoSize.y);
        drawList->AddRectFilled(stereoOrigin, stereoRectMax, bgColor, 4.0f);
        ImGui::PushClipRect(stereoOrigin, stereoRectMax, true);

        // Left and right delay arcs (visual metaphor for modulated delay taps)
        auto drawDelayArc = [&](bool isLeft, ImU32 color) {
            const float sideOffset = isLeft ? -stereoSize.x * 0.35f : stereoSize.x * 0.35f;
            const float baseX = stereoOrigin.x + stereoSize.x * 0.5f + sideOffset * 0.2f;
            const float baseY = stereoOrigin.y + stereoSize.y * 0.5f;
            const float maxRadius = juce::jmin(stereoSize.x, stereoSize.y) * 0.4f;
            const float depthScale = juce::jlimit(0.05f, 1.0f, vizData.currentDepth.load());
            const float radius = maxRadius * depthScale;

            float prevX = baseX;
            float prevY = baseY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float norm = (float)i / (float)(VizData::waveformPoints - 1);
                const float phaseOffset = norm * juce::MathConstants<float>::pi;
                const float modValue =
                    isLeft ? vizData.outputWaveformL[i].load() : vizData.outputWaveformR[i].load();
                const float arcX = baseX + std::cos(phaseOffset) * radius;
                const float arcY = baseY + modValue * radius * 0.6f;
                if (i > 0)
                {
                    ImVec4 colorVec4 = ImGui::ColorConvertU32ToFloat4(color);
                    colorVec4.w = 0.4f;
                    drawList->AddLine(
                        ImVec2(prevX, prevY),
                        ImVec2(arcX, arcY),
                        ImGui::ColorConvertFloat4ToU32(colorVec4),
                        2.2f);
                }
                prevX = arcX;
                prevY = arcY;
            }
        };

        drawDelayArc(true, inputColor);
        drawDelayArc(false, outputColor);

        // Mod depth bars on the sides
        const float barWidth = 6.0f;
        const float depthAmount = juce::jlimit(0.0f, 1.0f, vizData.currentDepth.load());
        const float barHeight = stereoSize.y * depthAmount * 0.8f;

        auto drawDepthBar = [&](bool isLeft) {
            const float x = isLeft ? stereoOrigin.x + 8.0f : stereoRectMax.x - 8.0f - barWidth;
            const float y = stereoOrigin.y + stereoSize.y - barHeight - 8.0f;
            drawList->AddRectFilled(
                ImVec2(x, y),
                ImVec2(x + barWidth, y + barHeight),
                isLeft ? inputColor : outputColor,
                2.0f);
        };

        drawDepthBar(true);
        drawDepthBar(false);

        ImGui::PopClipRect();
    }
    ImGui::EndChild();

    // simple modulation summary (to avoid extra clip rects)
    ImGui::Spacing();
    ImGui::Text(
        "Depth: %.2f  |  Rate: %.2f Hz", vizData.currentDepth.load(), vizData.currentRate.load());

    ImGui::Spacing();
    const ImVec4 accentVec4 = theme.accent;
    const ImU32  accentColor =
        ImGui::ColorConvertFloat4ToU32(ImVec4(accentVec4.x, accentVec4.y, accentVec4.z, 1.0f));

    auto drawMeter = [&](const char* label, float value, float normalized) {
        ImGui::Text("%s %.2f", label, value);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
        ImGui::ProgressBar(juce::jlimit(0.0f, 1.0f, normalized), ImVec2(itemWidth * 0.5f, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%.0f%%", juce::jlimit(0.0f, 1.0f, normalized) * 100.0f);
    };

    drawMeter(
        "Rate:", vizData.currentRate.load(), (vizData.currentRate.load() - 0.05f) / (5.0f - 0.05f));
    drawMeter("Depth:", vizData.currentDepth.load(), vizData.currentDepth.load());
    drawMeter("Mix:", vizData.currentMix.load(), vizData.currentMix.load());

    ImGui::Spacing();
    ImGui::Spacing();

    // === CHORUS PARAMETERS ===
    ThemeText("Chorus Parameters", theme.text.section_header);
    ImGui::Spacing();

    bool  isRateMod = isParamModulated(paramIdRateMod);
    float rate = isRateMod ? getLiveParamValueFor(paramIdRateMod, "rate_live", rateParam->load())
                           : rateParam->load();
    if (isRateMod)
        ImGui::BeginDisabled();
    // Draw inline pin for Rate Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2)) // Channel 2 = Rate Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Rate", &rate, 0.05f, 5.0f, "%.2f Hz"))
        if (!isRateMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdRate)) = rate;
    if (!isRateMod)
        adjustParamOnWheel(ap.getParameter(paramIdRate), "rate", rate);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isRateMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker("LFO modulation rate (0.05-5 Hz)\nControls how fast the chorus effect sweeps");

    bool  isDepthMod = isParamModulated(paramIdDepthMod);
    float depth = isDepthMod
                      ? getLiveParamValueFor(paramIdDepthMod, "depth_live", depthParam->load())
                      : depthParam->load();
    if (isDepthMod)
        ImGui::BeginDisabled();
    // Draw inline pin for Depth Mod (channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3)) // Channel 3 = Depth Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Depth", &depth, 0.0f, 1.0f, "%.2f"))
        if (!isDepthMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDepth)) = depth;
    if (!isDepthMod)
        adjustParamOnWheel(ap.getParameter(paramIdDepth), "depth", depth);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isDepthMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker("Modulation depth (0-1)\nControls intensity of pitch/time variation");

    bool  isMixMod = isParamModulated(paramIdMixMod);
    float mix = isMixMod ? getLiveParamValueFor(paramIdMixMod, "mix_live", mixParam->load())
                         : mixParam->load();
    if (isMixMod)
        ImGui::BeginDisabled();
    // Draw inline pin for Mix Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4)) // Channel 4 = Mix Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f"))
        if (!isMixMod)
            *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdMix)) = mix;
    if (!isMixMod)
        adjustParamOnWheel(ap.getParameter(paramIdMix), "mix", mix);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isMixMod)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker("Dry/wet mix (0-1)\n0 = dry only, 1 = fully chorused");

    ImGui::Spacing();
    ImGui::Spacing();

    ThemeText("CV Input Modes", theme.modulation.frequency);
    ImGui::Spacing();

    bool relativeRateMod = relativeRateModParam != nullptr && relativeRateModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Rate Mod", &relativeRateMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeRateMod")))
            *p = relativeRateMod;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±2 octaves)\nOFF: CV directly sets rate (0.05-5 Hz)");

    bool relativeDepthMod =
        relativeDepthModParam != nullptr && relativeDepthModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Depth Mod", &relativeDepthMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeDepthMod")))
            *p = relativeDepthMod;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±0.5)\nOFF: CV directly sets depth (0-1)");

    bool relativeMixMod = relativeMixModParam != nullptr && relativeMixModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Mix Mod", &relativeMixMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMixMod")))
            *p = relativeMixMod;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets mix (0-1)");

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void ChorusModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    auto dynamicInputs = getDynamicInputPins();
    auto dynamicOutputs = getDynamicOutputPins();

    const auto findInput = [&](const char* label) -> const DynamicPinInfo* {
        if (label == nullptr)
            return nullptr;
        for (const auto& pin : dynamicInputs)
            if (pin.name == label)
                return &pin;
        return nullptr;
    };

    const auto findOutput = [&](const char* label) -> const DynamicPinInfo* {
        if (label == nullptr)
            return nullptr;
        for (const auto& pin : dynamicOutputs)
            if (pin.name == label)
                return &pin;
        return nullptr;
    };

    const auto emitRow = [&](const char* inLabel, const char* outLabel) {
        const auto* inPin = findInput(inLabel);
        const auto* outPin = findOutput(outLabel);
        const char* resolvedInLabel = inPin ? inPin->name.toRawUTF8() : nullptr;
        const int   resolvedInChannel = inPin ? inPin->channel : -1;
        const char* resolvedOutLabel = outPin ? outPin->name.toRawUTF8() : nullptr;
        const int   resolvedOutChannel = outPin ? outPin->channel : -1;
        helpers.drawParallelPins(
            resolvedInLabel, resolvedInChannel, resolvedOutLabel, resolvedOutChannel);
    };

    emitRow("In L", "Out L");
    emitRow("In R", "Out R");
    // Rate Mod (ch 2), Depth Mod (ch 3), Mix Mod (ch 4) are drawn inline in drawParametersInNode
}
#endif

std::optional<RhythmInfo> ChorusModuleProcessor::getRhythmInfo() const
{
    RhythmInfo info;

    // Build display name with logical ID
    info.displayName = "Chorus #" + juce::String(getLogicalId());
    info.sourceType = "chorus";

    // Chorus LFO is free-running (not synced to transport)
    info.isSynced = false;
    info.isActive = true; // Always active when processing

    // Convert LFO rate (Hz) to BPM
    // Rate is in cycles per second (Hz), one cycle = one "beat"
    const float rate = rateParam ? rateParam->load() : 1.0f;
    info.bpm = rate * 60.0f; // Convert Hz to BPM

    // Validate BPM before returning
    if (!std::isfinite(info.bpm))
        info.bpm = 0.0f;

    return info;
}
