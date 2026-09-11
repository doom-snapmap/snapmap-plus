/* Synthetic clearance and compaction regression; optional installed AAS audit. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "grid_room_nav.h"
#include "navmesh.h"

void backend_log(const char *s){(void)s;}
int sh_config_get_bool(const char *s,int *v,unsigned *f)
{(void)s;if(v)*v=1;if(f)*f=0;return 1;}
static sh_aas *fixture(void)
{
    unsigned char bytes[2048]={0};size_t off=394;unsigned i,j;char err[192];sh_aas *a;
    unsigned n[22]={1,9,9,8,2,3,2,1,0,2,3,0,0,0,0,3,2,0,1,1,1,3};
    memcpy(bytes,"2SAA",4);bytes[4]=3;bytes[5]=29;
    for(i=0;i<3;i++)sh_aas_put_u32(bytes,34+i*68,64);
    for(i=0;i<6;i++)sh_aas_put_f32(bytes,238+i*4,i==2?0:i==5?180:i<3?-64:64);
    for(i=0;i<22;i++){
        sh_aas_put_u32(bytes,(unsigned)off,n[i]);off+=4;
        off+=n[i]*sh_aas_record_size((int)i);
    }
    assert(off<=sizeof bytes);a=sh_aas_parse(bytes,off,err,sizeof err);assert(a);
    /* Floor outside the doorway strip, then the strip that disappears for
     * a 128-unit demon when the modern room is only 272 units deep. */
    for(i=0;i<2;i++){
        float x0=i?-320:-2496,x1=i?-128:-320;
        float y0=i?-1120:-1208,y1=i?3680:3768;
        unsigned char *ar=sh_aas_rec(a,SH_AAS_L_AREAS,i+1);
        for(j=0;j<4;j++){
            unsigned char *v=sh_aas_rec(a,SH_AAS_L_VERTICES,1+i*4+j);
            unsigned char *e=sh_aas_rec(a,SH_AAS_L_EDGES,1+i*4+j);
            sh_aas_put_f32(v,0,(j==1||j==2)?x1:x0);sh_aas_put_f32(v,4,j>=2?y1:y0);
            sh_aas_put_u32(e,0,1+i*4+j);sh_aas_put_u32(e,4,1+i*4+(j+1)%4);
            sh_aas_put_i32(sh_aas_rec(a,SH_AAS_L_EDGEINDEX,i*4+j),0,(int)(1+i*4+j));
        }
        sh_aas_put_u16(ar,6,4);sh_aas_put_u32(ar,8,i*4);sh_aas_put_u16(ar,12,1);
        sh_aas_put_u16(ar,14,(uint16_t)i);sh_aas_put_u32(ar,16,i+1);
        sh_aas_put_i32(ar,20,(int)i);sh_aas_put_i32(ar,24,1-(int)i);
        sh_aas_put_u16(ar,32,(uint16_t)i);sh_aas_put_u16(ar,34,1);
        sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_AREACOVERINDEX,i),0,i+1);
        sh_aas_put_u16(sh_aas_rec(a,SH_AAS_L_COVER,i+1),30,(uint16_t)(i+1));
        {unsigned char *r=sh_aas_rec(a,SH_AAS_L_REACHABILITIES,i);
            sh_aas_put_u16(r,6,(uint16_t)(i+1));sh_aas_put_u16(r,8,(uint16_t)(2-i));
            sh_aas_put_i32(r,32,-1);sh_aas_put_i32(r,36,-1);}
    }
    sh_aas_put_i32(sh_aas_rec(a,SH_AAS_L_AREAS,0),20,-1);
    sh_aas_put_i32(sh_aas_rec(a,SH_AAS_L_AREAS,0),24,-1);
    for(i=0;i<3;i++)*sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,i)=7;
    sh_aas_put_f32(sh_aas_rec(a,SH_AAS_L_PLANES,0),0,1);
    sh_aas_put_i32(sh_aas_rec(a,SH_AAS_L_NODES,1),8,-1);
    sh_aas_put_i32(sh_aas_rec(a,SH_AAS_L_NODES,1),12,-2);
    sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_CLUSTERS,1),0,2);
    sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_CLUSTERS,1),4,2);
    sh_aas_put_f32(sh_aas_rec(a,SH_AAS_L_TREES,0),8,1);
    sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_TREES,0),12,1);
    sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_TREES,0),16,1);
    sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_TREES,0),20,3);
    return a;
}
static void validated(sh_aas *a)
{
    char err[192];size_t n;unsigned char *p=sh_aas_write(a,&n);assert(p);
    if(!sh_navmesh_validate_aas(p,n,err,sizeof err)){puts(err);assert(0);}
    HeapFree(GetProcessHeap(),0,p);
}
static void synthetic(void)
{
    sh_grid_size s={SH_GRID_MODERN,{864,272,432}};sh_aas *a=fixture();
    assert(sh_grid_nav_resize(a,&s));validated(a);
    assert(sh_aas_count(a,SH_AAS_L_AREAS)==2);
    assert(sh_aas_count(a,SH_AAS_L_REACHABILITIES)==0);
    assert(sh_aas_count(a,SH_AAS_L_COVER)==2);
    assert(sh_aas_count(a,SH_AAS_L_AREACOVERINDEX)==1);
    assert(sh_aas_get_i32(sh_aas_rec(a,SH_AAS_L_NODES,1),12)==0);
    assert(sh_aas_get_u32(sh_aas_rec(a,SH_AAS_L_CLUSTERS,1),0)==1);
    assert(sh_aas_get_u32(sh_aas_rec(a,SH_AAS_L_TREES,0),20)==2);
    assert(*sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,1)==3);
    assert(sh_aas_get_i16(sh_aas_rec(a,SH_AAS_L_AREABOUNDS,1),0)==-368);
    sh_aas_free(a);
    a=fixture();s.xyz[0]=s.xyz[1]=s.xyz[2]=10000;
    assert(sh_grid_nav_resize(a,&s));validated(a);
    assert(sh_aas_count(a,SH_AAS_L_AREAS)==3);
    assert(sh_aas_get_f32(sh_aas_rec(a,SH_AAS_L_VERTICES,1),0)==-4936);
    assert(sh_aas_get_f32(sh_aas_rec(a,SH_AAS_L_VERTICES,5),0)==-320);
    sh_aas_free(a);
    a=fixture();sh_aas_put_f32(sh_aas_rec(a,SH_AAS_L_PLANES,0),4,.5f);
    assert(!sh_grid_nav_resize(a,&s));sh_aas_free(a);
}
int main(int argc,char **argv)
{
    synthetic();
    if(argc==7){
        FILE *f=fopen(argv[1],"rb");long n;unsigned char *p,*out;size_t length;char err[192];sh_aas *a;
        sh_grid_size s={(sh_grid_kind)atoi(argv[2]),{(unsigned)atoi(argv[3]),(unsigned)atoi(argv[4]),(unsigned)atoi(argv[5])}};
        assert(f);fseek(f,0,SEEK_END);n=ftell(f);rewind(f);assert(n>0&&n<8*1024*1024);
        p=(unsigned char*)malloc((size_t)n);assert(p&&fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);
        a=sh_aas_parse(p,(size_t)n,err,sizeof err);free(p);assert(a);
        if(!sh_grid_nav_resize(a,&s)){puts("resize refused");return 1;}validated(a);
        out=sh_aas_write(a,&length);assert(out);f=fopen(argv[6],"wb");assert(f);
        assert(fwrite(out,1,length,f)==length);fclose(f);HeapFree(GetProcessHeap(),0,out);sh_aas_free(a);
    }
    puts("grid_room_nav_test: passed");return 0;
}
