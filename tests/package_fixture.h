/* Filesystem fixtures confined to one unique temporary directory. */
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
static char root[MAX_PATH];
#ifndef PACKAGE_FIXTURE_ENTRIES
#define PACKAGE_FIXTURE_ENTRIES 512u
#endif
static char created[PACKAGE_FIXTURE_ENTRIES][4096];
static int directories[PACKAGE_FIXTURE_ENTRIES];
static size_t created_count;

static void create(const char *relative, const char *body)
{
    char path[4096];
    wchar_t *wide;
    size_t i;
    HANDLE file;
    DWORD written;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    for (i = strlen(root) + 1u; ; i++) if (path[i] == '/' || !path[i]) {
        char saved = path[i];
        if (!saved && body) break;
        path[i] = 0; wide = sh_package_source_wide_path(path); CHECK(wide);
        if (!wide) return;
        if (CreateDirectoryW(wide, NULL)) {
            CHECK(created_count < PACKAGE_FIXTURE_ENTRIES);
            if (created_count >= PACKAGE_FIXTURE_ENTRIES) { free(wide); return; }
            strcpy_s(created[created_count], sizeof(created[0]), path); directories[created_count++] = 1;
        } else CHECK(GetLastError() == ERROR_ALREADY_EXISTS);
        free(wide); path[i] = saved;
        if (!saved) return;
    }
    wide = sh_package_source_wide_path(path); CHECK(wide);
    if (!wide) return;
    file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide); CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    CHECK(WriteFile(file, body, (DWORD)strlen(body), &written, NULL)); CHECK(written == strlen(body));
    CloseHandle(file);
    for (i = 0; i < created_count; i++) if (!strcmp(created[i], path)) return;
    CHECK(created_count < PACKAGE_FIXTURE_ENTRIES);
    if (created_count >= PACKAGE_FIXTURE_ENTRIES) return;
    strcpy_s(created[created_count], sizeof(created[0]), path); directories[created_count++] = 0;
}

/* Also used by tests with a nested data root. Never inspect a cache outside
 * the fixture, and delete only verified flat cache filenames. */
static void cleanup_resources(const char *fixture_root)
{
    CHECK(!strncmp(fixture_root, root, strlen(root)) &&
        (!fixture_root[strlen(root)] || fixture_root[strlen(root)] == '/' || fixture_root[strlen(root)] == '\\'));
    /* Every snapshot closes with delete-on-close. Remove only the now-empty,
     * verified fixture directory; surviving handles must fail this check. */
    {
        char bundles[4096];
        DWORD attributes;
        snprintf(bundles, sizeof(bundles), "%s/package-cache/bundles", fixture_root);
        attributes = GetFileAttributesA(bundles);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            CHECK((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                CHECK(RemoveDirectoryA(bundles));
        }
    }
    /* Runtime tests may prepare a flat, content-addressed resource cache.
     * Exact hash collisions or damaged old entries use a positive decimal
     * suffix. Verify that grammar too, confined to this fixture's own root. */
    {
        char directory[4096], pattern[4096], path[4352];
        WIN32_FIND_DATAA found;
        HANDLE search;
        snprintf(directory, sizeof(directory), "%s/package-cache/resources", fixture_root);
        snprintf(pattern, sizeof(pattern), "%s/*", directory);
        search = FindFirstFileA(pattern, &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                wchar_t *wide;
                size_t j,length;
                int valid;
                if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
                CHECK(!(found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
                length=strlen(found.cFileName);valid=length>=64;
                for(j=0;valid && j<64;j++)valid=strchr("0123456789abcdef",found.cFileName[j])!=NULL;
                if(valid && length>64) {
                    uint64_t suffix=0;
                    valid=length>65 && found.cFileName[64]=='.' && found.cFileName[65]>='1' && found.cFileName[65]<='9';
                    for(j=65;valid && j<length;j++) {
                        unsigned digit=(unsigned)(found.cFileName[j]-'0');
                        if(digit>9 || suffix>(UINT64_MAX-digit)/10)valid=0;
                        else suffix=suffix*10+digit;
                    }
                }
                CHECK(valid);
                if (!valid || (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
                snprintf(path, sizeof(path), "%s/%s", directory, found.cFileName);
                wide = sh_package_source_wide_path(path); CHECK(wide);
                if (wide) { CHECK(DeleteFileW(wide)); free(wide); }
            } while (FindNextFileA(search, &found));
            FindClose(search);
            CHECK(RemoveDirectoryA(directory));
        }
        snprintf(directory, sizeof(directory), "%s/package-cache", fixture_root);
        /* Derived-state files the runtime keeps beside the resources. */
        {
            static const char *const state[] = {"own-bytes-exclusions.txt", "own-bytes-exclusions-vk.txt",
                "own-bytes-exclusions-gl.txt", "shader-preparation-vk.txt", "shader-preparation-gl.txt",
                "warm-publication-vk.txt", "warm-publication-gl.txt", "scope-reuse-vk.txt", "scope-reuse-gl.txt",
                "map-contexts-vk.txt", "map-contexts-gl.txt"};
            for (size_t k = 0; k < sizeof(state) / sizeof(*state); k++) {
                snprintf(path, sizeof(path), "%s/%s", directory, state[k]);
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) CHECK(DeleteFileA(path));
            }
        }
        if (GetFileAttributesA(directory) != INVALID_FILE_ATTRIBUTES) CHECK(RemoveDirectoryA(directory));
    }
 }

static void cleanup(void)
{
    size_t i;
#ifdef SH_PACKAGE_RUNTIME_TESTING
    /* The fixture owns no native consumers. Retire its provider seals before
     * deleting cached files, while tests close their own retained streams. */
    extern void sh_package_runtime_test_dispose(void);
    sh_package_runtime_test_dispose();
#endif
    cleanup_resources(root);
    while (created_count) {
        wchar_t *wide;
        i = --created_count; CHECK(!strncmp(created[i], root, strlen(root)));
        wide = sh_package_source_wide_path(created[i]); CHECK(wide);
        if (wide) { CHECK(directories[i] ? RemoveDirectoryW(wide) : DeleteFileW(wide)); free(wide); }
    }
    CHECK(RemoveDirectoryA(root));
}
