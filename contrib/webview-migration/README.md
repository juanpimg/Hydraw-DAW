# WebView Migration Guide

## Status: BLOCKED — Missing system dev packages

The webview migration code is complete but requires:

```bash
sudo apt-get install libgtk-3-dev libwebkit2gtk-4.1-dev
```

## Files needed for migration:

1. Replace `src/main.cpp` with `contrib/webview-migration/main_webview.cpp`
2. Replace `CMakeLists.txt` with `contrib/webview-migration/CMakeLists_webview.txt`
3. Keep `assets/index.html` (already present)

## Architecture

- **main.cpp**: C++ bridge — AudioEngine + webview::webview
  - JS→C++ bindings: nativePlay, nativePause, nativeStop, nativeSetVolume, etc.
  - C++→JS eval: UI update thread pushes playhead/trackPeaks/volumes every 30ms
- **index.html**: Dark-themed UI with Tailwind CSS CDN
  - Grid layout: Transport | Timeline | Browser | FX Chain | Mixer
  - Vertical faders (input[type=range] slider-vertical)
  - LED-style VU meters (CSS gradient fill, height controlled by peak data)
  - Canvas-based ruler and waveform rendering
- **CMakeLists.txt**: FetchContent for webview/webview v0.12.0
  - Links webview::core, pthread, dl, rt, asound
  - Removes all ImGui/GLFW/OpenGL dependencies

## WebView API used (webview/webview v0.12.0)

```cpp
w.set_title("Hydraw DAW");
w.set_size(1280, 720, WEBVIEW_HINT_NONE);
w.navigate("file:///path/to/index.html");
w.bind("nativeFunc", callback);
w.eval("javascript_code");
w.run();  // blocking GTK main loop
```

Bind callback signature: `std::function<std::string(std::string)>`
