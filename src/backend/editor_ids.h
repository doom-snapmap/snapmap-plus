#ifndef SNAPMAP_PLUS_EDITOR_IDS_H
#define SNAPMAP_PLUS_EDITOR_IDS_H

#include <stdio.h>
#include <string.h>
#include "sha256.h"
#include "strkeys.h"

static inline int sh_editor_id_valid(const char *id)
{
    if (!id || !*id || *id == '/' || strstr(id, "..") || strstr(id, "//")) return 0;
    for (const char *p = id; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') &&
            c != '_' && c != '.' && c != '-' && c != '/') return 0;
        if (c == '/' && !p[1]) return 0;
    }
    return 1;
}

static inline int sh_editor_string_tag(const char *salt, const char *id, char *out, size_t cap)
{
    const char *key = id && !strncmp(id, "#str_", 5) ? id + 5 : id;
    if (!sh_editor_id_valid(key) || !salt) return 0;
    int known = sh_strkey_is_edit(key, strlen(key)) ||
                (!strncmp(key, salt, strlen(salt)) && key[strlen(salt)] == '_');
    int n = known ? snprintf(out, cap, "#str_%s", key) : snprintf(out, cap, "#str_%s_%s", salt, key);
    return n >= 0 && (size_t)n < cap;
}

/* Shared tabs use the same native key in every pack and language. */
static inline int sh_editor_tab_tag(const char *salt, const char *id, char *out, size_t cap)
{
    if (!id || !salt) return 0;
    const char *tag = !strcmp(id, "snapmap-plus/custom") ? "#str_sh_category_1" : id;
    if (!strncmp(tag, "#str_", 5)) {
        if (!sh_editor_id_valid(tag + 5) || strlen(tag) >= cap) return 0;
        memcpy(out, tag, strlen(tag) + 1);
        return 1;
    }
    if (!sh_editor_id_valid(id)) return 0;
    sh_sha256 hash;
    char digest[65];
    sh_sha256_init(&hash);
    sh_sha256_update(&hash, id, strlen(id));
    sh_sha256_hex(&hash, digest, 64);
    int n = snprintf(out, cap, "#str_%s_tab_%s", !strncmp(id, "shared/", 7) ? "smp" : salt, digest);
    return n >= 0 && (size_t)n < cap;
}
#endif
