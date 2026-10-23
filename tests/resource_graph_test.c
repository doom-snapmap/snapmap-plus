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
/* A barrier type is neither returned nor followed unless it is a seed; its
 * consumers still arrive through any other path. */
static int fixed_barrier(void *context, const char *type, const char *name)
{
    (void)name; ++*(int *)context;
    return !strcmp(type, "renderparm");
}
static void barrier_checks(void)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_impact impact = {0};
    sh_resource_graph_identity image = {"image", "black"}, parm = {"renderparm", "transmap"};
    int calls = 0;
    impact_source("image", "black", "generated/image/black.bimage");
    sh_resource_graph_begin(&frame, "renderParm", "transmap");
    sh_resource_graph_reference("image", "black");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "renderProg", "font");
    sh_resource_graph_reference("renderParm", "transmap");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "material", "glyphs");
    sh_resource_graph_reference("renderProg", "font");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "material", "direct");
    sh_resource_graph_reference("image", "black");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_consumers(&image, 1, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 5 && impact_has(&impact, "renderparm", "transmap") &&
          impact_has(&impact, "material", "glyphs"));
    sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_consumers_bounded(&image, 1, fixed_barrier, &calls, &impact) ==
          SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(calls == 2 && impact.count == 2 && impact_has(&impact, "image", "black") &&
          impact_has(&impact, "material", "direct"));
    sh_resource_graph_impact_free(&impact);
    /* A supplied barrier object keeps its consumers. */
    CHECK(sh_resource_graph_consumers_bounded(&parm, 1, fixed_barrier, &calls, &impact) ==
          SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 3 && impact_has(&impact, "renderparm", "transmap") &&
          impact_has(&impact, "renderprog", "font") && impact_has(&impact, "material", "glyphs"));
    sh_resource_graph_impact_free(&impact);
    /* A shared consumer reached another way is still returned. */
    sh_resource_graph_begin(&frame, "material", "glyphs");
    sh_resource_graph_reference("renderProg", "font");
    sh_resource_graph_reference("image", "black");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_consumers_bounded(&image, 1, fixed_barrier, &calls, &impact) ==
          SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 3 && impact_has(&impact, "material", "glyphs") &&
          !impact_has(&impact, "renderprog", "font"));
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_test_reset();
}
static int rooted_file_visit(void *context, const char *parent_type, const char *parent_name,
    const char *type, const char *name)
{
    unsigned *seen = context;
    (void)parent_type; (void)parent_name;
    if (!*type) {
        CHECK(!strcmp(name, "decls/renderprogs/global.inc")); ++*seen;
    }
    return 1;
}
static void rooted_file_checks(void)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_impact impact = {0};
    const char *input[] = {"\\DECLS\\renderprogs\\GLOBAL.inc"};
    size_t nodes, complete, edges;
    unsigned seen = 0;
    sh_resource_graph_test_reset();
    sh_resource_graph_begin(&frame, "material", "stock/cap");
    sh_resource_graph_file("/decls/renderprogs/global.inc");
    sh_resource_graph_file(input[0]);
    sh_resource_graph_file("decls/renderprogs/global.inc");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_walk_status("material", "stock/cap", rooted_file_visit, &seen) ==
        SH_RESOURCE_GRAPH_COMPLETE && seen == 1);
    sh_resource_graph_counts(&nodes, &complete, &edges);
    CHECK(nodes == 2 && complete == 2 && edges == 1);
    CHECK(sh_resource_graph_walk_status("", "/decls/renderprogs/global.inc", rooted_file_visit, &seen) ==
        SH_RESOURCE_GRAPH_COMPLETE && seen == 2);
    CHECK(sh_resource_graph_producer_inputs("shader compile", "/generated/program.bin", input, 1));
    CHECK(sh_resource_graph_affected(input, 1, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 3 && impact_has(&impact, "material", "stock/cap") &&
        impact_has(&impact, "", "generated/program.bin"));
    sh_resource_graph_impact_free(&impact);
    CHECK(sh_resource_graph_producer_inputs("shader compile", "generated/program.bin", NULL, 0));
    CHECK(sh_resource_graph_affected(input, 1, &impact) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && !impact_has(&impact, "", "generated/program.bin"));
    sh_resource_graph_impact_free(&impact);
    /* Only filesystem identities use the virtual-root rule. */
    sh_resource_graph_begin(&frame, "material", "/named");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_walk_status("material", "named", rooted_file_visit, &seen) ==
        SH_RESOURCE_GRAPH_INCOMPLETE);
    sh_resource_graph_test_reset();
}
/* R-309 copied-size history: a read recorded against an image survives the consumer's reparse to another
 * image and the image's own rebuild frame; only the test reset clears it. Marking an unknown identity
 * interns it without completing it, and the history begins only when told. */
static void history_checks(void)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_test_reset();
    CHECK(!sh_resource_graph_size_history_ready() && !sh_resource_graph_history("image", "i"));
    sh_resource_graph_begin(&frame, "material", "m"); sh_resource_graph_reference("image", "I");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_history_mark("image", "I", SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    sh_resource_graph_begin(&frame, "material", "m"); sh_resource_graph_reference("image", "J");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "image", "i"); sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_history("image", "I") == SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    CHECK(!sh_resource_graph_history("image", "J"));
    sh_resource_graph_history_mark("image", "new", SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE);
    sh_resource_graph_history_mark("image", "NEW", SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    CHECK(sh_resource_graph_history("image", "new") ==
        (SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE | SH_RESOURCE_GRAPH_HISTORY_ESCAPED));
    sh_resource_graph_identity seed = {"image", "new"};
    sh_resource_graph_impact impact = {0};
    CHECK(sh_resource_graph_consumers(&seed, 1, &impact) == SH_RESOURCE_GRAPH_INCOMPLETE);
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_size_history_begin();
    CHECK(sh_resource_graph_size_history_ready());
    sh_resource_graph_test_reset();
    CHECK(!sh_resource_graph_size_history_ready() && !sh_resource_graph_history("image", "I"));
}
/* R-321 request frames. A retirement default's frame also records names a hooked find asked for and did not
 * find. Only that frame records them: an ordinary frame, a nested parse inside it, a paused scope and no frame
 * at all record nothing. The latest frame replaces the identity's requests, and only a completed frame lets
 * the consumer closure count them as observed. */
static int observed_closure(const char *type, const char *name, sh_resource_graph_impact *impact, char first[160])
{
    sh_resource_graph_identity seed = {(char *)type, (char *)name};
    return (int)sh_resource_graph_consumers_observed(&seed, 1, NULL, NULL, impact, first, 160);
}
static void request_checks(void)
{
    sh_resource_graph_frame defaulted, nested, paused, ordinary;
    sh_resource_graph_impact impact = {0};
    size_t nodes, complete, edges;
    char first[160];
    sh_resource_graph_test_reset();
    CHECK(!sh_resource_graph_recording_requests());
    sh_resource_graph_request("image", "env/outside");
    sh_resource_graph_begin(&ordinary, "discreteAnimation", "anim/live");
    CHECK(sh_resource_graph_recording() && !sh_resource_graph_recording_requests());
    sh_resource_graph_request("image", "env/plain");
    sh_resource_graph_reference("image", "env/default"); /* a found object is a reference in any frame */
    sh_resource_graph_end(&ordinary, 1);
    sh_resource_graph_begin_requests(&defaulted, "discreteAnimation", "anim/retired");
    CHECK(sh_resource_graph_recording_requests());
    sh_resource_graph_request("image", "env/later");
    sh_resource_graph_begin(&nested, "material", "nested");
    CHECK(sh_resource_graph_recording() && !sh_resource_graph_recording_requests());
    sh_resource_graph_request("image", "env/nested");
    sh_resource_graph_end(&nested, 1);
    sh_resource_graph_pause(&paused);
    CHECK(!sh_resource_graph_recording_requests());
    sh_resource_graph_request("image", "env/paused");
    sh_resource_graph_end(&paused, 0);
    CHECK(sh_resource_graph_recording_requests()); /* the default's own frame records again */
    sh_resource_graph_request("IMAGE", "ENV\\LATER"); /* the same identity, folded */
    sh_resource_graph_reference("image", "env/default");
    sh_resource_graph_end(&defaulted, 1);
    CHECK(!sh_resource_graph_recording() && !sh_resource_graph_recording_requests());
    sh_resource_graph_counts(&nodes, &complete, &edges);
    /* anim/live, env/default, anim/retired, env/later and material:nested; nothing for the unrecorded names. */
    CHECK(nodes == 5 && complete == 3 && edges == 4);
    CHECK(observed_closure("image", "env/later", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE && !first[0]);
    CHECK(impact.count == 2 && impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("material", "nested", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("image", "env/default", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 3 && impact_has(&impact, "discreteanimation", "anim/live") &&
        impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    {
        const char *unrecorded[] = {"env/plain", "env/nested", "env/paused", "env/outside"};
        for (int i = 0; i < 4; i++) {
            char want[160];
            snprintf(want, sizeof(want), "image:%s (no recorded node)", unrecorded[i]);
            CHECK(observed_closure("image", unrecorded[i], &impact, first) == SH_RESOURCE_GRAPH_INCOMPLETE);
            CHECK(!impact.count && !strcmp(first, want));
            sh_resource_graph_impact_free(&impact);
        }
    }
    /* An unrelated change: a completely observed consumer of another identity, never the retired default. */
    sh_resource_graph_begin(&ordinary, "material", "elsewhere");
    sh_resource_graph_reference("image", "env/unrelated");
    sh_resource_graph_end(&ordinary, 1);
    CHECK(observed_closure("image", "env/unrelated", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && !impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    /* An incomplete end keeps the edges the frame recorded but leaves the identity incompletely observed. */
    sh_resource_graph_begin_requests(&defaulted, "discreteanimation", "anim/retired");
    sh_resource_graph_request("image", "env/later");
    sh_resource_graph_end(&defaulted, 0);
    CHECK(observed_closure("image", "env/later", &impact, first) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(!strcmp(first, "discreteanimation:anim/retired") && impact.count == 2 &&
        impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    /* A later frame replaces the requests: env/later keeps its node, but no consumer. */
    sh_resource_graph_begin_requests(&defaulted, "discreteanimation", "anim/retired");
    sh_resource_graph_request("image", "env/other");
    sh_resource_graph_end(&defaulted, 1);
    CHECK(observed_closure("image", "env/later", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 1 && !impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("image", "env/other", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("image", "env/default", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && !impact_has(&impact, "discreteanimation", "anim/retired"));
    sh_resource_graph_impact_free(&impact);
    /* Canonical deferred state has its own request set. Refreshing the state
     * preserves source requests; a later state read replaces only its phase. */
    sh_resource_graph_begin_requests(&ordinary, "entitydef", "state-owner");
    sh_resource_graph_request("image", "source/missing");
    sh_resource_graph_end(&ordinary, 1);
    sh_resource_graph_begin_state_requests(&nested, "entitydef", "state-owner", 1);
    CHECK(sh_resource_graph_recording_requests());
    sh_resource_graph_request("material", "state/missing");
    sh_resource_graph_pause(&paused);
    sh_resource_graph_request("material", "instance/missing");
    sh_resource_graph_end(&paused, 0);
    sh_resource_graph_end(&nested, 1);
    CHECK(observed_closure("material", "state/missing", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && impact_has(&impact, "entitydef", "state-owner"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("image", "source/missing", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && impact_has(&impact, "entitydef", "state-owner"));
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_begin_state_requests(&nested, "entitydef", "state-owner", 1);
    sh_resource_graph_end(&nested, 1);
    CHECK(observed_closure("material", "state/missing", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 1 && !impact_has(&impact, "entitydef", "state-owner"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("image", "source/missing", &impact, first) == SH_RESOURCE_GRAPH_COMPLETE);
    CHECK(impact.count == 2 && impact_has(&impact, "entitydef", "state-owner"));
    sh_resource_graph_impact_free(&impact);
    CHECK(observed_closure("material", "instance/missing", &impact, first) == SH_RESOURCE_GRAPH_INCOMPLETE);
    CHECK(!impact.count);
    sh_resource_graph_impact_free(&impact);
    sh_resource_graph_test_reset();
}
static void *graph_main_fiber;
static sh_resource_graph_frame graph_other_token, graph_abandoned_token;
static int graph_fiber_mode;
static int graph_fiber_visit(void *context,const char *parent_type,const char *parent_name,const char *type,const char *name)
{
    (void)parent_type; (void)parent_name;
    unsigned *seen=context;
    if (!*type && !strcmp(name,"fiber/a")) *seen|=1;
    if (!*type && !strcmp(name,"fiber/b")) *seen|=2;
    if (!*type && !strcmp(name,"fiber/after")) *seen|=4;
    return 1;
}
static int graph_fiber_walk(const char *name,unsigned *seen)
{ *seen=0; return sh_resource_graph_walk("material",name,graph_fiber_visit,seen); }
static void CALLBACK graph_fiber(void *unused)
{
    (void)unused;
    sh_resource_graph_frame local;
    const char *name=graph_fiber_mode==1 ? "abandoned" : graph_fiber_mode==2 ? "cross-a" : "fiber-a";
    sh_resource_graph_begin_requests(&local,"material",name);
    sh_resource_graph_file("fiber/a");
    CHECK(sh_resource_graph_frame_current(&local) && sh_resource_graph_recording_requests());
    if(graph_fiber_mode==1) {
        graph_abandoned_token=local; SwitchToFiber(graph_main_fiber); CHECK(0);
    }
    if(graph_fiber_mode==2) {
        CHECK(!sh_resource_graph_frame_current(&graph_other_token));
        sh_resource_graph_end(&graph_other_token,1); /* wrong fiber must not pop its owner */
        CHECK(sh_resource_graph_frame_current(&local));
    } else {
        SwitchToFiber(graph_main_fiber);
        CHECK(sh_resource_graph_frame_current(&local) && sh_resource_graph_recording_requests());
        sh_resource_graph_file("fiber/after");
    }
    sh_resource_graph_end(&local,1);
    CHECK(!sh_resource_graph_recording());
    SwitchToFiber(graph_main_fiber); CHECK(0);
}
static DWORD WINAPI graph_abandoned_thread(LPVOID unused)
{
    (void)unused; sh_resource_graph_frame local;
    sh_resource_graph_begin(&local,"material","thread-abandoned");
    sh_resource_graph_file("fiber/a"); return 0;
}
static void fiber_checks(void)
{
    sh_resource_graph_frame outer,inner; unsigned seen=0;
    sh_resource_graph_test_reset();
    int converted=!IsThreadAFiber();
    graph_main_fiber=converted ? ConvertThreadToFiber(NULL) : GetCurrentFiber(); CHECK(graph_main_fiber);
    graph_fiber_mode=0; void *fiber=CreateFiber(0,graph_fiber,NULL); CHECK(fiber);
    SwitchToFiber(fiber);
    CHECK(!sh_resource_graph_recording());
    sh_resource_graph_begin(&outer,"material","fiber-b"); graph_other_token=outer;
    sh_resource_graph_file("fiber/b");
    SwitchToFiber(fiber);
    CHECK(sh_resource_graph_frame_current(&outer));
    sh_resource_graph_end(&outer,1); DeleteFiber(fiber);
    CHECK(!sh_resource_graph_recording()); /* drains without reading a deleted fiber's stack */
    CHECK(graph_fiber_walk("fiber-a",&seen) && seen==5);
    CHECK(graph_fiber_walk("fiber-b",&seen) && seen==2);
    graph_fiber_mode=1; fiber=CreateFiber(0,graph_fiber,NULL); CHECK(fiber);
    SwitchToFiber(fiber); DeleteFiber(fiber);
    CHECK(!sh_resource_graph_recording());
    CHECK(!graph_fiber_walk("abandoned",&seen) && seen==1);
    sh_resource_graph_begin(&outer,"material","abandoned"); sh_resource_graph_end(&outer,1);
    CHECK(graph_fiber_walk("abandoned",&seen)); /* no stranded active counter */
    sh_resource_graph_end(&graph_abandoned_token,1);
    CHECK(graph_fiber_walk("abandoned",&seen)); /* an older generation cannot retire a fresh parse */
    graph_fiber_mode=2;
    sh_resource_graph_begin(&outer,"material","cross-b"); graph_other_token=outer;
    fiber=CreateFiber(0,graph_fiber,NULL); CHECK(fiber); SwitchToFiber(fiber);
    CHECK(sh_resource_graph_frame_current(&outer)); sh_resource_graph_end(&outer,1); DeleteFiber(fiber);
    CHECK(!sh_resource_graph_recording());
    CHECK(!graph_fiber_walk("cross-a",&seen) && !graph_fiber_walk("cross-b",&seen));
    /* Out-of-order and stale completion publish no success and cannot
     * underflow either node's active count on a later duplicate finish. */
    sh_resource_graph_begin(&outer,"material","out-outer");
    sh_resource_graph_begin(&inner,"material","out-inner");
    sh_resource_graph_end(&outer,1); CHECK(!sh_resource_graph_recording());
    sh_resource_graph_end(&inner,1);
    CHECK(!graph_fiber_walk("out-outer",&seen) && !graph_fiber_walk("out-inner",&seen));
    sh_resource_graph_begin(&outer,"material","out-outer"); sh_resource_graph_end(&outer,1);
    sh_resource_graph_begin(&inner,"material","out-inner"); sh_resource_graph_end(&inner,1);
    CHECK(graph_fiber_walk("out-outer",&seen) && graph_fiber_walk("out-inner",&seen));
    sh_resource_graph_end(&inner,1); CHECK(!graph_fiber_walk("out-inner",&seen));
    sh_resource_graph_begin(&inner,"material","out-inner"); sh_resource_graph_end(&inner,1);
    CHECK(graph_fiber_walk("out-inner",&seen));
    HANDLE thread=CreateThread(NULL,0,graph_abandoned_thread,NULL,0,NULL); CHECK(thread);
    WaitForSingleObject(thread,INFINITE); CloseHandle(thread);
    CHECK(!sh_resource_graph_recording());
    CHECK(!graph_fiber_walk("thread-abandoned",&seen) && seen==1);
    sh_resource_graph_begin(&outer,"material","thread-abandoned"); sh_resource_graph_end(&outer,1);
    CHECK(graph_fiber_walk("thread-abandoned",&seen));
    if(converted) CHECK(ConvertFiberToThread());
    sh_resource_graph_test_reset();
}
int main(void)
{
    fiber_checks();
    history_checks();
    rooted_file_checks();
    request_checks();
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
    barrier_checks();
    if (failures) return 1;
    puts("native dependency graph checks passed"); return 0;
}
