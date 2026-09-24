#include "audio_bank_gate.h"
#include "audio_native_image.h"
#include <stdio.h>

static int ag_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
int sh_audio_bank_gate_bind(sh_audio_bank_gate *gate,const unsigned char *image,
    const sig_result *results,size_t count,const sh_audio_limits_native *limits)
{
    const unsigned char *entry=NULL;
    if(!gate || gate->initialized)return 0;memset(gate,0,sizeof(*gate));
    if(!limits || !limits->manager_slot || !limits->render_lock || (count && !results))return 0;
    for(size_t i=0;i<count;i++)if(results[i].name && !strcmp(results[i].name,"AudioBankDispatch")) {
        if(entry || results[i].status!=SIG_OK)return 0;entry=(const unsigned char *)results[i].addr;
    }
    if(!ani_section(image,entry,23,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE) ||
        !ani_section(image,limits->manager_slot,8,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE) ||
        !ani_section(image,limits->render_lock,sizeof(CRITICAL_SECTION),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE))return 0;
    __try {
        /* Sixteen complete, position-independent bytes for the existing hook
         * implementation: two stack stores, PUSH RBP and LEA RBP,[RSP-0x57]. */
        static const unsigned char prologue[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9};
        if(memcmp(entry,prologue,sizeof(prologue)))return 0;
        gate->entry=(sh_audio_bank_dispatch_fn)entry;gate->manager_slot=limits->manager_slot;gate->render_lock=limits->render_lock;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int sh_audio_bank_gate_initialize(sh_audio_bank_gate *gate)
{
    if(!gate || gate->initialized || !gate->entry || !gate->manager_slot || !gate->render_lock)return 0;
    if(!InitializeCriticalSectionEx(&gate->lock,0,0))return 0;
    gate->initialized=1;return 1;
}
int sh_audio_bank_gate_dispatch(sh_audio_bank_gate *gate,void *manager,const void *command)
{
    int result;EnterCriticalSection(&gate->lock);gate->dispatch_depth++;
    if(gate->scope)gate->scope->intervened=1;
    __try {result=gate->original(manager,command);}
    __finally {gate->dispatch_depth--;LeaveCriticalSection(&gate->lock);}
    return result;
}
static int ag_owned(const sh_audio_bank_scope *scope)
{ return scope && scope->owner && scope->owner->scope==scope && scope->thread==GetCurrentThreadId(); }
int sh_audio_bank_gate_begin(sh_audio_bank_gate *gate,sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    int valid=0;if(error && capacity)*error=0;
    if(!gate || !gate->initialized || !gate->installed || !gate->original || !scope || scope->owner)
        return ag_fail(error,capacity,"native bank publication boundary is unavailable or already owned");
    __try {
        /* Detect reversed lock order before waiting on a bank operation whose
         * completion may need that same render lock. */
        if(gate->render_lock->OwningThread==(HANDLE)(uintptr_t)GetCurrentThreadId())
            return ag_fail(error,capacity,"audio render lock must be acquired after the bank publication boundary");
    } __except(EXCEPTION_EXECUTE_HANDLER) {return ag_fail(error,capacity,"native audio lock is unavailable");}
    EnterCriticalSection(&gate->lock);
    __try {
        if(!gate->scope && !gate->dispatch_depth && *gate->manager_slot) {
            *scope=(sh_audio_bank_scope){gate,*gate->manager_slot,GetCurrentThreadId(),0};gate->scope=scope;valid=1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {valid=0;}
    if(!valid)LeaveCriticalSection(&gate->lock);
    return valid ? 1 : ag_fail(error,capacity,"native bank publication cannot begin during another operation");
}
int sh_audio_bank_gate_ready(const sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    int ready=0;if(error && capacity)*error=0;
    if(ag_owned(scope) && !scope->intervened && !scope->owner->dispatch_depth) {
        __try {ready=scope->owner->installed && *scope->owner->manager_slot==scope->manager;}
        __except(EXCEPTION_EXECUTE_HANDLER) {ready=0;}
    }
    return ready ? 1 : ag_fail(error,capacity,"native bank publication ownership changed during the transition");
}
int sh_audio_bank_gate_end(sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    if(!ag_owned(scope))return ag_fail(error,capacity,"native bank publication scope belongs to another operation");
    int ready=sh_audio_bank_gate_ready(scope,error,capacity);sh_audio_bank_gate *gate=scope->owner;
    gate->scope=NULL;memset(scope,0,sizeof(*scope));LeaveCriticalSection(&gate->lock);return ready;
}
