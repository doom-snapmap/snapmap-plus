#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "decl_source_file_reads.h"
#include "overrides.h"
#include "resource_file_visibility.h"
#include "resource_graph_native.h"
#include "host_image.h"
#include "decl_diagnostic_reads.h"
#include "native_pin_hash.h"

typedef struct sf_pin {uint32_t rva;size_t length;unsigned char digest[32];} sf_pin;
typedef struct sf_profile {
    uint32_t holder,object,table,manager,mtable,open,script;
    uint32_t callbacks[2],fs[20],native[5],setters[4],client_tables[2],vcs[2],files[6][11],clients[2][4];
    struct {uint32_t return_pc;unsigned kind;} sites[8];unsigned site_count;
    uint32_t atomic_table,atomic[3],zip_alloc[2];
    const uint32_t *vcs_methods;size_t vcs_method_count;
    const sf_pin *pins;size_t count;
} sf_profile;
#include "decl_source_file_read_pins.h"
typedef struct sf_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} sf_probe;
static const unsigned char *sf_base;static size_t sf_size;static int sf_renderer=-1;
static volatile LONG sf_setter_mask,sf_writers,sf_generation_failed;
static volatile LONG64 sf_generation=2;
static void *sf_original_setters[4];
static DWORD sf_original_protection[4];
static const size_t sf_setter_slots[4]={0x28,0x30,0x38,0x40};
static void sf_writer_begin(void)
{InterlockedIncrement(&sf_writers);if(!InterlockedIncrement64(&sf_generation))InterlockedExchange(&sf_generation_failed,1);}
static void sf_writer_end(DWORD error)
{if(!InterlockedIncrement64(&sf_generation))InterlockedExchange(&sf_generation_failed,1);InterlockedDecrement(&sf_writers);SetLastError(error);}
static void sf_set28(void *self,void *callback)
{sf_writer_begin();__try {((void (*)(void *,void *))sf_original_setters[0])(self,callback);}__finally {sf_writer_end(GetLastError());}}
static void sf_set30(void *self,void *callback)
{sf_writer_begin();__try {((void (*)(void *,void *))sf_original_setters[1])(self,callback);}__finally {sf_writer_end(GetLastError());}}
static void sf_set38(void *self,void *read,void *write)
{sf_writer_begin();__try {((void (*)(void *,void *,void *))sf_original_setters[2])(self,read,write);}__finally {sf_writer_end(GetLastError());}}
static void sf_set40(void *self,void *callback)
{sf_writer_begin();__try {((void (*)(void *,void *))sf_original_setters[3])(self,callback);}__finally {sf_writer_end(GetLastError());}}
static void *sf_setters[4]={(void *)sf_set28,(void *)sf_set30,(void *)sf_set38,(void *)sf_set40};
static int sf_setters_owned(void)
{
    if(sf_setter_mask!=15 || !sf_base || sf_renderer<0 || sf_renderer>1)return 0;
    const sf_profile *b=&sf_profiles[sf_renderer];
    __try {for(unsigned i=0;i<4;i++)if(*(void **)(sf_base+b->table+sf_setter_slots[i])!=sf_setters[i] ||
       sf_original_setters[i]!=(void *)(sf_base+b->setters[i]))return 0;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 1;
}
static int sf_fail(sf_probe *p,sh_resource_default_read_reason reason,uintptr_t at)
{p->r->reason=reason;p->r->offending_address=at;return 0;}
static int sf_read(sf_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if(!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))return sf_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->in->work_limit && p->r->work>=p->in->work_limit)return sf_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    p->r->work++;
    return p->in->read(p->in->read_context,at+offset,out,size) || sf_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
}
static int sf_pins(sf_probe *p)
{
    const sf_profile *b=&sf_profiles[p->in->renderer];unsigned char bytes[128];
    for(size_t i=0;i<b->count;i++){
        const sf_pin *pin=b->pins+i;sh_native_pin_hash hash;unsigned char digest[32];sh_native_pin_hash_init(&hash);
        if(pin->rva>p->in->image_size || pin->length>p->in->image_size-pin->rva)return sf_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
        for(size_t off=0;off<pin->length;off+=sizeof bytes){size_t n=pin->length-off;if(n>sizeof bytes)n=sizeof bytes;
            if(!sf_read(p,p->in->image_base+pin->rva,off,bytes,n))return 0;
            sh_native_pin_hash_update(&hash,bytes,n);}
        sh_native_pin_hash_finish(&hash,digest);
        if(memcmp(digest,pin->digest,sizeof digest))return sf_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
    }return 1;
}
static int sf_slot(sf_probe *p,uintptr_t table,size_t slot,uint32_t expected)
{uintptr_t actual;if(!sf_read(p,table,slot,&actual,8))return 0;return actual==p->in->image_base+expected || sf_fail(p,SH_DEFAULT_READ_CALLBACK,actual);}
static int sf_source_control(sf_probe *p,uintptr_t object)
{
    const sf_profile *b=&sf_profiles[p->in->renderer];uintptr_t *seen=NULL;size_t count=0,capacity=0;int ok=0;
    while(object){
        for(size_t i=0;i<count;i++)if(seen[i]==object){sf_fail(p,SH_DEFAULT_READ_OWNERSHIP,object);goto done;}
        if(count==capacity){size_t next=capacity?capacity*2:16;
            if(next<capacity || next>SIZE_MAX/sizeof *seen){sf_fail(p,SH_DEFAULT_READ_ALLOCATION,object);goto done;}
            void *memory=realloc(seen,next*sizeof *seen);if(!memory){sf_fail(p,SH_DEFAULT_READ_ALLOCATION,object);goto done;}
            seen=memory;capacity=next;}
        uintptr_t table,callback;if(!sf_read(p,object,0,&table,8))goto done;
        if(!count){
            /* The stock wrapper's Init is an exact false-return leaf. All
             * guarded operations therefore stay in their native fallback;
             * GetAvailable alone can follow a populated inner wrapper. */
            for(size_t i=1;i<b->vcs_method_count;i++)if(!sf_slot(p,table,i*8,b->vcs_methods[i]))goto done;
        }
        if(!sf_read(p,table,0x40,&callback,8))goto done;
        if(callback==p->in->image_base+b->vcs_methods[1]){ok=1;goto done;}
        if(callback!=p->in->image_base+b->vcs_methods[8]){sf_fail(p,SH_DEFAULT_READ_CALLBACK,callback);goto done;}
        seen[count++]=object;
        if(!sf_read(p,object,8,&object,8))goto done;
    }ok=1;
done:free(seen);return ok;
}
static void sf_native(const sf_profile *b,uintptr_t base,sh_resource_file_visibility_native *n)
{
    n->slot=(void **)(base+b->mtable+0x68);n->open=(void *)(base+b->native[0]);
    n->operator_new=(void *)(base+b->native[1]);n->operator_delete=(void *)(base+b->native[2]);
    n->memory_file=(void *)(base+b->native[3]);n->fill=(void *)(base+b->native[4]);
}
static uint64_t sf_opening(uintptr_t caller,void *provider);
static void sf_returned(void *stream,uintptr_t caller,void *provider,uint64_t stamp);
static int sf_client_read(void *context,sh_resource_default_read_receipt *r,
    const sh_resource_default_read_input *in,uintptr_t client)
{
    sf_probe p={r,in};uintptr_t table;const sf_profile *b=&sf_profiles[in->renderer];
    if(!sf_read(&p,client,0,&table,8))return 0;
    for(unsigned i=0;i<2;i++){
        if(table!=in->image_base+b->client_tables[i])continue;
        uintptr_t read;if(!sf_read(&p,table,0x30,&read,8))return 0;
        if(read==in->image_base+b->clients[i][2]){
            if(context && !sf_slot(&p,table,0x60,b->clients[i][3]))return 0;
            uintptr_t again,current;if(!sf_read(&p,client,0,&again,8) || !sf_read(&p,table,0x30,&current,8))return 0;
            return (again==table && current==read) || sf_fail(&p,SH_DEFAULT_READ_OWNERSHIP,client);
        }
    }return sf_fail(&p,SH_DEFAULT_READ_CALLBACK,table);
}
static int sf_context(sf_probe *p)
{
    const sf_profile *b=&sf_profiles[p->in->renderer];uintptr_t object,table,callback;
    if(!sf_pins(p) || !sf_read(p,p->in->image_base,b->holder,&object,8) ||
       !sf_read(p,object,0,&table,8))return 0;
    if(object!=p->in->image_base+b->object || table!=p->in->image_base+b->table)return sf_fail(p,SH_DEFAULT_READ_CALLBACK,object);
    if(!sf_setters_owned() || !sh_overrides_file_return_observer_ready(sf_opening,sf_returned,(void *)table,(void *)(p->in->image_base+b->open)))
        return sf_fail(p,SH_DEFAULT_READ_CALLBACK,table+0xf8);
    const size_t slots[5]={0x20,0x68,0x90,0x120,0x140};
    for(unsigned i=0;i<5;i++)if(!sf_slot(p,table,slots[i],b->fs[i]))return 0;
    uintptr_t source_control;int source_control_enabled;
    if(!sf_read(p,p->in->image_base,b->vcs[0],&source_control,8) ||
       !sf_read(p,p->in->image_base,b->vcs[1],&source_control_enabled,4))return 0;
    if(source_control && !sf_source_control(p,source_control))return 0;
    (void)source_control_enabled;
    /* The two observed stock registrations enter FILE lookup or the actual
     * background-loader cache. Other mutable registrations require a real
     * callback contract; a non-null pointer is never treated as harmless. */
    if(!sf_read(p,object,0x180,&callback,8))return 0;
    if(callback){
        if(callback!=p->in->image_base+b->callbacks[0] && callback!=p->in->image_base+b->callbacks[1])
            return sf_fail(p,SH_DEFAULT_READ_CALLBACK,callback);
        uintptr_t manager,mtable;
        if(!sf_read(p,p->in->image_base,b->manager,&manager,8) || !sf_read(p,manager,0,&mtable,8))return 0;
        if(mtable!=p->in->image_base+b->mtable)return sf_fail(p,SH_DEFAULT_READ_CALLBACK,mtable);
        sh_resource_file_visibility_native n;sf_native(b,p->in->image_base,&n);
        if(!sh_resource_file_visibility_dispatch_ready(&n))return sf_fail(p,SH_DEFAULT_READ_CALLBACK,mtable+0x68);
    }
    for(size_t off=0x188;off<=0x1a0;off+=8){if(!sf_read(p,object,off,&callback,8))return 0;if(callback)return sf_fail(p,SH_DEFAULT_READ_CALLBACK,callback);}
    for(unsigned i=0;i<2;i++)if(!sf_slot(p,p->in->image_base+b->client_tables[i],0x30,b->clients[i][2]))return 0;
    return sh_decl_file_factory_read_check(p->r,p->in,sf_client_read,NULL);
}
static int sf_stream_one(sf_probe *p,void *stream,unsigned kind,void **nested)
{
    *nested=NULL;
    if(!stream)return 1; /* Native failed-open path has no file dispatch. */
    sh_overrides_file_dispatch d;if(!sh_overrides_file_dispatch_snapshot(stream,&d))return sf_fail(p,SH_DEFAULT_READ_CALLBACK,(uintptr_t)stream);
    const sf_profile *b=&sf_profiles[p->in->renderer];unsigned row;
    if(d.product){
        if(kind==2 && d.read_string!=p->in->image_base+b->files[0][7])return sf_fail(p,SH_DEFAULT_READ_CALLBACK,d.read_string);
        return 1; /* Exact product table and reached Tell authenticated by its owner. */
    }
    for(row=0;row<6;row++){
        unsigned i;for(i=0;i<5;i++)if(d.slots[i]!=p->in->image_base+b->files[row][i])break;
        if(i==5)break;
    }
    if(row==6)return sf_fail(p,SH_DEFAULT_READ_CALLBACK,d.table);
    /* The error-name and ReadAt branches dispatch on the actual returned
     * receiver. A matching public Read alone does not qualify either. */
    if(!sf_slot(p,d.table,0x20,b->files[row][5]) || !sf_slot(p,d.table,0x38,b->files[row][6]))return 0;
    if(kind==2 && (d.read_string!=p->in->image_base+b->files[row][7]))return sf_fail(p,SH_DEFAULT_READ_CALLBACK,d.read_string);
    if(d.receipt && d.shader && d.tell!=p->in->image_base+b->files[row][8])return sf_fail(p,SH_DEFAULT_READ_CALLBACK,d.tell);
    if(kind==4 && !sf_slot(p,d.table,0xa0,b->files[row][9]))return 0;
    if(kind==7 && !sf_slot(p,d.table,0xa0,b->files[row][9]))return 0;
    if(kind==6 && !sf_slot(p,d.table,0x70,b->files[row][10]))return 0;
    if(kind==6 && row==2 && d.tell!=p->in->image_base+b->files[row][8])return sf_fail(p,SH_DEFAULT_READ_CALLBACK,d.tell);
    if(row==2){
        uintptr_t archive,backing,state;
        if(!sf_read(p,(uintptr_t)stream,0x70,&archive,8) || !sf_read(p,archive,0,&backing,8) ||
           !sf_read(p,archive,0x80,&state,8))return 0;
        if(!backing)return sf_fail(p,SH_DEFAULT_READ_OWNERSHIP,archive);
        if(state){uintptr_t actual;int method,initialized;
            if(!sf_read(p,state,0x88,&actual,8) || !sf_read(p,state,0x90,&method,4) || !sf_read(p,state,0x64,&initialized,4))return 0;
            if(actual!=backing)return sf_fail(p,SH_DEFAULT_READ_OWNERSHIP,state);
            if(method || initialized)for(unsigned i=0;i<2;i++){
                if(!sf_read(p,state,0x38+i*8,&actual,8))return 0;
                if(actual!=p->in->image_base+b->zip_alloc[i])return sf_fail(p,SH_DEFAULT_READ_CALLBACK,actual);
            }
        }
        /* Unzip copies this exact backing into new decode state, and its
         * cursor/decode helpers dispatch backing Seek70 and Read28. Inspect
         * that actual receiver too; no archive-name inference or depth cap. */
        *nested=(void *)backing;
    }
    if(row==4){
        uintptr_t client,table; if(!sf_read(p,(uintptr_t)stream,0xf8,&client,8))return 0;
        if(client){
            if(!sf_read(p,client,0,&table,8))return 0;
            unsigned j;for(j=0;j<2;j++){
                uintptr_t actual[3];const size_t off[3]={0,0x28,0x30};unsigned k;
                for(k=0;k<3;k++){if(!sf_read(p,table,off[k],actual+k,8))return 0;if(actual[k]!=p->in->image_base+b->clients[j][k])break;}
                if(k==3)break;
            }if(j==2)return sf_fail(p,SH_DEFAULT_READ_CALLBACK,table);
        }
    }
    return 1;
}
static int sf_stream(sf_probe *p,void *stream,unsigned kind)
{
    void **seen=NULL;size_t count=0,capacity=0;int ok=0;
    while(stream){
        for(size_t i=0;i<count;i++)if(seen[i]==stream){sf_fail(p,SH_DEFAULT_READ_OWNERSHIP,(uintptr_t)stream);goto done;}
        if(count==capacity){size_t next=capacity?capacity*2:8;
            if(next<capacity || next>SIZE_MAX/sizeof *seen){sf_fail(p,SH_DEFAULT_READ_ALLOCATION,(uintptr_t)stream);goto done;}
            void *memory=realloc(seen,next*sizeof *seen);if(!memory){sf_fail(p,SH_DEFAULT_READ_ALLOCATION,(uintptr_t)stream);goto done;}
            seen=memory;capacity=next;}
        seen[count++]=stream;void *nested=NULL;if(!sf_stream_one(p,stream,kind,&nested))goto done;
        stream=nested;kind=6; /* The backing path consumes actual Seek and Read. */
    }ok=1;
done:free(seen);return ok;
}
int sh_decl_source_file_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    sf_probe p={r,in};
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->image_base || !in->image_size ||
       in->image_size>UINTPTR_MAX-in->image_base || !in->read || !in->resource || !in->default_callback ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return sf_fail(&p,SH_DEFAULT_READ_INPUT,0);
    return sf_context(&p);
}
int sh_decl_renderprog_file_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    if(!sh_decl_source_file_read_check(r,in))return 0;sf_probe p={r,in};const sf_profile *b=&sf_profiles[in->renderer];
    static const size_t slots[15]={0x80,0xa8,0xb0,0xc0,0xd8,0x100,0x108,0x118,0x88,0xe0,0x60,0x70,0xf0,0x110,0xc8};
    for(unsigned i=0;i<15;i++)if(!sf_slot(&p,in->image_base+b->table,slots[i],b->fs[5+i]))return 0;
    for(unsigned i=0;i<2;i++)if(!sf_slot(&p,in->image_base+b->client_tables[i],0x60,b->clients[i][3]))return 0;
    static const size_t atomic_slots[3]={0,0x30,0x40};
    for(unsigned i=0;i<3;i++)if(!sf_slot(&p,in->image_base+b->atomic_table,atomic_slots[i],b->atomic[i]))return 0;
    uintptr_t source_control;if(!sf_read(&p,in->image_base,b->vcs[0],&source_control,8))return 0;
    if(source_control && !sf_source_control(&p,source_control))return 0;
    return sh_decl_file_output_factory_read_check(r,in) && sh_decl_file_factory_read_check(r,in,sf_client_read,(void *)1);
}
static int sf_memory(void *context,uintptr_t at,void *out,size_t size)
{(void)context;__try {memcpy(out,(void *)at,size);return 1;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
static int sf_runtime(sh_resource_default_read_input *in,sh_resource_default_read_receipt *r)
{
    if(!sf_base || sf_renderer<0 || sf_renderer>1 || !sh_host_is_pinned_renderer_build(sf_renderer) || sh_host_is_vulkan()!=sf_renderer)return 0;
    memset(in,0,sizeof *in);in->image_base=(uintptr_t)sf_base;in->image_size=sf_size;in->renderer=sf_renderer;
    in->pinned_image=1;in->read=sf_memory;in->resource=1;in->default_callback=1;
    memset(r,0,sizeof *r);r->invocation=*in;r->state=1;r->reason=SH_DEFAULT_READ_UNSUPPORTED;return 1;
}
static unsigned sf_site(const sf_profile *b,uintptr_t caller)
{for(unsigned i=0;i<b->site_count;i++)if(caller==(uintptr_t)sf_base+b->sites[i].return_pc)return b->sites[i].kind;return 0;}
static uint64_t sf_opening(uintptr_t caller,void *provider)
{
    if(!sf_base || sf_renderer<0 || sf_renderer>1 || !sh_resource_graph_native_observing())return 0;
    const sf_profile *b=&sf_profiles[sf_renderer];if(!sf_site(b,caller))return 0;
    LONG64 before=InterlockedCompareExchange64(&sf_generation,0,0);
    if(sf_generation_failed || sf_writers || (before&1) || !before)return 0;
    sh_resource_default_read_input in;sh_resource_default_read_receipt r;
    if(!sf_runtime(&in,&r))return 0;sf_probe p={&r,&in};
    if(provider!=(void *)(sf_base+b->object) || !sf_context(&p) || sf_writers ||
       before!=InterlockedCompareExchange64(&sf_generation,0,0))return 0;
    return (uint64_t)before;
}
static void sf_returned(void *stream,uintptr_t caller,void *provider,uint64_t stamp)
{
    if(!sf_base || sf_renderer<0 || sf_renderer>1 || !sh_resource_graph_native_observing())return;
    const sf_profile *b=&sf_profiles[sf_renderer];
    unsigned kind=sf_site(b,caller);if(!kind)return;
    if(!stamp || sf_generation_failed || sf_writers || stamp!=(uint64_t)InterlockedCompareExchange64(&sf_generation,0,0))goto failed;
    sh_resource_default_read_input in;sh_resource_default_read_receipt r;
    if(!sf_runtime(&in,&r))goto failed;
    sf_probe p={&r,&in};
    if(provider!=(void *)((uintptr_t)sf_base+b->object) || !sf_context(&p) || !sf_stream(&p,stream,kind) ||
       sf_generation_failed || sf_writers || stamp!=(uint64_t)InterlockedCompareExchange64(&sf_generation,0,0))goto failed;
    return;
failed:sh_resource_graph_native_observe_resource(NULL);
}
int sh_decl_source_file_reads_ready(void)
{
    sh_resource_default_read_input in;sh_resource_default_read_receipt r;
    if(!sf_runtime(&in,&r))return 0;sf_probe p={&r,&in};return sf_context(&p);
}
int sh_decl_source_file_reads_install(const unsigned char *base)
{
    int renderer=sh_host_is_vulkan();
    if(!base || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer))return 0;
    if(sf_base && (sf_base!=base || sf_renderer!=renderer))return 0;
    __try {
        const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)base;
        const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(base+dos->e_lfanew);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return 0;
        sf_size=nt->OptionalHeader.SizeOfImage;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    sf_base=base;sf_renderer=renderer;
    const sf_profile *b=&sf_profiles[renderer];
    __try {for(size_t i=0;i<b->count;i++){
        const sf_pin *pin=b->pins+i;sh_native_pin_hash hash;unsigned char digest[32];
        if(pin->rva>sf_size || pin->length>sf_size-pin->rva)goto failed;
        sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,base+pin->rva,pin->length);sh_native_pin_hash_finish(&hash,digest);
        if(memcmp(digest,pin->digest,sizeof digest))goto failed;
    }
    for(unsigned i=0;i<4;i++){
        void **slot=(void **)(base+b->table+sf_setter_slots[i]);DWORD old,ignored;void *expected=(void *)(base+b->setters[i]);
        if(sf_setter_mask&(1<<i)){if(*slot!=sf_setters[i])goto failed;continue;}
        if(!VirtualProtect(slot,8,PAGE_READWRITE,&old))goto failed;
        sf_original_setters[i]=expected;
        sf_original_protection[i]=old;
        void *prior=InterlockedCompareExchangePointer(slot,sf_setters[i],expected);
        if(prior==expected)InterlockedOr(&sf_setter_mask,1<<i);
        int restored=VirtualProtect(slot,8,old,&ignored);
        if(prior!=expected || !restored)goto failed;
    }}__except(EXCEPTION_EXECUTE_HANDLER){goto failed;}
    if(sh_overrides_set_file_return_observer(sf_opening,sf_returned) && sh_decl_source_file_reads_ready())return 1;
failed:sh_decl_source_file_reads_uninstall();return 0;
}
int sh_decl_source_file_reads_uninstall(void)
{
    int ok=1;if(!sf_base || sf_renderer<0 || sf_renderer>1)return 1;
    const sf_profile *b=&sf_profiles[sf_renderer];
    for(int i=3;i>=0;i--)if(sf_setter_mask&(1<<i)){
        void **slot=(void **)(sf_base+b->table+sf_setter_slots[i]);DWORD old,ignored;
        if(!VirtualProtect(slot,8,PAGE_READWRITE,&old)){ok=0;continue;}
        void *prior=InterlockedCompareExchangePointer(slot,sf_original_setters[i],sf_setters[i]);
        int restored=VirtualProtect(slot,8,sf_original_protection[i],&ignored);
        if(prior==sf_setters[i])InterlockedAnd(&sf_setter_mask,~(1<<i));else ok=0;
        if(!restored)ok=0;
    }return ok && !sf_setter_mask;
}

