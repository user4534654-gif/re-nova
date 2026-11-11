/* Optional fast-start: jump straight into a level at boot. */
#ifndef NOVA_FASTSTART_H
#define NOVA_FASTSTART_H

#include "guest.h"

/* Parse a level spec ("2", "level_02", "jr_level_02.lvl", ...).
 * Pass NULL/empty to disable.  Safe to call once at boot. */
void faststart_init(const char *spec);

/* Drive the startup states: skip the logo/splash and, once the main menu is
 * up, load the requested level.  Call once per frame from the main loop. */
void faststart_tick(CPU *cpu, int frame);

/* Log a line whenever the engine's active state changes (menu, loading,
 * gameplay, ...).  Independent of faststart being enabled, so normal play
 * shows the same trail.  Call once per frame. */
void faststart_trace_state(void);

#endif
