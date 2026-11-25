/* modsound.h -- delivers installed mods' Wwise sound banks to the engine (build 13954591, no code patched): the
 * swapped open-by-name slots (every read-only qword holding AudioFileOpenName) serve a bank from its bundle's
 * file, and AudioBankSubmit submits it on the main thread in process heap 0. The LoadBanks slot
 * (idSoundHardware_WWise vtable +0x30) is swapped so s_loadBanks, s_reloadAudioEvents and s_restart run in heap 0
 * and resubmit every served bank. Stock banks a build's registries "banks" need load at the map pass through
 * operator new(0x130), the cookie ctor and the async LoadBank, waiting at most SH_MODSOUND_STOCK_WAIT_MS. */
#ifndef SH_MODSOUND_H
#define SH_MODSOUND_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "modbankinfo.h"   /* SH_MBI_BANKS_MAX: the game's own bank count */

#ifdef __cplusplus
extern "C" {
#endif

/* A served bank leaf, <stem>.<16 hex of its hash>.bnk (modserve.c): at most 255 characters, the longest Windows file name, for the
 * longest stem the builder admits (modbundle.h SH_MB_BANK_STEM_MAX), and its NUL. */
#define SH_MODSOUND_LEAF_MAX    256
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
    volatile long submitted_gen;   /* the LoadBanks generation of its submit; 0 when not submitted */
} sh_modsound_bank;

typedef struct sh_modsound_table {
    int count, cap;         /* banks grows: the mods in use carry as many banks as they have */
    int skipped;            /* stock pointers and repeated names, logged */
    sh_modsound_bank *banks;
} sh_modsound_table;

/* Add one served file: 1 for a bank added, 0 for a file that is not a bank,
 * -1 for a bank refused (a stock pointer, no disk path, a leaf already in
 * the table, out of memory). */
int sh_modsound_table_add(sh_modsound_table *t, const char *request, const char *disk_path, uint64_t size,
                          const char *bundle_id, int is_stock);
void sh_modsound_table_free(sh_modsound_table *t);

/* The open replacement's decision, without the engine: the bank `name`
 * names in `t`, or NULL. open_mode must be 0 (read). */
const sh_modsound_bank *sh_modsound_lookup(const sh_modsound_table *t, const wchar_t *name, int open_mode);

/* Fill the descriptor as the engine's default hook does. */
void sh_modsound_fill_desc(sh_modsound_file_desc *d, uint64_t size, void *handle, uint32_t device_id);

#define SH_MODSOUND_STOCK_WAIT_MS   3000   /* the map pass waits this long for a stock bank's callback */
#define SH_MODSOUND_COOKIE_SIZE     0x130u /* the wrapper's operator new for its cookie */

/* Decode AudioBankCookieSite (the wrapper's post-filter sequence): 1 with the
 * absolute targets of operator new, the cookie ctor, the callback LEA and the
 * LoadBank call when the bytes have the expected shape. Pure. */
int sh_modsound_decode_cookie_site(const uint8_t *site, uintptr_t *op_new, uintptr_t *cookie_ctor,
                                   uintptr_t *callback, uintptr_t *load_bank);

/* The stock banks each bundle needs (by bundle hash). A bundle may need every
 * bank the game's index lists (SH_MBI_BANKS_MAX, the game's own count); the
 * bundles grow as the builds in use have them. */
typedef struct sh_modsound_need {
    char hash[65];
    char key[80];
    int  count;
    char banks[SH_MBI_BANKS_MAX][SH_MODSOUND_LEAF_MAX];
} sh_modsound_need;
typedef struct sh_modsound_needs {
    int count, cap;
    sh_modsound_need *bundles;
} sh_modsound_needs;

/* Add that the bundle `hash` (key `key`) needs stock bank `bank`: 1 added, 0
 * already there, -1 refused (not a bank the game's index lists as one
 * SnapMap skips, or out of memory). Uses modbankinfo. */
int sh_modsound_needs_add(sh_modsound_needs *t, const char *hash, const char *key, const char *bank);
/* Add every bank a registries object's "banks" array names. Returns the
 * number added. */
int sh_modsound_needs_add_registries(sh_modsound_needs *t, const char *hash, const char *key, const char *registries);
const sh_modsound_need *sh_modsound_needs_find(const sh_modsound_needs *t, const char *hash);

/* The banks whose file changed after the game loaded them: Wwise keeps the first bytes under a name until DOOM
 * restarts. Calls fn(context, bank file name, salt of the mod serving the new bytes) once per bank and returns
 * the count; any thread. */
int  sh_modsound_restart_banks(void (*fn)(void *context, const char *bank, const char *owner), void *context);

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
