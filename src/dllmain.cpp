#include <windows.h>
#include "app.h"
#include "config.h"
#include "log.h"

void proxy_init();
void install_hooks(uintptr_t base);
void start_tray();

static DWORD WINAPI init_thread(LPVOID) {
    // 主模块(GameViewer.exe)在进程创建时即已映射, 可立即 hook
    uintptr_t base = 0;
    for (int i = 0; i < 200 && !base; ++i) {       // 最多等 ~10s
        base = (uintptr_t)GetModuleHandleW(L"GameViewer.exe");
        if (!base) Sleep(50);
    }
    if (!base) { uu_log("GameViewer.exe module not found, abort"); return 0; }
    uu_log("v%s, GameViewer.exe base = %p", UURE_VERSION, (void*)base);
    cfg::load();
    install_hooks(base);
    start_tray();
    uu_log("init done. viewOnly=%d clipSync=%d gamepadOff=%d",
           (int)cfg::g_viewOnly.load(), (int)cfg::g_clipSync.load(), (int)cfg::g_gamepadOff.load());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        proxy_init();                              // 先把真实 version.dll 转发就绪
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
