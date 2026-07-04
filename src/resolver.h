#pragma once
#include <windows.h>
#include <cstdint>
#include <initializer_list>

namespace resolver {
    struct ModRange {
        uintptr_t text_beg, text_end;
        uintptr_t rdata_beg, rdata_end;
        uintptr_t pdata_beg, pdata_end;
        uintptr_t img_beg, img_end;
    };

    bool get_ranges(HMODULE mod, ModRange& out);

    uintptr_t find_func(const ModRange& r, std::initializer_list<const char*> anchors);
    uintptr_t find_func(const ModRange& r, const char* const* anchors, int n);
    uintptr_t find_func_by_logstr(const ModRange& r, const char* logstr);
    uintptr_t find_string(const ModRange& r, const char* s);
    uintptr_t find_func_by_wstr(const ModRange& r, const wchar_t* ws);
}
