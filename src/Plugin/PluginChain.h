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
    float drive{1.0f};                  // soft clipper drive
};

class PluginChain {
public:
    PluginChain(int sampleRate, int blockSize, int trackIndex = 0);
    ~PluginChain();

    bool addPlugin(const char* libraryPath, const char* pluginId);
    int addBuiltinSoftClipper();
    void removePlugin(int index);
    void clear();
    void setBypass(int index, bool bypass);

    // Real-time safe: NEVER takes a mutex. If a slot is being modified
    // by the main thread, this returns a null snapshot and the audio
    // thread processes silence for that slot (inaudible glitch, not a hang).
    void process(const float* input, float* output, int frames, int channels);

    int getPluginCount() const;
    const char* getPluginName(int index) const;
    const char* getPluginPath(int index) const;
    const char* getPluginId(int index) const;
    bool isBypassed(int index) const;
    const clap_plugin_t* getPlugin(int index) const;

    // Patch the host_data of the plugin at `index` to the supplied handle.
    // Used by the GUI layer to install a globally unique handle that
    // survives any reordering / removal of plugins. The handle is opaque
    // to the plugin and only matters for host callbacks that need to find
    // the GUI window from inside the plugin thread.
    void setPluginHostData(int index, void* hostData);

    bool isBuiltin(int index) const;
    bool getGuiOpen(int index) const;
    void setGuiOpen(int index, bool open);
    float getDrive(int index) const;
    void setDrive(int index, float val);

    // Save/load the plugin's state via the CLAP state extension. The
    // payload is whatever the plugin writes to its ostream (typically
    // a UTF-8 XML blob for JUCE-based CLAPs). saveState returns "" on
    // failure or if the plugin is builtin / has no state extension.
    // loadState returns false on failure. Both are [main-thread] per
    // CLAP spec — do not call from the audio callback.
    std::string saveState(int index) const;
    bool loadState(int index, const std::string& blob);

    // Process pending flush requests from the main thread (for parameter sync)
    static void processPendingFlushes();

    // Set g_mainThreadId to the actual main thread. MUST be called at
    // the very top of main() before any PluginChain is constructed.
    // Without this, plugins that call clap_host.thread_check::is_main_thread
    // during their get_extension() probe will see "wrong thread" errors.
    static void initMainThreadId();

    // Mark the current thread as "inside the audio callback". Called
    // by AudioEngine's audio callback at the top and bottom. Plugins
    // use clap_host.thread-check::is_audio_thread() which reads this
    // flag to verify the host is calling process() on the audio thread.
    static void enterAudioThread();
    static void leaveAudioThread();

    // ── Lock-free snapshot model for the audio thread ──
    //
    // The audio thread reads `m_snapshots` atomically. The main thread
    // builds a NEW snapshot vector, then publishes it by swapping
    // `m_snapshots` (atomic shared_ptr exchange). The audio thread takes
    // a local shared_ptr copy before processing each block, so it is
    // never affected by concurrent edits and NEVER blocks.
    //
    // Why this fixes the timeline-pill bug:
    //   Previously the audio callback did `m_mutex.try_lock()` and returned
    //   silently when the main thread was inside addPlugin/removePlugin/
    //   setBypass/setPluginHostData. Result: a full block of silence, the
    //   playhead advance still ran, but the user perceived "pillado".
    //   Now the audio thread sees a stable snapshot and keeps processing.
    //
    // Why we cache port info:
    //   The CLAP spec says get_extension, audio_ports.count/info are
    //   [main-thread]. PeakEater (and other plugins implementing
    //   clap.thread-check) abort if we call them from the audio thread.
    //   So we query them ONCE on the main thread when start_processing
    //   is called, and store the result here.

    struct PortInfo {
        uint32_t inCount  = 0;
        uint32_t outCount = 0;
        bool     hasInStereo  = false;
        bool     hasOutStereo = false;
        bool     supportsStereo = true; // default assume stereo
    };

    struct Snapshot {
        std::vector<PluginInstance*> instances; // owned pointers, not the objects
        std::vector<bool> active;               // bypass state
        std::vector<PortInfo> ports;            // pre-computed on main thread
        std::vector<bool> processing;           // true once start_processing ran
        std::atomic<uint32_t> gen{0};           // increments on each publish
    };

private:
    void processBuiltin(PluginInstance& pi, const float* const* inPtrs, float* const* outPtrs, int frames, int channels);

    int m_sampleRate;
    int m_blockSize;
    int m_trackIdx;

    std::shared_ptr<Snapshot> m_snapshots;     // immutable from audio POV
    mutable std::mutex m_mutex;                 // ONLY for main-thread mutations
    std::vector<std::unique_ptr<PluginInstance>> m_storage; // owns the PluginInstance objects
    std::vector<float> m_scratchBuffer;
};
