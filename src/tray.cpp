#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "app.h"
#include "config.h"
#include "log.h"
#include "session.h"

#define WM_TRAY (WM_APP + 17)
// cmd = ID_SESS_BASE + sessionIndex*10 + field
enum { ID_GITHUB = 1, ID_CTRL_CLIP = 2, ID_SRV_VIEWONLY = 3, ID_SRV_FEAT_BASE = 100, ID_SESS_BASE = 2000 };

struct SrvFeatItem { cfg::SrvFeat bit; const wchar_t* name; };
static const SrvFeatItem kSrvFeats[] = {
    { cfg::SF_INPUT,    L"输入（鼠标/键盘/手柄）" },
    { cfg::SF_TERMINAL, L"终端" },
    { cfg::SF_PORTMAP,  L"端口映射" },
    { cfg::SF_FILE,     L"文件传输" },
    { cfg::SF_DISPLAY,  L"改分辨率/DPI/关显示器/翻屏" },
    { cfg::SF_PRIVACY,  L"隐私屏/锁屏" },
    { cfg::SF_AUDIO,    L"麦克风/静音" },
    { cfg::SF_POWER,    L"关机/重启/唤醒/自启" },
    { cfg::SF_LAUNCH,   L"启动应用" },
    { cfg::SF_VDISPLAY, L"虚拟屏/超级屏" },
    { cfg::SF_TEXT,     L"文本注入" },
};

static HWND  g_wnd = nullptr;
static NOTIFYICONDATAW g_nid{};
static std::vector<SessSnap> g_lastSnap;

static void show_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();

    g_lastSnap = sessions_snapshot();
    if (g_lastSnap.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(未连接远程会话)");
    } else {
        for (size_t i = 0; i < g_lastSnap.size(); ++i) {
            const auto& s = g_lastSnap[i];
            HMENU sub = CreatePopupMenu();
            int b = ID_SESS_BASE + (int)i * 10;
            AppendMenuW(sub, MF_STRING | (s.viewOnly   ? MF_CHECKED : 0), b + 0, L"仅浏览模式");
            AppendMenuW(sub, MF_STRING | (s.clipSync   ? MF_CHECKED : 0), b + 1, L"剪贴板同步");
            AppendMenuW(sub, MF_STRING | (s.gamepadOff ? MF_CHECKED : 0), b + 2, L"禁止手柄转发");
            wchar_t title[160];
            if (!s.name.empty()) wsprintfW(title, L"会话 %d：%ls", (int)i + 1, s.name.c_str());
            else                 wsprintfW(title, L"会话 %d", (int)i + 1);
            AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, title);
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING | (cfg::g_ctrlClip.load() ? MF_CHECKED : 0), ID_CTRL_CLIP, L"被控时允许剪贴板");
    AppendMenuW(m, MF_STRING | (cfg::g_srvViewOnly.load() ? MF_CHECKED : 0), ID_SRV_VIEWONLY, L"被控时仅浏览（对方只能看）");
    {
        uint32_t mask = cfg::g_srvBlockMask.load();
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < (int)(sizeof(kSrvFeats) / sizeof(kSrvFeats[0])); ++i)
            AppendMenuW(sub, MF_STRING | ((mask & kSrvFeats[i].bit) ? MF_CHECKED : 0),
                        ID_SRV_FEAT_BASE + i, kSrvFeats[i].name);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"　└ 仅浏览拦截项（勾=拦）");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    {
        DebugInfo dbg = debug_snapshot();
        HMENU sub = CreatePopupMenu();
        wchar_t line[256];
        swprintf_s(line, L"GameViewer %ls (%ls)", dbg.gvVersion.c_str(), dbg.gvKnown ? L"known" : L"unknown");
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        swprintf_s(line, L"offsets dev=%llu(%ls) vmw=%llu/%llu(%ls)",
                   (unsigned long long)dbg.devIdOff, dbg.devIdAuto ? L"auto" : L"table",
                   (unsigned long long)dbg.vmwDevIdOff, (unsigned long long)dbg.vmwTitleOff,
                   dbg.vmwAuto ? L"auto" : L"table");
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);

        auto addGroup = [&](const wchar_t* role, const wchar_t* label) {
            std::vector<const DbgLine*> items;
            for (const auto& h : dbg.hooks)
                if (wcscmp(h.role, role) == 0) items.push_back(&h);
            std::sort(items.begin(), items.end(), [](const DbgLine* a, const DbgLine* b) {
                if (a->ok != b->ok) return !a->ok;                 // 未定位的排最前
                if (int c = a->how.compare(b->how)) return c < 0;  // 再按类型(str/exp/aob)分块
                return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
            });
            HMENU g = CreatePopupMenu();
            int ok = 0;
            for (const DbgLine* h : items) {
                if (h->ok) ++ok;
                if (!h->ok)      swprintf_s(line, L"%hs  not found", h->name.c_str());
                else if (h->off) swprintf_s(line, L"%hs  %hs +0x%llX", h->name.c_str(), h->how.c_str(), h->off);
                else             swprintf_s(line, L"%hs  %hs", h->name.c_str(), h->how.c_str());
                AppendMenuW(g, MF_STRING | MF_GRAYED, 0, line);
            }
            wchar_t glabel[64];
            if (items.empty()) swprintf_s(glabel, L"%ls", label);
            else               swprintf_s(glabel, L"%ls（%d/%d）", label, ok, (int)items.size());
            AppendMenuW(sub, MF_POPUP | (items.empty() ? MF_GRAYED : 0), (UINT_PTR)g, glabel);
        };
        addGroup(L"ctl", L"主控");
        addGroup(L"srv", dbg.serverRunning ? L"被控" : L"被控（未运行）");

        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"调试信息");
    }

    AppendMenuW(m, MF_STRING, ID_GITHUB, L"项目主页 (GitHub)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"UU远程增强 v" UURE_VERSION_W);

    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);

    if (cmd == ID_GITHUB) {
        ShellExecuteW(nullptr, L"open", UURE_GITHUB_W, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == ID_CTRL_CLIP) {
        cfg::g_ctrlClip = !cfg::g_ctrlClip.load();
        cfg::save();
    } else if (cmd == ID_SRV_VIEWONLY) {
        cfg::g_srvViewOnly = !cfg::g_srvViewOnly.load();
        cfg::save();
    } else if (cmd >= ID_SRV_FEAT_BASE && cmd < ID_SRV_FEAT_BASE + (int)(sizeof(kSrvFeats) / sizeof(kSrvFeats[0]))) {
        cfg::g_srvBlockMask = cfg::g_srvBlockMask.load() ^ kSrvFeats[cmd - ID_SRV_FEAT_BASE].bit;
        cfg::save();
    } else if (cmd >= ID_SESS_BASE) {
        int idx = (cmd - ID_SESS_BASE) / 10, field = (cmd - ID_SESS_BASE) % 10;
        if (idx >= 0 && idx < (int)g_lastSnap.size() && field >= 0 && field <= 2)
            session_toggle(g_lastSnap[idx].key, field);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TRAY) {
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) show_menu(h);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static HICON overlayGear(HICON base) {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    int n = cx * cy;

    HDC screen = GetDC(nullptr);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = cx; bih.biHeight = -cy;
    bih.biPlanes = 1; bih.biBitCount = 32; bih.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(screen);
    DWORD* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, (BITMAPINFO*)&bih, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(px, 0, n * 4);
    DrawIconEx(dc, 0, 0, base, cx, cy, 0, nullptr, DI_NORMAL);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);

    HDC tmp = CreateCompatibleDC(screen);
    DWORD* tp = nullptr;
    HBITMAP tbmp = CreateDIBSection(tmp, (BITMAPINFO*)&bih, DIB_RGB_COLORS, (void**)&tp, nullptr, 0);
    HGDIOBJ oldTbmp = SelectObject(tmp, tbmp);

    int gs = cx * 9 / 16;
    if (gs < 7) gs = 7;
    HFONT font = CreateFontW(-gs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe MDL2 Assets");
    HGDIOBJ oldFont = SelectObject(tmp, font);
    SetBkMode(tmp, TRANSPARENT);
    int ox = cx - gs, oy = cy - gs;
    wchar_t glyph[] = { 0xE713, 0 };   // Segoe MDL2 Assets gear

    std::memset(tp, 0, n * 4);
    SetTextColor(tmp, RGB(255, 255, 255));
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            TextOutW(tmp, ox + dx, oy + dy, glyph, 1);
    GdiFlush();  // flush before reading DIB pixels
    for (int i = 0; i < n; i++)
        if (tp[i] & 0x00FFFFFF) px[i] = 0xFFFFFFFF;

    std::memset(tp, 0, n * 4);
    SetTextColor(tmp, RGB(80, 80, 80));
    TextOutW(tmp, ox, oy, glyph, 1);
    GdiFlush();
    for (int i = 0; i < n; i++)
        if (tp[i] & 0x00FFFFFF) px[i] = 0xFF000000 | (tp[i] & 0x00FFFFFF);

    SelectObject(tmp, oldFont);
    DeleteObject(font);
    SelectObject(tmp, oldTbmp);
    DeleteObject(tbmp);
    DeleteDC(tmp);

    // all-black mask: alpha comes from the color bitmap's per-pixel alpha
    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, nullptr);
    HDC mdc = CreateCompatibleDC(screen);
    HGDIOBJ oldM = SelectObject(mdc, mask);
    PatBlt(mdc, 0, 0, cx, cy, BLACKNESS);
    SelectObject(mdc, oldM);
    DeleteDC(mdc);
    ReleaseDC(nullptr, screen);

    ICONINFO ii{ TRUE, 0, 0, mask, bmp };
    HICON result = CreateIconIndirect(&ii);
    DeleteObject(bmp);
    DeleteObject(mask);
    return result ? result : base;
}

static DWORD WINAPI tray_thread(LPVOID) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = hInst;
    wc.lpszClassName = L"UUEnhanceTrayWnd";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_wnd) { uu_log("tray window create failed"); return 0; }

    HICON ico = nullptr;
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(GetModuleHandleW(nullptr), exePath, MAX_PATH))
        ico = ExtractIconW(hInst, exePath, 0);
    if (!ico || ico == (HICON)1) ico = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    ico = overlayGear(ico);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = ico;
    wcscpy_s(g_nid.szTip, L"UU远程增强 (右键按会话开关)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_nid.uFlags = NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"UU远程增强 v" UURE_VERSION_W);
    wcscpy_s(g_nid.szInfo, L"已成功加载。右键此图标，按会话开关：仅浏览/剪贴板/手柄。");
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    uu_log("tray icon added");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 0;
}
void start_tray() { CreateThread(nullptr, 0, tray_thread, nullptr, 0, nullptr); }
