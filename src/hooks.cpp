#include <windows.h>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <iterator>
#include <cstdint>
#include "MinHook.h"
#include "offsets.h"
#include "config.h"
#include "log.h"
#include "resolver.h"
#include "hookset.h"
#include "session.h"
#include "srvdbg.h"

using fn_send_t   = void*(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
using fn_cap_t    = void (__fastcall*)(void* thiz, unsigned __int8 enable, char toast, char a4);
using fn_clipupd_t= void (__fastcall*)(void* thiz);
using fn_fmtlist_t= __int64(__fastcall*)(void* thiz, void* a2, void* a3);
using fn_clipget_t= __int64(__fastcall*)(void* hwnd, unsigned int fmt, void* out);
using fn_sendfmt_t= __int64(__fastcall*)(void* thiz);
using fn_clipreq_t= __int64(__fastcall*)(void* thiz, void* a2, void* a3);
using fn_gpupd_t  = void (__fastcall*)(void* thiz, void* padState);
using fn_vmwctor_t= __int64(__fastcall*)(void* thiz, void* devidQs, void* a3, void* sp, int a5, __int64 a6);

static fn_send_t    o_sendMouse = nullptr, o_sendWheel = nullptr, o_sendKey = nullptr;
static fn_cap_t     o_enableCapture = nullptr;
static fn_clipupd_t o_clipUpdate = nullptr;
static fn_fmtlist_t o_clipFmtList = nullptr;
static fn_clipget_t o_clipGet = nullptr;
static fn_sendfmt_t o_clipSendFmt = nullptr;
static fn_clipreq_t o_clipReq = nullptr;
static fn_gpupd_t   o_gamepadUpdate = nullptr, o_gamepadConnect = nullptr, o_gamepadDisconnect = nullptr;
static fn_vmwctor_t o_vmwCtor = nullptr;

using fn_lock_t = BOOL(WINAPI*)(void);
static fn_lock_t o_lockWorkStation = nullptr;
static BOOL WINAPI h_lockWorkStation(void) {
    if (cfg::srv_block(cfg::SF_PRIVACY)) { uu_log("view-only: blocked LockWorkStation"); return TRUE; }
    return o_lockWorkStation();
}

static uintptr_t CCS_DEVICE_ID_OFF = 3984;
static uintptr_t VMW_DEVICE_ID_OFF = 344;
static uintptr_t VMW_TITLE_OFF     = 352;
static bool      g_devIdAuto  = false;
static bool      g_vmwOffAuto = false;

struct SessState { bool viewOnly; bool clipSync; bool gamepadOff; std::wstring devid; };
static std::mutex                 g_smtx;
static std::map<void*, SessState> g_sessions;
static void*                      g_activeCCS = nullptr;

static std::map<std::wstring, void*> g_devidToVmw;

static void*                      g_serverClip = nullptr;
static std::map<void*, void*>     g_clipToCcs;
static thread_local void*         t_curClip = nullptr;

static int safe_copy_devid(void* ccs, char* buf, int bufsz) {
    __try {
        char* s = (char*)ccs + CCS_DEVICE_ID_OFF;
        size_t len = *(size_t*)(s + 16);
        size_t cap = *(size_t*)(s + 24);
        const char* p = (cap >= 16) ? *(const char**)s : s;
        if (!p || len == 0 || len >= (size_t)bufsz) return 0;
        for (size_t i = 0; i < len; ++i) { unsigned char c = (unsigned char)p[i]; if (c < 0x20) return 0; buf[i] = p[i]; }
        return (int)len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static std::wstring read_device_id(void* ccs) {
    char buf[129];
    int len = safe_copy_devid(ccs, buf, sizeof(buf));
    if (len <= 0) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, buf, len, &w[0], n);
    return w;
}

// QString layout: d ptr -> QArrayData [+4]int size [+16]qptrdiff offset; chars at (char*)d+offset
static int safe_copy_qstr(const void* qsHolder, wchar_t* buf, int cap) {
    __try {
        const unsigned char* d = *(const unsigned char* const*)qsHolder;
        if (!d) return 0;
        int size = *(const int*)(d + 4);
        long long off = *(const long long*)(d + 16);
        if (size <= 0 || size >= cap) return 0;
        const unsigned short* s = (const unsigned short*)(d + off);
        for (int i = 0; i < size; ++i) {
            unsigned short c = s[i];
            if (c == 0) return i;
            buf[i] = (wchar_t)c;
        }
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static std::wstring read_qstring(const void* qsHolder) {
    wchar_t buf[256];
    int n = safe_copy_qstr(qsHolder, buf, 256);
    if (n <= 0) return L"";
    return std::wstring(buf, n);
}

static uintptr_t derive_off_after_str(const resolver::ModRange& r, uintptr_t func, const char* anchorStr) {
    if (!func) return 0;
    uintptr_t sa = resolver::find_string(r, anchorStr);
    if (!sa) return 0;
    uint8_t* p0 = (uint8_t*)func;
    uint8_t* end = p0 + 0x400;
    if ((uintptr_t)end > r.text_end) end = (uint8_t*)r.text_end;
    uint8_t* strLea = nullptr;
    for (uint8_t* q = p0; q + 7 <= end; ++q)
        if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05) {
            if ((uintptr_t)(q + 7) + *(int32_t*)(q + 3) == sa) { strLea = q; break; }
        }
    if (!strLea) return 0;
    uint8_t* s2end = strLea + 0x40;
    if ((uintptr_t)s2end > (uintptr_t)end) s2end = end;
    for (uint8_t* q = strLea + 7; q + 7 <= s2end; ++q)
        if ((q[0] == 0x48 || q[0] == 0x49) && q[1] == 0x8D && (q[2] & 0xC0) == 0x80) {
            uint8_t rm = q[2] & 0x07;
            if (rm != 4 && rm != 5) {
                int32_t disp = *(int32_t*)(q + 3);
                if (disp >= 0x40 && disp <= 0x8000) return (uintptr_t)disp;
            }
        }
    return 0;
}

static bool g_vmwDerived = false;
static void derive_vmw_off(void* thiz, const void* devidQs) {
    std::wstring want = read_qstring(devidQs);
    if (want.empty()) return;
    for (uintptr_t off = 0x100; off <= 0x400; off += 8)
        if (read_qstring((char*)thiz + off) == want) {
            VMW_DEVICE_ID_OFF = off;
            VMW_TITLE_OFF     = off + 8;
            g_vmwOffAuto = true;
            uu_log("vmw offsets auto-derived: devid=+%llu title=+%llu",
                   (unsigned long long)off, (unsigned long long)(off + 8));
            return;
        }
    uu_log("vmw offsets auto-derive missed, keep table devid=+%llu", (unsigned long long)VMW_DEVICE_ID_OFF);
}

static SessState& sessOf(void* ccs) {
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) return it->second;
    SessState s;
    s.viewOnly   = cfg::g_viewOnly.load();
    s.clipSync   = cfg::g_clipSync.load();
    s.gamepadOff = cfg::g_gamepadOff.load();
    s.devid       = read_device_id(ccs);
    uu_log("session new: ccs=%p devid=%ls viewOnly=%d", ccs, s.devid.c_str(), (int)s.viewOnly);
    return g_sessions.emplace(ccs, std::move(s)).first->second;
}
static bool input_viewOnly(void* ccs) {
    std::lock_guard<std::mutex> lk(g_smtx);
    g_activeCCS = ccs;
    return sessOf(ccs).viewOnly;
}
static bool active_viewOnly() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load();
    return sessOf(g_activeCCS).viewOnly;
}
static bool clip_allowed_locked(void* clip) {
    if (!clip) {
        if (g_activeCCS) return sessOf(g_activeCCS).clipSync;
        return true;
    }
    if (!g_activeCCS) {
        g_serverClip = clip;
        g_clipToCcs.erase(clip);
    }
    if (clip == g_serverClip) return cfg::g_ctrlClip.load() && !cfg::g_srvViewOnly.load();
    auto it = g_clipToCcs.find(clip);
    void* ccs = (it != g_clipToCcs.end()) ? it->second : nullptr;
    if (!ccs && g_activeCCS) { g_clipToCcs[clip] = g_activeCCS; ccs = g_activeCCS; }
    if (ccs) { auto s = g_sessions.find(ccs); if (s != g_sessions.end()) return s->second.clipSync; }
    return true;
}
static bool clip_allowed(void* clip) {
    std::lock_guard<std::mutex> lk(g_smtx);
    return clip_allowed_locked(clip);
}
static bool active_gpBlock() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load() || cfg::g_gamepadOff.load();
    auto& s = sessOf(g_activeCCS);
    return s.viewOnly || s.gamepadOff;
}

static void* __fastcall h_sendMouse(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (input_viewOnly(a1)) return nullptr;
    return o_sendMouse(a1, a2, a3, a4, a5, a6, a7, a8);
}
static void* __fastcall h_sendWheel(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (input_viewOnly(a1)) return nullptr;
    return o_sendWheel(a1, a2, a3, a4, a5, a6, a7, a8);
}
static void* __fastcall h_sendKey(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (input_viewOnly(a1)) return nullptr;
    return o_sendKey(a1, a2, a3, a4, a5, a6, a7, a8);
}
static void __fastcall h_enableCapture(void* thiz, unsigned __int8 enable, char toast, char a4) {
    if (active_viewOnly() || cfg::srv_block(cfg::SF_INPUT)) enable = 0;   // 仅浏览：不进捕获(不 ClipCursor/SetCursorPos)
    o_enableCapture(thiz, enable, toast, a4);
}

static void __fastcall h_clipUpdate(void* thiz) {
    if (!clip_allowed(thiz)) return;
    void* prev = t_curClip; t_curClip = thiz;
    o_clipUpdate(thiz);
    t_curClip = prev;
}
static __int64 __fastcall h_clipSendFmt(void* thiz) {
    if (!clip_allowed(thiz)) return 0;
    return o_clipSendFmt(thiz);
}
static __int64 __fastcall h_clipFmtList(void* thiz, void* a2, void* a3) {
    if (!clip_allowed(thiz)) return 0;
    return o_clipFmtList(thiz, a2, a3);
}
static __int64 __fastcall h_clipReq(void* thiz, void* a2, void* a3) {
    void* prev = t_curClip; t_curClip = thiz;
    __int64 r = o_clipReq(thiz, a2, a3);
    t_curClip = prev;
    return r;
}
// out is MSVC std::string: [0..15]SSO/ptr [16]size [24]cap
static __int64 __fastcall h_clipGet(void* hwnd, unsigned int fmt, void* out) {
    if (!clip_allowed(t_curClip)) {
        if (out) {
            size_t* s = (size_t*)out;
            char* buf = s[3] >= 0x10 ? *(char**)out : (char*)out;
            s[2] = 0;
            buf[0] = 0;
        }
        return 0;
    }
    return o_clipGet(hwnd, fmt, out);
}

static std::mutex        g_gpMtx;
static void*             g_gpMgr = nullptr;
static std::set<uint8_t> g_desired;
static void gp_reconcile(bool block) {
    void* mgr; std::vector<uint8_t> ids;
    {
        std::lock_guard<std::mutex> lk(g_gpMtx);
        mgr = g_gpMgr;
        ids.assign(g_desired.begin(), g_desired.end());
    }
    if (!mgr) return;
    for (uint8_t i : ids) {
        uint8_t v = i;
        if (block) { if (o_gamepadDisconnect) o_gamepadDisconnect(mgr, &v); }
        else       { if (o_gamepadConnect)    o_gamepadConnect(mgr, &v); }
    }
}
static void __fastcall h_gamepadConnect(void* thiz, void* idx) {
    { std::lock_guard<std::mutex> lk(g_gpMtx); g_gpMgr = thiz; if (idx) g_desired.insert(*(uint8_t*)idx); }
    if (active_gpBlock()) return;
    o_gamepadConnect(thiz, idx);
}
static void __fastcall h_gamepadDisconnect(void* thiz, void* idx) {
    { std::lock_guard<std::mutex> lk(g_gpMtx); g_gpMgr = thiz; if (idx) g_desired.erase(*(uint8_t*)idx); }
    o_gamepadDisconnect(thiz, idx);
}
static void __fastcall h_gamepadUpdate(void* thiz, void* padState) {
    { std::lock_guard<std::mutex> lk(g_gpMtx); g_gpMgr = thiz; }
    if (active_gpBlock()) return;
    o_gamepadUpdate(thiz, padState);
}

static __int64 __fastcall h_vmwCtor(void* thiz, void* devidQs, void* a3, void* sp, int a5, __int64 a6) {
    __int64 r = o_vmwCtor(thiz, devidQs, a3, sp, a5, a6);
    if (!g_vmwDerived) { g_vmwDerived = true; derive_vmw_off(thiz, devidQs); }
    std::wstring devid = read_qstring((char*)thiz + VMW_DEVICE_ID_OFF);
    if (!devid.empty()) {
        std::lock_guard<std::mutex> lk(g_smtx);
        g_devidToVmw[devid] = thiz;
        uu_log("vmw registered: devid=%ls vmw=%p", devid.c_str(), thiz);
    }
    return r;
}

std::vector<SessSnap> sessions_snapshot() {
    std::lock_guard<std::mutex> lk(g_smtx);
    std::vector<SessSnap> v;
    for (auto& kv : g_sessions) {
        std::wstring disp = kv.second.devid;
        auto it = g_devidToVmw.find(kv.second.devid);
        if (!kv.second.devid.empty() && it != g_devidToVmw.end()) {
            void* vmw = it->second;
            std::wstring vd = read_qstring((char*)vmw + VMW_DEVICE_ID_OFF);
            if (vd == kv.second.devid) {
                std::wstring title = read_qstring((char*)vmw + VMW_TITLE_OFF);
                if (!title.empty()) disp = title;
            }
        }
        v.push_back({ kv.first, disp, kv.second.viewOnly, kv.second.clipSync, kv.second.gamepadOff });
    }
    return v;
}
// field: 0=viewOnly 1=clipSync 2=gamepadOff
bool session_toggle(void* key, int field) {
    bool nv = false; bool doGpReconcile = false; bool block = false;
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        auto it = g_sessions.find(key);
        if (it == g_sessions.end()) return false;
        auto& s = it->second;
        if (field == 0) { s.viewOnly = !s.viewOnly; nv = s.viewOnly; doGpReconcile = true; block = s.viewOnly || s.gamepadOff; }
        else if (field == 1) { s.clipSync = !s.clipSync; nv = s.clipSync; }
        else { s.gamepadOff = !s.gamepadOff; nv = s.gamepadOff; doGpReconcile = true; block = s.viewOnly || s.gamepadOff; }
    }
    if (field == 0 && nv) ClipCursor(nullptr);
    if (doGpReconcile) gp_reconcile(block);
    uu_log("session_toggle key=%p field=%d -> %d", key, field, (int)nv);
    return nv;
}

using fn4_t = __int64(__fastcall*)(void*, void*, void*, void*);
static fn4_t o_setConnInfo = nullptr, o_closeConn = nullptr, o_exitRoom = nullptr;

static void session_remove(void* ccs) {
    std::lock_guard<std::mutex> lk(g_smtx);
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) {
        if (!it->second.devid.empty()) g_devidToVmw.erase(it->second.devid);
        g_sessions.erase(it);
        uu_log("session remove: ccs=%p", ccs);
    }
    if (g_activeCCS == ccs) g_activeCCS = nullptr;
    for (auto jt = g_clipToCcs.begin(); jt != g_clipToCcs.end(); )
        jt = (jt->second == ccs) ? g_clipToCcs.erase(jt) : std::next(jt);
}
static __int64 __fastcall h_setConnInfo(void* ccs, void* a2, void* a3, void* a4) {
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        std::wstring devid = read_device_id(ccs);
        if (!devid.empty())
            for (auto it = g_sessions.begin(); it != g_sessions.end(); )
                it = (it->first != ccs && it->second.devid == devid) ? g_sessions.erase(it) : std::next(it);
        g_activeCCS = ccs;
        sessOf(ccs);
    }
    return o_setConnInfo(ccs, a2, a3, a4);
}
static __int64 __fastcall h_closeConn(void* ccs, void* a2, void* a3, void* a4) {
    session_remove(ccs);
    return o_closeConn(ccs, a2, a3, a4);
}
static __int64 __fastcall h_exitRoom(void* ccs, void* a2, void* a3, void* a4) {
    session_remove(ccs);
    return o_exitRoom(ccs, a2, a3, a4);
}

using fn_curs_t = __int64(__fastcall*)(void*, unsigned int);
static fn_curs_t o_updateCursor = nullptr;
static __int64 __fastcall h_updateCursor(void* vw, unsigned int force) {
    if (active_viewOnly() || cfg::srv_block(cfg::SF_INPUT)) {
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_NO));
        return 1;
    }
    return o_updateCursor(vw, force);
}

// 仅浏览时不让光标被锁进视频窗口。游戏相对模式会持续 ClipCursor 锁回，故须 hook 持续拦，而非一次释放。
using fn_clip_t = BOOL(WINAPI*)(const RECT*);
static fn_clip_t o_ClipCursor = nullptr;
static BOOL WINAPI h_ClipCursor(const RECT* rc) {
    if (rc && (active_viewOnly() || cfg::srv_block(cfg::SF_INPUT))) return o_ClipCursor(nullptr);
    return o_ClipCursor(rc);
}

static bool g_verKnown = false;

static std::mutex g_dbgMtx;
static std::vector<HookStat> g_hookStats;
static std::wstring g_gvVersion = L"?";
static uintptr_t g_gvBase = 0;

static void record_hook(const char* name, void* addr, const char* how, bool ok) {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    g_hookStats.push_back({ name, addr, how, ok });
}

struct InProcRecorder : hookset::IRecorder {
    void record(const char* name, void* addr, const char* how, bool ok) override {
        record_hook(name, addr, how, ok);
    }
};

static const hookset::Hook kHooks[] = {
    { "sendMouseEvent",              { "ControlConnectionSession::sendMouseEvent", "[control] mouseObj size 0", nullptr, nullptr }, (void*)h_sendMouse, (void**)&o_sendMouse },
    { "sendMouseWheel",              { "ControlConnectionSession::sendMouseWheel", "sendMouseWheel failed, session_config_ handle invalid", nullptr, nullptr }, (void*)h_sendWheel, (void**)&o_sendWheel },
    { "sendKeyboardEvent",           { "ControlConnectionSession::sendKeyboardEvent", nullptr, nullptr, nullptr }, (void*)h_sendKey, (void**)&o_sendKey },
    { "enabledCaptureMouse",         { "VideoUi::VideoWidget::enabledCaptureMouse", "==== Enabled capture mouse: ", "Cursor not in rect", nullptr }, (void*)h_enableCapture, (void**)&o_enableCapture },
    { "GamepadManager::Connect",     { "GamepadManager::Connect(), index=", "GamepadManager::Connect", nullptr, nullptr }, (void*)h_gamepadConnect, (void**)&o_gamepadConnect },
    { "GamepadManager::Disconnect",  { "GamepadManager::Disconnect(), index=", "GamepadManager::Disconnect", nullptr, nullptr }, (void*)h_gamepadDisconnect, (void**)&o_gamepadDisconnect },
    { "GamepadManager::Update",      { "[%d] GamepadManager::Update(), json=%s", "GamepadManager::Update", nullptr, nullptr }, (void*)h_gamepadUpdate, (void**)&o_gamepadUpdate },
    { "on_clipboard_update",         { "Clipboard::on_clipboard_update", "Get clipboard data failed", nullptr, nullptr }, (void*)h_clipUpdate, (void**)&o_clipUpdate },
    { "do_handle_format_list_request",{ "Clipboard::do_handle_format_list_request", "do_handle_format_list_request: is_file_transferring=true", nullptr, nullptr }, (void*)h_clipFmtList, (void**)&o_clipFmtList },
    { "get_clipboard_data",          { "Clipboard::get_clipboard_data", "GlobalLock failed: ", nullptr, nullptr }, (void*)h_clipGet, (void**)&o_clipGet },
    { "do_send_format_list",         { "Clipboard::do_send_format_list", "do_send_format_list: send_request failed", nullptr, nullptr }, (void*)h_clipSendFmt, (void**)&o_clipSendFmt },
    { "handle_clipboard_request",    { "Clipboard::handle_clipboard_request", "Received auto_save_complete: total=", nullptr, nullptr }, (void*)h_clipReq, (void**)&o_clipReq },
    { "setConnectInfo",              { "ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: ", nullptr, nullptr }, (void*)h_setConnInfo, (void**)&o_setConnInfo },
    { "closeControlConnect",         { "ControlConnectionSession::closeControlConnect", nullptr, nullptr, nullptr }, (void*)h_closeConn, (void**)&o_closeConn },
    { "exitRoom",                    { "ControlConnectionSession::exitRoom", nullptr, nullptr, nullptr }, (void*)h_exitRoom, (void**)&o_exitRoom },
    { "VideoMainWindow::ctor",       { "home_control_session_start: window_created, device_id=", nullptr, nullptr, nullptr }, (void*)h_vmwCtor, (void**)&o_vmwCtor },
    { "updateCursor",                { "VideoUi::VideoWidget::updateCursor", "set cursor by id", "Default set arrow cursor", nullptr }, (void*)h_updateCursor, (void**)&o_updateCursor },
};

void install_hooks(uintptr_t base) {
    if (MH_Initialize() != MH_OK) { uu_log("MH_Initialize failed"); return; }
    std::wstring vs = cfg::exe_version();
    const ver::VerSet& V = ver::pick(vs.c_str());
    g_verKnown = (!vs.empty() && vs == V.version);
    g_gvBase = base;
    g_gvVersion = vs.empty() ? L"?" : vs;
    CCS_DEVICE_ID_OFF = V.deviceIdOff;
    VMW_DEVICE_ID_OFF = V.vmwDevIdOff;
    VMW_TITLE_OFF     = V.vmwTitleOff;
    uu_log("GameViewer version=%ls known=%d", vs.empty() ? L"?" : vs.c_str(), (int)g_verKnown);
    resolver::ModRange r{};
    resolver::get_ranges((HMODULE)base, r);
    {
        uintptr_t scfn = resolver::find_func(r, {"ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: "});
        uintptr_t d = derive_off_after_str(r, scfn, "startConnectOtherDevice, device_id: ");
        if (d) { CCS_DEVICE_ID_OFF = d; g_devIdAuto = true; }
        uu_log("deviceIdOff: table=%llu derived=%llu use=%llu", (unsigned long long)V.deviceIdOff,
               (unsigned long long)d, (unsigned long long)CCS_DEVICE_ID_OFF);
    }
    InProcRecorder rec;
    hookset::install(r, kHooks, (int)(sizeof(kHooks) / sizeof(kHooks[0])), rec);
    hookset::install_export(L"user32.dll", "LockWorkStation", (void*)h_lockWorkStation, (void**)&o_lockWorkStation, rec);
    hookset::install_export(L"user32.dll", "ClipCursor", (void*)h_ClipCursor, (void**)&o_ClipCursor, rec);
    uu_log("install_hooks done");
}

DebugInfo debug_snapshot() {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    DebugInfo d;
    d.gvVersion = g_gvVersion;
    d.gvKnown = g_verKnown;
    d.devIdOff = CCS_DEVICE_ID_OFF;
    d.vmwDevIdOff = VMW_DEVICE_ID_OFF;
    d.vmwTitleOff = VMW_TITLE_OFF;
    d.devIdAuto = g_devIdAuto;
    d.vmwAuto = g_vmwOffAuto;
    d.serverRunning = false;
    for (const auto& h : g_hookStats)
        d.hooks.push_back({ L"ctl", h.name, h.how, h.addr ? (unsigned long long)((uintptr_t)h.addr - g_gvBase) : 0, h.ok });
    if (HANDLE sm = OpenFileMappingW(FILE_MAP_READ, FALSE, srvdbg::MAP_NAME)) {
        if (auto* sh = (srvdbg::Shared*)MapViewOfFile(sm, FILE_MAP_READ, 0, 0, sizeof(srvdbg::Shared))) {
            int n = (int)sh->count; if (n < 0) n = 0; if (n > srvdbg::MAX_HOOKS) n = srvdbg::MAX_HOOKS;
            for (int i = 0; i < n; ++i) {
                const srvdbg::Entry& e = sh->hooks[i];
                char name[srvdbg::NAME_LEN]; lstrcpynA(name, e.name, srvdbg::NAME_LEN);
                char how[8];                 lstrcpynA(how,  e.how,  sizeof(how));
                d.hooks.push_back({ L"srv", std::string(name), std::string(how), e.off, e.ok != 0 });
            }
            d.serverRunning = true;
            UnmapViewOfFile(sh);
        }
        CloseHandle(sm);
    }
    return d;
}
