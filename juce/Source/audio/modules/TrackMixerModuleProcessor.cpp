#include "TrackMixerModuleProcessor.h"
#include <cmath>

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

TrackMixerModuleProcessor::TrackMixerModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput(
                  "Inputs",
                  juce::AudioChannelSet::discreteChannels(MAX_TRACKS + 1 + (MAX_TRACKS * 2)),
                  true) // 0-63: Audio, 64: NumTracks Mod, 65+: Gain/Pan Mods
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "TrackMixerParams", createParameterLayout())
{
    numTracksParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("numTracks"));
    numTracksMaxParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("numTracks_max"));
    globalVolumeParam = apvts.getRawParameterValue("globalVolume");

    trackGainParams.resize(MAX_TRACKS);
    trackPanParams.resize(MAX_TRACKS);
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        trackGainParams[i] = apvts.getRawParameterValue("track_gain_" + juce::String(i + 1));
        trackPanParams[i] = apvts.getRawParameterValue("track_pan_" + juce::String(i + 1));
    }

    // Initialize lastOutputValues for cable inspector
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out L
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f)); // Out R

    // Initialize effective track count for UI
    if (numTracksParam != nullptr)
        lastActiveTracks.store(numTracksParam->get());
}

juce::AudioProcessorValueTreeState::ParameterLayout TrackMixerModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(
        std::make_unique<juce::AudioParameterInt>("numTracks", "Num Tracks", 2, MAX_TRACKS, 8));
    p.push_back(
        std::make_unique<juce::AudioParameterInt>(
            "numTracks_max", "Num Tracks Max", 2, MAX_TRACKS, MAX_TRACKS));
    p.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "globalVolume",
            "Global Volume",
            juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f),
            0.0f));

    for (int i = 1; i <= MAX_TRACKS; ++i)
    {
        p.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "track_gain_" + juce::String(i),
                "Track " + juce::String(i) + " Gain",
                juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f),
                0.0f));
        p.push_back(
            std::make_unique<juce::AudioParameterFloat>(
                "track_pan_" + juce::String(i),
                "Track " + juce::String(i) + " Pan",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f),
                0.0f));
    }
    return {p.begin(), p.end()};
}

void TrackMixerModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);

#if defined(PRESET_CREATOR_UI)
    // Initialize visualization buffer (stereo)
    vizOutputBuffer.setSize(2, vizBufferSize);
    vizOutputBuffer.clear();
    vizWritePos = 0;
#endif
}

void TrackMixerModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer&         midi)
{
    juce::ignoreUnused(midi);
    auto inBus = getBusBuffer(buffer, true, 0);
    auto outBus = getBusBuffer(buffer, false, 0);

    const int numSamples = buffer.getNumSamples();

    // Determine the number of active tracks from the parameter or its modulation input
    int numTracks = numTracksParam->get();

    if (isParamInputConnected(paramIdNumTracksMod) && inBus.getNumChannels() > MAX_TRACKS)
    {
        float modValue = inBus.getReadPointer(MAX_TRACKS)[0]; // Channel 64 for numTracks mod

        // Interpret modValue as a raw track count (not normalized CV)
        int maxTracks = numTracksMaxParam->get();
        numTracks = juce::roundToInt(modValue); // Round the raw value to the nearest integer
        numTracks = juce::jlimit(2, maxTracks, numTracks); // Clamp it to the valid range
    }

    // Publish live value for UI and cache for drawing pins/controls
    lastActiveTracks.store(juce::jlimit(2, MAX_TRACKS, numTracks));
    setLiveParamValue("numTracks_live", (float)numTracks);

    juce::AudioBuffer<float> mixBus(2, numSamples);
    mixBus.clear();

    // Loop through every active track and add its sound to the mix
    for (int t = 0; t < numTracks; ++t)
    {
        const float* src = (t < inBus.getNumChannels()) ? inBus.getReadPointer(t) : nullptr;
        if (src == nullptr)
            continue;

        float* mixL = mixBus.getWritePointer(0);
        float* mixR = mixBus.getWritePointer(1);

        const juce::String trackNumStr = juce::String(t + 1);

        const bool isGainModulated = isParamInputConnected(paramIdGainModPrefix + trackNumStr);
        const bool isPanModulated = isParamInputConnected(paramIdPanModPrefix + trackNumStr);

        if (!isGainModulated && !isPanModulated)
        {
            // Optimized path for non-modulated tracks
            const float gainDb = trackGainParams[t]->load();
            const float panVal = trackPanParams[t]->load();
            const float gainLin = juce::Decibels::decibelsToGain(gainDb);
            const float angle = (panVal * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
            const float lMul = gainLin * std::cos(angle);
            const float rMul = gainLin * std::sin(angle);
            juce::FloatVectorOperations::addWithMultiply(mixL, src, lMul, numSamples);
            juce::FloatVectorOperations::addWithMultiply(mixR, src, rMul, numSamples);
        }
        else // Per-sample processing is needed if either gain or pan is modulated
        {
            const float baseGainDb = trackGainParams[t]->load();
            const float basePan = trackPanParams[t]->load();

            // Get modulation signals from unified input bus
            const int    gainModChannel = MAX_TRACKS + 1 + (t * 2);
            const int    panModChannel = MAX_TRACKS + 1 + (t * 2) + 1;
            const float* gainModSignal = isGainModulated && inBus.getNumChannels() > gainModChannel
                                             ? inBus.getReadPointer(gainModChannel)
                                             : nullptr;
            const float* panModSignal = isPanModulated && inBus.getNumChannels() > panModChannel
                                            ? inBus.getReadPointer(panModChannel)
                                            : nullptr;

            for (int i = 0; i < numSamples; ++i)
            {
                float currentGainDb = isGainModulated && gainModSignal
                                          ? juce::jmap(gainModSignal[i], 0.0f, 1.0f, -60.0f, 6.0f)
                                          : baseGainDb;
                float currentPan = isPanModulated && panModSignal
                                       ? juce::jmap(panModSignal[i], 0.0f, 1.0f, -1.0f, 1.0f)
                                       : basePan;

                const float gainLin = juce::Decibels::decibelsToGain(currentGainDb);
                const float angle = (currentPan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
                const float lMul = gainLin * std::cos(angle);
                const float rMul = gainLin * std::sin(angle);

                mixL[i] += src[i] * lMul;
                mixR[i] += src[i] * rMul;

                // Store live values for UI telemetry (update every 64 samples to avoid overhead)
                if ((i & 0x3F) == 0)
                {
                    if (isGainModulated)
                        setLiveParamValue("track_gain_" + trackNumStr + "_live", currentGainDb);
                    if (isPanModulated)
                        setLiveParamValue("track_pan_" + trackNumStr + "_live", currentPan);
                }
            }
        }
    }

    // Apply global volume to the mix
    const float globalVolumeDb = globalVolumeParam->load();
    const float globalVolumeLin = juce::Decibels::decibelsToGain(globalVolumeDb);
    mixBus.applyGain(globalVolumeLin);

    // Copy the final mixed signal to the output
    outBus.copyFrom(0, 0, mixBus, 0, 0, numSamples);
    if (outBus.getNumChannels() > 1)
        outBus.copyFrom(1, 0, mixBus, 1, 0, numSamples);

#if defined(PRESET_CREATOR_UI)
    // Capture output audio for visualization
    if (vizOutputBuffer.getNumSamples() > 0 && outBus.getNumChannels() >= 2)
    {
        const int samplesToCopy = juce::jmin(numSamples, vizBufferSize);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (outBus.getNumChannels() > ch)
            {
                const float* outputData = outBus.getReadPointer(ch);
                for (int i = 0; i < samplesToCopy; ++i)
                {
                    const int writeIdx = (vizWritePos + i) % vizBufferSize;
                    vizOutputBuffer.setSample(ch, writeIdx, outputData[i]);
                }
            }
        }
        vizWritePos = (vizWritePos + samplesToCopy) % vizBufferSize;
    }

    // Update visualization data (thread-safe)
    // Downsample waveforms from circular buffer
    const int stride = vizBufferSize / VizData::waveformPoints;
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx =
            (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) %
            vizBufferSize;
        if (vizOutputBuffer.getNumSamples() > 0)
        {
            vizData.outputWaveformL[i].store(vizOutputBuffer.getSample(0, readIdx));
            vizData.outputWaveformR[i].store(
                vizOutputBuffer.getNumChannels() > 1 ? vizOutputBuffer.getSample(1, readIdx)
                                                     : 0.0f);
        }
    }

    // Calculate output levels (RMS)
    float outputRmsL = 0.0f;
    float outputRmsR = 0.0f;
    if (numSamples > 0)
    {
        if (outBus.getNumChannels() > 0)
            outputRmsL = outBus.getRMSLevel(0, 0, numSamples);
        if (outBus.getNumChannels() > 1)
            outputRmsR = outBus.getRMSLevel(1, 0, numSamples);
    }
    vizData.outputLevelDbL.store(juce::Decibels::gainToDecibels(outputRmsL, -60.0f));
    vizData.outputLevelDbR.store(juce::Decibels::gainToDecibels(outputRmsR, -60.0f));
    vizData.activeTracks.store(numTracks);
#endif
}

int TrackMixerModuleProcessor::getEffectiveNumTracks() const
{
    return numTracksParam ? numTracksParam->get() : 8;
}

void TrackMixerModuleProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto                              state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void TrackMixerModuleProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

#if defined(PRESET_CREATOR_UI)
void TrackMixerModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded,
    const NodePinHelpers*                                   pinHelpers)
{
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    const int activeTracks = getEffectiveNumTracks();

    // --- Master "Tracks" Slider with correct modulation detection ---
    const bool isCountModulated = isParamModulated(paramIdNumTracksMod);
    int        displayedTracks = numTracksParam->get();
    // If modulated, show the live computed value in the disabled slider
    if (isCountModulated)
        displayedTracks = juce::roundToInt(
            getLiveParamValueFor(paramIdNumTracksMod, "numTracks_live", (float)displayedTracks));
    const int maxTracksBound = numTracksMaxParam->get();

    // Inline pin for Num Tracks Mod
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting(paramIdNumTracksMod, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }

    if (isCountModulated)
        ImGui::BeginDisabled();

    ImGui::PushItemWidth(itemWidth);
    if (ImGui::SliderInt("Tracks", &displayedTracks, 2, maxTracksBound))
    {
        if (!isCountModulated)
        {
            *numTracksParam = displayedTracks;
            onModificationEnded();
        }
    }
    if (!isCountModulated)
    {
        adjustParamOnWheel(ap.getParameter("numTracks"), "numTracks", (float)displayedTracks);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isCountModulated)
    {
        onModificationEnded();
    }
    ImGui::PopItemWidth();

    if (isCountModulated)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("(mod)");
    }

    ImGui::Spacing();

    // --- Global Volume Slider ---
    auto* globalVolumeParamPtr =
        dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("globalVolume"));
    if (globalVolumeParamPtr)
    {
        float volumeValue = globalVolumeParamPtr->get();
        ImGui::PushItemWidth(itemWidth);
        if (ImGui::SliderFloat("Volume", &volumeValue, -60.0f, 6.0f, "%.1f dB"))
        {
            *globalVolumeParamPtr = volumeValue;
        }
        adjustParamOnWheel(ap.getParameter("globalVolume"), "globalVolume", volumeValue);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            onModificationEnded();
        }
        ImGui::PopItemWidth();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // === STEREO WAVEFORM VISUALIZATION ===
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    // Helper for tooltips
    auto HelpMarkerTrackMixer = [](const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    // Read visualization data (thread-safe)
    float outputWaveformL[VizData::waveformPoints];
    float outputWaveformR[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        outputWaveformL[i] = vizData.outputWaveformL[i].load();
        outputWaveformR[i] = vizData.outputWaveformR[i].load();
    }
    const int   activeTracksCount = vizData.activeTracks.load();
    const float outputLevelDbL = vizData.outputLevelDbL.load();
    const float outputLevelDbR = vizData.outputLevelDbR.load();

    // Waveform visualization in child window
    const auto& freqColors = theme.modules.frequency_graph;
    const auto  resolveColor = [](ImU32 value, ImU32 fallback) {
        return value != 0 ? value : fallback;
    };
    const float  waveHeight = 140.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);

    if (ImGui::BeginChild(
            "TrackMixerOscilloscope",
            graphSize,
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImDrawList*  drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);

        // Background
        const ImU32 bgColor = resolveColor(freqColors.background, IM_COL32(18, 20, 24, 255));
        drawList->AddRectFilled(p0, p1, bgColor);

        // Grid lines
        const ImU32 gridColor = resolveColor(freqColors.grid, IM_COL32(50, 55, 65, 255));
        const float midY = p0.y + graphSize.y * 0.5f;
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), gridColor, 1.0f);

        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);

        // Draw stereo waveforms (L and R overlaid with different colors)
        const float scaleY = graphSize.y * 0.45f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        // Left channel - use theme accent color with slight blue tint
        ImVec4 accentL = theme.accent;
        accentL.x *= 0.7f; // Reduce red, keep blue/green
        accentL.y *= 1.0f;
        accentL.z = juce::jmin(1.0f, accentL.z * 1.2f); // Boost blue
        const ImU32 waveformColorL = ImGui::ColorConvertFloat4ToU32(accentL);
        float       prevXL = p0.x;
        float       prevYL = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveformL[i]);
            const float x = p0.x + i * stepX;
            const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
            if (i > 0)
                drawList->AddLine(ImVec2(prevXL, prevYL), ImVec2(x, y), waveformColorL, 2.0f);
            prevXL = x;
            prevYL = y;
        }

        // Right channel - use theme accent color with slight orange tint
        ImVec4 accentR = theme.accent;
        accentR.x = juce::jmin(1.0f, accentR.x * 1.3f); // Boost red
        accentR.y *= 0.8f;                              // Reduce green
        accentR.z *= 0.5f;                              // Reduce blue
        const ImU32 waveformColorR = ImGui::ColorConvertFloat4ToU32(accentR);
        float       prevXR = p0.x;
        float       prevYR = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            const float sample = juce::jlimit(-1.0f, 1.0f, outputWaveformR[i]);
            const float x = p0.x + i * stepX;
            const float y = juce::jlimit(p0.y, p1.y, midY - sample * scaleY);
            if (i > 0)
                drawList->AddLine(ImVec2(prevXR, prevYR), ImVec2(x, y), waveformColorR, 2.0f);
            prevXR = x;
            prevYR = y;
        }

        drawList->PopClipRect();

        // Info overlay
        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "%d tracks", activeTracksCount);

        // Level meters (bottom left) - match waveform colors
        ImGui::SetCursorPos(ImVec2(4, graphSize.y - 40));
        ImGui::TextColored(accentL, "L: %.1f dB", outputLevelDbL);
        ImGui::SetCursorPos(ImVec2(4, graphSize.y - 20));
        ImGui::TextColored(accentR, "R: %.1f dB", outputLevelDbR);

        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##trackMixerOscilloscopeDrag", graphSize);
    }
    ImGui::EndChild(); // CRITICAL: Must be OUTSIDE the if block!

    ImGui::Spacing();
    ImGui::Spacing();

    // --- Per-Track Sliders (Dynamically created for all active tracks) ---
    // --- FIX: Use the 'displayedTracks' variable here, which respects modulation ---
    for (int t = 0; t < displayedTracks; ++t)
    {
        ImGui::PushID(t);
        const juce::String trackNumStr = juce::String(t + 1);

        auto* gainParamPtr =
            dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("track_gain_" + trackNumStr));
        auto* panParamPtr =
            dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("track_pan_" + trackNumStr));

        if (!gainParamPtr || !panParamPtr)
        {
            ImGui::PopID();
            continue;
        }

        // --- Gain Slider for Track t+1 ---
        const bool isGainModulated = isParamModulated("track_gain_" + trackNumStr);
        float      gainVal = gainParamPtr->get(); // Get base value
        if (isGainModulated)
        {
            // If modulated, show the live computed value
            gainVal = getLiveParamValueFor(
                "track_gain_" + trackNumStr, "track_gain_" + trackNumStr + "_live", gainVal);
        }

        // Inline pin for Gain Mod
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            int busIdx, chanInBus;
            const juce::String gainModId = paramIdGainModPrefix + trackNumStr;
            if (getParamRouting(gainModId, busIdx, chanInBus))
            {
                int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
                if (pinHelpers->drawInlineInputPin(channel))
                    ImGui::SameLine();
            }
        }

        if (isGainModulated)
            ImGui::BeginDisabled();

        ImGui::PushItemWidth(itemWidth * 0.5f - 20); // Adjust width for mod indicator
        if (ImGui::SliderFloat(("G" + trackNumStr).toRawUTF8(), &gainVal, -60.0f, 6.0f, "%.1f dB"))
        {
            if (!isGainModulated)
                *gainParamPtr = gainVal;
        }
        if (!isGainModulated)
            adjustParamOnWheel(gainParamPtr, "gain", gainVal);
        if (ImGui::IsItemDeactivatedAfterEdit() && !isGainModulated)
        {
            onModificationEnded();
        }
        ImGui::PopItemWidth();

        if (isGainModulated)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted("(m)");
        }

        // --- Pan Slider for Track t+1 (on next line) ---
        const bool isPanModulated = isParamModulated("track_pan_" + trackNumStr);
        float      panVal = panParamPtr->get(); // Get base value
        if (isPanModulated)
        {
            // If modulated, show the live computed value
            panVal = getLiveParamValueFor(
                "track_pan_" + trackNumStr, "track_pan_" + trackNumStr + "_live", panVal);
        }

        // Inline pin for Pan Mod
        if (pinHelpers && pinHelpers->drawInlineInputPin)
        {
            int busIdx, chanInBus;
            const juce::String panModId = paramIdPanModPrefix + trackNumStr;
            if (getParamRouting(panModId, busIdx, chanInBus))
            {
                int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
                if (pinHelpers->drawInlineInputPin(channel))
                    ImGui::SameLine();
            }
        }

        if (isPanModulated)
            ImGui::BeginDisabled();

        ImGui::PushItemWidth(itemWidth); // Full width for Pan slider
        if (ImGui::SliderFloat(("P" + trackNumStr).toRawUTF8(), &panVal, -1.0f, 1.0f, "%.2f"))
        {
            if (!isPanModulated)
                *panParamPtr = panVal;
        }
        if (!isPanModulated)
            adjustParamOnWheel(panParamPtr, "pan", panVal);
        if (ImGui::IsItemDeactivatedAfterEdit() && !isPanModulated)
        {
            onModificationEnded();
        }
        ImGui::PopItemWidth();

        if (isPanModulated)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted("(m)");
        }

        // Audio input pin for this track (inline, below Pan)
        if (pinHelpers && pinHelpers->drawAudioInputPin)
        {
            pinHelpers->drawAudioInputPin(("Audio " + trackNumStr).toRawUTF8(), t);
        }

        ImGui::PopID();
    }
    ImGui::PopID();
}
#endif

// Human-legible per-channel labels for the single multichannel input bus
juce::String TrackMixerModuleProcessor::getAudioInputLabel(int channel) const
{
    // Channel names mirror visual controls: Audio N, Num Tracks Mod, Gain N Mod, Pan N Mod
    // Audio inputs occupy channels [0 .. MAX_TRACKS-1]
    if (channel >= 0 && channel < MAX_TRACKS)
        return "Audio " + juce::String(channel + 1);
    // Mod lanes begin after audio inputs
    const int modBase = MAX_TRACKS; // conceptual; we expose labels by absolute channel
    // For labeling, we follow the param routing: 0: numTracks, then pairs for each track
    const int idx = channel - MAX_TRACKS;
    if (idx == 0)
        return "Num Tracks Mod";
    if (idx > 0)
    {
        const int  pair = (idx - 1) / 2; // 0-based track
        const bool isGain = ((idx - 1) % 2) == 0;
        if (pair >= 0 && pair < MAX_TRACKS)
            return juce::String(isGain ? "Gain " : "Pan ") + juce::String(pair + 1) + " Mod";
    }
    return {};
}

void TrackMixerModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Use the last value computed on audio thread if available
    const int activeTracks = juce::jlimit(2, MAX_TRACKS, lastActiveTracks.load());

    // --- Draw Output Pins Only ---
    helpers.drawParallelPins(nullptr, -1, "Out L", 0);
    helpers.drawParallelPins(nullptr, -1, "Out R", 1);

    // --- Audio Input Pins and Modulation Pins are now inline, not drawn here ---
}

std::vector<DynamicPinInfo> TrackMixerModuleProcessor::getDynamicInputPins() const
{
    std::vector<DynamicPinInfo> pins;
    const int                   activeTracks = juce::jlimit(2, MAX_TRACKS, lastActiveTracks.load());

    // Audio input pins (channels 0 through activeTracks-1)
    for (int t = 0; t < activeTracks; ++t)
    {
        pins.push_back({"Audio " + juce::String(t + 1), t, PinDataType::Audio});
    }

    // NumTracks modulation pin
    int busIdx, chanInBus;
    if (getParamRouting(paramIdNumTracksMod, busIdx, chanInBus))
    {
        int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
        pins.push_back({"Num Tracks Mod", channel, PinDataType::Raw});
    }

    // Per-track modulation pins (Gain and Pan for each track)
    for (int t = 1; t <= activeTracks; ++t)
    {
        const juce::String trackNumStr = juce::String(t);
        const juce::String gainModId = paramIdGainModPrefix + trackNumStr;
        const juce::String panModId = paramIdPanModPrefix + trackNumStr;

        if (getParamRouting(gainModId, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            pins.push_back({"Gain " + trackNumStr + " Mod", channel, PinDataType::CV});
        }

        if (getParamRouting(panModId, busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            pins.push_back({"Pan " + trackNumStr + " Mod", channel, PinDataType::CV});
        }
    }

    return pins;
}

std::vector<DynamicPinInfo> TrackMixerModuleProcessor::getDynamicOutputPins() const
{
    std::vector<DynamicPinInfo> pins;

    // Stereo output
    pins.push_back({"Out L", 0, PinDataType::Audio});
    pins.push_back({"Out R", 1, PinDataType::Audio});

    return pins;
}

bool TrackMixerModuleProcessor::getParamRouting(
    const juce::String& paramId,
    int&                outBusIndex,
    int&                outChannelIndexInBus) const
{
    // All modulation is on the single input bus
    outBusIndex = 0;

    if (paramId == paramIdNumTracksMod)
    {
        outChannelIndexInBus = MAX_TRACKS; // Channel 64
        return true;
    }

    if (paramId.startsWith(paramIdGainModPrefix))
    {
        const juce::String trackNumStr =
            paramId.substring(juce::String(paramIdGainModPrefix).length());
        const int trackNum = trackNumStr.getIntValue();
        if (trackNum > 0 && trackNum <= MAX_TRACKS)
        {
            outChannelIndexInBus = MAX_TRACKS + 1 + (trackNum - 1) * 2; // Gain channels start at 65
            return true;
        }
    }
    else if (paramId.startsWith(paramIdPanModPrefix))
    {
        const juce::String trackNumStr =
            paramId.substring(juce::String(paramIdPanModPrefix).length());
        const int trackNum = trackNumStr.getIntValue();
        if (trackNum > 0 && trackNum <= MAX_TRACKS)
        {
            outChannelIndexInBus =
                MAX_TRACKS + 1 + (trackNum - 1) * 2 + 1; // Pan channels start at 66
            return true;
        }
    }
    return false;
}