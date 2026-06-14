#pragma once
#include "ClapHost.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>

struct PluginInstance {
    void* library{nullptr};
    const clap_plugin_t* plugin{nullptr};
    std::unique_ptr<clap_host_t> dedicatedHost;
    std::string name;
    std::string path;
    std::string id;
    bool active{true};
};

class PluginChain {
public:
    PluginChain(int sampleRate, int blockSize, int trackIndex = 0);
    ~PluginChain();

    bool addPlugin(const char* libraryPath, const char* pluginId);
    void removePlugin(int index);
    void clear();
    void setBypass(int index, bool bypass);

    void process(const float* input, float* output, int frames, int channels);

    int getPluginCount() const;
    const char* getPluginName(int index) const;
    const char* getPluginPath(int index) const;
    const char* getPluginId(int index) const;
    bool isBypassed(int index) const;
    const clap_plugin_t* getPlugin(int index) const;

    // Process pending flush requests from the main thread (for parameter sync)
    static void processPendingFlushes();

private:
    int m_sampleRate;
    int m_blockSize;
    int m_trackIdx;
    std::vector<PluginInstance> m_plugins;
    std::vector<float> m_scratchBuffer;
    mutable std::mutex m_mutex;
};
