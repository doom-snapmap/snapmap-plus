#include "../src/backend/grid_room_asset.h"
#include "../src/backend/grid_room_decl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reader {const char *root;unsigned calls,releases;char requested[768];} reader;
static unsigned char *read_source(void *context,const char *name,size_t *length)
{
    reader *r=(reader*)context;FILE *f;char path[2048];long n;unsigned char *data;
    ++r->calls;strcpy(r->requested,name);*length=0;
    if(!r->root)return NULL;
    /* Test extraction stores catalog text under its pindex name. */
    if(!strncmp(name,"generated/decls/snapmoduleinfo/",31)){
        size_t k=strlen(name+31);assert(k>=5);
        snprintf(path,sizeof path,"%s/%.*s",r->root,(int)(k-5),name+31);
    }else if(!strncmp(name,"generated/maps/modules/",23)&&strstr(name,".baas_")){
        const char *extension=strstr(name,".baas_");
        snprintf(path,sizeof path,"%s/%.*s.%s",r->root,(int)(extension-(name+10)),name+10,extension+2);
    }else snprintf(path,sizeof path,"%s/%s",r->root,name);
    f=fopen(path,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);n=ftell(f);rewind(f);assert(n>0&&n<64*1024*1024);
    data=(unsigned char*)malloc((size_t)n);assert(data);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);*length=(size_t)n;return data;
}
static void release(void *context,void *p){++((reader*)context)->releases;free(p);}
static void requests(void)
{
    const char *names[]={
        "maps/modules/smpgrid/v1/m/1536_1024_512.decl",
        "decltree/snapmodule/maps/modules/smpgrid/v1/m/1536_1024_512.decl",
        "decltree/snapModuleInfo/smpgrid/v1/c/512_768_384.decl",
        "generated/decls/snapmoduleinfo/smpgrid/v1/c/512_768_384.decl",
        "generated/decls/snapModuleInfo/smpgrid/v1/c/512_768_384.decl",
        "maps/modules/palettes/mega_blessed/smpgrid/v1/m/1536_1024_512.bmodel",
        "maps/modules/smpgrid/v1/m/1536_1024_512/_combo/world.bcm",
        "maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512_wall_0.bmodel",
        "maps/modules/smpgrid/v1/c/512_768_384/512_768_384_single_surface_walls.bmodel",
        "smpgrid/v1/m/1536_1024_512/maps/modules/ind_dlc/ind_totally_blank_room_4x/volume_flight_2.bcm",
        "maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/light_probe_1_compressed.bimage",
        "maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.aas_monster48",
        "maps/modules/smpgrid/v1/c/512_768_384/512_768_384.aas_monster128",
        "generated/maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.baas_monster48",
        "generated/maps/modules/smpgrid/v1/c/512_768_384/512_768_384.baas_monster96"
    };
    const char *sources[]={
        "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl",
        "generated/decls/snapmoduleinfo/classic/classic_blank_room.decl",
        "generated/decls/snapmoduleinfo/classic/classic_blank_room.decl",
        "generated/decls/snapmoduleinfo/classic/classic_blank_room.decl",
        "maps/modules/palettes/mega_blessed/ind_dlc/ind_totally_blank_room_4x.bmodel",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/_combo/world.bcm",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_wall_0.bmodel",
        "maps/modules/classic/classic_blank_room/classic_blank_room_single_surface_walls.bmodel",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/volume_flight_2.bcm",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage",
        "generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.baas_monster48",
        "generated/maps/modules/classic/classic_blank_room/classic_blank_room.baas_monster128",
        "generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.baas_monster48",
        "generated/maps/modules/classic/classic_blank_room/classic_blank_room.baas_monster96"
    };
    reader r={0};size_t bytes;unsigned char *out;unsigned i;
    for(i=0;i<sizeof names/sizeof names[0];++i){
        unsigned calls=r.calls;assert(sh_grid_asset_open(names[i],read_source,release,&r,&out,&bytes)==-1);
        assert(!out&&!bytes&&r.calls==calls+1);assert(!strcmp(r.requested,sources[i]));
    }
    {const char *bad[]={"smpgrid/v1/m/1536_1024_512/../../door.bcm",
        "smpgrid/v1/m/1536_1024_512/models/snapmaps/door_frames/frame_01.lwo",
        "maps/modules/smpgrid/v1/m/1_1_1.decl","maps/modules/smpgrid/v1/m/1536_1024_512evil.decl",
        "smpgrid/v1/c/512_768_384/maps/modules/other/classic_blank_room_wall_0.bmodel",
        "smpgrid/v1/m/1536_1024_512/maps/modules/palettes/mega/other/ind_totally_blank_room_4x.bmodel",
        "smpgrid/v1/m/1536_1024_512/maps/modules/ind_dlc/ind_totally_blank_room_4x_extra/door.bcm",
        "maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/../../light_probe_1_compressed.bimage",
        "maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/light_probe_2_compressed.bimage",
        "maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.aas_monster48evil",
        "maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.aas_monster999",
        "generated/maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.baas_monster999",
        "generated/maps/modules/smpgrid/v1/m/1536_1024_512/a"};
        for(i=0;i<sizeof bad/sizeof bad[0];++i){unsigned calls=r.calls;
            assert(sh_grid_asset_open(bad[i],read_source,release,&r,&out,&bytes)==-1&&calls==r.calls);}}
    assert(!sh_grid_asset_open("maps/modules/classic/classic_blank_room.decl",read_source,release,&r,&out,&bytes));
}
static int installed(const char *root,const char *classic,const char *modern)
{
    reader r={0};unsigned k,i,passed=0;const char kinds[]={'c','m'};
    const char *sizes[]={classic?classic:"512_768_384",modern?modern:"1536_1024_512"};r.root=root;
    for(k=0;k<2;++k){
        char requests[5][768];
        snprintf(requests[0],768,"maps/modules/smpgrid/v1/%c/%s.decl",kinds[k],sizes[k]);
        snprintf(requests[1],768,"decltree/snapModuleInfo/smpgrid/v1/%c/%s.decl",kinds[k],sizes[k]);
        snprintf(requests[2],768,"maps/modules/palettes/mega_blessed/smpgrid/v1/%c/%s.bmodel",kinds[k],sizes[k]);
        snprintf(requests[3],768,"maps/modules/smpgrid/v1/%c/%s/_combo/world.bcm",kinds[k],sizes[k]);
        snprintf(requests[4],768,"maps/modules/smpgrid/v1/%c/%s/%s_floor_collision.bcm",kinds[k],sizes[k],sizes[k]);
        for(i=0;i<5;++i){unsigned char *data;size_t bytes;
            int status=sh_grid_asset_open(requests[i],read_source,release,&r,&data,&bytes);
            if(status!=1){printf("REFUSED %s (source %s)\n",requests[i],r.requested);return 1;}
            assert(data&&bytes);free(data);++passed;}
        {sh_grid_size size={k==0?SH_GRID_CLASSIC:SH_GRID_MODERN,{16384,16384,16384}};
            sh_grid_builtin_light lights[8];size_t n=0,bytes;unsigned char *source;char name[160];
            snprintf(name,sizeof name,"%s.decl",sh_grid_stock_name(size.kind));
            source=read_source(&r,name,&bytes);assert(source);
            assert(sh_grid_decl_lights(&size,(const char*)source,bytes,lights,8,&n));
            assert(n==(k==0?1u:2u));
            for(i=0;i<n;++i){assert(lights[i].origin[2]>7000&&lights[i].radius[0]>8000);
                if(k==1&&!strcmp(lights[i].inherit,"snapmaps/light/dynamic_point")){
                    assert(lights[i].visible_range>40000&&lights[i].shadow_range>24000);
                    assert(lights[i].origin[2]+lights[i].center[2]>15900);}}
            release(&r,source);
        }
        {const char *suffix[]={"wall_0","wall_1","wall_2","wall_3","single_surface_walls","walls_floor"};
            for(i=0;i<sizeof suffix/sizeof suffix[0];++i){unsigned char *data;size_t bytes;char name[768];
                snprintf(name,sizeof name,"maps/modules/smpgrid/v1/%c/%s/%s_%s.bmodel",kinds[k],sizes[k],sizes[k],suffix[i]);
                if(sh_grid_asset_open(name,read_source,release,&r,&data,&bytes)!=1){printf("REFUSED %s\n",name);return 1;}
                free(data);++passed;
            }
        }
        for(i=2;i<=(k==0?14u:7u);++i){unsigned char *data;size_t bytes;char name[768];
            if(k==0&&i==7)continue;
            snprintf(name,sizeof name,"smpgrid/v1/%c/%s/%s/volume_flight_%u.bcm",kinds[k],sizes[k],
                sh_grid_stock_name(k==0?SH_GRID_CLASSIC:SH_GRID_MODERN),i);
            if(sh_grid_asset_open(name,read_source,release,&r,&data,&bytes)!=1){printf("REFUSED %s\n",name);return 1;}
            free(data);++passed;
        }
        {unsigned agents[3]={48,96,128};
            for(i=0;i<3;i++){unsigned char *data;size_t bytes;char name[768];
                snprintf(name,sizeof name,"generated/maps/modules/smpgrid/v1/%c/%s/%s.baas_monster%u",kinds[k],sizes[k],sizes[k],agents[i]);
                assert(sh_grid_asset_open(name,read_source,release,&r,&data,&bytes)==1);
                assert(bytes>4&&!memcmp(data,"2SAA",4));free(data);++passed;
            }
        }
    }
    assert(r.calls==r.releases);printf("%u installed module, catalog, shell and flight resources transformed\n",passed);return 0;
}
int main(int argc,char **argv)
{requests();if(argc==2||argc==4)return installed(argv[1],argc==4?argv[2]:NULL,argc==4?argv[3]:NULL);puts("grid_room_asset_test: passed");return 0;}
