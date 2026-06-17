# Hydraw DAW — Project Context

## Goal
Open‑source DAW for Linux with C++17, miniaudio, WebView (WebKit2GTK/GTK3). UI is HTML5+Tailwind+vanilla JS bridged via webview/webview v0.12.0. Replaced ImGui/GLFW in migration (see DECISIONS.md ADR-009).

## Quick Start
```bash
cmake -B build && cmake --build build -j$(nproc)
./build/hydraw_daw
```
Deps: `libgtk-3-dev`, `libwebkit2gtk-4.1-dev`, `libasound2-dev`.

## Architecture
- **C++ side** (`src/main.cpp`): 56 webview bindings (JS→C++), atomic audio state, UI update thread eval() every 30ms. Telemetry push uses **JSON serialization** (`buildJsonTelemetry` → `JSON.parse(...)` in JS) — array-literal eval was fragile in WebKitGTK's JSC and was the root cause of the post-load "no waveforms" bug. Generation counter (`g_telemetryGen`) ensures the JS side drops stale full updates from before a load. Crash handler writes async-signal-safe backtrace to `hydraw_crash_<pid>.log`. Cache-buster appends `?v=<epoch>` to HTML URL so WebKitGTK re-fetches on restart. Forces `GDK_BACKEND=x11` for CLAP plugin XEmbed.
- **Architecture**: `hydraw_core` is a static library (no WebKit/GTK) — see `ARCHITECTURE_SPEC.md`. `src/main.cpp` is the bridge: provides `FileLogSink` (ILogSink impl), `BridgeHostExtensions` (IHostExtensions impl), and is the ONLY TU that includes `<webview/webview.h>`, `<webkit2/...>`, `<gtk/gtk.h>`, `<gdk/...>`, `<GL/glx.h>`. Interfaces (`IAudioObserver`, `ILogSink`, `IHostExtensions`) exist in `src/Audio/` but are not yet fully adopted — some extern globals remain.
- **Audio engine** (`src/AudioEngine.h/cpp`): miniaudio callback, lock‑free atomics, peak cache (1 peak per 256 frames, no cap), offline render for WAV export, built-in SoftCliper DSP on master, DelayLine per-track + master for PDC. Deferred audio buffer/delete queues avoid use-after-free. Multi‑clip per track: primary + extras with atomic `ClipsSnapshot*`.
- **Plugins** (`src/Plugin/`): CLAP hosting via dlopen. Lock‑free snapshot model with raw `atomic<Snapshot*>` + deferred delete. Per-track PluginChain + master chain (with PeakEater). State save/load via `clap_istream_t`/`clap_ostream_t` adapters. Multi‑port support (capped at 4 ports). `processPendingFlushes` with real `clap_plugin_params_t::flush` for param automation. PDC latency via `clap_plugin_latency`.
- **HTML UI** (`assets/index.html`, ~2500 lines): Tailwind CDN, vanilla JS, no Node. State in global `STATE` object. `cn()` helper for native calls. CSS custom properties with semantic design tokens. 3‑tool system: pointer (select/drag), split (scissors cursor, clicks clip to split), fade (drag fade overlays or clip edges). Fused single `mousedown` handler for all clip interactions.
- **Project persistence** (`.opndaw`): JSON via `nlohmann/json` v3.11.3 with `FetchContent`. Atomic write (temp file + rename). Load uses **Strict Sequential Hydration**: stop device → drain → load → notify UI → restart. Emits `window.__onProjectLoaded()` so JS invalidates all caches.
- **Build**: CMake+FetchContent for webview/webview v0.12.0 + nlohmann/json v3.11.3. Two targets: `hydraw_core` (static lib, src/AudioEngine.cpp + PluginChain.cpp + ProjectSerializer.cpp) and `hydraw_daw` (executable, links core + webview + GTK + X11).

## Bindings (56 total)
| Group | Bindings |
|---|---|
| **Transport** | nativePlay, nativePause, nativeStop, nativeSetPlayhead, nativeSetBPM, nativeSetTimeSignature, nativeSetLoopEnabled, nativeSetLoopRange, nativeSetPunchRange |
| **Recording** | nativeStartRecording, nativeStopRecording |
| **Track/Clip** | nativeAddTrack, nativeRemoveTrack, nativeSetVolume, nativeSetMasterVolume, nativeSetSendLevel, nativeSetAuxVolume, nativeSetClipStart, nativeMoveClip, nativeSetPan, nativeSetMute, nativeSetSolo, nativeSetArm, nativeLoadWav, nativeAddClip, nativeRemoveClip, nativeSplitClip, nativeTrimClip, nativeSetFadeIn, nativeSetFadeOut, nativeGetPeakCache |
| **Freeze** | nativeFreezeTrack, nativeUnfreezeTrack |
| **Plugin** | nativePluginList, nativePluginLoad, nativePluginMove, nativeScanClap, nativePluginShowGUI, nativeGetPluginParams, nativeSetPluginParam |
| **MIDI** | nativeAddMidiNote, nativeClearMidiLane, nativeGetMidiNoteCount |
| **Undo/Redo** | nativeUndo, nativeRedo |
| **File/Project** | nativeListDir, nativeGetCwd, nativeGetAudioInfo, nativeShowSaveDialog, nativeShowOpenDialog, nativeSaveProject, nativeLoadProject, nativeExportAudio |
| **System** | nativePageReady, nativeLog, nativeSetSoftClipperDrive |

Args use comma‑delimited format (e.g. `"idx,val"` or `"track,clip,pos"`). All C++ functions use `safeStoi`/`safeStof` to prevent crashes. Void functions return `"null"`.

## Multi‑Clip Model
- **Primary clip**: stored directly on `Track` struct (`std::atomic<AudioBuffer*> audio`, `clipStart`, `primaryFadeIn/Out`). Lock‑free for audio thread.
- **Extra clips**: stored in `m_trackClips[trackIndex]` vector protected by `m_clipsMutex`. Published atomically to audio thread via `ClipsSnapshot*` (immutable once published, deferred‑deleted).
- **Snapshots**: `publishClipsSnapshot(trackIndex)` creates a deep copy of `m_trackClips` for the audio thread. Fade values are atomics on each `Clip` struct.
- **Promotion/demotion**: When primary ↔ extra transition happens (`addClip`, `moveClip`, `removeClip`, `rebuildPrimaryFromClips`), fade values (`primaryFadeIn/Out` ↔ `Clip.fadeIn/Out`) are transferred atomically.

## Audio Processing
- **Audio callback** (miniaudio, 256‑frame blocks): processes primary clip + all snapshot extras per track. Applies linear gain envelope for fades. Sums sends to aux buses then buses to master. Plugin chains process via snapshot (try_lock‑free). PDC delay line per track + master. Loop/punch wrap logic.
- **Fade gain**: `gain = absS / fIn` for frames < `fIn` (fade‑in), `gain = (frames - absS) / fOut` for frames ≥ `frames - fOut` (fade‑out).
- **Peak cache**: 1 peak per 256 frames (linear temporal resolution, no cap, no minimum floor). All peaks serialized in telemetry. JS draws 1 bar = 1 peak (no resampling).

## UI Design
- Layout: grid(44px transport | 1fr timeline | 1fr bottom). Bottom: 180px browser | 1fr FX chain | 280px mixer.
- Dark theme with CSS custom property tokens. Warm carbon base (`#121417`), slate text hierarchy. Accent blue `#5599ff`.
- 3 edit tools: **pointer** (select/move), **split** (click to split clip), **fade** (drag fade‑in/‑out overlays or clip edges).
- Fade overlays: dynamic width from telemetry, colored gradient, clickable (no `pointer-events:none`), `ew-resize` cursor in fade mode.
- Waveform: canvas 2D, `rgba(130,150,190,0.6)`. `drawAllWaveforms()` loops over all `.clip-block` elements in DOM.

## Git & Releases
- Repo: `git@github.com:juanpimg/Hydraw-DAW.git`, branch: `master`.
- Ignored: `build/`, `cmake-build-*/`, `extern/`, `plugins/`, `*.o`, `imgui.ini`, `.DS_Store`, `*.log`, `*.mp3`, `*.wav`. Whitelisted: `!plugins/*.clap`.
- **Releases**: GitHub Actions workflow `.github/workflows/release.yml` crea release al pushear tag `v*`. `git tag vX.Y.Z && git push origin vX.Y.Z`.

## Docs
- `ARCHITECTURE_SPEC.md` — Core/bridge separation spec (partially implemented).
- `PERSISTENCE_SPEC.md` — `.opndaw` file format and load/save pipeline.
- `CLAP_HOST_SPEC.md` — CLAP 1.x host compliance audit with 8 findings.
- `DECISIONS.md` — ADR‑000 through ADR‑012.
- `ROADMAP.md` — 3‑phase implementation plan (A/B/C), now complete.
- `AUDIT_ROADMAP.md` — Feature‑by‑feature implementation status.
- `CREDITS.md` — Third‑party credits (PeakEater GPL‑3.0).

## Known Limitations
- No VST3 support (CLAP only).
- WAV loading is synchronous (blocks UI briefly).
- Loop/punch: implemented but no UI controls for range editing.
- Recording: audio capture to WAV works, no overdub/punch‑in‑out UI.
- MIDI: note storage + add/clear/get bindings exist, no piano‑roll UI.
- Automation: volume/pan lanes exist in C++, no UI editor.
- Plugin params UI: `nativeGetPluginParams`/`nativeSetPluginParam` bindings exist, no UI.
- CLAP H1 (critical): `PluginChain::process` uses `try_lock`, drops audio if contended.
- CLAP H2 (high): `clap_host_request_resize` calls GTK from any thread.
- Multi‑port plugins: all ports share same buffer (crossovers incorrect).
- Plugin GUIs: standalone X11 windows, not embedded.
- No zoom, no timeline ruler range editing.
