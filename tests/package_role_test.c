/* Native role identity (R-155b): role equality on the native palette
 * contract, the widened identity scan, the default-marker lookup at verified
 * serializer sites only and, given an unpacked image, the exact sites of each
 * renderer:
 *   package_role_test.exe [<unpacked exe> vk|gl] */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/package_role.h"
#include "../src/backend/hook.h"

static int g_failed, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

void *hook_prepare(void *target, void *detour, size_t stolen) { (void)target; (void)detour; (void)stolen; return NULL; }
sh_patch_status hook_commit(void *tramp) { (void)tramp; return B2_PATCH_OK; }
int hook_unpatch(void *tramp) { (void)tramp; return 1; }
void backend_log(const char *message) { printf("log: %s\n", message); }

/* Native layouts: key holder name at +0x58; palette body rows at +0x10 with
 * their count at +0x18 and the lookup end at +0x28; map entity pointers at
 * +0xc0 with count at +0xc8; an entity's row at +8. */
typedef struct holder { unsigned char pad[0x58]; const char *name; } holder;
typedef struct palette { unsigned char pad[0x10]; unsigned char *begin; int count, capacity; unsigned char pad2[8]; unsigned char *end; } palette;
typedef struct entity { void *vt; const void *row; } entity;
typedef struct map { unsigned char pad[0xc0]; entity **items; int count; } map;

/* N native player start, V same-key package variant, W unrelated, P a
 * different key sharing a prefix, Z and Y without a holder, E an empty key,
 * X absent. */
static int N, V, W, P, Z, Y, E, X;
static holder hk_start = {{0}, "player_start"}, hk_start2 = {{0}, "player_start"};
static holder hk_weapon = {{0}, "weapon"}, hk_other = {{0}, "player_start_alt"}, hk_empty = {{0}, ""};

static unsigned char *rows(const void **decls, holder **keys, size_t n)
{
    unsigned char *r = calloc(n, 0x6f0);
    for (size_t i = 0; r && i < n; i++) {
        *(const void **)(r + i*0x6f0) = decls[i];
        *(holder **)(r + i*0x6f0 + 0x150) = keys[i];
    }
    return r;
}
/* The native lookup's contract: the first matching row, or the end pointer. */
static const unsigned char *row_of(const palette *p, const void *decl)
{
    for (unsigned char *e = p->begin; e < p->end; e += 0x6f0) if (*(const void **)e == decl) return e;
    return p->end;
}
static const void *fake_by_decl(void *pal, const void *decl) { return row_of(pal, decl); }
static int32_t g_flag = 1;
/* The live editor palette as palette_refresh reports it. */
static void *g_live;
void *sh_palette_refresh_editor_palette(void) { return g_live; }
static __declspec(align(16)) unsigned char g_bank[0x150];
static const unsigned char *fake_getter(void *holder_at, uint64_t zero) { (void)holder_at; (void)zero; return g_bank; }

static void palette_cases(int reversed)
{
    const void *d0[] = {&N, &V, &W, &P, &Z, &Y, &E}, *d1[] = {&V, &N, &W, &P, &Z, &Y, &E};
    holder *k0[] = {&hk_start, &hk_start2, &hk_weapon, &hk_other, NULL, NULL, &hk_empty};
    holder *k1[] = {&hk_start2, &hk_start, &hk_weapon, &hk_other, NULL, NULL, &hk_empty};
    palette pal = {{0}};
    pal.begin = rows(reversed ? d1 : d0, reversed ? k1 : k0, 7);
    CHECK(pal.begin != NULL);
    if (!pal.begin) return;
    pal.count = 7; pal.end = pal.begin + 7*0x6f0;

    /* Same key through a different declaration is the role; nothing else is. */
    CHECK(sh_package_role_equal(&pal, &V, &N) && sh_package_role_equal(&pal, &N, &V));
    CHECK(!sh_package_role_equal(&pal, &W, &N) && !sh_package_role_equal(&pal, &P, &N));
    CHECK(!sh_package_role_equal(&pal, &X, &N) && sh_package_role_equal(&pal, &X, &X));
    CHECK(!sh_package_role_equal(&pal, &Z, &N) && !sh_package_role_equal(&pal, &N, &Z));
    CHECK(!sh_package_role_equal(NULL, &V, &N));
    /* Keyless rows read as "" natively but never share a role. */
    CHECK(!sh_package_role_equal(&pal, &Z, &Y) && !sh_package_role_equal(&pal, &Y, &Z) && sh_package_role_equal(&pal, &Z, &Z));
    CHECK(!sh_package_role_equal(&pal, &E, &Z) && !sh_package_role_equal(&pal, &Z, &E) && !sh_package_role_equal(&pal, &E, &Y));

    /* Identity scan: player start presence by role on the live palette. */
    entity ew = {0, &W}, ev = {0, &V}, ep = {0, &P}, en = {0, &N}, ez = {0, &Z}, ee = {0, &E};
    entity *a[] = {&ew, &ev}, *b[] = {&ew, &ep, &ez}, *c[] = {&en}, *d[] = {NULL, &ev}, *e[] = {&ee};
    map ma = {{0}, a, 2}, mb = {{0}, b, 3}, mc = {{0}, c, 1}, md = {{0}, d, 2}, me = {{0}, e, 1}, empty = {{0}, NULL, 0};
    g_live = NULL;
    CHECK(!sh_package_role_contains(&ma, &N)); /* no live palette: pointer identity only */
    g_live = &pal;
    CHECK(sh_package_role_contains(&ma, &N));
    pal.end = pal.begin; /* a lookup prefix that exposes no rows hides none */
    CHECK(sh_package_role_contains(&ma, &N) && !sh_package_role_contains(&mb, &N));
    pal.count = 6; pal.end = pal.begin + 7*0x6f0; /* end past the bodies: not the native layout */
    CHECK(!sh_package_role_contains(&ma, &N));
    pal.count = 7;
    CHECK(sh_package_role_contains(&md, &N)); /* null entity slots are skipped */
    CHECK(!sh_package_role_contains(&mb, &N));
    CHECK(!sh_package_role_contains(&mc, &N)); /* the exact pointer stays the native result */
    CHECK(!sh_package_role_contains(&empty, &N));
    CHECK(!sh_package_role_contains(&ma, &X));
    CHECK(!sh_package_role_contains(&mb, &P)); /* P has no other row of its key */
    CHECK(!sh_package_role_contains(&mb, &Y) && !sh_package_role_contains(&me, &Y)); /* keyless slot */
    CHECK(!sh_package_role_contains(&mb, &E) && !sh_package_role_contains(&me, &Z));
    CHECK(!sh_package_role_contains(NULL, &N));

    /* Default marker: the slot's own row only for a role-equal declaration. */
    CHECK(sh_package_role_canonical(&pal, &V, row_of(&pal, &V), &N) == row_of(&pal, &N));
    CHECK(sh_package_role_canonical(&pal, &W, row_of(&pal, &W), &N) == row_of(&pal, &W));
    CHECK(sh_package_role_canonical(&pal, &P, row_of(&pal, &P), &N) == row_of(&pal, &P));
    CHECK(sh_package_role_canonical(&pal, &X, row_of(&pal, &X), &N) == pal.end);
    CHECK(sh_package_role_canonical(&pal, &V, row_of(&pal, &V), NULL) == row_of(&pal, &V));
    CHECK(sh_package_role_canonical(&pal, &N, row_of(&pal, &N), &N) == row_of(&pal, &N));
    CHECK(sh_package_role_canonical(&pal, &Y, row_of(&pal, &Y), &Z) == row_of(&pal, &Y));
    CHECK(sh_package_role_canonical(&pal, &E, row_of(&pal, &E), &Z) == row_of(&pal, &E));

    /* Through the detour: only the verified serializer return sites widen. */
    sh_package_role_sites sites = {{0x1000, 0x3000}, (uintptr_t)&g_flag, 0x5000, (uintptr_t)fake_getter, 2};
    sh_package_role_test_sites(&sites, fake_by_decl);
    *(const void **)(g_bank + 0x148) = &N; g_flag = 1;
    CHECK(sh_package_role_test_by_decl(&pal, &V, 0x3000) == row_of(&pal, &N));
    CHECK(sh_package_role_test_by_decl(&pal, &V, 0x1000) == row_of(&pal, &N));
    CHECK(sh_package_role_test_by_decl(&pal, &W, 0x3000) == row_of(&pal, &W));
    CHECK(sh_package_role_test_by_decl(&pal, &X, 0x3000) == pal.end);
    CHECK(sh_package_role_test_by_decl(&pal, &V, 0x3001) == row_of(&pal, &V));
    g_flag = 0; /* bank not built yet: the native path */
    CHECK(sh_package_role_test_by_decl(&pal, &V, 0x1000) == row_of(&pal, &V));
    g_flag = 1; *(const void **)(g_bank + 0x148) = &W;
    CHECK(sh_package_role_test_by_decl(&pal, &V, 0x1000) == row_of(&pal, &V));
    *(const void **)(g_bank + 0x148) = &Z; /* a keyless bank entry widens nothing */
    CHECK(sh_package_role_test_by_decl(&pal, &Y, 0x1000) == row_of(&pal, &Y));
    CHECK(sh_package_role_test_by_decl(&pal, &E, 0x3000) == row_of(&pal, &E));
    *(const void **)(g_bank + 0x148) = NULL;
    CHECK(sh_package_role_test_by_decl(&pal, &V, 0x1000) == row_of(&pal, &V));
    sh_package_role_test_sites(NULL, NULL);
    g_live = NULL;
    free(pal.begin);
}

/* Any number of rows may share a key: the match is the 17th and the 40th of
 * forty same-key variants, beside keyless and unrelated controls. */
static void many_rows_case(void)
{
    enum { MANY = 40 };
    static int native, variants[MANY], other, keyless;
    const void *decls[MANY + 3]; holder *keys[MANY + 3];
    decls[0] = &native; keys[0] = &hk_start;
    for (int i = 0; i < MANY; i++) { decls[1 + i] = &variants[i]; keys[1 + i] = &hk_start2; }
    decls[MANY + 1] = &other; keys[MANY + 1] = &hk_weapon;
    decls[MANY + 2] = &keyless; keys[MANY + 2] = NULL;
    palette pal = {{0}};
    pal.begin = rows(decls, keys, MANY + 3);
    CHECK(pal.begin != NULL);
    if (!pal.begin) return;
    pal.count = MANY + 3; pal.end = pal.begin + (MANY + 3)*0x6f0;
    g_live = &pal;
    entity e16 = {0, &variants[16]}, e39 = {0, &variants[MANY - 1]}, eo = {0, &other}, ek = {0, &keyless};
    entity *a[] = {&eo, &e16}, *b[] = {&ek, &eo, &e39}, *c[] = {&eo, &ek};
    map ma = {{0}, a, 2}, mb = {{0}, b, 3}, mc = {{0}, c, 2};
    CHECK(sh_package_role_contains(&ma, &native) && sh_package_role_contains(&mb, &native));
    CHECK(sh_package_role_contains(&ma, &variants[0]) && sh_package_role_contains(&mb, &variants[16]));
    CHECK(!sh_package_role_contains(&mc, &native) && !sh_package_role_contains(&mb, &keyless));
    CHECK(!sh_package_role_contains(&mb, &other));
    CHECK(sh_package_role_equal(&pal, &variants[MANY - 1], &native) && !sh_package_role_equal(&pal, &keyless, &native));
    CHECK(sh_package_role_canonical(&pal, &variants[MANY - 1], row_of(&pal, &variants[MANY - 1]), &native) == row_of(&pal, &native));
    g_live = NULL;
    free(pal.begin);
}

static uint8_t *map_pe_by_rva(const char *path)
{
    FILE *f = fopen(path, "rb"); long size; uint8_t *file, *img;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    file = malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(file); return NULL; }
    fclose(f);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    img = calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t n = sec[i].SizeOfRawData;
        if (sec[i].VirtualAddress + n > nt->OptionalHeader.SizeOfImage) n = nt->OptionalHeader.SizeOfImage - sec[i].VirtualAddress;
        if (n && sec[i].PointerToRawData + n <= (uint32_t)size) memcpy(img + sec[i].VirtualAddress, file + sec[i].PointerToRawData, n);
    }
    free(file);
    return img;
}

static void image_cases(const char *path, const char *renderer)
{
    int vk = !strcmp(renderer, "vk");
    uint8_t *img = map_pe_by_rva(path);
    sig_result decl = {0}, scan = {0};
    sh_package_role_sites sites;
    CHECK(img != NULL);
    if (!img) return;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++) {
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "SnapPaletteFindDecl")) sig_resolve_one(img, BACKEND_ENGINE_SIGNATURES + i, &decl);
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "SnapMapHasEntityDecl")) sig_resolve_one(img, BACKEND_ENGINE_SIGNATURES + i, &scan);
    }
    CHECK(decl.status == SIG_OK && scan.status == SIG_OK);
    CHECK(decl.rva == (vk ? 0x54AB20u : 0x54A280u));
    CHECK(scan.rva == (vk ? 0x5A4A00u : 0x5A4170u));
    CHECK(sh_package_role_find_sites(img, decl.addr, &sites));
    CHECK(sites.count == 2);
    CHECK(sites.holder - (uintptr_t)img == (vk ? 0x571B6D0u : 0x401DCD0u));
    CHECK(sites.flag - sites.holder == 0x28);
    /* Inside the serializers R-155b names: VK 0x1405d0f10/0x1405d2ba0, GL 0x1405d0690/0x1405d2290. */
    CHECK(sites.returns[0] - (uintptr_t)img == (vk ? 0x5D100Du : 0x5D078Du));
    CHECK(sites.returns[1] - (uintptr_t)img == (vk ? 0x5D2C9Du : 0x5D238Du));
    printf("%s: by_decl=%#x scan=%#x holder=%#llx flag=%#llx getter=%#llx returns=%#llx %#llx\n", renderer,
        (unsigned)decl.rva, (unsigned)scan.rva,
        (unsigned long long)(sites.holder - (uintptr_t)img), (unsigned long long)(sites.flag - (uintptr_t)img),
        (unsigned long long)(sites.getter - (uintptr_t)img),
        (unsigned long long)(sites.returns[0] - (uintptr_t)img), (unsigned long long)(sites.returns[1] - (uintptr_t)img));
    /* Another callee has no such tails. */
    CHECK(!sh_package_role_find_sites(img, scan.addr, &sites) && sites.count == 0);
    free(img);
}

int main(int argc, char **argv)
{
    palette_cases(0);
    palette_cases(1);
    many_rows_case();
    if (argc > 2) image_cases(argv[1], argv[2]);
    printf("package_role_test: %d checks, %d failed\n", g_checks, g_failed);
    if (!g_failed) printf("package_role_test: PASS\n");
    return g_failed != 0;
}
