# Hydraw DAW — Persistence Pipeline Spec

> Definitive spec for save/load of `.opndaw` projects. Codifies the
> findings of the May 2026 forensic audit and the consensus reached in
> the multi-agent debate.

## Goals

1. **Atomicity**: a crash mid-save never leaves a half-written file.
2. **Symmetry**: `load(p); save(p)` is the identity function (up to
   non-persisted fields like peak meters, transport state).
3. **Thread safety**: the audio thread never sees torn plugin/audio state.
4. **Fault tolerance**: missing audio files or missing plugins must
   not abort the load — the affected element is skipped and surfaced
   to the UI as a warning.
5. **No silent data loss**: every persisted field is restored or
   explicitly defaulted. Failed extraction is logged, not swallowed.

## Format

`.opndaw` is a UTF-8 JSON file (parsed with `nlohmann/json` v3.11.3)
with the following shape:

```json
{
  "version": 1,
  "masterVolume": 0.85,
  "bpm": 120.0,
  "timeSigNum": 4,
  "timeSigDen": 4,
  "playhead": 0,
  "loopEnabled": false,
  "loopStart": 0,
  "loopEnd": 441000,
  "punchIn": 0,
  "punchOut": 441000,
  "auxBuses": [
    { "name": "Reverb", "volume": 1.0, "muted": false, "plugins": [] }
  ],
  "master": {
    "plugins": [
      { "path": "/…/PeakEater.clap", "id": "studio.kx.distortion",
        "bypassed": false, "state": "<base64>" }
    ]
  },
  "tracks": [
    { "name": "Drums", "volume": 1.0, "pan": 0.0,
      "muted": false, "soloed": false, "armed": false,
      "frozen": false,
      "clipStart": 0, "audioPath": "/…/drums.wav",
      "audioFrames": 1234567,
      "fadeIn": 0, "fadeOut": 0,
      "extraClips": [
        { "path": "/…/drums_loop.wav", "start": 441000,
          "frames": 220500, "fadeIn": 100, "fadeOut": 200 }
      ],
      "automation": [
        { "paramId": "volume", "points": [
          { "pos": 0, "value": 0.8 },
          { "pos": 44100, "value": 1.0 }
        ]}
      ],
      "sends": [
        { "bus": 0, "level": 0.5 }
      ],
      "midiClips": [
        { "notes": [
          { "start": 0, "duration": 22050, "pitch": 60, "velocity": 100 }
        ]}
      ],
      "plugins": [
        { "path": "…", "id": "…", "bypassed": false, "state": "<base64>" }
      ]
    }
  ]
}
```

**Binary safety**: plugin state blobs are **base64-encoded** to
prevent JSON parser corruption from raw `0x22`/`0x5C` bytes in the
binary. The size penalty (~33%) is acceptable for correctness.

## Save pipeline

1. `save(path, engine)` writes the JSON string to `path + ".tmp"`.
2. `rename(".tmp", path)` atomically replaces the previous file.
3. State blobs are gathered under the **plugin chain mutex**, which
   serializes against `addPlugin`/`removePlugin`/loadState.

## Load pipeline (Strict Sequential Hydration)

The load is the inverse of save but requires strict ordering because
the audio thread, the UI thread, and the main thread can all be
touching the affected state.

### Phase 0 — Stop & drain
1. `engine->stop()` — pause the audio device. No more audio callbacks.
2. `PluginChain::drainPendingDeletes()` — free queued snapshots.
3. `engine->drainPendingAudioBufferDeletes()` — free queued audio
   buffers from previous sessions.

### Phase 1 — Read & parse
1. Read file fully into memory. Validate UTF-8 BOM and strip if present.
2. Parse JSON with `nlohmann::json::parse()`. No hand-rolled escaping.
3. If parse fails → return false, no state is modified.

### Phase 2 — Configure master
1. Stop and destroy the preloaded master chain (PeakEater).
2. Add master plugins from the file.
3. Restore bypass + state.

### Phase 3 — Configure tracks
For each saved track, in order:
1. Ensure track exists in the engine (`addTrack` if needed).
2. Set name, volume, pan, mute, solo, arm, freeze.
3. **Load audio**: `loadWavToTrack(i, path)`. The function:
   - decodes the file (synchronous, acceptable per spec for now),
   - stores the new `AudioBuffer*` atomically,
   - **stores `audioFilePath`** so the next save round-trips.
4. **Set `clipStart`** after `loadWavToTrack` returns.
5. Set fadeIn, fadeOut.
6. Load extra clips (paths, starts, fades).
7. Load MIDI clips and notes.
8. Load automation lanes and points.
9. Load sends routing.
10. Add plugin chain entries, restore bypass + state.

### Phase 4 — Configure aux buses and global state
1. Restore aux buses (name, volume, mute, plugins).
2. Restore global state: BPM, time signature, master volume.
3. Restore loop/punch range.
4. Clamp `playhead` to `[0, projectDuration]`.
5. Restore master volume.

### Phase 5 — Notify UI
1. `g_uiDirty = kDirtyExtended` (full refresh).
2. Emit `window.__onProjectLoaded()` so the JS side
   can invalidate its optimization caches (`_lastPluginSel`,
   `_clipNames`, `peakCaches`, `_lastRenderedAudioFrames`).

### Phase 6 — Resume
1. `engine->start()` — resume the audio device.

## Concurrency model

- `engine->m_chainMutex` is the **save/load serialization point**.
  `save()` and `load()` both acquire it as writers. Plugin chain
  mutations (add/remove/loadState) acquire it as writers too.
  Audio thread reads are atomic and require no lock.
- The audio thread never sees torn state because:
  - `m_pluginChains` entries are `std::unique_ptr<PluginChain>`.
  - During `load()` the audio device is stopped, so no thread
    dereferences the array.
  - After `load()` returns, all entries are valid `unique_ptr`s.
- The deferred-delete queue is drained at the end of every audio
  block. `load()` additionally drains the queue in Phase 0.

## Per-chain `s_lastSnapGen`

The `s_lastSnapGen` thread-local cache is per-chain, not global.
A single global cache invalidates itself on every chain's `process()`
call, defeating the purpose of the cache. Each `PluginChain` carries
its own atomic generation counter; the audio thread caches the last
seen per-chain generation in a `unordered_map<PluginChain*, uint32_t>`.

## Frontend invariants

- `STATE._lastPluginSel` is reset to `-2` on every `__onProjectLoaded`.
- `STATE._clipNames` is reset to `[]` on every `__onProjectLoaded`.
- `STATE.peakCaches` is reset to `[]` on every `__onProjectLoaded`.
- `STATE._lastRenderedAudioFrames` is reset on every `__onProjectLoaded`.
- `clipStarts` array is padded with zeros to match `trackCount`
  (WebKitGTK trailing-zero bug).
- Save/load success/failure shows a status toast.

## Error handling

| Failure                     | Behavior                          |
|----------------------------|-----------------------------------|
| File does not exist        | Return false.                     |
| File cannot be read        | Return false.                     |
| JSON parse error           | Return false, no state modified.  |
| Audio file missing         | Track kept silent, `audioPath=""`, `audioFrames=0`, warning logged. |
| Plugin .clap missing       | Plugin skipped, warning logged.   |
| Plugin has no state ext.   | `state` field omitted (no warning). |
| `state.load` returns false | Plugin kept, no state restored, warning logged. |

All warnings are also surfaced to the user via the JS log and a
single consolidated warning toast at the end of the load.

## Out of scope (future work)

- Async WAV loading (worker thread).
- Relative audio paths (currently absolute).
- Bundling audio inside `.opndaw` (zip container).
- Schema migration between versions.

## C++ → JS telemetry: JSON serialization (NOT array-literal eval)

### Problem

The C++ telemetry push (`updateUIFromNative`) used to build a giant JS
array literal as a single string and pass it to `g_wv->eval(...)`:

```
updateUIFromNative([1,0,221440,3,0,0,1,[0,0,0],[1,0,1],[0,0,0],[0,0,0],[0,0,0],["","Track 2","Track 3"],[7843143,7843143,1440000],[0,46800,15600])
```

This was observed to be **fragile in WebKitGTK's JavaScriptCore**.
The JSC `eval()` parser has known bugs with nested integer arrays
(zero-trailing, large integers, leading-zero patterns). The forensic
audit (June 2026) showed C++ emitting `audioFrames=[7843143,7843143,1440000]`
correctly, while JS received `audioFrames=[0,0,0]`. The `names` array
worked because it was an array of strings, not integers.

### Solution

**Build a proper JSON string on the C++ side and `JSON.parse` it on
the JS side.** JSON.parse is a well-defined code path in JSC and
does not suffer from the array-literal parser bugs.

C++ side (in `uiUpdateLoop`):
```cpp
// Build JSON: {"playing":0,"playhead":0,"trackCount":3,"masterVolume":1.0,
//              "peaks":[0,0,0],"volumes":[1,1,1],"mutes":[0,0,0],
//              "solos":[0,0,0],"arms":[0,0,0],"names":["","Track 2","Track 3"],
//              "audioFrames":[7843143,7843143,1440000],
//              "clipStarts":[0,46800,15600]}
std::ostringstream js;
js << "updateUIFromNative(JSON.parse('";
js << buildJsonPayload(state);  // returns valid JSON
js << "'))";
g_wv->dispatch([jsStr = js.str()]() {
    if (g_wv) g_wv->eval(jsStr);
});
```

JS side (in `updateUIFromNative`):
```js
function updateUIFromNative(obj) {
  // obj is now a proper object parsed by JSON.parse
  var isFull = obj.full === 1;
  STATE.playing = !!obj.playing;
  // ...
  STATE.audioFrames = obj.audioFrames || [];
  // ...
}
```

The `buildJsonPayload` function in C++ uses `escapeJson` (already
present) to escape string fields properly. Numbers are emitted as
JS-number-safe integers (within 2^53).

### Benefits

1. **No more JSC array-literal parser bugs.** JSON.parse is a single
   tokenizer with a well-defined grammar.
2. **No need for the "trailing zero" workaround** in JS. JSON.parse
   preserves exact element count.
3. **Easier to extend.** New fields can be added without re-engineering
   the array-index protocol.
4. **Forward-compatible.** If we add new telemetry fields, the JS
   side can use `obj.newField || defaultValue` without breaking.

### Field name → index migration

The old positional array (a[0]..a[14]) is replaced with a named
object. The JS-side rebuild check still works on the named fields:
```js
var _curAudio = STATE.audioFrames || [];
var _needRebuild = _forceFullRebuild
  || (_prevCount === undefined)
  || (_prevCount !== _curCount)
  || (isFull && audioFramesDiffer(_prevAudio, _curAudio));
```
