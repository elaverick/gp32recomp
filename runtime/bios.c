/* bios.c - high-level emulation of the GP32 firmware.
 *
 * The boot ROM is not needed: the services programs reach through SWI, and
 * the callbacks the firmware hands out, are implemented natively here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "internal.h"

#define STUB(n) (GP32_BIOS_STUB_BASE + 4u * (n))
enum { STUB_EXIT, STUB_FLIP, STUB_RET0, STUB_SMC0 };

static struct {
    uint32_t timer_cb[4];
    uint32_t path_len;
    int lcd_on;
} bios;

static void reg_and(gp32_cpu *c, uint32_t addr, uint32_t and_mask, uint32_t or_mask) {
    gp32_mmio_write(c, addr, (gp32_mmio_read(c, addr, 4) & and_mask) | or_mask, 4);
}

static void put_str(gp32_cpu *c, uint32_t addr, const char *s) {
    do gp32_wr8(c, addr++, (uint8_t)*s); while (*s++);
}

static uint32_t fb_addr(int bpp, unsigned idx) {
    return GP32_FB_BASE + (bpp == 16 ? (idx & 1) * 0x25800u : (idx & 3) * 0x12C00u);
}

/* ---- ARM semihosting, used by programs linked with newlib's stock crt0 -- */
static void semihost(gp32_cpu *c) {
    uint32_t op = c->r[0], p = c->r[1];
    switch (op) {
    case 0x03: fputc((int)gp32_rd8(c, p), stdout); break;
    case 0x04: {
        uint32_t ch;
        while ((ch = gp32_rd8(c, p++)) != 0) fputc((int)ch, stdout);
        fflush(stdout);
        break;
    }
    case 0x05: {
        uint32_t buf = gp32_rd32(c, p + 4), len = gp32_rd32(c, p + 8);
        while (len--) fputc((int)gp32_rd8(c, buf++), stdout);
        fflush(stdout);
        c->r[0] = 0;
        break;
    }
    case 0x09: c->r[0] = 1; break;
    case 0x10: c->r[0] = gp32_ms() / 10; break;
    case 0x11: c->r[0] = (uint32_t)time(NULL); break;
    case 0x15: gp32_wr8(c, gp32_rd32(c, p), 0); c->r[0] = 0; break;
    case 0x16: {
        uint32_t blk = gp32_rd32(c, p);
        gp32_wr32(c, blk + 0, 0);
        gp32_wr32(c, blk + 4, GP32_SYS_BASE);
        gp32_wr32(c, blk + 8, GP32_SVC_STACK);
        gp32_wr32(c, blk + 12, GP32_SYS_BASE);
        break;
    }
    case 0x18: fflush(stdout); gp32_quit(c, p == 0x20026u ? 0 : (int)(p & 0xFF ? p & 0xFF : 1));
    default: c->r[0] = 0xFFFFFFFFu; break;
    }
}

/* ---- SWI 0x08: LCD ----------------------------------------------------- */
static void swi_lcd(gp32_cpu *c) {
    uint32_t a0 = c->r[0], a1 = c->r[1];
    int bpp = gp32_lcd_bpp();
    switch (c->r[2]) {
    case 0: /* GpGraphicModeSet(bpp, palette) */
        bpp = a0 == 16 ? 16 : 8;
        gp32_lcd_set(c, fb_addr(bpp, 0), bpp);
        if (bpp == 8 && a1 >= GP32_RAM_BASE) {
            unsigned i;
            for (i = 0; i < 256; i++) {
                uint32_t v = gp32_rd32(c, a1 + 4 * i);
                gp32_lcd_palette()[i] = v & 0xFFFF;
                gp32_wr32(c, GP32_PAL_SHADOW + 4 * i, v);
            }
        }
        bios.lcd_on = 1;
        c->r[0] = 0;
        break;
    case 1: /* GpLcdSurfaceGet(surface, index) */
        gp32_wr32(c, a0 + 0, fb_addr(bpp, a1));
        gp32_wr32(c, a0 + 4, (uint32_t)bpp);
        gp32_wr32(c, a0 + 8, 320);
        gp32_wr32(c, a0 + 12, 240);
        gp32_wr32(c, a0 + 16, 0);
        gp32_wr32(c, a0 + 20, 0);
        gp32_wr32(c, a0 + 24, fb_addr(bpp, a1));
        c->r[0] = 0;
        break;
    case 3: bios.lcd_on = 1; reg_and(c, 0x14A00000u, ~0u, 1); c->r[0] = 0; break;
    case 4: bios.lcd_on = 0; reg_and(c, 0x14A00000u, ~1u, 0); c->r[0] = 0; break;
    case 5: c->r[0] = bios.lcd_on ? 0xE0 : 0; break;
    case 6: c->r[0] = gp32_lcd_fb(); break;
    case 7: { /* GpLcdInfoGet */
        unsigned i, n = bpp == 16 ? 2 : 4;
        gp32_wr32(c, a0, (n << 8) | ((uint32_t)bpp << 16) | (bios.lcd_on ? 0xE0000000u : 0));
        gp32_wr32(c, a0 + 4, bpp == 16 ? 0x25800u : 0x12C00u);
        for (i = 0; i < 4; i++) gp32_wr32(c, a0 + 8 + 4 * i, i < n ? fb_addr(bpp, i) : 0);
        gp32_wr32(c, a0 + 24, 0x14A00400u);
        gp32_wr32(c, a0 + 28, GP32_PAL_SHADOW);
        c->r[0] = 0;
        break;
    }
    default: c->r[0] = 0; break;
    }
}

/* Copy the logical palette the SDK keeps in RAM to the LCD controller. */
static void palette_realize(gp32_cpu *c, uint32_t src) {
    unsigned i;
    if (src < GP32_RAM_BASE) src = GP32_PAL_SHADOW;
    for (i = 0; i < 256; i++) gp32_lcd_palette()[i] = gp32_rd32(c, src + 4 * i) & 0xFFFF;
}

static uint32_t keys_from_ports(uint32_t pb, uint32_t pe) {
    /* active-low port bits to the firmware's active-low key word */
    return ~((((~pb) >> 8) & 0xFF) | ((((~pe) >> 6) & 3) << 8));
}

void gp32_swi(gp32_cpu *c, uint32_t n) {
    if (gp32_opt.trace)
        fprintf(stderr, "gp32: swi %X r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X\n", n, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
    switch (n) {
    case 0x123456: case 0xAB: semihost(c); break;
    case 0x02: case 0x07: case 0x17: break; /* MMU remap, port reset, volume */
    case 0x04: gp32_quit(c, 0); break;
    case 0x05: /* run the program image at r0 */
        if (c->r[0] >= GP32_RAM_BASE && c->r[0] < GP32_SYS_BASE) {
            if (c->r[1] >= GP32_RAM_BASE) c->r[13] = c->r[1];
            c->r[15] = c->r[0] & ~3u;
            gp32_code_overwritten(c);
        }
        break;
    case 0x08: swi_lcd(c); break;
    case 0x09: /* install interrupt handler r1 for source r0 */
        if (c->r[0] < 32) {
            gp32_wr32(c, GP32_IRQ_TABLE + 4 * c->r[0], c->r[1]);
            reg_and(c, 0x14400008u, ~(1u << c->r[0]), 0);
        }
        break;
    case 0x0A:
        if (c->r[0] < 32) {
            reg_and(c, 0x14400008u, ~0u, 1u << c->r[0]);
            gp32_wr32(c, GP32_IRQ_TABLE + 4 * c->r[0], 0);
        }
        break;
    case 0x0B:
        switch (c->r[0]) {
        case 0: c->r[0] = STUB(STUB_FLIP); break;
        case 2:
            gp32_wr32(c, c->r[1], 0x14A00400u);
            gp32_wr32(c, c->r[2], GP32_PAL_SHADOW);
            c->r[0] = 0;
            break;
        case 3: {
            unsigned i;
            for (i = 0; i < 16; i++) gp32_wr32(c, c->r[1] + 4 * i, STUB(STUB_SMC0 + i));
            c->r[0] = c->r[1];
            break;
        }
        case 4: c->r[0] = GP32_CLK_INFO; break;
        case 5: c->r[0] = GP32_SYS_BASE; break;
        case 6: c->r[0] = GP32_TICK_ADDR; break;
        default: c->r[0] = 0; break;
        }
        break;
    case 0x0D: { /* GpClockSpeedChange(speed, pll setting, divider mode) */
        uint32_t pll = gp32_rd32(c, c->r[0] + 4), mode = gp32_rd32(c, c->r[0] + 8);
        if (pll) gp32_hw_set_clock(c, pll, mode & 3);
        c->r[0] = 0;
        break;
    }
    case 0x0F:
        switch (c->r[4]) {
        case 1: /* GpAppPathGet(&len) */
            put_str(c, GP32_ARG_AREA, "gp:\\gpmm\\");
            if (c->r[0] >= GP32_RAM_BASE) gp32_wr32(c, c->r[0], 9);
            c->r[0] = GP32_ARG_AREA;
            break;
        case 3: /* GpUserInfoGet(id, password) */
            if (c->r[0] >= GP32_RAM_BASE) gp32_wr8(c, c->r[0], 0);
            if (c->r[1] >= GP32_RAM_BASE) gp32_wr8(c, c->r[1], 0);
            c->r[0] = 0;
            break;
        default: c->r[0] = 0; break;
        }
        break;
    case 0x10:
        if (c->r[0] == 0) {
            c->r[0] = 0x1560000Cu;
            c->r[1] = 0x15600030u;
        } else {
            c->r[0] = keys_from_ports(c->r[1], c->r[2]);
        }
        break;
    case 0x15: c->r[0] = 0; c->r[1] = 0; break; /* no command line */
    case 0x16:
        if (c->r[0] == 5) {
            gp32_wr32(c, c->r[1], 0x14A00400u);
            gp32_wr32(c, c->r[2], GP32_PAL_SHADOW);
        } else if (c->r[0] == 1 || c->r[0] == 2) {
            palette_realize(c, c->r[2]);
        }
        c->r[0] = 1;
        break;
    default:
        if (gp32_opt.trace) fprintf(stderr, "gp32: unimplemented swi %X\n", n);
        c->r[0] = 0;
        break;
    }
}

/* Called when the guest jumps to one of the firmware routines whose address
 * we handed out; behaves like the routine followed by "bx lr". */
void gp32_bios_call(gp32_cpu *c) {
    unsigned n = (c->r[15] - GP32_BIOS_STUB_BASE) / 4;
    uint32_t lr = c->r[14];
    switch (n) {
    case STUB_EXIT: gp32_quit(c, (int)(c->r[0] & 0xFF)); break;
    case STUB_FLIP: { /* GpSurfaceSet / GpSurfaceFlip(surface) */
        uint32_t fb = gp32_rd32(c, c->r[0]), bpp = gp32_rd32(c, c->r[0] + 4);
        if (fb >= GP32_RAM_BASE) gp32_lcd_set(c, fb, bpp == 16 ? 16 : gp32_lcd_bpp());
        gp32_wait_vsync(c);
        break;
    }
    default: c->r[0] = 0; break;
    }
    if (lr & 1) { c->cpsr |= GP32_PSR_T; c->r[15] = lr & ~1u; }
    else { c->cpsr &= ~GP32_PSR_T; c->r[15] = lr & ~3u; }
}

void gp32_bios_ms_tick(gp32_cpu *c) { (void)c; }

void gp32_bios_reset(gp32_cpu *c) {
    memset(&bios, 0, sizeof(bios));
    bios.lcd_on = 1;
    (void)c;
}
