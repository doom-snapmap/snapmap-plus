/* Keep the agent box and door clearance fixed while resizing the room floor.
 * BuildAAS still performs the native placement and cross-module merge. */
#include "grid_room_nav.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct nav_axis {unsigned n;float s[8],t[8];} nav_axis;
static float coord(const nav_axis *a,float v)
{
    unsigned j;
    if(v<a->s[0])return v+a->t[0]-a->s[0];
    for(j=1;j<a->n;j++)if(v<a->s[j])
        return a->t[j-1]+(v-a->s[j-1])*(a->t[j]-a->t[j-1])/(a->s[j]-a->s[j-1]);
    return v+a->t[a->n-1]-a->s[a->n-1];
}
static int axes(const sh_grid_size *size,float r,nav_axis a[3])
{
    sh_grid_warp shell;unsigned j,k;
    float x=(float)size->xyz[0],y=(float)size->xyz[1];
    if(!sh_grid_warp_init(size,&shell)||(r!=24&&r!=48&&r!=64))return 0;
    memset(a,0,3*sizeof *a);
    if(size->kind==SH_GRID_CLASSIC){
        for(j=0;j<2;j++){
            float h=(j?y:x)/2;
            nav_axis v={6,{-1280-r,-1248+r,-128+r,128-r,1248-r,1280+r},
                {-h-r,-h+32+r,-128+r,128-r,h-32-r,h+r}};
            a[j]=v;
        }
    }else{
        float lo=1280-y/2,hi=1280+y/2;
        nav_axis ax={8,{-2592-r,-2560+r,-256-r,-192+r,192-r,256+r,2560-r,2592+r},
            {-x/2-32-r,-x/2+r,-256-r,-192+r,192-r,256+r,x/2-r,x/2+32+r}};
        nav_axis ay={8,{-1280-r,-1272+r,-1248+r,-1184+r,3744-r,3808-r,3832-r,3840+r},
            {lo-r,lo+8+r,lo+32+r,fminf(lo+96+r,1280),fmaxf(hi-96-r,1280),hi-32-r,hi-8-r,hi+r}};
        a[0]=ax;a[1]=ay;
    }
    a[2].n=shell.axis[2].count;
    for(k=0;k<a[2].n;k++){a[2].s[k]=shell.axis[2].source[k];a[2].t[k]=shell.axis[2].target[k];}
    for(j=0;j<3;j++)for(k=1;k<a[j].n;k++)
        if(a[j].s[k]<=a[j].s[k-1]||a[j].t[k]<a[j].t[k-1])return 0;
    return 1;
}
/* Native AAS quantization rounds to nearest, ties to even. */
static int quant(float v,int16_t *out)
{
    float base=floorf(v),frac=v-base;
    if(!isfinite(v)||v<-32768||v>32767)return 0;
    if(frac>.5f||(frac==.5f&&((int)base&1)))base++;
    *out=(int16_t)base;return 1;
}
static int position(unsigned char *p,const nav_axis a[3],int shorts)
{
    unsigned k;
    for(k=0;k<3;k++){
        float v=shorts?(float)sh_aas_get_i16(p,k*2):sh_aas_get_f32(p,k*4);
        if(!isfinite(v))return 0;v=coord(a+k,v);
        if(shorts){int16_t q;if(!quant(v,&q))return 0;sh_aas_put_i16(p,k*2,q);}
        else {if(!isfinite(v))return 0;sh_aas_put_f32(p,k*4,v);}
    }
    return 1;
}
/* The supported modules have one tree and no inter-cluster portals or
 * traversal records. Compact vanished narrow strips, all area references,
 * cover membership, visibility rows and native route chains together. */
static int compact(sh_aas *a,const unsigned *remap,unsigned old,unsigned count)
{
    unsigned i,j,k,nc=sh_aas_count(a,SH_AAS_L_COVER),out=1,first;
    unsigned *covers=NULL,*cluster_count=NULL;
    unsigned char *pvs=NULL,*visible=NULL;
    unsigned pvs_bytes=sh_aas_count(a,SH_AAS_L_OBSTACLEPVS),used=0;
    unsigned stride=(count+6)/7;
    int ok=0;
    if(count==old)return 1;
    covers=(unsigned*)calloc(nc,sizeof *covers);
    cluster_count=(unsigned*)calloc(sh_aas_count(a,SH_AAS_L_CLUSTERS),sizeof *cluster_count);
    pvs=(unsigned char*)malloc((size_t)count*stride);
    visible=(unsigned char*)malloc(old);
    if(!covers||!cluster_count||!pvs||!visible)goto done;
    for(i=0;i<old;i++)if(!i||remap[i]){
        unsigned char *ar=sh_aas_rec(a,SH_AAS_L_AREAS,i);
        unsigned at=sh_aas_get_u32(ar,16),pos=0,offset=used;
        memset(visible,0,old);
        while(pos<old){
            unsigned token,run;
            if(at>=pvs_bytes)goto done;
            token=*sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,at++);
            if(token&128){
                run=token&63;
                if(token&64){if(at>=pvs_bytes)goto done;run|=(unsigned)*sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,at++)<<6;}
                pos+=run+1;
            }else for(k=0;k<7&&pos<old;k++,pos++)visible[pos]=(unsigned char)((token>>k)&1);
        }
        memset(pvs+used,0,stride);
        if(i)visible[i]=1;
        for(j=0;j<old;j++)if((!j||remap[j])&&visible[j])
            pvs[used+remap[j]/7]|=(unsigned char)(1u<<(remap[j]%7));
        used+=stride;sh_aas_put_u32(ar,16,offset);
    }
    if(!sh_aas_truncate(a,SH_AAS_L_OBSTACLEPVS,0)||!sh_aas_append(a,SH_AAS_L_OBSTACLEPVS,used,&first))goto done;
    memcpy(sh_aas_rec(a,SH_AAS_L_OBSTACLEPVS,0),pvs,used);
    for(i=1;i<nc;i++){
        unsigned char *c=sh_aas_rec(a,SH_AAS_L_COVER,i);
        unsigned area=sh_aas_get_u16(c,30);
        if(area>=old||sh_aas_get_u32(c,36)||sh_aas_get_u32(c,40))goto done;
        if(!remap[area])continue;
        covers[i]=out;sh_aas_put_u16(c,30,(uint16_t)remap[area]);
        memmove(sh_aas_rec(a,SH_AAS_L_COVER,out++),c,56);
    }
    if(!sh_aas_truncate(a,SH_AAS_L_COVER,out))goto done;
    out=0;
    for(i=0;i<old;i++)if(!i||remap[i]){
        unsigned char *ar=sh_aas_rec(a,SH_AAS_L_AREAS,i);
        unsigned begin=sh_aas_get_u16(ar,32),n=sh_aas_get_u16(ar,34),start=out;
        for(j=0;j<n;j++){
            unsigned char *p=sh_aas_rec(a,SH_AAS_L_AREACOVERINDEX,begin+j);
            unsigned c;if(!p)goto done;c=sh_aas_get_u32(p,0);if(c>=nc)goto done;
            if(covers[c])sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_AREACOVERINDEX,out++),0,covers[c]);
        }
        sh_aas_put_u16(ar,32,(uint16_t)start);sh_aas_put_u16(ar,34,(uint16_t)(out-start));
    }
    if(!sh_aas_truncate(a,SH_AAS_L_AREACOVERINDEX,out))goto done;
    out=0;
    for(i=0;i<old;i++)if(!i||remap[i]){
        unsigned char *ar=sh_aas_rec(a,SH_AAS_L_AREAS,i);
        unsigned begin=sh_aas_get_u32(ar,8),edges=sh_aas_get_u16(ar,6);
        /* Installed area edge spans are ordered and disjoint. */
        if(begin<out||begin+edges>sh_aas_count(a,SH_AAS_L_EDGEINDEX))goto done;
        if(edges)memmove(sh_aas_rec(a,SH_AAS_L_EDGEINDEX,out),sh_aas_rec(a,SH_AAS_L_EDGEINDEX,begin),edges*4);
        sh_aas_put_u32(ar,8,out);out+=edges;
    }
    if(!sh_aas_truncate(a,SH_AAS_L_EDGEINDEX,out))goto done;
    for(i=0;i<old;i++)if(!i||remap[i]){
        unsigned char *ar=sh_aas_rec(a,SH_AAS_L_AREAS,i);
        unsigned cluster=sh_aas_get_u16(ar,12);
        if(cluster>=sh_aas_count(a,SH_AAS_L_CLUSTERS))goto done;
        if(i)sh_aas_put_u16(ar,14,(uint16_t)cluster_count[cluster]++);
        sh_aas_put_i32(ar,20,-1);sh_aas_put_i32(ar,24,-1);
        memmove(sh_aas_rec(a,SH_AAS_L_AREAS,remap[i]),ar,44);
        memmove(sh_aas_rec(a,SH_AAS_L_AREABOUNDS,remap[i]),sh_aas_rec(a,SH_AAS_L_AREABOUNDS,i),12);
    }
    if(!sh_aas_truncate(a,SH_AAS_L_AREAS,count)||!sh_aas_truncate(a,SH_AAS_L_AREABOUNDS,count))goto done;
    out=0;
    for(i=0;i<sh_aas_count(a,SH_AAS_L_REACHABILITIES);i++){
        unsigned char *r=sh_aas_rec(a,SH_AAS_L_REACHABILITIES,i),*from,*to;
        unsigned f=sh_aas_get_u16(r,6),t=sh_aas_get_u16(r,8);
        if(f>=old||t>=old)goto done;
        if(!remap[f]||!remap[t])continue;
        f=remap[f];t=remap[t];from=sh_aas_rec(a,SH_AAS_L_AREAS,f);to=sh_aas_rec(a,SH_AAS_L_AREAS,t);
        sh_aas_put_u16(r,6,(uint16_t)f);sh_aas_put_u16(r,8,(uint16_t)t);
        sh_aas_put_i32(r,32,sh_aas_get_i32(from,20));sh_aas_put_i32(r,36,sh_aas_get_i32(to,24));
        sh_aas_put_i32(from,20,(int32_t)out);sh_aas_put_i32(to,24,(int32_t)out);
        memmove(sh_aas_rec(a,SH_AAS_L_REACHABILITIES,out++),r,40);
    }
    if(!sh_aas_truncate(a,SH_AAS_L_REACHABILITIES,out))goto done;
    for(i=0;i<sh_aas_count(a,SH_AAS_L_NODES);i++)for(j=8;j<=12;j+=4){
        unsigned char *p=sh_aas_rec(a,SH_AAS_L_NODES,i);int v=sh_aas_get_i32(p,j);
        if(v<0){if(v<=-(int)old)goto done;sh_aas_put_i32(p,j,-(int32_t)remap[-v]);}
    }
    for(i=1;i<sh_aas_count(a,SH_AAS_L_CLUSTERS);i++){
        unsigned char *p=sh_aas_rec(a,SH_AAS_L_CLUSTERS,i);
        sh_aas_put_u32(p,0,cluster_count[i]);sh_aas_put_u32(p,4,cluster_count[i]);
    }
    sh_aas_put_u32(sh_aas_rec(a,SH_AAS_L_TREES,0),20,count);
    ok=1;
done:free(covers);free(cluster_count);free(pvs);free(visible);return ok;
}

int sh_grid_nav_resize(sh_aas *a,const sh_grid_size *size)
{
    nav_axis axis[3];sh_grid_warp shell;float mins[3],maxs[3];
    unsigned i,j,k,n,count=1,*remap=NULL;int ok=0;
    if(!a||!size||!sh_grid_warp_init(size,&shell))return 0;
    sh_aas_agent_bounds(a,mins,maxs);
    if(mins[0]!=-maxs[0]||mins[1]!=-maxs[0]||maxs[1]!=maxs[0]||mins[2]!=0||
       !axes(size,maxs[0],axis))return 0;
    n=sh_aas_count(a,SH_AAS_L_AREAS);
    if(n<2||n>512||sh_aas_count(a,SH_AAS_L_PORTALS)!=1||sh_aas_count(a,SH_AAS_L_PORTALINDEX)||
       sh_aas_count(a,SH_AAS_L_TRAVERSALPOINTS)!=1||sh_aas_count(a,SH_AAS_L_HINTNODES)!=1||
       sh_aas_count(a,SH_AAS_L_TOUCHINGCOVERINDEX)||sh_aas_count(a,SH_AAS_L_TREES)!=1)return 0;
    remap=(unsigned*)calloc(n,sizeof *remap);if(!remap)return 0;
    for(i=0;i<sh_aas_count(a,SH_AAS_L_PLANES);i++){
        unsigned char *p=sh_aas_rec(a,SH_AAS_L_PLANES,i);int active=-1;float normal=0,dist=sh_aas_get_f32(p,12);
        if(!isfinite(dist))goto done;
        for(j=0;j<3;j++){
            float v=sh_aas_get_f32(p,j*4);if(!isfinite(v))goto done;
            if(fabsf(v)>1e-5f){if(active>=0)goto done;active=(int)j;normal=v;}
        }
        if(active>=0){
            for(j=0;j<3;j++)if(j!=(unsigned)active)sh_aas_put_f32(p,j*4,0);
            sh_aas_put_f32(p,12,-coord(axis+active,-dist/normal)*normal);
        }
    }
    for(i=0;i<sh_aas_count(a,SH_AAS_L_VERTICES);i++)
        if(!position(sh_aas_rec(a,SH_AAS_L_VERTICES,i),axis,0))goto done;
    for(i=0;i<sh_aas_count(a,SH_AAS_L_REACHABILITIES);i++){
        unsigned char *r=sh_aas_rec(a,SH_AAS_L_REACHABILITIES,i);
        float before=0,after=0;unsigned travel=sh_aas_get_u16(r,4);
        int16_t old[6];for(j=0;j<6;j++)old[j]=sh_aas_get_i16(r,12+j*2);
        if(!position(r+12,axis,1)||!position(r+18,axis,1))goto done;
        for(j=0;j<3;j++){
            float b=(float)old[j]-old[j+3],d=(float)sh_aas_get_i16(r,12+j*2)-sh_aas_get_i16(r,18+j*2);
            before+=b*b;after+=d*d;
        }
        if(before>.01f){float cost=ceilf(travel*sqrtf(after/before));
            sh_aas_put_u16(r,4,(uint16_t)fminf(65535,fmaxf(1,cost)));}
    }
    for(i=1;i<sh_aas_count(a,SH_AAS_L_COVER);i++){
        unsigned char *p=sh_aas_rec(a,SH_AAS_L_COVER,i);
        /* Cover is a world point/direction, not an agent inset boundary.
         * These blank-room markers retain their radius and direction. */
        for(j=0;j<3;j++){
            float v=sh_aas_get_f32(p,j*4);if(!isfinite(v))goto done;
            sh_aas_put_f32(p,j*4,sh_grid_coordinate(&shell,j,v));
        }
    }
    for(i=1;i<n;i++){
        unsigned char *ar=sh_aas_rec(a,SH_AAS_L_AREAS,i),*b=sh_aas_rec(a,SH_AAS_L_AREABOUNDS,i);
        unsigned edges=sh_aas_get_u16(ar,6),first=sh_aas_get_u32(ar,8);
        float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
        if(!b||edges<3)goto done;
        for(j=0;j<edges;j++){
            const unsigned char *ei=sh_aas_rec_const(a,SH_AAS_L_EDGEINDEX,first+j),*edge;int e;
            if(!ei)goto done;e=sh_aas_get_i32(ei,0);if(e==INT32_MIN)goto done;
            edge=sh_aas_rec_const(a,SH_AAS_L_EDGES,(unsigned)abs(e));if(!edge)goto done;
            for(k=0;k<2;k++){
                const unsigned char *v=sh_aas_rec_const(a,SH_AAS_L_VERTICES,sh_aas_get_u32(edge,k*4));unsigned d;
                if(!v)goto done;
                for(d=0;d<3;d++){float f=sh_aas_get_f32(v,d*4);lo[d]=fminf(lo[d],f);hi[d]=fmaxf(hi[d],f);}
            }
        }
        for(j=0;j<3;j++){int16_t l,h;if(!quant(lo[j],&l)||!quant(hi[j],&h))goto done;
            sh_aas_put_i16(b,j*2,l);sh_aas_put_i16(b,6+j*2,h);}
        if(hi[0]-lo[0]>.01f&&hi[1]-lo[1]>.01f)remap[i]=count++;
    }
    ok=compact(a,remap,n,count);
done:free(remap);return ok;
}
