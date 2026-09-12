/* Exercise the private-palette lifecycle with native-call doubles. */
#include "../src/backend/grid_room_native.c"
#include <assert.h>

static unsigned char editor[0x20750], anchor[32], heap_state[0x100];
static void *registry_slot,*registry_object[1],*registry_vtable[12];
static unsigned char stock_records[2][0x98],stock_wrappers[2][0x98];
static unsigned char map_data[0x800];
static float environment_limit=16000;
static unsigned loads,preflights,constructors,destructors;
static unsigned catalog_loads;
static int catalog_empty=1,catalog_fail,catalog_state;
static unsigned char entity_storage[7][0x6f8];
static unsigned char *entity_slots[7];
static int commit_calls,fail_commit,replace_calls,reconnect_calls;
static int surface_state[4],surface_refreshes;
static void *surface_wrapper;
static void refresh_surfaces(void *root,void *map,void *world)
{
    int depth=*(int*)(heap_state+0xc4);
    assert(root==surface_state&&map==map_data&&world==anchor);
    assert(depth>0&&*(int*)(heap_state+0x44+(depth-1)*4)==0);
    surface_wrapper=**(void***)(map_data+0x750);++surface_refreshes;
}
typedef struct light_tree {float radius[3],center[3],visible,shadow,color[3];} light_tree;
static light_tree live_trees[7];static unsigned tree_allocs,tree_frees;
static const char *installed_light_source="{edit={module={entities={}}}}";
static int entity_id(void *body)
{int i;for(i=1;i<7;++i)if(body==entity_slots[i]+8)return i;assert(0);return -1;}
static void **get_tree(void *body,void **out)
{*out=malloc(sizeof(light_tree));assert(*out);memcpy(*out,&live_trees[entity_id(body)],sizeof(light_tree));++tree_allocs;return out;}
static void apply_tree(void *body,void *tree)
{live_trees[entity_id(body)]=*(light_tree*)tree;}
static void read_properties(void *body,void *tree)
{memcpy((char*)body+0xa0,((light_tree*)tree)->radius,12);memcpy((char*)body+0xac,((light_tree*)tree)->center,12);}
static unsigned char set_tree_vec(void *tree,const char *path,const float *value,void *reflect)
{assert(reflect==editor);if(!strcmp(path,"lightRadius"))memcpy(((light_tree*)tree)->radius,value,12);
 else {assert(!strcmp(path,"lightCenter"));memcpy(((light_tree*)tree)->center,value,12);}return 1;}
static unsigned char set_tree_float(void *tree,const char *path,float value,void *reflect)
{assert(reflect==editor);if(!strcmp(path,"maxVisibleRange"))((light_tree*)tree)->visible=value;
 else {assert(!strcmp(path,"maxShadowVisibleRange"));((light_tree*)tree)->shadow=value;}return 1;}
static void tree_destroy(void *tree){assert(tree);}
static void tree_free(void *tree,size_t size){assert(size==0x48);++tree_frees;free(tree);}
static void *edit_entity(void *edit,int id)
{(void)edit;assert(id>=1&&id<7);++commit_calls;
 if(commit_calls==fail_commit)return NULL;return entity_slots[id]+8;}
static void set_transform(void *entity,const float *transform)
{memcpy((unsigned char*)entity+0x288,transform,48);}
static void replace_instance(void *map,unsigned instance,const void *record)
{assert(map==map_data&&instance<(unsigned)*(int*)(map_data+0x758));memcpy(*(unsigned char**)(map_data+0x750)+instance*0x98,record,0x98);++replace_calls;}
static void reconnect(void *map);
static int throw_dtor;
static void *allocations[512];
static unsigned allocation_count;
static void *allocate(size_t n)
{void *p=calloc(1,n);assert(p&&allocation_count<512);allocations[allocation_count++]=p;return p;}
static void geometry(unsigned char *module,const sh_grid_size *size)
{
    float *bounds=(float*)(module+0xa0),*portals=allocate(2*28);
    bounds[0]=-(float)size->xyz[0]/2;bounds[1]=1280-(float)size->xyz[1]/2;bounds[2]=0;
    bounds[3]=-bounds[0];bounds[4]=2560-bounds[1];bounds[5]=(float)size->xyz[2];
    portals[0]=portals[7]=-192;portals[3]=portals[10]=192;
    portals[1]=portals[4]=bounds[4];portals[8]=portals[11]=bounds[1];
    portals[5]=portals[12]=352;*(void**)(module+0xe8)=portals;*(int*)(module+0xf0)=2;
}
static float *world_bounds(void *record,float *out,const float *bounds)
{
    unsigned char *r=record;int k,rotation=*(int*)(r+0x18);float p[3],q[3];
    memcpy(p,bounds,12);memcpy(q,bounds+3,12);
    if(rotation==1){float x=p[0];p[0]=-p[1];p[1]=x;x=q[0];q[0]=-q[1];q[1]=x;}
    else assert(rotation==0);
    for(k=0;k<3;++k){float origin=*(float*)(r+0xc+4*k);
        out[k]=fminf(p[k],q[k])+origin;out[k+3]=fmaxf(p[k],q[k])+origin;}
    return out;
}
static float *portal_bounds(void *record,float *out,int door)
{unsigned char *module=**(unsigned char***)record;assert(door>=0&&door<2);
 return world_bounds(record,out,*(float**)(module+0xe8)+door*7);}
static int containment_result=-1,ray_calls,ray_up,ray_down;
static int native_containment(void *collision,const float *point)
{(void)collision;(void)point;return containment_result;}
static grid_ray_hit *native_ray(void *collision,grid_ray_hit *out,const float *point,
    const float *end,int ignore,void *records,int count)
{
    assert(collision==editor+0x20550&&ignore==-1&&records==editor&&count==2);
    assert(end[0]==point[0]&&end[1]==point[1]&&fabsf(end[2]-point[2])>=10016);
    ++ray_calls;out->module=end[2]>point[2]?ray_up:ray_down;return out;
}
static void containment_checks(void)
{
    unsigned char records[2][0x98]={0};float point[]={1000,1280,256};
    sh_grid_size huge={SH_GRID_MODERN,{10000,10000,10000}};
    void *saved=*(void**)(editor+0x204c8);
    g_containing_module=native_containment;g_module_ray=native_ray;
    *(void**)records[0]=sh_grid_native_wrapper(&huge);records[0][0x30]=1;
    *(void**)records[1]=stock_wrappers[1];records[1][0x30]=1;
    *(void**)(map_data+0x750)=records;*(int*)(map_data+0x758)=2;
    *(void**)(editor+0x204c8)=map_data;
    *(void**)(editor+0x20550)=editor;*(int*)(editor+0x20558)=2;
    ray_up=ray_down=0;ray_calls=0;
    assert(grid_containing_module(editor+0x20550,point)==0&&ray_calls==2);
    point[2]=9500;assert(grid_containing_module(editor+0x20550,point)==0&&ray_calls==4);
    containment_result=1;assert(grid_containing_module(editor+0x20550,point)==1&&ray_calls==4);
    containment_result=-1;ray_down=1;
    assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==6);
    ray_up=-1;assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    ray_up=ray_down=0;point[2]=10256;
    assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    point[2]=-256;assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    point[2]=256;point[0]=6000;
    assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    point[0]=NAN;assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    point[0]=1000;records[0][0x30]=0;
    assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    records[0][0x30]=1;*(void**)records[0]=stock_wrappers[1];
    assert(grid_containing_module(editor+0x20550,point)==-1&&ray_calls==7);
    assert(grid_containing_module(editor,point)==-1&&ray_calls==7);
    *(void**)(editor+0x204c8)=saved;
}
static void reconnect(void *map)
{
    int *keys=*(int**)(map_data+0x7d8),*links=*(int**)(map_data+0x7c0);
    unsigned char *records=*(unsigned char**)(map_data+0x750);int i,j,a,b,n=*(int*)(map_data+0x758);
    assert(map==map_data);++reconnect_calls;
    for(i=0;i<n;++i){float bounds[6];int k;unsigned char *r=records+i*0x98;
        world_bounds(r,bounds,(float*)(**(unsigned char***)r+0xa0));r[0x30]=1;
        for(k=0;k<3;++k)if(bounds[k]<-environment_limit||bounds[k+3]>environment_limit)r[0x30]=0;
    }
    for(i=0;i<keys[n];++i)links[i]=-1;
    for(i=0;i<n;++i)for(j=i+1;j<n;++j)for(a=0;a<2;++a)for(b=0;b<2;++b){float p[6],q[6];
        portal_bounds(records+i*0x98,p,a);portal_bounds(records+j*0x98,q,b);
        if(!memcmp(p,q,sizeof p)){links[keys[i]+a]=keys[j]+b;links[keys[j]+b]=keys[i]+a;}
    }
}
static void layout_checks(void)
{
    unsigned char local_map[0x800]={0},records[3][0x98]={0};grid_move moves[3];
    int keys[]={0,2,4,6},links[]={3,-1,-1,0,-1,-1};const char *reason="unknown";
    sh_grid_size huge={SH_GRID_MODERN,{10000,10000,10000}};void *wrapper=sh_grid_native_wrapper(&huge);
    int i;
    *(void**)(local_map+0x750)=records;*(void**)(local_map+0x7d8)=keys;*(int*)(local_map+0x7e0)=4;
    *(int*)(local_map+0x758)=3;*(void**)(local_map+0x7c0)=links;*(int*)(local_map+0x7c8)=6;
    for(i=0;i<3;++i)*(void**)records[i]=stock_wrappers[1];
    *(float*)(records[1]+0x10)=5120;*(float*)(records[2]+0xc)=20000;
    memset(moves,0,sizeof moves);assert(plan_connected(local_map,0,wrapper,moves,3,links,6,&reason));
    assert(*(float*)(moves[1].after+0x10)==7560&&!memcmp(moves[2].after,records[2],0x98));
    /* Rotating the same assembly rotates its branch translation, preserving
     * the original portal planes without changing either room's rotation. */
    *(int*)(records[0]+0x18)=*(int*)(records[1]+0x18)=1;
    *(float*)(records[1]+0x10)=0;*(float*)(records[1]+0xc)=-5120;
    memset(moves,0,sizeof moves);assert(plan_connected(local_map,0,wrapper,moves,3,links,6,&reason));
    assert(*(float*)(moves[1].after+0xc)==-7560&&*(int*)(moves[1].after+0x18)==1);
    *(int*)(records[0]+0x18)=*(int*)(records[1]+0x18)=0;
    *(float*)(records[1]+0x10)=5120;*(float*)(records[1]+0xc)=0;
    *(float*)(records[2]+0xc)=0;*(float*)(records[2]+0x10)=12000;
    memset(moves,0,sizeof moves);assert(!plan_connected(local_map,0,wrapper,moves,3,links,6,&reason));
    assert(strstr(reason,"overlap"));
    /* A second constraint back into an assigned branch cannot silently pull
     * the root away or break an existing edge. */
    links[1]=2;links[2]=1;
    memset(moves,0,sizeof moves);assert(!plan_connected(local_map,0,wrapper,moves,3,links,6,&reason));
    assert(strstr(reason,"loop"));
    /* A huge room near the south world edge used to become invalid while
     * retaining reciprocal doors, then crash native render construction. */
    links[1]=links[2]=-1;
    {sh_grid_size before,requested={SH_GRID_MODERN,{16384,16384,16384}};unsigned prior_loads=loads;
        sh_grid_default(SH_GRID_MODERN,&before);
        *(float*)(records[0]+0xc)=7296;*(float*)(records[0]+0x10)=-10504;
        *(float*)(records[1]+0xc)=7296;*(float*)(records[1]+0x10)=-5384;
        assert(fit_size(local_map,0,&before,&requested,&reason));
        assert(requested.xyz[0]==16384&&requested.xyz[1]==13552&&requested.xyz[2]==16000);
        assert(loads==prior_loads); /* Spatial fitting must not consume asset variants. */
        memset(moves,0,sizeof moves);
        assert(plan_size(local_map,0,&before,&requested,moves,3,links,6,&reason));
        assert(inside_environment(moves,3)&&moves[0].bounds[1]==-16000);
        /* The current cvar is read for each edit, with no 16384 size cap. */
        environment_limit=30000;
        requested.xyz[0]=requested.xyz[1]=requested.xyz[2]=30000;
        assert(fit_size(local_map,0,&before,&requested,&reason));
        assert(requested.xyz[0]==30000&&requested.xyz[1]==30000&&requested.xyz[2]==30000);
        environment_limit=16000;
        assert(fit_size(local_map,0,&before,&requested,&reason));
        assert(requested.xyz[0]==17408&&requested.xyz[1]==13552&&requested.xyz[2]==16000);
        /* The boundary of a moved neighbor can be tighter than the root's. */
        *(float*)(records[0]+0xc)=*(float*)(records[1]+0xc)=0;
        *(float*)(records[0]+0x10)=3000;*(float*)(records[1]+0x10)=8120;
        requested=huge;requested.xyz[1]=16384;
        assert(fit_size(local_map,0,&before,&requested,&reason)&&requested.xyz[1]==13200);
    }
}
void backend_log(const char *s){fprintf(stderr,"%s\n",s);}
uintptr_t glb_resolve(const uint8_t *b,const char *n,glb_status *info)
{(void)b;(void)n;(void)info;return 0;}
unsigned char *sh_overrides_read_engine_resource(const char *n,size_t *len)
{unsigned char *p;assert(strstr(n,"ind_totally_blank_room_4x.decl"));*len=strlen(installed_light_source);
 p=HeapAlloc(GetProcessHeap(),0,*len);assert(p);memcpy(p,installed_light_source,*len);return p;}
void *sh_typeinfo_get_reflect(void){return editor;}
int sh_grid_asset_open(const char *name,sh_grid_asset_read_fn read,
    sh_grid_asset_release_fn release,void *context,unsigned char **out,size_t *len)
{
    (void)read;(void)release;(void)context;
    assert(strstr(name,"smpgrid/v1/"));++preflights;
    *out=malloc(1);assert(*out);**out=0;*len=1;return 1;
}
static void *heap_get(void){return heap_state;}
static unsigned char collision_resources[2][0x40];
static unsigned char build_collision(void *collision,void *map,int instance)
{
    unsigned char *record=*(unsigned char**)((unsigned char*)map+0x750)+(size_t)instance*0x98;
    unsigned char *wrapper=*(unsigned char**)record;
    assert(collision==editor);
    if(wrapper==stock_wrappers[1]){assert(*(int*)(heap_state+0xc4)==0);return 1;}
    assert(*(int*)(heap_state+0xc4)==1);
    *(void**)(wrapper+0x68)=collision_resources[0];*(void**)(wrapper+0x70)=collision_resources[1];
    return 1;
}
static void heap_push(void *p,int which)
{int *d=(int*)((char*)p+0xc4);assert(which==0);*(int*)((char*)p+0x44+4*(*d)++)=which;}
static void heap_pop(void *p){int *d=(int*)((char*)p+0xc4);assert(*d>0);--*d;}
static void *string_ctor(void *p,const char *s)
{char *copy=_strdup(s);assert(copy);memset(p,0,48);*(char**)((char*)p+16)=copy;++constructors;return p;}
static void string_dtor(void *p)
{free(*(void**)((char*)p+16));++destructors;if(throw_dtor){throw_dtor=0;RaiseException(0xe0001234,0,0,NULL);}}
static void *get_type(void *r,const char *name)
{assert(r==registry_object&&!strcmp(name,"snapModuleInfo"));return registry_object;}
static void *find_source(void *r,const char *name)
{assert(r==registry_object&&strstr(name,"smpgrid/v1/"));return registry_object;}
static unsigned char register_file(void *r,const void *name,void *unused)
{(void)r;(void)name;(void)unused;assert(0);return 0;}
static void *find_decl(void *r,const char *name,unsigned char create)
{unsigned char *p;assert(r==registry_object&&name);
 if(!create)return NULL;
 p=allocate(0x240);p[0x2c]=(unsigned char)catalog_state;
 if(!catalog_empty){*(int*)(p+0x118)=1;*(int*)(p+0x130)=4;}return p;}
static void generic_load(void *decl)
{unsigned char *p=decl;assert(*(unsigned*)(p+0x28)==4&&!(p[0x2c]&3));++catalog_loads;
 if(!catalog_fail){p[0x2c]|=4;*(int*)(p+0x118)=1;*(int*)(p+0x130)=4;}}
static void *find_stock(void *palette,const char *name)
{
    sh_grid_kind kind=sh_grid_stock_kind(name);
    assert(palette==editor+0x206c0);
    if(!kind)return end_record(palette);
    return stock_records[kind==SH_GRID_CLASSIC?0:1];
}
static unsigned char load_variant(void *palette,void *palette_decl,void *palette_name,
                                  void *module_name,void *entity_palette)
{
    unsigned char *p=palette,*record=allocate(0x98),*wrapper=allocate(0x98),*module=allocate(0x140);
    char *name=*(char**)((char*)module_name+16);char *copy=allocate(strlen(name)+1);
    assert(palette_decl==editor&&entity_palette==editor+0x20660);
    assert(!strcmp(*(char**)((char*)palette_name+16),"mega_blessed"));
    assert(*(unsigned*)(p+0x20)==0x50000&&*(unsigned*)(p+0x38)==0x50000);
    assert(*(unsigned*)(p+0x60)==0x50000&&*(unsigned*)(p+0x78)==0x50000);
    assert(*(int*)(heap_state+0xc4)>=1&&*(int*)(heap_state+0xc4)<=2);strcpy(copy,name);
    *(void**)record=wrapper;*(void**)wrapper=module;*(void**)(module+8)=copy;
    {sh_grid_size size;assert(sh_grid_parse_name(name,&size));geometry(module,&size);}
    *(void**)(wrapper+0x80)=allocate(0x240);*(void**)(wrapper+0x88)=allocate(0x80);
    *(void**)(p+0x10)=record;*(int*)(p+0x18)=1;
    *(void**)(p+0x68)=wrapper;*(int*)(p+0x70)=1;++loads;return 1;
}
int main(void)
{
    sh_grid_size compact={SH_GRID_MODERN,{1536,1024,512}},large={SH_GRID_MODERN,{8192,6144,4096}},read;
    void *a,*b,*again;void *edit_data[1]={map_data};unsigned i;int32_t displacement;
    editor[8]=1;g_editor=editor;g_environment_bounds=&environment_limit;
    *(void**)(editor+0x206c0)=editor;
    *(void**)(editor+0x206d0)=stock_records;*(int*)(editor+0x206d8)=2;
    for(i=0;i<2;++i){
        unsigned char *module=allocate(0x140);
        *(void**)stock_records[i]=stock_wrappers[i];*(void**)(stock_wrappers[i]+0x90)=editor;
        *(void**)stock_wrappers[i]=module;
        {sh_grid_size size;sh_grid_default(SH_GRID_MODERN,&size);geometry(module,&size);}
        *(const char**)(module+8)=sh_grid_stock_name(i?SH_GRID_MODERN:SH_GRID_CLASSIC);
        *(const char**)(stock_wrappers[i]+0x18)=i?
            "maps/modules/palettes/mega_blessed/ind_dlc/ind_totally_blank_room_4x.bmodel":
            "maps/modules/palettes/mega_blessed/classic/classic_blank_room.bmodel";
    }
    g_find=find_stock;g_load=load_variant;g_ctor=string_ctor;g_dtor=string_dtor;
    g_type=get_type;g_register=register_file;g_source=find_source;g_decl=find_decl;
    g_generic_load=generic_load;
    g_heap.get=heap_get;g_heap.push=heap_push;g_heap.pop=heap_pop;
    registry_slot=registry_object;registry_object[0]=registry_vtable;
    registry_vtable[7]=(void*)register_file;registry_vtable[11]=(void*)get_type;
    g_anchor=anchor;memcpy(anchor+0x10,"\x48\x8b\x0d",3);
    assert((intptr_t)&registry_slot-(intptr_t)(anchor+0x17)>=INT32_MIN);
    assert((intptr_t)&registry_slot-(intptr_t)(anchor+0x17)<=INT32_MAX);
    displacement=(int32_t)((intptr_t)&registry_slot-(intptr_t)(anchor+0x17));
    memcpy(anchor+0x13,&displacement,4);
    a=sh_grid_native_wrapper(&compact);b=sh_grid_native_wrapper(&large);again=sh_grid_native_wrapper(&compact);
    assert(a&&b&&a!=b&&a==again&&loads==2&&preflights==14);
    assert(catalog_loads==2);
    assert(has_variant(editor+0x206c0,"maps/modules/smpgrid/v1/m/1536_1024_512.decl"));
    assert(has_variant(editor+0x206c0,sh_grid_stock_name(SH_GRID_MODERN)));
    assert(!has_variant(editor+0x206c0,"unknown/module.decl"));
    /* A cold saved variant is admitted during map load, before OnActivate. */
    {sh_grid_size cold={SH_GRID_MODERN,{900,700,3392}};void *loaded;
        editor[8]=0;loaded=sh_grid_native_wrapper(&cold);assert(loaded);
        assert(has_variant(editor+0x206c0,"maps/modules/smpgrid/v1/m/900_700_3392.decl"));
        editor[8]=1;assert(loads==3&&catalog_loads==3);}
    /* Already parsed catalogs are never torn down. Empty in-progress objects
     * and source loads that still lack geometry must not reach the loader. */
    catalog_empty=0;assert(catalog("smpgrid/v1/m/1536_1024_512")&&catalog_loads==3);
    catalog_empty=1;catalog_state=1;
    assert(!catalog("smpgrid/v1/m/1536_1024_512")&&catalog_loads==3);
    catalog_state=2;catalog_fail=1;
    assert(!catalog("smpgrid/v1/m/1536_1024_512")&&catalog_loads==4);
    catalog_state=0;catalog_fail=0;
    assert(*(int*)(heap_state+0xc4)==0&&constructors==destructors);
    assert(*(unsigned*)((char*)*(void**)a+0x28)==4);
    assert(*(unsigned*)((char*)*(void**)b+0x28)==4);
    {unsigned char placed[2][0x98]={0};
        *(void**)placed[0]=a;*(void**)placed[1]=b;
        *(void**)(map_data+0x750)=placed;*(int*)(map_data+0x758)=2;
        g_build_collision=build_collision;
        *(unsigned*)(collision_resources[0]+0x28)=*(unsigned*)(collision_resources[1]+0x28)=1;
        assert(grid_build_collision(editor,map_data,0));
        assert(*(unsigned*)(collision_resources[0]+0x28)==4&&*(unsigned*)(collision_resources[1]+0x28)==4);
        assert(*(int*)(heap_state+0xc4)==0);
        *(void**)placed[1]=stock_wrappers[1];assert(grid_build_collision(editor,map_data,1));
        assert(!*(void**)(stock_wrappers[1]+0x68)&&!*(void**)(stock_wrappers[1]+0x70));
        *(void**)placed[1]=b;
        assert(sh_grid_native_read(edit_data,0,&read)&&!memcmp(&read,&compact,sizeof read));
        assert(sh_grid_native_read(edit_data,1,&read)&&!memcmp(&read,&large,sizeof read));
        assert(!sh_grid_native_read(edit_data,-1,&read)&&!sh_grid_native_read(edit_data,2,&read));
        {int portal_keys[]={0,2,4},doors[]={1,2,-1,-1},frames[]={-1,-1,-1,-1};
            int connected[]={-1,-1,-1,-1},entity_keys[]={0,2,2,2},entities[]={1,2,3,4,5};
            sh_grid_size target={SH_GRID_MODERN,{1536,2048,768}};float saved_caps[2][12];
            float *cap0=(float*)(entity_storage[1]+0x290),*cap1=(float*)(entity_storage[2]+0x290);
            unsigned char untouched[0x98];void *resized;
            g_replace=replace_instance;g_reconnect=reconnect;g_edit_entity=edit_entity;g_set_transform=set_transform;
            g_refresh_surfaces=refresh_surfaces;surface_state[2]=2;
            *(void**)(editor+0x20548)=surface_state;*(void**)(editor+0x198)=anchor;
            g_world_bounds=world_bounds;g_portal_bounds=portal_bounds;
            for(i=1;i<7;++i){entity_slots[i]=entity_storage[i];*(void**)(entity_slots[i]+8)=editor;}
            *(void**)(map_data+0x6a0)=entity_slots;*(int*)(map_data+0x6a8)=7;
            *(void**)(editor+0x204c8)=map_data;*(void**)(editor+0x204d0)=edit_data;
            *(void**)(map_data+0x7d8)=portal_keys;*(int*)(map_data+0x7e0)=3;
            *(void**)(map_data+0x778)=doors;*(int*)(map_data+0x780)=4;
            *(void**)(map_data+0x790)=frames;*(int*)(map_data+0x798)=4;
            *(void**)(map_data+0x7c0)=connected;*(int*)(map_data+0x7c8)=4;
            *(void**)(map_data+0x720)=entity_keys;*(int*)(map_data+0x728)=4;
            *(void**)(map_data+0x708)=entities;*(int*)(map_data+0x710)=2;
            /* No serialized state or defsub exists here. Live transforms must
             * work after native cap replacement, even when edit is null. */
            cap0[1]=1792;cap1[1]=768;cap0[3]=cap0[7]=cap0[11]=1;
            cap1[3]=cap1[7]=cap1[11]=1;
            *(float*)(placed[1]+0xc)=30000;*(float*)(placed[1]+0x10)=30000;
            memset(placed[1]+0x40,0xa5,0x58);memcpy(untouched,placed[1],sizeof untouched);
            connected[0]=2;assert(!sh_grid_native_apply(edit_data,0,&target)&&!commit_calls&&!replace_calls);
            assert(!surface_refreshes);
            connected[0]=-1;
            assert(sh_grid_native_apply(edit_data,0,&target));resized=*(void**)placed[0];
            assert(surface_refreshes==1&&surface_wrapper==resized);
            assert(sh_grid_native_apply(edit_data,0,&target)&&surface_refreshes==1);
            assert(resized!=a&&resized!=b&&!memcmp(placed[1],untouched,sizeof untouched));
            assert(sh_grid_native_read(edit_data,0,&read)&&!memcmp(&read,&target,sizeof target));
            assert(cap0[1]==2304&&cap1[1]==256);
            assert(cap0[3]==1&&cap0[7]==1&&cap0[11]==1);
            memcpy(saved_caps[0],cap0,48);memcpy(saved_caps[1],cap1,48);fail_commit=commit_calls+2;
            assert(!sh_grid_native_apply(edit_data,0,&large));
            assert(surface_refreshes==1&&surface_wrapper==resized);
            assert(*(void**)placed[0]==resized&&!memcmp(saved_caps[0],cap0,48)&&!memcmp(saved_caps[1],cap1,48));
            assert(!memcmp(placed[1],untouched,sizeof untouched)&&!g_edit_faulted);
            assert(replace_calls==2&&reconnect_calls==2&&*(int*)(heap_state+0xc4)==0);
            /* A cancel back to stock dimensions also restores moduleName,
             * rather than leaving a same-size generated resource dependency. */
            {sh_grid_size original;unsigned previous_loads=loads;
                sh_grid_default(SH_GRID_MODERN,&original);fail_commit=0;
                assert(sh_grid_native_apply(edit_data,0,&original));
                assert(*(void**)placed[0]==stock_wrappers[1]&&loads==previous_loads);
                assert(sh_grid_native_read(edit_data,0,&read)&&!memcmp(&read,&original,sizeof read));
                assert(cap0[1]==3840&&cap1[1]==-1280);
                assert(!memcmp(placed[1],untouched,sizeof untouched));
            }
            /* A connected neighbor follows the enlarged north doorway. Its
             * dimensions and all local entity transforms stay unchanged. */
            {sh_grid_size huge={SH_GRID_MODERN,{10000,10000,10000}};float own[12];
                *(void**)placed[1]=stock_wrappers[1];*(float*)(placed[1]+0xc)=0;*(float*)(placed[1]+0x10)=5120;
                connected[0]=3;connected[3]=0;memcpy(own,entity_storage[3]+0x290,48);
                assert(sh_grid_native_apply(edit_data,0,&huge));
                assert(*(float*)(placed[1]+0x10)==7560&&*(void**)placed[1]==stock_wrappers[1]);
                assert(cap0[1]==6280&&cap1[1]==-3720);
                assert(!memcmp(own,entity_storage[3]+0x290,48));
                assert(sh_grid_native_read(edit_data,0,&read)&&!memcmp(&read,&huge,sizeof read));
                connected[0]=connected[3]=-1;
                assert(sh_grid_native_apply(edit_data,0,&target));
                assert(cap0[1]==2304&&cap1[1]==256);
                assert(*(float*)(placed[1]+0x10)==7560);
            }
            {unsigned char definitions[3][0x80]={0};float before_light[12],authored[12];
                sh_grid_size huge={SH_GRID_MODERN,{10000,10000,10000}};
                light_tree saved;float *pos=(float*)(entity_storage[3]+0x290);
                installed_light_source="{edit={module={entities={item[0]={name=\"room_light\";"
                    "text=\"{inherit=\\\"snapmaps/light/dynamic_point\\\";edit={spawnPosition={y=1280;z=1000;}"
                    "lightRadius={x=2000;y=2000;z=2500;}lightCenter={z=2000;}maxVisibleRange=10000;maxShadowVisibleRange=6000;}}\";}}}}}";
                g_entity_tree=get_tree;g_apply_tree=apply_tree;g_read_properties=read_properties;
                g_tree_vec=set_tree_vec;g_tree_float=set_tree_float;g_tree_destroy=tree_destroy;g_tree_free=tree_free;
                for(i=0;i<3;++i){*(void**)(entity_storage[i+3]+0x158)=definitions[i];
                    *(const char**)(definitions[i]+8)=i==1?"authored_light":"room_light";
                    *(const char**)(definitions[i]+0x58)="snapmaps/light/dynamic_point";}
                entity_keys[1]=4;entity_keys[2]=5;*(int*)(map_data+0x710)=5;
                *(float*)(placed[1]+0xc)=30000;
                pos[1]=1280;pos[2]=1000;pos[3]=pos[7]=pos[11]=1;
                memcpy(before_light,pos,48);memcpy(authored,entity_storage[4]+0x290,48);
                live_trees[3].color[0]=.7f;live_trees[3].radius[0]=123;
                {sh_grid_builtin_light fields[8];size_t light_total=0;
                    assert(sh_grid_decl_lights(&huge,installed_light_source,strlen(installed_light_source),fields,8,&light_total));
                    assert(light_total==1);}
                saved=live_trees[3];fail_commit=commit_calls+3;
                assert(!sh_grid_native_apply(edit_data,0,&huge));
                assert(!memcmp(pos,before_light,48)&&!memcmp(&live_trees[3],&saved,sizeof saved));
                fail_commit=0;assert(sh_grid_native_apply(edit_data,0,&huge));
                assert(pos[2]>2000&&live_trees[3].radius[0]>3000&&live_trees[3].visible>20000);
                assert(live_trees[3].shadow>12000&&live_trees[3].color[0]==.7f);
                assert(!memcmp(authored,entity_storage[4]+0x290,48)&&!live_trees[5].visible);
                assert(tree_allocs==tree_frees);
                /* A duplicate original identity cannot authorize editing an
                 * authored clone in the same room. Refuse before mutation. */
                entity_keys[1]=5;assert(!sh_grid_native_apply(edit_data,0,&target));
                assert(tree_allocs==tree_frees);
                installed_light_source="{edit={module={entities={}}}}";
                for(i=3;i<6;++i)*(void**)(entity_storage[i]+0x158)=NULL;
            }
        }
    }
    layout_checks();
    containment_checks();
    assert(find_variant(editor+0x206c0,"unrelated.decl")==end_record(editor+0x206c0));
    /* A native string destructor fault must still restore the heap scope. */
    compact.xyz[0]=1600;throw_dtor=1;assert(!sh_grid_native_wrapper(&compact));
    assert(g_faulted&&*(int*)(heap_state+0xc4)==0&&constructors==destructors);
    for(i=0;i<allocation_count;++i)free(allocations[i]);
    puts("grid_room_native_test: passed");return 0;
}
