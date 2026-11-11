/*
 * Runtime diagnostics for the N.O.V.A. HD recompilation.
 *
 * One plain-text line per interesting event, each carrying the frame number,
 * the guest call site and, when the game library ships a symbol table, the
 * name of the engine function that made the call.  That is the difference
 * between "the level did not load" and "Application::DeleteSaveGame called
 * remove() and the host stub pretended it succeeded".
 *
 * Output goes to stderr, which the GUI build redirects into
 * %LOCALAPPDATA%\Nova\nova.log; the launcher archives it per run.
 *
 *   NOVA_DIAG=0   silence everything
 *   NOVA_DIAG=1   normal (default)
 *   NOVA_DIAG=2   verbose, per-frame detail
 */
#ifndef NOVA_DIAG_H
#define NOVA_DIAG_H

#include <stdint.h>

/* Loads the guest library's symbol table (best effort) so diag_sym() can
 * name code addresses.  Safe to call once, before the first guest call. */
void diag_init(const char *so_path);

/* Same, split out so callers that already have the library path can load the
 * symbols at any time. */
void diag_so_symbols(const char *so_path);
int diag_sym_count(void);

int diag_on(void);
int diag_verbose(void);

/* Called once per rendered frame; every later line shows that frame. */
void diag_frame(void);

void diag_printf(const char *fmt, ...);

/* "Application::DeleteSaveGame+0x10" for a guest address, "0x00123456" when
 * the symbol table lacks it.  Points at a static buffer. */
const char *diag_sym(uint32_t addr);

#endif
