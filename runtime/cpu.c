/* cpu.c - processor state, the dispatcher and the fallback interpreter. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "internal.h"
#include "gp32_ops.h"

static gp32_fn *g_table[2]; /* [thumb][(addr - load) >> 1] */
static uint32_t g_load, g_span;
static jmp_buf g_exit;
static int g_exit_code;
static uint64_t g_interp_insns;
static int g_warned_interp;

/* ---- status register and banking --------------------------------------- */
static int bank_of(uint32_t mode) {
    switch (mode & GP32_PSR_MODE) {
    case GP32_MODE_FIQ: return 1;
    case GP32_MODE_IRQ: return 2;
    case GP32_MODE_SVC: return 3;
    case GP32_MODE_ABT: return 4;
    case GP32_MODE_UND: return 5;
    default: return 0;
    }
}

static void switch_mode(gp32_cpu *c, uint32_t mode) {
    int from = bank_of(c->cpsr), to = bank_of(mode);
    if (from != to) {
        c->bank_sp[from] = c->r[13];
        c->bank_lr[from] = c->r[14];
        c->bank_spsr[from] = c->spsr;
        if (from == 1) { memcpy(c->bank_fiq, &c->r[8], 20); memcpy(&c->r[8], c->bank_usr, 20); }
        if (to == 1) { memcpy(c->bank_usr, &c->r[8], 20); memcpy(&c->r[8], c->bank_fiq, 20); }
        c->r[13] = c->bank_sp[to];
        c->r[14] = c->bank_lr[to];
        c->spsr = c->bank_spsr[to];
    }
    c->cpsr = (c->cpsr & ~GP32_PSR_MODE) | (mode & GP32_PSR_MODE);
}

uint32_t gp32_get_cpsr(const gp32_cpu *c) {
    return (c->nf << 31) | (c->zf << 30) | (c->cf << 29) | (c->vf << 28) | (c->cpsr & 0xFFu);
}

void gp32_set_cpsr(gp32_cpu *c, uint32_t v) {
    uint32_t was_masked = c->cpsr & GP32_PSR_I;
    c->nf = v >> 31;
    c->zf = (v >> 30) & 1;
    c->cf = (v >> 29) & 1;
    c->vf = (v >> 28) & 1;
    switch_mode(c, v);
    c->cpsr = (v & 0xE0u) | (c->cpsr & GP32_PSR_MODE);
    if (was_masked && !(v & GP32_PSR_I)) gp32_force_event(c); /* let pending interrupts in */
}

void gp32_restore_cpsr(gp32_cpu *c) { gp32_set_cpsr(c, c->spsr); }

uint32_t gp32_get_user_reg(gp32_cpu *c, unsigned n) {
    int b = bank_of(c->cpsr);
    if (n == 13 && b) return c->bank_sp[0];
    if (n == 14 && b) return c->bank_lr[0];
    if (n >= 8 && n <= 12 && b == 1) return c->bank_usr[n - 8];
    return c->r[n];
}

void gp32_set_user_reg(gp32_cpu *c, unsigned n, uint32_t v) {
    int b = bank_of(c->cpsr);
    if (n == 13 && b) c->bank_sp[0] = v;
    else if (n == 14 && b) c->bank_lr[0] = v;
    else if (n >= 8 && n <= 12 && b == 1) c->bank_usr[n - 8] = v;
    else c->r[n] = v;
}

/* IRQ exception entry.  r[15] must hold the address of the next instruction. */
void gp32_enter_irq(gp32_cpu *c, uint32_t handler) {
    uint32_t old = gp32_get_cpsr(c), ret = c->r[15];
    switch_mode(c, GP32_MODE_IRQ);
    c->spsr = old;
    c->r[14] = ret + 4;
    c->cpsr = (c->cpsr & ~GP32_PSR_T) | GP32_PSR_I;
    c->r[15] = handler & ~3u;
}

void gp32_coproc(gp32_cpu *c, uint32_t insn) {
    /* Only the system control coprocessor exists; caches and the MMU need no modelling. */
    unsigned crn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
    if ((insn & 0x0F000010u) != 0x0E000010u || ((insn >> 8) & 15) != 15) return;
    if (insn & 0x00100000u) { /* MRC */
        uint32_t v = 0;
        if (crn == 0) v = 0x41129200u; /* ARM920T */
        else if (crn == 1) v = c->cp15_ctrl;
        else if (crn == 2) v = c->cp15_ttb;
        else if (crn == 3) v = c->cp15_dacr;
        if (rd != 15) c->r[rd] = v;
    } else {
        if (crn == 1) c->cp15_ctrl = c->r[rd];
        else if (crn == 2) c->cp15_ttb = c->r[rd];
        else if (crn == 3) c->cp15_dacr = c->r[rd];
    }
}

/* ---- errors ------------------------------------------------------------ */
void gp32_quit(gp32_cpu *c, int code) {
    (void)c;
    g_exit_code = code;
    longjmp(g_exit, 1);
}

void gp32_fatal(gp32_cpu *c, const char *fmt, ...) {
    va_list ap;
    int i;
    fprintf(stderr, "\ngp32: fatal: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    for (i = 0; i < 16; i++) fprintf(stderr, "r%-2d=%08X%s", i, c->r[i], (i & 3) == 3 ? "\n" : "  ");
    fprintf(stderr, "cpsr=%08X spsr=%08X\n", gp32_get_cpsr(c), c->spsr);
    gp32_quit(c, 3);
}

void gp32_undefined(gp32_cpu *c, uint32_t insn, uint32_t addr) {
    gp32_fatal(c, "undefined instruction %08X at %08X", insn, addr);
}

/* ---- recompiled code lookup -------------------------------------------- */
gp32_fn gp32_lookup(gp32_cpu *c) {
    uint32_t off = c->r[15] - g_load;
    if (off >= g_span || !c->native_ok) return NULL;
    return g_table[(c->cpsr >> 5) & 1][off >> 1];
}

void gp32_tail(gp32_cpu *c) {
    gp32_fn f;
    if (c->depth >= GP32_MAX_DEPTH) return;
    f = gp32_lookup(c);
    if (!f) return;
    c->depth++;
    f(c);
    c->depth--;
}

/* Recompiled functions describe the image as it was loaded.  Once the
 * guest replaces that code (a self-extracting program, a launcher) the only
 * correct thing left is to interpret what is really in memory. */
void gp32_code_overwritten(gp32_cpu *c) {
    const gp32_game *g = gp32_the_game;
    if (!c->native_ok) return;
    /* compare against the pristine image: data and bss change all the time,
     * so only a difference inside recompiled code counts */
    {
        uint32_t i;
        const uint8_t *p = c->ram + (g->load_addr - GP32_RAM_BASE);
        int changed = 0;
        for (i = 0; i < g->num_entries && !changed; i++) {
            uint32_t off = (g->entries[i].addr & ~1u) - g->load_addr;
            if (off + 4 <= g->image_size && memcmp(p + off, g->image + off, (g->entries[i].addr & 1) ? 2 : 4) != 0) changed = 1;
        }
        if (!changed) return;
    }
    c->native_ok = 0;
    fprintf(stderr, "gp32: the program replaced its own code in memory; continuing in the interpreter.\n"
                    "      (self-extracting images should be unpacked first, see gp32recomp --unpack)\n");
}

/* ---- interpreter ------------------------------------------------------- */
static void interp_step(gp32_cpu *c) {
    uint32_t pc = c->r[15];
    if (pc - GP32_RAM_BASE >= GP32_RAM_SIZE) gp32_fatal(c, "execution left RAM (pc=%08X)", pc);
    if (c->cpsr & GP32_PSR_T) {
        uint32_t insn = gp32_rd16(c, pc);
        c->r[15] = pc + 2;
        switch (thumb_classify(insn)) {
        case THUMB_OP: thumb_op(c, insn, pc + 4); break;
        case THUMB_BCOND:
            if (gp32_cond(c, (insn >> 8) & 15)) c->r[15] = pc + 4 + (uint32_t)((int32_t)(int8_t)insn * 2);
            break;
        case THUMB_B: c->r[15] = pc + 4 + (uint32_t)(((int32_t)(insn << 21) >> 21) * 2); break;
        case THUMB_BL_HI: c->r[14] = pc + 4 + ((uint32_t)((int32_t)(insn << 21) >> 21) << 12); break;
        case THUMB_BL_LO: {
            uint32_t t = c->r[14] + ((insn & 0x7FF) << 1);
            c->r[14] = (pc + 2) | 1;
            c->r[15] = t & ~1u;
            break;
        }
        case THUMB_SWI: gp32_swi(c, insn & 0xFF); break;
        default: gp32_undefined(c, insn, pc); break;
        }
    } else {
        uint32_t insn = gp32_rd32(c, pc);
        c->r[15] = pc + 4;
        if (!gp32_cond(c, insn >> 28)) return;
        switch (arm_classify(insn)) {
        case ARM_DP: arm_dp(c, insn, pc + 8); break;
        case ARM_MUL: arm_mul(c, insn); break;
        case ARM_MULL: arm_mull(c, insn); break;
        case ARM_SWP: arm_swp(c, insn); break;
        case ARM_BX: arm_bx(c, insn, pc + 8); break;
        case ARM_MRS: arm_mrs(c, insn); break;
        case ARM_MSR: arm_msr(c, insn); break;
        case ARM_LDRSTR: arm_ldrstr(c, insn, pc + 8); break;
        case ARM_LDRSTRH: arm_ldrstrh(c, insn, pc + 8); break;
        case ARM_LDM: arm_ldm(c, insn); break;
        case ARM_STM: arm_stm(c, insn, pc + 8); break;
        case ARM_BL: c->r[14] = pc + 4; /* fall through */
        case ARM_B: c->r[15] = pc + 8 + (uint32_t)(((int32_t)(insn << 8) >> 8) * 4); break;
        case ARM_COP: gp32_coproc(c, insn); break;
        case ARM_SWI: gp32_swi(c, insn & 0x00FFFFFFu); break;
        default: gp32_undefined(c, insn, pc); break;
        }
    }
}

void gp32_interp_one(gp32_cpu *c) { interp_step(c); }

/* Interpret until control reaches recompiled code again. */
static void interp_run(gp32_cpu *c) {
    for (;;) {
        uint32_t pc;
        if (c->cycles <= 0) gp32_event(c);
        pc = c->r[15];
        if (pc - GP32_BIOS_STUB_BASE < GP32_BIOS_STUB_COUNT * 4u) { gp32_bios_call(c); continue; }
        if (gp32_lookup(c) && !gp32_opt.interp) return;
        if (!gp32_opt.interp && c->native_ok && !g_warned_interp && ++g_interp_insns > 2000000) {
            g_warned_interp = 1;
            fprintf(stderr, "gp32: note: a lot of code is running in the interpreter (last pc %08X);\n"
                            "      it was not found by the recompiler's analysis.\n", pc);
        }
        interp_step(c);
        c->cycles--;
        /* writes into recompiled code are checked for when control returns to it */
        if (c->native_ok && c->r[15] - g_load < g_span && pc - g_load >= g_span) gp32_code_overwritten(c);
    }
}

void gp32_cpu_reset(gp32_cpu *c) {
    const gp32_game *g = gp32_the_game;
    uint32_t i;
    uint8_t *ram = c->ram;
    memset(c, 0, sizeof(*c));
    c->ram = ram;
    memset(ram, 0, GP32_RAM_SIZE);
    memcpy(ram + (g->load_addr - GP32_RAM_BASE), g->image, g->image_size);
    g_load = g->load_addr;
    g_span = (g->image_size + 3u) & ~3u;
    for (i = 0; i < 2; i++) {
        free(g_table[i]);
        g_table[i] = (gp32_fn *)calloc(g_span / 2 + 1, sizeof(gp32_fn));
        if (!g_table[i]) { fprintf(stderr, "gp32: out of memory\n"); exit(1); }
    }
    for (i = 0; i < g->num_entries; i++) {
        uint32_t a = g->entries[i].addr;
        if ((a & ~1u) - g_load < g_span) g_table[a & 1][((a & ~1u) - g_load) >> 1] = g->entries[i].fn;
    }
    c->native_ok = !gp32_opt.interp;
    /* the state the firmware's loader leaves behind */
    c->cpsr = GP32_MODE_SVC | GP32_PSR_F;
    c->bank_sp[2] = GP32_IRQ_STACK;
    c->r[13] = GP32_SVC_STACK;
    c->r[14] = GP32_BIOS_STUB_BASE; /* returning from the entry point exits */
    c->r[15] = g->entry;
}

void gp32_run(gp32_cpu *c) {
    if (setjmp(g_exit)) return;
    for (;;) {
        gp32_fn f;
        c->depth = 0;
        f = gp32_opt.interp ? NULL : gp32_lookup(c);
        if (f) f(c);
        else interp_run(c);
    }
}

int gp32_exit_code(void) { return g_exit_code; }
