/* Static check of the backend proxy's export table.
 *
 *   xinput_exports_test <build-dir> [<DOOM image> ...]
 *
 * build-dir must hold XINPUT1_4.dll. Its exports must be exactly the System32
 * XINPUT1_4.dll ordinal set (named exports under the same names, unnamed ones
 * without a name; DllMain excluded) plus the private exports at 200-202, with
 * XInputGetState/XInputSetState at ordinals 2/3. When the system DLL cannot be
 * read, its documented export map is used instead.
 *
 * Each DOOM image argument must import XINPUT1_4.dll by ordinals 2 and 3 only. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EXPORTS 512

typedef struct export_row { unsigned ordinal; char name[64]; } export_row;
typedef struct image { uint8_t *base; size_t size; } image;

static int load_image(const char *path, image *out)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    memset(out, 0, sizeof *out);
    if (!f) return 0;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(file); return 0; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS64 *)(file + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { free(file); return 0; }
    out->size = nt->OptionalHeader.SizeOfImage;
    out->base = (uint8_t *)calloc(1, out->size);
    if (!out->base) { free(file); return 0; }
    memcpy(out->base, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (sec[i].Misc.VirtualSize && rs > sec[i].Misc.VirtualSize) rs = sec[i].Misc.VirtualSize;
        if (rs && va + rs <= out->size && po + rs <= (uint32_t)fsz) memcpy(out->base + va, file + po, rs);
    }
    free(file);
    return 1;
}

/* Every exported ordinal, with its name or "" when exported by ordinal only. */
static int read_exports(const image *im, export_row *rows, int cap)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)im->base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(im->base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *ex;
    DWORD *functions, *names;
    WORD *ordinals;
    int n = 0;
    if (!dir.VirtualAddress) return 0;
    ex = (IMAGE_EXPORT_DIRECTORY *)(im->base + dir.VirtualAddress);
    functions = (DWORD *)(im->base + ex->AddressOfFunctions);
    names = (DWORD *)(im->base + ex->AddressOfNames);
    ordinals = (WORD *)(im->base + ex->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ex->NumberOfFunctions && n < cap; i++) {
        if (!functions[i]) continue;
        rows[n].ordinal = ex->Base + i;
        rows[n].name[0] = 0;
        for (DWORD k = 0; k < ex->NumberOfNames; k++)
            if (ordinals[k] == i)
                strncpy_s(rows[n].name, sizeof rows[n].name, (const char *)(im->base + names[k]), _TRUNCATE);
        n++;
    }
    return n;
}

static const export_row *find_ordinal(const export_row *rows, int n, unsigned ordinal)
{
    for (int i = 0; i < n; i++) if (rows[i].ordinal == ordinal) return &rows[i];
    return NULL;
}

/* System32 XINPUT1_4.dll, Windows 11 26200; used only when it cannot be read. */
static const export_row k_xinput14[] = {
    {1, "DllMain"}, {2, "XInputGetState"}, {3, "XInputSetState"}, {4, "XInputGetCapabilities"},
    {5, "XInputEnable"}, {7, "XInputGetBatteryInformation"}, {8, "XInputGetKeystroke"},
    {10, "XInputGetAudioDeviceIds"}, {100, ""}, {101, ""}, {102, ""}, {103, ""}, {104, ""},
    {108, ""}, {109, ""}
};
static const export_row k_private[] = {
    {200, "sh_rawmap_swap_is_armed"}, {201, "sh_rawmap_swap_complete_count"}, {202, "sh_rawmap_swap_will_fire"}
};

static int check_proxy(const char *build_dir)
{
    char path[MAX_PATH];
    image im, sys;
    export_row ours[MAX_EXPORTS], rows[MAX_EXPORTS];
    const export_row *sys_rows = rows;
    int n_ours, n_sys, bad = 0, expected = 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\XINPUT1_4.dll", build_dir);
    if (!load_image(path, &im)) { printf("BAD cannot read %s\n", path); return 1; }
    n_ours = read_exports(&im, ours, MAX_EXPORTS);
    free(im.base);
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, sizeof path, "\\XINPUT1_4.dll");
    if (load_image(path, &sys)) {
        n_sys = read_exports(&sys, rows, MAX_EXPORTS);
        free(sys.base);
        printf("    system XINPUT1_4.dll: %d exports read from %s\n", n_sys, path);
    } else {
        sys_rows = k_xinput14;
        n_sys = (int)(sizeof k_xinput14 / sizeof k_xinput14[0]);
        printf("    system XINPUT1_4.dll unreadable; using the documented map (%d exports)\n", n_sys);
    }
    for (int i = 0; i < n_sys; i++) {
        const export_row *mine;
        if (!strcmp(sys_rows[i].name, "DllMain")) continue;
        expected++;
        mine = find_ordinal(ours, n_ours, sys_rows[i].ordinal);
        if (!mine || strcmp(mine->name, sys_rows[i].name)) {
            printf("BAD ordinal %u: system %s, proxy %s\n", sys_rows[i].ordinal,
                   sys_rows[i].name[0] ? sys_rows[i].name : "(unnamed)",
                   mine ? (mine->name[0] ? mine->name : "(unnamed)") : "(missing)");
            bad++;
        }
    }
    for (size_t i = 0; i < sizeof k_private / sizeof k_private[0]; i++) {
        const export_row *mine = find_ordinal(ours, n_ours, k_private[i].ordinal);
        expected++;
        if (!mine || strcmp(mine->name, k_private[i].name) || find_ordinal(sys_rows, n_sys, k_private[i].ordinal)) {
            printf("BAD private export %s at %u\n", k_private[i].name, k_private[i].ordinal); bad++;
        }
    }
    if (n_ours != expected) { printf("BAD proxy exports %d ordinals, expected %d\n", n_ours, expected); bad++; }
    if (!find_ordinal(ours, n_ours, 2) || strcmp(find_ordinal(ours, n_ours, 2)->name, "XInputGetState") ||
        !find_ordinal(ours, n_ours, 3) || strcmp(find_ordinal(ours, n_ours, 3)->name, "XInputSetState")) {
        printf("BAD ordinals 2/3 are not XInputGetState/XInputSetState\n"); bad++;
    }
    if (!bad) printf("OK  XINPUT1_4.dll exports the system ordinal set plus the private exports (%d)\n", n_ours);
    return bad;
}

/* The executable's XInput import: XINPUT1_4.dll, ordinals 2 and 3 only. */
static int check_doom_import(const char *path)
{
    image im;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_IMPORT_DESCRIPTOR *d;
    int bad = 0, found = 0;
    if (!load_image(path, &im)) { printf("BAD cannot read %s\n", path); return 1; }
    dos = (IMAGE_DOS_HEADER *)im.base;
    nt = (IMAGE_NT_HEADERS64 *)(im.base + dos->e_lfanew);
    d = (IMAGE_IMPORT_DESCRIPTOR *)(im.base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; d->Name; d++) {
        const char *name = (const char *)(im.base + d->Name);
        uint64_t *thunk;
        unsigned ords[8];
        int n = 0;
        if (_strnicmp(name, "XINPUT", 6)) continue;
        found++;
        if (_stricmp(name, "XINPUT1_4.dll")) { printf("BAD %s imports %s\n", path, name); bad++; }
        thunk = (uint64_t *)(im.base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; *thunk && n < 8; thunk++) {
            if (!(*thunk & IMAGE_ORDINAL_FLAG64)) { printf("BAD %s imports %s by name\n", path, name); bad++; break; }
            ords[n++] = (unsigned)IMAGE_ORDINAL64(*thunk);
        }
        if (n != 2 || ords[0] + ords[1] != 5 || (ords[0] != 2 && ords[0] != 3)) {
            printf("BAD %s imports %d ordinal(s) from %s, expected exactly 2 and 3\n", path, n, name); bad++;
        }
    }
    if (found != 1) { printf("BAD %s has %d XInput imports\n", path, found); bad++; }
    if (!bad) printf("OK  %s imports XINPUT1_4.dll ordinals 2 and 3 only\n", path);
    free(im.base);
    return bad;
}

int main(int argc, char **argv)
{
    int bad;
    if (argc < 2) { fprintf(stderr, "usage: xinput_exports_test <build-dir> [<DOOM image> ...]\n"); return 2; }
    bad = check_proxy(argv[1]);
    for (int i = 2; i < argc; i++) bad += check_doom_import(argv[i]);
    printf("RESULT: %s (%d problem(s))\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
