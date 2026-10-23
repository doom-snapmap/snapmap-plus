#ifndef SH_AUDIO_BANK_GATE_H
#define SH_AUDIO_BANK_GATE_H
#include "audio_limits_native.h"

typedef int (*sh_audio_bank_dispatch_fn)(void *,const void *);
typedef struct sh_audio_bank_gate sh_audio_bank_gate;
typedef struct sh_audio_bank_scope {
    sh_audio_bank_gate *owner;
    void *manager;
    DWORD thread;
    int intervened;
} sh_audio_bank_scope;
struct sh_audio_bank_gate {
    sh_audio_bank_dispatch_fn entry,original;
    void **manager_slot;
    CRITICAL_SECTION *render_lock;
    CRITICAL_SECTION lock;
    sh_audio_bank_scope *scope;
    unsigned dispatch_depth;
    int initialized,installed;
};

/* Bind before native installation; do not rebind an initialized gate. No engine
 * globals are dereferenced. The common dispatcher covers synchronous submission
 * and the worker path, whose queue lock is released before dispatch. */
int sh_audio_bank_gate_bind(sh_audio_bank_gate *gate,const unsigned char *image,
    const sig_result *results,size_t count,const sh_audio_limits_native *limits);
/* Startup-only initialization. Install the dispatcher detour while native target
 * execution is quiescent, publish original before patching, then set installed.
 * Gate and original remain process-owned through all native callers. */
int sh_audio_bank_gate_initialize(sh_audio_bank_gate *gate);
/* Detour body: forward every native result/callback/exception unchanged while
 * excluding a shared-policy publication. Does not wait for an audio render lock. */
int sh_audio_bank_gate_dispatch(sh_audio_bank_gate *gate,void *manager,const void *command);
/* Main-thread publication boundary, after private bank loads finish and BEFORE
 * taking renderer, provider or audio locks. Do not submit/wait for bank commands
 * while holding this scope. Bank dispatch takes gate -> native locks; controls
 * take gate -> audio render lock, never the reverse. Scope storage is stable. */
int sh_audio_bank_gate_begin(sh_audio_bank_gate *gate,sh_audio_bank_scope *scope,char *error,size_t capacity);
int sh_audio_bank_gate_ready(const sh_audio_bank_scope *scope,char *error,size_t capacity);
/* Always releases an owned scope, even when readiness fails. Failure reports an
 * intervening native operation or manager change; never strand the bank worker
 * behind an uncertain rollback. Native-resource receipts remain separately owned. */
int sh_audio_bank_gate_end(sh_audio_bank_scope *scope,char *error,size_t capacity);
#endif
