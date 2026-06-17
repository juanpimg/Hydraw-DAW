#pragma once
#include "ClapHost.h"
#include "Audio/ILogSink.h"
#include "Audio/IHostExtensions.h"
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
    // NOTE: `guiOpen` was here previously; it has been removed.
    // The bridge owns its own "is GUI open" map (it was UI state
    // that didn't belong in the audio core).
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
    // hostExt: optional. If non-null, the chain uses the provider
    // for CLAP host extensions (notably the GUI extension, which is
    // bridge-side). Otherwise the chain returns nullptr for host
    // extensions it does not implement itself (timer, log, params,
    // thread-check are still implemented in the audio core).
    //
    // logSink: optional. If non-null, used for diagnostic logging.
    // The audio thread can call this (must be lock-free or
    // non-blocking).
    PluginChain(int sampleRate, int blockSize, int trackIndex = 0,
                hydraw::IHostExtensions* hostExt = nullptr,
                hydraw::ILogSink* logSink = nullptr);
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
    // Sum of reported latencies of all active plugins in this chain,
    // in samples. Thread-safe: locks the chain mutex briefly. Returns
    // 0 if no plugin reports latency.
    uint32_t getLatency() const;
    // Enumerate the parameters of the plugin at `index`. Returns a
    // vector of {id, name, min, max, default, value}. Empty if the
    // plugin does not expose the CLAP params extension.
    struct ParamInfo {
        uint32_t id;
        std::string name;
        double min{0.0};
        double max{1.0};
        double defaultValue{0.0};
        double value{0.0};
        bool isAutomatable{false};
    };
    std::vector<ParamInfo> getPluginParams(int index) const;
    // Set a parameter value via the CLAP params extension. The plugin
    // emits a value-changed event to its output events. Returns true
    // if the plugin accepted the value.
    bool setPluginParam(int index, uint32_t paramId, double value);
    const char* getPluginName(int index) const;
    const char* getPluginPath(int index) const;
    const char* getPluginId(int index) const;
    bool isBypassed(int index) const;
    const clap_plugin_t* getPlugin(int index) const;

    // Bridge-side: set a stable handle for this plugin instance
    // (used by the bridge to look up its GUI state in its own map).
    // Returns true on success. The handle is stored in the
    // PluginContext (allocated per-instance by addPlugin), which
    // is reachable via `clap_host_t::host_data`. This replaces the
    // previous `setPluginHostData` (which wrote to a raw int slot).
    bool setPluginHandle(int index, int handle);

    bool isBuiltin(int index) const;
    float getDrive(int index) const;
    void setDrive(int index, float val);

    std::string saveState(int index) const;
    bool loadState(int index, const std::string& blob);

    static void processPendingFlushes(const std::vector<PluginChain*>& chains);
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

    // ── Accessors for the file-scope static CLAP host callbacks
    // (clap_host_log, clap_get_extension) in PluginChain.cpp. These
    // need read-only access to the injected m_logSink and m_hostExt
    // without requiring friend declarations or making the fields
    // public.
    void logFromClap(int clapSeverity, const char* msg) noexcept;
    const void* getHostExt(const char* extension_id) const noexcept;

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

    // Per-instance context. Each addPlugin() creates a new context
    // and stores its pointer in `clap_host_t::host_data`. The
    // context holds the bridge's stable handle and the bridge's
    // host-extension provider. Both fields are written by the
    // bridge (via setPluginHandle) — the audio core never reads
    // them. The audio core uses the context pointer only to find
    // the chain's hostExt from inside the static
    // `clap_get_extension` function.
    struct PluginContext {
        int handle{0};
        hydraw::IHostExtensions* hostExt{nullptr};
    };
    std::vector<std::unique_ptr<PluginContext>> m_contexts;

    // Injected dependencies (may be null).
    hydraw::IHostExtensions* m_hostExt{nullptr};
    hydraw::ILogSink* m_logSink{nullptr};

    static std::mutex s_pendingDeleteMutex;
    static std::vector<Snapshot*> s_pendingDeletes;
};
