#include "PluginChain.h"
#include "ClapHost.h"
#include <cstring>
#include <mutex>
#include <cstdio>
#include <vector>

// CLAP extensions
#include "ext/timer-support.h"
#include "ext/log.h"
#include "ext/params.h"
#include "ext/audio-ports.h"

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

static void clap_host_log(const clap_host_t* host, clap_log_severity severity, const char* msg) {}
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

static const void* clap_get_extension(const clap_host_t* host, const char* extension_id) {
    if (std::strcmp(extension_id, CLAP_EXT_GUI) == 0) return &s_hostGui;
    if (std::strcmp(extension_id, CLAP_EXT_TIMER_SUPPORT) == 0) return &s_hostTimer;
    if (std::strcmp(extension_id, CLAP_EXT_LOG) == 0) return &s_hostLog;
    if (std::strcmp(extension_id, CLAP_EXT_PARAMS) == 0) return &s_hostParams;
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
    h.version = "1.0.0";
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
}

PluginChain::~PluginChain() { clear(); }

bool PluginChain::addPlugin(const char* libraryPath, const char* pluginId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    void* lib = ClapHost::loadLibrary(libraryPath);
    if (!lib) return false;

    auto dedicatedHost = std::make_unique<clap_host_t>(s_hostTemplate);
    dedicatedHost->host_data = (void*)(intptr_t)((m_trackIdx << 16) | m_plugins.size());

    const clap_plugin_t* plugin = ClapHost::createPlugin(lib, dedicatedHost.get(), pluginId);
    if (!plugin) {
        ClapHost::unloadLibrary(lib);
        return false;
    }

    bool ok = plugin->init ? plugin->init(plugin) : true;
    if (!ok) {
        ClapHost::destroyPlugin(plugin);
        ClapHost::unloadLibrary(lib);
        return false;
    }

    if (plugin->activate &&
        !plugin->activate(plugin, (double)m_sampleRate, (uint32_t)m_blockSize, (uint32_t)m_blockSize)) {
        plugin->destroy(plugin);
        ClapHost::unloadLibrary(lib);
        return false;
    }

    PluginInstance pi;
    pi.library = lib;
    pi.plugin = plugin;
    pi.dedicatedHost = std::move(dedicatedHost);
    pi.name = pluginId;
    pi.active = true;

    m_plugins.push_back(std::move(pi));
    return true;
}

void PluginChain::removePlugin(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_plugins.size()) return;
    auto& pi = m_plugins[index];
    if (pi.plugin) {
        if (pi.plugin->deactivate) pi.plugin->deactivate(pi.plugin);
        ClapHost::destroyPlugin(pi.plugin);
    }
    if (pi.library) ClapHost::unloadLibrary(pi.library);
    m_plugins.erase(m_plugins.begin() + index);
}

void PluginChain::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pi : m_plugins) {
        if (pi.plugin) {
            if (pi.plugin->deactivate) pi.plugin->deactivate(pi.plugin);
            ClapHost::destroyPlugin(pi.plugin);
        }
        if (pi.library) ClapHost::unloadLibrary(pi.library);
    }
    m_plugins.clear();
}

void PluginChain::setBypass(int index, bool bypass) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_plugins.size()) return;
    m_plugins[index].active = !bypass;
}

void PluginChain::process(const float* input, float* output, int frames, int channels) {
    if (m_plugins.empty()) return;
    if (frames <= 0 || channels <= 0) return;
    if (!m_mutex.try_lock()) return;
    std::lock_guard<std::mutex> lock(m_mutex, std::adopt_lock);

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
    for (auto& pi : m_plugins) {
        if (!pi.active) continue;
        if (!pi.plugin || !pi.plugin->process) continue;

        // Query plugin's input channel count
        int pluginChannels = numChannels;
        auto* audioPorts = (const clap_plugin_audio_ports_t*)
            pi.plugin->get_extension(pi.plugin, CLAP_EXT_AUDIO_PORTS);
        if (audioPorts && audioPorts->count && audioPorts->count(pi.plugin, true) > 0) {
            clap_audio_port_info_t portInfo;
            if (audioPorts->get(pi.plugin, 0, true, &portInfo))
                pluginChannels = (int)portInfo.channel_count;
        }
        int useChannels = pluginChannels < 1 ? 1 : (pluginChannels > 2 ? 2 : pluginChannels);
        bool needsDownmix = (numChannels == 2 && useChannels == 1);

        // Downmix stereo → mono for mono plugins
        std::vector<float> monoWork;
        const float* monoIn[2] = {inPtrs[0], inPtrs[1]};
        float* monoOut[2] = {outPtrs[0], outPtrs[1]};
        if (needsDownmix) {
            monoWork.resize(numFrames);
            for (int f = 0; f < numFrames; ++f)
                monoWork[f] = (inPtrs[0][f] + inPtrs[1][f]) * 0.5f;
            monoIn[0] = monoWork.data();
            monoIn[1] = nullptr;
            monoOut[0] = monoWork.data();
            monoOut[1] = nullptr;
        }

        clap_audio_buffer_t inBuf;
        inBuf.data32 = (float**)monoIn;
        inBuf.data64 = nullptr;
        inBuf.channel_count = (uint32_t)useChannels;
        inBuf.latency = 0;

        clap_audio_buffer_t outBuf;
        outBuf.data32 = (float**)monoOut;
        outBuf.data64 = nullptr;
        outBuf.channel_count = (uint32_t)useChannels;
        outBuf.latency = 0;

        // Provide proper CLAP event structures — CRITICAL for DPF parameter sync
        HostInputEvents inEvents;
        HostOutputEvents outEvents;

        clap_process_t proc;
        memset(&proc, 0, sizeof(proc));
        proc.audio_inputs = &inBuf;
        proc.audio_outputs = &outBuf;
        proc.audio_inputs_count = 1;
        proc.audio_outputs_count = 1;
        proc.frames_count = (uint32_t)numFrames;
        proc.in_events = &inEvents.list;
        proc.out_events = &outEvents.list;

        try {
            // Call flush with proper event structs first (parameter sync for non-DPF plugins)
            const clap_plugin_params_t* pparams = (const clap_plugin_params_t*)
                pi.plugin->get_extension(pi.plugin, CLAP_EXT_PARAMS);
            if (pparams && pparams->flush)
                pparams->flush(pi.plugin, &inEvents.list, &outEvents.list);
            pi.plugin->process(pi.plugin, &proc);
        } catch (...) {
            fprintf(stderr, "[PLUGIN] process exception in %s\n", pi.name.c_str());
        }

        // Upmix mono back to stereo
        if (needsDownmix && numChannels == 2) {
            for (int f = 0; f < numFrames; ++f) {
                outPtrs[0][f] = monoWork[f];
                outPtrs[1][f] = monoWork[f];
            }
        }

        if (first) {
            first = false;
            inPtrs[0] = output;
            if (numChannels > 1) inPtrs[1] = output + numFrames;
        }
    }
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
        intptr_t id = (intptr_t)host->host_data;
        int trackIdx = (int)(id >> 16);
        int pluginIdx = (int)(id & 0xFFFF);

        // Can't easily find the right chain here without a global registry.
        // The flush is also handled in process() on the audio thread.
        // This is a best-effort sync for non-DPF plugins that need flush on main thread.
    }
}

int PluginChain::getPluginCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_plugins.size();
}

const char* PluginChain::getPluginName(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_plugins.size()) return "";
    return m_plugins[index].name.c_str();
}

bool PluginChain::isBypassed(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_plugins.size()) return true;
    return !m_plugins[index].active;
}

const clap_plugin_t* PluginChain::getPlugin(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= (int)m_plugins.size()) return nullptr;
    return m_plugins[index].plugin;
}
