/* Preserve exact doorway alignment when a dimension variant meets a module. */
#include <windows.h>
#include <math.h>
#include <string.h>
#include "grid_room_snap.h"
#include "grid_room.h"
#include "engine_globals.h"
#include "hook.h"

void backend_log(const char *message);
typedef void (*snap_fn)(void *,void *,const float *,unsigned char);
typedef unsigned char (*align_fn)(void *,const float *,unsigned,int,int);
typedef float *(*quantize_fn)(float *,const float *,float);
typedef void (*confirm_fn)(void *,float);
typedef float *(*bounds_fn)(void *,float *,int);
typedef int (*direction_fn)(void *,int);
typedef void (*replace_fn)(void *,unsigned,const void *);
typedef void (*reconnect_fn)(void *);
static snap_fn g_snap;
static align_fn g_align;
static quantize_fn g_quantize;
static confirm_fn g_confirm;
static bounds_fn g_bounds;
static direction_fn g_direction;
static replace_fn g_replace;
static reconnect_fn g_reconnect;
static unsigned char *g_editor;
static LONG g_faulted;
static __declspec(thread) int g_exact;

typedef struct portal_view {float bounds[6];int direction,type,variant;} portal_view;
static unsigned char *record(unsigned char *map,int index)
{
    int n=*(int*)(map+0x758);unsigned char *p=*(unsigned char**)(map+0x750);
    if(!p||n<0||n>4096||index<0||index>=n)return NULL;
    return p+(size_t)index*0x98;
}
static int variant(unsigned char *p)
{
    sh_grid_size size;unsigned char *w,*m;const char *name;
    if(!p||!(w=*(unsigned char**)p)||!(m=*(unsigned char**)w))return 0;
    name=*(const char**)(m+8);return name&&sh_grid_parse_name(name,&size);
}
static int portal(unsigned char *map,int index,int door,portal_view *out)
{
    unsigned char *p=record(map,index),*w,*m,*info,*meta;int n,i;
    if(!p||!(w=*(unsigned char**)p)||!(m=*(unsigned char**)w)||
        !(info=*(unsigned char**)(w+0x80)))return 0;
    n=*(int*)(m+0xf0);if(n<0||n>64||door<0||door>=n)return 0;
    g_bounds(p,out->bounds,door);out->direction=g_direction(p,door);
    for(i=0;i<6;++i)if(!isfinite(out->bounds[i]))return 0;
    n=*(int*)(info+0x1e0);if(n<0||n>64)return 0;
    meta=door<n?*(unsigned char**)(info+0x1d8)+(size_t)door*0x20:info+0x1b8;
    if(!meta)return 0;
    out->type=*(int*)meta;out->variant=variant(p);return 1;
}
static unsigned char *editor_map(void *editor)
{
    unsigned char *e=editor,*edit,*map;
    if(e!=g_editor||!e[8]||!(edit=*(unsigned char**)(e+0x204d0)))return NULL;
    map=*(unsigned char**)edit;
    return map==*(unsigned char**)(e+0x204c8)?map:NULL;
}
/* Return a translation only for equal-size, opposite-facing doorways. The
 * bounded difference is the error introduced by 128-unit XY rounding, not an
 * expanded connection tolerance. The native connection test stays exact. */
static int correction(const portal_view *moving,const portal_view *fixed,float delta[3])
{
    int i;
    if((moving->direction^fixed->direction)!=2||moving->type!=fixed->type)return 0;
    for(i=0;i<3;++i){
        delta[i]=fixed->bounds[i]-moving->bounds[i];
        if(fabsf((fixed->bounds[i+3]-moving->bounds[i+3])-delta[i])>0.001f||
            fabsf(delta[i])>(i==2?0.001f:64.001f))return 0;
    }
    return 1;
}
static void finish_snap(unsigned char *snap,unsigned char *editor)
{
    unsigned char *map=editor_map(editor),*fixed,*moving,*edit;
    int nf,nm,i,j,k,chosen=-1,chosen_target=-1,needs_variant=0,n,*selected;
    float best=1e30f,delta[3]={0};
    if(!map||!snap[0x30])return; /* Native magnet must have accepted a target. */
    nf=*(int*)(snap+8);nm=*(int*)(snap+0x20);
    fixed=*(unsigned char**)snap;moving=*(unsigned char**)(snap+0x18);
    if(nf<=0||nf>16384||nm<=0||nm>16384||!fixed||!moving)return;
    for(i=0;i<nm;++i){
        portal_view a;int source=*(int*)(moving+i*8);
        if(!portal(map,source,*(int*)(moving+i*8+4),&a))continue;
        for(j=0;j<nf;++j){
            portal_view b;float d[3],distance;int target=*(int*)(fixed+j*24);
            if(source==target||!portal(map,target,*(int*)(fixed+j*24+4),&b)||!correction(&a,&b,d))continue;
            distance=d[0]*d[0]+d[1]*d[1]+d[2]*d[2];
            if(distance<best){best=distance;memcpy(delta,d,sizeof delta);chosen=source;
                chosen_target=target;needs_variant=a.variant||b.variant;}
        }
    }
    if(chosen<0||!needs_variant||best<0.000001f)return;
    edit=*(unsigned char**)(editor+0x204d0);n=*(int*)(edit+0x70);selected=*(int**)(edit+0x68);
    if(n<=0||n>4096||!selected)return;
    for(i=0,j=0;i<n;++i){
        if(!record(map,selected[i])||selected[i]==chosen_target)return;
        if(selected[i]==chosen)j=1;
        for(k=0;k<i;++k)if(selected[k]==selected[i])return;
    }
    if(!j)return;
    /* Move only the user's current module selection, as one rigid group.
     * Entity-local positions, door sizes and every stationary room are intact. */
    for(i=0;i<n;++i){
        unsigned char copy[0x98];memcpy(copy,record(map,selected[i]),sizeof copy);
        for(k=0;k<3;++k)*(float*)(copy+0xc+k*4)+=delta[k];
        memset(copy+0x24,0,12);g_replace(map,(unsigned)selected[i],copy);
    }
    g_reconnect(map);
}
static void snap_exact(void *snap,void *editor,const float *move,unsigned char vertical)
{
    g_snap(snap,editor,move,vertical);
    if(InterlockedCompareExchange(&g_faulted,0,0))return;
    __try {finish_snap(snap,editor);}
    __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);
        backend_log("GRID: doorway alignment disabled after an engine exception");}
}
static int variant_target(unsigned char *map,const float *point)
{
    int i,j,k,n=*(int*)(map+0x758);
    if(n<0||n>4096)return 0;
    for(i=0;i<n;++i){unsigned char *r=record(map,i),*module;
        if(!variant(r))continue;
        module=**(unsigned char***)r;
        for(j=0;j<*(int*)(module+0xf0);++j){portal_view p;
            if(!portal(map,i,j,&p))continue;
            for(k=0;k<3;++k)if(fabsf((p.bounds[k]+p.bounds[k+3])*0.5f-point[k])>0.001f)break;
            if(k==3)return 1;
        }
    }
    return 0;
}
static unsigned char align_exact(void *edit,const float *point,unsigned index,int door,int direction)
{
    int previous=g_exact,enabled=0;unsigned char result;
    if(!InterlockedCompareExchange(&g_faulted,0,0)){
        __try {unsigned char *map=editor_map(g_editor);
            enabled=map&&*(void**)(g_editor+0x204d0)==edit&&
                (variant(record(map,(int)index))||variant_target(map,point));
        } __except(EXCEPTION_EXECUTE_HANDLER){enabled=0;}
    }
    /* This native operation resets the instance origin before calculating its
     * exact target. Suppress only its final XY quantization, on this thread. */
    g_exact=previous||enabled;
    __try {result=g_align(edit,point,index,door,direction);}
    __finally {g_exact=previous;}
    return result;
}
static float *quantize_exact(float *out,const float *value,float step)
{
    if(g_exact){memmove(out,value,3*sizeof(float));return out;}
    return g_quantize(out,value,step);
}
static int selection_needs_exact(void *edit,float step)
{
    unsigned char *map=editor_map(g_editor);int i,k,n,*selected,off_grid=0;
    if(!map||*(void**)(g_editor+0x204d0)!=edit||step!=128.0f)return 0;
    n=*(int*)((unsigned char*)edit+0x70);selected=*(int**)((unsigned char*)edit+0x68);
    if(n<=0||n>4096||!selected)return 0;
    for(i=0;i<n;++i){unsigned char *r=record(map,selected[i]);
        if(!r)return 0;
        for(k=0;k<2;++k){float v=*(float*)(r+0xc+k*4);
            if(!isfinite(v))return 0;
            if(v!=floorf(v/step+0.5f)*step)off_grid=1;
        }
    }
    if(!off_grid)return 0;
    /* A stock neighbor can inherit a fractional origin from a resized room.
     * Preserve the accepted rigid selection in maps containing such a room;
     * stock-only maps retain native confirmation behavior. */
    n=*(int*)(map+0x758);
    for(i=0;i<n;++i)if(variant(record(map,i)))return 1;
    return 0;
}
static void confirm_exact(void *edit,float step)
{
    int previous=g_exact,enabled=0;
    if(!InterlockedCompareExchange(&g_faulted,0,0)){
        __try {enabled=selection_needs_exact(edit,step);}
        __except(EXCEPTION_EXECUTE_HANDLER){enabled=0;}
    }
    /* Add, move and duplicate confirmation quantize again after snapping.
     * Keep native dirty marking and remainder cleanup, but retain the exact
     * accepted origins instead of breaking the connection on mouse release. */
    g_exact=previous||enabled;
    __try {g_confirm(edit,step);}
    __finally {g_exact=previous;}
}
static void *clean(const sig_result *r,size_t n,const char *name)
{size_t i;for(i=0;i<n;++i)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return (void*)r[i].addr;return NULL;}
int sh_grid_snap_install(const sig_result *r,size_t n,const uint8_t *base)
{
    void *snap=clean(r,n,"GridPortalSnap"),*align=clean(r,n,"GridPortalAlign"),*quantize=clean(r,n,"GridQuantizeOrigin");
    void *confirm=clean(r,n,"GridConfirmOrigins");
    if(g_snap||g_align||g_quantize||g_confirm)return g_snap&&g_align&&g_quantize&&g_confirm&&
        hook_is_installed((void*)g_snap)&&hook_is_installed((void*)g_align)&&
        hook_is_installed((void*)g_quantize)&&hook_is_installed((void*)g_confirm);
    g_bounds=(bounds_fn)clean(r,n,"GridPortalBounds");g_direction=(direction_fn)clean(r,n,"GridPortalDirection");
    g_replace=(replace_fn)clean(r,n,"GridReplaceInstance");g_reconnect=(reconnect_fn)clean(r,n,"GridReconnectPortals");
    g_editor=(unsigned char*)glb_resolve(base,"editor_singleton",NULL);
    if(!snap||!align||!quantize||!confirm||!g_bounds||!g_direction||!g_replace||!g_reconnect||!g_editor)return 0;
    /* Each stolen prefix is 15 whole, register/stack-only bytes in both images. */
    g_snap=(snap_fn)hook_prepare(snap,(void*)snap_exact,15);
    g_align=(align_fn)hook_prepare(align,(void*)align_exact,15);
    g_quantize=(quantize_fn)hook_prepare(quantize,(void*)quantize_exact,15);
    /* Confirmation's 17-byte prefix ends after saving XMM6, before any branch. */
    g_confirm=(confirm_fn)hook_prepare(confirm,(void*)confirm_exact,17);
    if(g_snap&&g_align&&g_quantize&&g_confirm&&hook_commit((void*)g_snap)==B2_PATCH_OK&&
        hook_commit((void*)g_align)==B2_PATCH_OK&&hook_commit((void*)g_quantize)==B2_PATCH_OK&&
        hook_commit((void*)g_confirm)==B2_PATCH_OK){
        backend_log("GRID: exact doorway attachment and confirmation installed");return 1;}
    if(g_confirm&&hook_unpatch((void*)g_confirm))g_confirm=NULL;
    if(g_quantize&&hook_unpatch((void*)g_quantize))g_quantize=NULL;
    if(g_align&&hook_unpatch((void*)g_align))g_align=NULL;
    if(g_snap&&hook_unpatch((void*)g_snap))g_snap=NULL;
    return 0;
}
