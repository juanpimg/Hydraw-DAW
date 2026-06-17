#include "AudioEngine.h"
#include "Plugin/PluginChain.h"
#include "Plugin/ClapHost.h"
#include "Project/ProjectSerializer.h"
#include "Util/Logger.h"
#include <webview/webview.h>
#include <webkit2/webkit2.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <GL/glx.h>
#include <set>
#include <unordered_map>
#include <deque>
#include "clap/ext/timer-support.h"
#include "clap/ext/params.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <sstream>
#include <locale>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <ctime>
#include <fstream>
#include <mutex>
#include <csignal>
#include <execinfo.h>

static AudioEngine* g_engine = nullptr;
static webview::webview* g_wv = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_pageReady{false};
static std::atomic<uint32_t> g_uiDirty{0xFFFFFFFF};
static std::atomic<bool> g_projectLoadedPending{false};
static std::atomic<int> g_exportBusy{0};
// Set by nativeLoadProject for the duration of the load. The UI
// update thread checks this and skips ticks while a load is in
// progress. This prevents the race where the UI thread dispatches
// an updateUIFromNative with stale (or partial) state BEFORE the
// __onProjectLoaded callback has a chance to invalidate the JS
// optimization caches. Without this flag, the rebuild check in
// updateUIFromNative can see an old _lastRenderedTrackCount and
// skip the rebuild, leaving stale waveforms and a glitched
// timeline on the next partial update.
static std::atomic<bool> g_loadInProgress{false};
// Monotonic generation counter, incremented before every full
// telemetry push. Travels in the JSON payload as "gen". The JS
// side uses it as a defense-in-depth check: if __onProjectLoaded
// has reset caches for generation N, but a stale full update
// from generation N-1 arrives later, the JS side ignores it. This
// eliminates the "stale full update arrives after cache reset"
// race that the g_loadInProgress flag alone cannot fully prevent.
static std::atomic<uint64_t> g_telemetryGen{0};
constexpr uint32_t kDirtyExtended = 1u << 0;

// Undo/Redo history. Bounded to kUndoCapacity snapshots.
static std::vector<std::string> g_undoStack;
static std::vector<std::string> g_redoStack;
static constexpr size_t kUndoCapacity = 50;
static std::string g_currentProjectDir;

static void pushUndo() {
    if (!g_engine) return;
    std::string snap = ProjectSerializer::saveToString(g_engine, g_currentProjectDir);
    g_undoStack.push_back(std::move(snap));
    if (g_undoStack.size() > kUndoCapacity) g_undoStack.erase(g_undoStack.begin());
    g_redoStack.clear();
}

// Escape a JSON string fragment for embedding inside a single-quoted
// JS string literal. The JSON already uses double quotes for its
// own string fields (via escapeJson), so we only need to escape
// backslashes and single quotes for the outer JS literal. Without
// this, the eval "JSON.parse('...')" would break on JSON containing
// backslash-escaped double quotes (which escapeJson emits).
static std::string escapeForJsLiteral(const std::string& json) {
    std::string out;
    out.reserve(json.size() + 8);
    for (char c : json) {
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else out += c;
    }
    return out;
}

// Forward declarations for helper functions used by buildJsonTelemetry
// (defined later in this file). Note: no default args here — those
// are on the actual definitions to avoid -fpermissive warnings.
static float safeFloat(float v, float def);
static float safePeak(float v);
static std::string escapeJson(const std::string& s);

// Build a JSON payload for the JS telemetry push. Replaces the
// old array-literal format which was fragile in WebKitGTK's JSC
// eval parser (nested integer arrays with trailing/leading zeros
// were silently truncated to [0,0,0] — see PERSISTENCE_SPEC.md
// §"C++ → JS telemetry: JSON serialization").
//
// Always-sent fields (every tick): full, playing, playhead,
// trackCount, masterPeakL/R, masterVolume, peaks.
//
// Full-only fields (only on kDirtyExtended ticks): volumes,
// mutes, solos, arms, names, audioFrames, clipStarts. These are
// the per-track structural state that drives rebuilds.
static std::string buildJsonTelemetry(
    AudioEngine* engine,
    bool sendFull,
    bool playing,
    uint64_t ph,
    int tc,
    uint64_t gen)
{
    std::ostringstream js;
    js.imbue(std::locale::classic());

    js << "{";
    js << "\"full\":" << (sendFull ? 1 : 0) << ",";
    js << "\"gen\":" << gen << ",";
    js << "\"playing\":" << (playing ? 1 : 0) << ",";
    js << "\"playhead\":" << ph << ",";
    js << "\"trackCount\":" << tc << ",";

    auto* master = engine ? engine->getMaster() : nullptr;
    js << "\"masterPeakL\":" << safePeak(master ? master->peakL.load(std::memory_order_relaxed) : 0.0f) << ",";
    js << "\"masterPeakR\":" << safePeak(master ? master->peakR.load(std::memory_order_relaxed) : 0.0f) << ",";
    js << "\"masterVolume\":" << safeFloat(master ? master->volume.load(std::memory_order_relaxed) : 1.0f, 1.0f) << ",";
    js << "\"bpm\":" << safeFloat(engine ? engine->getBPM() : 120.0f, 120.0f) << ",";
    js << "\"timeSigNum\":" << (engine ? engine->getTimeSigNum() : 4) << ",";
    js << "\"timeSigDen\":" << (engine ? engine->getTimeSigDen() : 4) << ",";

    // Peaks (always sent)
    js << "\"peaks\":[";
    for (int i = 0; i < tc; ++i) {
        if (i > 0) js << ",";
        Track* trk = engine ? engine->getTrack(i) : nullptr;
        js << safePeak(std::max(trk ? trk->peakL.load(std::memory_order_relaxed) : 0.0f,
                                 trk ? trk->peakR.load(std::memory_order_relaxed) : 0.0f));
    }
    js << "]";

    if (sendFull) {
        // Volumes
        js << ",\"volumes\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << safeFloat(engine->getTrack(i)->volume.load(std::memory_order_relaxed), 1.0f);
        }
        js << "]";
        // Mutes
        js << ",\"mutes\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (engine->getTrack(i)->muted.load(std::memory_order_relaxed) ? 1 : 0);
        }
        js << "]";
        // Solos
        js << ",\"solos\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (engine->getTrack(i)->soloed.load(std::memory_order_relaxed) ? 1 : 0);
        }
        js << "]";
        // Arms
        js << ",\"arms\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (engine->getTrack(i)->armed.load(std::memory_order_relaxed) ? 1 : 0);
        }
        js << "]";
        // Names
        js << ",\"names\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << "\"" << escapeJson(engine->getTrack(i)->name) << "\"";
        }
        js << "]";
        // Audio frames (primary clip frames per track; 0 if no primary).
        // Kept for backwards compat with the JS STATE.audioFrames array.
        js << ",\"audioFrames\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            // CRITICAL: use memory_order_acquire here. The audio
            // thread (in process()) uses acquire too, and
            // loadWavToTrack uses release when storing the new
            // buffer. The UI thread reads non-atomic fields
            // (frames) of the buffer, so it MUST use acquire to
            // synchronize with the writer.
            AudioBuffer* buf = engine->getTrack(i)->audio.load(std::memory_order_acquire);
            js << (buf ? (int64_t)buf->frames : 0);
        }
        js << "]";
        // Clip starts (primary clipStart per track).
        js << ",\"clipStarts\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (int64_t)engine->getTrack(i)->clipStart.load(std::memory_order_relaxed);
        }
        js << "]";
        // Multi-clip data: for each track, an array of [start, frames]
        // pairs for ALL clips (primary + extras). The UI uses this to
        // render every clip independently. Format:
        //   "clipBlocks": [
        //     [[start0, frames0], [start1, frames1], ...],  // track 0
        //     [],                                            // track 1
        //     ...
        //   ]
        js << ",\"clipBlocks\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            int n = engine->getClipCount(i);
            js << "[";
            for (int c = 0; c < n; ++c) {
                if (c > 0) js << ",";
                uint64_t s = engine->getClipStart(i, c);
                uint64_t f = engine->getClipFrames(i, c);
                uint64_t fi = engine->getClipFadeIn(i, c);
                uint64_t fo = engine->getClipFadeOut(i, c);
                js << "[" << (int64_t)s << "," << (int64_t)f << "," << (int64_t)fi << "," << (int64_t)fo << "]";
            }
            js << "]";
        }
        js << "]";
        // Clip names (parallel to clipBlocks).
        js << ",\"clipNames\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            int n = engine->getClipCount(i);
            js << "[";
            for (int c = 0; c < n; ++c) {
                if (c > 0) js << ",";
                std::string p = engine->getClipPath(i, c);
                // Use just the basename
                size_t slash = p.find_last_of("/\\");
                std::string name = (slash != std::string::npos) ? p.substr(slash + 1) : p;
                if (name.empty()) {
                    // Fall back to track name
                    name = engine->getTrack(i)->name;
                }
                js << "\"" << escapeJson(name) << "\"";
            }
            js << "]";
        }
        js << "]";
        // Per-clip peak caches (parallel to clipBlocks). Used by the
        // UI to draw the waveform of every clip independently.
        // ALL clips (primary AND extras) get their cache serialized
        // synchronously from the same source — no async fetch needed.
        // This guarantees identical density for every clip piece.
        js << ",\"clipPeakCaches\":[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            int n = engine->getClipCount(i);
            js << "[";
            for (int c = 0; c < n; ++c) {
                if (c > 0) js << ",";
                js << "[";
                auto pc = engine->getClipPeakCache(i, c);
                for (size_t p = 0; p < pc.size(); ++p) {
                    if (p > 0) js << ",";
                    js << safePeak(pc[p]);
                }
                js << "]";
            }
            js << "]";
        }
        js << "]";
    }
    js << "}";
    return js.str();
}

// ── PLUGIN GUI ── (must be before uiUpdateLoop)
struct PluginGuiState {
    int trackIdx;
    int pluginIdx;
    GtkWidget* window;
    Window x11win;
    Display* x11dpy;
    const clap_plugin_t* plugin;
    const clap_plugin_gui_t* gui;
    // Index of self in g_pluginGuis (stable handle — see g_pluginGuisByHandle)
    int handle;
};
// g_pluginGuisByHandle maps a stable handle to an index into g_pluginGuis.
// We use std::deque so push_back/push_front/erase at any iterator does NOT
// invalidate references to other elements, and we keep a parallel stable
// handle → deque index map for the destroy signal handler so it can locate
// its own state safely even if the deque reallocates.
static std::deque<PluginGuiState> g_pluginGuis;
static std::unordered_map<int, int> g_pluginGuisByHandle; // handle -> deque index
static std::mutex g_pluginGuisMutex;
// Per-(track,plugin) "is GUI open" state for builtin plugins
// (soft clipper). The audio core used to store this in
// PluginInstance::guiOpen, but that field was removed in the
// bridge refactor because it was UI state leaking into the core.
static std::unordered_map<int, bool> g_builtinGuiOpen; // key = (track << 16) | plugin
static std::mutex g_builtinGuiOpenMutex;
static inline int builtinKey(int t, int p) { return (t << 16) | (p & 0xFFFF); }
static int g_nextPluginGuiHandle = 1;

static std::ofstream g_logFile;
static std::mutex g_logMutex;
// Separate crash log written from signal handlers via fprintf (async-
// signal-safe). User reads this file to find backtraces after a crash.
static FILE* g_crashLogFile = nullptr;

static void initLogger() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
    std::string logName = std::string("hydraw_") + buf + ".log";
    g_logFile.open(logName, std::ios::out | std::ios::trunc);
    if (g_logFile.is_open()) {
        g_logFile << "=== Hydraw DAW log started at " << buf << " ===" << std::endl;
        fprintf(stderr, "[SYS] Logging to %s\n", logName.c_str());
    } else {
        fprintf(stderr, "[SYS] WARNING: Could not open log file %s\n", logName.c_str());
    }
    // Separate crash log for signal-handler backtraces. Use C FILE* so we
    // can write from a signal handler with fprintf (async-signal-safe).
    std::string crashName = std::string("hydraw_crash_") + std::to_string(getpid()) + ".log";
    g_crashLogFile = fopen(crashName.c_str(), "w");
    if (g_crashLogFile) {
        fprintf(g_crashLogFile, "=== Hydraw DAW crash log (pid=%d) ===\n", (int)getpid());
        fflush(g_crashLogFile);
        fprintf(stderr, "[SYS] Crash log at %s\n", crashName.c_str());
    }
}

static void closeLogger() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile << "=== Hydraw DAW log ended ===" << std::endl;
        g_logFile.close();
    }
    if (g_crashLogFile) {
        fprintf(g_crashLogFile, "=== Clean exit ===\n");
        fclose(g_crashLogFile);
        g_crashLogFile = nullptr;
    }
}

static void log(const char* level, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    std::ostringstream line;
    line << "[" << buf << "." << std::to_string(ms) << "] [" << level << "] " << msg;
    // Always write to stderr
    fprintf(stderr, "%s\n", line.str().c_str());
    // Also write to log file
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile << line.str() << std::endl;
        g_logFile.flush();
    }
}

// dlog = "debug log". Writes ONLY to the log file (not stderr) so the
// user does not have to scroll/copy-paste a console. Used for verbose
// per-step traces in the plugin GUI open path, audio thread, etc.
// NOTE: the audio core no longer calls dlog() — it calls
// m_logSink->log() via the hydraw::ILogSink interface. The dlog()
// function below is still defined for the bridge's own use (it
// also serves as the FileLogSink's writeback helper in some
// code paths; see Util/Logger.h).
void dlog(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    std::ostringstream line;
    line << "[" << buf << "." << std::to_string(ms) << "] [DBUG] " << msg;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile << line.str() << std::endl;
        g_logFile.flush();
    }
}

// Returns the current thread as "0x<hex>". Used in bridge dlog
// messages to identify which thread is doing the work. The audio
// core used to have its own copy via `extern std::string threadId()`
// but the ILogSink interface provides a thread-aware log()
// implementation instead, so this is bridge-only.
std::string threadId() {
    std::ostringstream o;
    o << "0x" << std::hex << (unsigned long)pthread_self();
    return o.str();
}

static int safeStoi(const std::string& s, int def = 0) {
    try {
        size_t pos = 0;
        int r = std::stoi(s, &pos);
        if (pos != s.size()) return def;
        return r;
    } catch (...) { return def; }
}

static float safeStof(const std::string& s, float def = 0.0f) {
    try {
        size_t pos = 0;
        float r = std::stof(s, &pos);
        if (pos != s.size()) {
            // stof may stop early if locale uses comma as decimal separator.
            // Retry in classic ("C") locale via stringstream.
            std::istringstream iss(s);
            iss.imbue(std::locale::classic());
            float r2;
            iss >> r2;
            if (!iss.fail() && iss.eof()) return r2;
            return def;
        }
        return r;
    } catch (...) {
        // Fallback: retry in classic locale
        try {
            std::istringstream iss(s);
            iss.imbue(std::locale::classic());
            float r;
            iss >> r;
            if (!iss.fail() && iss.eof()) return r;
        } catch (...) {}
        return def;
    }
}

static float safeFloat(float v, float def = 0.0f) {
    return (std::isnan(v) || std::isinf(v)) ? def : v;
}

static float safePeak(float v) {
    return (std::isnan(v) || std::isinf(v)) ? 0.0f : std::max(0.0f, std::min(1.0f, v));
}

static std::string escapeJson(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: o << c;
        }
    }
    return o.str();
}

// webview v0.12 wraps single arguments as JSON arrays: ["actual_value"]
// This helper unwraps the JSON array and returns the raw inner value.
static std::string unwrapArg(const std::string& req) {
    if (req.size() >= 2 && req.front() == '[' && req.back() == ']') {
        std::string inner = req.substr(1, req.size() - 2);
        if (inner.size() >= 2 && inner.front() == '"' && inner.back() == '"') {
            return inner.substr(1, inner.size() - 2);
        }
        return inner;
    }
    return req;
}

static void uiUpdateLoop() {
    using namespace std::chrono_literals;
    int s_lastTrackCount = -1;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(30ms);
        if (!g_engine || !g_wv || !g_pageReady.load(std::memory_order_acquire)) {
            continue;
        }
        // Skip ticks while a project load is in progress. The load
        // thread is rebuilding C++ state (tracks, plugins, audio
        // buffers) and will dispatch __onProjectLoaded + set
        // g_uiDirty when done. Sending telemetry mid-load would
        // either read partial state or, worse, race with the load
        // and leave the JS side with stale data + reset caches.
        if (g_loadInProgress.load(std::memory_order_acquire)) {
            continue;
        }

        uint64_t ph = g_engine->getPlayhead();
        int tc = g_engine->getTrackCount();
        bool playing = g_engine->isPlaying();

        // Detect track count changes → force extended data
        if (tc != s_lastTrackCount) {
            s_lastTrackCount = tc;
            g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
        }

        auto* master = g_engine->getMaster();
        uint32_t dirty = g_uiDirty.load(std::memory_order_acquire);
        bool sendFull = (dirty & kDirtyExtended) != 0;
        if (sendFull)
            g_uiDirty.fetch_and(~kDirtyExtended, std::memory_order_release);

        // Bump the generation counter on every full push. The JS
        // side stores the last seen gen and ignores stale full
        // updates that belong to a generation whose caches have
        // already been reset (e.g. by a subsequent __onProjectLoaded).
        uint64_t gen = sendFull ? g_telemetryGen.fetch_add(1, std::memory_order_acq_rel) + 1
                                : g_telemetryGen.load(std::memory_order_acquire);

        // Build a JSON payload (not an array literal — JSC's array
        // literal parser has bugs with nested integer arrays that
        // drop elements to [0,0,0]). See PERSISTENCE_SPEC.md.
        std::string jsonBody = buildJsonTelemetry(g_engine, sendFull, playing, ph, tc, gen);
        std::string jsBody = "updateUIFromNative(JSON.parse('" + escapeForJsLiteral(jsonBody) + "'))";

        // DIAGNOSTIC: log the full payload once on the first full send
        // so we can verify the JSON is well-formed and audioFrames
        // is present in the object.
        if (sendFull) {
            static int s_diagCount = 0;
            if (s_diagCount < 3) {
                ++s_diagCount;
                log("DBUG", "uiUpdateLoop full payload (tc=" + std::to_string(tc) +
                     ", gen=" + std::to_string(gen) +
                     ", len=" + std::to_string(jsBody.size()) + "): " + jsBody);
            }
        }

        // Direct eval from the UI thread. The webview library's eval_impl
        // calls webkit_web_view_evaluate_javascript which IS thread-safe
        // in WebKitGTK 2.x (it marshals to the main thread internally).
        // We must guard against g_wv being null AND against the webview
        // main loop being dead (after w.run() returns). If the eval
        // would block, we just skip this tick — the UI thread will exit
        // at the next g_running check.
        if (g_wv) {
            // Wrap the eval so we never block the UI thread. The eval
            // itself is async in WebKit 2.40+, but the underlying GDK
            // send_event can deadlock if the main loop has exited.
            g_wv->dispatch([jsStr = std::move(jsBody)]() {
                if (g_wv) {
                    g_wv->eval(jsStr);
                }
            });
        }

        // Fire plugin timers. CRITICAL: plugin->get_extension() and
        // timer->on_timer() MUST be called on the main thread (per CLAP
        // spec [main-thread]). PeakEater (and other plugins implementing
        // clap.thread-check) abort if we call from the wrong thread.
        // We dispatch the work via g_main_context_invoke, which runs
        // on the main thread (the webview's main loop). The UI thread
        // does NOT block on it.
        g_main_context_invoke(nullptr, (GSourceFunc)(+[](void* ud) -> gboolean {
            std::vector<const clap_plugin_t*>* plugins = nullptr;
            std::vector<const clap_plugin_t*> localPlugins;
            {
                std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
                localPlugins.reserve(g_pluginGuis.size());
                for (const auto& gs : g_pluginGuis) {
                    if (gs.plugin) localPlugins.push_back(gs.plugin);
                }
                plugins = &localPlugins;
            }
            for (const auto* plugin : *plugins) {
                auto* timer = (const clap_plugin_timer_support_t*)
                    plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT);
                if (timer && timer->on_timer)
                    timer->on_timer(plugin, 1);
            }
            return G_SOURCE_REMOVE;
        }), nullptr);

        // Process pending flush requests from plugins (parameter sync)
        if (g_engine) {
            std::vector<PluginChain*> chains;
            int tc = g_engine->getTrackCount();
            for (int i = 0; i < tc; ++i) {
                PluginChain* c = g_engine->getPluginChain(i);
                if (c) chains.push_back(c);
            }
            PluginChain* m = g_engine->getPluginChain(-1);
            if (m) chains.push_back(m);
            PluginChain::processPendingFlushes(chains);
        }
    }
}

static std::string nativeLog(const std::string& req) {
    std::string a = unwrapArg(req);
    // JS sends: "LEVEL,label,msg" or batched "ITEM1|ITEM2|..." (pipe-delimited)
    size_t start = 0, end;
    while ((end = a.find('|', start)) != std::string::npos) {
        std::string item = a.substr(start, end - start);
        start = end + 1;
        auto p1 = item.find(',');
        if (p1 == std::string::npos) continue;
        std::string level = item.substr(0, p1);
        auto p2 = item.find(',', p1 + 1);
        std::string label, msg;
        if (p2 == std::string::npos) {
            label = "JS";
            msg = item.substr(p1 + 1);
        } else {
            label = item.substr(p1 + 1, p2 - p1 - 1);
            msg = item.substr(p2 + 1);
        }
        log(level.c_str(), "[" + label + "] " + msg);
    }
    // Last item (or single message)
    std::string item = a.substr(start);
    if (!item.empty()) {
        auto p1 = item.find(',');
        if (p1 == std::string::npos) {
            log("JS", item);
        } else {
            std::string level = item.substr(0, p1);
            auto p2 = item.find(',', p1 + 1);
            std::string label, msg;
            if (p2 == std::string::npos) {
                label = "JS";
                msg = item.substr(p1 + 1);
            } else {
                label = item.substr(p1 + 1, p2 - p1 - 1);
                msg = item.substr(p2 + 1);
            }
            log(level.c_str(), "[" + label + "] " + msg);
        }
    }
    return "null";
}

// ── NATIVE FILE DIALOGS (thread‑safe via main‑loop dispatch) ──
struct DialogData {
    std::string title;
    std::string defaultName;
    std::string extension;
    bool isSave;
    std::string result;
    bool done = false;
    std::mutex mtx;
    std::condition_variable cv;
};

static gboolean runDialogOnMainThread(gpointer userData) {
    auto* d = static_cast<DialogData*>(userData);
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkFileChooserAction action = d->isSave ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN;
    const char* acceptLabel = d->isSave ? "_Save" : "_Open";
    GtkFileChooserNative* dialog = gtk_file_chooser_native_new(
        d->title.c_str(), GTK_WINDOW(win), action, acceptLabel, "_Cancel");
    if (d->isSave && !d->defaultName.empty())
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), d->defaultName.c_str());
    if (!d->extension.empty()) {
        GtkFileFilter* filter = gtk_file_filter_new();
        std::string pat = "*." + d->extension;
        gtk_file_filter_add_pattern(filter, pat.c_str());
        gtk_file_filter_set_name(filter, (d->extension + " files").c_str());
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        if (!d->isSave) gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter);
    }
    int res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) {
            d->result = path;
            g_free(path);
        }
    }
    g_object_unref(dialog);
    gtk_widget_destroy(win);
    std::lock_guard<std::mutex> lock(d->mtx);
    d->done = true;
    d->cv.notify_one();
    return G_SOURCE_REMOVE;
}

static std::string nativeShowSaveDialog(const std::string& req) {
    DialogData d;
    d.isSave = true;
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    d.title = (p1 == std::string::npos) ? a : a.substr(0, p1);
    if (p1 != std::string::npos) {
        std::string rest = a.substr(p1 + 1);
        auto p2 = rest.find(',');
        d.defaultName = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
        if (p2 != std::string::npos) d.extension = rest.substr(p2 + 1);
    }
    // Insert extension hint into defaultName so the filter applies
    if (!d.extension.empty() && d.defaultName.empty())
        d.defaultName = "untitled." + d.extension;
    g_main_context_invoke(nullptr, runDialogOnMainThread, &d);
    {
        std::unique_lock<std::mutex> lock(d.mtx);
        d.cv.wait(lock, [&d]{ return d.done; });
    }
    return "\"" + escapeJson(d.result) + "\"";
}

static std::string nativeShowOpenDialog(const std::string& req) {
    DialogData d;
    d.isSave = false;
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    d.title = (p1 == std::string::npos) ? a : a.substr(0, p1);
    if (p1 != std::string::npos) d.extension = a.substr(p1 + 1);
    g_main_context_invoke(nullptr, runDialogOnMainThread, &d);
    {
        std::unique_lock<std::mutex> lock(d.mtx);
        d.cv.wait(lock, [&d]{ return d.done; });
    }
    return "\"" + escapeJson(d.result) + "\"";
}

// ── SOFT CLIPPER (builtin plugin) ──
// Format: "trackIdx,pluginIdx,value"
static std::string nativeSetSoftClipperDrive(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "null";
    int trackIdx = safeStoi(a.substr(0, p1));
    int pluginIdx = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    float drive = safeStof(a.substr(p2 + 1), 1.0f);
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (chain) {
        chain->setDrive(pluginIdx, drive);
        log("AUDIO", "softClipperDrive track=" + std::to_string(trackIdx) +
            " plugin=" + std::to_string(pluginIdx) + " val=" + std::to_string(drive));
    }
    return "null";
}

// ── PROJECT / EXPORT ──
static std::string nativeSaveProject(const std::string& req) {
    std::string path = unwrapArg(req);
    bool ok = g_engine ? ProjectSerializer::save(path.c_str(), g_engine) : false;
    if (ok) {
        // Track project dir for undo/redo
        std::filesystem::path p(path);
        g_currentProjectDir = p.parent_path().string();
    }
    log("SYS", "saveProject -> " + path + " " + (ok ? "OK" : "FAIL"));
    return ok ? "\"OK\"" : "\"FAIL\"";
}

static std::string nativeLoadProject(const std::string& req) {
    std::string path = unwrapArg(req);
    if (!g_engine) return "\"FAIL\"";

    // Strict Sequential Hydration (see PERSISTENCE_SPEC.md):
    //   1. Set g_loadInProgress — UI update thread will skip ticks.
    //   2. Stop audio device — no more audio callbacks. Required for
    //      thread safety: removeTrack swaps m_pluginChains[] which
    //      the audio thread dereferences, and CLAP's stop_processing
    //      is [audio-thread] and must not be called from main.
    //   3. Drain pending deletes from previous sessions.
    //   4. Load the project — rebuild C++ state.
    //   5. Restart audio device.
    //   6. Dispatch __onProjectLoaded (resets JS caches).
    //   7. Set g_uiDirty so the next UI tick sends a full update
    //      (which will see the reset caches and trigger a rebuild).
    //   8. Clear g_loadInProgress LAST so the UI thread sees both
    //      g_loadInProgress=false AND g_uiDirty=1 on its next tick.

    g_loadInProgress.store(true, std::memory_order_release);

    log("SYS", "loadProject: stopping audio engine...");
    g_engine->stop();
    PluginChain::drainPendingDeletes();
    g_engine->drainPendingAudioBufferDeletes();
    // Track project dir for undo/redo
    g_currentProjectDir = std::filesystem::path(path).parent_path().string();
    // Clear history — a project load is a new starting point.
    g_undoStack.clear();
    g_redoStack.clear();

    log("SYS", "loadProject: loading " + path);
    bool ok = ProjectSerializer::load(path.c_str(), g_engine);

    g_engine->start();

    if (ok) {
        log("SYS", "loadProject: OK, dispatching __onProjectLoaded");
        // Bump the generation counter BEFORE dispatching
        // __onProjectLoaded. The JS side stores this gen as the
        // "current" generation. Any subsequent full update from
        // C++ will carry gen+1. If a stale full update from
        // gen-1 (queued before the load) is still in flight, the
        // JS side will see its gen < current and ignore it. This
        // is the defense-in-depth against the rare race where
        // g_loadInProgress is cleared before the main loop has
        // processed the queued __onProjectLoaded.
        uint64_t loadGen = g_telemetryGen.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (g_wv) {
            g_wv->dispatch([loadGen]() {
                if (g_wv) {
                    std::string js = "window.__onProjectLoaded && window.__onProjectLoaded(" +
                                     std::to_string(loadGen) + ")";
                    g_wv->eval(js);
                }
            });
        }
        g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    } else {
        log("SYS", "loadProject: FAILED, restarting with current state");
    }

    // Order matters: clear the flag AFTER dispatching the eval
    // and setting g_uiDirty, so the UI thread's next tick sees
    // both g_loadInProgress=false and g_uiDirty=1.
    g_loadInProgress.store(false, std::memory_order_release);

    return ok ? "\"OK\"" : "\"FAIL\"";
}

static std::string nativeExportAudio(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "\"FAIL\"";
    std::string wavPath = a.substr(0, p1);
    auto p2 = a.find(',', p1 + 1);
    int sampleRate = 48000;
    int bitDepth = 16;
    if (p2 != std::string::npos) {
        sampleRate = safeStoi(a.substr(p1 + 1, p2 - p1 - 1), 48000);
        bitDepth = safeStoi(a.substr(p2 + 1), 16);
    } else {
        sampleRate = safeStoi(a.substr(p1 + 1), 48000);
    }
    log("EXPORT", "start async path=" + wavPath + " sr=" + std::to_string(sampleRate) + " bd=" + std::to_string(bitDepth));

    g_exportBusy.fetch_add(1, std::memory_order_release);
    std::thread t([wavPath, sampleRate, bitDepth]() {
        bool ok = g_engine ? g_engine->renderOffline(wavPath.c_str(), sampleRate, bitDepth) : false;
        log("EXPORT", std::string("done ") + (ok ? "OK" : "FAIL"));
        if (g_running.load(std::memory_order_acquire) && g_wv) {
            // eval() is NOT thread-safe — dispatch to the main thread.
            std::string js = std::string("window.onExportDone && window.onExportDone(") +
                             (ok ? "'OK'" : "'FAIL'") + ")";
            g_wv->dispatch([js]() { if (g_wv) g_wv->eval(js); });
        }
        g_exportBusy.fetch_sub(1, std::memory_order_release);
    });
    t.detach();
    return "\"PENDING\"";
}

// Transport bindings. The JS UI's play/pause/stop buttons call
// these via cn('nativePlay'|'nativePause'|'nativeStop', '').
static std::string nativePlay(const std::string&) {
    log("AUDIO", "play");
    if (g_engine) g_engine->play();
    return "null";
}

static std::string nativePause(const std::string&) {
    log("AUDIO", "pause");
    if (g_engine) g_engine->pause();
    return "null";
}

static std::string nativeStop(const std::string&) {
    log("AUDIO", "stop");
    if (g_engine) g_engine->stopTransport();
    return "null";
}

static std::string nativeSetPlayhead(const std::string& req) {
    std::string a = unwrapArg(req);
    uint64_t pos = (uint64_t)std::max(0, safeStoi(a));
    log("AUDIO", "setPlayhead " + std::to_string(pos));
    if (g_engine) g_engine->setPlayhead(pos);
    return "null";
}

static std::string nativeSetBPM(const std::string& req) {
    std::string a = unwrapArg(req);
    float bpm = safeStof(a);
    if (bpm < 20.0f) bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    if (g_engine) g_engine->setBPM(bpm);
    return std::to_string(bpm);
}

static std::string nativeSetTimeSignature(const std::string& req) {
    std::string a = unwrapArg(req);
    int comma = (int)a.find(',');
    int num = 4, den = 4;
    if (comma >= 0) {
        num = safeStoi(a.substr(0, comma));
        den = safeStoi(a.substr(comma + 1));
    } else {
        num = safeStoi(a);
    }
    if (g_engine) g_engine->setTimeSignature(num, den);
    return std::to_string(num) + "/" + std::to_string(den);
}

static std::string nativeSetLoopEnabled(const std::string& req) {
    std::string a = unwrapArg(req);
    bool en = a == "1" || a == "true";
    if (g_engine) g_engine->setLoopEnabled(en);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetLoopRange(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "null";
    uint64_t s = (uint64_t)std::max(0, safeStoi(a.substr(0, p)));
    uint64_t e = (uint64_t)std::max(0, safeStoi(a.substr(p + 1)));
    g_engine->setLoopRange(s, e);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetPunchRange(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "null";
    uint64_t s = (uint64_t)std::max(0, safeStoi(a.substr(0, p)));
    uint64_t e = (uint64_t)std::max(0, safeStoi(a.substr(p + 1)));
    g_engine->setPunchRange(s, e);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeStartRecording(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "false";
    int track = safeStoi(a.substr(0, p));
    std::string path = a.substr(p + 1);
    g_engine->startRecording(track, path.c_str());
    log("REC", "startRecording track=" + std::to_string(track) + " path=" + path);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "true";
}

static std::string nativeStopRecording(const std::string&) {
    if (g_engine) {
        g_engine->stopRecording();
        log("REC", "stopRecording");
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "true";
}

static std::string nativeFreezeTrack(const std::string& req) {
    int track = safeStoi(unwrapArg(req));
    if (g_engine) {
        g_engine->freezeTrack(track);
        log("FREEZE", "track=" + std::to_string(track));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeUnfreezeTrack(const std::string& req) {
    int track = safeStoi(unwrapArg(req));
    if (g_engine) {
        g_engine->unfreezeTrack(track);
        log("FREEZE", "unfreeze track=" + std::to_string(track));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeGetPluginParams(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "[]";
    int track = safeStoi(a.substr(0, p));
    int plugin = safeStoi(a.substr(p + 1));
    PluginChain* chain = g_engine->getPluginChain(track);
    if (!chain) return "[]";
    auto params = chain->getPluginParams(plugin);
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) o << ",";
        o << "{\"id\":" << params[i].id
          << ",\"name\":\"" << params[i].name << "\""
          << ",\"min\":" << params[i].min
          << ",\"max\":" << params[i].max
          << ",\"default\":" << params[i].defaultValue
          << ",\"value\":" << params[i].value
          << ",\"automatable\":" << (params[i].isAutomatable ? "true" : "false")
          << "}";
    }
    o << "]";
    return o.str();
}

static std::string nativeSetPluginParam(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "false";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "false";
    auto p3 = a.find(',', p2 + 1);
    if (p3 == std::string::npos) return "false";
    int track = safeStoi(a.substr(0, p1));
    int plugin = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    uint32_t paramId = (uint32_t)safeStoi(a.substr(p2 + 1, p3 - p2 - 1));
    double val = safeStof(a.substr(p3 + 1));
    PluginChain* chain = g_engine->getPluginChain(track);
    if (!chain) return "false";
    bool ok = chain->setPluginParam(plugin, paramId, val);
    return ok ? "true" : "false";
}

static std::string nativeAddTrack(const std::string&) {
    if (g_engine) {
        int n = g_engine->addTrack();
        log("TRACK", "addTrack -> " + std::to_string(n));
        g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
        return std::to_string(n);
    }
    log("TRACK", "addTrack failed: no engine");
    return "-1";
}

static std::string nativeRemoveTrack(const std::string& req) {
    std::string a = unwrapArg(req);
    int idx = safeStoi(a, -1);
    log("TRACK", "removeTrack idx=" + std::to_string(idx));
    if (idx >= 0 && g_engine) g_engine->removeTrack(idx);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetVolume(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(a.substr(0, pos));
    float val = safeStof(a.substr(pos + 1), 1.0f);
    Track* t = g_engine->getTrack(idx);
    if (t) {
        t->volume.store(val, std::memory_order_relaxed);
        log("MIX", "setVolume track=" + std::to_string(idx) + " val=" + std::to_string(val));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetMasterVolume(const std::string& req) {
    float val = safeStof(unwrapArg(req), 1.0f);
    if (g_engine) {
        MasterBus* m = g_engine->getMaster();
        if (m) {
            m->volume.store(val, std::memory_order_relaxed);
            log("MIX", "setMasterVolume val=" + std::to_string(val));
        }
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetSendLevel(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "null";
    int track = safeStoi(a.substr(0, p1));
    int bus = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    float val = safeStof(a.substr(p2 + 1), 0.0f);
    g_engine->setSendLevel(track, bus, val);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetAuxVolume(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "null";
    int bus = safeStoi(a.substr(0, p));
    float val = safeStof(a.substr(p + 1), 1.0f);
    AuxBus* ab = g_engine->getAuxBus(bus);
    if (ab) ab->volume.store(val, std::memory_order_relaxed);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetClipStart(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    auto pos2 = a.find(',', pos + 1);
    if (pos2 == std::string::npos) {
        // Legacy: "track,pos" — primary only
        int idx = safeStoi(a.substr(0, pos));
        uint64_t clipStart = (uint64_t)std::max(0, safeStoi(a.substr(pos + 1)));
        g_engine->setClipStart(idx, clipStart);
        log("TIMELINE", "setClipStart track=" + std::to_string(idx) + " pos=" + std::to_string(clipStart));
    } else {
        // Multi: "track,clipIdx,pos"
        int idx = safeStoi(a.substr(0, pos));
        int clipIdx = safeStoi(a.substr(pos + 1, pos2 - pos - 1));
        uint64_t clipStart = (uint64_t)std::max(0, safeStoi(a.substr(pos2 + 1)));
        g_engine->setClipStart(idx, clipIdx, clipStart);
        log("TIMELINE", "setClipStart track=" + std::to_string(idx) + " clip=" + std::to_string(clipIdx) + " pos=" + std::to_string(clipStart));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeMoveClip(const std::string& req) {
    std::string a = unwrapArg(req);
    // Legacy: "src,dst,pos" — single-clip cross-track
    // Multi: "srcTrack,srcClip,dstTrack,dstIndex,pos"
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "null";
    auto p3 = a.find(',', p2 + 1);
    if (p3 == std::string::npos) {
        int src = safeStoi(a.substr(0, p1));
        int dst = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
        uint64_t clipStart = (uint64_t)std::max(0, safeStoi(a.substr(p2 + 1)));
        if (src == dst) {
            g_engine->setClipStart(src, clipStart);
        } else {
            g_engine->moveTrackAudio(src, dst);
            g_engine->setClipStart(dst, clipStart);
        }
        log("TIMELINE", "moveClip legacy src=" + std::to_string(src) + " dst=" + std::to_string(dst) + " pos=" + std::to_string(clipStart));
    } else {
        auto p4 = a.find(',', p3 + 1);
        if (p4 == std::string::npos) return "null";
        int srcTrack = safeStoi(a.substr(0, p1));
        int srcClip = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
        int dstTrack = safeStoi(a.substr(p2 + 1, p3 - p2 - 1));
        int dstIndex = safeStoi(a.substr(p3 + 1, p4 - p3 - 1));
        uint64_t clipStart = (uint64_t)std::max(0, safeStoi(a.substr(p4 + 1)));
        if (srcTrack == dstTrack && srcClip == dstIndex) {
            g_engine->setClipStart(srcTrack, srcClip, clipStart);
        } else {
            g_engine->moveClip(srcTrack, srcClip, dstTrack, dstIndex, clipStart);
        }
        log("TIMELINE", "moveClip multi src=" + std::to_string(srcTrack) + ":" + std::to_string(srcClip) +
                          " dst=" + std::to_string(dstTrack) + ":" + std::to_string(dstIndex) +
                          " pos=" + std::to_string(clipStart));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeAddClip(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "-1";
    int track = safeStoi(a.substr(0, p1));
    auto p2 = a.find(',', p1 + 1);
    std::string path;
    uint64_t start = 0;
    if (p2 == std::string::npos) {
        path = a.substr(p1 + 1);
    } else {
        path = a.substr(p1 + 1, p2 - p1 - 1);
        start = (uint64_t)std::max(0, safeStoi(a.substr(p2 + 1)));
    }
    int idx = g_engine->addClip(track, path.c_str(), start);
    log("TIMELINE", "addClip track=" + std::to_string(track) + " idx=" + std::to_string(idx) + " path=" + path);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return std::to_string(idx);
}

static std::string nativeRemoveClip(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "false";
    int track = safeStoi(a.substr(0, p));
    int clip = safeStoi(a.substr(p + 1));
    bool ok = g_engine->removeClip(track, clip);
    log("TIMELINE", "removeClip track=" + std::to_string(track) + " clip=" + std::to_string(clip));
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return ok ? "true" : "false";
}

static std::string nativeSplitClip(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "-1";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "-1";
    int track = safeStoi(a.substr(0, p1));
    int clip = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    uint64_t pos = (uint64_t)std::max(0, safeStoi(a.substr(p2 + 1)));
    int idx = g_engine->splitClipAt(track, clip, pos);
    log("TIMELINE", "splitClip track=" + std::to_string(track) + " clip=" + std::to_string(clip) +
                    " pos=" + std::to_string(pos) + " newIdx=" + std::to_string(idx));
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return std::to_string(idx);
}

static std::string nativeTrimClip(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "false";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "false";
    auto p3 = a.find(',', p2 + 1);
    if (p3 == std::string::npos) return "false";
    int track = safeStoi(a.substr(0, p1));
    int clip = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    uint64_t newStart = (uint64_t)std::max(0, safeStoi(a.substr(p2 + 1, p3 - p2 - 1)));
    uint64_t newEnd = (uint64_t)std::max(0, safeStoi(a.substr(p3 + 1)));
    bool ok = g_engine->trimClip(track, clip, newStart, newEnd);
    log("TIMELINE", "trimClip track=" + std::to_string(track) + " clip=" + std::to_string(clip));
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return ok ? "true" : "false";
}

static std::string nativeSetFadeIn(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "null";
    int track = safeStoi(a.substr(0, p1));
    int clip = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    uint64_t samples = (uint64_t)std::max(0, safeStoi(a.substr(p2 + 1)));
    g_engine->setClipFadeIn(track, clip, samples);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetFadeOut(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "null";
    int track = safeStoi(a.substr(0, p1));
    int clip = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    uint64_t samples = (uint64_t)std::max(0, safeStoi(a.substr(p2 + 1)));
    g_engine->setClipFadeOut(track, clip, samples);
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetPan(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(a.substr(0, pos));
    float val = safeStof(a.substr(pos + 1));
    Track* t = g_engine->getTrack(idx);
    if (t) {
        t->pan.store(val, std::memory_order_relaxed);
        log("MIX", "setPan track=" + std::to_string(idx) + " val=" + std::to_string(val));
    }
    return "null";
}

static std::string nativeSetMute(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(a.substr(0, pos));
    bool val = (a.substr(pos + 1) == "1");
    Track* t = g_engine->getTrack(idx);
    if (t) {
        t->muted.store(val, std::memory_order_relaxed);
        log("MIX", "setMute track=" + std::to_string(idx) + " val=" + (val ? "true" : "false"));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetSolo(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(a.substr(0, pos));
    bool val = (a.substr(pos + 1) == "1");
    Track* t = g_engine->getTrack(idx);
    if (t) {
        t->soloed.store(val, std::memory_order_relaxed);
        log("MIX", "setSolo track=" + std::to_string(idx) + " val=" + (val ? "true" : "false"));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeSetArm(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(a.substr(0, pos));
    bool val = (a.substr(pos + 1) == "1");
    Track* t = g_engine->getTrack(idx);
    if (t) {
        t->armed.store(val, std::memory_order_relaxed);
        log("MIX", "setArm track=" + std::to_string(idx) + " val=" + (val ? "true" : "false"));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeLoadWav(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) {
        log("WAV", "loadWav failed: bad req or no engine");
        return "false";
    }
    int idx = safeStoi(a.substr(0, pos));
    std::string path = a.substr(pos + 1);
    if (path.empty()) {
        log("WAV", "loadWav failed: empty path");
        return "false";
    }
    log("WAV", "loadWav track=" + std::to_string(idx) + " path=" + path);
    bool ok = g_engine->loadWavToTrack(idx, path.c_str());
    if (ok) {
        log("WAV", "loadWav OK track=" + std::to_string(idx));
    } else {
        log("WAV", "loadWav FAILED");
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return ok ? "true" : "false";
}

static std::string nativeGetPeakCache(const std::string& req) {
    if (!g_engine) return "[]";
    int idx = safeStoi(unwrapArg(req));
    Track* t = g_engine->getTrack(idx);
    // Acquire load: synchronize with the release store in
    // loadWavToTrack so we see the up-to-date frames and
    // dataL/dataR vectors. (See comment in uiUpdateLoop for the
    // full rationale.)
    AudioBuffer* buf = t ? t->audio.load(std::memory_order_acquire) : nullptr;
    if (!buf || buf->frames == 0) {
        log("WAV", "getPeakCache idx=" + std::to_string(idx) + " no buffer or zero frames");
        return "[]";
    }
    // Always recompute the peak cache to guarantee consistent
    // temporal resolution (~10ms) regardless of when the buffer
    // was created. Caches pre-computed by loadWavToTrack/addClip/
    // splitClipAt/etc. could carry a different formula version.
    // Re-computation is O(frames) but only runs once per JS-side
    // cache invalidation — typical clips complete in <10ms.
    {
        size_t expectedNumP = (size_t)(buf->frames / 256);
        if (expectedNumP < 1) expectedNumP = 1;
        bool stale = buf->peakCache.empty() || buf->peakCache.size() != expectedNumP;
        if (stale) {
            buf->peakCache.clear();
            buf->peakCache.resize(expectedNumP);
            size_t numP = expectedNumP;
            for (size_t p = 0; p < numP; ++p) {
                size_t s = (size_t)((uint64_t)p * buf->frames / numP);
                size_t e = (size_t)((uint64_t)(p + 1) * buf->frames / numP);
                if (e > (size_t)buf->frames) e = (size_t)buf->frames;
                if (e <= s) e = s + 1;
                float pk = 0.0f;
                for (size_t i = s; i < e; ++i) {
                    pk = std::max(pk, std::abs(buf->dataL[i]));
                    pk = std::max(pk, std::abs(buf->dataR[i]));
                }
                buf->peakCache[p] = pk;
            }
        }
    }
    std::ostringstream js;
    js.imbue(std::locale::classic());
    js << "[";
    for (size_t i = 0; i < buf->peakCache.size(); ++i) {
        if (i > 0) js << ",";
        js << safePeak(buf->peakCache[i]);
    }
    js << "]";
    return js.str();
}

static std::string nativeListDir(const std::string& req) {
    std::string a = unwrapArg(req);
    std::ostringstream js;
    js << "[";
    bool first = true;
    int count = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(a)) {
            if (!first) js << ",";
            first = false;
            ++count;
            std::string name = entry.path().filename().string();
            std::string ext = entry.path().extension().string();
            js << "{"
               << "\"name\":\"" << escapeJson(name) << "\","
               << "\"dir\":" << (entry.is_directory() ? "true" : "false") << ","
               << "\"ext\":\"" << escapeJson(ext) << "\""
               << "}";
        }
    } catch (const std::filesystem::filesystem_error& e) {
        log("DIR", "listDir error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        log("DIR", "listDir exception: " + std::string(e.what()));
    }
    js << "]";
    return js.str();
}

static std::string nativeGetCwd(const std::string&) {
    std::string cwd = std::filesystem::current_path().string();
    return "\"" + escapeJson(cwd) + "\"";
}

static std::string nativeGetAudioInfo(const std::string& req) {
    if (!g_engine) return "{\"hasAudio\":false}";
    int idx = safeStoi(unwrapArg(req));
    Track* t = g_engine->getTrack(idx);
    AudioBuffer* buf = t ? t->audio.load(std::memory_order_relaxed) : nullptr;
    std::ostringstream js;
    js << "{";
    js << "\"hasAudio\":" << (buf && buf->frames > 0 ? "true" : "false") << ",";
    js << "\"frames\":" << (buf ? (int64_t)buf->frames : 0) << ",";
    js << "\"clipStart\":" << (t ? (int64_t)t->clipStart.load(std::memory_order_relaxed) : 0);
    js << "}";
    return js.str();
}

static std::string nativePluginList(const std::string& req) {
    if (!g_engine) return "[]";
    int idx = safeStoi(unwrapArg(req));
    PluginChain* chain = g_engine->getPluginChain(idx);
    if (!chain) return "[]";
    std::ostringstream js;
    js << "[";
    for (int i = 0; i < chain->getPluginCount(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"name\":\"" << escapeJson(chain->getPluginName(i)) << "\","
           << "\"bypassed\":" << (chain->isBypassed(i) ? "true" : "false")
           << "}";
    }
    js << "]";
    return js.str();
}

// nativePluginBypass was removed (dead binding — JS does not call it).

// Forward declarations for GUI helpers (defined later in this file)
static PluginGuiState* findPluginGui(int trackIdx, int pluginIdx);
static PluginGuiState* findPluginGuiByHandle(int handle);
static bool erasePluginGuiByHandle(int handle, PluginGuiState* outState);

// Close all open plugin GUI windows. Only destroys the GTK windows
// (does NOT call plugin gui->hide/destroy — those may block on the
// plugin's internal threads after the audio engine has stopped and
// the host is shutting down). The plugins themselves are cleaned up
// when the AudioEngine destructor runs at process exit.
static void closeAllPluginGuis() {
    std::vector<GtkWidget*> windows;
    {
        std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
        for (auto& g : g_pluginGuis)
            if (g.window) windows.push_back(g.window);
        g_pluginGuis.clear();
        g_pluginGuisByHandle.clear();
    }
    for (auto* w : windows) gtk_widget_destroy(w);
}

// nativePluginRemove was removed (dead binding — JS does not call it).
// Plugin removal in the live app would go through a future bridge-side
// method. For now, plugins are removed only by loading a new project
// (which clears all tracks).

static std::string nativePluginMove(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    int trackIdx = safeStoi(a.substr(0, p1));
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "null";
    int from = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    int to   = safeStoi(a.substr(p2 + 1));
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (!chain) return "null";
    int count = chain->getPluginCount();
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return "null";
    }
    chain->movePlugin(from, to);
    // The plugin that was at `from` is now at `to`. All other plugins
    // shift accordingly. Update the `pluginIdx` field of any open
    // PluginGuiState entries for this track so the GUI toggle/close
    // paths still find the right plugin. The handle itself is stable
    // and was already installed on the plugin's dedicated host, so the
    // CLAP plugin's host_data is unaffected.
    {
        std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
        for (auto& g : g_pluginGuis) {
            if (g.trackIdx != trackIdx) continue;
            int idx = g.pluginIdx;
            int newIdx = idx;
            if (idx == from) {
                newIdx = to;
            } else if (from < to && idx > from && idx <= to) {
                --newIdx;
            } else if (from > to && idx >= to && idx < from) {
                ++newIdx;
            }
            g.pluginIdx = newIdx;
        }
    }
    log("PLUGIN", "move track=" + std::to_string(trackIdx) +
        " from=" + std::to_string(from) + " to=" + std::to_string(to));
    g_engine->rebuildLatencyMap();
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativePluginLoad(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "false";
    int trackIdx = safeStoi(a.substr(0, p1));
    auto p2 = a.find(',', p1 + 1);
    if (p2 == std::string::npos) return "false";
    std::string libPath = a.substr(p1 + 1, p2 - p1 - 1);
    std::string pluginId = a.substr(p2 + 1);
    log("PLUGIN", "load track=" + std::to_string(trackIdx) +
        " lib=" + libPath + " id=" + pluginId);
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (!chain) return "false";
    bool ok = chain->addPlugin(libPath.c_str(), pluginId.c_str());
    log("PLUGIN", "load -> " + std::string(ok ? "OK" : "FAIL"));
    if (ok) g_engine->rebuildLatencyMap();
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return ok ? "true" : "false";
}

static std::string nativePageReady(const std::string&) {
    g_pageReady.store(true, std::memory_order_release);
    log("SYS", "pageReady signal received");
    return "null";
}

struct ClapEntry {
    std::string path, id, name;
};
static std::string nativeScanClap(const std::string& req) {
    std::vector<ClapEntry> entries;
    std::vector<std::string> paths = {req, "/usr/lib/clap", "/usr/local/lib/clap"};
    for (auto& dirPath : paths) {
        if (!std::filesystem::is_directory(dirPath)) continue;
        for (auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.path().extension() != ".clap") continue;
            std::string libPath = entry.path().string();
            void* lib = ClapHost::loadLibrary(libPath.c_str());
            if (!lib) continue;
            uint32_t count = ClapHost::getPluginCount(lib);
            for (uint32_t pi = 0; pi < count; ++pi) {
                auto* desc = ClapHost::getPluginDescriptor(lib, pi);
                if (!desc || !desc->id) continue;
                entries.push_back({libPath, desc->id,
                    desc->name ? std::string(desc->name) : std::string(desc->id)});
            }
            ClapHost::unloadLibrary(lib, libPath.c_str());
        }
    }
    // Sort by name case-insensitively
    std::sort(entries.begin(), entries.end(), [](const ClapEntry& a, const ClapEntry& b) {
        size_t len = std::min(a.name.size(), b.name.size());
        for (size_t i = 0; i < len; ++i) {
            int ca = std::tolower((unsigned char)a.name[i]);
            int cb = std::tolower((unsigned char)b.name[i]);
            if (ca != cb) return ca < cb;
        }
        return a.name.size() < b.name.size();
    });
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"path\":\"" << escapeJson(entries[i].path) << "\","
           << "\"id\":\"" << escapeJson(entries[i].id) << "\","
           << "\"name\":\"" << escapeJson(entries[i].name) << "\""
           << "}";
    }
    js << "]";
    return js.str();
}

// ── PLUGIN GUI ──


static PluginGuiState* findPluginGui(int trackIdx, int pluginIdx) {
    std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
    for (auto& g : g_pluginGuis)
        if (g.trackIdx == trackIdx && g.pluginIdx == pluginIdx) return &g;
    return nullptr;
}

// Look up a GUI by its stable handle. Returns nullptr if the handle was
// erased in the meantime. Safe to call from any thread.
static PluginGuiState* findPluginGuiByHandle(int handle) {
    if (handle <= 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
    auto it = g_pluginGuisByHandle.find(handle);
    if (it == g_pluginGuisByHandle.end()) return nullptr;
    int idx = it->second;
    if (idx < 0 || idx >= (int)g_pluginGuis.size()) return nullptr;
    if (g_pluginGuis[idx].handle != handle) return nullptr;
    return &g_pluginGuis[idx];
}

static bool erasePluginGuiByHandle(int handle, PluginGuiState* outState) {
    std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
    auto it = g_pluginGuisByHandle.find(handle);
    if (it == g_pluginGuisByHandle.end()) return false;
    int idx = it->second;
    if (idx < 0 || idx >= (int)g_pluginGuis.size()) return false;
    if (g_pluginGuis[idx].handle != handle) return false;
    if (outState) *outState = g_pluginGuis[idx];
    g_pluginGuis.erase(g_pluginGuis.begin() + idx);
    g_pluginGuisByHandle.erase(it);
    for (auto& kv : g_pluginGuisByHandle) {
        if (kv.second > idx) --kv.second;
    }
    return true;
}

// Plugin host callbacks. The CLAP host callbacks may be invoked from any
// thread the plugin chooses (commonly the audio thread or a timer thread).
// GTK/WebKit must be touched only from the GTK main thread, so we always
// dispatch to it via g_main_context_invoke.

// Spec: request_resize is [thread-safe]. The plugin may call this from
// any thread (audio thread, XEmbed thread, etc.). GTK requires GTK calls
// on the main thread, so we dispatch to it via g_main_context_invoke.
// We always return true (async accept) per spec:
//   "If not called from the main thread, then a return value simply means
//    that the host acknowledged the request and will process it asynchronously."
struct ResizePayload { int handle; int w; int h; };
static bool clap_host_request_resize(const clap_host_t* host, uint32_t width, uint32_t height) {
    dlog("clap_host_request_resize: w=" + std::to_string(width) + " h=" + std::to_string(height) + " thread=" + threadId());
    intptr_t id = (intptr_t)host->host_data;
    int handle = (int)(id & 0x7FFFFFFF);
    if (handle <= 0) return false;
    // Validate the handle exists before we accept. The lookup also
    // serves as a barrier so we know the GUI was opened.
    auto* gs = findPluginGuiByHandle(handle);
    if (!gs || !gs->window) return false;
    auto* p = new ResizePayload{handle, (int)width, (int)height};
    g_main_context_invoke(nullptr, (GSourceFunc)(+[](void* ud) -> gboolean {
        auto* pl = static_cast<ResizePayload*>(ud);
        PluginGuiState* gs2 = findPluginGuiByHandle(pl->handle);
        if (gs2 && gs2->window) {
            gtk_window_resize(GTK_WINDOW(gs2->window), pl->w, pl->h);
        }
        delete pl;
        return G_SOURCE_REMOVE;
    }), p);
    return true;
}

static void clap_host_gui_resize_hints_changed(const clap_host_t* host) {}
static bool clap_host_request_show(const clap_host_t* host) { return false; }
static bool clap_host_request_hide(const clap_host_t* host) { return false; }

// Called by the plugin when it has destroyed its own GUI (e.g. on sample
// accurate teardown, or when the plugin window was closed by the OS
// without our GTK window's destroy signal firing).
//
// CRITICAL: this can be called from ANY thread the plugin chooses (audio
// thread, plugin timer thread, etc.). We MUST NOT touch GTK here and
// MUST NOT take g_pluginGuisMutex if the main thread might already be
// holding it (the destroy signal handler also takes it). Doing either
// would deadlock the process — the symptom is a hard freeze the user
// perceives as "the program crashed".
//
// The safe approach is to do nothing here. The cleanup is handled by:
//   - The "destroy" signal handler in nativePluginShowGUI (main thread)
//   - closeAllPluginGuis at shutdown (main thread)
// If the plugin closes its GUI without our destroy signal firing we
// just leak the (empty) GTK window — the user can close it manually.
// Called by the plugin when it has destroyed its own GUI (e.g. on sample
// accurate teardown, or when the plugin window was closed by the OS
// without our GTK window's destroy signal firing).
//
// CRITICAL: this can be called from ANY thread the plugin chooses (audio
// thread, plugin timer thread, etc.). We MUST NOT touch GTK here and
// MUST NOT take g_pluginGuisMutex if the main thread might already be
// holding it (the destroy signal handler also takes it). Doing either
// would deadlock the process — the symptom is a hard freeze the user
// perceives as "the program crashed".
//
// The safe approach is to dispatch the cleanup to the main thread via
// g_main_context_invoke. The dispatched function runs on the main thread
// (where GTK calls are safe) and takes the mutex.
static void clap_host_gui_closed(const clap_host_t* host, bool was_destroyed) {
    dlog("clap_host_gui_closed: was_destroyed=" + std::string(was_destroyed ? "true" : "false") + " thread=" + threadId());
    if (!host) return;
    intptr_t id = (intptr_t)host->host_data;
    int handle = (int)(id & 0x7FFFFFFF);
    if (handle <= 0) return;
    // Encode the was_destroyed flag in the upper bit of the int (0 or 1).
    int payload = handle | (was_destroyed ? 0x40000000 : 0);
    g_main_context_invoke(nullptr, (GSourceFunc)(+[](void* ud) -> gboolean {
        int p = GPOINTER_TO_INT(ud);
        int h = p & 0x3FFFFFFF;
        bool wasDestroyed = (p & 0x40000000) != 0;
        dlog("clap_host_gui_closed lambda: handle=" + std::to_string(h) +
             " wasDestroyed=" + std::string(wasDestroyed ? "true" : "false") +
             " thread=" + threadId());
        PluginGuiState stolen;
        if (!erasePluginGuiByHandle(h, &stolen)) return G_SOURCE_REMOVE;
        // Per spec: if was_destroyed is true, the host MUST call
        // gui->destroy() to acknowledge the gui destruction.
        if (wasDestroyed && stolen.plugin && stolen.gui && stolen.gui->destroy) {
            stolen.gui->destroy(stolen.plugin);
        }
        if (stolen.window) gtk_widget_destroy(stolen.window);
        return G_SOURCE_REMOVE;
    }), GINT_TO_POINTER(payload));
}

// ── Bridge-side IHostExtensions implementation ──
// The audio core's PluginChain looks up CLAP host extensions via
// the IHostExtensions interface. The bridge provides the GUI
// extension (which is bridge-side: it touches GTK/X11 to host the
// plugin's UI). The other extensions (timer, log, params,
// thread-check) live in the audio core; this provider returns null
// for them so the core falls back to its own implementations.
class BridgeHostExtensions : public hydraw::IHostExtensions {
public:
    const void* getExtension(const char* id) const noexcept override {
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &s_bridgeHostGui;
        return nullptr;
    }
private:
    // Trampoline: each C function pointer below is a thin wrapper
    // around the file-scope C functions declared above. They
    // operate on the global g_pluginGuis state.
    static const clap_host_gui_t s_bridgeHostGui;
};
const clap_host_gui_t BridgeHostExtensions::s_bridgeHostGui = {
    clap_host_gui_resize_hints_changed,
    clap_host_request_resize,
    clap_host_request_show,
    clap_host_request_hide,
    clap_host_gui_closed
};

// The bridge creates a single instance and passes it to the
// audio engine. The audio engine passes it to each PluginChain.
static BridgeHostExtensions g_bridgeHostExtensions;

// nativePluginShowGUI is called by index.html in two places:
//   1. Clicking a plugin name in the FX chain list (line 1537) opens
//      or toggles the plugin's GUI window.
//   2. The "Open GUI" button in the plugin info popup (line 1700).
// Both use cn('nativePluginShowGUI', track, plugin) via string concat.
static std::string nativePluginShowGUI(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "false";
    int trackIdx = safeStoi(a.substr(0, p));
    int pluginIdx = safeStoi(a.substr(p + 1));
    dlog("nativePluginShowGUI enter track=" + std::to_string(trackIdx) +
         " plugin=" + std::to_string(pluginIdx) + " thread=" + threadId());

    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (!chain) { dlog("nativePluginShowGUI: chain is null"); return "false"; }

    // Handle builtin plugins (soft clipper)
    if (chain->isBuiltin(pluginIdx)) {
        dlog("nativePluginShowGUI: builtin plugin path");
        int key = builtinKey(trackIdx, pluginIdx);
        bool wasOpen;
        {
            std::lock_guard<std::mutex> lk(g_builtinGuiOpenMutex);
            wasOpen = g_builtinGuiOpen[key];
            g_builtinGuiOpen[key] = !wasOpen;
        }
        float drive = chain->getDrive(pluginIdx);
        if (g_wv) {
            if (!wasOpen) {
                std::string js = "showSoftClipperGUI(" + std::to_string(trackIdx) + "," +
                                 std::to_string(pluginIdx) + "," +
                                 std::to_string(drive) + ")";
                g_wv->eval(js.c_str());
                log("PLUGIN", "Builtin GUI shown track " + std::to_string(trackIdx) +
                    " plugin " + std::to_string(pluginIdx));
            } else {
                g_wv->eval("closeSoftClipperGUI()");
                log("PLUGIN", "Builtin GUI closed track " + std::to_string(trackIdx) +
                    " plugin " + std::to_string(pluginIdx));
            }
        }
        return "true";
    }

    // Already open? Close it (toggle off).
    {
        PluginGuiState stolen;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
            for (auto it = g_pluginGuis.begin(); it != g_pluginGuis.end(); ++it) {
                if (it->trackIdx == trackIdx && it->pluginIdx == pluginIdx) {
                    stolen = *it;
                    int erasedIdx = (int)(it - g_pluginGuis.begin());
                    g_pluginGuis.erase(it);
                    auto hit = g_pluginGuisByHandle.find(stolen.handle);
                    if (hit != g_pluginGuisByHandle.end()) g_pluginGuisByHandle.erase(hit);
                    for (auto& kv : g_pluginGuisByHandle)
                        if (kv.second > erasedIdx) --kv.second;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            dlog("nativePluginShowGUI: closing existing GUI (toggle off)");
            if (stolen.plugin) {
                if (stolen.gui && stolen.gui->hide)    stolen.gui->hide(stolen.plugin);
                if (stolen.gui && stolen.gui->destroy) stolen.gui->destroy(stolen.plugin);
            }
            if (stolen.window) gtk_widget_destroy(stolen.window);
            log("PLUGIN", "GUI closed track " + std::to_string(trackIdx) +
                " plugin " + std::to_string(pluginIdx));
            return "null";
        }
    }

    dlog("nativePluginShowGUI: getting plugin ptr");
    const clap_plugin_t* plugin = chain->getPlugin(pluginIdx);
    if (!plugin) { dlog("nativePluginShowGUI: plugin ptr is null"); return "false"; }

    dlog("nativePluginShowGUI: calling get_extension(CLAP_EXT_GUI)");
    const clap_plugin_gui_t* gui = (const clap_plugin_gui_t*)
        plugin->get_extension(plugin, CLAP_EXT_GUI);
    if (!gui) { dlog("nativePluginShowGUI: no CLAP_EXT_GUI"); log("PLUGIN", "GUI: no CLAP_EXT_GUI"); return "false"; }
    if (!gui->is_api_supported) { dlog("nativePluginShowGUI: no is_api_supported"); log("PLUGIN", "GUI: no is_api_supported"); return "false"; }
    if (!gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false)) {
        dlog("nativePluginShowGUI: X11 not supported");
        log("PLUGIN", "GUI: X11 not supported track=" + std::to_string(trackIdx));
        return "false";
    }

    // Stable handle for the plugin's host callbacks (fixes master chain
    // sign-extension bug where (trackIdx<<16) with trackIdx=-1 overflowed).
    int handle = g_nextPluginGuiHandle++;
    dlog("nativePluginShowGUI: handle=" + std::to_string(handle) + " calling setPluginHandle");
    chain->setPluginHandle(pluginIdx, handle);

    dlog("nativePluginShowGUI: creating GtkWindow");
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    // GLArea trick: force a GL-capable visual so JUCE plugins embed.
    {
        GtkWidget* glArea = gtk_gl_area_new();
        if (glArea) {
            gtk_container_add(GTK_CONTAINER(win), glArea);
            gtk_widget_realize(glArea);
            gtk_container_remove(GTK_CONTAINER(win), glArea);
        }
    }
    const char* title = plugin->desc && plugin->desc->name ? plugin->desc->name : "Plugin";
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 600);
    gtk_widget_realize(win);
    dlog("nativePluginShowGUI: window realized");

    GdkWindow* gdkWin = gtk_widget_get_window(win);
    if (!gdkWin || !GDK_IS_X11_WINDOW(gdkWin)) {
        dlog("nativePluginShowGUI: not an X11 GdkWindow, aborting");
        gtk_widget_destroy(win); return "false";
    }
    clap_xwnd xid = GDK_WINDOW_XID(gdkWin);
    dlog("nativePluginShowGUI: xid=" + std::to_string((unsigned long)xid));

    dlog("nativePluginShowGUI: calling gui->create");
    if (!gui->create || !gui->create(plugin, CLAP_WINDOW_API_X11, false)) {
        dlog("nativePluginShowGUI: gui->create failed");
        gtk_widget_destroy(win); return "false";
    }
    dlog("nativePluginShowGUI: gui->create OK");
    if (gui->set_scale) gui->set_scale(plugin, 1.0);
    uint32_t gw = 800, gh = 600;
    if (gui->get_size) gui->get_size(plugin, &gw, &gh);
    if (gw < 32) gw = 32;
    if (gh < 32) gh = 32;
    gtk_window_resize(GTK_WINDOW(win), (int)gw, (int)gh);
    dlog("nativePluginShowGUI: gui size=" + std::to_string(gw) + "x" + std::to_string(gh));

    clap_window_t cw;
    cw.api = CLAP_WINDOW_API_X11;
    cw.x11 = xid;
    dlog("nativePluginShowGUI: calling gui->set_parent");
    if (!gui->set_parent || !gui->set_parent(plugin, &cw)) {
        dlog("nativePluginShowGUI: gui->set_parent failed");
        if (gui->destroy) gui->destroy(plugin);
        gtk_widget_destroy(win);
        return "false";
    }
    dlog("nativePluginShowGUI: gui->set_parent OK");
    dlog("nativePluginShowGUI: calling gui->show");
    if (!gui->show || !gui->show(plugin)) {
        dlog("nativePluginShowGUI: gui->show failed");
        if (gui->destroy) gui->destroy(plugin);
        gtk_widget_destroy(win);
        return "false";
    }
    dlog("nativePluginShowGUI: gui->show OK");

    // Push state and connect destroy signal (like the original, but with
    // the stable handle instead of the buggy (trackIdx<<16)|pluginIdx).
    {
        std::lock_guard<std::mutex> lk(g_pluginGuisMutex);
        PluginGuiState s;
        s.trackIdx = trackIdx;  s.pluginIdx = pluginIdx;
        s.window = win;  s.x11win = 0;  s.x11dpy = nullptr;
        s.plugin = plugin;  s.gui = gui;  s.handle = handle;
        g_pluginGuis.push_back(s);
        int idx = (int)g_pluginGuis.size() - 1;
        g_pluginGuisByHandle[handle] = idx;
    }
    g_signal_connect_data(win, "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            int h = GPOINTER_TO_INT(data);
            dlog("destroy signal: handle=" + std::to_string(h) + " thread=" + threadId());
            PluginGuiState stolen;
            if (!erasePluginGuiByHandle(h, &stolen)) return;
            if (stolen.plugin) {
                if (stolen.gui && stolen.gui->hide)    stolen.gui->hide(stolen.plugin);
                if (stolen.gui && stolen.gui->destroy) stolen.gui->destroy(stolen.plugin);
            }
        }),
        GINT_TO_POINTER(handle), nullptr, (GConnectFlags)0);

    dlog("nativePluginShowGUI: calling gtk_widget_show");
    gtk_widget_show(win);
    dlog("nativePluginShowGUI: gtk_widget_show returned — function about to return");

    log("PLUGIN", "GUI shown track " + std::to_string(trackIdx) + " plugin " + std::to_string(pluginIdx));
    return "true";
}

// nativePluginHideGUI is currently unused (the toggle is in
// nativePluginShowGUI itself: if the GUI is already open, calling
// Show again closes it). We keep the implementation available for
// future use.

// Intercept file drops at the WebKit level (bypasses JS security restrictions)

// Track the URI+time of the last drop so drag-data-received can skip
// when decide-policy already processed it.
static std::string g_lastDropUri;
static gint64 g_lastDropTime = 0;

static gboolean onDecidePolicy(WebKitWebView* webview, WebKitPolicyDecision* decision,
                                WebKitPolicyDecisionType type, gpointer user_data) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;
    auto* navDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    auto* action = webkit_navigation_policy_decision_get_navigation_action(navDecision);
    auto* request = webkit_navigation_action_get_request(action);
    const char* uri = webkit_uri_request_get_uri(request);
    if (!uri) return FALSE;
    std::string suri(uri);
    // Only intercept file:// URIs with audio extensions
    if (suri.find("file://") != 0) return FALSE;
    // Skip navigation to the app's own HTML page
    if (suri.find("index.html") != std::string::npos) return FALSE;
    std::string ext;
    auto dot = suri.find_last_of('.');
    if (dot != std::string::npos) {
        ext = suri.substr(dot);
        for (auto& c : ext) c = tolower(c);
    }
    if (ext != ".wav" && ext != ".mp3" && ext != ".flac" && ext != ".ogg"
        && ext != ".aac" && ext != ".m4a" && ext != ".wma" && ext != ".opus"
        && ext != ".aiff" && ext != ".aif")
        return FALSE;
    // Extract path: file:///path/file.wav -> /path/file.wav
    std::string path = suri.substr(7); // strip "file://"
    // URL-decode percent-encoded characters
    std::string decoded;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            char hex[3] = {path[i+1], path[i+2], 0};
            char* end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                decoded += (char)val;
                i += 2;
                continue;
            }
        }
        decoded += path[i];
    }
    auto* wv = static_cast<webview::webview*>(user_data);
    std::string escaped = escapeJson(decoded);
    g_lastDropUri = decoded;
    g_lastDropTime = g_get_monotonic_time();
    wv->eval("window._onNativeDropPath(\"" + escaped + "\")");
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

// Fallback: capture file URI from GTK drag-data-received when decide-policy
// doesn't fire (e.g., for audio files that WebKit doesn't try to navigate to)
static void onDragDataReceived(GtkWidget* widget, GdkDragContext* context,
                                gint x, gint y, GtkSelectionData* data,
                                guint info, guint time, gpointer user_data) {
    gchar** uris = gtk_selection_data_get_uris(data);
    if (!uris || !uris[0]) return;
    std::string suri(uris[0]);
    g_strfreev(uris);
    if (suri.find("file://") != 0) return;
    std::string ext;
    auto dot = suri.find_last_of('.');
    if (dot != std::string::npos) {
        ext = suri.substr(dot);
        for (auto& c : ext) c = tolower(c);
    }
    if (ext != ".wav" && ext != ".mp3" && ext != ".flac" && ext != ".ogg")
        return;
    // Extract path: file:///path/file.wav -> /path/file.wav
    std::string path = suri.substr(7);
    std::string decoded;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            char hex[3] = {path[i+1], path[i+2], 0};
            char* end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                decoded += (char)val;
                i += 2;
                continue;
            }
        }
        decoded += path[i];
    }
    gint64 now = g_get_monotonic_time();
    if (decoded == g_lastDropUri && (now - g_lastDropTime) < 5000000) {
        return;
    }
    auto* wv = static_cast<webview::webview*>(user_data);
    std::string escaped = escapeJson(decoded);
    g_lastDropUri = decoded;
    g_lastDropTime = now;
    wv->eval("window._onNativeDropPath(\"" + escaped + "\")");
}

static void crashHandler(int sig) {
    // ONLY async-signal-safe calls below. No std::mutex, no std::ofstream,
    // no malloc past the initial backtrace() call. fprintf to stderr and
    // to the pre-opened crash log file is safe.
    void* buf[32];
    int n = backtrace(buf, 32);
    char** symbols = backtrace_symbols(buf, n);
    fprintf(stderr, "\n=== CRASH (signal %d) ===\n", sig);
    if (g_crashLogFile) {
        fprintf(g_crashLogFile, "\n=== CRASH (signal %d) ===\n", sig);
    }
    if (symbols) {
        for (int i = 0; i < n; ++i) {
            fprintf(stderr, "  %s\n", symbols[i]);
            if (g_crashLogFile) {
                fprintf(g_crashLogFile, "  %s\n", symbols[i]);
            }
        }
        free(symbols);
    } else {
        fprintf(stderr, "  (backtrace_symbols failed)\n");
        if (g_crashLogFile) {
            fprintf(g_crashLogFile, "  (backtrace_symbols failed)\n");
        }
    }
    if (g_crashLogFile) fflush(g_crashLogFile);
    fflush(stderr);
    _exit(128 + sig);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    // CRITICAL: do this BEFORE anything else. PluginChain's static
    // clap_host.thread-check reads g_mainThreadId to verify the host is
    // calling on the right thread. If we set it lazily (on first call)
    // and the first call comes from the plugin's XEmbed thread, the
    // baseline gets poisoned and every subsequent is_main_thread()
    // check returns the wrong answer.
    // PluginChain no longer needs initMainThreadId() — the
    // thread-check logic was simplified to "is the audio
    // thread?" only (see PluginChain.cpp).

    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGBUS,  crashHandler);
    signal(SIGTERM, +[](int) { _exit(0); });
    signal(SIGINT,  +[](int) { _exit(0); });
    initLogger();
    // Force X11 for CLAP plugin GUI embedding. The third argument MUST be 1
    // (overwrite=1) so that the value sticks even if the user has
    // GDK_BACKEND=wayland in their environment. With overwrite=0, GTK would
    // silently fall back to Wayland and X11 plugin embedding would not work
    // — the plugin GUI would never appear because Wayland has no equivalent
    // to XEmbed.
    setenv("GDK_BACKEND", "x11", 1);
    log("SYS", "=== Hydraw DAW v" HYDRAW_VERSION " starting ===");

    // The bridge's ILogSink implementation. Writes to the same log
    // file as the main log() function. The audio core calls this
    // from any thread (including the audio thread) via the
    // hydraw::ILogSink interface.
    hydraw::FileLogSink fileLogSink;
    // Open using the existing logFile path. The logger file is
    // opened in initLogger(); we open the sink's file in
    // append mode. The two file handles share the underlying file
    // safely (writes are atomic for small <PIPE_BUF buffers).
    {
        std::filesystem::path lp = std::filesystem::current_path() /
            ("hydraw_" + std::to_string(getpid()) + ".log");
        if (!fileLogSink.open(lp.string())) {
            fprintf(stderr, "[BRIDGE] WARN: failed to open FileLogSink at %s\n", lp.c_str());
        }
    }

    AudioEngine audioEngine(&fileLogSink, &g_bridgeHostExtensions);
    if (!audioEngine.init()) {
        log("SYS", "FATAL: Failed to initialize audio engine");
        closeLogger();
        return 1;
    }
    audioEngine.start();
    g_engine = &audioEngine;
    log("SYS", "Audio engine initialized and started");

    webview::webview w(true, nullptr);
    g_wv = &w;
    w.set_title("Hydraw DAW");
    w.set_size(1280, 720, WEBVIEW_HINT_NONE);

    // Register native drop interceptor at WebKit level
    auto browserCtrl = w.browser_controller();
    if (browserCtrl.has_value()) {
    auto* webkitView = WEBKIT_WEB_VIEW(browserCtrl.value());
    g_signal_connect(webkitView, "decide-policy",
                     G_CALLBACK(onDecidePolicy), &w);
    // Also connect drag-data-received as fallback for audio files
    // that don't trigger navigation policy
    g_signal_connect(webkitView, "drag-data-received",
                     G_CALLBACK(onDragDataReceived), &w);
    log("SYS", "Native drop interceptor registered");
    } else {
        log("SYS", "WARNING: Could not get WebKitWebView for drop interceptor");
    }

    w.bind("nativePlay", nativePlay);
    w.bind("nativePause", nativePause);
    w.bind("nativeStop", nativeStop);
    w.bind("nativeSetPlayhead", nativeSetPlayhead);
    w.bind("nativeSetBPM", nativeSetBPM);
    w.bind("nativeSetTimeSignature", nativeSetTimeSignature);
    w.bind("nativeSetLoopEnabled", nativeSetLoopEnabled);
    w.bind("nativeSetLoopRange", nativeSetLoopRange);
    w.bind("nativeSetPunchRange", nativeSetPunchRange);
    w.bind("nativeStartRecording", nativeStartRecording);
    w.bind("nativeStopRecording", nativeStopRecording);
    w.bind("nativeFreezeTrack", nativeFreezeTrack);
    w.bind("nativeUnfreezeTrack", nativeUnfreezeTrack);
    w.bind("nativeGetPluginParams", nativeGetPluginParams);
    w.bind("nativeSetPluginParam", nativeSetPluginParam);
    // Undo/Redo wrappers
    w.bind("nativeAddMidiNote", [](const std::string& req) -> std::string {
        std::string a = unwrapArg(req);
        auto p1 = a.find(',');
        if (p1 == std::string::npos || !g_engine) return "-1";
        int track = safeStoi(a.substr(0, p1));
        auto p2 = a.find(',', p1 + 1);
        if (p2 == std::string::npos) return "-1";
        auto p3 = a.find(',', p2 + 1);
        if (p3 == std::string::npos) return "-1";
        auto p4 = a.find(',', p3 + 1);
        if (p4 == std::string::npos) return "-1";
        MidiNote n;
        n.start = (uint64_t)std::max(0, safeStoi(a.substr(p1 + 1, p2 - p1 - 1)));
        n.duration = (uint64_t)std::max(1, safeStoi(a.substr(p2 + 1, p3 - p2 - 1)));
        n.pitch = (uint8_t)std::max(0, std::min(127, safeStoi(a.substr(p3 + 1, p4 - p3 - 1))));
        n.velocity = (uint8_t)std::max(1, std::min(127, safeStoi(a.substr(p4 + 1))));
        return std::to_string(g_engine->addMidiNote(track, n));
    });
    w.bind("nativeClearMidiLane", [](const std::string& req) -> std::string {
        int track = safeStoi(unwrapArg(req));
        if (g_engine) g_engine->clearMidiLane(track);
        return "true";
    });
    w.bind("nativeGetMidiNoteCount", [](const std::string& req) -> std::string {
        int track = safeStoi(unwrapArg(req));
        if (!g_engine) return "0";
        return std::to_string(g_engine->getMidiNoteCount(track));
    });
    w.bind("nativeUndo", [](const std::string&) -> std::string {
        if (!g_engine || g_undoStack.empty()) return "false";
        g_redoStack.push_back(ProjectSerializer::saveToString(g_engine, g_currentProjectDir));
        if (g_redoStack.size() > kUndoCapacity) g_redoStack.erase(g_redoStack.begin());
        std::string snap = std::move(g_undoStack.back());
        g_undoStack.pop_back();
        g_engine->stop();
        g_engine->drainPendingAudioBufferDeletes();
        bool ok = ProjectSerializer::loadFromString(snap, g_engine, g_currentProjectDir);
        g_engine->start();
        g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
        log("UNDO", ok ? "OK" : "FAIL");
        return ok ? "true" : "false";
    });
    w.bind("nativeRedo", [](const std::string&) -> std::string {
        if (!g_engine || g_redoStack.empty()) return "false";
        g_undoStack.push_back(ProjectSerializer::saveToString(g_engine, g_currentProjectDir));
        if (g_undoStack.size() > kUndoCapacity) g_undoStack.erase(g_undoStack.begin());
        std::string snap = std::move(g_redoStack.back());
        g_redoStack.pop_back();
        g_engine->stop();
        g_engine->drainPendingAudioBufferDeletes();
        bool ok = ProjectSerializer::loadFromString(snap, g_engine, g_currentProjectDir);
        g_engine->start();
        g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
        log("REDO", ok ? "OK" : "FAIL");
        return ok ? "true" : "false";
    });
    w.bind("nativeAddTrack", nativeAddTrack);
    w.bind("nativeRemoveTrack", nativeRemoveTrack);
    w.bind("nativeSetVolume", nativeSetVolume);
    w.bind("nativeSetMasterVolume", nativeSetMasterVolume);
    w.bind("nativeSetSendLevel", nativeSetSendLevel);
    w.bind("nativeSetAuxVolume", nativeSetAuxVolume);
    w.bind("nativeSetClipStart", nativeSetClipStart);
    w.bind("nativeMoveClip", nativeMoveClip);
    w.bind("nativeSetPan", nativeSetPan);
    w.bind("nativeSetMute", nativeSetMute);
    w.bind("nativeSetSolo", nativeSetSolo);
    w.bind("nativeSetArm", nativeSetArm);
    w.bind("nativeLoadWav", nativeLoadWav);
    w.bind("nativeAddClip", nativeAddClip);
    w.bind("nativeRemoveClip", nativeRemoveClip);
    w.bind("nativeSplitClip", nativeSplitClip);
    w.bind("nativeTrimClip", nativeTrimClip);
    w.bind("nativeSetFadeIn", nativeSetFadeIn);
    w.bind("nativeSetFadeOut", nativeSetFadeOut);
    w.bind("nativeGetPeakCache", nativeGetPeakCache);
    w.bind("nativeListDir", nativeListDir);
    w.bind("nativeGetCwd", nativeGetCwd);
    w.bind("nativeGetAudioInfo", nativeGetAudioInfo);
    w.bind("nativePluginList", nativePluginList);
    w.bind("nativePluginLoad", nativePluginLoad);
    w.bind("nativePluginMove", nativePluginMove);
    w.bind("nativeScanClap", nativeScanClap);
    w.bind("nativePluginShowGUI", nativePluginShowGUI);
    w.bind("nativePageReady", nativePageReady);
    w.bind("nativeLog", nativeLog);  // JS→C++ log relay
    w.bind("nativeShowSaveDialog", nativeShowSaveDialog);
    w.bind("nativeShowOpenDialog", nativeShowOpenDialog);
    w.bind("nativeSaveProject", nativeSaveProject);
    w.bind("nativeLoadProject", nativeLoadProject);
    w.bind("nativeExportAudio", nativeExportAudio);
    w.bind("nativeSetSoftClipperDrive", nativeSetSoftClipperDrive);

    log("SYS", "Native bindings registered (33 active, 4 dead removed in bridge refactor)");

    w.init(
        "window.updateUIFromNative=function(){};"
        "window.onNativeUpdate=function(){};"
        "window.addEventListener('DOMContentLoaded',function(){window.nativePageReady('')});"
    );

    std::thread uiThread(uiUpdateLoop);

    std::filesystem::path htmlPath = std::filesystem::current_path() / "assets" / "index.html";
    if (std::filesystem::exists(htmlPath)) {
        // Cache-buster: WebKitGTK caches file:// content by URL. We tried
        // using the file's mtime but std::filesystem::file_clock on
        // glibc has an epoch around year 2117, so duration_cast<seconds>
        // gives a NEGATIVE number that WebKit treats as the same URL
        // across restarts. Use system_clock::now() (epoch 1970) instead
        // so the query is always a positive, unique value per run.
        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string url = "file://" + htmlPath.string() + "?v=" + std::to_string(now_s);
        w.navigate(url.c_str());
        log("SYS", "Navigated to " + htmlPath.string() + " (cache-buster v=" + std::to_string(now_s) + ")");
    } else {
        log("SYS", "FATAL: Could not find " + htmlPath.string());
        g_running.store(false);
        uiThread.join();
        audioEngine.shutdown();
        closeLogger();
        return 1;
    }

    log("SYS", "Entering webview main loop");
    w.run();

    log("SYS", "Window closed, shutting down...");

    // 1) Stop the audio engine first so the audio thread is no longer
    //    calling into plugins. ma_device_uninit blocks until the audio
    //    thread is fully done.
    log("SYS", "shutdown: stopping audio engine...");
    audioEngine.stop();
    log("SYS", "shutdown: audio engine stopped, uninit...");
    audioEngine.shutdown();
    log("SYS", "shutdown: audio engine shutdown complete");

    // 2) Signal the UI thread to stop and wait for any in-flight export.
    //    This MUST happen BEFORE we touch g_pluginGuis* (which the UI
    //    thread iterates every 30ms) or closeAllPluginGuis (which clears
    //    the maps) — otherwise concurrent iteration would corrupt the
    //    unordered_map and produce a "corrupted double-linked list" abort.
    log("SYS", "shutdown: stopping UI thread...");
    g_running.store(false);
    int waitIters = 0;
    while (g_exportBusy.load(std::memory_order_acquire) > 0 && waitIters < 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++waitIters;
    }
    log("SYS", "shutdown: joining UI thread...");
    uiThread.join();
    log("SYS", "shutdown: UI thread joined");

    // 3) Tear down every plugin GUI window. Safe now because the UI
    //    thread is dead — no concurrent access to g_pluginGuis*.
    log("SYS", "shutdown: closing plugin GUIs...");
    closeAllPluginGuis();
    log("SYS", "shutdown: plugin GUIs closed, pumping events...");
    for (int i = 0; i < 5; ++i) {
        int iterCount = 0;
        while (g_main_context_iteration(nullptr, FALSE) && ++iterCount < 100) {}
    }
    log("SYS", "shutdown: events flushed");

    g_wv = nullptr;
    g_engine = nullptr;

    log("SYS", "=== Hydraw DAW shutdown complete ===");
    closeLogger();
    return 0;
}
