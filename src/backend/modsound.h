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
 *     `s_reloadAudioEvents` and `s_restart` (which rebuilds Wwise) are
 *     followed by a fresh submit of every served bank.
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
 * Logs (backend log; echoed to the console on the main thread):
 *   MODS: sound: bank routes installed (...)
 *   MODS: sound: submitted <file> (<bundle id>) ...
 *   MODS: sound: opened <file> for Wwise from <bundle id> (N bytes)    first 32
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
#endif

#ifdef __cplusplus
}
#endif
#endif
