/* PS1 GPU: command processor, software rasterizer, display timing. */
#include "psx.h"
#include <stdlib.h>

u16 vram[1024 * 512];

/* ---- state ---- */
static u32 stat_texpage;      /* GPUSTAT bits 0..10 (E1 bits 0..10) */
static bool tex_disable;
static bool flip_x, flip_y;
static u32 tw_mask_x, tw_mask_y, tw_off_x, tw_off_y, reg_e2;
static int da_x1, da_y1, da_x2, da_y2;     /* drawing area (inclusive) */
static u32 reg_e3, reg_e4, reg_e5;
static int off_x, off_y;
static bool set_mask, check_mask;
static bool irq1_flag;

static bool disp_disabled = true;
static u32 dma_dir;
static int disp_x, disp_y;                  /* display start in VRAM */
static u32 hrange, vrange;                  /* GP1(06), GP1(07) raw */
static u32 disp_mode;                       /* GP1(08) raw */

/* command fifo */
enum { M_CMD, M_POLYLINE, M_VWRITE, M_VREAD };
static int gp0_mode = M_CMD;
static u32 gp0_buf[16];
static int gp0_pos, gp0_need;
static u32 gp0_cmd;
static u32 gpuread_latch;

/* polyline state */
enum { PL_V0, PL_VN, PL_C, PL_V };
static struct { bool gouraud, semi; u32 color, pcolor, pending_color; int px, py; int state; } pl;

/* vram transfer state */
static struct { int x, y, w, h, cx, cy; u32 remaining_px; } vt;
static struct { int x, y, w, h, cx, cy; } vr;

static inline int sext11(int v) { return (v << 21) >> 21; }

/* ---- display timing ---- */
static bool region_pal = true;
static u64 frame_start;         /* cycle count at start of current frame */
static bool in_vblank;
static u64 next_gpu_event;

void gpu_set_region_pal(bool pal) { region_pal = pal; }

static bool mode_pal(void) { return (disp_mode >> 3) & 1; }
u64 gpu_frame_cycles(void) { return mode_pal() ? CPU_HZ / 50 : (u64)(CPU_HZ / 59.94); }
static int frame_lines(void) { return mode_pal() ? 314 : 263; }
static int vblank_line(void) { return mode_pal() ? 256 : 240; }
u64 gpu_line_cycles_x16(void) { return gpu_frame_cycles() * 16 / frame_lines(); }
u32 gpu_dot_divider(void) {
    if ((disp_mode >> 6) & 1) return 7;
    switch (disp_mode & 3) { case 0: return 10; case 1: return 8; case 2: return 5; default: return 4; }
}

static int current_line(u64 now) {
    u64 fc = gpu_frame_cycles();
    u64 pos = (now - frame_start);
    if (pos >= fc) pos %= fc;
    return (int)(pos * frame_lines() / fc);
}

u64 gpu_next_event(void) { return next_gpu_event; }

void gpu_tick(u64 now) {
    u64 fc = gpu_frame_cycles();
    for (;;) {
        u64 vb_at = frame_start + fc * vblank_line() / frame_lines();
        u64 end_at = frame_start + fc;
        if (!in_vblank) {
            if (now >= vb_at) {
                in_vblank = true;
                irq_raise(IRQ_VBLANK);
                timers_set_blank(0, 1);
                frontend_present();
                frontend_pace();
                continue;
            }
            next_gpu_event = vb_at;
            break;
        } else {
            if (now >= end_at) {
                in_vblank = false;
                frame_start = end_at;
                timers_set_blank(0, 0);
                continue;
            }
            next_gpu_event = end_at;
            break;
        }
    }
}

void gpu_get_display(DisplayInfo *d) {
    static const int hres[4] = {256, 320, 512, 640};
    d->enabled = !disp_disabled;
    d->depth24 = (disp_mode >> 4) & 1;
    d->pal = mode_pal();
    int w = ((disp_mode >> 6) & 1) ? 368 : hres[disp_mode & 3];
    int h = ((disp_mode >> 2) & 1) ? (mode_pal() ? 512 : 480) : (mode_pal() ? 256 : 240);
    /* honour vertical display range if it narrows the picture */
    int y1 = vrange & 0x3ff, y2 = (vrange >> 10) & 0x3ff;
    if (y2 > y1) {
        int rh = y2 - y1;
        if (((disp_mode >> 2) & 1)) rh *= 2;
        if (rh >= 64 && rh < h) h = rh;
    }
    d->x = disp_x; d->y = disp_y; d->w = w; d->h = h;
}

/* ---- pixel helpers ---- */
static inline u16 rgb8to15(u32 r, u32 g, u32 b) { return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }

static const s8 dither_tab[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline u16 blend_semi(u16 back, int r5, int g5, int b5, int mode) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int rr, gg, bb2;
    switch (mode) {
    case 0: rr = (br + r5) >> 1; gg = (bg + g5) >> 1; bb2 = (bb + b5) >> 1; break;
    case 1: rr = br + r5; gg = bg + g5; bb2 = bb + b5; break;
    case 2: rr = br - r5; gg = bg - g5; bb2 = bb - b5; break;
    default: rr = br + (r5 >> 2); gg = bg + (g5 >> 2); bb2 = bb + (b5 >> 2); break;
    }
    return (u16)(clampi(rr, 0, 31) | (clampi(gg, 0, 31) << 5) | (clampi(bb2, 0, 31) << 10));
}

/* write one pixel honouring drawing area + mask + (optional) semi transparency. col is 8-bit r,g,b */
static inline void put_pixel(int x, int y, int r8, int g8, int b8, bool semi, bool dither, int mask_bit_from_tex) {
    if (x < da_x1 || x > da_x2 || y < da_y1 || y > da_y2) return;
    u16 *p = &vram[(y & 511) * 1024 + (x & 1023)];
    if (check_mask && (*p & 0x8000)) return;
    if (dither) {
        int d = dither_tab[y & 3][x & 3];
        r8 = clampi(r8 + d, 0, 255); g8 = clampi(g8 + d, 0, 255); b8 = clampi(b8 + d, 0, 255);
    }
    int r5 = r8 >> 3, g5 = g8 >> 3, b5 = b8 >> 3;
    u16 out;
    if (semi) out = blend_semi(*p, r5, g5, b5, (stat_texpage >> 5) & 3);
    else out = (u16)(r5 | (g5 << 5) | (b5 << 10));
    if (set_mask || mask_bit_from_tex) out |= 0x8000;
    *p = out;
}

/* ---- texture fetch ---- */
typedef struct {
    int base_x, base_y, depth;   /* depth: 0=4bpp 1=8bpp 2=15bpp */
    int clut_x, clut_y;
} TexCtx;

static inline u16 tex_fetch(const TexCtx *t, int u, int v) {
    u = (u & ~(tw_mask_x * 8)) | ((tw_off_x & tw_mask_x) * 8);
    v = (v & ~(tw_mask_y * 8)) | ((tw_off_y & tw_mask_y) * 8);
    u &= 255; v &= 255;
    int vy = (t->base_y + v) & 511;
    switch (t->depth) {
    case 0: {
        u16 w = vram[vy * 1024 + ((t->base_x + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        return vram[(t->clut_y & 511) * 1024 + ((t->clut_x + idx) & 1023)];
    }
    case 1: {
        u16 w = vram[vy * 1024 + ((t->base_x + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        return vram[(t->clut_y & 511) * 1024 + ((t->clut_x + idx) & 1023)];
    }
    default:
        return vram[vy * 1024 + ((t->base_x + u) & 1023)];
    }
}

static void make_tex(TexCtx *t, u32 texpage, u32 clut) {
    t->base_x = (texpage & 15) * 64;
    t->base_y = ((texpage >> 4) & 1) * 256;
    int d = (texpage >> 7) & 3;
    t->depth = d > 2 ? 2 : d;
    t->clut_x = (clut & 0x3f) * 16;
    t->clut_y = (clut >> 6) & 0x1ff;
}

/* ---- triangle rasterizer ---- */
typedef struct { int x, y; int r, g, b; int u, v; } Vtx;

typedef struct {
    bool gouraud, textured, semi, raw;
    TexCtx tex;
} PrimCtx;

static inline s64 edge(const Vtx *a, const Vtx *b, int px, int py) {
    return (s64)(b->x - a->x) * (py - a->y) - (s64)(b->y - a->y) * (px - a->x);
}

static void draw_triangle(Vtx *v0, Vtx *v1, Vtx *v2, const PrimCtx *pc) {
    /* reject too-large primitives like hardware */
    int minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    Vtx *vs[3] = {v0, v1, v2};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    s64 area = edge(v0, v1, v2->x, v2->y);
    if (area == 0) return;
    if (area < 0) { Vtx *t = v1; v1 = v2; v2 = t; area = -area; }
    if (minx < da_x1) minx = da_x1;
    if (maxx > da_x2) maxx = da_x2;
    if (miny < da_y1) miny = da_y1;
    if (maxy > da_y2) maxy = da_y2;
    if (minx > maxx || miny > maxy) return;

    /* top-left rule bias per edge (CCW in screen space with y down => our 'area>0' orientation) */
    #define TL(a, b) (((b)->y > (a)->y) || (((b)->y == (a)->y) && ((b)->x < (a)->x)) ? 0 : -1)
    int bias0 = TL(v1, v2), bias1 = TL(v2, v0), bias2 = TL(v0, v1);
    #undef TL
    /* Our edge() > 0 inside for area>0. Edge with bias -1 excludes exact-zero samples. */

    bool dither = ((stat_texpage >> 9) & 1) && (pc->gouraud || (pc->textured && !pc->raw));
    double inv_area = 1.0 / (double)area;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            s64 w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double a0 = w0 * inv_area, a1 = w1 * inv_area, a2 = w2 * inv_area;
            int r, g, b;
            if (pc->gouraud) {
                r = (int)(v0->r * a0 + v1->r * a1 + v2->r * a2 + 0.5);
                g = (int)(v0->g * a0 + v1->g * a1 + v2->g * a2 + 0.5);
                b = (int)(v0->b * a0 + v1->b * a1 + v2->b * a2 + 0.5);
                r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
            } else { r = v0->r; g = v0->g; b = v0->b; }
            if (pc->textured) {
                int u = (int)(v0->u * a0 + v1->u * a1 + v2->u * a2 + 0.0001);
                int vv = (int)(v0->v * a0 + v1->v * a1 + v2->v * a2 + 0.0001);
                u16 t = tex_fetch(&pc->tex, u, vv);
                if (t == 0) continue;
                int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
                if (!pc->raw) {
                    tr = clampi((tr * r) >> 7, 0, 255);
                    tg = clampi((tg * g) >> 7, 0, 255);
                    tb = clampi((tb * b) >> 7, 0, 255);
                }
                bool semi = pc->semi && (t & 0x8000);
                put_pixel(x, y, tr, tg, tb, semi, dither, 0);
            } else {
                put_pixel(x, y, r, g, b, pc->semi, dither, 0);
            }
        }
    }
}

/* ---- lines ---- */
int g_gpu_log_frame = -1;     /* dump line primitives when frame counter matches */
int g_gpu_frame_ctr;
static void draw_line(const Vtx *a, const Vtx *b, bool gouraud, bool semi) {
    if (g_gpu_log_frame >= 0 && g_gpu_frame_ctr == g_gpu_log_frame)
        fprintf(stderr, "[gpu] line (%d,%d)-(%d,%d) c=%02x%02x%02x g=%d semi=%d\n", a->x, a->y, b->x, b->y, a->r, a->g, a->b, gouraud, semi);
    int dx = b->x - a->x, dy = b->y - a->y;
    if (abs(dx) >= 1024 || abs(dy) >= 512) return;
    int adx = abs(dx), ady = abs(dy);
    int steps = adx > ady ? adx : ady;
    bool dither = ((stat_texpage >> 9) & 1) && gouraud;
    if (steps == 0) {
        put_pixel(a->x, a->y, a->r, a->g, a->b, semi, dither, 0);
        return;
    }
    /* 16.16 fixed DDA with half-pixel start on the minor axis */
    s64 xf = ((s64)a->x << 16) + 0x8000, yf = ((s64)a->y << 16) + 0x8000;
    s64 xs = ((s64)dx << 16) / steps, ys = ((s64)dy << 16) / steps;
    s64 rf = ((s64)a->r << 16) + 0x8000, gf = ((s64)a->g << 16) + 0x8000, bf = ((s64)a->b << 16) + 0x8000;
    s64 rs = ((s64)(b->r - a->r) << 16) / steps, gs = ((s64)(b->g - a->g) << 16) / steps, bs = ((s64)(b->b - a->b) << 16) / steps;
    for (int i = 0; i <= steps; i++) {
        int px = (int)(xf >> 16), py = (int)(yf >> 16);
        if (adx >= ady) px = a->x + (dx < 0 ? -i : i);
        else py = a->y + (dy < 0 ? -i : i);
        int r = gouraud ? (int)(rf >> 16) : a->r, g = gouraud ? (int)(gf >> 16) : a->g, bl = gouraud ? (int)(bf >> 16) : a->b;
        put_pixel(px, py, clampi(r, 0, 255), clampi(g, 0, 255), clampi(bl, 0, 255), semi, dither, 0);
        xf += xs; yf += ys; rf += rs; gf += gs; bf += bs;
    }
}

/* ---- rectangles ---- */
static void draw_rect(int x0, int y0, int w, int h, u32 color, bool textured, bool raw, bool semi, u32 clut, int u0, int v0) {
    int r = color & 255, g = (color >> 8) & 255, b = (color >> 16) & 255;
    TexCtx t;
    if (textured) make_tex(&t, stat_texpage, clut);
    bool dither = false;
    for (int dy = 0; dy < h; dy++) {
        int y = y0 + dy;
        if (y < da_y1 || y > da_y2) continue;
        for (int dx = 0; dx < w; dx++) {
            int x = x0 + dx;
            if (x < da_x1 || x > da_x2) continue;
            if (textured) {
                int u = flip_x ? (u0 - dx) : (u0 + dx);
                int v = flip_y ? (v0 - dy) : (v0 + dy);
                u16 tx = tex_fetch(&t, u, v);
                if (tx == 0) continue;
                int tr = (tx & 31) << 3, tg = ((tx >> 5) & 31) << 3, tb = ((tx >> 10) & 31) << 3;
                if (!raw) {
                    tr = clampi((tr * r) >> 7, 0, 255);
                    tg = clampi((tg * g) >> 7, 0, 255);
                    tb = clampi((tb * b) >> 7, 0, 255);
                }
                put_pixel(x, y, tr, tg, tb, semi && (tx & 0x8000), dither, 0);
            } else {
                put_pixel(x, y, r, g, b, semi, dither, 0);
            }
        }
    }
}

/* ---- VRAM transfers ---- */
static void vram_fill(u32 color, u32 xy, u32 wh) {
    int x = xy & 0x3f0, y = (xy >> 16) & 0x1ff;
    int w = (((wh & 0x3ff) + 0xf) & ~0xf), h = (wh >> 16) & 0x1ff;
    u16 c = (u16)(((color & 0xff) >> 3) | (((color >> 8) & 0xff) >> 3 << 5) | (((color >> 16) & 0xff) >> 3 << 10));
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            vram[((y + dy) & 511) * 1024 + ((x + dx) & 1023)] = c;
}

static void vram_copy(u32 src, u32 dst, u32 wh) {
    int sx = src & 0x3ff, sy = (src >> 16) & 0x1ff, dx0 = dst & 0x3ff, dy0 = (dst >> 16) & 0x1ff;
    int w = wh & 0x3ff, h = (wh >> 16) & 0x1ff;
    if (!w) w = 0x400;
    if (!h) h = 0x200;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u16 px = vram[((sy + y) & 511) * 1024 + ((sx + x) & 1023)];
            u16 *d = &vram[((dy0 + y) & 511) * 1024 + ((dx0 + x) & 1023)];
            if (check_mask && (*d & 0x8000)) continue;
            *d = px | (set_mask ? 0x8000 : 0);
        }
}

static inline void vwrite_pixel(u16 px) {
    u16 *d = &vram[(vt.cy & 511) * 1024 + (vt.cx & 1023)];
    if (!(check_mask && (*d & 0x8000))) *d = px | (set_mask ? 0x8000 : 0);
    if (++vt.cx >= vt.x + vt.w) { vt.cx = vt.x; vt.cy++; }
}

/* ---- command execution ---- */
static inline void vtx_xy(Vtx *v, u32 w) {
    v->x = sext11((int)(w & 0x7ff)) + off_x;
    v->y = sext11((int)((w >> 16) & 0x7ff)) + off_y;
}
static inline void vtx_col(Vtx *v, u32 w) { v->r = w & 255; v->g = (w >> 8) & 255; v->b = (w >> 16) & 255; }

static void exec_polygon(void) {
    u32 cmd = gp0_buf[0] >> 24;
    bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
    int n = quad ? 4 : 3;
    Vtx v[4];
    int pos = 0;
    u32 clut = 0, page = 0;
    for (int i = 0; i < n; i++) {
        u32 cw = (i == 0 || gouraud) ? gp0_buf[pos++] : gp0_buf[0];
        vtx_col(&v[i], cw);
        vtx_xy(&v[i], gp0_buf[pos++]);
        if (textured) {
            u32 uv = gp0_buf[pos++];
            v[i].u = uv & 255; v[i].v = (uv >> 8) & 255;
            if (i == 0) clut = uv >> 16;
            if (i == 1) page = uv >> 16;
        }
    }
    PrimCtx pc = {gouraud, textured, semi, raw, {0}};
    if (textured) {
        /* texpage attribute updates GPUSTAT texpage bits 0..8 */
        stat_texpage = (stat_texpage & ~0x1ffu) | (page & 0x1ff);
        make_tex(&pc.tex, page, clut);
        if (tex_disable) { /* texture disable bit only honoured for rects in hw; ignore */ }
    }
    draw_triangle(&v[0], &v[1], &v[2], &pc);
    if (quad) draw_triangle(&v[1], &v[2], &v[3], &pc);
}

static void exec_rect(void) {
    u32 cmd = gp0_buf[0] >> 24;
    bool textured = cmd & 4, semi = cmd & 2, raw = cmd & 1;
    int sizeSel = (cmd >> 3) & 3;
    int pos = 1;
    u32 xy = gp0_buf[pos++];
    u32 clut = 0; int u = 0, v = 0;
    if (textured) { u32 uv = gp0_buf[pos++]; u = uv & 255; v = (uv >> 8) & 255; clut = uv >> 16; }
    int w, h;
    switch (sizeSel) {
    case 0: { u32 wh = gp0_buf[pos++]; w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; break; }
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    int x = sext11((int)(xy & 0x7ff)) + off_x, y = sext11((int)((xy >> 16) & 0x7ff)) + off_y;
    draw_rect(x, y, w, h, gp0_buf[0], textured, raw, semi, clut, u, v);
}

static void exec_line_simple(void) {
    u32 cmd = gp0_buf[0] >> 24;
    bool gouraud = cmd & 0x10, semi = cmd & 2;
    Vtx a, b;
    vtx_col(&a, gp0_buf[0]);
    vtx_xy(&a, gp0_buf[1]);
    if (gouraud) { vtx_col(&b, gp0_buf[2]); vtx_xy(&b, gp0_buf[3]); }
    else { b.r = a.r; b.g = a.g; b.b = a.b; vtx_xy(&b, gp0_buf[2]); }
    draw_line(&a, &b, gouraud, semi);
}

static int cmd_words(u32 cmd, bool *polyline) {
    *polyline = false;
    if (cmd >= 0x20 && cmd <= 0x3f) {
        bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04;
        int n = quad ? 4 : 3;
        return 1 + n * (1 + (textured ? 1 : 0)) + (gouraud ? n - 1 : 0);
    }
    if (cmd >= 0x40 && cmd <= 0x5f) {
        if (cmd & 0x08) { *polyline = true; return 0; }
        return (cmd & 0x10) ? 4 : 3;
    }
    if (cmd >= 0x60 && cmd <= 0x7f) {
        return 2 + ((cmd & 4) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
    }
    switch (cmd) {
    case 0x02: return 3;
    case 0x80 ... 0x9f: return 4;
    case 0xa0 ... 0xbf: return 3;
    case 0xc0 ... 0xdf: return 3;
    default: return 1;
    }
}

static void exec_command(void) {
    u32 cmd = gp0_cmd;
    if (cmd >= 0x20 && cmd <= 0x3f) { exec_polygon(); return; }
    if (cmd >= 0x40 && cmd <= 0x5f) { exec_line_simple(); return; }
    if (cmd >= 0x60 && cmd <= 0x7f) { exec_rect(); return; }
    switch (cmd) {
    case 0x02: vram_fill(gp0_buf[0], gp0_buf[1], gp0_buf[2]); break;
    case 0x80 ... 0x9f: vram_copy(gp0_buf[1], gp0_buf[2], gp0_buf[3]); break;
    case 0xa0 ... 0xbf: {
        vt.x = gp0_buf[1] & 0x3ff; vt.y = (gp0_buf[1] >> 16) & 0x1ff;
        vt.w = gp0_buf[2] & 0x3ff; vt.h = (gp0_buf[2] >> 16) & 0x1ff;
        if (!vt.w) vt.w = 0x400;
        if (!vt.h) vt.h = 0x200;
        vt.cx = vt.x; vt.cy = vt.y;
        vt.remaining_px = (u32)vt.w * vt.h;
        gp0_mode = M_VWRITE;
        break;
    }
    case 0xc0 ... 0xdf: {
        vr.x = gp0_buf[1] & 0x3ff; vr.y = (gp0_buf[1] >> 16) & 0x1ff;
        vr.w = gp0_buf[2] & 0x3ff; vr.h = (gp0_buf[2] >> 16) & 0x1ff;
        if (!vr.w) vr.w = 0x400;
        if (!vr.h) vr.h = 0x200;
        vr.cx = vr.x; vr.cy = vr.y;
        gp0_mode = M_VREAD;
        break;
    }
    case 0xe1: {
        stat_texpage = gp0_buf[0] & 0x7ff;
        tex_disable = (gp0_buf[0] >> 11) & 1;
        flip_x = (gp0_buf[0] >> 12) & 1;
        flip_y = (gp0_buf[0] >> 13) & 1;
        break;
    }
    case 0xe2:
        reg_e2 = gp0_buf[0] & 0xfffff;
        tw_mask_x = gp0_buf[0] & 31; tw_mask_y = (gp0_buf[0] >> 5) & 31;
        tw_off_x = (gp0_buf[0] >> 10) & 31; tw_off_y = (gp0_buf[0] >> 15) & 31;
        break;
    case 0xe3: reg_e3 = gp0_buf[0] & 0xfffff; da_x1 = gp0_buf[0] & 0x3ff; da_y1 = (gp0_buf[0] >> 10) & 0x1ff; break;
    case 0xe4: reg_e4 = gp0_buf[0] & 0xfffff; da_x2 = gp0_buf[0] & 0x3ff; da_y2 = (gp0_buf[0] >> 10) & 0x1ff; break;
    case 0xe5:
        reg_e5 = gp0_buf[0] & 0x3fffff;
        off_x = sext11((int)(gp0_buf[0] & 0x7ff)); off_y = sext11((int)((gp0_buf[0] >> 11) & 0x7ff));
        break;
    case 0xe6: set_mask = gp0_buf[0] & 1; check_mask = (gp0_buf[0] >> 1) & 1; break;
    case 0x1f: irq1_flag = true; irq_raise(IRQ_GPU); break;
    default: break;
    }
}

static inline bool pl_terminator(u32 v) { return (v & 0xf000f000u) == 0x50005000u; }

static void gp0_word(u32 v) {
    switch (gp0_mode) {
    case M_VWRITE:
        vwrite_pixel((u16)v);
        if (vt.remaining_px) vt.remaining_px--;
        if (vt.remaining_px) { vwrite_pixel((u16)(v >> 16)); vt.remaining_px--; }
        if (!vt.remaining_px) gp0_mode = M_CMD;
        return;
    case M_POLYLINE: {
        Vtx a, b;
        switch (pl.state) {
        case PL_V0:
            vtx_xy(&a, v); pl.px = a.x; pl.py = a.y; pl.pcolor = pl.color;
            pl.state = pl.gouraud ? PL_C : PL_VN;
            return;
        case PL_VN:
            if (pl_terminator(v)) { gp0_mode = M_CMD; return; }
            vtx_col(&a, pl.color); a.x = pl.px; a.y = pl.py;
            b = a; vtx_xy(&b, v);
            draw_line(&a, &b, false, pl.semi);
            pl.px = b.x; pl.py = b.y;
            return;
        case PL_C:
            if (pl_terminator(v)) { gp0_mode = M_CMD; return; }
            pl.pending_color = v; pl.state = PL_V;
            return;
        default: /* PL_V */
            vtx_col(&a, pl.pcolor); a.x = pl.px; a.y = pl.py;
            vtx_col(&b, pl.pending_color); vtx_xy(&b, v);
            draw_line(&a, &b, true, pl.semi);
            pl.px = b.x; pl.py = b.y; pl.pcolor = pl.pending_color;
            pl.state = PL_C;
            return;
        }
    }
    default: break;
    }
    if (gp0_pos == 0) {
        gp0_cmd = v >> 24;
        bool poly;
        int need = cmd_words(gp0_cmd, &poly);
        if (poly) {
            pl.gouraud = gp0_cmd & 0x10; pl.semi = gp0_cmd & 2; pl.color = v & 0xffffff; pl.state = PL_V0;
            gp0_mode = M_POLYLINE;
            return;
        }
        gp0_need = need;
    }
    gp0_buf[gp0_pos++] = v;
    if (gp0_pos < gp0_need) return;
    gp0_pos = 0;
    exec_command();
}

void gpu_write(u32 reg, u32 v) {
    if (reg == 0) { gp0_word(v); return; }
    /* GP1 */
    u32 cmd = v >> 24;
    switch (cmd) {
    case 0x00:
        stat_texpage = 0; tex_disable = false; flip_x = flip_y = false;
        tw_mask_x = tw_mask_y = tw_off_x = tw_off_y = 0; reg_e2 = 0;
        da_x1 = da_y1 = 0; da_x2 = da_y2 = 0; reg_e3 = reg_e4 = reg_e5 = 0;
        off_x = off_y = 0; set_mask = check_mask = false;
        irq1_flag = false; disp_disabled = true; dma_dir = 0;
        gp0_mode = M_CMD; gp0_pos = 0;
        disp_x = disp_y = 0; hrange = 0xc00200; vrange = 0x100010;
        break;
    case 0x01: gp0_mode = M_CMD; gp0_pos = 0; break;
    case 0x02: irq1_flag = false; break;
    case 0x03: disp_disabled = v & 1; break;
    case 0x04: dma_dir = v & 3; break;
    case 0x05: disp_x = v & 0x3ff; disp_y = (v >> 10) & 0x1ff; break;
    case 0x06: hrange = v & 0xffffff; break;
    case 0x07: vrange = v & 0xfffff; break;
    case 0x08: disp_mode = v & 0xff; break;
    case 0x09: break;
    default:
        if (cmd >= 0x10 && cmd <= 0x1f) {
            switch (v & 15) {
            case 2: gpuread_latch = reg_e2; break;
            case 3: gpuread_latch = reg_e3; break;
            case 4: gpuread_latch = reg_e4; break;
            case 5: gpuread_latch = reg_e5; break;
            case 7: gpuread_latch = 2; break;
            case 8: gpuread_latch = 0; break;
            default: break;
            }
        }
        break;
    }
}

u32 gpu_read(u32 reg) {
    if (reg == 4) {
        u32 s = stat_texpage & 0x7ff;
        s &= ~(1u << 10);
        s |= (stat_texpage & (1u << 10));
        if (tex_disable) s |= 1u << 15;
        if (set_mask) s |= 1u << 11;
        if (check_mask) s |= 1u << 12;
        s |= (1u << 13);
        s |= ((disp_mode >> 7) & 1) << 14;
        s |= ((disp_mode >> 6) & 1) << 16;
        s |= (disp_mode & 3) << 17;
        s |= ((disp_mode >> 2) & 1) << 19;
        s |= ((disp_mode >> 3) & 1) << 20;
        s |= ((disp_mode >> 4) & 1) << 21;
        s |= ((disp_mode >> 5) & 1) << 22;
        if (disp_disabled) s |= 1u << 23;
        if (irq1_flag) s |= 1u << 24;
        s |= (1u << 26) | (1u << 28);
        if (gp0_mode == M_VREAD) s |= 1u << 27; else s |= 1u << 27;
        s |= dma_dir << 29;
        u32 dreq = 0;
        switch (dma_dir) { case 1: dreq = 1; break; case 2: dreq = (s >> 28) & 1; break; case 3: dreq = (s >> 27) & 1; break; }
        s |= dreq << 25;
        /* bit 31: odd/even line */
        if (!in_vblank) {
            int line = current_line(cpu.cycles);
            bool interlaced = (disp_mode >> 5) & 1;
            if (interlaced) { /* field alternates each frame */ static int fld; (void)fld; s |= ((u32)(cpu.cycles / gpu_frame_cycles()) & 1) << 31; }
            else s |= (u32)(line & 1) << 31;
        }
        return s;
    }
    /* GPUREAD */
    if (gp0_mode == M_VREAD) {
        u32 lo = vram[(vr.cy & 511) * 1024 + (vr.cx & 1023)];
        if (++vr.cx >= vr.x + vr.w) { vr.cx = vr.x; vr.cy++; }
        u32 hi = vram[(vr.cy & 511) * 1024 + (vr.cx & 1023)];
        if (++vr.cx >= vr.x + vr.w) { vr.cx = vr.x; vr.cy++; }
        if (vr.cy >= vr.y + vr.h) gp0_mode = M_CMD;
        gpuread_latch = lo | (hi << 16);
    }
    return gpuread_latch;
}

void gpu_dma_write(const u32 *words, u32 count) {
    for (u32 i = 0; i < count; i++) gp0_word(words[i]);
}
void gpu_dma_read(u32 *words, u32 count) {
    for (u32 i = 0; i < count; i++) words[i] = gpu_read(0);
}

void gpu_init(void) {
    memset(vram, 0, sizeof vram);
    gpu_write(4, 0);
    disp_mode = region_pal ? 8 : 0;
    frame_start = cpu.cycles;
    in_vblank = false;
    next_gpu_event = 0;
    da_x2 = 1023; da_y2 = 511;
}

bool gpu_vblank_event(void) { return in_vblank; }
