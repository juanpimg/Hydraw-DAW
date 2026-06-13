#include "PluginChain.h"
#include "ClapHost.h"
#include <cstring>
#include <mutex>

// CLAP extensions
#include "ext/timer-support.h"
#include <mutex>

// Forward declaration of GUI host extension (defined in main.cpp)
extern const clap_host_gui_t s_hostGui;

// Minimal timer support stub (DPF plugins need this)
static bool clap_host_register_timer(const clap_host_t* host, uint32_t period_ms, clap_id* timer_id) {
    if (timer_id) *timer_id = 1; // dummy timer id
    // The plugin calls plugin->on_timer periodically
    return true;
}
static bool clap_host_unregister_timer(const clap_host_t* host, clap_id timer_id) { return true; }
static const clap_host_timer_support_t s_hostTimer = {
    clap_host_register_timer,
    clap_host_unregister_timer
};

// Minimal CLAP host callbacks (plugins call these during init/activate)
static const void* clap_get_extension(const clap_host_t* host, const char* extension_id) {
    if (std::strcmp(extension_id, CLAP_EXT_GUI) == 0)
        return &s_hostGui;
    if (std::strcmp(extension_id, CLAP_EXT_TIMER_SUPPORT) == 0)
        return &s_hostTimer;
    return nullptr; // no other extensions yet
}
static void clap_request_restart(const clap_host_t* host) {}
static void clap_request_process(const clap_host_t* host) {}
static void clap_request_callback(const clap_host_t* host) {}

static const clap_host_t s_host = []() {
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

const clap_host_t* PluginChain::getMinimalHost() {
    return &s_host;
}

// Each plugin gets its own host with a unique host_data that encodes
// (trackIdx << 16 | pluginIdx) for the GUI extension callbacks.
const clap_host_t* PluginChain::makeHost(int trackIdx, int pluginIdx) {
    thread_local static clap_host_t perPluginHost[64];
    thread_local static int nextSlot = 0;
    int slot = nextSlot++ % 64;
    perPluginHost[slot] = s_host;
    perPluginHost[slot].host_data = (void*)(intptr_t)((trackIdx << 16) | pluginIdx);
    return &perPluginHost[slot];
}

PluginChain::PluginChain(int sampleRate, int blockSize, int trackIndex)
    : m_sampleRate(sampleRate)
    , m_blockSize(blockSize)
    , m_trackIdx(trackIndex)
{
    m_scratchBuffer.resize(blockSize * 2, 0.0f);
}

PluginChain::~PluginChain() {
    clear();
}

bool PluginChain::addPlugin(const char* libraryPath, const char* pluginId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    void* lib = ClapHost::loadLibrary(libraryPath);
    if (!lib) return false;

    const clap_plugin_t* plugin = ClapHost::createPlugin(lib, makeHost(m_trackIdx, (int)m_plugins.size()), pluginId);
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

    if (plugin->activate && !plugin->activate(plugin, (double)m_sampleRate, (uint32_t)m_blockSize, (uint32_t)m_blockSize)) {
        plugin->destroy(plugin);
        ClapHost::unloadLibrary(lib);
        return false;
    }

    PluginInstance pi;
    pi.library = lib;
    pi.plugin = plugin;
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
    // Non-blocking try_lock — if UI thread is modifying, skip this block
    if (!m_mutex.try_lock()) return;
    std::lock_guard<std::mutex> lock(m_mutex, std::adopt_lock);

    int numChannels = channels < 2 ? channels : 2; // clamp to stereo
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

        clap_audio_buffer_t inBuf;
        inBuf.data32 = (float**)inPtrs;
        inBuf.data64 = nullptr;
        inBuf.channel_count = (uint32_t)numChannels;
        inBuf.latency = 0;

        clap_audio_buffer_t outBuf;
        outBuf.data32 = (float**)outPtrs;
        outBuf.data64 = nullptr;
        outBuf.channel_count = (uint32_t)numChannels;
        outBuf.latency = 0;

        clap_process_t proc;
        memset(&proc, 0, sizeof(proc));
        proc.audio_inputs = &inBuf;
        proc.audio_outputs = &outBuf;
        proc.audio_inputs_count = 1;
        proc.audio_outputs_count = 1;
        proc.frames_count = (uint32_t)numFrames;

        pi.plugin->process(pi.plugin, &proc);

        if (first) {
            first = false;
            inPtrs[0] = output;
            if (numChannels > 1) inPtrs[1] = output + numFrames;
        }
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
