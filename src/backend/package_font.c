#include "package_font.h"
#include <stdio.h>
#include <string.h>

static const char *const pf_slots[] = {
    "font/material/48", "font/material/64_df", "font/material/12",
    "font/worldText", "font/snapLabel"
};
static int pf_fail(char *error, size_t capacity, const char *message)
{ if (error && capacity && !*error) snprintf(error, capacity, "Font constructor: %s", message); return 0; }
static unsigned char pf_lower(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c; }
static int pf_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && pf_lower((unsigned char)*a) == pf_lower((unsigned char)*b)) { a++; b++; }
    return *a == *b;
}
static int pf_request(const char *request, const char *prefix, const char *font, const char *suffix)
{
    if (!request || !font || !*font) return 0;
    while (*prefix) if (pf_lower((unsigned char)*request++) != (unsigned char)*prefix++) return 0;
    while (*font) {
        unsigned char c = (unsigned char)*font++;
        if (c == ' ' || c == '-') c = '_';
        if (pf_lower((unsigned char)*request++) != pf_lower(c)) return 0;
    }
    return pf_equal(request, suffix);
}
int sh_package_font_plan(const sh_package_consumer *owner, const char *materials[SH_PACKAGE_FONT_MATERIAL_COUNT])
{
    if (!materials) return 0;
    memset(materials, 0, sizeof(*materials) * SH_PACKAGE_FONT_MATERIAL_COUNT);
    if (!owner || !owner->family || strcmp(owner->family, "font") || !owner->name || !*owner->name ||
        !owner->targets || !owner->count || owner->count > SH_PACKAGE_FONT_MATERIAL_COUNT) return 0;
    for (size_t i = 0; i < owner->count; i++) {
        const sh_package_consumer_target *target = owner->targets + i; size_t slot = 0;
        if (!target->slot || !target->family || strcmp(target->family, "material") || !target->name || !*target->name) goto failed;
        while (slot < SH_PACKAGE_FONT_MATERIAL_COUNT && strcmp(target->slot, pf_slots[slot])) slot++;
        if (slot == SH_PACKAGE_FONT_MATERIAL_COUNT || materials[slot]) goto failed;
        materials[slot] = target->name;
    }
    if (materials[0] || materials[1] || materials[2]) return 1;
failed:
    memset(materials, 0, sizeof(*materials) * SH_PACKAGE_FONT_MATERIAL_COUNT); return 0;
}
int sh_package_font_capture(const char *native_name, const sh_package_font_member *members,
    size_t count, int (*visit)(void *, const char *, const char *, int), void *context, char *error, size_t capacity)
{
    sh_font_binary_paths paths[3] = {0};
    unsigned seen = 0, present = 0; int ok = 0;
    if (error && capacity) *error = 0;
    if (!native_name || !*native_name || !members || !count || count > 3 || !visit)
        return pf_fail(error, capacity, "requires a native font name and complete captured glyph forms");
    for (unsigned i = 0; i < 3; i++)
        if (!sh_font_binary_paths_for(native_name, (sh_font_binary_variant)i, paths + i)) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_package_font_member *member = members + i; unsigned form;
        for (form = 0; form < 3 && !pf_equal(member->path, paths[form].file); form++) {}
        if (form == 3 || (seen & (1u << form))) {
            pf_fail(error, capacity, "glyph members repeat or do not match the native font name"); goto done;
        }
        seen |= 1u << form;
        /* Missing and empty selected files both reach native 12.dat fallback.
         * An empty member is not a malformed header and has no own material. */
        if (!member->length) continue;
        if (!sh_font_binary_read(member->body, member->length, NULL, error, capacity)) goto done;
        present |= 1u << form;
    }
    if (!present || ((seen & ~present & 3u) && !(present & (1u << SH_FONT_BITMAP_12)))) {
        pf_fail(error, capacity, "empty glyph forms require nonempty 12.dat fallback"); goto done;
    }
    for (unsigned i = 0; i < 3; i++) if ((present & (1u << i)) && visit(context, pf_slots[i], paths[i].material, 0) != 1) {
        pf_fail(error, capacity, "glyph material could not be captured in its source context"); goto done;
    }
    for (unsigned i = 3; i < 5; i++) {
        int status = visit(context, pf_slots[i], i == 3 ? paths[0].world_text : paths[0].snap_label, 1);
        if (status != 0 && status != 1) {
            pf_fail(error, capacity, "optional material availability could not be established"); goto done;
        }
    }
    ok = 1;
done:
    for (unsigned i = 0; i < 3; i++) sh_font_binary_paths_clear(paths + i);
    return ok || pf_fail(error, capacity, "cannot capture native font requests");
}
int sh_package_font_route(const sh_package_consumer *owner, const char *family,
    const char *request, const char **replacement)
{
    static const char *const suffixes[] = {"/48.tga", "/64_df.tga", "/12.tga", "_worldText", "_snapLabel"};
    if (replacement) *replacement = NULL;
    if (!replacement || !family || !request) return -1;
    if (!owner || !owner->family || strcmp(owner->family, "font") || !pf_equal(family, "material")) return 0;
    if (!owner->name || !*owner->name) return -1;
    for (unsigned i = 0; i < 5; i++) if (pf_request(request, i < 3 ? "fonts/" : "snapmap/", owner->name, suffixes[i])) {
        const sh_package_consumer_target *target = sh_package_consumer_find(owner, pf_slots[i]);
        if (!target) return i < 3 ? -1 : 2;
        if (!target->family || strcmp(target->family, "material") || !target->name || !*target->name) return -1;
        *replacement = target->name; return 1;
    }
    return 0;
}
