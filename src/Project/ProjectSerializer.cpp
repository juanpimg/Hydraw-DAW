#include "ProjectSerializer.h"
#include "Core/Constants.h"
#include "Plugin/PluginChain.h"
#include "Util/Base64.h"
#include "Util/PathUtils.h"
#include <cstdio>
#include <vector>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

bool ProjectSerializer::save(const char* path, AudioEngine* engine) {
    json j;
    j["version"] = 1;
    j["masterVolume"] = engine->getMaster()->volume.load(std::memory_order_relaxed);
    j["playhead"] = engine->getPlayhead();
    // Aux buses volumes
    json aux = json::array();
    for (int b = 0; b < engine->getAuxBusCount(); ++b) {
        AuxBus* ab = engine->getAuxBus(b);
        if (!ab) continue;
        json bj;
        bj["name"] = ab->name;
        bj["volume"] = ab->volume.load(std::memory_order_relaxed);
        aux.push_back(std::move(bj));
    }
    j["auxBuses"] = std::move(aux);
    j["bpm"] = engine->getBPM();
    j["timeSigNum"] = engine->getTimeSigNum();
    j["timeSigDen"] = engine->getTimeSigDen();
    j["loopEnabled"] = engine->getLoopEnabled();
    j["loopStart"] = (int64_t)engine->getLoopStart();
    j["loopEnd"] = (int64_t)engine->getLoopEnd();
    j["punchStart"] = (int64_t)engine->getPunchStart();
    j["punchEnd"] = (int64_t)engine->getPunchEnd();

    json masterJson;
    json masterPlugins = json::array();
    PluginChain* mchain = engine->getPluginChain(-1);
    if (mchain) {
        int mpc = mchain->getPluginCount();
        for (int p = 0; p < mpc; ++p) {
            json pj;
            pj["path"] = mchain->getPluginPath(p);
            pj["id"] = mchain->getPluginId(p);
            pj["bypassed"] = mchain->isBypassed(p);
            std::string st = mchain->saveState(p);
            if (!st.empty())
                pj["state"] = base64::encode(st);
            masterPlugins.push_back(std::move(pj));
        }
    }
    masterJson["plugins"] = std::move(masterPlugins);
    j["master"] = std::move(masterJson);

    json tracks = json::array();
    int tc = engine->getTrackCount();
    for (int i = 0; i < tc; ++i) {
        Track* t = engine->getTrack(i);
        AudioBuffer* buf = t->audio.load(std::memory_order_relaxed);
        json tj;
        tj["name"] = t->name;
        tj["volume"] = t->volume.load(std::memory_order_relaxed);
        tj["pan"] = t->pan.load(std::memory_order_relaxed);
        tj["muted"] = t->muted.load(std::memory_order_relaxed);
        tj["soloed"] = t->soloed.load(std::memory_order_relaxed);
        tj["armed"] = t->armed.load(std::memory_order_relaxed);
        tj["clipStart"] = t->clipStart.load(std::memory_order_relaxed);
        json sendsArr = json::array();
        for (int b = 0; b < MAX_BUSES; ++b) {
            sendsArr.push_back(t->sends[b].load(std::memory_order_relaxed));
        }
        tj["sends"] = std::move(sendsArr);
        tj["audioPath"] = pathutil::storeForm(pathutil::projectDir(path), t->audioFilePath);
        tj["audioFrames"] = buf ? (int64_t)buf->frames : 0;
        tj["fadeIn"] = (int64_t)t->primaryFadeIn.load(std::memory_order_relaxed);
        tj["fadeOut"] = (int64_t)t->primaryFadeOut.load(std::memory_order_relaxed);

        // Additional clips (multi-clip per track)
        json extraClips = json::array();
        int clipCount = engine->getClipCount(i);
        for (int c = 1; c < clipCount; ++c) {
            json cj;
            cj["audioPath"] = pathutil::storeForm(pathutil::projectDir(path), engine->getClipPath(i, c));
            cj["audioFrames"] = (int64_t)engine->getClipFrames(i, c);
            cj["clipStart"] = (int64_t)engine->getClipStart(i, c);
            cj["fadeIn"] = (int64_t)engine->getClipFadeIn(i, c);
            cj["fadeOut"] = (int64_t)engine->getClipFadeOut(i, c);
            extraClips.push_back(std::move(cj));
        }
        tj["extraClips"] = std::move(extraClips);

        json plugins = json::array();
        PluginChain* chain = engine->getPluginChain(i);
        int pc = chain ? chain->getPluginCount() : 0;
        for (int p = 0; p < pc; ++p) {
            json pj;
            pj["path"] = chain->getPluginPath(p);
            pj["id"] = chain->getPluginId(p);
            pj["bypassed"] = chain->isBypassed(p);
            std::string st = chain->saveState(p);
            if (!st.empty())
                pj["state"] = base64::encode(st);
            plugins.push_back(std::move(pj));
        }
        tj["plugins"] = std::move(plugins);
        tracks.push_back(std::move(tj));
    }
    j["tracks"] = std::move(tracks);

    std::string out = j.dump(2) + "\n";

    std::string tmpPath = std::string(path) + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "w");
    if (!f) return false;
    bool writeOk = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!writeOk) { std::remove(tmpPath.c_str()); return false; }
    if (std::rename(tmpPath.c_str(), path) != 0) {
        std::remove(tmpPath.c_str());
        return false;
    }
    return true;
}

bool ProjectSerializer::load(const char* path, AudioEngine* engine) {
    std::string content = readFile(path);
    if (content.empty()) return false;

    json j;
    try {
        j = json::parse(content);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "[LOAD] ERROR: JSON parse error: %s\n", e.what());
        return false;
    }

    if (!j.is_object()) {
        fprintf(stderr, "[LOAD] ERROR: root must be a JSON object\n");
        return false;
    }

    float masterVol = j.value("masterVolume", 1.0f);
    engine->getMaster()->volume.store(masterVol, std::memory_order_relaxed);

    uint64_t playhead = j.value("playhead", 0ULL);
    engine->setPlayhead(playhead);
    engine->setBPM(j.value("bpm", 120.0f));
    {
        int num = j.value("timeSigNum", 4);
        int den = j.value("timeSigDen", 4);
        engine->setTimeSignature(num, den);
    }
    engine->setLoopEnabled(j.value("loopEnabled", false));
    engine->setLoopRange((uint64_t)j.value("loopStart", (int64_t)0),
                          (uint64_t)j.value("loopEnd", (int64_t)0));
    engine->setPunchRange((uint64_t)j.value("punchStart", (int64_t)0),
                           (uint64_t)j.value("punchEnd", (int64_t)0));
    if (j.contains("auxBuses") && j["auxBuses"].is_array()) {
        int idx = 0;
        for (auto& bj : j["auxBuses"]) {
            AuxBus* ab = engine->getAuxBus(idx);
            if (!ab) break;
            if (bj.is_object()) {
                ab->name = bj.value("name", ab->name);
                ab->volume.store(bj.value("volume", 1.0f), std::memory_order_relaxed);
            }
            ++idx;
        }
    }

    int currentCount = engine->getTrackCount();
    for (int i = currentCount - 1; i > 0; --i)
        engine->removeTrack(i);

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

    PluginChain* mchain = engine->getPluginChain(-1);
    if (mchain) {
        while (mchain->getPluginCount() > 0)
            mchain->removePlugin(0);
    }
    if (j.contains("master") && j["master"].is_object() && mchain) {
        auto& master = j["master"];
        if (master.contains("plugins") && master["plugins"].is_array()) {
            for (auto& pj : master["plugins"]) {
                if (!pj.is_object()) continue;
                std::string plPath = pj.value("path", "");
                std::string plId = pj.value("id", "");
                bool bypassed = pj.value("bypassed", false);
                std::string plStateB64 = pj.value("state", "");
                std::string plState = plStateB64.empty() ? "" : base64::decode(plStateB64);
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

    json tracks = j.value("tracks", json::array());
    if (!tracks.is_array()) tracks = json::array();
    size_t trackCount = tracks.size();

    for (size_t i = 1; i < trackCount; ++i)
        engine->addTrack();

    int tc = engine->getTrackCount();
    int toConfigure = std::min(tc, (int)trackCount);

    for (int i = 0; i < toConfigure; ++i) {
        auto& tj = tracks[i];
        if (!tj.is_object()) continue;
        Track* t = engine->getTrack(i);

        t->name = tj.value("name", "");
        t->audioFilePath = tj.value("audioPath", "");
        t->volume.store(tj.value("volume", 1.0f), std::memory_order_relaxed);
        t->pan.store(tj.value("pan", 0.0f), std::memory_order_relaxed);
        t->muted.store(tj.value("muted", false), std::memory_order_relaxed);
        t->soloed.store(tj.value("soloed", false), std::memory_order_relaxed);
        t->armed.store(tj.value("armed", false), std::memory_order_relaxed);

        if (!t->audioFilePath.empty()) {
            std::string resolved = pathutil::resolve(pathutil::projectDir(path), t->audioFilePath);
            bool loaded = engine->loadWavToTrack(i, resolved.c_str());
            if (!loaded) {
                fprintf(stderr, "[LOAD] WARN: audio file not found: %s (track %d will be silent)\n",
                        resolved.c_str(), i);
                t->audioFilePath.clear();
            } else {
                // Store the resolved absolute path so subsequent saves
                // can relativize it correctly.
                t->audioFilePath = resolved;
            }
        }

        t->clipStart.store((uint64_t)std::max((int64_t)0, tj.value("clipStart", (int64_t)0)), std::memory_order_relaxed);
        t->primaryFadeIn.store((uint64_t)std::max((int64_t)0, tj.value("fadeIn", (int64_t)0)), std::memory_order_relaxed);
        t->primaryFadeOut.store((uint64_t)std::max((int64_t)0, tj.value("fadeOut", (int64_t)0)), std::memory_order_relaxed);
        if (tj.contains("sends") && tj["sends"].is_array()) {
            int bi = 0;
            for (auto& sv : tj["sends"]) {
                if (bi >= MAX_BUSES) break;
                t->sends[bi].store(sv.get<float>(), std::memory_order_relaxed);
                ++bi;
            }
        }

        // Restore additional clips (extraClips array). Each entry is
        // loaded via addClip so the multi-clip machinery handles slot
        // allocation correctly.
        if (tj.contains("extraClips") && tj["extraClips"].is_array()) {
            for (auto& ec : tj["extraClips"]) {
                if (!ec.is_object()) continue;
                std::string ecPath = ec.value("audioPath", "");
                uint64_t ecStart = (uint64_t)std::max((int64_t)0, ec.value("clipStart", (int64_t)0));
                if (ecPath.empty()) continue;
                std::string resolved = pathutil::resolve(pathutil::projectDir(path), ecPath);
                int rc = engine->addClip(i, resolved.c_str(), ecStart);
                if (rc < 0) {
                    fprintf(stderr, "[LOAD] WARN: extra clip not found: %s (track %d will lose this clip)\n",
                            resolved.c_str(), i);
                } else {
                    uint64_t eFin = (uint64_t)std::max((int64_t)0, ec.value("fadeIn", (int64_t)0));
                    uint64_t eFout = (uint64_t)std::max((int64_t)0, ec.value("fadeOut", (int64_t)0));
                    engine->setClipFadeIn(i, rc, eFin);
                    engine->setClipFadeOut(i, rc, eFout);
                }
            }
        }

        if (tj.contains("plugins") && tj["plugins"].is_array()) {
            PluginChain* chain = engine->getPluginChain(i);
            int baseIdx = chain ? chain->getPluginCount() : 0;
            for (auto& pj : tj["plugins"]) {
                if (!pj.is_object()) continue;
                std::string plPath = pj.value("path", "");
                std::string plId = pj.value("id", "");
                bool bypassed = pj.value("bypassed", false);
                std::string plStateB64 = pj.value("state", "");
                std::string plState = plStateB64.empty() ? "" : base64::decode(plStateB64);
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

std::string ProjectSerializer::saveToString(AudioEngine* engine, const std::string& projectDir) {
    json j;
    j["version"] = 1;
    j["masterVolume"] = engine->getMaster()->volume.load(std::memory_order_relaxed);
    j["playhead"] = engine->getPlayhead();
    j["bpm"] = engine->getBPM();
    j["timeSigNum"] = engine->getTimeSigNum();
    j["timeSigDen"] = engine->getTimeSigDen();
    j["loopEnabled"] = engine->getLoopEnabled();
    j["loopStart"] = (int64_t)engine->getLoopStart();
    j["loopEnd"] = (int64_t)engine->getLoopEnd();
    j["punchStart"] = (int64_t)engine->getPunchStart();
    j["punchEnd"] = (int64_t)engine->getPunchEnd();
    json aux = json::array();
    for (int b = 0; b < engine->getAuxBusCount(); ++b) {
        AuxBus* ab = engine->getAuxBus(b);
        if (!ab) continue;
        json bj;
        bj["name"] = ab->name;
        bj["volume"] = ab->volume.load(std::memory_order_relaxed);
        aux.push_back(std::move(bj));
    }
    j["auxBuses"] = std::move(aux);
    json masterJson;
    json masterPlugins = json::array();
    PluginChain* mchain = engine->getPluginChain(-1);
    if (mchain) {
        int mpc = mchain->getPluginCount();
        for (int p = 0; p < mpc; ++p) {
            json pj;
            pj["path"] = mchain->getPluginPath(p);
            pj["id"] = mchain->getPluginId(p);
            pj["bypassed"] = mchain->isBypassed(p);
            std::string st = mchain->saveState(p);
            if (!st.empty()) pj["state"] = base64::encode(st);
            masterPlugins.push_back(std::move(pj));
        }
    }
    masterJson["plugins"] = std::move(masterPlugins);
    j["master"] = std::move(masterJson);
    json tracks = json::array();
    int tc = engine->getTrackCount();
    for (int i = 0; i < tc; ++i) {
        Track* t = engine->getTrack(i);
        AudioBuffer* buf = t->audio.load(std::memory_order_relaxed);
        json tj;
        tj["name"] = t->name;
        tj["volume"] = t->volume.load(std::memory_order_relaxed);
        tj["pan"] = t->pan.load(std::memory_order_relaxed);
        tj["muted"] = t->muted.load(std::memory_order_relaxed);
        tj["soloed"] = t->soloed.load(std::memory_order_relaxed);
        tj["armed"] = t->armed.load(std::memory_order_relaxed);
        tj["clipStart"] = t->clipStart.load(std::memory_order_relaxed);
        tj["audioPath"] = pathutil::storeForm(projectDir, t->audioFilePath);
        tj["audioFrames"] = buf ? (int64_t)buf->frames : 0;
        tj["fadeIn"] = (int64_t)t->primaryFadeIn.load(std::memory_order_relaxed);
        tj["fadeOut"] = (int64_t)t->primaryFadeOut.load(std::memory_order_relaxed);
        json sendsArr = json::array();
        for (int b = 0; b < MAX_BUSES; ++b) {
            sendsArr.push_back(t->sends[b].load(std::memory_order_relaxed));
        }
        tj["sends"] = std::move(sendsArr);
        json extraClips = json::array();
        int clipCount = engine->getClipCount(i);
        for (int c = 1; c < clipCount; ++c) {
            json cj;
            cj["audioPath"] = pathutil::storeForm(projectDir, engine->getClipPath(i, c));
            cj["audioFrames"] = (int64_t)engine->getClipFrames(i, c);
            cj["clipStart"] = (int64_t)engine->getClipStart(i, c);
            cj["fadeIn"] = (int64_t)engine->getClipFadeIn(i, c);
            cj["fadeOut"] = (int64_t)engine->getClipFadeOut(i, c);
            extraClips.push_back(std::move(cj));
        }
        tj["extraClips"] = std::move(extraClips);
        json plugins = json::array();
        PluginChain* chain = engine->getPluginChain(i);
        int pc = chain ? chain->getPluginCount() : 0;
        for (int p = 0; p < pc; ++p) {
            json pj;
            pj["path"] = chain->getPluginPath(p);
            pj["id"] = chain->getPluginId(p);
            pj["bypassed"] = chain->isBypassed(p);
            std::string st = chain->saveState(p);
            if (!st.empty()) pj["state"] = base64::encode(st);
            plugins.push_back(std::move(pj));
        }
        tj["plugins"] = std::move(plugins);
        tracks.push_back(std::move(tj));
    }
    j["tracks"] = std::move(tracks);
    return j.dump(2) + "\n";
}

bool ProjectSerializer::loadFromString(const std::string& content, AudioEngine* engine, const std::string& projectDir) {
    json j;
    try { j = json::parse(content); }
    catch (const json::parse_error& e) {
        fprintf(stderr, "[UNDO] ERROR: JSON parse error: %s\n", e.what());
        return false;
    }
    if (!j.is_object()) return false;
    float masterVol = j.value("masterVolume", 1.0f);
    engine->getMaster()->volume.store(masterVol, std::memory_order_relaxed);
    engine->setPlayhead((uint64_t)j.value("playhead", (int64_t)0));
    engine->setBPM(j.value("bpm", 120.0f));
    {
        int num = j.value("timeSigNum", 4);
        int den = j.value("timeSigDen", 4);
        engine->setTimeSignature(num, den);
    }
    engine->setLoopEnabled(j.value("loopEnabled", false));
    engine->setLoopRange((uint64_t)j.value("loopStart", (int64_t)0),
                          (uint64_t)j.value("loopEnd", (int64_t)0));
    engine->setPunchRange((uint64_t)j.value("punchStart", (int64_t)0),
                           (uint64_t)j.value("punchEnd", (int64_t)0));
    if (j.contains("auxBuses") && j["auxBuses"].is_array()) {
        int idx = 0;
        for (auto& bj : j["auxBuses"]) {
            AuxBus* ab = engine->getAuxBus(idx);
            if (!ab) break;
            if (bj.is_object()) {
                ab->name = bj.value("name", ab->name);
                ab->volume.store(bj.value("volume", 1.0f), std::memory_order_relaxed);
            }
            ++idx;
        }
    }
    int currentCount = engine->getTrackCount();
    for (int i = currentCount - 1; i > 0; --i) engine->removeTrack(i);
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
    PluginChain* mchain = engine->getPluginChain(-1);
    if (mchain) {
        while (mchain->getPluginCount() > 0) mchain->removePlugin(0);
    }
    if (j.contains("master") && j["master"].is_object() && mchain) {
        auto& master = j["master"];
        if (master.contains("plugins") && master["plugins"].is_array()) {
            for (auto& pj : master["plugins"]) {
                if (!pj.is_object()) continue;
                std::string plPath = pj.value("path", "");
                std::string plId = pj.value("id", "");
                bool bypassed = pj.value("bypassed", false);
                std::string plStateB64 = pj.value("state", "");
                std::string plState = plStateB64.empty() ? "" : base64::decode(plStateB64);
                if (!plPath.empty() && !plId.empty()) {
                    bool ok = mchain->addPlugin(plPath.c_str(), plId.c_str());
                    if (ok) {
                        int idx = mchain->getPluginCount() - 1;
                        if (bypassed) mchain->setBypass(idx, true);
                        if (!plState.empty()) mchain->loadState(idx, plState);
                    }
                }
            }
        }
    }
    json tracks = j.value("tracks", json::array());
    if (!tracks.is_array()) tracks = json::array();
    size_t trackCount = tracks.size();
    for (size_t i = 1; i < trackCount; ++i) engine->addTrack();
    int tc = engine->getTrackCount();
    int toConfigure = std::min(tc, (int)trackCount);
    for (int i = 0; i < toConfigure; ++i) {
        auto& tj = tracks[i];
        if (!tj.is_object()) continue;
        Track* t = engine->getTrack(i);
        t->name = tj.value("name", "");
        t->audioFilePath = tj.value("audioPath", "");
        t->volume.store(tj.value("volume", 1.0f), std::memory_order_relaxed);
        t->pan.store(tj.value("pan", 0.0f), std::memory_order_relaxed);
        t->muted.store(tj.value("muted", false), std::memory_order_relaxed);
        t->soloed.store(tj.value("soloed", false), std::memory_order_relaxed);
        t->armed.store(tj.value("armed", false), std::memory_order_relaxed);
        t->clipStart.store((uint64_t)std::max((int64_t)0, tj.value("clipStart", (int64_t)0)), std::memory_order_relaxed);
        t->primaryFadeIn.store((uint64_t)std::max((int64_t)0, tj.value("fadeIn", (int64_t)0)), std::memory_order_relaxed);
        t->primaryFadeOut.store((uint64_t)std::max((int64_t)0, tj.value("fadeOut", (int64_t)0)), std::memory_order_relaxed);
        if (tj.contains("sends") && tj["sends"].is_array()) {
            int bi = 0;
            for (auto& sv : tj["sends"]) {
                if (bi >= MAX_BUSES) break;
                t->sends[bi].store(sv.get<float>(), std::memory_order_relaxed);
                ++bi;
            }
        }
        if (!t->audioFilePath.empty()) {
            std::string resolved = pathutil::resolve(projectDir, t->audioFilePath);
            bool loaded = engine->loadWavToTrack(i, resolved.c_str());
            if (!loaded) {
                t->audioFilePath.clear();
            } else {
                t->audioFilePath = resolved;
            }
        }
        if (tj.contains("extraClips") && tj["extraClips"].is_array()) {
            for (auto& ec : tj["extraClips"]) {
                if (!ec.is_object()) continue;
                std::string ecPath = ec.value("audioPath", "");
                uint64_t ecStart = (uint64_t)std::max((int64_t)0, ec.value("clipStart", (int64_t)0));
                if (ecPath.empty()) continue;
                std::string resolved = pathutil::resolve(projectDir, ecPath);
                int rc = engine->addClip(i, resolved.c_str(), ecStart);
                if (rc >= 0) {
                    uint64_t eFin = (uint64_t)std::max((int64_t)0, ec.value("fadeIn", (int64_t)0));
                    uint64_t eFout = (uint64_t)std::max((int64_t)0, ec.value("fadeOut", (int64_t)0));
                    engine->setClipFadeIn(i, rc, eFin);
                    engine->setClipFadeOut(i, rc, eFout);
                }
            }
        }
        if (tj.contains("plugins") && tj["plugins"].is_array()) {
            PluginChain* chain = engine->getPluginChain(i);
            int baseIdx = chain ? chain->getPluginCount() : 0;
            for (auto& pj : tj["plugins"]) {
                if (!pj.is_object()) continue;
                std::string plPath = pj.value("path", "");
                std::string plId = pj.value("id", "");
                bool bypassed = pj.value("bypassed", false);
                std::string plStateB64 = pj.value("state", "");
                std::string plState = plStateB64.empty() ? "" : base64::decode(plStateB64);
                if (!plPath.empty() && !plId.empty()) {
                    bool ok = chain->addPlugin(plPath.c_str(), plId.c_str());
                    if (ok) {
                        if (bypassed) chain->setBypass(baseIdx, true);
                        if (!plState.empty()) chain->loadState(baseIdx, plState);
                        ++baseIdx;
                    }
                }
            }
        }
    }
    return true;
}
