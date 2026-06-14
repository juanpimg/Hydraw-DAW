#include "AudioEngine.h"
#include "Plugin/PluginChain.h"
#include "Plugin/ClapHost.h"
#include "Project/ProjectSerializer.h"
#include <webview/webview.h>
#include <webkit2/webkit2.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <GL/glx.h>
#include <set>
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
constexpr uint32_t kDirtyExtended = 1u << 0;

// ── PLUGIN GUI ── (must be before uiUpdateLoop)
struct PluginGuiState {
    int trackIdx;
    int pluginIdx;
    GtkWidget* window;
    Window x11win;
    Display* x11dpy;
    const clap_plugin_t* plugin;
    const clap_plugin_gui_t* gui;
};
static std::vector<PluginGuiState> g_pluginGuis;

static std::ofstream g_logFile;
static std::mutex g_logMutex;

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
}

static void closeLogger() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile << "=== Hydraw DAW log ended ===" << std::endl;
        g_logFile.close();
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
    int tickCount = 0;
    int s_lastTrackCount = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(30ms);
        if (!g_engine || !g_wv || !g_pageReady.load(std::memory_order_acquire)) {
            ++tickCount;
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

        std::ostringstream js;
        js.imbue(std::locale::classic());
        js << "updateUIFromNative(["
           << (sendFull ? "1" : "0") << ","
           << (playing ? "1" : "0") << "," << ph << "," << tc << ","
           << safePeak(master ? master->peakL.load(std::memory_order_relaxed) : 0.0f) << ","
           << safePeak(master ? master->peakR.load(std::memory_order_relaxed) : 0.0f) << ","
           << safeFloat(master ? master->volume.load(std::memory_order_relaxed) : 1.0f, 1.0f);
        // Track peaks (always sent)
        js << ",[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << safePeak(std::max(g_engine->getTrack(i)->peakL.load(std::memory_order_relaxed),
                           g_engine->getTrack(i)->peakR.load(std::memory_order_relaxed)));
        }
        js << "]";

        if (sendFull) {
            // Volumes
            js << ",[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                js << safeFloat(g_engine->getTrack(i)->volume.load(std::memory_order_relaxed), 1.0f);
            }
            // Mutes
            js << "],[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                js << (g_engine->getTrack(i)->muted.load(std::memory_order_relaxed) ? "1" : "0");
            }
            // Solos
            js << "],[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                js << (g_engine->getTrack(i)->soloed.load(std::memory_order_relaxed) ? "1" : "0");
            }
            // Arms
            js << "],[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                js << (g_engine->getTrack(i)->armed.load(std::memory_order_relaxed) ? "1" : "0");
            }
            // Names
            js << "],[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                js << "\"" << escapeJson(g_engine->getTrack(i)->name) << "\"";
            }
            // Audio frames
            js << "],[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                AudioBuffer* buf = g_engine->getTrack(i)->audio.load(std::memory_order_relaxed);
                js << (buf ? (int64_t)buf->frames : 0);
            }
            // Clip starts
            js << "],[";
            for (int i = 0; i < tc; ++i) {
                if (i > 0) js << ",";
                js << (int64_t)g_engine->getTrack(i)->clipStart.load(std::memory_order_relaxed);
            }
        }
        js << "])";

        g_wv->eval(js.str());
        ++tickCount;

        // Fire plugin timers so DPF/Pugl can render the GUI (on GTK windows)
        for (auto& gs : g_pluginGuis) {
            if (gs.plugin) {
                auto* timer = (const clap_plugin_timer_support_t*)
                    gs.plugin->get_extension(gs.plugin, CLAP_EXT_TIMER_SUPPORT);
                if (timer && timer->on_timer)
                     timer->on_timer(gs.plugin, 1);
            }
        }

        // Process pending flush requests from plugins (parameter sync)
        PluginChain::processPendingFlushes();
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

// ── PROJECT / EXPORT ──
static std::string nativeSaveProject(const std::string& req) {
    std::string path = unwrapArg(req);
    bool ok = g_engine ? ProjectSerializer::save(path.c_str(), g_engine) : false;
    log("SYS", "saveProject -> " + path + " " + (ok ? "OK" : "FAIL"));
    return ok ? "\"OK\"" : "\"FAIL\"";
}

static std::string nativeLoadProject(const std::string& req) {
    std::string path = unwrapArg(req);
    bool ok = g_engine ? ProjectSerializer::load(path.c_str(), g_engine) : false;
    log("SYS", "loadProject -> " + path + " " + (ok ? "OK" : "FAIL"));
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
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

    // Launch export on a worker thread so the web process doesn't hang
    std::thread t([wavPath, sampleRate, bitDepth]() {
        bool ok = g_engine ? g_engine->renderOffline(wavPath.c_str(), sampleRate, bitDepth) : false;
        log("EXPORT", std::string("done ") + (ok ? "OK" : "FAIL"));
        // Notify JS from the worker thread (eval posts to main loop internally)
        if (g_wv && g_running.load(std::memory_order_acquire)) {
            g_wv->eval(("window.onExportDone && window.onExportDone(" +
                        std::string(ok ? "'OK'" : "'FAIL'") + ")").c_str());
        }
    });
    t.detach();
    return "\"PENDING\"";
}

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

static std::string nativeSetClipStart(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(a.substr(0, pos));
    uint64_t clipStart = (uint64_t)std::max(0, safeStoi(a.substr(pos + 1)));
    g_engine->setClipStart(idx, clipStart);
    log("TIMELINE", "setClipStart track=" + std::to_string(idx) + " pos=" + std::to_string(clipStart));
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

static std::string nativeMoveClip(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos1 = a.find(',');
    if (pos1 == std::string::npos || !g_engine) return "null";
    auto pos2 = a.find(',', pos1 + 1);
    if (pos2 == std::string::npos) return "null";
    int src = safeStoi(a.substr(0, pos1));
    int dst = safeStoi(a.substr(pos1 + 1, pos2 - pos1 - 1));
    uint64_t clipStart = (uint64_t)std::max(0, safeStoi(a.substr(pos2 + 1)));
    if (src == dst) {
        g_engine->setClipStart(src, clipStart);
    } else {
        g_engine->moveTrackAudio(src, dst);
        g_engine->setClipStart(dst, clipStart);
    }
    log("TIMELINE", "moveClip src=" + std::to_string(src) + " dst=" + std::to_string(dst) + " pos=" + std::to_string(clipStart));
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
    AudioBuffer* buf = t ? t->audio.load(std::memory_order_relaxed) : nullptr;
    if (!buf || buf->frames == 0) {
        log("WAV", "getPeakCache idx=" + std::to_string(idx) + " no buffer or zero frames");
        return "[]";
    }
    if (buf->peakCache.empty()) {
        int numP = std::max(100, (int)(buf->frames / 600));
        buf->peakCache.resize(numP);
        for (int p = 0; p < numP; ++p) {
            uint64_t s = (uint64_t)p * buf->frames / numP;
            uint64_t e = (uint64_t)(p + 1) * buf->frames / numP;
            if (e > buf->frames) e = buf->frames;
            if (e <= s) e = s + 1;
            float pk = 0.0f;
            for (uint64_t i = s; i < e; ++i) {
                pk = std::max(pk, std::abs(buf->dataL[i]));
                pk = std::max(pk, std::abs(buf->dataR[i]));
            }
            buf->peakCache[p] = pk;
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

static std::string nativePluginBypass(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p1 = a.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    int trackIdx = safeStoi(a.substr(0, p1));
    auto p2 = a.find(',', p1 + 1);
    int pluginIdx = safeStoi(a.substr(p1 + 1, p2 - p1 - 1));
    bool bp = (a.substr(p2 + 1) == "1");
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (chain) {
        chain->setBypass(pluginIdx, bp);
        log("PLUGIN", "bypass track=" + std::to_string(trackIdx) +
            " plugin=" + std::to_string(pluginIdx) + " bp=" + (bp ? "1" : "0"));
    }
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return "null";
}

// Forward declaration for GUI cleanup in nativePluginRemove
static PluginGuiState* findPluginGui(int trackIdx, int pluginIdx);

static std::string nativePluginRemove(const std::string& req) {
    std::string a = unwrapArg(req);
    auto pos = a.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int trackIdx = safeStoi(a.substr(0, pos));
    int pluginIdx = safeStoi(a.substr(pos + 1));
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (chain) {
        // Close GUI if open before removing the plugin
        auto* gs = findPluginGui(trackIdx, pluginIdx);
        if (gs) {
            if (gs->gui->hide) gs->gui->hide(gs->plugin);
            if (gs->gui->destroy) gs->gui->destroy(gs->plugin);
            if (gs->window) gtk_widget_destroy(gs->window);
            for (size_t i = 0; i < g_pluginGuis.size(); ++i) {
                if (g_pluginGuis[i].trackIdx == trackIdx && g_pluginGuis[i].pluginIdx == pluginIdx) {
                    g_pluginGuis.erase(g_pluginGuis.begin() + i); break;
                }
            }
        }
        chain->removePlugin(pluginIdx);
        log("PLUGIN", "remove track=" + std::to_string(trackIdx) +
            " plugin=" + std::to_string(pluginIdx));
    }
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
    g_uiDirty.fetch_or(kDirtyExtended, std::memory_order_release);
    return ok ? "true" : "false";
}

static std::string nativePageReady(const std::string&) {
    g_pageReady.store(true, std::memory_order_release);
    log("SYS", "pageReady signal received");
    return "null";
}

static std::string nativeScanClap(const std::string& req) {
    std::ostringstream js;
    js << "[";
    bool first = true;
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
                if (!first) js << ",";
                first = false;
                js << "{"
                   << "\"path\":\"" << escapeJson(libPath) << "\","
                   << "\"id\":\"" << escapeJson(desc->id) << "\","
                   << "\"name\":\"" << escapeJson(desc->name ? desc->name : desc->id) << "\""
                   << "}";
            }
            ClapHost::unloadLibrary(lib);
        }
    }
    js << "]";
    return js.str();
}

// ── PLUGIN GUI ──


static PluginGuiState* findPluginGui(int trackIdx, int pluginIdx) {
    for (auto& g : g_pluginGuis)
        if (g.trackIdx == trackIdx && g.pluginIdx == pluginIdx) return &g;
    return nullptr;
}

static bool clap_host_request_resize(const clap_host_t* host, uint32_t width, uint32_t height) {
    intptr_t id = (intptr_t)host->host_data;
    int trackIdx = (int)(id >> 16);
    int pluginIdx = (int)(id & 0xFFFF);
    auto* gs = findPluginGui(trackIdx, pluginIdx);
    if (!gs || !gs->window) return false;
    gtk_window_resize(GTK_WINDOW(gs->window), (int)width, (int)height);
    return true;
}

static void clap_host_gui_resize_hints_changed(const clap_host_t* host) {}
static bool clap_host_request_show(const clap_host_t* host) { return false; }
static bool clap_host_request_hide(const clap_host_t* host) { return false; }
static void clap_host_gui_closed(const clap_host_t* host, bool was_destroyed) {}

extern const clap_host_gui_t s_hostGui = {
    clap_host_gui_resize_hints_changed,
    clap_host_request_resize,
    clap_host_request_show,
    clap_host_request_hide,
    clap_host_gui_closed
};

static std::string nativePluginShowGUI(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "false";
    int trackIdx = safeStoi(a.substr(0, p));
    int pluginIdx = safeStoi(a.substr(p + 1));
    if (findPluginGui(trackIdx, pluginIdx)) {
        // Already open — close it (toggle off)
        auto* gs = findPluginGui(trackIdx, pluginIdx);
        if (gs->plugin && gs->gui->hide) gs->gui->hide(gs->plugin);
        if (gs->plugin && gs->gui->destroy) gs->gui->destroy(gs->plugin);
        if (gs->window) gtk_widget_destroy(gs->window);
        for (size_t i = 0; i < g_pluginGuis.size(); ++i) {
            if (g_pluginGuis[i].trackIdx == trackIdx && g_pluginGuis[i].pluginIdx == pluginIdx) {
                g_pluginGuis.erase(g_pluginGuis.begin() + i); break;
            }
        }
        log("PLUGIN", "GUI closed track " + std::to_string(trackIdx) + " plugin " + std::to_string(pluginIdx));
        return "null";
    }

    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (!chain) return "false";

    const clap_plugin_t* plugin = chain->getPlugin(pluginIdx);
    if (!plugin) return "false";

    const clap_plugin_gui_t* gui = (const clap_plugin_gui_t*)
        plugin->get_extension(plugin, CLAP_EXT_GUI);
    if (!gui) return "false";
    if (!gui->is_api_supported || !gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false))
        return "false";

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget* glArea = gtk_gl_area_new();
    gtk_container_add(GTK_CONTAINER(win), glArea);
    gtk_widget_realize(glArea);
    gtk_container_remove(GTK_CONTAINER(win), glArea);

    const char* title = plugin->desc && plugin->desc->name ? plugin->desc->name : "Plugin";
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 600);
    gtk_widget_realize(win);

    GdkWindow* gdkWin = gtk_widget_get_window(win);
    if (!gdkWin || !GDK_IS_X11_WINDOW(gdkWin)) { gtk_widget_destroy(win); return "false"; }
    clap_xwnd xid = GDK_WINDOW_XID(gdkWin);

    // Connect destroy signal (packed track/plugin index as user data)
    void* signalData = (void*)(intptr_t)((trackIdx << 16) | pluginIdx);

    // Create plugin GUI
    if (!gui->create || !gui->create(plugin, CLAP_WINDOW_API_X11, false)) { gtk_widget_destroy(win); return "false"; }
    if (gui->set_scale) gui->set_scale(plugin, 1.0);
    uint32_t gw = 800, gh = 600;
    if (gui->get_size) gui->get_size(plugin, &gw, &gh);
    gtk_window_resize(GTK_WINDOW(win), (int)gw, (int)gh);

    clap_window_t cw;
    cw.api = CLAP_WINDOW_API_X11;
    cw.x11 = xid;
    if (!gui->set_parent(plugin, &cw)) { gui->destroy(plugin); gtk_widget_destroy(win); return "false"; }
    if (!gui->show(plugin)) { gui->destroy(plugin); gtk_widget_destroy(win); return "false"; }

    // Push state BEFORE connecting destroy signal so findPluginGui finds it
    g_pluginGuis.push_back({trackIdx, pluginIdx, win, 0, nullptr, plugin, gui});
    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        intptr_t id = (intptr_t)data;
        int t = (int)(id >> 16);
        int p = (int)(id & 0xFFFF);
        auto* gs = findPluginGui(t, p);
        if (!gs) return;
        if (gs->gui->hide) gs->gui->hide(gs->plugin);
        if (gs->gui->destroy) gs->gui->destroy(gs->plugin);
        for (size_t i = 0; i < g_pluginGuis.size(); ++i) {
            if (g_pluginGuis[i].trackIdx == t && g_pluginGuis[i].pluginIdx == p) {
                g_pluginGuis.erase(g_pluginGuis.begin() + i); break;
            }
        }
    }), signalData);

    gtk_widget_show(win);
    log("PLUGIN", "GUI shown track " + std::to_string(trackIdx) + " plugin " + std::to_string(pluginIdx));
    return "true";
}

static std::string nativePluginHideGUI(const std::string& req) {
    std::string a = unwrapArg(req);
    auto p = a.find(',');
    if (p == std::string::npos || !g_engine) return "false";
    int trackIdx = safeStoi(a.substr(0, p));
    int pluginIdx = safeStoi(a.substr(p + 1));
    auto* gs = findPluginGui(trackIdx, pluginIdx);
    if (!gs) return "false";
    if (gs->plugin && gs->gui->hide) gs->gui->hide(gs->plugin);
    if (gs->plugin && gs->gui->destroy) gs->gui->destroy(gs->plugin);
    if (gs->window) gtk_widget_destroy(gs->window);
    if (gs->x11dpy && gs->x11win) {
        XDestroyWindow(gs->x11dpy, gs->x11win);
        XCloseDisplay(gs->x11dpy);
    }
    // Remove from vector
    for (size_t i = 0; i < g_pluginGuis.size(); ++i) {
        if (g_pluginGuis[i].trackIdx == trackIdx && g_pluginGuis[i].pluginIdx == pluginIdx) {
            g_pluginGuis.erase(g_pluginGuis.begin() + i);
            break;
        }
    }
    log("PLUGIN", "GUI hidden for track " + std::to_string(trackIdx) + " plugin " + std::to_string(pluginIdx));
    return "null";
}

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
    void* buf[32];
    int n = backtrace(buf, 32);
    fprintf(stderr, "SIGNAL %d caught! Backtrace:\n", sig);
    char** symbols = backtrace_symbols(buf, n);
    for (int i = 0; i < n; ++i)
        fprintf(stderr, "  %s\n", symbols[i]);
    free(symbols);
    _exit(1);
}

int main() {
    signal(SIGSEGV, crashHandler);
    initLogger();
    setenv("GDK_BACKEND", "x11", 0); // Force X11 for CLAP plugin GUI embedding
    log("SYS", "=== Hydraw DAW v" HYDRAW_VERSION " starting ===");

    AudioEngine audioEngine;
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
    w.bind("nativeAddTrack", nativeAddTrack);
    w.bind("nativeRemoveTrack", nativeRemoveTrack);
    w.bind("nativeSetVolume", nativeSetVolume);
    w.bind("nativeSetMasterVolume", nativeSetMasterVolume);
    w.bind("nativeSetClipStart", nativeSetClipStart);
    w.bind("nativeMoveClip", nativeMoveClip);
    w.bind("nativeSetPan", nativeSetPan);
    w.bind("nativeSetMute", nativeSetMute);
    w.bind("nativeSetSolo", nativeSetSolo);
    w.bind("nativeSetArm", nativeSetArm);
    w.bind("nativeLoadWav", nativeLoadWav);
    w.bind("nativeGetPeakCache", nativeGetPeakCache);
    w.bind("nativeListDir", nativeListDir);
    w.bind("nativeGetCwd", nativeGetCwd);
    w.bind("nativeGetAudioInfo", nativeGetAudioInfo);
    w.bind("nativePluginList", nativePluginList);
    w.bind("nativePluginBypass", nativePluginBypass);
    w.bind("nativePluginRemove", nativePluginRemove);
    w.bind("nativePluginLoad", nativePluginLoad);
    w.bind("nativeScanClap", nativeScanClap);
    w.bind("nativePluginShowGUI", nativePluginShowGUI);
    w.bind("nativePluginHideGUI", nativePluginHideGUI);
    w.bind("nativePageReady", nativePageReady);
    w.bind("nativeLog", nativeLog);  // JS→C++ log relay
    w.bind("nativeShowSaveDialog", nativeShowSaveDialog);
    w.bind("nativeShowOpenDialog", nativeShowOpenDialog);
    w.bind("nativeSaveProject", nativeSaveProject);
    w.bind("nativeLoadProject", nativeLoadProject);
    w.bind("nativeExportAudio", nativeExportAudio);

    log("SYS", "All 31 native bindings registered");

    w.init(
        "window.updateUIFromNative=function(){};"
        "window.onNativeUpdate=function(){};"
        "window.addEventListener('DOMContentLoaded',function(){window.nativePageReady('')});"
    );

    std::thread uiThread(uiUpdateLoop);

    std::filesystem::path htmlPath = std::filesystem::current_path() / "assets" / "index.html";
    if (std::filesystem::exists(htmlPath)) {
        w.navigate(("file://" + htmlPath.string()).c_str());
        log("SYS", "Navigated to " + htmlPath.string());
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
    g_running.store(false);
    uiThread.join();
    audioEngine.stop();
    audioEngine.shutdown();
    g_wv = nullptr;
    g_engine = nullptr;

    log("SYS", "=== Hydraw DAW shutdown complete ===");
    closeLogger();
    return 0;
}
