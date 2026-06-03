#pragma once
#include <cstdint>
#include <cwchar>

// 按版本的地址表。每个目标含写死 RVA + 一段函数开头的字节特征(AOB，rip 相对位移用 ?? 通配)。
// 定位优先级见 resolver / hooks.cpp：多锚点字符串 → AOB → 这里的 RVA → 跳过。
// 适配新版本：在 kVer 里加一行即可(RVA 用 IDA 重新核对，AOB 重新抓函数开头字节)。
namespace ver {

struct Target { uintptr_t rva; const char* aob; };

struct VerSet {
    const wchar_t* version;
    uintptr_t deviceIdOff;          // CCS 内 device_id std::string 偏移
    Target sendMouse, sendWheel, sendKey, enableCapture, updateCursor;
    Target gpConnect, gpDisconnect, gpUpdate;
    Target clipUpdate, clipHandle;
    Target setConnInfo, closeConn, exitRoom;
};

inline const VerSet kVer[] = {
    { L"4.26.0.8259", 3984,
      /*sendMouse   */ { 0x862080, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 80 FB FF FF 48 81 EC 80 05 00 00 48" },
      /*sendWheel   */ { 0x862bc0, "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ??" },
      /*sendKey     */ { 0x860650, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*enableCap   */ { 0x6639e0, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*updateCursor*/ { 0x66dba0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 44 0F B6 F2" },
      /*gpConnect   */ { 0x9ea770, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpDisconnect*/ { 0x9eac20, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpUpdate    */ { 0x9eeeb0, "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 48 8B C2 48 8B D9 FF 05 ?? ?? ?? ?? 48 8D 54 24 28 48 8B" },
      /*clipUpdate  */ { 0x8c5280, "48 89 5C 24 10 55 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 48 8B" },
      /*clipHandle  */ { 0x8bf810, "40 55 53 56 57 41 56 48 8D AC 24 40 FF FF FF 48 81 EC C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B0 00 00 00 8B" },
      /*setConnInfo */ { 0x867ce0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 4C 8B F2 48 8B F1 E8" },
      /*closeConn   */ { 0x83a3f0, "48 89 5C 24 08 57 48 81 EC 70 01 00 00 48 8B F9 83 A1 00 11 00 00 FE E8 ?? ?? ?? ?? 48 8B D8 48 8D 05 ?? ?? ?? ?? 48 89" },
      /*exitRoom    */ { 0x841b90, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B D9 E8 ?? ?? ??" },
    },
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];   // 未知版本用基线兜底(RVA 多半不对，但字符串/AOB 会先生效)
}

} // namespace ver
