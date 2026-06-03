// UU远程增强 一键安装器
#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include "resource.h"
#include "app.h"

#define WM_UPDATE_AVAILABLE (WM_APP + 100)

static constexpr int BANNER_H  = 68;
static constexpr int MARGIN    = 20;
static constexpr int CW        = 520;
static constexpr int WIN_W     = CW + MARGIN * 2;
static constexpr int WIN_H     = 420;

static constexpr COLORREF CLR_BANNER     = RGB(24, 90, 188);
static constexpr COLORREF CLR_BANNER_SUB = RGB(160, 195, 240);
static constexpr COLORREF CLR_GREEN      = RGB(22, 163, 74);
static constexpr COLORREF CLR_GRAY       = RGB(120, 120, 120);

static HWND  g_hwnd, g_path, g_log, g_state, g_pathHint, g_footer;
static HWND  g_btnInstall, g_btnUninstall;
static HFONT g_font, g_fontBold, g_fontTitle;
static HBRUSH g_brushBanner;
static std::wstring g_latestVer;

static void logln(const std::wstring& s) {
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    std::wstring line = s + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring parentDir(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

static std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t back = dir.back();
    return (back == L'\\' || back == L'/') ? dir + name : dir + L"\\" + name;
}

static std::wstring verString(const std::wstring& file, const wchar_t* field) {
    DWORD h = 0, sz = GetFileVersionInfoSizeW(file.c_str(), &h);
    if (!sz) return L"";
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(file.c_str(), 0, sz, buf.data())) return L"";
    struct LangCp { WORD lang, cp; } *tr = nullptr; UINT n = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &n) || !tr)
        return L"";
    wchar_t sub[64];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%ls", tr[0].lang, tr[0].cp, field);
    wchar_t* val = nullptr; UINT vn = 0;
    if (VerQueryValueW(buf.data(), sub, (LPVOID*)&val, &vn) && val) return val;
    return L"";
}

static bool isOurDll(const std::wstring& file) {
    return verString(file, L"ProductName") == L"UU Remote Enhance";
}

static std::wstring normalizeBinDir(const std::wstring& dir) {
    if (dir.empty()) return L"";
    if (fileExists(joinPath(dir, L"GameViewer.exe"))) return dir;
    std::wstring bin = joinPath(dir, L"bin");
    if (fileExists(joinPath(bin, L"GameViewer.exe"))) return bin;
    return L"";
}

static std::wstring runningGameViewerPath() {
    std::wstring out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"GameViewer.exe") != 0) continue;
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (ph) {
            wchar_t buf[MAX_PATH]; DWORD cb = MAX_PATH;
            if (QueryFullProcessImageNameW(ph, 0, buf, &cb)) out = buf;
            CloseHandle(ph);
        }
        if (!out.empty()) break;
    }
    CloseHandle(snap);
    return out;
}

static bool isGameViewerRunning() { return !runningGameViewerPath().empty(); }

static std::wstring fromRegistry() {
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    const wchar_t* root = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (REGSAM view : views) {
        HKEY hRoot;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | view, &hRoot) != ERROR_SUCCESS)
            continue;
        wchar_t sub[256]; DWORD i = 0, len;
        while (len = 256, RegEnumKeyExW(hRoot, i++, sub, &len, nullptr, nullptr, nullptr, nullptr)
                         == ERROR_SUCCESS) {
            HKEY hk;
            if (RegOpenKeyExW(hRoot, sub, 0, KEY_READ | view, &hk) != ERROR_SUCCESS) continue;
            auto readStr = [&](const wchar_t* name) -> std::wstring {
                wchar_t v[MAX_PATH]; DWORD t, cb = sizeof(v);
                if (RegQueryValueExW(hk, name, nullptr, &t, (LPBYTE)v, &cb) == ERROR_SUCCESS &&
                    (t == REG_SZ || t == REG_EXPAND_SZ))
                    return v;
                return L"";
            };
            std::wstring name = readStr(L"DisplayName");
            std::wstring lower = name;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            bool match = !name.empty() &&
                (lower.find(L"gameviewer") != std::wstring::npos ||
                 name.find(L"网易UU") != std::wstring::npos ||
                 name.find(L"UU远程") != std::wstring::npos);
            if (match) {
                std::wstring loc = readStr(L"InstallLocation");
                std::wstring bin = normalizeBinDir(loc);
                if (bin.empty()) {
                    std::wstring icon = readStr(L"DisplayIcon");
                    if (!icon.empty()) bin = normalizeBinDir(parentDir(icon));
                }
                if (!bin.empty()) { RegCloseKey(hk); RegCloseKey(hRoot); return bin; }
            }
            RegCloseKey(hk);
        }
        RegCloseKey(hRoot);
    }
    return L"";
}

static std::wstring fromDefaults() {
    const wchar_t* envs[] = { L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)" };
    for (const wchar_t* e : envs) {
        wchar_t pf[MAX_PATH];
        if (!GetEnvironmentVariableW(e, pf, MAX_PATH)) continue;
        std::wstring bin = std::wstring(pf) + L"\\Netease\\GameViewer\\bin";
        if (fileExists(joinPath(bin, L"GameViewer.exe"))) return bin;
    }
    return L"";
}

static std::wstring autoDetect() {
    std::wstring p = runningGameViewerPath();
    if (!p.empty()) return parentDir(p);
    p = fromRegistry();   if (!p.empty()) return p;
    p = fromDefaults();   if (!p.empty()) return p;
    return L"";
}

// 返回 -1 / 0 / 1
static int cmpVer(const std::wstring& a, const std::wstring& b) {
    auto parse = [](const std::wstring& s, int out[4]) {
        out[0] = out[1] = out[2] = out[3] = 0;
        swscanf_s(s.c_str(), L"%d.%d.%d.%d", &out[0], &out[1], &out[2], &out[3]);
    };
    int va[4], vb[4];
    parse(a, va); parse(b, vb);
    for (int i = 0; i < 4; ++i) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return  1;
    }
    return 0;
}

static DWORD WINAPI checkUpdateThread(LPVOID param) {
    HINTERNET ses = WinHttpOpen(L"uu-enhance-installer/" UURE_VERSION_W,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);

    HINTERNET con = WinHttpConnect(ses, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) { WinHttpCloseHandle(ses); return 0; }

    HINTERNET req = WinHttpOpenRequest(con, L"GET",
        L"/repos/djkcyl/uu-enhance/releases/latest",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return 0; }

    if (!WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return 0;
    }

    std::string body;
    char buf[4096]; DWORD n = 0;
    while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0)
        { body.append(buf, n); n = 0; }
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);

    auto pos = body.find("\"tag_name\"");
    if (pos == std::string::npos) return 0;
    pos = body.find('"', pos + 10);
    if (pos == std::string::npos) return 0;
    auto end = body.find('"', pos + 1);
    if (end == std::string::npos) return 0;
    std::string tag(body, pos + 1, end - pos - 1);
    if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);

    std::wstring wtag(tag.begin(), tag.end());
    if (cmpVer(wtag, UURE_VERSION_W) > 0) {
        g_latestVer = wtag;
        PostMessageW((HWND)param, WM_UPDATE_AVAILABLE, 0, 0);
    }
    return 0;
}

static std::wstring currentBinDir() {
    int len = GetWindowTextLengthW(g_path);
    std::wstring s(len, 0);
    GetWindowTextW(g_path, &s[0], len + 1);
    while (!s.empty() && (s.back() == 0 || s.back() == L' ')) s.pop_back();
    return s;
}

enum InstallState { ST_NO_DIR, ST_NOT_INSTALLED, ST_OUTDATED, ST_CURRENT, ST_NEWER };
static InstallState g_installState = ST_NO_DIR;

static void refreshState() {
    std::wstring bin = normalizeBinDir(currentBinDir());
    bool dirOk = !bin.empty();
    std::wstring installedVer;

    if (dirOk) {
        std::wstring dll = joinPath(bin, L"version.dll");
        if (fileExists(dll) && isOurDll(dll))
            installedVer = verString(dll, L"FileVersion");
    }

    if (!dirOk) {
        g_installState = ST_NO_DIR;
        SetWindowTextW(g_state, L"  未找到 GameViewer");
        SetWindowTextW(g_pathHint, L"请手动浏览选择安装目录");
        SetWindowTextW(g_btnInstall, L"安装");
    } else if (installedVer.empty()) {
        g_installState = ST_NOT_INSTALLED;
        SetWindowTextW(g_state, L"  未安装");
        SetWindowTextW(g_pathHint, L"");
        SetWindowTextW(g_btnInstall, L"安装");
    } else {
        int cmp = cmpVer(installedVer, UURE_VERSION_W);
        if (cmp < 0) {
            g_installState = ST_OUTDATED;
            std::wstring txt = L"  v" + installedVer + L"  ->  v" UURE_VERSION_W;
            SetWindowTextW(g_state, txt.c_str());
            SetWindowTextW(g_pathHint, L"");
            SetWindowTextW(g_btnInstall, L"更新");
        } else if (cmp == 0) {
            g_installState = ST_CURRENT;
            SetWindowTextW(g_state, L"  已安装  v" UURE_VERSION_W);
            SetWindowTextW(g_pathHint, L"");
            SetWindowTextW(g_btnInstall, L"重新安装");
        } else {
            g_installState = ST_NEWER;
            std::wstring txt = L"  已安装更新版本  v" + installedVer;
            SetWindowTextW(g_state, txt.c_str());
            SetWindowTextW(g_pathHint, L"当前安装器版本 v" UURE_VERSION_W L"，安装将降级");
            SetWindowTextW(g_btnInstall, L"降级");
        }
    }
    InvalidateRect(g_state, nullptr, TRUE);

    EnableWindow(g_btnInstall, dirOk);
    EnableWindow(g_btnUninstall, dirOk && !installedVer.empty());
}

static bool extractPayload(const std::wstring& dest, std::wstring& errOut) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD_DLL), MAKEINTRESOURCEW(10));
    if (!res) { errOut = L"安装器内未找到内嵌 DLL 资源"; return false; }
    DWORD sz = SizeofResource(nullptr, res);
    HGLOBAL g = LoadResource(nullptr, res);
    void* data = g ? LockResource(g) : nullptr;
    if (!data || !sz) { errOut = L"内嵌 DLL 资源为空"; return false; }

    HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        errOut = (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
                     ? L"文件被占用，请先退出 GameViewer（含托盘图标）"
                     : L"无法写入文件（错误码 " + std::to_wstring(e) + L"）";
        return false;
    }
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data, sz, &wrote, nullptr);
    CloseHandle(f);
    if (!ok || wrote != sz) { errOut = L"写入未完成"; return false; }
    return true;
}

static void doInstall(HWND hwnd) {
    std::wstring bin = normalizeBinDir(currentBinDir());
    if (bin.empty()) {
        MessageBoxW(hwnd, L"目录里找不到 GameViewer.exe，请重新选择。", L"目录无效", MB_ICONWARNING);
        return;
    }
    if (g_installState == ST_NEWER) {
        int r = MessageBoxW(hwnd,
            L"已安装的版本比当前安装器更新，确定要降级吗?",
            L"确认降级", MB_YESNO | MB_ICONWARNING);
        if (r != IDYES) return;
    }

    if (isGameViewerRunning()) {
        MessageBoxW(hwnd,
            L"GameViewer 正在运行，DLL 会被占用。\n请先完全退出（含右下角托盘图标）再安装。",
            L"请先退出 GameViewer", MB_ICONWARNING);
        return;
    }

    std::wstring dll = joinPath(bin, L"version.dll");
    if (fileExists(dll) && !isOurDll(dll)) {
        std::wstring bak = dll + L".bak";
        if (!fileExists(bak) && MoveFileW(dll.c_str(), bak.c_str()))
            logln(L"已备份原有 version.dll -> version.dll.bak");
    }

    std::wstring err;
    if (!extractPayload(dll, err)) {
        logln(L"安装失败: " + err);
        MessageBoxW(hwnd, err.c_str(), L"安装失败", MB_ICONERROR);
        refreshState();
        return;
    }

    bool wasUpdate = (g_installState == ST_OUTDATED || g_installState == ST_CURRENT);
    logln(wasUpdate ? L"已更新 version.dll" : L"已释放 version.dll");
    refreshState();
    if (wasUpdate) {
        MessageBoxW(hwnd,
            L"更新完成!\n\n下次启动 GameViewer 即生效。",
            L"更新成功", MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd,
            L"安装完成!\n\n"
            L"打开 GameViewer 后，系统托盘会出现「UU远程增强」图标，\n"
            L"右键它就能按会话开关: 仅浏览 / 剪贴板 / 手柄。",
            L"安装成功", MB_ICONINFORMATION);
    }
}

static void doUninstall(HWND hwnd) {
    std::wstring bin = normalizeBinDir(currentBinDir());
    if (bin.empty()) {
        MessageBoxW(hwnd, L"目录里找不到 GameViewer.exe。", L"目录无效", MB_ICONWARNING);
        return;
    }
    std::wstring dll = joinPath(bin, L"version.dll");
    if (!fileExists(dll) || !isOurDll(dll)) {
        MessageBoxW(hwnd, L"没有找到已安装的补丁。", L"无需卸载", MB_ICONINFORMATION);
        refreshState();
        return;
    }
    if (isGameViewerRunning()) {
        MessageBoxW(hwnd,
            L"GameViewer 正在运行，DLL 被占用。\n请先完全退出再卸载。",
            L"请先退出 GameViewer", MB_ICONWARNING);
        return;
    }

    if (!DeleteFileW(dll.c_str())) {
        logln(L"卸载失败: 无法删除 " + dll);
        MessageBoxW(hwnd, L"删除失败，请确认 GameViewer 已退出。", L"卸载失败", MB_ICONERROR);
        return;
    }
    logln(L"已删除 " + dll);

    std::wstring bak = dll + L".bak";
    if (fileExists(bak) && MoveFileW(bak.c_str(), dll.c_str()))
        logln(L"已还原备份的原 version.dll");

    std::wstring ini = joinPath(bin, L"uu-enhance.ini");
    if (fileExists(ini) && DeleteFileW(ini.c_str()))
        logln(L"已删除配置 uu-enhance.ini");

    refreshState();
    MessageBoxW(hwnd, L"已卸载，GameViewer 已恢复原状。", L"卸载完成", MB_ICONINFORMATION);
}

static void doBrowse(HWND hwnd) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择 GameViewer 安装目录（或 bin 文件夹）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
        std::wstring bin = normalizeBinDir(path);
        if (bin.empty()) {
            MessageBoxW(hwnd, L"选的目录里没有 GameViewer.exe。", L"目录无效", MB_ICONWARNING);
        } else {
            SetWindowTextW(g_path, bin.c_str());
            logln(L"手动选择: " + bin);
            refreshState();
        }
    }
    CoTaskMemFree(pidl);
}

static void applyFont(HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE); }

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {

    case WM_CREATE: {
        int y = BANNER_H + 20;

        CreateWindowExW(0, L"STATIC", L"安装目录",
            WS_CHILD | WS_VISIBLE, MARGIN, y, CW, 18, hwnd, nullptr, nullptr, nullptr);
        y += 22;

        g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            MARGIN, y, CW - 80, 28, hwnd, (HMENU)ID_PATH, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"浏览",
            WS_CHILD | WS_VISIBLE, MARGIN + CW - 72, y, 72, 28,
            hwnd, (HMENU)ID_BROWSE, nullptr, nullptr);
        y += 32;

        g_pathHint = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, MARGIN, y, CW, 16, hwnd, (HMENU)ID_PATH_HINT, nullptr, nullptr);
        y += 26;

        CreateWindowExW(0, L"STATIC", L"当前状态",
            WS_CHILD | WS_VISIBLE, MARGIN, y, 60, 18, hwnd, nullptr, nullptr, nullptr);
        g_state = CreateWindowExW(0, L"STATIC", L"  检测中...",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            MARGIN + 62, y - 2, CW - 62, 22, hwnd, (HMENU)ID_STATE, nullptr, nullptr);
        y += 34;

        g_btnInstall = CreateWindowExW(0, L"BUTTON", L"安装",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            MARGIN, y, 130, 38, hwnd, (HMENU)ID_INSTALL, nullptr, nullptr);
        g_btnUninstall = CreateWindowExW(0, L"BUTTON", L"卸载",
            WS_CHILD | WS_VISIBLE,
            MARGIN + 142, y, 100, 38, hwnd, (HMENU)ID_UNINSTALL, nullptr, nullptr);
        y += 52;

        CreateWindowExW(0, L"STATIC", L"日志",
            WS_CHILD | WS_VISIBLE, MARGIN, y, CW, 16, hwnd, nullptr, nullptr, nullptr);
        y += 18;
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            MARGIN, y, CW, 100, hwnd, (HMENU)ID_LOG, nullptr, nullptr);
        y += 108;

        g_footer = CreateWindowExW(0, L"SysLink",
            L"v" UURE_VERSION_W
            L"  <a href=\"" UURE_GITHUB_W L"\">GitHub</a>",
            WS_CHILD | WS_VISIBLE,
            MARGIN, y, CW, 18, hwnd, (HMENU)ID_LINK, nullptr, nullptr);

        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            applyFont(c);
        SendMessageW(g_state, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

        std::wstring bin = autoDetect();
        if (bin.empty()) {
            logln(L"未能自动找到 GameViewer，请点 [浏览] 手动选择。");
        } else {
            SetWindowTextW(g_path, bin.c_str());
            logln(L"自动定位: " + bin);
            if (isGameViewerRunning())
                logln(L"提示: GameViewer 正在运行，操作前请先退出。");
        }
        refreshState();
        CreateThread(nullptr, 0, checkUpdateThread, (LPVOID)hwnd, 0, nullptr);
        return 0;
    }

    case WM_UPDATE_AVAILABLE: {
        logln(L"安装器有新版本 v" + g_latestVer + L" 可用");
        std::wstring txt = L"v" UURE_VERSION_W
            L"  |  <a href=\"" UURE_GITHUB_W L"/releases/latest\">"
            L"新版本 v" + g_latestVer + L" 可用</a>";
        SetWindowTextW(g_footer, txt.c_str());
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{ 0, 0, WIN_W + 100, BANNER_H };
        FillRect(hdc, &rc, g_brushBanner);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, g_fontTitle);
        RECT rtTitle{ MARGIN, 14, WIN_W, 44 };
        DrawTextW(hdc, L"UU远程增强", -1, &rtTitle, DT_SINGLELINE | DT_NOPREFIX);

        SetTextColor(hdc, CLR_BANNER_SUB);
        SelectObject(hdc, g_font);
        RECT rtSub{ MARGIN, 42, WIN_W, 60 };
        DrawTextW(hdc, L"主控端功能增强补丁  ·  一键安装器", -1, &rtSub,
                  DT_SINGLELINE | DT_NOPREFIX);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)w;
        HWND ctl = (HWND)l;
        SetBkColor(hdc, RGB(255, 255, 255));
        if (ctl == g_state) {
            COLORREF c = CLR_GRAY;
            if (g_installState == ST_CURRENT)  c = CLR_GREEN;
            if (g_installState == ST_OUTDATED) c = CLR_BANNER;
            if (g_installState == ST_NEWER)    c = RGB(202, 138, 4);
            SetTextColor(hdc, c);
        } else if (ctl == g_pathHint) {
            SetTextColor(hdc, CLR_GRAY);
        } else {
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BROWSE:    doBrowse(hwnd);    return 0;
        case ID_INSTALL:   doInstall(hwnd);   return 0;
        case ID_UNINSTALL: doUninstall(hwnd); return 0;
        case ID_PATH:
            if (HIWORD(w) == EN_CHANGE) refreshState();
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto nm = (LPNMHDR)l;
        if (nm->idFrom == ID_LINK && (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
            ShellExecuteW(nullptr, L"open", UURE_GITHUB_W, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    LOGFONTW lfBold = ncm.lfMessageFont;
    lfBold.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lfBold);

    LOGFONTW lfTitle = ncm.lfMessageFont;
    lfTitle.lfHeight = -20;
    lfTitle.lfWeight = FW_BOLD;
    g_fontTitle = CreateFontIndirectW(&lfTitle);

    g_brushBanner = CreateSolidBrush(CLR_BANNER);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"UUEnhanceInstaller";
    wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT wr{ 0, 0, WIN_W, WIN_H };
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    int cw = wr.right - wr.left, ch = wr.bottom - wr.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"UU远程增强",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, cw, ch, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
