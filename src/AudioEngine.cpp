#include "AudioEngine.h"
#include <algorithm>
#include <cmath>
#include <cstring>

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
    ma_backend backends[] = { ma_backend_alsa, ma_backend_pulseaudio };
    ma_context_config ctxConfig = ma_context_config_init();
    ctxConfig.threadPriority = ma_thread_priority_realtime;

    if (ma_context_init(backends, 2, &ctxConfig, &m_context) != MA_SUCCESS) {
        return false;
    }

    m_deviceConfig = ma_device_config_init(ma_device_type_duplex);
    m_deviceConfig.playback.format   = ma_format_f32;
    m_deviceConfig.playback.channels = 2;
    m_deviceConfig.capture.format    = ma_format_f32;
    m_deviceConfig.capture.channels  = 2;
    m_deviceConfig.sampleRate        = SAMPLE_RATE;
    m_deviceConfig.dataCallback      = audioCallback;
    m_deviceConfig.pUserData         = this;
    m_deviceConfig.periodSizeInFrames = BLOCK_SIZE;
    m_deviceConfig.periods           = 3;

    if (ma_device_init(&m_context, &m_deviceConfig, &m_device) != MA_SUCCESS) {
        ma_context_uninit(&m_context);
        return false;
    }

    for (int t = 0; t < MAX_TRACKS; ++t) {
        m_pluginChains[t] = std::make_unique<PluginChain>(SAMPLE_RATE, BLOCK_SIZE, t);
    }

    m_initialized = true;
    return true;
}

void AudioEngine::start() {
    if (m_initialized) ma_device_start(&m_device);
}

void AudioEngine::stop() {
    if (m_initialized) ma_device_stop(&m_device);
}

void AudioEngine::shutdown() {
    if (m_initialized) {
        m_transport.playing.store(false);
        ma_device_uninit(&m_device);
        ma_context_uninit(&m_context);
        for (auto& track : m_tracks) {
            AudioBuffer* buf = track.audio.load(std::memory_order_relaxed);
            if (buf) delete buf;
            track.audio.store(nullptr, std::memory_order_relaxed);
        }
        m_initialized = false;
    }
}

Track* AudioEngine::getTrack(int index) {
    if (index < 0 || index >= MAX_TRACKS) return nullptr;
    return &m_tracks[index];
}

int AudioEngine::getTrackCount() const {
    return m_trackCount.load();
}

int AudioEngine::getTrackCountAtomic() const { return m_trackCount.load(); }

void AudioEngine::play() {
    m_transport.playing.store(true);
}

void AudioEngine::pause() {
    m_transport.playing.store(false);
}

void AudioEngine::stopTransport() {
    m_transport.playing.store(false);
    m_transport.playhead.store(0);
}

void AudioEngine::setPlayhead(uint64_t pos) {
    m_transport.playhead.store(pos);
}

bool AudioEngine::isPlaying() const {
    return m_transport.playing.load();
}

uint64_t AudioEngine::getPlayhead() const {
    return m_transport.playhead.load();
}

MasterBus* AudioEngine::getMaster() { return &m_master; }

int AudioEngine::addTrack() {
    int current = m_trackCount.load();
    if (current >= MAX_TRACKS) return -1;
    int newIndex = current;
    m_tracks[newIndex].name = "Track " + std::to_string(newIndex + 1);
    m_tracks[newIndex].volume.store(1.0f);
    m_tracks[newIndex].pan.store(0.0f);
    m_trackCount.store(current + 1);
    return newIndex;
}

void AudioEngine::removeTrack(int index) {
    int current = m_trackCount.load();
    if (current <= 1) return;
    if (index < 0 || index >= current) return;
    for (int i = index; i < current - 1; ++i) {
        // Free buffer being overwritten
        AudioBuffer* toFree = m_tracks[i].audio.load(std::memory_order_relaxed);
        if (toFree) delete toFree;
        m_tracks[i].name = m_tracks[i + 1].name;
        m_tracks[i].volume.store(m_tracks[i + 1].volume.load());
        m_tracks[i].pan.store(m_tracks[i + 1].pan.load());
        m_tracks[i].muted.store(m_tracks[i + 1].muted.load());
        m_tracks[i].soloed.store(m_tracks[i + 1].soloed.load());
        m_tracks[i].armed.store(m_tracks[i + 1].armed.load());
        m_tracks[i].audio.store(m_tracks[i + 1].audio.load());
        m_tracks[i].clipStart.store(m_tracks[i + 1].clipStart.load());
        std::swap(m_pluginChains[i], m_pluginChains[i + 1]);
    }
    int newCount = current - 1;
    // Free buffer at vacated slot
    AudioBuffer* last = m_tracks[newCount].audio.load(std::memory_order_relaxed);
    if (last) delete last;
    m_tracks[newCount].audio.store(nullptr);
    m_trackCount.store(newCount);
}

bool AudioEngine::loadWavToTrack(int trackIndex, const char* path) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return false;

    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, SAMPLE_RATE);
    if (ma_decoder_init_file(path, &config, &decoder) != MA_SUCCESS) {
        return false;
    }

    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (totalFrames == 0) {
        ma_decoder_uninit(&decoder);
        return false;
    }

    AudioBuffer* buf = new AudioBuffer();
    buf->dataL.resize(totalFrames);
    buf->dataR.resize(totalFrames);
    std::vector<float> temp(totalFrames * 2);

    ma_uint64 framesRead = 0;
    ma_result readResult = ma_decoder_read_pcm_frames(&decoder, temp.data(), totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);

    if (readResult != MA_SUCCESS || framesRead == 0) {
        delete buf;
        return false;
    }

    for (ma_uint64 i = 0; i < framesRead; ++i) {
        buf->dataL[i] = temp[i * 2 + 0];
        buf->dataR[i] = temp[i * 2 + 1];
    }
    buf->frames = framesRead;

    // Dynamic peak count: ~1 peak per 600 frames (~12.5ms at 48kHz), min 100
    int numPeaks = std::max(100, (int)(framesRead / 600));
    buf->peakCache.resize(numPeaks);
    if (framesRead > 0) {
        for (int p = 0; p < numPeaks; ++p) {
            uint64_t start = (uint64_t)p * framesRead / numPeaks;
            uint64_t end = (uint64_t)(p + 1) * framesRead / numPeaks;
            if (end > framesRead) end = framesRead;
            if (end <= start) end = start + 1;
            float peak = 0.0f;
            for (uint64_t i = start; i < end; ++i) {
                peak = std::max(peak, std::abs(buf->dataL[i]));
                peak = std::max(peak, std::abs(buf->dataR[i]));
            }
            buf->peakCache[p] = peak;
        }
    }

    // Free old buffer on this track before overwriting
    AudioBuffer* oldBuf = m_tracks[trackIndex].audio.load(std::memory_order_acquire);
    if (oldBuf) delete oldBuf;

    m_tracks[trackIndex].audio.store(buf, std::memory_order_release);
    m_tracks[trackIndex].clipStart.store(0, std::memory_order_release);
    return true;
}

void AudioEngine::setClipStart(int trackIndex, uint64_t pos) {
    if (trackIndex >= 0 && trackIndex < m_trackCount.load())
        m_tracks[trackIndex].clipStart.store(pos, std::memory_order_release);
}

void AudioEngine::swapTrackAudio(int t1, int t2) {
    if (t1 < 0 || t2 < 0 || t1 >= MAX_TRACKS || t2 >= MAX_TRACKS) return;
    AudioBuffer* a1 = m_tracks[t1].audio.load();
    AudioBuffer* a2 = m_tracks[t2].audio.load();
    uint64_t s1 = m_tracks[t1].clipStart.load();
    uint64_t s2 = m_tracks[t2].clipStart.load();
    m_tracks[t1].audio.store(a2, std::memory_order_release);
    m_tracks[t2].audio.store(a1, std::memory_order_release);
    m_tracks[t1].clipStart.store(s2, std::memory_order_release);
    m_tracks[t2].clipStart.store(s1, std::memory_order_release);
    std::swap(m_tracks[t1].name, m_tracks[t2].name);
}

void AudioEngine::moveTrackAudio(int src, int dst) {
    if (src < 0 || dst < 0 || src >= m_trackCount.load() || dst >= m_trackCount.load()) return;
    // Move audio buffer from src to dst, clear src
    AudioBuffer* buf = m_tracks[src].audio.exchange(nullptr);
    if (!buf) return; // nothing to move — don't touch dst
    AudioBuffer* oldBuf = m_tracks[dst].audio.exchange(buf);
    if (oldBuf) delete oldBuf;
    m_tracks[src].clipStart.store(0, std::memory_order_release);
}

PluginChain* AudioEngine::getPluginChain(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return nullptr;
    return m_pluginChains[trackIndex].get();
}

void AudioEngine::audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioEngine* engine = static_cast<AudioEngine*>(pDevice->pUserData);
    float* out = static_cast<float*>(pOutput);
    const float* in = static_cast<const float*>(pInput);

    std::fill(out, out + frameCount * 2, 0.0f);

    bool playing = engine->m_transport.playing.load(std::memory_order_acquire);
    uint64_t basePlayhead = engine->m_transport.playhead.load(std::memory_order_relaxed);
    int trackCount = engine->m_trackCount.load(std::memory_order_relaxed);

    if (!playing && basePlayhead == 0) return;

    for (int t = 0; t < trackCount; ++t) {
        Track& track = engine->m_tracks[t];
        if (track.muted.load(std::memory_order_relaxed)) continue;

        // Read block of samples for this track
        float blockL[BLOCK_SIZE], blockR[BLOCK_SIZE];
        bool hasAudio = false;

        if (playing) {
            AudioBuffer* buf = track.audio.load(std::memory_order_acquire);
            uint64_t cs = track.clipStart.load(std::memory_order_relaxed);
            if (buf) {
                for (ma_uint32 f = 0; f < frameCount; ++f) {
                    uint64_t pos = basePlayhead + f;
                    if (pos >= cs) {
                        uint64_t offset = pos - cs;
                        if (offset < buf->frames) {
                            blockL[f] = buf->dataL[offset];
                            blockR[f] = buf->dataR[offset];
                            if (buf->dataL[offset] != 0.0f || buf->dataR[offset] != 0.0f)
                                hasAudio = true;
                        } else {
                            blockL[f] = 0.0f;
                            blockR[f] = 0.0f;
                        }
                    } else {
                        blockL[f] = 0.0f;
                        blockR[f] = 0.0f;
                    }
                }
            } else {
                memset(blockL, 0, sizeof(blockL));
                memset(blockR, 0, sizeof(blockR));
            }
        } else {
            memset(blockL, 0, sizeof(blockL));
            memset(blockR, 0, sizeof(blockR));
        }

        // Add armed input
        if (track.armed.load(std::memory_order_relaxed) && in) {
            hasAudio = true;
            for (ma_uint32 f = 0; f < frameCount; ++f) {
                blockL[f] += in[f * 2 + 0];
                blockR[f] += in[f * 2 + 1];
                track.bufferL[track.writeIndex].store(in[f * 2 + 0], std::memory_order_relaxed);
                track.bufferR[track.writeIndex].store(in[f * 2 + 1], std::memory_order_relaxed);
            }
        }

        // Process per-track plugin chain on the full block
        auto& chain = engine->m_pluginChains[t];
        if (chain && chain->getPluginCount() > 0 && hasAudio) {
            // Convert to planar layout expected by PluginChain::process
            float planar[BLOCK_SIZE * 2];
            for (ma_uint32 f = 0; f < frameCount; ++f) {
                planar[f] = blockL[f];
                planar[f + frameCount] = blockR[f];
            }
            chain->process(planar, planar, (int)frameCount, 2);
            // De-interleave back
            for (ma_uint32 f = 0; f < frameCount; ++f) {
                blockL[f] = planar[f];
                blockR[f] = planar[f + frameCount];
            }
        }

        float vol = track.volume.load(std::memory_order_relaxed);
        float panVal = track.pan.load(std::memory_order_relaxed);
        double leftGain = std::min(1.0, 1.0 - panVal) * vol;
        double rightGain = std::min(1.0, 1.0 + panVal) * vol;

        float peakL = 0.0f, peakR = 0.0f;
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            double tL = blockL[f] * leftGain;
            double tR = blockR[f] * rightGain;
            out[f * 2 + 0] += (float)tL;
            out[f * 2 + 1] += (float)tR;

            float absL = (float)std::abs(tL);
            float absR = (float)std::abs(tR);
            if (absL > peakL) peakL = absL;
            if (absR > peakR) peakR = absR;
        }

        // Track peak envelope (peak hold with slow decay)
        float oldPL = track.peakL.load(std::memory_order_relaxed);
        float oldPR = track.peakR.load(std::memory_order_relaxed);
        track.peakL.store(peakL > oldPL ? peakL : oldPL * 0.999f, std::memory_order_relaxed);
        track.peakR.store(peakR > oldPR ? peakR : oldPR * 0.999f, std::memory_order_relaxed);
    }

    // Apply master volume and update master peaks
    float masterVol = engine->m_master.volume.load(std::memory_order_relaxed);
    float masterPeakL = 0.0f, masterPeakR = 0.0f;
    for (ma_uint32 f = 0; f < frameCount; ++f) {
        float mL = out[f * 2 + 0] * masterVol;
        float mR = out[f * 2 + 1] * masterVol;
        out[f * 2 + 0] = mL;
        out[f * 2 + 1] = mR;
        float absML = std::abs(mL), absMR = std::abs(mR);
        if (absML > masterPeakL) masterPeakL = absML;
        if (absMR > masterPeakR) masterPeakR = absMR;
    }
    float oldML = engine->m_master.peakL.load(std::memory_order_relaxed);
    float oldMR = engine->m_master.peakR.load(std::memory_order_relaxed);
    engine->m_master.peakL.store(masterPeakL > oldML ? masterPeakL : oldML * 0.999f, std::memory_order_relaxed);
    engine->m_master.peakR.store(masterPeakR > oldMR ? masterPeakR : oldMR * 0.999f, std::memory_order_relaxed);

    if (playing) {
        uint64_t newPlayhead = engine->m_transport.playhead.fetch_add(frameCount, std::memory_order_release) + frameCount;

        bool anyHasAudio = false;
        bool stillPlaying = false;
        for (int t = 0; t < trackCount; ++t) {
            AudioBuffer* buf = engine->m_tracks[t].audio.load(std::memory_order_relaxed);
            if (buf && buf->frames > 0) {
                anyHasAudio = true;
                uint64_t cs = engine->m_tracks[t].clipStart.load(std::memory_order_relaxed);
                if (newPlayhead < cs + buf->frames)
                    stillPlaying = true;
            }
        }
        if (anyHasAudio && !stillPlaying)
            engine->m_transport.playing.store(false, std::memory_order_release);
    }
}
