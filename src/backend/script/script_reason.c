/* script_reason.c -- see script_reason.h. Every rule below matches the
 * wording of the vendored Lua (src/third_party/lua: ldebug.c, lauxlib.c,
 * llex.c, lparser.c, lstrlib.c, ldo.c); the tests feed each one. */
#include "script_reason.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

static void put(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!cap) return;
    va_start(ap, fmt);
    _vsnprintf_s(out, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
    out[cap - 1] = 0;
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

const char *sh_script_split_where(const char *msg, char *file, size_t file_cap, int *line)
{
    const char *colon;
    size_t n;
    int v = 0;
    if (file_cap) file[0] = 0;
    *line = 0;
    if (!msg || !starts(msg, "scripts\\")) return msg ? msg : "";
    colon = strstr(msg, ".lua:");
    if (!colon) return msg;
    colon += 4;
    n = (size_t)(colon - msg);
    if (colon[1] == ' ') {                     /* "scripts\x.lua: text": a file, no line */
        if (n >= file_cap) n = file_cap ? file_cap - 1 : 0;
        if (file_cap) { memcpy(file, msg, n); file[n] = 0; }
        return colon + 2;
    }
    {
        const char *d = colon + 1;
        if (*d < '0' || *d > '9') return msg;
        while (*d >= '0' && *d <= '9') { if (v < 100000000) v = v * 10 + (*d - '0'); d++; }
        if (*d != ':') return msg;
        d++;
        if (*d == ' ') d++;
        if (n >= file_cap) n = file_cap ? file_cap - 1 : 0;
        if (file_cap) { memcpy(file, msg, n); file[n] = 0; }
        *line = v;
        return d;
    }
}

/* "nil" -> "nothing" and so on: the type names Lua prints, including the
 * __name of the runtime's own objects. */
static const char *a_type(const char *t, size_t n)
{
    static const struct { const char *lua, *plain; } names[] = {
        { "nil", "nothing" }, { "no value", "nothing" }, { "boolean", "true or false" },
        { "number", "a number" }, { "string", "text" }, { "table", "a table" },
        { "function", "a function" }, { "userdata", "a game value" }, { "thread", "a coroutine" },
        { "entity", "an entity" }, { "timer", "a timer" }, { "game name", "a game name" },
        { "light userdata", "a game value" }, { "integer", "a whole number" },
    };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i].lua) == n && strncmp(names[i].lua, t, n) == 0) return names[i].plain;
    return NULL;
}

/* The variable a Lua message names at its end: " (global 'x')", " (field
 * 'x')" and the other kinds ldebug.c's varinfo writes. Returns 1 with the
 * name and the message cut before it. */
static int var_name(const char *text, size_t *body_len, char *name, size_t cap, int *is_method)
{
    static const char *kinds[] = { "global", "local", "field", "method", "upvalue", "constant", "for iterator" };
    size_t len = strlen(text), i;
    const char *open, *q1, *q2;
    *is_method = 0;
    if (len < 6 || text[len - 1] != ')' || text[len - 2] != '\'') return 0;
    open = NULL;
    for (i = len; i > 0; i--) if (text[i - 1] == '(' && i >= 2 && text[i - 2] == ' ') { open = text + i - 1; break; }
    if (!open) return 0;
    q1 = strchr(open, '\'');
    if (!q1) return 0;
    q2 = text + len - 2;
    if (q2 <= q1) return 0;
    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        size_t k = strlen(kinds[i]);
        if ((size_t)(q1 - open - 1) == k + 1 && strncmp(open + 1, kinds[i], k) == 0) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= cap) n = cap - 1;
            memcpy(name, q1 + 1, n);
            name[n] = 0;
            *body_len = (size_t)(open - 1 - text);
            *is_method = (i == 3);
            return strcmp(name, "?") != 0;   /* Lua's "?" names nothing */
        }
    }
    return 0;
}

/* Lua's quoted token ('end', <eof>) as an author reads it. */
static void token(const char *t, char *out, size_t cap)
{
    size_t n = strlen(t);
    if (strcmp(t, "<eof>") == 0) { put(out, cap, "the end of the file"); return; }
    if (n >= 2 && t[0] == '\'' && t[n - 1] == '\'') { put(out, cap, "\"%.*s\"", (int)(n - 2), t + 1); return; }
    put(out, cap, "%s", t);
}

/* Single quotes Lua puts around names, as double quotes. */
static void requote(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) out[o++] = (*s == '\'') ? '"' : *s;
    out[o] = 0;
}

static const char *ordinal(int n)
{
    static const char *w[] = { "", "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth" };
    return (n >= 1 && n <= 8) ? w[n] : NULL;
}

/* "bad argument #2 to 'insert' (position out of bounds)" and the self form. */
static int bad_argument(const char *text, char *what, size_t wcap, char *todo, size_t tcap)
{
    int n = 0;
    const char *p, *fn, *fn_end, *inner;
    char fname[64], pos[32], got[48];
    const char *exp_end, *got_s;
    size_t inner_len;
    int self = 0;
    if (starts(text, "bad argument #")) {
        p = text + 14;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; if (n > 1000) return 0; }
        if (!starts(p, " to '")) return 0;
        fn = p + 5;
    } else if (starts(text, "calling '")) {
        fn = text + 9;
        self = 1;
    } else return 0;
    fn_end = strchr(fn, '\'');
    if (!fn_end) return 0;
    put(fname, sizeof fname, "%.*s", (int)(fn_end - fn), fn);
    inner = fn_end + 1;
    if (self) {
        if (!starts(inner, " on bad self")) return 0;
        inner += 12;
    }
    if (!starts(inner, " (")) inner = "";
    else inner += 2;
    inner_len = strlen(inner);
    if (inner_len && inner[inner_len - 1] == ')') inner_len--;
    put(todo, tcap, "Check the value you pass");
    if (strcmp(fname, "for iterator") == 0 || strcmp(fname, "?") == 0) {
        put(what, wcap, "a \"for\" loop was given %s to go through",
            (strstr(inner, "got nil") || strstr(inner, "got no value")) ? "nothing" : "something it cannot go through");
        put(todo, tcap, "Check the table you loop over");
        return 1;
    }
    if (self) put(pos, sizeof pos, "the value before the colon");
    else if (ordinal(n)) put(pos, sizeof pos, "its %s value", ordinal(n));
    else put(pos, sizeof pos, "its value number %d", n);
    /* "<type> expected, got <type>" */
    exp_end = strstr(inner, " expected, got ");
    if (exp_end && exp_end < inner + inner_len) {
        const char *want = a_type(inner, (size_t)(exp_end - inner));
        got_s = exp_end + 15;
        {
            size_t gl = (size_t)(inner + inner_len - got_s);
            const char *g = a_type(got_s, gl);
            if (gl == 3 && strncmp(got_s, "nil", 3) == 0) g = "nil";
            put(got, sizeof got, "%s", g ? g : "something else");
        }
        if (want) put(what, wcap, "\"%s\" needs %s as %s, got %s", fname, want, pos, got);
        else put(what, wcap, "\"%s\" needs %.*s as %s, got %s", fname, (int)(exp_end - inner), inner, pos, got);
        return 1;
    }
    if (strncmp(inner, "value expected", 14) == 0) {
        put(what, wcap, "\"%s\" needs a value as %s, got nothing", fname, pos);
        return 1;
    }
    if (strncmp(inner, "number has no integer representation", 36) == 0) {
        put(what, wcap, "\"%s\" needs a whole number as %s", fname, pos);
        return 1;
    }
    if (strncmp(inner, "invalid option '", 16) == 0) {
        put(what, wcap, "\"%s\" does not know the option \"%.*s\"", fname,
            (int)(inner_len > 17 ? inner_len - 17 : 0), inner + 16);
        return 1;
    }
    {
        char r[160];
        put(r, sizeof r, "%.*s", (int)inner_len, inner);
        requote(r, r, sizeof r);
        if (inner_len) put(what, wcap, "\"%s\" cannot use %s (%s)", fname, pos, r);
        else put(what, wcap, "\"%s\" cannot use %s", fname, pos);
    }
    return 1;
}

/* "attempt to <op> a <type> value (<kind> '<name>')" and the compare forms. */
static int attempt(const char *text, char *what, size_t wcap, char *todo, size_t tcap)
{
    static const struct { const char *op, *nothing, *other; } ops[] = {
        { "index",                     "\"%s\" is nothing here",                                "\"%s\" is %s, not a table" },
        { "call",                      "\"%s\" is not a function here (it is nothing)",         "\"%s\" is %s, not a function" },
        { "perform arithmetic on",     "\"%s\" is nothing here, so it cannot be used in arithmetic", "\"%s\" is %s, so it cannot be used in arithmetic" },
        { "concatenate",               "\"%s\" is nothing here, so it cannot be joined to text", "\"%s\" is %s, so it cannot be joined to text" },
        { "perform bitwise operation on", "\"%s\" is nothing here, so it cannot be used in a bitwise operation", "\"%s\" is %s, so it cannot be used in a bitwise operation" },
        { "get length of",             "\"%s\" is nothing here, so it has no length",          "\"%s\" is %s, so it has no length" },
        { "close",                     "\"%s\" is nothing here, so it cannot be closed",       "\"%s\" is %s, so it cannot be closed" },
    };
    size_t i;
    const char *rest;
    if (!starts(text, "attempt to ")) return 0;
    rest = text + 11;
    if (starts(rest, "compare ")) {
        const char *c = rest + 8;
        put(todo, tcap, "Check both values before comparing them");
        if (starts(c, "two ")) {
            const char *v = strstr(c + 4, " values");
            const char *t = v ? a_type(c + 4, (size_t)(v - c - 4)) : NULL;
            put(what, wcap, "two values that are %s were compared with < or >", t ? t : "not numbers or text");
            return 1;
        } else {
            const char *w = strstr(c, " with ");
            if (w) {
                const char *a = a_type(c, (size_t)(w - c));
                const char *b = a_type(w + 6, strlen(w + 6));
                put(what, wcap, "%s was compared with %s using < or >", a ? a : "a value", b ? b : "another value");
                return 1;
            }
        }
        return 0;
    }
    if (starts(rest, "perform 'n//0'") || starts(rest, "perform 'n%0'")) {
        put(what, wcap, "a whole number was divided by zero");
        put(todo, tcap, "Check the divisor before dividing");
        return 1;
    }
    if (starts(rest, "divide by zero")) {
        put(what, wcap, "a whole number was divided by zero");
        put(todo, tcap, "Check the divisor before dividing");
        return 1;
    }
    if (starts(rest, "load a binary chunk")) {
        put(what, wcap, "the file is compiled Lua, not script text, so it did not run");
        put(todo, tcap, "Put the .lua source text in the file");
        return 1;
    }
    if (starts(rest, "yield")) {
        put(what, wcap, "\"wait\" or coroutine.yield was used outside run(...)");
        put(todo, tcap, "Put the code that waits inside run(function() ... end)");
        return 1;
    }
    if (starts(rest, "assign to const variable '")) {
        char n[64];
        const char *s = rest + 26, *e = strchr(s, '\'');
        put(n, sizeof n, "%.*s", e ? (int)(e - s) : 0, s);
        put(what, wcap, "\"%s\" is a constant and cannot be changed", n);
        put(todo, tcap, "Remove <const> or use another variable");
        return 1;
    }
    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        size_t k = strlen(ops[i].op);
        const char *t, *v;
        char name[80], body[160];
        size_t body_len = 0;
        int is_method = 0, named;
        if (strncmp(rest, ops[i].op, k) != 0 || strncmp(rest + k, " a ", 3) != 0) continue;
        t = rest + k + 3;
        v = strstr(t, " value");
        if (!v) return 0;
        named = var_name(text, &body_len, name, sizeof name, &is_method);
        (void)body;
        {
            const char *plain = a_type(t, (size_t)(v - t));
            int is_nil = (size_t)(v - t) == 3 && strncmp(t, "nil", 3) == 0;
            if (!named) put(name, sizeof name, "%s", "a value used here");
            if (i == 1 && is_method) {
                put(what, wcap, is_nil ? "\"%s\" is not something this value can do" : "\"%s\" is %s, not something this value can do",
                    name, plain ? plain : "something else");
                put(todo, tcap, "Check the name of the call");
                return 1;
            }
            if (!named) {
                /* "a value used here is nothing here" reads badly: word it whole. */
                if (is_nil) put(what, wcap, "%s", i == 0 ? "a value used here as a table is nothing" :
                                               i == 1 ? "a value called here as a function is nothing" :
                                               "a value used here is nothing");
                else put(what, wcap, "a value used here is %s, which cannot be used this way", plain ? plain : "something else");
            } else if (is_nil) put(what, wcap, ops[i].nothing, name);
            else put(what, wcap, ops[i].other, name, plain ? plain : "something else");
            put(todo, tcap, i == 1 ? "Check the spelling of the name" : "Check that it is set before it is used");
            return 1;
        }
    }
    return 0;
}

/* The parser's and lexer's messages: "<msg> near <token>". */
static int syntax(const char *text, char *what, size_t wcap, char *todo, size_t tcap)
{
    const char *near = strstr(text, " near ");
    char tok[96], head[200];
    size_t hl;
    if (!near) {
        if (starts(text, "unfinished long string") || starts(text, "unfinished long comment")) {
            put(what, wcap, "a [[ ... ]] block is not closed");
            put(todo, tcap, "Close it, then press Refresh");
            return 1;
        }
        if (starts(text, "break outside a loop at line ")) {
            put(what, wcap, "\"break\" is used outside a loop (line %s)", text + 29);
            put(todo, tcap, "Remove it or move it into the loop, then press Refresh");
            return 1;
        }
        if (starts(text, "no visible label '")) {
            char r[160];
            requote(text, r, sizeof r);
            put(what, wcap, "%s", r);
            put(todo, tcap, "Add the label or correct the goto, then press Refresh");
            return 1;
        }
        return 0;
    }
    token(near + 6, tok, sizeof tok);
    hl = (size_t)(near - text);
    if (hl >= sizeof head) hl = sizeof head - 1;
    memcpy(head, text, hl);
    head[hl] = 0;
    put(todo, tcap, "Correct it, then press Refresh");
    /* "'end' expected (to close 'function' at line 3)" */
    if (head[0] == '\'' && strstr(head, "' expected (to close '")) {
        char a[32], b[32];
        int line = 0;
        const char *e1 = strchr(head + 1, '\'');
        const char *c = strstr(head, "(to close '");
        const char *e2 = c ? strchr(c + 11, '\'') : NULL;
        const char *l = c ? strstr(c, " at line ") : NULL;
        if (e1 && e2 && l) {
            put(a, sizeof a, "%.*s", (int)(e1 - head - 1), head + 1);
            put(b, sizeof b, "%.*s", (int)(e2 - c - 11), c + 11);
            line = atoi(l + 9);
            put(what, wcap, "\"%s\" expected to close \"%s\" on line %d", a, b, line);
            return 1;
        }
    }
    if (head[0] == '\'' && strstr(head, "' expected") == head + strlen(head) - 10) {
        put(what, wcap, "\"%.*s\" expected near %s", (int)(hl - 11), head + 1, tok);
        return 1;
    }
    if (strcmp(head, "<eof> expected") == 0) {
        if (strcmp(tok, "\"end\"") == 0) put(what, wcap, "there is one \"end\" too many");
        else put(what, wcap, "something is left over near %s", tok);
        return 1;
    }
    if (strcmp(head, "<name> expected") == 0) { put(what, wcap, "a name was expected near %s", tok); return 1; }
    if (strcmp(head, "unexpected symbol") == 0) { put(what, wcap, "something unexpected near %s", tok); return 1; }
    if (strcmp(head, "unfinished string") == 0) { put(what, wcap, "text in quotes is not closed near %s", tok); return 1; }
    if (strcmp(head, "malformed number") == 0) { put(what, wcap, "a number is written wrongly near %s", tok); return 1; }
    if (strcmp(head, "unexpected end of file") == 0 || strcmp(tok, "the end of the file") == 0) {
        char r[200];
        requote(head, r, sizeof r);
        put(what, wcap, "%s at the end of the file", r);
        return 1;
    }
    if (head[0] == '\'' || starts(head, "invalid ") || starts(head, "unfinished ") || starts(head, "decimal escape") ||
        starts(head, "hexadecimal digit") || starts(head, "missing ") || starts(head, "UTF-8 value")) {
        char r[200];
        requote(head, r, sizeof r);
        put(what, wcap, "%s near %s", r, tok);
        return 1;
    }
    todo[0] = 0;
    return 0;
}

void sh_script_reason(const char *text, char *what, size_t what_cap, char *todo, size_t todo_cap)
{
    static const struct { const char *lua, *what, *todo; } exact[] = {
        { "stack overflow", "a function called itself too many times", "Look for a function that never stops calling itself" },
        { "C stack overflow", "functions called each other too deeply", "Look for functions that keep calling each other" },
        { "error in error handling", "an error happened while handling another error", "Check the error handler given to xpcall" },
        { "table index is nil", "a table was given nothing as a key", "Check the key before using it" },
        { "table index is NaN", "a table was given a number that is not a number (NaN) as a key", "Check the key before using it" },
        { "'for' initial value must be a number", "a \"for\" loop starts from something that is not a number", "Check the loop's first value" },
        { "'for' limit must be a number", "a \"for\" loop counts to something that is not a number", "Check the loop's limit" },
        { "'for' step must be a number", "a \"for\" loop counts by something that is not a number", "Check the loop's step" },
        { "'for' step is zero", "a \"for\" loop counts by zero, so it would never end", "Give the loop a step that is not zero" },
        { "number has no integer representation", "a number with a fraction was used where a whole number is needed", "Round it with math.floor" },
        { "pattern too complex", "a text pattern is too complex to match", "Use a simpler pattern" },
        { "cannot resume dead coroutine", "a coroutine that already finished was resumed", "Check the coroutine's status before resuming it" },
        { "cannot resume non-suspended coroutine", "a coroutine that is already running was resumed", "Check the coroutine's status before resuming it" },
        { "string length overflow", "a text grew too large", "Build smaller texts" },
        { "resulting string too large", "a text grew too large", "Build smaller texts" },
        { "not enough memory", "ran out of memory", "Look for a table that keeps growing" },
        { "'__index' chain too long; possible loop", "a table's __index leads around in a loop", "Check the metatables" },
        { "'__newindex' chain too long; possible loop", "a table's __newindex leads around in a loop", "Check the metatables" },
        { "chunk has too many lines", "the file has too many lines", "Split it into smaller files" },
        { "control structure too long", "a block of code is too long", "Split it into functions" },
        { "function or expression needs too many registers", "an expression or function is too large", "Split it into smaller parts" },
    };
    size_t i;
    if (what_cap) what[0] = 0;
    if (todo_cap) todo[0] = 0;
    if (!text) text = "";
    for (i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        if (strcmp(text, exact[i].lua) == 0) {
            put(what, what_cap, "%s", exact[i].what);
            put(todo, todo_cap, "%s", exact[i].todo);
            return;
        }
    }
    if (attempt(text, what, what_cap, todo, todo_cap)) return;
    if (bad_argument(text, what, what_cap, todo, todo_cap)) return;
    if (starts(text, "malformed pattern") || starts(text, "invalid pattern capture") ||
        starts(text, "unfinished capture") || starts(text, "invalid capture index") ||
        starts(text, "invalid use of '%' in replacement string") || starts(text, "missing '[' after '%f' in pattern")) {
        char r[200];
        requote(text, r, sizeof r);
        put(what, what_cap, "a text pattern is written wrongly (%s)", r);
        put(todo, todo_cap, "Check the pattern");
        return;
    }
    if (starts(text, "invalid value (at index ")) {
        put(what, what_cap, "table.concat met a value that is not text or a number at position %d", atoi(text + 24));
        put(todo, todo_cap, "Check the table's values");
        return;
    }
    if (starts(text, "wrong number of arguments to '")) {
        const char *s = text + 30, *e = strchr(s, '\'');
        put(what, what_cap, "\"%.*s\" was given the wrong number of values", e ? (int)(e - s) : 0, s);
        put(todo, todo_cap, "Check the values you pass");
        return;
    }
    if (starts(text, "variable '") && strstr(text, "' got a non-closable value")) {
        const char *s = text + 10, *e = strchr(s, '\'');
        put(what, what_cap, "\"%.*s\" is marked <close> but its value cannot be closed", e ? (int)(e - s) : 0, s);
        put(todo, todo_cap, "Give it a value with a __close metamethod");
        return;
    }
    if (syntax(text, what, what_cap, todo, todo_cap)) return;
    /* An author's own error text or an API refusal: as written. */
    {
        size_t n;
        put(what, what_cap, "%s", text[0] ? text : "the script raised an error without a message");
        n = strlen(what);
        while (n && (what[n - 1] == '.' || what[n - 1] == ' ' || what[n - 1] == '\n')) what[--n] = 0;
    }
}

void sh_script_reason_stop(int stop, unsigned long long limit, char *what, size_t what_cap,
                           char *todo, size_t todo_cap)
{
    switch (stop) {
    case SH_SCRIPT_STOP_MEMORY:
        put(what, what_cap, "ran out of memory, so this mod's scripts stopped for this map");
        put(todo, todo_cap, "Look for a table or text that keeps growing");
        break;
    case SH_SCRIPT_STOP_WALL:
        if (limit % 1000 == 0)
            put(what, what_cap, "ran for more than %llu second%s in one call without returning, so this mod's scripts "
                                "stopped for this map", limit / 1000, limit == 1000 ? "" : "s");
        else
            put(what, what_cap, "ran for more than %llu ms in one call without returning, so this mod's scripts "
                                "stopped for this map", limit);
        put(todo, todo_cap, "Look for a loop that does not end, or spread long work over several frames with after or run");
        break;
    default:
        put(what, what_cap, "stopped");
        if (todo_cap) todo[0] = 0;
        break;
    }
}

void sh_script_stop_text(const char *mod, const char *file, int line, const char *what,
                         const char *todo, char *out, size_t cap)
{
    char where[600];   /* a script's path: a Windows file name is at most 255 characters */
    if (file && file[0] && line > 0) put(where, sizeof where, ", %s line %d", file, line);
    else if (file && file[0]) put(where, sizeof where, ", %s", file);
    else where[0] = 0;
    if (todo && todo[0]) put(out, cap, "Script stopped: %s%s: %s. %s.", mod, where, what, todo);
    else put(out, cap, "Script stopped: %s%s: %s.", mod, where, what);
}
