/* modsound.h -- deliver installed mods' Wwise sound banks to the engine.
 *
 * Why a separate route: Wwise reads a bank with its own CreateFileW under the
 * OS path of base\sound\soundbanks\pc\, never through the file system's
 * +0x148 open that the override provider answers, and the engine lists that
 * folder once in LoadBanks (idSoundHardware_WWise vtable +0x30) and submits
 * each listed name to its per-bank wrapper (filter, then an async LoadBank).
 * A bank a bundle carries is therefore invisible twice: not listed and not
 * readable.
 *
 * The route (build 13954591, both executables; no code is patched):
 *   - open by name: every read-only qword holding AudioFileOpenName (the
 *     resolver's +0x10, two vtables on each executable) is swapped. For a
 *     file name a served bank answers, the replacement opens the bundle's
 *     file with CreateFileW and fills the AkFileDesc the way the engine's
 *     default blocking hook does (size, sector 0, no custom parameter, the
 *     handle, the device id at resolver+0x630), sync open; the engine's own
 *     Close calls CloseHandle on it. Every other name goes to the original.
 *   - submit: the engine's per-bank wrapper (AudioBankSubmit, (hardware,
 *     const char *file name)) for each served bank, on the main thread inside
 *     process heap 0, at the first main-thread pass after the bank is indexed
 *     (the editor entry, a map load, or the index rebuild itself when it runs
 *     on the main thread, as `sh_mods install` does). The wrapper applies
 *     the engine's listed-bank filter and prints its own result line
 *     ("async load success id <signed id> : <name>").
 *   - LoadBanks: its one vtable slot is swapped so `s_loadBanks`,
 *     `s_reloadAudioEvents` and `s_restart` (which rebuilds Wwise) run in
 *     process heap 0 on the main thread and are followed by a fresh submit
 *     of every served bank. Wwise allocates through the engine allocator
 *     with heap -1 (the top of the heap stack), so the engine's own rerun
 *     from the console in the editor put the reloaded stream packages in the
 *     map heap; the map unload at Play freed them and the next streamed
 *     open (by id, through the package list) read a freed package.
 *   - The hardware object is soundSystem+0x140, both decoded from the
 *     s_loadBanks console function (SoundLoadBanksCmd: LEA RCX,[soundSystem];
 *     CALL; MOV RCX,[soundSystem+0x140]; JMP [RAX+0x30]); a submit waits
 *     until hardware+8 bit 1 (Wwise initialised) is set.
 *
 * A bank installed mid-session is submitted at once (the engine's own
 * s_loadBanks submits newly listed banks after boot the same way, live on
 * 13954591). A bank whose bundle is removed stays loaded until the game
 * restarts; nothing here unloads.
 *
 * Stock banks on demand (DESIGN-additive 2.4, "Sound events"; snaphak-re
 * lanes G4 and L8). A mod's sound decl at a stock event's name borrows the
 * event; an event filed only by a bank SnapMap's game-mode filter skips
 * (doom_weapon_sp.bnk on 13954591) fails to start until that bank is loaded.
 * Per bundle, the banks needed are the product block's registries "banks"
 * (the builder's list) together with what its served sound decls need now
 * (modbankinfo.h), each checked against the game's own bank index. At the
 * map pass (modserve_activate_bundles: main thread, before the level load),
 * each bank a declared bundle needs and this process has not asked for is
 * loaded through the call the per-bank wrapper makes once its filter passes:
 * operator new(0x130), the wrapper's cookie ctor (the bank name), then the
 * async LoadBank(name, callback, cookie, -1, &id). The callback is the
 * product's, which records the outcome and hands the cookie to the engine's
 * own (its "async load success id <id> : <name>" line, then the cookie's
 * delete). The pass waits for the callback inside process heap 0 (Wwise
 * allocates through the top of the main thread's heap stack), at most
 * SH_MODSOUND_STOCK_WAIT_MS. Never the wrapper (its filter refuses the name
 * and answers success), never s_loadBanks, and the game-mode global is not
 * touched. A bank is asked for once per process; it stays loaded (about the
 * bank file's size in memory, 85 MB for doom_weapon_sp.bnk) until the game
 * exits. The outcome is recorded in modbankinfo's per-process states.
 *
 * Logs (backend log; echoed to the console on the main thread):
 *   MODS: sound: bank routes installed (...)
 *   MODS: sound: submitted <file> (<bundle id>) ...
 *   MODS: sound: opened <file> for Wwise from <bundle id> (N bytes)    first 32
 *   MODS: sound: loaded stock bank <name> (<ms> ms) for <key>
 */
#ifndef SH_MODSOUND_H
#define SH_MODSOUND_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODSOUND_MAX_BANKS   64
#define SH_MODSOUND_LEAF_MAX    72      /* <stem>.bnk */
#define SH_MODSOUND_HW_OFF      0x140u  /* soundSystem -> idSoundHardware_WWise * */
#define SH_MODSOUND_DEVICE_OFF  0x630u  /* resolver -> AkDeviceID */
#define SH_MODSOUND_MAX_SLOTS   4

/* The AkFileDesc the resolver fills (13954591, default blocking hook). */
typedef struct sh_modsound_file_desc {
    uint64_t file_size;
    uint32_t sector;
    uint32_t custom_param_size;
    void    *custom_param;
    void    *handle;
    uint32_t device_id;
    uint32_t reserved;
} sh_modsound_file_desc;

/* The file name Wwise is asked for, from a served request
 * sound/soundbanks/pc/<stem>.bnk (any case, either separator). 1 and the
 * lower-cased leaf, else 0. Pure. */
int sh_modsound_leaf(const char *request, char *leaf, size_t cap);

/* 1 when the wide name Wwise opens equals `leaf` ASCII-case-insensitively and
 * holds no separator. Pure. */
int sh_modsound_name_matches(const wchar_t *name, const char *leaf);

/* Decode SoundLoadBanksCmd: 1 with the soundSystem address and the address of
 * its +0x140 hardware pointer when the body has the expected shape and the
 * two RIP-relative operands are 0x140 apart. Pure. */
int sh_modsound_decode_cmd(const uint8_t *code, uintptr_t *sound_system, uintptr_t *hw_slot);

/* Every 8-aligned qword equal to `value` inside a read-only, non-executable
 * section of the PE at module_base, up to cap. Returns the count found (which
 * may exceed cap) or -1 for a bad image. */
int sh_modsound_find_slots(const uint8_t *module_base, uint64_t value, void ***slots, int cap);

/* The bank table one serving index yields. */
typedef struct sh_modsound_bank {
    char     leaf[SH_MODSOUND_LEAF_MAX];
    char    *disk_path;     /* UTF-8, owned */
    uint64_t size;
    char     bundle[80];
    volatile long submitted_gen;   /* the LoadBanks generation it was submitted in, 0 never */
} sh_modsound_bank;

typedef struct sh_modsound_table {
    int count;
    int skipped;            /* stock pointers and overflow, logged */
    sh_modsound_bank banks[SH_MODSOUND_MAX_BANKS];
} sh_modsound_table;

/* Add one served file: 1 for a bank added, 0 for a file that is not a bank,
 * -1 for a bank refused (a stock pointer, no disk path, a full table, a leaf
 * already in the table). */
int sh_modsound_table_add(sh_modsound_table *t, const char *request, const char *disk_path, uint64_t size,
                          const char *bundle_id, int is_stock);
void sh_modsound_table_free(sh_modsound_table *t);

/* The open replacement's decision, without the engine: the bank `name`
 * names in `t`, or NULL. open_mode must be 0 (read). */
const sh_modsound_bank *sh_modsound_lookup(const sh_modsound_table *t, const wchar_t *name, int open_mode);

/* Fill the descriptor as the engine's default hook does. */
void sh_modsound_fill_desc(sh_modsound_file_desc *d, uint64_t size, void *handle, uint32_t device_id);

/* ------------------------------------------------ stock banks on demand */

#define SH_MODSOUND_STOCK_WAIT_MS   3000   /* the map pass waits this long for a stock bank's callback */
#define SH_MODSOUND_COOKIE_SIZE     0x130u /* the wrapper's operator new for its cookie */
#define SH_MODSOUND_NEED_BANKS      4      /* stock banks one bundle may need */
#define SH_MODSOUND_NEED_BUNDLES    128

/* Decode AudioBankCookieSite (the wrapper's post-filter sequence): 1 with the
 * absolute targets of operator new, the cookie ctor, the callback LEA and the
 * LoadBank call when the bytes have the expected shape. Pure. */
int sh_modsound_decode_cookie_site(const uint8_t *site, uintptr_t *op_new, uintptr_t *cookie_ctor,
                                   uintptr_t *callback, uintptr_t *load_bank);

/* The stock banks each bundle needs (by bundle hash). */
typedef struct sh_modsound_need {
    char hash[65];
    char key[80];
    int  count;
    char banks[SH_MODSOUND_NEED_BANKS][SH_MODSOUND_LEAF_MAX];
} sh_modsound_need;
typedef struct sh_modsound_needs {
    int count;
    sh_modsound_need bundles[SH_MODSOUND_NEED_BUNDLES];
} sh_modsound_needs;

/* Add that the bundle `hash` (key `key`) needs stock bank `bank`: 1 added, 0
 * already there, -1 refused (not a bank the game's index lists as one
 * SnapMap skips, or a full table). Uses modbankinfo. */
int sh_modsound_needs_add(sh_modsound_needs *t, const char *hash, const char *key, const char *bank);
/* Add every bank a registries object's "banks" array names. Returns the
 * number added. */
int sh_modsound_needs_add_registries(sh_modsound_needs *t, const char *hash, const char *key, const char *registries);
/* Add what a served request needs when it is a sound decl
 * (generated/decls/sound/<name>.decl). Returns 1 when one was added. */
int sh_modsound_needs_add_served(sh_modsound_needs *t, const char *hash, const char *key, const char *request);
const sh_modsound_need *sh_modsound_needs_find(const sh_modsound_needs *t, const char *hash);

#ifndef SH_MODSOUND_PURE
#include "signatures.h"
/* Resolve the four signatures (SIG_OK only), decode the hardware pointer,
 * swap the open-by-name and LoadBanks slots and index the served banks. Call
 * after the serving index and the override provider are up. 1 when live. */
int  sh_modsound_install(const sig_result *results, size_t count, const uint8_t *module_base);
/* Submit every served bank not yet submitted in the current Wwise lifetime.
 * Main thread only (silently nothing elsewhere). */
void sh_modsound_before_pass(const char *phase);
/* One status line for sh_mods status. */
void sh_modsound_print_status(void);
/* The map pass for stock banks: load each bank the declared bundles need
 * (64-hex hashes; aliases resolve) and this process has not asked for. Main
 * thread only, before the level load. Returns the number loaded. */
int  sh_modsound_stock_before_map(const char *const *bundle_hashes, int n);
#endif

#ifdef SH_MODSOUND_TESTING
typedef int32_t (*sh_modsound_open_fn)(void *self, const wchar_t *name, int32_t mode, const void *flags,
                                       uint8_t *sync, sh_modsound_file_desc *desc);
typedef int (*sh_modsound_submit_fn)(void *hardware, const char *name);
typedef void (*sh_modsound_loadbanks_fn)(void *hardware);
/* Bind the replacements to fakes: the original open and LoadBanks, the
 * submit, a hardware object and a table (copied). */
void sh_modsound_test_bind(sh_modsound_open_fn original_open, sh_modsound_loadbanks_fn original_loadbanks,
                           sh_modsound_submit_fn submit, void *hardware, const sh_modsound_table *table);
int32_t sh_modsound_test_open(void *self, const wchar_t *name, int32_t mode, const void *flags, uint8_t *sync,
                              sh_modsound_file_desc *desc);
void sh_modsound_test_loadbanks(void *hardware);
/* The pass, with "on the main thread" forced. Returns the number submitted. */
int  sh_modsound_test_pass(void);
/* Stand-ins for entering and leaving process heap 0 (enter returns 1 when
 * entered). */
void sh_modsound_test_heap(int (*enter)(void), void (*leave)(void));
/* Stock banks: the engine's operator new, cookie ctor, LoadBank and callback
 * as fakes, the needs table (copied), the wait, and a sink for MODS: lines. */
typedef void *(*sh_modsound_opnew_fn)(size_t size);
typedef void *(*sh_modsound_cookie_ctor_fn)(void *self, const char *name);
typedef void (*sh_modsound_bank_cb_fn)(uint32_t bank_id, const void *in_memory, int32_t result, int32_t pool, void *cookie);
typedef int32_t (*sh_modsound_loadbank_fn)(const char *name, sh_modsound_bank_cb_fn callback, void *cookie, int32_t pool,
                                           uint32_t *bank_id);
void sh_modsound_test_bind_stock(sh_modsound_opnew_fn op_new, sh_modsound_cookie_ctor_fn ctor,
                                 sh_modsound_loadbank_fn load_bank, sh_modsound_bank_cb_fn engine_callback,
                                 const sh_modsound_needs *needs, unsigned wait_ms);
void sh_modsound_test_log(void (*sink)(const char *line));
/* The product's callback, as Wwise would call it. */
void sh_modsound_test_bank_callback(uint32_t bank_id, const void *in_memory, int32_t result, int32_t pool, void *cookie);
/* The map pass over these hashes (no alias resolution). */
int  sh_modsound_test_stock(const char *const *bundle_hashes, int n);
/* Forget the stock-bank slots (the per-process set, for a fresh test). */
void sh_modsound_test_stock_reset(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
