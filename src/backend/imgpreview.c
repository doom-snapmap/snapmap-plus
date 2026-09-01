/* imgpreview.c -- see imgpreview.h. The SECOND preview producer: plain (non-megatexture)
 * materials and direct images, read out of the game's own `.index`/`.resources` containers and
 * decoded on the CPU.
 *
 * megapreview.c covers the 5,033 materials that have a `.vmtr` atlas rect. The other ~4,772
 * render fine in game but are backed by ordinary image assets, so the atlas route cannot see
 * them at all. The decode chain is:
 *
 *     name -> material record -> inflate decl -> `*map` field -> image record
 *          -> inflate .bimage -> first mip record -> BC1/BC3/BC7 -> RGBA
 *
 * A direct image request starts at the image-record step. Catalog metadata is retained as compact
 * strings and offsets; payload bytes stay in the installed game files until a preview is requested.
 *
 * Everything is read-only against files the game ships. No engine call at all -- unlike the
 * megatexture codec, BCn and DEFLATE are public formats. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>

#include "imgpreview.h"
#include "preview.h"
#include "bcn.h"
#include "megapreview.h"   /* the .vmtr atlas: the other half of the material catalog */
#include "backend_log.h"
#include "raw_deflate.h"

#define MAX_PREVIEW 240u        /* matches megapreview's 2x2-page budget, so both routes agree */

/* ------------------------------------------------------------------- containers ---------------*/

typedef struct { const char *name; unsigned long long roff; unsigned usz, csz;
                 unsigned char kind, box, hidden; } rec_t;
/* kind: one of SH_ASSET_*, below.  box: which .resources file.
 * hidden: excluded from the browser listing (a duplicate of a SnapMap record, or a base-game
 * record of a type we do not offer). Still findable by name -- see find_rec. */

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
 * of the two. The `model` type's other 9,961 snap-box records are .bmodel -- baked brush geometry,
 * sharing no name with any .lwo. Those are not padding for Models; they get their own two categories
 * (MODULE / BMODEL) so the props list stays a props list. */
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
    { "entityDef",           9, SH_ASSET_ENTITYDEF,  NULL,   0 },
    /* The `model` type's OTHER half: baked BRUSH geometry under maps/. Split from Models on purpose --
     * .bmodel and .lwo share ZERO stems, so these are disjoint content, not duplicates of the props.
     * The 232 palette modules are promoted out of this kind in imgpreview_load. */
    { "model",               5, SH_ASSET_BMODEL,     ".bmodel", 7 },
    { "cm",                  2, SH_ASSET_CLIPMODEL,  NULL,   0 },
    /* The THIRD source of Models, and the reason breakable props looked missing. A `breakable` decl
     * describes how something shatters and NAMES a model -- `breakable/barrel2` points at
     * `models/mapobjects/prop/destroyables/barrel2gib.lwo` -- and every one of those models is
     * indexed under `discreteAnimation`, not `model`. All 108 are .lwo, none of them duplicates a
     * name already in Models, and they are in the SNAP box, so unlike a base-game-only model they
     * actually load rather than rendering as a black cube. They take renderModelInfo.model like any
     * other model, so they belong in the same category rather than a separate one. */
    { "discreteAnimation",  17, SH_ASSET_MODEL,      NULL,   0 },
    { "perks",               5, SH_ASSET_PERK,       NULL,   0 },
    /* `file` is a mixed bag -- .bimage, .tome, .sbsp, .ambientsh -- and only the .bswf half is worth
     * offering, so the suffix does the filtering the type cannot. See imgpreview_swf_name for why
     * the listed name is not the name stored here. */
    { "file",                4, SH_ASSET_SWF,        ".bswf", 5 },
    /* Cooked MD6 geometry. An md6Def is the public logical model and names one of these rows through
     * its `mesh` field. Keep the row addressable by exact name, but never expose it as a browser kind. */
    { "baseModel",           9, SH_IMGPREVIEW_BASEMODEL_KIND, NULL, 0 }
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
 * That exact-name dedup is necessary but NOT sufficient, because the two sources name the same
 * sound two different ways. 5,160 decls are flat and literally named `play_*`, so they equal their
 * event outright and collapse here. The other 449 are PATH-FORM, where the rule is instead
 * `<twin> == "Play_" + the decl's LEAF` (`scripted_events/cyberdemon/head_splat_01` <->
 * `Play_head_splat_01`). Those slipped through as a second row for an already-listed sound.
 * imgpreview_hide_wrapped_sounds below is the pass that collapses them; note that the twin may be
 * either a Wwise event OR a flat `play_*` decl, and checking only the events fixes barely half of
 * them. Its comment carries the split and the evidence that dropping the decl side is lossless.
 *
 * `soundbanksinfo.xml` is the Wwise-generated manifest id shipped with the game (26 MB, next to the
 * .bnk files). Parsed for `<Event Id="..." Name="..."/>` only; everything else is ignored. */
static unsigned char *g_wwise;           /* compact strings; relevant tag stream only on pool OOM */
static size_t         g_wwiseBytes;
static size_t         g_wwiseSourceBytes;
static size_t         g_wwiseTagBytes;
static const char   **g_ev;              /* event names NOT already present as a decl */
static int            g_evCount;

/* ---- .vmtr-only materials: the OTHER half of the MATERIAL catalog --------------------------------
 * Exactly the same shape of problem as the Wwise/sound split above, for the same reason: a material
 * has TWO independent ways to be addressed, and neither set contains the other.
 *
 *   - by NAME, through a `material` decl -> the `customMaterial` field.
 *   - by RECTANGLE, through the `.vmtr` megatexture atlas -> the `virtualmapping` renderParm.
 *
 * The atlas does not need a decl. A row in `_vmtr.vmtr` is paintable as a virtualmapping value
 * whether or not anyone ever authored a `material` decl of that name, and thousands of shipped
 * rows have no decl at all. Listing only decls therefore hides them completely -- they cannot be
 * searched for, so they cannot be applied, even though the art is right there in the atlas.
 *
 * So MATERIALS list the UNION, deduped case-insensitively, exactly like sounds. The decl record
 * wins where both exist (it can do both carriers); an atlas-only name is offered as a material
 * that supports Virtual Mapping but not Custom Material, which is what `has_decl` reports to the
 * UI so it can gate the carrier honestly instead of guessing. */
static const char   **g_vt;              /* .vmtr names with NO material decl; point into megapreview */
static int            g_vtCount;

/* ---- which soundbank a sound came from -----------------------------------------------------------
 * `soundbanksinfo.xml` groups its events under <SoundBank><ShortName>doom_snapmaps</ShortName>...,
 * and that grouping is the only meaningful structure the sound catalog has. The names themselves are
 * almost entirely FLAT (5,160 of the decls are `play_*` with no path at all), so a folder tree built
 * from names is ~95% one giant root -- which is what it looks like in the browser today.
 *
 * 26 distinct banks, sensibly sized (doom_vo 1666, doom_initial 1551, doom_snapmaps 485,
 * doom_monsters 424, ... only 3 under 20 events), so they make a usable filter where a name-prefix
 * split does not: prefixes give 835 buckets, 585 of them holding a single event, and `vo` alone
 * swallowing 46% of the catalog.
 *
 * ONE EVENT CAN BE IN SEVERAL BANKS -- 1,619 of 7,649 are. That sounds fatal for a single-valued
 * filter and is not, because the overlap is almost entirely `doom_initial` (the always-loaded base
 * bank) paired with the bank that actually means something:
 *
 *     345  doom_initial + doom_monsters        121  doom_initial + doom_effects
 *     180  doom_initial + doom_scripted_events 114  doom_initial + doom_ui
 *     162  doom_initial + doom_weapon_sp       109  doom_initial + doom_ambience
 *
 * So the tie-break is "prefer the specific bank over doom_initial", which resolves nearly all of it.
 * doom_snapmaps is cleaner still: 485 events, only 27 of which appear in any other bank. */
typedef struct { const char *name; const char *bank; } sndbank_t;
static sndbank_t     *g_sb;              /* one row per DISTINCT event; both ptrs into g_wwise      */
static int            g_sbCount;
#define SB_BASE_BANK  "doom_initial"     /* the always-loaded bank a specific one should win over   */

static box_t   g_box[2];                 /* 0 = snap_gameresources, 1 = gameresources */
static rec_t  *g_rec;
static int     g_recCount;
static char   *g_namePool;               /* compact record names after raw indexes are parsed */
static size_t  g_namePoolBytes;
static int     g_loaded;
static int     g_soundLoaded;            /* optional Wwise union attempted */
static int     g_vmtrLoaded;             /* optional decl-less material union attempted */
static char    g_baseDir[MAX_PATH];
static CRITICAL_SECTION g_lock;

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static unsigned long long be64(const unsigned char *p)
{ unsigned long long v=0; for (int i=0;i<8;++i) v=(v<<8)|p[i]; return v; }
static unsigned le32(const unsigned char *p)
{ return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24); }

/* Turn the stored SWF record name into the one decls actually use.
 *
 *     generated/swf/interactables/elite_guard.bswf   <- what the index stores
 *     swf/interactables/elite_guard.swf              <- what an entityDef references
 *
 * The baked `.bswf` under `generated/` is the compiled artifact, the same relationship `.bimage`
 * has to an image decl. It appears in no decl anywhere, so listing it would give the mapper a name
 * that cannot be pasted into anything.
 *
 * Rewritten IN PLACE, which is safe only because the wanted form is strictly SHORTER: dropping
 * `generated/` frees ten bytes and `.bswf` -> `.swf` one more. The prefix is skipped by moving the
 * POINTER (no copying), and the extension is overwritten across its own five bytes. Anything not
 * shaped as expected is returned untouched rather than half-converted. */
static const char *imgpreview_swf_name(char *name, unsigned nl)
{
    if (nl >= 5 && _stricmp(name + nl - 5, ".bswf") == 0)
        memcpy(name + nl - 5, ".swf", 5);           /* copies the terminator too */
    return (_strnicmp(name, "generated/", 10) == 0) ? name + 10 : name;
}

static int imgpreview_take_field(unsigned char *buf, size_t len, size_t *offset,
                                 unsigned char **value, unsigned *value_len)
{
    if (!buf || !offset || !value || !value_len || *offset > len || len - *offset < 4u) return 0;
    unsigned n = le32(buf + *offset);
    *offset += 4u;
    if ((size_t)n > len - *offset) return 0;
    *value = buf + *offset;
    *value_len = n;
    *offset += n;
    return 1;
}

/* Parse one complete index buffer. A malformed record rejects the whole box and rolls back every
 * record appended from it, so callers never expose a partial catalog assembled from corrupt input. */
static int imgpreview_parse_box_index(int b, unsigned char *buf, size_t len)
{
    if (b < 0 || b >= (int)(sizeof g_box / sizeof g_box[0]) || !buf || len < 0x28u) return 0;
    if (buf[0] != 0x05 || memcmp(buf + 1, "SER", 3) != 0) return 0;

    unsigned n = be32(buf + 0x20);
    size_t o = 0x28;
    int first_record = g_recCount;
    for (unsigned i = 0; i < n; ++i) {
        unsigned char *type_bytes = NULL, *name_bytes = NULL, *path_bytes = NULL;
        unsigned tl = 0, nl = 0, pl = 0;
        if (!imgpreview_take_field(buf, len, &o, &type_bytes, &tl) ||
            !imgpreview_take_field(buf, len, &o, &name_bytes, &nl) ||
            !imgpreview_take_field(buf, len, &o, &path_bytes, &pl) ||
            o > len) {
            g_recCount = first_record;
            return 0;
        }
        /* Every non-final record carries a 25-byte block. Both shipped indexes omit the final
         * four inter-record bytes at EOF, leaving a complete 21-byte terminal block. Accept that
         * exact terminal shape; any other short block is truncation and rejects the whole box. */
        size_t block_len = 25u;
        if (i + 1u == n && len - o == 21u) block_len = 21u;
        if (len - o < block_len) {
            g_recCount = first_record;
            return 0;
        }
        (void)path_bytes; (void)pl;
        const char *type = (const char *)type_bytes;
        const char *name = (const char *)name_bytes;
        const unsigned char *blk = buf + o;
        o += block_len;

        int kind = -1;
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (tl != g_kinds[k].len || memcmp(type, g_kinds[k].type, tl) != 0) continue;
            /* Names are not NUL-terminated here (that happens below), so match the suffix against
             * the known length rather than with strcmp. */
            /* `continue`, NOT `break`: one decl type can map to several kinds discriminated only by
             * extension (`model` -> .lwo Models and .bmodel Brush models). Breaking here on the
             * first entry whose suffix misses would drop every record the later entry owns. */
            if (g_kinds[k].suffix &&
                (nl < g_kinds[k].slen ||
                 _strnicmp(name + nl - g_kinds[k].slen, g_kinds[k].suffix, g_kinds[k].slen) != 0))
                continue;
            kind = g_kinds[k].kind;
            break;
        }
        if (kind < 0) continue;

        /* The broader base-game box contributes only the three routes this browser can actually
         * use: material and image records can satisfy cross-box pixel previews, and sound records
         * are deliberately offered in the catalog. Its models, collision, FX, defs, particles,
         * decals, perks, and SWFs are neither listed nor previewed, so retaining 28,230 records
         * and their names for those types served no request. The SnapMap box remains complete. */
        if (b == 1 && kind != SH_ASSET_MATERIAL && kind != SH_ASSET_IMAGE &&
            kind != SH_ASSET_SOUND && kind != SH_IMGPREVIEW_BASEMODEL_KIND)
            continue;

        if ((g_recCount & 1023) == 0) {
            rec_t *bigger = (rec_t *)realloc(g_rec, (size_t)(g_recCount + 1024) * sizeof *bigger);
            if (!bigger) {
                g_recCount = first_record;
                return 0;
            }
            g_rec = bigger;
        }
        rec_t *r = &g_rec[g_recCount++];
        r->name = name; r->roff = be64(blk); r->usz = be32(blk + 8); r->csz = be32(blk + 12);
        r->kind = (unsigned char)kind; r->box = (unsigned char)b; r->hidden = 0;
        /* The path-length prefix immediately follows the name. It has already been decoded, so its
         * first byte can safely terminate the retained name in place. */
        ((char *)name)[nl] = '\0';
        if (kind == SH_ASSET_SWF) r->name = imgpreview_swf_name((char *)name, nl);
    }
    return 1;
}

static int imgpreview_load_box(int b, const char *stem)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s.index", g_baseDir, stem);
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 ||
        (unsigned long long)sz.QuadPart > 0xFFFFFFFFull ||
        (unsigned long long)sz.QuadPart > (size_t)-1) {
        CloseHandle(f);
        return 0;
    }
    size_t len = (size_t)sz.QuadPart;
    unsigned char *buf = (unsigned char *)malloc(len);
    DWORD got = 0;
    if (!buf || !ReadFile(f, buf, (DWORD)len, &got, NULL) || got != (DWORD)len) {
        CloseHandle(f); free(buf); return 0;
    }
    CloseHandle(f);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s.resources", g_baseDir, stem);
    HANDLE res = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (res == INVALID_HANDLE_VALUE) { free(buf); return 0; }

    /* Header: magic "\x05SER", BE count at +0x20, records at +0x28. Each record is three
     * length-prefixed ASCII strings then a 25-byte fixed block. */
    if (!imgpreview_parse_box_index(b, buf, len)) {
        CloseHandle(res);
        free(buf);
        return 0;
    }
    g_box[b].idx = buf;
    g_box[b].idxLen = len;
    g_box[b].res = res;
    return 1;
}

static const rec_t *g_sortRec;

/* Sort record indices by case-insensitive name, then original position. The position tie-break
 * makes the earliest spelling the canonical string when the two boxes differ only in case. */
static int __cdecl cmp_rec_name(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = _stricmp(g_sortRec[ia].name, g_sortRec[ib].name);
    return c ? c : ia - ib;
}

/* The parser initially points record names into the raw index buffers. Those buffers also contain
 * every skipped type, payload path, and fixed record field, so retaining both complete files just
 * to keep the recognized names alive wastes tens of megabytes. Copy one instance of each distinct
 * recognized name into a compact pool, point duplicate records at that same immutable string, then
 * release both raw indexes. Resource payloads remain on disk and are still addressed by each
 * record's offset and sizes. Returns the number of raw bytes released. */
static size_t imgpreview_compact_names(void)
{
    if (g_namePool) return 0;

    int *ord = g_recCount ? (int *)malloc((size_t)g_recCount * sizeof *ord) : NULL;
    if (ord) {
        for (int i = 0; i < g_recCount; ++i) ord[i] = i;
        g_sortRec = g_rec;
        qsort(ord, (size_t)g_recCount, sizeof *ord, cmp_rec_name);
    }

    size_t need = 0;
    for (int i = 0; i < g_recCount; ++i) {
        int at = ord ? ord[i] : i;
        if (ord && i > 0 && _stricmp(g_rec[at].name, g_rec[ord[i - 1]].name) == 0) continue;
        size_t n = strlen(g_rec[at].name) + 1u;
        if (n > (size_t)-1 - need) { free(ord); return 0; }
        need += n;
    }

    char *pool = need ? (char *)malloc(need) : NULL;
    if (need && !pool) { free(ord); return 0; }

    char *dst = pool;
    const char *shared = NULL;
    for (int i = 0; i < g_recCount; ++i) {
        int at = ord ? ord[i] : i;
        int same = ord && i > 0 && _stricmp(g_rec[at].name, g_rec[ord[i - 1]].name) == 0;
        if (!same) {
            size_t n = strlen(g_rec[at].name) + 1u;
            memcpy(dst, g_rec[at].name, n);
            shared = dst;
            dst += n;
        }
        g_rec[at].name = shared;
    }
    free(ord);

    size_t released = 0;
    for (int b = 0; b < (int)(sizeof g_box / sizeof g_box[0]); ++b) {
        if (!g_box[b].idx) continue;
        released += g_box[b].idxLen;
        free(g_box[b].idx);
        g_box[b].idx = NULL;
        g_box[b].idxLen = 0;
    }
    g_namePool = pool;
    g_namePoolBytes = need;
    return released;
}

/* Sort/search helper: the two sources disagree on case, so every comparison here is case-folded. */
static int __cdecl cmp_ci(const void *a, const void *b)
{
    return _stricmp(*(const char * const *)a, *(const char * const *)b);
}

/* Sorts RECORD INDICES by (kind, box, name, original index) so repeats of one name inside one box
 * land next to each other. The index is the last key on purpose: it makes the order total, so the
 * pass that walks the result can rely on the earlier-indexed record always coming first and keep
 * that one. Reads through g_sortRec because qsort gives the comparator no context of its own. */
static int __cdecl cmp_rec_kind_name(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    const rec_t *x = &g_sortRec[ia], *y = &g_sortRec[ib];
    if (x->kind != y->kind) return (int)x->kind - (int)y->kind;
    if (x->box  != y->box)  return (int)x->box  - (int)y->box;
    int c = _stricmp(x->name, y->name);
    if (c) return c;
    return ia - ib;
}

/* Same, over the (event, bank) pair table -- groups an event's per-bank repeats together. */
static int __cdecl cmp_sb_name(const void *a, const void *b)
{
    return _stricmp(((const sndbank_t *)a)->name, ((const sndbank_t *)b)->name);
}

static int __cdecl cmp_sb_key(const void *a, const void *b)
{
    return _stricmp(*(const char * const *)a, ((const sndbank_t *)b)->name);
}

static int imgpreview_add_string_bytes(size_t *total, const char *value)
{
    size_t n = strlen(value) + 1u;
    if (n > (size_t)-1 - *total) return 0;
    *total += n;
    return 1;
}

static char *imgpreview_copy_string(char **next, const char *value)
{
    size_t n = strlen(value) + 1u;
    char *copy = *next;
    memcpy(copy, value, n);
    *next += n;
    return copy;
}

static void *imgpreview_shrink_array(void *items, size_t count, size_t itemBytes)
{
    if (!items) return NULL;
    if (!count) { free(items); return NULL; }
    if (itemBytes > (size_t)-1 / count) return items;
    void *exact = realloc(items, count * itemBytes);
    return exact ? exact : items;
}

static void imgpreview_shrink_wwise_tables(void)
{
    g_ev = (const char **)imgpreview_shrink_array(g_ev, (size_t)g_evCount, sizeof *g_ev);
    g_sb = (sndbank_t *)imgpreview_shrink_array(g_sb, (size_t)g_sbCount, sizeof *g_sb);
}

/* Parsing needs a mutable tag stream, but steady-state catalog use needs only each distinct event
 * name and each distinct bank name once. g_sb already has one row per event and g_ev is a subset of
 * those same events, so both tables can point into one interned pool instead of duplicating event
 * strings or repeating one bank name hundreds of times. */
static size_t imgpreview_compact_wwise_strings(void)
{
    const char **banks = g_sbCount ? (const char **)malloc((size_t)g_sbCount * sizeof *banks) : NULL;
    int *sbBank = g_sbCount ? (int *)malloc((size_t)g_sbCount * sizeof *sbBank) : NULL;
    int *evRow = g_evCount ? (int *)malloc((size_t)g_evCount * sizeof *evRow) : NULL;
    if ((g_sbCount && (!banks || !sbBank)) || (g_evCount && !evRow)) {
        free(banks); free(sbBank); free(evRow);
        return g_wwiseBytes;
    }

    for (int i = 0; i < g_sbCount; ++i) banks[i] = g_sb[i].bank;
    qsort(banks, (size_t)g_sbCount, sizeof *banks, cmp_ci);
    int bankCount = 0;
    for (int i = 0; i < g_sbCount; ++i)
        if (i == 0 || _stricmp(banks[i], banks[i - 1]) != 0) banks[bankCount++] = banks[i];

    for (int i = 0; i < g_sbCount; ++i) {
        const char *key = g_sb[i].bank;
        const char **found = (const char **)bsearch(&key, banks, (size_t)bankCount,
                                                    sizeof *banks, cmp_ci);
        if (!found) { free(banks); free(sbBank); free(evRow); return g_wwiseBytes; }
        sbBank[i] = (int)(found - banks);
    }
    for (int i = 0; i < g_evCount; ++i) {
        const char *key = g_ev[i];
        sndbank_t *found = (sndbank_t *)bsearch(&key, g_sb, (size_t)g_sbCount,
                                                sizeof *g_sb, cmp_sb_key);
        if (!found) { free(banks); free(sbBank); free(evRow); return g_wwiseBytes; }
        evRow[i] = (int)(found - g_sb);
    }

    size_t need = 0;
    for (int i = 0; i < g_sbCount; ++i)
        if (!imgpreview_add_string_bytes(&need, g_sb[i].name)) {
            free(banks); free(sbBank); free(evRow); return g_wwiseBytes;
        }
    for (int i = 0; i < bankCount; ++i)
        if (!imgpreview_add_string_bytes(&need, banks[i])) {
            free(banks); free(sbBank); free(evRow); return g_wwiseBytes;
        }

    unsigned char *pool = need ? (unsigned char *)malloc(need) : NULL;
    if (need && !pool) {
        free(banks); free(sbBank); free(evRow); return g_wwiseBytes;
    }

    char *next = (char *)pool;
    for (int i = 0; i < g_sbCount; ++i)
        g_sb[i].name = imgpreview_copy_string(&next, g_sb[i].name);
    for (int i = 0; i < bankCount; ++i)
        banks[i] = imgpreview_copy_string(&next, banks[i]);
    for (int i = 0; i < g_sbCount; ++i) g_sb[i].bank = banks[sbBank[i]];
    for (int i = 0; i < g_evCount; ++i) g_ev[i] = g_sb[evRow[i]].name;

    free(g_wwise);
    g_wwise = pool;
    g_wwiseBytes = need;
    free(banks); free(sbBank); free(evRow);
    return need;
}

/* Parse a mutable, NUL-padded stream of only the relevant Wwise tags into g_ev, keeping only events
 * with no `sound` decl of the same name. Takes ownership of `buf`; normal completion compacts every
 * retained pointer into a small owned string pool before releasing the transient tag stream.
 * `sourceBytes` is the full manifest size for the production measurement; tests pass their buffer
 * size directly. */
static void imgpreview_parse_wwise_buffer_sized(unsigned char *buf, size_t len, size_t sourceBytes)
{
    if (!buf) return;
    buf[len] = '\0';
    g_wwise = buf;
    g_wwiseBytes = len + 1u;
    g_wwiseSourceBytes = sourceBytes;
    g_wwiseTagBytes = len + 1u;

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
    if (!g_ev) {
        free(decl);
        free(g_wwise);
        g_wwise = NULL;
        g_wwiseBytes = 0;
        return;
    }

    /* Every (event, bank) pair, before dedup. The manifest repeats an event once per bank that
     * includes it -- 39,971 elements for 7,649 names -- so this is the raw pair list that the
     * prefer-specific-bank collapse below reduces to one row per event. */
    int sbCap = 4096;
    sndbank_t *sbRaw = (sndbank_t *)malloc((size_t)sbCap * sizeof *sbRaw);
    int sbRawCount = 0;

    /* The bank whose <IncludedEvents> we are currently inside. The manifest is ordered, so tracking
     * the most recent <SoundBank>'s <ShortName> is enough. Deliberately NOT any <ShortName>: the
     * <StreamedFiles> block earlier in the file uses that tag too, for .wav paths. Those precede
     * the first <SoundBank> and contain no <Event>, so anchoring to <SoundBank> skips them. */
    const char *bank = "";

    for (char *s = (char *)buf; ; ) {
        char *nb = strstr(s, "<SoundBank ");
        char *ne = strstr(s, "<Event ");
        if (!ne) break;                                       /* no events left */
        if (nb && nb < ne) {                                  /* a new bank starts first */
            char *sn = strstr(nb, "<ShortName>");
            char *se = sn ? strstr(sn + 11, "</ShortName>") : NULL;
            if (sn && se && sn < ne) { *se = '\0'; bank = sn + 11; s = se + 1; }
            else s = nb + 11;
            continue;
        }
        s = ne;
        char *nm = strstr(s, "Name=\"");
        char *end = strchr(s, '>');
        if (!nm || (end && nm > end)) { s += 7; continue; }   /* no Name on this element */
        nm += 6;
        char *q = strchr(nm, '"');
        if (!q) break;
        *q = '\0';                                            /* terminate in place */
        s = q + 1;

        if (sbRaw) {                                          /* record the pair before any dedup */
            if (sbRawCount == sbCap) {
                sndbank_t *bigger = (sndbank_t *)realloc(sbRaw, (size_t)(sbCap * 2) * sizeof *bigger);
                if (bigger) { sbRaw = bigger; sbCap *= 2; }
            }
            if (sbRawCount < sbCap) { sbRaw[sbRawCount].name = nm; sbRaw[sbRawCount].bank = bank; sbRawCount++; }
        }

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

    /* Collapse the raw (event, bank) pairs to one row per event, preferring a specific bank over
     * the always-loaded base one. Sorting by name groups the repeats; within a group the first
     * non-base bank wins, and a group that is ONLY the base bank keeps it. */
    if (sbRaw && sbRawCount > 0) {
        qsort(sbRaw, (size_t)sbRawCount, sizeof *sbRaw, cmp_sb_name);
        g_sb = sbRaw;
        int w = 0;
        for (int i = 0; i < sbRawCount; ) {
            int j = i;
            const char *best = sbRaw[i].bank;
            while (j < sbRawCount && _stricmp(sbRaw[j].name, sbRaw[i].name) == 0) {
                if (_stricmp(best, SB_BASE_BANK) == 0 && _stricmp(sbRaw[j].bank, SB_BASE_BANK) != 0)
                    best = sbRaw[j].bank;
                j++;
            }
            g_sb[w].name = sbRaw[i].name;
            g_sb[w].bank = best;
            w++;
            i = j;
        }
        g_sbCount = w;
    } else {
        free(sbRaw);
    }

    size_t retainedBytes = imgpreview_compact_wwise_strings();
    imgpreview_shrink_wwise_tables();
    char line[500];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- Wwise manifest: %d distinct event(s) with no decl added to the sound "
        "catalog (%d bank-repeats collapsed, %d already had a decl); %d event(s) mapped to a bank; "
        "%llu source bytes streamed through %llu relevant tag bytes to %llu retained string bytes",
        g_evCount, raw - g_evCount, dup, g_sbCount,
        (unsigned long long)sourceBytes, (unsigned long long)(len + 1u),
        (unsigned long long)retainedBytes);
    backend_log(line);
}

static void imgpreview_parse_wwise_buffer(unsigned char *buf, size_t len)
{
    imgpreview_parse_wwise_buffer_sized(buf, len, len + 1u);
}

#define WWISE_RELEVANT_MAX (16u * 1024u * 1024u)

static int imgpreview_append_wwise(unsigned char **buf, size_t *len, size_t *cap,
                                   const char *value, size_t valueLen)
{
    if (!buf || !len || !cap || !value || valueLen > WWISE_RELEVANT_MAX - *len) return 0;
    size_t need = *len + valueLen + 1u;
    if (need > *cap) {
        size_t grown = *cap ? *cap : 65536u;
        while (grown < need) {
            if (grown > WWISE_RELEVANT_MAX / 2u) { grown = WWISE_RELEVANT_MAX; break; }
            grown *= 2u;
        }
        if (grown < need) return 0;
        unsigned char *bigger = (unsigned char *)realloc(*buf, grown);
        if (!bigger) return 0;
        *buf = bigger;
        *cap = grown;
    }
    memcpy(*buf + *len, value, valueLen);
    *len += valueLen;
    (*buf)[*len] = '\0';
    return 1;
}

static int imgpreview_append_wwise_tag(unsigned char **buf, size_t *len, size_t *cap,
                                       const char *prefix, const char *value, size_t valueLen,
                                       const char *suffix)
{
    return imgpreview_append_wwise(buf, len, cap, prefix, strlen(prefix)) &&
           imgpreview_append_wwise(buf, len, cap, value, valueLen) &&
           imgpreview_append_wwise(buf, len, cap, suffix, strlen(suffix));
}

/* Stream the Wwise manifest line by line and retain only bank names and Event names in a compact
 * transient tag stream. The installed document is about 26 MiB, while these relevant tags are only
 * a small fraction of it; duration, attenuation, streamed-file, media, and hash metadata never
 * enters the process. Failure is non-fatal and leaves the decl-only sound catalog available. */
static void imgpreview_load_wwise_file(void)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\sound\\soundbanks\\pc\\soundbanksinfo.xml", g_baseDir);
    FILE *f = _fsopen(p, "rb", _SH_DENYNO);
    if (!f) {
        backend_log("B2: imgpreview -- no soundbanksinfo.xml; Wwise events unavailable");
        return;
    }
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long long fileEnd = _ftelli64(f);
    if (fileEnd < 0 || (unsigned long long)fileEnd > (size_t)-1) {
        fclose(f);
        backend_log("B2: imgpreview -- soundbanksinfo.xml is too large to read safely");
        return;
    }
    size_t sourceBytes = (size_t)fileEnd;
    if (_fseeki64(f, 0, SEEK_SET) != 0) { fclose(f); return; }

    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[4096], bank[256] = {0};
    int inBank = 0, ok = 1;
    while (ok && fgets(line, sizeof line, f)) {
        if (strstr(line, "<SoundBank ")) { inBank = 1; bank[0] = '\0'; }
        if (inBank && !bank[0]) {
            char *start = strstr(line, "<ShortName>");
            char *end = start ? strstr(start + 11, "</ShortName>") : NULL;
            if (start && end && (size_t)(end - (start + 11)) < sizeof bank) {
                size_t n = (size_t)(end - (start + 11));
                memcpy(bank, start + 11, n); bank[n] = '\0';
                ok = imgpreview_append_wwise_tag(&buf, &len, &cap,
                    "<SoundBank ><ShortName>", bank, n, "</ShortName>");
            }
        }
        if (inBank && bank[0]) {
            char *event = strstr(line, "<Event ");
            char *name = event ? strstr(event, "Name=\"") : NULL;
            char *end = name ? strchr(name + 6, '\"') : NULL;
            if (name && end)
                ok = imgpreview_append_wwise_tag(&buf, &len, &cap,
                    "<Event Name=\"", name + 6, (size_t)(end - (name + 6)), "\">");
        }
        if (strstr(line, "</SoundBank>")) { inBank = 0; bank[0] = '\0'; }
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok || !buf) {
        free(buf);
        backend_log("B2: imgpreview -- Wwise relevant-tag stream could not be built");
        return;
    }
    imgpreview_parse_wwise_buffer_sized(buf, len, sourceBytes);
}

/* Hide a sound decl that is nothing but a wrapper around a Wwise event already in the catalog.
 *
 * The manifest's exact-name dedup catches the 5,160 decls literally NAMED `play_*`,
 * because those equal their event's name outright. It cannot catch the other 449, which are
 * PATH-FORM -- and those were showing up as a second row for a sound already listed:
 *
 *     decl   scripted_events/cyberdemon/head_splat_01      <- this row, redundant
 *     event  Play_head_splat_01                            <- same sound
 *
 * The naming rule is `event == "Play_" + the decl's LEAF`, and it is safe to apply mechanically:
 * NONE of the 449 path-form decls already has a `play_` leaf, so the prefix can never double up.
 *
 * Dropping the decl side loses NOTHING. Measured across the shipped set, 5,657 of 5,658 sound decls
 * are empty wrappers -- `inherit = "default"` and an empty `edit` block. The single exception is
 * `default.decl` itself, the base they all inherit. A sound decl carries no volume, no falloff, no
 * randomisation the event does not already have, so the two rows are the same sound and the event is
 * the one that actually plays. What is lost is only the folder path, which the owner explicitly did
 * not want to keep for sounds ("I don't think we need the folder structure").
 *
 * Where two decls map to one event -- 8 pairs, e.g. monster/baron/attacks/groundpound and
 * monster/hellknight/attacks/groundpound both -> Play_groundpound -- collapsing them is correct
 * rather than lossy, for the same reason: neither decl adds anything to distinguish them.
 *
 * The twin is looked for in BOTH sources, because it can be either one and checking only the events
 * fixes only half the rows. Of the 449 path-form decls:
 *
 *     181  twin is a Wwise EVENT in g_ev          (no flat decl of that name exists)
 *     129  twin is a flat `play_<leaf>` DECL      (so the event was exact-matched OUT of g_ev)
 *     139  no twin at all -- genuinely unique, and correctly kept
 *
 * That second bucket is the trap: `g_ev` deliberately holds only events with NO exact-name decl, so
 * for `effects/explosions/rocket_explosion_default` the event `Play_rocket_explosion_default` is
 * absent from g_ev -- it was claimed by the flat decl `play_rocket_explosion_default`, which is
 * itself a listed row. Searching g_ev alone leaves that pair on screen.
 *
 * Only PATH-FORM records are ever hidden, and the name looked up (`play_` + leaf) never contains a
 * '/', so a flat row can never be hidden by this pass and two records can never hide each other.
 * The decl array is built from the records still VISIBLE at this point, so a box-1 twin that
 * the box dedup already hid cannot suppress the row that survived it.
 *
 * Deliberately does NOT touch flat-named records (no '/'), which the exact-name pass already
 * settled. Returns how many rows it hid. */
static int imgpreview_hide_wrapped_sounds(void)
{
    /* The sound rows the browser would list right now, sorted for a binary search. */
    const char **snd = (const char **)malloc((size_t)g_recCount * sizeof *snd);
    int sn = 0;
    if (snd) {
        for (int i = 0; i < g_recCount; ++i)
            if (g_rec[i].kind == SH_ASSET_SOUND && !g_rec[i].hidden) snd[sn++] = g_rec[i].name;
        qsort(snd, (size_t)sn, sizeof *snd, cmp_ci);
    }

    int hid = 0;
    for (int i = 0; i < g_recCount; ++i) {
        if (g_rec[i].kind != SH_ASSET_SOUND || g_rec[i].hidden) continue;
        const char *leaf = strrchr(g_rec[i].name, '/');
        if (!leaf) continue;                    /* flat name -- already exact-deduped */
        char want[320];
        _snprintf_s(want, sizeof want, _TRUNCATE, "Play_%s", leaf + 1);
        const char *key = want;                 /* both arrays are sorted case-insensitively */
        int twin = 0;
        if (g_ev && g_evCount > 0 && bsearch(&key, g_ev, (size_t)g_evCount, sizeof *g_ev, cmp_ci))
            twin = 1;                           /* twin is a Wwise event */
        else if (snd && sn > 0 && bsearch(&key, snd, (size_t)sn, sizeof *snd, cmp_ci))
            twin = 1;                           /* twin is a flat `play_*` decl */
        if (twin) { g_rec[i].hidden = 1; hid++; }
    }
    free(snd);
    return hid;
}

/* Sound-only augmentation is deliberately separate from the base resource index. Opening Images,
 * Models, or any other unrelated category must not read or retain Wwise metadata. */
static void imgpreview_load_sound_catalog(void)
{
    if (g_soundLoaded) return;
    g_soundLoaded = 1;
    ULONGLONG started = GetTickCount64();
    imgpreview_load_wwise_file();

    int wrapped = imgpreview_hide_wrapped_sounds();
    char line[220];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- sound catalog ready in %llu ms; "
        "%d wrapper sound decl(s) hidden behind a Play_ event",
        (unsigned long long)(GetTickCount64() - started), wrapped);
    backend_log(line);
}

/* Fold the `.vmtr` atlas rows that have NO material decl into the material catalog. Same shape as
 * the sound union: sort the decl names once, then binary-search each atlas row against them.
 * Names point into megapreview's parsed table, which lives for the process, so nothing is copied.
 * Failure is non-fatal -- a missing atlas just leaves the catalog decl-only, as it was before. */
static void imgpreview_load_vmtr(void)
{
    if (g_vmtrLoaded) return;
    g_vmtrLoaded = 1;
    ULONGLONG started = GetTickCount64();

    const char **decl = (const char **)malloc((size_t)g_recCount * sizeof *decl);
    int dn = 0;
    if (decl) {
        for (int i = 0; i < g_recCount; ++i)
            if (g_rec[i].kind == SH_ASSET_MATERIAL && !g_rec[i].hidden) decl[dn++] = g_rec[i].name;
        qsort(decl, (size_t)dn, sizeof *decl, cmp_ci);
    }

    int cap = 1024, dup = 0;
    g_vt = (const char **)malloc((size_t)cap * sizeof *g_vt);
    if (!g_vt) { free(decl); return; }

    for (int i = 0; ; ++i) {
        const char *nm = sh_megapreview_name_at(i);
        if (!nm) break;
        if (decl && bsearch(&nm, decl, (size_t)dn, sizeof *decl, cmp_ci)) { dup++; continue; }
        if (g_vtCount == cap) {
            const char **bigger = (const char **)realloc(g_vt, (size_t)(cap * 2) * sizeof *bigger);
            if (!bigger) break;
            g_vt = bigger; cap *= 2;
        }
        g_vt[g_vtCount++] = nm;
    }
    free(decl);

    /* The atlas lists a name once per shard it appears in, so collapse adjacent equals after
     * sorting -- same reason the Wwise manifest needs it. */
    int raw = g_vtCount;
    if (g_vtCount > 1) {
        qsort(g_vt, (size_t)g_vtCount, sizeof *g_vt, cmp_ci);
        int w = 1;
        for (int i = 1; i < g_vtCount; ++i)
            if (_stricmp(g_vt[i], g_vt[w - 1]) != 0) g_vt[w++] = g_vt[i];
        g_vtCount = w;
    }
    g_vt = (const char **)imgpreview_shrink_array(g_vt, (size_t)g_vtCount, sizeof *g_vt);

    char line[280];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- .vmtr atlas: %d distinct decl-less material(s) added to the material "
        "catalog (%d shard-repeats collapsed, %d already had a decl) in %llu ms",
        g_vtCount, raw - g_vtCount, dup,
        (unsigned long long)(GetTickCount64() - started));
    backend_log(line);
}

static int imgpreview_load(void)
{
    if (g_loaded) return g_loaded > 0;
    g_loaded = -1;
    ULONGLONG started = GetTickCount64();
    char exe[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return 0;
    char *slash = strrchr(exe, '\\'); if (!slash) return 0;
    *slash = '\0';
    _snprintf_s(g_baseDir, sizeof g_baseDir, _TRUNCATE, "%s\\base", exe);

    int a = imgpreview_load_box(0, "snap_gameresources");
    int b = imgpreview_load_box(1, "gameresources");
    size_t indexBytes = g_box[0].idxLen + g_box[1].idxLen;
    size_t releasedIndexBytes = imgpreview_compact_names();
    size_t retainedIndexBytes =
        (releasedIndexBytes == indexBytes) ? g_namePoolBytes : indexBytes;
    const char *indexMode = (releasedIndexBytes == indexBytes) ? "compacted" : "raw retained";

    /* Promote the 232 SnapMap MODULES out of the brush-model pile. They are the only .bmodel
     * records that pair 1:1 with a collision model, so they are the only ones that can be placed
     * as something both visible AND solid -- a different proposition from the wall/floor pieces
     * they are assembled from, and worth its own category rather than being lost among 9,729
     * fragments and invisible internals. */
    int modules = 0;
    for (int i = 0; i < g_recCount; ++i)
        if (g_rec[i].kind == SH_ASSET_BMODEL && strstr(g_rec[i].name, "/palettes/mega_blessed/")) {
            g_rec[i].kind = SH_ASSET_MODULE;
            modules++;
        }

    /* Lights is material DECLS only, and that is not an arbitrary choice -- it is what makes the
     * list agree with a known-good one. Including `lightatlas` rows too gave 117 names; filtering to
     * decls gives 89, which is exactly the 88 the whitelist is known to accept plus
     * `lights/defaultprojectedlight`, sibling of defaultpointlight and defaultparallellight, both
     * already known good.
     *
     * The 28 dropped rows are atlas entries with NO material decl -- the light IMAGES, several with
     * a `.tga` on the end and five not even under lights/ (textures/common/white.tga and friends).
     * `lightMaterial` names a material, so an image with no decl has nothing to resolve, the same
     * reason a decl-less atlas row cannot take customMaterial.
     *
     * COPY, do not move. Promoting these out of Materials the way palette modules are promoted was
     * wrong: a move is only right when the source list should not contain the rows at all, and a
     * `lights/` material is still a material. */
    int lights = 0;
    for (int i = 0, n0 = g_recCount; i < n0; ++i) {
        if (g_rec[i].kind != SH_ASSET_MATERIAL) continue;
        /* BOTH prefixes. `lights_blended/` is a real second family -- 11 of them, and 11 of the 88
         * names on the known-good list live there -- and matching only `lights/` silently dropped
         * every one. It is not a subfolder of `lights/`; the underscore makes it a sibling. */
        if (_strnicmp(g_rec[i].name, "lights/", 7) != 0 &&
            _strnicmp(g_rec[i].name, "lights_blended/", 15) != 0)
            continue;
        if ((g_recCount & 1023) == 0) {
            rec_t *bigger = (rec_t *)realloc(g_rec, (size_t)(g_recCount + 1024) * sizeof *bigger);
            if (!bigger) break;
            g_rec = bigger;
        }
        g_rec[g_recCount] = g_rec[i];
        g_rec[g_recCount].kind = SH_ASSET_LIGHT;
        g_recCount++;
        lights++;
    }

    /* Collapse records that repeat a name WITHIN one box, before anything else looks at the list.
     *
     * The index is a record-per-blob table, not a catalog of distinct assets: the same decl can be
     * baked into the .resources file more than once, at different offsets. `decalatlas` is where it
     * shows -- 1,673 records for 1,024 distinct names -- and the browser was faithfully listing all
     * of them, so a mapper saw every decal twice. Clicking one selected both rows and starring one
     * starred both, because the two rows ARE the same name and the UI keys off the name.
     *
     * Measured across snap_gameresources: decalatlas 649 repeats, image 1, and exactly zero for
     * material, model, md6Def, sound, fx, particle, entityDef, snapEditorEntityDef and cm. So this
     * is general on purpose but only ever fires where the data actually repeats.
     *
     * The FIRST record wins, which is also what find_rec would have resolved to, so nothing that
     * already previewed changes which blob it reads. */
    int boxdup = 0;
    {
        int *ord = (int *)malloc((size_t)g_recCount * sizeof *ord);
        if (ord) {
            for (int i = 0; i < g_recCount; ++i) ord[i] = i;
            g_sortRec = g_rec;
            qsort(ord, (size_t)g_recCount, sizeof *ord, cmp_rec_kind_name);
            for (int i = 1; i < g_recCount; ++i) {
                const rec_t *a = &g_rec[ord[i - 1]], *b = &g_rec[ord[i]];
                if (a->kind != b->kind || a->box != b->box) continue;
                if (_stricmp(a->name, b->name) != 0) continue;
                /* Sorted by (kind, box, name, ORIGINAL INDEX), so ord[i] is always the later
                 * record of the pair and the first one indexed is the one left standing. */
                if (!g_rec[ord[i]].hidden) { g_rec[ord[i]].hidden = 1; boxdup++; }
            }
            free(ord);
        }
    }

    /* Decide, once, which records the browser will LIST.
     *
     * Box 0 (`snap_gameresources`) is everything SnapMap ships with. Box 1 (`gameresources`) is
     * the base game's broader set; most of it is unreferencable from SnapMap, which is why it is not
     * offered wholesale. SOUNDS are the exception worth making: 1,186 sound decls exist only in
     * box 1 and 231 of those are `vo_*`, which is a category a mapper visibly misses. The browser's
     * working play/stop path makes each offered event directly testable from a SnapMap session.
     *
     * Duplicates are dropped rather than shown twice: 2,401 sound names appear in both boxes. The
     * box-0 record wins, so nothing that already worked changes route. */
    const char **snapSounds = (const char **)malloc((size_t)g_recCount * sizeof *snapSounds);
    int snapSoundCount = 0;
    if (snapSounds) {
        for (int i = 0; i < g_recCount; ++i)
            if (g_rec[i].box == 0 && g_rec[i].kind == SH_ASSET_SOUND && !g_rec[i].hidden)
                snapSounds[snapSoundCount++] = g_rec[i].name;
        qsort(snapSounds, (size_t)snapSoundCount, sizeof *snapSounds, cmp_ci);
    }

    int dup = 0, extra = 0;
    for (int i = 0; i < g_recCount; ++i) {
        if (g_rec[i].box == 0) continue;
        if (g_rec[i].kind != SH_ASSET_SOUND) { g_rec[i].hidden = 1; continue; }
        if (g_rec[i].hidden) continue;

        const char *key = g_rec[i].name;
        int twin = snapSounds &&
            bsearch(&key, snapSounds, (size_t)snapSoundCount, sizeof *snapSounds, cmp_ci) != NULL;
        if (!snapSounds) {
            for (int j = 0; j < g_recCount && !twin; ++j)
                if (g_rec[j].box == 0 && g_rec[j].kind == SH_ASSET_SOUND &&
                    !g_rec[j].hidden && _stricmp(g_rec[j].name, key) == 0)
                    twin = 1;
        }
        if (twin) { g_rec[i].hidden = 1; dup++; }
        else extra++;
    }
    free(snapSounds);

    g_rec = (rec_t *)imgpreview_shrink_array(g_rec, (size_t)g_recCount, sizeof *g_rec);

    char line[600];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- indexed %d records (snap=%s game=%s); %d SnapMap modules; "
        "%d light material(s) also listed under Lights; "
        "%d record(s) collapsed as a repeat of a name in the same box; "
        "index metadata %llu raw bytes -> %llu retained bytes (%s); "
        "base-game sounds offered: %d (%d duplicates of SnapMap sounds dropped); "
        "optional sound and .vmtr catalogs remain unloaded; base catalog ready in %llu ms",
        g_recCount, a ? "ok" : "MISSING", b ? "ok" : "missing", modules, lights, boxdup,
        (unsigned long long)indexBytes, (unsigned long long)retainedIndexBytes, indexMode,
        extra, dup, (unsigned long long)(GetTickCount64() - started));
    backend_log(line);
    g_loaded = (g_recCount > 0) ? 1 : -1;
    return g_loaded > 0;
}

static void imgpreview_prepare_list_kind(int kind)
{
    if (kind == SH_ASSET_SOUND || kind == SH_ASSET_SNDBANK) {
        imgpreview_load_sound_catalog();
    } else if (kind == SH_ASSET_MATERIAL || kind == SH_ASSET_VTONLY) {
        imgpreview_load_vmtr();
    }
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
 * by trying it. Takes the same lock as the list path; the base catalog is parsed lazily, and Wwise
 * metadata is added only for a sound lookup. */
int sh_imgpreview_has(int kind, const char *name)
{
    if (!name || !name[0]) return 0;
    EnterCriticalSection(&g_lock);
    int ok = 0;
    if (imgpreview_load()) {
        if (kind == SH_ASSET_SOUND) imgpreview_load_sound_catalog();
        ok = find_rec(name, kind) != NULL;
        /* A Wwise event with no decl is still a real, playable name -- up to 2,591 relative to the
         * SnapMap-only decl set, including generic VO (the broader sound union claims more exact
         * matches). The engine resolves it through find-or-CREATE, which builds the
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
    const rec_t *r = find_rec(name, SH_ASSET_IMAGE);
    if (r) return r;
    size_t n = strlen(name);
    for (int i = 0; i < g_recCount; ++i) {
        if (g_rec[i].kind != SH_ASSET_IMAGE) continue;
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
    size_t n = sh_inflate_raw(raw, r->csz, out, r->usz);
    free(raw);
    *out_len = n;
    return out;
}

int sh_imgpreview_read_payload(int kind, const char *name, size_t max_bytes,
                               unsigned char **out_bytes, size_t *out_len)
{
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    if (!out_bytes || !out_len || !name || !name[0] || max_bytes == 0) return 0;

    EnterCriticalSection(&g_lock);
    int ok = 0;
    if (imgpreview_load()) {
        const rec_t *r = find_rec(name, kind);
        if (r && r->usz > 0 && (size_t)r->usz <= max_bytes) {
            size_t n = 0;
            unsigned char *body = read_payload(r, &n);
            if (body && n == (size_t)r->usz && n <= max_bytes) {
                *out_bytes = body;
                *out_len = n;
                ok = 1;
            } else {
                free(body);
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
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

static int imgpreview_produce_kind(const char *name, unsigned long generation, int direct_image)
{
    ULONGLONG started = GetTickCount64();
    EnterCriticalSection(&g_lock);
    int ok = 0;
    unsigned char *decl = NULL, *bim = NULL, *rgba = NULL, *thumb = NULL;
    __try {
        if (!imgpreview_load()) __leave;

        char img[256];
        const char *image_name = NULL;
        const rec_t *ir = NULL;
        const rec_t *mr = direct_image ? NULL : find_rec(name, SH_ASSET_MATERIAL);
        if (mr) {
            size_t dlen = 0; decl = read_payload(mr, &dlen);
            if (!decl || !dlen) { backend_log("B2: imgpreview -- decl read failed"); __leave; }
            if (!decl_find_image((const char *)decl, dlen, img, sizeof img)) {
                char l[320]; _snprintf_s(l,sizeof l,_TRUNCATE,
                    "B2: imgpreview -- '%s' decl names no usable image (atlased decal/particle?)", name);
                backend_log(l); __leave;
            }
            ir = find_image(img);
            image_name = img;
            if (!ir) {
                char l[400]; _snprintf_s(l,sizeof l,_TRUNCATE,
                    "B2: imgpreview -- '%s' -> image '%s' not in any container", name, img);
                backend_log(l); __leave;
            }
        } else {
            ir = find_image(name);
            if (!ir) {
                backend_log(direct_image ? "B2: imgpreview -- no direct image record"
                                         : "B2: imgpreview -- no material or image record");
                __leave;
            }
            image_name = ir->name;
        }

        size_t blen = 0; bim = read_payload(ir, &blen);
        if (!bim || blen < 0x40 || memcmp(bim + 4, "\x07MIB", 4) != 0) {
            backend_log("B2: imgpreview -- not a .bimage"); __leave;
        }
        unsigned fmt = le32(bim + 0x20);
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

        ok = sh_preview_publish(generation, thumb, dw, dh);
        if (ok != SH_PREVIEW_PUBLISHED) __leave;
        char l[400];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "B2: imgpreview -- '%s' -> image '%s' %ux%u fmt=%u -> %ux%u preview in %llu ms",
            name, image_name, w, h, fmt, dw, dh,
            (unsigned long long)(GetTickCount64() - started));
        backend_log(l);
    } __finally {
        free(decl); free(bim); free(rgba); free(thumb);
        LeaveCriticalSection(&g_lock);
    }
    return ok;
}

int sh_imgpreview_produce(const char *name, unsigned long generation)
{
    return imgpreview_produce_kind(name, generation, 0);
}

int sh_imgpreview_produce_image(const char *name, unsigned long generation)
{
    return imgpreview_produce_kind(name, generation, 1);
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
        imgpreview_prepare_list_kind(kind);
        size_t used = 0;
        unsigned seen = 0;
        for (int i = 0; i < g_recCount; ++i) {
            /* `hidden` is decided once at load: everything in box 1 except sounds, and box-1
             * sounds that duplicate a SnapMap one. See imgpreview_load. */
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
        /* The sound -> soundbank map, served on its own pseudo-kind as `event|bank` lines. Not
         * folded into SH_ASSET_SOUND: that list is names the UI applies verbatim, and appending a
         * bank to them would corrupt every one. The UI keeps this as a side map instead. */
        if (kind == SH_ASSET_SNDBANK) {
            for (int b = 0; b < g_sbCount; ++b) {
                if (seen++ < start) continue;
                size_t n = strlen(g_sb[b].name), m = strlen(g_sb[b].bank);
                if (used + n + m + 3 > cap) break;               /* name + '|' + bank + '\n' + NUL */
                memcpy(out + used, g_sb[b].name, n); used += n;
                out[used++] = '|';
                memcpy(out + used, g_sb[b].bank, m); used += m;
                out[used++] = '\n';
                written++;
            }
        }
        /* Same continuation for the decl-less `.vmtr` materials -- they are as applyable as any
         * decl-backed one (by rectangle rather than by name), so they belong in the same list.
         * SH_ASSET_VTONLY serves the SAME array on its own, so the UI can tell the two apart. */
        if (kind == SH_ASSET_MATERIAL || kind == SH_ASSET_VTONLY) {
            for (int v = 0; v < g_vtCount; ++v) {
                if (seen++ < start) continue;
                size_t n = strlen(g_vt[v]);
                if (used + n + 2 > cap) break;
                memcpy(out + used, g_vt[v], n); used += n;
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
