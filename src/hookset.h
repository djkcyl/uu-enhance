#pragma once
#include <cstdint>
#include "resolver.h"

namespace hookset {
    struct IRecorder {
        virtual void record(const char* name, void* addr, const char* how, bool ok) = 0;
    };

    struct Hook {
        const char* name;
        const char* anchors[4];
        void*       detour;
        void**      orig;
    };

    int install(const resolver::ModRange& r, const Hook* hooks, int count, IRecorder& rec);

    // Hook an exported WinAPI by name (module loaded on demand). how="exp".
    bool install_export(const wchar_t* moduleName, const char* proc, void* detour, void** orig, IRecorder& rec);

    // Hook an already-resolved address (caller did its own custom resolution). Centralizes MH + record.
    bool install_at(void* target, const char* name, const char* how, void* detour, void** orig, IRecorder& rec);
}
