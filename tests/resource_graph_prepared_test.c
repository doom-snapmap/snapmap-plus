#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/resource_graph_prepared.h"
#include "../src/backend/resource_graph.h"

typedef struct region { void *p; size_t length; } region;
typedef struct fixture {
    region regions[128]; size_t count;
    unsigned char *registry, *reflection, *root;
    uintptr_t unreadable;
    int replace_generation;
} fixture;
static void *alloc(fixture *f, size_t n)
{ void *p = calloc(1, n); assert(p && f->count < 128); f->regions[f->count++] = (region){p,n}; return p; }
static void ptr(void *p, size_t offset, const void *value)
{ uintptr_t a = (uintptr_t)value; memcpy((char *)p + offset, &a, sizeof(a)); }
static void num(void *p, size_t offset, int32_t n) { memcpy((char *)p + offset, &n, 4); }
static char *str(fixture *f, const char *s) { char *p = alloc(f, strlen(s)+1); strcpy(p,s); return p; }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = context; size_t i;
    if (address == f->unreadable) return 0;
    if (f->replace_generation && address == (uintptr_t)f->root + 0x18) {
        sh_resource_graph_frame source;
        f->replace_generation = 0;
        sh_resource_graph_begin(&source, "entitydef", "root");
        sh_resource_graph_file("replacement"); sh_resource_graph_end(&source, 1);
    }
    for (i=0;i<f->count;i++) {
        uintptr_t a = (uintptr_t)f->regions[i].p;
        if (address >= a && address-a <= f->regions[i].length && length <= f->regions[i].length-(size_t)(address-a)) {
            memcpy(out,(void *)address,length); return 1;
        }
    }
    return 0;
}
static void setup(fixture *f, const char *body)
{
    static const char *classes[] = {"idDeclEntityDef", "Entity", "OpaqueStringReader"};
    void *container, *records, *objects, *pointers, *fields, *manager, *managers, *entries, *child;
    size_t i;
    memset(f,0,sizeof(*f));
    f->registry=alloc(f,0x18); managers=alloc(f,8); manager=alloc(f,0x90);
    ptr(f->registry,8,managers); num(f->registry,0x10,1); ptr(managers,0,manager);
    ptr(manager,8,str(f,"entityDef")); ptr(manager,0x10,str(f,"idDeclEntityDef"));
    entries=alloc(f,16); ptr(manager,0x20,entries); num(manager,0x28,2);
    f->root=alloc(f,0x168); child=alloc(f,0x168); ptr(entries,0,f->root); ptr(entries,8,child);
    ptr(f->root,8,str(f,"root")); ptr(child,8,str(f,"child"));
    ptr(f->root,0x10,manager); ptr(child,0x10,manager);
    ptr(f->root,0x60,str(f,"Entity")); ptr(f->root,0x140,str(f,body));
    num(f->root,0x138,(int32_t)strlen(body)); f->root[0x128]=1;
    f->reflection=alloc(f,0xb0); container=alloc(f,0x30); records=alloc(f,4*0x38);
    objects=alloc(f,4*16); pointers=alloc(f,4*16);
    ptr(f->reflection,0,container); ptr(container,0x20,records);
    ptr(f->reflection,0x88,objects); num(f->reflection,0x90,4);
    ptr(f->reflection,0xa0,pointers); num(f->reflection,0xa8,4);
    for(i=0;i<3;i++) {
        ptr(records,i*0x38,str(f,classes[i])); ptr(records,i*0x38+8,str(f,""));
    }
    ptr(pointers,8,(void *)0x10); ptr(objects,2*16+8,(void *)0x20);
    fields=alloc(f,3*0x48); ptr(records,0x38+0x20,fields);
    ptr(fields,0,str(f,"idDeclEntityDef")); ptr(fields,8,str(f,"*"));
    ptr(fields,0x10,str(f,"target")); num(fields,0x1c,8);
    ptr(fields,0x48,str(f,"OpaqueStringReader")); ptr(fields,0x48+8,str(f,""));
    ptr(fields,0x48+0x10,str(f,"custom")); num(fields,0x48+0x1c,8);
}
static sh_resource_graph_preparation *open_preparation(fixture *f)
{
    sh_decl_registry_source source={f,read_memory,(uintptr_t)f->registry,NULL};
    return sh_resource_graph_prepare_open(source,(uintptr_t)f->reflection);
}
static void seed(const char *type, const char *name)
{ sh_resource_graph_frame f; sh_resource_graph_begin(&f,type,name); sh_resource_graph_end(&f,1); }
static int visit(void *context,const char *pt,const char *pn,const char *type,const char *name)
{
    unsigned *bits=context; (void)pt;(void)pn;(void)type;
    if(!strcmp(name,"child")) *bits|=1;
    if(!strcmp(name,"native")) *bits|=2;
    if(!strcmp(name,"replacement")) *bits|=4;
    return 1;
}
static unsigned edges(void) { unsigned bits=0; sh_resource_graph_walk("entitydef","root",visit,&bits); return bits; }
static int inline_reference(void *context, const char *type, const char *name)
{
    unsigned *bits = context;
    assert(!strcmp(type, "entityDef"));
    if (*bits == 128) return 0;
    if (!strcmp(name, "child")) *bits |= 1;
    if (!strcmp(name, "root")) *bits |= 2;
    return 1;
}
static void inline_checks(void)
{
    fixture f;
    sh_resource_graph_preparation *p;
    sh_decl_dependency_result result;
    sh_resource_graph_frame enclosing;
    unsigned bits = 0;
    const char *json = "{\"target\":\"child\"}";
    setup(&f, "edit = { target = \"root\"; }"); p = open_preparation(&f); assert(p);
    /* Both explicit and inherited class resolution use the native registry.
     * A surrounding asset parse must not inherit this instance's child. */
    sh_resource_graph_begin(&enclosing, "entitydef", "root");
    assert(sh_resource_graph_prepare_inline(p, "Entity", "root", json, strlen(json),
        inline_reference, &bits, &result));
    assert(bits == 1 && result.references == 1 && !result.gaps);
    sh_resource_graph_file("native"); sh_resource_graph_end(&enclosing, 1);
    assert(edges() == 2);
    bits = 0;
    assert(sh_resource_graph_prepare_inline(p, "", "root", json, strlen(json),
        inline_reference, &bits, &result) && bits == 1 && edges() == 2);
    bits = 0; json = "{\"target\":\"root\",\"custom\":\"unresolved\"}";
    assert(!sh_resource_graph_prepare_inline(p, "Entity", "", json, strlen(json),
        inline_reference, &bits, &result));
    assert(bits == 2 && result.gaps == 1 && !result.aborted && edges() == 2);
    bits = 128; json = "{\"target\":\"child\"}";
    assert(!sh_resource_graph_prepare_inline(p, "Entity", "", json, strlen(json),
        inline_reference, &bits, &result) && result.aborted);
    bits = 0;
    assert(sh_resource_graph_prepare_inline(p, "Entity", "", json, strlen(json),
        inline_reference, &bits, &result) && bits == 1);
    bits = 0; json = "{\"target\":\"absent\"}";
    assert(!sh_resource_graph_prepare_inline(p, "Entity", "", json, strlen(json),
        inline_reference, &bits, &result) && !bits && result.gaps && !result.aborted);
    assert(!sh_resource_graph_prepare_inline(p, "", "absent", json, strlen(json),
        inline_reference, &bits, &result) && !bits && result.gaps && !result.aborted);
    sh_resource_graph_prepare_close(p);
    while (f.count) free(f.regions[--f.count].p);
    sh_resource_graph_test_reset();
}
static void cleanup(fixture *f,sh_resource_graph_preparation *p)
{ sh_resource_graph_prepare_close(p); while(f->count) free(f->regions[--f->count].p); sh_resource_graph_test_reset(); }
int main(void)
{
    fixture f; sh_resource_graph_preparation *p; sh_decl_dependency_result result;
    sh_resource_graph_frame native;
    setup(&f,"edit = { target = \"child\"; }"); p=open_preparation(&f); assert(p);
    seed("entitydef","root");
    assert(sh_resource_graph_prepare_entity(p,"root",&result) && result.references==1 && !result.gaps);
    assert(edges()==1);
    sh_resource_graph_begin_state(&native,"entitydef","root",1);
    sh_resource_graph_file("native"); sh_resource_graph_end(&native,1);
    assert(!sh_resource_graph_prepare_entity(p,"root",&result) && !result.visited && edges()==2);
    seed("entitydef","root"); f.root[0x2c]=2;
    assert(!sh_resource_graph_prepare_entity(p,"root",&result) && !edges());
    f.root[0x2c]=0; f.replace_generation=1;
    assert(sh_resource_graph_prepare_entity(p,"root",&result));
    assert(edges()==4); /* Old-generation static edges must not publish. */
    cleanup(&f,p);

    setup(&f,"edit = { target = \"child\"; custom = \"unknown\"; }");
    p=open_preparation(&f); assert(p); seed("entitydef","root");
    assert(!sh_resource_graph_prepare_entity(p,"root",&result) && result.gaps==1 && edges()==1);
    f.unreadable=(uintptr_t)f.root+0x60;
    assert(!sh_resource_graph_prepare_entity(p,"root",&result) && edges()==1);
    cleanup(&f,p);
    inline_checks();
    puts("resource_graph_prepared_test: PASS"); return 0;
}
