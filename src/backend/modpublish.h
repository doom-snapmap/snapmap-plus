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
 *   save          the publish's save pins a copy of its rows to the prepared units (sh_modpublish_pin_save, bound
 *                 as modmanifest.h's pin step): the text the engine publishes (the cloud save's read of the slot,
 *                 sh_modpublish_cloud_text) names units only. The open map and its slot keep their own rows. The
 *                 SerializeToJson detour (rawmap.c) reports the final text.
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

/* The publish screens' PUBLISH on FINISH, after the game's own checks passed and before the screens close (the
 * MapProperties frame's gate call, while the question is asked there: sh_modpublish_finish_set). 1 holds the publish
 * with the screens up: the hold prepares, asks S7 in the editor's own dialog over them (the one the game's publish
 * errors use), uploads, then closes the screens and starts the engine's PublishStart itself. 0 lets the frame go on
 * (a map without mod rows, or the question asked at PublishStart as before). Main thread. */
int  sh_modpublish_finish_gate(void *editor, unsigned char overwrite);

/* Where S7 is asked: 1 at the FINISH step, 0 at PublishStart after the screens closed (the default until the FINISH
 * path is proven live). The engine side patches the gate call only while it is on. */
void sh_modpublish_finish_set(int on);
int  sh_modpublish_finish_on(void);

/* The EditorFrame detour, after the engine's frame: a publish held at FINISH closes the screens and starts here, where
 * the product already changes the editor's state (fly_to.c). Idle cost: one read. */
void sh_modpublish_editor_tick(void *editor);

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

/* The cloud save's read of a map's slot (ReadLocalSavedMapText, while it uploads a publish): the slot's text with the
 * publish's pinned rows (the published copy, HeapFree), or NULL to upload the slot as it is. The slot itself keeps the
 * map's own rows, so the author's map follows their folders (R-LB1 F6). Main thread. */
char *sh_modpublish_cloud_text(const char *local_id, const char *text, size_t length, size_t *out_length);

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
    /* The signed-in account's name (UTF-8; "" signed out): 1, or 0 when the game's read is not available. Main
     * thread. NULL: no name is read. */
    int  (*account_name)(void *c, char *out, size_t cap);
    /* This publish made the computer's publishing key: the backup offer is pending (SPEC 4.2). NULL: nothing. */
    void (*key_made)(void *c);
    /* 1 when the Snapmap+ window is showing (it shows the backup offer itself). NULL or 0: the publish's notice says
     * where the offer is (SH_MODPUBLISH_KEY_MADE_TEXT). */
    int  (*window_open)(void *c);
    /* A publish held at FINISH (sh_modpublish_finish_gate), with the publish screens up. NULL: FINISH never holds. */
    int  (*finish_can_ask)(void *c, void *editor);   /* 1 when neither the editor's dialog nor its pacifier is up */
    /* The editor's own dialog over the screens: Yes/No (yes_no 1) or one button; title NULL is the game's "publish
     * failed" title. 1 when it shows. */
    int  (*finish_ask)(void *c, void *editor, const char *title, const char *text, int yes_no);
    int  (*finish_poll)(void *c, void *editor);   /* 0 still up, 1 the first button, 2 the second or Back */
    void (*finish_close)(void *c, void *editor);  /* the screens close, as the frame closes them before PublishStart */
    void (*finish_done)(void *c, void *editor);   /* the frame's success event, as it follows PublishStart there */
    /* 1 when the cloud save's read of the slot reaches sh_modpublish_cloud_text, so the published copy can carry the
     * pinned rows. NULL: it does (tests). */
    int  (*cloud_ready)(void *c);
    void (*finish_clear)(void *c, void *editor);  /* a screens' result left while the publish was held is forgotten */
} sh_modpublish_ops;

/* After a publish that made the key while the Snapmap+ window was closed (the editor has no passphrase field). */
#define SH_MODPUBLISH_KEY_MADE_TEXT "Your publishing key was made. Open Snapmap+ to save a backup of it."

#ifdef SH_MODPUBLISH_TESTING
void sh_modpublish_test_set_ops(const sh_modpublish_ops *ops);
void sh_modpublish_test_reset(void);
/* 0 idle, 1 preparing, 2 raise, 3 asking, 4 uploading, 5 waiting for the editor frame to resume a FINISH hold */
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
/* The engine functions the publish hooks read, each at its signature's address in the running image (or in a mapped
 * executable, for the image test). */
typedef struct sh_modpublish_sites {
    const uint8_t *fail, *copy, *browser, *start, *after_save, *str_ctor, *str_dtor;
} sh_modpublish_sites;
/* What the checks decode out of those functions' code. */
typedef struct sh_modpublish_bound {
    const void *toast, *screen_shell, *shell_manager, *record, *entry, *browser_state, *hide_progress;
    const void *tags_grow, *idstr_assign;
} sh_modpublish_bound;
/* Every byte the publish hooks rely on (PublishFail's toast, the two copy entries' record and model reads,
 * PublishStart's tag append and Save callback), checked in the functions' own code: NULL when each holds (*out
 * filled), else the name of the first check that failed, for the log. The bind and tests/modpub_sites_test.c both
 * run this function. */
const char *sh_modpublish_check_sites(const sh_modpublish_sites *sites, sh_modpublish_bound *out);

/* The cloud-save request's read of the slot (CloudSaveSlotReadCall, VK 0x11BE407, GL 0x11BD7D7), the read the
 * published copy is made at: NULL when its bytes hold, else the first check that failed. */
const char *sh_modpublish_check_cloud_read(const uint8_t *call, const uint8_t *read_text);

/* The FINISH step (VK 0x1190362, GL 0x118F4E2: the MapProperties frame's PUBLISH case): `call` is its gate call
 * (PublishFinishGateCall), `start` PublishStart, `gdm_show` the editor dialog's show (EditorGdmShow), `answer` the
 * EditorFrame call of that dialog's input (EditorGdmAnswerCall). */
typedef struct sh_modpublish_finish_sites {
    const uint8_t *call, *start, *gdm_show, *answer;
} sh_modpublish_finish_sites;
typedef struct sh_modpublish_finish_bound {
    const void *gate;    /* (editor, show 1, back 1): the game's limits, start-module and deprecated-entity checks */
    const void *close;   /* (editor): the screens close (the editor's state stack pops) */
    const void *input;   /* (dialog, input): the dialog's answer, +0x1DC under the flag +0x1D8 */
    /* the frame's own success event after PublishStart: event_log(event_owner(), event_name, event_desc) */
    const void *event_owner, *event_log;
    const char *event_name, *event_desc;
} sh_modpublish_finish_bound;
/* Every byte the FINISH hold relies on, in the functions' own code: the case's arguments (editor rdi, state rsi, the
 * overwrite byte +0x21), the gate, the screens' close and PublishStart in order, the gate's own dialog on
 * [editor+0x21088]+0x8E8 with +0xA2C bit 0, the descriptor the show reads, the screens' pending result
 * [editor+0x21088]+0xA18 -> +0x8E0, and the answer EditorFrame waits for before it clears bit 0. NULL when each
 * holds, else the first check that failed. The bind and tests/modpub_sites_test.c both run it. */
const char *sh_modpublish_check_finish(const sh_modpublish_finish_sites *sites, sh_modpublish_finish_bound *out);

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
