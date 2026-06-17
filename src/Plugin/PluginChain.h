#pragma once
#include "ClapHost.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

struct PluginInstance {
    void* library{nullptr};
    const clap_plugin_t* plugin{nullptr};
    std::unique_ptr<clap_host_t> dedicatedHost;
    std::string name;
    std::string path;
    std::string id;
    bool active{true};

    // Builtin plugin support
    bool isBuiltin{false};
    bool guiOpen{false};
    float drive{1.0f};
    bool started{false};

    ~PluginInstance() {
        if (isBuiltin) return;
        if (plugin) {
            if (started && plugin->stop_processing) plugin->stop_processing(plugin);
            if (plugin->deactivate) plugin->deactivate(plugin);
            ClapHost::destroyPlugin(plugin);
        }
        if (library && !path.empty())
            ClapHost::unloadLibrary(library, path.c_str());
    }
};

class PluginChain {
public:
    PluginChain(int sampleRate, int blockSize, int trackIndex = 0);
    ~PluginChain();

    bool addPlugin(const char* libraryPath, const char* pluginId);
    int addBuiltinSoftClipper();
    void removePlugin(int index);
    void movePlugin(int from, int to);
    void clear();
    void setBypass(int index, bool bypass);

    void process(const float* input, float* output, int frames, int channels);

    int getPluginCount() const;
    bool hasPlugins() const;
    const char* getPluginName(int index) const;
    const char* getPluginPath(int index) const;
    const char* getPluginId(int index) const;
    bool isBypassed(int index) const;
    const clap_plugin_t* getPlugin(int index) const;

    void setPluginHostData(int index, void* hostData);

    bool isBuiltin(int index) const;
    bool getGuiOpen(int index) const;
    void setGuiOpen(int index, bool open);
    float getDrive(int index) const;
    void setDrive(int index, float val);

    std::string saveState(int index) const;
    bool loadState(int index, const std::string& blob);

    static void processPendingFlushes();
    static void initMainThreadId();
    static void enterAudioThread();
    static void leaveAudioThread();

    // ── Lock‑free snapshot model using raw atomic<Snapshot*> + deferred delete ──
    //
    // m_atomicSnapshot is a raw pointer to an immutable Snapshot. The audio
    // thread reads it with a plain atomic load(acquire). The main thread
    // publishes a new Snapshot* via exchange(acq_rel) and queues the old
    // pointer for deferred deletion. Because the audio thread reads the
    // pointer BEFORE processing and doesn't touch it afterward, the defer
    // guarantees the old Snapshot survives the entire audio callback.
    //
    // This avoids the internal mutex that std::atomic_load/store on
    // shared_ptr uses in libstdc++ (which would deadlock if a plugin
    // callback takes g_flushMutex while the main thread holds it).

    // Must be called at the end of each audio callback (after all
    // PluginChain::process calls) to free snapshots that the main
    // thread has retired.
    static void drainPendingDeletes();

    struct PortInfo {
        uint32_t inCount  = 0;
        uint32_t outCount = 0;
        uint32_t inCh  = 0;
        uint32_t outCh = 0;
        bool     hasInStereo  = false;
        bool     hasOutStereo = false;
        bool     supportsStereo = true;
    };

    struct Snapshot {
        std::vector<std::shared_ptr<PluginInstance>> instances;
        std::vector<bool> active;
        std::vector<PortInfo> ports;
        std::vector<bool> processing;
        std::atomic<uint32_t> gen{0};
    };

private:
    void processBuiltin(PluginInstance& pi, const float* const* inPtrs, float* const* outPtrs, int frames, int channels);

    // Lock‑free publish: exchanges the current snapshot, queues old for delete.
    void publishSnapshot(Snapshot* snap);

    int m_sampleRate;
    int m_blockSize;
    int m_trackIdx;

    std::atomic<Snapshot*> m_atomicSnapshot{nullptr};
    std::atomic<uint32_t> m_gen{0};
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<PluginInstance>> m_storage;
    std::vector<float> m_scratchBuffer;

    static std::mutex s_pendingDeleteMutex;
    static std::vector<Snapshot*> s_pendingDeletes;
};
