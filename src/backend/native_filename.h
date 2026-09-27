/* Filesystem GenerateFilename contract, shared by shader and particle data. */
#ifndef SH_NATIVE_FILENAME_H
#define SH_NATIVE_FILENAME_H
#include <stddef.h>
#include <string.h>

/* Native virtual-filesystem opens may start at its root, including shader
 * includes. Return a borrowed catalog-relative spelling, not a host filesystem
 * path. Authored package members still use their separate strict validation. */
static inline const char *sh_native_virtual_path(const char *path)
{
    if (path) while (*path == '/' || *path == '\\') path++;
    return path;
}

/* Inputs are already normalized for their native consumer. Unlike idResource's
 * generated-path routine, this does not sanitize punctuation, preserve options
 * before replacing an extension, or bypass the prefix for a cooked extension.
 * The caller supplies a prefix ending in a slash and a dot-prefixed suffix. */
static int sh_native_generated_filename(const char *prefix, const char *name,
    const char *suffix, char *out, size_t capacity)
{
    const char *slash, *dot, *base;
    size_t directory, count, prefix_length, suffix_length, separators, at, original_count;
    if (out && capacity) *out = 0;
    if (!prefix || !name || !*name || !suffix || *suffix != '.' || !out || !capacity) return 0;
    slash = strrchr(name, '/'); dot = strrchr(name, '.'); base = slash ? slash + 1 : name;
    directory = (size_t)(base - name);
    count = dot && dot >= base ? (size_t)(dot - base) : strlen(base);
    if (!*base) return 0;
    prefix_length = strlen(prefix); suffix_length = strlen(suffix);
    if (!prefix_length || prefix[prefix_length - 1] != '/') return 0;
    /* Native measures the basename after replacing the extension. */
    separators = !count || (count < 38 && suffix_length < 38 - count) ? 0 : (count - 1) / 30;
    original_count = count;
    if (separators) {
        /* The long branch applies SetFileExtension again after partitioning.
         * A second dot in its final component is stripped too; dots in earlier
         * components remain directory text. Match that native spelling exactly. */
        for (size_t i = count; i > separators * 30; i--)
            if (base[i - 1] == '.') { count = i - 1; break; }
    }
    if (prefix_length >= capacity || directory >= capacity - prefix_length ||
        count >= capacity - prefix_length - directory ||
        separators >= capacity - prefix_length - directory - count ||
        suffix_length >= capacity - prefix_length - directory - count - separators) return 0;
    memcpy(out, prefix, prefix_length); at = prefix_length;
    memcpy(out + at, name, directory); at += directory;
    for (size_t i = 0; i < count; i++) {
        if (separators && i && i % 30 == 0) out[at++] = '/';
        out[at++] = base[i];
    }
    if (separators && count < original_count && count == separators * 30) out[at++] = '/';
    memcpy(out + at, suffix, suffix_length + 1); return 1;
}
#endif
