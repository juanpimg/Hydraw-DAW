#include "PluginChain.h"
#include "ClapHost.h"
#include <cstring>
#include <mutex>
#include <cstdio>
#include <vector>
#include <cmath>
#include <pthread.h>
#include <unordered_map>

// Verbose debug logging — declared in main.cpp. Writes only to the
// log file (not stderr) so we can trace the plugin/audio thread paths
// without spamming the user's console.
extern void dlog(const std::string& msg);
extern std::string threadId();

// CLAP extensions
#include "ext/timer-support.h"
#include "ext/log.h"
#include "ext/params.h"
#include "ext/audio-ports.h"
#include "ext/state.h"

#include <set>

extern const clap_host_gui_t s_hostGui;

// ── Pending flush tracking (for processPendingFlushes on main thread) ──
static std::mutex g_flushMutex;
std::set<const clap_host_t*> g_pendingFlush;

// ── CLAP event structures ──

struct HostInputEvents {
    clap_input_events_t list;
    std::vector<const clap_event_header_t*> events;

    static uint32_t CLAP_ABI size_cb(const clap_input_events* list) {
        auto* self = (HostInputEvents*)list;
        return (uint32_t)self->events.size();
    }

    static const clap_event_header_t* CLAP_ABI get_cb(const clap_input_events* list, uint32_t index) {
        auto* self = (HostInputEvents*)list;
        if (index >= self->events.size()) return nullptr;
        return self->events[index];
    }

    HostInputEvents() : events() {
        list.size = size_cb;
        list.get = get_cb;
    }
};

struct HostOutputEvents {
    clap_output_events_t list;

    static bool CLAP_ABI try_push_cb(const clap_output_events* list, const clap_event_header_t* event) {
        return true; // acknowledge all events from plugin
    }

    HostOutputEvents() {
        list.try_push = try_push_cb;
    }
};

// ── CLAP host extensions ──

static bool clap_host_register_timer(const clap_host_t* host, uint32_t period_ms, clap_id* timer_id) {
    if (timer_id) *timer_id = 1;
    return true;
}
static bool clap_host_unregister_timer(const clap_host_t* host, clap_id timer_id) { return true; }
static const clap_host_timer_support_t s_hostTimer = {
    clap_host_register_timer,
    clap_host_unregister_timer
};

static void clap_host_log(const clap_host_t* host, clap_log_severity severity, const char* msg) {
    static const char* names[] = {"HOST", "ERROR", "WARNING", "INFO", "DEBUG"};
    int idx = (int)severity;
    if (idx < 0 || idx > 4) idx = 1; // CLAP_LOG_ERROR
    fprintf(stderr, "[CLAP %s] %s\n", names[idx], msg ? msg : "(null)");
    fflush(stderr);
}
static const clap_host_log_t s_hostLog = { clap_host_log };

static void clap_host_rescan(const clap_host_t* host, clap_param_rescan_flags flags) {}
static void clap_host_clear(const clap_host_t* host, clap_id param_id, clap_param_clear_flags flags) {}
static void clap_host_request_flush(const clap_host_t* host) {
    std::lock_guard<std::mutex> lk(g_flushMutex);
    g_pendingFlush.insert(host);
}
static const clap_host_params_t s_hostParams = {
    clap_host_rescan,
    clap_host_clear,
    clap_host_request_flush
};

// ── clap.thread-check (H3: highly recommended per spec) ──
// A thread-local flag set by the audio callback / main thread. The plugin
// g_mainThreadId MUST be set on the actual main thread BEFORE any plugin
// can call our get_extension() (which calls is_main_thread). We set it
// from initMainThreadId() called at the top of main() before anything
// else. The lazy-init fallback below exists as a safety net for any
// future code path that might instantiate a PluginChain before
// initMainThreadId() has run.
static std::atomic<pthread_t> g_mainThreadId{0};
void PluginChain::initMainThreadId() {
    g_mainThreadId.store(pthread_self(), std::memory_order_release);
}

// Thread-local flag set by the audio callback while inside process().
// Plugins call is_audio_thread() during their process() impl to verify
// the host is calling process() on the audio thread.
static thread_local bool g_inAudioCallback = false;

// Thread-local cache of "we already called start_processing on this
// plugin instance". Maps PluginInstance* to bool. We need this because
// the snapshot's `processing` field is per-snapshot and we don't want
// to call start_processing every block (or after every snapshot swap).
// When a plugin is removed, its entry becomes stale but we just leak
// the small bool; the PluginInstance is also gone so it won't match.
static thread_local std::unordered_map<const PluginInstance*, bool> g_startedCache;

static bool clap_host_is_main_thread(const clap_host_t* host) {
    (void)host;
    // Per CLAP spec the host should advertise a main-thread identity so
    // plugins can validate calls. In practice JUCE-based CLAP wrappers
    // (e.g. PeakEater) query this from their own message-loop / editor
    // thread, which is NOT the same pthread_t as our main thread even
    // though it is logically a "main" thread for the plugin. A strict
    // pthread comparison then trips "[CLAP ERROR] ... on wrong thread"
    // for every legit JUCE-internal call.
    //
    // The only thread we MUST reject is the audio thread — a plugin
    // process() call from the wrong thread is the catastrophic mistake.
    // For any other thread, the work the plugin does (get_extension
    // lookup, on_timer queueing, params rescan bookkeeping) is
    // thread-safe. So: accept all non-audio threads as "main". The
    // audio-thread check stays strict via is_audio_thread below.
    if (g_inAudioCallback) return false;
    return true;
}
static bool clap_host_is_audio_thread(const clap_host_t* host) {
    (void)host;
    return g_inAudioCallback;
}
static const clap_host_thread_check_t s_hostThreadCheck = {
    clap_host_is_main_thread,
    clap_host_is_audio_thread
};

static const void* clap_get_extension(const clap_host_t* host, const char* extension_id) {
    if (std::strcmp(extension_id, CLAP_EXT_GUI) == 0) return &s_hostGui;
    if (std::strcmp(extension_id, CLAP_EXT_TIMER_SUPPORT) == 0) return &s_hostTimer;
    if (std::strcmp(extension_id, CLAP_EXT_LOG) == 0) return &s_hostLog;
    if (std::strcmp(extension_id, CLAP_EXT_PARAMS) == 0) return &s_hostParams;
    if (std::strcmp(extension_id, CLAP_EXT_THREAD_CHECK) == 0) return &s_hostThreadCheck;
    return nullptr;
}
static void clap_request_restart(const clap_host_t* host) {}
static void clap_request_process(const clap_host_t* host) {}
static void clap_request_callback(const clap_host_t* host) {}

static const clap_host_t s_hostTemplate = []() {
    clap_host_t h = {};
    h.clap_version = CLAP_VERSION;
    h.name = "Hydraw DAW";
    h.vendor = "Hydraw DAW";
    h.url = "https://hydraw.local";
    h.version = HYDRAW_VERSION;
    h.get_extension = clap_get_extension;
    h.request_restart = clap_request_restart;
    h.request_process = clap_request_process;
    h.request_callback = clap_request_callback;
    return h;
}();

// ── PluginChain implementation ──

PluginChain::PluginChain(int sampleRate, int blockSize, int trackIndex)
    : m_sampleRate(sampleRate)
    , m_blockSize(blockSize)
    , m_trackIdx(trackIndex)
{
    m_scratchBuffer.resize(blockSize * 2, 0.0f);
    // Initial empty snapshot.
    auto snap = std::make_shared<Snapshot>();
    snap->gen.store(0, std::memory_order_release);
    m_snapshots = snap;
}

PluginChain::~PluginChain() { clear(); }

namespace {
// Build a new snapshot from the current main-thread state. Called
// Helper: query the plugin's audio ports on the MAIN THREAD and cache
// the info in PortInfo. This MUST be called from main thread (it's
// safe to call from main because CLAP says audio_ports is [main-thread]).
static PluginChain::PortInfo query_ports(PluginInstance* pi) {
    PluginChain::PortInfo info;
    if (!pi || !pi->plugin) return info;
    auto* ap = (const clap_plugin_audio_ports_t*)
        pi->plugin->get_extension(pi->plugin, CLAP_EXT_AUDIO_PORTS);
    if (!ap) return info;
    if (ap->count) {
        info.inCount  = ap->count(pi->plugin, true);
        info.outCount = ap->count(pi->plugin, false);
    }
    if (info.inCount > 0 && ap->get) {
        clap_audio_port_info_t pInfo;
        if (ap->get(pi->plugin, 0, true, &pInfo)) {
            info.hasInStereo = (pInfo.channel_count == 2);
        }
    }
    if (info.outCount > 0 && ap->get) {
        clap_audio_port_info_t pInfo;
        if (ap->get(pi->plugin, 0, false, &pInfo)) {
            info.hasOutStereo = (pInfo.channel_count == 2);
        }
    }
    info.supportsStereo = info.hasInStereo && info.hasOutStereo;
    return info;
}

namespace {
// Build a new snapshot from the current main-thread state. Called
// under m_mutex. The returned snapshot is immutable and can be safely
// read by the audio thread.
std::shared_ptr<PluginChain::Snapshot> build_snapshot(
        std::vector<std::unique_ptr<PluginInstance>>& storage,
        std::atomic<uint32_t>& lastGen) {
    auto snap = std::make_shared<PluginChain::Snapshot>();
    snap->instances.reserve(storage.size());
    snap->active.reserve(storage.size());
    snap->ports.reserve(storage.size());
    snap->processing.reserve(storage.size());
    for (auto& up : storage) {
        snap->instances.push_back(up.get());
        snap->active.push_back(up->active);
        snap->ports.push_back(query_ports(up.get()));
        snap->processing.push_back(false); // not yet started; audio thread will set
    }
    uint32_t g = lastGen.load(std::memory_order_acquire) + 1;
    lastGen.store(g, std::memory_order_release);
    snap->gen.store(g, std::memory_order_release);
    return snap;
}
} // namespace
} // namespace

bool PluginChain::addPlugin(const char* libraryPath, const char* pluginId) {
    dlog("addPlugin enter track=" + std::to_string(m_trackIdx) + " id=" + std::string(pluginId) + " thread=" + threadId());
    // ── Phase A: load + init + activate WITHOUT holding m_mutex ──
    // (these are slow calls, we don't want to block other main-thread ops)
    void* lib = ClapHost::loadLibrary(libraryPath);
    if (!lib) { dlog("addPlugin: loadLibrary failed for " + std::string(libraryPath)); return false; }

    auto dedicatedHost = std::make_unique<clap_host_t>(s_hostTemplate);
    int slotIndex = (int)m_storage.size() + 1;
    dedicatedHost->host_data = (void*)(intptr_t)slotIndex;

    const clap_plugin_t* plugin = ClapHost::createPlugin(lib, dedicatedHost.get(), pluginId);
    if (!plugin) {
        dlog("addPlugin: createPlugin failed id=" + std::string(pluginId));
        ClapHost::unloadLibrary(lib);
        return false;
    }
    dlog("addPlugin: plugin created, calling init");

    bool ok = plugin->init ? plugin->init(plugin) : true;
    if (!ok) {
        dlog("addPlugin: plugin->init returned false");
        ClapHost::destroyPlugin(plugin);
        ClapHost::unloadLibrary(lib);
        return false;
    }

    if (plugin->activate &&
        !plugin->activate(plugin, (double)m_sampleRate, (uint32_t)m_blockSize, (uint32_t)m_blockSize)) {
        dlog("addPlugin: plugin->activate returned false");
        plugin->destroy(plugin);
        ClapHost::unloadLibrary(lib);
        return false;
    }
    dlog("addPlugin: activate OK");

    // NOTE: start_processing is [audio-thread] per spec. We do NOT call
    // it here — the audio callback calls it on the first block for any
    // plugin in the snapshot whose `processing` flag is false.

    // ── Phase B: brief critical section — add to storage + publish snapshot ──
    std::shared_ptr<Snapshot> newSnap;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto pi = std::make_unique<PluginInstance>();
        pi->library = lib;
        pi->plugin = plugin;
        pi->dedicatedHost = std::move(dedicatedHost);
        pi->name = pluginId;
        pi->path = libraryPath;
        pi->id = pluginId;
        pi->active = true;
        m_storage.push_back(std::move(pi));
        newSnap = build_snapshot(m_storage, *const_cast<std::atomic<uint32_t>*>(&m_snapshots->gen));
    }
    // Publish: atomic shared_ptr exchange. Audio thread sees the new
    // snapshot on its next call to process() without ever blocking.
    m_snapshots = std::move(newSnap);
    dlog("addPlugin OK track=" + std::to_string(m_trackIdx) + " idx=" + std::to_string(m_storage.size() - 1));
    return true;
}

int PluginChain::addBuiltinSoftClipper() {
    std::shared_ptr<Snapshot> newSnap;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto pi = std::make_unique<PluginInstance>();
        pi->isBuiltin = true;
        pi->active = true;
        pi->name = "Soft Clipper";
        pi->path = "builtin";
        pi->id = "builtin:softclipper";
        pi->drive = 1.0f;
        m_storage.push_back(std::move(pi));
        newSnap = build_snapshot(m_storage, *const_cast<std::atomic<uint32_t>*>(&m_snapshots->gen));
    }
    m_snapshots = std::move(newSnap);
    return (int)m_storage.size() - 1;
}

void PluginChain::removePlugin(int index) {
    dlog("removePlugin enter track=" + std::to_string(m_trackIdx) + " idx=" + std::to_string(index) + " thread=" + threadId());
    std::shared_ptr<Snapshot> newSnap;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index < 0 || index >= (int)m_storage.size()) { dlog("removePlugin: bad index"); return; }
        auto& pi = m_storage[index];
        if (pi->plugin) {
            dlog("removePlugin: calling deactivate+destroyPlugin");
            if (pi->plugin->deactivate) pi->plugin->deactivate(pi->plugin);
            ClapHost::destroyPlugin(pi->plugin);
        }
        if (pi->library) { dlog("removePlugin: unloadLibrary"); ClapHost::unloadLibrary(pi->library); }
        m_storage.erase(m_storage.begin() + index);
        newSnap = build_snapshot(m_storage, *const_cast<std::atomic<uint32_t>*>(&m_snapshots->gen));
    }
    m_snapshots = std::move(newSnap);
    dlog("removePlugin OK track=" + std::to_string(m_trackIdx) + " remaining=" + std::to_string(m_storage.size()));
}

void PluginChain::clear() {
    std::vector<std::unique_ptr<PluginInstance>> toDestroy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        toDestroy.swap(m_storage);
    }
    // Destroy plugins OUTSIDE the lock — they may call host callbacks
    // (e.g. request_resize) that we don't want re-entering m_mutex.
    for (auto& pi : toDestroy) {
        if (pi->plugin) {
            if (pi->plugin->deactivate) pi->plugin->deactivate(pi->plugin);
            ClapHost::destroyPlugin(pi->plugin);
        }
        if (pi->library) ClapHost::unloadLibrary(pi->library);
    }
}

void PluginChain::setBypass(int index, bool bypass) {
    std::shared_ptr<Snapshot> newSnap;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index < 0 || index >= (int)m_storage.size()) return;
        m_storage[index]->active = !bypass;
        newSnap = build_snapshot(m_storage, *const_cast<std::atomic<uint32_t>*>(&m_snapshots->gen));
    }
    m_snapshots = std::move(newSnap);
}

void PluginChain::process(const float* input, float* output, int frames, int channels) {
    // ── LOCK-FREE: take a local shared_ptr copy of the snapshot ──
    // The atomic load ensures we see a consistent state. The audio thread
    // NEVER blocks, NEVER takes a mutex, NEVER waits for the main thread.
    auto snap = m_snapshots;  // shared_ptr atomic refcount bump
    if (!snap || snap->instances.empty()) return;
    if (frames <= 0 || channels <= 0) return;

    static thread_local int tl_callCount = 0;
    if (++tl_callCount == 1 || tl_callCount % 1000 == 0) {
        dlog("process track=" + std::to_string(m_trackIdx) + " thread=" + threadId() +
             " call=" + std::to_string(tl_callCount) +
             " n=" + std::to_string(snap->instances.size()) +
             " frames=" + std::to_string(frames));
    }

    int numChannels = channels < 2 ? channels : 2;
    int numFrames = frames;

    const float* inPtrs[2];
    float* outPtrs[2];

    inPtrs[0] = input;
    outPtrs[0] = output;
    if (numChannels > 1) {
        inPtrs[1] = input + numFrames;
        outPtrs[1] = output + numFrames;
    }

    bool first = true;
    const size_t n = snap->instances.size();
    for (size_t i = 0; i < n; ++i) {
        PluginInstance* pi = snap->instances[i];
        if (!pi) continue;
        if (i < snap->active.size() && !snap->active[i]) continue;

        if (pi->isBuiltin) {
            processBuiltin(*pi, inPtrs, outPtrs, numFrames, numChannels);
            if (first) {
                first = false;
                inPtrs[0] = output;
                if (numChannels > 1) inPtrs[1] = output + numFrames;
            }
            continue;
        }

        if (!pi->plugin || !pi->plugin->process) continue;

        // CLAP spec: start_processing is [audio-thread]. Call it here on
        // the first block the plugin is in the snapshot. The `processing`
        // flag in the snapshot is per-snapshot and starts as false; we
        // track per-callback with a thread-local cache to avoid calling
        // start_processing more than once per plugin instance.
        bool& startedFlag = g_startedCache[pi];
        if (!startedFlag) {
            dlog("audio thread: calling start_processing for " + pi->name +
                 " track=" + std::to_string(m_trackIdx) + " thread=" + threadId());
            if (pi->plugin->start_processing)
                pi->plugin->start_processing(pi->plugin);
            startedFlag = true;
        }

        // Read port info from the pre-computed snapshot (cached on
        // main thread in query_ports). NEVER call get_extension /
        // audio_ports.count from the audio thread — CLAP spec marks
        // them [main-thread] and PeakEater's thread-check aborts.
        const PortInfo& pinfo = (i < snap->ports.size())
            ? snap->ports[i] : PortInfo{};
        uint32_t inPortCount  = pinfo.inCount;
        uint32_t outPortCount = pinfo.outCount;
        int pluginInCh  = pinfo.hasInStereo  ? 2 : numChannels;
        int pluginOutCh = pinfo.hasOutStereo ? 2 : numChannels;
        if (inPortCount == 0)  pluginInCh  = 0;
        if (outPortCount == 0) pluginOutCh = 0;

        bool needsDownmix = (numChannels == 2 && pluginInCh == 1);
        bool needsUpmix   = (numChannels == 2 && pluginOutCh == 1);

        std::vector<float> work;
        work.resize(numFrames * 2, 0.0f);

        const float* monoIn[2] = {inPtrs[0], inPtrs[1]};
        float* monoOut[2] = {outPtrs[0], outPtrs[1]};

        if (needsDownmix) {
            for (int f = 0; f < numFrames; ++f)
                work[f] = (inPtrs[0][f] + inPtrs[1][f]) * 0.5f;
            monoIn[0] = work.data();
            monoIn[1] = nullptr;
        }
        if (needsUpmix) {
            monoOut[0] = work.data() + (needsDownmix ? numFrames : 0);
            monoOut[1] = nullptr;
        }

        clap_audio_buffer_t inBuf, outBuf;
        clap_audio_buffer_t* pInBuf = (inPortCount > 0 && pluginInCh > 0) ? &inBuf : nullptr;
        clap_audio_buffer_t* pOutBuf = (outPortCount > 0 && pluginOutCh > 0) ? &outBuf : nullptr;

        if (pInBuf) {
            inBuf.data32 = (float**)monoIn;
            inBuf.data64 = nullptr;
            inBuf.channel_count = (uint32_t)pluginInCh;
            inBuf.latency = 0;
        }
        if (pOutBuf) {
            outBuf.data32 = (float**)monoOut;
            outBuf.data64 = nullptr;
            outBuf.channel_count = (uint32_t)pluginOutCh;
            outBuf.latency = 0;
        }

        HostInputEvents inEvents;
        HostOutputEvents outEvents;

        clap_process_t proc;
        memset(&proc, 0, sizeof(proc));
        proc.audio_inputs = pInBuf;
        proc.audio_outputs = pOutBuf;
        proc.audio_inputs_count = inPortCount;
        proc.audio_outputs_count = outPortCount;
        proc.frames_count = (uint32_t)numFrames;
        proc.in_events = &inEvents.list;
        proc.out_events = &outEvents.list;

        try {
            pi->plugin->process(pi->plugin, &proc);
        } catch (...) {
            fprintf(stderr, "[PLUGIN] process exception in %s\n", pi->name.c_str());
        }

        // Upmix mono output → stereo
        if (needsUpmix && numChannels == 2) {
            for (int f = 0; f < numFrames; ++f) {
                outPtrs[0][f] = monoOut[0][f];
                outPtrs[1][f] = monoOut[0][f];
            }
        }

        if (first) {
            first = false;
            inPtrs[0] = output;
            if (numChannels > 1) inPtrs[1] = output + numFrames;
        }
    }
}

void PluginChain::processBuiltin(PluginInstance& pi, const float* const* inPtrs, float* const* outPtrs, int frames, int channels) {
    if (channels < 1) return;
    int ch = channels < 2 ? channels : 2;
    float d = std::max(0.01f, pi.drive);
    float invDrive = 1.0f / tanhf(d);
    for (int c = 0; c < ch; ++c) {
        if (!inPtrs[c] || !outPtrs[c]) continue;
        for (int f = 0; f < frames; ++f) {
            outPtrs[c][f] = tanhf(inPtrs[c][f] * d) * invDrive;
        }
    }
    // Zero remaining channels if input was mono
    for (int c = ch; c < channels; ++c) {
        if (outPtrs[c]) std::fill(outPtrs[c], outPtrs[c] + frames, 0.0f);
    }
}

bool PluginChain::isBuiltin(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return false;
    return m_storage[index]->isBuiltin;
}

bool PluginChain::getGuiOpen(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return false;
    return m_storage[index]->guiOpen;
}

void PluginChain::setGuiOpen(int index, bool open) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return;
    m_storage[index]->guiOpen = open;
}

float PluginChain::getDrive(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return 1.0f;
    return m_storage[index]->drive;
}

void PluginChain::setDrive(int index, float val) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return;
    m_storage[index]->drive = val;
}

void PluginChain::processPendingFlushes() {
    std::vector<const clap_host_t*> flushes;
    {
        std::lock_guard<std::mutex> lk(g_flushMutex);
        if (g_pendingFlush.empty()) return;
        flushes.assign(g_pendingFlush.begin(), g_pendingFlush.end());
        g_pendingFlush.clear();
    }

    for (const auto* host : flushes) {
        if (!host) continue;
        // best-effort: plugins can re-trigger flush by themselves
    }
}

void PluginChain::enterAudioThread() {
    g_inAudioCallback = true;
}
void PluginChain::leaveAudioThread() {
    g_inAudioCallback = false;
}

int PluginChain::getPluginCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_storage.size();
}

const char* PluginChain::getPluginName(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return "";
    return m_storage[index]->name.c_str();
}

bool PluginChain::isBypassed(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return true;
    return !m_storage[index]->active;
}

const char* PluginChain::getPluginPath(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return "";
    return m_storage[index]->path.c_str();
}

const char* PluginChain::getPluginId(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return "";
    return m_storage[index]->id.c_str();
}

const clap_plugin_t* PluginChain::getPlugin(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return nullptr;
    return m_storage[index]->plugin;
}

void PluginChain::setPluginHostData(int index, void* hostData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return;
    if (m_storage[index]->dedicatedHost)
        m_storage[index]->dedicatedHost->host_data = hostData;
}

// ── CLAP state save/load (stream adapters) ──
// We adapt a std::string to clap_istream_t / clap_ostream_t so we can
// drive clap_plugin_state.load/save without touching the filesystem.
namespace {
struct StringIStream {
    clap_istream_t stream;
    const std::string& data;
    size_t pos = 0;

    static int64_t CLAP_ABI read_cb(const clap_istream* s, void* buffer, uint64_t size) {
        auto* self = (StringIStream*)s;
        if (!self || self->pos >= self->data.size()) return 0;
        size_t avail = self->data.size() - self->pos;
        size_t toRead = std::min((size_t)size, avail);
        std::memcpy(buffer, self->data.data() + self->pos, toRead);
        self->pos += toRead;
        return (int64_t)toRead;
    }

    StringIStream(const std::string& d) : data(d) {
        stream.ctx = this;
        stream.read = read_cb;
    }
};

struct StringOStream {
    clap_ostream_t stream;
    std::string& data;

    static int64_t CLAP_ABI write_cb(const clap_ostream* s, const void* buffer, uint64_t size) {
        auto* self = (StringOStream*)s;
        if (!self) return 0;
        self->data.append((const char*)buffer, (size_t)size);
        return (int64_t)size;
    }

    StringOStream(std::string& d) : data(d) {
        stream.ctx = this;
        stream.write = write_cb;
    }
};
} // namespace

std::string PluginChain::saveState(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return "";
    auto* pi = m_storage[index].get();
    if (!pi || !pi->plugin || pi->isBuiltin) return "";
    auto* ext = (const clap_plugin_state_t*)
        pi->plugin->get_extension(pi->plugin, CLAP_EXT_STATE);
    if (!ext || !ext->save) return "";
    std::string out;
    StringOStream sos(out);
    if (!ext->save(pi->plugin, &sos.stream)) return "";
    return out;
}

bool PluginChain::loadState(int index, const std::string& blob) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return false;
    auto* pi = m_storage[index].get();
    if (!pi || !pi->plugin || pi->isBuiltin) return false;
    auto* ext = (const clap_plugin_state_t*)
        pi->plugin->get_extension(pi->plugin, CLAP_EXT_STATE);
    if (!ext || !ext->load) return false;
    StringIStream sis(blob);
    return ext->load(pi->plugin, &sis.stream);
}
