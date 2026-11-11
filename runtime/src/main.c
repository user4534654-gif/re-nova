/* Boot and frame loop for the N.O.V.A. HD 1.1.4 static recompilation.
 *
 * The translated library (libnova.so) keeps its original JNI entry points.
 * This host drives them the same way the game's Java activity does:
 *
 *   NovaRenderer.onSurfaceCreated -> nativeInit(tier, 1, w, h, lang)
 *   NovaRenderer.onSurfaceChanged -> nativeResize(w, h)
 *   NovaRenderer.onDrawFrame     -> nativeRender()
 *   Nova.onTouchEvent            -> nativeTouchPressed/Moved/Released(x, y, id)
 *
 * Everything else (resource loading, audio, video) goes through the fake
 * JNIEnv set up by core/jni.c.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "guest.h"
#include "diag.h"
#include "gl/gl_internal.h"
#include "platform/kbm.h"
#include "platform/faststart.h"
#include "platform/movie.h"

#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <math.h>
#include <stdlib.h>

/* ---- creator link ---------------------------------------------------- */

#define CREATOR_URL "https://www.media-encrypt.online/links"

/* ---- per-user preferences (input.ini) -------------------------------- */

static const char *prefs_path(void)
{
    static char path[MAX_PATH];
    static int init;
    if (!init) {
        const char *la = getenv("LOCALAPPDATA");
        if (la && *la) snprintf(path, sizeof(path), "%s\\Nova\\input.ini", la);
        else snprintf(path, sizeof(path), "input.ini");
        init = 1;
    }
    return path;
}

static int ini_int(const char *path, const char *key, int def)
{
    FILE *f = fopen(path, "r");
    if (!f) return def;
    char line[256];
    size_t klen = strlen(key);
    int value = def;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, klen) != 0) continue;
        char sep = p[klen];
        if (sep != ' ' && sep != '\t' && sep != '=') continue;
        char *q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        value = atoi(q + 1);
        break;
    }
    fclose(f);
    return value;
}

static DWORD WINAPI open_creator_link(LPVOID param)
{
    /* Let the game window come up first; the link is branding, not a gate. */
    Sleep(2500);
    fprintf(stderr, "[link] opening %s\n", (const char *)param);
    ShellExecuteA(NULL, "open", (const char *)param, NULL, NULL, SW_SHOWNORMAL);
    return 0;
}

/* ---- frame pacing ---------------------------------------------------- */

static int g_frame_cap;
static const char *g_start_level;

void platform_set_frame_cap(int cap)
{
    if (cap < 0) cap = 0;
    g_frame_cap = cap;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
typedef HANDLE (WINAPI *CreateWaitableTimerExWFn)(LPSECURITY_ATTRIBUTES, LPCWSTR,
                                                  DWORD, DWORD);

static void frame_limit(void)
{
    static int active_cap = -1;
    static LARGE_INTEGER freq;
    static double next;
    static HANDLE timer;
    static int timer_tried;

    if (g_frame_cap != active_cap) {
        active_cap = g_frame_cap;
        next = 0.0;
        if (active_cap > 0) timeBeginPeriod(1);
        fprintf(stderr, "[win] frame cap %d FPS\n", active_cap);
    }
    if (active_cap <= 0) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);

    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    double now = (double)q.QuadPart / (double)freq.QuadPart;
    if (next <= 0.0) next = now;
    next += 1.0 / (double)active_cap;

    if (!timer_tried) {
        timer_tried = 1;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        CreateWaitableTimerExWFn create = k32
            ? (CreateWaitableTimerExWFn)(void *)GetProcAddress(k32, "CreateWaitableTimerExW")
            : NULL;
        if (create)
            timer = create(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                           TIMER_ALL_ACCESS);
    }
    if (timer) {
        double wait = next - now;
        if (wait > 0.0010) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((wait - 0.0005) * 10000000.0);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, INFINITE);
        }
    } else {
        for (;;) {
            QueryPerformanceCounter(&q);
            now = (double)q.QuadPart / (double)freq.QuadPart;
            if (now >= next - 0.0015) break;
            Sleep(1);
        }
    }
}

/* ---- JNI helpers ----------------------------------------------------- */

/* N.O.V.A.'s library is com.gameloft.android.TBFV.GloftNOHP.ML. */
static const char *g_native_prefix = "Java_com_gameloft_android_TBFV_GloftNOHP_ML_";

static uint32_t native_sym(const char *suffix)
{
    char name[512];
    snprintf(name, sizeof(name), "%s%s", g_native_prefix, suffix);
    return guest_symbol(name);
}

static void dump_regs(CPU *c)
{
    fprintf(stderr, "pc=%08x lr=%08x sp=%08x cpsr=%08x\n",
            c->pc, c->r[14], c->r[13], c->cpsr);
    for (int i = 0; i < 13; i++)
        fprintf(stderr, "r%-2d=%08x%s", i, c->r[i], (i % 4 == 3) ? "\n" : " ");
    fprintf(stderr, "\n");
}

/* Call a JNI entry point.  r0/r1 are the classic (env, thisOrClass) pair;
 * r2/r3 and any stack words are the method arguments. */
static void call_native(CPU *c, const char *suffix, uint32_t r2, uint32_t r3,
                        const uint32_t *stack, int nstack)
{
    uint32_t a = native_sym(suffix);
    if (!a) { fprintf(stderr, "[boot] missing native %s\n", suffix); return; }
    if (g_verbose) fprintf(stderr, "[boot] -> %s\n", suffix);
    c->r[0] = jni_env_addr();
    c->r[1] = 0x2001;
    c->r[2] = r2;
    c->r[3] = r3;
    c->r[14] = 0;
    uint32_t oldsp = c->r[13];
    if (nstack) {
        c->r[13] = (c->r[13] - (uint32_t)(nstack * 4)) & ~7u;
        for (int i = 0; i < nstack; i++) wr32(c->r[13] + (uint32_t)(i * 4), stack[i]);
    }
    guest_call(c, a);
    c->r[13] = oldsp;
    if (g_verbose) fprintf(stderr, "[boot] <- %s r0=%08x\n", suffix, c->r[0]);
}

static void touch_call(CPU *c, const char *suffix, int x, int y, int id)
{
    uint32_t st[1] = { (uint32_t)id };
    platform_client_to_logical(&x, &y);
    call_native(c, suffix, (uint32_t)x, (uint32_t)y, st, 1);
}

/* ---- host-injected touches (PC keyboard/mouse mode) ------------------ */

static CPU *g_boot_cpu;

static void host_touch(CPU *c, const char *suffix, int x, int y, int id)
{
    uint32_t st[1] = { (uint32_t)id };
    call_native(c, suffix, (uint32_t)x, (uint32_t)y, st, 1);
}

void host_touch_press(int x, int y, int id)
{
    if (g_boot_cpu) host_touch(g_boot_cpu, "Nova_nativeTouchPressed", x, y, id);
}
void host_touch_move(int x, int y, int id)
{
    if (g_boot_cpu) host_touch(g_boot_cpu, "Nova_nativeTouchMoved", x, y, id);
}
void host_touch_release(int x, int y, int id)
{
    if (g_boot_cpu) host_touch(g_boot_cpu, "Nova_nativeTouchReleased", x, y, id);
}

uint32_t host_call(uint32_t fn, uint32_t r0, uint32_t r1)
{
    CPU *c = g_boot_cpu;
    if (!c || !fn) return 0;
    CPU saved = *c;
    c->r[0] = r0;
    c->r[1] = r1;
    c->r[14] = 0;
    guest_call(c, fn);
    uint32_t ret = c->r[0];
    *c = saved;
    return ret;
}

/* ---- locating the original library ----------------------------------- */

static int host_file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static const char *find_game_so(void)
{
    static char alt[8][MAX_PATH * 2];
    static const char *cands[] = {
        "libnova.so",
        "gamefiles/libnova.so",
        "gamefiles/lib/armeabi-v7a/libnova.so",
        "original/libnova.so",
        "lib/armeabi-v7a/libnova.so",
    };
    const size_t ncand = sizeof(cands) / sizeof(cands[0]);
    const char *env = getenv("RECOMP_SO");
    if (env && *env) return env;

    for (size_t i = 0; i < ncand; i++)
        if (host_file_exists(cands[i])) return cands[i];

    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n > 0 && n < sizeof(exe)) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = 0;
            size_t k = 0;
            for (size_t i = 0; i < ncand && k < 8; i++) {
                snprintf(alt[k], sizeof(alt[k]), "%s\\%s", exe, cands[i]);
                if (host_file_exists(alt[k])) return alt[k];
                k++;
            }
        }
    }
    return cands[0];
}

/* ---- input ----------------------------------------------------------- */

static int g_was_down;
static int g_last_x, g_last_y;

/* RECOMP_TAP="frame:x:y;frame:x:y" injects a press at the given frame and the
 * matching release two frames later, so unattended runs can walk the menus. */
#define MAX_TAPS 8
static int tap_frames[MAX_TAPS], tap_x[MAX_TAPS], tap_y[MAX_TAPS], tap_n = -1;
static int tap_release = -1, tap_rx, tap_ry;
static int tap_moved;
static int tap_hold = 2;

static void tap_script_init(void)
{
    if (tap_n >= 0) return;
    tap_n = 0;
    if (getenv("RECOMP_TAP_HOLD")) tap_hold = atoi(getenv("RECOMP_TAP_HOLD"));
    if (tap_hold < 2) tap_hold = 2;
    const char *s = getenv("RECOMP_TAP");
    while (s && *s && tap_n < MAX_TAPS) {
        int f, x, y;
        if (sscanf(s, "%d:%d:%d", &f, &x, &y) == 3) {
            tap_frames[tap_n] = f; tap_x[tap_n] = x; tap_y[tap_n] = y; tap_n++;
        }
        const char *semi = strchr(s, ';');
        if (!semi) break;
        s = semi + 1;
    }
}

static void poll_tap_script(CPU *c, int frame)
{
    tap_script_init();
    if (tap_release >= 0 && !tap_moved && getenv("RECOMP_TAP_MOVE")
            && frame >= tap_release - tap_hold + 1) {
        touch_call(c, "Nova_nativeTouchMoved", tap_rx, tap_ry, 0);
        tap_moved = 1;
    }
    if (tap_release >= 0 && frame >= tap_release) {
        touch_call(c, "Nova_nativeTouchReleased", tap_rx, tap_ry, 0);
        tap_release = -1;
    }
    for (int i = 0; i < tap_n; i++) {
        if (tap_frames[i] == frame) {
            touch_call(c, "Nova_nativeTouchPressed", tap_x[i], tap_y[i], 0);
            tap_rx = tap_x[i]; tap_ry = tap_y[i];
            tap_release = frame + tap_hold;
            tap_moved = 0;
        }
    }
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_quit = 1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void poll_touch(CPU *c)
{
    if (!g_hwnd) return;
    if (getenv("RECOMP_NO_MOUSE")) return;
    POINT p;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p)) return;
    int x = (int)p.x, y = (int)p.y;
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    if (down && !g_was_down) {
        touch_call(c, "Nova_nativeTouchPressed", x, y, 0);
        g_last_x = x; g_last_y = y;
    } else if (down && (x != g_last_x || y != g_last_y)) {
        touch_call(c, "Nova_nativeTouchMoved", x, y, 0);
        g_last_x = x; g_last_y = y;
    } else if (!down && g_was_down) {
        touch_call(c, "Nova_nativeTouchReleased", g_last_x, g_last_y, 0);
    }
    g_was_down = down;
}

/* ---- boot ------------------------------------------------------------ */

static int run_boot(const char *so)
{
    if (guest_mem_init() != 0) return 1;
    guest_register_generated();
    guest_register_imports();
    guest_register_plt_imports();
    jni_init();
    if (guest_load_elf_auto(so) != 0) return 2;
    diag_init(so);

    CPU cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[13] = 0xBF800000;
    cpu.cpsr = 0x10;
    cpu.tid = 1;
    g_boot_cpu = &cpu;

    uint32_t m_env = guest_symbol("mEnv");
    if (m_env) wr32(m_env, jni_env_addr());

    /* Windowed by default; Alt+Enter or F11 toggles borderless fullscreen. */
    SetProcessDPIAware();
    int win_w = ini_int(prefs_path(), "width", 1280);
    int win_h = ini_int(prefs_path(), "height", 720);
    int start_fullscreen = ini_int(prefs_path(), "fullscreen", 0) ? 1 : 0;
    int movies = ini_int(prefs_path(), "movies", 1) ? 1 : 0;
    int open_link = ini_int(prefs_path(), "open_link", 1) ? 1 : 0;
    if (getenv("RECOMP_FULLSCREEN"))
        start_fullscreen = atoi(getenv("RECOMP_FULLSCREEN")) ? 1 : 0;
    if (getenv("NOVA_NO_MOVIE") || getenv("RECOMP_NO_MOVIE")) movies = 0;
    if (getenv("NOVA_NO_LINK")) open_link = 0;
    if (win_w < 320) win_w = 1280;
    if (win_h < 200) win_h = 720;
    {
        const char *res = getenv("RECOMP_RES");
        const char *ew = getenv("RECOMP_WIDTH");
        const char *eh = getenv("RECOMP_HEIGHT");
        if (res) sscanf(res, "%dx%d", &win_w, &win_h);
        if (ew) win_w = atoi(ew);
        if (eh) win_h = atoi(eh);
        if (win_w < 320) win_w = 320;
        if (win_h < 200) win_h = 200;
    }

    /* The recompiled engine lays its UI out in a fixed pixel canvas and never
     * scales it, so drive it at its design resolution and let the GL layer map
     * that canvas onto the real window (see scale_view_rect / gl_Viewport).
     * RECOMP_ENGINE_RES="WxH" or "client" overrides. */
    int eng_w = 1280, eng_h = 720;
    {
        const char *er = getenv("RECOMP_ENGINE_RES");
        if (er && (er[0] == 'c' || er[0] == 'C')) { eng_w = win_w; eng_h = win_h; }
        else if (er) sscanf(er, "%dx%d", &eng_w, &eng_h);
        if (eng_w < 320) eng_w = 1280;
        if (eng_h < 200) eng_h = 720;
    }
    int vsync = getenv("RECOMP_NO_VSYNC") ? 0 : 1;
    int fps = 0;
    if (getenv("RECOMP_FPS")) { fps = atoi(getenv("RECOMP_FPS")); vsync = 0; }

    fprintf(stderr, "[boot] resolution %dx%d\n", win_w, win_h);
    if (platform_init(win_w, win_h) != 0) return 7;
    if (start_fullscreen) platform_set_fullscreen(1);
    if (open_link) {
        HANDLE th = CreateThread(NULL, 0, open_creator_link,
                                 (void *)CREATOR_URL, 0, NULL);
        if (th) CloseHandle(th);
    }
    platform_set_vsync(vsync);
    platform_set_frame_cap(vsync ? 0 : fps);
    platform_set_aspect_fit((int64_t)eng_w * win_h != (int64_t)eng_h * win_w);
    platform_set_cpu(&cpu);

    fprintf(stderr, "[boot] running init_array\n");
    guest_run_init_array(&cpu);
    fprintf(stderr, "[boot] init_array done\n");

    extern void platform_set_logical_size(int w, int h);
    platform_set_logical_size(eng_w, eng_h);

    /* Boot chain mirrors the Java activity. */
    call_native(&cpu, "GLResLoader_nativeInit", 0, 0, NULL, 0);
    call_native(&cpu, "GLMediaPlayer_nativeInit", 0, 0, NULL, 0);
    call_native(&cpu, "Nova_nativeInit", 0, 0, NULL, 0);
    call_native(&cpu, "NovaRenderer_nativeGetJNIEnv", 0, 0, NULL, 0);

    int tier = getenv("RECOMP_TIER") ? atoi(getenv("RECOMP_TIER")) : 1;
    int lang = getenv("RECOMP_LANG") ? atoi(getenv("RECOMP_LANG")) : 0;
    uint32_t args[3] = { (uint32_t)eng_w,
                         (uint32_t)eng_h,
                         (uint32_t)lang };
    fprintf(stderr, "[boot] NovaRenderer.nativeInit(tier=%d, 1, %u, %u, lang=%d)\n",
            tier, args[0], args[1], lang);
    call_native(&cpu, "NovaRenderer_nativeInit", (uint32_t)tier, 1, args, 3);
    call_native(&cpu, "NovaRenderer_nativeResize", args[0], args[1], NULL, 0);
    fprintf(stderr, "[boot] appInit returned\n");

    faststart_init(g_start_level ? g_start_level : getenv("NOVA_START_LEVEL"));

    int frame = 0;
    int max_frames = getenv("RECOMP_MAX_FRAMES") ? atoi(getenv("RECOMP_MAX_FRAMES")) : 0;
    while (!platform_should_close()) {
        pump_messages();
        if (platform_should_close()) break;
        if (max_frames > 0 && frame >= max_frames) break;
        kbm_tick(frame);
        if (!kbm_owns_mouse()) poll_touch(&cpu);
        poll_tap_script(&cpu, frame);
        faststart_tick(&cpu, frame);
        faststart_trace_state();
        kbm_probe_tick(frame);
        call_native(&cpu, "NovaRenderer_nativeRender", 0, 0, NULL, 0);
        /* The engine enters GS_Intro on the first frames and asks Java for
         * NOVA.mp4; play it here, with the engine paused, exactly like the
         * Android activity did.  Later career/trailer requests use the same
         * path. */
        if (movie_has_pending()) {
            if (movies)
                movie_play_pending(platform_client_width(), platform_client_height());
            else
                movie_discard_pending();
        }
        platform_swap();
        frame_limit();
        frame++;
        diag_frame();
        if ((frame % 120) == 0 && getenv("RECOMP_FRAME_STATS")) {
            int tw, th, tf, tif;
            gles1_last_tex(&tw, &th, &tf, &tif);
            const float *cc = gles1_clear_color();
            fprintf(stderr, "[boot] frame %d draws=%llu clears=%llu tex=%llu/%llu "
                    "clear=%.2f/%.2f/%.2f/%.2f tex=%dx%d fmt=%x/%x nonwhite=%d\n",
                    frame, (unsigned long long)gles1_draws(),
                    (unsigned long long)gles1_clears(),
                    (unsigned long long)gles1_uploaded(),
                    (unsigned long long)gles1_generated(),
                    cc[0], cc[1], cc[2], cc[3], tw, th, tif, tf, gles1_nonwhite());
        }
    }
    fprintf(stderr, "[boot] leaving main loop after %d frames\n", frame);
    dump_regs(&cpu);
    return 0;
}

int main(int argc, char **argv)
{
#ifdef RECOMP_GUI
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
        if (n > 0 && n < sizeof(exe)) {
            char *slash = strrchr(exe, '\\');
            if (slash) { *slash = 0; SetCurrentDirectoryA(exe); }
        }
        {
            char dir[MAX_PATH], path[MAX_PATH];
            const char *la = getenv("LOCALAPPDATA");
            if (la && *la) {
                snprintf(dir, sizeof(dir), "%s\\Nova", la);
                CreateDirectoryA(dir, NULL);
                snprintf(path, sizeof(path), "%s\\nova.log", dir);
            } else {
                snprintf(path, sizeof(path), "nova.log");
            }
            freopen(path, "w", stderr);
            freopen(path, "a", stdout);
        }
    }
    int boot = 1;
#else
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int boot = 1;
    if (getenv("RECOMP_HIDE_CONSOLE") || getenv("RECOMP_OFFSCREEN")) {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);
    }
#endif
    g_verbose = getenv("RECOMP_VERBOSE") ? 1 : 0;
    const char *so = find_game_so();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "boot")) boot = 1;
        else if (!strcmp(argv[i], "-q")) g_verbose = 0;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if ((!strcmp(argv[i], "--level") || !strcmp(argv[i], "-l")) && i + 1 < argc)
            g_start_level = argv[++i];
        else so = argv[i];
    }
    (void)boot;
    int rc = run_boot(so);
#ifdef RECOMP_GUI
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "N.O.V.A. failed to start (code %d).\n\nSee nova.log in "
                 "%%LOCALAPPDATA%%\\Nova for details.", rc);
        MessageBoxA(NULL, msg, "N.O.V.A.", MB_ICONERROR | MB_OK);
    }
#endif
    return rc;
}
