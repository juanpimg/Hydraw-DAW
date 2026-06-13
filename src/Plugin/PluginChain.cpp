#include "PluginChain.h"
#include <cstring>

static const clap_host_t s_host = []() {
    clap_host_t h = {};
    h.clap_version = CLAP_VERSION;
    h.name = "Hydraw DAW";
    h.vendor = "Hydraw DAW";
    h.url = "https://hydraw.local";
    h.version = "1.0.0";
    return h;
}();

const clap_host_t* PluginChain::getMinimalHost() {
    return &s_host;
}

PluginChain::PluginChain(int sampleRate, int blockSize)
    : m_sampleRate(sampleRate)
    , m_blockSize(blockSize)
{
    m_scratchBuffer.resize(blockSize * 2, 0.0f);
}

PluginChain::~PluginChain() {
    clear();
}

bool PluginChain::addPlugin(const char* libraryPath, const char* pluginId) {
    void* lib = ClapHost::loadLibrary(libraryPath);
    if (!lib) return false;

    const clap_plugin_t* plugin = ClapHost::createPlugin(lib, getMinimalHost(), pluginId);
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
    if (index < 0 || index >= (int)m_plugins.size()) return;
    m_plugins[index].active = !bypass;
}

void PluginChain::process(const float* input, float* output, int frames, int channels) {
    if (m_plugins.empty()) return;
    if (frames <= 0 || channels <= 0) return;

    int numChannels = channels;
    int numFrames = frames;

    std::vector<const float*> inPtrs(numChannels);
    std::vector<float*> outPtrs(numChannels);

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
        inBuf.data32 = (float**)inPtrs.data();
        inBuf.data64 = nullptr;
        inBuf.channel_count = (uint32_t)numChannels;
        inBuf.latency = 0;

        clap_audio_buffer_t outBuf;
        outBuf.data32 = (float**)outPtrs.data();
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
    return (int)m_plugins.size();
}

const char* PluginChain::getPluginName(int index) const {
    if (index < 0 || index >= (int)m_plugins.size()) return "";
    return m_plugins[index].name.c_str();
}

bool PluginChain::isBypassed(int index) const {
    if (index < 0 || index >= (int)m_plugins.size()) return true;
    return !m_plugins[index].active;
}
