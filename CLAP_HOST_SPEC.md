# Hydraw DAW — CLAP Host Robustness Specification

> Spec para la Fase 1 ("Host Defensivo") y Fase 3 ("Logs Forenses") del plan de orquestación.
> Generado tras auditoría forense del código contra la spec oficial de CLAP 1.x.

## 1. Hallazgos verificados contra spec

| # | Hallazgo | Spec referenciada | Severidad |
|---|----------|-------------------|-----------|
| H1 | `PluginChain::process` usa `try_lock` y silencia audio si el lock está ocupado | `clap/plugin.h:process` [audio-thread] | **CRÍTICO** — timeline se queda pillado |
| H2 | `clap_host_request_resize` llama `gtk_window_resize` desde cualquier hilo | `clap/ext/gui.h:request_resize` thread-safe | **ALTO** — segfault con plugins JUCE/PeakEater |
| H3 | No se ofrece `clap.thread-check` | `clap/ext/thread-check.h` "highly recommended" | **ALTO** — plugins que asumen el check fallan |
| H4 | No se sigue el orden oficial de GUI (saltan `can_resize`/`adjust_size`) | `clap/ext/gui.h` doc pág GUI | **MEDIO** |
| H5 | `gui->closed` callback hace no-op, ignora `was_destroyed` | `clap/ext/gui.h:closed` thread-safe | **MEDIO** |
| H6 | `clap_host_log` callback es vacío | `clap/ext/log.h` | **MEDIO** — cero diagnóstica |
| H7 | `nativeScanClap` carga + cierra .clap en el main thread sin sandbox | práctica estándar | **MEDIO** — un plugin roto mata el DAW |
| H8 | `nativePluginLoad` hace `init` + `activate` síncronamente en main thread | `clap/plugin.h:init` [main-thread] | **BAJO** — congela la UI si el plugin es lento |

## 2. Arquitectura objetivo

### 2.1 Threading model

```
┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐
│   Main thread   │   │  Audio thread   │   │  UI update thr  │
│  (GTK + WebKit) │   │  (miniaudio cb) │   │   (telemetry)   │
├─────────────────┤   ├─────────────────┤   ├─────────────────┤
│ plugin->init    │   │ plugin->process │   │ host_get_ext()  │
│ plugin->activate│   │ chain->process  │   │ call g_wv->eval │
│ plugin->destroy │   │ (lock-free)     │   │                 │
│ plugin->gui.*   │   │                 │   │                 │
│ host_log()      │   │                 │   │                 │
└────────┬────────┘   └────────┬────────┘   └────────┬────────┘
         │                     │                     │
         └──── clap_host_request_resize() ──────────►
         │       (thread-safe, redirigido a main)    │
         │                                             │
         ▼                                             ▼
   g_main_context_invoke() ◄──────────────────────────
   (dispatch a main thread)
```

### 2.2 Cambios en el host

#### 2.2.1 PluginChain::process — sin locks, copia lock-free

```cpp
// ANTES (buggy):
if (!m_mutex.try_lock()) return;  // ⚠️ silencia audio
std::lock_guard<std::mutex> lock(m_mutex, std::adopt_lock);

// DESPUÉS (lock-free):
// - El estado compartido (PluginInstance*) se lee de un std::atomic<PluginInstance*>
//   o un std::shared_ptr<const PluginInstance>
// - No se toma ningún mutex desde el audio thread
// - La modificación desde main thread usa un generation counter o RCU
```

Usaremos **std::atomic<PluginInstance*> por slot** + un `std::vector<std::atomic<uint32_t>> generation` para detectar cambios. La lectura desde audio thread es 100% lock-free. La escritura desde main thread es un `compare_exchange` atómico.

#### 2.2.2 `clap_host_request_resize` — dispatch a main thread

```cpp
// DESPUÉS:
static bool clap_host_request_resize(const clap_host_t* host, uint32_t w, uint32_t h) {
    auto* payload = new ResizePayload(host->host_data, w, h);
    g_main_context_invoke(nullptr, [](void* ud) -> gboolean {
        auto* p = static_cast<ResizePayload*>(ud);
        // ... lookup, gtk_window_resize, delete p ...
        return G_SOURCE_REMOVE;
    }, payload);
    return true;  // async accept
}
```

Esto **requiere** que `host_data` siempre apunte a un handle estable, no a un puntero que pueda liberarse.

#### 2.2.3 `clap.thread-check` extension

```cpp
static bool clap_host_is_main_thread(const clap_host_t* h) {
    return g_isMainThread.load(std::memory_order_acquire);
}
static bool clap_host_is_audio_thread(const clap_host_t* h) {
    return ma_is_audio_thread(); // función de miniaudio si está disponible
                                    // o thread_local bool
}
```

Antes de cada llamada al plugin, el host verifica que estamos en el hilo correcto y loguea si no.

#### 2.2.4 Log callback real

```cpp
static void clap_host_log(const clap_host_t* host, clap_log_severity sev, const char* msg) {
    static const char* names[] = {"HOST","ERROR","WARNING","INFO","DEBUG"};
    int idx = std::clamp<int>((int)sev, 0, 4);
    log("CLAP", std::string(names[idx]) + ": " + (msg ? msg : "(null)"));
}
```

#### 2.2.5 `gui->closed` con flag respetado

```cpp
static void clap_host_gui_closed(const clap_host_t* host, bool was_destroyed) {
    if (!host) return;
    int handle = (int)((intptr_t)host->host_data & 0x7FFFFFFF);
    // Solo encolar el cleanup; si was_destroyed el host debe llamar gui->destroy()
    g_main_context_invoke(nullptr, [](void* ud) -> gboolean {
        int h = GPOINTER_TO_INT(ud);
        PluginGuiState stolen;
        if (!erasePluginGuiByHandle(h, &stolen)) return G_SOURCE_REMOVE;
        if (was_destroyed && stolen.gui && stolen.gui->destroy) {
            stolen.gui->destroy(stolen.plugin);
        }
        return G_SOURCE_REMOVE;
    }, GINT_TO_POINTER(handle));
}
```

#### 2.2.6 Scan seguro — `clap_probe.h`

Nuevo header `src/Plugin/ClapProbe.h` que:
- Carga el .clap en un **proceso hijo via `posix_spawn`** (fork+exec) o usa `dlopen` en un thread dedicado con timeout.
- Si el plugin crashea durante `entry->init`, el host detecta el crash y lo marca como "unsafe".
- Devuelve solo nombre + id al proceso principal.

**Fase 1 — implementación mínima viable:**
En lugar de sandbox, usar **try/catch + timeout watchdog** con un thread dedicado que:
- Hace `dlopen`
- Si el thread no responde en 5s, asume crash y marca unsafe
- El main thread espera con un timeout

### 2.3 Contrato de errores

| Operación | Error devuelto al JS | Acción del host |
|-----------|----------------------|-----------------|
| `dlopen` falla | `"false"` + log "PLUGIN" con path | sigue |
| `entry->init` retorna false | `"false"` + log "PLUGIN" con id | hace `destroy` + `dlclose` |
| `entry->init` crashea (SIGSEGV) | el plugin queda en `failed` state | JS lo ve en rojo, no aparece en scan |
| `activate` falla | log + `destroy` | sigue sin activarlo |
| `gui->create` falla | log + `destroy` GUI | sigue, no muestra GUI |
| `gui->set_parent` falla | log + `destroy` GUI | sigue |
| `gui->show` falla | log + `destroy` GUI | sigue |
| `plugin->process` retorna `CLAP_PROCESS_ERROR` | log + desactivar plugin | el plugin queda bypassed |

### 2.4 Logs forenses

Añadir a `log()`:
- `[CLAP] <nivel>: <mensaje>` cuando el plugin loguea
- `[PLUGIN] load <id> from <path> -> <ok|fail>`
- `[PLUGIN] init <id> -> <ok|fail>`  
- `[PLUGIN] activate <id> -> <ok|fail>`
- `[PLUGIN] process <id> returned <status>`
- `[PLUGIN] thread-check FAIL: <method> called from wrong thread`

## 3. Plan de implementación por fases

### Fase 1: Host Defensivo (PRIORIDAD MÁXIMA)
- [ ] H1: PluginChain::process lock-free con `std::atomic<PluginInstance*>` + generation
- [ ] H2: `clap_host_request_resize` via `g_main_context_invoke`
- [ ] H3: Implementar `clap_host_thread_check_t`
- [ ] H5: `clap_host_gui_closed` con dispatch + respeto de `was_destroyed`
- [ ] H6: `clap_host_log` real con relé a log()
- [ ] H4: Seguir orden oficial de GUI: añadir `get_preferred_api`, `can_resize`, `adjust_size`, `get_resize_hints`
- [ ] Validación: cada puntero de función del plugin se valida `!= nullptr` antes de llamar

### Fase 2: State & Parámetros async
- [ ] Implementar `clap_istream`/`clap_ostream` con streams de memoria
- [ ] `clap_plugin_state->load/save` desde un thread dedicado

### Fase 3: Logs forenses
- [ ] Cada fase del ciclo de vida del plugin loguea con timestamp
- [ ] JS muestra estado failed en rojo en la lista de plugins

## 4. Criterios de aceptación

- Cargar `lsp-plugins.clap` y abrir su GUI no debe colgar el DAW.
- Cargar un .clap inválido (fichero que no sea CLAP) no debe crashear.
- `kill -SIGKILL` durante `plugin->init` no debe dejar el main thread bloqueado (timeout en wait).
- El binario sigue compilando con 0 warnings.
