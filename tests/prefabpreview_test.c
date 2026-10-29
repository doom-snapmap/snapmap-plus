/* prefabpreview_test.c -- bounded BMODEL/MD6 decode and transport-blob contract. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/prefabpreview.c"

static int failures;
static int resolver_mode;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }
int sh_imgpreview_read_payload(int kind, const char *name, size_t max_bytes,
                               unsigned char **out_bytes, size_t *out_len)
{
    (void)max_bytes;
    if (out_bytes) *out_bytes = NULL; if (out_len) *out_len = 0;
    const char *body=NULL;
    if (resolver_mode && kind==SH_ASSET_SNAPDEF) {
        if (_stricmp(name,"spawner/test")==0)
            body="{ edit={ spawnerEntityPair={ entityStatic=\"pickup/test\"; } } }";
        else if (_stricmp(name,"pickup/test")==0)
            body="{ inherit=\"pickup/base\"; edit={} }";
        else if (_stricmp(name,"pickup/base")==0)
            body="{ inherit=\"pickup/root\"; edit={ renderModelInfo={ model=\"models/pickup.lwo\"; scale={ x=2; z=4; } } } }";
        else if (_stricmp(name,"pickup/root")==0)
            body="{ edit={ renderModelInfo={ scale={ y=3; } } } }";
        else if (_stricmp(name,"block/child")==0)
            body="{ inherit=\"block/base\"; edit={ renderModelInfo={ scale={ x=1e300; y=115; z=216; } } } }";
        else if (_stricmp(name,"block/base")==0)
            body="{ edit={ renderModelInfo={ model=\"models/block.lwo\"; scale={ x=16; y=128; z=64; } } } }";
        else if (_stricmp(name,"model/only")==0)
            body="{ edit={ renderModelInfo={ model=\"models/plain.lwo\"; } } }";
        /* Palette rows: the player row's attachment names its own model first in a derived row. */
        else if (_stricmp(name,"player/start")==0)
            body="{ inherit=\"player/base\"; edit={ renderInfo={ renderModelAttachments={ num=1; item[0]={ "
                 "renderModelName=\"weapons/gun.md6\"; tagName=\"right_hand\"; } } } } }";
        else if (_stricmp(name,"player/base")==0)
            body="{ edit={ renderInfo={ renderModelName=\"chars/player_snapeditor.md6\"; "
                 "size={ x=32; y=32; z=96; } arrowSize={ x=2; y=2; z=1; } } } }";
    }
    if (body) {
        size_t n=strlen(body); unsigned char *copy=(unsigned char *)malloc(n);
        if (!copy) return 0;
        memcpy(copy,body,n); *out_bytes=copy; *out_len=n; return 1;
    }
    return 0;
}

static void put_le32(unsigned char *p, unsigned v)
{
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static void put_be16(unsigned char *p, unsigned v) { p[0]=(unsigned char)(v>>8); p[1]=(unsigned char)v; }
static void put_be32(unsigned char *p, unsigned v)
{
    p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v;
}
static void put_bef(unsigned char *p, float value)
{
    unsigned bits=0; memcpy(&bits,&value,4); put_be32(p,bits);
}
static void put_idstr(unsigned char *p, size_t *at, const char *value)
{
    unsigned n=(unsigned)strlen(value); put_le32(p+*at,n); *at+=4; memcpy(p+*at,value,n); *at+=n;
}
static void put_vertex(unsigned char *p, float x, float y, float z, unsigned char nx)
{
    memset(p,0,48); put_bef(p,x); put_bef(p+4,y); put_bef(p+8,z);
    p[0x14]=nx; p[0x15]=128; p[0x16]=255; p[0x17]=255;
}

static size_t make_bmodel_n(unsigned char *body, size_t cap, unsigned surfaces)
{
    memset(body,0,cap); size_t at=0;
    memcpy(body,"\x1b\x4c\x4d\x42",4); at=8; put_be32(body+at,surfaces); at+=4;
    for (unsigned s=0;s<surfaces;s++) {
        put_le32(body+at,3); at+=4; memcpy(body+at,"mat",3); at+=3;
        at+=16; put_be32(body+at,3); put_be32(body+at+4,3); at+=8; at+=44;
        put_vertex(body+at,(float)(s*4),0,0,255); at+=48;
        put_vertex(body+at,(float)(s*4+2),0,0,255); at+=48;
        put_vertex(body+at,(float)(s*4),3,0,255); at+=48;
        put_be16(body+at,0); put_be16(body+at+2,1); put_be16(body+at+4,2); at+=6;
        at+=32;
    }
    at+=24; return at;
}

static size_t make_bmodel(unsigned char *body, size_t cap) { return make_bmodel_n(body,cap,1); }

static size_t make_md6(unsigned char *body, size_t cap)
{
    (void)cap; memset(body,0,1024); size_t at=0;
    memcpy(body+at,"\x2b\x02\x4d\x4d",4); at+=4; at+=8;
    put_idstr(body,&at,""); at+=24; body[at++]=1; put_idstr(body,&at,"");
    put_be16(body+at,0); at+=2; at+=24; put_be32(body+at,0); at+=4; at+=36;
    put_be32(body+at,1); at+=4; put_idstr(body,&at,"surface"); put_idstr(body,&at,"material/test");
    body[at++]=1; put_be32(body+at,1); put_be32(body+at+4,3); put_be32(body+at+8,1); at+=12; at+=24;
    put_vertex(body+at,-1,0,0,0); at+=48; put_vertex(body+at,1,0,0,0); at+=48;
    put_vertex(body+at,0,2,0,0); at+=48;
    put_be16(body+at,0); put_be16(body+at+2,1); put_be16(body+at+4,2); at+=6;
    at+=12; body[at++]=0; put_be32(body+at,0); at+=4;
    put_be32(body+at,0); at+=4; memcpy(body+at,"\x2b\x02\x4d\x4d",4); at+=4;
    return at;
}

/* Two md6 surfaces: a body triangle at x 0..1 and a wound overlay triangle at x 10..11. */
static size_t make_md6_two(unsigned char *body, size_t cap)
{
    const char *names[2]={"body","wound_gut"};
    memset(body,0,cap); size_t at=0;
    memcpy(body+at,"\x2b\x02\x4d\x4d",4); at+=4; at+=8;
    put_idstr(body,&at,"skel"); at+=24; body[at++]=1; put_idstr(body,&at,"");
    put_be16(body+at,0); at+=2; at+=24; put_be32(body+at,0); at+=4; at+=36;
    put_be32(body+at,2); at+=4;
    for (unsigned s=0;s<2;s++) {
        float x=(float)(s*10);
        put_idstr(body,&at,names[s]); put_idstr(body,&at,"material/test");
        body[at++]=1; put_be32(body+at,1); put_be32(body+at+4,3); put_be32(body+at+8,1); at+=12; at+=24;
        put_vertex(body+at,x,0,0,255); at+=48; put_vertex(body+at,x+1,0,0,255); at+=48;
        put_vertex(body+at,x,1,0,255); at+=48;
        put_be16(body+at,0); put_be16(body+at+2,1); put_be16(body+at+4,2); at+=6;
        at+=12; body[at++]=0; put_be32(body+at,0); at+=4;
    }
    put_be32(body+at,0); at+=4; memcpy(body+at,"\x2b\x02\x4d\x4d",4); at+=4;
    return at;
}

static void check_md6_kits_and_bounds(void)
{
    unsigned char body[2048]; size_t len=make_md6_two(body,sizeof body);
    const unsigned char def[]="{ init { mesh \"md6/a.md6mesh\" } meshKits { Heads 0 { } "
                              "Gore 2 { \"wound_gut\" = \"wound_gut\" \"wound_body_B\" = \"wounds_body_B\" } "
                              "Door 0 { } } }";
    pp_names *hidden=(pp_names *)calloc(1,sizeof *hidden);
    CHECK(hidden!=NULL); if (!hidden) return;
    pp_md6_hidden_kits(def,sizeof def-1,hidden);
    CHECK(hidden->count==4);
    CHECK(pp_names_has(hidden,(const unsigned char *)"wounds_body_B",13));
    CHECK(!pp_names_has(hidden,(const unsigned char *)"body",4));
    pp_mesh all; memset(&all,0,sizeof all);
    CHECK(pp_decode_md6_ex(body,len,&all,NULL)==1 && all.vertex_count==6 && all.index_count==6);
    pp_mesh shown; memset(&shown,0,sizeof shown);
    CHECK(pp_decode_md6_ex(body,len,&shown,hidden)==1);
    CHECK(shown.vertex_count==3 && shown.index_count==3 && shown.bounds[3]<2.0f);
    pp_mesh_free(&all); pp_mesh_free(&shown); free(hidden);
}

/* A dense grid above the bound clusters below it, keeping its bounds; a mesh within is untouched. */
static void check_triangle_bound(void)
{
    const unsigned n=200; pp_mesh m; memset(&m,0,sizeof m);
    CHECK(pp_reserve(&m,(n+1)*(n+1),n*n*6));
    unsigned char disk[48]; memset(disk,0,sizeof disk);
    for (unsigned y=0;y<=n;y++) for (unsigned x=0;x<=n;x++) {
        put_vertex(disk,(float)x,(float)y,(float)((x+y)&1),128); disk[0x16]=255;
        CHECK(pp_add_vertex(&m,disk));
    }
    for (unsigned y=0;y<n;y++) for (unsigned x=0;x<n;x++) {
        unsigned a=y*(n+1)+x, b=a+1, c=a+n+1, d=c+1;
        m.indices[m.index_count++]=a; m.indices[m.index_count++]=b; m.indices[m.index_count++]=d;
        m.indices[m.index_count++]=a; m.indices[m.index_count++]=d; m.indices[m.index_count++]=c;
    }
    CHECK(m.index_count/3==80000);
    CHECK(pp_limit_triangles(&m,20000)==1);
    CHECK(m.index_count/3<=20000 && m.index_count/3>1000 && m.index_count%3==0);
    CHECK(m.bounds[0]==0.0f && m.bounds[3]==(float)n);
    for (uint32_t i=0;i<m.index_count;i++) CHECK(m.indices[i]<m.vertex_count);
    uint32_t keep=m.index_count;
    CHECK(pp_limit_triangles(&m,20000)==1 && m.index_count==keep);
    pp_mesh_free(&m);
}

static void check_decoder(int md6)
{
    unsigned char body[1024]; size_t len=md6?make_md6(body,sizeof body):make_bmodel(body,sizeof body);
    pp_mesh mesh; memset(&mesh,0,sizeof mesh);
    int ok=md6?pp_decode_md6(body,len,&mesh):pp_decode_bmodel(body,len,&mesh);
    CHECK(ok==1); CHECK(mesh.vertex_count==3); CHECK(mesh.index_count==3);
    CHECK(mesh.indices && mesh.indices[0]==0 && mesh.indices[1]==1 && mesh.indices[2]==2);
    CHECK(mesh.vertices && fabs(mesh.vertices[1].x-(md6?1.0f:2.0f))<0.001f);
    CHECK(mesh.have_bounds==1); CHECK(mesh.bounds[3]>mesh.bounds[0]); CHECK(mesh.bounds[4]>mesh.bounds[1]);
    pp_mesh_free(&mesh);
}

int main(void)
{
    check_decoder(0); check_decoder(1);
    check_md6_kits_and_bounds();
    check_triangle_bound();
    CHECK(sh_prefabpreview_is_mesh_name("a/b.md6") && sh_prefabpreview_is_mesh_name("a/B.LWO") &&
          sh_prefabpreview_is_mesh_name("m.bmodel") && !sh_prefabpreview_is_mesh_name("maps/x/lift_1") &&
          !sh_prefabpreview_is_mesh_name(".md6") && !sh_prefabpreview_is_mesh_name(NULL));
    {
        char model[128]; float size[3]; resolver_mode=1;
        CHECK(sh_prefabpreview_editor_model("player/start",model,sizeof model,size)==
              (SH_PREFAB_DEFAULT_MODEL|SH_PREFAB_DEFAULT_SIZE));
        CHECK(strcmp(model,"chars/player_snapeditor.md6")==0);
        CHECK(size[0]==32.0f && size[1]==32.0f && size[2]==96.0f);
        CHECK(sh_prefabpreview_editor_model("model/only",model,sizeof model,size)==0 && model[0]==0);
        resolver_mode=0;
    }
    {
        unsigned char body[1024];
        pp_mesh mesh; memset(&mesh,0,sizeof mesh);
        CHECK(pp_decode_bmodel(body,make_bmodel_n(body,sizeof body,2),&mesh)==1);
        CHECK(mesh.vertex_count==6 && mesh.index_count==6 && mesh.indices[3]==3);
        pp_mesh_free(&mesh);
    }
    {
        char model[128]; float scale[3]; resolver_mode=1;
        CHECK(sh_prefabpreview_resolve_model("spawner/test",model,sizeof model)==1);
        CHECK(strcmp(model,"models/pickup.lwo")==0);
        CHECK(sh_prefabpreview_resolve_defaults("spawner/test",model,sizeof model,scale)==
              (SH_PREFAB_DEFAULT_MODEL|SH_PREFAB_DEFAULT_SCALE));
        CHECK(strcmp(model,"models/pickup.lwo")==0);
        CHECK(fabs(scale[0]-2.0f)<0.001f && fabs(scale[1]-3.0f)<0.001f &&
              fabs(scale[2]-4.0f)<0.001f);
        CHECK(sh_prefabpreview_resolve_defaults("block/child",model,sizeof model,scale)==
              (SH_PREFAB_DEFAULT_MODEL|SH_PREFAB_DEFAULT_SCALE));
        CHECK(strcmp(model,"models/block.lwo")==0);
        CHECK(fabs(scale[0]-16.0f)<0.001f && fabs(scale[1]-115.0f)<0.001f &&
              fabs(scale[2]-216.0f)<0.001f);
        CHECK(sh_prefabpreview_resolve_defaults("model/only",model,sizeof model,scale)==
              SH_PREFAB_DEFAULT_MODEL);
        CHECK(strcmp(model,"models/plain.lwo")==0);
        CHECK(scale[0]==1.0f && scale[1]==1.0f && scale[2]==1.0f);
        CHECK(sh_prefabpreview_resolve_model("missing",model,sizeof model)==0);
        resolver_mode=0;
    }
    {
        const unsigned char decl[]="{ init { mesh \"md6/objects/test.md6mesh\" } }";
        char found[128];
        CHECK(pp_find_decl_mesh(decl,sizeof decl-1,found,sizeof found)==1);
        CHECK(strcmp(found,"md6/objects/test.md6mesh")==0);
    }
    {
        pp_mesh mesh; memset(&mesh,0,sizeof mesh); unsigned char body[512];
        CHECK(pp_decode_bmodel(body,make_bmodel(body,sizeof body),&mesh)==1);
        unsigned char *blob=NULL; int bytes=0;
        CHECK(pp_build_blob(17,"models/test.lwo",&mesh,&blob,&bytes)==1);
        CHECK(bytes>56 && blob!=NULL);
        if (blob) {
            sh_prefab_mesh_blob_header *h=(sh_prefab_mesh_blob_header *)blob;
            CHECK(h->magic==SH_PREFAB_MESH_MAGIC && h->generation==17 && h->status==SH_PREFAB_MESH_OK);
            CHECK(h->vertex_stride==16 && h->vertex_count==3 && h->index_count==3);
        }
        free(blob); pp_mesh_free(&mesh);
    }
    if (failures) { fprintf(stderr,"prefabpreview_test: %d failure(s)\n",failures); return 1; }
    puts("prefabpreview_test: OK"); return 0;
}
