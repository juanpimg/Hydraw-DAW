#pragma once
#include "ClapHost.h"
#include <vector>
#include <string>
#include <memory>

struct PluginInstance {
    void* library{nullptr};
    const clap_plugin_t* plugin{nullptr};
    std::string name;
    bool active{true};
};

class PluginChain {
public:
    PluginChain(int sampleRate, int blockSize);
    ~PluginChain();

    bool addPlugin(const char* libraryPath, const char* pluginId);
    void removePlugin(int index);
    void clear();
    void setBypass(int index, bool bypass);

    void process(const float* input, float* output, int frames, int channels);

    int getPluginCount() const;
    const char* getPluginName(int index) const;
    bool isBypassed(int index) const;

private:
    int m_sampleRate;
    int m_blockSize;
    std::vector<PluginInstance> m_plugins;
    std::vector<float> m_scratchBuffer;

    static const clap_host_t* getMinimalHost();
};
