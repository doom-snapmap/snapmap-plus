#include "grid_room_asset.h"
#include "grid_room_decl.h"
#include "grid_room_nav.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID_ASSET_LIMIT (64u*1024u*1024u)
enum { ASSET_MODULE=1,ASSET_INFO,ASSET_GEOMETRY,ASSET_COPY,ASSET_NAV };

static int starts(const char *s,const char *prefix)
{return !strncmp(s,prefix,strlen(prefix));}
static int safe_path(const char *s)
{
    const char *begin=s;
    if(!s||!*s||strlen(s)>700)return 0;
    for(;*s;++s){
        if(*s=='/'){
            if(s==begin||(s-begin==1&&begin[0]=='.')||
               (s-begin==2&&begin[0]=='.'&&begin[1]=='.'))return 0;
            begin=s+1;
        }else if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.'))return 0;
    }
    return s!=begin&&strcmp(begin,".")&&strcmp(begin,"..");
}
/* Parse a dimension-bearing directory or basename without accepting trailing
 * garbage through the saved module-name parser. */
static const char *dimensions(const char *p,sh_grid_size *size)
{
    const char *end=p;char name[112];size_t n;
    if(!starts(p,"v1/")||(p[3]!='c'&&p[3]!='m')||p[4]!='/')return NULL;
    end=p+5;while((*end>='0'&&*end<='9')||*end=='_')++end;
    n=(size_t)(end-p);
    if(n>70)return NULL;
    memcpy(name,"maps/modules/smpgrid/",21);memcpy(name+21,p,n);name[21+n]=0;
    return sh_grid_parse_name(name,size)?end:NULL;
}
static int request(const char *name,sh_grid_size *size,char source[768],int *type)
{
    const char *p,*end,*stock;char check[768];
    if(starts(name,"smpgrid/")){
        if(!safe_path(name)||(end=dimensions(name+8,size))==NULL||*end!='/')return -1;
        stock=end+1;
        if(!sh_grid_resource_name(size,stock,check,sizeof check)||strcmp(check,name))return -1;
        memcpy(source,stock,strlen(stock)+1);*type=ASSET_GEOMETRY;return 1;
    }
    p=name;
    if(starts(p,"generated/maps/modules/smpgrid/"))p+=10;
    if(starts(p,"decltree/snapmodule/"))p+=20;
    else if(starts(p,"decltree/snapModule/"))p+=20;
    if(starts(p,"maps/modules/smpgrid/")){
        end=dimensions(p+21,size);if(!end)return -1;
        stock=sh_grid_stock_name(size->kind);
        if(!strcmp(end,".decl")){snprintf(source,768,"%s.decl",stock);*type=ASSET_MODULE;return 1;}
        if(*end=='/'){
            const char *leaf=p+26;
            size_t length=(size_t)(end-leaf);
            if(strlen(end+1)>=length&&!strncmp(end+1,leaf,length)){
                const char *ext=end+1+length;
                const char *kind=starts(ext,".baas_")?ext+6:starts(ext,".aas_")?ext+5:NULL;
                if(kind&&
               (!strcmp(kind,"monster48")||!strcmp(kind,"monster96")||!strcmp(kind,"monster128"))){
                snprintf(source,768,"generated/%s/%s.baas_%s",stock,strrchr(stock,'/')+1,kind);
                *type=ASSET_NAV;return 1;
                }
            }
        }
        /* The modern room's compressed probe is requested relative to its
         * private module directory. Preserve the installed image bytes. */
        if(size->kind==SH_GRID_MODERN&&!strcmp(end,"/lightprobes/light_probe_1_compressed.bimage")){
            snprintf(source,768,"%s%s",stock,end);*type=ASSET_COPY;return 1;
        }
        /* Collision and auxiliary geometry use the module's implicit path. */
        if(*end=='/'&&safe_path(name)){
            const char *leaf=p+26,*stock_leaf=strrchr(stock,'/')+1;
            size_t leaf_len=(size_t)(end-leaf);
            /* Native Blueprint geometry derives both the directory and the
             * basename from snapModuleInfo's private name. Translate both
             * before reading the installed source. */
            if(!strncmp(end+1,leaf,leaf_len)&&end[1+leaf_len]=='_')
                snprintf(source,768,"%s/%s%s",stock,stock_leaf,end+1+leaf_len);
            else snprintf(source,768,"%s%s",stock,end);
            if(!sh_grid_resource_name(size,source,check,sizeof check))return -1;
            *type=ASSET_GEOMETRY;return 1;
        }
        return -1;
    }
    p=name;
    if(starts(p,"decltree/snapmoduleinfo/"))p+=24;
    else if(starts(p,"decltree/snapModuleInfo/"))p+=24;
    else if(starts(p,"generated/decls/snapmoduleinfo/"))p+=31;
    else if(starts(p,"generated/decls/snapModuleInfo/"))p+=31;
    if(starts(p,"smpgrid/")){
        end=dimensions(p+8,size);if(!end||strcmp(end,".decl"))return -1;
        snprintf(source,768,"generated/decls/snapmoduleinfo/%s.decl",sh_grid_stock_name(size->kind)+13);
        *type=ASSET_INFO;return 1;
    }
    if(starts(name,"maps/modules/palettes/")){
        const char *palette=name+22;size_t n;
        p=strchr(palette,'/');if(!p||!starts(p+1,"smpgrid/"))return 0;
        if(!safe_path(name)||(end=dimensions(p+9,size))==NULL||strcmp(end,".bmodel"))return -1;
        n=(size_t)(p-name)+1;if(n>200)return -1;
        memcpy(source,name,n);snprintf(source+n,768-n,"%s.bmodel",sh_grid_stock_name(size->kind)+13);
        *type=ASSET_GEOMETRY;return 1;
    }
    return 0;
}

int sh_grid_asset_open(const char *name,sh_grid_asset_read_fn read,
                        sh_grid_asset_release_fn release,void *context,
                        unsigned char **out,size_t *length)
{
    sh_grid_size size;sh_grid_warp warp;char source[768],module[128];
    unsigned char *input=NULL,*result=NULL,*decl=NULL;size_t bytes=0,decl_bytes=0,result_bytes=0;
    int type=0,claimed,ok=0;
    if(out)*out=NULL;if(length)*length=0;
    if(!name||!out||!length)return 0;
    claimed=request(name,&size,source,&type);if(claimed<=0)return claimed;
    if(!read||!release)return -1;
    input=read(context,source,&bytes);
    if(!input||!bytes||bytes>GRID_ASSET_LIMIT)goto done;
    if(type==ASSET_MODULE||type==ASSET_INFO){
        result=(unsigned char*)sh_grid_decl(&size,(const char*)input,bytes,type==ASSET_INFO,&result_bytes);
        ok=result!=NULL;
    }else if(type==ASSET_NAV){
        char error[192];sh_aas *aas=sh_aas_parse(input,bytes,error,sizeof error);
        unsigned char *encoded=NULL;
        if(aas&&sh_grid_nav_resize(aas,&size))encoded=sh_aas_write(aas,&result_bytes);
        sh_aas_free(aas);
        if(encoded){result=(unsigned char*)malloc(result_bytes);
            if(result){memcpy(result,encoded,result_bytes);ok=1;}
            HeapFree(GetProcessHeap(),0,encoded);}
    }else if(type==ASSET_COPY){
        result=(unsigned char*)malloc(bytes);
        if(result){memcpy(result,input,bytes);result_bytes=bytes;ok=1;}
    }else{
        if(!sh_grid_warp_init(&size,&warp))goto done;
        if(strstr(source,"/volume_flight_")){
            snprintf(module,sizeof module,"%s.decl",sh_grid_stock_name(size.kind));
            decl=read(context,module,&decl_bytes);
            if(!decl||!sh_grid_flight_warp(&size,(const char*)decl,decl_bytes,source,&warp))goto done;
        }
        result=(unsigned char*)malloc(bytes);if(!result)goto done;
        ok=bytes>=4&&!memcmp(input,"BCM8",4)?sh_grid_bcm(&warp,input,bytes,result):
                                                   sh_grid_bmodel(&warp,input,bytes,result);
        result_bytes=bytes;
    }
done:
    if(input)release(context,input);if(decl)release(context,decl);
    if(!ok){free(result);return -1;}
    *out=result;*length=result_bytes;return 1;
}
