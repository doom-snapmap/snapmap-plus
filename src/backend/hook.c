/* Reversible 14-byte absolute detours. Callers choose instruction boundaries.
 * One explicit preparation variant relocates a verified RIP-relative LEA. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "hook.h"
#include "patch.h"

static void write_absjmp(uint8_t *p, void *dest)
{
    p[0] = 0xFF; p[1] = 0x25;                  /* jmp qword ptr [rip+0] */
    p[2] = p[3] = p[4] = p[5] = 0x00;
    *(uint64_t *)(p + 6) = (uint64_t)dest;     /* the absolute 8-byte target follows inline */
}

/* Stable records retain original bytes and trampoline ownership until unpatch.
 * Registry growth must not silently disable later feature installations. */
typedef struct patch_record {
    struct patch_record *next;
    void   *target;            /* engine fn whose prologue we overwrote */
    void   *tramp;             /* the trampoline (also this record's identity key) */
    void   *detour;
    size_t stolen;
    uint8_t original[48];      /* Expected target bytes, separate from relocated code. */
    int    installed;
    sh_patch_handle patch;    /* Includes partial writes and pending protection restoration. */
    RUNTIME_FUNCTION unwind[2];
    DWORD unwind_count;
} patch_record;

static patch_record *g_patches;
static SRWLOCK g_patch_lock = SRWLOCK_INIT;

/* Inline detours begin with one of these transfers. A relative call is also
 * refused here; the explicit relocation variant re-allows it at its offset. */
static int redirect_bytes(const uint8_t *b, size_t n)
{
    if (n >= 1 && (b[0] == 0xE9 || b[0] == 0xEB || b[0] == 0xE8)) return 1;
    if (n >= 2 && b[0] == 0xFF && (b[1] == 0x25 || b[1] == 0x15)) return 1;
    if (n >= 12 && b[0] == 0x48 && (b[1] & 0xF8) == 0xB8 &&
        b[10] == 0xFF && (b[11] & 0xF8) == 0xE0) return 1;              /* mov r64,imm64; jmp r64 */
    if (n >= 13 && b[0] == 0x49 && (b[1] & 0xF8) == 0xB8 &&
        b[10] == 0x41 && b[11] == 0xFF && (b[12] & 0xF8) == 0xE0) return 1;
    if (n >= 6 && b[0] == 0x68 && (b[5] == 0xC3 ||
        (n >= 14 && b[5] == 0xC7 && b[6] == 0x44 && b[7] == 0x24 && b[8] == 0x04 && b[13] == 0xC3)))
        return 1;                                                       /* push imm32; [mov]; ret */
    return 0;
}

int hook_target_is_redirect(const void *target)
{
    uint8_t b[14];
    if (!target) return 0;
    __try { memcpy(b, target, sizeof b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return redirect_bytes(b, sizeof b);
}

/* Keep the actual FF15 instruction within signed-disp32 reach of its slot.
 * Query whole regions rather than assuming any preferred address is free. */
static void *allocate_call_slot(size_t size,uintptr_t slot,size_t next_offset)
{
    SYSTEM_INFO system;MEMORY_BASIC_INFORMATION region;
    GetSystemInfo(&system);
    if(slot<next_offset)return NULL;
    uintptr_t anchor=slot-next_offset,granularity=system.dwAllocationGranularity;
    uintptr_t low=anchor>INT32_MAX?anchor-INT32_MAX:0;
    uintptr_t high=anchor>UINTPTR_MAX-((uintptr_t)INT32_MAX+1)?UINTPTR_MAX:
        anchor+(uintptr_t)INT32_MAX+1;
    if(low<(uintptr_t)system.lpMinimumApplicationAddress)low=(uintptr_t)system.lpMinimumApplicationAddress;
    if(high>(uintptr_t)system.lpMaximumApplicationAddress-size)high=(uintptr_t)system.lpMaximumApplicationAddress-size;
    if(low>UINTPTR_MAX-(granularity-1))return NULL;
    uintptr_t address=(low+granularity-1)&~(granularity-1);
    while(address<=high) {
        if(!VirtualQuery((void *)address,&region,sizeof region))break;
        uintptr_t end=(uintptr_t)region.BaseAddress+region.RegionSize;
        if(end<=address)break;
        if(region.State==MEM_FREE) {
            while(address<=high && size<=end-address) {
                void *memory=VirtualAlloc((void *)address,size,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
                if(memory)return memory;
                if(address>UINTPTR_MAX-granularity)return NULL;
                address+=granularity;
            }
        }
        if(end>UINTPTR_MAX-(granularity-1))break;
        address=(end+granularity-1)&~(granularity-1);
    }
    return NULL;
}

static void *prepare_locked(void *target, void *detour, size_t stolen, size_t offset, int relocation)
{
    uint8_t original[48];
    size_t emitted = stolen;
    uintptr_t call_target = 0;
    unsigned destination=0, branch_condition=0; size_t branch_length=0, load_length=0;
    unsigned stack_allocation=0;size_t function_size=0,unwind_offset=0,allocation_size=0;
    unsigned char function_tail[11];
    if (!target || !detour || stolen < 14 || stolen > 48) return NULL;
    __try { memcpy(original, target, stolen); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    /* Callers verify their window when they resolve it, which can be long
     * before preparation. Recheck the bytes actually copied: an observer that
     * hooked the target in between must not have its jump re-evaluated here. */
    if (redirect_bytes(original, stolen) && !(relocation == 2 && offset == 0 && original[0] == 0xE8))
        return NULL;
    if (relocation == 6) {
        DWORD64 source_base=0;PRUNTIME_FUNCTION source;
        unsigned char info[6];int32_t displacement;
        if(stolen<10 || offset<4 || offset>stolen-6 || original[0]!=0x48 ||
           original[1]!=0x83 || original[2]!=0xec || original[3]<8 ||
           original[3]>0x78 || (original[3]&15)!=8 ||
           original[offset]!=0xff || original[offset+1]!=0x15)return NULL;
        stack_allocation=original[3];
        source=RtlLookupFunctionEntry((DWORD64)(uintptr_t)target,&source_base,NULL);
        if(!source)return NULL;
        __try {
            if(source_base+source->BeginAddress!=(DWORD64)(uintptr_t)target ||
               source->EndAddress<source->BeginAddress ||
               stolen>source->EndAddress-source->BeginAddress)return NULL;
            function_size=source->EndAddress-source->BeginAddress;
            memcpy(info,(const void *)(uintptr_t)(source_base+source->UnwindData),sizeof info);
            if(function_size-stolen!=5 && function_size-stolen!=11)return NULL;
            memcpy(function_tail,(const uint8_t *)target+stolen,function_size-stolen);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return NULL; }
        if(info[0]!=1 || info[1]!=4 || info[2]!=1 || info[3]!=0 || info[4]!=4 ||
           info[5]!=(unsigned char)(((stack_allocation-8)/8)<<4|2))return NULL;
        const unsigned char epilogue[]={0x48,0x83,0xc4,(unsigned char)stack_allocation,0xc3};
        size_t tail_size=function_size-stolen;
        if(tail_size==11 && memcmp(function_tail,"\x48\x85\xc0\x0f\x95\xc0",6))return NULL;
        if(memcmp(function_tail+tail_size-5,epilogue,5))return NULL;
        memcpy(&displacement,original+offset+2,4);
        call_target=(uintptr_t)target+offset+6+(intptr_t)displacement; /* pointer slot, not its value */
        if(call_target>=(uintptr_t)target && call_target<(uintptr_t)target+function_size)return NULL;
        unwind_offset=(function_size+3)&~(size_t)3;
        allocation_size=unwind_offset+8;
    } else if (relocation == 1) {
        const uint8_t *lea;
        if (offset > stolen - 7) return NULL;
        lea = original + offset;
        if ((lea[0] != 0x48 && lea[0] != 0x4c) || lea[1] != 0x8d ||
            (lea[2] & 0xc7) != 0x05) return NULL;
        emitted += 3;
    } else if (relocation == 2) {
        int32_t displacement;
        if (offset > stolen - 5 || original[offset] != 0xe8) return NULL;
        memcpy(&displacement, original + offset + 1, sizeof(displacement));
        call_target = (uintptr_t)target + offset + 5 + (intptr_t)displacement;
        if (call_target >= (uintptr_t)target && call_target < (uintptr_t)target + stolen) return NULL;
        emitted += 11;
    } else if (relocation == 3) {
        if(offset>stolen-7) return NULL;
        const uint8_t *load=original+offset;
        if((load[0]!=0x48 && load[0]!=0x4c) || load[1]!=0x8b || (load[2]&0xc7)!=0x05) return NULL;
        destination=((load[2]>>3)&7) | ((load[0]&4)?8:0);
        if(destination==4) return NULL;
        load_length=13+((destination&7)==4 || (destination&7)==5);
        emitted+=load_length-7;
    } else if (relocation == 5) {
        if(offset>stolen-7 || original[offset]!=0x83 || original[offset+1]!=0x3d) return NULL;
        emitted+=8;
    } else if (relocation == 4) {
        if(offset>=stolen) return NULL;
        const uint8_t *branch=original+offset;
        intptr_t displacement;
        if((branch[0]&0xf0)==0x70) {
            if(stolen-offset<2) return NULL;
            branch_length=2; branch_condition=branch[0]&15; displacement=(int8_t)branch[1];
        } else if(branch[0]==0x0f && stolen-offset>=6 && (branch[1]&0xf0)==0x80) {
            int32_t relative; memcpy(&relative,branch+2,4);
            branch_length=6; branch_condition=branch[1]&15; displacement=relative;
        } else return NULL;
        call_target=(uintptr_t)target+offset+branch_length+displacement;
        if(call_target>=(uintptr_t)target && call_target<(uintptr_t)target+stolen) return NULL;
        emitted+=16-branch_length;
    }

    patch_record *rec;
    for (rec = g_patches; rec; rec = rec->next)
        if (rec->target == target) return NULL;
    rec = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*rec));
    if (!rec) return NULL;

    if(!allocation_size)allocation_size=emitted+14;
    uint8_t *tramp = relocation==6?allocate_call_slot(allocation_size,call_target,offset+6):
        (uint8_t *)VirtualAlloc(NULL, allocation_size, MEM_COMMIT | MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if (!tramp) { HeapFree(GetProcessHeap(), 0, rec); return NULL; }
    if (relocation == 6) {
        intptr_t relative=(intptr_t)call_target-(intptr_t)(tramp+offset+6);
        if(relative<INT32_MIN || relative>INT32_MAX) {
            VirtualFree(tramp,0,MEM_RELEASE);HeapFree(GetProcessHeap(),0,rec);return NULL;
        }
        int32_t displacement=(int32_t)relative;
        memcpy(tramp,original,stolen);memcpy(tramp+stolen,function_tail,function_size-stolen);
        memcpy(tramp+offset+2,&displacement,4);
        /* Copy the verified function through its native epilogue. A synthetic
         * FF25 jump directly after CALL is mistaken for a completed epilogue
         * by Windows; keeping the actual body and return avoids that ambiguity.
         * The native FF15 itself preserves every register, flags and stack arg. */
        unsigned char *info=tramp+unwind_offset;
        memset(info,0,8);info[0]=1;info[1]=4;info[2]=1;info[4]=4;
        info[5]=(unsigned char)(((stack_allocation-8)/8)<<4|2);
        rec->unwind[0]=(RUNTIME_FUNCTION){0,(DWORD)function_size,(DWORD)unwind_offset};
    } else if (relocation == 1) {
        const uint8_t *lea = original + offset;
        int32_t displacement;
        uintptr_t address;
        memcpy(&displacement, lea + 3, sizeof(displacement));
        address = (uintptr_t)target + offset + 7 + (intptr_t)displacement;
        memcpy(tramp, original, offset);
        tramp[offset] = (uint8_t)(0x48 | ((lea[0] & 4) ? 1 : 0));
        tramp[offset + 1] = (uint8_t)(0xb8 | ((lea[2] >> 3) & 7));
        memcpy(tramp + offset + 2, &address, sizeof(address));
        memcpy(tramp + offset + 10, original + offset + 7, stolen - offset - 7);
    } else if (relocation == 2) {
        memcpy(tramp, original, offset);
        memcpy(tramp + offset, "\xff\x15\x02\x00\x00\x00\xeb\x08", 8);
        memcpy(tramp + offset + 8, &call_target, sizeof(call_target));
        memcpy(tramp + offset + 16, original + offset + 5, stolen - offset - 5);
    } else if (relocation == 3) {
        int32_t displacement; uintptr_t address;
        memcpy(&displacement,original+offset+3,4);
        address=(uintptr_t)target+offset+7+(intptr_t)displacement;
        memcpy(tramp,original,offset);
        tramp[offset]=(uint8_t)(0x48|((destination&8)?1:0));
        tramp[offset+1]=(uint8_t)(0xb8|(destination&7));
        memcpy(tramp+offset+2,&address,8);
        tramp[offset+10]=(uint8_t)(0x48|((destination&8)?5:0));
        tramp[offset+11]=0x8b;
        unsigned low=destination&7;
        tramp[offset+12]=(uint8_t)((low<<3)|low|(low==5?0x40:0));
        if(low==4) tramp[offset+13]=0x24;
        else if(low==5) tramp[offset+13]=0;
        memcpy(tramp+offset+load_length,original+offset+7,stolen-offset-7);
    } else if (relocation == 5) {
        int32_t displacement; uintptr_t address;
        memcpy(&displacement,original+offset+2,4);
        address=(uintptr_t)target+offset+7+(intptr_t)displacement;
        memcpy(tramp,original,offset);
        tramp[offset]=0x50; tramp[offset+1]=0x48; tramp[offset+2]=0xb8;
        memcpy(tramp+offset+3,&address,8);
        tramp[offset+11]=0x83; tramp[offset+12]=0x38;
        tramp[offset+13]=original[offset+6]; tramp[offset+14]=0x58;
        memcpy(tramp+offset+15,original+offset+7,stolen-offset-7);
    } else if (relocation == 4) {
        memcpy(tramp,original,offset);
        tramp[offset]=(uint8_t)(0x70|(branch_condition^1));
        tramp[offset+1]=14;
        write_absjmp(tramp+offset+2,(void *)call_target);
        memcpy(tramp+offset+16,original+offset+branch_length,stolen-offset-branch_length);
    } else {
        memcpy(tramp, original, stolen);
    }
    if(relocation!=6)write_absjmp(tramp + emitted, (uint8_t *)target + stolen);
    FlushInstructionCache(GetCurrentProcess(), tramp, allocation_size);
    if(relocation==6) {
        if(!RtlAddFunctionTable(rec->unwind,1,(DWORD64)(uintptr_t)tramp)) {
            VirtualFree(tramp,0,MEM_RELEASE);HeapFree(GetProcessHeap(),0,rec);return NULL;
        }
        rec->unwind_count=1;
    }
    rec->target = target;
    rec->tramp  = tramp;
    rec->detour = detour;
    rec->stolen = stolen;
    memcpy(rec->original, original, stolen);
    rec->installed = 0;
    memset(&rec->patch, 0, sizeof rec->patch);
    rec->next = g_patches;
    g_patches = rec;
    return tramp;
}

static void *prepare(void *target, void *detour, size_t stolen, size_t offset, int relocation)
{
    void *tramp;
    AcquireSRWLockExclusive(&g_patch_lock);
    tramp = prepare_locked(target, detour, stolen, offset, relocation);
    ReleaseSRWLockExclusive(&g_patch_lock);
    return tramp;
}

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    return prepare(target, detour, stolen, 0, 0);
}

void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea_offset)
{
    if (lea_offset == SIZE_MAX) return NULL;
    return prepare(target, detour, stolen, lea_offset, 1);
}

void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t call_offset)
{
    return prepare(target, detour, stolen, call_offset, 2);
}
void *hook_prepare_rip_load(void *target,void *detour,size_t stolen,size_t offset)
{ return prepare(target,detour,stolen,offset,3); }
void *hook_prepare_conditional_jump(void *target,void *detour,size_t stolen,size_t offset)
{ return prepare(target,detour,stolen,offset,4); }
void *hook_prepare_rip_cmp32_imm8(void *target,void *detour,size_t stolen,size_t offset)
{ return prepare(target,detour,stolen,offset,5); }
void *hook_prepare_rip_indirect_call(void *target,void *detour,size_t stolen,size_t offset)
{ return prepare(target,detour,stolen,offset,6); }

static patch_record *find_record(void *tramp)
{
    if (!tramp) return NULL;
    for (patch_record *rec = g_patches; rec; rec = rec->next)
        if (rec->tramp == tramp) return rec;
    return NULL;
}

static sh_patch_status commit_locked(void *tramp)
{
    patch_record *rec = find_record(tramp);
    if (!rec) return B2_PATCH_REFUSED_BADARG;
    if (rec->installed) return B2_PATCH_OK;
    if (rec->patch.live) return B2_PATCH_FAIL_ROLLBACK;
    uint8_t patch[48];
    write_absjmp(patch, rec->detour);
    if (rec->stolen > 14) memset(patch + 14, 0x90, rec->stolen - 14);
    MemoryBarrier(); /* The caller's original callback and detour dependencies precede target writes. */
    sh_patch_status result = code_patch(rec->target, rec->original, patch, rec->stolen, &rec->patch);
    if (result == B2_PATCH_OK) rec->installed = 1;
    return result;
}

sh_patch_status hook_commit(void *tramp)
{
    sh_patch_status result;
    AcquireSRWLockExclusive(&g_patch_lock);
    result = commit_locked(tramp);
    ReleaseSRWLockExclusive(&g_patch_lock);
    return result;
}

int hook_is_installed(void *tramp)
{
    int installed;
    AcquireSRWLockShared(&g_patch_lock);
    patch_record *rec = find_record(tramp);
    installed = rec && rec->installed;
    ReleaseSRWLockShared(&g_patch_lock);
    return installed;
}

static int revert_record(patch_record *rec)
{
    if (!rec->tramp) return 0;
    rec->installed = 0;
    if (rec->patch.live && code_unpatch(&rec->patch) != B2_PATCH_OK) return 0;
    if(rec->unwind_count) {
        if(!RtlDeleteFunctionTable(rec->unwind))return 0;
        rec->unwind_count=0;
    }
    if (!VirtualFree(rec->tramp, 0, MEM_RELEASE)) return 0;
    rec->tramp = NULL;
    return 1;
}

int hook_unpatch(void *tramp)
{
    int result = 0;
    AcquireSRWLockExclusive(&g_patch_lock);
    for (patch_record **slot = &g_patches; *slot; slot = &(*slot)->next) {
        patch_record *rec = *slot;
        if (rec->tramp != tramp) continue;
        result = revert_record(rec);
        if (result) {
            *slot = rec->next;
            HeapFree(GetProcessHeap(), 0, rec);
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    return result;
}

int hook_unpatch_all(void)
{
    int n = 0;
    AcquireSRWLockExclusive(&g_patch_lock);
    for (patch_record **slot = &g_patches; *slot;) {
        patch_record *rec = *slot;
        if (revert_record(rec)) {
            *slot = rec->next;
            HeapFree(GetProcessHeap(), 0, rec);
            ++n;
        } else slot = &rec->next;
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    return n;
}

int hook_installed_count(void)
{
    int n = 0;
    AcquireSRWLockShared(&g_patch_lock);
    for (patch_record *rec = g_patches; rec; rec = rec->next) if (rec->installed) n++;
    ReleaseSRWLockShared(&g_patch_lock);
    return n;
}

int hook_owned_count(void)
{
    int n = 0;
    AcquireSRWLockShared(&g_patch_lock);
    for (patch_record *rec = g_patches; rec; rec = rec->next) ++n;
    ReleaseSRWLockShared(&g_patch_lock);
    return n;
}
