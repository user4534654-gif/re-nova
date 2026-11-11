/* Ring of the most recent indirect guest transfers.
 *
 * The translated code stores cpu->pc only for indirect PC writes, so a native
 * fault inside a callee reports the last transfer site rather than the crash
 * site.  Keeping the recent transfers makes the real path visible in the
 * crash dump.
 *
 * Kept out of guest.h on purpose: guest.h is included by every generated
 * translation unit, so editing it forces a full rebuild.
 */
#ifndef NOVA_XFER_RING_H
#define NOVA_XFER_RING_H

#include <stdint.h>

#define GUEST_RING 64
extern uint32_t g_xfer_site[GUEST_RING];
extern uint32_t g_xfer_target[GUEST_RING];
extern uint32_t g_xfer_lr[GUEST_RING];
extern volatile unsigned g_xfer_pos;
void guest_ring_push(uint32_t site, uint32_t target, uint32_t lr);

/* Cached environment probes for temporary diagnostics inside generated code. */
int guest_env_on(const char *name);

#endif