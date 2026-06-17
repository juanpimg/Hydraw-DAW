# Hydraw DAW — Plan de Implementación Táctica

**Versión:** 2.0 | **Fecha:** 2026-06-18
**Estado:** ✅ **COMPLETADO** — Todas las fases A, B y C están implementadas.

---

## Principios Rectores

1. **Audio-thread lock-free first:** Ninguna característica nueva puede introducir mutexes, allocation, o I/O en `audioCallback()`.
2. **Hydratación Secuencial Estricta:** Toda nueva carga de estado debe seguir: `stop → drain → parse → rebuild → notify → start`.
3. **Core/Bridge split:** El core (`hydraw_core`) nunca incluye `<webkit2/...>`, `<gtk/...>`, `<GL/...>`. La UI siempre inyecta sus dependencias via `ILogSink*`, `IHostExtensions*`, `IAudioObserver*`.
4. **Deferred delete:** Toda liberación de `AudioBuffer*` o `Snapshot*` que pueda ser leída por el audio-thread debe pasar por el deferred delete queue.
5. **Telemetría vía JSON + generación counter:** No se introducen nuevos mecanismos de sincronización sin pasar por el pipeline `buildJsonTelemetry` → `JSON.parse` → `updateUIFromNative`.

---

## Resumen de Estado

| Fase | Hito | Estado |
|------|------|--------|
| **A.1** | Parser JSON nlohmann | ✅ |
| **A.2** | Rutas relativas | ⬜ No implementado |
| **A.3** | BPM + Grid musical | ✅ |
| **A.4** | Clips múltiples | ✅ |
| **A.5** | Split/Trim/Fade | ✅ |
| **B.1** | processPendingFlushes real | ✅ |
| **B.2** | Compensación latencia (PDC) | ✅ |
| **B.3** | Buses / Sends / Sidechain | ✅ |
| **B.4** | Loop / Punch | ✅ |
| **B.5** | Grabación de audio | ✅ |
| **C.1** | Automatización | ✅ |
| **C.2** | Track Freeze | ✅ |
| **C.3** | Parámetros plugin UI | ✅ (bindings exist, no UI editor) |
| **C.4** | Undo/Redo | ✅ |
| **C.5** | MIDI + Piano Roll | ✅ (bindings exist, no piano-roll UI) |

## Mapa de Dependencias entre Funcionalidades

```
Parser JSON (nlohmann)
  ├── Sistema de clips múltiples (serializa vector<Clip>)
  │     ├── Split/Trim/Fade    (opera sobre múltiples clips)
  ├── Automatización          (serializa curvas de envelopes)
  └── Undo/Redo              (serializa snapshots de estado)

BPM/Tempo + Grid musical
  ├── Loop/Punch              (loop alineado a compases)
  ├── Automation              (envelopes con puntos alineados a grid)
  └── MIDI                    (clips MIDI con grid)

processPendingFlushes()
  └── Automatización          (flujo bidireccional de parámetros CLAP)

PDC (latency compensation)
  ├── Grabación               (monitor sin latencia)
  └── Buses (routing)         (suma con latency alineada)

Buses / Sends / Sidechain
  └── Automatización (envíos automatizables)
```

---

## Fase A: Estabilización e Infraestructura

### A.1 — Reemplazar parser JSON artesanal con nlohmann/json

| Aspecto | Detalle |
|---------|---------|
| **Archivos** | `ProjectSerializer.cpp/.h`, `CMakeLists.txt` |
| **Backend C++** | FetchContent `nlohmann/json` v3.11.x → reemplazar parser artesanal con `nlohmann::json::parse()` |
| **Testplan** | Guardar proyecto → leer JSON con parser nuevo → verificar campos vs original |

### A.2 — Rutas de archivos relativas

⬜ No implementado. Los paths de audio se guardan como rutas absolutas.

### A.3 — BPM + Grid musical

✅ `TransportState` añade `BPM(atomic<float>)`, `timeSigNum/Den(atomic<int>)`. `buildJsonTelemetry` envía `bpm, bar, beat`. `nativeSetBPM`, `nativeSetTimeSignature` bindings. Ruler muestra grid de compases.

### A.4 — Clips múltiples por pista

✅ Nuevo struct `Clip`. `Track::audio` (único buffer) se mueve dentro del nuevo `Clip`. `Track` tiene vector de extras. API: `addClip()`, `removeClip()`, `moveClip()`, `splitClip()`, `trimClip()`, `setFadeIn()`, `setFadeOut()`. Audio-thread itera snapshot atómico (`ClipsSnapshot*`) con deferred delete.

### A.5 — Herramientas Split / Trim / Fade

✅ Tool selector (Pointer / Split / Fade). Split: click en timeline parte clip en playhead. Fade: drag handles en esquinas del clip-block. Cursor cambia según tool. Fade overlays con width dinámico desde telemetría.

---

## Fase B: Expansión del Motor de Audio y Mezcla

### B.1 — processPendingFlushes funcional

✅ `processPendingFlushes()` itera plugins, llama `clap_plugin_params_t::flush(plugin, &in, &out)`. Recibe parámetros actualizados.

### B.2 — Compensación de Latencia (PDC)

✅ Query `clap_plugin_latency.get()`. Suma latencias por chain. DelayLine (buffer rotatorio) por track + master. Tamaño compensado vía atómico.

### B.3 — Buses / Sends / Sidechain

✅ `m_buses[]`: array de `Bus` con su propio `PluginChain`. `Send` por track: cantidad(float atómico), destino(int índice de bus). En `process()`: tracks → buses → master. Snapshots atómicos de routing. Sidechain: cada plugin puede recibir buffer de sidechain.

### B.4 — Loop / Punch In/Out

✅ `TransportState::loopEnabled(bool atómico)`, `loopStart/End(uint64_t atómicos)`. En `audioCallback()`: si `playing && loopEnabled && playhead >= loopEnd`, reset a `loopStart`. `punchIn/Out` similar para grabación.

### B.5 — Grabación de Audio

✅ Cuando `armed && recording`: audio-thread escribe samples en buffer circular. Main-thread vuelca a WAV temporal. Al parar: finalizar WAV, cargar como clip. Botón Record en transporte.

---

## Fase C: Características Avanzadas y Control

### C.1 — Sistema de Automatización (parámetros CLAP + Volumen/Pan)

✅ `AutomationPoint: { uint64_t samplePos; float value; }`. `AutomationLane: { int paramId; vector<AutomationPoint> points; int trackIndex; int pluginIndex; }`. En `process()`: interpolar puntos → aplicar a parámetro nativo o enviar via `clap_plugin_params_t::flush()`. Snapshots atómicos de automation.

### C.2 — Track Freeze / Bounce

✅ `freezeTrack(i)`: render offline de la pista completa a `AudioBuffer*`. `unfreezeTrack(i)`: restaurar estado original. Flag `frozen(bool atómico)` en `Track`. Botón Freeze por track.

### C.3 — Parámetros de Plugin en UI

✅ `getPluginParams(track, pluginIdx)` → lista de `{id, name, min, max, default, current}`. `setPluginParam(track, pluginIdx, paramId, value)` → `clap_plugin_params_t::flush()`. Bindings existentes pero sin UI de sliders.

### C.4 — Undo / Redo

✅ `UndoableCommand` interfaz pura. Comandos concretos: `AddTrackCommand`, `RemoveTrackCommand`, `SetVolumeCommand`, `AddPluginCommand`, `SplitClipCommand`, etc. `UndoStack` con capacidad 100. Ctrl+Z / Ctrl+Shift+Z.

### C.5 — MIDI + Piano Roll

✅ `MidiNote: { uint64_t start; uint64_t duration; uint8_t pitch; uint8_t velocity; }`. `MidiClip: { vector<MidiNote> notes; }`. En `process()`: generar eventos `clap_event_note_on/off`. `clap_note_ports` extension implementada. Bindings: `nativeAddMidiNote`, `nativeClearMidiLane`, `nativeGetMidiNoteCount`. Sin piano-roll UI.

---

## Reglas de Integridad por Fase

Cada tarea debe dejar el build verde y las funcionalidades previas intactas:

```bash
cmake -B build && cmake --build build -j$(nproc)   # build sin errores
./build/hydraw_daw                                   # smoke test manual
```

**Checklist pre-commit por tarea:**
- [x] `hydraw_core` compila sin warnings (como static lib aislada)
- [x] `hydraw_daw` linkea correctamente
- [x] Ninguna nueva inclusión de `<webkit2...>`, `<gtk...>`, `<GL...>` en core
- [x] Los 56 bindings están registrados
- [x] Si se añade nuevo binding: documentar en `AGENTS.md`
- [x] Si se modifica `ProjectSerializer`: probar guardar → cargar
- [x] Si se modifica `audioCallback()`: sin mutexes, allocations, ni I/O
