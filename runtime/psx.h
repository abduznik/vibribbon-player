/* vibrecomp runtime: shared declarations.
 *
 * The runtime is deliberately layered:
 *   - CPU state + memory map          (mem.c, cpu.c)
 *   - hardware models                 (gpu.c, spu.c, cdrom.c, sio.c, timers.c, dma.c, irq.c, gte.c)
 *   - HLE kernel                      (kernel.c)
 *   - guest-code execution backends   (interpreter in cpu.c, recompiled code in gen/ dir)
 *   - platform frontend               (main.c, audio_in.c)
 */
#ifndef PSX_H
#define PSX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

/* Microsoft C runtime spellings (clang targeting MSVC) */
#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#define fseeko _fseeki64
#define ftello _ftelli64
typedef long long off_t;
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define CPU_HZ 33868800u
#define RAM_SIZE 0x200000u

/* ------------------------------------------------------------------ CPU */
typedef struct {
    u32 r[32];
    u32 hi, lo;
    u32 pc;       /* interpreter only */
    u32 cop0[32]; /* only 12 (SR), 13 (CAUSE), 14 (EPC) are meaningful */
    u32 gd[32];   /* GTE data regs */
    u32 gc[32];   /* GTE control regs */
    u64 cycles;
} CPU;

extern CPU cpu;
extern u8 ram[RAM_SIZE];
extern u8 scratch[1024];

#define R(n) (cpu.r[n])
#define SR_IEC 1u
#define SR_IM2 (1u << 10)

/* Event scheduling: next absolute cycle at which rt_event() must be called. */
extern u64 g_next_event;
void rt_event(void);                    /* run hardware to cpu.cycles, deliver IRQs */
void sched_update(void);                /* recompute g_next_event */

/* ------------------------------------------------------------------ memory */
u32 mem_read_slow(u32 addr, int size);
void mem_write_slow(u32 addr, u32 val, int size);

static inline u32 rd8(u32 a) {
    u32 p = a & 0x1fffffffu;
    if (likely(p < 0x800000u)) return ram[p & 0x1fffffu];
    return mem_read_slow(a, 1);
}
static inline u32 rd16(u32 a) {
    u32 p = a & 0x1fffffffu;
    if (likely(p < 0x800000u)) return *(u16 *)(ram + (p & 0x1ffffeu));
    return mem_read_slow(a, 2);
}
static inline u32 rd32(u32 a) {
    u32 p = a & 0x1fffffffu;
    if (likely(p < 0x800000u)) return *(u32 *)(ram + (p & 0x1ffffcu));
    return mem_read_slow(a, 4);
}
/* Stores into the address range covered by recompiled code are checked: self-modifying code would make
 * the generated C stale (see code_store in guest.c). */
extern u32 g_code_lo, g_code_len;
void code_store(u32 phys, u32 val, int size);
static inline void wr8(u32 a, u32 v) {
    u32 p = a & 0x1fffffffu;
    if (likely(p < 0x800000u)) {
        if (unlikely((p & 0x1fffffu) - g_code_lo < g_code_len)) code_store(p & 0x1fffffu, v, 1);
        ram[p & 0x1fffffu] = (u8)v; return;
    }
    mem_write_slow(a, v, 1);
}
static inline void wr16(u32 a, u32 v) {
    u32 p = a & 0x1fffffffu;
    if (likely(p < 0x800000u)) {
        if (unlikely((p & 0x1ffffeu) - g_code_lo < g_code_len)) code_store(p & 0x1ffffeu, v, 2);
        *(u16 *)(ram + (p & 0x1ffffeu)) = (u16)v; return;
    }
    mem_write_slow(a, v, 2);
}
static inline void wr32(u32 a, u32 v) {
    u32 p = a & 0x1fffffffu;
    if (likely(p < 0x800000u)) {
        if (unlikely((p & 0x1ffffcu) - g_code_lo < g_code_len)) code_store(p & 0x1ffffcu, v, 4);
        *(u32 *)(ram + (p & 0x1ffffcu)) = v; return;
    }
    mem_write_slow(a, v, 4);
}

/* ------------------------------------------------------------------ interrupts */
enum { IRQ_VBLANK = 0, IRQ_GPU, IRQ_CDROM, IRQ_DMA, IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_SIO0, IRQ_SIO1, IRQ_SPU, IRQ_PIO };
extern u32 i_stat, i_mask;
void irq_raise(int n);
static inline bool irq_pending(void) { return (i_stat & i_mask & 0x7ff) != 0; }
u32 irq_read(u32 off);
void irq_write(u32 off, u32 v, int size);

/* ------------------------------------------------------------------ GTE */
void gte_cmd(u32 op);
u32 gte_mfc2(int r);
void gte_mtc2(int r, u32 v);
u32 gte_cfc2(int r);
void gte_ctc2(int r, u32 v);
void gte_lwc2(int r, u32 v);
u32 gte_swc2(int r);

/* ------------------------------------------------------------------ hardware blocks */
/* GPU */
void gpu_init(void);
u32 gpu_read(u32 reg);                   /* reg 0 = GPUREAD, 4 = GPUSTAT */
void gpu_write(u32 reg, u32 v);          /* reg 0 = GP0, 4 = GP1 */
void gpu_dma_write(const u32 *words, u32 count);
void gpu_dma_read(u32 *words, u32 count);
void gpu_tick(u64 cycles);
bool gpu_vblank_event(void);
extern u16 vram[1024 * 512];
typedef struct { int x, y, w, h; bool depth24; bool enabled; bool pal; } DisplayInfo;
void gpu_get_display(DisplayInfo *d);
u64 gpu_frame_cycles(void);
void gpu_set_region_pal(bool pal);

/* timers */
void timers_init(void);
u32 timers_read(u32 off);
void timers_write(u32 off, u32 v, int size);
void timers_tick(u64 now);
u64 timers_next_event(void);
void timers_hblank(u64 now, int lines);   /* gpu tells timers about hblank/vblank */
void timers_set_blank(int hblank, int vblank);

/* DMA */
void dma_init(void);
u32 dma_read(u32 off);
void dma_write(u32 off, u32 v, int size);
void dma_tick(u64 now);
u64 dma_next_event(void);

/* CD-ROM */
void cd_init(void);
u32 cd_read(u32 off, int size);
void cd_write(u32 off, u32 v);
void cd_tick(u64 now);
u64 cd_next_event(void);
void cd_dma_read(u8 *dst, u32 bytes);   /* DMA3 */
u32 cd_data_fifo_pop32(void);
void cd_get_cdda(s16 *l, s16 *r);        /* one CD-DA sample pair if playing, else silence */

/* SPU */
void spu_init(void);
u32 spu_read(u32 off, int size);
void spu_write(u32 off, u32 v, int size);
void spu_tick(u64 now);
u64 spu_next_event(void);
void spu_dma_write(const u8 *src, u32 bytes);
void spu_dma_read(u8 *dst, u32 bytes);
u32 spu_dma_addr_after(void);
extern int (*spu_audio_out)(const s16 *stereo_frames, int nframes); /* returns frames accepted */
void spu_set_sink(void (*sink)(s16 l, s16 r));

/* SIO0 (pads / memory cards) */
void sio_init(void);
u32 sio_read(u32 off, int size);
void sio_write(u32 off, u32 v, int size);
void sio_tick(u64 now);
u64 sio_next_event(void);
extern u16 g_pad_buttons;    /* active-low PS1 button word, set by frontend */
extern u8 g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;

/* MDEC (stub) */
u32 mdec_read(u32 off);
void mdec_write(u32 off, u32 v);

/* ------------------------------------------------------------------ disc */
typedef struct DiscTrack {
    int number;
    bool audio;
    u32 start_lba;       /* absolute LBA of INDEX 01 */
    u32 length;          /* sectors */
} DiscTrack;

#define MAX_TRACKS 99
typedef struct Disc Disc;
struct Disc {
    DiscTrack tracks[MAX_TRACKS + 1];
    int ntracks;
    u32 total_sectors;
    bool is_psx_data_disc;
    /* backend */
    int (*read_sector)(Disc *d, u32 lba, u8 *out2352);  /* raw 2352-byte sector, 0 on success */
    void *priv;
    void (*destroy)(Disc *d);                            /* optional: release priv (called by disc_free) */
    char label[64];
};
Disc *disc_open_cue(const char *cue_path);
Disc *disc_open_audio_file(const char *path);       /* single-track audio CD from any audio file */
Disc *disc_open_audio_pcm(const s16 *interleaved, size_t nframes, const char *label);
void disc_free(Disc *d);
extern Disc *g_disc;           /* currently inserted disc */
extern Disc *g_game_disc;      /* the original game disc, kept for file loads */
void cd_insert_disc(Disc *d);  /* shell open/close cycle + media change */
void cd_set_shell(bool open);
extern bool g_cd_turbo;
extern void (*g_cd_command_hook)(u8 cmd);
void cd_begin_swap(Disc *newd, u64 open_cycles);   /* open lid, replace disc, close lid (async) */

/* ISO9660 over a Disc (game files) */
typedef struct { char name[64]; u32 lba; u32 size; } IsoFile;
bool iso_find(Disc *d, const char *path, IsoFile *out);
bool disc_boot_name(Disc *d, char *out, size_t n);   /* executable named by SYSTEM.CNF BOOT=, without cdrom:\\ and ;1 */
u8 *iso_read_file(Disc *d, const IsoFile *f);   /* malloc'd */

/* autoplay (scripted menu navigation) */
void autoplay_init(Disc *song);          /* NULL disables */
bool autoplay_active(void);
void autoplay_on_frame(void);
extern bool g_fast_forward;
extern bool g_pad_override_active;
extern u16 g_pad_override;

/* ------------------------------------------------------------------ kernel (HLE BIOS) */
void kernel_init(void);
void kernel_boot(const char *boot_exe_path_on_disc);
int bios_call(u32 table, u32 fn);        /* called from interpreter/recomp when a BIOS vector is hit */
void kernel_syscall(u32 code);
void kernel_irq_deliver(void);
extern bool g_irq_enabled_shadow;
extern jmp_buf g_exec_jmp;               /* DoExecute / exit unwind here */
extern jmp_buf g_irq_jmp;
extern int g_irq_depth;

/* guest execution API (backend independent) */
void guest_call(u32 addr);                         /* run guest function until it returns */
void guest_run_exe(u32 pc, u32 gp, u32 sp);        /* top-level program start (never returns until exit) */
void interp_run(u32 pc, u32 stop_pc);              /* interpreter loop */
extern bool g_force_interp;
extern bool g_sync_mode;                 /* interpreter looks at hardware events only at recompiler safe points */
void vtrace_open(const char *path, u64 max_records);
extern bool g_slow;                      /* recompiled safe points take the slow path (tracing / code modified) */
extern bool g_code_dirty;
void learn_writes_start(const char *path);   /* record words written inside the text range (feeds tools/recomp.py --patched) */
void learn_writes_dump(void);
void learn_set_range(u32 lo, u32 len);
void vtrace_close(void);
extern bool g_trace_exec;

/* recompiled code registry (filled in by generated code) */
typedef void (*RecompFn)(void);
typedef struct {
    u32 addr;
    RecompFn fn;
} RecompEntry;
typedef struct {
    const char *name;
    u32 text_addr, text_size;
    u32 crc;
    u32 entry_pc, gp, sp_base;
    const RecompEntry *table;   /* sorted by addr */
    u32 count;
    u32 cover_lo, cover_len;    /* RAM byte range spanned by recompiled instructions */
    const u32 *cover_bits;      /* one bit per word from cover_lo: word is part of a recompiled function */
} RecompOverlay;
extern const RecompOverlay *const g_overlays[];
extern const int g_overlay_count;
void recomp_select(const RecompOverlay *o);
RecompFn recomp_lookup(u32 addr);
RecompFn recomp_entry(u32 addr);      /* function starting exactly at addr, even when running interpreted */
extern const RecompOverlay *g_cur_overlay;

/* recompiled code helpers (called from generated code) */
void rt_dyn_call(u32 addr);       /* jalr / indirect call */
void rt_dyn_jump(u32 addr);       /* jr to non-ra register: tail transfer */
void rt_call(u32 addr);           /* direct call to function missing from table (interp fallback) */
void rt_syscall(u32 code_pc);
void rt_break(u32 pc);
void rt_event_hook(void);         /* slow path of the safe-point check */
void rt_cop0_write(int reg, u32 v);
u32  rt_cop0_read(int reg);
void rt_rfe(void);
void rt_bios_vector(u32 vec);     /* jr to 0xA0/0xB0/0xC0 */

/* whole-machine control (runtime.c) */
void runtime_reset(Disc *game_disc, bool pal);
void runtime_run(const char *boot_exe);          /* blocks until the guest exits or g_quit */
extern jmp_buf g_quit_jmp;
/* audio ring (spu.c): 44.1 kHz stereo s16 produced by the SPU */
size_t spu_ring_fill(void);
size_t spu_ring_pull(s16 *dst, size_t frames);
void spu_ring_clear(void);

/* ------------------------------------------------------------------ frontend hooks */
void frontend_present(void);                /* called each vblank */
void frontend_pace(void);                   /* throttle to real time / audio */
extern bool g_quit;
extern int g_verbose;

#define LOG(...) do { if (g_verbose) { fprintf(stderr, __VA_ARGS__); } } while (0)
#define WARN(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

#endif
