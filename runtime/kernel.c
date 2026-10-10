/* High-level emulation of the PS1 BIOS kernel: events, interrupt hook, syscalls, Exec, boot. */
#include "coro.h"      /* must come first: it may define feature macros for <ucontext.h> */
#include "psx.h"
#include <stdlib.h>

jmp_buf g_exec_jmp;
jmp_buf g_irq_jmp;
int g_irq_depth;
bool g_irq_enabled_shadow;

/* ------------------------------------------------------------ events */
#define EV_MAX 64
typedef struct { u32 cls, spec, mode, handler; u32 status; } Event;
static Event events[EV_MAX];

#define EVST_FREE 0
#define EVST_DISABLED 0x1000
#define EVST_ENABLED 0x2000
#define EVST_DELIVERED 0x4000
#define EVMODE_CALLBACK 0x1000

static u32 ev_handle(int i) { return 0xf1000000u + (u32)i; }
static Event *ev_get(u32 h) {
    u32 i = h - 0xf1000000u;
    if (i >= EV_MAX) return NULL;
    return &events[i];
}

static void ev_deliver(u32 cls, u32 spec) {
    if (g_verbose >= 2) {
        fprintf(stderr, "[ev] deliver %08x/%04x; table:", cls, spec);
        for (int i = 0; i < EV_MAX; i++) if (events[i].status) fprintf(stderr, " [%d: %08x/%04x st=%04x mode=%04x]", i, events[i].cls, events[i].spec, events[i].status, events[i].mode);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < EV_MAX; i++) {
        Event *e = &events[i];
        if (e->status != EVST_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == EVMODE_CALLBACK && e->handler) {
            guest_call(e->handler);
        } else {
            e->status = EVST_DELIVERED;
        }
    }
}

/* run emulated time forward until predicate is true (used by WaitEvent / VSync-like blocking) */
static void idle_advance(void) {
    u64 t = g_next_event;
    if (t > cpu.cycles) cpu.cycles = t; else cpu.cycles++;
    rt_event();
}

/* ------------------------------------------------------------ BIOS threads
 * Modelled like the real kernel: a PCB holding a pointer to the current TCB, and a TCB table in
 * guest memory (games read and even write these). ChangeThread saves the registers into the TCB the
 * PCB currently points at, makes the target current and loads its registers. Host coroutines only
 * carry each context's C stack so recompiled code (which keeps state on the host stack) can resume.
 *
 * TCB layout: +0 status, +8+4*i = r[i], +0x88 EPC, +0x8c HI, +0x90 LO, +0x94 SR, +0x98 CAUSE. */
#define TCB_V 0x80006c00u
#define PCB_V 0x80006bf0u
#define TCB_SIZE 0xc0u
#define TCB_SLOTS 8
#define TCB_GAME_COUNT 4
#define THREAD_STACK (512 * 1024)
#define TCB_STATUS_FREE 0x1000u
#define TCB_STATUS_USED 0x4000u

static Coro co_ctx[TCB_SLOTS];
static bool co_valid[TCB_SLOTS];
static void *co_stack[TCB_SLOTS];
static int depth_of[TCB_SLOTS];            /* g_irq_depth of a parked context */
static bool parked_in_irq[TCB_SLOTS];      /* parked inside ReturnFromException (preempted by an interrupt) */
static jmp_buf irq_env_of[TCB_SLOTS];

static u32 tcb_addr(int slot) { return TCB_V + (u32)slot * TCB_SIZE; }
static int pcb_slot(void) {
    u32 cur = rd32(PCB_V);
    int s = (int)((cur - TCB_V) / TCB_SIZE);
    return (s >= 0 && s < TCB_SLOTS) ? s : 0;
}
static u32 push_sr(u32 sr) { return (sr & ~0x3fu) | ((sr << 2) & 0x3cu); }   /* exception entry */
static u32 pop_sr(u32 sr) { return (sr & ~0xfu) | ((sr >> 2) & 0xfu); }      /* rfe */

static void tcb_save(int slot, u32 resume_pc) {
    u32 t = tcb_addr(slot);
    for (int i = 1; i < 32; i++) wr32(t + 8 + 4 * (u32)i, cpu.r[i]);
    wr32(t + 0x88, resume_pc);
    wr32(t + 0x8c, cpu.hi);
    wr32(t + 0x90, cpu.lo);
    wr32(t + 0x94, push_sr(cpu.cop0[12]));
}
static void tcb_load(int slot) {
    u32 t = tcb_addr(slot);
    for (int i = 1; i < 32; i++) cpu.r[i] = rd32(t + 8 + 4 * (u32)i);
    cpu.hi = rd32(t + 0x8c);
    cpu.lo = rd32(t + 0x90);
    cpu.cop0[12] = pop_sr(rd32(t + 0x94));
}

static void thread_trampoline(void) {
    int slot = pcb_slot();
    g_irq_depth = 0;
    u32 pc = rd32(tcb_addr(slot) + 0x88);
    guest_call(pc);
    /* a thread function returned (the BIOS would jump to address 0); retire it and resume slot 0 */
    WARN("[kernel] thread in slot %d returned\n", slot);
    wr32(tcb_addr(slot), TCB_STATUS_FREE);
    wr32(PCB_V, tcb_addr(0));
    tcb_load(0);
    coro_jump(&co_ctx[0]);
}


/* Park the running host context under slot `from` and continue the context belonging to slot `to`. */
static void co_switch(int from, int to, bool from_in_irq) {
    if (!co_valid[to]) {
        if (!co_stack[to]) co_stack[to] = malloc(THREAD_STACK);
        coro_create(&co_ctx[to], co_stack[to], THREAD_STACK, thread_trampoline);
    }
    co_valid[from] = true;
    depth_of[from] = g_irq_depth;
    parked_in_irq[from] = from_in_irq;
    if (from_in_irq) memcpy(irq_env_of[from], g_irq_jmp, sizeof g_irq_jmp);
    coro_switch(&co_ctx[from], &co_ctx[to]);
    /* resumed: whoever switched back already loaded this context's registers */
    g_irq_depth = depth_of[from];
    if (from_in_irq) memcpy(g_irq_jmp, irq_env_of[from], sizeof g_irq_jmp);
}

static void kernel_threads_init(void) {
    coro_thread_init();
    for (int i = 1; i < TCB_SLOTS; i++) coro_destroy(&co_ctx[i]);   /* contexts left over from an earlier run */
    memset(co_valid, 0, sizeof co_valid);
    for (int i = 0; i < TCB_SLOTS; i++) {
        u32 t = tcb_addr(i);
        for (u32 o = 0; o < TCB_SIZE; o += 4) wr32(t + o, 0);
        wr32(t, i == 0 ? TCB_STATUS_USED : TCB_STATUS_FREE);
    }
    wr32(PCB_V, tcb_addr(0));
    wr32(PCB_V + 4, 0);
    wr32(0x108, PCB_V);
    wr32(0x10c, 8);
    wr32(0x110, TCB_V);
    wr32(0x114, TCB_GAME_COUNT * TCB_SIZE);
}

static u32 thread_open(u32 pc, u32 sp, u32 gp) {
    for (int i = 1; i < TCB_GAME_COUNT; i++) {
        u32 t = tcb_addr(i);
        if (rd32(t) == TCB_STATUS_USED) continue;
        for (u32 o = 0; o < TCB_SIZE; o += 4) wr32(t + o, 0);
        wr32(t, TCB_STATUS_USED);
        wr32(t + 8 + 4 * 28, gp);
        wr32(t + 8 + 4 * 29, sp);
        wr32(t + 8 + 4 * 30, sp);
        wr32(t + 0x88, pc);
        wr32(t + 0x94, 0x40000404u);
        co_valid[i] = false;
        LOG("[kernel] OpenThread -> slot %d (pc=%08x sp=%08x gp=%08x)\n", i, pc, sp, gp);
        return 0xff000000u + (u32)i;
    }
    return 0xffffffffu;
}

static void thread_switch(u32 handle) {
    int idx = (int)(handle & 0xffff);
    if (g_verbose >= 2) fprintf(stderr, "[thread] ChangeThread(%08x) ra=%08x; PCB.current slot=%d\n", handle, cpu.r[31], pcb_slot());
    if ((handle & 0xffff0000u) != 0xff000000u || idx >= TCB_GAME_COUNT) { cpu.r[2] = 0; return; }
    int cur = pcb_slot();
    /* syscall entry: registers go into the TCB the PCB currently points at */
    tcb_save(cur, cpu.r[31]);
    wr32(PCB_V, tcb_addr(idx));
    tcb_load(idx);
    if (idx == cur) { cpu.r[2] = 1; return; }
    co_switch(cur, idx, false);
    cpu.r[2] = 1;
}

static void thread_close(u32 handle) {
    int idx = (int)(handle & 0xffff);
    if ((handle & 0xffff0000u) != 0xff000000u || idx == 0 || idx >= TCB_GAME_COUNT) { cpu.r[2] = 0; return; }
    wr32(tcb_addr(idx), TCB_STATUS_FREE);
    co_valid[idx] = false;
    cpu.r[2] = 1;
}

/* ------------------------------------------------------------ interrupts */
/* BIOS priority queues (SysEnqIntRP): each entry {next, handler(+4), verifier(+8)} */
static u32 irq_queue[4];
static u32 hook_buf;            /* address of the jmp_buf-like struct registered with HookEntryInt */
static bool hook_set;
#define IRQ_STACK_TOP 0x8000f800u

static void sys_deq_int(u32 prio, u32 entry);
static void sys_enq_int(u32 prio, u32 entry) {
    if (prio > 3) return;
    sys_deq_int(prio, entry);               /* avoid double insertion */
    wr32(entry, irq_queue[prio]);
    irq_queue[prio] = entry;
}
static void sys_deq_int(u32 prio, u32 entry) {
    if (prio > 3) return;
    u32 cur = irq_queue[prio], prev = 0;
    while (cur) {
        if (cur == entry) {
            if (prev) wr32(prev, rd32(cur)); else irq_queue[prio] = rd32(cur);
            return;
        }
        prev = cur; cur = rd32(cur);
    }
}

static void run_queues(void) {
    for (u32 prio = 0; prio < 4; prio++) {
        u32 e = irq_queue[prio];
        int guard = 0;
        while (e && guard++ < 64) {
            u32 next = rd32(e);
            u32 handler = rd32(e + 4), verifier = rd32(e + 8);
            cpu.r[29] = IRQ_STACK_TOP - 0x400u * (u32)g_irq_depth;
            bool fire = true;
            if (verifier) {
                cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
                guest_call(verifier);
                fire = cpu.r[2] != 0;
            }
            if (fire && handler) {
                cpu.r[29] = IRQ_STACK_TOP - 0x400u * (u32)g_irq_depth;
                cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
                guest_call(handler);
            }
            e = next;
        }
    }
}

static int irq_entry_slot[4];

void kernel_irq_deliver(void) {
    if (g_irq_depth >= 3) return;
    if (!hook_set && !irq_queue[0] && !irq_queue[1] && !irq_queue[2] && !irq_queue[3]) return;
    u32 gd[32], gc[32];
    memcpy(gd, cpu.gd, sizeof gd); memcpy(gc, cpu.gc, sizeof gc);

    /* exception entry: save the interrupted context into the current TCB, interrupts off */
    int slot = pcb_slot();
    tcb_save(slot, cpu.pc);
    cpu.cop0[12] &= ~SR_IEC;
    irq_entry_slot[g_irq_depth] = slot;
    g_irq_depth++;
    jmp_buf saved_jmp;
    memcpy(saved_jmp, g_irq_jmp, sizeof saved_jmp);
    if (setjmp(g_irq_jmp) == 0) {
        run_queues();
        if (hook_set) {
            /* hook buffer layout (setjmp compatible): pc, sp, fp, s0..s7, gp */
            u32 b = hook_buf;
            u32 pc = rd32(b + 0);
            cpu.r[29] = rd32(b + 4);
            cpu.r[30] = rd32(b + 8);
            for (int i = 0; i < 8; i++) cpu.r[16 + i] = rd32(b + 12 + i * 4);
            cpu.r[28] = rd32(b + 44);
            cpu.r[2] = 1;
            cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
            guest_call(pc);
        }
    }
    memcpy(g_irq_jmp, saved_jmp, sizeof saved_jmp);
    g_irq_depth--;

    /* exception exit: restore from whichever TCB is current now */
    tcb_load(pcb_slot());
    memcpy(cpu.gd, gd, sizeof gd); memcpy(cpu.gc, gc, sizeof gc);
}

/* B0 17 ReturnFromException: leave the interrupt handler. If the handler retargeted the PCB to another
 * thread, this is a preemptive thread switch. */
static void return_from_exception(void) {
    if (g_irq_depth <= 0) return;
    int entry = irq_entry_slot[g_irq_depth - 1];
    int now = pcb_slot();
    if (now != entry) {
        tcb_load(now);
        co_switch(entry, now, true);       /* resumes here when `entry` is scheduled again */
        tcb_load(entry);
    }
    longjmp(g_irq_jmp, 1);
}

/* ------------------------------------------------------------ Exec / boot */

static u32 crc32_buf(const u8 *p, size_t n) {
    static u32 tab[256];
    if (!tab[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    u32 c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

static void select_overlay_for(u32 t_addr, u32 t_size) {
    recomp_select(NULL);
    learn_set_range(t_addr, t_size);
    if (g_force_interp && !g_sync_mode) return;
    u32 p = t_addr & 0x1fffffffu;
    if (p + t_size > RAM_SIZE) return;
    u32 crc = crc32_buf(ram + p, t_size);
    for (int i = 0; i < g_overlay_count; i++) {
        const RecompOverlay *o = g_overlays[i];
        if (o->text_addr == t_addr && o->text_size == t_size && o->crc == crc) {
            recomp_select(o);
            LOG("[kernel] recompiled overlay '%s' selected\n", o->name);
            return;
        }
    }
    LOG("[kernel] no recompiled overlay for text %08x+%x crc %08x -> interpreter\n", t_addr, t_size, crc);
}

static void do_exec(u32 hdr, u32 argc, u32 argv) {
    /* EXEC struct: pc0 gp0 t_addr t_size d_addr d_size b_addr b_size stk_addr stk_size ... */
    u32 pc0 = rd32(hdr + 0), gp0 = rd32(hdr + 4), t_addr = rd32(hdr + 8), t_size = rd32(hdr + 12);
    u32 b_addr = rd32(hdr + 24), b_size = rd32(hdr + 28), stk_addr = rd32(hdr + 32), stk_size = rd32(hdr + 36);
    LOG("[kernel] Exec pc=%08x gp=%08x text=%08x+%x bss=%08x+%x stack=%08x+%x\n", pc0, gp0, t_addr, t_size, b_addr, b_size, stk_addr, stk_size);
    if (b_size) memset(ram + (b_addr & 0x1ffffc), 0, b_size);

    /* the BIOS saves the caller's context and resumes it (with v0 = 1) when the program returns */
    u32 saved[32];
    memcpy(saved, cpu.r, sizeof saved);
    const RecompOverlay *saved_ov = g_cur_overlay;
    u32 sp = stk_addr ? (stk_addr + stk_size) : cpu.r[29];

    select_overlay_for(t_addr, t_size);
    cpu.r[28] = gp0;
    cpu.r[29] = cpu.r[30] = sp;
    cpu.r[4] = argc; cpu.r[5] = argv;
    guest_call(pc0);

    memcpy(cpu.r, saved, sizeof saved);
    recomp_select(saved_ov);
    cpu.r[2] = 1;
    LOG("[kernel] program at %08x returned to Exec caller\n", pc0);
}

int bios_call(u32 table, u32 fn) {
    u32 a0 = cpu.r[4], a1 = cpu.r[5], a2 = cpu.r[6], a3 = cpu.r[7];
    fn &= 0xff;
    if (g_verbose >= 2) {
        extern const char *bios_name(u32 table, u32 fn);
        fprintf(stderr, "[bios] %02x/%02x %s(%08x,%08x,%08x,%08x) ra=%08x\n", table, fn, bios_name(table, fn), a0, a1, a2, a3, cpu.r[31]);
    }
    switch (table) {
    case 0xa0:
        switch (fn) {
        case 0x13: /* setjmp(buf) */
            wr32(a0 + 0, cpu.r[31]); wr32(a0 + 4, cpu.r[29]); wr32(a0 + 8, cpu.r[30]);
            for (int i = 0; i < 8; i++) wr32(a0 + 12 + i * 4, cpu.r[16 + i]);
            wr32(a0 + 44, cpu.r[28]);
            cpu.r[2] = 0; return 0;
        case 0x14: { /* longjmp(buf, v) */
            cpu.r[31] = rd32(a0 + 0); cpu.r[29] = rd32(a0 + 4); cpu.r[30] = rd32(a0 + 8);
            for (int i = 0; i < 8; i++) cpu.r[16 + i] = rd32(a0 + 12 + i * 4);
            cpu.r[28] = rd32(a0 + 44);
            cpu.r[2] = a1;
            return 0;
        }
        case 0x17: { /* strcmp */
            u32 i = 0; for (;; i++) { u8 x = rd8(a0 + i), y = rd8(a1 + i); if (x != y || !x) { cpu.r[2] = (u32)((int)x - (int)y); break; } }
            return 0;
        }
        case 0x18: { /* strncmp */
            u32 r = 0; for (u32 i = 0; i < a2; i++) { u8 x = rd8(a0 + i), y = rd8(a1 + i); if (x != y) { r = (u32)((int)x - (int)y); break; } if (!x) break; }
            cpu.r[2] = r; return 0;
        }
        case 0x19: { /* strcpy */
            u32 i = 0; for (;; i++) { u8 c = rd8(a1 + i); wr8(a0 + i, c); if (!c) break; }
            cpu.r[2] = a0; return 0;
        }
        case 0x1b: { u32 n = 0; while (rd8(a0 + n)) n++; cpu.r[2] = n; return 0; }
        case 0x27: for (u32 i = 0; i < a2; i++) wr8(a1 + i, rd8(a0 + i)); return 0;                       /* bcopy(src,dst,n) */
        case 0x28: for (u32 i = 0; i < a1; i++) wr8(a0 + i, 0); return 0;                                   /* bzero */
        case 0x2a: case 0x2c: /* memcpy / memmove */
            if (fn == 0x2c && a0 > a1) for (u32 i = a2; i > 0; i--) wr8(a0 + i - 1, rd8(a1 + i - 1));
            else for (u32 i = 0; i < a2; i++) wr8(a0 + i, rd8(a1 + i));
            cpu.r[2] = a0; return 0;
        case 0x2b: for (u32 i = 0; i < a2; i++) wr8(a0 + i, a1); cpu.r[2] = a0; return 0;                  /* memset */
        case 0x2d: { u32 r = 0; for (u32 i = 0; i < a2; i++) { u8 x = rd8(a0 + i), y = rd8(a1 + i); if (x != y) { r = (u32)((int)x - (int)y); break; } } cpu.r[2] = r; return 0; }
        case 0x39: cpu.r[2] = 0; return 0;                       /* InitHeap */
        case 0x3a: /* SystemErrorExit */ WARN("[kernel] SystemErrorExit(%u)\n", a0); g_quit = true; return 0;
        case 0x3f: case 0x3e: case 0x3d: return 0;               /* printf / puts: ignored */
        case 0x43: do_exec(a0, a1, a2); return 0;                /* Exec */
        case 0x44: return 0;                                     /* FlushCache */
        case 0x45: return 0;                                     /* init_a0_b0_c0_vectors */
        case 0x49: gpu_write(0, a0); return 0;                   /* GPU_cw */
        case 0x4a: for (u32 i = 0; i < a1; i++) gpu_write(0, rd32(a0 + i * 4)); return 0;   /* GPU_cwp */
        case 0x71: cpu.r[2] = 1; return 0;                       /* _96_init */
        case 0x72: cpu.r[2] = 0; return 0;                       /* _96_remove */
        default:
            LOG("[kernel] unhandled A0 %02x (%s)\n", fn, "?");
            cpu.r[2] = 0; return 0;
        }
    case 0xb0:
        switch (fn) {
        case 0x07: ev_deliver(a0, a1); cpu.r[2] = 0; return 0;           /* DeliverEvent */
        case 0x08: { /* OpenEvent(class, spec, mode, handler) */
            for (int i = 0; i < EV_MAX; i++) if (events[i].status == EVST_FREE) {
                events[i] = (Event){a0, a1, a2, a3, EVST_DISABLED};
                cpu.r[2] = ev_handle(i); return 0;
            }
            cpu.r[2] = 0xffffffffu; return 0;
        }
        case 0x09: { Event *e = ev_get(a0); if (e) e->status = EVST_FREE; cpu.r[2] = 1; return 0; }
        case 0x0a: { /* WaitEvent */
            Event *e = ev_get(a0);
            if (!e || e->status == EVST_FREE) { cpu.r[2] = 0; return 0; }
            int guard = 0;
            while (e->status != EVST_DELIVERED && !g_quit && ++guard < 100000000) idle_advance();
            e->status = EVST_ENABLED;
            cpu.r[2] = 1; return 0;
        }
        case 0x0b: { /* TestEvent */
            Event *e = ev_get(a0);
            if (e && e->status == EVST_DELIVERED) { e->status = EVST_ENABLED; cpu.r[2] = 1; }
            else cpu.r[2] = 0;
            return 0;
        }
        case 0x0c: { Event *e = ev_get(a0); if (e && e->status != EVST_FREE) e->status = EVST_ENABLED; cpu.r[2] = 1; return 0; }
        case 0x0d: { Event *e = ev_get(a0); if (e && e->status != EVST_FREE) e->status = EVST_DISABLED; cpu.r[2] = 1; return 0; }
        case 0x17: return_from_exception(); return 0;                   /* ReturnFromException */
        case 0x18: hook_set = false; hook_buf = 0; return 0;           /* ResetEntryInt */
        case 0x19: hook_buf = a0; hook_set = a0 != 0; return 0;        /* HookEntryInt */
        case 0x35: /* FileWrite(fd, buf, n): tty output */
            if (a0 == 1 && g_verbose > 1) { for (u32 i = 0; i < a2; i++) fputc(rd8(a1 + i), stderr); }
            cpu.r[2] = a2; return 0;
        case 0x3d: case 0x3f: case 0x3c: return 0;                      /* putchar / puts */
        case 0x5b: return 0;                                            /* ChangeClearPad */
        case 0x56: cpu.r[2] = 0xa0000674u; return 0;                    /* GetC0Table (RAM-backed fake) */
        case 0x57: cpu.r[2] = 0xa0000874u; return 0;                    /* GetB0Table */
        case 0x0e: cpu.r[2] = thread_open(a0, a1, a2); return 0;       /* OpenThread(pc, sp, gp) */
        case 0x0f: thread_close(a0); return 0;                          /* CloseThread */
        case 0x10: thread_switch(a0); return 0;                         /* ChangeThread */
        default:
            LOG("[kernel] unhandled B0 %02x\n", fn);
            cpu.r[2] = 0; return 0;
        }
    case 0xc0:
        switch (fn) {
        case 0x02: sys_enq_int(a0, a1); cpu.r[2] = 0; return 0;
        case 0x03: sys_deq_int(a0, a1); cpu.r[2] = 0; return 0;
        case 0x0a: return 0;                                           /* ChangeClearRCnt */
        default:
            LOG("[kernel] unhandled C0 %02x\n", fn);
            cpu.r[2] = 0; return 0;
        }
    }
    return 0;
}

void kernel_syscall(u32 op) {
    (void)op;
    switch (cpu.r[4]) {
    case 1: /* EnterCriticalSection */
        cpu.r[2] = (cpu.cop0[12] & SR_IEC) ? 1 : 0;
        cpu.cop0[12] &= ~SR_IEC;
        break;
    case 2: /* ExitCriticalSection */
        cpu.cop0[12] |= SR_IEC;
        g_next_event = 0;                /* an interrupt may now be deliverable */
        break;
    default:
        LOG("[kernel] syscall %u\n", cpu.r[4]);
        break;
    }
}

void rt_bios_vector(u32 vec) { bios_call(vec, cpu.r[9]); }

void kernel_init(void) {
    memset(events, 0, sizeof events);
    memset(irq_queue, 0, sizeof irq_queue);
    kernel_threads_init();
    /* fake kernel jump tables: entries point at distinct scratch "handler" blocks in kernel RAM */
    for (int i = 0; i < 32; i++) { wr32(0x674 + 4 * i, 0x80000c80u + 0x80u * (u32)i); wr32(0x874 + 4 * i, 0x80004c80u + 0x80u * (u32)i); wr32(0x200 + 4 * i, 0x80008c80u + 0x80u * (u32)i); }
    hook_set = false; hook_buf = 0;
    g_irq_depth = 0;
    cpu.cop0[12] = 0x40000401u;       /* COP2 usable, IEc + IM2 set */
}

/* ------------------------------------------------------------ boot */
static bool load_exe_to_ram(const u8 *img, size_t size, u32 *pc, u32 *gp, u32 *sp, u32 *taddr, u32 *tsize) {
    if (size < 0x800 || memcmp(img, "PS-X EXE", 8)) return false;
    u32 h[16];
    memcpy(h, img + 0x10, sizeof h);
    *pc = h[0]; *gp = h[1]; *taddr = h[2]; *tsize = h[3];
    u32 bss = h[6], bsize = h[7];
    u32 sp_base = h[8], sp_off = h[9];
    *sp = sp_base ? sp_base + sp_off : 0;
    if (0x800 + *tsize > size) return false;
    memcpy(ram + (*taddr & 0x1ffffc), img + 0x800, *tsize);
    if (bsize) memset(ram + (bss & 0x1ffffc), 0, bsize);
    return true;
}

static void run_program(u32 pc, u32 gp, u32 sp, u32 a0, u32 a1) {
    cpu.r[28] = gp;
    cpu.r[29] = cpu.r[30] = sp;
    cpu.r[4] = a0; cpu.r[5] = a1;
    cpu.r[31] = 0x1ffffff0u;
    guest_call(pc);
}

void kernel_boot(const char *boot_path_override) {
    Disc *d = g_game_disc;
    char boot[64] = "SCES_028.73";
    IsoFile f;
    if (boot_path_override) snprintf(boot, sizeof boot, "%s", boot_path_override);
    else if (iso_find(d, "SYSTEM.CNF", &f)) {
        u8 *cnf = iso_read_file(d, &f);
        char *p = strstr((char *)cnf, "BOOT");
        if (p) {
            p = strstr(p, "cdrom:");
            if (p) {
                p += 6; while (*p == '\\' || *p == '/') p++;
                int i = 0; while (*p && *p != ';' && *p != '\r' && *p != '\n' && i < 60) boot[i++] = *p++;
                boot[i] = 0;
            }
        }
        free(cnf);
    }
    LOG("[kernel] booting %s\n", boot);
    if (!iso_find(d, boot, &f)) { WARN("boot executable %s not found on disc\n", boot); return; }
    u8 *img = iso_read_file(d, &f);
    u32 pc, gp, sp, taddr, tsize;
    if (!load_exe_to_ram(img, f.size, &pc, &gp, &sp, &taddr, &tsize)) { WARN("bad boot executable\n"); free(img); return; }
    free(img);
    if (!sp) sp = 0x801ffff0u;
    select_overlay_for(taddr, tsize);

    run_program(pc, gp, sp, 0, 0);
    LOG("[kernel] boot program returned\n");
}
