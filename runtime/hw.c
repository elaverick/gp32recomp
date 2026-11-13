/* hw.c - Samsung S3C2400 peripherals as the GP32 uses them.
 *
 * Time is measured in executed instructions.  Generated code counts
 * c->cycles down; when it reaches zero gp32_event() runs every device that
 * is due, delivers interrupts and reloads the counter with the distance to
 * the next deadline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "internal.h"

#define REGION(a) (((a) - 0x14000000u) >> 20)
#define NREGION 0x1Cu

enum { SRCPND, INTMOD, INTMSK, PRIORITY, INTPND, INTOFFSET };
enum { INT_TICK = 8, INT_TIMER0 = 10, INT_DMA0 = 17, INT_RTC = 30 };

typedef struct {
    double when;       /* expiry time, <0 when stopped */
    double period;     /* instruction ticks per count */
    uint32_t cntb, cmpb, cnt;
} pwm_timer;

typedef struct {
    uint32_t disrc, didst, dcon, dmasktrig;
    uint32_t cur_src, cur_dst, cur_tc;
    int busy;
} dma_chan;

static struct {
    uint32_t io[NREGION][0x100]; /* plain storage for registers with no behaviour */
    uint32_t irq[6];
    uint32_t lcd[8];
    uint32_t pal[256];
    uint32_t mpllcon, clkdivn;
    uint32_t tcfg0, tcfg1, tcon;
    pwm_timer tm[5];
    dma_chan dma[4];
    uint32_t iiscon, iismod, iispsr, iisfcon;
    uint32_t gpdat[7], gpcon[7], gpup[7];
    uint32_t ticnt;
    uint32_t buttons;
    uint64_t target;       /* absolute time at which c->cycles reaches zero */
    double ips;            /* instructions per emulated second */
    double ev_ms, ev_vsync, ev_rtc, ev_hle[4];
    double hle_period[4];
    double frame_ticks, frame_start;
    uint32_t ms;
    uint64_t frame_no;
    double audio_due;
    int16_t fifo_l;
    int fifo_have_l;
    uint32_t xrgb[GP32_LCD_W * GP32_LCD_H];
    uint64_t real_start_us;
    double virt_start;
    int vsync_wait;
    const char *key_script;
    uint32_t script_keys;
} hw;

static uint64_t now_ticks(const gp32_cpu *c) { return hw.target - (uint64_t)c->cycles; }

void gp32_force_event(gp32_cpu *c) {
    hw.target -= (uint64_t)c->cycles;
    c->cycles = 0;
}

/* ---- clocks ------------------------------------------------------------ */
uint32_t gp32_hw_fclk(void) {
    uint32_t m = (hw.mpllcon >> 12) & 0xFF, p = (hw.mpllcon >> 4) & 0x3F, s = hw.mpllcon & 3;
    return (uint32_t)(((double)(m + 8) * 12000000.0) / (double)((p + 2) * (1u << s)));
}
uint32_t gp32_hw_hclk(void) { return gp32_hw_fclk() / ((hw.clkdivn & 2) ? 2 : 1); }
uint32_t gp32_hw_pclk(void) { return gp32_hw_hclk() / ((hw.clkdivn & 1) ? 2 : 1); }

static void clock_changed(gp32_cpu *c) {
    hw.ips = (double)gp32_hw_fclk() / gp32_opt.cpi;
    if (hw.ips < 1e6) hw.ips = 1e6;
    gp32_wr32(c, GP32_CLK_INFO + 0, gp32_hw_fclk());
    gp32_wr32(c, GP32_CLK_INFO + 4, gp32_hw_hclk());
    gp32_wr32(c, GP32_CLK_INFO + 8, gp32_hw_pclk());
}

void gp32_hw_set_clock(gp32_cpu *c, uint32_t mpllcon, uint32_t clkdivn) {
    hw.mpllcon = mpllcon;
    hw.clkdivn = clkdivn;
    clock_changed(c);
}

/* ---- interrupts -------------------------------------------------------- */
void gp32_irq_request(gp32_cpu *c, unsigned n) {
    hw.irq[SRCPND] |= 1u << n;
    if (!(hw.irq[INTMSK] & (1u << n))) gp32_force_event(c);
}

/* The boot ROM's IRQ veneer: acknowledge the source and jump through the
 * handler table it keeps in RAM. */
static void irq_deliver(gp32_cpu *c) {
    uint32_t pend = hw.irq[SRCPND] & ~hw.irq[INTMSK];
    unsigned n = 0;
    if (!pend || (c->cpsr & GP32_PSR_I)) return;
    while (!(pend & (1u << n))) n++;
    hw.irq[SRCPND] &= ~(1u << n);
    hw.irq[INTPND] = 0;
    hw.irq[INTOFFSET] = n;
    {
        uint32_t handler = gp32_rd32(c, GP32_IRQ_TABLE + 4 * n);
        if (handler >= GP32_RAM_BASE && handler < GP32_RAM_BASE + GP32_RAM_SIZE) gp32_enter_irq(c, handler);
    }
}

/* ---- PWM timers -------------------------------------------------------- */
static double timer_tick_len(unsigned n) {
    static const double div[16] = { 2, 4, 8, 16, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    uint32_t presc = n < 2 ? (hw.tcfg0 & 0xFF) : ((hw.tcfg0 >> 8) & 0xFF);
    double f = (double)gp32_hw_pclk() / (double)(presc + 1) / div[(hw.tcfg1 >> (4 * n)) & 15];
    return hw.ips / f;
}

static void timer_bits(unsigned n, unsigned *start, unsigned *manual, unsigned *reload) {
    static const unsigned char s[5] = { 0, 8, 12, 16, 20 };
    *start = (hw.tcon >> s[n]) & 1;
    *manual = (hw.tcon >> (s[n] + 1)) & 1;
    *reload = n == 4 ? (hw.tcon >> 22) & 1 : (hw.tcon >> (s[n] + 3)) & 1;
}

static void tcon_write(gp32_cpu *c, uint32_t v) {
    uint32_t old = hw.tcon;
    unsigned n;
    hw.tcon = v;
    for (n = 0; n < 5; n++) {
        static const unsigned char s[5] = { 0, 8, 12, 16, 20 };
        unsigned start, manual, reload, was = (old >> s[n]) & 1;
        pwm_timer *t = &hw.tm[n];
        timer_bits(n, &start, &manual, &reload);
        if (manual) t->cnt = t->cntb;
        if (start && (!was || t->when < 0)) {
            t->period = timer_tick_len(n);
            t->when = (double)now_ticks(c) + ((double)t->cnt + 1.0) * t->period;
        } else if (!start && was) {
            if (t->when >= 0) {
                double left = (t->when - (double)now_ticks(c)) / t->period;
                t->cnt = left > 0 ? (uint32_t)left : 0;
            }
            t->when = -1;
        }
    }
    gp32_force_event(c);
}

static uint32_t timer_count(const gp32_cpu *c, unsigned n) {
    const pwm_timer *t = &hw.tm[n];
    double left;
    if (t->when < 0) return t->cnt & 0xFFFF;
    left = (t->when - (double)now_ticks(c)) / t->period;
    return left > 0 ? ((uint32_t)left & 0xFFFF) : 0;
}

static void timer_fire(gp32_cpu *c, unsigned n) {
    unsigned start, manual, reload;
    pwm_timer *t = &hw.tm[n];
    timer_bits(n, &start, &manual, &reload);
    gp32_irq_request(c, INT_TIMER0 + n);
    if (reload && start) {
        t->cnt = t->cntb;
        t->period = timer_tick_len(n);
        t->when += ((double)t->cnt + 1.0) * t->period;
    } else {
        t->cnt = 0;
        t->when = -1;
    }
}

/* A periodic interrupt the BIOS runs on behalf of the SDK (see bios.c). */
void gp32_hle_timer(gp32_cpu *c, unsigned idx, uint32_t hz) {
    if (idx >= 4) return;
    if (!hz) { hw.ev_hle[idx] = -1; return; }
    hw.hle_period[idx] = hw.ips / (double)hz;
    hw.ev_hle[idx] = (double)now_ticks(c) + hw.hle_period[idx];
    gp32_force_event(c);
}

/* ---- LCD --------------------------------------------------------------- */
static void lcd_timing(void) {
    uint32_t c1 = hw.lcd[0], c2 = hw.lcd[1], c3 = hw.lcd[2], c4 = hw.lcd[3];
    double lines = ((c2 & 0x3F) + 1) + ((c2 >> 24) + 1) + (((c2 >> 14) & 0x3FF) + 1) + (((c2 >> 6) & 0xFF) + 1);
    double cols = ((c4 & 0xFF) + 1) + (((c3 >> 19) & 0x7F) + 1) + (((c3 >> 8) & 0x7FF) + 1) + ((c3 & 0xFF) + 1);
    double vclk = (double)gp32_hw_hclk() / ((((c1 >> 8) & 0x3FF) + 1) * 2.0);
    double hz = vclk / (lines * cols);
    if (!(hz >= 20.0 && hz <= 150.0)) hz = 60.0;
    hw.frame_ticks = hw.ips / hz;
}

uint32_t gp32_lcd_fb(void) { return hw.lcd[5] << 1; }
int gp32_lcd_bpp(void) { return ((hw.lcd[0] >> 1) & 15) == 12 ? 16 : 8; }
uint32_t *gp32_lcd_palette(void) { return hw.pal; }

void gp32_lcd_set(gp32_cpu *c, uint32_t fb, int bpp) {
    (void)c;
    hw.lcd[0] = (hw.lcd[0] & ~0x1Eu) | ((bpp == 16 ? 12u : 11u) << 1) | 1u;
    hw.lcd[5] = fb >> 1;
}

/* Position of the beam within the frame, as LCDCON1/LCDCON5 report it. */
static void lcd_position(const gp32_cpu *c, uint32_t *linecnt, uint32_t *vstatus) {
    uint32_t c2 = hw.lcd[1];
    uint32_t vspw = (c2 & 0x3F) + 1, vbpd = (c2 >> 24) + 1, lineval = (c2 >> 14) & 0x3FF, vfpd = ((c2 >> 6) & 0xFF) + 1;
    double total = vspw + vbpd + lineval + 1 + vfpd;
    double frac = ((double)now_ticks(c) - hw.frame_start) / hw.frame_ticks;
    uint32_t line;
    if (frac < 0) frac = 0;
    if (frac >= 1) frac = 0.999999;
    line = (uint32_t)(frac * total);
    if (line < vspw) { *vstatus = 0; *linecnt = 0; }
    else if (line < vspw + vbpd) { *vstatus = 1; *linecnt = lineval; }
    else if (line < vspw + vbpd + lineval + 1) { *vstatus = 2; *linecnt = lineval - (line - vspw - vbpd); }
    else { *vstatus = 3; *linecnt = 0; }
}

static uint32_t rgb5551(uint32_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

/* The panel is mounted sideways: the frame buffer holds 320 columns of 240
 * pixels, each column running from the bottom of the screen to the top. */
static void lcd_render(gp32_cpu *c) {
    uint32_t fb = gp32_lcd_fb() - GP32_RAM_BASE;
    int bpp = gp32_lcd_bpp(), x, y;
    if (!(hw.lcd[0] & 1)) { memset(hw.xrgb, 0, sizeof(hw.xrgb)); return; }
    if (bpp == 16) {
        if (fb > GP32_RAM_SIZE - GP32_LCD_W * GP32_LCD_H * 2) return;
        for (x = 0; x < GP32_LCD_W; x++) {
            const uint8_t *col = c->ram + fb + (size_t)x * GP32_LCD_H * 2;
            for (y = 0; y < GP32_LCD_H; y++) {
                const uint8_t *p = col + (GP32_LCD_H - 1 - y) * 2;
                hw.xrgb[y * GP32_LCD_W + x] = rgb5551((uint32_t)p[0] | ((uint32_t)p[1] << 8));
            }
        }
    } else {
        uint32_t lut[256];
        if (fb > GP32_RAM_SIZE - GP32_LCD_W * GP32_LCD_H) return;
        for (x = 0; x < 256; x++) lut[x] = rgb5551(hw.pal[x]);
        for (x = 0; x < GP32_LCD_W; x++) {
            const uint8_t *col = c->ram + fb + (size_t)x * GP32_LCD_H;
            for (y = 0; y < GP32_LCD_H; y++) hw.xrgb[y * GP32_LCD_W + x] = lut[col[GP32_LCD_H - 1 - y]];
        }
    }
}

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    uint8_t h[54] = { 'B', 'M' };
    uint32_t size = 54 + GP32_LCD_W * GP32_LCD_H * 3;
    int x, y;
    if (!f) { fprintf(stderr, "gp32: cannot write %s\n", path); return; }
    h[2] = (uint8_t)size; h[3] = (uint8_t)(size >> 8); h[4] = (uint8_t)(size >> 16);
    h[10] = 54; h[14] = 40;
    h[18] = (uint8_t)GP32_LCD_W; h[19] = (uint8_t)(GP32_LCD_W >> 8);
    h[22] = (uint8_t)GP32_LCD_H;
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    for (y = GP32_LCD_H - 1; y >= 0; y--)
        for (x = 0; x < GP32_LCD_W; x++) {
            uint32_t p = hw.xrgb[y * GP32_LCD_W + x];
            uint8_t bgr[3];
            bgr[0] = (uint8_t)p; bgr[1] = (uint8_t)(p >> 8); bgr[2] = (uint8_t)(p >> 16);
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

static void script_keys(void) {
    /* "frame:mask,frame:mask" - the mask applies from that frame onwards */
    const char *p = hw.key_script;
    while (p && *p) {
        char *end;
        unsigned long frame = strtoul(p, &end, 0), mask;
        if (*end != ':') break;
        mask = strtoul(end + 1, &end, 0);
        if (frame > hw.frame_no) break;
        hw.script_keys = (uint32_t)mask;
        hw.key_script = p = (*end == ',') ? end + 1 : end;
    }
}

static void vsync(gp32_cpu *c) {
    hw.frame_start = hw.ev_vsync;
    lcd_timing();
    lcd_render(c);
    hw.frame_no++;
    hw.vsync_wait = 0;
    if (plat_present(hw.xrgb)) gp32_quit(c, 0);
    script_keys();
    hw.buttons = plat_buttons() | hw.script_keys;
    if (c->native_ok && (hw.frame_no & 31) == 0) gp32_code_overwritten(c);
    if (gp32_opt.frames && hw.frame_no >= (uint64_t)gp32_opt.frames) {
        if (gp32_opt.dump) write_bmp(gp32_opt.dump);
        gp32_quit(c, 0);
    }
    if (!gp32_opt.no_throttle) {
        /* Sleep until real time catches up with emulated time. */
        double virt_us = (hw.ev_vsync - hw.virt_start) / hw.ips * 1e6;
        uint64_t real_us = plat_time_us() - hw.real_start_us;
        if (virt_us > (double)real_us + 500.0) {
            plat_sleep_us((uint64_t)(virt_us - (double)real_us));
        } else if ((double)real_us > virt_us + 100000.0) {
            hw.real_start_us = plat_time_us();
            hw.virt_start = hw.ev_vsync;
        }
    }
}

uint32_t gp32_buttons(void) { return hw.buttons; }
uint32_t gp32_ms(void) { return hw.ms; }

/* Used by BIOS calls that block until the next frame: burn the time. */
void gp32_wait_vsync(gp32_cpu *c) {
    double left = hw.ev_vsync - (double)now_ticks(c);
    if (left > 0) c->cycles -= (int64_t)left;
    if (c->cycles > 0) gp32_force_event(c);
}

/* ---- sound ------------------------------------------------------------- */
static FILE *wav_file;
static uint32_t wav_bytes, wav_rate;

static void wav_header(void) {
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
    uint32_t v[2], i;
    v[0] = wav_rate;
    v[1] = wav_rate * 4;
    for (i = 0; i < 8; i++) h[24 + i] = (uint8_t)(v[i / 4] >> (8 * (i % 4)));
    h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4);
    for (i = 0; i < 4; i++) { h[4 + i] = (uint8_t)((wav_bytes + 36) >> (8 * i)); h[40 + i] = (uint8_t)(wav_bytes >> (8 * i)); }
    fseek(wav_file, 0, SEEK_SET);
    fwrite(h, 1, 44, wav_file);
}

void gp32_hw_shutdown(void) {
    if (!wav_file) return;
    wav_header();
    fclose(wav_file);
    wav_file = NULL;
}

static void audio_out(const int16_t *stereo, unsigned frames, uint32_t rate) {
    if (gp32_opt.wav) {
        unsigned i;
        if (!wav_file && (wav_file = fopen(gp32_opt.wav, "wb")) != NULL) { wav_rate = rate; wav_bytes = 0; wav_header(); }
        for (i = 0; wav_file && i < frames * 2; i++) {
            fputc(stereo[i] & 0xFF, wav_file);
            fputc((stereo[i] >> 8) & 0xFF, wav_file);
        }
        wav_bytes += frames * 4;
    }
    if (!gp32_opt.no_audio) plat_audio(stereo, (int)frames, (int)rate);
}
static uint32_t audio_rate(void) {
    uint32_t presc = ((hw.iispsr >> 5) & 31) + 1;
    double r = (double)gp32_hw_pclk() / presc / ((hw.iismod & 4) ? 384.0 : 256.0);
    static const uint32_t common[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000 };
    unsigned i, best = 3;
    if (!(hw.iiscon & 1)) return 0;
    for (i = 0; i < 7; i++) {
        double d = r - common[i], e = r - common[best];
        if ((d < 0 ? -d : d) < (e < 0 ? -e : e)) best = i;
    }
    return common[best];
}

static void dma_finish(gp32_cpu *c, unsigned n) {
    dma_chan *d = &hw.dma[n];
    if (d->dcon & (1u << 28)) gp32_irq_request(c, INT_DMA0 + n);
    if (!(d->dcon & (1u << 22))) { /* auto reload */
        d->cur_src = d->disrc & 0x1FFFFFFFu;
        d->cur_dst = d->didst & 0x1FFFFFFFu;
        d->cur_tc = d->dcon & 0xFFFFFu;
    } else {
        d->busy = 0;
        d->dmasktrig &= ~2u;
    }
}

static int dma_is_audio(const dma_chan *d) {
    return d->busy && (d->dcon & (1u << 23)) && (d->didst & 0x1FFFFFFFu) == 0x15508010u;
}

/* Feed the host sound device with one millisecond of whatever the IIS DMA
 * channel is playing, reading guest memory as late as possible. */
static void audio_pump(gp32_cpu *c) {
    uint32_t rate = audio_rate();
    int16_t out[2 * 64];
    unsigned n, frames = 0, want;
    dma_chan *d = NULL;
    for (n = 0; n < 4; n++)
        if (dma_is_audio(&hw.dma[n])) d = &hw.dma[n];
    if (!rate || !d) { hw.audio_due = 0; return; }
    hw.audio_due += rate / 1000.0;
    want = (unsigned)hw.audio_due;
    if (want > 64) want = 64;
    hw.audio_due -= want;
    while (frames < want && d->busy) {
        unsigned dsz = (d->dcon >> 20) & 3, ch;
        int16_t s[2];
        for (ch = 0; ch < 2; ch++) {
            if (!d->cur_tc) { s[ch] = 0; continue; }
            if (dsz == 0) {
                s[ch] = (int16_t)((int)(int8_t)gp32_rd8(c, d->cur_src) * 256);
                d->cur_src += 1;
            } else {
                s[ch] = (int16_t)gp32_rd16(c, d->cur_src);
                d->cur_src += 2;
            }
            if (!--d->cur_tc) dma_finish(c, (unsigned)(d - hw.dma));
        }
        out[frames * 2] = s[0];
        out[frames * 2 + 1] = s[1];
        frames++;
    }
    if (frames) audio_out(out, frames, rate);
}

static void dma_trigger(gp32_cpu *c, unsigned n, uint32_t v) {
    dma_chan *d = &hw.dma[n];
    d->dmasktrig = v & 7;
    if (v & 4) { d->busy = 0; d->dmasktrig &= ~2u; return; }
    if (!(v & 2)) { d->busy = 0; return; }
    if (!d->busy) {
        d->cur_src = d->disrc & 0x1FFFFFFFu;
        d->cur_dst = d->didst & 0x1FFFFFFFu;
        d->cur_tc = d->dcon & 0xFFFFFu;
        d->busy = 1;
    }
    if ((v & 1) && !(d->dcon & (1u << 23))) { /* software-triggered copy: do it at once */
        unsigned unit = 1u << ((d->dcon >> 20) & 3);
        int sfix = (d->disrc >> 29) & 1, dfix = (d->didst >> 29) & 1;
        if (unit > 4) unit = 4;
        while (d->cur_tc) {
            if (unit == 1) gp32_wr8(c, d->cur_dst, gp32_rd8(c, d->cur_src));
            else if (unit == 2) gp32_wr16(c, d->cur_dst, gp32_rd16(c, d->cur_src));
            else gp32_wr32(c, d->cur_dst, gp32_rd32(c, d->cur_src));
            if (!sfix) d->cur_src += unit;
            if (!dfix) d->cur_dst += unit;
            d->cur_tc--;
        }
        d->dmasktrig &= ~1u;
        d->busy = 0;
        d->dmasktrig &= ~2u;
        if (d->dcon & (1u << 28)) gp32_irq_request(c, INT_DMA0 + n);
    }
}

static void iis_fifo_write(gp32_cpu *c, uint32_t v) {
    uint32_t rate = audio_rate();
    if (!rate) rate = 22050;
    if (!hw.fifo_have_l) {
        hw.fifo_l = (int16_t)v;
        hw.fifo_have_l = 1;
    } else {
        int16_t s[2];
        s[0] = hw.fifo_l;
        s[1] = (int16_t)v;
        hw.fifo_have_l = 0;
        audio_out(s, 1, rate);
    }
    /* The FIFO drains at the sample rate; charge the time a real write would wait. */
    c->cycles -= (int64_t)(hw.ips / (rate * 2.0));
}

/* ---- events ------------------------------------------------------------ */
int gp32_event(gp32_cpu *c) {
    uint64_t now = now_ticks(c);
    uint32_t pc0 = c->r[15], mode0 = c->cpsr;
    double next;
    unsigned n;
    while (hw.ev_ms <= (double)now) {
        hw.ms++;
        gp32_wr32(c, GP32_TICK_ADDR, hw.ms);
        audio_pump(c);
        gp32_bios_ms_tick(c);
        hw.ev_ms += hw.ips / 1000.0;
    }
    for (n = 0; n < 5; n++)
        while (hw.tm[n].when >= 0 && hw.tm[n].when <= (double)now) timer_fire(c, n);
    for (n = 0; n < 4; n++)
        while (hw.ev_hle[n] >= 0 && hw.ev_hle[n] <= (double)now) {
            gp32_irq_request(c, INT_TIMER0 + n);
            hw.ev_hle[n] += hw.hle_period[n];
        }
    while (hw.ev_rtc >= 0 && hw.ev_rtc <= (double)now) {
        gp32_irq_request(c, INT_TICK);
        hw.ev_rtc += hw.ips * ((hw.ticnt & 0x7F) + 1) / 128.0;
    }
    while (hw.ev_vsync <= (double)now) {
        vsync(c);
        hw.ev_vsync += hw.frame_ticks;
    }
    irq_deliver(c);
    next = hw.ev_ms;
    if (hw.ev_vsync < next) next = hw.ev_vsync;
    if (hw.ev_rtc >= 0 && hw.ev_rtc < next) next = hw.ev_rtc;
    for (n = 0; n < 5; n++)
        if (hw.tm[n].when >= 0 && hw.tm[n].when < next) next = hw.tm[n].when;
    for (n = 0; n < 4; n++)
        if (hw.ev_hle[n] >= 0 && hw.ev_hle[n] < next) next = hw.ev_hle[n];
    now = now_ticks(c);
    hw.target = (uint64_t)next + 1;
    if (hw.target <= now) hw.target = now + 1;
    c->cycles = (int64_t)(hw.target - now);
    return c->r[15] != pc0 || c->cpsr != mode0;
}

/* ---- register access --------------------------------------------------- */
static uint32_t bcd(int v) { return (uint32_t)(((v / 10) << 4) | (v % 10)); }

static uint32_t rtc_read(uint32_t off) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return 0;
    switch (off) {
    case 0x70: return bcd(tm->tm_sec);
    case 0x74: return bcd(tm->tm_min);
    case 0x78: return bcd(tm->tm_hour);
    case 0x7C: return bcd(tm->tm_mday);
    case 0x80: return (uint32_t)tm->tm_wday + 1;
    case 0x84: return bcd(tm->tm_mon + 1);
    case 0x88: return bcd(tm->tm_year % 100);
    default: return 0;
    }
}

static uint32_t reg_read(gp32_cpu *c, uint32_t a) {
    uint32_t off = a & 0xFFFFu, rg = REGION(a);
    switch (a >> 20) {
    case 0x144: return off < 0x18 ? hw.irq[off >> 2] : 0;
    case 0x146: {
        dma_chan *d = &hw.dma[(off >> 5) & 3];
        switch (off & 0x1F) {
        case 0x00: return d->disrc;
        case 0x04: return d->didst;
        case 0x08: return d->dcon;
        case 0x0C: return ((uint32_t)(d->busy ? 1 : 0) << 20) | d->cur_tc;
        case 0x10: return d->cur_src;
        case 0x14: return d->cur_dst;
        default: return d->dmasktrig;
        }
    }
    case 0x148:
        if (off == 0x04) return hw.mpllcon;
        if (off == 0x14) return hw.clkdivn;
        break;
    case 0x14A:
        if (off == 0x00) { uint32_t l, v; lcd_position(c, &l, &v); return (hw.lcd[0] & 0x3FFFFu) | (l << 18); }
        if (off == 0x10) { uint32_t l, v; lcd_position(c, &l, &v); return (hw.lcd[4] & ~0x60000u) | (v << 17); }
        if (off < 0x20) return hw.lcd[off >> 2];
        if (off >= 0x400 && off < 0x800) return hw.pal[(off - 0x400) >> 2];
        break;
    case 0x150:
        if ((off & 0x3FFF) == 0x10) return 6; /* UART: transmitter always idle */
        break;
    case 0x151:
        if (off == 0x00) return hw.tcfg0;
        if (off == 0x04) return hw.tcfg1;
        if (off == 0x08) return hw.tcon;
        if (off >= 0x0C && off < 0x44) {
            unsigned n = (off - 0x0C) / 12, w = (off - 0x0C) % 12;
            if (n == 4) return w == 0 ? hw.tm[4].cntb : timer_count(c, 4);
            return w == 0 ? hw.tm[n].cntb : w == 4 ? hw.tm[n].cmpb : timer_count(c, n);
        }
        break;
    case 0x155:
        if (off == 0x8000) return hw.iiscon | 0x80u; /* transmit FIFO ready */
        if (off == 0x8004) return hw.iismod;
        if (off == 0x8008) return hw.iispsr;
        if (off == 0x800C) return hw.iisfcon;
        break;
    case 0x156:
        if (off < 0x54) {
            unsigned port, w;
            if (off < 8) return off ? hw.gpdat[0] : hw.gpcon[0];
            port = (off - 8) / 12 + 1;
            w = (off - 8) % 12;
            if (w == 0) return hw.gpcon[port];
            if (w == 8) return hw.gpup[port];
            if (port == 1) return gp32_smc_gpio_read(1, (hw.gpdat[1] & 0x00FFu) | ((~hw.buttons & 0xFFu) << 8));
            if (port == 4) return gp32_smc_gpio_read(4, (hw.gpdat[4] & ~0xC0u) | ((~(hw.buttons >> 8) & 3u) << 6));
            if (port == 3) return gp32_smc_gpio_read(3, hw.gpdat[3]);
            return hw.gpdat[port];
        }
        break;
    case 0x157:
        if (off == 0x44) return hw.ticnt;
        if (off >= 0x70 && off <= 0x88) return rtc_read(off);
        break;
    case 0x158:
        if (off == 0x00) return 0x8000u | (hw.io[rg][0] & 0x7FFFu); /* conversion finished */
        if (off == 0x04) return 0x300; /* healthy battery */
        break;
    default: break;
    }
    if (rg < NREGION && (off & 0x3FF) == off) return hw.io[rg][off >> 2];
    return 0;
}

static void reg_write(gp32_cpu *c, uint32_t a, uint32_t v) {
    uint32_t off = a & 0xFFFFu, rg = REGION(a);
    switch (a >> 20) {
    case 0x144:
        if (off == 0x00) hw.irq[SRCPND] &= ~v;
        else if (off == 0x10) hw.irq[INTPND] &= ~v;
        else if (off < 0x10) hw.irq[off >> 2] = v;
        gp32_force_event(c);
        return;
    case 0x146: {
        unsigned n = (off >> 5) & 3;
        dma_chan *d = &hw.dma[n];
        switch (off & 0x1F) {
        case 0x00: d->disrc = v; break;
        case 0x04: d->didst = v; break;
        case 0x08: d->dcon = v; break;
        case 0x18: dma_trigger(c, n, v); break;
        default: break;
        }
        return;
    }
    case 0x148:
        if (off == 0x04) { hw.mpllcon = v; clock_changed(c); return; }
        if (off == 0x14) { hw.clkdivn = v; clock_changed(c); return; }
        break;
    case 0x14A:
        if (off < 0x20) { hw.lcd[off >> 2] = v; return; }
        if (off >= 0x400 && off < 0x800) { hw.pal[(off - 0x400) >> 2] = v & 0xFFFFu; return; }
        break;
    case 0x150:
        if ((off & 0x3FFF) == 0x20) { fputc((int)(v & 0xFF), stdout); fflush(stdout); return; }
        break;
    case 0x151:
        if (off == 0x00) { hw.tcfg0 = v; return; }
        if (off == 0x04) { hw.tcfg1 = v; return; }
        if (off == 0x08) { tcon_write(c, v); return; }
        if (off >= 0x0C && off < 0x44) {
            unsigned n = (off - 0x0C) / 12, w = (off - 0x0C) % 12;
            if (w == 0) hw.tm[n].cntb = v & 0xFFFF;
            else if (w == 4 && n < 4) hw.tm[n].cmpb = v & 0xFFFF;
            return;
        }
        break;
    case 0x155:
        if (off == 0x8000) { hw.iiscon = v; return; }
        if (off == 0x8004) { hw.iismod = v; return; }
        if (off == 0x8008) { hw.iispsr = v; return; }
        if (off == 0x800C) { hw.iisfcon = v; return; }
        if (off == 0x8010) { iis_fifo_write(c, v); return; }
        break;
    case 0x156:
        if (off < 0x54) {
            unsigned port, w;
            if (off < 8) { if (off) hw.gpdat[0] = v; else hw.gpcon[0] = v; return; }
            port = (off - 8) / 12 + 1;
            w = (off - 8) % 12;
            if (w == 0) hw.gpcon[port] = v;
            else if (w == 8) hw.gpup[port] = v;
            else { hw.gpdat[port] = v; gp32_smc_gpio_write(port, v); }
            return;
        }
        break;
    case 0x157:
        if (off == 0x44) {
            hw.ticnt = v & 0xFF;
            hw.ev_rtc = (v & 0x80) ? (double)now_ticks(c) + hw.ips * ((v & 0x7F) + 1) / 128.0 : -1;
            gp32_force_event(c);
            return;
        }
        break;
    default: break;
    }
    if (rg < NREGION && (off & 0x3FF) == off) hw.io[rg][off >> 2] = v;
}

uint32_t gp32_mmio_read(gp32_cpu *c, uint32_t addr, unsigned size) {
    uint32_t v = 0;
    if (addr >= 0x14000000u && addr < 0x14000000u + (NREGION << 20)) v = reg_read(c, addr & ~3u);
    else if (gp32_opt.trace) fprintf(stderr, "gp32: read of unmapped %08X (pc near %08X)\n", addr, c->r[15]);
    if (size < 4) v >>= 8 * (addr & 3);
    return v;
}

void gp32_mmio_write(gp32_cpu *c, uint32_t addr, uint32_t val, unsigned size) {
    (void)size;
    if (addr >= 0x14000000u && addr < 0x14000000u + (NREGION << 20)) reg_write(c, addr & ~3u, val);
    else if (gp32_opt.trace) fprintf(stderr, "gp32: write of %08X to unmapped %08X (pc near %08X)\n", val, addr, c->r[15]);
}

void gp32_hw_reset(gp32_cpu *c) {
    unsigned n;
    memset(&hw, 0, sizeof(hw));
    hw.irq[INTMSK] = 0xFFFFFFFFu;
    hw.mpllcon = 0x3A012; /* 66 MHz, the speed the firmware leaves the CPU at */
    hw.clkdivn = 1;
    clock_changed(c);
    /* 8-bit TFT mode on the firmware's first frame buffer */
    hw.lcd[0] = (5u << 8) | (3u << 5) | (11u << 1) | 1u;
    hw.lcd[1] = (1u << 24) | (319u << 14) | (2u << 6) | 1u;
    hw.lcd[2] = (5u << 19) | (239u << 8) | 3u;
    hw.lcd[3] = 4u;
    hw.lcd[4] = 0;
    hw.lcd[5] = GP32_FB_BASE >> 1;
    for (n = 0; n < 256; n++) /* 3-3-2 colour cube until the program loads its own palette */
        hw.pal[n] = (((n >> 5) & 7) * 31 / 7) << 11 | (((n >> 2) & 7) * 31 / 7) << 6 | ((n & 3) * 31 / 3) << 1;
    for (n = 0; n < 7; n++) hw.gpdat[n] = 0xFFFF;
    for (n = 0; n < 5; n++) hw.tm[n].when = -1;
    for (n = 0; n < 4; n++) hw.ev_hle[n] = -1;
    hw.ev_rtc = -1;
    hw.iispsr = (2u << 5) | 2u;
    lcd_timing();
    hw.ev_ms = hw.ips / 1000.0;
    hw.ev_vsync = hw.frame_ticks;
    hw.target = 1;
    c->cycles = 1;
    hw.real_start_us = plat_time_us();
    hw.key_script = gp32_opt.keys;
}
