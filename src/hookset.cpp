#include "hookset.h"
#include <windows.h>
#include "MinHook.h"
#include "log.h"

namespace hookset {

bool install_export(const wchar_t* moduleName, const char* proc, void* detour, void** orig, IRecorder& rec) {
    HMODULE m = GetModuleHandleW(moduleName);
    if (!m) m = LoadLibraryW(moduleName);
    void* p = m ? (void*)GetProcAddress(m, proc) : nullptr;
    bool ok = p && MH_CreateHook(p, detour, orig) == MH_OK && MH_EnableHook(p) == MH_OK;
    if (ok) uu_log("hooked %s @ %p (exp)", proc, p);
    else    uu_log("hook %s failed (exp)", proc);
    rec.record(proc, nullptr, "exp", ok);
    return ok;
}

int install(const resolver::ModRange& r, const Hook* hooks, int count, IRecorder& rec) {
    int ok = 0;
    for (int i = 0; i < count; ++i) {
        const Hook& h = hooks[i];
        const char* anchors[4]; int n = 0;
        for (int k = 0; k < 4 && h.anchors[k]; ++k) anchors[n++] = h.anchors[k];
        uintptr_t tgt = n ? resolver::find_func(r, anchors, n) : 0;
        if (!tgt) { uu_log("resolve %s failed, skip", h.name); rec.record(h.name, nullptr, "", false); continue; }
        if (MH_CreateHook((void*)tgt, h.detour, h.orig) != MH_OK) { uu_log("CreateHook %s failed", h.name); rec.record(h.name, (void*)tgt, "str", false); continue; }
        if (MH_EnableHook((void*)tgt) != MH_OK) { uu_log("EnableHook %s failed", h.name); rec.record(h.name, (void*)tgt, "str", false); continue; }
        uu_log("hooked %s @ %p (str)", h.name, (void*)tgt);
        rec.record(h.name, (void*)tgt, "str", true);
        ++ok;
    }
    return ok;
}

}
