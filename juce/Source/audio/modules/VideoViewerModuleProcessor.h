#pragma once

#include "ModuleProcessor.h"
#include <opencv2/core.hpp>
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Forward declarations
class VideoViewerWindow;

/**
 * VideoViewerModule - Displays video in a separate external window.
 *
 * This module receives video input and displays it in a resizable external
 * window that can be positioned on any monitor. The external window uses
 * a native title bar, making it compatible with OBS window capture.
 */
class VideoViewerModuleProcessor : public ModuleProcessor
{
public:
    VideoViewerModuleProcessor();
    ~VideoViewerModuleProcessor() override;

    const juce::String getName() const override { return "video_viewer"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    std::vector<DynamicPinInfo> getDynamicInputPins() const override;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded) override;
#endif

    // Called by VideoViewerComponent to get the latest frame
    juce::Image getLatestFrame();

    // Current video source ID (read from input pin)
    std::atomic<juce::uint32> currentSourceId{0};

    // Check if viewer window is open
    bool isViewerOpen() const { return windowOpen.load(); }

    // Called when window is closed
    void onWindowClosed();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Frame storage for GUI display
    juce::CriticalSection imageLock;
    juce::Image           latestFrameForGui;
    cv::Mat               lastFrameBgr;
    juce::CriticalSection frameLock;

    // External viewer window (owned, created on demand)
    VideoViewerWindow* viewerWindow = nullptr;
    std::atomic<bool>  windowOpen{false};

    // Helper to convert cv::Mat to juce::Image
    void updateGuiFrame(const cv::Mat& frame);

    // Open the viewer window
    void openViewerWindow();
};
