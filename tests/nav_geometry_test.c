/* nav_geometry_test.c -- geometric coverage, occupancy and union invariants. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../src/backend/nav_geometry.h"
static int failures;
#define CHECK(x) do { if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);failures++;} }while(0)
static sh_aug_platform boxes[8],result[SH_AUG_MAX_PLATFORMS];
static int source[SH_AUG_MAX_PLATFORMS],pieces[SH_AUG_MAX_PLATFORMS];
static unsigned char buried[SH_AUG_MAX_PLATFORMS];
static void box(int i,float x0,float y0,float x1,float y1,float bottom,float top)
{
    sh_aug_platform *p=&boxes[i];memset(p,0,sizeof *p);
    p->c[0][0]=p->c[3][0]=x0;p->c[1][0]=p->c[2][0]=x1;
    p->c[0][1]=p->c[1][1]=y1;p->c[2][1]=p->c[3][1]=y0;
    p->c[0][2]=p->c[1][2]=p->c[2][2]=p->c[3][2]=top;
    p->n[2]=1;p->depth=top-bottom;p->face=4;
}
static int bake(int n,double r,double h)
{return sh_nav_geometry_build(boxes,n,r,h,0.7,0,result,source,pieces,buried,SH_AUG_MAX_PLATFORMS);}
static double area(int n)
{
    int i,k;double sum=0;
    for(i=0;i<n;i++)for(k=0;k<result[i].corners;k++){
        int j=(k+1)%result[i].corners;
        sum+=result[i].c[j][0]*result[i].c[k][1]-result[i].c[k][0]*result[i].c[j][1];
    }
    return sum*0.5;
}
static int contains(int n,double x,double y,double z)
{
    int i,k;
    for(i=0;i<n;i++){
        const sh_aug_platform *p=&result[i];int in=1;
        if(buried[i])continue;
        if(fabs(p->n[0]*(x-p->c[0][0])+p->n[1]*(y-p->c[0][1])+p->n[2]*(z-p->c[0][2]))>0.01)continue;
        for(k=0;k<p->corners;k++){
            int j=(k+1)%p->corners;
            if((p->c[j][1]-p->c[k][1])*(x-p->c[k][0])-
               (p->c[j][0]-p->c[k][0])*(y-p->c[k][1])< -0.01)in=0;
        }
        if(in)return 1;
    }
    return 0;
}

static int bake_steps(int n,double radius)
{return sh_nav_geometry_build(boxes,n,radius,80,0.7,18,result,source,pieces,buried,SH_AUG_MAX_PLATFORMS);}

static void test_body_ray_exit(void)
{
    double start[3]={0,0,0},direction[3]={1,0,0},distance;
    box(0,-256,-256,256,256,0,128);
    CHECK(sh_nav_geometry_ray_exit(&boxes[0],start,direction,64,80,&distance));
    CHECK(fabs(distance-320)<0.01);
    start[0]=400;direction[0]=-1;
    CHECK(sh_nav_geometry_ray_exit(&boxes[0],start,direction,64,80,&distance));
    CHECK(fabs(distance-720)<0.01);
    direction[0]=1;
    CHECK(sh_nav_geometry_ray_exit(&boxes[0],start,direction,64,80,&distance));
    CHECK(distance==0);
    start[0]=0;start[2]=128;
    CHECK(sh_nav_geometry_ray_exit(&boxes[0],start,direction,64,80,&distance));
    CHECK(distance==0); /* Standing on the top is contact, not penetration. */
    direction[0]=0;
    CHECK(!sh_nav_geometry_ray_exit(&boxes[0],start,direction,64,80,&distance));
    direction[0]=NAN;
    CHECK(!sh_nav_geometry_ray_exit(&boxes[0],start,direction,64,80,&distance));
}

/* Independent rectilinear oracle: partition the agent footprint at every box
 * boundary, then require each open cell to have support. Four-corner tests are
 * insufficient for concave unions and holes. This uses no baker predicates. */
static int supported_square(int count,double x,double y,double radius)
{
    double xs[18],ys[18];int nx=2,ny=2,i,j,k;
    xs[0]=x-radius;xs[1]=x+radius;ys[0]=y-radius;ys[1]=y+radius;
    for(i=0;i<count;i++)for(j=0;j<2;j++) {
        double a=boxes[i].c[j][0],b=boxes[i].c[j*2][1];
        if(a>xs[0]&&a<xs[1])xs[nx++]=a;
        if(b>ys[0]&&b<ys[1])ys[ny++]=b;
    }
    for(i=1;i<nx;i++)for(j=i;j>0&&xs[j]<xs[j-1];j--){double t=xs[j];xs[j]=xs[j-1];xs[j-1]=t;}
    for(i=1;i<ny;i++)for(j=i;j>0&&ys[j]<ys[j-1];j--){double t=ys[j];ys[j]=ys[j-1];ys[j-1]=t;}
    for(i=1;i<nx;i++)for(j=1;j<ny;j++) {
        double px=(xs[i-1]+xs[i])*0.5,py=(ys[j-1]+ys[j])*0.5;int found=0;
        if(xs[i]==xs[i-1]||ys[j]==ys[j-1])continue;
        for(k=0;k<count;k++)if(px>=boxes[k].c[0][0]&&px<=boxes[k].c[1][0]&&
            py>=boxes[k].c[2][1]&&py<=boxes[k].c[0][1])found=1;
        if(!found)return 0;
    }
    return 1;
}

static void test_step_clearance(void)
{
    int r,n;double radii[3]={24,48,64};
    for(r=0;r<3;r++) {
        /* Partial overlap: remove the buried lower face, retain both sides
         * of the actual riser, and still erode the outside of the union. */
        box(0,0,0,256,512,0,128);box(1,192,0,512,512,96,112);
        n=bake_steps(2,radii[r]);CHECK(n>0);
        CHECK(contains(n,255,256,128));CHECK(contains(n,257,256,112));
        CHECK(!contains(n,220,256,112));CHECK(!contains(n,1,256,128));
        /* A low unmarked obstacle cannot grant support or a step exception. */
        boxes[0].obstacle_only=1;n=bake_steps(2,radii[r]);CHECK(n>0);
        CHECK(!contains(n,257,256,112));
        /* Thin floating solids still bury the centre column below them. */
        box(0,0,0,256,512,124,128);n=bake_steps(2,radii[r]);CHECK(n>0);
        CHECK(!contains(n,220,256,112));CHECK(contains(n,257,256,112));
        /* An overhead lip within body height keeps horizontal clearance. */
        box(0,0,0,256,512,0,112);box(1,256,0,512,512,184,200);
        n=bake_steps(2,radii[r]);CHECK(n>0);CHECK(!contains(n,255,256,112));
        box(1,256,0,512,512,208,224);n=bake_steps(2,radii[r]);CHECK(n>0);
        CHECK(contains(n,220,256,112)==(220<=256-radii[r]));
        /* Close in height alone cannot make unsupported space into a step. */
        box(1,257,0,512,512,0,120);n=bake_steps(2,radii[r]);CHECK(n>0);
        CHECK(!contains(n,255,256,112));CHECK(!contains(n,258,256,120));
    }
    CHECK(sh_nav_geometry_build(boxes,2,24,80,0.7,NAN,result,source,pieces,buried,512)==-1);
    CHECK(sh_nav_geometry_build(boxes,2,24,80,0.7,80,result,source,pieces,buried,512)==-1);
}

static unsigned rng_state=0x4b71e239u;
static unsigned random_u32(void)
{rng_state=1664525u*rng_state+1013904223u;return rng_state;}

static void test_union_oracle(void)
{
    int trial,i,x,y,n,comparisons=0;
    for(trial=0;trial<128;trial++) {
        double ox=(trial&1)?-24000.0:0.0,oy=(trial&2)?19000.0:0.0;
        double radius=(trial%3==0)?24.0:(trial%3==1)?48.0:64.0;
        for(i=0;i<8;i++) {
            float x0=(float)(ox+(int)(random_u32()%17)*32-256);
            float y0=(float)(oy+(int)(random_u32()%17)*32-256);
            box(i,x0,y0,x0+64+(random_u32()%8)*32,y0+64+(random_u32()%8)*32,
                (i&1)?96.0f:0.0f,128.0f);
        }
        n=bake(8,radius,80);CHECK(n>0);
        if(n<0)continue;
        for(x=-300;x<560;x+=37)for(y=-300;y<560;y+=41) {
            double px=ox+x+0.375,py=oy+y+0.625;
            int expected=supported_square(8,px,py,radius),actual=contains(n,px,py,128);
            comparisons++;
            if(actual!=expected) {
                printf("union oracle trial %d at %.3f %.3f radius %.0f expected %d got %d\n",
                    trial,px,py,radius,expected,actual);
                CHECK(actual==expected);return;
            }
        }
        /* Reverse input order without changing the union. */
        for(i=0;i<4;i++){sh_aug_platform p=boxes[i];boxes[i]=boxes[7-i];boxes[7-i]=p;}
        n=bake(8,radius,80);CHECK(n>0);
        for(x=-256;x<512;x+=61)for(y=-256;y<512;y+=67) {
            double px=ox+x+0.375,py=oy+y+0.625;
            CHECK(contains(n,px,py,128)==supported_square(8,px,py,radius));comparisons++;
        }
    }
    printf("independent support-union comparisons: %d\n",comparisons);
}

static void test_rotated_obstacle_occupancy(void)
{
    int trial,i,k,x,y,n,checked=0;
    for(trial=0;trial<96;trial++) {
        double a=trial*.413,b=trial*.731,c=trial*.197;
        double ca=cos(a),sa=sin(a),cb=cos(b),sb=sin(b),cc=cos(c),sc=sin(c);
        double rot[3][3]={{cc*cb,cc*sb*sa-sc*ca,cc*sb*ca+sc*sa},
                          {sc*cb,sc*sb*sa+cc*ca,sc*sb*ca-cc*sa},{-sb,cb*sa,cb*ca}};
        double centre[3]={31.0,-17.0,32.0+trial*3.0};
        double radius=(trial%3==0)?24:(trial%3==1)?48:64;
        box(0,-512,-512,512,512,0,16);box(1,-64,-96,64,96,-48,48);
        for(i=0;i<4;i++) {
            double p[3];for(k=0;k<3;k++)p[k]=boxes[1].c[i][k];
            for(k=0;k<3;k++)boxes[1].c[i][k]=(float)(centre[k]+rot[k][0]*p[0]+rot[k][1]*p[1]+rot[k][2]*p[2]);
        }
        for(k=0;k<3;k++)boxes[1].n[k]=(float)rot[k][2];
        boxes[1].obstacle_only=1;n=bake(2,radius,128);CHECK(n>0);
        /* An independent world-to-local point test must never find solid
         * interior inside an admitted standing footprint. Sample the interior
         * as well as corners, including ceilings spanning over all four corners. */
        for(x=-240;x<=240;x+=30)for(y=-240;y<=240;y+=30) {
            int dx,dy,dz;
            if(!contains(n,x,y,16))continue;
            for(dx=-1;dx<=1;dx++)for(dy=-1;dy<=1;dy++)for(dz=0;dz<=4;dz++) {
                double p[3]={x+dx*radius-centre[0],y+dy*radius-centre[1],16+dz*32-centre[2]},v[3];
                for(k=0;k<3;k++)v[k]=rot[0][k]*p[0]+rot[1][k]*p[1]+rot[2][k]*p[2];
                CHECK(!(fabs(v[0])<63.99&&fabs(v[1])<95.99&&fabs(v[2])<47.99));checked++;
            }
        }
    }
    printf("rotated obstacle body-point checks: %d\n",checked);
}
int main(void)
{
    test_body_ray_exit();
    test_step_clearance();
    int n,i,k;double a;
    test_union_oracle();
    test_rotated_obstacle_occupancy();
    box(0,0,0,256,256,0,128);n=bake(1,24,80);
    CHECK(n>0);CHECK(fabs(area(n)-208.0*208.0)<0.1);
    box(1,256,0,512,256,0,128);n=bake(2,24,80);
    CHECK(n>0);CHECK(fabs(area(n)-464.0*208.0)<0.1);
    CHECK(contains(n,255,128,128));CHECK(contains(n,257,128,128));
    /* Duplicate boxes do not duplicate navigation or erase their common top. */
    boxes[2]=boxes[0];a=area(n);n=bake(3,24,80);CHECK(fabs(area(n)-a)<0.1);
    /* A deck floating between supports shares one continuous upper surface. */
    box(0,0,0,128,128,0,128);box(1,384,0,512,128,0,128);
    box(2,128,0,384,128,96,128);n=bake(3,24,80);
    CHECK(fabs(area(n)-464.0*80.0)<0.1);CHECK(contains(n,256,64,128));
    /* A corridor just wider than the agent retains a nonempty centre interval. */
    box(0,0,0,256,50,0,128);n=bake(1,24,80);CHECK(n>0);CHECK(contains(n,128,25,128));
    box(0,0,0,256,47,0,128);n=bake(1,24,80);CHECK(n==1 && buried[0]);
    /* Level support joins a rising ramp at its true contact edge. */
    box(0,-256,-128,0,128,0,64);box(1,0,-128,256,128,32,64);
    boxes[1].c[1][2]=boxes[1].c[2][2]=192;
    boxes[1].n[0]=(float)(-1/sqrt(5.0));boxes[1].n[2]=(float)(2/sqrt(5.0));
    n=bake(2,24,80);CHECK(n>0);
    CHECK(contains(n,-1,0,64));CHECK(contains(n,1,0,64.5));
    /* Clearance uses the underside, not the top or centroid of the overhead box. */
    box(0,0,0,512,512,0,16);box(1,128,128,384,384,64,160);
    n=bake(2,24,80);CHECK(n>0);CHECK(!contains(n,256,256,16));CHECK(contains(n,64,64,16));
    box(1,128,128,384,384,128,160);n=bake(2,24,80);CHECK(contains(n,256,256,16));
    /* Rotating a cube 45 degrees about Y exposes TWO walkable faces. */
    box(0,-256,-256,256,256,-256,256);
    for(i=0;i<4;i++){float x=boxes[0].c[i][0],z=boxes[0].c[i][2];
        boxes[0].c[i][0]=(float)((x+z)*sqrt(0.5));boxes[0].c[i][2]=(float)((z-x)*sqrt(0.5));}
    boxes[0].n[0]=boxes[0].n[2]=(float)sqrt(0.5);
    n=bake(1,24,80);CHECK(n>=2);
    CHECK(contains(n,-1,0,512*sqrt(0.5)-1));
    CHECK(contains(n,1,0,512*sqrt(0.5)-1));
    {int pos=0,neg=0;for(i=0;i<n;i++){if(result[i].n[0]>0.1)pos=1;if(result[i].n[0]<-0.1)neg=1;}CHECK(pos&&neg);}
    /* Rigid yaw preserves solid intersection, with square agent clearance
     * measured in world XY. No emitted vertex lies inside a crossing pillar. */
    box(0,-512,-512,512,512,0,16);box(1,-64,-64,64,64,0,256);
    for(i=0;i<4;i++){float x=boxes[1].c[i][0],y=boxes[1].c[i][1];
        boxes[1].c[i][0]=(float)((x-y)*sqrt(0.5));boxes[1].c[i][1]=(float)((x+y)*sqrt(0.5));}
    n=bake(2,24,80);CHECK(n>0);CHECK(!contains(n,0,0,16));CHECK(contains(n,100,100,16));
    for(i=0;i<n;i++)for(k=0;k<result[i].corners;k++)CHECK(isfinite(result[i].c[k][0]));
    /* A cap is a failed transaction, never a successful incomplete mesh. */
    CHECK(sh_nav_geometry_build(boxes,2,24,80,0.7,0,result,source,pieces,buried,1)==-1);
    /* A non-navigable box still removes occupied standing room. */
    box(0,0,0,512,512,0,16);box(1,128,128,384,384,16,256);
    boxes[1].obstacle_only=1;n=bake(2,24,80);
    CHECK(n>0);CHECK(!contains(n,256,256,16));
    for(i=0;i<n;i++)CHECK(source[i]==0);
    /* Sweeps use actual bottoms and continuous clipping. A one-unit wall
     * between old sample locations must not disappear. */
    {
        double s[3]={0,64,0},e[3]={512,64,0};
        box(0,173,0,174,128,0,200);
        CHECK(!sh_nav_geometry_path_clear(boxes,1,-1,-1,s,e,24,80));
        box(0,128,0,384,128,100,128);
        CHECK(sh_nav_geometry_path_clear(boxes,1,-1,-1,s,e,24,80));
        box(0,128,0,384,128,64,128);
        CHECK(!sh_nav_geometry_path_clear(boxes,1,-1,-1,s,e,24,80));
        CHECK(sh_nav_geometry_path_clear(boxes,1,0,-1,s,e,24,80));
    }
    /* Reordering an intersecting coplanar construction preserves coverage. */
    box(0,0,0,256,256,0,128);box(1,128,128,512,384,96,128);
    box(2,384,0,640,256,0,128);n=bake(3,24,80);a=area(n);
    {sh_aug_platform swap=boxes[0];boxes[0]=boxes[2];boxes[2]=swap;}
    n=bake(3,24,80);CHECK(n>0);CHECK(fabs(area(n)-a)<0.1);
    CHECK(contains(n,250,200,128));CHECK(!contains(n,320,64,128));
    CHECK(sh_nav_geometry_build(boxes,3,NAN,80,0.7,0,result,source,pieces,buried,512)==-1);
    /* Rigid rotations exercise every local face as a possible floor. The
     * expected normals come directly from the rotation matrix, independently
     * of the baker's reconstructed box and face enumeration. */
    {
        int trial;
        for(trial=0;trial<256;trial++) {
            double ax=trial*0.371,ay=trial*0.719,az=trial*1.131;
            double cx=cos(ax),sx=sin(ax),cy=cos(ay),sy=sin(ay),cz=cos(az),sz=sin(az);
            double rot[3][3]={{cz*cy,cz*sy*sx-sz*cx,cz*sy*cx+sz*sx},
                              {sz*cy,sz*sy*sx+cz*cx,sz*sy*cx-cz*sx},
                              {-sy,cy*sx,cy*cx}};
            double origin[3]={-4096.0+trial*31,2048.0-trial*17,512.0+trial*3};
            int expected=0,seen[3]={0,0,0},r,j;
            box(0,-256,-256,256,256,-256,256);
            for(i=0;i<4;i++) {
                double p[3];for(k=0;k<3;k++)p[k]=boxes[0].c[i][k];
                for(k=0;k<3;k++)boxes[0].c[i][k]=(float)(origin[k]+rot[k][0]*p[0]+rot[k][1]*p[1]+rot[k][2]*p[2]);
            }
            for(k=0;k<3;k++){boxes[0].n[k]=(float)rot[k][2];if(fabs(rot[2][k])>=0.7)expected++;}
            n=bake(1,24,80);CHECK(n>0);
            for(r=0;r<n;r++)if(!buried[r]) {
                int axis=-1;double best=0;
                CHECK(result[r].n[2]>=0.7-0.0001);
                for(j=0;j<3;j++) {
                    double dot=0;for(k=0;k<3;k++)dot+=rot[k][j]*result[r].n[k];
                    if(fabs(dot)>best){best=fabs(dot);axis=j;}
                }
                CHECK(best>0.9999);CHECK(axis>=0);
                if(axis>=0)seen[axis]=1;
                for(i=0;i<result[r].corners;i++) {
                    double d=0;for(k=0;k<3;k++) {CHECK(isfinite(result[r].c[i][k]));d+=result[r].n[k]*(result[r].c[i][k]-origin[k]);}
                    CHECK(fabs(d-256)<0.02);
                }
            }
            CHECK(seen[0]+seen[1]+seen[2]==expected);
        }
    }
    printf("nav geometry: %d failures\n",failures);return failures?1:0;
}
