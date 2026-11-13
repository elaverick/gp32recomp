/* gp32rt.h - public interface of the GP32 recompilation runtime.
 *
 * Generated code, the fallback interpreter and the hardware model all share
 * this one CPU state structure.  Everything the guest can observe lives in
 * it, so native code may be abandoned at any block boundary and resumed
 * later (by native code or by the interpreter) without loss.
 */
#ifndef GP32RT_H
#define GP32RT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GP32_RAM_BASE 0x0C000000u
#define GP32_RAM_SIZE 0x00800000u /* 8 MB SDRAM */

/* CPSR bits kept in gp32_cpu.cpsr (N/Z/C/V are kept unpacked). */
#define GP32_PSR_T 0x20u
#define GP32_PSR_F 0x40u
#define GP32_PSR_I 0x80u
#define GP32_PSR_MODE 0x1Fu

#define GP32_MODE_USR 0x10u
#define GP32_MODE_FIQ 0x11u
#define GP32_MODE_IRQ 0x12u
#define GP32_MODE_SVC 0x13u
#define GP32_MODE_ABT 0x17u
#define GP32_MODE_UND 0x1Bu
#define GP32_MODE_SYS 0x1Fu

#if defined(_MSC_VER)
#define GP32_INLINE static __forceinline
#define GP32_NOINLINE __declspec(noinline)
#else
#define GP32_INLINE static inline __attribute__((always_inline))
#define GP32_NOINLINE __attribute__((noinline))
#endif

typedef struct gp32_cpu gp32_cpu;
typedef void (*gp32_fn)(gp32_cpu *);

struct gp32_cpu {
    uint32_t r[16];         /* current-mode registers; r[15] is only valid at block exits */
    uint32_t nf, zf, cf, vf; /* condition flags, each 0 or 1 */
    uint32_t cpsr;          /* I, F, T and mode bits */
    uint32_t spsr;          /* SPSR of the current mode */
    int64_t cycles;         /* instructions left before the next hardware event */
    uint8_t *ram;           /* GP32_RAM_SIZE bytes */
    uint32_t depth;         /* native call nesting, bounded by GP32_MAX_DEPTH */
    uint32_t native_ok;     /* cleared when the guest overwrites recompiled code */
    /* banked registers: 0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und */
    uint32_t bank_sp[6], bank_lr[6], bank_spsr[6];
    uint32_t bank_fiq[5], bank_usr[5]; /* r8-r12 */
    uint32_t cp15_ctrl, cp15_ttb, cp15_dacr;
};

/* Description of one recompiled image, emitted by gp32recomp. */
typedef struct gp32_entry {
    uint32_t addr; /* bit 0 set for Thumb code */
    gp32_fn fn;
} gp32_entry;

typedef struct gp32_game {
    const char *name;
    uint32_t load_addr;
    uint32_t entry;
    uint32_t image_size;
    const uint8_t *image;
    const gp32_entry *entries;
    uint32_t num_entries;
} gp32_game;

/* ---- provided by the runtime ------------------------------------------ */
int gp32_main(int argc, char **argv, const gp32_game *game);

uint32_t gp32_mmio_read(gp32_cpu *c, uint32_t addr, unsigned size);
void gp32_mmio_write(gp32_cpu *c, uint32_t addr, uint32_t val, unsigned size);

int gp32_event(gp32_cpu *c);            /* returns non-zero if r[15] changed */
void gp32_swi(gp32_cpu *c, uint32_t n); /* BIOS call; may change r[15] */
void gp32_tail(gp32_cpu *c);            /* continue at r[15] natively if possible */
gp32_fn gp32_lookup(gp32_cpu *c);
void gp32_interp_one(gp32_cpu *c);      /* interpret the instruction at r[15] */

uint32_t gp32_get_cpsr(const gp32_cpu *c);
void gp32_set_cpsr(gp32_cpu *c, uint32_t value);
void gp32_restore_cpsr(gp32_cpu *c);    /* CPSR = SPSR (exception return) */
uint32_t gp32_get_user_reg(gp32_cpu *c, unsigned n);
void gp32_set_user_reg(gp32_cpu *c, unsigned n, uint32_t v);
void gp32_coproc(gp32_cpu *c, uint32_t insn);
void gp32_undefined(gp32_cpu *c, uint32_t insn, uint32_t addr);

#define GP32_MAX_DEPTH 400u

/* ---- memory ------------------------------------------------------------ */
GP32_INLINE uint32_t gp32_rd32(gp32_cpu *c, uint32_t a) {
    uint32_t o = (a & ~3u) - GP32_RAM_BASE;
    if (o < GP32_RAM_SIZE) { uint32_t v; memcpy(&v, c->ram + o, 4); return v; }
    return gp32_mmio_read(c, a & ~3u, 4);
}
GP32_INLINE uint32_t gp32_rd16(gp32_cpu *c, uint32_t a) {
    uint32_t o = (a & ~1u) - GP32_RAM_BASE;
    if (o < GP32_RAM_SIZE) { uint16_t v; memcpy(&v, c->ram + o, 2); return v; }
    return gp32_mmio_read(c, a & ~1u, 2) & 0xFFFFu;
}
GP32_INLINE uint32_t gp32_rd8(gp32_cpu *c, uint32_t a) {
    uint32_t o = a - GP32_RAM_BASE;
    if (o < GP32_RAM_SIZE) return c->ram[o];
    return gp32_mmio_read(c, a, 1) & 0xFFu;
}
GP32_INLINE void gp32_wr32(gp32_cpu *c, uint32_t a, uint32_t v) {
    uint32_t o = (a & ~3u) - GP32_RAM_BASE;
    if (o < GP32_RAM_SIZE) memcpy(c->ram + o, &v, 4);
    else gp32_mmio_write(c, a & ~3u, v, 4);
}
GP32_INLINE void gp32_wr16(gp32_cpu *c, uint32_t a, uint32_t v) {
    uint32_t o = (a & ~1u) - GP32_RAM_BASE;
    if (o < GP32_RAM_SIZE) { uint16_t h = (uint16_t)v; memcpy(c->ram + o, &h, 2); }
    else gp32_mmio_write(c, a & ~1u, v & 0xFFFFu, 2);
}
GP32_INLINE void gp32_wr8(gp32_cpu *c, uint32_t a, uint32_t v) {
    uint32_t o = a - GP32_RAM_BASE;
    if (o < GP32_RAM_SIZE) c->ram[o] = (uint8_t)v;
    else gp32_mmio_write(c, a, v & 0xFFu, 1);
}

/* ---- helpers used by generated code ------------------------------------ */

/* Start of a basic block: service hardware when the budget is spent. */
#define GP32_BLOCK(addr, n)                              \
    if (c->cycles <= 0) {                                \
        c->r[15] = (addr);                               \
        if (gp32_event(c)) return;                       \
    }                                                    \
    c->cycles -= (n);

/* Direct call to a recompiled function; `ret` is the guest return address. */
#define GP32_CALL(fn, target, ret)                       \
    do {                                                 \
        c->r[15] = (target);                             \
        if (c->depth >= GP32_MAX_DEPTH) return;          \
        c->depth++;                                      \
        fn(c);                                           \
        c->depth--;                                      \
        if (c->r[15] != (ret)) return;                   \
    } while (0)

/* Call through a guest pointer already stored in r[15]. */
#define GP32_CALL_IND(ret)                               \
    do {                                                 \
        gp32_fn f_ = gp32_lookup(c);                     \
        if (!f_ || c->depth >= GP32_MAX_DEPTH) return;   \
        c->depth++;                                      \
        f_(c);                                           \
        c->depth--;                                      \
        if (c->r[15] != (ret)) return;                   \
    } while (0)

/* Leave the current function for an address outside it. */
#define GP32_TAIL()                                      \
    do {                                                 \
        gp32_tail(c);                                    \
        return;                                          \
    } while (0)

#define GP32_SWI(n, next)                                \
    do {                                                 \
        c->r[15] = (next);                               \
        gp32_swi(c, (n));                                \
        if (c->r[15] != (next)) return;                  \
    } while (0)

/* An instruction the translator leaves to the interpreter. */
#define GP32_INTERP(addr, next)                          \
    do {                                                 \
        c->r[15] = (addr);                               \
        gp32_interp_one(c);                              \
        if (c->r[15] != (next)) return;                  \
    } while (0)

#ifdef __cplusplus
}
#endif
#endif
