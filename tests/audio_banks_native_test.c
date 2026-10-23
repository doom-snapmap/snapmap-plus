#include <stdio.h>
#include <string.h>
#include "../src/backend/audio_bank_commands.c"
#include "../src/backend/audio_limits_native.c"
#include "../src/backend/audio_environment_native.c"
#include "../src/backend/audio_state_native.c"
#include "../src/backend/audio_parameter_native.c"
#include "../src/backend/audio_subscription_native.c"
#include "../src/backend/audio_switch_native.c"
#include "../src/backend/audio_bus_native.c"
#include "../src/backend/audio_bank_gate.c"
#include "../src/backend/audio_banks_native.c"

static int failures, loads, releases;
static unsigned char loaded[5], desired[5], actual[5];
static int fail_load_call, original_locale, original_failed, paths_failed;
static int originals_ready, packaged_failed;
static uint32_t packaged_identity;
static const char *shadowed_path;      /* engine path the packages shadow */
#define BANK_MAX 192
static unsigned char shadow[BANK_MAX];
static size_t shadow_length;
static char language_name[64] = "English(US)";
#define BANK_A UINT32_C(84696446)      /* a.bnk */
#define BANK_B UINT32_C(84696445)      /* b.bnk */
#define BANK_C UINT32_C(84696444)      /* c.bnk */
#define BANK_N UINT32_C(777)           /* 777.bnk, named by its own identity */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)
static char root[MAX_PATH];

static char logged[4096];
void backend_log(const char *text)
{
    size_t used=strlen(logged), length=strlen(text);
    if(used+length+2<sizeof(logged)) {
        memcpy(logged+used,text,length); logged[used+length]=10; logged[used+length+1]=0;
    }
}
uintptr_t sig_addr_by_name(const sig_result *r,size_t n,const char *name)
{ (void)r; (void)n; (void)name; return 0; }

static unsigned slot_of(uint32_t id)
{ return id==BANK_A?1:id==BANK_B?2:id==BANK_C?3:id==BANK_N?4:0; }
static uint32_t id_of(const char *name)
{
    uint32_t id=0;
    CHECK(sh_audio_bank_identity(name,&id)); return id;
}
static int native_load(const char *name,int pool,uint32_t *id)
{
    unsigned index;
    CHECK(pool==-1); *id=id_of(name); index=slot_of(*id); CHECK(index); ++loads;
    if(loads==fail_load_call)return 2;
    if(loaded[index]) return 69;
    loaded[index]=1; actual[index]=desired[index]; return 1;
}
static int numeric_result;
static int numeric_init(void *manager,abc_completion *completion)
{ (void)manager; (void)completion; return 1; }
static int numeric_submit(void *manager,const abc_command *command)
{
    unsigned index=slot_of(command->id);
    (void)manager;
    CHECK(command->operation==0 && command->mode==1 && command->pool==-1);
    CHECK(!command->memory && !command->length && index && !command->callback);
    ++loads;
    numeric_result=loads==fail_load_call?2:loaded[index]?69:1;
    if(numeric_result==1) { loaded[index]=1; actual[index]=desired[index]; }
    return 1;
}
static int numeric_wait(void *manager,abc_completion *completion,int submitted)
{ (void)manager; (void)completion; return submitted==1?numeric_result:submitted; }
static void *numeric_vtable[6]={NULL,NULL,NULL,(void *)numeric_submit,(void *)numeric_init,(void *)numeric_wait};
static void **numeric_object=numeric_vtable;
static void *numeric_manager=&numeric_object, *numeric_callback;
static int native_unload(uint32_t id,const void *memory,int *pool)
{
    unsigned index=slot_of(id);
    CHECK(!memory && !pool && index && loaded[index]); ++releases;
    loaded[index]=0; return 1;
}
int sh_audio_files_native_bank_paths(const char *name,char **localized,char **fallback)
{
    char path[512];*localized=NULL;*fallback=NULL;
    if(paths_failed)return 0;
    snprintf(path,sizeof(path),"sound/soundbanks/pc/english(us)/%s",name);*localized=_strdup(path);
    snprintf(path,sizeof(path),"sound/soundbanks/pc/%s",name);*fallback=_strdup(path);
    return *localized && *fallback;
}
static char prefix_name[64];
int sh_audio_files_native_bank_prefix(char *out,size_t capacity)
{
    size_t length=strlen(prefix_name);
    if(!out || !capacity || length+1>capacity)return 0;
    memcpy(out,prefix_name,length+1); return 1;
}
int sh_audio_files_native_language(wchar_t *out,size_t capacity)
{
    size_t length=strlen(language_name);
    if(!length || length+1>capacity)return 0;
    for(size_t i=0;i<=length;i++)out[i]=(wchar_t)(unsigned char)language_name[i];
    return 1;
}
int sh_package_runtime_audio_original(const char *path,sh_package_original_identity *out,
    char *error,size_t capacity)
{
    CHECK(strstr(path,"/english(us)/"));memset(out,0,sizeof(*out));
    if(original_failed){snprintf(error,capacity,"original read failed");return -1;}
    out->scope=original_locale;return 1;
}
int sh_package_runtime_audio_originals_ready(void) { return originals_ready; }
/* Serve the effective installed original: the packed shadow when one is
 * registered for the path, otherwise the loose file under the fixture root. */
int sh_package_runtime_audio_original_read(const char *path,uint64_t offset,void *out,
    size_t span,uint64_t *length,char *error,size_t capacity)
{
    static const char prefix[]="sound/soundbanks/pc/";
    char absolute[MAX_PATH];FILE *stream=NULL;__int64 size=0;int ok;
    if(length)*length=0;
    CHECK(originals_ready);
    if(strncmp(path,prefix,sizeof(prefix)-1))return 0;
    if(shadowed_path && !strcmp(path,shadowed_path)) {
        if(length)*length=shadow_length;
        if(offset>shadow_length || span>shadow_length-offset) {
            snprintf(error,capacity,"packed original read is out of range");return -1;
        }
        if(span)memcpy(out,shadow+(size_t)offset,span);
        return 1;
    }
    snprintf(absolute,sizeof(absolute),"%s\\%s",root,path+sizeof(prefix)-1);
    for(char *p=absolute;*p;p++)if(*p=='/')*p='\\';
    if(fopen_s(&stream,absolute,"rb") || !stream)return 0;
    ok=!_fseeki64(stream,0,SEEK_END) && (size=_ftelli64(stream))>=0;
    if(ok && length)*length=(uint64_t)size;
    if(ok && span)ok=!_fseeki64(stream,(__int64)offset,SEEK_SET) && fread(out,1,span,stream)==span;
    fclose(stream);
    if(!ok)snprintf(error,capacity,"installed audio original is unreadable");
    return ok?1:-1;
}
int sh_package_runtime_audio_packaged_banks(sh_audio_originals_bank_visit visit,void *visitor,
    char *error,size_t capacity)
{
    if(packaged_failed){snprintf(error,capacity,"package metadata unreadable");return -1;}
    if(packaged_identity)visit(visitor,packaged_identity,0);
    return 1;
}
int sh_package_runtime_audio_original_banks(const char *language,const char *prefix,
    sh_audio_originals_inventory_visit visit,void *context,char *error,size_t capacity)
{
    an_names names={0};sh_audio_original_bank *rows=NULL;size_t count=0;int ok=0;
    char directory[MAX_PATH];
    CHECK(originals_ready && !_stricmp(language,language_name) && !strcmp(prefix,prefix_name));
    if(packaged_failed){snprintf(error,capacity,"package metadata unreadable");return -1;}
    if(!an_directory_of(directory,sizeof(directory),NULL) || !an_collect(directory,1,&names,error,capacity))goto done;
    if(!an_directory_of(directory,sizeof(directory),g_language) || !an_collect(directory,0,&names,error,capacity))goto done;
    if(packaged_identity) {
        char name[32];snprintf(name,sizeof(name),"%u.bnk",packaged_identity);
        char **grown=realloc(names.items,(names.count+1)*sizeof(*grown));CHECK(grown);if(!grown)goto done;
        names.items=grown;names.items[names.count++]=_strdup(name);
    }
    rows=calloc(names.count ? names.count : 1,sizeof(*rows));if(!rows)goto done;
    for(size_t i=0;i<names.count;i++) {
        char *paths[2]={0};an_reader *reader=calloc(1,sizeof(*reader));sh_audio_bank bank={0};int present=0;
        if(!reader || !sh_audio_files_native_bank_paths(names.items[i],paths,paths+1)){free(reader);goto done;}
        reader->error=error;reader->capacity=capacity;
        for(size_t p=0;p<2;p++) {
            present=sh_package_runtime_audio_original_read(paths[p],0,NULL,0,&reader->length,error,capacity);
            if(present){reader->path=paths[p];break;}
        }
        if(present>0) {
            present=sh_audio_bank_read_source((sh_audio_bank_source){reader,reader->length,an_read},&bank,error,capacity);
            if(present==1 && !sh_audio_bank_names_identity(names.items[i],bank.id)) {
                snprintf(error,capacity,"audio bank filename mismatch");present=-1;
            }
            for(size_t r=0;present==1 && r<count;r++)if(rows[r].metadata.id==bank.id) {
                snprintf(error,capacity,"ambiguous bank identity");present=-1;
            }
            if(present==1) {
                rows[count]=(sh_audio_original_bank){_strdup(reader->path),names.items[i],bank};
                memset(&bank,0,sizeof(bank));count++;
            }
        }
        sh_audio_bank_free(&bank);free(reader);free(paths[0]);free(paths[1]);
        if(present<0)goto done;
    }
    ok=visit(context,rows,count,error,capacity)==1;
done:
    for(size_t i=0;i<count;i++){free((char *)rows[i].path);sh_audio_bank_free(&rows[i].metadata);}
    free(rows);an_names_free(&names);return ok ? 1 : -1;
}

static void put32(unsigned char *p,uint32_t n)
{ for(int i=0;i<4;++i) p[i]=(unsigned char)(n>>(8*i)); }
/* One event, optionally a play action naming another bank, optionally a carried
 * media row and a streamed source. Layout follows the verified v113 shapes. */
static size_t bank_bytes(unsigned char *p,uint32_t id,uint32_t event,int language,
    uint32_t needs,uint32_t carried,uint32_t streamed)
{
    size_t at=24,hirc,objects=1;
    memset(p,0,BANK_MAX);
    memcpy(p,"BKHD",4); put32(p+4,16); put32(p+8,113);
    put32(p+12,id); put32(p+16,(uint32_t)language);
    if(carried) {
        memcpy(p+at,"DIDX",4); put32(p+at+4,12);
        put32(p+at+8,carried); put32(p+at+12,0); put32(p+at+16,4); at+=20;
        memcpy(p+at,"DATA",4); put32(p+at+4,4); at+=12;
    }
    memcpy(p+at,"HIRC",4); hirc=at+8; at=hirc+4;
    p[at]=4; put32(p+at+1,needs?12:8); put32(p+at+5,event);
    if(needs) { put32(p+at+9,1); put32(p+at+13,50); at+=17; } else at+=13;
    if(needs) {
        p[at]=3; put32(p+at+1,18); put32(p+at+5,50);
        p[at+9]=0x03; p[at+10]=0x04; put32(p+at+11,600);
        put32(p+at+19,needs); at+=23; ++objects;
    }
    if(!needs) {
        p[at]=2;put32(p+at+1,18);put32(p+at+5,600);at+=23;++objects;
    }
    if(carried) {
        p[at]=2; put32(p+at+1,18); put32(p+at+5,601);
        put32(p+at+9,0x20001); p[at+13]=2; put32(p+at+14,carried); at+=23; ++objects;
    }
    if(streamed) {
        p[at]=2; put32(p+at+1,18); put32(p+at+5,602);
        put32(p+at+9,0x20001); p[at+13]=1; put32(p+at+14,streamed); at+=23; ++objects;
    }
    put32(p+hirc,(uint32_t)objects); put32(p+hirc-4,(uint32_t)(at-hirc));
    return at;
}
static void bank_file(const char *path,uint32_t id,uint32_t event,int language,
    uint32_t needs,uint32_t carried,uint32_t streamed)
{
    unsigned char bytes[BANK_MAX]; FILE *f=NULL;
    size_t size=bank_bytes(bytes,id,event,language,needs,carried,streamed);
    CHECK(!fopen_s(&f,path,"wb") && f);
    if(f) { CHECK(fwrite(bytes,1,size,f)==size); fclose(f); }
}

/* Catalog order follows directory enumeration, so tests address slots by the
 * identity they carry rather than by position. */
static sh_audio_bank_slot *slot(uint32_t id)
{
    for(size_t i=0;i<g_count;++i)if(g_slots[i].id==id)return g_slots+i;
    CHECK(0); return NULL;
}
static int activate(const uint32_t *events,size_t count,const sh_package_audio_bank *banks,
    size_t bank_count,char *error,size_t capacity)
{ return sh_audio_banks_native_activate(events,count,banks,bank_count,NULL,0,error,capacity); }

static sh_audio_limits_value playback;
static int playback_threshold(float value,int priority)
{ playback.threshold=value;playback.threshold_priority=priority;playback.amplitude=0.0001f;return 1; }
static int playback_voices(uint16_t value,int priority)
{ playback.voices=value;playback.voices_priority=priority;return 1; }
static int program_curve(sh_audio_native_curve *curve,const void *points,uint32_t count,uint32_t scaling)
{
    free(curve->points);memset(curve,0,sizeof(*curve));
    if(!count)return 31;
    curve->points=malloc((size_t)count*12);CHECK(curve->points);if(!curve->points)return 52;
    memcpy(curve->points,points,(size_t)count*12);curve->count=count;curve->scaling=scaling;return 1;
}
static int program_environment(sh_audio_environment *environment,int row,int column,uint32_t count,const void *points,uint32_t scaling)
{ return program_curve(environment->curves+row*3+column,points,count,scaling); }
static int program_dispatch(void *manager,const void *command)
{(void)manager;(void)command;return 1;}
static void *program_state_allocate(int pool,size_t bytes)
{CHECK(pool==17);return calloc(1,bytes);}
static void program_state_release(int pool,void *memory)
{CHECK(pool==17);free(memory);}
static int program_state_ensure(sh_audio_state_table *table,uint32_t id)
{
    CHECK(table->count==0 && table->capacity==1);
    sh_audio_state_group *group=calloc(1,sizeof(*group));if(!group)return 2;
    table->items[0]=(sh_audio_state_entry){id,0,group};table->count=1;return 1;
}
static void program_state_destroy(sh_audio_state_group *group)
{CHECK(!group->consumers);free(group->rules.items);memset(&group->rules,0,sizeof(group->rules));}
static sh_audio_parameter_node *program_parameter_ensure(sh_audio_parameter_table *table,uint32_t id)
{
    CHECK(!table->count);sh_audio_parameter_node *node=calloc(1,sizeof(*node));if(!node)return NULL;
    node->id=id;table->buckets[id%193]=node;table->count=1;return node;
}
static void program_parameter_destroy(sh_audio_parameter_node *node)
{CHECK(ap_idle(node));}
/* Native publication consumes a bound program. Originals keep their authored
 * ABI; each distinct private binding receives a fresh fixture identity. */
typedef struct native_assigned { unsigned domain; unsigned char key[32]; uint32_t id; } native_assigned;
static native_assigned g_native_assigned[64];
static size_t g_native_assigned_count;
static int native_program_assign(void *context,const sh_audio_link_graph *graph,size_t node,
    sh_audio_identity_domain domain,uint32_t authored,const unsigned char binding[32],int original,int absent,
    uint32_t *id,char *error,size_t capacity)
{
    (void)context;(void)graph;(void)node;(void)absent;(void)error;(void)capacity;
    if(original){*id=authored;return 1;}
    for(size_t i=0;i<g_native_assigned_count;i++)if(g_native_assigned[i].domain==(unsigned)domain &&
        !memcmp(g_native_assigned[i].key,binding,32)){*id=g_native_assigned[i].id;return 1;}
    if(g_native_assigned_count==64)return 0;
    native_assigned *row=g_native_assigned+g_native_assigned_count;row->domain=(unsigned)domain;
    memcpy(row->key,binding,32);*id=row->id=200000+(uint32_t)g_native_assigned_count++;return 1;
}
static void compiled_playback_settings(void)
{
    unsigned char bank[2][224]={{0},{0}};char error[512];sh_compiled_resource files[2]={{0},{0}};
    sh_package_compilation scopes[2]={{0},{0}};const sh_package_compilation *inputs[]={scopes,scopes+1};
    for(size_t i=0;i<2;i++) {
        uint32_t version=113,id=BANK_A,chunk=16,count=1,object_length=8,event=7;
        memcpy(bank[i],"BKHD",4);memcpy(bank[i]+4,&chunk,4);memcpy(bank[i]+8,&version,4);memcpy(bank[i]+12,&id,4);
        memcpy(bank[i]+24,"HIRC",4);chunk=17;memcpy(bank[i]+28,&chunk,4);memcpy(bank[i]+32,&count,4);
        bank[i][36]=4;memcpy(bank[i]+37,&object_length,4);memcpy(bank[i]+41,&event,4);
        memcpy(bank[i]+49,"STMG",4);chunk=63;memcpy(bank[i]+53,&chunk,4);
        float threshold=i ? -72.0f : -60.0f;uint16_t voices=i ? 256 : 128;
        memcpy(bank[i]+57,&threshold,4);memcpy(bank[i]+61,&voices,2);
        uint32_t groups=1,group=10,duration=100,from=1,to=(uint32_t)(2+i),rule=(uint32_t)(20+i);
        memcpy(bank[i]+63,&groups,4);memcpy(bank[i]+67,&group,4);memcpy(bank[i]+71,&duration,4);
        memcpy(bank[i]+75,&groups,4);memcpy(bank[i]+79,&from,4);memcpy(bank[i]+83,&to,4);memcpy(bank[i]+87,&rule,4);
        uint32_t parameter=30+(uint32_t)i;float value=0.5f+(float)i;
        memcpy(bank[i]+95,&groups,4);memcpy(bank[i]+99,&parameter,4);memcpy(bank[i]+103,&value,4);
        bank[i][119]=(unsigned char)(i+1);
        memcpy(bank[i]+120,"ENVS",4);chunk=96;memcpy(bank[i]+124,&chunk,4);
        for(unsigned n=0;n<6;n++) {
            unsigned char *curve=bank[i]+128+n*16;curve[0]=1;curve[1]=1;curve[2]=1;
            float y=(float)(n+1)*(float)(i+1);uint32_t interpolation=4;
            memcpy(curve+8,&y,4);memcpy(curve+12,&interpolation,4);
        }
        files[i].engine_path="sound/soundbanks/pc/a.bnk";files[i].body=bank[i];files[i].body_length=sizeof(bank[i]);
        files[i].owners.bits=files[i].gameplay_owners.bits=1;scopes[i].resources=files+i;scopes[i].resource_count=1;
    }
    sh_audio_originals *originals=sh_audio_originals_open(root,L"English(US)",NULL);CHECK(originals);
    sh_package_audio_program *program=sh_package_audio_program_open(inputs,2,originals,error,sizeof(error));CHECK(program);
    int prepared=sh_package_audio_program_prepare(program,error,sizeof(error));
    if(!prepared)fprintf(stderr,"playback program: %s\n",error);
    CHECK(prepared);if(!prepared){sh_package_audio_program_close(program);sh_audio_originals_close(originals);return;}
    /* Unbound programs have no publishable shared view. */
    CHECK(!sh_package_audio_program_bound_shared(program));
    sh_package_compilation bound_output={0};
    CHECK(sh_package_audio_program_bind(program,&bound_output,native_program_assign,NULL,error,sizeof(error)));
    CHECK(sh_package_audio_program_bound_shared(program)!=NULL);
    playback=(sh_audio_limits_value){-48,0.003f,0,1,64};sh_audio_limits_value before=playback;
    CRITICAL_SECTION lock;InitializeCriticalSection(&lock);void *manager=&playback;
    g_limits=(sh_audio_limits_native){playback_threshold,playback_voices,&playback.threshold,&playback.amplitude,
        &playback.threshold_priority,&playback.voices_priority,&playback.voices,&manager,&lock,NULL};
    sh_audio_environment environment={0},*environment_slot=&environment;
    g_environment=(sh_audio_environment_native){program_environment,program_curve,&environment_slot,&manager,&lock,NULL};
    sh_audio_environment_change curves={0};
    sh_audio_state_table states={0},*state_slot=&states;sh_audio_state_change state_change={0};int state_pool=17;
    g_state=(sh_audio_state_native){program_state_ensure,program_state_destroy,program_state_allocate,program_state_release,
        &state_pool,&state_slot,&manager,&lock,NULL};
    struct {sh_audio_parameter_table table;unsigned char rest[0x680];} parameters={0};
    sh_audio_parameter_table *parameter_slot=&parameters.table;sh_audio_parameter_change parameter_change={0};
    g_parameter=(sh_audio_parameter_native){program_parameter_ensure,program_parameter_destroy,program_state_allocate,program_state_release,
        &state_pool,&parameter_slot,&manager,&lock,NULL};
    g_bank_gate.entry=g_bank_gate.original=program_dispatch;g_bank_gate.manager_slot=&manager;g_bank_gate.render_lock=&lock;
    CHECK(sh_audio_bank_gate_initialize(&g_bank_gate));g_bank_gate.installed=1;
    sh_audio_bank_scope shared={0};
    sh_audio_limits_change change={0};
    g_commands.uncertain=1;
    CHECK(!sh_audio_banks_native_environment_apply(program,&curves,error,sizeof(error)) && !curves.owner);
    CHECK(!sh_audio_banks_native_limits_apply(program,&change,error,sizeof(error)) && !change.owner);
    CHECK(!sh_audio_banks_native_state_apply(program,&state_change,error,sizeof(error)) && !state_change.owner);
    CHECK(!sh_audio_banks_native_parameter_apply(program,&parameter_change,error,sizeof(error)) && !parameter_change.owner);
    CHECK(!sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    g_commands.uncertain=0;
    CHECK(sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_limits_apply(program,&change,error,sizeof(error))==1);
    CHECK(playback.threshold==-72 && playback.voices==256 && playback.threshold_priority==0 && playback.voices_priority==1);
    CHECK(sh_audio_banks_native_environment_apply(program,&curves,error,sizeof(error))==1 && curves.changed==63);
    CHECK(sh_audio_banks_native_state_apply(program,&state_change,error,sizeof(error))==1);
    CHECK(states.count==1 && states.items[0].id==10 && states.items[0].group->rules.count==2);
    CHECK(states.items[0].group->rules.items[0].duration==20 && states.items[0].group->rules.items[1].duration==21);
    CHECK(sh_audio_banks_native_parameter_apply(program,&parameter_change,error,sizeof(error))==1);
    CHECK(parameters.table.count==2 && ap_find(parameter_slot,30)->settings[0]==0x3f000000u &&
        ap_find(parameter_slot,31)->settings[0]==0x3fc00000u);
    CHECK(ap_bindings(parameter_slot)[0].count==1 && *(uint32_t *)ap_bindings(parameter_slot)[0].items==30);
    CHECK(ap_bindings(parameter_slot)[1].count==1 && *(uint32_t *)ap_bindings(parameter_slot)[1].items==31);
    for(unsigned n=0;n<6;n++)CHECK(environment.enabled[n] && environment.curves[n].points && environment.curves[n].count==1);
    CHECK(sh_audio_banks_native_shared_ready(&shared,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_activate(NULL,0,NULL,0,NULL,0,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_additions_begin(error,sizeof(error)));
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)) && al_equal(&before,&playback,3));
    CHECK(!sh_audio_banks_native_additions_begin(error,sizeof(error)));
    CHECK(sh_audio_environment_native_restore(&curves,error,sizeof(error)) && !g_environment.pending);
    CHECK(!sh_audio_banks_native_additions_begin(error,sizeof(error)));
    states.items[0].group->consumers=&states;
    CHECK(!sh_audio_state_native_restore(&state_change,error,sizeof(error)) && state_change.owner);
    states.items[0].group->consumers=NULL;
    CHECK(sh_audio_state_native_restore(&state_change,error,sizeof(error)) && !states.count && !states.items);
    CHECK(!sh_audio_banks_native_additions_begin(error,sizeof(error)));
    CHECK(sh_audio_parameter_native_restore(&parameter_change,error,sizeof(error)) && !parameters.table.count);
    CHECK(sh_audio_banks_native_shared_end(&shared,error,sizeof(error)));
    /* An unfinished recovery must retain its resources, but it must not hold
     * the bank worker indefinitely after control returns to the engine. */
    CHECK(sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_limits_apply(program,&change,error,sizeof(error))==1);
    CHECK(!sh_audio_banks_native_shared_end(&shared,error,sizeof(error)) && !shared.owner && change.owner);
    CHECK(!sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_recover(&shared,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_environment_apply(program,&curves,error,sizeof(error)) && !curves.owner);
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_end(&shared,error,sizeof(error)) && !g_shared_recovery);
    CHECK(sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_state_apply(program,&state_change,error,sizeof(error))==1);
    CHECK(!sh_audio_banks_native_shared_end(&shared,error,sizeof(error)) && state_change.owner && !shared.owner);
    CHECK(!sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_recover(&shared,error,sizeof(error)));
    sh_audio_state_change another_state={0};
    CHECK(!sh_audio_banks_native_state_apply(program,&another_state,error,sizeof(error)) && !another_state.owner);
    CHECK(sh_audio_state_native_restore(&state_change,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_end(&shared,error,sizeof(error)) && !g_shared_recovery);
    CHECK(sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_parameter_apply(program,&parameter_change,error,sizeof(error))==1);
    ap_find(parameter_slot,30)->subscribers[0].count=1;
    CHECK(!sh_audio_parameter_native_restore(&parameter_change,error,sizeof(error)) && parameter_change.owner);
    CHECK(!sh_audio_banks_native_shared_end(&shared,error,sizeof(error)) && !shared.owner && parameter_change.owner);
    CHECK(!sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_recover(&shared,error,sizeof(error)));
    sh_audio_parameter_change another_parameter={0};
    CHECK(!sh_audio_banks_native_parameter_apply(program,&another_parameter,error,sizeof(error)) && !another_parameter.owner);
    ap_find(parameter_slot,30)->subscribers[0].count=0;
    CHECK(sh_audio_parameter_native_restore(&parameter_change,error,sizeof(error)) && !g_parameter.pending);
    CHECK(sh_audio_banks_native_shared_end(&shared,error,sizeof(error)) && !g_shared_recovery);
    sh_audio_bank_addition *addition=sh_audio_banks_native_additions_begin(error,sizeof(error));CHECK(addition);
    CHECK(!sh_audio_banks_native_limits_apply(program,&change,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_environment_apply(program,&curves,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_state_apply(program,&state_change,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_parameter_apply(program,&parameter_change,error,sizeof(error)));
    CHECK(!sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_activate_additive(addition,NULL,0,NULL,0,NULL,0,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_limits_apply(program,&change,error,sizeof(error))==1);
    CHECK(!sh_audio_banks_native_additions_end(&addition,0,error,sizeof(error)) && addition);
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
    CHECK(sh_audio_banks_native_shared_end(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_additions_end(&addition,0,error,sizeof(error)) && !addition);
    CHECK(sh_audio_banks_native_shared_begin(&shared,error,sizeof(error)));
    CHECK(sh_audio_banks_native_limits_apply(program,&change,error,sizeof(error))==1);
    CHECK(sh_audio_banks_native_environment_apply(program,&curves,error,sizeof(error))==1);
    CHECK(sh_audio_banks_native_state_apply(program,&state_change,error,sizeof(error))==1);
    CHECK(sh_audio_banks_native_parameter_apply(program,&parameter_change,error,sizeof(error))==1);
    sh_package_audio_program_close(program);
    sh_audio_originals_close(originals);
    CHECK(sh_audio_limits_native_ready(&change,error,sizeof(error)));
    sh_audio_limits_native_accept(&change);CHECK(!g_limits.pending);
    CHECK(sh_audio_environment_native_ready(&curves,error,sizeof(error)));
    sh_audio_environment_native_accept(&curves);CHECK(g_environment.pending==&curves);
    CHECK(sh_audio_environment_native_release(&curves,error,sizeof(error)) && !g_environment.pending);
    CHECK(sh_audio_state_native_ready(&state_change,error,sizeof(error)));
    sh_audio_state_native_accept(&state_change);
    CHECK(sh_audio_state_native_release(&state_change,error,sizeof(error)) && !g_state.pending);
    CHECK(sh_audio_parameter_native_ready(&parameter_change,error,sizeof(error)));
    sh_audio_parameter_native_accept(&parameter_change);
    CHECK(sh_audio_parameter_native_release(&parameter_change,error,sizeof(error)) && !g_parameter.pending);
    CHECK(sh_audio_banks_native_shared_end(&shared,error,sizeof(error)));
    DeleteCriticalSection(&g_bank_gate.lock);memset(&g_bank_gate,0,sizeof(g_bank_gate));
    for(unsigned n=0;n<6;n++)CHECK(program_curve(environment.curves+n,NULL,0,0)==31);
    for(uint32_t n=0;n<states.count;n++){program_state_destroy(states.items[n].group);free(states.items[n].group);}
    free(states.items);memset(&g_state,0,sizeof(g_state));
    for(unsigned i=0;i<193;i++)for(sh_audio_parameter_node *node=parameters.table.buckets[i];node;) {
        sh_audio_parameter_node *next=node->next;program_parameter_destroy(node);free(node);node=next;
    }
    for(unsigned i=0;i<6;i++)free(ap_bindings(parameter_slot)[i].items);memset(&g_parameter,0,sizeof(g_parameter));
    memset(&g_environment,0,sizeof(g_environment));memset(&g_limits,0,sizeof(g_limits));DeleteCriticalSection(&lock);
}

static int named_probe_calls;
static int named_probe(const char *name,int pool,uint32_t *id)
{ ++named_probe_calls; CHECK(pool==-1); CHECK(sh_audio_bank_identity(name,id)); return 1; }
static void numeric_name_dispatch(void)
{
    uint32_t named=0, result=0;
    sh_audio_bank_slot candidate={0};
    CHECK(sh_audio_bank_identity("777.bnk",&named) && named!=777);
    candidate.name="777.bnk"; candidate.id=named;
    g_slots=&candidate; g_count=1; g_load=named_probe;
    CHECK(an_load(NULL,"777.bnk",&result)==1 && result==named && named_probe_calls==1);
    candidate.id=777;
    /* Literal identity requires its native binding; it must never fall back to
     * the working name loader and construct an unrelated hashed bank. */
    CHECK(an_load(NULL,"777.bnk",&result)==2 && result==777 && named_probe_calls==1);
    g_slots=NULL; g_count=0; g_load=NULL;
}

/* The engine mounts one language directory. Indexing every locale merged event
 * sets that the native loader never presents together. */
static void language_scoped_catalog(char *error,size_t capacity)
{
    uint32_t event;
    CHECK(sh_audio_banks_native_activate(NULL,0,NULL,0,NULL,0,error,sizeof(error)));
    CHECK(!activate(&(uint32_t){7},1,NULL,0,error,capacity));
    strcpy_s(g_directory,sizeof(g_directory),root); g_load=native_load; g_unload=native_unload;
    g_commands.manager_slot=&numeric_manager; g_commands.callback_slot=&numeric_callback;
    event=99; /* The localized copy is the candidate the native loader opens. */
    CHECK(activate(&event,1,NULL,0,error,capacity));
    CHECK(g_count==3 && loaded[1] && loads==1);
    event=7;  /* The shadowed root copy is not part of the catalog. */
    CHECK(activate(&event,1,NULL,0,error,capacity));
    CHECK(!loaded[1] && loads==1 && releases==1);
    event=55; /* Another locale's directory is never indexed. */
    CHECK(activate(&event,1,NULL,0,error,capacity));
    CHECK(loads==1 && releases==1);
    event=21; /* A bank named by its own decimal identity loads like any other. */
    CHECK(activate(&event,1,NULL,0,error,capacity));
    CHECK(loaded[4] && loads==2);
    CHECK(activate(NULL,0,NULL,0,error,capacity));
    CHECK(!loaded[4] && releases==2);
}

/* A play action records the bank holding its target, so wanting one bank wants
 * the banks it names, without loading the rest of the catalog. */
static void dependency_closure(char *error,size_t capacity)
{
    uint32_t event=13;
    CHECK(activate(&event,1,NULL,0,error,capacity));
    CHECK(loaded[2] && loaded[1] && !loaded[4] && loads==4);
    CHECK(activate(NULL,0,NULL,0,error,capacity));
    CHECK(!loaded[1] && !loaded[2] && releases==4);
}

static void media_interaction(char *error,size_t capacity)
{
    sh_package_audio_media media[3]={
        {"sound/soundbanks/pc/900.wem","900.wem",900,{4,{9}},1},
        {"sound/soundbanks/pc/901.wem","901.wem",901,{4,{9}},1},
        {"sound/soundbanks/pc/902.wem","902.wem",902,{4,{9}},0}
    };
    uint32_t event=13;
    int before=loads;
    /* Media never load banks by themselves; they are served by path. */
    CHECK(sh_audio_banks_native_activate(NULL,0,NULL,0,media,3,error,capacity));
    CHECK(loads==before);
    /* 900 is carried inside b.bnk, which event 13 wants, so the pass refuses. */
    CHECK(!sh_audio_banks_native_activate(&event,1,NULL,0,media,3,error,capacity));
    CHECK(strstr(error,"cannot replace media 900"));
    /* 901 is only in the DIDX of nothing wanted here; 902 streams. */
    CHECK(sh_audio_banks_native_activate(&event,1,NULL,0,media+1,2,error,capacity));
    CHECK(loaded[2] && loaded[1]);
    CHECK(sh_audio_banks_native_activate(NULL,0,NULL,0,media+1,2,error,capacity));
    CHECK(!loaded[1] && !loaded[2]);
}


/* Retained programs against a fake engine: bank usage slots, object tables and
 * load/unload commands follow the verified native layouts. */
static unsigned char *rp_manager, *rp_index;
static CRITICAL_SECTION rp_bank_lock;
static void *rp_vtable[6];
static int rp_loads, rp_unloads, rp_cut_voices, rp_fail_id_count;
static uint32_t rp_fail_id, rp_retained_object;
typedef struct rp_object { unsigned char bytes[0x18]; uint32_t bank; } rp_object;
static rp_object rp_objects[64];
static size_t rp_object_count;
static unsigned char *rp_find_slot(uint32_t id, unsigned char ***link)
{
    unsigned char **at = (unsigned char **)(rp_manager + 0x690 + (id % 193) * 8);
    while (*at) {
        uint32_t value; memcpy(&value, *at + 0x10, 4);
        if (value == id) { if (link) *link = at; return *at; }
        at = (unsigned char **)(*at + 0x20);
    }
    return NULL;
}
static void rp_register_object(uint32_t id, uint32_t bank)
{
    CHECK(rp_object_count < 64); if (rp_object_count >= 64) return;
    rp_object *o = rp_objects + rp_object_count++; memset(o, 0, sizeof(*o));
    memcpy(o->bytes + 0x10, &id, 4); o->bank = bank;
    unsigned char **bucket = (unsigned char **)(rp_index + 0x28 + (id % 193) * 8);
    memcpy(o->bytes + 8, bucket, sizeof(void *)); *bucket = o->bytes;
}
static void rp_rebuild_objects(void)
{
    for (size_t t = 0; t < 12; t++) memset(rp_index + t * 0x638 + 0x28, 0, 193 * 8);
    size_t kept = 0; rp_object copy[64]; memcpy(copy, rp_objects, sizeof(copy));
    size_t count = rp_object_count; rp_object_count = 0;
    for (size_t i = 0; i < count; i++) { uint32_t id; memcpy(&id, copy[i].bytes + 0x10, 4); rp_register_object(id, copy[i].bank); kept++; }
    (void)kept;
}
static void rp_drop_objects(uint32_t bank)
{
    size_t used = 0;
    for (size_t i = 0; i < rp_object_count; i++) {
        uint32_t id; memcpy(&id, rp_objects[i].bytes + 0x10, 4);
        if (rp_objects[i].bank == bank && id != rp_retained_object) continue;
        rp_objects[used++] = rp_objects[i];
    }
    rp_object_count = used; rp_rebuild_objects();
}
/* Buses following the native rules the bus leg relies on: an ID index,
 * reference counts, parent attach/detach through the vtable. */
static unsigned char rb_index[0x2000], *rb_index_value = rb_index;
static unsigned char *rb_objects[16];
static size_t rb_object_count;
static void *rb_normal[0x2b8 / 8], *rb_aux[0x2b8 / 8], *rb_consumer[8];
static unsigned char *rb_roots[2];
static uint32_t rb_rate = 48000, rb_expect_bus, rb_expect_bank;
static uint16_t rb_floor = 16;
static int32_t rb_properties[AB_PROPERTY_COUNT];
static int rb_missing_at_load, rb_value_notices, rb_recalc_notices, rb_param_notices;
static unsigned char *rb_registered(uint32_t id)
{ for (size_t i = 0; i < rb_object_count; i++) if (ab_u32(rb_objects[i] + AB_ID) == id) return rb_objects[i]; return NULL; }
static uint32_t rb_refs(const unsigned char *o) { return ab_u32(o + 0x14); }
static void rb_set_refs(unsigned char *o, uint32_t v) { memcpy(o + 0x14, &v, 4); }
static int rb_addref(void *o) { rb_set_refs(o, rb_refs(o) + 1); return (int)rb_refs(o); }
static void rb_detach(void *parent, void *child);
static int rb_release(void *o)
{
    unsigned char *bus = o; CHECK(rb_refs(bus) > 0);
    rb_set_refs(bus, rb_refs(bus) - 1);
    if (rb_refs(bus)) return (int)rb_refs(bus);
    for (size_t i = 0; i < rb_object_count; i++) if (rb_objects[i] == bus) { rb_objects[i] = rb_objects[--rb_object_count]; break; }
    if (ab_ptr(bus + AB_PARENT)) rb_detach(ab_ptr(bus + AB_PARENT), bus);
    if (ab_ptr(bus + AB_PROPERTIES)) free(ab_ptr(bus + AB_PROPERTIES));
    free(bus); return 0;
}
static int rb_attach(void *parent, void *child)
{
    int result = 1;
    if (ab_ptr((unsigned char *)child + AB_PARENT)) result = 0x15;
    else { ab_set_ptr((unsigned char *)child + AB_PARENT, parent); rb_addref(parent); }
    rb_release(child); return result;
}
static void rb_detach(void *parent, void *child)
{
    if (ab_ptr((unsigned char *)child + AB_PARENT) != parent) return;
    ab_set_ptr((unsigned char *)child + AB_PARENT, NULL); rb_release(parent);
}
/* The native notifier walks the consumer's registered instances through their
 * optional list (consumer +8), which it dereferences without a check (R307). */
static void rb_value(void *consumer, char flag, uint32_t parameter)
{ (void)flag; (void)parameter; CHECK(ab_ptr((unsigned char *)consumer + 8) != NULL); rb_value_notices++; }
static void rb_recalc(void *bus, char flag) { (void)bus; CHECK(!flag); rb_recalc_notices++; }
/* The bus's parameter notification receives only the HDR parameters. */
static void rb_param(void *bus, void *record)
{ uint32_t type; (void)bus; memcpy(&type, record, 4); CHECK(type >= 0x20 && type <= 0x22); rb_param_notices++; }
static void *rb_lookup(void *table, uint32_t id)
{
    CHECK(table == rb_index + AB_TABLE);
    unsigned char *o = rb_registered(id);
    if (o) rb_addref(o);
    return o;
}
static void *rb_construct(void *memory, uint32_t id)
{
    unsigned char *bus = memory; uint32_t floor_volume = 0xc2c0999a, limit = 100;
    memset(bus, 0, AB_SIZE); ab_set_ptr(bus, rb_normal); ab_set_ptr(bus + AB_CONSUMER, rb_consumer);
    memcpy(bus + AB_ID, &id, 4); rb_set_refs(bus, 1);
    bus[AB_FLAGS_A] = 0x20; bus[AB_FLAGS_B] = 0x01; bus[AB_FLAGS_C] = 0x30;
    memcpy(bus + AB_FLOOR, &floor_volume, 4); memcpy(bus + 0xdc, &limit, 4);
    return bus;
}
static int rb_initialize(void *bus) { CHECK(rb_object_count < 16); rb_objects[rb_object_count++] = bus; return 1; }
static void *rb_create_aux(uint32_t id)
{
    unsigned char *bus = program_state_allocate(17, AB_SIZE);
    rb_construct(bus, id); ab_set_ptr(bus, rb_aux); rb_initialize(bus); return bus;
}
static int rb_mixer(void *bus, uint32_t id, char shared, int priority) { (void)bus; (void)shared; (void)priority; return id ? 2 : 1; }
static void rb_none(void *bus) { (void)bus; }
static void rb_curve(void *bus, uint32_t parameter) { (void)bus; (void)parameter; }
static void rb_setup(void **manager, CRITICAL_SECTION *lock)
{
    void **tables[2] = {rb_normal, rb_aux};
    for (int i = 0; i < 2; i++) {
        tables[i][1] = (void *)rb_addref; tables[i][2] = (void *)rb_release; tables[i][0x48 / 8] = (void *)rb_attach;
        tables[i][0x60 / 8] = (void *)rb_detach; tables[i][0xe8 / 8] = (void *)rb_curve; tables[i][0x1b0 / 8] = (void *)rb_none;
        tables[i][0x1c8 / 8] = (void *)rb_mixer; tables[i][0x1a0 / 8] = (void *)rb_recalc; tables[i][0xa8 / 8] = (void *)rb_param;
    }
    rb_consumer[3] = (void *)rb_value;
    for (int i = 0; i < AB_PROPERTY_COUNT; i++) rb_properties[i] = i < 6 ? i : 64;
    rb_properties[26] = 0x20; rb_properties[27] = 0x22; rb_properties[28] = 0x21;   /* as both images */
    memset(&g_bus, 0, sizeof(g_bus));
    g_bus.subscriptions = &g_subscription; g_bus.render_lock = lock; g_bus.manager_slot = manager;
    g_bus.index_slot = (unsigned char **)&rb_index_value; g_bus.lookup = rb_lookup; g_bus.construct = rb_construct;
    g_bus.initialize = rb_initialize; g_bus.create_aux = rb_create_aux; g_bus.normal_vtable = rb_normal;
    g_bus.aux_vtable = rb_aux; g_bus.consumer_vtable = rb_consumer;
    g_bus.roots[0] = (void **)&rb_roots[0]; g_bus.roots[1] = (void **)&rb_roots[1];
    g_bus.sample_rate = &rb_rate; g_bus.duck_floor = &rb_floor; g_bus.property_parameters = rb_properties;
    /* The master bus, held by its bank. */
    unsigned char *master = rb_construct(program_state_allocate(17, AB_SIZE), 1);
    rb_initialize(master); rb_roots[0] = master;
}
static int rp_bank_load(uint32_t id)
{
    ++rp_loads;
    if (id == rb_expect_bank && rb_expect_bus && !rb_registered(rb_expect_bus)) rb_missing_at_load++;
    if (id == rp_fail_id && rp_fail_id_count-- > 0) return 2;
    if (rp_find_slot(id, NULL)) return 69;
    unsigned char *slot = calloc(1, 0x80); if (!slot) return 52;
    int one = 1; memcpy(slot + 0x10, &id, 4); memcpy(slot + 0x70, &one, 4); slot[0x78] = 1;
    unsigned char **bucket = (unsigned char **)(rp_manager + 0x690 + (id % 193) * 8);
    memcpy(slot + 0x20, bucket, sizeof(void *)); *bucket = slot;
    return 1;
}
static int rp_submit(void *manager, const abc_command *command)
{ (void)manager; return rp_bank_load(command->id); }
static int rp_init(void *manager, abc_completion *completion) { (void)manager; (void)completion; return 1; }
static int rp_wait(void *manager, abc_completion *completion, int submitted) { (void)manager; (void)completion; return submitted; }
static int rp_named_load(const char *name, int pool, uint32_t *id)
{ CHECK(pool == -1); CHECK(sh_audio_bank_identity(name, id)); return rp_bank_load(*id); }
static HANDLE rp_retirement_reached;
static int rp_unload(uint32_t id, const void *memory, int *pool)
{
    unsigned char **link = NULL, *slot = rp_find_slot(id, &link); (void)memory; (void)pool;
    ++rp_unloads;
    if (!slot) return 2;
    int32_t refs; memcpy(&refs, slot + 0x70, 4);
    rp_cut_voices += refs - 1; /* Native unload stops every item holding the slot. */
    memcpy(link, slot + 0x20, sizeof(void *)); free(slot);
    rp_drop_objects(id);
    if (rp_retirement_reached) SetEvent(rp_retirement_reached);
    return 1;
}
static void rp_set_refs(uint32_t id, int32_t refs)
{ unsigned char *slot = rp_find_slot(id, NULL); CHECK(slot); if (slot) memcpy(slot + 0x70, &refs, 4); }
static int rp_occupied(void *context, sh_audio_identity_domain domain, uint32_t id, char *error, size_t capacity)
{ (void)context; (void)domain; return sh_audio_registry_occupied(&g_registry, id, error, capacity); }
typedef struct rp_owner { int references; uint32_t media; } rp_owner;
static rp_owner rp_owners[10];
static int rp_media_failed, rp_media_opens;
static sh_package_file *rp_media_file;
static HANDLE rp_reader_entered, rp_reader_resume;
int sh_package_runtime_audio_file(void *owner, const char *path, sh_package_file **file)
{
    rp_owner *o = owner;
    char exact[96];
    *file = NULL;
    CHECK(o >= rp_owners && o < rp_owners + 10 && o->references > 0);
    ++rp_media_opens;
    snprintf(exact, sizeof(exact), "sound/soundbanks/pc/%u.wem", o->media);
    if (!o->media || strcmp(path, exact)) return 0;
    if (rp_media_failed) return -1;
    if (rp_reader_entered) {
        SetEvent(rp_reader_entered);
        CHECK(WaitForSingleObject(rp_reader_resume, 5000) == WAIT_OBJECT_0);
        CHECK(o->references > 0);
    }
    *file = sh_package_file_retain(rp_media_file);
    return *file ? 1 : -1;
}
typedef struct rp_stream_read { const char *path; FILE *stream; uint64_t length; int result; } rp_stream_read;
static DWORD WINAPI rp_stream_reader(void *context)
{
    rp_stream_read *read = context;
    read->result = sh_audio_banks_native_open_media(read->path, &read->stream, &read->length);
    return 0;
}
static DWORD WINAPI rp_stream_retire(void *context)
{
    char error[512];
    *(int *)context = sh_audio_banks_native_programs_retire(error, sizeof(error));
    return 0;
}
static void rp_release(void *owner) { rp_owner *o = owner; CHECK(o->references > 0); o->references--; }
static sh_audio_active_program rp_ref(size_t owner, sh_package_audio_program *program, sh_audio_id_lease *lease)
{ rp_owners[owner].references++; return (sh_audio_active_program){rp_owners + owner, program, lease}; }
/* A program kept only to serve consumers (a rejected candidate's movies). */
static sh_audio_active_program rp_serve(size_t owner, sh_package_audio_program *program, sh_audio_id_lease *lease)
{ sh_audio_active_program ref = rp_ref(owner, program, lease); ref.serving = 1; return ref; }
/* Optional STMG switch record for the next built bank: group, RTPC
 * controller and two points mapping inputs 0 and 10 to two switch values. */
static uint32_t rp_switch_group, rp_switch_controller = 77, rp_bus_id;
/* An event and one new normal bus under the master bus with a volume. */
static size_t rp_bus_bank(unsigned char *bank, const char *name, uint32_t event, uint32_t bus)
{
    uint32_t id = 0, version = 113, size = 16, count = 2, length = 8, bus_length = 48, parent = 1; float volume = -6.0f;
    CHECK(sh_audio_bank_identity(name, &id)); memset(bank, 0, 160);
    memcpy(bank, "BKHD", 4); memcpy(bank + 4, &size, 4); memcpy(bank + 8, &version, 4); memcpy(bank + 12, &id, 4);
    memcpy(bank + 24, "HIRC", 4); size = 70; memcpy(bank + 28, &size, 4); memcpy(bank + 32, &count, 4);
    bank[36] = 4; memcpy(bank + 37, &length, 4); memcpy(bank + 41, &event, 4);
    unsigned char *o = bank + 49;
    o[0] = 8; memcpy(o + 1, &bus_length, 4); memcpy(o + 5, &bus, 4); memcpy(o + 9, &parent, 4);
    o[13] = 1; o[14] = 0; memcpy(o + 15, &volume, 4);
    return 102;
}
/* init.bnk holding one bus under the master bus (1) with the given
 * properties and no events, laid out as the bus grammar reads it
 * (audio_hirc.c ah_bus): ID, parent, properties, settings, ducking, effects,
 * mixer, flags, RTPC curves and state groups. */
static size_t rp_init_bus_bank(unsigned char *bank, uint32_t bus, const unsigned char *keys, const float *values, size_t n)
{
    uint32_t id = 0, version = 113, size = 16, count = 1, parent = 1, body = (uint32_t)(43 + 5 * n), hirc = 4 + 5 + body;
    CHECK(sh_audio_bank_identity("init.bnk", &id) && n <= 4); memset(bank, 0, 160);
    memcpy(bank, "BKHD", 4); memcpy(bank + 4, &size, 4); memcpy(bank + 8, &version, 4); memcpy(bank + 12, &id, 4);
    memcpy(bank + 24, "HIRC", 4); memcpy(bank + 28, &hirc, 4); memcpy(bank + 32, &count, 4);
    unsigned char *o = bank + 36;
    o[0] = 8; memcpy(o + 1, &body, 4); memcpy(o + 5, &bus, 4); memcpy(o + 9, &parent, 4);
    o[13] = (unsigned char)n; memcpy(o + 14, keys, n); memcpy(o + 14 + n, values, 4 * n);
    return 36 + 5 + body;
}
static size_t rp_bank(unsigned char *bank, const char *name, uint32_t event, float threshold)
{
    if (rp_bus_id) return rp_bus_bank(bank, name, event, rp_bus_id);
    uint32_t id = 0, version = 113, size = 16, count = 1, length = 8; CHECK(sh_audio_bank_identity(name, &id));
    memset(bank, 0, 160);
    memcpy(bank, "BKHD", 4); memcpy(bank + 4, &size, 4); memcpy(bank + 8, &version, 4); memcpy(bank + 12, &id, 4);
    memcpy(bank + 24, "HIRC", 4); size = 17; memcpy(bank + 28, &size, 4); memcpy(bank + 32, &count, 4);
    bank[36] = 4; memcpy(bank + 37, &length, 4); memcpy(bank + 41, &event, 4);
    if (!threshold) return 49;
    memcpy(bank + 49, "STMG", 4); size = 18; memcpy(bank + 53, &size, 4);
    uint16_t voices = 64; memcpy(bank + 57, &threshold, 4); memcpy(bank + 61, &voices, 2);
    if (!rp_switch_group) return 75;
    uint32_t switches = 1, points = 2, shape = 4, values[2] = {5001, 5002}; float x[2] = {0, 10};
    size = 55; memcpy(bank + 53, &size, 4);
    memcpy(bank + 67, &switches, 4); memcpy(bank + 71, &rp_switch_group, 4); memcpy(bank + 75, &rp_switch_controller, 4);
    bank[79] = 0; memcpy(bank + 80, &points, 4);
    for (size_t i = 0; i < 2; i++) {
        memcpy(bank + 84 + i * 12, x + i, 4); memcpy(bank + 88 + i * 12, values + i, 4); memcpy(bank + 92 + i * 12, &shape, 4);
    }
    return 112;
}
/* Switch groups and the subscription core with the verified layouts; the
 * parameter controller list was reserved by the adapter before insertion. */
static struct { sh_audio_switch_table table; unsigned char rest[0x700]; } rp_switches;
static void *rp_switch_slot, *rp_modulator_table[193], *rp_modulator_slot;
static sh_audio_parameter_native rp_subscription_parameters;
static int rp_switch_refreshes;
static void *rp_switch_ensure(void *t, uint32_t id)
{
    sh_audio_switch_table *table = t;
    for (sh_audio_switch_node *n = table->buckets[id % 193]; n; n = n->next) if (n->id == id) return n;
    sh_audio_switch_node *n = calloc(1, sizeof(*n)); if (!n) return NULL;
    n->id = id; n->vtable = rp_vtable; n->next = table->buckets[id % 193]; table->buckets[id % 193] = n; table->count++;
    return n;
}
static sh_audio_parameter_node *rp_parameter_ensure(sh_audio_parameter_table *table, uint32_t id)
{
    for (sh_audio_parameter_node *n = table->buckets[id % 193]; n; n = n->next) if (n->id == id) return n;
    sh_audio_parameter_node *n = calloc(1, sizeof(*n)); if (!n) return NULL;
    n->id = id; n->next = table->buckets[id % 193]; table->buckets[id % 193] = n; table->count++;
    return n;
}
static void rp_parameter_destroy(sh_audio_parameter_node *node)
{ for (unsigned i = 0; i < 2; i++) free(node->subscribers[i].items); memset(node->subscribers, 0, sizeof(node->subscribers)); }
static int rp_reverse_add(sh_audio_parameter_node *node, void *record, char selector)
{
    sh_audio_parameter_vector *list = node->subscribers + (selector ? 0 : 1);
    CHECK(list->count < list->capacity); if (list->count >= list->capacity) return 0x34;
    ((void **)list->items)[list->count++] = record; return 1;
}
static char rp_reverse_remove(sh_audio_parameter_node *node, void *record)
{
    for (unsigned l = 0; l < 2; l++) {
        void **items = node->subscribers[l].items;
        for (uint32_t i = 0; i < node->subscribers[l].count; i++) if (items[i] == record) {
            memmove(items + i, items + i + 1, (node->subscribers[l].count - i - 1) * sizeof(void *));
            node->subscribers[l].count--; break;
        }
    }
    return 1;
}
static int rp_modulator_add(void *m, uint32_t id, void *record) { (void)m; (void)id; (void)record; return 2; }
static void rp_modulator_remove(void *m, void *record, uint32_t id) { (void)m; (void)record; (void)id; }
static void rp_refresh(sh_audio_parameter_table *t, void *record, char flag) { (void)t; (void)record; (void)flag; rp_switch_refreshes++; }
static sh_audio_switch_node *rp_switch(uint32_t id)
{ for (sh_audio_switch_node *n = rp_switches.table.buckets[id % 193]; n; n = n->next) if (n->id == id) return n; return NULL; }
typedef struct rp_program {
    unsigned char banks[2][160]; sh_compiled_resource files[3]; sh_package_compilation scope;
    char paths[2][64]; sh_package_audio_program *program; sh_audio_id_lease *lease;
} rp_program;
static int rp_streamed;
/* Real event -> play action -> streamed sound, with media 900 in the same scope. */
static size_t rp_stream_bank(unsigned char *bank, const char *name, uint32_t event)
{
    uint32_t id = 0;
    CHECK(sh_audio_bank_identity(name, &id));
    memset(bank, 0, 160);
#define RP_U32(at, value) do { uint32_t v_ = (value); memcpy(bank + (at), &v_, 4); } while (0)
    memcpy(bank, "BKHD", 4); RP_U32(4, 16); RP_U32(8, 113); RP_U32(12, id);
    memcpy(bank + 24, "HIRC", 4); RP_U32(28, 95); RP_U32(32, 3);
    bank[36] = 4; RP_U32(37, 12); RP_U32(41, event); RP_U32(45, 1); RP_U32(49, event + 1);
    bank[53] = 3; RP_U32(54, 18); RP_U32(58, event + 1);
    bank[62] = 3; bank[63] = 4; RP_U32(64, event + 2); RP_U32(72, id);
    bank[76] = 2; RP_U32(77, 46); RP_U32(81, event + 2); RP_U32(85, 0x20001);
    bank[89] = 1; RP_U32(90, 900); RP_U32(94, 4);
#undef RP_U32
    return 127;
}
static int rp_media_id(void *context, uint32_t authored, uint32_t bound, const sh_audio_graph_media *media)
{
    CHECK(authored == 900 && !media->original && bound != 900);
    *(uint32_t *)context = bound; return 1;
}
static int rp_build(rp_program *p, sh_audio_originals *originals, sh_audio_id_pool *pool,
    const char *first, const char *second, uint32_t event, float threshold)
{
    char error[512]; const sh_package_compilation *scope = &p->scope;
    memset(p, 0, sizeof(*p));
    const char *names[2] = {first, second};
    for (size_t i = 0; i < 2 && names[i]; i++) {
        snprintf(p->paths[i], sizeof(p->paths[i]), "sound/soundbanks/pc/%s", names[i]);
        p->files[i].engine_path = p->paths[i]; p->files[i].body = p->banks[i];
        p->files[i].body_length = rp_bank(p->banks[i], names[i], event + (uint32_t)i, i ? 0 : threshold);
        if (rp_streamed && !i) p->files[i].body_length = rp_stream_bank(p->banks[i], names[i], event);
        p->files[i].owners.bits = p->files[i].gameplay_owners.bits = 1; p->scope.resource_count++;
    }
    if (rp_streamed) {
        sh_compiled_resource *media = p->files + p->scope.resource_count++;
        media->engine_path = "sound/soundbanks/pc/900.wem";
        media->body = (unsigned char *)"tone"; media->body_length = 4;
        media->owners.bits = media->gameplay_owners.bits = 1;
        /* Compilation lookup uses canonical sorted paths. */
        for (size_t j = p->scope.resource_count - 1; j > 0 &&
            strcmp(p->files[j].engine_path, p->files[j - 1].engine_path) < 0; j--) {
            sh_compiled_resource swap = p->files[j];
            p->files[j] = p->files[j - 1]; p->files[j - 1] = swap;
        }
    }
    p->scope.resources = p->files;
    p->program = sh_package_audio_program_open(&scope, 1, originals, error, sizeof(error));
    int ok = p->program && sh_package_audio_program_prepare(p->program, error, sizeof(error));
    if (ok) {
        sh_package_compilation output = {0};
        p->lease = sh_audio_id_lease_begin(pool);
        ok = p->lease && sh_package_audio_program_bind(p->program, &output, sh_audio_id_reserve, p->lease, error, sizeof(error));
    }
    if (!ok) fprintf(stderr, "retained program fixture: %s\n", error);
    CHECK(ok); return ok;
}
typedef struct rp_plan { uint32_t ids[4]; size_t count; } rp_plan;
static int rp_plan_visit(void *context, uint32_t id, const char *filename, int numeric)
{ rp_plan *plan = context; (void)filename; CHECK(numeric); if (plan->count < 4) plan->ids[plan->count++] = id; return 1; }
static rp_plan rp_loads_of(const rp_program *p)
{
    rp_plan plan = {0}; char error[256];
    CHECK(sh_package_audio_program_loads(p->program, rp_plan_visit, &plan, error, sizeof(error)));
    return plan;
}
typedef struct rp_exclusive { uint32_t except, found; } rp_exclusive;
static int rp_exclusive_visit(void *context, sh_audio_identity_domain domain, uint32_t id)
{ rp_exclusive *e = context; (void)domain; if (id != e->except && !e->found) e->found = id; return 1; }
static uint32_t rp_first_exclusive(sh_audio_id_lease *lease, uint32_t except)
{ rp_exclusive e = {except, 0}; CHECK(sh_audio_id_lease_exclusive(lease, rp_exclusive_visit, &e) && e.found); return e.found; }
static int prepared_loads;
static int prepared_named_load(const char *name, int pool, uint32_t *id)
{ (void)pool; prepared_loads++; return sh_audio_bank_identity(name, id) ? 1 : 2; }
/* Sound init prepares doom_initial.bnk (VK 141d2d790, GL 141d46bb0): one
 * preparation reference, no load reference and a clear loaded bit. Its
 * objects are registered, so a program shares it and never loads over it. */
static void prepared_native_slot(void)
{
    unsigned char *manager = calloc(1, 0xca0), *slot = calloc(1, 0x80);
    uint32_t id = 0, *owned = NULL; int32_t one = 1, refs = -1, prepared = -1; size_t owned_count = 0;
    char error[256] = {0}; CRITICAL_SECTION lock;
    CHECK(manager && slot && sh_audio_bank_identity("doom_initial.bnk", &id));
    if (!manager || !slot) { free(manager); free(slot); return; }
    InitializeCriticalSection(&lock);
    memcpy(slot + 0x10, &id, 4); memcpy(slot + 0x74, &one, 4);
    memcpy(manager + 0x690 + (id % 193) * 8, &slot, sizeof(slot));
    void *manager_slot = manager;
    sh_audio_registry saved_registry = g_registry; ab_load_fn saved_load = g_load;
    an_pbank *saved_banks = g_pbanks; size_t saved_count = g_pbank_count;
    an_pbank bank = {0}; bank.id = id; strcpy_s(bank.filename, sizeof(bank.filename), "doom_initial.bnk");
    g_registry = (sh_audio_registry){&manager_slot, NULL, &lock}; g_load = prepared_named_load;
    g_pbanks = &bank; g_pbank_count = 1; prepared_loads = 0;
    CHECK(an_pbank_load(0, &owned, &owned_count, error, sizeof(error)) == 1);
    CHECK(!prepared_loads && !bank.owned && !owned_count);
    CHECK(an_pbank_unload_idle(0, error, sizeof(error)) == 1);
    memcpy(&refs, slot + 0x70, 4); memcpy(&prepared, slot + 0x74, 4);
    CHECK(!refs && prepared == 1 && !(slot[0x78] & 1));
    /* Without a native slot the same plan does load (and then owns) the bank. */
    memset(manager + 0x690 + (id % 193) * 8, 0, sizeof(slot));
    CHECK(an_pbank_load(0, &owned, &owned_count, error, sizeof(error)) == 1);
    CHECK(prepared_loads == 1 && bank.owned && owned_count == 1);
    free(owned);
    g_registry = saved_registry; g_load = saved_load; g_pbanks = saved_banks; g_pbank_count = saved_count;
    DeleteCriticalSection(&lock); free(slot); free(manager);
}
static void retained_programs(void)
{
    char base[MAX_PATH], error[512];
    snprintf(base, sizeof(base), "%s\\retained-base", root); CHECK(CreateDirectoryA(base, NULL));
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); CHECK(originals);
    rp_manager = calloc(1, 0xca0); rp_index = calloc(1, 0x4aa0); CHECK(rp_manager && rp_index);
    for (size_t i = 0; i < 12; i++) InitializeCriticalSection((CRITICAL_SECTION *)(rp_index + i * 0x638));
    InitializeCriticalSection((CRITICAL_SECTION *)(rp_manager + 0x48)); InitializeCriticalSection(&rp_bank_lock);
    rp_vtable[3] = (void *)rp_submit; rp_vtable[4] = (void *)rp_init; rp_vtable[5] = (void *)rp_wait;
    void **table = rp_vtable; memcpy(rp_manager, &table, sizeof(table));
    void *manager = rp_manager, *index = rp_index, *callback = NULL;
    sh_audio_bank_commands saved_commands = g_commands; sh_audio_registry saved_registry = g_registry;
    ab_load_fn saved_load = g_load; ab_unload_fn saved_unload = g_unload;
    g_commands = (sh_audio_bank_commands){&manager, &callback, {0}, 0};
    g_registry = (sh_audio_registry){&manager, &index, &rp_bank_lock};
    g_load = rp_named_load; g_unload = rp_unload;
    /* Shared-control legs with the fake playback/parameter/state natives. */
    CRITICAL_SECTION lock; InitializeCriticalSection(&lock); void *audio_manager = &playback;
    playback = (sh_audio_limits_value){-60, 0.001f, 0, 0, 64};
    g_limits = (sh_audio_limits_native){playback_threshold, playback_voices, &playback.threshold, &playback.amplitude,
        &playback.threshold_priority, &playback.voices_priority, &playback.voices, &audio_manager, &lock, NULL};
    static sh_audio_environment environment; sh_audio_environment *environment_slot = &environment;
    g_environment = (sh_audio_environment_native){program_environment, program_curve, &environment_slot, &audio_manager, &lock, NULL};
    static sh_audio_state_table states; sh_audio_state_table *state_slot = &states; int pool_id = 17;
    g_state = (sh_audio_state_native){program_state_ensure, program_state_destroy, program_state_allocate, program_state_release,
        &pool_id, &state_slot, &audio_manager, &lock, NULL};
    static struct { sh_audio_parameter_table table; unsigned char rest[0x680]; } parameters;
    sh_audio_parameter_table *parameter_slot = &parameters.table;
    g_parameter = (sh_audio_parameter_native){program_parameter_ensure, program_parameter_destroy, program_state_allocate,
        program_state_release, &pool_id, &parameter_slot, &audio_manager, &lock, NULL};
    g_bank_gate.entry = g_bank_gate.original = program_dispatch; g_bank_gate.manager_slot = &audio_manager;
    g_bank_gate.render_lock = &lock; CHECK(sh_audio_bank_gate_initialize(&g_bank_gate)); g_bank_gate.installed = 1;
    rp_switch_slot = &rp_switches.table; rp_modulator_slot = rp_modulator_table;
    rp_subscription_parameters = (sh_audio_parameter_native){rp_parameter_ensure, rp_parameter_destroy, program_state_allocate,
        program_state_release, &pool_id, &parameter_slot, &audio_manager, &lock, NULL};
    g_subscription = (sh_audio_subscription_native){&rp_subscription_parameters, &rp_modulator_slot, rp_reverse_add,
        rp_reverse_remove, rp_modulator_add, rp_modulator_remove, rp_refresh, program_curve, &rp_switch_slot, rp_switch_ensure};
    CHECK(sh_audio_switch_native_bind(&g_switch, &g_subscription, &g_limits));
    rb_setup(&audio_manager, &lock);
    /* Native Init baseline: -60 threshold, 64 voices. */
    unsigned char init[18] = {0}; float baseline_threshold = -60; uint16_t baseline_voices = 64;
    memcpy(init, &baseline_threshold, 4); memcpy(init + 4, &baseline_voices, 2);
    sh_audio_shared_input init_input = {"STMG", init, sizeof(init), 0, 1};
    sh_audio_shared_policy baseline = {0};
    CHECK(sh_audio_shared_policy_compile(&init_input, 1, &baseline, error, sizeof(error)));
    sh_audio_id_pool *pool = sh_audio_id_pool_open(rp_occupied, NULL); CHECK(pool);
    static rp_program a, b, c, s, d, w, v, m, l;
    rp_streamed = 1;
    CHECK(rp_build(&a, originals, pool, "pa.bnk", NULL, 70, 0));
    rp_streamed = 0;
    CHECK(sh_package_audio_program_media(a.program, 0, rp_media_id, &rp_owners[0].media, error, sizeof(error)));
    CHECK(rp_owners[0].media);
    { char path[MAX_PATH]; FILE *stream = NULL;
      snprintf(path, sizeof(path), "%s/retiring-a.wem", root);
      CHECK(!fopen_s(&stream, path, "wb") && stream);
      if (stream) { CHECK(fwrite("map-a", 1, 5, stream) == 5); fclose(stream); }
      rp_media_file = sh_package_file_capture(path, error, sizeof(error)); CHECK(rp_media_file); }
    CHECK(rp_build(&b, originals, pool, "pb.bnk", NULL, 80, 0));
    CHECK(rp_build(&c, originals, pool, "pc1.bnk", "pc2.bnk", 90, 0));
    CHECK(rp_build(&s, originals, pool, "ps.bnk", NULL, 100, -72));
    CHECK(rp_build(&d, originals, pool, "pd.bnk", NULL, 110, 0));
    rp_switch_group = 620; CHECK(rp_build(&w, originals, pool, "pw.bnk", NULL, 120, -60)); rp_switch_group = 0;
    rp_bus_id = 630; CHECK(rp_build(&v, originals, pool, "pv.bnk", NULL, 130, 0)); rp_bus_id = 0;
    rp_plan pa = rp_loads_of(&a), pb = rp_loads_of(&b), pc = rp_loads_of(&c), pd = rp_loads_of(&d);
    uint32_t pa_id = 0; CHECK(sh_audio_bank_identity("pa.bnk", &pa_id));
    CHECK(pa.count == 1 && pa.ids[0] != pa_id && pc.count == 2 && pb.count == 1);
    /* 1. Activation loads a private bank by file ID and activates the lease. */
    sh_audio_active_program set[3];
    set[0] = rp_ref(0, a.program, a.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_find_slot(pa.ids[0], NULL) && sh_audio_id_lease_active(a.lease) && sh_audio_banks_native_programs_count() == 1);
    CHECK(rp_owners[0].references == 1 && playback.threshold == -60);
    /* 2. A transition loads the new program; the idle old one retires fully. */
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_find_slot(pb.ids[0], NULL) && !rp_find_slot(pa.ids[0], NULL) && !rp_cut_voices);
    CHECK(sh_audio_banks_native_programs_count() == 1 && rp_owners[0].references == 0 && rp_owners[1].references == 1);
    /* 3. Late playback keeps a superseded program and its bank until the voice ends. */
    set[0] = rp_ref(0, a.program, a.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_owners[1].references == 0 && rp_find_slot(pa.ids[0], NULL));
    rp_set_refs(pa.ids[0], 2);
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(sh_audio_banks_native_programs_count() == 2 && rp_find_slot(pa.ids[0], NULL) && rp_owners[0].references == 1);
    CHECK(sh_audio_banks_native_programs_retire(error, sizeof(error)) == 1 && rp_find_slot(pa.ids[0], NULL));
    char media_path[96], wrong_path[112]; FILE *media_stream = NULL; uint64_t media_length = 0;
    snprintf(media_path, sizeof(media_path), "sound/soundbanks/pc/%u.wem", rp_owners[0].media);
    snprintf(wrong_path, sizeof(wrong_path), "sound/soundbanks/pc/french/%u.wem", rp_owners[0].media);
    CHECK(sh_audio_banks_native_open_media(media_path, &media_stream, &media_length) == 1);
    CHECK(media_stream && media_length == 5);
    FILE *unclaimed = NULL;
    CHECK(sh_audio_banks_native_open_media(wrong_path, &unclaimed, &media_length) == 0 && !unclaimed);
    int opens_before = rp_media_opens;
    CHECK(sh_audio_banks_native_open_media("sound/soundbanks/pc/900.wem", &unclaimed, &media_length) == 0);
    CHECK(sh_audio_banks_native_open_media("sound/soundbanks/pc/1.bnk", &unclaimed, &media_length) == 0);
    CHECK(rp_media_opens == opens_before);
    rp_media_failed = 1;
    CHECK(sh_audio_banks_native_open_media(media_path, &unclaimed, &media_length) == -1 && !unclaimed);
    rp_media_failed = 0;
    /* Leaving B for vanilla cannot remove a still-playing A's exact file. */
    CHECK(sh_audio_banks_native_programs_activate(NULL, 0, &baseline, rp_release, error, sizeof(error)));
    CHECK(sh_audio_banks_native_open_media(media_path, &unclaimed, &media_length) == 1);
    if (unclaimed) fclose(unclaimed);
    rp_set_refs(pa.ids[0], 1);
    /* Streaming may already be copying a retained owner when the main-thread
     * boundary unloads the last idle bank. Removal must wait for that read. */
    rp_reader_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    rp_reader_resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    rp_retirement_reached = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(rp_reader_entered && rp_reader_resume && rp_retirement_reached);
    rp_stream_read read = {media_path, NULL, 0, -2};
    int retirement_result = -2;
    HANDLE reader = CreateThread(NULL, 0, rp_stream_reader, &read, 0, NULL);
    CHECK(reader && WaitForSingleObject(rp_reader_entered, 5000) == WAIT_OBJECT_0);
    HANDLE retire = CreateThread(NULL, 0, rp_stream_retire, &retirement_result, 0, NULL);
    CHECK(retire && WaitForSingleObject(rp_retirement_reached, 5000) == WAIT_OBJECT_0);
    CHECK(rp_owners[0].references == 1);
    SetEvent(rp_reader_resume);
    CHECK(WaitForSingleObject(reader, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(retire, 5000) == WAIT_OBJECT_0);
    CHECK(read.result == 1 && read.stream && read.length == 5 && retirement_result == 0);
    if (read.stream) fclose(read.stream);
    CloseHandle(reader); CloseHandle(retire);
    CloseHandle(rp_reader_entered); CloseHandle(rp_reader_resume); CloseHandle(rp_retirement_reached);
    rp_reader_entered = rp_reader_resume = rp_retirement_reached = NULL;
    CHECK(!rp_find_slot(pa.ids[0], NULL) && rp_owners[0].references == 0 && !rp_cut_voices);
    CHECK(sh_audio_banks_native_open_media(media_path, &unclaimed, &media_length) == 0 && !unclaimed);
    /* An already-opened stream has its own lifetime beyond program retirement. */
    if (media_stream) {
        char bytes[6] = {0};
        rewind(media_stream); CHECK(fread(bytes, 1, 5, media_stream) == 5 && !strcmp(bytes, "map-a"));
        fclose(media_stream);
    }
    /* 4. Registered private objects outlive the bank: the lease waits for them. */
    set[0] = rp_ref(0, a.program, a.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    /* A streamed item still owns the private event object after the bank
     * itself became idle and unloaded. */
    uint32_t held = rp_first_exclusive(a.lease, pa.ids[0]);
    rp_register_object(held, pa.ids[0]); rp_retained_object = held;
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(!rp_find_slot(pa.ids[0], NULL) && sh_audio_banks_native_programs_count() == 2 && rp_owners[0].references == 1);
    CHECK(sh_audio_banks_native_programs_retire(error, sizeof(error)) == 1);
    rp_retained_object = 0; rp_drop_objects(pa.ids[0]);
    CHECK(sh_audio_banks_native_programs_retire(error, sizeof(error)) == 0 && rp_owners[0].references == 0);
    /* 5. A retiring program returns to service without another load. */
    set[0] = rp_ref(0, a.program, a.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    rp_set_refs(pa.ids[0], 2);
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    int loads_before = rp_loads;
    set[0] = rp_ref(0, a.program, a.lease); set[1] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 2, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_loads == loads_before + 0 && rp_owners[0].references == 1 && rp_owners[1].references == 1);
    CHECK(sh_audio_banks_native_programs_count() == 2);
    rp_set_refs(pa.ids[0], 1);
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_owners[0].references == 0 && sh_audio_banks_native_programs_count() == 1);
    /* 6. A failed second load unloads the first and keeps the prior program. */
    rp_fail_id = pc.ids[1]; rp_fail_id_count = 1;
    set[0] = rp_ref(2, c.program, c.lease);
    CHECK(!sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)) && *error);
    CHECK(!rp_find_slot(pc.ids[0], NULL) && !rp_find_slot(pc.ids[1], NULL) && rp_find_slot(pb.ids[0], NULL));
    CHECK(rp_owners[2].references == 0 && !sh_audio_id_lease_active(c.lease) && sh_audio_banks_native_programs_count() == 1);
    /* 7. Shared controls: an edit publishes, a departure restores the baseline. */
    set[0] = rp_ref(3, s.program, s.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(playback.threshold == -72 && !g_limits.pending && !g_bank_gate.scope);
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(playback.threshold == -60 && !g_limits.pending);
    /* A failing later leg restores the playback edit and unloads the new bank. */
    state_slot = NULL;
    set[0] = rp_ref(3, s.program, s.lease);
    CHECK(!sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(playback.threshold == -60 && !g_limits.pending && !g_bank_gate.scope && rp_owners[3].references == 0);
    state_slot = &states;
    /* 7b. A switch edit publishes through the switch leg; the departure
     * returns the group to its original (unmapped) state. */
    const sh_audio_shared_policy *bound = sh_package_audio_program_bound_shared(w.program); uint32_t controller = 0;
    for (size_t i = 0; bound && i < bound->count; i++)
        if (bound->settings[i].record.kind == SH_AUDIO_SHARED_SWITCH) memcpy(&controller, bound->settings[i].body + 4, 4);
    CHECK(controller);
    set[0] = rp_ref(5, w.program, w.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    sh_audio_switch_node *group = rp_switch(620);
    CHECK(group && group->controller == controller && group->values.count == 2 && ((uint32_t *)group->values.items)[1] == 5002);
    CHECK(!g_switch.pending && g_switch.original_count == 1 && rp_switch_refreshes == 1 && playback.threshold == -60);
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_switch(620) == group && !group->controller && !group->values.count && !g_switch.original_count && !g_switch.pending);
    /* 7c. A new bus exists before the private bank routing to it loads, is
     * configured afterwards, and is released once no program defines it. */
    const sh_audio_bus_policy *buses = sh_package_audio_program_bound_bus_policy(v.program); uint32_t made_id = 0;
    for (size_t i = 0; buses && i < buses->bus_count; i++) if (buses->buses[i].kind == 8) made_id = buses->buses[i].bus;
    CHECK(made_id && sh_package_audio_program_bus_edited(v.program));
    rp_plan pv = rp_loads_of(&v); rb_expect_bank = pv.ids[0]; rb_expect_bus = made_id;
    set[0] = rp_ref(6, v.program, v.lease); int made_recalcs = rb_recalc_notices;
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    unsigned char *made = rb_registered(made_id), *block = made ? ab_ptr(made + AB_PROPERTIES) : NULL;
    float made_volume = 0; if (block) memcpy(&made_volume, block + 4, 4);
    CHECK(made && ab_ptr(made + AB_PARENT) == rb_roots[0] && !rb_missing_at_load && rp_find_slot(pv.ids[0], NULL));
    /* The new bus has no registered instances to notify; it recalculates. */
    CHECK(block && block[0] == 1 && made_volume == -6.0f && !rb_value_notices && rb_recalc_notices == made_recalcs + 1);
    CHECK(g_bus.owned_count == 1 && !g_bus.pending && rb_refs(rb_roots[0]) == 2);
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(!rb_registered(made_id) && !g_bus.owned_count && rb_refs(rb_roots[0]) == 1);
    rb_expect_bus = rb_expect_bank = 0;
    /* 7d. An edit of a native bus after a bank-less program (R307). The
     * bank-less program activates first and carries no native baseline; the
     * map program's Init edit then adds Bus Volume (property 5) to native bus
     * 640. The edit publishes through the native bus, with its parent and
     * other properties unchanged, and leaving restores the native properties;
     * re-entry publishes it again. Before the union of program baselines this
     * failed with "audio bus 640 has no parent". Without registered instances
     * the edit and its restore recalculate the bus and send no value notice;
     * with them, each changed parameter is also notified. */
    char native_base[MAX_PATH], native_dirs[3][MAX_PATH], native_init[MAX_PATH];
    snprintf(native_base, sizeof(native_base), "%s\\native-bus-base", root);
    snprintf(native_dirs[0], MAX_PATH, "%s\\sound", native_base);
    snprintf(native_dirs[1], MAX_PATH, "%s\\soundbanks", native_dirs[0]);
    snprintf(native_dirs[2], MAX_PATH, "%s\\pc", native_dirs[1]);
    snprintf(native_init, sizeof(native_init), "%s\\init.bnk", native_dirs[2]);
    CHECK(CreateDirectoryA(native_base, NULL));
    for (size_t i = 0; i < 3; i++) CHECK(CreateDirectoryA(native_dirs[i], NULL));
    {
        unsigned char native_bank[160], native_keys[1] = {26}; float native_values[1] = {0}; FILE *stream = NULL;
        size_t length = rp_init_bus_bank(native_bank, 640, native_keys, native_values, 1);
        CHECK(!fopen_s(&stream, native_init, "wb") && stream);
        if (stream) { CHECK(fwrite(native_bank, 1, length, stream) == length); fclose(stream); }
    }
    sh_audio_originals *natives = sh_audio_originals_open(native_base, L"English(US)", NULL); CHECK(natives);
    {
        const unsigned char keys[2] = {5, 26}; const float values[2] = {-12.0f, 0};
        const sh_package_compilation *scope = &m.scope; sh_package_compilation output = {0};
        memset(&m, 0, sizeof(m)); snprintf(m.paths[0], sizeof(m.paths[0]), "sound/soundbanks/pc/init.bnk");
        m.files[0].engine_path = m.paths[0]; m.files[0].body = m.banks[0];
        m.files[0].body_length = rp_init_bus_bank(m.banks[0], 640, keys, values, 2);
        m.files[0].baseline_known = 1; m.files[0].owners.bits = m.files[0].gameplay_owners.bits = 1;
        m.scope.resources = m.files; m.scope.resource_count = 1;
        m.program = sh_package_audio_program_open(&scope, 1, natives, error, sizeof(error));
        int ok = m.program && sh_package_audio_program_prepare(m.program, error, sizeof(error)) &&
            (m.lease = sh_audio_id_lease_begin(pool)) != NULL &&
            sh_package_audio_program_bind(m.program, &output, sh_audio_id_reserve, m.lease, error, sizeof(error));
        if (!ok) fprintf(stderr, "native bus edit program: %s\n", error);
        CHECK(ok && sh_package_audio_program_bus_edited(m.program));
        const sh_audio_bus_policy *native = sh_package_audio_program_bound_baseline_bus(m.program);
        CHECK(native && native->count && sh_audio_effective_bus_kind(native, 640) == 8);
    }
    {
        const sh_package_compilation *scope = &l.scope; sh_package_compilation output = {0};
        memset(&l, 0, sizeof(l)); l.scope.resources = l.files;
        l.program = sh_package_audio_program_open(&scope, 1, natives, error, sizeof(error));
        int ok = l.program && sh_package_audio_program_prepare(l.program, error, sizeof(error)) &&
            (l.lease = sh_audio_id_lease_begin(pool)) != NULL &&
            sh_package_audio_program_bind(l.program, &output, sh_audio_id_reserve, l.lease, error, sizeof(error));
        if (!ok) fprintf(stderr, "bank-less program: %s\n", error);
        const sh_audio_bus_policy *none = ok ? sh_package_audio_program_bound_baseline_bus(l.program) : NULL;
        CHECK(ok && none && !none->count);
    }
    /* Native bus 640 under the master bus, as Init loads it: properties {26: 0}. */
    unsigned char *native_bus = rb_construct(program_state_allocate(17, AB_SIZE), 640), *native_block = calloc(1, 8);
    CHECK(native_bus && native_block); rb_initialize(native_bus);
    ab_set_ptr(native_bus + AB_PARENT, rb_roots[0]); rb_addref(rb_roots[0]);
    native_block[0] = 1; native_block[1] = 26; ab_set_ptr(native_bus + AB_PROPERTIES, native_block);
    sh_audio_bus_policy_free(&g_bus_baseline); g_bus_baseline_known = 0;
    set[0] = rp_ref(8, l.program, l.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(!g_bus_baseline.count && !g_bus.pending);
    unsigned char *instances = NULL;
    for (int round = 0; round < 3; round++) {
        if (round == 2) { instances = calloc(1, 0x20); CHECK(instances); ab_set_ptr(native_bus + AB_MASKS, instances); }
        int notices = rb_value_notices, recalcs = rb_recalc_notices; float edited = 0;
        set[0] = rp_ref(7, m.program, m.lease); set[1] = rp_ref(8, l.program, l.lease);
        int ok = sh_audio_banks_native_programs_activate(set, 2, &baseline, rp_release, error, sizeof(error));
        if (!ok) fprintf(stderr, "native bus edit, round %d: %s\n", round, error);
        CHECK(ok);
        const unsigned char *block = ab_ptr(native_bus + AB_PROPERTIES);
        int has5 = block && block[0] == 2 && ((block[1] == 5 && block[2] == 26) || (block[1] == 26 && block[2] == 5));
        if (has5) memcpy(&edited, block + 4 + 4 * (block[1] == 5 ? 0 : 1), 4);
        CHECK(has5 && edited == -12.0f && ab_ptr(native_bus + AB_PARENT) == rb_roots[0] && rb_recalc_notices == recalcs + 1);
        CHECK(rb_value_notices == notices + (instances ? 1 : 0));
        CHECK(!g_bus.pending && !g_bus.owned_count && rb_registered(640) == native_bus);
        set[0] = rp_ref(8, l.program, l.lease);
        ok = sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error));
        if (!ok) fprintf(stderr, "native bus restore, round %d: %s\n", round, error);
        CHECK(ok);
        block = ab_ptr(native_bus + AB_PROPERTIES);
        CHECK(block && block[0] == 1 && block[1] == 26 && ab_ptr(native_bus + AB_PARENT) == rb_roots[0] && !g_bus.pending);
        CHECK(rb_recalc_notices == recalcs + 2 && rb_value_notices == notices + (instances ? 2 : 0));
    }
    ab_set_ptr(native_bus + AB_MASKS, NULL); free(instances);
    /* 7e (native edit). A committed program that then only serves, as a
     * rejected candidate's program does in recovery, loses its native bus
     * edit; the program itself stays loaded for its consumers. */
    {
        set[0] = rp_ref(7, m.program, m.lease); set[1] = rp_ref(8, l.program, l.lease);
        CHECK(sh_audio_banks_native_programs_activate(set, 2, &baseline, rp_release, error, sizeof(error)));
        const unsigned char *block = ab_ptr(native_bus + AB_PROPERTIES);
        CHECK(block && block[0] == 2);
        set[0] = rp_ref(8, l.program, l.lease); set[1] = rp_serve(7, m.program, m.lease);
        int ok = sh_audio_banks_native_programs_activate(set, 2, &baseline, rp_release, error, sizeof(error));
        if (!ok) fprintf(stderr, "serving program: %s\n", error);
        block = ab_ptr(native_bus + AB_PROPERTIES);
        CHECK(ok && block && block[0] == 1 && block[1] == 26 && ab_ptr(native_bus + AB_PARENT) == rb_roots[0]);
        CHECK(!g_bus.pending && !g_bus.owned_count && rp_owners[7].references == 1 && sh_audio_banks_native_programs_count() == 2);
    }
    set[0] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_owners[7].references == 0 && rp_owners[8].references == 0);
    /* Its bank unloads: detach from the master bus and drop the bank's reference. */
    rb_detach(rb_roots[0], native_bus); CHECK(rb_refs(native_bus) == 1); rb_release(native_bus);
    CHECK(!rb_registered(640) && rb_refs(rb_roots[0]) == 1);
    /* 7e. A serving program (R307) is kept only for consumers that began while
     * it was exposed. Its banks load, and the bus it introduces exists before
     * them and is configured; its shared controls are not published, and the
     * authority programs' policy stays in force. Leaving retires it. */
    {
        rp_plan ps = rp_loads_of(&s);
        set[0] = rp_ref(1, b.program, b.lease); set[1] = rp_serve(3, s.program, s.lease);
        int ok = sh_audio_banks_native_programs_activate(set, 2, &baseline, rp_release, error, sizeof(error));
        if (!ok) fprintf(stderr, "serving shared program: %s\n", error);
        CHECK(ok && playback.threshold == -60 && !g_limits.pending && rp_owners[3].references == 1);
        CHECK(!ps.count || rp_find_slot(ps.ids[0], NULL));
        rp_plan pv2 = rp_loads_of(&v); rb_expect_bank = pv2.ids[0]; rb_expect_bus = made_id; rb_missing_at_load = 0;
        set[0] = rp_ref(1, b.program, b.lease); set[1] = rp_serve(6, v.program, v.lease);
        ok = sh_audio_banks_native_programs_activate(set, 2, &baseline, rp_release, error, sizeof(error));
        if (!ok) fprintf(stderr, "serving bus program: %s\n", error);
        unsigned char *served = rb_registered(made_id), *served_block = served ? ab_ptr(served + AB_PROPERTIES) : NULL;
        float served_volume = 0; if (served_block) memcpy(&served_volume, served_block + 4, 4);
        CHECK(ok && served && ab_ptr(served + AB_PARENT) == rb_roots[0] && !rb_missing_at_load && rp_find_slot(pv2.ids[0], NULL));
        CHECK(served_block && served_block[0] == 1 && served_volume == -6.0f && g_bus.owned_count == 1 && rp_owners[3].references == 0);
        set[0] = rp_ref(1, b.program, b.lease);
        CHECK(sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
        CHECK(!rb_registered(made_id) && !g_bus.owned_count && rp_owners[6].references == 0 && playback.threshold == -60);
        rb_expect_bus = rb_expect_bank = 0;
    }
    /* 8. Additive admission: rollback unloads, accept activates the lease. */
    sh_audio_bank_addition *addition = sh_audio_banks_native_additions_begin(error, sizeof(error)); CHECK(addition);
    set[0] = rp_ref(4, d.program, d.lease);
    CHECK(sh_audio_banks_native_programs_add(addition, set, 1, rp_release, error, sizeof(error)));
    CHECK(rp_find_slot(pd.ids[0], NULL) && !sh_audio_id_lease_active(d.lease));
    CHECK(sh_audio_banks_native_additions_end(&addition, 0, error, sizeof(error)) && !addition);
    CHECK(!rp_find_slot(pd.ids[0], NULL) && rp_owners[4].references == 0 && sh_audio_banks_native_programs_count() == 1);
    addition = sh_audio_banks_native_additions_begin(error, sizeof(error)); CHECK(addition);
    set[0] = rp_ref(4, d.program, d.lease);
    CHECK(sh_audio_banks_native_programs_add(addition, set, 1, rp_release, error, sizeof(error)));
    rp_set_refs(pd.ids[0], 2);
    CHECK(sh_audio_banks_native_additions_end(&addition, 0, error, sizeof(error)) && !addition);
    CHECK(rp_find_slot(pd.ids[0], NULL) && rp_owners[4].references == 1);
    rp_set_refs(pd.ids[0], 1);
    CHECK(sh_audio_banks_native_programs_retire(error, sizeof(error)) == 0 && !rp_owners[4].references);
    addition = sh_audio_banks_native_additions_begin(error, sizeof(error)); CHECK(addition);
    set[0] = rp_ref(4, d.program, d.lease); set[1] = rp_ref(1, b.program, b.lease);
    CHECK(sh_audio_banks_native_programs_add(addition, set, 2, rp_release, error, sizeof(error)));
    CHECK(sh_audio_banks_native_additions_ready(addition, error, sizeof(error)));
    sh_audio_banks_native_additions_accept(&addition);
    CHECK(sh_audio_id_lease_active(d.lease) && rp_owners[1].references == 1 && rp_owners[4].references == 1);
    /* 9. An identity taken natively before activation refuses the program. */
    rp_register_object(pc.ids[0], 0);
    set[0] = rp_ref(2, c.program, c.lease);
    CHECK(!sh_audio_banks_native_programs_activate(set, 1, &baseline, rp_release, error, sizeof(error)));
    CHECK(rp_owners[2].references == 0 && rp_find_slot(pb.ids[0], NULL));
    rp_drop_objects(0);
    /* 10. Leaving every program restores baseline controls and retires all. */
    CHECK(sh_audio_banks_native_programs_activate(NULL, 0, &baseline, rp_release, error, sizeof(error)));
    CHECK(sh_audio_banks_native_programs_count() == 0 && !rp_find_slot(pb.ids[0], NULL) && !rp_find_slot(pd.ids[0], NULL));
    for (size_t i = 0; i < 10; i++) CHECK(rp_owners[i].references == 0);
    sh_package_file_release(rp_media_file); rp_media_file = NULL;
    { char path[MAX_PATH]; snprintf(path, sizeof(path), "%s/retiring-a.wem", root); CHECK(DeleteFileA(path)); }
    CHECK(!rp_cut_voices && !g_pbank_count);
    rp_program *all[] = {&a, &b, &c, &s, &d, &w, &v, &m, &l};
    for (size_t i = 0; i < 9; i++) { sh_audio_id_lease_release(all[i]->lease); sh_package_audio_program_close(all[i]->program); }
    CHECK(sh_audio_id_pool_close(pool));
    sh_audio_shared_policy_free(&baseline); sh_audio_originals_close(originals); sh_audio_originals_close(natives);
    CHECK(DeleteFileA(native_init));
    for (size_t i = 3; i-- > 0;) CHECK(RemoveDirectoryA(native_dirs[i]));
    CHECK(RemoveDirectoryA(native_base));
    DeleteCriticalSection(&g_bank_gate.lock); memset(&g_bank_gate, 0, sizeof(g_bank_gate));
    for (uint32_t n = 0; n < states.count; n++) { program_state_destroy(states.items[n].group); free(states.items[n].group); }
    free(states.items); memset(&states, 0, sizeof(states));
    memset(&g_state, 0, sizeof(g_state)); memset(&g_parameter, 0, sizeof(g_parameter));
    memset(&g_environment, 0, sizeof(g_environment)); memset(&g_limits, 0, sizeof(g_limits));
    for (unsigned i = 0; i < 193; i++) for (sh_audio_switch_node *n = rp_switches.table.buckets[i]; n;) {
        sh_audio_switch_node *next = n->next; free(n->values.items); free(n); n = next;
    }
    for (unsigned i = 0; i < 193; i++) for (sh_audio_parameter_node *n = parameters.table.buckets[i]; n;) {
        sh_audio_parameter_node *next = n->next; rp_parameter_destroy(n); free(n); n = next;
    }
    memset(&rp_switches, 0, sizeof(rp_switches)); memset(&parameters, 0, sizeof(parameters));
    aw_originals_free(g_switch.originals, g_switch.original_count);
    memset(&g_switch, 0, sizeof(g_switch)); memset(&g_subscription, 0, sizeof(g_subscription));
    while (rb_object_count) { unsigned char *o = rb_objects[0]; rb_set_refs(o, 1); rb_release(o); }
    free(g_bus.owned); free(g_bus.touched); memset(&g_bus, 0, sizeof(g_bus));
    sh_audio_bus_policy_free(&g_bus_baseline); sh_audio_bus_policy_free(&g_bus_effective); g_bus_baseline_known = 0;
    DeleteCriticalSection(&lock);
    g_commands = saved_commands; g_registry = saved_registry; g_load = saved_load; g_unload = saved_unload;
    for (size_t i = 0; i < 12; i++) DeleteCriticalSection((CRITICAL_SECTION *)(rp_index + i * 0x638));
    DeleteCriticalSection((CRITICAL_SECTION *)(rp_manager + 0x48)); DeleteCriticalSection(&rp_bank_lock);
    printf("retained programs: %d native loads, %d unloads, %d voices cut\n", rp_loads, rp_unloads, rp_cut_voices);
    free(rp_manager); free(rp_index); RemoveDirectoryA(base);
}
int main(void)
{
    char temp[MAX_PATH], locale[MAX_PATH], other[MAX_PATH], path[MAX_PATH], error[256];
    uint32_t event;
    CHECK(GetTempPathA(sizeof(temp),temp)>0);
    snprintf(root,sizeof(root),"%ssh-audio-%lu",temp,(unsigned long)GetCurrentProcessId());
    snprintf(locale,sizeof(locale),"%s\\English(US)",root);
    snprintf(other,sizeof(other),"%s\\French(France)",root);
    CHECK(CreateDirectoryA(root,NULL)); CHECK(CreateDirectoryA(locale,NULL));
    CHECK(CreateDirectoryA(other,NULL));
    snprintf(path,sizeof(path),"%s\\a.bnk",root); bank_file(path,BANK_A,7,0,0,0,0);
    snprintf(path,sizeof(path),"%s\\b.bnk",root); bank_file(path,BANK_B,13,0,BANK_A,900,902);
    snprintf(path,sizeof(path),"%s\\777.bnk",root); bank_file(path,BANK_N,21,0,0,0,0);
    snprintf(path,sizeof(path),"%s\\a.bnk",locale); bank_file(path,BANK_A,99,1,0,0,0);
    snprintf(path,sizeof(path),"%s\\c.bnk",other); bank_file(path,BANK_C,55,2,0,0,0);
    sh_audio_banks_native_install(NULL,0);
    numeric_name_dispatch();

    language_scoped_catalog(error,sizeof(error));
    dependency_closure(error,sizeof(error));
    media_interaction(error,sizeof(error));

    /* Discovery follows the engine's configured bank prefix, not an assumed
     * plain root: a prefixed install exposes only the prefixed banks. */
    {
        char prefixed[MAX_PATH], prefixed_locale[MAX_PATH];
        uint32_t event=7;
        snprintf(prefixed,sizeof(prefixed),"%s\\banks",root);
        snprintf(prefixed_locale,sizeof(prefixed_locale),"%s\\English(US)",prefixed);
        CHECK(CreateDirectoryA(prefixed,NULL)); CHECK(CreateDirectoryA(prefixed_locale,NULL));
        snprintf(path,sizeof(path),"%s\\a.bnk",prefixed); bank_file(path,BANK_A,7,0,0,0,0);
        snprintf(path,sizeof(path),"%s\\c.bnk",prefixed_locale); bank_file(path,BANK_C,55,1,0,0,0);
        an_clear_catalog(); g_catalog_ready=0;
        memset(loaded,0,sizeof(loaded)); loads=releases=0;
        strcpy_s(prefix_name,sizeof(prefix_name),"banks/");
        CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
        CHECK(g_count==2 && loaded[1] && loads==1);
        CHECK(!strcmp(g_prefix,"banks/"));
        event=55; /* The prefixed language directory is indexed as well. */
        CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
        CHECK(loaded[3] && !loaded[1]);
        event=13; /* b.bnk sits at the unprefixed root and is not in scope. */
        CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
        CHECK(!loaded[2] && !loaded[3]);
        prefix_name[0]=0; an_clear_catalog(); g_catalog_ready=0;
        snprintf(path,sizeof(path),"%s\\a.bnk",prefixed); CHECK(DeleteFileA(path));
        snprintf(path,sizeof(path),"%s\\c.bnk",prefixed_locale); CHECK(DeleteFileA(path));
        CHECK(RemoveDirectoryA(prefixed_locale)); CHECK(RemoveDirectoryA(prefixed));
        memset(loaded,0,sizeof(loaded)); loads=releases=0;
        event=99; /* Rebuild the unprefixed catalog for the checks that follow. */
        CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
        CHECK(g_count==3 && !g_prefix[0]);
        CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));
        memset(loaded,0,sizeof(loaded)); loads=releases=0;
    }

    /* An installed originals index replaces the loose bytes with the effective
     * packed candidate, and reports package identities that have no name. */
    CHECK(g_catalog_provisional);
    originals_ready=1; packaged_identity=UINT32_C(555);
    shadow_length=bank_bytes(shadow,BANK_A,UINT32_C(1234),1,0,0,0);
    shadowed_path="sound/soundbanks/pc/english(us)/a.bnk";
    CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));
    CHECK(!g_catalog_provisional && !g_packaged_only);
    event=1234;
    CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
    CHECK(loaded[1]); /* The packed copy supplied this event, the loose one did not. */
    event=99;
    CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
    CHECK(!loaded[1]);
    packaged_identity=0; shadowed_path=NULL;
    packaged_failed=1; an_clear_catalog(); g_catalog_ready=0;
    CHECK(!activate(NULL,1,NULL,0,error,sizeof(error)));
    packaged_failed=0;

    /* A mismatched bank identity invalidates the complete catalog before calls. */
    an_clear_catalog(); g_catalog_ready=0;
    snprintf(path,sizeof(path),"%s\\b.bnk",root); bank_file(path,BANK_A,13,0,0,0,0);
    event=7;
    CHECK(!activate(&event,1,NULL,0,error,sizeof(error)));
    CHECK(!g_count && !g_catalog_ready);
    CHECK(strstr(error,"filename")!=NULL);
    bank_file(path,BANK_B,13,0,BANK_A,900,902);
    CHECK(activate(&event,1,NULL,0,error,sizeof(error)));
    CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));

    {   /* Two installed names that fold to one identity refuse the catalog. */
        int previous_loads=loads;
        uint32_t first=0, second=0;
        const char *first_name="fixture_zoq7xy1tkidsi.bnk";
        const char *second_name="fixture_1lf8xob10q04s6.bnk";
        char one[MAX_PATH], two[MAX_PATH];
        an_clear_catalog(); g_catalog_ready=0;
        CHECK(sh_audio_bank_identity(first_name,&first));
        CHECK(sh_audio_bank_identity(second_name,&second));
        CHECK(first==UINT32_C(626941048) && first==second);
        snprintf(one,sizeof(one),"%s\\%s",root,first_name);
        snprintf(two,sizeof(two),"%s\\%s",root,second_name);
        bank_file(one,first,7,0,0,0,0); bank_file(two,second,13,0,0,0,0);
        event=7;
        CHECK(!activate(&event,1,NULL,0,error,sizeof(error)));
        CHECK(strstr(error,"ambiguous")!=NULL);
        CHECK(!g_count && !g_catalog_ready && loads==previous_loads);
        CHECK(DeleteFileA(one)); CHECK(DeleteFileA(two));
    }

    /* Effective compiled bytes override prepared installed metadata, including
     * bank-only packages. A native-owned original is restored on map exit. */
    an_clear_catalog(); g_catalog_ready=0;
    memset(loaded,0,sizeof(loaded));loads=releases=0;loaded[1]=1;desired[1]=1;
    uint32_t changed_event=17, other_event=19;
    sh_audio_bank changed={BANK_A,0,&changed_event,1}, added={BANK_C,0,&other_event,1};
    sh_package_audio_bank packages[3]={
        {"sound/soundbanks/pc/a.bnk","a.bnk",&changed,{49,{1}},1},
        {"sound/soundbanks/pc/english(us)/a.bnk","a.bnk",&changed,{49,{2}},1},
        {"sound/soundbanks/pc/c.bnk","c.bnk",&added,{49,{3}},1}
    };
    CHECK(activate(NULL,0,packages,1,error,sizeof(error)));
    CHECK(actual[1]==1 && loads==2 && releases==1 && slot(BANK_A)->restore_native);
    CHECK(activate(NULL,0,packages,1,error,sizeof(error)));
    CHECK(releases==1);
    desired[1]=2;
    CHECK(activate(NULL,0,packages,2,error,sizeof(error)));
    CHECK(actual[1]==2 && releases==2); /* Current locale, not root payload. */
    desired[1]=0;
    CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));
    CHECK(loaded[1] && actual[1]==0 && !slot(BANK_A)->owned && !slot(BANK_A)->restore_native);
    original_locale=1;int before=loads;
    CHECK(activate(NULL,0,packages,1,error,sizeof(error)));
    CHECK(loads==before); /* Native localized original shadows the root package. */
    original_locale=0;original_failed=1;
    CHECK(!activate(NULL,0,packages,1,error,sizeof(error)));
    CHECK(loads==before && strstr(error,"original read failed"));
    original_failed=0;paths_failed=1;
    CHECK(!activate(NULL,0,packages,1,error,sizeof(error)));
    CHECK(loads==before);paths_failed=0;
    desired[1]=1;
    CHECK(activate(NULL,0,packages,1,error,sizeof(error)));
    desired[1]=2;fail_load_call=loads+1;
    CHECK(!activate(NULL,0,packages,2,error,sizeof(error)));
    CHECK(!loaded[1] && slot(BANK_A)->restore_native);
    fail_load_call=0;desired[1]=1;
    CHECK(activate(NULL,0,packages,1,error,sizeof(error)));
    CHECK(loaded[1] && actual[1]==1);
    desired[1]=0;
    CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));
    CHECK(loaded[1] && actual[1]==0);
    desired[3]=3;
    CHECK(activate(NULL,0,packages+2,1,error,sizeof(error)));
    CHECK(g_count==4 && loaded[3] && actual[3]==3); /* No installed loose c.bnk. */
    CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));
    CHECK(!loaded[3] && loaded[1] && g_count==3); /* Retire only the added bank. */
    before=loads;
    packages[2].filename="a.bnk";
    CHECK(!activate(NULL,0,packages+2,1,error,sizeof(error)));
    CHECK(loads==before);
    CHECK(!activate(NULL,1,NULL,0,error,sizeof(error)));

    /* A changed media carried inside an unchanged installed bank is reported
     * rather than silently half-applied; a streamed one is simply served. */
    {
        sh_package_audio_media media[2]={
            {"sound/soundbanks/pc/900.wem","900.wem",900,{4,{9}},1},
            {"sound/soundbanks/pc/902.wem","902.wem",902,{4,{9}},1}
        };
        uint32_t wanted=13;
        an_clear_catalog(); g_catalog_ready=0;
        memset(loaded,0,sizeof(loaded));loads=releases=0;logged[0]=0;
        /* 900 is carried inside b.bnk, so replacing only the loose file refuses
         * with both names. 902 is streamed and simply served. */
        CHECK(!sh_audio_banks_native_activate(&wanted,1,NULL,0,media,2,error,sizeof(error)));
        CHECK(strstr(error,"900.wem cannot replace media 900") && strstr(error,"'b.bnk'"));
        CHECK(!strstr(error,"902"));
        logged[0]=0;
        CHECK(sh_audio_banks_native_activate(NULL,0,NULL,0,media,2,error,sizeof(error)));
        /* Nothing plays from a bank this pass leaves out, so nothing is claimed. */
        CHECK(!strstr(logged,"cannot replace"));
        CHECK(sh_audio_banks_native_activate(&wanted,1,NULL,0,media+1,1,error,sizeof(error)));
        CHECK(loaded[2] && strstr(logged,"1 changed media served"));
        CHECK(sh_audio_banks_native_activate(NULL,0,NULL,0,NULL,0,error,sizeof(error)));
    }

    /* Portable imports append new banks without disturbing an existing voice.
     * Rollback targets only their own successful loads; the session excludes
     * unrelated normal reconciliation until it has finished. */
    {
        an_clear_catalog(); g_catalog_ready=0;
        memset(loaded,0,sizeof(loaded)); loads=releases=0;
        desired[1]=1; desired[3]=3; packages[2].filename="c.bnk";
        sh_package_audio_bank both[2]={packages[0],packages[2]};
        CHECK(activate(NULL,0,packages,1,error,sizeof(error)));
        sh_audio_bank_addition *addition=sh_audio_banks_native_additions_begin(error,sizeof(error));
        CHECK(addition && !sh_audio_banks_native_additions_begin(error,sizeof(error)));
        CHECK(!sh_audio_banks_native_additions_ready(addition,error,sizeof(error)));
        CHECK(sh_audio_banks_native_activate_additive(addition,NULL,0,both,2,NULL,0,error,sizeof(error)));
        CHECK(loaded[1] && loaded[3] && actual[1]==1 && !releases);
        CHECK(!activate(NULL,0,NULL,0,error,sizeof(error)) && !releases);
        CHECK(sh_audio_banks_native_additions_end(&addition,0,error,sizeof(error)) && !addition);
        CHECK(loaded[1] && !loaded[3] && releases==1 && slot(BANK_A)->owned);
        addition=sh_audio_banks_native_additions_begin(error,sizeof(error));
        int prior_loads=loads, prior_releases=releases;
        CHECK(!sh_audio_banks_native_activate_additive(addition,NULL,0,packages,2,NULL,0,error,sizeof(error)));
        CHECK(loads==prior_loads && releases==prior_releases && loaded[1]);
        CHECK(sh_audio_banks_native_additions_end(&addition,0,error,sizeof(error)));
        addition=sh_audio_banks_native_additions_begin(error,sizeof(error));
        CHECK(sh_audio_banks_native_activate_additive(addition,NULL,0,both,2,NULL,0,error,sizeof(error)));
        CHECK(sh_audio_banks_native_additions_ready(addition,error,sizeof(error)));
        sh_audio_banks_native_additions_accept(&addition);
        CHECK(!addition && !g_addition);
        CHECK(loaded[1] && loaded[3]);
        CHECK(activate(NULL,0,NULL,0,error,sizeof(error)));
        CHECK(!loaded[1] && !loaded[3]);
    }
    an_clear_catalog();g_catalog_ready=0;
    compiled_playback_settings();
    prepared_native_slot();
    retained_programs();
    snprintf(path,sizeof(path),"%s\\a.bnk",root); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\b.bnk",root); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\777.bnk",root); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\a.bnk",locale); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\c.bnk",other); CHECK(DeleteFileA(path));
    CHECK(RemoveDirectoryA(locale)); CHECK(RemoveDirectoryA(other)); CHECK(RemoveDirectoryA(root));
    if(failures) return 1;
    puts("native audio bank catalog tests passed"); return 0;
}
