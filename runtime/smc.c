/* smc.c - SmartMedia card slot (not populated yet). */
#include "internal.h"

int gp32_smc_open(const char *path) { (void)path; return -1; }
void gp32_smc_close(void) {}
void gp32_smc_gpio_write(unsigned port, uint32_t val) { (void)port; (void)val; }
uint32_t gp32_smc_gpio_read(unsigned port, uint32_t latch) { (void)port; return latch; }
