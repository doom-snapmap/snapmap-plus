#include "native_pin_fixture.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/resource_presentation.c"
static int failures,renderer,pinned=1,prepares,commits,removes,prepare_fault,commit_fault,remove_fault,installed;
static int begins,ends,depth,calls,native_throw,begin_throw,end_throw,nested;
static void *common,*table[32];
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); failures++; } } while(0)
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int r) { return pinned && r==renderer; }
static int begin_scope(sh_resource_presentation_scope *scope)
{ begins++; depth++; scope->opened=1; if(begin_throw) RaiseException(0xe1320021,0,0,NULL); return 1; }
static void end_scope(sh_resource_presentation_scope *scope)
{ ends++; CHECK(scope->opened && depth>0); scope->opened=0; depth--; if(end_throw) RaiseException(0xe1320022,0,0,NULL); }
static void native_update(void *self,unsigned char force)
{
    CHECK(self==&common && force==0xa7); calls++;
    if(nested) { nested=0; CHECK(depth==1); rp_call(self,force,g_base+g_sites[renderer][1]); CHECK(depth==1); }
    if(native_throw) RaiseException(0xe1320023,0,0,NULL);
}
void *hook_prepare_relative_call(void *target,void *detour,size_t stolen,size_t offset)
{ CHECK(target==g_base+g_update_rva[renderer] && detour==rp_update && stolen==14 && offset==9); prepares++; return prepare_fault?NULL:native_update; }
sh_patch_status hook_commit(void *trampoline)
{
    CHECK(trampoline==native_update); commits++; if(commit_fault) return B2_PATCH_FAIL_SEH;
    unsigned char *entry=(unsigned char *)g_base+g_update_rva[renderer];
    unsigned char redirect[6]={0xff,0x25,0,0,0,0}; void *target=rp_update;
    memcpy(entry,redirect,6); memcpy(entry+6,&target,8); installed=1; return B2_PATCH_OK;
}
int hook_is_installed(void *trampoline) { CHECK(trampoline==native_update); return installed; }
int hook_unpatch(void *trampoline)
{ CHECK(trampoline==native_update); removes++; if(remove_fault) return 0;
    memcpy((unsigned char *)g_base+g_update_rva[renderer],g_update_head[renderer],14); installed=0; return 1; }
int main(void)
{
    unsigned char *base=VirtualAlloc(NULL,0x2000000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(base); if(!base) return 1;
    common=table;
    for(renderer=0;renderer<2;renderer++) {
        memcpy(base+g_refresh_rva[renderer],g_refresh[renderer],37);
        memcpy(base+g_update_rva[renderer],g_update_head[renderer],14);
        if(renderer) CHECK(sh_native_pin_fixture_copy(renderer,g_update_rva[renderer]+14,base+g_update_rva[renderer]+14,131));
        else CHECK(sh_native_pin_fixture_copy(renderer,g_update_rva[renderer]+14,base+g_update_rva[renderer]+14,131));
        for(size_t i=0;i<2;i++) memcpy(base+g_sites[renderer][i]-15,g_dispatch[i],27);
        table[0xe0/8]=base+g_update_rva[renderer];
        prepares=commits=removes=0; pinned=0; CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope) && !prepares); pinned=1;
        CHECK(!sh_resource_presentation_install(base,NULL,end_scope));
        for(size_t i=0;i<37;i++) { base[g_refresh_rva[renderer]+i]^=1; CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope)); base[g_refresh_rva[renderer]+i]^=1; }
        for(size_t i=0;i<14;i++) { base[g_update_rva[renderer]+i]^=1; CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope)); base[g_update_rva[renderer]+i]^=1; }
        for(size_t i=0;i<2;i++) for(size_t j=0;j<27;j++) { base[g_sites[renderer][i]-15+j]^=1; CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope)); base[g_sites[renderer][i]-15+j]^=1; }
        prepare_fault=1; CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope)); prepare_fault=0;
        commit_fault=remove_fault=1; CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope) && g_update);
        CHECK(!sh_resource_presentation_install(base,begin_scope,end_scope)); remove_fault=commit_fault=0; CHECK(sh_resource_presentation_uninstall());
        CHECK(sh_resource_presentation_install(base,begin_scope,end_scope) && sh_resource_presentation_ready());
        base[g_update_rva[renderer]]^=1; CHECK(!sh_resource_presentation_ready()); base[g_update_rva[renderer]]^=1;
        base[g_update_rva[renderer]+6]^=1; CHECK(!sh_resource_presentation_ready()); base[g_update_rva[renderer]+6]^=1;
        base[g_update_rva[renderer]+20]^=1; CHECK(!sh_resource_presentation_ready()); base[g_update_rva[renderer]+20]^=1;
        installed=0; CHECK(!sh_resource_presentation_ready()); installed=1;
        begins=ends=depth=calls=0;
        rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(calls==1 && begins==1 && ends==1 && !depth);
        rp_call(&common,0xa7,base+g_sites[renderer][1]); CHECK(calls==2 && begins==2 && ends==2 && !depth);
        nested=1; rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(calls==4 && begins==4 && ends==4 && !depth);
        int prior=begins; rp_call(&common,0xa7,base+g_sites[renderer][0]+1); CHECK(begins==prior);
        table[0xe0/8]=NULL; rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(begins==prior); table[0xe0/8]=base+g_update_rva[renderer];
        base[g_refresh_rva[renderer]]^=1; rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(begins==prior); base[g_refresh_rva[renderer]]^=1;
        pinned=0; rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(begins==prior); pinned=1;
        begin_throw=1; prior=calls; rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(calls==prior+1 && !depth && begins==ends); begin_throw=0;
        end_throw=1; rp_call(&common,0xa7,base+g_sites[renderer][0]); CHECK(!depth && begins==ends); end_throw=0;
        native_throw=1; int caught=0; prior=calls;
        __try { rp_call(&common,0xa7,base+g_sites[renderer][0]); } __except(GetExceptionCode()==0xe1320023?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
        CHECK(caught && calls==prior+1 && !depth && begins==ends); native_throw=0;
        CHECK(sh_resource_presentation_uninstall() && !sh_resource_presentation_ready());
    }
    VirtualFree(base,0,MEM_RELEASE);
    if(failures) fprintf(stderr,"%d failures\n",failures); else puts("resource_presentation_test: PASS");
    return failures?1:0;
}
