/* vtpack.h -- mods' lit virtual-texture pages packed into square 16 of the
 * stock `_vmtr` family (docs/vt-baker.md, "Lit pages"). Pure: no engine, no
 * file system; modvt.c serves what this builds and modbundle.c checks a
 * mod's member with it.
 *
 * Why: every lit model program of DOOM 2016 (skinvmtr, skinvmtrwounds,
 * outside and the rest) samples only the stock `_vmtr` family, so a mod's lit
 * pages draw only from inside it. Square 16 of the stock family holds no
 * level-0 page and one page row at its top edge at levels 1 to 8.
 *
 * The mod form, relative to the mod (nothing names square 16 or another mod):
 *   member     virtualtextures/mods/<salt>/vmtr.mega2: a version 2 member
 *              whose level table is square 1's, its pages from the origin.
 *              Its extent is the bounding box of its level-0 cells (w x h
 *              pages); its node is the smallest power of two S >= max(w, h),
 *              at most SH_VTP_NODE_MAX; every non-empty cell of level L lies
 *              inside max(S >> L, 1) cells per side and has a page above it.
 *   materials  landpagefile "mods/<salt>/vmtr", usevirtualmapping 1 and a
 *              virtualmapping { w, h, x, y } relative to the member's origin.
 *
 * The packer, deterministic for its inputs:
 *   place      a node already held keeps its place; every other node, largest
 *              first, then by key, takes the S-aligned position of square 16
 *              that touches no stock cell at levels 0..log2 S and no other
 *              node, choosing the most carried levels (below), then the lowest
 *              row, then the lowest column.
 *   carry      levels 0..log2 S of a mod are always written: its own cells
 *              moved by the node's position >> L. A coarser level L, up to
 *              SH_VTP_CARRY_MAX, is also written (its own cell L, 0, 0) while
 *              the node is aligned to 2^L, the stock cell there is empty and no
 *              other served node lies in that 2^L block. Levels 9 to 11 are
 *              cells shared with other squares and stay stock.
 *   member     every stock page unchanged and in its stock order (so every
 *              stock cell keeps its page and bytes), then each served mod's
 *              pages in key order, each page once.
 *   material   the stock form: landpagefile and any stock vmtr parameter
 *              dropped, the stock vmtr tail added before the closing brace, and
 *              the mapping's x and y moved to the node's place in the family.
 *
 * Doomforge's tools/vt/vmtr_square.py is the reference implementation; the
 * tests compare the two. */
#ifndef SH_VTPACK_H
#define SH_VTPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_VTP_MAGIC      0xA63FBB21u
#define SH_VTP_HDR        0x170u
#define SH_VTP_LEVELS     12
#define SH_VTP_SQ_PAGES   512          /* level-0 pages per square side */
#define SH_VTP_PAGES0     2048         /* level-0 pages per family side */
#define SH_VTP_TEXELS     245760       /* level-0 texels per family side */
#define SH_VTP_PAGE       120          /* payload texels per page side */
#define SH_VTP_CELLS      349527u      /* cells of one member, every level */
#define SH_VTP_EMPTY      0xFFFFFFFFu
#define SH_VTP_SQUARE     16           /* the stock square the packer fills */
#define SH_VTP_LIT_SQUARE 1            /* the relative form's own square */
#define SH_VTP_NODE_MAX   256          /* a 512 node is the whole square, its top row included */
#define SH_VTP_CARRY_MAX  8            /* levels 9..11 are shared with other squares */
#define SH_VTP_PAGE_MAX   0x30000u     /* 16 + three 64 KiB streams + extra + the cover bitmap */

/* ------------------------------------------------------------------ members */

typedef struct sh_vtp_member {
    const uint8_t *data;
    size_t len;
    uint64_t table_off, index_off;
    uint32_t page_count;
    int square;                         /* the square its level table is, 1..16 */
} sh_vtp_member;

/* Check and open a member held in memory (docs/vt-baker.md Validation 1):
 * magic, version 2, the family constants, a level table that is one square's,
 * the cell count, the index and page table inside the file, every non-empty
 * cell naming a page below the page count, and every page inside the payload
 * area with no two overlapping. 1 opened, else 0 with the cause in `why`. */
int sh_vtp_open(sh_vtp_member *m, const uint8_t *data, size_t len, char *why, size_t cap);

/* The page id at square-local cell (x, y) of level L, or SH_VTP_EMPTY. */
uint32_t sh_vtp_cell(const sh_vtp_member *m, int level, int x, int y);

/* A stored page: 1 with its bytes, 0 when id is out of range. */
int sh_vtp_page(const sh_vtp_member *m, uint32_t id, const uint8_t **bytes, size_t *len);

/* A stored page's structure (Validation 2): 16-byte header, stored size equal
 * to the header plus its streams (and the cover bitmap), consistent absent
 * flags, never the lightmap flag. 1 good, else 0 with the cause. */
int sh_vtp_page_ok(const uint8_t *p, size_t n, char *why, size_t cap);

/* ------------------------------------------------------------------ the relative form */

typedef struct sh_vtp_lit {
    int w, h;                           /* extent in level-0 pages */
    int node;                           /* S */
    uint32_t pages;
    uint64_t page_bytes;
} sh_vtp_lit;

/* A mod's lit member in the relative form (above): an opened member of
 * square 1 with level-0 pages, a node of at most SH_VTP_NODE_MAX, every cell
 * inside its node with a page above it, every page structurally sound. 1, or
 * 0 with the cause in a sentence fragment ("its pages cover 300 x 40 level-0
 * pages; ..."). */
int sh_vtp_lit_info(const sh_vtp_member *m, sh_vtp_lit *out, char *why, size_t cap);

/* ------------------------------------------------------------------ packing */

typedef struct sh_vtp_node { int x, y, size; } sh_vtp_node;   /* square-local level-0 pages */

/* 1 when an S x S node at (x, y) touches no stock cell at levels 0..log2 S. */
int sh_vtp_fits(const sh_vtp_member *stock, int x, int y, int size);

/* The highest level written for `node` beside `others` (the other served
 * nodes; the node itself must not be among them). */
int sh_vtp_carry(const sh_vtp_member *stock, sh_vtp_node node, const sh_vtp_node *others, int n);

typedef struct sh_vtp_req {
    const char *key;                    /* orders the placement and the member */
    int size;                           /* S */
    int held;                           /* in: 1 keeps `node` as it is */
    int placed;                         /* out: 1 placed (held, or a new place) */
    sh_vtp_node node;                   /* in when held, out when placed */
} sh_vtp_req;

/* Place every request (rules above). Returns how many are placed; a request
 * with no room stays unplaced. */
int sh_vtp_place(const sh_vtp_member *stock, sh_vtp_req *reqs, int n);

typedef struct sh_vtp_mod {
    const char *key;
    const sh_vtp_member *m;             /* its relative-form member */
    sh_vtp_node node;
    int levels;                         /* out: levels written (log2 S + 1 and more) */
    uint32_t cells, pages;              /* out */
} sh_vtp_mod;

/* The merged member of square 16: the stock member (a member of square 16)
 * plus `mods`, served now, each at its node. A malloc'd image in *out. 1, or
 * 0 with the cause (out of memory, a member that is not square 16's, two mods
 * on one cell). */
int sh_vtp_compose(const sh_vtp_member *stock, sh_vtp_mod *mods, int n, uint8_t **out, size_t *out_len,
                   char *why, size_t cap);

/* ------------------------------------------------------------------ materials */

/* 1 when `text` names landpagefile "mods/<salt>/vmtr" (ASCII case-insensitive). */
int sh_vtp_material_names(const char *text, size_t len, const char *salt);

/* The virtualmapping { w, h, x, y } of `text`: 1 with the four values, else 0. */
int sh_vtp_material_mapping(const char *text, size_t len, double out[4]);

/* The material in stock form at `node` (rules above): a malloc'd text, LF line
 * ends, or NULL when it has no virtualmapping or no closing brace line. */
char *sh_vtp_material_moved(const char *text, size_t len, sh_vtp_node node, size_t *out_len);

/* The material of a mod whose pages found no room: every virtual-texture line
 * dropped and the programs that read no virtual texture (outsidenonvmtr,
 * shadownonvmtr), so it draws its ordinary images. Malloc'd, or NULL with no
 * closing brace line. */
char *sh_vtp_material_plain(const char *text, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
