/* Reflection for mod scripts: a path such as "aiEditable.staggerEnabled" or
 * "encounterMods.healthMod" or "inventory[2].ammo" is walked through the
 * game's own type tables (every class, field, offset and enum the engine
 * reflects), so a script reaches any reflected field of any object by name
 * and no field needs an offset written into Snapmap+.
 *
 * This file is the walk, pure: the host supplies the types (the engine's, or
 * a test's) and reads memory. A path is names separated by dots, with [n]
 * after a list or array. A pointer or an entity reference met inside a path
 * is followed.
 *
 * Kinds of value a path can end in:
 *   BOOL INT UINT FLOAT DOUBLE   numbers (size bytes at addr)
 *   ENUM      a named enum (int at addr); `type` is the enum
 *   STR       an idStr at addr
 *   STRUCT    a reflected class at addr; `type` is the class
 *   POINTER   a pointer at addr to `type` (a decl, an entity, another class)
 *   REF       an idManagedClassPtr < T > at addr (an entity by spawn id)
 *   LIST      an idList / idStaticList header at addr of `type` elements
 *   ARRAY     `count` elements of `type` in place (T x[N], idArray < T , N >)
 *   OPAQUE    a type the engine does not reflect (bytes only) */
#ifndef SH_SCRIPT_REFLECT_H
#define SH_SCRIPT_REFLECT_H

#include <stddef.h>
#include <stdint.h>

typedef struct sh_rf_field {
    const char *type;      /* "float", "idVec3", "idList < idStr , TAG_IDLIST , false >", "gameTeam_t" */
    const char *ops;       /* "", "*", "[4]" */
    const char *name;
    uint32_t offset;       /* from the start of the object */
    uint32_t size;
} sh_rf_field;

typedef struct sh_rf_host {
    void *ctx;
    /* 1 with the size when `type` is a reflected class. */
    int (*class_size)(void *ctx, const char *type, uint32_t *size);
    /* The class's superclass, NULL or "" at a root. */
    const char *(*class_super)(void *ctx, const char *type);
    /* The index-th field the class itself declares; 0 past the last. */
    int (*class_field)(void *ctx, const char *type, uint32_t index, sh_rf_field *out);
    /* 1 when `type` is an enum; with `member`, 1 and its value when it has that member. */
    int (*enum_value)(void *ctx, const char *type, const char *member, long long *value);
    /* The member of `type` with that value, NULL when none. */
    const char *(*enum_name)(void *ctx, const char *type, long long value);
    /* Copy n bytes at addr; 0 when unreadable. */
    int (*read)(void *ctx, const void *addr, void *out, size_t n);
    /* The object a reference at addr names now and its class (an entity's own
     * class, not the reference's declared one); 0 when it names none. */
    int (*follow_ref)(void *ctx, const void *addr, const char *declared, const void **object, const char **cls);
    /* The class of the object a pointer of declared type `declared` points
     * at (an entity's or decl's own class); NULL keeps the declared one. */
    const char *(*object_class)(void *ctx, const void *object, const char *declared);
} sh_rf_host;

enum {
    SH_RF_NONE, SH_RF_BOOL, SH_RF_INT, SH_RF_UINT, SH_RF_FLOAT, SH_RF_DOUBLE, SH_RF_ENUM, SH_RF_STR,
    SH_RF_STRUCT, SH_RF_POINTER, SH_RF_REF, SH_RF_LIST, SH_RF_ARRAY, SH_RF_OPAQUE
};

/* Where a path ended. `type` and `elem_ops` are owned text (sh_rf_loc_free). */
typedef struct sh_rf_loc {
    int kind;
    const uint8_t *addr;
    uint32_t size;         /* bytes of the value; one element's for LIST and ARRAY (0 when unknown) */
    uint32_t count;        /* ARRAY: its elements */
    char *type;            /* the class, enum, pointee or element type */
    char *elem_ops;        /* LIST and ARRAY: the element's "*" when the elements are pointers */
} sh_rf_loc;

void sh_rf_loc_free(sh_rf_loc *loc);

/* What a field of type `type` with qualifier `ops` and size `size` is, at addr. 1, or 0 when memory ran out. */
int sh_rf_classify(const sh_rf_host *h, const char *type, const char *ops, uint32_t size, const uint8_t *addr,
                   sh_rf_loc *out);

/* The field `name` of class `cls` (its own or an ancestor's): 1 with it. */
int sh_rf_find_field(const sh_rf_host *h, const char *cls, const char *name, sh_rf_field *out);

/* Walk `path` from the object at `object` of class `cls`. 1 with the place
 * it ends; 0 with a sentence in err that names the part that failed. An
 * empty path ends at the object itself (STRUCT). */
int sh_rf_resolve(const sh_rf_host *h, const void *object, const char *cls, const char *path, sh_rf_loc *out,
                  char *err, size_t err_cap);

/* The element `index` of a LIST or ARRAY: 1 with its place, 0 with err. */
int sh_rf_element(const sh_rf_host *h, const sh_rf_loc *list, uint32_t index, sh_rf_loc *out, char *err, size_t err_cap);

/* How many elements a LIST or ARRAY holds now; -1 when unreadable. */
long long sh_rf_count(const sh_rf_host *h, const sh_rf_loc *list);

/* idList / idStaticList header and idManagedClassPtr layout (both renderers). */
#define SH_RF_LIST_PTR     0x00u
#define SH_RF_LIST_NUM     0x08u
#define SH_RF_REF_SPAWN_ID 0x00u

#endif
