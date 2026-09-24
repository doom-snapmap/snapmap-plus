#include "../src/backend/audio_effective_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytes { unsigned char data[1024]; size_t length; } bytes;
static void put(bytes *b, uint32_t value, unsigned n)
{ for (unsigned i = 0; i < n; i++) { assert(b->length < sizeof(b->data)); b->data[b->length++] = (unsigned char)(value >> (8 * i)); } }
static void stmg(bytes *b, float threshold, uint16_t voices, uint32_t group, uint32_t duration, uint32_t extra_group)
{
    memset(b, 0, sizeof(*b)); memcpy(b->data, &threshold, 4); b->length = 4; put(b, voices, 2);
    put(b, extra_group ? 2 : 1, 4); put(b, group, 4); put(b, duration, 4); put(b, 1, 4);
    put(b, 1, 4); put(b, 2, 4); put(b, 50, 4);
    if (extra_group) { put(b, extra_group, 4); put(b, 70, 4); put(b, 0, 4); }
    put(b, 0, 4); put(b, 1, 4); put(b, 30, 4); put(b, 0x3f000000u, 4); put(b, 0, 4); put(b, 0, 4); put(b, 0, 4); put(b, 1, 1);
}
static sh_audio_shared_policy program(const bytes *baseline, const bytes *authored)
{
    sh_audio_shared_input inputs[2] = {{"STMG", baseline->data, baseline->length, 0, 1},
        {"STMG", authored ? authored->data : NULL, authored ? authored->length : 0, 1, 0}};
    sh_audio_shared_policy p = {0}; char error[256];
    if (!sh_audio_shared_policy_compile(inputs, authored ? 2 : 1, &p, error, sizeof(error))) { fprintf(stderr, "%s\n", error); assert(0); }
    return p;
}
static const sh_audio_shared_setting *find(const sh_audio_shared_policy *p, sh_audio_shared_kind kind, uint32_t key)
{
    for (size_t i = 0; i < p->count; i++) if (p->settings[i].record.kind == kind && p->settings[i].record.key[0] == key)
        return p->settings + i;
    return NULL;
}
static uint32_t word(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void sorted(const sh_audio_shared_policy *p)
{
    for (size_t i = 1; i < p->count; i++) {
        const sh_audio_shared_record *a = &p->settings[i - 1].record, *b = &p->settings[i].record;
        int order = a->kind != b->kind ? (a->kind < b->kind ? -1 : 1) : 0;
        for (size_t k = 0; !order && k < 3; k++) if (a->key[k] != b->key[k]) order = a->key[k] < b->key[k] ? -1 : 1;
        assert(order < 0);
    }
}
int main(void)
{
    bytes base, map, library;
    stmg(&base, -60, 64, 10, 100, 0);
    stmg(&map, -60, 32, 10, 200, 0);        /* map edits group 10; fewer voices */
    stmg(&library, -80, 128, 10, 300, 11);  /* library edits 10, adds 11, limits */
    sh_audio_shared_policy baseline = program(&base, NULL), m = program(&base, &map), l = program(&base, &library);
    const sh_audio_shared_policy *views[2] = {&m, &l};
    sh_audio_shared_policy e = {0}; char error[256];
    assert(sh_audio_effective_shared(&baseline, views, 2, &e, error, sizeof(error)));
    sorted(&e);
    const sh_audio_shared_setting *g10 = find(&e, SH_AUDIO_SHARED_GROUP, 10), *g11 = find(&e, SH_AUDIO_SHARED_GROUP, 11);
    /* Map precedence for the overlapping key; the library's other key survives. */
    assert(g10 && word(g10->body + 4) == 200 && g10->edited && g10->input == 1);
    assert(g11 && word(g11->body + 4) == 70 && g11->edited && g11->input == 2);
    /* Limits compose permissively across programs and the baseline. */
    const sh_audio_shared_setting *voices = find(&e, SH_AUDIO_SHARED_VOICES, 0), *threshold = find(&e, SH_AUDIO_SHARED_THRESHOLD, 0);
    assert(voices && voices->body[0] == 128 && voices->edited);
    float t; memcpy(&t, threshold->body, 4); assert(t == -80 && threshold->edited);
    /* Unedited keys stay native. */
    assert(!find(&e, SH_AUDIO_SHARED_TRANSITION, 10)->edited && !find(&e, SH_AUDIO_SHARED_RTPC, 30)->edited);
    assert(sh_audio_effective_shared_edited(&e)); sh_audio_shared_policy_free(&e);
    /* Precedence reversed: the library's value wins group 10. */
    const sh_audio_shared_policy *reversed[2] = {&l, &m};
    assert(sh_audio_effective_shared(&baseline, reversed, 2, &e, error, sizeof(error)));
    assert(word(find(&e, SH_AUDIO_SHARED_GROUP, 10)->body + 4) == 300); sh_audio_shared_policy_free(&e);
    /* Leaving every program restores the baseline values, all unedited. */
    assert(sh_audio_effective_shared(&baseline, NULL, 0, &e, error, sizeof(error)));
    assert(e.count == baseline.count && !sh_audio_effective_shared_edited(&e));
    assert(word(find(&e, SH_AUDIO_SHARED_GROUP, 10)->body + 4) == 100); sh_audio_shared_policy_free(&e);
    /* A program without edits for a key leaves the lower-precedence edit. */
    sh_audio_shared_policy plain = program(&base, NULL);
    const sh_audio_shared_policy *mixed[2] = {&plain, &l};
    assert(sh_audio_effective_shared(&baseline, mixed, 2, &e, error, sizeof(error)));
    assert(word(find(&e, SH_AUDIO_SHARED_GROUP, 10)->body + 4) == 300); sh_audio_shared_policy_free(&e);
    /* Keys only a program introduces are edits even without a baseline. */
    assert(sh_audio_effective_shared(NULL, views, 2, &e, error, sizeof(error)) && find(&e, SH_AUDIO_SHARED_GROUP, 11)->edited);
    sh_audio_shared_policy_free(&e);
    /* Platform labels must agree across programs. */
    unsigned char linux_label[9] = {5, 0, 0, 0, 'L', 'i', 'n', 'u', 'x'}, windows_label[11] = {7, 0, 0, 0, 'W', 'i', 'n', 'd', 'o', 'w', 's'};
    sh_audio_shared_input a_in = {"PLAT", windows_label, sizeof(windows_label), 0, 1}, b_in = {"PLAT", linux_label, sizeof(linux_label), 1, 0};
    sh_audio_shared_policy pa = {0}, pb = {0};
    assert(sh_audio_shared_policy_compile(&a_in, 1, &pa, error, sizeof(error)) && sh_audio_shared_policy_compile(&b_in, 1, &pb, error, sizeof(error)));
    const sh_audio_shared_policy *platforms[1] = {&pb};
    assert(!sh_audio_effective_shared(&pa, platforms, 1, &e, error, sizeof(error)) && !e.count && strstr(error, "platform"));
    const sh_audio_shared_policy *missing[1] = {NULL};
    assert(!sh_audio_effective_shared(&baseline, missing, 1, &e, error, sizeof(error)));
    sh_audio_shared_policy_free(&pa); sh_audio_shared_policy_free(&pb); sh_audio_shared_policy_free(&plain);
    sh_audio_shared_policy_free(&baseline); sh_audio_shared_policy_free(&m); sh_audio_shared_policy_free(&l);
    puts("effective audio policy checks passed"); return 0;
}
