#include "DriveModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif
#include <cmath> // For std::tanh

juce::AudioProcessorValueTreeState::ParameterLayout DriveModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdDrive, "Drive", 0.0f, 2.0f, 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 0.5f));

    params.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeDriveMod", "Relative Drive Mod", true));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("relativeMixMod", "Relative Mix Mod", true));

    return {params.begin(), params.end()};
}

DriveModuleProcessor::DriveModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Inputs",
                  juce::AudioChannelSet::discreteChannels(4),
                  true) // 0-1: Audio In, 2: Drive Mod, 3: Mix Mod
              .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "DriveParams", createParameterLayout())
{
    driveParam = apvts.getRawParameterValue(paramIdDrive);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    relativeDriveModParam = apvts.getRawParameterValue("relativeDriveMod");
    relativeMixModParam = apvts.getRawParameterValue("relativeMixMod");

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R
}

void DriveModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    smoothedDrive.reset(sampleRate, 0.01);
    smoothedMix.reset(sampleRate, 0.01);
    tempBuffer.setSize(2, samplesPerBlock);
#if defined(PRESET_CREATOR_UI)
    vizDryBuffer.setSize(2, samplesPerBlock);
    vizWetBuffer.setSize(2, samplesPerBlock);
    vizDryBuffer.clear();
    vizWetBuffer.clear();
    vizData.harmonicEnergy.store(0.0f);
    vizData.outputLevelDb.store(-60.0f);
#endif
}

void DriveModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    // Get pointers to modulation CV inputs from unified input bus
    const bool isDriveMod = isParamInputConnected(paramIdDrive);
    const bool isMixMod = isParamInputConnected(paramIdMix);
    auto       inBus = getBusBuffer(buffer, true, 0);
    auto       outBus = getBusBuffer(buffer, false, 0);

    // SAFE: Read input pointers BEFORE any output operations
    const float* driveCV =
        isDriveMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* mixCV = isMixMod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;

    // Get base parameter values ONCE
    const float baseDrive = driveParam != nullptr ? driveParam->load() : 0.0f;
    const float baseMix = mixParam != nullptr ? mixParam->load() : 0.5f;
    const bool  relativeDriveMode = relativeDriveModParam && relativeDriveModParam->load() > 0.5f;
    const bool  relativeMixMode = relativeMixModParam && relativeMixModParam->load() > 0.5f;

    // Copy input to output
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

    const int numChannels = juce::jmin(numInputChannels, numOutputChannels);

#if defined(PRESET_CREATOR_UI)
    vizDryBuffer.makeCopyOf(outBus);
#endif

    // Store dry signal for mixing
    tempBuffer.makeCopyOf(outBus);

    // Process with per-sample modulation
    for (int i = 0; i < numSamples; ++i)
    {
        // PER-SAMPLE: Calculate effective drive FOR THIS SAMPLE
        float currentDrive = baseDrive;
        if (isDriveMod && driveCV != nullptr)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, driveCV[i]);
            if (relativeDriveMode)
            {
                // RELATIVE: CV modulates around base value (±2.0 range centered at base)
                const float range = 2.0f;
                const float offset = (cv - 0.5f) * (range * 2.0f);
                currentDrive = juce::jlimit(0.0f, 2.0f, baseDrive + offset);
            }
            else
            {
                // ABSOLUTE: CV directly maps to drive (0-2)
                currentDrive = juce::jmap(cv, 0.0f, 2.0f);
            }
        }
        smoothedDrive.setTargetValue(currentDrive);
        const float driveAmount = smoothedDrive.getNextValue();

        // PER-SAMPLE: Calculate effective mix FOR THIS SAMPLE
        float currentMix = baseMix;
        if (isMixMod && mixCV != nullptr)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
            if (relativeMixMode)
            {
                // RELATIVE: CV modulates around base value (±1.0 range centered at base)
                const float range = 1.0f;
                const float offset = (cv - 0.5f) * (range * 2.0f);
                currentMix = juce::jlimit(0.0f, 1.0f, baseMix + offset);
            }
            else
            {
                // ABSOLUTE: CV directly maps to mix (0-1)
                currentMix = cv;
            }
        }
        smoothedMix.setTargetValue(currentMix);
        const float mixAmount = smoothedMix.getNextValue();

        // Apply distortion to wet signal
        const float k = juce::jlimit(0.0f, 10.0f, driveAmount) * 5.0f;
        const float dryLevel = 1.0f - mixAmount;
        const float wetLevel = mixAmount;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float drySample = tempBuffer.getSample(ch, i);
            const float wetSample = std::tanh(k * drySample);
            outBus.setSample(ch, i, dryLevel * drySample + wetLevel * wetSample);
        }
    }

    // Update live parameter values for telemetry
    if (numSamples > 0)
    {
        float finalDrive = baseDrive;
        if (isDriveMod && driveCV != nullptr)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, driveCV[numSamples - 1]);
            if (relativeDriveMode)
            {
                const float range = 2.0f;
                const float offset = (cv - 0.5f) * (range * 2.0f);
                finalDrive = juce::jlimit(0.0f, 2.0f, baseDrive + offset);
            }
            else
            {
                finalDrive = juce::jmap(cv, 0.0f, 2.0f);
            }
        }
        setLiveParamValue("drive_live", finalDrive);

        float finalMix = baseMix;
        if (isMixMod && mixCV != nullptr)
        {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[numSamples - 1]);
            if (relativeMixMode)
            {
                const float range = 1.0f;
                const float offset = (cv - 0.5f) * (range * 2.0f);
                finalMix = juce::jlimit(0.0f, 1.0f, baseMix + offset);
            }
            else
            {
                finalMix = cv;
            }
        }
        setLiveParamValue("mix_live", finalMix);
    }

#if defined(PRESET_CREATOR_UI)
    // Create wet buffer for visualization (apply distortion to dry signal)
    vizWetBuffer.makeCopyOf(tempBuffer);
    float finalDriveForViz = baseDrive;
    if (isDriveMod && driveCV != nullptr && numSamples > 0)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, driveCV[numSamples - 1]);
        if (relativeDriveMode)
        {
            const float range = 2.0f;
            const float offset = (cv - 0.5f) * (range * 2.0f);
            finalDriveForViz = juce::jlimit(0.0f, 2.0f, baseDrive + offset);
        }
        else
        {
            finalDriveForViz = juce::jmap(cv, 0.0f, 2.0f);
        }
    }
    const float k = juce::jlimit(0.0f, 10.0f, finalDriveForViz) * 5.0f;
    for (int ch = 0; ch < vizWetBuffer.getNumChannels(); ++ch)
    {
        auto* data = vizWetBuffer.getWritePointer(ch);
        for (int i = 0; i < vizWetBuffer.getNumSamples(); ++i)
        {
            data[i] = std::tanh(k * data[i]);
        }
    }

    auto captureWaveform = [&](const juce::AudioBuffer<float>&                          source,
                               std::array<std::atomic<float>, VizData::waveformPoints>& dest) {
        const int samples = juce::jmin(source.getNumSamples(), numSamples);
        if (samples <= 0)
            return;
        const int stride = juce::jmax(1, samples / VizData::waveformPoints);
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const int idx = juce::jmin(samples - 1, i * stride);
            float     value = source.getSample(0, idx);
            if (source.getNumChannels() > 1)
                value = 0.5f * (value + source.getSample(1, idx));
            dest[i].store(juce::jlimit(-1.0f, 1.0f, value));
        }
    };

    captureWaveform(vizDryBuffer, vizData.dryWaveform);
    captureWaveform(vizWetBuffer, vizData.wetWaveform);
    captureWaveform(outBus, vizData.mixWaveform);

    const int visualSamples = juce::jmin(numSamples, vizDryBuffer.getNumSamples());
    float     diffAccum = 0.0f;
    for (int i = 0; i < visualSamples; ++i)
    {
        const float drySample = vizDryBuffer.getSample(0, i);
        const float wetSample = vizWetBuffer.getSample(0, i);
        diffAccum += std::abs(wetSample - drySample);
    }
    const float harmonicEnergy =
        (visualSamples > 0) ? juce::jlimit(0.0f, 1.0f, diffAccum / (float)visualSamples) : 0.0f;
    vizData.harmonicEnergy.store(harmonicEnergy);

    // Store final values for visualization (use last sample's values)
    float finalDrive = baseDrive;
    if (isDriveMod && driveCV != nullptr && numSamples > 0)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, driveCV[numSamples - 1]);
        if (relativeDriveMode)
        {
            const float range = 2.0f;
            const float offset = (cv - 0.5f) * (range * 2.0f);
            finalDrive = juce::jlimit(0.0f, 2.0f, baseDrive + offset);
        }
        else
        {
            finalDrive = juce::jmap(cv, 0.0f, 2.0f);
        }
    }
    vizData.currentDrive.store(finalDrive);

    float finalMix = baseMix;
    if (isMixMod && mixCV != nullptr && numSamples > 0)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, mixCV[numSamples - 1]);
        if (relativeMixMode)
        {
            const float range = 1.0f;
            const float offset = (cv - 0.5f) * (range * 2.0f);
            finalMix = juce::jlimit(0.0f, 1.0f, baseMix + offset);
        }
        else
        {
            finalMix = cv;
        }
    }
    vizData.currentMix.store(finalMix);

    float outputPeak = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
        outputPeak = juce::jmax(outputPeak, outBus.getRMSLevel(ch, 0, numSamples));
    vizData.outputLevelDb.store(juce::Decibels::gainToDecibels(outputPeak, -60.0f));
#endif

    // Update output values for tooltips
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1] && numChannels > 1)
            lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
    }
}

bool DriveModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus

    if (paramId == paramIdDrive)
    {
        outChannelIndexInBus = 2;
        return true;
    }
    if (paramId == paramIdMix)
    {
        outChannelIndexInBus = 3;
        return true;
    }
    return false;
}

juce::String DriveModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
    case 0:
        return "In L";
    case 1:
        return "In R";
    case 2:
        return "Drive Mod";
    case 3:
        return "Mix Mod";
    default:
        return juce::String("In ") + juce::String(channel + 1);
    }
}

juce::String DriveModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0)
        return "Out L";
    if (channel == 1)
        return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void DriveModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
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
                          float               min,
                          float               max,
                          const char*         format,
                          const char*         tooltip,
                          int                 channel = -1) {
        bool  isModulated = isParamModulated(paramId);
        float value = ap.getRawParameterValue(paramId)->load();
        if (isModulated)
        {
            value = getLiveParamValueFor(paramId, paramId + "_live", value);
            ImGui::BeginDisabled();
        }

        // Draw inline pin if channel is specified and pinHelpers is available
        if (channel >= 0 && pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }

        if (ImGui::SliderFloat(label, &value, min, max, format))
            if (!isModulated)
                *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramId)) = value;
        if (!isModulated)
            adjustParamOnWheel(ap.getParameter(paramId), paramId, value);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            onModificationEnded();
        }
        if (isModulated)
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

    ImGui::Spacing();
    ImGui::Text("Drive Visualizer");
    ImGui::Spacing();

    auto*       drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 dryColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 wetColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
    const ImU32 mixColor = ImGui::ColorConvertFloat4ToU32(theme.accent);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  vizHeight = 110.0f;
    const ImVec2 rectMax = ImVec2(origin.x + itemWidth, origin.y + vizHeight);
    drawList->AddRectFilled(origin, rectMax, bgColor, 4.0f);
    ImGui::PushClipRect(origin, rectMax, true);

    float dryWave[VizData::waveformPoints];
    float wetWave[VizData::waveformPoints];
    float mixWave[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        dryWave[i] = vizData.dryWaveform[i].load();
        wetWave[i] = vizData.wetWaveform[i].load();
        mixWave[i] = vizData.mixWaveform[i].load();
    }

    const float midY = origin.y + vizHeight * 0.5f;
    const float scaleY = vizHeight * 0.4f;
    const float stepX = itemWidth / (float)(VizData::waveformPoints - 1);

    auto drawWave = [&](float* data, ImU32 color, float thickness) {
        float px = origin.x;
        float py = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float x = origin.x + i * stepX;
            const float y = midY - juce::jlimit(-1.0f, 1.0f, data[i]) * scaleY;
            if (i > 0)
                drawList->AddLine(ImVec2(px, py), ImVec2(x, y), color, thickness);
            px = x;
            py = y;
        }
    };

    drawWave(dryWave, dryColor, 1.6f);
    drawWave(wetWave, wetColor, 2.6f);
    drawWave(mixWave, mixColor, 1.2f);

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, rectMax.y));
    ImGui::Dummy(ImVec2(itemWidth, 0));

    const float harmonic = vizData.harmonicEnergy.load();
    const float outputDb = vizData.outputLevelDb.load();
    const float driveVal = vizData.currentDrive.load();
    const float mixVal = vizData.currentMix.load();

    ImGui::Spacing();
    ImGui::Text("Harmonic Density");
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, wetColor);
    ImGui::ProgressBar(
        harmonic, ImVec2(itemWidth * 0.5f, 0), juce::String(harmonic * 100.0f, 1).toRawUTF8());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("Output: %.1f dB", outputDb);

    ImGui::Text("Drive %.2f  |  Mix %.2f", driveVal, mixVal);

    ImGui::Spacing();
    ThemeText("Drive Parameters", theme.text.section_header);
    ImGui::Spacing();

    drawSlider(
        "Drive",
        paramIdDrive,
        0.0f,
        2.0f,
        "%.2f",
        "Saturation amount (0-2)\n0 = clean, 2 = heavy distortion",
        2); // Channel 2 = Drive Mod
    drawSlider(
        "Mix",
        paramIdMix,
        0.0f,
        1.0f,
        "%.2f",
        "Dry/wet mix (0-1)\n0 = clean, 1 = fully driven",
        3); // Channel 3 = Mix Mod

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void DriveModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-3) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Drive Mod (ch 2), Mix Mod (ch 3) are drawn inline in drawParametersInNode
}
#endif
