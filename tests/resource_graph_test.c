#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "resource_graph.h"

static int failures, files, materials, pruned;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
static int abort_visit(void *context, const char *parent_type, const char *parent_name,
                       const char *type, const char *name)
{
    (void)context; (void)parent_type; (void)parent_name; (void)type; (void)name;
    return -1;
}
static int visit(void *context, const char *parent_type, const char *parent_name, const char *type, const char *name)
{
    (void)context; (void)parent_type; (void)parent_name;
    if (!strcmp(type, "snapeditorentity")) { pruned++; return 0; }
    if (!*type) {
        CHECK(!strcmp(name, "generated/image/body.bimage") || !strcmp(name, "generated/image/revised.bimage")); files++;
    }
    if (!strcmp(type, "material")) materials++;
    return 1;
}
static DWORD WINAPI reader(LPVOID context)
{
    const char *name = (const char *)context;
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, "material", name);
    sh_resource_graph_file("generated/image/body.bimage");
    sh_resource_graph_end(&frame, 1);
    return 0;
}
static int phase_visit(void *context, const char *parent_type, const char *parent_name, const char *type, const char *name)
{
    unsigned *seen = (unsigned *)context;
    (void)parent_type; (void)parent_name; (void)type;
    if (!strcmp(name, "source")) *seen |= 1;
    else if (!strcmp(name, "model")) *seen |= 2;
    else if (!strcmp(name, "revised")) *seen |= 4;
    else if (!strcmp(name, "map-edit")) *seen |= 8;
    else if (!strcmp(name, "isolated")) *seen |= 16;
    return 1;
}
static unsigned phase_walk(int expected)
{
    unsigned seen = 0;
    CHECK(sh_resource_graph_walk("entitydef", "phases", phase_visit, &seen) == expected);
    return seen;
}
static void phase_checks(void)
{
    sh_resource_graph_frame source, state, paused, child;
    sh_resource_graph_begin(&source, "entityDef", "phases");
    sh_resource_graph_file("source");
    sh_resource_graph_end(&source, 1);
    (void)phase_walk(0); /* Parsing alone does not cover deferred gameplay state. */
    sh_resource_graph_begin_state(&state, "entityDef", "phases", 0);
    sh_resource_graph_file("model");
    sh_resource_graph_pause(&paused);
    CHECK(!sh_resource_graph_recording());
    sh_resource_graph_file("map-edit");
    sh_resource_graph_begin(&child, "material", "isolated");
    sh_resource_graph_file("map-edit");
    sh_resource_graph_end(&child, 1);
    CHECK(!sh_resource_graph_recording());
    sh_resource_graph_end(&paused, 1);
    CHECK(sh_resource_graph_recording());
    sh_resource_graph_end(&state, 1);
    CHECK(phase_walk(1) == 3); /* Both phases, with no instance contamination. */
    {
        unsigned seen = 0;
        CHECK(sh_resource_graph_walk("material", "isolated", phase_visit, &seen));
        CHECK(seen == (8 | 16)); /* Nested source parse still has its own graph. */
    }
    sh_resource_graph_begin_state(&state, "entitydef", "phases", 0);
    sh_resource_graph_file("revised");
    sh_resource_graph_end(&state, 1);
    CHECK(phase_walk(1) == 5); /* State replacement retains source edges. */
    sh_resource_graph_begin_state(&state, "entitydef", "phases", 0);
    sh_resource_graph_file("model");
    sh_resource_graph_begin(&source, "entitydef", "phases");
    sh_resource_graph_file("source");
    sh_resource_graph_end(&source, 1);
    sh_resource_graph_end(&state, 1); /* This result belongs to the old source. */
    (void)phase_walk(0);
    sh_resource_graph_begin_state(&state, "entitydef", "phases", 0);
    sh_resource_graph_file("revised");
    sh_resource_graph_end(&state, 1);
    CHECK(phase_walk(1) == 5);
    sh_resource_graph_begin_state(&state, "entitydef", "phases", 0);
    sh_resource_graph_file("model");
    sh_resource_graph_end(&state, 0);
    (void)phase_walk(0); /* A failed state refresh never reuses an old success. */
    sh_resource_graph_begin(&source, "entitydef", "phases");
    sh_resource_graph_end(&source, 0);
    sh_resource_graph_begin_state(&state, "entitydef", "phases", 0);
    sh_resource_graph_end(&state, 1);
    (void)phase_walk(0); /* State success cannot repair a failed source parse. */
    CHECK(!sh_resource_graph_recording());
}
static void inheritance_checks(void)
{
    sh_resource_graph_frame frame;
    unsigned seen;
    sh_resource_graph_begin(&frame, "entitydef", "ancestor");
    sh_resource_graph_file("source");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "entitydef", "base");
    sh_resource_graph_reference("entitydef", "ancestor");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "entitydef", "phases");
    sh_resource_graph_reference("entitydef", "base");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin_state(&frame, "entitydef", "phases", 1);
    sh_resource_graph_file("model");
    sh_resource_graph_end(&frame, 1);
    CHECK(phase_walk(1) == 3); /* Expanded state supplies all inherited fields. */
    sh_resource_graph_begin_state(&frame, "entitydef", "base", 1);
    sh_resource_graph_file("map-edit");
    sh_resource_graph_end(&frame, 1);
    CHECK(phase_walk(1) == 3); /* Parent's standalone state is not child's state. */
    seen = 0;
    CHECK(sh_resource_graph_walk("entitydef", "base", phase_visit, &seen));
    CHECK(seen == 9);

    sh_resource_graph_begin_state(&frame, "entitydef", "phases", 1);
    sh_resource_graph_file("model");
    sh_resource_graph_reference("entitydef", "base"); /* Also used as an entity. */
    sh_resource_graph_end(&frame, 1);
    CHECK(phase_walk(1) == 11); /* Upgrade the inheritance-only visit. */

    sh_resource_graph_begin_state(&frame, "entitydef", "phases", 0);
    sh_resource_graph_file("model");
    sh_resource_graph_end(&frame, 1);
    CHECK(phase_walk(1) == 11); /* Unexpanded state needs the parent's state. */
    sh_resource_graph_begin_state(&frame, "entitydef", "base", 0);
    sh_resource_graph_end(&frame, 1);
    CHECK(phase_walk(0) == 3); /* Ancestor state missing; all known edges still visited. */
    sh_resource_graph_begin_state(&frame, "entitydef", "ancestor", 1);
    sh_resource_graph_file("revised");
    sh_resource_graph_end(&frame, 1);
    CHECK(phase_walk(1) == 7);
}

static unsigned producer_walk(void)
{
    unsigned seen = 0;
    CHECK(sh_resource_graph_walk("", "derived", phase_visit, &seen));
    return seen;
}
static void producer_checks(void)
{
    sh_resource_graph_frame source;
    const char *first[] = {"model", "MODEL", "derived"};
    const char *second[] = {"revised"}, *other[] = {"map-edit"};
    size_t nodes, complete, edges;
    sh_resource_graph_begin(&source, "", "derived");
    sh_resource_graph_file("source"); sh_resource_graph_end(&source, 1);
    CHECK(sh_resource_graph_producer_inputs("navigation bake", "derived", first, 3));
    CHECK(producer_walk() == 3);
    sh_resource_graph_counts(&nodes, &complete, &edges);
    CHECK(nodes == 3 && complete == 3 && edges == 2); /* duplicate and self edges omitted */
    CHECK(sh_resource_graph_producer_inputs("other producer", "derived", other, 1));
    CHECK(producer_walk() == 11);
    CHECK(sh_resource_graph_producer_inputs("NAVIGATION BAKE", "DERIVED", second, 1));
    CHECK(producer_walk() == 13); /* Replace only this producer, retaining native/other inputs. */
    CHECK(sh_resource_graph_producer_inputs("other producer", "derived", NULL, 0));
    CHECK(producer_walk() == 5);
    CHECK(sh_resource_graph_producer_inputs("navigation bake", "revised", (const char *const[]){"derived"}, 1));
    CHECK(producer_walk() == 5); /* Product-stage cycles still terminate graph walks. */
    sh_resource_graph_begin(&source, "", "derived");
    sh_resource_graph_file("isolated"); sh_resource_graph_end(&source, 1);
    CHECK(producer_walk() == 16); /* Reload retires every old product contribution. */
    CHECK(!sh_resource_graph_producer_inputs("navigation bake", "derived", (const char *const[]){NULL}, 1));
    CHECK(sh_resource_graph_walk_status("", "derived", phase_visit, &edges) == SH_RESOURCE_GRAPH_ERROR);
    sh_resource_graph_test_reset();
    /* Interning a large input set may move the node array. */
    {
        char names[1200][32]; const char *inputs[1200]; size_t i;
        for (i = 0; i < 1200; i++) { snprintf(names[i], sizeof(names[i]), "source-%zu", i); inputs[i] = names[i]; }
        CHECK(sh_resource_graph_producer_inputs("bulk", "derived", inputs, 1200));
        sh_resource_graph_counts(&nodes, &complete, &edges);
        CHECK(nodes == 1201 && complete == 1201 && edges == 1200);
    }
}
static int impact_has(const sh_resource_graph_impact *impact, const char *type, const char *name)
{
    size_t i;
    for (i = 0; i < impact->count; i++)
        if (!strcmp(impact->items[i].type, type) && !strcmp(impact->items[i].name, name)) return 1;
    return 0;
}
static void impact_source(const char *type, const char *name, const char *path)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, type, name);
    if (path) sh_resource_graph_file(path);
    sh_resource_graph_end(&frame, 1);
}
static void impact_checks(void)
{
    const char *body[] = {"GENERATED\\IMAGES\\BODY.BIMAGE", "generated/images/body.bimage"};
    const char *animation[] = {"generated/anims/attack.md6anim"};
    const char *unknown[] = {"not-observed"};
    sh_resource_graph_frame frame;
    sh_resource_graph_impact impact = {0};
    size_t i;
    impact_source("image", "images/body", body[0]);
    sh_resource_graph_begin(&frame, "material", "body");
    sh_resource_graph_reference("image", "images/body");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "model", "cyberdemon");
    sh_resource_graph_reference("material", "body");
    sh_resource_graph_file("generated/models/cyberdemon.bmodel");
    sh_resource_graph_end(&frame, 1);
    impact_source("anim", "attack", animation[0]);
    sh_resource_graph_begin(&frame, "entityDef", "boss");
    sh_resource_graph_file("generated/decls/entitydef/boss.decl");
    sh_resource_graph_reference("model", "cyberdemon");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin_state(&frame, "entityDef", "boss", 0);
    sh_resource_graph_reference("anim", "attack");
    sh_resource_graph_end(&frame, 1);
    impact_source("snapEditorEntity", "local_tool", "editor/local.decl");
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 5);
    CHECK(impact_has(&impact, "", "generated/images/body.bimage"));
    CHECK(impact_has(&impact, "image", "images/body"));
    CHECK(impact_has(&impact, "material", "body"));
    CHECK(impact_has(&impact, "model", "cyberdemon"));
    CHECK(impact_has(&impact, "entitydef", "boss"));
    CHECK(!impact_has(&impact, "anim", "attack")); /* Sibling dependencies are not consumers. */
    CHECK(!impact_has(&impact, "snapeditorentity", "local_tool"));
    for (i = 1; i < impact.count; i++) {
        int cmp = strcmp(impact.items[i - 1].type, impact.items[i].type);
        CHECK(cmp < 0 || (!cmp && strcmp(impact.items[i - 1].name, impact.items[i].name) < 0));
    }
    sh_resource_graph_impact_free(&impact);
    CHECK(!impact.items && !impact.count);
    CHECK(sh_resource_graph_affected(animation, 1, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 3 && impact_has(&impact, "entitydef", "boss"));
    sh_resource_graph_impact_free(&impact);

    CHECK(sh_resource_graph_producer_inputs("generated texture", "derived", body, 2));
    impact_source("material", "derived_material", "derived");
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 7 && impact_has(&impact, "material", "derived_material"));
    sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_producer_inputs("cycle", body[0], (const char *const[]){"derived"}, 1));
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 7); /* Producer cycles and duplicate seeds terminate once per identity. */
    sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_producer_inputs("cycle", body[0], NULL, 0));
    CHECK(sh_resource_graph_producer_inputs("generated texture", "derived", animation, 1));
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 5 && !impact_has(&impact, "material", "derived_material"));
    sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_affected(animation, 1, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 5 && impact_has(&impact, "material", "derived_material"));
    sh_resource_graph_impact_free(&impact);

    sh_resource_graph_begin(&frame, "material", "body");
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(impact.count == 2); /* Active parse removed the old consumer edge. */
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_reference("image", "images/body");
    sh_resource_graph_end(&frame, 0);
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(impact.count == 5); /* Failed parse retains known edges, without complete coverage. */
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_begin(&frame, "material", "body");
    sh_resource_graph_reference("image", "images/body");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_affected(unknown, 1, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(!impact.count); sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_affected((const char *const[]){body[0], unknown[0]}, 2, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(impact.count == 5); sh_resource_graph_impact_free(&impact);
    sh_resource_graph_begin(&frame, "entityDef", "unrelated_incomplete");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(impact.count == 5); sh_resource_graph_impact_free(&impact);
    sh_resource_graph_begin_state(&frame, "entityDef", "unrelated_incomplete", 0);
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    sh_resource_graph_test_reset(); /* Returned names never borrow the graph's storage or lock. */
    CHECK(impact.count == 5 && impact_has(&impact, "model", "cyberdemon"));
    sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_affected(NULL, 0, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(!impact.items && !impact.count);
    CHECK(sh_resource_graph_affected(NULL, 1, &impact) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(sh_resource_graph_affected((const char *const[]){NULL}, 1, &impact) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(sh_resource_graph_affected((const char *const[]){""}, 1, &impact) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(sh_resource_graph_affected(body, 2, NULL) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(!sh_resource_graph_producer_inputs("invalid", "derived", (const char *const[]){NULL}, 1));
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(!impact.items && !impact.count);
    sh_resource_graph_test_reset();

    /* A long chain exercises the non-recursive consumer closure and many
     * independently loaded models sharing an underlying file. */
    for (i = 0; i < 4096; i++) {
        char name[32], prior[32];
        snprintf(name, sizeof(name), "model-%04zu", i);
        sh_resource_graph_begin(&frame, "model", name);
        if (i) {
            snprintf(prior, sizeof(prior), "model-%04zu", i - 1);
            sh_resource_graph_reference("model", prior);
        } else sh_resource_graph_file(body[0]);
        sh_resource_graph_end(&frame, 1);
    }
    CHECK(sh_resource_graph_affected(body, 2, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 4097 && impact_has(&impact, "model", "model-4095"));
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_test_reset();
}
int main(void)
{
    {
        sh_resource_graph_frame source, native, fallback;
        sh_resource_graph_begin(&source, "entitydef", "static_guard");
        CHECK(!sh_resource_graph_begin_missing_state(&fallback, "entitydef", "static_guard", 1));
        CHECK(sh_resource_graph_source_active("entitydef", "static_guard"));
        sh_resource_graph_end(&source, 1);
        CHECK(sh_resource_graph_begin_missing_state(&fallback, "entitydef", "static_guard", 1));
        sh_resource_graph_reference("material", "partial");
        sh_resource_graph_end(&fallback, 0);
        sh_resource_graph_begin_state(&native, "entitydef", "static_guard", 1);
        CHECK(!sh_resource_graph_begin_missing_state(&fallback, "entitydef", "static_guard", 1));
        sh_resource_graph_end(&native, 1);
        CHECK(!sh_resource_graph_begin_missing_state(&fallback, "entitydef", "static_guard", 1));
        CHECK(!sh_resource_graph_recording());
        sh_resource_graph_begin(&source, "entitydef", "static_guard");
        sh_resource_graph_end(&source, 1);
        CHECK(sh_resource_graph_begin_missing_state(&fallback, "entitydef", "static_guard", 0));
        sh_resource_graph_end(&fallback, 0);
        sh_resource_graph_test_reset();
    }
    sh_resource_graph_frame root, child;
    size_t nodes, complete, edges;
    HANDLE threads[4];
    const char *names[] = {"one", "two", "three", "four"};
    size_t i;
    CHECK(!sh_resource_graph_recording());
    sh_resource_graph_reference("ignored", "outside-a-parse");
    sh_resource_graph_begin(&root, "entityDef", "demo");
    CHECK(sh_resource_graph_recording());
    sh_resource_graph_begin(&child, "material", "body");
    sh_resource_graph_file("generated\\image\\body.bimage");
    sh_resource_graph_file("GENERATED/image/BODY.bimage");
    sh_resource_graph_reference("entitydef", "demo"); /* cycle */
    sh_resource_graph_end(&child, 1);
    sh_resource_graph_reference("material", "body"); /* repeated lookup */
    sh_resource_graph_reference("snapeditorentity", "tool"); /* intentionally not loaded */
    sh_resource_graph_end(&root, 1);
    CHECK(!sh_resource_graph_recording());
    CHECK(!sh_resource_graph_walk("ENTITYDEF", "demo", visit, NULL));
    files = materials = pruned = 0;
    sh_resource_graph_begin_state(&root, "entitydef", "demo", 0);
    sh_resource_graph_end(&root, 1);
    CHECK(sh_resource_graph_walk("ENTITYDEF", "demo", visit, NULL));
    CHECK(files == 1 && materials == 1 && pruned == 1);
    sh_resource_graph_counts(&nodes, &complete, &edges);
    CHECK(nodes == 4 && complete == 3 && edges == 4);
    sh_resource_graph_begin(&child, "material", "body");
    CHECK(!sh_resource_graph_walk("entitydef", "demo", visit, NULL));
    sh_resource_graph_file("generated/image/revised.bimage");
    sh_resource_graph_end(&child, 1);
    files = materials = pruned = 0;
    CHECK(sh_resource_graph_walk("entitydef", "demo", visit, NULL));
    CHECK(files == 1 && materials == 1);
    sh_resource_graph_begin(&child, "material", "body"); sh_resource_graph_end(&child, 0);
    CHECK(!sh_resource_graph_walk("entitydef", "demo", visit, NULL));
    CHECK(!sh_resource_graph_walk("entitydef", "unknown", visit, NULL));
    CHECK(sh_resource_graph_walk_status("entitydef", "unknown", visit, NULL) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(sh_resource_graph_walk_status("entitydef", "demo", visit, NULL) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(sh_resource_graph_walk_status("entitydef", "demo", abort_visit, NULL) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(sh_resource_graph_walk_status(NULL, "demo", visit, NULL) == SH_RESOURCE_GRAPH_ERROR);
    CHECK(!sh_resource_graph_walk("entitydef", "demo", abort_visit, NULL));
    for (i = 0; i < 4; i++) { threads[i] = CreateThread(NULL, 0, reader, (void *)names[i], 0, NULL); CHECK(threads[i]); }
    CHECK(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 4; i++) { CloseHandle(threads[i]); CHECK(sh_resource_graph_walk("material", names[i], visit, NULL)); }
    sh_resource_graph_test_reset();
    phase_checks();
    sh_resource_graph_test_reset();
    inheritance_checks();
    sh_resource_graph_test_reset();
    producer_checks();
    sh_resource_graph_test_reset();
    impact_checks();
    if (failures) return 1;
    puts("native dependency graph checks passed"); return 0;
}
