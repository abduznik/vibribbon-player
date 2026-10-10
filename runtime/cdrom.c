/* CD-ROM controller (the drive's micro-controller as seen through 1F801800h..03h). */
#include "psx.h"
#include <stdlib.h>

#define ST_ERROR 0x01
#define ST_MOTOR 0x02
#define ST_SEEKERR 0x04
#define ST_IDERR 0x08
#define ST_SHELL 0x10
#define ST_READ 0x20
#define ST_SEEK 0x40
#define ST_PLAY 0x80

typedef struct {
    bool pending;
    u64 at;
    int intno;
    u8 data[16];
    int len;
} Resp;

static struct {
    u8 index, ien, ifl;
    u8 params[16]; int nparams;
    u8 resp[16]; int resp_len, resp_pos;
    u8 mode;
    bool motor;
    bool shell_open, shell_seen_open;
    bool busy;
    Resp first, second;
    bool irq_line;
    /* position */
    u32 pos_lba;                  /* head position, absolute disc LBA */
    u32 setloc_lba; bool setloc_set;
    bool reading, playing, seeking;
    u64 next_sector_at;
    u32 play_end_lba;
    /* data buffer */
    u8 sector[2352];
    u32 data_off, data_len, data_pos;
    bool bfrd;                    /* software asked for the data FIFO */
    bool fifo_loaded;             /* FIFO holds the current sector (not reloaded by repeated BFRD=1) */
    bool data_available;
    /* cdda */
    u8 cdda_sector[2352];
    int cdda_idx;                 /* frame index within current sector */
    bool cdda_valid;
    int peak_l, peak_r;
    int report_counter;
    bool report_pending, int4_pending;
    u8 report_peak[2];
    /* volume matrix: ll = CD L -> SPU L, lr = CD L -> SPU R, rl = CD R -> SPU L, rr = CD R -> SPU R */
    u8 vol_ll, vol_lr, vol_rl, vol_rr, tmp_ll, tmp_lr, tmp_rl, tmp_rr;
    bool adpmute;
    u8 filter_file, filter_chan;
    /* disc swap in progress */
    Disc *swap_disc; u64 swap_close_at; bool swap_active;
    u64 spinup_done_at;
} cd;

static inline u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static inline int unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

static void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f) {
    lba += 150;
    *m = bcd(lba / (75 * 60)); *s = bcd((lba / 75) % 60); *f = bcd(lba % 75);
}
static u32 msf_to_lba(u8 m, u8 s, u8 f) {
    int v = (unbcd(m) * 60 + unbcd(s)) * 75 + unbcd(f);
    return v >= 150 ? (u32)(v - 150) : 0;
}

static u8 cur_stat(void) {
    u8 s = 0;
    if (cd.motor && g_disc && !cd.shell_open) s |= ST_MOTOR;
    if (cd.shell_open || cd.shell_seen_open) s |= ST_SHELL;
    if (cd.reading) s |= ST_READ;
    if (cd.seeking) s |= ST_SEEK;
    if (cd.playing) s |= ST_PLAY;
    return s;
}

static void check_irq(void) {
    bool line = (cd.ifl & cd.ien & 7) != 0;
    if (line && !cd.irq_line) irq_raise(IRQ_CDROM);
    cd.irq_line = line;
}

static void deliver(Resp *r) {
    if (g_verbose >= 2) {
        fprintf(stderr, "[cd]   INT%d:", r->intno);
        for (int i = 0; i < r->len; i++) fprintf(stderr, " %02x", r->data[i]);
        fprintf(stderr, "\n");
    }
    cd.resp_len = r->len; cd.resp_pos = 0;
    memcpy(cd.resp, r->data, r->len);
    cd.ifl = (cd.ifl & ~7) | (u8)r->intno;
    r->pending = false;
    cd.busy = false;
    check_irq();
}

static void set_first(int intno, u64 delay, const u8 *d, int n) {
    cd.first.pending = true; cd.first.at = cpu.cycles + delay; cd.first.intno = intno;
    cd.first.len = n; memcpy(cd.first.data, d, n);
}
static void set_second(int intno, u64 delay, const u8 *d, int n) {
    cd.second.pending = true; cd.second.at = cpu.cycles + delay; cd.second.intno = intno;
    cd.second.len = n; memcpy(cd.second.data, d, n);
}

bool g_cd_turbo;            /* boot acceleration: data reads/seeks run much faster than the real drive */
void (*g_cd_command_hook)(u8 cmd);
static u64 sector_interval(void) {
    u64 t = (cd.mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75;
    return g_cd_turbo ? t / 10 : t;
}

void cd_init(void) {
    memset(&cd, 0, sizeof cd);
    cd.mode = 0x20;
    cd.vol_ll = 0x80; cd.vol_rr = 0x80;
    cd.tmp_ll = 0x80; cd.tmp_rr = 0x80;
    cd.motor = true;
    cd.ien = 0x1f;
}

u64 cd_next_event(void) {
    u64 n = ~0ull;
    if (cd.swap_active && cd.swap_close_at < n) n = cd.swap_close_at;
    if (cd.spinup_done_at && cd.spinup_done_at < n) n = cd.spinup_done_at;
    if (cd.first.pending && cd.first.at < n) n = cd.first.at;
    if (cd.second.pending && cd.second.at < n) n = cd.second.at;
    if (cd.reading && cd.next_sector_at < n) n = cd.next_sector_at;
    if ((cd.report_pending || cd.int4_pending) && !(cd.ifl & 7)) n = cpu.cycles + 1;
    return n;
}

void cd_set_shell(bool open) {
    if (open) {
        cd.shell_open = true; cd.shell_seen_open = true;
        cd.reading = cd.playing = cd.seeking = false; cd.cdda_valid = false;
        cd.first.pending = cd.second.pending = false; cd.busy = false;
        cd.bfrd = false; cd.data_available = false;
    } else {
        cd.shell_open = false;
    }
}

void cd_insert_disc(Disc *d) {
    g_disc = d;
    cd.pos_lba = 0; cd.setloc_set = false;
    cd.reading = cd.playing = cd.seeking = false; cd.cdda_valid = false;
    cd.shell_seen_open = true;
    cd.motor = d != NULL;
}

static void load_sector_for_read(void) {
    if (!g_disc || g_disc->read_sector(g_disc, cd.pos_lba, cd.sector) != 0) memset(cd.sector, 0, sizeof cd.sector);
    if (cd.mode & 0x20) { cd.data_off = 12; cd.data_len = 2340; }
    else { cd.data_off = 24; cd.data_len = 2048; }
    cd.data_pos = 0;
    cd.data_available = true;
    cd.fifo_loaded = false;
}

static u64 seek_time(u32 target) {
    u32 d = target > cd.pos_lba ? target - cd.pos_lba : cd.pos_lba - target;
    u64 t = 150000 + (u64)d * 200;
    if (t > 3000000) t = 3000000;
    return g_cd_turbo ? t / 10 : t;
}

static int track_of_lba(u32 lba) {
    int t = 1;
    if (!g_disc) return 1;
    for (int i = 1; i <= g_disc->ntracks; i++) if (lba >= g_disc->tracks[i].start_lba) t = i;
    return t;
}

static void first_err(u8 code) {
    u8 d[2] = {(u8)(cur_stat() | ST_ERROR), code};
    set_first(5, 24000, d, 2);
}

static void stat_resp(u64 delay) {
    u8 s = cur_stat();
    cd.shell_seen_open = false;      /* the shell-open event is reported once */
    set_first(3, delay, &s, 1);
}

static void exec_command(u8 cmd) {
    const u8 *p = cd.params;
    int np = cd.nparams;
    if (g_verbose >= 2) {
        fprintf(stderr, "[cd] cmd %02x params[%d]:", cmd, np);
        for (int i = 0; i < np; i++) fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, " (cycle %llu)\n", (unsigned long long)cpu.cycles);
    }
    cd.nparams = 0;
    cd.busy = true;
    if (cd.first.pending) { /* controller busy: new command overrides */ }
    cd.second.pending = false;

    bool has_disc = g_disc && !cd.shell_open;
    if (g_cd_command_hook) g_cd_command_hook(cmd);

    switch (cmd) {
    case 0x01: /* Getstat */
        stat_resp(24000);
        break;
    case 0x02: /* Setloc */
        if (np < 3) { first_err(0x20); break; }
        cd.setloc_lba = msf_to_lba(p[0], p[1], p[2]);
        cd.setloc_set = true;
        stat_resp(24000);
        break;
    case 0x03: { /* Play */
        if (!has_disc) { first_err(0x80); break; }
        u32 target = cd.setloc_set ? cd.setloc_lba : cd.pos_lba;
        if (np >= 1 && p[0]) {
            int t = unbcd(p[0]);
            if (t >= 1 && t <= g_disc->ntracks) target = g_disc->tracks[t].start_lba;
        }
        cd.setloc_set = false;
        cd.pos_lba = target;
        cd.reading = false; cd.playing = true; cd.seeking = false;
        cd.cdda_valid = false; cd.cdda_idx = 0; cd.report_counter = 0;
        int t = track_of_lba(target);
        cd.play_end_lba = (t < g_disc->ntracks) ? g_disc->tracks[t + 1].start_lba : g_disc->total_sectors;
        cd.motor = true;
        stat_resp(30000);
        break;
    }
    case 0x04: case 0x05: /* Forward / Backward */
        stat_resp(24000);
        break;
    case 0x06: case 0x1b: { /* ReadN / ReadS */
        if (!has_disc) { first_err(0x80); break; }
        u32 target = cd.setloc_set ? cd.setloc_lba : cd.pos_lba;
        cd.setloc_set = false;
        int trk = track_of_lba(target);
        /* A data read of an audio track fails on real hardware: the drive finds no sector header and the
         * command is rejected with a seek error as the *first* response. Software (this game included)
         * uses that to tell audio CDs from discs with a data track. */
        if (g_disc->tracks[trk].audio || !g_disc->is_psx_data_disc) {
            cd.reading = false; cd.playing = false; cd.seeking = false;
            cd.pos_lba = target;
            u8 d[2] = {(u8)(cur_stat() | ST_ERROR | ST_SEEKERR), 0x04};
            set_first(5, 60000, d, 2);
            break;
        }
        cd.playing = false;
        cd.reading = true;
        cd.seeking = true;
        cd.motor = true;
        u64 st = seek_time(target);
        cd.pos_lba = target;
        cd.next_sector_at = cpu.cycles + st + sector_interval();
        stat_resp(30000);
        break;
    }
    case 0x07: /* MotorOn */
        cd.motor = true;
        stat_resp(24000);
        { u8 s = cur_stat(); set_second(2, 400000, &s, 1); }
        break;
    case 0x08: /* Stop */
        stat_resp(24000);
        cd.reading = cd.playing = cd.seeking = false; cd.motor = false;
        { u8 s = cur_stat(); set_second(2, 900000, &s, 1); }
        break;
    case 0x09: /* Pause */
        stat_resp(24000);
        { bool was = cd.reading || cd.playing;
          cd.reading = cd.playing = cd.seeking = false;
          u8 s = cur_stat();
          set_second(2, was ? 1100000 : 280000, &s, 1); }
        break;
    case 0x0a: /* Init */
        cd.mode = 0x20; cd.reading = cd.playing = cd.seeking = false; cd.motor = has_disc || g_disc;
        cd.setloc_set = false;
        stat_resp(60000);
        { u8 s = cur_stat(); set_second(2, 400000, &s, 1); }
        break;
    case 0x0b: case 0x0c: /* Mute / Demute */
        stat_resp(24000);
        break;
    case 0x0d: /* Setfilter */
        if (np >= 2) { cd.filter_file = p[0]; cd.filter_chan = p[1]; }
        stat_resp(24000);
        break;
    case 0x0e: /* Setmode */
        if (np >= 1) cd.mode = p[0];
        stat_resp(24000);
        break;
    case 0x0f: { /* Getparam */
        u8 d[5] = {cur_stat(), cd.mode, 0, cd.filter_file, cd.filter_chan};
        cd.shell_seen_open = false;
        set_first(3, 24000, d, 5);
        break;
    }
    case 0x10: { /* GetlocL */
        if (!cd.data_available && !cd.reading) { first_err(0x80); break; }
        u8 d[8];
        memcpy(d, cd.sector + 12, 4);
        d[3] = 2;
        memcpy(d + 4, cd.sector + 16, 4);
        set_first(3, 24000, d, 8);
        break;
    }
    case 0x11: { /* GetlocP */
        if (!g_disc) { first_err(0x80); break; }
        u8 d[8];
        int t = track_of_lba(cd.pos_lba);
        u32 rel = cd.pos_lba - g_disc->tracks[t].start_lba;
        u8 rm, rs, rf, am, as, af;
        u32 r150 = rel;
        rm = bcd(r150 / (75 * 60)); rs = bcd((r150 / 75) % 60); rf = bcd(r150 % 75);
        lba_to_msf(cd.pos_lba, &am, &as, &af);
        d[0] = bcd(t); d[1] = 1; d[2] = rm; d[3] = rs; d[4] = rf; d[5] = am; d[6] = as; d[7] = af;
        set_first(3, 24000, d, 8);
        break;
    }
    case 0x13: { /* GetTN */
        if (!has_disc) { first_err(0x80); break; }
        u8 d[3] = {cur_stat(), bcd(1), bcd(g_disc->ntracks)};
        cd.shell_seen_open = false;
        set_first(3, 24000, d, 3);
        break;
    }
    case 0x14: { /* GetTD */
        if (!has_disc || np < 1) { first_err(has_disc ? 0x20 : 0x80); break; }
        int t = unbcd(p[0]);
        u32 lba;
        if (t == 0) lba = g_disc->total_sectors;
        else if (t <= g_disc->ntracks) lba = g_disc->tracks[t].start_lba;
        else { first_err(0x10); break; }
        u8 m, s, f;
        lba_to_msf(lba, &m, &s, &f);
        u8 d[3] = {cur_stat(), m, s};
        cd.shell_seen_open = false;
        set_first(3, 24000, d, 3);
        break;
    }
    case 0x15: case 0x16: { /* SeekL / SeekP */
        if (!has_disc) { first_err(0x80); break; }
        u32 target = cd.setloc_set ? cd.setloc_lba : cd.pos_lba;
        cd.setloc_set = false;
        u64 st = seek_time(target);
        cd.pos_lba = target;
        cd.reading = cd.playing = false; cd.seeking = true;
        stat_resp(30000);
        cd.seeking = false;
        { u8 s = cur_stat(); set_second(2, st, &s, 1); }
        break;
    }
    case 0x19: { /* Test */
        if (np >= 1 && p[0] == 0x20) {
            u8 d[4] = {0x95, 0x07, 0x24, 0xc1};
            set_first(3, 24000, d, 4);
        } else stat_resp(24000);
        break;
    }
    case 0x1a: { /* GetID */
        if (!g_disc || cd.shell_open) {
            u8 d[2] = {(u8)(cur_stat() | ST_ERROR), 0x80};
            set_first(5, 24000, d, 2);
            break;
        }
        stat_resp(24000);
        if (g_disc->is_psx_data_disc) {
            u8 d[8] = {cur_stat(), 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E'};
            set_second(2, 40000, d, 8);
        } else {
            u8 d[8] = {(u8)(cur_stat() | ST_IDERR | ST_ERROR), 0x90, 0x00, 0x00, 0, 0, 0, 0};
            set_second(5, 40000, d, 8);
        }
        break;
    }
    case 0x1e: /* ReadTOC */
        stat_resp(24000);
        { u8 s = cur_stat(); set_second(2, 1000000, &s, 1); }
        break;
    default:
        first_err(0x40);
        break;
    }
    sched_update();
}

/* Disc swap as seen by the drive: lid opens, motor stops, a different disc is put in, lid closes,
 * the drive spins the new disc up. delay_open/closed are in CPU cycles. */
void cd_begin_swap(Disc *newd, u64 open_cycles) {
    cd_set_shell(true);
    cd.motor = false;
    g_disc = NULL;
    cd.swap_disc = newd;
    cd.swap_active = true;
    cd.swap_close_at = cpu.cycles + open_cycles;
    sched_update();
}

void cd_tick(u64 now) {
    if (cd.swap_active && now >= cd.swap_close_at) {
        cd.swap_active = false;
        g_disc = cd.swap_disc;
        cd.pos_lba = 0; cd.setloc_set = false; cd.cdda_valid = false;
        cd.shell_open = false;
        cd.shell_seen_open = true;
        cd.motor = false;
        cd.spinup_done_at = now + CPU_HZ * 2;      /* ~2 s of spin-up */
    }
    if (cd.spinup_done_at && now >= cd.spinup_done_at) { cd.spinup_done_at = 0; cd.motor = g_disc != NULL; }
    for (int guard = 0; guard < 8; guard++) {
        bool progressed = false;
        if (cd.first.pending && now >= cd.first.at && !(cd.ifl & 7)) {
            deliver(&cd.first); progressed = true;
        } else if (!cd.first.pending && cd.second.pending && now >= cd.second.at && !(cd.ifl & 7)) {
            deliver(&cd.second); progressed = true;
        } else if (cd.int4_pending && !(cd.ifl & 7) && !cd.first.pending) {
            u8 s = cur_stat();
            Resp r = {.pending = true, .intno = 4, .len = 1}; r.data[0] = s;
            cd.int4_pending = false;
            deliver(&r); progressed = true;
        } else if (cd.report_pending && !(cd.ifl & 7) && !cd.first.pending && cd.playing) {
            u8 d[8];
            int t = track_of_lba(cd.pos_lba);
            u8 m, s, f;
            lba_to_msf(cd.pos_lba, &m, &s, &f);
            d[0] = cur_stat(); d[1] = bcd(t); d[2] = 1; d[3] = m; d[4] = s; d[5] = f;
            d[6] = (u8)(cd.report_peak[0]); d[7] = (u8)(cd.report_peak[1]);
            Resp r = {.pending = true, .intno = 1, .len = 8};
            memcpy(r.data, d, 8);
            cd.report_pending = false;
            deliver(&r); progressed = true;
        }
        if (cd.reading && now >= cd.next_sector_at) {
            if (!(cd.ifl & 7) && !cd.first.pending) {
                cd.seeking = false;
                load_sector_for_read();
                u8 s = cur_stat();
                Resp r = {.pending = true, .intno = 1, .len = 1}; r.data[0] = s;
                deliver(&r);
                cd.pos_lba++;
                cd.next_sector_at += sector_interval();
                if (cd.next_sector_at < now) cd.next_sector_at = now + sector_interval();
                progressed = true;
            } else {
                cd.next_sector_at = now + 2000;
            }
        }
        if (!progressed) break;
    }
}

/* ---- CD-DA playback (driven by the SPU's 44.1 kHz clock) ---- */
void cd_get_cdda(s16 *l, s16 *r) {
    *l = *r = 0;
    if (!cd.playing || !g_disc || cd.shell_open) return;
    if (!cd.cdda_valid || cd.cdda_idx >= 588) {
        if (cd.cdda_valid) {
            cd.pos_lba++;
            /* report: peak of the sector just finished */
            if ((cd.mode & 4) && ++cd.report_counter >= 10) {
                cd.report_counter = 0;
                cd.report_peak[0] = (u8)(cd.peak_l & 0xff); cd.report_peak[1] = (u8)((cd.peak_l >> 8) & 0xff);
                cd.report_pending = true;
            }
            cd.peak_l = cd.peak_r = 0;
        }
        if (cd.pos_lba >= cd.play_end_lba && ((cd.mode & 2) || cd.pos_lba >= g_disc->total_sectors)) {
            cd.playing = false; cd.cdda_valid = false; cd.int4_pending = true;
            return;
        }
        if (g_disc->read_sector(g_disc, cd.pos_lba, cd.cdda_sector) != 0) memset(cd.cdda_sector, 0, sizeof cd.cdda_sector);
        cd.cdda_idx = 0;
        cd.cdda_valid = true;
    }
    const s16 *smp = (const s16 *)cd.cdda_sector;
    int a = smp[cd.cdda_idx * 2], b = smp[cd.cdda_idx * 2 + 1];
    cd.cdda_idx++;
    int aa = a < 0 ? -a : a;
    if (aa > cd.peak_l) cd.peak_l = aa;
    int ol = (a * cd.vol_ll + b * cd.vol_rl) >> 7;
    int or_ = (b * cd.vol_rr + a * cd.vol_lr) >> 7;
    if (ol > 32767) ol = 32767; else if (ol < -32768) ol = -32768;
    if (or_ > 32767) or_ = 32767; else if (or_ < -32768) or_ = -32768;
    *l = (s16)ol; *r = (s16)or_;
}

/* ---- register interface ---- */
u32 cd_read(u32 off, int size) {
    switch (off) {
    case 0: {
        u32 v = cd.index;
        v |= (cd.nparams == 0) << 3;
        v |= (cd.nparams < 16) << 4;
        v |= (cd.resp_pos < cd.resp_len) << 5;
        v |= ((cd.bfrd && cd.data_available && cd.data_pos < cd.data_len) ? 1u : 0u) << 6;
        v |= (cd.busy ? 1u : 0u) << 7;
        return v;
    }
    case 1: {
        if (cd.resp_pos < cd.resp_len) return cd.resp[cd.resp_pos++];
        return 0;
    }
    case 2: {
        u32 v = 0;
        for (int i = 0; i < size; i++) {
            u8 b = 0;
            if (cd.data_available) {
                u32 o = cd.data_off + cd.data_pos;
                b = o < 2352 ? cd.sector[o] : 0;
                if (cd.data_pos < cd.data_len) cd.data_pos++;
            }
            v |= (u32)b << (8 * i);
        }
        return v;
    }
    case 3:
        if (cd.index & 1) return cd.ifl | 0xe0;
        return cd.ien | 0xe0;
    }
    return 0;
}

void cd_write(u32 off, u32 v) {
    v &= 0xff;
    if (off == 0) { cd.index = v & 3; return; }
    switch (cd.index * 4 + (off - 1)) {
    case 0: /* idx0 reg1: command */
        exec_command((u8)v);
        break;
    case 1: /* idx0 reg2: parameter */
        if (cd.nparams < 16) cd.params[cd.nparams++] = (u8)v;
        break;
    case 2: /* idx0 reg3: request */
        cd.bfrd = (v >> 7) & 1;
        if (cd.bfrd) {
            if (!cd.fifo_loaded) { cd.data_pos = 0; cd.fifo_loaded = true; }
        } else {
            cd.fifo_loaded = false;
        }
        break;
    case 4: break;                             /* idx1 reg1: sound map data */
    case 5: cd.ien = v & 0x1f; check_irq(); break;
    case 6:                                    /* idx1 reg3: interrupt flag ack */
        cd.ifl &= ~(v & 0x1f);
        if (v & 0x40) cd.nparams = 0;
        check_irq();
        sched_update();
        break;
    case 8: break;                             /* idx2 reg1: coding info */
    case 9: cd.tmp_ll = (u8)v; break;
    case 10: cd.tmp_lr = (u8)v; break;
    case 12: cd.tmp_rr = (u8)v; break;         /* idx3 reg1: R->R */
    case 13: cd.tmp_rl = (u8)v; break;
    case 14:
        if (g_verbose >= 3) fprintf(stderr, "[cd] vol apply: ll=%02x lr=%02x rl=%02x rr=%02x mute=%d (cyc %llu)\n", cd.tmp_ll, cd.tmp_lr, cd.tmp_rl, cd.tmp_rr, v & 1, (unsigned long long)cpu.cycles);
        cd.adpmute = v & 1;
        if (v & 0x20) { cd.vol_ll = cd.tmp_ll; cd.vol_lr = cd.tmp_lr; cd.vol_rl = cd.tmp_rl; cd.vol_rr = cd.tmp_rr; }
        break;
    default: break;
    }
}

void cd_dma_read(u8 *dst, u32 bytes) {
    if (g_verbose >= 2) fprintf(stderr, "[cd] dma read %u bytes, data_pos=%u avail=%d off=%u bfrd=%d first=%02x%02x%02x%02x\n", bytes, cd.data_pos, cd.data_available, cd.data_off, cd.bfrd, cd.sector[cd.data_off], cd.sector[cd.data_off+1], cd.sector[cd.data_off+2], cd.sector[cd.data_off+3]);
    for (u32 i = 0; i < bytes; i++) {
        u8 b = 0;
        if (cd.data_available) {
            u32 o = cd.data_off + cd.data_pos;
            b = o < 2352 ? cd.sector[o] : 0;
            if (cd.data_pos < cd.data_len) cd.data_pos++;
        }
        dst[i] = b;
    }
}

u32 cd_data_fifo_pop32(void) { return cd_read(2, 4); }
