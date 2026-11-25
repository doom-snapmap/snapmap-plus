/* Carriers for one Play's modded animation events and attachment tags. */
#ifndef SH_MD6_REGISTRY_H
#define SH_MD6_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SH_MD6_CLIP_EVENTS, SH_MD6_ATTACHMENT_TAGS } sh_md6_registry_kind;
typedef void (*sh_ce_line_fn)(void *context, const char *line);

/* Row states, written by the engine side; every row starts at SH_MD6_ROW_NEW. */
#define SH_MD6_ROW_NEW       0   /* the carrier has not been looked for in this Play */
#define SH_MD6_ROW_READY     1
#define SH_MD6_ROW_MISSING   2   /* the carrier did not load */

typedef struct sh_md6_row {
    char *name;               /* registered clip or attached md6Def */
    char *carrier;            /* mods/<salt>/....md6 */
    char *base;               /* the model the carrier inherits */
    char *salt;
    char *label;              /* the mod as lines name it */
    void *volatile model;     /* the carrier's md6Def in this Play, or NULL */
    volatile long state;
    volatile long answered;   /* lookups the carrier answered this Play */
    volatile long logged;     /* the first answer's line is written */
} sh_md6_row;

typedef struct sh_md6_registry {
    sh_md6_row *rows;          /* sorted by name (sh_md6_registry_seal) */
    int n, cap;
    int mods;                 /* mods that registered at least one row */
} sh_md6_registry;

/* Each name keeps its first valid carrier. Returns the number added, or -1 if allocation failed. */
int sh_md6_registry_add(sh_md6_registry *r, sh_md6_registry_kind kind, const char *salt, const char *label,
                        const char *registries, sh_ce_line_fn line, void *context);
void sh_md6_registry_seal(sh_md6_registry *r);
sh_md6_row *sh_md6_registry_find(const sh_md6_registry *r, const char *name);
void sh_md6_registry_free(sh_md6_registry *r);
void sh_md6_registry_note(sh_ce_line_fn line, void *context, int *oom, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
