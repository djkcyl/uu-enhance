#pragma once
#include <cstdint>
#include <cwchar>

namespace ver {

struct VerSet {
    const wchar_t* version;
    uintptr_t deviceIdOff;
    uintptr_t vmwDevIdOff, vmwTitleOff;
};

inline const VerSet kVer[] = {
    { L"4.29.0.8620", 4296, 344, 352 },
    { L"4.26.0.8259", 3984, 344, 352 },   // vmw offsets unverified
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
