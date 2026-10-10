/* DMA controller. Transfers are performed immediately at start; the busy bit and completion
 * IRQ are delayed by an approximate bus time so software polling/IRQ logic behaves. */
#include "psx.h"

typedef struct {
    u32 madr, bcr, chcr;
    u64 done_at;      /* cycle when the transfer completes (0 = idle) */
} Chan;

static Chan ch[7];
static u32 dpcr = 0x07654321;
static u32 dicr;

void dma_init(void) {
    memset(ch, 0, sizeof ch);
    dpcr = 0x07654321;
    dicr = 0;
}

static bool master_flag(void) {
    bool force = (dicr >> 15) & 1;
    bool en = (dicr >> 23) & 1;
    bool any = ((dicr >> 16) & 0x7f) & ((dicr >> 24) & 0x7f);
    return force || (en && any);
}

static void dicr_update_irq(bool was) {
    if (!was && master_flag()) irq_raise(IRQ_DMA);
}

static void channel_complete(int n) {
    Chan *c = &ch[n];
    c->chcr &= ~((1u << 24) | (1u << 28));
    c->done_at = 0;
    bool was = master_flag();
    if ((dicr >> (16 + n)) & 1) dicr |= 1u << (24 + n);
    dicr_update_irq(was);
}

u64 dma_next_event(void) {
    u64 best = ~0ull;
    for (int i = 0; i < 7; i++)
        if (ch[i].done_at && ch[i].done_at < best) best = ch[i].done_at;
    return best;
}

void dma_tick(u64 now) {
    for (int i = 0; i < 7; i++)
        if (ch[i].done_at && ch[i].done_at <= now) channel_complete(i);
}

u32 dma_read(u32 off) {
    if (off < 0xf0) {
        int n = (off - 0x80) >> 4;
        Chan *c = &ch[n];
        switch ((off >> 2) & 3) {
        case 0: return c->madr;
        case 1: return c->bcr;
        case 2: return c->chcr;
        }
        return 0;
    }
    if (off == 0xf0) return dpcr;
    if (off == 0xf4) return (dicr & 0x7fffffffu) | (master_flag() ? 0x80000000u : 0);
    return 0;
}

static u32 ram_word(u32 a) { return *(u32 *)(ram + (a & 0x1ffffc)); }
static void ram_wr(u32 a, u32 v) { *(u32 *)(ram + (a & 0x1ffffc)) = v; }

static void start_channel(int n) {
    Chan *c = &ch[n];
    u32 chcr = c->chcr;
    int sync = (chcr >> 9) & 3;
    bool from_ram = chcr & 1;
    int step = (chcr & 2) ? -4 : 4;
    u32 words = 0;
    u32 addr = c->madr & 0x1ffffc;

    if (g_verbose >= 2)
        fprintf(stderr, "[dma] ch%d start madr=%08x bcr=%08x chcr=%08x\n", n, c->madr, c->bcr, c->chcr);
    if (!((dpcr >> (3 + n * 4)) & 1)) {
        /* channel disabled in DPCR: transfer would never run */
        return;
    }

    switch (n) {
    case 6: { /* OTC: build reverse-linked ordering table */
        words = c->bcr & 0xffff;
        if (!words) words = 0x10000;
        u32 a = c->madr & 0x1ffffc;
        for (u32 i = 0; i < words; i++) {
            u32 next = (i == words - 1) ? 0x00ffffffu : ((a - 4) & 0x1ffffc);
            ram_wr(a, next);
            a = (a - 4) & 0x1ffffc;
        }
        break;
    }
    case 2: { /* GPU */
        if (sync == 2) {
            u32 a = c->madr & 0x1ffffc;
            int guard = 0;
            for (;;) {
                u32 hdr = ram_word(a);
                u32 n32 = hdr >> 24;
                if (n32) {
                    u32 tmp[256];
                    for (u32 i = 0; i < n32; i++) tmp[i] = ram_word(a + 4 + i * 4);
                    gpu_dma_write(tmp, n32);
                }
                words += n32 + 1;
                if ((hdr & 0xffffff) == 0xffffff || (hdr & 0x800000)) break;
                a = hdr & 0x1ffffc;
                if (++guard > 0x40000) break;
            }
        } else {
            u32 bs = c->bcr & 0xffff, bc = (c->bcr >> 16) & 0xffff;
            words = (sync == 0) ? (bs ? bs : 0x10000) : bs * bc;
            u32 a = addr;
            if (from_ram) {
                u32 tmp[1024];
                u32 left = words;
                while (left) {
                    u32 k = left > 1024 ? 1024 : left;
                    for (u32 i = 0; i < k; i++) { tmp[i] = ram_word(a); a = (a + step) & 0x1ffffc; }
                    gpu_dma_write(tmp, k);
                    left -= k;
                }
            } else {
                u32 tmp[1024];
                u32 left = words;
                while (left) {
                    u32 k = left > 1024 ? 1024 : left;
                    gpu_dma_read(tmp, k);
                    for (u32 i = 0; i < k; i++) { ram_wr(a, tmp[i]); a = (a + step) & 0x1ffffc; }
                    left -= k;
                }
            }
        }
        break;
    }
    case 3: { /* CD-ROM -> RAM */
        u32 bs = c->bcr & 0xffff, bc = (c->bcr >> 16) & 0xffff;
        words = (sync == 0) ? (bs ? bs : 0x10000) : bs * bc;
        if (!from_ram) cd_dma_read(ram + addr, words * 4);
        break;
    }
    case 4: { /* SPU */
        u32 bs = c->bcr & 0xffff, bc = (c->bcr >> 16) & 0xffff;
        words = (sync == 0) ? (bs ? bs : 0x10000) : bs * bc;
        if (from_ram) spu_dma_write(ram + addr, words * 4);
        else { extern int g_spu_dma_rlog; if (g_spu_dma_rlog > 0) fprintf(stderr, "[dma] SPU->RAM dest %08x bytes %u\n", 0x80000000u + addr, words * 4); spu_dma_read(ram + addr, words * 4); }
        break;
    }
    default: {
        /* MDEC in/out, PIO: nothing attached; complete quickly */
        u32 bs = c->bcr & 0xffff, bc = (c->bcr >> 16) & 0xffff;
        words = (sync == 0) ? (bs ? bs : 0x10000) : bs * bc;
        break;
    }
    }

    /* approximate bus timing */
    u64 per_word = (n == 3) ? 24 : (n == 4) ? 4 : (n == 2 ? 1 : 1);
    c->done_at = cpu.cycles + 32 + (u64)words * per_word;
    if (c->done_at <= cpu.cycles) c->done_at = cpu.cycles + 1;
    sched_update();
}

void dma_write(u32 off, u32 v, int size) {
    (void)size;
    if (off < 0xf0) {
        int n = (off - 0x80) >> 4;
        Chan *c = &ch[n];
        switch ((off >> 2) & 3) {
        case 0: c->madr = v & 0xffffff; break;
        case 1: c->bcr = v; break;
        case 2: {
            if (n == 6) v = (v & 0x51000002u) | 0; /* OTC: only start/trigger/step bits writable */
            else v &= 0x71770703u;
            c->chcr = v;
            int sync = (v >> 9) & 3;
            bool start = (v >> 24) & 1;
            bool trig = (v >> 28) & 1;
            if (start && (sync != 0 || trig)) start_channel(n);
            else if (!start) { c->done_at = 0; }
            break;
        }
        }
        return;
    }
    if (off == 0xf0) { dpcr = v; return; }
    if (off == 0xf4) {
        bool was = master_flag();
        u32 keep = dicr & 0x00ffffffu;
        keep = (keep & ~0x00ff803fu) | (v & 0x00ff803fu);
        u32 flags = (dicr >> 24) & 0x7f;
        flags &= ~((v >> 24) & 0x7f);
        dicr = keep | (flags << 24);
        dicr_update_irq(was);
        return;
    }
}
