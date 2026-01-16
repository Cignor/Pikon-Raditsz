#pragma once

#include "ModuleProcessor.h"
#include "../dsp/TimePitchProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>
// Forward declare ImGui types so Collider can include this header without ImGui
struct ImVec2;

// --- Timing Data Structures for Word and Phoneme Control ---
struct PhonemeTiming
{
    juce::String phoneme;          // The phoneme symbol (e.g., "AH", "T", "S")
    double       startTimeSeconds; // Start time in seconds
    double       endTimeSeconds;   // End time in seconds
    double       durationSeconds;  // Duration in seconds (calculated)

    PhonemeTiming() = default;
    PhonemeTiming(const juce::String& p, double start, double end)
        : phoneme(p), startTimeSeconds(start), endTimeSeconds(end), durationSeconds(end - start)
    {
    }
};

struct WordTiming
{
    juce::String               word;             // The word text (e.g., "Hello", "world")
    double                     startTimeSeconds; // Start time in seconds
    double                     endTimeSeconds;   // End time in seconds
    double                     durationSeconds;  // Duration in seconds (calculated)
    std::vector<PhonemeTiming> phonemes;         // Phonemes within this word

    WordTiming() = default;
    WordTiming(const juce::String& w, double start, double end)
        : word(w), startTimeSeconds(start), endTimeSeconds(end), durationSeconds(end - start)
    {
    }
};

class TTSPerformerModuleProcessor : public ModuleProcessor
{
public:
    TTSPerformerModuleProcessor();
    ~TTSPerformerModuleProcessor() override;

    const juce::String getName() const override { return "tts_performer"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}

    void setTimingInfo(const TransportState& state) override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorValueTreeState& getAPVTS() override { return apvts; }

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Extra state for preset saving (selected clip persistence)
    juce::ValueTree getExtraStateTree() const override;
    void            setExtraStateTree(const juce::ValueTree& vt) override;

    void startSynthesis(const juce::String& text);
    void cancelSynthesis();

    // --- Timing Data Access Methods ---
    const std::vector<WordTiming>& getLastSynthesisTimings() const { return lastSynthesisTimings; }
    bool                           isWordActiveAtTime(double timeInSeconds) const;
    const WordTiming*              getCurrentWordAtTime(double timeInSeconds) const;
    const PhonemeTiming*           getCurrentPhonemeAtTime(double timeInSeconds) const;

#if defined(PRESET_CREATOR_UI)
    void drawParametersInNode(
        float                                                   itemWidth,
        const std::function<bool(const juce::String& paramId)>& isParamModulated,
        const std::function<void()>&                            onModificationEnded,
        const NodePinHelpers*                                   pinHelpers = nullptr) override;
    void drawIoPins(const NodePinHelpers& helpers) override;
#endif

    bool getParamRouting(const juce::String& paramId, int& outBusIndex, int& outChannelIndexInBus)
        const override;
    juce::String getAudioInputLabel(int channel) const override;
    juce::String getAudioOutputLabel(int channel) const override;

    // --- Phase 3: Sequencer Helper Methods ---
    void   advanceSequencerStep();
    void   resetSequencer();
    int    getSequencerCurrentIndex() const;
    double getSequencerCurrentDuration() const;
    void   handleLoopMode();

    // Cross-app helpers (exposed for both apps)
    juce::File                     resolveModelsBaseDir() const;
    juce::File                     resolveSelectedModelFile() const;
    bool                           loadVoicesFromMapFile(const juce::File& mapFile);
    void                           normalizeModelSelection();
    const std::vector<WordTiming>& getActiveTimings() const;
    int                            findFirstWordIndexAtOrAfter(double timeSec) const;
    int                            findLastWordIndexAtOrBefore(double timeSec) const;
    int                            findWordIndexForTime(float timeSeconds) const;
    void                           clampWordIndexToTrim();

    // --- Voice Manifest and Status Checking (for download system) ---
    enum class VoiceStatus
    {
        NotInstalled,
        Installed,
        Partial,
        Error
    };

    struct VoiceEntry
    {
        juce::String name;       // e.g., "en_US-lessac-medium"
        juce::String language;   // e.g., "English (US)"
        juce::String accent;     // e.g., "General American"
        juce::String gender;     // e.g., "Female"
        juce::String quality;    // e.g., "Medium"
        bool         isIncluded; // Is this one of the 3-4 default voices in distribution?

        VoiceEntry() = default;
        VoiceEntry(
            const juce::String& n,
            const juce::String& lang,
            const juce::String& acc,
            const juce::String& gen,
            const juce::String& qual,
            bool                included = false)
            : name(n), language(lang), accent(acc), gender(gen), quality(qual), isIncluded(included)
        {
        }
    };

    static std::vector<VoiceEntry>      getAllAvailableVoices();
    VoiceStatus                         checkVoiceStatus(const juce::String& voiceName) const;
    std::map<juce::String, VoiceStatus> checkAllVoiceStatuses() const;

    // --- Phase 5: Waveform Visualization Methods (UI-only) ---
#if defined(PRESET_CREATOR_UI)
    void drawWaveform(void* drawList, const ImVec2& pos, const ImVec2& size);
    void drawWordBoundaries(
        void*         drawList,
        const ImVec2& pos,
        const ImVec2& size,
        int           numSamples,
        float         centerY);
    void drawPhonemeBoundaries(
        void*             drawList,
        const ImVec2&     pos,
        const ImVec2&     size,
        const WordTiming& word,
        float             wordStartX,
        float             wordWidth,
        float             centerY);
    void drawPlayheadIndicator(
        void*         drawList,
        const ImVec2& pos,
        const ImVec2& size,
        int           numSamples,
        float         centerY);
    void highlightCurrentWord(
        void*         drawList,
        const ImVec2& pos,
        const ImVec2& size,
        float         centerY,
        double        currentTimeSeconds);
    bool handleWaveformInteraction(const ImVec2& pos, const ImVec2& size, int numSamples);
#endif

    // Cross-app helpers (declared once)

    // --- Audio-based word detection for precise timing ---
    std::vector<WordTiming> detectWordsFromAudio(
        const juce::AudioBuffer<float>& audio,
        double                          sampleRate);

private:
    // Centralised channel map for all inputs (single input bus)
    struct ChannelMap
    {
        static constexpr int CH_RATE = 0;
        static constexpr int CH_GATE = 1;
        static constexpr int CH_TRIGGER = 2;
        static constexpr int CH_RESET = 3;
        static constexpr int CH_TRIM_START = 4;
        static constexpr int CH_TRIM_END = 5;
        static constexpr int CH_SPEED = 6;
        static constexpr int CH_PITCH = 7;
        static constexpr int CH_WORD_BASE = 8; // Word 1..16 at 8..23
        static constexpr int wordChannel(int wordIndex /*0-based*/)
        {
            return CH_WORD_BASE + wordIndex;
        }
    };
    // --- Persistent Clip Model ---
    struct TTSClip
    {
        juce::String                          clipId;   // hash key (text+voice+params)
        juce::String                          name;     // user/display name (default: trimmed text)
        juce::String                          text;     // original text
        juce::String                          modelKey; // voice/model identifier
        juce::AudioBuffer<float>              audio;    // mono audio
        double                                sampleRate{48000.0};
        double                                durationSeconds{0.0};
        std::vector<WordTiming>               timings;
        std::chrono::steady_clock::time_point lastUsed{std::chrono::steady_clock::now()};
    };

    // Clip cache (memory) and selection
    std::map<juce::String, std::shared_ptr<TTSClip>> clipCache;
    juce::CriticalSection                            clipCacheLock;
    std::shared_ptr<TTSClip>                         selectedClip;
    int                                              clipCacheMax{16};
    juce::String                                     computeClipKey(const juce::String& text) const;
    void                     addClipToCache(const std::shared_ptr<TTSClip>& clip);
    std::shared_ptr<TTSClip> findClipInCache(const juce::String& key) const;
    void                     selectClipByKey(const juce::String& key);
    void                     loadClipsFromDisk();
    bool                     clipsLoadedFromDisk{false};
    // Disk persistence helpers
    juce::File   getClipsRootDir() const;
    juce::String sanitizeForDir(const juce::String& text) const;
    void         persistClipToDisk(
                const juce::String&             text,
                const juce::File&               modelFile,
                const juce::AudioBuffer<float>& audioBuffer,
                const juce::String&             jsonContent);
    // UI helpers for clips
    void drawClipsPanel(float itemWidth);
    // Timeline interaction state (waveform handles)
    bool draggingTrimStart{false};
    bool draggingTrimEnd{false};
    bool draggingScrub{false};
    // Clip actions
    void playSelectedClipFromTrim();
    void stopPlayback();
    void forceStop() override; // Force stop (used after patch load)
    bool deleteSelectedClipFromDisk();
    bool renameSelectedClipOnDisk(const juce::String& newName);

    class SynthesisThread : public juce::Thread
    {
    public:
        SynthesisThread(TTSPerformerModuleProcessor& owner);
        ~SynthesisThread() override;
        void run() override;

        // --- Phase 4: Cache Management Methods ---
        void clearVoiceCache();
        void setMaxCachedVoices(int maxVoices);
        void updateMaxCachedVoicesFromParameter();
        int  getCacheSize() const;
        bool isVoiceCached(const juce::String& modelPath) const;

    private:
        TTSPerformerModuleProcessor& owner;
        juce::File                   piperExecutable;

    public:
        juce::File currentModelFile;

        // --- Phase 4: Voice Cache System ---
        struct CachedVoice
        {
            juce::String                          modelPath;
            juce::String                          configPath;
            std::chrono::steady_clock::time_point lastUsed;
            bool                                  isValid;

            CachedVoice() : lastUsed(std::chrono::steady_clock::now()), isValid(false) {}
            CachedVoice(const juce::String& model, const juce::String& config)
                : modelPath(model), configPath(config), lastUsed(std::chrono::steady_clock::now()),
                  isValid(true)
            {
            }
        };

        std::map<juce::String, CachedVoice> voiceCache;
        juce::CriticalSection               cacheLock;
        int                                 maxCachedVoices{3};

        // --- Phase 4: Cache Helper Methods ---
        void         addVoiceToCache(const juce::String& modelPath, const juce::String& configPath);
        void         removeOldestVoice();
        juce::String getCacheKey(const juce::String& modelPath) const;
    };

    // --- Audio Storage and Playback ---
    juce::AudioBuffer<float> bakedAudioBuffer;
    bool                     isPlaying{false};
    double                   phase{0.0};           // Phase accumulator for rate-based auto-advance
    bool                     lastTrigHigh{false};  // Trigger edge detection
    bool                     lastResetHigh{false}; // Reset edge detection
    bool                     lastRandomizeTriggerHigh{false}; // Randomize trigger edge detection
    std::array<int, 16>      wordTriggerPending{}; // Per-word trigger pulse counters (1ms)
    std::array<bool, 16>     lastWordTrigHigh{};   // Per-word trigger edge detection

    // --- Word and Phoneme Timing Data ---
    std::vector<WordTiming> lastSynthesisTimings; // Protected by audioBufferLock

    // --- Phase 3: Sequencer State ---
    int    currentWordIndex{0};         // Current word being played in sequencer mode
    int    currentPhonemeIndex{0};      // Current phoneme being played in sequencer mode
    double sequencerStartTime{0.0};     // When the current word/phoneme started playing
    bool   sequencerActive{false};      // Whether sequencer mode is currently active
    float  lastStepTriggerValue{0.0f};  // For detecting step trigger edges
    float  lastResetTriggerValue{0.0f}; // For detecting reset trigger edges
    bool   lastPlaybackState{false};    // For EOP gate detection
    bool   lastGateState{false};        // For detecting gate input edges
    int    eopPulseSamplesRemaining{0}; // For short EOP gate pulses
    int    lastClipIndexValue{-1};      // For detecting clip index param changes

    // --- Text Input (Thread-Safe Queue) ---
    juce::AbstractFifo        textFifo;
    std::vector<juce::String> textFifoBuffer;
    juce::CriticalSection     textBufferLock;
    juce::CriticalSection     messageLock;
    juce::CriticalSection     audioBufferLock;

    // --- Status Tracking ---
    enum class Status
    {
        Idle,
        Synthesizing,
        Playing,
        Error
    };
    std::atomic<Status> currentStatus{Status::Idle};
    juce::String        errorMessage;

    // --- APVTS and Parameters ---
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState                         apvts;

    // Core parameters
    std::atomic<float>* volumeParam{nullptr};
    std::atomic<float>* rateParam{nullptr}; // Rate in Hz for auto-advance
    std::atomic<float>* gateParam{nullptr}; // Gate/VCA control

    SynthesisThread synthesisThread;

    // --- DSP ---
    TimePitchProcessor     timePitch;
    juce::HeapBlock<float> interleavedInput, interleavedOutput;
    int                    interleavedCapacityFrames{0};

    // --- Engine state (ported from SampleVoiceProcessor) ---
    double readPosition{0.0};   // fractional read position in samples
    double startSamplePos{0.0}; // trim start in samples
    double endSamplePos{-1.0};  // trim end in samples (exclusive)

    // --- Rate step scheduler ---
    double stepAccumulatorSec{0.0};    // countdown timer for next step jump
    double lastRateForScheduler{-1.0}; // detect rate changes for immediate reschedule

    // Track last applied time/pitch for RubberBand reset on change
    float lastEffectiveTime{-1.0f};
    float lastEffectivePitch{-10000.0f};

    // Small de-click ramp for RubberBand after hard jumps/param changes
    int rbFadeSamplesRemaining{0};
    int rbFadeSamplesTotal{0};

    // --- NEW: State for word-jump crossfading ---
    int    crossfadeSamplesRemaining{0};
    int    crossfadeSamplesTotal{0};
    double crossfadeStartPosition{0.0}; // The read position we are fading FROM
    double crossfadeEndPosition{0.0};   // The read position we are fading TO

    // --- UI helpers (bypass engine connectivity gating for live telemetry when UI knows a cable is
    // connected) ---
#if defined(PRESET_CREATOR_UI)
    float getLiveNoGate(const juce::String& liveKey, float fallback) const;
#endif

    // Model selection registry (available in both apps)
    struct ModelEntry
    {
        juce::String language;
        juce::String locale;
        juce::String voice;
        juce::String quality;
        juce::String relativeOnnx;
    };
    mutable juce::CriticalSection modelLock;
    std::vector<ModelEntry>       modelEntries;
    juce::String                  selectedLanguage{"en"};
    juce::String                  selectedLocale{"en_US"};
    juce::String                  selectedVoice{"lessac"};
    juce::String                  selectedQuality{"medium"};

#if defined(PRESET_CREATOR_UI)
    char uiTextBuffer[2048]{"Hello, modular world."};
    bool showDeleteConfirm{false};
    char renameBuffer[256]{""};
    bool showRenamePopup{false};
    void refreshModelChoices();
#endif

    // Clip selection helpers
    int          getNumCachedClips() const { return (int)clipCache.size(); }
    void         selectClipByIndex(int index);
    juce::String selectedClipId; // persisted between sessions

    // Explicitly support any discrete channel layout for modular synthesis
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        // Accept any layout as long as we have at least the minimum channels
        if (layouts.getMainInputChannelSet().isDisabled())
            return false;
        if (layouts.getMainOutputChannelSet().isDisabled())
            return false;
        return true;
    }

private:
    // Virtual IDs for modulation inputs
    static constexpr auto paramIdRateMod = "rate_mod";
    static constexpr auto paramIdGateMod = "gate_mod";
    static constexpr auto paramIdTriggerMod = "trigger_mod";
    static constexpr auto paramIdResetMod = "reset_mod";
    static constexpr auto paramIdRandomizeMod = "randomize_mod"; // <-- NEW
    static constexpr auto paramIdTrimStartMod = "trimStart_mod";
    static constexpr auto paramIdTrimEndMod = "trimEnd_mod";
    static constexpr auto paramIdSpeedMod = "speed_mod";
    static constexpr auto paramIdPitchMod = "pitch_mod";

    TransportState m_currentTransport;
    double         lastScaledBeats_tts{0.0};
    bool           wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TTSPerformerModuleProcessor);
};