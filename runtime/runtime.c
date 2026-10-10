/* Whole-machine init/reset, shared by every frontend. */
#include "psx.h"

jmp_buf g_quit_jmp;

void runtime_reset(Disc *game_disc, bool pal) {
    memset(&cpu, 0, sizeof cpu);
    memset(ram, 0, sizeof ram);
    memset(scratch, 0, sizeof scratch);
    i_stat = i_mask = 0;
    g_next_event = 0;
    g_quit = false;
    g_force_interp = g_force_interp;
    g_game_disc = game_disc;
    g_disc = NULL;
    gpu_set_region_pal(pal);
    gpu_init(); timers_init(); dma_init(); cd_init(); spu_init(); sio_init(); kernel_init();
    g_pad_buttons = 0xffff;
    cd_insert_disc(game_disc);
    sched_update();
}

/* Run the machine until it exits or g_quit is set. Returns when finished. */
void runtime_run(const char *boot_exe) {
    if (setjmp(g_quit_jmp) == 0) kernel_boot(boot_exe);
}
