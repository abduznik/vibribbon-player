/* Run-control flags shared between the core and whatever drives it (autoplay, frontends, the Rust player). */
#include "psx.h"

bool g_fast_forward;                 /* muted, unthrottled: used while a macro skips through menus */
bool g_pad_override_active;          /* macro input replaces the physical pad */
u16 g_pad_override = 0xffff;         /* active-low */
