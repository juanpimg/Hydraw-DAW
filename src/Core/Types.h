#pragma once
#include "Constants.h"
#include <atomic>
#include <array>
#include <string>
#include <vector>
#include <memory>

struct AudioSample {
    std::atomic<float> left{0.0f};
    std::atomic<float> right{0.0f};
};

struct AudioBuffer {
    std::vector<float> dataL;
    std::vector<float> dataR;
    uint64_t frames{0};
    std::vector<float> peakCache;
};

struct TransportState {
    std::atomic<bool> playing{false};
    std::atomic<uint64_t> playhead{0};
    std::atomic<uint64_t> totalFrames{0};
};

struct MasterBus {
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
};

struct AuxBus {
    std::string name;
    std::atomic<float> volume{1.0f};
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
};

struct MidiNote {
    uint64_t start{0};      // sample offset in project timeline
    uint64_t duration{0};   // in samples
    uint8_t pitch{60};      // 0-127, middle C = 60
    uint8_t velocity{100};  // 1-127
    uint8_t channel{0};     // 0-15
    uint8_t _pad{0};
};

// A single audio clip on a track. The "primary" clip is stored directly
// on Track (audio/clipStart) for lock-free access from the audio
// thread. Additional clips (2nd, 3rd, etc.) live in Track::extraClips
// and are mixed in by the audio thread via the per-track atomic
// pointer to a small immutable vector snapshot.
struct Clip {
    AudioBuffer* buffer{nullptr};   // owned, freed via deferred delete
    uint64_t clipStart{0};          // sample offset in project timeline
    std::atomic<uint64_t> fadeIn{0};   // linear fade-in length, samples
    std::atomic<uint64_t> fadeOut{0};  // linear fade-out length, samples
    std::string filePath;           // for save/load (relative when possible)
    // Copy ctor: atomics are not movable, so we re-init them from source.
    Clip() = default;
    Clip(const Clip& o)
        : buffer(o.buffer), clipStart(o.clipStart),
          fadeIn(o.fadeIn.load(std::memory_order_relaxed)),
          fadeOut(o.fadeOut.load(std::memory_order_relaxed)),
          filePath(o.filePath) {}
    Clip& operator=(const Clip& o) {
        if (this != &o) {
            buffer = o.buffer;
            clipStart = o.clipStart;
            fadeIn.store(o.fadeIn.load(std::memory_order_relaxed), std::memory_order_relaxed);
            fadeOut.store(o.fadeOut.load(std::memory_order_relaxed), std::memory_order_relaxed);
            filePath = o.filePath;
        }
        return *this;
    }
    // Move ctor: same restriction; just copy.
    Clip(Clip&& o) noexcept : Clip(o) {}
    Clip& operator=(Clip&& o) noexcept {
        return (*this = o);
    }
};

struct Track {
    std::string name;
    std::string audioFilePath;
    // Recording scratch buffers (audio-thread write only, never read by
    // any other thread while recording). Plain floats — no atomics needed
    // and no false-sharing cache lines. 2KB/track × 16 tracks = 32KB
    // saved vs the previous std::atomic<float>[BLOCK_SIZE] version.
    float bufferL[BLOCK_SIZE];
    float bufferR[BLOCK_SIZE];
    int writeIndex{0};
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<bool> armed{false};
    std::atomic<bool> muted{false};
    std::atomic<bool> soloed{false};
    // Legacy single-clip fields — used for the FIRST clip only.
    // When a second clip is added, the first one is moved into
    // extraClips and these become nullptr/0.
    std::atomic<AudioBuffer*> audio{nullptr};
    std::atomic<uint64_t> clipStart{0};
    std::atomic<uint64_t> primaryFadeIn{0};
    std::atomic<uint64_t> primaryFadeOut{0};
    // Send levels to aux buses [0..MAX_BUSES-1]. 0.0 = no send.
    std::array<std::atomic<float>, MAX_BUSES> sends{};
    // Automation: volume / pan lanes. Each lane is a sorted array of
    // (samplePos, value) points. When empty, the static value (m_volume
    // / m_pan) is used. When non-empty, the audio thread interpolates
    // the value at the current playhead.
    struct AutoPoint {
        uint64_t samplePos;
        float value;
    };
    std::vector<AutoPoint> volumeAutomation;
    std::vector<AutoPoint> panAutomation;
    // MIDI notes for this track. Sorted by start. The audio thread
    // reads via atomic snapshot pointer.
    std::vector<MidiNote> midiNotes;
    // Additional clips (track 1+). Protected by m_trackClipsMutex
    // (main thread only). The audio thread reads them via atomic
    // pointer to a snapshot vector. See AudioEngine::m_clipsSnapshot.
    // Declared here only as a forward-declared pointer; the actual
    // vector is owned by AudioEngine.
    void* extraClipsHandle{nullptr};
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
    // Track freeze: when non-null, this AudioBuffer* holds the
    // pre-rendered (audio + plugin chain) output of the track. The
    // audio thread plays from this instead of the live chain. Allocated
    // by the main thread (via freezeTrack), freed via deferred delete
    // on unfreeze.
    std::atomic<AudioBuffer*> frozenBuffer{nullptr};
    std::atomic<uint64_t> frozenClipStart{0};
    std::atomic<bool> frozen{false};
};
