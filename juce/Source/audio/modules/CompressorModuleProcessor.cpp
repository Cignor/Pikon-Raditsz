#include "CompressorModuleProcessor.h"
#include <cmath>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout CompressorModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdThreshold, "Threshold", -60.0f, 0.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdRatio, "Ratio", 1.0f, 20.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdAttack, "Attack", 0.1f, 200.0f, 10.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdRelease, "Release", 5.0f, 1000.0f, 100.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMakeup, "Makeup Gain", -12.0f, 12.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 1.0f));
    
    // Relative modulation parameters
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeThresholdMod", "Relative Threshold Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeRatioMod", "Relative Ratio Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeAttackMod", "Relative Attack Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeReleaseMod", "Relative Release Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeMakeupMod", "Relative Makeup Mod", true));
    
    return { params.begin(), params.end() };
}

CompressorModuleProcessor::CompressorModuleProcessor()
    : ModuleProcessor(BusesProperties()
          .withInput("Inputs", juce::AudioChannelSet::discreteChannels(8), true) // 0-1: Audio In, 2-6: Threshold/Ratio/Attack/Release/Makeup Mods, 7: Mix Mod
          .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "CompressorParams", createParameterLayout())
{
    thresholdParam = apvts.getRawParameterValue(paramIdThreshold);
    ratioParam = apvts.getRawParameterValue(paramIdRatio);
    attackParam = apvts.getRawParameterValue(paramIdAttack);
    releaseParam = apvts.getRawParameterValue(paramIdRelease);
    makeupParam = apvts.getRawParameterValue(paramIdMakeup);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    relativeThresholdModParam = apvts.getRawParameterValue("relativeThresholdMod");
    relativeRatioModParam = apvts.getRawParameterValue("relativeRatioMod");
    relativeAttackModParam = apvts.getRawParameterValue("relativeAttackMod");
    relativeReleaseModParam = apvts.getRawParameterValue("relativeReleaseMod");
    relativeMakeupModParam = apvts.getRawParameterValue("relativeMakeupMod");

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R
}

void CompressorModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = 2;

    compressor.prepare(spec);
    compressor.reset();
    smoothedMix.reset(sampleRate, 0.01);
    dryBuffer.setSize(2, samplesPerBlock);
}

void CompressorModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    
    // Get pointers to modulation CV inputs from unified input bus
    const bool isMixMod = isParamInputConnected(paramIdMixMod);
    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);
    
    // SAFE: Read input pointers BEFORE any output operations
    const float* mixCV = isMixMod && inBus.getNumChannels() > 7 ? inBus.getReadPointer(7) : nullptr;
    
    // Get base parameter values ONCE
    const float baseMix = mixParam != nullptr ? mixParam->load() : 1.0f;

    // Copy input to output for in-place processing
    const int numInputChannels = inBus.getNumChannels();
    const int numOutputChannels = outBus.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    float* outL = numOutputChannels > 0 ? outBus.getWritePointer(0) : nullptr;
    float* outR = numOutputChannels > 1 ? outBus.getWritePointer(1) : outL;

    // Capture dry signal for visualization (only audio channels 0/1)
    if (numSamples > dryBuffer.getNumSamples())
        dryBuffer.setSize(2, numSamples, false, false, true);
    dryBuffer.clear();
    if (numInputChannels > 0)
        dryBuffer.copyFrom(0, 0, inBus, 0, 0, numSamples);
    if (numInputChannels > 1)
        dryBuffer.copyFrom(1, 0, inBus, 1, 0, numSamples);
    else
        dryBuffer.copyFrom(1, 0, dryBuffer, 0, 0, numSamples);
    const float* dryL = dryBuffer.getReadPointer(0);
    const float* dryR = dryBuffer.getReadPointer(1);

    if (numInputChannels > 0 && outL != nullptr)
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
    const float baseThreshold = thresholdParam->load();
    const float baseRatio = ratioParam->load();
    const float baseAttack = attackParam->load();
    const float baseRelease = releaseParam->load();
    const float baseMakeup = makeupParam->load();
    const bool relativeThresholdMode = relativeThresholdModParam && relativeThresholdModParam->load() > 0.5f;
    const bool relativeRatioMode = relativeRatioModParam && relativeRatioModParam->load() > 0.5f;
    const bool relativeAttackMode = relativeAttackModParam && relativeAttackModParam->load() > 0.5f;
    const bool relativeReleaseMode = relativeReleaseModParam && relativeReleaseModParam->load() > 0.5f;
    const bool relativeMakeupMode = relativeMakeupModParam && relativeMakeupModParam->load() > 0.5f;
    
    // --- Update DSP Parameters from unified input bus (once per block) ---
    float finalThreshold = baseThreshold;
    if (isParamInputConnected(paramIdThresholdMod) && inBus.getNumChannels() > 2) {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(2, 0));
        if (relativeThresholdMode) {
            // RELATIVE: ±30dB offset
            const float offset = (cv - 0.5f) * 60.0f;
            finalThreshold = baseThreshold + offset;
        } else {
            // ABSOLUTE: CV directly sets threshold
            finalThreshold = juce::jmap(cv, -60.0f, 0.0f);
        }
        finalThreshold = juce::jlimit(-60.0f, 0.0f, finalThreshold);
    }
        
    float finalRatio = baseRatio;
    if (isParamInputConnected(paramIdRatioMod) && inBus.getNumChannels() > 3) {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(3, 0));
        if (relativeRatioMode) {
            // RELATIVE: 0.25x to 4x multiplier
            const float octaveOffset = (cv - 0.5f) * 4.0f;
            finalRatio = baseRatio * std::pow(2.0f, octaveOffset);
        } else {
            // ABSOLUTE: CV directly sets ratio
            finalRatio = juce::jmap(cv, 1.0f, 20.0f);
        }
        finalRatio = juce::jlimit(1.0f, 20.0f, finalRatio);
    }

    float finalAttack = baseAttack;
    if (isParamInputConnected(paramIdAttackMod) && inBus.getNumChannels() > 4) {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(4, 0));
        if (relativeAttackMode) {
            // RELATIVE: 0.25x to 4x time scaling
            const float octaveOffset = (cv - 0.5f) * 4.0f;
            finalAttack = baseAttack * std::pow(2.0f, octaveOffset);
        } else {
            // ABSOLUTE: CV directly sets attack
            finalAttack = juce::jmap(cv, 0.1f, 200.0f);
        }
        finalAttack = juce::jlimit(0.1f, 200.0f, finalAttack);
    }

    float finalRelease = baseRelease;
    if (isParamInputConnected(paramIdReleaseMod) && inBus.getNumChannels() > 5) {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(5, 0));
        if (relativeReleaseMode) {
            // RELATIVE: 0.25x to 4x time scaling
            const float octaveOffset = (cv - 0.5f) * 4.0f;
            finalRelease = baseRelease * std::pow(2.0f, octaveOffset);
        } else {
            // ABSOLUTE: CV directly sets release
            finalRelease = juce::jmap(cv, 5.0f, 1000.0f);
        }
        finalRelease = juce::jlimit(5.0f, 1000.0f, finalRelease);
    }
        
    float finalMakeup = baseMakeup;
    if (isParamInputConnected(paramIdMakeupMod) && inBus.getNumChannels() > 6) {
        const float cv = juce::jlimit(0.0f, 1.0f, inBus.getSample(6, 0));
        if (relativeMakeupMode) {
            // RELATIVE: ±12dB offset
            const float offset = (cv - 0.5f) * 24.0f;
            finalMakeup = baseMakeup + offset;
        } else {
            // ABSOLUTE: CV directly sets makeup gain
            finalMakeup = juce::jmap(cv, -12.0f, 12.0f);
        }
        finalMakeup = juce::jlimit(-12.0f, 12.0f, finalMakeup);
    }

    compressor.setThreshold(finalThreshold);
    compressor.setRatio(finalRatio);
    compressor.setAttack(finalAttack);
    compressor.setRelease(finalRelease);
    
    // --- Process the Audio ---
    juce::dsp::AudioBlock<float> block(outBus);
    juce::dsp::ProcessContextReplacing<float> context(block);
    compressor.process(context);

    // --- Collect visualization metrics before makeup gain ---
    float inputPeak = 0.0f;
    float outputPeakPreMakeup = 0.0f;
    float totalReductionDb = 0.0f;
    float maxReductionDb = 0.0f;
    int reductionSamples = 0;

    if (outL != nullptr && dryL != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float drySampleL = std::abs(dryL[i]);
            const float drySampleR = dryR ? std::abs(dryR[i]) : drySampleL;
            const float wetL = std::abs(outL[i]);
            const float wetR = outR ? std::abs(outR[i]) : wetL;

            const float dry = 0.5f * (drySampleL + drySampleR);
            const float wet = 0.5f * (wetL + wetR);

            inputPeak = std::max(inputPeak, dry);
            outputPeakPreMakeup = std::max(outputPeakPreMakeup, wet);

            if (dry > 1.0e-6f)
            {
                const float ratio = juce::jlimit(1.0e-6f, 1.0f, wet / dry);
                const float reductionDb = -juce::Decibels::gainToDecibels(ratio);
                if (std::isfinite(reductionDb))
                {
                    totalReductionDb += reductionDb;
                    if (reductionDb > maxReductionDb)
                        maxReductionDb = reductionDb;
                    ++reductionSamples;
                }
            }
        }
    }

    const float averageReductionDb = reductionSamples > 0 ? (totalReductionDb / (float)reductionSamples) : 0.0f;
    const float blockReductionDb = juce::jmax(averageReductionDb, maxReductionDb);

    // Apply makeup gain
    const float makeupGain = juce::Decibels::decibelsToGain(finalMakeup);
    outBus.applyGain(makeupGain);
    
    // Apply dry/wet mix
    for (int i = 0; i < numSamples; ++i)
    {
        float currentMix = baseMix;
        if (isMixMod && mixCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[i]);
            currentMix = cv;
        }
        smoothedMix.setTargetValue(currentMix);
        const float mix = smoothedMix.getNextValue();
        const float dry = 1.0f - mix;
        
        if (outL != nullptr && dryL != nullptr) {
            const float wetL = outL[i];
            const float drySampleL = dryL[i];
            outL[i] = dry * drySampleL + mix * wetL;
        }
        if (outR != nullptr && outR != outL && dryR != nullptr) {
            const float wetR = outR[i];
            const float drySampleR = dryR[i];
            outR[i] = dry * drySampleR + mix * wetR;
        }
    }

    // Update visualization data (input/output levels relative to -60..0 dB)
    const float inputDb = juce::Decibels::gainToDecibels(inputPeak, -90.0f);
    const float outputDb = juce::Decibels::gainToDecibels(outputPeakPreMakeup * makeupGain, -90.0f);
    vizData.inputLevelDb.store(inputDb);
    vizData.outputLevelDb.store(outputDb);
    vizData.currentGRDb.store(blockReductionDb);

    const float normalizedGr = juce::jlimit(0.0f, 1.0f, blockReductionDb / 24.0f); // assume up to 24 dB GR
    int writeIdx = vizData.historyWriteIndex.load();
    vizData.inputHistoryDb[writeIdx].store(inputDb);
    vizData.outputHistoryDb[writeIdx].store(outputDb);
    vizData.grHistory[writeIdx].store(normalizedGr);
    writeIdx = (writeIdx + 1) % VizData::historyPoints;
    vizData.historyWriteIndex.store(writeIdx);

    // --- Update UI Telemetry & Tooltips ---
    setLiveParamValue("threshold_live", finalThreshold);
    setLiveParamValue("ratio_live", finalRatio);
    setLiveParamValue("attack_live", finalAttack);
    setLiveParamValue("release_live", finalRelease);
    setLiveParamValue("makeup_live", finalMakeup);
    
    // Update live mix value for telemetry
    if (numSamples > 0) {
        float finalMix = baseMix;
        if (isMixMod && mixCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, mixCV[numSamples - 1]);
            finalMix = cv;
        }
        setLiveParamValue("mix_live", finalMix);
    }
    
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0]) lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1]) lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
    }
}

bool CompressorModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus
    
    if (paramId == paramIdThresholdMod) { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdRatioMod)     { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdAttackMod)    { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdReleaseMod)   { outChannelIndexInBus = 5; return true; }
    if (paramId == paramIdMakeupMod)    { outChannelIndexInBus = 6; return true; }
    if (paramId == paramIdMixMod)       { outChannelIndexInBus = 7; return true; }
    return false;
}

juce::String CompressorModuleProcessor::getAudioInputLabel(int channel) const
{
    if (channel == 0) return "In L";
    if (channel == 1) return "In R";
    // Modulation bus starts at channel 2
    if (channel == 2) return "Thresh Mod";
    if (channel == 3) return "Ratio Mod";
    if (channel == 4) return "Attack Mod";
    if (channel == 5) return "Release Mod";
    if (channel == 6) return "Makeup Mod";
    if (channel == 7) return "Mix Mod";
    return {};
}

juce::String CompressorModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0) return "Out L";
    if (channel == 1) return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void CompressorModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushItemWidth(itemWidth);

    // Helper for tooltips
    auto HelpMarkerComp = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    // Lambda that correctly handles modulation
    auto drawSlider = [&](const char* label, const juce::String& paramId, const juce::String& modId, float min, float max, const char* format, const char* tooltip, int channel = -1) {
        bool isMod = isParamModulated(modId);
        float value = isMod ? getLiveParamValueFor(modId, paramId + juce::String("_live"), ap.getRawParameterValue(paramId)->load())
                            : ap.getRawParameterValue(paramId)->load();
        
        if (isMod) ImGui::BeginDisabled();
        
        // Draw inline pin if channel is specified and pinHelpers is available
        if (channel >= 0 && pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
        
        if (ImGui::SliderFloat(label, &value, min, max, format))
            if (!isMod) *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramId)) = value;
        if (!isMod) adjustParamOnWheel(ap.getParameter(paramId), paramId, value);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        if (tooltip) { ImGui::SameLine(); HelpMarkerComp(tooltip); }
    };

    // === DYNAMICS SECTION ===
    ThemeText("Dynamics", theme.text.section_header);
    ImGui::Spacing();

    drawSlider("Threshold", paramIdThreshold, paramIdThresholdMod, -60.0f, 0.0f, "%.1f dB", "Level above which compression starts (-60 to 0 dB)", 2);  // Channel 2 = Thresh Mod
    drawSlider("Ratio", paramIdRatio, paramIdRatioMod, 1.0f, 20.0f, "%.1f : 1", "Compression ratio (1:1 to 20:1)\n4:1 = moderate, 10:1 = heavy, 20:1 = limiting", 3);  // Channel 3 = Ratio Mod

    ImGui::Spacing();
    ImGui::Spacing();

    // === TIMING SECTION ===
    ThemeText("Timing", theme.text.section_header);
    ImGui::Spacing();

    drawSlider("Attack", paramIdAttack, paramIdAttackMod, 0.1f, 200.0f, "%.1f ms", "How fast compression engages (0.1-200 ms)\nFast = punchy, Slow = smooth", 4);  // Channel 4 = Attack Mod
    drawSlider("Release", paramIdRelease, paramIdReleaseMod, 5.0f, 1000.0f, "%.0f ms", "How fast compression releases (5-1000 ms)\nFast = pumping, Slow = transparent", 5);  // Channel 5 = Release Mod

    ImGui::Spacing();
    ImGui::Spacing();

    // === OUTPUT SECTION ===
    ThemeText("Output", theme.text.section_header);
    ImGui::Spacing();

    drawSlider("Makeup", paramIdMakeup, paramIdMakeupMod, -12.0f, 12.0f, "%.1f dB", "Output gain compensation (-12 to +12 dB)", 6);  // Channel 6 = Makeup Mod
    drawSlider("Mix", paramIdMix, paramIdMixMod, 0.0f, 1.0f, "%.2f", "Wet/dry balance (0=dry, 1=wet)", 7);  // Channel 7 = Mix Mod

    ImGui::Spacing();
    ImGui::Spacing();

    // === Compressor Activity Visualization ===
    ThemeText("Compressor Activity", theme.text.section_header);
    ImGui::Spacing();

    auto* drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
    const ImU32 grColor = ImGui::ColorConvertFloat4ToU32(theme.accent);

    const float timelineHeight = 60.0f;
    const ImVec2 timelineOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 timelineRectMax = ImVec2(timelineOrigin.x + itemWidth, timelineOrigin.y + timelineHeight);
    drawList->AddRectFilled(timelineOrigin, timelineRectMax, bgColor, 4.0f);
    ImGui::PushClipRect(timelineOrigin, timelineRectMax, true);

    const int writeIdx = vizData.historyWriteIndex.load();
    const float stepX = itemWidth / static_cast<float>(VizData::historyPoints - 1);
    auto drawTimeline = [&](const std::array<std::atomic<float>, VizData::historyPoints>& history, ImU32 color)
    {
        float prevX = timelineOrigin.x;
        float prevY = timelineRectMax.y;
        for (int i = 0; i < VizData::historyPoints; ++i)
        {
            const int idx = (writeIdx + i) % VizData::historyPoints;
            const float val = juce::jlimit(-60.0f, 0.0f, history[idx].load());
            const float normalized = juce::jmap(val, -60.0f, 0.0f, 0.0f, 1.0f);
            const float x = timelineOrigin.x + i * stepX;
            const float y = timelineRectMax.y - normalized * (timelineHeight - 16.0f) - 8.0f;
            if (i > 0)
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), color, 2.0f);
            prevX = x;
            prevY = y;
        }
    };

    drawTimeline(vizData.inputHistoryDb, inputColor);
    drawTimeline(vizData.outputHistoryDb, outputColor);

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(timelineOrigin.x, timelineRectMax.y));
    ImGui::Dummy(ImVec2(itemWidth, 0));

    const float grHeight = 40.0f;
    const ImVec2 grOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 grRectMax = ImVec2(grOrigin.x + itemWidth, grOrigin.y + grHeight);
    drawList->AddRectFilled(grOrigin, grRectMax, bgColor, 4.0f);
    ImGui::PushClipRect(grOrigin, grRectMax, true);

    float prevX = grOrigin.x;
    float prevY = grRectMax.y - 8.0f;
    for (int i = 0; i < VizData::historyPoints; ++i)
    {
        const int idx = (writeIdx + i) % VizData::historyPoints;
        const float val = vizData.grHistory[idx].load();
        const float x = grOrigin.x + i * stepX;
        const float y = grRectMax.y - val * (grHeight - 16.0f) - 8.0f;
        if (i > 0)
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), grColor, 2.0f);
        prevX = x;
        prevY = y;
    }

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(grOrigin.x, grRectMax.y));
    ImGui::Dummy(ImVec2(itemWidth, 0));

    const float inputDb = vizData.inputLevelDb.load();
    const float outputDb = vizData.outputLevelDb.load();
    const float grDb = vizData.currentGRDb.load();
    
    ImGui::Text("Input: %.1f dB  |  Output: %.1f dB", inputDb, outputDb);
    ImGui::Text("Reduction: %.1f dB", grDb);

    ImGui::Spacing();
    ImGui::Spacing();

    // === RELATIVE MODULATION SECTION ===
    ThemeText("CV Input Modes", theme.modulation.frequency);
    ImGui::Spacing();
    
    // Relative Threshold Mod checkbox
    bool relativeThresholdMod = relativeThresholdModParam != nullptr && relativeThresholdModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Threshold Mod", &relativeThresholdMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeThresholdMod")))
            *p = relativeThresholdMod;
        juce::Logger::writeToLog("[Compressor UI] Relative Threshold Mod: " + juce::String(relativeThresholdMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±30dB)\nOFF: CV directly sets threshold (-60dB to 0dB)");
    }
    
    // Relative Ratio Mod checkbox
    bool relativeRatioMod = relativeRatioModParam != nullptr && relativeRatioModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Ratio Mod", &relativeRatioMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeRatioMod")))
            *p = relativeRatioMod;
        juce::Logger::writeToLog("[Compressor UI] Relative Ratio Mod: " + juce::String(relativeRatioMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (0.25x to 4x)\nOFF: CV directly sets ratio (1:1 to 20:1)");
    }
    
    // Relative Attack Mod checkbox
    bool relativeAttackMod = relativeAttackModParam != nullptr && relativeAttackModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Attack Mod", &relativeAttackMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeAttackMod")))
            *p = relativeAttackMod;
        juce::Logger::writeToLog("[Compressor UI] Relative Attack Mod: " + juce::String(relativeAttackMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (0.25x to 4x)\nOFF: CV directly sets attack (0.1-200ms)");
    }
    
    // Relative Release Mod checkbox
    bool relativeReleaseMod = relativeReleaseModParam != nullptr && relativeReleaseModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Release Mod", &relativeReleaseMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeReleaseMod")))
            *p = relativeReleaseMod;
        juce::Logger::writeToLog("[Compressor UI] Relative Release Mod: " + juce::String(relativeReleaseMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (0.25x to 4x)\nOFF: CV directly sets release (5-1000ms)");
    }
    
    // Relative Makeup Mod checkbox
    bool relativeMakeupMod = relativeMakeupModParam != nullptr && relativeMakeupModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Makeup Mod", &relativeMakeupMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMakeupMod")))
            *p = relativeMakeupMod;
        juce::Logger::writeToLog("[Compressor UI] Relative Makeup Mod: " + juce::String(relativeMakeupMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±12dB)\nOFF: CV directly sets makeup (-12dB to +12dB)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void CompressorModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-7) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Thresh Mod (ch 2), Ratio Mod (ch 3), Attack Mod (ch 4), Release Mod (ch 5), Makeup Mod (ch 6), Mix Mod (ch 7) are drawn inline in drawParametersInNode
}
#endif

std::vector<DynamicPinInfo> CompressorModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});
    
    // Modulation inputs (channels 2-7)
    pins.push_back({"Thresh Mod", 2, PinDataType::CV});
    pins.push_back({"Ratio Mod", 3, PinDataType::CV});
    pins.push_back({"Attack Mod", 4, PinDataType::CV});
    pins.push_back({"Release Mod", 5, PinDataType::CV});
    pins.push_back({"Makeup Mod", 6, PinDataType::CV});
    pins.push_back({"Mix Mod", 7, PinDataType::CV});
    
    return pins;
}

std::vector<DynamicPinInfo> CompressorModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});
    
    return pins;
}

