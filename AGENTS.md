# Hydraw DAW — Project Context

## Goal
Open‑source DAW for Linux with C++17, miniaudio, WebView (WebKit2GTK/GTK3). UI is HTML5+Tailwind+vanilla JS bridged via webview/webview v0.12.0. Replaced ImGui/GLFW in migration.

## Quick Start
```bash
cmake -B build && cmake --build build -j$(nproc)
./build/hydraw_daw
```
Deps: `libgtk-3-dev`, `libwebkit2gtk-4.1-dev`, `libasound2-dev`.

## Architecture
- **C++ side** (`src/main.cpp`): 22+ webview bindings (JS→C++), atomic audio state, UI update thread eval() every 30ms. Crash handler writes async-signal-safe backtrace to `hydraw_crash_<pid>.log`. Cache-buster appends `?v=<epoch>` to HTML URL so WebKitGTK re-fetches on restart.
- **Audio engine** (`src/AudioEngine.h/cpp`): miniaudio callback, lock‑free atomics, peak cache. Deferred audio buffer delete queue avoids use-after-free between main and audio threads.
- **Plugins** (`src/Plugin/`): CLAP hosting via dlopen. PluginChain per track + master chain. State save/load via `clap_istream_t`/`clap_ostream_t` adapters (state persistence in `.opndaw` files).
- **HTML UI** (`assets/index.html`): Tailwind CDN, vanilla JS, no Node. State in global `STATE` object. `cn()` helper for native calls. Padding fix compensates WebKitGTK trailing-zero array bug.
- **Build**: CMake+FetchContent for webview/webview v0.12.0. No ImGui/GLFW/OpenGL.

## Bindings
`nativePlay`, `nativePause`, `nativeStop`, `nativeAddTrack`, `nativeRemoveTrack`, `nativeSetVolume`, `nativeSetPan`, `nativeSetMute`, `nativeSetSolo`, `nativeSetArm`, `nativeLoadWav`, `nativeGetPeakCache`, `nativeListDir`, `nativeGetCwd`, `nativeGetAudioInfo`, `nativePluginList`, `nativePluginBypass`, `nativePluginRemove`, `nativePluginLoad`, `nativeScanClap`, `nativePageReady`, `onNativeUpdate`.

Args use pipe‑delimited format (e.g. `"idx,val"`). All C++ functions use `safeStoi`/`safeStof` to prevent crashes. Void functions return `"null"` (valid JSON). File paths are escaped with `escapeJson()`.

## Critical Context
- **Binary**: `build/hydraw_daw` (~1 MB), links `libwebkit2gtk-4.1.so.0`, `libgtk-3.so.0`.
- **Tailwind**: CDN only — no build step. Keep Tailwind classes minimal; font is system sans-serif (`-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif`). Default text `#e4e4e8` on `#141418`.
- **Drag & drop**: WebKitGTK provides `text/uri-list` (file URIs). Fallback: `files[].path` / `files[].name`.
- **`cn()` helper**: `cn(name, arg)`. Arg is `String(arg)`, or `''` if undefined. Returns native JS value (webview auto‑parses JSON return).
- **UI loop**: Thread pushes `updateUIFromNative(...)` with playhead, peaks, volumes, mutes, solos, arms, names, audioFrames, clipStarts, peakCaches. JS debounces at 25ms. Full updates every ~30ms.
- **`g_pageReady` guard**: C++ doesn't eval() JS until `nativePageReady` fires after DOMContentLoaded.
- **Window**: `set_title("Hydraw DAW")`, `set_size(1280, 720)`.
- **WebKitGTK trailing-zero bug**: `JavaScriptCore` drops trailing zeros from arrays parsed via `eval()`. `[7843143, 0]` → `[7843143]`. Fix: `while (STATE.audioFrames.length < STATE.trackCount) STATE.audioFrames.push(0)` runs on every `updateUIFromNative` call (not just full updates).
- **Deferred audio delete**: `queueAudioBufferDelete()` / `drainPendingAudioBufferDeletes()`. Prevents SIGSEGV when main thread frees an `AudioBuffer*` while the audio callback holds a local copy.
- **Plugin persistence**: Per-plugin state blob (XML text) saved as JSON-escaped string in `.opndaw` files. `PluginChain::saveState(index)` / `loadState(index, blob)` drive CLAP `state.save`/`state.load` extensions via `StringIStream`/`StringOStream`.

## UI Design
- Layout: grid(44px transport | 1fr timeline | 1fr bottom). Bottom: 180px browser | 1fr FX chain | 280px mixer.
- Dark theme (`#141418` bg, not pure black) with Ableton‑inspired contrast. Accent blue `#5599ff` for selection/interaction. Green `#44bb66` for audio presence. Yellow `#eebb44` for playhead/solo. Red `#ee4444` for mute/arm.
- Faders: vertical `<input type=range>` styled with circular blue thumbs. Meters: gradient bars (green→yellow→red).
- Waveform: canvas 2D, `rgba(130,150,190,0.6)`, 200 bars max. `drawAllWaveforms()` explicitly clears canvas when `audioFrames[i] <= 0` to prevent ghost waveforms.
- Popups: centered modal for CLAP plugin list.

## Git & Releases
- Repo: `git@github.com:juanpimg/Hydraw-DAW.git`, branch: `master`.
- Ignored: `build/`, `cmake-build-*/`, `extern/`, `plugins/`, `*.o`, `imgui.ini`, `.DS_Store`, `*.log`, `*.mp3`, `*.wav`. Whitelisted: `!plugins/*.clap`.
- **Releases**: GitHub Actions workflow (`.github/workflows/release.yml`) crea release automáticamente al pushear un tag `v*`. Para crear release: `git tag vX.Y.Z && git push origin vX.Y.Z`. Los tags actuales siempre cuentan como pre‑release mientras el proyecto sea temprano. Todas las versions significan poner tag + push.

## Known Limitations
- No recording, MIDI, or audio export yet.
- Max 16 tracks (`MAX_TRACKS` in `src/Core/Constants.h`).
- WAV loading is synchronous (blocks UI briefly).
- No VST3 support (CLAP only).
- No loop/punch in‑out.
- `build/` dir is ~174 MB (mostly webview FetchContent deps).
- CLAP plugin GUI windows are standalone X11 windows (not embedded in WebView).
