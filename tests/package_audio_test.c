#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_audio.h"
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

int main(void)
{
    char temp[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temp),temp)>0);
    snprintf(root,sizeof(root),"%ssh-package-audio-%lu",temp,(unsigned long)GetCurrentProcessId());
    CHECK(CreateDirectoryA(root,NULL));
    memory_index(); media_index(); bank_content_identity(); authored_and_map(); cleanup();
    if(failures) return 1;
    puts("package audio source indexes passed"); return 0;
}
