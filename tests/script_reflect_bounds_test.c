/* script_reflect_bounds_test.c - sh_rf_bounds (src/backend/script/script_reflect.h): an entity's collision box
 * found by name in a type table shaped like the game's. A demon's box comes from its own physics
 * (aiVolatile.physics.object, an actor whose clipModels field the game describes as one 16-byte "idClipModel"
 * though it holds two pointers), the first set pointer of the two; another entity's from its
 * defaultPhysicsObj.clipModel; an entity with neither, or with no clip model now, has none. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/script/script_reflect.h"

static int g_failures, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct vec3 { float x, y, z; } vec3;
typedef struct bounds { vec3 b[2]; } bounds;
typedef struct clipmodel { void *vt; char enabled; char pad[23]; bounds absBounds; bounds bounds; } clipmodel;
typedef struct physics_actor { void *vt; char pad[208]; clipmodel *clipModels[2]; int clipMasks[2]; } physics_actor;
typedef struct physics_ai { physics_actor actor; float maxStepHeight; } physics_ai;
typedef struct ai_physics { physics_ai object; float pad; } ai_physics;
typedef struct ai_volatile { ai_physics physics; int perception; } ai_volatile;
typedef struct physics_static { void *vt; char pad[56]; clipmodel *clipModel; } physics_static;
typedef struct entity { void *vt; int team; physics_static defaultPhysicsObj; } entity;
typedef struct demon { entity base; ai_volatile aiVolatile; } demon;

#define F(T, name, type, ops) { type, ops, #name, (uint32_t)offsetof(T, name), (uint32_t)sizeof(((T *)0)->name) }
static const sh_rf_field k_vec3[] = { F(vec3, x, "float", ""), F(vec3, y, "float", ""), F(vec3, z, "float", "") };
static const sh_rf_field k_bounds[] = { F(bounds, b, "idVec3", "[2]") };
static const sh_rf_field k_clip[] = { F(clipmodel, enabled, "bool", ""), F(clipmodel, absBounds, "idBounds", ""),
                                      F(clipmodel, bounds, "idBounds", "") };
/* as the game describes it: the element type only, 16 bytes */
static const sh_rf_field k_actor[] = { F(physics_actor, clipModels, "idClipModel", ""),
                                       F(physics_actor, clipMasks, "int", "[2]") };
static const sh_rf_field k_ai_phys[] = { F(physics_ai, maxStepHeight, "float", "") };
static const sh_rf_field k_aiphys_t[] = { F(ai_physics, object, "idPhysics_AI", "") };
static const sh_rf_field k_volatile[] = { F(ai_volatile, physics, "idAI2::idAIVolatile::physics_t", ""),
                                          F(ai_volatile, perception, "int", "") };
static const sh_rf_field k_static[] = { F(physics_static, clipModel, "idClipModel", "*") };
static const sh_rf_field k_entity[] = { F(entity, team, "int", ""), F(entity, defaultPhysicsObj, "idPhysics_Static", "") };
static const sh_rf_field k_demon[] = { F(demon, aiVolatile, "idAI2::idAIVolatile", "") };

typedef struct cls { const char *name, *super; const sh_rf_field *fields; uint32_t count, size; } cls;
#define C(name, super, arr, T) { name, super, arr, (uint32_t)(sizeof arr / sizeof arr[0]), (uint32_t)sizeof(T) }
static const cls k_classes[] = {
    C("idVec3", "", k_vec3, vec3), C("idBounds", "", k_bounds, bounds), C("idClipModel", "", k_clip, clipmodel),
    C("idPhysics_Actor", "", k_actor, physics_actor), C("idPhysics_AI", "idPhysics_Actor", k_ai_phys, physics_ai),
    C("idAI2::idAIVolatile::physics_t", "", k_aiphys_t, ai_physics), C("idAI2::idAIVolatile", "", k_volatile, ai_volatile),
    C("idPhysics_Static", "", k_static, physics_static), C("idEntity", "", k_entity, entity),
    C("idAI2", "idEntity", k_demon, demon),
};
static const cls *find_cls(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++) if (!strcmp(k_classes[i].name, name)) return &k_classes[i];
    return NULL;
}
static int h_size(void *c, const char *type, uint32_t *size)
{
    const cls *k = find_cls(type);
    (void)c;
    if (k && size) *size = k->size;
    return k != NULL;
}
static const char *h_super(void *c, const char *type) { const cls *k = find_cls(type); (void)c; return k ? k->super : NULL; }
static int h_field(void *c, const char *type, uint32_t index, sh_rf_field *out)
{
    const cls *k = find_cls(type);
    (void)c;
    if (!k || index >= k->count) return 0;
    *out = k->fields[index];
    return 1;
}
static int h_enum(void *c, const char *t, const char *m, long long *v) { (void)c; (void)t; (void)m; (void)v; return 0; }
static const char *h_enum_name(void *c, const char *t, long long v) { (void)c; (void)t; (void)v; return NULL; }
static int h_read(void *c, const void *addr, void *out, size_t n) { (void)c; if (!addr) return 0; memcpy(out, addr, n); return 1; }
static const sh_rf_host H = { NULL, h_size, h_super, h_field, h_enum, h_enum_name, h_read, NULL, NULL };

static void box(clipmodel *m, float x0, float y0, float z0, float x1, float y1, float z1)
{
    memset(m, 0, sizeof *m);
    m->enabled = 1;
    m->bounds.b[0].x = x0; m->bounds.b[0].y = y0; m->bounds.b[0].z = z0;
    m->bounds.b[1].x = x1; m->bounds.b[1].y = y1; m->bounds.b[1].z = z1;
    m->absBounds.b[0].x = 1000.0f;                 /* the world box is not the answer */
}

int main(void)
{
    static demon d;
    static entity crate;
    clipmodel body, second, rock;
    float mn[3], mx[3];
    char err[200];

    memset(&d, 0, sizeof d);
    memset(&crate, 0, sizeof crate);
    box(&body, -48, -48, 0, 48, 48, 180);
    box(&second, -10, -10, 0, 10, 10, 20);
    box(&rock, -16, -8, 0, 16, 8, 32);

    /* a demon: the first set pointer of its actor's pair, its local bounds */
    d.aiVolatile.physics.object.actor.clipModels[0] = &body;
    d.aiVolatile.physics.object.actor.clipModels[1] = &second;
    CHECK(sh_rf_bounds(&H, &d, "idAI2", mn, mx, err, sizeof err));
    CHECK(mn[0] == -48 && mn[1] == -48 && mn[2] == 0 && mx[0] == 48 && mx[1] == 48 && mx[2] == 180);

    /* the first pointer empty: the second */
    d.aiVolatile.physics.object.actor.clipModels[0] = NULL;
    CHECK(sh_rf_bounds(&H, &d, "idAI2", mn, mx, err, sizeof err));
    CHECK(mx[0] == 10 && mx[2] == 20);

    /* no clip model now: none, in a sentence */
    d.aiVolatile.physics.object.actor.clipModels[1] = NULL;
    d.base.defaultPhysicsObj.clipModel = NULL;
    CHECK(!sh_rf_bounds(&H, &d, "idAI2", mn, mx, err, sizeof err));
    CHECK(strstr(err, "no collision model now") != NULL);

    /* an entity that is not a demon: its default physics' one clip model */
    crate.defaultPhysicsObj.clipModel = &rock;
    CHECK(sh_rf_bounds(&H, &crate, "idEntity", mn, mx, err, sizeof err));
    CHECK(mn[0] == -16 && mn[1] == -8 && mx[0] == 16 && mx[2] == 32);

    /* a box that is not set (min over max): none */
    box(&rock, 5, 0, 0, -5, 1, 1);
    CHECK(!sh_rf_bounds(&H, &crate, "idEntity", mn, mx, err, sizeof err));

    /* a class with no physics the table describes */
    CHECK(!sh_rf_bounds(&H, &crate, "idVec3", mn, mx, err, sizeof err));
    CHECK(strstr(err, "no physics") != NULL);

    printf("script_reflect_bounds_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
