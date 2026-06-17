#pragma once
#include <string>
#include <vector>
#include "AudioEngine.h"

class ProjectSerializer {
public:
    // Save / load a .opndaw project file. Audio and plugin paths are
    // stored relative to the project file's directory when possible
    // (see Util/PathUtils.h). The audio device MUST be stopped before
    // calling load() — the Strict Sequential Hydration pattern in
    // main.cpp handles this.
    static bool save(const char* path, AudioEngine* engine);
    static bool load(const char* path, AudioEngine* engine);
    // Snapshot the full project state to a JSON string. Used by
    // undo/redo (C.4): capture the JSON blob before the user action,
    // push to history; on undo, restore from JSON via loadString.
    static std::string saveToString(AudioEngine* engine, const std::string& projectDir);
    // Restore from a JSON string. Same strict sequence as load(): the
    // device must be stopped by the caller. projectDir is used to
    // resolve relative audio paths.
    static bool loadFromString(const std::string& json, AudioEngine* engine, const std::string& projectDir);

private:
    static std::string readFile(const char* path);
};
