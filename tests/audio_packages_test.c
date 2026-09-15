/* PCK metadata, bounded reads, language identity and all native row formats. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_packages.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
typedef struct fixture { unsigned char bytes[256]; size_t length, read_bytes; int fail; } fixture;
static void put(unsigned char *p,uint32_t value)
{ for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(i*8)); }
static void row(unsigned char *p,uint32_t id,uint32_t language,uint32_t block,uint32_t sector,uint32_t length)
{ put(p,id);put(p+4,block);put(p+8,length);put(p+12,sector);put(p+16,language); }
static void string16(unsigned char *p,const char *name)
{ while(*name) { *p++=(unsigned char)*name++;*p++=0; }*p++=0;*p=0; }
static fixture sample(void)
{
    fixture f={0};
    memcpy(f.bytes,"AKPK",4);put(f.bytes+4,160);put(f.bytes+8,1);
    put(f.bytes+12,44);put(f.bytes+16,44);put(f.bytes+20,24);put(f.bytes+24,28);
    put(f.bytes+28,2);put(f.bytes+32,20);put(f.bytes+36,1);put(f.bytes+40,36);put(f.bytes+44,0);
    string16(f.bytes+48,"english");string16(f.bytes+64,"sfx");
    put(f.bytes+72,2);row(f.bytes+76,7,0,256,2,3);row(f.bytes+96,7,1,256,3,4);
    put(f.bytes+116,1);row(f.bytes+120,99,1,UINT32_MAX,2,5);
    put(f.bytes+140,1);put(f.bytes+144,7);put(f.bytes+148,1);
    put(f.bytes+152,256);put(f.bytes+156,6);put(f.bytes+160,5);put(f.bytes+164,1);
    f.length=168;return f;
}
static int read_fixture(void *context,uint64_t offset,void *out,size_t length)
{
    fixture *f=context;
    if(f->fail || offset>f->length || length>f->length-offset)return 0;
    memcpy(out,f->bytes+(size_t)offset,length);f->read_bytes+=length;return 1;
}
static sh_audio_bank_source source(fixture *f)
{ sh_audio_bank_source s={f,(UINT64_C(1)<<33)+16,read_fixture};return s; }
static void invalid(fixture *f)
{
    sh_audio_package p={0};char error[128];
    CHECK(sh_audio_package_read(source(f),&p,error,sizeof(error))==0 && error[0]);
    CHECK(!p.language_count && !p.languages && !p.entries[0] && !p.entries[1] && !p.entries[2]);
    sh_audio_package_free(&p);
}
static int read_file(void *context,uint64_t offset,void *out,size_t length)
{
    FILE *file=context;
    return offset<=INT64_MAX && !_fseeki64(file,(long long)offset,SEEK_SET) && fread(out,1,length,file)==length;
}
static size_t audit_directory(const char *directory)
{
    char pattern[4096],path[4096],error[256];WIN32_FIND_DATAA entry;size_t count=0;
    snprintf(pattern,sizeof(pattern),"%s/*",directory);
    HANDLE scan=FindFirstFileA(pattern,&entry);CHECK(scan!=INVALID_HANDLE_VALUE);
    if(scan==INVALID_HANDLE_VALUE)return 0;
    do {
        if(!strcmp(entry.cFileName,".") || !strcmp(entry.cFileName,".."))continue;
        snprintf(path,sizeof(path),"%s/%s",directory,entry.cFileName);
        CHECK(!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)continue;
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){count+=audit_directory(path);continue;}
        const char *dot=strrchr(entry.cFileName,'.');if(!dot || _stricmp(dot,".pck"))continue;
        FILE *file=NULL;CHECK(!fopen_s(&file,path,"rb"));if(!file)continue;
        CHECK(!_fseeki64(file,0,SEEK_END));long long size=_ftelli64(file);CHECK(size>=0);
        sh_audio_package package={0};sh_audio_bank_source s={file,(uint64_t)size,read_file};
        int result=sh_audio_package_read(s,&package,error,sizeof(error));
        if(result!=1)fprintf(stderr,"%s: %s\n",path,error);
        CHECK(result==1);count++;
        for(unsigned kind=0;kind<3;kind++) for(size_t i=0;i<package.counts[kind];i++) {
            const sh_audio_package_entry *r=&package.entries[kind][i];
            CHECK(sh_audio_package_find(&package,(sh_audio_package_kind)kind,r->id,r->language)==r);
        }
        for(size_t i=0;i<package.language_count;i++) {
            uint32_t id;CHECK(sh_audio_package_language_id(&package,package.languages[i].name,&id));
            CHECK(id==package.languages[i].id);
        }
        sh_audio_package_free(&package);fclose(file);
    } while(FindNextFileA(scan,&entry));
    CHECK(GetLastError()==ERROR_NO_MORE_FILES);FindClose(scan);return count;
}
int main(int argc,char **argv)
{
    fixture f=sample(),changed;sh_audio_package p={0};char error[256];uint32_t id;
    const sh_audio_package_entry *r;
    CHECK(sh_audio_package_read(source(&f),&p,error,sizeof(error))==1 && !error[0]);
    CHECK(f.read_bytes==168 && p.counts[0]==2 && p.counts[1]==1 && p.counts[2]==1);
    CHECK(sh_audio_package_language_id(&p,L"EnGLish",&id) && id==1);
    CHECK(sh_audio_package_language_id(&p,L"sfx",&id) && id==0);
    CHECK(!sh_audio_package_language_id(&p,L"french",&id) && id==0);
    CHECK(!sh_audio_package_language_id(NULL,L"english",&id) && id==0);
    CHECK(!sh_audio_package_language_id(&p,NULL,&id));
    CHECK(!sh_audio_package_language_id(&p,L"english",NULL));
    r=sh_audio_package_find(&p,SH_AUDIO_PACKAGE_BANK,7,0);CHECK(r && r->offset==512 && r->length==3);
    r=sh_audio_package_find(&p,SH_AUDIO_PACKAGE_BANK,7,1);CHECK(r && r->offset==768 && r->length==4);
    CHECK(!sh_audio_package_find(&p,SH_AUDIO_PACKAGE_BANK,7,2));
    CHECK(!sh_audio_package_find(&p,SH_AUDIO_PACKAGE_MEDIA,7,1));
    r=sh_audio_package_find(&p,SH_AUDIO_PACKAGE_MEDIA,99,1);
    CHECK(r && r->offset==UINT64_C(8589934590) && r->length==5);
    r=sh_audio_package_find(&p,SH_AUDIO_PACKAGE_EXTERNAL,UINT64_C(0x100000007),1);
    CHECK(r && r->offset==1280 && r->length==6 && r->block==256);
    CHECK(!sh_audio_package_find(&p,SH_AUDIO_PACKAGE_EXTERNAL,7,1));
    CHECK(!sh_audio_package_find(&p,SH_AUDIO_PACKAGE_BANK,UINT64_C(0x100000007),1));
    CHECK(!sh_audio_package_find(&p,(sh_audio_package_kind)-1,7,1));
    CHECK(!sh_audio_package_find(&p,(sh_audio_package_kind)3,7,1));
    memset(f.bytes,0,sizeof(f.bytes));
    CHECK(sh_audio_package_language_id(&p,L"english",&id) && id==1);
    CHECK(sh_audio_package_find(&p,SH_AUDIO_PACKAGE_BANK,7,0)->offset==512);
    sh_audio_package_free(&p);sh_audio_package_free(&p);f=sample();

    /* Corrupt each structural field independently. Rejection owns no prefix. */
    const struct {size_t offset;uint32_t value;} mutations[]={
        {0,0},{4,0},{4,UINT32_MAX},{8,0},{12,UINT32_MAX},{16,3},{20,3},{24,3},
        {28,0},{28,UINT32_MAX},{32,19},{32,44},{36,65536},{40,20},
        {72,UINT32_MAX},{96,6},{112,0},{80,0},{88,0},{136,65536},
        {120+12,UINT32_MAX},{140,UINT32_MAX},{144+8,0}
    };
    for(size_t i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++) {
        changed=f;put(changed.bytes+mutations[i].offset,mutations[i].value);invalid(&changed);
    }
    changed=f;changed.bytes[70]='x';changed.bytes[71]='x';invalid(&changed);
    changed=f;put(changed.bytes+8,2);
    CHECK(sh_audio_package_read(source(&changed),&p,error,sizeof(error))==1);sh_audio_package_free(&p);
    changed=f;changed.fail=1;
    CHECK(sh_audio_package_read(source(&changed),&p,error,sizeof(error))==-1 && !p.languages);
    changed=f;changed.length=100;
    CHECK(sh_audio_package_read(source(&changed),&p,error,sizeof(error))==-1 && !p.languages);
    sh_audio_bank_source short_file=source(&f);short_file.length=167;
    CHECK(sh_audio_package_read(short_file,&p,error,sizeof(error))==0 && !p.entries[0]);
    short_file.length=20;CHECK(sh_audio_package_read(short_file,&p,error,sizeof(error))==0);
    short_file.read=NULL;CHECK(sh_audio_package_read(short_file,&p,error,sizeof(error))==-1);
    CHECK(sh_audio_package_read(source(&f),NULL,NULL,0)==-1);
    if(argc==2)printf("installed PCK metadata files checked: %zu\n",audit_directory(argv[1]));
    if(failures)return 1;puts("audio package metadata checks passed");return 0;
}
