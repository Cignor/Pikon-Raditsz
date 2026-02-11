#include "PolymetricSlicerModuleProcessor.h"
#include "../graph/ModularSynthProcessor.h"
#include <cmath>

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include "../../preset_creator/theme/ThemeManager.h"
#include "../../preset_creator/ControllerPresetManager.h"
#endif

using APVTS = juce::AudioProcessorValueTreeState;

// ═══════════════════════════════════════════════════════════════════════
// Parameter Layout
// ═══════════════════════════════════════════════════════════════════════

APVTS::ParameterLayout PolymetricSlicerModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Transport sync
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("sync", "Sync to Transport", false));
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "rate_division",
            "Division",
            juce::StringArray{"1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8"},
            3));

    // Free-running rate (Hz)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "rate", "Rate", juce::NormalisableRange<float>(0.1f, 20.0f, 0.01f, 0.5f), 2.0f));

    // Gate length (proportion of step that gate is high)
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "gateLength", "Gate Length", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));

    // Per-timeline: number of bars (1–8), active steps (1–32), active rows (1–12)
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        const juce::String ts = juce::String(t + 1);
        params.push_back(
            std::make_unique<juce::AudioParameterInt>(
                "bars_" + ts, "Bars TL" + ts, 1, 8, (t + 1))); // Default: 1,2,3,4
        params.push_back(
            std::make_unique<juce::AudioParameterInt>(
                "steps_" + ts, "Steps TL" + ts, 1, MAX_STEPS, 16));
        params.push_back(
            std::make_unique<juce::AudioParameterInt>(
                "rows_" + ts, "Rows TL" + ts, 1, MAX_ROWS, 8));
    }

    return {params.begin(), params.end()};
}

// ═══════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════

PolymetricSlicerModuleProcessor::PolymetricSlicerModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Inputs", juce::AudioChannelSet::discreteChannels(TOTAL_IN_CHANNELS), true)
              .withOutput(
                  "Outputs",
                  juce::AudioChannelSet::discreteChannels(TOTAL_OUT_CHANNELS),
                  true)),
      apvts(*this, nullptr, "PolySlicerParams", createParameterLayout())
{
    // Zero-init all grid state
    for (auto& tl : gridState)
        for (auto& row : tl)
            row.fill(false);

    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        currentStep[t].store(0);
        phase[t] = 0.0;
        pendingTriggerSamples[t] = 0;
        previousGateOn[t] = false;
        gateFadeProgress[t] = 0.0f;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Prepare / Release
// ═══════════════════════════════════════════════════════════════════════

void PolymetricSlicerModuleProcessor::prepareToPlay(double newSampleRate, int /*samplesPerBlock*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    for (int t = 0; t < NUM_TIMELINES; ++t)
        phase[t] = 0.0;
}

// ═══════════════════════════════════════════════════════════════════════
// Transport Sync
// ═══════════════════════════════════════════════════════════════════════

void PolymetricSlicerModuleProcessor::setTimingInfo(const TransportState& state)
{
    ModuleProcessor::setTimingInfo(state);

    const TransportCommand command = state.lastCommand.load();
    if (command != lastTransportCommand)
    {
        if (command == TransportCommand::Stop)
        {
            for (int t = 0; t < NUM_TIMELINES; ++t)
            {
                currentStep[t].store(0);
                phase[t] = 0.0;
            }
        }
        lastTransportCommand = command;
    }

    wasPlaying = state.isPlaying;
    m_currentTransport = state;
}

void PolymetricSlicerModuleProcessor::forceStop()
{
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        currentStep[t].store(0);
        phase[t] = 0.0;
        pendingTriggerSamples[t] = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Process Block — Core DSP
// ═══════════════════════════════════════════════════════════════════════

void PolymetricSlicerModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    juce::ignoreUnused(midi);
    const int numSamples = buffer.getNumSamples();
    const int numOutCh = buffer.getNumChannels();

    // Read global parameters
    const bool  syncEnabled = apvts.getRawParameterValue("sync")->load() > 0.5f;
    const float rate = apvts.getRawParameterValue("rate")->load();
    const float gateLen = apvts.getRawParameterValue("gateLength")->load();

    // Check Global Reset
    if (m_currentTransport.forceGlobalReset.load())
    {
        for (int t = 0; t < NUM_TIMELINES; ++t)
        {
            currentStep[t].store(0);
            phase[t] = 0.0;
        }
    }

    // Read per-timeline parameters
    std::array<int, NUM_TIMELINES> bars{};
    std::array<int, NUM_TIMELINES> activeSteps{};
    std::array<int, NUM_TIMELINES> activeRows{};

    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        const juce::String ts = juce::String(t + 1);
        bars[t] = juce::jlimit(1, 8, (int)apvts.getRawParameterValue("bars_" + ts)->load());
        activeSteps[t] =
            juce::jlimit(1, MAX_STEPS, (int)apvts.getRawParameterValue("steps_" + ts)->load());
        activeRows[t] =
            juce::jlimit(1, MAX_ROWS, (int)apvts.getRawParameterValue("rows_" + ts)->load());
    }

    // Get input bus
    const auto& inputBus = getBusBuffer(buffer, true, 0);

    // Process each sample
    for (int i = 0; i < numSamples; ++i)
    {
        for (int t = 0; t < NUM_TIMELINES; ++t)
        {
            bool stepAdvanced = false;

            // Wrap current step if it exceeds active steps
            if (currentStep[t].load() >= activeSteps[t])
                currentStep[t].store(0);

            // --- Step Advancement ---
            if (syncEnabled && m_currentTransport.isPlaying)
            {
                // SYNC MODE: derive step from song position
                int divisionIndex = (int)apvts.getRawParameterValue("rate_division")->load();
                if (getParent())
                {
                    int globalDiv = getParent()->getTransportState().globalDivisionIndex.load();
                    if (globalDiv >= 0)
                        divisionIndex = globalDiv;
                }

                static const double divisions[] = {
                    1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0, 2.0, 4.0, 8.0};
                const double beatDivision = divisions[juce::jlimit(0, 8, divisionIndex)];

                // Calculate total beats in this timeline's bar pattern
                const double beatsPerPattern = bars[t] * 4.0; // 4 beats per bar
                const double posInPattern =
                    std::fmod(m_currentTransport.songPositionBeats, beatsPerPattern);

                // Map position to step
                const int totalSteps = juce::jlimit(1, MAX_STEPS, activeSteps[t]);
                const int stepForBeat =
                    static_cast<int>(std::fmod(posInPattern * beatDivision, totalSteps));

                if (stepForBeat != currentStep[t].load())
                {
                    currentStep[t].store(stepForBeat);
                    stepAdvanced = true;
                }
            }
            else
            {
                // FREE-RUNNING MODE
                if (m_currentTransport.isPlaying)
                {
                    const double phaseInc = (sampleRate > 0.0 ? (double)rate / sampleRate : 0.0);
                    phase[t] += phaseInc;
                    if (phase[t] >= 1.0)
                    {
                        phase[t] -= 1.0;
                        const int next = (currentStep[t].load() + 1) %
                                         juce::jlimit(1, MAX_STEPS, activeSteps[t]);
                        currentStep[t].store(next);
                        stepAdvanced = true;
                    }
                }
            }

            const int step = currentStep[t].load();

            // --- Determine highest active row for this step (pitch) ---
            int  highestActiveRow = -1;
            bool anyRowActive = false;
            for (int r = activeRows[t] - 1; r >= 0; --r)
            {
                if (gridState[t][r][step])
                {
                    highestActiveRow = r;
                    anyRowActive = true;
                    break;
                }
            }

            // --- CV Outputs ---
            // Pitch: normalized 0..1 based on row position
            const float pitchValue = anyRowActive ? (float)highestActiveRow /
                                                        juce::jmax(1.0f, (float)(activeRows[t] - 1))
                                                  : 0.0f;

            // Gate
            const float fadeIncrement =
                sampleRate > 0.0f ? (1000.0f / GATE_FADE_TIME_MS) / (float)sampleRate : 0.0f;

            if (anyRowActive && !previousGateOn[t])
                gateFadeProgress[t] = 0.0f;
            else if (!anyRowActive && previousGateOn[t])
                gateFadeProgress[t] = 0.0f;

            gateFadeProgress[t] = juce::jmin(1.0f, gateFadeProgress[t] + fadeIncrement);
            const float fadeMultiplier =
                anyRowActive ? gateFadeProgress[t] : (1.0f - gateFadeProgress[t]);

            const float gateValue =
                (m_currentTransport.isPlaying && anyRowActive && phase[t] < gateLen)
                    ? fadeMultiplier
                    : 0.0f;
            previousGateOn[t] = anyRowActive;

            // Trigger
            if (stepAdvanced)
                pendingTriggerSamples[t] = anyRowActive ? (int)std::round(0.001 * sampleRate) : 0;

            float trigValue = 0.0f;
            if (m_currentTransport.isPlaying && pendingTriggerSamples[t] > 0)
            {
                trigValue = 1.0f;
                --pendingTriggerSamples[t];
            }

            // Write CV outputs
            const int cvBaseChannel = AUDIO_OUT_CHANNELS + t * CV_OUT_PER_TIMELINE;
            if (cvBaseChannel + 0 < numOutCh)
                buffer.setSample(cvBaseChannel + 0, i, pitchValue);
            if (cvBaseChannel + 1 < numOutCh)
                buffer.setSample(cvBaseChannel + 1, i, gateValue);
            if (cvBaseChannel + 2 < numOutCh)
                buffer.setSample(cvBaseChannel + 2, i, trigValue);

            // --- Audio Slicing ---
            // Gate the stereo input pair through to the stereo output pair
            const int audioInL = t * 2;
            const int audioInR = t * 2 + 1;
            const int audioOutL = t * 2;
            const int audioOutR = t * 2 + 1;

            const float sliceGain =
                (m_currentTransport.isPlaying && anyRowActive) ? gateValue : 0.0f;

            if (audioOutL < numOutCh)
            {
                const float inSample =
                    (audioInL < inputBus.getNumChannels()) ? inputBus.getSample(audioInL, i) : 0.0f;
                buffer.setSample(audioOutL, i, inSample * sliceGain);
            }
            if (audioOutR < numOutCh)
            {
                const float inSample =
                    (audioInR < inputBus.getNumChannels()) ? inputBus.getSample(audioInR, i) : 0.0f;
                buffer.setSample(audioOutR, i, inSample * sliceGain);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// State Persistence (Extra State Tree — grid + presets)
// ═══════════════════════════════════════════════════════════════════════

juce::ValueTree PolymetricSlicerModuleProcessor::getExtraStateTree() const
{
    juce::ValueTree vt("PolySlicerState");

    // Save sync settings
    vt.setProperty("sync", apvts.getRawParameterValue("sync")->load(), nullptr);
    vt.setProperty("rate_division", apvts.getRawParameterValue("rate_division")->load(), nullptr);

    // Save grid state for each timeline
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        juce::ValueTree tlTree("Timeline_" + juce::String(t));

        for (int r = 0; r < MAX_ROWS; ++r)
        {
            juce::String rowData;
            for (int s = 0; s < MAX_STEPS; ++s)
                rowData += gridState[t][r][s] ? "1" : "0";

            tlTree.setProperty("row_" + juce::String(r), rowData, nullptr);
        }

        vt.addChild(tlTree, -1, nullptr);
    }

#if defined(PRESET_CREATOR_UI)
    vt.setProperty("activePresetName", activePresetName, nullptr);
    vt.setProperty("selectedPresetIndex", selectedPresetIndex, nullptr);
    vt.setProperty("selectedStandardPresetIndex", selectedStandardPresetIndex, nullptr);
#endif

    return vt;
}

void PolymetricSlicerModuleProcessor::setExtraStateTree(const juce::ValueTree& vt)
{
    if (!vt.hasType("PolySlicerState"))
        return;

    // Restore sync settings
    if (auto* p = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("sync")))
        *p = (bool)vt.getProperty("sync", false);
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("rate_division")))
        *p = (int)vt.getProperty("rate_division", 3);

    // Restore grid state
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        auto tlTree = vt.getChildWithName("Timeline_" + juce::String(t));
        if (!tlTree.isValid())
            continue;

        for (int r = 0; r < MAX_ROWS; ++r)
        {
            juce::String rowData = tlTree.getProperty("row_" + juce::String(r), "").toString();

            for (int s = 0; s < MAX_STEPS && s < rowData.length(); ++s)
                gridState[t][r][s] = (rowData[s] == '1');
        }
    }

#if defined(PRESET_CREATOR_UI)
    activePresetName = vt.getProperty("activePresetName", "").toString();
    selectedPresetIndex = (int)vt.getProperty("selectedPresetIndex", -1);
    selectedStandardPresetIndex = (int)vt.getProperty("selectedStandardPresetIndex", 0);
#endif
}

// ═══════════════════════════════════════════════════════════════════════
// Pin Labels
// ═══════════════════════════════════════════════════════════════════════

juce::String PolymetricSlicerModuleProcessor::getAudioOutputLabel(int channel) const
{
    // Channels 0..7: Sliced audio
    if (channel < AUDIO_OUT_CHANNELS)
    {
        const int tl = channel / 2 + 1;
        return (channel % 2 == 0) ? "TL" + juce::String(tl) + " Audio L"
                                  : "TL" + juce::String(tl) + " Audio R";
    }

    // Channels 8..19: CV outputs
    const int cvIndex = channel - AUDIO_OUT_CHANNELS;
    if (cvIndex >= 0 && cvIndex < CV_OUT_CHANNELS)
    {
        const int tl = cvIndex / CV_OUT_PER_TIMELINE + 1;
        const int type = cvIndex % CV_OUT_PER_TIMELINE;
        switch (type)
        {
        case 0:
            return "TL" + juce::String(tl) + " Pitch";
        case 1:
            return "TL" + juce::String(tl) + " Gate";
        case 2:
            return "TL" + juce::String(tl) + " Trigger";
        }
    }

    return "Out " + juce::String(channel + 1);
}

juce::String PolymetricSlicerModuleProcessor::getAudioInputLabel(int channel) const
{
    // Channels 0..7: Audio inputs
    if (channel < AUDIO_IN_CHANNELS)
    {
        const int tl = channel / 2 + 1;
        return (channel % 2 == 0) ? "TL" + juce::String(tl) + " In L"
                                  : "TL" + juce::String(tl) + " In R";
    }

    // Channels 8..11: Mod inputs
    if (channel < TOTAL_IN_CHANNELS)
    {
        const int tl = channel - AUDIO_IN_CHANNELS + 1;
        return "TL" + juce::String(tl) + " Mod";
    }

    return "In " + juce::String(channel + 1);
}

// ═══════════════════════════════════════════════════════════════════════
// UI — Draw Parameters In Node
// ═══════════════════════════════════════════════════════════════════════

#if defined(PRESET_CREATOR_UI)

void PolymetricSlicerModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    auto&       ap = getAPVTS();
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushID(this);

    // ─── PRESET SECTION ─────────────────────────────────────────────
    auto&       presetManager = ControllerPresetManager::get();
    const auto& savedPresetNames =
        presetManager.getPresetNamesFor(ControllerPresetManager::ModuleType::PolymetricSlicer);

    // Standard preset names
    static const char* standardPresets[] = {"Empty", "Four on Floor", "Tresillo", "Euclidean 5/8"};
    static constexpr int numStandardPresets = 4;

    // Build combo items
    std::vector<const char*> comboItems;
    for (int i = 0; i < numStandardPresets; ++i)
        comboItems.push_back(standardPresets[i]);

    if (savedPresetNames.size() > 0)
    {
        comboItems.push_back("---"); // separator
        for (const auto& name : savedPresetNames)
            comboItems.push_back(name.toRawUTF8());
    }

    int comboSelection = 0;
    if (selectedPresetIndex >= 0 && selectedPresetIndex < savedPresetNames.size())
        comboSelection = numStandardPresets + 1 + selectedPresetIndex;
    else
        comboSelection = juce::jlimit(0, numStandardPresets - 1, selectedStandardPresetIndex);

    auto applyPresetSelection = [&](int selection) {
        if (selection < numStandardPresets)
        {
            // Apply standard preset: clear grid then set pattern
            for (auto& tl : gridState)
                for (auto& row : tl)
                    row.fill(false);

            if (selection == 1) // Four on Floor
            {
                for (int t = 0; t < NUM_TIMELINES; ++t)
                    for (int s = 0; s < MAX_STEPS; s += 4)
                        gridState[t][0][s] = true;
            }
            else if (selection == 2) // Tresillo
            {
                // 3+3+2 pattern on row 0
                gridState[0][0][0] = true;
                gridState[0][0][3] = true;
                gridState[0][0][6] = true;
            }
            else if (selection == 3) // Euclidean 5/8
            {
                gridState[0][0][0] = true;
                gridState[0][0][2] = true;
                gridState[0][0][3] = true;
                gridState[0][0][5] = true;
                gridState[0][0][6] = true;
            }

            selectedPresetIndex = -1;
            selectedStandardPresetIndex = selection;
            activePresetName = "";
            onModificationEnded();
        }
        else if (selection > numStandardPresets)
        {
            int savedIndex = selection - numStandardPresets - 1;
            if (savedIndex >= 0 && savedIndex < savedPresetNames.size())
            {
                activePresetName = savedPresetNames[savedIndex];
                juce::ValueTree presetData = presetManager.loadPreset(
                    ControllerPresetManager::ModuleType::PolymetricSlicer, activePresetName);
                setExtraStateTree(presetData);
                selectedPresetIndex = savedIndex;
                selectedStandardPresetIndex = -1;
                onModificationEnded();
            }
        }
    };

    ThemeText("PRESETS", theme.text.section_header);
    ImGui::SetNextItemWidth(itemWidth * 0.5f);
    if (ImGui::Combo(
            "##PolySlicerPreset", &comboSelection, comboItems.data(), (int)comboItems.size()))
    {
        applyPresetSelection(comboSelection);
    }

    // Scroll-edit support
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const int maxIndex = (int)comboItems.size() - 1;
            const int delta = wheel > 0.0f ? -1 : 1;
            int       newSelection = juce::jlimit(0, maxIndex, comboSelection + delta);
            if (newSelection == numStandardPresets)
                newSelection = wheel > 0.0f ? numStandardPresets - 1 : numStandardPresets + 1;
            newSelection = juce::jlimit(0, maxIndex, newSelection);
            if (newSelection != comboSelection)
                applyPresetSelection(newSelection);
        }
    }

    // Save / Delete buttons
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        ImGui::OpenPopup("SavePolySlicerPresetPopup");

    if (ImGui::BeginPopup("SavePolySlicerPresetPopup"))
    {
        ImGui::Text("Preset Name:");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("##presetName", presetNameBuffer, sizeof(presetNameBuffer));

        if (ImGui::Button("OK") && strlen(presetNameBuffer) > 0)
        {
            juce::ValueTree presetData = getExtraStateTree();
            presetManager.savePreset(
                ControllerPresetManager::ModuleType::PolymetricSlicer,
                juce::String(presetNameBuffer),
                presetData);
            activePresetName = juce::String(presetNameBuffer);
            presetNameBuffer[0] = '\0';
            onModificationEnded();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (activePresetName.isNotEmpty() && ImGui::Button("Delete"))
    {
        presetManager.deletePreset(
            ControllerPresetManager::ModuleType::PolymetricSlicer, activePresetName);
        activePresetName = "";
        selectedPresetIndex = -1;
        selectedStandardPresetIndex = 0;
        onModificationEnded();
    }

    ImGui::Spacing();

    // ─── SYNC CONTROLS ──────────────────────────────────────────────
    bool sync = apvts.getRawParameterValue("sync")->load() > 0.5f;
    if (ImGui::Checkbox("Sync", &sync))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(ap.getParameter("sync")))
            *p = sync;
        onModificationEnded();
    }

    ImGui::SameLine();
    ImGui::PushItemWidth(itemWidth * 0.3f);
    if (sync)
    {
        int globalDiv =
            getParent() ? getParent()->getTransportState().globalDivisionIndex.load() : -1;
        bool isGlobalDiv = globalDiv >= 0;
        int  division =
            isGlobalDiv ? globalDiv : (int)apvts.getRawParameterValue("rate_division")->load();

        if (isGlobalDiv)
            ImGui::BeginDisabled();
        if (ImGui::Combo(
                "Div",
                &division,
                "1/32\0"
                "1/16\0"
                "1/8\0"
                "1/4\0"
                "1/2\0"
                "1\0"
                "2\0"
                "4\0"
                "8\0\0"))
        {
            if (!isGlobalDiv)
                if (auto* p =
                        dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("rate_division")))
                    *p = division;
            onModificationEnded();
        }
        if (isGlobalDiv)
            ImGui::EndDisabled();
    }
    else
    {
        const bool isModulated = isParamModulated("rate");
        float      rateDisplay = apvts.getRawParameterValue("rate")->load();
        if (isModulated)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Rate", &rateDisplay, 0.1f, 20.0f, "%.2f Hz"))
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("rate")))
                *p = rateDisplay;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            onModificationEnded();
        if (!isModulated)
            adjustParamOnWheel(ap.getParameter("rate"), "rate", rateDisplay);
        if (isModulated)
            ImGui::EndDisabled();
    }
    ImGui::PopItemWidth();

    // Gate length
    ImGui::PushItemWidth(itemWidth * 0.4f);
    const bool isGateModulated = isParamModulated("gateLength");
    float      gateLenDisplay = apvts.getRawParameterValue("gateLength")->load();
    if (isGateModulated)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Gate", &gateLenDisplay, 0.0f, 1.0f, "%.2f"))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("gateLength")))
            *p = gateLenDisplay;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (!isGateModulated)
        adjustParamOnWheel(ap.getParameter("gateLength"), "gateLength", gateLenDisplay);
    if (isGateModulated)
        ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::Spacing();

    // ─── TIMELINE TABS ──────────────────────────────────────────────
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        if (t > 0)
            ImGui::SameLine();

        const bool isSelected = (t == activeTimeline);
        if (isSelected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        }

        const juce::String label = "TL " + juce::String(t + 1);
        if (ImGui::Button(label.toRawUTF8(), ImVec2(itemWidth / NUM_TIMELINES - 4.0f, 0)))
            activeTimeline = t;

        if (isSelected)
            ImGui::PopStyleColor(2);
    }

    // Per-timeline controls
    const juce::String ts = juce::String(activeTimeline + 1);

    ImGui::PushItemWidth(itemWidth * 0.3f);

    // Bars
    const juce::String barsParamId = "bars_" + ts;
    int                barsVal = (int)apvts.getRawParameterValue(barsParamId)->load();
    const bool         isBarsModulated = isParamModulated(barsParamId);
    if (isBarsModulated)
        ImGui::BeginDisabled();
    if (ImGui::SliderInt("Bars", &barsVal, 1, 8))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(barsParamId)))
            *p = barsVal;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (!isBarsModulated)
        adjustParamOnWheel(ap.getParameter(barsParamId), barsParamId, (float)barsVal);
    if (isBarsModulated)
        ImGui::EndDisabled();

    ImGui::SameLine();

    // Steps
    const juce::String stepsParamId = "steps_" + ts;
    int                stepsVal = (int)apvts.getRawParameterValue(stepsParamId)->load();
    const bool         isStepsModulated = isParamModulated(stepsParamId);
    if (isStepsModulated)
        ImGui::BeginDisabled();
    if (ImGui::SliderInt("Steps", &stepsVal, 1, MAX_STEPS))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(stepsParamId)))
            *p = stepsVal;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (!isStepsModulated)
        adjustParamOnWheel(ap.getParameter(stepsParamId), stepsParamId, (float)stepsVal);
    if (isStepsModulated)
        ImGui::EndDisabled();

    ImGui::SameLine();

    // Rows
    const juce::String rowsParamId = "rows_" + ts;
    int                rowsVal = (int)apvts.getRawParameterValue(rowsParamId)->load();
    const bool         isRowsModulated = isParamModulated(rowsParamId);
    if (isRowsModulated)
        ImGui::BeginDisabled();
    if (ImGui::SliderInt("Rows", &rowsVal, 1, MAX_ROWS))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter(rowsParamId)))
            *p = rowsVal;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    if (!isRowsModulated)
        adjustParamOnWheel(ap.getParameter(rowsParamId), rowsParamId, (float)rowsVal);
    if (isRowsModulated)
        ImGui::EndDisabled();

    ImGui::PopItemWidth();

    ImGui::Spacing();

    // ─── CHECKERBOARD GRID ──────────────────────────────────────────
    const int tl = activeTimeline;
    const int shownSteps = juce::jlimit(1, MAX_STEPS, stepsVal);
    const int shownRows = juce::jlimit(1, MAX_ROWS, rowsVal);

    const float spacing = 2.0f;
    const float cellW =
        juce::jmax(8.0f, (itemWidth - spacing * (shownSteps + 1)) / (float)shownSteps);
    const float cellH = juce::jmax(8.0f, juce::jmin(16.0f, cellW));

    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
    const float  gridWidth = shownSteps * (cellW + spacing) + spacing;
    const float  gridHeight = shownRows * (cellH + spacing) + spacing;

    // Background
    drawList->AddRectFilled(
        gridOrigin,
        ImVec2(gridOrigin.x + gridWidth, gridOrigin.y + gridHeight),
        IM_COL32(20, 20, 30, 255),
        4.0f);

    // Draw cells (rows from top = highest pitch, bottom = lowest)
    for (int r = 0; r < shownRows; ++r)
    {
        const int rowIndex = shownRows - 1 - r; // flip so row 0 is at bottom
        for (int s = 0; s < shownSteps; ++s)
        {
            const float x = gridOrigin.x + spacing + s * (cellW + spacing);
            const float y = gridOrigin.y + spacing + r * (cellH + spacing);

            const bool isActive = gridState[tl][rowIndex][s];

            // Checkerboard base color
            const bool isLight = ((r + s) % 2 == 0);
            ImU32      cellColor;

            if (isActive)
            {
                // Active cell — timeline-specific colors
                static const ImU32 tlColors[NUM_TIMELINES] = {
                    IM_COL32(100, 180, 255, 220), // TL1: Blue
                    IM_COL32(255, 140, 80, 220),  // TL2: Orange
                    IM_COL32(100, 220, 140, 220), // TL3: Green
                    IM_COL32(220, 100, 220, 220)  // TL4: Purple
                };
                cellColor = tlColors[tl];
            }
            else
            {
                cellColor = isLight ? IM_COL32(45, 45, 60, 255) : IM_COL32(35, 35, 50, 255);
            }

            drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cellW, y + cellH), cellColor, 2.0f);

            // Click detection
            const ImVec2 cellMin(x, y);
            const ImVec2 cellMax(x + cellW, y + cellH);
            if (ImGui::IsMouseHoveringRect(cellMin, cellMax) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                gridState[tl][rowIndex][s] = !gridState[tl][rowIndex][s];
                onModificationEnded();
            }
        }
    }

    // Draw playhead
    if (m_currentTransport.isPlaying)
    {
        const int playStep = currentStep[tl].load();
        if (playStep < shownSteps)
        {
            const float phX = gridOrigin.x + spacing + playStep * (cellW + spacing) + cellW * 0.5f;
            drawList->AddLine(
                ImVec2(phX, gridOrigin.y),
                ImVec2(phX, gridOrigin.y + gridHeight),
                IM_COL32(255, 80, 180, 200),
                2.0f);
        }
    }

    // Reserve space for the grid
    ImGui::Dummy(ImVec2(gridWidth, gridHeight));

    ImGui::Spacing();

    // ─── TIMELINE OVERVIEW (all 4 timelines) ────────────────────────
    ThemeText("TIMELINES", theme.text.section_header);

    const float tlBarHeight = 14.0f;
    const float tlSpacing = 3.0f;
    const float tlTotalWidth = itemWidth;

    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        const ImVec2 tlOrigin = ImGui::GetCursorScreenPos();
        const int    tlSteps = juce::jlimit(
            1, MAX_STEPS, (int)apvts.getRawParameterValue("steps_" + juce::String(t + 1))->load());
        const float stepW = tlTotalWidth / (float)tlSteps;

        // Per-timeline color
        static const ImU32 tlBgColors[NUM_TIMELINES] = {
            IM_COL32(40, 60, 100, 180),
            IM_COL32(100, 55, 30, 180),
            IM_COL32(30, 80, 50, 180),
            IM_COL32(80, 30, 80, 180)};
        static const ImU32 tlActiveColors[NUM_TIMELINES] = {
            IM_COL32(80, 140, 220, 220),
            IM_COL32(220, 120, 60, 220),
            IM_COL32(60, 180, 100, 220),
            IM_COL32(180, 80, 180, 220)};

        drawList->AddRectFilled(
            tlOrigin,
            ImVec2(tlOrigin.x + tlTotalWidth, tlOrigin.y + tlBarHeight),
            tlBgColors[t],
            3.0f);

        // Draw active cells as highlights (just row 0 for compact view)
        for (int s = 0; s < tlSteps; ++s)
        {
            bool      hasAnyActive = false;
            const int tlRows = juce::jlimit(
                1,
                MAX_ROWS,
                (int)apvts.getRawParameterValue("rows_" + juce::String(t + 1))->load());
            for (int r = 0; r < tlRows; ++r)
            {
                if (gridState[t][r][s])
                {
                    hasAnyActive = true;
                    break;
                }
            }

            if (hasAnyActive)
            {
                drawList->AddRectFilled(
                    ImVec2(tlOrigin.x + s * stepW + 1.0f, tlOrigin.y + 1.0f),
                    ImVec2(tlOrigin.x + (s + 1) * stepW - 1.0f, tlOrigin.y + tlBarHeight - 1.0f),
                    tlActiveColors[t],
                    2.0f);
            }
        }

        // Playhead
        if (m_currentTransport.isPlaying)
        {
            const int playStep = currentStep[t].load();
            if (playStep < tlSteps)
            {
                const float phX = tlOrigin.x + playStep * stepW + stepW * 0.5f;
                drawList->AddLine(
                    ImVec2(phX, tlOrigin.y),
                    ImVec2(phX, tlOrigin.y + tlBarHeight),
                    IM_COL32(255, 255, 255, 200),
                    2.0f);
            }
        }

        // Label
        const juce::String tlLabel =
            "TL" + juce::String(t + 1) + " (" +
            juce::String((int)apvts.getRawParameterValue("bars_" + juce::String(t + 1))->load()) +
            "b)";
        drawList->AddText(
            ImVec2(tlOrigin.x + 4.0f, tlOrigin.y + 1.0f),
            IM_COL32(255, 255, 255, 180),
            tlLabel.toRawUTF8());

        ImGui::Dummy(ImVec2(tlTotalWidth, tlBarHeight + tlSpacing));
    }

    ImGui::PopID();
}

// ═══════════════════════════════════════════════════════════════════════
// Custom Pin Layout
// ═══════════════════════════════════════════════════════════════════════

void PolymetricSlicerModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // --- Audio I/O (parallel layout for compactness) ---
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        const juce::String tlStr = "TL" + juce::String(t + 1);

        // Audio stereo pair
        helpers.drawParallelPins(
            (tlStr + " In L").toRawUTF8(), t * 2, (tlStr + " Out L").toRawUTF8(), t * 2);
        helpers.drawParallelPins(
            (tlStr + " In R").toRawUTF8(), t * 2 + 1, (tlStr + " Out R").toRawUTF8(), t * 2 + 1);
    }

    ImGui::Spacing();

    // --- CV Outputs ---
    for (int t = 0; t < NUM_TIMELINES; ++t)
    {
        const juce::String tlStr = "TL" + juce::String(t + 1);
        const int          cvBase = AUDIO_OUT_CHANNELS + t * CV_OUT_PER_TIMELINE;

        helpers.drawParallelPins(
            (tlStr + " Mod").toRawUTF8(),
            AUDIO_IN_CHANNELS + t,
            (tlStr + " Pitch").toRawUTF8(),
            cvBase);
        helpers.drawParallelPins(nullptr, -1, (tlStr + " Gate").toRawUTF8(), cvBase + 1);
        helpers.drawParallelPins(nullptr, -1, (tlStr + " Trigger").toRawUTF8(), cvBase + 2);
    }
}

#endif // PRESET_CREATOR_UI
