#pragma once
// IAudioObserver — push-based event bus from the audio core to the bridge.
//
// The previous design polled the audio engine every 30ms from a
// dedicated UI thread (main.cpp:uiUpdateLoop). This had two
// problems:
//   1. Race conditions between the poller and the audio state
//      required workarounds (g_loadInProgress, g_telemetryGen,
//      g_uiDirty dance).
//   2. State changes were "lost" between polls — the JS side
//      could see stale audioFrames for up to 30ms after a project
//      load.
//
// The new design: the audio core PUBLISHES state changes via this
// observer. The bridge subscribes and forwards to WebKit on the
// main thread. The 30ms telemetry tick is gone.
//
// CONTRACT:
//   - All methods MAY be called from any thread, including the
//     audio thread (onSnapshot) and arbitrary plugin threads.
//   - Bridge implementations MUST be lock-free or use a
//     SPSC/MPMC ring buffer; they MUST NOT block the caller.
//   - `TelemetrySnapshot` is passed by const reference and is
//     valid for the duration of the call only. The bridge MUST
//     copy fields it needs to retain.
//   - Optional hooks (onPlayheadMoved, onTracksAdded, etc.)
//     have default no-op implementations so observers can
//     subscribe to only the events they care about.

#include <cstdint>
#include <string>
#include <vector>

namespace hydraw {

struct TrackSnapshot {
    std::string  name;
    float        volume{1.0f};
    float        pan{0.0f};
    bool         muted{false};
    bool         soloed{false};
    bool         armed{false};
    uint64_t     audioFrames{0};
    uint64_t     clipStart{0};
    // Per-track max(L,R) peak from the last audio block. The
    // bridge forwards these to the VU meter. 0 if no audio.
    float        peakL{0.0f};
    float        peakR{0.0f};
};

struct TelemetrySnapshot {
    uint64_t    playhead{0};
    int         trackCount{0};
    bool        playing{false};
    float       masterPeakL{0.0f};
    float       masterPeakR{0.0f};
    float       masterVolume{1.0f};
    // Per-track meter peaks (parallel array to STATE.peaks in JS).
    std::vector<float> peaks;
    // Full per-track state. Populated by the audio core whenever
    // a "structural" event happens (add/remove/load/move/...). The
    // bridge uses this to decide whether to do a full DOM rebuild
    // vs. a partial update.
    bool        full{false};
    std::vector<TrackSnapshot> tracks;
    // Monotonic generation, incremented on every full snapshot.
    // The bridge uses it to drop stale full updates.
    uint64_t    gen{0};
};

class IAudioObserver {
public:
    virtual ~IAudioObserver() = default;

    // The hot path. Called by the audio thread roughly once per
    // block (e.g. every 5ms at 48kHz / 256 frames). Implementation
    // MUST be lock-free; consider a SPSC ring buffer that the main
    // thread drains.
    virtual void onTelemetry(const TelemetrySnapshot& s) noexcept = 0;

    // Project load finished. gen is the new monotonic generation
    // number. The bridge uses this to invalidate its caches.
    virtual void onProjectLoaded(uint64_t gen) noexcept = 0;

    // Offline render finished. ok=true on success.
    virtual void onExportComplete(bool ok) noexcept = 0;

    // Generic log forwarding. The audio core emits via ILogSink;
    // this is for observers that want to log too.
    // (Implementation may simply forward to ILogSink.)
    virtual void onLog(int level, const char* msg) noexcept = 0;

    // ── Optional hooks (default no-op) ──
    // The audio core may call these on the main thread to signal
    // discrete events. The observer can opt in to just the events
    // it cares about.

    virtual void onPlayheadMoved(uint64_t /*pos*/) noexcept {}
    virtual void onTracksAdded(int /*newCount*/) noexcept {}
    virtual void onTracksRemoved(int /*newCount*/) noexcept {}
    virtual void onTrackAudioLoaded(int /*trackIdx*/) noexcept {}
    virtual void onProjectLoadStarted() noexcept {}
};

} // namespace hydraw
