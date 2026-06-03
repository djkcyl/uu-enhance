#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include "app.h"
#include "config.h"
#include "log.h"
#include "session.h"

#define WM_TRAY (WM_APP + 17)
// 菜单命令编码：每个会话 3 个开关。cmd = ID_SESS_BASE + sessionIndex*10 + field
enum { ID_GITHUB = 1, ID_SESS_BASE = 2000 };

static HWND  g_wnd = nullptr;
static NOTIFYICONDATAW g_nid{};
static std::vector<SessSnap> g_lastSnap;   // 与菜单 index 对应

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

    // 调试信息：版本 + 各 hook 点定位情况
    {
        DebugInfo dbg = debug_snapshot();
        HMENU sub = CreatePopupMenu();
        int okN = 0; for (const auto& h : dbg.hooks) if (h.ok) ++okN;
        wchar_t line[256];
        swprintf_s(line, L"GameViewer %ls (%ls)", dbg.gvVersion.c_str(), dbg.gvKnown ? L"已识别" : L"未识别");
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        swprintf_s(line, L"已挂 %d/%d 个 hook", okN, (int)dbg.hooks.size());
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        for (const auto& h : dbg.hooks) {
            if (h.ok)
                swprintf_s(line, L"%hs  %hs +0x%llX", h.name.c_str(), h.how.c_str(),
                           (unsigned long long)((uintptr_t)h.addr - dbg.gvBase));
            else if (h.addr)
                swprintf_s(line, L"%hs  挂钩失败", h.name.c_str());
            else
                swprintf_s(line, L"%hs  未定位(已跳过)", h.name.c_str());
            AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        }
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

// 在基础图标右下角叠加一个小齿轮(Segoe MDL2 Assets U+E713)，区分补丁托盘和 UU 自己的图标。
// GDI 文字渲染不写 alpha，所以在临时 DIB 上画字形取覆盖范围，再手动合成到主图标。
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
    wchar_t glyph[] = { 0xE713, 0 };

    std::memset(tp, 0, n * 4);
    SetTextColor(tmp, RGB(255, 255, 255));
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            TextOutW(tmp, ox + dx, oy + dy, glyph, 1);
    GdiFlush();  // 读 DIB 像素前必须刷，否则 GDI 还没把字形写进内存
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

    // mask 全黑，让 alpha 完全由 color bitmap 的 per-pixel alpha 决定
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
