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
#include "megapreview.h"   /* the .vmtr atlas: the other half of the material catalog */
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
     * name already in Models, and they are in the SNAP box, so unlike a campaign-box model they
     * actually load rather than rendering as a black cube. They take renderModelInfo.model like any
     * other model, so they belong in the same category rather than a separate one. */
    { "discreteAnimation",  17, SH_ASSET_MODEL,      NULL,   0 },
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
static unsigned char *g_wwise;           /* the manifest text, kept alive: names point into it */
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
            /* `continue`, NOT `break`: one decl type can map to several kinds discriminated only by
             * extension (`model` -> .lwo Models and .bmodel Brush models). Breaking here on the
             * first entry whose suffix misses would drop every record the later entry owns. */
            if (g_kinds[k].suffix &&
                (nl < g_kinds[k].slen ||
                 _strnicmp(name + nl - g_kinds[k].slen, g_kinds[k].suffix, g_kinds[k].slen) != 0))
                continue;                           /* right type, wrong extension -> try next kind */
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

/* Sorts RECORD INDICES by (kind, box, name, original index) so repeats of one name inside one box
 * land next to each other. The index is the last key on purpose: it makes the order total, so the
 * pass that walks the result can rely on the earlier-indexed record always coming first and keep
 * that one. Reads through g_sortRec because qsort gives the comparator no context of its own. */
static const rec_t *g_sortRec;
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

    char line[300];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- Wwise manifest: %d distinct event(s) with no decl added to the sound "
        "catalog (%d bank-repeats collapsed, %d already had a decl); %d event(s) mapped to a bank",
        g_evCount, raw - g_evCount, dup, g_sbCount);
    backend_log(line);
}

/* Hide a sound decl that is nothing but a wrapper around a Wwise event already in the catalog.
 *
 * The exact-name dedup in imgpreview_load_wwise catches the 5,160 decls literally NAMED `play_*`,
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
 * The decl array is built from the records still VISIBLE at this point, so a campaign-box twin that
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

/* Fold the `.vmtr` atlas rows that have NO material decl into the material catalog. Same shape as
 * imgpreview_load_wwise: sort the decl names once, then binary-search each atlas row against them.
 * Names point into megapreview's parsed table, which lives for the process, so nothing is copied.
 * Failure is non-fatal -- a missing atlas just leaves the catalog decl-only, as it was before. */
static void imgpreview_load_vmtr(void)
{
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

    char line[240];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- .vmtr atlas: %d distinct decl-less material(s) added to the material "
        "catalog (%d shard-repeats collapsed, %d already had a decl)",
        g_vtCount, raw - g_vtCount, dup);
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

    /* AFTER the box dedup, so a campaign sound already hidden as a box-0 duplicate is not counted
     * twice, and the tally reports only rows this pass is actually responsible for removing. */
    int wrapped = imgpreview_hide_wrapped_sounds();

    /* AFTER the hide pass: it dedupes against the decls the browser will actually list, so a
     * campaign-box material that got hidden above must not suppress its atlas twin. */
    imgpreview_load_vmtr();

    char line[480];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: imgpreview -- indexed %d records (snap=%s game=%s); %d SnapMap modules; "
        "%d record(s) collapsed as a repeat of a name in the same box; "
        "campaign sounds offered: %d (%d duplicates of SnapMap sounds dropped); "
        "%d wrapper sound decl(s) hidden behind their Play_ event",
        g_recCount, a ? "ok" : "MISSING", b ? "ok" : "missing", modules, boxdup, extra, dup, wrapped);
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
