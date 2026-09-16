/* Installed packed/loose audio ownership and real runtime availability. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_originals.h"
#include "audio_packages.h"
#include "package_runtime.h"
#include "package_fixture.h"

void backend_log(const char *message) { (void)message; }
static void put(unsigned char *p,uint32_t value)
{ for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(i*8)); }
static void utf16(unsigned char *p,const char *name)
{ while(*name){*p++=(unsigned char)*name++;*p++=0;}*p++=0;*p=0; }
static void binary(const char *relative,const unsigned char *bytes,size_t length)
{
    char path[4096];FILE *file=NULL;
    create(relative,"");snprintf(path,sizeof(path),"%s/%s",root,relative);
    CHECK(!fopen_s(&file,path,"wb"));if(!file)return;
    CHECK(fwrite(bytes,1,length,file)==length);CHECK(!fclose(file));
}
static void packed(const char *relative,unsigned kind,uint64_t id,uint32_t language,const char *body)
{
    unsigned char bytes[1024]={0};uint32_t position=72,width=kind==2 ? 24 : 20;
    size_t length=strlen(body);CHECK(length<=sizeof(bytes)-512);if(length>sizeof(bytes)-512)return;
    memcpy(bytes,"AKPK",4);put(bytes+4,76+width);put(bytes+8,1);put(bytes+12,44);
    for(unsigned i=0;i<3;i++)put(bytes+16+4*i,4+(i==kind ? width : 0));
    put(bytes+28,2);put(bytes+32,20);put(bytes+36,1);put(bytes+40,36);put(bytes+44,0);
    utf16(bytes+48,"english");utf16(bytes+64,"sfx");
    for(unsigned i=0;i<3;i++) {
        if(i==kind) {
            put(bytes+position,1);unsigned char *row=bytes+position+4;
            put(row,(uint32_t)id);if(kind==2)put(row+4,(uint32_t)(id>>32));
            row+=width-16;put(row,256);put(row+4,(uint32_t)length);put(row+8,2);put(row+12,language);
            position+=width;
        }
        position+=4;
    }
    memcpy(bytes+512,body,length);binary(relative,bytes,512+length);
}
static sh_audio_originals *open_prefixed(const char *base,const wchar_t *language,const char *prefix)
{
    char path[4096];snprintf(path,sizeof(path),"%s/%s",root,base);
    sh_audio_originals *out=sh_audio_originals_open(path,language,prefix);CHECK(out);return out;
}
static sh_audio_originals *open_originals(const char *base,const wchar_t *language)
{ return open_prefixed(base,language,NULL); }
static const char *g_prefix="";
static void expect(sh_audio_originals *originals,const char *relative,int scope,const char *body)
{
    char path[4096],error[256];sh_package_original_identity answer;sh_package_file_identity wanted;
    snprintf(path,sizeof(path),"sound/soundbanks/pc/%s%s",g_prefix,relative);
    CHECK(sh_audio_originals_identity(originals,path,&answer,error,sizeof(error))==1 && !error[0]);
    if(error[0]) fprintf(stderr,"identity failed for %s: %s\n",path,error);
    if(answer.scope!=scope) fprintf(stderr,"scope %d wanted %d for %s\n",answer.scope,scope,path);
    CHECK(answer.scope==scope);
    if(scope) {
        CHECK(sh_package_bytes_identity(body,strlen(body),&wanted));
        CHECK(answer.file.length==wanted.length && !memcmp(answer.file.digest,wanted.digest,32));
    } else CHECK(!answer.file.length);
}
/* The bounded reader answers from the same packed-before-loose candidate the
 * identity reader chose, without hashing the payload. */
static void expect_read(sh_audio_originals *originals,const char *relative,int present,const char *body)
{
    char path[4096],error[256],buffer[64];uint64_t length=0;
    snprintf(path,sizeof(path),"sound/soundbanks/pc/%s",relative);
    CHECK(sh_audio_originals_read(originals,path,0,NULL,0,&length,error,sizeof(error))==present);
    if(!present) { CHECK(!length); return; }
    CHECK(length==strlen(body) && length<sizeof(buffer));
    CHECK(sh_audio_originals_read(originals,path,0,buffer,(size_t)length,NULL,error,sizeof(error))==1);
    CHECK(!memcmp(buffer,body,(size_t)length));
    if(length>1) {
        CHECK(sh_audio_originals_read(originals,path,1,buffer,(size_t)length-1,NULL,error,sizeof(error))==1);
        CHECK(!memcmp(buffer,body+1,(size_t)length-1));
    }
    CHECK(sh_audio_originals_read(originals,path,0,buffer,(size_t)length+1,NULL,error,sizeof(error))==-1);
    CHECK(strstr(error,"range")!=NULL);
}
static int count_banks(void *context,uint32_t id,uint32_t language)
{ (void)id;(void)language;++*(size_t *)context;return 1; }
static int stop_banks(void *context,uint32_t id,uint32_t language)
{ (void)id;(void)language;++*(size_t *)context;return 0; }

static int activate(void *context,int restoring,const sh_package_changes *changes,char *error,size_t capacity)
{ (void)context;(void)restoring;(void)error;(void)capacity;CHECK(changes);return 1; }
static void runtime_originals(void)
{
    char base[4096],map[4096],error[2048];sh_package_missing missing={0};
    const char *bank="sound/soundbanks/pc/foo.bnk";
    snprintf(base,sizeof(base),"%s/base",root);snprintf(map,sizeof(map),"%s/map",root);
    create("overrides/copy/package.json","{\"id\":\"copy\",\"name\":\"Copy\"}");
    create("overrides/copy/assets/sound/soundbanks/pc/foo.bnk","PACK");
    create("overrides/copy/assets/sound/soundbanks/pc/doom_weapon_sp.bnk","CAMPAIGN");
    sh_package_runtime_test_empty_catalog();sh_package_runtime_test_audio_originals(base,L"english");
    int refreshed=sh_package_runtime_refresh(root);
    if(!refreshed){sh_package_runtime_error(error,sizeof(error));fprintf(stderr,"runtime original refresh: %s\n",error);}
    CHECK(refreshed);
    const sh_package_compilation *library=sh_package_runtime_library_acquire();
    const sh_compiled_resource *resource=sh_package_compilation_find(library,bank);
    CHECK(resource && resource->baseline_known==1 && !sh_package_owners_count(&resource->gameplay_owners));
    resource=sh_package_compilation_find(library,"sound/soundbanks/pc/doom_weapon_sp.bnk");
    CHECK(resource && resource->baseline_known==2 && sh_package_owners_count(&resource->gameplay_owners)==1);
    sh_package_runtime_release();sh_package_runtime_test_dispose();
    /* An empty local library still recognizes installed original paths. The
     * map keeps its own bytes and does not rewrite those installed originals. */
    create("overrides/copy/package.json","{\"id\":\"copy\",\"name\":\"Copy\"}");
    char installed[4096];snprintf(installed,sizeof(installed),"%s/empty-library",root);
    create("empty-library",NULL);CHECK(sh_package_runtime_refresh(installed));
    create("map/overrides/authored/package.json","{\"id\":\"authored\",\"name\":\"Authored\"}");
    create("map/overrides/authored/assets/sound/soundbanks/pc/foo.bnk","MAP");
    create("map/overrides/authored/assets/sound/soundbanks/pc/doom_weapon_sp.bnk","MAP CAMPAIGN");
    sh_package_map_plan *plan=sh_package_runtime_prepare_map(root,map,error,sizeof(error));
    if(!plan)fprintf(stderr,"runtime original plan: %s\n",error);
    CHECK(plan);
    if(plan) {
        CHECK(sh_package_map_plan_payload_missing(plan,&missing,error,sizeof(error)) && missing.checked==2 && !missing.count);
        CHECK(sh_package_runtime_activate_prepared_map(plan,(sh_package_activation_guard){0},activate,NULL));
        unsigned char *bytes=NULL;size_t length=0;
        CHECK(sh_package_runtime_read(bank,&bytes,&length)==1 && length==3 && !memcmp(bytes,"MAP",3));free(bytes);
        CHECK(sh_package_runtime_activate_map(root,NULL,(sh_package_activation_guard){0},activate,NULL));
        bytes=NULL;length=0;CHECK(sh_package_runtime_read(bank,&bytes,&length)==0);free(bytes);
        sh_package_missing_free(&missing);sh_package_map_plan_free(plan);
    }
    sh_package_runtime_test_dispose();sh_package_runtime_test_audio_originals(NULL,NULL);
}
int main(void)
{
    char temporary[MAX_PATH],path[4096],error[256];uint32_t foo,bar;
    sh_package_original_identity answer;
    CHECK(GetTempPathA(sizeof(temporary),temporary));CHECK(GetTempFileNameA(temporary,"aor",0,root));
    CHECK(DeleteFileA(root));CHECK(CreateDirectoryA(root,NULL));
    CHECK(sh_audio_bank_identity("foo.bnk",&foo));CHECK(sh_audio_bank_identity("bar.bnk",&bar));
    packed("base/sound/soundbanks/pc/a.pck",0,foo,0,"PACK");
    packed("base/sound/soundbanks/pc/b.pck",0,foo,0,"PACK");
    packed("base/sound/soundbanks/pc/numeric.pck",0,42,0,"NUMERIC");
    packed("base/sound/soundbanks/pc/media.pck",1,99,0,"MEDIA");
    packed("base/sound/soundbanks/pc/external.pck",2,UINT64_C(0xa60566c319115c49),0,"EXTERNAL");
    packed("base/sound/soundbanks/pc/english/voice.pck",0,bar,1,"ENGLISH");
    create("base/sound/soundbanks/pc/foo.bnk","LOOSE");
    create("base/sound/soundbanks/pc/english/bar.bnk","LOOSE ENGLISH");
    create("base/sound/soundbanks/pc/doom_weapon_sp.bnk","CAMPAIGN");
    create("base/sound/soundbanks/pc/init.bnk","INIT");
    create("base/sound/soundbanks/pc/english/init.bnk","LOCAL INIT");
    create("base/sound/soundbanks/pc/doom_initial.bnk","INITIAL");
    create("base/sound/soundbanks/pc/deeper/path/loose.bnk","DEEP");
    create("base/sound/soundbanks/pc/extra_initial.pck","EXCLUDED");
    sh_audio_originals *originals=open_originals("base",L"ENGLISH");
    expect(originals,"foo.bnk",1,"PACK");expect(originals,"42.bnk",2,"NUMERIC");
    expect(originals,"99.wem",1,"MEDIA");expect(originals,"ext.wem",1,"EXTERNAL");
    expect(originals,"english/bar.bnk",1,"ENGLISH");expect(originals,"bar.bnk",0,NULL);
    expect(originals,"french/bar.bnk",0,NULL);expect(originals,"english/foo.bnk",0,NULL);
    expect(originals,"doom_weapon_sp.bnk",2,"CAMPAIGN");expect(originals,"init.bnk",1,"INIT");
    expect(originals,"english/init.bnk",2,"LOCAL INIT");expect(originals,"doom_initial.bnk",1,"INITIAL");
    expect(originals,"deeper/path/loose.bnk",2,"DEEP");expect(originals,"extra_initial.pck",2,"EXCLUDED");
    expect(originals,"missing.wem",0,NULL);
    CHECK(sh_audio_originals_identity(originals,"generated/image.bimage",&answer,error,sizeof(error))==0 && !answer.scope);
    /* The engine's configured bank prefix moves the whole tree the audio system
     * requests, so discovery and the identity reader must both follow it. A path
     * without the prefix is not this reader's to answer. */
    {
        sh_audio_originals *prefixed;
        create("prefixed/sound/soundbanks/pc/custom/kept.bnk","PREFIXED");
        create("prefixed/sound/soundbanks/pc/plain.bnk","OUTSIDE");
        packed("prefixed/sound/soundbanks/pc/custom/english/voice.pck",0,bar,1,"PREFIXED ENGLISH");
        prefixed=open_prefixed("prefixed",L"ENGLISH","custom/");
        g_prefix="custom/";
        expect(prefixed,"kept.bnk",1,"PREFIXED");
        /* Only inside a mounted package here, so startup would not have listed
         * it: available to install checks, not proof it is already loaded. */
        expect(prefixed,"english/bar.bnk",2,"PREFIXED ENGLISH");
        g_prefix="";
        /* A path outside the configured prefix is not this reader's to answer. */
        CHECK(sh_audio_originals_identity(prefixed,"sound/soundbanks/pc/kept.bnk",&answer,
            error,sizeof(error))==0 && !answer.scope);
        CHECK(sh_audio_originals_identity(prefixed,"sound/soundbanks/pc/plain.bnk",&answer,
            error,sizeof(error))==0 && !answer.scope);
        sh_audio_originals_close(prefixed);
    }
    snprintf(path,sizeof(path),"%s/base/sound/soundbanks/pc/a.pck",root);
    HANDLE writer=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    expect(originals,"foo.bnk",1,"PACK");
    /* Reading follows the same order: the package shadows the loose file. */
    expect_read(originals,"foo.bnk",1,"PACK");
    expect_read(originals,"english/bar.bnk",1,"ENGLISH");
    expect_read(originals,"deeper/path/loose.bnk",1,"DEEP");
    expect_read(originals,"99.wem",1,"MEDIA");
    expect_read(originals,"missing.bnk",0,NULL);
    CHECK(!sh_audio_originals_read(originals,"generated/image.bimage",0,NULL,0,NULL,error,sizeof(error)));
    CHECK(!sh_audio_originals_read(originals,"sound/soundbanks/pc/a.pck",0,NULL,0,NULL,error,sizeof(error)));
    CHECK(sh_audio_originals_read(NULL,"sound/soundbanks/pc/foo.bnk",0,NULL,0,NULL,error,sizeof(error))==0);
    {
        size_t visited=0;
        CHECK(sh_audio_originals_ready(originals) && !sh_audio_originals_ready(NULL));
        /* a.pck, b.pck, numeric.pck and the english voice package carry banks. */
        CHECK(sh_audio_originals_packaged_banks(originals,count_banks,&visited,error,sizeof(error))==1);
        CHECK(visited==4);
        visited=0;
        CHECK(sh_audio_originals_packaged_banks(originals,stop_banks,&visited,error,sizeof(error))==0);
        CHECK(visited==1);
        CHECK(sh_audio_originals_packaged_banks(NULL,count_banks,&visited,error,sizeof(error))==-1);
        CHECK(sh_audio_originals_packaged_banks(originals,NULL,&visited,error,sizeof(error))==-1);
    }
    sh_audio_originals_close(originals);
    originals=open_originals("base",NULL);
    CHECK(sh_audio_originals_identity(originals,"decls/entitydef/example.decl",&answer,error,sizeof(error))==0);
    CHECK(sh_audio_originals_identity(originals,"sound/soundbanks/pc/foo.bnk",&answer,error,sizeof(error))==-1 && strstr(error,"initializing"));
    CHECK(sh_audio_originals_read(originals,"sound/soundbanks/pc/foo.bnk",0,NULL,0,NULL,error,sizeof(error))==-1 && strstr(error,"initializing"));
    {
        size_t visited=0;
        CHECK(!sh_audio_originals_ready(originals));
        CHECK(sh_audio_originals_packaged_banks(originals,count_banks,&visited,error,sizeof(error))==-1);
        CHECK(!visited && strstr(error,"initializing"));
    }
    sh_audio_originals_close(originals);
    /* Two differing payloads inside one mount stage have no native winner: the
     * engine lists a directory unsorted, so the order is not defined. */
    packed("conflict/sound/soundbanks/pc/a.pck",0,foo,0,"FIRST");
    packed("conflict/sound/soundbanks/pc/b.pck",0,foo,0,"SECOND");
    originals=open_originals("conflict",L"english");
    CHECK(sh_audio_originals_identity(originals,"sound/soundbanks/pc/foo.bnk",&answer,error,sizeof(error))==-1 && strstr(error,"ambiguous") && !answer.scope);
    sh_audio_originals_close(originals);
    /* Across stages the order is defined: the language directory is mounted
     * after the platform directory and every lookup walks the package list from
     * its head, so the language package answers. */
    packed("stage/sound/soundbanks/pc/a.pck",0,foo,0,"PLATFORM");
    packed("stage/sound/soundbanks/pc/english/voice.pck",0,foo,0,"LANGUAGE");
    originals=open_originals("stage",L"english");
    expect(originals,"foo.bnk",2,"LANGUAGE");
    sh_audio_originals_close(originals);
    /* A package in another language directory is not mounted for this run. */
    packed("otherlang/sound/soundbanks/pc/a.pck",0,foo,0,"PLATFORM");
    packed("otherlang/sound/soundbanks/pc/french/voice.pck",0,foo,0,"FRENCH");
    originals=open_originals("otherlang",L"english");
    expect(originals,"foo.bnk",2,"PLATFORM");
    sh_audio_originals_close(originals);
    create("broken/sound/soundbanks/pc/bad.pck","AKPK");originals=open_originals("broken",L"english");
    CHECK(sh_audio_originals_identity(originals,"sound/soundbanks/pc/foo.bnk",&answer,error,sizeof(error))==-1 && strstr(error,"metadata"));
    sh_audio_originals_close(originals);
    originals=open_originals("absent",L"english");expect(originals,"foo.bnk",0,NULL);sh_audio_originals_close(originals);
    runtime_originals();
    originals=open_originals("base",L"english");expect(originals,"foo.bnk",1,"PACK");sh_audio_originals_close(originals);
    cleanup();if(failures)return 1;puts("installed audio original and runtime availability checks passed");return 0;
}
