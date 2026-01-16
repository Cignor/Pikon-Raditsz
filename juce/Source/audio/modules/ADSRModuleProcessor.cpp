#include "ADSRModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

ADSRModuleProcessor::ADSRModuleProcessor()
    : ModuleProcessor (BusesProperties()
                        .withInput ("Inputs", juce::AudioChannelSet::discreteChannels(6), true) // ch0 Gate, ch1 Trigger, ch2-5 mods
                        .withOutput("Output", juce::AudioChannelSet::quadraphonic(), true)),
      apvts (*this, nullptr, "ADSRParams", createParameterLayout())
{
    attackParam  = apvts.getRawParameterValue (paramIdAttack);
    decayParam   = apvts.getRawParameterValue (paramIdDecay);
    sustainParam = apvts.getRawParameterValue (paramIdSustain);
    releaseParam = apvts.getRawParameterValue (paramIdRelease);
    attackModParam  = apvts.getRawParameterValue (paramIdAttackMod);
    decayModParam   = apvts.getRawParameterValue (paramIdDecayMod);
    sustainModParam = apvts.getRawParameterValue (paramIdSustainMod);
    releaseModParam = apvts.getRawParameterValue (paramIdReleaseMod);
    relativeAttackModParam = apvts.getRawParameterValue("relativeAttackMod");
    relativeDecayModParam = apvts.getRawParameterValue("relativeDecayMod");
    relativeSustainModParam = apvts.getRawParameterValue("relativeSustainMod");
    relativeReleaseModParam = apvts.getRawParameterValue("relativeReleaseMod");
    
    // CORRECTED INITIALIZATION:
    // Create unique_ptrs to heap-allocated atomics for each output channel.
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Channel 0
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Channel 1
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Channel 2
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Channel 3
}

juce::AudioProcessorValueTreeState::ParameterLayout ADSRModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdAttack,  "Attack",  juce::NormalisableRange<float> (0.001f, 5.0f, 0.01f, 0.4f), 0.01f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdDecay,   "Decay",   juce::NormalisableRange<float> (0.001f, 5.0f, 0.01f, 0.4f), 0.1f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdSustain, "Sustain", juce::NormalisableRange<float> (0.0f, 1.0f), 0.7f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdRelease, "Release", juce::NormalisableRange<float> (0.001f, 5.0f, 0.01f, 0.4f), 0.2f));
    
    // Add modulation parameters
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdAttackMod,  "Attack Mod",  0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdDecayMod,   "Decay Mod",   0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdSustainMod, "Sustain Mod", 0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (paramIdReleaseMod, "Release Mod", 0.0f, 1.0f, 0.0f));
    
    // Relative modulation modes
    p.push_back (std::make_unique<juce::AudioParameterBool>("relativeAttackMod", "Relative Attack Mod", true));
    p.push_back (std::make_unique<juce::AudioParameterBool>("relativeDecayMod", "Relative Decay Mod", true));
    p.push_back (std::make_unique<juce::AudioParameterBool>("relativeSustainMod", "Relative Sustain Mod", true));
    p.push_back (std::make_unique<juce::AudioParameterBool>("relativeReleaseMod", "Relative Release Mod", true));
    
    return { p.begin(), p.end() };
}

void ADSRModuleProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    eorPending = eocPending = 0; envLevel = 0.0f; lastGate = false; lastTrigger = false; stage = Stage::Idle;

#if defined(PRESET_CREATOR_UI)
    vizEnvelopeBuffer.setSize(1, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.envelopeWaveform) v.store(0.0f);
    vizData.currentEnvelope.store(0.0f);
    vizData.currentStage.store(0);
    vizData.gateActive.store(false);
    vizData.triggerActive.store(false);
#endif
}

void ADSRModuleProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    
    // Read CV from input buses (if connected)
    float attackModCV = 0.5f;   // Default to neutral (0.5)
    float decayModCV = 0.5f;    // Default to neutral (0.5)
    float sustainModCV = 0.5f;  // Default to neutral (0.5)
    float releaseModCV = 0.5f;  // Default to neutral (0.5)
    
    // Get single bus for all modulation inputs
    auto inBus = getBusBuffer(buffer, true, 0);
    
    // Check if attack mod is connected and read CV (channel 2)
    if (isParamInputConnected(paramIdAttackMod) && inBus.getNumChannels() > 2)
        attackModCV = inBus.getReadPointer(2)[0]; // Read first sample
    
    // Check if decay mod is connected and read CV (channel 3)
    if (isParamInputConnected(paramIdDecayMod) && inBus.getNumChannels() > 3)
        decayModCV = inBus.getReadPointer(3)[0]; // Read first sample
    
    // Check if sustain mod is connected and read CV (channel 4)
    if (isParamInputConnected(paramIdSustainMod) && inBus.getNumChannels() > 4)
        sustainModCV = inBus.getReadPointer(4)[0]; // Read first sample
    
    // Check if release mod is connected and read CV (channel 5)
    if (isParamInputConnected(paramIdReleaseMod) && inBus.getNumChannels() > 5)
        releaseModCV = inBus.getReadPointer(5)[0]; // Read first sample

    const float aBase = attackParam  ? attackParam->load()  : 0.01f;
    const float dBase = decayParam   ? decayParam->load()   : 0.10f;
    const float sBase = sustainParam ? sustainParam->load() : 0.70f;
    const float rBase = releaseParam ? releaseParam->load() : 0.20f;
    
    const bool relativeAttackMode = relativeAttackModParam && relativeAttackModParam->load() > 0.5f;
    const bool relativeDecayMode = relativeDecayModParam && relativeDecayModParam->load() > 0.5f;
    const bool relativeSustainMode = relativeSustainModParam && relativeSustainModParam->load() > 0.5f;
    const bool relativeReleaseMode = relativeReleaseModParam && relativeReleaseModParam->load() > 0.5f;

    // Relative modulation: 0.5 = neutral. Times scaled roughly 0.25x..4x; sustain +/-0.5.
    auto timeScale = [](float norm){ return juce::jlimit(0.25f, 4.0f, std::pow(2.0f, (norm - 0.5f) * 2.0f)); }; // 0.25..4.0
    auto sustDelta = [](float norm){ return juce::jlimit(-0.5f, 0.5f, norm - 0.5f); };

    // Apply CV modulation or fallback to parameter values
    float aEff = aBase;
    if (isParamInputConnected(paramIdAttackMod)) {
        aEff = relativeAttackMode ? (aBase * timeScale(attackModCV)) : juce::jmap(attackModCV, 0.001f, 5.0f);
    }
    aEff = juce::jlimit(0.001f, 5.0f, aEff);
    
    float dEff = dBase;
    if (isParamInputConnected(paramIdDecayMod)) {
        dEff = relativeDecayMode ? (dBase * timeScale(decayModCV)) : juce::jmap(decayModCV, 0.001f, 5.0f);
    }
    dEff = juce::jlimit(0.001f, 5.0f, dEff);
    
    float sEff = sBase;
    if (isParamInputConnected(paramIdSustainMod)) {
        sEff = relativeSustainMode ? (sBase + sustDelta(sustainModCV)) : sustainModCV;
    }
    sEff = juce::jlimit(0.0f, 1.0f, sEff);
    
    float rEff = rBase;
    if (isParamInputConnected(paramIdReleaseMod)) {
        rEff = relativeReleaseMode ? (rBase * timeScale(releaseModCV)) : juce::jmap(releaseModCV, 0.001f, 5.0f);
    }
    rEff = juce::jlimit(0.001f, 5.0f, rEff);

    // Ensure UI reflects effective modulation in tooltips (optional debug)

    const float atkSec = aEff;
    const float decSec = dEff;
    const float susLvl = sEff;
    const float relSec = rEff;

    // Gate input from bus 0 channel 0; Trigger from bus 0 channel 1
    const float* gateIn = inBus.getNumChannels() > 0 ? inBus.getReadPointer (0) : nullptr;
    const float* trigIn = inBus.getNumChannels() > 1 ? inBus.getReadPointer (1) : nullptr;

    // Clear only our output bus before writing envelope
    auto out = getBusBuffer (buffer, false, 0);
    float* envOut = out.getWritePointer (0);
    float* invOut = out.getNumChannels() > 1 ? out.getWritePointer (1) : envOut;
    float* eorGate = out.getNumChannels() > 2 ? out.getWritePointer (2) : nullptr;
    float* eocGate = out.getNumChannels() > 3 ? out.getWritePointer (3) : nullptr;
    const int n = buffer.getNumSamples();

    for (int i = 0; i < n; ++i)
    {
        const bool trigHigh = trigIn != nullptr ? (trigIn[i] > 0.5f) : false;
        const bool gateHigh = gateIn != nullptr ? (gateIn[i] > 0.5f) : false;
        const bool trigRise = (trigHigh && ! lastTrigger);
        lastTrigger = trigHigh;

        if (trigRise) { stage = Stage::Attack; }
        else {
            if (gateHigh && ! lastGate) stage = Stage::Attack;
            else if (! gateHigh && lastGate && stage != Stage::Idle) stage = Stage::Release;
        }
        lastGate = gateHigh;
        const bool wasActive = (stage != Stage::Idle);

        // advance envelope one sample
        const float dt = 1.0f / (float) sr;
        switch (stage)
        {
            case Stage::Idle: envLevel = 0.0f; break;
            case Stage::Attack: {
                const float rate = (atkSec <= 0.0005f) ? 1.0f : dt / atkSec;
                envLevel += rate;
                if (envLevel >= 1.0f) { envLevel = 1.0f; stage = Stage::Decay; }
            } break;
            case Stage::Decay: {
                const float target = susLvl;
                const float rate = (decSec <= 0.0005f) ? 1.0f : dt / decSec;
                envLevel += (target - envLevel) * rate;
                if (std::abs(envLevel - target) < 0.0005f) { envLevel = target; stage = Stage::Sustain; }
            } break;
            case Stage::Sustain: {
                envLevel = susLvl;
                if (!gateHigh) stage = Stage::Release;
            } break;
            case Stage::Release: {
                const float rate = (relSec <= 0.0005f) ? 1.0f : dt / relSec;
                envLevel += (0.0f - envLevel) * rate;
                if (envLevel <= 0.0005f) { envLevel = 0.0f; stage = Stage::Idle; }
            } break;
        }
        const bool isActive = (stage != Stage::Idle);
        
        // EOR/EOC pulses
        if (wasActive && !isActive) { eorPending = (int) std::round (0.001f * (float)sr); }
        // EOC when we arrive at sustain level (end of attack/decay) OR when we return to Idle
        static bool atSustainPrev = false;
        const bool atSustainNow = (stage == Stage::Sustain);
        if (atSustainNow && !atSustainPrev) { eocPending = (int) std::round (0.001f * (float)sr); }
        if (!isActive && stage == Stage::Idle && wasActive) { eocPending = (int) std::round (0.001f * (float)sr); }
        atSustainPrev = atSustainNow;
        const float eorValue = (eorPending > 0 ? 1.0f : 0.0f);
        const float eocValue = (eocPending > 0 ? 1.0f : 0.0f);
        if (eorPending > 0) --eorPending;
        if (eocPending > 0) --eocPending;
        
        envOut[i] = envLevel;
        invOut[i] = 1.0f - envLevel;
        if (eorGate != nullptr) eorGate[i] = eorValue;
        if (eocGate != nullptr) eocGate[i] = eocValue;

#if defined(PRESET_CREATOR_UI)
        // Capture envelope into circular buffer
        if (vizEnvelopeBuffer.getNumSamples() > 0)
        {
            vizEnvelopeBuffer.setSample(0, vizWritePos, envLevel);
            vizWritePos = (vizWritePos + 1) % vizBufferSize;
        }
#endif
    }

    // Inspector values: use block peak magnitude to capture fast changes
    if (lastOutputValues.size() >= 4)
    {
        auto peakAbs = [&](int ch){ if (ch >= out.getNumChannels()) return 0.0f; const float* p = out.getReadPointer(ch); float m=0.0f; for (int i=0;i<n;++i) m = juce::jmax(m, std::abs(p[i])); return m; };
        if (lastOutputValues[0]) lastOutputValues[0]->store(peakAbs(0));
        if (lastOutputValues[1]) lastOutputValues[1]->store(peakAbs(1));
        if (lastOutputValues[2]) lastOutputValues[2]->store(peakAbs(2));
        if (lastOutputValues[3]) lastOutputValues[3]->store(peakAbs(3));
    }

    // Store live modulated values for UI display
    setLiveParamValue("attack_live", aEff);
    setLiveParamValue("decay_live", dEff);
    setLiveParamValue("sustain_live", sEff);
    setLiveParamValue("release_live", rEff);

#if defined(PRESET_CREATOR_UI)
    // Update visualization data (thread-safe)
    // Downsample envelope waveform from circular buffer
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx = (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) % vizBufferSize;
        const float sample = vizEnvelopeBuffer.getSample(0, readIdx);
        vizData.envelopeWaveform[i].store(sample);
    }

    // Update current state
    vizData.currentEnvelope.store(envLevel);
    vizData.currentStage.store(static_cast<int>(stage));
    vizData.gateActive.store(gateIn != nullptr && gateIn[n-1] > 0.5f);
    vizData.triggerActive.store(trigIn != nullptr && trigIn[n-1] > 0.5f);
    vizData.currentAttack.store(aEff);
    vizData.currentDecay.store(dEff);
    vizData.currentSustain.store(sEff);
    vizData.currentRelease.store(rEff);
#endif
}

// Parameter bus contract implementation
bool ADSRModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All inputs are on bus 0
    if (paramId == paramIdAttackMod)  { outChannelIndexInBus = 2; return true; }  // Attack Mod
    if (paramId == paramIdDecayMod)   { outChannelIndexInBus = 3; return true; }  // Decay Mod
    if (paramId == paramIdSustainMod) { outChannelIndexInBus = 4; return true; }  // Sustain Mod
    if (paramId == paramIdReleaseMod) { outChannelIndexInBus = 5; return true; }  // Release Mod
    return false;
}

#if defined(PRESET_CREATOR_UI)
// Helper function for tooltip with help marker
void ADSRModuleProcessor::HelpMarkerADSR(const char* desc)
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

void ADSRModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    ImGui::PushID(this);  // Prevent ImGui ID collisions between module instances
    
    auto& ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    
    // Get live modulated values for display
    bool isAttackModulated = isParamModulated(paramIdAttackMod);
    bool isDecayModulated = isParamModulated(paramIdDecayMod);
    bool isSustainModulated = isParamModulated(paramIdSustainMod);
    bool isReleaseModulated = isParamModulated(paramIdReleaseMod);
    
    float a = isAttackModulated ? getLiveParamValueFor("attack_mod", "attack_live", attackParam->load()) : (attackParam != nullptr ? attackParam->load() : 0.01f);
    float d = isDecayModulated ? getLiveParamValueFor("decay_mod", "decay_live", decayParam->load()) : (decayParam != nullptr ? decayParam->load() : 0.1f);
    float s = isSustainModulated ? getLiveParamValueFor("sustain_mod", "sustain_live", sustainParam->load()) : (sustainParam != nullptr ? sustainParam->load() : 0.7f);
    float r = isReleaseModulated ? getLiveParamValueFor("release_mod", "release_live", releaseParam->load()) : (releaseParam != nullptr ? releaseParam->load() : 0.2f);
    
    ImGui::PushItemWidth(itemWidth);
    
    // === ENVELOPE PARAMETERS SECTION ===
    ThemeText("Envelope Shape", theme.text.section_header);
    ImGui::Spacing();
    
    // Attack with inline pin
    if (isAttackModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))  // Channel 2 = Attack Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Attack (s)", &a, 0.001f, 5.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) if (!isAttackModulated) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdAttack))) *p = a;
    if (!isAttackModulated) adjustParamOnWheel(ap.getParameter(paramIdAttack), "attack", a);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isAttackModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerADSR("Attack time in seconds\nTime to reach peak from gate trigger");

    // Decay with inline pin
    if (isDecayModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(3))  // Channel 3 = Decay Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Decay (s)", &d, 0.001f, 5.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) if (!isDecayModulated) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdDecay))) *p = d;
    if (!isDecayModulated) adjustParamOnWheel(ap.getParameter(paramIdDecay), "decay", d);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isDecayModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerADSR("Decay time in seconds\nTime to reach sustain level");

    // Sustain with inline pin
    if (isSustainModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(4))  // Channel 4 = Sustain Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Sustain", &s, 0.0f, 1.0f)) if (!isSustainModulated) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdSustain))) *p = s;
    if (!isSustainModulated) adjustParamOnWheel(ap.getParameter(paramIdSustain), "sustain", s);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isSustainModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerADSR("Sustain level (0-1)\nLevel maintained while gate is held");

    // Release with inline pin
    if (isReleaseModulated) ImGui::BeginDisabled();
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(5))  // Channel 5 = Release Mod
            ImGui::SameLine();
    }
    if (ImGui::SliderFloat("Release (s)", &r, 0.001f, 5.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) if (!isReleaseModulated) if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter(paramIdRelease))) *p = r;
    if (!isReleaseModulated) adjustParamOnWheel(ap.getParameter(paramIdRelease), "release", r);
    if (ImGui::IsItemDeactivatedAfterEdit()) { onModificationEnded(); }
    if (isReleaseModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    ImGui::SameLine();
    HelpMarkerADSR("Release time in seconds\nTime to fade to zero after gate off");

    ImGui::Spacing();
    ImGui::Spacing();

    // === MODULATION MODE SECTION ===
    ThemeText("Modulation Mode", theme.text.section_header);
    ImGui::Spacing();
    
    bool relativeAttackMod = relativeAttackModParam ? (relativeAttackModParam->load() > 0.5f) : true;
    if (ImGui::Checkbox("Relative Attack Mod", &relativeAttackMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeAttackMod")))
        {
            *p = relativeAttackMod;
            juce::Logger::writeToLog("[ADSR UI] Relative Attack Mod changed to: " + juce::String(relativeAttackMod ? "TRUE" : "FALSE"));
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerADSR("Relative: CV scales around slider time (0.25x-4x)\nAbsolute: CV directly sets time (0.001s-5s)");

    bool relativeDecayMod = relativeDecayModParam ? (relativeDecayModParam->load() > 0.5f) : true;
    if (ImGui::Checkbox("Relative Decay Mod", &relativeDecayMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeDecayMod")))
        {
            *p = relativeDecayMod;
            juce::Logger::writeToLog("[ADSR UI] Relative Decay Mod changed to: " + juce::String(relativeDecayMod ? "TRUE" : "FALSE"));
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerADSR("Relative: CV scales around slider time (0.25x-4x)\nAbsolute: CV directly sets time (0.001s-5s)");

    bool relativeSustainMod = relativeSustainModParam ? (relativeSustainModParam->load() > 0.5f) : true;
    if (ImGui::Checkbox("Relative Sustain Mod", &relativeSustainMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeSustainMod")))
        {
            *p = relativeSustainMod;
            juce::Logger::writeToLog("[ADSR UI] Relative Sustain Mod changed to: " + juce::String(relativeSustainMod ? "TRUE" : "FALSE"));
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerADSR("Relative: CV adds offset to slider (±0.5)\nAbsolute: CV directly sets level (0-1)");

    bool relativeReleaseMod = relativeReleaseModParam ? (relativeReleaseModParam->load() > 0.5f) : true;
    if (ImGui::Checkbox("Relative Release Mod", &relativeReleaseMod))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("relativeReleaseMod")))
        {
            *p = relativeReleaseMod;
            juce::Logger::writeToLog("[ADSR UI] Relative Release Mod changed to: " + juce::String(relativeReleaseMod ? "TRUE" : "FALSE"));
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    ImGui::SameLine();
    HelpMarkerADSR("Relative: CV scales around slider time (0.25x-4x)\nAbsolute: CV directly sets time (0.001s-5s)");

    ImGui::Spacing();
    ImGui::Spacing();

    // === REAL-TIME ENVELOPE VISUALIZATION ===
    ThemeText("Envelope Output", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushID(this); // Unique ID for this node's UI
    auto* drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 envelopeColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
    const ImU32 gateColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 triggerColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);

    // Real-time envelope waveform
    const ImVec2 waveOrigin = ImGui::GetCursorScreenPos();
    const float waveHeight = 100.0f;
    const ImVec2 waveMax = ImVec2(waveOrigin.x + itemWidth, waveOrigin.y + waveHeight);
    drawList->AddRectFilled(waveOrigin, waveMax, bgColor, 4.0f);
    ImGui::PushClipRect(waveOrigin, waveMax, true);

    // Read visualization data (thread-safe)
    float envelopeWaveform[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
        envelopeWaveform[i] = vizData.envelopeWaveform[i].load();

    const float midY = waveOrigin.y + waveHeight * 0.5f;
    const float scaleY = waveHeight * 0.45f;
    const float stepX = itemWidth / (float)(VizData::waveformPoints - 1);

    // Draw center line
    drawList->AddLine(ImVec2(waveOrigin.x, midY), ImVec2(waveMax.x, midY), IM_COL32(150, 150, 150, 100), 1.0f);

    // Draw envelope waveform
    float prevX = waveOrigin.x;
    float prevY = midY;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const float sample = juce::jlimit(0.0f, 1.0f, envelopeWaveform[i]);
        const float x = waveOrigin.x + i * stepX;
        const float y = midY - sample * scaleY;
        if (i > 0)
            drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), envelopeColor, 2.5f);
        prevX = x;
        prevY = y;
    }

    // Draw sustain level line
    const float sustainLevel = vizData.currentSustain.load();
    const float sustainY = midY - sustainLevel * scaleY;
    drawList->AddLine(ImVec2(waveOrigin.x, sustainY), ImVec2(waveMax.x, sustainY), 
                     IM_COL32(255, 255, 255, 80), 1.0f);

    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(waveOrigin.x, waveMax.y));
    ImGui::Dummy(ImVec2(itemWidth, 0));

    ImGui::Spacing();

    // Gate/Trigger status indicators
    const bool gateActive = vizData.gateActive.load();
    const bool triggerActive = vizData.triggerActive.load();
    auto drawStatus = [&](const char* label, bool isActive, ImU32 color)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float radius = 6.0f;
        const ImU32 indicatorColor = isActive ? color : IM_COL32(90, 90, 90, 160);
        drawList->AddCircleFilled(ImVec2(pos.x + radius, pos.y + radius), radius, indicatorColor, 24);
        ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    };

    ImGui::BeginGroup();
    drawStatus("Gate", gateActive, gateColor);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 4.0f);
    drawStatus("Trigger", triggerActive, triggerColor);
    ImGui::EndGroup();

    ImGui::Spacing();

    // Current envelope value and stage
    const float currentEnvValue = vizData.currentEnvelope.load();
    const int currentStageIdx = vizData.currentStage.load();
    const char* stageNames[] = { "Idle", "Attack", "Decay", "Sustain", "Release" };
    const char* stageName = (currentStageIdx >= 0 && currentStageIdx < 5) ? stageNames[currentStageIdx] : "Unknown";
    
    ImGui::Text("Current: %.3f", currentEnvValue);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.7f, 1.0f));
    ImGui::Text("Stage: %s", stageName);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Spacing();

    // === ENVELOPE PREVIEW (Reference Shape) ===
    ThemeText("Envelope Preview", theme.text.section_header);
    ImGui::Spacing();

    // Generate ADSR curve for visualization
    float adsrCurve[100];
    float totalTime = a + d + r + 0.5f; // Add 0.5s for sustain visualization
    float timePerSample = totalTime / 100.0f;
    
    for (int i = 0; i < 100; ++i)
    {
        float t = i * timePerSample;
        if (t < a)
        {
            // Attack phase
            adsrCurve[i] = t / a;
        }
        else if (t < a + d)
        {
            // Decay phase
            float decayProgress = (t - a) / d;
            adsrCurve[i] = 1.0f + decayProgress * (s - 1.0f);
        }
        else if (t < a + d + 0.5f)
        {
            // Sustain phase (0.5s duration for visualization)
            adsrCurve[i] = s;
        }
        else
        {
            // Release phase
            float releaseProgress = (t - a - d - 0.5f) / r;
            adsrCurve[i] = s * (1.0f - releaseProgress);
        }
        adsrCurve[i] = juce::jlimit(0.0f, 1.0f, adsrCurve[i]);
    }

    ImGui::PushStyleColor(ImGuiCol_PlotLines, envelopeColor);
    ImGui::PlotLines("##envelopePreview", adsrCurve, 100, 0, nullptr, 0.0f, 1.0f, ImVec2(itemWidth, 60));
    ImGui::PopStyleColor();

    ImGui::PopItemWidth();
    ImGui::PopID(); // End visualization child window ID
    ImGui::PopID(); // End module instance ID
}
#endif


