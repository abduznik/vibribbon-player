/* SIO0: controller port. Emulates one digital pad on port 1; no memory cards. */
#include "psx.h"

u16 g_pad_buttons = 0xffff;   /* active low */
u8 g_pad_lx = 128, g_pad_ly = 128, g_pad_rx = 128, g_pad_ry = 128;

static struct {
    u32 ctrl, mode, baud;
    u8 rx;
    bool rx_valid;
    bool irq;
    int pos;
    bool pad_selected;
    u64 rx_at;     /* cycle at which rx byte becomes valid */
    u64 ack_at;    /* cycle at which /ACK pulse raises the interrupt (0 = none) */
    bool more;     /* more bytes expected after the current one */
    bool dtr_prev;
} s;

void sio_init(void) { memset(&s, 0, sizeof s); }

u64 sio_next_event(void) { return s.ack_at ? s.ack_at : ~0ull; }

void sio_tick(u64 now) {
    if (s.ack_at && now >= s.ack_at) {
        s.ack_at = 0;
        s.irq = true;
        if (s.ctrl & (1u << 12)) irq_raise(IRQ_SIO0);
    }
}

static void settle(void) {
    if (!s.rx_valid && s.rx_at && cpu.cycles >= s.rx_at) { s.rx_valid = true; s.rx_at = 0; }
}

static void pad_byte(u8 tx) {
    if (g_verbose >= 3) fprintf(stderr, "[sio] tx %02x pos=%d ctrl=%04x\n", tx, s.pos, s.ctrl);
    u8 out = 0xff;
    bool ack = false;
    bool more = false;
    switch (s.pos) {
    case 0:
        if (tx == 0x01 && !(s.ctrl & (1u << 13))) { s.pad_selected = true; ack = true; more = true; }
        else { s.pad_selected = false; }
        break;
    case 1:
        if (s.pad_selected) { out = 0x41; ack = true; more = true; }
        break;
    case 2:
        if (s.pad_selected) { out = 0x5a; ack = true; more = true; }
        break;
    case 3:
        if (s.pad_selected) { out = (u8)((g_pad_override_active ? g_pad_override : g_pad_buttons) & 0xff); ack = true; more = true; }
        break;
    case 4:
        if (s.pad_selected) { out = (u8)((g_pad_override_active ? g_pad_override : g_pad_buttons) >> 8); ack = false; }
        break;
    default: break;
    }
    s.pos++;
    s.rx = out;
    s.rx_valid = false;
    s.rx_at = cpu.cycles + 1100;
    s.ack_at = ack ? cpu.cycles + 1100 + 300 : 0;
    s.more = more;
    sched_update();
}

u32 sio_read(u32 off, int size) {
    (void)size;
    settle();
    switch (off) {
    case 0x40: {
        u32 v = s.rx_valid ? s.rx : 0xff;
        if (g_verbose >= 3) fprintf(stderr, "[sio] rx %02x valid=%d\n", v, s.rx_valid);
        s.rx_valid = false;
        return v | 0xffffff00u * 0;
    }
    case 0x44: {
        u32 st = 1 | 4;
        if (s.rx_valid) st |= 2;
        if (s.irq) st |= 1u << 9;
        if (s.ack_at) st |= 0; /* /ACK low while pulsing is not modelled */
        return st;
    }
    case 0x48: return s.mode;
    case 0x4a: return s.ctrl;
    case 0x4e: return s.baud;
    }
    return 0;
}

void sio_write(u32 off, u32 v, int size) {
    if (g_verbose >= 3) fprintf(stderr, "[sio] write off=%02x v=%04x size=%d cyc=%llu\n", off, v, size, (unsigned long long)cpu.cycles);
    switch (off) {
    case 0x40:
        if (s.ctrl & 1) pad_byte((u8)v);
        break;
    case 0x48: s.mode = v & 0xffff; break;
    case 0x4a:
        if (v & (1u << 6)) { /* reset */
            s.ctrl = 0; s.irq = false; s.pos = 0; s.rx_valid = false; s.ack_at = 0; s.rx_at = 0;
            return;
        }
        if (v & (1u << 4)) { s.irq = false; }
        s.ctrl = v & 0x3f2f;
        if (!(v & 2)) { s.pos = 0; s.pad_selected = false; s.ack_at = 0; }
        break;
    case 0x4e: s.baud = v & 0xffff; break;
    }
}

/* MDEC: not used by this game; behave as an idle device. */
u32 mdec_read(u32 off) { return off == 4 ? 0x80040000u : 0; }
void mdec_write(u32 off, u32 v) { (void)off; (void)v; }
