/* The world-area cell size redirect on scratch code, never game bytes. */
#include "../src/backend/world_area_cell.c"
#include <assert.h>

static char last_log[512];
static int adaptive=-1,count_ok=1,map_n;
static sh_render_cell_choice next_choice;
static void *editor_map;
void backend_log(const char *message){strncpy_s(last_log,sizeof last_log,message,_TRUNCATE);}
void sh_grid_native_set_adaptive_cells(int live){adaptive=live;}
int sh_grid_native_render_cell(void *map,sh_render_cell_choice *out)
{(void)map;if(!count_ok)return 0;*out=next_choice;return 1;}
void *sh_grid_native_editor_map(void){return editor_map;}

/* A BuildRenderWorld-shaped site: the cell-size load, its store, then the
 * 512.0 constant it reads 0x40 bytes further on. */
static unsigned char *make_site(float value)
{
    static const unsigned char code[14]={0xf3,0x0f,0x10,0x05,0x32,0,0,0,0xf3,0x0f,0x11,0x44,0x24,0x20};
    unsigned char *p=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(p);memcpy(p,code,sizeof code);memcpy(p+0x3a,&value,4);
    return p;
}
static float site_reads(const unsigned char *site)
{
    int32_t disp;memcpy(&disp,site+4,4);
    return *(const volatile float*)(site+8+disp);
}
int main(void)
{
    sig_result r={"WorldAreaCellLoad",SIG_OK,0,0};
    unsigned char *site=make_site(512.0f),*bad;uintptr_t constant;int32_t disp;
    unsigned char map[0x800]={0};

    /* Pure checks: the decoded operand, and reach. */
    assert(sh_world_area_cell_site(site,&constant)&&constant==(uintptr_t)(site+0x3a));
    site[9]^=1;assert(!sh_world_area_cell_site(site,&constant));site[9]^=1;
    site[3]=0x0d;assert(!sh_world_area_cell_site(site,&constant));site[3]=0x05;
    assert(!sh_world_area_cell_site(NULL,&constant));
    assert(sh_world_area_cell_disp(0x140000000u,0x140000100u,&disp)&&disp==0xf8);
    assert(sh_world_area_cell_disp(0x140000000u,0x100000000u,&disp)&&disp==(int32_t)(0x100000000-0x140000008));
    assert(!sh_world_area_cell_disp(0x140000000u,0x240000000u,&disp));

    /* Unavailable: no binding, wrong bytes, wrong constant. Nothing is patched
     * and resizes keep the render area limit. */
    assert(!sh_world_area_cell_install(&r,0,NULL)&&adaptive==0&&strstr(last_log,"not found"));
    bad=make_site(511.0f);r.addr=(uintptr_t)bad;
    assert(!sh_world_area_cell_install(&r,1,NULL)&&adaptive==0&&strstr(last_log,"not 512"));
    assert(site_reads(bad)==511.0f&&!g_patch.live&&!g_page);
    {float native=512.0f;bad[8]=0x90;memcpy(bad+0x3a,&native,4);}
    assert(!sh_world_area_cell_install(&r,1,NULL)&&strstr(last_log,"unexpected bytes")&&!g_page);
    r.status=SIG_OK_HOOKED;r.addr=(uintptr_t)site;
    assert(!sh_world_area_cell_install(&r,1,NULL)&&!g_live);
    /* Not live: the hooks do nothing. */
    sh_world_area_cell_build(map);sh_world_area_cell_build_end();
    assert(sh_world_area_cell_current()==512.0f&&site_reads(site)==512.0f);

    /* Installed: the load reads the product page, 512 at rest. */
    r.status=SIG_OK;
    assert(sh_world_area_cell_install(&r,1,NULL)&&g_live&&adaptive==1&&g_patch.live);
    assert(strstr(last_log,"follows the map"));
    assert(site_reads(site)==512.0f&&sh_world_area_cell_site(site,&constant)&&constant==(uintptr_t)&g_page->cell);
    assert(sh_world_area_cell_install(&r,1,NULL));

    /* A map that needs 1024 gets it for the build only. */
    next_choice.cell=1024;next_choice.fits=1;next_choice.count.areas=4160;next_choice.count.nodes=4159;
    next_choice.native.areas=16512;next_choice.native.nodes=16511;
    sh_world_area_cell_build(map);
    assert(site_reads(site)==1024.0f);
    assert(!strcmp(last_log,"RENDER: world-area cell size 1024 for 4160 areas / 4159 nodes (512 would give 16512 areas / 16511 nodes)"));
    sh_world_area_cell_build_end();assert(site_reads(site)==512.0f);
    /* A map that fits keeps 512. */
    next_choice.cell=512;next_choice.count.areas=272;next_choice.count.nodes=271;next_choice.native=next_choice.count;
    sh_world_area_cell_build(map);
    assert(site_reads(site)==512.0f&&!strcmp(last_log,"RENDER: world-area cell size 512 for 272 areas / 271 nodes"));
    sh_world_area_cell_build_end();
    /* Nothing fits: the coarsest size is used and the log says the map cannot load. */
    next_choice.cell=65536;next_choice.fits=0;next_choice.count.areas=9000;next_choice.count.nodes=9100;
    sh_world_area_cell_build(map);
    assert(site_reads(site)==65536.0f&&strstr(last_log,"over the engine maximum of 8192; this map cannot load"));
    sh_world_area_cell_build_end();assert(site_reads(site)==512.0f);
    /* An uncountable map builds at 512. */
    count_ok=0;sh_world_area_cell_build(map);
    assert(site_reads(site)==512.0f&&strstr(last_log,"could not be counted"));count_ok=1;

    /* Opening a map logs once its module count has settled. */
    last_log[0]=0;next_choice.cell=1024;next_choice.fits=1;next_choice.count.areas=4160;next_choice.count.nodes=4159;
    sh_world_area_cell_map_opened();
    {int i;for(i=0;i<100;++i)sh_world_area_cell_editor_tick();}
    assert(!last_log[0]);
    editor_map=map;*(int*)(map+0x758)=1;
    {int i;for(i=0;i<59;++i)sh_world_area_cell_editor_tick();}
    assert(!last_log[0]);
    *(int*)(map+0x758)=2;
    {int i;for(i=0;i<60;++i)sh_world_area_cell_editor_tick();}
    assert(!last_log[0]);
    sh_world_area_cell_editor_tick();
    assert(strstr(last_log,"RENDER: map opened; Play will use world-area cell size 1024 for 4160 areas"));
    last_log[0]=0;sh_world_area_cell_editor_tick();assert(!last_log[0]);

    /* The redirect is removable: unpatching restores the native operand. */
    assert(code_unpatch(&g_patch)==B2_PATCH_OK&&site_reads(site)==512.0f);
    assert(sh_world_area_cell_site(site,&constant)&&constant==(uintptr_t)(site+0x3a));
    puts("world_area_cell_test: passed");
    return 0;
}
