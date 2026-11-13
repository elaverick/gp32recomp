/* internal.h - shared between the runtime's own translation units. */
#ifndef GP32_INTERNAL_H
#define GP32_INTERNAL_H

#include "gp32rt.h"

#define GP32_LCD_W 320
#define GP32_LCD_H 240

/* Button bits, in the order the hardware reports them. */
enum {
    GP32_KEY_LEFT = 1 << 0, GP32_KEY_DOWN = 1 << 1, GP32_KEY_RIGHT = 1 << 2, GP32_KEY_UP = 1 << 3,
    GP32_KEY_L = 1 << 4, GP32_KEY_B = 1 << 5, GP32_KEY_A = 1 << 6, GP32_KEY_R = 1 << 7,
    GP32_KEY_START = 1 << 8, GP32_KEY_SELECT = 1 << 9
};

/* Memory the firmware reserves at the top of RAM (addresses match the real BIOS). */
#define GP32_SYS_BASE 0x0C780000u
#define GP32_IRQ_TABLE 0x0C7AC000u
#define GP32_IRQ_STACK 0x0C7AE800u
#define GP32_SVC_STACK 0x0C7AFF00u
#define GP32_CLK_INFO 0x0C7B0000u /* FCLK, HCLK, PCLK */
#define GP32_TICK_ADDR 0x0C7B0020u
#define GP32_PAL_SHADOW 0x0C7B0400u /* 256 words: logical palette */
#define GP32_ARG_AREA 0x0C7B0C00u
#define GP32_FB_BASE 0x0C7B4000u

/* Addresses in the (absent) boot ROM that stand for native BIOS routines. */
#define GP32_BIOS_STUB_BASE 0x00000100u
#define GP32_BIOS_STUB_COUNT 32u

typedef struct gp32_options {
    int scale;
    int frames;          /* stop after this many frames (0 = run forever) */
    const char *dump;    /* write the last frame here as a BMP */
    const char *smc;     /* SmartMedia image */
    const char *wav;     /* write everything played to this WAV file */
    const char *keys;    /* scripted input: "frame:mask,frame:mask" */
    int no_throttle;
    int no_audio;
    int interp;          /* ignore recompiled code */
    int trace;
    double cpi;          /* cycles charged per instruction */
} gp32_options;

extern gp32_options gp32_opt;
extern const gp32_game *gp32_the_game;

/* cpu.c */
void gp32_cpu_reset(gp32_cpu *c);
void gp32_run(gp32_cpu *c);
void gp32_quit(gp32_cpu *c, int code);
void gp32_fatal(gp32_cpu *c, const char *fmt, ...);
void gp32_enter_irq(gp32_cpu *c, uint32_t handler);
void gp32_force_event(gp32_cpu *c);
void gp32_code_overwritten(gp32_cpu *c);
int gp32_exit_code(void);

/* hw.c */
void gp32_hw_reset(gp32_cpu *c);
void gp32_hw_shutdown(void);
void gp32_irq_request(gp32_cpu *c, unsigned n);
uint32_t gp32_hw_fclk(void);
uint32_t gp32_hw_hclk(void);
uint32_t gp32_hw_pclk(void);
void gp32_hw_set_clock(gp32_cpu *c, uint32_t mpllcon, uint32_t clkdivn);
void gp32_lcd_set(gp32_cpu *c, uint32_t fb, int bpp);
uint32_t gp32_lcd_fb(void);
int gp32_lcd_bpp(void);
uint32_t *gp32_lcd_palette(void);
uint32_t gp32_buttons(void);
uint32_t gp32_ms(void);
void gp32_wait_vsync(gp32_cpu *c);
void gp32_hle_timer(gp32_cpu *c, unsigned idx, uint32_t hz);

/* bios.c */
void gp32_bios_reset(gp32_cpu *c);
void gp32_bios_call(gp32_cpu *c);
void gp32_bios_ms_tick(gp32_cpu *c);

/* smc.c */
int gp32_smc_open(const char *path);
void gp32_smc_close(void);
void gp32_smc_gpio_write(unsigned port, uint32_t val);
uint32_t gp32_smc_gpio_read(unsigned port, uint32_t latch);

/* platform_*.c */
int plat_init(const char *title, int scale);
void plat_shutdown(void);
int plat_present(const uint32_t *xrgb); /* returns non-zero when the user asked to quit */
uint32_t plat_buttons(void);
void plat_audio(const int16_t *stereo, int frames, int rate);
uint64_t plat_time_us(void);
void plat_sleep_us(uint64_t us);

#endif
