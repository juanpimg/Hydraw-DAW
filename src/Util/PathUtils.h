#pragma once
// Path utilities for project portability. Hydraw stores audio and CLAP
// plugin paths relative to the .opndaw project file when possible, so
// that moving a project directory preserves the wiring.
//
// Format used in the JSON:
//   "audioPath": "sounds/kick.wav"        // relative
//   "audioPath": "/abs/path/kick.wav"     // absolute
//
// On save: if audioPath is under the project directory (or can be made
// relative to it), store the relative form. Otherwise keep absolute.
// On load: resolve relative paths against the project directory; if the
// file is missing, fall back to the absolute path (and log a warning).

#include <string>
#include <filesystem>

namespace pathutil {

// Returns true if `child` is the same as or under `parent` after
// canonicalization. Both paths may be relative or absolute. Used to
// decide whether a path can be made relative.
inline bool isUnder(const std::string& parent, const std::string& child) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path pParent = fs::weakly_canonical(fs::path(parent), ec);
    if (ec) return false;
    fs::path pChild = fs::weakly_canonical(fs::path(child), ec);
    if (ec) return false;
    // Append separator to prevent /foo/bar matching /foo/barbaz
    std::string sParent = pParent.string();
    if (sParent.back() != '/') sParent += '/';
    std::string sChild = pChild.string();
    if (sChild.size() < sParent.size()) return false;
    return sChild.compare(0, sParent.size(), sParent) == 0;
}

// Convert child to a path relative to parent. Both must be absolute.
inline std::string makeRelative(const std::string& parent, const std::string& child) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path pParent = fs::weakly_canonical(fs::path(parent), ec);
    if (ec) return child;
    fs::path pChild = fs::weakly_canonical(fs::path(child), ec);
    if (ec) return child;
    fs::path rel = fs::relative(pChild, pParent, ec);
    if (ec) return child;
    return rel.string();
}

// Resolve a possibly-relative path against the project directory.
// If the input is absolute and exists, returns it as-is.
// If the input is relative, tries `<projectDir>/<input>` first; if that
// fails, returns `<projectDir>/<input>` anyway (caller will detect
// the missing file via the load result).
inline std::string resolve(const std::string& projectDir, const std::string& input) {
    namespace fs = std::filesystem;
    if (input.empty()) return "";
    fs::path p(input);
    if (p.is_absolute()) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p.lexically_normal().string();
        return p.string();
    }
    if (projectDir.empty()) return p.lexically_normal().string();
    fs::path joined = fs::path(projectDir) / p;
    return joined.lexically_normal().string();
}

// Returns the directory portion of a project file path.
// "/foo/bar.opndaw" -> "/foo"
inline std::string projectDir(const std::string& projectPath) {
    namespace fs = std::filesystem;
    fs::path p(projectPath);
    return p.parent_path().string();
}

// Decide how to store a path: relative if under projectDir, else
// absolute. Returns the form to write into the JSON file.
inline std::string storeForm(const std::string& projectDirPath, const std::string& audioPath) {
    if (audioPath.empty() || projectDirPath.empty()) return audioPath;
    if (!std::filesystem::path(audioPath).is_absolute()) return audioPath;
    if (!isUnder(projectDirPath, audioPath)) return audioPath;
    return makeRelative(projectDirPath, audioPath);
}

} // namespace pathutil
