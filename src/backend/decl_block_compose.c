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
    {"subWebs/subWeb/node/edges", BC_KEYED, "", "", "edge", "", 0, 0},
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
