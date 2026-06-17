#pragma once
// ILogSink — pure log sink interface for the audio core.
//
// Replaces the previous `extern void dlog(const std::string&)` and
// `fprintf(stderr, ...)` in the audio code. The audio core has zero
// knowledge of files, stderr, the JS bridge, or threading policy —
// it just calls `m_logSink->log(...)` and the bridge implementation
// decides what to do.
//
// CONTRACT:
//   - The sink MAY be null. The audio core checks for null and skips.
//   - `log()` is called from ANY thread, including the audio thread
//     (miniaudio callback) and arbitrary plugin worker threads.
//   - Implementations MUST be lock-free or non-blocking. They may
//     drop, debounce, batch into a SPSC ring, or forward — but they
//     MUST NOT take a mutex that could be held by another thread
//     that the audio thread depends on.
//   - The `msg` pointer is only valid for the duration of the call.
//     The sink MUST copy the bytes if it wants to retain them.
//
// The hydraw_bridge executable provides a default implementation
// in src/Util/Logger.h that writes to a file and (optionally)
// debounces audio-thread messages.

#include <cstdarg>
#include <cstddef>

namespace hydraw {

class ILogSink {
public:
    enum class Level : int {
        Trace = 0,
        Debug = 1,
        Info  = 2,
        Warn  = 3,
        Error = 4,
    };

    virtual ~ILogSink() = default;

    // Hot path. May be called from the audio thread, plugin GUI
    // thread, main thread, or worker threads. Implementation MUST be
    // safe under concurrent invocation from multiple threads.
    // Implementation MUST NOT block. The default no-op is allowed.
    virtual void log(Level lvl, const char* msg) noexcept = 0;
};

} // namespace hydraw
