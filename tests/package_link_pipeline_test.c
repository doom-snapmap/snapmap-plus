/* Real source inventories through scope, binding equivalence and provider projection. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_scope.h"
#include "package_compiler.h"
#include "package_link_graph.h"
#include "decl_rebind.h"
#include "package_fixture.h"

static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *out)
{
    (void)context; memset(out, 0, sizeof(*out));
    out->kind = !strcmp(type.name, "Entity") ? SH_DECL_VALUE_OBJECT :
        !strcmp(type.name, "Material") ? SH_DECL_VALUE_REFERENCE : SH_DECL_VALUE_IGNORE;
    return 1;
}
static int field(void *context, sh_decl_value_type type, const char *name, sh_decl_value_type *out)
{
    (void)context; (void)type;
    *out = !strcmp(name, "material") ? (sh_decl_value_type){"Material", "*"} : (sh_decl_value_type){"int", ""};
    return 1;
}
static const sh_decl_dependency_schema schema = {NULL, describe, field};

typedef struct binding {
    const sh_package_compilation *scope;
    size_t base;
    sh_package_link_edge edge;
    const sh_package_link_classes *classes;
    char **names;
} binding;
static int reference(void *context, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    binding *b = context;
    char path[256]; const sh_compiled_resource *target;
    assert(!strcmp(slot, "edit.material") && !strcmp(type.name, "Material"));
    snprintf(path, sizeof(path), "generated/decls/material/%.*s.decl", (int)length, name);
    target = sh_package_compilation_find(b->scope, path); assert(target);
    b->edge = (sh_package_link_edge){"edit.material", b->base + (size_t)(target - b->scope->resources)};
    if (!b->names) return 0;
    *replacement = b->names[b->classes->nodes[b->edge.target]]; return 1;
}
static int available(void *context, const char *path, char *error, size_t capacity)
{
    (void)error; (void)capacity;
    assert(!strstr(path, "/smp/"));
    assert(!strcmp(path, "generated/decls/entitydef/props/rock.decl") ||
        !strcmp(path, "generated/decls/material/materials/stone.decl"));
    return *(int *)context;
}

int main(void)
{
    const char *folders[] = {"mars", "green", "duplicate"};
    const char *entity = "{ edit = { material = \"materials/stone\"; health = 10; } }";
    const char *colors[] = {"{ color {1,0,0,1} }", "{ color {0,1,0,1} }", "{ color {1,0,0,1} }"};
    sh_package_sources *sources;
    sh_package_scope views[3] = {{0}};
    sh_package_compilation *scopes[3] = {0}, *projected = NULL, *copy = NULL;
    sh_package_compilation empty = {0}; sh_package_sources no_sources = {0};
    sh_package_link_node nodes[6] = {{0}};
    sh_package_link_edge edges[6] = {{0}};
    sh_package_link_classes classes = {0};
    sh_package_resource_projection projections[6] = {{0}};
    const sh_compiled_resource *contributors[6][6] = {{0}};
    char *names[6] = {0}, *paths[6] = {0}, *bodies[6] = {0};
    char temporary[MAX_PATH], relative[256], error[512];
    size_t entities[3], materials[3];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-link-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, "{\"id\":\"rocks\",\"name\":\"Rocks\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/entitydef/props/rock.decl", folders[i]); create(relative, entity);
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/material/materials/stone.decl", folders[i]); create(relative, colors[i]);
    }
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources && sources->component_count == 3);
    for (size_t s = 0; s < 3; s++) {
        assert(sh_package_scope_open(sources, s, &views[s], error, sizeof(error)));
        scopes[s] = sh_package_compile(&views[s].sources, NULL, NULL, error, sizeof(error));
        assert(scopes[s] && scopes[s]->resource_count == 2);
        for (size_t r = 0; r < 2; r++) {
            size_t n = s * 2 + r, length;
            const sh_compiled_resource *resource = &scopes[s]->resources[r];
            unsigned char *body = sh_package_compilation_read(scopes[s], resource, SIZE_MAX, &length, error, sizeof(error));
            sh_package_file_identity bytes;
            assert(body && sh_package_bytes_identity(body, length, &bytes));
            nodes[n].family = resource->type; nodes[n].path = resource->engine_path; nodes[n].length = bytes.length;
            memcpy(nodes[n].content, bytes.digest, 32);
            if (!strcmp(resource->type, "entitydef")) {
                binding b = {scopes[s], s * 2};
                sh_decl_rebind_result result;
                char *unchanged = sh_decl_rebind_entity((sh_decl_source){(char *)body, length},
                    (sh_decl_value_type){"Entity", ""}, &schema, reference, &b, NULL, &result);
                assert(unchanged && !result.changed && result.dependencies.references == 1);
                edges[n] = b.edge; nodes[n].edges = &edges[n]; nodes[n].edge_count = 1; entities[s] = n;
                free(unchanged);
            } else materials[s] = n;
            free(body);
        }
    }
    assert(sh_package_link_identify(nodes, 6, &classes, error, sizeof(error)) && classes.count == 4);
    /* Source discovery orders duplicate/green/mars; identical red packs collapse. */
    assert(classes.nodes[entities[0]] == classes.nodes[entities[2]]);
    assert(classes.nodes[entities[0]] != classes.nodes[entities[1]]);
    assert(classes.nodes[materials[0]] == classes.nodes[materials[2]]);
    for (size_t c = 0; c < classes.count; c++) {
        size_t n = classes.representatives[c];
        const sh_compiled_resource *resource = &scopes[n / 2]->resources[n % 2];
        char hex[65];
        for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", classes.identities[c][i]);
        names[c] = malloc(strlen(resource->name) + 70); paths[c] = malloc(strlen(resource->engine_path) + 70);
        assert(names[c] && paths[c]);
        sprintf(names[c], "smp/%s/%s", hex, resource->name);
        sprintf(paths[c], "generated/decls/%s/%s.decl", resource->type, names[c]);
        projections[c].engine_path = paths[c]; projections[c].inputs = contributors[c];
        for (size_t i = 0; i < 6; i++) if (classes.nodes[i] == c)
            contributors[c][projections[c].input_count++] = &scopes[i / 2]->resources[i % 2];
    }
    for (size_t c = 0; c < classes.count; c++) {
        size_t n = classes.representatives[c], length;
        const sh_compiled_resource *resource = &scopes[n / 2]->resources[n % 2];
        if (strcmp(resource->type, "entitydef")) continue;
        unsigned char *body = sh_package_compilation_read(scopes[n / 2], resource, SIZE_MAX, &length, error, sizeof(error));
        binding b = {scopes[n / 2], n / 2 * 2, {0}, &classes, names};
        sh_decl_rebind_result result;
        assert(body);
        bodies[c] = sh_decl_rebind_entity((sh_decl_source){(char *)body, length}, (sh_decl_value_type){"Entity", ""},
            &schema, reference, &b, &projections[c].length, &result);
        assert(bodies[c] && result.changed == 1);
        projections[c].body = (unsigned char *)bodies[c]; free(body);
    }
    projected = sh_package_compilation_project(sources, projections, classes.count, NULL, NULL, error, sizeof(error));
    if (!projected) fprintf(stderr, "%s\n", error);
    assert(projected && projected->resource_count == 4 && projected->duplicate_count == 2);
    for (size_t s = 0; s < 3; s++) {
        size_t length, c = classes.nodes[entities[s]];
        const sh_compiled_resource *resource = sh_package_compilation_find(projected, paths[c]);
        unsigned char *body = sh_package_compilation_read(projected, resource, SIZE_MAX, &length, error, sizeof(error));
        assert(body && strstr((char *)body, names[classes.nodes[materials[s]]]) && strstr((char *)body, "health = 10"));
        assert(!strcmp(resource->original_path, "generated/decls/entitydef/props/rock.decl"));
        assert(resource->source_count == (s == 1 ? 1 : 2)); free(body);
    }
    {
        int present = 1;
        sh_package_missing missing = {0};
        assert(sh_package_compilation_payload_missing(projected, available, &present, &missing, error, sizeof(error)) && !missing.count);
        present = 0;
        assert(sh_package_compilation_payload_missing(projected, available, &present, &missing, error, sizeof(error)));
        assert(missing.count == 2 && sh_package_owners_count(&missing.packages) == 3);
        sh_package_missing_free(&missing);
    }
    empty.sources = &no_sources;
    copy = sh_package_compilation_overlay(&empty, projected, error, sizeof(error)); assert(copy);
    sh_package_compilation_free(projected); projected = NULL;
    for (size_t i = 0; i < 3; i++) { sh_package_compilation_free(scopes[i]); sh_package_scope_close(&views[i]); }
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory) assert(sh_package_source_verify(&sources->files[i]));
    sh_package_sources_free(sources); sources = NULL;
    assert(copy->resource_count == 4 && sh_package_compilation_find(copy, paths[0])->original_path);
    sh_package_compilation_free(copy);
    for (size_t i = 0; i < classes.count; i++) { free(names[i]); free(paths[i]); free(bodies[i]); }
    sh_package_link_classes_free(&classes); cleanup();
    puts("package link pipeline tests passed"); return failures != 0;
}
