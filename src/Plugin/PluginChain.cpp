#include "PluginChain.h"
#include "ClapHost.h"
#include <cstring>
#include <mutex>
#include <cstdio>
#include <vector>
#include <cmath>
#include <pthread.h>
#include <unordered_map>
#include <memory>

// CLAP extensions
#include "ext/timer-support.h"
#include "ext/log.h"
#include "ext/params.h"
#include "ext/audio-ports.h"
#include "ext/state.h"

#include <set>

// ── Deferred snapshot delete queue ──
// The main thread publishes new snapshots via exchange() and queues the
// old one here. The audio thread drains the queue at the end of each
// callback (drainPendingDeletes). This guarantees the old Snapshot stays
// alive while any audio thread might still be reading it.
std::mutex PluginChain::s_pendingDeleteMutex;
std::vector<PluginChain::Snapshot*> PluginChain::s_pendingDeletes;

// ── Pending flush tracking (for processPendingFlushes on main thread) ──
static std::mutex g_flushMutex;
std::set<const clap_host_t*> g_pendingFlush;

// ── CLAP event structures ──

struct HostInputEvents {
    clap_input_events_t list;
    std::vector<const clap_event_header_t*> events;
    // Storage for events embedded by callers. The pointers in `events`
    // reference these so the events remain valid until the caller
    // releases the HostInputEvents.
    clap_event_param_value_t paramVal{};

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

    void pushParamValue(uint32_t paramId, double value) {
        paramVal.header.size = sizeof(paramVal);
        paramVal.header.time = 0;
        paramVal.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        paramVal.header.type = CLAP_EVENT_PARAM_VALUE;
        paramVal.header.flags = 0;
        paramVal.param_id = paramId;
        paramVal.value = value;
        paramVal.cookie = nullptr;
        events.push_back(&paramVal.header);
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

// CLAP_LOG_* values per clap/ext/log.h
static hydraw::ILogSink::Level clapLogToLevel(int severity) {
    // 0=HOST MISUSE, 1=ERROR, 2=WARNING, 3=INFO, 4=DEBUG
    switch (severity) {
        case 0: return hydraw::ILogSink::Level::Error;
        case 1: return hydraw::ILogSink::Level::Error;
        case 2: return hydraw::ILogSink::Level::Warn;
        case 3: return hydraw::ILogSink::Level::Info;
        case 4: return hydraw::ILogSink::Level::Debug;
        default: return hydraw::ILogSink::Level::Error;
    }
}

static void clap_host_log(const clap_host_t* host, clap_log_severity severity, const char* msg) {
    // The static function installed in s_hostTemplate; can be
    // called from ANY thread (audio, plugin worker, main). Forward
    // to the chain's ILogSink via a thread_local "current chain"
    // sentinel. The sentinel is set by addPlugin() around
    // createPlugin/init/activate (the only places get_extension
    // and log are likely to be called by the plugin).
    (void)host;
    extern thread_local PluginChain* t_currentChain;
    PluginChain* chain = t_currentChain;
    if (chain) {
        chain->logFromClap(severity, msg ? msg : "(null)");
        return;
    }
    // Last-resort fallback (init time only). The audio thread
    // should never hit this when the bridge provided a sink.
    static const char* names[] = {"HOST", "ERROR", "WARNING", "INFO", "DEBUG"};
    int idx = (int)severity;
    if (idx < 0 || idx > 4) idx = 1;
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
// A thread-local flag set by the audio callback. Plugins call
// is_audio_thread() during their process() impl to verify the host
// is calling process() on the audio thread.
//
// (Previously we also tracked g_mainThreadId and called
// initMainThreadId() from main(). This was vestigial — the
// is_main_thread check accepted any non-audio thread, so the
// pthread_t comparison was never read. Removed in the bridge
// refactor.)
static thread_local bool g_inAudioCallback = false;

// Thread-local cache of "we already called start_processing on this
// plugin instance". Maps packed (chain|instance) pointer-pair to bool.
// The pair key is required because the audio thread processes multiple
// chains per block (e.g. track 0..N-1, then master) and each chain
// has its own independent snapshot generation. With a single global
// key the cache would self-invalidate on every chain's process() call,
// defeating its purpose and triggering start_processing on every block
// for every plugin (this was a real perf bug, see
// PERSISTENCE_SPEC.md / HIGH-1).
//
// We use a packed uintptr_t key (chain in high 32 bits, instance in
// low 32 bits) because std::unordered_map<std::pair<...>> has no
// std::hash specialization and writing a custom hasher for every
// compiler's std::hash<T*> implementation is fragile. The two pointers
// are truncated to 32 bits each — safe in practice (no DAW has 4B
// live PluginInstances) and avoids the hasher issue entirely.
static inline uint64_t packPtr(const PluginChain* c, const PluginInstance* p) {
    return ((uint64_t)(uintptr_t)c << 32) | (uint64_t)(uintptr_t)p;
}
static thread_local std::unordered_map<uint64_t, bool> g_startedCache;
static thread_local std::unordered_map<const PluginChain*, uint32_t> g_lastSnapGen;

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

// Thread-local "current chain" sentinel. The static
// `clap_get_extension` and `clap_host_log` functions can be called
// from any thread (the plugin chooses). They need a way to find
// the right PluginChain (and thus its IHostExtensions* and
// ILogSink*). The PluginContext (stored in host_data) doesn't point
// back to the chain directly; instead we use a thread_local
// pointer set by addPlugin() (the only place that creates
// dedicatedHost with a PluginContext) and read by the static
// CLAP callbacks. Since plugin init/activate happens on the main
// thread, and the audio thread dereferences the returned extension
// pointers in process() (which are stable for the plugin
// lifetime), this thread_local is set in the main thread at init
// time and the audio thread only reads the resolved extension
// pointers, not the thread_local itself.
thread_local PluginChain* t_currentChain = nullptr;

static const void* clap_get_extension(const clap_host_t* host, const char* extension_id) {
    // GUI: bridge-provided via the chain's IHostExtensions.
    // The static function can be called from any thread. We use
    // a thread_local "current chain" sentinel that addPlugin()
    // sets before createPlugin() (which is the only place that
    // may call get_extension) and clears after. The audio thread
    // only reads the returned extension pointers (which are
    // stable for the plugin's lifetime), not this thread_local.
    if (std::strcmp(extension_id, CLAP_EXT_GUI) == 0) {
        PluginChain* chain = t_currentChain;
        if (!chain) return nullptr;
        return chain->getHostExt(CLAP_EXT_GUI);
    }
    // Other extensions stay in the audio core.
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

PluginChain::PluginChain(int sampleRate, int blockSize, int trackIndex,
                         hydraw::IHostExtensions* hostExt,
                         hydraw::ILogSink* logSink)
    : m_sampleRate(sampleRate)
    , m_blockSize(blockSize)
    , m_trackIdx(trackIndex)
    , m_hostExt(hostExt)
    , m_logSink(logSink)
{
    m_scratchBuffer.resize(blockSize * 2, 0.0f);
    // Initial empty snapshot — heap-allocated, published via m_atomicSnapshot.
    // Never deleted (it's the first snapshot and stays live until replaced).
    auto* snap = new Snapshot();
    m_gen.store(0, std::memory_order_release);
    snap->gen.store(0, std::memory_order_release);
    m_atomicSnapshot.store(snap, std::memory_order_release);
}

// Accessor used by the file-scope static `clap_host_log` to forward
// plugin-emitted log messages to the chain's ILogSink. Public so the
// static (file-scope) function can call it without needing friend
// access to private members.
void PluginChain::logFromClap(int clapSeverity, const char* msg) noexcept {
    if (m_logSink) {
        m_logSink->log(clapLogToLevel(clapSeverity), msg);
    }
}

// Accessor used by the file-scope static `clap_get_extension` to
// resolve the GUI extension via the chain's IHostExtensions.
const void* PluginChain::getHostExt(const char* extension_id) const noexcept {
    return m_hostExt ? m_hostExt->getExtension(extension_id) : nullptr;
}

PluginChain::~PluginChain() {
    clear();
    // Drain any snapshots still pending (e.g. the pre-clear snapshot).
    // By this point the audio thread is dead, so no concurrent readers.
    std::vector<Snapshot*> toFree;
    {
        std::lock_guard<std::mutex> lk(s_pendingDeleteMutex);
        toFree.swap(s_pendingDeletes);
    }
    for (auto* s : toFree) delete s;
    // Also free the current atomic snapshot.
    Snapshot* cur = m_atomicSnapshot.load(std::memory_order_relaxed);
    if (cur) { delete cur; m_atomicSnapshot.store(nullptr, std::memory_order_relaxed); }
}

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
            info.inCh = pInfo.channel_count;
            info.hasInStereo = (pInfo.channel_count == 2);
        }
    }
    if (info.outCount > 0 && ap->get) {
        clap_audio_port_info_t pInfo;
        if (ap->get(pi->plugin, 0, false, &pInfo)) {
            info.outCh = pInfo.channel_count;
            info.hasOutStereo = (pInfo.channel_count == 2);
        }
    }
    info.supportsStereo = info.hasInStereo && info.hasOutStereo;
    return info;
}

PluginChain::Snapshot* build_snapshot(
        std::vector<std::shared_ptr<PluginInstance>>& storage,
        std::atomic<uint32_t>& genCounter) {
    auto* snap = new PluginChain::Snapshot();
    snap->instances.reserve(storage.size());
    snap->active.reserve(storage.size());
    snap->ports.reserve(storage.size());
    snap->processing.reserve(storage.size());
    for (auto& pi : storage) {
        snap->instances.push_back(pi);
        snap->active.push_back(pi->active);
        snap->ports.push_back(query_ports(pi.get()));
        snap->processing.push_back(false);
    }
    uint32_t g = genCounter.load(std::memory_order_acquire) + 1;
    genCounter.store(g, std::memory_order_release);
    snap->gen.store(g, std::memory_order_release);
    return snap;
}
} // namespace

void PluginChain::publishSnapshot(Snapshot* snap) {
    Snapshot* old = m_atomicSnapshot.exchange(snap, std::memory_order_acq_rel);
    if (old) {
        std::lock_guard<std::mutex> lk(s_pendingDeleteMutex);
        s_pendingDeletes.push_back(old);
    }
}

void PluginChain::drainPendingDeletes() {
    std::vector<Snapshot*> toFree;
    {
        std::lock_guard<std::mutex> lk(s_pendingDeleteMutex);
        if (s_pendingDeletes.empty()) return;
        toFree.swap(s_pendingDeletes);
    }
    for (auto* s : toFree) delete s;
}

bool PluginChain::addPlugin(const char* libraryPath, const char* pluginId) {
    auto log = [this](const std::string& m) {
        if (m_logSink) m_logSink->log(hydraw::ILogSink::Level::Debug, m.c_str());
    };
    log("addPlugin enter track=" + std::to_string(m_trackIdx) + " id=" + std::string(pluginId));
    // ── Phase A: load + init + activate WITHOUT holding m_mutex ──
    // (these are slow calls, we don't want to block other main-thread ops)
    void* lib = ClapHost::loadLibrary(libraryPath);
    if (!lib) { log("addPlugin: loadLibrary failed for " + std::string(libraryPath)); return false; }

    auto dedicatedHost = std::make_unique<clap_host_t>(s_hostTemplate);
    // Allocate a PluginContext for this instance. The bridge later
    // sets the handle via setPluginHandle(). For now we just store
    // the hostExt pointer (which doesn't change).
    auto ctx = std::make_unique<PluginContext>();
    ctx->hostExt = m_hostExt;
    int ctxIndex = (int)m_contexts.size();
    // Set host_data to a "back-reference" so the static
    // clap_get_extension can find the chain. We use a
    // thread_local pattern: the static callback reads the
    // current chain from t_currentChain, which addPlugin() sets
    // before createPlugin() and clears after. The PluginContext*
    // is stored in host_data for the bridge to write the handle.
    dedicatedHost->host_data = (void*)this;  // temporary: chain ptr for static get_extension
    t_currentChain = this;

    const clap_plugin_t* plugin = ClapHost::createPlugin(lib, dedicatedHost.get(), pluginId);
    if (!plugin) {
        log("addPlugin: createPlugin failed id=" + std::string(pluginId));
        t_currentChain = nullptr;
        ClapHost::unloadLibrary(lib, libraryPath);
        return false;
    }
    log("addPlugin: plugin created, calling init");

    bool ok = plugin->init ? plugin->init(plugin) : true;
    if (!ok) {
        log("addPlugin: plugin->init returned false");
        t_currentChain = nullptr;
        ClapHost::destroyPlugin(plugin);
        ClapHost::unloadLibrary(lib, libraryPath);
        return false;
    }

    if (plugin->activate &&
        !plugin->activate(plugin, (double)m_sampleRate, (uint32_t)m_blockSize, (uint32_t)m_blockSize)) {
        log("addPlugin: plugin->activate returned false");
        t_currentChain = nullptr;
        plugin->destroy(plugin);
        ClapHost::unloadLibrary(lib, libraryPath);
        return false;
    }
    log("addPlugin: activate OK");

    // NOTE: start_processing is [audio-thread] per spec. We do NOT call
    // it here — the audio callback calls it on the first block for any
    // plugin in the snapshot whose `processing` flag is false.

    // ── Phase B: brief critical section — add to storage + publish snapshot ──
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto pi = std::make_shared<PluginInstance>();
        pi->library = lib;
        pi->plugin = plugin;
        pi->dedicatedHost = std::move(dedicatedHost);
        pi->name = pluginId;
        pi->path = libraryPath;
        pi->id = pluginId;
        pi->active = true;
        m_storage.push_back(std::move(pi));
        m_contexts.push_back(std::move(ctx));
        publishSnapshot(build_snapshot(m_storage, m_gen));
    }
    t_currentChain = nullptr;
    log("addPlugin OK track=" + std::to_string(m_trackIdx) + " idx=" + std::to_string(m_storage.size() - 1));
    return true;
}

int PluginChain::addBuiltinSoftClipper() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto pi = std::make_shared<PluginInstance>();
        pi->isBuiltin = true;
        pi->active = true;
        pi->name = "Soft Clipper";
        pi->path = "builtin";
        pi->id = "builtin:softclipper";
        pi->drive = 1.0f;
        m_storage.push_back(std::move(pi));
        publishSnapshot(build_snapshot(m_storage, m_gen));
    }
    return (int)m_storage.size() - 1;
}

void PluginChain::removePlugin(int index) {
    auto log = [this](const std::string& m) {
        if (m_logSink) m_logSink->log(hydraw::ILogSink::Level::Debug, m.c_str());
    };
    log("removePlugin enter track=" + std::to_string(m_trackIdx) + " idx=" + std::to_string(index));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index < 0 || index >= (int)m_storage.size()) { log("removePlugin: bad index"); return; }
        auto& pi = m_storage[index];
        if (pi->plugin) {
            if (pi->started && pi->plugin->stop_processing)
                pi->plugin->stop_processing(pi->plugin);
            pi->started = false;
            if (pi->plugin->deactivate) pi->plugin->deactivate(pi->plugin);
            ClapHost::destroyPlugin(pi->plugin);
            pi->plugin = nullptr;
        }
        if (pi->library) {
            ClapHost::unloadLibrary(pi->library, pi->path.c_str());
            pi->library = nullptr;
        }
        m_storage.erase(m_storage.begin() + index);
        if ((int)m_contexts.size() > index) m_contexts.erase(m_contexts.begin() + index);
        publishSnapshot(build_snapshot(m_storage, m_gen));
    }
    log("removePlugin OK track=" + std::to_string(m_trackIdx) + " remaining=" + std::to_string(m_storage.size()));
}

void PluginChain::movePlugin(int from, int to) {
    auto log = [this](const std::string& m) {
        if (m_logSink) m_logSink->log(hydraw::ILogSink::Level::Debug, m.c_str());
    };
    log("movePlugin enter track=" + std::to_string(m_trackIdx) +
         " from=" + std::to_string(from) + " to=" + std::to_string(to));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = (int)m_storage.size();
        if (n < 2) { log("movePlugin: nothing to move"); return; }
        if (from < 0 || from >= n) { log("movePlugin: bad from"); return; }
        if (to < 0 || to >= n)      { log("movePlugin: bad to"); return; }
        if (from == to) { log("movePlugin: from==to, no-op"); return; }
        auto pi = m_storage[from];
        m_storage.erase(m_storage.begin() + from);
        m_storage.insert(m_storage.begin() + to, pi);
        // Move the matching PluginContext too.
        if (from < (int)m_contexts.size() && to < (int)m_contexts.size()) {
            auto ctx = std::move(m_contexts[from]);
            m_contexts.erase(m_contexts.begin() + from);
            m_contexts.insert(m_contexts.begin() + to, std::move(ctx));
        }
        publishSnapshot(build_snapshot(m_storage, m_gen));
    }
    log("movePlugin OK track=" + std::to_string(m_trackIdx));
}

void PluginChain::clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_storage.clear();
    }
    m_gen.store(0, std::memory_order_release);
    auto* emptySnap = new Snapshot();
    emptySnap->gen.store(0, std::memory_order_release);
    publishSnapshot(emptySnap);
}

void PluginChain::setBypass(int index, bool bypass) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index < 0 || index >= (int)m_storage.size()) return;
        m_storage[index]->active = !bypass;
        publishSnapshot(build_snapshot(m_storage, m_gen));
    }
}

void PluginChain::process(const float* input, float* output, int frames, int channels) {
    // ── LOCK-FREE: atomic load of the raw Snapshot* ──
    // No mutex, no deadlock. The main thread queues old snapshots for
    // deferred deletion (drainPendingDeletes) so our pointer stays
    // valid for the duration of this call.
    auto* snap = m_atomicSnapshot.load(std::memory_order_acquire);
    if (!snap || snap->instances.empty()) return;
    if (frames <= 0 || channels <= 0) return;

    static thread_local int tl_callCount = 0;
    if (m_logSink && (++tl_callCount == 1 || tl_callCount % 1000 == 0)) {
        m_logSink->log(hydraw::ILogSink::Level::Debug,
            ("process track=" + std::to_string(m_trackIdx) +
             " call=" + std::to_string(tl_callCount) +
             " n=" + std::to_string(snap->instances.size()) +
             " frames=" + std::to_string(frames)).c_str());
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

    // Thread-local work buffer for downmix/upmix. Sized to the block size
    // × 2 channels — stack-friendly and avoids heap allocation in the
    // real-time audio callback. BLOCK_SIZE from Constants.h is always ≥
    // frames for the configured device, but we allocate m_blockSize which
    // was set in the constructor and is >= any frame count we see.
    static thread_local float tlsWork[512]; // 256 frames × 2 channels
    if (numFrames > (int)(sizeof(tlsWork)/sizeof(tlsWork[0])/2))
        numFrames = (int)(sizeof(tlsWork)/sizeof(tlsWork[0])/2);

    // Per-chain snapshot generation tracking (fixes HIGH-1). The
    // audio thread processes each chain independently; the cache is
    // keyed by (chain, PluginInstance*) so chains with stable gens
    // don't get their cache invalidated by a peer chain's swap.
    uint32_t thisSnapGen = snap->gen.load(std::memory_order_acquire);
    auto genIt = g_lastSnapGen.find(this);
    bool needPrime = (genIt == g_lastSnapGen.end()) || (genIt->second != thisSnapGen);
    if (needPrime) {
        // Drop entries for this chain from the started cache (keep
        // entries for other chains intact). Stale entries with
        // address-reused PluginInstance* would otherwise incorrectly
        // skip start_processing.
        const uintptr_t chainAddr = (uintptr_t)this;
        const uintptr_t chainMask = ~((uintptr_t)0xFFFFFFFFu);  // high 32 bits
        for (auto it = g_startedCache.begin(); it != g_startedCache.end(); ) {
            if ((it->first & chainMask) == (chainAddr << 32)) it = g_startedCache.erase(it);
            else ++it;
        }
        for (auto& pi : snap->instances) {
            if (pi && pi->started)
                g_startedCache[packPtr(this, pi.get())] = true;
        }
        g_lastSnapGen[this] = thisSnapGen;
    }

    // Allocate buffer arrays for multi-port plugins (e.g. crossover with
    // 3 output bands). Cap at 4 to prevent stack overflow — no CLAP
    // plugin in practice exceeds 4 ports.
    static constexpr int kMaxPorts = 4;
    clap_audio_buffer_t inBufs[kMaxPorts];
    clap_audio_buffer_t outBufs[kMaxPorts];

    bool first = true;
    const size_t n = snap->instances.size();
    for (size_t i = 0; i < n; ++i) {
        auto& pi = snap->instances[i];
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

        // CLAP spec: start_processing is [audio-thread]
        bool& startedFlag = g_startedCache[packPtr(this, pi.get())];
        if (!startedFlag) {
            if (m_logSink) {
                m_logSink->log(hydraw::ILogSink::Level::Debug,
                    ("audio thread: calling start_processing for " + pi->name +
                     " track=" + std::to_string(m_trackIdx)).c_str());
            }
            if (pi->plugin->start_processing)
                pi->plugin->start_processing(pi->plugin);
            pi->started = true;
            startedFlag = true;
        }

        // Read cached port info from snapshot (queried on main thread).
        const PortInfo& pinfo = (i < snap->ports.size())
            ? snap->ports[i] : PortInfo{};

        uint32_t inPortCount  = std::min(pinfo.inCount, (uint32_t)kMaxPorts);
        uint32_t outPortCount = std::min(pinfo.outCount, (uint32_t)kMaxPorts);

        int pluginInCh;
        if (pinfo.inCount == 0)    pluginInCh = 0;
        else if (pinfo.inCh > 0)   pluginInCh = (int)pinfo.inCh;
        else                       pluginInCh = numChannels;
        int pluginOutCh;
        if (pinfo.outCount == 0)   pluginOutCh = 0;
        else if (pinfo.outCh > 0)  pluginOutCh = (int)pinfo.outCh;
        else                       pluginOutCh = numChannels;

        bool needsDownmix = (numChannels == 2 && pluginInCh == 1);
        bool needsUpmix   = (numChannels == 2 && pluginOutCh == 1);

        std::memset(tlsWork, 0, sizeof(tlsWork));

        const float* monoIn[2]  = {inPtrs[0],  inPtrs[1]};
        float*       monoOut[2] = {outPtrs[0], outPtrs[1]};

        if (needsDownmix) {
            for (int f = 0; f < numFrames; ++f)
                tlsWork[f] = (inPtrs[0][f] + inPtrs[1][f]) * 0.5f;
            monoIn[0] = tlsWork;
            monoIn[1] = nullptr;
        }
        if (needsUpmix) {
            monoOut[0] = tlsWork + (needsDownmix ? numFrames : 0);
            monoOut[1] = nullptr;
        }

        // Set up port buffers — all input ports point to the same input
        // data, all output ports point to the same output data. This
        // prevents crashes with multi-port plugins (e.g. a 3-band
        // crossover) while preserving correct pass-through for single-port
        // plugins. Multi-port plugins will overwrite each other's output
        // on shared buffers, but signal passes through without crashing.
        for (uint32_t p = 0; p < inPortCount; ++p) {
            inBufs[p].data32        = const_cast<float**>(monoIn);
            inBufs[p].data64        = nullptr;
            inBufs[p].channel_count = (uint32_t)pluginInCh;
            inBufs[p].latency       = 0;
        }
        for (uint32_t p = 0; p < outPortCount; ++p) {
            outBufs[p].data32        = monoOut;
            outBufs[p].data64        = nullptr;
            outBufs[p].channel_count = (uint32_t)pluginOutCh;
            outBufs[p].latency       = 0;
        }

        clap_audio_buffer_t* pInBuf  = (inPortCount > 0 && pluginInCh > 0) ? inBufs : nullptr;
        clap_audio_buffer_t* pOutBuf = (outPortCount > 0 && pluginOutCh > 0) ? outBufs : nullptr;

        HostInputEvents inEvents;
        HostOutputEvents outEvents;

        clap_process_t proc;
        memset(&proc, 0, sizeof(proc));
        proc.audio_inputs       = pInBuf;
        proc.audio_outputs      = pOutBuf;
        proc.audio_inputs_count  = inPortCount;
        proc.audio_outputs_count = outPortCount;
        proc.frames_count       = (uint32_t)numFrames;
        proc.in_events          = &inEvents.list;
        proc.out_events         = &outEvents.list;

        pi->plugin->process(pi->plugin, &proc);

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

// guiOpen/setGuiOpen were UI state and have been moved to the
// bridge (the bridge owns its own "is GUI open" map keyed by
// the stable handle set via setPluginHandle).

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

void PluginChain::processPendingFlushes(const std::vector<PluginChain*>& chains) {
    std::vector<const clap_host_t*> flushes;
    {
        std::lock_guard<std::mutex> lk(g_flushMutex);
        if (g_pendingFlush.empty()) return;
        flushes.assign(g_pendingFlush.begin(), g_pendingFlush.end());
        g_pendingFlush.clear();
    }

    // For each host that requested a flush, walk every plugin in
    // m_storage and call clap_plugin_params_t::flush on it. The plugin
    // pushes parameter change events to our output events. We don't
    // consume those events here yet (no parameter automation UI), but
    // the round-trip to the plugin is what most CLAP plugins rely on
    // to refresh their internal state and to repaint their GUIs.
    clap_output_events_t outEvents{};
    outEvents.ctx = nullptr;
    outEvents.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool {
        return true;
    };
    for (const auto* host : flushes) {
        if (!host) continue;
        for (PluginChain* chain : chains) {
            if (!chain) continue;
            std::lock_guard<std::mutex> lock(chain->m_mutex);
            for (auto& pi : chain->m_storage) {
                if (!pi->plugin) continue;
                auto* params = (const clap_plugin_params_t*)pi->plugin->get_extension(
                    pi->plugin, CLAP_EXT_PARAMS);
                if (!params || !params->flush) continue;
                params->flush(pi->plugin, nullptr, &outEvents);
            }
        }
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

bool PluginChain::hasPlugins() const {
    auto* s = m_atomicSnapshot.load(std::memory_order_acquire);
    return s && !s->instances.empty();
}

uint32_t PluginChain::getLatency() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t total = 0;
    for (const auto& pi : m_storage) {
        if (!pi->plugin) continue;
        auto* lat = (const clap_plugin_latency_t*)pi->plugin->get_extension(
            pi->plugin, CLAP_EXT_LATENCY);
        if (lat && lat->get) {
            total += lat->get(pi->plugin);
        }
    }
    return total;
}

std::vector<PluginChain::ParamInfo> PluginChain::getPluginParams(int index) const {
    std::vector<ParamInfo> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return out;
    auto& pi = m_storage[index];
    if (!pi->plugin) return out;
    auto* params = (const clap_plugin_params_t*)pi->plugin->get_extension(
        pi->plugin, CLAP_EXT_PARAMS);
    if (!params || !params->count || !params->get_info) return out;
    uint32_t n = params->count(pi->plugin);
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        if (!params->get_info(pi->plugin, i, &info)) continue;
        ParamInfo p;
        p.id = info.id;
        p.name = info.name;
        p.min = info.min_value;
        p.max = info.max_value;
        p.defaultValue = info.default_value;
        p.isAutomatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        if (params->get_value) {
            double v = 0.0;
            if (params->get_value(pi->plugin, info.id, &v)) p.value = v;
        }
        out.push_back(std::move(p));
    }
    return out;
}

bool PluginChain::setPluginParam(int index, uint32_t paramId, double value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return false;
    auto& pi = m_storage[index];
    if (!pi->plugin) return false;
    auto* params = (const clap_plugin_params_t*)pi->plugin->get_extension(
        pi->plugin, CLAP_EXT_PARAMS);
    if (!params) return false;
    HostInputEvents inEvents;
    inEvents.pushParamValue(paramId, value);
    clap_output_events_t outEvents{};
    outEvents.ctx = nullptr;
    outEvents.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; };
    if (params->flush) {
        params->flush(pi->plugin, &inEvents.list, &outEvents);
    }
    return true;
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

bool PluginChain::setPluginHandle(int index, int handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_storage.size()) return false;
    if (index >= (int)m_contexts.size()) return false;
    // Write the handle to the PluginContext (audio core allocates
    // it; bridge sets the handle). The dedicatedHost->host_data
    // remains the chain pointer (used by clap_get_extension via
    // t_currentChain — see top of file).
    m_contexts[index]->handle = handle;
    return true;
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
