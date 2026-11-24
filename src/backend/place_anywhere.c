/* Unrestricted entity placement (place_anywhere.h). placementFlags is the dword at snapEditorEntityDef+0x1d0
 * (FLOOR 1, WALL 2, CEILING 4, SURFACE_ALIGNED 8, AIR 0x10, AIR_UNALIGNED 0x20; VK 0x143529f90 / GL 0x143560ba0).
 * The element gate (VK FUN_1412197b0 / GL FUN_141218c00) and group gate (VK FUN_1412194b0 / GL FUN_141218900)
 * refuse an element that is blocked (+0x30), in module -1, or has surface (+8) 0 without flags & 0x30. Their
 * `CMP dword [elem+8],0; JNZ` (PlaceGateCommit VK 0x14121951c / GL 0x14121896c, PlaceGateElement at the element
 * gate) skips that test, so the JNZ becomes a JMP. */
#include "place_anywhere.h"
#include <string.h>

/* Exact bytes, no relocations: both sites are position-independent. */
static const unsigned char k_commit[]={
    0x41,0x83,0x7c,0x14,0x08,0x00, 0x75,0x27, 0x49,0x8b,0x46,0x28, 0x49,0x63,0x0c,0x14,
    0x48,0x8b,0x00, 0x48,0x8b,0x80,0xa0,0x06,0x00,0x00, 0x48,0x8b,0x0c,0xc8, 0x48,0x8b,0x41,0x08,
    0xf6,0x80,0xd0,0x01,0x00,0x00,0x30, 0x0f,0x84};
static const unsigned char k_element[]={
    0x40,0x55, 0x57, 0x48,0x81,0xec,0xd8,0x00,0x00,0x00, 0x41,0x80,0x78,0x30,0x00, 0x48,0x8b,0xea,
    0x48,0x8b,0xf9, 0x75,0x30, 0x41,0x83,0x78,0x04,0xff, 0x74,0x29, 0x41,0x83,0x78,0x08,0x00,
    0x75,0x2e, 0x48,0x8b,0x41,0x28, 0x49,0x63,0x08, 0x48,0x8b,0x00, 0x48,0x8b,0x80,0xa0,0x06,0x00,0x00,
    0x48,0x8b,0x0c,0xc8, 0x48,0x8b,0x41,0x08, 0xf6,0x80,0xd0,0x01,0x00,0x00,0x30, 0x75,0x0c, 0x32,0xc0};
static const struct {const unsigned char *bytes;size_t len;int branch;const char *name;} k_gate[SH_PLACE_GATE_COUNT]={
    {k_commit,sizeof k_commit,6,"PlaceGateCommit"},
    {k_element,sizeof k_element,0x23,"PlaceGateElement"}};

int sh_place_gate_branch(const unsigned char *site,int gate)
{
    if(!site||gate<0||gate>=SH_PLACE_GATE_COUNT)return -1;
    if(memcmp(site,k_gate[gate].bytes,k_gate[gate].len))return -1;
    return k_gate[gate].branch;
}

#ifndef SH_PLACE_ANYWHERE_PURE
#include "patch.h"
#include <stdio.h>

void backend_log(const char *message);

static sh_patch_handle g_patch[SH_PLACE_GATE_COUNT];
static int g_live;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}
static int site_matches(uintptr_t site,int gate)
{
    __try {return sh_place_gate_branch((const unsigned char*)site,gate);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return -1;}
}
int sh_place_anywhere_install(const sig_result *results,size_t count)
{
    static const uint8_t jnz=SH_PLACE_GATE_JNZ,jmp=SH_PLACE_GATE_JMP;
    uintptr_t at[SH_PLACE_GATE_COUNT];const char *why=NULL;char line[320];int i;
    if(g_live)return 1;
    for(i=0;i<SH_PLACE_GATE_COUNT&&!why;i++) {
        const sig_result *r=binding(results,count,k_gate[i].name);int branch;
        if(!r){why=i?"the ghost colour test was not found":"the Place test was not found";break;}
        if((branch=site_matches(r->addr,i))<0){why=i?"the ghost colour test has unexpected bytes":"the Place test has unexpected bytes";break;}
        at[i]=r->addr+(uintptr_t)branch;
    }
    if(!why)for(i=0;i<SH_PLACE_GATE_COUNT;i++)
        if(code_patch((void*)at[i],&jnz,&jmp,1,&g_patch[i])!=B2_PATCH_OK) {
            why="a placement test could not be patched";
            while(--i>=0)if(g_patch[i].live)(void)code_unpatch(&g_patch[i]);
            break;
        }
    if(why) {
        snprintf(line,sizeof line,"EDITOR: unrestricted placement unavailable (%s); entities keep their native floor, wall and air rules",why);
        backend_log(line);return 0;
    }
    g_live=1;
    snprintf(line,sizeof line,"EDITOR: unrestricted placement at %p and %p, so any entity can be placed or moved into open air",
        (void*)at[0],(void*)at[1]);
    backend_log(line);
    return 1;
}
int sh_place_anywhere_live(void){return g_live;}
#endif
