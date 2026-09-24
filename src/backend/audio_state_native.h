#ifndef SH_AUDIO_STATE_NATIVE_H
#define SH_AUDIO_STATE_NATIVE_H
#include "audio_limits_native.h"

typedef struct sh_audio_state_rule { uint32_t from, to, duration; } sh_audio_state_rule;
typedef struct sh_audio_state_rules {
    sh_audio_state_rule *items;
    uint32_t count, capacity;
} sh_audio_state_rules;
typedef struct sh_audio_state_group {
    uint32_t duration, current;
    uint64_t reserved;
    void *consumers;
    sh_audio_state_rules rules;
} sh_audio_state_group;
typedef struct sh_audio_state_entry {
    uint32_t id, padding;
    sh_audio_state_group *group;
} sh_audio_state_entry;
/* Only the native manager's first field. Its other collections and lock are
 * outside this view and are never copied or replaced. */
typedef struct sh_audio_state_table {
    sh_audio_state_entry *items;
    uint32_t count, capacity;
} sh_audio_state_table;
typedef struct sh_audio_state_change sh_audio_state_change;
typedef struct sh_audio_state_row sh_audio_state_row;
typedef struct sh_audio_state_native {
    int (*ensure)(sh_audio_state_table *,uint32_t);
    void (*destroy)(sh_audio_state_group *);
    void *(*allocate)(int,size_t);
    void (*release)(int,void *);
    int *pool;
    sh_audio_state_table **state_slot;
    void **manager_slot;
    CRITICAL_SECTION *render_lock;
    sh_audio_state_change *pending;
} sh_audio_state_native;
struct sh_audio_state_change {
    sh_audio_state_native *owner;
    sh_audio_state_table *target, before, prepared;
    sh_audio_state_row *rows;
    size_t count;
    void *manager;
    DWORD thread;
    int pool, state, uncertain, table_installed;
};
int sh_audio_state_native_bind(sh_audio_state_native *native,const unsigned char *image,
    const sig_result *results,size_t count,const sh_audio_limits_native *limits);
/* Serialized main-thread bank-publication boundary, never concurrent bank
 * commands/shutdown. Each represented group supplies its complete compiled
 * transition table; absent groups belong to other native content. The compiler
 * includes original rules when composing effective policy. Existing identities,
 * current states and consumer links survive publication. New groups use the
 * native constructor and become visible together after all preparation succeeds.
 * Returns 1 applied, 0 fully cleaned failure, -1 retained recovery ownership. */
int sh_audio_state_native_apply(sh_audio_state_native *native,const sh_audio_shared_policy *policy,
    sh_audio_state_change *change,char *error,size_t capacity);
int sh_audio_state_native_ready(sh_audio_state_change *change,char *error,size_t capacity);
/* Rollback requires new groups to have no attached consumers. Failure retains
 * the receipt: detach prepared bank consumers before retrying. Existing group
 * runtime state is preserved. Unexpected configuration writes are not erased. */
int sh_audio_state_native_restore(sh_audio_state_change *change,char *error,size_t capacity);
void sh_audio_state_native_accept(sh_audio_state_change *change);
/* Retire detached old storage after commit. New live groups/arrays now belong
 * to the native manager and its ordinary group-clear/shutdown lifecycle. This
 * does not unload banks, release private IDs, or discard active sound users. */
int sh_audio_state_native_release(sh_audio_state_change *change,char *error,size_t capacity);
#endif
