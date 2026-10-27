#include "../src/backend/grid_room.h"
#include "../src/backend/grid_room_decl.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void close_to(float a, float b) { assert(fabsf(a-b) < .005f); }
static void put32(unsigned char *p, uint32_t v)
{ p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }
static void putf(unsigned char *p, float v) { uint32_t b; memcpy(&b,&v,4); put32(p,b); }
static float getf(const unsigned char *p)
{ uint32_t b=(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; float f; memcpy(&f,&b,4); return f; }

static void geometry(void)
{
    const sh_grid_size sizes[] = {
        {SH_GRID_MODERN,{1536,1024,512}}, {SH_GRID_MODERN,{16384,15360,16384}},
        {SH_GRID_CLASSIC,{512,768,384}}, {SH_GRID_CLASSIC,{16384,12288,8192}}
    };
    unsigned i, axis, j;
    for (i=0;i<sizeof sizes/sizeof sizes[0];++i) {
        sh_grid_warp w; sh_grid_size parsed; char name[100];
        assert(sh_grid_warp_init(&sizes[i],&w));
        assert(sh_grid_name(&sizes[i],name,sizeof name));
        assert(sh_grid_parse_name(name,&parsed));
        assert(parsed.kind==sizes[i].kind && !memcmp(parsed.xyz,sizes[i].xyz,sizeof parsed.xyz));
        for (axis=0;axis<3;++axis) {
            float previous=sh_grid_coordinate(&w,axis,-20000);
            for (j=1;j<=40000;++j) {
                float v=sh_grid_coordinate(&w,axis,(float)j-20000);
                assert(v>previous); previous=v;
            }
        }
        /* All four corners of each opening must undergo one rigid translation. */
        for (axis=0;axis<2;++axis) {
            unsigned side, corner;
            if (sizes[i].kind==SH_GRID_MODERN && axis==0) continue;
            for (side=0;side<2;++side) {
                float delta[3]={0,0,0};
                for (corner=0;corner<4;++corner) {
                    float p[3],q[3],half=sizes[i].kind==SH_GRID_MODERN?256.0f:128.0f;
                    p[axis]=sizes[i].kind==SH_GRID_MODERN?(side?3840.0f:-1280.0f):(side?1280.0f:-1280.0f);
                    p[1-axis]=corner&1?half:-half;
                    p[2]=corner&2?(sizes[i].kind==SH_GRID_MODERN?256.0f:192.0f):0;
                    sh_grid_point(&w,p,q);
                    for(j=0;j<3;++j) {
                        if(!corner)delta[j]=q[j]-p[j];
                        close_to(q[j]-p[j],delta[j]);
                    }
                }
            }
        }
    }
    {
        sh_grid_warp a,b; float point[3]={-2560,3840,3392}, before[3],after[3];
        assert(sh_grid_warp_init(&sizes[0],&a)); sh_grid_point(&a,point,before);
        assert(sh_grid_warp_init(&sizes[1],&b)); sh_grid_point(&a,point,after);
        assert(!memcmp(before,after,sizeof before));
        assert(sh_grid_coordinate(&a,0,-2560)!=sh_grid_coordinate(&b,0,-2560));
    }
}

static void identities(void)
{
    const char *bad[] = {
        "maps/modules/ind_dlc/ind_big_blank_room_4x", "maps/modules/classic/classic_blank_room_extra",
        "maps/modules/smpgrid/v1/m/1536_1024_512/../other", "maps/modules/smpgrid/v1/m/01536_1024_512",
        "maps/modules/smpgrid/v1/m/1536_1024_512.decl.extra", "maps/modules/smpgrid/v2/m/1536_1024_512",
        "maps/modules/smpgrid/v1/m/999999999999999_1024_512", "maps/modules/smpgrid/v1/m/512_512_128",
        "maps/modules/smpgrid/v1/m/1536_-1_512", "maps/modules/smpgrid/v1/m/1536_1024_nan"
    };
    sh_grid_size s; sh_grid_warp w; unsigned i; char tiny[2];
    for(i=0;i<sizeof bad/sizeof bad[0];++i) {
        assert(!sh_grid_stock_kind(bad[i])); assert(!sh_grid_parse_name(bad[i],&s));
    }
    assert(sh_grid_stock_kind("maps/modules/classic/classic_blank_room.decl")==SH_GRID_CLASSIC);
    assert(sh_grid_default(SH_GRID_MODERN,&s));
    assert(!sh_grid_name(&s,tiny,sizeof tiny) && !tiny[0]);
    s.xyz[2]=20000; assert(sh_grid_warp_init(&s,&w));
    s.xyz[2]=32768; assert(!sh_grid_warp_init(&s,&w));
    {float requested[]={-10,500,20000};
        assert(sh_grid_clamp(SH_GRID_MODERN,requested,&s));
        assert(s.xyz[0]==864&&s.xyz[1]==500&&s.xyz[2]==20000&&sh_grid_warp_init(&s,&w));
        assert(sh_grid_clamp(SH_GRID_CLASSIC,requested,&s));
        assert(s.xyz[0]==416&&s.xyz[1]==500&&s.xyz[2]==20000&&sh_grid_warp_init(&s,&w));
        requested[0]=requested[1]=requested[2]=1e20f;
        assert(sh_grid_clamp(SH_GRID_MODERN,requested,&s));
        assert(s.xyz[0]==65342&&s.xyz[1]==62846&&s.xyz[2]==32767);
        requested[0]=INFINITY;assert(!sh_grid_clamp(SH_GRID_MODERN,requested,&s));
        assert(!sh_grid_clamp(SH_GRID_NONE,requested,&s));
    }
}

static void lights(void)
{
    sh_grid_size s={SH_GRID_MODERN,{1536,1024,512}}; sh_grid_warp w;
    float p[3]={0,1264,1436},r[3]={3535.165527f,3459.952637f,2556.275879f},c[3]={0,0,1568};
    float o[3],nr[3],nc[3];
    assert(sh_grid_warp_init(&s,&w)); assert(sh_grid_light(&w,p,r,c,1,o,nr,nc));
    close_to(o[1],1276.8f); close_to(o[2]+nc[2],348);
    close_to(nr[0],1060.5496581f); assert(o[2]>0 && o[2]<464 && nr[2]>0);
    r[0]=-1; assert(!sh_grid_light(&w,p,r,c,1,o,nr,nc));
}

static void near_rel(float a, float b) { assert(fabsf(a-b) <= .0005f*fmaxf(1,fabsf(b))); }

/* Synthetic modern-room light fixture with the stock geometry roles: a probe,
 * a quarter-turn static point light, an unrotated fill light and the
 * roof-anchored dynamic point. Values are illustrative, not game data. */
static const char light_module[] =
    "{edit={module={name=\"modules/ind_dlc/ind_totally_blank_room_4x.map\";"
    "bounds={b={b[0]={x=-2560;y=-1280;z=0;}b[1]={x=2560;y=3840;z=3392;}}}"
    "portals={num=0;}entities={num=5;"
    "item[0]={name=\"probe_a\";text=\"{inherit=\\\"light/probe\\\";edit={spawnPosition={y=1248;z=1659;}lightRadius={x=2696;y=2699;z=1850;}}}\";}"
    "item[1]={name=\"point_turned\";text=\"{inherit=\\\"light/point\\\";edit={spawnPosition={x=2;y=1264;z=1600;}"
        "spawnOrientation={mat={mat[0]={x=0;y=-1;}mat[1]={x=1;y=0;}}}lightRadius={x=2500;y=2900;z=2300;}maxShadowVisibleRange=2500;}}\";}"
    "item[2]={name=\"point_fill\";text=\"{inherit=\\\"light/point\\\";edit={spawnPosition={x=-1280;z=1600;}lightRadius={x=1536;y=1536;z=2048;}}}\";}"
    "item[3]={name=\"room_light\";text=\"{inherit=\\\"snapmaps/light/dynamic_point\\\";edit={spawnPosition={y=1264;z=1436;}"
        "lightRadius={x=3535;y=3460;z=2556;}lightCenter={z=1568;}maxVisibleRange=10000;maxShadowVisibleRange=6000;}}\";}"
    "item[4]={name=\"not_a_light\";text=\"{inherit=\\\"info/cover\\\";edit={spawnPosition={x=-1024;}}}\";}"
    "}}}}";

static const sh_grid_builtin_light *find_light(const sh_grid_builtin_light *l, size_t n, const char *name)
{ size_t i; for (i=0;i<n;++i) if (!strcmp(l[i].name,name)) return &l[i]; assert(0); return NULL; }

static void light_cases(void)
{
    sh_grid_size stock, s; sh_grid_warp w;
    const float p[3]={0,1264,1436}, r[3]={3535,3460,2556}, c[3]={0,0,1568};
    float o[3], nr[3], nc[3];
    sh_grid_default(SH_GRID_MODERN,&stock);
    /* Stock size is the identity, including the roof anchor. */
    assert(sh_grid_warp_init(&stock,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    near_rel(o[1],1264); near_rel(o[2],1436); near_rel(nr[0],3535); near_rel(nr[2],2556); near_rel(nc[2],1568);
    /* X only: X coverage doubles, height and depth are untouched. */
    s=stock; s.xyz[0]=10240; assert(sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    near_rel(nr[0],7070); near_rel(nr[1],3460); near_rel(nr[2],2556); near_rel(o[2]+nc[2],3004);
    /* Y only: depth scales about the modern Y pivot 1280. */
    s=stock; s.xyz[1]=2560; assert(sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    near_rel(o[1],1272); near_rel(nr[1],1730); near_rel(nr[0],3535); near_rel(o[2]+nc[2],3004);
    /* Z only: height follows the interior (height-48), the effective point
     * keeps its 340-unit stock clearance below the ceiling. */
    s=stock; s.xyz[2]=6736; assert(sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    near_rel(o[2],2872); near_rel(nr[2],5112); near_rel(nr[0],3535); near_rel(o[2]+nc[2],6688-340);
    /* All three axes together equal the per-axis composition. */
    s.xyz[0]=10240; s.xyz[1]=2560; assert(sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    near_rel(nr[0],7070); near_rel(nr[1],1730); near_rel(nr[2],5112); near_rel(o[1],1272); near_rel(o[2]+nc[2],6348);
    /* Shrink below stock: the clearance is limited to a quarter of the height. */
    s.xyz[0]=864; s.xyz[1]=272; s.xyz[2]=432; assert(sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    assert(o[2]>0&&o[2]<384&&o[2]+nc[2]<384); near_rel(o[2]+nc[2],384-96); near_rel(nr[0],3535.f*864/5120);
    /* Extreme: the format maximum still yields finite, contained geometry. */
    assert(sh_grid_maximum(SH_GRID_MODERN,&s)&&sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,p,r,c,1,o,nr,nc));
    near_rel(o[2]+nc[2],32719-340); assert(nr[0]>40000&&o[1]>-30000&&o[1]<33000);
    /* Classic: no pivot offset, no ceiling margin, no anchor. */
    sh_grid_default(SH_GRID_CLASSIC,&s); s.xyz[2]=4096;
    {const float cp[3]={0,0,1024},cr[3]={1296,1296,1040},cc[3]={0,0,0};
        assert(sh_grid_warp_init(&s,&w)&&sh_grid_light(&w,cp,cr,cc,0,o,nr,nc));
        near_rel(o[2],2048); near_rel(nr[2],2080); near_rel(nr[0],1296); near_rel(nc[2],0);}
    /* A quarter-turn light's X extent follows room Y and vice versa. */
    {const float axis[9]={0,-1,0, 1,0,0, 0,0,1},pr[3]={2500,2900,2300},zero[3]={0,0,0},pp[3]={2,1264,1600};
        s=stock; s.xyz[0]=10240; assert(sh_grid_warp_init(&s,&w));
        assert(sh_grid_light_axes(&w,pp,pr,zero,axis,0,o,nr,nc));
        near_rel(nr[0],2500); near_rel(nr[1],5800); near_rel(nr[2],2300);
        s=stock; s.xyz[1]=2560; assert(sh_grid_warp_init(&s,&w)&&sh_grid_light_axes(&w,pp,pr,zero,axis,0,o,nr,nc));
        near_rel(nr[0],1250); near_rel(nr[1],2900);
        {const float bad[9]={2,0,0, 0,1,0, 0,0,1}; assert(!sh_grid_light_axes(&w,pp,pr,zero,bad,0,o,nr,nc));}}
    /* Distance cutoffs grow with the largest ratio and never shrink. */
    s=stock; s.xyz[2]=432; near_rel(sh_grid_light_range_scale(&s),1);
    s.xyz[0]=20480; near_rel(sh_grid_light_range_scale(&s),4);
}

/* Map load and live resize must converge: the generated variant declaration
 * (what a saved map loads and Play spawns) carries exactly the values the
 * editor writes when the same room is resized. */
static void light_convergence(void)
{
    const sh_grid_size sizes[] = {
        {SH_GRID_MODERN,{10240,5120,3392}}, {SH_GRID_MODERN,{5120,2560,3392}},
        {SH_GRID_MODERN,{5120,5120,6736}}, {SH_GRID_MODERN,{16384,15360,16384}},
        {SH_GRID_MODERN,{864,272,432}}, {SH_GRID_MODERN,{5120,5120,3392}}
    };
    sh_grid_size stock; unsigned i, k, a;
    sh_grid_default(SH_GRID_MODERN,&stock);
    for (i=0;i<sizeof sizes/sizeof sizes[0];++i) {
        sh_grid_builtin_light resize[16], loaded[16]; size_t n=0, m=0, bytes; char *variant;
        assert(sh_grid_decl_lights(&sizes[i],light_module,strlen(light_module),resize,16,&n));
        assert(n==4); /* probe, two static points, dynamic point; cover ignored */
        variant=sh_grid_decl(&sizes[i],light_module,strlen(light_module),0,&bytes);
        assert(variant);
        assert(sh_grid_decl_light_values(variant,bytes,loaded,16,&m)&&m==n);
        for (k=0;k<n;++k) {
            const sh_grid_builtin_light *l=find_light(loaded,m,resize[k].name);
            assert(!strcmp(l->inherit,resize[k].inherit)&&l->has_radius==resize[k].has_radius);
            for (a=0;a<3;++a) {
                near_rel(l->origin[a],resize[k].origin[a]);
                near_rel(l->radius[a],resize[k].radius[a]);
                assert(l->origin[a]==resize[k].origin[a]&&l->radius[a]==resize[k].radius[a]);
                assert(l->center[a]==resize[k].center[a]);
            }
            assert(l->visible_range==resize[k].visible_range&&l->shadow_range==resize[k].shadow_range);
        }
        {const sh_grid_builtin_light *t=find_light(resize,n,"point_turned");
            near_rel(t->radius[0],2500.f*sizes[i].xyz[1]/5120); near_rel(t->radius[1],2900.f*sizes[i].xyz[0]/5120);}
        free(variant);
    }
}

static void buffers(void)
{
    sh_grid_size s={SH_GRID_MODERN,{1536,1024,512}}; sh_grid_warp w;
    unsigned char src[512]={0},out[512]; size_t i;
    const size_t vertex=80, n=266;
    assert(sh_grid_warp_init(&s,&w));
    put32(src,0x1a4c4d42); put32(src+8,1); /* empty material name */
    put32(src+28,3); put32(src+32,3); put32(src+36,0x1801f);
    putf(src+vertex,-2560); putf(src+vertex+4,-1280);
    putf(src+vertex+48,2560); putf(src+vertex+52,-1280);
    putf(src+vertex+96,2560); putf(src+vertex+100,3840);
    src[227]=1;src[229]=2;
    putf(src+230,-2560);putf(src+234,-1280);putf(src+242,2560);putf(src+246,3840);
    put32(src+258,0x1a4c4d42);
    assert(sh_grid_bmodel(&w,src,n,out));
    close_to(getf(out+vertex),-768);close_to(getf(out+vertex+4),768);
    close_to(getf(out+vertex+48),768);close_to(getf(out+vertex+100),1792);
    for(i=0;i<n;++i) assert(!sh_grid_bmodel(&w,src,i,out));
    src[229]=3;assert(!sh_grid_bmodel(&w,src,n,out));src[229]=2;
    put32(src+28,0xffffffff);assert(!sh_grid_bmodel(&w,src,n,out));
    memset(src,0,sizeof src);memcpy(src,"BCM8",4);memcpy(src+65,"BCM8",4);
    assert(sh_grid_bcm(&w,src,69,out));
    for(i=0;i<69;++i)assert(!sh_grid_bcm(&w,src,i,out));
    src[56]=1;assert(!sh_grid_bcm(&w,src,69,out));
}

/* Optional read-only test against a player's extracted file, never a fixture
 * stored in the product. Exit nonzero for an unhandled format or deformation. */
static int asset(const char *path, const char *kind)
{
    FILE *f=fopen(path,"rb");long length;unsigned char *s,*d;int ok;
    sh_grid_size size={SH_GRID_MODERN,{1536,1024,512}};sh_grid_warp w;
    if(!strncmp(kind,"classic",7)){size.kind=SH_GRID_CLASSIC;size.xyz[0]=512;size.xyz[1]=768;size.xyz[2]=384;}
    if(!f)return 2;
    fseek(f,0,SEEK_END);length=ftell(f);rewind(f);
    if(length<0||length>64*1024*1024){fclose(f);return 2;}
    s=(unsigned char*)malloc((size_t)length);d=(unsigned char*)malloc((size_t)length);
    if(!s||!d){fclose(f);free(s);free(d);return 2;}
    ok=fread(s,1,(size_t)length,f)==(size_t)length;fclose(f);
    if(ok){assert(sh_grid_warp_init(&size,&w));
        if(length>0&&s[0]=='{'){
            size_t bytes;char *text=sh_grid_decl(&size,(const char*)s,(size_t)length,strstr(kind,"info")!=NULL,&bytes);
            ok=text!=NULL;free(text);
        }else ok=length>=4&&!memcmp(s,"BCM8",4)?
            sh_grid_bcm(&w,s,(size_t)length,d):sh_grid_bmodel(&w,s,(size_t)length,d);}
    printf("%s %s\n",ok?"PASS":"REFUSED",path);free(s);free(d);return ok?0:1;
}

int main(int argc,char **argv)
{
    if(argc==3)return asset(argv[1],argv[2]);
    identities();geometry();lights();light_cases();light_convergence();buffers();puts("grid_room_test: passed");return 0;
}
