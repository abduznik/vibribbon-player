/* Glue between recompiled code, the interpreter and the kernel. */
#include "psx.h"
#include <stdlib.h>

#define GUEST_STOP 0x1ffffff0u

bool g_force_interp = false;
bool g_sync_mode = false;
bool g_quit = false;
int g_verbose = 0;
const RecompOverlay *g_cur_overlay;

#ifdef VIBRT_NO_OVERLAYS
/* the library carries no recompiled game code */
const RecompOverlay *const g_overlays[] = {0};
const int g_overlay_count = 0;
#else
__attribute__((weak)) const RecompOverlay *const g_overlays[] = {0};
__attribute__((weak)) const int g_overlay_count = 0;
#endif

/* O(1) lookup: one slot per text word, built when an overlay is selected */
static RecompFn *g_map;
static u32 g_map_base, g_map_words;
static const RecompOverlay *g_map_for;

u32 g_code_lo, g_code_len;
bool g_slow, g_code_dirty;
static const u32 *g_cover_bits;
static u8 *g_learn_bits;                 /* words changed inside the text range (learn mode) */
static const char *g_learn_path;
static u32 g_learn_lo, g_learn_len;

static void guard_set(u32 lo, u32 len, const u32 *bits) {
    g_code_lo = lo; g_code_len = len; g_cover_bits = bits;
}

void recomp_select(const RecompOverlay *o) {
    g_cur_overlay = o;
    if (!o) { if (!g_learn_bits) guard_set(0, 0, NULL); return; }
    guard_set(o->cover_lo & 0x1fffffu, o->cover_len, o->cover_bits);
    if (o == g_map_for) return;
    free(g_map);
    g_map_words = o->text_size >> 2;
    g_map_base = o->text_addr & 0x1fffffffu;
    g_map = calloc(g_map_words ? g_map_words : 1, sizeof *g_map);
    for (u32 i = 0; i < o->count; i++) {
        u32 w = ((o->table[i].addr & 0x1fffffffu) - g_map_base) >> 2;
        if (w < g_map_words) g_map[w] = o->table[i].fn;
    }
    g_map_for = o;
}

/* A store hit the recompiled-code range. Learn mode records changed words; otherwise a change to a word that
 * belongs to a recompiled function means the generated C is stale: fall back to the interpreter from now on. */
void code_store(u32 p, u32 v, int size) {
    u32 w = p & ~3u, old = *(u32 *)(ram + w);
    u32 nw = old;
    if (size == 4) nw = v;
    else if (size == 2) { u32 sh = (p & 2) * 8; nw = (old & ~(0xffffu << sh)) | ((v & 0xffffu) << sh); }
    else { u32 sh = (p & 3) * 8; nw = (old & ~(0xffu << sh)) | ((v & 0xffu) << sh); }
    if (nw == old) return;
    if (g_learn_bits && w - g_learn_lo < g_learn_len) { u32 wi = w >> 2; g_learn_bits[wi >> 3] |= (u8)(1u << (wi & 7)); }
    if (g_cover_bits && w >= g_code_lo) {
        u32 i = (w - g_code_lo) >> 2;
        if (i < (g_code_len >> 2) && (g_cover_bits[i >> 5] >> (i & 31) & 1) && !g_code_dirty) {
            WARN("[recomp] recompiled code at %08x was modified at run time (%08x -> %08x): falling back to the interpreter\n",
                 0x80000000u + w, old, nw);
            g_code_dirty = true;
            g_force_interp = true;
            g_slow = true;
            g_next_event = 0;
        }
    }
}

void learn_writes_start(const char *path) {
    g_learn_path = path;
    g_learn_bits = calloc(RAM_SIZE / 4 / 8 + 1, 1);
}
/* select_overlay_for (kernel.c) calls this with the text range when learn mode is on */
void learn_set_range(u32 lo, u32 len) {
    if (!g_learn_bits) return;
    g_learn_lo = lo & 0x1fffffu; g_learn_len = len;
    if (!g_cover_bits) guard_set(g_learn_lo, g_learn_len, NULL);
}
void learn_writes_dump(void) {
    if (!g_learn_bits || !g_learn_path) return;
    FILE *f = fopen(g_learn_path, "a");
    if (!f) return;
    for (u32 w = 0; w < RAM_SIZE / 4; w++) if (g_learn_bits[w >> 3] & (1u << (w & 7))) fprintf(f, "%08x\n", 0x80000000u + w * 4);
    fclose(f);
}

/* the recompiled function starting exactly at addr (ignores g_force_interp: used for sync-mode safe points) */
RecompFn recomp_entry(u32 addr) {
    u32 w = ((addr & 0x1fffffffu) - g_map_base) >> 2;
    return (g_cur_overlay && w < g_map_words && !(addr & 3)) ? g_map[w] : NULL;
}

RecompFn recomp_lookup(u32 addr) {
    return g_force_interp ? NULL : recomp_entry(addr);
}

void guest_call(u32 addr) {
    u32 saved_ra = cpu.r[31];
    cpu.r[31] = GUEST_STOP;
    RecompFn f = recomp_lookup(addr);
    if (f) f(); else interp_run(addr, GUEST_STOP);
    cpu.r[31] = saved_ra;
}

/* called by generated code for indirect calls / calls to functions missing from the table */
void rt_dyn_call(u32 addr) {
    RecompFn f = recomp_lookup(addr);
    if (f) { f(); return; }
    u32 ret = cpu.r[31];
    cpu.r[31] = GUEST_STOP;
    interp_run(addr, GUEST_STOP);
    cpu.r[31] = ret;
}
void rt_call(u32 addr) { rt_dyn_call(addr); }

/* `jr reg` to a computed address from inside a recompiled function: continue there (we then return to our
 * own caller, which is where the target's `jr ra` goes as well). The interpreter fallback must not clobber ra. */
void rt_dyn_jump(u32 addr) {
    RecompFn f = recomp_lookup(addr);
    if (f) { f(); return; }
    u32 ret = cpu.r[31];
    cpu.r[31] = GUEST_STOP;
    interp_run(addr, GUEST_STOP);
    cpu.r[31] = ret;
}

void rt_ret_mismatch(u32 target, u32 expected) {
    static int n;
    if (n++ < 20) WARN("[recomp] function returned to %08x instead of %08x\n", target, expected);
    if (target == GUEST_STOP) return;
    rt_dyn_jump(target);
}

void rt_syscall(u32 code_pc) { kernel_syscall(rd32(code_pc)); }

/* ------------------------------------------------------------ lock-step verification trace
 * In sync mode both backends look at hardware events only at the same safe points (function entries and
 * backward branches); each safe point can log (pc, cycles, register hash). Two runs - interpreter vs
 * recompiled code - must produce identical logs. */
static FILE *vt_f;
static u64 vt_n, vt_max;
bool g_vtrace;
void vtrace_open(const char *path, u64 max_records) {
    vt_f = fopen(path, "wb");
    vt_max = max_records;
    g_vtrace = vt_f != NULL;
    g_slow = g_vtrace || g_code_dirty;
}
void vtrace_close(void) { if (vt_f) fclose(vt_f); vt_f = NULL; g_vtrace = false; g_slow = g_code_dirty; }

/* VT_STRIDE=N writes only every Nth record, but each written hash covers the whole history so far
 * (a divergence shows up at the first written record after it); VT_FROM=M skips records before M. */
void vtrace_record(u32 pc, int preg, u32 pval) {
    if (!vt_f) return;
    if (vt_n >= vt_max) { g_vtrace = false; g_slow = g_code_dirty; return; }
    static u64 stride = 0, from = 0;
    static u32 roll = 2166136261u;
    if (!stride) { const char *e = getenv("VT_STRIDE"); stride = e ? strtoull(e, NULL, 0) : 1; if (!stride) stride = 1; e = getenv("VT_FROM"); from = e ? strtoull(e, NULL, 0) : 0; }
    u32 r[32];
    memcpy(r, cpu.r, sizeof r);
    if (preg) r[preg] = pval;
    u32 h = 2166136261u;
    for (int i = 1; i < 32; i++) { h ^= r[i]; h *= 16777619u; }
    h ^= cpu.hi; h *= 16777619u;
    h ^= cpu.lo; h *= 16777619u;
    h ^= cpu.cop0[12]; h *= 16777619u;
    static long dump_at = -2;
    if (dump_at == -2) { const char *e = getenv("VT_DUMP_AT"); dump_at = e ? atol(e) : -1; }
    if ((long)vt_n >= dump_at && (long)vt_n < dump_at + 4 && dump_at >= 0) {
        fprintf(stderr, "[vt %llu] pc=%08x cyc=%llu:", (unsigned long long)vt_n, pc, (unsigned long long)cpu.cycles);
        for (int i = 1; i < 32; i++) fprintf(stderr, " r%d=%08x", i, r[i]);
        fprintf(stderr, "\n");
    }
    if (stride > 1) { roll = (roll ^ h ^ (u32)cpu.cycles ^ (pc & 0x1fffffffu)) * 16777619u; h = roll; }
    if (vt_n >= from && vt_n % stride == 0) {
        u32 rec[3] = { pc & 0x1fffffffu, (u32)cpu.cycles, h };
        fwrite(rec, 4, 3, vt_f);
    }
    vt_n++;
}

void rt_check(u32 pc) {
    cpu.pc = pc;
    if (g_vtrace) vtrace_record(pc, 0, 0);
    if (cpu.cycles >= g_next_event) rt_event();
}

/* Function entry slow path. Returns nonzero when the function was executed by the interpreter instead
 * (recompiled code has been invalidated), in which case the generated function just returns. */
int rt_entry(u32 pc) {
    if (g_code_dirty) {
        u32 ret = cpu.r[31];
        cpu.r[31] = GUEST_STOP;
        interp_run(pc, GUEST_STOP);
        cpu.r[31] = ret;
        return 1;
    }
    rt_check(pc);
    return 0;
}
