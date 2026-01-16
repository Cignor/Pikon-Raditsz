#include "WaveshaperModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif
#include <array>
#include <cmath> // For std::tanh

juce::AudioProcessorValueTreeState::ParameterLayout WaveshaperModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "drive", "Drive", juce::NormalisableRange<float>(1.0f, 100.0f, 0.01f, 0.3f), 1.0f));
    p.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "type", "Type", juce::StringArray{"Soft Clip (tanh)", "Hard Clip", "Foldback"}, 0));

    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "mix", "Mix", juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // Relative modulation parameters
    p.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeDriveMod", "Relative Drive Mod", true));

    return {p.begin(), p.end()};
}

WaveshaperModuleProcessor::WaveshaperModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Inputs",
                  juce::AudioChannelSet::discreteChannels(5),
                  true) // 0-1: Audio In, 2: Drive Mod, 3: Type Mod, 4: Mix Mod
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "WaveshaperParams", createParameterLayout())
{
    driveParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("drive"));
    typeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("type"));
    mixParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("mix"));
    relativeDriveModParam = apvts.getRawParameterValue("relativeDriveMod");

    // Initialize output value tracking for tooltips
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // For Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // For Out R

#if defined(PRESET_CREATOR_UI)
    for (auto& sample : vizData.transferCurve)
        sample.store(0.0f);
    for (auto& sample : vizData.driveHistory)
        sample.store(0.0f);
    curveScratch.resize(VizData::curvePoints, 0.0f);
    driveHistoryScratch.resize(VizData::historySize, 0.0f);
    vizData.historyWriteIndex.store(0);
#endif
}

void WaveshaperModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    smoothedMix.reset(sampleRate, 0.01);
#if defined(PRESET_CREATOR_UI)
    dryBlockTemp.setSize(2, samplesPerBlock);
    dryBlockTemp.clear();
    curveScratch.assign(VizData::curvePoints, 0.0f);
    driveHistoryScratch.assign(VizData::historySize, 0.0f);
    vizData.historyWriteIndex.store(0);
    driveHistoryWriteIndex = 0;
#endif
}

void WaveshaperModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    juce::ignoreUnused(midi);

    // Get pointers to modulation CV inputs from unified input bus
    const bool isDriveMod = isParamInputConnected("drive");
    const bool isTypeMod = isParamInputConnected("type");
    const bool isMixMod = isParamInputConnected("mix");
    auto       inBus = getBusBuffer(buffer, true, 0);
    auto       outBus = getBusBuffer(buffer, false, 0);

    // SAFE: Read input pointers BEFORE any output operations
    const float* driveCV =
        isDriveMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* typeCV =
        isTypeMod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;
    const float* mixCV = isMixMod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;

    // Get base parameter values ONCE
    const float baseDrive = driveParam != nullptr ? driveParam->get() : 1.0f;
    const int   baseType = typeParam != nullptr ? typeParam->getIndex() : 0;
    const float baseMix = mixParam != nullptr ? mixParam->get() : 1.0f;
    const bool  relativeDriveMode = relativeDriveModParam && relativeDriveModParam->load() > 0.5f;

    const int numSamples = buffer.getNumSamples();

#if defined(PRESET_CREATOR_UI)
    const int dryChannels = juce::jmin(2, buffer.getNumChannels());
    if (dryBlockTemp.getNumChannels() < dryChannels || dryBlockTemp.getNumSamples() < numSamples)
        dryBlockTemp.setSize(juce::jmax(2, dryChannels), numSamples, false, false, true);
    dryBlockTemp.clear();
    for (int ch = 0; ch < dryChannels; ++ch)
        dryBlockTemp.copyFrom(ch, 0, buffer, ch, 0, numSamples);
#endif

    // Store dry signal for mixing (always needed for dry/wet)
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.setSize(2, numSamples, false, false, true);
    for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // Process waveshaping first, then apply dry/wet mix
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* data = buffer.getWritePointer(ch);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            // PER-SAMPLE: Calculate effective drive FOR THIS SAMPLE
            float drive = baseDrive;
            if (isDriveMod && driveCV != nullptr)
            {
                const float cv = juce::jlimit(0.0f, 1.0f, driveCV[i]);
                if (relativeDriveMode)
                {
                    // RELATIVE: ±3 octaves (0.125x to 8x)
                    const float octaveRange = 3.0f;
                    const float octaveOffset = (cv - 0.5f) * (octaveRange * 2.0f);
                    drive = baseDrive * std::pow(2.0f, octaveOffset);
                }
                else
                {
                    // ABSOLUTE: CV directly sets drive (1-100)
                    drive = juce::jmap(cv, 1.0f, 100.0f);
                }
                drive = juce::jlimit(1.0f, 100.0f, drive);
            }

            // PER-SAMPLE: Calculate effective type FOR THIS SAMPLE
            int type = baseType;
            if (isTypeMod && typeCV != nullptr)
            {
                const float cv = juce::jlimit(0.0f, 1.0f, typeCV[i]);
                // Map CV [0,1] to type [0,2] with wrapping
                type = static_cast<int>(cv * 3.0f) % 3;
            }

            // Process waveshaping (this becomes the wet signal)
            float s = data[i] * drive;

            switch (type)
            {
            case 0: // Soft Clip (tanh)
                data[i] = std::tanh(s);
                break;
            case 1: // Hard Clip
                data[i] = juce::jlimit(-1.0f, 1.0f, s);
                break;
            case 2: // Foldback
                data[i] = std::abs(std::abs(std::fmod(s - 1.0f, 4.0f)) - 2.0f) - 1.0f;
                break;
            }
        }
    }

    // Apply dry/wet mix (after processing)
    for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
    {
        float*       wetData = buffer.getWritePointer(ch);
        const float* dryData = dryBuffer.getReadPointer(ch);

        for (int i = 0; i < numSamples; ++i)
        {
            // PER-SAMPLE: Calculate effective mix FOR THIS SAMPLE
            float currentMix = baseMix;
            if (isMixMod && mixCV != nullptr)
            {
                const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
                currentMix = cv;
            }
            smoothedMix.setTargetValue(currentMix);
            const float mix = smoothedMix.getNextValue();
            const float dry = 1.0f - mix;

            // Mix dry and wet
            wetData[i] = dry * dryData[i] + mix * wetData[i];
        }
    }

    // Store live modulated values for UI display (use last sample's values)
    float finalDrive = baseDrive;
    if (isDriveMod && driveCV != nullptr)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, driveCV[buffer.getNumSamples() - 1]);
        if (relativeDriveMode)
        {
            // RELATIVE: ±3 octaves
            const float octaveRange = 3.0f;
            const float octaveOffset = (cv - 0.5f) * (octaveRange * 2.0f);
            finalDrive = baseDrive * std::pow(2.0f, octaveOffset);
        }
        else
        {
            // ABSOLUTE: CV directly sets drive
            finalDrive = juce::jmap(cv, 1.0f, 100.0f);
        }
        finalDrive = juce::jlimit(1.0f, 100.0f, finalDrive);
    }
    setLiveParamValue("drive_live", finalDrive);

    int finalType = baseType;
    if (isTypeMod && typeCV != nullptr)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, typeCV[buffer.getNumSamples() - 1]);
        finalType = static_cast<int>(cv * 3.0f) % 3;
    }
    setLiveParamValue("type_live", static_cast<float>(finalType));

    float finalMix = baseMix;
    if (isMixMod && mixCV != nullptr)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, mixCV[buffer.getNumSamples() - 1]);
        finalMix = cv;
    }
    setLiveParamValue("mix_live", finalMix);

#if defined(PRESET_CREATOR_UI)
    vizData.liveDrive.store(finalDrive);
    vizData.liveType.store(finalType);

    auto computeRms = [numSamples](juce::AudioBuffer<float>& buf) {
        if (buf.getNumChannels() == 0 || numSamples <= 0)
            return 0.0f;
        const int channels = juce::jmin(2, buf.getNumChannels());
        float     sum = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            sum += buf.getRMSLevel(ch, 0, numSamples);
        return sum / (float)channels;
    };

    vizData.inputRms.store(computeRms(dryBlockTemp));
    vizData.outputRms.store(computeRms(buffer));

    const auto applyTransfer = [](float input, float drive, int type) {
        const float s = input * drive;
        switch (type)
        {
        case 0:
            return std::tanh(s);
        case 1:
            return juce::jlimit(-1.0f, 1.0f, s);
        case 2:
        default:
            return std::abs(std::abs(std::fmod(s - 1.0f, 4.0f)) - 2.0f) - 1.0f;
        }
    };

    if ((int)curveScratch.size() != VizData::curvePoints)
        curveScratch.resize(VizData::curvePoints);
    for (int i = 0; i < VizData::curvePoints; ++i)
    {
        const float x =
            juce::jmap((float)i / (float)(VizData::curvePoints - 1), 0.0f, 1.0f, -1.0f, 1.0f);
        const float y = applyTransfer(x, finalDrive, finalType);
        vizData.transferCurve[i].store(y);
    }

    const float normalizedDrive = juce::jlimit(0.0f, 1.0f, (finalDrive - 1.0f) / 99.0f);
    vizData.driveHistory[driveHistoryWriteIndex].store(normalizedDrive);
    driveHistoryWriteIndex = (driveHistoryWriteIndex + 1) % VizData::historySize;
    vizData.historyWriteIndex.store(driveHistoryWriteIndex);
#endif

    // Update output values for tooltips
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(buffer.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1])
            lastOutputValues[1]->store(buffer.getSample(1, buffer.getNumSamples() - 1));
    }
}

#if defined(PRESET_CREATOR_UI)
void WaveshaperModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto&       ap = getAPVTS();
    const auto& freqColors = theme.modules.frequency_graph;
    const auto  resolveColor = [](ImU32 value, ImU32 fallback) {
        return value != 0 ? value : fallback;
    };
    const ImU32 curveBg = resolveColor(freqColors.background, IM_COL32(18, 20, 24, 255));
    const ImU32 curveGrid = resolveColor(freqColors.grid, IM_COL32(50, 55, 65, 255));
    const ImU32 curveLine = resolveColor(freqColors.live_line, IM_COL32(255, 140, 90, 255));
    const ImU32 diagLine = resolveColor(freqColors.peak_line, IM_COL32(90, 140, 255, 180));
    const ImU32 meterBg = resolveColor(freqColors.background, IM_COL32(25, 27, 32, 255));
    const ImU32 meterFill = resolveColor(freqColors.peak_line, IM_COL32(255, 120, 60, 200));
    const ImU32 meterFillB = resolveColor(freqColors.live_line, IM_COL32(120, 200, 255, 200));
    const ImU32 driveActive = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 driveInactive = IM_COL32(70, 75, 85, 255);

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

    static constexpr std::array<const char*, 3> typeNames{"Soft Clip", "Hard Clip", "Foldback"};

    float drive = 1.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("drive")))
        drive = *p;
    int type = 0;
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("type")))
        type = p->getIndex();

    std::array<float, VizData::curvePoints> curvePoints{};
    for (int i = 0; i < VizData::curvePoints; ++i)
        curvePoints[i] = vizData.transferCurve[i].load();
    std::array<float, VizData::historySize> driveHistory{};
    for (int i = 0; i < VizData::historySize; ++i)
        driveHistory[i] = vizData.driveHistory[i].load();
    const int   historyWriteIdx = vizData.historyWriteIndex.load();
    const float liveDrive = vizData.liveDrive.load();
    const int   liveType = vizData.liveType.load();
    const float inputRms = vizData.inputRms.load();
    const float outputRms = vizData.outputRms.load();

    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    if (ImGui::BeginChild(
            "WaveshaperCurveViz",
            ImVec2(itemWidth, 170.0f),
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        auto*        drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1{p0.x + itemWidth, p0.y + 170.0f};
        drawList->AddRectFilled(p0, p1, curveBg);
        drawList->PushClipRect(p0, p1, true);

        auto xToScreen = [&](float x) {
            return juce::jmap(x, -1.0f, 1.0f, p0.x + 6.0f, p1.x - 6.0f);
        };
        auto yToScreen = [&](float y) {
            return juce::jmap(y, 1.2f, -1.2f, p0.y + 6.0f, p1.y - 6.0f);
        };

        // Axes
        drawList->AddLine(
            ImVec2(p0.x, yToScreen(0.0f)), ImVec2(p1.x, yToScreen(0.0f)), curveGrid, 1.0f);
        drawList->AddLine(
            ImVec2(xToScreen(0.0f), p0.y), ImVec2(xToScreen(0.0f), p1.y), curveGrid, 1.0f);

        // Diagonal reference
        drawList->AddLine(
            ImVec2(xToScreen(-1.0f), yToScreen(-1.0f)),
            ImVec2(xToScreen(1.0f), yToScreen(1.0f)),
            diagLine,
            1.5f);

        // Active transfer curve
        for (int i = 1; i < VizData::curvePoints; ++i)
        {
            const float prevX =
                juce::jmap((float)(i - 1) / (VizData::curvePoints - 1), 0.0f, 1.0f, -1.0f, 1.0f);
            const float currX =
                juce::jmap((float)i / (VizData::curvePoints - 1), 0.0f, 1.0f, -1.0f, 1.0f);
            drawList->AddLine(
                ImVec2(xToScreen(prevX), yToScreen(curvePoints[i - 1])),
                ImVec2(xToScreen(currX), yToScreen(curvePoints[i])),
                curveLine,
                liveType == 2 ? 2.5f : 2.0f);
        }

        drawList->PopClipRect();
        const ImVec2 childSize = ImGui::GetWindowSize();
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton(
            "CurveDragBlocker",
            childSize,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        drawList->AddText(
            ImVec2(p0.x + 8.0f, p0.y + 8.0f),
            IM_COL32(220, 220, 230, 255),
            (juce::String("Transfer: ") + typeNames[(size_t)liveType] + "  |  Drive " +
             juce::String(liveDrive, 2))
                .toRawUTF8());
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::BeginChild(
            "WaveshaperEnergy",
            ImVec2(itemWidth, 80.0f),
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        auto*        drawList = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetWindowPos();
        const ImVec2 end{start.x + itemWidth, start.y + 80.0f};
        drawList->AddRectFilled(start, end, meterBg);
        drawList->PushClipRect(start, end, true);

        const float meterWidth = (itemWidth - 40.0f) * 0.5f;
        auto        drawMeter = [&](float value, float xOffset, ImU32 fill) {
            const float  clamped = juce::jlimit(0.0f, 1.0f, value);
            const float  height = (end.y - start.y - 25.0f) * clamped;
            const ImVec2 base{start.x + xOffset, end.y - 10.0f};
            const ImVec2 top{base.x + meterWidth, base.y - height};
            drawList->AddRectFilled(ImVec2(base.x, top.y), ImVec2(top.x, base.y), fill, 2.0f);
            drawList->AddRect(ImVec2(base.x, top.y), ImVec2(top.x, base.y), IM_COL32(0, 0, 0, 100));
        };

        drawMeter(inputRms, 12.0f, meterFillB);
        drawMeter(outputRms, 24.0f + meterWidth, meterFill);

        drawList->PopClipRect();

        const ImVec2 childSize = ImGui::GetWindowSize();
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton(
            "EnergyDragBlocker",
            childSize,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));

        drawList->AddText(
            ImVec2(start.x + 12.0f, start.y + 6.0f), IM_COL32(200, 200, 210, 255), "Energy");
        drawList->AddText(
            ImVec2(start.x + 12.0f, start.y + 60.0f),
            IM_COL32(150, 150, 160, 255),
            (juce::String("Dry ") +
             juce::String(juce::Decibels::gainToDecibels(inputRms + 1.0e-5f), 1) + " dB")
                .toRawUTF8());
        drawList->AddText(
            ImVec2(start.x + meterWidth + 36.0f, start.y + 60.0f),
            IM_COL32(150, 150, 160, 255),
            (juce::String("Wet ") +
             juce::String(juce::Decibels::gainToDecibels(outputRms + 1.0e-5f), 1) + " dB")
                .toRawUTF8());
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::BeginChild(
            "WaveshaperDriveHistory",
            ImVec2(itemWidth, 60.0f),
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        auto*        drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1{p0.x + itemWidth, p0.y + 60.0f};
        drawList->AddRectFilled(p0, p1, meterBg);
        drawList->PushClipRect(p0, p1, true);

        auto idxToValue = [&](int visualIndex) {
            const int histSize = VizData::historySize;
            const int absolute = (historyWriteIdx + visualIndex) % histSize;
            return driveHistory[absolute];
        };

        float prevX = p0.x;
        float prevY = p1.y;
        for (int i = 0; i < VizData::historySize; ++i)
        {
            const float value = idxToValue(i);
            const float x = juce::jmap(
                (float)i, 0.0f, (float)(VizData::historySize - 1), p0.x + 4.0f, p1.x - 4.0f);
            const float y = juce::jmap(value, 0.0f, 1.0f, p1.y - 6.0f, p0.y + 6.0f);
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), driveActive, 2.0f);
            prevX = x;
            prevY = y;
        }

        drawList->PopClipRect();
        const ImVec2 childSize = ImGui::GetWindowSize();
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton(
            "DriveHistoryDragBlocker",
            childSize,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        drawList->AddText(
            ImVec2(p0.x + 8.0f, p0.y + 4.0f),
            IM_COL32(200, 200, 210, 255),
            (juce::String("Drive History ") + juce::String(liveDrive, 2)).toRawUTF8());
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Type selector bars
    const bool isTypeModulated = isParamModulated("type");
    int        displayedType = type;
    if (isTypeModulated)
        displayedType =
            static_cast<int>(getLiveParamValueFor("type", "type_live", static_cast<float>(type)));
    // Draw inline pin for Type Mod (channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3)) // Channel 3 = Type Mod
            ImGui::SameLine();
    }
    const float barHeight = 32.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float totalSpacing = 2.0f * spacing;
    const float barWidth = (itemWidth - totalSpacing) / 3.0f;
    ImGui::TextUnformatted("Waveshape Selector");
    if (isTypeModulated)
        ImGui::SameLine(), ImGui::TextUnformatted("(mod)");
    ImGui::BeginDisabled(isTypeModulated);
    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
            ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 size{barWidth, barHeight};
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool   pressed = ImGui::InvisibleButton("TypeBar", size);
        auto*        drawList = ImGui::GetWindowDrawList();
        const bool   hovered = ImGui::IsItemHovered();
        const bool   active = displayedType == i;
        const ImU32  fill = active ? driveActive : driveInactive;
        drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth, pos.y + barHeight), fill, 4.0f);
        drawList->AddRect(
            pos,
            ImVec2(pos.x + barWidth, pos.y + barHeight),
            IM_COL32(0, 0, 0, 180),
            4.0f,
            0,
            hovered ? 2.0f : 1.0f);
        drawList->AddText(
            ImVec2(pos.x + 8.0f, pos.y + 8.0f), IM_COL32(15, 15, 20, 255), typeNames[(size_t)i]);

        if (hovered && ImGui::BeginItemTooltip())
        {
            ImGui::Text("%s\nCtrl+Click cycles to next type.", typeNames[(size_t)i]);
            ImGui::EndTooltip();
        }

        if (pressed)
        {
            int newType = i;
            if (ImGui::GetIO().KeyCtrl)
                newType = (displayedType + 1) % 3;
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("type")))
                *p = newType;
            displayedType = newType;
            onModificationEnded();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    ThemeText("Waveshaper Parameters", theme.text.section_header);
    ImGui::Spacing();

    // Drive
    bool isDriveModulated = isParamModulated("drive");
    if (isDriveModulated)
    {
        drive = getLiveParamValueFor("drive", "drive_live", drive);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Drive Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2)) // Channel 2 = Drive Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Drive", &drive, 1.0f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
        if (!isDriveModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("drive")))
                *p = drive;
    if (!isDriveModulated)
        adjustParamOnWheel(ap.getParameter("drive"), "drive", drive);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isDriveModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker("Drive amount (1-100)\nLogarithmic scale for fine control");

    ImGui::Spacing();

    // Mix
    bool  isMixModulated = isParamModulated("mix");
    float mix = 1.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("mix")))
        mix = *p;
    if (isMixModulated)
    {
        mix = getLiveParamValueFor("mix", "mix_live", mix);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Mix Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4)) // Channel 4 = Mix Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f"))
        if (!isMixModulated)
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("mix")))
                *p = mix;
    if (!isMixModulated)
        adjustParamOnWheel(ap.getParameter("mix"), "mix", mix);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        onModificationEnded();
    }
    if (isMixModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }
    ImGui::SameLine();
    HelpMarker("Wet/dry balance (0=dry, 1=wet)");

    ImGui::Spacing();

    // === RELATIVE MODULATION SECTION ===
    ThemeText("CV Input Modes", theme.modulation.frequency);
    ImGui::Spacing();

    // Relative Drive Mod checkbox
    bool relativeDriveMod =
        relativeDriveModParam != nullptr && relativeDriveModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Drive Mod", &relativeDriveMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeDriveMod")))
            *p = relativeDriveMod;
        juce::Logger::writeToLog(
            "[Waveshaper UI] Relative Drive Mod: " + juce::String(relativeDriveMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "ON: CV modulates around slider (±3 octaves)\nOFF: CV directly sets drive (1-100)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void WaveshaperModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-4) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Drive Mod (ch 2), Type Mod (ch 3), Mix Mod (ch 4) are drawn inline in drawParametersInNode
}
#endif

std::vector<DynamicPinInfo> WaveshaperModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});

    // Modulation inputs (channels 2-4)
    pins.push_back({"Drive Mod", 2, PinDataType::CV});
    pins.push_back({"Type Mod", 3, PinDataType::CV});
    pins.push_back({"Mix Mod", 4, PinDataType::CV});

    return pins;
}

std::vector<DynamicPinInfo> WaveshaperModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});

    return pins;
}

// Parameter bus contract implementation
bool WaveshaperModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus

    if (paramId == "drive")
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == "type")
    {
        outChannelIndexInBus = 3;
        return true;
    }
    if (paramId == "mix")
    {
        outChannelIndexInBus = 4;
        return true;
    }
    return false;
}
