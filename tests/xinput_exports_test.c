/* Static check of the backend proxy's export table.
 *
 *   xinput_exports_test <build-dir> [<DOOM image> ...]
 *
 * build-dir must hold XINPUT1_4.dll. Its exports must be exactly the System32
 * XINPUT1_4.dll ordinal set (named exports under the same names, unnamed ones
 * without a name; DllMain excluded), or for a diagnostic build (build.ps1
 * -Diag) that set plus exactly the diagnostic exports, and nothing else, with
 * XInputGetState/XInputSetState at ordinals 2/3. When the system DLL cannot be
 * read, its documented export map is used instead. The diagnostic exports are
 * the entries of src/backend/xinput1_4_diag.def (beside build-dir's parent)
 * that xinput1_4.def lacks; when those files cannot be read, k_diag below.
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

/* What build.ps1 -Diag adds: mirrors src/backend/xinput1_4_diag.def's lines
 * after xinput1_4.def's (tests/diag_status_test.c checks that pair); used only
 * when the .def files cannot be read. */
static const export_row k_diag[] = {
    {300, "sh_diag_status"}, {301, "sh_diag_note"}, {302, "sh_diag_note_listening"}, {303, "sh_diag_hold_entity"}
};

/* The EXPORTS entries of a .def file ("name @ordinal [NONAME] [DATA]"); -1 unreadable. */
static int read_def(const char *path, export_row *rows, int cap)
{
    FILE *f = NULL;
    char line[256], name[64], rest[64];
    unsigned ordinal;
    int n = 0, in_exports = 0;
    if (fopen_s(&f, path, "r") || !f) return -1;
    while (fgets(line, sizeof line, f) && n < cap) {
        int got;
        line[strcspn(line, "\r\n;")] = 0;
        rest[0] = 0;
        if (sscanf_s(line, " %63s", name, (unsigned)sizeof name) != 1) continue;
        if (!strcmp(name, "EXPORTS")) { in_exports = 1; continue; }
        got = sscanf_s(line, " %63s @%u %63s", name, (unsigned)sizeof name, &ordinal, rest, (unsigned)sizeof rest);
        if (!in_exports || got < 2) continue;
        rows[n].ordinal = ordinal;
        strcpy_s(rows[n].name, sizeof rows[n].name, strcmp(rest, "NONAME") ? name : "");
        n++;
    }
    fclose(f);
    return n;
}

/* The diagnostic exports: xinput1_4_diag.def's entries whose ordinal
 * xinput1_4.def does not export. */
static int diag_exports(const char *build_dir, export_row *out, int cap)
{
    char path[MAX_PATH];
    export_row rel[64], diag[64];
    int n_rel, n_diag, n = 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\..\\src\\backend\\xinput1_4.def", build_dir);
    n_rel = read_def(path, rel, 64);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\..\\src\\backend\\xinput1_4_diag.def", build_dir);
    n_diag = read_def(path, diag, 64);
    if (n_rel <= 0 || n_diag <= n_rel) {
        n = (int)(sizeof k_diag / sizeof k_diag[0]);
        memcpy(out, k_diag, sizeof k_diag);
        printf("    diagnostic exports: .def files unreadable; using the mirrored list (%d)\n", n);
        return n;
    }
    for (int i = 0; i < n_diag && n < cap; i++)
        if (!find_ordinal(rel, n_rel, diag[i].ordinal)) out[n++] = diag[i];
    printf("    diagnostic exports: %d read from %s\n", n, path);
    return n;
}

#define SHAPE_RELEASE 0
#define SHAPE_DIAG    1

/* Problems with `ours` against the system set `sys` (DllMain excluded) and
 * the diagnostic set `diag`: every system ordinal under its name, any other
 * export one of the diagnostic set at its ordinal and name, and either none
 * or all of that set. *shape says which of the two shapes was found. */
static int compare_exports(const export_row *ours, int n_ours, const export_row *sys, int n_sys,
                           const export_row *diag, int n_diag, int verbose, int *shape)
{
    int bad = 0, extras = 0;
    const export_row *r2 = find_ordinal(ours, n_ours, 2), *r3 = find_ordinal(ours, n_ours, 3);
    for (int i = 0; i < n_sys; i++) {
        const export_row *mine;
        if (!strcmp(sys[i].name, "DllMain")) continue;
        mine = find_ordinal(ours, n_ours, sys[i].ordinal);
        if (!mine || strcmp(mine->name, sys[i].name)) {
            if (verbose)
                printf("BAD ordinal %u: system %s, proxy %s\n", sys[i].ordinal, sys[i].name[0] ? sys[i].name : "(unnamed)",
                       mine ? (mine->name[0] ? mine->name : "(unnamed)") : "(missing)");
            bad++;
        }
    }
    for (int i = 0; i < n_ours; i++) {
        const export_row *s = find_ordinal(sys, n_sys, ours[i].ordinal), *d;
        if (s && strcmp(s->name, "DllMain")) continue;
        d = find_ordinal(diag, n_diag, ours[i].ordinal);
        if (d && !strcmp(d->name, ours[i].name)) { extras++; continue; }
        if (verbose)
            printf("BAD proxy exports ordinal %u (%s), neither a system nor a diagnostic export\n", ours[i].ordinal,
                   ours[i].name[0] ? ours[i].name : "unnamed");
        bad++;
    }
    if (extras && extras != n_diag) {
        if (verbose) printf("BAD proxy exports %d of the %d diagnostic exports\n", extras, n_diag);
        bad++;
    }
    if (!r2 || strcmp(r2->name, "XInputGetState") || !r3 || strcmp(r3->name, "XInputSetState")) {
        if (verbose) printf("BAD ordinals 2/3 are not XInputGetState/XInputSetState\n");
        bad++;
    }
    *shape = extras ? SHAPE_DIAG : SHAPE_RELEASE;
    return bad;
}

/* compare_exports on made-up tables: both accepted shapes, and each rejection. */
static int self_check(void)
{
    const int n_sys = (int)(sizeof k_xinput14 / sizeof k_xinput14[0]), n_diag = (int)(sizeof k_diag / sizeof k_diag[0]);
    export_row t[64];
    int n = 0, shape, bad = 0;
    for (int i = 0; i < n_sys; i++) if (strcmp(k_xinput14[i].name, "DllMain")) t[n++] = k_xinput14[i];
    bad += compare_exports(t, n, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) != 0 || shape != SHAPE_RELEASE;
    memcpy(t + n, k_diag, sizeof k_diag);
    bad += compare_exports(t, n + n_diag, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) != 0 || shape != SHAPE_DIAG;
    bad += compare_exports(t, n + n_diag - 1, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) == 0;   /* part of the set */
    t[n].ordinal = 400; strcpy_s(t[n].name, sizeof t[n].name, "sh_unknown");
    bad += compare_exports(t, n + 1, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) == 0;            /* an unknown export */
    t[n] = k_diag[0]; t[n].ordinal = 304;
    bad += compare_exports(t, n + 1, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) == 0;            /* a diagnostic name elsewhere */
    t[n].ordinal = 1; strcpy_s(t[n].name, sizeof t[n].name, "DllMain");
    bad += compare_exports(t, n + 1, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) == 0;            /* DllMain exported */
    bad += compare_exports(t, n - 1, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) == 0;        /* a system export missing */
    strcpy_s(t[0].name, sizeof t[0].name, "XInputSetState"); strcpy_s(t[1].name, sizeof t[1].name, "XInputGetState");
    bad += compare_exports(t, n, k_xinput14, n_sys, k_diag, n_diag, 0, &shape) == 0;                /* 2/3 swapped */
    if (bad) printf("BAD the export comparison failed %d of its own cases\n", bad);
    else printf("OK  the export comparison accepts both shapes and rejects its 6 bad cases\n");
    return bad;
}

static int check_proxy(const char *build_dir)
{
    char path[MAX_PATH];
    image im, sys;
    export_row ours[MAX_EXPORTS], rows[MAX_EXPORTS], diag[64];
    const export_row *sys_rows = rows;
    int n_ours, n_sys, n_diag, bad, shape;
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
    n_diag = diag_exports(build_dir, diag, 64);
    bad = compare_exports(ours, n_ours, sys_rows, n_sys, diag, n_diag, 1, &shape);
    if (!bad && shape == SHAPE_RELEASE)
        printf("OK  XINPUT1_4.dll exports exactly the system ordinal set (%d): a release build\n", n_ours);
    else if (!bad)
        printf("OK  XINPUT1_4.dll exports the system ordinal set plus the %d diagnostic exports (%d): a -Diag build\n",
               n_diag, n_ours);
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
    bad = self_check();
    bad += check_proxy(argv[1]);
    for (int i = 2; i < argc; i++) bad += check_doom_import(argv[i]);
    printf("RESULT: %s (%d problem(s))\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
