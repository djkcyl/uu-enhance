// version.dll 代理：把标准导出转发到系统真正的 version.dll。
// GameViewer 和 Qt 都会用到 version.dll，导入会绑到本 dll，所以这些导出都得有。
#include <windows.h>

static HMODULE g_real = nullptr;

// 导出名 = 真名（x64 没有 stdcall 修饰）
#define EXPORT(name) __pragma(comment(linker, "/EXPORT:" #name "=my_" #name))

EXPORT(GetFileVersionInfoA)        EXPORT(GetFileVersionInfoW)
EXPORT(GetFileVersionInfoExA)      EXPORT(GetFileVersionInfoExW)
EXPORT(GetFileVersionInfoSizeA)    EXPORT(GetFileVersionInfoSizeW)
EXPORT(GetFileVersionInfoSizeExA)  EXPORT(GetFileVersionInfoSizeExW)
EXPORT(GetFileVersionInfoByHandle)
EXPORT(VerQueryValueA)             EXPORT(VerQueryValueW)
EXPORT(VerLanguageNameA)           EXPORT(VerLanguageNameW)
EXPORT(VerFindFileA)               EXPORT(VerFindFileW)
EXPORT(VerInstallFileA)            EXPORT(VerInstallFileW)

static FARPROC R(const char* n) { return g_real ? GetProcAddress(g_real, n) : nullptr; }

void proxy_init() {
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat_s(sys, L"\\version.dll");
    g_real = LoadLibraryW(sys);
}

// 真函数取不到（g_real 没加载，或这个导出在当前系统不存在）时返回失败值，别去 call 空指针。
#define FWD(ret, fail, name, ptypes, params, args) \
    extern "C" ret WINAPI my_##name params { \
        auto __fp = (ret(WINAPI*)ptypes)R(#name); \
        return __fp ? __fp args : fail; \
    }

FWD(BOOL,  FALSE,   GetFileVersionInfoA,       (LPCSTR,DWORD,DWORD,LPVOID),
    (LPCSTR a,DWORD b,DWORD c,LPVOID d), (a,b,c,d))
FWD(BOOL,  FALSE,   GetFileVersionInfoW,       (LPCWSTR,DWORD,DWORD,LPVOID),
    (LPCWSTR a,DWORD b,DWORD c,LPVOID d), (a,b,c,d))
FWD(BOOL,  FALSE,   GetFileVersionInfoExA,     (DWORD,LPCSTR,DWORD,DWORD,LPVOID),
    (DWORD f,LPCSTR a,DWORD b,DWORD c,LPVOID d), (f,a,b,c,d))
FWD(BOOL,  FALSE,   GetFileVersionInfoExW,     (DWORD,LPCWSTR,DWORD,DWORD,LPVOID),
    (DWORD f,LPCWSTR a,DWORD b,DWORD c,LPVOID d), (f,a,b,c,d))
FWD(DWORD, 0,       GetFileVersionInfoSizeA,   (LPCSTR,LPDWORD),
    (LPCSTR a,LPDWORD b), (a,b))
FWD(DWORD, 0,       GetFileVersionInfoSizeW,   (LPCWSTR,LPDWORD),
    (LPCWSTR a,LPDWORD b), (a,b))
FWD(DWORD, 0,       GetFileVersionInfoSizeExA, (DWORD,LPCSTR,LPDWORD),
    (DWORD f,LPCSTR a,LPDWORD b), (f,a,b))
FWD(DWORD, 0,       GetFileVersionInfoSizeExW, (DWORD,LPCWSTR,LPDWORD),
    (DWORD f,LPCWSTR a,LPDWORD b), (f,a,b))
FWD(void*, nullptr, GetFileVersionInfoByHandle,(void*,void*,void*,void*),
    (void* a,void* b,void* c,void* d), (a,b,c,d))
FWD(BOOL,  FALSE,   VerQueryValueA,            (LPCVOID,LPCSTR,LPVOID*,PUINT),
    (LPCVOID a,LPCSTR b,LPVOID* c,PUINT d), (a,b,c,d))
FWD(BOOL,  FALSE,   VerQueryValueW,            (LPCVOID,LPCWSTR,LPVOID*,PUINT),
    (LPCVOID a,LPCWSTR b,LPVOID* c,PUINT d), (a,b,c,d))
FWD(DWORD, 0,       VerLanguageNameA,          (DWORD,LPSTR,DWORD),
    (DWORD a,LPSTR b,DWORD c), (a,b,c))
FWD(DWORD, 0,       VerLanguageNameW,          (DWORD,LPWSTR,DWORD),
    (DWORD a,LPWSTR b,DWORD c), (a,b,c))
FWD(DWORD, 0,       VerFindFileA,              (DWORD,LPCSTR,LPCSTR,LPCSTR,LPSTR,PUINT,LPSTR,PUINT),
    (DWORD a,LPCSTR b,LPCSTR c,LPCSTR d,LPSTR e,PUINT f,LPSTR g,PUINT h), (a,b,c,d,e,f,g,h))
FWD(DWORD, 0,       VerFindFileW,              (DWORD,LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,PUINT,LPWSTR,PUINT),
    (DWORD a,LPCWSTR b,LPCWSTR c,LPCWSTR d,LPWSTR e,PUINT f,LPWSTR g,PUINT h), (a,b,c,d,e,f,g,h))
FWD(DWORD, 0,       VerInstallFileA,           (DWORD,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPSTR,PUINT),
    (DWORD a,LPCSTR b,LPCSTR c,LPCSTR d,LPCSTR e,LPCSTR f,LPSTR g,PUINT h), (a,b,c,d,e,f,g,h))
FWD(DWORD, 0,       VerInstallFileW,           (DWORD,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,PUINT),
    (DWORD a,LPCWSTR b,LPCWSTR c,LPCWSTR d,LPCWSTR e,LPCWSTR f,LPWSTR g,PUINT h), (a,b,c,d,e,f,g,h))
