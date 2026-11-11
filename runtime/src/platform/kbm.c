/* Keyboard + mouse input for N.O.V.A. HD (host side).
 *
 * The engine is touch-only:  nativeTouch* -> notifyTouch* -> TouchScreenBase
 * and then a widget's touchBegan/Moved/Ended.  Instead of patching the
 * translation this module drives that same pipeline with synthetic fingers:
 *
 *   WASD   -> a "walk finger" feeding WalkJoystick (its RaiseMoveEvent turns
 *             each per-event position delta of 40 px into a unit direction)
 *   mouse  -> a "look finger" feeding FullScreenRotateJoystick (delta -> turn)
 *   keys   -> press/release on the live HUD button widgets
 *
 * Widgets are not guessed: every running game state keeps a linked list of the
 * touch widgets registered with it (head at state+4, node {next@0,?,widget@8}).
 * Walking that list yields the live rects and the exact widget class, so the
 * layout follows the engine instead of hard-coded pixels.  See hud_scan().
 *
 * The engine has a real (but tiny) Android key path; notifyKeyPressed only
 * understands Back(4)/Menu(0x52), so everything else must be touches.
 *
 * Bindings live in %LOCALAPPDATA%\Nova\input.ini (see load_ini()).
 */

#include "guest.h"
#include "diag.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- engine addresses ------------------------------------------------- */

#define ADDR_APP_SINGLETON 0x0055ebe4u

/* Game-state vtables (vptr = _ZTV+8).  The engine's state stack is
 * Application+4, top index at stack+0x34. */
#define VT_GS_TEST     (0x00520c78u + 8)
#define VT_GS_INGAMEMENU (0x00521a70u + 8)
#define VT_GS_MINIGAME (0x00520ac0u + 8)
#define VT_GS_ROTGAME  (0x00520c08u + 8)
#define VT_GS_GLLIVE   (0x00520918u + 8)
#define VT_GS_PLAYERDEAD (0x00520ba8u + 8)
#define VT_GS_RESPAWN  (0x00520bd8u + 8)

/* Widget vtables (vptr = _ZTV+8). */
#define VT_WALK    (0x00522ce8u + 8)   /* WalkJoystick              */
#define VT_LOOK    (0x005227c0u + 8)   /* FullScreenRotateJoystick  */
#define VT_SHOOT   (0x00522488u + 8)   /* CButtonShoot   (fire)     */
#define VT_SWEEP   (0x00522620u + 8)   /* CButtonSweep   (weapons)  */
#define VT_MINIMAP (0x005221c8u + 8)   /* CButtonMinimap (map)      */
#define VT_BTNSPR  (0x00522510u + 8)   /* CButtonSpr     (buttons)  */

/* The six CButtonSpr HUD buttons, identified by their reference centres in
 * the 1280x720 logical canvas.  The seventh CButtonSpr sits off-screen. */
enum { B_PAUSE, B_X, B_RING, B_GRENADE, B_ACTION, B_JUMP, B_COUNT };
static const int btn_ref[B_COUNT][2] = {
    { 73, 22 }, { 26, 123 }, { 176, 273 }, { 1211, 199 }, { 1051, 649 }, { 1210, 567 }
};

/* Synthetic finger ids.  The engine indexes per-touch arrays by id, so keep
 * them small and distinct.  id 0 stays with the legacy mouse path. */
enum {
    ID_LOOK = 1, ID_WALK = 2, ID_FIRE = 3, ID_JUMP = 4,
    ID_GRENADE = 5, ID_ACTION = 6, ID_TAP = 7
};

/* ---- bindings --------------------------------------------------------- */

enum {
    A_FWD, A_BACK, A_LEFT, A_RIGHT, A_FIRE, A_ALT, A_JUMP, A_GRENADE,
    A_RELOAD, A_NEXTW, A_PREVW, A_MAP, A_PAUSE, A_MELEE, A_XBTN,
    A_COUNT
};

static const char *act_name[A_COUNT] = {
    "forward", "back", "left", "right", "fire", "alt", "jump", "grenade",
    "reload", "next_weapon", "prev_weapon", "map", "pause", "melee",
    "minimap_toggle"
};

static int g_vk[A_COUNT] = {
    'W', 'S', 'A', 'D', VK_LBUTTON, VK_RBUTTON, VK_SPACE, 'G',
    'R', 'E', 'Q', 'M', VK_ESCAPE, 'F', 'X'
};

static float g_mouse_sens = 1.0f;
static float g_walk_step  = 90.0f;   /* stick deflection in px (deadzone is 40) */
static int   g_invert_y;

static const struct { const char *n; int vk; } key_names[] = {
    { "LMB", VK_LBUTTON }, { "RMB", VK_RBUTTON }, { "MMB", VK_MBUTTON },
    { "SPACE", VK_SPACE }, { "SHIFT", VK_SHIFT }, { "CTRL", VK_CONTROL },
    { "ALT", VK_MENU }, { "ENTER", VK_RETURN }, { "TAB", VK_TAB },
    { "ESC", VK_ESCAPE }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
    { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { NULL, 0 }
};

static int key_from_name(const char *s)
{
    for (int i = 0; key_names[i].n; i++)
        if (!_stricmp(s, key_names[i].n)) return key_names[i].vk;
    if (s[0] && !s[1]) return (unsigned char)s[0];
    return atoi(s);
}

/* ---- state ------------------------------------------------------------ */

static int g_init;
static int g_enabled = 1;
static int g_owns_mouse;
static int g_tab_hold;

/* Scope zoom: the GL layer scales the 3D perspective matrix by this factor,
 * so the HUD and touch controls stay the same size.  1 = no zoom. */
static float g_zoom = 1.0f;
static float g_zoom_step = 1.25f;
static float g_zoom_max = 4.0f;
/* Off by default: N.O.V.A.'s wheel never aimed a scope, and players read the
 * sudden magnification as a bug.  input.ini: wheel_zoom = 1 re-enables it. */
static int   g_wheel_zoom = 0;

void kbm_wheel(int delta)
{
    if (!g_wheel_zoom || !g_owns_mouse || !delta) return;
    if (delta > 0) g_zoom *= g_zoom_step;
    else           g_zoom /= g_zoom_step;
    if (g_zoom < 1.0f) g_zoom = 1.0f;
    if (g_zoom > g_zoom_max) g_zoom = g_zoom_max;
}

float kbm_zoom_scale(void)
{
    return g_owns_mouse ? g_zoom : 1.0f;
}

static uint32_t current_state(void)
{
    uint32_t app = rd32(ADDR_APP_SINGLETON);
    if (!app) return 0;
    uint32_t stack = app + 4;
    int32_t top = s32(stack + 0x34);
    if (top < 0 || top > 32) return 0;
    return rd32(stack + (uint32_t)top * 4);
}

static int state_is(uint32_t vt)
{
    uint32_t st = current_state();
    return st && rd32(st) == vt;
}

/* ---- live HUD map ----------------------------------------------------- */

typedef struct {
    int have_walk, wx, wy, wl, wt, wr, wb;
    int have_look;
    int have_fire, fx, fy;
    int have_sweep, sl, st, sr, sb;
    int have_map, mx, my;
    uint32_t sweep_w, map_w, fire_w, walk_w;
    int btn[B_COUNT][2];
    int btn_ok[B_COUNT];
} HudMap;

static void hud_scan(HudMap *m)
{
    memset(m, 0, sizeof(*m));
    uint32_t st = current_state();
    if (!st) return;
    int node_n = 0;
    for (uint32_t node = rd32(st + 4); node && node_n < 256;
         node = rd32(node + 0), node_n++) {
        uint32_t w = rd32(node + 8);
        if (w < 0x100000u || w >= 0x40000000u) continue;
        uint32_t vt = rd32(w + 0);
        int l = s16(w + 0x170), t = s16(w + 0x172);
        int r = s16(w + 0x174), b = s16(w + 0x176);
        int cx = (l + r) / 2, cy = (t + b) / 2;
        if (vt == VT_WALK) {
            m->have_walk = 1; m->wl = l; m->wt = t; m->wr = r; m->wb = b;
            m->wx = cx; m->wy = cy; m->walk_w = w;
        } else if (vt == VT_LOOK) {
            m->have_look = 1;
        } else if (vt == VT_SHOOT) {
            m->have_fire = 1; m->fx = cx; m->fy = cy; m->fire_w = w;
        } else if (vt == VT_SWEEP) {
            m->have_sweep = 1; m->sl = l; m->st = t; m->sr = r; m->sb = b;
            m->sweep_w = w;
        } else if (vt == VT_MINIMAP) {
            m->have_map = 1; m->mx = cx; m->my = cy; m->map_w = w;
        } else if (vt == VT_BTNSPR) {
            int best = -1, bestd = 90 * 90;
            for (int i = 0; i < B_COUNT; i++) {
                int ddx = cx - btn_ref[i][0], ddy = cy - btn_ref[i][1];
                int d = ddx * ddx + ddy * ddy;
                if (d < bestd) { bestd = d; best = i; }
            }
            if (best >= 0) {
                m->btn_ok[best] = 1;
                m->btn[best][0] = cx; m->btn[best][1] = cy;
            }
        }
    }
}

/* ---- cursor lock ------------------------------------------------------ */

static int g_cursor_locked;
static POINT g_center;

static void lock_cursor(void)
{
    HWND h = (HWND)platform_hwnd();
    if (!h || g_cursor_locked) return;
    RECT r;
    if (!GetClientRect(h, &r)) return;
    POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
    ClientToScreen(h, &tl);
    ClientToScreen(h, &br);
    if (br.x - tl.x < 8 || br.y - tl.y < 8) return;
    g_center.x = (tl.x + br.x) / 2;
    g_center.y = (tl.y + br.y) / 2;
    RECT clip = { tl.x, tl.y, br.x, br.y };
    ClipCursor(&clip);
    SetCursorPos(g_center.x, g_center.y);
    ShowCursor(FALSE);
    g_cursor_locked = 1;
}

static void unlock_cursor(void)
{
    if (!g_cursor_locked) return;
    ClipCursor(NULL);
    ShowCursor(TRUE);
    g_cursor_locked = 0;
}

/* ---- injection -------------------------------------------------------- */

static int g_look_lx, g_look_ly, g_look_on;
static int g_walk_on, g_walk_lx, g_walk_ly, g_walk_dx, g_walk_dy;
static int g_hold_on[A_COUNT], g_hold_x[A_COUNT], g_hold_y[A_COUNT], g_hold_id[A_COUNT];
static int g_tap_on, g_tap_x, g_tap_y, g_tap_wait;

static void kbm_release_all(void)
{
    if (g_look_on) { host_touch_release(g_look_lx, g_look_ly, ID_LOOK); g_look_on = 0; }
    if (g_walk_on) { host_touch_release(g_walk_lx, g_walk_ly, ID_WALK); g_walk_on = 0; }
    for (int a = 0; a < A_COUNT; a++)
        if (g_hold_on[a]) { host_touch_release(g_hold_x[a], g_hold_y[a], g_hold_id[a]); g_hold_on[a] = 0; }
    if (g_tap_on) { host_touch_release(g_tap_x, g_tap_y, ID_TAP); g_tap_on = 0; }
}

/* Live HUD centre of a CButtonSpr, or its reference position if the widget is
 * not in the current state's list.  Never fall back to (0,0): that point sits
 * inside the big pause button, so e.g. a missing jump widget would open the
 * pause menu instead of jumping. */
static int btn_x(const HudMap *m, int b) { return m->btn_ok[b] ? m->btn[b][0] : btn_ref[b][0]; }
static int btn_y(const HudMap *m, int b) { return m->btn_ok[b] ? m->btn[b][1] : btn_ref[b][1]; }

static void hold_begin(HudMap *m, int act, int x, int y, int id)
{
    if (g_hold_on[act]) return;
    if (x < 0 || y < 0 || (x == 0 && y == 0)) return;
    host_touch_press(x, y, id);
    g_hold_on[act] = 1; g_hold_x[act] = x; g_hold_y[act] = y; g_hold_id[act] = id;
}

static void hold_end(int act)
{
    if (!g_hold_on[act]) return;
    host_touch_release(g_hold_x[act], g_hold_y[act], g_hold_id[act]);
    g_hold_on[act] = 0;
}

static void schedule_tap(int act, const HudMap *m)
{
    int x = -1, y = -1;
    switch (act) {
    case A_NEXTW:
        if (m->have_sweep) { x = m->sl + (m->sr - m->sl) / 4; y = (m->st + m->sb) / 2; }
        else { x = 975; y = 63; }
        break;
    case A_PREVW:
        if (m->have_sweep) { x = m->sr - (m->sr - m->sl) / 4; y = (m->st + m->sb) / 2; }
        else { x = 1257; y = 63; }
        break;
    case A_MAP:
        if (m->have_map) { x = m->mx; y = m->my; } else { x = 101; y = 198; }
        break;
    case A_XBTN:
        if (m->btn_ok[B_X]) { x = m->btn[B_X][0]; y = m->btn[B_X][1]; }
        else { x = 26; y = 123; }
        break;
    case A_MELEE: case A_RELOAD:
        if (m->btn_ok[B_ACTION]) { x = m->btn[B_ACTION][0]; y = m->btn[B_ACTION][1]; }
        else { x = 1051; y = 649; }
        break;
    default: return;
    }
    if (g_tap_on) { host_touch_release(g_tap_x, g_tap_y, ID_TAP); g_tap_on = 0; }
    host_touch_press(x, y, ID_TAP);
    g_tap_on = 1; g_tap_x = x; g_tap_y = y; g_tap_wait = 2;
}

static void tick_tap(void)
{
    if (g_tap_on && --g_tap_wait <= 0) {
        host_touch_release(g_tap_x, g_tap_y, ID_TAP);
        g_tap_on = 0;
    }
}

/* ---- Caps Lock gesture recorder --------------------------------------- */
/* Some doors ask for several fingers at once, which one mouse cannot give.
 * Caps Lock on = "drawing mode": the pointer is freed (no camera), and each
 * left-button drag records one stroke, up to three.  Caps Lock off replays
 * every stroke at the same time as three distinct fingers. */
#define GEST_MAX_STROKES 3
#define GEST_MAX_POINTS  256
#define GEST_HOLD_FRAMES 8

typedef struct { int n; short x[GEST_MAX_POINTS], y[GEST_MAX_POINTS]; } GestStroke;

static GestStroke g_gest[GEST_MAX_STROKES];
static int g_gest_n;                 /* strokes recorded so far */
static int g_gest_active = -1;       /* stroke being drawn, -1 = pen up */
static int g_gest_was_down;
static int g_gest_draw;              /* drawing mode is on */
static int g_gest_replay;            /* replay is running */
static int g_gest_t, g_gest_len;
static const int g_gest_id[GEST_MAX_STROKES] = { 8, 10, 11 };  /* 9 is special */

static void gest_reset(void)
{
    g_gest_n = 0;
    g_gest_active = -1;
    g_gest_was_down = 0;
}

static void gest_point(int x, int y)
{
    if (g_gest_active < 0 || g_gest_active >= g_gest_n) return;
    GestStroke *s = &g_gest[g_gest_active];
    if (s->n > 0) {
        int dx = x - s->x[s->n - 1], dy = y - s->y[s->n - 1];
        if (dx * dx + dy * dy < 4) return;     /* ignore 1-px jitter */
    }
    if (s->n >= GEST_MAX_POINTS) return;
    s->x[s->n] = (short)x;
    s->y[s->n] = (short)y;
    s->n++;
}

static void gest_cancel(void)
{
    if (g_gest_replay) {
        for (int i = 0; i < g_gest_n; i++)
            if (g_gest[i].n > 0)
                host_touch_release(g_gest[i].x[g_gest[i].n - 1],
                                   g_gest[i].y[g_gest[i].n - 1], g_gest_id[i]);
        g_gest_replay = 0;
        diag_printf("[kbm] gesture replay cancelled\n");
    }
    g_gest_draw = 0;
    gest_reset();
}

static void gest_draw_begin(void)
{
    kbm_release_all();
    g_gest_draw = 1;
    g_gest_replay = 0;
    gest_reset();
    unlock_cursor();
    diag_printf("[kbm] Caps Lock ON: drawing mode, drag LMB to record up to %d strokes\n",
                GEST_MAX_STROKES);
}

static void gest_draw_finish(void)
{
    g_gest_draw = 0;
    g_gest_active = -1;
    g_gest_was_down = 0;
    int used = 0, len = 1;
    for (int i = 0; i < g_gest_n; i++) {
        if (g_gest[i].n <= 0) continue;
        used++;
        if (g_gest[i].n > len) len = g_gest[i].n;
        host_touch_press(g_gest[i].x[0], g_gest[i].y[0], g_gest_id[i]);
    }
    if (used <= 0) {
        diag_printf("[kbm] Caps Lock OFF: no strokes to replay\n");
        return;
    }
    if (len < 2) len = 2;   /* a single dot still needs one move frame */
    g_gest_len = len;
    g_gest_t = 0;
    g_gest_replay = 1;
    diag_printf("[kbm] Caps Lock OFF: replaying %d stroke(s) over %d frames\n",
                used, len);
}

static void gest_record_tick(void)
{
    HWND h = (HWND)platform_hwnd();
    POINT p;
    if (!h || !GetCursorPos(&p) || !ScreenToClient(h, &p)) return;
    int x = (int)p.x, y = (int)p.y;
    platform_client_to_logical(&x, &y);
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !g_gest_was_down) {
        if (g_gest_n < GEST_MAX_STROKES) {
            GestStroke *s = &g_gest[g_gest_n];
            s->n = 0;
            g_gest_active = g_gest_n++;
            gest_point(x, y);
            diag_printf("[kbm] stroke %d started at %d,%d\n", g_gest_n, x, y);
        }
    } else if (down && g_gest_was_down) {
        gest_point(x, y);
    } else if (!down && g_gest_was_down) {
        gest_point(x, y);
        if (g_gest_active >= 0 && g_gest_active < g_gest_n)
            diag_printf("[kbm] stroke %d finished: %d points\n",
                        g_gest_active + 1, g_gest[g_gest_active].n);
        g_gest_active = -1;
    }
    g_gest_was_down = down;
}

static void gest_replay_tick(void)
{
    if (!g_gest_replay) return;
    if (g_gest_t < g_gest_len) {
        int t = g_gest_t++;
        if (t == 0) return;   /* already pressed on the first point */
        for (int i = 0; i < g_gest_n; i++) {
            GestStroke *s = &g_gest[i];
            if (s->n <= 0) continue;
            int idx = s->n > 1 ? (int)((long)t * (s->n - 1) / (g_gest_len - 1)) : 0;
            if (idx >= s->n) idx = s->n - 1;
            host_touch_move(s->x[idx], s->y[idx], g_gest_id[i]);
        }
        return;
    }
    if (g_gest_t < g_gest_len + GEST_HOLD_FRAMES) { g_gest_t++; return; }
    for (int i = 0; i < g_gest_n; i++)
        if (g_gest[i].n > 0)
            host_touch_release(g_gest[i].x[g_gest[i].n - 1],
                               g_gest[i].y[g_gest[i].n - 1], g_gest_id[i]);
    diag_printf("[kbm] gesture replay finished\n");
    g_gest_replay = 0;
    gest_reset();
}

/* ---- ini -------------------------------------------------------------- */

static const char *key_name_from_vk(int vk)
{
    for (int i = 0; key_names[i].n; i++)
        if (key_names[i].vk == vk) return key_names[i].n;
    return NULL;
}

/* Letter/digit keys print as themselves; everything else as a number. */
static void ini_key_value(char *buf, size_t n, int vk)
{
    const char *nm = key_name_from_vk(vk);
    if (nm) snprintf(buf, n, "%s", nm);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        snprintf(buf, n, "%c", (char)vk);
    else
        snprintf(buf, n, "%d", vk);
}

/* Write a commented default file on first run so the bindings are easy to
 * find and change.  Existing files are never touched. */
static void write_default_ini(const char *dir, const char *path)
{
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryA(dir, NULL);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "; N.O.V.A. HD - keyboard + mouse bindings (PC input mode)\n"
        "; Keys: a letter/digit, or one of LMB RMB MMB SPACE SHIFT CTRL ALT\n"
        ";       ENTER TAB ESC UP DOWN LEFT RIGHT, or a raw virtual-key number.\n"
        "; Hold Tab in game to fall back to touch/mouse input for a moment.\n"
        "\n"
        "[window]\n"
        "width = 1280\n"
        "height = 720\n"
        "fullscreen = 0\n"
        "movies = 1\n"
        "open_link = 1\n"
        "\n"
        "[aim]\n"
        "mouse_sens = %.2f\n"
        "invert_y = %d\n"
        "walk_step = %.0f\n"
        "zoom_step = %.2f\n"
        "zoom_max = %.2f\n"
        "wheel_zoom = %d\n"
        "\n"
        "[keys]\n",
        (double)g_mouse_sens, g_invert_y, (double)g_walk_step,
        (double)g_zoom_step, (double)g_zoom_max, g_wheel_zoom);
    for (int i = 0; i < A_COUNT; i++) {
        char kv[32];
        ini_key_value(kv, sizeof(kv), g_vk[i]);
        fprintf(f, "%-12s = %s\n", act_name[i], kv);
    }
    fclose(f);
}

static void load_ini(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    const char *la = getenv("LOCALAPPDATA");
    if (!la || !*la) return;
    snprintf(dir, sizeof(dir), "%s\\Nova", la);
    snprintf(path, sizeof(path), "%s\\input.ini", dir);
    write_default_ini(dir, path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        while (*v == ' ' || *v == '\t') v++;
        char *e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\r' || ve[-1] == '\n' ||
                          ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        if (!strcmp(k, "mouse_sens")) { g_mouse_sens = (float)atof(v); continue; }
        if (!strcmp(k, "walk_step"))  {
            float s = (float)atof(v);
            if (s < 40) s = 40; else if (s > 140) s = 140;  /* 149 px = engine limit */
            g_walk_step = s; continue;
        }
        if (!strcmp(k, "invert_y"))   { g_invert_y = atoi(v); continue; }
        if (!strcmp(k, "zoom_step"))  { g_zoom_step = (float)atof(v); continue; }
        if (!strcmp(k, "zoom_max"))   { g_zoom_max = (float)atof(v); continue; }
        if (!strcmp(k, "wheel_zoom")) { g_wheel_zoom = atoi(v) ? 1 : 0; continue; }
        for (int i = 0; i < A_COUNT; i++)
            if (!_stricmp(k, act_name[i])) { g_vk[i] = key_from_name(v); break; }
    }
    fclose(f);
}

/* ---- per-frame -------------------------------------------------------- */

/* ---- test script (verification without real hardware) ----------------- */

typedef struct { int frame, vk, down; } KeyEv;
typedef struct { int frame, dx, dy; } MouseEv;
typedef struct { int frame, delta; } WheelEv;
static KeyEv g_skeys[64]; static int g_skeys_n;
static MouseEv g_smouse[64]; static int g_smouse_n;
static WheelEv g_swheel[64]; static int g_swheel_n;
static int g_test_mode;

static int g_syn_down[256];

static void parse_script(void)
{
    const char *ks = getenv("RECOMP_KBM_TEST_KEYS");
    if (ks) {
        g_test_mode = 1;
        const char *s = ks;
        while (*s && g_skeys_n < 64) {
            int f, vk, d;
            if (sscanf(s, "%d,%d,%d", &f, &vk, &d) == 3) {
                g_skeys[g_skeys_n].frame = f;
                g_skeys[g_skeys_n].vk = vk;
                g_skeys[g_skeys_n].down = d;
                g_skeys_n++;
            }
            const char *sc = strchr(s, ';');
            if (!sc) break;
            s = sc + 1;
        }
    }
    const char *ms = getenv("RECOMP_KBM_TEST_MOUSE");
    if (ms) {
        g_test_mode = 1;
        const char *s = ms;
        while (*s && g_smouse_n < 64) {
            int f, dx, dy;
            if (sscanf(s, "%d,%d,%d", &f, &dx, &dy) == 3) {
                g_smouse[g_smouse_n].frame = f;
                g_smouse[g_smouse_n].dx = dx;
                g_smouse[g_smouse_n].dy = dy;
                g_smouse_n++;
            }
            const char *sc = strchr(s, ';');
            if (!sc) break;
            s = sc + 1;
        }
    }
    const char *ws = getenv("RECOMP_KBM_TEST_WHEEL");
    if (ws) {
        g_test_mode = 1;
        const char *s = ws;
        while (*s && g_swheel_n < 64) {
            int f, d;
            if (sscanf(s, "%d,%d", &f, &d) == 2) {
                g_swheel[g_swheel_n].frame = f;
                g_swheel[g_swheel_n].delta = d;
                g_swheel_n++;
            }
            const char *sc = strchr(s, ';');
            if (!sc) break;
            s = sc + 1;
        }
    }
}

static void apply_script(int frame)
{
    if (!g_test_mode) return;
    for (int i = 0; i < g_skeys_n; i++)
        if (g_skeys[i].frame == frame) {
            int vk = g_skeys[i].vk;
            if (vk >= 0 && vk < 256) g_syn_down[vk] = g_skeys[i].down;
        }
}

static void script_mouse(int frame, int *dx, int *dy)
{
    *dx = *dy = 0;
    for (int i = 0; i < g_smouse_n; i++)
        if (g_smouse[i].frame == frame) { *dx = g_smouse[i].dx; *dy = g_smouse[i].dy; }
}

/* Scripted wheel notches, so the zoom can be verified without a real mouse. */
static void script_wheel(int frame)
{
    for (int i = 0; i < g_swheel_n; i++)
        if (g_swheel[i].frame == frame) kbm_wheel(g_swheel[i].delta);
}

static int key_down(int vk)
{
    if (vk >= 0 && vk < 256 && g_syn_down[vk]) return 1;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void kbm_init(void)
{
    if (g_init) return;
    g_init = 1;
    const char *k = getenv("RECOMP_KBM");
    if (k) g_enabled = atoi(k) != 0;
    else g_enabled = getenv("RECOMP_NO_MOUSE") ? 0 : 1;
    parse_script();
    if (g_test_mode) g_enabled = 1;
    load_ini();
    if (getenv("RECOMP_KBM_LOG"))
        fprintf(stderr, "[kbm] enabled=%d sens=%.2f step=%.1f test=%d\n",
                g_enabled, g_mouse_sens, g_walk_step, g_test_mode);
}

static int g_key_prev[A_COUNT];
static int g_edge_down[A_COUNT], g_edge_up[A_COUNT];

void kbm_tick(int frame)
{
    (void)frame;
    kbm_init();

    apply_script(frame);

    for (int a = 0; a < A_COUNT; a++) {
        int k = key_down(g_vk[a]);
        g_edge_down[a] = k && !g_key_prev[a];
        g_edge_up[a]   = !k && g_key_prev[a];
        g_key_prev[a]  = k;
    }
    g_tab_hold = key_down(VK_TAB);

    /* Esc = Android Back.  N.O.V.A.'s pause path is notifyKeyPressed(4) ->
     * Application::Set_BackKey, consumed by CLevel::Update; the HUD pause
     * button only calls GS_Test::Pause() (controls off) and never opens the
     * menu.  The same back key closes GS_InGameMenu. */
    if (g_edge_down[A_PAUSE]) {
        uint32_t st = current_state();
        uint32_t vt = st ? rd32(st) : 0;
        if (vt == VT_GS_TEST || vt == VT_GS_INGAMEMENU) {
            host_android_back_key();
            diag_printf("[kbm] Esc -> Android Back (state vt=%08x)\n", vt);
        }
    }

    /* Caps Lock = drawing mode while in game.  Strokes are recorded instead
     * of camera look, then replayed together when Caps Lock is switched off.
     * Leaving the level cancels everything so strokes never leak into menus. */
    {
        int caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        int in_game = g_enabled && state_is(VT_GS_TEST);
        if (!in_game) {
            if (g_gest_draw || g_gest_replay) gest_cancel();
        } else if (caps) {
            if (!g_gest_draw && !g_gest_replay) gest_draw_begin();
        } else if (g_gest_draw) {
            gest_draw_finish();
        }
    }

    HWND h = (HWND)platform_hwnd();
    HWND fg = GetForegroundWindow();
    int focused = g_test_mode || !h || !fg ||
                  GetAncestor(fg, GA_ROOT) == GetAncestor(h, GA_ROOT);
    int busy = g_gest_draw || g_gest_replay;
    int want = g_enabled && focused && state_is(VT_GS_TEST) && !g_tab_hold;

    if (want != g_owns_mouse) {
        if (!want) { unlock_cursor(); kbm_release_all(); g_zoom = 1.0f; }
        g_owns_mouse = want;
        if (getenv("RECOMP_KBM_LOG"))
            fprintf(stderr, "[kbm] %s\n", want ? "active" : "touch mode");
    }
    /* Keep the free pointer while drawing/replaying; lock it again afterwards
     * (including when only the busy flag changed, not the ownership). */
    if (g_owns_mouse && !busy) lock_cursor();
    else unlock_cursor();

    if (g_owns_mouse && !busy) script_wheel(frame);

    tick_tap();
    if (!g_owns_mouse) return;

    if (g_gest_draw)   { gest_record_tick(); return; }
    if (g_gest_replay) { gest_replay_tick(); return; }

    HudMap hud;
    hud_scan(&hud);

    /* ---- look (FullScreenRotateJoystick, delta-based) ---- */
    POINT p;
    int mdx = 0, mdy = 0;
    if (g_test_mode) {
        script_mouse(frame, &mdx, &mdy);
    } else if (GetCursorPos(&p)) {
        mdx = p.x - g_center.x;
        mdy = p.y - g_center.y;
        if (mdx || mdy) SetCursorPos(g_center.x, g_center.y);
    }
    if (g_invert_y) mdy = -mdy;
    int ldx = (int)(mdx * g_mouse_sens);
    int ldy = (int)(mdy * g_mouse_sens);
    if (ldx > 120) ldx = 120; else if (ldx < -120) ldx = -120;
    if (ldy > 120) ldy = 120; else if (ldy < -120) ldy = -120;
    if (!g_look_on) {
        g_look_lx = 640; g_look_ly = 360; g_look_on = 1;
        host_touch_press(g_look_lx, g_look_ly, ID_LOOK);
    } else if (ldx || ldy) {
        g_look_lx += ldx; g_look_ly += ldy;
        if (g_look_lx < 8 || g_look_lx > 1272 || g_look_ly < 8 || g_look_ly > 712) {
            host_touch_release(g_look_lx, g_look_ly, ID_LOOK);
            g_look_lx = 640; g_look_ly = 360;
            host_touch_press(g_look_lx, g_look_ly, ID_LOOK);
        } else {
            host_touch_move(g_look_lx, g_look_ly, ID_LOOK);
        }
    }

    /* ---- walk (WalkJoystick) ----
     * The engine derives the stick deflection from the *per-event* move delta
     * (40 px deadzone, /40, clamped to unit) and re-bases the origin on every
     * move.  Stepping toward the edge and re-centering therefore pulsed:
     * push forward, then a frame at centre.  Instead we press once and send a
     * single move whose offset *is* the wanted deflection, resending only when
     * the direction changes.  Holding W keeps a steady full-forward stick. */
    int wdx = (key_down(g_vk[A_RIGHT]) ? 1 : 0) - (key_down(g_vk[A_LEFT]) ? 1 : 0);
    int wdy = (key_down(g_vk[A_BACK])  ? 1 : 0) - (key_down(g_vk[A_FWD])  ? 1 : 0);
    if (wdx || wdy) {
        if (!g_walk_on) {
            g_walk_lx = hud.have_walk ? (hud.wl + hud.wr) / 2 : 162;
            g_walk_ly = hud.have_walk ? (hud.wt + hud.wb) / 2 : 480;
            g_walk_on = 1; g_walk_dx = 0; g_walk_dy = 0;
            host_touch_press(g_walk_lx, g_walk_ly, ID_WALK);
        }
        if (wdx != g_walk_dx || wdy != g_walk_dy) {
            int def = (int)g_walk_step;
            g_walk_lx += (wdx - g_walk_dx) * def;
            g_walk_ly += (wdy - g_walk_dy) * def;
            g_walk_dx = wdx; g_walk_dy = wdy;
            host_touch_move(g_walk_lx, g_walk_ly, ID_WALK);
        }
    } else if (g_walk_on) {
        host_touch_release(g_walk_lx, g_walk_ly, ID_WALK);
        g_walk_on = 0; g_walk_dx = 0; g_walk_dy = 0;
    }

    /* ---- hold buttons ---- */
    if (g_edge_down[A_FIRE])
        hold_begin(&hud, A_FIRE, hud.have_fire ? hud.fx : 1117,
                                    hud.have_fire ? hud.fy : 480, ID_FIRE);
    if (g_edge_up[A_FIRE])      hold_end(A_FIRE);
    if (g_edge_down[A_JUMP])
        hold_begin(&hud, A_JUMP, btn_x(&hud, B_JUMP), btn_y(&hud, B_JUMP), ID_JUMP);
    if (g_edge_up[A_JUMP])      hold_end(A_JUMP);
    if (g_edge_down[A_GRENADE])
        hold_begin(&hud, A_GRENADE, btn_x(&hud, B_GRENADE), btn_y(&hud, B_GRENADE), ID_GRENADE);
    if (g_edge_up[A_GRENADE])   hold_end(A_GRENADE);
    if (g_edge_down[A_ALT])
        hold_begin(&hud, A_ALT, btn_x(&hud, B_ACTION), btn_y(&hud, B_ACTION), ID_ACTION);
    if (g_edge_up[A_ALT])       hold_end(A_ALT);

    /* ---- tap buttons ---- */
    if (g_edge_down[A_NEXTW]) schedule_tap(A_NEXTW, &hud);
    if (g_edge_down[A_PREVW]) schedule_tap(A_PREVW, &hud);
    if (g_edge_down[A_MAP])   schedule_tap(A_MAP, &hud);
    if (g_edge_down[A_XBTN])  schedule_tap(A_XBTN, &hud);
    if (g_edge_down[A_MELEE]) schedule_tap(A_MELEE, &hud);
    if (g_edge_down[A_RELOAD]) schedule_tap(A_RELOAD, &hud);
}

int kbm_owns_mouse(void) { return g_owns_mouse; }

/* ---- diagnostics ------------------------------------------------------ */

/* Dump the live touch-widget list of the current state.  This is the registry
 * the engine hit-tests against, so it is the authoritative HUD layout. */
void kbm_probe_tick(int frame)
{
    const char *every = getenv("RECOMP_KBM_PROBE_EVERY");
    if (!getenv("RECOMP_KBM_PROBE")) return;
    if (every) { int n = atoi(every); if (n > 0 && (frame % n) != 0) return; }
    else if ((frame % 200) != 0) return;

    FILE *f = fopen("kbm_probe.txt", "a");
    if (!f) return;
    uint32_t st = current_state();
    fprintf(f, "=== frame %d state=%08x vt=%08x ===\n", frame, st, st ? rd32(st) : 0);
    int n = 0;
    for (uint32_t node = st ? rd32(st + 4) : 0; node && n < 256;
         node = rd32(node + 0), n++) {
        uint32_t w = rd32(node + 8);
        if (w < 0x100000u || w >= 0x40000000u) continue;
        fprintf(f, "  [%2d] w=%08x vt=%08x rect=%d,%d..%d,%d\n", n, w, rd32(w),
                s16(w + 0x170), s16(w + 0x172), s16(w + 0x174), s16(w + 0x176));
    }
    fprintf(f, "  (%d widgets)\n", n);
    fclose(f);
}
