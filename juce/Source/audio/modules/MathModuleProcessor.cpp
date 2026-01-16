#include "MathModuleProcessor.h"
#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif

MathModuleProcessor::MathModuleProcessor()
    : ModuleProcessor (BusesProperties()
                        .withInput ("In A", juce::AudioChannelSet::mono(), true)
                        .withInput ("In B", juce::AudioChannelSet::mono(), true)
                        .withInput ("CV Mod", juce::AudioChannelSet::mono(), true)
                        .withOutput("Out", juce::AudioChannelSet::mono(), true)),
      apvts (*this, nullptr, "MathParams", createParameterLayout())
{
    valueAParam    = apvts.getRawParameterValue ("valueA");
    valueBParam    = apvts.getRawParameterValue ("valueB");
    operationParam = apvts.getRawParameterValue ("operation");
    operationModParam = apvts.getRawParameterValue ("operation_mod");
    
    // ADD THIS:
    lastOutputValues.push_back(std::make_unique<std::atomic<float>>(0.0f));
}

juce::AudioProcessorValueTreeState::ParameterLayout MathModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    // Enhanced operation list with 30 mathematical functions
    p.push_back (std::make_unique<juce::AudioParameterChoice> ("operation", "Operation", 
        juce::StringArray { 
            "Add", "Subtract", "Multiply", "Divide",
            "Min", "Max", "Power", "Sqrt(A)",
            "Sin(A)", "Cos(A)", "Tan(A)",
            "Abs(A)", "Modulo", "Fract(A)", "Int(A)",
            "A > B", "A < B",
            "Log(A)", "Log10(A)", "Exp(A)", "Exp2(A)",
            "Limit(A, B)", "A == B", "A != B", "A >= B", "A <= B",
            "Round(A)", "Sign(A)", "Atan(A)", "Atan2(A, B)"
        }, 0));
    // New Value A slider default
    p.push_back (std::make_unique<juce::AudioParameterFloat> ("valueA", "Value A", juce::NormalisableRange<float> (-100.0f, 100.0f), 0.0f));
    // Expanded Value B range from -100 to 100 for more creative possibilities
    p.push_back (std::make_unique<juce::AudioParameterFloat> ("valueB", "Value B", juce::NormalisableRange<float> (-100.0f, 100.0f), 0.0f));
    // Operation modulation CV input (0-1 maps to 0-29 operation indices - 30 operations total)
    p.push_back (std::make_unique<juce::AudioParameterFloat> ("operation_mod", "Operation Mod", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    return { p.begin(), p.end() };
}

void MathModuleProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate);
#if defined(PRESET_CREATOR_UI)
    captureBuffer.setSize(3, samplesPerBlock); // 0=In A, 1=In B, 2=Out
    captureBuffer.clear();
    for (auto& v : vizData.inputAWaveform) v.store(0.0f);
    for (auto& v : vizData.inputBWaveform) v.store(0.0f);
    for (auto& v : vizData.outputWaveform) v.store(0.0f);
    vizData.writeIndex.store(0);
    vizData.inputARms.store(0.0f);
    vizData.inputBRms.store(0.0f);
    vizData.outputRms.store(0.0f);
#endif
}

void MathModuleProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);

    auto inA = getBusBuffer (buffer, true, 0);
    auto out = getBusBuffer (buffer, false, 0);

    // CORRECTED LOGIC:
    auto inB = getBusBuffer(buffer, true, 1);
    auto cvModBus = getBusBuffer(buffer, true, 2);
    
    // Use robust connection detection
    const bool inAConnected = isParamInputConnected("valueA");
    const bool inBConnected = isParamInputConnected("valueB");
    const bool operationModConnected = isParamInputConnected("operation_mod");

    const float valueA = valueAParam != nullptr ? valueAParam->load() : 0.0f;
    const float valueB = valueBParam->load();
    
    // Get base operation index
    int baseOperation = static_cast<int>(operationParam->load());
    
    // Get CV pointer for per-sample modulation (if connected)
    const float* operationModPtr = nullptr;
    if (operationModConnected && cvModBus.getNumChannels() > 0)
    {
        operationModPtr = cvModBus.getReadPointer(0);
    }
    
    const float* srcA = inA.getNumChannels() > 0 ? inA.getReadPointer (0) : nullptr;
    const float* srcB = inBConnected ? inB.getReadPointer (0) : nullptr;
    float* dst = out.getWritePointer (0);

    float sum = 0.0f;
    float sumA = 0.0f;
    float sumB = 0.0f;
    
#if defined(PRESET_CREATOR_UI)
    // Capture audio for visualization
    const int numSamples = buffer.getNumSamples();
    if (captureBuffer.getNumSamples() < numSamples)
        captureBuffer.setSize(3, numSamples, false, false, true);
    
    float rmsA = 0.0f;
    float rmsB = 0.0f;
    float rmsOut = 0.0f;
    int currentOperationForViz = baseOperation; // Track operation for visualization
#endif
    
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        float valA = inAConnected && srcA != nullptr ? srcA[i] : valueA;
        float valB = inBConnected ? srcB[i] : valueB;

        // Calculate operation index with modulation (per-sample if CV connected)
        int operation = baseOperation;
        if (operationModPtr != nullptr)
        {
            // Map CV (0-1) to operation index (0-29) - 30 operations total
            // Evenly distribute: CV 0.0 -> op 0, CV 1.0 -> op 29
            float modCV = juce::jlimit(0.0f, 1.0f, operationModPtr[i]);
            operation = static_cast<int>(juce::jlimit(0.0f, 29.0f, modCV * 29.0f));
        }
        
#if defined(PRESET_CREATOR_UI)
        // Track operation for visualization (use first sample's operation)
        if (i == 0)
            currentOperationForViz = operation;
#endif

        // Enhanced mathematical operations with 30 different functions
        float result = 0.0f;
        switch (operation)
        {
            case 0:  result = valA + valB; break; // Add
            case 1:  result = valA - valB; break; // Subtract
            case 2:  result = valA * valB; break; // Multiply
            case 3:  result = (std::abs(valB) < 1e-9f) ? 0.0f : (valA / valB); break; // Divide (safe)
            case 4:  result = std::min(valA, valB); break; // Min
            case 5:  result = std::max(valA, valB); break; // Max
            case 6:  result = std::pow(valA, valB); break; // Power
            case 7:  result = std::sqrt(std::abs(valA)); break; // Sqrt(A) - only on A
            case 8:  result = std::sin(valA * juce::MathConstants<float>::twoPi); break; // Sin(A) - only on A
            case 9:  result = std::cos(valA * juce::MathConstants<float>::twoPi); break; // Cos(A) - only on A
            case 10: result = std::tan(valA * juce::MathConstants<float>::pi); break; // Tan(A) - only on A
            case 11: result = std::abs(valA); break; // Abs(A) - only on A
            case 12: result = (std::abs(valB) < 1e-9f) ? 0.0f : std::fmod(valA, valB); break; // Modulo (safe)
            case 13: result = valA - std::trunc(valA); break; // Fract(A) - only on A
            case 14: result = std::trunc(valA); break; // Int(A) - only on A
            case 15: result = (valA > valB) ? 1.0f : 0.0f; break; // A > B
            case 16: result = (valA < valB) ? 1.0f : 0.0f; break; // A < B
            case 17: result = (valA > 1e-9f) ? std::log(valA) : -20.0f; break; // Log(A) - only on A, safe for <= 0
            case 18: result = (valA > 1e-9f) ? std::log10(valA) : -20.0f; break; // Log10(A) - only on A, safe for <= 0
            case 19: result = std::exp(juce::jlimit(-10.0f, 10.0f, valA)); break; // Exp(A) - only on A, clamped to prevent overflow
            case 20: result = std::exp2(juce::jlimit(-10.0f, 10.0f, valA)); break; // Exp2(A) - only on A, clamped to prevent overflow
            case 21: result = juce::jlimit(-std::abs(valB), std::abs(valB), valA); break; // Limit(A, B) - clamps A to [-|B|, |B|]
            case 22: result = (std::abs(valA - valB) < 1e-6f) ? 1.0f : 0.0f; break; // A == B
            case 23: result = (std::abs(valA - valB) >= 1e-6f) ? 1.0f : 0.0f; break; // A != B
            case 24: result = (valA >= valB) ? 1.0f : 0.0f; break; // A >= B
            case 25: result = (valA <= valB) ? 1.0f : 0.0f; break; // A <= B
            case 26: result = std::round(valA); break; // Round(A) - only on A
            case 27: result = (valA > 1e-9f) ? 1.0f : ((valA < -1e-9f) ? -1.0f : 0.0f); break; // Sign(A) - only on A
            case 28: result = std::atan(valA) / juce::MathConstants<float>::pi; break; // Atan(A) - only on A, normalized to [-1, 1]
            case 29: result = std::atan2(valA, valB) / juce::MathConstants<float>::pi; break; // Atan2(A, B) - normalized to [-1, 1]
        }
        dst[i] = result;
        
        sum += result;
        sumA += valA;
        sumB += valB;
        
#if defined(PRESET_CREATOR_UI)
        // Capture for visualization
        captureBuffer.setSample(0, i, valA);
        captureBuffer.setSample(1, i, valB);
        captureBuffer.setSample(2, i, result);
        
        rmsA += valA * valA;
        rmsB += valB * valB;
        rmsOut += result * result;
#endif
        
        // Update telemetry for live UI feedback (throttled to every 64 samples)
        if ((i & 0x3F) == 0) {
            setLiveParamValue("valueA_live", valA);
            setLiveParamValue("valueB_live", valB);
            setLiveParamValue("operation_live", static_cast<float>(operation));
            if (operationModPtr != nullptr)
            {
                setLiveParamValue("operation_mod_live", operationModPtr[i]);
            }
        }
    }
    
#if defined(PRESET_CREATOR_UI)
    // Calculate RMS and update visualization data
    const int numSamplesForRms = buffer.getNumSamples();
    rmsA = std::sqrt(rmsA / numSamplesForRms);
    rmsB = std::sqrt(rmsB / numSamplesForRms);
    rmsOut = std::sqrt(rmsOut / numSamplesForRms);
    
    vizData.inputARms.store(rmsA);
    vizData.inputBRms.store(rmsB);
    vizData.outputRms.store(rmsOut);
    vizData.currentOperation.store(currentOperationForViz);
    
    // Down-sample waveforms
    const int stride = juce::jmax(1, numSamplesForRms / VizData::waveformPoints);
    int writeIdx = vizData.writeIndex.load();
    
    for (int i = 0; i < VizData::waveformPoints && (i * stride) < numSamplesForRms; ++i)
    {
        const int sampleIdx = i * stride;
        vizData.inputAWaveform[i].store(captureBuffer.getSample(0, sampleIdx));
        vizData.inputBWaveform[i].store(captureBuffer.getSample(1, sampleIdx));
        vizData.outputWaveform[i].store(captureBuffer.getSample(2, sampleIdx));
    }
    
    writeIdx = (writeIdx + 1) % VizData::waveformPoints;
    vizData.writeIndex.store(writeIdx);
#endif
    lastValue.store(sum / (float) buffer.getNumSamples());
    lastValueA.store(sumA / (float) buffer.getNumSamples());
    lastValueB.store(sumB / (float) buffer.getNumSamples());
    
    // ADD THIS BLOCK:
    if (!lastOutputValues.empty() && lastOutputValues[0])
    {
        lastOutputValues[0]->store(out.getSample(0, buffer.getNumSamples() - 1));
    }
}

#if defined(PRESET_CREATOR_UI)
void MathModuleProcessor::drawParametersInNode (float itemWidth, const std::function<bool(const juce::String& paramId)>& isParamModulated, const std::function<void()>& onModificationEnded, const NodePinHelpers* pinHelpers)
{
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();
    auto& ap = getAPVTS();
    ImGui::PushID(this);
    ImGui::PushItemWidth (itemWidth);
    
    // Operation combo box with CV modulation and scroll-edit support
    bool isOperationModulated = isParamModulated("operation_mod");
    int op = 0; 
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("operation"))) 
        op = p->getIndex();
    
    if (isOperationModulated) {
        op = static_cast<int>(getLiveParamValueFor("operation_mod", "operation_live", static_cast<float>(op)));
    }
    
    // Inline pin for Op Mod (Channel 2)
    if (pinHelpers && pinHelpers->drawInlineInputPin)
    {
        if (pinHelpers->drawInlineInputPin(2))
            ImGui::SameLine();
    }
    
    if (isOperationModulated) ImGui::BeginDisabled();
    if (ImGui::Combo ("Operation", &op, 
        "Add\0Subtract\0Multiply\0Divide\0"
        "Min\0Max\0Power\0Sqrt(A)\0"
        "Sin(A)\0Cos(A)\0Tan(A)\0"
        "Abs(A)\0Modulo\0Fract(A)\0Int(A)\0"
        "A > B\0A < B\0"
        "Log(A)\0Log10(A)\0Exp(A)\0Exp2(A)\0"
        "Limit(A, B)\0A == B\0A != B\0A >= B\0A <= B\0"
        "Round(A)\0Sign(A)\0Atan(A)\0Atan2(A, B)\0\0"))
        if (!isOperationModulated) 
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("operation"))) 
                *p = op;
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    
    // Scroll wheel editing for Operation combo
    if (!isOperationModulated && ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const int maxIndex = 29; // 30 operations: 0-29
            const int newIndex = juce::jlimit(0, maxIndex, op + (wheel > 0.0f ? -1 : 1));
            if (newIndex != op) {
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(ap.getParameter("operation"))) {
                    *p = newIndex;
                    onModificationEnded();
                }
            }
        }
    }
    
    if (isOperationModulated) { 
        ImGui::EndDisabled(); 
        ImGui::SameLine(); 
        ImGui::TextUnformatted("(mod)"); 
    }
    
    float valA = valueAParam != nullptr ? valueAParam->load() : 0.0f;
    float valB = valueBParam != nullptr ? valueBParam->load() : 0.0f;

    // Value A slider with live modulation feedback
    bool isValueAModulated = isParamModulated("valueA");
    if (isValueAModulated) {
        valA = getLiveParamValueFor("valueA", "valueA_live", valA);
        ImGui::BeginDisabled();
    }
    if (ImGui::SliderFloat ("Value A", &valA, -100.0f, 100.0f)) {
        if (!isValueAModulated) {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("valueA"))) *p = valA;
        }
    }
    if (!isValueAModulated) adjustParamOnWheel (ap.getParameter("valueA"), "valueA", valA);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (isValueAModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }

    // Value B slider with live modulation feedback
    bool isValueBModulated = isParamModulated("valueB");
    if (isValueBModulated) {
        valB = getLiveParamValueFor("valueB", "valueB_live", valB);
        ImGui::BeginDisabled();
    }
    if (ImGui::SliderFloat ("Value B", &valB, -100.0f, 100.0f)) {
        if (!isValueBModulated) {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(ap.getParameter("valueB"))) *p = valB;
        }
    }
    if (!isValueBModulated) adjustParamOnWheel (ap.getParameter("valueB"), "valueB", valB);
    if (ImGui::IsItemDeactivatedAfterEdit()) onModificationEnded();
    if (isValueBModulated) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextUnformatted("(mod)"); }

    ImGui::Spacing();
    
    // Waveform Visualization
    const float graphHeight = 120.0f;
    const ImVec2 graphSize(itemWidth, graphHeight);
    
    // Use unique ID for child window to avoid conflicts with multiple instances
    if (ImGui::BeginChild("MathWaveformViz", graphSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + graphSize.x, p0.y + graphSize.y);
        
        // Background
        const auto& freqColors = theme.modules.frequency_graph;
        const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
        const ImU32 bgColor = resolveColor(freqColors.background, IM_COL32(18, 20, 24, 255));
        drawList->AddRectFilled(p0, p1, bgColor);
        
        // Grid lines
        const ImU32 gridColor = resolveColor(freqColors.grid, IM_COL32(50, 55, 65, 255));
        const float centerY = p0.y + graphSize.y * 0.5f;
        drawList->AddLine(ImVec2(p0.x, centerY), ImVec2(p1.x, centerY), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), gridColor, 1.0f);
        
        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);
        
        // Read waveform data
        std::array<float, VizData::waveformPoints> inputA, inputB, output;
        for (int i = 0; i < VizData::waveformPoints; ++i)
        {
            inputA[i] = vizData.inputAWaveform[i].load();
            inputB[i] = vizData.inputBWaveform[i].load();
            output[i] = vizData.outputWaveform[i].load();
        }
        
        // Determine operation type: single-input vs dual-input
        int currentOp = vizData.currentOperation.load();
        // Single-input operations: 7-11 (Sqrt, Sin, Cos, Tan, Abs), 13-14 (Fract, Int), 17-20 (Log, Log10, Exp, Exp2), 26-28 (Round, Sign, Atan)
        bool isSingleInputOp = (currentOp >= 7 && currentOp <= 11) || (currentOp >= 13 && currentOp <= 14) || 
                               (currentOp >= 17 && currentOp <= 20) || (currentOp >= 26 && currentOp <= 28);
        
        // Draw waveforms
        const float halfHeight = graphSize.y * 0.5f;
        const float scale = halfHeight * 0.9f; // Leave 10% margin
        
        // Visual styling constants
        const float originalAlpha = 0.5f;        // Faded original signals
        const float originalThickness = 1.0f;   // Thinner lines for originals
        const float transformedAlpha = 1.0f;    // Bright transformed output
        const float transformedThickness = 2.5f; // Thicker line for output
        const float verticalOffset = 2.0f;      // Offset output downward to show it's "on top"
        
        // Input A waveform (cyan/blue) - shown as original for all operations
        ImU32 colorA = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.8f, 1.0f, originalAlpha));
        for (int i = 1; i < VizData::waveformPoints; ++i)
        {
            float x0 = p0.x + (float)(i - 1) / (float)(VizData::waveformPoints - 1) * graphSize.x;
            float x1 = p0.x + (float)i / (float)(VizData::waveformPoints - 1) * graphSize.x;
            float y0 = juce::jlimit(p0.y, p1.y, centerY - inputA[i - 1] * scale);
            float y1 = juce::jlimit(p0.y, p1.y, centerY - inputA[i] * scale);
            drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorA, originalThickness);
        }
        
        // Input B waveform (magenta/pink) - shown as original only for dual-input operations
        if (!isSingleInputOp)
        {
            ImU32 colorB = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.3f, 0.8f, originalAlpha));
            for (int i = 1; i < VizData::waveformPoints; ++i)
            {
                float x0 = p0.x + (float)(i - 1) / (float)(VizData::waveformPoints - 1) * graphSize.x;
                float x1 = p0.x + (float)i / (float)(VizData::waveformPoints - 1) * graphSize.x;
                float y0 = juce::jlimit(p0.y, p1.y, centerY - inputB[i - 1] * scale);
                float y1 = juce::jlimit(p0.y, p1.y, centerY - inputB[i] * scale);
                drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorB, originalThickness);
            }
        }
        
        // Output waveform (white/yellow) - always shown as transformed (bright, thick, offset)
        ImU32 colorOut = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 0.6f, transformedAlpha));
        for (int i = 1; i < VizData::waveformPoints; ++i)
        {
            float x0 = p0.x + (float)(i - 1) / (float)(VizData::waveformPoints - 1) * graphSize.x;
            float x1 = p0.x + (float)i / (float)(VizData::waveformPoints - 1) * graphSize.x;
            // Add vertical offset to show output is "on top" of the original
            float y0 = juce::jlimit(p0.y, p1.y, centerY - output[i - 1] * scale + verticalOffset);
            float y1 = juce::jlimit(p0.y, p1.y, centerY - output[i] * scale + verticalOffset);
            drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorOut, transformedThickness);
        }
        
        drawList->PopClipRect();
        
        // Operation label and RMS values
        const char* opNames[] = {
            "Add", "Subtract", "Multiply", "Divide", "Min", "Max", "Power",
            "Sqrt(A)", "Sin(A)", "Cos(A)", "Tan(A)", "Abs(A)", "Modulo",
            "Fract(A)", "Int(A)", "A > B", "A < B",
            "Log(A)", "Log10(A)", "Exp(A)", "Exp2(A)",
            "Limit(A, B)", "A == B", "A != B", "A >= B", "A <= B",
            "Round(A)", "Sign(A)", "Atan(A)", "Atan2(A, B)"
        };
        // currentOp already declared above for operation type detection
        if (currentOp >= 0 && currentOp < 30)
        {
            ImGui::SetCursorPos(ImVec2(4, 4));
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%s", opNames[currentOp]);
        }
        
        // RMS values
        float rmsA = vizData.inputARms.load();
        float rmsB = vizData.inputBRms.load();
        float rmsOut = vizData.outputRms.load();
        
        ImGui::SetCursorPos(ImVec2(4, graphHeight - 60));
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "A: %.3f", rmsA);
        ImGui::SetCursorPos(ImVec2(4, graphHeight - 45));
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.8f, 1.0f), "B: %.3f", rmsB);
        ImGui::SetCursorPos(ImVec2(4, graphHeight - 30));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "Out: %.3f", rmsOut);
        
        // Legend
        ImGui::SetCursorPos(ImVec2(itemWidth - 80, graphHeight - 60));
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "In A");
        ImGui::SetCursorPos(ImVec2(itemWidth - 80, graphHeight - 45));
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.8f, 1.0f), "In B");
        ImGui::SetCursorPos(ImVec2(itemWidth - 80, graphHeight - 30));
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "Out");
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##mathWaveformDrag", graphSize);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Pedagogical Visualization - Example of how the operation transforms a simple signal
    const float pedagogicalHeight = 120.0f;
    const ImVec2 pedagogicalSize(itemWidth, pedagogicalHeight);
    
    if (ImGui::BeginChild("MathPedagogicalViz", pedagogicalSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + pedagogicalSize.x, p0.y + pedagogicalSize.y);
        
        // Background
        const auto& freqColors = theme.modules.frequency_graph;
        const auto resolveColor = [](ImU32 value, ImU32 fallback) { return value != 0 ? value : fallback; };
        const ImU32 bgColor = resolveColor(freqColors.background, IM_COL32(18, 20, 24, 255));
        drawList->AddRectFilled(p0, p1, bgColor);
        
        // Grid lines
        const ImU32 gridColor = resolveColor(freqColors.grid, IM_COL32(50, 55, 65, 255));
        const float centerY = p0.y + pedagogicalSize.y * 0.5f;
        drawList->AddLine(ImVec2(p0.x, centerY), ImVec2(p1.x, centerY), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y), gridColor, 1.0f);
        drawList->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y), gridColor, 1.0f);
        
        // Clip to graph area
        drawList->PushClipRect(p0, p1, true);
        
        // Generate animated demo signals
        const int numPoints = VizData::waveformPoints;
        std::array<float, VizData::waveformPoints> demoA, demoB, demoOutput;
        
        // Animation phase based on time
        const float time = static_cast<float>(ImGui::GetTime());
        const float animationSpeed = 0.3f; // Cycles per second
        const float phase = time * animationSpeed * juce::MathConstants<float>::twoPi;
        
        // Get current operation
        int currentOp = vizData.currentOperation.load();
        bool isSingleInputOp = (currentOp >= 7 && currentOp <= 11) || (currentOp >= 13 && currentOp <= 14);
        
        // Generate Input A: 1.5 cycles sine wave, amplitude 0.8
        const float cyclesA = 1.5f;
        const float amplitudeA = 0.8f;
        for (int i = 0; i < numPoints; ++i)
        {
            const float x = (float)i / (float)(numPoints - 1);
            demoA[i] = std::sin(x * cyclesA * juce::MathConstants<float>::twoPi + phase) * amplitudeA;
        }
        
        // Generate Input B: 1.0 cycle sine wave, amplitude 0.5, 90° phase offset (for dual-input ops)
        const float cyclesB = 1.0f;
        const float amplitudeB = 0.5f;
        const float phaseOffsetB = juce::MathConstants<float>::pi * 0.5f; // 90 degrees
        for (int i = 0; i < numPoints; ++i)
        {
            const float x = (float)i / (float)(numPoints - 1);
            demoB[i] = std::sin(x * cyclesB * juce::MathConstants<float>::twoPi + phase + phaseOffsetB) * amplitudeB;
        }
        
        // Apply current operation to compute output
        for (int i = 0; i < numPoints; ++i)
        {
            const float valA = demoA[i];
            const float valB = demoB[i];
            float result = 0.0f;
            
            switch (currentOp)
            {
                case 0:  result = valA + valB; break; // Add
                case 1:  result = valA - valB; break; // Subtract
                case 2:  result = valA * valB; break; // Multiply
                case 3:  result = (std::abs(valB) < 1e-9f) ? 0.0f : (valA / valB); break; // Divide
                case 4:  result = std::min(valA, valB); break; // Min
                case 5:  result = std::max(valA, valB); break; // Max
                case 6:  result = std::pow(valA, valB); break; // Power
                case 7:  result = std::sqrt(std::abs(valA)); break; // Sqrt(A)
                case 8:  result = std::sin(valA * juce::MathConstants<float>::twoPi); break; // Sin(A)
                case 9:  result = std::cos(valA * juce::MathConstants<float>::twoPi); break; // Cos(A)
                case 10: result = std::tan(valA * juce::MathConstants<float>::pi); break; // Tan(A)
                case 11: result = std::abs(valA); break; // Abs(A)
                case 12: result = (std::abs(valB) < 1e-9f) ? 0.0f : std::fmod(valA, valB); break; // Modulo
                case 13: result = valA - std::trunc(valA); break; // Fract(A)
                case 14: result = std::trunc(valA); break; // Int(A)
                case 15: result = (valA > valB) ? 1.0f : 0.0f; break; // A > B
                case 16: result = (valA < valB) ? 1.0f : 0.0f; break; // A < B
                case 17: result = (valA > 1e-9f) ? std::log(valA) : -20.0f; break; // Log(A)
                case 18: result = (valA > 1e-9f) ? std::log10(valA) : -20.0f; break; // Log10(A)
                case 19: result = std::exp(juce::jlimit(-10.0f, 10.0f, valA)); break; // Exp(A)
                case 20: result = std::exp2(juce::jlimit(-10.0f, 10.0f, valA)); break; // Exp2(A)
                case 21: result = juce::jlimit(-std::abs(valB), std::abs(valB), valA); break; // Limit(A, B)
                case 22: result = (std::abs(valA - valB) < 1e-6f) ? 1.0f : 0.0f; break; // A == B
                case 23: result = (std::abs(valA - valB) >= 1e-6f) ? 1.0f : 0.0f; break; // A != B
                case 24: result = (valA >= valB) ? 1.0f : 0.0f; break; // A >= B
                case 25: result = (valA <= valB) ? 1.0f : 0.0f; break; // A <= B
                case 26: result = std::round(valA); break; // Round(A)
                case 27: result = (valA > 1e-9f) ? 1.0f : ((valA < -1e-9f) ? -1.0f : 0.0f); break; // Sign(A)
                case 28: result = std::atan(valA) / juce::MathConstants<float>::pi; break; // Atan(A)
                case 29: result = std::atan2(valA, valB) / juce::MathConstants<float>::pi; break; // Atan2(A, B)
            }
            
            // Clamp result to reasonable range for visualization
            demoOutput[i] = juce::jlimit(-2.0f, 2.0f, result);
        }
        
        // Draw waveforms with same styling as live visualization
        const float halfHeight = pedagogicalSize.y * 0.5f;
        const float scale = halfHeight * 0.9f;
        
        const float originalAlpha = 0.5f;
        const float originalThickness = 1.0f;
        const float transformedAlpha = 1.0f;
        const float transformedThickness = 2.5f;
        const float verticalOffset = 2.0f;
        
        // Input A waveform (cyan/blue) - shown for all operations
        ImU32 colorA = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.8f, 1.0f, originalAlpha));
        for (int i = 1; i < numPoints; ++i)
        {
            float x0 = p0.x + (float)(i - 1) / (float)(numPoints - 1) * pedagogicalSize.x;
            float x1 = p0.x + (float)i / (float)(numPoints - 1) * pedagogicalSize.x;
            float y0 = juce::jlimit(p0.y, p1.y, centerY - demoA[i - 1] * scale);
            float y1 = juce::jlimit(p0.y, p1.y, centerY - demoA[i] * scale);
            drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorA, originalThickness);
        }
        
        // Input B waveform (magenta/pink) - shown only for dual-input operations
        if (!isSingleInputOp)
        {
            ImU32 colorB = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.3f, 0.8f, originalAlpha));
            for (int i = 1; i < numPoints; ++i)
            {
                float x0 = p0.x + (float)(i - 1) / (float)(numPoints - 1) * pedagogicalSize.x;
                float x1 = p0.x + (float)i / (float)(numPoints - 1) * pedagogicalSize.x;
                float y0 = juce::jlimit(p0.y, p1.y, centerY - demoB[i - 1] * scale);
                float y1 = juce::jlimit(p0.y, p1.y, centerY - demoB[i] * scale);
                drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorB, originalThickness);
            }
        }
        
        // Output waveform (white/yellow) - always shown as transformed
        ImU32 colorOut = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 0.6f, transformedAlpha));
        for (int i = 1; i < numPoints; ++i)
        {
            float x0 = p0.x + (float)(i - 1) / (float)(numPoints - 1) * pedagogicalSize.x;
            float x1 = p0.x + (float)i / (float)(numPoints - 1) * pedagogicalSize.x;
            float y0 = juce::jlimit(p0.y, p1.y, centerY - demoOutput[i - 1] * scale + verticalOffset);
            float y1 = juce::jlimit(p0.y, p1.y, centerY - demoOutput[i] * scale + verticalOffset);
            drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colorOut, transformedThickness);
        }
        
        drawList->PopClipRect();
        
        // Label to indicate this is an example
        ImGui::SetCursorPos(ImVec2(4, 4));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.8f), "Example");
        
        // Invisible drag blocker
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##mathPedagogicalDrag", pedagogicalSize);
    }
    ImGui::EndChild();

    ImGui::PopItemWidth();
    ImGui::PopID();
}

void MathModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawParallelPins("In A", 0, "Out", 0);
    helpers.drawParallelPins("In B", 1, nullptr, -1);
    
    int busIdx, chanInBus;
    if (getParamRouting("operation_mod", busIdx, chanInBus))
        helpers.drawParallelPins("Op Mod", getChannelIndexInProcessBlockBuffer(true, busIdx, chanInBus), nullptr, -1);
}
#endif

float MathModuleProcessor::getLastValue() const
{
    return lastValue.load();
}

float MathModuleProcessor::getLastValueA() const
{
    return lastValueA.load();
}

float MathModuleProcessor::getLastValueB() const
{
    return lastValueB.load();
}

bool MathModuleProcessor::getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus) const
{
    if (paramId == "valueA") { outBusIndex = 0; outChannelIndexInBus = 0; return true; }
    if (paramId == "valueB") { outBusIndex = 1; outChannelIndexInBus = 0; return true; }
    if (paramId == "operation_mod") { outBusIndex = 2; outChannelIndexInBus = 0; return true; }
    return false;
}