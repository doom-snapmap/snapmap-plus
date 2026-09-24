#include <stdio.h>
#include <string.h>
#include "../src/backend/audio_bank_commands.c"
#include "../src/backend/audio_limits_native.c"
#include "../src/backend/audio_environment_native.c"
#include "../src/backend/audio_state_native.c"
#include "../src/backend/audio_parameter_native.c"
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
    snprintf(path,sizeof(path),"%s\\a.bnk",root); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\b.bnk",root); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\777.bnk",root); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\a.bnk",locale); CHECK(DeleteFileA(path));
    snprintf(path,sizeof(path),"%s\\c.bnk",other); CHECK(DeleteFileA(path));
    CHECK(RemoveDirectoryA(locale)); CHECK(RemoveDirectoryA(other)); CHECK(RemoveDirectoryA(root));
    if(failures) return 1;
    puts("native audio bank catalog tests passed"); return 0;
}
