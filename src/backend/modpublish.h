/* modpublish.h -- the publish side of the mod system (SPEC-distribution 5.3-5.5).
 *
 * A map that places mod objects reaches players with its mod content in the mod store, named by units (2.8):
 *
 *   PublishStart  (the editor's Publish) a map with mod rows is held before the engine starts: on the modnet worker the
 *                 units of its local rows are computed (modpin.h), the store says which it holds, and what it lacks is
 *                 sized. Bytes to send: "Publishing uploads 212 MB of mod content that the mod store does not have
 *                 yet. Publish?" (S7); No and the publish never starts. Yes: the units go up with progress on the
 *                 editor's pacifier. Then the engine's PublishStart runs. A row whose mod is not on this computer
 *                 refuses the publish with one sentence; a busy store past two minutes lets it go on, queued (N27).
 *   save          the publish's save writes its rows pinned to the prepared units (sh_modpublish_pin_save, bound as
 *                 modmanifest.h's pin step): the cloud-saved text the engine publishes names units only. The open map
 *                 keeps its own rows. The SerializeToJson detour (rawmap.c) reports the final text.
 *   PublishAfterSave  a text over the service bound (10,485,760 bytes) or an unreadable mod list is refused through
 *                 the engine's own PublishFail.
 *   PublishFeedPost   (the item id known) the save's units are claimed for the item (modclaim.h); a republish replaces
 *                 the set, a republish without rows releases it. A claim the store could not take is queued and retried
 *                 at every start. The engine call is never delayed.
 *   unpublish     the MapInfo screen's unpublish completion releases the item's claim (the store only).
 *   copy to My Maps   the two UI entries refuse, before any request, a published map whose cached text is over the
 *                 service bound.
 *   launch        queued claims and releases are retried, and claims whose units the store lost are repaired.
 *
 *   search tag    a publish whose save carries rows writes the search tag "mods" into the saved map's metadata when
 *                 it is absent: a player without Snapmap+ cannot play a map that needs mods. The cloud save re-reads
 *                 the local save's metadata from the profile (ReadLocalSaveMeta) instead of the editor's model, so the
 *                 tag has to be in the model WriteLocalSavedMapText writes: PublishStart arms it for the edit's local
 *                 id, and the next write of that local save, once the save observer has seen its rows, gets the tag.
 *   delete from My Maps   removes the local slot and the cloud-saved item only; a published item stays playable for
 *                 everyone. The delete completion therefore releases a claim only when its item is no longer
 *                 published: the feed post records a link (item -> cloud-saved item id and local id, in the "links"
 *                 section of mods\snapmap-plus.json); at delete, the linked items with a claim are checked against the
 *                 engine's own query of the items published from that cloud-saved item, and only an item missing from
 *                 a successful answer is released. No link, no claim, a link younger than an hour, a full search
 *                 page, a failed or late answer, or no cloud-saved id: nothing is released.
 *
 * Threads: every entry point runs on DOOM's main thread; store work runs on the modnet worker; the dialog and the job
 * results are polled from the main-thread tick (sh_modpublish_poll). Nothing here blocks the engine.
 */
#ifndef SH_MODPUBLISH_H
#define SH_MODPUBLISH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The published-map text bound the service enforces (the engine refuses its
 * own upload above 0xA00000 only after the item request was sent). */
#define SH_MODPUBLISH_TEXT_BOUND  10485760ull
/* The search tag a publish with mods carries. */
#define SH_MODPUBLISH_TAG "mods"
/* Item -> saved-map links written at the feed post, in the store root. */
#define SH_MODPUBLISH_LINKS_SECTION "links"   /* the section of mods\snapmap-plus.json */

/* 1 when the record's "links" section names `item`: a map saved on this
 * machine was published as that item. */
int  sh_modpublish_item_linked(const char *item);
/* A delete check waits this long for the engine's published-items answer. */
#define SH_MODPUBLISH_DELETE_CHECK_MS 60000ull

/* ------------------------------------------------------------ save */

/* The SerializeToJson detour's report of the final saved text (rows
 * included). Main thread; elsewhere it only invalidates the record. A save
 * while a publish is held abandons the hold. */
void sh_modpublish_save_seen(const char *text, size_t length);

/* ------------------------------------------------------------ decisions */

/* PublishStart(editorLocal, overwrite), before the engine's: 1 to call it now (a map without mod rows, or the resume
 * of a held publish), 0 when the publish is held (or a held one is still preparing). Main thread. */
int  sh_modpublish_publish_start(void *editor, unsigned char overwrite);

/* The pin step of a held publish's save (modmanifest.h sh_modmanifest_pin_fn): pins `rows` from the prepared units.
 * Unarmed (no publish resumed in the last 30 s) it returns 0 and the save writes its rows as they are. */
struct sh_modmanifest_list;
int  sh_modpublish_pin_save(struct sh_modmanifest_list *rows, const char *json, size_t length);

/* PublishStart(editorLocal): a publish begins; its local save follows. Arms
 * the search tag for this local id ("" when the map has none yet: the next
 * write is taken). Main thread. */
void sh_modpublish_publish_started(const char *local_id);

/* WriteLocalSavedMapText(localId, model, text), before the original, for
 * every local save the observer just reported: the search tag is made to
 * match the save. A save that carries rows gets it (ops.add_tag into `model`,
 * an idLocalSavedMapModel; the result, 1 added or 0 present), one without
 * rows loses it (ops.remove_tag; 2 removed, 0 absent). The model is copied
 * back to the editor after the write, so the edit's own list follows. An
 * armed publish's own write disarms the arm. -1 when the list could not be
 * changed, 0 when nothing was asked (no fresh save, no model). */
int  sh_modpublish_save_tag(const char *local_id, void *model);

/* PublishAfterSave(editorLocal): 1 to call the original now, 0 when the
 * publish was held or refused (the detour returns without the original). */
int  sh_modpublish_after_save(void *editor);

/* PublishFeedPost(edit, model) with the model's item id and revision text.
 * Queues the claim; never delays the engine. */
void sh_modpublish_feed_post(void *edit, const char *item_id, const char *revision);

/* The unpublish completion for this item id: queues the release. */
void sh_modpublish_unpublished(const char *item_id);

/* The My Maps delete completion with the deleted slot's local id and
 * cloud-saved item id (either may be ""). Starts the published-items check
 * for linked items that hold a claim; does nothing otherwise. */
void sh_modpublish_deleted(const char *local_id, const char *cloud_id);

/* The engine's answer to a published-items query started through
 * ops.published_query: ok with the item ids published from that cloud-saved
 * item, or !ok. Any thread; consumed on the main tick. */
void sh_modpublish_published_result(unsigned ticket, int ok, const char *const *item_ids, int count);

/* Copy to My Maps of (item id, revision): 1 to call the original, 0 when it
 * was refused (a notice is queued). */
int  sh_modpublish_copy_allowed(const char *item_id, const char *revision);

/* Main-thread tick, after every native frame. Idle cost: a few reads. */
void sh_modpublish_poll(void);

/* At backend start: retry queued claims and releases and repair claims whose units the store lost, on the worker. */
void sh_modpublish_launch(void);

/* 1 while a publish is held (preparing, asking or uploading). */
int  sh_modpublish_holding(void);

/* S7: "Publishing uploads 212 MB of mod content that the mod store does not have yet. Publish?", under 256 bytes. */
void sh_modpublish_compose_question(char *out, size_t cap, unsigned long long bytes);
/* The pacifier's line: "Uploading mod content: 84 of 212 MB.", or "Preparing this map's mod content." for total 0. */
void sh_modpublish_progress_text(char *out, size_t cap, unsigned long long done, unsigned long long total);

/* Byte counts as the notices print them: "1 byte", "512 bytes", "12 MB" (whole MB, at least 1). */
void sh_modpublish_size_text(char *out, size_t cap, unsigned long long bytes);

/* The cached published text the copy gate measures:
 * <cache root>\<id>_<rev>_map.decl (malloc'd), or NULL. */
char *sh_modpublish_cache_path(const char *item_id, const char *revision);

/* ------------------------------------------------------------ seam */

typedef struct sh_modpublish_ops {
    void *context;
    int  (*on_main)(void *c);
    int  (*can_ask)(void *c);
    int  (*ask)(void *c, unsigned gdm_id, unsigned button_set, const char *text);
    int  (*poll)(void *c, int ticket);            /* an sh_engine_dialog_result */
    void (*release)(void *c, int ticket);
    /* The editor's live edit object (its token) or 0 outside a session. */
    unsigned long long (*editor_token)(void *c, void *editor);
    void (*publish_fail)(void *c, void *editor);   /* the engine's PublishFail */
    /* The engine's PublishStart, called at the resume of a held publish (through its detour, which passes). */
    void (*start)(void *c, void *editor, unsigned char overwrite);
    /* The editor's pacifier with `text` under the publish title; NULL clears it. NULL: none is shown. */
    void (*busy)(void *c, void *editor, const char *text);
    /* The names the open editor map places now (modui.h sh_modui_placed_names): the count, -1 on failure. */
    int  (*placed_names)(void *c, char ***names, size_t *count);
    void (*editor_notice)(void *c, void *editor, const char *text);   /* the editor toast */
    void (*log)(void *c, const char *line);
    /* Add `tag` to a saved-map model's tags (idLocalSavedMapModel, tags at
     * +0x1A0) when absent: 1 added, 0 present, -1 failed. Called from the
     * local save write of an armed publish. NULL: no tag. */
    int  (*add_tag)(void *c, void *model, const char *tag);
    /* The published edit's cloud-saved item id and local id at the feed post
     * (either may come back ""). 0 when neither could be read. */
    int  (*edit_ids)(void *c, void *edit, char *cloud, size_t cloud_cap, char *local, size_t local_cap);
    /* Start the engine's query of the items published from this cloud-saved
     * item; its answer arrives through sh_modpublish_published_result with
     * `ticket`. 1 started, 0 not. NULL: delete never releases. */
    int  (*published_query)(void *c, const char *cloud_id, unsigned ticket);
    /* Remove `tag` from a saved-map model's tags: 1 removed, 0 absent, -1
     * failed. Called from a local save write whose save carries no mod rows,
     * so the search tag matches the save. NULL: the tag is never removed. */
    int  (*remove_tag)(void *c, void *model, const char *tag);
    /* A one-button notice in the menus (a copy refused): the mod system's
     * one notice queue, modpublished.h sh_modpublished_notice. */
    void (*notice)(void *c, const char *text);
} sh_modpublish_ops;

#ifdef SH_MODPUBLISH_TESTING
void sh_modpublish_test_set_ops(const sh_modpublish_ops *ops);
void sh_modpublish_test_reset(void);
/* 0 idle, 1 preparing, 2 raise, 3 asking, 4 uploading */
int  sh_modpublish_test_state(void);
int  sh_modpublish_test_pin_armed(void);          /* the next save pins its rows */
/* The rows a publish finds (NULL: the save's own rule, modmanifest.h sh_modmanifest_rows_for). */
void sh_modpublish_test_set_rows_for(int (*fn)(const char *const *names, size_t count, struct sh_modmanifest_list *out));
size_t sh_modpublish_test_context_units(void);    /* the units the feed post would claim */
int  sh_modpublish_test_background_busy(void);    /* queued or running background jobs */
void sh_modpublish_test_set_cache_root(const char *root);
int  sh_modpublish_test_delete_checks(void);      /* delete checks waiting for an answer */
void sh_modpublish_test_age_delete_checks(unsigned long long ms);   /* make them older */
void sh_modpublish_test_set_link_min_age(long long seconds);        /* default 3600 */
void sh_modpublish_test_age_tag_arm(unsigned long long ms);         /* make the armed tag older */
#endif

/* ------------------------------------------------------------ engine */
#ifndef SH_MODPUBLISH_PURE
#include "signatures.h"
/* Bind PublishAfterSave, PublishFail, PublishFeedPost, PublishStart (held for mod content), the editor's pacifier,
 * WriteLocalSavedMapText, the unpublish and delete completions and the two
 * copy entries (all or none), the tag-list helpers PublishStart uses and the
 * published-items query, register the save
 * observer, and run the launch step. Install after the engine dialog.
 * Returns 1 when the hooks are armed. */
int sh_modpublish_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The search tag in an open edit's own tags (edit+0x998: the model the next
 * local save writes, which the cloud save copies). remove 0 asks: 1 present,
 * 0 absent, -1 unreadable. remove 1 takes it out: 1 removed, 0 absent, -1
 * failed (the hooks are not installed, or the list could not be written).
 * The Mods tab removes "mods" when a map's last mod is removed. Main thread. */
int sh_modpublish_edit_tag(void *edit, const char *tag, int remove);
#endif

#ifdef __cplusplus
}
#endif

#endif
