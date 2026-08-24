/* override_packages_test.c -- the file shadow resolves a decl out of ANY
 * installed package, not just the pre-package shared tree.
 *
 * The engine only ever asks for a decl by its canonical virtual name,
 * `generated/decls/<type>/<name>.decl`. Before packages that mapped one-to-one
 * onto `overrides\generated\decls\...`, so a straight join of the name onto the
 * overrides root was the whole resolver. A package owns its own root
 * (`overrides\cyberdemon\decls\...`), so that join can never reach it and the
 * package's decl bodies are silently never served -- the engine parses an empty
 * default instead, which is what left the Cyberdemon out of the Toybox.
 *
 * These tests pin the resolver: the legacy tree still resolves where it always
 * did, every package is reachable at any depth, and a package can serve nothing
 * outside its own `decls` subdirectory. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "overrides.h"
#include "resource_bridge.h"

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

/* The resolver is the only thing under test here, so the rest of the overrides
 * layer's collaborators are stubbed. The user layer is reported ON because a
 * disabled user layer is a separate contract, already covered elsewhere. */
void backend_log(const char *message)
{
    (void)message;
}

int sh_user_overrides_enabled_for_launch(void)
{
    return 1;
}

int sh_resource_bridge_capture(const char *data_root)
{
    (void)data_root;
    return 1;
}

void sh_resource_bridge_set_provider_ready(int ready)
{
    (void)ready;
}

int sh_resource_bridge_open(const char *name, unsigned char **out,
                            size_t *out_length, const char **out_source)
{
    (void)name;
    if (out) *out = NULL;
    if (out_length) *out_length = 0;
    if (out_source) *out_source = NULL;
    return SH_RESOURCE_BRIDGE_MISS;
}

static void join(char *out, size_t size, const char *a, const char *b)
{
    _snprintf_s(out, size, _TRUNCATE, "%s\\%s", a, b);
}

/* Create every missing directory along `path` (which names a directory). */
static int make_dirs(const char *path)
{
    char work[MAX_PATH];
    char *p;
    strcpy_s(work, sizeof(work), path);
    for (p = work + 3; *p; p++) {
        if (*p != '\\') continue;
        *p = '\0';
        if (!CreateDirectoryA(work, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return 0;
        *p = '\\';
    }
    return CreateDirectoryA(work, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Write `text` to `path`, creating its parent directories. */
static int write_file(const char *path, const char *text)
{
    char parent[MAX_PATH];
    char *slash;
    HANDLE file;
    DWORD written = 0;
    DWORD length = (DWORD)strlen(text);
    strcpy_s(parent, sizeof(parent), path);
    slash = strrchr(parent, '\\');
    if (!slash) return 0;
    *slash = '\0';
    if (!make_dirs(parent)) return 0;
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    WriteFile(file, text, length, &written, NULL);
    CloseHandle(file);
    return written == length;
}

static void remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(found.cFileName, ".") == 0 ||
                strcmp(found.cFileName, "..") == 0) continue;
            join(child, sizeof(child), path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileA(child);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    RemoveDirectoryA(path);
}

static int ends_with_ci(const char *value, const char *suffix)
{
    size_t v = value ? strlen(value) : 0;
    size_t s = suffix ? strlen(suffix) : 0;
    return value && suffix && v >= s && _stricmp(value + v - s, suffix) == 0;
}

int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH], resolved[MAX_PATH];

    if (!GetTempPathA(sizeof(temp), temp)) {
        fprintf(stderr, "GetTempPathA failed\n");
        return 1;
    }
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%soverride_packages_test_%lu",
                temp, (unsigned long)GetCurrentProcessId());
    remove_tree(root);
    if (!make_dirs(root)) {
        fprintf(stderr, "could not create %s\n", root);
        return 1;
    }

    /* The pre-package shared tree, still read as a package named "generated". */
    join(path, sizeof(path), root,
         "overrides\\generated\\decls\\snapeditorentitydef\\demons\\demon_base.decl");
    CHECK(write_file(path, "{ inherit = \"demons/base\"; }\n"));

    /* A dragged-in package at the top level. */
    join(path, sizeof(path), root, "overrides\\cyberdemon\\package.json");
    CHECK(write_file(path, "{ \"name\": \"cyberdemon\" }\n"));
    join(path, sizeof(path), root,
         "overrides\\cyberdemon\\decls\\snapeditorentitydef\\demons\\cyberdemon_enc.decl");
    CHECK(write_file(path, "{ inherit = \"demons/demon_base\"; }\n"));

    /* A package shipping a custom render program. The subtree under shaders\\
     * mirrors the engine resource namespace verbatim, so nothing is re-derived. */
    join(path, sizeof(path), root,
         "overrides\\cyberdemon\\shaders\\generated\\spirv\\cyberdemonshockwave.vspv");
    CHECK(write_file(path, "spirv-bytes"));
    join(path, sizeof(path), root,
         "overrides\\cyberdemon\\shaders\\generated\\renderprogs\\cyberdemonshockwave_pc_vulkan.bin");
    CHECK(write_file(path, "blob-bytes"));

    /* A package the user filed inside grouping folders. */
    join(path, sizeof(path), root, "overrides\\demons\\hell\\imps\\package.json");
    CHECK(write_file(path, "{ \"name\": \"imps\" }\n"));
    join(path, sizeof(path), root,
         "overrides\\demons\\hell\\imps\\decls\\entitydef\\ai\\demon\\imp_hell.decl");
    CHECK(write_file(path, "{ class = \"idAI\"; }\n"));

    CHECK(sh_overrides_set_root(root));

    /* 1. The legacy tree still resolves exactly where it always did. */
    CHECK(sh_overrides_test_resolve_existing(
              "generated/decls/snapeditorentitydef/demons/demon_base.decl",
              resolved, sizeof(resolved)));
    CHECK(ends_with_ci(resolved,
                       "\\overrides\\generated\\decls\\snapeditorentitydef\\demons\\demon_base.decl"));

    /* 2. A top-level package's decl resolves under the canonical engine name. */
    CHECK(sh_overrides_test_resolve_existing(
              "generated/decls/snapeditorentitydef/demons/cyberdemon_enc.decl",
              resolved, sizeof(resolved)));
    CHECK(ends_with_ci(resolved,
                       "\\overrides\\cyberdemon\\decls\\snapeditorentitydef\\demons\\cyberdemon_enc.decl"));

    /* 3. A nested package is reachable too -- grouping folders are free. */
    CHECK(sh_overrides_test_resolve_existing(
              "generated/decls/entitydef/ai/demon/imp_hell.decl",
              resolved, sizeof(resolved)));
    CHECK(ends_with_ci(resolved,
                       "\\overrides\\demons\\hell\\imps\\decls\\entitydef\\ai\\demon\\imp_hell.decl"));

    /* 4. The engine spells a decl type with its registered casing; the tree on
     *    disk carries whatever casing the packer wrote. Resolution must not care. */
    CHECK(sh_overrides_test_resolve_existing(
              "generated/decls/snapEditorEntityDef/demons/cyberdemon_enc.decl",
              resolved, sizeof(resolved)));

    /* 5. A name nothing provides stays unresolved -- no partial path handed back. */
    CHECK(!sh_overrides_test_resolve_existing(
              "generated/decls/entitydef/ai/demon/nothing_here.decl",
              resolved, sizeof(resolved)));

    /* 6. A package serves its `decls` subdirectory and nothing else. Its
     *    package.json must not become reachable as an engine resource. */
    CHECK(!sh_overrides_test_resolve_existing("generated/package.json",
                                              resolved, sizeof(resolved)));
    CHECK(!sh_overrides_test_resolve_existing("package.json",
                                              resolved, sizeof(resolved)));

    /* 8. A package's SPIR-V module resolves. The engine opens this name through
     *    provider vtable +0xf8 with mode 0, which the shadow's mode<2 guard admits. */
    CHECK(sh_overrides_test_resolve_existing(
              "generated/spirv/cyberdemonshockwave.vspv", resolved, sizeof(resolved)));
    CHECK(ends_with_ci(resolved,
                       "\\overrides\\cyberdemon\\shaders\\generated\\spirv\\cyberdemonshockwave.vspv"));

    /* 9. So does the pre-translated blob the freshness hash is taken over. */
    CHECK(sh_overrides_test_resolve_existing(
              "generated/renderprogs/cyberdemonshockwave_pc_vulkan.bin",
              resolved, sizeof(resolved)));
    CHECK(ends_with_ci(resolved,
                       "\\overrides\\cyberdemon\\shaders\\generated\\renderprogs\\cyberdemonshockwave_pc_vulkan.bin"));

    /* 10. A shader name nothing ships stays unresolved. */
    CHECK(!sh_overrides_test_resolve_existing(
              "generated/spirv/nothing_here.vspv", resolved, sizeof(resolved)));

    /* 11. Only the mirrored namespaces are served -- a package cannot expose
     *     arbitrary files by parking them under shaders\\. */
    CHECK(!sh_overrides_test_resolve_existing(
              "generated/other/cyberdemonshockwave.vspv", resolved, sizeof(resolved)));

    /* 7. Names outside the generated/ namespace are not package-resolved. */
    CHECK(!sh_overrides_test_resolve_existing(
              "cooked/decls/snapeditorentitydef/demons/cyberdemon_enc.decl",
              resolved, sizeof(resolved)));

    remove_tree(root);
    if (g_failed) {
        fprintf(stderr, "override package resolution tests FAILED (%d)\n", g_failed);
        return 1;
    }
    printf("override package resolution tests passed\n");
    return 0;
}
