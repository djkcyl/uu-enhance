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

// CCS 内 device_id (std::string) 偏移，由所选版本表设置；仅用于去重/回退名(读错不影响分会话)
static uintptr_t CCS_DEVICE_ID_OFF = 3984;
// VideoMainWindow 内 deviceId / 标题(好友名) QString 偏移，由所选版本表设置
static uintptr_t VMW_DEVICE_ID_OFF = 344;
static uintptr_t VMW_TITLE_OFF     = 352;
// 偏移是否由运行时自动推导得到(而非套版本表)。给托盘调试面板显示用。
static bool      g_devIdAuto  = false;
static bool      g_vmwOffAuto = false;

// 每个会话的状态，用 CCS 指针做 key。显示名不存这里，快照时按 devid 现查 VideoMainWindow 标题。
struct SessState { bool viewOnly; bool clipSync; bool gamepadOff; std::wstring devid; };
static std::mutex                 g_smtx;
static std::map<void*, SessState> g_sessions;
static void*                      g_activeCCS = nullptr;   // 最近有输入事件的会话(前台)

// deviceId -> VideoMainWindow 实例(该会话的视频窗)。构造时登记，快照时按 devid 反查取标题。
// 按 devid 字符串匹配而非裸指针：既躲开多重继承指针偏移问题，也让悬空指针无害
// (死窗口的 devid 对不上任何在线会话，快照里会重新核对 devid，对不上就丢弃)。持 g_smtx。
static std::map<std::wstring, void*> g_devidToVmw;

// 剪贴板角色区分(持 g_smtx)：每条连接各有独立的 Clipboard 实例(thiz)。
static void*                      g_serverClip = nullptr;  // 被控侧 Clipboard 实例
static std::map<void*, void*>     g_clipToCcs;             // 主控 Clipboard 实例 -> 所属 CCS
static thread_local void*         t_curClip = nullptr;     // 当前处理中的 Clipboard(给 get_clipboard_data 查)

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

// SEH 安全读取一个 Qt5 QString 的 UTF-16 内容到 POD 缓冲(无 C++ 对象，可用 __try)。
// qsHolder 指向 QString 对象；其首成员是 d 指针，指向 QArrayData：
//   [+4]int size  [+16]qptrdiff offset，字符在 (char*)d + offset。悬空/垃圾指针经 SEH 兜住。
static int safe_copy_qstr(const void* qsHolder, wchar_t* buf, int cap) {
    __try {
        const unsigned char* d = *(const unsigned char* const*)qsHolder;
        if (!d) return 0;
        int size = *(const int*)(d + 4);
        long long off = *(const long long*)(d + 16);
        if (size <= 0 || size >= cap) return 0;   // size 是垃圾大值也挡掉
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

// —— 结构体偏移的运行时自动推导：让升级免手工找地址 ——
// 思路：这些偏移就编码在“会自愈”的函数体/传参里，运行时抠出来即可，写死的版本表只兜底。
//
// deviceIdOff：指令扫描 setConnectInfo。UU 自己会 `append(stream, "..device_id: ")` 再
// `append(stream, this->device_id)`——即定位那句日志串的引用，其后第一条
// `lea reg,[GP基址+disp32]`(大位移，非栈基址)的 disp32 就是 device_id 成员偏移。失败返回 0。
static uintptr_t derive_off_after_str(const resolver::ModRange& r, uintptr_t func, const char* anchorStr) {
    if (!func) return 0;
    uintptr_t sa = resolver::find_string(r, anchorStr);
    if (!sa) return 0;
    uint8_t* p0 = (uint8_t*)func;
    uint8_t* end = p0 + 0x400;
    if ((uintptr_t)end > r.text_end) end = (uint8_t*)r.text_end;
    // 1) 找到指向该串的 RIP 相对 lea(48/4C 8D，mod=00 rm=101)
    uint8_t* strLea = nullptr;
    for (uint8_t* q = p0; q + 7 <= end; ++q)
        if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05) {
            if ((uintptr_t)(q + 7) + *(int32_t*)(q + 3) == sa) { strLea = q; break; }
        }
    if (!strLea) return 0;
    // 2) 其后近处第一条 48/49 8D，mod=10(disp32)，rm∉{rsp=4, rbp=5} 的 lea reg,[base+disp32]
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

// vmwDevIdOff/vmwTitleOff：数据驱动。首个 VideoMainWindow 构造时，手上有 deviceId 的 QString(devidQs)，
// 拿它的值在 this 里逐 8 字节比对，命中处就是 deviceId 成员偏移；标题是紧邻的下一个 QString(=+8)。
// 不碰指令编码，不假设寄存器分配。命中才覆盖，读到的都经 SEH+值匹配，安全。
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


// 取/建会话状态(持 g_smtx)
static SessState& sessOf(void* ccs) {
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) return it->second;
    SessState s;
    s.viewOnly   = cfg::g_viewOnly.load();   // 新会话默认值
    s.clipSync   = cfg::g_clipSync.load();
    s.gamepadOff = cfg::g_gamepadOff.load();
    s.devid       = read_device_id(ccs);       // 去重 + 反查 VideoMainWindow 标题的 key
    uu_log("session new: ccs=%p devid=%ls viewOnly=%d", ccs, s.devid.c_str(), (int)s.viewOnly);
    return g_sessions.emplace(ccs, std::move(s)).first->second;
}
// 输入 hook 用：记录活动会话，返回该会话是否仅浏览。
// (显示名不再靠前台窗口标题猜——见 sessions_snapshot 按 devid 直接读 VideoMainWindow 标题。)
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
// 剪贴板按“哪个 Clipboard 实例(clip)在处理”区分角色，实现主控/被控独立开关、互不连累：
//  - 每条主控会话有独立的 Clipboard 实例；被控侧也有一个独立实例(无主控会话时出现的那个)。
//  - clip == g_serverClip  → 被控：按全局开关 cfg::g_ctrlClip。
//  - 其它 clip             → 主控：按该实例绑定的会话 clipSync（首次在某主控会话活动时出现即绑定）。
// g_serverClip 在“无主控会话(g_activeCCS 为空)”时出现的实例上学习，稳定后固定；此时也把它从
// 主控映射里剔除，防止早期未学习时被误绑。持 g_smtx 调用。
static bool clip_allowed_locked(void* clip) {
    if (!clip) {   // 未知来源(拿不到实例) → 回退到活动会话/全局，尽量放行
        if (g_activeCCS) return sessOf(g_activeCCS).clipSync;
        return true;
    }
    if (!g_activeCCS) {          // 无主控会话 → 当前活动的必是被控/本机实例
        g_serverClip = clip;
        g_clipToCcs.erase(clip);
    }
    if (clip == g_serverClip) return cfg::g_ctrlClip.load();   // 被控
    // 主控：绑定实例到会话
    auto it = g_clipToCcs.find(clip);
    void* ccs = (it != g_clipToCcs.end()) ? it->second : nullptr;
    if (!ccs && g_activeCCS) { g_clipToCcs[clip] = g_activeCCS; ccs = g_activeCCS; }
    if (ccs) { auto s = g_sessions.find(ccs); if (s != g_sessions.end()) return s->second.clipSync; }
    return true;   // 认不出的主控实例 → 放行
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

// 出向：本地剪贴板一变就外推给对端。thiz 就是这条连接的 Clipboard 实例。它内部会调
// get_clipboard_data 读本地剪贴板，所以进原函数前把 t_curClip 设成本实例，供其查角色。
static void __fastcall h_clipUpdate(void* thiz) {
    if (!clip_allowed(thiz)) return;
    void* prev = t_curClip; t_curClip = thiz;
    o_clipUpdate(thiz);
    t_curClip = prev;
}
// 出向格式表通告(主动发)：关则不发，对端不会 EmptyClipboard。
static __int64 __fastcall h_clipSendFmt(void* thiz) {
    if (!clip_allowed(thiz)) return 0;
    return o_clipSendFmt(thiz);
}
// 入向格式表落地：关则不处理对端通告(不接收对端剪贴板)。
static __int64 __fastcall h_clipFmtList(void* thiz, void* a2, void* a3) {
    if (!clip_allowed(thiz)) return 0;
    return o_clipFmtList(thiz, a2, a3);
}
// 中央请求分发器 handle_clipboard_request：不能掐(掐了握手会卡死对端)，只用它带的 thiz
// 标出“当前正在为哪个 Clipboard 处理请求”，供内部 get_clipboard_data 查角色，照常放行。
static __int64 __fastcall h_clipReq(void* thiz, void* a2, void* a3) {
    void* prev = t_curClip; t_curClip = thiz;
    __int64 r = o_clipReq(thiz, a2, a3);
    t_curClip = prev;
    return r;
}
// 出向数据服务点：对端粘贴时来拉本机剪贴板，经此读本地剪贴板应答。它不带会话/实例，用
// t_curClip(由上面两个入口设置)判角色。关则把输出 std::string 置空、返回失败——等同剪贴板
// 为空(app 正常分支)，对端拿到空、不卡，本地剪贴板也没被读出去。
// out 是 MSVC std::string：[0..15]SSO/指针 [16]size [24]cap。
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

// VideoMainWindow 构造函数：每开一个会话视频窗就来一次。构造参数里带 deviceId 和该会话的
// CCS(shared_ptr)。这里在原函数跑完后按 thiz+偏移读 deviceId，登记 devid -> 本窗口实例，
// 供托盘快照按 devid 反查窗口标题(=好友名)。标题此刻多半还空，所以快照时才现读，不在这里存。
static __int64 __fastcall h_vmwCtor(void* thiz, void* devidQs, void* a3, void* sp, int a5, __int64 a6) {
    __int64 r = o_vmwCtor(thiz, devidQs, a3, sp, a5, a6);
    if (!g_vmwDerived) { g_vmwDerived = true; derive_vmw_off(thiz, devidQs); }  // 首个窗口时自动定偏移
    std::wstring devid = read_qstring((char*)thiz + VMW_DEVICE_ID_OFF);
    if (!devid.empty()) {
        std::lock_guard<std::mutex> lk(g_smtx);
        g_devidToVmw[devid] = thiz;
        uu_log("vmw registered: devid=%ls vmw=%p", devid.c_str(), thiz);
    }
    return r;
}

// 给托盘菜单用，声明在 session.h
std::vector<SessSnap> sessions_snapshot() {
    std::lock_guard<std::mutex> lk(g_smtx);
    std::vector<SessSnap> v;
    for (auto& kv : g_sessions) {
        std::wstring disp = kv.second.devid;   // 回退显示名：deviceId
        auto it = g_devidToVmw.find(kv.second.devid);
        if (!kv.second.devid.empty() && it != g_devidToVmw.end()) {
            void* vmw = it->second;
            // 重新核对该窗口当前的 deviceId：对得上才是本会话活着的窗口(挡悬空/复用)。
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
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) {
        if (!it->second.devid.empty()) g_devidToVmw.erase(it->second.devid);   // 摘掉该会话的窗口映射
        g_sessions.erase(it);
        uu_log("session remove: ccs=%p", ccs);
    }
    if (g_activeCCS == ccs) g_activeCCS = nullptr;
    for (auto jt = g_clipToCcs.begin(); jt != g_clipToCcs.end(); )   // 解绑该会话的 Clipboard 实例
        jt = (jt->second == ccs) ? g_clipToCcs.erase(jt) : std::next(jt);
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
    VMW_DEVICE_ID_OFF = V.vmwDevIdOff;
    VMW_TITLE_OFF     = V.vmwTitleOff;
    uu_log("GameViewer version=%ls known=%d", vs.empty() ? L"?" : vs.c_str(), (int)g_verKnown);
    resolver::ModRange r{};
    resolver::get_ranges((HMODULE)base, r);
    // device_id 成员偏移：优先从 setConnectInfo 指令里自动抠(抗更新)，抠不到才用版本表
    {
        uintptr_t scfn = (g_verKnown && V.setConnInfo.rva) ? base + V.setConnInfo.rva
            : resolver::find_func(r, {"ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: "});
        uintptr_t d = derive_off_after_str(r, scfn, "startConnectOtherDevice, device_id: ");
        if (d) { CCS_DEVICE_ID_OFF = d; g_devIdAuto = true; }
        uu_log("deviceIdOff: table=%llu derived=%llu use=%llu", (unsigned long long)V.deviceIdOff,
               (unsigned long long)d, (unsigned long long)CCS_DEVICE_ID_OFF);
    }
    mk(r, base, {"ControlConnectionSession::sendMouseEvent", "[control] mouseObj size 0"},      V.sendMouse,    (void*)h_sendMouse, (void**)&o_sendMouse, "sendMouseEvent");
    mk(r, base, {"ControlConnectionSession::sendMouseWheel", "sendMouseWheel failed, session_config_ handle invalid"}, V.sendWheel, (void*)h_sendWheel, (void**)&o_sendWheel, "sendMouseWheel");
    mk(r, base, {"ControlConnectionSession::sendKeyboardEvent"}, V.sendKey, (void*)h_sendKey,   (void**)&o_sendKey,   "sendKeyboardEvent");
    mk(r, base, {"VideoUi::VideoWidget::enabledCaptureMouse", "==== Enabled capture mouse: ", "Cursor not in rect"}, V.enableCapture, (void*)h_enableCapture, (void**)&o_enableCapture, "enabledCaptureMouse");
    mk(r, base, {"GamepadManager::Connect(), index=", "GamepadManager::Connect"},       V.gpConnect,    (void*)h_gamepadConnect,    (void**)&o_gamepadConnect,    "GamepadManager::Connect");
    mk(r, base, {"GamepadManager::Disconnect(), index=", "GamepadManager::Disconnect"}, V.gpDisconnect, (void*)h_gamepadDisconnect, (void**)&o_gamepadDisconnect, "GamepadManager::Disconnect");
    mk(r, base, {"[%d] GamepadManager::Update(), json=%s", "GamepadManager::Update"},    V.gpUpdate,   (void*)h_gamepadUpdate,     (void**)&o_gamepadUpdate,     "GamepadManager::Update");
    mk(r, base, {"Clipboard::on_clipboard_update", "Get clipboard data failed"},         V.clipUpdate, (void*)h_clipUpdate, (void**)&o_clipUpdate, "on_clipboard_update");
    mk(r, base, {"Clipboard::do_handle_format_list_request", "do_handle_format_list_request: is_file_transferring=true"}, V.clipFmtList, (void*)h_clipFmtList, (void**)&o_clipFmtList, "do_handle_format_list_request");
    mk(r, base, {"Clipboard::get_clipboard_data", "GlobalLock failed: "}, V.clipGet, (void*)h_clipGet, (void**)&o_clipGet, "get_clipboard_data");
    mk(r, base, {"Clipboard::do_send_format_list", "do_send_format_list: send_request failed"}, V.clipSendFmt, (void*)h_clipSendFmt, (void**)&o_clipSendFmt, "do_send_format_list");
    mk(r, base, {"Clipboard::handle_clipboard_request", "Received auto_save_complete: total="}, V.clipReq, (void*)h_clipReq, (void**)&o_clipReq, "handle_clipboard_request");
    // 会话注册/移除
    mk(r, base, {"ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: "}, V.setConnInfo, (void*)h_setConnInfo, (void**)&o_setConnInfo, "setConnectInfo");
    mk(r, base, {"ControlConnectionSession::closeControlConnect"}, V.closeConn, (void*)h_closeConn,   (void**)&o_closeConn,   "closeControlConnect");
    mk(r, base, {"ControlConnectionSession::exitRoom"},            V.exitRoom,  (void*)h_exitRoom,    (void**)&o_exitRoom,    "exitRoom");
    // 视频窗构造：登记 deviceId -> 窗口，供托盘取好友名(标题)
    mk(r, base, {"home_control_session_start: window_created, device_id="}, V.vmwCtor, (void*)h_vmwCtor, (void**)&o_vmwCtor, "VideoMainWindow::ctor");
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
    d.devIdOff = CCS_DEVICE_ID_OFF;
    d.vmwDevIdOff = VMW_DEVICE_ID_OFF;
    d.vmwTitleOff = VMW_TITLE_OFF;
    d.devIdAuto = g_devIdAuto;
    d.vmwAuto = g_vmwOffAuto;
    d.hooks = g_hookStats;
    return d;
}
