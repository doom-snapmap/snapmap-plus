#include "decl_md6_compose.h"
#include "resource_types.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mc_token { size_t begin, end, close; } mc_token;
typedef struct mc_input {
    sh_decl_source source;
    mc_token *tokens;
    size_t count, capacity;
    sh_decl_node *tree;
    const sh_decl_node *init_writers[5];
    char *binding, *text;
    size_t length;
} mc_input;
typedef struct mc_context { char *error; size_t capacity; int failed; } mc_context;
static const char *mc_sections[] = {"init", "userProps", "jointGroups", "events", "aliases", "props",
    "eyeInfoCollection", "meshKits", "userChannelToAnimationAliasOverrides", "baseUserChannel", "userChannelWeightGroupOverride", "rigs"};
/* The native joint-group collection reader dispatches these thirteen category
 * names case-insensitively, in this order; category 9 also answers to "misc".
 * Each category owns one per-joint scalar key, empty where it has none. */
static const char *mc_groups[] = {"damageGroup", "painGroup", "twitchGroup", "deathGroup", "limblossGroup",
    "headTrackingGroup", "focusGroup", "orientationGroup", "hitTestGroup", "eyeGroup", "feetGroup",
    "silhouetteGroup", "traceGroup"};
static const char *mc_group_scalars[] = {"", "", "", "", "index", "weight", "", "", "radius", "",
    "", "bias", "radius"};
/* The same reader skips these five obsolete group kinds and their whole block
 * without an error, so a source that still carries one stays composable. */
static const char *mc_obsolete_groups[] = {"radiusDamageGroup", "headTrackingIKGroup",
    "reparentGroup", "upgradeGroup", "autoblendGroup"};
/* Mesh kits are read in this fixed order; each kit carries a reservation count
 * the reader recomputes from the entries it actually reads. */
static const char *mc_kit_names[] = {"Heads", "Gear", "Gore", "Door", "Melee_Highlight"};
static int mc_fail(mc_context *c, const char *reason)
{
    if (!c->failed && c->error && c->capacity) snprintf(c->error, c->capacity, "MD6 composition: %s", reason);
    c->failed = 1; return 0;
}
static char *mc_copy(mc_context *c, const char *s, size_t n)
{
    char *out = n < SIZE_MAX ? malloc(n + 1) : NULL;
    if (!out) mc_fail(c, "allocation failed");
    else { memcpy(out, s, n); out[n] = 0; }
    return out;
}
static sh_decl_node *mc_node(mc_context *c, const char *key, const char *value)
{
    sh_decl_node *n = calloc(1, sizeof(*n));
    if (!n) { mc_fail(c, "allocation failed"); return NULL; }
    n->key = key ? mc_copy(c, key, strlen(key)) : NULL;
    n->value = value ? mc_copy(c, value, strlen(value)) : NULL;
    n->compound = value == NULL; n->assignment = key != NULL;
    if (c->failed) { sh_decl_tree_free(n); return NULL; }
    return n;
}
static sh_decl_node *mc_add(sh_decl_node *parent, sh_decl_node *child)
{
    sh_decl_node **tail;
    if (!parent) { sh_decl_tree_free(child); return NULL; }
    for (tail = &parent->children; *tail; tail = &(*tail)->next) {}
    *tail = child; return child;
}
static int mc_is(const mc_input *in, size_t i, const char *word)
{
    return i < in->count && in->tokens[i].end - in->tokens[i].begin == strlen(word) &&
        !memcmp(in->source.text + in->tokens[i].begin, word, strlen(word));
}
static int mc_is_ci(const mc_input *in, size_t i, const char *word)
{
    size_t n = strlen(word);
    if (i >= in->count || in->tokens[i].end - in->tokens[i].begin != n) return 0;
    return !_strnicmp(in->source.text + in->tokens[i].begin, word, n);
}
static int mc_group(const mc_input *in, size_t i)
{
    for (int n = 0; n < (int)(sizeof(mc_groups) / sizeof(mc_groups[0])); n++)
        if (mc_is_ci(in, i, mc_groups[n])) return n;
    if (mc_is_ci(in, i, "misc")) return 9;   /* native alias for eyeGroup */
    return -1;
}
static int mc_obsolete_group(const mc_input *in, size_t i)
{
    for (int n = 0; n < (int)(sizeof(mc_obsolete_groups) / sizeof(mc_obsolete_groups[0])); n++)
        if (mc_is_ci(in, i, mc_obsolete_groups[n])) return n;
    return -1;
}
/* Token boundaries are used only for this envelope, never to infer fields in
 * opaque payloads. Complete slices retain their authored spelling and comments.
 * Balanced delimiters use a heap stack; there is no fixed token/depth budget. */
static int mc_tokenize(mc_context *c, mc_input *in)
{
    size_t p = 0, *stack = NULL, depth = 0, stack_capacity = 0;
    const char *s = in->source.text;
    if (!s || memchr(s, 0, in->source.length)) return mc_fail(c, "invalid source bytes");
    while (p < in->source.length && !c->failed) {
        size_t start, i;
        unsigned char ch = (unsigned char)s[p];
        if (isspace(ch)) { p++; continue; }
        if (ch == '/' && p + 1 < in->source.length && s[p + 1] == '/') {
            p += 2; while (p < in->source.length && s[p] != '\n') p++; continue;
        }
        if (ch == '/' && p + 1 < in->source.length && s[p + 1] == '*') {
            p += 2; while (p + 1 < in->source.length && (s[p] != '*' || s[p + 1] != '/')) p++;
            if (p + 1 >= in->source.length) { mc_fail(c, "unterminated comment"); break; }
            p += 2; continue;
        }
        if (ch < 32) { mc_fail(c, "invalid source control byte"); break; }
        start = p++;
        if (ch == '"') {
            int closed = 0;
            while (p < in->source.length) {
                ch = (unsigned char)s[p++];
                if (ch == '"') { closed = 1; break; }
                if (ch < 32) { mc_fail(c, "control byte in string"); break; }
                /* The native declaration lexer disables string escapes.
                 * A backslash remains a byte, including before the end quote. */
            }
            if (!closed) { mc_fail(c, "unterminated string"); break; }
        } else if (!strchr("{}()[]=;", ch)) {
            while (p < in->source.length && !isspace((unsigned char)s[p]) &&
                   !strchr("{}()[]=;\"", s[p])) {
                if (s[p] == '/' && p + 1 < in->source.length && (s[p + 1] == '/' || s[p + 1] == '*')) break;
                if ((unsigned char)s[p] < 32) { mc_fail(c, "invalid token byte"); break; }
                p++;
            }
        }
        if (in->count == in->capacity) {
            size_t cap = in->capacity ? in->capacity * 2 : 128;
            mc_token *next = cap > in->capacity && cap <= SIZE_MAX / sizeof(*next) ?
                realloc(in->tokens, cap * sizeof(*next)) : NULL;
            if (!next) { mc_fail(c, "token allocation failed"); break; }
            in->tokens = next; in->capacity = cap;
        }
        i = in->count++;
        in->tokens[i] = (mc_token){start, p, SIZE_MAX};
        if (p != start + 1) continue;
        ch = (unsigned char)s[start];
        if (strchr("{([", ch)) {
            if (depth == stack_capacity) {
                size_t cap = stack_capacity ? stack_capacity * 2 : 16;
                size_t *next = cap > stack_capacity && cap <= SIZE_MAX / sizeof(*next) ? realloc(stack, cap * sizeof(*next)) : NULL;
                if (!next) { mc_fail(c, "delimiter allocation failed"); break; }
                stack = next; stack_capacity = cap;
            }
            stack[depth++] = i;
        } else if (strchr("})]", ch)) {
            char opening = ch == '}' ? '{' : ch == ')' ? '(' : '[';
            if (!depth || s[in->tokens[stack[depth - 1]].begin] != opening) {
                mc_fail(c, "mismatched delimiter"); break;
            }
            in->tokens[stack[--depth]].close = i;
        }
    }
    if (depth) mc_fail(c, "unclosed delimiter");
    free(stack); return !c->failed;
}
static char *mc_hex(mc_context *c, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    char *out = n <= (SIZE_MAX - 3) / 2 ? malloc(n * 2 + 3) : NULL;
    if (!out) { mc_fail(c, "payload allocation failed"); return NULL; }
    out[0] = '"';
    for (size_t i = 0; i < n; i++) { out[1 + i * 2] = hex[(unsigned char)s[i] >> 4]; out[2 + i * 2] = hex[(unsigned char)s[i] & 15]; }
    out[n * 2 + 1] = '"'; out[n * 2 + 2] = 0; return out;
}
static int mc_nibble(char ch)
{ return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
static char *mc_unhex(mc_context *c, const sh_decl_node *node)
{
    const char *s; size_t n; char *out;
    if (!sh_decl_tree_literal(node, &s, &n) || n % 2) { mc_fail(c, "invalid private payload"); return NULL; }
    out = malloc(n / 2 + 1);
    if (!out) { mc_fail(c, "payload allocation failed"); return NULL; }
    for (size_t i = 0; i < n; i += 2) {
        int a = mc_nibble(s[i]), b = mc_nibble(s[i + 1]);
        if (a < 0 || b < 0 || !(a | b)) { free(out); mc_fail(c, "invalid private byte"); return NULL; }
        out[i / 2] = (char)((a << 4) | b);
    }
    out[n / 2] = 0; return out;
}
/* A record identity is the bytes the native lexer interns. Its flags disable
 * escape processing, so a backslash is an ordinary byte -- including one
 * immediately before the closing quote -- and a quote can only end the token.
 * Bytes above ASCII are carried through unchanged; the engine stores bytes, not
 * code points, and nothing here re-encodes an engine or Windows path. Control
 * bytes cannot appear: the tokenizer refuses them inside a string. */
static char *mc_identity(mc_context *c, const mc_input *in, size_t i, int path)
{
    const char *s; size_t n; char *name;
    if (i >= in->count) { mc_fail(c, "missing named record identity"); return NULL; }
    s = in->source.text + in->tokens[i].begin; n = in->tokens[i].end - in->tokens[i].begin;
    if (n < 3 || *s != '"' || s[n - 1] != '"') { mc_fail(c, "record identity must be a nonempty quoted string"); return NULL; }
    s++; n -= 2;
    for (size_t j = 0; j < n; j++) if ((unsigned char)s[j] < 32) {
        mc_fail(c, "record identity contains a control byte"); return NULL;
    }
    /* Native animation event paths lowercase and strip a single-root prefix.
     * UNC prefixes are preserved. Lowercasing is the reader's ASCII fold, so a
     * multi-byte sequence keeps its own bytes. */
    if (path && *s == '/' && (n < 2 || s[1] != '/')) { do { s++; n--; } while (n && *s == '/'); }
    name = mc_copy(c, s, n);
    if (name && path) for (size_t j = 0; j < n; j++) if (name[j] >= 'A' && name[j] <= 'Z') name[j] += 'a' - 'A';
    return name;
}
static char *mc_group_id(mc_context *c, int kind, const char *name)
{
    size_t n = strlen(name); char *id = n <= SIZE_MAX - 24 ? malloc(n + 24) : NULL;
    if (!id) mc_fail(c, "identity allocation failed"); else snprintf(id, n + 24, "%d:%s", kind, name);
    return id;
}
static char *mc_animation_identity(mc_context *c, const mc_input *in, size_t i)
{
    const char *s; size_t n;
    if (i >= in->count) { mc_fail(c, "missing alias animation string"); return NULL; }
    s = in->source.text + in->tokens[i].begin; n = in->tokens[i].end - in->tokens[i].begin;
    if (n < 2 || *s != '"' || s[n - 1] != '"') {
        mc_fail(c, "alias animation must be a quoted string"); return NULL;
    }
    /* The alias reader interns the literal bytes; it does not use the event
     * reader's lowercase/root-prefix normalization. This includes backslashes. */
    return mc_copy(c, s + 1, n - 2);
}
static int mc_slice(mc_context *c, const mc_input *in, sh_decl_node *parent,
    const char *key, size_t begin, size_t end)
{
    char *encoded;
    if (begin >= end || end > in->count) return mc_fail(c, "invalid record extent");
    encoded = mc_hex(c, in->source.text + in->tokens[begin].begin,
        in->tokens[end - 1].end - in->tokens[begin].begin);
    if (encoded) mc_add(parent, mc_node(c, key, encoded));
    free(encoded); return !c->failed;
}
static int mc_has_id(const sh_decl_node *list, const char *id)
{
    const sh_decl_node *entry;
    for (entry = list ? list->children : NULL; entry; entry = entry->next) {
        const sh_decl_node *key = sh_decl_tree_member(entry, "id");
        if (key && !strcmp(key->value, id)) return 1;
    }
    return 0;
}
static sh_decl_node *mc_entry(mc_context *c, sh_decl_node *list, size_t index, const char *identity)
{
    char key[64], *id = mc_hex(c, identity, strlen(identity));
    sh_decl_node *entry = NULL;
    if (!id) return NULL;
    if (mc_has_id(list, id)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "duplicate named record in %s: %s", list->key, identity);
        mc_fail(c, detail);
    }
    else {
        snprintf(key, sizeof(key), "item[%zu]", index);
        entry = mc_add(list, mc_node(c, key, NULL));
        mc_add(entry, mc_node(c, "id", id));
    }
    free(id); return entry;
}
/* Native alias animations and flags may repeat. A value plus its occurrence
 * retains that multiplicity while equal contributions share one effective edit. */
static sh_decl_node *mc_occurrence(mc_context *c, sh_decl_node *list, size_t index, const char *identity)
{
    size_t n = strlen(identity), occurrence = 0;
    char *id = n <= SIZE_MAX - 32 ? malloc(n + 32) : NULL;
    sh_decl_node *entry = NULL;
    if (!id) { mc_fail(c, "occurrence identity allocation failed"); return NULL; }
    while (!c->failed) {
        char *encoded; int used;
        snprintf(id, n + 32, "%s:%zu", identity, occurrence++);
        encoded = mc_hex(c, id, strlen(id));
        if (!encoded) break;
        used = mc_has_id(list, encoded); free(encoded);
        if (!used) { entry = mc_entry(c, list, index, id); break; }
    }
    free(id); return entry;
}
static void mc_count(mc_context *c, sh_decl_node *number, size_t count)
{
    char text[32];
    if (!number || c->failed) return;
    snprintf(text, sizeof(text), "%zu", count);
    free(number->value); number->value = mc_copy(c, text, strlen(text));
}
static int mc_alias(mc_context *c, mc_input *in, sh_decl_node *entry, size_t start, size_t block)
{
    sh_decl_node *parts = mc_add(entry, mc_node(c, "body", NULL));
    sh_decl_node *number = mc_add(parts, mc_node(c, "num", "0"));
    size_t i = block + 3, end = in->tokens[block].close, count = 0;
    mc_slice(c, in, entry, "head", start, i);
    while (i < end && !c->failed) {
        size_t begin = i++;
        if (mc_is(in, begin, "anim")) {
            char *name = mc_animation_identity(c, in, i++), *id = NULL;
            sh_decl_node *part;
            if (name) {
                size_t n = strlen(name);
                id = n <= SIZE_MAX - 6 ? malloc(n + 6) : NULL;
                if (id) snprintf(id, n + 6, "anim:%s", name);
                else mc_fail(c, "animation identity allocation failed");
            }
            part = id ? mc_occurrence(c, parts, count++, id) : NULL;
            if (part) {
                char *value = mc_hex(c, name, strlen(name));
                if (value) mc_add(part, mc_node(c, "value", value));
                free(value); mc_slice(c, in, part, "text", begin, i);
            }
            free(id); free(name);
        } else if (mc_is(in, begin, "flags")) {
            sh_decl_node *part, *flags, *flag_number;
            size_t flag_end, flag_count = 0;
            if (!mc_is(in, i, "{") || in->tokens[i].close >= end) { mc_fail(c, "alias flags need a complete block"); break; }
            flag_end = in->tokens[i++].close;
            part = mc_occurrence(c, parts, count++, "flags");
            flags = mc_add(part, mc_node(c, "flags", NULL));
            flag_number = mc_add(flags, mc_node(c, "num", "0"));
            while (i < flag_end && !c->failed) {
                char *id; sh_decl_node *flag;
                if (mc_is(in, i, "{") || mc_is(in, i, "(") || mc_is(in, i, "[")) {
                    mc_fail(c, "nested alias flags are not native flag tokens"); break;
                }
                id = mc_copy(c, in->source.text + in->tokens[i].begin, in->tokens[i].end - in->tokens[i].begin);
                flag = id ? mc_occurrence(c, flags, flag_count++, id) : NULL;
                if (flag) mc_slice(c, in, flag, "body", i, i + 1);
                free(id); i++;
            }
            mc_count(c, flag_number, flag_count); i = flag_end + 1;
        } else { mc_fail(c, "unknown field in animation alias"); break; }
    }
    mc_count(c, number, count);
    return !c->failed;
}
/* True when two tokens sit on one source line. The engine reads to the end of a
 * line in exactly two places -- a contentsFlags list and a flag-enum argument,
 * both through its read-token-on-this-line helper -- so line position is part of
 * the native grammar only for those value lists. Every other key has a fixed
 * native arity and is parsed without regard to how the author wrapped the
 * source. A value list additionally stops at anything that can begin a key,
 * which is what keeps an author's wrapping out of the composition granularity;
 * because every value is retained and re-emitted as its authored slice, this
 * choice cannot change the bytes the engine sees either way. */
static int mc_same_line(const mc_input *in, size_t a, size_t b)
{
    if (a >= in->count || b >= in->count) return 0;
    for (size_t at = in->tokens[a].end; at < in->tokens[b].begin; at++)
        if (in->source.text[at] == '\n') return 0;
    return 1;
}
/* Native key sets. Joint-group payloads take the category's own scalar plus
 * offset (per-joint) or vec3 (args), surfType and contentsFlags, and refuse
 * anything else the way the readers do. Animation-event payloads take
 * frame/row/locked and then arguments written as their native type names. */
typedef struct mc_key_rules { const char *scalar; int event, args; } mc_key_rules;

static const char *mc_event_single[] = {"frame", "row", "locked", "bool", "float",
    "string", "animalias", "joint", "jointName"};
static const char *mc_event_group[] = {"vec3", "angles"};

static const char *mc_declaration_family(const mc_input *in, size_t i);

static int mc_numeric(const mc_input *in, size_t i)
{
    if (i >= in->count || in->tokens[i].end == in->tokens[i].begin) return 0;
    for (size_t at = in->tokens[i].begin; at < in->tokens[i].end; at++) {
        char ch = in->source.text[at];
        if (!isdigit((unsigned char)ch) && ch != '-' && ch != '+' && ch != '.' &&
            ch != 'e' && ch != 'E') return 0;
    }
    return 1;
}
/* Whether this token can begin a key of the payload being read. */
static int mc_key_start(const mc_input *in, size_t i, const mc_key_rules *rules)
{
    if (rules->event) {
        for (size_t n = 0; n < sizeof(mc_event_single) / sizeof(mc_event_single[0]); n++)
            if (mc_is_ci(in, i, mc_event_single[n])) return 1;
        for (size_t n = 0; n < sizeof(mc_event_group) / sizeof(mc_event_group[0]); n++)
            if (mc_is_ci(in, i, mc_event_group[n])) return 1;
        return mc_is_ci(in, i, "int") || mc_declaration_family(in, i) != NULL;
    }
    if (mc_is(in, i, "surfType") || mc_is(in, i, "contentsFlags")) return 1;
    if (mc_is(in, i, rules->args ? "vec3" : "offset")) return 1;
    return rules->scalar && rules->scalar[0] && mc_is(in, i, rules->scalar);
}
/* Index one past the last token of this key's value, or key_at + 1 when the key
 * has no value at all. */
static size_t mc_value_extent(mc_context *c, const mc_input *in, const mc_key_rules *rules,
    size_t key_at, size_t end)
{
    size_t i = key_at + 1;
    int single = 0, group = 0;
    if (i >= end) return i;
    if (rules->event) {
        for (size_t n = 0; n < sizeof(mc_event_single) / sizeof(mc_event_single[0]); n++)
            if (mc_is_ci(in, key_at, mc_event_single[n])) single = 1;
        for (size_t n = 0; n < sizeof(mc_event_group) / sizeof(mc_event_group[0]); n++)
            if (mc_is_ci(in, key_at, mc_event_group[n])) group = 1;
        /* A declaration-typed argument reads one token. `int` reads one number,
         * or an enum type name whose flag form keeps reading constants. Any
         * other type name is such an enum. */
        if (!single && !group && mc_declaration_family(in, key_at)) single = 1;
        if (!single && !group && mc_is_ci(in, key_at, "int") && mc_numeric(in, i)) single = 1;
    } else {
        if (mc_is(in, key_at, "surfType")) single = 1;
        else if (mc_is(in, key_at, rules->args ? "vec3" : "offset")) group = 1;
        else if (rules->scalar && rules->scalar[0] && mc_is(in, key_at, rules->scalar)) single = 1;
        else if (!mc_is(in, key_at, "contentsFlags")) {
            mc_fail(c, "unsupported key in a native joint-group payload");
            return i;
        }
    }
    if (group) {
        if (!mc_is(in, i, "(") || in->tokens[i].close >= end) {
            mc_fail(c, "native vector value needs its complete parenthesized group");
            return i;
        }
        return in->tokens[i].close + 1;
    }
    if (single) return i + 1;
    /* The value lists the engine reads to the end of their line. */
    i++;
    while (i < end && mc_same_line(in, i - 1, i) && !mc_key_start(in, i, rules) &&
           !mc_is(in, i, "{") && !mc_is(in, i, "}") && !mc_is(in, i, "(")) i++;
    return i;
}
/* A native assignment payload: each key owns its native value, so independent
 * edits of different keys compose and two edits of one key conflict, however the
 * source is wrapped. The authored slice is retained whole; no value is
 * reinterpreted. */
static int mc_keys(mc_context *c, mc_input *in, sh_decl_node *parent, size_t begin, size_t end,
    const mc_key_rules *rules)
{
    sh_decl_node *keys = mc_add(parent, mc_node(c, "keys", NULL));
    sh_decl_node *number = mc_add(keys, mc_node(c, "num", "0"));
    size_t i = begin, count = 0;
    while (i < end && !c->failed) {
        size_t key_at = i, stop;
        char *name; sh_decl_node *item;
        if (mc_is(in, key_at, "{") || mc_is(in, key_at, "}") || mc_is(in, key_at, "(")) {
            mc_fail(c, "nested block inside a native assignment payload"); break;
        }
        stop = mc_value_extent(c, in, rules, key_at, end);
        if (c->failed) break;
        if (stop == key_at + 1) { mc_fail(c, "native payload key has no value"); break; }
        i = stop;
        name = mc_copy(c, in->source.text + in->tokens[key_at].begin,
            in->tokens[key_at].end - in->tokens[key_at].begin);
        item = name ? mc_occurrence(c, keys, count++, name) : NULL;
        if (item) { mc_slice(c, in, item, "name", key_at, key_at + 1); mc_slice(c, in, item, "body", key_at, stop); }
        free(name);
    }
    mc_count(c, number, count);
    return !c->failed;
}
/* The value of a named animation-event key, unquoted, located by walking the
 * payload with its native arities rather than by source line. */
static char *mc_key_value(mc_context *c, const mc_input *in, size_t begin, size_t end, const char *key)
{
    static const mc_key_rules rules = {"", 1, 0};
    mc_context quiet = {NULL, 0, 0};
    size_t i = begin;
    while (i < end && !quiet.failed) {
        size_t stop = mc_value_extent(&quiet, in, &rules, i, end);
        if (quiet.failed || stop <= i + 1) break;
        if (mc_is_ci(in, i, key)) {
            const char *s = in->source.text + in->tokens[i + 1].begin;
            size_t n = in->tokens[i + 1].end - in->tokens[i + 1].begin;
            if (n >= 2 && *s == '"') { s++; n -= 2; }
            return mc_copy(c, s, n);
        }
        i = stop;
    }
    return NULL;
}
/* A joint-group body is an ordered sequence of native records: an args block
 * that sets the running defaults for the joints listed after it, and joint
 * entries that may carry their own payload block. The engine appends each
 * joint's skeleton index to the group and writes that joint's payload at that
 * index, so a payload travels with its entry and the composed order re-derives
 * every index. Repeated joint names are native, so identity is the lowercased
 * name the reader resolves plus its occurrence. */
static int mc_group_body(mc_context *c, mc_input *in, sh_decl_node *entry, int category,
    size_t begin, size_t end)
{
    const mc_key_rules args_rules = {"", 0, 1};
    const mc_key_rules joint_rules = {category >= 0 &&
        category < (int)(sizeof(mc_group_scalars) / sizeof(mc_group_scalars[0])) ?
        mc_group_scalars[category] : "", 0, 0};
    sh_decl_node *list = mc_add(entry, mc_node(c, "records", NULL));
    sh_decl_node *number = mc_add(list, mc_node(c, "num", "0"));
    size_t i = begin, count = 0;
    while (i < end && !c->failed) {
        sh_decl_node *record;
        if (mc_is(in, i, "args")) {
            size_t close;
            if (!mc_is(in, i + 1, "{") || (close = in->tokens[i + 1].close) >= end) {
                mc_fail(c, "joint-group args needs a complete native block"); break;
            }
            record = mc_occurrence(c, list, count++, "args");
            if (!record) break;
            mc_add(record, mc_node(c, "args", "1"));
            mc_keys(c, in, record, i + 2, close, &args_rules);
            i = close + 1;
        } else {
            size_t at = i++, n = in->tokens[at].end - in->tokens[at].begin;
            char *name = NULL, *id = NULL;
            if (mc_is(in, at, "{") || mc_is(in, at, "}") || mc_is(in, at, "(")) {
                mc_fail(c, "joint group expects a joint name or an args block"); break;
            }
            name = mc_copy(c, in->source.text + in->tokens[at].begin, n);
            if (name) {
                id = n <= SIZE_MAX - 8 ? malloc(n + 8) : NULL;
                if (!id) mc_fail(c, "joint identity allocation failed");
                else {
                    for (char *letter = name; *letter; letter++)
                        if (*letter >= 'A' && *letter <= 'Z') *letter += 'a' - 'A';
                    snprintf(id, n + 8, "joint:%s", name);
                }
            }
            record = id ? mc_occurrence(c, list, count++, id) : NULL;
            if (record) {
                mc_slice(c, in, record, "name", at, at + 1);
                if (i < end && mc_is(in, i, "{")) {
                    size_t close = in->tokens[i].close;
                    if (close >= end) mc_fail(c, "joint payload needs a complete native block");
                    else { mc_keys(c, in, record, i + 1, close, &joint_rules); i = close + 1; }
                }
            }
            free(name); free(id);
        }
    }
    mc_count(c, number, count);
    return !c->failed;
}
/* One animation's event records. The reader accepts only event blocks here and
 * refuses a duplicate animation block outright. An event is identified by its
 * frame-command name with the frame and row it is placed at, plus an occurrence
 * for the eleven shipped cases that repeat all three. */
static int mc_event_records(mc_context *c, mc_input *in, sh_decl_node *entry, size_t begin, size_t end)
{
    sh_decl_node *list = mc_add(entry, mc_node(c, "records", NULL));
    sh_decl_node *number = mc_add(list, mc_node(c, "num", "0"));
    size_t i = begin, count = 0;
    while (i < end && !c->failed) {
        size_t start = i, close = 0;
        char *name, *frame, *row, *id = NULL;
        sh_decl_node *record;
        if (!mc_is(in, i, "event")) { mc_fail(c, "animation block accepts only event records"); break; }
        name = mc_identity(c, in, i + 1, 0);
        if (!name) break;
        if (!mc_is(in, i + 2, "{") || (close = in->tokens[i + 2].close) >= end) {
            free(name); mc_fail(c, "animation event needs a complete native block"); break;
        }
        frame = mc_key_value(c, in, i + 3, close, "frame");
        row = mc_key_value(c, in, i + 3, close, "row");
        if (!c->failed) {
            size_t n = strlen(name) + strlen(frame ? frame : "") + strlen(row ? row : "");
            id = n <= SIZE_MAX - 32 ? malloc(n + 32) : NULL;
            if (!id) mc_fail(c, "event identity allocation failed");
            else snprintf(id, n + 32, "%s|%s|%s", name, frame ? frame : "", row ? row : "");
        }
        record = id ? mc_occurrence(c, list, count++, id) : NULL;
        if (record) {
            mc_slice(c, in, record, "head", start, i + 3);
            {
                static const mc_key_rules rules = {"", 1, 0};
                mc_keys(c, in, record, i + 3, close, &rules);
            }
        }
        free(name); free(frame); free(row); free(id);
        i = close + 1;
    }
    mc_count(c, number, count);
    return !c->failed;
}
/* eyeInfoCollection is a counted list of anonymous eyeInfo records whose native
 * index is assignment order; the leading count is a reservation the reader
 * recomputes from the records it reads. Existing records stay positional, so one
 * record cannot be edited two different ways. */
static int mc_eye_records(mc_context *c, mc_input *in, sh_decl_node *list, size_t begin, size_t end)
{
    sh_decl_node *number = mc_add(list, mc_node(c, "num", "0"));
    size_t i = begin, count = 0;
    while (i < end && !c->failed) {
        size_t start = i, close;
        char key[48]; sh_decl_node *record;
        if (!mc_is(in, i, "eyeInfo")) { mc_fail(c, "eye info collection accepts only eyeInfo records"); break; }
        if (!mc_is(in, i + 1, "{") || (close = in->tokens[i + 1].close) >= end) {
            mc_fail(c, "eyeInfo needs a complete native block"); break;
        }
        snprintf(key, sizeof(key), "slot:%zu", count);
        record = mc_entry(c, list, count++, key);
        if (record) mc_slice(c, in, record, "body", start, close + 1);
        i = close + 1;
    }
    mc_count(c, number, count);
    return !c->failed;
}
/* meshKits holds up to five fixed kits in reader order. A kit entry names one
 * mesh set of the selected model; the engine resolves those names to surface
 * indices itself, so the composed text keeps the names and never an index. */
static int mc_kits(mc_context *c, mc_input *in, sh_decl_node *list, size_t begin, size_t end)
{
    sh_decl_node *number = mc_add(list, mc_node(c, "num", "0"));
    size_t i = begin, count = 0;
    int previous = -1;
    while (i < end && !c->failed) {
        size_t close, block = i + 1, entries_count = 0;
        int kit = -1;
        sh_decl_node *record, *entries, *entry_number;
        for (int n = 0; n < (int)(sizeof(mc_kit_names) / sizeof(mc_kit_names[0])); n++)
            if (mc_is_ci(in, i, mc_kit_names[n])) { kit = n; break; }
        if (kit < 0 || kit <= previous) { mc_fail(c, "unknown, repeated or out-of-order mesh kit"); break; }
        previous = kit;
        if (!mc_is(in, block, "{")) block++;   /* the reservation count is optional */
        if (!mc_is(in, block, "{") || (close = in->tokens[block].close) >= end) {
            mc_fail(c, "mesh kit needs a complete native block"); break;
        }
        record = mc_entry(c, list, count++, mc_kit_names[kit]);
        if (!record) break;
        mc_slice(c, in, record, "name", i, i + 1);
        entries = mc_add(record, mc_node(c, "entries", NULL));
        entry_number = mc_add(entries, mc_node(c, "num", "0"));
        i = block + 1;
        while (i < close && !c->failed) {
            char *name = mc_identity(c, in, i, 0);
            sh_decl_node *item = NULL;
            if (!name) break;
            if (!mc_is(in, i + 1, "=") || i + 2 >= close) mc_fail(c, "mesh kit entry needs its native assignment");
            else item = mc_entry(c, entries, entries_count++, name);
            if (item) mc_slice(c, in, item, "body", i, i + 3);
            free(name); i += 3;
        }
        mc_count(c, entry_number, entries_count);
        i = close + 1;
    }
    mc_count(c, number, count);
    return !c->failed;
}
static int mc_records(mc_context *c, mc_input *in, sh_decl_node *list, int kind, size_t begin, size_t end)
{
    size_t i = begin, count = 0;
    sh_decl_node *number = mc_add(list, mc_node(c, "num", "0"));
    while (i < end && !c->failed) {
        size_t start = i, body;
        char *name = NULL, *id = NULL, *target = NULL;
        sh_decl_node *entry;
        int category = -1, obsolete = -1;
        if (kind == 2) {
            /* The collection reader ignores a bare jointGroup token. */
            if (mc_is_ci(in, i, "jointGroup")) { i++; continue; }
            category = mc_group(in, i);
            obsolete = category < 0 ? mc_obsolete_group(in, i) : -1;
            if (category < 0 && obsolete < 0) { mc_fail(c, "unsupported joint-group category or obsolete syntax"); break; }
            i++;
            name = mc_identity(c, in, i++, 0);
            if (name && obsolete >= 0) {
                size_t n = strlen(name) + strlen(mc_obsolete_groups[obsolete]);
                id = n <= SIZE_MAX - 16 ? malloc(n + 16) : NULL;
                /* The engine skips these without an error, so they are retained
                 * whole rather than interpreted or dropped. */
                if (!id) mc_fail(c, "identity allocation failed");
                else snprintf(id, n + 16, "skipped:%s:%s", mc_obsolete_groups[obsolete], name);
            } else if (name) id = mc_group_id(c, category, name);
        } else if (kind == 3) {
            if (!mc_is(in, i++, "anim")) { mc_fail(c, "expected animation event block"); break; }
            name = mc_identity(c, in, i++, 1);
            if (name) id = mc_copy(c, name, strlen(name));
        } else {
            if (!mc_is(in, i++, "alias") || !mc_is(in, i, "{") || !mc_is(in, i + 1, "name")) {
                mc_fail(c, "alias must begin with its native name field"); break;
            }
            name = mc_identity(c, in, i + 2, 0);
            if (name) id = mc_copy(c, name, strlen(name));
        }
        body = i;
        if (!c->failed && kind == 2 && mc_is(in, i, "=")) {
            int target_category = mc_group(in, i + 1);
            char *target_name = target_category >= 0 ? mc_identity(c, in, i + 2, 0) : NULL;
            if (target_name) { char *plain = mc_group_id(c, target_category, target_name); if (plain) target = mc_hex(c, plain, strlen(plain)); free(plain); }
            free(target_name);
            if (!target || !mc_has_id(list, target)) mc_fail(c, "joint-group copy must reference an earlier local group");
            i += 3;
        } else if (!c->failed) {
            if (!mc_is(in, body, "{") || in->tokens[body].close >= end) mc_fail(c, "named record needs a complete block");
            else i = in->tokens[body].close + 1;
        }
        if (!c->failed && i <= end && id) {
            entry = mc_entry(c, list, count++, id);
            if (entry) {
                if (kind == 4) mc_alias(c, in, entry, start, body);
                else if (target || obsolete >= 0) mc_slice(c, in, entry, "body", start, i);
                else if (kind == 2) {
                    mc_slice(c, in, entry, "head", start, body + 1);
                    mc_group_body(c, in, entry, category, body + 1, in->tokens[body].close);
                } else if (kind == 3) {
                    mc_slice(c, in, entry, "head", start, body + 1);
                    mc_event_records(c, in, entry, body + 1, in->tokens[body].close);
                } else mc_slice(c, in, entry, "body", start, i);
                if (target) mc_add(entry, mc_node(c, "target", target));
            }
        }
        free(name); free(id); free(target);
    }
    if (!c->failed && i != end) mc_fail(c, "record exceeds its section");
    if (!c->failed && number) { char text[32]; snprintf(text, sizeof(text), "%zu", count); free(number->value); number->value = mc_copy(c, text, strlen(text)); }
    return !c->failed;
}
static int mc_init(mc_context *c, mc_input *in, sh_decl_node *list, size_t begin, size_t end)
{
    static const char *fields[] = {"inherit", "mesh", "offset", "calcRefBoundsFromJoints"};
    sh_decl_node *binding = mc_node(c, NULL, NULL);
    sh_decl_node *number = mc_add(list, mc_node(c, "num", "0"));
    size_t i = begin, index = 0, count = 0, occurrences[4] = {0}, length;
    while (i < end && !c->failed) {
        size_t start = i++, value = i; char key[80]; int field = -1;
        sh_decl_node *entry;
        for (int f = 0; f < 4; f++) if (mc_is(in, start, fields[f])) { field = f; break; }
        if (field < 0) { mc_fail(c, "unsupported init field"); break; }
        if (field < 2) {
            char *name = NULL;
            if (!(field == 0 && mc_is(in, i, "\"\""))) name = mc_identity(c, in, i, 0);
            free(name);
            snprintf(key, sizeof(key), "item[%zu]", index++);
            mc_slice(c, in, binding, key, start, ++i);
        } else if (field == 2) {
            if (!mc_is(in, i, "(") || in->tokens[i].close >= end) mc_fail(c, "invalid init offset");
            else i = in->tokens[i].close + 1;
        } else {
            if (i >= end || mc_is(in, i, "{") || mc_is(in, i, "}")) mc_fail(c, "missing init bounds value"); else i++;
        }
        if (c->failed) break;
        snprintf(key, sizeof(key), "%s:%zu", fields[field], occurrences[field]++);
        entry = mc_entry(c, list, count++, key);
        if (!entry || !mc_slice(c, in, entry, "body", start, i)) break;
        /* The native reader executes these setters in source order. Nonempty
         * inherit also copies the parent's model, offset and bounds flag. An
         * empty inherit only changes its name; it does not clear a prior parent.
         * Keep repeated writes and use final writers to detect hidden clobbers. */
        if (!field) {
            in->init_writers[0] = sh_decl_tree_member(entry, "body");
            if (!mc_is(in, value, "\"\"")) for (size_t w = 1; w < 5; w++)
                in->init_writers[w] = in->init_writers[0];
        } else in->init_writers[field + 1] = sh_decl_tree_member(entry, "body");
    }
    if (!c->failed && number) { char text[32]; snprintf(text, sizeof(text), "%zu", count); free(number->value); number->value = mc_copy(c, text, strlen(text)); }
    if (!c->failed) in->binding = sh_decl_tree_write(binding, &length);
    sh_decl_tree_free(binding);
    if (!c->failed && !in->binding) mc_fail(c, "binding allocation failed");
    return !c->failed;
}
static int mc_parse(mc_context *c, mc_input *in)
{
    sh_decl_node *edit;
    size_t i = 1; int previous = -1; unsigned seen = 0;
    if (!mc_tokenize(c, in)) return 0;
    if (!mc_is(in, 0, "{") || in->tokens[0].close != in->count - 1) return mc_fail(c, "expected one complete MD6 definition");
    in->tree = mc_node(c, NULL, NULL); edit = mc_add(in->tree, mc_node(c, "edit", NULL));
    while (i + 1 < in->count && !c->failed) {
        size_t start = i++, block = i, end;
        int section = -1;
        for (int n = 0; n < (int)(sizeof(mc_sections) / sizeof(mc_sections[0])); n++) if (mc_is(in, start, mc_sections[n])) { section = n; break; }
        if (section < 0 || section <= previous) return mc_fail(c, "unknown, repeated or out-of-order MD6 section");
        previous = section; seen |= 1u << section;
        if (section == 6) block++; /* Native eyeInfoCollection has a leading count. */
        if (!mc_is(in, block, "{") || in->tokens[block].close >= in->count - 1) return mc_fail(c, "section needs a complete native block");
        end = in->tokens[block].close; i = end + 1;
        if (!section) {
            sh_decl_node *list = mc_add(edit, mc_node(c, mc_sections[section], NULL));
            mc_init(c, in, list, block + 1, end);
        } else if (section >= 2 && section <= 4) {
            sh_decl_node *list = mc_add(edit, mc_node(c, mc_sections[section], NULL));
            mc_records(c, in, list, section, block + 1, end);
        } else if (section == 6) {
            sh_decl_node *list = mc_add(edit, mc_node(c, mc_sections[section], NULL));
            mc_eye_records(c, in, list, block + 1, end);
        } else if (section == 7) {
            sh_decl_node *list = mc_add(edit, mc_node(c, mc_sections[section], NULL));
            mc_kits(c, in, list, block + 1, end);
        } else {
            mc_slice(c, in, edit, mc_sections[section], start, i);
        }
    }
    if (!c->failed && seen && (seen & 61u) != 61u) mc_fail(c, "missing required init/jointGroups/events/aliases/props section");
    if (!c->failed) in->text = sh_decl_tree_write(in->tree, &in->length);
    if (!c->failed && !in->text) mc_fail(c, "normalization allocation failed");
    return !c->failed;
}
static int mc_append(mc_context *c, char **out, size_t *length, const char *text)
{
    size_t n = strlen(text); char *next;
    if (*length > SIZE_MAX - n - 1 || !(next = realloc(*out, *length + n + 1))) return mc_fail(c, "output allocation failed");
    memcpy(next + *length, text, n + 1); *length += n; *out = next; return 1;
}
static void mc_emit_slice(mc_context *c, char **out, size_t *length, const sh_decl_node *node)
{
    char *text = mc_unhex(c, node);
    if (text) { mc_append(c, out, length, text); mc_append(c, out, length, "\n"); }
    free(text);
}
static void mc_emit_alias(mc_context *c, char **out, size_t *length, const sh_decl_node *entry)
{
    const sh_decl_node *parts = sh_decl_tree_member(entry, "body");
    mc_emit_slice(c, out, length, sh_decl_tree_member(entry, "head"));
    for (const sh_decl_node *part = parts ? parts->children : NULL; part && !c->failed; part = part->next) {
        const sh_decl_node *flags;
        if (!strcmp(part->key, "num")) continue;
        flags = sh_decl_tree_member(part, "flags");
        if (!flags) mc_emit_slice(c, out, length, sh_decl_tree_member(part, "text"));
        else {
            mc_append(c, out, length, "flags {\n");
            for (const sh_decl_node *flag = flags->children; flag && !c->failed; flag = flag->next)
                if (strcmp(flag->key, "num")) mc_emit_slice(c, out, length, sh_decl_tree_member(flag, "body"));
            mc_append(c, out, length, "}\n");
        }
    }
    mc_append(c, out, length, "}\n");
}
static void mc_emit_keys(mc_context *c, char **out, size_t *length, const sh_decl_node *parent)
{
    const sh_decl_node *keys = sh_decl_tree_member(parent, "keys");
    for (const sh_decl_node *item = keys ? keys->children : NULL; item && !c->failed; item = item->next)
        if (strcmp(item->key, "num")) mc_emit_slice(c, out, length, sh_decl_tree_member(item, "body"));
}
static size_t mc_records_count(const sh_decl_node *list)
{
    size_t count = 0;
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next)
        count += strcmp(item->key, "num") != 0;
    return count;
}
/* A joint group re-emits its authored head, then its args blocks and joint
 * entries in composed order. The engine derives every joint index from that
 * order, so no index is written here. */
static void mc_emit_group(mc_context *c, char **out, size_t *length, const sh_decl_node *entry)
{
    const sh_decl_node *body = sh_decl_tree_member(entry, "records");
    if (!sh_decl_tree_member(entry, "head")) {
        mc_emit_slice(c, out, length, sh_decl_tree_member(entry, "body")); return;
    }
    mc_emit_slice(c, out, length, sh_decl_tree_member(entry, "head"));
    for (const sh_decl_node *record = body ? body->children : NULL; record && !c->failed; record = record->next) {
        if (!strcmp(record->key, "num")) continue;
        if (sh_decl_tree_member(record, "args")) {
            mc_append(c, out, length, "args {\n");
            mc_emit_keys(c, out, length, record);
            mc_append(c, out, length, "}\n");
        } else {
            char *name = mc_unhex(c, sh_decl_tree_member(record, "name"));
            if (name) mc_append(c, out, length, name);
            free(name);
            if (sh_decl_tree_member(record, "keys")) {
                mc_append(c, out, length, " {\n");
                mc_emit_keys(c, out, length, record);
                mc_append(c, out, length, "}\n");
            } else mc_append(c, out, length, "\n");
        }
    }
    mc_append(c, out, length, "}\n");
}
static void mc_emit_events(mc_context *c, char **out, size_t *length, const sh_decl_node *entry)
{
    const sh_decl_node *body = sh_decl_tree_member(entry, "records");
    mc_emit_slice(c, out, length, sh_decl_tree_member(entry, "head"));
    for (const sh_decl_node *record = body ? body->children : NULL; record && !c->failed; record = record->next) {
        if (!strcmp(record->key, "num")) continue;
        mc_emit_slice(c, out, length, sh_decl_tree_member(record, "head"));
        mc_emit_keys(c, out, length, record);
        mc_append(c, out, length, "}\n");
    }
    mc_append(c, out, length, "}\n");
}
/* Both counted collections are re-counted from the records that survive
 * composition; the native readers size their storage from these numbers. */
static void mc_emit_eye(mc_context *c, char **out, size_t *length, const sh_decl_node *node)
{
    char head[64];
    snprintf(head, sizeof(head), "eyeInfoCollection %zu {\n", mc_records_count(node));
    mc_append(c, out, length, head);
    for (const sh_decl_node *item = node->children; item && !c->failed; item = item->next)
        if (strcmp(item->key, "num")) mc_emit_slice(c, out, length, sh_decl_tree_member(item, "body"));
    mc_append(c, out, length, "}\n");
}
static void mc_emit_kits(mc_context *c, char **out, size_t *length, const sh_decl_node *node)
{
    mc_append(c, out, length, "meshKits {\n");
    for (const sh_decl_node *kit = node->children; kit && !c->failed; kit = kit->next) {
        const sh_decl_node *entries = sh_decl_tree_member(kit, "entries");
        char *name; char count[32];
        if (!strcmp(kit->key, "num")) continue;
        name = mc_unhex(c, sh_decl_tree_member(kit, "name"));
        if (name) { mc_append(c, out, length, name); mc_append(c, out, length, " "); }
        free(name);
        snprintf(count, sizeof(count), "%zu {\n", mc_records_count(entries));
        mc_append(c, out, length, count);
        for (const sh_decl_node *item = entries ? entries->children : NULL; item && !c->failed; item = item->next)
            if (strcmp(item->key, "num")) mc_emit_slice(c, out, length, sh_decl_tree_member(item, "body"));
        mc_append(c, out, length, "}\n");
    }
    mc_append(c, out, length, "}\n");
}
static char *mc_emit(mc_context *c, const sh_decl_node *root, size_t *length)
{
    const sh_decl_node *edit = sh_decl_tree_member(root, "edit");
    char *out = NULL;
    *length = 0; mc_append(c, &out, length, "{\n");
    for (size_t section = 0; edit && section < sizeof(mc_sections) / sizeof(mc_sections[0]) && !c->failed; section++) {
        const sh_decl_node *node = sh_decl_tree_member(edit, mc_sections[section]);
        if (!node) continue;
        if (section == 6) { mc_emit_eye(c, &out, length, node); continue; }
        if (section == 7) { mc_emit_kits(c, &out, length, node); continue; }
        if (!section || (section >= 2 && section <= 4)) {
            mc_append(c, &out, length, mc_sections[section]); mc_append(c, &out, length, " {\n");
            for (const sh_decl_node *entry = node->children; entry && !c->failed; entry = entry->next) {
                char *body;
                if (!strcmp(entry->key, "num")) continue;
                if (section == 2) { mc_emit_group(c, &out, length, entry); continue; }
                if (section == 3) { mc_emit_events(c, &out, length, entry); continue; }
                if (section == 4) { mc_emit_alias(c, &out, length, entry); continue; }
                body = mc_unhex(c, sh_decl_tree_member(entry, "body"));
                if (body) { mc_append(c, &out, length, body); mc_append(c, &out, length, "\n"); } free(body);
            }
            mc_append(c, &out, length, "}\n");
        } else {
            char *body = mc_unhex(c, node);
            if (body) { mc_append(c, &out, length, body); mc_append(c, &out, length, "\n"); } free(body);
        }
    }
    if (!c->failed) mc_append(c, &out, length, "}\n");
    if (c->failed) { free(out); out = NULL; *length = 0; }
    return out;
}
static int mc_same(const sh_decl_node *a, const sh_decl_node *b)
{
    char *x, *y; size_t xn, yn; int same;
    sh_decl_node av, bv;
    if (!a || !b) return a == b;
    if (a->compound != b->compound) return 0;
    if (!a->compound) return !strcmp(a->value, b->value);
    av = *a; bv = *b; av.key = bv.key = NULL; av.next = bv.next = NULL;
    x = sh_decl_tree_write(&av, &xn); y = sh_decl_tree_write(&bv, &yn);
    same = x && y && xn == yn && !memcmp(x, y, xn); free(x); free(y); return same;
}
static int mc_changed_state(const mc_input *a, const mc_input *b)
{
    const sh_decl_node *ae = sh_decl_tree_member(a->tree, "edit"), *be = sh_decl_tree_member(b->tree, "edit");
    for (size_t i = 1; i < sizeof(mc_sections) / sizeof(mc_sections[0]); i++)
        if (!mc_same(sh_decl_tree_member(ae, mc_sections[i]), sh_decl_tree_member(be, mc_sections[i]))) return 1;
    return 0;
}
static const sh_decl_node *mc_match_entry(const sh_decl_node *list, const sh_decl_node *entry)
{
    const sh_decl_node *id = sh_decl_tree_member(entry, "id");
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        const sh_decl_node *other = sh_decl_tree_member(item, "id");
        if (id && other && !strcmp(id->value, other->value)) return item;
    }
    return NULL;
}
/* The key items of one payload carry their native key name, so a semantic rule
 * can find the key it governs without reinterpreting any value. */
static const sh_decl_node *mc_key_item(mc_context *c, const sh_decl_node *record, const char *name)
{
    const sh_decl_node *keys = sh_decl_tree_member(record, "keys");
    for (const sh_decl_node *item = keys ? keys->children : NULL; item; item = item->next) {
        char *text;
        if (!strcmp(item->key, "num")) continue;
        text = mc_unhex(c, sh_decl_tree_member(item, "name"));
        if (text && !strcmp(text, name)) { free(text); return item; }
        free(text);
    }
    return NULL;
}
/* The args vec3 in force when the reader reaches one joint entry: it writes that
 * default into the joint it is listed before, so a composed order that moves a
 * joint across an args block would silently change its payload. */
static const sh_decl_node *mc_running_vec3(mc_context *c, const sh_decl_node *body,
    const char *joint_id, int *found)
{
    const sh_decl_node *vec3 = NULL;
    *found = 0;
    for (const sh_decl_node *record = body ? body->children : NULL; record; record = record->next) {
        const sh_decl_node *id;
        if (!strcmp(record->key, "num")) continue;
        if (sh_decl_tree_member(record, "args")) {
            const sh_decl_node *item = mc_key_item(c, record, "vec3");
            if (item) vec3 = sh_decl_tree_member(item, "body");
            continue;
        }
        id = sh_decl_tree_member(record, "id");
        if (id && joint_id && !strcmp(id->value, joint_id)) { *found = 1; return vec3; }
    }
    return vec3;
}
/* surfType and contentsFlags are single group-wide values wherever they appear,
 * so the last writer in body order is the one the engine keeps. */
static const sh_decl_node *mc_final_writer(mc_context *c, const sh_decl_node *body, const char *key)
{
    const sh_decl_node *writer = NULL;
    for (const sh_decl_node *record = body ? body->children : NULL; record; record = record->next) {
        const sh_decl_node *item;
        if (!strcmp(record->key, "num")) continue;
        item = mc_key_item(c, record, key);
        if (item) writer = sh_decl_tree_member(item, "body");
    }
    return writer;
}
/* "<category>:<name>" as the group list stores it. */
static char *mc_group_identity(mc_context *c, const sh_decl_node *group, int *category)
{
    const sh_decl_node *id = sh_decl_tree_member(group, "id");
    char *text = id ? mc_unhex(c, id) : NULL;
    const char *colon = text ? strchr(text, ':') : NULL;
    *category = -1;
    if (colon && colon != text) {
        int value = 0;
        for (const char *at = text; at < colon; at++) {
            if (*at < '0' || *at > '9') { value = -1; break; }
            value = value * 10 + (*at - '0');
        }
        *category = value;
    }
    return text;
}
static const sh_decl_node *mc_section_node(const mc_input *in, const char *section)
{ return sh_decl_tree_member(sh_decl_tree_member(in->tree, "edit"), section); }

/* Joint-group payload semantics beyond record identity: the running args default
 * each joint inherits and the single group-wide surfType/contentsFlags writer.
 * A contribution that changed either one must still see its own value in the
 * composed result, exactly as the init setters are checked. */
static int mc_group_semantics(mc_context *c, const mc_input *original, const mc_input *input,
    const mc_input *result, sh_decl_conflict *conflict, size_t index)
{
    const sh_decl_node *base = mc_section_node(original, "jointGroups");
    const sh_decl_node *mine = mc_section_node(input, "jointGroups");
    const sh_decl_node *out = mc_section_node(result, "jointGroups");
    for (const sh_decl_node *group = mine ? mine->children : NULL; group && !c->failed; group = group->next) {
        const sh_decl_node *original_group, *result_group, *body, *result_body;
        char detail[320], *name; int category;
        if (!strcmp(group->key, "num")) continue;
        original_group = mc_match_entry(base, group);
        result_group = mc_match_entry(out, group);
        if (!result_group) continue;
        body = sh_decl_tree_member(group, "records");
        result_body = sh_decl_tree_member(result_group, "records");
        name = mc_group_identity(c, group, &category);
        for (int k = 0; k < 2 && !c->failed; k++) {
            const char *key = k ? "contentsFlags" : "surfType";
            const sh_decl_node *was = mc_final_writer(c, sh_decl_tree_member(original_group, "records"), key);
            const sh_decl_node *contributed = mc_final_writer(c, body, key);
            const sh_decl_node *now = mc_final_writer(c, result_body, key);
            if (mc_same(was, contributed) || mc_same(contributed, now)) continue;
            snprintf(detail, sizeof(detail), "joint group '%s' %s write would be overwritten by another contribution",
                name ? name : "?", key);
            if (conflict) conflict->first = index;
            mc_fail(c, detail);
        }
        for (const sh_decl_node *record = body ? body->children : NULL; record && !c->failed; record = record->next) {
            const sh_decl_node *id = sh_decl_tree_member(record, "id"), *was, *now, *before;
            int found_input = 0, found_result = 0, found_original = 0;
            char *joint;
            if (!strcmp(record->key, "num") || sh_decl_tree_member(record, "args") || !id) continue;
            was = mc_running_vec3(c, body, id->value, &found_input);
            now = mc_running_vec3(c, result_body, id->value, &found_result);
            before = mc_running_vec3(c, sh_decl_tree_member(original_group, "records"),
                id->value, &found_original);
            if (!found_input || !found_result || mc_same(was, now)) continue;
            /* An entry the original already carried is only contended when this
             * contribution itself changed the default it inherits. */
            if (found_original && mc_same(before, was)) continue;
            joint = mc_unhex(c, sh_decl_tree_member(record, "name"));
            snprintf(detail, sizeof(detail),
                "joint group '%s' entry %s would inherit a different args default after composition",
                name ? name : "?", joint ? joint : "?");
            free(joint);
            if (conflict) conflict->first = index;
            mc_fail(c, detail);
        }
        free(name);
    }
    return !c->failed;
}
/* A limb-loss group's per-joint index is authored, not derived, so two entries
 * of one group may not claim the same index in the composed result. */
static int mc_group_indices(mc_context *c, const mc_input *result)
{
    const sh_decl_node *out = mc_section_node(result, "jointGroups");
    for (const sh_decl_node *group = out ? out->children : NULL; group && !c->failed; group = group->next) {
        const sh_decl_node *body;
        char *name; int category;
        if (!strcmp(group->key, "num")) continue;
        name = mc_group_identity(c, group, &category);
        body = category == 4 ? sh_decl_tree_member(group, "records") : NULL;
        for (const sh_decl_node *record = body ? body->children : NULL; record && !c->failed; record = record->next) {
            const sh_decl_node *item = mc_key_item(c, record, mc_group_scalars[4]);
            if (!item || !strcmp(record->key, "num")) continue;
            for (const sh_decl_node *other = record->next; other && !c->failed; other = other->next) {
                const sh_decl_node *rival = mc_key_item(c, other, mc_group_scalars[4]);
                char detail[320], *text;
                if (!rival || !mc_same(sh_decl_tree_member(item, "body"), sh_decl_tree_member(rival, "body")))
                    continue;
                text = mc_unhex(c, sh_decl_tree_member(item, "body"));
                snprintf(detail, sizeof(detail), "joint group '%s' has two entries claiming '%s'",
                    name ? name : "?", text ? text : "?");
                free(text); mc_fail(c, detail);
            }
        }
        free(name);
    }
    return !c->failed;
}
static int mc_alias_slots(mc_context *c, mc_input *in, const mc_input *baseline)
{
    const sh_decl_node *base = sh_decl_tree_member(sh_decl_tree_member(baseline->tree, "edit"), "aliases");
    sh_decl_node *aliases = (sh_decl_node *)sh_decl_tree_member(sh_decl_tree_member(in->tree, "edit"), "aliases");
    for (sh_decl_node *alias = aliases ? aliases->children : NULL; alias && !c->failed; alias = alias->next) {
        const sh_decl_node *original, *original_parts;
        sh_decl_node *parts; size_t extent = 0, index = 0;
        if (!strcmp(alias->key, "num")) continue;
        original = mc_match_entry(base, alias);
        original_parts = sh_decl_tree_member(original, "body");
        for (const sh_decl_node *part = original_parts ? original_parts->children : NULL; part; part = part->next)
            extent += sh_decl_tree_member(part, "text") != NULL;
        parts = (sh_decl_node *)sh_decl_tree_member(alias, "body");
        for (sh_decl_node *part = parts ? parts->children : NULL; part && !c->failed; part = part->next) {
            sh_decl_node *id; char *encoded = NULL;
            if (!sh_decl_tree_member(part, "text")) continue;
            id = (sh_decl_node *)sh_decl_tree_member(part, "id");
            /* Existing native slots remain positional: two edits of one slot
             * must conflict rather than becoming two new random variants.
             * Appended slots use value/occurrence identity to retain independent
             * additions, including intentional duplicates, from every owner. */
            if (index++ < extent) {
                char key[48]; snprintf(key, sizeof(key), "slot:%zu", index - 1);
                encoded = mc_hex(c, key, strlen(key));
            } else {
                char *value = mc_unhex(c, sh_decl_tree_member(part, "value")), *key = NULL;
                size_t n = value ? strlen(value) : 0, occurrence = 0;
                if (value) key = n <= SIZE_MAX - 40 ? malloc(n + 40) : NULL;
                if (!key) mc_fail(c, "animation slot allocation failed");
                while (key && !c->failed) {
                    snprintf(key, n + 40, "append:%s:%zu", value, occurrence++);
                    encoded = mc_hex(c, key, strlen(key));
                    if (!encoded || !mc_has_id(parts, encoded)) break;
                    free(encoded); encoded = NULL;
                }
                free(key); free(value);
            }
            if (encoded) { free(id->value); id->value = encoded; }
        }
    }
    if (!c->failed) {
        free(in->text); in->text = sh_decl_tree_write(in->tree, &in->length);
        if (!in->text) mc_fail(c, "alias slot projection allocation failed");
    }
    return !c->failed;
}
static void mc_free(mc_input *in)
{ free(in->tokens); free(in->binding); free(in->text); sh_decl_tree_free(in->tree); }

/* An animation event argument is written as its native type name followed by one
 * value. The frame-command reader resolves that type name through the engine's
 * declaration-type registry and then looks the value up in that type's manager,
 * so an argument whose type is a declaration family is a dependency of this
 * definition. These are the reader's own primitive and skeleton-resolved forms,
 * which name no resource. */
static const char *mc_event_primitives[] = {"frame", "row", "locked", "bool", "int", "float",
    "vec3", "angles", "string", "animalias", "joint", "jointName"};
static const char *mc_declaration_family(const mc_input *in, size_t i)
{
    for (size_t n = 0; n < sizeof(mc_event_primitives) / sizeof(mc_event_primitives[0]); n++)
        if (mc_is_ci(in, i, mc_event_primitives[n])) return NULL;
    for (size_t n = 0; n < sizeof(SH_RESOURCE_TYPES) / sizeof(SH_RESOURCE_TYPES[0]); n++)
        if (mc_is_ci(in, i, SH_RESOURCE_TYPES[n].type)) return SH_RESOURCE_TYPES[n].type;
    return NULL;
}
/* The value token of such an argument, quoted or bare. */
static char *mc_argument_identity(mc_context *c, const mc_input *in, size_t i)
{
    const char *s; size_t n;
    if (i >= in->count) { mc_fail(c, "event argument has no value"); return NULL; }
    s = in->source.text + in->tokens[i].begin; n = in->tokens[i].end - in->tokens[i].begin;
    if (n >= 2 && *s == '"' && s[n - 1] == '"') { s++; n -= 2; }
    if (!n) { mc_fail(c, "event argument names an empty identity"); return NULL; }
    for (size_t j = 0; j < n; j++) if ((unsigned char)s[j] < 32) {
        mc_fail(c, "event argument identity contains a control byte"); return NULL;
    }
    return mc_copy(c, s, n);
}
/* Resource identities this envelope names, in the reader's own terms: the
 * inherited definition, the bound mesh, each alias animation and every
 * declaration an animation event argument resolves. Mesh kits and joint groups
 * name native records of the selected model, not resources. */
static int mc_emit_reference(mc_context *c, sh_decl_md6_reference_visitor visitor,
    void *context, const char *type, char *name)
{
    int ok = name && visitor(context, type, name);
    if (name && !ok) mc_fail(c, "reference visitor refused an identity");
    free(name); return ok;
}
int sh_decl_md6_references(sh_decl_source source, sh_decl_md6_reference_visitor visitor,
    void *context, char *error, size_t capacity)
{
    mc_context c = {error, capacity, 0};
    mc_input in = {0};
    size_t i = 1;
    if (error && capacity) error[0] = 0;
    if (!visitor) return 0;
    in.source = source;
    if (!mc_tokenize(&c, &in)) goto done;
    if (!mc_is(&in, 0, "{") || in.tokens[0].close != in.count - 1) {
        mc_fail(&c, "expected one complete MD6 definition"); goto done;
    }
    while (i + 1 < in.count && !c.failed) {
        size_t start = i++, block = i, end;
        int section = -1;
        for (int n = 0; n < (int)(sizeof(mc_sections) / sizeof(mc_sections[0])); n++)
            if (mc_is(&in, start, mc_sections[n])) { section = n; break; }
        if (section < 0) { mc_fail(&c, "unknown MD6 section"); break; }
        if (section == 6) block++;   /* eyeInfoCollection has a leading count */
        if (!mc_is(&in, block, "{") || in.tokens[block].close >= in.count - 1) {
            mc_fail(&c, "section needs a complete native block"); break;
        }
        end = in.tokens[block].close; i = end + 1;
        if (section == 0) {
            size_t f = block + 1;
            while (f < end && !c.failed) {
                if (mc_is(&in, f, "inherit")) {
                    if (!mc_is(&in, f + 1, "\"\"") &&
                        !mc_emit_reference(&c, visitor, context, "md6def", mc_identity(&c, &in, f + 1, 0))) break;
                    f += 2;
                } else if (mc_is(&in, f, "mesh")) {
                    if (!mc_emit_reference(&c, visitor, context, "basemodel", mc_identity(&c, &in, f + 1, 0))) break;
                    f += 2;
                } else if (mc_is(&in, f, "offset")) {
                    if (!mc_is(&in, f + 1, "(") || in.tokens[f + 1].close >= end) { mc_fail(&c, "invalid init offset"); break; }
                    f = in.tokens[f + 1].close + 1;
                } else if (mc_is(&in, f, "calcRefBoundsFromJoints")) {
                    f += 2;
                } else { mc_fail(&c, "unsupported init field"); break; }
            }
        } else if (section == 3) {
            size_t a = block + 1;
            while (a < end && !c.failed) {
                size_t close;
                if (!mc_is(&in, a, "anim") || !mc_is(&in, a + 2, "{") ||
                    (close = in.tokens[a + 2].close) >= end) {
                    mc_fail(&c, "animation event block is not native"); break;
                }
                for (size_t e = a + 3; e < close && !c.failed; ) {
                    size_t record;
                    if (!mc_is(&in, e, "event") || !mc_is(&in, e + 2, "{") ||
                        (record = in.tokens[e + 2].close) > close) {
                        mc_fail(&c, "animation event record is not native"); break;
                    }
                    {
                        static const mc_key_rules rules = {"", 1, 0};
                        size_t k = e + 3;
                        while (k + 1 < record && !c.failed) {
                            size_t stop = mc_value_extent(&c, &in, &rules, k, record);
                            const char *family = mc_declaration_family(&in, k);
                            if (c.failed || stop <= k + 1) break;
                            /* The reader skips the lookup for an empty name, and
                             * shipped content uses that to mean no declaration. */
                            if (family && !mc_is(&in, k + 1, "\"\"") &&
                                !mc_emit_reference(&c, visitor, context, family,
                                    mc_argument_identity(&c, &in, k + 1))) break;
                            k = stop;
                        }
                    }
                    e = record + 1;
                }
                a = close + 1;
            }
        } else if (section == 4) {
            size_t a = block + 1;
            while (a < end && !c.failed) {
                size_t body;
                if (!mc_is(&in, a, "alias") || !mc_is(&in, a + 1, "{") || in.tokens[a + 1].close >= end) {
                    mc_fail(&c, "alias must begin with its native block"); break;
                }
                body = in.tokens[a + 1].close;
                for (size_t t = a + 2; t < body; t++) if (mc_is(&in, t, "anim")) {
                    if (!mc_emit_reference(&c, visitor, context, "anim",
                            mc_animation_identity(&c, &in, t + 1))) break;
                }
                a = body + 1;
            }
        }
    }
done:
    mc_free(&in);
    return !c.failed;
}
char *sh_decl_md6_compose(sh_decl_source baseline, const sh_decl_source *sources, size_t count,
    size_t *length, char *error, size_t capacity, sh_decl_conflict *conflict)
{
    static const sh_decl_collection_rule rules[] = {{"edit.init", "id"}, {"edit.jointGroups", "id"},
        {"edit.jointGroups.item[*].records", "id"},
        {"edit.jointGroups.item[*].records.item[*].keys", "id"},
        {"edit.events", "id"}, {"edit.events.item[*].records", "id"},
        {"edit.events.item[*].records.item[*].keys", "id"},
        {"edit.aliases", "id"}, {"edit.aliases.item[*].body", "id"},
        {"edit.aliases.item[*].body.item[*].flags", "id"},
        {"edit.eyeInfoCollection", "id"}, {"edit.meshKits", "id"},
        {"edit.meshKits.item[*].entries", "id"}};
    mc_context c = {error, capacity, 0};
    mc_input original = {0}, result = {0}, *inputs = NULL;
    sh_decl_source *views = NULL;
    sh_decl_node *merged = NULL;
    char *text = NULL, *out = NULL; size_t n = 0;
    if (length) *length = 0;
    if (error && capacity) *error = 0;
    if (conflict) *conflict = (sh_decl_conflict){SIZE_MAX, SIZE_MAX};
    if ((!sources && count) || count > SIZE_MAX / sizeof(*inputs) || count > SIZE_MAX / sizeof(*views)) { mc_fail(&c, "invalid input count"); goto done; }
    inputs = calloc(count ? count : 1, sizeof(*inputs)); views = calloc(count ? count : 1, sizeof(*views));
    if (!inputs || !views) { mc_fail(&c, "allocation failed"); goto done; }
    original.source = baseline;
    if (!mc_parse(&c, &original)) goto done;
    for (size_t i = 0; i < count; i++) {
        inputs[i].source = sources[i];
        if (!mc_parse(&c, &inputs[i])) { if (conflict) conflict->first = i; goto done; }
    }
    if (!mc_alias_slots(&c, &original, &original)) goto done;
    for (size_t i = 0; i < count; i++) {
        if (!mc_alias_slots(&c, &inputs[i], &original)) { if (conflict) conflict->first = i; goto done; }
        views[i] = (sh_decl_source){inputs[i].text, inputs[i].length};
    }
    text = sh_decl_compose((sh_decl_source){original.text, original.length}, views, count,
        rules, sizeof(rules) / sizeof(rules[0]), &n, error, capacity, conflict);
    if (!text) goto done;
    merged = sh_decl_tree_parse((sh_decl_source){text, n}, error, capacity);
    if (!merged) goto done;
    out = mc_emit(&c, merged, &n);
    if (!out) goto done;
    result.source = (sh_decl_source){out, n};
    if (!mc_parse(&c, &result)) goto done;
    for (size_t i = 0; i < count; i++) for (size_t w = 0; w < 5; w++) {
        if (!mc_same(original.init_writers[w], inputs[i].init_writers[w]) &&
            !mc_same(inputs[i].init_writers[w], result.init_writers[w])) {
            static const char *values[] = {"parent name", "parent", "model", "offset", "bounds flag"};
            char detail[160];
            if (conflict) conflict->first = i;
            snprintf(detail, sizeof(detail), "init write for %s would be overwritten by another contribution", values[w]);
            mc_fail(&c, detail); goto done;
        }
    }
    for (size_t i = 0; i < count; i++)
        if (!mc_group_semantics(&c, &original, &inputs[i], &result, conflict, i)) goto done;
    if (!mc_group_indices(&c, &result)) goto done;
    /* A group/animation edit was authored against a selected model/parent.
     * Do not transplant it onto another contribution's different binding.
     * Resolving changes inside that referenced parent/skeleton remains a native
     * dependency/activation responsibility, not proof supplied by this envelope. */
    for (size_t i = 0; i < count; i++) if (mc_changed_state(&original, &inputs[i])) {
        const char *a = inputs[i].binding ? inputs[i].binding : "", *b = result.binding ? result.binding : "";
        if (strcmp(a, b)) {
            if (conflict) conflict->first = i;
            mc_fail(&c, "model or parent binding changed underneath a contributed MD6 record"); goto done;
        }
    }
    if (length) *length = n;
done:
    if (c.failed) { free(out); out = NULL; }
    free(text); sh_decl_tree_free(merged); mc_free(&original); mc_free(&result);
    if (inputs) for (size_t i = 0; i < count; i++) mc_free(&inputs[i]);
    free(inputs); free(views); return out;
}
