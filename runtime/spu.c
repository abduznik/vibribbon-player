/* PS1 SPU: 24 ADPCM voices, ADSR, reverb, CD input, capture buffers, SPU IRQ, DMA. */
#include "psx.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>

#define SPU_RAM_SIZE 0x80000

static u8 spu_ram[SPU_RAM_SIZE];
static u16 regs[0x200];             /* raw register file, 16-bit entries (offset/2) */

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
    u16 vol_l, vol_r, pitch, start, adsr1, adsr2, repeat;
    s32 vl, vr;                  /* resolved fixed volumes */
    int phase;
    s32 level;                   /* envelope 0..0x7fff */
    s32 env_counter;             /* samples until next step */
    u32 cur_addr;                /* address of the current block (byte address) */
    u32 counter;                 /* 4.12 sample position within block */
    s16 samples[28];
    int sample_idx;              /* next sample to consume (0..28) */
    s16 hist[4];
    s16 s1, s2;                  /* ADPCM history (old, older) */
    bool block_loaded;
    u8 flags;
    s32 last_out;                /* last output sample (for pitch modulation) */
    bool repeat_set_explicit;
} Voice;

static Voice v[24];
static u32 spucnt, spustat_irq, endx, kon_pending, koff_pending;
static u32 irq_addr, xfer_addr, xfer_cur;
static u16 mvol_l, mvol_r, cdvol_l, cdvol_r, extvol_l, extvol_r;
static s32 mvol_l_val, mvol_r_val;
static u32 pmon, non, eon;
static u32 reverb_base, reverb_cur;
static u16 rv[32];                   /* reverb config regs 0x1c0..0x1fe */
static u16 vlout, vrout;
static u32 capture_idx;              /* byte index within the 0x200 capture region */
static u64 spu_time;
static u32 noise_level = 1, noise_count;
static int reverb_phase;
static s32 rv_out_l, rv_out_r;
static double acc_voice, acc_cd, acc_rev, acc_out; static u64 acc_n; static double acc_v[24]; static u64 acc_vn[24];

/* ADSR rate tables */
static const s8 filt[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

/* ------------------------------------------------------------ audio output ring */
#define RING_FRAMES (1 << 16)
static s16 ring[RING_FRAMES * 2];
static _Atomic u64 ring_w, ring_r;

size_t spu_ring_fill(void) { return (size_t)(atomic_load(&ring_w) - atomic_load(&ring_r)); }
size_t spu_ring_pull(s16 *dst, size_t frames) {
    u64 r = atomic_load(&ring_r), w = atomic_load(&ring_w);
    size_t avail = (size_t)(w - r);
    size_t n = frames < avail ? frames : avail;
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)((r + i) & (RING_FRAMES - 1));
        dst[i * 2] = ring[k * 2]; dst[i * 2 + 1] = ring[k * 2 + 1];
    }
    atomic_store(&ring_r, r + n);
    return n;
}
void spu_ring_clear(void) { atomic_store(&ring_r, atomic_load(&ring_w)); }
static void ring_push(s16 l, s16 r) {
    u64 w = atomic_load(&ring_w);
    if (w - atomic_load(&ring_r) >= RING_FRAMES) return;   /* overflow: drop */
    size_t k = (size_t)(w & (RING_FRAMES - 1));
    ring[k * 2] = l; ring[k * 2 + 1] = r;
    atomic_store(&ring_w, w + 1);
}

/* ------------------------------------------------------------ helpers */
static void spu_irq_check(u32 byte_addr) {
    if (!(spucnt & 0x40)) return;
    if (((byte_addr >> 3) & 0xfffff) == (irq_addr & 0xffff)) {
        if (g_verbose >= 3 && !spustat_irq) fprintf(stderr, "[spu] IRQ9 raised at %05x\n", byte_addr);
        if (!spustat_irq) { spustat_irq = 1; irq_raise(IRQ_SPU); }
    }
}
static inline u16 ram16(u32 a) { return *(u16 *)(spu_ram + (a & (SPU_RAM_SIZE - 2))); }
static inline void ram16w(u32 a, u16 val) { *(u16 *)(spu_ram + (a & (SPU_RAM_SIZE - 2))) = val; }

static s32 clamp16(s32 x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

void spu_init(void) {
    memset(spu_ram, 0, sizeof spu_ram);
    memset(v, 0, sizeof v);
    memset(regs, 0, sizeof regs);
    spucnt = 0; spustat_irq = 0; endx = 0; irq_addr = 0; xfer_addr = 0; xfer_cur = 0;
    capture_idx = 0; spu_time = cpu.cycles;
    atomic_store(&ring_w, 0); atomic_store(&ring_r, 0);
}

u64 spu_next_event(void) {
    /* With the SPU IRQ armed, wake exactly when the IRQ address will be hit so software that syncs its
     * capture-buffer reads to that IRQ (this game does) sees the same buffer contents as on hardware. */
    if ((spucnt & 0x40) && !spustat_irq) {
        u32 a = (irq_addr & 0xffff) << 3;
        if (a < 0x1000) {
            u32 idx_lo = a & 0x3ff;
            u32 d = ((idx_lo - capture_idx) & 0x3ff) >> 1;      /* samples to generate before the matching write */
            return spu_time + 768ull * (d + 1);
        }
        return spu_time + 768ull * 8;
    }
    return spu_time + 768ull * 64;
}

/* ------------------------------------------------------------ ADPCM */
static void decode_block(Voice *vc) {
    u32 a = vc->cur_addr & (SPU_RAM_SIZE - 1);
    spu_irq_check(a); spu_irq_check(a + 8);
    u8 hdr = spu_ram[a];
    int shift = hdr & 15, filter = (hdr >> 4) & 7;
    if (filter > 4) filter = 4;
    if (shift > 12) shift = 9;
    vc->flags = spu_ram[(a + 1) & (SPU_RAM_SIZE - 1)];
    if (vc->flags & 4) { if (!vc->repeat_set_explicit) vc->repeat = (u16)(a >> 3); vc->repeat_set_explicit = false; }
    s32 s1 = vc->s1, s2 = vc->s2;
    for (int i = 0; i < 28; i++) {
        u8 byte = spu_ram[(a + 2 + i / 2) & (SPU_RAM_SIZE - 1)];
        int nib = (i & 1) ? (byte >> 4) : (byte & 15);
        s32 smp = (s16)(nib << 12) >> shift;
        smp += (s1 * filt[filter][0] + s2 * filt[filter][1] + 32) >> 6;
        smp = clamp16(smp);
        s2 = s1; s1 = smp;
        vc->samples[i] = (s16)smp;
    }
    vc->s1 = (s16)s1; vc->s2 = (s16)s2;
    vc->sample_idx = 0;
    vc->block_loaded = true;
}

int g_spu_keyon_log;
extern int g_gpu_frame_ctr;
static void voice_key_on(int n) {
    Voice *vc = &v[n];
    if (g_spu_keyon_log) fprintf(stderr, "[keyon] frame %d voice %d start %05x pitch %04x\n", g_gpu_frame_ctr, n, (u32)vc->start << 3, (unsigned)vc->pitch);
    vc->cur_addr = (u32)vc->start << 3;
    vc->repeat = vc->start;
    vc->repeat_set_explicit = false;
    vc->counter = 0;
    vc->s1 = vc->s2 = 0;
    memset(vc->hist, 0, sizeof vc->hist);
    vc->phase = PH_ATTACK;
    vc->level = 0;
    vc->env_counter = 0;
    vc->block_loaded = false;
    endx &= ~(1u << n);
    vc->last_out = 0;
}

static void voice_next_sample(Voice *vc, int n) {
    if (!vc->block_loaded) decode_block(vc);
    s16 s = vc->samples[vc->sample_idx++];
    vc->hist[0] = vc->hist[1]; vc->hist[1] = vc->hist[2]; vc->hist[2] = vc->hist[3]; vc->hist[3] = s;
    if (vc->sample_idx >= 28) {
        /* block finished: handle flags */
        u8 fl = vc->flags;
        if (fl & 1) {
            endx |= 1u << n;
            if (fl & 2) vc->cur_addr = (u32)vc->repeat << 3;
            else {
                vc->cur_addr = (u32)vc->repeat << 3;
                vc->phase = PH_OFF; vc->level = 0;
            }
        } else {
            vc->cur_addr = (vc->cur_addr + 16) & (SPU_RAM_SIZE - 1);
        }
        vc->block_loaded = false;
        vc->sample_idx = 0;
    }
}

/* ------------------------------------------------------------ envelope */
static void env_step(Voice *vc, int shift, int step_field, bool decreasing, bool exponential) {
    if (vc->env_counter > 0) { vc->env_counter--; if (vc->env_counter > 0) return; }
    s32 step = decreasing ? (-8 + step_field) : (7 - step_field);
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    if (shift < 11) step <<= (11 - shift);
    if (exponential && !decreasing && vc->level > 0x6000) cycles *= 4;
    if (exponential && decreasing) step = (step * vc->level) >> 15;
    vc->level += step;
    if (vc->level < 0) vc->level = 0;
    if (vc->level > 0x7fff) vc->level = 0x7fff;
    vc->env_counter = cycles;
}

static void env_update(Voice *vc) {
    switch (vc->phase) {
    case PH_ATTACK:
        env_step(vc, (vc->adsr1 >> 10) & 31, (vc->adsr1 >> 8) & 3, false, (vc->adsr1 >> 15) & 1);
        if (vc->level >= 0x7fff) { vc->level = 0x7fff; vc->phase = PH_DECAY; vc->env_counter = 0; }
        break;
    case PH_DECAY: {
        env_step(vc, (vc->adsr1 >> 4) & 15, 0, true, true);
        s32 sl = ((vc->adsr1 & 15) + 1) * 0x800;
        if (vc->level <= sl) { vc->phase = PH_SUSTAIN; vc->env_counter = 0; }
        break;
    }
    case PH_SUSTAIN:
        env_step(vc, (vc->adsr2 >> 8) & 31, (vc->adsr2 >> 6) & 3, (vc->adsr2 >> 14) & 1, (vc->adsr2 >> 15) & 1);
        break;
    case PH_RELEASE:
        env_step(vc, vc->adsr2 & 31, 0, true, (vc->adsr2 >> 5) & 1);
        if (vc->level <= 0) { vc->level = 0; vc->phase = PH_OFF; }
        break;
    default: break;
    }
}

static inline s32 vol_resolve(u16 r) {
    if (r & 0x8000) return (s32)(s16)(r << 1) >> 1 << 1;   /* sweep mode approximated by its target magnitude */
    return (s32)(s16)(r << 1);
}

/* ------------------------------------------------------------ reverb */
static inline s16 rvread(u32 off_bytes) {
    u32 span = SPU_RAM_SIZE - reverb_base;
    u32 a = reverb_base + ((off_bytes + (reverb_cur - reverb_base)) % (span ? span : 1));
    spu_irq_check(a);
    return (s16)ram16(a);
}
static inline void rvwrite(u32 off_bytes, s32 val) {
    /* The master-enable bit only gates writes into the reverb work area; the reverb keeps reading it and
     * producing output regardless. (Vib-Ribbon streams PCM into the work area and uses the reverb as its
     * audio output path with the master enable cleared.) */
    if (!(spucnt & 0x80)) return;
    u32 span = SPU_RAM_SIZE - reverb_base;
    u32 a = reverb_base + ((off_bytes + (reverb_cur - reverb_base)) % (span ? span : 1));
    spu_irq_check(a);
    ram16w(a, (u16)clamp16(val));
}
#define RV(i) ((s16)rv[(i)])
/* register indices into rv[] (offset from 0x1c0 /2) */
enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
       mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
       mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };

static void reverb_run(s32 lin, s32 rin) {
    #define OFF(i) ((u32)rv[(i)] * 8)
    s32 vl = RV(vLIN), vr = RV(vRIN);
    s32 L = (lin * vl) >> 15, Rr = (rin * vr) >> 15;
    s32 wall = RV(vWALL), iir = RV(vIIR);
    /* same side reflection */
    s32 t;
    t = (L + ((rvread(OFF(dLSAME)) * wall) >> 15) - rvread(OFF(mLSAME) - 2)) ; t = ((t * iir) >> 15) + rvread(OFF(mLSAME) - 2);
    rvwrite(OFF(mLSAME), t);
    t = (Rr + ((rvread(OFF(dRSAME)) * wall) >> 15) - rvread(OFF(mRSAME) - 2)); t = ((t * iir) >> 15) + rvread(OFF(mRSAME) - 2);
    rvwrite(OFF(mRSAME), t);
    /* different side reflection */
    t = (L + ((rvread(OFF(dRDIFF)) * wall) >> 15) - rvread(OFF(mLDIFF) - 2)); t = ((t * iir) >> 15) + rvread(OFF(mLDIFF) - 2);
    rvwrite(OFF(mLDIFF), t);
    t = (Rr + ((rvread(OFF(dLDIFF)) * wall) >> 15) - rvread(OFF(mRDIFF) - 2)); t = ((t * iir) >> 15) + rvread(OFF(mRDIFF) - 2);
    rvwrite(OFF(mRDIFF), t);
    /* early echo */
    s32 Lo = (RV(vCOMB1) * rvread(OFF(mLCOMB1)) + RV(vCOMB2) * rvread(OFF(mLCOMB2)) +
              RV(vCOMB3) * rvread(OFF(mLCOMB3)) + RV(vCOMB4) * rvread(OFF(mLCOMB4))) >> 15;
    s32 Ro = (RV(vCOMB1) * rvread(OFF(mRCOMB1)) + RV(vCOMB2) * rvread(OFF(mRCOMB2)) +
              RV(vCOMB3) * rvread(OFF(mRCOMB3)) + RV(vCOMB4) * rvread(OFF(mRCOMB4))) >> 15;
    /* late reverb: two all-pass filters */
    s32 apf1 = RV(vAPF1), apf2 = RV(vAPF2);
    Lo = Lo - ((apf1 * rvread(OFF(mLAPF1) - OFF(dAPF1))) >> 15); rvwrite(OFF(mLAPF1), Lo);
    Lo = ((Lo * apf1) >> 15) + rvread(OFF(mLAPF1) - OFF(dAPF1));
    Ro = Ro - ((apf1 * rvread(OFF(mRAPF1) - OFF(dAPF1))) >> 15); rvwrite(OFF(mRAPF1), Ro);
    Ro = ((Ro * apf1) >> 15) + rvread(OFF(mRAPF1) - OFF(dAPF1));
    Lo = Lo - ((apf2 * rvread(OFF(mLAPF2) - OFF(dAPF2))) >> 15); rvwrite(OFF(mLAPF2), Lo);
    Lo = ((Lo * apf2) >> 15) + rvread(OFF(mLAPF2) - OFF(dAPF2));
    Ro = Ro - ((apf2 * rvread(OFF(mRAPF2) - OFF(dAPF2))) >> 15); rvwrite(OFF(mRAPF2), Ro);
    Ro = ((Ro * apf2) >> 15) + rvread(OFF(mRAPF2) - OFF(dAPF2));
    rv_out_l = clamp16(Lo); rv_out_r = clamp16(Ro);
    /* advance */
    reverb_cur += 2;
    if (reverb_cur >= SPU_RAM_SIZE) reverb_cur = reverb_base;
    #undef OFF
}

/* ------------------------------------------------------------ sample generation */
static s32 voice_interp(const Voice *vc) {
    /* cubic (Catmull-Rom) between hist[1] and hist[2] */
    float t = (float)((vc->counter >> 4) & 0xff) / 256.0f;
    float p0 = vc->hist[0], p1 = vc->hist[1], p2 = vc->hist[2], p3 = vc->hist[3];
    float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    float c = -0.5f * p0 + 0.5f * p2;
    float r = ((a * t + b) * t + c) * t + p1;
    return clamp16((s32)r);
}

static void generate_sample(void) {
    s32 mix_l = 0, mix_r = 0, rev_in_l = 0, rev_in_r = 0;
    s32 cap_v1 = 0, cap_v3 = 0;
    bool enabled = spucnt & 0x8000;

    /* CD audio */
    s16 cdl, cdr;
    cd_get_cdda(&cdl, &cdr);
    /* capture buffers (CD L/R, voice 1, voice 3) -- written continuously */
    {
        static float cap_scale = -1;
        if (cap_scale < 0) { const char *e = getenv("VIBREG_CAPSCALE"); cap_scale = e ? (float)atof(e) : 1.0f; }
        if (cap_scale != 1.0f) { cdl = (s16)(cdl * cap_scale); cdr = (s16)(cdr * cap_scale); }
    }
    /* capture buffers: 1 KB each - CD left @000, CD right @400, voice 1 @800, voice 3 @C00 */
    ram16w(0x000 + capture_idx, (u16)cdl);
    ram16w(0x400 + capture_idx, (u16)cdr);
    spu_irq_check(0x000 + capture_idx);
    spu_irq_check(0x400 + capture_idx);

    if (enabled) {
        for (int n = 0; n < 24; n++) {
            Voice *vc = &v[n];
            if (vc->phase == PH_OFF) { vc->last_out = 0; continue; }
            env_update(vc);
            if (vc->phase == PH_OFF) { vc->last_out = 0; continue; }
            s32 smp;
            if (non & (1u << n)) smp = (s16)noise_level;
            else smp = voice_interp(vc);
            s32 out = (smp * vc->level) >> 15;
            vc->last_out = out;
            acc_v[n] += (double)out * out; acc_vn[n]++;
            if (n == 1) cap_v1 = out;
            if (n == 3) cap_v3 = out;
            s32 l = (out * vol_resolve(vc->vol_l)) >> 15;
            s32 r = (out * vol_resolve(vc->vol_r)) >> 15;
            mix_l += l; mix_r += r;
            if (eon & (1u << n)) { rev_in_l += l; rev_in_r += r; }
            /* advance */
            u32 step = vc->pitch;
            if (pmon & (1u << n) && n > 0) {
                s32 factor = v[n - 1].last_out + 0x8000;
                step = (u32)(((s32)step * factor) >> 15);
            }
            if (step > 0x3fff) step = 0x3fff;
            vc->counter += step;
            while (vc->counter >= 0x1000) {
                vc->counter -= 0x1000;
                voice_next_sample(vc, n);
                if (vc->phase == PH_OFF) break;
            }
        }
        /* noise generator */
        {
            int shift = (spucnt >> 10) & 15, step = (spucnt >> 8) & 3;
            noise_count += 4 + step;
            u32 thr = 0x20000u >> shift;
            if (noise_count >= thr) {
                noise_count -= thr;
                u32 parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
                noise_level = ((noise_level << 1) | parity) & 0xffff;
            }
        }
    }
    ram16w(0x800 + capture_idx, (u16)cap_v1);
    ram16w(0xc00 + capture_idx, (u16)cap_v3);
    spu_irq_check(0x800 + capture_idx);
    spu_irq_check(0xc00 + capture_idx);
    capture_idx = (capture_idx + 2) & 0x3ff;

    /* CD into the mix */
    if (spucnt & 1) {
        s32 l = (cdl * (s32)(s16)cdvol_l) >> 15, r = (cdr * (s32)(s16)cdvol_r) >> 15;
        mix_l += l; mix_r += r;
        if (spucnt & 4) { rev_in_l += l; rev_in_r += r; }
    }

    /* reverb at 22.05kHz (every other sample) */
    reverb_phase ^= 1;
    if (reverb_phase) reverb_run(clamp16(rev_in_l), clamp16(rev_in_r));
    mix_l += (rv_out_l * (s32)(s16)vlout) >> 15;
    mix_r += (rv_out_r * (s32)(s16)vrout) >> 15;

    acc_n++;
    acc_voice += (double)mix_l * mix_l; acc_out += 0;
    s32 ol = 0, orr = 0;
    if (enabled && (spucnt & 0x4000)) {
        ol = clamp16((clamp16(mix_l) * mvol_l_val) >> 15);
        orr = clamp16((clamp16(mix_r) * mvol_r_val) >> 15);
    }
    acc_out += (double)ol * ol;
    acc_rev += (double)rv_out_l * rv_out_l;
    acc_cd += (double)cdl * cdl;
    ring_push((s16)ol, (s16)orr);
}

void spu_tick(u64 now) {
    while (spu_time + 768 <= now) {
        generate_sample();
        spu_time += 768;
    }
}

/* ------------------------------------------------------------ transfers */
static void xfer_write16(u16 val) {
    spu_irq_check(xfer_cur);
    ram16w(xfer_cur, val);
    xfer_cur = (xfer_cur + 2) & (SPU_RAM_SIZE - 1);
}
int g_spu_dma_log;
void spu_dma_write(const u8 *src, u32 bytes) {
    spu_tick(cpu.cycles);
    bool nz = false;
    for (u32 i = 0; i < bytes && i < 1024; i++) if (src[i]) { nz = true; break; }
    if (g_spu_dma_log > 0 && bytes <= 1024 && nz) {
        g_spu_dma_log--;
        fprintf(stderr, "[spu] dma write %u bytes -> %05x :", bytes, xfer_cur);
        for (u32 i = 0; i < 32 && i < bytes; i++) fprintf(stderr, " %02x", src[i]);
        fprintf(stderr, "\n");
    }
    for (u32 i = 0; i + 1 < bytes + 1 && i < bytes; i += 2) xfer_write16(*(const u16 *)(src + i));
}
int g_spu_dma_rlog;
void spu_dma_read(u8 *dst, u32 bytes) {
    spu_tick(cpu.cycles);
    if (g_spu_dma_rlog > 0) { g_spu_dma_rlog--; fprintf(stderr, "[spu] dma READ %u bytes from %05x (capture_idx=%03x, cyc %llu)\n", bytes, xfer_cur, capture_idx, (unsigned long long)cpu.cycles); }
    for (u32 i = 0; i < bytes; i += 2) {
        spu_irq_check(xfer_cur);
        *(u16 *)(dst + i) = ram16(xfer_cur);
        xfer_cur = (xfer_cur + 2) & (SPU_RAM_SIZE - 1);
    }
}
u32 spu_dma_addr_after(void) { return xfer_cur; }

/* ------------------------------------------------------------ registers */
static u32 spustat(void) {
    u32 s = spucnt & 0x3f;
    if (spustat_irq) s |= 0x40;
    u32 mode = (spucnt >> 4) & 3;
    if (mode == 2 || mode == 3) s |= 0x80;           /* DMA request */
    if (mode == 2) s |= 0x100;
    if (mode == 3) s |= 0x200;
    if (capture_idx >= 0x200) s |= 0x800;       /* capture buffers: writing to the second half */
    return s;
}

u32 spu_read(u32 off, int size) {
    spu_tick(cpu.cycles);
    u32 idx = (off & ~1u) >> 1;
    u32 val = 0;
    if (off < 0x180) {
        int n = off >> 4;
        switch ((off & 15) >> 1) {
        case 0: val = v[n].vol_l; break;
        case 1: val = v[n].vol_r; break;
        case 2: val = v[n].pitch; break;
        case 3: val = v[n].start; break;
        case 4: val = v[n].adsr1; break;
        case 5: val = v[n].adsr2; break;
        case 6: val = (u32)v[n].level; break;
        case 7: val = v[n].repeat; break;
        }
    } else switch (off & ~1u) {
        case 0x180: val = mvol_l; break;
        case 0x182: val = mvol_r; break;
        case 0x184: val = vlout; break;
        case 0x186: val = vrout; break;
        case 0x188: val = 0; break;
        case 0x18a: val = 0; break;
        case 0x190: val = pmon & 0xffff; break;
        case 0x192: val = pmon >> 16; break;
        case 0x194: val = non & 0xffff; break;
        case 0x196: val = non >> 16; break;
        case 0x198: val = eon & 0xffff; break;
        case 0x19a: val = eon >> 16; break;
        case 0x19c: val = endx & 0xffff; break;
        case 0x19e: val = (endx >> 16) & 0xff; break;
        case 0x1a2: val = reverb_base >> 3; break;
        case 0x1a4: val = irq_addr; break;
        case 0x1a6: val = (xfer_addr >> 3) & 0xffff; break;
        case 0x1a8: val = 0; break;
        case 0x1aa: val = spucnt; break;
        case 0x1ac: val = 4; break;
        case 0x1ae: val = spustat(); break;
        case 0x1b0: val = cdvol_l; break;
        case 0x1b2: val = cdvol_r; break;
        case 0x1b4: val = extvol_l; break;
        case 0x1b6: val = extvol_r; break;
        case 0x1b8: val = (u32)mvol_l_val >> 1 & 0xffff; break;
        case 0x1ba: val = (u32)mvol_r_val >> 1 & 0xffff; break;
        default:
            if (off >= 0x1c0 && off < 0x200) val = rv[(off - 0x1c0) >> 1];
            else if (off >= 0x200 && off < 0x260) { int n = (off - 0x200) >> 2; val = (off & 2) ? (u32)(v[n].last_out) & 0xffff : (u32)(v[n].last_out) & 0xffff; }
            else val = regs[idx & 0x1ff];
            break;
    }
    if (size == 4) {
        /* 32-bit read: combine two consecutive halfwords */
        u32 hi = spu_read(off + 2, 2);
        return (val & 0xffff) | (hi << 16);
    }
    if (size == 1 && (off & 1)) return (val >> 8) & 0xff;
    return val & 0xffff;
}

static void write16(u32 off, u32 val) {
    val &= 0xffff;
    if (g_verbose >= 3 && (off == 0x1a4 || off == 0x1aa || off == 0x1a6 || off == 0x188 || off == 0x18a || off == 0x18c || off == 0x18e || off == 0x1a2 || off == 0x1b0 || off == 0x1b2 || off == 0x180 || off == 0x182 || off == 0x198 || off == 0x19a))
        fprintf(stderr, "[spu] w %03x = %04x (cyc %llu)\n", off, val, (unsigned long long)cpu.cycles);
    regs[(off >> 1) & 0x1ff] = (u16)val;
    if (off < 0x180) {
        int n = off >> 4;
        Voice *vc = &v[n];
        switch ((off & 15) >> 1) {
        case 0: vc->vol_l = (u16)val; break;
        case 1: vc->vol_r = (u16)val; break;
        case 2: vc->pitch = (u16)val; break;
        case 3: vc->start = (u16)val; break;
        case 4: vc->adsr1 = (u16)val; break;
        case 5: vc->adsr2 = (u16)val; break;
        case 6: break;
        case 7: vc->repeat = (u16)val; vc->repeat_set_explicit = true; break;
        }
        return;
    }
    switch (off) {
    case 0x180: mvol_l = (u16)val; mvol_l_val = vol_resolve((u16)val); break;
    case 0x182: mvol_r = (u16)val; mvol_r_val = vol_resolve((u16)val); break;
    case 0x184: vlout = (u16)val; break;
    case 0x186: vrout = (u16)val; break;
    case 0x188: case 0x18a: {
        u32 bits = val << ((off == 0x18a) ? 16 : 0);
        for (int n = 0; n < 24; n++) if (bits & (1u << n)) voice_key_on(n);
        break;
    }
    case 0x18c: case 0x18e: {
        u32 bits = val << ((off == 0x18e) ? 16 : 0);
        for (int n = 0; n < 24; n++) if ((bits & (1u << n)) && v[n].phase != PH_OFF) { v[n].phase = PH_RELEASE; v[n].env_counter = 0; }
        break;
    }
    case 0x190: pmon = (pmon & 0xffff0000u) | val; break;
    case 0x192: pmon = (pmon & 0xffffu) | (val << 16); break;
    case 0x194: non = (non & 0xffff0000u) | val; break;
    case 0x196: non = (non & 0xffffu) | (val << 16); break;
    case 0x198: eon = (eon & 0xffff0000u) | val; break;
    case 0x19a: eon = (eon & 0xffffu) | (val << 16); break;
    case 0x1a2: reverb_base = (val << 3) & (SPU_RAM_SIZE - 1); reverb_cur = reverb_base; break;
    case 0x1a4: irq_addr = val; break;
    case 0x1a6: xfer_addr = (val << 3) & (SPU_RAM_SIZE - 1); xfer_cur = xfer_addr; break;
    case 0x1a8: xfer_write16((u16)val); break;
    case 0x1aa:
        spucnt = val;
        if (!(val & 0x40)) spustat_irq = 0;
        break;
    case 0x1b0: cdvol_l = (u16)val; break;
    case 0x1b2: cdvol_r = (u16)val; break;
    case 0x1b4: extvol_l = (u16)val; break;
    case 0x1b6: extvol_r = (u16)val; break;
    default:
        if (off >= 0x1c0 && off < 0x200) rv[(off - 0x1c0) >> 1] = (u16)val;
        break;
    }
}

void spu_write(u32 off, u32 val, int size) {
    spu_tick(cpu.cycles);
    if (size == 4) { write16(off & ~1u, val & 0xffff); write16((off & ~1u) + 2, val >> 16); return; }
    if (size == 1) {
        u32 cur = regs[(off >> 1) & 0x1ff];
        if (off & 1) val = (cur & 0x00ff) | (val << 8); else val = (cur & 0xff00) | (val & 0xff);
        write16(off & ~1u, val);
        return;
    }
    write16(off & ~1u, val);
}

void spu_dump_state(void) {
    fprintf(stderr, "[spu] reverb vLOUT=%04x vROUT=%04x mBASE=%05x regs:", vlout, vrout, reverb_base);
    for (int i = 0; i < 32; i++) fprintf(stderr, " %04x", rv[i]);
    fprintf(stderr, "\n");
    if (acc_n) fprintf(stderr, "[spu] last-window rms: mix(pre-master)=%.0f cd_in=%.0f reverb=%.0f out=%.0f over %llu samples\n", sqrt(acc_voice / acc_n), sqrt(acc_cd / acc_n), sqrt(acc_rev / acc_n), sqrt(acc_out / acc_n), (unsigned long long)acc_n);
    for (int n = 0; n < 24; n++) if (acc_vn[n]) { fprintf(stderr, "[spu]   voice %02d active %llu samples, rms(out)=%.0f pitch=%04x start=%04x vol=%04x/%04x\n", n, (unsigned long long)acc_vn[n], sqrt(acc_v[n] / acc_vn[n]), v[n].pitch, v[n].start, v[n].vol_l, v[n].vol_r); acc_v[n] = 0; acc_vn[n] = 0; }
    acc_voice = acc_cd = acc_rev = acc_out = 0; acc_n = 0;
    for (int b = 0; b < 4; b++) {
        double e = 0; int mx = 0;
        for (int i = 0; i < 512; i++) { int v16 = (s16)ram16(b * 0x400 + i * 2); e += (double)v16 * v16; if (abs(v16) > mx) mx = abs(v16); }
        fprintf(stderr, "[spu] capture buf %d (@%03x): rms=%.0f max=%d\n", b, b * 0x400, sqrt(e / 512), mx);
    }
    fprintf(stderr, "[spu] SPUCNT=%04x mvol=%04x/%04x cdvol=%04x/%04x extvol=%04x/%04x reverb_base=%05x irq_addr=%04x endx=%06x non=%06x eon=%06x pmon=%06x\n",
            spucnt, mvol_l, mvol_r, cdvol_l, cdvol_r, extvol_l, extvol_r, reverb_base, irq_addr, endx, non, eon, pmon);
    for (int n = 0; n < 24; n++) {
        Voice *vc = &v[n];
        if (vc->phase == PH_OFF && !vc->pitch) continue;
        fprintf(stderr, "[spu]  v%02d ph=%d lvl=%04x pitch=%04x start=%04x rep=%04x cur=%05x vol=%04x/%04x adsr=%04x/%04x\n", n, vc->phase, vc->level, vc->pitch, vc->start, vc->repeat, vc->cur_addr, vc->vol_l, vc->vol_r, vc->adsr1, vc->adsr2);
    }
}
