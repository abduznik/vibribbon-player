/* Interrupt controller (I_STAT / I_MASK) and the central event scheduler. */
#include "psx.h"

u32 i_stat, i_mask;
u64 g_next_event;
static int in_event;

void irq_raise(int n) {
    i_stat |= 1u << n;
    /* a newly pending interrupt must be looked at on the next safe point */
    if (i_stat & i_mask) g_next_event = 0;
}
u32 irq_read(u32 off) { return off == 0x70 ? i_stat : i_mask; }
void irq_write(u32 off, u32 v, int size) {
    (void)size;
    if (off == 0x70) {
        i_stat &= v;
    } else {
        i_mask = v & 0x7ff;
        if (i_stat & i_mask) g_next_event = 0;
    }
}

static u64 min64(u64 a, u64 b) { return a < b ? a : b; }

void sched_update(void) {
    u64 n = ~0ull;
    n = min64(n, timers_next_event());
    n = min64(n, dma_next_event());
    n = min64(n, cd_next_event());
    n = min64(n, spu_next_event());
    n = min64(n, sio_next_event());
    u64 g = gpu_frame_cycles(); /* unused: gpu ticks via vblank below */
    (void)g;
    extern u64 gpu_next_event(void);
    n = min64(n, gpu_next_event());
    g_next_event = n;
}

extern jmp_buf g_quit_jmp;

void rt_event(void) {
    if (in_event) { sched_update(); return; }
    in_event = 1;
    u64 now = cpu.cycles;
    gpu_tick(now);
    timers_tick(now);
    dma_tick(now);
    cd_tick(now);
    sio_tick(now);
    spu_tick(now);
    in_event = 0;

    if (irq_pending() && (cpu.cop0[12] & SR_IEC) && (cpu.cop0[12] & SR_IM2)) {
        kernel_irq_deliver();
    }
    if (g_quit) longjmp(g_quit_jmp, 1);
    sched_update();
    /* if an interrupt is still deliverable (e.g. raised during the handler), re-check soon */
    if (irq_pending() && (cpu.cop0[12] & SR_IEC) && (cpu.cop0[12] & SR_IM2)) g_next_event = cpu.cycles + 64;
}

void rt_event_hook(void) { rt_event(); }
