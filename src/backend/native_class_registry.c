/* native_class_registry.c -- see native_class_registry.h. */
#include <windows.h>
#include <string.h>

#include "native_class_registry.h"
#include "engine_cvar_read.h"

/* idTypeInfo record (R-221): +0 classname, +0x28 CreateInstance, +0x60
 * abstract, +0x68 typeNum, +0x6c lastChild. The sorted array is an idList of
 * record pointers: data at the bound slot, num at the count slot. */
enum { NC_NAME = 0x0, NC_FACTORY = 0x28, NC_ABSTRACT = 0x60, NC_TYPE_NUM = 0x68, NC_LAST_CHILD = 0x6c };

static int nc_read(const sh_native_class_source *source, uintptr_t address, void *out, size_t length)
{
    return address && source->read(source->context, address, out, length) == 1;
}

/* idStr::Cmp (VK 0x1419feb20 / GL 0x1419f0d80) of a record name against the
 * key: signed byte difference, stopping at the first difference or at the
 * record name's terminator. The name is read one byte at a time, so no byte
 * past either string is requested (a key byte is read only while every
 * earlier byte matched a nonzero name byte), and each address is checked
 * before it is formed. 0 when a needed name byte cannot be read. */
static int nc_compare(const sh_native_class_source *source, uintptr_t name, const char *key, int *order)
{
    for (size_t at = 0;; at++) {
        unsigned char c;
        int difference;
        if (at > UINTPTR_MAX - name || !nc_read(source, name + at, &c, 1)) return 0;
        difference = (int)(signed char)c - (int)(signed char)key[at];
        if (difference) { *order = difference > 0 ? 1 : -1; return 1; }
        if (!c) { *order = 0; return 1; }
    }
}

typedef struct nc_state { unsigned char ready; uintptr_t types; int count; } nc_state;
static int nc_state_read(const sh_native_class_source *source, nc_state *state)
{
    memset(state, 0, sizeof(*state));
    return nc_read(source, source->ready, &state->ready, sizeof(state->ready)) &&
        nc_read(source, source->types, &state->types, sizeof(state->types)) &&
        nc_read(source, source->count, &state->count, sizeof(state->count));
}

int sh_native_class_lookup(const sh_native_class_source *source, const char *name, sh_native_class *out)
{
    nc_state before, after;
    uintptr_t record = 0;
    sh_native_class found = {0};
    int first = 0, last = 0;
    long long low = 0, high;
    if (out) memset(out, 0, sizeof(*out));
    if (!source || !source->read || !name || !source->managed_first || !source->managed_last) return -1;
    if (!nc_state_read(source, &before) || !before.ready || before.count < 0 || (before.count && !before.types)) return -1;
    high = (long long)before.count - 1;
    while (low <= high) {
        long long middle = (low + high) / 2;
        uintptr_t candidate = 0, candidate_name = 0;
        int order = 0;
        if (!nc_read(source, before.types + (uintptr_t)middle * sizeof(uintptr_t), &candidate, sizeof(candidate)) ||
            !candidate || !nc_read(source, candidate + NC_NAME, &candidate_name, sizeof(candidate_name)) ||
            !candidate_name || !nc_compare(source, candidate_name, name, &order)) return -1;
        if (!order) { record = candidate; break; }
        if (order < 0) low = middle + 1; else high = middle - 1;
    }
    if (record) {
        unsigned char abstract_class = 0;
        if (!nc_read(source, record + NC_FACTORY, &found.factory, sizeof(found.factory)) ||
            !nc_read(source, record + NC_ABSTRACT, &abstract_class, sizeof(abstract_class)) ||
            !nc_read(source, record + NC_TYPE_NUM, &found.type_num, sizeof(found.type_num)) ||
            !nc_read(source, record + NC_LAST_CHILD, &found.last_child, sizeof(found.last_child)) ||
            !nc_read(source, source->managed_first, &first, sizeof(first)) ||
            !nc_read(source, source->managed_last, &last, sizeof(last))) return -1;
        /* Init numbers types depth-first, so a record spans its descendants. */
        if (first > last || found.type_num > found.last_child) return -1;
        found.abstract_class = abstract_class != 0;
        found.managed = found.type_num >= first && found.type_num <= last;
    }
    /* Shutdown frees the arrays and clears readiness; a change while reading
     * is no answer. */
    if (!nc_state_read(source, &after) || !after.ready || after.types != before.types || after.count != before.count)
        return -1;
    if (out) *out = found;
    return record != 0;
}

sh_native_construction sh_native_class_construction(const sh_native_class_source *source,
    const char *name, sh_native_class *out)
{
    sh_native_class found;
    int present;
    if (out) memset(out, 0, sizeof(*out));
    if (!name || !*name) return SH_NATIVE_CONSTRUCTION_NO_CLASS;
    present = sh_native_class_lookup(source, name, &found);
    if (present < 0) return SH_NATIVE_CONSTRUCTION_UNAVAILABLE;
    if (out) *out = found;
    if (!present) return SH_NATIVE_CONSTRUCTION_ABSENT;
    if (!found.managed) return SH_NATIVE_CONSTRUCTION_UNMANAGED;
    /* A registered record always carries a factory; without one this is not
     * the record the constructor reads. */
    if (!found.factory) return SH_NATIVE_CONSTRUCTION_UNAVAILABLE;
    return found.abstract_class ? SH_NATIVE_CONSTRUCTION_ABSTRACT : SH_NATIVE_CONSTRUCTION_ELIGIBLE;
}

const char *sh_native_construction_text(sh_native_construction construction)
{
    switch (construction) {
    case SH_NATIVE_CONSTRUCTION_ELIGIBLE: return "is a concrete idManagedClass descendant";
    case SH_NATIVE_CONSTRUCTION_NO_CLASS: return "has no class name";
    case SH_NATIVE_CONSTRUCTION_ABSENT: return "is not in the engine's idClass registry";
    case SH_NATIVE_CONSTRUCTION_UNMANAGED: return "is registered but not derived from idManagedClass";
    case SH_NATIVE_CONSTRUCTION_ABSTRACT: return "is abstract, so its registered factory returns NULL";
    default: return "cannot be checked because the engine's idClass registry is unavailable";
    }
}

static int nc_process_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    if (!address || length > UINTPTR_MAX - address) return 0;
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static volatile uintptr_t g_cvar_slot;
static int nc_process_pool_settings(void *context, sh_native_pool_settings *out)
{
    static const char *const names[3] = {"g_entityPoolTypeInfoClone", "g_entityPoolMode", "ai_pool_enable"};
    int values[3];
    (void)context;
    for (int i = 0; i < 3; i++)
        if (!sh_engine_cvar_read_int(sh_engine_cvar_find((const void *)g_cvar_slot, names[i]), &values[i])) return 0;
    out->type_info_clone = values[0]; out->pool_mode = values[1]; out->ai_pool_enable = values[2];
    return 1;
}

static sh_native_class_source g_process = {NULL, nc_process_read, 0, 0, 0, 0, 0, nc_process_pool_settings};
static const sh_native_class_source g_unbound = {NULL, nc_process_read, 0, 0, 0, 0, 0, NULL};
static volatile LONG g_process_bound;

void sh_native_class_bind_process(uintptr_t ready, uintptr_t types, uintptr_t count,
    uintptr_t managed_first, uintptr_t managed_last)
{
    /* Installation binds once before any walk can read the source. Unresolved
     * globals stay zero, so every lookup reports the registry unavailable. */
    if (InterlockedCompareExchange(&g_process_bound, 1, 0)) return;
    g_process.ready = ready; g_process.types = types; g_process.count = count;
    g_process.managed_first = managed_first; g_process.managed_last = managed_last;
    MemoryBarrier();
    InterlockedExchange(&g_process_bound, 2);
}

const sh_native_class_source *sh_native_class_process(void)
{
    /* Before binding every address is zero: the registry is unavailable,
     * never absent. */
    return g_process_bound == 2 ? &g_process : &g_unbound;
}

void sh_native_pool_bind_process(uintptr_t cvar_system_slot)
{
    if (!g_cvar_slot) InterlockedExchangePointer((PVOID volatile *)&g_cvar_slot, (PVOID)cvar_system_slot);
}

int sh_native_pool_settings_read(const sh_native_class_source *source, sh_native_pool_settings *out)
{
    sh_native_pool_settings read = {0};
    if (!source || !source->pool_settings || !out || !source->pool_settings(source->context, &read)) return 0;
    *out = read;
    return 1;
}

/* Event registry methods are read-only fixed-shape accessors on both proved
 * images. Decode their RIP-relative globals; do not call a vtable from an
 * unverified prospective source. The index method's 0x1000 bound is native. */
static int ne_read(const sh_native_event_source *s, uintptr_t address, void *out, size_t length)
{
    return s && s->read && address && length <= UINTPTR_MAX - address &&
        s->read(s->context, address, out, length) == 1;
}
static uintptr_t ne_relative(uintptr_t end, int32_t displacement)
{
    if (displacement < 0) return end >= (uintptr_t)(-(int64_t)displacement) ? end - (uintptr_t)(-(int64_t)displacement) : 0;
    return (uintptr_t)displacement <= UINTPTR_MAX - end ? end + (uintptr_t)displacement : 0;
}
/* Native event-table lookup selects the first exact name. A preceding record
 * with that name changes the selection even if the expected numbered row is
 * still present. -1 means a byte required by the comparison was unreadable. */
static int ne_name(const sh_native_event_source *s, uintptr_t address, const char *name)
{
    for (size_t i = 0;; i++) {
        unsigned char held = 0;
        if (address > UINTPTR_MAX - i || !ne_read(s, address + i, &held, 1)) return -1;
        if (held != (unsigned char)name[i]) return 0;
        if (!held) return 1;
    }
}
static int ne_record(const sh_native_event_source *s, const char *canonical, int expected, int returns, int arity,
    const char *format, const char *types)
{
    static const unsigned char index_prefix[] = {0x48,0x83,0xec,0x28,0x81,0xfa,0xff,0x0f,0,0,0x77,0x13,0x48,0x63,0xc2,0x48,0x8d,0x0d};
    static const unsigned char index_suffix[] = {0x48,0x8b,0x04,0xc1,0x48,0x83,0xc4,0x28,0xc3};
    uintptr_t manager_table = 0, accessor = 0, event_manager, event_table = 0, index = 0, count_method = 0;
    uintptr_t array, count_slot, record = 0, name_at = 0, alias = 0, again_record = 0, format_at = 0, types_at = 0;
    unsigned char getter[8], index_code[31], count_code[7];
    int32_t member_offset, displacement;
    int count = 0, again_count = 0, number = -1, kind = -1, result_type = -1, arguments = -1;
    if (!s || s->dispatch_verified != 1) return 0;
    if (!ne_read(s, s->decl_manager, &manager_table, sizeof(manager_table)) || manager_table > UINTPTR_MAX - 0x90 ||
        !ne_read(s, manager_table + 0x90, &accessor, sizeof(accessor)) ||
        !ne_read(s, accessor, getter, sizeof(getter)) || memcmp(getter, "\x48\x8d\x81", 3) || getter[7] != 0xc3) return 0;
    memcpy(&member_offset, getter + 3, 4);
    event_manager = ne_relative(s->decl_manager, member_offset);
    if (!ne_read(s, event_manager, &event_table, sizeof(event_table)) || event_table > UINTPTR_MAX - 0x28 ||
        !ne_read(s, event_table + 8, &index, sizeof(index)) || !ne_read(s, event_table + 0x28, &count_method, sizeof(count_method)) ||
        !ne_read(s, index, index_code, sizeof(index_code)) || memcmp(index_code, index_prefix, sizeof(index_prefix)) ||
        memcmp(index_code + 22, index_suffix, sizeof(index_suffix)) ||
        !ne_read(s, count_method, count_code, sizeof(count_code)) || count_code[0] != 0x8b ||
        count_code[1] != 5 || count_code[6] != 0xc3 || index > UINTPTR_MAX - 22 || count_method > UINTPTR_MAX - 6) return 0;
    memcpy(&displacement, index_code + 18, 4); array = ne_relative(index + 22, displacement);
    memcpy(&displacement, count_code + 2, 4); count_slot = ne_relative(count_method + 6, displacement);
    if (!array || array > UINTPTR_MAX - (uintptr_t)expected * 8 || !ne_read(s, count_slot, &count, sizeof(count)) ||
        count <= expected || count > 0x1000 || !ne_read(s, array + (uintptr_t)expected * 8, &record, sizeof(record)) ||
        !record || record > UINTPTR_MAX - 0x50 || !ne_read(s, record, &name_at, sizeof(name_at)) || !name_at ||
        !ne_read(s, record + 0x28, &result_type, sizeof(result_type)) || result_type != returns ||
        !ne_read(s, record + 0x2c, &arguments, sizeof(arguments)) || arguments != arity ||
        !ne_read(s, record + 0x34, &number, sizeof(number)) || number != expected ||
        !ne_read(s, record + 0x3c, &kind, sizeof(kind)) || kind ||
        !ne_read(s, record + 0x50, &alias, sizeof(alias)) || alias) return 0;
    if (ne_name(s, name_at, canonical) != 1) return 0;
    if (format && types) {
        if (!ne_read(s, record + 8, &format_at, sizeof(format_at)) || !format_at ||
            !ne_read(s, record + 0x10, &types_at, sizeof(types_at)) || !types_at ||
            ne_name(s, format_at, format) != 1 || ne_name(s, types_at, types) != 1) return 0;
    }
    for (int i = 0; i < expected; i++) {
        uintptr_t earlier = 0, earlier_name = 0;
        if (!ne_read(s, array + (uintptr_t)i * 8, &earlier, sizeof(earlier)) || !earlier ||
            !ne_read(s, earlier, &earlier_name, sizeof(earlier_name)) || !earlier_name ||
            ne_name(s, earlier_name, canonical) != 0) return 0;
    }
    /* The source is borrowed only at a stable engine boundary. A changed
     * table, selected row or metadata during the read is no proof. */
    {
        int after_number = -1, after_kind = -1, after_type = -1, after_arguments = -1;
        uintptr_t after_name = 0, after_alias = 0, after_format = 0, after_types = 0;
        return ne_read(s, count_slot, &again_count, sizeof(again_count)) && again_count == count &&
            ne_read(s, array + (uintptr_t)expected * 8, &again_record, sizeof(again_record)) && again_record == record &&
            ne_read(s, record, &after_name, sizeof(after_name)) && after_name == name_at &&
            ne_read(s, record + 0x28, &after_type, sizeof(after_type)) && after_type == result_type &&
            ne_read(s, record + 0x2c, &after_arguments, sizeof(after_arguments)) && after_arguments == arguments &&
            ne_read(s, record + 0x34, &after_number, sizeof(after_number)) && after_number == number &&
            ne_read(s, record + 0x3c, &after_kind, sizeof(after_kind)) && after_kind == kind &&
            ne_read(s, record + 0x50, &after_alias, sizeof(after_alias)) && after_alias == alias &&
            (!format || (ne_read(s, record + 8, &after_format, sizeof(after_format)) && after_format == format_at &&
                ne_read(s, record + 0x10, &after_types, sizeof(after_types)) && after_types == types_at &&
                ne_name(s, after_format, format) == 1 && ne_name(s, after_types, types) == 1));
    }
}
int sh_native_event_discarded_name(const sh_native_event_source *s, const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "findEntity")) return ne_record(s, "findEntity", 159, 'e', 1, NULL, NULL);
    if (!strcmp(name, "getName")) return ne_record(s, "getName", 160, 's', 0, NULL, NULL);
    return 0;
}
int sh_native_event_decl_constructs(const sh_native_event_source *s, const char *name, size_t index, size_t count,
    const char *signature, const char *klass)
{
    static const char format[] = "devibi";
    size_t length, supplied = 0;
    if (!name || strcmp(name, "dropEntity") || index || !count || !signature || !klass ||
        _stricmp(klass, "idDeclEntityDef")) return 0;
    length = strlen(signature);
    if (!length || length > sizeof(format) - 1 || signature[0] != 'd') return 0;
    for (size_t i = 0; i < length; i++) {
        if (signature[i] != format[i] && signature[i] != '0' && signature[i] != '?') return 0;
        if (signature[i] != '?') supplied++;
    }
    if (supplied > count) return 0;
    return ne_record(s, "dropEntity", 158, 0, 6, format, "idDeclEntityDef*;idEntity*;idVec3&;int;bool;gameTeam_t;");
}
