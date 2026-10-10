/* PS1 Geometry Transformation Engine (COP2). Implemented from the public hardware documentation
 * (psx-spx), including saturation flags, the 44-bit MAC range checks and the UNR divider. */
#include "psx.h"

#define D cpu.gd
#define C cpu.gc

/* data register indices */
enum { VXY0, VZ0, VXY1, VZ1, VXY2, VZ2, RGBC, OTZ, IR0, IR1, IR2, IR3, SXY0, SXY1, SXY2, SXYP,
       SZ0, SZ1, SZ2, SZ3, RGB0, RGB1, RGB2, RES1, MAC0, MAC1, MAC2, MAC3, IRGB, ORGB, LZCS, LZCR };
/* control register indices */
enum { RT0, RT1, RT2, RT3, RT4, TRX, TRY, TRZ, L0, L1, L2, L3, L4, RBK, GBK, BBK, LC0, LC1, LC2, LC3, LC4,
       RFC, GFC, BFC, OFX, OFY, HREG, DQA, DQB, ZSF3, ZSF4, FLAG };

static u32 flag;
int g_gte_debug;
static u8 unr_table[0x101];
static bool unr_init;

static void gte_tables(void) {
    for (int i = 0; i <= 0x100; i++) {
        int n = ((0x40000 / (i + 0x100)) + 1) / 2 - 0x101;
        unr_table[i] = n < 0 ? 0 : (u8)n;
    }
    unr_init = true;
}

#define F_MAC1P (1u << 30)
#define F_MAC2P (1u << 29)
#define F_MAC3P (1u << 28)
#define F_MAC1N (1u << 27)
#define F_MAC2N (1u << 26)
#define F_MAC3N (1u << 25)
#define F_IR1 (1u << 24)
#define F_IR2 (1u << 23)
#define F_IR3 (1u << 22)
#define F_CR (1u << 21)
#define F_CG (1u << 20)
#define F_CB (1u << 19)
#define F_SZ3 (1u << 18)
#define F_DIV (1u << 17)
#define F_MAC0P (1u << 16)
#define F_MAC0N (1u << 15)
#define F_SX2 (1u << 14)
#define F_SY2 (1u << 13)
#define F_IR0 (1u << 12)

static inline s16 vx(int n) { return (s16)D[n * 2]; }
static inline s16 vy(int n) { return (s16)(D[n * 2] >> 16); }
static inline s16 vz(int n) { return (s16)D[n * 2 + 1]; }
static inline s32 ir(int n) { return (s32)D[IR0 + n]; } /* n = 0..3 */
static inline s16 cr16lo(int i) { return (s16)C[i]; }
static inline s16 cr16hi(int i) { return (s16)(C[i] >> 16); }

/* matrix element access: m = 0 rotation, 1 light, 2 color. e(row,col) */
static s16 mat(int m, int row, int col) {
    if (m == 3) {
        /* reserved selector: the hardware uses a "garbage" matrix built from RGBC.R, IR0 and RT13/RT22 */
        s32 r = (s32)(D[RGBC] & 0xff) << 4;
        if (row == 0) return col == 0 ? (s16)(-r) : col == 1 ? (s16)r : (s16)D[IR0];
        if (row == 1) return (s16)C[RT1];
        return (s16)C[RT2];
    }
    int base = (m == 0) ? RT0 : (m == 1) ? L0 : LC0;
    int idx = row * 3 + col;              /* 0..8, packed two per register */
    u32 w = C[base + (idx >> 1)];
    return (idx & 1) ? (s16)(w >> 16) : (s16)w;
}

/* sign-extend to 44 bits as the hardware does (keeps low 44 bits) */
static inline s64 wrap44(s64 v) { return (s64)((u64)v << 20) >> 20; }
/* flag a 44-bit overflow; the hardware accumulator then continues with the wrapped value */
static inline s64 chk44(s64 v, u32 pos, u32 neg) {
    if (v > 0x7ffffffffffLL) flag |= pos;
    else if (v < -0x80000000000LL) flag |= neg;
    return wrap44(v);
}

static inline s32 setmac(int i, s64 v, int shift) {
    /* i = 1..3 ; v already range-checked (flags) */
    v = wrap44(v) >> shift;
    D[MAC0 + i] = (u32)v;
    return (s32)v;
}

static inline s32 clamp_ir(int i, s32 v, bool lm) {
    static const u32 bits[4] = {F_IR0, F_IR1, F_IR2, F_IR3};
    s32 lo = lm ? 0 : -0x8000;
    if (v > 0x7fff) { flag |= bits[i]; v = 0x7fff; }
    else if (v < lo) { flag |= bits[i]; v = lo; }
    return v;
}
static inline void set_ir(int i, s32 v, bool lm) { D[IR0 + i] = (u32)clamp_ir(i, v, lm); }

static inline s32 mac0_chk(s64 v) {
    if (v > 0x7fffffffLL) flag |= F_MAC0P;
    else if (v < -0x80000000LL) flag |= F_MAC0N;
    return (s32)v;
}

static void push_sz(s32 v) {
    D[SZ0] = D[SZ1]; D[SZ1] = D[SZ2]; D[SZ2] = D[SZ3];
    if (v < 0) { flag |= F_SZ3; v = 0; }
    else if (v > 0xffff) { flag |= F_SZ3; v = 0xffff; }
    D[SZ3] = (u32)v;
}
static void push_sxy(s32 x, s32 y) {
    if (x < -0x400) { flag |= F_SX2; x = -0x400; } else if (x > 0x3ff) { flag |= F_SX2; x = 0x3ff; }
    if (y < -0x400) { flag |= F_SY2; y = -0x400; } else if (y > 0x3ff) { flag |= F_SY2; y = 0x3ff; }
    D[SXY0] = D[SXY1]; D[SXY1] = D[SXY2];
    D[SXY2] = ((u32)(u16)x) | ((u32)(u16)y << 16);
}
static void push_color(s32 r, s32 g, s32 b) {
    if (r < 0) { flag |= F_CR; r = 0; } else if (r > 255) { flag |= F_CR; r = 255; }
    if (g < 0) { flag |= F_CG; g = 0; } else if (g > 255) { flag |= F_CG; g = 255; }
    if (b < 0) { flag |= F_CB; b = 0; } else if (b > 255) { flag |= F_CB; b = 255; }
    D[RGB0] = D[RGB1]; D[RGB1] = D[RGB2];
    D[RGB2] = (u32)r | ((u32)g << 8) | ((u32)b << 16) | (D[RGBC] & 0xff000000u);
}

static u32 gte_divide(u32 h, u32 sz3) {
    if (h < sz3 * 2) {
        int z = __builtin_clz((u32)(u16)sz3) - 16;
        if (sz3 == 0) z = 16;
        u64 n = (u64)h << z;
        u64 d = (u64)sz3 << z;
        u64 u = (u64)unr_table[(d - 0x7fc0) >> 7] + 0x101;
        d = (0x2000080 - (d * u)) >> 8;
        d = (0x0000080 + (d * u)) >> 8;
        u64 r = ((n * d) + 0x8000) >> 16;
        return r > 0x1ffff ? 0x1ffff : (u32)r;
    }
    flag |= F_DIV;
    return 0x1ffff;
}

/* Core matrix-vector op producing MAC1..3/IR1..3.
 * tmode: 0 TR, 1 BK, 2 FC (hardware quirk), 3 none */
static void mvmva_core(int m, s32 v0, s32 v1, s32 v2, int tmode, int shift, bool lm) {
    s64 t[3] = {0, 0, 0};
    switch (tmode) {
    case 0: t[0] = (s32)C[TRX]; t[1] = (s32)C[TRY]; t[2] = (s32)C[TRZ]; break;
    case 1: t[0] = (s32)C[RBK]; t[1] = (s32)C[GBK]; t[2] = (s32)C[BBK]; break;
    case 2: t[0] = (s32)C[RFC]; t[1] = (s32)C[GFC]; t[2] = (s32)C[BFC]; break;
    }
    static const u32 posf[3] = {F_MAC1P, F_MAC2P, F_MAC3P};
    static const u32 negf[3] = {F_MAC1N, F_MAC2N, F_MAC3N};
    s32 vec[3] = {v0, v1, v2};
    s32 out[3];
    for (int i = 0; i < 3; i++) {
        s64 sum = t[i] * 0x1000;
        if (tmode != 3) sum = chk44(sum, posf[i], negf[i]);
        if (tmode == 2) {
            /* far-color quirk: first term only affects flags */
            s64 s1 = chk44(sum + (s64)mat(m, i, 0) * vec[0], posf[i], negf[i]);
            s32 tmp = (s32)(wrap44(s1) >> shift);
            clamp_ir(1 + i, tmp, false);
            sum = (s64)mat(m, i, 1) * vec[1];
            sum = chk44(sum, posf[i], negf[i]);
            sum = chk44(sum + (s64)mat(m, i, 2) * vec[2], posf[i], negf[i]);
        } else {
            sum = chk44(sum + (s64)mat(m, i, 0) * vec[0], posf[i], negf[i]);
            sum = chk44(sum + (s64)mat(m, i, 1) * vec[1], posf[i], negf[i]);
            sum = chk44(sum + (s64)mat(m, i, 2) * vec[2], posf[i], negf[i]);
        }
        out[i] = setmac(1 + i, sum, shift);
    }
    for (int i = 0; i < 3; i++) set_ir(1 + i, out[i], lm);
}

static void do_rtp(int vi, int shift, bool lm, bool last) {
    s32 x = vx(vi), y = vy(vi), z = vz(vi);
    u32 pre_ir3 = flag & F_IR3;                 /* flags accumulate across the vertices of RTPT */
    mvmva_core(0, x, y, z, 0, shift, lm);
    /* SZ3 = (44-bit MAC3 before sf shift) >> 12 */
    s64 raw3 = wrap44((s64)(s32)C[TRZ] * 0x1000 + (s64)mat(0, 2, 0) * x + (s64)mat(0, 2, 1) * y + (s64)mat(0, 2, 2) * z);
    s32 szv = (s32)(raw3 >> 12);
    if (!shift) {
        /* hardware quirk: with sf=0 the IR3 saturation flag is judged on MAC3 >> 12, while IR3 itself is
         * still clamped from the unshifted MAC3 */
        flag = (flag & ~F_IR3) | pre_ir3;
        if (szv > 0x7fff || szv < -0x8000) flag |= F_IR3;     /* lm is ignored by this flag quirk */
    }
    push_sz(szv);
    u32 q = gte_divide((u16)C[HREG], D[SZ3]);
    s64 sx = (s64)(s32)q * ir(1) + (s32)C[OFX];
    s64 sy = (s64)(s32)q * ir(2) + (s32)C[OFY];
    mac0_chk(sx); mac0_chk(sy);
    if (g_gte_debug && (((sx >> 16) < -0x400 || (sx >> 16) > 0x3ff || (sy >> 16) < -0x400 || (sy >> 16) > 0x3ff))) {
        fprintf(stderr, "[gte] RTP sat: V=(%d,%d,%d) IR=(%d,%d,%d) SZ3=%u H=%u q=%x OFX=%d OFY=%d RT=[%04x %04x %04x %04x %04x] TR=(%d,%d,%d) mac3=%d\n", x, y, z,
                ir(1), ir(2), ir(3), D[SZ3], (u16)C[HREG], q, (s32)C[OFX], (s32)C[OFY], C[RT0] & 0xffff, C[RT1] & 0xffff, C[RT2] & 0xffff, C[RT3] & 0xffff, C[RT4] & 0xffff,
                (s32)C[TRX], (s32)C[TRY], (s32)C[TRZ], (s32)D[MAC3]);
    }
    push_sxy((s32)(sx >> 16), (s32)(sy >> 16));
    if (last) {
        s64 m0 = (s64)(s32)q * (s16)C[DQA] + (s32)C[DQB];
        D[MAC0] = (u32)mac0_chk(m0);
        s32 i0 = (s32)(m0 >> 12);
        if (i0 < 0) { flag |= F_IR0; i0 = 0; } else if (i0 > 0x1000) { flag |= F_IR0; i0 = 0x1000; }
        D[IR0] = (u32)i0;
    }
}

static void do_nclip(void) {
    s32 x0 = (s16)D[SXY0], y0 = (s16)(D[SXY0] >> 16);
    s32 x1 = (s16)D[SXY1], y1 = (s16)(D[SXY1] >> 16);
    s32 x2 = (s16)D[SXY2], y2 = (s16)(D[SXY2] >> 16);
    s64 v = (s64)x0 * (y1 - y2) + (s64)x1 * (y2 - y0) + (s64)x2 * (y0 - y1);
    D[MAC0] = (u32)mac0_chk(v);
}

static void color_from_mac(int unused) {
    (void)unused;
    push_color((s32)D[MAC1] >> 4, (s32)D[MAC2] >> 4, (s32)D[MAC3] >> 4);
}

/* lighting helpers */
static void light_stage(s32 v0, s32 v1, s32 v2, int shift, bool lm) {
    mvmva_core(1, v0, v1, v2, 3, shift, lm);           /* LLM * V */
    mvmva_core(2, ir(1), ir(2), ir(3), 1, shift, lm);  /* BK + LCM * IR */
}

static void do_ncs(int vi, int shift, bool lm) {
    light_stage(vx(vi), vy(vi), vz(vi), shift, lm);
    color_from_mac(0);
}
static void do_nccs(int vi, int shift, bool lm) {
    light_stage(vx(vi), vy(vi), vz(vi), shift, lm);
    u32 c = D[RGBC];
    s64 r = ((s64)(c & 0xff) * ir(1)) << 4, g = ((s64)((c >> 8) & 0xff) * ir(2)) << 4, b = ((s64)((c >> 16) & 0xff) * ir(3)) << 4;
    s32 m1 = setmac(1, chk44(r, F_MAC1P, F_MAC1N), shift), m2 = setmac(2, chk44(g, F_MAC2P, F_MAC2N), shift), m3 = setmac(3, chk44(b, F_MAC3P, F_MAC3N), shift);
    set_ir(1, m1, lm); set_ir(2, m2, lm); set_ir(3, m3, lm);
    color_from_mac(0);
}
/* depth cue: MAC = v + (FC - v) * IR0, result in MAC/IR (no colour push) */
static void depth_cue(s64 r, s64 g, s64 b, int shift, bool lm) {
    /* r,g,b are the "source" terms already scaled to MAC domain (<<12) */
    s64 v[3] = {r, g, b};
    static const u32 pf[3] = {F_MAC1P, F_MAC2P, F_MAC3P}, nf[3] = {F_MAC1N, F_MAC2N, F_MAC3N};
    for (int i = 0; i < 3; i++) {
        s64 fc = (s64)(s32)C[RFC + i] * 0x1000;
        s64 d = chk44(fc - v[i], pf[i], nf[i]);
        s32 tmp = (s32)(wrap44(d) >> shift);
        s32 t2 = clamp_ir(1 + i, tmp, false);
        s64 sum = chk44((s64)t2 * (s32)D[IR0] + v[i], pf[i], nf[i]);
        setmac(1 + i, sum, shift);
    }
    for (int i = 0; i < 3; i++) set_ir(1 + i, (s32)D[MAC1 + i], lm);
}

static void do_ncds(int vi, int shift, bool lm) {
    light_stage(vx(vi), vy(vi), vz(vi), shift, lm);
    u32 c = D[RGBC];
    s64 r = ((s64)(c & 0xff) * ir(1)) << 4, g = ((s64)((c >> 8) & 0xff) * ir(2)) << 4, b = ((s64)((c >> 16) & 0xff) * ir(3)) << 4;
    depth_cue(r, g, b, shift, lm);
    color_from_mac(0);
}
static void do_cc(int shift, bool lm) {
    mvmva_core(2, ir(1), ir(2), ir(3), 1, shift, lm);
    u32 c = D[RGBC];
    s64 r = ((s64)(c & 0xff) * ir(1)) << 4, g = ((s64)((c >> 8) & 0xff) * ir(2)) << 4, b = ((s64)((c >> 16) & 0xff) * ir(3)) << 4;
    s32 m1 = setmac(1, chk44(r, F_MAC1P, F_MAC1N), shift), m2 = setmac(2, chk44(g, F_MAC2P, F_MAC2N), shift), m3 = setmac(3, chk44(b, F_MAC3P, F_MAC3N), shift);
    set_ir(1, m1, lm); set_ir(2, m2, lm); set_ir(3, m3, lm);
    color_from_mac(0);
}
static void do_cdp(int shift, bool lm) {
    mvmva_core(2, ir(1), ir(2), ir(3), 1, shift, lm);
    u32 c = D[RGBC];
    s64 r = ((s64)(c & 0xff) * ir(1)) << 4, g = ((s64)((c >> 8) & 0xff) * ir(2)) << 4, b = ((s64)((c >> 16) & 0xff) * ir(3)) << 4;
    depth_cue(r, g, b, shift, lm);
    color_from_mac(0);
}
static void do_op(int shift, bool lm) {
    s32 d1 = (s16)C[RT0], d2 = (s16)(C[RT2]), d3 = (s16)(C[RT4]); /* diagonal: RT11, RT22, RT33 */
    d1 = (s16)C[RT0]; d2 = (s16)C[RT2]; d3 = (s16)C[RT4];
    s32 i1 = ir(1), i2 = ir(2), i3 = ir(3);
    s64 m1 = (s64)i3 * d2 - (s64)i2 * d3;
    s64 m2 = (s64)i1 * d3 - (s64)i3 * d1;
    s64 m3 = (s64)i2 * d1 - (s64)i1 * d2;
    s32 a = setmac(1, chk44(m1, F_MAC1P, F_MAC1N), shift);
    s32 b = setmac(2, chk44(m2, F_MAC2P, F_MAC2N), shift);
    s32 c = setmac(3, chk44(m3, F_MAC3P, F_MAC3N), shift);
    set_ir(1, a, lm); set_ir(2, b, lm); set_ir(3, c, lm);
}
static void do_sqr(int shift, bool lm) {
    s32 i1 = ir(1), i2 = ir(2), i3 = ir(3);
    s32 a = setmac(1, (s64)i1 * i1, shift), b = setmac(2, (s64)i2 * i2, shift), c = setmac(3, (s64)i3 * i3, shift);
    set_ir(1, a, lm); set_ir(2, b, lm); set_ir(3, c, lm);
}
static void do_gpf(int shift, bool lm) {
    s32 i0 = ir(0);
    s32 a = setmac(1, (s64)ir(1) * i0, shift), b = setmac(2, (s64)ir(2) * i0, shift), c = setmac(3, (s64)ir(3) * i0, shift);
    set_ir(1, a, lm); set_ir(2, b, lm); set_ir(3, c, lm);
    color_from_mac(0);
}
static void do_gpl(int shift, bool lm) {
    s32 i0 = ir(0);
    s64 b1 = (s64)(s32)D[MAC1] << shift, b2 = (s64)(s32)D[MAC2] << shift, b3 = (s64)(s32)D[MAC3] << shift;
    s32 a = setmac(1, chk44((s64)ir(1) * i0 + b1, F_MAC1P, F_MAC1N), shift);
    s32 b = setmac(2, chk44((s64)ir(2) * i0 + b2, F_MAC2P, F_MAC2N), shift);
    s32 c = setmac(3, chk44((s64)ir(3) * i0 + b3, F_MAC3P, F_MAC3N), shift);
    set_ir(1, a, lm); set_ir(2, b, lm); set_ir(3, c, lm);
    color_from_mac(0);
}
static void do_avsz3(void) {
    s64 v = (s64)(s16)C[ZSF3] * (D[SZ1] + D[SZ2] + D[SZ3]);
    D[MAC0] = (u32)mac0_chk(v);
    s32 o = (s32)(v >> 12);
    if (o < 0) { flag |= F_SZ3; o = 0; } else if (o > 0xffff) { flag |= F_SZ3; o = 0xffff; }
    D[OTZ] = (u32)o;
}
static void do_avsz4(void) {
    s64 v = (s64)(s16)C[ZSF4] * (D[SZ0] + D[SZ1] + D[SZ2] + D[SZ3]);
    D[MAC0] = (u32)mac0_chk(v);
    s32 o = (s32)(v >> 12);
    if (o < 0) { flag |= F_SZ3; o = 0; } else if (o > 0xffff) { flag |= F_SZ3; o = 0xffff; }
    D[OTZ] = (u32)o;
}

u64 g_gte_hist[64];
void gte_cmd(u32 op) {
    g_gte_hist[op & 0x3f]++;
    if (!unr_init) gte_tables();
    flag = 0;
    int shift = (op & (1u << 19)) ? 12 : 0;
    bool lm = (op & (1u << 10)) != 0;
    switch (op & 0x3f) {
    case 0x01: do_rtp(0, shift, lm, true); break;
    case 0x06: do_nclip(); break;
    case 0x0c: do_op(shift, lm); break;
    case 0x10: { /* DPCS */
        u32 c = D[RGBC];
        depth_cue(((s64)(c & 0xff)) << 16, ((s64)((c >> 8) & 0xff)) << 16, ((s64)((c >> 16) & 0xff)) << 16, shift, lm);
        color_from_mac(0);
        break;
    }
    case 0x11: { /* INTPL */
        depth_cue(((s64)ir(1)) << 12, ((s64)ir(2)) << 12, ((s64)ir(3)) << 12, shift, lm);
        color_from_mac(0);
        break;
    }
    case 0x12: { /* MVMVA */
        int mx = (op >> 17) & 3, vv = (op >> 15) & 3, tv = (op >> 13) & 3;
        s32 a, b, c;
        if (vv == 3) { a = ir(1); b = ir(2); c = ir(3); }
        else { a = vx(vv); b = vy(vv); c = vz(vv); }
        mvmva_core(mx, a, b, c, tv, shift, lm);
        break;
    }
    case 0x13: do_ncds(0, shift, lm); break;
    case 0x14: do_cdp(shift, lm); break;
    case 0x16: do_ncds(0, shift, lm); do_ncds(1, shift, lm); do_ncds(2, shift, lm); break;
    case 0x1b: do_nccs(0, shift, lm); break;
    case 0x1c: do_cc(shift, lm); break;
    case 0x1e: do_ncs(0, shift, lm); break;
    case 0x20: do_ncs(0, shift, lm); do_ncs(1, shift, lm); do_ncs(2, shift, lm); break;
    case 0x28: do_sqr(shift, lm); break;
    case 0x29: { /* DCPL */
        u32 c = D[RGBC];
        s64 r = ((s64)(c & 0xff) * ir(1)) << 4, g = ((s64)((c >> 8) & 0xff) * ir(2)) << 4, b = ((s64)((c >> 16) & 0xff) * ir(3)) << 4;
        depth_cue(r, g, b, shift, lm);
        color_from_mac(0);
        break;
    }
    case 0x2a: { /* DPCT */
        for (int i = 0; i < 3; i++) {
            u32 c = D[RGB0];
            depth_cue(((s64)(c & 0xff)) << 16, ((s64)((c >> 8) & 0xff)) << 16, ((s64)((c >> 16) & 0xff)) << 16, shift, lm);
            color_from_mac(0);
        }
        break;
    }
    case 0x2d: do_avsz3(); break;
    case 0x2e: do_avsz4(); break;
    case 0x30: do_rtp(0, shift, lm, false); do_rtp(1, shift, lm, false); do_rtp(2, shift, lm, true); break;
    case 0x3d: do_gpf(shift, lm); break;
    case 0x3e: do_gpl(shift, lm); break;
    case 0x3f: do_nccs(0, shift, lm); do_nccs(1, shift, lm); do_nccs(2, shift, lm); break;
    default: WARN("GTE: unknown command %08x\n", op); break;
    }
    if (flag & 0x7f87e000u) flag |= 0x80000000u;
    C[FLAG] = flag;
    if (g_gte_debug > 1 && (flag & 0x7fc00000u) && (op & 0x3f) != 0x01 && (op & 0x3f) != 0x30) {
        static int shown;
        if (shown++ < 25)
            fprintf(stderr, "[gte] cmd %02x op=%07x flag=%08x IR=(%d,%d,%d) IR0=%d MAC=(%d,%d,%d) V0=(%d,%d,%d) RT11=%d\n", op & 0x3f, op, flag, ir(1), ir(2), ir(3), ir(0), (s32)D[MAC1], (s32)D[MAC2], (s32)D[MAC3], vx(0), vy(0), vz(0), (s16)C[RT0]);
    }
}

/* ---- register access ---- */
u32 gte_mfc2(int r) {
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (u32)(s32)(s16)D[r];
    case 7: case 16: case 17: case 18: case 19:
        return D[r] & 0xffff;
    case 15: return D[SXY2];
    case 28: case 29: {
        s32 r5 = ir(1) >> 7, g5 = ir(2) >> 7, b5 = ir(3) >> 7;
        r5 = r5 < 0 ? 0 : r5 > 0x1f ? 0x1f : r5;
        g5 = g5 < 0 ? 0 : g5 > 0x1f ? 0x1f : g5;
        b5 = b5 < 0 ? 0 : b5 > 0x1f ? 0x1f : b5;
        return (u32)(r5 | (g5 << 5) | (b5 << 10));
    }
    default: return D[r & 31];
    }
}
void gte_mtc2(int r, u32 v) {
    switch (r) {
    case 15:
        D[SXY0] = D[SXY1]; D[SXY1] = D[SXY2]; D[SXY2] = v; D[SXYP] = v; break;
    case 28:
        D[IRGB] = v & 0x7fff;
        D[IR1] = (u32)((v & 0x1f) * 0x80);
        D[IR2] = (u32)(((v >> 5) & 0x1f) * 0x80);
        D[IR3] = (u32)(((v >> 10) & 0x1f) * 0x80);
        break;
    case 30: {
        D[LZCS] = v;
        u32 x = (s32)v < 0 ? ~v : v;
        D[LZCR] = x == 0 ? 32 : (u32)__builtin_clz(x);
        break;
    }
    case 29: case 31: break;
    case 7: case 16: case 17: case 18: case 19: D[r] = v & 0xffff; break;
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        D[r] = (u32)(s32)(s16)v;           /* 16-bit registers: only the low halfword is stored */
        break;
    default: D[r] = v; break;
    }
}
u32 gte_cfc2(int r) {
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (u32)(s32)(s16)C[r];
    default: return C[r & 31];
    }
}
void gte_ctc2(int r, u32 v) {
    switch (r) {
    case 31:
        v &= 0x7ffff000u;
        if (v & 0x7f87e000u) v |= 0x80000000u;
        C[31] = v;
        break;
    default: C[r & 31] = v; break;
    }
}
void gte_lwc2(int r, u32 v) { gte_mtc2(r, v); }
u32 gte_swc2(int r) { return gte_mfc2(r); }
