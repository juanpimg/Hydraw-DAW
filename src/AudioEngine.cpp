#include "AudioEngine.h"
#include "Export/WavWriter.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

AudioEngine::AudioEngine(hydraw::ILogSink* logSink, hydraw::IHostExtensions* hostExt)
    : m_logSink(logSink), m_hostExt(hostExt) {
    m_trackClips.resize(MAX_TRACKS);
    for (int i = 0; i < MAX_TRACKS; ++i) {
        m_clipsSnapshot[i].store(nullptr, std::memory_order_relaxed);
        m_autoSnapshot[i].store(nullptr, std::memory_order_relaxed);
    }
    for (int b = 0; b < MAX_BUSES; ++b) {
        m_auxBuses[b].name = "Aux " + std::to_string(b + 1);
    }
}

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
        m_pluginChains[t] = std::make_unique<PluginChain>(SAMPLE_RATE, BLOCK_SIZE, t, m_hostExt, m_logSink);
    }
    m_masterPluginChain = std::make_unique<PluginChain>(SAMPLE_RATE, BLOCK_SIZE, -1, m_hostExt, m_logSink);

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

void AudioEngine::setBPM(float bpm) {
    if (bpm < 20.0f) bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    m_bpm.store(bpm, std::memory_order_relaxed);
}

float AudioEngine::getBPM() const {
    return m_bpm.load(std::memory_order_relaxed);
}

void AudioEngine::setTimeSignature(int numerator, int denominator) {
    if (numerator < 1) numerator = 1;
    if (numerator > 32) numerator = 32;
    if (denominator < 1) denominator = 1;
    if (denominator > 32) denominator = 32;
    m_timeSigNum.store(numerator, std::memory_order_relaxed);
    m_timeSigDen.store(denominator, std::memory_order_relaxed);
}

int AudioEngine::getTimeSigNum() const {
    return m_timeSigNum.load(std::memory_order_relaxed);
}

int AudioEngine::getTimeSigDen() const {
    return m_timeSigDen.load(std::memory_order_relaxed);
}

void AudioEngine::sampleToBarBeat(uint64_t sample, int& bar, int& beat, double& beatFrac) const {
    double bpm = (double)m_bpm.load(std::memory_order_relaxed);
    if (bpm < 1.0) bpm = 120.0;
    int num = m_timeSigNum.load(std::memory_order_relaxed);
    if (num < 1) num = 4;
    // samples per beat (quarter note)
    double samplesPerBeat = (double)SAMPLE_RATE * 60.0 / bpm;
    double totalBeats = (double)sample / samplesPerBeat;
    if (totalBeats < 0.0) totalBeats = 0.0;
    int wholeBeat = (int)totalBeats;
    beatFrac = totalBeats - (double)wholeBeat;
    bar = wholeBeat / num + 1;     // 1-based
    beat = wholeBeat % num + 1;    // 1-based
}

uint64_t AudioEngine::barBeatToSample(int bar, int beat) const {
    double bpm = (double)m_bpm.load(std::memory_order_relaxed);
    if (bpm < 1.0) bpm = 120.0;
    int num = m_timeSigNum.load(std::memory_order_relaxed);
    if (num < 1) num = 4;
    if (bar < 1) bar = 1;
    if (beat < 1) beat = 1;
    int64_t totalBeats = (int64_t)(bar - 1) * num + (beat - 1);
    double samplesPerBeat = (double)SAMPLE_RATE * 60.0 / bpm;
    return (uint64_t)((double)totalBeats * samplesPerBeat);
}

void AudioEngine::setLoopEnabled(bool enabled) {
    m_loopEnabled.store(enabled, std::memory_order_relaxed);
}
bool AudioEngine::getLoopEnabled() const {
    return m_loopEnabled.load(std::memory_order_relaxed);
}
void AudioEngine::setLoopRange(uint64_t start, uint64_t end) {
    if (end < start) std::swap(start, end);
    m_loopStart.store(start, std::memory_order_relaxed);
    m_loopEnd.store(end, std::memory_order_relaxed);
}
uint64_t AudioEngine::getLoopStart() const { return m_loopStart.load(std::memory_order_relaxed); }
uint64_t AudioEngine::getLoopEnd() const { return m_loopEnd.load(std::memory_order_relaxed); }
void AudioEngine::setPunchRange(uint64_t start, uint64_t end) {
    if (end < start) std::swap(start, end);
    m_punchStart.store(start, std::memory_order_relaxed);
    m_punchEnd.store(end, std::memory_order_relaxed);
}
uint64_t AudioEngine::getPunchStart() const { return m_punchStart.load(std::memory_order_relaxed); }
uint64_t AudioEngine::getPunchEnd() const { return m_punchEnd.load(std::memory_order_relaxed); }

MasterBus* AudioEngine::getMaster() { return &m_master; }

AuxBus* AudioEngine::getAuxBus(int index) {
    if (index < 0 || index >= MAX_BUSES) return nullptr;
    return &m_auxBuses[index];
}

void AudioEngine::setSendLevel(int trackIndex, int busIndex, float level) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    if (busIndex < 0 || busIndex >= MAX_BUSES) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    m_tracks[trackIndex].sends[busIndex].store(level, std::memory_order_relaxed);
}

float AudioEngine::getSendLevel(int trackIndex, int busIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0.0f;
    if (busIndex < 0 || busIndex >= MAX_BUSES) return 0.0f;
    return m_tracks[trackIndex].sends[busIndex].load(std::memory_order_relaxed);
}

int AudioEngine::addTrack() {
    int current = m_trackCount.load();
    if (current >= MAX_TRACKS) return -1;
    int newIndex = current;
    m_tracks[newIndex].name = "Track " + std::to_string(newIndex + 1);
    m_tracks[newIndex].audioFilePath.clear();
    m_tracks[newIndex].volume.store(1.0f, std::memory_order_relaxed);
    m_tracks[newIndex].pan.store(0.0f, std::memory_order_relaxed);
    m_tracks[newIndex].muted.store(false, std::memory_order_relaxed);
    m_tracks[newIndex].soloed.store(false, std::memory_order_relaxed);
    m_tracks[newIndex].armed.store(false, std::memory_order_relaxed);
    m_tracks[newIndex].audio.store(nullptr, std::memory_order_relaxed);
    m_tracks[newIndex].clipStart.store(0, std::memory_order_relaxed);
    m_tracks[newIndex].peakL.store(0.0f, std::memory_order_relaxed);
    m_tracks[newIndex].peakR.store(0.0f, std::memory_order_relaxed);
    for (int b = 0; b < MAX_BUSES; ++b) {
        m_tracks[newIndex].sends[b].store(0.0f, std::memory_order_relaxed);
    }
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
        m_pluginChains[newCount] = std::make_unique<PluginChain>(SAMPLE_RATE, BLOCK_SIZE, newCount, m_hostExt, m_logSink);
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

    // Dynamic peak count: 1 peak per 256 frames (~5.3ms at 48kHz), min 1
    int numPeaks = (int)(framesRead / 256);
    if (numPeaks < 1) numPeaks = 1;
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
    m_tracks[trackIndex].audioFilePath = path ? path : "";
    // NOTE: clipStart is intentionally NOT reset here. The caller
    // (interactive load, project load, etc.) decides whether to
    // reset it. Project load restores the saved value AFTER calling
    // this function; interactive loads rely on the default of 0 set
    // in addTrack()/removeTrack().
    return true;
}

void AudioEngine::setClipStart(int trackIndex, uint64_t pos) {
    if (trackIndex >= 0 && trackIndex < m_trackCount.load())
        m_tracks[trackIndex].clipStart.store(pos, std::memory_order_release);
}

void AudioEngine::setClipStart(int trackIndex, int clipIndex, uint64_t pos) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return;
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0) {
        m_tracks[trackIndex].clipStart.store(pos, std::memory_order_release);
    } else if (hasPrimary) {
        int idx = clipIndex - 1;
        if (idx >= 0 && idx < (int)m_trackClips[trackIndex].size())
            m_trackClips[trackIndex][idx].clipStart = pos;
    } else {
        if (clipIndex >= 0 && clipIndex < (int)m_trackClips[trackIndex].size())
            m_trackClips[trackIndex][clipIndex].clipStart = pos;
    }
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

void AudioEngine::publishClipsSnapshot(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    auto* snap = new ClipsSnapshot();
    {
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        snap->clips = m_trackClips[trackIndex];
    }
    auto* old = m_clipsSnapshot[trackIndex].exchange(snap, std::memory_order_acq_rel);
    if (old) {
        std::lock_guard<std::mutex> lk(m_pendingClipsSnapshotMutex);
        m_pendingClipsSnapshots.push_back(old);
    }
}

// When the first extra clip is added, move the primary clip into the
// extras vector and clear the legacy fields. When the last extra is
// removed, restore the primary clip from the first extra.
void AudioEngine::rebuildPrimaryFromClips(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    auto& clips = m_trackClips[trackIndex];
    if (clips.empty()) {
        // Move nothing; primary stays
    } else if (m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr) {
        // Promote primary to first extra
        Clip c;
        c.buffer = m_tracks[trackIndex].audio.exchange(nullptr, std::memory_order_acq_rel);
        c.clipStart = m_tracks[trackIndex].clipStart.load(std::memory_order_relaxed);
        c.filePath = m_tracks[trackIndex].audioFilePath;
        c.fadeIn.store(m_tracks[trackIndex].primaryFadeIn.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        c.fadeOut.store(m_tracks[trackIndex].primaryFadeOut.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
        m_tracks[trackIndex].clipStart.store(0, std::memory_order_relaxed);
        m_tracks[trackIndex].primaryFadeIn.store(0, std::memory_order_relaxed);
        m_tracks[trackIndex].primaryFadeOut.store(0, std::memory_order_relaxed);
        clips.insert(clips.begin(), c);
    } else if (clips.size() == 1) {
        // Demote first extra to primary
        const Clip& c = clips[0];
        m_tracks[trackIndex].audio.store(c.buffer, std::memory_order_release);
        m_tracks[trackIndex].clipStart.store(c.clipStart, std::memory_order_relaxed);
        m_tracks[trackIndex].audioFilePath = c.filePath;
        m_tracks[trackIndex].primaryFadeIn.store(c.fadeIn.load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
        m_tracks[trackIndex].primaryFadeOut.store(c.fadeOut.load(std::memory_order_relaxed),
                                                   std::memory_order_relaxed);
        clips.erase(clips.begin());
    }
}

int AudioEngine::addClip(int trackIndex, const char* path, uint64_t start) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return -1;
    if (!path) return -1;
    // Load the WAV into an AudioBuffer
    auto* buf = new AudioBuffer();
    {
        ma_decoder dec;
        ma_decoder_config decCfg = ma_decoder_config_init(ma_format_f32, 2, SAMPLE_RATE);
        if (ma_decoder_init_file(path, &decCfg, &dec) != MA_SUCCESS) {
            delete buf;
            return -1;
        }
        ma_uint64 totalFrames = 0;
        ma_decoder_get_length_in_pcm_frames(&dec, &totalFrames);
        if (totalFrames == 0) totalFrames = 1;
        buf->dataL.resize((size_t)totalFrames);
        buf->dataR.resize((size_t)totalFrames);
        ma_uint64 got = 0;
        float tmp[4096 * 2];
        size_t pos = 0;
        while (pos < (size_t)totalFrames) {
            ma_uint64 toRead = std::min((ma_uint64)(4096), (ma_uint64)(totalFrames - pos));
            ma_decoder_read_pcm_frames(&dec, tmp, (ma_uint32)toRead, &got);
            if (got == 0) break;
            for (ma_uint64 i = 0; i < got; ++i) {
                buf->dataL[pos + i] = tmp[i * 2 + 0];
                buf->dataR[pos + i] = tmp[i * 2 + 1];
            }
            pos += (size_t)got;
        }
        buf->frames = (uint64_t)pos;
        ma_decoder_uninit(&dec);
    }
    if (buf->frames == 0) { delete buf; return -1; }
    // Peak cache: 1 peak per 256 frames (~5.3ms @ 48kHz), min 1.
    // No maximum cap — all clips share the same temporal resolution
    // regardless of duration, guaranteeing uniform visual density.
    size_t numP = (size_t)(buf->frames / 256);
    if (numP < 1) numP = 1;
    buf->peakCache.resize(numP);
    for (size_t i = 0; i < numP; ++i) {
        size_t start = (size_t)((uint64_t)i * buf->frames / numP);
        size_t end = (size_t)((uint64_t)(i + 1) * buf->frames / numP);
        if (end > (size_t)buf->frames) end = (size_t)buf->frames;
        if (end <= start) end = start + 1;
        if (end > (size_t)buf->frames) end = (size_t)buf->frames;
        float peak = 0.0f;
        for (size_t s = start; s < end; ++s) {
            float a = buf->dataL[s] < 0 ? -buf->dataL[s] : buf->dataL[s];
            float b = buf->dataR[s] < 0 ? -buf->dataR[s] : buf->dataR[s];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        buf->peakCache[i] = peak;
    }
    // Add to clips
    {
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        // If primary slot is free, use it
        if (m_tracks[trackIndex].audio.load(std::memory_order_relaxed) == nullptr &&
            m_trackClips[trackIndex].empty()) {
            m_tracks[trackIndex].audio.store(buf, std::memory_order_release);
            m_tracks[trackIndex].clipStart.store(start, std::memory_order_relaxed);
            m_tracks[trackIndex].audioFilePath = path;
        } else {
            // Otherwise promote primary if needed, then append
            if (m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr) {
                Clip prim;
                prim.buffer = m_tracks[trackIndex].audio.exchange(nullptr, std::memory_order_acq_rel);
                prim.clipStart = m_tracks[trackIndex].clipStart.load(std::memory_order_relaxed);
                prim.filePath = m_tracks[trackIndex].audioFilePath;
                prim.fadeIn.store(m_tracks[trackIndex].primaryFadeIn.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
                prim.fadeOut.store(m_tracks[trackIndex].primaryFadeOut.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
                m_trackClips[trackIndex].insert(m_trackClips[trackIndex].begin(), prim);
                m_tracks[trackIndex].clipStart.store(0, std::memory_order_relaxed);
                m_tracks[trackIndex].primaryFadeIn.store(0, std::memory_order_relaxed);
                m_tracks[trackIndex].primaryFadeOut.store(0, std::memory_order_relaxed);
            }
            Clip c;
            c.buffer = buf;
            c.clipStart = start;
            c.filePath = path;
            m_trackClips[trackIndex].push_back(c);
        }
    }
    publishClipsSnapshot(trackIndex);
    return (int)getClipCount(trackIndex) - 1;
}

bool AudioEngine::removeClip(int trackIndex, int clipIndex) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return false;
    if (clipIndex < 0) return false;
    AudioBuffer* toDelete = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        auto& clips = m_trackClips[trackIndex];
        if (m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr) {
            // Primary is occupied
            if (clipIndex == 0) {
                toDelete = m_tracks[trackIndex].audio.exchange(nullptr, std::memory_order_acq_rel);
                m_tracks[trackIndex].clipStart.store(0, std::memory_order_relaxed);
                m_tracks[trackIndex].audioFilePath.clear();
            } else {
                int idx = clipIndex - 1;
                if (idx >= (int)clips.size()) return false;
                toDelete = clips[idx].buffer;
                clips.erase(clips.begin() + idx);
            }
        } else {
            if (clipIndex >= (int)clips.size()) return false;
            toDelete = clips[clipIndex].buffer;
            clips.erase(clips.begin() + clipIndex);
            // If only one extra remains, demote to primary
            if (clips.size() == 1) {
                const Clip& c = clips[0];
                m_tracks[trackIndex].audio.store(c.buffer, std::memory_order_release);
                m_tracks[trackIndex].clipStart.store(c.clipStart, std::memory_order_relaxed);
                m_tracks[trackIndex].audioFilePath = c.filePath;
                m_tracks[trackIndex].primaryFadeIn.store(c.fadeIn.load(std::memory_order_relaxed),
                                                          std::memory_order_relaxed);
                m_tracks[trackIndex].primaryFadeOut.store(c.fadeOut.load(std::memory_order_relaxed),
                                                           std::memory_order_relaxed);
                clips.erase(clips.begin());
            }
        }
    }
    if (toDelete) queueAudioBufferDelete(toDelete);
    publishClipsSnapshot(trackIndex);
    return true;
}

int AudioEngine::getClipCount(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    int primary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) ? 1 : 0;
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    return primary + (int)m_trackClips[trackIndex].size();
}

uint64_t AudioEngine::getClipStart(int trackIndex, int clipIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0)
        return m_tracks[trackIndex].clipStart.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    int idx = hasPrimary ? clipIndex - 1 : clipIndex;
    if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return 0;
    return m_trackClips[trackIndex][idx].clipStart;
}

uint64_t AudioEngine::getClipFrames(int trackIndex, int clipIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0) {
        AudioBuffer* b = m_tracks[trackIndex].audio.load(std::memory_order_relaxed);
        return b ? b->frames : 0;
    }
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    int idx = hasPrimary ? clipIndex - 1 : clipIndex;
    if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return 0;
    AudioBuffer* b = m_trackClips[trackIndex][idx].buffer;
    return b ? b->frames : 0;
}

std::string AudioEngine::getClipPath(int trackIndex, int clipIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return "";
    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0)
        return m_tracks[trackIndex].audioFilePath;
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    int idx = hasPrimary ? clipIndex - 1 : clipIndex;
    if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return "";
    return m_trackClips[trackIndex][idx].filePath;
}

std::vector<float> AudioEngine::getClipPeakCache(int trackIndex, int clipIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return {};

    auto computeCache = [](AudioBuffer* b) -> std::vector<float> {
        if (!b) return {};
        size_t expected = (size_t)(b->frames / 256);
        if (expected < 1) expected = 1;
        if (!b->peakCache.empty() && b->peakCache.size() == expected)
            return b->peakCache;
        // Recomputed: cache is empty, wrong size (old formula), or
        // the buffer was created through a path that didn't compute it.
        std::vector<float> pc(expected, 0.0f);
        if (b->frames == 0) return pc;
        for (size_t i = 0; i < expected; ++i) {
            size_t s0 = (size_t)((uint64_t)i * b->frames / expected);
            size_t s1 = (size_t)((uint64_t)(i + 1) * b->frames / expected);
            if (s1 <= s0) s1 = s0 + 1;
            if (s1 > (size_t)b->frames) s1 = (size_t)b->frames;
            float peak = 0.0f;
            for (size_t s = s0; s < s1; ++s) {
                float a = std::abs(b->dataL[s]);
                float bv = std::abs(b->dataR[s]);
                if (a > peak) peak = a;
                if (bv > peak) peak = bv;
            }
            pc[i] = peak;
        }
        // Write-back so subsequent calls are O(1) again.
        b->peakCache = pc;
        return pc;
    };

    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0) {
        AudioBuffer* b = m_tracks[trackIndex].audio.load(std::memory_order_relaxed);
        return computeCache(b);
    }

    std::lock_guard<std::mutex> lk(m_clipsMutex);
    int idx = hasPrimary ? clipIndex - 1 : clipIndex;
    if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return {};
    return computeCache(m_trackClips[trackIndex][idx].buffer);
}

bool AudioEngine::moveClip(int srcTrack, int srcClip, int dstTrack, int dstIndex, uint64_t newClipStart) {
    if (srcTrack < 0 || srcTrack >= m_trackCount.load()) return false;
    if (dstTrack < 0 || dstTrack >= m_trackCount.load()) return false;
    // Extract source
    Clip moved;
    bool srcWasPrimary = false;
    {
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        bool hasPrimary = m_tracks[srcTrack].audio.load(std::memory_order_relaxed) != nullptr;
        if (hasPrimary && srcClip == 0) {
            moved.buffer = m_tracks[srcTrack].audio.exchange(nullptr, std::memory_order_acq_rel);
            moved.clipStart = m_tracks[srcTrack].clipStart.load(std::memory_order_relaxed);
            moved.filePath = m_tracks[srcTrack].audioFilePath;
            moved.fadeIn.store(m_tracks[srcTrack].primaryFadeIn.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            moved.fadeOut.store(m_tracks[srcTrack].primaryFadeOut.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
            m_tracks[srcTrack].clipStart.store(0, std::memory_order_relaxed);
            m_tracks[srcTrack].primaryFadeIn.store(0, std::memory_order_relaxed);
            m_tracks[srcTrack].primaryFadeOut.store(0, std::memory_order_relaxed);
            m_tracks[srcTrack].audioFilePath.clear();
            srcWasPrimary = true;
        } else {
            int idx = hasPrimary ? srcClip - 1 : srcClip;
            if (idx < 0 || idx >= (int)m_trackClips[srcTrack].size()) return false;
            moved = m_trackClips[srcTrack][idx];
            m_trackClips[srcTrack].erase(m_trackClips[srcTrack].begin() + idx);
            // Demote single extra to primary
            if (!hasPrimary && m_trackClips[srcTrack].size() == 1) {
                const Clip& c = m_trackClips[srcTrack][0];
                m_tracks[srcTrack].audio.store(c.buffer, std::memory_order_release);
                m_tracks[srcTrack].clipStart.store(c.clipStart, std::memory_order_relaxed);
                m_tracks[srcTrack].audioFilePath = c.filePath;
                m_tracks[srcTrack].primaryFadeIn.store(c.fadeIn.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
                m_tracks[srcTrack].primaryFadeOut.store(c.fadeOut.load(std::memory_order_relaxed),
                                                         std::memory_order_relaxed);
                m_trackClips[srcTrack].erase(m_trackClips[srcTrack].begin());
            }
        }
    }
    moved.clipStart = newClipStart;
    // Insert into destination
    {
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        if (dstIndex <= 0 &&
            m_tracks[dstTrack].audio.load(std::memory_order_relaxed) == nullptr &&
            m_trackClips[dstTrack].empty()) {
            m_tracks[dstTrack].audio.store(moved.buffer, std::memory_order_release);
            m_tracks[dstTrack].clipStart.store(moved.clipStart, std::memory_order_relaxed);
            m_tracks[dstTrack].audioFilePath = moved.filePath;
        } else {
            if (m_tracks[dstTrack].audio.load(std::memory_order_relaxed) != nullptr) {
                Clip prim;
                prim.buffer = m_tracks[dstTrack].audio.exchange(nullptr, std::memory_order_acq_rel);
                prim.clipStart = m_tracks[dstTrack].clipStart.load(std::memory_order_relaxed);
                prim.filePath = m_tracks[dstTrack].audioFilePath;
                prim.fadeIn.store(m_tracks[dstTrack].primaryFadeIn.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
                prim.fadeOut.store(m_tracks[dstTrack].primaryFadeOut.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
                m_trackClips[dstTrack].insert(m_trackClips[dstTrack].begin(), prim);
                m_tracks[dstTrack].clipStart.store(0, std::memory_order_relaxed);
                m_tracks[dstTrack].primaryFadeIn.store(0, std::memory_order_relaxed);
                m_tracks[dstTrack].primaryFadeOut.store(0, std::memory_order_relaxed);
            }
            int insIdx = dstIndex - 1;
            if (insIdx < 0) insIdx = 0;
            if (insIdx > (int)m_trackClips[dstTrack].size()) insIdx = (int)m_trackClips[dstTrack].size();
            m_trackClips[dstTrack].insert(m_trackClips[dstTrack].begin() + insIdx, moved);
            // Demote if exactly one extra
            if (m_tracks[dstTrack].audio.load(std::memory_order_relaxed) == nullptr &&
                m_trackClips[dstTrack].size() == 1) {
                const Clip& c = m_trackClips[dstTrack][0];
                m_tracks[dstTrack].audio.store(c.buffer, std::memory_order_release);
                m_tracks[dstTrack].clipStart.store(c.clipStart, std::memory_order_relaxed);
                m_tracks[dstTrack].audioFilePath = c.filePath;
                m_tracks[dstTrack].primaryFadeIn.store(c.fadeIn.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
                m_tracks[dstTrack].primaryFadeOut.store(c.fadeOut.load(std::memory_order_relaxed),
                                                         std::memory_order_relaxed);
                m_trackClips[dstTrack].erase(m_trackClips[dstTrack].begin());
            }
        }
    }
    publishClipsSnapshot(srcTrack);
    publishClipsSnapshot(dstTrack);
    return true;
}

int AudioEngine::splitClipAt(int trackIndex, int clipIndex, uint64_t timelinePos) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return -1;
    // Determine the source clip's AudioBuffer, start, frames
    AudioBuffer* srcBuf = nullptr;
    uint64_t srcStart = 0;
    std::string srcPath;
    uint64_t srcFadeIn = 0, srcFadeOut = 0;
    bool isPrimary = false;
    int extraIdx = -1;
    {
        bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
        if (hasPrimary && clipIndex == 0) {
            srcBuf = m_tracks[trackIndex].audio.load(std::memory_order_relaxed);
            srcStart = m_tracks[trackIndex].clipStart.load(std::memory_order_relaxed);
            srcPath = m_tracks[trackIndex].audioFilePath;
            srcFadeIn = m_tracks[trackIndex].primaryFadeIn.load(std::memory_order_relaxed);
            srcFadeOut = m_tracks[trackIndex].primaryFadeOut.load(std::memory_order_relaxed);
            isPrimary = true;
        } else {
            std::lock_guard<std::mutex> lk(m_clipsMutex);
            int idx = hasPrimary ? clipIndex - 1 : clipIndex;
            if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return -1;
            srcBuf = m_trackClips[trackIndex][idx].buffer;
            srcStart = m_trackClips[trackIndex][idx].clipStart;
            srcPath = m_trackClips[trackIndex][idx].filePath;
            srcFadeIn = m_trackClips[trackIndex][idx].fadeIn.load(std::memory_order_relaxed);
            srcFadeOut = m_trackClips[trackIndex][idx].fadeOut.load(std::memory_order_relaxed);
            extraIdx = idx;
        }
    }
    if (!srcBuf) return -1;
    if (timelinePos <= srcStart) return -1;
    uint64_t splitOffset = timelinePos - srcStart;
    if (splitOffset >= srcBuf->frames) return -1;

    // ── Strategy: keep the primary as the FIRST HALF (no promotion
    // to extras). This way the UI's single-clip-per-track render
    // path still works for the most common case, and only the
    // second half becomes an extra clip. The primary's frames
    // are reduced to the first half; we still keep a fresh
    // AudioBuffer for the second half to avoid sharing the same
    // pointer (which would cause UAF if the primary is later
    // trimmed/replaced).
    auto* buf2 = new AudioBuffer();
    uint64_t firstFrames = splitOffset;
    uint64_t secondFrames = srcBuf->frames - splitOffset;

    // Build the second-half buffer (copy of splitOffset..end)
    buf2->dataL.resize((size_t)secondFrames);
    buf2->dataR.resize((size_t)secondFrames);
    std::memcpy(buf2->dataL.data(), &srcBuf->dataL[firstFrames], (size_t)secondFrames * sizeof(float));
    std::memcpy(buf2->dataR.data(), &srcBuf->dataR[firstFrames], (size_t)secondFrames * sizeof(float));
    buf2->frames = secondFrames;
    // Recompute peak cache (1 peak per 256 frames)
    size_t numP = (size_t)(secondFrames / 256);
    if (numP < 1) numP = 1;
    buf2->peakCache.resize(numP);
    for (size_t i = 0; i < numP; ++i) {
        size_t s0 = (size_t)((uint64_t)i * secondFrames / numP);
        size_t s1 = (size_t)((uint64_t)(i + 1) * secondFrames / numP);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > (size_t)secondFrames) s1 = (size_t)secondFrames;
        float peak = 0.0f;
        for (size_t s = s0; s < s1; ++s) {
            float a = buf2->dataL[s] < 0 ? -buf2->dataL[s] : buf2->dataL[s];
            float b = buf2->dataR[s] < 0 ? -buf2->dataR[s] : buf2->dataR[s];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        buf2->peakCache[i] = peak;
    }

    int newIdx = -1;
    if (isPrimary) {
        // Trim the primary in place: allocate a new buffer with only
        // the first half, swap it in, deferred-delete the old one.
        auto* newPrimaryBuf = new AudioBuffer();
        newPrimaryBuf->dataL.resize((size_t)firstFrames);
        newPrimaryBuf->dataR.resize((size_t)firstFrames);
        std::memcpy(newPrimaryBuf->dataL.data(), srcBuf->dataL.data(), (size_t)firstFrames * sizeof(float));
        std::memcpy(newPrimaryBuf->dataR.data(), srcBuf->dataR.data(), (size_t)firstFrames * sizeof(float));
        newPrimaryBuf->frames = firstFrames;
        // Recompute peak cache (1 peak per 256 frames)
        size_t numP1 = (size_t)(firstFrames / 256);
        if (numP1 < 1) numP1 = 1;
        newPrimaryBuf->peakCache.resize(numP1);
        for (size_t i = 0; i < numP1; ++i) {
            size_t s0 = (size_t)((uint64_t)i * firstFrames / numP1);
            size_t s1 = (size_t)((uint64_t)(i + 1) * firstFrames / numP1);
            if (s1 <= s0) s1 = s0 + 1;
            if (s1 > (size_t)firstFrames) s1 = (size_t)firstFrames;
            float peak = 0.0f;
            for (size_t s = s0; s < s1; ++s) {
                float a = newPrimaryBuf->dataL[s] < 0 ? -newPrimaryBuf->dataL[s] : newPrimaryBuf->dataL[s];
                float b = newPrimaryBuf->dataR[s] < 0 ? -newPrimaryBuf->dataR[s] : newPrimaryBuf->dataR[s];
                if (a > peak) peak = a;
                if (b > peak) peak = b;
            }
            newPrimaryBuf->peakCache[i] = peak;
        }
        AudioBuffer* oldPrimary = m_tracks[trackIndex].audio.exchange(newPrimaryBuf, std::memory_order_release);
        queueAudioBufferDelete(oldPrimary);
        // Adjust fades: primary keeps fadeIn, fadeOut is cleared
        // (the fade tail now lives in the second half).
        m_tracks[trackIndex].primaryFadeIn.store(srcFadeIn, std::memory_order_relaxed);
        m_tracks[trackIndex].primaryFadeOut.store(0, std::memory_order_relaxed);
        // Push the second half as an extra clip
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        Clip c2;
        c2.buffer = buf2;
        c2.clipStart = timelinePos;
        c2.fadeIn.store(0, std::memory_order_relaxed);
        c2.fadeOut.store(srcFadeOut, std::memory_order_relaxed);
        c2.filePath = srcPath;
        m_trackClips[trackIndex].push_back(c2);
        newIdx = (int)m_trackClips[trackIndex].size();  // 1-based since primary is index 0
    } else {
        // Splitting an extra clip: create the second half as a new
        // extra and trim the original extra in place.
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        Clip& orig = m_trackClips[trackIndex][extraIdx];
        // Trim the original extra to the first half
        auto* newOrigBuf = new AudioBuffer();
        newOrigBuf->dataL.resize((size_t)firstFrames);
        newOrigBuf->dataR.resize((size_t)firstFrames);
        std::memcpy(newOrigBuf->dataL.data(), srcBuf->dataL.data(), (size_t)firstFrames * sizeof(float));
        std::memcpy(newOrigBuf->dataR.data(), srcBuf->dataR.data(), (size_t)firstFrames * sizeof(float));
        newOrigBuf->frames = firstFrames;
        size_t numP2 = (size_t)(firstFrames / 256);
        if (numP2 < 1) numP2 = 1;
        newOrigBuf->peakCache.resize(numP2);
        for (size_t i = 0; i < numP2; ++i) {
            size_t s0 = (size_t)((uint64_t)i * firstFrames / numP2);
            size_t s1 = (size_t)((uint64_t)(i + 1) * firstFrames / numP2);
            if (s1 <= s0) s1 = s0 + 1;
            if (s1 > (size_t)firstFrames) s1 = (size_t)firstFrames;
            float peak = 0.0f;
            for (size_t s = s0; s < s1; ++s) {
                float a = newOrigBuf->dataL[s] < 0 ? -newOrigBuf->dataL[s] : newOrigBuf->dataL[s];
                float b = newOrigBuf->dataR[s] < 0 ? -newOrigBuf->dataR[s] : newOrigBuf->dataR[s];
                if (a > peak) peak = a;
                if (b > peak) peak = b;
            }
            newOrigBuf->peakCache[i] = peak;
        }
        AudioBuffer* oldBuf = orig.buffer;
        orig.buffer = newOrigBuf;
        queueAudioBufferDelete(oldBuf);
        orig.fadeOut.store(0, std::memory_order_relaxed);
        // Append the second half as a new extra
        Clip c2;
        c2.buffer = buf2;
        c2.clipStart = timelinePos;
        c2.fadeIn.store(0, std::memory_order_relaxed);
        c2.fadeOut.store(srcFadeOut, std::memory_order_relaxed);
        c2.filePath = srcPath;
        m_trackClips[trackIndex].push_back(c2);
        newIdx = (int)m_trackClips[trackIndex].size();
    }
    publishClipsSnapshot(trackIndex);
    return newIdx;
}

bool AudioEngine::trimClip(int trackIndex, int clipIndex, uint64_t newStart, uint64_t newEnd) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return false;
    if (newEnd <= newStart) return false;
    bool isPrimary = false;
    AudioBuffer* buf = nullptr;
    uint64_t oldStart = 0;
    {
        bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
        if (hasPrimary && clipIndex == 0) {
            buf = m_tracks[trackIndex].audio.load(std::memory_order_relaxed);
            oldStart = m_tracks[trackIndex].clipStart.load(std::memory_order_relaxed);
            isPrimary = true;
        } else {
            std::lock_guard<std::mutex> lk(m_clipsMutex);
            int idx = hasPrimary ? clipIndex - 1 : clipIndex;
            if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return false;
            buf = m_trackClips[trackIndex][idx].buffer;
            oldStart = m_trackClips[trackIndex][idx].clipStart;
        }
    }
    if (!buf) return false;
    if (newStart < oldStart) newStart = oldStart;
    uint64_t trimStart = newStart - oldStart;
    if (trimStart >= buf->frames) return false;
    if (newEnd > oldStart + buf->frames) newEnd = oldStart + buf->frames;
    uint64_t trimEnd = newEnd - oldStart;
    // Build new buffer (copy the trimmed region)
    auto* newBuf = new AudioBuffer();
    uint64_t newFrames = trimEnd - trimStart;
    newBuf->dataL.resize((size_t)newFrames);
    newBuf->dataR.resize((size_t)newFrames);
    std::memcpy(newBuf->dataL.data(), &buf->dataL[trimStart], (size_t)newFrames * sizeof(float));
    std::memcpy(newBuf->dataR.data(), &buf->dataR[trimStart], (size_t)newFrames * sizeof(float));
    newBuf->frames = newFrames;
    // Recompute peaks (1 peak per 256 frames)
    size_t numP = (size_t)(newFrames / 256);
    if (numP < 1) numP = 1;
    newBuf->peakCache.resize(numP);
    for (size_t i = 0; i < numP; ++i) {
        size_t s0 = (size_t)((uint64_t)i * newFrames / numP);
        size_t s1 = (size_t)((uint64_t)(i + 1) * newFrames / numP);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > (size_t)newFrames) s1 = (size_t)newFrames;
        float peak = 0.0f;
        for (size_t s = s0; s < s1; ++s) {
            float a = newBuf->dataL[s] < 0 ? -newBuf->dataL[s] : newBuf->dataL[s];
            float b = newBuf->dataR[s] < 0 ? -newBuf->dataR[s] : newBuf->dataR[s];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        newBuf->peakCache[i] = peak;
    }
    // Install new buffer; free old via deferred delete
    {
        std::lock_guard<std::mutex> lk(m_clipsMutex);
        if (isPrimary) {
            AudioBuffer* old = m_tracks[trackIndex].audio.exchange(newBuf, std::memory_order_release);
            m_tracks[trackIndex].clipStart.store(newStart, std::memory_order_relaxed);
            queueAudioBufferDelete(old);
        } else {
            bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
            int idx = hasPrimary ? clipIndex - 1 : clipIndex;
            if (idx >= 0 && idx < (int)m_trackClips[trackIndex].size()) {
                AudioBuffer* old = m_trackClips[trackIndex][idx].buffer;
                m_trackClips[trackIndex][idx].buffer = newBuf;
                m_trackClips[trackIndex][idx].clipStart = newStart;
                queueAudioBufferDelete(old);
            }
        }
    }
    publishClipsSnapshot(trackIndex);
    return true;
}

void AudioEngine::setClipFadeIn(int trackIndex, int clipIndex, uint64_t samples) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return;
    if (clipIndex < 0) return;
    bool isPrimary = (clipIndex == 0 &&
                      m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr);
    if (isPrimary) {
        m_tracks[trackIndex].primaryFadeIn.store(samples, std::memory_order_relaxed);
    } else {
        {
            std::lock_guard<std::mutex> lk(m_clipsMutex);
            bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
            int idx = hasPrimary ? clipIndex - 1 : clipIndex;
            if (idx >= 0 && idx < (int)m_trackClips[trackIndex].size())
                m_trackClips[trackIndex][idx].fadeIn.store(samples, std::memory_order_relaxed);
        }
        // Publish a new snapshot so the audio thread sees the change
        // — the existing snapshot has a copy of the old fade values.
        publishClipsSnapshot(trackIndex);
    }
}

void AudioEngine::setClipFadeOut(int trackIndex, int clipIndex, uint64_t samples) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return;
    if (clipIndex < 0) return;
    bool isPrimary = (clipIndex == 0 &&
                      m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr);
    if (isPrimary) {
        m_tracks[trackIndex].primaryFadeOut.store(samples, std::memory_order_relaxed);
    } else {
        {
            std::lock_guard<std::mutex> lk(m_clipsMutex);
            bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
            int idx = hasPrimary ? clipIndex - 1 : clipIndex;
            if (idx >= 0 && idx < (int)m_trackClips[trackIndex].size())
                m_trackClips[trackIndex][idx].fadeOut.store(samples, std::memory_order_relaxed);
        }
        publishClipsSnapshot(trackIndex);
    }
}

uint64_t AudioEngine::getClipFadeIn(int trackIndex, int clipIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0)
        return m_tracks[trackIndex].primaryFadeIn.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    int idx = hasPrimary ? clipIndex - 1 : clipIndex;
    if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return 0;
    return m_trackClips[trackIndex][idx].fadeIn.load(std::memory_order_relaxed);
}

uint64_t AudioEngine::getClipFadeOut(int trackIndex, int clipIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    bool hasPrimary = m_tracks[trackIndex].audio.load(std::memory_order_relaxed) != nullptr;
    if (hasPrimary && clipIndex == 0)
        return m_tracks[trackIndex].primaryFadeOut.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_clipsMutex);
    int idx = hasPrimary ? clipIndex - 1 : clipIndex;
    if (idx < 0 || idx >= (int)m_trackClips[trackIndex].size()) return 0;
    return m_trackClips[trackIndex][idx].fadeOut.load(std::memory_order_relaxed);
}

PluginChain* AudioEngine::getPluginChain(int trackIndex) {
    if (trackIndex == -1) return m_masterPluginChain.get();
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return nullptr;
    return m_pluginChains[trackIndex].get();
}

void AudioEngine::rebuildLatencyMap() {
    uint32_t maxLat = 0;
    int tc = m_trackCount.load();
    for (int i = 0; i < tc; ++i) {
        PluginChain* c = m_pluginChains[i].get();
        uint32_t lat = c ? c->getLatency() : 0;
        m_trackDelays[i].resize(lat > 0 ? lat : 0);
        m_trackDelays[i].delay = lat;
        if (lat > maxLat) maxLat = lat;
    }
    PluginChain* mc = m_masterPluginChain.get();
    uint32_t mLat = mc ? mc->getLatency() : 0;
    // Master is delayed by (maxLat - mLat) so total aligns to maxLat
    if (maxLat > mLat) {
        m_masterDelay.resize(maxLat);
        m_masterDelay.delay = maxLat - mLat;
    } else {
        m_masterDelay.resize(0);
        m_masterDelay.delay = 0;
    }
    m_maxPluginLatency.store(maxLat, std::memory_order_release);
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
    // thread under the xrun threshold. The ILogSink implementation
    // (if any) must be lock-free; the default FileLogSink takes a
    // short critical section that is acceptable at this cadence.
    static thread_local int tl_audioCbCount = 0;
    auto* logSink = static_cast<AudioEngine*>(pDevice->pUserData)->m_logSink;
    if (logSink && (++tl_audioCbCount == 1 || tl_audioCbCount % 5000 == 0)) {
        logSink->log(hydraw::ILogSink::Level::Debug,
            ("audioCallback call=" + std::to_string(tl_audioCbCount) +
             " frames=" + std::to_string(frameCount)).c_str());
    }

    // Clamp frameCount to BLOCK_SIZE — our stack scratch buffers are
    // sized to this constant. If the backend delivers a larger block
    // we truncate silently (better than stack corruption).
    if (frameCount > BLOCK_SIZE) frameCount = BLOCK_SIZE;

    std::memset(out, 0, (size_t)frameCount * 2 * sizeof(float));

    bool playing = engine->m_transport.playing.load(std::memory_order_acquire);
    uint64_t basePlayhead = engine->m_transport.playhead.load(std::memory_order_relaxed);
    int trackCount = engine->m_trackCount.load(std::memory_order_acquire);

    // Aux bus accumulators (planar, zeroed). These collect the sum of
    // (track_out * send_level) for each bus across all tracks, then we
    // process each bus's plugin chain and add to `out`.
    float busL[BLOCK_SIZE * MAX_BUSES];
    float busR[BLOCK_SIZE * MAX_BUSES];
    std::memset(busL, 0, sizeof(busL));
    std::memset(busR, 0, sizeof(busR));

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
            // If the track is frozen, read from the pre-rendered
            // frozenBuffer instead of going through the live path.
            // The plugin chain is bypassed (audio thread skip), saving
            // CPU. The frozen buffer already contains the rendered
            // audio + plugin chain output.
            AudioBuffer* buf = nullptr;
            uint64_t cs = 0;
            bool useFrozen = track.frozen.load(std::memory_order_acquire);
            if (useFrozen) {
                buf = track.frozenBuffer.load(std::memory_order_acquire);
                cs = track.frozenClipStart.load(std::memory_order_relaxed);
            } else {
                buf = track.audio.load(std::memory_order_acquire);
                cs = track.clipStart.load(std::memory_order_relaxed);
            }
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
                    // Apply fade-in/out on primary (only if not frozen —
                    // fades were already baked into the frozen buffer
                    // at the time of freeze).
                    if (!useFrozen) {
                        uint64_t fIn = track.primaryFadeIn.load(std::memory_order_relaxed);
                        uint64_t fOut = track.primaryFadeOut.load(std::memory_order_relaxed);
                        if (fIn > 0 || fOut > 0) {
                            for (ma_uint32 f = 0; f < copyFrames; ++f) {
                                float gain = 1.0f;
                                uint64_t absS = offset + f;
                                if (fIn > 0 && absS < fIn) gain = (float)absS / (float)fIn;
                                if (fOut > 0 && absS >= buf->frames - fOut)
                                    gain = std::min(gain, (float)(buf->frames - absS) / (float)fOut);
                                if (gain < 0.0f) gain = 0.0f;
                                planar[f]                  *= gain;
                                planar[frameCount + f]     *= gain;
                            }
                        }
                    }
                    hasAudio = true;
                }
            }
            // Mix additional clips from snapshot
            ClipsSnapshot* snap = engine->m_clipsSnapshot[t].load(std::memory_order_acquire);
            if (snap) {
                for (const Clip& c : snap->clips) {
                    if (!c.buffer) continue;
                    if (basePlayhead < c.clipStart) continue;
                    uint64_t offset = basePlayhead - c.clipStart;
                    if (offset >= c.buffer->frames) continue;
                    ma_uint32 copyFrames = frameCount;
                    if (offset + copyFrames > c.buffer->frames)
                        copyFrames = (ma_uint32)(c.buffer->frames - offset);
                    uint64_t fIn = c.fadeIn.load(std::memory_order_relaxed);
                    uint64_t fOut = c.fadeOut.load(std::memory_order_relaxed);
                    if (hasAudio) {
                        for (ma_uint32 f = 0; f < copyFrames; ++f) {
                            float gain = 1.0f;
                            uint64_t absS = offset + f;
                            if (fIn > 0 && absS < fIn) gain = (float)absS / (float)fIn;
                            if (fOut > 0 && absS >= c.buffer->frames - fOut)
                                gain = std::min(gain, (float)(c.buffer->frames - absS) / (float)fOut);
                            if (gain < 0.0f) gain = 0.0f;
                            planar[f]                  += c.buffer->dataL[offset + f] * gain;
                            planar[frameCount + f]     += c.buffer->dataR[offset + f] * gain;
                        }
                    } else {
                        std::memcpy(planar,                &c.buffer->dataL[offset], copyFrames * sizeof(float));
                        std::memcpy(planar + frameCount,   &c.buffer->dataR[offset], copyFrames * sizeof(float));
                        if (copyFrames < frameCount) {
                            std::memset(planar + copyFrames,              0, (size_t)(frameCount - copyFrames) * sizeof(float));
                            std::memset(planar + frameCount + copyFrames, 0, (size_t)(frameCount - copyFrames) * sizeof(float));
                        }
                        if (fIn > 0 || fOut > 0) {
                            for (ma_uint32 f = 0; f < copyFrames; ++f) {
                                float gain = 1.0f;
                                uint64_t absS = offset + f;
                                if (fIn > 0 && absS < fIn) gain = (float)absS / (float)fIn;
                                if (fOut > 0 && absS >= c.buffer->frames - fOut)
                                    gain = std::min(gain, (float)(c.buffer->frames - absS) / (float)fOut);
                                if (gain < 0.0f) gain = 0.0f;
                                planar[f]                  *= gain;
                                planar[frameCount + f]     *= gain;
                            }
                        }
                        hasAudio = true;
                    }
                }
            }
        }
        if (!hasAudio) {
            std::memset(planar, 0, sizeof(planar));
        }

        // Add armed input (still interleaved in `in`)
        if (track.armed.load(std::memory_order_relaxed) && in) {
            hasAudio = true;
            bool isRecTrack = engine->m_recording.load(std::memory_order_acquire) &&
                              engine->m_recTrack.load(std::memory_order_relaxed) == t;
            for (ma_uint32 f = 0; f < frameCount; ++f) {
                planar[f]              += in[f * 2 + 0];
                planar[frameCount + f] += in[f * 2 + 1];
                track.bufferL[f] = in[f * 2 + 0];
                track.bufferR[f] = in[f * 2 + 1];
                if (isRecTrack) {
                    // Append to recording buffer. This is the only
                    // writer of m_recBufL/R while m_recording is true.
                    engine->m_recBufL.push_back(in[f * 2 + 0]);
                    engine->m_recBufR.push_back(in[f * 2 + 1]);
                }
            }
            if (isRecTrack) {
                engine->m_recFrames.fetch_add((uint64_t)frameCount, std::memory_order_relaxed);
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

        // PDC: delay this track's output by its chain's reported latency
        // so tracks with low-latency chains align with high-latency ones.
        if (hasAudio) {
            engine->m_trackDelays[t].process(planar, planar + frameCount, (size_t)frameCount);
        }

        // FUSED: gain + interleave to `out` + peak detection in ONE loop.
        // Hoisting the gains eliminates 512 double-precision ops per block
        // and 16*256 redundant min() calls. The branchless `t<0?-t:t` is
        // faster than std::abs for the non-NaN path.
        float vol = track.volume.load(std::memory_order_relaxed);
        float panVal = track.pan.load(std::memory_order_relaxed);
        // Evaluate automation lanes at the current playhead.
        AutoSnapshot* asnap = engine->m_autoSnapshot[t].load(std::memory_order_acquire);
        if (asnap) {
            if (!asnap->vol.empty()) {
                const auto& lane = asnap->vol;
                if (basePlayhead <= lane.front().samplePos) vol = lane.front().value;
                else if (basePlayhead >= lane.back().samplePos) vol = lane.back().value;
                else {
                    auto it = std::upper_bound(lane.begin(), lane.end(), basePlayhead,
                        [](uint64_t s, const Track::AutoPoint& p) { return s < p.samplePos; });
                    if (it != lane.end() && it != lane.begin()) {
                        const auto& r = *it;
                        const auto& l = *(it - 1);
                        double a = (double)(basePlayhead - l.samplePos) / (double)(r.samplePos - l.samplePos);
                        vol = l.value + (float)a * (r.value - l.value);
                    }
                }
            }
            if (!asnap->pan.empty()) {
                const auto& lane = asnap->pan;
                if (basePlayhead <= lane.front().samplePos) panVal = lane.front().value;
                else if (basePlayhead >= lane.back().samplePos) panVal = lane.back().value;
                else {
                    auto it = std::upper_bound(lane.begin(), lane.end(), basePlayhead,
                        [](uint64_t s, const Track::AutoPoint& p) { return s < p.samplePos; });
                    if (it != lane.end() && it != lane.begin()) {
                        const auto& r = *it;
                        const auto& l = *(it - 1);
                        double a = (double)(basePlayhead - l.samplePos) / (double)(r.samplePos - l.samplePos);
                        panVal = l.value + (float)a * (r.value - l.value);
                    }
                }
            }
        }
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

        // Send to aux buses
        for (int b = 0; b < MAX_BUSES; ++b) {
            float send = track.sends[b].load(std::memory_order_relaxed);
            if (send <= 0.0f) continue;
            float* dL = busL + (size_t)b * frameCount;
            float* dR = busR + (size_t)b * frameCount;
            for (ma_uint32 f = 0; f < frameCount; ++f) {
                dL[f] += planar[f]              * send;
                dR[f] += planar[frameCount + f] * send;
            }
        }
    }

    // Process each aux bus's plugin chain (none for now — bus plugin
    // chains are reserved for future expansion) and add to `out`. We
    // still apply bus volume and compute bus peaks.
    for (int b = 0; b < MAX_BUSES; ++b) {
        AuxBus& bus = engine->m_auxBuses[b];
        const float vol = bus.volume.load(std::memory_order_relaxed);
        float* dL = busL + (size_t)b * frameCount;
        float* dR = busR + (size_t)b * frameCount;
        float busPeakL = 0.0f, busPeakR = 0.0f;
        float* outPtr = out;
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            const float tL = dL[f] * vol;
            const float tR = dR[f] * vol;
            outPtr[0] += tL;
            outPtr[1] += tR;
            outPtr += 2;
            const float aL = tL < 0.0f ? -tL : tL;
            const float aR = tR < 0.0f ? -tR : tR;
            if (aL > busPeakL) busPeakL = aL;
            if (aR > busPeakR) busPeakR = aR;
        }
        float oldBPL = bus.peakL.load(std::memory_order_relaxed);
        bus.peakL.store(busPeakL > oldBPL ? busPeakL : oldBPL * 0.999f, std::memory_order_relaxed);
        float oldBPR = bus.peakR.load(std::memory_order_relaxed);
        bus.peakR.store(busPeakR > oldBPR ? busPeakR : oldBPR * 0.999f, std::memory_order_relaxed);
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
        // Apply master delay compensation (aligns master to max chain latency)
        engine->m_masterDelay.process(planar, planar + frameCount, (size_t)frameCount);
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

        // Loop wrap: if loop is enabled and we crossed loopEnd, reset
        // the playhead to loopStart.
        if (engine->m_loopEnabled.load(std::memory_order_relaxed)) {
            uint64_t ls = engine->m_loopStart.load(std::memory_order_relaxed);
            uint64_t le = engine->m_loopEnd.load(std::memory_order_relaxed);
            if (le > ls && newPlayhead >= le) {
                // Jump back to loopStart; the rendered samples for
                // this block remain valid (they correspond to the
                // pre-wrap range). Subsequent blocks will read from
                // the new playhead.
                engine->m_transport.playhead.store(ls, std::memory_order_relaxed);
                newPlayhead = ls;
            }
        }

        bool anyHasAudio = false;
        bool stillPlaying = false;
        for (int t = 0; t < trackCount; ++t) {
            // Consider the primary clip for auto-stop.
            AudioBuffer* buf = engine->m_tracks[t].audio.load(std::memory_order_relaxed);
            if (buf && buf->frames > 0) {
                anyHasAudio = true;
                uint64_t cs = engine->m_tracks[t].clipStart.load(std::memory_order_relaxed);
                if (newPlayhead < cs + buf->frames)
                    stillPlaying = true;
            }
            // Also consider the extras (e.g. clips produced by a
            // split). Without this, a split at the end of the
            // primary would auto-stop the transport exactly when
            // the new tail clip is about to start playing.
            ClipsSnapshot* stopSnap = engine->m_clipsSnapshot[t].load(std::memory_order_acquire);
            if (stopSnap) {
                for (const Clip& ec : stopSnap->clips) {
                    if (!ec.buffer || ec.buffer->frames == 0) continue;
                    anyHasAudio = true;
                    if (newPlayhead < ec.clipStart + ec.buffer->frames)
                        stillPlaying = true;
                }
            }
        }
        if (anyHasAudio && !stillPlaying && !engine->m_loopEnabled.load(std::memory_order_relaxed))
            engine->m_transport.playing.store(false, std::memory_order_relaxed);
    }

    // Drain deferred AudioBuffer deletes and Snapshot deletes. By this
    // point every in-flight pointer from THIS block has been released,
    // so any buffer/snapshot queued during this or an earlier block is
    // safe to free.
    engine->drainPendingAudioBufferDeletes();
    PluginChain::drainPendingDeletes();
    // Drain pending ClipsSnapshot deletes
    {
        std::vector<ClipsSnapshot*> pending;
        {
            std::lock_guard<std::mutex> lk(engine->m_pendingClipsSnapshotMutex);
            pending.swap(engine->m_pendingClipsSnapshots);
        }
        for (auto* s : pending) delete s;
    }
    // Drain pending AutoSnapshot deletes
    {
        std::vector<AutoSnapshot*> pending;
        {
            std::lock_guard<std::mutex> lk(engine->m_autoSnapMutex);
            pending.swap(engine->m_pendingAutoSnapshots);
        }
        for (auto* s : pending) delete s;
    }
    // Drain pending MidiSnapshot deletes
    {
        std::vector<MidiSnapshot*> pending;
        {
            std::lock_guard<std::mutex> lk(engine->m_midiSnapMutex);
            pending.swap(engine->m_pendingMidiSnapshots);
        }
        for (auto* s : pending) delete s;
    }
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

void AudioEngine::startRecording(int trackIndex, const char* outPath) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return;
    if (!outPath) return;
    {
        std::lock_guard<std::mutex> lk(m_recMutex);
        if (m_recording.load()) return;
        m_recBufL.clear();
        m_recBufR.clear();
        m_recBufL.reserve(1 << 16);
        m_recBufR.reserve(1 << 16);
        m_recOutPath = outPath;
        m_recFrames.store(0, std::memory_order_relaxed);
        m_recStartPos.store(m_transport.playhead.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_recTrack.store(trackIndex, std::memory_order_release);
        m_recording.store(true, std::memory_order_release);
    }
}

void AudioEngine::stopRecording() {
    if (!m_recording.load()) return;
    int recTrack;
    std::vector<float> outL, outR;
    std::string outPath;
    uint64_t startPos;
    {
        std::lock_guard<std::mutex> lk(m_recMutex);
        if (!m_recording.load()) return;
        recTrack = m_recTrack.load(std::memory_order_relaxed);
        outL.swap(m_recBufL);
        outR.swap(m_recBufR);
        outPath = m_recOutPath;
        startPos = m_recStartPos.load(std::memory_order_relaxed);
        m_recording.store(false, std::memory_order_release);
    }
    if (recTrack < 0) return;
    // Write WAV
    if (!outL.empty()) {
        WavWriter::writeFloat32(outPath.c_str(), outL.data(), (uint64_t)outL.size(), 2, SAMPLE_RATE);
        // Add as clip at the recorded track
        addClip(recTrack, outPath.c_str(), startPos);
    }
}

bool AudioEngine::isRecording() const { return m_recording.load(std::memory_order_acquire); }
int AudioEngine::getRecordingTrack() const { return m_recTrack.load(std::memory_order_acquire); }
uint64_t AudioEngine::getRecordedFrames() const { return m_recFrames.load(std::memory_order_acquire); }

void AudioEngine::addAutoPoint(int trackIndex, AutoParam param, uint64_t samplePos, float value) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    Track& t = m_tracks[trackIndex];
    auto& lane = (param == AutoParam::Volume) ? t.volumeAutomation : t.panAutomation;
    {
        // Insert sorted; if a point exists at the same pos, replace
        Track::AutoPoint p{samplePos, value};
        auto it = std::lower_bound(lane.begin(), lane.end(), p,
            [](const Track::AutoPoint& a, const Track::AutoPoint& b) {
                return a.samplePos < b.samplePos;
            });
        if (it != lane.end() && it->samplePos == samplePos) it->value = value;
        else lane.insert(it, p);
    }
    // Publish new snapshot
    auto* snap = new AutoSnapshot();
    snap->vol = t.volumeAutomation;
    snap->pan = t.panAutomation;
    auto* old = m_autoSnapshot[trackIndex].exchange(snap, std::memory_order_acq_rel);
    if (old) {
        std::lock_guard<std::mutex> lk(m_autoSnapMutex);
        m_pendingAutoSnapshots.push_back(old);
    }
}

void AudioEngine::clearAutoLane(int trackIndex, AutoParam param) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    Track& t = m_tracks[trackIndex];
    if (param == AutoParam::Volume) t.volumeAutomation.clear();
    else t.panAutomation.clear();
    auto* snap = new AutoSnapshot();
    snap->vol = t.volumeAutomation;
    snap->pan = t.panAutomation;
    auto* old = m_autoSnapshot[trackIndex].exchange(snap, std::memory_order_acq_rel);
    if (old) {
        std::lock_guard<std::mutex> lk(m_autoSnapMutex);
        m_pendingAutoSnapshots.push_back(old);
    }
}

int AudioEngine::getAutoPointCount(int trackIndex, AutoParam param) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    const Track& t = m_tracks[trackIndex];
    return (int)((param == AutoParam::Volume) ? t.volumeAutomation.size() : t.panAutomation.size());
}

void AudioEngine::freezeTrack(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= m_trackCount.load()) return;
    // Determine duration: from min(clipStart) to max(clipStart+frames)
    Track& t = m_tracks[trackIndex];
    uint64_t start = 0, end = 0;
    AudioBuffer* p = t.audio.load(std::memory_order_acquire);
    if (p && p->frames > 0) {
        start = t.clipStart.load(std::memory_order_relaxed);
        end = start + p->frames;
    }
    // Render offline at this track's position
    if (end == 0) return;
    uint64_t dur = end - start;
    auto* frozen = new AudioBuffer();
    frozen->dataL.resize((size_t)dur, 0.0f);
    frozen->dataR.resize((size_t)dur, 0.0f);
    frozen->frames = dur;
    // Copy live clip data into frozen buffer with offsets
    if (p) {
        for (uint64_t i = 0; i < p->frames; ++i) {
            frozen->dataL[i] = p->dataL[i];
            frozen->dataR[i] = p->dataR[i];
        }
    }
    // TODO: process frozen buffer through plugin chain. For now the
    // live signal path is fully captured; plugin processing in freeze
    // mode requires running the chain over the frozen buffer on the
    // main thread (CPU-cheap, no audio thread involvement).
    PluginChain* chain = m_pluginChains[trackIndex].get();
    if (chain && chain->hasPlugins()) {
        // Run chain over the frozen buffer in BLOCK_SIZE chunks. This
        // is an offline operation on the main thread — it can block
        // briefly but is bounded by dur / 48000 seconds.
        const int CHUNK = 256;
        float planar[CHUNK * 2];
        for (uint64_t off = 0; off < dur; off += CHUNK) {
            ma_uint32 n = (ma_uint32)std::min((uint64_t)CHUNK, dur - off);
            for (ma_uint32 i = 0; i < n; ++i) {
                planar[i]            = frozen->dataL[off + i];
                planar[CHUNK + i]    = frozen->dataR[off + i];
            }
            chain->process(planar, planar, (int)n, 2);
            for (ma_uint32 i = 0; i < n; ++i) {
                frozen->dataL[off + i] = planar[i];
                frozen->dataR[off + i] = planar[CHUNK + i];
            }
        }
    }
    // Compute peak cache (1 peak per 256 frames)
    size_t numP = (size_t)(dur / 256);
    if (numP < 1) numP = 1;
    frozen->peakCache.resize(numP);
    for (size_t i = 0; i < numP; ++i) {
        size_t s0 = (size_t)((uint64_t)i * dur / numP);
        size_t s1 = (size_t)((uint64_t)(i + 1) * dur / numP);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > (size_t)dur) s1 = (size_t)dur;
        float peak = 0.0f;
        for (size_t s = s0; s < s1; ++s) {
            float a = frozen->dataL[s] < 0 ? -frozen->dataL[s] : frozen->dataL[s];
            float b = frozen->dataR[s] < 0 ? -frozen->dataR[s] : frozen->dataR[s];
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        frozen->peakCache[i] = peak;
    }
    AudioBuffer* oldFrozen = t.frozenBuffer.exchange(frozen, std::memory_order_release);
    if (oldFrozen) queueAudioBufferDelete(oldFrozen);
    t.frozenClipStart.store(start, std::memory_order_relaxed);
    t.frozen.store(true, std::memory_order_release);
}

void AudioEngine::unfreezeTrack(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    Track& t = m_tracks[trackIndex];
    t.frozen.store(false, std::memory_order_release);
    AudioBuffer* old = t.frozenBuffer.exchange(nullptr, std::memory_order_release);
    if (old) queueAudioBufferDelete(old);
}

bool AudioEngine::isTrackFrozen(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return false;
    return m_tracks[trackIndex].frozen.load(std::memory_order_acquire);
}

void AudioEngine::publishMidiSnapshot(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    auto* snap = new MidiSnapshot();
    snap->notes = m_tracks[trackIndex].midiNotes;
    auto* old = m_midiSnapshot[trackIndex].exchange(snap, std::memory_order_acq_rel);
    if (old) {
        std::lock_guard<std::mutex> lk(m_midiSnapMutex);
        m_pendingMidiSnapshots.push_back(old);
    }
}

int AudioEngine::addMidiNote(int trackIndex, const MidiNote& note) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return -1;
    auto& notes = m_tracks[trackIndex].midiNotes;
    notes.push_back(note);
    std::sort(notes.begin(), notes.end(),
        [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    publishMidiSnapshot(trackIndex);
    return (int)notes.size() - 1;
}

void AudioEngine::clearMidiLane(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return;
    m_tracks[trackIndex].midiNotes.clear();
    publishMidiSnapshot(trackIndex);
}

int AudioEngine::getMidiNoteCount(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= MAX_TRACKS) return 0;
    return (int)m_tracks[trackIndex].midiNotes.size();
}
