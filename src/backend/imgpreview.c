/* imgpreview.c -- see imgpreview.h. The SECOND preview producer: plain (non-megatexture)
 * materials, read out of the game's own `.index`/`.resources` containers and decoded on the CPU.
 *
 * megapreview.c covers the 5,033 materials that have a `.vmtr` atlas rect. The other ~4,772
 * render fine in game but are backed by ordinary image assets, so the atlas route cannot see
 * them at all. Chain (doom-re campaign `revenant-asset-index-and-viewport`, evidence 09):
 *
 *     name -> material record -> inflate decl -> `*map` field -> image record
 *          -> inflate .bimage -> first mip record -> BC1/BC3/BC7 -> RGBA
 *
 * Everything is read-only against files the game ships. No engine call at all -- unlike the
 * megatexture codec, BCn and DEFLATE are public formats. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgpreview.h"
#include "preview.h"
#include "bcn.h"
#include "backend_log.h"

#define MAX_PREVIEW 240u        /* matches megapreview's 2x2-page budget, so both routes agree */

/* ------------------------------------------------------------------ raw DEFLATE ---------------
 * The backend links no zlib, and the payloads are raw DEFLATE terminated by a Z_SYNC_FLUSH
 * marker rather than a BFINAL block (evidence 03 §5). We always know the uncompressed size from
 * the index record, so this stops on output-full and never needs to see the terminator. */

typedef struct { const unsigned char *src; size_t len, pos; unsigned bitbuf, bitcnt; } inf_t;

static unsigned inf_bits(inf_t *s, unsigned n)
{
    while (s->bitcnt < n) {
        unsigned b = (s->pos < s->len) ? s->src[s->pos++] : 0u;
        s->bitbuf |= b << s->bitcnt;
        s->bitcnt += 8;
    }
    unsigned v = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n; s->bitcnt -= n;
    return v;
}

typedef struct { unsigned short count[16], symbol[288]; } huff_t;

static void huff_build(huff_t *h, const unsigned char *lens, unsigned n)
{
    unsigned offs[16], i;
    for (i = 0; i < 16; ++i) h->count[i] = 0;
    for (i = 0; i < n; ++i) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[0] = 0;
    for (i = 1; i < 16; ++i) offs[i] = offs[i-1] + h->count[i-1];
    for (i = 0; i < n; ++i) if (lens[i]) h->symbol[offs[lens[i]]++] = (unsigned short)i;
}

static int huff_decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= (int)inf_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static const unsigned short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned short LEXT [29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned short DEXT [30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* Returns bytes produced (== dst_len on success). */
static size_t inflate_raw(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len)
{
    inf_t s = { src, src_len, 0, 0, 0 };
    size_t out = 0;
    huff_t lit, dist;
    for (;;) {
        if (out >= dst_len) break;
        unsigned final = inf_bits(&s, 1), type = inf_bits(&s, 2);
        if (type == 0) {                                  /* stored */
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.pos + 4 > s.len) break;
            unsigned len = s.src[s.pos] | (s.src[s.pos+1] << 8);
            s.pos += 4;
            if (s.pos + len > s.len) len = (unsigned)(s.len - s.pos);
            if (out + len > dst_len) len = (unsigned)(dst_len - out);
            memcpy(dst + out, s.src + s.pos, len);
            s.pos += len; out += len;
        } else if (type == 1 || type == 2) {
            if (type == 1) {                              /* fixed tables */
                unsigned char l[288], d[30];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                for (i = 0; i < 30; ++i) d[i] = 5;
                huff_build(&lit, l, 288); huff_build(&dist, d, 30);
            } else {                                      /* dynamic tables */
                static const unsigned char ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                unsigned nlen = inf_bits(&s,5)+257, ndist = inf_bits(&s,5)+1, ncode = inf_bits(&s,4)+4;
                unsigned char cl[19]; memset(cl, 0, sizeof cl);
                for (unsigned i = 0; i < ncode; ++i) cl[ord[i]] = (unsigned char)inf_bits(&s,3);
                huff_t clh; huff_build(&clh, cl, 19);
                unsigned char lens[320]; memset(lens, 0, sizeof lens);
                unsigned i = 0;
                while (i < nlen + ndist) {
                    int sym = huff_decode(&s, &clh);
                    if (sym < 0) return out;
                    if (sym < 16) lens[i++] = (unsigned char)sym;
                    else if (sym == 16) { unsigned char prev = i ? lens[i-1] : 0; unsigned r = 3 + inf_bits(&s,2); while (r-- && i < 320) lens[i++] = prev; }
                    else if (sym == 17) { unsigned r = 3 + inf_bits(&s,3); while (r-- && i < 320) lens[i++] = 0; }
                    else                { unsigned r = 11 + inf_bits(&s,7); while (r-- && i < 320) lens[i++] = 0; }
                }
                huff_build(&lit, lens, nlen); huff_build(&dist, lens + nlen, ndist);
            }
            for (;;) {
                int sym = huff_decode(&s, &lit);
                if (sym < 0) return out;
                if (sym < 256) { if (out < dst_len) dst[out++] = (unsigned char)sym; else return out; }
                else if (sym == 256) break;
                else {
                    sym -= 257; if (sym >= 29) return out;
                    unsigned len = LBASE[sym] + inf_bits(&s, LEXT[sym]);
                    int ds = huff_decode(&s, &dist);
                    if (ds < 0 || ds >= 30) return out;
                    unsigned d = DBASE[ds] + inf_bits(&s, DEXT[ds]);
                    if (d > out) return out;
                    while (len-- && out < dst_len) { dst[out] = dst[out - d]; out++; }
                }
            }
        } else return out;
        if (final) break;
        if (s.pos >= s.len && s.bitcnt == 0) break;
    }
    return out;
}

/* ------------------------------------------------------------------- containers ---------------*/

typedef struct { const char *name; unsigned long long roff; unsigned usz, csz;
                 unsigned char kind, box, hidden; } rec_t;
/* kind: one of SH_ASSET_*, below.  box: which .resources file.
 * hidden: excluded from the browser listing (a campaign duplicate of a SnapMap record, or a
 * campaign record of a type we do not offer). Still findable by name -- see find_rec. */

/* The decl types we index. `suffix`, when set, additionally requires the record NAME to end with
 * it -- that is how one browser category draws from more than one decl type, and how a decl type
 * contributes only part of itself.
 *
 * Counted directly out of snap_gameresources.index: model 12,630 / material 9,805 / sound 5,658 /
 * image 3,423 / entityDef 2,520 / decalatlas 1,673 / particle 1,523 / snapEditorEntityDef 1,362 /
 * md6Def 506 / fx 476. Types outside this table (renderProg, anim, cm, aas, ...) are engine
 * internals with nothing a mapper can place, so they are skipped entirely.
 *
 * MODELS is the one category that is not a straight type mapping. `renderModelInfo.model` takes
 * .lwo and .md6 values, and .md6 models live in their own decl type, so the category is the union
 * of the two. The `model` type's other 9,961 records are .bmodel, which is baked per-map world and
 * combo geometry (`maps/.../_combo/_world`, `dynamicsnapmap/...`) sharing no name with any .lwo --
 * not a prop anyone places, so it is filtered out rather than padding the list four-fold. */
static const struct { const char *type; unsigned len; unsigned char kind; const char *suffix; unsigned slen; } g_kinds[] = {
    { "material",            8, SH_ASSET_MATERIAL,   NULL,   0 },
    { "image",               5, SH_ASSET_IMAGE,      NULL,   0 },
    { "model",               5, SH_ASSET_MODEL,      ".lwo", 4 },
    { "md6Def",              6, SH_ASSET_MODEL,      NULL,   0 },
    { "sound",               5, SH_ASSET_SOUND,      NULL,   0 },
    { "fx",                  2, SH_ASSET_FX,         NULL,   0 },
    { "particle",            8, SH_ASSET_PARTICLE,   NULL,   0 },
    { "decalatlas",         10, SH_ASSET_DECALATLAS, NULL,   0 },
    { "snapEditorEntityDef",19, SH_ASSET_SNAPDEF,    NULL,   0 },
    { "entityDef",           9, SH_ASSET_ENTITYDEF,  NULL,   0 }
};
#define KIND_COUNT ((int)(sizeof g_kinds / sizeof g_kinds[0]))

typedef struct { unsigned char *idx; size_t idxLen; HANDLE res; } box_t;

/* ---- Wwise events: the OTHER half of the sound catalog -------------------------------------------
 * A `sound` decl is a thin wrapper naming a Wwise event, but the two sets are not the same and
 * NEITHER contains the other. Measured against the shipped files:
 *
 *     Wwise events (soundbanksinfo.xml) 7,649   |   sound decls (snap box) 5,658
 *     overlap 5,058   |   events with no decl 2,591   |   decls not in the manifest 600
 *     union, deduped                    8,249
 *
 * The 2,591 event-only names are what a mapper notices missing -- 594 of them are
 * `play_vo_snapmaps_*`, the generic male/female SnapMap VO, much of it DLC1-3. The 600 decl-only
 * names are mostly path-form (`ambient_events/...`, `effects/...`) plus specials like `_silence`.
 * So the browser lists the UNION and dedupes case-insensitively -- the manifest spells events
 * `Play_Vo_...` while decls are lowercase, so a naive merge would double 5,058 entries.
 *
 * `soundbanksinfo.xml` is the Wwise-generated manifest id shipped with the game (26 MB, next to the
 * .bnk files). Parsed for `<Event Id="..." Name="..."/>` only; everything else is ignored. */
static unsigned char *g_wwise;           /* the manifest text, kept alive: names point into it */
static const char   **g_ev;              /* event names NOT already present as a decl */
static int            g_evCount;

static box_t   g_box[2];                 /* 0 = snap_gameresources, 1 = gameresources */
static rec_t  *g_rec;
static int     g_recCount;
static int     g_loaded;
static char    g_baseDir[MAX_PATH];
static CRITICAL_SECTION g_lock;

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static unsigned long long be64(const unsigned char *p)
{ unsigned long long v=0; for (int i=0;i<8;++i) v=(v<<8)|p[i]; return v; }

static int imgpreview_load_box(int b, const char *stem)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s.index", g_baseDir, stem);
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!buf || !ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart) {
        CloseHandle(f); free(buf); return 0;
    }
    CloseHandle(f);
    g_box[b].idx = buf; g_box[b].idxLen = (size_t)sz.QuadPart;

    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s.resources", g_baseDir, stem);
    g_box[b].res = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (g_box[b].res == INVALID_HANDLE_VALUE) { g_box[b].res = NULL; return 0; }

    /* Header: magic "\x05SER", BE count at +0x20, records at +0x28. Each record is three
     * length-prefixed ASCII strings then a 25-byte fixed block (evidence 03 §2). */
    if (buf[0] != 0x05 || memcmp(buf+1, "SER", 3) != 0) return 0;
    unsigned n = be32(buf + 0x20);
    size_t o = 0x28;
    for (unsigned i = 0; i < n && o + 12 < g_box[b].idxLen; ++i) {
        unsigned tl = *(unsigned *)(buf + o); const char *type = (const char *)(buf + o + 4); o += 4 + tl;
        unsigned nl = *(unsigned *)(buf + o); const char *name = (const char *)(buf + o + 4); o += 4 + nl;
        unsigned pl = *(unsigned *)(buf + o);                                                   o += 4 + pl;
        if (o + 25 > g_box[b].idxLen) break;
        const unsigned char *blk = buf + o; o += 25;

        int kind = -1;
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (tl != g_kinds[k].len || memcmp(type, g_kinds[k].type, tl) != 0) continue;
            /* Names are not NUL-terminated here (that happens below), so match the suffix against
             * the known length rather than with strcmp. */
            if (g_kinds[k].suffix &&
                (nl < g_kinds[k].slen ||
                 _strnicmp(name + nl - g_kinds[k].slen, g_kinds[k].suffix, g_kinds[k].slen) != 0))
                break;                              /* right type, wrong extension -> skip it */
            kind = g_kinds[k].kind;
            break;
        }
        if (kind < 0) continue;

        if ((g_recCount & 1023) == 0) {
            rec_t *bigger = (rec_t *)realloc(g_rec, (size_t)(g_recCount + 1024) * sizeof *bigger);
            if (!bigger) break;
            g_rec = bigger;
        }
        rec_t *r = &g_rec[g_recCount++];
        r->name = name; r->roff = be64(blk); r->usz = be32(blk+8); r->csz = be32(blk+12);
        /* `hidden` MUST be set here: the record array grows by realloc, which does not zero, so
         * leaving it uninitialised would hide records at random. */
        r->kind = (unsigned char)kind; r->box = (unsigned char)b; r->hidden = 0;
        /* Names are NOT NUL-terminated in the file; terminate in place. The byte we overwrite
         * is the first of the next length prefix, which we have already consumed. */
        ((char *)name)[nl] = '\0';
    }
    return 1;
}

/* Sort/search helper: the two sources disagree on case, so every comparison here is case-folded. */
static int __cdecl cmp_ci(const void *a, const void *b)
{
    return _stricmp(*(const char * const *)a, *(const char * const *)b);
}

/* Parse the Wwise manifest into g_ev, keeping only events with no `sound` decl of the same name.
 * Failure is non-fatal: a missing or unreadable manifest just means the catalog stays as it was. */
static void imgpreview_load_wwise(void)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\sound\\soundbanks\\pc\\soundbanksinfo.xml", g_baseDir);
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { backend_log("B2: imgpreview -- no soundbanksinfo.xml; Wwise events unavailable"); return; }
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    /* +1 so a name ending at EOF still has a byte to take the NUL. */
    unsigned char *buf = (unsigned char *)malloc((size_t)sz.QuadPart + 1);
    DWORD got = 0;
    if (!buf || !ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart) {
        CloseHandle(f); free(buf); return;
    }
    CloseHandle(f);
    buf[got] = '\0';
    g_wwise = buf;

    /* The decl names, sorted case-insensitively, so the dedup is a binary search rather than a
     * 7,649 x 5,658 scan. */
    const char **decl = (const char **)malloc((size_t)g_recCount * sizeof *decl);
    int dn = 0;
    if (decl) {
        for (int i = 0; i < g_recCount; ++i)
            if (g_rec[i].kind == SH_ASSET_SOUND) decl[dn++] = g_rec[i].name;
        qsort(decl, (size_t)dn, sizeof *decl, cmp_ci);
    }

    int cap = 1024, dup = 0;
    g_ev = (const char **)malloc((size_t)cap * sizeof *g_ev);
    if (!g_ev) { free(decl); return; }

    for (char *s = (char *)buf; (s = strstr(s, "<Event ")) != NULL; ) {
        char *nm = strstr(s, "Name=\"");
        char *end = strchr(s, '>');
        if (!nm || (end && nm > end)) { s += 7; continue; }   /* no Name on this element */
        nm += 6;
        char *q = strchr(nm, '"');
        if (!q) break;
        *q = '\0';                                            /* terminate in place */
        s = q + 1;

        if (decl && bsearch(&nm, decl, (size_t)dn, sizeof *decl, cmp_ci)) { dup++; continue; }
        if (g_evCount == cap) {
            const char **bigger = (const char **)realloc(g_ev, (size_t)(cap * 2) * sizeof *bigger);
            if (!bigger) break;
            g_ev = bigger; cap *= 2;
        }
        g_ev[g_evCount++] = nm;
    }
    free(decl);

    /* The manifest lists each event once PER SOUNDBANK that includes it -- 39,971 <Event> elements
     * for 7,649 distinct names. Without this the browser would show roughly 9,400 duplicate rows.
     * Sort case-insensitively, then collapse adjacent equals. */
    int raw = g_evCount;
    if (g_evCount > 1) {
        qsort(g_ev, (size_t)g_evCount, sizeof *g_ev, cmp_ci);
        int w = 1;
        for (int i = 1; i < g_evCount; ++i)
            if (_stricmp(g_ev[i], g_ev[w - 1]) != 0) g_ev[w++] = g_ev[i];
        g_evCount = w;
    }

    char line[240];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- Wwise manifest: %d distinct event(s) with no decl added to the sound "
        "catalog (%d bank-repeats collapsed, %d already had a decl)",
        g_evCount, raw - g_evCount, dup);
    backend_log(line);
}

static int imgpreview_load(void)
{
    if (g_loaded) return g_loaded > 0;
    g_loaded = -1;
    char exe[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return 0;
    char *slash = strrchr(exe, '\\'); if (!slash) return 0;
    *slash = '\0';
    _snprintf_s(g_baseDir, sizeof g_baseDir, _TRUNCATE, "%s\\base", exe);

    int a = imgpreview_load_box(0, "snap_gameresources");
    int b = imgpreview_load_box(1, "gameresources");

    imgpreview_load_wwise();

    /* Decide, once, which records the browser will LIST.
     *
     * Box 0 (`snap_gameresources`) is everything SnapMap ships with. Box 1 (`gameresources`) is
     * the campaign's set; most of it is unreferencable from SnapMap, which is why it is not
     * offered wholesale. SOUNDS are the exception worth making: 1,186 sound decls exist only in
     * the campaign box and 231 of those are `vo_*`, which is a category a mapper visibly misses.
     * Whether the engine will actually SOUND one from a SnapMap session is campaign question Q11
     * and is not answerable from the files -- but the browser now has a working play/stop, so
     * offering them turns an unanswerable question into a one-click test.
     *
     * Duplicates are dropped rather than shown twice: 2,401 sound names appear in both boxes. The
     * box-0 record wins, so nothing that already worked changes route. */
    int dup = 0, extra = 0;
    for (int i = 0; i < g_recCount; ++i) {
        if (g_rec[i].box == 0) continue;
        if (g_rec[i].kind != SH_ASSET_SOUND) { g_rec[i].hidden = 1; continue; }
        for (int j = 0; j < g_recCount; ++j) {
            if (g_rec[j].box != 0 || g_rec[j].kind != SH_ASSET_SOUND) continue;
            if (_stricmp(g_rec[j].name, g_rec[i].name) == 0) { g_rec[i].hidden = 1; dup++; break; }
        }
        if (!g_rec[i].hidden) extra++;
    }

    char line[260];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- indexed %d records (snap=%s game=%s); campaign sounds offered: %d "
        "(%d duplicates of SnapMap sounds dropped)",
        g_recCount, a ? "ok" : "MISSING", b ? "ok" : "missing", extra, dup);
    backend_log(line);
    g_loaded = (g_recCount > 0) ? 1 : -1;
    return g_loaded > 0;
}

static const rec_t *find_rec(const char *name, int kind)
{
    for (int i = 0; i < g_recCount; ++i)
        if (g_rec[i].kind == kind && _stricmp(g_rec[i].name, name) == 0) return &g_rec[i];
    return NULL;
}

/* Is `name` a real decl of this type in the shipped containers? Exposed for callers that are about
 * to hand a name to an ENGINE lookup and need to know it exists first -- the engine's find-or-create
 * primitive fatals on a miss, so "does this name exist" has to be answered from our own data, never
 * by trying it. Takes the same lock as the list path; the catalog is parsed lazily on first use. */
int sh_imgpreview_has(int kind, const char *name)
{
    if (!name || !name[0]) return 0;
    EnterCriticalSection(&g_lock);
    int ok = 0;
    if (imgpreview_load()) {
        ok = find_rec(name, kind) != NULL;
        /* A Wwise event with no decl is still a real, playable name -- 2,591 of them, including
         * the generic SnapMap VO. The engine resolves it through find-or-CREATE, which builds the
         * decl on demand; that path is safe by default (its only fatal is gated on
         * `resource_errorInGame == 2`, and the cvar ships at 0 = "Nothing"). */
        if (!ok && kind == SH_ASSET_SOUND) {
            for (int e = 0; e < g_evCount && !ok; ++e)
                if (_stricmp(g_ev[e], name) == 0) ok = 1;
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

/* Images carry sampler-variant suffixes (`$nearest`, `$bc7`, `$borderclamp$alpha`), so an exact
 * miss falls back to the first record whose base name matches. */
static const rec_t *find_image(const char *name)
{
    const rec_t *r = find_rec(name, 1);
    if (r) return r;
    size_t n = strlen(name);
    for (int i = 0; i < g_recCount; ++i) {
        if (g_rec[i].kind != 1) continue;
        const char *nm = g_rec[i].name;
        if (_strnicmp(nm, name, n) == 0 && nm[n] == '$') return &g_rec[i];
    }
    return NULL;
}

/* Read a record's payload, inflating if needed. Caller frees. */
static unsigned char *read_payload(const rec_t *r, size_t *out_len)
{
    HANDLE h = g_box[r->box].res;
    if (!h || !r->usz) return NULL;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)r->roff;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return NULL;
    unsigned char *raw = (unsigned char *)malloc(r->csz);
    DWORD got = 0;
    if (!raw || !ReadFile(h, raw, r->csz, &got, NULL) || got != r->csz) { free(raw); return NULL; }
    if (r->csz == r->usz) { *out_len = r->usz; return raw; }
    unsigned char *out = (unsigned char *)malloc(r->usz);
    if (!out) { free(raw); return NULL; }
    size_t n = inflate_raw(raw, r->csz, out, r->usz);
    free(raw);
    *out_len = n;
    return out;
}

/* --------------------------------------------------------------------- decl parse -------------
 * Decls are plain text. The field naming its image varies by `stageprogram` -- `transmap` is by
 * far the most common, then `texturemap` -- so the rule is "any field whose name ends in `map`",
 * skipping engine built-ins (`_black`, `_vmtrpagetable`, ...) which all start with '_'.
 * Albedo-ish names are preferred so a normal or specular map never wins over the base colour. */
static const char *PREF[] = { "texturemap","transmap","transsortmap","transatlasmap",
                              "virtualtransmap","basecolormap","albedomap","diffusemap",
                              "colormap","sparediffusemap", NULL };

static int decl_find_image(const char *txt, size_t len, char *out, size_t cap)
{
    char best[256] = {0}; int bestRank = 9999;
    size_t i = 0;
    while (i < len) {
        while (i < len && (txt[i]=='\n' || txt[i]=='\r' || txt[i]==' ' || txt[i]=='\t')) ++i;
        size_t ks = i;
        while (i < len && (txt[i]>='a'&&txt[i]<='z')) ++i;
        size_t ke = i;
        if (ke > ks && ke - ks >= 4 && memcmp(txt + ke - 3, "map", 3) == 0) {
            while (i < len && (txt[i]==' ' || txt[i]=='\t')) ++i;
            size_t vs = i;
            if (i < len && txt[i]=='"') { ++vs; ++i; while (i < len && txt[i] != '"') ++i; }
            else while (i < len && txt[i] != '\n' && txt[i] != '\r' && txt[i] != ' ' && txt[i] != '\t') ++i;
            size_t vlen = i - vs;
            if (vlen && vlen < 250 && txt[vs] != '_' && txt[vs] != '{') {
                char key[64]; size_t klen = ke - ks; if (klen > 63) klen = 63;
                memcpy(key, txt + ks, klen); key[klen] = 0;
                int rank = 500;
                for (int p = 0; PREF[p]; ++p) if (strcmp(key, PREF[p]) == 0) { rank = p; break; }
                if (rank < bestRank) {
                    bestRank = rank;
                    size_t c = vlen < sizeof best - 1 ? vlen : sizeof best - 1;
                    memcpy(best, txt + vs, c); best[c] = 0;
                }
            }
        }
        while (i < len && txt[i] != '\n') ++i;
    }
    if (!best[0]) return 0;
    strncpy_s(out, cap, best, _TRUNCATE);
    return 1;
}

/* --------------------------------------------------------------------- box downscale ----------*/
static void downscale(const unsigned char *src, unsigned sw, unsigned sh, unsigned pitch,
                      unsigned char *dst, unsigned dw, unsigned dh)
{
    for (unsigned y = 0; y < dh; ++y) {
        unsigned y0 = y * sh / dh, y1 = (y + 1) * sh / dh; if (y1 <= y0) y1 = y0 + 1;
        for (unsigned x = 0; x < dw; ++x) {
            unsigned x0 = x * sw / dw, x1 = (x + 1) * sw / dw; if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0,0,0,0}, n = 0;
            for (unsigned yy = y0; yy < y1; ++yy)
                for (unsigned xx = x0; xx < x1; ++xx) {
                    const unsigned char *s = src + ((size_t)yy * pitch + xx) * 4;
                    acc[0]+=s[0]; acc[1]+=s[1]; acc[2]+=s[2]; acc[3]+=s[3]; ++n;
                }
            unsigned char *d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = (unsigned char)(acc[c] / (n ? n : 1));
        }
    }
}

int sh_imgpreview_produce(const char *name)
{
    EnterCriticalSection(&g_lock);
    int ok = 0;
    unsigned char *decl = NULL, *bim = NULL, *rgba = NULL, *thumb = NULL;
    __try {
        if (!imgpreview_load()) __leave;

        const rec_t *mr = find_rec(name, 0);
        if (!mr) { backend_log("B2: imgpreview -- no material record"); __leave; }

        size_t dlen = 0; decl = read_payload(mr, &dlen);
        if (!decl || !dlen) { backend_log("B2: imgpreview -- decl read failed"); __leave; }

        char img[256];
        if (!decl_find_image((const char *)decl, dlen, img, sizeof img)) {
            char l[320]; _snprintf_s(l,sizeof l,_TRUNCATE,
                "B2: imgpreview -- '%s' decl names no usable image (atlased decal/particle?)", name);
            backend_log(l); __leave;
        }
        const rec_t *ir = find_image(img);
        if (!ir) {
            char l[400]; _snprintf_s(l,sizeof l,_TRUNCATE,
                "B2: imgpreview -- '%s' -> image '%s' not in any container", name, img);
            backend_log(l); __leave;
        }

        size_t blen = 0; bim = read_payload(ir, &blen);
        if (!bim || blen < 0x40 || memcmp(bim + 4, "\x07MIB", 4) != 0) {
            backend_log("B2: imgpreview -- not a .bimage"); __leave;
        }
        unsigned fmt = *(unsigned *)(bim + 0x20);
        /* First mip record at +0x30: BE u32 level, BE u16 w @+4, BE u16 h @+8, BE u32 size @+0x0A. */
        unsigned w  = ((unsigned)bim[0x34] << 8) | bim[0x35];
        unsigned h  = ((unsigned)bim[0x38] << 8) | bim[0x39];
        unsigned sz = be32(bim + 0x3A);
        if (!w || !h || w > 8192 || h > 8192 || (size_t)0x3E + sz > blen) {
            backend_log("B2: imgpreview -- bad mip record"); __leave;
        }

        size_t need = bcn_rgba_size(w, h);
        rgba = (unsigned char *)malloc(need);
        if (!rgba) __leave;
        if (!bcn_decode(fmt, bim + 0x3E, sz, w, h, rgba)) {
            char l[220]; _snprintf_s(l,sizeof l,_TRUNCATE,
                "B2: imgpreview -- '%s' format code %u not decodable (only 10/11/23)", name, fmt);
            backend_log(l); __leave;
        }

        unsigned pw = BCN_PAD(w);
        unsigned dw = w, dh = h;
        if (dw > MAX_PREVIEW || dh > MAX_PREVIEW) {
            if (dw >= dh) { dh = dh * MAX_PREVIEW / dw; dw = MAX_PREVIEW; }
            else          { dw = dw * MAX_PREVIEW / dh; dh = MAX_PREVIEW; }
            if (!dw) dw = 1; if (!dh) dh = 1;
        }
        thumb = (unsigned char *)malloc((size_t)dw * dh * 4);
        if (!thumb) __leave;
        downscale(rgba, w, h, pw, thumb, dw, dh);

        sh_preview_publish(thumb, dw, dh);
        char l[360];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "B2: imgpreview -- '%s' -> image '%s' %ux%u fmt=%u -> %ux%u preview",
            name, img, w, h, fmt, dw, dh);
        backend_log(l);
        ok = 1;
    } __finally {
        free(decl); free(bim); free(rgba); free(thumb);
        LeaveCriticalSection(&g_lock);
    }
    return ok;
}

int sh_imgpreview_list(int kind, unsigned start, char *out, size_t cap)
{
    if (!out || cap < 2) return 0;
    out[0] = '\0';
    /* Bound by the ASSET id space, not the table length -- the table has more rows than there are
     * categories now that MODELS is fed by two decl types. */
    if (kind < 0 || kind >= SH_ASSET_COUNT) return 0;
    EnterCriticalSection(&g_lock);
    int written = 0;
    if (imgpreview_load()) {
        size_t used = 0;
        unsigned seen = 0;
        for (int i = 0; i < g_recCount; ++i) {
            /* `hidden` is decided once at load: everything in the campaign box except sounds, and
             * campaign sounds that duplicate a SnapMap one. See imgpreview_load. */
            if (g_rec[i].kind != kind || g_rec[i].hidden) continue;
            if (seen++ < start) continue;
            size_t n = strlen(g_rec[i].name);
            if (used + n + 2 > cap) break;                            /* leave room for \n and NUL */
            memcpy(out + used, g_rec[i].name, n); used += n;
            out[used++] = '\n';
            written++;
        }
        /* Wwise events continue the SAME `seen` sequence, so the caller's paging (add the returned
         * count to `start` until it returns 0) crosses the two sources without a special case. */
        if (kind == SH_ASSET_SOUND) {
            for (int e = 0; e < g_evCount; ++e) {
                if (seen++ < start) continue;
                size_t n = strlen(g_ev[e]);
                if (used + n + 2 > cap) break;
                memcpy(out + used, g_ev[e], n); used += n;
                out[used++] = '\n';
                written++;
            }
        }
        out[used] = '\0';
    }
    LeaveCriticalSection(&g_lock);
    return written;
}

int sh_imgpreview_install(void)
{
    InitializeCriticalSection(&g_lock);
    backend_log("B2: imgpreview -- installed (containers load lazily on first request)");
    return 1;
}
