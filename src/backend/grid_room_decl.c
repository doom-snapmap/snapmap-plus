#include "grid_room_decl.h"
#include "config_json.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID_DECL_LIMIT (8u*1024u*1024u)
#define GRID_NODE_LIMIT 16384u

typedef struct node {
    char *key, *value;
    struct node *children, *next;
    int reset;
} node;
typedef struct parser { const char *s; size_t n, p; unsigned nodes; int failed; } parser;
typedef struct buffer { char *s; size_t n, cap; int failed; } buffer;

static char *copy(const char *s, size_t n)
{
    char *out;
    if(n>GRID_DECL_LIMIT)return NULL;
    out=(char*)malloc(n+1);
    if(out){memcpy(out,s,n);out[n]=0;}return out;
}
static void dispose(node *n)
{
    while(n){node *next=n->next;dispose(n->children);free(n->key);free(n->value);free(n);n=next;}
}
static node *member(node *n, const char *key)
{
    for(n=n?n->children:NULL;n;n=n->next)if(!strcmp(n->key,key))return n;
    return NULL;
}
static int append(buffer *b,const char *s,size_t n)
{
    size_t cap;char *p;
    if(b->failed||n>GRID_DECL_LIMIT-b->n){b->failed=1;return 0;}
    if(b->n+n+1>b->cap){cap=b->cap?b->cap:256;
        while(cap<b->n+n+1)cap=cap>GRID_DECL_LIMIT/2?GRID_DECL_LIMIT+1:cap*2;
        p=(char*)realloc(b->s,cap);if(!p){b->failed=1;return 0;}b->s=p;b->cap=cap;}
    memcpy(b->s+b->n,s,n);b->n+=n;b->s[b->n]=0;return 1;
}
static void ws(parser *p)
{
    for(;;){
        while(p->p<p->n&&isspace((unsigned char)p->s[p->p]))++p->p;
        if(p->p+1>=p->n||p->s[p->p]!='/')return;
        if(p->s[p->p+1]=='/'){
            p->p+=2;while(p->p<p->n&&p->s[p->p]!='\n')++p->p;
        }else if(p->s[p->p+1]=='*'){
            p->p+=2;while(p->p+1<p->n&&(p->s[p->p]!='*'||p->s[p->p+1]!='/'))++p->p;
            if(p->p+1>=p->n){p->failed=1;return;}p->p+=2;
        }else return;
    }
}
static char *token(parser *p)
{
    size_t begin;ws(p);begin=p->p;
    if(p->failed||p->p>=p->n)return NULL;
    if(p->s[p->p]=='"'){
        ++p->p;
        while(p->p<p->n){char c=p->s[p->p++];
            if(c=='"')return copy(p->s+begin,p->p-begin);
            if(c=='\\'){if(p->p==p->n)return NULL;++p->p;}
            else if((unsigned char)c<32)return NULL;
        }return NULL;
    }
    while(p->p<p->n&&!isspace((unsigned char)p->s[p->p])&&
          !strchr("{}=;\"",p->s[p->p]))++p->p;
    return p->p==begin?NULL:copy(p->s+begin,p->p-begin);
}
static int take(parser *p,char c)
{ ws(p);if(p->failed||p->p==p->n||p->s[p->p]!=c)return 0;++p->p;return 1; }
static node *parse_block(parser *p,unsigned depth)
{
    node *root,*last=NULL;
    if(depth>32||++p->nodes>GRID_NODE_LIMIT||!take(p,'{'))return NULL;
    root=(node*)calloc(1,sizeof *root);if(!root)return NULL;
    for(;;){node *n;char *key;ws(p);
        if(p->failed||p->p==p->n)break;
        if(p->s[p->p]=='}'){++p->p;return root;}
        if(++p->nodes>GRID_NODE_LIMIT)break;
        key=token(p);if(!key)break;
        if(key[0]=='"'||member(root,key)||!take(p,'=')){free(key);break;}
        n=(node*)calloc(1,sizeof *n);if(!n){free(key);break;}n->key=key;
        if(last)last->next=n;else root->children=n;last=n;
        ws(p);
        if(p->p<p->n&&p->s[p->p]=='!'){n->reset=1;++p->p;ws(p);
            if(p->p==p->n||p->s[p->p]!='{')break;}
        if(p->p<p->n&&p->s[p->p]=='{'){
            node *child=parse_block(p,depth+1);if(!child)break;
            n->children=child->children;free(child);
            /* Empty compound nodes need a marker distinct from scalars. */
            if(!n->children){n->value=copy("{}",2);if(!n->value)break;}
            ws(p);if(p->p<p->n&&p->s[p->p]==';')++p->p;
        }else{n->value=token(p);if(!n->value||!take(p,';'))break;}
    }
    dispose(root);return NULL;
}
static node *parse(const char *s,size_t n)
{
    parser p;node *root;
    if(!s||!n||n>GRID_DECL_LIMIT||memchr(s,0,n))return NULL;
    memset(&p,0,sizeof p);p.s=s;p.n=n;root=parse_block(&p,0);ws(&p);
    if(!root||p.failed||p.p!=n){dispose(root);return NULL;}return root;
}
static int emit(buffer *b,const node *root)
{
    const node *n;
    if(!append(b,"{\n",2))return 0;
    for(n=root->children;n;n=n->next){
        if(!append(b,n->key,strlen(n->key))||!append(b," = ",3))return 0;
        if(n->reset&&!append(b,"! ",2))return 0;
        if(n->children){if(!emit(b,n))return 0;}
        else if(!n->value||!append(b,n->value,strlen(n->value)))return 0;
        if(!append(b,n->children||(n->value&&!strcmp(n->value,"{}"))?"\n":";\n",
                     n->children||(n->value&&!strcmp(n->value,"{}"))?1:2))return 0;
    }
    return append(b,"}",1);
}
static int set(node *parent,const char *key,const char *value)
{
    node *n=member(parent,key),**tail;char *text=copy(value,strlen(value));
    if(!parent||!text){free(text);return 0;}
    if(!n){n=(node*)calloc(1,sizeof *n);if(!n){free(text);return 0;}
        n->key=copy(key,strlen(key));if(!n->key){free(n);free(text);return 0;}
        tail=&parent->children;while(*tail)tail=&(*tail)->next;*tail=n;}
    dispose(n->children);n->children=NULL;free(n->value);n->value=text;return 1;
}
static int number(node *parent,const char *key,float *out)
{
    node *n=member(parent,key);char *end;double v;
    if(!n){*out=0;return 1;}if(!n->value)return 0;
    v=strtod(n->value,&end);if(end==n->value||*end||!isfinite(v)||fabs(v)>1e20)return 0;
    *out=(float)v;return 1;
}
static int set_number(node *parent,const char *key,float value)
{
    char text[48];if(!isfinite(value))return 0;
    snprintf(text,sizeof text,"%.9g",(double)value);return set(parent,key,text);
}
static int vec(node *parent,const char *key,float v[3])
{
    node *n=member(parent,key);
    if(!n||!n->children)return 0;
    return number(n,"x",v)&&number(n,"y",v+1)&&number(n,"z",v+2);
}
static int set_vec(node *parent,const char *key,const float v[3])
{
    node *n=member(parent,key);
    if(!n){if(!set(parent,key,"{}"))return 0;n=member(parent,key);free(n->value);n->value=NULL;}
    return set_number(n,"x",v[0])&&set_number(n,"y",v[1])&&set_number(n,"z",v[2]);
}
static int transform(node *n,const sh_grid_warp *w,int height)
{
    float p[3],q[3],h;
    if(!n||!number(n,"x",p)||!number(n,"y",p+1)||!number(n,"z",p+2))return 0;
    sh_grid_point(w,p,q);
    if(!set_number(n,"x",q[0])||!set_number(n,"y",q[1])||!set_number(n,"z",q[2]))return 0;
    return !height||(number(n,"w",&h)&&set_number(n,"w",sh_grid_coordinate(w,2,h)));
}
static int transform_bounds(node *n,const sh_grid_warp *w)
{
    node *b=member(n,"b");
    return b&&transform(member(b,"b[0]"),w,0)&&transform(member(b,"b[1]"),w,0);
}
static char *unquote(const char *text)
{
    size_t n;if(!text)return NULL;n=strlen(text);
    {char *out=(char*)malloc(n+1);if(!out)return NULL;
        if(!sh_json_decode_string(text,n,out,n+1,NULL)){free(out);return NULL;}return out;}
}
static char *quote(const char *text)
{
    buffer b={0};const unsigned char *p=(const unsigned char*)text;
    if(!append(&b,"\"",1))goto failed;
    for(;*p;++p){char escaped[7];const char *s=escaped;size_t n=1;escaped[0]=(char)*p;
        if(*p=='"'||*p=='\\'){escaped[0]='\\';escaped[1]=(char)*p;n=2;}
        else if(*p=='\n'||*p=='\r'||*p=='\t'){
            escaped[0]='\\';escaped[1]=*p=='\n'?'n':*p=='\r'?'r':'t';n=2;
        }else if(*p<32)goto failed;
        if(!append(&b,s,n))goto failed;}
    if(append(&b,"\"",1))return b.s;
failed: free(b.s);return NULL;
}
static int set_string(node *n,const char *key,const char *text)
{char *q=quote(text);int ok=q&&set(n,key,q);free(q);return ok;}

static int owned_resource(sh_grid_kind kind,const char *source,const char *stock)
{
    static const char *classic_models[]={
        "models/maps/game/snapmaps/classic/connections/classic_blank_room_blueprint_floor_00.lwo",
        "models/maps/game/snapmaps/classic/connections/classic_blank_room_aas.lwo",
        "models/maps/game/snapmaps/classic/connections/classic_blank_room_clipping.lwo",
        "models/maps/game/snapmaps/classic/connections/classic_blank_room_geo.lwo"
    };
    const char *p;size_t n=strlen(stock);unsigned i;
    if(!strncmp(source,stock,n)&&source[n]=='/')return 1;
    if(!strncmp(source,"maps/modules/palettes/",22)){
        p=strchr(source+22,'/');
        if(p&&p>source+22&&!strncmp(p+1,stock+13,n-13)&&!strcmp(p+1+n-13,".bmodel"))return 1;
    }
    if(kind==SH_GRID_CLASSIC){
        for(i=0;i<sizeof classic_models/sizeof classic_models[0];++i)
            if(!strcmp(source,classic_models[i]))return 1;
    }else if(kind==SH_GRID_MODERN){
        return !strcmp(source,"models/maps/game/snapmaps/industrial_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_geo.lwo")||
            !strcmp(source,"models/maps/game/snapmaps/industrial_dlc/ind_big_blank_room_4x/ind_big_blank_room_4x_blueprint_floor_00.lwo");
    }
    return 0;
}

int sh_grid_resource_name(const sh_grid_size *size,const char *source,char *out,size_t capacity)
{
    const char *stock=sh_grid_stock_name(size?size->kind:SH_GRID_NONE);
    char name[100];int n;
    if(!stock||!source||!out||!capacity||!sh_grid_name(size,name,sizeof name))return 0;
    out[0]=0;
    if(strncmp(source,"maps/modules/",13)&&strncmp(source,"models/maps/",12))return 0;
    {const char *ext=strrchr(source,'.');
        if(!ext){size_t k=strlen(stock);
            if(strncmp(source,stock,k)||strncmp(source+k,"/volume_flight_",15))return 0;
            k+=15;if(!source[k])return 0;
            for(;source[k];++k)if(source[k]<'0'||source[k]>'9')return 0;
        }else if(strcmp(ext,".bmodel")&&strcmp(ext,".bcm")&&strcmp(ext,".lwo"))return 0;}
    if(!owned_resource(size->kind,source,stock))return 0;
    if(strstr(source,"..")||strchr(source,'\\')||source[0]=='/'||strchr(source,':'))return 0;
    n=snprintf(out,capacity,"smpgrid/%s/%s",name+strlen("maps/modules/smpgrid/"),source);
    if(n<0||(size_t)n>=capacity){out[0]=0;return 0;}return 1;
}
static int aliases(node *root,const sh_grid_size *size)
{
    node *n;
    for(n=root->children;n;n=n->next){
        if(n->children){if(!aliases(n,size))return 0;}
        else if(n->value&&n->value[0]=='"'){
            char *value=unquote(n->value),name[768];
            if(!value)return 0;
            if(sh_grid_resource_name(size,value,name,sizeof name)){
                char *q=quote(name);if(!q){free(value);return 0;}free(n->value);n->value=q;
            }free(value);
        }
    }return 1;
}
static int entity(node *entry,const sh_grid_warp *w)
{
    node *text=member(entry,"text"),*root=NULL,*edit,*inherit;
    char *decoded=NULL,*kind=NULL,*encoded=NULL;buffer output={0};int ok=0;
    float p[3],r[3],c[3]={0,0,0},np[3],nr[3],nc[3];
    if(!text||!text->value||!(decoded=unquote(text->value))||!(root=parse(decoded,strlen(decoded))))goto done;
    edit=member(root,"edit");inherit=member(root,"inherit");kind=unquote(inherit?inherit->value:NULL);
    if(!edit||!kind)goto done;
    if(member(edit,"spawnPosition")){
        if(!vec(edit,"spawnPosition",p))goto done;
        if(!strncmp(kind,"light/",6)||!strcmp(kind,"snapmaps/light/dynamic_point")){
            int have_radius=member(edit,"lightRadius")!=NULL;
            r[0]=r[1]=r[2]=0;
            if(have_radius&&!vec(edit,"lightRadius",r))goto done;
            if(member(edit,"lightCenter")&&!vec(edit,"lightCenter",c))goto done;
            if(!sh_grid_light(w,p,r,c,w->size.kind==SH_GRID_MODERN&&!strcmp(kind,"snapmaps/light/dynamic_point"),np,nr,nc)||
               !set_vec(edit,"spawnPosition",np)||(have_radius&&!set_vec(edit,"lightRadius",nr))||
               !set_vec(edit,"lightCenter",nc))goto done;
            {const char *ranges[]={"maxVisibleRange","maxShadowVisibleRange"};
                sh_grid_size stock;float scale=1,value;unsigned axis,index;
                sh_grid_default(w->size.kind,&stock);
                for(axis=0;axis<3;++axis)scale=fmaxf(scale,(float)w->size.xyz[axis]/stock.xyz[axis]);
                for(index=0;index<2;++index)if(member(edit,ranges[index])&&
                    (!number(edit,ranges[index],&value)||!set_number(edit,ranges[index],value*scale)))goto done;
            }
        }else{sh_grid_point(w,p,np);if(!set_vec(edit,"spawnPosition",np))goto done;}
    }
    if(!aliases(root,&w->size)||!emit(&output,root)||!(encoded=quote(output.s)))goto done;
    free(text->value);text->value=encoded;encoded=NULL;ok=1;
done:free(decoded);free(kind);free(encoded);free(output.s);dispose(root);return ok;
}

char *sh_grid_decl(const sh_grid_size *size,const char *source,size_t length,
                   int module_info,size_t *out_length)
{
    sh_grid_warp w;node *root=NULL,*edit,*module,*list,*n;buffer output={0};
    char name[112],map[120];
    if(out_length)*out_length=0;
    if(!sh_grid_warp_init(size,&w)||!(root=parse(source,length))||!(edit=member(root,"edit")))goto failed;
    if(module_info){
        list=member(edit,"walls");if(!list)goto failed;
        for(n=list->children;n;n=n->next)if(!strncmp(n->key,"item[",5)){
            node *edge=member(n,"edge");
            if(!edge||!transform(member(edge,"vert0"),&w,1)||!transform(member(edge,"vert1"),&w,1))goto failed;
        }
        if(!aliases(root,size))goto failed;
    }else{
        module=member(edit,"module");
        if(!module||!transform_bounds(member(module,"bounds"),&w)||!sh_grid_name(size,name,sizeof name))goto failed;
        {node *original=member(module,"name");char *old=unquote(original?original->value:NULL);
            char expected[112];int same;
            snprintf(expected,sizeof expected,"%s.map",sh_grid_stock_name(size->kind)+5);
            same=old&&!strcmp(old,expected);free(old);if(!same)goto failed;}
        snprintf(map,sizeof map,"%s.map",name+5);
        if(!set_string(module,"name",map))goto failed;
        list=member(module,"portals");if(!list)goto failed;
        for(n=list->children;n;n=n->next)if(!strncmp(n->key,"item[",5)&&!transform_bounds(member(n,"bounds"),&w))goto failed;
        list=member(module,"entities");if(!list)goto failed;
        for(n=list->children;n;n=n->next)if(!strncmp(n->key,"item[",5)&&!entity(n,&w))goto failed;
    }
    if(!emit(&output,root)||output.failed)goto failed;
    dispose(root);if(out_length)*out_length=output.n;return output.s;
failed:dispose(root);free(output.s);return NULL;
}

int sh_grid_decl_lights(const sh_grid_size *size,const char *source,size_t length,
                       sh_grid_builtin_light *out,size_t capacity,size_t *count)
{
    node *root=NULL,*list,*n;sh_grid_warp warp;size_t used=0;int ok=0;
    if(count)*count=0;
    if(!out||!count||!sh_grid_warp_init(size,&warp)||!(root=parse(source,length)))goto done;
    list=member(member(member(root,"edit"),"module"),"entities");if(!list)goto done;
    for(n=list->children;n;n=n->next)if(!strncmp(n->key,"item[",5)){
        node *t=member(n,"text"),*r=NULL,*edit,*field;char *decoded=NULL,*kind=NULL,*name=NULL;
        float origin[3],radius[3],center[3]={0,0,0};int valid=0;
        if(!t||!t->value||!(decoded=unquote(t->value))||!(r=parse(decoded,strlen(decoded))))goto item_done;
        field=member(r,"inherit");kind=unquote(field?field->value:NULL);if(!kind)goto item_done;
        if(strcmp(kind,"light/probe")&&strcmp(kind,"snapmaps/light/dynamic_point")){valid=1;goto item_done;}
        field=member(n,"name");name=unquote(field?field->value:NULL);edit=member(r,"edit");
        if(!name||!*name||used==capacity||strlen(name)>=sizeof out[0].name||
           strlen(kind)>=sizeof out[0].inherit||!vec(edit,"spawnPosition",origin)||
           !vec(edit,"lightRadius",radius)||(member(edit,"lightCenter")&&!vec(edit,"lightCenter",center)))goto item_done;
        if(!sh_grid_light(&warp,origin,radius,center,size->kind==SH_GRID_MODERN&&!strcmp(kind,"snapmaps/light/dynamic_point"),
                          out[used].origin,out[used].radius,out[used].center))goto item_done;
        if(!number(edit,"maxVisibleRange",&out[used].visible_range)||
           !number(edit,"maxShadowVisibleRange",&out[used].shadow_range))goto item_done;
        {sh_grid_size stock;float scale=1;unsigned axis;
            sh_grid_default(size->kind,&stock);
            for(axis=0;axis<3;++axis)scale=fmaxf(scale,(float)size->xyz[axis]/stock.xyz[axis]);
            out[used].visible_range*=scale;out[used].shadow_range*=scale;
        }
        memcpy(out[used].name,name,strlen(name)+1);memcpy(out[used].inherit,kind,strlen(kind)+1);++used;valid=1;
item_done:free(decoded);free(kind);free(name);dispose(r);if(!valid)goto done;
    }
    *count=used;ok=1;
done:dispose(root);return ok;
}

int sh_grid_flight_warp(const sh_grid_size *size,const char *source,size_t length,
                         const char *resource,sh_grid_warp *out)
{
    node *root=NULL,*module,*list,*n;sh_grid_warp w,result;
    char identity[768];const char *ext;size_t len;unsigned matches=0,a,k;int ok=0;
    memset(&result,0,sizeof result);
    if(!resource||!out||!sh_grid_warp_init(size,&w))return 0;
    len=strlen(resource);ext=strrchr(resource,'.');
    if(ext){if(strcmp(ext,".bcm")&&strcmp(ext,".bmodel"))return 0;len=(size_t)(ext-resource);}
    if(!len||len>=sizeof identity)return 0;
    memcpy(identity,resource,len);identity[len]=0;
    {char alias[768];if(!strstr(identity,"/volume_flight_")||
        !sh_grid_resource_name(size,identity,alias,sizeof alias))return 0;}
    root=parse(source,length);module=member(member(root,"edit"),"module");
    list=member(module,"entities");if(!list)goto done;
    for(n=list->children;n;n=n->next)if(!strncmp(n->key,"item[",5)){
        node *t=member(n,"text"),*entity_root=NULL,*edit,*name;
        char *decoded=NULL,*path=NULL,*kind=NULL;float p[3],q[3];int valid=0;
        if(!t||!t->value||!strstr(t->value,"volume/flight"))continue;
        decoded=unquote(t->value);if(!decoded)goto done;
        entity_root=parse(decoded,strlen(decoded));free(decoded);
        edit=member(entity_root,"edit");name=member(entity_root,"inherit");
        kind=unquote(name?name->value:NULL);
        name=member(member(edit,"clipModelInfo"),"clipModelName");
        path=unquote(name?name->value:NULL);
        if(kind&&path&&!strcmp(kind,"volume/flight")&&!strcmp(path,identity)){
            /* All measured stock owners omit rotation. A future rotated
             * resource needs its own verified change of basis. */
            if(!member(edit,"spawnOrientation")&&vec(edit,"spawnPosition",p)){
                sh_grid_point(&w,p,q);result=w;
                for(a=0;a<3;++a)for(k=0;k<w.axis[a].count;++k){
                    result.axis[a].source[k]-=p[a];result.axis[a].target[k]-=q[a];
                }
                valid=1;
            }
            ++matches;
        }else valid=1;
        free(kind);free(path);dispose(entity_root);
        if(!valid||matches>1)goto done;
    }
    if(matches==1){*out=result;ok=1;}
done:dispose(root);return ok;
}
