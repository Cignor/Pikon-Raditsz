#include "QuantizerModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

QuantizerModuleProcessor::QuantizerModuleProcessor()
    : ModuleProcessor(BusesProperties()
                        .withInput("Inputs", juce::AudioChannelSet::discreteChannels(3), true) // 0: Audio In, 1: Scale Mod, 2: Root Mod
                        .withOutput("Out", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "QuantizerParams", createParameterLayout())
{
    scaleParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("scale"));
    rootNoteParam = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("rootNote"));
    scaleModParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("scale_mod"));
    rootModParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("root_mod"));

    // Define scales as semitone offsets from the root
    scales.push_back({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }); // Chromatic
    scales.push_back({ 0, 2, 4, 5, 7, 9, 11 }); // Major
    scales.push_back({ 0, 2, 3, 5, 7, 8, 10 }); // Natural Minor
    scales.push_back({ 0, 2, 4, 7, 9 }); // Major Pentatonic
    scales.push_back({ 0, 3, 5, 7, 10 }); // Minor Pentatonic
    
    // ADD THIS:
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout QuantizerModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterChoice>("scale", "Scale",
        juce::StringArray{ "Chromatic", "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic" }, 0));
    p.push_back(std::make_unique<juce::AudioParameterInt>("rootNote", "Root Note", 0, 11, 0)); // 0=C, 1=C#, etc.
    p.push_back(std::make_unique<juce::AudioParameterFloat>("scale_mod", "Scale Mod", 0.0f, 1.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("root_mod", "Root Mod", 0.0f, 1.0f, 0.0f));
    return { p.begin(), p.end() };
}

void QuantizerModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate);
#if defined(PRESET_CREATOR_UI)
    vizInputBuffer.setSize(1, samplesPerBlock);
    vizOutputBuffer.setSize(1, samplesPerBlock);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
#endif
}

void QuantizerModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    auto in = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);

    // Read CV from unified input bus (if connected)
    float scaleModCV = 0.0f;
    float rootModCV = 0.0f;
    
    // Check if scale mod is connected and read CV from channel 1
    if (isParamInputConnected("scale_mod") && in.getNumChannels() > 1)
    {
        scaleModCV = in.getReadPointer(1)[0];
    }
    
    // Check if root mod is connected and read CV from channel 2
    if (isParamInputConnected("root_mod") && in.getNumChannels() > 2)
    {
        rootModCV = in.getReadPointer(2)[0];
    }

    // Apply modulation or use parameter values
    float scaleModValue = 0.0f;
    if (isParamInputConnected("scale_mod")) // Scale Mod bus connected
    {
        scaleModValue = scaleModCV;
    }
    else
    {
        scaleModValue = scaleModParam != nullptr ? scaleModParam->get() : 0.0f;
    }
    
    float rootModValue = 0.0f;
    if (isParamInputConnected("root_mod")) // Root Mod bus connected
    {
        rootModValue = rootModCV;
    }
    else
    {
        rootModValue = rootModParam != nullptr ? rootModParam->get() : 0.0f;
    }

    // Calculate final scale index, wrapping around if necessary
    int finalScaleIdx = (scaleParam != nullptr ? scaleParam->getIndex() : 0) + static_cast<int>(scaleModValue * (float)scales.size());
    finalScaleIdx = finalScaleIdx % (int)scales.size();

    // Calculate final root note, wrapping around the 12-semitone octave
    int finalRootNote = (rootNoteParam != nullptr ? rootNoteParam->get() : 0) + static_cast<int>(rootModValue * 12.0f);
    finalRootNote = finalRootNote % 12;

    const auto& currentScale = scales[finalScaleIdx];
    const float rootNote = (float)finalRootNote;

    const float* src = in.getReadPointer(0);
    float* dst = out.getWritePointer(0);

#if defined(PRESET_CREATOR_UI)
    // Capture input for visualization
    if (in.getNumChannels() > 0)
    {
        vizInputBuffer.copyFrom(0, 0, in, 0, 0, buffer.getNumSamples());
    }
#endif
    
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float inputCV = juce::jlimit(0.0f, 1.0f, src[i]);
        
        // Map 0..1 CV to a 5-octave range (60 semitones)
        const float totalSemitones = inputCV * 60.0f;
        const int octave = static_cast<int>(totalSemitones / 12.0f);
        const float noteInOctave = totalSemitones - (octave * 12.0f);
        
        // Find the closest note in the scale
        float closestNote = currentScale[0];
        float minDistance = 12.0f;
        for (float scaleNote : currentScale)
        {
            float distance = std::abs(noteInOctave - scaleNote);
            if (distance < minDistance)
            {
                minDistance = distance;
                closestNote = scaleNote;
            }
        }
        
        // Combine octave, root, and quantized note, then map back to 0..1 CV
        float finalSemitones = (octave * 12.0f) + closestNote + rootNote;
        dst[i] = juce::jlimit(0.0f, 1.0f, finalSemitones / 60.0f);
    }
    
    // Store live modulated values for UI display
    setLiveParamValue("scale_live", static_cast<float>(finalScaleIdx));
    setLiveParamValue("root_live", static_cast<float>(finalRootNote));

#if defined(PRESET_CREATOR_UI)
    // Capture output for visualization
    if (out.getNumChannels() > 0)
    {
        vizOutputBuffer.copyFrom(0, 0, out, 0, 0, buffer.getNumSamples());
    }

    // Down-sample and store waveforms
    auto captureWaveform = [&](const juce::AudioBuffer<float>& source, int channel, std::array<std::atomic<float>, VizData::waveformPoints>& dest)
    {
        const int samples = juce::jmin(source.getNumSamples(), buffer.getNumSamples());
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

    // Calculate quantization amount (difference between input and output)
    float quantDiff = 0.0f;
    const int visualSamples = juce::jmin(buffer.getNumSamples(), vizInputBuffer.getNumSamples());
    if (visualSamples > 0)
    {
        for (int i = 0; i < visualSamples; ++i)
        {
            const float inVal = vizInputBuffer.getSample(0, i);
            const float outVal = vizOutputBuffer.getSample(0, i);
            quantDiff += std::abs(outVal - inVal);
        }
        quantDiff /= static_cast<float>(visualSamples);
    }
    vizData.quantizationAmount.store(quantDiff);

    // Store current scale and root note
    vizData.currentScaleIdx.store(finalScaleIdx);
    vizData.currentRootNote.store(finalRootNote);
#endif

    // ADD THIS BLOCK:
    if (!lastOutputValues.empty() && lastOutputValues[0])
    {
        lastOutputValues[0]->store(out.getSample(0, buffer.getNumSamples() - 1));
    }
}

#if defined(PRESET_CREATOR_UI)
void QuantizerModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    const char* scaleNames[] = { "Chromatic", "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic" };
    const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // Visualization section
    ImGui::Spacing();
    ImGui::Text("Quantization Visualizer");
    ImGui::Spacing();

    // Load waveform data from atomics - BEFORE BeginChild
    float inputWave[VizData::waveformPoints];
    float outputWave[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWave[i] = vizData.inputWaveform[i].load();
        outputWave[i] = vizData.outputWaveform[i].load();
    }
    const int currentScaleIdx = vizData.currentScaleIdx.load();
    const int currentRootNote = vizData.currentRootNote.load();
    const float quantAmount = vizData.quantizationAmount.load();

    // Waveform visualization in child window
    const float waveHeight = 140.0f;
    const ImVec2 graphSize(itemWidth, waveHeight);
    const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("QuantizerViz", graphSize, false, childFlags))
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

        // Draw output waveforms
        const float midY = p0.y + graphSize.y * 0.5f;
        const float scaleY = graphSize.y * 0.4f;
        const float stepX = graphSize.x / (float)(VizData::waveformPoints - 1);

        // Draw center line
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 0.3f)), 1.0f);

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

        drawList->PopClipRect();

        // Current quantization info overlay
        ImGui::SetCursorPos(ImVec2(4, waveHeight + 4));
        if (currentScaleIdx >= 0 && currentScaleIdx < 5)
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "Scale: %s | Root: %s | Quantization: %.1f%%", 
                              scaleNames[currentScaleIdx], noteNames[currentRootNote % 12], quantAmount * 100.0f);
        else
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), "Quantization: %.1f%%", quantAmount * 100.0f);
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##quantizerVizDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ThemeText("Quantizer Parameters", theme.text.section_header);
    ImGui::Spacing();

    int scale = 0; if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("scale"))) scale = p->getIndex();
    int root = 0; if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter("rootNote"))) root = *p;

    const char* scales = "Chromatic\0Major\0Natural Minor\0Major Pentatonic\0Minor Pentatonic\0\0";
    const char* notes = "C\0C#\0D\0D#\0E\0F\0F#\0G\0G#\0A\0A#\0B\0\0";

    // Scale Combo Box
    bool isScaleModulated = isParamModulated("scale_mod");
    if (isScaleModulated) {
        scale = static_cast<int>(getLiveParamValueFor("scale_mod", "scale_live", static_cast<float>(scale)));
    }
    
    // Inline pin for Scale Mod (Channel 1)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(1))
            ImGui::SameLine();
    }
    
    if (isScaleModulated) ImGui::BeginDisabled();
    if (ImGui::Combo("Scale", &scale, scales)) if (!isScaleModulated) if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("scale"))) *p = scale;
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    // Scroll wheel editing for Scale combo
    if (!isScaleModulated && ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const int maxIndex = 4; // 5 scales: 0-4
            const int newIndex = juce::jlimit(0, maxIndex, scale + (wheel > 0.0f ? -1 : 1));
            if (newIndex != scale) {
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("scale"))) {
                    *p = newIndex;
                    onModificationEnded();
                }
            }
        }
    }
    if (isScaleModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }

    // Root Note Combo Box
    bool isRootModulated = isParamModulated("root_mod");
    if (isRootModulated) {
        root = static_cast<int>(getLiveParamValueFor("root_mod", "root_live", static_cast<float>(root)));
    }
    
    // Inline pin for Root Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }
    
    if (isRootModulated) ImGui::BeginDisabled();
    if (ImGui::Combo("Root", &root, notes)) if (!isRootModulated) if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter("rootNote"))) *p = root;
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    // Scroll wheel editing for Root combo
    if (!isRootModulated && ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const int maxIndex = 11; // 12 notes: 0-11
            const int newIndex = juce::jlimit(0, maxIndex, root + (wheel > 0.0f ? -1 : 1));
            if (newIndex != root) {
                if (auto* p = dynamic_cast<juce::AudioParameterInt*>(ap.getParameter("rootNote"))) {
                    *p = newIndex;
                    onModificationEnded();
                }
            }
        }
    }
    if (isRootModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    
    ImGui::PopItemWidth();
    ImGui::PopID();
}
#endif

void QuantizerModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Channel 0 stays parallel (CV In)
    helpers.drawParallelPins("In", 0, "Out", 0);
    // Channels 1-2 are now inline (Scale Mod, Root Mod)
}

bool QuantizerModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outBusIndex = 0; // All modulation is on the single input bus
    
    if (paramId == "scale_mod") { outChannelIndexInBus = 1; return true; }
    if (paramId == "root_mod")  { outChannelIndexInBus = 2; return true; }
    return false;
}
