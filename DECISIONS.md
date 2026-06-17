# Registro de Arquitectura (ADR)

## ADR-000: Formato de este documento

**Contexto**: Necesitamos un formato estandarizado para registrar las decisiones arquitectónicas del proyecto Hydraw DAW, de modo que sean trazables y entendibles por cualquier persona que se incorpore al desarrollo.

**Decisión**: Usamos el formato ADR (Architecture Decision Record) ligero con tres secciones obligatorias:

- **Contexto**: Describe el problema, motivación y restricciones.
- **Decisión**: La alternativa concreta que se adopta.
- **Consecuencias**: Efectos positivos y negativos de la decisión.

Cada registro se numera secuencialmente (ADR-NNN) y tiene un título descriptivo.

**Consecuencias**: Documentación liviana y fácil de mantener. No requiere herramientas externas. Se actualiza añadiendo nuevas entradas al final.

---

## ADR-001: Arquitectura General

**Contexto**: Necesitamos un DAW ágil, moderno, open-source para Linux (con miras a multiplataforma). Debe priorizar flujo rápido tipo Ableton Live / BandLab.

**Decisión**: Arquitectura modular con motor de audio en hilo separado, UI en hilo principal con ImGui, comunicación vía estructuras atómicas compartidas (sin locks en el callback de audio).

**Consecuencias**:
- Posibles glitches si el hilo de UI bloquea, mitigado con atomics y doble buffer.
- Separación clara entre presentación y procesamiento de audio.
- Facilita la extensión con nuevos módulos (MIDI, VST3) en el futuro.

---

## ADR-002: Motor de Audio (miniaudio vs RtAudio)

**Contexto**: Se requiere streaming de audio de baja latencia con soporte ALSA/PulseAudio/JACK.

**Decisión**: Se elige **miniaudio** (cabecera única, sin dependencias adicionales, más simple de integrar, licencia MIT, soporta los 3 backends).

**Consecuencias**:
- Menos control fino que RtAudio, pero mucha más facilidad de integración y despliegue.
- No requiere compilar bibliotecas de audio externas; se copia el header y ya funciona.
- Al ser cabecera única, se acelera el bootstrap del proyecto.

---

## ADR-003: Interfaz Gráfica (ImGui + GLFW + OpenGL3)

**Contexto**: UI debe ser rápida, renderizada por GPU, single-window layout con 3 regiones.

**Decisión**: Se usa **Dear ImGui** con backend GLFW + OpenGL3. Layout fijo con `ImGui::BeginChild` en 3 paneles.

**Consecuencias**:
- UI inmediata y rápida de prototipar.
- No es nativa, pero es multiplataforma y GPU-accelerada.
- Layout fijo sin dockings, funcional para V1; se puede migrar a docking en el futuro si es necesario.

---

## ADR-004: Concurrencia y Modelo de Hilos

**Contexto**: El callback de audio no debe ser bloqueado por la UI.

**Decisión**: Dos hilos:

1. **Hilo principal** = UI loop (GLFW event loop + ImGui rendering).
2. **Hilo secundario** = audio callback (proporcionado por miniaudio).

El audio callback escribe/lee datos desde variables atómicas (`std::atomic`) y un búfer de doble muestra (`Track` estructuras). La UI solo lee.

**Consecuencias**:
- Sin locks en el audio callback, evitando priority inversion y glitches.
- La UI puede leer datos desactualizados por un frame, pero eso es aceptable para medidores/VU.
- Modelo probado y predecible para un DAW mono-hilo en UI.

---

## ADR-005: Layout de UI (4 Regiones — Timeline + Mixer)

**Contexto**: El espacio de trabajo debe maximizar el área de edición y ofrecer un mixer visual por pistas.

**Decisión**: División en 4 regiones vía `ImGui::BeginChild`:
- **Superior** (100% ancho, 55% alto): Timeline con pistas como filas horizontales scrolleables, con controles M/S/A por pista y ruler de tiempo.
- **Inferior-Izquierda** (15% ancho): File Browser básico (árbol de directorios).
- **Inferior-Centro** (55% ancho): FX Chain de la pista seleccionada.
- **Inferior-Derecha** (30% ancho): Mixer con faders verticales, medidores de nivel y botones M/S/A por canal.

**Consecuencias**:
- Layout fijo sin dockings. Simple pero funcional para V1.
- El mixer y timeline comparten la selección de pista activa.
- Las dimensiones relativas permiten escalado con el tamaño de la ventana.

---

## ADR-006: Estándar de Plugins (CLAP)

**Contexto**: Se requiere un sistema de plugins para procesar audio en cada pista.

**Decisión**: Se adopta **CLAP** (CLever Audio Plugin) como estándar de plugins.

**Razones**:
- Estándar abierto y gratuito (licencia MIT, sin fees).
- Diseñado por U-he, Plugin Alliance y Bitwig para ser moderno y simple.
- Soporte nativo en Linux (`.clap` = `.so` con dlopen).
- Thread-safe por diseño (el proceso de audio es claramente separado de la UI).
- Crecimiento rápido en la industria.

**Consecuencias**:
- No hay compatibilidad directa con VST3/LV2 en V1 (se puede añadir como bridge después).
- El hosting es más simple que VST3 (sin SDK propietario, sin COM).
- El ecosistema de plugins CLAP es más pequeño que VST3 por ahora.

---

## ADR-007: Reproducción de Audio y Modelo de Playback

**Contexto**: Se necesita reproducir archivos WAV cargados en pistas.

**Decisión**:
- Los datos de audio se cargan con `ma_decoder` (miniaudio) desde el hilo UI.
- El playhead es un `std::atomic<uint64_t>` en `TransportState`.
- El callback de audio lee el playhead y los datos de audio con atomics.
- Las pistas soportan tanto reproducción desde archivo como entrada en vivo (armed).

**Consecuencias**:
- No hay soporte de loop/punch in-out todavía (añadido en ADR-011).
- La carga de WAV es síncrona (bloquea la UI brevemente).

---

## ADR-008: Pistas Dinámicas y Bus Maestro

**Contexto**: El usuario debe poder añadir/eliminar pistas dinámicamente y tener un bus maestro.

**Decisión**:
- `m_trackCount` es `std::atomic<int>`.
- `addTrack()` incrementa el contador, `removeTrack()` despliega las pistas superiores.
- `MasterBus` es un struct separado con su propio `volume`/`pan` atómicos.

**Consecuencias**:
- Máximo 16 pistas (constante `MAX_TRACKS`).
- RemoveTrack desplaza pistas (cambio de índice).

---

## ADR-009: Migración de UI — ImGui/GLFW → WebView (WebKit2GTK)

**Contexto**: La UI con Dear ImGui + GLFW + OpenGL 3 era funcional pero se veía anticuada.

**Decisión**: Se reemplaza ImGui/GLFW/OpenGL por `webview::webview` (WebKit2GTK + GTK3). La UI pasa a ser HTML5+TailwindCSS+vanilla JS, comunicándose con C++ mediante bindings nativos (`w.bind`/`eval`).

**Consecuencias**:
- UI profesional con HTML/CSS/JS, sin toolchain frontend (Tailwind CDN).
- WebKit2GTK requiere `libwebkit2gtk-4.1-dev`.
- Binding sin `JSON.parse()` en JS — webview ya devuelve JS nativo.
- Se añade `g_pageReady` guard para evitar eval() antes del DOMContentLoaded.
- Buena práctica: `safeStoi`/`safeStof` en bindings para evitar crashes.

---

## ADR-010: UI Dark Theme Profesional (Ableton‑inspirado)

**Contexto**: La UI original usaba colores planos con contraste insuficiente.

**Decisión**: Diseño oscuro con paleta de alto contraste basada en Ableton Live/Bitwig:
- Fondo `#141418` (no negro puro).
- Texto primario `#e4e4e8`, secundario `#a0a0b0`, terciario `#68687a`.
- Tipografía sistema sans-serif, tamaños +1-2px.
- Colores funcionales: azul `#5599ff`, verde `#44bb66`, amarillo `#eebb44`, rojo `#ee4444`.

**Consecuencias**:
- WCAG AA para todo texto (≥4.5:1).
- Waveform canvas usa `rgba(130,150,190,0.6)`.
- Layout más espacioso (transport 44px, tracks 48px).

---

## ADR-011: Expansión del Motor — Multi-Clip, Buses, Loop, Grabación, MIDI

**Contexto**: Tras la migración a WebView (ADR-009), se necesitaban funcionalidades DAW esenciales ausentes: múltiples clips por pista, herramientas de edición (split/trim/fade), loop/punch, grabación de audio, buses/sends, automatización, congelamiento de pistas, undo/redo, y soporte MIDI.

**Decisión**: Se implementa un modelo de multi-clip con snapshot atómico (`ClipsSnapshot*`) que publica el vector de clips extras para el audio thread. Se añaden 23 nuevos bindings nativos (56 total). Se integra `nlohmann/json` v3.11.3 para el parser de proyectos. Se implementa `processPendingFlushes` real con `clap_plugin_params_t::flush`. El pipeline de grabación usa buffer circular con flush asíncrono a disco. Los buses/sends usan matriz de mezcla con snapshot atómico de routing. La automatización usa `AutomationSnapshot*` con búsqueda binaria de puntos. Freeze usa render offline por pista. Undo/Redo usa Command pattern con historial de 100 operaciones. MIDI usa `clap_note_ports` con `MidiNote` snapshots.

**Consecuencias**:
- El número de bindings creció de 33 a 56, lo que hace urgente una refactorización (`BridgeController` class, Phase 7 de ARCHITECTURE_SPEC.md).
- La telemetría JSON se volvió más compleja (arrays de clips, automation, MIDI, buses).
- Se eliminó la limitación 1-clip-por-pista.
- `nlohmann/json` eliminó el riesgo de corrupción del parser artesanal.
- Todas las fases A, B y C del roadmap están completadas.

---

## ADR-012: Telemetría via JSON.parse en lugar de array-literal eval

**Contexto**: La telemetría C++→JS usaba un array literal JS gigante en `eval()`. WebKitGTK's JSC tenía bugs con arrays anidados de enteros grandes, causando que `audioFrames` llegara como `[0,0,0]` en lugar de los valores reales.

**Decisión**: Reemplazar el array-literal `eval` con `JSON.parse()` de un string JSON construido en C++. Se añade `buildJsonTelemetry()` que produce JSON válido. Se introduce `g_telemetryGen` (generation counter) para que JS descarte actualizaciones completas (full=true) obsoletas de cargas de proyecto anteriores.

**Consecuencias**:
- Los waveforms se renderizan correctamente tras cargar un proyecto.
- No más bugs de parser JSC con arrays anidados.
- La telemetría es extensible: nuevos campos se añaden al objeto JSON sin cambiar el protocolo de índices.
- JS usa `obj.field || default` en lugar de `a[14]`.
- Generación counter evita actualizaciones fantasma tras load.
