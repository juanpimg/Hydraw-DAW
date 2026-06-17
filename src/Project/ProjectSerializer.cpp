#include "ProjectSerializer.h"
#include "Core/Constants.h"
#include "Plugin/PluginChain.h"
#include <cstdio>
#include <sstream>
#include <vector>
#include <fstream>
#include <algorithm>

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

static std::string unescapeJson(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i+1]) {
                case '"': r += '"'; ++i; break;
                case '\\': r += '\\'; ++i; break;
                case 'n': r += '\n'; ++i; break;
                case 'r': r += '\r'; ++i; break;
                case 't': r += '\t'; ++i; break;
                default: r += s[i]; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

// Simple JSON string value extraction: finds "key": "value" pattern
static std::string extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    std::string val;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '"') break;
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            val += json[pos];
            val += json[pos+1];
            ++pos;
        } else {
            val += json[pos];
        }
    }
    return unescapeJson(val);
}

static int extractInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) ++pos;
    int sign = 1;
    if (pos < json.size() && json[pos] == '-') { sign = -1; ++pos; }
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val * sign;
}

static float extractFloat(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0f;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) ++pos;
    try {
        size_t end;
        return std::stof(json.c_str() + pos, &end);
    } catch (...) {
        return 0.0f;
    }
}

static bool extractBool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) ++pos;
    return json.substr(pos, 4) == "true";
}

static std::string trim(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) ++start;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\n' || s[end-1] == '\r')) --end;
    return s.substr(start, end - start);
}

// Extract the array portion between [ and ] with matching braces
static std::string extractArray(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) ++pos;
    if (pos >= json.size() || json[pos] != '[') return "";
    int depth = 1;
    size_t start = pos;
    ++pos;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '[') ++depth;
        else if (json[pos] == ']') --depth;
        else if (json[pos] == '"') {
            ++pos;
            while (pos < json.size() && !(json[pos] == '"' && json[pos-1] != '\\')) ++pos;
        }
        ++pos;
    }
    return json.substr(start, pos - start);
}

// Split a JSON array of objects: [{...},{...}]
static std::vector<std::string> splitObjects(const std::string& arr) {
    std::vector<std::string> objs;
    size_t i = 0;
    // skip leading [
    if (i < arr.size() && arr[i] == '[') ++i;
    while (i < arr.size()) {
        while (i < arr.size() && (arr[i] == ' ' || arr[i] == '\t' || arr[i] == '\n' || arr[i] == '\r' || arr[i] == ',')) ++i;
        if (i >= arr.size() || arr[i] == ']') break;
        if (arr[i] == '{') {
            int depth = 1;
            size_t start = i;
            ++i;
            while (i < arr.size() && depth > 0) {
                if (arr[i] == '{') ++depth;
                else if (arr[i] == '}') --depth;
                else if (arr[i] == '"') {
                    ++i;
                    while (i < arr.size() && !(arr[i] == '"' && arr[i-1] != '\\')) ++i;
                }
                ++i;
            }
            objs.push_back(arr.substr(start, i - start));
        } else {
            ++i;
        }
    }
    return objs;
}

bool ProjectSerializer::save(const char* path, AudioEngine* engine) {
    std::ostringstream json;
    json.imbue(std::locale::classic());

    json << "{\n";
    json << "  \"version\": 1,\n";
    json << "  \"masterVolume\": " << engine->getMaster()->volume.load(std::memory_order_relaxed) << ",\n";
    json << "  \"playhead\": " << engine->getPlayhead() << ",\n";

    // Master plugin chain (trackIndex = -1). Same shape as a track's
    // "plugins" array. Plugins write their state to a UTF-8 XML blob
    // via the CLAP state extension; we embed it as a JSON string.
    json << "  \"master\": {\n";
    json << "    \"plugins\": [\n";
    {
        PluginChain* mchain = engine->getPluginChain(-1);
        int mpc = mchain ? mchain->getPluginCount() : 0;
        for (int p = 0; p < mpc; ++p) {
            json << "      {\n";
            json << "        \"path\": \"" << ::escapeJson(mchain->getPluginPath(p)) << "\",\n";
            json << "        \"id\": \"" << ::escapeJson(mchain->getPluginId(p)) << "\",\n";
            json << "        \"bypassed\": " << (mchain->isBypassed(p) ? "true" : "false");
            std::string st = mchain->saveState(p);
            if (!st.empty()) {
                json << ",\n        \"state\": \"" << ::escapeJson(st) << "\"\n";
            } else {
                json << "\n";
            }
            json << "      }" << (p < mpc - 1 ? "," : "") << "\n";
        }
    }
    json << "    ]\n";
    json << "  },\n";

    int tc = engine->getTrackCount();
    json << "  \"tracks\": [\n";
    for (int i = 0; i < tc; ++i) {
        Track* t = engine->getTrack(i);
        AudioBuffer* buf = t->audio.load(std::memory_order_relaxed);
        json << "    {\n";
        json << "      \"name\": \"" << ::escapeJson(t->name) << "\",\n";
        json << "      \"volume\": " << t->volume.load(std::memory_order_relaxed) << ",\n";
        json << "      \"pan\": " << t->pan.load(std::memory_order_relaxed) << ",\n";
        json << "      \"muted\": " << (t->muted.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
        json << "      \"soloed\": " << (t->soloed.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
        json << "      \"armed\": " << (t->armed.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
        json << "      \"clipStart\": " << t->clipStart.load(std::memory_order_relaxed) << ",\n";
        json << "      \"audioPath\": \"" << ::escapeJson(t->audioFilePath) << "\",\n";
        json << "      \"audioFrames\": " << (buf ? (int64_t)buf->frames : 0) << ",\n";

        // Plugin chain
        json << "      \"plugins\": [\n";
        PluginChain* chain = engine->getPluginChain(i);
        int pc = chain ? chain->getPluginCount() : 0;
        for (int p = 0; p < pc; ++p) {
            json << "        {\n";
            json << "          \"path\": \"" << ::escapeJson(chain->getPluginPath(p)) << "\",\n";
            json << "          \"id\": \"" << ::escapeJson(chain->getPluginId(p)) << "\",\n";
            json << "          \"bypassed\": " << (chain->isBypassed(p) ? "true" : "false");
            std::string st = chain->saveState(p);
            if (!st.empty()) {
                json << ",\n          \"state\": \"" << ::escapeJson(st) << "\"\n";
            } else {
                json << "\n";
            }
            json << "        }" << (p < pc - 1 ? "," : "") << "\n";
        }
        json << "      ]\n";
        json << "    }" << (i < tc - 1 ? "," : "") << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    // Atomic write: write to .tmp then rename. Prevents project corruption
    // if the app crashes or loses power mid-write.
    std::string tmpPath = std::string(path) + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "w");
    if (!f) return false;
    std::string out = json.str();
    bool writeOk = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!writeOk) {
        std::remove(tmpPath.c_str());
        return false;
    }
    if (std::rename(tmpPath.c_str(), path) != 0) {
        std::remove(tmpPath.c_str());
        return false;
    }
    return true;
}

bool ProjectSerializer::load(const char* path, AudioEngine* engine) {
    std::string json = readFile(path);
    if (json.empty()) return false;

    // Extract masterVolume
    float masterVol = extractFloat(json, "masterVolume");
    engine->getMaster()->volume.store(masterVol, std::memory_order_relaxed);

    // Extract playhead
    uint64_t playhead = (uint64_t)std::max(0, extractInt(json, "playhead"));
    engine->setPlayhead(playhead);

    // Extract tracks array
    std::string tracksArr = extractArray(json, "tracks");
    if (tracksArr.empty()) return false;

    auto trackObjects = splitObjects(tracksArr);

    // Clear existing tracks down to 1, then add as needed
    int currentCount = engine->getTrackCount();
    for (int i = currentCount - 1; i > 0; --i)
        engine->removeTrack(i);
    // Track 0 remains, we'll configure it

    // Get rid of track 0 audio using deferred delete (safe against
    // audio thread holding a local copy from the last block).
    {
        Track* t0 = engine->getTrack(0);
        AudioBuffer* old = t0->audio.exchange(nullptr, std::memory_order_release);
        if (old) engine->queueAudioBufferDelete(old);
        t0->name = "";
        t0->audioFilePath = "";
        t0->volume.store(1.0f, std::memory_order_relaxed);
        t0->pan.store(0.0f, std::memory_order_relaxed);
        t0->muted.store(false, std::memory_order_relaxed);
        t0->soloed.store(false, std::memory_order_relaxed);
        t0->armed.store(false, std::memory_order_relaxed);
        t0->clipStart.store(0, std::memory_order_relaxed);
    }

    // Restore master chain BEFORE tracks. First, remove ALL existing
    // master plugins (e.g. the preloaded PeakEater from init()), then
    // add the plugins saved in the project file. This prevents the old
    // append-bug that duplicated plugins on every save→load cycle.
    {
        PluginChain* mchain = engine->getPluginChain(-1);
        if (mchain) {
            while (mchain->getPluginCount() > 0)
                mchain->removePlugin(0);
        }
        // Parse the "master" object and its "plugins" array
        std::string masterKey = "\"master\":";
        auto mp = json.find(masterKey);
        if (mp != std::string::npos) {
            auto brace = json.find('{', mp + masterKey.size());
            if (brace != std::string::npos) {
                int depth = 1;
                size_t end = brace + 1;
                while (end < json.size() && depth > 0) {
                    if (json[end] == '{') ++depth;
                    else if (json[end] == '}') --depth;
                    else if (json[end] == '"') {
                        ++end;
                        while (end < json.size() && !(json[end] == '"' && json[end-1] != '\\')) ++end;
                    }
                    ++end;
                }
                std::string masterObj = json.substr(brace, end - brace);
                std::string mpArr = extractArray(masterObj, "plugins");
                if (!mpArr.empty()) {
                    auto pluginObjects = splitObjects(mpArr);
                    if (mchain) {
                        for (size_t p = 0; p < pluginObjects.size(); ++p) {
                            std::string plPath = extractString(pluginObjects[p], "path");
                            std::string plId = extractString(pluginObjects[p], "id");
                            bool bypassed = extractBool(pluginObjects[p], "bypassed");
                            std::string plState = extractString(pluginObjects[p], "state");
                            if (!plPath.empty() && !plId.empty()) {
                                bool ok = mchain->addPlugin(plPath.c_str(), plId.c_str());
                                if (ok) {
                                    int idx = mchain->getPluginCount() - 1;
                                    if (bypassed) mchain->setBypass(idx, true);
                                    if (!plState.empty()) mchain->loadState(idx, plState);
                                } else {
                                    fprintf(stderr, "[LOAD] WARN: failed to restore master plugin %s (%s)\n",
                                            plPath.c_str(), plId.c_str());
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Add tracks to match saved count
    for (size_t i = 1; i < trackObjects.size(); ++i)
        engine->addTrack();

    int tc = engine->getTrackCount();
    int toConfigure = std::min(tc, (int)trackObjects.size());

    for (int i = 0; i < toConfigure; ++i) {
        Track* t = engine->getTrack(i);
        const std::string& obj = trackObjects[i];

        t->name = extractString(obj, "name");
        t->audioFilePath = extractString(obj, "audioPath");
        t->volume.store(extractFloat(obj, "volume"), std::memory_order_relaxed);
        t->pan.store(extractFloat(obj, "pan"), std::memory_order_relaxed);
        t->muted.store(extractBool(obj, "muted"), std::memory_order_relaxed);
        t->soloed.store(extractBool(obj, "soloed"), std::memory_order_relaxed);
        t->armed.store(extractBool(obj, "armed"), std::memory_order_relaxed);
        t->clipStart.store((uint64_t)std::max(0, extractInt(obj, "clipStart")), std::memory_order_relaxed);

        // Reload audio if path exists. If the file has been moved or
        // deleted since the project was saved, the track stays silent
        // (no crash) and we log a warning.
        if (!t->audioFilePath.empty()) {
            bool loaded = engine->loadWavToTrack(i, t->audioFilePath.c_str());
            if (!loaded) {
                fprintf(stderr, "[LOAD] WARN: audio file not found: %s (track %d will be silent)\n",
                        t->audioFilePath.c_str(), i);
            }
        }

        // Restore plugin chain
        std::string pluginsArr = extractArray(obj, "plugins");
        if (!pluginsArr.empty()) {
            auto pluginObjects = splitObjects(pluginsArr);
            PluginChain* chain = engine->getPluginChain(i);
            int baseIdx = chain ? chain->getPluginCount() : 0;
            for (size_t p = 0; p < pluginObjects.size(); ++p) {
                std::string plPath = extractString(pluginObjects[p], "path");
                std::string plId = extractString(pluginObjects[p], "id");
                bool bypassed = extractBool(pluginObjects[p], "bypassed");
                std::string plState = extractString(pluginObjects[p], "state");
                if (!plPath.empty() && !plId.empty()) {
                    bool ok = chain->addPlugin(plPath.c_str(), plId.c_str());
                    if (ok) {
                        if (bypassed) chain->setBypass(baseIdx, true);
                        if (!plState.empty()) chain->loadState(baseIdx, plState);
                        ++baseIdx;
                    } else {
                        fprintf(stderr, "[LOAD] WARN: failed to restore plugin %s (%s) on track %d\n",
                                plPath.c_str(), plId.c_str(), i);
                    }
                }
            }
        }
    }

    return true;
}

std::string ProjectSerializer::readFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string r;
    r.resize(size);
    size_t got = fread(&r[0], 1, size, f);
    (void)got;
    fclose(f);
    return r;
}
