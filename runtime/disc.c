/* Disc images: CUE/BIN (multi-bin), synthetic audio CDs from PCM, ISO9660 reader. */
#include "psx.h"
#include <stdlib.h>
#include <ctype.h>

Disc *g_disc;
Disc *g_game_disc;

/* ------------------------------------------------------------ CUE / BIN */
typedef struct {
    FILE *f;
    char path[1024];
    u32 start_lba;      /* absolute LBA (relative to disc start = MSF 00:02:00) of first sector in file */
    u32 sectors;
    u32 sector_size;    /* 2352 */
} BinFile;

typedef struct {
    BinFile files[MAX_TRACKS + 1];
    int nfiles;
} CuePriv;

static int cue_read_sector(Disc *d, u32 lba, u8 *out) {
    CuePriv *p = d->priv;
    for (int i = 0; i < p->nfiles; i++) {
        BinFile *b = &p->files[i];
        if (lba >= b->start_lba && lba < b->start_lba + b->sectors) {
            if (fseeko(b->f, (off_t)(lba - b->start_lba) * b->sector_size, SEEK_SET)) return -1;
            return fread(out, 1, 2352, b->f) == 2352 ? 0 : -1;
        }
    }
    memset(out, 0, 2352);
    return -1;
}

static int msf_to_frames(const char *s) {
    int m = 0, sec = 0, f = 0;
    if (sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3) return -1;
    return (m * 60 + sec) * 75 + f;
}

static void dirname_of(const char *path, char *out, size_t n) {
    strncpy(out, path, n - 1);
    out[n - 1] = 0;
    char *sl = strrchr(out, '/');
    if (sl) sl[1] = 0; else out[0] = 0;
}

Disc *disc_open_cue(const char *cue_path) {
    FILE *f = fopen(cue_path, "r");
    if (!f) return NULL;
    Disc *d = calloc(1, sizeof *d);
    CuePriv *p = calloc(1, sizeof *p);
    d->priv = p;
    d->read_sector = cue_read_sector;
    char dir[1024];
    dirname_of(cue_path, dir, sizeof dir);

    char line[2048];
    int cur_file = -1;
    u32 next_file_lba = 0;
    int cur_track = 0;
    u32 idx1_in_file[MAX_TRACKS + 1] = {0};
    int track_file[MAX_TRACKS + 1] = {0};

    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (!strncasecmp(s, "FILE", 4)) {
            char *q1 = strchr(s, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            *q2 = 0;
            BinFile *b = &p->files[p->nfiles];
            snprintf(b->path, sizeof b->path, "%s%s", dir, q1 + 1);
            b->f = fopen(b->path, "rb");
            if (!b->f) { WARN("cue: cannot open %s\n", b->path); disc_free(d); fclose(f); return NULL; }
            fseeko(b->f, 0, SEEK_END);
            off_t sz = ftello(b->f);
            b->sector_size = 2352;
            b->sectors = (u32)(sz / 2352);
            b->start_lba = next_file_lba;
            next_file_lba += b->sectors;
            cur_file = p->nfiles++;
        } else if (!strncasecmp(s, "TRACK", 5)) {
            int num = 0; char type[32] = {0};
            sscanf(s + 5, "%d %31s", &num, type);
            cur_track = num;
            if (num < 1 || num > MAX_TRACKS) continue;
            d->tracks[num].number = num;
            d->tracks[num].audio = !strncasecmp(type, "AUDIO", 5);
            track_file[num] = cur_file;
            if (num > d->ntracks) d->ntracks = num;
        } else if (!strncasecmp(s, "INDEX", 5)) {
            int idx = 0; char msf[32] = {0};
            sscanf(s + 5, "%d %31s", &idx, msf);
            if (idx == 1 && cur_track >= 1) idx1_in_file[cur_track] = (u32)msf_to_frames(msf);
        }
    }
    fclose(f);
    for (int t = 1; t <= d->ntracks; t++) {
        BinFile *b = &p->files[track_file[t]];
        d->tracks[t].start_lba = b->start_lba + idx1_in_file[t];
    }
    for (int t = 1; t <= d->ntracks; t++) {
        u32 end = (t < d->ntracks) ? d->tracks[t + 1].start_lba : next_file_lba;
        d->tracks[t].length = end - d->tracks[t].start_lba;
    }
    d->total_sectors = next_file_lba;
    d->is_psx_data_disc = d->ntracks >= 1 && !d->tracks[1].audio;
    snprintf(d->label, sizeof d->label, "%s", cue_path);
    return d;
}

/* ------------------------------------------------------------ synthetic audio CD */
typedef struct {
    s16 *pcm;           /* interleaved stereo */
    size_t frames;
    bool owned;
} PcmPriv;

static int pcm_read_sector(Disc *d, u32 lba, u8 *out) {
    PcmPriv *p = d->priv;
    memset(out, 0, 2352);
    size_t first = (size_t)lba * 588;
    if (first >= p->frames) return 0;
    size_t n = p->frames - first;
    if (n > 588) n = 588;
    memcpy(out, p->pcm + first * 2, n * 4);
    return 0;
}

Disc *disc_open_audio_pcm(const s16 *interleaved, size_t nframes, const char *label) {
    Disc *d = calloc(1, sizeof *d);
    PcmPriv *p = calloc(1, sizeof *p);
    /* pad so the track is at least a few seconds and a whole number of sectors */
    size_t sectors = (nframes + 587) / 588;
    if (sectors < 4 * 75) sectors = 4 * 75;
    p->frames = sectors * 588;
    p->pcm = calloc(p->frames, 4);
    memcpy(p->pcm, interleaved, nframes * 4);
    p->owned = true;
    d->priv = p;
    d->read_sector = pcm_read_sector;
    d->ntracks = 1;
    d->tracks[1].number = 1;
    d->tracks[1].audio = true;
    d->tracks[1].start_lba = 0;
    d->tracks[1].length = (u32)sectors;
    d->total_sectors = (u32)sectors;
    snprintf(d->label, sizeof d->label, "%s", label ? label : "audio");
    return d;
}

void disc_free(Disc *d) {
    if (!d) return;
    if (d->destroy) {
        d->destroy(d);
    } else if (d->read_sector == cue_read_sector) {
        CuePriv *p = d->priv;
        for (int i = 0; i < p->nfiles; i++) if (p->files[i].f) fclose(p->files[i].f);
        free(p);
    } else if (d->read_sector == pcm_read_sector) {
        PcmPriv *p = d->priv;
        free(p->pcm);
        free(p);
    }
    free(d);
}

/* ------------------------------------------------------------ ISO9660 */
static bool disc_read_user(Disc *d, u32 lba, u8 *out2048) {
    u8 raw[2352];
    if (d->read_sector(d, lba, raw) != 0) return false;
    int off = (raw[15] == 2) ? 24 : 16;
    memcpy(out2048, raw + off, 2048);
    return true;
}

static bool iso_lookup_in_dir(Disc *d, u32 dir_lba, u32 dir_size, const char *name, IsoFile *out, bool want_dir) {
    u8 sec[2048];
    for (u32 off = 0; off < dir_size; off += 2048) {
        if (!disc_read_user(d, dir_lba + off / 2048, sec)) return false;
        u32 i = 0;
        while (i < 2048) {
            u32 len = sec[i];
            if (!len) break;
            u32 nlen = sec[i + 32];
            char nm[64] = {0};
            memcpy(nm, sec + i + 33, nlen < 63 ? nlen : 63);
            char *semi = strchr(nm, ';');
            if (semi) *semi = 0;
            bool is_dir = sec[i + 25] & 2;
            if (!strcasecmp(nm, name) && is_dir == want_dir) {
                out->lba = sec[i + 2] | (sec[i + 3] << 8) | (sec[i + 4] << 16) | ((u32)sec[i + 5] << 24);
                out->size = sec[i + 10] | (sec[i + 11] << 8) | (sec[i + 12] << 16) | ((u32)sec[i + 13] << 24);
                snprintf(out->name, sizeof out->name, "%s", nm);
                return true;
            }
            i += len;
        }
    }
    return false;
}

bool iso_find(Disc *d, const char *path, IsoFile *out) {
    u8 pvd[2048];
    if (!disc_read_user(d, 16, pvd) || memcmp(pvd + 1, "CD001", 5)) return false;
    u32 lba = pvd[158] | (pvd[159] << 8) | (pvd[160] << 16) | ((u32)pvd[161] << 24);
    u32 size = pvd[166] | (pvd[167] << 8) | (pvd[168] << 16) | ((u32)pvd[169] << 24);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "/\\", &save); tok; ) {
        char *next = strtok_r(NULL, "/\\", &save);
        IsoFile f;
        if (!iso_lookup_in_dir(d, lba, size, tok, &f, next != NULL)) return false;
        if (!next) { *out = f; return true; }
        lba = f.lba; size = f.size;
        tok = next;
    }
    return false;
}

u8 *iso_read_file(Disc *d, const IsoFile *f) {
    u8 *buf = malloc(((f->size + 2047) / 2048) * 2048);
    for (u32 s = 0; s < (f->size + 2047) / 2048; s++) {
        if (!disc_read_user(d, f->lba + s, buf + s * 2048)) { free(buf); return NULL; }
    }
    return buf;
}

bool disc_boot_name(Disc *d, char *out, size_t n) {
    IsoFile f;
    if (!iso_find(d, "SYSTEM.CNF", &f) || f.size > 4096) return false;
    u8 *cnf = iso_read_file(d, &f);
    if (!cnf) return false;
    char text[4097];
    memcpy(text, cnf, f.size);
    text[f.size] = 0;
    free(cnf);
    char *p = strstr(text, "BOOT");
    if (p) p = strstr(p, "cdrom:");
    if (!p) return false;
    p += 6;
    while (*p == '\\' || *p == '/') p++;
    size_t i = 0;
    while (*p && *p != ';' && *p != '\r' && *p != '\n' && *p != ' ' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}
