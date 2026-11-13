/* main.c - command line and start-up for a recompiled program. */
#include <stdio.h>
#include <stdlib.h>
#include "internal.h"

gp32_options gp32_opt;
const gp32_game *gp32_the_game;

static void usage(const char *exe, const gp32_game *g) {
    printf("%s - GP32 program recompiled with gp32recomp\n\n"
           "usage: %s [options]\n"
           "  --scale N        window size multiplier (default 2)\n"
           "  --smc FILE       SmartMedia card image to insert\n"
           "  --no-audio       do not open a sound device\n"
           "  --cpi X          CPU cycles charged per instruction (default 2.0;\n"
           "                   lower values make the emulated CPU faster)\n"
           "  --interp         run everything in the interpreter (for debugging)\n"
           "  --frames N       stop after N frames\n"
           "  --dump FILE      with --frames: save the last frame as a BMP\n"
           "  --keys SCRIPT    scripted input, \"frame:mask,frame:mask\"\n"
           "  --no-throttle    run as fast as possible\n"
           "  --trace          log firmware calls and odd memory accesses\n\n"
           "keys: arrows = stick, X = A, Z = B, A = L, S = R, Enter = START,\n"
           "      Backspace = SELECT, Esc = quit\n",
           g->name, exe);
}

int gp32_main(int argc, char **argv, const gp32_game *game) {
    static gp32_cpu cpu;
    int i;
    gp32_the_game = game;
    gp32_opt.scale = 2;
    gp32_opt.cpi = 2.0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--scale") && v) { gp32_opt.scale = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { gp32_opt.frames = atoi(v); i++; }
        else if (!strcmp(a, "--dump") && v) { gp32_opt.dump = v; i++; }
        else if (!strcmp(a, "--smc") && v) { gp32_opt.smc = v; i++; }
        else if (!strcmp(a, "--keys") && v) { gp32_opt.keys = v; i++; }
        else if (!strcmp(a, "--cpi") && v) { gp32_opt.cpi = atof(v); i++; }
        else if (!strcmp(a, "--no-throttle")) gp32_opt.no_throttle = 1;
        else if (!strcmp(a, "--no-audio")) gp32_opt.no_audio = 1;
        else if (!strcmp(a, "--interp")) gp32_opt.interp = 1;
        else if (!strcmp(a, "--trace")) gp32_opt.trace = 1;
        else { usage(argv[0], game); return strcmp(a, "--help") ? 2 : 0; }
    }
    if (gp32_opt.cpi < 0.05) gp32_opt.cpi = 0.05;
    if (gp32_opt.scale < 1) gp32_opt.scale = 1;
    cpu.ram = (uint8_t *)malloc(GP32_RAM_SIZE);
    if (!cpu.ram) { fprintf(stderr, "gp32: out of memory\n"); return 1; }
    if (plat_init(game->name, gp32_opt.scale)) { fprintf(stderr, "gp32: cannot open a window\n"); return 1; }
    if (gp32_opt.smc && gp32_smc_open(gp32_opt.smc)) fprintf(stderr, "gp32: cannot open card image %s\n", gp32_opt.smc);
    gp32_cpu_reset(&cpu);
    gp32_hw_reset(&cpu);
    gp32_bios_reset(&cpu);
    gp32_run(&cpu);
    gp32_smc_close();
    plat_shutdown();
    free(cpu.ram);
    return gp32_exit_code();
}
