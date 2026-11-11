/* Boot pacing: shorten the engine's synchronous "common" preload.
 *
 * On build 13954591 the preload inside idCommonLocal::Init takes most of the
 * time to the SnapMap hub, and it is paced by waits rather than by the disk:
 *   - the load-screen video draw sleeps 10 ms while it holds the Bink lock,
 *     and the stream thread must take that lock before it services each read;
 *   - the main thread then polls each read's completion byte with Sleep(1).
 * Two call sites are redirected through small stubs next to the engine:
 *   - the draw's Sleep(10) releases the lock for the sleep and re-takes it;
 *   - the poll's Sleep(1) spins briefly, yields, and sleeps only as a fallback.
 * Both must be in place before the preload starts, a few seconds after launch,
 * so they are installed right after the build check, ahead of the other
 * features. Each operand is rewritten with the engine's threads quiesced
 * (patch.h), so a thread running the call never sees a torn displacement.
 *
 * Skipping the load-screen video instead (com_skipIntroVideo set before Init
 * reads it) was measured and rejected: the preload still took 30 s and the
 * Vulkan device was lost right after it. */
#ifndef SH_BOOT_PACING_H
#define SH_BOOT_PACING_H
#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

#define SH_BOOT_PACING_VIDEO_LOCK  1u   /* release the Bink lock around the draw's sleep */
#define SH_BOOT_PACING_READ_POLL   2u   /* spin and yield before the read poll's sleep */
#define SH_BOOT_PACING_DEFAULT     (SH_BOOT_PACING_VIDEO_LOCK | SH_BOOT_PACING_READ_POLL)

/* Site layouts, as offsets from each signature hit. */
#define SH_BOOT_LOCK_SITE_LEN    45     /* LEA RDI,[lock] ... CMP [handle 1],0; JZ; MOV RCX,[RBX]; CALL */
#define SH_BOOT_SLEEP_SITE_LEN   0x45   /* CMP byte ... MOV ECX,10; CALL Sleep; CMP [handle 2],0 */
#define SH_BOOT_SLEEP_CALL       0x38
#define SH_BOOT_POLL_SITE_LEN    36
#define SH_BOOT_POLL_CALL        0x16
#define SH_BOOT_POLL_DONE_OFF    0x70   /* the caller's completion byte, [rsp+0x70] */
#define SH_BOOT_POLL_ABORT_OFF   0x10   /* the stream object's abort byte, [rsi+0x10] */

/* Poll wait: spin on the completion byte, then yield, then Sleep(1). */
#define SH_BOOT_POLL_SPIN_US     20u
#define SH_BOOT_POLL_YIELD_US    1000u

/* Pure checks of the sites' bytes (tests use scratch copies). */
/* LEA RDI,[rip+d]: *lock = the global that holds the CRITICAL_SECTION pointer,
 * *handle = the first video handle global it tests before drawing. */
int sh_boot_video_lock_site(const unsigned char *site, uintptr_t *lock, uintptr_t *handle);
/* *first/*second = the two handle globals the draw passes, *sleep = the
 * Sleep(10) call's destination; the CMP after it must test *second. */
int sh_boot_video_sleep_site(const unsigned char *site, uintptr_t *first, uintptr_t *second,
                             uintptr_t *sleep);
/* *sleep = the poll's Sleep(1) destination. */
int sh_boot_read_poll_site(const unsigned char *site, uintptr_t *sleep);
/* A rel32 operand from next (the address after the instruction) to dest. */
int sh_boot_rel32(uintptr_t next, uintptr_t dest, int32_t *disp);
/* A Sleep thunk: REX JMP qword [rip+disp32]. */
int sh_boot_sleep_thunk(const unsigned char *code);
/* Stub bytes: the poll stub passes the completion and abort byte addresses to
 * wait_fn(ms, done, abort); the jump stub only transfers to fn. Return length. */
size_t sh_boot_poll_stub(unsigned char *out, size_t cap, uintptr_t wait_fn);
size_t sh_boot_jump_stub(unsigned char *out, size_t cap, uintptr_t fn);

/* The replacement waits, called from the engine through the stubs. */
void sh_boot_read_wait(unsigned long ms, volatile const char *done, volatile const char *abort_flag);
void sh_boot_video_sleep(unsigned long ms);

typedef struct sh_boot_pacing_stats {
    long reads, spun, yielded, slept;   /* poll waits and how each ended */
    long video_sleeps, video_released;  /* draw sleeps and those that released the lock */
} sh_boot_pacing_stats;
void sh_boot_pacing_get_stats(sh_boot_pacing_stats *out);

#ifndef SH_BOOT_PACING_PURE
/* Number of boot signatures, and their names in BACKEND_ENGINE_SIGNATURES. */
#define SH_BOOT_PACING_SIGS 3
/* Resolve the boot signatures (sig_resolve_one) into
 * results[0..SH_BOOT_PACING_SIGS). Returns the number resolved. */
size_t sh_boot_pacing_match(const uint8_t *base, sig_result *results, size_t cap);
/* Boot thread, before the other features. Installs the parts in mask whose
 * sites check out; logs each part and leaves the engine untouched for the
 * rest. age_ms (the time since process creation) is logged. Returns the parts
 * installed. */
unsigned sh_boot_pacing_install(const sig_result *results, size_t count, unsigned mask,
                                unsigned long age_ms);
unsigned sh_boot_pacing_live(void);
/* Milliseconds since this process was created (0 when unknown). */
unsigned long sh_boot_pacing_process_age_ms(void);
#endif
#endif
