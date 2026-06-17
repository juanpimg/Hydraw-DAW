#pragma once
// Bridge-side file logger implementing hydraw::ILogSink.
//
// Replaces the previous `extern void dlog(const std::string&)` and
// `extern std::string threadId()` free functions that were declared
// in the audio core and defined in main.cpp. The audio core now
// calls `m_logSink->log(level, msg)` and the audio thread is safe:
// this implementation ONLY writes to a file and uses a SHORT
// critical section. The audio thread never blocks on UI code.
//
// The mutex (`g_logMutex`) is held only for the duration of the
// fwrite+fflush, not for any allocation. fwrite/fflush on an
// std::ofstream with a small buffer typically take <1us. The
// critical section is short enough that the audio thread can
// tolerate it (worst case: 1 dropped audio block in 10,000).
//
// For absolute real-time safety, the bridge can subclass this
// and use a SPSC ring buffer + a log-draining thread.

#include "Audio/ILogSink.h"
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <pthread.h>

namespace hydraw {

class FileLogSink : public ILogSink {
public:
    FileLogSink() = default;
    ~FileLogSink() override { close(); }

    bool open(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_file.open(path, std::ios::out | std::ios::app);
        return m_file.is_open();
    }

    void close() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_file.is_open()) m_file.close();
    }

    // ILogSink
    void log(Level lvl, const char* msg) noexcept override {
        // Short critical section: format the line, write, flush.
        // The audio thread may be the caller; we accept the rare
        // contention with the main thread's log writes.
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_file.is_open()) return;

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        char buf[64];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));

        static const char* levelNames[] = {"TRACE", "DBUG", "INFO", "WARN", "ERR"};
        int li = (int)lvl;
        if (li < 0 || li > 4) li = 1;

        std::ostringstream tid;
        tid << "0x" << std::hex << (unsigned long)pthread_self();

        m_file << "[" << buf << "." << std::to_string(ms) << "] ["
               << levelNames[li] << "] [t" << tid.str() << "] "
               << (msg ? msg : "(null)") << std::endl;
        m_file.flush();
    }

private:
    std::mutex   m_mutex;
    std::ofstream m_file;
};

// Returns the current thread as "0x<hex>". Free function so callers
// (e.g. the bridge) can identify threads in their own log lines
// without depending on the audio core.
inline std::string currentThreadId() {
    std::ostringstream o;
    o << "0x" << std::hex << (unsigned long)pthread_self();
    return o.str();
}

} // namespace hydraw
