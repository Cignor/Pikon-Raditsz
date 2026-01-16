// Rebuilt module implementation
#include "VocalTractFilterModuleProcessor.h"
#include "../../utils/RtLogger.h"
#include <cstdio>
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/ImGuiNodeEditorComponent.h"
#include "../../preset_creator/theme/ThemeManager.h"
#include <imgui.h>
#endif

// Static formant tables
const FormantData VocalTractFilterModuleProcessor::VOWEL_A[4] = { {700.0f,1.0f,6.0f},{1220.0f,0.5f,8.0f},{2600.0f,0.2f,12.0f},{3800.0f,0.15f,15.0f} };
const FormantData VocalTractFilterModuleProcessor::VOWEL_E[4] = { {500.0f,1.0f,7.0f},{1800.0f,0.6f,9.0f},{2800.0f,0.3f,13.0f},{3900.0f,0.2f,16.0f} };
const FormantData VocalTractFilterModuleProcessor::VOWEL_I[4] = { {270.0f,1.0f,8.0f},{2300.0f,0.4f,10.0f},{3000.0f,0.2f,14.0f},{4000.0f,0.1f,18.0f} };
const FormantData VocalTractFilterModuleProcessor::VOWEL_O[4] = { {450.0f,1.0f,6.0f},{800.0f,0.7f,8.0f},{2830.0f,0.15f,12.0f},{3800.0f,0.1f,15.0f} };
const FormantData VocalTractFilterModuleProcessor::VOWEL_U[4] = { {300.0f,1.0f,7.0f},{870.0f,0.6f,9.0f},{2240.0f,0.1f,13.0f},{3500.0f,0.05f,16.0f} };

VocalTractFilterModuleProcessor::VocalTractFilterModuleProcessor()
    : ModuleProcessor(BusesProperties().withInput("Audio In", juce::AudioChannelSet::stereo(), true)
                                        .withInput("Vowel Mod", juce::AudioChannelSet::mono(), true)
                                        .withInput("Formant Mod", juce::AudioChannelSet::mono(), true)
                                        .withInput("Instability Mod", juce::AudioChannelSet::mono(), true)
                                        .withInput("Gain Mod", juce::AudioChannelSet::mono(), true)
                                        .withOutput("Audio Out", juce::AudioChannelSet::stereo(), true))
    , apvts(*this, nullptr, "VocalTractParams", createParameterLayout())
{
    vowelShapeParam   = apvts.getRawParameterValue("vowelShape");
    formantShiftParam = apvts.getRawParameterValue("formantShift");
    instabilityParam  = apvts.getRawParameterValue("instability");
    outputGainParam   = apvts.getRawParameterValue("formantGain");

    // Initialize oscillators with sine wave functions
    wowOscillator.initialise([](float x) { return std::sin(x); }, 128);
    flutterOscillator.initialise([](float x) { return std::sin(x); }, 128);
    
    // Initialize lastOutputValues for cable inspector (stereo)
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

void VocalTractFilterModuleProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (sampleRate <= 0.0 || samplesPerBlock <= 0) return;

    dspSpec.sampleRate = sampleRate;
    dspSpec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    dspSpec.numChannels = 1; // Each filter processes one channel

    // Prepare left channel filters
    bandsL.b0.prepare(dspSpec); bandsL.b1.prepare(dspSpec);
    bandsL.b2.prepare(dspSpec); bandsL.b3.prepare(dspSpec);
    bandsL.b0.reset(); bandsL.b1.reset(); bandsL.b2.reset(); bandsL.b3.reset();

    // Prepare right channel filters
    bandsR.b0.prepare(dspSpec); bandsR.b1.prepare(dspSpec);
    bandsR.b2.prepare(dspSpec); bandsR.b3.prepare(dspSpec);
    bandsR.b0.reset(); bandsR.b1.reset(); bandsR.b2.reset(); bandsR.b3.reset();

    wowOscillator.prepare(dspSpec); wowOscillator.setFrequency(0.5f); wowOscillator.reset();
    flutterOscillator.prepare(dspSpec); flutterOscillator.setFrequency(7.5f); flutterOscillator.reset();

    ensureWorkBuffers(2, samplesPerBlock); // Stereo work buffers
    updateCoefficients(0.0f, 0.0f, 0.0f); // Initialize with default values

#if defined(PRESET_CREATOR_UI)
    vizInputBufferL.setSize(1, vizBufferSize, false, true, true);
    vizInputBufferR.setSize(1, vizBufferSize, false, true, true);
    vizOutputBufferL.setSize(1, vizBufferSize, false, true, true);
    vizOutputBufferR.setSize(1, vizBufferSize, false, true, true);
    vizWritePos = 0;
    for (auto& v : vizData.inputWaveformL) v.store(0.0f);
    for (auto& v : vizData.inputWaveformR) v.store(0.0f);
    for (auto& v : vizData.outputWaveformL) v.store(0.0f);
    for (auto& v : vizData.outputWaveformR) v.store(0.0f);
    for (auto& v : vizData.formantEnergyHistory) v.store(0.0f);
    for (auto& v : vizData.bandEnergy) v.store(0.0f);
    vizData.currentVowelShape.store(0.0f);
    vizData.currentFormantShift.store(0.0f);
    vizData.currentInstability.store(0.0f);
    vizData.currentGainDb.store(0.0f);
    vizData.inputLevel.store(0.0f);
    vizData.outputLevel.store(0.0f);
#endif
}

void VocalTractFilterModuleProcessor::ensureWorkBuffers(int numChannels, int numSamples)
{
    workBuffer.setSize(numChannels, numSamples, false, false, true);
    sumBuffer.setSize(numChannels, numSamples, false, false, true);
    tmpBuffer.setSize(numChannels, numSamples, false, false, true);
}

void VocalTractFilterModuleProcessor::updateCoefficients(float vowelShape, float formantShift, float instability)
{
    const FormantData* tables[] = { VOWEL_A, VOWEL_E, VOWEL_I, VOWEL_O, VOWEL_U };
    float shape = juce::jlimit(0.0f, 3.999f, vowelShape);
    int i0 = (int) std::floor(shape);
    int i1 = juce::jmin(4, i0 + 1);
    float t = shape - (float)i0;
    float shift = std::pow(2.0f, juce::jlimit(-1.0f, 1.0f, formantShift));
    float inst = juce::jlimit(0.0f, 1.0f, instability);
    float wow = wowOscillator.processSample(0.0f) * 0.03f * inst;
    float flt = flutterOscillator.processSample(0.0f) * 0.01f * inst;
    float mult = 1.0f + wow + flt;

    const FormantData* a = tables[i0];
    const FormantData* b = tables[i1];

    auto setBand = [&](IIRFilter& f, int bandIdx)
    {
        float cf = juce::jmap(t, a[bandIdx].frequency, b[bandIdx].frequency) * shift * mult;
        float q  = juce::jmap(t, a[bandIdx].q,         b[bandIdx].q);
        bandGains[bandIdx] = juce::jmap(t, a[bandIdx].gain, b[bandIdx].gain);
        cf = juce::jlimit(20.0f, (float)dspSpec.sampleRate * 0.49f, cf);
        q  = juce::jlimit(0.1f, 40.0f, q);
        f.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass(dspSpec.sampleRate, cf, q);
#if defined(PRESET_CREATOR_UI)
        vizData.formantFrequency[bandIdx].store(cf);
        vizData.formantGain[bandIdx].store(bandGains[bandIdx]);
        vizData.formantQ[bandIdx].store(q);
#endif
    };

    // Update both left and right channel filters with same coefficients
    setBand(bandsL.b0, 0);
    setBand(bandsL.b1, 1);
    setBand(bandsL.b2, 2);
    setBand(bandsL.b3, 3);
    
    setBand(bandsR.b0, 0);
    setBand(bandsR.b1, 1);
    setBand(bandsR.b2, 2);
    setBand(bandsR.b3, 3);
}

void VocalTractFilterModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    // Check for modulation inputs and read CV values
    float vowelModCV = 0.0f, formantModCV = 0.0f, instabilityModCV = 0.0f, gainModCV = 0.0f;
    
    if (isParamInputConnected("vowelShape")) {
        const auto& vowelModBus = getBusBuffer(buffer, true, 1);
        if (vowelModBus.getNumChannels() > 0)
            vowelModCV = vowelModBus.getReadPointer(0)[0];
    }
    
    if (isParamInputConnected("formantShift")) {
        const auto& formantModBus = getBusBuffer(buffer, true, 2);
        if (formantModBus.getNumChannels() > 0)
            formantModCV = formantModBus.getReadPointer(0)[0];
    }
    
    if (isParamInputConnected("instability")) {
        const auto& instabilityModBus = getBusBuffer(buffer, true, 3);
        if (instabilityModBus.getNumChannels() > 0)
            instabilityModCV = instabilityModBus.getReadPointer(0)[0];
    }
    
    if (isParamInputConnected("formantGain")) {
        const auto& gainModBus = getBusBuffer(buffer, true, 4);
        if (gainModBus.getNumChannels() > 0)
            gainModCV = gainModBus.getReadPointer(0)[0];
    }

    // Apply modulation to parameters
    float vowelShape = vowelShapeParam ? vowelShapeParam->load() : 0.0f;
    float formantShift = formantShiftParam ? formantShiftParam->load() : 0.0f;
    float instability = instabilityParam ? instabilityParam->load() : 0.0f;
    float outputGain = outputGainParam ? outputGainParam->load() : 0.0f;
    
    if (isParamInputConnected("vowelShape")) {
        vowelShape = juce::jlimit(0.0f, 4.0f, vowelShape + (vowelModCV - 0.5f) * 2.0f);
    }
    if (isParamInputConnected("formantShift")) {
        formantShift = juce::jlimit(-1.0f, 1.0f, formantShift + (formantModCV - 0.5f) * 2.0f);
    }
    if (isParamInputConnected("instability")) {
        instability = juce::jlimit(0.0f, 1.0f, instability + (instabilityModCV - 0.5f) * 0.5f);
    }
    if (isParamInputConnected("formantGain")) {
        outputGain = juce::jlimit(-24.0f, 24.0f, outputGain + (gainModCV - 0.5f) * 48.0f);
    }

    // Store live modulated values for UI display
    setLiveParamValue("vowelShape_live", vowelShape);
    setLiveParamValue("formantShift_live", formantShift);
    setLiveParamValue("instability_live", instability);
    setLiveParamValue("formantGain_live", outputGain);

#if defined(PRESET_CREATOR_UI)
    vizData.currentVowelShape.store(vowelShape);
    vizData.currentFormantShift.store(formantShift);
    vizData.currentInstability.store(instability);
    vizData.currentGainDb.store(outputGain);
#endif

    // Update coefficients every block so UI changes apply
    updateCoefficients(vowelShape, formantShift, instability);

    auto in  = getBusBuffer(buffer, true, 0);
    auto out = getBusBuffer(buffer, false, 0);
    if (in.getNumChannels() == 0 || out.getNumChannels() < 1)
    { out.clear(); return; }

    ensureWorkBuffers(2, numSamples); // Stereo work buffers

    // Process left and right channels separately
    std::array<float, 4> bandRms { 0.0f, 0.0f, 0.0f, 0.0f };
    float outGain = juce::Decibels::decibelsToGain(juce::jlimit(-24.0f, 24.0f, outputGain));

    // Process each channel
    for (int ch = 0; ch < juce::jmin(2, in.getNumChannels(), out.getNumChannels()); ++ch)
    {
        // Copy input channel to work buffer (mono processing per channel)
        workBuffer.copyFrom(0, 0, in, ch, 0, numSamples);

        // Sum of bands for this channel
        sumBuffer.clear();
        Bands& bandsToUse = (ch == 0) ? bandsL : bandsR;

        auto processBand = [&](IIRFilter& f, float g, int bandIdx)
        {
            tmpBuffer.makeCopyOf(workBuffer);
            juce::dsp::AudioBlock<float> b(tmpBuffer);
            juce::dsp::ProcessContextReplacing<float> ctx(b);
            f.process(ctx);
            tmpBuffer.applyGain(g);
            sumBuffer.addFrom(0, 0, tmpBuffer, 0, 0, numSamples);
#if defined(PRESET_CREATOR_UI)
            if (ch == 0) // Only measure band energy from left channel
                bandRms[bandIdx] = tmpBuffer.getRMSLevel(0, 0, numSamples);
#endif
        };

        processBand(bandsToUse.b0, bandGains[0], 0);
        processBand(bandsToUse.b1, bandGains[1], 1);
        processBand(bandsToUse.b2, bandGains[2], 2);
        processBand(bandsToUse.b3, bandGains[3], 3);

        // Apply gain and copy to output
        sumBuffer.applyGain(outGain);
        out.copyFrom(ch, 0, sumBuffer, 0, 0, numSamples);
    }
    
    // Update lastOutputValues for cable inspector (stereo)
    if (lastOutputValues.size() >= 2)
    {
        if (lastOutputValues[0] && out.getNumChannels() > 0)
            lastOutputValues[0]->store(out.getSample(0, out.getNumSamples() - 1));
        if (lastOutputValues[1] && out.getNumChannels() > 1)
            lastOutputValues[1]->store(out.getSample(1, out.getNumSamples() - 1));
        else if (lastOutputValues[1] && out.getNumChannels() > 0)
            lastOutputValues[1]->store(out.getSample(0, out.getNumSamples() - 1)); // Duplicate L if mono
    }

#if defined(PRESET_CREATOR_UI)
    // Capture waveforms into circular buffers (stereo)
    if (vizInputBufferL.getNumSamples() > 0 && in.getNumChannels() > 0)
    {
        const float* inDataL = in.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputBufferL.setSample(0, writeIdx, inDataL[i]);
        }
    }
    if (vizInputBufferR.getNumSamples() > 0 && in.getNumChannels() > 1)
    {
        const float* inDataR = in.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputBufferR.setSample(0, writeIdx, inDataR[i]);
        }
    }
    else if (vizInputBufferR.getNumSamples() > 0 && in.getNumChannels() > 0)
    {
        // Duplicate L to R if mono input
        const float* inDataL = in.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizInputBufferR.setSample(0, writeIdx, inDataL[i]);
        }
    }

    if (vizOutputBufferL.getNumSamples() > 0 && out.getNumChannels() > 0)
    {
        const float* outDataL = out.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizOutputBufferL.setSample(0, writeIdx, outDataL[i]);
        }
    }
    if (vizOutputBufferR.getNumSamples() > 0 && out.getNumChannels() > 1)
    {
        const float* outDataR = out.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizOutputBufferR.setSample(0, writeIdx, outDataR[i]);
        }
    }
    else if (vizOutputBufferR.getNumSamples() > 0 && out.getNumChannels() > 0)
    {
        // Duplicate L to R if mono output
        const float* outDataL = out.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const int writeIdx = (vizWritePos + i) % vizBufferSize;
            vizOutputBufferR.setSample(0, writeIdx, outDataL[i]);
        }
    }

    vizWritePos = (vizWritePos + numSamples) % vizBufferSize;

    // Downsample circular buffers into visualization arrays
    const int stride = juce::jmax(1, vizBufferSize / VizData::waveformPoints);
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        const int readIdx = (vizWritePos - VizData::waveformPoints * stride + i * stride + vizBufferSize) % vizBufferSize;
        if (vizInputBufferL.getNumSamples() > 0)
            vizData.inputWaveformL[i].store(vizInputBufferL.getSample(0, readIdx));
        if (vizInputBufferR.getNumSamples() > 0)
            vizData.inputWaveformR[i].store(vizInputBufferR.getSample(0, readIdx));
        if (vizOutputBufferL.getNumSamples() > 0)
            vizData.outputWaveformL[i].store(vizOutputBufferL.getSample(0, readIdx));
        if (vizOutputBufferR.getNumSamples() > 0)
            vizData.outputWaveformR[i].store(vizOutputBufferR.getSample(0, readIdx));
    }

    // Update band energy meters
    for (int i = 0; i < 4; ++i)
        vizData.bandEnergy[i].store(bandRms[i]);

    // Update input/output levels (average of L and R)
    const float inputRmsL = in.getNumChannels() > 0 ? in.getRMSLevel(0, 0, numSamples) : 0.0f;
    const float inputRmsR = in.getNumChannels() > 1 ? in.getRMSLevel(1, 0, numSamples) : inputRmsL;
    const float outputRmsL = out.getNumChannels() > 0 ? out.getRMSLevel(0, 0, numSamples) : 0.0f;
    const float outputRmsR = out.getNumChannels() > 1 ? out.getRMSLevel(1, 0, numSamples) : outputRmsL;
    vizData.inputLevel.store((inputRmsL + inputRmsR) * 0.5f);
    vizData.outputLevel.store((outputRmsL + outputRmsR) * 0.5f);
#endif
}

juce::AudioProcessorValueTreeState::ParameterLayout VocalTractFilterModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterFloat>("vowelShape",   "Vowel Shape",   0.0f, 4.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("formantShift", "Formant Shift", -1.0f, 1.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("instability",  "Instability",  0.0f, 1.0f, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("formantGain",  "Formant Gain", juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    return { p.begin(), p.end() };
}

#include <vector>
std::vector<DynamicPinInfo> VocalTractFilterModuleProcessor::getDynamicInputPins() const
{
    if (usesCustomPinLayout())
        return {};

    std::vector<DynamicPinInfo> pins;
    // Bus 0: Audio In (Stereo)
    pins.push_back({ "Audio In L", getChannelIndexInProcessBlockBuffer(true, 0, 0), PinDataType::Audio });
    pins.push_back({ "Audio In R", getChannelIndexInProcessBlockBuffer(true, 0, 1), PinDataType::Audio });
    // Bus 1-4: CV Mod inputs
    pins.push_back({ "Vowel Mod",       getChannelIndexInProcessBlockBuffer(true, 1, 0), PinDataType::CV });
    pins.push_back({ "Formant Mod",     getChannelIndexInProcessBlockBuffer(true, 2, 0), PinDataType::CV });
    pins.push_back({ "Instability Mod", getChannelIndexInProcessBlockBuffer(true, 3, 0), PinDataType::CV });
    pins.push_back({ "Gain Mod",        getChannelIndexInProcessBlockBuffer(true, 4, 0), PinDataType::CV });
    return pins;
}

std::vector<DynamicPinInfo> VocalTractFilterModuleProcessor::getDynamicOutputPins() const
{
    if (usesCustomPinLayout())
        return {};

    std::vector<DynamicPinInfo> pins;
    // Stereo output on bus 0
    pins.push_back({ "Audio Out L", 0, PinDataType::Audio });
    pins.push_back({ "Audio Out R", 1, PinDataType::Audio });
    return pins;
}

#if defined(PRESET_CREATOR_UI)
void VocalTractFilterModuleProcessor::drawParametersInNode(float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    if (!vowelShapeParam || !formantShiftParam || !instabilityParam || !outputGainParam) return;
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    ImGui::PushID(this);
    ImGui::PushItemWidth(itemWidth);

    // === Visualization ===
    ThemeText("Vocal Tract Activity", theme.text.section_header);
    ImGui::Spacing();

    ImGui::PushID(this);
    
    // Read visualization data (thread-safe) - BEFORE BeginChild
    float inputWaveformL[VizData::waveformPoints];
    float inputWaveformR[VizData::waveformPoints];
    float outputWaveformL[VizData::waveformPoints];
    float outputWaveformR[VizData::waveformPoints];
    for (int i = 0; i < VizData::waveformPoints; ++i)
    {
        inputWaveformL[i] = vizData.inputWaveformL[i].load();
        inputWaveformR[i] = vizData.inputWaveformR[i].load();
        outputWaveformL[i] = vizData.outputWaveformL[i].load();
        outputWaveformR[i] = vizData.outputWaveformR[i].load();
    }
    const float inputLevel = vizData.inputLevel.load();
    const float outputLevel = vizData.outputLevel.load();
    
    const ImU32 bgColor = ThemeManager::getInstance().getCanvasBackground();
    const ImU32 inputColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.frequency);
    const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(theme.accent);
    const ImU32 axisColor = IM_COL32(120, 120, 120, 120);

    const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    // Waveform view (input vs output)
    const float waveHeight = 150.0f;
    const ImVec2 waveGraphSize(itemWidth, waveHeight);
    if (ImGui::BeginChild("VocalTractWaveforms", waveGraphSize, false, childFlags))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + waveGraphSize.x, p0.y + waveGraphSize.y);
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        drawList->PushClipRect(p0, p1, true);

        const float stepX = waveGraphSize.x / (float)(VizData::waveformPoints - 1);
        const float midY = p0.y + waveGraphSize.y * 0.5f;
        const float scaleY = waveGraphSize.y * 0.45f;
        drawList->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), axisColor, 1.0f);

        auto drawWaveform = [&](const float* data, ImU32 color, float thickness, float alpha)
        {
            float prevX = p0.x;
            float prevY = midY;
            for (int i = 0; i < VizData::waveformPoints; ++i)
            {
                const float sample = juce::jlimit(-1.0f, 1.0f, data[i]);
                const float x = p0.x + i * stepX;
                const float y = midY - sample * scaleY;
                if (i > 0)
                {
                    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
                    c.w = alpha;
                    drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(c), thickness);
                }
                prevX = x;
                prevY = y;
            }
        };

        // Draw input waveforms (L and R, background layers)
        const ImU32 inputRColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre);
        drawWaveform(inputWaveformL, inputColor, 1.2f, 0.35f);
        drawWaveform(inputWaveformR, inputRColor, 1.2f, 0.35f);
        
        // Draw output waveforms (L and R, foreground layers)
        const ImU32 outputRColor = ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude);
        drawWaveform(outputWaveformL, outputColor, 2.5f, 0.9f);
        drawWaveform(outputWaveformR, outputRColor, 2.5f, 0.9f);

        drawList->PopClipRect();
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##vocalTractWaveformDrag", waveGraphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Formant map
    ThemeText("Formant Map", theme.text.section_header);
    const float mapHeight = 120.0f;
    const ImVec2 mapGraphSize(itemWidth, mapHeight);
    if (ImGui::BeginChild("VocalTractFormantMap", mapGraphSize, false, childFlags))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + mapGraphSize.x, p0.y + mapGraphSize.y);
        drawList->AddRectFilled(p0, p1, bgColor, 4.0f);
        drawList->PushClipRect(p0, p1, true);

        static constexpr float minFreq = 100.0f;
        static constexpr float maxFreq = 5000.0f;
        auto freqToX = [&](float freq)
        {
            const float clamped = juce::jlimit(minFreq, maxFreq, freq);
            const float norm = (std::log(clamped / minFreq) / std::log(maxFreq / minFreq));
            return p0.x + norm * mapGraphSize.x;
        };

        const ImU32 bandColors[4] = {
            ImGui::ColorConvertFloat4ToU32(theme.modulation.timbre),
            ImGui::ColorConvertFloat4ToU32(theme.modulation.amplitude),
            ImGui::ColorConvertFloat4ToU32(theme.modulation.filter),
            ImGui::ColorConvertFloat4ToU32(theme.accent)
        };

        for (int i = 0; i < 4; ++i)
        {
            const float freq = vizData.formantFrequency[i].load();
            const float gain = vizData.formantGain[i].load();
            const float q = vizData.formantQ[i].load();
            const float energy = vizData.bandEnergy[i].load();
            const float x = freqToX(freq);
            const float radius = juce::jlimit(4.0f, 24.0f, 200.0f * energy + 4.0f);
            const float alpha = juce::jlimit(0.3f, 1.0f, 0.3f + gain);
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(bandColors[i]);
            c.w = alpha;
            drawList->AddCircleFilled(ImVec2(x, p0.y + mapGraphSize.y * 0.6f), radius, ImGui::ColorConvertFloat4ToU32(c), 32);
            drawList->AddLine(ImVec2(x, p0.y + 6.0f), ImVec2(x, p1.y - 6.0f), bandColors[i], 1.2f);
            char label[64];
            std::snprintf(label, sizeof(label), "F%d\n%.0f Hz\nQ %.1f", i + 1, freq, q);
            drawList->AddText(ImVec2(x - 22.0f, p1.y - 36.0f), bandColors[i], label);
        }

        drawList->PopClipRect();
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##vocalTractFormantDrag", mapGraphSize);
    }
    ImGui::EndChild();

    ImGui::PopID(); // End unique ID for visualization section
    
    ImGui::Spacing();
    
    // Live readouts (constrained width to prevent node expansion)
    // Use BeginChild with fixed width/height to prevent text from expanding the node
    const float readoutHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
    const ImVec2 readoutSize(itemWidth, readoutHeight);
    if (ImGui::BeginChild("VocalTractReadouts", readoutSize, false, ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::Text("In: %.1f dB  |  Out: %.1f dB", juce::Decibels::gainToDecibels(juce::jmax(1.0e-5f, inputLevel)),
                    juce::Decibels::gainToDecibels(juce::jmax(1.0e-5f, outputLevel)));
        ImGui::Text("Vowel: %.2f  |  Formant: %.2f  |  Instability: %.2f  |  Gain: %.1f dB",
                    vizData.currentVowelShape.load(),
                    vizData.currentFormantShift.load(),
                    vizData.currentInstability.load(),
                    vizData.currentGainDb.load());
    }
    ImGui::EndChild();
    
    ImGui::Spacing();
    ImGui::Spacing();

    // Vowel Shape
    bool isVowelModulated = isParamModulated("vowelShape");
    float v = vowelShapeParam->load();
    if (isVowelModulated) {
        v = getLiveParamValueFor("vowelShape", "vowelShape_live", v);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Vowel Mod (channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting("vowelShape", busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }
    if (ImGui::SliderFloat("Vowel", &v, 0.0f, 4.0f, "%.1f")) {
        if (!isVowelModulated) { *vowelShapeParam = v; }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isVowelModulated) { onModificationEnded(); }
    if (!isVowelModulated) adjustParamOnWheel(apvts.getParameter("vowelShape"), "vowelShape", v);
    if (isVowelModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    
    // Formant Shift
    bool isFormantModulated = isParamModulated("formantShift");
    float s = formantShiftParam->load();
    if (isFormantModulated) {
        s = getLiveParamValueFor("formantShift", "formantShift_live", s);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Formant Mod (channel 3)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting("formantShift", busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }
    if (ImGui::SliderFloat("Formant", &s, -1.0f, 1.0f, "%.2f")) {
        if (!isFormantModulated) { *formantShiftParam = s; }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isFormantModulated) { onModificationEnded(); }
    if (!isFormantModulated) adjustParamOnWheel(apvts.getParameter("formantShift"), "formantShift", s);
    if (isFormantModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    
    // Instability
    bool isInstabilityModulated = isParamModulated("instability");
    float i = instabilityParam->load();
    if (isInstabilityModulated) {
        i = getLiveParamValueFor("instability", "instability_live", i);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Instability Mod (channel 4)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting("instability", busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }
    if (ImGui::SliderFloat("Instab", &i, 0.0f, 1.0f, "%.2f")) {
        if (!isInstabilityModulated) { *instabilityParam = i; }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isInstabilityModulated) { onModificationEnded(); }
    if (!isInstabilityModulated) adjustParamOnWheel(apvts.getParameter("instability"), "instability", i);
    if (isInstabilityModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    
    // Gain
    bool isGainModulated = isParamModulated("formantGain");
    float g = outputGainParam->load();
    if (isGainModulated) {
        g = getLiveParamValueFor("formantGain", "formantGain_live", g);
        ImGui::BeginDisabled();
    }
    // Draw inline pin for Gain Mod (channel 5)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        int busIdx, chanInBus;
        if (getParamRouting("formantGain", busIdx, chanInBus))
        {
            int channel = getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus);
            if (pinHelpers->drawInlineInputPin(channel))
                ImGui::SameLine();
        }
    }
    if (ImGui::SliderFloat("Gain", &g, -24.0f, 24.0f, "%.1f dB")) {
        if (!isGainModulated) { *outputGainParam = g; }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && !isGainModulated) { onModificationEnded(); }
    if (!isGainModulated) adjustParamOnWheel(apvts.getParameter("formantGain"), "formantGain", g);
    if (isGainModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }
    
    ImGui::PopItemWidth();
    ImGui::PopID();
}

void VocalTractFilterModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    // Only draw audio pins - modulation pins are now inline
    helpers.drawParallelPins("Audio In L", 0, "Audio Out L", 0);
    helpers.drawParallelPins("Audio In R", 1, "Audio Out R", 1);
    // Vowel Mod, Formant Mod, Instability Mod, Gain Mod are drawn inline in drawParametersInNode
}
#endif

bool VocalTractFilterModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    outChannelIndexInBus = 0;
    if (paramId == "vowelShape") { outBusIndex = 1; return true; }
    if (paramId == "formantShift") { outBusIndex = 2; return true; }
    if (paramId == "instability") { outBusIndex = 3; return true; }
    if (paramId == "formantGain") { outBusIndex = 4; return true; }
    return false;
}
