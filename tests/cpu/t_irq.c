/* Timer interrupts through the firmware's handler table, and the tick counter. */
#include "test.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define TCFG0 REG(0x15100000)
#define TCFG1 REG(0x15100004)
#define TCON REG(0x15100008)
#define TCNTB4 REG(0x1510003C)
#define TCNTO4 REG(0x15100040)
#define LCDCON1 REG(0x14A00000)

volatile uint32_t *bios_tick_ptr(void);
void bios_install_irq(unsigned n, void (*handler)(void));
void irq_enable(void);

static volatile uint32_t ticks;
static volatile uint32_t in_irq_mode;

__attribute__((interrupt("IRQ"), target("arm"))) static void timer4(void) {
    uint32_t cpsr;
    __asm__("mrs %0, cpsr" : "=r"(cpsr));
    in_irq_mode += (cpsr & 0x1F) == 0x12;
    ticks++;
}

int main(void) {
    uint32_t spin = 0, h = 0, t0, t1;
    volatile uint32_t *tick = bios_tick_ptr();
    /* PCLK / 256 / 16, divided by 81: about 100 interrupts a second */
    TCFG0 |= 0xFF << 8;
    TCFG1 = (TCFG1 & ~(15u << 16)) | (3u << 16);
    TCNTB4 = 80;
    TCON = (1u << 21);
    bios_install_irq(14, timer4);
    TCON = (1u << 22) | (1u << 20);
    irq_enable();
    t0 = *tick;
    while (ticks < 50) { spin++; h = h * 3 + spin; }
    t1 = *tick;
    TCON = 0;
    put_hex("ticks", ticks);
    put_hex("irq.mode", in_irq_mode == ticks);
    put_hex("work.done", spin > 1000 && h != 0);
    put_hex("elapsed.ok", t1 - t0 >= 480 && t1 - t0 <= 520); /* 50 periods of 10 ms */
    /* the LCD line counter must sweep through its whole range */
    {
        uint32_t seen_lo = 0, seen_hi = 0, n;
        for (n = 0; n < 4000000 && !(seen_lo && seen_hi); n++) {
            uint32_t line = LCDCON1 >> 18;
            if (line == 1) seen_lo = 1;
            if (line == 319) seen_hi = 1;
        }
        put_hex("lcd.sweep", seen_lo && seen_hi);
    }
    return 0;
}
