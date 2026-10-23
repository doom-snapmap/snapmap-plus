#include "../src/backend/resource_md6_reads.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int renderer,pinned=1,current=1,observing=1,unknown,refs,throw_observe,replace_model,replace_skeleton;
static unsigned char model[0x178],skeleton[0x60],other[0x60];
static uint64_t mt=11,st=12;
static sh_resource_return_observer registered;
static unsigned char resource[0x430],table[0xb0];
static int pre_ok=1,bound_ok=1,pre_calls,bound_calls,native_calls,native_throw,native_drift,hook_live;
static void *hook_target,*hook_detour;static unsigned char hook_saved[16];
static int fail_prepare,fail_commit,removed;
static void rig_native(void *setup,void *object)
{
 assert(setup==resource+0x308 && object==resource);native_calls++;
 if(native_drift)st++;
 if(native_throw)RaiseException(0xe0137022,0,0,NULL);
}
void *hook_prepare(void *target,void *detour,size_t length)
{assert(length==16);if(fail_prepare)return NULL;hook_target=target;hook_detour=detour;memcpy(hook_saved,target,16);return rig_native;}
sh_patch_status hook_commit(void *tramp)
{assert(tramp==rig_native);if(fail_commit)return B2_PATCH_FAIL_ROLLBACK;memset(hook_target,0x90,16);memcpy(hook_target,"\xff\x25\0\0\0\0",6);memcpy((unsigned char *)hook_target+6,&hook_detour,8);hook_live=1;return B2_PATCH_OK;}
int hook_is_installed(void *tramp){return tramp==rig_native && hook_live;}
int hook_unpatch(void *tramp){assert(tramp==rig_native);memcpy(hook_target,hook_saved,16);hook_live=0;removed++;return 1;}
size_t sh_host_image_size(void){return 0x1500000;}
int sh_decl_md6_rig_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{assert(r->state==1 && !r->complete && r->reason==SH_DEFAULT_READ_UNSUPPORTED && in->resource==(uintptr_t)resource);pre_calls++;return pre_ok;}
int sh_decl_md6_rig_bound_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,uintptr_t m,uintptr_t s)
{assert(!r->complete && in->resource==(uintptr_t)resource && m==(uintptr_t)model && s==(uintptr_t)skeleton);bound_calls++;return bound_ok;}
int sh_host_is_vulkan(void){return renderer;}
int sh_host_is_pinned_renderer_build(int r){return pinned && r==renderer;}
int sh_resource_graph_native_observing(void){return observing;}
int sh_resource_graph_native_set_return_observer(sh_resource_return_observer fn){if(registered && registered!=fn)return 0;registered=fn;return 1;}
int sh_resource_graph_native_return_observer_ready(sh_resource_return_observer fn){return current && registered==fn;}
void sh_resource_graph_incomplete(void){unknown++;}
int sh_resource_graph_native_observe_resource(void *p){assert(!p);return 0;}
uint64_t sh_resource_instances_token(const void *p){return p==model?mt:p==skeleton?st:p==resource?13:0;}
static int observe(void *p)
{
 if(!p)return 0;
 assert(p==model || p==skeleton);refs++;
 if(throw_observe)RaiseException(0xe0137021,0,0,NULL);
 if(p==skeleton && replace_model)memcpy(model+0x138,&(void *){other},sizeof(void *));
 if(p==skeleton && replace_skeleton)st++;
 return 1;
}
static void reset(void)
{
 memset(model,0,sizeof model);memset(skeleton,0,sizeof skeleton);
 void *p=skeleton;memcpy(model+0x138,&p,sizeof p);
 mt=11;st=12;refs=unknown=throw_observe=replace_model=replace_skeleton=0;observing=pinned=current=1;
 memset(resource,0,sizeof resource);memset(table,0,sizeof table);
 memcpy(resource,&(void *){table},8);memcpy(resource+0x60,&(void *){model},8);
 pre_ok=bound_ok=1;pre_calls=bound_calls=native_calls=native_throw=native_drift=0;
}
static void controls(void)
{
 unsigned char *base=VirtualAlloc(NULL,0x1500000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(base);
 for(renderer=0;renderer<2;renderer++) {
  for(unsigned i=0;i<2;i++)memcpy(base+md6_sites_by_renderer[renderer][i].caller-5,md6_sites_by_renderer[renderer][i].call,5);
  memcpy(base+md6_rig_rvas[renderer],md6_rig_prefix,sizeof md6_rig_prefix);
  g_md6_base=NULL;g_md6_renderer=-1;g_md6_observe=NULL;g_md6_rig_original=NULL;g_md6_rig_ready=0;registered=NULL;reset();
  assert(sh_resource_md6_reads_install(base,observe) && sh_resource_md6_reads_ready());
  assert(!sh_resource_md6_reads_install(base+1,observe));
  for(unsigned i=0;i<2;i++) {
   uintptr_t pc=(uintptr_t)base+md6_sites_by_renderer[renderer][i].caller;
   reset();registered(model,pc);assert(refs==2 && !unknown);
   reset();registered(model,pc+1);assert(!refs && !unknown);
   reset();observing=0;registered(model,pc);assert(!refs && !unknown);observing=1;
   reset();registered(NULL,pc);assert(!refs && !unknown);
   reset();memset(model+0x138,0,sizeof(void *));registered(model,pc);assert(refs==1 && !unknown);
   reset();current=0;registered(model,pc);assert(!refs && unknown);current=1;
   reset();pinned=0;registered(model,pc);assert(!refs && unknown);pinned=1;
   reset();mt=0;registered(model,pc);assert(refs==1 && unknown);
   reset();st=0;registered(model,pc);assert(refs==2 && unknown);
   reset();replace_model=1;registered(model,pc);assert(refs==2 && unknown);
   reset();replace_skeleton=1;registered(model,pc);assert(refs==2 && unknown);
   reset();throw_observe=1;registered(model,pc);assert(refs==1 && unknown);
   for(unsigned j=0;j<5;j++) {
    reset();base[md6_sites_by_renderer[renderer][i].caller-5+j]^=1;
    assert(!sh_resource_md6_reads_ready());registered(model,pc);assert(!refs && unknown);
    base[md6_sites_by_renderer[renderer][i].caller-5+j]^=1;
   }
  }
  reset();md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && pre_calls==1 && bound_calls==1 && refs==2 && !unknown);
  reset();observing=0;md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && !pre_calls && !refs && !unknown);
  reset();pre_ok=0;md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && pre_calls==1 && !bound_calls && unknown);
  reset();bound_ok=0;md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && bound_calls==1 && unknown);
  reset();native_drift=1;md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && unknown);
  reset();throw_observe=1;md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && !pre_calls && unknown);
  reset();replace_model=1;md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && !pre_calls && unknown);
  reset();native_throw=1;int caught=0;
  __try {md6_rig_hook(resource+0x308,resource);} __except(GetExceptionCode()==0xe0137022?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
  assert(caught && native_calls==1 && unknown);
  for(unsigned j=0;j<16;j++) {reset();base[md6_rig_rvas[renderer]+j]^=1;assert(!sh_resource_md6_reads_ready());md6_rig_hook(resource+0x308,resource);assert(native_calls==1 && unknown);base[md6_rig_rvas[renderer]+j]^=1;}
  assert(hook_unpatch(g_md6_rig_original));
  for(unsigned failure=0;failure<2;failure++) {
   g_md6_base=NULL;g_md6_renderer=-1;g_md6_observe=NULL;g_md6_rig_original=NULL;g_md6_rig_ready=0;registered=NULL;
   fail_prepare=failure==0;fail_commit=failure==1;removed=0;
   assert(!sh_resource_md6_reads_install(base,observe) && !sh_resource_md6_reads_ready());
   assert(removed==(failure==1));fail_prepare=fail_commit=0;
  }
 }
 g_md6_base=NULL;g_md6_renderer=-1;g_md6_observe=NULL;registered=NULL;VirtualFree(base,0,MEM_RELEASE);
}
static void image_case(const char *path,int r)
{
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 unsigned char *raw=malloc((size_t)n);assert(raw && fread(raw,1,n,f)==(size_t)n);fclose(f);
 IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)raw;assert(dos->e_magic==IMAGE_DOS_SIGNATURE);
 IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);assert(nt->Signature==IMAGE_NT_SIGNATURE);
 IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt);
 for(unsigned i=0;i<2;i++) {
  uint32_t rva=md6_sites_by_renderer[r][i].caller-5;int found=0;
  for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++)if(rva>=sec[j].VirtualAddress &&
   rva-sec[j].VirtualAddress<=sec[j].SizeOfRawData && 5<=sec[j].SizeOfRawData-(rva-sec[j].VirtualAddress)) {
    size_t off=sec[j].PointerToRawData+rva-sec[j].VirtualAddress;assert(off<=(size_t)n && 5<=(size_t)n-off);
    assert(!memcmp(raw+off,md6_sites_by_renderer[r][i].call,5));found=1;break;
   }
  assert(found);
 }
 {uint32_t rva=md6_rig_rvas[r];int found=0;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++)if(rva>=sec[j].VirtualAddress && rva-sec[j].VirtualAddress<=sec[j].SizeOfRawData && 16<=sec[j].SizeOfRawData-(rva-sec[j].VirtualAddress)) {
  size_t off=sec[j].PointerToRawData+rva-sec[j].VirtualAddress;assert(off<=(size_t)n && 16<=(size_t)n-off && !memcmp(raw+off,md6_rig_prefix,16));found=1;break;}assert(found);}
 free(raw);
}
int main(int argc,char **argv)
{
 if(argc==3){image_case(argv[1],atoi(argv[2]));puts("resource_md6_reads_image_test: passed");return 0;}
 controls();puts("resource_md6_reads_test: passed");return 0;
}
