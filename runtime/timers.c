/* Root counters (timers 0..2). Counters are advanced lazily from the CPU cycle counter. */
#include "psx.h"

typedef struct {
    u32 counter;
    u32 mode;
    u32 target;
    u64 last_units;   /* clock-domain units already accounted for */
    bool irq_pulse;   /* for toggle mode bookkeeping */
    bool paused;
    bool one_shot_done;
} Timer;

static Timer tm[3];
static int hblank_state, vblank_state;
static u64 line_cycles_x16;      /* cycles per scanline * 16 */
static u32 dot_div = 10;          /* dot clock divider */

extern u32 gpu_dot_divider(void);
extern u64 gpu_line_cycles_x16(void);

void timers_init(void) {
    memset(tm, 0, sizeof tm);
    line_cycles_x16 = 2157 * 16;
}

static u64 units_for(int t, u64 now) {
    int src = (tm[t].mode >> 8) & 3;
    switch (t) {
    case 0: return (src & 1) ? (now * 11 / 7) / gpu_dot_divider() : now;      /* dotclock = 53.693MHz / div ; 11/7 ratio */
    case 1: return (src & 1) ? (now * 16) / gpu_line_cycles_x16() : now;
    default: return (src & 2) ? now / 8 : now;
    }
}
/* cpu cycles per clock unit (approximate, for scheduling) */
static u64 cycles_per_unit_x16(int t) {
    int src = (tm[t].mode >> 8) & 3;
    switch (t) {
    case 0: return (src & 1) ? (u64)(16 * 7 * gpu_dot_divider() / 11) : 16;
    case 1: return (src & 1) ? gpu_line_cycles_x16() : 16;
    default: return (src & 2) ? 16 * 8 : 16;
    }
}

static void fire_irq(int t) {
    Timer *x = &tm[t];
    bool toggle = (x->mode >> 7) & 1;
    bool repeat = (x->mode >> 6) & 1;
    if (!repeat && x->one_shot_done) return;
    x->one_shot_done = true;
    if (toggle) {
        x->mode ^= 1u << 10;
        if (!((x->mode >> 10) & 1)) irq_raise(IRQ_TMR0 + t);   /* IRQ flag 0 == requested */
    } else {
        x->mode &= ~(1u << 10);
        irq_raise(IRQ_TMR0 + t);
        x->mode |= 1u << 10;                                    /* pulse: bit returns to 1 */
    }
}

static void advance(int t, u64 now) {
    Timer *x = &tm[t];
    u64 u = units_for(t, now);
    if (u < x->last_units) { x->last_units = u; return; }
    u64 du = u - x->last_units;
    x->last_units = u;
    if (x->paused) return;
    const bool reset_target = (x->mode >> 3) & 1;
    const u32 tgt = x->target & 0xffff;
    while (du) {
        u32 c = x->counter;
        u32 stop = (c < tgt) ? tgt : 0xffff;
        u64 steps = stop - c;
        if (du < steps) { x->counter = c + (u32)du; return; }
        du -= steps;
        x->counter = stop;
        if (stop == tgt && steps > 0) {
            x->mode |= 1u << 11;
            if (x->mode & (1u << 4)) fire_irq(t);
        }
        if (stop == tgt && reset_target) {
            if (!du) return;
            du--; x->counter = 0;
            continue;
        }
        if (stop == 0xffff) {
            if (!du) return;
            du--; x->counter = 0;
            x->mode |= 1u << 12;
            if (x->mode & (1u << 5)) fire_irq(t);
        }
    }
}

void timers_tick(u64 now) {
    for (int t = 0; t < 3; t++) advance(t, now);
}

u64 timers_next_event(void) {
    u64 best = ~0ull;
    for (int t = 0; t < 3; t++) {
        Timer *x = &tm[t];
        if (x->paused) continue;
        if (!(x->mode & ((1u << 4) | (1u << 5)))) continue;
        if (!((x->mode >> 6) & 1) && x->one_shot_done) continue;
        u32 c = x->counter, tgt = x->target & 0xffff;
        u64 d = ~0ull;
        if (x->mode & (1u << 4)) {
            u64 dt = (c < tgt) ? (u64)(tgt - c) : (u64)(0x10000 - c) + tgt + 1;
            d = dt;
        }
        if ((x->mode & (1u << 5)) && !(((x->mode >> 3) & 1) && tgt < 0xffff && c <= tgt)) {
            u64 dw = (u64)0x10000 - c;
            if (dw < d) d = dw;
        }
        if (d == ~0ull) continue;
        u64 when = cpu.cycles + (d * cycles_per_unit_x16(t)) / 16 + 2;
        if (when < best) best = when;
    }
    return best;
}

void timers_set_blank(int hblank, int vblank) {
    /* gpu informs about blanking so sync modes can react; only timer 2 stop-modes matter in practice */
    hblank_state = hblank; vblank_state = vblank;
    (void)hblank_state; (void)vblank_state;
}

u32 timers_read(u32 off) {
    int t = (off - 0x100) >> 4;
    int r = (off & 0xf) >> 2;
    if (t > 2) return 0;
    advance(t, cpu.cycles);
    Timer *x = &tm[t];
    switch (r) {
    case 0: return x->counter & 0xffff;
    case 1: {
        u32 m = x->mode;
        x->mode &= ~((1u << 11) | (1u << 12));
        return m;
    }
    case 2: return x->target & 0xffff;
    }
    return 0;
}

void timers_write(u32 off, u32 v, int size) {
    (void)size;
    int t = (off - 0x100) >> 4;
    int r = (off & 0xf) >> 2;
    if (t > 2) return;
    Timer *x = &tm[t];
    advance(t, cpu.cycles);
    switch (r) {
    case 0: x->counter = v & 0xffff; break;
    case 1:
        x->mode = (v & 0x3ff) | (1u << 10);
        x->counter = 0;
        x->one_shot_done = false;
        x->last_units = units_for(t, cpu.cycles);
        /* timer 2 sync modes 0 and 3 halt the counter */
        x->paused = (t == 2) && ((v & 1) && (((v >> 1) & 3) == 0 || ((v >> 1) & 3) == 3));
        if (t == 2 && !(v & 1)) x->paused = false;
        break;
    case 2: x->target = v & 0xffff; break;
    }
    sched_update();
}
