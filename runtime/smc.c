/* smc.c - SmartMedia card slot.
 *
 * The card's NAND lines hang off GPIO ports B, D and E.  No card is modelled
 * yet, so the pins simply read back whatever was last driven: programs see
 * an empty slot and file access fails cleanly.
 */
#include "internal.h"

void gp32_smc_gpio_write(unsigned port, uint32_t val) { (void)port; (void)val; }
uint32_t gp32_smc_gpio_read(unsigned port, uint32_t latch) { (void)port; return latch; }
