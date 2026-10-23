/* Verified executable identity must not admit another renderer's RVA table. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "host_image.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failed++; } } while (0)

int wmain(int argc, wchar_t **argv)
{
    int failed = 0;
    if (argc != 3) return 2;
    int renderer = _wtoi(argv[2]);
    CHECK(renderer == 0 || renderer == 1);
    CHECK(sh_host_test_file_renderer(argv[1]) == renderer);
    unsigned char image = 0;
    CHECK(sh_host_test_bind_pinned_image(&image, argv[1]) == renderer);
    CHECK(sh_host_is_pinned_rva_image(&image) == renderer);
    CHECK(sh_host_test_file_renderer(NULL) == -1);
    CHECK(!sh_host_is_pinned_renderer_build(-1));
    CHECK(!sh_host_is_pinned_renderer_build(2));
    /* This test process is not either game executable. A basename or loaded
     * renderer library cannot turn an unknown file into an admitted build. */
    CHECK(!sh_host_is_pinned_renderer_build(0));
    CHECK(!sh_host_is_pinned_renderer_build(1));

    wchar_t root[MAX_PATH], path[MAX_PATH];
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD count = 0;
    unsigned char byte = 0;
    CHECK(GetTempPathW(MAX_PATH, root) && GetTempFileNameW(root, L"shr", 0, path));
    if (!failed) {
        CHECK(CopyFileW(argv[1], path, FALSE));
        file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            CHECK(ReadFile(file, &byte, 1, &count, NULL) && count == 1);
            byte ^= 1;
            CHECK(SetFilePointer(file, 0, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER);
            CHECK(WriteFile(file, &byte, 1, &count, NULL) && count == 1);
            CloseHandle(file);
            CHECK(sh_host_test_file_renderer(path) == -1);
            CHECK(!sh_host_test_bind_pinned_image(&image, path));
            CHECK(!sh_host_is_pinned_rva_image(&image));
        }
        DeleteFileW(path);
    }
    if (!failed) printf("renderer %d executable identity checks passed\n", renderer);
    return failed ? 1 : 0;
}
