<p align="center">
  <img src="assets/logo.png" alt="Hydraw DAW" width="120"/>
</p>

<h1 align="center">Hydraw DAW ⚡</h1>

<p align="center">
  <b>DAW ágil y moderno para Linux — HTML5 + WebKit2GTK + C++17 + miniaudio</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/PLATFORM-Linux-important?style=flat-square" alt="Platform: Linux"/>
  <img src="https://img.shields.io/badge/LANG-C%2B%2B17-00599C?style=flat-square" alt="C++17"/>
  <img src="https://img.shields.io/badge/UI-HTML5%2BJS%2BTailwind-8A2BE2?style=flat-square" alt="HTML5+JS+Tailwind"/>
  <img src="https://img.shields.io/badge/LICENSE-MIT-yellow?style=flat-square" alt="MIT License"/>
  <img src="https://img.shields.io/badge/version-v0.3.0-blue?style=flat-square" alt="Version 0.3.0"/>
</p>

---

Hydraw DAW es una estación de trabajo de audio digital open-source para Linux. Combina un motor de audio lock-free en C++17 con una interfaz moderna HTML5+Tailwind+Vanilla JS puenteada mediante WebKit2GTK. CLAP como estándar de plugins.

## Características

**Motor de audio**
- [x] miniaudio (48kHz estéreo, bloque 256 frames, baja latencia)
- [x] Transporte completo (Play/Pause/Stop, loop, punch)
- [x] Multi-clip por pista con snapshots atómicos lock-free
- [x] Herramientas de edición: split, trim, fade (lineal)
- [x] Automatización por parámetro (volumen, pan, plugins CLAP)
- [x] Compensación de latencia (PDC) por cadena de efectos
- [x] Congelamiento de pistas (freeze/bounce)
- [x] Buses/Sends auxiliares con sidechain
- [x] Loop y punch in/out

**Grabación**
- [x] Grabación multipista con buffer circular y flush a WAV
- [x] Armado por pista, monitoreo en tiempo real

**Plugins CLAP**
- [x] Hosting completo con dlopen
- [x] Cadena de efectos por pista + master
- [x] Estado persistente (save/load) con base64
- [x] Parámetros editables via `clap_plugin_params_t::flush()`
- [x] GUI en ventana X11 independiente
- [x] `thread-check`, `note-ports`, `latency`, `render` extensions

**MIDI**
- [x] Almacenamiento de notas MIDI por pista
- [x] `clap_note_ports` con eventos note-on/note-off

**Mezcla**
- [x] Mezclador multipista (volumen, pan, mute, solo, armado)
- [x] Faders verticales con medidor de pico
- [x] Bus maestro con soft clipper integrado
- [x] Envíos a buses auxiliares

**Interfaz**
- [x] UI HTML5+Tailwind+Vanilla JS (sin Node, sin toolchain)
- [x] Timeline con waveform canvas por clip
- [x] Regla de tiempo con grid de compases
- [x] 3 herramientas de edición: puntero, split, fade
- [x] Mixer con canales y medidores CSS
- [x] File browser con drag & drop
- [x] Tema oscuro profesional (paleta carbón + azul acento)
- [x] Telemetría C++→JS cada 30ms vía JSON push

**Proyecto**
- [x] Persistencia en `.opndaw` (JSON con `nlohmann/json`)
- [x] Escritura atómica (temp file + rename)
- [x] Exportación offline WAV (16-bit PCM / 32-bit float)
- [x] Undo/Redo (100 operaciones)
- [x] Atajos de teclado (Space → Play/Pause)

## Compilación

```bash
git clone git@github.com:juanpimg/Hydraw-DAW.git
cd Hydraw-DAW
chmod +x scripts/install_deps.sh && ./scripts/install_deps.sh
cmake -B build && cmake --build build -j$(nproc)
./build/hydraw_daw
```

## Dependencias

- **Compilador**: GCC 8+ o Clang 7+ (C++17)
- **CMake** 3.16+
- **GTK3** y **WebKit2GTK 4.1** (`libgtk-3-dev`, `libwebkit2gtk-4.1-dev`)
- **ALSA** (`libasound2-dev`)
- miniaudio, webview y nlohmann/json se descargan automáticamente via FetchContent.

## Documentación técnica

| Documento | Descripción |
|-----------|-------------|
| [`AGENTS.md`](AGENTS.md) | Contexto completo del proyecto (bindings, estado, limitaciones) |
| [`ARCHITECTURE_SPEC.md`](ARCHITECTURE_SPEC.md) | Separación Core/Bridge, interfaces, plan de migración |
| [`PERSISTENCE_SPEC.md`](PERSISTENCE_SPEC.md) | Formato `.opndaw`, pipeline save/load |
| [`CLAP_HOST_SPEC.md`](CLAP_HOST_SPEC.md) | Auditoría CLAP, hallazgos, plan de remediación |
| [`DECISIONS.md`](DECISIONS.md) | Registro de decisiones arquitectónicas (ADR-000 a ADR-012) |
| [`ROADMAP.md`](ROADMAP.md) | Plan de implementación por fases (A/B/C completadas) |
| [`AUDIT_ROADMAP.md`](AUDIT_ROADMAP.md) | Auditoría global de funcionalidades y brechas |

## Limitaciones conocidas

- Solo Linux (WebKit2GTK 4.1)
- Solo CLAP (no VST3/LV2)
- Carga WAV síncrona (bloquea UI brevemente)
- GUI de plugins en ventanas X11 separadas (no embebidas)
- Sin editor de automatización en UI
- Sin piano roll (MIDI storage funcional, sin interfaz gráfica)
- Sin zoom en timeline

## Licencia

MIT. Consulta [`LICENSE`](LICENSE).

## Contribuciones

Bienvenidas. Abre un issue o PR en [github.com/juanpimg/Hydraw-DAW](https://github.com/juanpimg/Hydraw-DAW).
