#include "GateModuleProcessor.h"

juce::AudioProcessorValueTreeState::ParameterLayout GateModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdThreshold, "Threshold", -80.0f, 0.0f, -40.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdAttack, "Attack", 0.1f, 100.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdRelease, "Release", 5.0f, 1000.0f, 50.0f));
    
    return { params.begin(), params.end() };
}

GateModuleProcessor::GateModuleProcessor()
    : ModuleProcessor(BusesProperties()
          .withInput("Inputs", juce::AudioChannelSet::discreteChannels(5), true) // ch0-1: audio, ch2-4: mods
          .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "GateParams", createParameterLayout())
{
    thresholdParam = apvts.getRawParameterValue(paramIdThreshold);
    attackParam = apvts.getRawParameterValue(paramIdAttack);
    releaseParam = apvts.getRawParameterValue(paramIdRelease);

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R
}

void GateModuleProcessor::prepareToPlay(double newSampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = newSampleRate;
    envelope = 0.0f;

#if defined(PRESET_CREATOR_UI)
    vizData.gateAmount.store(0.0f);
    vizData.currentThresholdDb.store(thresholdParam ? thresholdParam->load() : -40.0f);
    vizData.currentAttackMs.store(attackParam ? attackParam->load() : 1.0f);
    vizData.currentReleaseMs.store(releaseParam ? releaseParam->load() : 50.0f);
    vizData.writeIndex.store(0);
    for (auto& v : vizData.inputHistory) v.store(-80.0f);
    for (auto& v : vizData.envelopeHistory) v.store(-80.0f);
    for (auto& v : vizData.gateHistory) v.store(0.0f);
#endif
}

void GateModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    // Copy audio input to output (channels 0-1)
    const int numInputChannels = inBus.getNumChannels();
    const int numOutputChannels = outBus.getNumChannels();

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

    // Check for modulation inputs and get CV pointers
    const bool isThresholdMod = isParamInputConnected(paramIdThresholdMod);
    const bool isAttackMod = isParamInputConnected(paramIdAttackMod);
    const bool isReleaseMod = isParamInputConnected(paramIdReleaseMod);

    const float* thresholdCV = isThresholdMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* attackCV = isAttackMod && inBus.getNumChannels() > 3 ? inBus.getReadPointer(3) : nullptr;
    const float* releaseCV = isReleaseMod && inBus.getNumChannels() > 4 ? inBus.getReadPointer(4) : nullptr;

    // Get base parameters
    const float baseThresholdDb = thresholdParam != nullptr ? thresholdParam->load() : -40.0f;
    const float baseAttackMs = attackParam != nullptr ? attackParam->load() : 1.0f;
    const float baseReleaseMs = releaseParam != nullptr ? releaseParam->load() : 50.0f;

    // Calculate effective parameters (with modulation) - use first sample for per-block processing
    float thresholdDb = baseThresholdDb;
    if (isThresholdMod && thresholdCV != nullptr)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, thresholdCV[0]);
        thresholdDb = juce::jmap(cv, -80.0f, 0.0f); // CV maps to full threshold range
    }
    thresholdDb = juce::jlimit(-80.0f, 0.0f, thresholdDb);
    const float thresholdLinear = juce::Decibels::decibelsToGain(thresholdDb);

    float attackMs = baseAttackMs;
    if (isAttackMod && attackCV != nullptr)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, attackCV[0]);
        attackMs = juce::jmap(cv, 0.1f, 100.0f); // CV maps to full attack range
    }
    attackMs = juce::jmax(0.1f, attackMs);

    float releaseMs = baseReleaseMs;
    if (isReleaseMod && releaseCV != nullptr)
    {
        const float cv = juce::jlimit(0.0f, 1.0f, releaseCV[0]);
        releaseMs = juce::jmap(cv, 5.0f, 1000.0f); // CV maps to full release range
    }
    releaseMs = juce::jmax(1.0f, releaseMs);

    // Convert attack/release times from ms to per-sample coefficients
    const float attackCoeff = 1.0f - std::exp(-1.0f / (attackMs * 0.001f * (float)currentSampleRate));
    const float releaseCoeff = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * (float)currentSampleRate));

    // Store live values for UI display (CRITICAL: outside any conditional blocks)
    setLiveParamValue("threshold_live", thresholdDb);
    setLiveParamValue("attack_live", attackMs);
    setLiveParamValue("release_live", releaseMs);

    auto* leftData = outBus.getWritePointer(0);
    auto* rightData = numChannels > 1 ? outBus.getWritePointer(1) : nullptr;

    float peakInput = 0.0f;
    float peakEnvelope = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        // Get the magnitude of the input signal (mono or stereo)
        float magnitude = std::abs(leftData[i]);
        if (rightData)
            magnitude = std::max(magnitude, std::abs(rightData[i]));

        peakInput = std::max(peakInput, magnitude);

        // Determine if the gate should be open or closed
        float target = (magnitude >= thresholdLinear) ? 1.0f : 0.0f;

        // Move the envelope towards the target using the appropriate attack or release time
        if (target > envelope)
            envelope += (target - envelope) * attackCoeff;
        else
            envelope += (target - envelope) * releaseCoeff;

        peakEnvelope = std::max(peakEnvelope, envelope);
        
        // Apply the envelope as a gain to the signal
        leftData[i] *= envelope;
        if (rightData)
            rightData[i] *= envelope;
    }

#if defined(PRESET_CREATOR_UI)
    const float inputDb = juce::Decibels::gainToDecibels(juce::jmax(peakInput, 1.0e-6f), -80.0f);
    const float envelopeDb = juce::Decibels::gainToDecibels(juce::jmax(peakEnvelope, 1.0e-6f), -80.0f);
    int writeIdx = vizData.writeIndex.load();
    vizData.inputHistory[writeIdx].store(inputDb);
    vizData.envelopeHistory[writeIdx].store(envelopeDb);
    vizData.gateHistory[writeIdx].store(peakEnvelope);
    writeIdx = (writeIdx + 1) % VizData::historyPoints;
    vizData.writeIndex.store(writeIdx);
    vizData.currentThresholdDb.store(thresholdDb);
    vizData.currentAttackMs.store(attackMs);
    vizData.currentReleaseMs.store(releaseMs);
    vizData.gateAmount.store(envelope);
#endif

    // Update output values for tooltips
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0]) lastOutputValues[0]->store(leftData[numSamples - 1]);
        if (lastOutputValues[1] && rightData) lastOutputValues[1]->store(rightData[numSamples - 1]);
    }
}

bool GateModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All inputs are on bus 0
    if (paramId == paramIdThresholdMod) { outChannelIndexInBus = 2; return true; }  // Threshold Mod
    if (paramId == paramIdAttackMod) { outChannelIndexInBus = 3; return true; }     // Attack Mod
    if (paramId == paramIdReleaseMod) { outChannelIndexInBus = 4; return true; }   // Release Mod
    return false;
}

juce::String GateModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "In L";
        case 1: return "In R";
        case 2: return "Threshold Mod";
        case 3: return "Attack Mod";
        case 4: return "Release Mod";
        default: return {};
    }
}

juce::String GateModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0) return "Out L";
    if (channel == 1) return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void GateModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    auto& ap = getAPVTS();
    ImGui::PushItemWidth(itemWidth);

    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto* drawList = ImGui::GetWindowDrawList();

    ImGui::Spacing();
    ImGui::Text("Gate Visualizer");
    ImGui::Spacing();

    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 envelopeColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
    const ImU32 gateColor = ImGui::ColorConvertFloat4ToU32(theme.accent);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float vizHeight = 90.0f;
    const ImVec2 rectMax = ImVec2(origin.x + itemWidth, origin.y + vizHeight);
    drawList->AddRectFilled(origin, rectMax, bgColor, 4.0f);
    ImGui::PushClipRect(origin, rectMax, true);

    float inputHistory[VizData::historyPoints];
    float envelopeHistory[VizData::historyPoints];
    float gateHistory[VizData::historyPoints];
    const int writeIdx = vizData.writeIndex.load();
    for (int i = 0; i < VizData::historyPoints; ++i)
    {
        const int idx = (writeIdx + i) % VizData::historyPoints;
        inputHistory[i] = vizData.inputHistory[idx].load();
        envelopeHistory[i] = vizData.envelopeHistory[idx].load();
        gateHistory[i] = vizData.gateHistory[idx].load();
    }

    auto mapDbToNorm = [](float db)
    {
        return juce::jlimit(0.0f, 1.0f, (db + 80.0f) / 80.0f);
    };

    const float stepX = itemWidth / (float)(VizData::historyPoints - 1);
    float prevX = origin.x;
    float prevY = rectMax.y;
    for (int i = 0; i < VizData::historyPoints; ++i)
    {
        const float norm = mapDbToNorm(inputHistory[i]);
        const float y = rectMax.y - norm * (vizHeight - 8.0f) - 4.0f;
        const float x = origin.x + i * stepX;
        if (i > 0)
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), inputColor, 2.0f);
        prevX = x;
        prevY = y;
    }

    prevX = origin.x;
    prevY = rectMax.y;
    for (int i = 0; i < VizData::historyPoints; ++i)
    {
        const float norm = mapDbToNorm(envelopeHistory[i]);
        const float y = rectMax.y - norm * (vizHeight - 8.0f) - 4.0f;
        const float x = origin.x + i * stepX;
        if (i > 0)
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), envelopeColor, 2.2f);
        prevX = x;
        prevY = y;
    }

    for (int i = 0; i < VizData::historyPoints; ++i)
    {
        const float state = gateHistory[i];
        if (state > 0.01f)
        {
            const float x = origin.x + i * stepX;
            const float yTop = origin.y + 4.0f;
            const float yBottom = origin.y + 12.0f + (1.0f - state) * 10.0f;
            drawList->AddLine(ImVec2(x, yTop), ImVec2(x, yBottom), gateColor, 1.2f);
        }
    }

    const float thresholdDb = vizData.currentThresholdDb.load();
    const float thresholdNorm = mapDbToNorm(thresholdDb);
    const float thresholdY = rectMax.y - thresholdNorm * (vizHeight - 8.0f) - 4.0f;
    drawList->AddLine(ImVec2(origin.x, thresholdY), ImVec2(rectMax.x, thresholdY), IM_COL32(255, 255, 255, 120), 1.5f);
    const juce::String threshText = juce::String(thresholdDb, 1) + " dB";
    drawList->AddText(ImVec2(origin.x + 6.0f, thresholdY - ImGui::GetTextLineHeight()), IM_COL32(255, 255, 255, 160), threshText.toRawUTF8());

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, rectMax.y));
    ImGui::Dummy(ImVec2(itemWidth, 0));

    ImGui::Spacing();
    ImGui::Text("Gate State");
    const float gateAmt = vizData.gateAmount.load();
    const char* gateLabel = gateAmt > 0.5f ? "OPEN" : (gateAmt > 0.1f ? "TRANSIENT" : "CLOSED");
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, gateColor);
    ImGui::ProgressBar(gateAmt, ImVec2(itemWidth * 0.6f, 0), gateLabel);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    auto drawSlider = [&](const char* label, const juce::String& paramId, const juce::String& modId, float min, float max, const char* format, int channel) {
        bool isMod = isParamModulated(modId);
        float value = isMod ? getLiveParamValueFor(modId, paramId + juce::String("_live"), ap.getRawParameterValue(paramId)->load())
                            : ap.getRawParameterValue(paramId)->load();
        
        if (isMod) ImGui::BeginDisabled();
        
        // Draw inline pin if channel is specified
        if (channel >= 0 && pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
        
        if (ImGui::SliderFloat(label, &value, min, max, format))
        {
            if (!isMod)
                *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramId)) = value;
        }
        if (!isMod) adjustParamOnWheel(ap.getParameter(paramId), paramId, value);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    };

    drawSlider("Threshold", paramIdThreshold, paramIdThresholdMod, -80.0f, 0.0f, "%.1f dB", 2);
    drawSlider("Attack", paramIdAttack, paramIdAttackMod, 0.1f, 100.0f, "%.1f ms", 3);
    drawSlider("Release", paramIdRelease, paramIdReleaseMod, 5.0f, 1000.0f, "%.0f ms", 4);

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void GateModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Audio inputs and outputs stay as parallel pins
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Threshold Mod (ch 2), Attack Mod (ch 3), Release Mod (ch 4) are drawn inline in drawParametersInNode
}
#endif

