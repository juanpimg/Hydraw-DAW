#pragma once
#include "Core/Constants.h"
#include "Core/Types.h"
#include "Plugin/PluginChain.h"
#include "miniaudio.h"
#include <array>
#include <atomic>
#include <mutex>

class AudioEngine {
public:
    AudioEngine();
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

    // Master
    MasterBus* getMaster();

    // WAV loading
    bool loadWavToTrack(int trackIndex, const char* path);

    // Clip positioning
    void setClipStart(int trackIndex, uint64_t pos);
    void swapTrackAudio(int t1, int t2);
    void moveTrackAudio(int src, int dst);

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
    // The audio thread NEVER takes this mutex — it must run in real time
    // without blocking. addTrack/removeTrack are the only writers and they
    // just atomically swap / update the per-track fields. We keep the
    // mutex around for any future lock-protected helper that needs to
    // observe a consistent view (e.g. project save) but the audio
    // callback stays lock-free.
    std::mutex m_chainMutex;
    // Protected by m_pendingDeleteMutex. The audio thread drains this
    // at the end of every block (so any in-flight load() from a previous
    // block has already finished using the buffer). The main thread
    // only ever appends to it.
    std::mutex m_pendingDeleteMutex;
    std::vector<AudioBuffer*> m_pendingAudioBufferDeletes;
};
