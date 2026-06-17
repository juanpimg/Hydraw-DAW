# Auditoría Global — Hydraw DAW
## Sumario de Funcionalidades, Análisis de Brechas y Hoja de Ruta

**Fecha:** 2026-06-18 | **Base de código:** ~6.500 líneas C++ + ~3.000 líneas JS/HTML/CSS
**Arquitectura:** `hydraw_core` (static lib, zero WebKit) → `hydraw_daw` (WebKit bridge)

---

## 1. Sumario de Funcionalidades Implementadas (Estado Actual)

### 1.1 Motor de Audio y DSP

| Componente | Estado | Detalle |
|---|---|---|
| **Inicialización miniaudio** | ✅ Operativo | `ma_format_f32`, estéreo, 48kHz fijo, block 256 frames, 3 períodos, prioridad tiempo real |
| **Transporte (Play/Pause/Stop)** | ✅ Operativo | Señalización atómica `playing`/`playhead`; auto-stop cuando todas las pistas terminan |
| **Mezcla por pista** | ✅ Operativo | Volumen, paneo, mute, solo por pista — todos `std::atomic` |
| **Medidores de pico** | ✅ Operativo | `peakL`/`peakR` atómicos por pista y master; actualizados en callback y empujados cada 30ms |
| **Cadena de efectos por pista** | ✅ Operativo | `PluginChain` único por pista con modelo de snapshot lock-free |
| **Cadena de efectos master** | ✅ Operativo | Master `PluginChain` con PeakEater pre-cargado |
| **Soft Clipper** | ✅ Operativo | Implementado como builtin plugin en `PluginChain::processBuiltin()` (tanh con drive parametrizable) |
| **Carga de WAV** | ✅ Operativo | `loadWavToTrack()` sincrónico; `clipStart` atómico |
| **Movimiento de clips** | ✅ Operativo | `moveClip()` con intercambio atómico de punteros y snapshot |
| **Render offline (export)** | ✅ Operativo | `renderOffline()` recorre toda la duración del proyecto |
| **Buffer deferred delete** | ✅ Operativo | `queueAudioBufferDelete()` / `drainPendingAudioBufferDeletes()` previene UAF |
| **Multi-clip por pista** | ✅ Operativo | Primary clip + extras via `std::atomic<ClipsSnapshot*>`. Snapshots inmutables con deferred delete. |
| **Split/Trim/Fade** | ✅ Operativo | `splitClip()` duplica AudioBuffer con refcount. `trimClip()` ajusta offset/frames. Fade lineal con `fadeInSamples`/`fadeOutSamples` atómicos en `Clip`. |
| **Loop / Punch** | ✅ Operativo | `loopEnabled`, `loopStart/End` atómicos. Auto-wrap playhead. Punch in/out para grabación. |
| **Grabación de audio** | ✅ Operativo | Buffer circular por track, flush asíncrono a WAV, carga como clip al parar. |
| **Buses / Sends** | ✅ Operativo | Array de buses con propio PluginChain. Sends por track con nivel atómico. Sidechain injection. Snapshot atómico de routing. |
| **Automatización** | ✅ Operativo | `AutomationPoint` con `samplePos`/`value`. Búsqueda binaria en `process()`. Snapshots atómicos. Flush bidireccional de parámetros CLAP. |
| **Track Freeze** | ✅ Operativo | Render offline por pista. Flag `frozen` atómico. Swap de buffer con deferred delete. |
| **PDC (latency compensation)** | ✅ Operativo | `clap_plugin_latency` query. DelayLine rotatorio por track + master. Tamaño compensado vía atómico. |

### 1.2 Host de Plugins CLAP

| Componente | Estado | Detalle |
|---|---|---|
| **Carga via dlopen** | ✅ Operativo | `ClapHost::loadLibrary()` con refcounting |
| **Enumeración de fábrica** | ✅ Operativo | `getPluginCount()` / `getPluginDescriptor()` / `createPlugin()` vía descriptor index |
| **Ciclo de vida completo** | ✅ Operativo | `init` → `activate(48000,256)` → `start_processing` → `process` → `stop_processing` → `deactivate` → `destroy` |
| **Modelo de snapshot lock-free** | ✅ Operativo | `std::atomic<Snapshot*>` con deferred delete queue |
| **Bypass** | ✅ Operativo | Toggle atómico `active` por plugin |
| **Reordenación** | ✅ Operativo | `movePlugin()` con publicación de snapshot |
| **Multi-puerto** | ⚠️ Parcial | Buffer compartido entre todos los puertos (`kMaxPorts=4`). Plugins multi-output producen salida incorrecta en bandas secundarias. |
| **Persistencia de estado** | ✅ Operativo | `StringIStream`/`StringOStream` adapters; blobs en base64 |
| **GUI (XEmbed)** | ✅ Operativo | Ventana X11 independiente via `clap_host_gui`; dispatch `gui_closed` a main thread |
| **Extensiones CLAP implementadas** | ✅ | `audio-ports`, `state`, `gui`, `thread-check`, `timer-support`, `log`, `params` (con flush real), `render` (offline), `note-ports`, `latency` |
| **Parameter flush** | ✅ Operativo | `request_flush` procesado en `processPendingFlushes()` con llamada real a `clap_plugin_params_t::flush()` |
| **Eventos MIDI/nota** | ✅ Operativo | `clap_input_events_t` genera eventos `clap_event_note_on/off` desde `MidiNote` snapshots. `clap_note_ports` extension. |

### 1.3 Persistencia y Archivos

| Componente | Estado | Detalle |
|---|---|---|
| **Guardado JSON** | ✅ Operativo | `save()` serializa versión, BPM, timeSig, masterVolume, playhead, loop/punch range, auxBuses, tracks (nombre, volumen, pan, mute, solo, arm, freeze, clipStart, audioPath, audioFrames, fadeIn/Out, extraClips, automation lanes/points, sends, midiClips/notes, plugins con estado en base64) |
| **Parser JSON** | ✅ Operativo | `nlohmann/json` v3.11.3 via FetchContent. Reemplaza el parser artesanal. |
| **Escritura atómica** | ✅ Operativo | Escribe a `.tmp` → `rename()` al destino final |
| **Carga (Hydratación Secuencial Estricta)** | ✅ Operativo | `stop` → `drain` → `parse` → `rebuild` → `notify` → `start` |
| **Base64 para blobs** | ✅ Operativo | `Base64.h` header-only codec RFC 4648 |
| **Manejo de errores** | ✅ Operativo | WAV faltante → pista en silencio con warning. Plugin faltante → se salta con warning. JSON inválido → retorna false. |

### 1.4 Exportación Offline

| Componente | Estado | Detalle |
|---|---|---|
| **WAV 16-bit PCM** | ✅ Operativo | `WavWriter::write16bit()` con clamp [-1.0, 1.0] y escala 32767 |
| **WAV 32-bit float** | ✅ Operativo | `WavWriter::writeFloat32()` escritura raw |
| **Zenity dialog** | ✅ Operativo | Diálogo nativo para seleccionar ruta de guardado |
| **Export asíncrono** | ✅ Operativo | `nativeExportAudio` lanza detached thread; notifica `window.onExportDone()` |

### 1.5 Interfaz WebKit (DOM + JS)

| Componente | Estado | Detalle |
|---|---|---|
| **Layout Grid** | ✅ Operativo | `grid-template-rows: 44px 1fr 1fr` (transport | timeline + bottom) |
| **Transporte** | ✅ Operativo | Botones Play/Pause/Stop, Record, Loop, BPM editable, time signature, display `MM:SS.mmm`; Save/Load/Export; +Track/−Track; Undo/Redo |
| **Timeline (track list)** | ✅ Operativo | Filas de 48px con header (nombre + M/S/A/R) y área de clip; múltiples clips por pista |
| **Canvas de forma de onda** | ✅ Operativo | 1 canvas por clip con audio; dibuja barras verticales desde peak cache |
| **Regla de tiempo** | ✅ Operativo | Canvas con marcas por segundo + grid de compases, playhead amarillo; clic para seeking |
| **Scroll horizontal** | ✅ Operativo | `input[type=range]` con transformación `translateX`; escala fija 80px/segundo |
| **Mixer** | ✅ Operativo | Canales de 72px con fader vertical, medidor CSS, lectura dB, botones M/S/A/R; Master con borde distintivo; sección de buses |
| **Cadena FX** | ✅ Operativo | Lista de plugins con índice, flechas de orden, bypass, toggle GUI, eliminar; drag-and-drop reorder; panel lateral con Vol/Pan + M/S/R/Sends |
| **Browser** | ✅ Operativo | Listado de directorios y archivos de audio; doble clic para navegar/cargar |
| **Popup plugins CLAP** | ✅ Operativo | Modal centrado con lista de plugins escaneados y botón de carga |
| **Soft Clipper GUI** | ✅ Operativo | Ventana independiente con slider de drive |
| **Drag & drop archivos** | ✅ Operativo | `text/uri-list` de WebKitGTK; carga en pista destino |
| **56 bindings nativos** | ✅ Operativo | Todos funcionales: transport (9), recording (2), track/clip (19), freeze (2), plugin (7), MIDI (3), undo/redo (2), file/project (9), system (3) |
| **Telemetría C++→JS** | ✅ Operativo | Push JSON vía `updateUIFromNative(JSON.parse(...))` cada 30ms; generación counter anti-stale; full rebuild tras load |
| **3 edit tools** | ✅ Operativo | Pointer (select/drag), Split (cuchilla), Fade (fade overlays). Tool selector en transporte. |
| **Fade overlays** | ✅ Operativo | Triángulos en esquinas de clip, width dinámico desde telemetría, gradient, cursor `ew-resize` en fade mode |
| **Design tokens CSS** | ✅ Operativo | 70+ custom properties; paleta carbón cálido, azul acento |

---

## 2. Análisis de Brechas (Gap Analysis)

### 2.1 Deuda Técnica Identificada

| Brecha | Archivo(s) | Severidad | Impacto |
|--------|-----------|-----------|---------|
| **Multi-puerto CLAP comparte buffer** | `PluginChain.cpp` | 🟠 Media | Plugins multi-output producen salida incorrecta en bandas secundarias |
| **`IAudioObserver` nunca cableado** | `src/Audio/IAudioObserver.h` | 🟢 Baja | Interfaz de push subscription diseñada pero nunca integrada. Polling 30ms funciona. |
| **`escapeJson` duplicado** | `ProjectSerializer.cpp` + `main.cpp` | 🟢 Baja | Código idéntico en dos lugares |
| **`SoftClipper.h` vs `processBuiltin()`** | `DSP/SoftClipper.h` + `PluginChain.cpp` | 🟢 Baja | Lógica duplicada |
| **`m_chainMutex` adquirido pero nunca lockeado** | `AudioEngine.h` | 🟢 Baja | Miembro muerto |
| **Buffer thread-local `tlsWork[512]`** | `PluginChain.cpp` | 🟢 Baja | Hardcodeado para 256×2 canales |
| **Carga WAV sincrónica** | `main.cpp` | 🟢 Baja | Bloquea la UI brevemente |
| **Escaneo CLAP sincrónico** | `main.cpp` | 🟢 Baja | Bloquea la UI mientras escanea |
| **Sin rutas relativas** | `ProjectSerializer.cpp` | 🟢 Baja | Paths absolutos en `.opndaw` |

### 2.2 Brechas Funcionales vs. DAW Profesional

| Brecha | Impacto UX | Estado actual |
|--------|-----------|---------------|
| **Piano roll (UI de edición MIDI)** | 🟠 Media | MIDI storage OK, bindings OK, sin piano-roll en HTML |
| **UI de parámetros de plugin** | 🟡 Media | Bindings `nativeGetPluginParams`/`nativeSetPluginParam` existen, sin sliders en DOM |
| **UI de automatización (lanes)** | 🟡 Media | Automation engine OK, sin editor de envelopes en DOM |
| **UI de loop/punch range editing** | 🟡 Media | Loop/punch engine OK, sin handles en ruler |
| **Zoom horizontal/vertical** | 🟡 Media | Escala fija 80px/segundo |
| **Color de pista** | 🟡 Media | Sin selector de color |
| **Monitor de CPU/tiempo real** | 🟡 Media | Sin medidor |
| **Ventanas plugin embebidas** | 🟡 Media | GUIs en ventanas X11 separadas |
| **Grabación overdub/punch-in-out UI** | 🟡 Media | Engine OK, sin controles visuales de punch |
| **Atajos de teclado configurables** | 🟢 Baja | Solo Space = Play/Pause |
| **Menús contextuales** | 🟢 Baja | Sin right-click |
| **Rutas relativas** | 🟢 Baja | Paths absolutos |

---

## 3. Trabajo Futuro

### Prioridad Media — Madurez Profesional

| # | Feature | Esfuerzo estimado |
|---|---------|-------------------|
| 1 | **Piano roll UI** (grid de teclas + notas rectangulares + dibujar/borrar) | 5-7 días |
| 2 | **UI de parámetros de plugin** (sliders/knobs genéricos en FX panel) | 3-4 días |
| 3 | **Editor de automatización** (lanes en timeline, puntos draggables) | 3-5 días |
| 4 | **Loop/punch range handles** (drag en ruler para ajustar loop/punch) | 1-2 días |
| 5 | **Zoom horizontal** (slider o rueda ratón para escala variable) | 1 día |
| 6 | **Zoom vertical** (altura de pista ajustable) | 1 día |
| 7 | **Ventanas plugin embebidas** (XEmbed dentro del panel FX) | 3-5 días |

### Prioridad Baja — Pulido

| # | Feature | Esfuerzo estimado |
|---|---------|-------------------|
| 8 | **Rutas relativas** en persistencia | 1 día |
| 9 | **Coloreado de pistas** | 0.5 días |
| 10 | **Atajos de teclado** | 1 día |
| 11 | **Menús contextuales** | 1 día |
| 12 | **Monitor de CPU** | 1 día |
| 13 | **Escaneo CLAP asíncrono** | 1-2 días |
| 14 | **Carga WAV asíncrona** | 1-2 días |
| 15 | **Multi-puerto CLAP correcto** | 1-2 días |
| 16 | **IAudioObserver push subscription** | 2-3 días |

---

## Resumen de Métricas

| Categoría | Líneas | Archivos | Estado general |
|-----------|--------|----------|----------------|
| **Core C++ (hydraw_core)** | ~3.800 | 14 | ✅ Maduro, lock-free |
| **Bridge C++ (hydraw_daw)** | ~2.700 | 1 | ✅ Funcional, 56 bindings |
| **UI WebKit (HTML+JS+CSS)** | ~3.000 | 1 | ✅ Funcional, gaps UI menores |
| **Documentación técnica** | ~500+ | 5 (SPECs) | ✅ Detallada |

**Bindings activos:** 56 | **Extensiones CLAP implementadas:** 10/12 | **Parser JSON:** nlohmann ✅ | **Grabación:** ✅ | **MIDI:** ✅ (storage, no piano-roll) | **Automation:** ✅ (engine, no UI editor)
