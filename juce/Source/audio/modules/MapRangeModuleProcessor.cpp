#include "MapRangeModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

MapRangeModuleProcessor::MapRangeModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("In", juce::AudioChannelSet::mono(), true)
              .withOutput("Out", juce::AudioChannelSet::discreteChannels(3), true)),
      apvts(*this, nullptr, "MapRangeParams", createParameterLayout())
{
    inMinParam = apvts.getRawParameterValue("inMin");
    inMaxParam = apvts.getRawParameterValue("inMax");
    outMinParam = apvts.getRawParameterValue("outMin");
    outMaxParam = apvts.getRawParameterValue("outMax");
    normMinParam = apvts.getRawParameterValue("normMin");
    normMaxParam = apvts.getRawParameterValue("normMax");

    // Cache new CV parameters
    cvMinParam = apvts.getRawParameterValue("cvMin");
    cvMaxParam = apvts.getRawParameterValue("cvMax");

    // Initialize storage for the three output pins (Norm Out, Raw Out, CV Out)
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout MapRangeModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "inMin", "Input Min", juce::NormalisableRange<float>(-100.0f, 100.0f), 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "inMax", "Input Max", juce::NormalisableRange<float>(-100.0f, 100.0f), 1.0f));
    // Bipolar Norm Out range [-1, 1]
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "normMin", "Norm Min", juce::NormalisableRange<float>(-1.0f, 1.0f, 0.0001f), -1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "normMax", "Norm Max", juce::NormalisableRange<float>(-1.0f, 1.0f, 0.0001f), 1.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "outMin", "Output Min", juce::NormalisableRange<float>(-10000.0f, 10000.0f), 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "outMax", "Output Max", juce::NormalisableRange<float>(-10000.0f, 10000.0f), 1.0f));

    // Add new CV parameters
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "cvMin", "CV Min", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "cvMax", "CV Max", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    return {params.begin(), params.end()};
}

void MapRangeModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate);
#if defined(PRESET_CREATOR_UI)
    vizInputBuffer.setSize(1, samplesPerBlock);
    vizOutputBuffer.setSize(3, samplesPerBlock);
    vizInputBuffer.clear();
    vizOutputBuffer.clear();
#endif
}

void MapRangeModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);

    auto in = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);
    // Ensure we present audio on a conventional stereo bus for downstream audio nodes.
    // Norm Out (0) is intended for CV; Raw Out (1) is wide-range audio.
    // Duplicate Raw Out into both L/R when the main graph expects stereo.

    const float inMin = inMinParam->load();
    const float inMax = inMaxParam->load();
    const float normMin = normMinParam->load();
    const float normMax = normMaxParam->load();
    const float outMin = outMinParam->load();
    const float outMax = outMaxParam->load();
    const float cvMin = cvMinParam->load();
    const float cvMax = cvMaxParam->load();

    const float inRange = inMax - inMin;
    const float outRange = outMax - outMin;

    // Get pointers to all three output channels: 0 for Norm (CV), 1 for Raw (audio), 2 for CV
    float* normDst = out.getWritePointer(0);
    float* rawDst = out.getNumChannels() > 1 ? out.getWritePointer(1) : nullptr;
    float* cvDst = out.getNumChannels() > 2 ? out.getWritePointer(2) : nullptr;

#if defined(PRESET_CREATOR_UI)
    // Capture input for visualization (No reallocation in processBlock!)
    if (in.getNumChannels() > 0 && vizInputBuffer.getNumSamples() >= buffer.getNumSamples())
    {
        vizInputBuffer.copyFrom(0, 0, in, 0, 0, buffer.getNumSamples());
    }
    // If buffer sizes mismatch (e.g. variable block size), we skip viz update to be safe
#endif

    if (std::abs(inRange) < 0.0001f)
    {
        // Handle division by zero: output the middle of the output range.
        const float rawVal = (outMin + outMax) * 0.5f;
        const float normVal = 0.5f;
        const float cvVal = (cvMin + cvMax) * 0.5f;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            normDst[i] = normVal;
            if (rawDst)
                rawDst[i] = rawVal;
            if (cvDst)
                cvDst[i] = cvVal;
        }
        lastInputValue.store(inMin);
        lastOutputValue.store(rawVal);
        lastCvOutputValue.store(cvVal);
    }
    else
    {
        const float* src = in.getReadPointer(0);
        float        sumInput = 0.0f;
        float        sumOutput = 0.0f;
        float        sumCvOutput = 0.0f;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            // 1. Clamp and normalize the input signal (0..1 range)
            float clampedInput = juce::jlimit(inMin, inMax, src[i]);
            float normalizedInput = 0.0f;
            if (std::abs(inRange) > 1e-9f)
                normalizedInput = (clampedInput - inMin) / inRange;

            // 2. Calculate the three separate outputs from the normalized value
            float rawOutputVal = juce::jmap(normalizedInput, outMin, outMax);
            float cvOutputVal = juce::jmap(normalizedInput, cvMin, cvMax);

            // 3. Write to the respective output channels
            const float norm01 = juce::jlimit(0.0f, 1.0f, normalizedInput);
            const float normVal = juce::jmap(norm01, normMin, normMax);
            normDst[i] = normVal; // Norm Out (bipolar CV)
            if (rawDst)
                rawDst[i] = rawOutputVal; // Raw Out (audio)
            if (cvDst)
                cvDst[i] = cvOutputVal; // CV Out

            sumInput += clampedInput;
            sumOutput += rawOutputVal;
            sumCvOutput += cvOutputVal;
        }

        lastInputValue.store(sumInput / (float)buffer.getNumSamples());
        lastOutputValue.store(sumOutput / (float)buffer.getNumSamples());
        lastCvOutputValue.store(sumCvOutput / (float)buffer.getNumSamples());

        // Store live parameter values for UI display (currently no modulation, so store parameter
        // values)
        setLiveParamValue("inMin_live", inMin);
        setLiveParamValue("inMax_live", inMax);
        setLiveParamValue("normMin_live", normMin);
        setLiveParamValue("normMax_live", normMax);
        setLiveParamValue("outMin_live", outMin);
        setLiveParamValue("outMax_live", outMax);
        setLiveParamValue("cvMin_live", cvMin);
        setLiveParamValue("cvMax_live", cvMax);

#if defined(PRESET_CREATOR_UI)
        // Capture output waveforms for visualization
        // Capture output waveforms for visualization
        if (out.getNumChannels() >= 3 && vizOutputBuffer.getNumSamples() >= buffer.getNumSamples())
        {
            vizOutputBuffer.copyFrom(0, 0, out, 0, 0, buffer.getNumSamples()); // Norm Out
            vizOutputBuffer.copyFrom(1, 0, out, 1, 0, buffer.getNumSamples()); // Raw Out
            vizOutputBuffer.copyFrom(2, 0, out, 2, 0, buffer.getNumSamples()); // CV Out
        }

        // Down-sample and store waveforms
        auto captureWaveform = [&](const juce::AudioBuffer<float>&                          source,
                                   int                                                      channel,
                                   std::array<std::atomic<float>, VizData::waveformPoints>& dest,
                                   bool normalizeRaw = false) {
            const int samples = juce::jmin(source.getNumSamples(), buffer.getNumSamples());
            if (samples <= 0 || channel >= source.getNumChannels())
                return;
            const int stride = juce::jmax(1, samples / VizData::waveformPoints);
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const int idx = juce::jmin(samples - 1, i * stride);
                float     value = source.getSample(channel, idx);
                if (std::isnan(value))
                    value = 0.0f; // Guard: protect against NaNs corrupting the visualizer

                if (normalizeRaw && std::abs(outRange) > 1e-6f)
                {
                    // Normalize raw output to [-1, 1] range for visualization
                    value = (value - outMin) / outRange * 2.0f - 1.0f;
                }

                if (std::isnan(value))
                    value = 0.0f; // Extra safety if outMin/outRange were weird
                dest[i].store(juce::jlimit(-1.0f, 1.0f, value));
            }
        };

        captureWaveform(vizInputBuffer, 0, vizData.inputWaveform);
        if (out.getNumChannels() >= 3)
        {
            captureWaveform(vizOutputBuffer, 0, vizData.normWaveform);
            captureWaveform(vizOutputBuffer, 1, vizData.rawWaveform, true); // Normalize raw output
            captureWaveform(vizOutputBuffer, 2, vizData.cvWaveform);
        }

        // Store current parameter values for visualization
        vizData.currentInMin.store(inMin);
        vizData.currentInMax.store(inMax);
        vizData.currentNormMin.store(normMin);
        vizData.currentNormMax.store(normMax);
        vizData.currentOutMin.store(outMin);
        vizData.currentOutMax.store(outMax);
        vizData.currentCvMin.store(cvMin);
        vizData.currentCvMax.store(cvMax);
#endif
    }

    // Update the hover-value display for all three output pins
    if (lastOutputValues.size() >= 3)
    {
        if (lastOutputValues[0])
            lastOutputValues[0]->store(out.getSample(0, buffer.getNumSamples() - 1));
        if (out.getNumChannels() > 1 && lastOutputValues[1])
            lastOutputValues[1]->store(out.getSample(1, buffer.getNumSamples() - 1));
        if (out.getNumChannels() > 2 && lastOutputValues[2])
            lastOutputValues[2]->store(out.getSample(2, buffer.getNumSamples() - 1));
    }
}

#if defined(PRESET_CREATOR_UI)
void MapRangeModuleProcessor::drawParametersInNode(
    float                                                   itemWidth,
    const std::function<bool(const juce::String& paramId)>& isParamModulated,
    const std::function<void()>&                            onModificationEnded)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto&       ap = getAPVTS();
    // Intentionally removed PushID(this) to let ImNodes manage ID scope and avoiding stack leaks on
    // early return

    // Visualization section
    ImGui::Spacing();
    ImGui::Text("Range Mapping Visualizer");
    ImGui::Spacing();

    auto*       drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 normColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
    const ImU32 rawColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
    const ImU32 cvColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);

    // Waveform visualization area
    const float waveHeight = 140.0f;

    // Use manual layout instead of Child Window to prevent render stack corruption
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // SAFETY: Clamp itemWidth to sane values to prevent "world explosion"
    // If itemWidth is uninit or garbage, it could be huge.
    float safeWidth = juce::jlimit(10.0f, 2000.0f, itemWidth);

    // SAFETY: Check for invalid origin (NaN/Inf) from ImNodes
    if (std::isnan(origin.x) || std::isnan(origin.y) || std::isinf(origin.x) ||
        std::isinf(origin.y))
    {
        // LOGGING as requested by user
        static int logCounter = 0;
        if ((logCounter++ % 60) == 0) // Log once per second approx
        {
            juce::Logger::writeToLog(
                "MapRange GLITCH DETECTED: Invalid Origin! x=" + juce::String(origin.x) +
                " y=" + juce::String(origin.y));
        }
        ImGui::Dummy(ImVec2(safeWidth, waveHeight));
        return;
    }

    const ImVec2 rectMax = ImVec2(origin.x + safeWidth, origin.y + waveHeight);

    // Double check rectMax sanity
    if (std::abs(rectMax.x) > 1.0e8f || std::abs(rectMax.y) > 1.0e8f)
    {
        static int logCounter = 0;
        if ((logCounter++ % 60) == 0)
        {
            juce::Logger::writeToLog(
                "MapRange GLITCH DETECTED: RectMax Explosion! rectMax=(" + juce::String(rectMax.x) +
                ", " + juce::String(rectMax.y) + ") origin=(" + juce::String(origin.x) + ", " +
                juce::String(origin.y) + ") width=" + juce::String(safeWidth));
        }
        ImGui::Dummy(ImVec2(safeWidth, waveHeight));
        return;
    }

    // Reserve space for the visualization
    ImGui::Dummy(ImVec2(safeWidth, waveHeight));

    // Force clipping to prevent any drawing outside the intended area
    drawList->PushClipRect(origin, rectMax, true);

    // Background
    drawList->AddRectFilled(origin, rectMax, bgColor, 4.0f);

    // Load waveform data from atomics
    float inputWave[VizData::waveformPoints];
    float normWave[VizData::waveformPoints];
    float rawWave[VizData::waveformPoints];
    float cvWave[VizData::waveformPoints];

    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWave[i] = vizData.inputWaveform[i].load();
        normWave[i] = vizData.normWaveform[i].load();
        rawWave[i] = vizData.rawWaveform[i].load();
        cvWave[i] = vizData.cvWaveform[i].load();
    }

    const float midY = origin.y + waveHeight * 0.5f;
    const float scaleY = waveHeight * 0.35f;
    const float stepX = safeWidth / (float)(VizData::waveformPoints - 1);

    auto drawWave = [&](const float* data, ImU32 color, float thickness) {
        float px = origin.x;
        float py = midY;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            float val = data[i];
            if (std::isnan(val) || std::isinf(val))
                val = 0.0f; // Safety guard for rendering

            const float x = origin.x + i * stepX;
            const float y = midY - juce::jlimit(-1.0f, 1.0f, val) * scaleY;

            // STRICT CLAMPING to bounds to prevent "infinite" coordinates
            const float clampedX = juce::jlimit(origin.x, rectMax.x, x);
            const float clampedY = juce::jlimit(origin.y, rectMax.y, y);

            if (i > 0)
                drawList->AddLine(ImVec2(px, py), ImVec2(clampedX, clampedY), color, thickness);
            px = clampedX;
            py = clampedY;
        }
    };

    // Draw waveforms (input first, then outputs)
    drawWave(inputWave, inputColor, 1.5f);
    drawWave(normWave, normColor, 1.8f);
    drawWave(rawWave, rawColor, 1.6f);
    drawWave(cvWave, cvColor, 1.4f);

    // Draw center line (CLIPPED)
    // We must clamp X to rectMax.x to avoid infinite lines
    drawList->AddLine(
        ImVec2(origin.x, midY),
        ImVec2(juce::jlimit(origin.x, rectMax.x, rectMax.x), midY),
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 0.3f)),
        1.0f);

    drawList->PopClipRect();

    // BeginChild automatically advances the cursor, so we don't need manual positioning

    // Display current parameter values - positioned below waveform
    const float inMin = vizData.currentInMin.load();
    const float inMax = vizData.currentInMax.load();
    const float normMin = vizData.currentNormMin.load();
    const float normMax = vizData.currentNormMax.load();
    const float outMin = vizData.currentOutMin.load();
    const float outMax = vizData.currentOutMax.load();
    const float cvMin = vizData.currentCvMin.load();
    const float cvMax = vizData.currentCvMax.load();

    ImGui::Text("Input: [%.2f, %.2f] -> Norm: [%.4f, %.4f]", inMin, inMax, normMin, normMax);
    ImGui::Text("Raw: [%.2f, %.2f]  |  CV: [%.2f, %.2f]", outMin, outMax, cvMin, cvMax);

    ImGui::Spacing();
    ThemeText("Range Mapping Parameters", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushItemWidth(itemWidth);
    float inMinEdit = inMinParam->load();
    float inMaxEdit = inMaxParam->load();
    float normMinEdit = normMinParam->load();
    float normMaxEdit = normMaxParam->load();
    float outMinEdit = outMinParam->load();
    float outMaxEdit = outMaxParam->load();
    float cvMinEdit = cvMinParam->load();
    float cvMaxEdit = cvMaxParam->load();

    // Input Range Sliders
    if (ImGui::SliderFloat("Input Min", &inMinEdit, -100.0f, 100.0f))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("inMin")))
            *p = inMinEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("inMin"), "inMin", inMinEdit);

    if (ImGui::SliderFloat("Input Max", &inMaxEdit, -100.0f, 100.0f))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("inMax")))
            *p = inMaxEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("inMax"), "inMax", inMaxEdit);

    // Norm Out precise bipolar range [-1, 1]
    if (ImGui::SliderFloat("Norm Min", &normMinEdit, -1.0f, 1.0f, "%.4f"))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("normMin")))
            *p = normMinEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("normMin"), "normMin", normMinEdit);

    if (ImGui::SliderFloat("Norm Max", &normMaxEdit, -1.0f, 1.0f, "%.4f"))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("normMax")))
            *p = normMaxEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("normMax"), "normMax", normMaxEdit);

    // CV Output Range Sliders (0.0-1.0 range)
    if (ImGui::SliderFloat("CV Min", &cvMinEdit, 0.0f, 1.0f))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("cvMin")))
            *p = cvMinEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("cvMin"), "cvMin", cvMinEdit);

    if (ImGui::SliderFloat("CV Max", &cvMaxEdit, 0.0f, 1.0f))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("cvMax")))
            *p = cvMaxEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("cvMax"), "cvMax", cvMaxEdit);

    // Raw Output Range Sliders (wide range)
    if (ImGui::SliderFloat(
            "Output Min", &outMinEdit, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMin")))
            *p = outMinEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("outMin"), "outMin", outMinEdit);

    if (ImGui::SliderFloat(
            "Output Max", &outMaxEdit, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic))
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("outMax")))
            *p = outMaxEdit;
    if (ImGui::IsItemDeactivatedAfterEdit())
        onModificationEnded();
    adjustParamOnWheel(ap.getParameter("outMax"), "outMax", outMaxEdit);

    // Output value displays
    ImGui::Spacing();
    ImGui::Text("Live Values:");
    ImGui::Text("Input:     %.2f", getLastInputValue());
    ImGui::Text("Raw Out:   %.2f", getLastOutputValue());
    ImGui::Text("CV Out:    %.2f", getLastCvOutputValue());

    ImGui::PopItemWidth();
    // Removed PopID() because we removed PushID(this) at the start
}

void MapRangeModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawParallelPins("Input", 0, "Norm Out", 0);
    helpers.drawParallelPins(nullptr, -1, "Raw Out", 1);
    helpers.drawParallelPins(nullptr, -1, "CV Out", 2);
}
#endif

float MapRangeModuleProcessor::getLastInputValue() const { return lastInputValue.load(); }

float MapRangeModuleProcessor::getLastOutputValue() const { return lastOutputValue.load(); }

float MapRangeModuleProcessor::getLastCvOutputValue() const { return lastCvOutputValue.load(); }
