/* The Object Mode entry drop redirect on scratch code, never game bytes. */
#include "../src/backend/object_entry.c"
#include <assert.h>
#include <math.h>

static char last_log[512];
void backend_log(const char *message){strncpy_s(last_log,sizeof last_log,message,_TRUNCATE);}

/* An entry-solver-shaped site: MOVSS xmm9,[rip+0x37] (the 18364.0 constant
 * 0x40 bytes on), XORPS xmm7,xmm7, MOVSS xmm8,[rip+...]. */
static unsigned char *make_site(float value)
{
    static const unsigned char code[17]={0xf3,0x44,0x0f,0x10,0x0d,0x37,0,0,0,
        0x0f,0x57,0xff,0xf3,0x44,0x0f,0x10,0x05};
    unsigned char *p=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(p);memcpy(p,code,sizeof code);memcpy(p+0x40,&value,4);
    return p;
}
static float site_reads(const unsigned char *site)
{
    int32_t disp;memcpy(&disp,site+5,4);
    return *(const volatile float*)(site+9+disp);
}
/* The solver's descent (VK FUN_1412b3780) over a column of surfaces given
 * top-down: start 100 above the map top, segments of `drop`, restart 1 below
 * each hit, stop at the first miss. Returns the index of the surface it keeps,
 * or -1 when the first segment misses (the previous FPS position is kept). */
static int descend(float map_top,const float *surface,int n,float drop)
{
    float z=map_top+100.0f;int i=0,kept=-1;
    for(;;){
        float end=z-drop;
        while(i<n&&surface[i]>z)++i;
        if(i==n||surface[i]<end)return kept;
        kept=i;z=surface[i]-1.0f;++i;
    }
}
int main(void)
{
    sig_result r={"ObjectEntryDrop",SIG_OK,0,0};
    unsigned char *site=make_site(18364.0f),*bad;uintptr_t constant;int32_t disp;

    /* Pure checks: the decoded operand, and reach. */
    assert(sh_object_entry_site(site,&constant)&&constant==(uintptr_t)(site+0x40));
    site[10]^=1;assert(!sh_object_entry_site(site,&constant));site[10]^=1;
    site[4]=0x05;assert(!sh_object_entry_site(site,&constant));site[4]=0x0d;
    site[1]=0x45;assert(!sh_object_entry_site(site,&constant));site[1]=0x44;
    assert(!sh_object_entry_site(NULL,&constant));
    assert(sh_object_entry_disp(0x140000000u,0x140000100u,&disp)&&disp==0xf7);
    assert(!sh_object_entry_disp(0x140000000u,0x240000000u,&disp));

    /* Unavailable: no binding, wrong constant, wrong bytes, hooked. Nothing is patched. */
    assert(!sh_object_entry_install(&r,0)&&strstr(last_log,"not found"));
    bad=make_site(18000.0f);r.addr=(uintptr_t)bad;
    assert(!sh_object_entry_install(&r,1)&&strstr(last_log,"not 18364")&&!g_page&&!g_patch.live);
    assert(site_reads(bad)==18000.0f);
    {float native=18364.0f;memcpy(bad+0x40,&native,4);bad[9]=0x90;}
    assert(!sh_object_entry_install(&r,1)&&strstr(last_log,"unexpected bytes")&&!g_page);
    r.status=SIG_OK_HOOKED;r.addr=(uintptr_t)site;
    assert(!sh_object_entry_install(&r,1)&&!g_live&&strstr(last_log,"lands under its roof"));
    assert(sh_object_entry_drop()==SH_OBJECT_ENTRY_NATIVE_DROP&&site_reads(site)==18364.0f);

    /* Installed: the load reads the product page. A second install is a no-op. */
    r.status=SIG_OK;
    assert(sh_object_entry_install(&r,1)&&g_live&&g_patch.live&&strstr(last_log,"entry drop 66048"));
    assert(site_reads(site)==SH_OBJECT_ENTRY_DROP&&sh_object_entry_drop()==SH_OBJECT_ENTRY_DROP);
    assert(sh_object_entry_site(site,&constant)&&constant==(uintptr_t)&g_page->drop);
    assert(sh_object_entry_install(&r,1));

    /* The length covers the whole coordinate range: from 100 above the
     * highest module top to below the lowest module bottom. */
    assert(SH_OBJECT_ENTRY_DROP>=32671.0f+100.0f+32671.0f);

    /* The descent, natively and with the published length. */
    {
        /* Stock room: roof 1280 over the floor at 0; any stock map's tallest gap
         * is well inside 18364, so both lengths keep the same surface. */
        float stock[]={1280.0f,1248.0f,0.0f,-32.0f};
        assert(descend(1280.0f,stock,4,18364.0f)==3&&descend(1280.0f,stock,4,SH_OBJECT_ENTRY_DROP)==3);
        /* Two stacked stock floors (25 floors of 192 either way): same answer. */
        {float stacked[]={4800.0f+1280.0f,4800.0f,-4800.0f+1280.0f,-4800.0f};
         assert(descend(6080.0f,stacked,4,18364.0f)==descend(6080.0f,stacked,4,SH_OBJECT_ENTRY_DROP));}
        /* test2's room: 32671 tall, roof slab at the top. Natively the descent
         * stops on the roof's underside; with the published length it reaches
         * the floor. */
        {float tall[]={16335.0f,16303.0f,-16336.0f+32.0f,-16336.0f};
         assert(descend(16335.0f,tall,4,18364.0f)==1);
         assert(descend(16335.0f,tall,4,SH_OBJECT_ENTRY_DROP)==3);}
        /* A stock module beside it, its top 30000 below the map top: natively
         * the first segment misses and the previous position is kept. */
        {float beside[]={-16336.0f+1280.0f,-16336.0f};
         assert(descend(16335.0f,beside,2,18364.0f)==-1);
         assert(descend(16335.0f,beside,2,SH_OBJECT_ENTRY_DROP)==1);}
        /* Extremes of the coordinate range still reach. */
        {float far_[]={-32671.0f};assert(descend(32671.0f,far_,1,SH_OBJECT_ENTRY_DROP)==0);}
    }

    /* The redirect is removable: unpatching restores the native operand. */
    assert(code_unpatch(&g_patch)==B2_PATCH_OK&&site_reads(site)==18364.0f);
    assert(sh_object_entry_site(site,&constant)&&constant==(uintptr_t)(site+0x40));
    puts("object_entry_test: passed");
    return 0;
}
