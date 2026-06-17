#pragma once
// IHostExtensions — pure provider of CLAP host extensions.
//
// Replaces the previous `extern const clap_host_gui_t s_hostGui`
// (and the related `s_hostTimer`, `s_hostLog`, `s_hostParams`,
// `s_hostThreadCheck` static globals) that were declared in the
// audio core but defined in the bridge. The audio core can now be
// built as a standalone static library with no link-time
// dependency on bridge code.
//
// CONTRACT:
//   - The provider MAY be null. The audio core treats null as
//     "this extension is not available" and returns null from
//     getExtension accordingly.
//   - `getExtension()` returns a const void* that matches the CLAP
//     contract for the requested extension id. Cast to the typed
//     struct (e.g. `clap_host_gui_t*`) by the caller.
//   - Returned pointers MUST remain valid for the lifetime of the
//     audio engine. The audio core stores them and the audio
//     thread dereferences them in plugin callbacks.
//   - Implementation MAY be called from any thread the plugin
//     chooses (audio, main, plugin worker, etc.). Bridge
//     implementations typically route to the main thread via
//     g_main_context_invoke for GTK work.

#include "clap/clap.h"

namespace hydraw {

class IHostExtensions {
public:
    virtual ~IHostExtensions() = default;

    // CLAP contract: returns the extension struct for the given
    // extension_id, or nullptr if not supported. The returned
    // pointer's lifetime is the lifetime of this provider.
    virtual const void* getExtension(const char* extension_id) const noexcept = 0;
};

} // namespace hydraw
