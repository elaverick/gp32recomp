/* gp32_ops.h - ARMv4T instruction semantics.
 *
 * Every handler takes the raw instruction word.  The interpreter calls them
 * with words fetched at run time; recompiled code calls the very same
 * functions with constant arguments and relies on the C compiler to fold the
 * decoding away.  There is therefore exactly one definition of what each
 * instruction does.
 *
 * `pcv` is the value an instruction observes when it reads r15
 * (address + 8 in ARM state, address + 4 in Thumb state).  A handler that
 * writes r15 stores the new address in c->r[15]; the caller decides what to
 * do next.  Condition codes are checked by the caller.
 */
#ifndef GP32_OPS_H
#define GP32_OPS_H

#include "gp32rt.h"

/* ---- instruction classes (shared by interpreter and recompiler) -------- */
enum {
    ARM_DP, ARM_MUL, ARM_MULL, ARM_SWP, ARM_BX, ARM_MRS, ARM_MSR, ARM_LDRSTR,
    ARM_LDRSTRH, ARM_LDM, ARM_STM, ARM_B, ARM_BL, ARM_COP, ARM_SWI, ARM_UNDEF
};

GP32_INLINE int arm_classify(uint32_t i) {
    switch ((i >> 25) & 7) {
    case 0:
        if ((i & 0x0FFFFFF0u) == 0x012FFF10u) return ARM_BX;
        if ((i & 0x0FC000F0u) == 0x00000090u) return ARM_MUL;
        if ((i & 0x0F8000F0u) == 0x00800090u) return ARM_MULL;
        if ((i & 0x0FB00FF0u) == 0x01000090u) return ARM_SWP;
        if ((i & 0x00000090u) == 0x00000090u) return (i & 0x60u) ? ARM_LDRSTRH : ARM_UNDEF;
        if ((i & 0x0FBF0FFFu) == 0x010F0000u) return ARM_MRS;
        if ((i & 0x0FB0FFF0u) == 0x0120F000u) return ARM_MSR;
        if ((i & 0x01900000u) == 0x01000000u) return ARM_UNDEF;
        return ARM_DP;
    case 1:
        if ((i & 0x0FB0F000u) == 0x0320F000u) return ARM_MSR;
        if ((i & 0x01900000u) == 0x01000000u) return ARM_UNDEF;
        return ARM_DP;
    case 2: return ARM_LDRSTR;
    case 3: return (i & 0x10u) ? ARM_UNDEF : ARM_LDRSTR;
    case 4: return (i & 0x00100000u) ? ARM_LDM : ARM_STM;
    case 5: return (i & 0x01000000u) ? ARM_BL : ARM_B;
    case 6: return ARM_COP;
    default: return (i & 0x01000000u) ? ARM_SWI : ARM_COP;
    }
}

GP32_INLINE int gp32_cond(const gp32_cpu *c, unsigned cc) {
    switch (cc & 15) {
    case 0: return c->zf != 0;
    case 1: return c->zf == 0;
    case 2: return c->cf != 0;
    case 3: return c->cf == 0;
    case 4: return c->nf != 0;
    case 5: return c->nf == 0;
    case 6: return c->vf != 0;
    case 7: return c->vf == 0;
    case 8: return c->cf && !c->zf;
    case 9: return !c->cf || c->zf;
    case 10: return c->nf == c->vf;
    case 11: return c->nf != c->vf;
    case 12: return !c->zf && c->nf == c->vf;
    case 13: return c->zf || c->nf != c->vf;
    case 14: return 1;
    default: return 0;
    }
}

/* ---- barrel shifter ---------------------------------------------------- */
/* Shift by an immediate taken from the instruction (0 has special meanings). */
GP32_INLINE uint32_t gp32_shift_imm(const gp32_cpu *c, uint32_t v, unsigned type, unsigned amt, uint32_t *carry) {
    switch (type & 3) {
    case 0:
        if (!amt) return v;
        *carry = (v >> (32 - amt)) & 1;
        return v << amt;
    case 1:
        if (!amt) { *carry = v >> 31; return 0; }
        *carry = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (!amt) { *carry = v >> 31; return (v >> 31) ? 0xFFFFFFFFu : 0; }
        *carry = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default:
        if (!amt) { *carry = v & 1; return (c->cf << 31) | (v >> 1); }
        *carry = (v >> (amt - 1)) & 1;
        return (v >> amt) | (v << (32 - amt));
    }
}

/* Shift by the low byte of a register. */
GP32_INLINE uint32_t gp32_shift_reg(uint32_t v, unsigned type, uint32_t amt, uint32_t *carry) {
    amt &= 0xFF;
    if (!amt) return v;
    switch (type & 3) {
    case 0:
        if (amt < 32) { *carry = (v >> (32 - amt)) & 1; return v << amt; }
        *carry = (amt == 32) ? (v & 1) : 0;
        return 0;
    case 1:
        if (amt < 32) { *carry = (v >> (amt - 1)) & 1; return v >> amt; }
        *carry = (amt == 32) ? (v >> 31) : 0;
        return 0;
    case 2:
        if (amt < 32) { *carry = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *carry = v >> 31;
        return (v >> 31) ? 0xFFFFFFFFu : 0;
    default:
        amt &= 31;
        if (!amt) { *carry = v >> 31; return v; }
        *carry = (v >> (amt - 1)) & 1;
        return (v >> amt) | (v << (32 - amt));
    }
}

GP32_INLINE uint32_t gp32_adc(gp32_cpu *c, uint32_t a, uint32_t b, uint32_t cin, int setflags) {
    uint64_t t = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)t;
    if (setflags) {
        c->cf = (uint32_t)(t >> 32);
        c->vf = (~(a ^ b) & (a ^ res)) >> 31;
        c->nf = res >> 31;
        c->zf = res == 0;
    }
    return res;
}

GP32_INLINE void gp32_nz(gp32_cpu *c, uint32_t res) {
    c->nf = res >> 31;
    c->zf = res == 0;
}

/* ---- data processing --------------------------------------------------- */
GP32_INLINE void arm_dp(gp32_cpu *c, uint32_t insn, uint32_t pcv) {
    unsigned op = (insn >> 21) & 15, s = (insn >> 20) & 1;
    unsigned rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
    uint32_t sc = c->cf, a, b, res;
    if (insn & 0x02000000u) {
        unsigned rot = (insn >> 7) & 0x1E;
        uint32_t imm = insn & 0xFF;
        b = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
        if (rot) sc = b >> 31;
        a = rn == 15 ? pcv : c->r[rn];
    } else {
        unsigned rm = insn & 15, type = (insn >> 5) & 3;
        if (insn & 0x10u) { /* shift by register: r15 reads as address + 12 */
            uint32_t v = rm == 15 ? pcv + 4 : c->r[rm];
            b = gp32_shift_reg(v, type, c->r[(insn >> 8) & 15], &sc);
            a = rn == 15 ? pcv + 4 : c->r[rn];
        } else {
            uint32_t v = rm == 15 ? pcv : c->r[rm];
            b = gp32_shift_imm(c, v, type, (insn >> 7) & 31, &sc);
            a = rn == 15 ? pcv : c->r[rn];
        }
    }
    /* "S" with rd = r15 means "also restore CPSR", not "set flags". */
    {
        int fl = s && (rd != 15 || (op >= 8 && op <= 11));
        switch (op) {
        case 0: case 8: res = a & b; break;
        case 1: case 9: res = a ^ b; break;
        case 2: case 10: res = gp32_adc(c, a, ~b, 1, fl); break;
        case 3: res = gp32_adc(c, b, ~a, 1, fl); break;
        case 4: case 11: res = gp32_adc(c, a, b, 0, fl); break;
        case 5: res = gp32_adc(c, a, b, c->cf, fl); break;
        case 6: res = gp32_adc(c, a, ~b, c->cf, fl); break;
        case 7: res = gp32_adc(c, b, ~a, c->cf, fl); break;
        case 12: res = a | b; break;
        case 13: res = b; break;
        case 14: res = a & ~b; break;
        default: res = ~b; break;
        }
        if (fl && (op < 2 || op == 8 || op == 9 || op >= 12)) {
            gp32_nz(c, res);
            c->cf = sc;
        }
    }
    if (op >= 8 && op <= 11) return;
    if (rd == 15) {
        if (s) {
            gp32_restore_cpsr(c);
            c->r[15] = res & ((c->cpsr & GP32_PSR_T) ? ~1u : ~3u);
        } else {
            c->r[15] = res & ~3u;
        }
    } else {
        c->r[rd] = res;
    }
}

GP32_INLINE void arm_mul(gp32_cpu *c, uint32_t insn) {
    unsigned rd = (insn >> 16) & 15, rn = (insn >> 12) & 15, rs = (insn >> 8) & 15, rm = insn & 15;
    uint32_t res = c->r[rm] * c->r[rs];
    if (insn & 0x00200000u) res += c->r[rn];
    c->r[rd] = res;
    if (insn & 0x00100000u) gp32_nz(c, res);
}

GP32_INLINE void arm_mull(gp32_cpu *c, uint32_t insn) {
    unsigned rdhi = (insn >> 16) & 15, rdlo = (insn >> 12) & 15, rs = (insn >> 8) & 15, rm = insn & 15;
    uint64_t res;
    if (insn & 0x00400000u) res = (uint64_t)((int64_t)(int32_t)c->r[rm] * (int64_t)(int32_t)c->r[rs]);
    else res = (uint64_t)c->r[rm] * (uint64_t)c->r[rs];
    if (insn & 0x00200000u) res += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
    c->r[rdlo] = (uint32_t)res;
    c->r[rdhi] = (uint32_t)(res >> 32);
    if (insn & 0x00100000u) {
        c->nf = (uint32_t)(res >> 63);
        c->zf = res == 0;
    }
}

GP32_INLINE void arm_swp(gp32_cpu *c, uint32_t insn) {
    unsigned rn = (insn >> 16) & 15, rd = (insn >> 12) & 15, rm = insn & 15;
    uint32_t addr = c->r[rn], src = c->r[rm], old;
    if (insn & 0x00400000u) {
        old = gp32_rd8(c, addr);
        gp32_wr8(c, addr, src);
    } else {
        old = gp32_rd32(c, addr);
        if (addr & 3) old = (old >> (8 * (addr & 3))) | (old << (32 - 8 * (addr & 3)));
        gp32_wr32(c, addr, src);
    }
    c->r[rd] = old;
}

GP32_INLINE void arm_bx(gp32_cpu *c, uint32_t insn, uint32_t pcv) {
    unsigned rm = insn & 15;
    uint32_t t = rm == 15 ? pcv : c->r[rm];
    if (t & 1) { c->cpsr |= GP32_PSR_T; c->r[15] = t & ~1u; }
    else { c->cpsr &= ~GP32_PSR_T; c->r[15] = t & ~3u; }
}

GP32_INLINE void arm_mrs(gp32_cpu *c, uint32_t insn) {
    c->r[(insn >> 12) & 15] = (insn & 0x00400000u) ? c->spsr : gp32_get_cpsr(c);
}

GP32_INLINE void arm_msr(gp32_cpu *c, uint32_t insn) {
    uint32_t v, mask = 0;
    if (insn & 0x02000000u) {
        unsigned rot = (insn >> 7) & 0x1E;
        uint32_t imm = insn & 0xFF;
        v = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
    } else {
        v = c->r[insn & 15];
    }
    if (insn & 0x00010000u) mask |= 0x000000FFu;
    if (insn & 0x00020000u) mask |= 0x0000FF00u;
    if (insn & 0x00040000u) mask |= 0x00FF0000u;
    if (insn & 0x00080000u) mask |= 0xFF000000u;
    if (insn & 0x00400000u) {
        c->spsr = (c->spsr & ~mask) | (v & mask);
    } else {
        uint32_t cur = gp32_get_cpsr(c);
        if ((cur & GP32_PSR_MODE) == GP32_MODE_USR) mask &= 0xFF000000u;
        mask &= ~GP32_PSR_T; /* MSR cannot change state */
        gp32_set_cpsr(c, (cur & ~mask) | (v & mask));
    }
}

/* ---- single register load/store ---------------------------------------- */
GP32_INLINE void arm_ldrstr(gp32_cpu *c, uint32_t insn, uint32_t pcv) {
    unsigned rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
    uint32_t base = rn == 15 ? pcv : c->r[rn], off, addr, wb;
    if (insn & 0x02000000u) {
        unsigned rm = insn & 15;
        uint32_t sc = 0;
        off = gp32_shift_imm(c, rm == 15 ? pcv : c->r[rm], (insn >> 5) & 3, (insn >> 7) & 31, &sc);
    } else {
        off = insn & 0xFFF;
    }
    wb = (insn & 0x00800000u) ? base + off : base - off;
    addr = (insn & 0x01000000u) ? wb : base;
    if (insn & 0x00100000u) {
        uint32_t v;
        if (insn & 0x00400000u) {
            v = gp32_rd8(c, addr);
        } else {
            v = gp32_rd32(c, addr);
            if (addr & 3) v = (v >> (8 * (addr & 3))) | (v << (32 - 8 * (addr & 3)));
        }
        if (!(insn & 0x01000000u) || (insn & 0x00200000u)) c->r[rn] = wb;
        if (rd == 15) c->r[15] = v & ~3u;
        else c->r[rd] = v;
    } else {
        uint32_t v = rd == 15 ? pcv + 4 : c->r[rd];
        if (insn & 0x00400000u) gp32_wr8(c, addr, v);
        else gp32_wr32(c, addr, v);
        if (!(insn & 0x01000000u) || (insn & 0x00200000u)) c->r[rn] = wb;
    }
}

/* Halfword and signed-byte forms. */
GP32_INLINE void arm_ldrstrh(gp32_cpu *c, uint32_t insn, uint32_t pcv) {
    unsigned rn = (insn >> 16) & 15, rd = (insn >> 12) & 15, sh = (insn >> 5) & 3;
    uint32_t base = rn == 15 ? pcv : c->r[rn], off, addr, wb;
    if (insn & 0x00400000u) off = ((insn >> 4) & 0xF0) | (insn & 0xF);
    else off = c->r[insn & 15];
    wb = (insn & 0x00800000u) ? base + off : base - off;
    addr = (insn & 0x01000000u) ? wb : base;
    if (insn & 0x00100000u) {
        uint32_t v;
        if (sh == 1) v = gp32_rd16(c, addr);
        else if (sh == 2) v = (uint32_t)(int32_t)(int8_t)gp32_rd8(c, addr);
        else v = (uint32_t)(int32_t)(int16_t)gp32_rd16(c, addr);
        if (!(insn & 0x01000000u) || (insn & 0x00200000u)) c->r[rn] = wb;
        if (rd == 15) c->r[15] = v & ~3u;
        else c->r[rd] = v;
    } else {
        if (sh == 1) gp32_wr16(c, addr, rd == 15 ? pcv + 4 : c->r[rd]);
        if (!(insn & 0x01000000u) || (insn & 0x00200000u)) c->r[rn] = wb;
    }
}

/* ---- block transfers --------------------------------------------------- */
GP32_INLINE unsigned gp32_popcount16(uint32_t v) {
    unsigned n = 0;
    v &= 0xFFFF;
    while (v) { n++; v &= v - 1; }
    return n;
}

GP32_INLINE void arm_ldm(gp32_cpu *c, uint32_t insn) {
    unsigned rn = (insn >> 16) & 15, i;
    uint32_t list = insn & 0xFFFF, n = gp32_popcount16(list), base = c->r[rn], addr;
    int user = (insn & 0x00400000u) && !(list & 0x8000u);
    if (insn & 0x00800000u) addr = (insn & 0x01000000u) ? base + 4 : base;
    else addr = (insn & 0x01000000u) ? base - 4 * n : base - 4 * n + 4;
    if (insn & 0x00200000u) c->r[rn] = (insn & 0x00800000u) ? base + 4 * n : base - 4 * n;
    for (i = 0; i < 15; i++) {
        if (list & (1u << i)) {
            uint32_t v = gp32_rd32(c, addr);
            if (user) gp32_set_user_reg(c, i, v);
            else c->r[i] = v;
            addr += 4;
        }
    }
    if (list & 0x8000u) {
        uint32_t v = gp32_rd32(c, addr);
        if (insn & 0x00400000u) {
            gp32_restore_cpsr(c);
            c->r[15] = v & ((c->cpsr & GP32_PSR_T) ? ~1u : ~3u);
        } else {
            c->r[15] = v & ~3u;
        }
    }
}

GP32_INLINE void arm_stm(gp32_cpu *c, uint32_t insn, uint32_t pcv) {
    unsigned rn = (insn >> 16) & 15, i, first = 1;
    uint32_t list = insn & 0xFFFF, n = gp32_popcount16(list), base = c->r[rn], addr;
    int user = (insn & 0x00400000u) != 0;
    uint32_t wb = (insn & 0x00800000u) ? base + 4 * n : base - 4 * n;
    if (insn & 0x00800000u) addr = (insn & 0x01000000u) ? base + 4 : base;
    else addr = (insn & 0x01000000u) ? base - 4 * n : base - 4 * n + 4;
    for (i = 0; i < 16; i++) {
        if (list & (1u << i)) {
            uint32_t v = i == 15 ? pcv + 4 : (user ? gp32_get_user_reg(c, i) : c->r[i]);
            gp32_wr32(c, addr, v);
            addr += 4;
            /* the base is updated after the first transfer */
            if (first && (insn & 0x00200000u)) c->r[rn] = wb;
            first = 0;
        }
    }
}

/* ---- Thumb ------------------------------------------------------------- */
enum { THUMB_OP, THUMB_BCOND, THUMB_B, THUMB_BL_HI, THUMB_BL_LO, THUMB_SWI, THUMB_UNDEF };

GP32_INLINE int thumb_classify(uint32_t i) {
    switch (i >> 12) {
    case 0xD:
        if ((i & 0x0F00u) == 0x0F00u) return THUMB_SWI;
        if ((i & 0x0F00u) == 0x0E00u) return THUMB_UNDEF;
        return THUMB_BCOND;
    case 0xE: return (i & 0x0800u) ? THUMB_UNDEF : THUMB_B;
    case 0xF: return (i & 0x0800u) ? THUMB_BL_LO : THUMB_BL_HI;
    case 0xB:
        if ((i & 0x0F00u) == 0x0000u) return THUMB_OP;
        if ((i & 0x0600u) == 0x0400u) return THUMB_OP;
        return THUMB_UNDEF;
    default: return THUMB_OP;
    }
}

/* Executes every Thumb instruction except branches and SWI.
 * Writes c->r[15] for BX, MOV/ADD pc and POP {pc}. */
GP32_INLINE void thumb_op(gp32_cpu *c, uint32_t insn, uint32_t pcv) {
    unsigned rd = insn & 7, rs = (insn >> 3) & 7;
    switch (insn >> 11) {
    case 0: case 1: case 2: { /* LSL/LSR/ASR by immediate */
        uint32_t sc = c->cf;
        uint32_t res = gp32_shift_imm(c, c->r[rs], insn >> 11, (insn >> 6) & 31, &sc);
        c->r[rd] = res;
        gp32_nz(c, res);
        c->cf = sc;
        return;
    }
    case 3: { /* ADD/SUB register or 3-bit immediate */
        uint32_t b = (insn & 0x0400u) ? ((insn >> 6) & 7) : c->r[(insn >> 6) & 7];
        if (insn & 0x0200u) c->r[rd] = gp32_adc(c, c->r[rs], ~b, 1, 1);
        else c->r[rd] = gp32_adc(c, c->r[rs], b, 0, 1);
        return;
    }
    case 4: case 5: case 6: case 7: { /* MOV/CMP/ADD/SUB 8-bit immediate */
        unsigned r = (insn >> 8) & 7;
        uint32_t imm = insn & 0xFF;
        switch ((insn >> 11) & 3) {
        case 0: c->r[r] = imm; gp32_nz(c, imm); break;
        case 1: (void)gp32_adc(c, c->r[r], ~imm, 1, 1); break;
        case 2: c->r[r] = gp32_adc(c, c->r[r], imm, 0, 1); break;
        default: c->r[r] = gp32_adc(c, c->r[r], ~imm, 1, 1); break;
        }
        return;
    }
    case 8:
        if (!(insn & 0x0400u)) { /* ALU operations */
            uint32_t a = c->r[rd], b = c->r[rs], res, sc = c->cf;
            switch ((insn >> 6) & 15) {
            case 0: res = a & b; break;
            case 1: res = a ^ b; break;
            case 2: res = gp32_shift_reg(a, 0, b, &sc); c->cf = sc; break;
            case 3: res = gp32_shift_reg(a, 1, b, &sc); c->cf = sc; break;
            case 4: res = gp32_shift_reg(a, 2, b, &sc); c->cf = sc; break;
            case 5: c->r[rd] = gp32_adc(c, a, b, c->cf, 1); return;
            case 6: c->r[rd] = gp32_adc(c, a, ~b, c->cf, 1); return;
            case 7: res = gp32_shift_reg(a, 3, b, &sc); c->cf = sc; break;
            case 8: gp32_nz(c, a & b); return;
            case 9: c->r[rd] = gp32_adc(c, 0, ~b, 1, 1); return;
            case 10: (void)gp32_adc(c, a, ~b, 1, 1); return;
            case 11: (void)gp32_adc(c, a, b, 0, 1); return;
            case 12: res = a | b; break;
            case 13: res = a * b; break;
            case 14: res = a & ~b; break;
            default: res = ~b; break;
            }
            c->r[rd] = res;
            gp32_nz(c, res);
        } else { /* high register operations and BX */
            unsigned hd = rd | ((insn >> 4) & 8), hs = (insn >> 3) & 15;
            uint32_t b = hs == 15 ? pcv : c->r[hs];
            switch ((insn >> 8) & 3) {
            case 0:
                if (hd == 15) c->r[15] = (pcv + b) & ~1u;
                else c->r[hd] += b;
                break;
            case 1: (void)gp32_adc(c, hd == 15 ? pcv : c->r[hd], ~b, 1, 1); break;
            case 2:
                if (hd == 15) c->r[15] = b & ~1u;
                else c->r[hd] = b;
                break;
            default:
                if (b & 1) { c->r[15] = b & ~1u; }
                else { c->cpsr &= ~GP32_PSR_T; c->r[15] = b & ~3u; }
                break;
            }
        }
        return;
    case 9: /* LDR rd, [pc, #imm] */
        c->r[(insn >> 8) & 7] = gp32_rd32(c, (pcv & ~2u) + ((insn & 0xFF) << 2));
        return;
    case 10: case 11: { /* register-offset load/store */
        uint32_t addr = c->r[rs] + c->r[(insn >> 6) & 7];
        switch ((insn >> 9) & 7) {
        case 0: gp32_wr32(c, addr, c->r[rd]); break;
        case 1: gp32_wr16(c, addr, c->r[rd]); break;
        case 2: gp32_wr8(c, addr, c->r[rd]); break;
        case 3: c->r[rd] = (uint32_t)(int32_t)(int8_t)gp32_rd8(c, addr); break;
        case 4: {
            uint32_t v = gp32_rd32(c, addr);
            if (addr & 3) v = (v >> (8 * (addr & 3))) | (v << (32 - 8 * (addr & 3)));
            c->r[rd] = v;
            break;
        }
        case 5: c->r[rd] = gp32_rd16(c, addr); break;
        case 6: c->r[rd] = gp32_rd8(c, addr); break;
        default: c->r[rd] = (uint32_t)(int32_t)(int16_t)gp32_rd16(c, addr); break;
        }
        return;
    }
    case 12: gp32_wr32(c, c->r[rs] + (((insn >> 6) & 31) << 2), c->r[rd]); return;
    case 13: {
        uint32_t addr = c->r[rs] + (((insn >> 6) & 31) << 2), v = gp32_rd32(c, addr);
        if (addr & 3) v = (v >> (8 * (addr & 3))) | (v << (32 - 8 * (addr & 3)));
        c->r[rd] = v;
        return;
    }
    case 14: gp32_wr8(c, c->r[rs] + ((insn >> 6) & 31), c->r[rd]); return;
    case 15: c->r[rd] = gp32_rd8(c, c->r[rs] + ((insn >> 6) & 31)); return;
    case 16: gp32_wr16(c, c->r[rs] + (((insn >> 6) & 31) << 1), c->r[rd]); return;
    case 17: c->r[rd] = gp32_rd16(c, c->r[rs] + (((insn >> 6) & 31) << 1)); return;
    case 18: gp32_wr32(c, c->r[13] + ((insn & 0xFF) << 2), c->r[(insn >> 8) & 7]); return;
    case 19: c->r[(insn >> 8) & 7] = gp32_rd32(c, c->r[13] + ((insn & 0xFF) << 2)); return;
    case 20: c->r[(insn >> 8) & 7] = (pcv & ~2u) + ((insn & 0xFF) << 2); return;
    case 21: c->r[(insn >> 8) & 7] = c->r[13] + ((insn & 0xFF) << 2); return;
    case 22: case 23:
        if ((insn & 0x0F00u) == 0) { /* ADD sp, #imm */
            uint32_t imm = (insn & 0x7F) << 2;
            c->r[13] = (insn & 0x80u) ? c->r[13] - imm : c->r[13] + imm;
        } else if (insn & 0x0800u) { /* POP */
            uint32_t sp = c->r[13];
            unsigned i;
            for (i = 0; i < 8; i++)
                if (insn & (1u << i)) { c->r[i] = gp32_rd32(c, sp); sp += 4; }
            if (insn & 0x0100u) { c->r[15] = gp32_rd32(c, sp) & ~1u; sp += 4; }
            c->r[13] = sp;
        } else { /* PUSH */
            uint32_t sp = c->r[13] - 4 * (gp32_popcount16(insn & 0xFF) + ((insn >> 8) & 1)), a = sp;
            unsigned i;
            for (i = 0; i < 8; i++)
                if (insn & (1u << i)) { gp32_wr32(c, a, c->r[i]); a += 4; }
            if (insn & 0x0100u) gp32_wr32(c, a, c->r[14]);
            c->r[13] = sp;
        }
        return;
    case 24: { /* STMIA rb!, {list} */
        unsigned rb = (insn >> 8) & 7, i;
        uint32_t a = c->r[rb];
        for (i = 0; i < 8; i++)
            if (insn & (1u << i)) { gp32_wr32(c, a, c->r[i]); a += 4; }
        c->r[rb] = a;
        return;
    }
    case 25: { /* LDMIA rb!, {list} */
        unsigned rb = (insn >> 8) & 7, i;
        uint32_t a = c->r[rb];
        for (i = 0; i < 8; i++)
            if (insn & (1u << i)) { c->r[i] = gp32_rd32(c, a); a += 4; }
        if (!(insn & (1u << rb))) c->r[rb] = a;
        return;
    }
    default: return;
    }
}

#endif
