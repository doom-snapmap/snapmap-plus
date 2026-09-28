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
static void packed_bytes(const char *relative,unsigned kind,uint64_t id,uint32_t language,const void *body,size_t length)
{
    unsigned char bytes[1024]={0};uint32_t position=72,width=kind==2 ? 24 : 20;
    CHECK(length<=sizeof(bytes)-512);if(length>sizeof(bytes)-512)return;
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
static void packed(const char *relative,unsigned kind,uint64_t id,uint32_t language,const char *body)
{ packed_bytes(relative,kind,id,language,body,strlen(body)); }
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

static void cooked_bank(unsigned char bytes[49],uint32_t id,uint32_t event,uint32_t language)
{
    memset(bytes,0,49);memcpy(bytes,"BKHD",4);put(bytes+4,16);put(bytes+8,113);
    put(bytes+12,id);put(bytes+16,language);memcpy(bytes+24,"HIRC",4);put(bytes+28,17);
    put(bytes+32,1);bytes[36]=4;put(bytes+37,8);put(bytes+41,event);
}
static void bank_catalog(void)
{
    unsigned char bank[49],other[49];uint32_t foo;char error[512],path[4096];
    CHECK(sh_audio_bank_identity("foo.bnk",&foo));cooked_bank(bank,foo,77,1);cooked_bank(other,foo,88,0);
    binary("catalog/sound/soundbanks/pc/foo.bnk",other,sizeof(other));
    binary("catalog/sound/soundbanks/pc/english/foo.bnk",other,sizeof(other));
    packed_bytes("catalog/sound/soundbanks/pc/english/voice.pck",0,foo,1,bank,sizeof(bank));
    cooked_bank(other,42,99,0);packed_bytes("catalog/sound/soundbanks/pc/a.pck",0,42,0,other,sizeof(other));
    packed("catalog/sound/soundbanks/pc/media.pck",1,9001,0,"UNUSED MEDIA");
    create("catalog/sound/soundbanks/pc/9002.wem","LOOSE UNUSED MEDIA");
    create("catalog/sound/soundbanks/pc/english/9003.wem","LOCALIZED UNUSED MEDIA");
    sh_audio_originals *originals=open_originals("catalog",L"english");
    const sh_audio_original_bank *rows=NULL,*again=NULL;size_t count=0,next=0;
    int result=sh_audio_originals_banks(originals,&rows,&count,error,sizeof(error));
    if(result!=1)fprintf(stderr,"catalog: %s\n",error);
    CHECK(result==1 && count==2);
    if(count==2) {
        CHECK(rows[0].metadata.id==foo && rows[0].metadata.events[0]==77);
        CHECK(!strcmp(rows[0].path,"sound/soundbanks/pc/english/foo.bnk"));
        CHECK(rows[1].metadata.id==42 && rows[1].metadata.events[0]==99);
        CHECK(!strcmp(rows[1].filename,"42.bnk"));
    }
    CHECK(sh_audio_originals_banks(originals,&again,&next,error,sizeof(error))==1 && again==rows && next==count);
    {
        const uint32_t *ids=NULL,*reused=NULL;size_t used=0,reused_count=0;
        CHECK(sh_audio_originals_ids(originals,&ids,&used,error,sizeof(error))==1 && used==7);
        uint32_t expected[]={42,77,99,9001,9002,9003,foo};
        for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
            size_t hits=0;for(size_t j=0;j<used;j++)hits+=ids[j]==expected[i];CHECK(hits==1);
        }
        for(size_t i=1;i<used;i++)CHECK(ids[i]>ids[i-1]);
        CHECK(sh_audio_originals_ids(originals,&reused,&reused_count,error,sizeof(error))==1 && reused==ids && reused_count==used);
    }
    snprintf(path,sizeof(path),"%s/catalog/sound/soundbanks/pc/english/voice.pck",root);
    HANDLE writer=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    sh_audio_originals_close(originals);
    originals=open_originals("catalog-absent",L"english");
    CHECK(sh_audio_originals_banks(originals,&rows,&count,error,sizeof(error))==1 && !count);
    { const uint32_t *ids=NULL;size_t used=3;CHECK(sh_audio_originals_ids(originals,&ids,&used,error,sizeof(error))==1 && !used); }
    sh_audio_originals_close(originals);
    create("catalog-bad/sound/soundbanks/pc/bad.bnk","BROKEN");
    originals=open_originals("catalog-bad",L"english");
    CHECK(sh_audio_originals_banks(originals,&rows,&count,error,sizeof(error))==-1 && !rows && !count && *error);
    { const uint32_t *ids=(const uint32_t *)1;size_t used=3;
      CHECK(sh_audio_originals_ids(originals,&ids,&used,error,sizeof(error))==-1 && !ids && !used && *error); }
    sh_audio_originals_close(originals);
}

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
static int rank_of(sh_audio_originals *originals,const char *relative,int authored,int authored_root,
    sh_audio_native_rank *rank)
{
    char path[4096],error[256]={0};
    snprintf(path,sizeof(path),"sound/soundbanks/pc/%s",relative);
    int result=sh_audio_originals_registration(originals,path,authored,authored_root,rank,error,sizeof(error));
    if(result<0)fprintf(stderr,"registration %s: %s\n",path,error);
    return result;
}
/* SnapMap's sound init (VK FUN_1402b97d0, GL FUN_1402b87c0): Init.bnk, the
 * prepared doom_initial.bnk, root names, then names only the language
 * directory lists; "initial" and SnapMap-mode "_sp" names are skipped. Within
 * a pass the order is the directory's own unsorted listing. */
static void registration_ranks(void)
{
    static const char *files[]={"init.bnk","doom_initial.bnk","doom_items.bnk","ab.bnk","a_b.bnk","x_sp.bnk",
        "my_initial.bnk","english/doom_vo.bnk","english/doom_items.bnk","english/init.bnk","french/f.bnk"};
    char relative[512],pattern[4096];sh_audio_native_rank rank,items,ab,a_b;
    for(size_t i=0;i<sizeof(files)/sizeof(*files);i++) {
        snprintf(relative,sizeof(relative),"ranks/sound/soundbanks/pc/%s",files[i]);create(relative,files[i]);
    }
    sh_audio_originals *originals=open_originals("ranks",L"ENGLISH");if(!originals)return;
    CHECK(rank_of(originals,"init.bnk",0,0,&rank)==1 && rank.phase==0);
    CHECK(rank_of(originals,"doom_initial.bnk",0,0,&rank)==1 && rank.phase==1);
    CHECK(rank_of(originals,"doom_items.bnk",0,0,&items)==1 && items.phase==2 && items.position!=SIZE_MAX);
    /* A language file the root pass requests by name registers at that root position. */
    CHECK(rank_of(originals,"english/doom_items.bnk",0,0,&rank)==1 && rank.phase==2 && rank.position==items.position);
    CHECK(rank_of(originals,"english/doom_vo.bnk",0,0,&rank)==1 && rank.phase==3 && rank.position!=SIZE_MAX);
    CHECK(rank_of(originals,"x_sp.bnk",0,0,&rank)==0);
    CHECK(rank_of(originals,"my_initial.bnk",0,0,&rank)==0);
    CHECK(rank_of(originals,"english/init.bnk",0,0,&rank)==0);
    CHECK(rank_of(originals,"french/f.bnk",0,0,&rank)==0);
    CHECK(rank_of(originals,"absent.bnk",0,0,&rank)==0);
    /* Authored files rank as if installed; an unlisted name has no native position. */
    CHECK(rank_of(originals,"doom_items.bnk",1,0,&rank)==1 && rank.phase==2 && rank.position==items.position);
    CHECK(rank_of(originals,"new.bnk",1,0,&rank)==1 && rank.phase==2 && rank.position==SIZE_MAX);
    CHECK(rank_of(originals,"new_sp.bnk",1,0,&rank)==0);
    CHECK(rank_of(originals,"english/new.bnk",1,0,&rank)==1 && rank.phase==3);
    CHECK(rank_of(originals,"english/new.bnk",1,1,&rank)==1 && rank.phase==2);
    /* The listing order, not a name order ("ab" and "a_b" compare differently
     * by lowercase and by uppercase), orders one pass. */
    CHECK(rank_of(originals,"ab.bnk",0,0,&ab)==1 && rank_of(originals,"a_b.bnk",0,0,&a_b)==1);
    snprintf(pattern,sizeof(pattern),"%s\\ranks\\sound\\soundbanks\\pc\\*",root);
    WIN32_FIND_DATAA found;HANDLE search=FindFirstFileA(pattern,&found);int ab_first=-1;
    CHECK(search!=INVALID_HANDLE_VALUE);
    if(search!=INVALID_HANDLE_VALUE) {
        do {
            if(!_stricmp(found.cFileName,"ab.bnk")){ab_first=1;break;}
            if(!_stricmp(found.cFileName,"a_b.bnk")){ab_first=0;break;}
        } while(FindNextFileA(search,&found));
        FindClose(search);
    }
    CHECK(ab_first>=0 && (ab.position<a_b.position)==(ab_first==1));
    sh_audio_originals_close(originals);
}
int main(int argc,char **argv)
{
    if(argc==3) {
        wchar_t language[256];char error[2048];const sh_audio_original_bank *banks=NULL;size_t count=0,events=0,media=0,targets=0;
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[2],-1,language,256))return 2;
        sh_audio_originals *catalog=sh_audio_originals_open(argv[1],language,NULL);
        int ok=catalog && sh_audio_originals_banks(catalog,&banks,&count,error,sizeof(error))==1;
        if(!ok)fprintf(stderr,"installed audio catalog: %s\n",catalog ? error : "allocation failed");
        for(size_t i=0;ok && i<count;i++){events+=banks[i].metadata.event_count;media+=banks[i].metadata.media_count;targets+=banks[i].metadata.target_count;}
        if(ok) {
            printf("installed audio catalog: %zu banks, %zu events, %zu media rows, %zu external targets\n",count,events,media,targets);
            const uint32_t *ids=NULL;size_t id_count=0;
            ok=sh_audio_originals_ids(catalog,&ids,&id_count,error,sizeof(error))==1;
            printf("installed original identities: %zu, %s%s%s\n",id_count,ok ? "pass" : "failed",ok ? "" : ": ",ok ? "" : error);
            if(!ok){sh_audio_originals_close(catalog);return 1;}
            const sh_audio_bank **view=calloc(count ? count : 1,sizeof(*view));unsigned char *selected=malloc(count ? count : 1);
            ok=view && selected;
            if(ok) {
                memset(selected,1,count);for(size_t i=0;i<count;i++)view[i]=&banks[i].metadata;
                ok=sh_audio_bank_closure(view,count,NULL,0,1,selected,NULL,error,sizeof(error));
                printf("installed complete inventory dependency closure: %s%s%s\n",ok ? "pass" : "failed",ok ? "" : ": ",ok ? "" : error);
            }
            free(view);free(selected);
        }
        sh_audio_originals_close(catalog);return ok ? 0 : 1;
    }
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
    bank_catalog();runtime_originals();registration_ranks();
    originals=open_originals("base",L"english");expect(originals,"foo.bnk",1,"PACK");sh_audio_originals_close(originals);
    cleanup();if(failures)return 1;puts("installed audio original and runtime availability checks passed");return 0;
}
