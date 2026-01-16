#include "PhaserModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

juce::AudioProcessorValueTreeState::ParameterLayout PhaserModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdRate, "Rate", 0.01f, 10.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdDepth, "Depth", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdCentreHz, "Centre Freq",
        juce::NormalisableRange<float>(20.0f, 10000.0f, 1.0f, 0.25f), 1000.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdFeedback, "Feedback", -0.95f, 0.95f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdMix, "Mix", 0.0f, 1.0f, 0.5f));
    
    // Relative modulation parameters
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeRateMod", "Relative Rate Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeDepthMod", "Relative Depth Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeCentreMod", "Relative Centre Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeFeedbackMod", "Relative Feedback Mod", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>("relativeMixMod", "Relative Mix Mod", true));
    
    return { params.begin(), params.end() };
}

PhaserModuleProcessor::PhaserModuleProcessor()
    : ModuleProcessor(BusesProperties()
          .withInput("Inputs", juce::AudioChannelSet::discreteChannels(7), true) // 0-1: Audio In, 2: Rate Mod, 3: Depth Mod, 4: Centre Mod, 5: Feedback Mod, 6: Mix Mod
          .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PhaserParams", createParameterLayout())
{
    rateParam = apvts.getRawParameterValue(paramIdRate);
    depthParam = apvts.getRawParameterValue(paramIdDepth);
    centreHzParam = apvts.getRawParameterValue(paramIdCentreHz);
    feedbackParam = apvts.getRawParameterValue(paramIdFeedback);
    mixParam = apvts.getRawParameterValue(paramIdMix);
    relativeRateModParam = apvts.getRawParameterValue("relativeRateMod");
    relativeDepthModParam = apvts.getRawParameterValue("relativeDepthMod");
    relativeCentreModParam = apvts.getRawParameterValue("relativeCentreMod");
    relativeFeedbackModParam = apvts.getRawParameterValue("relativeFeedbackMod");
    relativeMixModParam = apvts.getRawParameterValue("relativeMixMod");

    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R
    
    // Initialize LFO phase accumulator
    lfoPhaseAccumulator = 0.0;
}

void PhaserModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = 2; // Process in stereo

    phaser.prepare(spec);
    phaser.reset();
    
    tempBuffer.setSize(2, samplesPerBlock);
}

void PhaserModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    
    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    // Copy dry input to the output buffer to start
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

    // --- Get Modulation CVs from unified input bus ---
    auto readCv = [&](const juce::String& paramId, int channelIndex) -> float {
        if (isParamInputConnected(paramId) && inBus.getNumChannels() > channelIndex) {
            return inBus.getReadPointer(channelIndex)[0]; // Read first sample
        }
        return -1.0f; // Use a sentinel to indicate no CV
    };

    float rateCv = readCv(paramIdRateMod, 2);
    float depthCv = readCv(paramIdDepthMod, 3);
    float centreCv = readCv(paramIdCentreHzMod, 4);
    float feedbackCv = readCv(paramIdFeedbackMod, 5);
    float mixCv = readCv(paramIdMixMod, 6);

    // --- Get base values and relative modes ---
    const float baseRate = rateParam->load();
    const float baseDepth = depthParam->load();
    const float baseCentre = centreHzParam->load();
    const float baseFeedback = feedbackParam->load();
    const float baseMix = mixParam->load();
    const bool relativeRateMode = relativeRateModParam && relativeRateModParam->load() > 0.5f;
    const bool relativeDepthMode = relativeDepthModParam && relativeDepthModParam->load() > 0.5f;
    const bool relativeCentreMode = relativeCentreModParam && relativeCentreModParam->load() > 0.5f;
    const bool relativeFeedbackMode = relativeFeedbackModParam && relativeFeedbackModParam->load() > 0.5f;
    const bool relativeMixMode = relativeMixModParam && relativeMixModParam->load() > 0.5f;

    // --- Update DSP Parameters (once per block) ---
    float finalRate = baseRate;
    if (rateCv >= 0.0f) {
        const float cv = juce::jlimit(0.0f, 1.0f, rateCv);
        if (relativeRateMode) {
            // RELATIVE: ±2 octaves (0.25x to 4x)
            const float octaveOffset = (cv - 0.5f) * 4.0f;
            finalRate = baseRate * std::pow(2.0f, octaveOffset);
        } else {
            // ABSOLUTE: CV directly sets rate (0.01-10 Hz)
            finalRate = juce::jmap(cv, 0.01f, 10.0f);
        }
    }
    
    float finalDepth = baseDepth;
    if (depthCv >= 0.0f) {
        const float cv = juce::jlimit(0.0f, 1.0f, depthCv);
        if (relativeDepthMode) {
            // RELATIVE: ±0.5 offset
            const float offset = (cv - 0.5f) * 1.0f;
            finalDepth = baseDepth + offset;
        } else {
            // ABSOLUTE: CV directly sets depth
            finalDepth = cv;
        }
        finalDepth = juce::jlimit(0.0f, 1.0f, finalDepth);
    }
    
    float finalCentre = baseCentre;
    if (centreCv >= 0.0f) {
        const float cv = juce::jlimit(0.0f, 1.0f, centreCv);
        if (relativeCentreMode) {
            // RELATIVE: ±4 octaves around base frequency
            const float octaveOffset = (cv - 0.5f) * 8.0f;
            finalCentre = baseCentre * std::pow(2.0f, octaveOffset);
        } else {
            // ABSOLUTE: CV directly sets frequency (20-10000 Hz)
            finalCentre = juce::jmap(cv, 20.0f, 10000.0f);
        }
        finalCentre = juce::jlimit(20.0f, 10000.0f, finalCentre);
    }
    
    float finalFeedback = baseFeedback;
    if (feedbackCv >= 0.0f) {
        const float cv = juce::jlimit(0.0f, 1.0f, feedbackCv);
        if (relativeFeedbackMode) {
            // RELATIVE: ±0.5 offset
            const float offset = (cv - 0.5f) * 1.0f;
            finalFeedback = baseFeedback + offset;
        } else {
            // ABSOLUTE: CV directly sets feedback
            finalFeedback = juce::jmap(cv, -0.95f, 0.95f);
        }
        finalFeedback = juce::jlimit(-0.95f, 0.95f, finalFeedback);
    }
    
    float finalMix = baseMix;
    if (mixCv >= 0.0f) {
        const float cv = juce::jlimit(0.0f, 1.0f, mixCv);
        if (relativeMixMode) {
            // RELATIVE: ±0.5 offset
            const float offset = (cv - 0.5f) * 1.0f;
            finalMix = baseMix + offset;
        } else {
            // ABSOLUTE: CV directly sets mix
            finalMix = cv;
        }
        finalMix = juce::jlimit(0.0f, 1.0f, finalMix);
    }

    phaser.setRate(finalRate);
    phaser.setDepth(finalDepth);
    phaser.setCentreFrequency(finalCentre);
    phaser.setFeedback(finalFeedback);
    
    // --- Update LFO Phase for Visualization ---
    const double sampleRate = getSampleRate();
    if (sampleRate > 0.0)
    {
        const double phaseIncrement = finalRate / sampleRate;
        lfoPhaseAccumulator += phaseIncrement * numSamples;
        while (lfoPhaseAccumulator >= 1.0)
            lfoPhaseAccumulator -= 1.0;
        
        // Update visualization data (throttled - every block)
        vizData.lfoPhase.store((float)lfoPhaseAccumulator);
        vizData.currentRate.store(finalRate);
        vizData.currentDepth.store(finalDepth);
        vizData.currentCentre.store(finalCentre);
        vizData.currentFeedback.store(finalFeedback);
        vizData.currentMix.store(finalMix);
        
        // Calculate approximate frequency response
        // Phaser creates notches at frequencies determined by the LFO phase
        // The centre frequency sweeps based on depth and phase
        const float lfoPhase = (float)lfoPhaseAccumulator;
        const float lfoValue = std::sin(lfoPhase * juce::MathConstants<float>::twoPi); // -1 to 1
        const float sweepRange = finalDepth * 0.5f; // Depth controls sweep range
        const float currentSweepFreq = finalCentre * std::pow(2.0f, lfoValue * sweepRange * 2.0f);
        
        // Calculate frequency response points (logarithmic scale, 20 Hz to 20 kHz)
        for (int i = 0; i < VizData::frequencyPoints; ++i)
        {
            // Logarithmic frequency scale
            const float freqLin = std::pow(10.0f, std::log10(20.0f) + (std::log10(20000.0f) - std::log10(20.0f)) * (float)i / (float)(VizData::frequencyPoints - 1));
            
            // Approximate phaser response: notches near the sweep frequency
            // Phaser has 6 stages, creating multiple notches
            float response = 1.0f;
            const float distFromSweep = std::abs(std::log2(freqLin / currentSweepFreq));
            
            // Create notch effect (simplified model)
            if (distFromSweep < 0.5f) // Within half octave of sweep frequency
            {
                const float notchDepth = finalDepth * (1.0f - distFromSweep * 2.0f);
                response = 1.0f - notchDepth * 0.7f; // Notch reduces magnitude
            }
            
            // Feedback adds resonance peaks
            if (finalFeedback > 0.0f)
            {
                const float resonanceDist = std::abs(std::log2(freqLin / (currentSweepFreq * 1.5f)));
                if (resonanceDist < 0.3f)
                {
                    response += finalFeedback * 0.3f * (1.0f - resonanceDist / 0.3f);
                }
            }
            
            response = juce::jlimit(0.0f, 2.0f, response); // Clamp to reasonable range
            vizData.frequencyResponse[i].store(response);
        }
    }
    
    // --- Process the Audio with Dry/Wet Mix ---
    // The JUCE Phaser's built-in mix is not ideal for this use case.
    // We'll implement a manual dry/wet mix for better results, like in VoiceProcessor.cpp
    tempBuffer.makeCopyOf(outBus); // Copy the dry signal

    juce::dsp::AudioBlock<float> block(tempBuffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    phaser.process(context); // Process to get the fully wet signal

    // Manually blend the original dry signal (in outBus) with the wet signal (in tempBuffer)
    for (int ch = 0; ch < numChannels; ++ch)
    {
        outBus.applyGain(ch, 0, buffer.getNumSamples(), 1.0f - finalMix);
        outBus.addFrom(ch, 0, tempBuffer, ch, 0, buffer.getNumSamples(), finalMix);
    }
    
    // --- Update UI Telemetry ---
    setLiveParamValue("rate_live", finalRate);
    setLiveParamValue("depth_live", finalDepth);
    setLiveParamValue("centreHz_live", finalCentre);
    setLiveParamValue("feedback_live", finalFeedback);
    setLiveParamValue("mix_live", finalMix);

    // --- Update Tooltips ---
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0]) lastOutputValues[0]->store(outBus.getSample(0, buffer.getNumSamples() - 1));
        if (lastOutputValues[1]) lastOutputValues[1]->store(outBus.getSample(1, buffer.getNumSamples() - 1));
    }
}

bool PhaserModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus
    
    if (paramId == paramIdRateMod) { outChannelIndexInBus = 2; return true; }
    if (paramId == paramIdDepthMod) { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdCentreHzMod) { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdFeedbackMod) { outChannelIndexInBus = 5; return true; }
    if (paramId == paramIdMixMod) { outChannelIndexInBus = 6; return true; }
    return false;
}

juce::String PhaserModuleProcessor::getAudioInputLabel(int channel) const
{
    if (channel == 0) return "In L";
    if (channel == 1) return "In R";
    if (channel == 2) return "Rate Mod";
    if (channel == 3) return "Depth Mod";
    if (channel == 4) return "Centre Mod";
    if (channel == 5) return "Feedback Mod";
    if (channel == 6) return "Mix Mod";
    return {};
}

juce::String PhaserModuleProcessor::getAudioOutputLabel(int channel) const
{
    if (channel == 0) return "Out L";
    if (channel == 1) return "Out R";
    return {};
}

#if defined(PRESET_CREATOR_UI)
void PhaserModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String&)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    auto HelpMarker = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f); ImGui::TextUnformatted(desc); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    };

    auto drawSlider = [&](const char* label, const juce::String& paramId, const juce::String& modId, float min, float max, const char* format, const char* tooltip, ImGuiSliderFlags flags = 0, int channel = -1)
    {
        bool isMod = isParamModulated(modId);
        float value = isMod ? getLiveParamValueFor(modId, paramId + "_live", ap.getRawParameterValue(paramId)->load())
                            : ap.getRawParameterValue(paramId)->load();
        
        if (isMod) ImGui::BeginDisabled();
        
        // Draw inline pin if channel is specified and pinHelpers is available
        if (channel >= 0 && pinHelpers && pinHelpers->drawInlineInputPin)
        {
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
        
        if (ImGui::SliderFloat(label, &value, min, max, format, flags))
            if (!isMod) *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramId)) = value;
        if (!isMod) adjustParamOnWheel(ap.getParameter(paramId), paramId, value);
        if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
        if (isMod) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
        if (tooltip) { ImGui::SameLine(); HelpMarker(tooltip); }
    };

    ThemeText("Phaser Parameters", theme.text.section_header);
    ImGui::Spacing();

    // === VISUALIZATION ===
    const float vizHeight = 120.0f;
    const float vizWidth = itemWidth;
    ImVec2 vizOrigin = ImGui::GetCursorScreenPos();
    ImVec2 vizRectMax(vizOrigin.x + vizWidth, vizOrigin.y + vizHeight);
    
    auto* drawList = ImGui::GetWindowDrawList();
    
    // Color resolution helper (from Granulator pattern)
    auto resolveColor = [](ImU32 primary, ImU32 fallback1, ImU32 fallback2) -> ImU32 {
        return (primary != 0) ? primary : ((fallback1 != 0) ? fallback1 : fallback2);
    };
    
    // Background: scope_plot_bg -> canvas_background -> ChildBg -> fallback
    const ImU32 canvasBg = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ChildBg]);
    const ImU32 bgColor = resolveColor(theme.modules.scope_plot_bg, canvasBg, IM_COL32(20, 20, 25, 255));
    
    // Frequency response line (cyan/blue)
    const ImVec4 frequencyColorVec4 = theme.modulation.frequency;
    const ImU32 frequencyColor = ImGui::ColorConvertFloat4ToU32(ImVec4(frequencyColorVec4.x, frequencyColorVec4.y, frequencyColorVec4.z, 0.9f));
    
    // LFO phase indicator (orange/yellow)
    const ImVec4 timbreColorVec4 = theme.modulation.timbre;
    const ImU32 lfoColor = ImGui::ColorConvertFloat4ToU32(ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, 1.0f));
    
    // Centre frequency marker (magenta/pink)
    const ImVec4 amplitudeColorVec4 = theme.modulation.amplitude;
    const ImU32 centreMarkerColor = ImGui::ColorConvertFloat4ToU32(ImVec4(amplitudeColorVec4.x, amplitudeColorVec4.y, amplitudeColorVec4.z, 0.8f));
    
    drawList->AddRectFilled(vizOrigin, vizRectMax, bgColor, 4.0f);
    ImGui::PushClipRect(vizOrigin, vizRectMax, true);
    
    // Read visualization data (thread-safe)
    float frequencyResponse[VizData::frequencyPoints];
    for (int i = 0; i < VizData::frequencyPoints; ++i)
    {
        frequencyResponse[i] = vizData.frequencyResponse[i].load();
    }
    const float lfoPhase = vizData.lfoPhase.load();
    const float currentCentre = vizData.currentCentre.load();
    const float currentDepth = vizData.currentDepth.load();
    
    // Draw frequency response graph (logarithmic frequency axis)
    const float midY = vizOrigin.y + vizHeight * 0.5f;
    const float scaleY = vizHeight * 0.4f;
    const float stepX = vizWidth / (float)(VizData::frequencyPoints - 1);
    
    // Draw center line (0 dB reference)
    drawList->AddLine(ImVec2(vizOrigin.x, midY), ImVec2(vizRectMax.x, midY), IM_COL32(100, 100, 100, 80), 1.0f);
    
    // Draw frequency response curve
    float prevX = vizOrigin.x, prevY = midY;
    for (int i = 0; i < VizData::frequencyPoints; ++i)
    {
        const float response = frequencyResponse[i];
        const float x = vizOrigin.x + i * stepX;
        // Map response: 0.0 = -scaleY, 1.0 = midY, 2.0 = +scaleY
        const float y = midY - (response - 1.0f) * scaleY;
        if (i > 0) drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), frequencyColor, 2.0f);
        prevX = x; prevY = y;
    }
    
    // Draw centre frequency marker (vertical line)
    const float centreFreqLog = std::log10(juce::jlimit(20.0f, 20000.0f, currentCentre));
    const float centreFreqNorm = (centreFreqLog - std::log10(20.0f)) / (std::log10(20000.0f) - std::log10(20.0f));
    const float centreX = vizOrigin.x + centreFreqNorm * vizWidth;
    drawList->AddLine(ImVec2(centreX, vizOrigin.y), ImVec2(centreX, vizRectMax.y), centreMarkerColor, 1.5f);
    
    // Draw LFO phase indicator (circular at top-right)
    const float lfoRadius = 12.0f;
    const float lfoCenterX = vizRectMax.x - lfoRadius - 5.0f;
    const float lfoCenterY = vizOrigin.y + lfoRadius + 5.0f;
    
    // Draw LFO circle background
    drawList->AddCircleFilled(ImVec2(lfoCenterX, lfoCenterY), lfoRadius, IM_COL32(40, 40, 45, 200), 16);
    drawList->AddCircle(ImVec2(lfoCenterX, lfoCenterY), lfoRadius, IM_COL32(100, 100, 100, 150), 16, 1.5f);
    
    // Draw LFO phase dot (rotates around circle)
    const float lfoAngle = lfoPhase * juce::MathConstants<float>::twoPi - juce::MathConstants<float>::halfPi; // Start at top
    const float dotX = lfoCenterX + std::cos(lfoAngle) * (lfoRadius - 2.0f);
    const float dotY = lfoCenterY + std::sin(lfoAngle) * (lfoRadius - 2.0f);
    drawList->AddCircleFilled(ImVec2(dotX, dotY), 3.0f, lfoColor, 8);
    
    // Draw LFO trail (shows recent phase history)
    const int trailPoints = 8;
    for (int i = 0; i < trailPoints; ++i)
    {
        const float trailPhase = lfoPhase - (float)i * 0.05f;
        if (trailPhase < 0.0f) continue;
        const float trailAngle = trailPhase * juce::MathConstants<float>::twoPi - juce::MathConstants<float>::halfPi;
        const float trailX = lfoCenterX + std::cos(trailAngle) * (lfoRadius - 2.0f);
        const float trailY = lfoCenterY + std::sin(trailAngle) * (lfoRadius - 2.0f);
        const float alpha = 1.0f - (float)i / (float)trailPoints;
        const ImU32 trailColor = ImGui::ColorConvertFloat4ToU32(ImVec4(timbreColorVec4.x, timbreColorVec4.y, timbreColorVec4.z, alpha * 0.5f));
        drawList->AddCircleFilled(ImVec2(trailX, trailY), 2.0f, trailColor, 6);
    }
    
    ImGui::PopClipRect();
    
    // Move cursor past visualization
    ImGui::SetCursorScreenPos(ImVec2(vizOrigin.x, vizRectMax.y + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(vizWidth, 0));
    ImGui::Spacing();
    
    // Info text below visualization
    const float currentRate = vizData.currentRate.load();
    ImGui::TextDisabled("LFO: %.2f Hz | Centre: %.0f Hz | Depth: %.0f%%", 
                        currentRate, currentCentre, currentDepth * 100.0f);
    ImGui::Spacing();

    drawSlider("Rate", paramIdRate, paramIdRateMod, 0.01f, 10.0f, "%.2f Hz", "LFO sweep rate (0.01-10 Hz)", 0, 2);  // Channel 2 = Rate Mod
    drawSlider("Depth", paramIdDepth, paramIdDepthMod, 0.0f, 1.0f, "%.2f", "Modulation depth (0-1)", 0, 3);  // Channel 3 = Depth Mod
    drawSlider("Centre", paramIdCentreHz, paramIdCentreHzMod, 20.0f, 10000.0f, "%.0f Hz", "Center frequency of phase shift", ImGuiSliderFlags_Logarithmic, 4);  // Channel 4 = Centre Mod
    drawSlider("Feedback", paramIdFeedback, paramIdFeedbackMod, -0.95f, 0.95f, "%.2f", "Feedback amount\nNegative = darker, Positive = brighter", 0, 5);  // Channel 5 = Feedback Mod
    drawSlider("Mix", paramIdMix, paramIdMixMod, 0.0f, 1.0f, "%.2f", "Dry/wet mix (0-1)", 0, 6);  // Channel 6 = Mix Mod

    ImGui::Spacing();
    ImGui::Spacing();

    // === RELATIVE MODULATION SECTION ===
    ThemeText("CV Input Modes", theme.modulation.frequency);
    ImGui::Spacing();
    
    // Relative Rate Mod checkbox
    bool relativeRateMod = relativeRateModParam != nullptr && relativeRateModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Rate Mod", &relativeRateMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeRateMod")))
            *p = relativeRateMod;
        juce::Logger::writeToLog("[Phaser UI] Relative Rate Mod: " + juce::String(relativeRateMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±2 octaves)\nOFF: CV directly sets rate (0.01-10 Hz)");
    }
    
    // Relative Depth Mod checkbox
    bool relativeDepthMod = relativeDepthModParam != nullptr && relativeDepthModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Depth Mod", &relativeDepthMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeDepthMod")))
            *p = relativeDepthMod;
        juce::Logger::writeToLog("[Phaser UI] Relative Depth Mod: " + juce::String(relativeDepthMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets depth (0-1)");
    }
    
    // Relative Centre Mod checkbox
    bool relativeCentreMod = relativeCentreModParam != nullptr && relativeCentreModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Centre Mod", &relativeCentreMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeCentreMod")))
            *p = relativeCentreMod;
        juce::Logger::writeToLog("[Phaser UI] Relative Centre Mod: " + juce::String(relativeCentreMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±4 octaves)\nOFF: CV directly sets freq (20-10000 Hz)");
    }
    
    // Relative Feedback Mod checkbox
    bool relativeFeedbackMod = relativeFeedbackModParam != nullptr && relativeFeedbackModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Feedback Mod", &relativeFeedbackMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeFeedbackMod")))
            *p = relativeFeedbackMod;
        juce::Logger::writeToLog("[Phaser UI] Relative Feedback Mod: " + juce::String(relativeFeedbackMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets feedback (-0.95 to 0.95)");
    }
    
    // Relative Mix Mod checkbox
    bool relativeMixMod = relativeMixModParam != nullptr && relativeMixModParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Mix Mod", &relativeMixMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeMixMod")))
            *p = relativeMixMod;
        juce::Logger::writeToLog("[Phaser UI] Relative Mix Mod: " + juce::String(relativeMixMod ? "ON" : "OFF"));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("ON: CV modulates around slider (±0.5)\nOFF: CV directly sets mix (0-1)");
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void PhaserModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins (channels 2-6) are now inline
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    // Rate Mod (ch 2), Depth Mod (ch 3), Centre Mod (ch 4), Feedback Mod (ch 5), Mix Mod (ch 6) are drawn inline in drawParametersInNode
}
#endif

std::vector<DynamicPinInfo> PhaserModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio inputs (channels 0-1)
    pins.push_back({"In L", 0, PinDataType::Audio});
    pins.push_back({"In R", 1, PinDataType::Audio});
    
    // Modulation inputs (channels 2-6)
    pins.push_back({"Rate Mod", 2, PinDataType::CV});
    pins.push_back({"Depth Mod", 3, PinDataType::CV});
    pins.push_back({"Centre Mod", 4, PinDataType::CV});
    pins.push_back({"Feedback Mod", 5, PinDataType::CV});
    pins.push_back({"Mix Mod", 6, PinDataType::CV});
    
    return pins;
}

std::vector<DynamicPinInfo> PhaserModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;
    
    // Audio outputs (channels 0-1)
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});
    
    return pins;
}

std::optional<RhythmInfo> PhaserModuleProcessor::getRhythmInfo() const
{
    RhythmInfo info;
    
    // Build display name with logical ID
    info.displayName = "Phaser #" + juce::String(getLogicalId());
    info.sourceType = "phaser";
    
    // Phaser LFO is free-running (not synced to transport)
    info.isSynced = false;
    info.isActive = true; // Always active when processing
    
    // Convert LFO rate (Hz) to BPM
    // Rate is in cycles per second (Hz), one cycle = one "beat"
    const float rate = rateParam ? rateParam->load() : 0.5f;
    info.bpm = rate * 60.0f; // Convert Hz to BPM
    
    // Validate BPM before returning
    if (!std::isfinite(info.bpm))
        info.bpm = 0.0f;
    
    return info;
}

