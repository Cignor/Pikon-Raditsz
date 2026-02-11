#include "RecordModuleProcessor.h"

#if defined(PRESET_CREATOR_UI)
#include "../../preset_creator/theme/ThemeManager.h"
#endif
#include "../graph/ModularSynthProcessor.h"

// --- WriterThread with Corrected File Logic ---

RecordModuleProcessor::WriterThread::WriterThread(RecordModuleProcessor& o)
    : juce::Thread("Audio Recorder Thread"), owner(o)
{
}

RecordModuleProcessor::WriterThread::~WriterThread() { stopThread(5000); }

// This function is now private and runs ONLY on the background thread
bool RecordModuleProcessor::WriterThread::doStartRecording()
{
    const juce::ScopedLock lock(writerLock);
    if (writer != nullptr)
        return false;

    juce::File file;
    {
        const juce::ScopedLock startLock(owner.startRequestLock);
        file = owner.pendingFileToRecord;
        owner.startRequestPending = false;
    }

    // --- CRITICAL FIX #1: Auto-increment logic now happens AFTER extension is added ---
    juce::String chosenExtension = "." + owner.formatParam->getCurrentChoiceName().toLowerCase();
    juce::File   fileWithExt = file.withFileExtension(chosenExtension);

    juce::File fileToUse = fileWithExt;
    if (fileToUse.existsAsFile())
    {
        int          counter = 1;
        juce::String originalName = fileWithExt.getFileNameWithoutExtension();
        while (fileToUse.existsAsFile())
        {
            juce::String counterStr = juce::String(counter++).paddedLeft('0', 3);
            fileToUse =
                fileWithExt.getSiblingFile(originalName + "_" + counterStr + chosenExtension);
        }
    }
    // --- END OF FIX ---

    auto* format = owner.formatManager.findFormatForFileExtension(fileToUse.getFileExtension());
    if (format == nullptr)
        return false;

    auto fileStream = std::make_unique<juce::FileOutputStream>(fileToUse);
    if (!fileStream->openedOk())
        return false;

    writer.reset(format->createWriterFor(
        fileStream.release(),
        owner.getSampleRate(),
        2,  // Stereo
        24, // Bit depth
        {},
        0));
    if (writer != nullptr)
    {
        owner.currentFileRecording = fileToUse.getFullPathName();
        owner.totalSamplesRecorded = 0;
        owner.waveformData.clear();
        owner.abstractFifo.reset();
        owner.isRecording = true;
        owner.isPaused = false;
        return true;
    }
    return false;
}

void RecordModuleProcessor::WriterThread::stopRecording()
{
    owner.isRecording = false;
    notify();
}

void RecordModuleProcessor::WriterThread::run()
{
    while (!threadShouldExit())
    {
        if (owner.startRequestPending.load())
        {
            doStartRecording();
        }

        bool hasAudioToProcess = owner.abstractFifo.getNumReady() > 0;
        bool shouldFinalize =
            !owner.isRecording.load() && !hasAudioToProcess && (writer != nullptr);

        if (hasAudioToProcess)
        {
            const juce::ScopedLock lock(writerLock);
            if (writer != nullptr)
            {
                int samplesAvailable = owner.abstractFifo.getNumReady();
                if (samplesAvailable > 0)
                {
                    juce::AudioBuffer<float> tempBuffer(2, samplesAvailable);
                    auto                     read = owner.abstractFifo.read(samplesAvailable);
                    tempBuffer.copyFrom(
                        0, 0, owner.fifoBuffer, 0, read.startIndex1, read.blockSize1);
                    tempBuffer.copyFrom(
                        1, 0, owner.fifoBuffer, 0, read.startIndex1, read.blockSize1);
                    if (read.blockSize2 > 0)
                    {
                        tempBuffer.copyFrom(
                            0,
                            read.blockSize1,
                            owner.fifoBuffer,
                            0,
                            read.startIndex2,
                            read.blockSize2);
                        tempBuffer.copyFrom(
                            1,
                            read.blockSize1,
                            owner.fifoBuffer,
                            0,
                            read.startIndex2,
                            read.blockSize2);
                    }
                    writer->writeFromAudioSampleBuffer(tempBuffer, 0, samplesAvailable);
                }
            }
        }
        else if (shouldFinalize)
        {
            const juce::ScopedLock lock(writerLock);
            if (writer != nullptr)
            {
                writer.reset();
                owner.currentFileRecording = "";
            }
            wait(-1);
        }
        else
        {
            wait(50);
        }
    }

    if (writer != nullptr)
        writer.reset();
}

// --- RecordModuleProcessor Implementation ---

juce::AudioProcessorValueTreeState::ParameterLayout RecordModuleProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(
        std::make_unique<juce::AudioParameterChoice>(
            "format", "Format", juce::StringArray{"WAV", "AIFF", "FLAC"}, 0));
    params.push_back(
        std::make_unique<juce::AudioParameterFloat>(
            "ceiling", "Ceiling", juce::NormalisableRange<float>(-24.0f, 0.0f, 0.1f), -0.1f));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("use_agc", "Auto Gain Control", false));
    params.push_back(
        std::make_unique<juce::AudioParameterBool>("anti_clip", "Anti-Clipping", false));
    return {params.begin(), params.end()};
}

RecordModuleProcessor::RecordModuleProcessor()
    : ModuleProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "RecordParams", createParameterLayout()), waveformFifoBuffer(4096),
      writerThread(*this)
{
    formatManager.registerBasicFormats();
    formatManager.registerFormat(new juce::FlacAudioFormat(), true);
    formatParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("format"));
    ceilingParam = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ceiling"));
    agcParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("use_agc"));
    antiClipParam = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("anti_clip"));
    writerThread.startThread();

#if defined(PRESET_CREATOR_UI)
    // Default to exe/record/ directory, create if it doesn't exist
    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto recordDir = exeDir.getChildFile("record");
    if (recordDir.exists() && recordDir.isDirectory())
        saveDirectory = recordDir;
    else if (recordDir.createDirectory())
        saveDirectory = recordDir;
    else
        saveDirectory = juce::File::getSpecialLocation(
            juce::File::userMusicDirectory); // Fallback to user music directory
#endif

    waveformFifo.setTotalSize(4096);
}

RecordModuleProcessor::~RecordModuleProcessor()
{
    if (isRecording.load())
        writerThread.stopRecording();
}

void RecordModuleProcessor::releaseResources()
{
    if (isRecording.load())
        writerThread.stopRecording();
}

void RecordModuleProcessor::prepareToPlay(double sampleRate, int)
{
    int fifoLen = (int)(sampleRate * 10.0);
    fifoBuffer.setSize(1, fifoLen, false, true, false);
    abstractFifo.setTotalSize(fifoLen);
    abstractFifo.reset();
}

void RecordModuleProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!isRecording.load() || isPaused.load())
        return;

    auto      inBus = getBusBuffer(buffer, true, 0);
    const int numSamples = inBus.getNumSamples();

    workBuffer.setSize(1, numSamples, false, false, true);
    workBuffer.copyFrom(0, 0, inBus, 0, 0, numSamples);
    if (inBus.getNumChannels() > 1)
    {
        workBuffer.addFrom(0, 0, inBus, 1, 0, numSamples);
        workBuffer.applyGain(0.5f);
    }

    // AGC Logic
    if (agcParam->get())
    {
        // Simple peak envelope follower
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float absSample = std::abs(workBuffer.getSample(0, i));
            if (absSample > peak)
                peak = absSample;
        }

        // Smooth envelope
        float attack = 0.9995f; // Slow attack
        float release = 0.99f;  // Faster release

        if (peak > envelope)
            envelope = peak; // Instant attack for measurement
        else
            envelope = envelope * release;

        // Target level -6dB (0.5)
        float target = 0.5f;
        float error = target - envelope;

        // Adjust gain
        // If too quiet (error > 0), boost slowly
        // If too loud (error < 0), reduce fast
        float adjustment = error > 0 ? 1.0001f : 0.995f;

        if (envelope > target)
            currentAgcGain *= 0.995f; // Fast reduction
        else if (envelope < target)
            currentAgcGain *= 1.0002f; // Very slow boost

        // Clamp gain (0dB to +24dB)
        currentAgcGain = juce::jlimit(1.0f, 15.8f, currentAgcGain);

        workBuffer.applyGain(currentAgcGain);
    }
    else
    {
        currentAgcGain = 1.0f;
        envelope = 0.0f;
    }

    // Clipping / Limiting
    const float ceilingDb = ceilingParam->get();
    const bool  useAntiClip = antiClipParam->get();
    const float ceilingLin = juce::Decibels::decibelsToGain(ceilingDb);
    float*      data = workBuffer.getWritePointer(0);

    for (int i = 0; i < numSamples; ++i)
    {
        if (useAntiClip)
        {
            // Soft clipper (tanh)
            // x = input / ceiling
            // out = tanh(x) * ceiling
            if (ceilingLin > 0.0001f)
                data[i] = std::tanh(data[i] / ceilingLin) * ceilingLin;
        }
        else
        {
            // Hard clip
            data[i] = juce::jlimit(-ceilingLin, ceilingLin, data[i]);
        }
    }

    if (abstractFifo.getFreeSpace() >= numSamples)
    {
        auto write = abstractFifo.write(numSamples);
        if (write.blockSize1 > 0)
            fifoBuffer.copyFrom(0, write.startIndex1, workBuffer, 0, 0, write.blockSize1);
        if (write.blockSize2 > 0)
            fifoBuffer.copyFrom(
                0, write.startIndex2, workBuffer, 0, write.blockSize1, write.blockSize2);
        writerThread.notify();
    }

    if (waveformFifo.getFreeSpace() >= 1)
    {
        float peak = workBuffer.getMagnitude(0, numSamples);
        auto  write = waveformFifo.write(1);
        waveformFifoBuffer[write.startIndex1] = peak;
    }
    totalSamplesRecorded += numSamples;
}

juce::String RecordModuleProcessor::getAudioInputLabel(int channel) const
{
    if (channel == 0)
        return "In L";
    if (channel == 1)
        return "In R";
    return {};
}

void RecordModuleProcessor::programmaticStartRecording()
{
#if defined(PRESET_CREATOR_UI)
    // This function is called externally, e.g., by a global start button.
    // It uses the path and filename currently set in the node's UI.
    if (saveDirectory.exists())
    {
        juce::String filenameToSave = autoGeneratedPrefix + juce::String(userSuffixBuffer);
        if (filenameToSave.isEmpty())
            filenameToSave = "recording";

        juce::File fileToSave = saveDirectory.getChildFile(filenameToSave);

        // Use the async request method
        requestStartRecording(fileToSave);
    }
#endif
}

void RecordModuleProcessor::programmaticStopRecording()
{
    if (isRecording.load())
    {
        writerThread.stopRecording();
    }
}

#if defined(PRESET_CREATOR_UI)

void RecordModuleProcessor::requestStartRecording(const juce::File& file)
{
    const juce::ScopedLock lock(startRequestLock);
    pendingFileToRecord = file;
    startRequestPending = true;
    writerThread.notify();
}

// --- NEW FUNCTION: Generates filename with source name passed directly ---
void RecordModuleProcessor::updateSuggestedFilename(const juce::String& sourceName)
{
    juce::String timeString = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");

    if (sourceName.isEmpty())
    {
        // No source provided, mark as unconnected
        autoGeneratedPrefix = timeString + "+Unconnected";
    }
    else
    {
        // Source name provided, use it
        juce::String srcName = sourceName.removeCharacters(" ");
        autoGeneratedPrefix = timeString + "+" + srcName;
    }
}

void RecordModuleProcessor::setPropertiesFile(juce::PropertiesFile* props)
{
    propertiesFile = props;
    if (propertiesFile != nullptr)
    {
        // On initialization, load the last path from settings
        juce::String lastPath = propertiesFile->getValue("lastRecorderPath");
        if (juce::File(lastPath).isDirectory())
        {
            saveDirectory = juce::File(lastPath);
        }
    }
}

void RecordModuleProcessor::drawParametersInNode(
    float /*itemWidth*/,
    const std::function<bool(const juce::String&)>&,
    const std::function<void()>&)
{
    // Use a wider, fixed width for this node to ensure everything fits
    const float nodeWidth = 350.0f;
    ImGui::PushItemWidth(nodeWidth);
    const auto& theme = ThemeManager::getInstance().getCurrentTheme();

    // Ceiling Control
    float ceilingVal = ceilingParam->get();
    ImGui::Text("Clipping Ceiling");
    if (ImGui::SliderFloat("##ceiling", &ceilingVal, -24.0f, 0.0f, "%.1f dB"))
    {
        *ceilingParam = ceilingVal;
    }

    // Checkboxes
    bool agc = agcParam->get();
    if (ImGui::Checkbox("Auto Gain", &agc))
        *agcParam = agc;

    ImGui::SameLine();

    bool antiClip = antiClipParam->get();
    if (ImGui::Checkbox("Anti-Clip", &antiClip))
        *antiClipParam = antiClip;

    if (isRecording.load() || !currentFileRecording.isEmpty())
    {
        if (isPaused.load())
            ThemeText("Status: Paused", theme.text.warning);
        else
            ImGui::Text("Status: Recording...");

        double elapsed = (double)totalSamplesRecorded.load() / getSampleRate();
        ImGui::Text("Time: %.2fs", elapsed);
        ImGui::TextWrapped("File: %s", juce::File(currentFileRecording).getFileName().toRawUTF8());

        int available = waveformFifo.getNumReady();
        if (available > 0)
        {
            auto read = waveformFifo.read(available);
            for (int i = 0; i < read.blockSize1; ++i)
                waveformData.push_back(waveformFifoBuffer[read.startIndex1 + i]);
            if (read.blockSize2 > 0)
                for (int i = 0; i < read.blockSize2; ++i)
                    waveformData.push_back(waveformFifoBuffer[read.startIndex2 + i]);

            const int max_display_points = 2000;
            if (waveformData.size() > max_display_points)
                waveformData.erase(
                    waveformData.begin(),
                    waveformData.begin() + (waveformData.size() - max_display_points));
        }

        ImVec2      canvas_p0 = ImGui::GetCursorScreenPos();
        ImVec2      canvas_sz = ImVec2(nodeWidth, 60.0f);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImU32       bgCol = theme.canvas.canvas_background == 0 ? IM_COL32(30, 30, 30, 255)
                                                                : theme.canvas.canvas_background;
        draw_list->AddRectFilled(
            canvas_p0, ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y), bgCol);
        if (!waveformData.empty())
        {
            float max_val = 1.0f;
            for (float v : waveformData)
            {
                if (v > max_val)
                    max_val = v;
            }
            for (size_t i = 0; i + 1 < waveformData.size(); ++i)
            {
                ImVec2 p1 = ImVec2(
                    canvas_p0.x + ((float)i / waveformData.size()) * canvas_sz.x,
                    canvas_p0.y + (1.0f - (waveformData[i] / max_val)) * canvas_sz.y);
                ImVec2 p2 = ImVec2(
                    canvas_p0.x + ((float)(i + 1) / waveformData.size()) * canvas_sz.x,
                    canvas_p0.y + (1.0f - (waveformData[i + 1] / max_val)) * canvas_sz.y);
                draw_list->AddLine(
                    p1,
                    p2,
                    theme.modules.scope_plot_fg != 0 ? theme.modules.scope_plot_fg
                                                     : IM_COL32(120, 255, 120, 255));
            }
            if (max_val > 1.0f)
            {
                float clip_y = canvas_p0.y + (1.0f - (1.0f / max_val)) * canvas_sz.y;
                draw_list->AddLine(
                    ImVec2(canvas_p0.x, clip_y),
                    ImVec2(canvas_p0.x + canvas_sz.x, clip_y),
                    ImGui::ColorConvertFloat4ToU32(theme.text.error),
                    1.5f);
            }
        }
        ImGui::Dummy(canvas_sz);

        if (ImGui::Button("Stop", ImVec2(nodeWidth, 0)))
        {
            writerThread.stopRecording();
        }
    }
    else // --- NEW, SIMPLIFIED IDLE STATE UI ---
    {
        // Load the last saved directory if available, but prefer exe/record/ if it doesn't exist
        auto exeDir =
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        auto defaultRecordDir = exeDir.getChildFile("record");

        // If saveDirectory is still at default location, try to load from properties or use
        // exe/record/
        if (propertiesFile)
        {
            juce::String lastPath = propertiesFile->getValue("lastRecorderPath");
            if (lastPath.isNotEmpty() && juce::File(lastPath).isDirectory())
            {
                saveDirectory = juce::File(lastPath);
            }
            else if (defaultRecordDir.exists() && defaultRecordDir.isDirectory())
            {
                saveDirectory = defaultRecordDir;
            }
        }
        else if (defaultRecordDir.exists() && defaultRecordDir.isDirectory())
        {
            saveDirectory = defaultRecordDir;
        }

        // This layout provides more space as requested
        ImGui::Text("Save Location:");
        ImGui::TextWrapped("%s", saveDirectory.getFullPathName().toRawUTF8());
        if (ImGui::Button("Browse...", ImVec2(nodeWidth, 0)))
        {
            fileChooser =
                std::make_unique<juce::FileChooser>("Choose Save Directory", saveDirectory);
            fileChooser->launchAsync(
                juce::FileBrowserComponent::openMode |
                    juce::FileBrowserComponent::canSelectDirectories,
                [this](const juce::FileChooser& fc) {
                    auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        saveDirectory = dir;
                        // Save the path for next time
                        if (propertiesFile)
                            propertiesFile->setValue("lastRecorderPath", dir.getFullPathName());
                    }
                });
        }

        // Filename Prefix (read-only) + Suffix (editable)
        ImGui::Text("Filename Prefix:");
        ImGui::TextWrapped("%s", autoGeneratedPrefix.toRawUTF8());

        ImGui::InputText("Suffix", userSuffixBuffer, sizeof(userSuffixBuffer));

        int formatIdx = formatParam->getIndex();
        if (ImGui::Combo("Format", &formatIdx, "WAV\0AIFF\0FLAC\0\0"))
        {
            *formatParam = formatIdx;
        }

        // Full filename preview
        juce::String chosenExtension = "." + formatParam->getCurrentChoiceName().toLowerCase();
        juce::String finalName =
            autoGeneratedPrefix + juce::String(userSuffixBuffer) + chosenExtension;
        ImGui::Text("Final Name Preview:");
        ImGui::TextWrapped("%s", finalName.toRawUTF8());

        if (ImGui::Button("Record", ImVec2(nodeWidth, 0)))
        {
            juce::String filenameToSave = autoGeneratedPrefix + juce::String(userSuffixBuffer);
            if (filenameToSave.isEmpty())
                filenameToSave = "recording";

            juce::File fileToSave = saveDirectory.getChildFile(filenameToSave);
            requestStartRecording(fileToSave);
        }
    }

    ImGui::PopItemWidth();
}

void RecordModuleProcessor::drawIoPins(const NodePinHelpers& helpers)
{
    helpers.drawAudioInputPin("In L", 0);
    helpers.drawAudioInputPin("In R", 1);
}

#endif
