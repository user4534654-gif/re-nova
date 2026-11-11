/* Fast-start: load a level directly at boot instead of walking the menus.
 *
 * Why this exists: during development the menu trek (logo -> splash -> main
 * menu -> New Game -> difficulty) costs several seconds and a tap every run.
 * This module optionally skips it and drops straight into gameplay.
 *
 * How it works (all host-side, no generated code touched):
 *
 *   1. We watch the engine's state stack (Application+4, top index at +0x34).
 *   2. GS_Splash waits for a tap; we set its "advance" flag instead.
 *   3. Once GS_MainMenu is current we replay the New Game path, but without
 *      the taps: the engine's "New Game" branch builds GS_Difficulty(8) and,
 *      when a difficulty is chosen, loads one hard-coded level file.  We build
 *      that same GS_Difficulty(8), then call its DifficultySelected(), which
 *      clears the menu stack and pushes GS_Loading for that level.
 *   4. The hard-coded level name (a writable .data string) is overwritten
 *      with the requested level before we trigger the load.
 *
 * This reuses the engine's own loading code end to end (string handling,
 * save cleanup, GS_Loading construction), so it behaves exactly like a real
 * New Game with the level swapped out.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "guest.h"
#include "diag.h"
#include "faststart.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- engine addresses (guest virtual; g_load_bias is 0) -------------- */

#define ADDR_OP_NEW        0x0019c104u  /* operator new(unsigned int)          */
#define ADDR_GS_DIFF_CTOR  0x001c7870u  /* GS_Difficulty::GS_Difficulty(int)   */
#define ADDR_GS_DIFF_SEL   0x001c7b98u  /* GS_Difficulty::DifficultySelected() */
#define ADDR_PUSH_STATE    0x001be348u  /* gxStateStack::PushState(state)      */
#define ADDR_APP_SINGLETON 0x0055ebe4u  /* Application::GetInstance singleton  */
#define ADDR_CURRENT_LOC   0x00558704u  /* GS_BaseMenu::m_currentLocation      */

/* The "New Game" difficulty path loads this literal level name (0x004e1080 is
 * writable .data, followed by "level_01.lvl" at 0x004e1094). */
#define ADDR_NEWGAME_LEVEL 0x004e1080u
#define NEWGAME_LEVEL_CAP  19

/* GS_Difficulty object size (from GS_MainMenu::Update: new(0x2c)). */
#define GSD_SIZE 0x2cu
/* The "New Game" entry opens GS_Difficulty with argument 8. */
#define GSD_NEWGAME 8

/* Object vtable slots hold the vtable symbol + 8. */
#define VT_GS_LOGO       0x00520a60u
#define VT_GS_SPLASH     0x00521288u
#define VT_GS_MAINMENU   0x005210d0u
#define VT_GS_DIFFICULTY 0x00520f20u

/* GS_Splash advance flag; set it and the next Update moves to the menu. */
#define SPLASH_ADVANCE 0x21
/* GS_Logo phase/timer (timer counts down from 3000 ms). */
#define LOGO_PHASE 0x1c
#define LOGO_TIMER 0x18

static const char *k_level_files[] = {
    "jr_tutorial_old.lvl", "level_01.lvl", "level_02.lvl",
    "jr_Level_01_part1.lvl", "jr_Level_01_part2.lvl",
    "bk_level_01.lvl", "bk_level_02.lvl", "jr_level_02.lvl",
    "RF_level_01.lvl", "mc_level_01.lvl", "mc_level_02.lvl",
    "mc_level_03.lvl", "mc_level_boss.lvl",
};
#define LEVEL_COUNT ((int)(sizeof(k_level_files) / sizeof(k_level_files[0])))

static int  g_enabled;
static int  g_phase;          /* 0 wait, 1 difficulty open, 2 done */
static char g_level[96];

/* Which location pack holds each levelFile entry.  The engine mounts the
 * location pack named by _defaultLocations[m_currentLocation]; the tutorial is
 * in the jungle pack, which is why the untouched default (1) loads it.  Order
 * of _defaultLocations: 0 ship, 1 jungle, 2 monolitic, 3 robotic, 4 bunker. */
static const int k_level_location[] = {
    1, /*  0 jr_tutorial_old.lvl    : jungle    */
    0, /*  1 level_01.lvl            : ship      */
    0, /*  2 level_02.lvl            : ship      */
    1, /*  3 jr_Level_01_part1.lvl   : jungle    */
    1, /*  4 jr_Level_01_part2.lvl   : jungle    */
    4, /*  5 bk_level_01.lvl         : bunker    */
    4, /*  6 bk_level_02.lvl         : bunker    */
    1, /*  7 jr_level_02.lvl         : jungle    */
    3, /*  8 RF_level_01.lvl         : robotic   */
    2, /*  9 mc_level_01.lvl         : monolitic */
    2, /* 10 mc_level_02.lvl         : monolitic */
    2, /* 11 mc_level_03.lvl         : monolitic */
    2, /* 12 mc_level_boss.lvl       : monolitic */
};
static int g_location = -1;

static uint32_t app_instance(void)
{
    uint32_t a = rd32(ADDR_APP_SINGLETON);
    if (a < 0x00100000u || a >= 0x40000000u) return 0;
    return a;
}

static uint32_t current_state(void)
{
    uint32_t app = app_instance();
    if (!app) return 0;
    int32_t top = s32(app + 4 + 0x34);
    if (top < 0 || top > 32) return 0;
    return rd32(app + 4 + (uint32_t)top * 4);
}

static uint32_t g_trace_last;
static int g_trace_valid;

void faststart_trace_state(void)
{
    uint32_t st = current_state();
    uint32_t vt = st ? rd32(st) : 0;
    if (g_trace_valid && vt == g_trace_last) return;
    g_trace_last = vt;
    g_trace_valid = 1;
    if (!st)
        diag_printf("state -> (app not ready)");
    else
        diag_printf("state -> %s @ %08x", diag_sym(vt), st);
}

void faststart_init(const char *spec)
{
    if (!spec || !*spec) return;

    char *end = NULL;
    long n = strtol(spec, &end, 10);
    if (end && *end == '\0' && n >= 0 && n < LEVEL_COUNT) {
        snprintf(g_level, sizeof(g_level), "%s", k_level_files[n]);
        g_location = k_level_location[n];
    } else {
        snprintf(g_level, sizeof(g_level), "%s", spec);
        size_t len = strlen(g_level);
        if (len < 4 || strcmp(g_level + len - 4, ".lvl") != 0)
            snprintf(g_level + len, sizeof(g_level) - len, ".lvl");
        /* Match the name against the built-in table to find its pack. */
        for (int i = 0; i < LEVEL_COUNT; i++) {
            if (!strcmp(g_level, k_level_files[i])) { g_location = k_level_location[i]; break; }
        }
    }
    g_enabled = 1;
    fprintf(stderr, "[faststart] target level: %s (location %d)\n", g_level, g_location);
}

/* Call a guest function that takes (r0[, r1]) and returns to us. */
static void gcall(CPU *cpu, uint32_t addr, uint32_t r0, uint32_t r1)
{
    cpu->r[0] = r0;
    cpu->r[1] = r1;
    cpu->r[14] = 0;
    guest_call(cpu, addr);
}

static void write_level_name(void)
{
    size_t len = strlen(g_level);
    if (len > NEWGAME_LEVEL_CAP) len = NEWGAME_LEVEL_CAP;
    memcpy(G(ADDR_NEWGAME_LEVEL), g_level, len);
    wr8(ADDR_NEWGAME_LEVEL + (uint32_t)len, 0);
}

void faststart_tick(CPU *cpu, int frame)
{
    (void)frame;
    if (!g_enabled || g_phase >= 2) return;

    uint32_t st = current_state();
    if (!st) return;
    uint32_t vt = rd32(st);

    if (g_phase == 0) {
        if (vt == VT_GS_SPLASH) {
            wr8(st + SPLASH_ADVANCE, 1);
            return;
        }
        if (vt == VT_GS_LOGO) {
            /* The logo counts down 3 s once past its init phase; cut that
             * short without disturbing the one-time InitFromLogo call. */
            if (s32(st + LOGO_PHASE) >= 2)
                wr32(st + LOGO_TIMER, 0);
            return;
        }
        if (vt == VT_GS_MAINMENU) {
            if (g_location >= 0) wr32(ADDR_CURRENT_LOC, (uint32_t)g_location);
            write_level_name();
            gcall(cpu, ADDR_OP_NEW, GSD_SIZE, 0);
            uint32_t obj = cpu->r[0];
            if (obj < 0x00100000u || obj >= 0x40000000u) {
                fprintf(stderr, "[faststart] allocation failed\n");
                g_phase = 2;
                return;
            }
            gcall(cpu, ADDR_GS_DIFF_CTOR, obj, GSD_NEWGAME);
            gcall(cpu, ADDR_PUSH_STATE, app_instance() + 4, obj);
            fprintf(stderr, "[faststart] opening difficulty (New Game), level '%s'\n",
                    g_level);
            g_phase = 1;
        }
        return;
    }

    /* phase 1: difficulty screen is up; select it without a tap. */
    if (vt == VT_GS_DIFFICULTY) {
        fprintf(stderr, "[faststart] selecting difficulty -> load '%s'\n", g_level);
        gcall(cpu, ADDR_GS_DIFF_SEL, st, 0);
        g_phase = 2;
    }
}
