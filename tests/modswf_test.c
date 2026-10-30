/* modswf_test.c -- the cooked-movie route (modswf.c).
 *
 * Offline: the movie-request predicate, and the replacement Load driven
 * against synthetic engine functions (a served movie takes the engine's
 * below-2 path; everything else, and a failed open, goes to the original).
 * The builder admits .bswf under generated/swf/ only, names it swf/<stem>.swf,
 * and applies the shadow and mods/<id>/ rules to it.
 * With build 13954591 executables as arguments: FileResourceLoad resolves
 * uniquely, its four calls decode to the recorded functions, the open-into
 * helper opens through +0x148, and exactly one read-only qword holds the
 * Load (the idFileResource vtable +0x40). Run via run-tests.ps1. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include "modswf.h"
#include "modbundle.h"
#include "signatures.h"
#include "host_image.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* ------------------------------------------------------------ synthetic engine */

typedef struct fake_file { void **vtbl; const char *name; int destroyed; } fake_file;
typedef struct fake_resource { void *vtbl; const char *name; char pad[0x28]; fake_file *file; } fake_resource;

static int n_alloc, n_ctor, n_open, n_orig, n_dtor, fail_alloc;
static uint8_t open_result;
static void *seen_arg;
static fake_resource *seen_res;
static const char *served_name;

static void fake_dtor(void *self, int flags) { (void)flags; ((fake_file *)self)->destroyed = 1; n_dtor++; }
static void *fake_vtbl[1] = { (void *)fake_dtor };
static void *fake_alloc(size_t size) { n_alloc++; return (fail_alloc || size != 0x180) ? NULL : calloc(1, size); }
static void *fake_ctor(void *mem, const char *name)
{
    fake_file *f = (fake_file *)mem;
    n_ctor++;
    f->vtbl = fake_vtbl;
    f->name = name;
    return f;
}
static uint8_t fake_open_into(void *res, void **slot, const char *name, char must_exist, void *arg)
{
    fake_resource *r = (fake_resource *)res;
    n_open++;
    CHECK(slot == (void **)&r->file);
    CHECK(r->file && r->file->name == name && r->file->vtbl == fake_vtbl);
    CHECK(must_exist == 1);
    seen_arg = arg;
    seen_res = r;
    return open_result;
}
static int fake_state(void) { return 3; }
static int64_t fake_original(void *res, void *arg) { (void)res; (void)arg; n_orig++; return 7; }
static int n_lookup;
static int fake_lookup(const char *request, const char **bundle)
{
    n_lookup++;
    if (bundle) *bundle = "alex.screens";
    return served_name && _stricmp(request, served_name) == 0;
}

static void reset(void) { n_alloc = n_ctor = n_open = n_orig = n_dtor = fail_alloc = n_lookup = 0; open_result = 0; seen_arg = NULL; seen_res = NULL; }

static void test_predicate(void)
{
    CHECK(sh_modswf_is_movie("generated/swf/screens_tech/vega_flat.bswf"));
    CHECK(sh_modswf_is_movie("Generated\\SWF\\mods\\alex.screens\\a.BSWF"));
    CHECK(sh_modswf_is_movie("generated/swf/x.bswf"));
    CHECK(!sh_modswf_is_movie("generated/swf/.bswf"));
    CHECK(!sh_modswf_is_movie("generated/swf/screens_tech/vega_flat.bimage"));
    CHECK(!sh_modswf_is_movie("swf/screens_tech/vega_flat.swf"));
    CHECK(!sh_modswf_is_movie("generated/decls/weapon/x.decl"));
    CHECK(!sh_modswf_is_movie("generated/swfx/a.bswf"));
    CHECK(!sh_modswf_is_movie(NULL));
    /* A module's per-map files take the same route. */
    CHECK(sh_modswf_is_module_file("maps/modules/mods/alex.mp-basin/basin/basin.sbsp"));
    CHECK(sh_modswf_is_module_file("maps/modules/mods/alex.mp-basin/basin/basin.ambientsh"));
    CHECK(sh_modswf_is_module_file("Maps\\Modules\\Mods\\a\\b\\_combo\\_world.TOME"));
    CHECK(sh_modswf_is_module_file("maps/modules/mods/a/b/lightprobes/lightprobe_light_probe_10_compressed.bimage"));
    CHECK(!sh_modswf_is_module_file("generated/image/mods/a/lightprobes.bimage"));
    CHECK(!sh_modswf_is_module_file("maps/modules/mods/a/b/_combo/world.bcm"));
    CHECK(!sh_modswf_is_module_file("maps/modules/mods/a/b/.sbsp"));
    CHECK(!sh_modswf_is_module_file("maps/modules/mods/a/b.sbsp/x.decl"));
    CHECK(!sh_modswf_is_module_file(NULL));
}

static void test_routing(void)
{
    sh_modswf_engine e;
    fake_resource res;
    fake_file stale = { fake_vtbl, "stale", 0 };
    int64_t r;
    e.state = (uintptr_t)fake_state;
    e.alloc = (uintptr_t)fake_alloc;
    e.named_ctor = (uintptr_t)fake_ctor;
    e.open_into = (uintptr_t)fake_open_into;
    sh_modswf_test_bind(&e, fake_original, fake_lookup);

    /* A served movie: the engine's below-2 sequence, the previous file released. */
    reset();
    memset(&res, 0, sizeof res);
    res.name = "generated/swf/screens_tech/vega_flat.bswf";
    res.file = &stale;
    served_name = res.name;
    r = sh_modswf_test_load(&res, (void *)0x1234);
    CHECK(r == 0 && n_alloc == 1 && n_ctor == 1 && n_open == 1 && n_orig == 0);
    CHECK(stale.destroyed == 1 && n_dtor == 1 && res.file && res.file != &stale);
    CHECK(seen_arg == (void *)0x1234 && seen_res == &res);
    free(res.file);

    /* An unserved movie: the original Load, no allocation. */
    reset();
    memset(&res, 0, sizeof res);
    res.name = "generated/swf/hud/hud_main.bswf";
    r = sh_modswf_test_load(&res, NULL);
    CHECK(r == 7 && n_orig == 1 && n_alloc == 0 && n_lookup == 1);

    /* A served name that is not a movie never reaches the lookup. */
    reset();
    res.name = "generated/decls/weapon/weapon/zion/player/sp/pistol.decl";
    served_name = res.name;
    r = sh_modswf_test_load(&res, NULL);
    CHECK(r == 7 && n_orig == 1 && n_lookup == 0 && n_alloc == 0);

    /* A served module file (a stock pointer in the bundle) takes the movie path. */
    reset();
    memset(&res, 0, sizeof res);
    res.name = "maps/modules/mods/alex.mp-basin/basin/_combo/_world.tome";
    served_name = res.name;
    r = sh_modswf_test_load(&res, NULL);
    CHECK(r == 0 && n_alloc == 1 && n_ctor == 1 && n_open == 1 && n_orig == 0 && n_lookup == 1);
    free(res.file);

    /* An unserved one is the engine's. */
    reset();
    memset(&res, 0, sizeof res);
    res.name = "maps/modules/classic/classic_blank_room/classic_blank_room.sbsp";
    r = sh_modswf_test_load(&res, NULL);
    CHECK(r == 7 && n_orig == 1 && n_alloc == 0 && n_lookup == 1);

    /* The open fails: the original Load replaces what was put at +0x38. */
    reset();
    memset(&res, 0, sizeof res);
    res.name = "generated/swf/mods/alex.screens/panel.bswf";
    served_name = res.name;
    open_result = 2;
    r = sh_modswf_test_load(&res, NULL);
    CHECK(r == 7 && n_open == 1 && n_orig == 1);
    free(res.file);

    /* The engine allocation fails: the original Load, +0x38 untouched. */
    reset();
    memset(&res, 0, sizeof res);
    res.file = &stale;
    stale.destroyed = 0;
    res.name = "generated/swf/mods/alex.screens/panel.bswf";
    fail_alloc = 1;
    r = sh_modswf_test_load(&res, NULL);
    CHECK(r == 7 && n_orig == 1 && n_open == 0 && res.file == &stale && !stale.destroyed);

    /* An unreadable resource name. */
    reset();
    r = sh_modswf_test_load(NULL, NULL);
    CHECK(r == 7 && n_orig == 1);
}

/* ------------------------------------------------------------ builder */

static void logical(const char *request, const char *type, const char *name)
{
    char t[64], n[512];
    int ok = sh_mb_logical_name(request, t, sizeof t, n, sizeof n);
    CHECK(ok && !strcmp(t, type) && !strcmp(n, name));
    if (!ok || strcmp(t, type) || strcmp(n, name)) printf("  %s -> %s %s\n", request, ok ? t : "?", ok ? n : "?");
}

static char g_dir[MAX_PATH];
static void put(const char *rel, const char *text)
{
    char path[MAX_PATH], dir[MAX_PATH];
    FILE *f;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_dir, rel);
    for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
    strcpy_s(dir, sizeof dir, path);
    *strrchr(dir, '\\') = 0;
    SHCreateDirectoryExA(NULL, dir, NULL);
    f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}
static void drop(const char *rel)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_dir, rel);
    for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
    DeleteFileA(path);
}
static int stock(void *ctx, const char *request)
{
    (void)ctx;
    return !strcmp(request, "generated/swf/screens_tech/vega_flat.bswf") ||
           !strcmp(request, "generated/swf/screens_tech/vega_flat.bimage");
}

static void test_builder(void)
{
    static const sh_mb_resolver resolver = { NULL, NULL, stock, NULL };
    static const char *const movies[] = {
        "assets/generated/swf/mods/alex.screens/panel.bswf", "assets/generated/swf/mods/alex.screens/panel.bimage",
        "assets/generated/swf/screens_tech/vega_flat.bswf", "assets/generated/swf/screens_tech/vega_flat.bimage", NULL };
    char tmp[MAX_PATH];
    sh_mb_bundle b;
    size_t i, swf = 0;

    CHECK(sh_mb_extension_allowed("generated/swf/screens_tech/vega_flat.bswf"));
    CHECK(sh_mb_extension_allowed("Generated/SWF/a.BSWF"));
    CHECK(!sh_mb_extension_allowed("generated/image/a.bswf"));
    CHECK(!sh_mb_extension_allowed("swf/a.bswf"));
    CHECK(!sh_mb_extension_allowed("generated/swf/a.swf"));
    CHECK(sh_mb_extension_allowed("generated/swf/screens_tech/vega_flat.bimage"));
    logical("generated/swf/screens_tech/vega_flat.bswf", "swf", "swf/screens_tech/vega_flat.swf");
    logical("generated/swf/mods/alex.screens/panel.bswf", "swf", "swf/mods/alex.screens/panel.swf");
    logical("generated/swf/x.bswf", "swf", "swf/x.swf");
    logical("generated/swf/screens_tech/vega_flat.bimage", "file", "generated/swf/screens_tech/vega_flat.bimage");

    /* A private movie under mods/<id>/ and a listed stock shadow build. */
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_dir, sizeof g_dir, _TRUNCATE, "%ssh_modswf_test_%lu\\alex.screens", tmp, GetCurrentProcessId());
    put("mod.json", "{\"id\": \"alex.screens\", \"version\": \"1.0.0\", \"name\": \"Screens\", "
                    "\"shadows\": [\"generated/swf/screens_tech/vega_flat.bswf\", "
                    "\"generated/swf/screens_tech/vega_flat.bimage\"]}");
    for (i = 0; movies[i]; i++) put(movies[i], "BSW movie bytes");
    CHECK(sh_mb_build(g_dir, &resolver, &b));
    if (b.errors) printf("  build: %s\n", b.first_cause);
    for (i = 0; i < b.file_count; i++) if (!strcmp(b.files[i].ptype, "swf")) swf++;
    CHECK(b.file_count == 4 && swf == 2 && b.shadow_count == 2);
    sh_mb_free(&b);

    /* A movie at a stock path that is not listed is refused by the namespace rule. */
    put("mod.json", "{\"id\": \"alex.screens\", \"version\": \"1.0.0\", \"name\": \"Screens\", "
                    "\"shadows\": [\"generated/swf/screens_tech/vega_flat.bimage\"]}");
    CHECK(!sh_mb_build(g_dir, &resolver, &b) && strstr(b.first_cause, "vega_flat.bswf"));
    sh_mb_free(&b);
    /* A .bswf outside generated/swf/ is not a carried type. */
    put("mod.json", "{\"id\": \"alex.screens\", \"version\": \"1.0.0\", \"name\": \"Screens\", "
                    "\"shadows\": [\"generated/swf/screens_tech/vega_flat.bswf\", "
                    "\"generated/swf/screens_tech/vega_flat.bimage\"]}");
    put("assets/generated/image/mods/alex.screens/stray.bswf", "x");
    CHECK(!sh_mb_build(g_dir, &resolver, &b) && strstr(b.first_cause, "file type not allowed"));
    sh_mb_free(&b);

    drop("assets/generated/image/mods/alex.screens/stray.bswf");
    for (i = 0; movies[i]; i++) drop(movies[i]);
    drop("mod.json");
    drop("bundle.json");
    drop("bundle.canonical.json");
    {
        static const char *const dirs[] = { "assets\\generated\\image\\mods\\alex.screens", "assets\\generated\\image\\mods",
            "assets\\generated\\image", "assets\\generated\\swf\\mods\\alex.screens", "assets\\generated\\swf\\mods",
            "assets\\generated\\swf\\screens_tech", "assets\\generated\\swf", "assets\\generated", "assets", "", NULL };
        char path[MAX_PATH];
        for (i = 0; dirs[i]; i++) { _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_dir, dirs[i]); RemoveDirectoryA(path); }
        *strrchr(g_dir, '\\') = 0;
        RemoveDirectoryA(g_dir);
    }
}

/* ------------------------------------------------------------ images */

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    if (!f) return NULL;
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
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz) memcpy(img + va, file + po, rs);
    }
    *image_sz = nt->OptionalHeader.SizeOfImage;
    free(file);
    return img;
}

typedef struct expect { uint32_t load, state, alloc, ctor, open, slot; } expect;
static const expect VK = { 0x172B3B0u, 0x282140u, 0x1B2BD1Cu, 0x29F950u, 0x16C66A0u, 0x2868BB8u };
static const expect GL = { 0x172C040u, 0x282360u, 0x1B4623Cu, 0x29FB50u, 0x16C7380u, 0x2882A18u };

static void test_image(const char *path)
{
    size_t size = 0, i;
    uint8_t *base = map_pe_by_rva(path, &size);
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    const sig_entry *entry = NULL;
    sig_result r;
    sh_modswf_engine e;
    const expect *x;
    uint64_t preferred;
    void **slot;
    CHECK(base != NULL);
    if (!base) return;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    if (!id) { free(base); return; }
    x = id->vulkan ? &VK : &GL;
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "FileResourceLoad")) entry = &BACKEND_ENGINE_SIGNATURES[i];
    CHECK(entry != NULL);
    if (!entry) { free(base); return; }
    CHECK(sig_resolve_one(base, entry, &r) == SIG_OK);
    CHECK(r.rva == x->load && (id->vulkan ? entry->rva_vk : entry->rva_gl) == x->load);
    CHECK(sh_modswf_decode_load(base + r.rva, &e));
    CHECK(e.state - (uintptr_t)base == x->state);
    CHECK(e.alloc - (uintptr_t)base == x->alloc);
    CHECK(e.named_ctor - (uintptr_t)base == x->ctor);
    CHECK(e.open_into - (uintptr_t)base == x->open);
    CHECK(sh_modswf_open_into_uses_provider(base + x->open));
    preferred = ((IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.ImageBase;
    slot = sh_modswf_find_slot(base, preferred + r.rva);
    CHECK(slot != NULL && (size_t)((uint8_t *)slot - base) == x->slot);
    /* A value present nowhere and a decode of a shifted body are refused. */
    CHECK(sh_modswf_find_slot(base, preferred + r.rva + 1) == NULL);
    CHECK(!sh_modswf_decode_load(base + r.rva + 1, &e));
    printf("%s %s: FileResourceLoad 0x%x, open-into 0x%x, vtable slot 0x%x (+0x40 of 0x%x)\n",
           g_fail ? "BAD" : "OK ", id->vulkan ? "Vulkan" : "OpenGL", r.rva, x->open, x->slot, x->slot - 0x40);
    free(base);
}

int main(int argc, char **argv)
{
    test_predicate();
    test_routing();
    test_builder();
    for (int i = 1; i < argc; i++) test_image(argv[i]);
    printf("modswf_test: %s (%d image%s)\n", g_fail ? "FAIL" : "ok", argc - 1, argc == 2 ? "" : "s");
    return g_fail;
}
