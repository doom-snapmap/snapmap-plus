/* nav_geometry.c -- convex half-space geometry in module-local coordinates. */
#include "nav_geometry.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NG_EPS 0.001
#define NG_MAX SH_AUG_MAX_CORNERS
typedef struct ng_poly { int n; double p[NG_MAX][3]; } ng_poly;
typedef struct ng_box { double c[3], axis[3][3], half[3]; int solid; } ng_box;
typedef struct ng_face { ng_poly p; double normal[3]; int source, face; } ng_face;
typedef struct ng_interval { double lo, hi; } ng_interval;

static double ng_dot(const double *a, const double *b)
{ return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static double ng_area(const ng_poly *p)
{
    double a = 0.0; int i;
    for (i = 0; i < p->n; i++) {
        int j = (i+1)%p->n;
        a += p->p[i][0]*p->p[j][1] - p->p[j][0]*p->p[i][1];
    }
    return fabs(a)*0.5;
}

static int ng_append(ng_poly *p, const double *v)
{
    int k;
    if (p->n && fabs(v[0]-p->p[p->n-1][0]) < 1e-7 &&
        fabs(v[1]-p->p[p->n-1][1]) < 1e-7) return 1;
    if (p->n == NG_MAX) return 0;
    for (k=0;k<3;k++) p->p[p->n][k]=v[k];
    p->n++; return 1;
}

/* Keep n.p <= d. The same signed distance drives classification and
 * interpolation: no snapping a crossing to an unrelated bounding box. */
static int ng_clip(const ng_poly *p, const double *normal, double d, ng_poly *out)
{
    int i,k; out->n=0;
    for (i=0;i<p->n;i++) {
        int j=(i+1)%p->n;
        double a=ng_dot(normal,p->p[i])-d, b=ng_dot(normal,p->p[j])-d;
        if (a<=0.0 && !ng_append(out,p->p[i])) return 0;
        if ((a<0.0 && b>0.0)||(a>0.0 && b<0.0)) {
            double v[3], t=a/(a-b);
            for(k=0;k<3;k++) v[k]=p->p[i][k]+t*(p->p[j][k]-p->p[i][k]);
            if(!ng_append(out,v)) return 0;
        }
    }
    if(out->n>1 && fabs(out->p[0][0]-out->p[out->n-1][0])<1e-7 &&
        fabs(out->p[0][1]-out->p[out->n-1][1])<1e-7) out->n--;
    if(out->n<3 || ng_area(out)<1e-6) out->n=0;
    return 1;
}

/* Subtract a convex clockwise cutter. Emitted pieces have disjoint interiors;
 * the remainder is passed to the next half-space, not emitted a second time. */
static int ng_subtract(const ng_poly *p, const ng_poly *cut,
                       ng_poly *out, int *count, int cap)
{
    ng_poly rest=*p, inside, outside; int i;
    if(!cut->n) { if(*count==cap)return 0; out[(*count)++]=*p; return 1; }
    {
        int separated=0,j;
        for(i=0;i<cut->n&&!separated;i++) {
            int next=(i+1)%cut->n,inside_any=0;
            double nx=cut->p[i][1]-cut->p[next][1],ny=cut->p[next][0]-cut->p[i][0];
            for(j=0;j<p->n;j++)if(nx*(p->p[j][0]-cut->p[i][0])+ny*(p->p[j][1]-cut->p[i][1])< -1e-8){inside_any=1;break;}
            if(!inside_any)separated=1;
        }
        if(separated){if(*count==cap)return 0;out[(*count)++]=*p;return 1;}
    }
    for(i=0;i<cut->n && rest.n;i++) {
        int j=(i+1)%cut->n;
        double normal[3]={cut->p[i][1]-cut->p[j][1],
                          cut->p[j][0]-cut->p[i][0],0.0};
        double d=ng_dot(normal,cut->p[i]), neg[3]={-normal[0],-normal[1],0.0};
        if(!ng_clip(&rest,normal,d,&inside) || !ng_clip(&rest,neg,-d,&outside))return 0;
        if(outside.n) {if(*count==cap)return 0;out[(*count)++]=outside;}
        rest=inside;
    }
    return 1;
}

static int ng_cut_all(ng_poly **a, ng_poly **b, int *count, const ng_poly *cut,int cap)
{
    int i,n=0; ng_poly *tmp;
    for(i=0;i<*count;i++) if(!ng_subtract(&(*a)[i],cut,*b,&n,cap))return 0;
    tmp=*a;*a=*b;*b=tmp;*count=n;return 1;
}

static int ng_box_read(const sh_aug_platform *p, ng_box *b)
{
    int k,a; double len;
    memset(b,0,sizeof *b);
    for(k=0;k<3;k++) {
        b->axis[0][k]=p->c[1][k]-p->c[0][k];
        b->axis[1][k]=p->c[3][k]-p->c[0][k];
        b->axis[2][k]=p->n[k];
        b->c[k]=(p->c[0][k]+p->c[2][k])*0.5-p->n[k]*p->depth*0.5;
        if(!isfinite(b->c[k]))return 0;
    }
    for(a=0;a<3;a++) {
        len=sqrt(ng_dot(b->axis[a],b->axis[a]));
        if(!isfinite(len)||len<1e-6)return 0;
        b->half[a]=len*0.5;
        for(k=0;k<3;k++)b->axis[a][k]/=len;
    }
    b->half[2]=p->depth*0.5;
    if(p->depth<0.0f || !isfinite(p->depth))return 0;
    for(a=0;a<3;a++) if(fabs(ng_dot(b->axis[a],b->axis[(a+1)%3]))>0.001)return 0;
    b->solid=p->depth>0.0f; return 1;
}

static void ng_make_face(const ng_box *b,int axis,int sign,ng_face *f)
{
    static const int su[4]={-1,1,1,-1},sv[4]={-1,-1,1,1};
    int u=(axis+1)%3,v=(axis+2)%3,i,k; double area=0.0;
    f->p.n=4;
    for(k=0;k<3;k++)f->normal[k]=sign*b->axis[axis][k];
    for(i=0;i<4;i++)for(k=0;k<3;k++)f->p.p[i][k]=b->c[k]+
        sign*b->half[axis]*b->axis[axis][k]+
        su[i]*b->half[u]*b->axis[u][k]+sv[i]*b->half[v]*b->axis[v][k];
    for(i=0;i<4;i++)area+=f->p.p[i][0]*f->p.p[(i+1)%4][1]-f->p.p[(i+1)%4][0]*f->p.p[i][1];
    if(area>0.0)for(k=0;k<3;k++){double t=f->p.p[1][k];f->p.p[1][k]=f->p.p[3][k];f->p.p[3][k]=t;}
}

static int ng_coplanar(const ng_face *a,const ng_face *b)
{
    double d[3]; int k;
    if(ng_dot(a->normal,b->normal)<1.0-1e-8)return 0;
    for(k=0;k<3;k++)d[k]=a->p.p[0][k]-b->p.p[0][k];
    return fabs(ng_dot(a->normal,d))<NG_EPS;
}

int sh_nav_geometry_path_clear(const sh_aug_platform *src,int count,
    int skip_a,int skip_b,const double start[3],const double end[3],
    double radius,double height)
{
    int i,a,k,j;
    for(i=0;i<count;i++) {
        ng_box b; double axes[15][3]={{0}},lo=0.0,hi=1.0,centre[3];
        if(i==skip_a||i==skip_b||src[i].depth==0.0f)continue;
        if(!ng_box_read(&src[i],&b))return 0;
        for(k=0;k<3;k++){axes[k+3][k]=1.0;centre[k]=b.c[k];}
        centre[2]-=height*0.5;
        for(a=0;a<3;a++)for(k=0;k<3;k++)axes[a][k]=b.axis[a][k];
        for(a=0;a<3;a++)for(k=0;k<3;k++){
            double *v=axes[6+a*3+k];
            v[(k+1)%3]=b.axis[a][(k+2)%3];
            v[(k+2)%3]=-b.axis[a][(k+1)%3];
        }
        for(a=0;a<15&&lo<hi;a++) {
            double len=sqrt(ng_dot(axes[a],axes[a])),rad,speed,offset,l,h;
            if(len<1e-8)continue;
            for(k=0;k<3;k++)axes[a][k]/=len;
            rad=radius*(fabs(axes[a][0])+fabs(axes[a][1]))+
                height*0.5*fabs(axes[a][2]);
            for(j=0;j<3;j++)rad+=b.half[j]*fabs(ng_dot(axes[a],b.axis[j]));
            rad-=NG_EPS; /* Mere contact does not intersect the open solid. */
            offset=ng_dot(axes[a],start)-ng_dot(axes[a],centre);
            speed=ng_dot(axes[a],end)-ng_dot(axes[a],start);
            if(fabs(speed)<1e-12){if(fabs(offset)>=rad){lo=1.0;hi=0.0;}continue;}
            l=(-rad-offset)/speed;h=(rad-offset)/speed;
            if(l>h){double t=l;l=h;h=t;}
            if(l>lo)lo=l;if(h<hi)hi=h;
        }
        if(lo<hi)return 0;
    }
    return 1;
}

/* Intersect a face with the occupied standing space. Start with the exact
 * OBB/agent Minkowski half-spaces (three box axes, three world axes and nine
 * edge cross products), then retain the physical floor-contact height on
 * marked walkable planes so a ramp can meet a level support continuously. */
static int ng_obstacle(const ng_face *f,const ng_box *b,double r,double h,
                       double floor_cos,int support,ng_poly *out)
{
    double axes[15][3]={{0}},centre[3];ng_poly p=f->p,q;int a,k,j,s;
    for(k=0;k<3;k++){axes[k+3][k]=1.0;centre[k]=b->c[k];}
    centre[2]-=h*0.5;
    for(a=0;a<3;a++)for(k=0;k<3;k++)axes[a][k]=b->axis[a][k];
    for(a=0;a<3;a++)for(k=0;k<3;k++){
        double *v=axes[6+a*3+k];
        v[(k+1)%3]=b->axis[a][(k+2)%3];
        v[(k+2)%3]=-b->axis[a][(k+1)%3];
    }
    for(a=0;a<15 && p.n;a++) {
        double len=sqrt(ng_dot(axes[a],axes[a])),rad;
        if(len<1e-8)continue;
        for(k=0;k<3;k++)axes[a][k]/=len;
        rad=r*(fabs(axes[a][0])+fabs(axes[a][1]))+h*0.5*fabs(axes[a][2]);
        for(j=0;j<3;j++)rad+=b->half[j]*fabs(ng_dot(axes[a],b->axis[j]));
        for(s=-1;s<=1;s+=2) {
            double norm[3],d,minimum=1e30;
            for(k=0;k<3;k++)norm[k]=s*axes[a][k];
            d=ng_dot(norm,centre)+rad;
            /* A walkable support plane is a floor contact, not a wall for the
             * agent's lower corners. Keep its true height when joining ramps.
             * Side walls and downward ceiling faces retain full clearance.
             * Without this distinction, every rising ramp cuts an artificial
             * agent-radius gap out of its adjoining level floor. */
            if(support && a<3 && norm[2]+1e-7>=floor_cos)
                d-=r*(fabs(norm[0])+fabs(norm[1]));
            for(j=0;j<p.n;j++){double v=ng_dot(norm,p.p[j])-d;if(v<minimum)minimum=v;}
            /* Face contact is support, not an occupied open volume. */
            if(minimum>=-NG_EPS){out->n=0;return 1;}
            if(!ng_clip(&p,norm,d,&q))return 0;
            p=q;
        }
    }
    *out=p;return 1;
}

static double ng_cross2(const double *a,const double *b,const double *c)
{return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);}

/* Convex hull of the exposed segment swept by the agent's XY footprint. */
static void ng_boundary_cut(const ng_face *f,const double *a,const double *b,
                            double radius,ng_poly *out)
{
    double pts[8][3],hull[16][3];int i,j,k,n=0,t;
    for(i=0;i<8;i++) {
        pts[i][0]=(i<4?a[0]:b[0])+((i&1)?radius:-radius);
        pts[i][1]=(i<4?a[1]:b[1])+((i&2)?radius:-radius);
        pts[i][2]=f->p.p[0][2]-(f->normal[0]*(pts[i][0]-f->p.p[0][0])+
                          f->normal[1]*(pts[i][1]-f->p.p[0][1]))/f->normal[2];
    }
    for(i=1;i<8;i++)for(j=i;j>0&&(pts[j][0]<pts[j-1][0]||
        (pts[j][0]==pts[j-1][0]&&pts[j][1]<pts[j-1][1]));j--)
        for(k=0;k<3;k++){double v=pts[j][k];pts[j][k]=pts[j-1][k];pts[j-1][k]=v;}
    for(i=0;i<8;i++){while(n>=2&&ng_cross2(hull[n-2],hull[n-1],pts[i])<=0.0)n--;memcpy(hull[n++],pts[i],sizeof pts[i]);}
    for(i=6,t=n+1;i>=0;i--){while(n>=t&&ng_cross2(hull[n-2],hull[n-1],pts[i])<=0.0)n--;memcpy(hull[n++],pts[i],sizeof pts[i]);}
    out->n=n-1;
    for(i=0;i<out->n;i++)memcpy(out->p[i],hull[out->n-1-i],sizeof hull[0]);
}

/* Portion of edge covered on its exterior side by another coplanar face. */
static int ng_edge_cover(const ng_poly *other,const double *a,const double *b,
                         double ox,double oy,double *lo,double *hi)
{
    int i;*lo=0.0;*hi=1.0;
    for(i=0;i<other->n;i++) {
        int j=(i+1)%other->n;
        double nx=other->p[j][1]-other->p[i][1],ny=other->p[i][0]-other->p[j][0];
        double c=nx*(a[0]+ox*NG_EPS-other->p[i][0])+ny*(a[1]+oy*NG_EPS-other->p[i][1]);
        double slope=nx*(b[0]-a[0])+ny*(b[1]-a[1]);
        if(fabs(slope)<1e-10){if(c<0.0)return 0;continue;}
        if(slope>0.0){double t=-c/slope;if(t>*lo)*lo=t;}
        else {double t=-c/slope;if(t<*hi)*hi=t;}
        if(*lo>=*hi)return 0;
    }
    return *lo<*hi;
}

/* A slope transition is support too. Shared edges need not have the same
 * normal: both outward faces may be walkable on a tipped box or joined ramp. */
static int ng_support_cover(const ng_face *other,const double *a,const double *b,
                             double ox,double oy,double *lo,double *hi)
{
    double delta[3],slope,offset;int k;
    if(!ng_edge_cover(&other->p,a,b,ox,oy,lo,hi))return 0;
    for(k=0;k<3;k++)delta[k]=a[k]-other->p.p[0][k];
    offset=ng_dot(other->normal,delta);
    for(k=0;k<3;k++)delta[k]=b[k]-a[k];
    slope=ng_dot(other->normal,delta);
    if(fabs(slope)<1e-9)return fabs(offset)<NG_EPS;
    {
        double l=(-NG_EPS-offset)/slope,h=(NG_EPS-offset)/slope;
        if(l>h){double t=l;l=h;h=t;}
        if(l>*lo)*lo=l;if(h<*hi)*hi=h;
    }
    return *hi-*lo>0.0001;
}

/* Merge only when the convex hull has exactly the sum of the two cell areas.
 * This removes artificial partition edges without filling concave corners. */
static int ng_merge(const ng_poly *a,const ng_poly *b,ng_poly *out)
{
    double pts[2*NG_MAX][3],hull[4*NG_MAX][3];
    int i,j,k,n=0,t,total=a->n+b->n;
    for(i=0;i<total;i++)memcpy(pts[i],i<a->n?a->p[i]:b->p[i-a->n],sizeof pts[0]);
    for(i=1;i<total;i++)for(j=i;j>0&&(pts[j][0]<pts[j-1][0]||
        (pts[j][0]==pts[j-1][0]&&pts[j][1]<pts[j-1][1]));j--)
        for(k=0;k<3;k++){double v=pts[j][k];pts[j][k]=pts[j-1][k];pts[j-1][k]=v;}
    for(i=0;i<total;i++){
        while(n>=2&&ng_cross2(hull[n-2],hull[n-1],pts[i])<=1e-8)n--;
        memcpy(hull[n++],pts[i],sizeof pts[i]);
    }
    for(i=total-2,t=n+1;i>=0;i--){
        while(n>=t&&ng_cross2(hull[n-2],hull[n-1],pts[i])<=1e-8)n--;
        memcpy(hull[n++],pts[i],sizeof pts[i]);
    }
    if(n-1>NG_MAX||n<4)return 0;
    out->n=n-1;
    for(i=0;i<out->n;i++)memcpy(out->p[i],hull[out->n-1-i],sizeof hull[0]);
    return fabs(ng_area(out)-ng_area(a)-ng_area(b))<1e-5;
}

int sh_nav_geometry_build(const sh_aug_platform *src,int count,double radius,
    double height,double floor_cos,sh_aug_platform *out,int *source,int *pieces,
    unsigned char *buried,int capacity)
{
    ng_box *boxes=NULL;ng_face *faces=NULL;ng_poly *mem=NULL,*a,*b,cut;
    ng_interval *iv=NULL,*next=NULL;
    typedef struct ng_boundary { ng_poly p; int face; } ng_boundary;
    ng_boundary *boundaries=NULL;
    int nb=0,nbcap=0,i,j,k,f,nf=0,nout=0,rc=-1;
    if(count<0||count>SH_AUG_MAX_PLATFORMS||capacity<1||radius<0.0||height<=0.0||
       floor_cos<=0.0||floor_cos>1.0||!isfinite(radius)||!isfinite(height)||!isfinite(floor_cos)||
       (count&&!src)||!out||!source||!pieces||!buried)return -1;
    boxes=(ng_box*)calloc(count?count:1,sizeof *boxes);
    faces=(ng_face*)calloc(count?count*6:1,sizeof *faces);
    mem=(ng_poly*)malloc(2*(size_t)capacity*sizeof *mem);
    iv=(ng_interval*)malloc(2*(size_t)(count*6+1)*sizeof *iv);
    if(!boxes||!faces||!mem||!iv)goto done;
    next=iv+count*6+1;
    for(i=0;i<count;i++) {
        if(!ng_box_read(&src[i],&boxes[i]))goto done;
        if(src[i].obstacle_only)continue;
        for(j=0;j<(boxes[i].solid?6:1);j++) {
            ng_face face;int axis=j/2,sign=(j&1)?-1:1;
            if(boxes[i].solid)ng_make_face(&boxes[i],axis,sign,&face);
            else {
                face.p.n=4;
                for(k=0;k<3;k++)face.normal[k]=src[i].n[k];
                for(k=0;k<4;k++){int x;for(x=0;x<3;x++)face.p.p[k][x]=src[i].c[k][x];}
            }
            if(face.normal[2]+1e-7<floor_cos)continue;
            face.source=i;face.face=boxes[i].solid?(axis==2?src[i].face:j):src[i].face;
            faces[nf++]=face;
        }
    }
        /* Erode only the exposed boundary of the entire coplanar support set. */
    for(i=0;i<nf&&radius>0.0;i++) {
            for(k=0;k<faces[i].p.n;k++) {
                double *p=faces[i].p.p[k],*q=faces[i].p.p[(k+1)%faces[i].p.n];
                double dx=q[0]-p[0],dy=q[1]-p[1],len=sqrt(dx*dx+dy*dy);
                int ni=1,v;iv[0].lo=0.0;iv[0].hi=1.0;
                if(len<1e-9)continue;
                for(j=0;j<nf&&ni;j++)if(j!=i) {
                    double lo,hi;int nn=0;
                    if(!ng_support_cover(&faces[j],p,q,-dy/len,dx/len,&lo,&hi))continue;
                    for(v=0;v<ni;v++) {
                        if(hi<=iv[v].lo||lo>=iv[v].hi)next[nn++]=iv[v];
                        else {
                            if(lo>iv[v].lo){next[nn].lo=iv[v].lo;next[nn++].hi=lo;}
                            if(hi<iv[v].hi){next[nn].lo=hi;next[nn++].hi=iv[v].hi;}
                        }
                    }
                    memcpy(iv,next,(size_t)nn*sizeof *iv);ni=nn;
                }
                for(v=0;v<ni;v++) {
                    double p0[3],p1[3];int x;
                    for(x=0;x<3;x++){p0[x]=p[x]+iv[v].lo*(q[x]-p[x]);p1[x]=p[x]+iv[v].hi*(q[x]-p[x]);}
                    if(nb==nbcap) {
                        int nc=nbcap?nbcap*2:128;ng_boundary *grown;
                        if(nc>32768)goto done;
                        grown=(ng_boundary*)realloc(boundaries,(size_t)nc*sizeof *grown);
                        if(!grown)goto done;boundaries=grown;nbcap=nc;
                    }
                    ng_boundary_cut(&faces[i],p0,p1,radius,&boundaries[nb].p);
                    boundaries[nb++].face=i;
                }
            }
        }
    for(f=0;f<nf;f++) {
        ng_face *face=&faces[f];int np=1,start=nout;
        a=mem;b=mem+capacity;a[0]=face->p;
        /* Partition overlapping support without inset along partition seams. */
        for(j=0;j<f&&np;j++)if(ng_coplanar(face,&faces[j]))
            if(!ng_cut_all(&a,&b,&np,&faces[j].p,capacity))goto done;
        /* Subtract occupied standing space, including the actual underside of
         * an elevated box. Steep boxes remain obstacles even with no nav face. */
        for(j=0;j<count&&np;j++)if(j!=face->source&&boxes[j].solid) {
            if(!ng_obstacle(face,&boxes[j],radius,height,floor_cos,
                           !src[j].obstacle_only,&cut))goto done;
            if(cut.n&&!ng_cut_all(&a,&b,&np,&cut,capacity))goto done;
        }
        for(i=0;i<nb&&np;i++)if(ng_coplanar(face,&faces[boundaries[i].face]))
            if(!ng_cut_all(&a,&b,&np,&boundaries[i].p,capacity))goto done;
        for(i=0;i<np;i++)for(j=i+1;j<np;) {
            ng_poly merged;
            if(ng_merge(&a[i],&a[j],&merged)) {
                a[i]=merged;a[j]=a[--np];i=-1;break;
            } else j++;
        }
        for(i=0;i<np;i++) {
            sh_aug_platform *dst;
            if(nout==capacity)goto done;
            dst=&out[nout];*dst=src[face->source];dst->corners=a[i].n;dst->prepared=1;
            dst->depth=0.0f;dst->face=face->face;
            for(k=0;k<3;k++)dst->n[k]=(float)face->normal[k];
            for(j=0;j<a[i].n;j++)for(k=0;k<3;k++)dst->c[j][k]=(float)a[i].p[j][k];
            for(j=0;j<4;j++)for(k=0;k<3;k++)dst->support[j][k]=(float)face->p.p[j][k];
            source[nout]=face->source;buried[nout]=0;nout++;
        }
        for(i=start;i<nout;i++)pieces[i]=nout-start;
    }
    /* Keep a diagnostic for a source with no admitted face or standing room. */
    for(i=0;i<count;i++) {
        int found=0;
        if(src[i].obstacle_only)continue;
        for(j=0;j<nout;j++)if(source[j]==i)found=1;
        if(found)continue;
        if(nout==capacity)goto done;
        out[nout]=src[i];source[nout]=i;pieces[nout]=0;buried[nout]=1;nout++;
    }
    rc=nout;
done:
    free(boundaries);free(iv);free(mem);free(faces);free(boxes);return rc;
}
