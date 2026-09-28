#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_audio.h"
#include "package_audio_capture.h"
#include "package_audio_program.h"
#include "package_sound.h"
#include "package_fixture.h"

static void put32(unsigned char *p, uint32_t n)
{ for (int i=0;i<4;i++) p[i]=(unsigned char)(n>>(8*i)); }
/* One event with an empty action list, in the verified v113 object shape. */
#define BANK_BYTES 49
static void bank_bytes(unsigned char *bytes, uint32_t id, uint32_t event, uint32_t language)
{
    memset(bytes,0,BANK_BYTES); memcpy(bytes,"BKHD",4); put32(bytes+4,16); put32(bytes+8,113);
    put32(bytes+12,id); put32(bytes+16,language);
    memcpy(bytes+24,"HIRC",4); put32(bytes+28,17); put32(bytes+32,1);
    bytes[36]=4; put32(bytes+37,8); put32(bytes+41,event);
}

static void memory_index(void)
{
    unsigned char a[BANK_BYTES], locale[BANK_BYTES], b[BANK_BYTES], bad[BANK_BYTES];
    sh_compiled_resource resources[5] = {0};
    sh_package_compilation compiled = {0};
    sh_package_audio_report report;
    sh_package_audio *audio;
    const sh_package_audio_reference *references = NULL;
    const sh_package_audio_bank *banks = NULL;
    sh_package_file_identity wanted;
    char error[1024];
    bank_bytes(a,84696446,7,0); bank_bytes(locale,84696446,7,12);
    bank_bytes(b,84696445,13,0); bank_bytes(bad,42,17,0);
    resources[0].engine_path="sound/soundbanks/pc/english(us)/a.bnk"; resources[0].body=locale;
    resources[1].engine_path="sound/soundbanks/pc/b.bnk"; resources[1].body=b;
    resources[2].engine_path="sound/soundbanks/pc/a.bnk"; resources[2].body=a;
    resources[3].engine_path="sound/soundbanks/pc/mismatch.bnk"; resources[3].body=bad;
    resources[4].engine_path="author-notes/fake.bnk"; resources[4].body=(unsigned char *)"not a bank";
    for(size_t i=0;i<5;i++) resources[i].body_length=BANK_BYTES;
    compiled.resources=resources; compiled.resource_count=5;
    resources[0].owners.bits=1;
    resources[1].owners.bits=1; resources[1].restored_original=1;
    audio=sh_package_audio_open(&compiled,&report,error,sizeof(error)); CHECK(audio);
    if(!audio) return;
    CHECK(report.banks==3 && report.invalid==1 && strstr(report.first_gap,"mismatch.bnk"));
    CHECK(sh_package_audio_banks(audio,&banks)==3 && banks);
    CHECK(banks && banks[0].metadata->language==12 && banks[2].metadata->id==84696446);
    CHECK(banks && banks[0].contributed && !banks[1].contributed && !banks[2].contributed);
    CHECK(sh_package_bytes_identity(a,sizeof(a),&wanted));
    CHECK(banks && banks[2].identity.length==sizeof(a) && !memcmp(banks[2].identity.digest,wanted.digest,32));
    CHECK(!sh_package_audio_banks(audio,NULL));
    CHECK(sh_package_audio_find(audio,7,&references)==2);
    CHECK(references && !strcmp(references[0].path,"sound/soundbanks/pc/a.bnk"));
    CHECK(references && references[0].bank==84696446 && !references[0].language && references[1].language==12);
    CHECK(references && !strcmp(references[1].filename,"a.bnk"));
    CHECK(sh_package_audio_find(audio,13,&references)==1 && references[0].bank==84696445);
    CHECK(!sh_package_audio_find(audio,17,&references) && !references);
    CHECK(!sh_package_audio_find(audio,99,&references) && !references);
    CHECK(!sh_package_audio_find(audio,0,&references) && !references);
    CHECK(!sh_package_audio_find(audio,7,NULL));
    memset(a,0,sizeof(a)); memset(locale,0,sizeof(locale));
    CHECK(sh_package_audio_find(audio,7,&references)==2); /* Index owns its captured metadata. */
    CHECK(banks && !memcmp(banks[2].identity.digest,wanted.digest,32));
    sh_package_audio_close(audio);
    audio=sh_package_audio_open(&compiled,&report,error,sizeof(error)); CHECK(audio);
    CHECK(report.banks==1 && report.invalid==3);
    sh_package_audio_close(audio);
    CHECK(!sh_package_audio_open(NULL,&report,error,sizeof(error)) && error[0]);
    compiled.resource_count=0;
    audio=sh_package_audio_open(&compiled,&report,error,sizeof(error)); CHECK(audio && !report.banks);
    sh_package_audio_close(audio); sh_package_audio_close(NULL);
    CHECK(!sh_package_audio_banks(NULL,&banks) && !banks);
}

/* Media files are inventoried by their native decimal identity so that a changed
 * payload can be matched against the banks naming it. A bank named by identity
 * rather than by hash is the same bank to the native loader. */
static void media_index(void)
{
    unsigned char numbered[BANK_BYTES];
    unsigned char first[3]={1,2,3}, second[4]={4,5,6,7};
    sh_compiled_resource resources[5]={0};
    sh_package_compilation compiled={0};
    sh_package_audio_report report;
    sh_package_audio *audio;
    const sh_package_audio_media *media=NULL;
    const sh_package_audio_bank *banks=NULL;
    char error[1024];
    bank_bytes(numbered,4242,7,0);
    resources[0].engine_path="sound/soundbanks/pc/900.wem"; resources[0].body=first;
    resources[0].body_length=sizeof(first); resources[0].owners.bits=1;
    resources[1].engine_path="sound/soundbanks/pc/english(us)/901.wem"; resources[1].body=second;
    resources[1].body_length=sizeof(second);
    resources[2].engine_path="sound/soundbanks/pc/voice.wem"; resources[2].body=second;
    resources[2].body_length=sizeof(second);
    resources[3].engine_path="sound/soundbanks/pc/4242.bnk"; resources[3].body=numbered;
    resources[3].body_length=sizeof(numbered);
    resources[4].engine_path="author-notes/902.wem"; resources[4].body=first;
    resources[4].body_length=sizeof(first);
    compiled.resources=resources; compiled.resource_count=5;
    audio=sh_package_audio_open(&compiled,&report,error,sizeof(error)); CHECK(audio);
    if(!audio) return;
    CHECK(report.banks==1 && report.media==2 && report.unnamed_media==1 && !report.invalid);
    CHECK(sh_package_audio_banks(audio,&banks)==1 && banks && banks[0].metadata->id==4242);
    CHECK(sh_package_audio_media_files(audio,&media)==2 && media);
    CHECK(media && media[0].id==900 && media[1].id==901);
    CHECK(media && media[0].contributed && !media[1].contributed);
    CHECK(media && media[0].identity.length==sizeof(first));
    CHECK(media && !strcmp(media[1].filename,"901.wem"));
    CHECK(!sh_package_audio_media_files(audio,NULL));
    CHECK(!sh_package_audio_media_files(NULL,&media) && !media);
    sh_package_audio_close(audio);
}

static void bank_content_identity(void)
{
    unsigned char bytes[BANK_BYTES];char error[256];
    sh_compiled_resource resource={0};sh_package_compilation compiled={0};
    sh_package_audio_report report;const sh_package_audio_bank *before=NULL,*after=NULL;
    bank_bytes(bytes,84696446,7,0);
    resource.engine_path="sound/soundbanks/pc/a.bnk";resource.body=bytes;resource.body_length=sizeof(bytes);
    compiled.resources=&resource;compiled.resource_count=1;
    sh_package_audio *first=sh_package_audio_open(&compiled,&report,error,sizeof(error));CHECK(first);
    bytes[20]=1; /* Header data changes without changing event membership. */
    sh_package_audio *second=sh_package_audio_open(&compiled,&report,error,sizeof(error));CHECK(second);
    CHECK(sh_package_audio_banks(first,&before)==1 && sh_package_audio_banks(second,&after)==1);
    if(before && after) {
        CHECK(before->metadata->events[0]==after->metadata->events[0]);
        CHECK(before->identity.length==after->identity.length && memcmp(before->identity.digest,after->identity.digest,32));
    }
    sh_package_audio_close(first);sh_package_audio_close(second);
    put32(bytes+28,4);put32(bytes+32,0);resource.body_length=36;
    first=sh_package_audio_open(&compiled,&report,error,sizeof(error));CHECK(first && !report.invalid);
    CHECK(sh_package_audio_banks(first,&before)==1 && before && !before->metadata->event_count);
    sh_package_audio_close(first);
}

static void binary(const char *relative, const unsigned char *bytes, size_t size)
{
    char path[4096]; FILE *stream=NULL;
    create(relative,""); snprintf(path,sizeof(path),"%s/%s",root,relative);
    CHECK(!fopen_s(&stream,path,"wb") && stream);
    if(stream) { CHECK(fwrite(bytes,1,size,stream)==size); fclose(stream); }
}
static sh_package_compilation *compile(const char *folder, sh_package_sources **sources)
{
    char path[4096], error[1024]; sh_package_compilation *compiled;
    snprintf(path,sizeof(path),"%s/%s",root,folder);
    *sources=sh_package_sources_scan(path,error,sizeof(error)); CHECK(*sources);
    if(!*sources) return NULL;
    compiled=sh_package_compile(*sources,NULL,NULL,error,sizeof(error)); CHECK(compiled);
    if(!compiled) fprintf(stderr,"%s\n",error);
    return compiled;
}
static void authored_and_map(void)
{
    static const char *path="sound/soundbanks/pc/a.bnk";
    unsigned char a[BANK_BYTES], changed[BANK_BYTES];
    sh_package_sources *local_sources=NULL, *map_sources=NULL;
    sh_package_compilation *local=NULL, *map=NULL, *overlay=NULL;
    sh_package_audio *local_audio=NULL, *map_audio=NULL;
    sh_package_audio_report report;
    const sh_package_audio_reference *references;
    const sh_compiled_resource *resource;
    char error[1024];
    bank_bytes(a,84696446,7,0); bank_bytes(changed,84696446,13,0);
    create("local/overrides/one/package.json","{\"id\":\"one\",\"name\":\"One\"}");
    create("local/overrides/two/package.json","{\"id\":\"two\",\"name\":\"Two\"}");
    binary("local/overrides/one/assets/sound/soundbanks/pc/a.bnk",a,sizeof(a));
    binary("local/overrides/two/assets/sound/soundbanks/pc/a.bnk",a,sizeof(a));
    create("map/overrides/author/package.json","{\"id\":\"author\",\"name\":\"Author\"}");
    binary("map/overrides/author/assets/sound/soundbanks/pc/a.bnk",changed,sizeof(changed));
    local=compile("local",&local_sources); map=compile("map",&map_sources);
    if(!local || !map) goto done;
    resource=sh_package_compilation_find(local,path);
    CHECK(resource && resource->source_count==2 && sh_package_owners_count(&resource->gameplay_owners)==2);
    overlay=sh_package_compilation_overlay(local,map,error,sizeof(error)); CHECK(overlay);
    if(!overlay) goto done;
    local_audio=sh_package_audio_open(local,&report,error,sizeof(error)); CHECK(local_audio && report.banks==1);
    map_audio=sh_package_audio_open(overlay,&report,error,sizeof(error)); CHECK(map_audio && report.banks==1);
    if(!local_audio || !map_audio) goto done;
    CHECK(sh_package_audio_find(local_audio,7,&references)==1);
    CHECK(!sh_package_audio_find(local_audio,13,&references));
    CHECK(sh_package_audio_find(map_audio,13,&references)==1);
    CHECK(!sh_package_audio_find(map_audio,7,&references));
    CHECK(sh_package_source_verify(&local_sources->files[resource->source]));
    /* A changed captured source refuses a new index; existing metadata survives. */
    binary("map/overrides/author/assets/sound/soundbanks/pc/a.bnk",a,sizeof(a));
    CHECK(!sh_package_audio_open(overlay,&report,error,sizeof(error)) && error[0]);
    sh_package_compilation_free(overlay); overlay=NULL;
    sh_package_compilation_free(map); map=NULL;
    sh_package_sources_free(map_sources); map_sources=NULL;
    CHECK(sh_package_audio_find(map_audio,13,&references)==1 && !strcmp(references[0].path,path));
    /* An immutable cache can still index the captured bytes after source edits. */
    {
        sh_compiled_resource *r=local->resources;
        sh_package_source_file snapshot=local_sources->files[r->source];
        char absolute[4096];
        binary("snapshot.bnk",a,sizeof(a));
        snprintf(absolute,sizeof(absolute),"%s/snapshot.bnk",root); snapshot.absolute=absolute;
        r->cache_file=sh_package_file_seal(&snapshot,error,sizeof(error)); CHECK(r->cache_file);
        FILE *edited=NULL;
        CHECK(!fopen_s(&edited,local_sources->files[r->source].absolute,"wb") && edited);
        if(edited) { CHECK(fwrite(changed,1,sizeof(changed),edited)==sizeof(changed)); fclose(edited); }
        CHECK(!sh_package_source_verify(&local_sources->files[r->source]));
        sh_package_audio *cached=sh_package_audio_open(local,&report,error,sizeof(error)); CHECK(cached);
        CHECK(sh_package_audio_find(cached,7,&references)==1);
        sh_package_audio_close(cached);
    }
done:
    sh_package_audio_close(local_audio); sh_package_audio_close(map_audio);
    sh_package_compilation_free(local); sh_package_compilation_free(map); sh_package_compilation_free(overlay);
    sh_package_sources_free(local_sources); sh_package_sources_free(map_sources);
}

typedef struct closure_paths { char paths[8][256]; int originals[8]; size_t count; } closure_paths;
static int closure_path(void *context, const char *path, int original)
{
    closure_paths *out=context;
    if(out->count>=8)return 0;
    snprintf(out->paths[out->count],256,"%s",path);out->originals[out->count++]=original;return 1;
}
static int closure_has(const closure_paths *paths,const char *suffix,int original)
{
    for(size_t i=0;i<paths->count;i++)
        if(strstr(paths->paths[i],suffix) && paths->originals[i]==original)return 1;
    return 0;
}
static size_t dependency_bank(unsigned char bytes[128],uint32_t id,uint32_t event,uint32_t owner,uint32_t media)
{
    size_t at=24;memset(bytes,0,128);memcpy(bytes,"BKHD",4);put32(bytes+4,16);put32(bytes+8,113);put32(bytes+12,id);
    memcpy(bytes+at,"HIRC",4);at+=8;put32(bytes+at,owner ? 3 : 2);at+=4;
    bytes[at]=4;put32(bytes+at+1,owner ? 12 : 8);put32(bytes+at+5,event);
    if(owner){put32(bytes+at+9,1);put32(bytes+at+13,50);at+=17;}else at+=13;
    if(owner) {
        bytes[at]=3;put32(bytes+at+1,18);put32(bytes+at+5,50);bytes[at+9]=3;bytes[at+10]=4;
        put32(bytes+at+11,600);put32(bytes+at+19,owner);at+=23;
    }
    bytes[at]=2;put32(bytes+at+1,18);put32(bytes+at+5,owner ? 601 : 600);
    put32(bytes+at+9,0x20001);bytes[at+13]=media ? 1 : 0;put32(bytes+at+14,media);at+=23;
    put32(bytes+28,(uint32_t)(at-32));return at;
}
static void immutable_scope(void)
{
    unsigned char original[128],changed[49],dependent[128],media[4]={1,2,3,4};
    char base[4096],error[1024];closure_paths paths={0};
    size_t original_length=dependency_bank(original,84696446,17,0,0);
    size_t dependent_length=dependency_bank(dependent,84696445,23,84696446,99);
    binary("scope-base/sound/soundbanks/pc/english(us)/a.bnk",original,original_length);
    binary("scope-base/sound/soundbanks/pc/99.wem",media,sizeof(media));
    snprintf(base,sizeof(base),"%s/scope-base",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    bank_bytes(changed,84696446,13,0);
    sh_compiled_resource files[2]={{0},{0}};sh_package_compilation compiled={0};
    files[0].engine_path="sound/soundbanks/pc/a.bnk";files[0].body=changed;files[0].body_length=sizeof(changed);
    files[1].engine_path="sound/soundbanks/pc/b.bnk";files[1].body=dependent;files[1].body_length=dependent_length;
    compiled.resources=files;compiled.resource_count=2;
    sh_package_audio_scope *scope=sh_package_audio_scope_open(&compiled,originals,error,sizeof(error));CHECK(scope);
    if(scope) {
        CHECK(sh_package_audio_scope_event(scope,23,closure_path,&paths,error,sizeof(error)) && paths.count==3);
        CHECK(closure_has(&paths,"english(us)/a.bnk",1) && closure_has(&paths,"pc/b.bnk",0) && closure_has(&paths,"pc/99.wem",1));
        paths.count=0;CHECK(!sh_package_audio_scope_event(scope,13,closure_path,&paths,error,sizeof(error)) && !paths.count);
        paths.count=0;CHECK(sh_package_audio_scope_event(scope,17,closure_path,&paths,error,sizeof(error)) && paths.count==1);
        sh_package_audio_scope_close(scope);
    }
    /* A file in the same captured locale replaces that original within this
     * authored scope, without consulting another installed package. */
    files[0].engine_path="sound/soundbanks/pc/english(us)/a.bnk";
    scope=sh_package_audio_scope_open(&compiled,originals,error,sizeof(error));CHECK(scope);
    if(scope) {
        paths.count=0;CHECK(sh_package_audio_scope_event(scope,13,closure_path,&paths,error,sizeof(error)) && paths.count==1);
        CHECK(closure_has(&paths,"english(us)/a.bnk",0));
        paths.count=0;CHECK(!sh_package_audio_scope_event(scope,23,closure_path,&paths,error,sizeof(error)) &&
            !paths.count && strstr(error,"absent object 600"));
        paths.count=0;CHECK(!sh_package_audio_scope_event(scope,17,closure_path,&paths,error,sizeof(error)) && !paths.count);
        sh_package_audio_scope_close(scope);
    }
    sh_audio_originals_close(originals);
    snprintf(base,sizeof(base),"%s/empty-scope",root);originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    compiled.resources=files+1;compiled.resource_count=1;
    scope=sh_package_audio_scope_open(&compiled,originals,error,sizeof(error));CHECK(scope);
    if(scope) {
        paths.count=0;CHECK(!sh_package_audio_scope_event(scope,23,closure_path,&paths,error,sizeof(error)) && !paths.count && strstr(error,"absent bank"));
        sh_package_audio_scope_close(scope);
    }
    sh_audio_originals_close(originals);
}

static size_t typed_play_bank(unsigned char bytes[256], uint32_t event)
{
    memset(bytes, 0, 256);
    size_t length = dependency_bank(bytes, 84696446, event, 84696446, 99);
    size_t object = length - 23;
    put32(bytes + object + 1, 46); put32(bytes + object + 5, 600);
    put32(bytes + object + 9, 0x20001);
    length += 28; put32(bytes + 28, (uint32_t)(length - 32)); return length;
}
static void compact_event_scope(void)
{
    unsigned char native[256], compact[53], media[4]={1,2,3,4};
    size_t length=typed_play_bank(native,17);char base[4096],error[1024];
    binary("compact-scope/sound/soundbanks/pc/a.bnk",native,length);
    binary("compact-scope/sound/soundbanks/pc/99.wem",media,sizeof(media));
    snprintf(base,sizeof(base),"%s/compact-scope",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    bank_bytes(compact,123,99,0);put32(compact+28,21);put32(compact+37,12);
    put32(compact+45,1);put32(compact+49,50);
    sh_compiled_resource file={0};sh_package_compilation compiled={0};
    file.engine_path="sound/soundbanks/pc/123.bnk";file.body=compact;file.body_length=sizeof(compact);
    compiled.resources=&file;compiled.resource_count=1;
    sh_package_audio_scope *scope=sh_package_audio_scope_open(&compiled,originals,error,sizeof(error));CHECK(scope);
    if(scope) {
        closure_paths paths={0};
        CHECK(sh_package_audio_scope_event(scope,99,closure_path,&paths,error,sizeof(error)) && paths.count==3);
        CHECK(closure_has(&paths,"pc/123.bnk",0) && closure_has(&paths,"pc/a.bnk",1) && closure_has(&paths,"pc/99.wem",1));
        sh_package_audio_scope_close(scope);
    }
    sh_audio_originals_close(originals);
}

static void typed_capture(void)
{
    unsigned char bank[256], original_media[4] = {1, 2, 3, 4}, changed_media[4] = {4, 3, 2, 1};
    size_t bank_length = typed_play_bank(bank, 17); char base[4096], error[1024]; uint32_t events[] = {17, 17};
    /* A plug-in MEDIA field, absent from the lightweight source-descriptor
     * inventory, needs resident bytes from a different bank. */
    size_t effect = bank_length; bank[effect] = 18; put32(bank + effect + 1, 22);
    put32(bank + effect + 5, 555); put32(bank + effect + 9, 0x123456);
    bank[effect + 17] = 1; put32(bank + effect + 19, 88);
    bank_length += 27; put32(bank + 28, (uint32_t)(bank_length - 32)); put32(bank + 32, 4);
    unsigned char resident[56] = {0}; memcpy(resident, "BKHD", 4); put32(resident + 4, 16);
    put32(resident + 8, 113); put32(resident + 12, 84696445);
    memcpy(resident + 24, "DIDX", 4); put32(resident + 28, 12); put32(resident + 32, 88); put32(resident + 40, 4);
    memcpy(resident + 44, "DATA", 4); put32(resident + 48, 4); put32(resident + 52, 123);
    binary("capture-base/sound/soundbanks/pc/english(us)/a.bnk", bank, bank_length);
    binary("capture-base/sound/soundbanks/pc/b.bnk", resident, sizeof(resident));
    binary("capture-base/sound/soundbanks/pc/99.wem", original_media, sizeof(original_media));
    binary("capture-bank-cache.bnk", bank, bank_length);
    snprintf(base, sizeof(base), "%s/capture-base", root);
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); CHECK(originals);
    sh_compiled_resource files[3] = {{0}, {0}, {0}}; sh_package_compilation compiled = {0};
    files[0].engine_path = "sound/soundbanks/pc/99.wem"; files[0].body = changed_media; files[0].body_length = 4;
    files[1].engine_path = "sound/soundbanks/pc/english(us)/a.bnk"; files[1].body = bank; files[1].body_length = bank_length;
    files[2].engine_path = "sound/soundbanks/pc/french/99.wem"; files[2].body = changed_media; files[2].body_length = 4;
    compiled.resources = files; compiled.resource_count = 3;
    sh_package_audio_capture *capture = sh_package_audio_capture_open(&compiled, originals, events, 2, error, sizeof(error)); CHECK(capture);
    sh_audio_graph_root changed = {0};
    if (capture) {
        const sh_audio_graph_scope *input = sh_package_audio_capture_input(capture);
        CHECK(input && input->bank_count == 2 && input->banks[0].original && input->banks[1].original);
        sh_audio_link_graph *graph = sh_audio_link_graph_open(input, 1, error, sizeof(error));
        if (!graph) fprintf(stderr, "%s\n", error); CHECK(graph);
        if (graph) {
            CHECK(sh_audio_link_graph_event(graph, 0, 17, &changed, error, sizeof(error)) && !changed.original);
            sh_audio_link_graph_close(graph);
        }
        sh_package_audio_capture_close(capture);
    }
    /* The current locale ignores French data, and a file-backed exact original
     * bank uses the same typed capture path as a compiler-owned memory body. */
    files[0].body = original_media; files[1].body = NULL;
    snprintf(base, sizeof(base), "%s/capture-bank-cache.bnk", root);
    files[1].cache_file = sh_package_file_capture(base, error, sizeof(error)); CHECK(files[1].cache_file);
    capture = sh_package_audio_capture_open(&compiled, originals, events, 1, error, sizeof(error)); CHECK(capture);
    if (capture) {
        const sh_audio_graph_scope *input = sh_package_audio_capture_input(capture);
        CHECK(input && input->bank_count == 2 && input->banks[0].original && input->banks[1].original);
        sh_audio_link_graph *graph = sh_audio_link_graph_open(input, 1, error, sizeof(error)); CHECK(graph);
        if (graph) {
            sh_audio_graph_root restored;
            CHECK(sh_audio_link_graph_event(graph, 0, 17, &restored, error, sizeof(error)) && restored.original);
            CHECK(memcmp(restored.identity, changed.identity, 32) != 0);
            sh_audio_link_graph_close(graph);
        }
        sh_package_audio_capture_close(capture);
    }
    sh_package_file_release(files[1].cache_file);
    sh_audio_originals_close(originals);
}

typedef struct program_ids {
    struct { sh_audio_identity_domain domain; unsigned char key[32]; uint32_t id; } rows[64];
    size_t count;
    int remaining;
} program_ids;
static int program_assign(void *context, const sh_audio_link_graph *graph, size_t node, sh_audio_identity_domain domain, uint32_t authored,
    const unsigned char binding[32], int original, int absent, uint32_t *id, char *error, size_t capacity)
{
    program_ids *ids = context; (void)absent; CHECK(graph); (void)node;
    if (!ids->remaining--) { snprintf(error, capacity, "reservation rejected by fixture"); return 0; }
    if (original) { *id = authored; return 1; }
    for (size_t i = 0; i < ids->count; i++) if (ids->rows[i].domain == domain && !memcmp(ids->rows[i].key, binding, 32)) {
        *id = ids->rows[i].id; return 1;
    }
    if (ids->count == 64) return 0;
    ids->rows[ids->count].domain = domain; memcpy(ids->rows[ids->count].key, binding, 32);
    *id = ids->rows[ids->count].id = 100000 + (uint32_t)ids->count; ids->count++; return 1;
}
static int program_write(void *context, const void *bytes, size_t length)
{
    size_t *count = context; CHECK(bytes != NULL); *count += length; return 1;
}
static int program_media(void *context, uint32_t authored, uint32_t bound, const sh_audio_graph_media *media)
{
    size_t *count = context; unsigned char bytes[4];
    CHECK(authored == 99 && bound && bound != authored && media->source.length == 4 && !media->original);
    CHECK(media->source.read(media->source.context, 0, bytes, sizeof(bytes)));
    CHECK(bytes[0] == 8 && bytes[1] == 8 && bytes[2] == 8 && bytes[3] == 8);
    (*count)++; return 1;
}
static void materialized_program(sh_package_audio_program *program, const sh_package_compilation *compiled)
{
    char directory[4096], error[1024];
    snprintf(directory, sizeof(directory), "%s/package-cache", root);
    CHECK(CreateDirectoryA(directory, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(directory, sizeof(directory), "%s/package-cache/resources", root);
    CHECK(CreateDirectoryA(directory, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    sh_package_sources *sources = calloc(1, sizeof(*sources)); CHECK(sources); if (!sources) return;
    sources->packages = calloc(2, sizeof(*sources->packages)); sources->package_count = 2;
    CHECK(sources->packages); if (!sources->packages) { free(sources); return; }
    const sh_compiled_resource *inputs[2] = {compiled->resources, compiled->resources + 1};
    sh_package_resource_projection rows[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        rows[i].engine_path = inputs[i]->engine_path; rows[i].inputs = inputs + i; rows[i].input_count = 1;
    }
    sh_package_compilation *provider = sh_package_compilation_project(sources, rows, 2, NULL, NULL, error, sizeof(error));
    CHECK(provider); if (!provider) { sh_package_sources_free(sources); return; }
    provider->owns_sources = 1; provider->map_overlay = 1; provider->map_owner_begin = 1;
    provider->independent_sources = provider->map_independent_sources = 1;
    sh_package_compilation *before = provider;
    CHECK(!sh_package_audio_program_materialize(program, &provider, "missing-directory", error, sizeof(error)));
    CHECK(provider == before && provider->owns_sources && provider->resource_count == 2);
    /* A stale bound declaration fails after file generation without changing
     * the provider. Complete cache objects can be reused by the retry. */
    uint32_t event = provider->resources[1].sound_event;
    provider->resources[1].sound_event++;
    CHECK(!sh_package_audio_program_materialize(program, &provider, directory, error, sizeof(error)));
    CHECK(provider == before && provider->owns_sources && provider->resource_count == 2);
    provider->resources[1].sound_event = event;
    int ok = sh_package_audio_program_materialize(program, &provider, directory, error, sizeof(error));
    if (!ok) fprintf(stderr, "materialize: %s\n", error); CHECK(ok);
    if (!ok) { sh_package_compilation_free(provider); return; }
    CHECK(provider != before && provider->resource_count == 4 && provider->owns_sources);
    CHECK(provider->map_overlay && provider->map_owner_begin == 1 && provider->independent_sources && provider->map_independent_sources);
    sh_package_changes generated = {0};
    CHECK(sh_package_compilation_changes(NULL, provider, &generated, error, sizeof(error)) &&
        generated.count == provider->resource_count);
    CHECK(sh_package_compilation_changes(provider, provider, &generated, error, sizeof(error)) && !generated.count);
    sh_package_changes_free(&generated);
    const sh_compiled_resource *native = sh_package_compilation_find(provider, "generated/decls/sound/bound/first.decl");
    const sh_compiled_resource *variant = sh_package_compilation_find(provider, "generated/decls/sound/bound/second.decl");
    CHECK(native && !native->bound_dependency_count);
    CHECK(variant && variant->sound_event == event && variant->bound_dependency_count == 2);
    FILE *retained = NULL; uint64_t retained_length = 0; char *cached = NULL;
    size_t banks = 0, media = 0;
    for (size_t i = 0; i < provider->resource_count; i++) {
        const sh_compiled_resource *r = provider->resources + i;
        int kind = sh_package_audio_path_kind(r->engine_path);
        if (!kind) continue;
        CHECK(r->cache_file && r->cache_path && r->original_path && !r->body && r->source == SIZE_MAX);
        CHECK(sh_package_compilation_probe(provider, r->engine_path, error, sizeof(error)) == 1);
        CHECK(strcmp(r->engine_path, r->original_path));
        uint64_t size; FILE *stream = sh_package_file_open(r->cache_file, &size, error, sizeof(error)); CHECK(stream);
        if (strstr(r->engine_path, ".bnk")) {
            sh_audio_bank bank = {0}; banks++;
            CHECK(stream && sh_audio_bank_read(stream, &bank, error, sizeof(error)) == 1);
            CHECK(bank.event_count == 1 && bank.events[0] == event && r->bound_dependency_count == 1);
            uint32_t named = 0;
            CHECK(sh_audio_bank_numeric_identity(strrchr(r->engine_path, '/') + 1, &named) && named == bank.id);
            CHECK(!sh_package_owners_count(&r->owners));
            sh_audio_bank_free(&bank);
        } else {
            unsigned char bytes[4]; media++;
            CHECK(stream && size == 4 && fread(bytes, 1, 4, stream) == 4 && !memcmp(bytes, "\10\10\10\10", 4));
            CHECK(r->owners.bits == 2 && r->gameplay_owners.bits == 2);
            retained = sh_package_file_open(r->cache_file, &retained_length, error, sizeof(error));
            cached = _strdup(r->cache_path);
        }
        if (stream) fclose(stream);
    }
    CHECK(banks == 1 && media == 1);
    CHECK(sh_package_audio_program_materialize(program, &provider, directory, error, sizeof(error)));
    CHECK(provider->resource_count == 4); /* Repeated preparation adds no files/edges. */
    variant = sh_package_compilation_find(provider, "generated/decls/sound/bound/second.decl");
    CHECK(variant && variant->bound_dependency_count == 2);
    sh_package_compilation_free(provider);
    CHECK(retained && cached && retained_length == 4 && fgetc(retained) == 8);
    if (cached) CHECK(!DeleteFileA(cached) && GetLastError() == ERROR_SHARING_VIOLATION);
    if (retained) fclose(retained); free(cached);
}
static void retained_program(void)
{
    unsigned char bank[256], original_media[] = {1, 2, 3, 4}; uint32_t event;
    const char *sound = "events/example"; char base[4096], author_path[4096], error[1024] = {0};
    CHECK(sh_audio_event_identity(sound, &event));
    size_t length = typed_play_bank(bank, event);
    binary("program-base/sound/soundbanks/pc/a.bnk", bank, length);
    binary("program-base/sound/soundbanks/pc/99.wem", original_media, sizeof(original_media));
    snprintf(base, sizeof(base), "%s/program-base", root);
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); CHECK(originals);
    sh_compiled_resource files[2] = {{0}, {0}};
    sh_package_compilation scopes[3] = {{0}, {0}, {0}};
    const sh_package_compilation *inputs[] = {scopes, scopes + 1, scopes + 2};
    files[0].engine_path = "sound/soundbanks/pc/99.wem"; files[0].body_length = 4;
    files[0].baseline_known = 1; files[0].owners.bits = files[0].gameplay_owners.bits = 1;
    files[0].body = malloc(4); CHECK(files[0].body); memcpy(files[0].body, original_media, 4);
    files[1] = files[0]; files[1].body = malloc(4); CHECK(files[1].body); memset(files[1].body, 8, 4);
    files[1].owners.bits = files[1].gameplay_owners.bits = 2;
    binary("program-authored.wem", files[1].body, 4);
    snprintf(author_path, sizeof(author_path), "%s/program-authored.wem", root);
    sh_package_file_identity authored_identity;
    CHECK(sh_package_bytes_identity(files[1].body, 4, &authored_identity));
    sh_package_source_file authored_file = {0}; authored_file.absolute = author_path; authored_file.length = 4;
    memcpy(authored_file.digest, authored_identity.digest, 32);
    sh_package_sources authored_sources = {0}; authored_sources.files = &authored_file; authored_sources.file_count = 1;
    free(files[1].body); files[1].body = NULL; files[1].body_length = 0; files[1].source = 0;
    scopes[1].resources = files; scopes[1].resource_count = 1;
    scopes[2].resources = files + 1; scopes[2].resource_count = 1;
    scopes[2].sources = &authored_sources;
    sh_package_audio_program *p = sh_package_audio_program_open(inputs, 3, originals, error, sizeof(error)); CHECK(p);
    if (!p) goto done;
    for (size_t i = 0; i < 3; i++) {
        CHECK(sh_package_audio_program_collect(p, i, sound, error, sizeof(error)));
        CHECK(sh_package_audio_program_collect(p, i, sound, error, sizeof(error)));
    }
    CHECK(!sh_package_audio_program_collect(p, 3, sound, error, sizeof(error)));
    error[0] = 0;
    int ready = sh_package_audio_program_prepare(p, error, sizeof(error));
    if (!ready) fprintf(stderr, "program prepare: %s\n", error); CHECK(ready);
    if (!ready) goto done;
    /* Prepared outputs cannot borrow the transient compiler bodies or arrays. */
    free(files[0].body); free(files[1].body); memset(files, 0, sizeof(files)); memset(scopes, 0, sizeof(scopes));
    sh_audio_graph_root a, b, c;
    CHECK(sh_package_audio_program_root(p, 0, sound, &a, error, sizeof(error)) && a.original);
    CHECK(sh_package_audio_program_root(p, 1, sound, &b, error, sizeof(error)) && b.original);
    CHECK(sh_package_audio_program_root(p, 2, sound, &c, error, sizeof(error)) && !c.original);
    CHECK(!memcmp(a.identity, b.identity, 32) && memcmp(a.identity, c.identity, 32));
    const sh_package_owners *owners = NULL, *gameplay = NULL;
    CHECK(sh_package_audio_program_owners(p, 0, sound, &owners, &gameplay) && !sh_package_owners_count(owners));
    CHECK(sh_package_audio_program_owners(p, 1, sound, &owners, &gameplay) && !sh_package_owners_count(owners));
    CHECK(sh_package_audio_program_owners(p, 2, sound, &owners, &gameplay) && owners->bits == 2 && gameplay->bits == 2);
    CHECK(!sh_package_audio_program_owners(p, 3, sound, &owners, &gameplay) && !owners && !gameplay);
    CHECK(!sh_package_audio_program_collect(p, 0, "events/late", error, sizeof(error)));
    CHECK(!sh_package_audio_program_root(p, 0, "events/late", &a, error, sizeof(error)));
    CHECK(sh_package_audio_program_output(p, 0, sound, "bound/first", error, sizeof(error)));
    CHECK(sh_package_audio_program_output(p, 1, sound, "bound/first", error, sizeof(error)));
    CHECK(!sh_package_audio_program_output(p, 2, sound, "bound/first", error, sizeof(error)));
    CHECK(sh_package_audio_program_output(p, 2, sound, "bound/second", error, sizeof(error)));
    size_t media_count = 0;
    CHECK(sh_package_audio_program_scopes(p) == 3);
    CHECK(!sh_package_audio_program_media(p, 2, program_media, &media_count, error, sizeof(error)) && !media_count);
    sh_compiled_resource emitted[3] = {{0}, {0}, {0}}; sh_package_compilation compiled = {0};
    for (size_t i = 0; i < 2; i++) {
        emitted[i].engine_path = i ? "generated/decls/sound/bound/second.decl" : "generated/decls/sound/bound/first.decl";
        emitted[i].original_path = "generated/decls/sound/events/example.decl";
        emitted[i].type = "sound"; emitted[i].name = i ? "bound/second" : "bound/first";
        emitted[i].source = SIZE_MAX; emitted[i].body = (unsigned char *)"{}"; emitted[i].body_length = 2;
    }
    /* A SnapMap sound the program did not collect keeps its native event. */
    emitted[2] = emitted[0]; emitted[2].original_path = NULL;
    emitted[2].engine_path = "generated/decls/sound/stock/native.decl"; emitted[2].name = "stock/native";
    compiled.resources = emitted; compiled.resource_count = 3;
    CHECK(sh_package_sound_prepare(&compiled, error, sizeof(error)));
    uint32_t stock = 0; CHECK(sh_audio_event_identity("stock/native", &stock) && emitted[2].sound_event == stock);
    program_ids ids = {0}; ids.remaining = 1000;
    CHECK(sh_package_audio_program_bind(p, &compiled, program_assign, &ids, error, sizeof(error)));
    CHECK(emitted[0].sound_event == event && emitted[1].sound_event != event && emitted[1].sound_event);
    CHECK(emitted[2].sound_event == stock);
    /* Renamed without a captured output, it has no event to play. */
    emitted[2].original_path = "generated/decls/sound/stock/other.decl";
    emitted[2].sound_event = emitted[2].sound_authored_event = 0;
    CHECK(sh_package_sound_prepare(&compiled, error, sizeof(error)));
    CHECK(!sh_package_audio_program_bind(p, &compiled, program_assign, &ids, error, sizeof(error)) &&
        strstr(error, "no captured event output"));
    compiled.resource_count = 2;
    CHECK(sh_package_audio_program_bind(p, &compiled, program_assign, &ids, error, sizeof(error)));
    CHECK(sh_package_audio_program_media(p, 2, program_media, &media_count, error, sizeof(error)) && media_count == 1);
    uint32_t retained = emitted[1].sound_event;
    const sh_audio_bank_binding *binding = NULL;
    const sh_audio_graph_bank *captured = sh_package_audio_program_bank(p, 2, 0, &binding);
    CHECK(sh_package_audio_program_banks(p, 2) == 1 && captured && binding && binding->count);
    if (binding && captured) {
        size_t written = 0, edits = binding->count;
        CHECK(sh_audio_bank_binding_write(captured->source, binding, program_write, &written, error, sizeof(error)));
        CHECK(written == binding->output_length);
        ids.remaining = 2;
        CHECK(!sh_package_audio_program_bind(p, &compiled, program_assign, &ids, error, sizeof(error)));
        CHECK(emitted[1].sound_event == retained);
        CHECK(sh_package_audio_program_bank(p, 2, 0, &binding) == captured && binding->count == edits);
        ids.remaining = 1000;
        CHECK(sh_package_audio_program_bind(p, &compiled, program_assign, &ids, error, sizeof(error)));
        CHECK(emitted[1].sound_event == retained);
    }
    materialized_program(p, &compiled);
    /* Prepared providers keep the old bytes without holding author files open. */
    FILE *edited = fopen(author_path, "wb"); CHECK(edited);
    if (edited) { CHECK(fwrite("new!", 1, 4, edited) == 4); fclose(edited); }
    media_count = 0;
    CHECK(sh_package_audio_program_media(p, 2, program_media, &media_count, error, sizeof(error)) && media_count == 1);
done:
    sh_package_audio_program_close(p); sh_audio_originals_close(originals);
    free(files[0].body); free(files[1].body);
    /* No audio requests must remain usable before the native audio subsystem
     * has initialized. Unrelated packages cannot acquire an audio requirement. */
    sh_package_compilation empty = {0}; const sh_package_compilation *one = &empty;
    p = sh_package_audio_program_open(&one, 1, NULL, error, sizeof(error)); CHECK(p);
    CHECK(sh_package_audio_program_prepare(p, error, sizeof(error)));
    CHECK(!sh_package_audio_program_banks(p, 0)); sh_package_audio_program_close(p);
}

static void inventory_capture(void)
{
    unsigned char a[256], b[256] = {0}, original_media[] = {1, 2, 3, 4}, changed_media[] = {8, 8, 8, 8};
    char base[4096], error[1024] = {0}; uint32_t event;
    CHECK(sh_audio_event_identity("events/example", &event));
    size_t total = typed_play_bank(a, event);
    /* Separate the event/action from its node. A media-only replacement must
     * nominate the upstream event bank through typed cross-bank references. */
    memcpy(b, a, 36); put32(b + 12, 84696445); put32(b + 32, 1);
    memcpy(b + 36, a + 76, total - 76); size_t b_length = total - 40; put32(b + 28, (uint32_t)(b_length - 32));
    put32(a + 28, 44); put32(a + 32, 2); put32(a + 72, 84696445);
    binary("inventory-base/sound/soundbanks/pc/a.bnk", a, 76);
    binary("inventory-base/sound/soundbanks/pc/b.bnk", b, b_length);
    binary("inventory-base/sound/soundbanks/pc/99.wem", original_media, sizeof(original_media));
    snprintf(base, sizeof(base), "%s/inventory-base", root);
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); CHECK(originals);
    sh_compiled_resource file = {0}; sh_package_compilation compiled = {0};
    file.engine_path = "sound/soundbanks/pc/99.wem"; file.body = changed_media; file.body_length = sizeof(changed_media);
    file.owners.bits = file.gameplay_owners.bits = 1; file.baseline_known = 1;
    compiled.resources = &file; compiled.resource_count = 1;
    sh_package_audio_capture *capture = sh_package_audio_capture_open_inventory(&compiled, originals, error, sizeof(error));
    if (!capture) fprintf(stderr, "inventory capture: %s\n", error);
    CHECK(capture);
    if (capture) {
        const sh_audio_graph_scope *input = sh_package_audio_capture_input(capture);
        CHECK(input->bank_count == 2);
        sh_audio_link_graph *graph = sh_audio_link_graph_open(input, 1, error, sizeof(error)); CHECK(graph);
        sh_audio_graph_root binding;
        CHECK(sh_audio_link_graph_event(graph, 0, event, &binding, error, sizeof(error)) && !binding.original);
        sh_audio_link_graph_close(graph); sh_package_audio_capture_close(capture);
    }
    /* Inactive locale files stay in the bundle without nominating a current
     * language variant. Identical current-locale bytes still certify stock. */
    file.engine_path = "sound/soundbanks/pc/french/99.wem";
    capture = sh_package_audio_capture_open_inventory(&compiled, originals, error, sizeof(error)); CHECK(capture);
    CHECK(capture && !sh_package_audio_capture_input(capture)->bank_count); sh_package_audio_capture_close(capture);
    file.engine_path = "sound/soundbanks/pc/99.wem"; file.body = original_media;
    const sh_package_compilation *one = &compiled;
    sh_package_audio_program *program = sh_package_audio_program_open(&one, 1, originals, error, sizeof(error)); CHECK(program);
    CHECK(sh_package_audio_program_collect(program, 0, "events/example", error, sizeof(error)));
    CHECK(sh_package_audio_program_prepare(program, error, sizeof(error)));
    sh_audio_graph_root binding;
    CHECK(sh_package_audio_program_affected(program, 0, "events/example"));
    CHECK(sh_package_audio_program_root(program, 0, "events/example", &binding, error, sizeof(error)) && binding.original);
    sh_package_audio_program_close(program);
    /* A valid bank with no named declaration remains a prepared native input;
     * it does not get misparsed as a generic resource-manager file. */
    file.engine_path = "sound/soundbanks/pc/b.bnk"; file.body = b; file.body_length = b_length;
    program = sh_package_audio_program_open(&one, 1, originals, error, sizeof(error)); CHECK(program);
    CHECK(sh_package_audio_program_prepare(program, error, sizeof(error)));
    CHECK(sh_package_audio_program_banks(program, 0) == 2); sh_package_audio_program_close(program);
    file.body_length = 3;
    capture = sh_package_audio_capture_open_inventory(&compiled, originals, error, sizeof(error));
    CHECK(!capture && *error); sh_package_audio_capture_close(capture);
    sh_audio_originals_close(originals);
}

static void retry_program(void)
{
    unsigned char bank[BANK_BYTES]; char base[4096], error[1024] = {0}; uint32_t event;
    CHECK(sh_audio_event_identity("events/example", &event));
    bank_bytes(bank, 84696446, event + 1, 0);
    snprintf(base, sizeof(base), "%s/program-base", root);
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); CHECK(originals);
    sh_compiled_resource bad = {0};
    bad.engine_path = "sound/soundbanks/pc/a.bnk"; bad.body = bank; bad.body_length = sizeof(bank);
    sh_package_compilation scopes[2] = {{0}, {0}}; const sh_package_compilation *inputs[] = {scopes, scopes + 1};
    scopes[1].resources = &bad; scopes[1].resource_count = 1;
    sh_package_audio_program *p = sh_package_audio_program_open(inputs, 2, originals, error, sizeof(error)); CHECK(p);
    CHECK(sh_package_audio_program_collect(p, 0, "events/example", error, sizeof(error)));
    CHECK(sh_package_audio_program_collect(p, 1, "events/example", error, sizeof(error)));
    CHECK(!sh_package_audio_program_prepare(p, error, sizeof(error)) && *error);
    CHECK(!sh_package_audio_program_scopes(p) && !sh_package_audio_program_banks(p, 0));
    /* Scope zero was captured before the second scope failed. Retry must drop
     * all of that attempt's captures and read the corrected authored source. */
    bank_bytes(bank, 84696446, event, 0); error[0] = 0;
    CHECK(sh_package_audio_program_prepare(p, error, sizeof(error)));
    sh_audio_graph_root root_binding;
    CHECK(sh_package_audio_program_root(p, 1, "events/example", &root_binding, error, sizeof(error)));
    CHECK(!root_binding.original && sh_package_audio_program_banks(p, 1) == 1);
    sh_package_audio_program_close(p); sh_audio_originals_close(originals);
}

static size_t shared_bank(unsigned char bank[256],uint32_t event,uint32_t duration,int extra)
{
    memset(bank,0,256);bank_bytes(bank,84696446,event,0);
    unsigned char *chunk=bank+BANK_BYTES,*body=chunk+8;float threshold=-60;
    memcpy(chunk,"STMG",4);put32(chunk+4,30+(extra ? 12 : 0));memcpy(body,&threshold,4);body[4]=128;
    put32(body+6,extra ? 2 : 1);put32(body+10,42);put32(body+14,duration);
    if(extra){put32(body+22,43);put32(body+26,90);}
    size_t length=BANK_BYTES+8+30+(extra ? 12 : 0);
    memcpy(bank+length,"PLAT",4);put32(bank+length+4,11);put32(bank+length+8,7);
    memcpy(bank+length+12,"Windows",7);return length+19;
}
static int shared_file_read(void *context,uint64_t offset,void *body,size_t length)
{
    FILE *file=context;
    return !_fseeki64(file,(__int64)offset,SEEK_SET) && fread(body,1,length,file)==length;
}
static void shared_materialization(sh_package_audio_program *program)
{
    char directory[4096],error[1024]={0};
    snprintf(directory,sizeof(directory),"%s/package-cache/resources",root);
    sh_package_sources *sources=calloc(1,sizeof(*sources));CHECK(sources);if(!sources)return;
    sources->packages=calloc(2,sizeof(*sources->packages));sources->package_count=2;
    CHECK(sources->packages);if(!sources->packages){free(sources);return;}
    sh_package_compilation *provider=sh_package_compilation_project(sources,NULL,0,NULL,NULL,error,sizeof(error));
    CHECK(provider);if(!provider){sh_package_sources_free(sources);return;}provider->owns_sources=1;
    program_ids ids={0};ids.remaining=1000;
    int ready=sh_package_audio_program_bind(program,provider,program_assign,&ids,error,sizeof(error));
    if(!ready)fprintf(stderr,"shared bind: %s\n",error);CHECK(ready);
    if(ready) {
        ready=sh_package_audio_program_materialize(program,&provider,directory,error,sizeof(error));
        if(!ready)fprintf(stderr,"shared materialize: %s\n",error);CHECK(ready);
    }
    if(ready) {
        CHECK(provider->resource_count==2);
        for(size_t i=0;i<provider->resource_count;i++) {
            const sh_compiled_resource *resource=provider->resources+i;uint64_t length=0;
            CHECK(sh_package_audio_path_kind(resource->engine_path)==1 && resource->cache_file);
            FILE *file=sh_package_file_open(resource->cache_file,&length,error,sizeof(error));CHECK(file);
            if(file) {
                sh_audio_bank_codec parsed={0};
                CHECK(sh_audio_bank_codec_read((sh_audio_bank_source){file,length,shared_file_read},&parsed,error,sizeof(error))==1);
                /* The event is already supplied by the retained native bank. */
                CHECK(!parsed.shared_chunks && parsed.object_count==0 && length==36);
                sh_audio_bank_codec_free(&parsed);fclose(file);
            }
        }
        for(size_t i=0;i<2;i++) {
            const sh_audio_bank_binding *binding=NULL;
            const sh_audio_graph_bank *bank=sh_package_audio_program_bank(program,i,0,&binding);
            CHECK(bank && binding && bank->codec->shared_chunks && binding->output_length>BANK_BYTES);
            size_t written=0;
            CHECK(bank && binding && sh_audio_bank_binding_write(bank->source,binding,program_write,&written,error,sizeof(error)));
            CHECK(binding && written==binding->output_length);
        }
        CHECK(sh_package_audio_program_shared(program)->count>0);
    }
    sh_package_compilation_free(provider);
}
static void shared_program(void)
{
    unsigned char original[256],a[256],b[256];uint32_t event;char base[4096],error[1024]={0};
    CHECK(sh_audio_event_identity("events/shared",&event));
    size_t original_length=shared_bank(original,event,100,0),a_length=shared_bank(a,event,180,0),b_length=shared_bank(b,event,100,1);
    binary("shared-base/sound/soundbanks/pc/a.bnk",original,original_length);
    snprintf(base,sizeof(base),"%s/shared-base",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    sh_compiled_resource files[2]={{0},{0}};sh_package_compilation scopes[2]={{0},{0}};
    const sh_package_compilation *inputs[]={scopes,scopes+1};
    for(size_t i=0;i<2;i++) {
        files[i].engine_path="sound/soundbanks/pc/a.bnk";files[i].body=i ? b : a;files[i].body_length=i ? b_length : a_length;
        files[i].owners.bits=files[i].gameplay_owners.bits=(uint64_t)1<<i;
        scopes[i].resources=files+i;scopes[i].resource_count=1;
    }
    sh_package_audio_program *p=sh_package_audio_program_open(inputs,2,originals,error,sizeof(error));CHECK(p);
    CHECK(!sh_package_audio_program_shared(p));
    /* A malformed platform is not hidden by a different bank being valid.
     * A failed attempt retains no partially prepared controls and can retry. */
    b[b_length-1]='x';CHECK(!sh_package_audio_program_prepare(p,error,sizeof(error)) && strstr(error,"platform"));
    CHECK(!sh_package_audio_program_shared(p));b[b_length-1]='s';error[0]=0;
    int ready=sh_package_audio_program_prepare(p,error,sizeof(error));
    if(!ready)fprintf(stderr,"shared program: %s\n",error);CHECK(ready);
    if(ready) {
        const sh_audio_shared_policy *policy=sh_package_audio_program_shared(p);CHECK(policy);
        /* Neither input is the original. The program must capture the real
         * baseline to distinguish the copied 100 from the authored 180. */
        const sh_audio_shared_setting *changed=NULL,*addition=NULL;
        for(size_t i=0;i<policy->count;i++)if(policy->settings[i].record.kind==SH_AUDIO_SHARED_GROUP) {
            if(policy->settings[i].record.key[0]==42)changed=policy->settings+i;
            if(policy->settings[i].record.key[0]==43)addition=policy->settings+i;
        }
        CHECK(changed && changed->body[4]==180 && addition && addition->body[4]==90);
        size_t scope=SIZE_MAX,bank=SIZE_MAX;int baseline=-1;
        CHECK(changed && sh_package_audio_program_shared_origin(p,changed->input,&scope,&bank,&baseline));
        CHECK(scope==0 && bank==0 && !baseline);
        /* Prepared settings and source attribution survive author input edits. */
        memset(a,0,sizeof(a));memset(b,0,sizeof(b));memset(files,0,sizeof(files));memset(scopes,0,sizeof(scopes));
        unsigned char *body=NULL;size_t length=0;
        CHECK(sh_audio_shared_policy_chunk(policy,"STMG",&body,&length,error,sizeof(error)));
        CHECK(length==42 && body && body[14]==180 && body[26]==90);free(body);
        CHECK(sh_package_audio_program_prepare(p,error,sizeof(error)));
        CHECK(sh_package_audio_program_shared(p)==policy);
        shared_materialization(p);
    }
    sh_package_audio_program_close(p);sh_audio_originals_close(originals);
}

static void shared_native_baseline(void)
{
    unsigned char initial[256],authored[256];uint32_t init_id;char base[4096],error[1024]={0};
    CHECK(sh_audio_bank_identity("init.bnk",&init_id));
    size_t initial_length=shared_bank(initial,100,100,0),authored_length=shared_bank(authored,101,100,1);
    put32(initial+12,init_id);initial[BANK_BYTES+8+4]=200;
    binary("shared-new-base/sound/soundbanks/pc/init.bnk",initial,initial_length);
    snprintf(base,sizeof(base),"%s/shared-new-base",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    sh_compiled_resource file={0};file.engine_path="sound/soundbanks/pc/a.bnk";
    file.body=authored;file.body_length=authored_length;
    sh_package_compilation compiled={0};compiled.resources=&file;compiled.resource_count=1;
    const sh_package_compilation *scope=&compiled;
    sh_package_audio_program *p=sh_package_audio_program_open(&scope,1,originals,error,sizeof(error));CHECK(p);
    authored[authored_length-1]='x';CHECK(!sh_package_audio_program_prepare(p,error,sizeof(error)) && strstr(error,"platform"));
    authored[authored_length-1]='s';error[0]=0;
    int ready=sh_package_audio_program_prepare(p,error,sizeof(error));
    if(!ready)fprintf(stderr,"native shared baseline: %s\n",error);CHECK(ready);
    if(ready) {
        const sh_audio_shared_policy *policy=sh_package_audio_program_shared(p);
        for(size_t i=0;i<policy->count;i++)if(policy->settings[i].record.kind==SH_AUDIO_SHARED_VOICES) {
            CHECK(policy->settings[i].body[0]==200);
            size_t source=0,bank=0;int baseline=0;
            CHECK(sh_package_audio_program_shared_origin(p,policy->settings[i].input,&source,&bank,&baseline));
            CHECK(source==SIZE_MAX && bank==SIZE_MAX && baseline);
        }
    }
    sh_package_audio_program_close(p);sh_audio_originals_close(originals);
}

static size_t bus_bank(unsigned char bank[256],uint32_t event,unsigned property)
{
    memset(bank,0,256);bank_bytes(bank,84696446,event,0);
    size_t at=BANK_BYTES;bank[at++]=8;size_t length_at=at;at+=4;size_t body=at;
    put32(bank+at,71);at+=8; /* ID and root parent. */
    bank[at++]=1;bank[at++]=7;put32(bank+at,property);at+=4;
    at+=9+12; /* Fixed settings and empty ducking list. */
    bank[at++]=1;bank[at++]=0;bank[at++]=2;put32(bank+at,74);at+=4;bank[at++]=1;at++;
    at+=5+1+2+4; /* Mixer, flags, empty RTPC/state lists. */
    put32(bank+length_at,(uint32_t)(at-body));put32(bank+28,(uint32_t)(at-32));put32(bank+32,2);
    return at;
}
static size_t effect_bank(unsigned char bank[256],unsigned tone)
{
    memset(bank,0,256);bank_bytes(bank,84696445,777,0);
    size_t at=BANK_BYTES;bank[at++]=18;put32(bank+at,18);at+=4;
    put32(bank+at,74);at+=4;put32(bank+at,1234);at+=4;put32(bank+at,1);at+=4;
    bank[at++]=(unsigned char)tone;at+=5;
    put32(bank+28,(uint32_t)(at-32));put32(bank+32,2);return at;
}
static void bus_program(void)
{
    unsigned char banks[4][256];uint32_t event;char base[4096],error[1024]={0};
    CHECK(sh_audio_event_identity("events/bus",&event));
    size_t sizes[]={bus_bank(banks[0],event,10),effect_bank(banks[1],11),
        bus_bank(banks[2],event,10),effect_bank(banks[3],22)};
    size_t packed_at=BANK_BYTES+5+8+6;
    banks[0][packed_at]=0x81;banks[2][packed_at]=0x41;
    snprintf(base,sizeof(base),"%s/bus-empty-base",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    sh_compiled_resource files[4]={{0}};sh_package_compilation scopes[2]={{0}};
    const sh_package_compilation *inputs[]={scopes,scopes+1};
    for(size_t i=0;i<4;i++) {
        files[i].engine_path=i%2 ? "sound/soundbanks/pc/b.bnk" : "sound/soundbanks/pc/a.bnk";
        files[i].body=banks[i];files[i].body_length=sizes[i];
        files[i].owners.bits=files[i].gameplay_owners.bits=(uint64_t)1<<i;
    }
    scopes[0].resources=files;scopes[0].resource_count=2;scopes[1].resources=files+2;scopes[1].resource_count=2;
    sh_package_audio_program *p=sh_package_audio_program_open(inputs,2,originals,error,sizeof(error));CHECK(p);
    CHECK(!sh_package_audio_program_bus_values(p) && !sh_package_audio_program_bus_at(p,0));
    CHECK(!sh_package_audio_program_bus_policy(p) && !sh_package_audio_program_bound_bus_policy(p));
    /* A shared-policy failure occurs after bus preparation. Retrying must not
     * retain the first attempt's values, owners, or captured file pointers. */
    size_t tail=sizes[2];memcpy(banks[2]+tail,"PLAT",4);put32(banks[2]+tail+4,5);put32(banks[2]+tail+8,1);
    banks[2][tail+12]='x';files[2].body_length+=13;
    size_t other=sizes[0];memcpy(banks[0]+other,"PLAT",4);put32(banks[0]+other+4,5);put32(banks[0]+other+8,1);
    banks[0][other+12]='y';files[0].body_length+=13;
    CHECK(!sh_package_audio_program_prepare(p,error,sizeof(error)) && strstr(error,"platform"));
    CHECK(!sh_package_audio_program_bus_values(p));files[0].body_length=sizes[0];files[2].body_length=sizes[2];
    CHECK(!sh_package_audio_program_bus_policy(p));
    int ready=sh_package_audio_program_prepare(p,error,sizeof(error));
    if(!ready)fprintf(stderr,"bus program: %s\n",error);CHECK(ready);
    if(ready) {
        size_t fx[2]={SIZE_MAX,SIZE_MAX},prop[2]={SIZE_MAX,SIZE_MAX};
        for(size_t i=0;i<sh_package_audio_program_bus_values(p);i++) {
            const sh_package_audio_bus_value *value=sh_package_audio_program_bus_at(p,i);
            CHECK(value && value->scope<2 && value->bus==71 && value->kind==8);
            if(value->field.field==SH_AUDIO_BUS_FIELD_EFFECT)fx[value->scope]=i;
            if(value->field.field==SH_AUDIO_BUS_FIELD_PROPERTY)prop[value->scope]=i;
        }
        CHECK(fx[0]!=SIZE_MAX && fx[1]!=SIZE_MAX && prop[0]!=SIZE_MAX && prop[1]!=SIZE_MAX);
        CHECK(!sh_package_audio_program_bus_equal(p,fx[0],fx[1]));
        CHECK(sh_package_audio_program_bus_equal(p,prop[0],prop[1]));
        const sh_audio_bus_policy *policy=sh_package_audio_program_bus_policy(p);CHECK(policy && policy->bus_count==1);
        CHECK(sh_package_audio_program_bus_edited(p));
        size_t selected_fx=SIZE_MAX;
        for(size_t i=0;policy && i<policy->count;i++) {
            const sh_audio_bus_setting *s=policy->settings+i;const sh_package_owners *owners=NULL,*gameplay=NULL;
            CHECK(sh_package_audio_program_bus_policy_owners(p,i,&owners,&gameplay));
            if(s->field.field==SH_AUDIO_BUS_FIELD_EFFECT) {
                selected_fx=i;CHECK(s->value==fx[0] || s->value==fx[1]);
                uint64_t expected=s->value==fx[0] ? 3 : 12;
                CHECK(owners && owners->bits==expected && gameplay && gameplay->bits==expected);
            }
            if(s->field.field==SH_AUDIO_BUS_FIELD_PROPERTY)CHECK(owners && owners->bits==5 && gameplay && gameplay->bits==5);
            if(s->field.field==SH_AUDIO_BUS_FIELD_SETTING && !s->field.key[0] && s->field.key[1]==1)
                CHECK(s->body[0]==1 && s->field.length==1 && owners && owners->bits==5);
        }
        CHECK(selected_fx!=SIZE_MAX);
        for(size_t s=0;s<2;s++) {
            const sh_package_audio_bus_value *effect=sh_package_audio_program_bus_at(p,fx[s]);
            const sh_package_audio_bus_value *property=sh_package_audio_program_bus_at(p,prop[s]);
            CHECK(effect && effect->owners.bits==((uint64_t)3<<(s*2)) && effect->gameplay.bits==effect->owners.bits);
            CHECK(property && property->owners.bits==((uint64_t)1<<(s*2)));
        }
        /* No metadata or source pointer is borrowed from the closed compiler. */
        memset(banks,0,sizeof(banks));memset(files,0,sizeof(files));memset(scopes,0,sizeof(scopes));
        unsigned char *body=NULL;size_t length=0;
        CHECK(sh_package_audio_program_bus_body(p,fx[0],0,&body,&length,error,sizeof(error)));
        CHECK(length==5 && body && body[0]==74);free(body);
        sh_package_compilation output={0};program_ids ids={0};ids.remaining=1000;
        CHECK(sh_package_audio_program_bind(p,&output,program_assign,&ids,error,sizeof(error)));
        CHECK(sh_package_audio_program_bus_body(p,fx[0],1,&body,&length,error,sizeof(error)));
        CHECK(length==5 && body && body[4]==1 && memcmp(body,"\x4a\0\0\0",4));free(body);
        const sh_audio_bus_policy *bound=sh_package_audio_program_bound_bus_policy(p);CHECK(bound && policy && bound->count==policy->count);
        unsigned masked=0;
        for(size_t i=0;bound && i<bound->count;i++) {
            const sh_audio_bus_setting *s=bound->settings+i;uint32_t mask=sh_audio_bus_record_mask(&s->field);
            if(!mask || s->removed)continue;masked++;
            for(size_t b=0;b<s->field.length;b++)CHECK(!(s->body[b]&~(unsigned char)(mask>>(8*b))));
            CHECK(s->field.length==policy->settings[i].field.length && !memcmp(s->body,policy->settings[i].body,s->field.length));
            if(s->field.field==SH_AUDIO_BUS_FIELD_SETTING && !s->field.key[0] && s->field.key[1]==1)CHECK(s->body[0]==1);
        }
        CHECK(masked>8);
        if(bound && selected_fx<bound->count) {
            const sh_audio_bus_setting *selected=bound->settings+selected_fx;
            CHECK(sh_package_audio_program_bus_body(p,selected->value,1,&body,&length,error,sizeof(error)));
            CHECK(length==selected->field.length && !memcmp(body,selected->body,length));free(body);
            unsigned char retained[5];memcpy(retained,selected->body,sizeof(retained));
            CHECK(policy->settings[selected_fx].body[0]==74 && memcmp(retained,policy->settings[selected_fx].body,4));
            ids.remaining=0;CHECK(!sh_package_audio_program_bind(p,&output,program_assign,&ids,error,sizeof(error)));
            CHECK(bound==sh_package_audio_program_bound_bus_policy(p) && !memcmp(retained,selected->body,5));
            ids.remaining=1000;CHECK(sh_package_audio_program_bind(p,&output,program_assign,&ids,error,sizeof(error)));
            bound=sh_package_audio_program_bound_bus_policy(p);CHECK(bound && !memcmp(retained,bound->settings[selected_fx].body,5));
        }
    }
    sh_package_audio_program_close(p);sh_audio_originals_close(originals);
}
static void bus_baseline_program(void)
{
    unsigned char original[256],effect[256],authored[256],unused[BANK_BYTES];
    uint32_t event;char base[4096],error[1024]={0};CHECK(sh_audio_event_identity("events/baseline-bus",&event));
    size_t size=bus_bank(original,event,10),effect_size=effect_bank(effect,11),authored_size=bus_bank(authored,event,20);
    binary("bus-baseline/sound/soundbanks/pc/a.bnk",original,size);
    binary("bus-baseline/sound/soundbanks/pc/b.bnk",effect,effect_size);
    uint32_t unused_id=0;CHECK(sh_audio_bank_identity("unused.bnk",&unused_id));bank_bytes(unused,unused_id,901,0);
    binary("bus-baseline/sound/soundbanks/pc/unused.bnk",unused,sizeof(unused));
    snprintf(base,sizeof(base),"%s/bus-baseline",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    sh_compiled_resource file={0};file.engine_path="sound/soundbanks/pc/a.bnk";file.body=authored;
    file.body_length=authored_size;file.owners.bits=file.gameplay_owners.bits=1;file.baseline_known=1;
    sh_package_compilation compiled={0};compiled.resources=&file;compiled.resource_count=1;
    const sh_package_compilation *input=&compiled;
    sh_package_audio_program *p=sh_package_audio_program_open(&input,1,originals,error,sizeof(error));CHECK(p);
    int ready=sh_package_audio_program_prepare(p,error,sizeof(error));
    if(!ready)fprintf(stderr,"bus baseline: %s\n",error);CHECK(ready);
    if(ready) {
        CHECK(sh_package_audio_program_scopes(p)==1 && sh_package_audio_program_banks(p,0)==2);
        size_t fx[2]={SIZE_MAX,SIZE_MAX},prop[2]={SIZE_MAX,SIZE_MAX};
        for(size_t i=0;i<sh_package_audio_program_bus_values(p);i++) {
            const sh_package_audio_bus_value *value=sh_package_audio_program_bus_at(p,i);size_t side=!!value->baseline;
            if(value->field.field==SH_AUDIO_BUS_FIELD_EFFECT)fx[side]=i;
            if(value->field.field==SH_AUDIO_BUS_FIELD_PROPERTY)prop[side]=i;
            if(side)CHECK(value->root.original && !value->owners.bits && !value->gameplay.bits);
        }
        CHECK(sh_package_audio_program_bus_equal(p,fx[0],fx[1]));
        CHECK(!sh_package_audio_program_bus_equal(p,prop[0],prop[1]));
        const sh_package_audio_bus_value *property=sh_package_audio_program_bus_at(p,prop[0]);
        const sh_package_audio_bus_value *same=sh_package_audio_program_bus_at(p,fx[0]);
        CHECK(property && !property->root.original && property->owners.bits==1);
        CHECK(same && same->root.original && !same->owners.bits);
        const sh_audio_bus_policy *policy=sh_package_audio_program_bus_policy(p);CHECK(policy);
        for(size_t i=0;policy && i<policy->count;i++) {
            const sh_audio_bus_setting *s=policy->settings+i;const sh_package_owners *owners=NULL,*gameplay=NULL;
            CHECK(sh_package_audio_program_bus_policy_owners(p,i,&owners,&gameplay));
            if(s->field.field==SH_AUDIO_BUS_FIELD_PROPERTY)CHECK(s->body[0]==20 && s->edited && owners->bits==1 && gameplay->bits==1);
            else CHECK(!s->edited && !owners->bits && !gameplay->bits);
        }
    }
    sh_package_audio_program_close(p);
    /* Removing a baseline property belongs to its author, not to the unchanged
     * private effect dependency in that same bus recipe. */
    authored_size=bus_bank(authored,event,10);
    size_t properties=BANK_BYTES+5+8;
    CHECK(authored[properties]==1 && authored[properties+1]==7);
    memmove(authored+properties+1,authored+properties+6,authored_size-properties-6);
    authored[properties]=0;authored_size-=5;
    put32(authored+BANK_BYTES+1,(uint32_t)(authored_size-BANK_BYTES-5));
    put32(authored+28,(uint32_t)(authored_size-32));file.body_length=authored_size;
    p=sh_package_audio_program_open(&input,1,originals,error,sizeof(error));CHECK(p);
    ready=sh_package_audio_program_prepare(p,error,sizeof(error));CHECK(ready);
    if(ready) {
        const sh_audio_bus_policy *policy=sh_package_audio_program_bus_policy(p);int removed=0;
        for(size_t i=0;policy && i<policy->count;i++)if(policy->settings[i].field.field==SH_AUDIO_BUS_FIELD_PROPERTY) {
            const sh_package_owners *owners=NULL,*gameplay=NULL;removed++;
            CHECK(policy->settings[i].removed && policy->settings[i].value==SIZE_MAX);
            CHECK(sh_package_audio_program_bus_policy_owners(p,i,&owners,&gameplay));
            CHECK(owners && owners->bits==1 && gameplay && gameplay->bits==1);
        }
        CHECK(removed==1);
        sh_package_compilation output={0};program_ids ids={0};ids.remaining=1000;
        CHECK(sh_package_audio_program_bind(p,&output,program_assign,&ids,error,sizeof(error)));
        const sh_audio_bus_policy *bound=sh_package_audio_program_bound_bus_policy(p);CHECK(bound);
        for(size_t i=0;bound && i<bound->count;i++)if(bound->settings[i].field.field==SH_AUDIO_BUS_FIELD_PROPERTY)
            CHECK(bound->settings[i].removed && bound->settings[i].edited && !bound->settings[i].body);
        /* The native configuration keeps the removed key's original value. */
        const sh_audio_bus_policy *native=sh_package_audio_program_bound_baseline_bus(p);int kept=0;
        CHECK(native && native->count && native->bus_count==bound->bus_count);
        for(size_t i=0;native && i<native->count;i++) {
            CHECK(!native->settings[i].edited);
            if(native->settings[i].field.field==SH_AUDIO_BUS_FIELD_PROPERTY) {
                kept++;CHECK(!native->settings[i].removed && native->settings[i].body[0]==10);
            }
        }
        CHECK(kept==1);
    }
    sh_package_audio_program_close(p);
    /* Missing explicit roots must not silently capture a different bank. */
    sh_package_compilation empty={0};uint32_t missing=12345;
    sh_package_audio_capture *capture=sh_package_audio_capture_open_roots(&empty,originals,&missing,1,error,sizeof(error));
    CHECK(!capture && strstr(error,"absent"));sh_package_audio_capture_close(capture);
    sh_audio_originals_close(originals);
    /* Dormant duplicate definitions must not turn one forward dependency into
     * loading every containing bank. Prefer an existing selected provider,
     * otherwise the first original registration inventory provider. */
    size_t duplicate_size=effect_bank(effect,55);put32(effect+12,unused_id);
    binary("bus-baseline/sound/soundbanks/pc/unused.bnk",effect,duplicate_size);
    originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    uint32_t wanted=84696446;
    capture=sh_package_audio_capture_open_roots(&empty,originals,&wanted,1,error,sizeof(error));
    CHECK(capture);
    if(capture) {
        const sh_audio_graph_scope *selected=sh_package_audio_capture_input(capture);
        CHECK(selected->bank_count==2);
        for(size_t i=0;i<selected->bank_count;i++)CHECK(selected->banks[i].codec->metadata.id!=unused_id);
    }
    sh_package_audio_capture_close(capture);wanted=84696445;
    capture=sh_package_audio_capture_open_roots(&empty,originals,&wanted,1,error,sizeof(error));
    CHECK(capture && sh_package_audio_capture_input(capture)->bank_count==1);
    sh_package_audio_capture_close(capture);sh_audio_originals_close(originals);
}

static int probe_original_id(void *context, const sh_audio_link_graph *graph, size_t node, sh_audio_identity_domain domain, uint32_t authored,
    const unsigned char binding[32], int original, int absent, uint32_t *id, char *error, size_t capacity)
{
    (void)context; (void)domain; (void)binding; (void)absent; CHECK(graph); (void)node;
    if (!original) { snprintf(error, capacity, "unmodified original audio acquired a private binding: %u", authored); return 0; }
    *id = authored; return 1;
}
static int probe_capture(const char *base, const char *filename, int inventory)
{
    char error[1024] = {0}; const sh_audio_original_bank *banks = NULL; size_t count = 0;
    sh_package_compilation empty = {0}; int ok = 0;
    uint32_t *events = NULL; size_t event_count = 0, event_at = 0, original_count = 0;
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL);
    sh_package_audio_capture *capture = NULL; sh_audio_link_graph *graph = NULL;
    sh_compiled_resource file = {0};
    if (!originals || sh_audio_originals_banks(originals, &banks, &count, error, sizeof(error)) != 1) goto done;
    if (inventory) {
        const char *path = NULL;uint32_t root_bank=0;
        for (size_t i = 0; i < count; i++) if (!_stricmp(banks[i].filename, filename)) { path = banks[i].path;root_bank=banks[i].metadata.id; break; }
        if(inventory==2) {
            if(!path)goto done;
            capture=sh_package_audio_capture_open_roots(&empty,originals,&root_bank,1,error,sizeof(error));
        } else {
        uint64_t length = 0;
        if (!path || sh_audio_originals_read(originals, path, 0, NULL, 0, &length, error, sizeof(error)) != 1 || length > SIZE_MAX) goto done;
        file.body = malloc(length ? (size_t)length : 1); if (!file.body) goto done;
        if (sh_audio_originals_read(originals, path, 0, file.body, (size_t)length, NULL, error, sizeof(error)) != 1) goto done;
        file.engine_path = path; file.body_length = (size_t)length;
        empty.resources = &file; empty.resource_count = 1;
        capture = sh_package_audio_capture_open_inventory(&empty, originals, error, sizeof(error));
        }
        if (!capture) goto done;
        const sh_audio_graph_scope *input = sh_package_audio_capture_input(capture);
        for (size_t i = 0; i < input->bank_count; i++) event_count += input->banks[i].codec->metadata.event_count;
        events = malloc((event_count ? event_count : 1) * sizeof(*events)); if (!events) goto done;
        for (size_t i = 0; i < input->bank_count; i++) {
            const sh_audio_bank *bank = &input->banks[i].codec->metadata;
            if (bank->event_count) memcpy(events + event_at, bank->events, bank->event_count * sizeof(*events));
            event_at += bank->event_count;
        }
    } else {
    for (size_t i = 0; i < count; i++) if (!strcmp(filename, "*") || !_stricmp(banks[i].filename, filename))
        event_count += banks[i].metadata.event_count;
    if (!event_count) { snprintf(error, sizeof(error), "original bank has no events: %s", filename); goto done; }
    events = malloc(event_count * sizeof(*events)); if (!events) goto done;
    for (size_t i = 0; i < count; i++) if (!strcmp(filename, "*") || !_stricmp(banks[i].filename, filename)) {
        memcpy(events + event_at, banks[i].metadata.events, banks[i].metadata.event_count * sizeof(*events));
        event_at += banks[i].metadata.event_count;
    }
    capture = sh_package_audio_capture_open(&empty, originals, events, event_count, error, sizeof(error));
    }
    if (!capture) goto done;
    const sh_audio_graph_scope *input = sh_package_audio_capture_input(capture);
    graph = sh_audio_link_graph_open(input, 1, error, sizeof(error)); if (!graph) goto done;
    for (size_t i = 0; i < event_count; i++) {
        sh_audio_graph_root event;
        if (!sh_audio_link_graph_event(graph, 0, events[i], &event, error, sizeof(error))) goto done;
        original_count += event.original != 0;
    }
    if (original_count != event_count ||
        !sh_audio_link_graph_bind(graph, probe_original_id, NULL, error, sizeof(error))) goto done;
    for (size_t i = 0; i < input->bank_count; i++) {
        const sh_audio_bank_binding *binding = sh_audio_link_graph_bank(graph, 0, i);
        if (!binding || binding->count) {
            snprintf(error, sizeof(error), "original audio bank unexpectedly requires rewriting: %s", input->banks[i].path); goto done;
        }
    }
    printf("captured %s: %zu event entries, %zu selected banks, %zu original event bindings\n",
        filename, event_count, input->bank_count, original_count); ok = 1;
done:
    if (!ok) fprintf(stderr, "audio capture probe: %s\n", error);
    sh_audio_link_graph_close(graph); sh_package_audio_capture_close(capture); sh_audio_originals_close(originals); free(events); free(file.body);
    return ok ? 0 : 1;
}
static int probe_resource_order(const void *left,const void *right)
{ return strcmp(((const sh_compiled_resource *)left)->engine_path,((const sh_compiled_resource *)right)->engine_path); }
static int probe_policy(const char *base,const char *filename)
{
    char error[1024]={0};const sh_audio_original_bank *banks=NULL;size_t count=0;int ok=0;
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);
    sh_package_compilation compiled={0},output={0};sh_package_audio_program *program=NULL;
    if(!originals || sh_audio_originals_banks(originals,&banks,&count,error,sizeof(error))!=1)goto done;
    compiled.resources=calloc(count ? count : 1,sizeof(*compiled.resources));if(!compiled.resources)goto done;
    for(size_t i=0;i<count;i++)if(!strcmp(filename,"*") || !_stricmp(filename,banks[i].filename)) {
        sh_compiled_resource *r=compiled.resources+compiled.resource_count++;uint64_t length=0;
        r->engine_path=banks[i].path;r->baseline_known=1;
        if(sh_audio_originals_read(originals,r->engine_path,0,NULL,0,&length,error,sizeof(error))!=1 || length>SIZE_MAX)goto done;
        r->body=malloc(length ? (size_t)length : 1);r->body_length=(size_t)length;if(!r->body)goto done;
        if(sh_audio_originals_read(originals,r->engine_path,0,r->body,(size_t)length,NULL,error,sizeof(error))!=1)goto done;
    }
    if(!compiled.resource_count){snprintf(error,sizeof(error),"bank was not found");goto done;}
    qsort(compiled.resources,compiled.resource_count,sizeof(*compiled.resources),probe_resource_order);
    const sh_package_compilation *input=&compiled;
    program=sh_package_audio_program_open(&input,1,originals,error,sizeof(error));
    if(!program || !sh_package_audio_program_prepare(program,error,sizeof(error)) ||
        !sh_package_audio_program_bind(program,&output,probe_original_id,NULL,error,sizeof(error)))goto done;
    if(sh_package_audio_program_bus_edited(program)){snprintf(error,sizeof(error),"original buses reported an edit");goto done;}
    const sh_audio_bus_policy *unbound=sh_package_audio_program_bus_policy(program);
    const sh_audio_bus_policy *bound=sh_package_audio_program_bound_bus_policy(program);
    if(!unbound || !bound || bound->count!=unbound->count)goto done;
    size_t removed=0;
    for(size_t i=0;i<bound->count;i++) {
        const sh_audio_bus_setting *a=unbound->settings+i,*b=bound->settings+i;
        const sh_package_owners *owners=NULL,*gameplay=NULL;
        if(!sh_package_audio_program_bus_policy_owners(program,i,&owners,&gameplay) ||
            sh_package_owners_count(owners) || sh_package_owners_count(gameplay) || a->removed!=b->removed ||
            a->field.length!=b->field.length || (a->field.length && memcmp(a->body,b->body,a->field.length))) {
            snprintf(error,sizeof(error),"original bus setting gained an edit or owner");goto done;
        }
        removed+=b->removed!=0;
    }
    /* Installed originals: the native-only composition is the same policy. */
    const sh_audio_bus_policy *native=sh_package_audio_program_bound_baseline_bus(program);
    if(!native || native->count!=bound->count || native->bus_count!=bound->bus_count) {
        snprintf(error,sizeof(error),"native baseline bus policy differs from the composed originals");goto done;
    }
    for(size_t i=0;i<native->count;i++) {
        const sh_audio_bus_setting *a=native->settings+i,*b=bound->settings+i;
        if(a->edited || b->edited || a->bus!=b->bus || a->field.field!=b->field.field || a->field.length!=b->field.length ||
            (a->field.length && memcmp(a->body,b->body,a->field.length))) {
            snprintf(error,sizeof(error),"native baseline bus setting differs from the composed originals");goto done;
        }
    }
    /* Shared STMG/ENVS/PLAT: exact graph identities certify the installed
     * controls as baseline, and binding never changes their bytes. */
    const sh_audio_shared_policy *shared=sh_package_audio_program_shared(program);
    const sh_audio_shared_policy *bound_shared=sh_package_audio_program_bound_shared(program);
    if(!shared || !bound_shared || shared->count!=bound_shared->count)goto done;
    for(size_t i=0;i<shared->count;i++) {
        const sh_audio_shared_setting *a=shared->settings+i,*b=bound_shared->settings+i;
        const sh_package_owners *owners=NULL,*gameplay=NULL;
        if(a->edited || !sh_package_audio_program_shared_owners(program,i,&owners,&gameplay) ||
            sh_package_owners_count(owners) || sh_package_owners_count(gameplay) ||
            a->record.length!=b->record.length || memcmp(a->body,b->body,a->record.length)) {
            snprintf(error,sizeof(error),"original shared setting gained an edit or owner");goto done;
        }
    }
    printf("composed/bound %s: %zu input banks, %zu selected banks, %zu buses, %zu settings, %zu removed, %zu shared controls; original IDs/bytes, no package owners\n",
        filename,compiled.resource_count,sh_package_audio_program_banks(program,0),bound->bus_count,bound->count,removed,shared->count);ok=1;
    /* Field inventory of the native bus configuration a publication must own. */
    static const char *const names[]={"parent","property","setting","duck-setting","duck","bypass","effect","mixer",
        "flags","rtpc-mode","rtpc-curve","state-group","state"};
    size_t fields[13]={0},kinds[2]={0},zero_parent=0;
    for(size_t i=0;i<bound->count;i++)if(bound->settings[i].field.field<13 && !bound->settings[i].removed) {
        const sh_audio_bus_setting *s=bound->settings+i;fields[s->field.field]++;
        if(s->field.field==SH_AUDIO_BUS_FIELD_PARENT && s->field.length==4 && !s->body[0] && !s->body[1] && !s->body[2] && !s->body[3])zero_parent++;
    }
    for(size_t i=0;i<bound->bus_count;i++)kinds[bound->buses[i].kind==20]++;
    printf("bus fields %s: %zu normal, %zu auxiliary, %zu root parents;",filename,kinds[0],kinds[1],zero_parent);
    for(size_t f=0;f<13;f++)printf(" %s %zu",names[f],fields[f]);
    printf("\n");
done:
    if(!ok)fprintf(stderr,"audio bus policy probe: %s\n",error);
    sh_package_audio_program_close(program);
    for(size_t i=0;i<compiled.resource_count;i++)free(compiled.resources[i].body);
    free(compiled.resources);sh_audio_originals_close(originals);return ok ? 0 : 1;
}

/* Init copies whose STMG switch is driven by a private modulator. The cooked
 * STMG bytes are identical; only the modulator definition differs. */
static int program_sink(void *context,const void *bytes,size_t length)
{
    struct { unsigned char *body;size_t used; } *sink=context;
    memcpy(sink->body+sink->used,bytes,length);sink->used+=length;return 1;
}
static size_t modulator_bank(unsigned char bank[256],uint32_t id,unsigned tone)
{
    memset(bank,0,256);memcpy(bank,"BKHD",4);put32(bank+4,16);put32(bank+8,113);put32(bank+12,id);
    size_t at=24;memcpy(bank+at,"HIRC",4);put32(bank+at+4,35);put32(bank+at+8,2);at+=12;
    bank[at]=4;put32(bank+at+1,8);put32(bank+at+5,90);at+=13;
    bank[at]=22;put32(bank+at+1,13);put32(bank+at+5,555);bank[at+9]=1;bank[at+10]=7;put32(bank+at+11,tone);at+=18;
    memcpy(bank+at,"STMG",4);put32(bank+at+4,43);at+=8;
    float threshold=-60;memcpy(bank+at,&threshold,4);bank[at+4]=64;at+=10;put32(bank+at,1);at+=4;
    put32(bank+at,700);put32(bank+at+4,555);bank[at+8]=2;put32(bank+at+9,1);put32(bank+at+17,3);put32(bank+at+21,4);at+=25;
    at+=4;return at;
}
static uint32_t bound_modulator(sh_package_audio_program *p,size_t scope)
{
    const sh_audio_bank_binding *binding=NULL;char error[512];size_t written=0;uint32_t id=0;
    const sh_audio_graph_bank *bank=sh_package_audio_program_bank(p,scope,0,&binding);
    CHECK(bank && binding);if(!bank || !binding)return 0;
    unsigned char *out=malloc((size_t)binding->output_length);CHECK(out);if(!out)return 0;
    struct { unsigned char *body;size_t used; } sink={out,0};
    CHECK(sh_audio_bank_binding_write(bank->source,binding,program_sink,&sink,error,sizeof(error)));written=sink.used;
    for(size_t at=0;at+8<=written;) {
        uint32_t size=(uint32_t)out[at+4]|(uint32_t)out[at+5]<<8|(uint32_t)out[at+6]<<16|(uint32_t)out[at+7]<<24;
        if(!memcmp(out+at,"HIRC",4)) {
            size_t cursor=at+12,end=at+8+size;
            while(cursor+5<=end) {
                uint32_t length=(uint32_t)out[cursor+1]|(uint32_t)out[cursor+2]<<8|(uint32_t)out[cursor+3]<<16|(uint32_t)out[cursor+4]<<24;
                if(out[cursor]==22)memcpy(&id,out+cursor+5,4);
                cursor+=5+length;
            }
        }
        at+=8+size;
    }
    free(out);return id;
}
static uint32_t bound_switch_controller(sh_package_audio_program *p,size_t *setting_index)
{
    const sh_audio_shared_policy *bound=sh_package_audio_program_bound_shared(p);CHECK(bound);if(!bound)return 0;
    for(size_t i=0;i<bound->count;i++)if(bound->settings[i].record.kind==SH_AUDIO_SHARED_SWITCH) {
        uint32_t controller;memcpy(&controller,bound->settings[i].body+4,4);if(setting_index)*setting_index=i;return controller;
    }
    CHECK(0);return 0;
}
static void shared_modulator_program(void)
{
    unsigned char native[256],a[256],b[256],copy[256];uint32_t init_id;char base[4096],error[1024]={0};
    CHECK(sh_audio_bank_identity("init.bnk",&init_id));
    size_t native_length=modulator_bank(native,init_id,1),a_length=modulator_bank(a,init_id,2);
    size_t b_length=modulator_bank(b,init_id,3),copy_length=modulator_bank(copy,init_id,2);
    binary("modulator-base/sound/soundbanks/pc/init.bnk",native,native_length);
    snprintf(base,sizeof(base),"%s/modulator-base",root);
    sh_audio_originals *originals=sh_audio_originals_open(base,L"English(US)",NULL);CHECK(originals);
    sh_compiled_resource files[3]={{0},{0},{0}};sh_package_compilation scopes[3]={{0},{0},{0}};
    const sh_package_compilation *inputs[]={scopes,scopes+1,scopes+2};
    unsigned char *bodies[3]={a,b,copy};size_t lengths[3]={a_length,b_length,copy_length};
    for(size_t i=0;i<3;i++) {
        files[i].engine_path="sound/soundbanks/pc/init.bnk";files[i].body=bodies[i];files[i].body_length=lengths[i];
        files[i].owners.bits=files[i].gameplay_owners.bits=(uint64_t)1<<i;
        scopes[i].resources=files+i;scopes[i].resource_count=1;
    }
    sh_package_audio_program *p=sh_package_audio_program_open(inputs,3,originals,error,sizeof(error));CHECK(p);
    int ready=p && sh_package_audio_program_prepare(p,error,sizeof(error));
    if(!ready)fprintf(stderr,"modulator program: %s\n",error);CHECK(ready);
    if(ready) {
        const sh_audio_shared_policy *policy=sh_package_audio_program_shared(p);CHECK(policy);
        const sh_audio_shared_setting *choice=NULL;size_t choice_index=0;
        for(size_t i=0;policy && i<policy->count;i++)if(policy->settings[i].record.kind==SH_AUDIO_SHARED_SWITCH){choice=policy->settings+i;choice_index=i;}
        /* Byte-identical STMG from every scope, yet the switch is an edit: its
         * modulator is not the native one. Only exact contributors share it. */
        CHECK(choice && choice->edited);
        size_t winner=SIZE_MAX,bank=SIZE_MAX;int baseline=1;
        CHECK(choice && sh_package_audio_program_shared_origin(p,choice->input,&winner,&bank,&baseline) && !baseline);
        const sh_package_owners *owners=NULL,*gameplay=NULL;
        CHECK(sh_package_audio_program_shared_owners(p,choice_index,&owners,&gameplay));
        uint64_t expected=winner==1 ? 2 : 5;
        CHECK(owners && owners->bits==expected && gameplay && gameplay->bits==expected);
        CHECK(choice && choice->contributor_count==(winner==1 ? 1u : 2u));
        /* The unchanged threshold stays native and owns nothing. */
        for(size_t i=0;policy && i<policy->count;i++)if(policy->settings[i].record.kind==SH_AUDIO_SHARED_THRESHOLD) {
            CHECK(!policy->settings[i].edited);
            CHECK(sh_package_audio_program_shared_owners(p,i,&owners,&gameplay) && !owners->bits);
        }
        CHECK(!sh_package_audio_program_bound_shared(p));
        sh_package_compilation provider={0};program_ids ids={0};ids.remaining=1000;
        CHECK(sh_package_audio_program_bind(p,&provider,program_assign,&ids,error,sizeof(error)));
        uint32_t controller=bound_switch_controller(p,NULL);
        CHECK(controller && controller!=555 && winner<3 && controller==bound_modulator(p,winner));
        CHECK(bound_modulator(p,1)!=bound_modulator(p,0) && bound_modulator(p,0)==bound_modulator(p,2));
        /* The unbound policy keeps the authored number for comparison. */
        uint32_t authored;memcpy(&authored,choice->body+4,4);CHECK(authored==555);
        /* A failed reservation keeps the complete prior bound view. */
        program_ids refused={0};refused.remaining=0;
        CHECK(!sh_package_audio_program_bind(p,&provider,program_assign,&refused,error,sizeof(error)));
        CHECK(bound_switch_controller(p,NULL)==controller);
    }
    sh_package_audio_program_close(p);sh_audio_originals_close(originals);
}
typedef struct witness_loads { uint32_t ids[4]; size_t count; int refuse; } witness_loads;
static int witness_load(void *context,uint32_t id,const char *filename,int numeric)
{
    witness_loads *plan=context;
    CHECK(filename && !strcmp(filename,"a.bnk"));
    CHECK(numeric==(id!=84696446));
    if(plan->refuse)return 0;
    CHECK(plan->count<4);if(plan->count==4)return 0;
    plan->ids[plan->count++]=id;return 1;
}
static void native_event_witness(void)
{
    unsigned char native[BANK_BYTES + 13], changed[sizeof(native)];
    uint32_t kept, removed, added; char base[4096], error[1024] = {0};
    CHECK(sh_audio_event_identity("events/kept", &kept));
    CHECK(sh_audio_event_identity("events/removed", &removed));
    CHECK(sh_audio_event_identity("events/added", &added));
    bank_bytes(native, 84696446, kept, 0);
    memcpy(native + BANK_BYTES, native + 36, 13);
    put32(native + BANK_BYTES + 5, removed);
    put32(native + 28, 30); put32(native + 32, 2);
    memcpy(changed, native, sizeof(native)); put32(changed + BANK_BYTES + 5, added);
    binary("event-witness/sound/soundbanks/pc/a.bnk", native, sizeof(native));
    snprintf(base, sizeof(base), "%s/event-witness", root);
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); CHECK(originals);
    sh_compiled_resource file = {0}; sh_package_compilation scope = {0};
    file.engine_path = "sound/soundbanks/pc/a.bnk"; file.body = changed; file.body_length = sizeof(changed);
    file.baseline_known = 1; file.owners.bits = file.gameplay_owners.bits = 1;
    scope.resources = &file; scope.resource_count = 1;
    const sh_package_compilation *input = &scope;
    sh_package_audio_program *p = sh_package_audio_program_open(&input, 1, originals, error, sizeof(error)); CHECK(p);
    CHECK(sh_package_audio_program_collect(p, 0, "events/kept", error, sizeof(error)));
    CHECK(sh_package_audio_program_collect(p, 0, "events/added", error, sizeof(error)));
    int ready = sh_package_audio_program_prepare(p, error, sizeof(error));
    if (!ready) fprintf(stderr, "event witness: %s\n", error); CHECK(ready);
    if (ready) {
        sh_audio_graph_root a, b; const sh_package_owners *owners, *gameplay;
        CHECK(sh_package_audio_program_root(p, 0, "events/kept", &a, error, sizeof(error)) && a.original);
        CHECK(sh_package_audio_program_root(p, 0, "events/added", &b, error, sizeof(error)) && !b.original);
        CHECK(sh_package_audio_program_owners(p, 0, "events/kept", &owners, &gameplay) && !owners->bits && !gameplay->bits);
        CHECK(sh_package_audio_program_owners(p, 0, "events/added", &owners, &gameplay) && owners->bits == 1 && gameplay->bits == 1);
        CHECK(!sh_package_audio_program_root(p, 0, "events/removed", &a, error, sizeof(error)));
        sh_package_compilation output={0};program_ids ids={0};ids.remaining=1000;
        CHECK(sh_package_audio_program_bind(p,&output,program_assign,&ids,error,sizeof(error)));
        witness_loads plan={0};
        CHECK(sh_package_audio_program_loads(p,witness_load,&plan,error,sizeof(error)));
        CHECK(plan.count==2 && plan.ids[0]==84696446 && plan.ids[1]!=84696446);
        plan.refuse=1;
        CHECK(!sh_package_audio_program_loads(p,witness_load,&plan,error,sizeof(error)) && *error);
    }
    sh_package_audio_program_close(p); sh_audio_originals_close(originals);
}
typedef struct probe_reservations {
    const uint32_t *originals; size_t original_count, native_count[11], private_count[11];
    uint32_t next;
} probe_reservations;
static int probe_number_order(const void *a, const void *b)
{ uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b; return x<y ? -1 : x>y; }
static int probe_reserve(void *context, const sh_audio_link_graph *graph, size_t node,
    sh_audio_identity_domain domain, uint32_t authored, const unsigned char binding[32],
    int original, int absent, uint32_t *id, char *error, size_t capacity)
{
    probe_reservations *p=context; (void)graph; (void)node; (void)binding; (void)absent;
    if(original) {p->native_count[domain]++;*id=authored;return 1;}
    do {
        if(!++p->next) {snprintf(error,capacity,"probe exhausted numeric IDs");return 0;}
    } while(bsearch(&p->next,p->originals,p->original_count,sizeof(uint32_t),probe_number_order));
    p->private_count[domain]++;*id=p->next;return 1;
}
static int probe_authored_bank(const char *base,const char *engine_path,const char *filename)
{
    char error[1024]={0};int ok=0;FILE *stream=NULL;__int64 length=0;
    sh_audio_originals *originals=NULL;sh_package_audio_program *program=NULL;
    sh_compiled_resource file={0};sh_package_compilation scope={0},output={0};
    probe_reservations ids={0};
    stream=fopen(filename,"rb");if(!stream || _fseeki64(stream,0,SEEK_END) ||
        (length=_ftelli64(stream))<0 || (uint64_t)length>SIZE_MAX || _fseeki64(stream,0,SEEK_SET))goto done;
    file.body=malloc(length ? (size_t)length : 1);file.body_length=(size_t)length;
    if(!file.body || fread(file.body,1,file.body_length,stream)!=file.body_length)goto done;
    fclose(stream);stream=NULL;
    file.engine_path=engine_path;file.baseline_known=1;file.owners.bits=file.gameplay_owners.bits=1;
    scope.resources=&file;scope.resource_count=1;
    originals=sh_audio_originals_open(base,L"English(US)",NULL);
    if(!originals || sh_audio_originals_ids(originals,&ids.originals,&ids.original_count,error,sizeof(error))!=1)goto done;
    const sh_package_compilation *input=&scope;
    program=sh_package_audio_program_open(&input,1,originals,error,sizeof(error));
    if(!program || !sh_package_audio_program_prepare(program,error,sizeof(error)) ||
        !sh_package_audio_program_bind(program,&output,probe_reserve,&ids,error,sizeof(error)))goto done;
    size_t private_banks=0;uint64_t private_bytes=0;
    for(size_t i=0;i<sh_package_audio_program_banks(program,0);i++) {
        const sh_audio_bank_binding *binding=NULL;
        const sh_audio_graph_bank *bank=sh_package_audio_program_bank(program,0,i,&binding);
        binding=sh_package_audio_program_private_bank(program,0,i);
        if(binding->count) {
            private_banks++;private_bytes+=binding->output_length;
            printf("private bank: %s, %llu bytes, %zu edits\n",bank->path,(unsigned long long)binding->output_length,binding->count);
        }
    }
    printf("selected banks=%zu private banks=%zu private bytes=%llu\n",sh_package_audio_program_banks(program,0),
        private_banks,(unsigned long long)private_bytes);
    for(size_t i=0;i<11;i++)printf("domain=%zu native=%zu private=%zu\n",i,ids.native_count[i],ids.private_count[i]);
    ok=1;
done:
    if(!ok)fprintf(stderr,"authored bank probe: %s\n",*error ? error : "input unavailable");
    if(stream)fclose(stream);
    sh_package_audio_program_close(program);sh_audio_originals_close(originals);free(file.body);return ok ? 0 : 1;
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--inspect-bank")) return probe_authored_bank(argv[2],argv[3],argv[4]);
    if (argc == 4 && !strcmp(argv[1], "--capture-originals")) return probe_capture(argv[2], argv[3], 0);
    if (argc == 4 && !strcmp(argv[1], "--inventory-originals")) return probe_capture(argv[2], argv[3], 1);
    if (argc == 4 && !strcmp(argv[1], "--roots-originals")) return probe_capture(argv[2], argv[3], 2);
    if (argc == 4 && !strcmp(argv[1], "--policy-originals")) return probe_policy(argv[2],argv[3]);
    char temp[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temp),temp)>0);
    snprintf(root,sizeof(root),"%ssh-package-audio-%lu",temp,(unsigned long)GetCurrentProcessId());
    CHECK(CreateDirectoryA(root,NULL));
    memory_index(); media_index(); bank_content_identity(); authored_and_map(); immutable_scope(); compact_event_scope(); typed_capture(); retained_program(); inventory_capture(); retry_program(); shared_program(); shared_native_baseline(); bus_program(); bus_baseline_program(); shared_modulator_program(); native_event_witness(); cleanup();
    if(failures) return 1;
    puts("package audio source indexes passed"); return 0;
}
