/* Blueprint zoom-out and the matching Blueprint far plane.
 *
 * Build 13954591 evidence (snaphak-re editor-fixes-13954591 R-004):
 *   OrbitDollyClamp VK 0x12b57a0 / GL 0x12b4e80: camera mode 0 dolly,
 *     z = clamp(z + d, -limit, limit); MOVSS xmm3,[cameraDistanceLimit] at +0.
 *   OrbitPanClamp VK 0x12b5ae3 / GL 0x12b51c3: mode 0 pan clamps X, Y, Z to
 *     +/-[environmentModuleBounds]; MOVSS xmm1,[rip+disp32].
 *   BlueprintZoomClamp VK 0x12b5130 / GL 0x12b4810: sky-view dolly,
 *     height <= bounds * 0.5; MULSS at +0x23 (disp32 +0x27) reads a shared 0.5.
 *   Editor camera view update VK 0x1242b90 / GL 0x12421c0, called from VK
 *     FUN_141181ba0 with t = EditorClockMs() * 0.001 when editor+9 != 0,
 *     editor+0x2120d == 0 and editor+0x2125c < 1 (EditorFrame); it sets
 *     r_zfar = 33000 when snapEdit_blueprintTransition < blend(camera, t).
 *     The camera is editor+0x170: mode +0x268, previous mode +0x26c, views at
 *     +0x148 (mode 0, orbit), +0x220 (1, sky), +0x188 (2..4, FPS). View slots:
 *     +0x30 transition fraction, +0x38 in transition, +0x40 is blueprint
 *     (orbit always true; sky and FPS read their blueprint cvars).
 *   BlueprintFarClipA/B VK 0x1242faa / 0x12430ee, GL 0x12425da / 0x124271e:
 *     the two r_zfar writes of that update, each
 *     MOVSS xmm1,[rip+disp32 -> 33000.0f] (F3 0F 10 0D, 8 bytes) followed by
 *     LEA rcx,[r_zfar]; MOV r8b,1; CALL idCVar::SetFloat. The environment
 *     blend stops writing r_zfar once the camera leaves the editor environment
 *     volume (+/-snapEdit_environmentFogBounds, 33000), so above z 33000 these
 *     writes alone decide the far clip (editor-fixes-13954591 R-006, R-007).
 *   EditorFogVolume VK 0x12397b0 / GL 0x1238c50: called once from the editor
 *     render-world setup (VK 0x123e210, GL 0x123d6a0) with the render world at
 *     editor+0x198. Creates _unitCube (render world vtable +0x90), stores
 *     scale 32768.0f with three MOV dword [rax+0xf8/0xfc/0x100],imm32 (imm at
 *     +0x43/+0x4d/+0x57), then MOVSS xmm1,[snapEdit_environmentFogBounds+0x34]
 *     at +0x67 (disp32 +0x6b) for the +/-value area it adds through render
 *     world vtable +0x38 (AddEnvironmentArea, VK 0x1907ae0). Result kept at
 *     editor+0x418 (R-009).
 *   Ground plane VK 0x123aac0 / GL 0x1239f60: _unitSquare with scale
 *     (bounds, bounds, 1) at +0xf8/+0xfc/+0x100, kept at editor+0x1a0; the
 *     render entity update RenderEntityUpdate (VK 0x15d8020, GL 0x15d92a0)
 *     commits a change.
 *   ModulePickHover VK 0x11f45cc / GL 0x11f37ec: in the module pick (VK
 *     FUN_1411f45a0 / GL FUN_1411f37c0, called only from the hover update VK
 *     FUN_14118af50 / GL FUN_14118a0b0, which GridBlueprintUpdate and its
 *     sibling Blueprint state call every frame) MOVSS xmm2,[rip+disp32 ->
 *     snapEdit_modulePickingDistance+0x34] (F3 0F 10 15); end = origin +
 *     dir * value with a unit dir, traced per module by GridModuleRay.
 *   ModulePickFocus VK 0x1257a58 / GL 0x1256f58: the same length in
 *     idSnapEditorState_ModuleMode vtable +0x10 (VK FUN_141257870), MOVSS
 *     xmm0,[rip+disp32] (F3 0F 10 05). These are the cvar's only two readers
 *     on each renderer (R-011). */
#include "editor_zoom.h"
#include <math.h>

float sh_editor_zoom_clamp_scale(float scale)
{
    if(!isfinite(scale)||scale<1)return 1;
    return scale>SH_ZOOM_SCALE_MAX?SH_ZOOM_SCALE_MAX:scale;
}
static float native_height(float bounds)
{
    if(!isfinite(bounds)||bounds<SH_ZOOM_NATIVE_BOUNDS)bounds=SH_ZOOM_NATIVE_BOUNDS;
    return fmaxf(SH_ZOOM_NATIVE_HEIGHT,bounds);
}
float sh_editor_zoom_orbit_height(float bounds,float scale)
{
    return native_height(bounds)*sh_editor_zoom_clamp_scale(scale);
}
float sh_editor_zoom_pan_limit(float bounds,float scale)
{
    if(!isfinite(bounds)||bounds<SH_ZOOM_NATIVE_BOUNDS)bounds=SH_ZOOM_NATIVE_BOUNDS;
    return fmaxf(bounds,sh_editor_zoom_orbit_height(bounds,scale));
}
float sh_editor_zoom_sky_factor(float scale)
{
    return SH_ZOOM_SKY_FACTOR*sh_editor_zoom_clamp_scale(scale);
}
float sh_editor_zoom_blueprint_far(float bounds,float scale,float blended)
{
    /* Native: 33000 for the native 16000 height; keep that proportion. */
    float far_plane=SH_ZOOM_EDITOR_FAR*sh_editor_zoom_orbit_height(bounds,scale)/SH_ZOOM_NATIVE_HEIGHT;
    if(far_plane>SH_ZOOM_FAR_MAX)far_plane=SH_ZOOM_FAR_MAX;
    if(isfinite(blended)&&blended>far_plane)return blended;
    return far_plane;
}
float sh_editor_zoom_capped_height(float height,int editor_far_plane)
{
    /* Without the editor's own far plane, only the environment blend raises
     * r_zfar, and it stops above the editor environment volume. */
    if(!editor_far_plane&&height>SH_ZOOM_BLEND_CEILING)return SH_ZOOM_BLEND_CEILING;
    return height;
}
float sh_editor_zoom_pick_distance(float bounds,float scale,float configured)
{
    /* Native: 50000 for the native 33000 far plane; keep that proportion so a
     * ray reaches anything the Blueprint far plane lets the camera see. */
    float pick=ceilf(sh_editor_zoom_blueprint_far(bounds,scale,0)*(SH_ZOOM_NATIVE_PICK/SH_ZOOM_EDITOR_FAR));
    if(pick<SH_ZOOM_NATIVE_PICK)pick=SH_ZOOM_NATIVE_PICK;
    if(isfinite(configured)&&configured>pick)return configured;
    return pick;
}
float sh_editor_zoom_fog_bounds(float reach)
{
    float fog;
    if(!isfinite(reach)||reach<0)reach=0;
    fog=ceilf(reach*(SH_ZOOM_NATIVE_FOG/SH_ZOOM_BLEND_CEILING));
    return fog>SH_ZOOM_NATIVE_FOG?fog:SH_ZOOM_NATIVE_FOG;
}
float sh_editor_zoom_fog_volume(void)
{
    return sh_editor_zoom_fog_bounds(sh_editor_zoom_pan_limit(SH_ZOOM_MAX_BOUNDS,SH_ZOOM_SCALE_MAX));
}
float sh_editor_zoom_fog_cube(float fog_bounds)
{
    if(!isfinite(fog_bounds)||fog_bounds<SH_ZOOM_NATIVE_FOG)fog_bounds=SH_ZOOM_NATIVE_FOG;
    return floorf(fog_bounds*(SH_ZOOM_NATIVE_FOG_CUBE/SH_ZOOM_NATIVE_FOG));
}
float sh_editor_zoom_volume_height(float height,float fog_bounds)
{
    float top;
    if(!isfinite(fog_bounds)||fog_bounds<SH_ZOOM_NATIVE_FOG)fog_bounds=SH_ZOOM_NATIVE_FOG;
    top=fog_bounds*(SH_ZOOM_BLEND_CEILING/SH_ZOOM_NATIVE_FOG);
    return height>top?top:height;
}
float sh_editor_zoom_blend(int current,int previous,int transitioning,float fraction)
{
    float factor=current?1.0f:0.0f,prev;
    if(!transitioning||(current!=0)==(previous!=0))return factor;
    prev=previous?1.0f:0.0f;
    factor=(1.0f-fraction)*prev+fraction*factor;
    return factor<0?0:factor>1?1:factor;
}

#ifndef SH_EDITOR_ZOOM_PURE
#include "engine_build.h"
#include "engine_cvar_read.h"
#include "engine_globals.h"
#include "patch.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

void backend_log(const char *message);

/* volatile: the engine reads these through the redirected instructions. */
typedef struct zoom_page {volatile float dolly,pan,sky,far_plane,fog,pick;} zoom_page;
typedef void (*entity_update_fn)(void *);
typedef int (*clock_fn)(void);
typedef unsigned char (*view_flag_fn)(void *);
typedef unsigned char (*view_busy_fn)(void *,float);
typedef float (*view_fraction_fn)(void *,float);
static zoom_page *g_page;
static sh_patch_handle g_patches[3],g_far_patches[2],g_fog_patches[4],g_pick_patches[2];
static const volatile float *g_pick_configured;
static int g_pick_live;
static entity_update_fn g_entity_update;
static float g_fog=SH_ZOOM_NATIVE_FOG;
static int g_fog_live;
static float g_scale=SH_ZOOM_SCALE_DEFAULT,g_bounds=SH_ZOOM_NATIVE_BOUNDS;
static const uint8_t *g_base;
static unsigned char *g_editor;
static clock_fn g_clock;
static const float *g_threshold;
static uintptr_t g_vtables[3];
static LONG g_faulted;
static int g_installed,g_far_live;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}
static zoom_page *near_page(uintptr_t low,uintptr_t high)
{
    SYSTEM_INFO info;uintptr_t step,center,delta;
    GetSystemInfo(&info);step=info.dwAllocationGranularity;center=low&~(step-1);
    for(delta=step;delta<0x7fff0000u;delta+=step) {
        uintptr_t candidates[]={center>=delta?center-delta:0,center+delta};int i;
        for(i=0;i<2;i++) {
            MEMORY_BASIC_INFORMATION mbi;void *page;uintptr_t a=candidates[i];
            if(a<(uintptr_t)info.lpMinimumApplicationAddress||a>(uintptr_t)info.lpMaximumApplicationAddress)continue;
            if(a+0x1000>low+0x7fff0000u||high>a+0x7fff0000u)continue;
            if(!VirtualQuery((void*)a,&mbi,sizeof mbi)||mbi.State!=MEM_FREE)continue;
            page=VirtualAlloc((void*)a,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if(page)return (zoom_page*)page;
        }
    }
    return NULL;
}
static void publish(void)
{
    char line[224];
    if(!g_page)return;
    /* Far plane first: the editor reads it as soon as the camera may rise. */
    g_page->far_plane=sh_editor_zoom_blueprint_far(g_bounds,g_scale,0);
    {
        /* The camera stays inside the editor environment volume, and without
         * the editor's far-plane redirect below the blend's 33000 limit. */
        float height=sh_editor_zoom_orbit_height(g_bounds,g_scale);
        height=sh_editor_zoom_volume_height(height,g_fog);
        if(!g_far_live)height=sh_editor_zoom_capped_height(height,0);
        g_page->dolly=height;
        g_page->pan=fmaxf(fmaxf(g_bounds,SH_ZOOM_NATIVE_BOUNDS),height);
    }
    g_page->sky=sh_editor_zoom_sky_factor(g_scale);
    /* The cvar's own value still wins when someone raised it further. */
    g_page->pick=sh_editor_zoom_pick_distance(g_bounds,g_scale,g_pick_configured?*g_pick_configured:SH_ZOOM_NATIVE_PICK);
    snprintf(line,sizeof line,"ZOOM: Blueprint zoom-out x%.3g: height %.0f, pan %.0f, far plane %.0f%s, environment volume %.0f, pick %.0f%s (bounds %.0f)",
        g_scale,g_page->dolly,g_page->pan,g_page->far_plane,
        g_far_live?"":" through the environment blend only",g_fog,
        g_pick_live?g_page->pick:SH_ZOOM_NATIVE_PICK,g_pick_live?"":" native",g_bounds);
    backend_log(line);
}
/* Engine thread. The Blueprint ground plane is scaled to the module bound
 * once when the editor render world is set up; rescale the live one. */
static void refresh_ground(void)
{
    unsigned char *e=g_editor,*world,*plane;float *scale;
    if(!g_entity_update||!e||!g_base||InterlockedCompareExchange(&g_faulted,0,0))return;
    __try {
        world=*(unsigned char**)(e+0x198);plane=*(unsigned char**)(e+0x1a0);
        if(!world||!plane||*(uintptr_t*)world!=(uintptr_t)g_base+SH_ZOOM_VT_RENDER_WORLD)return;
        scale=(float*)(plane+0xf8);
        /* Only a plane this code recognises: square, flat, a sane size. */
        if(scale[0]!=scale[1]||scale[2]!=1.0f||!(scale[0]>=1000.0f&&scale[0]<=SH_ZOOM_MAX_BOUNDS))return;
        if(scale[0]==g_bounds)return;
        {
            char line[128];
            snprintf(line,sizeof line,"ZOOM: Blueprint ground plane resized from %.0f to %.0f",scale[0],g_bounds);
            scale[0]=scale[1]=g_bounds;
            g_entity_update(plane);
            backend_log(line);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(!InterlockedExchange(&g_faulted,1))backend_log("ZOOM: Blueprint ground plane refresh disabled after an engine exception");
    }
}
void sh_editor_zoom_set_scale(float scale){g_scale=sh_editor_zoom_clamp_scale(scale);publish();}
void sh_editor_zoom_update(float bounds)
{
    if(isfinite(bounds)&&bounds>0)g_bounds=bounds>SH_ZOOM_MAX_BOUNDS?SH_ZOOM_MAX_BOUNDS:bounds;
    publish();
    if(g_installed)refresh_ground();
}

static void *view_at(unsigned char *camera,int mode)
{
    return camera+(mode==1?0x220:(unsigned)mode-2u<3u?0x188:0x148);
}
static int known_view(void *view)
{
    uintptr_t vt=*(uintptr_t*)view;
    return vt&&(vt==g_vtables[0]||vt==g_vtables[1]||vt==g_vtables[2]);
}
/* The editor camera update's own condition for forcing r_zfar this frame. */
static int blueprint_frame(void)
{
    unsigned char *e=g_editor,*camera;void *current,*previous;float t;int busy,cur,prev;
    if(!e||!g_clock||!g_threshold)return 0;
    if(!e[9]||e[0x2120d]||*(int*)(e+0x2125c)>=1)return 0;
    camera=e+0x170;
    current=view_at(camera,*(int*)(camera+0x268));previous=view_at(camera,*(int*)(camera+0x26c));
    if(!known_view(current)||!known_view(previous))return 0;
    t=(float)g_clock()*0.001f;
    cur=(*(view_flag_fn*)(*(uintptr_t*)current+0x40))(current)!=0;
    busy=(*(view_busy_fn*)(*(uintptr_t*)current+0x38))(current,t)!=0;
    prev=(*(view_flag_fn*)(*(uintptr_t*)previous+0x40))(previous)!=0;
    return *g_threshold<sh_editor_zoom_blend(cur,prev,busy,
        busy&&cur!=prev?(*(view_fraction_fn*)(*(uintptr_t*)current+0x30))(current,t):0);
}
float sh_editor_zoom_far_plane(float blended)
{
    int blueprint=0;
    if(!g_installed||InterlockedCompareExchange(&g_faulted,0,0))return blended;
    __try {blueprint=blueprint_frame();}
    __except(EXCEPTION_EXECUTE_HANDLER) {
        if(!InterlockedExchange(&g_faulted,1))backend_log("ZOOM: Blueprint far plane disabled after an engine exception");
        return blended;
    }
    return blueprint?sh_editor_zoom_blueprint_far(g_bounds,g_scale,blended):blended;
}

/* Repoint one rip-relative disp32 after checking it addresses expected_at. */
static int redirect(uintptr_t disp_at,uintptr_t end,uintptr_t expected_at,volatile float *dest,sh_patch_handle *h)
{
    int32_t old_disp,new_disp;intptr_t distance;
    memcpy(&old_disp,(void*)disp_at,4);
    if(expected_at&&end+(intptr_t)old_disp!=expected_at)return 0;
    distance=(intptr_t)dest-(intptr_t)end;
    if(distance<INT32_MIN||distance>INT32_MAX)return 0;
    new_disp=(int32_t)distance;
    return code_patch((void*)disp_at,(const uint8_t*)&old_disp,(const uint8_t*)&new_disp,4,h)==B2_PATCH_OK;
}
/* The editor camera update's two r_zfar writes load 33000.0f with
 * MOVSS xmm1,[rip+disp32] and pass r_zfar in rcx (LEA rcx,[rip+disp32] at +8).
 * Repoint both loads at the published Blueprint far plane, after checking the
 * opcode, the 33000 operand and that the LEA names r_zfar. All or nothing. */
static int install_far_plane(const sig_result *const sites[2],void *zfar_cvar)
{
    static const unsigned char movss_xmm1[4]={0xf3,0x0f,0x10,0x0d},lea_rcx[3]={0x48,0x8d,0x0d};
    int32_t disp;uintptr_t constant;int i;
    if(!sites[0]||!sites[1]||!zfar_cvar||!g_page)return 0;
    for(i=0;i<2;i++) {
        uintptr_t a=sites[i]->addr;
        if(memcmp((void*)a,movss_xmm1,4)||memcmp((void*)(a+8),lea_rcx,3))return 0;
        memcpy(&disp,(void*)(a+11),4);
        if(a+15+(intptr_t)disp!=(uintptr_t)zfar_cvar)return 0;
        memcpy(&disp,(void*)(a+4),4);constant=a+8+(intptr_t)disp;
        if(*(const float*)constant!=SH_ZOOM_EDITOR_FAR)return 0;
    }
    for(i=0;i<2;i++) {
        uintptr_t a=sites[i]->addr;
        memcpy(&disp,(void*)(a+4),4);
        if(!redirect(a+4,a+8,a+8+(intptr_t)disp,&g_page->far_plane,&g_far_patches[i])) {
            while(i-->0)if(g_far_patches[i].live)(void)code_unpatch(&g_far_patches[i]);
            return 0;
        }
    }
    return 1;
}
/* The module pick and the Module Mode focus ray load
 * snapEdit_modulePickingDistance with MOVSS xmm,[rip+disp32] (F3 0F 10 and a
 * rip-relative ModRM). Repoint both at the published pick length after
 * checking the opcode and that each operand addresses that cvar's float.
 * All or nothing. */
static int install_pick(const sig_result *const sites[2],void *pick_cvar)
{
    static const unsigned char movss[3]={0xf3,0x0f,0x10};
    uintptr_t value;int32_t disp;int i;
    if(!sites[0]||!sites[1]||!pick_cvar||!g_page)return 0;
    value=(uintptr_t)pick_cvar+0x34;
    for(i=0;i<2;i++) {
        uintptr_t a=sites[i]->addr;
        if(memcmp((void*)a,movss,3)||(*(const unsigned char*)(a+3)&0xc7)!=0x05)return 0;
        memcpy(&disp,(void*)(a+4),4);
        if(a+8+(intptr_t)disp!=value)return 0;
    }
    for(i=0;i<2;i++) {
        uintptr_t a=sites[i]->addr;
        if(!redirect(a+4,a+8,value,&g_page->pick,&g_pick_patches[i])) {
            while(i-->0)if(g_pick_patches[i].live)(void)code_unpatch(&g_pick_patches[i]);
            return 0;
        }
    }
    return 1;
}
/* Size the editor environment volume and its cube for g_fog: repoint the
 * builder's cvar load at the page and rewrite the three 32768.0f scale
 * immediates. All or nothing; checked against the expected bytes first. */
static int install_fog_volume(const sig_result *site,void *fog_cvar)
{
    static const unsigned char scale_ops[3][6]={{0xc7,0x80,0xf8,0,0,0},{0xc7,0x80,0xfc,0,0,0},{0xc7,0x80,0x00,0x01,0,0}};
    static const unsigned char movss_xmm1[4]={0xf3,0x0f,0x10,0x0d};
    float native=SH_ZOOM_NATIVE_FOG_CUBE,cube=sh_editor_zoom_fog_cube(g_fog);int32_t disp;uintptr_t a;int i;
    if(!site||!fog_cvar||!g_page)return 0;
    a=site->addr;
    for(i=0;i<3;i++)
        if(memcmp((void*)(a+0x3d+i*10),scale_ops[i],6)||memcmp((void*)(a+0x43+i*10),&native,4))return 0;
    if(memcmp((void*)(a+0x67),movss_xmm1,4))return 0;
    memcpy(&disp,(void*)(a+0x6b),4);
    if(a+0x6f+(intptr_t)disp!=(uintptr_t)fog_cvar+0x34)return 0;
    g_page->fog=g_fog;
    if(!redirect(a+0x6b,a+0x6f,(uintptr_t)fog_cvar+0x34,&g_page->fog,&g_fog_patches[0]))return 0;
    for(i=0;i<3;i++)
        if(code_patch((void*)(a+0x43+i*10),(const uint8_t*)&native,(const uint8_t*)&cube,4,&g_fog_patches[1+i])!=B2_PATCH_OK) {
            for(i=3;i>=0;i--)if(g_fog_patches[i].live)(void)code_unpatch(&g_fog_patches[i]);
            return 0;
        }
    return 1;
}
int sh_editor_zoom_install(const sig_result *results,size_t count,const uint8_t *base)
{
    static const unsigned char dolly_op[4]={0xf3,0x0f,0x10,0x1d},pan_op[4]={0xf3,0x0f,0x10,0x0d},
        sky_op[4]={0xf3,0x0f,0x59,0x05};
    const sig_result *dolly,*pan,*sky,*clock,*far_sites[2],*fog_site,*update,*pick_sites[2];
    void *slot,*camera_cvar,*bounds_cvar,*threshold_cvar,*zfar_cvar,*fog_cvar,*pick_cvar;
    uintptr_t low,high,half;int32_t disp;int i;
    if(g_installed)return 1;
    dolly=binding(results,count,"OrbitDollyClamp");pan=binding(results,count,"OrbitPanClamp");
    sky=binding(results,count,"BlueprintZoomClamp");clock=binding(results,count,"EditorClockMs");
    far_sites[0]=binding(results,count,"BlueprintFarClipA");far_sites[1]=binding(results,count,"BlueprintFarClipB");
    fog_site=binding(results,count,"EditorFogVolume");update=binding(results,count,"RenderEntityUpdate");
    pick_sites[0]=binding(results,count,"ModulePickHover");pick_sites[1]=binding(results,count,"ModulePickFocus");
    if(!dolly||!pan||!sky||!clock||!base)goto unavailable;
    if(memcmp((void*)dolly->addr,dolly_op,4)||memcmp((void*)pan->addr,pan_op,4)||memcmp((void*)(sky->addr+0x23),sky_op,4))
        goto unavailable;
    slot=(void*)glb_resolve(base,"cvar_system_slot",NULL);
    camera_cvar=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,"snapEdit_cameraDistanceLimit");
    bounds_cvar=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,"snapEdit_environmentModuleBounds");
    threshold_cvar=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,"snapEdit_blueprintTransition");
    zfar_cvar=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,"r_zfar");
    fog_cvar=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,"snapEdit_environmentFogBounds");
    pick_cvar=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,"snapEdit_modulePickingDistance");
    g_editor=(unsigned char*)glb_resolve(base,"editor_singleton",NULL);
    for(i=0;i<3;i++) {
        uint32_t rva=i==0?SH_ZOOM_VT_ORBIT:i==1?SH_ZOOM_VT_SKY:SH_ZOOM_VT_FPS;
        if(!rva)goto unavailable;
        g_vtables[i]=(uintptr_t)base+rva;
    }
    if(!camera_cvar||!bounds_cvar||!threshold_cvar||!g_editor)goto unavailable;
    /* The sky factor is the shared 0.5 constant; check its value, not an address. */
    memcpy(&disp,(void*)(sky->addr+0x27),4);half=sky->addr+0x2b+(intptr_t)disp;
    if(*(const float*)half!=SH_ZOOM_SKY_FACTOR)goto unavailable;
    low=dolly->addr<pan->addr?dolly->addr:pan->addr;if(sky->addr<low)low=sky->addr;
    high=dolly->addr>pan->addr?dolly->addr:pan->addr;if(sky->addr>high)high=sky->addr;
    for(i=0;i<2;i++)if(far_sites[i]) {
        if(far_sites[i]->addr<low)low=far_sites[i]->addr;
        if(far_sites[i]->addr>high)high=far_sites[i]->addr;
    }
    if(fog_site) {
        if(fog_site->addr<low)low=fog_site->addr;
        if(fog_site->addr+0x70>high)high=fog_site->addr+0x70;
    }
    for(i=0;i<2;i++)if(pick_sites[i]) {
        if(pick_sites[i]->addr<low)low=pick_sites[i]->addr;
        if(pick_sites[i]->addr>high)high=pick_sites[i]->addr;
    }
    g_page=near_page(low,high+0x40);if(!g_page)goto unavailable;
    g_base=base;g_clock=(clock_fn)clock->addr;
    g_threshold=(const float*)((unsigned char*)threshold_cvar+0x34);
    /* The far plane goes live before any height above native can be reached;
     * without it the height stays inside the editor environment volume. */
    g_page->far_plane=sh_editor_zoom_blueprint_far(g_bounds,g_scale,0);
    g_far_live=install_far_plane(far_sites,zfar_cvar);
    if(!g_far_live)backend_log("ZOOM: editor far-plane write unavailable; Blueprint height kept inside the editor environment volume");
    /* The volume is built when the editor render world is set up, after this
     * install; sized once for the highest camera on any map. */
    g_fog=sh_editor_zoom_fog_volume();
    g_fog_live=install_fog_volume(fog_site,fog_cvar);
    if(!g_fog_live) {
        g_fog=SH_ZOOM_NATIVE_FOG;
        backend_log("ZOOM: editor environment volume unavailable; Blueprint height kept inside the native 33000 volume");
    }
    /* Picking reaches as far as the camera can see; written before the redirect. */
    g_pick_configured=pick_cvar?(const volatile float*)((unsigned char*)pick_cvar+0x34):NULL;
    g_page->pick=sh_editor_zoom_pick_distance(g_bounds,g_scale,g_pick_configured?*g_pick_configured:SH_ZOOM_NATIVE_PICK);
    g_pick_live=install_pick(pick_sites,pick_cvar);
    if(!g_pick_live)backend_log("ZOOM: module picking distance unavailable; modules farther than snapEdit_modulePickingDistance cannot be hovered or selected");
    g_entity_update=update?(entity_update_fn)update->addr:NULL;
    if(!g_entity_update)backend_log("ZOOM: Blueprint ground plane refresh unavailable; it follows map bounds when the editor is set up");
    publish();
    if(!redirect(dolly->addr+4,dolly->addr+8,(uintptr_t)camera_cvar+0x34,&g_page->dolly,&g_patches[0])||
       !redirect(pan->addr+4,pan->addr+8,(uintptr_t)bounds_cvar+0x34,&g_page->pan,&g_patches[1])||
       !redirect(sky->addr+0x27,sky->addr+0x2b,half,&g_page->sky,&g_patches[2])) {
        for(i=2;i>=0;i--)if(g_patches[i].live)code_unpatch(&g_patches[i]);
        for(i=1;i>=0;i--)if(g_far_patches[i].live)code_unpatch(&g_far_patches[i]);
        for(i=3;i>=0;i--)if(g_fog_patches[i].live)code_unpatch(&g_fog_patches[i]);
        for(i=1;i>=0;i--)if(g_pick_patches[i].live)code_unpatch(&g_pick_patches[i]);
        g_far_live=g_fog_live=g_pick_live=0;
        for(i=0;i<3;i++)if(g_patches[i].live)break;
        if(i==3&&!g_far_patches[0].live&&!g_far_patches[1].live&&!g_fog_patches[0].live&&!g_fog_patches[1].live&&
           !g_fog_patches[2].live&&!g_fog_patches[3].live&&!g_pick_patches[0].live&&!g_pick_patches[1].live){VirtualFree(g_page,0,MEM_RELEASE);g_page=NULL;}
        goto unavailable;
    }
    g_installed=1;
    backend_log("ZOOM: Blueprint zoom-out and far plane installed");
    return 1;
unavailable:
    backend_log("ZOOM: Blueprint zoom-out unavailable on this build");return 0;
}
#endif
