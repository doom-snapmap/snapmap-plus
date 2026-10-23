#include "decl_native_rebind.h"
#include "decl_md6_compose.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct nr_walk {
    sh_decl_source source;
    sh_resource_binding_site visitor;
    void *context;
} nr_walk;

static int nr_keyword(const sh_decl_tokens *tokens, size_t index, const char *value)
{
    const sh_decl_token *token;
    size_t length = strlen(value);
    if (index >= tokens->count) return 0;
    token = tokens->items + index;
    return token->kind == SH_DECL_TOKEN_NAME && token->end - token->begin == length &&
        !_strnicmp(tokens->source.text + token->begin, value, length);
}

static int nr_float(const sh_decl_tokens *tokens, size_t *at)
{
    if (sh_decl_token_is(tokens, *at, "-")) (*at)++;
    if (*at >= tokens->count || tokens->items[*at].kind != SH_DECL_TOKEN_NUMBER) return 0;
    (*at)++; return 1;
}

static int nr_take(const sh_decl_tokens *tokens, size_t *at, const char *text)
{
    if (!sh_decl_token_is(tokens, *at, text)) return 0;
    (*at)++; return 1;
}

static int nr_atom(const sh_decl_tokens *tokens, size_t *at, int quoted)
{
    sh_decl_token_kind kind;
    if (*at >= tokens->count) return 0;
    kind = tokens->items[*at].kind;
    if (kind != SH_DECL_TOKEN_STRING && (quoted ||
        (kind != SH_DECL_TOKEN_NAME && kind != SH_DECL_TOKEN_NUMBER))) return 0;
    (*at)++; return 1;
}

/* The lexer's boolean reader (Vulkan 0x141a14340): one number token, or one
 * name token spelled exactly true or false. */
static int nr_bool(const sh_decl_tokens *tokens, size_t *at)
{
    const sh_decl_token *token;
    size_t length;
    if (*at >= tokens->count) return 0;
    token = tokens->items + *at; length = token->end - token->begin;
    if (token->kind != SH_DECL_TOKEN_NUMBER && (token->kind != SH_DECL_TOKEN_NAME ||
        !((length == 4 && !memcmp(tokens->source.text + token->begin, "true", 4)) ||
          (length == 5 && !memcmp(tokens->source.text + token->begin, "false", 5))))) return 0;
    (*at)++; return 1;
}

/* AF geometry and joint identifiers are local to the figure. Only the settings
 * model/material and a custom shape's collision model perform resource lookup. */
static int nr_af_vector(const sh_decl_tokens *tokens, size_t *at)
{
    int joints;
    nr_take(tokens, at, "-");
    if (nr_take(tokens, at, "("))
        return nr_float(tokens, at) && nr_take(tokens, at, ",") && nr_float(tokens, at) &&
            nr_take(tokens, at, ",") && nr_float(tokens, at) && nr_take(tokens, at, ")");
    joints = nr_keyword(tokens, *at, "joint") ? 1 :
        (nr_keyword(tokens, *at, "bonecenter") || nr_keyword(tokens, *at, "bonedir")) ? 2 : 0;
    if (!joints) return 0;
    (*at)++;
    if (!nr_take(tokens, at, "(") || !nr_atom(tokens, at, 0)) return 0;
    if (joints == 2 && (!nr_take(tokens, at, ",") || !nr_atom(tokens, at, 0))) return 0;
    return nr_take(tokens, at, ")");
}

static int nr_reference(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    const char *family, int quoted)
{
    size_t index = *at, offset, length;
    char *name;
    int ok;
    if (!nr_atom(tokens, at, quoted)) return 0;
    offset = tokens->items[index].begin;
    length = tokens->items[index].end - offset;
    if (tokens->items[index].kind == SH_DECL_TOKEN_STRING) { offset++; length -= 2; }
    if (!length) return 1;
    name = malloc(length + 1); if (!name) return 0;
    memcpy(name, walk->source.text + offset, length); name[length] = 0;
    ok = walk->visitor(walk->context, family, name, offset, length);
    free(name); return ok;
}

enum {
    NR_AF_SETTINGS = 1, NR_AF_BODY = 2, NR_AF_FIXED = 4, NR_AF_BALL = 8,
    NR_AF_UNIVERSAL = 16, NR_AF_HINGE = 32, NR_AF_SLIDER = 64, NR_AF_SPRING = 128,
    NR_AF_SOLVER = 256,
    NR_AF_JOINTS = NR_AF_FIXED | NR_AF_BALL | NR_AF_UNIVERSAL | NR_AF_HINGE | NR_AF_SLIDER | NR_AF_SPRING,
    NR_AF_ANGULAR = NR_AF_BALL | NR_AF_UNIVERSAL | NR_AF_HINGE
};
typedef struct nr_af_field { const char *name, *pattern; unsigned blocks; } nr_af_field;
static const nr_af_field nr_af_fields[] = {
    {"mesh", "s", NR_AF_SETTINGS}, {"anim", "s", NR_AF_SETTINGS},
    {"model", "D", NR_AF_SETTINGS}, {"clipMaterial", "R", NR_AF_SETTINGS},
    {"skin", "K", NR_AF_SETTINGS}, {"base", "t", NR_AF_SETTINGS},
    {"defaultBouncyness", "n", NR_AF_SETTINGS}, {"defaultLinearFriction", "n", NR_AF_SETTINGS},
    {"defaultAngularFriction", "n", NR_AF_SETTINGS}, {"defaultContactFriction", "n", NR_AF_SETTINGS},
    {"defaultConstraintFriction", "n", NR_AF_SETTINGS}, {"suspendSpeed", "n,n,n,n", NR_AF_SETTINGS},
    {"noMoveTime", "n", NR_AF_SETTINGS}, {"noMoveTranslation", "n", NR_AF_SETTINGS},
    {"noMoveRotation", "n", NR_AF_SETTINGS}, {"minMoveTime", "n", NR_AF_SETTINGS},
    {"maxMoveTime", "n", NR_AF_SETTINGS},
    {"solverConstants", "S", NR_AF_SETTINGS}, {"syncSolverConstants", "S", NR_AF_SETTINGS},
    {"contents", "F", NR_AF_SETTINGS | NR_AF_BODY}, {"clipMask", "F", NR_AF_SETTINGS | NR_AF_BODY},
    {"selfCollision", "b", NR_AF_SETTINGS | NR_AF_BODY},
    {"model", "M", NR_AF_BODY}, {"joint", "s", NR_AF_BODY}, {"mod", "t", NR_AF_BODY},
    {"origin", "v", NR_AF_BODY}, {"angles", "v", NR_AF_BODY}, {"density", "n", NR_AF_BODY},
    {"inertiaScale", "(nnnnnnnnn)", NR_AF_BODY}, {"linearFriction", "n", NR_AF_BODY},
    {"angularFriction", "n", NR_AF_BODY}, {"contactFriction", "n", NR_AF_BODY},
    {"bouncyness", "n", NR_AF_BODY}, {"noSyncCollide", "b", NR_AF_BODY},
    {"clearClipMaskInSolid", "b", NR_AF_BODY}, {"containedJoints", "s", NR_AF_BODY},
    {"frictionDirection", "v", NR_AF_BODY}, {"contactMotorDirection", "v", NR_AF_BODY},
    {"body1", "s", NR_AF_JOINTS}, {"body2", "s", NR_AF_JOINTS},
    {"syncConstraint", "b", NR_AF_JOINTS},
    {"friction", "n", NR_AF_ANGULAR | NR_AF_SLIDER | NR_AF_SPRING},
    {"anchor", "v", NR_AF_ANGULAR}, {"shafts", "v,v", NR_AF_ANGULAR},
    {"conelimit", "v,n,v", NR_AF_BALL}, {"pyramidlimit", "v,n,n,n,v", NR_AF_BALL},
    {"conelimit", "v,n", NR_AF_UNIVERSAL}, {"pyramidlimit", "v,n,n,n", NR_AF_UNIVERSAL},
    {"limitFudge", "b", NR_AF_ANGULAR}, {"minTwist", "n", NR_AF_ANGULAR},
    {"maxTwist", "n", NR_AF_ANGULAR}, {"limit", "n,n,nH", NR_AF_HINGE},
    {"axis", "v", NR_AF_HINGE | NR_AF_SLIDER},
    {"anchor1", "v", NR_AF_SPRING}, {"anchor2", "v", NR_AF_SPRING},
    {"stretch", "n", NR_AF_SPRING}, {"compress", "n", NR_AF_SPRING},
    {"damping", "n", NR_AF_SPRING}, {"restLength", "n", NR_AF_SPRING},
    {"minLength", "n", NR_AF_SPRING}, {"maxLength", "n", NR_AF_SPRING},
    {"errorReduction", "n", NR_AF_SOLVER}, {"errorReductionMax", "n", NR_AF_SOLVER},
    {"lcpEpsilon", "n", NR_AF_SOLVER}, {"maxLcpEpsilon", "n", NR_AF_SOLVER},
    {"limitErrorReduction", "n", NR_AF_SOLVER}, {"limitErrorReductionMax", "n", NR_AF_SOLVER},
    {"limitLcpEpsilon", "n", NR_AF_SOLVER}, {"contactErrorReduction", "n", NR_AF_SOLVER},
    {"contactErrorReductionMax", "n", NR_AF_SOLVER}, {"contactLcpEpsilon", "n", NR_AF_SOLVER},
    {"universalErrorReduction", "n", NR_AF_SOLVER}, {"universalErrorReductionMax", "n", NR_AF_SOLVER},
    {"universalTorsionLcpEpsilon", "n", NR_AF_SOLVER}, {"minTwistErrorReduction", "n", NR_AF_SOLVER},
    {"maxTwistErrorReduction", "n", NR_AF_SOLVER}
};

static int nr_af_block(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    unsigned block, const char **reason);
static int nr_af_pattern(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    const char *pattern, const char **reason)
{
    for (; *pattern; pattern++) {
        switch (*pattern) {
        case 's': if (!nr_atom(tokens, at, 1)) return 0; break;
        case 't': if (!nr_atom(tokens, at, 0)) return 0; break;
        case 'n': if (!nr_float(tokens, at)) return 0; break;
        case 'i': {
            size_t index;
            nr_take(tokens, at, "-"); index = *at;
            if (index >= tokens->count || tokens->items[index].kind != SH_DECL_TOKEN_NUMBER) return 0;
            for (size_t i = tokens->items[index].begin; i < tokens->items[index].end; i++)
                if (tokens->source.text[i] < '0' || tokens->source.text[i] > '9') return 0;
            (*at)++; break;
        }
        case 'b':
            if (*at >= tokens->count || (tokens->items[*at].kind != SH_DECL_TOKEN_NUMBER &&
                !nr_keyword(tokens, *at, "true") && !nr_keyword(tokens, *at, "false"))) return 0;
            (*at)++; break;
        case 'v': if (!nr_af_vector(tokens, at)) return 0; break;
        case 'D': if (!nr_reference(walk, tokens, at, "md6def", 1)) return 0; break;
        case 'R': if (!nr_reference(walk, tokens, at, "material", 1)) return 0; break;
        case 'C': if (!nr_reference(walk, tokens, at, "cm", 1)) return 0; break;
        case 'K': {
            size_t index = *at;
            if (!nr_atom(tokens, at, 1)) return 0;
            if (tokens->items[index].end - tokens->items[index].begin != 2) {
                *reason = "nonempty articulated figure skin requires a verified native binding"; return 0;
            }
            break;
        }
        case 'F':
            if (!nr_atom(tokens, at, 0)) return 0;
            while (nr_take(tokens, at, ",")) if (!nr_atom(tokens, at, 0)) return 0;
            break;
        case 'H': if (nr_take(tokens, at, ",") && !nr_float(tokens, at)) return 0; break;
        case 'S': if (!nr_af_block(walk, tokens, at, NR_AF_SOLVER, reason)) return 0; break;
        case 'M': {
            const char *shape = NULL;
            if (nr_keyword(tokens, *at, "box") || nr_keyword(tokens, *at, "octahedron") ||
                nr_keyword(tokens, *at, "dodecahedron")) shape = "(v,v)";
            else if (nr_keyword(tokens, *at, "cylinder") || nr_keyword(tokens, *at, "cone")) shape = "(v,v,i)";
            else if (nr_keyword(tokens, *at, "bone")) shape = "(v,v,n)";
            else if (nr_keyword(tokens, *at, "custom")) shape = "(C,v,v)";
            if (!shape) return 0;
            (*at)++;
            if (!nr_af_pattern(walk, tokens, at, shape, reason)) return 0;
            break;
        }
        default: {
            char punctuation[2] = {*pattern, 0};
            if (!nr_take(tokens, at, punctuation)) return 0;
            break;
        }
        }
    }
    return 1;
}

static int nr_af_block(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    unsigned block, const char **reason)
{
    if (!nr_take(tokens, at, "{")) return 0;
    while (*at < tokens->count && !nr_take(tokens, at, "}")) {
        size_t field;
        for (field = 0; field < sizeof(nr_af_fields) / sizeof(nr_af_fields[0]); field++)
            if ((nr_af_fields[field].blocks & block) && nr_keyword(tokens, *at, nr_af_fields[field].name)) break;
        if (field == sizeof(nr_af_fields) / sizeof(nr_af_fields[0])) return 0;
        (*at)++;
        if (!nr_af_pattern(walk, tokens, at, nr_af_fields[field].pattern, reason)) return 0;
    }
    return *at <= tokens->count && sh_decl_token_is(tokens, *at - 1, "}");
}

static int nr_af(nr_walk *walk, char *error, size_t capacity)
{
    static const char *const blocks[] = {"settings", "body", "fixed", "ballAndSocketJoint",
        "universalJoint", "hinge", "slider", "spring"};
    sh_decl_tokens tokens = {0};
    const char *reason = "unsupported articulated figure grammar";
    size_t at = 1;
    int ok = 0;
    if (!sh_decl_native_lex(walk->source, &tokens, error, capacity)) return 0;
    if (tokens.count < 2 || !sh_decl_token_is(&tokens, 0, "{") ||
        tokens.items[0].close != tokens.count - 1) goto done;
    while (at < tokens.count - 1) {
        size_t block;
        for (block = 0; block < sizeof(blocks) / sizeof(blocks[0]); block++)
            if (nr_keyword(&tokens, at, blocks[block])) break;
        if (block == sizeof(blocks) / sizeof(blocks[0])) goto done;
        at++;
        if (block && !nr_atom(&tokens, &at, 1)) goto done;
        if (!nr_af_block(walk, &tokens, &at, 1u << block, &reason)) goto done;
    }
    ok = at == tokens.count - 1;
done:
    if (!ok && error && capacity) snprintf(error, capacity, "%s at byte %zu", reason,
        at < tokens.count ? tokens.items[at].begin : walk->source.length);
    sh_decl_tokens_free(&tokens); return ok;
}

/* Breakable source is a token grammar, not reflected assignment state. The
 * model manager is discreteanimation; identical spelling in another grammar
 * does not establish the same resource family. Lists of local piece indices,
 * names, armor/health and damage spheres perform no external resource lookup.
 * Leave their values and native model-dependent checks to the engine. */
enum { NR_BR_ROOT = 1, NR_BR_EXPLOSION = 2, NR_BR_TRAIL = 4, NR_BR_DECAL = 8 };
static const nr_af_field nr_br_fields[] = {
    {"model", "D", NR_BR_ROOT},
    {"impactParticle", "P", NR_BR_ROOT}, {"deathParticle", "P", NR_BR_ROOT},
    {"harmlessParticle", "P", NR_BR_ROOT},
    {"pieceNames", "L", NR_BR_ROOT}, {"useablePieces", "L", NR_BR_ROOT},
    {"touchingBasePieces", "L", NR_BR_ROOT}, {"armoredPieces", "L", NR_BR_ROOT},
    {"healthPieces", "L", NR_BR_ROOT}, {"damageSpheres", "L", NR_BR_ROOT},
    {"linearFriction", "n", NR_BR_ROOT}, {"angularFriction", "n", NR_BR_ROOT},
    {"contactFriction", "n", NR_BR_ROOT}, {"linearFrictionWater", "n", NR_BR_ROOT},
    {"angularFrictionWater", "n", NR_BR_ROOT}, {"bouncyness", "n", NR_BR_ROOT},
    {"dampeningDecay", "n", NR_BR_ROOT}, {"gravity", "nnn", NR_BR_ROOT},
    {"worldCollisionOnly", "b", NR_BR_ROOT}, {"simplePointCollision", "b", NR_BR_ROOT},
    {"noShadows", "b", NR_BR_ROOT}, {"crazyBounceChance", "n", NR_BR_ROOT},
    {"maxSimulationTime", "n", NR_BR_ROOT}, {"stopSpeed", "n", NR_BR_ROOT},
    {"maxLinearVelocity", "n", NR_BR_ROOT}, {"maxAngularVelocity", "n", NR_BR_ROOT},
    {"clipMask", "F", NR_BR_ROOT},
    {"explosion", "E", NR_BR_ROOT}, {"trail", "T", NR_BR_ROOT}, {"decal", "C", NR_BR_ROOT},
    {"name", "t", NR_BR_EXPLOSION | NR_BR_TRAIL | NR_BR_DECAL},
    {"particle", "P", NR_BR_EXPLOSION}, {"fx", "X", NR_BR_EXPLOSION},
    {"position", "nnn", NR_BR_EXPLOSION}, {"radius", "n", NR_BR_EXPLOSION},
    {"impulse", "n", NR_BR_EXPLOSION}, {"falloff", "n", NR_BR_EXPLOSION},
    {"angular", "n", NR_BR_EXPLOSION}, {"activateAllDelay", "n", NR_BR_EXPLOSION},
    {"excludePieces", "L", NR_BR_EXPLOSION},
    {"delay", "n", NR_BR_EXPLOSION | NR_BR_TRAIL},
    {"duration", "n", NR_BR_EXPLOSION | NR_BR_TRAIL},
    {"particleTrail", "P", NR_BR_TRAIL}, {"particleBounce", "P", NR_BR_TRAIL},
    {"particleDie", "P", NR_BR_TRAIL}, {"glowQuadMaterial", "R", NR_BR_TRAIL},
    {"glowQuadSize", "n", NR_BR_TRAIL}, {"fadeInDuration", "n", NR_BR_TRAIL},
    {"fadeOutDuration", "n", NR_BR_TRAIL}, {"minTrailVelocity", "n", NR_BR_TRAIL},
    {"minBounceVelocity", "n", NR_BR_TRAIL | NR_BR_DECAL},
    {"trailSpacing", "n", NR_BR_TRAIL}, {"trailVelocityScale", "n", NR_BR_TRAIL},
    {"randomPercentage", "n", NR_BR_TRAIL}, {"pieces", "L", NR_BR_TRAIL | NR_BR_DECAL},
    {"maxSizeX", "n", NR_BR_DECAL}, {"maxSizeY", "n", NR_BR_DECAL},
    {"minSizeX", "n", NR_BR_DECAL}, {"minSizeY", "n", NR_BR_DECAL},
    {"lifetime", "n", NR_BR_DECAL}, {"fadetime", "n", NR_BR_DECAL},
    {"fadeInEndTime", "n", NR_BR_DECAL}, {"decalSpacing", "n", NR_BR_DECAL},
    {"splatterOnBounce", "b", NR_BR_DECAL}, {"splatterOnRest", "b", NR_BR_DECAL},
    {"depth", "n", NR_BR_DECAL},
    {"dripMinSizeX", "n", NR_BR_DECAL}, {"dripMinSizeY", "n", NR_BR_DECAL},
    {"floorDeclMinSizeX", "n", NR_BR_DECAL}, {"floorDeclMinSizeY", "n", NR_BR_DECAL},
    {"materials", "{R}", NR_BR_DECAL}, {"dripMaterials", "{R}", NR_BR_DECAL},
    {"floorDecalFromCeilingDrip", "{R}", NR_BR_DECAL},
    {"ceilingParticles", "{P}", NR_BR_DECAL}, {"floorParticleFromCeilingDrip", "{P}", NR_BR_DECAL}
};

static int nr_br_block(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at, unsigned block);
/* Stock breakables write NULL for an absent effect or particle (fx NULL,
 * particleTrail NULL): a sentinel that names no resource. */
static int nr_br_null(const sh_decl_tokens *tokens, size_t *at)
{
    if (!nr_keyword(tokens, *at, "NULL")) return 0;
    (*at)++; return 1;
}
static int nr_br_pattern(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at, const char *pattern)
{
    for (; *pattern; pattern++) {
        const char *family = *pattern == 'D' ? "discreteanimation" :
            *pattern == 'P' ? "particle" : *pattern == 'R' ? "material" : *pattern == 'X' ? "fx" : NULL;
        if (family) { if (!nr_br_null(tokens, at) && !nr_reference(walk, tokens, at, family, 0)) return 0; continue; }
        switch (*pattern) {
        case 'n': if (!nr_float(tokens, at)) return 0; break;
        case 'b': case 't': if (!nr_atom(tokens, at, 0)) return 0; break;
        case 'F':
            if (!nr_atom(tokens, at, 0)) return 0;
            while (nr_take(tokens, at, ",")) if (!nr_atom(tokens, at, 0)) return 0;
            break;
        case 'L':
            if (!sh_decl_token_is(tokens, *at, "{") || tokens->items[*at].close == SIZE_MAX) return 0;
            *at = tokens->items[*at].close + 1; break;
        case 'E': if (!nr_br_block(walk, tokens, at, NR_BR_EXPLOSION)) return 0; break;
        case 'T': if (!nr_br_block(walk, tokens, at, NR_BR_TRAIL)) return 0; break;
        case 'C': if (!nr_br_block(walk, tokens, at, NR_BR_DECAL)) return 0; break;
        case '{': {
            /* Resource lists read every token as a name; commas are not list
             * separators here, unlike clipMask and local indexed piece lists. */
            const char *list_family = pattern[1] == 'R' ? "material" : "particle";
            if (!nr_take(tokens, at, "{")) return 0;
            while (*at < tokens->count && !sh_decl_token_is(tokens, *at, "}"))
                if (!nr_reference(walk, tokens, at, list_family, 0)) return 0;
            if (!nr_take(tokens, at, "}")) return 0;
            pattern += 2; break;
        }
        default: return 0;
        }
    }
    return 1;
}
static int nr_br_block(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at, unsigned block)
{
    if (!nr_take(tokens, at, "{")) return 0;
    while (*at < tokens->count && !sh_decl_token_is(tokens, *at, "}")) {
        size_t i;
        for (i = 0; i < sizeof(nr_br_fields) / sizeof(nr_br_fields[0]); i++)
            if ((nr_br_fields[i].blocks & block) && nr_keyword(tokens, *at, nr_br_fields[i].name)) break;
        if (i == sizeof(nr_br_fields) / sizeof(nr_br_fields[0])) return 0;
        (*at)++;
        if (!nr_br_pattern(walk, tokens, at, nr_br_fields[i].pattern)) return 0;
    }
    return nr_take(tokens, at, "}");
}
static int nr_breakable(nr_walk *walk, char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0}; size_t at = 0;
    if (!sh_decl_native_lex(walk->source, &tokens, error, capacity)) return 0;
    int ok = nr_br_block(walk, &tokens, &at, NR_BR_ROOT) && at == tokens.count;
    if (!ok && error && capacity) snprintf(error, capacity, "unsupported breakable grammar at byte %zu",
        at < tokens.count ? tokens.items[at].begin : walk->source.length);
    sh_decl_tokens_free(&tokens); return ok;
}

/* Native lookup tables contain numeric curve samples, never resource names.
 * Validate their own grammar instead of misreading them as reflected edit state.
 * The 64-point bound is the engine's fixed curve storage, not a package quota. */
static int nr_table(sh_decl_source source, char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0};
    size_t at = 1, points = 0;
    int ok = 0;
    const char *reason = "unsupported numeric table grammar";
    if (!sh_decl_native_lex(source, &tokens, error, capacity)) return 0;
    if (tokens.count < 2 || !sh_decl_token_is(&tokens, 0, "{") ||
        tokens.items[0].close != tokens.count - 1) goto done;
    while (at < tokens.count - 1) {
        if (nr_keyword(&tokens, at, "snap") || nr_keyword(&tokens, at, "clamp") ||
            nr_keyword(&tokens, at, "spline")) { at++; continue; }
        if (nr_keyword(&tokens, at, "min") || nr_keyword(&tokens, at, "max") ||
            nr_keyword(&tokens, at, "left") || nr_keyword(&tokens, at, "right")) {
            at++; if (!nr_float(&tokens, &at)) goto done;
            continue;
        }
        if (!sh_decl_token_is(&tokens, at, "{")) goto done;
        at++;
        for (;;) {
            if (!nr_float(&tokens, &at)) goto done;
            if (sh_decl_token_is(&tokens, at, ":")) {
                at++; if (!nr_float(&tokens, &at)) goto done;
            }
            if (++points > 64) { reason = "table exceeds the engine's 64 curve points"; goto done; }
            if (sh_decl_token_is(&tokens, at, "}")) { at++; break; }
            if (!sh_decl_token_is(&tokens, at, ",")) goto done;
            at++;
        }
    }
    ok = at == tokens.count - 1;
done:
    if (!ok && error && capacity) snprintf(error, capacity, "%s", reason);
    sh_decl_tokens_free(&tokens); return ok;
}

/* Animation webs have an ordered grammar, not reflected assignments. Model
 * aliases, state names, tag names and modelInfos labels are local identifiers.
 * Only the typed model/web lists and direct animation operands are bindings. */
enum { NR_AW_PROPS = 1, NR_AW_SUB_PROPS = 2, NR_AW_NODE_PROPS = 4,
    NR_AW_ALIAS = 8, NR_AW_BLEND = 16, NR_AW_LAYER = 32, NR_AW_STATE = 64 };
typedef struct nr_aw_field { const char *name, *pattern; unsigned blocks; } nr_aw_field;
static const nr_aw_field nr_aw_fields[] = {
    {"editPos", "(nnn)", NR_AW_PROPS}, {"gridSize", "i", NR_AW_PROPS},
    {"showGrid", "b", NR_AW_PROPS}, {"alwaysShowIntraSubwebLinks", "b", NR_AW_PROPS},
    {"snapToGrid", "b", NR_AW_PROPS}, {"alwaysShowNodeProps", "b", NR_AW_PROPS},
    {"showAbbreviatedNodeProps", "b", NR_AW_PROPS}, {"showIncomingLinks", "b", NR_AW_PROPS},
    {"defaultBlendOutWindow", "i", NR_AW_PROPS}, {"defaultBlendDuration", "i", NR_AW_PROPS},
    {"defaultBlendType", "t", NR_AW_PROPS}, {"subWebBlend", "s", NR_AW_PROPS},
    {"mergeModels", "b", NR_AW_PROPS}, {"models", "D", NR_AW_PROPS},
    {"modelInfos", "I", NR_AW_PROPS}, {"composite", "W", NR_AW_PROPS},
    {"showAnimName", "b", NR_AW_PROPS}, {"showWrapMode", "b", NR_AW_PROPS},
    {"showRate", "b", NR_AW_PROPS}, {"showDelta", "b", NR_AW_PROPS},
    {"showBlendEq", "b", NR_AW_PROPS}, {"showTransition", "b", NR_AW_PROPS},
    {"showWeakRefs", "b", NR_AW_PROPS},
    {"color", "(nnnn)", NR_AW_SUB_PROPS}, {"hub", "b", NR_AW_SUB_PROPS},
    {"visible", "b", NR_AW_SUB_PROPS | NR_AW_LAYER}, {"subwebs", "S", NR_AW_LAYER},
    {"destination", "", NR_AW_STATE},
    {"delta", "t", NR_AW_NODE_PROPS}, {"pos", "(nnn)", NR_AW_NODE_PROPS},
    {"startTransition", "", NR_AW_NODE_PROPS}, {"transition", "", NR_AW_NODE_PROPS},
    {"stopTransition", "", NR_AW_NODE_PROPS}, {"stepTransition", "", NR_AW_NODE_PROPS},
    {"turnTransition", "", NR_AW_NODE_PROPS}, {"genericTransition", "", NR_AW_NODE_PROPS},
    {"focusTransition", "", NR_AW_NODE_PROPS}, {"customFlags", "L", NR_AW_NODE_PROPS},
    {"metaData", "M", NR_AW_NODE_PROPS},
    {"wrap", "t", NR_AW_ALIAS}, {"rate", "n", NR_AW_ALIAS},
    {"randomStart", "", NR_AW_ALIAS}, {"tags", "L", NR_AW_ALIAS},
    {"coordinate", "V", NR_AW_ALIAS},
    {"srcAnim", "A", NR_AW_BLEND}, {"exceptSrcAnim", "A", NR_AW_BLEND},
    {"destAnim", "A", NR_AW_BLEND}, {"exceptLooping", "", NR_AW_BLEND},
    {"sourceStartFrame", "i", NR_AW_BLEND}, {"sourceDuration", "i", NR_AW_BLEND},
    {"destStartFrame", "i", NR_AW_BLEND}, {"destDuration", "i", NR_AW_BLEND},
    {"duration", "i", NR_AW_BLEND}, {"durationf", "i", NR_AW_BLEND},
    {"srcFirstFrame", "i", NR_AW_BLEND}, {"srcLastFrame", "i", NR_AW_BLEND},
    {"destFrame", "i", NR_AW_BLEND}, {"originBlend", "t", NR_AW_BLEND},
    {"blendType", "t", NR_AW_BLEND}, {"sourceEndRelative", "", NR_AW_BLEND},
    {"destEndRelative", "", NR_AW_BLEND}
};

static int nr_aw_keyword(const sh_decl_tokens *tokens, size_t *at, const char *word)
{
    if (!nr_keyword(tokens, *at, word)) return 0;
    (*at)++; return 1;
}

static int nr_aw_pattern(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    const char *pattern, size_t field_line, int *composite)
{
    const char *reason = NULL;
    for (; *pattern; pattern++) {
        switch (*pattern) {
        case 'D': case 'I': case 'W': case 'S':
            if (!nr_take(tokens, at, "{")) return 0;
            if (*pattern == 'W') *composite = 1;
            while (*at < tokens->count && !sh_decl_token_is(tokens, *at, "}")) {
                if (*pattern == 'W' && nr_take(tokens, at, ",")) continue;
                if (*pattern == 'S') { if (!nr_atom(tokens, at, 1)) return 0; }
                else if (!nr_reference(walk, tokens, at, *pattern == 'W' ? "animweb" : "md6def",
                    *pattern != 'W')) return 0;
                if (*pattern == 'I' && !nr_atom(tokens, at, 0)) return 0;
            }
            if (!nr_take(tokens, at, "}")) return 0;
            break;
        case 'A': if (!nr_reference(walk, tokens, at, "anim", 1)) return 0; break;
        case 'L':
            /* Native ReadTokenOnLine consumes tags/custom flags to newline,
             * including tokens that resemble another field. Never bind them. */
            while (*at < tokens->count && tokens->items[*at].line == field_line) {
                if (tokens->items[*at].kind == SH_DECL_TOKEN_PUNCTUATION) return 0;
                (*at)++;
            }
            break;
        case 'M':
            /* Both supported builds dispatch idMetaDataSet* by exact class/
             * enum name, find no reader, and skip this balanced source block.
             * idMetaDataSetBase is a distinct type, not a fallback reader. */
            if (!nr_take(tokens, at, "=") || !sh_decl_token_is(tokens, *at, "{") ||
                tokens->items[*at].close == SIZE_MAX) return 0;
            *at = tokens->items[*at].close + 1; break;
        case 'V':
            if (!nr_take(tokens, at, "(")) return 0;
            while (*at < tokens->count && !sh_decl_token_is(tokens, *at, ")")) {
                if (nr_take(tokens, at, ",")) continue;
                if (!nr_float(tokens, at)) return 0;
            }
            if (!nr_take(tokens, at, ")")) return 0;
            break;
        default: {
            char single[] = {*pattern, 0};
            if (!nr_af_pattern(walk, tokens, at, single, &reason)) return 0;
            break;
        }
        }
    }
    return 1;
}

static int nr_aw_fields_read(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    unsigned block, int *composite)
{
    while (*at < tokens->count && !sh_decl_token_is(tokens, *at, "}")) {
        size_t i, line = tokens->items[*at].line;
        for (i = 0; i < sizeof(nr_aw_fields) / sizeof(nr_aw_fields[0]); i++)
            if ((nr_aw_fields[i].blocks & block) && nr_keyword(tokens, *at, nr_aw_fields[i].name)) break;
        if (i == sizeof(nr_aw_fields) / sizeof(nr_aw_fields[0])) return 0;
        (*at)++;
        if (!nr_aw_pattern(walk, tokens, at, nr_aw_fields[i].pattern, line, composite)) return 0;
    }
    return nr_take(tokens, at, "}");
}

static int nr_aw_block(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    unsigned block, int *composite)
{
    return nr_take(tokens, at, "{") && nr_aw_fields_read(walk, tokens, at, block, composite);
}

static int nr_aw_name(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at)
{
    size_t index = *at, length, dot = SIZE_MAX;
    const char *name;
    if (!nr_atom(tokens, at, 1)) return 0;
    name = tokens->source.text + tokens->items[index].begin + 1;
    length = tokens->items[index].end - tokens->items[index].begin - 2;
    for (size_t i = 0; i < length; i++) {
        if (name[i] == '/' || name[i] == '\\') dot = SIZE_MAX;
        else if (name[i] == '.') dot = i;
    }
    if (dot == SIZE_MAX) return 1; /* Model animation alias, not a file. */
    if (length - dot != 8 || _strnicmp(name + dot, ".md6anim", 8)) return 0;
    /* Native compares this sentinel case-sensitively after slash normalization. */
    if (length == sizeof("_default.md6anim") - 1 && !memcmp(name, "_default.md6anim", length)) return 1;
    *at = index;
    return nr_reference(walk, tokens, at, "anim", 1);
}

static int nr_aw_tree(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at, int *composite)
{
    if (!nr_aw_keyword(tokens, at, "tree") || !nr_take(tokens, at, "{")) return 0;
    if (nr_aw_keyword(tokens, at, "modelIndex") &&
        !nr_aw_pattern(walk, tokens, at, "i", 0, composite)) return 0;
    if (!nr_aw_keyword(tokens, at, "blendEq") || !nr_atom(tokens, at, 1)) return 0;
    while (nr_aw_keyword(tokens, at, "tagGroup")) {
        if (!nr_atom(tokens, at, 1) || !nr_take(tokens, at, "{")) return 0;
        while (nr_aw_keyword(tokens, at, "tag"))
            if (!nr_atom(tokens, at, 1) || !nr_aw_pattern(walk, tokens, at, "i", 0, composite)) return 0;
        if (!nr_take(tokens, at, "}")) return 0;
    }
    if (!nr_aw_keyword(tokens, at, "anims") || !nr_take(tokens, at, "{")) return 0;
    while (nr_aw_keyword(tokens, at, "alias")) {
        if (!nr_take(tokens, at, "{") || !nr_aw_keyword(tokens, at, "name") ||
            !nr_aw_name(walk, tokens, at) || !nr_aw_fields_read(walk, tokens, at, NR_AW_ALIAS, composite)) return 0;
    }
    return nr_take(tokens, at, "}") && nr_take(tokens, at, "}");
}

static int nr_aw_node(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at, int *composite)
{
    size_t states = 0;
    while (*at < tokens->count && !sh_decl_token_is(tokens, *at, "{"))
        if (++states > 4 || !nr_atom(tokens, at, 0)) return 0;
    if (!nr_take(tokens, at, "{") || !nr_aw_keyword(tokens, at, "props") ||
        !nr_aw_block(walk, tokens, at, NR_AW_NODE_PROPS, composite) ||
        !nr_aw_keyword(tokens, at, "blendTrees") || !nr_take(tokens, at, "{")) return 0;
    while (nr_keyword(tokens, *at, "tree"))
        if (!nr_aw_tree(walk, tokens, at, composite)) return 0;
    if (!nr_take(tokens, at, "}") || !nr_aw_keyword(tokens, at, "edges") || !nr_take(tokens, at, "{")) return 0;
    while (nr_aw_keyword(tokens, at, "edge")) {
        if (!nr_take(tokens, at, "{") || !nr_aw_keyword(tokens, at, "toState") || !nr_atom(tokens, at, 1)) return 0;
        if (nr_aw_keyword(tokens, at, "toSubWeb") && !nr_atom(tokens, at, 1)) return 0;
        if (nr_aw_keyword(tokens, at, "weightScale") && !nr_float(tokens, at)) return 0;
        if (nr_aw_keyword(tokens, at, "randomizeWeight") &&
            !nr_aw_pattern(walk, tokens, at, "b", 0, composite)) return 0;
        if (nr_aw_keyword(tokens, at, "blendParms") &&
            !nr_aw_block(walk, tokens, at, NR_AW_BLEND, composite)) return 0;
        if (nr_aw_keyword(tokens, at, "customFlags") &&
            !nr_aw_pattern(walk, tokens, at, "L", tokens->items[*at - 1].line, composite)) return 0;
        if (!nr_take(tokens, at, "}")) return 0;
    }
    return nr_take(tokens, at, "}") && nr_take(tokens, at, "}");
}

static int nr_aw_named_blocks(nr_walk *walk, const sh_decl_tokens *tokens, size_t *at,
    const char *kind, unsigned block, int *composite)
{
    if (!nr_take(tokens, at, "{")) return 0;
    while (nr_aw_keyword(tokens, at, kind))
        if (!nr_atom(tokens, at, 1) || !nr_aw_block(walk, tokens, at, block, composite)) return 0;
    return nr_take(tokens, at, "}");
}

static int nr_animation_web(nr_walk *walk, char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0}; size_t at = 0; int composite = 0, ok = 0;
    if (!sh_decl_native_lex(walk->source, &tokens, error, capacity)) return 0;
    if (!nr_take(&tokens, &at, "{")) goto done;
    if (!sh_decl_token_is(&tokens, at, "}")) {
        if (!nr_aw_keyword(&tokens, &at, "props") ||
            !nr_aw_block(walk, &tokens, &at, NR_AW_PROPS, &composite)) goto done;
        if (!composite) {
            if (nr_aw_keyword(&tokens, &at, "layers") &&
                !nr_aw_named_blocks(walk, &tokens, &at, "layer", NR_AW_LAYER, &composite)) goto done;
            if (!nr_aw_keyword(&tokens, &at, "states") ||
                !nr_aw_named_blocks(walk, &tokens, &at, "state", NR_AW_STATE, &composite)) goto done;
            if (nr_aw_keyword(&tokens, &at, "scalars")) {
                if (!nr_take(&tokens, &at, "{")) goto done;
                while (at < tokens.count && tokens.items[at].kind == SH_DECL_TOKEN_NAME) {
                    at++; if (!nr_aw_pattern(walk, &tokens, &at, "i", 0, &composite)) goto done;
                }
                if (!nr_take(&tokens, &at, "}")) goto done;
            }
            if (!nr_aw_keyword(&tokens, &at, "subWebs") || !nr_take(&tokens, &at, "{")) goto done;
            while (nr_aw_keyword(&tokens, &at, "subWeb")) {
                if (!nr_atom(&tokens, &at, 1) || !nr_take(&tokens, &at, "{")) goto done;
                while (at < tokens.count && !sh_decl_token_is(&tokens, at, "}")) {
                    if (nr_aw_keyword(&tokens, &at, "props")) {
                        if (!nr_aw_block(walk, &tokens, &at, NR_AW_SUB_PROPS, &composite)) goto done;
                    } else if (!nr_aw_keyword(&tokens, &at, "node") ||
                        !nr_aw_node(walk, &tokens, &at, &composite)) goto done;
                }
                if (!nr_take(&tokens, &at, "}")) goto done;
            }
            if (!nr_take(&tokens, &at, "}")) goto done;
        }
    }
    ok = nr_take(&tokens, &at, "}") && at == tokens.count;
done:
    if (!ok && error && capacity) snprintf(error, capacity, "unsupported animation web grammar at byte %zu",
        at < tokens.count ? tokens.items[at].begin : walk->source.length);
    sh_decl_tokens_free(&tokens); return ok;
}

static int nr_md6(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    nr_walk *walk = context;
    if (offset > walk->source.length || length > walk->source.length - offset) return 0;
    if (length >= 2 && walk->source.text[offset] == '"' && walk->source.text[offset + length - 1] == '"') {
        offset++; length -= 2;
    }
    if (length != strlen(name) || memcmp(walk->source.text + offset, name, length)) return 0;
    return walk->visitor(walk->context, family, name, offset, length);
}

static int nr_material(nr_walk *walk, const sh_decl_material_reference *refs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const sh_decl_material_reference *ref = &refs[i];
        uintptr_t start = (uintptr_t)ref->name.text, base = (uintptr_t)walk->source.text;
        size_t offset = SIZE_MAX;
        char *name;
        int ok;
        if (!strcmp(ref->family, "sampler")) continue;
        if (ref->name.length == SIZE_MAX || !(name = malloc(ref->name.length + 1))) return 0;
        memcpy(name, ref->name.text, ref->name.length); name[ref->name.length] = 0;
        if (start >= base && start - base <= walk->source.length &&
            ref->name.length <= walk->source.length - (size_t)(start - base)) offset = (size_t)(start - base);
        ok = walk->visitor(walk->context, ref->family, name, offset, ref->name.length);
        free(name); if (!ok) return 0;
    }
    return 1;
}

/* idDeclEnv::Parse (Vulkan 0x1417af1d0, OpenGL 0x1417a1cb0): inherit { base }
 * finds its parent env without a default (an unknown base is a parse error);
 * renderParms { ... } is read by the same parm block reader materials use
 * (0x140db3710), so it binds render parameters and their values exactly like a
 * material's parameters; allowOverride is the assignment `= <boolean> ;`. */
static int nr_environment(nr_walk *walk, const sh_decl_material_schema *schema, char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0}; size_t at = 0; int ok = 0;
    if (!sh_decl_native_lex(walk->source, &tokens, error, capacity)) return 0;
    if (!nr_take(&tokens, &at, "{")) goto done;
    while (at < tokens.count && !sh_decl_token_is(&tokens, at, "}")) {
        if (nr_keyword(&tokens, at, "inherit")) {
            at++;
            if (!nr_take(&tokens, &at, "{")) goto done;
            if (!sh_decl_token_is(&tokens, at, "}") && !nr_reference(walk, &tokens, &at, "env", 0)) goto done;
            if (!nr_take(&tokens, &at, "}")) goto done;
        } else if (nr_keyword(&tokens, at, "renderParms")) {
            at++;
            if (!sh_decl_token_is(&tokens, at, "{")) goto done;
            size_t close = tokens.items[at].close;
            if (close >= tokens.count) goto done;
            sh_decl_source block = {walk->source.text + tokens.items[at].begin,
                tokens.items[close].end - tokens.items[at].begin};
            sh_decl_material parameters = {0};
            int read = sh_decl_material_read(block, schema, &parameters, error, capacity) &&
                nr_material(walk, parameters.references, parameters.reference_count);
            sh_decl_material_free(&parameters);
            if (!read) goto done;
            at = close + 1;
        } else if (nr_keyword(&tokens, at, "allowOverride")) {
            at++;
            if (!nr_take(&tokens, &at, "=") || !nr_bool(&tokens, &at) || !nr_take(&tokens, &at, ";")) goto done;
        } else goto done;
    }
    ok = nr_take(&tokens, &at, "}") && at == tokens.count;
done:
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "unsupported environment grammar at byte %zu",
        at < tokens.count ? tokens.items[at].begin : walk->source.length);
    sh_decl_tokens_free(&tokens); return ok;
}

/* The cloth Parse (Vulkan 0x14143c5d0) reads flat keywords. material finds its
 * declaration with makeDefault; clothData { model ... } and ropeModel { model
 * ... } find their md6 definition without a default and fail fatally when it is
 * missing ("Unable to find md6 decl"), unless clothData names none; the rest of
 * those blocks and hitTestGroups hold joint names and numbers local to the
 * cloth. clothDeclFX names a declaration type not yet mapped here: refused. */
static int nr_cloth_block(const sh_decl_tokens *tokens, size_t *at)
{
    if (!sh_decl_token_is(tokens, *at, "{") || tokens->items[*at].close >= tokens->count) return 0;
    *at = tokens->items[*at].close + 1; return 1;
}
static int nr_cloth(nr_walk *walk, char *error, size_t capacity)
{
    static const char *const scalars[] = {"width", "height", "vspacing", "hspacing", "springs", "mass",
        "friction", "collisionfriction", "gravity", "springLength", "springFriction",
        "springMaxLengthMultiplier", "collisionSphereRadiusMultiplier", "type"};
    sh_decl_tokens tokens = {0}; size_t at = 0; int ok = 0;
    if (!sh_decl_native_lex(walk->source, &tokens, error, capacity)) return 0;
    if (!nr_take(&tokens, &at, "{")) goto done;
    while (at < tokens.count && !sh_decl_token_is(&tokens, at, "}")) {
        int scalar = 0;
        for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) scalar |= nr_keyword(&tokens, at, scalars[i]);
        if (scalar) {
            at++;
            if (!nr_float(&tokens, &at) && !nr_atom(&tokens, &at, 0)) goto done;
        } else if (nr_keyword(&tokens, at, "material")) {
            at++;
            if (!nr_reference(walk, &tokens, &at, "material", 0)) goto done;
        } else if (nr_keyword(&tokens, at, "clothData") || nr_keyword(&tokens, at, "ropeModel")) {
            int cloth = nr_keyword(&tokens, at, "clothData");
            at++;
            if (!sh_decl_token_is(&tokens, at, "{")) goto done;
            size_t block = at, end = tokens.items[at].close;
            if (end >= tokens.count) goto done;
            at++;
            if (!(cloth && nr_keyword(&tokens, at, "none")) && !nr_reference(walk, &tokens, &at, "md6def", 0)) goto done;
            at = block;
            if (!nr_cloth_block(&tokens, &at)) goto done;
        } else if (nr_keyword(&tokens, at, "hitTestGroups")) {
            at++;
            if (!nr_cloth_block(&tokens, &at)) goto done;
        } else goto done;
    }
    ok = nr_take(&tokens, &at, "}") && at == tokens.count;
done:
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "unsupported cloth grammar at byte %zu",
        at < tokens.count ? tokens.items[at].begin : walk->source.length);
    sh_decl_tokens_free(&tokens); return ok;
}

int sh_decl_native_binding_family(const char *family, sh_decl_native_binding_format *format)
{
    static const struct { const char *family; sh_decl_native_binding_format format; } formats[] = {
        {"material", SH_BINDING_MATERIAL}, {"renderparm", SH_BINDING_RENDERPARM},
        {"md6def", SH_BINDING_MD6_DEFINITION}, {"table", SH_BINDING_TABLE},
        {"articulatedfigure", SH_BINDING_ARTICULATED_FIGURE}, {"breakable", SH_BINDING_BREAKABLE},
        {"animweb", SH_BINDING_ANIMATION_WEB}, {"env", SH_BINDING_ENVIRONMENT}, {"cloth", SH_BINDING_CLOTH}
    };
    if (family) for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        if (!_stricmp(family, formats[i].family)) {
            if (format) *format = formats[i].format;
            return 1;
        }
    return 0;
}

int sh_decl_native_reference_sites(sh_decl_native_binding_format format, sh_decl_source source,
    const sh_decl_material_schema *schema, sh_resource_binding_site visitor, void *context,
    char *error, size_t capacity)
{
    nr_walk walk = {source, visitor, context};
    int ok = 0;
    if (error && capacity) *error = 0;
    if (!source.text || !visitor) return 0;
    if (format == SH_BINDING_TABLE) return nr_table(source, error, capacity);
    if (format == SH_BINDING_ARTICULATED_FIGURE) return nr_af(&walk, error, capacity);
    if (format == SH_BINDING_BREAKABLE) return nr_breakable(&walk, error, capacity);
    if (format == SH_BINDING_ANIMATION_WEB) return nr_animation_web(&walk, error, capacity);
    if (format == SH_BINDING_MD6_DEFINITION)
        return sh_decl_md6_reference_sites(source, nr_md6, &walk, error, capacity);
    if (format == SH_BINDING_CLOTH) return nr_cloth(&walk, error, capacity);
    if (format == SH_BINDING_ENVIRONMENT)
        return schema && schema->resolve && nr_environment(&walk, schema, error, capacity);
    if (schema && schema->resolve && format == SH_BINDING_MATERIAL) {
        sh_decl_material material = {0};
        if (sh_decl_material_read(source, schema, &material, error, capacity))
            ok = nr_material(&walk, material.references, material.reference_count);
        sh_decl_material_free(&material);
    } else if (schema && schema->resolve && format == SH_BINDING_RENDERPARM) {
        sh_decl_renderparm parameter = {0};
        if (sh_decl_renderparm_read(source, schema, &parameter, error, capacity))
            ok = nr_material(&walk, parameter.references, parameter.reference_count);
        sh_decl_renderparm_free(&parameter);
    }
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "native declaration binding was refused");
    return ok;
}

typedef struct nr_reader {
    sh_decl_native_binding_format format;
    const sh_decl_material_schema *schema;
} nr_reader;

static int nr_sites(void *context, const unsigned char *body, size_t length,
    sh_resource_binding_site visitor, void *visitor_context, char *error, size_t capacity)
{
    nr_reader *reader = context;
    return sh_decl_native_reference_sites(reader->format, (sh_decl_source){(const char *)body, length},
        reader->schema, visitor, visitor_context, error, capacity);
}

char *sh_decl_native_rebind(sh_decl_native_binding_format format, sh_decl_source source,
    const sh_decl_material_schema *schema, sh_resource_binding_resolver resolver, void *context,
    size_t *output_length, size_t *changed, char *error, size_t capacity)
{
    nr_reader reader = {format, schema};
    return (char *)sh_resource_rebind_with(nr_sites, &reader, SH_BINDING_NATIVE_NAME,
        (const unsigned char *)source.text, source.length, resolver, context,
        output_length, changed, error, capacity);
}

typedef struct nr_rp_token { size_t begin, end; char quote; } nr_rp_token;
typedef struct nr_rp_cursor { sh_decl_source source; size_t at; } nr_rp_cursor;
/* This only locates outer commands and opaque balanced source blocks. It does
 * not expand shader preprocessing or interpret HLSL, state, flags or defines. */
static int nr_rp_token_next(nr_rp_cursor *cursor, nr_rp_token *token)
{
    const char *s = cursor->source.text; size_t n = cursor->source.length, p = cursor->at;
    for (;;) {
        while (p < n && (unsigned char)s[p] <= 32) p++;
        if (p + 1 < n && s[p] == '/' && s[p + 1] == '/') {
            p += 2; while (p < n && s[p] != '\n') p++; continue;
        }
        if (p + 1 < n && s[p] == '/' && s[p + 1] == '*') {
            p += 2; while (p + 1 < n && (s[p] != '*' || s[p + 1] != '/')) p++;
            if (p + 1 >= n) return -1;
            p += 2; continue;
        }
        break;
    }
    if (p == n) { cursor->at = p; return 0; }
    token->begin = p; token->quote = 0;
    if (s[p] == '"' || s[p] == '\'') {
        token->quote = s[p++];
        while (p < n && s[p] != token->quote) p++;
        if (p == n) return -1;
        p++;
    } else if (strchr("{};", s[p])) p++;
    else {
        do { p++; } while (p < n && (unsigned char)s[p] > 32 && !strchr("{};\"'", s[p]) &&
            !(s[p] == '/' && p + 1 < n && (s[p + 1] == '/' || s[p + 1] == '*')));
    }
    token->end = cursor->at = p; return 1;
}
static int nr_rp_token_is(sh_decl_source source, nr_rp_token token, const char *name)
{ return !token.quote && token.end - token.begin == strlen(name) && !memcmp(source.text + token.begin, name, strlen(name)); }
typedef struct nr_rp_sites {
    sh_resource_binding_site visit;
    void *context;
    size_t base;
} nr_rp_sites;
static int nr_rp_site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    nr_rp_sites *sites = context;
    return sites->visit(sites->context, family, name, offset == SIZE_MAX ? SIZE_MAX : sites->base + offset, length);
}
int sh_decl_render_program_sites(sh_decl_source source, const sh_decl_material_schema *schema,
    sh_resource_binding_site visit, void *context, char *error, size_t capacity)
{
    nr_rp_cursor cursor = {source, 0}; nr_rp_token token;
    int inherited = 0, commands = 0;
    if (!source.text || !visit || memchr(source.text, 0, source.length) || nr_rp_token_next(&cursor, &token) != 1 ||
        !nr_rp_token_is(source, token, "{")) goto gap;
    while (nr_rp_token_next(&cursor, &token) == 1) {
        if (nr_rp_token_is(source, token, "}"))
            return nr_rp_token_next(&cursor, &token) == 0;
        if (nr_rp_token_is(source, token, "inherit")) {
            if (commands || inherited || nr_rp_token_next(&cursor, &token) != 1) goto gap;
            size_t begin = token.begin + (token.quote != 0), end = token.end - (token.quote != 0);
            if (end <= begin) goto gap;
            char *name = malloc(end - begin + 1); if (!name) goto gap;
            memcpy(name, source.text + begin, end - begin); name[end - begin] = 0;
            int ok = visit(context, "renderprog", name, begin, end - begin); free(name);
            if (!ok) return 0;
            inherited = 1; continue;
        }
        static const char *const blocks[] = {"permutations", "parms", "state", "flags", "inout",
            "computelayout", "renderoutput", "buffer", "compileroptions", "wavelimits",
            "hlsl_header", "hlsl_vp_header", "hlsl_fp_header", "hlsl_cp_header", "hlsl_vp", "hlsl_fp", "hlsl_cp"};
        size_t kind = 0;
        while (kind < sizeof(blocks) / sizeof(*blocks) && !nr_rp_token_is(source, token, blocks[kind])) kind++;
        if (kind == sizeof(blocks) / sizeof(*blocks) || nr_rp_token_next(&cursor, &token) != 1 ||
            !nr_rp_token_is(source, token, "{")) goto gap;
        size_t begin = token.begin, depth = 1;
        while (depth) {
            if (nr_rp_token_next(&cursor, &token) != 1) goto gap;
            if (nr_rp_token_is(source, token, "{")) depth++;
            if (nr_rp_token_is(source, token, "}")) depth--;
        }
        if (kind == 1) {
            nr_rp_sites sites = {visit, context, begin};
            if (!sh_decl_native_reference_sites(SH_BINDING_MATERIAL,
                (sh_decl_source){source.text + begin, token.end - begin}, schema,
                nr_rp_site, &sites, error, capacity)) return 0;
        }
        commands++;
    }
gap:
    if (error && capacity && !*error) snprintf(error, capacity,
        "shader source relocation requires verified outer commands and unexpanded parameter blocks");
    return 0;
}

int sh_decl_skin_sites(sh_decl_source source, sh_resource_binding_site visit, void *context,
    char *error, size_t capacity)
{
    nr_walk walk = {source, visit, context};
    sh_decl_tokens tokens = {0}; size_t at = 0; int ok = 0;
    if (error && capacity) *error = 0;
    if (!source.text || !visit || !sh_decl_native_lex(source, &tokens, error, capacity)) return 0;
    if (!nr_take(&tokens, &at, "{")) goto done;
    while (at < tokens.count && !sh_decl_token_is(&tokens, at, "}")) {
        if (!nr_atom(&tokens, &at, 0) || !nr_take(&tokens, &at, "{")) goto done;
        while (at < tokens.count && !sh_decl_token_is(&tokens, at, "}"))
            if (!nr_reference(&walk, &tokens, &at, "material", 0) || sh_decl_token_is(&tokens, at, "}") ||
                !nr_reference(&walk, &tokens, &at, "material", 0)) goto done;
        if (!nr_take(&tokens, &at, "}")) goto done;
    }
    ok = nr_take(&tokens, &at, "}") && at == tokens.count;
done:
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "unsupported skin grammar at byte %zu",
        at < tokens.count ? tokens.items[at].begin : source.length);
    sh_decl_tokens_free(&tokens); return ok;
}
