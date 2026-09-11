#include "../src/backend/grid_room_snap.c"
#include <assert.h>
#include <stdio.h>

static unsigned char editor[0x20700],edit[0x90],map[0x800];
static unsigned char records[3][0x98],wrappers[3][0x98],modules[3][0x140],infos[3][0x240];
static float doors[3][2][7];
static int selected[3]={0},fixed[6]={1,1},moving[2]={0,0};
static unsigned char snap[0x40];
static int replaces,reconnects,originals,quantizes,confirms,throw_align,throw_confirm;
void backend_log(const char *message){(void)message;}
uintptr_t glb_resolve(const uint8_t *b,const char *n,glb_status *s)
{(void)b;(void)n;(void)s;return 0;}
static float *bounds(void *r,float *out,int door)
{unsigned char *p=r,*m=**(unsigned char***)p;float *b=*(float**)(m+0xe8)+door*7;int i;
 for(i=0;i<6;++i)out[i]=b[i]+*(float*)(p+0xc+(i%3)*4);return out;}
static int direction(void *r,int door){(void)r;return door?3:1;}
static void replace(void *m,unsigned index,const void *copy)
{assert(m==map&&index<3);memcpy(records[index],copy,0x98);++replaces;}
static void reconnect(void *m)
{assert(m==map);++reconnects;}
static void original_snap(void *s,void *e,const float *d,unsigned char v)
{assert(s==snap&&e==editor);(void)d;(void)v;++originals;}
static float *quantize(float *out,const float *v,float step)
{++quantizes;out[0]=floorf(v[0]/step+0.5f)*step;out[1]=floorf(v[1]/step+0.5f)*step;out[2]=v[2];return out;}
static unsigned char align(void *e,const float *v,unsigned i,int p,int d)
{float out[3];assert(e==edit);(void)i;(void)p;(void)d;quantize_exact(out,v,128);
 assert(out[0]==v[0]&&out[1]==v[1]);if(throw_align)RaiseException(0xe0011234,0,0,NULL);return 1;}
static void original_confirm(void *e,float step)
{int i;assert(e==edit);++confirms;
 for(i=0;i<*(int*)(edit+0x70);++i){float out[3];unsigned char *r=records[selected[i]];
  quantize_exact(out,(float*)(r+0xc),step);memcpy(r+0xc,out,sizeof out);memset(r+0x24,0,12);}
 if(throw_confirm)RaiseException(0xe0011234,0,0,NULL);}
static void fixture(void)
{
    int i;memset(editor,0,sizeof editor);memset(edit,0,sizeof edit);memset(map,0,sizeof map);
    memset(records,0,sizeof records);memset(snap,0,sizeof snap);
    for(i=0;i<3;++i){
        *(void**)records[i]=wrappers[i];*(void**)wrappers[i]=modules[i];
        *(void**)(wrappers[i]+0x80)=infos[i];*(void**)(modules[i]+0xe8)=doors[i];*(int*)(modules[i]+0xf0)=2;
        *(const char**)(modules[i]+8)=i?"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl":
            "maps/modules/smpgrid/v1/m/900_700_3392.decl";
        doors[i][0][0]=doors[i][1][0]=-256;doors[i][0][3]=doors[i][1][3]=256;
        doors[i][0][5]=doors[i][1][5]=256;
        doors[i][0][1]=doors[i][0][4]=i?3840:1630;
        doors[i][1][1]=doors[i][1][4]=i?-1280:930;
        *(float*)(records[i]+0xc)=7296;
    }
    *(float*)(records[0]+0x10)=-5888;*(float*)(records[1]+0x10)=-2944;
    *(void**)(map+0x750)=records;*(int*)(map+0x758)=3;
    *(void**)edit=map;*(void**)(edit+0x68)=selected;*(int*)(edit+0x70)=1;selected[0]=0;
    *(void**)(editor+0x204c8)=map;*(void**)(editor+0x204d0)=edit;editor[8]=1;
    *(void**)snap=fixed;*(int*)(snap+8)=1;*(void**)(snap+0x18)=moving;*(int*)(snap+0x20)=1;snap[0x30]=1;
    fixed[0]=1;fixed[1]=1;moving[0]=0;moving[1]=0;
    g_editor=editor;g_snap=original_snap;g_bounds=bounds;g_direction=direction;
    g_replace=replace;g_reconnect=reconnect;g_align=align;g_quantize=quantize;g_faulted=g_exact=0;
    g_confirm=original_confirm;
    replaces=reconnects=originals=quantizes=confirms=throw_align=throw_confirm=0;
}
int main(void)
{
    float zero[3]={0},a[6],b[6],point[3]={7296,-4258,128},out[3];unsigned char before[0x98];int i;
    fixture();memcpy(before,records[1],sizeof before);snap_exact(snap,editor,zero,0);
    assert(originals==1&&replaces==1&&reconnects==1&&!g_faulted);
    assert(*(float*)(records[0]+0x10)==-5854&&!memcmp(before,records[1],sizeof before));
    bounds(records[0],a,0);bounds(records[1],b,1);assert(!memcmp(a,b,sizeof a));
    snap_exact(snap,editor,zero,0);assert(replaces==1); /* Already exact. */
    original_confirm(edit,128); /* Reproduce the missed native release step. */
    bounds(records[0],a,0);bounds(records[1],b,1);assert(memcmp(a,b,sizeof a));
    fixture();memcpy(before,records[1],sizeof before);snap_exact(snap,editor,zero,0);
    confirm_exact(edit,128);bounds(records[0],a,0);bounds(records[1],b,1);
    assert(!memcmp(a,b,sizeof a)&&!memcmp(before,records[1],sizeof before));
    assert(confirms==1&&!quantizes&&!g_exact);
    fixture();selected[0]=1;moving[0]=1;moving[1]=1;fixed[0]=0;fixed[1]=0;
    memcpy(before,records[0],sizeof before);snap_exact(snap,editor,zero,0);
    assert(replaces==1&&*(float*)(records[1]+0x10)==-2978&&!memcmp(before,records[0],sizeof before));
    confirm_exact(edit,128);assert(!quantizes&&!g_exact); /* Stock neighbor stays attached too. */
    bounds(records[0],a,0);bounds(records[1],b,1);assert(!memcmp(a,b,sizeof a));
    fixture();selected[1]=2;*(int*)(edit+0x70)=2;snap_exact(snap,editor,zero,0);
    assert(replaces==2&&*(float*)(records[2]+0x10)==34); /* Rigid selection. */
    confirm_exact(edit,128);assert(*(float*)(records[2]+0x10)==34&&!quantizes);
    fixture();snap[0x30]=0;snap_exact(snap,editor,zero,0);assert(!replaces);
    fixture();*(const char**)(modules[0]+8)=sh_grid_stock_name(SH_GRID_MODERN);
    snap_exact(snap,editor,zero,0);assert(!replaces); /* Stock-only operation. */
    fixture();*(float*)(records[0]+0x10)-=31;snap_exact(snap,editor,zero,0);assert(!replaces);
    fixture();doors[1][1][3]+=16;snap_exact(snap,editor,zero,0);assert(!replaces); /* Different door size. */
    fixture();selected[0]=1;snap_exact(snap,editor,zero,0);assert(!replaces); /* Never move a fixed target. */
    fixture();assert(align_exact(edit,point,1,1,3)&&!g_exact&&!quantizes);
    quantize_exact(out,point,128);assert(quantizes==1&&out[1]==-4224);
    throw_align=1;
    __try {align_exact(edit,point,0,0,1);assert(0);}
    __except(EXCEPTION_EXECUTE_HANDLER){assert(!g_exact);}
    fixture();snap_exact(snap,editor,zero,0);throw_confirm=1;
    __try {confirm_exact(edit,128);assert(0);}
    __except(EXCEPTION_EXECUTE_HANDLER){assert(!g_exact);}
    fixture();*(const char**)(modules[0]+8)=sh_grid_stock_name(SH_GRID_MODERN);
    *(float*)(records[0]+0x10)=-5854;confirm_exact(edit,128);
    assert(quantizes==1&&*(float*)(records[0]+0x10)==-5888); /* Stock-only map. */
    fixture();*(float*)(records[0]+0x10)=-5854;confirm_exact(edit,64);
    assert(quantizes==1&&*(float*)(records[0]+0x10)==-5824); /* Other grid commands. */
    for(i=0;i<4;++i){portal_view p={{0,0,0,512,0,256},0,0,1},q=p;float d[3];
        p.direction=i;q.direction=i^2;q.bounds[1]=q.bounds[4]=33.5f;
        assert(correction(&p,&q,d)&&d[1]==33.5f);q.type=1;assert(!correction(&p,&q,d));}
    puts("grid_room_snap_test: passed");return 0;
}
