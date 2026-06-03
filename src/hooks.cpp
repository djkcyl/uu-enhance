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
#include "session.h"

// 原函数指针（trampoline）
using fn_send_t   = void*(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
using fn_cap_t    = void (__fastcall*)(void* thiz, unsigned __int8 enable, char toast, char a4);
using fn_clipupd_t= void (__fastcall*)(void* thiz);
using fn_cliphdl_t= __int64(__fastcall*)(void* thiz, void* a2, void* a3);
using fn_gpupd_t  = void (__fastcall*)(void* thiz, void* padState);

static fn_send_t    o_sendMouse = nullptr, o_sendWheel = nullptr, o_sendKey = nullptr;
static fn_cap_t     o_enableCapture = nullptr;
static fn_clipupd_t o_clipUpdate = nullptr;
static fn_cliphdl_t o_clipHandle = nullptr;
static fn_gpupd_t   o_gamepadUpdate = nullptr, o_gamepadConnect = nullptr, o_gamepadDisconnect = nullptr;

// CCS 内 device_id (std::string) 偏移，由所选版本表设置；仅用于去重/回退名(读错不影响分会话)
static uintptr_t CCS_DEVICE_ID_OFF = 3984;

// 每个会话的状态，用 CCS 指针做 key
struct SessState { bool viewOnly; bool clipSync; bool gamepadOff; std::wstring devid; std::wstring name; DWORD lastNameTick; };
static std::mutex                 g_smtx;
static std::map<void*, SessState> g_sessions;
static void*                      g_activeCCS = nullptr;   // 最近有输入事件的会话(前台)

// SEH 安全读取 CCS+OFF 处 std::string 的字节到 POD 缓冲(无 C++ 对象，可用 __try)
static int safe_copy_devid(void* ccs, char* buf, int bufsz) {
    __try {
        char* s = (char*)ccs + CCS_DEVICE_ID_OFF;
        size_t len = *(size_t*)(s + 16);
        size_t cap = *(size_t*)(s + 24);
        const char* p = (cap >= 16) ? *(const char**)s : s;
        if (!p || len == 0 || len >= (size_t)bufsz) return 0;   // 用 size_t 比较，len 是垃圾大值也不会变负绕过
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


// 取/建会话状态(持 g_smtx)
static SessState& sessOf(void* ccs) {
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) return it->second;
    SessState s;
    s.viewOnly   = cfg::g_viewOnly.load();   // 新会话默认值
    s.clipSync   = cfg::g_clipSync.load();
    s.gamepadOff = cfg::g_gamepadOff.load();
    s.devid       = read_device_id(ccs);       // 用于去重
    s.name        = s.devid;                    // 回退显示名，随后由窗口标题覆盖
    s.lastNameTick= 0;
    uu_log("session new: ccs=%p devid=%ls viewOnly=%d", ccs, s.devid.c_str(), (int)s.viewOnly);
    return g_sessions.emplace(ccs, std::move(s)).first->second;
}
// 输入 hook 用：记录活动会话，返回该会话是否仅浏览。
// 设备名取自前台窗口标题（UU 把视频窗口标题设成了设备名），每秒最多抓一次。
// 抓标题不能在持锁时做：同进程窗口的 GetWindowText 会同步发 WM_GETTEXT 回 UI 线程，
// 持锁期间跑宿主代码有重入死锁风险。所以锁内只标记活动会话，锁外读标题，再短暂回锁写回。
static bool input_viewOnly(void* ccs) {
    bool vo, wantName = false;
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        g_activeCCS = ccs;
        SessState& s = sessOf(ccs);
        vo = s.viewOnly;
        DWORD now = GetTickCount();
        if (now - s.lastNameTick >= 1000) { s.lastNameTick = now; wantName = true; }
    }
    if (wantName) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        wchar_t t[128];
        int n = (fg && pid == GetCurrentProcessId()) ? GetWindowTextW(fg, t, 128) : 0;  // 只认本进程窗口
        if (n > 0) {
            std::lock_guard<std::mutex> lk(g_smtx);
            auto it = g_sessions.find(ccs);
            if (it != g_sessions.end()) it->second.name.assign(t, n);
        }
    }
    return vo;
}
static bool active_viewOnly() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load();
    return sessOf(g_activeCCS).viewOnly;
}
static bool active_clipSync() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_clipSync.load();
    return sessOf(g_activeCCS).clipSync;
}
static bool active_gpBlock() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load() || cfg::g_gamepadOff.load();
    auto& s = sessOf(g_activeCCS);
    return s.viewOnly || s.gamepadOff;
}

// 仅浏览：拦掉输入发送
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
// 仅浏览时别让它锁鼠标，把 enable 当成 0
static void __fastcall h_enableCapture(void* thiz, unsigned __int8 enable, char toast, char a4) {
    if (active_viewOnly()) enable = 0;
    o_enableCapture(thiz, enable, toast, a4);
}

// 剪贴板
static void __fastcall h_clipUpdate(void* thiz) {
    if (!active_clipSync()) return;
    o_clipUpdate(thiz);
}
static __int64 __fastcall h_clipHandle(void* thiz, void* a2, void* a3) {
    if (!active_clipSync()) return 0;
    return o_clipHandle(thiz, a2, a3);
}

// 手柄
static std::mutex        g_gpMtx;
static void*             g_gpMgr = nullptr;
static std::set<uint8_t> g_desired;          // 物理存在的手柄索引
// 切换仅浏览/禁手柄时，把已连手柄在被控端拔掉或重连。
// 先在锁内把实例和索引拷出来，解锁后再调原始函数——不在锁里跑宿主代码。
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

// 给托盘菜单用，声明在 session.h
std::vector<SessSnap> sessions_snapshot() {
    std::lock_guard<std::mutex> lk(g_smtx);
    std::vector<SessSnap> v;
    for (auto& kv : g_sessions) {
        const std::wstring& disp = !kv.second.name.empty() ? kv.second.name : kv.second.devid;
        v.push_back({ kv.first, disp, kv.second.viewOnly, kv.second.clipSync, kv.second.gamepadOff });
    }
    return v;
}
// field: 0=viewOnly 1=clipSync 2=gamepadOff ; 返回切换后的值
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
    if (field == 0 && nv) ClipCursor(nullptr);   // 立即释放鼠标
    if (doGpReconcile) gp_reconcile(block);
    uu_log("session_toggle key=%p field=%d -> %d", key, field, (int)nv);
    return nv;
}

// 会话注册/移除。UU 断开时不销毁 CCS（留着重连），所以不能 hook 析构，只能 hook 关闭和退出。
using fn4_t = __int64(__fastcall*)(void*, void*, void*, void*);
static fn4_t o_setConnInfo = nullptr, o_closeConn = nullptr, o_exitRoom = nullptr;

static void session_remove(void* ccs) {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (g_sessions.erase(ccs)) uu_log("session remove: ccs=%p", ccs);
    if (g_activeCCS == ccs) g_activeCCS = nullptr;
}
static __int64 __fastcall h_setConnInfo(void* ccs, void* a2, void* a3, void* a4) {
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        std::wstring devid = read_device_id(ccs);
        if (!devid.empty())  // 去重：移除同设备的旧(stale)会话
            for (auto it = g_sessions.begin(); it != g_sessions.end(); )
                it = (it->first != ccs && it->second.devid == devid) ? g_sessions.erase(it) : std::next(it);
        g_activeCCS = ccs;
        sessOf(ccs);   // 注册+置活动(名字待首次操作时由窗口标题抓取)
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

// 仅浏览时把光标显示成禁用图标，同时挡掉远端光标同步
using fn_curs_t = __int64(__fastcall*)(void*, unsigned int);
static fn_curs_t o_updateCursor = nullptr;
static __int64 __fastcall h_updateCursor(void* vw, unsigned int force) {
    if (active_viewOnly()) {
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_NO));  // 禁用标志，且不应用远端光标
        return 1;
    }
    return o_updateCursor(vw, force);
}

static bool g_verKnown = false;   // 当前 GameViewer 版本号是否在 offsets 表里

// 给托盘“调试信息”用：每个 hook 点的定位结果、模块基址、版本号
static std::mutex g_dbgMtx;
static std::vector<HookStat> g_hookStats;
static std::wstring g_gvVersion = L"?";
static uintptr_t g_gvBase = 0;

static void record_hook(const char* name, void* addr, const char* how, bool ok) {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    g_hookStats.push_back({ name, addr, how, ok });
}

// 版本认识就先用精确 RVA（字符串只做比对校验）；版本不认识就字符串 → AOB，绝不套别的版本的 RVA。
static bool mk(const resolver::ModRange& r, uintptr_t base, std::initializer_list<const char*> anchors,
               const ver::Target& t, void* detour, void** orig, const char* name) {
    uintptr_t byStr = anchors.size() ? resolver::find_func(r, anchors) : 0;
    uintptr_t tgt = 0; const char* how = "";
    if (g_verKnown && t.rva) {
        tgt = base + t.rva; how = "rva";
        if (byStr && byStr != tgt) uu_log("%s: str %p != rva %p, keep rva", name, (void*)byStr, (void*)tgt);
        else if (!byStr) uu_log("%s: string resolve missed (更新后会失效)", name);
    } else if (byStr) {
        tgt = byStr; how = "str";
    } else {
        uintptr_t byAob = resolver::find_func_by_aob(r, t.aob);
        if (byAob) { tgt = byAob; how = "aob"; }
    }
    if (!tgt) { uu_log("resolve %s failed, skip", name); record_hook(name, nullptr, "", false); return false; }
    if (MH_CreateHook((void*)tgt, detour, orig) != MH_OK) { uu_log("CreateHook %s failed", name); record_hook(name, (void*)tgt, how, false); return false; }
    if (MH_EnableHook((void*)tgt) != MH_OK) { uu_log("EnableHook %s failed", name); record_hook(name, (void*)tgt, how, false); return false; }
    uu_log("hooked %s @ %p (%s)", name, (void*)tgt, how);
    record_hook(name, (void*)tgt, how, true);
    return true;
}

void install_hooks(uintptr_t base) {
    if (MH_Initialize() != MH_OK) { uu_log("MH_Initialize failed"); return; }
    std::wstring vs = cfg::exe_version();
    const ver::VerSet& V = ver::pick(vs.c_str());
    g_verKnown = (!vs.empty() && vs == V.version);
    g_gvBase = base;
    g_gvVersion = vs.empty() ? L"?" : vs;
    CCS_DEVICE_ID_OFF = V.deviceIdOff;
    uu_log("GameViewer version=%ls known=%d", vs.empty() ? L"?" : vs.c_str(), (int)g_verKnown);
    resolver::ModRange r{};
    resolver::get_ranges((HMODULE)base, r);
    mk(r, base, {"ControlConnectionSession::sendMouseEvent", "[control] mouseObj size 0"},      V.sendMouse,    (void*)h_sendMouse, (void**)&o_sendMouse, "sendMouseEvent");
    mk(r, base, {"ControlConnectionSession::sendMouseWheel", "sendMouseWheel failed, session_config_ handle invalid"}, V.sendWheel, (void*)h_sendWheel, (void**)&o_sendWheel, "sendMouseWheel");
    mk(r, base, {"ControlConnectionSession::sendKeyboardEvent"}, V.sendKey, (void*)h_sendKey,   (void**)&o_sendKey,   "sendKeyboardEvent");
    mk(r, base, {"VideoUi::VideoWidget::enabledCaptureMouse", "==== Enabled capture mouse: ", "Cursor not in rect"}, V.enableCapture, (void*)h_enableCapture, (void**)&o_enableCapture, "enabledCaptureMouse");
    mk(r, base, {"GamepadManager::Connect(), index=", "GamepadManager::Connect"},       V.gpConnect,    (void*)h_gamepadConnect,    (void**)&o_gamepadConnect,    "GamepadManager::Connect");
    mk(r, base, {"GamepadManager::Disconnect(), index=", "GamepadManager::Disconnect"}, V.gpDisconnect, (void*)h_gamepadDisconnect, (void**)&o_gamepadDisconnect, "GamepadManager::Disconnect");
    mk(r, base, {"[%d] GamepadManager::Update(), json=%s", "GamepadManager::Update"},    V.gpUpdate,   (void*)h_gamepadUpdate,     (void**)&o_gamepadUpdate,     "GamepadManager::Update");
    mk(r, base, {"Clipboard::on_clipboard_update", "Get clipboard data failed"},         V.clipUpdate, (void*)h_clipUpdate, (void**)&o_clipUpdate, "on_clipboard_update");
    mk(r, base, {"Clipboard::handle_clipboard_request", "Received auto_save_complete: total="}, V.clipHandle, (void*)h_clipHandle, (void**)&o_clipHandle, "handle_clipboard_request");
    // 会话注册/移除
    mk(r, base, {"ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: "}, V.setConnInfo, (void*)h_setConnInfo, (void**)&o_setConnInfo, "setConnectInfo");
    mk(r, base, {"ControlConnectionSession::closeControlConnect"}, V.closeConn, (void*)h_closeConn,   (void**)&o_closeConn,   "closeControlConnect");
    mk(r, base, {"ControlConnectionSession::exitRoom"},            V.exitRoom,  (void*)h_exitRoom,    (void**)&o_exitRoom,    "exitRoom");
    // 光标
    mk(r, base, {"VideoUi::VideoWidget::updateCursor", "set cursor by id", "Default set arrow cursor"}, V.updateCursor, (void*)h_updateCursor, (void**)&o_updateCursor, "updateCursor");
    uu_log("install_hooks done");
}

DebugInfo debug_snapshot() {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    DebugInfo d;
    d.gvVersion = g_gvVersion;
    d.gvKnown = g_verKnown;
    d.gvBase = g_gvBase;
    d.hooks = g_hookStats;
    return d;
}
