#pragma once
#include "Core/Constants.h"
#include "Core/Types.h"
#include "Plugin/PluginChain.h"
#include "Audio/ILogSink.h"
#include "Audio/IHostExtensions.h"
#include "miniaudio.h"
#include <array>
#include <atomic>
#include <mutex>

class AudioEngine {
public:
    // hostExt: optional provider of CLAP host extensions (notably GUI).
    // logSink: optional log sink (audio-thread safe; see contract).
    // Both are forwarded to each PluginChain.
    explicit AudioEngine(hydraw::ILogSink* logSink = nullptr,
                         hydraw::IHostExtensions* hostExt = nullptr);
    ~AudioEngine();

    bool init();
    void start();
    void stop();
    void shutdown();

    Track* getTrack(int index);
    int getTrackCount() const;

    // Transport
    void play();
    void pause();
    void stopTransport();
    void setPlayhead(uint64_t pos);
    bool isPlaying() const;
    uint64_t getPlayhead() const;

    // Tempo & time signature. BPM is an atomic float read by the audio
    // thread (currently only used for time→bar conversion, not for any
    // audio processing — transport stays sample-accurate). Defaults:
    // 120 BPM, 4/4.
    void setBPM(float bpm);
    float getBPM() const;
    void setTimeSignature(int numerator, int denominator);
    int getTimeSigNum() const;
    int getTimeSigDen() const;

    // Convert a sample position to (bar, beat, sub-beat) under the
    // current BPM + time signature. 1-based: bar starts at 1. The
    // audio thread must not call this (it's a UI helper), but it is
    // reentrant and lock-free in practice.
    void sampleToBarBeat(uint64_t sample, int& bar, int& beat, double& beatFrac) const;
    // Convert a (bar, beat) back to a sample offset. Used for click
    // markers, loop region alignment, etc.
    uint64_t barBeatToSample(int bar, int beat) const;

    // Loop / Punch. loop region: [start, end) in samples. When
    // loopEnabled and playing, playhead wraps from end to start at
    // the audio-thread boundary. Punch region: [punchIn, punchOut)
    // controls auto-record (used by B.5).
    void setLoopEnabled(bool enabled);
    bool getLoopEnabled() const;
    void setLoopRange(uint64_t start, uint64_t end);
    uint64_t getLoopStart() const;
    uint64_t getLoopEnd() const;
    void setPunchRange(uint64_t start, uint64_t end);
    uint64_t getPunchStart() const;
    uint64_t getPunchEnd() const;

    // Master
    MasterBus* getMaster();
    // Aux buses (sends). Up to MAX_BUSES. getAuxBusCount() always
    // returns MAX_BUSES (4). Aux buses have volume and a plugin chain
    // (via getPluginChain with index 0..MAX_BUSES-1 plus an offset
    // handled internally — see addTrack chains).
    int getAuxBusCount() const { return MAX_BUSES; }
    AuxBus* getAuxBus(int index);
    // Send level: track t -> aux bus b in [0..1]
    void setSendLevel(int trackIndex, int busIndex, float level);
    float getSendLevel(int trackIndex, int busIndex) const;

    // Recording. startRecording() puts a track in "armed" state and
    // begins capturing input to an internal buffer. stopRecording()
    // finalizes the buffer into a new AudioBuffer loaded as a clip at
    // the current playhead position. Only one track can be recorded
    // at a time (the one passed to startRecording).
    void startRecording(int trackIndex, const char* outPath);
    void stopRecording();
    bool isRecording() const;
    int getRecordingTrack() const;
    uint64_t getRecordedFrames() const;

    // WAV loading
    bool loadWavToTrack(int trackIndex, const char* path);

    // Clip positioning
    void setClipStart(int trackIndex, uint64_t pos);
    void setClipStart(int trackIndex, int clipIndex, uint64_t pos);
    void swapTrackAudio(int t1, int t2);
    void moveTrackAudio(int src, int dst);

    // Multi-clip API. addClip appends a new clip to the track at the
    // given start position. The new clip owns its own AudioBuffer and
    // is added to a per-track vector protected by m_clipsMutex. The
    // audio thread reads via atomic snapshot pointer. removeClip
    // removes by index (0 = primary, 1+ = extras). splitClip creates
    // a new clip from an existing one at the given sample offset.
    int addClip(int trackIndex, const char* path, uint64_t start);
    bool removeClip(int trackIndex, int clipIndex);
    int getClipCount(int trackIndex) const;
    uint64_t getClipStart(int trackIndex, int clipIndex) const;
    uint64_t getClipFrames(int trackIndex, int clipIndex) const;
    std::string getClipPath(int trackIndex, int clipIndex) const;
    // Return a copy of the peak cache for clip `clipIndex` of track
    // `trackIndex`. Used by the UI to draw the waveform of every
    // clip independently. Thread-safe: locks the clips mutex briefly.
    std::vector<float> getClipPeakCache(int trackIndex, int clipIndex) const;
    // Move a clip (cross-track or reorder within same track). The clip
    // keeps its AudioBuffer and clipStart. On cross-track moves, the
    // ownership of the AudioBuffer is transferred.
    bool moveClip(int srcTrack, int srcClip, int dstTrack, int dstIndex, uint64_t newClipStart = 0);

    // Split the clip at the given absolute timeline position. Creates
    // a new clip that owns the second half. The original keeps the
    // first half. Returns the new clip index, or -1 on error.
    // NOTE: copies samples; expensive on large clips but always safe.
    int splitClipAt(int trackIndex, int clipIndex, uint64_t timelinePos);
    // Trim the clip to a new [newStart, newEnd] (both absolute timeline
    // positions, in samples). Returns true on success.
    bool trimClip(int trackIndex, int clipIndex, uint64_t newStart, uint64_t newEnd);
    // Set linear fade-in (in samples) or fade-out. The audio thread
    // applies the fade at the buffer edges.
    void setClipFadeIn(int trackIndex, int clipIndex, uint64_t samples);
    void setClipFadeOut(int trackIndex, int clipIndex, uint64_t samples);
    uint64_t getClipFadeIn(int trackIndex, int clipIndex) const;
    uint64_t getClipFadeOut(int trackIndex, int clipIndex) const;

    // Automation. addAutoPoint appends a (samplePos, value) point to
    // the track's volume or pan lane. clearAutoLane removes all points.
    // The audio thread reads a snapshot at the start of each block.
    // Snapshot is a heap-allocated struct (atomic<AutoSnapshot*>) and
    // published via a swap; old snapshot is queued for deferred delete.
    enum class AutoParam { Volume, Pan };
    void addAutoPoint(int trackIndex, AutoParam param, uint64_t samplePos, float value);
    void clearAutoLane(int trackIndex, AutoParam param);
    int getAutoPointCount(int trackIndex, AutoParam param) const;

    // Track freeze: renders the track's current audio + plugin chain
    // to a temporary AudioBuffer and stores it as the track's
    // Track freeze: renders the track's current audio + plugin chain
    // to a temporary AudioBuffer and stores it as the track's
    // "frozen" output. While frozen, the live plugin chain is bypassed
    // by the audio thread. unfreezeTrack restores live processing.
    // This is a CPU-saver when a track's plugins are expensive and
    // the arrangement is settled.
    void freezeTrack(int trackIndex);
    void unfreezeTrack(int trackIndex);
    bool isTrackFrozen(int trackIndex) const;

    // MIDI. addMidiNote appends a note to the track's MIDI lane.
    // The audio thread generates clap_event_note_on/off events at the
    // appropriate sample positions and feeds them to the plugin chain
    // via input events. clearMidiLane removes all notes. This is a
    // basic implementation — synth playback is delegated to CLAP
    // instruments (the plugin chain must contain at least one
    // note-generating plugin, or no audio is produced).
    int addMidiNote(int trackIndex, const MidiNote& note);
    void clearMidiLane(int trackIndex);
    int getMidiNoteCount(int trackIndex) const;

    // Dynamic tracks
    int addTrack();
    void removeTrack(int index);
    int getTrackCountAtomic() const;

    // Plugin chain access (trackIndex == -1 returns master chain)
    PluginChain* getPluginChain(int trackIndex);

    // Export offline render (bouncing)
    bool renderOffline(const char* wavPath, int sampleRate, int bitDepth);

    // Track audio file path
    void setTrackAudioPath(int index, const char* path);
    std::string getTrackAudioPath(int index);

    // Deferred-delete queue. When the main thread (or any non-audio
    // thread) wants to free an AudioBuffer that the audio thread might
    // still be reading, it pushes the pointer here instead of calling
    // delete. The audio callback drains the queue at the end of each
    // block, after every in-flight memcpy has returned. Closes the
    // use-after-free window where the audio thread holds a local copy
    // of `track.audio` from a load() that races with a main-thread
    // delete.
    void queueAudioBufferDelete(AudioBuffer* buf);
    void drainPendingAudioBufferDeletes();
    // Recompute per-chain and master latency compensation. Call after
    // any plugin add/remove/move or bypass change.
    void rebuildLatencyMap();

private:
    static void audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    ma_device_config m_deviceConfig;
    ma_device m_device;
    ma_context m_context;
    std::array<Track, MAX_TRACKS> m_tracks;
    std::array<std::unique_ptr<PluginChain>, MAX_TRACKS> m_pluginChains;
    std::unique_ptr<PluginChain> m_masterPluginChain;
    std::atomic<int> m_trackCount{1};
    bool m_initialized{false};
    TransportState m_transport;
    MasterBus m_master;
    std::array<AuxBus, MAX_BUSES> m_auxBuses;
    std::atomic<float> m_bpm{120.0f};
    std::atomic<int> m_timeSigNum{4};
    std::atomic<int> m_timeSigDen{4};
    std::atomic<bool> m_loopEnabled{false};
    std::atomic<uint64_t> m_loopStart{0};
    std::atomic<uint64_t> m_loopEnd{0};
    std::atomic<uint64_t> m_punchStart{0};
    std::atomic<uint64_t> m_punchEnd{0};
    // Recording state. m_recording protects the audio thread from
    // racing with stopRecording on the main thread. m_recBuf is grown
    // by the audio thread when recording. m_recStartPos captures the
    // playhead at recording start so the clip's clipStart is stable.
    std::atomic<bool> m_recording{false};
    std::atomic<int> m_recTrack{-1};
    std::atomic<uint64_t> m_recStartPos{0};
    std::atomic<uint64_t> m_recFrames{0};
    std::vector<float> m_recBufL;
    std::vector<float> m_recBufR;
    std::mutex m_recMutex;
    std::string m_recOutPath;
    // The audio thread NEVER takes this mutex — it must run in real time
    // without blocking. addTrack/removeTrack are the only writers and they
    // just atomically swap / update the per-track fields. We keep the
    // mutex around for any future lock-protected helper that needs to
    // observe a consistent view (e.g. project save) but the audio
    // callback stays lock-free.
    std::mutex m_chainMutex;

    // Optional logger. May be null (no logging). The audio thread
    // calls log() from the callback (throttled) and from the CLAP
    // host log handler. The sink MUST be audio-thread safe.
    hydraw::ILogSink* m_logSink{nullptr};

    // Optional provider of CLAP host extensions. Each PluginChain
    // receives this pointer. May be null (the audio core then
    // returns null for extensions it doesn't implement itself).
    hydraw::IHostExtensions* m_hostExt{nullptr};
    // Protected by m_pendingDeleteMutex. The audio thread drains this
    // at the end of every block (so any in-flight load() from a previous
    // block has already finished using the buffer). The main thread
    // only ever appends to it.
    std::mutex m_pendingDeleteMutex;
    std::vector<AudioBuffer*> m_pendingAudioBufferDeletes;

    // ── Latency compensation (PDC) ──
    // Per-track circular delay buffer of size = maxLatency. When a
    // plugin chain reports N samples of latency, the wet output of
    // the chain is delayed by N - chainBaseLatency samples so that all
    // tracks align in time. The master chain latency is the final
    // delay applied to `out` before the consumer hears it.
    struct DelayLine {
        std::vector<float> bufL;
        std::vector<float> bufR;
        size_t capacity{0};
        size_t writePos{0};
        uint32_t delay{0};
        void resize(uint32_t n) {
            if (n == capacity) return;
            capacity = n;
            bufL.assign(n, 0.0f);
            bufR.assign(n, 0.0f);
            writePos = 0;
        }
        // In-place delay: x[i] = (buf read at writePos + i)
        void process(float* L, float* R, size_t frames) {
            if (capacity == 0 || delay == 0) return;
            for (size_t i = 0; i < frames; ++i) {
                size_t rpos = (writePos + capacity - delay) % capacity;
                float oL = bufL[rpos];
                float oR = bufR[rpos];
                bufL[writePos] = L[i];
                bufR[writePos] = R[i];
                L[i] = oL;
                R[i] = oR;
                writePos = (writePos + 1) % capacity;
            }
        }
    };
    std::array<DelayLine, MAX_TRACKS> m_trackDelays;
    DelayLine m_masterDelay;
    // Max latency observed in plugin chains (recomputed when chains
    // change). Master delay always uses the largest chain latency.
    std::atomic<uint32_t> m_maxPluginLatency{0};

    // ── Multi-clip support ──
    // Per-track vector of additional clips (index 0 = first extra clip).
    // m_clipsMutex is held only on the main thread. The audio thread
    // reads via m_clipsSnapshot (atomic pointer to an immutable vector).
    // When the main thread mutates m_trackClips, it builds a new vector
    // and atomically swaps the snapshot pointer; the old one is queued
    // for deferred delete at the end of the audio callback.
    struct ClipsSnapshot {
        std::vector<Clip> clips; // for a single track
    };
    std::vector<std::vector<Clip>> m_trackClips;
    mutable std::mutex m_clipsMutex;
    // Per-track atomic snapshot pointer read by the audio thread.
    std::array<std::atomic<ClipsSnapshot*>, MAX_TRACKS> m_clipsSnapshot;
    // Pending deletes for old snapshots. Drained by the audio thread
    // at the end of each callback, same pattern as AudioBuffer deletes.
    std::mutex m_pendingClipsSnapshotMutex;
    std::vector<ClipsSnapshot*> m_pendingClipsSnapshots;

    void publishClipsSnapshot(int trackIndex);
    void rebuildPrimaryFromClips(int trackIndex);

    // Automation snapshot: per-track copy of volumeAutomation and
    // panAutomation. The audio thread reads via atomic pointer.
    struct AutoSnapshot {
        std::vector<Track::AutoPoint> vol;
        std::vector<Track::AutoPoint> pan;
    };
    std::array<std::atomic<AutoSnapshot*>, MAX_TRACKS> m_autoSnapshot;
    std::mutex m_autoSnapMutex;
    std::vector<AutoSnapshot*> m_pendingAutoSnapshots;

    // MIDI snapshot. The audio thread reads a per-track immutable
    // vector of MidiNote via atomic pointer. Main thread publishes
    // new snapshot on add/clear.
    struct MidiSnapshot {
        std::vector<MidiNote> notes;
    };
    std::array<std::atomic<MidiSnapshot*>, MAX_TRACKS> m_midiSnapshot;
    std::mutex m_midiSnapMutex;
    std::vector<MidiSnapshot*> m_pendingMidiSnapshots;
    void publishMidiSnapshot(int trackIndex);
};
