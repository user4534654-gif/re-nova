/* Host-side keyboard + mouse input for N.O.V.A. HD. */
#ifndef NOVA_KBM_H
#define NOVA_KBM_H

/* One PC-input frame.  Installs the keyboard/mouse input mode: while the game
 * is in first-person gameplay (and Tab is not held) it locks the cursor and
 * feeds the engine's touch widgets; otherwise it stays out of the way so the
 * existing mouse-as-touch path keeps driving the menus. */
void kbm_tick(int frame);

/* 1 while KBM owns the mouse (cursor locked).  Callers that map raw mouse
 * input to touches (main.c's poll_touch) must stay quiet in that case. */
int kbm_owns_mouse(void);

/* Diagnostic: dump the live touch-widget registry when RECOMP_KBM_PROBE=1. */
void kbm_probe_tick(int frame);

/* Mouse wheel, forwarded from the window procedure.  Zoom is disabled unless
 * input.ini sets wheel_zoom = 1 (kept for a future sniper-scope mapping). */
void kbm_wheel(int delta);

/* Projection zoom factor for the 3D scene (1.0 = normal, >1 = zoomed in).
 * Applied by the GL layer to the perspective matrix only, so the HUD and the
 * touch controls keep their size. */
float kbm_zoom_scale(void);

#endif
