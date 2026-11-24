/* modpublish.h -- the publish side of the mod system. A map with mod rows is held at FINISH while the units of its local
 * rows are computed and the units the store lacks are uploaded once the author agrees; its save then pins a copy of the
 * rows and the feed post claims the units for the item. Unpublish and My Maps delete release claims (delete only when
 * the item is no longer published), and the search tag "mods" is written into a publishing save's metadata. Every entry
 * point runs on DOOM's main thread; store work runs on the modnet worker and its results are polled from the tick. */
#ifndef SH_MODPUBLISH_H
#define SH_MODPUBLISH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The published-map text bound the service enforces (the engine refuses its own upload above 0xA00000 only after the
 * item request was sent). */
#define SH_MODPUBLISH_TEXT_BOUND  10485760ull
/* The search tag a publish with mods carries. */
#define SH_MODPUBLISH_TAG "mods"
/* Item -> saved-map links written at the feed post, in the store root. */
#define SH_MODPUBLISH_LINKS_SECTION "links"   /* the section of mods\snapmap-plus.json */

/* 1 when the record's "links" section names `item`: a map saved on this machine was published as that item. */
int  sh_modpublish_item_linked(const char *item);
/* A delete check waits this long for the engine's published-items answer. */
#define SH_MODPUBLISH_DELETE_CHECK_MS 60000ull

/* The SerializeToJson detour's report of the final saved text (rows included). Main thread; elsewhere it only
 * invalidates the record. A save while a publish is held abandons the hold. */
void sh_modpublish_save_seen(const char *text, size_t length);

/* PublishStart(editorLocal, overwrite), before the engine's: 1 to call it now (a map without mod rows, or the resume
 * of a FINISH hold), 0 when a hold is on its way, or when a map with mod rows reaches it without its FINISH hold (the
 * FINISH step did not bind): that publish is refused with one sentence. Main thread. */
int  sh_modpublish_publish_start(void *editor, unsigned char overwrite);

/* The publish screens' PUBLISH on FINISH, after the game's own checks passed and before the screens close (the
 * MapProperties frame's gate call). 1 holds the publish with the screens up (prepare, ask in the editor's own dialog,
 * upload, then close the screens and start PublishStart); 0 lets the frame go on (a map without mod rows). Main thread. */
int  sh_modpublish_finish_gate(void *editor, unsigned char overwrite);

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

/* WriteLocalSavedMapText(localId, model, text), before the original, for every local save the observer just reported:
 * makes the search tag match the save (ops.add_tag when it carries rows, 1 added or 0 present; ops.remove_tag when not,
 * 2 removed or 0 absent). An armed publish's own write disarms it. -1 when the list could not be changed, 0 when
 * nothing was asked (no fresh save, no model). */
int  sh_modpublish_save_tag(const char *local_id, void *model);

/* The cloud save's read of a map's slot (ReadLocalSavedMapText, while it uploads a publish): the slot's text with the
 * publish's pinned rows (the published copy, HeapFree), or NULL to upload the slot as it is. The slot itself keeps the
 * map's own rows. Main thread. */
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

/* Copy to My Maps of (item id, revision): 1 to call the original, 0 when it was refused (a notice is queued). */
int  sh_modpublish_copy_allowed(const char *item_id, const char *revision);

/* Main-thread tick, after every native frame. Idle cost: a few reads. */
void sh_modpublish_poll(void);

/* At backend start: retry queued claims and releases and repair claims whose units the store lost, on the worker. */
void sh_modpublish_launch(void);

/* 1 while a publish is held (preparing, asking or uploading). */
int  sh_modpublish_holding(void);

/* The upload question, "Publishing uploads 212 MB of mod content that the mod store does not have yet. Publish?", under 256
 * bytes. */
void sh_modpublish_compose_question(char *out, size_t cap, unsigned long long bytes);
/* The pacifier's line: "Uploading mod content: 84 of 212 MB.", or "Preparing this map's mod content." for total 0. */
void sh_modpublish_progress_text(char *out, size_t cap, unsigned long long done, unsigned long long total);

/* Byte counts as the notices print them: "1 byte", "512 bytes", "12 MB" (whole MB, at least 1). */
void sh_modpublish_size_text(char *out, size_t cap, unsigned long long bytes);

/* The cached published text the copy gate measures:
 * <cache root>\<id>_<rev>_map.decl (malloc'd), or NULL. */
char *sh_modpublish_cache_path(const char *item_id, const char *revision);

typedef struct sh_modpublish_ops {
    void *context;
    int  (*on_main)(void *c);
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
    /* This publish made the computer's publishing key: the backup offer is pending. NULL: nothing. */
    void (*key_made)(void *c);
    /* 1 when the Snapmap+ window is showing (it shows the backup offer itself). NULL or 0: the publish's notice says
     * where the offer is (SH_MODPUBLISH_KEY_MADE_TEXT). */
    int  (*window_open)(void *c);
    /* The publish held at FINISH (sh_modpublish_finish_gate), with the publish screens up. NULL: FINISH never holds,
     * and a map with mods is not published. */
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
/* 0 idle, 1 preparing, 2 raise, 3 asking, 4 uploading, 5 waiting for the editor frame to resume the hold */
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
/* Every byte the publish hooks rely on (PublishFail's toast, the two copy entries' record and model reads, PublishStart's
 * tag append and Save callback), checked in the functions' own code: NULL when each holds (*out filled), else the first
 * check that failed. The bind and tests/modpub_sites_test.c both run this function. */
const char *sh_modpublish_check_sites(const sh_modpublish_sites *sites, sh_modpublish_bound *out);

/* The cloud-save request's read of the slot (CloudSaveSlotReadCall, VK 0x11BE407, GL 0x11BD7D7), the read the
 * published copy is made at: NULL when its bytes hold, else the first check that failed. */
const char *sh_modpublish_check_cloud_read(const uint8_t *call, const uint8_t *read_text);

/* The cloud-save request's read of the slot's metadata (CloudSaveMetaReadCall, VK 0x11BE673, GL 0x11BDA43, calling
 * `meta_read`, ReadLocalSaveMeta), and id's hash of the map text that fills a model's declChecksum (+0xB8): `create`
 * (NewSavedMapDeclChecksumCall) and `local` (LocalSaveDeclChecksumCall) both call it. NULL when every byte holds,
 * with *hash the hash function. */
const char *sh_modpublish_check_cloud_meta(const uint8_t *meta, const uint8_t *meta_read, const uint8_t *create,
                                           const uint8_t *local, const void **hash);

/* The published copy's declChecksum: noted when the cloud save's read of the slot gets the pinned text (the engine's
 * own hash of it), taken once by that request's metadata read of the same slot. 1 with *sum, 0 none. */
void sh_modpublish_cloud_sum_note(const char *local_id, unsigned sum);
int  sh_modpublish_cloud_sum_take(const char *local_id, unsigned *sum);

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
/* Binds the publish hooks (all or none): PublishAfterSave, PublishFail, PublishFeedPost, PublishStart (held for mod
 * content), the pacifier, WriteLocalSavedMapText, the unpublish and delete completions, the two copy entries, the
 * tag-list helpers and the published-items query; registers the save observer and runs the launch step. Install after
 * the engine dialog. Returns 1 when the hooks are armed. */
int sh_modpublish_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The search tag in an open edit's own tags (edit+0x998, which the cloud save copies). remove 0 asks (1 present, 0
 * absent, -1 unreadable); remove 1 takes it out (1 removed, 0 absent, -1 failed). The Mods tab removes "mods" when a
 * map's last mod is removed. Main thread. */
int sh_modpublish_edit_tag(void *edit, const char *tag, int remove);
#endif

#ifdef __cplusplus
}
#endif

#endif
