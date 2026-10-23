#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SH_RESOURCE_MATERIAL_READS_TESTING
#define SH_RESOURCE_MATERIAL_READS_TESTING
#endif
#include "../src/backend/resource_material_reads.c"

#include "native_pin_fixture.h"
static void mm_fixture(int renderer,uint32_t rva,void *out,size_t length)
{ if(!sh_native_pin_fixture_copy(renderer,rva,out,length))abort(); }

static int renderer,allow_image=1,observing=1,installed=1;
static int prepares,commits,removes,fail_prepare=-1,fail_commit=-1,fail_remove;
static void *targets[MM_ROUTES],*detours[MM_ROUTES];static size_t lengths[MM_ROUTES];static int relocations[MM_ROUTES];
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int r) { return allow_image && r==renderer; }
int sh_resource_graph_native_observing(void) { return observing; }
static int unknown,graph_failed,fallback_unknown,throw_unknown,throw_observe;
static void *seen[32],*reject;static size_t seen_count;
static unsigned char *token_changed;
uint64_t sh_resource_instances_token(const void *object)
{ return object==(void *)1 ? 0 : object==token_changed ? 9 : object ? 7 : 0; }
void sh_resource_graph_incomplete(void) { graph_failed++; }
int sh_resource_graph_native_observe_resource(void *object)
{ assert(!object);fallback_unknown++;return 0; }
static int observe(void *object)
{
    if(!object) { unknown++;if(throw_unknown) RaiseException(0xe0134001,0,0,NULL);return 0; }
    assert(seen_count<32);seen[seen_count++]=object;
    if(throw_observe) RaiseException(0xe0134002,0,0,NULL);
    return object!=reject;
}
static void clear(void)
{ unknown=graph_failed=fallback_unknown=0;seen_count=0;reject=NULL;throw_unknown=throw_observe=0;token_changed=NULL; }
static void *prepare(void *target,void *detour,size_t length,int relocation)
{
    int i=prepares++;assert(i<MM_ROUTES);if(i==fail_prepare)return NULL;
    targets[i]=target;detours[i]=detour;lengths[i]=length;relocations[i]=relocation;return target;
}
void *hook_prepare(void *target,void *detour,size_t length) { return prepare(target,detour,length,0); }
void *hook_prepare_rip_load(void *target,void *detour,size_t length,size_t offset)
{ assert(offset==4);return prepare(target,detour,length,1); }
sh_patch_status hook_commit(void *tramp)
{
    int i=commits++;assert(tramp);if(i==fail_commit)return B2_PATCH_FAIL_PROTECT;
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    memcpy(tramp,jump,6);memcpy((unsigned char *)tramp+6,detours+i,8);
    if(lengths[i]>14) memset((unsigned char *)tramp+14,0x90,lengths[i]-14);
    return B2_PATCH_OK;
}
int hook_is_installed(void *tramp) { return tramp && installed; }
int hook_unpatch(void *tramp)
{
    removes++;if(fail_remove)return 0;
    for(size_t i=0;i<MM_ROUTES;i++) if(targets[i]==tramp)
        mm_fixture(renderer,mm_images[renderer].routes[i].rva,tramp,mm_images[renderer].routes[i].stolen);
    return 1;
}
typedef struct region { uintptr_t address;const unsigned char *data;size_t length; } region;
typedef struct fixture { region rows[16];size_t count;uintptr_t denied;size_t reads; } fixture;
static void add(fixture *f,const void *data,size_t length)
{ assert(f->count<16);f->rows[f->count++]=(region){(uintptr_t)data,data,length}; }
static void ptr(void *p,size_t offset,const void *value) { memcpy((unsigned char *)p+offset,&value,8); }
static void integer(void *p,size_t offset,int32_t value) { memcpy((unsigned char *)p+offset,&value,4); }
static int reader(void *context,uintptr_t address,void *out,size_t length)
{
    fixture *f=context;f->reads++;
    if(address<=f->denied && f->denied-address<length) return 0;
    for(size_t i=0;i<f->count;i++) {
        const region *r=f->rows+i;
        if(address>=r->address && address-r->address<=r->length && length<=r->length-(address-r->address)) {
            memcpy(out,r->data+address-r->address,length);return 1;
        }
    }return 0;
}
static int probe(fixture *f,int kind,void *a,void *b)
{ clear();return sh_resource_material_reads_test_probe(kind,a,b,reader,f,observe); }
static void probes(void)
{
    fixture f={0};unsigned char program[0x16a0]={0},program2[0x16a0]={0},material[0x1b8]={0},definition[0x80]={0};
    unsigned char ops[24]={1,2,3},values[48]={4,5,6};int16_t indices[3]={4,5,6};
    add(&f,program,sizeof program);add(&f,program2,sizeof program2);add(&f,material,sizeof material);
    add(&f,definition,sizeof definition);add(&f,ops,sizeof ops);add(&f,values,sizeof values);add(&f,indices,sizeof indices);
    f.denied=(uintptr_t)program+0xe8;
    assert(probe(&f,MM_COPY,NULL,program+0xe8) && seen_count==1 && !unknown); /* Only source+8 is reached. */
    integer(program,0xf0,-1);assert(probe(&f,MM_COPY,NULL,program+0xe8));
    integer(program,0xf0,3);assert(!probe(&f,MM_COPY,NULL,program+0xe8) && unknown);
    f.denied=0;ptr(program,0xe8,ops);ptr(program,0x100,values);
    f.denied=(uintptr_t)program+0x108; /* Native source value-count is irrelevant. */
    assert(probe(&f,MM_COPY,NULL,program+0xe8) && seen_count==1);
    f.denied=(uintptr_t)values+47;assert(!probe(&f,MM_COPY,NULL,program+0xe8));
    f.denied=0;clear();reject=program;
    assert(!sh_resource_material_reads_test_probe(MM_COPY,NULL,program+0xe8,reader,&f,observe) && unknown);
    assert(!probe(&f,MM_COPY,NULL,(void *)0xe9)); /* Actual object has no known token. */
    assert(!probe(&f,MM_COPY,NULL,(void *)0x10));
    clear();throw_observe=1;assert(!sh_resource_material_reads_test_probe(MM_COPY,NULL,program+0xe8,reader,&f,observe) && unknown);

    assert(probe(&f,MM_FLAGS,material+0x70,NULL) && !seen_count);
    ptr(material,0x110,program);integer(program,0x1688,8);
    ptr(material,0x108,program2);f.denied=(uintptr_t)program2+0x1688;
    assert(probe(&f,MM_FLAGS,material+0x70,NULL) && seen_count==2); /* Its later +e0 read is real; unused +108 flags are not read. */
    integer(program,0x1688,0);
    assert(!probe(&f,MM_FLAGS,material+0x70,NULL));
    f.denied=0;ptr(material,0x108,program2);
    assert(probe(&f,MM_FLAGS,material+0x70,NULL) && seen_count==2);
    f.denied=(uintptr_t)program2+0x1688;assert(!probe(&f,MM_FLAGS,material+0x70,NULL));
    f.denied=0;ptr(material,0x110,NULL);
    assert(probe(&f,MM_FLAGS,material+0x70,NULL) && seen_count==1 && seen[0]==program2);
    for(size_t i=0;i<9;i++) ptr(material,0xf0+i*8,program);
    assert(probe(&f,MM_FLAGS,material+0x70,NULL) && seen_count==1);
    f.denied=(uintptr_t)program+0xe0;assert(!probe(&f,MM_FLAGS,material+0x70,NULL));f.denied=0;

    memset(program,0,sizeof program);integer(definition,0x68,4);
    f.denied=(uintptr_t)definition+0x68;
    assert(probe(&f,MM_HAS,program,definition) && seen_count==1); /* Empty lists never touch definition. */
    integer(program,0x120,-1);assert(probe(&f,MM_HAS,program,NULL) && seen_count==1);
    integer(program,0x120,3);ptr(program,0x118,indices);f.denied=0;
    assert(probe(&f,MM_HAS,program,definition) && seen_count==2);
    f.denied=(uintptr_t)indices+2;assert(probe(&f,MM_HAS,program,definition)); /* First-match return. */
    f.denied=(uintptr_t)program+0x1f8;assert(probe(&f,MM_HAS,program,definition));
    integer(definition,0x68,5);f.denied=(uintptr_t)indices+4;
    assert(probe(&f,MM_HAS,program,definition)); /* Second-match return. */
    integer(definition,0x68,8);assert(!probe(&f,MM_HAS,program,definition));
    f.denied=0;assert(probe(&f,MM_HAS,program,definition));
    clear();reject=definition;assert(!sh_resource_material_reads_test_probe(MM_HAS,program,definition,reader,&f,observe) && unknown);
    integer(program,0x120,0);integer(program,0x558,1);ptr(program,0x550,indices);integer(definition,0x68,4);
    assert(probe(&f,MM_HAS,program,definition)); /* Final stage/list really is reached. */
}

static unsigned char *image(void)
{
    unsigned char *base=VirtualAlloc(NULL,0x3100000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(base);
    const mm_image *p=mm_images+renderer;mm_fixture(renderer,p->parser,base+p->parser,p->parser_length);
    for(size_t i=0;i<MM_ROUTES;i++) mm_fixture(renderer,p->routes[i].rva,base+p->routes[i].rva,p->routes[i].length);
    return base;
}
static void reset(void)
{ sh_resource_material_reads_test_reset();prepares=commits=removes=0;fail_prepare=fail_commit=-1;fail_remove=0;clear(); }
static void installation(void)
{
    for(renderer=0;renderer<2;renderer++) {
        unsigned char *base=image();const mm_image *p=mm_images+renderer;
        for(size_t function=0;function<=MM_ROUTES;function++) {
            uint32_t rva=function?p->routes[function-1].rva:p->parser;
            size_t size=function?p->routes[function-1].length:p->parser_length;
            for(size_t byte=0;byte<size;byte++) {
                reset();base[rva+byte]^=1;
                assert(!sh_resource_material_reads_install(base,observe) && !prepares);
                base[rva+byte]^=1;
            }
        }
        for(int i=0;i<MM_ROUTES;i++) {
            reset();fail_prepare=i;assert(!sh_resource_material_reads_install(base,observe) && removes==i);
            reset();fail_commit=i;assert(!sh_resource_material_reads_install(base,observe) && removes==MM_ROUTES);
        }
        reset();assert(sh_resource_material_reads_install(base,observe) && prepares==MM_ROUTES && commits==MM_ROUTES);
        assert(sh_resource_material_reads_ready());
        allow_image=0;assert(!sh_resource_material_reads_ready());allow_image=1;
        installed=0;assert(!sh_resource_material_reads_ready());installed=1;
        base[p->parser+100]^=1;assert(!sh_resource_material_reads_ready());base[p->parser+100]^=1;
        for(size_t i=0;i<MM_ROUTES;i++) {
            const mm_route *r=p->routes+i;
            assert(lengths[i]==r->stolen && relocations[i]==r->relocation);
            for(size_t byte=0;byte<r->length;byte++) {
                base[r->rva+byte]^=1;assert(!sh_resource_material_reads_ready());base[r->rva+byte]^=1;
            }
        }
        assert(sh_resource_material_reads_ready());
        assert(sh_resource_material_reads_install(base,observe) && prepares==MM_ROUTES);
        VirtualFree(base,0,MEM_RELEASE);reset();
        base=image();fail_commit=1;fail_remove=1;
        assert(!sh_resource_material_reads_install(base,observe) && mm_copy_original && mm_integer_original && mm_has_original && mm_sort_original);
        assert(!sh_resource_material_reads_ready());VirtualFree(base,0,MEM_RELEASE);reset();
    }
}
static int calls,mutate,throw_native,original_observations;
static unsigned char *changed_resource;
static void copy_original(void *destination,void *source)
{
    assert(destination);calls++;original_observations=(int)seen_count;
    if(throw_native) RaiseException(0xe0134003,0,0,NULL);
    if(mutate==1) ((unsigned char *)source)[8]^=1;
    if(mutate==2) token_changed=changed_resource;
}
static int integer_original(void *block,void *definition,uintptr_t fallback)
{
    assert(block && definition && fallback==0x1122334455667788);calls++;original_observations=(int)seen_count;
    if(throw_native) RaiseException(0xe0134003,0,0,NULL);
    return -17;
}
static unsigned char has_original(void *program,void *definition)
{
    assert(program && definition);calls++;original_observations=(int)seen_count;
    if(throw_native) RaiseException(0xe0134003,0,0,NULL);
    if(mutate==2) token_changed=changed_resource;
    return mutate==3 ? 1 : 0;
}
static float sort_value;
static char *video_value;
static void *video_original(void *block,void *definition,uintptr_t fallback)
{
    assert(block && definition && fallback==0x1122334455667788);calls++;original_observations=(int)seen_count;
    if(throw_native)RaiseException(0xe0134003,0,0,NULL);
    if(mutate==4 && video_value)video_value[0]='v';
    return video_value;
}
static void native_float_branch(void)
{
    /* Execute the same UCOMISS/JP/JE decision on isolated CPU values. This
     * is an instruction control, not execution of the game's parser. */
    static const unsigned char bytes[]={0x66,0x0f,0x6e,0xc1,0x0f,0x57,0xc9,0x0f,0x2e,0xc1,0x7a,0x02,0x74,0x06,0xb8,1,0,0,0,0xc3,0x33,0xc0,0xc3};
    unsigned char *code=VirtualAlloc(NULL,sizeof bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(code);memcpy(code,bytes,sizeof bytes);
    DWORD old;assert(VirtualProtect(code,sizeof bytes,PAGE_EXECUTE_READ,&old));assert(FlushInstructionCache(GetCurrentProcess(),code,sizeof bytes));
    int (*branch)(uint32_t)=(int (*)(uint32_t))code;unsigned saved=_mm_getcsr();
    struct { uint32_t bits;unsigned control;int reached,trapped; } cases[]={
        {0,0x180,0,0},{0x80000000,0x180,0,0},{0x7fc01234,0x180,1,0},
        {0x7f801234,0x180,1,0},{0x7f801234,0x100,0,1},
        {1,0x1c0,0,0},{1,0x180,1,0},{1,0x80,0,1}
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        int result=0,trapped=0;_mm_setcsr((saved&~0x1ffu)|cases[i].control);
        __try { result=branch(cases[i].bits); }
        __except(EXCEPTION_EXECUTE_HANDLER) { trapped=1; }
        _mm_setcsr(saved);assert(trapped==cases[i].trapped && (trapped || result==cases[i].reached));
    }
    VirtualFree(code,0,MEM_RELEASE);
}
static float sort_original(void *block,void *definition,uintptr_t fallback)
{
    assert(block && definition && fallback==0x1122334455667788);calls++;original_observations=(int)seen_count;
    if(throw_native) RaiseException(0xe0134003,0,0,NULL);
    return sort_value;
}
static void wrappers(int r)
{
    renderer=r;reset();unsigned char *base=image();assert(sh_resource_material_reads_install(base,observe));
    mm_copy_original=copy_original;mm_integer_original=integer_original;mm_has_original=has_original;mm_sort_original=sort_original;mm_video_original=video_original;
    unsigned char program[0x16a0]={0},material[0x1b8]={0},definition[0x80]={0},destination[0x40]={0},threshold[0x90]={0};
    const mm_image *p=mm_images+r;uintptr_t copy=(uintptr_t)base+p->copy_return,integer_return=(uintptr_t)base+p->integer_return;
    uintptr_t has=(uintptr_t)base+p->has_returns[0];changed_resource=program;
    clear();mm_copy_at(destination,program+0xe8,copy);
    assert(seen_count==1 && seen[0]==program && original_observations==1 && !unknown);
    clear();mm_copy_at(destination,program+0xe8,copy+1);assert(!seen_count && !unknown);
    clear();mutate=1;mm_copy_at(destination,program+0xe8,copy);assert(unknown);mutate=0;integer(program,0xf0,0);
    clear();mutate=2;mm_copy_at(destination,program+0xe8,copy);assert(unknown);mutate=0;
    clear();throw_observe=1;int prior=calls;mm_copy_at(destination,program+0xe8,copy);
    assert(calls==prior+1 && unknown);throw_observe=0;
    clear();throw_observe=throw_unknown=1;prior=calls;mm_copy_at(destination,program+0xe8,copy);
    assert(calls==prior+1 && fallback_unknown);throw_observe=throw_unknown=0;
    clear();throw_native=1;int caught=0;
    __try { mm_copy_at(destination,program+0xe8,copy); }
    __except(GetExceptionCode()==0xe0134003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
    assert(caught && unknown);throw_native=0;

    ptr(material,0x110,program);integer(program,0x1688,8);
    clear();assert(mm_integer_at(material+0x70,definition,0x1122334455667788,integer_return)==-17);
    assert(seen_count==1 && original_observations==0 && !unknown); /* Observe only after native returned. */
    clear();assert(mm_integer_at(material+0x70,definition,0x1122334455667788,integer_return-1)==-17 && !seen_count);
    clear();throw_native=1;caught=0;
    __try { mm_integer_at(material+0x70,definition,0x1122334455667788,integer_return); }
    __except(GetExceptionCode()==0xe0134003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
    assert(caught && !seen_count && !unknown);throw_native=0;
    clear();throw_observe=1;assert(mm_integer_at(material+0x70,definition,0x1122334455667788,integer_return)==-17 && unknown);throw_observe=0;

    clear();assert(mm_has_at(program,definition,has)==0 && seen_count==1 && original_observations==1 && !unknown);
    for(size_t i=0;i<3;i++) { clear();mm_has_at(program,definition,(uintptr_t)base+p->has_returns[i]);assert(seen_count==1 && !unknown); }
    clear();mutate=3;assert(mm_has_at(program,definition,has)==1 && unknown);mutate=0; /* Native/probe return disagreement. */
    clear();mutate=2;mm_has_at(program,definition,has);assert(unknown);mutate=0;
    clear();throw_native=1;caught=0;
    __try { mm_has_at(program,definition,has); }
    __except(GetExceptionCode()==0xe0134003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
    assert(caught && unknown);throw_native=0;
    clear();observing=0;prior=calls;
    mm_copy_at(destination,program+0xe8,copy);mm_has_at(program,definition,has);
    assert(mm_integer_at(material+0x70,definition,0x1122334455667788,integer_return)==-17);
    assert(!seen_count && !unknown && calls==prior+3);observing=1;
    clear();installed=0;prior=calls;mm_copy_at(destination,program+0xe8,copy);
    assert(calls==prior+1 && unknown);installed=1;
    uintptr_t sort=(uintptr_t)base+p->sort_return;
    ptr(base,p->sort_slot,threshold);ptr(base,p->uber_slot,NULL);
    clear();sort_value=0.0f;assert(mm_sort_at((void *)1,definition,0x1122334455667788,sort)==0.0f && !seen_count && !unknown);
    uint32_t bits=0x80000000;memcpy(&sort_value,&bits,4);float result=mm_sort_at((void *)1,definition,0x1122334455667788,sort);
    uint32_t actual;memcpy(&actual,&result,4);assert(actual==bits && !seen_count && !unknown);
    clear();sort_value=1.0f;ptr(material,0x110,NULL);
    assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==1.0f && !seen_count && !unknown);
    ptr(material,0x110,(void *)1);ptr(base,p->uber_slot,(void *)1);
    assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==1.0f && !seen_count && !unknown); /* No body read at identity-only branch. */
    ptr(material,0x110,program);ptr(base,p->uber_slot,NULL);
    const uint32_t excluded[]={0x38,1u<<17,0x40,0x2040,0x4040};
    for(size_t i=0;i<sizeof excluded/sizeof *excluded;i++) {
        clear();integer(program,0xe0,(int32_t)excluded[i]);ptr(base,p->sort_slot,(void *)1);
        assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==1.0f && seen_count==1 && !unknown);
    }
    integer(program,0xe0,0);ptr(base,p->sort_slot,threshold);
    clear();assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==1.0f && seen_count==2 && seen[1]==threshold && !unknown);
    clear();integer(program,0xe0,0x6040);sort_value=-1.0f;
    assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==-1.0f && seen_count==2 && !unknown);
    clear();bits=0x7fc01234;memcpy(&sort_value,&bits,4);result=mm_sort_at(material+0x70,definition,0x1122334455667788,sort);
    memcpy(&actual,&result,4);assert(actual==bits && seen_count==2 && !unknown); /* Native unordered branch reaches the threshold. */
    unsigned saved_control=_mm_getcsr();
    clear();bits=1;memcpy(&sort_value,&bits,4);_mm_setcsr(saved_control|0x40);
    result=mm_sort_at(material+0x70,definition,0x1122334455667788,sort);memcpy(&actual,&result,4);
    assert(actual==bits && !seen_count && !unknown && _mm_getcsr()==(saved_control|0x40)); /* DAZ is native zero. */
    clear();_mm_setcsr(saved_control&~0x140u);
    result=mm_sort_at(material+0x70,definition,0x1122334455667788,sort);memcpy(&actual,&result,4);
    assert(actual==bits && !seen_count && unknown && _mm_getcsr()==(saved_control&~0x140u)); /* Native UCOMISS would trap before the body. */
    clear();bits=0x7f801234;memcpy(&sort_value,&bits,4);_mm_setcsr(saved_control&~0x80u);
    result=mm_sort_at(material+0x70,definition,0x1122334455667788,sort);memcpy(&actual,&result,4);
    assert(actual==bits && !seen_count && unknown && _mm_getcsr()==(saved_control&~0x80u));
    clear();_mm_setcsr(saved_control|0x180u);
    result=mm_sort_at(material+0x70,definition,0x1122334455667788,sort);memcpy(&actual,&result,4);
    assert(actual==bits && seen_count==2 && !unknown && _mm_getcsr()==(saved_control|0x180u)); /* Masked signaling NaN follows native unordered branch. */
    _mm_setcsr(saved_control);
    clear();sort_value=1.0f;ptr(base,p->sort_slot,(void *)1);
    assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==1.0f && unknown);
    ptr(base,p->sort_slot,threshold);clear();throw_observe=1;
    assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort)==1.0f && unknown);throw_observe=0;
    clear();assert(mm_sort_at(material+0x70,definition,0x1122334455667788,sort+1)==1.0f && !seen_count && !unknown);
    clear();throw_native=1;caught=0;
    __try { mm_sort_at(material+0x70,definition,0x1122334455667788,sort); }
    __except(GetExceptionCode()==0xe0134003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
    assert(caught && !seen_count && !unknown);throw_native=0;
    uintptr_t video=(uintptr_t)base+p->video_return;char value[16]={0};
    clear();video_value=NULL;assert(mm_video_at(material+0x70,(void *)1,0x1122334455667788,video)==NULL && !seen_count && !unknown);
    clear();video_value=value;assert(mm_video_at(material+0x70,definition,0x1122334455667788,video)==value && seen_count==1 && seen[0]==definition && !unknown);
    /* Earlier empty state is not the actual branch decision. Native GetString
     * can return a changed/nonempty value; the exact return guard poisons the
     * owner before Bink while forwarding that identical pointer. */
    clear();mutate=4;assert(!value[0]);assert(mm_video_at(material+0x70,definition,0x1122334455667788,video)==value && value[0]=='v' && unknown);mutate=0;
    clear();assert(mm_video_at(material+0x70,definition,0x1122334455667788,video+1)==value && !seen_count && !unknown);
    clear();video_value=(char *)1;assert(mm_video_at(material+0x70,definition,0x1122334455667788,video)==(void *)1 && unknown);
    clear();video_value=value;value[0]=0;throw_observe=1;assert(mm_video_at(material+0x70,definition,0x1122334455667788,video)==value && unknown);throw_observe=0;
    clear();throw_native=1;caught=0;
    __try{mm_video_at(material+0x70,definition,0x1122334455667788,video);}
    __except(GetExceptionCode()==0xe0134003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
    assert(caught && !seen_count && !unknown);throw_native=0;
    VirtualFree(base,0,MEM_RELEASE);reset();
}
static void image_case(const char *path,int r)
{
    FILE *file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);assert(length>0);
    unsigned char *raw=malloc((size_t)length);assert(raw && fread(raw,1,length,file)==(size_t)length);fclose(file);
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)raw;assert(dos->e_magic==IMAGE_DOS_SIGNATURE);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);assert(nt->Signature==IMAGE_NT_SIGNATURE);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);const mm_image *p=mm_images+r;
    for(size_t i=0;i<=MM_ROUTES;i++) {
        uint32_t rva=i?p->routes[i-1].rva:p->parser;
        size_t size=i?p->routes[i-1].length:p->parser_length;
        const unsigned char *bytes=i?p->routes[i-1].digest:p->parser_digest;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            IMAGE_SECTION_HEADER *s=sections+j;
            if(rva>=s->VirtualAddress && rva-s->VirtualAddress<=s->SizeOfRawData && size<=s->SizeOfRawData-(rva-s->VirtualAddress)) {
                size_t offset=s->PointerToRawData+rva-s->VirtualAddress;
                assert(offset<=(size_t)length && size<=(size_t)length-offset && sh_native_pin_fixture_digest(raw+offset,size,bytes));found=1;break;
            }
        }assert(found);
    }
    free(raw);puts("resource_material_reads_image_test: passed");
}
int main(int argc,char **argv)
{
    if(argc==3) { image_case(argv[1],atoi(argv[2]));return 0; }
    native_float_branch();probes();installation();wrappers(0);wrappers(1);
    puts("resource_material_reads_test: passed");return 0;
}

