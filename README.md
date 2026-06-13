<p align="center">
  <img src="assets/logo.png" alt="Hydraw DAW" width="120"/>
</p>

<h1 align="center">Hydraw DAW ⚡</h1>

<p align="center">
  <b>Un DAW ágil y moderno para Linux</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/PLATFORM-Linux-important?style=flat-square" alt="Platform: Linux"/>
  <img src="https://img.shields.io/badge/LANG-C%2B%2B17-00599C?style=flat-square" alt="C++17"/>
  <img src="https://img.shields.io/badge/LICENSE-MIT-yellow?style=flat-square" alt="MIT License"/>
</p>

---

**Hydraw DAW** es una estación de trabajo de audio digital minimalista, rápida y open-source. Inspirada en la inmediatez de BandLab y la flexibilidad de Ableton Live.

## Características

- [x] Motor de audio con miniaudio (baja latencia, ALSA/PulseAudio)
- [x] UI híbrida HTML5+Tailwind+WebKit2GTK con puente nativo C++/JS
- [x] Layout estilo Ableton (Transporte, Timeline, Mixer, FX Chain, File Browser)
- [x] Carga de archivos WAV con visualización de waveform
- [x] Mezclador multipista (volumen, pan, mute, solo, armado)
- [x] Cadena de plugins CLAP por pista
- [x] Arrastrar y soltar archivos desde el sistema
- [ ] Grabación multipista (próximamente)
- [ ] Exportación de audio (próximamente)

## Compilación

```bash
git clone git@github.com:juanpimg/Hydraw-DAW.git
cd Hydraw-DAW
chmod +x scripts/install_deps.sh && ./scripts/install_deps.sh
cmake -B build && cmake --build build
./build/hydraw_daw
```

## Dependencias

- **Compilador**: GCC 8+ o Clang 7+ (con soporte C++17)
- **CMake** 3.16+
- **GTK3** y **WebKit2GTK 4.1** (`libgtk-3-dev`, `libwebkit2gtk-4.1-dev`)
- **ALSA** (`libasound2-dev`)
- El motor de miniaudio y webview se descargan automáticamente via FetchContent.

## Licencia

Distribuido bajo la licencia MIT. Consulta el archivo `LICENSE` para más información.

## Contribuciones

Las contribuciones son bienvenidas. Por favor, abre un issue o un pull request en el repositorio.
