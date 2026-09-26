#include "decl_block_compose.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decl_native_lex.h"
#include "decl_tree.h"

/* ------------------------------------------------------------------ grammar */

typedef enum bc_body {
    BC_KEYED,   /* <key> <values...>, or <key> [name] { ... } */
    BC_LIST,    /* fixed-arity positional tuples, identified by position */
    BC_OPEN,    /* any key, exactly one value token or one bracketed group */
    BC_OPAQUE   /* one indivisible record: the native grammar is not established */
} bc_body;

typedef struct bc_scope {
    const char *path;    /* "" is the root body; nested bodies use key paths */
    bc_body body;
    const char *keys;    /* keywords that take at least one value */
    const char *flags;   /* keywords that take no value of their own */
    const char *blocks;  /* keywords that open a nested body */
    const char *named;   /* blocks whose keyword is followed by one name token */
    int arity;           /* BC_LIST: tokens per record */
    int any_block;       /* BC_KEYED: any other name opens a nested "*" body */
    /* "<block> <key>...": an unnamed block identified by the values of those
     * keys in its own body rather than by its position among its peers. */
    const char *keyed;
} bc_scope;

typedef struct bc_grammar {
    const char *family;
    const bc_scope *scopes;
    size_t count;
} bc_grammar;

/* idDeclBreakable::Parse and its block readers. Keys, value counts and nested
 * readers come from the reader's own token comparisons. "all" is a value of
 * pieces rather than a keyword, so it is not listed here. */
static const bc_scope bc_breakable[] = {
    {"", BC_KEYED,
     "model linearFriction angularFriction contactFriction linearFrictionWater "
     "angularFrictionWater bouncyness dampeningDecay gravity worldCollisionOnly "
     "simplePointCollision crazyBounceChance maxSimulationTime stopSpeed "
     "maxLinearVelocity maxAngularVelocity noShadows clipMask impactParticle "
     "deathParticle harmlessParticle", "",
     "pieceNames useablePieces touchingBasePieces explosion trail armoredPieces "
     "healthPieces decal damageSpheres", "", 0, 0},
    {"pieceNames", BC_LIST, "", "", "", "", 2, 0},
    {"useablePieces", BC_LIST, "", "", "", "", 1, 0},
    {"touchingBasePieces", BC_OPAQUE, "", "", "", "", 0, 0},
    {"armoredPieces", BC_OPAQUE, "", "", "", "", 0, 0},
    {"healthPieces", BC_OPAQUE, "", "", "", "", 0, 0},
    {"damageSpheres", BC_OPAQUE, "", "", "", "", 0, 0},
    {"explosion", BC_KEYED,
     "name particle fx position radius impulse falloff angular delay "
     "activateAllDelay duration", "", "excludePieces", "", 0, 0},
    {"explosion/excludePieces", BC_LIST, "", "", "", "", 1, 0},
    {"trail", BC_KEYED,
     "name particleTrail particleBounce particleDie glowQuadMaterial glowQuadSize "
     "fadeInDuration fadeOutDuration minTrailVelocity minBounceVelocity trailSpacing "
     "trailVelocityScale delay duration randomPercentage", "", "pieces", "", 0, 0},
    {"trail/pieces", BC_LIST, "", "", "", "", 1, 0},
    {"decal", BC_KEYED,
     "name maxSizeX maxSizeY minSizeX minSizeY lifetime fadetime fadeInEndTime "
     "decalSpacing minBounceVelocity splatterOnBounce splatterOnRest depth "
     "dripMinSizeX dripMinSizeY floorDeclMinSizeX floorDeclMinSizeY", "",
     "materials dripMaterials ceilingParticles pieces "
     "floorParticleFromCeilingDrip floorDecalFromCeilingDrip", "", 0, 0},
    {"decal/materials", BC_LIST, "", "", "", "", 1, 0},
    {"decal/dripMaterials", BC_LIST, "", "", "", "", 1, 0},
    {"decal/ceilingParticles", BC_LIST, "", "", "", "", 1, 0},
    {"decal/pieces", BC_LIST, "", "", "", "", 1, 0},
    {"decal/floorParticleFromCeilingDrip", BC_LIST, "", "", "", "", 1, 0},
    {"decal/floorDecalFromCeilingDrip", BC_LIST, "", "", "", "", 1, 0},
};

/* idDeclEnv::Parse: an inherit block naming one base declaration, a render-parm
 * block whose names are the engine's own parm declarations, and one
 * allowOverride assignment. */
static const bc_scope bc_env[] = {
    {"", BC_KEYED, "allowOverride", "", "inherit renderParms", "", 0, 0},
    {"inherit", BC_LIST, "", "", "", "", 1, 0},
    {"renderParms", BC_OPEN, "", "", "", "", 0, 0},
};

/* A table declaration carries optional interpolation flags, four range scalars
 * and one sample group that the reader consumes whole. */
static const bc_scope bc_table[] = {
    {"", BC_KEYED, "min max left right", "snap clamp spline noclamp", "", "", 0, 0},
};

/* A render parm declaration is one atomic parm: the reader takes its kind, its
 * value and its modifiers together, so the body is one record rather than a set
 * of independently editable fields. */
static const bc_scope bc_renderparm[] = {
    {"", BC_OPAQUE, "", "", "", "", 0, 0},
};

/* A skin declaration is a set of named channels; each channel body is a list of
 * source/destination material pairs. */
static const bc_scope bc_skins[] = {
    {"", BC_KEYED, "", "", "", "", 0, 1},
    {"*", BC_LIST, "", "", "", "", 2, 0},
};

/* idDeclAF::Parse: a settings block, then named rigid bodies and constraints. */
static const bc_scope bc_af[] = {
    {"", BC_KEYED, "", "", "settings body ballAndSocketJoint universalJoint hinge slider spring fixed",
     "body ballAndSocketJoint universalJoint hinge slider spring fixed", 0, 0},
    {"settings", BC_KEYED,
     "mesh anim model clipMaterial skin defaultBouncyness defaultLinearFriction "
     "defaultAngularFriction defaultContactFriction defaultConstraintFriction "
     "suspendSpeed noMoveTime noMoveTranslation noMoveRotation minMoveTime "
     "maxMoveTime contents clipMask selfCollision base",
     "syncSolverConstants", "solverConstants", "", 0, 0},
    {"settings/solverConstants", BC_KEYED,
     "errorReduction errorReductionMax lcpEpsilon maxLcpEpsilon limitErrorReduction "
     "limitErrorReductionMax limitLcpEpsilon contactErrorReduction "
     "contactErrorReductionMax contactLcpEpsilon universalErrorReduction "
     "universalErrorReductionMax universalTorsionLcpEpsilon minTwistErrorReduction "
     "maxTwistErrorReduction", "", "", "", 0, 0},
    {"body", BC_KEYED,
     "origin model angles joint mod density inertiaScale linearFriction "
     "angularFriction contactFriction contents clipMask selfCollision bouncyness "
     "noSyncCollide clearClipMaskInSolid containedJoints containedjoints "
     "frictionDirection contactMotorDirection", "", "", "", 0, 0},
    {"ballAndSocketJoint", BC_OPAQUE, "", "", "", "", 0, 0},
    {"universalJoint", BC_OPAQUE, "", "", "", "", 0, 0},
    {"hinge", BC_OPAQUE, "", "", "", "", 0, 0},
    {"slider", BC_OPAQUE, "", "", "", "", 0, 0},
    {"spring", BC_OPAQUE, "", "", "", "", 0, 0},
    {"fixed", BC_OPAQUE, "", "", "", "", 0, 0},
};

/* idDeclAnimWeb::Parse and its sub-web, node, blend-tree and edge readers. */
static const bc_scope bc_animweb[] = {
    {"", BC_KEYED, "", "", "props layers states scalars subWebs", "", 0, 0},
    {"props", BC_KEYED,
     "editPos gridSize snapToGrid showGrid alwaysShowIntraSubwebLinks "
     "defaultBlendOutWindow defaultBlendDuration defaultBlendType alwaysShowNodeProps "
     "showAbbreviatedNodeProps showIncomingLinks showAnimName showWrapMode showRate "
     "showDelta showBlendEq showTransition showWeakRefs subWebBlend sourceNode "
     "composite mergeModels", "", "modelInfos models", "", 0, 0},
    {"props/modelInfos", BC_LIST, "", "", "", "", 2, 0},
    {"props/models", BC_LIST, "", "", "", "", 1, 0},
    {"layers", BC_KEYED, "", "", "layer", "layer", 0, 0},
    {"layers/layer", BC_OPAQUE, "", "", "", "", 0, 0},
    {"states", BC_KEYED, "", "", "state", "state", 0, 0},
    {"states/state", BC_KEYED, "destination", "", "", "", 0, 0},
    {"scalars", BC_OPAQUE, "", "", "", "", 0, 0},
    {"subWebs", BC_KEYED, "", "", "subWeb", "subWeb", 0, 0},
    {"subWebs/subWeb", BC_KEYED, "", "", "props node edges subwebs", "node", 0, 0},
    {"subWebs/subWeb/props", BC_KEYED, "color hub visible", "", "", "", 0, 0},
    {"subWebs/subWeb/subwebs", BC_OPAQUE, "", "", "", "", 0, 0},
    {"subWebs/subWeb/edges", BC_OPAQUE, "", "", "", "", 0, 0},
    {"subWebs/subWeb/node", BC_KEYED, "", "", "props blendTrees edges", "", 0, 0},
    {"subWebs/subWeb/node/props", BC_KEYED,
     "delta pos customFlags metaData metaBlock",
     "startTransition transition stopTransition stepTransition turnTransition "
     "genericTransition focusTransition", "", "", 0, 0},
    {"subWebs/subWeb/node/blendTrees", BC_KEYED, "", "", "tree", "", 0, 0},
    {"subWebs/subWeb/node/blendTrees/tree", BC_KEYED,
     "modelIndex blendEq tagGroup tag rate randomStart tags coordinate wrap",
     "", "anims", "", 0, 0},
    {"subWebs/subWeb/node/blendTrees/tree/anims", BC_KEYED, "", "", "alias", "", 0, 0},
    {"subWebs/subWeb/node/blendTrees/tree/anims/alias", BC_KEYED,
     "name md6anim wrap rate randomStart tags", "", "", "", 0, 0},
    /* An edge is the transition to its destination (the engine searches edges
     * for a path to a desired state and reports "could not find path"), so
     * inserting one does not renumber its peers. */
    {"subWebs/subWeb/node/edges", BC_KEYED, "", "", "edge", "", 0, 0, "edge toSubWeb toState"},
    {"subWebs/subWeb/node/edges/edge", BC_KEYED,
     "toState toSubWeb weightScale randomizeWeight customFlags", "", "blendParms", "", 0, 0},
    {"subWebs/subWeb/node/edges/edge/blendParms", BC_KEYED,
     "srcAnim exceptSrcAnim destAnim sourceStartFrame sourceDuration "
     "destStartFrame destDuration duration durationf srcFirstFrame srcLastFrame "
     "destFrame originBlend blendType",
     "sourceEndRelative destEndRelative exceptLooping", "", "", 0, 0},
};

/* A cloth declaration carries simulation scalars plus one clothData block whose
 * per-joint reader is not established here, so that block stays indivisible. */
static const bc_scope bc_cloth[] = {
    {"", BC_KEYED,
     "mass gravity friction springMaxLengthMultiplier collisionSphereRadiusMultiplier "
     "type", "", "clothData", "", 0, 0},
    {"clothData", BC_OPAQUE, "", "", "", "", 0, 0},
};

/* A render-program flag declaration carries one value. */
static const bc_scope bc_renderprogflag[] = {
    {"", BC_LIST, "", "", "", "", 1, 0},
};

/* idDeclDetail::Parse and idDeclFoliage::Parse are flat readers; their keyword
 * sets are the tokens each one compares. Neither family ships a declaration in
 * this installation, so the tables come from the readers alone. */
static const bc_scope bc_detail[] = {
    {"", BC_KEYED, "model colorVariance", "", "", "", 0, 0},
};

static const bc_scope bc_foliage[] = {
    {"", BC_KEYED,
     "quadWidth quadHeight widthVariance heightVariance colorVariance rndFlipHoriz "
     "swayMagnitude material shape autosprites doubles stampMaterial stampScale "
     "stampParms stampCovers stampBlendMode referenceType singlePlacement",
     "", "", "", 0, 0},
};

/* The viseme-set reader's root keywords are established; the internal layout of
 * its viseme and phoneme collections is not, so those bodies stay indivisible
 * rather than being guessed at. */
static const bc_scope bc_visemeset[] = {
    {"", BC_KEYED,
     "phonemeSet visemeSilence phonemeSilence alias weightScale durationScale "
     "timeOffsetMS", "", "visemes phonemes", "", 0, 0},
    {"visemes", BC_OPAQUE, "", "", "", "", 0, 0},
    {"phonemes", BC_OPAQUE, "", "", "", "", 0, 0},
};

static const bc_grammar bc_grammars[] = {
    {"breakable", bc_breakable, sizeof(bc_breakable) / sizeof(bc_breakable[0])},
    {"env", bc_env, sizeof(bc_env) / sizeof(bc_env[0])},
    {"table", bc_table, sizeof(bc_table) / sizeof(bc_table[0])},
    {"renderparm", bc_renderparm, sizeof(bc_renderparm) / sizeof(bc_renderparm[0])},
    {"skins", bc_skins, sizeof(bc_skins) / sizeof(bc_skins[0])},
    {"articulatedfigure", bc_af, sizeof(bc_af) / sizeof(bc_af[0])},
    {"animweb", bc_animweb, sizeof(bc_animweb) / sizeof(bc_animweb[0])},
    {"cloth", bc_cloth, sizeof(bc_cloth) / sizeof(bc_cloth[0])},
    {"renderprogflag", bc_renderprogflag,
     sizeof(bc_renderprogflag) / sizeof(bc_renderprogflag[0])},
    {"detail", bc_detail, sizeof(bc_detail) / sizeof(bc_detail[0])},
    {"foliage", bc_foliage, sizeof(bc_foliage) / sizeof(bc_foliage[0])},
    {"visemeset", bc_visemeset, sizeof(bc_visemeset) / sizeof(bc_visemeset[0])},
};

static const bc_grammar *bc_find_grammar(const char *family)
{
    if (!family) return NULL;
    for (size_t i = 0; i < sizeof(bc_grammars) / sizeof(bc_grammars[0]); i++)
        if (!strcmp(bc_grammars[i].family, family)) return &bc_grammars[i];
    return NULL;
}

int sh_decl_block_family(const char *family) { return bc_find_grammar(family) != NULL; }

/* ---------------------------------------------------------------- utilities */

typedef struct bc_context {
    const bc_grammar *grammar;
    sh_decl_tokens tokens;
    char *error;
    size_t capacity;
    int failed;
} bc_context;

static int bc_fail(bc_context *c, const char *reason)
{
    if (!c->failed && c->error && c->capacity)
        snprintf(c->error, c->capacity, "%s composition: %s", c->grammar->family, reason);
    c->failed = 1;
    return 0;
}

static char *bc_copy(bc_context *c, const char *text, size_t length)
{
    char *out = length < SIZE_MAX ? malloc(length + 1) : NULL;
    if (!out) bc_fail(c, "allocation failed");
    else { memcpy(out, text, length); out[length] = 0; }
    return out;
}

static sh_decl_node *bc_node(bc_context *c, const char *key, const char *value)
{
    sh_decl_node *node = calloc(1, sizeof(*node));
    if (!node) { bc_fail(c, "allocation failed"); return NULL; }
    node->key = key ? bc_copy(c, key, strlen(key)) : NULL;
    node->value = value ? bc_copy(c, value, strlen(value)) : NULL;
    node->compound = value == NULL;
    node->assignment = key != NULL;
    if (c->failed) { sh_decl_tree_free(node); return NULL; }
    return node;
}

static sh_decl_node *bc_add(sh_decl_node *parent, sh_decl_node *child)
{
    sh_decl_node **tail;
    if (!parent) { sh_decl_tree_free(child); return NULL; }
    for (tail = &parent->children; *tail; tail = &(*tail)->next) {}
    *tail = child;
    return child;
}

/* Record bytes travel through the generic merge as hex so that a value keeps
 * the author's exact spelling, including quotes, separators and inner comments. */
static char *bc_hex(bc_context *c, const char *text, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    char *out = length <= (SIZE_MAX - 3) / 2 ? malloc(length * 2 + 3) : NULL;
    if (!out) { bc_fail(c, "record allocation failed"); return NULL; }
    out[0] = '"';
    for (size_t i = 0; i < length; i++) {
        out[1 + i * 2] = digits[(unsigned char)text[i] >> 4];
        out[2 + i * 2] = digits[(unsigned char)text[i] & 15];
    }
    out[length * 2 + 1] = '"';
    out[length * 2 + 2] = 0;
    return out;
}

static int bc_nibble(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static char *bc_unhex(bc_context *c, const sh_decl_node *node, size_t *length)
{
    const char *text; size_t count; char *out;
    if (!node || !sh_decl_tree_literal(node, &text, &count) || count % 2) {
        bc_fail(c, "invalid composed record"); return NULL;
    }
    out = malloc(count / 2 + 1);
    if (!out) { bc_fail(c, "record allocation failed"); return NULL; }
    for (size_t i = 0; i < count; i += 2) {
        int high = bc_nibble(text[i]), low = bc_nibble(text[i + 1]);
        if (high < 0 || low < 0 || !(high | low)) {
            free(out); bc_fail(c, "invalid composed record byte"); return NULL;
        }
        out[i / 2] = (char)((high << 4) | low);
    }
    out[count / 2] = 0;
    if (length) *length = count / 2;
    return out;
}

static const bc_scope *bc_find_scope(const bc_grammar *grammar, const char *path)
{
    for (size_t i = 0; i < grammar->count; i++)
        if (!strcmp(grammar->scopes[i].path, path)) return &grammar->scopes[i];
    return NULL;
}

/* Space-separated vocabulary membership, compared as the native readers do. */
static int bc_listed(const char *list, const char *word, size_t length)
{
    const char *at = list;
    if (!list) return 0;
    while (*at) {
        const char *end = at;
        while (*end && *end != ' ') end++;
        if ((size_t)(end - at) == length && !memcmp(at, word, length)) return 1;
        at = *end ? end + 1 : end;
    }
    return 0;
}

static const char *bc_text(const bc_context *c, size_t index, size_t *length)
{
    const sh_decl_token *token = &c->tokens.items[index];
    *length = token->end - token->begin;
    return c->tokens.source.text + token->begin;
}

/* A keyword position: only a bare name can begin a record. */
static int bc_is_name(const bc_context *c, size_t index)
{
    return c->tokens.items[index].kind == SH_DECL_TOKEN_NAME;
}

/* A value or a record name: anything the lexer did not classify as punctuation. */
static int bc_is_word(const bc_context *c, size_t index)
{
    return c->tokens.items[index].kind != SH_DECL_TOKEN_PUNCTUATION;
}

static int bc_opens(const bc_context *c, size_t index)
{
    size_t length; const char *text = bc_text(c, index, &length);
    return c->tokens.items[index].kind == SH_DECL_TOKEN_PUNCTUATION && length == 1 &&
        (*text == '{' || *text == '(' || *text == '[');
}

static int bc_is_brace(const bc_context *c, size_t index)
{
    size_t length; const char *text = bc_text(c, index, &length);
    return c->tokens.items[index].kind == SH_DECL_TOKEN_PUNCTUATION && length == 1 && *text == '{';
}

/* One value: a bracketed group, a signed number, or a single token. The native
 * lexer reports a leading sign as its own punctuation, and the readers apply it
 * to the number that follows. */
static size_t bc_value_end(const bc_context *c, size_t index, size_t end)
{
    size_t length; const char *text;
    if (bc_opens(c, index)) return c->tokens.items[index].close + 1;
    text = bc_text(c, index, &length);
    if (c->tokens.items[index].kind == SH_DECL_TOKEN_PUNCTUATION && length == 1 &&
        (*text == '-' || *text == '+') && index + 1 < end &&
        c->tokens.items[index + 1].kind == SH_DECL_TOKEN_NUMBER) return index + 2;
    return index + 1;
}

/* A keyword of this scope: the token the native reader would treat as the start
 * of the next record rather than as another value of the current one. */
static int bc_keyword(const bc_context *c, const bc_scope *scope, size_t index)
{
    size_t length; const char *text;
    if (!bc_is_name(c, index)) return 0;
    text = bc_text(c, index, &length);
    return bc_listed(scope->keys, text, length) || bc_listed(scope->flags, text, length) ||
        bc_listed(scope->blocks, text, length);
}

/* A keyed block's identity (bc_scope.keyed): the unquoted values of its
 * identifying keys directly in its own body, joined by '/'. 0 when the block
 * is not keyed in this scope. */
static int bc_keyed_identity(bc_context *c, const bc_scope *scope, const char *key, size_t key_length,
    size_t begin, size_t end, char *out, size_t capacity)
{
    const char *at = scope->keyed;
    size_t used = 0, word = 0;
    int first = 1;
    if (!at) return 0;
    while (at[word] && at[word] != ' ') word++;
    if (word != key_length || memcmp(at, key, key_length)) return 0;
    out[0] = 0;
    for (at += word; *at;) {
        const char *field, *value = "";
        size_t field_length, value_length = 0;
        while (*at == ' ') at++;
        field = at;
        while (*at && *at != ' ') at++;
        field_length = (size_t)(at - field);
        if (!field_length) break;
        for (size_t i = begin; i < end;) {
            size_t n; const char *text = bc_text(c, i, &n);
            if (bc_is_name(c, i) && n == field_length && !memcmp(text, field, n) &&
                i + 1 < end && bc_is_word(c, i + 1)) {
                value = bc_text(c, i + 1, &value_length);
                if (value_length >= 2 && *value == '"') { value++; value_length -= 2; }
                break;
            }
            i = bc_opens(c, i) ? c->tokens.items[i].close + 1 : i + 1;
        }
        if (used + value_length + 2 > capacity) { bc_fail(c, "keyed block identity is too long"); return 0; }
        if (!first) out[used++] = '/';
        memcpy(out + used, value, value_length); used += value_length; out[used] = 0;
        first = 0;
    }
    return 1;
}

/* --------------------------------------------------------------- normalizing */

static int bc_body_records(bc_context *c, const bc_scope *scope, const char *path,
    sh_decl_node *list, size_t begin, size_t end);

static char *bc_identity(bc_context *c, const char *base, size_t base_length,
    const char *name, size_t name_length, size_t occurrence)
{
    size_t need = base_length + name_length + 32;
    char *out = malloc(need);
    if (!out) { bc_fail(c, "identity allocation failed"); return NULL; }
    if (name) snprintf(out, need, "%.*s:%.*s#%zu", (int)base_length, base,
        (int)name_length, name, occurrence);
    else snprintf(out, need, "%.*s#%zu", (int)base_length, base, occurrence);
    return out;
}

/* A record identity stays readable while its bytes are safe inside a native
 * literal, so a diagnostic names the field an author recognizes. A name the
 * native lexer accepted but a literal cannot carry travels hex-encoded behind
 * an "x:" marker, which no readable identity can produce. */
static int bc_safe_identity(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch >= 'a' && ch <= 'z') continue;
        if (ch >= 'A' && ch <= 'Z') continue;
        if (ch >= '0' && ch <= '9') continue;
        if (!strchr("_-./:#* ", ch)) return 0;
    }
    return length > 0;
}

static char *bc_encode_id(bc_context *c, const char *text, size_t length)
{
    char *out;
    if (!bc_safe_identity(text, length)) {
        char *hex = bc_hex(c, text, length), *marked;
        size_t n;
        if (!hex) return NULL;
        n = strlen(hex);
        marked = malloc(n + 3);
        if (!marked) { free(hex); bc_fail(c, "identity allocation failed"); return NULL; }
        marked[0] = '"'; marked[1] = 'x'; marked[2] = ':';
        memcpy(marked + 3, hex + 1, n - 1); marked[n + 2] = 0;
        free(hex);
        return marked;
    }
    out = malloc(length + 3);
    if (!out) { bc_fail(c, "identity allocation failed"); return NULL; }
    out[0] = '"'; memcpy(out + 1, text, length); out[length + 1] = '"'; out[length + 2] = 0;
    return out;
}

/* How many records already in this list carry the given identity prefix. */
static size_t bc_occurrence(bc_context *c, const sh_decl_node *list, const char *prefix)
{
    size_t count = 0, length = strlen(prefix), encoded_length;
    int safe = bc_safe_identity(prefix, length);
    const char *separator = safe ? "#" : "23";
    size_t separator_length = safe ? 1 : 2;
    char *encoded = bc_encode_id(c, prefix, length);
    if (!encoded) return 0;
    encoded_length = strlen(encoded) - 2;
    for (const sh_decl_node *item = list->children; item; item = item->next) {
        const sh_decl_node *id = sh_decl_tree_member(item, "id");
        const char *text; size_t n;
        if (!id || !sh_decl_tree_literal(id, &text, &n)) continue;
        if (n >= encoded_length + separator_length &&
            !memcmp(text, encoded + 1, encoded_length) &&
            !memcmp(text + encoded_length, separator, separator_length)) count++;
    }
    free(encoded);
    return count;
}

static size_t bc_count(const sh_decl_node *list)
{
    size_t count = 0;
    for (const sh_decl_node *item = list->children; item; item = item->next) count++;
    return count;
}

/* The shared merge reads an indexed collection with its own declared extent, so
 * every record list closes with the count the reader produced. */
static int bc_close_list(bc_context *c, sh_decl_node *list)
{
    char number[32];
    snprintf(number, sizeof(number), "%zu", bc_count(list));
    return bc_add(list, bc_node(c, "num", number)) != NULL && !c->failed;
}

/* One record: its identity, the authored bytes it owns, and -- for a nested
 * body -- the records inside it. */
static int bc_record(bc_context *c, sh_decl_node *list, const char *identity,
    size_t slice_begin, size_t slice_end, int nested)
{
    sh_decl_node *item;
    char *encoded, *id;
    char key[64];
    if (slice_begin >= slice_end || slice_end > c->tokens.count)
        return bc_fail(c, "invalid record extent");
    snprintf(key, sizeof(key), "item[%zu]", bc_count(list));
    item = bc_add(list, bc_node(c, key, NULL));
    if (!item) return 0;
    /* The identity travels as a literal so the generic merge compares native
     * record names by their exact bytes. */
    id = bc_encode_id(c, identity, strlen(identity));
    if (!id) return 0;
    if (!bc_add(item, bc_node(c, "id", id))) { free(id); return 0; }
    free(id);
    encoded = bc_hex(c, c->tokens.source.text + c->tokens.items[slice_begin].begin,
        c->tokens.items[slice_end - 1].end - c->tokens.items[slice_begin].begin);
    if (!encoded) return 0;
    if (!bc_add(item, bc_node(c, nested ? "head" : "slice", encoded))) { free(encoded); return 0; }
    free(encoded);
    return !c->failed;
}

static sh_decl_node *bc_last_record(sh_decl_node *list)
{
    sh_decl_node *item = list->children;
    if (!item) return NULL;
    while (item->next) item = item->next;
    return item;
}

static int bc_nested_body(bc_context *c, const char *path, const char *key, size_t key_length,
    int any, sh_decl_node *item, size_t begin, size_t end)
{
    const bc_scope *scope;
    sh_decl_node *body, *records;
    char child[256];
    /* An author-named block, such as a skin channel, resolves through the "*"
     * scope rather than through its own name. */
    if (any) key = "*", key_length = 1;
    if (*path) snprintf(child, sizeof(child), "%s/%.*s", path, (int)key_length, key);
    else snprintf(child, sizeof(child), "%.*s", (int)key_length, key);
    scope = bc_find_scope(c->grammar, child);
    if (!scope) {
        char detail[320];
        snprintf(detail, sizeof(detail), "no verified grammar for the '%s' block", child);
        return bc_fail(c, detail);
    }
    body = bc_add(item, bc_node(c, "body", NULL));
    if (!body) return 0;
    records = bc_add(body, bc_node(c, "records", NULL));
    if (!records) return 0;
    if (scope->body == BC_OPAQUE) {
        /* An empty block keeps an empty record list. */
        if (begin < end && !bc_record(c, records, "payload#0", begin, end, 0)) return 0;
        return bc_close_list(c, records);
    }
    return bc_body_records(c, scope, child, records, begin, end) && bc_close_list(c, records);
}

static int bc_body_records(bc_context *c, const bc_scope *scope, const char *path,
    sh_decl_node *list, size_t begin, size_t end)
{
    size_t at = begin;
    if (scope->body == BC_OPAQUE)
        return begin >= end ? 1 : bc_record(c, list, "payload#0", begin, end, 0);
    if (scope->body == BC_LIST) {
        size_t position = 0;
        while (at < end) {
            size_t stop = at, arity = scope->arity > 0 ? (size_t)scope->arity : 1;
            char identity[64];
            for (size_t taken = 0; taken < arity; taken++) {
                if (stop >= end) return bc_fail(c, "positional record is incomplete");
                stop = bc_value_end(c, stop, end);
            }
            snprintf(identity, sizeof(identity), "entry#%zu", position++);
            if (!bc_record(c, list, identity, at, stop, 0)) return 0;
            at = stop;
        }
        return 1;
    }
    while (at < end) {
        size_t key_length, stop;
        const char *key;
        char *identity;
        if (!bc_is_name(c, at)) {
            if (!bc_opens(c, at)) {
                size_t n; const char *text = bc_text(c, at, &n);
                char detail[288];
                snprintf(detail, sizeof(detail), "unexpected token '%.*s' where a record starts in a native %s body",
                    (int)(n > 60 ? 60 : n), text, *path ? path : "root");
                return bc_fail(c, detail);
            }
            /* A bracketed group standing on its own is one positional record,
             * which is how a table carries its sample list. */
            stop = c->tokens.items[at].close + 1;
            identity = bc_identity(c, "group", 5, NULL, 0, bc_occurrence(c, list, "group"));
            if (!identity) return 0;
            if (!bc_record(c, list, identity, at, stop, 0)) { free(identity); return 0; }
            free(identity); at = stop; continue;
        }
        key = bc_text(c, at, &key_length);
        if (scope->body == BC_OPEN) {
            char prefix[288];
            stop = at + 1;
            if (stop >= end) {
                char detail[288];
                snprintf(detail, sizeof(detail), "key '%.*s' has no value in a native %s body",
                    (int)key_length, key, *path ? path : "root");
                return bc_fail(c, detail);
            }
            stop = bc_value_end(c, stop, end);
            snprintf(prefix, sizeof(prefix), "%.*s", (int)key_length, key);
            identity = bc_identity(c, key, key_length, NULL, 0, bc_occurrence(c, list, prefix));
            if (!identity) return 0;
            if (!bc_record(c, list, identity, at, stop, 0)) { free(identity); return 0; }
            free(identity); at = stop; continue;
        }
        int any = scope->any_block && !bc_listed(scope->blocks, key, key_length) &&
            !bc_listed(scope->keys, key, key_length) && !bc_listed(scope->flags, key, key_length);
        if (any || bc_listed(scope->blocks, key, key_length)) {
            size_t head = at + 1, inner;
            const char *name = NULL; size_t name_length = 0;
            if (bc_listed(scope->named, key, key_length)) {
                if (head >= end || !bc_is_word(c, head))
                    return bc_fail(c, "named block is missing its name");
                name = bc_text(c, head, &name_length);
                if (name_length >= 2 && *name == '"') { name++; name_length -= 2; }
                head++;
            }
            if (head >= end || !bc_is_brace(c, head))
                return bc_fail(c, "block keyword is not followed by a body");
            inner = c->tokens.items[head].close;
            if (inner == SIZE_MAX || inner >= end)
                return bc_fail(c, "block body is not closed inside its parent");
            char keyed[192];
            if (!name && bc_keyed_identity(c, scope, key, key_length, head + 1, inner, keyed, sizeof(keyed))) {
                name = keyed; name_length = strlen(keyed);
            }
            if (c->failed) return 0;
            {
                char prefix[288];
                if (name) snprintf(prefix, sizeof(prefix), "%.*s:%.*s", (int)key_length, key,
                    (int)name_length, name);
                else snprintf(prefix, sizeof(prefix), "%.*s", (int)key_length, key);
                identity = bc_identity(c, key, key_length, name, name_length,
                    bc_occurrence(c, list, prefix));
            }
            if (!identity) return 0;
            if (!bc_record(c, list, identity, at, head + 1, 1)) { free(identity); return 0; }
            free(identity);
            if (!bc_nested_body(c, path, key, key_length, any, bc_last_record(list), head + 1, inner))
                return 0;
            at = inner + 1;
            continue;
        }
        if (!bc_listed(scope->keys, key, key_length) && !bc_listed(scope->flags, key, key_length)) {
            char detail[288];
            snprintf(detail, sizeof(detail), "unsupported key '%.*s' in a native %s body",
                (int)key_length, key, *path ? path : "root");
            return bc_fail(c, detail);
        }
        stop = at + 1;
        if (!bc_listed(scope->flags, key, key_length)) {
            /* A key must own at least one value. A keyword can therefore still
             * be that first value, which is how a body origin names its joint. */
            if (stop >= end || bc_is_brace(c, stop)) {
                char detail[288];
                snprintf(detail, sizeof(detail), "key '%.*s' has no value in a native %s body",
                    (int)key_length, key, *path ? path : "root");
                return bc_fail(c, detail);
            }
            stop = bc_value_end(c, stop, end);
            /* A brace group begins the next record -- a nested body or a
             * positional payload -- so it never continues a value list. */
            while (stop < end && !bc_keyword(c, scope, stop) && !bc_is_brace(c, stop))
                stop = bc_value_end(c, stop, end);
        }
        {
            char prefix[288];
            snprintf(prefix, sizeof(prefix), "%.*s", (int)key_length, key);
            identity = bc_identity(c, key, key_length, NULL, 0, bc_occurrence(c, list, prefix));
        }
        if (!identity) return 0;
        if (!bc_record(c, list, identity, at, stop, 0)) { free(identity); return 0; }
        free(identity);
        at = stop;
    }
    return 1;
}

static char *bc_normalize(bc_context *c, sh_decl_source source, size_t *length)
{
    const bc_scope *root = bc_find_scope(c->grammar, "");
    sh_decl_node *tree = NULL, *edit, *records;
    char *text = NULL;
    size_t end;
    sh_decl_tokens_free(&c->tokens);
    if (!sh_decl_native_lex(source, &c->tokens, c->error, c->capacity)) { c->failed = 1; return NULL; }
    if (!root) { bc_fail(c, "no root grammar"); return NULL; }
    if (!c->tokens.count || !bc_is_brace(c, 0)) { bc_fail(c, "declaration must open with a brace"); return NULL; }
    end = c->tokens.items[0].close;
    if (end != c->tokens.count - 1) { bc_fail(c, "declaration body is not the whole file"); return NULL; }
    tree = bc_node(c, NULL, NULL);
    if (!tree) return NULL;
    edit = bc_add(tree, bc_node(c, "edit", NULL));
    records = edit ? bc_add(edit, bc_node(c, "records", NULL)) : NULL;
    if (!records) { sh_decl_tree_free(tree); return NULL; }
    if (bc_body_records(c, root, "", records, 1, end) && bc_close_list(c, records))
        text = sh_decl_tree_write(tree, length);
    sh_decl_tree_free(tree);
    if (!text && !c->failed) bc_fail(c, "normalized allocation failed");
    return c->failed ? (free(text), NULL) : text;
}

/* ------------------------------------------------------------------ emitting */

typedef struct bc_output { char *text; size_t length, capacity; } bc_output;

static int bc_put(bc_context *c, bc_output *out, const char *text, size_t length)
{
    if (out->length + length + 1 > out->capacity) {
        size_t capacity = out->capacity ? out->capacity * 2 : 4096;
        char *grown;
        while (capacity < out->length + length + 1) capacity *= 2;
        grown = realloc(out->text, capacity);
        if (!grown) return bc_fail(c, "output allocation failed");
        out->text = grown; out->capacity = capacity;
    }
    memcpy(out->text + out->length, text, length);
    out->length += length;
    out->text[out->length] = 0;
    return 1;
}

static int bc_indent(bc_context *c, bc_output *out, size_t depth)
{
    for (size_t i = 0; i < depth; i++) if (!bc_put(c, out, "\t", 1)) return 0;
    return 1;
}

static int bc_emit_records(bc_context *c, bc_output *out, const sh_decl_node *records, size_t depth)
{
    for (const sh_decl_node *item = records ? records->children : NULL; item; item = item->next) {
        const sh_decl_node *slice = sh_decl_tree_member(item, "slice");
        const sh_decl_node *head = sh_decl_tree_member(item, "head");
        const sh_decl_node *body = sh_decl_tree_member(item, "body");
        char *bytes; size_t length;
        if (!strcmp(item->key, "num")) continue;   /* the collection extent */
        if (!slice && !head) return bc_fail(c, "composed record lost its authored bytes");
        bytes = bc_unhex(c, slice ? slice : head, &length);
        if (!bytes) return 0;
        if (!bc_indent(c, out, depth) || !bc_put(c, out, bytes, length)) { free(bytes); return 0; }
        free(bytes);
        if (head) {
            const sh_decl_node *inner = body ? sh_decl_tree_member(body, "records") : NULL;
            if (!bc_put(c, out, "\n", 1)) return 0;
            if (!bc_emit_records(c, out, inner, depth + 1)) return 0;
            if (!bc_indent(c, out, depth) || !bc_put(c, out, "}\n", 2)) return 0;
            continue;
        }
        if (!bc_put(c, out, "\n", 1)) return 0;
    }
    return 1;
}

static char *bc_emit(bc_context *c, const sh_decl_node *tree, size_t *length)
{
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const sh_decl_node *records = edit ? sh_decl_tree_member(edit, "records") : NULL;
    bc_output out = {0};
    if (!records) { bc_fail(c, "composed declaration lost its record list"); return NULL; }
    if (!bc_put(c, &out, "{\n", 2) || !bc_emit_records(c, &out, records, 1) ||
        !bc_put(c, &out, "}\n", 2)) { free(out.text); return NULL; }
    if (length) *length = out.length;
    return out.text;
}

/* ------------------------------------------------------------------- compose */

char *sh_decl_block_compose(const char *family, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict)
{
    /* Records and every nested record list merge by native identity. The depth
     * covers the deepest registered block grammar. */
    static const sh_decl_collection_rule rules[] = {
        {"edit.records", "id"},
        {"edit.records.item[*].body.records", "id"},
        {"edit.records.item[*].body.records.item[*].body.records", "id"},
        {"edit.records.item[*].body.records.item[*].body.records.item[*].body.records", "id"},
        {"edit.records.item[*].body.records.item[*].body.records.item[*].body.records"
         ".item[*].body.records", "id"},
        {"edit.records.item[*].body.records.item[*].body.records.item[*].body.records"
         ".item[*].body.records.item[*].body.records", "id"},
        {"edit.records.item[*].body.records.item[*].body.records.item[*].body.records"
         ".item[*].body.records.item[*].body.records.item[*].body.records", "id"},
        {"edit.records.item[*].body.records.item[*].body.records.item[*].body.records"
         ".item[*].body.records.item[*].body.records.item[*].body.records"
         ".item[*].body.records", "id"},
    };
    bc_context c;
    sh_decl_source *views = NULL;
    char **normalized = NULL, *merged = NULL, *out = NULL;
    sh_decl_node *tree = NULL;
    size_t n = 0;
    memset(&c, 0, sizeof(c));
    c.error = error; c.capacity = capacity;
    if (length) *length = 0;
    if (error && capacity) *error = 0;
    if (conflict) *conflict = (sh_decl_conflict){SIZE_MAX, SIZE_MAX};
    c.grammar = bc_find_grammar(family);
    if (!c.grammar) {
        if (error && capacity) snprintf(error, capacity, "no verified block grammar for '%s'",
            family ? family : "");
        return NULL;
    }
    if ((!sources && count) || count > SIZE_MAX / sizeof(*views) - 1) {
        bc_fail(&c, "invalid input count"); return NULL;
    }
    views = calloc(count + 1, sizeof(*views));
    normalized = calloc(count + 1, sizeof(*normalized));
    if (!views || !normalized) { bc_fail(&c, "allocation failed"); goto done; }
    normalized[count] = bc_normalize(&c, baseline, &n);
    if (!normalized[count]) goto done;
    for (size_t i = 0; i < count; i++) {
        size_t size = 0;
        normalized[i] = bc_normalize(&c, sources[i], &size);
        if (!normalized[i]) { if (conflict) conflict->first = i; goto done; }
        views[i] = (sh_decl_source){normalized[i], size};
    }
    merged = sh_decl_compose((sh_decl_source){normalized[count], n}, views, count,
        rules, sizeof(rules) / sizeof(rules[0]), &n, error, capacity, conflict);
    if (!merged) { c.failed = 1; goto done; }
    tree = sh_decl_tree_parse((sh_decl_source){merged, n}, error, capacity);
    if (!tree) { c.failed = 1; goto done; }
    out = bc_emit(&c, tree, &n);
    if (out && length) *length = n;
done:
    if (c.failed) { free(out); out = NULL; }
    sh_decl_tree_free(tree);
    sh_decl_tokens_free(&c.tokens);
    free(merged);
    if (normalized) for (size_t i = 0; i <= count; i++) free(normalized[i]);
    free(normalized); free(views);
    return out;
}

/* ------------------------------------------------------ player hands web */

/* The player's hands web is one global registry: a weapon names its subweb
 * (subweb_normal), its hit-reaction subweb and its states, and the hands
 * animator resolves each by exact name in that one web (Vulkan FUN_140cbd0c0,
 * FUN_1402652d0, FUN_140265170). Contributions are whole copies of the web, so
 * before they compose each is aligned against the stock original here. */

typedef struct aw_block { size_t key, open, close; const char *name; size_t name_length; } aw_block;
typedef struct aw_web {
    sh_decl_tokens tokens;
    size_t props_open, props_close, models_open, models_close;
    size_t subwebs_open, subwebs_close, states_open, states_close, layers_open, layers_close;
    aw_block *subwebs, *states;
    size_t subweb_count, state_count;
    size_t *models, model_count;      /* token of each modelInfos entry's model */
    size_t *map;                      /* entry position -> aligned position */
} aw_web;
typedef struct aw_edit { size_t begin, end, order; char *text; size_t length; } aw_edit;
typedef struct aw_edits { aw_edit *items; size_t count, capacity; } aw_edits;

static int aw_fail(char *error, size_t capacity, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "player hands web: %s", reason);
    return 0;
}
static const char *aw_text(const aw_web *w, size_t i, size_t *n)
{
    const sh_decl_token *t = w->tokens.items + i;
    *n = t->end - t->begin; return w->tokens.source.text + t->begin;
}
static int aw_is(const aw_web *w, size_t i, const char *text) { return sh_decl_token_is(&w->tokens, i, text); }
static int aw_opens(const aw_web *w, size_t i)
{
    size_t n; const char *text = aw_text(w, i, &n);
    return w->tokens.items[i].kind == SH_DECL_TOKEN_PUNCTUATION && n == 1 &&
        (*text == '{' || *text == '(' || *text == '[');
}
static size_t aw_skip(const aw_web *w, size_t i) { return aw_opens(w, i) ? w->tokens.items[i].close + 1 : i + 1; }
static int aw_word(const aw_web *w, size_t i)
{ return i < w->tokens.count && w->tokens.items[i].kind != SH_DECL_TOKEN_PUNCTUATION; }
static void aw_unquote(const char **text, size_t *n) { if (*n >= 2 && **text == '"') { (*text)++; *n -= 2; } }
/* Token i spells this unquoted name. */
static int aw_names(const aw_web *w, size_t i, const char *name, size_t length)
{
    size_t n; const char *text;
    if (!aw_word(w, i)) return 0;
    text = aw_text(w, i, &n); aw_unquote(&text, &n);
    return n == length && !memcmp(text, name, length);
}
/* Key followed by a braced body at depth 0 of (open, close); SIZE_MAX absent. */
static size_t aw_find(const aw_web *w, size_t open, size_t close, const char *key)
{
    for (size_t i = open + 1; i < close;) {
        if (aw_is(w, i, key) && i + 1 < close && aw_is(w, i + 1, "{")) return i;
        i = aw_skip(w, i);
    }
    return SIZE_MAX;
}
static int aw_named(aw_web *w, size_t open, size_t close, const char *key, aw_block **out, size_t *count)
{
    size_t capacity = 0;
    if (open == SIZE_MAX) return 1;
    for (size_t i = open + 1; i < close;) {
        if (aw_is(w, i, key) && i + 2 < close && aw_word(w, i + 1) && aw_is(w, i + 2, "{")) {
            aw_block *grown = *out;
            if (*count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                if (!(grown = realloc(*out, capacity * sizeof(*grown)))) return 0;
                *out = grown;
            }
            aw_block *b = *out + (*count)++;
            b->key = i; b->open = i + 2; b->close = w->tokens.items[i + 2].close;
            b->name = aw_text(w, i + 1, &b->name_length); aw_unquote(&b->name, &b->name_length);
            i = b->close + 1; continue;
        }
        i = aw_skip(w, i);
    }
    return 1;
}
static void aw_free(aw_web *w)
{
    sh_decl_tokens_free(&w->tokens); free(w->subwebs); free(w->states); free(w->models); free(w->map);
    memset(w, 0, sizeof(*w));
}
static int aw_parse(aw_web *w, sh_decl_source source, char *error, size_t capacity)
{
    size_t count, found;
    memset(w, 0, sizeof(*w));
    w->props_open = w->props_close = w->models_open = w->models_close = SIZE_MAX;
    w->subwebs_open = w->subwebs_close = w->states_open = w->states_close = SIZE_MAX;
    w->layers_open = w->layers_close = SIZE_MAX;
    if (!sh_decl_native_lex(source, &w->tokens, error, capacity)) return 0;
    count = w->tokens.count;
    if (!count || !aw_is(w, 0, "{") || w->tokens.items[0].close != count - 1)
        return aw_fail(error, capacity, "the web must be one braced body");
    if ((found = aw_find(w, 0, count - 1, "props")) != SIZE_MAX) {
        w->props_open = found + 1; w->props_close = w->tokens.items[found + 1].close;
        if ((found = aw_find(w, w->props_open, w->props_close, "modelInfos")) != SIZE_MAX) {
            w->models_open = found + 1; w->models_close = w->tokens.items[found + 1].close;
        }
    }
    if ((found = aw_find(w, 0, count - 1, "subWebs")) != SIZE_MAX) {
        w->subwebs_open = found + 1; w->subwebs_close = w->tokens.items[found + 1].close;
    }
    if ((found = aw_find(w, 0, count - 1, "states")) != SIZE_MAX) {
        w->states_open = found + 1; w->states_close = w->tokens.items[found + 1].close;
    }
    if ((found = aw_find(w, 0, count - 1, "layers")) != SIZE_MAX) {
        w->layers_open = found + 1; w->layers_close = w->tokens.items[found + 1].close;
    }
    if (w->models_open != SIZE_MAX) {
        size_t capacity_models = 0;
        for (size_t i = w->models_open + 1; i < w->models_close;) {
            if (w->model_count == capacity_models) {
                size_t *grown;
                capacity_models = capacity_models ? capacity_models * 2 : 32;
                if (!(grown = realloc(w->models, capacity_models * sizeof(*grown)))) return aw_fail(error, capacity, "allocation failed");
                w->models = grown;
            }
            w->models[w->model_count++] = i;
            i = aw_skip(w, i);
            if (i < w->models_close) i = aw_skip(w, i);
        }
    }
    if (!aw_named(w, w->subwebs_open, w->subwebs_close, "subWeb", &w->subwebs, &w->subweb_count) ||
        !aw_named(w, w->states_open, w->states_close, "state", &w->states, &w->state_count))
        return aw_fail(error, capacity, "allocation failed");
    if (!(w->map = calloc(w->model_count ? w->model_count : 1, sizeof(*w->map))))
        return aw_fail(error, capacity, "allocation failed");
    for (size_t i = 0; i < w->model_count; i++) w->map[i] = i;
    return 1;
}
static const aw_block *aw_block_named(const aw_block *blocks, size_t count, const char *name, size_t length)
{
    for (size_t i = 0; i < count; i++)
        if (blocks[i].name_length == length && !memcmp(blocks[i].name, name, length)) return blocks + i;
    return NULL;
}

static int aw_add_edit(aw_edits *e, size_t begin, size_t end, const char *text, size_t length)
{
    if (e->count == e->capacity) {
        size_t capacity = e->capacity ? e->capacity * 2 : 64;
        aw_edit *grown = realloc(e->items, capacity * sizeof(*grown));
        if (!grown) return 0;
        e->items = grown; e->capacity = capacity;
    }
    aw_edit *item = e->items + e->count;
    if (!(item->text = malloc(length + 1))) return 0;
    memcpy(item->text, text, length); item->text[length] = 0;
    item->begin = begin; item->end = end; item->length = length; item->order = e->count++;
    return 1;
}
static void aw_edits_free(aw_edits *e)
{
    for (size_t i = 0; i < e->count; i++) free(e->items[i].text);
    free(e->items); memset(e, 0, sizeof(*e));
}
static int aw_edit_order(const void *left, const void *right)
{
    const aw_edit *a = left, *b = right;
    if (a->begin != b->begin) return a->begin < b->begin ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}
/* Apply edits to a source slice [from, to); NULL on overlap or allocation. */
static char *aw_apply(const char *source, size_t from, size_t to, aw_edits *e, size_t *length)
{
    size_t total = to - from, at = from, used = 0;
    char *out;
    qsort(e->items, e->count, sizeof(*e->items), aw_edit_order);
    for (size_t i = 0; i < e->count; i++) {
        if (e->items[i].begin < at || e->items[i].end > to) return NULL;
        at = e->items[i].end;
        total += e->items[i].length;
    }
    if (!(out = malloc(total + 1))) return NULL;
    at = from;
    for (size_t i = 0; i < e->count; i++) {
        memcpy(out + used, source + at, e->items[i].begin - at); used += e->items[i].begin - at;
        memcpy(out + used, e->items[i].text, e->items[i].length); used += e->items[i].length;
        at = e->items[i].end;
    }
    memcpy(out + used, source + at, to - at); used += to - at;
    out[used] = 0; *length = used; return out;
}

/* An edge whose destination subweb the stock web lacks: the transition into a
 * contribution's own new weapon, which joins every web it is written in. */
static int aw_glue(const aw_web *w, size_t open, const aw_web *base)
{
    for (size_t i = open + 1; i < w->tokens.items[open].close;) {
        if (aw_is(w, i, "toSubWeb") && aw_word(w, i + 1)) {
            size_t n; const char *name = aw_text(w, i + 1, &n); aw_unquote(&name, &n);
            return !aw_block_named(base->subwebs, base->subweb_count, name, n);
        }
        i = aw_skip(w, i);
    }
    return 0;
}
/* Next token of a subweb walk: skips editor-only placement (node pos, subweb
 * color) and, when base is given, glue edges. */
static size_t aw_walk(const aw_web *w, size_t i, size_t end, const aw_web *base)
{
    while (i < end) {
        if ((aw_is(w, i, "pos") || aw_is(w, i, "color")) && i + 1 < end && aw_is(w, i + 1, "(")) {
            i = w->tokens.items[i + 1].close + 1; continue;
        }
        if (base && aw_is(w, i, "edge") && i + 1 < end && aw_is(w, i + 1, "{") && aw_glue(w, i + 1, base)) {
            i = w->tokens.items[i + 1].close + 1; continue;
        }
        break;
    }
    return i;
}
/* The value a token carries after alignment: a model index maps through its
 * web's model table. */
static int aw_token_same(const aw_web *a, size_t i, const aw_web *b, size_t j)
{
    size_t n, m; const char *x = aw_text(a, i, &n), *y = aw_text(b, j, &m);
    if (i && j && aw_is(a, i - 1, "modelIndex") && aw_is(b, j - 1, "modelIndex") &&
        a->tokens.items[i].kind == SH_DECL_TOKEN_NUMBER && b->tokens.items[j].kind == SH_DECL_TOKEN_NUMBER) {
        size_t p = (size_t)strtoul(x, NULL, 10), q = (size_t)strtoul(y, NULL, 10);
        if (p < a->model_count) p = a->map[p];
        if (q < b->model_count) q = b->map[q];
        return p == q;
    }
    return n == m && !memcmp(x, y, n);
}
/* Two subweb blocks bind the same behavior (names aside). glue_base, when
 * given, lets a's glue edges into new subwebs stand beside b's records. */
static int aw_same_subweb(const aw_web *a, const aw_block *x, const aw_web *b, const aw_block *y, const aw_web *glue_base)
{
    size_t i = aw_walk(a, x->open, x->close + 1, glue_base), j = aw_walk(b, y->open, y->close + 1, NULL);
    while (i <= x->close && j <= y->close) {
        if (!aw_token_same(a, i, b, j)) return 0;
        i = aw_walk(a, i + 1, x->close + 1, glue_base); j = aw_walk(b, j + 1, y->close + 1, NULL);
    }
    return i > x->close && j > y->close;
}
static uint64_t aw_hash_subweb(const aw_web *w, const aw_block *x)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = aw_walk(w, x->open, x->close + 1, NULL); i <= x->close; i = aw_walk(w, i + 1, x->close + 1, NULL)) {
        size_t n; const char *text = aw_text(w, i, &n);
        char number[32];
        if (i && aw_is(w, i - 1, "modelIndex") && w->tokens.items[i].kind == SH_DECL_TOKEN_NUMBER) {
            size_t p = (size_t)strtoul(text, NULL, 10);
            snprintf(number, sizeof(number), "%zu", p < w->model_count ? w->map[p] : p);
            text = number; n = strlen(number);
        }
        for (size_t k = 0; k < n; k++) { hash ^= (unsigned char)text[k]; hash *= 1099511628211ull; }
        hash ^= 0x1f; hash *= 1099511628211ull;
    }
    return hash;
}
/* Whitespace that leads token i's line, for inserted siblings. */
static size_t aw_indent(const aw_web *w, size_t i)
{
    const char *text = w->tokens.source.text;
    size_t at = w->tokens.items[i].begin, begin = at;
    while (begin && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) begin--;
    return at - begin;
}

typedef struct aw_move { size_t source; char *from, *to; uint64_t hash; } aw_move;
typedef struct aw_state {
    const aw_web *base;
    aw_web *webs;
    size_t count;
    aw_edits *edits;
    aw_move *moves;
    size_t move_count;
    char **merged; size_t *merged_lengths, merged_count; /* aligned modelInfos entries */
    char *error; size_t capacity;
} aw_state;

/* One relocation of subweb x in web s to `to`: its own edges follow it, each
 * other edge entering it gains a twin entering the copy, and a layer naming it
 * names the copy too. */
static int aw_relocate(aw_state *st, size_t s, const aw_block *x, const char *to, uint64_t hash)
{
    const aw_web *w = st->webs + s;
    aw_edits *e = st->edits + s;
    size_t to_length = strlen(to);
    char quoted[512];
    snprintf(quoted, sizeof(quoted), "\"%s\"", to);
    if (!aw_add_edit(e, w->tokens.items[x->key + 1].begin, w->tokens.items[x->key + 1].end, quoted, to_length + 2)) return 0;
    for (size_t i = x->open; i < x->close; i++)
        if (aw_is(w, i, "toSubWeb") && aw_names(w, i + 1, x->name, x->name_length) &&
            !aw_add_edit(e, w->tokens.items[i + 1].begin, w->tokens.items[i + 1].end, quoted, to_length + 2)) return 0;
    for (size_t i = w->subwebs_open + 1; i < w->subwebs_close; i++) {
        if (i >= x->key && i <= x->close) continue;
        if (!aw_is(w, i, "edge") || !aw_is(w, i + 1, "{")) continue;
        size_t close = w->tokens.items[i + 1].close, value = SIZE_MAX;
        for (size_t k = i + 2; k < close;) {
            if (aw_is(w, k, "toSubWeb") && aw_names(w, k + 1, x->name, x->name_length)) { value = k + 1; break; }
            k = aw_skip(w, k);
        }
        if (value == SIZE_MAX) continue;
        aw_edits twin = {0};
        size_t length = 0, indent = aw_indent(w, i);
        char *copy;
        if (!aw_add_edit(&twin, w->tokens.items[value].begin, w->tokens.items[value].end, quoted, to_length + 2) ||
            !(copy = aw_apply(w->tokens.source.text, w->tokens.items[i].begin - indent, w->tokens.items[close].end, &twin, &length))) {
            aw_edits_free(&twin); return 0;
        }
        aw_edits_free(&twin);
        char *line = malloc(length + 2);
        if (!line) { free(copy); return 0; }
        line[0] = '\n'; memcpy(line + 1, copy, length); free(copy);
        int ok = aw_add_edit(e, w->tokens.items[close].end, w->tokens.items[close].end, line, length + 1);
        free(line);
        if (!ok) return 0;
    }
    if (w->layers_open != SIZE_MAX) {
        char spaced[520];
        snprintf(spaced, sizeof(spaced), " %s", quoted);
        for (size_t i = w->layers_open + 1; i < w->layers_close; i++)
            if (w->tokens.items[i].kind == SH_DECL_TOKEN_STRING && aw_names(w, i, x->name, x->name_length) &&
                !aw_add_edit(e, w->tokens.items[i].end, w->tokens.items[i].end, spaced, strlen(spaced))) return 0;
    }
    aw_move *grown = realloc(st->moves, (st->move_count + 1) * sizeof(*grown));
    if (!grown) return 0;
    st->moves = grown;
    aw_move *m = st->moves + st->move_count;
    m->source = s; m->hash = hash; m->from = malloc(x->name_length + 1); m->to = malloc(to_length + 1);
    if (!m->from || !m->to) { free(m->from); free(m->to); return 0; }
    memcpy(m->from, x->name, x->name_length); m->from[x->name_length] = 0; memcpy(m->to, to, to_length + 1);
    st->move_count++;
    return 1;
}
/* The stock subweb as this contribution sees it: the original block with the
 * contribution's glue edges placed in the same nodes. */
static char *aw_stock_with_glue(const aw_state *st, size_t s, const aw_block *x, const aw_block *stock, size_t *length)
{
    const aw_web *w = st->webs + s, *base = st->base;
    aw_edits e = {0};
    const char *node = NULL; size_t node_length = 0, node_close = 0;
    char *out;
    for (size_t i = x->open + 1; i < x->close; i++) {
        if (aw_is(w, i, "node") && aw_word(w, i + 1) && aw_is(w, i + 2, "{")) {
            node = aw_text(w, i + 1, &node_length); aw_unquote(&node, &node_length);
            node_close = w->tokens.items[i + 2].close;
        }
        if (node && i > node_close) node = NULL;
        if (!node || !aw_is(w, i, "edge") || !aw_is(w, i + 1, "{") || !aw_glue(w, i + 1, base)) continue;
        /* The same node of the stock subweb receives the edge beside its peers. */
        size_t target = SIZE_MAX, edges = SIZE_MAX, close = w->tokens.items[i + 1].close, indent = aw_indent(w, i);
        for (size_t k = stock->open + 1; k < stock->close;) {
            if (aw_is(base, k, "node") && aw_names(base, k + 1, node, node_length) && aw_is(base, k + 2, "{")) { target = k + 2; break; }
            k = aw_skip(base, k);
        }
        if (target == SIZE_MAX) { i = close; continue; } /* a node only this contribution has */
        edges = aw_find(base, target, base->tokens.items[target].close, "edges");
        const char *text = w->tokens.source.text + w->tokens.items[i].begin - indent;
        size_t n = w->tokens.items[close].end - w->tokens.items[i].begin + indent;
        char *line = malloc(n + 16);
        if (!line) { aw_edits_free(&e); return NULL; }
        size_t at;
        if (edges != SIZE_MAX) {
            line[0] = '\n'; memcpy(line + 1, text, n);
            at = base->tokens.items[base->tokens.items[edges + 1].close].begin;
            if (!aw_add_edit(&e, at, at, line, n + 1)) { free(line); aw_edits_free(&e); return NULL; }
        } else {
            static const char head[] = "\nedges {\n", tail[] = "\n}\n";
            char *block = malloc(n + sizeof(head) + sizeof(tail));
            if (!block) { free(line); aw_edits_free(&e); return NULL; }
            memcpy(block, head, sizeof(head) - 1); memcpy(block + sizeof(head) - 1, text, n);
            memcpy(block + sizeof(head) - 1 + n, tail, sizeof(tail));
            at = base->tokens.items[base->tokens.items[target].close].begin;
            int ok = aw_add_edit(&e, at, at, block, strlen(block));
            free(block);
            if (!ok) { free(line); aw_edits_free(&e); return NULL; }
        }
        free(line);
        i = close;
    }
    out = aw_apply(base->tokens.source.text, base->tokens.items[stock->key].begin,
        base->tokens.items[stock->close].end, &e, length);
    aw_edits_free(&e); return out;
}
static int aw_insert(aw_edits *e, const aw_web *w, size_t close, const char *text, size_t length)
{
    size_t at = w->tokens.items[close].begin;
    char *line = malloc(length + 2);
    if (!line) return 0;
    line[0] = '\n'; memcpy(line + 1, text, length);
    int ok = aw_add_edit(e, at, at, line, length + 1);
    free(line); return ok;
}
/* Insert a block before token `key`, which starts a line at its indentation,
 * so the inserted block takes that position among its siblings. */
static int aw_insert_before(aw_edits *e, const aw_web *w, size_t key, const char *text, size_t length)
{
    size_t indent = aw_indent(w, key), at = w->tokens.items[key].begin;
    char *line = malloc(length + indent + 2);
    if (!line) return 0;
    memcpy(line, text, length); line[length] = '\n';
    memcpy(line + length + 1, w->tokens.source.text + at - indent, indent);
    int ok = aw_add_edit(e, at, at, line, length + indent + 1);
    free(line); return ok;
}
/* Restore a stock block this contribution lacks where the stock order puts
 * it: before the next stock block the contribution has, else at the end. */
static int aw_restore(aw_edits *e, const aw_web *w, const aw_block *own, size_t own_count, size_t close,
    const aw_web *base, const aw_block *stock, size_t stock_count, size_t index)
{
    const char *text = base->tokens.source.text + base->tokens.items[stock[index].key].begin;
    size_t length = base->tokens.items[stock[index].close].end - base->tokens.items[stock[index].key].begin;
    for (size_t next = index + 1; next < stock_count; next++) {
        const aw_block *mine = aw_block_named(own, own_count, stock[next].name, stock[next].name_length);
        if (mine) return aw_insert_before(e, w, mine->key, text, length);
    }
    return aw_insert(e, w, close, text, length);
}
/* A unique name for a contribution's own copy of subweb `name`. */
static int aw_relocated_name(const aw_state *st, const char *name, size_t length, uint64_t hash, char *out, size_t capacity)
{
    for (int digits = 8; digits <= 16; digits += 8) {
        int taken = 0;
        snprintf(out, capacity, "%.*s_%0*llx", (int)length, name, digits,
            (unsigned long long)(digits == 8 ? hash >> 32 : hash));
        size_t n = strlen(out);
        taken |= aw_block_named(st->base->subwebs, st->base->subweb_count, out, n) != NULL;
        for (size_t s = 0; s < st->count && !taken; s++)
            taken |= aw_block_named(st->webs[s].subwebs, st->webs[s].subweb_count, out, n) != NULL;
        for (size_t m = 0; m < st->move_count && !taken; m++)
            taken |= !strcmp(st->moves[m].to, out) && (strlen(st->moves[m].from) != length ||
                memcmp(st->moves[m].from, name, length) || st->moves[m].hash != hash);
        if (!taken) return 1;
    }
    return 0;
}

int sh_decl_animweb_prepare(sh_decl_source baseline, sh_decl_source *sources, size_t count,
    char **owned, sh_decl_animweb_move **moves, size_t *move_count, char *error, size_t capacity)
{
    static const char *editor_keys[] = {"editPos", "gridSize", "snapToGrid", "showGrid",
        "alwaysShowIntraSubwebLinks", "alwaysShowNodeProps", "showAbbreviatedNodeProps",
        "showIncomingLinks", "showAnimName", "showWrapMode", "showRate", "showDelta",
        "showBlendEq", "showTransition", "showWeakRefs"};
    aw_web base;
    aw_state st = {0};
    size_t merged_capacity = 0, *model_length = NULL, *label_length = NULL;
    const char **model_text = NULL, **label_text = NULL;
    int ok = 0;
    if (moves) *moves = NULL;
    if (move_count) *move_count = 0;
    if (error && capacity) *error = 0;
    for (size_t i = 0; i < count; i++) owned[i] = NULL;
    if (!aw_parse(&base, baseline, error, capacity)) { aw_free(&base); return 0; }
    st.base = &base; st.count = count; st.error = error; st.capacity = capacity;
    st.webs = calloc(count ? count : 1, sizeof(*st.webs));
    st.edits = calloc(count ? count : 1, sizeof(*st.edits));
    if (!st.webs || !st.edits) { aw_fail(error, capacity, "allocation failed"); goto done; }
    for (size_t s = 0; s < count; s++)
        if (!aw_parse(st.webs + s, sources[s], error, capacity)) goto done;

    /* Models: the stock list, then each contribution's additions in order;
     * every model index follows its entry. */
    for (size_t pass = 0; pass <= count; pass++) {
        aw_web *w = pass ? st.webs + pass - 1 : &base;
        for (size_t k = 0; k < w->model_count; k++) {
            size_t n, lab = 0; const char *text = aw_text(w, w->models[k], &n), *label = "";
            size_t next = aw_skip(w, w->models[k]);
            if (next < w->models_close) label = aw_text(w, next, &lab);
            size_t found;
            for (found = 0; found < st.merged_count; found++)
                if (model_length[found] == n && !memcmp(model_text[found], text, n)) break;
            if (found == st.merged_count) {
                if (st.merged_count == merged_capacity) {
                    merged_capacity = merged_capacity ? merged_capacity * 2 : 64;
                    const char **mt = realloc((void *)model_text, merged_capacity * sizeof(*mt));
                    if (mt) model_text = mt;
                    const char **lt = realloc((void *)label_text, merged_capacity * sizeof(*lt));
                    if (lt) label_text = lt;
                    size_t *ml = realloc(model_length, merged_capacity * sizeof(*ml));
                    if (ml) model_length = ml;
                    size_t *ll = realloc(label_length, merged_capacity * sizeof(*ll));
                    if (ll) label_length = ll;
                    if (!mt || !lt || !ml || !ll) { aw_fail(error, capacity, "allocation failed"); goto models_done; }
                }
                model_text[found] = text; model_length[found] = n;
                label_text[found] = label; label_length[found] = lab;
                st.merged_count++;
            }
            w->map[k] = found;
        }
    }
    for (size_t s = 0; s < count; s++) {
        aw_web *w = st.webs + s;
        int same = w->model_count == st.merged_count;
        for (size_t k = 0; same && k < w->model_count; k++) same = w->map[k] == k;
        if (!same && st.merged_count) {
            size_t total = 16;
            for (size_t k = 0; k < st.merged_count; k++) total += model_length[k] + label_length[k] + 8;
            char *list = malloc(total + 64), *at = list, *limit = list + total + 64;
            if (!list) { aw_fail(error, capacity, "allocation failed"); goto models_done; }
            if (w->models_open == SIZE_MAX) at += snprintf(at, (size_t)(limit - at), "\n\t\tmodelInfos {");
            for (size_t k = 0; k < st.merged_count; k++) {
                at += snprintf(at, (size_t)(limit - at), "\n\t\t\t%.*s %.*s", (int)model_length[k], model_text[k],
                    (int)label_length[k], label_text[k]);
            }
            at += snprintf(at, (size_t)(limit - at), "\n\t\t");
            if (w->models_open == SIZE_MAX) at += snprintf(at, (size_t)(limit - at), "}\n\t");
            int placed;
            if (w->models_open != SIZE_MAX)
                placed = aw_add_edit(st.edits + s, w->tokens.items[w->models_open].end,
                    w->tokens.items[w->models_close].begin, list, (size_t)(at - list));
            else if (w->props_open != SIZE_MAX)
                placed = aw_add_edit(st.edits + s, w->tokens.items[w->props_close].begin,
                    w->tokens.items[w->props_close].begin, list, (size_t)(at - list));
            else placed = 1; /* no props at all: the stock props compose unchanged */
            free(list);
            if (!placed) { aw_fail(error, capacity, "allocation failed"); goto models_done; }
        }
        for (size_t i = 0; i + 1 < w->tokens.count; i++) {
            if (!aw_is(w, i, "modelIndex") || w->tokens.items[i + 1].kind != SH_DECL_TOKEN_NUMBER) continue;
            size_t n; const char *text = aw_text(w, i + 1, &n);
            size_t p = (size_t)strtoul(text, NULL, 10);
            if (p >= w->model_count || w->map[p] == p) continue;
            char number[32]; int written = snprintf(number, sizeof(number), "%zu", w->map[p]);
            if (!aw_add_edit(st.edits + s, w->tokens.items[i + 1].begin, w->tokens.items[i + 1].end, number, (size_t)written)) {
                aw_fail(error, capacity, "allocation failed"); goto models_done;
            }
        }
        /* Editor view settings are not behavior: every contribution keeps the
         * stock values, so authors' different views never collide. */
        for (size_t i = w->props_open == SIZE_MAX ? SIZE_MAX : w->props_open + 1;
             w->props_open != SIZE_MAX && i < w->props_close; i = aw_skip(w, i)) {
            for (size_t k = 0; k < sizeof(editor_keys) / sizeof(editor_keys[0]); k++) {
                if (!aw_is(w, i, editor_keys[k]) || i + 1 >= w->props_close) continue;
                size_t at = base.props_open == SIZE_MAX ? SIZE_MAX : base.props_open + 1;
                for (; at != SIZE_MAX && at < base.props_close; at = aw_skip(&base, at))
                    if (aw_is(&base, at, editor_keys[k]) && at + 1 < base.props_close) break;
                if (at == SIZE_MAX || at >= base.props_close) continue;
                size_t value_end = aw_skip(w, i + 1) - 1, base_end = aw_skip(&base, at + 1) - 1;
                const char *stock = base.tokens.source.text + base.tokens.items[at + 1].begin;
                size_t stock_length = base.tokens.items[base_end].end - base.tokens.items[at + 1].begin;
                const char *own = w->tokens.source.text + w->tokens.items[i + 1].begin;
                size_t own_length = w->tokens.items[value_end].end - w->tokens.items[i + 1].begin;
                if (own_length == stock_length && !memcmp(own, stock, own_length)) continue;
                if (!aw_add_edit(st.edits + s, w->tokens.items[i + 1].begin, w->tokens.items[value_end].end, stock, stock_length)) {
                    aw_fail(error, capacity, "allocation failed"); goto models_done;
                }
            }
        }
    }
    free((void *)model_text); free((void *)label_text); free(model_length); free(label_length);
    model_text = label_text = NULL; model_length = label_length = NULL;

    /* Subwebs: a stock subweb a contribution changes beyond glue, or a new
     * subweb another contribution defines differently, becomes that
     * contribution's own copy; equal copies share one name. */
    for (size_t s = 0; s < count; s++) {
        aw_web *w = st.webs + s;
        for (size_t b = 0; b < w->subweb_count; b++) {
            const aw_block *x = w->subwebs + b;
            const aw_block *stock = aw_block_named(base.subwebs, base.subweb_count, x->name, x->name_length);
            int move = 0;
            if (stock) move = !aw_same_subweb(w, x, &base, stock, &base);
            else {
                /* The earliest contribution defining this name keeps it. */
                for (size_t t = 0; t < s && !move; t++) {
                    const aw_block *y = aw_block_named(st.webs[t].subwebs, st.webs[t].subweb_count, x->name, x->name_length);
                    if (!y) continue;
                    int kept = 1;
                    for (size_t m = 0; m < st.move_count; m++)
                        if (st.moves[m].source == t && strlen(st.moves[m].from) == x->name_length &&
                            !memcmp(st.moves[m].from, x->name, x->name_length)) kept = 0;
                    if (kept) move = !aw_same_subweb(w, x, st.webs + t, y, NULL);
                }
            }
            if (!move) continue;
            char name[512];
            uint64_t hash = aw_hash_subweb(w, x);
            if (!aw_relocated_name(&st, x->name, x->name_length, hash, name, sizeof(name))) {
                aw_fail(error, capacity, "no unique name for a contribution's own subweb"); goto done;
            }
            if (!aw_relocate(&st, s, x, name, hash)) { aw_fail(error, capacity, "allocation failed"); goto done; }
            if (stock) {
                /* The stock subweb keeps its place; the copy follows it. */
                size_t length = 0;
                char *restored = aw_stock_with_glue(&st, s, x, stock, &length);
                if (!restored || !aw_insert_before(st.edits + s, w, x->key, restored, length)) {
                    free(restored); aw_fail(error, capacity, "allocation failed"); goto done;
                }
                free(restored);
            }
        }
        /* A contribution never deletes a stock subweb or state for the others. */
        if (w->subwebs_open != SIZE_MAX) for (size_t b = 0; b < base.subweb_count; b++) {
            const aw_block *y = base.subwebs + b;
            if (aw_block_named(w->subwebs, w->subweb_count, y->name, y->name_length)) continue;
            if (!aw_restore(st.edits + s, w, w->subwebs, w->subweb_count, w->subwebs_close,
                &base, base.subwebs, base.subweb_count, b)) { aw_fail(error, capacity, "allocation failed"); goto done; }
        }
        if (w->states_open != SIZE_MAX) for (size_t b = 0; b < base.state_count; b++) {
            const aw_block *y = base.states + b;
            if (aw_block_named(w->states, w->state_count, y->name, y->name_length)) continue;
            if (!aw_restore(st.edits + s, w, w->states, w->state_count, w->states_close,
                &base, base.states, base.state_count, b)) { aw_fail(error, capacity, "allocation failed"); goto done; }
        }
    }
    for (size_t s = 0; s < count; s++) {
        if (!st.edits[s].count) continue;
        size_t length = 0;
        owned[s] = aw_apply(sources[s].text, 0, sources[s].length, st.edits + s, &length);
        if (!owned[s]) { aw_fail(error, capacity, "contribution edits overlap"); goto done; }
        sources[s] = (sh_decl_source){owned[s], length};
    }
    if (moves && st.move_count) {
        *moves = calloc(st.move_count, sizeof(**moves));
        if (!*moves) { aw_fail(error, capacity, "allocation failed"); goto done; }
        for (size_t m = 0; m < st.move_count; m++) {
            (*moves)[m].source = st.moves[m].source; (*moves)[m].from = st.moves[m].from; (*moves)[m].to = st.moves[m].to;
            st.moves[m].from = st.moves[m].to = NULL;
        }
        *move_count = st.move_count;
    }
    ok = 1;
    goto done;
models_done:
    free((void *)model_text); free((void *)label_text); free(model_length); free(label_length);
done:
    if (!ok) for (size_t i = 0; i < count; i++) { free(owned[i]); owned[i] = NULL; }
    for (size_t m = 0; m < st.move_count; m++) { free(st.moves[m].from); free(st.moves[m].to); }
    free(st.moves);
    if (st.webs) for (size_t s = 0; s < count; s++) aw_free(st.webs + s);
    if (st.edits) for (size_t s = 0; s < count; s++) aw_edits_free(st.edits + s);
    free(st.webs); free(st.edits); aw_free(&base);
    return ok;
}

void sh_decl_animweb_moves_free(sh_decl_animweb_move *moves, size_t count)
{
    for (size_t i = 0; moves && i < count; i++) { free(moves[i].from); free(moves[i].to); }
    free(moves);
}

char *sh_decl_weapon_subweb_rename(sh_decl_source weapon, const char *from, const char *to,
    size_t *length, int *changed, char *error, size_t capacity)
{
    static const char *fields[] = {"subweb_normal", "handsHitReactionSubweb"};
    sh_decl_tokens tokens = {0};
    aw_edits e = {0};
    char quoted[512];
    char *out = NULL;
    size_t from_length = strlen(from);
    *changed = 0; *length = 0;
    if (error && capacity) *error = 0;
    if (strlen(to) + 3 > sizeof(quoted)) { aw_fail(error, capacity, "subweb name is too long"); return NULL; }
    snprintf(quoted, sizeof(quoted), "\"%s\"", to);
    if (!sh_decl_native_lex(weapon, &tokens, error, capacity)) return NULL;
    for (size_t i = 0; i + 2 < tokens.count; i++) {
        int field = 0;
        for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) field |= sh_decl_token_is(&tokens, i, fields[k]);
        if (!field || !sh_decl_token_is(&tokens, i + 1, "=") || tokens.items[i + 2].kind != SH_DECL_TOKEN_STRING) continue;
        const char *value = tokens.source.text + tokens.items[i + 2].begin;
        size_t n = tokens.items[i + 2].end - tokens.items[i + 2].begin;
        if (n != from_length + 2 || memcmp(value + 1, from, from_length)) continue;
        if (!aw_add_edit(&e, tokens.items[i + 2].begin, tokens.items[i + 2].end, quoted, strlen(quoted))) {
            aw_fail(error, capacity, "allocation failed"); goto done;
        }
    }
    if (!e.count) goto done;
    out = aw_apply(weapon.text, 0, weapon.length, &e, length);
    if (!out) aw_fail(error, capacity, "allocation failed");
    else *changed = 1;
done:
    aw_edits_free(&e); sh_decl_tokens_free(&tokens); return out;
}
