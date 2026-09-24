#include "package_audio_program.h"
#include "package_sound.h"
#include <stdlib.h>
#include <string.h>

typedef struct pap_sound {
    char *name, *output;
    uint32_t event;
    sh_audio_graph_root root;
    sh_package_owners owners, gameplay;
} pap_sound;
typedef struct pap_file {
    char *path;
    sh_package_owners owners, gameplay;
    int baseline;
} pap_file;
typedef struct pap_scope {
    const sh_package_compilation *compiled;
    pap_sound *sounds;
    size_t count;
    sh_package_audio_capture *capture;
    uint32_t *inventory_events;
    size_t inventory_count;
    int inventory;
    pap_file *files;
    size_t file_count;
} pap_scope;
/* Graph scope/bank/chunk of one shared policy input. The native baseline scope
 * is index count; public origin queries report it as SIZE_MAX. */
typedef struct pap_shared_origin { size_t scope,bank,chunk,records;int baseline; } pap_shared_origin;
typedef struct pap_shared_owners { sh_package_owners owners,gameplay; } pap_shared_owners;
typedef struct pap_bus_recipe {
    size_t first,count;
    uint32_t bus;
    unsigned kind;
    int baseline;
    sh_audio_graph_root root;
    sh_package_owners owners,gameplay;
} pap_bus_recipe;
typedef struct pap_bus_owners { sh_package_owners owners,gameplay; } pap_bus_owners;
struct sh_package_audio_program {
    pap_scope *scopes;
    size_t count;
    sh_audio_originals *originals;
    sh_audio_link_graph *graph;
    sh_audio_shared_policy shared, bound_shared;
    pap_shared_origin *shared_origins;
    size_t shared_count;
    pap_shared_owners *shared_owners;
    int shared_bound;
    uint32_t *baseline_roots;
    size_t baseline_root_count;
    sh_package_audio_bus_value *bus_values;
    size_t bus_count;
    sh_package_audio_capture *bus_baseline;
    sh_package_compilation baseline_scope;
    pap_bus_recipe *bus_recipes;
    size_t bus_recipe_count;
    sh_audio_bus_policy bus_policy;
    sh_audio_bus_policy bound_bus_policy;
    int bus_bound;
    pap_bus_owners *bus_policy_owners;
    size_t failed_scope; /* Scope whose capture failed in the last prepare. */
};
static int pap_fail(char *error, size_t capacity, const char *message, const char *name)
{
    if (error && capacity) snprintf(error, capacity, "%s%s%s", message, name ? ": " : "", name ? name : "");
    return 0;
}
static pap_sound *pap_find(const sh_package_audio_program *p, size_t scope, const char *name)
{
    if (p && scope < p->count && name) for (size_t i = 0; i < p->scopes[scope].count; i++)
        if (!strcmp(p->scopes[scope].sounds[i].name, name)) return p->scopes[scope].sounds + i;
    return NULL;
}
static void pap_files_clear(pap_scope *scope)
{
    for (size_t i = 0; i < scope->file_count; i++) {
        free(scope->files[i].path);
        sh_package_owners_free(&scope->files[i].owners);
        sh_package_owners_free(&scope->files[i].gameplay);
    }
    free(scope->files); scope->files = NULL; scope->file_count = 0;
}
static int pap_files_capture(pap_scope *scope)
{
    size_t count = 0;
    for (size_t i = 0; i < scope->compiled->resource_count; i++)
        count += sh_package_audio_path_kind(scope->compiled->resources[i].engine_path) != 0;
    if (count > SIZE_MAX / sizeof(*scope->files)) return 0;
    scope->files = calloc(count ? count : 1, sizeof(*scope->files));
    if (!scope->files) return 0;
    for (size_t i = 0; i < scope->compiled->resource_count; i++) {
        const sh_compiled_resource *r = scope->compiled->resources + i;
        if (!sh_package_audio_path_kind(r->engine_path)) continue;
        pap_file *file = scope->files + scope->file_count++;
        file->path = _strdup(r->engine_path); file->baseline = r->baseline_known;
        if (!file->path || !sh_package_owners_union(&file->owners, &r->owners) ||
            !sh_package_owners_union(&file->gameplay, &r->gameplay_owners)) return 0;
    }
    return 1;
}
static void pap_bus_clear(sh_package_audio_program *p)
{
    sh_audio_bus_policy_free(&p->bound_bus_policy);p->bus_bound=0;
    if(p->bus_policy_owners)for(size_t i=0;i<p->bus_policy.count;i++) {
        sh_package_owners_free(&p->bus_policy_owners[i].owners);
        sh_package_owners_free(&p->bus_policy_owners[i].gameplay);
    }
    free(p->bus_policy_owners);p->bus_policy_owners=NULL;sh_audio_bus_policy_free(&p->bus_policy);
    for(size_t i=0;i<p->bus_recipe_count;i++) {
        sh_package_owners_free(&p->bus_recipes[i].owners);
        sh_package_owners_free(&p->bus_recipes[i].gameplay);
    }
    free(p->bus_recipes);p->bus_recipes=NULL;p->bus_recipe_count=0;
    for(size_t i=0;i<p->bus_count;i++) {
        sh_package_owners_free(&p->bus_values[i].owners);
        sh_package_owners_free(&p->bus_values[i].gameplay);
    }
    free(p->bus_values);p->bus_values=NULL;p->bus_count=0;
}
static void pap_shared_clear(sh_package_audio_program *p)
{
    if(p->shared_owners)for(size_t i=0;i<p->shared.count;i++) {
        sh_package_owners_free(&p->shared_owners[i].owners);
        sh_package_owners_free(&p->shared_owners[i].gameplay);
    }
    free(p->shared_owners);p->shared_owners=NULL;
    sh_audio_shared_policy_free(&p->bound_shared);p->shared_bound=0;
    sh_audio_shared_policy_free(&p->shared);free(p->shared_origins);p->shared_origins=NULL;p->shared_count=0;
    free(p->baseline_roots);p->baseline_roots=NULL;p->baseline_root_count=0;
}
void sh_package_audio_program_close(sh_package_audio_program *p)
{
    if (!p) return;
    pap_bus_clear(p);
    pap_shared_clear(p);
    sh_audio_link_graph_close(p->graph);
    sh_package_audio_capture_close(p->bus_baseline);
    for (size_t i = 0; i < p->count; i++) {
        pap_scope *scope = p->scopes + i;
        pap_files_clear(scope);
        sh_package_audio_capture_close(scope->capture);
        for (size_t j = 0; j < scope->count; j++) {
            free(scope->sounds[j].name); free(scope->sounds[j].output);
            sh_package_owners_free(&scope->sounds[j].owners); sh_package_owners_free(&scope->sounds[j].gameplay);
        }
        free(scope->sounds);
        free(scope->inventory_events);
    }
    free(p->scopes); free(p);
}
sh_package_audio_program *sh_package_audio_program_open(
    const sh_package_compilation *const *scopes, size_t count,
    sh_audio_originals *originals, char *error, size_t capacity)
{
    if ((count && !scopes) || count > SIZE_MAX / sizeof(pap_scope)) goto invalid;
    sh_package_audio_program *p = calloc(1, sizeof(*p));
    if (!p) goto invalid;
    p->scopes = calloc(count ? count : 1, sizeof(*p->scopes));
    if (!p->scopes) { free(p); goto invalid; }
    p->count = count; p->originals = originals; p->failed_scope = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (!scopes[i]) { sh_package_audio_program_close(p); goto invalid; }
        p->scopes[i].compiled = scopes[i];
        for (size_t f = 0; f < scopes[i]->resource_count; f++)
            if (sh_package_audio_path_kind(scopes[i]->resources[f].engine_path)) { p->scopes[i].inventory = 1; break; }
    }
    return p;
invalid:
    pap_fail(error, capacity, "cannot retain audio source scopes", NULL); return NULL;
}
int sh_package_audio_program_collect(sh_package_audio_program *p,
    size_t index, const char *name, char *error, size_t capacity)
{
    uint32_t event;
    if (!p || p->graph || index >= p->count || !sh_audio_event_identity(name, &event))
        return pap_fail(error, capacity, "invalid audio discovery request", name);
    if (pap_find(p, index, name)) return 1;
    pap_scope *scope = p->scopes + index;
    if (scope->count >= SIZE_MAX / sizeof(*scope->sounds))
        return pap_fail(error, capacity, "too many authored sounds", name);
    char *copy = _strdup(name);
    pap_sound *grown = copy ? realloc(scope->sounds, (scope->count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return pap_fail(error, capacity, "cannot retain authored sound", name); }
    scope->sounds = grown;
    scope->sounds[scope->count++] = (pap_sound){copy, NULL, event, {0}};
    return 1;
}
static int pap_event_order(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return a < b ? -1 : a != b;
}
static int pap_inventory(sh_package_audio_program *p, size_t index, char *error, size_t capacity)
{
    pap_scope *scope = p->scopes + index;
    if (!scope->inventory || scope->capture) return 1;
    scope->capture = sh_package_audio_capture_open_inventory(scope->compiled, p->originals, error, capacity);
    if (!scope->capture) return 0;
    const sh_audio_graph_scope *input = sh_package_audio_capture_input(scope->capture);
    size_t count = 0, at = 0;
    for (size_t i = 0; i < input->bank_count; i++) {
        size_t events = input->banks[i].codec->metadata.event_count;
        if (events > SIZE_MAX - count) goto failed;
        count += events;
    }
    if (count > SIZE_MAX / sizeof(uint32_t)) goto failed;
    uint32_t *events = malloc((count ? count : 1) * sizeof(*events)); if (!events) goto failed;
    for (size_t i = 0; i < input->bank_count; i++) {
        const sh_audio_bank *bank = &input->banks[i].codec->metadata;
        if (bank->event_count) memcpy(events + at, bank->events, bank->event_count * sizeof(*events));
        at += bank->event_count;
    }
    qsort(events, count, sizeof(*events), pap_event_order);
    at = 0;
    for (size_t i = 0; i < count; i++) if (!at || events[i] != events[at - 1]) events[at++] = events[i];
    free(scope->inventory_events); scope->inventory_events = events; scope->inventory_count = at; return 1;
failed:
    sh_package_audio_capture_close(scope->capture); scope->capture = NULL;
    return pap_fail(error, capacity, "cannot retain audio inventory events", NULL);
}
int sh_package_audio_program_affected(const sh_package_audio_program *p, size_t index, const char *name)
{
    uint32_t event;
    if (!p || index >= p->count || !sh_audio_event_identity(name, &event)) return 0;
    const pap_scope *scope = p->scopes + index;
    return scope->inventory_count && bsearch(&event, scope->inventory_events,
        scope->inventory_count, sizeof(event), pap_event_order) != NULL;
}
int sh_package_audio_program_seed(sh_package_audio_program *p, size_t index,
    const sh_resource_catalog *catalog, int (*visit)(void *, const char *), void *context,
    char *error, size_t capacity)
{
    if (!p || p->graph || index >= p->count || !visit)
        return pap_fail(error, capacity, "invalid audio inventory discovery", NULL);
    if (!pap_inventory(p, index, error, capacity)) return 0;
    if (!p->scopes[index].inventory_count) return 1;
    for (size_t i = 0; i < sh_resource_catalog_count(catalog); i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(catalog, i);
        if (!row || strcmp(row->type, "sound") ||
            !sh_package_audio_program_affected(p, index, row->name)) continue;
        if (row->archive >= 2 && !sh_package_compilation_find(p->scopes[index].compiled, row->path)) continue;
        if (!visit(context, row->name)) {
            if (error && capacity && !*error) pap_fail(error, capacity, "cannot capture affected sound", row->name);
            return 0;
        }
    }
    return 1;
}
typedef struct pap_root_ref { pap_sound *sound; } pap_root_ref;
typedef struct pap_sources { const sh_package_compilation *compiled; pap_sound *sound; } pap_sources;
static int pap_source_owners(void *context, const char *path, uint64_t offset, uint64_t length)
{
    pap_sources *walk = context; (void)offset; (void)length;
    const sh_compiled_resource *resource = sh_package_compilation_find(walk->compiled, path);
    if (!resource) return 1;
    /* An exact original root does not acquire a map mod because a different
     * record in its SnapMap container changed. Campaign-only originals still
     * need their authored delivery/activation even when their bytes are exact. */
    if (walk->sound->root.original && resource->baseline_known == 1) return 1;
    return sh_package_owners_union(&walk->sound->owners, &resource->owners) &&
        sh_package_owners_union(&walk->sound->gameplay, &resource->gameplay_owners);
}
static int pap_root_order(const void *a, const void *b)
{
    const pap_root_ref *x = a, *y = b;
    return memcmp(x->sound->root.identity, y->sound->root.identity, 32);
}
static int pap_policy_bank(const sh_audio_bank_codec *codec)
{ return codec->bus_record_count || codec->shared_chunks; }
static int pap_bus_capture_baseline(sh_package_audio_program *p,const sh_audio_graph_scope *scopes,
    char *error,size_t capacity)
{
    int any=0;
    for(size_t s=0;s<p->count;s++)for(size_t b=0;b<scopes[s].bank_count;b++)any|=pap_policy_bank(scopes[s].banks[b].codec);
    if(!any)return 1;
    const sh_audio_original_bank *banks=NULL;size_t count=0;uint32_t init=0;
    if(!sh_audio_bank_identity("init.bnk",&init) || sh_audio_originals_banks(p->originals,&banks,&count,error,capacity)!=1 ||
        count>SIZE_MAX/sizeof(uint32_t))return 0;
    uint32_t *roots=malloc((count ? count : 1)*sizeof(*roots));size_t used=0;if(!roots)return 0;
    for(size_t i=0;i<count;i++) {
        int needed=banks[i].metadata.id==init;
        for(size_t s=0;s<p->count && !needed;s++)for(size_t b=0;b<scopes[s].bank_count && !needed;b++)
            needed=pap_policy_bank(scopes[s].banks[b].codec) && scopes[s].banks[b].codec->metadata.id==banks[i].metadata.id;
        if(needed)roots[used++]=banks[i].metadata.id;
    }
    /* A separate scope prevents original definitions from filling holes in an
     * authored recipe. Its graph witnesses certify equality, not availability
     * from a peer. Capture follows the same typed closure as event selection.
     * The engine loads Init independently of any event; newly named banks and
     * edited shared controls are therefore compared with native Init too. */
    if(used)p->bus_baseline=sh_package_audio_capture_open_roots(&p->baseline_scope,p->originals,roots,used,error,capacity);
    if(!used || !p->bus_baseline){free(roots);return !used;}
    p->baseline_roots=roots;p->baseline_root_count=used;return 1;
}
static int pap_bus_prepare(sh_package_audio_program *p,const sh_audio_graph_scope *scopes,
    sh_audio_link_graph *graph,char *error,size_t capacity)
{
    size_t count=0,scope_count=p->count+(p->bus_baseline!=NULL);
    for(size_t s=0;s<scope_count;s++)for(size_t b=0;b<scopes[s].bank_count;b++) {
        size_t extra=scopes[s].banks[b].codec->bus_record_count;
        if(extra>SIZE_MAX-count)return 0;count+=extra;
    }
    if(count>SIZE_MAX/sizeof(*p->bus_values) || count>SIZE_MAX/sizeof(*p->bus_recipes))return 0;
    p->bus_values=calloc(count ? count : 1,sizeof(*p->bus_values));if(!p->bus_values)return 0;
    p->bus_recipes=calloc(count ? count : 1,sizeof(*p->bus_recipes));if(!p->bus_recipes)return 0;
    for(size_t s=0;s<scope_count;s++)for(size_t b=0;b<scopes[s].bank_count;b++) {
        const sh_audio_bank_codec *codec=scopes[s].banks[b].codec;
        for(size_t o=0;o<codec->object_count;o++) {
            const sh_audio_bank_object *object=codec->objects+o;
            if(object->kind!=8 && object->kind!=20)continue;
            pap_bus_recipe *recipe=p->bus_recipes+p->bus_recipe_count++;
            recipe->first=p->bus_count;recipe->count=object->bus_record_count;recipe->bus=object->id;
            recipe->kind=object->kind;recipe->baseline=s==p->count;
            if(!sh_audio_link_graph_bus_object(graph,s,b,o,&recipe->root))return 0;
            if(!recipe->baseline) {
                const sh_compiled_resource *resource=sh_package_compilation_find(p->scopes[s].compiled,scopes[s].banks[b].path);
                if(resource && (!recipe->root.original || resource->baseline_known!=1) &&
                    (!sh_package_owners_union(&recipe->owners,&resource->owners) ||
                     !sh_package_owners_union(&recipe->gameplay,&resource->gameplay_owners)))return 0;
            }
            for(size_t r=object->first_bus_record;r<object->first_bus_record+object->bus_record_count;r++) {
                sh_package_audio_bus_value *value=p->bus_values+p->bus_count++;
                value->scope=s;value->bank=b;value->object=o;value->record=r;
                value->bus=object->id;value->kind=object->kind;value->field=codec->bus_records[r];
                value->baseline=s==p->count;
                if(!sh_audio_link_graph_bus_record(graph,s,b,r,&value->root))return 0;
                if(value->baseline)continue;
                pap_sound witness={0};witness.root=value->root;
                pap_sources owners={p->scopes[s].compiled,&witness};
                int retained=sh_audio_link_graph_root_sources(graph,value->root.node,pap_source_owners,&owners,error,capacity);
                value->owners=witness.owners;value->gameplay=witness.gameplay;
                if(!retained)return 0;
            }
        }
    }
    return 1;
}
static int pap_bus_compose(sh_package_audio_program *p,sh_audio_link_graph *graph,char *error,size_t capacity)
{
    sh_audio_bus_recipe_input *inputs=NULL;sh_audio_bus_value_input *values=NULL;int ok=0;
    if(p->bus_recipe_count>SIZE_MAX/sizeof(*inputs) || p->bus_count>SIZE_MAX/sizeof(*values))goto done;
    inputs=calloc(p->bus_recipe_count ? p->bus_recipe_count : 1,sizeof(*inputs));
    values=calloc(p->bus_count ? p->bus_count : 1,sizeof(*values));if(!inputs || !values)goto done;
    for(size_t i=0;i<p->bus_count;i++) {
        const sh_package_audio_bus_value *value=p->bus_values+i;unsigned char *body=NULL;size_t length=0;
        if(!sh_audio_link_graph_bus_value(graph,value->scope,value->bank,value->record,0,&body,&length,error,capacity))goto done;
        values[i]=(sh_audio_bus_value_input){value->field,value->root,body,i};
    }
    for(size_t i=0;i<p->bus_recipe_count;i++) {
        const pap_bus_recipe *recipe=p->bus_recipes+i;
        inputs[i]=(sh_audio_bus_recipe_input){recipe->bus,recipe->kind,recipe->baseline,recipe->root,
            values+recipe->first,recipe->count};
    }
    if(!sh_audio_bus_policy_compile(graph,inputs,p->bus_recipe_count,&p->bus_policy,error,capacity))goto done;
    p->bus_policy_owners=calloc(p->bus_policy.count ? p->bus_policy.count : 1,sizeof(*p->bus_policy_owners));
    if(!p->bus_policy_owners)goto done;
    for(size_t i=0;i<p->bus_policy.count;i++) {
        const sh_audio_bus_setting *setting=p->bus_policy.settings+i;pap_bus_owners *owners=p->bus_policy_owners+i;
        for(size_t c=0;c<setting->contributor_count;c++) {
            sh_audio_bus_contributor source=setting->contributors[c];
            const sh_package_owners *contributor,*gameplay;
            if(source.value==SIZE_MAX) {
                if(source.recipe>=p->bus_recipe_count)goto done;
                contributor=&p->bus_recipes[source.recipe].owners;gameplay=&p->bus_recipes[source.recipe].gameplay;
            } else {
                if(source.value>=p->bus_count)goto done;
                contributor=&p->bus_values[source.value].owners;gameplay=&p->bus_values[source.value].gameplay;
            }
            if(!sh_package_owners_union(&owners->owners,contributor) || !sh_package_owners_union(&owners->gameplay,gameplay))goto done;
        }
    }
    ok=1;
done:
    if(values)for(size_t i=0;i<p->bus_count;i++)free((void *)values[i].body);
    free(values);free(inputs);
    if(!ok && error && capacity && !*error)pap_fail(error,capacity,"cannot compose shared bus policy",NULL);
    return ok;
}
int sh_package_audio_program_bus_edited(const sh_package_audio_program *p)
{
    if(!p || !p->graph)return 0;
    for(size_t i=0;i<p->bus_policy.count;i++) {
        const sh_audio_bus_setting *setting=p->bus_policy.settings+i;
        if(setting->removed || setting->recipe>=p->bus_recipe_count || !p->bus_recipes[setting->recipe].baseline)return 1;
    }
    return 0;
}
const sh_audio_bus_policy *sh_package_audio_program_bus_policy(const sh_package_audio_program *p)
{ return p && p->graph ? &p->bus_policy : NULL; }
const sh_audio_bus_policy *sh_package_audio_program_bound_bus_policy(const sh_package_audio_program *p)
{ return p && p->bus_bound ? &p->bound_bus_policy : NULL; }
int sh_package_audio_program_bus_policy_owners(const sh_package_audio_program *p,size_t setting,
    const sh_package_owners **owners,const sh_package_owners **gameplay)
{
    if(owners)*owners=NULL;if(gameplay)*gameplay=NULL;
    if(!p || !p->graph || !owners || !gameplay || setting>=p->bus_policy.count)return 0;
    *owners=&p->bus_policy_owners[setting].owners;*gameplay=&p->bus_policy_owners[setting].gameplay;return 1;
}
size_t sh_package_audio_program_bus_values(const sh_package_audio_program *p)
{ return p && p->graph ? p->bus_count : 0; }
const sh_package_audio_bus_value *sh_package_audio_program_bus_at(const sh_package_audio_program *p,size_t index)
{ return index<sh_package_audio_program_bus_values(p) ? p->bus_values+index : NULL; }
int sh_package_audio_program_bus_body(const sh_package_audio_program *p,size_t index,int bound,
    unsigned char **body,size_t *length,char *error,size_t capacity)
{
    const sh_package_audio_bus_value *value=sh_package_audio_program_bus_at(p,index);
    if(!value) {
        if(body)*body=NULL;if(length)*length=0;
        return pap_fail(error,capacity,"shared bus value is outside the prepared audio program",NULL);
    }
    return sh_audio_link_graph_bus_value(p->graph,value->scope,value->bank,value->record,bound,body,length,error,capacity);
}
int sh_package_audio_program_bus_equal(const sh_package_audio_program *p,size_t left,size_t right)
{
    const sh_package_audio_bus_value *a=sh_package_audio_program_bus_at(p,left),*b=sh_package_audio_program_bus_at(p,right);
    return a && b && sh_audio_link_graph_equal(p->graph,a->root.node,b->root.node);
}
typedef struct pap_shared_inputs {
    sh_audio_shared_input *rows;
    pap_shared_origin *origins;
    sh_audio_graph_root **roots; /* Exact graph root of each parsed record. */
    size_t count,allocated;
    const sh_audio_link_graph *graph;
} pap_shared_inputs;
static void pap_shared_inputs_free(pap_shared_inputs *inputs)
{
    for(size_t i=0;i<inputs->count;i++){free((void *)inputs->rows[i].body);free(inputs->roots[i]);}
    free(inputs->rows);free(inputs->origins);free(inputs->roots);memset(inputs,0,sizeof(*inputs));
}
static const sh_audio_graph_root *pap_shared_root(const pap_shared_inputs *inputs,size_t input,size_t record)
{
    return input<inputs->count && record<inputs->origins[input].records && inputs->roots[input] ?
        inputs->roots[input]+record : NULL;
}
static int pap_shared_equal(void *context,size_t li,size_t lr,size_t ri,size_t rr)
{
    const pap_shared_inputs *inputs=context;
    const sh_audio_graph_root *left=pap_shared_root(inputs,li,lr),*right=pap_shared_root(inputs,ri,rr);
    return left && right ? sh_audio_link_graph_equal(inputs->graph,left->node,right->node) : -1;
}
static const unsigned char *pap_shared_identity(void *context,size_t input,size_t record)
{
    const sh_audio_graph_root *root=pap_shared_root(context,input,record);
    return root ? root->identity : NULL;
}
static int pap_shared_bank(pap_shared_inputs *inputs,const sh_audio_graph_bank *bank,
    size_t scope,size_t index,int baseline,char *error,size_t capacity)
{
    for(size_t c=0;c<bank->codec->chunk_count;c++) {
        const sh_audio_bank_chunk *chunk=bank->codec->chunks+c;
        const char *tag=!strcmp(chunk->tag,"STMG") ? "STMG" : !strcmp(chunk->tag,"ENVS") ? "ENVS" :
            !strcmp(chunk->tag,"PLAT") ? "PLAT" : NULL;
        if(!tag)continue;
        if(inputs->count==inputs->allocated) {
            size_t next=inputs->allocated ? inputs->allocated*2 : 16;
            if(next<inputs->allocated || next>SIZE_MAX/sizeof(*inputs->rows) || next>SIZE_MAX/sizeof(*inputs->origins))return 0;
            sh_audio_shared_input *rows=realloc(inputs->rows,next*sizeof(*rows));if(!rows)return 0;inputs->rows=rows;
            pap_shared_origin *origins=realloc(inputs->origins,next*sizeof(*origins));if(!origins)return 0;
            inputs->origins=origins;
            sh_audio_graph_root **roots=realloc(inputs->roots,next*sizeof(*roots));if(!roots)return 0;
            inputs->roots=roots;inputs->allocated=next;
        }
        if(chunk->shared_record_count>SIZE_MAX/sizeof(sh_audio_graph_root))return 0;
        unsigned char *body=malloc(chunk->length ? chunk->length : 1);
        sh_audio_graph_root *roots=calloc(chunk->shared_record_count ? chunk->shared_record_count : 1,sizeof(*roots));
        sh_audio_hirc parsed={0};int valid=body && roots;
        /* The codec retains this chunk's records in the same order the policy
         * reader parses them; each maps to its exact graph root. */
        if(valid)valid=bank->source.read(bank->source.context,chunk->offset+8,body,chunk->length) &&
            sh_audio_shared_chunk_read(tag,body,chunk->length,&parsed,error,capacity)==1 &&
            parsed.shared_count==chunk->shared_record_count;
        for(size_t r=0;valid && r<chunk->shared_record_count;r++)
            valid=sh_audio_link_graph_shared_record(inputs->graph,scope,index,chunk->first_shared_record+r,roots+r);
        sh_audio_hirc_free(&parsed);
        if(!valid){free(body);free(roots);return pap_fail(error,capacity,"cannot capture shared audio settings",bank->path);}
        inputs->rows[inputs->count]=(sh_audio_shared_input){tag,body,chunk->length,scope,baseline};
        inputs->roots[inputs->count]=roots;
        inputs->origins[inputs->count++]=(pap_shared_origin){scope,index,c,chunk->shared_record_count,baseline};
    }
    return 1;
}
static int pap_baseline_root(const sh_package_audio_program *p,uint32_t id)
{
    for(size_t i=0;i<p->baseline_root_count;i++)if(p->baseline_roots[i]==id)return 1;
    return 0;
}
typedef struct pap_record_owners { const sh_package_compilation *compiled;sh_audio_graph_root root;pap_shared_owners *out; } pap_record_owners;
static int pap_record_owner(void *context,const char *path,uint64_t offset,uint64_t length)
{
    pap_record_owners *walk=context;(void)offset;(void)length;
    const sh_compiled_resource *resource=sh_package_compilation_find(walk->compiled,path);
    if(!resource || (walk->root.original && resource->baseline_known==1))return 1;
    return sh_package_owners_union(&walk->out->owners,&resource->owners) &&
        sh_package_owners_union(&walk->out->gameplay,&resource->gameplay_owners);
}
static int pap_shared_prepare(sh_package_audio_program *p,const sh_audio_graph_scope *scopes,
    const sh_audio_link_graph *graph,char *error,size_t capacity)
{
    pap_shared_inputs inputs={0};sh_audio_shared_policy policy={0};pap_shared_owners *owners=NULL;int ok=0,any=0;
    inputs.graph=graph;
    for(size_t s=0;s<p->count;s++)for(size_t b=0;b<scopes[s].bank_count;b++)any|=!!scopes[s].banks[b].codec->shared_chunks;
    if(any && p->bus_baseline) {
        /* Native witnesses: Init and the original counterparts of edited banks.
         * They retain their own typed closure and carry no delivery owner. */
        const sh_audio_graph_scope *baseline=scopes+p->count;
        for(size_t b=0;b<baseline->bank_count;b++)
            if(baseline->banks[b].codec->shared_chunks && pap_baseline_root(p,baseline->banks[b].codec->metadata.id) &&
                !pap_shared_bank(&inputs,baseline->banks+b,p->count,b,1,error,capacity))goto done;
    }
    for(size_t s=0;s<p->count;s++)for(size_t b=0;b<scopes[s].bank_count;b++) {
        const sh_audio_graph_bank *bank=scopes[s].banks+b;
        /* An exact original copy is another native witness, not an edit. */
        if(bank->codec->shared_chunks && !pap_shared_bank(&inputs,bank,s,b,!!bank->original,error,capacity))goto done;
    }
    sh_audio_shared_exact compare={&inputs,pap_shared_equal,pap_shared_identity};
    if(!sh_audio_shared_policy_compile_exact(inputs.rows,inputs.count,&compare,&policy,error,capacity))goto done;
    owners=calloc(policy.count ? policy.count : 1,sizeof(*owners));if(!owners)goto done;
    for(size_t i=0;i<policy.count;i++) {
        const sh_audio_shared_setting *setting=policy.settings+i;
        if(!setting->edited)continue;
        for(size_t c=0;c<setting->contributor_count;c++) {
            const sh_audio_shared_contributor *source=setting->contributors+c;
            const pap_shared_origin *origin=inputs.origins+source->input;
            if(origin->baseline || origin->scope>=p->count)continue;
            const sh_audio_graph_root *root=pap_shared_root(&inputs,source->input,source->record);
            pap_record_owners walk={p->scopes[origin->scope].compiled,{0},owners+i};
            if(!root)goto done;
            walk.root=*root;
            if(!sh_audio_link_graph_root_sources(graph,walk.root.node,pap_record_owner,&walk,error,capacity))goto done;
        }
    }
    p->shared=policy;memset(&policy,0,sizeof(policy));p->shared_origins=inputs.origins;
    p->shared_count=inputs.count;inputs.origins=NULL;p->shared_owners=owners;owners=NULL;ok=1;
done:
    if(owners)for(size_t i=0;i<policy.count;i++) {
        sh_package_owners_free(&owners[i].owners);sh_package_owners_free(&owners[i].gameplay);
    }
    free(owners);
    pap_shared_inputs_free(&inputs);
    sh_audio_shared_policy_free(&policy);
    if(!ok && error && capacity && !*error)pap_fail(error,capacity,"cannot compose shared audio controls",NULL);
    return ok;
}
typedef struct pap_original_reader { sh_audio_originals *originals;const char *path; } pap_original_reader;
static int pap_original_read(void *context,uint64_t at,void *body,size_t length)
{
    pap_original_reader *r=context;
    return sh_audio_originals_read(r->originals,r->path,at,body,length,NULL,NULL,0)==1;
}
int sh_package_audio_baseline_shared(sh_audio_originals *originals,sh_audio_shared_policy *out,
    char *error,size_t capacity)
{
    const sh_audio_original_bank *banks=NULL;size_t count=0,used=0;uint32_t init=0;int ok=0;
    sh_audio_shared_input inputs[3];unsigned char *bodies[3]={0};sh_audio_bank_codec codec={0};
    if(error && capacity)*error=0;
    if(!out)return 0;memset(out,0,sizeof(*out));
    if(!sh_audio_bank_identity("init.bnk",&init) || sh_audio_originals_banks(originals,&banks,&count,error,capacity)!=1)
        return pap_fail(error,capacity,"native audio baseline is unavailable",NULL);
    const sh_audio_original_bank *bank=NULL;
    for(size_t i=0;i<count && !bank;i++)if(banks[i].metadata.id==init)bank=banks+i;
    if(!bank)return 1;
    uint64_t length=0;pap_original_reader reader={originals,bank->path};
    if(sh_audio_originals_read(originals,bank->path,0,NULL,0,&length,error,capacity)!=1)
        return pap_fail(error,capacity,"cannot read the native audio baseline",bank->path);
    if(sh_audio_bank_codec_read((sh_audio_bank_source){&reader,length,pap_original_read},&codec,error,capacity)!=1)
        return pap_fail(error,capacity,"cannot decode the native audio baseline",bank->path);
    for(size_t c=0;c<codec.chunk_count;c++) {
        const sh_audio_bank_chunk *chunk=codec.chunks+c;
        const char *tag=!strcmp(chunk->tag,"STMG") ? "STMG" : !strcmp(chunk->tag,"ENVS") ? "ENVS" :
            !strcmp(chunk->tag,"PLAT") ? "PLAT" : NULL;
        if(!tag)continue;
        if(used==3){pap_fail(error,capacity,"native audio baseline repeats a shared chunk",bank->path);goto done;}
        bodies[used]=malloc(chunk->length ? chunk->length : 1);if(!bodies[used])goto done;
        if(!pap_original_read(&reader,chunk->offset+8,bodies[used],chunk->length)){free(bodies[used]);bodies[used]=NULL;goto done;}
        inputs[used]=(sh_audio_shared_input){tag,bodies[used],chunk->length,0,1};used++;
    }
    ok=sh_audio_shared_policy_compile(inputs,used,out,error,capacity);
done:
    for(size_t i=0;i<3;i++)free(bodies[i]);
    sh_audio_bank_codec_free(&codec);
    if(!ok && error && capacity && !*error)pap_fail(error,capacity,"cannot compose the native audio baseline",NULL);
    return ok;
}
const sh_audio_shared_policy *sh_package_audio_program_shared(const sh_package_audio_program *p)
{ return p && p->graph ? &p->shared : NULL; }
const sh_audio_shared_policy *sh_package_audio_program_bound_shared(const sh_package_audio_program *p)
{ return p && p->shared_bound ? &p->bound_shared : NULL; }
int sh_package_audio_program_shared_owners(const sh_package_audio_program *p,size_t setting,
    const sh_package_owners **owners,const sh_package_owners **gameplay)
{
    if(owners)*owners=NULL;if(gameplay)*gameplay=NULL;
    if(!p || !p->graph || !owners || !gameplay || setting>=p->shared.count)return 0;
    *owners=&p->shared_owners[setting].owners;*gameplay=&p->shared_owners[setting].gameplay;return 1;
}
int sh_package_audio_program_shared_origin(const sh_package_audio_program *p,
    size_t input,size_t *scope,size_t *bank,int *baseline)
{
    if(!p || !p->graph || input>=p->shared_count || !scope || !bank || !baseline)return 0;
    pap_shared_origin row=p->shared_origins[input];
    *scope=row.scope<p->count ? row.scope : SIZE_MAX;*bank=row.scope<p->count ? row.bank : SIZE_MAX;
    *baseline=row.baseline;return 1;
}
int sh_package_audio_program_contains(const sh_package_audio_program *p, size_t scope, const char *sound)
{ return pap_find(p, scope, sound) != NULL; }
size_t sh_package_audio_program_failed_scope(const sh_package_audio_program *p)
{ return p ? p->failed_scope : SIZE_MAX; }
int sh_package_audio_program_prepare(sh_package_audio_program *p, char *error, size_t capacity)
{
    if (!p) return pap_fail(error, capacity, "audio program is unavailable", NULL);
    if (p->graph) return 1;
    sh_audio_graph_scope *inputs = NULL; sh_audio_link_graph *graph = NULL;
    pap_root_ref *roots = NULL; size_t count = 0, at = 0; int ok = 0;
    p->failed_scope = SIZE_MAX;
    if (p->count >= SIZE_MAX / sizeof(*inputs)) goto done;
    inputs = calloc(p->count+1, sizeof(*inputs)); if (!inputs) goto done;
    for (size_t i = 0; i < p->count; i++) {
        pap_scope *scope = p->scopes + i;
        if (scope->count > SIZE_MAX - count || scope->count > SIZE_MAX / sizeof(uint32_t)) goto done;
        count += scope->count;
        if (!scope->count && !scope->inventory) continue;
        p->failed_scope = i;
        if (!pap_inventory(p, i, error, capacity)) goto done;
        uint32_t *events = malloc((scope->count ? scope->count : 1) * sizeof(*events)); if (!events) goto done;
        for (size_t j = 0; j < scope->count; j++) events[j] = scope->sounds[j].event;
        int captured;
        if (scope->capture) captured = sh_package_audio_capture_extend(scope->capture, events, scope->count, error, capacity);
        else {
            scope->capture = sh_package_audio_capture_open(scope->compiled, p->originals,
                events, scope->count, error, capacity);
            captured = scope->capture != NULL;
        }
        free(events); if (!captured) goto done;
        inputs[i] = *sh_package_audio_capture_input(scope->capture);
        p->failed_scope = SIZE_MAX;
    }
    if(!pap_bus_capture_baseline(p,inputs,error,capacity))goto done;
    if(p->bus_baseline)inputs[p->count]=*sh_package_audio_capture_input(p->bus_baseline);
    graph = sh_audio_link_graph_open(inputs, p->count+(p->bus_baseline!=NULL), error, capacity); if (!graph) goto done;
    if (count > SIZE_MAX / sizeof(*roots)) goto done;
    roots = calloc(count ? count : 1, sizeof(*roots)); if (!roots) goto done;
    for (size_t i = 0; i < p->count; i++) for (size_t j = 0; j < p->scopes[i].count; j++) {
        pap_sound *sound = p->scopes[i].sounds + j;
        if (!sh_audio_link_graph_event(graph, i, sound->event, &sound->root, error, capacity)) {
            p->failed_scope = i; goto done;
        }
        pap_sources sources = {p->scopes[i].compiled, sound};
        if (!sh_audio_link_graph_sources(graph, i, sound->event, pap_source_owners, &sources, error, capacity)) goto done;
        roots[at++].sound = sound;
    }
    qsort(roots, count, sizeof(*roots), pap_root_order);
    for (size_t i = 1; i < count; i++) if (!memcmp(roots[i - 1].sound->root.identity, roots[i].sound->root.identity, 32) &&
        !sh_audio_link_graph_equal(graph, roots[i - 1].sound->root.node, roots[i].sound->root.node)) {
        pap_fail(error, capacity, "audio binding fingerprint collision", roots[i].sound->name); goto done;
    }
    for (size_t i = 0; i < p->count; i++) {
        if (!pap_files_capture(p->scopes + i)) goto done;
    }
    if(!pap_bus_prepare(p,inputs,graph,error,capacity) || !pap_bus_compose(p,graph,error,capacity) ||
        !pap_shared_prepare(p,inputs,graph,error,capacity))goto done;
    for (size_t i = 0; i < p->count; i++) {
        sh_package_audio_capture_seal(p->scopes[i].capture);
        p->scopes[i].compiled = NULL;
    }
    sh_package_audio_capture_seal(p->bus_baseline);
    p->graph = graph; graph = NULL; ok = 1;
done:
    sh_audio_link_graph_close(graph); free(inputs); free(roots);
    if (!ok) {
        pap_bus_clear(p);pap_shared_clear(p);
        sh_package_audio_capture_close(p->bus_baseline);p->bus_baseline=NULL;
        for (size_t i = 0; i < p->count; i++) {
            pap_files_clear(p->scopes + i);
            sh_package_audio_capture_close(p->scopes[i].capture); p->scopes[i].capture = NULL;
            for (size_t j = 0; j < p->scopes[i].count; j++) {
                sh_package_owners_free(&p->scopes[i].sounds[j].owners);
                sh_package_owners_free(&p->scopes[i].sounds[j].gameplay);
            }
        }
        if (error && capacity && !*error) pap_fail(error, capacity, "cannot prepare audio program", NULL);
    }
    return ok;
}
int sh_package_audio_program_root(const sh_package_audio_program *p,
    size_t scope, const char *name, sh_audio_graph_root *out, char *error, size_t capacity)
{
    pap_sound *sound = pap_find(p, scope, name);
    if (!p || !p->graph || !sound || !out)
        return pap_fail(error, capacity, "sound is outside the prepared audio scope", name);
    *out = sound->root; return 1;
}
int sh_package_audio_program_output(sh_package_audio_program *p,
    size_t scope, const char *name, const char *linked, char *error, size_t capacity)
{
    pap_sound *sound = pap_find(p, scope, name); uint32_t ignored;
    if (!p || !p->graph || !sound || !sh_audio_event_identity(linked, &ignored))
        return pap_fail(error, capacity, "invalid linked sound identity", linked);
    if (sound->output) return !strcmp(sound->output, linked) ||
        pap_fail(error, capacity, "captured sound already has a different output", name);
    for (size_t i = 0; i < p->count; i++) for (size_t j = 0; j < p->scopes[i].count; j++) {
        pap_sound *peer = p->scopes[i].sounds + j;
        if (peer->output && !strcmp(peer->output, linked) &&
            !sh_audio_link_graph_equal(p->graph, peer->root.node, sound->root.node))
            return pap_fail(error, capacity, "unequal audio bindings share an output name", linked);
    }
    sound->output = _strdup(linked);
    return sound->output != NULL || pap_fail(error, capacity, "cannot retain linked sound", linked);
}
int sh_package_audio_program_owners(const sh_package_audio_program *p,
    size_t scope, const char *name, const sh_package_owners **owners, const sh_package_owners **gameplay)
{
    if (owners) *owners = NULL;
    if (gameplay) *gameplay = NULL;
    pap_sound *sound = pap_find(p, scope, name);
    if (!p || !p->graph || !sound || !owners || !gameplay) return 0;
    *owners = &sound->owners; *gameplay = &sound->gameplay; return 1;
}
typedef struct pap_binding { sh_compiled_resource *resource; size_t scope; uint32_t event; } pap_binding;
static int pap_bus_bind(const sh_package_audio_program *p,const sh_audio_link_binding *binding,
    sh_audio_bus_policy *out,char *error,size_t capacity)
{
    const sh_audio_bus_policy *input=&p->bus_policy;
    out->buses=calloc(input->bus_count ? input->bus_count : 1,sizeof(*out->buses));
    out->settings=calloc(input->count ? input->count : 1,sizeof(*out->settings));
    if(!out->buses || !out->settings)return pap_fail(error,capacity,"cannot retain bound bus policy",NULL);
    memcpy(out->buses,input->buses,input->bus_count*sizeof(*out->buses));out->bus_count=input->bus_count;
    for(size_t i=0;i<input->count;i++) {
        const sh_audio_bus_setting *source=input->settings+i;sh_audio_bus_setting *target=out->settings+out->count++;
        *target=*source;target->body=NULL;target->contributors=NULL;
        if(source->contributor_count) {
            target->contributors=malloc(source->contributor_count*sizeof(*target->contributors));
            if(!target->contributors)return pap_fail(error,capacity,"cannot retain bound bus contributors",NULL);
            memcpy(target->contributors,source->contributors,source->contributor_count*sizeof(*target->contributors));
        }
        if(source->removed)continue;
        if(source->value>=p->bus_count)return pap_fail(error,capacity,"bus policy has no retained binding",NULL);
        const sh_package_audio_bus_value *value=p->bus_values+source->value;size_t length=0;
        if(!sh_audio_link_binding_bus_value(binding,value->scope,value->bank,value->record,
            &target->body,&length,error,capacity))return 0;
        if(length!=source->field.length)return pap_fail(error,capacity,"bound bus value changed shape",NULL);
    }
    return 1;
}
/* Winning shared values relocate their private references with the same
 * transaction IDs as the bank streams. Baseline witnesses keep original IDs. */
static int pap_shared_bind(const sh_package_audio_program *p,const sh_audio_link_binding *binding,
    sh_audio_shared_policy *out,char *error,size_t capacity)
{
    if(!sh_audio_shared_policy_copy(&p->shared,out))return pap_fail(error,capacity,"cannot retain bound shared audio policy",NULL);
    for(size_t i=0;i<out->count;i++) {
        sh_audio_shared_setting *setting=out->settings+i;
        if(setting->input>=p->shared_count)return pap_fail(error,capacity,"shared audio policy has no retained binding",NULL);
        const pap_shared_origin *origin=p->shared_origins+setting->input;
        const sh_audio_graph_scope *scope=origin->scope<p->count ?
            sh_package_audio_capture_input(p->scopes[origin->scope].capture) : sh_package_audio_capture_input(p->bus_baseline);
        if(!scope || origin->bank>=scope->bank_count || setting->index>=origin->records)
            return pap_fail(error,capacity,"shared audio policy has no retained binding",NULL);
        const sh_audio_bank_chunk *chunk=scope->banks[origin->bank].codec->chunks+origin->chunk;
        unsigned char *body=NULL;size_t length=0;
        if(!sh_audio_link_binding_shared_value(binding,origin->scope,origin->bank,chunk->first_shared_record+setting->index,
            &body,&length,error,capacity))return 0;
        if(length!=setting->record.length){free(body);return pap_fail(error,capacity,"bound shared audio value changed shape",NULL);}
        free(setting->body);setting->body=body;
    }
    return 1;
}
int sh_package_audio_program_bind(sh_package_audio_program *p, sh_package_compilation *compiled,
    sh_audio_graph_assign assign, void *context, char *error, size_t capacity)
{
    pap_binding *bindings = NULL; size_t count = 0; int ok = 0;
    sh_audio_link_binding *transaction=NULL;sh_audio_bus_policy bus={0};sh_audio_shared_policy shared={0};
    if (!p || !p->graph || !compiled || !assign || compiled->resource_count > SIZE_MAX / sizeof(*bindings))
        return pap_fail(error, capacity, "invalid audio publication input", NULL);
    bindings = calloc(compiled->resource_count ? compiled->resource_count : 1, sizeof(*bindings));
    if (!bindings) goto done;
    /* Validate without calling sound_prepare: even its initial scalars must not
     * change when a later reservation fails. The outer compiler prepares them. */
    for (size_t f = 0; f < compiled->resource_count; f++) {
        sh_compiled_resource *r = compiled->resources + f;
        const char *family = r->native_type ? r->native_type : r->type;
        const char *name = r->native_name ? r->native_name : r->name;
        if (!family || strcmp(family, "sound")) continue;
        pap_sound *found = NULL; size_t scope = 0;
        for (size_t i = 0; i < p->count; i++) for (size_t j = 0; j < p->scopes[i].count; j++) {
            pap_sound *sound = p->scopes[i].sounds + j;
            if (name && sound->output && !strcmp(sound->output, name)) { found = sound; scope = i; break; }
        }
        sh_compiled_resource probe = *r;
        if (!found || !sh_package_sound_rebind(&probe, found->event, found->event)) {
            pap_fail(error, capacity, "compiled sound has no captured event output", name); goto done;
        }
        bindings[count++] = (pap_binding){r, scope, found->event};
    }
    transaction=sh_audio_link_graph_prepare_bind(p->graph,assign,context,error,capacity);
    if(!transaction || !pap_bus_bind(p,transaction,&bus,error,capacity) ||
        !pap_shared_bind(p,transaction,&shared,error,capacity))goto done;
    /* Every root was proved during prepare; a successful bind assigns all
     * roots atomically. No fallible allocation or validation remains here. */
    sh_audio_link_graph_commit_bind(transaction);transaction=NULL;
    sh_audio_bus_policy_free(&p->bound_bus_policy);p->bound_bus_policy=bus;memset(&bus,0,sizeof(bus));p->bus_bound=1;
    sh_audio_shared_policy_free(&p->bound_shared);p->bound_shared=shared;memset(&shared,0,sizeof(shared));p->shared_bound=1;
    for (size_t i = 0; i < count; i++) {
        uint32_t event = 0;
        (void)sh_audio_link_graph_event_id(p->graph, bindings[i].scope, bindings[i].event, &event);
        bindings[i].resource->sound_event = event;
    }
    ok = 1;
done:
    sh_audio_link_graph_cancel_bind(transaction);sh_audio_bus_policy_free(&bus);sh_audio_shared_policy_free(&shared);
    free(bindings);
    if (!ok && error && capacity && !*error) pap_fail(error, capacity, "cannot bind audio program", NULL);
    return ok;
}
size_t sh_package_audio_program_banks(const sh_package_audio_program *p, size_t scope)
{
    const sh_audio_graph_scope *input = p && p->graph && scope < p->count ?
        sh_package_audio_capture_input(p->scopes[scope].capture) : NULL;
    return input ? input->bank_count : 0;
}
size_t sh_package_audio_program_scopes(const sh_package_audio_program *p)
{ return p && p->graph ? p->count : 0; }
const sh_audio_graph_bank *sh_package_audio_program_bank(const sh_package_audio_program *p,
    size_t scope, size_t index, const sh_audio_bank_binding **binding)
{
    if (binding) *binding = NULL;
    if (index >= sh_package_audio_program_banks(p, scope)) return NULL;
    if (binding) *binding = sh_audio_link_graph_bank(p->graph, scope, index);
    return sh_package_audio_capture_input(p->scopes[scope].capture)->banks + index;
}
int sh_package_audio_program_loads(const sh_package_audio_program *p,
    int (*visit)(void *, uint32_t id, const char *filename, int numeric), void *context,
    char *error, size_t capacity)
{
    if(!p || !p->graph || !visit)return pap_fail(error,capacity,"invalid audio activation plan request",NULL);
    for(size_t s=0;s<p->count;s++)for(size_t j=0;j<sh_package_audio_program_banks(p,s);j++) {
        const sh_audio_bank_binding *binding=NULL;uint32_t bound=0,numeric=0;
        const sh_audio_graph_bank *bank=sh_package_audio_program_bank(p,s,j,&binding);
        const char *leaf=bank ? strrchr(bank->path,'/') : NULL;
        if(!bank || !binding || !leaf || !sh_audio_link_graph_bank_id(p->graph,s,bank->codec->metadata.id,&bound))
            return pap_fail(error,capacity,"audio program has not been bound",bank ? bank->path : NULL);
        leaf++;
        /* A relocated bank is served under its decimal identity; the native
         * name loader would hash that stem. Named originals keep their name. */
        int by_id=bound!=bank->codec->metadata.id || (sh_audio_bank_numeric_identity(leaf,&numeric) && numeric==bound);
        if(!visit(context,bound,leaf,by_id)) {
            if(error && capacity && !*error)pap_fail(error,capacity,"cannot plan audio bank activation",bank->path);
            return 0;
        }
    }
    return 1;
}
typedef struct pap_media_visit {
    const sh_package_audio_program *program;
    size_t scope;
    int (*visit)(void *, uint32_t, uint32_t, const sh_audio_graph_media *);
    void *context;
} pap_media_visit;
static int pap_media(void *context, uint32_t authored, const sh_audio_graph_media *media)
{
    pap_media_visit *walk = context; uint32_t bound;
    return sh_audio_link_graph_media_id(walk->program->graph, walk->scope, authored, &bound) &&
        walk->visit(walk->context, authored, bound, media);
}
int sh_package_audio_program_media(const sh_package_audio_program *p, size_t scope,
    int (*visit)(void *, uint32_t, uint32_t, const sh_audio_graph_media *),
    void *context, char *error, size_t capacity)
{
    if (!p || !p->graph || scope >= p->count || !visit)
        return pap_fail(error, capacity, "invalid audio media output request", NULL);
    pap_media_visit walk = {p, scope, visit, context};
    return sh_package_audio_capture_media(p->scopes[scope].capture, pap_media, &walk) ||
        pap_fail(error, capacity, "audio media is unbound or its output failed", NULL);
}

typedef struct pap_output {
    sh_package_compilation *files;
    struct pap_path { size_t scope, resource; const char *source; } *paths;
    size_t path_count, scope;
    const sh_package_audio_program *program;
    const char *directory;
    char *error;
    size_t capacity;
} pap_output;
typedef struct pap_stream { sh_audio_bank_source source; const sh_audio_bank_binding *binding; } pap_stream;
static int pap_stream_write(void *context, int (*write)(void *, const void *, size_t),
    void *sink, char *error, size_t capacity)
{
    pap_stream *stream = context;
    if (stream->binding) return sh_audio_bank_binding_write(stream->source, stream->binding,
        write, sink, error, capacity);
    unsigned char buffer[65536];
    for (uint64_t offset = 0; offset < stream->source.length;) {
        size_t length = stream->source.length - offset < sizeof(buffer) ?
            (size_t)(stream->source.length - offset) : sizeof(buffer);
        if (!stream->source.read(stream->source.context, offset, buffer, length) || !write(sink, buffer, length))
            return pap_fail(error, capacity, "cannot stream prepared audio media", NULL);
        offset += length;
    }
    return 1;
}
static char *pap_output_path(const char *path, uint32_t authored, uint32_t bound)
{
    if (authored == bound) return _strdup(path);
    const char *leaf = strrchr(path, '/'), *extension = strrchr(path, '.');
    if (!leaf || !extension || extension <= leaf || !bound) return NULL;
    size_t prefix = (size_t)(leaf + 1 - path), suffix = strlen(extension);
    if (prefix > SIZE_MAX - suffix - 12) return NULL;
    char *out = malloc(prefix + suffix + 12);
    if (out) {
        memcpy(out, path, prefix);
        snprintf(out + prefix, suffix + 12, "%lu%s", (unsigned long)bound, extension);
    }
    return out;
}
static const pap_file *pap_authored_file(const pap_scope *scope, const char *path)
{
    /* Compiler resources, and this filtered copy, are sorted by engine path. */
    size_t lo = 0, hi = scope->file_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = strcmp(scope->files[mid].path, path);
        if (!order) return scope->files + mid;
        if (order < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static int pap_output_file(pap_output *out, const char *path, sh_audio_bank_source source,
    int original, uint32_t authored, uint32_t bound, const sh_audio_bank_binding *binding)
{
    const pap_file *file = pap_authored_file(out->program->scopes + out->scope, path);
    /* Unchanged SnapMap bytes stay in the native archives. Campaign/new files
     * still need their delivery provider even if an original witness exists. */
    if (original && authored == bound && (!binding || !binding->count) &&
        (!file || file->baseline == 1)) return 1;
    size_t index = out->files->resource_count;
    if (index == SIZE_MAX / sizeof(*out->files->resources) ||
        out->path_count == SIZE_MAX / sizeof(*out->paths)) return 0;
    sh_compiled_resource *grown = realloc(out->files->resources, (index + 1) * sizeof(*grown));
    if (!grown) return 0;
    out->files->resources = grown;
    sh_compiled_resource *r = grown + out->files->resource_count++;
    memset(r, 0, sizeof(*r)); r->source = SIZE_MAX; r->owns_engine_path = 1;
    r->engine_path = pap_output_path(path, authored, bound); r->original_path = _strdup(path);
    r->baseline_known = file ? file->baseline : original;
    r->native_original = original && authored == bound && (!binding || !binding->count);
    if (!r->engine_path || !r->original_path || (file &&
        (!sh_package_owners_union(&r->owners, &file->owners) ||
         !sh_package_owners_union(&r->gameplay_owners, &file->gameplay)))) return 0;
    pap_stream stream = {source, binding};
    r->cache_file = sh_package_file_generate(out->directory, binding ? binding->output_length : source.length,
        NULL, pap_stream_write, &stream, &r->cache_path, out->error, out->capacity);
    if (!r->cache_file) return 0;
    /* The cache generator proves exact bytes before returning a shared path.
     * Two different outputs at one engine identity are an allocator error. */
    for (size_t i = 0; i < index; i++) if (!strcmp(grown[i].engine_path, r->engine_path)) {
        if (strcmp(grown[i].cache_path, r->cache_path) && sh_package_file_equal(grown[i].cache_file, r->cache_file) != 1)
            return pap_fail(out->error, out->capacity, "audio reservation produced unequal files at one identity", r->engine_path);
        if (!sh_package_owners_union(&grown[i].owners, &r->owners) ||
            !sh_package_owners_union(&grown[i].gameplay_owners, &r->gameplay_owners)) return 0;
        /* One numeric bank may have equivalent named and numeric source paths.
         * Choose the stable availability spelling; authored bundles stay intact. */
        if (strcmp(r->original_path, grown[i].original_path) < 0) {
            char *swap = r->original_path; r->original_path = grown[i].original_path; grown[i].original_path = swap;
        }
        free((void *)r->engine_path); free(r->original_path); free(r->cache_path);
        sh_package_file_release(r->cache_file);
        sh_package_owners_free(&r->owners); sh_package_owners_free(&r->gameplay_owners);
        memset(r, 0, sizeof(*r)); out->files->resource_count--; index = i; break;
    }
    struct pap_path *paths = realloc(out->paths, (out->path_count + 1) * sizeof(*paths));
    if (!paths) return 0;
    out->paths = paths;
    paths[out->path_count++] = (struct pap_path){out->scope, index, path}; return 1;
}
static int pap_output_media(void *context, uint32_t authored, uint32_t bound, const sh_audio_graph_media *media)
{
    pap_output *out = context;
    return pap_output_file(out, media->path, media->source, media->original, authored, bound, NULL);
}
static int pap_path_order(const void *a, const void *b)
{
    const struct pap_path *left = a, *right = b;
    if (left->scope != right->scope) return left->scope < right->scope ? -1 : 1;
    return strcmp(left->source, right->source);
}
static const struct pap_path *pap_path_find(const pap_output *out, size_t scope, const char *source)
{
    struct pap_path key = {scope, 0, source};
    return out->path_count ? bsearch(&key, out->paths, out->path_count, sizeof(key), pap_path_order) : NULL;
}
typedef struct pap_dependency { const pap_output *output; size_t scope; sh_compiled_resource *resource; } pap_dependency;
static int pap_output_dependency(void *context, const char *path, uint64_t offset, uint64_t length)
{
    pap_dependency *walk = context; sh_compiled_resource *r = walk->resource; (void)offset; (void)length;
    const struct pap_path *mapping = pap_path_find(walk->output, walk->scope, path);
    if (mapping) {
        const char *target = walk->output->files->resources[mapping->resource].engine_path;
        if (!strcmp(target, r->engine_path)) return 1;
        for (size_t j = 0; j < r->bound_dependency_count; j++)
            if (!strcmp(r->bound_dependencies[j], target)) return 1;
        if (r->bound_dependency_count == SIZE_MAX / sizeof(*r->bound_dependencies)) return 0;
        char *copy = _strdup(target);
        char **grown = copy ? realloc(r->bound_dependencies, (r->bound_dependency_count + 1) * sizeof(*grown)) : NULL;
        if (!grown) { free(copy); return 0; }
        r->bound_dependencies = grown; grown[r->bound_dependency_count++] = copy; return 1;
    }
    return 1; /* Unchanged native originals do not acquire a private lifetime. */
}
int sh_package_audio_program_materialize(const sh_package_audio_program *p,
    sh_package_compilation **target, const char *directory, char *error, size_t capacity)
{
    sh_package_compilation *result = NULL, *compiled = target ? *target : NULL;
    sh_package_resource_projection *rows = NULL;
    const sh_compiled_resource **inputs = NULL;
    pap_output out = {0}; size_t count = 0; int ok = 0;
    if (error && capacity) *error = 0;
    if (!p || !p->graph || !compiled || !compiled->sources || !directory)
        return pap_fail(error, capacity, "invalid prepared audio provider", NULL);
    out.files = calloc(1, sizeof(*out.files)); if (!out.files) goto done;
    out.program = p; out.directory = directory; out.error = error; out.capacity = capacity;
    for (size_t i = 0; i < p->count; i++) {
        out.scope = i;
        for (size_t j = 0; j < sh_package_audio_program_banks(p, i); j++) {
            const sh_audio_bank_binding *binding = NULL; uint32_t bound;
            const sh_audio_graph_bank *bank = sh_package_audio_program_bank(p, i, j, &binding);
            if (!binding || !sh_audio_link_graph_bank_id(p->graph, i, bank->codec->metadata.id, &bound)) {
                pap_fail(error, capacity, "audio program has not been bound", bank->path); goto done;
            }
            binding=sh_audio_link_graph_private_bank(p->graph,i,j);
            if (!pap_output_file(&out, bank->path, bank->source, bank->original,
                bank->codec->metadata.id, bound, binding)) goto done;
        }
        if (!sh_package_audio_program_media(p, i, pap_output_media, &out, error, capacity)) goto done;
    }
    if (out.path_count > 1) qsort(out.paths, out.path_count, sizeof(*out.paths), pap_path_order);
    if (compiled->resource_count > SIZE_MAX - out.files->resource_count) goto done;
    size_t maximum = compiled->resource_count + out.files->resource_count;
    if (maximum > SIZE_MAX / sizeof(*rows) || maximum > SIZE_MAX / sizeof(*inputs)) goto done;
    rows = calloc(maximum ? maximum : 1, sizeof(*rows)); inputs = calloc(maximum ? maximum : 1, sizeof(*inputs));
    if (!rows || !inputs) goto done;
    for (size_t i = 0; i < maximum; i++) {
        const sh_compiled_resource *r = i < compiled->resource_count ? compiled->resources + i :
            out.files->resources + i - compiled->resource_count;
        if (i < compiled->resource_count && sh_package_audio_path_kind(r->engine_path)) continue;
        inputs[count] = r;
        rows[count] = (sh_package_resource_projection){r->engine_path, inputs + count, 1, NULL, 0,
            r->native_type, r->native_name, 0}; count++;
    }
    result = sh_package_compilation_project(compiled->sources, rows, count, &compiled->policy,
        compiled->canonical_path, error, capacity);
    if (!result) goto done;
    /* Rebinding an unpublished compilation replaces old audio dependency edges.
     * All other bound native dependencies survive the projection unchanged. */
    for (size_t i = 0; i < result->resource_count; i++) {
        sh_compiled_resource *r = result->resources + i; size_t kept = 0;
        for (size_t j = 0; j < r->bound_dependency_count; j++) {
            if (sh_package_audio_path_kind(r->bound_dependencies[j])) free(r->bound_dependencies[j]);
            else r->bound_dependencies[kept++] = r->bound_dependencies[j];
        }
        r->bound_dependency_count = kept;
    }
    for (size_t i = 0; i < p->count; i++) {
        for (size_t j = 0; j < p->scopes[i].count; j++) {
            const pap_sound *sound = p->scopes[i].sounds + j; uint32_t event;
            if (!sound->output) continue;
            sh_compiled_resource *r = NULL;
            for (size_t k = 0; k < result->resource_count; k++)
                if (sh_package_resource_native_matches(result->resources + k, "sound", sound->output)) { r = result->resources + k; break; }
            if (!r || !sh_audio_link_graph_event_id(p->graph, i, sound->event, &event) || r->sound_event != event) {
                pap_fail(error, capacity, "prepared sound disagrees with its provider binding", sound->output); goto done;
            }
            pap_dependency walk = {&out, i, r};
            if (!sh_audio_link_graph_sources(p->graph, i, sound->event, pap_output_dependency, &walk, error, capacity)) goto done;
        }
        for (size_t j = 0; j < sh_package_audio_program_banks(p, i); j++) {
            const sh_audio_graph_bank *bank = sh_package_audio_program_bank(p, i, j, NULL);
            const struct pap_path *mapping = pap_path_find(&out, i, bank->path);
            if (mapping) {
                sh_compiled_resource *r = (sh_compiled_resource *)sh_package_compilation_find(result,
                    out.files->resources[mapping->resource].engine_path);
                pap_dependency walk = {&out, i, r};
                if (!r || !sh_audio_link_graph_bank_sources(p->graph, i, bank->codec->metadata.id,
                    pap_output_dependency, &walk, error, capacity)) goto done;
            }
        }
    }
    if (compiled->duplicate_count > SIZE_MAX - result->duplicate_count) goto done;
    /* Captured graphs retain their original recipes for future exact identity
     * checks, but editable package paths must not remain locked by a provider. */
    for (size_t i = 0; i < p->count; i++)
        if (!sh_package_audio_capture_cache(p->scopes[i].capture, directory, error, capacity)) goto done;
    result->duplicate_count += compiled->duplicate_count;
    result->map_overlay = compiled->map_overlay; result->map_owner_begin = compiled->map_owner_begin;
    result->independent_sources = compiled->independent_sources;
    result->map_independent_sources = compiled->map_independent_sources;
    result->owns_sources = compiled->owns_sources; compiled->owns_sources = 0;
    *target = result; result = NULL; sh_package_compilation_free(compiled); ok = 1;
done:
    free(inputs); free(rows); free(out.paths);
    sh_package_compilation_free(result); sh_package_compilation_free(out.files);
    if (!ok && error && capacity && !*error) pap_fail(error, capacity, "cannot materialize prepared audio provider", NULL);
    return ok;
}
