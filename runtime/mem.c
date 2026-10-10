/* Memory map: RAM, scratchpad, I/O dispatch. */
#include "psx.h"

CPU cpu;
u8 ram[RAM_SIZE];
u8 scratch[1024];
static u8 bios_rom_stub[16];
static u32 mem_ctrl[16];
static u32 ram_size_reg = 0x00000b88;
static u32 cache_ctrl;

bool g_trace_io = false;

#define IO(p) ((p) - 0x1f801000u)

static u32 io_read(u32 p, int size) {
    u32 off = p - 0x1f801000u;
    if (off < 0x24) { /* memory control 1 */
        return mem_ctrl[off >> 2] >> ((off & 3) * 8);
    }
    if (off >= 0x40 && off < 0x50) return sio_read(off, size);
    if (off >= 0x50 && off < 0x60) return 0;           /* SIO1 */
    if (off == 0x60) return ram_size_reg;
    if (off == 0x70) return i_stat;
    if (off == 0x74) return i_mask;
    if (off >= 0x80 && off < 0x100) return dma_read(off);
    if (off >= 0x100 && off < 0x130) return timers_read(off);
    if (off >= 0x800 && off < 0x804) return cd_read(off - 0x800, size);
    if (off == 0x810) return gpu_read(0);
    if (off == 0x814) return gpu_read(4);
    if (off == 0x820) return mdec_read(0);
    if (off == 0x824) return mdec_read(4);
    if (off >= 0xc00 && off < 0x1000) return spu_read(off - 0xc00, size);
    if (g_trace_io) fprintf(stderr, "io read  %08x size %d (unmapped)\n", p, size);
    return 0;
}

static void io_write(u32 p, u32 v, int size) {
    u32 off = p - 0x1f801000u;
    if (off < 0x24) { mem_ctrl[off >> 2] = v; return; }
    if (off >= 0x40 && off < 0x50) { sio_write(off, v, size); return; }
    if (off >= 0x50 && off < 0x60) return;
    if (off == 0x60) { ram_size_reg = v; return; }
    if (off == 0x70 || off == 0x74) { irq_write(off, v, size); return; }
    if (off == 0x72 || off == 0x76) return;
    if (off >= 0x80 && off < 0x100) { dma_write(off, v, size); return; }
    if (off >= 0x100 && off < 0x130) { timers_write(off, v, size); return; }
    if (off >= 0x800 && off < 0x804) { cd_write(off - 0x800, v); return; }
    if (off == 0x810) { gpu_write(0, v); return; }
    if (off == 0x814) { gpu_write(4, v); return; }
    if (off == 0x820) { mdec_write(0, v); return; }
    if (off == 0x824) { mdec_write(4, v); return; }
    if (off >= 0xc00 && off < 0x1000) { spu_write(off - 0xc00, v, size); return; }
    if (g_trace_io) fprintf(stderr, "io write %08x = %08x size %d (unmapped)\n", p, v, size);
}

u32 mem_read_slow(u32 a, int size) {
    u32 p = a & 0x1fffffffu;
    if (p >= 0x1f800000u && p < 0x1f800400u) {
        u32 o = p - 0x1f800000u;
        if (size == 1) return scratch[o];
        if (size == 2) return *(u16 *)(scratch + (o & ~1u));
        return *(u32 *)(scratch + (o & ~3u));
    }
    if (p >= 0x1f801000u && p < 0x1f803000u) {
        cpu.cycles += 6;               /* I/O reads stall the bus */
        if (p >= 0x1f802000u) return 0;
        return io_read(p, size);
    }
    if (a == 0xfffe0130u) return cache_ctrl;
    if (p >= 0x1fc00000u && p < 0x1fc80000u) {
        /* BIOS ROM: not present in the HLE build */
        return 0;
    }
    if (p >= 0x1f000000u && p < 0x1f800000u) return 0xffffffffu >> (32 - size * 8); /* expansion 1 absent */
    (void)bios_rom_stub;
    return 0;
}

void mem_write_slow(u32 a, u32 v, int size) {
    u32 p = a & 0x1fffffffu;
    if (p >= 0x1f800000u && p < 0x1f800400u) {
        u32 o = p - 0x1f800000u;
        if (size == 1) scratch[o] = (u8)v;
        else if (size == 2) *(u16 *)(scratch + (o & ~1u)) = (u16)v;
        else *(u32 *)(scratch + (o & ~3u)) = v;
        return;
    }
    if (p >= 0x1f801000u && p < 0x1f803000u) {
        if (p >= 0x1f802000u) return;
        io_write(p, v, size);
        return;
    }
    if (a == 0xfffe0130u) { cache_ctrl = v; return; }
}
