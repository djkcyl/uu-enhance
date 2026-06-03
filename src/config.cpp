#include "config.h"
#include <windows.h>
#include <string>

namespace cfg {
    std::atomic<bool> g_viewOnly{true};
    std::atomic<bool> g_clipSync{false};
    std::atomic<bool> g_gamepadOff{false};

    static std::wstring iniPath() {
        wchar_t buf[MAX_PATH]{};
        // ini 放在本 DLL 同目录 (GameViewer\bin\uu-enhance.ini)
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&iniPath, &self);
        GetModuleFileNameW(self, buf, MAX_PATH);
        std::wstring p(buf);
        size_t s = p.find_last_of(L"\\/");
        if (s != std::wstring::npos) p = p.substr(0, s + 1);
        p += L"uu-enhance.ini";
        return p;
    }

    void load() {
        auto p = iniPath();
        g_viewOnly = GetPrivateProfileIntW(L"general", L"view_only", 1, p.c_str()) != 0;
        g_clipSync = GetPrivateProfileIntW(L"general", L"clipboard_sync", 0, p.c_str()) != 0;
        g_gamepadOff = GetPrivateProfileIntW(L"general", L"gamepad_off", 0, p.c_str()) != 0;
    }

    void save() {
        auto p = iniPath();
        WritePrivateProfileStringW(L"general", L"view_only",      g_viewOnly ? L"1" : L"0", p.c_str());
        WritePrivateProfileStringW(L"general", L"clipboard_sync", g_clipSync ? L"1" : L"0", p.c_str());
        WritePrivateProfileStringW(L"general", L"gamepad_off",    g_gamepadOff ? L"1" : L"0", p.c_str());
    }

    std::wstring exe_version() {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return L"";
        // 动态调用系统 version.dll，避免与我们自己的代理 dll 自引用，也不必链接 version.lib
        wchar_t sys[MAX_PATH]{}; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\version.dll");
        HMODULE v = LoadLibraryW(sys);
        if (!v) return L"";
        auto pSize = (DWORD(WINAPI*)(LPCWSTR, LPDWORD))GetProcAddress(v, "GetFileVersionInfoSizeW");
        auto pGet  = (BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID))GetProcAddress(v, "GetFileVersionInfoW");
        auto pQry  = (BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT))GetProcAddress(v, "VerQueryValueW");
        std::wstring out;
        if (pSize && pGet && pQry) {
            DWORD h = 0, sz = pSize(path, &h);
            if (sz) {
                std::wstring buf(sz, 0);
                VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
                if (pGet(path, 0, sz, &buf[0]) && pQry(&buf[0], L"\\", (LPVOID*)&fi, &len) && fi) {
                    wchar_t s[64];
                    swprintf_s(s, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                               HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
                    out = s;
                }
            }
        }
        FreeLibrary(v);
        return out;
    }
}
