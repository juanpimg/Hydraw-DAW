#pragma once
#include "clap/clap.h"
#include <dlfcn.h>
#include <cstring>
#include <unordered_map>
#include <string>

struct ClapHost {
    static bool isClapLibrary(const char* path) {
        const char* ext = std::strrchr(path, '.');
        if (!ext) return false;
        return std::strcmp(ext, ".clap") == 0;
    }

    // CLAP's entry->init / entry->deinit are a per-library lifecycle, not
    // per-plugin. When two plugins come from the same .clap (e.g. multiple
    // instances from lsp-plugins.clap), calling init() a second time on
    // the already-initialized library corrupts the plugin's internal
    // state and create_plugin() starts returning null. We keep a refcount
    // keyed by the library path: init on the first open, deinit when the
    // last reference is released. dlopen/dlclose still happen every time
    // (they're just refcounted by libdl).
    struct LibState {
        int refcount = 0;
        bool initialized = false;
    };
    static std::unordered_map<std::string, LibState>& libStates() {
        static std::unordered_map<std::string, LibState> s;
        return s;
    }

    static void* loadLibrary(const char* path) {
        void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!lib) return nullptr;

        const clap_plugin_entry_t* entry =
            (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
        if (!entry) {
            dlclose(lib);
            return nullptr;
        }

        auto& state = libStates()[path];
        if (!state.initialized) {
            if (entry->init) entry->init(path);
            state.initialized = true;
        }
        state.refcount++;
        return lib;
    }

    static void unloadLibrary(void* lib, const char* path) {
        if (!lib) return;
        const clap_plugin_entry_t* entry =
            (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
        if (path) {
            auto& states = libStates();
            auto it = states.find(path);
            if (it != states.end()) {
                it->second.refcount--;
                if (it->second.refcount <= 0) {
                    if (entry && entry->deinit) entry->deinit();
                    states.erase(it);
                }
            }
        }
        dlclose(lib);
    }

    static uint32_t getPluginCount(void* lib) {
        const clap_plugin_entry_t* entry =
            (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
        if (!entry) return 0;
        const clap_plugin_factory_t* factory =
            (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        if (!factory || !factory->get_plugin_count) return 0;
        return factory->get_plugin_count(factory);
    }

    static const clap_plugin_descriptor_t* getPluginDescriptor(void* lib, uint32_t index) {
        const clap_plugin_entry_t* entry =
            (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
        if (!entry) return nullptr;
        const clap_plugin_factory_t* factory =
            (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        if (!factory || !factory->get_plugin_descriptor) return nullptr;
        return factory->get_plugin_descriptor(factory, index);
    }

    static const clap_plugin_t* createPlugin(void* lib, const clap_host_t* host,
                                             const char* pluginId) {
        const clap_plugin_entry_t* entry =
            (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
        if (!entry) return nullptr;
        const clap_plugin_factory_t* factory =
            (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        if (!factory || !factory->create_plugin) return nullptr;
        return factory->create_plugin(factory, host, pluginId);
    }

    static void destroyPlugin(const clap_plugin_t* plugin) {
        if (plugin && plugin->destroy) plugin->destroy(plugin);
    }
};
