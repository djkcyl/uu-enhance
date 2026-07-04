// 封装 MinHook 自带的 HDE64：给定位扫描提供指令级视图，取代按原始字节硬匹配。
#pragma once
#include <cstdint>

namespace xd {

struct Insn {
    unsigned len;        // 0 = 解码失败, 调用方跳 1 字节重同步
    uint8_t  opcode;     // 两字节指令时 =0x0F, 真 opcode 在 opcode2
    uint8_t  opcode2;
    bool     two_byte;
    bool     has_modrm;
    uint8_t  mod, reg, rm;
    bool     rex_w, rex_r, rex_x, rex_b;
    bool     has_sib;
    uint8_t  sib_base, sib_index, sib_scale;
    bool     rip_rel;
    bool     has_disp;
    int32_t  disp;
    bool     has_rel;
    int32_t  rel;
};

Insn decode(const void* code);

inline uintptr_t rip_target(const uint8_t* p, const Insn& i) { return (uintptr_t)(p + i.len) + i.disp; }
inline uintptr_t rel_target(const uint8_t* p, const Insn& i) { return (uintptr_t)(p + i.len) + i.rel; }

}
