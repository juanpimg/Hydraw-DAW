# Hydraw DAW — Architecture Spec (Clean Separation: Core vs Bridge)

> Codifies the consensus reached in the May 2026 forensic audit of
> the audio core / WebKit bridge coupling.

## Goal

Make `hydraw_core` a standalone static library (`.a`) that:
- Has zero WebKit/GTK/X11/GLib dependencies.
- Compiles and links on any host (Linux, macOS, future Windows).
- Can be reused by any UI front-end (WebView, native, headless tests,
  mobile companion, etc.).

The bridge (`hydraw_daw` executable) becomes a thin adapter that:
- Owns the WebKit/GTK integration.
- Implements an observer interface the core uses to push events.
- Translates native bindings (JS → C++) into audio-engine method calls.
- Has zero state owned by the audio core.

## Layer map (current)

```
┌──────────────────────────────────────────────────────────────────┐
│  hydraw_daw  (executable) — WebKit/GTK bridge                   │
│  ─ src/main.cpp  (BridgeController, ~56 bindings)                │
│  ─ src/Util/CrashHandler.{h,cpp}       (signal-safe crash log)   │
│  ─ ILogSink impl (FileLogSink inline in main.cpp)                │
│  ─ IHostExtensions impl (BridgeHostExtensions inline in main.cpp)│
└──────────────────────────────────────────────────────────────────┘
                                │
                                │  IAudioObserver, ILogSink,
                                │  IHostExtensions  (pure interfaces)
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│  hydraw_core  (static library) — audio engine + persistence    │
│  ─ src/AudioEngine.h/cpp       (audio callback, tracks, buses)   │
│  ─ src/Plugin/PluginChain.h/cpp (CLAP hosting, snapshots)        │
│  ─ src/Audio/IAudioObserver.h   (pure interface)                  │
│  ─ src/Audio/IHostExtensions.h  (pure interface)                  │
│  ─ src/Audio/ILogSink.h         (pure interface)                  │
│  ─ src/Project/ProjectSerializer.{h,cpp}                          │
│  ─ src/Export/WavWriter.h                                        │
│  ─ src/Core/Types.h, Constants.h                                 │
│  ─ src/DSP/SoftClipper.h                                         │
│  ─ src/Util/Base64.h                                              │
└──────────────────────────────────────────────────────────────────┘
```

> **Note:** Bridge refactor classes (`PluginGuiHost`, `DragDropAdapter`,
> `ExportService`) from the original spec are NOT yet extracted as
> separate files — they remain inline in `main.cpp`. Phases 5–7 of the
> migration plan are still pending.

## The 3 interfaces (pure C++)

### `IAudioObserver` — the event bus from core → bridge

```cpp
namespace hydraw {
class IAudioObserver {
public:
    virtual ~IAudioObserver() = default;
    virtual void onTelemetry(const TelemetrySnapshot& s) noexcept = 0;
    virtual void onProjectLoaded(uint64_t gen) noexcept = 0;
    virtual void onExportComplete(bool ok) noexcept = 0;
    virtual void onLog(ILogSink::Level lvl, const char* msg) noexcept = 0;
    // Optional hooks (default no-op):
    virtual void onPlayheadMoved(uint64_t pos) noexcept {}
    virtual void onTracksAdded(int newCount) noexcept {}
    virtual void onTracksRemoved(int newCount) noexcept {}
    virtual void onTrackAudioLoaded(int trackIdx) noexcept {}
};
} // namespace hydraw
```

`AudioEngine` holds a `std::vector<IAudioObserver*>` protected by a
`std::shared_mutex`. Observer methods may be called from the audio
thread (`onTelemetry`, `onLog` for real-time events) or from the main
thread (everything else). Implementations must be reentrant and
lock-free or use a deferred-dispatch pattern.

### `ILogSink` — the logger

```cpp
namespace hydraw {
class ILogSink {
public:
    enum class Level { Trace, Debug, Info, Warn, Error };
    virtual ~ILogSink() = default;
    // Called from ANY thread, including the audio thread.
    // Implementation must be lock-free or use a SPSC ring buffer;
    // MUST NOT block, MUST NOT allocate.
    virtual void log(Level lvl, const char* msg) noexcept = 0;
};
} // namespace hydraw
```

Replaces `extern void dlog(const std::string&)` and the
`fprintf(stderr, ...)` in `clap_host_log`. The bridge provides a
default implementation that writes to a file (current `dlog` behavior)
and forwards to the JS console via WebKit (optional).

### `IHostExtensions` — CLAP host extension provider

```cpp
namespace hydraw {
class IHostExtensions {
public:
    virtual ~IHostExtensions() = default;
    // CLAP contract: returns a const void* matching the requested
    // extension id (CLAP_EXT_GUI, CLAP_EXT_TIMER_SUPPORT, etc.) or
    // nullptr. The returned pointer's lifetime MUST outlive the
    // audio engine.
    virtual const void* getExtension(const char* id) const noexcept = 0;
    // Convenience helpers (callers cast to typed CLAP structs):
    const clap_host_gui_t*       gui()        const { return (const clap_host_gui_t*)       getExtension(CLAP_EXT_GUI); }
    const clap_host_timer_support_t* timer() const { return (const clap_host_timer_support_t*) getExtension(CLAP_EXT_TIMER_SUPPORT); }
    // ... etc
};
} // namespace hydraw
```

`PluginChain` takes an `IHostExtensions*` at construction. The bridge
implements it (the `s_hostGui`, `s_hostTimer`, etc. globals become
fields of the `BridgeController` or a `PluginGuiHost`).

## CMake structure (current)

```cmake
# 1. The audio core — pure static library
add_library(hydraw_core STATIC
    src/AudioEngine.cpp
    src/Plugin/PluginChain.cpp
    src/Project/ProjectSerializer.cpp
)
target_include_directories(hydraw_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/clap
)
set_source_files_properties(src/AudioEngine.cpp
    PROPERTIES COMPILE_DEFINITIONS MINIAUDIO_IMPLEMENTATION)
target_link_libraries(hydraw_core PUBLIC pthread dl)
if(UNIX AND NOT APPLE)
    target_link_libraries(hydraw_core PUBLIC asound)
endif()
# CRITICAL: NO webview::core, NO GL, NO X11 in the core's link list

# 2. The WebKit bridge — executable
add_executable(hydraw_daw
    src/main.cpp
    src/Util/CrashHandler.cpp
)
target_link_libraries(hydraw_daw PRIVATE
    hydraw_core
    webview::core
    GL
    X11
    asound
    pthread
    dl
    rt
)
```

## Implementation phases (in dependency order)

Each phase MUST keep the build green before proceeding.

### Phase 1: Pure interfaces (no behavior change)
- [x] Create `src/Audio/IAudioObserver.h`, `src/Audio/ILogSink.h`, `src/Audio/IHostExtensions.h`.
- [x] These are pure header-only interfaces in the `hydraw` namespace.
- **No changes to existing code.** Just adds 3 new headers.

### Phase 2: Log sink migration (audio thread safe)
- [x] `FileLogSink` implementation inline in `main.cpp` (implements `ILogSink`).
- [x] `AudioEngine` and `PluginChain` use a `const ILogSink*` (constructor-injected).
- [x] `dlog()` calls replaced with `if (m_logSink) m_logSink->log(...)`.
- [x] `clap_host_log` routes through the sink.

### Phase 3: Host extensions migration
- [x] `PluginChain` constructor takes a `IHostExtensions*`.
- [x] `clap_get_extension` returns `m_hostExt ? m_hostExt->getExtension(id) : nullptr`.
- [x] `BridgeHostExtensions` implementation inline in `main.cpp`.
- [ ] `s_hostGui` extern is still present (not yet fully removed).

### Phase 4: UI-state leak cleanup
- [ ] Remove `PluginInstance::guiOpen` field (bridge owns its own map).
- [ ] Remove `PluginInstance::setPluginHostData` (bridge writes directly).

### Phase 5: Delete dead code
- [x] `GuiLayer.{h,cpp}`, `MixerPanel.{h,cpp}`, `TimelinePanel.{h,cpp}` deleted.
- [ ] Delete `initMainThreadId` / `g_mainThreadId` (vestigial).
- [ ] Delete 8 dead bindings (`nativePluginBypass`, `nativePluginRemove`, etc. — JS uses different bindings).
- [ ] Fix the stale "56 native bindings registered" log line.

### Phase 6: CMake split
- [x] Split into `hydraw_core` static lib + `hydraw_daw` executable.
- [x] `hydraw_core` compiles WITHOUT webview/gtk/glib headers.
- [x] Executable still builds and links.

### Phase 7 (optional, next wave): Bridge refactor
- [ ] `BridgeController` class with 56 private methods + `registerBindings()`.
- [ ] `PluginGuiHost` class encapsulating X11+GTK+CLAP.
- [ ] `DragDropAdapter` class for WebKit drag/drop.
- [ ] `ExportService` class returning `std::future<bool>`.
- [ ] `IAudioObserver` push-based telemetry (replaces `uiUpdateLoop` polling).
- [ ] `RequestParser` for the 50+ duplicated arg-parsing sites.

## Thread safety contract

| Action | Thread | Mechanism |
|---|---|---|
| Read audio state for telemetry | any | `IAudioObserver::onTelemetry` (audio core calls) |
| Modify audio state | main (or worker) | Engine method calls from bridge, documented as "main thread" |
| Load project | main | `engine->loadProject(path)` returns `void`; bridge does the post-load `__onProjectLoaded` eval |
| Export audio | worker | `engine->renderOffline(...)` returns `void`; bridge does the `onExportDone` eval |
| Open plugin GUI | main | `PluginGuiHost::openGui` (uses `g_main_context_invoke` internally if needed) |
| Plugin CLAP host callbacks | plugin's thread | `IHostExtensions` static methods; always `g_main_context_invoke` to main thread for GTK work |
| Audio thread | miniaudio callback | only reads `IAudioObserver*` list and `ILogSink*` (both lock-free) |

## Backward compatibility

- The .opndaw file format is unchanged.
- The JS-side `cn()` API is unchanged.
- The `assets/index.html` is unchanged.
- The C++ public API of `AudioEngine` and `PluginChain` is mostly unchanged (the only breaking change is the constructor signature for `PluginChain` to take `IHostExtensions*`).

## Out of scope (future work)

- Async WAV loading (worker thread).
- macOS WKWebView bridge (instead of WebKitGTK).
- Mobile companion app bridge (different transport).
- Unit tests for the core (would use the `core_smoke` target).
