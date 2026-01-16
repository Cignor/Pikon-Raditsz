#include "VideoViewerModuleProcessor.h"
#include "../../video/VideoFrameManager.h"
#include <opencv2/imgproc.hpp>

#if defined(PRESET_CREATOR_UI)
#include <imgui.h>
#endif

// ==============================================================================
// === VIDEO VIEWER COMPONENT (renders video inside the window) =================
// ==============================================================================

class VideoViewerComponent : public juce::Component, private juce::Timer
{
public:
    VideoViewerComponent(VideoViewerModuleProcessor* processor) : owner(processor)
    {
        setSize(640, 480);
        startTimerHz(60); // 60fps refresh
    }

    ~VideoViewerComponent() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);

        if (owner != nullptr)
        {
            juce::Image frame = owner->getLatestFrame();
            if (frame.isValid())
            {
                // Scale to fit while maintaining aspect ratio
                float frameAspect = (float)frame.getWidth() / (float)frame.getHeight();
                float compAspect = (float)getWidth() / (float)getHeight();

                int drawW, drawH, drawX, drawY;
                if (frameAspect > compAspect)
                {
                    // Frame is wider - fit to width
                    drawW = getWidth();
                    drawH = (int)(getWidth() / frameAspect);
                    drawX = 0;
                    drawY = (getHeight() - drawH) / 2;
                }
                else
                {
                    // Frame is taller - fit to height
                    drawH = getHeight();
                    drawW = (int)(getHeight() * frameAspect);
                    drawX = (getWidth() - drawW) / 2;
                    drawY = 0;
                }

                // Draw checkerboard pattern for alpha transparency visualization
                const int          checkerSize = 16;
                const juce::Colour light(180, 180, 180);
                const juce::Colour dark(120, 120, 120);

                for (int cy = drawY; cy < drawY + drawH; cy += checkerSize)
                {
                    for (int cx = drawX; cx < drawX + drawW; cx += checkerSize)
                    {
                        bool isLight =
                            (((cx - drawX) / checkerSize) + ((cy - drawY) / checkerSize)) % 2 == 0;
                        g.setColour(isLight ? light : dark);
                        g.fillRect(
                            cx,
                            cy,
                            juce::jmin(checkerSize, drawX + drawW - cx),
                            juce::jmin(checkerSize, drawY + drawH - cy));
                    }
                }

                // Draw the video frame with alpha compositing
                g.drawImage(
                    frame, drawX, drawY, drawW, drawH, 0, 0, frame.getWidth(), frame.getHeight());
            }
            else
            {
                g.setColour(juce::Colours::grey);
                g.setFont(20.0f);
                g.drawText("No Video Signal", getLocalBounds(), juce::Justification::centred);
            }
        }
    }

    void timerCallback() override { repaint(); }

private:
    VideoViewerModuleProcessor* owner = nullptr;
};

// ==============================================================================
// === VIDEO VIEWER WINDOW (external resizable window) ==========================
// ==============================================================================

class VideoViewerWindow : public juce::DocumentWindow
{
public:
    VideoViewerWindow(VideoViewerModuleProcessor* processor)
        : DocumentWindow("Video Viewer", juce::Colours::darkgrey, juce::DocumentWindow::allButtons),
          owner(processor)
    {
        setContentOwned(new VideoViewerComponent(processor), true);
        setResizable(true, true);
        setResizeLimits(320, 240, 4096, 4096);
        setUsingNativeTitleBar(true);
        centreWithSize(800, 600);
        setVisible(true);

        juce::Logger::writeToLog("[VideoViewer] Window opened");
    }

    ~VideoViewerWindow() override { juce::Logger::writeToLog("[VideoViewer] Window destroyed"); }

    void closeButtonPressed() override
    {
        if (owner != nullptr)
            owner->onWindowClosed();

        // Delete self - the window manages its own lifetime
        delete this;
    }

private:
    VideoViewerModuleProcessor* owner = nullptr;
};

// ==============================================================================
// === VIDEO VIEWER MODULE PROCESSOR ============================================
// ==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout VideoViewerModuleProcessor::
    createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // No audio parameters needed for this module
    // The window state could be saved but we keep it simple for now

    return {params.begin(), params.end()};
}

VideoViewerModuleProcessor::VideoViewerModuleProcessor()
    : ModuleProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::mono(), true)
              .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "VideoViewerParams", createParameterLayout())
{
    juce::Logger::writeToLog("[VideoViewer] Module created");
}

VideoViewerModuleProcessor::~VideoViewerModuleProcessor()
{
    // Close the window if it's still open
    if (viewerWindow != nullptr)
    {
        juce::MessageManager::callAsync([window = viewerWindow]() { delete window; });
        viewerWindow = nullptr;
    }
    juce::Logger::writeToLog("[VideoViewer] Module destroyed");
}

void VideoViewerModuleProcessor::prepareToPlay(double /*sampleRate*/, int /*samplesPerBlock*/)
{
    // Nothing to prepare for video viewing
}

void VideoViewerModuleProcessor::releaseResources()
{
    // Nothing to release
}

void VideoViewerModuleProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& /*midi*/)
{
    // Read video source ID from audio input (encoded as float)
    if (buffer.getNumSamples() > 0 && buffer.getNumChannels() > 0)
    {
        float        sourceIdFloat = buffer.getSample(0, 0);
        juce::uint32 sourceId = static_cast<juce::uint32>(sourceIdFloat);
        currentSourceId.store(sourceId);

        // Always fetch and store the frame for canvas preview (not just when window is open)
        if (sourceId != 0)
        {
            cv::Mat frame = VideoFrameManager::getInstance().getFrame(sourceId);
            if (!frame.empty())
            {
                updateGuiFrame(frame);
            }
        }
    }

    // Clear audio buffer - we don't produce audio
    buffer.clear();
}

std::vector<DynamicPinInfo> VideoViewerModuleProcessor::getDynamicInputPins() const
{
    // Constructor arguments must be: name, channel, type
    // DynamicPinInfo(const juce::String& n, int channel, PinDataType type)
    return {DynamicPinInfo{"Video In", 0, PinDataType::Video}};
}

void VideoViewerModuleProcessor::updateGuiFrame(const cv::Mat& frame)
{
    if (frame.empty())
        return;

    // Convert to BGRA format (handles 1, 3, and 4 channel inputs safely)
    cv::Mat bgraFrame;
    if (frame.channels() == 1)
    {
        // Handle 1-channel Grayscale (Masks/Mattes) - same as VideoCompositorModule
        // Convert grayscale to BGR first, then to BGRA
        cv::Mat bgrFrame;
        cv::cvtColor(frame, bgrFrame, cv::COLOR_GRAY2BGR);
        cv::cvtColor(bgrFrame, bgraFrame, cv::COLOR_BGR2BGRA);
    }
    else if (frame.channels() == 3)
    {
        // BGR input - convert to BGRA (opaque)
        cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);
    }
    else if (frame.channels() == 4)
    {
        // Already BGRA - use directly
        bgraFrame = frame.clone();
    }
    else
    {
        // Unsupported channel count - skip
        return;
    }

    const juce::ScopedLock sl(imageLock);

    // Create or resize the image if needed
    if (latestFrameForGui.isNull() || latestFrameForGui.getWidth() != bgraFrame.cols ||
        latestFrameForGui.getHeight() != bgraFrame.rows)
    {
        latestFrameForGui = juce::Image(juce::Image::ARGB, bgraFrame.cols, bgraFrame.rows, true);
    }

    // Use memcpy for efficient transfer (BGRA->ARGB is native on Windows/JUCE)
    juce::Image::BitmapData destData(latestFrameForGui, juce::Image::BitmapData::writeOnly);
    memcpy(destData.data, bgraFrame.data, bgraFrame.total() * bgraFrame.elemSize());
}

juce::Image VideoViewerModuleProcessor::getLatestFrame()
{
    const juce::ScopedLock sl(imageLock);
    return latestFrameForGui;
}

void VideoViewerModuleProcessor::onWindowClosed()
{
    windowOpen.store(false);
    viewerWindow = nullptr;
    juce::Logger::writeToLog("[VideoViewer] Window closed by user");
}

void VideoViewerModuleProcessor::openViewerWindow()
{
    if (windowOpen.load())
        return; // Already open

    windowOpen.store(true);

    // Capture this pointer for async call
    VideoViewerModuleProcessor* self = this;
    juce::MessageManager::callAsync([self]() {
        if (self->windowOpen.load())
        {
            self->viewerWindow = new VideoViewerWindow(self);
        }
    });
}

#if defined(PRESET_CREATOR_UI)
void VideoViewerModuleProcessor::drawParametersInNode(
    float itemWidth,
    const std::function<bool(const juce::String& paramId)>& /*isParamModulated*/,
    const std::function<void()>& /*onModificationEnded*/)
{
    // === CANVAS VIDEO PREVIEW ===
    // Show video status indicator
    juce::uint32 srcId = currentSourceId.load();
    if (srcId != 0)
    {
        // Video is connected - show info
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Video Signal Active");
        ImGui::TextDisabled("Source ID: %u", srcId);
    }
    else
    {
        // No video - show placeholder
        ImGui::TextDisabled("No Video Signal");
    }

    // === EXTERNAL WINDOW CONTROLS ===
    if (windowOpen.load())
    {
        ImGui::BeginDisabled();
        ImGui::Button("Viewer Open", ImVec2(itemWidth, 0));
        ImGui::EndDisabled();

        // Show close button
        if (ImGui::Button("Close Viewer", ImVec2(itemWidth, 0)))
        {
            if (viewerWindow != nullptr)
            {
                juce::MessageManager::callAsync([window = viewerWindow]() {
                    if (window != nullptr)
                        window->closeButtonPressed();
                });
            }
        }
    }
    else
    {
        if (ImGui::Button("Open External Viewer", ImVec2(itemWidth, 0)))
        {
            openViewerWindow();
        }
    }
}

#endif
