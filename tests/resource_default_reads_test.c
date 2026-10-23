/* Modeled native ownership/read contracts; no game process. */
#include <stdlib.h>
static int allocation_budget=-1;
static void *probe_calloc(size_t n,size_t size) { if(allocation_budget==0)return NULL; if(allocation_budget>0)allocation_budget--; return calloc(n,size); }
static void *probe_realloc(void *p,size_t size) { if(allocation_budget==0)return NULL; if(allocation_budget>0)allocation_budget--; return realloc(p,size); }
#define calloc probe_calloc
#define realloc probe_realloc
#include "../src/backend/resource_default_reads.c"
#undef calloc
#undef realloc
#include <stdio.h>

#define CHECK(x) do { if(!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)
#define IMAGE_BASE ((uintptr_t)UINT64_C(0x140000000))
typedef struct fixture_region { unsigned char *data; size_t size; } fixture_region;
typedef struct fixture {
    fixture_region regions[16000]; size_t count;
    unsigned char heads[sizeof(g_dr_pins)/sizeof(g_dr_pins[0])][16];
    int renderer;
    uintptr_t allocator, denied;
    sh_resource_default_read_input input;
} fixture;
static int failures;
static uintptr_t block(fixture *f,size_t size)
{
    unsigned char *data;
    CHECK(f->count<sizeof(f->regions)/sizeof(f->regions[0]));
    if(f->count>=sizeof(f->regions)/sizeof(f->regions[0])) exit(2);
    data=(unsigned char *)calloc(size?size:1,1); CHECK(data!=NULL); if(!data) exit(2);
    f->regions[f->count++]=(fixture_region){data,size?size:1};
    return (uintptr_t)data;
}
static void p64(uintptr_t a,size_t off,uintptr_t v) { uint64_t n=(uint64_t)v; memcpy((void *)(a+off),&n,8); }
static void p32(uintptr_t a,size_t off,uint32_t v) { memcpy((void *)(a+off),&v,4); }
static void p8(uintptr_t a,size_t off,unsigned char v) { *(unsigned char *)(a+off)=v; }
static uintptr_t pin(fixture *f,unsigned id) { return IMAGE_BASE+g_dr_pins[id].rva[f->renderer]; }
static int read_memory(void *context,uintptr_t a,void *out,size_t size)
{
    fixture *f=(fixture *)context; size_t i;
    uintptr_t holder=IMAGE_BASE+(f->renderer?0x55b7fd8u:0x3eba5f8u);
    if(a==f->denied || size>UINTPTR_MAX-a) return 0;
    if(a==holder && size==8) { uint64_t v=f->allocator; memcpy(out,&v,8); return 1; }
    for(i=0;i<sizeof(g_dr_pins)/sizeof(g_dr_pins[0]);i++) if(a==pin(f,(unsigned)i) && size==16) {
        memcpy(out,f->heads[i],size); return 1;
    }
    for(i=0;i<f->count;i++) {
        uintptr_t base=(uintptr_t)f->regions[i].data;
        if(a>=base && a-base<=f->regions[i].size && size<=f->regions[i].size-(a-base)) {
            memcpy(out,(const void *)a,size); return 1;
        }
    }
    return 0;
}
static void init(fixture *f,int renderer,unsigned routine)
{
    size_t i; uintptr_t vt,allocator_vt;
    memset(f,0,sizeof(*f)); f->renderer=renderer;
    for(i=0;i<sizeof(g_dr_pins)/sizeof(g_dr_pins[0]);i++)
        memcpy(f->heads[i],g_dr_pins[i].head[renderer],16);
    f->input=(sh_resource_default_read_input){IMAGE_BASE,0x8000000,renderer,1,0,0,read_memory,f,0};
    f->input.resource=block(f,0x200); vt=block(f,0x60);
    f->input.default_callback=pin(f,routine);
    p64(f->input.resource,0,vt); p64(vt,0x50,f->input.default_callback);
    f->allocator=block(f,8); allocator_vt=block(f,0x28);
    p64(f->allocator,0,allocator_vt);
    p64(allocator_vt,0x20,pin(f,DR_P_JSON_ALLOCATOR_FREE));
    p64(allocator_vt,0x10,pin(f,DR_P_JSON_ALLOCATOR_KEY_FREE));
}
static void destroy(fixture *f)
{ size_t i; for(i=0;i<f->count;i++) free(f->regions[i].data); }
static void raw_list(uintptr_t a,size_t off,uintptr_t data,uint32_t num,uint32_t cap,unsigned flags)
{ p64(a,off,data);p32(a,off+8,num);p32(a,off+12,cap);p8(a,off+19,(unsigned char)flags); }
static uintptr_t object(fixture *f,size_t size,unsigned callback)
{
    uintptr_t a=block(f,size),vt=block(f,0x18);
    p64(a,0,vt); p64(vt,0x10,pin(f,callback)); return a;
}
static void callback_at(fixture *f,uintptr_t a,unsigned callback,unsigned slot)
{
    uintptr_t vt=block(f,0x18);p64(a,0,vt);p64(vt,slot,pin(f,callback));
}
static uintptr_t pairs(fixture *f,uintptr_t value,size_t n)
{
    uintptr_t manager=block(f,0x18),array=n?block(f,n*8):0;
    size_t i; p64(value,0,manager);p32(value,8,7);raw_list(manager,0,array,(uint32_t)n,(uint32_t)n,0);
    for(i=0;i<n;i++) p64(array,i*8,block(f,0x28));
    return array;
}
static uintptr_t pair_value(uintptr_t array,size_t i)
{ uintptr_t pair;memcpy(&pair,(void *)(array+i*8),8);return pair+0x10; }
static int probe(fixture *f,sh_resource_default_read_receipt *r)
{
    int result=sh_resource_default_read_begin(r,&f->input);
    if(!result) CHECK(r->reason!=SH_DEFAULT_READ_OK);
    return result;
}
static void positive(fixture *f)
{
    sh_resource_default_read_receipt r;
    CHECK(probe(f,&r));CHECK(r.complete && r.reason==SH_DEFAULT_READ_OK);
    CHECK(sh_resource_default_read_finish(&r,&f->input,1,1));
    CHECK(!sh_resource_default_read_finish(&r,&f->input,1,1));
}
static void test_json(int renderer)
{
    fixture *f=(fixture *)calloc(1,sizeof(*f)); sh_resource_default_read_receipt r;
    uintptr_t array,value,custom,data,comments,pair,vt; size_t i;
    static const unsigned callbacks[]={DR_P_STRINGS2F0,DR_P_STRINGS_ARRAYS328,DR_P_STRING40,
        DR_P_RAW38A,DR_P_RAW38B,DR_P_RECORDS860,DR_P_BASE,DR_P_NESTED48,DR_P_NESTED78,
        DR_P_NESTED_D8,DR_P_NESTED28,DR_P_NESTED_LIST48};
    static const size_t sizes[]={0x2f0,0x328,0x40,0x38,0x38,0x860,8,0x48,0x78,0xd8,0x28,0x48};
    CHECK(f!=NULL);if(!f)exit(2);
    init(f,renderer,DR_P_JSON_DEFAULT);positive(f); /* native Clear ignores scalar root */
    destroy(f);
    init(f,renderer,DR_P_JSON_DEFAULT);
    array=pairs(f,f->input.resource+0x38,sizeof(callbacks)/sizeof(callbacks[0])+2);
    for(i=0;i<sizeof(callbacks)/sizeof(callbacks[0]);i++) {
        value=pair_value(array,i);custom=object(f,sizes[i],callbacks[i]);
        p64(value,0,custom);p32(value,8,8);
    }
    value=pair_value(array,12);p32(value,8,4);p8(value,12,1);p64(value,0,block(f,12));
    comments=block(f,8+3*8);p64(comments,0,3);p64(comments,8,block(f,9));p64(value,16,comments+8);
    value=pair_value(array,13);pairs(f,value,0); /* nested manager destructor */
    memcpy(&pair,(void *)array,8);p64(pair,0,block(f,12));p32(pair,8,1);
    /* Reach nonempty owned raw-buffer and owned-string cleanup too. */
    memcpy(&custom,(void *)pair_value(array,3),8);raw_list(custom,0x18,block(f,8),1,1,0);
    memcpy(&custom,(void *)pair_value(array,2),8);p64(custom+0x10,0x10,block(f,12));p32(custom+0x10,0x18,0x40000000);
    positive(f);
    /* Every native implementation contract is selected by callback, not a payload name. */
    allocation_budget=0;CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_ALLOCATION);allocation_budget=-1;
    value=pair_value(array,0);memcpy(&custom,(void *)value,8);
    memcpy(&vt,(void *)custom,8);p64(vt,0x10,IMAGE_BASE+0x123);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    p64(vt,0x10,pin(f,DR_P_STRINGS2F0));
    f->denied=value;CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f->denied=0;
    p32(value,8,9);CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_TYPE);p32(value,8,8);
    f->input.work_limit=2;CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f->input.work_limit=0;
    f->heads[DR_P_STRINGS2F0][7]^=1;CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_PIN);
    f->heads[DR_P_STRINGS2F0][7]^=1;
    f->input.pinned_image=0;CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_IMAGE);f->input.pinned_image=1;
    f->input.renderer=1-renderer;CHECK(!probe(f,&r));f->input.renderer=renderer;
    f->input.image_size=0x1000;CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_IMAGE);f->input.image_size=0x8000000;
    {
        uintptr_t saved=f->input.default_callback, resource_vt;
        memcpy(&resource_vt,(void *)f->input.resource,8);
        f->input.default_callback=IMAGE_BASE+0x1234;p64(resource_vt,0x50,f->input.default_callback);
        CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_UNSUPPORTED && r.routine==SH_DEFAULT_READ_ROUTINE_UNSUPPORTED);
        f->input.default_callback=saved;p64(resource_vt,0x50,saved);
        CHECK(probe(f,&r) && r.routine==SH_DEFAULT_READ_ROUTINE_JSON);
    }
    CHECK(probe(f,&r));CHECK(!sh_resource_default_read_finish(&r,&f->input,0,1) && r.reason==SH_DEFAULT_READ_NATIVE_RETURN);
    CHECK(probe(f,&r));CHECK(!sh_resource_default_read_finish(&r,&f->input,1,0) && r.reason==SH_DEFAULT_READ_ORDER);
    CHECK(probe(f,&r));f->input.resource++;CHECK(!sh_resource_default_read_finish(&r,&f->input,1,1));f->input.resource--;
    /* An actual JSON allocator callback change is not accepted as a plain free. */
    memcpy(&vt,(void *)f->allocator,8);p64(vt,0x20,IMAGE_BASE+0x456);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    p64(vt,0x20,pin(f,DR_P_JSON_ALLOCATOR_FREE));
    /* Malformed comments never become a silent successful receipt. */
    value=pair_value(array,12);p64(value,16,1);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);p64(value,16,comments+8);
    p64(comments,0,UINT64_MAX);CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);p64(comments,0,3);
    /* Same allocated string owned by two descendants is malformed. */
    value=pair_value(array,11);p32(value,8,4);p8(value,12,1);
    memcpy(&data,(void *)pair_value(array,12),8);p64(value,0,data);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    destroy(f);
    /* The root tree does not enumerate unused JSON capacity. */
    init(f,renderer,DR_P_JSON_DEFAULT);array=pairs(f,f->input.resource+0x38,2);
    memcpy(&custom,(void *)(f->input.resource+0x38),8);p32(custom,8,1);
    p32(pair_value(array,1),8,9);positive(f);
    p32(custom,8,3);CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    destroy(f);
    /* Ownership cycle is reported, not treated as successful truncation. */
    init(f,renderer,DR_P_JSON_DEFAULT);array=pairs(f,f->input.resource+0x38,1);
    memcpy(&custom,(void *)(f->input.resource+0x38),8);value=pair_value(array,0);
    p64(value,0,custom);p32(value,8,6);
    f->input.work_limit=10000; /* A broken cycle guard must terminate the negative control. */
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    destroy(f);
    /* More than a thousand nested containers remains iterative. */
    init(f,renderer,DR_P_JSON_DEFAULT);value=f->input.resource+0x38;
    for(i=0;i<1100;i++) {array=pairs(f,value,1);value=pair_value(array,0);}
    positive(f);destroy(f);free(f);
}
static void test_capacity(int renderer)
{
    fixture *f=(fixture *)calloc(1,sizeof(*f));sh_resource_default_read_receipt r;
    uintptr_t a,v,root,buffer,inner,vt;CHECK(f!=NULL);if(!f)exit(2);
    init(f,renderer,DR_P_JSON_DEFAULT);a=pairs(f,f->input.resource+0x38,1);v=pair_value(a,0);
    root=object(f,0x328,DR_P_STRINGS_ARRAYS328);p64(v,0,root);p32(v,8,8);
    buffer=block(f,2*0x48);raw_list(root,0x310,buffer,0,2,0);
    callback_at(f,buffer,DR_P_NESTED48,0x10);callback_at(f,buffer+0x48,DR_P_NESTED_LIST48,0x10);
    inner=block(f,0x78);raw_list(buffer+0x48,0x30,inner,0,1,0);
    callback_at(f,inner,DR_P_NESTED78,0x10);
    positive(f);
    memcpy(&vt,(void *)(buffer+0x48),8);p64(vt,0x10,IMAGE_BASE+0x1234);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    p64(vt,0x10,pin(f,DR_P_NESTED_LIST48));
    f->input.work_limit=10000;
    raw_list(buffer+0x48,0x30,buffer,0,1,0);CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    destroy(f);free(f);
}
static void test_file_anim(int renderer)
{
    fixture *f=(fixture *)calloc(1,sizeof(*f));sh_resource_default_read_receipt r;
    uintptr_t memory,vt,bytes,strings;CHECK(f!=NULL);if(!f)exit(2);
    init(f,renderer,DR_P_FILE_DEFAULT);positive(f);
    memory=block(f,0x180);vt=block(f,8);p64(memory,0,vt);p64(vt,0,pin(f,DR_P_MEMORY_DTOR));
    p64(f->input.resource,0x38,memory);bytes=block(f,8);p64(memory,0x168,bytes);p8(memory,0x178,1);
    positive(f);
    p64(vt,0,pin(f,DR_P_BASE));CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    p64(vt,0,pin(f,DR_P_MEMORY_DTOR));p64(memory+8,0x10,bytes);p32(memory+8,0x18,0x40000000);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    /* Native static name resets the inline pointer before destroying idStr. */
    p64(memory+8,0x10,memory+0x38);positive(f);destroy(f);
    init(f,renderer,DR_P_ANIMPARA_DEFAULT);positive(f);
    strings=block(f,2*0x30);raw_list(f->input.resource+0x38,0x80,strings,0,2,0);
    callback_at(f,strings,DR_P_STRING_DTOR,0);callback_at(f,strings+0x30,DR_P_STRING_DTOR,0);
    positive(f);
    memcpy(&vt,(void *)(strings+0x30),8);p64(vt,0,pin(f,DR_P_BASE));
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    /* A borrowed array is not destroyed, so its unused callback does not run. */
    p8(f->input.resource+0x38,0x80+19,1);positive(f);
    p8(f->input.resource+0x38,0x80+19,0);p32(f->input.resource+0x38,0x80+12,0xffffffffu);
    CHECK(!probe(f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    destroy(f);free(f);
}
int main(void)
{
    int renderer;for(renderer=0;renderer<2;renderer++) {test_json(renderer);test_capacity(renderer);test_file_anim(renderer);}
    if(failures) printf("%d failures\n",failures);else puts("resource_default_reads_test: OK");
    return failures?1:0;
}

