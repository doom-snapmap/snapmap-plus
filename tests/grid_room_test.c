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
static void put16(unsigned char *p, int v) { p[0]=(unsigned char)((unsigned)v>>8); p[1]=(unsigned char)v; }
static int get16(const unsigned char *p) { int v=p[0]<<8|p[1]; return v>=32768?v-65536:v; }
static void putv(unsigned char *p, float x, float y, float z) { putf(p,x); putf(p+4,y); putf(p+8,z); }
static void putp(unsigned char *p, float x, float y, float z, float d) { putv(p,x,y,z); putf(p+12,d); }

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

/* A sound BSP with one quad on the x=2560 wall and an ambient field of two
 * samples, warped into a 1536x1024x512 modern room. */
static void members(void)
{
    sh_grid_size s={SH_GRID_MODERN,{1536,1024,512}},big={SH_GRID_MODERN,{16384,15360,16384}};
    sh_grid_warp w,wb; unsigned char src[400]={0},out[400]; size_t n,i,at=4,planes;
    assert(sh_grid_warp_init(&s,&w)&&sh_grid_warp_init(&big,&wb));
    put32(src,0x534e440b);put32(src+at,2);at+=4;planes=at;
    putp(src+at,1,0,0,-2560);putp(src+at+16,0,0,-1,0);at+=32;
    put32(src+at,4);at+=4;
    putv(src+at,2560,-1280,0);putv(src+at+12,2560,3840,0);putv(src+at+24,2560,3840,3392);putv(src+at+36,2560,-1280,3392);at+=48;
    put32(src+at,4);at+=4;for(i=0;i<4;++i)put32(src+at+i*4,(uint32_t)i);at+=16;
    put32(src+at,2);at+=4+16;
    put16(src+at,0);put16(src+at+2,4);put16(src+at+4,0);put32(src+at+6,0x00010002);
    put16(src+at+10,2560);put16(src+at+12,1280);put16(src+at+14,1696);at+=16;
    put32(src+at,0);put32(src+at+4,0);put32(src+at+8,0);at+=12;
    put32(src+at,2);put32(src+at+4,0);put32(src+at+8,0x534e440b);n=at+12;
    assert(sh_grid_sbsp(&w,src,n,out));
    close_to(getf(out+planes+12),-768);close_to(getf(out+planes+16+12),0);
    close_to(getf(out+planes+36),768);close_to(getf(out+planes+40),768);close_to(getf(out+planes+52),1792);
    close_to(getf(out+planes+68),512);
    assert(get16(out+n-40+10)==768&&get16(out+n-40+12)==1280&&get16(out+n-40+14)==256);
    assert(!memcmp(out+n-40,src+n-40,10));
    for(i=0;i<n;++i)assert(!sh_grid_sbsp(&w,src,i,out));
    putf(src+planes+4,.5f);assert(!sh_grid_sbsp(&w,src,n,out));putf(src+planes+4,0);
    put32(src,0x534e4408);assert(!sh_grid_sbsp(&w,src,n,out));put32(src,0x534e440b);
    put32(src+n-4,0x534e440a);assert(!sh_grid_sbsp(&w,src,n,out));put32(src+n-4,0x534e440b);
    put32(src+planes+32+4+48+4+12,4);assert(!sh_grid_sbsp(&w,src,n,out));put32(src+planes+32+4+48+4+12,3);
    assert(sh_grid_sbsp(&w,src,n,out));

    memset(src,0,sizeof src);put32(src,0x21155702);put32(src+4,1);put32(src+8,2);
    for(i=0;i<8;++i)put32(src+12+i*4,0x40000000u|(uint32_t)(i&1));
    for(i=0;i<54;++i)src[44+i]=src[104+i]=(unsigned char)(i*7+1);
    put16(src+44+54,2560);put16(src+44+56,-1280);put16(src+44+58,0);
    put16(src+104+54,0);put16(src+104+56,1280);put16(src+104+58,1696);n=164;
    assert(sh_grid_ambient(&w,src,n,out));
    assert(!memcmp(out,src,44)&&!memcmp(out+44,src+44,54)&&!memcmp(out+104,src+104,54));
    assert(get16(out+98)==768&&get16(out+100)==768&&get16(out+102)==0);
    assert(get16(out+158)==0&&get16(out+160)==1280&&get16(out+162)==395);
    for(i=0;i<n;++i)assert(!sh_grid_ambient(&w,src,i,out));
    put32(src+12,0x40000002u);assert(!sh_grid_ambient(&w,src,n,out));
    put32(src+12,1);assert(!sh_grid_ambient(&w,src,n,out));
    put32(src+12,0x80000000u);assert(!sh_grid_ambient(&w,src,n,out));put32(src+12,0x40000000u);
    put16(src+44+54,32000);assert(!sh_grid_ambient(&wb,src,n,out));
    put32(src,0x21155703);assert(!sh_grid_ambient(&w,src,n,out));
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
        }else if(length>=4&&!memcmp(s,"SND",3))ok=sh_grid_sbsp(&w,s,(size_t)length,d);
        else if(length>=4&&!memcmp(s,"!W",4))ok=sh_grid_ambient(&w,s,(size_t)length,d);
        else ok=length>=4&&!memcmp(s,"BCM8",4)?
            sh_grid_bcm(&w,s,(size_t)length,d):sh_grid_bmodel(&w,s,(size_t)length,d);}
    printf("%s %s\n",ok?"PASS":"REFUSED",path);free(s);free(d);return ok?0:1;
}

int main(int argc,char **argv)
{
    if(argc==3)return asset(argv[1],argv[2]);
    identities();geometry();lights();buffers();members();puts("grid_room_test: passed");return 0;
}
