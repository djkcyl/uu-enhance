#include "xdisasm.h"
#include "hde64.h"

namespace xd {

Insn decode(const void* code) {
    Insn r{};
    hde64s hs;
    unsigned len = hde64_disasm(code, &hs);
    if (len == 0 || (hs.flags & F_ERROR)) { r.len = 0; return r; }
    r.len      = len;
    r.opcode   = hs.opcode;
    r.opcode2  = hs.opcode2;
    r.two_byte = (hs.opcode == 0x0F);
    r.rex_w = hs.rex_w != 0; r.rex_r = hs.rex_r != 0;
    r.rex_x = hs.rex_x != 0; r.rex_b = hs.rex_b != 0;
    r.has_modrm = (hs.flags & F_MODRM) != 0;
    if (r.has_modrm) {
        r.mod = hs.modrm_mod;
        r.reg = hs.modrm_reg;
        r.rm  = hs.modrm_rm;
        r.rip_rel = (r.mod == 0 && r.rm == 5);
    }
    r.has_sib = (hs.flags & F_SIB) != 0;
    if (r.has_sib) {
        r.sib_base  = hs.sib_base;
        r.sib_index = hs.sib_index;
        r.sib_scale = hs.sib_scale;
    }
    if      (hs.flags & F_DISP32) { r.has_disp = true; r.disp = (int32_t)hs.disp.disp32; }
    else if (hs.flags & F_DISP16) { r.has_disp = true; r.disp = (int16_t)hs.disp.disp16; }
    else if (hs.flags & F_DISP8)  { r.has_disp = true; r.disp = (int8_t)hs.disp.disp8; }
    if (hs.flags & F_RELATIVE) {
        r.has_rel = true;
        if      (hs.flags & F_IMM32) r.rel = (int32_t)hs.imm.imm32;
        else if (hs.flags & F_IMM8)  r.rel = (int8_t)hs.imm.imm8;
    }
    return r;
}

}
