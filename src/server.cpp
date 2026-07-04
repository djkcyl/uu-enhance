#include <windows.h>
#include <sddl.h>
#include <string>
#include <cmath>
#include "MinHook.h"
#include "config.h"
#include "log.h"
#include "resolver.h"
#include "xdisasm.h"
#include "hookset.h"
#include "srvdbg.h"
#include "prts_cursor.h"

static void note_block(const char* what);

using fn_gvsend_t = UINT   (__fastcall*)(UINT, void*, int, char);
using fn_send1_t  = __int64(__fastcall*)(void*);
using fn_sendinput_t = UINT(WINAPI*)(UINT, LPINPUT, int);
using fn_gpupd_t  = __int64(__fastcall*)(void*, void*);
using fn_gpconn_t = __int64(__fastcall*)(void*, unsigned __int64);
using fn_setres_t = __int64(__fastcall*)(void*, unsigned, unsigned);
using fn_setref_t = __int64(__fastcall*)(void*, unsigned);
using fn_ev_t     = __int64(__fastcall*)(void*, void*);
using fn_ev1_t    = __int64(__fastcall*)(void*);
using fn_setpriv_t = __int64(__fastcall*)(unsigned*, unsigned, unsigned, __int64, __int64,
                                          unsigned char, unsigned char, __int64);

static fn_gvsend_t   o_gvInputSend = nullptr;
static fn_send1_t    o_mouseSend   = nullptr;
static fn_send1_t    o_kbdSend     = nullptr;
static fn_sendinput_t o_SendInput  = nullptr;
static fn_gpupd_t    o_gpUpdate    = nullptr;
static fn_gpconn_t   o_gpConnect   = nullptr;
static fn_setres_t   o_setRes      = nullptr;
static fn_setres_t   o_setResAsync = nullptr;
static fn_setref_t   o_setRef      = nullptr;
static fn_setref_t   o_setDpiAsync = nullptr;
static fn_ev_t       o_recvFile    = nullptr;
static fn_ev_t       o_dragPaste   = nullptr;
static fn_ev1_t      o_micDefault  = nullptr;
static fn_setpriv_t  o_setPrivacy  = nullptr;
static fn_ev1_t      o_privLock    = nullptr;
using fn_mute_t   = void(__fastcall*)(void*, void*);
static fn_mute_t     o_doSetMute   = nullptr;
using fn_mkvd_t   = __int64(__fastcall*)(void*, __int64);
using fn_mss_t    = __int64(__fastcall*)(void*, int, int);
static fn_mkvd_t     o_mkVirtualDisp   = nullptr;
static fn_mss_t      o_enterSuperScreen= nullptr;
using fn_v0_t     = __int64(__fastcall*)();
using fn_reboot_t = __int64(__fastcall*)(__int64, __int64);
static fn_v0_t       o_shutdownSystem = nullptr;
static fn_reboot_t   o_rebootSystem   = nullptr;
using fn_evv_t    = void(__fastcall*)(void*, void*);
static fn_evv_t      o_launchApp      = nullptr;
using fn_bridge_t = char(__fastcall*)(void*, void*, int, int, unsigned int, void*);
static fn_bridge_t   o_createBridge = nullptr;
static char __fastcall h_createBridge(void* a1, void* a2, int a3, int a4, unsigned int a5, void* a6) {
    if (cfg::srv_block(cfg::SF_TERMINAL)) {
        note_block("terminal(shell_not_allowed)");
        static const std::string blocked = "uu-enhance-view-only";
        return o_createBridge(a1, (void*)&blocked, a3, a4, a5, a6);
    }
    return o_createBridge(a1, a2, a3, a4, a5, a6);
}
using fn_cpau_t = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
static fn_cpau_t     o_createProcAsUser = nullptr;
using fn_shutdown_t = BOOL(WINAPI*)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL);
using fn_shutdownex_t = BOOL(WINAPI*)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL, DWORD);
static fn_shutdown_t   o_initShutdown   = nullptr;
static fn_shutdownex_t o_initShutdownEx = nullptr;

static void note_block(const char* what) {
    static volatile LONG s = 0; LONG n = InterlockedIncrement(&s);
    if (n == 1 || (n % 500) == 0) uu_log("server: blocked %hs x%ld", what, n);
}

static UINT __fastcall h_gvInputSend(UINT c, void* in, int cb, char a4) {
    if (cfg::srv_block(cfg::SF_INPUT)) { note_block("GVInputSend"); return c; }
    return o_gvInputSend(c, in, cb, a4);
}
static __int64 __fastcall h_mouseSend(void* ev) {
    if (cfg::srv_block(cfg::SF_INPUT)) { note_block("mouse"); return 0; }
    return o_mouseSend(ev);
}
static __int64 __fastcall h_kbdSend(void* ev) {
    if (cfg::srv_block(cfg::SF_INPUT)) { note_block("keyboard"); return 0; }
    return o_kbdSend(ev);
}
static UINT WINAPI h_SendInput(UINT n, LPINPUT p, int cb) {
    if (cfg::srv_block(cfg::SF_INPUT)) { note_block("SendInput"); return n; }
    return o_SendInput(n, p, cb);
}
static __int64 __fastcall h_gpUpdate(void* thiz, void* pad) {
    if (cfg::srv_block(cfg::SF_INPUT)) { note_block("gp_update"); return 0; }
    return o_gpUpdate(thiz, pad);
}
static __int64 __fastcall h_gpConnect(void* thiz, unsigned __int64 idx) {
    if (cfg::srv_block(cfg::SF_INPUT)) { note_block("gp_connect"); return 0; }
    return o_gpConnect(thiz, idx);
}
// 吸鼠标(跨机器/对方无插件也有效)：streamer 靠 GetCursorInfo 检测光标隐藏(游戏独占鼠标时 flags=0)，隐藏就通知
// 主控进捕获、把光标锁进窗口。仅浏览时强制 flags=可见，streamer 永远以为没隐藏 → 主控不捕获。x64dbg 实测坐实。
using fn_gci_t = BOOL(WINAPI*)(PCURSORINFO);
static fn_gci_t o_GetCursorInfo = nullptr;
static BOOL WINAPI h_GetCursorInfo(PCURSORINFO pci) {
    BOOL r = o_GetCursorInfo(pci);
    if (r && pci && cfg::srv_block(cfg::SF_INPUT)) pci->flags = CURSOR_SHOWING;
    return r;
}
static __int64 __fastcall h_setRes(void* dev, unsigned w, unsigned h) {
    if (cfg::srv_block(cfg::SF_DISPLAY)) { note_block("SetMonitorResolution"); return 1; }
    return o_setRes(dev, w, h);
}
static __int64 __fastcall h_setResAsync(void* dev, unsigned w, unsigned h) {
    if (cfg::srv_block(cfg::SF_DISPLAY)) { note_block("SetMonitorResolutionAsync"); return 1; }
    return o_setResAsync(dev, w, h);
}
static __int64 __fastcall h_setRef(void* dev, unsigned hz) {
    if (cfg::srv_block(cfg::SF_DISPLAY)) { note_block("SetMonitorRefreshRate"); return 1; }
    return o_setRef(dev, hz);
}
static __int64 __fastcall h_setDpiAsync(void* dev, unsigned dpi) {
    if (cfg::srv_block(cfg::SF_DISPLAY)) { note_block("SetDpiScaleAsync"); return 1; }
    return o_setDpiAsync(dev, dpi);
}
// host::disableMonitor(devNum, flag)：ChangeDisplaySettingsExW 关物理显示器，成功返 1。仅浏览时返 1 装作关成功、实际不关。
static fn_setref_t o_disableMon = nullptr;
static __int64 __fastcall h_disableMon(void* devNum, unsigned flag) {
    if (cfg::srv_block(cfg::SF_DISPLAY)) { note_block("disableMonitor"); return 1; }
    return o_disableMon(devNum, flag);
}
static fn_evv_t o_flipScreen = nullptr;
static void __fastcall h_flipScreen(void* a1, void* a2) {   // onEventFlipScreen：主控翻转/旋转被控画面
    if (cfg::srv_block(cfg::SF_DISPLAY)) { note_block("onEventFlipScreen"); return; }
    o_flipScreen(a1, a2);
}
static __int64 __fastcall h_recvFile(void* thiz, void* msg) {
    if (cfg::srv_block(cfg::SF_FILE)) { note_block("onEventReceiveFile"); return 0; }
    return o_recvFile(thiz, msg);
}
static __int64 __fastcall h_dragPaste(void* thiz, void* msg) {
    if (cfg::srv_block(cfg::SF_FILE)) { note_block("onEventDragDropPaste"); return 0; }
    return o_dragPaste(thiz, msg);
}
static __int64 __fastcall h_micDefault(void* thiz) {
    if (cfg::srv_block(cfg::SF_AUDIO)) { note_block("SetMicrophoneAsDefault"); return 0; }
    return o_micDefault(thiz);
}
static __int64 __fastcall h_setPrivacy(unsigned* a1, unsigned a2, unsigned a3, __int64 a4,
                                       __int64 a5, unsigned char a6, unsigned char a7, __int64 a8) {
    if (cfg::srv_block(cfg::SF_PRIVACY)) { note_block("setPrivacyMode"); return 0; }
    return o_setPrivacy(a1, a2, a3, a4, a5, a6, a7, a8);
}
static __int64 __fastcall h_privLock(void* thiz) {
    if (cfg::srv_block(cfg::SF_PRIVACY)) { note_block("onEventReqDisablePrivacyModeAndLock"); return 0; }
    return o_privLock(thiz);
}
static void __fastcall h_doSetMute(void* thiz, void* outState) {
    if (cfg::srv_block(cfg::SF_AUDIO)) { note_block("do_set_mute"); return; }
    o_doSetMute(thiz, outState);
}
static __int64 __fastcall h_mkVirtualDisp(void* thiz, __int64 req) {
    if (cfg::srv_block(cfg::SF_VDISPLAY)) { note_block("manualCreateVirtualDisplay"); return (unsigned int)-60; }
    return o_mkVirtualDisp(thiz, req);
}
static __int64 __fastcall h_enterSuperScreen(void* thiz, int reason, int fps) {
    if (cfg::srv_block(cfg::SF_VDISPLAY)) { note_block("manualEnterSuperScreenMode"); return (unsigned int)-60; }
    return o_enterSuperScreen(thiz, reason, fps);
}
static __int64 __fastcall h_shutdownSystem() {
    if (cfg::srv_block(cfg::SF_POWER)) { note_block("shutdownSystem"); return 0; }
    return o_shutdownSystem();
}
static __int64 __fastcall h_rebootSystem(__int64 a1, __int64 a2) {
    if (cfg::srv_block(cfg::SF_POWER)) { note_block("rebootSystem"); return 0; }
    return o_rebootSystem(a1, a2);
}
static fn_ev_t o_autoRun = nullptr;
static __int64 __fastcall h_autoRun(void* a1, void* a2) {   // onEventReqAutoRunChange：改自动登录/开机自启
    if (cfg::srv_block(cfg::SF_POWER)) { note_block("onEventReqAutoRunChange"); return 0; }
    return o_autoRun(a1, a2);
}
static fn_evv_t o_wolEnable = nullptr;
static void __fastcall h_wolEnable(void* a1, void* a2) {     // onEventEnableWolSetting：改网络唤醒
    if (cfg::srv_block(cfg::SF_POWER)) { note_block("onEventEnableWolSetting"); return; }
    o_wolEnable(a1, a2);
}
static void __fastcall h_launchApp(void* thiz, void* msg) {
    if (cfg::srv_block(cfg::SF_LAUNCH)) { note_block("handle_launch_app"); return; }
    o_launchApp(thiz, msg);
}
static fn_ev_t o_pmOnFrame = nullptr;
static __int64 __fastcall h_pmOnFrame(void* thiz, void* frame) {
    if (cfg::srv_block(cfg::SF_PORTMAP)) { note_block("port_mapping"); return 0; }
    return o_pmOnFrame(thiz, frame);
}
// 文本注入：handle_text_change_request → notifyReceiveText 把文本经 IPC 转发给注入器。拦转发即掐注入，
// 返回 0 让 handler 照常回失败响应，不断会话。
static fn_ev_t o_notifyText = nullptr;
static __int64 __fastcall h_notifyText(void* thiz, void* text) {
    if (cfg::srv_block(cfg::SF_TEXT)) { note_block("notifyReceiveText"); return 0; }
    return o_notifyText(thiz, text);
}
static BOOL WINAPI h_createProcAsUser(HANDLE tok, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                      LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD flags, LPVOID env,
                                      LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    if (cfg::srv_block(cfg::SF_TERMINAL) && cmd && wcsstr(cmd, L"conpty_bridge")) {
        note_block("terminal(conpty_bridge)"); SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    return o_createProcAsUser(tok, app, cmd, pa, ta, inh, flags, env, dir, si, pi);
}
static BOOL WINAPI h_initShutdown(LPWSTR mach, LPWSTR msg, DWORD to, BOOL force, BOOL reboot) {
    if (cfg::srv_block(cfg::SF_POWER)) { note_block("InitiateSystemShutdown(fallback)"); return TRUE; }
    return o_initShutdown(mach, msg, to, force, reboot);
}
static BOOL WINAPI h_initShutdownEx(LPWSTR mach, LPWSTR msg, DWORD to, BOOL force, BOOL reboot, DWORD reason) {
    if (cfg::srv_block(cfg::SF_POWER)) { note_block("InitiateSystemShutdownEx(fallback)"); return TRUE; }
    return o_initShutdownEx(mach, msg, to, force, reboot, reason);
}

// 抗锯齿禁止符 ⊘：SDF(圆环 ∪ 斜杠)求覆盖率，红色直通 alpha BGRA。
static void draw_no_cursor(unsigned char* p, int w, int h) {
    if (w < 6 || h < 6) return;
    const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
    const float s = (float)(w < h ? w : h);
    const float R = s * 0.45f, half = R * 0.15f, rmid = R - half, outer = rmid + half;
    const float aa = 1.1f, k = 0.70710678f;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float dx = x - cx, dy = y - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        float ring = fabsf(dist - rmid) - half;
        float slash = fabsf(dx - dy) * k - half;
        float cap = dist - outer; if (cap > slash) slash = cap;
        float sdf = ring < slash ? ring : slash;
        float cov = 0.5f - sdf / aa; cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
        unsigned char* q = p + (y * w + x) * 4;
        q[0] = 0; q[1] = 0; q[2] = 255; q[3] = (unsigned char)(cov * 255.0f + 0.5f);
    }
}
// 方舟专属信号：PC 端看 Unity 存档键 Software\Hypergryph\Arknights(运行时才建，装哪个盘都在)，MuMu 版看 YXArkNights 卸载项。
static bool g_prts = false;
static bool reg_key_exists(HKEY root, const wchar_t* sub) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k); return true;
}
static bool uninstall_has_yxark(HKEY root, const wchar_t* uninstallPath) {
    HKEY u;
    if (RegOpenKeyExW(root, uninstallPath, 0, KEY_READ, &u) != ERROR_SUCCESS) return false;
    bool hit = false;
    for (DWORD i = 0; !hit; ++i) {
        wchar_t sub[256]; DWORD len = 256;
        if (RegEnumKeyExW(u, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (_wcsnicmp(sub, L"YXArkNights", 11) == 0) hit = true;
    }
    RegCloseKey(u);
    return hit;
}
static bool detect_arknights() {
    HKEY users;
    if (RegOpenKeyExW(HKEY_USERS, nullptr, 0, KEY_READ, &users) == ERROR_SUCCESS) {
        for (DWORD i = 0; ; ++i) {
            wchar_t sid[256]; DWORD len = 256;
            if (RegEnumKeyExW(users, i, sid, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring p = std::wstring(sid) + L"\\Software\\Hypergryph\\Arknights";
            if (reg_key_exists(HKEY_USERS, p.c_str())) { RegCloseKey(users); return true; }
        }
        RegCloseKey(users);
    }
    if (uninstall_has_yxark(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall")) return true;
    if (uninstall_has_yxark(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall")) return true;
    return false;
}

static DWORD WINAPI poll_thread(LPVOID) {
    bool last = false, first = true;
    for (;;) {
        cfg::refresh_srv_view_only();
        bool cur = cfg::g_srvViewOnly.load();
        if (first || cur != last) { uu_log("server: view-only = %d", (int)cur); first = false; last = cur; }
        Sleep(500);
    }
}

static srvdbg::Entry s_dbgHooks[srvdbg::MAX_HOOKS];
static int           s_dbgCount = 0;
static uintptr_t     s_srvBase  = 0;
static void dbg_add(const char* name, bool ok, const char* how, unsigned long long off) {
    if (s_dbgCount >= srvdbg::MAX_HOOKS) return;
    srvdbg::Entry& e = s_dbgHooks[s_dbgCount++];
    lstrcpynA(e.name, name, srvdbg::NAME_LEN);
    lstrcpynA(e.how, how, sizeof(e.how));
    e.ok = ok ? 1 : 0;
    e.off = off;
}
struct SharedMemRecorder : hookset::IRecorder {
    void record(const char* name, void* addr, const char* how, bool ok) override {
        dbg_add(name, ok, how, addr ? ((uintptr_t)addr - s_srvBase) : 0);
    }
};

static const hookset::Hook kInputHooks[] = {
    { "GVInputSend",                        { "gvinput failed! use SendInput instead", nullptr, nullptr, nullptr }, (void*)h_gvInputSend, (void**)&o_gvInputSend },
    { "mouse_send_gvinput",                 { "gvinput_update_mouse failed!", nullptr, nullptr, nullptr },          (void*)h_mouseSend,   (void**)&o_mouseSend },
    { "keyboard_send_gvinput",              { "gvinput_update_keyboard failed!", nullptr, nullptr, nullptr },       (void*)h_kbdSend,     (void**)&o_kbdSend },
    { "GamepadManagerServer::Update",       { "GamepadManagerServer::Update", nullptr, nullptr, nullptr },          (void*)h_gpUpdate,    (void**)&o_gpUpdate },
    { "GamepadManagerServer::connectInternal",{ "GamepadManagerServer::connectInternal", nullptr, nullptr, nullptr },(void*)h_gpConnect,  (void**)&o_gpConnect },
};

static const hookset::Hook kCtrlHooks[] = {
    { "SetMonitorResolution",               { "host::SetMonitorResolution", nullptr, nullptr, nullptr },       (void*)h_setRes,      (void**)&o_setRes },
    { "SetMonitorResolutionAsync",          { "host::SetMonitorResolutionAsync", nullptr, nullptr, nullptr },  (void*)h_setResAsync, (void**)&o_setResAsync },
    { "SetMonitorRefreshRate",              { "host::SetMonitorRefreshRate", nullptr, nullptr, nullptr },      (void*)h_setRef,      (void**)&o_setRef },
    { "SetDpiScaleAsync",                   { "host::SetDpiScaleAsync", nullptr, nullptr, nullptr },           (void*)h_setDpiAsync, (void**)&o_setDpiAsync },
    { "onEventReceiveFile",                 { "host::ControlledServer::onEventReceiveFile", nullptr, nullptr, nullptr },   (void*)h_recvFile,  (void**)&o_recvFile },
    { "onEventDragDropPaste",               { "host::ControlledServer::onEventDragDropPaste", nullptr, nullptr, nullptr }, (void*)h_dragPaste, (void**)&o_dragPaste },
    { "SetMicrophoneAsDefault",             { "NevaudioHelper::SetMicrophoneAsDefault", nullptr, nullptr, nullptr },       (void*)h_micDefault,(void**)&o_micDefault },
    { "setPrivacyMode",                     { "host::PrivacyScreenHelper::setPrivacyMode", nullptr, nullptr, nullptr },    (void*)h_setPrivacy,(void**)&o_setPrivacy },
    { "onEventReqDisablePrivacyModeAndLock",{ "host::ControlledServer::onEventReqDisablePrivacyModeAndLock", nullptr, nullptr, nullptr }, (void*)h_privLock, (void**)&o_privLock },
    { "do_set_mute",                        { "host::MuteSettingManager::do_set_mute", nullptr, nullptr, nullptr },        (void*)h_doSetMute, (void**)&o_doSetMute },
    { "manualCreateVirtualDisplay",         { "host::ControlledConnection::manualCreateVirtualDisplay", nullptr, nullptr, nullptr },       (void*)h_mkVirtualDisp,    (void**)&o_mkVirtualDisp },
    { "manualEnterSuperScreenMode",         { "host::ControlledConnection::manualEnterSuperScreenMode", nullptr, nullptr, nullptr },       (void*)h_enterSuperScreen,(void**)&o_enterSuperScreen },
    { "shutdownSystem",                     { "host::PushMessageHandler::shutdownSystem", nullptr, nullptr, nullptr },     (void*)h_shutdownSystem, (void**)&o_shutdownSystem },
    { "rebootSystem",                       { "host::PushMessageHandler::rebootSystem", nullptr, nullptr, nullptr },       (void*)h_rebootSystem,   (void**)&o_rebootSystem },
    { "handle_launch_app",                  { "host::TextMessageHandler::handle_launch_app", nullptr, nullptr, nullptr }, (void*)h_launchApp, (void**)&o_launchApp },
    { "create_bridge_process",              { "create_bridge_process_failed_", "bridge_exe_not_found", nullptr, nullptr }, (void*)h_createBridge, (void**)&o_createBridge },
    { "PortMappingService::onFrame",        { "port_mapping::PortMappingService::onFrame", "[PM] frame for unknown rule_id=", nullptr, nullptr }, (void*)h_pmOnFrame, (void**)&o_pmOnFrame },
    { "notifyReceiveText",                  { "host::IpcSvrWrapper::notifyReceiveText", nullptr, nullptr, nullptr }, (void*)h_notifyText, (void**)&o_notifyText },
    { "disableMonitor",                     { "host::disableMonitor", nullptr, nullptr, nullptr }, (void*)h_disableMon, (void**)&o_disableMon },
    { "onEventFlipScreen",                  { "host::ControlledServer::onEventFlipScreen", nullptr, nullptr, nullptr }, (void*)h_flipScreen, (void**)&o_flipScreen },
    { "onEventReqAutoRunChange",            { "host::ControlledServer::onEventReqAutoRunChange", nullptr, nullptr, nullptr }, (void*)h_autoRun,   (void**)&o_autoRun },
    { "onEventEnableWolSetting",            { "host::ControlledServer::onEventEnableWolSetting", nullptr, nullptr, nullptr }, (void*)h_wolEnable, (void**)&o_wolEnable },
};

static void publish_srv_dbg() {
    static HANDLE s_map = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            srvdbg::SDDL, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
        uu_log("server: shared-mem SDDL build failed"); return;
    }
    s_map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                               0, sizeof(srvdbg::Shared), srvdbg::MAP_NAME);
    LocalFree(sa.lpSecurityDescriptor);
    if (!s_map) { uu_log("server: CreateFileMapping failed err=%lu", GetLastError()); return; }
    auto* sh = (srvdbg::Shared*)MapViewOfFile(s_map, FILE_MAP_WRITE, 0, 0, sizeof(srvdbg::Shared));
    if (!sh) { uu_log("server: MapViewOfFile failed"); return; }
    int n = s_dbgCount > srvdbg::MAX_HOOKS ? srvdbg::MAX_HOOKS : s_dbgCount;
    for (int i = 0; i < n; ++i) sh->hooks[i] = s_dbgHooks[i];
    sh->count = n;
    UnmapViewOfFile(sh);
    uu_log("server: debug info published (%d entries)", n);
}

// sub_14067B290 把光标 BGRA 编码成 PNG 发给主控。喂 kPrts/⊘ 给原函数(自身分配、caller 释放，输入只读)。
// 主控把 PNG 重采样到 CursorShape 尺寸，故 kPrts 尺寸只关清晰度、不决定大小(大小见 h_curState)。
using fn_png_t = void*(__fastcall*)(void*, unsigned, unsigned, int*);
static fn_png_t o_pngConv = nullptr;
static void* __fastcall h_pngConv(void* src, unsigned w, unsigned h, int* outSize) {
    if (cfg::srv_block(cfg::SF_INPUT)) {
        if (g_prts) return o_pngConv((void*)kPrts, (unsigned)PRTS_N, (unsigned)PRTS_N, outSize);
        static unsigned char noSym[PRTS_N * PRTS_N * 4];
        static bool inited = false;
        if (!inited) { draw_no_cursor(noSym, PRTS_N, PRTS_N); inited = true; }
        return o_pngConv(noSym, (unsigned)PRTS_N, (unsigned)PRTS_N, outSize);
    }
    return o_pngConv(src, w, h, outSize);
}

// 光标状态：+0x28/+0x2C=热点X/Y，+0x30/+0x34=宽/高，成为 CursorShape。主控端热点分数=hotspotX/width。
// 故固定尺寸 + 热点设尺寸/2(分数 0.5=居中)。仅 PNG hook 已装时改，否则原编码器按放大尺寸越界读缓冲。
static const int kCursorSize = 64;
using fn_curstate_t = char(__fastcall*)(void*, void*);
static fn_curstate_t o_curState = nullptr;
static char __fastcall h_curState(void* a1, void* state) {
    char ok = o_curState(a1, state);
    if (ok && state && o_pngConv && cfg::srv_block(cfg::SF_INPUT)) __try {
        int* w = (int*)((char*)state + 0x30), * h = (int*)((char*)state + 0x34);
        int* hx = (int*)((char*)state + 0x28), * hy = (int*)((char*)state + 0x2C);
        if (*w > 0 && *h > 0 && *w <= 256 && *h <= 256) {
            *w = *h = kCursorSize;
            *hx = *hy = kCursorSize / 2;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}
// 在 sendCursorShape 内定位对 sub_1400020A9 的调用：其返回值紧跟 test al,al / jz。
static uintptr_t find_cursorstate_fn(const resolver::ModRange& r, uintptr_t send) {
    if (!send) return 0;
    const uint8_t* p0 = (const uint8_t*)send;
    const uint8_t* end = p0 + 0x300;
    if ((uintptr_t)end > r.text_end) end = (const uint8_t*)r.text_end;
    uintptr_t call = 0;
    for (const uint8_t* q = p0; q < end; ) {
        xd::Insn i = xd::decode(q);
        if (!i.len) { ++q; call = 0; continue; }
        if (i.opcode == 0xE8 && !i.two_byte && i.has_rel) {
            call = xd::rel_target(q, i);
        }
        else if (call && i.opcode == 0x84 && !i.two_byte && i.has_modrm
                 && i.mod == 3 && i.reg == 0 && i.rm == 0 && !i.rex_r && !i.rex_b) {   // test al,al
            xd::Insn j = xd::decode(q + i.len);
            if (j.len && j.two_byte && j.opcode2 == 0x84                               // jz near
                && call >= r.text_beg && call < r.text_end) return call;
            call = 0;
        }
        else call = 0;
        q += i.len;
    }
    return 0;
}

void install_server_hooks() {
    HMODULE base = GetModuleHandleW(nullptr);
    s_srvBase = (uintptr_t)base;
    resolver::ModRange r{};
    if (!resolver::get_ranges(base, r)) { uu_log("server: get_ranges failed"); return; }

    bool hasInput = resolver::find_string(r, "gvinput failed! use SendInput instead") != 0;
    bool hasCtrl  = resolver::find_string(r, "host::SetMonitorResolution") != 0
                 || resolver::find_string(r, "host::ControlledServer::onEventReceiveFile") != 0;
    if (!hasInput && !hasCtrl) { uu_log("server: no anchors, skip (helper process)"); return; }
    if (MH_Initialize() != MH_OK) { uu_log("server: MH_Initialize failed"); return; }

    SharedMemRecorder rec;
    if (hasInput) {
        hookset::install(r, kInputHooks, (int)(sizeof(kInputHooks) / sizeof(kInputHooks[0])), rec);
        hookset::install_export(L"user32.dll", "SendInput", (void*)h_SendInput, (void**)&o_SendInput, rec);
    }
    if (hasCtrl) {
        hookset::install(r, kCtrlHooks, (int)(sizeof(kCtrlHooks) / sizeof(kCtrlHooks[0])), rec);
        hookset::install_export(L"advapi32.dll", "InitiateSystemShutdownW",   (void*)h_initShutdown,     (void**)&o_initShutdown,     rec);
        hookset::install_export(L"advapi32.dll", "InitiateSystemShutdownExW", (void*)h_initShutdownEx,   (void**)&o_initShutdownEx,   rec);
        hookset::install_export(L"advapi32.dll", "CreateProcessAsUserW",      (void*)h_createProcAsUser, (void**)&o_createProcAsUser, rec);
    }
    g_prts = detect_arknights();
    uu_log("server: arknights detected = %d (cursor: %hs)", (int)g_prts, g_prts ? "prts" : "no-symbol");
    // 防锁鼠标：伪装 GetCursorInfo 始终"光标可见"(streamer 靠它判隐藏来锁鼠标)。两被控进程都装。
    hookset::install_export(L"user32.dll", "GetCursorInfo", (void*)h_GetCursorInfo, (void**)&o_GetCursorInfo, rec);
    {
        uintptr_t pngfn = resolver::find_func_by_wstr(r, L"image/png");
        bool ok = pngfn && MH_CreateHook((void*)pngfn, (void*)h_pngConv, (void**)&o_pngConv) == MH_OK
                        && MH_EnableHook((void*)pngfn) == MH_OK;
        uu_log("server: hook cursor png %hs @ %p", ok ? "ok" : (pngfn ? "failed" : "not found"), (void*)pngfn);
        dbg_add("cursorPngShape", ok, "wstr", pngfn ? (pngfn - s_srvBase) : 0);
    }
    if (o_pngConv) {   // 尺寸放大依赖 PNG hook 已装，见 h_curState
        uintptr_t send = resolver::find_func(r, { "Failed to convert cursor shape to png." });
        uintptr_t csfn = find_cursorstate_fn(r, send);
        bool ok = csfn && MH_CreateHook((void*)csfn, (void*)h_curState, (void**)&o_curState) == MH_OK
                       && MH_EnableHook((void*)csfn) == MH_OK;
        uu_log("server: hook cursor size %hs @ %p (=%dpx)", ok ? "ok" : (csfn ? "failed" : "not found"), (void*)csfn, kCursorSize);
        dbg_add("cursorSize", ok, "scan", csfn ? (csfn - s_srvBase) : 0);
    }
    publish_srv_dbg();
    CreateThread(nullptr, 0, poll_thread, nullptr, 0, nullptr);
    uu_log("server: ready input=%d ctrl=%d view-only=%d", (int)hasInput, (int)hasCtrl, (int)cfg::g_srvViewOnly.load());
}
