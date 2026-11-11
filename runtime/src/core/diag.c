#define _CRT_SECURE_NO_WARNINGS
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- settings -------------------------------------------------------- */

static int g_on = 1;
static int g_verbose;
static unsigned g_frame;
static clock_t g_t0;

void diag_init(const char *so_path)
{
    g_t0 = clock();
    const char *e = getenv("NOVA_DIAG");
    if (e && *e) {
        int v = atoi(e);
        g_on = v > 0;
        g_verbose = v >= 2;
    } else {
        g_on = 1;
        g_verbose = 0;
    }
    if (so_path && *so_path) diag_so_symbols(so_path);
    if (g_on)
        diag_printf("diag on (verbose=%d, symbols=%s)", g_verbose,
                    diag_sym_count() ? "yes" : "no");
}

int diag_on(void) { return g_on; }
int diag_verbose(void) { return g_verbose; }

void diag_frame(void) { g_frame++; }

void diag_printf(const char *fmt, ...)
{
    if (!g_on) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    unsigned ms = (unsigned)((clock() - g_t0) * 1000 / CLOCKS_PER_SEC);
    fprintf(stderr, "[diag] f=%-6u t=%-6ums %s\n", g_frame, ms, msg);
    fflush(stderr);
}

/* ---- guest symbol table --------------------------------------------- */

typedef struct {
    uint32_t addr;
    uint32_t size;
    const char *name;
} DiagSym;

static DiagSym *g_syms;
static int g_nsyms;
static char *g_strtab;
static char g_namebuf[256];

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Turn "_ZN11Application14DeleteSaveGameEv" into "Application::DeleteSaveGame".
 * Only the common length-prefixed nested-name form is handled; anything else
 * is copied as-is. */
static const char *demangle(const char *m, char *out, size_t cap)
{
    size_t o = 0;
    if (m[0] == '_' && m[1] == 'Z') {
        const char *p = m + 2;
        while (*p && (*p < '0' || *p > '9')) p++;   /* skip N/K/V/R/T markers */
        while (*p && *p != 'E') {
            if (*p < '0' || *p > '9') break;        /* not a length prefix */
            int n = 0;
            while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            if (n <= 0 || (size_t)n + 3 > cap - o) break;
            if (o) { out[o++] = ':'; out[o++] = ':'; }
            memcpy(out + o, p, (size_t)n);
            o += (size_t)n;
            p += n;
        }
        out[o] = 0;
        if (o) return out;
    }
    snprintf(out, cap, "%s", m);
    return out;
}

int diag_sym_count(void) { return g_nsyms; }

static int sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const DiagSym *)a)->addr, y = ((const DiagSym *)b)->addr;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void diag_so_symbols(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 64) { fclose(f); return; }
    uint8_t *img = (uint8_t *)malloc((size_t)size);
    if (!img) { fclose(f); return; }
    if (fread(img, 1, (size_t)size, f) != (size_t)size) {
        free(img); fclose(f); return;
    }
    fclose(f);

    /* 32-bit ELF: e_shoff@0x20, e_shentsize@0x2e, e_shnum@0x30. */
    if (!(img[0] == 0x7f && img[1] == 'E' && img[2] == 'L' && img[3] == 'F') ||
        img[4] != 1) { free(img); return; }
    uint32_t shoff = le32(img + 0x20);
    uint16_t shentsize = le16(img + 0x2e);
    uint16_t shnum = le16(img + 0x30);
    if (!shoff || !shentsize) { free(img); return; }

    uint32_t sym_off = 0, sym_size = 0, sym_link = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (size_t)i * shentsize;
        if ((shoff + (size_t)i * shentsize + shentsize) > (uint32_t)size) break;
        if (le32(sh + 4) == 2) {                       /* SHT_SYMTAB */
            sym_off = le32(sh + 16);
            sym_size = le32(sh + 20);
            sym_link = le32(sh + 24);
            break;
        }
    }
    if (!sym_off || !sym_size || sym_link >= shnum) { free(img); return; }
    const uint8_t *strsh = img + shoff + (size_t)sym_link * shentsize;
    uint32_t str_off = le32(strsh + 16), str_size = le32(strsh + 20);
    if (!str_off || !str_size ||
        str_off + str_size > (uint32_t)size) { free(img); return; }

    g_strtab = (char *)malloc(str_size + 1);
    if (!g_strtab) { free(img); return; }
    memcpy(g_strtab, img + str_off, str_size);
    g_strtab[str_size] = 0;

    int cap = (int)(sym_size / 16);
    g_syms = (DiagSym *)malloc(sizeof(DiagSym) * (size_t)(cap ? cap : 1));
    if (!g_syms) { free(img); return; }
    for (int i = 0; i < cap; i++) {
        const uint8_t *e = img + sym_off + (size_t)i * 16;
        if ((sym_off + (size_t)i * 16 + 16) > (uint32_t)size) break;
        uint32_t nameoff = le32(e + 0);
        uint32_t value = le32(e + 4);
        uint32_t sz = le32(e + 8);
        uint8_t info = e[12];
        uint16_t shndx = le16(e + 14);
        int type = info & 0xf;
        if (!value || !shndx || nameoff >= str_size) continue;
        if (type != 1 && type != 2) continue;          /* OBJECT, FUNC */
        g_syms[g_nsyms].addr = value;
        g_syms[g_nsyms].size = sz;
        g_syms[g_nsyms].name = g_strtab + nameoff;
        g_nsyms++;
    }
    qsort(g_syms, (size_t)g_nsyms, sizeof(DiagSym), sym_cmp);
    free(img);
}

const char *diag_sym(uint32_t addr)
{
    if (!g_nsyms) {
        snprintf(g_namebuf, sizeof(g_namebuf), "0x%08x", addr);
        return g_namebuf;
    }
    int lo = 0, hi = g_nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) {
        snprintf(g_namebuf, sizeof(g_namebuf), "0x%08x", addr);
        return g_namebuf;
    }
    char name[192];
    demangle(g_syms[best].name, name, sizeof(name));
    uint32_t off = addr - g_syms[best].addr;
    if (off)
        snprintf(g_namebuf, sizeof(g_namebuf), "%s+0x%x", name, off);
    else
        snprintf(g_namebuf, sizeof(g_namebuf), "%s", name);
    return g_namebuf;
}
