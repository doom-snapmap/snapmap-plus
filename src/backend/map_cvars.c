#include "map_cvars.h"
#include "text.h"

typedef struct cvar_entry {
    struct cvar_entry *next;
    char *name, *base, *value[2];
    enum sh_cvar_owner top;
} cvar_entry;

static cvar_entry *entries;
static int dirty;
static char *last_journal;
static int (*write_journal)(const char *);

int sh_map_cvar_same(const char *a, const char *b)
{
    char *ea, *eb;
    double x, y;
    if (!strcmp(a, b)) return 1;
    if (!*a || !*b) return 0;
    x = strtod(a, &ea);
    y = strtod(b, &eb);
    return !*ea && !*eb && x == y;
}

static cvar_entry **find(const char *name)
{
    cvar_entry **p = &entries;
    while (*p && _stricmp((*p)->name, name)) p = &(*p)->next;
    return p;
}

static void discard(cvar_entry **p)
{
    cvar_entry *e = *p;
    *p = e->next;
    free(e->name); free(e->base); free(e->value[0]); free(e->value[1]); free(e);
    dirty = 1;
}

int sh_map_cvar_hold(enum sh_cvar_owner owner, const char *name, const char *before, const char *value,
                     sh_cvar_setter set, void *context)
{
    cvar_entry **slot = find(name), *old = *slot, *fresh = NULL;
    int outside = old && !sh_map_cvar_same(before, old->value[old->top]);
    char *copy;
    int result;

    if (old && !outside && old->value[owner] && !strcmp(old->value[owner], value) &&
        (old->top == owner || !set)) return set ? set(context, name, value) : 1;
    if (!old || outside) {
        fresh = calloc(1, sizeof *fresh);
        if (!fresh) return 0;
        fresh->name = _strdup(name);
        fresh->base = _strdup(before);
        if (!fresh->name || !fresh->base) {
            free(fresh->name); free(fresh->base); free(fresh); return 0;
        }
    }
    copy = _strdup(value);
    result = copy ? (set ? set(context, name, value) : 1) : 0;
    if (result <= 0) {
        if (fresh) { free(fresh->name); free(fresh->base); free(fresh); }
        free(copy);
        return result;
    }
    if (fresh) {
        if (old) discard(slot);
        fresh->next = *slot;
        old = *slot = fresh;
    }
    free(old->value[owner]);
    old->value[owner] = copy;
    old->top = owner;
    dirty = 1;
    return result;
}

const char *sh_map_cvar_restore_to(enum sh_cvar_owner owner, const char *name, const char *now)
{
    cvar_entry **slot = find(name), *e = *slot;
    if (!e || !e->value[owner]) return now;
    if (!sh_map_cvar_same(now, e->value[e->top])) {
        /* A player edit supersedes both layers. */
        discard(slot);
        return now;
    }
    if (e->top != owner) return now;
    return e->value[1-owner] ? e->value[1-owner] : e->base;
}

void sh_map_cvar_release(enum sh_cvar_owner owner, const char *name)
{
    cvar_entry **slot = find(name), *e = *slot;
    if (!e || !e->value[owner]) return;
    if (!e->value[1-owner]) { discard(slot); return; }
    free(e->value[owner]);
    e->value[owner] = NULL;
    e->top = (enum sh_cvar_owner)(1-owner);
    dirty = 1;
}

void sh_map_cvars_end(enum sh_cvar_owner owner, char *(*read)(const char *), sh_cvar_setter set, void *context)
{
    cvar_entry *e = entries;
    while (e) {
        cvar_entry *next = e->next;
        if (e->value[owner]) {
            char *now = read ? read(e->name) : NULL;
            if (!now) sh_map_cvar_release(owner, e->name);
            else if (!sh_map_cvar_same(now, e->value[e->top])) discard(find(e->name));
            else {
                const char *want = sh_map_cvar_restore_to(owner, e->name, now);
                if (sh_map_cvar_same(now, want) || (set && set(context, e->name, want) == 1))
                    sh_map_cvar_release(owner, e->name);
            }
            free(now);
        }
        e = next;
    }
    sh_map_cvars_flush();
}

static void append_string(sh_text_buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    sh_text_append_str(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32) {
            char esc[] = "\\u0000";
            esc[4] = hex[c >> 4]; esc[5] = hex[c & 15];
            sh_text_append(b, esc, 6);
        } else {
            if (c == '"' || c == '\\') sh_text_append_str(b, "\\");
            sh_text_append(b, s, 1);
        }
    }
    sh_text_append_str(b, "\"");
}

void sh_map_cvars_writer(int (*write)(const char *json))
{
    write_journal = write;
}

void sh_map_cvars_flush(void)
{
    sh_text_buf b = {0};
    cvar_entry *e;
    if (!dirty || !write_journal) return;
    for (e = entries; e; e = e->next) {
        if (sh_map_cvar_same(e->base, e->value[e->top])) continue;
        sh_text_append_str(&b, b.n ? ",{\"name\":" : "{\"format\":1,\"cvars\":[{\"name\":");
        append_string(&b, e->name);
        sh_text_append_str(&b, ",\"was\":"); append_string(&b, e->base);
        sh_text_append_str(&b, ",\"set\":"); append_string(&b, e->value[e->top]);
        sh_text_append_str(&b, "}");
    }
    if (b.n) sh_text_append_str(&b, "]}");
    if (!b.oom) {
        if ((!b.p && !last_journal) || (b.p && last_journal && !strcmp(b.p, last_journal))) dirty = 0;
        else if (write_journal(b.p)) {
            free(last_journal); last_journal = b.p; b.p = NULL; dirty = 0;
        }
    }
    free(b.p);
}
