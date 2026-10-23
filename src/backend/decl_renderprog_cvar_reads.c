#include "native_pin_hash.h"
#include <stdint.h>
#include <string.h>
#include "decl_renderprog_cvar_reads.h"
typedef struct rpc_pin {uint32_t rva;size_t length;const unsigned char *hash;} rpc_pin;
#include "decl_renderprog_cvar_read_pins.h"
typedef struct rpc_profile {uint32_t cvars,cvar_table,commands,command_table;} rpc_profile;
static const rpc_profile rpc_profiles[2]={
    {0x3eb98b0,0x27a2bf0,0x3eb98a0,0x27a39f0},
    {0x55b7290,0x278c9d0,0x55b7280,0x278d7d0}};
typedef struct rpc_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} rpc_probe;
static int rpc_fail(rpc_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{p->r->reason=why;p->r->offending_address=at;return 0;}
static int rpc_read(rpc_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if(!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))return rpc_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->in->work_limit && p->r->work>=p->in->work_limit)return rpc_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    p->r->work++;
    return p->in->read(p->in->read_context,at+offset,out,size) || rpc_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
}
static int rpc_image(rpc_probe *p,uint32_t rva,size_t size)
{return rva<=p->in->image_size && size<=p->in->image_size-rva;}
static int rpc_pin_check(rpc_probe *p,unsigned index)
{
    const rpc_pin *pin=&rpc_pins[p->in->renderer][index];unsigned char bytes[128],digest[32];
    sh_native_pin_hash hash;sh_native_pin_hash_init(&hash);
    if(!rpc_image(p,pin->rva,pin->length))return rpc_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    for(size_t off=0;off<pin->length;off+=sizeof bytes){size_t n=pin->length-off;if(n>sizeof bytes)n=sizeof bytes;
        if(!rpc_read(p,p->in->image_base+pin->rva,off,bytes,n))return 0;
        sh_native_pin_hash_update(&hash,bytes,n);}
    sh_native_pin_hash_finish(&hash,digest);
    return !memcmp(digest,pin->hash,sizeof digest) || rpc_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
}
static int rpc_string(rpc_probe *p,uintptr_t string)
{
    unsigned char ch;
    /* The fixed CRT %s formatter accepts NULL and formats its native null
     * marker. No other cvar-specific name/value restrictions are invented. */
    if(!string)return 1;
    for(size_t off=0;;off++){if(!rpc_read(p,string,off,&ch,1))return 0;if(!ch)return 1;}
}
static int rpc_binding(rpc_probe *p,uint32_t holder,uint32_t table,uintptr_t *object)
{
    uintptr_t actual;
    if(!rpc_image(p,holder,8) || !rpc_image(p,table,0x18))return rpc_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    if(!rpc_read(p,p->in->image_base,holder,object,8) || !rpc_read(p,*object,0,&actual,8))return 0;
    return actual==p->in->image_base+table || rpc_fail(p,SH_DEFAULT_READ_CALLBACK,actual);
}
static int rpc_slot(rpc_probe *p,uint32_t table,size_t slot,unsigned index)
{
    uintptr_t callback;
    if(!rpc_read(p,p->in->image_base+table,slot,&callback,8))return 0;
    return callback==p->in->image_base+rpc_pins[p->in->renderer][index].rva || rpc_fail(p,SH_DEFAULT_READ_CALLBACK,callback);
}
int sh_decl_renderprog_cvar_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    rpc_probe p={r,in};uintptr_t system,command,list;int mode,count,capacity;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->image_base || !in->image_size ||
       in->image_size>UINTPTR_MAX-in->image_base || !in->read || !in->resource || !in->default_callback ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return rpc_fail(&p,SH_DEFAULT_READ_INPUT,0);
    const rpc_profile *b=&rpc_profiles[in->renderer];
    for(unsigned i=0;i<RPC_PINS;i++)if(!rpc_pin_check(&p,i))return 0;
    if(!rpc_binding(&p,b->cvars,b->cvar_table,&system) || !rpc_binding(&p,b->commands,b->command_table,&command) ||
       !rpc_slot(&p,b->cvar_table,8,RPC_COUNT) || !rpc_slot(&p,b->cvar_table,0x10,RPC_INDEX) ||
       !rpc_slot(&p,b->command_table,0x10,RPC_MODE) || !rpc_read(&p,command,0x200a8,&mode,4))return 0;
    size_t header=mode?0x20:8;
    if(!rpc_read(&p,system,header+8,&count,4))return 0;
    if(count>0){
        if(!rpc_read(&p,system,header,&list,8) || !rpc_read(&p,system,header+12,&capacity,4))return 0;
        if(count>capacity || (size_t)count>SIZE_MAX/8)return rpc_fail(&p,SH_DEFAULT_READ_OWNERSHIP,system+header);
        for(int i=0;i<count;i++){
            uintptr_t object,name,value;uint32_t flags;unsigned char number[4];
            if(!rpc_read(&p,list,(size_t)i*8,&object,8) || !rpc_read(&p,object,0x58,&flags,4))return 0;
            unsigned type=flags&7;
            if(!(flags&0x100000) || (type!=0 && type!=1 && type!=2 && type!=4))continue;
            if(!rpc_read(&p,object,0x40,&name,8) || !rpc_string(&p,name))return 0;
            if(type==0){if(!rpc_read(&p,object,0x10,&value,8) || !rpc_string(&p,value))return 0;}
            else if(!rpc_read(&p,object,type==4?0x34:0x30,number,4))return 0;
        }
    }
    /* Count and index perform separate native selector calls. Detect a
     * changed selector/list during this observation instead of inventing a
     * list identity from the first returned count. */
    uintptr_t now,table;int current;
    if(!rpc_read(&p,command,0x200a8,&current,4) || current!=mode ||
       !rpc_read(&p,system,header+8,&current,4) || current!=count)
        return r->reason==SH_DEFAULT_READ_UNSUPPORTED?rpc_fail(&p,SH_DEFAULT_READ_OWNERSHIP,command):0;
    if(count>0 && (!rpc_read(&p,system,header,&now,8) || now!=list))
        return r->reason==SH_DEFAULT_READ_UNSUPPORTED?rpc_fail(&p,SH_DEFAULT_READ_OWNERSHIP,system):0;
    if(!rpc_binding(&p,b->cvars,b->cvar_table,&now) || now!=system ||
       !rpc_binding(&p,b->commands,b->command_table,&table) || table!=command)
        return r->reason==SH_DEFAULT_READ_UNSUPPORTED?rpc_fail(&p,SH_DEFAULT_READ_OWNERSHIP,system):0;
    return 1;
}
