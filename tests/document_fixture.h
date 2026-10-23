/* The runtime test's typed-entity admission registry, the historical writers' stock baseline and two helpers,
 * shared by package_document_test and published_source_test. Include after package_fixture.h. */
/* The runtime test's typed-entity registry: entityDef with target/custom/health. */
typedef struct admission_region { void *address; size_t length; } admission_region;
typedef struct admission_fixture {
    admission_region regions[64]; size_t count;
    void *registry, *reflection;
} admission_fixture;
static void *admission_alloc(admission_fixture *f, size_t length)
{
    void *p = calloc(1, length); CHECK(p && f->count < 64);
    if (p && f->count < 64) f->regions[f->count++] = (admission_region){p, length};
    return p;
}
static void admission_pointer(void *p, size_t at, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)p + at, &address, 8); }
static void admission_number(void *p, size_t at, int value)
{ memcpy((char *)p + at, &value, 4); }
static char *admission_string(admission_fixture *f, const char *text)
{ char *copy = admission_alloc(f, strlen(text) + 1); if (copy) strcpy(copy, text); return copy; }
static int admission_read(void *context, uintptr_t address, void *out, size_t length)
{
    admission_fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (void *)address, length); return 1;
        }
    }
    return 0;
}
static void admission_setup(admission_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "OpaqueStringReader", "idDeclTypeInfo", "int"};
    const char *types[] = {"idDeclEntityDef", "OpaqueStringReader", "int"};
    const char *ops[] = {"*", "", ""}, *names[] = {"target", "custom", "health"};
    void *manager, *managers, *container, *records, *objects, *pointers, *fields;
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18); managers = admission_alloc(f, 8);
    manager = admission_alloc(f, 0x90); admission_pointer(managers, 0, manager);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 1);
    admission_pointer(manager, 8, admission_string(f, "entityDef"));
    admission_pointer(manager, 0x10, admission_string(f, classes[0]));
    f->reflection = admission_alloc(f, 0xb0); container = admission_alloc(f, 0x30);
    records = admission_alloc(f, 6 * 0x38); objects = admission_alloc(f, 6 * 16);
    pointers = admission_alloc(f, 6 * 16); fields = admission_alloc(f, 4 * 0x48);
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 6);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 6);
    for (size_t i = 0; i < 5; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, admission_string(f, ""));
    }
    admission_pointer(pointers, 8, (void *)0x10);
    admission_pointer(objects, 2 * 16 + 8, (void *)0x20);
    admission_pointer(records, 0x38 + 0x20, fields);
    for (size_t i = 0; i < 3; i++) {
        admission_pointer(fields, i * 0x48, admission_string(f, types[i]));
        admission_pointer(fields, i * 0x48 + 8, admission_string(f, ops[i]));
        admission_pointer(fields, i * 0x48 + 0x10, admission_string(f, names[i]));
        admission_number(fields, i * 0x48 + 0x1c, 8);
    }
}

/* The stock entity the historical package overrides. */
static const char *historical_root = "generated/decls/entitydef/history/root.decl";
static int historical_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, historical_root) ? "{ class = \"Entity\"; edit = { health = 100; target = \"\"; } }" :
        !strcmp(path, "generated/decls/entitydef/worldspawn.decl") ?
            "{ class = \"idWorldspawn\"; edit = { globalAIsettings = \"default\"; } }" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* The historical context compiles its entities under private names and paths;
 * find one by an authored value. */
static const sh_compiled_resource *authored_resource(const sh_package_compilation *compiled, const char *value)
{
    if (compiled) for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->body && strstr((const char *)r->body, value)) return r;
    }
    return NULL;
}

static char *heap_copy(const char *bytes, size_t length)
{
    char *copy = HeapAlloc(GetProcessHeap(), 0, length + 1);
    if (copy) { memcpy(copy, bytes, length); copy[length] = 0; }
    return copy;
}
