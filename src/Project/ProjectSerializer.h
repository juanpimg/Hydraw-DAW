#pragma once
#include <string>
#include "AudioEngine.h"

class ProjectSerializer {
public:
    static bool save(const char* path, AudioEngine* engine);
    static bool load(const char* path, AudioEngine* engine);

private:
    static std::string escapeJson(const std::string& s);
    static std::string unescapeJson(const std::string& s);
    static std::string readFile(const char* path);
};
