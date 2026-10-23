#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/resource_joint_reads.c"

static int renderer,allow_image=1,recording=1,independent,prepares,commits,removes,fail_prepare=-1,fail_commit=-1,fail_remove;
static void *targets[2],*detours[2];static size_t lengths[2];static int installed[2];
static void original(void *,void *,const char *,void *);
static short *native_symbol(void *,short *,const char *);
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int r) { return allow_image && r==renderer; }
int sh_resource_graph_recording(void) { return recording; }
int sh_resource_graph_native_observing(void) { return recording || independent; }
static int graph_failed,fallback_unknown;
void sh_resource_graph_incomplete(void) {graph_failed++;}
int sh_resource_graph_native_observe_resource(void *resource) {assert(!resource);fallback_unknown++;return 0;}
void *hook_prepare(void *target,void *detour,size_t size)
{ int i=prepares++;assert(i<2 && detour);targets[i]=target;detours[i]=detour;lengths[i]=size;return i==fail_prepare?NULL:i?(void *)native_symbol:(void *)original; }
sh_patch_status hook_commit(void *tramp)
{
    assert(tramp);if(commits++==fail_commit)return B2_PATCH_FAIL_PROTECT;
    unsigned i=tramp==(void *)original?0:1;unsigned char *entry=targets[i];
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    memcpy(entry,jump,6);memcpy(entry+6,detours+i,8);memset(entry+14,0x90,lengths[i]-14);installed[i]=1;return B2_PATCH_OK;
}
int hook_is_installed(void *tramp) {return installed[tramp==(void *)original?0:1];}
int hook_unpatch(void *tramp)
{
    assert(tramp);unsigned i=tramp==(void *)original?0:1;removes++;installed[i]=0;
    if(fail_remove)return 0;
    memcpy(targets[i],i?jr_symbol_bytes:jr_list_bytes,lengths[i]);return 1;
}
static unsigned char *image,model[0x140],skeleton[0x48],symbol[8],table[0x40];
static void *observed[32],*reject;static size_t count;static int unknown,native_calls,native_mode;
static int symbol_calls,symbol_mode,observer_throw,unknown_throw,token_mutate,token_mint;
static uint64_t model_token=11,skeleton_token=12;
uint64_t sh_resource_instances_token(const void *object)
{return object==model?model_token:object==skeleton?skeleton_token:0;}
static void *fiber_main;
static void put(void *p,size_t offset,void *value) { memcpy((char *)p+offset,&value,sizeof value); }
static int observe(void *resource)
{
    if(!resource) {unknown++;if(unknown_throw)RaiseException(0xe0123459,0,0,NULL);return 0;}
    assert(count<32);observed[count++]=resource;
    if(token_mutate==1 && resource==model)model_token++;
    if(token_mutate==2 && resource==skeleton)skeleton_token++;
    if(token_mint && resource==model && !model_token)model_token=21;
    if(token_mint && resource==skeleton && !skeleton_token)skeleton_token=22;
    if(observer_throw)RaiseException(0xe0123458,0,0,NULL);
    return resource!=reject;
}
static void clear(void) {count=unknown=graph_failed=fallback_unknown=0;reject=NULL;token_mutate=token_mint=observer_throw=unknown_throw=0;}
static void reached(uintptr_t caller) { jr_reached(symbol,caller); }
static uintptr_t site(void) { return (uintptr_t)image+jr_bindings[renderer].continuation; }
static void original(void *skel,void *base,const char *pattern,void *out)
{
    assert(skel==skeleton && base==model && pattern && out==table);native_calls++;
    if(native_mode==1) RaiseException(0xe0123456,0,0,NULL);
    if(pattern[0]=='n')assert(jr_symbol_call(symbol,(short *)out,pattern,site())==(short *)out);
    if(native_mode==2)model_token++;
}
static short *native_symbol(void *self,short *result,const char *name)
{
    assert(self==symbol && name);symbol_calls++;
    if(symbol_mode)RaiseException(0xe0123457,0,0,NULL);
    return result;
}
static void reset(void)
{
    assert(!jr_scopes);jr_ready=jr_attempted=0;prepares=commits=removes=0;
    fail_prepare=fail_commit=-1;fail_remove=0;jr_fail_alloc=0;
    jr_list_original=NULL;jr_symbol_original=NULL;jr_base=NULL;jr_observe=NULL;
    memset(installed,0,sizeof installed);recording=1;independent=0;allow_image=1;
    model_token=11;skeleton_token=12;
    memcpy(image+jr_bindings[renderer].list,jr_list_bytes,sizeof jr_list_bytes);
    memcpy(image+jr_bindings[renderer].symbol,jr_symbol_bytes,sizeof jr_symbol_bytes);
    memcpy(image+jr_bindings[renderer].continuation-3,jr_site_bytes,sizeof jr_site_bytes);
}
static void setup(int r)
{
    renderer=r;const jr_binding *b=jr_bindings+r;
    memcpy(image+b->list,jr_list_bytes,sizeof jr_list_bytes);
    memcpy(image+b->symbol,jr_symbol_bytes,sizeof jr_symbol_bytes);
    memcpy(image+b->continuation-3,jr_site_bytes,sizeof jr_site_bytes);
    put(image,b->holder,symbol);put(symbol,0,table);put(table,0x38,image+b->symbol);
    put(model,0x138,skeleton);
}
static void installation(void)
{
    for(int r=0;r<2;r++) {
        setup(r);const jr_binding *b=jr_bindings+r;
        const uint32_t offsets[]={b->list,b->symbol,b->continuation-3};
        const size_t sizes[]={sizeof jr_list_bytes,sizeof jr_symbol_bytes,sizeof jr_site_bytes};
        for(int i=0;i<3;i++) for(size_t j=0;j<sizes[i];j++) {
            reset();image[offsets[i]+j]^=1;
            assert(!sh_resource_joint_reads_install(image,observe) && !prepares);
            image[offsets[i]+j]^=1;
        }
        for(int i=0;i<2;i++) {
            reset();fail_prepare=i;assert(!sh_resource_joint_reads_install(image,observe) && removes==i);
            reset();fail_commit=i;fail_remove=1;
            assert(!sh_resource_joint_reads_install(image,observe) && removes==2 && !sh_resource_joint_reads_ready());
            /* A reachable residual detour keeps its original and forwards. */
            jr_list_original=original;clear();jr_list_hook(skeleton,model,"name",table);
            assert(!count && unknown); /* Residual hook still forwards, but cannot claim completeness. */
        }
        reset();allow_image=0;assert(!sh_resource_joint_reads_install(image,observe) && !prepares);allow_image=1;
        reset();assert(sh_resource_joint_reads_install(image,observe) && prepares==2 && commits==2);
        assert(targets[0]==image+b->list && targets[1]==image+b->symbol && lengths[0]==19 && lengths[1]==15);
        assert(sh_resource_joint_reads_install(image,observe) && prepares==2);
        for(unsigned i=0;i<2;i++) {
            unsigned char *entry=targets[i];
            for(size_t j=0;j<lengths[i];j++) {
                unsigned char old=entry[j];entry[j]^=1;
                assert(!sh_resource_joint_reads_ready());entry[j]=old;
            }
            installed[i]=0;assert(!sh_resource_joint_reads_ready());installed[i]=1;
        }
        allow_image=0;assert(!sh_resource_joint_reads_ready());allow_image=1;
        renderer=1-r;assert(!sh_resource_joint_reads_ready());renderer=r;
        for(size_t j=0;j<sizeof jr_site_bytes;j++) {
            image[b->continuation-3+j]^=1;assert(!sh_resource_joint_reads_ready());image[b->continuation-3+j]^=1;
        }
        assert(sh_resource_joint_reads_ready());
    }
}
static void semantic(void)
{
    uint64_t context,a,b;int caught=0;
    for(int r=0;r<2;r++) {
        reset();setup(r);assert(sh_resource_joint_reads_install(image,observe));jr_list_original=original;
        clear();jr_list_hook(skeleton,model,"",table);jr_list_hook(skeleton,model,"-*",table);
        assert(!count && !unknown && !jr_scopes);
        jr_list_hook(skeleton,model,"name",table);
        assert(count==2 && observed[0]==model && observed[1]==skeleton && !unknown && !jr_scopes);
        clear();a=jr_enter(skeleton,model,&context);reached(site()+1);assert(!count && !unknown);
        recording=0;reached(site());assert(!count && !unknown);recording=1;
        reached(site());assert(count==2 && !unknown);jr_leave(context,a,1);
        /* Each independent DERIVED/DEFAULT/OWNER/FIELD consumer above a
         * barrier remains observable even while the outer graph is paused. */
        recording=0;
        for(int kind=1;kind<=4;kind++) {clear();independent=kind;jr_list_hook(skeleton,model,"name",table);assert(count==2 && !unknown && !jr_scopes);}
        independent=0;clear();jr_list_hook(skeleton,model,"name",table);assert(!count && !unknown);recording=1;
        clear();put(model,0x138,NULL);jr_list_hook(skeleton,model,"name",table);
        assert(!count && unknown && !jr_scopes);put(model,0x138,skeleton);
        clear();reject=skeleton;jr_list_hook(skeleton,model,"name",table);assert(count==2 && unknown);
        clear();put(table,0x38,image+jr_bindings[r].symbol+1);jr_list_hook(skeleton,model,"",table);
        assert(!count && unknown);put(table,0x38,image+jr_bindings[r].symbol);
        clear();jr_fail_alloc=1;jr_list_hook(skeleton,model,"name",table);assert(!count && unknown);jr_fail_alloc=0;
        clear();a=jr_enter(skeleton,model,&context);jr_fail_alloc=1;
        uint64_t nested;assert(!jr_enter(skeleton,model,&nested));jr_fail_alloc=0;
        reached(site());assert(!count && unknown);jr_leave(context,a,1);assert(!jr_scopes);
        /* Later independent traversal is eligible again. */
        clear();jr_list_hook(skeleton,model,"name",table);assert(count==2 && !unknown);
        clear();a=jr_enter(skeleton,model,&context);b=jr_enter(skeleton,model,&nested);
        jr_leave(context,a,1);reached(site());assert(!count && unknown);jr_leave(nested,b,1);assert(!jr_scopes);
        clear();native_mode=1;
        __try { jr_list_hook(skeleton,model,"name",table); }
        __except(GetExceptionCode()==0xe0123456?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught++; }
        native_mode=0;assert(unknown && !jr_scopes);
        clear();symbol_mode=1;
        __try {jr_list_hook(skeleton,model,"name",table);}
        __except(GetExceptionCode()==0xe0123457?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {caught++;}
        symbol_mode=0;assert(unknown && !count && !jr_scopes);
        clear();observer_throw=1;unknown_throw=1;
        jr_list_hook(skeleton,model,"name",table);assert(count==2 && unknown && graph_failed && fallback_unknown && !jr_scopes);
        clear();a=jr_enter(skeleton,model,&context);model_token++;
        reached(site());assert(unknown && !count);jr_leave(context,a,1);
        clear();a=jr_enter(skeleton,model,&context);skeleton_token++;
        reached(site());assert(unknown && !count);jr_leave(context,a,1);
        for(int token=1;token<=2;token++) {clear();token_mutate=token;jr_list_hook(skeleton,model,"name",table);assert(count==2 && unknown && !jr_scopes);}
        clear();native_mode=2;jr_list_hook(skeleton,model,"name",table);assert(count==2 && unknown && !jr_scopes);native_mode=0;
        clear();model_token=skeleton_token=0;jr_list_hook(skeleton,model,"name",table);assert(count==2 && unknown);
        clear();token_mint=1;jr_list_hook(skeleton,model,"name",table);assert(count==2 && !unknown);
        clear();a=jr_enter(skeleton,model,&context);reached(site());assert(count==2 && !unknown);
        model_token++;clear();reached(site());assert(!count && unknown);jr_leave(context,a,1);
        clear();a=jr_enter(skeleton,model,&context);
        assert(jr_symbol_call(symbol,NULL,"name",site()+1)==NULL && !count && !unknown);jr_leave(context,a,1);
        clear();installed[0]=0;jr_list_hook(skeleton,model,"name",table);assert(!count && unknown && !jr_scopes);installed[0]=1;
        clear();reached(site());assert(unknown && !count);
    }
    assert(caught==4 && native_calls>0 && symbol_calls>0);
}
static VOID WINAPI abandoned(void *unused)
{
    (void)unused;uint64_t context;assert(jr_enter(skeleton,model,&context));
    SwitchToFiber(fiber_main);assert(0);
}
static void fibers(void)
{
    clear();fiber_main=ConvertThreadToFiber(NULL);assert(fiber_main);
    uint64_t context;uint64_t serial=jr_enter(skeleton,model,&context);
    void *other=CreateFiber(0,abandoned,NULL);assert(other);SwitchToFiber(other);
    reached(site());assert(count==2 && !unknown); /* Other fiber's scope is invisible. */
    DeleteFiber(other);(void)sh_execution_context(&jr_owner,0);
    AcquireSRWLockShared(&jr_lock);assert(jr_scopes && !jr_scopes->next && jr_scopes->serial==serial);ReleaseSRWLockShared(&jr_lock);
    jr_leave(context,serial,1);assert(!jr_scopes && !unknown);
    assert(ConvertFiberToThread());
}
static void image_case(const char *path,int r)
{
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);assert(length>0);
    unsigned char *raw=malloc((size_t)length);assert(raw);assert(fread(raw,1,length,f)==(size_t)length);fclose(f);
    assert((size_t)length>sizeof(IMAGE_DOS_HEADER));IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)raw;
    assert(dos->e_magic==IMAGE_DOS_SIGNATURE && dos->e_lfanew>0 && (size_t)dos->e_lfanew+sizeof(IMAGE_NT_HEADERS64)<=(size_t)length);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);assert(nt->Signature==IMAGE_NT_SIGNATURE);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);const jr_binding *b=jr_bindings+r;
    const uint32_t offsets[]={b->list,b->symbol,b->continuation-3};
    const unsigned char *bytes[]={jr_list_bytes,jr_symbol_bytes,jr_site_bytes};
    const size_t sizes[]={sizeof jr_list_bytes,sizeof jr_symbol_bytes,sizeof jr_site_bytes};
    for(int i=0;i<3;i++) { int found=0;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
        IMAGE_SECTION_HEADER *s=sections+j;
        if(offsets[i]>=s->VirtualAddress && offsets[i]-s->VirtualAddress<=s->SizeOfRawData && sizes[i]<=s->SizeOfRawData-(offsets[i]-s->VirtualAddress)) {
            size_t offset=s->PointerToRawData+offsets[i]-s->VirtualAddress;
            assert(offset<=(size_t)length && sizes[i]<=(size_t)length-offset && !memcmp(raw+offset,bytes[i],sizes[i]));found=1;break;
        }
    } assert(found); } free(raw);
}
int main(int argc,char **argv)
{
    if(argc==3) { image_case(argv[1],atoi(argv[2]));puts("resource_joint_reads_image_test: passed");return 0; }
    image=VirtualAlloc(NULL,0x5000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(image);
    installation();semantic();fibers();assert(!jr_scopes);VirtualFree(image,0,MEM_RELEASE);
    puts("resource_joint_reads_test: passed");return 0;
}
