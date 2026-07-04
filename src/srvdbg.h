#pragma once
#include <windows.h>

namespace srvdbg {
    constexpr wchar_t MAP_NAME[] = L"Global\\uu-enhance-srv-dbg";
    constexpr wchar_t SDDL[]     = L"D:(A;;GRGW;;;WD)";

    constexpr int MAX_HOOKS = 64;
    constexpr int NAME_LEN  = 48;

    struct Entry { char name[NAME_LEN]; char how[8]; unsigned char ok; unsigned long long off; };
    struct Shared {
        volatile LONG count;
        Entry hooks[MAX_HOOKS];
    };
}
