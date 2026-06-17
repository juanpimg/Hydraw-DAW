#include "AudioEngine.h"
#include "Export/WavWriter.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

extern void dlog(const std::string& msg);
extern std::string threadId();

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

    m_deviceConfig = ma_device_config_init(ma_device_type_playback);
    m_deviceConfig.playback.format   = ma_format_f32;
    m_deviceConfig.playback.channels = 2;
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
    m_masterPluginChain = std::make_unique<PluginChain>(SAMPLE_RATE, BLOCK_SIZE, -1);

    // ── Preload bundled CLAP plugins on master ──
    // PeakEater ships in the repo at plugins/peakeater.clap and is
    // copied next to the binary by CMake. We always load it on master
    // with a transparent state so the user gets the plugin for free
    // and can adjust it from the FX chain. If the .clap is missing
    // (e.g. installed via package manager) we just skip silently.
    {
        // JUCE AudioProcessorValueTreeState format. JUCE plugins identify
        // by their root tag (== JucePlugin_Name). For PeakEater this is
        // "PeakEater". AudioParameterChoice stores the SELECTED INDEX
        // (not the name). AudioParameterBool stores 0/1. AudioParameterFloat
        // stores the raw value in its native unit.
        //   InputGain / OutputGain  : float dB
        //   LinkInOut / BYPASS       : bool 0/1
        //   Ceiling                  : float dB (0.0 .. -24.0)
        //   ClippingType             : int 0=HARD, 1=QUINTIC, 2=CUBIC, 3=HYP_TAN, 4=ALGEBRAIC, 5=ARCTANGENT
        //   OversampleRate           : int 0=Off, 1=2x, 2=4x
        //   DryWet                   : float 0.0..1.0
        // "Transparent" preset: 0 dB, 2x oversampling, HARD, 100% wet.
        // With a 0 dB ceiling and HARD clipping the plugin is a pass-through
        // for any signal that stays below 0 dBFS.
        static constexpr const char* kTransparentXml =
            "<PeakEater>"
            "<PARAM id=\"InputGain\" value=\"0.0\"/>"
            "<PARAM id=\"OutputGain\" value=\"0.0\"/>"
            "<PARAM id=\"LinkInOut\" value=\"0\"/>"
            "<PARAM id=\"BYPASS\" value=\"0\"/>"
            "<PARAM id=\"Ceiling\" value=\"0.0\"/>"
            "<PARAM id=\"ClippingType\" value=\"0\"/>"
            "<PARAM id=\"OversampleRate\" value=\"1\"/>"
            "<PARAM id=\"DryWet\" value=\"1.0\"/>"
            "</PeakEater>";
        static constexpr const char* kPeakEaterPath = "plugins/peakeater.clap";
        static constexpr const char* kPeakEaterId   = "com.T-Audio.peakeater";

        if (std::filesystem::exists(kPeakEaterPath)) {
            bool ok = m_masterPluginChain->addPlugin(kPeakEaterPath, kPeakEaterId);
            if (!ok) {
                fprintf(stderr, "[AUDIO] WARN: failed to preload %s (id=%s)\n",
                        kPeakEaterPath, kPeakEaterId);
            } else {
                bool okState = m_masterPluginChain->loadState(0, kTransparentXml);
                if (!okState) {
                    fprintf(stderr, "[AUDIO] WARN: preload ok but state load failed for %s\n",
                            kPeakEaterPath);
                } else {
                    fprintf(stderr, "[AUDIO] Preloaded %s on master with transparent state\n",
                            kPeakEaterPath);
                }
            }
        } else {
            fprintf(stderr, "[AUDIO] Note: %s not found, master chain empty\n",
                    kPeakEaterPath);
        }
    }

    m_initialized = true;
    return true;
}

void AudioEngine::start() {
    if (m_initialized) {
        // The audio callback itself calls plugin->start_processing() on
        // the first block (per CLAP spec, start_processing is [audio-thread]).
        // We just need to start the device.
        ma_device_start(&m_device);
    }
}

void AudioEngine::stop() {
    if (m_initialized) {
        ma_device_stop(&m_device);
        // After ma_device_stop returns, the audio thread is done. The
        // callback itself calls plugin->stop_processing() on the last
        // block before returning.
    }
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
    m_tracks[newIndex].volume.store(1.0f, std::memory_order_relaxed);
    m_tracks[newIndex].pan.store(0.0f, std::memory_order_relaxed);
    m_tracks[newIndex].muted.store(false, std::memory_order_relaxed);
    m_tracks[newIndex].soloed.store(false, std::memory_order_relaxed);
    m_tracks[newIndex].armed.store(false, std::memory_order_relaxed);
    m_tracks[newIndex].audio.store(nullptr, std::memory_order_relaxed);
    m_tracks[newIndex].clipStart.store(0, std::memory_order_relaxed);
    m_tracks[newIndex].peakL.store(0.0f, std::memory_order_relaxed);
    m_tracks[newIndex].peakR.store(0.0f, std::memory_order_relaxed);
    m_trackCount.store(current + 1, std::memory_order_release);
    return newIndex;
}

void AudioEngine::removeTrack(int index) {
    int current = m_trackCount.load(std::memory_order_acquire);
    if (current <= 1) return;
    if (index < 0 || index >= current) return;
    for (int i = index; i < current - 1; ++i) {
        AudioBuffer* toFree = m_tracks[i].audio.load(std::memory_order_relaxed);
        queueAudioBufferDelete(toFree);
        m_tracks[i].name = m_tracks[i + 1].name;
        m_tracks[i].audioFilePath = m_tracks[i + 1].audioFilePath;
        m_tracks[i].volume.store(m_tracks[i + 1].volume.load(), std::memory_order_relaxed);
        m_tracks[i].pan.store(m_tracks[i + 1].pan.load(), std::memory_order_relaxed);
        m_tracks[i].muted.store(m_tracks[i + 1].muted.load(), std::memory_order_relaxed);
        m_tracks[i].soloed.store(m_tracks[i + 1].soloed.load(), std::memory_order_relaxed);
        m_tracks[i].armed.store(m_tracks[i + 1].armed.load(), std::memory_order_relaxed);
        m_tracks[i].audio.store(m_tracks[i + 1].audio.load(), std::memory_order_relaxed);

        m_tracks[i + 1].audio.store(nullptr, std::memory_order_relaxed);
        m_tracks[i].clipStart.store(m_tracks[i + 1].clipStart.load(), std::memory_order_relaxed);
        m_tracks[i].peakL.store(0.0f, std::memory_order_relaxed);
        m_tracks[i].peakR.store(0.0f, std::memory_order_relaxed);
        std::swap(m_pluginChains[i], m_pluginChains[i + 1]);
    }
    int newCount = current - 1;
    AudioBuffer* last = m_tracks[newCount].audio.load(std::memory_order_relaxed);
    queueAudioBufferDelete(last);
    m_tracks[newCount].audio.store(nullptr, std::memory_order_relaxed);

    m_tracks[newCount].name.clear();
    m_tracks[newCount].audioFilePath.clear();
    m_tracks[newCount].volume.store(1.0f, std::memory_order_relaxed);
    m_tracks[newCount].pan.store(0.0f, std::memory_order_relaxed);
    m_tracks[newCount].muted.store(false, std::memory_order_relaxed);
    m_tracks[newCount].soloed.store(false, std::memory_order_relaxed);
    m_tracks[newCount].armed.store(false, std::memory_order_relaxed);
    m_tracks[newCount].clipStart.store(0, std::memory_order_relaxed);
    m_tracks[newCount].peakL.store(0.0f, std::memory_order_relaxed);
    m_tracks[newCount].peakR.store(0.0f, std::memory_order_relaxed);

    if (m_pluginChains[newCount]) {
        m_pluginChains[newCount] = std::make_unique<PluginChain>(SAMPLE_RATE, BLOCK_SIZE, newCount);
    }

    m_trackCount.store(newCount, std::memory_order_release);
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

    // Free old buffer on this track before overwriting. Deferred delete
    // to avoid UAF: the audio thread might be reading oldBuf right now.
    AudioBuffer* oldBuf = m_tracks[trackIndex].audio.load(std::memory_order_acquire);
    queueAudioBufferDelete(oldBuf);

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
    // Deferred delete: the audio thread might be reading oldBuf right now.
    AudioBuffer* oldBuf = m_tracks[dst].audio.exchange(buf);
    queueAudioBufferDelete(oldBuf);
    m_tracks[src].clipStart.store(0, std::memory_order_release);
}

PluginChain* AudioEngine::getPluginChain(int trackIndex) {
    if (trackIndex == -1) return m_masterPluginChain.get();
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return nullptr;
    return m_pluginChains[trackIndex].get();
}

// RAII guard that sets/clears the audio-callback thread flag. Even on
// early return or exception (we don't throw today, but be defensive
// against future plugin changes) the destructor restores the state.
struct AudioScope {
    AudioScope()  { PluginChain::enterAudioThread(); }
    ~AudioScope() { PluginChain::leaveAudioThread(); }
};

void AudioEngine::audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioScope scope;  // marks thread as audio for clap.thread-check
    AudioEngine* engine = static_cast<AudioEngine*>(pDevice->pUserData);
    float* out = static_cast<float*>(pOutput);
    const float* in = static_cast<const float*>(pInput);

    // HOT PATH: zero allocations, no mutex, no ostringstream. Logs only
    // on the first call + every 5000 calls (~26s) to keep the audio
    // thread under the xrun threshold. dlog() allocates inside its
    // ostringstream but at this cadence (1/5000 blocks) that's ~1
    // alloc every 26s — negligible.
    static thread_local int tl_audioCbCount = 0;
    if (++tl_audioCbCount == 1 || tl_audioCbCount % 5000 == 0) {
        dlog("audioCallback call=" + std::to_string(tl_audioCbCount) +
             " frames=" + std::to_string(frameCount));
    }

    // Clamp frameCount to BLOCK_SIZE — our stack scratch buffers are
    // sized to this constant. If the backend delivers a larger block
    // we truncate silently (better than stack corruption).
    if (frameCount > BLOCK_SIZE) frameCount = BLOCK_SIZE;

    std::memset(out, 0, (size_t)frameCount * 2 * sizeof(float));

    bool playing = engine->m_transport.playing.load(std::memory_order_acquire);
    uint64_t basePlayhead = engine->m_transport.playhead.load(std::memory_order_relaxed);
    int trackCount = engine->m_trackCount.load(std::memory_order_acquire);

    if (!playing && basePlayhead == 0) return;

    // REAL-TIME: never take a mutex. addTrack/removeTrack may mutate
    // m_tracks/m_pluginChains on the JS thread but the per-track fields
    // are all atomic and the unique_ptrs in m_pluginChains only get
    // swapped (never destroyed until engine shutdown). At most we get
    // one block of audio routed to the wrong track — inaudible.

    for (int t = 0; t < trackCount; ++t) {
        Track& track = engine->m_tracks[t];
        if (track.muted.load(std::memory_order_relaxed)) continue;

        // PLANAR PACKED scratch: [L0..LN-1 | R0..RN-1]. This is the
        // exact layout PluginChain::process expects (input==output is
        // one contiguous buffer). By keeping it planar from the start
        // we avoid the pack/unpack loops the old code did per plugin
        // chain. Reading from AudioBuffer::dataL/dataR is two memcpys
        // directly into planar halves — no interleave needed.
        float planar[BLOCK_SIZE * 2];
        bool hasAudio = false;

        if (playing) {
            AudioBuffer* buf = track.audio.load(std::memory_order_acquire);
            uint64_t cs = track.clipStart.load(std::memory_order_relaxed);
            if (buf && basePlayhead >= cs) {
                uint64_t offset = basePlayhead - cs;
                if (offset < buf->frames) {
                    ma_uint32 copyFrames = frameCount;
                    if (offset + copyFrames > buf->frames)
                        copyFrames = (ma_uint32)(buf->frames - offset);
                    std::memcpy(planar,                    &buf->dataL[offset], copyFrames * sizeof(float));
                    std::memcpy(planar + frameCount,       &buf->dataR[offset], copyFrames * sizeof(float));
                    if (copyFrames < frameCount) {
                        std::memset(planar + copyFrames,                  0, (size_t)(frameCount - copyFrames) * sizeof(float));
                        std::memset(planar + frameCount + copyFrames,     0, (size_t)(frameCount - copyFrames) * sizeof(float));
                    }
                    hasAudio = true;
                }
            }
        }
        if (!hasAudio) {
            std::memset(planar, 0, sizeof(planar));
        }

        // Add armed input (still interleaved in `in`)
        if (track.armed.load(std::memory_order_relaxed) && in) {
            hasAudio = true;
            for (ma_uint32 f = 0; f < frameCount; ++f) {
                planar[f]              += in[f * 2 + 0];
                planar[f + frameCount] += in[f * 2 + 1];
                track.bufferL[f] = in[f * 2 + 0];
                track.bufferR[f] = in[f * 2 + 1];
            }
        }

        // Process per-track plugin chain IN-PLACE on the planar buffer.
        // hasPlugins() is lock-free (reads the snapshot atomically).
        auto& chain = engine->m_pluginChains[t];
        if (hasAudio && chain && chain->hasPlugins()) {
            // PluginChain::process(input, output, frames, channels):
            //   input  = planar  (L half at [0..N-1], R half at [N..2N-1])
            //   output = planar  (in-place processing)
            // Mono plugins only touch [0..N-1]; stereo plugins touch both.
            chain->process(planar, planar, (int)frameCount, 2);
        }

        // FUSED: gain + interleave to `out` + peak detection in ONE loop.
        // Hoisting the gains eliminates 512 double-precision ops per block
        // and 16*256 redundant min() calls. The branchless `t<0?-t:t` is
        // faster than std::abs for the non-NaN path.
        const float vol = track.volume.load(std::memory_order_relaxed);
        const float panVal = track.pan.load(std::memory_order_relaxed);
        const float leftGain  = (panVal < 0.0f ? 1.0f : 1.0f - panVal) * vol;
        const float rightGain = (panVal > 0.0f ? 1.0f : 1.0f + panVal) * vol;

        float peakL = 0.0f, peakR = 0.0f;
        float* outPtr = out;
        const float* inL = planar;
        const float* inR = planar + frameCount;
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            const float tL = inL[f] * leftGain;
            const float tR = inR[f] * rightGain;
            outPtr[0] += tL;
            outPtr[1] += tR;
            outPtr += 2;
            const float aL = tL < 0.0f ? -tL : tL;
            const float aR = tR < 0.0f ? -tR : tR;
            if (aL > peakL) peakL = aL;
            if (aR > peakR) peakR = aR;
        }

        // Peak envelope (hold with slow decay)
        float oldPL = track.peakL.load(std::memory_order_relaxed);
        track.peakL.store(peakL > oldPL ? peakL : oldPL * 0.999f, std::memory_order_relaxed);
        float oldPR = track.peakR.load(std::memory_order_relaxed);
        track.peakR.store(peakR > oldPR ? peakR : oldPR * 0.999f, std::memory_order_relaxed);
    }

    // Master plugin chain. Read hasPlugins() lock-free here too.
    auto& mstChain = engine->m_masterPluginChain;
    if (mstChain && mstChain->hasPlugins()) {
        // Master bus is interleaved in `out`. PluginChain wants planar
        // [L | R] — we DO need one pre-pass split, but we can do it into
        // a stack scratch and then re-interleave after, sharing the
        // master-volume+peak pass at the end.
        float planar[BLOCK_SIZE * 2];
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            planar[f]              = out[f * 2 + 0];
            planar[f + frameCount] = out[f * 2 + 1];
        }
        mstChain->process(planar, planar, (int)frameCount, 2);
        // Fused: re-interleave + master vol + master peak in ONE loop.
        const float masterVol = engine->m_master.volume.load(std::memory_order_relaxed);
        float masterPeakL = 0.0f, masterPeakR = 0.0f;
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            const float mL = planar[f]              * masterVol;
            const float mR = planar[f + frameCount] * masterVol;
            out[f * 2 + 0] = mL;
            out[f * 2 + 1] = mR;
            const float aL = mL < 0.0f ? -mL : mL;
            const float aR = mR < 0.0f ? -mR : mR;
            if (aL > masterPeakL) masterPeakL = aL;
            if (aR > masterPeakR) masterPeakR = aR;
        }
        float oldML = engine->m_master.peakL.load(std::memory_order_relaxed);
        engine->m_master.peakL.store(masterPeakL > oldML ? masterPeakL : oldML * 0.999f, std::memory_order_relaxed);
        float oldMR = engine->m_master.peakR.load(std::memory_order_relaxed);
        engine->m_master.peakR.store(masterPeakR > oldMR ? masterPeakR : oldMR * 0.999f, std::memory_order_relaxed);
    } else {
        // No master chain: just apply vol + compute peaks in one fused pass.
        const float masterVol = engine->m_master.volume.load(std::memory_order_relaxed);
        float masterPeakL = 0.0f, masterPeakR = 0.0f;
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            const float mL = out[f * 2 + 0] * masterVol;
            const float mR = out[f * 2 + 1] * masterVol;
            out[f * 2 + 0] = mL;
            out[f * 2 + 1] = mR;
            const float aL = mL < 0.0f ? -mL : mL;
            const float aR = mR < 0.0f ? -mR : mR;
            if (aL > masterPeakL) masterPeakL = aL;
            if (aR > masterPeakR) masterPeakR = aR;
        }
        float oldML = engine->m_master.peakL.load(std::memory_order_relaxed);
        engine->m_master.peakL.store(masterPeakL > oldML ? masterPeakL : oldML * 0.999f, std::memory_order_relaxed);
        float oldMR = engine->m_master.peakR.load(std::memory_order_relaxed);
        engine->m_master.peakR.store(masterPeakR > oldMR ? masterPeakR : oldMR * 0.999f, std::memory_order_relaxed);
    }

    if (playing) {
        // relaxed: the playhead is monotonic and the UI thread reads it
        // with its own acquire on the next tick. We don't publish any
        // other data through this atomic.
        uint64_t newPlayhead = engine->m_transport.playhead.fetch_add(frameCount, std::memory_order_relaxed) + frameCount;

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
            engine->m_transport.playing.store(false, std::memory_order_relaxed);
    }

    // Drain deferred AudioBuffer deletes and Snapshot deletes. By this
    // point every in-flight pointer from THIS block has been released,
    // so any buffer/snapshot queued during this or an earlier block is
    // safe to free.
    engine->drainPendingAudioBufferDeletes();
    PluginChain::drainPendingDeletes();
}

void AudioEngine::setTrackAudioPath(int index, const char* path) {
    if (index >= 0 && index < MAX_TRACKS)
        m_tracks[index].audioFilePath = path ? path : "";
}

std::string AudioEngine::getTrackAudioPath(int index) {
    if (index >= 0 && index < MAX_TRACKS)
        return m_tracks[index].audioFilePath;
    return "";
}

// Queue an AudioBuffer for deferred deletion. The audio thread drains
// the queue at the end of each block. This is the ONLY safe way to free
// an AudioBuffer that the audio callback might be reading concurrently
// via a local copy of `track.audio`.
void AudioEngine::queueAudioBufferDelete(AudioBuffer* buf) {
    if (!buf) return;
    std::lock_guard<std::mutex> lk(m_pendingDeleteMutex);
    m_pendingAudioBufferDeletes.push_back(buf);
}

// Called by the audio thread at the end of each block. By this point,
// every audio thread in-flight load() of `track.audio` from THIS block
// has either returned (so the local buf pointer is no longer used) or
// is in the next block (and will see the NEW buffer, not the deleted
// one). All buffers queued in earlier blocks are now safe to free.
void AudioEngine::drainPendingAudioBufferDeletes() {
    std::vector<AudioBuffer*> toFree;
    {
        std::lock_guard<std::mutex> lk(m_pendingDeleteMutex);
        if (m_pendingAudioBufferDeletes.empty()) return;
        toFree.swap(m_pendingAudioBufferDeletes);
    }
    for (AudioBuffer* p : toFree) delete p;
}

bool AudioEngine::renderOffline(const char* wavPath, int sampleRate, int bitDepth) {
    int tc = m_trackCount.load(std::memory_order_relaxed);

    // Calculate total duration: max(clipStart + audioFrames) across all tracks
    uint64_t totalFrames = 0;
    bool anyAudio = false;
    for (int t = 0; t < tc; ++t) {
        AudioBuffer* buf = m_tracks[t].audio.load(std::memory_order_acquire);
        if (buf && buf->frames > 0) {
            anyAudio = true;
            uint64_t end = m_tracks[t].clipStart.load(std::memory_order_relaxed) + buf->frames;
            if (end > totalFrames) totalFrames = end;
        }
    }
    if (!anyAudio || totalFrames == 0) return false;

    // Allocate output buffer (stereo interleaved float)
    std::vector<float> outBuffer(totalFrames * 2, 0.0f);

    uint64_t playhead = 0;
    while (playhead < totalFrames) {
        uint64_t remaining = totalFrames - playhead;
        uint32_t block = (uint32_t)std::min<uint64_t>(remaining, BLOCK_SIZE);
        float* out = outBuffer.data() + playhead * 2;

        for (int t = 0; t < tc; ++t) {
            Track& track = m_tracks[t];
            if (track.muted.load(std::memory_order_relaxed)) continue;

            float blockL[BLOCK_SIZE], blockR[BLOCK_SIZE];
            bool hasAudio = false;

            AudioBuffer* buf = track.audio.load(std::memory_order_acquire);
            uint64_t cs = track.clipStart.load(std::memory_order_relaxed);
            if (buf && playhead >= cs) {
                uint64_t offset = playhead - cs;
                if (offset < buf->frames) {
                    uint32_t copyFrames = block;
                    if (offset + copyFrames > buf->frames)
                        copyFrames = (uint32_t)(buf->frames - offset);
                    std::memcpy(blockL, &buf->dataL[offset], copyFrames * sizeof(float));
                    std::memcpy(blockR, &buf->dataR[offset], copyFrames * sizeof(float));
                    hasAudio = true;
                    if (copyFrames < block) {
                        std::memset(blockL + copyFrames, 0, (block - copyFrames) * sizeof(float));
                        std::memset(blockR + copyFrames, 0, (block - copyFrames) * sizeof(float));
                    }
                } else {
                    std::memset(blockL, 0, sizeof(float) * BLOCK_SIZE);
                    std::memset(blockR, 0, sizeof(float) * BLOCK_SIZE);
                }
            } else {
                std::memset(blockL, 0, sizeof(float) * BLOCK_SIZE);
                std::memset(blockR, 0, sizeof(float) * BLOCK_SIZE);
            }

            // Process plugins
            auto& chain = m_pluginChains[t];
            if (chain && chain->getPluginCount() > 0 && hasAudio) {
                float planar[BLOCK_SIZE * 2];
                for (uint32_t f = 0; f < block; ++f) {
                    planar[f] = blockL[f];
                    planar[f + block] = blockR[f];
                }
                chain->process(planar, planar, (int)block, 2);
                for (uint32_t f = 0; f < block; ++f) {
                    blockL[f] = planar[f];
                    blockR[f] = planar[f + block];
                }
            }

            float vol = track.volume.load(std::memory_order_relaxed);
            float panVal = track.pan.load(std::memory_order_relaxed);
            double leftGain = std::min(1.0, 1.0 - panVal) * vol;
            double rightGain = std::min(1.0, 1.0 + panVal) * vol;

            for (uint32_t f = 0; f < block; ++f) {
                out[f * 2 + 0] += (float)(blockL[f] * leftGain);
                out[f * 2 + 1] += (float)(blockR[f] * rightGain);
            }
        }

        // Master plugin chain
        if (m_masterPluginChain && m_masterPluginChain->getPluginCount() > 0) {
            float planar[BLOCK_SIZE * 2];
            for (uint32_t f = 0; f < block; ++f) {
                planar[f] = out[f * 2 + 0];
                planar[f + block] = out[f * 2 + 1];
            }
            m_masterPluginChain->process(planar, planar, (int)block, 2);
            for (uint32_t f = 0; f < block; ++f) {
                out[f * 2 + 0] = planar[f];
                out[f * 2 + 1] = planar[f + block];
            }
        }

        // Master volume
        float masterVol = m_master.volume.load(std::memory_order_relaxed);
        if (masterVol != 1.0f) {
            for (uint32_t f = 0; f < block * 2; ++f)
                out[f] *= masterVol;
        }

        playhead += block;
    }

    // Write WAV
    if (bitDepth == 32)
        return WavWriter::writeFloat32(wavPath, outBuffer.data(), totalFrames, 2, sampleRate);
    else
        return WavWriter::write16bit(wavPath, outBuffer.data(), totalFrames, 2, sampleRate);
}
