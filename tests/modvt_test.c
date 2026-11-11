/* modvt_test.c -- a mod's own lit virtual-texture board (modvt.c, pure half).
 *
 *   modvt_test.exe [<DOOMx64vk.exe> [<DOOMx64.exe>]]
 *
 * Bundles are built by the real builder and admitted by the real serving
 * index; the engine is a fake 32-slot record table with record memory, a fake
 * material-format cache and a fake parameter set. Checked: the member path
 * rule, which boards register (one record per board, several per mod), each
 * pointed at the material-format cache, rescan idempotence, release only at
 * the frame boundary (pending before), a changed build, a full table (stock
 * records and record 31 untouched, one line, one player notice), a name
 * another record holds, a record the engine took back, a cache that cannot be
 * adopted, the lookup returning a board for its own name only, the binder
 * addition on board materials (two boards at once), a stock material's
 * parameters and record 1 byte-identical with and without boards, and earlier
 * materials bound again when a board returns. With executables: the four
 * signatures resolve at their recorded RVAs on each, and everything modvt.c
 * decodes from them matches the recorded addresses and the signatures it is
 * checked against. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modvt.h"
#include "../src/backend/signatures.h"
#include "../src/backend/host_image.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ------------------------------------------------------------------ log capture */

static char g_log[64][1024];
static int g_nlog;
static int g_echo_log;

void backend_log(const char *line)
{
    if (g_nlog < 64) strncpy_s(g_log[g_nlog++], sizeof g_log[0], line, _TRUNCATE);
    if (g_echo_log) printf("    | %s\n", line);
}

static int log_count(const char *needle)
{
    int i, n = 0;
    for (i = 0; i < g_nlog; ++i) n += strstr(g_log[i], needle) != NULL;
    return n;
}

/* ------------------------------------------------------------------ fake engine */

#define REC_BYTES 0x420

typedef struct fake {
    int ready;
    char name[SH_MODVT_SLOTS][HO_TEXT];
    char path[SH_MODVT_SLOTS][HO_TEXT];
    uint8_t rec[SH_MODVT_SLOTS][REC_BYTES];
    int cache_vmtr[4], cache_unique_vmtrs[4];     /* first int: the cache's flavor */
    uint8_t ptimage[SH_MODVT_SLOTS][8];           /* stands for each record's page-table image */
    int sets, releases, refuse_set, adopts, refuse_adopt, rebinds, notices;
    char notice[256];
} fake;

static int f_ready(void *c) { return ((fake *)c)->ready; }
static int f_find(void *c, const char *name)
{
    fake *f = (fake *)c;
    int k;
    for (k = 1; k < SH_MODVT_SLOTS; ++k) if (f->name[k][0] && !strcmp(f->name[k], name)) return k;
    return -1;
}
static int f_is_free(void *c, int k) { fake *f = (fake *)c; return !f->name[k][0] && !f->path[k][0]; }
static void *f_record(void *c, int k) { return ((fake *)c)->rec[k]; }
static void put_ptr(uint8_t *at, void *p) { memcpy(at, &p, sizeof p); }
static void *get_ptr(const uint8_t *at) { void *p; memcpy(&p, at, sizeof p); return p; }
static int f_set(void *c, int k, const char *name, const char *path)
{
    fake *f = (fake *)c;
    f->sets++;
    if (k < 1 || k > 30 || f->name[k][0]) { printf("    fake: set_source on a used or forbidden record %d\n", k); return -1; }
    if (f->refuse_set) return 0;   /* the engine clears a record whose open fails */
    strcpy_s(f->name[k], sizeof f->name[k], name);
    strcpy_s(f->path[k], sizeof f->path[k], path);
    /* SetSource with flavor 2: the UniqueVmtrs cache, a page-table image, the source index. */
    memset(f->rec[k], 0, REC_BYTES);
    *(int *)(f->rec[k] + SH_VT_REC_TYPE) = 2;
    *(int *)(f->rec[k] + SH_VT_REC_SOURCE) = k;
    put_ptr(f->rec[k] + SH_VT_REC_CACHE, f->cache_unique_vmtrs);
    put_ptr(f->rec[k] + SH_VT_REC_PTIMAGE, f->ptimage[k]);
    return 1;
}
static int f_adopt(void *c, int k)
{
    fake *f = (fake *)c;
    int *cache = (int *)get_ptr(f->rec[1] + SH_VT_REC_CACHE);
    f->adopts++;
    if (f->refuse_adopt || !cache || cache[0] != 0 || k < 2) return 0;
    put_ptr(f->rec[k] + SH_VT_REC_CACHE, cache);
    return 1;
}
static int f_release(void *c, int k)
{
    fake *f = (fake *)c;
    f->releases++;
    f->name[k][0] = 0;
    f->path[k][0] = 0;
    memset(f->rec[k], 0, REC_BYTES);
    return 1;
}
static void f_notice(void *c, const char *text)
{
    fake *f = (fake *)c;
    f->notices++;
    strcpy_s(f->notice, sizeof f->notice, text);
}

/* ---- the fake material and binder: a parameter set of 16-byte slots by handle */

#define PARMS 8
enum { H_FEEDBACK_U = 1, H_PAGETABLE_U, H_PHYS_U, H_VMTR_FEEDBACK, H_VMTR_PAGETABLE };
typedef struct fake_material {
    uint8_t head[SH_VT_MAT_PARMS];
    uint8_t parms[PARMS][16];
    int record;
    const char *landpage;
} fake_material;

static float *b_feedback(void *rec, float *out)
{
    out[0] = (float)*(int *)((uint8_t *)rec + SH_VT_REC_SOURCE);
    out[1] = 2048.0f;
    out[2] = 120.0f * 2048.0f;
    out[3] = 16.0f;
    return out;
}
static void b_vec4(void *parms, void *handle, const float *v) { memcpy((uint8_t *)parms + 16 * (size_t)handle, v, 16); }
static void b_tex(void *parms, void *handle, void *image)
{
    memset((uint8_t *)parms + 16 * (size_t)handle, 0, 16);
    memcpy((uint8_t *)parms + 16 * (size_t)handle, &image, sizeof image);
}
static const sh_modvt_bind_ops g_bops = { b_feedback, b_vec4, b_tex, (void *)(uintptr_t)H_VMTR_FEEDBACK,
                                          (void *)(uintptr_t)H_VMTR_PAGETABLE };

/* The engine binder's own writes (the unprefixed names lit programs never
 * read), then the detour's addition. */
static void fake_binder(void *parms, void *rec)
{
    float w[4];
    b_vec4(parms, (void *)(uintptr_t)H_FEEDBACK_U, b_feedback(rec, w));
    b_tex(parms, (void *)(uintptr_t)H_PAGETABLE_U, get_ptr((uint8_t *)rec + SH_VT_REC_PTIMAGE));
    b_tex(parms, (void *)(uintptr_t)H_PHYS_U, get_ptr((uint8_t *)rec + SH_VT_REC_CACHE));
    (void)sh_modvt_bind_board(parms, rec, &g_bops);
}

/* The material parse: the record whose name equals the landpagefile. */
static void fake_parse(fake *f, fake_material *m, const char *landpage)
{
    int k = f_find(f, landpage);
    memset(m, 0, sizeof *m);
    m->landpage = landpage;
    m->record = k < 0 ? 1 : k;
    fake_binder(m->parms, f->rec[m->record]);
}

static int f_rebind(void *c, void *material, int k, const char *name)
{
    fake *f = (fake *)c;
    fake_material *m = (fake_material *)material;
    if (strcmp(m->landpage, name)) return 0;
    f->rebinds++;
    m->record = k;
    memset(m->parms, 0, sizeof m->parms);
    fake_binder(m->parms, f->rec[k]);
    return 1;
}

static void fake_init(fake *f)
{
    memset(f, 0, sizeof *f);
    f->ready = 1;
    f->cache_vmtr[0] = 0;
    f->cache_unique_vmtrs[0] = 2;
    strcpy_s(f->name[1], sizeof f->name[1], "_vmtr");
    strcpy_s(f->path[1], sizeof f->path[1], "virtualtextures/_vmtr.pages");
    *(int *)(f->rec[1] + SH_VT_REC_TYPE) = 2;
    *(int *)(f->rec[1] + SH_VT_REC_SOURCE) = 1;
    put_ptr(f->rec[1] + SH_VT_REC_CACHE, f->cache_vmtr);
    put_ptr(f->rec[1] + SH_VT_REC_PTIMAGE, f->ptimage[1]);
    strcpy_s(f->name[2], sizeof f->name[2], "maps/game/sp/shell/shell");      /* the hub's unique family */
    strcpy_s(f->path[2], sizeof f->path[2], "virtualtextures/maps/game/sp/shell/shell.pages");
    *(int *)(f->rec[2] + SH_VT_REC_TYPE) = 1;
    strcpy_s(f->name[3], sizeof f->name[3], "maps/modules/palettes/mega_blessed/mega");
    strcpy_s(f->path[3], sizeof f->path[3], "virtualtextures/maps/modules/palettes/mega_blessed/mega.pages");
    *(int *)(f->rec[3] + SH_VT_REC_TYPE) = 1;
    *(int *)(f->rec[3] + SH_VT_REC_SOURCE) = 3;
    put_ptr(f->rec[3] + SH_VT_REC_CACHE, f->cache_unique_vmtrs);
    put_ptr(f->rec[3] + SH_VT_REC_PTIMAGE, f->ptimage[3]);
}

/* ------------------------------------------------------------------ helpers */

static int sync_index(fake *f, const char *phase, int release_now, sh_modvt_result *res)
{
    sh_modvt_family *fams = NULL;
    sh_modvt_ops ops = { f, f_ready, f_find, f_is_free, f_set, f_adopt, f_release, f_record, f_rebind, f_notice };
    int n = sh_modvt_collect(&fams), r;
    r = sh_modvt_sync(fams, n, &ops, phase, release_now, res);
    free(fams);
    return r;
}

static int stock_exists(void *ctx, const char *request) { (void)ctx; return strstr(request, "mods/") == NULL; }

static int build_folder(const char *folder, char *hash_out)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    char cause[HO_TEXT];
    int ok = sh_mb_build(folder, NULL, &r, &b) == 1 && b.errors == 0 && HO(cause, sh_mb_write(&b, folder, HO_P(0))) == 1;
    if (!ok) printf("    build %s: %u error(s): %s\n", folder, b.errors, b.errors ? sh_mb_first_cause(&b) : "");
    if (hash_out) strcpy_s(hash_out, 65, b.hash);
    sh_mb_free(&b);
    return ok;
}

static const char *build_cause(const char *folder, char *cause, size_t cap)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    sh_mb_build(folder, NULL, &r, &b);
    strcpy_s(cause, cap, b.errors ? sh_mb_first_cause(&b) : "");
    sh_mb_free(&b);
    return cause;
}

static void put_mod(const char *folder, const char *id)
{
    char json[256];
    /* The block the store's scan writes for a fresh folder: the salt alone. */
    _snprintf_s(json, sizeof json, _TRUNCATE,
                "{\"name\":\"%s\",\"author\":\"t\",\"snapmap-plus\":{\"format\":2,\"salt\":\"%s\"}}", id, id);
    mt_write(folder, "mod.json", json);
}

static int g_listener_calls;
static void listener(void) { g_listener_calls++; }

/* ------------------------------------------------------------------ cases */

static void member_rule(void)
{
    char fam[HO_TEXT];
    int m = 0;
    printf("-- the member path rule\n");
    CHECK(HO(fam, sh_mb_vt_member("virtualtextures/mods/rctest00/fam_sq1.mega2", HO_P(0), &m)) == 1 &&
          !strcmp(fam, "mods/rctest00/fam") && m == 1);
    CHECK(HO(fam, sh_mb_vt_member("virtualtextures/mods/alexpink/skin/body_sq16.mega2", HO_P(0), &m)) == 1 &&
          !strcmp(fam, "mods/alexpink/skin/body") && m == 16);
    CHECK(sh_mb_vt_member("VirtualTextures/Mods/rctest00/Fam_SQ3.MEGA2", NULL, &m) == 1 && m == 3);
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/fam_sq17.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/fam_sq0.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/fam_sq01.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/fam.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/_sq1.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/fam_q1.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/fam_sq1.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/_vmtr_sq1.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/my_vmtr_sq1.mega2", NULL, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest00/fam_sq1.pages", NULL, NULL));
    CHECK(!sh_mb_vt_member("models/mods/rctest00/fam_sq1.mega2", NULL, NULL));
    CHECK(sh_mb_extension_allowed("virtualtextures/mods/rctest00/fam_sq2.mega2"));
    CHECK(!sh_mb_extension_allowed("models/mods/rctest00/fam_sq2.mega2"));
    CHECK(!sh_mb_extension_allowed("virtualtextures/mods/rctest00/fam.pages"));
}

static void decode_helpers(void)
{
    /* SetSource +0x50 (VK) and +0x4E (GL): LEA RCX,[sys]; CALL sync; MOV RCX,RSI; CALL Release. */
    static const unsigned char vk[] = { 0x48, 0x89, 0xF1, 0x48, 0x8D, 0x0D, 0xB9, 0x5C, 0xE8, 0x04, 0xE8, 0x64, 0xAD,
                                        0xF8, 0xFF, 0x48, 0x8B, 0xCE, 0xE8, 0xEC, 0xE7, 0xFF, 0xFF, 0x48, 0x8D };
    static const unsigned char gl[] = { 0x48, 0x8D, 0x0D, 0x3B, 0xA4, 0x78, 0x03, 0xE8, 0x86, 0xAB, 0xF8, 0xFF, 0x48,
                                        0x8B, 0xCE, 0xE8, 0x6E, 0xE7, 0xFF, 0xFF };
    /* VirtualTextureForMaterial +0x61 (VK) / +0x81 (GL): MOV RAX,[R13+table+8]; JMP. */
    static const unsigned char tvk[] = { 0x75, 0x0C, 0x49, 0x8B, 0x85, 0x08, 0x8B, 0x00, 0x00, 0xE9, 0x38, 0x01 };
    static const unsigned char tgl[] = { 0x49, 0x8B, 0x85, 0x08, 0xA4, 0x00, 0x00, 0xE9, 0x38, 0x01, 0x00, 0x00 };
    /* The per-view setter's two sites (VK 0x17B4578, 0x17B4643) and JobsWaitAll's LEA (VK 0x31BF7E). */
    static const unsigned char pvk[] = { 0x4C, 0x8B, 0x05, 0x01, 0xCC, 0xF2, 0x04, 0x49, 0x8B, 0xCF, 0x48, 0x8B, 0x15,
                                         0x2F, 0x0A, 0x09, 0x02, 0xE8, 0xC2, 0x25, 0x14, 0x00 };
    static const unsigned char fvk[] = { 0x48, 0x8B, 0x1D, 0x8E, 0x0A, 0x09, 0x02, 0x48, 0x8D, 0x95, 0xE0, 0x00, 0x00,
                                         0x00, 0x48, 0x8D, 0x0D, 0x40, 0xC7, 0xF2, 0x04, 0xE8, 0x23, 0x51, 0x0A, 0x00 };
    static const unsigned char rel[] = { 0x48, 0x8B, 0xC4, 0x55, 0x90, 0x83, 0xB9, 0xC0, 0x02, 0x00, 0x00, 0x00 };
    printf("-- decoding helpers\n");
    CHECK(sh_modvt_find_setsource_head(vk, sizeof vk) == 3);
    CHECK(sh_modvt_find_setsource_head(gl, sizeof gl) == 0);
    CHECK(sh_modvt_find_setsource_head(gl, 10) == -1);
    CHECK(sh_modvt_find_table_disp(tvk, sizeof tvk) == 0x8B08u);
    CHECK(sh_modvt_find_table_disp(tgl, sizeof tgl) == 0xA408u);
    CHECK(sh_modvt_find_table_disp(tgl, 7) == 0);
    CHECK(sh_modvt_release_head_ok(rel, sizeof rel));
    CHECK(!sh_modvt_release_head_ok(rel, 8));
    {
        static unsigned char ss[0xC0], vf[0xC0], bd[0xC0], rb[0x70];
        sh_modvt_layout l;
        sh_modvt_binder b;
        sh_modvt_rebind r;
        memset(ss, 0x90, sizeof ss);
        memcpy(ss + 0x40, gl, sizeof gl);
        memset(vf, 0x90, sizeof vf);
        memcpy(vf + 0x81, tgl, sizeof tgl);
        CHECK(sh_modvt_decode_layout(ss, sizeof ss, 0x1000, vf, sizeof vf, 0x8000, &l) == 1);
        CHECK(l.table_off == 0xA400u && l.sys_rva == 0x1000u + 0x40 + 7 + 0x0378A43Bu &&
              l.sync_rva == 0x1000u + 0x40 + 12 - 0x7547Au && l.release_rva == 0x1000u + 0x40 + 20 - 0x1892u);
        /* The per-view sites decode to the handles, record 1 and the feedback function. */
        {
            sh_modvt_view v;
            static unsigned char jb[0x40];
            CHECK(sh_modvt_decode_view(pvk, sizeof pvk, 0x17B4578u, fvk, sizeof fvk, 0x17B4643u, &v) == 1 &&
                  v.pagetable_src_rva == 0x66E1180u && v.pagetable_handle_rva == 0x3844FB8u &&
                  v.feedback_handle_rva == 0x38450D8u && v.feedback_rec_rva == 0x66E0D98u &&
                  v.feedback_fn_rva == 0x1859780u);
            /* Read from another record than the one whose page table it sets: refused. */
            CHECK(sh_modvt_decode_view(pvk, sizeof pvk, 0x17B4570u, fvk, sizeof fvk, 0x17B4643u, &v) == 0);
            memset(jb, 0x90, sizeof jb);
            CHECK(sh_modvt_decode_jobs(jb, sizeof jb, 0x31BF50u) == 0);
            memcpy(jb + 0x2E, "\x48\x8D\x0D\xBB\x60\x52\x05\xE8", 8);
            CHECK(sh_modvt_decode_jobs(jb, sizeof jb, 0x31BF50u) == 0x5842040u);
        }
        /* EndFrame's VT frame-update site (VK 0x1558A79) and the update's prologue. */
        {
            static const unsigned char fcall[] = { 0x44, 0x0F, 0xB6, 0xC5, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8D,
                                                   0x0D, 0xB6, 0x7E, 0x18, 0x05, 0xE8, 0x51, 0xF0, 0x28, 0x00 };
            static const unsigned char head[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
                                                  0x83, 0xEC, 0x50 };
            uint32_t fsys = 0, fupd = 0;
            CHECK(sh_modvt_decode_frame_call(fcall, sizeof fcall, 0x1558A79u, &fsys, &fupd) == 1 && fsys == 0x66E0940u &&
                  fupd == 0x17E7AE0u);
            CHECK(sh_modvt_decode_frame_call(fcall + 1, sizeof fcall - 1, 0x1558A7Au, &fsys, &fupd) == 0);
            CHECK(sh_modvt_frame_head_ok(head, sizeof head) && !sh_modvt_frame_head_ok(head, 14) &&
                  !sh_modvt_frame_head_ok(fcall, sizeof fcall));
        }
        /* Binder: CALL at +0x4E and +0x66, the texture setter after them. */
        memset(bd, 0x90, sizeof bd);
        CHECK(sh_modvt_decode_binder(bd, sizeof bd, 0x2000, &b) == 0);
        memcpy(bd + 0x4E, "\xE8\x10\x00\x00\x00", 5);
        memcpy(bd + 0x66, "\xE8\x20\x00\x00\x00", 5);
        CHECK(sh_modvt_decode_binder(bd, sizeof bd, 0x2000, &b) == 0);
        memcpy(bd + 0xA8, "\x48\x8B\x15\x00\x00\x00\x00\x48\x8B\xCE\xE8\x30\x00\x00\x00", 15);
        CHECK(sh_modvt_decode_binder(bd, sizeof bd, 0x2000, &b) == 1 && b.feedback_rva == 0x2000u + 0x53 + 0x10 &&
              b.set_vec4_rva == 0x2000u + 0x6B + 0x20 && b.set_texture_rva == 0x2000u + 0xB7 + 0x30);
        /* Rebind: the five fixed sites. */
        memset(rb, 0x90, sizeof rb);
        CHECK(sh_modvt_decode_rebind(rb, sizeof rb, 0x3000, &r) == 0);
        memcpy(rb + 0x16, "\x48\x8D\x0D\x00\x01\x00\x00", 7);
        memcpy(rb + 0x1D, "\xE8\x00\x02\x00\x00", 5);
        memcpy(rb + 0x45, "\x83\x3D\x00\x03\x00\x00\x00", 7);
        memcpy(rb + 0x55, "\xE8\x16\x00\x00\x00", 5);
        memcpy(rb + 0x62, "\xE9\x00\xF0\xFF\xFF", 5);
        CHECK(sh_modvt_decode_rebind(rb, sizeof rb, 0x3000, &r) == 1 && r.sys_rva == 0x3000u + 0x1D + 0x100 &&
              r.vf_rva == 0x3000u + 0x22 + 0x200 && r.vt_off_rva == 0x3000u + 0x4C + 0x300 &&
              r.binder_rva == 0x3000u + 0x5A + 0x16 && r.copy_rva == 0x3000u + 0x67 - 0x1000);
    }
}

static void families(void)
{
    char root[MAX_PATH], mods[MAX_PATH], a[MAX_PATH], b[MAX_PATH], c[MAX_PATH], cause[HO_TEXT];
    char hash_a[65], hash_a2[65];
    sh_modvt_family *fams = NULL;
    sh_modvt_result res;
    static fake f;
    int n, k, rec_fam, rec_other;

    printf("-- boards from the serving index, registration and release with a fake engine\n");
    CHECK(mt_temp_dir("modvt", root, sizeof root));
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\vtmod000", mods);
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\plain000", mods);
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\badvt000", root);
    CreateDirectoryA(mods, NULL);

    /* Builder refusals: a misplaced or misnamed member. */
    put_mod(c, "badvt000");
    mt_write(c, "assets\\virtualtextures\\mods\\badvt000\\fam.mega2", "MEGA");
    CHECK(strstr(build_cause(c, cause, sizeof cause), "file type not allowed") != NULL);
    mt_delete(c, "assets\\virtualtextures\\mods\\badvt000\\fam.mega2");
    mt_write(c, "assets\\virtualtextures\\mods\\other\\fam_sq1.mega2", "MEGA");
    CHECK(strstr(build_cause(c, cause, sizeof cause), "is under mods/other/, which is neither this mod folder") != NULL || (printf("  cause: %s\n", cause), 0));
    mt_delete(c, "assets\\virtualtextures\\mods\\other\\fam_sq1.mega2");
    mt_write(c, "assets\\virtualtextures\\mods\\badvt000\\mods\\badvt000\\fam_sq1.mega2", "MEGA");
    CHECK(strstr(build_cause(c, cause, sizeof cause), "virtual-texture family member") == NULL);   /* nested board name */
    mt_remove_tree(c);

    /* vtmod000: two boards (fam: members 1 and 3; other: member 2). plain000: none. */
    put_mod(a, "vtmod000");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod000\\fam_sq1.mega2", "member one");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod000\\fam_sq3.mega2", "member three");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod000\\other_sq2.mega2", "other two");
    put_mod(b, "plain000");
    mt_write(b, "assets\\maps\\mods\\plain000\\x.bin", "plain000");
    CHECK(build_folder(a, hash_a));
    CHECK(build_folder(b, NULL));

    sh_modserve_set_index_listener(listener);
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(g_listener_calls == 1);
    n = sh_modvt_collect(&fams);
    CHECK(n == 2);
    CHECK(n == 2 && !strcmp(fams[0].name, "mods/vtmod000/fam") && fams[0].members == 0x5u && fams[0].bytes == 22 &&
          !strcmp(fams[0].path, "virtualtextures/mods/vtmod000/fam.pages") && !strcmp(fams[0].bundle, "vtmod000") &&
          !strcmp(fams[0].bundle_hash, hash_a));
    CHECK(n == 2 && !strcmp(fams[1].name, "mods/vtmod000/other") && fams[1].members == 0x2u);
    free(fams);
    CHECK(sh_modvt_collect(NULL) == 2);

    /* Not up yet: nothing is called. */
    fake_init(&f);
    f.ready = 0;
    sh_modvt_reset();
    CHECK(sync_index(&f, "boot", 0, &res) == -1 && res.not_ready && f.sets == 0);

    /* Up: one record per board in the first free records, each on the material-format cache. */
    f.ready = 1;
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", 0, &res) == 2 && res.registered == 2 && f.sets == 2 && f.adopts == 2);
    rec_fam = f_find(&f, "mods/vtmod000/fam");
    rec_other = f_find(&f, "mods/vtmod000/other");
    CHECK(rec_fam == 4 && rec_other == 5);
    CHECK(!strcmp(f.path[4], "virtualtextures/mods/vtmod000/fam.pages"));
    CHECK(get_ptr(f.rec[4] + SH_VT_REC_CACHE) == (void *)f.cache_vmtr && get_ptr(f.rec[5] + SH_VT_REC_CACHE) == (void *)f.cache_vmtr);
    CHECK(sh_modvt_record_owner(4) && !strcmp(sh_modvt_record_owner(4), "mods/vtmod000/fam"));
    CHECK(sh_modvt_record_owner(3) == NULL && sh_modvt_owned_count() == 2);
    CHECK(log_count("registered board 'mods/vtmod000/fam' (vtmod000, 2 members, 22 bytes) as record 4") == 1);

    /* Rescan: idempotent. */
    CHECK(sh_modserve_index_build(mods) == 2 && g_listener_calls == 2);
    CHECK(sync_index(&f, "frame", 1, &res) == 2 && res.kept == 2 && res.registered == 0 && f.sets == 2 && f.releases == 0);
    CHECK(sync_index(&f, "editor", 0, &res) == 2 && f.sets == 2);

    /* A changed build of the same mod: at a pass the release waits for the
     * frame boundary (the old board keeps drawing meanwhile); there it is
     * released and the new build registered in the same record. */
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod000\\fam_sq1.mega2", "member one, rebaked");
    CHECK(build_folder(a, hash_a2) && strcmp(hash_a, hash_a2) != 0);
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(sync_index(&f, "map", 0, &res) == 2 && res.pending == 2 && res.released == 0 && res.registered == 0 &&
          f.releases == 0 && f.sets == 2);
    CHECK(sync_index(&f, "frame", 1, &res) == 2 && res.released == 2 && res.registered == 2 && f.releases == 2 && f.sets == 4);
    CHECK(f_find(&f, "mods/vtmod000/fam") == 4);

    /* No longer served: released at the frame boundary only. */
    mt_remove_tree(a);
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sh_modvt_collect(NULL) == 0);
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", 0, &res) == 2 && res.pending == 2 && f.releases == 2);
    CHECK(sync_index(&f, "frame", 1, &res) == 0 && res.released == 2 && f.releases == 4);
    CHECK(f_find(&f, "mods/vtmod000/fam") < 0 && f_is_free(&f, 4) && f_is_free(&f, 5));
    CHECK(log_count("released record 4, board 'mods/vtmod000/fam'") == 1);

    /* The engine took the record back: registered again in the same record. */
    put_mod(a, "vtmod000");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod000\\fam_sq1.mega2", "member one");
    CHECK(build_folder(a, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(sync_index(&f, "editor", 0, &res) == 1 && res.registered == 1);
    k = f_find(&f, "mods/vtmod000/fam");
    CHECK(k == 4);
    f.name[k][0] = 0;
    f.path[k][0] = 0;
    strcpy_s(f.name[6], sizeof f.name[6], "maps/x/y");    /* something else took the next free record */
    strcpy_s(f.path[6], sizeof f.path[6], "virtualtextures/maps/x/y.pages");
    CHECK(sync_index(&f, "map", 0, &res) == 1 && res.lost == 1 && res.registered == 1 && f_find(&f, "mods/vtmod000/fam") == 4);
    f.name[6][0] = f.path[6][0] = 0;

    /* A name another record already holds is never taken over or released. */
    sh_modvt_reset();
    fake_init(&f);
    strcpy_s(f.name[7], sizeof f.name[7], "mods/vtmod000/fam");
    strcpy_s(f.path[7], sizeof f.path[7], "virtualtextures/mods/vtmod000/fam.pages");
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", 0, &res) == 0 && res.refused == 1 && f.sets == 0);
    CHECK(sync_index(&f, "map", 0, &res) == 0 && log_count("already holds that name") == 1);
    mt_remove_tree(a);
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sync_index(&f, "frame", 1, &res) == 0 && f.releases == 0 && f_find(&f, "mods/vtmod000/fam") == 7);

    /* Records 1..30 full: refused once, one player notice; no stock record and not record 31 touched. */
    put_mod(a, "vtmod000");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod000\\fam_sq1.mega2", "member one");
    CHECK(build_folder(a, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    fake_init(&f);
    for (k = 4; k <= 30; ++k) {
        _snprintf_s(f.name[k], sizeof f.name[k], _TRUNCATE, "maps/stock/%d", k);
        _snprintf_s(f.path[k], sizeof f.path[k], _TRUNCATE, "virtualtextures/maps/stock/%d.pages", k);
    }
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", 0, &res) == 0 && res.full == 1 && f.sets == 0 && f.releases == 0);
    CHECK(sync_index(&f, "map", 0, &res) == 0 && res.full == 1 && log_count("are all in use") == 1);
    CHECK(f.notices == 1 && strstr(f.notice, "no room for another mod texture board") && strstr(f.notice, "vtmod000") &&
          strlen(f.notice) < 256);
    printf("    notice: %s\n", f.notice);
    CHECK(f_is_free(&f, 31));
    /* A slot frees up: the board registers in it. */
    f.name[17][0] = f.path[17][0] = 0;
    CHECK(sync_index(&f, "map", 0, &res) == 1 && f_find(&f, "mods/vtmod000/fam") == 17);

    /* The material-format cache cannot be adopted: released at once, refused. */
    sh_modvt_reset();
    fake_init(&f);
    f.refuse_adopt = 1;
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", 0, &res) == 0 && res.refused == 1 && f.sets == 1 && f.releases == 1 &&
          f_find(&f, "mods/vtmod000/fam") < 0 && log_count("material-format page cache") == 1);

    /* SetSource refused (no member opened): logged once, retried at later syncs. */
    sh_modvt_reset();
    fake_init(&f);
    f.refuse_set = 1;
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", 0, &res) == 0 && res.refused == 1 && f.sets == 1);
    CHECK(sync_index(&f, "map", 0, &res) == 0 && f.sets == 2 && log_count("SetSource on record 4 refused") == 1);

    sh_modserve_set_index_listener(NULL);
    sh_modvt_reset();
    mt_remove_tree(root);
}

/* The detours with the fake engine: the lookup, the binder addition, a stock
 * material untouched, a board's materials bound again when it returns. */
static void detours(void)
{
    char root[MAX_PATH], mods[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    static fake f;
    static fake_material stock_before, stock_after, map_before, body, wounds, other, m;
    static uint8_t rec1_before[REC_BYTES], rec3_before[REC_BYTES];
    sh_modvt_result res;
    float w[4];
    int kt, ks;

    printf("-- the lookup and binder detours with a fake engine\n");
    CHECK(mt_temp_dir("modvt_bind", root, sizeof root));
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\tyrant00", mods);
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\second00", mods);
    CreateDirectoryA(mods, NULL);
    put_mod(a, "tyrant00");
    mt_write(a, "assets\\virtualtextures\\mods\\tyrant00\\tyrant_sq1.mega2", "tyrant square one");
    put_mod(b, "second00");
    mt_write(b, "assets\\virtualtextures\\mods\\second00\\skin_sq2.mega2", "second square two");
    CHECK(build_folder(a, NULL) && build_folder(b, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    sh_modvt_reset();
    fake_init(&f);

    /* Before any board: a stock material (the game's board) and a map module material. */
    fake_parse(&f, &stock_before, "_vmtr");
    fake_parse(&f, &map_before, "maps/modules/palettes/mega_blessed/mega");
    CHECK(sh_modvt_board_record("mods/tyrant00/tyrant") == NULL);
    memcpy(rec1_before, f.rec[1], REC_BYTES);
    memcpy(rec3_before, f.rec[3], REC_BYTES);

    /* Two boards at once. */
    CHECK(sync_index(&f, "editor", 0, &res) == 2 && res.registered == 2);
    kt = f_find(&f, "mods/tyrant00/tyrant");
    ks = f_find(&f, "mods/second00/skin");
    CHECK(kt >= 4 && kt <= 5 && ks >= 4 && ks <= 5 && kt != ks);
    if (kt < 0 || ks < 0) { mt_remove_tree(root); return; }
    CHECK(memcmp(rec1_before, f.rec[1], REC_BYTES) == 0 && memcmp(rec3_before, f.rec[3], REC_BYTES) == 0);

    /* The lookup returns a board only for its own name. */
    CHECK(sh_modvt_board_record("mods/tyrant00/tyrant") == (void *)f.rec[kt]);
    CHECK(sh_modvt_board_record("mods/second00/skin") == (void *)f.rec[ks]);
    CHECK(sh_modvt_board_record("_vmtr") == NULL && sh_modvt_board_record("mods/tyrant00/other") == NULL &&
          sh_modvt_board_record("") == NULL && sh_modvt_board_record(NULL) == NULL);

    /* A stock material bound with two boards registered: byte-identical. */
    fake_parse(&f, &stock_after, "_vmtr");
    CHECK(memcmp(&stock_before, &stock_after, sizeof stock_before) == 0);
    fake_parse(&f, &stock_after, "maps/modules/palettes/mega_blessed/mega");
    CHECK(memcmp(&map_before, &stock_after, sizeof map_before) == 0);
    CHECK(sh_modvt_bind_board(stock_after.parms, f.rec[1], &g_bops) == 0 &&
          memcmp(&map_before, &stock_after, sizeof map_before) == 0);

    /* A board material: the engine's own writes, plus the two the lit programs read. */
    fake_parse(&f, &body, "mods/tyrant00/tyrant");
    fake_parse(&f, &wounds, "mods/tyrant00/tyrant");
    fake_parse(&f, &other, "mods/second00/skin");
    CHECK(body.record == kt && other.record == ks);
    CHECK(memcmp(body.parms[H_FEEDBACK_U], body.parms[H_VMTR_FEEDBACK], 16) == 0 &&
          ((float *)body.parms[H_VMTR_FEEDBACK])[0] == (float)kt && ((float *)other.parms[H_VMTR_FEEDBACK])[0] == (float)ks);
    CHECK(get_ptr(body.parms[H_VMTR_PAGETABLE]) == (void *)f.ptimage[kt] &&
          get_ptr(other.parms[H_VMTR_PAGETABLE]) == (void *)f.ptimage[ks]);
    CHECK(get_ptr(body.parms[H_PHYS_U]) == (void *)f.cache_vmtr);   /* the material-format cache */
    CHECK(sh_modvt_material_count("mods/tyrant00/tyrant") == 2 && sh_modvt_material_count("mods/second00/skin") == 1);
    /* Missing handles: nothing added. */
    {
        sh_modvt_bind_ops none = g_bops;
        none.pagetable_handle = NULL;
        memset(&m, 0, sizeof m);
        CHECK(sh_modvt_bind_board(m.parms, f.rec[kt], &none) == 0 && get_ptr(m.parms[H_VMTR_PAGETABLE]) == NULL);
    }

    /* The map stops serving the Tyrant: released at the frame boundary; the
     * second board stays. Its materials are remembered. */
    mt_remove_tree(a);
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sync_index(&f, "map", 0, &res) == 2 && res.pending == 1);
    CHECK(sync_index(&f, "frame", 1, &res) == 1 && res.released == 1 && f_find(&f, "mods/tyrant00/tyrant") < 0);
    CHECK(sh_modvt_board_record("mods/tyrant00/tyrant") == NULL &&
          sh_modvt_board_record("mods/second00/skin") == (void *)f.rec[ks]);
    CHECK(sh_modvt_bind_board(m.parms, f.rec[kt], &g_bops) == 0);

    /* Served again: the same record, and both earlier materials bound again
     * (index, page table, feedback) before the pass parses anything. */
    put_mod(a, "tyrant00");
    mt_write(a, "assets\\virtualtextures\\mods\\tyrant00\\tyrant_sq1.mega2", "tyrant square one");
    CHECK(build_folder(a, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    memset(body.parms, 0, sizeof body.parms);
    f.rebinds = 0;
    g_nlog = 0;
    CHECK(sync_index(&f, "map", 0, &res) == 2 && res.registered == 1 && res.rebound == 2 && f.rebinds == 2);
    CHECK(f_find(&f, "mods/tyrant00/tyrant") == kt);
    CHECK(body.record == kt && get_ptr(body.parms[H_VMTR_PAGETABLE]) == (void *)f.ptimage[kt] &&
          ((float *)body.parms[H_VMTR_FEEDBACK])[0] == (float)kt);
    CHECK(log_count("2 materials of board 'mods/tyrant00/tyrant' bound again") == 1);
    b_feedback(f.rec[kt], w);
    CHECK(memcmp(w, body.parms[H_VMTR_FEEDBACK], 16) == 0);
    /* And the stock material and record 1 are still untouched. */
    fake_parse(&f, &stock_after, "_vmtr");
    CHECK(memcmp(&stock_before, &stock_after, sizeof stock_before) == 0 && memcmp(rec1_before, f.rec[1], REC_BYTES) == 0);

    sh_modvt_reset();
    mt_remove_tree(root);
}

/* ------------------------------------------------------------------ executables */

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = NULL;
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].SizeOfRawData && sec[i].VirtualAddress + sec[i].SizeOfRawData <= nt->OptionalHeader.SizeOfImage &&
            sec[i].PointerToRawData + sec[i].SizeOfRawData <= (uint32_t)fsz)
            memcpy(img + sec[i].VirtualAddress, file + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    *image_sz = nt->OptionalHeader.SizeOfImage;
    free(file);
    return img;
}

/* A RIP-relative LEA in code[0..len) (at rva) whose target is `target`. */
static int lea_to(const uint8_t *code, size_t len, uint32_t rva, uint32_t target)
{
    size_t i;
    int32_t d;
    for (i = 0; i + 7 <= len; ++i) {
        if ((code[i] != 0x48 && code[i] != 0x4C) || code[i + 1] != 0x8D || (code[i + 2] & 0xC7) != 0x05) continue;
        memcpy(&d, code + i + 3, 4);
        if ((uint32_t)((int64_t)rva + (int64_t)i + 7 + d) == target) return 1;
    }
    return 0;
}

static const sig_result *find_sig(sig_result *results, size_t total, const char *name)
{
    size_t i;
    for (i = 0; i < total; ++i)
        if (!strcmp(results[i].name, name)) return results[i].status == SIG_OK ? &results[i] : NULL;
    return NULL;
}

static void image(const char *path)
{
    size_t sz = 0, total;
    uint8_t *base = map_pe_by_rva(path, &sz);
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    static sig_result results[SIG_RESULTS_MAX];
    const sig_result *ss, *vf, *bd, *rb, *vp, *vb, *jw, *fc;
    uint32_t fsys = 0, fupd = 0;
    sh_modvt_layout l;
    sh_modvt_binder b;
    sh_modvt_rebind r;
    sh_modvt_view v;
    uint32_t jobs;
    int vk;
    printf("-- %s\n", path);
    CHECK(base != NULL);
    if (!base) return;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    if (!id) { free(base); return; }
    vk = id->vulkan;
    total = sig_db_count();
    sig_resolve_all(base, results, SIG_RESULTS_MAX);
    ss = find_sig(results, total, "VtSetSource");
    vf = find_sig(results, total, "VtForMaterial");
    bd = find_sig(results, total, "VtMaterialBind");
    rb = find_sig(results, total, "VtMaterialRebind");
    vp = find_sig(results, total, "VtViewPageTable");
    vb = find_sig(results, total, "VtViewFeedback");
    jw = find_sig(results, total, "JobsWaitAll");
    fc = find_sig(results, total, "VtFrameCall");
    CHECK(ss && vf && bd && rb && vp && vb && jw && fc);
    if (!ss || !vf || !bd || !rb || !vp || !vb || !jw || !fc) { free(base); return; }
    CHECK(fc->rva == (vk ? 0x1558A79u : 0x1559E29u));
    CHECK(sh_modvt_decode_frame_call(base + fc->rva, 0x20, fc->rva, &fsys, &fupd) == 1 &&
          fupd == (vk ? 0x17E7AE0u : 0x17E89B0u) && fupd + 0x10 <= sz && sh_modvt_frame_head_ok(base + fupd, 0x10));
    CHECK(vp->rva == (vk ? 0x17B4578u : 0x17B5808u) && vb->rva == (vk ? 0x17B4643u : 0x17B58D3u) &&
          jw->rva == (vk ? 0x31BF50u : 0x31BAE0u));
    CHECK(ss->rva == (vk ? 0x185AC30u : 0x185BC90u));
    CHECK(vf->rva == (vk ? 0x17E8230u : 0x17E9100u));
    CHECK(bd->rva == (vk ? 0x16CCCF0u : 0x16CD9A0u));
    CHECK(rb->rva == (vk ? 0x16CCC80u : 0x16CD930u));
    CHECK(sh_modvt_decode_layout(base + ss->rva, 0xC0, ss->rva, base + vf->rva, 0xC0, vf->rva, &l) == 1);
    CHECK(sh_modvt_decode_view(base + vp->rva, 0x20, vp->rva, base + vb->rva, 0x20, vb->rva, &v) == 1);
    jobs = sh_modvt_decode_jobs(base + jw->rva, 0x40, jw->rva);
    CHECK(l.release_rva + 0x50 <= sz && sh_modvt_release_head_ok(base + l.release_rva, 0x50));
    CHECK(sh_modvt_decode_binder(base + bd->rva, 0xC0, bd->rva, &b) == 1);
    CHECK(sh_modvt_decode_rebind(base + rb->rva, 0x70, rb->rva, &r) == 1);
    printf("    %s: VT system 0x%X, sync 0x%X, Release 0x%X, table sys+0x%X; binder feedback 0x%X, vec4 0x%X, "
           "texture 0x%X; rebind copy 0x%X, VT off 0x%X; view handles 0x%X 0x%X, record 1 0x%X; jobs 0x%X\n",
           vk ? "Vulkan" : "OpenGL", l.sys_rva, l.sync_rva, l.release_rva, l.table_off, b.feedback_rva, b.set_vec4_rva,
           b.set_texture_rva, r.copy_rva, r.vt_off_rva, v.pagetable_handle_rva, v.feedback_handle_rva,
           v.feedback_rec_rva, jobs);
    /* The cross-checks the install makes. */
    CHECK(r.sys_rva == l.sys_rva && r.vf_rva == vf->rva && r.binder_rva == bd->rva && v.feedback_fn_rva == b.feedback_rva &&
          fsys == l.sys_rva);
    if (vk) {
        CHECK(l.sys_rva == 0x66E0940u && l.sync_rva == 0x17E59F0u && l.release_rva == 0x1859480u && l.table_off == 0x8B00u);
        CHECK(b.feedback_rva == 0x1859780u && b.set_vec4_rva == 0x15B9560u && b.set_texture_rva == 0x15B93C0u &&
              r.copy_rva == 0x16C9BD0u && r.vt_off_rva == 0x5D490A0u);
        CHECK(v.pagetable_handle_rva == 0x3844FB8u && v.feedback_handle_rva == 0x38450D8u &&
              v.feedback_rec_rva == 0x66E0D98u && jobs == 0x5842040u);
    } else {
        CHECK(l.sys_rva == 0x4FE6120u && l.release_rva == 0x185A460u && l.table_off == 0xA400u);
        CHECK(b.feedback_rva == 0x185A7E0u && b.set_vec4_rva == 0x15BA410u && b.set_texture_rva == 0x15BA270u &&
              r.copy_rva == 0x16CA880u && r.vt_off_rva == 0x464E720u);
        CHECK(v.pagetable_handle_rva == 0x388CBD8u && v.feedback_handle_rva == 0x388CCF8u &&
              v.feedback_rec_rva == 0x4FE6640u && jobs != 0);
    }
    /* Record 1 is the record the table's first slot holds: VK sys+0x458, GL sys+0x520 (the strides). */
    CHECK(v.feedback_rec_rva == l.sys_rva + (vk ? 0x458u : 0x520u));
    /* SetSource gives flavor 0 the material-format cache and flavor 2 UniqueVmtrs
     * (VK sys+0xFCA0 / +0xFEB0, GL sys+0x115A0 / +0x11870): record 1's cache,
     * which a board adopts, is the first. */
    CHECK(ss->rva + 0x1000 <= sz && lea_to(base + ss->rva, 0x1000, ss->rva, l.sys_rva + (vk ? 0xFCA0u : 0x115A0u)));
    CHECK(lea_to(base + ss->rva, 0x1000, ss->rva, l.sys_rva + (vk ? 0xFEB0u : 0x11870u)));
    /* The detours' stolen bytes end in the security-cookie load, at +11 and +10. */
    CHECK(!memcmp(base + vf->rva + 11, "\x48\x8B\x05", 3) && !memcmp(base + bd->rva + 10, "\x48\x8B\x05", 3));
    free(base);
}

/* One salt at the root and in a map's own folder: a stranger's map that
 * brought its copy pages the family from that copy, not the root folder's;
 * with no map open the root folder's serves. */
static void families_follow_the_map(void)
{
    char root[MAX_PATH], mods[MAX_PATH], own[MAX_PATH], placed[MAX_PATH], hash_own[65], hash_placed[65];
    const char *list[1];
    sh_modvt_family *fams = NULL;
    int n;
    printf("-- a board comes from the build the loaded map serves\n");
    CHECK(mt_temp_dir("modvt_place", root, sizeof root));
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(own, sizeof own, _TRUNCATE, "%s\\vtplace0", mods);
    _snprintf_s(placed, sizeof placed, _TRUNCATE, "%s\\maps\\Vt map\\vtplace0", mods);
    CreateDirectoryA(mods, NULL);
    put_mod(own, "vtplace0");
    mt_write(own, "assets\\virtualtextures\\mods\\vtplace0\\fam_sq1.mega2", "root one");
    put_mod(placed, "vtplace0");
    mt_write(placed, "assets\\virtualtextures\\mods\\vtplace0\\fam_sq1.mega2", "the map's own one");
    mt_write(placed, "assets\\virtualtextures\\mods\\vtplace0\\fam_sq2.mega2", "the map's two");
    CHECK(build_folder(own, hash_own) && build_folder(placed, hash_placed));
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(mods) == 2);
    n = sh_modvt_collect(&fams);
    CHECK(n == 1 && !strcmp(fams[0].bundle_hash, hash_own) && fams[0].members == 0x1u);
    free(fams);
    CHECK(sh_modserve_place_set("maps\\Vt map") >= 0);
    list[0] = hash_placed;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    n = sh_modvt_collect(&fams);
    CHECK(n == 1 && !strcmp(fams[0].bundle_hash, hash_placed) && fams[0].members == 0x3u &&
          fams[0].bytes == strlen("the map's own one") + strlen("the map's two"));
    free(fams);
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    CHECK(sh_modserve_place_set(NULL) >= 0);
    sh_modserve_scope_enable(0);
    mt_remove_tree(root);
}

int main(int argc, char **argv)
{
    int i;
    g_echo_log = GetEnvironmentVariableA("MODVT_ECHO", NULL, 0) != 0;
    member_rule();
    decode_helpers();
    families();
    detours();
    families_follow_the_map();
    if (argc < 2) printf("[skip] the executables (pass DOOMx64vk.exe and DOOMx64.exe)\n");
    for (i = 1; i < argc; ++i) image(argv[i]);
    printf("modvt_test: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
