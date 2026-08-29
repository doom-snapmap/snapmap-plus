/* strip_dump.c -- run the production payload strip over a real map and write the result.
 *
 * A delivered map that carries its packages is stripped in the LOAD path, and a map that fails
 * after that point gives no way to see what the strip actually produced -- the bytes never reach
 * disk. This runs the same sh_mpkg_strip the engine runs, on a file, so the output can be parsed,
 * diffed and validated like any other artifact.
 *
 *     strip_dump <in.json> <out.json>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "map_package.h"

int sh_decl_server_registration_succeeded(void) { return 0; }
void sh_decl_server_request_rearm(void) { }

/* The strip narrates what it did through backend_log; print it, so a refusal explains itself
 * instead of just returning NULL. */
void backend_log(const char *line) { printf("[log] %s\n", line ? line : "(null)"); }

int main(int argc, char **argv)
{
    FILE *f;
    char *buf, *out;
    long n;
    size_t out_len = 0;

    if (argc < 3) { fprintf(stderr, "usage: strip_dump <in.json> <out.json>\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 2; }
    buf[n] = 0; fclose(f);

    out = sh_mpkg_strip(buf, (size_t)n, &out_len);
    if (!out) { printf("strip returned NULL (nothing removed or refused)\n"); return 1; }
    printf("stripped %ld -> %zu bytes\n", n, out_len);

    f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    fwrite(out, 1, out_len, f);
    fclose(f);
    printf("wrote %s\n", argv[2]);
    return 0;
}
