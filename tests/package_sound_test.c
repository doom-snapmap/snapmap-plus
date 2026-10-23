#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_sound.h"
#include "audio_banks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static sh_package_compilation *alias(const sh_package_sources *sources, const char *authored,
    const char *native, const char *body)
{
    char original[320], path[320], error[512];
    snprintf(original, sizeof(original), "generated/decls/sound/%s.decl", authored);
    snprintf(path, sizeof(path), "generated/decls/sound/%s.decl", native);
    sh_compiled_resource input = {0};
    input.engine_path = original; input.type = "sound"; input.name = (char *)authored;
    input.body = (unsigned char *)body; input.body_length = strlen(body); input.source = SIZE_MAX;
    const sh_compiled_resource *first = &input;
    sh_package_resource_projection row = {path, &first, 1, NULL, 0, "sound", native};
    sh_package_compilation *compiled = sh_package_compilation_project(sources, &row, 1,
        NULL, NULL, error, sizeof(error));
    CHECK(compiled != NULL);
    CHECK(compiled && sh_package_sound_prepare(compiled, error, sizeof(error)));
    return compiled;
}

static void association_and_copies(void)
{
    sh_package_sources sources = {0}; char error[512]; uint32_t expected = 0, native = 0, event = 0;
    sh_package_compilation *a = alias(&sources, "play_sample", "smp/sound_a", "{ value = 1; }");
    sh_package_compilation *b = alias(&sources, "play_sample", "smp/sound_b", "{ value = 2; }");
    CHECK(sh_audio_event_identity("play_sample", &expected));
    CHECK(sh_audio_event_identity("smp/sound_a", &native) && native != expected);
    CHECK(a && b && sh_package_sound_requires_binding(a));
    if (!a || !b) goto done;
    CHECK(sh_package_sound_event(a, "smp/sound_a", &event) && event == expected);
    CHECK(!sh_package_sound_event(a, "smp/sound_b", &event) && !event);
    CHECK(sh_package_sound_event(b, "smp/sound_b", &event) && event == expected);
    CHECK(!sh_package_sound_event(a, "play_sample", &event) && !event);
    CHECK(!strcmp(a->resources[0].original_path, "generated/decls/sound/play_sample.decl"));
    CHECK(!strcmp((const char *)a->resources[0].body, "{ value = 1; }"));

    sh_package_compilation *view = sh_package_compilation_overlay(a, b, error, sizeof(error));
    CHECK(view && view->resource_count == 2);
    CHECK(view && sh_package_sound_event(view, "smp/sound_a", &event) && event == expected);
    CHECK(view && sh_package_sound_event(view, "smp/sound_b", &event) && event == expected);
    sh_package_compilation_free(view);

    const sh_compiled_resource *input = &a->resources[0];
    sh_package_resource_projection row = {input->engine_path, &input, 1};
    sh_package_compilation *retained = sh_package_compilation_project(&sources, &row, 1,
        NULL, NULL, error, sizeof(error));
    CHECK(retained && retained->resources[0].sound_event == expected);
    CHECK(retained && !sh_package_sound_rebind(retained->resources, expected + 1, 13));
    CHECK(retained && sh_package_sound_rebind(retained->resources, expected, 13));
    CHECK(retained && sh_package_sound_prepare(retained, error, sizeof(error)) &&
        retained->resources[0].sound_event == 13);
    sh_package_changes changes = {0};
    CHECK(retained && sh_package_compilation_changes(a, retained, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && changes.items[0].kind == SH_PACKAGE_RESOURCE_REPLACED);
    sh_package_changes_free(&changes);
    /* Equal bytes and original paths cannot coalesce different native events. */
    const sh_compiled_resource *mixed[] = {input, retained ? retained->resources : input};
    row.inputs = mixed; row.input_count = 2;
    sh_package_compilation *bad = sh_package_compilation_project(&sources, &row, 1,
        NULL, NULL, error, sizeof(error));
    CHECK(!bad); sh_package_compilation_free(bad);
    sh_package_compilation_free(retained);
done:
    sh_package_compilation_free(a); sh_package_compilation_free(b);
}

static void conversion_and_atomic_failure(void)
{
    sh_package_sources sources = {0}; char error[512]; uint32_t expected = 0, event = 0;
    sh_package_compilation *flat = alias(&sources, "stop_sample", "stop_sample", "{}");
    CHECK(flat && !sh_package_sound_requires_binding(flat));
    CHECK(sh_audio_event_identity("stop_sample", &expected));
    CHECK(flat && sh_package_sound_event(flat, "stop_sample", &event) && event == expected);
    sh_package_compilation_free(flat);
    flat = alias(&sources, "sounds/folder/sample.wav", "smp/path", "{}");
    CHECK(sh_audio_event_identity("play_sample", &expected));
    CHECK(flat && sh_package_sound_event(flat, "smp/path", &event) && event == expected);
    sh_package_compilation_free(flat);

    sh_compiled_resource rows[2] = {0}; sh_package_compilation compilation = {0};
    rows[0].engine_path = "generated/decls/sound/play_valid.decl";
    rows[0].type = "sound"; rows[0].name = "play_valid";
    rows[1].engine_path = "generated/decls/sound/smp/other.decl";
    rows[1].type = "sound"; rows[1].name = "smp/other";
    rows[1].original_path = "generated/decls/material/other.decl";
    compilation.resources = rows; compilation.resource_count = 2;
    CHECK(!sh_package_sound_prepare(&compilation, error, sizeof(error)));
    CHECK(!rows[0].sound_event && !rows[0].sound_authored_event);
    rows[1].original_path = NULL; rows[1].type = "material";
    rows[1].sound_event = rows[1].sound_authored_event = 12;
    CHECK(!sh_package_sound_prepare(&compilation, error, sizeof(error)));
    CHECK(!rows[0].sound_event);
    rows[1].type = "sound"; rows[1].sound_authored_event = 0;
    CHECK(!sh_package_sound_prepare(&compilation, error, sizeof(error)));
    rows[1].sound_event = 0;
    CHECK(sh_package_sound_prepare(&compilation, error, sizeof(error)));
    CHECK(rows[0].sound_event && rows[1].sound_event);
    uint32_t first = rows[0].sound_event;
    rows[0].sound_authored_event++;
    CHECK(!sh_package_sound_prepare(&compilation, error, sizeof(error)) && rows[0].sound_event == first);
}

int main(void)
{
    association_and_copies(); conversion_and_atomic_failure();
    printf("package_sound_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
