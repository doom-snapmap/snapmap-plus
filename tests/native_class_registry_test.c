/* Controls for the read-only idClass registry view: readiness, conclusive
 * absence versus unreadable state, the constructor's verdict order, signed
 * idStr::Cmp ordering, and that no factory is ever called. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/native_class_registry.h"

enum { RECORDS = 5 };
static unsigned char g_records[RECORDS][0x70];
static uintptr_t g_sorted[RECORDS], g_other[RECORDS], g_types_slot;
static int g_count, g_first = 10, g_last = 20;
static unsigned char g_ready;
static int g_factory_ran;
static void *fake_factory(void) { g_factory_ran = 1; return NULL; }

static void record(int at, const char *name, int factory, int abstract_class, int type_num, int last_child)
{
    uintptr_t name_address = (uintptr_t)name, factory_address = factory ? (uintptr_t)fake_factory : 0;
    memset(g_records[at], 0, sizeof(g_records[at]));
    memcpy(g_records[at] + 0x00, &name_address, sizeof(name_address));
    memcpy(g_records[at] + 0x28, &factory_address, sizeof(factory_address));
    g_records[at][0x60] = (unsigned char)abstract_class;
    memcpy(g_records[at] + 0x68, &type_num, sizeof(type_num));
    memcpy(g_records[at] + 0x6c, &last_child, sizeof(last_child));
    g_sorted[at] = (uintptr_t)g_records[at];
}
/* Sorted by idStr::Cmp: signed bytes, so a high-bit first byte sorts first
 * and the comparison is case-sensitive. */
static void reset(void)
{
    record(0, "\xC3xotic", 1, 0, 12, 12);
    record(1, "Abstract", 1, 1, 13, 13);
    record(2, "Entity", 1, 0, 11, 14);
    record(3, "Unmanaged", 1, 0, 30, 30);
    record(4, "entity", 1, 0, 15, 15);
    g_types_slot = (uintptr_t)g_sorted; g_count = RECORDS; g_ready = 1; g_first = 10; g_last = 20;
}

/* A callback source that changes the registry after a number of reads. */
static int g_reads, g_flip_at, g_flip_kind, g_short_read;
static int changing_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    if (g_short_read) return 2;
    memcpy(out, (const void *)address, length);
    if (++g_reads == g_flip_at) {
        if (g_flip_kind == 0) g_ready = 0;
        else if (g_flip_kind == 1) g_count = RECORDS - 1;
        else { memcpy(g_other, g_sorted, sizeof(g_other)); g_types_slot = (uintptr_t)g_other; }
    }
    return 1;
}

static sh_native_construction verdict(const sh_native_class_source *source, const char *name)
{
    return sh_native_class_construction(source, name, NULL);
}

/* ---- name comparison reads ----
 * Record names come through the source, which copies exactly the bytes
 * requested (the process source copies any mapped memory). The comparison
 * reads a name only up to its first difference from the key or its
 * terminator: exact-size names (one byte, equal, prefix, longer, early
 * difference, signed high bytes, long) through a strict reader and through the
 * unbounded process source, a name ending at an inaccessible page, an
 * unterminated name, an unreadable one and one at the top of the address
 * space. A byte the comparison needs and cannot read makes the lookup
 * unavailable; a registry without the key stays conclusively absent. */
typedef struct name_source {
    void *owned[24]; size_t sizes[24], count, past_end, faults, wrapped;
    const char *top;  /* bytes served at the top of the address space */
    size_t top_length;
} name_source;
static void *name_alloc(name_source *s, size_t size)
{
    void *p = calloc(1, size);
    assert(p && s->count < 24);
    s->owned[s->count] = p; s->sizes[s->count++] = size; return p;
}
static int name_read(void *context, uintptr_t address, void *out, size_t length)
{
    name_source *s = context;
    uintptr_t top = UINTPTR_MAX - (s->top_length - 1);
    if (s->top_length && address >= top) {
        if (length - 1 > UINTPTR_MAX - address) { s->wrapped++; return 0; }
        memcpy(out, s->top + (address - top), length); return 1;
    }
    for (size_t i = 0; i < s->count; i++) {
        uintptr_t start = (uintptr_t)s->owned[i];
        if (address >= start && address - start < s->sizes[i]) {
            if (length > s->sizes[i] - (size_t)(address - start)) { s->past_end++; return 0; }
            break;
        }
    }
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { s->faults++; return 0; }
}
/* An exact-size name record for the sorted array (managed, factory-less is
 * enough for lookup). */
static uintptr_t name_record(name_source *s, uintptr_t name)
{
    unsigned char *record = name_alloc(s, 0x70);
    int number = 11;
    memcpy(record, &name, sizeof(name));
    memcpy(record + 0x68, &number, sizeof(number)); memcpy(record + 0x6c, &number, sizeof(number));
    return (uintptr_t)record;
}
static char *name_text(name_source *s, const char *text)
{
    char *p = name_alloc(s, strlen(text) + 1);
    strcpy(p, text); return p;
}
static void test_name_reads(const sh_native_class_source *process, unsigned char *reserved)
{
    static const char *const sorted[] = {"\xC3\xA9t\xC3\xA9", "A", "Abstract", "AbstractLonger", NULL, NULL,
        "Z\xC3\xA9", "Zz"};
    static const char *const absent[] = {"", "Abs", "Abstracts", "AbstractLongerStill", "B", "\xC3\xA9t", "Zz\xC3\xA9",
        "\x80"};
    name_source s = {0};
    sh_native_class_source strict = *process;
    uintptr_t *array;
    size_t n = sizeof(sorted) / sizeof(sorted[0]);
    char *names[8];
    SYSTEM_INFO system;
    DWORD previous;
    unsigned char *page;
    sh_native_class found;
    strict.read = name_read; strict.context = &s;
    names[4] = name_alloc(&s, 64); memset(names[4], 'M', 63);
    names[5] = name_alloc(&s, 5001); memset(names[5], 'P', 5000);
    for (size_t i = 0; i < n; i++) if (sorted[i]) names[i] = name_text(&s, sorted[i]);
    array = name_alloc(&s, n * sizeof(uintptr_t));
    for (size_t i = 0; i < n; i++) array[i] = name_record(&s, (uintptr_t)names[i]);
    g_types_slot = (uintptr_t)array; g_count = (int)n; g_ready = 1; g_first = 10; g_last = 20;
    for (int unbounded = 0; unbounded < 2; unbounded++) {
        const sh_native_class_source *source = unbounded ? process : &strict;
        for (size_t i = 0; i < n; i++)
            assert(sh_native_class_lookup(source, names[i], &found) == 1 && found.type_num == 11);
        for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
            assert(sh_native_class_lookup(source, absent[i], &found) == 0);
    }
    if (s.past_end || s.faults)
        fprintf(stderr, "name reads: %zu request(s) past an allocation's end, %zu fault(s)\n", s.past_end, s.faults);
    assert(!s.past_end && !s.faults);

    /* One record at a time at the page boundary: a name whose terminator
     * ends the readable page, then name bytes that run into the inaccessible
     * page. Only a comparison that needs the inaccessible byte is unavailable. */
    GetSystemInfo(&system);
    page = VirtualAlloc(NULL, 2 * system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(page && VirtualProtect(page + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &previous));
    memcpy(page + system.dwPageSize - 8, "PageEnd", 8);
    array[0] = name_record(&s, (uintptr_t)(page + system.dwPageSize - 8)); g_count = 1;
    assert(sh_native_class_lookup(&strict, "PageEnd", &found) == 1 && !s.faults);
    assert(sh_native_class_lookup(&strict, "PageEndX", &found) == 0 && !s.faults);
    assert(sh_native_class_lookup(process, "PageEnd", &found) == 1);
    memcpy(page + system.dwPageSize - 12, "Unterminated", 12);
    array[0] = name_record(&s, (uintptr_t)(page + system.dwPageSize - 12));
    assert(sh_native_class_lookup(&strict, "Ux", &found) == 0 && !s.faults);
    assert(sh_native_class_lookup(&strict, "Unterminated", &found) == -1 && s.faults == 1);
    assert(sh_native_class_lookup(process, "Unterminated", &found) == -1);
    array[0] = name_record(&s, (uintptr_t)reserved);
    assert(sh_native_class_lookup(&strict, "A", &found) == -1 && s.faults == 2);
    /* A name at the very top of the address space: its bytes are compared and
     * the comparison stops before forming an address past the top. */
    s.top = "abcd"; s.top_length = 4;
    array[0] = name_record(&s, UINTPTR_MAX - 3);
    assert(sh_native_class_lookup(&strict, "abce", &found) == 0);
    assert(sh_native_class_lookup(&strict, "abcd", &found) == -1 && !s.wrapped && !s.past_end);
    VirtualFree(page, 0, MEM_RELEASE);
    for (size_t i = 0; i < s.count; i++) free(s.owned[i]);
}

/* A byte-backed event registry: accessors are decoded rather than executed.
 * Every candidate source is borrowed and may fail or change during a read. */
typedef struct event_fixture {
    unsigned char bytes[0x1000];
    int reads, short_read, fail, change, change_format, alias_reads;
} event_fixture;
static void event_ptr(event_fixture *f, size_t at, uintptr_t value) { memcpy(f->bytes + at, &value, sizeof(value)); }
static void event_int(event_fixture *f, size_t at, int value) { memcpy(f->bytes + at, &value, sizeof(value)); }
static int event_read(void *context, uintptr_t address, void *out, size_t length)
{
    event_fixture *f = context;
    uintptr_t base = (uintptr_t)f->bytes;
    if (f->fail || address < base || address - base >= sizeof(f->bytes) || length > sizeof(f->bytes) - (address - base)) return 0;
    memcpy(out, (const void *)address, length);
    f->reads++;
    if (f->change && address == base + 0xe20 && length == sizeof(uintptr_t)) event_int(f, 0xe20 + 0x3c, 1);
    if (f->change_format && address == base + 0xc80 + 0x50 && ++f->alias_reads == 2) f->bytes[0xd00 + 1] = 's';
    return f->short_read ? 2 : 1;
}
static sh_native_event_source event_reset(event_fixture *f)
{
    static const unsigned char getter[] = {0x48,0x8d,0x81,0xa0,1,0,0,0xc3};
    static const unsigned char index[] = {0x48,0x83,0xec,0x28,0x81,0xfa,0xff,0x0f,0,0,0x77,0x13,0x48,0x63,0xc2,
        0x48,0x8d,0x0d,0,0,0,0,0x48,0x8b,0x04,0xc1,0x48,0x83,0xc4,0x28,0xc3};
    static const unsigned char count[] = {0x8b,5,0,0,0,0,0xc3};
    int32_t relative;
    memset(f, 0, sizeof(*f));
    event_ptr(f, 0, (uintptr_t)f->bytes + 0x200);
    event_ptr(f, 0x200 + 0x90, (uintptr_t)f->bytes + 0x400);
    event_ptr(f, 0x1a0, (uintptr_t)f->bytes + 0x300);
    event_ptr(f, 0x300 + 8, (uintptr_t)f->bytes + 0x420);
    event_ptr(f, 0x300 + 0x28, (uintptr_t)f->bytes + 0x460);
    memcpy(f->bytes + 0x400, getter, sizeof(getter)); memcpy(f->bytes + 0x420, index, sizeof(index));
    memcpy(f->bytes + 0x460, count, sizeof(count));
    relative = 0x500 - (0x420 + 22); memcpy(f->bytes + 0x420 + 18, &relative, 4);
    relative = 0xe00 - (0x460 + 6); memcpy(f->bytes + 0x460 + 2, &relative, 4);
    event_int(f, 0xe00, 162);
    event_ptr(f, 0xd80, (uintptr_t)f->bytes + 0xf40); strcpy((char *)f->bytes + 0xf40, "unrelated");
    for (int i = 0; i < 159; i++) event_ptr(f, 0x500 + i * 8, (uintptr_t)f->bytes + 0xd80);
    event_ptr(f, 0x500 + 158 * 8, (uintptr_t)f->bytes + 0xc80);
    event_ptr(f, 0xc80, (uintptr_t)f->bytes + 0xcf0); strcpy((char *)f->bytes + 0xcf0, "dropEntity");
    event_ptr(f, 0xc80 + 8, (uintptr_t)f->bytes + 0xd00); strcpy((char *)f->bytes + 0xd00, "devibi");
    event_ptr(f, 0xc80 + 0x10, (uintptr_t)f->bytes + 0xd20);
    strcpy((char *)f->bytes + 0xd20, "idDeclEntityDef*;idEntity*;idVec3&;int;bool;gameTeam_t;");
    event_int(f, 0xc80 + 0x2c, 6); event_int(f, 0xc80 + 0x34, 158);
    for (int i = 0; i < 2; i++) {
        size_t row = i ? 0xe80 : 0xe20, text = i ? 0xf20 : 0xf00;
        event_ptr(f, 0x500 + (159 + i) * 8, (uintptr_t)f->bytes + row);
        event_ptr(f, row, (uintptr_t)f->bytes + text);
        strcpy((char *)f->bytes + text, i ? "getName" : "findEntity");
        event_int(f, row + 0x28, i ? 's' : 'e'); event_int(f, row + 0x34, 159 + i);
        event_int(f, row + 0x2c, i ? 0 : 1);
    }
    return (sh_native_event_source){f, event_read, (uintptr_t)f->bytes, 1};
}
static void test_event_registry(void)
{
    event_fixture f;
    sh_native_event_source s = event_reset(&f);
    assert(sh_native_event_discarded_name(&s, "findEntity") && sh_native_event_discarded_name(&s, "getName"));
    assert(!sh_native_event_discarded_name(&s, "GETNAME") && !sh_native_event_discarded_name(&s, "FindEntity"));
    assert(!sh_native_event_discarded_name(&s, "setName") && !sh_native_event_discarded_name(&s, "findEntityExtra"));
    s.dispatch_verified = 0; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.short_read = 1; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.fail = 1; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_int(&f, 0xe00, 159); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_int(&f, 0xe00, 4097); assert(!sh_native_event_discarded_name(&s, "getName"));
    s = event_reset(&f); event_int(&f, 0xe20 + 0x34, 160); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_int(&f, 0xe20 + 0x3c, 3); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_int(&f, 0xe20 + 0x28, 's'); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_ptr(&f, 0xe20 + 0x50, 1); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_ptr(&f, 0xe80 + 0x50, 1); assert(!sh_native_event_discarded_name(&s, "getName"));
    s = event_reset(&f); event_ptr(&f, 0xd80, (uintptr_t)f.bytes + 0xf00);
    assert(!sh_native_event_discarded_name(&s, "findEntity")); /* earlier override wins */
    s = event_reset(&f); event_ptr(&f, 0x500, 0); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.bytes[0xf00] = 'F'; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.bytes[0xf0a] = 'x'; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.bytes[0x400] = 0xc3; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.bytes[0x420 + 5] = 0xfb; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.bytes[0x460] = 0xc3; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_ptr(&f, 0x500 + 159 * 8, 0); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); event_ptr(&f, 0xe20, UINTPTR_MAX - 1); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f); f.change = 1; assert(!sh_native_event_discarded_name(&s, "findEntity"));
    assert(!sh_native_event_discarded_name(NULL, "findEntity"));
    /* A different case is a distinct native registry key, not proof about the
     * canonical handler. A preceding exact name still overrides the record. */
    s = event_reset(&f); strcpy((char *)f.bytes + 0xf40, "GETNAME");
    assert(sh_native_event_discarded_name(&s, "getName") && !sh_native_event_discarded_name(&s, "GETNAME"));
    s = event_reset(&f); event_int(&f, 0xe20 + 0x2c, 2); assert(!sh_native_event_discarded_name(&s, "findEntity"));
    s = event_reset(&f);
#define CONSTRUCT(name,index,count,sig,klass) sh_native_event_decl_constructs(&s,name,index,count,sig,klass)
    assert(CONSTRUCT("dropEntity",0,6,"devibi","idDeclEntityDef"));
    assert(CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    assert(CONSTRUCT("dropEntity",0,6,"d????i","idDeclEntityDef"));
    assert(CONSTRUCT("dropEntity",0,2,"d????i","idDeclEntityDef"));
    assert(CONSTRUCT("dropEntity",0,6,"d00000","idDeclEntityDef"));
    assert(CONSTRUCT("dropEntity",0,8,"d","idDeclEntityDef")); /* native stops at closing brace */
    assert(!CONSTRUCT("DropEntity",0,6,"devibi","idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",1,6,"devibi","idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",0,0,"d","idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",0,1,"de","idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",0,6,"dsvibi","idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",0,6,"0evibi","idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",0,6,NULL,"idDeclEntityDef"));
    assert(!CONSTRUCT("dropEntity",0,6,"devibi","idDeclLootDrop"));
    assert(!CONSTRUCT("dropEntity",0,7,"devibii","idDeclEntityDef"));
    s.dispatch_verified = 0; assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    s = event_reset(&f); event_int(&f, 0xc80 + 0x2c, 5); assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    s = event_reset(&f); f.bytes[0xd00 + 1] = 's'; assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    s = event_reset(&f); f.bytes[0xd20 + 2] = 'x'; assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    s = event_reset(&f); event_ptr(&f, 0xc80 + 0x50, 1); assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    s = event_reset(&f); event_ptr(&f, 0xd80, (uintptr_t)f.bytes + 0xcf0);
    assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
    s = event_reset(&f); f.change_format = 1; assert(!CONSTRUCT("dropEntity",0,1,"d","idDeclEntityDef"));
#undef CONSTRUCT
}

int main(void)
{
    const sh_native_class_source *process = sh_native_class_process();
    sh_native_class found;
    unsigned char *reserved = VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
    assert(reserved);
    reset();

    /* Unbound: unavailable, never absent. */
    assert(sh_native_class_lookup(process, "Entity", &found) == -1);
    assert(verdict(process, "Missing") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    sh_native_class_bind_process((uintptr_t)&g_ready, (uintptr_t)&g_types_slot, (uintptr_t)&g_count,
        (uintptr_t)&g_first, (uintptr_t)&g_last);
    process = sh_native_class_process();
    assert(process->ready == (uintptr_t)&g_ready);
    /* A second bind is ignored. */
    sh_native_class_bind_process(1, 2, 3, 4, 5);
    assert(sh_native_class_process()->ready == (uintptr_t)&g_ready);

    /* Before Init (readiness clear): unavailable, even for a listed name. */
    g_ready = 0;
    assert(sh_native_class_lookup(process, "Entity", &found) == -1 && !found.factory);
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    assert(verdict(process, "Missing") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    g_ready = 1;

    /* The constructor's order: absent, unmanaged, then the factory. */
    assert(sh_native_class_lookup(process, "Entity", &found) == 1);
    assert(found.managed && !found.abstract_class && found.type_num == 11 && found.last_child == 14 &&
        found.factory == (uintptr_t)fake_factory);
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_ELIGIBLE);
    assert(verdict(process, "Abstract") == SH_NATIVE_CONSTRUCTION_ABSTRACT);
    assert(verdict(process, "Unmanaged") == SH_NATIVE_CONSTRUCTION_UNMANAGED);
    assert(verdict(process, "\xC3xotic") == SH_NATIVE_CONSTRUCTION_ELIGIBLE);
    assert(verdict(process, "entity") == SH_NATIVE_CONSTRUCTION_ELIGIBLE);
    assert(verdict(process, "ENTITY") == SH_NATIVE_CONSTRUCTION_ABSENT);
    assert(verdict(process, "Entit") == SH_NATIVE_CONSTRUCTION_ABSENT);
    assert(verdict(process, "EntityX") == SH_NATIVE_CONSTRUCTION_ABSENT);
    assert(sh_native_class_lookup(process, "Missing", &found) == 0);
    assert(verdict(process, "") == SH_NATIVE_CONSTRUCTION_NO_CLASS);
    assert(verdict(NULL, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    assert(verdict(process, NULL) == SH_NATIVE_CONSTRUCTION_NO_CLASS);
    /* A managed range bound at either end. */
    g_first = 30; g_last = 30;
    assert(verdict(process, "Unmanaged") == SH_NATIVE_CONSTRUCTION_ELIGIBLE);
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNMANAGED);
    g_first = 10; g_last = 20;
    /* A missing factory is not the record the constructor reads. */
    record(2, "Entity", 0, 0, 11, 14);
    assert(sh_native_class_lookup(process, "Entity", &found) == 1 && !found.factory);
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    reset();
    /* Inconsistent numbering is no answer. */
    record(2, "Entity", 1, 0, 15, 14);
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    reset();
    g_first = 21;
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    assert(verdict(process, "Missing") == SH_NATIVE_CONSTRUCTION_ABSENT);
    g_first = 10;

    /* Unreadable state is unavailable, never a class miss. */
    g_sorted[2] = (uintptr_t)reserved;
    assert(sh_native_class_lookup(process, "Missing", &found) == -1);
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    reset();
    {
        uintptr_t bad = (uintptr_t)reserved;
        memcpy(g_records[1], &bad, sizeof(bad));
        assert(verdict(process, "Abstract") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
        reset();
        g_sorted[1] = 0;
        assert(verdict(process, "Abstract") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
        reset();
    }
    g_count = -1;
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    g_count = RECORDS; g_types_slot = 0;
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    /* Shutdown frees the arrays and clears readiness. */
    g_ready = 0; g_count = 0;
    assert(verdict(process, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    /* An initialized empty registry is conclusive. */
    g_ready = 1;
    assert(sh_native_class_lookup(process, "Entity", &found) == 0);
    reset();
    /* Unresolved globals stay unavailable. */
    {
        sh_native_class_source partial = *process;
        partial.managed_last = 0;
        assert(verdict(&partial, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
        partial = *process; partial.count = 0;
        assert(verdict(&partial, "Missing") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
    }

    /* A registry that changes while it is read gives no answer: readiness
     * cleared, count changed, or the array replaced, at every read between
     * the two state reads of that field (ready, types, count are reads 1-3
     * and the last three). A change before the first read or after the last
     * leaves one consistent snapshot. */
    {
        sh_native_class_source changing = *process;
        int total;
        changing.read = changing_read;
        g_reads = 0; g_flip_at = 0;
        assert(verdict(&changing, "Entity") == SH_NATIVE_CONSTRUCTION_ELIGIBLE);
        total = g_reads;
        assert(total > 4);
        for (int kind = 0; kind < 3; kind++)
            for (int at = 1; at <= total; at++) {
                static const int field[3] = {0, 2, 1}; /* ready, count, types: index among the three */
                int first = 1 + field[kind], last = total - 2 + field[kind];
                sh_native_construction got, want = at >= first && at < last ?
                    SH_NATIVE_CONSTRUCTION_UNAVAILABLE : SH_NATIVE_CONSTRUCTION_ELIGIBLE;
                reset(); g_reads = 0; g_flip_at = at; g_flip_kind = kind;
                got = verdict(&changing, "Entity");
                if (got != want)
                    fprintf(stderr, "changed state kind %d at read %d of %d: %d\n", kind, at, total, (int)got);
                assert(got == want);
            }
        reset(); g_flip_at = 0; g_short_read = 1;
        assert(verdict(&changing, "Entity") == SH_NATIVE_CONSTRUCTION_UNAVAILABLE);
        g_short_read = 0;
    }

    /* M94 pool settings: the three cvars found by name in the live cvar
     * registry (rows: +0x30 integer value, +0x40 name), all or nothing; an
     * unbound slot, a missing or unreadable row, or a source without the
     * reader leaves them unavailable and the output untouched. */
    {
        static const char *names[3] = {"g_entityPoolTypeInfoClone", "g_entityPoolMode", "ai_pool_enable"};
        static unsigned char rows[4][0x48];
        static const unsigned char *row_list[4];
        static unsigned char system[0x18];
        static const unsigned char *system_slot;
        const int values[3] = {1, 2, 0};
        sh_native_pool_settings s = {7, 7, 7};
        sh_native_class_source without = *process;
        uintptr_t list = (uintptr_t)row_list;
        unsigned count = 4;
        assert(!sh_native_pool_settings_read(process, &s) && s.type_info_clone == 7 && s.pool_mode == 7);
        for (int i = 0; i < 3; i++) {
            uintptr_t name = (uintptr_t)names[i];
            memcpy(rows[i] + 0x30, &values[i], 4); memcpy(rows[i] + 0x40, &name, sizeof(name));
            row_list[i] = rows[i];
        }
        {
            uintptr_t other = (uintptr_t)"g_entityPoolModeX";
            memcpy(rows[3] + 0x40, &other, sizeof(other)); row_list[3] = rows[3];
        }
        memcpy(system + 8, &list, sizeof(list)); memcpy(system + 0x10, &count, 4);
        system_slot = system;
        sh_native_pool_bind_process((uintptr_t)&system_slot);
        assert(sh_native_pool_settings_read(process, &s));
        assert(s.type_info_clone == 1 && s.pool_mode == 2 && s.ai_pool_enable == 0);
        /* Later binds keep the first slot. */
        sh_native_pool_bind_process(1);
        s.pool_mode = 9; assert(sh_native_pool_settings_read(process, &s) && s.pool_mode == 2);
        /* A row the engine registers under another name never matches by prefix. */
        row_list[1] = rows[3]; s.pool_mode = 9;
        assert(!sh_native_pool_settings_read(process, &s) && s.pool_mode == 9);
        /* An unreadable row is unavailable, never a value. */
        row_list[1] = reserved; assert(!sh_native_pool_settings_read(process, &s) && s.pool_mode == 9);
        row_list[1] = rows[1];
        assert(sh_native_pool_settings_read(process, &s) && s.pool_mode == 2);
        without.pool_settings = NULL; s.pool_mode = 9;
        assert(!sh_native_pool_settings_read(&without, &s) && s.pool_mode == 9);
        assert(!sh_native_pool_settings_read(NULL, &s));
    }

    test_name_reads(process, reserved);
    test_event_registry();
    reset();

    /* Metadata only: no factory ever ran. */
    assert(!g_factory_ran);
    VirtualFree(reserved, 0, MEM_RELEASE);
    printf("native_class_registry_test: PASS\n");
    return 0;
}
