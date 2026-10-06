/* bbport: cheat engine (design-review section 4). Loads GoldHEN / shadPS4 cheat JSON files for
 * the running game serial+version, applies their memory mods as raw byte writes into the eboot
 * image, and lets the in-game menu toggle them.
 *
 * Format subset (whitelist): {name, id, version, process, credits,
 *   master: {challenged, memory: [{offset, on}]},
 *   mods: [{name, description, type, memory: [{offset, on, off}]}]}.
 * Offsets are module-relative (eboot vaddr 0 = image offset 0, the BBPATCH2 convention: the
 * community jump sites resolve to their caves exactly under it). on/off are hex byte strings
 * written verbatim. The guest is real x86-64 code, so cave code and relative jumps from the
 * repositories run as written — the same bytes GoldHEN writes on hardware. The game itself
 * executes the patched code: no host-side per-frame emulation exists or is needed, so the
 * "master code" sections are not applied implicitly either (shadPS4's own UI ignores them);
 * they surface as an opt-in entry because repository files use them for optional trampoline
 * scaffolding.
 *
 * Safety: cheat files are data, not code. The parser accepts only the fields above with hard
 * limits; every write is validated against the image segments before any byte lands; a failed
 * write disables its mod and never aborts the game. State persists in state.txt next to the
 * JSONs (TSV: <file>\t<mod name> lines, "M" for the master section). */
#include "runtime_cheats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#define CHEAT_MAX_FILES 16
#define CHEAT_MAX_MODS 128
#define CHEAT_MAX_WRITES 64
#define CHEAT_MAX_BYTES 4096
#define CHEAT_MAX_FILE (4u << 20)
#define CHEAT_MAX_STR 256

typedef struct {
    uint64_t offset;
    uint32_t on_size, off_size;
    unsigned char *on, *off; /* off == NULL: one-way code */
} CheatWrite;

typedef struct {
    char *name, *desc;
    int enabled;
    CheatWrite *writes;
    int write_count;
} CheatMod;

typedef struct {
    char *path, *name; /* path and display name (the JSON "name", else the file name) */
    char id[16], version[16], process[CHEAT_MAX_STR];
    CheatWrite *master;
    int master_count, master_applied, master_wanted;
    CheatMod *mods;
    int mod_count;
} CheatFile;

static CheatFile files[CHEAT_MAX_FILES];
static int file_count;
static char cheat_dir[1024] = "?";

/* ---------------------------------------------------------------- JSON scanner (whitelist) */

typedef struct { const char *p, *end; int depth; } JParser;

static void j_ws(JParser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) ++j->p;
}
static int j_peek(JParser *j) {
    j_ws(j);
    return j->p < j->end ? (unsigned char)*j->p : -1;
}
static int j_expect(JParser *j, char c) {
    if (j_peek(j) != (unsigned char)c) return 0;
    ++j->p;
    return 1;
}
static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Exactly four hex digits into out. */
static int j_hex4(JParser *j, unsigned *out) {
    if (j->end - j->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        int d = hex_nibble((unsigned char)j->p[i]);
        if (d < 0) return 0;
        v = v * 16 + (unsigned)d;
    }
    j->p += 4;
    *out = v;
    return 1;
}
/* String into out (NUL-terminated, truncated at cap), or discarded when out is NULL. */
static int j_string(JParser *j, char *out, size_t cap) {
    if (!j_expect(j, '"')) return 0;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            if (j->p >= j->end) return 0;
            char e = *j->p++;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned v;
                if (!j_hex4(j, &v)) return 0;
                if (v >= 0xD800 && v < 0xDC00) { /* high surrogate: \uDC00-\uDFFF must follow */
                    if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') return 0;
                    unsigned lo;
                    j->p += 2;
                    if (!j_hex4(j, &lo) || lo < 0xDC00 || lo >= 0xE000) return 0;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                } else if (v >= 0xDC00 && v < 0xE000) return 0; /* lone low surrogate */
                if (!out) continue;
                unsigned char utf[4];
                unsigned len;
                if (v < 0x80) {
                    utf[0] = (unsigned char)v;
                    len = 1;
                } else if (v < 0x800) {
                    utf[0] = (unsigned char)(0xC0 | (v >> 6));
                    utf[1] = (unsigned char)(0x80 | (v & 0x3F));
                    len = 2;
                } else if (v < 0x10000) {
                    utf[0] = (unsigned char)(0xE0 | (v >> 12));
                    utf[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
                    utf[2] = (unsigned char)(0x80 | (v & 0x3F));
                    len = 3;
                } else {
                    utf[0] = (unsigned char)(0xF0 | (v >> 18));
                    utf[1] = (unsigned char)(0x80 | ((v >> 12) & 0x3F));
                    utf[2] = (unsigned char)(0x80 | ((v >> 6) & 0x3F));
                    utf[3] = (unsigned char)(0x80 | (v & 0x3F));
                    len = 4;
                }
                if (n + len >= cap) return 0;
                memcpy(out + n, utf, len);
                n += len;
                continue;
            }
            default: return 0;
            }
        }
        if (out) {
            if (n + 1 >= cap) return 0;
            out[n++] = (char)c;
        }
    }
    if (j->p >= j->end) return 0;
    ++j->p;
    if (out) out[n] = 0;
    return 1;
}
/* Skips any value (object, array, string, number, literal). */
static int j_skip(JParser *j) {
    int c = j_peek(j);
    if (c == '"') return j_string(j, NULL, 0);
    if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        ++j->p;
        if (++j->depth > 32) return 0;
        for (;;) {
            c = j_peek(j);
            if (c == (unsigned char)close) { ++j->p; --j->depth; return 1; }
            if (c == -1) return 0;
            if (close == '}') {
                if (!j_string(j, NULL, 0) || !j_expect(j, ':')) return 0;
            }
            if (!j_skip(j)) return 0;
            c = j_peek(j);
            if (c == ',') { ++j->p; continue; }
            if (c == (unsigned char)close) { ++j->p; --j->depth; return 1; }
            return 0;
        }
    }
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' &&
           *j->p != '\t' && *j->p != '\n' && *j->p != '\r')
        ++j->p;
    return 1;
}

/* ------------------------------------------------------------------- files, mods, writes */

static int parse_hex_bytes(const char *s, unsigned char **out, uint32_t *size) {
    unsigned char *buf = malloc(CHEAT_MAX_BYTES);
    uint32_t n = 0;
    if (!buf) return 0;
    while (*s) {
        if (*s == ' ' || *s == '\t') { ++s; continue; }
        int hi = hex_nibble((unsigned char)*s++);
        int lo = hi >= 0 ? hex_nibble((unsigned char)*s++) : -1;
        if (lo < 0 || n >= CHEAT_MAX_BYTES) { free(buf); return 0; }
        buf[n++] = (unsigned char)(hi * 16 + lo);
    }
    if (!n) { free(buf); return 0; }
    unsigned char *tight = realloc(buf, n); /* the exact size: buffers are per write entry */
    if (tight) buf = tight;
    *out = buf;
    *size = n;
    return 1;
}
static int parse_hex_offset(const char *s, uint64_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t v = 0;
    int digits = 0;
    for (; *s; ++s) {
        int d = hex_nibble((unsigned char)*s);
        if (d < 0 || ++digits > 16) return 0;
        v = v * 16 + (uint64_t)d;
    }
    if (!digits) return 0;
    *out = v;
    return 1;
}
/* One memory entry: {offset, on[, off]}. on must be non-empty; an absent or empty off leaves
 * the write one-way. */
static int parse_write(JParser *j, CheatWrite *w) {
    char offset[40] = "", on[2 * CHEAT_MAX_BYTES + 8] = "", off[2 * CHEAT_MAX_BYTES + 8] = "";
    int has_off = 0;
    memset(w, 0, sizeof *w);
    if (!j_expect(j, '{')) return 0;
    for (;;) {
        int c = j_peek(j);
        if (c == (unsigned char)'}') { ++j->p; break; }
        if (c == -1) return 0;
        char key[64];
        if (!j_string(j, key, sizeof key) || !j_expect(j, ':')) return 0;
        if (!strcmp(key, "offset")) { if (!j_string(j, offset, sizeof offset)) return 0; }
        else if (!strcmp(key, "on")) { if (!j_string(j, on, sizeof on)) return 0; }
        else if (!strcmp(key, "off")) { has_off = 1; if (!j_string(j, off, sizeof off)) return 0; }
        else if (!j_skip(j)) return 0;
        c = j_peek(j);
        if (c == ',') { ++j->p; continue; }
        if (c == (unsigned char)'}') { ++j->p; break; }
        return 0;
    }
    if (!parse_hex_offset(offset, &w->offset)) goto fail;
    if (!parse_hex_bytes(on, &w->on, &w->on_size)) goto fail;
    if (has_off && *off && !parse_hex_bytes(off, &w->off, &w->off_size)) goto fail;
    return 1;
fail:
    free(w->on);
    free(w->off);
    memset(w, 0, sizeof *w);
    return 0;
}
static void free_writes(CheatWrite *ws, int n) {
    for (int i = 0; i < n; ++i) {
        free(ws[i].on);
        free(ws[i].off);
    }
    free(ws);
}
/* A memory array of a mod or the master section. */
static int parse_memory(JParser *j, CheatWrite **out, int *count) {
    CheatWrite *ws = calloc(CHEAT_MAX_WRITES, sizeof *ws);
    if (!ws || !j_expect(j, '[')) { free(ws); return 0; }
    int n = 0;
    for (;;) {
        int c = j_peek(j);
        if (c == (unsigned char)']') { ++j->p; break; }
        if (c == -1 || n >= CHEAT_MAX_WRITES) { free_writes(ws, n); return 0; }
        if (!parse_write(j, &ws[n])) { free_writes(ws, n); return 0; }
        ++n;
        c = j_peek(j);
        if (c == ',') { ++j->p; continue; }
        if (c == (unsigned char)']') { ++j->p; break; }
        free_writes(ws, n);
        return 0;
    }
    *out = ws;
    *count = n;
    return 1;
}
static int parse_mod(JParser *j, CheatMod *m) {
    char name[CHEAT_MAX_STR] = "", desc[CHEAT_MAX_STR] = "";
    CheatMod tmp;
    memset(m, 0, sizeof *m);
    memset(&tmp, 0, sizeof tmp);
    if (!j_expect(j, '{')) return 0;
    for (;;) {
        int c = j_peek(j);
        if (c == (unsigned char)'}') { ++j->p; break; }
        if (c == -1) goto fail;
        char key[64];
        if (!j_string(j, key, sizeof key) || !j_expect(j, ':')) goto fail;
        if (!strcmp(key, "name")) { if (!j_string(j, name, sizeof name)) goto fail; }
        else if (!strcmp(key, "description")) { if (!j_string(j, desc, sizeof desc)) goto fail; }
        else if (!strcmp(key, "memory")) {
            if (tmp.writes || !parse_memory(j, &tmp.writes, &tmp.write_count)) goto fail;
        } else if (!j_skip(j)) goto fail;
        c = j_peek(j);
        if (c == ',') { ++j->p; continue; }
        if (c == (unsigned char)'}') { ++j->p; break; }
        goto fail;
    }
    if (!name[0] || !tmp.write_count) goto fail;
    tmp.name = strdup(name);
    tmp.desc = strdup(desc);
    if (!tmp.name || !tmp.desc) goto fail;
    *m = tmp;
    return 1;
fail:
    free(tmp.name);
    free(tmp.desc);
    free_writes(tmp.writes, tmp.write_count);
    return 0;
}
/* One cheat file into cf; cf is zeroed by the caller and freed on failure. */
static int parse_cheat(JParser *j, CheatFile *cf) {
    if (!j_expect(j, '{')) return 0;
    for (;;) {
        int c = j_peek(j);
        if (c == (unsigned char)'}') { ++j->p; break; }
        if (c == -1) return 0;
        char key[64];
        if (!j_string(j, key, sizeof key) || !j_expect(j, ':')) return 0;
        if (!strcmp(key, "name")) {
            char name[CHEAT_MAX_STR];
            if (cf->name || !j_string(j, name, sizeof name)) return 0;
            cf->name = strdup(name);
            if (!cf->name) return 0;
        } else if (!strcmp(key, "id")) {
            if (!j_string(j, cf->id, sizeof cf->id)) return 0;
        } else if (!strcmp(key, "version")) {
            if (!j_string(j, cf->version, sizeof cf->version)) return 0;
        } else if (!strcmp(key, "process")) {
            if (!j_string(j, cf->process, sizeof cf->process)) return 0;
        } else if (!strcmp(key, "master")) {
            if (cf->master || !j_expect(j, '{')) return 0;
            for (;;) {
                c = j_peek(j);
                if (c == (unsigned char)'}') { ++j->p; break; }
                if (c == -1) return 0;
                char mkey[64];
                if (!j_string(j, mkey, sizeof mkey) || !j_expect(j, ':')) return 0;
                if (!strcmp(mkey, "memory")) {
                    if (!parse_memory(j, &cf->master, &cf->master_count)) return 0;
                } else if (!j_skip(j)) return 0;
                c = j_peek(j);
                if (c == ',') { ++j->p; continue; }
                if (c == (unsigned char)'}') { ++j->p; break; }
                return 0;
            }
        } else if (!strcmp(key, "mods")) {
            if (cf->mods) return 0;
            cf->mods = calloc(CHEAT_MAX_MODS, sizeof *cf->mods);
            if (!cf->mods || !j_expect(j, '[')) return 0;
            for (;;) {
                c = j_peek(j);
                if (c == (unsigned char)']') { ++j->p; break; }
                if (c == -1 || cf->mod_count >= CHEAT_MAX_MODS) return 0;
                if (!parse_mod(j, &cf->mods[cf->mod_count])) return 0;
                ++cf->mod_count;
                c = j_peek(j);
                if (c == ',') { ++j->p; continue; }
                if (c == (unsigned char)']') { ++j->p; break; }
                return 0;
            }
        } else if (!j_skip(j)) return 0;
        c = j_peek(j);
        if (c == ',') { ++j->p; continue; }
        if (c == (unsigned char)'}') { ++j->p; break; }
        return 0;
    }
    return cf->id[0] && cf->mod_count > 0;
}

/* ----------------------------------------------------------------------- applying writes */

static const char *write_entries(const CheatWrite *ws, int n, int use_on) {
    /* Enable follows the file order: cave bodies land before the hook jumps into them.
     * Disable walks it backwards: a live hook must never target a cleared cave, or the
     * game thread jumps into zeroed bytes and crashes between the two writes. */
    for (int i = 0; i < n; ++i) {
        const CheatWrite *w = &ws[use_on ? i : n - 1 - i];
        const uint32_t size = use_on ? w->on_size : w->off_size;
        if (size && !runtime_cheat_write(w->offset, use_on ? w->on : w->off, size))
            return "target is outside the game image";
    }
    return NULL;
}

/* ------------------------------------------------------------------------- persist state */

static void state_escape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s && n + 2 < cap; ++s) {
        if (*s == '\\' || *s == '\t' || *s == '\n') {
            out[n++] = '\\';
            out[n++] = *s == '\\' ? '\\' : *s == '\t' ? 't' : 'n';
        } else out[n++] = *s;
    }
    out[n] = 0;
}
static void state_unescape(char *s) {
    char *r = s;
    for (; *s; ++s) {
        if (*s == '\\' && (s[1] == '\\' || s[1] == 't' || s[1] == 'n')) {
            *r++ = s[1] == 't' ? '\t' : s[1] == 'n' ? '\n' : '\\';
            ++s;
        } else *r++ = *s;
    }
    *r = 0;
}
static void save_state(void) {
    char path[1100], tmp[1100], esc[600];
    snprintf(path, sizeof path, "%s/state.txt", cheat_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        printf("Cheats: cannot write %s\n", tmp);
        return;
    }
    for (int i = 0; i < file_count; ++i) {
        if (files[i].master_wanted) {
            state_escape(files[i].path, esc, sizeof esc);
            fprintf(f, "%s\tM\n", esc);
        }
        for (int m = 0; m < files[i].mod_count; ++m) {
            if (!files[i].mods[m].enabled) continue;
            state_escape(files[i].path, esc, sizeof esc);
            fprintf(f, "%s\t", esc);
            state_escape(files[i].mods[m].name, esc, sizeof esc);
            fprintf(f, "%s\n", esc);
        }
    }
    fclose(f);
    remove(path);
    if (rename(tmp, path)) printf("Cheats: cannot save state to %s\n", path);
}
static int state_line(FILE *f, char *file, size_t file_cap, char *mod, size_t mod_cap) {
    char line[1200];
    if (!fgets(line, sizeof line, f)) return 0;
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[--n] = 0;
    char *tab = strchr(line, '\t');
    if (!tab) return 1; /* blank or malformed: skip */
    *tab = 0;
    state_unescape(line);
    state_unescape(tab + 1);
    snprintf(file, file_cap, "%s", line);
    snprintf(mod, mod_cap, "%s", tab + 1);
    return 1;
}
static void load_state(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/state.txt", cheat_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char file[600], mod[600];
    while (state_line(f, file, sizeof file, mod, sizeof mod)) {
        CheatFile *cf = NULL;
        for (int i = 0; i < file_count && !cf; ++i)
            if (!strcmp(files[i].path, file)) cf = &files[i];
        if (!cf) continue;
        if (!strcmp(mod, "M")) { cf->master_wanted = 1; continue; }
        for (int m = 0; m < cf->mod_count; ++m)
            if (!strcmp(cf->mods[m].name, mod)) cf->mods[m].enabled = 1;
    }
    fclose(f);
}

/* ------------------------------------------------------------------------------- bridge */

static CheatFile *checked(int file) {
    return file >= 0 && file < file_count ? &files[file] : NULL;
}
int bbcheats_file_count(void) {
    return file_count;
}
const char *bbcheats_file_name(int file) {
    CheatFile *cf = checked(file);
    return cf ? (cf->name ? cf->name : cf->path) : "";
}
const char *bbcheats_file_dir(void) {
    return cheat_dir;
}
int bbcheats_mod_count(int file) {
    CheatFile *cf = checked(file);
    return cf ? cf->mod_count : 0;
}
const char *bbcheats_mod_name(int file, int mod) {
    CheatFile *cf = checked(file);
    return cf && mod >= 0 && mod < cf->mod_count ? cf->mods[mod].name : "";
}
const char *bbcheats_mod_desc(int file, int mod) {
    CheatFile *cf = checked(file);
    return cf && mod >= 0 && mod < cf->mod_count ? cf->mods[mod].desc : "";
}
int bbcheats_mod_enabled(int file, int mod) {
    CheatFile *cf = checked(file);
    return cf && mod >= 0 && mod < cf->mod_count ? cf->mods[mod].enabled : 0;
}
int bbcheats_mod_one_way(int file, int mod) {
    CheatFile *cf = checked(file);
    if (!cf || mod < 0 || mod >= cf->mod_count) return 0;
    for (int i = 0; i < cf->mods[mod].write_count; ++i)
        if (!cf->mods[mod].writes[i].off) return 1;
    return 0;
}
const char *bbcheats_toggle(int file, int mod, int enable) {
    CheatFile *cf = checked(file);
    if (!cf || mod < 0 || mod >= cf->mod_count) return "unknown cheat";
    CheatMod *m = &cf->mods[mod];
    if (m->enabled == (enable != 0)) return NULL;
    if (!enable && bbcheats_mod_one_way(file, mod))
        return "one-way code: restart the game to revert it";
    const char *error = write_entries(m->writes, m->write_count, enable);
    if (error) return error;
    m->enabled = enable != 0;
    save_state();
    return NULL;
}
int bbcheats_master_available(int file) {
    CheatFile *cf = checked(file);
    return cf && cf->master_count > 0;
}
const char *bbcheats_master_name(int file) {
    return bbcheats_master_available(file) ? "主代码（文件前置）" : "";
}
int bbcheats_master_enabled(int file) {
    CheatFile *cf = checked(file);
    return cf ? cf->master_applied : 0;
}
const char *bbcheats_master_toggle(int file, int enable) {
    CheatFile *cf = checked(file);
    if (!cf || !cf->master_count) return "no master code in this file";
    if (cf->master_applied == (enable != 0)) return NULL;
    if (!enable) return "already applied: restart the game to revert it";
    const char *error = write_entries(cf->master, cf->master_count, 1);
    if (error) return error;
    cf->master_applied = 1;
    cf->master_wanted = 1;
    save_state();
    return NULL;
}

/* ---------------------------------------------------------------------------------- boot */

static void free_file(CheatFile *cf) {
    free(cf->path);
    free(cf->name);
    free_writes(cf->master, cf->master_count);
    for (int m = 0; m < cf->mod_count; ++m) {
        free(cf->mods[m].name);
        free(cf->mods[m].desc);
        free_writes(cf->mods[m].writes, cf->mods[m].write_count);
    }
    free(cf->mods);
}
static int ends_with_json(const char *name) {
    size_t n = strlen(name);
    return n > 5 && !strcasecmp(name + n - 5, ".json");
}
void runtime_cheats_boot(const char *serial, const char *version) {
    const char *env = getenv("BB_CHEATS_DIR");
    if (env && *env) snprintf(cheat_dir, sizeof cheat_dir, "%s", env);
    else {
        const char *config = getenv("BB_CONFIG");
        if (config && *config) {
            snprintf(cheat_dir, sizeof cheat_dir, "%s", config);
            char *slash = strrchr(cheat_dir, '/');
            char *back = strrchr(cheat_dir, '\\');
            if (back > slash) slash = back;
            if (slash) *slash = 0;
            else snprintf(cheat_dir, sizeof cheat_dir, ".");
            strncat(cheat_dir, "/cheats", sizeof cheat_dir - strlen(cheat_dir) - 1);
        } else snprintf(cheat_dir, sizeof cheat_dir, "cheats");
    }
    if (!serial || !*serial || !strcmp(serial, "UNKNOWN")) {
        printf("Cheats: game serial unknown, not loading cheat files\n");
        return;
    }
    DIR *dir = opendir(cheat_dir);
    if (!dir) return; /* no cheats installed: quiet */
    struct dirent *de;
    int other_serials = 0;
    while ((de = readdir(dir)) && file_count < CHEAT_MAX_FILES) {
        if (!ends_with_json(de->d_name)) continue;
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", cheat_dir, de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char *data = malloc(CHEAT_MAX_FILE + 1);
        if (!data) { fclose(f); continue; }
        size_t len = fread(data, 1, CHEAT_MAX_FILE, f);
        fclose(f);
        data[len] = 0;
        CheatFile cf;
        memset(&cf, 0, sizeof cf);
        cf.path = strdup(path);
        JParser j = {data, data + len, 0};
        int ok = cf.path && parse_cheat(&j, &cf);
        free(data);
        if (!ok) {
            printf("Cheats: %s: not a valid cheat file (ignored)\n", de->d_name);
            free_file(&cf);
            continue;
        }
        if (cf.process[0] && strcmp(cf.process, "eboot.bin")) {
            printf("Cheats: %s: targets %s, not eboot.bin (skipped)\n", de->d_name, cf.process);
            free_file(&cf);
            continue;
        }
        if (strcmp(cf.id, serial)) {
            free_file(&cf);
            ++other_serials; /* a cheat file for another region */
            continue;
        }
        if (version[0] && cf.version[0] && strcmp(cf.version, version)) {
            printf("Cheats: %s: version %s, the game is %s (skipped)\n",
                   de->d_name, cf.version, version);
            free_file(&cf);
            continue;
        }
        files[file_count++] = cf;
    }
    closedir(dir);
    load_state();
    int enabled = 0, failed = 0;
    for (int i = 0; i < file_count; ++i) {
        if (files[i].master_wanted && files[i].master_count) {
            if (!write_entries(files[i].master, files[i].master_count, 1))
                files[i].master_applied = 1;
            else ++failed;
        }
        for (int m = 0; m < files[i].mod_count; ++m) {
            if (!files[i].mods[m].enabled) continue;
            if (write_entries(files[i].mods[m].writes, files[i].mods[m].write_count, 1)) {
                printf("Cheats: %s: %s is outside the image, not enabled\n",
                       files[i].path, files[i].mods[m].name);
                files[i].mods[m].enabled = 0;
                ++failed;
            } else ++enabled;
        }
    }
    if (file_count) {
        printf("Cheats: %d file%s for %s %s, %d mod%s enabled%s\n", file_count,
               file_count == 1 ? "" : "s", serial, version[0] ? version : "?", enabled,
               enabled == 1 ? "" : "s", failed ? ", with failures" : "");
        save_state(); /* rewrite: stale entries for skipped mods drop out */
    } else if (other_serials) {
        printf("Cheats: %d file%s in %s, none for %s\n", other_serials,
               other_serials == 1 ? "" : "s", cheat_dir, serial);
    }
}
