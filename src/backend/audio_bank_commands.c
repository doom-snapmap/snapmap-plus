#include "audio_bank_commands.h"
#include "audio_native_image.h"
#include <string.h>
#include <stdio.h>

/* The native queue copies this value, but retains the completion pointer until
 * its callback runs. Both synchronous and worker-backed managers expose the
 * same initialize/submit/wait virtual interface. */
typedef struct abc_completion {
    int result;
    uint32_t padding;
    void *event;
    int pool;
    uint32_t tail;
} abc_completion;
typedef struct abc_command {
    uint32_t operation, padding;
    void *callback;
    abc_completion *completion;
    uint32_t mode, padding2;
    uint32_t id;
    int pool;
    const void *memory;
    uint32_t length, padding3;
} abc_command;
typedef int (*abc_init_fn)(void *, abc_completion *);
typedef int (*abc_submit_fn)(void *, const abc_command *);
typedef int (*abc_wait_fn)(void *, abc_completion *, int);
typedef char abc_completion_layout[(sizeof(abc_completion) == 24) ? 1 : -1];
typedef char abc_command_layout[(sizeof(abc_command) == 56) ? 1 : -1];
typedef char abc_command_id_offset[(offsetof(abc_command, id) == 32) ? 1 : -1];

static void **abc_slot(const unsigned char *instruction, unsigned char destination)
{
    int32_t relative;
    if (instruction[0] != 0x48 || instruction[1] != 0x8b || instruction[2] != destination) return NULL;
    memcpy(&relative, instruction + 3, sizeof(relative));
    return (void **)((uintptr_t)(instruction + 7) + (intptr_t)relative);
}

int sh_audio_registry_bind(sh_audio_registry *registry,const unsigned char *image,
    const sig_result *results,size_t count)
{
    const unsigned char *post=NULL,*prepare=NULL;sh_audio_bank_commands commands={0};
    if(!registry)return 0;memset(registry,0,sizeof(*registry));
    if(!sh_audio_bank_commands_bind(&commands,image,results,count))return 0;
    for(size_t i=0;i<count;i++) {
        if(!results[i].name)continue;
        const unsigned char **target=!strcmp(results[i].name,"AudioEventPost") ? &post :
            !strcmp(results[i].name,"AudioBankPrepare") ? &prepare : NULL;
        if(target) { if(*target || results[i].status!=SIG_OK)return 0;*target=(const unsigned char *)results[i].addr; }
    }
    if(!ani_section(image,post,0x2d,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE) ||
        !ani_section(image,prepare,0x2d,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE))return 0;
    __try {
        void **index=abc_slot(post+0x1a,0x0d);int32_t relative;
        static const unsigned char event_table[]={0x48,0x81,0xc1,0xa8,0x12,0,0};
        if(!index || memcmp(post+0x21,event_table,sizeof(event_table)) ||
            prepare[0x26]!=0x48 || prepare[0x27]!=0x8d || prepare[0x28]!=0x0d)return 0;
        memcpy(&relative,prepare+0x29,4);
        CRITICAL_SECTION *lock=(CRITICAL_SECTION *)((uintptr_t)(prepare+0x2d)+(intptr_t)relative);
        if(((uintptr_t)index&7) || ((uintptr_t)lock&7) || index==commands.manager_slot ||
            !ani_section(image,index,sizeof(void *),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image,lock,sizeof(*lock),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE))return 0;
        *registry=(sh_audio_registry){commands.manager_slot,index,lock};return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Floyd detection bounds malformed chains without imposing a package/object
 * count limit. Caller holds the table lock for every pointer and ID read. */
static int abc_chain(const unsigned char *node,size_t next,size_t identity,uint32_t id)
{
    const unsigned char *slow=node,*fast=node;
    while(node) {
        uint32_t value;memcpy(&value,node+identity,sizeof(value));if(value==id)return 1;
        memcpy(&node,node+next,sizeof(node));
        if(slow)memcpy(&slow,slow+next,sizeof(slow));
        if(fast)memcpy(&fast,fast+next,sizeof(fast));
        if(fast)memcpy(&fast,fast+next,sizeof(fast));
        if(slow && slow==fast)return -1;
    }
    return 0;
}

static int abc_table(CRITICAL_SECTION *lock,const unsigned char *buckets,
    size_t first,size_t count,size_t next,size_t identity,uint32_t id)
{
    int result=0;volatile int held=0;
    __try {
        __try {
            EnterCriticalSection(lock);held=1;
            for(size_t i=first;i<first+count && !result;i++) {
                const unsigned char *node;memcpy(&node,buckets+i*sizeof(void *),sizeof(node));
                result=abc_chain(node,next,identity,id);
            }
        } __finally { if(held)LeaveCriticalSection(lock); }
    } __except(EXCEPTION_EXECUTE_HANDLER) { result=-1; }
    return result;
}

int sh_audio_registry_occupied(const sh_audio_registry *registry,uint32_t id,char *error,size_t capacity)
{
    const unsigned char *index=NULL,*manager=NULL;int result=-1;
    if(error && capacity)*error=0;
    if(!registry || !registry->manager_slot || !registry->index_slot || !registry->bank_lock)goto done;
    __try { index=*registry->index_slot;manager=*registry->manager_slot; }
    __except(EXCEPTION_EXECUTE_HANDLER) { goto done; }
    if(!index || !manager)goto done;
    if(!id)return 1;
    for(size_t i=0;i<12;i++) {
        const unsigned char *table=index+i*0x638;
        result=abc_table((CRITICAL_SECTION *)table,table+0x28,id%193,1,8,0x10,id);
        if(result)goto done;
    }
    result=abc_table((CRITICAL_SECTION *)(manager+0x48),manager+0x80,id%193,1,0,8,id);
    if(result)goto done;
    /* Memory banks include their backing pointer in the hash, so inspect all
     * buckets; any matching ID reserves that name regardless of load mode. */
    result=abc_table(registry->bank_lock,manager+0x690,0,193,0x20,0x10,id);
done:
    if(result<0 && error && capacity)snprintf(error,capacity,"native audio identity registry is unavailable or invalid");
    return result;
}

int sh_audio_bank_commands_bind(sh_audio_bank_commands *commands,
    const unsigned char *image, const sig_result *results, size_t count)
{
    const unsigned char *load = NULL;
    void **manager, **callback;
    if (!commands || commands->uncertain) return 0;
    memset(commands, 0, sizeof(*commands));
    if (!results) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (!results[i].name || strcmp(results[i].name, "AudioBankLoad")) continue;
        if (load || results[i].status != SIG_OK) return 0;
        load = (const unsigned char *)results[i].addr;
    }
    if (!ani_section(image, load, 0xd3, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE)) return 0;
    __try {
        manager = abc_slot(load + 0x1e, 0x0d);
        callback = abc_slot(load + 0x41, 0x05);
        if (!manager || !callback || manager == callback ||
            ((uintptr_t)manager & 7) || ((uintptr_t)callback & 7) ||
            abc_slot(load + 0x68, 0x0d) != manager || abc_slot(load + 0xac, 0x0d) != manager ||
            load[0x28] != 0xff || load[0x29] != 0x50 || load[0x2a] != 0x20 ||
            load[0xa9] != 0xff || load[0xaa] != 0x50 || load[0xab] != 0x18 ||
            load[0xbe] != 0x41 || load[0xbf] != 0xff || load[0xc0] != 0x51 || load[0xc1] != 0x28 ||
            !ani_section(image, manager, sizeof(void *), IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image, callback, sizeof(void *), IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE)) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    commands->manager_slot = manager;
    commands->callback_slot = callback;
    return 1;
}

int sh_audio_bank_commands_load(sh_audio_bank_commands *commands, uint32_t id)
{
    void *manager, **table, *callback;
    abc_init_fn initialize;
    abc_submit_fn submit;
    abc_wait_fn wait;
    abc_completion *completion;
    abc_command command = {0};
    int result;
    if (!commands || !id || !commands->manager_slot || !commands->callback_slot) return 2;
    if (commands->uncertain) return -1;
    /* Before the first native call, failure has no native side effects. */
    __try {
        manager = *commands->manager_slot;
        callback = *commands->callback_slot;
        if (!manager || !(table = *(void ***)manager)) return 2;
        submit = (abc_submit_fn)table[3];
        initialize = (abc_init_fn)table[4];
        wait = (abc_wait_fn)table[5];
        if (!submit || !initialize || !wait) return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
    completion = (abc_completion *)commands->completion;
    memset(completion, 0, sizeof(*completion));
    command.callback = callback;
    command.completion = completion;
    command.mode = 1; /* Native file-ID mode bypasses the bank name cache. */
    command.id = id;
    command.pool = -1;
    __try {
        result = initialize(manager, completion);
        if (result == 1) {
            result = submit(manager, &command);
            /* Wait also disposes initialized completion resources when submit
             * fails. The worker manager waits for its callback on success. */
            result = wait(manager, completion, result);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        commands->uncertain = 1;
        return -1;
    }
    return result;
}
