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

    // Plugin chain access
    PluginChain* getPluginChain(int trackIndex);

private:
    static void audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    ma_device_config m_deviceConfig;
    ma_device m_device;
    ma_context m_context;
    std::array<Track, MAX_TRACKS> m_tracks;
    std::array<std::unique_ptr<PluginChain>, MAX_TRACKS> m_pluginChains;
    std::mutex m_pluginMutex;
    std::atomic<int> m_trackCount{1};
    bool m_initialized{false};
    TransportState m_transport;
    MasterBus m_master;
};
