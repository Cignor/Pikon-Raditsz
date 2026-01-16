#include "LFOModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif
#include "../graph/ModularSynthProcessor.h"

LFOModuleProcessor::LFOModuleProcessor()
    // CORRECTED: Use a single input bus with 3 discrete channels
    : ModuleProcessor(BusesProperties()
                        .withInput("Inputs", juce::AudioChannelSet::discreteChannels(3), true) // ch0:Rate, ch1:Depth, ch2:Wave
                        .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "LFOParams", createParameterLayout())
{
    rateParam = apvts.getRawParameterValue(paramIdRate);
    depthParam = apvts.getRawParameterValue(paramIdDepth);
    bipolarParam = apvts.getRawParameterValue(paramIdBipolar);
    waveParam = apvts.getRawParameterValue(paramIdWave);
    syncParam = apvts.getRawParameterValue(paramIdSync);
    rateDivisionParam = apvts.getRawParameterValue(paramIdRateDivision);
    relativeModeParam = apvts.getRawParameterValue(paramIdRelativeMode);
    
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    
    osc.initialise([](float x) { return std::sin(x); }, 128);
}

juce::AudioProcessorValueTreeState::ParameterLayout LFOModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdRate, "Rate", juce::NormalisableRange<float>(0.05f, 20.0f, 0.01f, 0.3f), 1.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(paramIdDepth, "Depth", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdBipolar, "Bipolar", true));
    p.push_back(std::make_unique<juce::AudioParameterChoice>(paramIdWave, "Wave", juce::StringArray{ "Sine", "Tri", "Saw" }, 0));
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdSync, "Sync", false));
    p.push_back(std::make_unique<juce::AudioParameterChoice>(paramIdRateDivision, "Division", 
        juce::StringArray{ "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8" }, 3)); // Default: 1/4 note
    p.push_back(std::make_unique<juce::AudioParameterBool>(paramIdRelativeMode, "Relative Mod", true)); // Default: Relative (additive) mode
    return { p.begin(), p.end() };
}

void LFOModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32)samplesPerBlock, 2 };
    osc.prepare(spec);

#if defined(PRESET_CREATOR_UI)
    vizLfoBuffer.setSize(1, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.lfoWaveform) v.store(0.0f);
    vizData.currentValue.store(0.0f);
    vizData.currentRate.store(1.0f);
    vizData.currentDepth.store(0.5f);
    vizData.currentWave.store(0);
    vizData.isBipolar.store(true);
    vizData.isSynced.store(false);
#endif
}

void LFOModuleProcessor::setTimingInfo(const TransportState& state)
{
    m_currentTransport = state;
}

void LFOModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    auto out = getBusBuffer(buffer, false, 0);

    // CORRECTED: All inputs are on a single bus at index 0
    auto inBus = getBusBuffer(buffer, true, 0);
    
    // CORRECTED: Use the _mod IDs to check for connections
    const bool isRateMod = isParamInputConnected(paramIdRateMod);
    const bool isDepthMod = isParamInputConnected(paramIdDepthMod);
    const bool isWaveMod = isParamInputConnected(paramIdWaveMod);

    // CORRECTED: Read CVs from the correct channels on the single input bus
    const float* rateCV = isRateMod && inBus.getNumChannels() > 0 ? inBus.getReadPointer(0) : nullptr;
    const float* depthCV = isDepthMod && inBus.getNumChannels() > 1 ? inBus.getReadPointer(1) : nullptr;
    const float* waveCV = isWaveMod && inBus.getNumChannels() > 2 ? inBus.getReadPointer(2) : nullptr;

    const float baseRate = rateParam->load();
    const float baseDepth = depthParam->load();
    const int baseWave = static_cast<int>(waveParam->load());
    const bool bipolar = bipolarParam->load() > 0.5f;
    const bool syncEnabled = syncParam->load() > 0.5f;
    const bool relativeMode = relativeModeParam->load() > 0.5f; // NEW: Read relative mode setting
    int rateDivisionIndex = static_cast<int>(rateDivisionParam->load());
    
    // DEBUG: Log relative mode status (once per buffer)
    static int logCounter = 0;
    if (++logCounter % 100 == 0) // Log every 100th buffer to avoid spam
    {
        juce::Logger::writeToLog("[LFO] Relative Mode = " + juce::String(relativeMode ? "TRUE (additive)" : "FALSE (absolute)"));
        juce::Logger::writeToLog("[LFO] Base Rate = " + juce::String(baseRate) + " Hz, Base Depth = " + juce::String(baseDepth));
        juce::Logger::writeToLog("[LFO] Rate CV connected = " + juce::String(isRateMod ? "YES" : "NO") + 
                   ", Depth CV connected = " + juce::String(isDepthMod ? "YES" : "NO"));
    }
    // If a global division is broadcast by a master clock, adopt it when sync is enabled
    // IMPORTANT: Read from parent's LIVE transport state, not cached copy (which is stale)
    if (syncEnabled && getParent())
    {
        int globalDiv = getParent()->getTransportState().globalDivisionIndex.load();
        if (globalDiv >= 0)
            rateDivisionIndex = globalDiv;
    }

    // Rate division map: 1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 4, 8
    static const double divisions[] = { 1.0/32.0, 1.0/16.0, 1.0/8.0, 1.0/4.0, 1.0/2.0, 1.0, 2.0, 4.0, 8.0 };
    const double beatDivision = divisions[juce::jlimit(0, 8, rateDivisionIndex)];

    float lastRate = baseRate, lastDepth = baseDepth;
    int lastWave = baseWave;

    for (int i = 0; i < out.getNumSamples(); ++i)
    {
        float finalRate = baseRate;
        if (isRateMod && rateCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, rateCV[i]);
            if (relativeMode) {
                // RELATIVE MODE: Modulate around the base value
                finalRate = baseRate * std::pow(4.0f, cv - 0.5f); // +/- 2 octaves from base
                
                // DEBUG: Log first sample calculation
                if (i == 0 && logCounter % 100 == 0) {
                    juce::Logger::writeToLog("[LFO Rate] RELATIVE mode: CV=" + juce::String(cv, 3) + 
                               ", baseRate=" + juce::String(baseRate, 3) + 
                               " Hz, finalRate=" + juce::String(finalRate, 3) + " Hz");
                }
            } else {
                // ABSOLUTE MODE: CV directly controls the parameter
                finalRate = juce::jmap(cv, 0.05f, 20.0f); // Full range 0.05Hz to 20Hz
                
                // DEBUG: Log first sample calculation
                if (i == 0 && logCounter % 100 == 0) {
                    juce::Logger::writeToLog("[LFO Rate] ABSOLUTE mode: CV=" + juce::String(cv, 3) + 
                               ", finalRate=" + juce::String(finalRate, 3) + " Hz (ignores slider)");
                }
            }
        }
        
        float depth = baseDepth;
        if (isDepthMod && depthCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, depthCV[i]);
            if (relativeMode) {
                // RELATIVE MODE: Add CV offset to base value
                depth = juce::jlimit(0.0f, 1.0f, baseDepth + (cv - 0.5f)); // +/- 0.5 from base
                
                // DEBUG: Log first sample calculation
                if (i == 0 && logCounter % 100 == 0) {
                    juce::Logger::writeToLog("[LFO Depth] RELATIVE mode: CV=" + juce::String(cv, 3) + 
                               ", baseDepth=" + juce::String(baseDepth, 3) + 
                               ", finalDepth=" + juce::String(depth, 3));
                }
            } else {
                // ABSOLUTE MODE: CV directly sets depth
                depth = cv;
                
                // DEBUG: Log first sample calculation
                if (i == 0 && logCounter % 100 == 0) {
                    juce::Logger::writeToLog("[LFO Depth] ABSOLUTE mode: CV=" + juce::String(cv, 3) + 
                               ", finalDepth=" + juce::String(depth, 3) + " (ignores slider)");
                }
            }
        }
        
        int w = baseWave;
        if (isWaveMod && waveCV != nullptr) {
            const float cv = juce::jlimit(0.0f, 1.0f, waveCV[i]);
            // Waveform is always absolute (discrete selection)
            w = static_cast<int>(cv * 2.99f);
        }

        lastRate = finalRate;
        lastDepth = depth;
        lastWave = w;
        
        if (currentWaveform != w) {
            if (w == 0)      osc.initialise ([](float x){ return std::sin(x); }, 128);
            else if (w == 1) osc.initialise ([](float x){ return 2.0f / juce::MathConstants<float>::pi * std::asin(std::sin(x)); }, 128);
            else             osc.initialise ([](float x){ return (x / juce::MathConstants<float>::pi); }, 128);
            currentWaveform = w;
        }
        
        float lfoSample = 0.0f;
        
        // Check Global Reset (pulse from Timeline Master loop)
        if (m_currentTransport.forceGlobalReset.load())
        {
            // Reset oscillator phase to 0
            osc.reset();
        }
        
        if (syncEnabled && m_currentTransport.isPlaying)
        {
            // Transport-synced mode: calculate phase directly from song position
            double phase = std::fmod(m_currentTransport.songPositionBeats * beatDivision, 1.0);
            double phaseRadians = phase * juce::MathConstants<double>::twoPi;
            
            // Generate waveform based on phase
            if (w == 0) // Sine
                lfoSample = std::sin(phaseRadians);
            else if (w == 1) // Triangle
                lfoSample = 2.0f / juce::MathConstants<float>::pi * std::asin(std::sin(phaseRadians));
            else // Saw
                lfoSample = (phaseRadians / juce::MathConstants<float>::pi);
        }
        else
        {
            // Free-running mode: use internal oscillator
            osc.setFrequency(finalRate);
            lfoSample = osc.processSample(0.0f);
        }
        
        const float finalSample = (bipolar ? lfoSample : (lfoSample * 0.5f + 0.5f)) * depth;

        out.setSample(0, i, finalSample);

#if defined(PRESET_CREATOR_UI)
        // Capture LFO into circular buffer
        if (vizLfoBuffer.getNumSamples() > 0)
        {
            vizLfoBuffer.setSample(0, vizWritePos, finalSample);
            vizWritePos = (vizWritePos + 1) % vizBufferSize;
        }
#endif
    }
    
    // Update inspector values
    updateOutputTelemetry(out);

    // Store live modulated values for UI display
    setLiveParamValue("rate_live", lastRate);
    setLiveParamValue("depth_live", lastDepth);
    setLiveParamValue("wave_live", (float)lastWave);

#if defined(PRESET_CREATOR_UI)
    // Update visualization data (thread-safe)
    // Downsample LFO waveform from circular buffer
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx = (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) % vizBufferSize;
        const float sample = vizLfoBuffer.getSample(0, readIdx);
        vizData.lfoWaveform[i].store(sample);
    }

    // Update current state
    const float lastSample = out.getNumSamples() > 0 ? out.getSample(0, out.getNumSamples() - 1) : 0.0f;
    vizData.currentValue.store(lastSample);
    vizData.currentRate.store(lastRate);
    vizData.currentDepth.store(lastDepth);
    vizData.currentWave.store(lastWave);
    vizData.isBipolar.store(bipolar);
    vizData.isSynced.store(syncEnabled);
#endif
}

// CORRECTED: Clean, unambiguous routing for a single multi-channel input bus
bool LFOModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus.
    if (paramId == paramIdRateMod) { outChannelIndexInBus = 0; return true; }
    if (paramId == paramIdDepthMod) { outChannelIndexInBus = 1; return true; }
    if (paramId == paramIdWaveMod) { outChannelIndexInBus = 2; return true; }
    return false;
}

#if defined(PRESET_CREATOR_UI)

// Helper function for tooltip with help marker
static void HelpMarkerLFO(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void LFOModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    auto& ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    
    bool isRateModulated = isParamInputConnected(paramIdRateMod);
    bool isDepthModulated = isParamInputConnected(paramIdDepthMod);
    bool isWaveModulated = isParamInputConnected(paramIdWaveMod);
    
    float rate = isRateModulated ? getLiveParamValueFor(paramIdRateMod, "rate_live", rateParam->load()) : rateParam->load();
    float depth = isDepthModulated ? getLiveParamValueFor(paramIdDepthMod, "depth_live", depthParam->load()) : depthParam->load();
    int wave = isWaveModulated ? (int)getLiveParamValueFor(paramIdWaveMod, "wave_live", (float)static_cast<int>(waveParam->load())) : static_cast<int>(waveParam->load());
    bool bipolar = bipolarParam->load() > 0.5f;
    
    ImGui::PushItemWidth(itemWidth);

    // === LFO PARAMETERS SECTION ===
    ThemeText("LFO Parameters", theme.text.section_header);
    ImGui::Spacing();

    // Rate slider with inline pin and tooltip
    if (isRateModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(0))  // Channel 0 = Rate Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Rate", &rate, 0.05f, 20.0f, "%.2f Hz", ImGuiSliderFlags_Logarithmic)) if (!isRateModulated) *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdRate)) = rate;
    if (!isRateModulated) adjustParamOnWheel(ap.getParameter(paramIdRate), "rate", rate);
    if (ImGui::IsItemDeactivatedAfterEdit() && !isRateModulated) onModificationEnded();
    if (isRateModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerLFO("LFO rate in Hz\nLogarithmic scale from 0.05 Hz to 20 Hz");
    
    // Depth slider with inline pin and tooltip
    if (isDepthModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1))  // Channel 1 = Depth Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Depth", &depth, 0.0f, 1.0f)) if (!isDepthModulated) *dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDepth)) = depth;
    if (!isDepthModulated) adjustParamOnWheel(ap.getParameter(paramIdDepth), "depth", depth);
    if (ImGui::IsItemDeactivatedAfterEdit() && !isDepthModulated) onModificationEnded();
    if (isDepthModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerLFO("LFO depth/amplitude (0-1)\nControls output signal strength");

    // Wave combo with inline pin and tooltip
    if (isWaveModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))  // Channel 2 = Wave Mod
            ImGui::SameLine();
    }
    if (ImGui::Combo("Wave", &wave, "Sine\0Tri\0Saw\0\0")) if (!isWaveModulated) *dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdWave)) = wave;
    if (!isWaveModulated && ImGui::IsItemHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int newWave = juce::jlimit(0, 2, wave + (wheel > 0.0f ? -1 : 1));
            if (newWave != wave)
            {
                wave = newWave;
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdWave))) *p = wave;
                onModificationEnded();
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isWaveModulated) onModificationEnded();
    if (isWaveModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerLFO("Waveform shape:\nSine = smooth\nTri = linear\nSaw = ramp");

    // Bipolar checkbox
    if (ImGui::Checkbox("Bipolar", &bipolar)) *dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdBipolar)) = bipolar;
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerLFO("Bipolar: -1 to +1\nUnipolar: 0 to +1");

    ImGui::Spacing();
    ImGui::Spacing();

    // === MODULATION MODE SECTION ===
    ThemeText("Modulation Mode", theme.text.section_header);
    ImGui::Spacing();

    // Relative Mode checkbox
    bool relativeMode = relativeModeParam->load() > 0.5f;
    if (ImGui::Checkbox("Relative Modulation", &relativeMode)) 
    {
        *dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdRelativeMode)) = relativeMode;
        juce::Logger::writeToLog("[LFO UI] Relative Modulation checkbox changed to: " + juce::String(relativeMode ? "TRUE" : "FALSE"));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerLFO("Relative: CV modulates around slider position\nAbsolute: CV completely replaces slider value\n\nExample:\n- Relative: Slider at 5Hz, CV adds ±2 octaves\n- Absolute: CV directly sets 0.05-20Hz range");

    ImGui::Spacing();
    ImGui::Spacing();

    // === TRANSPORT SYNC SECTION ===
    ThemeText("Transport Sync", theme.text.section_header);
    ImGui::Spacing();

    // Sync checkbox
    bool sync = syncParam->load() > 0.5f;
    if (ImGui::Checkbox("Sync to Transport", &sync)) *dynamic_cast<juce::AudioParameterBool*>(ap.getParameter(paramIdSync)) = sync;
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerLFO("Sync LFO rate to host transport tempo");
    
    if (sync)
    {
        // Check if global division is active (Tempo Clock override)
        // IMPORTANT: Read from parent's LIVE transport state, not cached copy
        int globalDiv = getParent() ? getParent()->getTransportState().globalDivisionIndex.load() : -1;
        bool isGlobalDivisionActive = globalDiv >= 0;
        int division = isGlobalDivisionActive ? globalDiv : static_cast<int>(rateDivisionParam->load());
        
        const char* items[] = { "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8" };
        
        // Grey out if controlled by Tempo Clock
        if (isGlobalDivisionActive) ImGui::BeginDisabled();
        
        if (ImGui::Combo("Division", &division, items, (int)(sizeof(items)/sizeof(items[0]))))
            if (!isGlobalDivisionActive)
                *dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter(paramIdRateDivision)) = division;
        if (ImGui::IsItemDeactivatedAfterEdit() && !isGlobalDivisionActive) onModificationEnded();
        
        if (isGlobalDivisionActive)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
                ThemeText("Tempo Clock Division Override Active", theme.text.warning);
                ImGui::TextUnformatted("A Tempo Clock node with 'Division Override' enabled is controlling the global division.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        else
        {
            ImGui::SameLine();
            HelpMarkerLFO("Note division for tempo sync\n1/16 = sixteenth notes, 1 = whole notes, etc.");
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // === LFO WAVEFORM VISUALIZATION ===
    ThemeText("LFO Output", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushID(this); // Unique ID for this node's UI
    auto* drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 lfoColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
    const ImU32 centerLineColor = IM_COL32(150, 150, 150, 100);

    // Real-time LFO waveform
    const ImVec2 waveOrigin = ImGui::GetCursorScreenPos();
    const float waveHeight = 120.0f;
    const ImVec2 waveMax = ImVec2(waveOrigin.x + itemWidth, waveOrigin.y + waveHeight);
    drawList->AddRectFilled(waveOrigin, waveMax, bgColor, 4.0f);
    ImGui::PushClipRect(waveOrigin, waveMax, true);

    // Read visualization data (thread-safe)
    float lfoWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
        lfoWaveform[i] = vizData.lfoWaveform[i].load();

    const bool isBipolar = vizData.isBipolar.load();
    const float currentValue = vizData.currentValue.load();
    const float currentRate = vizData.currentRate.load();
    const float currentDepth = vizData.currentDepth.load();
    const int currentWave = vizData.currentWave.load();

    // Calculate Y range based on bipolar/unipolar
    const float midY = waveOrigin.y + waveHeight * 0.5f;
    const float scaleY = waveHeight * 0.45f;
    const float stepX = itemWidth / (float)(VizData::waveformPoints - 1);

    // Draw center line (zero reference for bipolar, or bottom for unipolar)
    if (isBipolar)
    {
        drawList->AddLine(ImVec2(waveOrigin.x, midY), ImVec2(waveMax.x, midY), centerLineColor, 1.0f);
    }
    else
    {
        // For unipolar, draw bottom line
        drawList->AddLine(ImVec2(waveOrigin.x, waveMax.y - 4.0f), ImVec2(waveMax.x, waveMax.y - 4.0f), centerLineColor, 1.0f);
    }

    // Draw LFO waveform
    float prevX = waveOrigin.x;
    float prevY = isBipolar ? midY : (waveMax.y - 4.0f);
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        float sample = juce::jlimit(-1.0f, 1.0f, lfoWaveform[i]);
        const float x = waveOrigin.x + i * stepX;
        float y;
        if (isBipolar)
        {
            y = midY - sample * scaleY;
        }
        else
        {
            // Unipolar: map 0-1 to bottom-top
            sample = juce::jmax(0.0f, sample); // Clamp to positive
            y = waveMax.y - 4.0f - sample * scaleY * 2.0f;
        }
        if (i > 0)
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), lfoColor, 2.5f);
        prevX = x;
        prevY = y;
    }

    // Draw current value indicator (vertical line at the end)
    float indicatorY;
    if (isBipolar)
    {
        indicatorY = midY - currentValue * scaleY;
    }
    else
    {
        const float clampedValue = juce::jmax(0.0f, currentValue);
        indicatorY = waveMax.y - 4.0f - clampedValue * scaleY * 2.0f;
    }
    drawList->AddLine(ImVec2(waveMax.x - 2.0f, waveOrigin.y), ImVec2(waveMax.x - 2.0f, waveMax.y), 
                     IM_COL32(255, 255, 255, 120), 2.0f);

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(waveOrigin.x, waveMax.y));
    ImGui::Dummy(ImVec2(itemWidth, 0));

    ImGui::Spacing();

    // Live parameter readouts
    const char* waveNames[] = { "Sine", "Tri", "Saw" };
    const char* waveName = (currentWave >= 0 && currentWave < 3) ? waveNames[currentWave] : "Unknown";
    
    ImGui::Text("Output: %.3f", currentValue);
    ImGui::SameLine();
    ImGui::Text("| Rate: %.2f Hz", currentRate);
    ImGui::SameLine();
    ImGui::Text("| %s", waveName);

    // Progress bar showing current LFO position (normalized)
    float normalizedValue = isBipolar ? (currentValue + 1.0f) / 2.0f : currentValue;
    normalizedValue = juce::jlimit(0.0f, 1.0f, normalizedValue);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, lfoColor);
    ImGui::ProgressBar(normalizedValue, ImVec2(itemWidth * 0.6f, 0), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%.0f%%", normalizedValue * 100.0f);

    ImGui::PopItemWidth();
    ImGui::PopID(); // End visualization child window ID
    ImGui::PopID(); // End module instance ID
}

void LFOModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // All modulation inputs (Rate, Depth, Wave) are now inline pins
    // Only draw the output pin
    helpers.drawAudioOutputPin("Out", 0);
}

juce::String LFOModuleProcessor::getAudioInputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Rate Mod";
        case 1: return "Depth Mod";
        case 2: return "Wave Mod";
        default: return {};
    }
}

juce::String LFOModuleProcessor::getAudioOutputLabel(int channel) const
{
    switch (channel)
    {
        case 0: return "Out";
        default: return {};
    }
}

std::optional<RhythmInfo> LFOModuleProcessor::getRhythmInfo() const
{
    RhythmInfo info;
    
    // Build display name with logical ID
    info.displayName = "LFO #" + juce::String(getLogicalId());
    info.sourceType = "lfo";
    
    // Check if synced to transport
    const bool syncEnabled = syncParam && syncParam->load() > 0.5f;
    info.isSynced = syncEnabled;
    
    // Read LIVE transport state from parent (not cached copy)
    TransportState transport;
    bool hasTransport = false;
    if (getParent())
    {
        transport = getParent()->getTransportState();
        hasTransport = true;
    }
    
    // LFO is always active when running
    info.isActive = true;
    
    // Calculate effective BPM
    if (syncEnabled && hasTransport && transport.isPlaying)
    {
        // In sync mode: calculate effective BPM from transport + division
        int divisionIndex = rateDivisionParam ? (int)rateDivisionParam->load() : 3;
        
        // Check for global division override from Tempo Clock
        int globalDiv = transport.globalDivisionIndex.load();
        if (globalDiv >= 0)
            divisionIndex = globalDiv;
        
        // Division multipliers: 1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 4, 8
        static const double divisions[] = { 1.0/32.0, 1.0/16.0, 1.0/8.0, 1.0/4.0, 1.0/2.0, 1.0, 2.0, 4.0, 8.0 };
        const double beatDivision = divisions[juce::jlimit(0, 8, divisionIndex)];
        
        // Effective BPM = transport BPM * division
        // (One complete LFO cycle = one "beat" at the division rate)
        info.bpm = static_cast<float>(transport.bpm * beatDivision);
    }
    else if (!syncEnabled)
    {
        // Free-running mode: convert Hz rate to BPM
        // Rate is in cycles per second (Hz), one cycle = one "beat"
        const float rate = rateParam ? rateParam->load() : 1.0f;
        info.bpm = rate * 60.0f; // Convert Hz to BPM
    }
    else
    {
        // Synced but transport stopped
        info.bpm = 0.0f;
    }
    
    // Validate BPM before returning
    if (!std::isfinite(info.bpm))
        info.bpm = 0.0f;
    
    return info;
}
#endif