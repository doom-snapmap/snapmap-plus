/* script_node_row.c -- the Script node's Toybox row and the names the
 * builder and the serving index ask about (see script_node.h, "the row").
 * No Lua and no engine: the builder, the store tool and the serving index
 * link it without the script runtime. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_node.h"
#include "script_runtime.h"

static void copyz(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (c == '\\' ? '/' : c); }

static int ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int valid_salt(const char *s)
{
    size_t n = s ? strlen(s) : 0, i;
    if (!n || n >= SH_SCRIPT_SALT_CAP) return 0;
    for (i = 0; i < n; i++) if (!ident_char(s[i]) && s[i] != '-') return 0;
    return 1;
}

/* ================================================================ the row */

/* The level of a long bracket "[" "="* "[" at p, or -1. */
static int long_open(const char *p, const char *end)
{
    int level = 0;
    if (p >= end || *p != '[') return -1;
    p++;
    while (p < end && *p == '=') { level++; p++; }
    return (p < end && *p == '[') ? level : -1;
}

/* Past the long bracket closing `level` from p, or end. */
static const char *long_close(const char *p, const char *end, int level)
{
    for (; p < end; p++) {
        int k = 0;
        if (*p != ']') continue;
        while (p + 1 + k < end && p[1 + k] == '=') k++;
        if (k == level && p + 1 + k < end && p[1 + k] == ']') return p + 2 + k;
    }
    return end;
}

int sh_script_node_registers_input(const char *text, size_t len)
{
    const char *p = text, *end = text ? text + len : NULL;
    char prev = 0;                 /* the last significant character before the current token */
    if (!text) return 0;
    while (p < end) {
        char c = *p;
        if (c == '-' && p + 1 < end && p[1] == '-') {
            int level = long_open(p + 2, end);
            if (level >= 0) p = long_close(p + 2, end, level);
            else { while (p < end && *p != '\n') p++; }
            continue;
        }
        if (c == '"' || c == '\'') {
            p++;
            while (p < end && *p != c && *p != '\n') { if (*p == '\\' && p + 1 < end) p++; p++; }
            if (p < end) p++;
            prev = '"';
            continue;
        }
        if (c == '[' && long_open(p, end) >= 0) {
            p = long_close(p, end, long_open(p, end));
            prev = '"';
            continue;
        }
        if (ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *s = p;
            while (p < end && ident_char(*p)) p++;
            if (p - s == 2 && s[0] == 'o' && s[1] == 'n' && prev != '.' && prev != ':') {
                const char *q = p;
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
                if (q < end && *q == '.') {
                    q++;
                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
                    if (end - q >= 5 && !memcmp(q, "input", 5) && (q + 5 == end || !ident_char(q[5]))) {
                        q += 5;
                        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
                        if (q < end && (*q == '(' || *q == '"' || *q == '\'' || *q == '{')) return 1;
                    }
                }
            }
            prev = 'a';
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') prev = c;
        p++;
    }
    return 0;
}

int sh_script_node_is_def(const char *name, char *salt_out, size_t cap)
{
    const char *s, *slash;
    size_t n;
    if (salt_out && cap) salt_out[0] = 0;
    if (!name || _strnicmp(name, "mods", 4) || (name[4] != '/' && name[4] != '\\')) return 0;
    s = name + 5;
    slash = s;
    while (*slash && *slash != '/' && *slash != '\\') slash++;
    n = (size_t)(slash - s);
    if (!n || n >= SH_SCRIPT_SALT_CAP || !*slash) return 0;
    if (_stricmp(slash + 1, SH_SCRIPT_NODE_LEAF)) return 0;
    if (salt_out && cap) {
        if (n >= cap) n = cap - 1;
        memcpy(salt_out, s, n);
        salt_out[n] = 0;
    }
    return 1;
}

static int path_eq(const char *a, const char *b)
{
    while (*a && *b) { if (fold(*a) != fold(*b)) return 0; a++; b++; }
    return !*a && !*b;
}

int sh_script_node_reserved(const char *salt, const char *request)
{
    char a[160], b[160];
    if (!valid_salt(salt) || !request) return 0;
    _snprintf_s(a, sizeof a, _TRUNCATE, "generated/decls/entitydef/mods/%s/" SH_SCRIPT_NODE_LEAF ".decl", salt);
    _snprintf_s(b, sizeof b, _TRUNCATE, "generated/decls/snapeditorentitydef/mods/%s/" SH_SCRIPT_NODE_LEAF ".decl", salt);
    return path_eq(request, a) || path_eq(request, b);
}

static char *fmt_alloc(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    va_start(ap, fmt);
    vsprintf_s(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/* The entityDef: the stock Relay, nothing changed. */
static const char k_def[] =
    "{\r\n"
    "\tinherit = \"" SH_SCRIPT_NODE_DEF_PARENT "\";\r\n"
    "\tedit = {\r\n"
    "\t}\r\n"
    "}\r\n";

/* The row: the stock Relay row with the mod's name, category and entityDef.
 * Its property sheet keeps only the name (Run Once is the Relay's and means
 * nothing to a script). %1 salt, %2 category tag. */
static const char k_row[] =
    "{\r\n"
    "\tinherit = \"" SH_SCRIPT_NODE_ROW_PARENT "\";\r\n"
    "\tedit = {\r\n"
    "\t\tdisplayNameTag = \"#str_%s_script_node\";\r\n"
    "\t\tdisplayDescriptionTag = \"#str_%s_script_node_desc\";\r\n"
    "\t\tcategoryNameTag = \"%s\";\r\n"
    "\t\tentityDef = \"mods/%s/" SH_SCRIPT_NODE_LEAF "\";\r\n"
    "\t\tpropertySheets = {\r\n"
    "\t\t\tnum = 1;\r\n"
    "\t\t\titem[0] = {\r\n"
    "\t\t\t\tproperties = {\r\n"
    "\t\t\t\t\tnum = 1;\r\n"
    "\t\t\t\t\titem[0] = {\r\n"
    "\t\t\t\t\t\tpath = \"displayName\";\r\n"
    "\t\t\t\t\t\tdisplayNameTag = \"#str_snapproperty_variable_name\";\r\n"
    "\t\t\t\t\t\tdescriptionTag = \"#str_snapproperty_variable_name_desc\";\r\n"
    "\t\t\t\t\t\tinspector = \"display_name_inspector\";\r\n"
    "\t\t\t\t\t}\r\n"
    "\t\t\t\t}\r\n"
    "\t\t\t}\r\n"
    "\t\t}\r\n"
    "\t}\r\n"
    "}\r\n";

void sh_script_node_row_free(sh_script_node_row *row)
{
    int i;
    if (!row) return;
    free(row->def_text);
    free(row->row_text);
    for (i = 0; i < SH_SCRIPT_NODE_ROW_STRINGS; i++) free(row->string_text[i]);
    memset(row, 0, sizeof *row);
}

int sh_script_node_row_make(const char *salt, const char *mod_name, const char *category_tag,
                            sh_script_node_row *out)
{
    char category[96];
    int own_category = !category_tag || !category_tag[0];
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!valid_salt(salt)) return 0;
    if (!own_category && (strlen(category_tag) >= sizeof category || strchr(category_tag, '"'))) own_category = 1;
    if (own_category) _snprintf_s(category, sizeof category, _TRUNCATE, "#str_%s_script_category", salt);
    else copyz(category, sizeof category, category_tag);
    _snprintf_s(out->def_path, sizeof out->def_path, _TRUNCATE,
                "generated/decls/entitydef/mods/%s/" SH_SCRIPT_NODE_LEAF ".decl", salt);
    _snprintf_s(out->row_path, sizeof out->row_path, _TRUNCATE,
                "generated/decls/snapeditorentitydef/mods/%s/" SH_SCRIPT_NODE_LEAF ".decl", salt);
    out->def_text = fmt_alloc("%s", k_def);
    out->row_text = fmt_alloc(k_row, salt, salt, category, salt);
    _snprintf_s(out->string_key[0], sizeof out->string_key[0], _TRUNCATE, "%s_script_node", salt);
    out->string_text[0] = fmt_alloc("Script");
    _snprintf_s(out->string_key[1], sizeof out->string_key[1], _TRUNCATE, "%s_script_node_desc", salt);
    out->string_text[1] = fmt_alloc("Runs this mod's script. Name the node after an input the script handles; "
                                    "the script can also fire this node's outputs by its name.");
    out->nstrings = 2;
    if (own_category) {
        _snprintf_s(out->string_key[2], sizeof out->string_key[2], _TRUNCATE, "%s_script_category", salt);
        out->string_text[2] = fmt_alloc("%s", mod_name && mod_name[0] ? mod_name : salt);
        out->nstrings = 3;
    }
    if (!out->def_text || !out->row_text || !out->string_text[0] || !out->string_text[1] ||
        (own_category && !out->string_text[2])) {
        sh_script_node_row_free(out);
        return 0;
    }
    return 1;
}
