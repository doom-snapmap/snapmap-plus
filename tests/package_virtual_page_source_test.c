#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/package_virtual_page_source.c"
#include "package_fixture.h"

enum { PAGE_LENGTH = 0x199 };
static void put32(unsigned char *p, uint32_t x)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(x >> (i * 8)); }
static void put64(unsigned char *p, uint64_t x)
{ for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(x >> (i * 8)); }
static void page(unsigned char bytes[PAGE_LENGTH], unsigned char payload)
{
    memset(bytes, 0, PAGE_LENGTH); put32(bytes, 0xa63fbb21u); put32(bytes + 4, 2);
    put32(bytes + 0xc, 12); put64(bytes + 0x38, 0x170); put64(bytes + 0x40, 0x180);
    put32(bytes + 0x48, 1); put32(bytes + 0x4c, 2);
    put32(bytes + 0x58, 2); put32(bytes + 0x5c, 1); put32(bytes + 0x64, 2);
    put64(bytes + 0x170, 0x188); put64(bytes + 0x178, 17);
    put32(bytes + 0x184, UINT32_MAX); memset(bytes + 0x188, payload, 17);
}
static sh_package_file *saved(const char *relative, const unsigned char *body, size_t length)
{
    char path[4096], error[256]; create(relative, "");
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(body, 1, length, file) == length && !fclose(file));
    sh_package_file *result = sh_package_file_capture(path, error, sizeof(error)); assert(result); return result;
}
typedef struct originals {
    const char *stem;
    sh_package_file *files[21];
    unsigned calls[21], count;
    int fail;
} originals;
static int original(void *context, const char *path, sh_package_file **out, char *error, size_t capacity)
{
    originals *f = context; (void)error; (void)capacity; *out = NULL;
    if (f->fail) return -1;
    for (unsigned i = 0; i < 21; i++) {
        char *expected = sh_package_virtual_page_companion(f->stem, i);
        int equal = expected && !strcmp(path, expected); free(expected);
        if (!equal) continue;
        assert(f->count < 21); f->calls[f->count++] = i;
        *out = sh_package_file_retain(f->files[i]); return *out != NULL;
    }
    assert(0); return -1;
}
static sh_compiled_resource resource(const char *path, unsigned char *body, size_t size)
{
    sh_compiled_resource out = {0}; out.engine_path = path; out.source = SIZE_MAX;
    out.body = body; out.body_length = size; return out;
}
static void sources_and_seals(void)
{
    unsigned char a[PAGE_LENGTH], b[PAGE_LENGTH], empty[1] = {0};
    char error[512]; page(a, 0x37); page(b, 0x63);
    originals f = {0}; f.stem = "virtualtextures/authored";
    f.files[1] = saved("original/second.mega2", b, sizeof(b));
    /* The authored empty member suppresses this original slot. */
    f.files[2] = saved("original/third.mega2", a, sizeof(a));
    sh_compiled_resource input[] = {resource("virtualtextures/authored_sq1.mega2", a, sizeof(a)),
        resource("virtualtextures/authored_sq3.mega2", empty, 0)};
    size_t input_sources[] = {5, 9}; input[0].source = 5; input[0].sources = input_sources; input[0].source_count = 2;
    assert(sh_package_owners_add(&input[0].owners, 7));
    sh_package_compilation scope = {0}; scope.resources = input; scope.resource_count = 2;
    sh_virtual_page_source *source = sh_virtual_page_source_open(&scope, f.stem, original, &f, error, sizeof(error));
    assert(source && f.count == 19 && f.calls[0] == 1 && f.calls[1] == 3 && f.calls[18] == 20);
    const sh_compiled_resource *member = sh_virtual_page_source_member(source, 0);
    assert(member && member->source == 5 && member->source_count == 2 && member->sources[1] == 9);
    assert(sh_package_owners_contains(&member->owners, 7) && !sh_virtual_page_source_member(source, 2));
    assert(!strcmp(sh_virtual_page_source_origin(source), "virtualtextures/authored.pages"));
    sh_package_owners_free(&input[0].owners);
    a[PAGE_LENGTH - 1] ^= 1; /* Capture owns the authored body. */
    assert(member->body[PAGE_LENGTH - 1] == 0x37); a[PAGE_LENGTH - 1] ^= 1;

    const char *paths[21] = {"virtualtextures/private_sq1.mega2", "virtualtextures/private_sq2.mega2"};
    sh_compiled_resource output[] = {resource(paths[0], a, sizeof(a)), resource(paths[1], NULL, 0)};
    output[1].cache_file = f.files[2]; /* Valid file, wrong captured bytes. */
    sh_package_compilation compiled = {0}; compiled.resources = output; compiled.resource_count = 2;
    assert(!sh_virtual_page_source_seal(source, &compiled, paths, error, sizeof(error)));
    assert(!sh_virtual_page_source_sealed(source) && !source->members[0].sealed_body);
    FILE *left = NULL, *right = NULL; uint64_t length = 0;
    assert(sh_virtual_page_source_open_member(source, 1, &left, &length, error, sizeof(error)) == -1 && !left);
    output[1].cache_file = f.files[1];
    assert(sh_virtual_page_source_seal(source, &compiled, paths, error, sizeof(error)));
    assert(sh_virtual_page_source_seal(source, &compiled, paths, error, sizeof(error)));
    size_t body_length = 0;
    const unsigned char *body = sh_virtual_page_source_body_member(source, 0, &body_length);
    assert(body && body_length == sizeof(a) && !memcmp(body, a, sizeof(a)));
    a[PAGE_LENGTH - 1] ^= 1;
    assert(!sh_virtual_page_source_seal(source, &compiled, paths, error, sizeof(error)));
    assert(sh_virtual_page_source_sealed(source) && body[PAGE_LENGTH - 1] == 0x37);
    assert(sh_virtual_page_source_open_member(source, 1, &left, &length, error, sizeof(error)) == 1 && length == sizeof(b));
    assert(sh_virtual_page_source_open_member(source, 1, &right, &length, error, sizeof(error)) == 1);
    assert(fgetc(left) == 0x21 && _ftelli64(right) == 0);
    sh_virtual_page_source *held = sh_virtual_page_source_retain(source);
    sh_virtual_page_source_release(source); sh_virtual_page_source_release(held);
    sh_package_file_release(f.files[1]); sh_package_file_release(f.files[2]);
    assert(!_fseeki64(left, PAGE_LENGTH - 1, SEEK_SET) && fgetc(left) == 0x63);
    assert(fgetc(right) == 0x21); assert(!fclose(left) && !fclose(right));
}
static void exact_family_equality(void)
{
    unsigned char first[PAGE_LENGTH], second[PAGE_LENGTH]; char error[512];
    page(first, 1); page(second, 1);
    sh_compiled_resource input[] = {resource("virtualtextures/a_sq1.mega2", first, sizeof(first)),
        resource("virtualtextures/b_sq1.mega2", second, sizeof(second))};
    sh_package_compilation scope = {0}; scope.resources = input; scope.resource_count = 2;
    sh_virtual_page_source *a = sh_virtual_page_source_open(&scope, "virtualtextures/a", NULL, NULL, error, sizeof(error));
    sh_virtual_page_source *b = sh_virtual_page_source_open(&scope, "virtualtextures/b", NULL, NULL, error, sizeof(error));
    assert(a && b && !memcmp(a->identity, b->identity, 32));
    assert(sh_virtual_page_source_equal(a, b, error, sizeof(error)) == 1);
    /* Inject equal digest metadata with unequal bytes. Positive comparison must
     * still distinguish the family; names and digests are not certificates. */
    b->members[0].input.body[PAGE_LENGTH - 1] ^= 1;
    assert(!memcmp(a->members[0].identity.digest, b->members[0].identity.digest, 32));
    assert(sh_virtual_page_source_equal(a, b, error, sizeof(error)) == 0);
    b->members[0].input.body[PAGE_LENGTH - 1] ^= 1;
    b->members[0].present = 0;
    assert(sh_virtual_page_source_equal(a, b, error, sizeof(error)) == 0);
    b->members[0].present = 1;
    sh_virtual_page_source_release(a); sh_virtual_page_source_release(b);
    input[0].body_length = 0x16f;
    assert(!sh_virtual_page_source_open(&scope, "virtualtextures/a", NULL, NULL, error, sizeof(error)));
    scope.resource_count = 0;
    assert(!sh_virtual_page_source_open(&scope, "virtualtextures/a", NULL, NULL, error, sizeof(error)));
    originals failed = {0}; failed.fail = 1;
    assert(!sh_virtual_page_source_open(&scope, "virtualtextures/a", original, &failed, error, sizeof(error)));
}
static void stale_editable_source(void)
{
    unsigned char bytes[PAGE_LENGTH]; char absolute[4096], error[512]; page(bytes, 0x75);
    sh_package_file *cached = saved("authored/sq1.mega2", bytes, sizeof(bytes));
    sh_package_file_identity identity; assert(sh_package_file_get_identity(cached, &identity));
    sh_package_file_release(cached);
    snprintf(absolute, sizeof(absolute), "%s/authored/sq1.mega2", root);
    sh_package_source_file file = {0}; file.absolute = absolute; file.engine_path = "virtualtextures/a_sq1.mega2";
    file.length = identity.length; memcpy(file.digest, identity.digest, 32);
    sh_package_sources inventory = {0}; inventory.files = &file; inventory.file_count = 1;
    sh_compiled_resource input = resource(file.engine_path, NULL, 0); input.source = 0;
    sh_package_compilation scope = {0}; scope.sources = &inventory; scope.resources = &input; scope.resource_count = 1;
    sh_virtual_page_source *source = sh_virtual_page_source_open(&scope, "virtualtextures/a", NULL, NULL, error, sizeof(error));
    assert(source);
    sh_compiled_resource output = resource("virtualtextures/private_sq1.mega2", bytes, sizeof(bytes));
    sh_package_compilation compiled = {0}; compiled.resources = &output; compiled.resource_count = 1;
    const char *paths[21] = {output.engine_path};
    FILE *writer = fopen(absolute, "r+b"); assert(writer);
    assert(!_fseeki64(writer, PAGE_LENGTH - 1, SEEK_SET) && fputc(0x76, writer) == 0x76 && !fclose(writer));
    assert(!sh_virtual_page_source_seal(source, &compiled, paths, error, sizeof(error)));
    assert(!sh_virtual_page_source_sealed(source)); sh_virtual_page_source_release(source);
}
int main(void)
{
    char temporary[MAX_PATH]; assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-vt-source-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    sources_and_seals(); exact_family_equality(); stale_editable_source();
    cleanup(); assert(!failures); puts("package_virtual_page_source_test: PASS"); return 0;
}
