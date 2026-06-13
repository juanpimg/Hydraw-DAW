#include "AudioEngine.h"
#include "Plugin/PluginChain.h"
#include <webview/webview.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

static AudioEngine* g_engine = nullptr;
static webview::webview* g_wv = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_pageReady{false};

static int safeStoi(const std::string& s, int def = 0) {
    try { return std::stoi(s); } catch (...) { return def; }
}
static float safeStof(const std::string& s, float def = 0.0f) {
    try { return std::stof(s); } catch (...) { return def; }
}

static void uiUpdateLoop() {
    using namespace std::chrono_literals;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(30ms);
        if (!g_engine || !g_wv || !g_pageReady.load(std::memory_order_acquire)) continue;

        uint64_t ph = g_engine->getPlayhead();
        int tc = g_engine->getTrackCount();
        bool playing = g_engine->isPlaying();

        std::ostringstream js;
        js << "updateUIFromNative({"
           << "playing:" << (playing ? "true" : "false") << ","
           << "playhead:" << ph << ","
           << "trackCount:" << tc << ","
           << "trackPeaks:[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            float peak = std::max(
                g_engine->getTrack(i)->peakL.load(std::memory_order_relaxed),
                g_engine->getTrack(i)->peakR.load(std::memory_order_relaxed));
            js << peak;
        }
        js << "],";
        js << "volumes:[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << g_engine->getTrack(i)->volume.load(std::memory_order_relaxed);
        }
        js << "],";
        js << "mutes:[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (g_engine->getTrack(i)->muted.load(std::memory_order_relaxed) ? "true" : "false");
        }
        js << "],";
        js << "solos:[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (g_engine->getTrack(i)->soloed.load(std::memory_order_relaxed) ? "true" : "false");
        }
        js << "],";
        js << "arms:[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
            js << (g_engine->getTrack(i)->armed.load(std::memory_order_relaxed) ? "true" : "false");
        }
        js << "],";
        js << "trackNames:[";
        for (int i = 0; i < tc; ++i) {
            if (i > 0) js << ",";
        js << "\"" << g_engine->getTrack(i)->name << "\"";
    }
    js << "],";
    js << "audioFrames:[";
    for (int i = 0; i < tc; ++i) {
        if (i > 0) js << ",";
        AudioBuffer* buf = g_engine->getTrack(i)->audio.load(std::memory_order_relaxed);
        js << (buf ? (int64_t)buf->frames : 0);
    }
    js << "],";
    js << "clipStarts:[";
    for (int i = 0; i < tc; ++i) {
        if (i > 0) js << ",";
        js << (int64_t)g_engine->getTrack(i)->clipStart.load(std::memory_order_relaxed);
    }
    js << "],";
    js << "peakCaches:[";
    for (int i = 0; i < tc; ++i) {
        if (i > 0) js << ",";
        AudioBuffer* buf = g_engine->getTrack(i)->audio.load(std::memory_order_relaxed);
        if (buf && !buf->peakCache.empty()) {
            js << "[";
            for (size_t j = 0; j < buf->peakCache.size(); ++j) {
                if (j > 0) js << ",";
                js << buf->peakCache[j];
            }
            js << "]";
        } else {
            js << "[]";
        }
    }
    js << "]})";

        g_wv->eval(js.str());
    }
}

static std::string nativePlay(const std::string&)    { if (g_engine) g_engine->play(); return "null"; }
static std::string nativePause(const std::string&)   { if (g_engine) g_engine->pause(); return "null"; }
static std::string nativeStop(const std::string&)    { if (g_engine) g_engine->stopTransport(); return "null"; }

static std::string nativeAddTrack(const std::string&) {
    if (g_engine) { int n = g_engine->addTrack(); return std::to_string(n); }
    return "-1";
}

static std::string nativeRemoveTrack(const std::string& req) {
    int idx = safeStoi(req, -1);
    if (idx >= 0 && g_engine) g_engine->removeTrack(idx);
    return "null";
}

static std::string nativeSetVolume(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(req.substr(0, pos));
    float val = safeStof(req.substr(pos + 1), 1.0f);
    Track* t = g_engine->getTrack(idx);
    if (t) t->volume.store(val, std::memory_order_relaxed);
    return "null";
}

static std::string nativeSetPan(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(req.substr(0, pos));
    float val = safeStof(req.substr(pos + 1));
    Track* t = g_engine->getTrack(idx);
    if (t) t->pan.store(val, std::memory_order_relaxed);
    return "null";
}

static std::string nativeSetMute(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(req.substr(0, pos));
    bool val = (req.substr(pos + 1) == "1");
    Track* t = g_engine->getTrack(idx);
    if (t) t->muted.store(val, std::memory_order_relaxed);
    return "null";
}

static std::string nativeSetSolo(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(req.substr(0, pos));
    bool val = (req.substr(pos + 1) == "1");
    Track* t = g_engine->getTrack(idx);
    if (t) t->soloed.store(val, std::memory_order_relaxed);
    return "null";
}

static std::string nativeSetArm(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int idx = safeStoi(req.substr(0, pos));
    bool val = (req.substr(pos + 1) == "1");
    Track* t = g_engine->getTrack(idx);
    if (t) t->armed.store(val, std::memory_order_relaxed);
    return "null";
}

static std::string nativeLoadWav(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "false";
    int idx = safeStoi(req.substr(0, pos));
    std::string path = req.substr(pos + 1);
    if (path.empty()) return "false";
    bool ok = g_engine->loadWavToTrack(idx, path.c_str());
    if (ok) {
        std::filesystem::path p(path);
        g_engine->getTrack(idx)->name = p.stem().string();
    }
    return ok ? "true" : "false";
}

static std::string nativeGetPeakCache(const std::string& req) {
    if (!g_engine) return "[]";
    int idx = safeStoi(req);
    Track* t = g_engine->getTrack(idx);
    AudioBuffer* buf = t ? t->audio.load(std::memory_order_relaxed) : nullptr;
    if (!buf || buf->peakCache.empty()) return "[]";
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < buf->peakCache.size(); ++i) {
        if (i > 0) js << ",";
        js << buf->peakCache[i];
    }
    js << "]";
    return js.str();
}

static std::string escapeJson(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        if (c == '"' || c == '\\') o << '\\';
        o << c;
    }
    return o.str();
}

static std::string nativeListDir(const std::string& req) {
    std::ostringstream js;
    js << "[";
    bool first = true;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(req, ec)) {
        if (!first) js << ",";
        first = false;
        bool isDir = entry.is_directory();
        std::string name = entry.path().filename().string();
        std::string ext = entry.path().extension().string();
        js << "{"
           << "\"name\":\"" << escapeJson(name) << "\","
           << "\"dir\":" << (isDir ? "true" : "false") << ","
           << "\"ext\":\"" << ext << "\""
           << "}";
    }
    js << "]";
    return js.str();
}

static std::string nativeGetCwd(const std::string&) {
    return "\"" + escapeJson(std::filesystem::current_path().string()) + "\"";
}

static std::string nativeGetAudioInfo(const std::string& req) {
    if (!g_engine) return "{\"hasAudio\":false}";
    int idx = safeStoi(req);
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
    int idx = safeStoi(req);
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
    auto p1 = req.find(',');
    if (p1 == std::string::npos || !g_engine) return "null";
    int trackIdx = safeStoi(req.substr(0, p1));
    auto p2 = req.find(',', p1 + 1);
    int pluginIdx = safeStoi(req.substr(p1 + 1, p2 - p1 - 1));
    bool bp = (req.substr(p2 + 1) == "1");
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (chain) chain->setBypass(pluginIdx, bp);
    return "null";
}

static std::string nativePluginRemove(const std::string& req) {
    auto pos = req.find(',');
    if (pos == std::string::npos || !g_engine) return "null";
    int trackIdx = safeStoi(req.substr(0, pos));
    int pluginIdx = safeStoi(req.substr(pos + 1));
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (chain) chain->removePlugin(pluginIdx);
    return "null";
}

static std::string nativePluginLoad(const std::string& req) {
    auto p1 = req.find(',');
    if (p1 == std::string::npos || !g_engine) return "false";
    int trackIdx = safeStoi(req.substr(0, p1));
    auto p2 = req.find(',', p1 + 1);
    if (p2 == std::string::npos) return "false";
    std::string libPath = req.substr(p1 + 1, p2 - p1 - 1);
    std::string pluginId = req.substr(p2 + 1);
    PluginChain* chain = g_engine->getPluginChain(trackIdx);
    if (!chain) return "false";
    bool ok = chain->addPlugin(libPath.c_str(), pluginId.c_str());
    return ok ? "true" : "false";
}

static std::string nativePageReady(const std::string&) {
    g_pageReady.store(true, std::memory_order_release);
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
            if (entry.path().extension() == ".clap") {
                if (!first) js << ",";
                first = false;
                js << "\"" << escapeJson(entry.path().string()) << "\"";
            }
        }
    }
    js << "]";
    return js.str();
}

int main() {
    AudioEngine audioEngine;
    if (!audioEngine.init()) {
        fprintf(stderr, "Failed to initialize audio engine\n");
        return 1;
    }
    audioEngine.start();
    g_engine = &audioEngine;

    webview::webview w(true, nullptr);
    g_wv = &w;
    w.set_title("Hydraw DAW");
    w.set_size(1280, 720, WEBVIEW_HINT_NONE);

    w.bind("nativePlay", nativePlay);
    w.bind("nativePause", nativePause);
    w.bind("nativeStop", nativeStop);
    w.bind("nativeAddTrack", nativeAddTrack);
    w.bind("nativeRemoveTrack", nativeRemoveTrack);
    w.bind("nativeSetVolume", nativeSetVolume);
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
    w.bind("nativePageReady", nativePageReady);

    w.init(
        "window.updateUIFromNative=function(){};"
        "window.onNativeUpdate=function(){};"
        "window.addEventListener('DOMContentLoaded',function(){window.nativePageReady('')});"
    );

    std::thread uiThread(uiUpdateLoop);

    std::filesystem::path htmlPath = std::filesystem::current_path() / "assets" / "index.html";
    if (std::filesystem::exists(htmlPath)) {
        w.navigate(("file://" + htmlPath.string()).c_str());
    } else {
        fprintf(stderr, "Could not find %s\n", htmlPath.string().c_str());
        g_running.store(false);
        uiThread.join();
        audioEngine.shutdown();
        return 1;
    }

    w.run();

    g_running.store(false);
    uiThread.join();
    audioEngine.stop();
    audioEngine.shutdown();
    g_wv = nullptr;
    g_engine = nullptr;

    return 0;
}
