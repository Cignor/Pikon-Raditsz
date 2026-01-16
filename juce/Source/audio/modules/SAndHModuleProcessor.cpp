#include "SAndHModuleProcessor.h"
#include <juce_core/juce_core.h>

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

juce::AudioProcessorValueTreeState::ParameterLayout SAndHModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    // Threshold: 0.0 to 1.0 (for trigger detection)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdThreshold, "Threshold",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.5f));
    
    // Edge: Rising, Falling, Both
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdEdge, "Edge",
        juce::StringArray{ "Rising", "Falling", "Both" },
        0));
    
    // Slew: 0.0 to 1.0 (smoothing amount)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdSlew, "Slew",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 0.5f),
        0.0f));
    
    // Mode: Classic S&H, Track & Hold
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        paramIdMode, "Mode",
        juce::StringArray{ "Sample & Hold", "Track & Hold" },
        0));
    
    // Modulation parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdThresholdMod, "Threshold Mod",
        0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdEdgeMod, "Edge Mod",
        0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        paramIdSlewMod, "Slew Mod",
        0.0f, 1.0f, 0.0f));
    
    return { params.begin(), params.end() };
}

SAndHModuleProcessor::SAndHModuleProcessor()
    : ModuleProcessor(BusesProperties()
                        .withInput("Inputs", juce::AudioChannelSet::discreteChannels(6), true) // ch0-1: Audio, ch2: Trigger, ch3-5: CV mods
                        .withOutput("Outputs", juce::AudioChannelSet::discreteChannels(4), true)), // ch0-1: Audio, ch2: Smoothed, ch3: Trigger
      apvts(*this, nullptr, "SAndHParams", createParameterLayout())
{
    thresholdParam = apvts.getRawParameterValue(paramIdThreshold);
    edgeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(paramIdEdge));
    slewParam = apvts.getRawParameterValue(paramIdSlew);
    modeParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(paramIdMode));
    
    // Initialize output value tracking
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

void SAndHModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    heldValue = 0.0f;
    smoothedValue = 0.0f;
    lastTriggerValue = 0.0f;
    wasTriggerHigh = false;
    
    // Initialize slew smoother
    slewSmoother.reset(sampleRate, 0.01);
    slewSmoother.setCurrentAndTargetValue(0.0f);
    lastSlewTimeSec = 0.01f;
    
    // Set initial edge type
    if (edgeParam)
        currentEdgeType = static_cast<EdgeType>(edgeParam->getIndex());

#if defined(PRESET_CREATOR_UI)
    vizInputBuffer.setSize(1, samplesPerBlock);
    vizOutputBuffer.setSize(1, samplesPerBlock);
    vizTriggerBuffer.setSize(1, samplesPerBlock);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
    vizTriggerBuffer.clear();
#endif
}

void SAndHModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    
    // CRITICAL: Get input bus BEFORE any output operations (buffer aliasing safety)
    // Follow BitCrusher pattern: single bus with discreteChannels
    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);
    
    const int numSamples = buffer.getNumSamples();
    
    // Get base parameter values
    float baseThreshold = thresholdParam ? thresholdParam->load() : 0.5f;
    float baseSlew = slewParam ? slewParam->load() : 0.0f;
    const int mode = modeParam ? modeParam->getIndex() : 0;
    
    // DEBUG: Log bus status
    static int blockCounter = 0;
    static bool firstBlock = true;
    if (firstBlock || blockCounter % 100 == 0) // Log first block and every 100 blocks
    {
        juce::Logger::writeToLog("[S&H] Block #" + juce::String(blockCounter) + 
            " | inChannels=" + juce::String(inBus.getNumChannels()) + 
            " | outChannels=" + juce::String(outBus.getNumChannels()) +
            " | samples=" + juce::String(numSamples) +
            " | mode=" + juce::String(mode) +
            " | threshold=" + juce::String(baseThreshold, 3));
        firstBlock = false;
    }
    blockCounter++;
    
    // ✅ CRITICAL FIX: Read CV inputs BEFORE any output operations (like BitCrusher)
    // Check for modulation
    const bool isThresholdModulated = isParamInputConnected(paramIdThresholdMod);
    const bool isEdgeModulated = isParamInputConnected(paramIdEdgeMod);
    const bool isSlewModulated = isParamInputConnected(paramIdSlewMod);
    
    // Read input pointers from single bus (ch0-1: Audio, ch2: Trigger, ch3-5: CV mods)
    // Follow BitCrusher pattern: read from discrete channels on same bus
    const float* signalL = inBus.getNumChannels() > 0 ? inBus.getReadPointer(0) : nullptr;
    const float* signalR = inBus.getNumChannels() > 1 ? inBus.getReadPointer(1) : nullptr;
    const float* trigger = inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;
    const float* thresholdMod = (isThresholdModulated && inBus.getNumChannels() > 3) ? inBus.getReadPointer(3) : nullptr;
    const float* edgeMod = (isEdgeModulated && inBus.getNumChannels() > 4) ? inBus.getReadPointer(4) : nullptr;
    const float* slewMod = (isSlewModulated && inBus.getNumChannels() > 5) ? inBus.getReadPointer(5) : nullptr;
    
    // DEBUG: Log raw input sample values (first few samples)
    if (blockCounter % 100 == 0 && numSamples > 0)
    {
        juce::String inputSamples = "[S&H] Raw input samples: ";
        for (int i = 0; i < juce::jmin(5, numSamples); ++i)
        {
            if (signalL) inputSamples += "L[" + juce::String(i) + "]=" + juce::String(signalL[i], 4) + " ";
            if (signalR) inputSamples += "R[" + juce::String(i) + "]=" + juce::String(signalR[i], 4) + " ";
        }
        juce::Logger::writeToLog(inputSamples);
    }
    
    // ✅ CRITICAL: Copy input to output BEFORE getting write pointers (like BitCrusher)
    // This prevents buffer aliasing issues - input might be in the same buffer as output
    if (inBus.getNumChannels() > 0 && outBus.getNumChannels() > 0)
    {
        // Copy audio input to output (ch0-1) - preserve input data before any clearing
        const int channelsToCopy = juce::jmin(inBus.getNumChannels(), 2, outBus.getNumChannels());
        for (int ch = 0; ch < channelsToCopy; ++ch)
        {
            outBus.copyFrom(ch, 0, inBus, ch, 0, numSamples);
        }
    }
    else
    {
        // No input - clear outputs
        outBus.clear();
    }
    
    // Get output pointers AFTER copying (safe now)
    float* sampledL = outBus.getNumChannels() > 0 ? outBus.getWritePointer(0) : nullptr;
    float* sampledR = outBus.getNumChannels() > 1 ? outBus.getWritePointer(1) : nullptr;
    float* smoothed = outBus.getNumChannels() > 2 ? outBus.getWritePointer(2) : nullptr;
    float* trigOut = outBus.getNumChannels() > 3 ? outBus.getWritePointer(3) : nullptr;
    
    // Safety check - require at least left output and smoothed output
    if (!sampledL || !smoothed || !trigOut)
    {
        // Critical outputs missing, bail out
        if (blockCounter % 100 == 0)
        {
            juce::Logger::writeToLog("[S&H] ERROR: Missing output pointers! sampledL=" + 
                juce::String(sampledL != nullptr ? "OK" : "NULL") + 
                " smoothed=" + juce::String(smoothed != nullptr ? "OK" : "NULL") + 
                " trigOut=" + juce::String(trigOut != nullptr ? "OK" : "NULL"));
        }
        return;
    }
    
    // Use left channel as primary signal (or mono sum if stereo)
    // Always create mono signal buffer - if no input, it will be zeros (which is fine)
    juce::AudioBuffer<float> monoSignal(1, numSamples);
    if (inBus.getNumChannels() > 0)
    {
        if (inBus.getNumChannels() > 1 && signalR)
        {
            // Stereo: sum to mono (average)
            monoSignal.copyFrom(0, 0, inBus, 0, 0, numSamples);
            monoSignal.addFrom(0, 0, inBus, 1, 0, numSamples, 0.5f);
        }
        else
        {
            // Mono: copy left channel
            monoSignal.copyFrom(0, 0, inBus, 0, 0, numSamples);
        }
        
        // Check input levels AFTER creating mono signal (for accurate RMS)
        if (blockCounter % 100 == 0 && numSamples > 0)
        {
            float inLRMS = 0.0f, inRRMS = 0.0f, monoRMS = 0.0f;
            if (signalL)
            {
                for (int i = 0; i < numSamples; ++i)
                    inLRMS += signalL[i] * signalL[i];
                inLRMS = std::sqrt(inLRMS / numSamples);
            }
            if (signalR)
            {
                for (int i = 0; i < numSamples; ++i)
                    inRRMS += signalR[i] * signalR[i];
                inRRMS = std::sqrt(inRRMS / numSamples);
            }
            const float* sig = monoSignal.getReadPointer(0);
            if (sig)
            {
                for (int i = 0; i < numSamples; ++i)
                    monoRMS += sig[i] * sig[i];
                monoRMS = std::sqrt(monoRMS / numSamples);
            }
            juce::Logger::writeToLog("[S&H] Input: L_RMS=" + juce::String(inLRMS, 4) + 
                " R_RMS=" + juce::String(inRRMS, 4) + 
                " Mono_RMS=" + juce::String(monoRMS, 4));
        }
    }
    else
    {
        // No input channels - clear buffer (will hold 0.0)
        monoSignal.clear();
        if (blockCounter % 100 == 0)
            juce::Logger::writeToLog("[S&H] WARNING: No input channels!");
    }
    const float* signal = monoSignal.getReadPointer(0);
    
    // Log trigger status
    if (blockCounter % 100 == 0 && numSamples > 0)
    {
        float triggerRMS = 0.0f;
        if (trigger)
        {
            for (int i = 0; i < numSamples; ++i)
                triggerRMS += trigger[i] * trigger[i];
            triggerRMS = std::sqrt(triggerRMS / numSamples);
        }
        juce::Logger::writeToLog("[S&H] Trigger: RMS=" + juce::String(triggerRMS, 4) + 
            " | ptr=" + juce::String(trigger != nullptr ? "OK" : "NULL") +
            " | threshold=" + juce::String(baseThreshold, 3) +
            " | mode=" + juce::String(mode == 0 ? "S&H" : "T&H"));
    }
    
    // Update edge type (can be modulated) - compute once per block
    EdgeType edgeType = currentEdgeType;
    float finalThreshold = baseThreshold;
    float finalSlew = baseSlew;
    
    if (edgeParam)
    {
        if (isEdgeModulated && edgeMod)
        {
            // Map CV (0-1) to edge type (0-2) - use first sample for display
            const float edgeCV = juce::jlimit(0.0f, 1.0f, (edgeMod[0] + 1.0f) * 0.5f);
            const int edgeIndex = juce::jlimit(0, 2, static_cast<int>(edgeCV * 3.0f));
            edgeType = static_cast<EdgeType>(edgeIndex);
        }
        else
        {
            edgeType = static_cast<EdgeType>(edgeParam->getIndex());
        }
    }
    currentEdgeType = edgeType;
    
    // Get first sample's modulated values for telemetry (if modulated)
    if (isThresholdModulated && thresholdMod && numSamples > 0)
    {
        finalThreshold = juce::jlimit(0.0f, 1.0f, (thresholdMod[0] + 1.0f) * 0.5f);
    }
    if (isSlewModulated && slewMod && numSamples > 0)
    {
        finalSlew = juce::jlimit(0.0f, 1.0f, (slewMod[0] + 1.0f) * 0.5f);
    }
    
    // Store live modulated values for UI display (Modulation Trinity) - before processing loop
    setLiveParamValue("threshold_live", finalThreshold);
    setLiveParamValue("edge_live", static_cast<float>(edgeType));
    setLiveParamValue("slew_live", finalSlew);

#if defined(PRESET_CREATOR_UI)
    // Capture input for visualization (use mono signal)
    if (monoSignal.getNumChannels() > 0)
    {
        vizInputBuffer.copyFrom(0, 0, monoSignal, 0, 0, numSamples);
    }
    if (inBus.getNumChannels() > 2)
    {
        vizTriggerBuffer.copyFrom(0, 0, inBus, 2, 0, numSamples);
    }
#endif
    
    for (int i = 0; i < numSamples; ++i)
    {
        // Get modulated threshold
        float threshold = baseThreshold;
        if (isThresholdModulated && thresholdMod)
        {
            threshold = juce::jlimit(0.0f, 1.0f, (thresholdMod[i] + 1.0f) * 0.5f);
        }
        
        // Get modulated slew
        float slew = baseSlew;
        if (isSlewModulated && slewMod)
        {
            slew = juce::jlimit(0.0f, 1.0f, (slewMod[i] + 1.0f) * 0.5f);
        }
        
        // Update slew smoother time constant only if it changed
        const float maxSlewTime = 1000.0f; // 1 second max
        const float slewTimeMs = slew * maxSlewTime;
        const float slewTimeSec = slewTimeMs / 1000.0f;
        const float clampedSlewTimeSec = juce::jmax(0.001f, slewTimeSec);
        
        // Only reset smoother if slew time changed (avoids pops from constant resets)
        if (std::abs(clampedSlewTimeSec - lastSlewTimeSec) > 0.0001f)
        {
            slewSmoother.reset(currentSampleRate, clampedSlewTimeSec);
            lastSlewTimeSec = clampedSlewTimeSec;
        }
        
        // Update target value (but don't reset the smoother state)
        slewSmoother.setTargetValue(heldValue);
        
        // Process trigger signal
        bool shouldSample = false;
        bool triggerPulse = false;
        
        if (trigger)
        {
            const float triggerValue = trigger[i];
            const bool isTriggerHigh = triggerValue > threshold;
            
            // Edge detection
            if (edgeType == EdgeType::Rising)
            {
                if (isTriggerHigh && !wasTriggerHigh)
                {
                    shouldSample = true;
                    triggerPulse = true;
                }
            }
            else if (edgeType == EdgeType::Falling)
            {
                if (!isTriggerHigh && wasTriggerHigh)
                {
                    shouldSample = true;
                    triggerPulse = true;
                }
            }
            else if (edgeType == EdgeType::Both)
            {
                if (isTriggerHigh != wasTriggerHigh)
                {
                    shouldSample = true;
                    triggerPulse = true;
                }
            }
            
            wasTriggerHigh = isTriggerHigh;
            lastTriggerValue = triggerValue;
        }
        else
        {
            // No trigger input - use signal itself as trigger (self-triggering)
            const float signalValue = signal[i];
            const bool isSignalHigh = signalValue > threshold;
            
            if (edgeType == EdgeType::Rising)
            {
                if (isSignalHigh && !wasTriggerHigh)
                {
                    shouldSample = true;
                    triggerPulse = true;
                }
            }
            else if (edgeType == EdgeType::Falling)
            {
                if (!isSignalHigh && wasTriggerHigh)
                {
                    shouldSample = true;
                    triggerPulse = true;
                }
            }
            else if (edgeType == EdgeType::Both)
            {
                if (isSignalHigh != wasTriggerHigh)
                {
                    shouldSample = true;
                    triggerPulse = true;
                }
            }
            
            wasTriggerHigh = isSignalHigh;
        }
        
        // Sample & Hold or Track & Hold logic
        if (mode == 0) // Sample & Hold
        {
            if (shouldSample)
            {
                heldValue = signal[i];
                // Update target for smoother (but don't reset - let it smooth naturally)
                slewSmoother.setTargetValue(heldValue);
                // If slew is 0, jump immediately; otherwise let it smooth
                if (slew < 0.001f)
                {
                    slewSmoother.setCurrentAndTargetValue(heldValue);
                }
                // DEBUG: Log sampling events
                if (blockCounter % 100 == 0 && i < 10)
                {
                    juce::Logger::writeToLog("[S&H] SAMPLED at i=" + juce::String(i) + 
                        " | signal[i]=" + juce::String(signal[i], 4) + 
                        " | heldValue=" + juce::String(heldValue, 4));
                }
            }
        }
        else // Track & Hold (mode == 1)
        {
            if (trigger && trigger[i] > threshold)
            {
                // Track mode: follow input while trigger is high
                heldValue = signal[i];
                slewSmoother.setTargetValue(heldValue);
                // If slew is 0, jump immediately; otherwise let it smooth
                if (slew < 0.001f)
                {
                    slewSmoother.setCurrentAndTargetValue(heldValue);
                }
            }
            // When trigger goes low, hold the last value
        }
        
        // Apply slew limiting to smoothed output
        smoothedValue = slewSmoother.getNextValue();
        
        // Outputs - stereo sampled output
        sampledL[i] = heldValue;
        if (sampledR) sampledR[i] = heldValue; // Copy to right channel
        smoothed[i] = smoothedValue;
        trigOut[i] = triggerPulse ? 1.0f : 0.0f;
    }
    
    // DEBUG: Log output levels every 100 blocks
    if (blockCounter % 100 == 0 && numSamples > 0)
    {
        float outRMS = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            outRMS += sampledL[i] * sampledL[i];
        outRMS = std::sqrt(outRMS / numSamples);
        juce::Logger::writeToLog("[S&H] Output RMS=" + juce::String(outRMS, 4) + 
            " | heldValue=" + juce::String(heldValue, 4) + 
            " | smoothedValue=" + juce::String(smoothedValue, 4));
    }
    
    // Store last output values for tooltips
    if (!lastOutputValues.empty() && lastOutputValues[0] && sampledL)
        lastOutputValues[0]->store(sampledL[numSamples - 1]);
    if (lastOutputValues.size() > 1 && lastOutputValues[1] && smoothed)
        lastOutputValues[1]->store(smoothed[numSamples - 1]);
    if (lastOutputValues.size() > 2 && lastOutputValues[2] && trigOut)
        lastOutputValues[2]->store(trigOut[numSamples - 1]);

#if defined(PRESET_CREATOR_UI)
    // Capture output for visualization (use left channel)
    if (outBus.getNumChannels() > 0)
    {
        vizOutputBuffer.copyFrom(0, 0, outBus, 0, 0, numSamples);
    }
    
    // Down-sample and store waveforms
    auto captureWaveform = [&](const juce::AudioBuffer<float>& source, int channel, std::array<std::atomic<float>, VizData::waveformPoints>& dest)
    {
        const int samples = juce::jmin(source.getNumSamples(), numSamples);
        if (samples <= 0 || channel >= source.getNumChannels()) return;
        const int stride = juce::jmax(1, samples / VizData::waveformPoints);
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const int idx = juce::jmin(samples - 1, i * stride);
            float value = source.getSample(channel, idx);
            dest[i].store(juce::jlimit(-1.0f, 1.0f, value));
        }
    };
    
    captureWaveform(vizInputBuffer, 0, vizData.inputWaveform);
    captureWaveform(vizOutputBuffer, 0, vizData.outputWaveform);
    
    // Capture smoothed output (for visualization)
    if (outBus.getNumChannels() > 2)
    {
        juce::AudioBuffer<float> smoothedVizBuffer(1, numSamples);
        smoothedVizBuffer.copyFrom(0, 0, outBus, 2, 0, numSamples);
        captureWaveform(smoothedVizBuffer, 0, vizData.smoothedWaveform);
    }
    
    // Capture trigger markers
    captureWaveform(vizTriggerBuffer, 0, vizData.triggerMarkers);
    
    // Store current parameter values for UI
    vizData.currentThreshold.store(baseThreshold);
    vizData.currentEdge.store(edgeParam ? edgeParam->getIndex() : 0);
    vizData.currentSlew.store(baseSlew);
    vizData.currentMode.store(mode);
    vizData.sampleCount.store(static_cast<int>(heldValue * 1000.0f)); // For display
#endif
}

#if defined(PRESET_CREATOR_UI)
static void HelpMarker(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SAndHModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);
    
    // Load waveform data from atomics
    float inputWave[VizData::waveformPoints];
    float outputWave[VizData::waveformPoints];
    float smoothedWave[VizData::waveformPoints];
    float triggerMarkers[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWave[i] = vizData.inputWaveform[i].load();
        outputWave[i] = vizData.outputWaveform[i].load();
        smoothedWave[i] = vizData.smoothedWaveform[i].load();
        triggerMarkers[i] = vizData.triggerMarkers[i].load();
    }
    const float currentThreshold = vizData.currentThreshold.load();
    const int currentEdge = vizData.currentEdge.load();
    const float currentSlew = vizData.currentSlew.load();
    const int currentMode = vizData.currentMode.load();
    
    // Visualization section
    ImGui::Spacing();
    ThemeText("Sample & Hold Visualizer", theme.text.section_header);
    ImGui::Spacing();
    
    const float waveHeight = 180.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("SAndHViz", graphSize, false, childFlags))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);
        
        // Background
        const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        
        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);
        
        const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
        const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
        const ImU32 smoothedColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);
        const ImU32 triggerColor = IM_COL32(255, 100, 100, 255);
        const ImU32 thresholdColor = IM_COL32(255, 255, 0, 128);
        
        // Draw center line
        const float midY = p0.y + graphSize.y * 0.5f;
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 0.3f)), 1.0f);
        
        // Draw threshold line
        const float thresholdY = p0.y + graphSize.y * (1.0f - currentThreshold);
        drawList->AddLine(ImVec2(p0.x, thresholdY), ImVec2(p1.x, thresholdY), thresholdColor, 1.0f);
        
        const float scaleY = graphSize.y * 0.4f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);
        
        auto drawWave = [&](float* data, ImU32 color, float thickness)
        {
            float px = p0.x;
            float py = midY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float x = p0.x + i * stepX;
                const float y = midY - juce::jlimit(-1.0f, 1.0f, data[i]) * scaleY;
                const float clampedY = juce::jlimit(p0.y, p1.y, y);
                if (i > 0)
                    drawList->AddLine(ImVec2(px, py), ImVec2(x, clampedY), color, thickness);
                px = x;
                py = clampedY;
            }
        };
        
        // Draw waveforms
        drawWave(inputWave, inputColor, 1.5f);
        drawWave(outputWave, outputColor, 2.0f);
        drawWave(smoothedWave, smoothedColor, 1.5f);
        
        // Draw trigger markers
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            if (triggerMarkers[i] > 0.5f)
            {
                const float x = p0.x + i * stepX;
                drawList->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), triggerColor, 2.0f);
            }
        }
        
        drawList->PopClipRect();
        
        // Info overlay
        const char* modeNames[] = { "Sample & Hold", "Track & Hold" };
        const char* edgeNames[] = { "Rising", "Falling", "Both" };
        ImGui::SetCursorPos(ImVec2(4, waveHeight - 20));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%s | %s | Slew: %.1f%%", 
                          modeNames[currentMode], edgeNames[currentEdge], currentSlew * 100.0f);
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##sAndHVizDrag", graphSize);
    }
    ImGui::EndChild();
    
    ImGui::Spacing();
    ThemeText("Parameters", theme.text.section_header);
    ImGui::Spacing();
    
    // Threshold slider
    float threshold = thresholdParam ? thresholdParam->load() : 0.5f;
    bool isThresholdModulated = isParamModulated(paramIdThresholdMod);
    if (isThresholdModulated)
    {
        threshold = getLiveParamValueFor(paramIdThresholdMod, "threshold_live", threshold);
    }
    
    // Inline pin for Threshold Mod (Channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))
            ImGui::SameLine();
    }
    
    if (isThresholdModulated) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 0.0f, 0.3f));
    if (ImGui::SliderFloat("Threshold", &threshold, 0.0f, 1.0f, "%.3f"))
    {
        if (!isThresholdModulated && thresholdParam)
            *thresholdParam = threshold;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemDeactivatedAfterEdit() && !isThresholdModulated) onModificationEnded();
    if (!isThresholdModulated) adjustParamOnWheel(ap.getParameter(paramIdThreshold), paramIdThreshold, threshold);
    if (isThresholdModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    HelpMarker("Trigger detection threshold. When trigger signal crosses this level, sampling occurs.");
    
    // Edge selection
    int edge = edgeParam ? edgeParam->getIndex() : 0;
    bool isEdgeModulated = isParamModulated(paramIdEdgeMod);
    if (isEdgeModulated)
    {
        const float edgeCV = getLiveParamValueFor(paramIdEdgeMod, "edge_live", static_cast<float>(edge));
        edge = juce::jlimit(0, 2, static_cast<int>(edgeCV));
    }
    
    // Inline pin for Edge Mod (Channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))
            ImGui::SameLine();
    }
    
    if (isEdgeModulated) ImGui::BeginDisabled();
    const char* edges = "Rising\0Falling\0Both\0\0";
    if (ImGui::Combo("Edge", &edge, edges))
    {
        if (!isEdgeModulated && edgeParam)
            *edgeParam = edge;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isEdgeModulated) onModificationEnded();
    // Scroll wheel editing for Edge combo
    if (!isEdgeModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newIndex = juce::jlimit(0, 2, edge + (wheel > 0.0f ? -1 : 1));
            if (newIndex != edge && edgeParam)
            {
                *edgeParam = newIndex;
                onModificationEnded();
            }
        }
    }
    if (isEdgeModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    HelpMarker("Edge type for trigger detection:\nRising: Sample on rising edge\nFalling: Sample on falling edge\nBoth: Sample on both edges");
    
    // Slew slider
    float slew = slewParam ? slewParam->load() : 0.0f;
    bool isSlewModulated = isParamModulated(paramIdSlewMod);
    if (isSlewModulated)
    {
        slew = getLiveParamValueFor(paramIdSlewMod, "slew_live", slew);
    }
    
    // Inline pin for Slew Mod (Channel 5)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(5))
            ImGui::SameLine();
    }
    
    if (isSlewModulated) ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Slew", &slew, 0.0f, 1.0f, "%.3f"))
    {
        if (!isSlewModulated && slewParam)
            *slewParam = slew;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isSlewModulated) onModificationEnded();
    if (!isSlewModulated) adjustParamOnWheel(ap.getParameter(paramIdSlew), paramIdSlew, slew);
    if (isSlewModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    HelpMarker("Smoothing amount for transitions between sampled values. 0 = instant, 1 = smooth over 1 second.");
    
    // Mode selection
    int mode = modeParam ? modeParam->getIndex() : 0;
    const char* modes = "Sample & Hold\0Track & Hold\0\0";
    if (ImGui::Combo("Mode", &mode, modes))
    {
        if (modeParam)
            *modeParam = mode;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    // Scroll wheel editing for Mode combo
    if (ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newIndex = juce::jlimit(0, 1, mode + (wheel > 0.0f ? -1 : 1));
            if (newIndex != mode && modeParam)
            {
                *modeParam = newIndex;
                onModificationEnded();
            }
        }
    }
    HelpMarker("Mode:\nSample & Hold: Sample input on trigger, hold until next trigger\nTrack & Hold: Track input while trigger is high, hold when trigger goes low");
    
    ImGui::PopItemWidth();
    ImGui::PopID();
}

void SAndHModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Channels 0-2 stay parallel (audio inputs and trigger)
    helpers.drawParallelPins("In L", 0, "Out L", 0);
    helpers.drawParallelPins("In R", 1, "Out R", 1);
    helpers.drawParallelPins("Trigger In", 2, "Smoothed Out", 2);
    
    // Channels 3-5 are now inline (Threshold Mod, Edge Mod, Slew Mod)
    // Draw remaining outputs (output-only pin)
    helpers.drawParallelPins(nullptr, -1, "Trigger Out", 3);
}

juce::String SAndHModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "In L";
        case 1: return "In R";
        case 2: return "Trigger In";
        case 3: return "Threshold Mod";
        case 4: return "Edge Mod";
        case 5: return "Slew Mod";
        default: return juce::String("In ") + juce::String(channel + 1);
    }
}

juce::String SAndHModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Out L";
        case 1: return "Out R";
        case 2: return "Smoothed Out";
        case 3: return "Trigger Out";
        default: return juce::String("Out ") + juce::String(channel + 1);
    }
}

bool SAndHModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All inputs are on bus 0 (like BitCrusher)
    // ch0-1: Audio, ch2: Trigger, ch3-5: CV modulations
    if (paramId == paramIdThresholdMod) { outChannelIndexInBus = 3; return true; }
    if (paramId == paramIdEdgeMod) { outChannelIndexInBus = 4; return true; }
    if (paramId == paramIdSlewMod) { outChannelIndexInBus = 5; return true; }
    
    return false;
}
#endif

