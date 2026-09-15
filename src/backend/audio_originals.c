#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "audio_originals.h"
#include "audio_packages.h"
#pragma comment(lib,"bcrypt.lib")

typedef struct ao_file {
    char *path;
    HANDLE handle;
    uint64_t length;
    sh_audio_package package;
    int packed;
} ao_file;
typedef struct ao_answer { char *path; sh_package_original_identity original; } ao_answer;
struct sh_audio_originals {
    char *root, *language;
    ao_file **files;
    size_t count;
    ao_answer *answers;
    size_t answer_count;
    SRWLOCK lock;
    int scanned, scan_failed;
};

static char *ao_utf8(const wchar_t *text)
{
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,NULL,0,NULL,NULL);
    char *out=size ? malloc((size_t)size) : NULL;
    if(out && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,out,size,NULL,NULL)){free(out);out=NULL;}
    return out;
}
static char *ao_join(const char *a,const char *b)
{
    size_t x=strlen(a),y=strlen(b);char *out;
    if(x>SIZE_MAX-y-2)return NULL;
    out=malloc(x+y+2);if(!out)return NULL;
    memcpy(out,a,x);out[x]='/';memcpy(out+x+1,b,y+1);return out;
}
static int ao_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return -1; }
static void ao_file_free(ao_file *file)
{
    if(!file)return;
    if(file->handle!=INVALID_HANDLE_VALUE)CloseHandle(file->handle);
    sh_audio_package_free(&file->package);free(file->path);free(file);
}
void sh_audio_originals_close(sh_audio_originals *originals)
{
    if(!originals)return;
    for(size_t i=0;i<originals->count;i++)ao_file_free(originals->files[i]);
    for(size_t i=0;i<originals->answer_count;i++)free(originals->answers[i].path);
    free(originals->files);free(originals->answers);free(originals->root);free(originals->language);free(originals);
}
sh_audio_originals *sh_audio_originals_open(const char *doom_base,const wchar_t *language)
{
    sh_audio_originals *out;
    if(!doom_base || !*doom_base)return NULL;
    out=calloc(1,sizeof(*out));if(!out)return NULL;
    InitializeSRWLock(&out->lock);
    out->root=ao_join(doom_base,"sound/soundbanks/pc");out->language=language ? ao_utf8(language) : NULL;
    if(!out->root || (language && !out->language)){sh_audio_originals_close(out);return NULL;}
    if(out->language)for(char *p=out->language;*p;p++)if(*p>='A' && *p<='Z')*p+='a'-'A';
    return out;
}
static int ao_read(void *context,uint64_t offset,void *out,size_t length)
{
    ao_file *file=context;LARGE_INTEGER position;unsigned char *p=out;
    if(offset>file->length || length>file->length-offset || offset>INT64_MAX)return 0;
    position.QuadPart=(LONGLONG)offset;
    if(!SetFilePointerEx(file->handle,position,NULL,FILE_BEGIN))return 0;
    while(length) {
        DWORD amount=length>1048576 ? 1048576 : (DWORD)length,got=0;
        if(!ReadFile(file->handle,p,amount,&got,NULL) || got!=amount)return 0;
        length-=got;p+=got;
    }
    return 1;
}
/* Returns absent separately from unreadable; every acquired handle denies
 * writes/deletion. All file positions and lazy caches are under the owner lock. */
static int ao_open(sh_audio_originals *originals,const char *path,ao_file **out)
{
    char *absolute;wchar_t *wide;ao_file *file;ao_file **grown;
    BY_HANDLE_FILE_INFORMATION info;LARGE_INTEGER length;
    *out=NULL;
    for(size_t i=0;i<originals->count;i++)if(!strcmp(originals->files[i]->path,path)){*out=originals->files[i];return 1;}
    absolute=ao_join(originals->root,path);if(!absolute)return -1;
    wide=sh_package_source_wide_path(absolute);free(absolute);if(!wide)return -1;
    file=calloc(1,sizeof(*file));if(!file){free(wide);return -1;}
    file->handle=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_RANDOM_ACCESS,NULL);
    DWORD open_error=GetLastError();free(wide);
    if(file->handle==INVALID_HANDLE_VALUE) {
        free(file);return open_error==ERROR_FILE_NOT_FOUND || open_error==ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    if(!GetFileInformationByHandle(file->handle,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file->handle,&length) || length.QuadPart<0)goto failed;
    file->length=(uint64_t)length.QuadPart;file->path=_strdup(path);
    if(!file->path || originals->count>=SIZE_MAX/sizeof(*grown)-1)goto failed;
    grown=realloc(originals->files,(originals->count+1)*sizeof(*grown));if(!grown)goto failed;
    originals->files=grown;grown[originals->count++]=file;*out=file;return 1;
failed:
    ao_file_free(file);return -1;
}
static int ao_pck_mounted(const char *path)
{
    /* Initial is mounted explicitly. The enumeration skips all other names
     * containing initial, in both native modes and both renderers. */
    return !strcmp(path,"initial.pck") || !strstr(path,"initial");
}
static int ao_scan(sh_audio_originals *originals,const char *relative,int descend)
{
    char *directory=relative[0] ? ao_join(originals->root,relative) : _strdup(originals->root);
    char *pattern=directory ? ao_join(directory,"*") : NULL;
    wchar_t *wide=pattern ? sh_package_source_wide_path(pattern) : NULL;
    WIN32_FIND_DATAW found;HANDLE search;int result=1;
    free(directory);free(pattern);if(!wide)return -1;
    search=FindFirstFileW(wide,&found);DWORD search_error=GetLastError();free(wide);
    if(search==INVALID_HANDLE_VALUE)return search_error==ERROR_FILE_NOT_FOUND || search_error==ERROR_PATH_NOT_FOUND ? 1 : -1;
    do {
        char *name,*joined,*path;
        if(!wcscmp(found.cFileName,L".") || !wcscmp(found.cFileName,L".."))continue;
        name=ao_utf8(found.cFileName);if(!name){result=-1;break;}
        joined=relative[0] ? ao_join(relative,name) : _strdup(name);free(name);
        path=joined ? sh_package_engine_path(joined) : NULL;free(joined);
        if(!path){result=-1;break;}
        const char *dot=strrchr(path,'.');
        int is_directory=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        int is_package=dot && !strcmp(dot,".pck") && ao_pck_mounted(path);
        if((found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
            ((is_directory && descend) || (!is_directory && is_package))) { free(path);result=-1;break; }
        if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
            if(descend)result=ao_scan(originals,path,0);
        } else {
            if(is_package) {
                ao_file *file=NULL;result=ao_open(originals,path,&file);
                if(result==1 && !file->packed) {
                    sh_audio_bank_source source={file,file->length,ao_read};
                    result=sh_audio_package_read(source,&file->package,NULL,0);
                    if(result==1)file->packed=1;
                }
                if(result!=1)result=-1;
            }
        }
        free(path);if(result!=1)break;
    } while(FindNextFileW(search,&found));
    if(result==1 && GetLastError()!=ERROR_NO_MORE_FILES)result=-1;
    FindClose(search);return result;
}
static int ao_digest(ao_file *file,uint64_t offset,uint64_t length,sh_package_file_identity *out)
{
    BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;unsigned char bytes[65536];
    uint64_t position=0;int ok=0;
    memset(out,0,sizeof(*out));
    if(offset>file->length || length>file->length-offset)return 0;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0 ||
        BCryptCreateHash(algorithm,&hash,NULL,0,NULL,0,0)<0)goto done;
    while(position<length) {
        size_t count=length-position>sizeof(bytes) ? sizeof(bytes) : (size_t)(length-position);
        if(!ao_read(file,offset+position,bytes,count) || BCryptHashData(hash,bytes,(ULONG)count,0)<0)goto done;
        position+=count;
    }
    if(BCryptFinishHash(hash,out->digest,sizeof(out->digest),0)<0)goto done;
    out->length=length;ok=1;
done:
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok)memset(out,0,sizeof(*out));return ok;
}
static int ao_numeric(const char *name,size_t length,uint32_t *id)
{
    uint64_t value=0;
    if(!length)return 0;
    for(size_t i=0;i<length;i++) {
        if(name[i]<'0' || name[i]>'9')return 0;
        value=value*10+(unsigned)(name[i]-'0');if(value>UINT32_MAX)return 0;
    }
    *id=(uint32_t)value;return 1;
}
/* Native external-source names retain the extension and path. Conversion uses
 * the Windows ANSI code page and the native wchar-count-plus-one byte capacity.
 * A conversion that cannot fit yields the native empty-name hash. */
static int ao_external_id(const char *name,uint64_t *id)
{
    int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,NULL,0);
    wchar_t *wide=units ? malloc((size_t)units*sizeof(*wide)) : NULL;
    char *bytes=units ? malloc((size_t)units) : NULL;
    uint64_t hash=UINT64_C(0xcbf29ce484222325);
    if(!wide || !bytes || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,wide,units)) {
        free(wide);free(bytes);return 0;
    }
    int count=WideCharToMultiByte(CP_ACP,0,wide,units-1,bytes,units,NULL,NULL);
    for(int i=0;i<count;i++) {
        unsigned char c=(unsigned char)bytes[i];
        if(c>='A' && c<='Z')c+='a'-'A';
        hash=hash*UINT64_C(0x100000001b3)^c;
    }
    free(wide);free(bytes);*id=hash;return 1;
}
typedef struct ao_span { ao_file *file; uint64_t offset,length; } ao_span;
/* Locate the packed original, if any. Equal candidates coalesce and differing
 * candidates refuse; identity is filled only when the caller needs a digest or
 * a second candidate forces the comparison. */
static int ao_packed(sh_audio_originals *originals,const char *path,const char *name,
    const char *extension,sh_package_file_identity *identity,ao_span *span,int want_digest)
{
    uint64_t ids[3];sh_audio_package_kind kinds[3];size_t count=0;int present=0,hashed=0;
    char *locale=NULL;wchar_t *wide_locale=NULL;
    size_t prefix=(size_t)(name-path);
    if(prefix) {
        locale=malloc(prefix);if(!locale)return -1;
        memcpy(locale,path,prefix-1);locale[prefix-1]=0;
        /* The native startup enumerates one language directory, not arbitrary
         * deeper bank paths. Those can still have a loose original below. */
        if(strchr(locale,'/')){free(locale);return 0;}
        int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,locale,-1,NULL,0);
        wide_locale=units ? malloc((size_t)units*sizeof(wchar_t)) : NULL;
        if(!wide_locale || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,locale,-1,wide_locale,units)){free(locale);free(wide_locale);return -1;}
    }
    uint32_t named;
    if(!strcmp(extension,".bnk") && sh_audio_bank_identity(name,&named)) {
        ids[count]=named;kinds[count++]=SH_AUDIO_PACKAGE_BANK;
    }
    uint32_t numeric;
    if(ao_numeric(name,(size_t)(extension-name),&numeric) && (!count || ids[0]!=numeric)) {
        ids[count]=numeric;kinds[count++]=!strcmp(extension,".bnk") ? SH_AUDIO_PACKAGE_BANK : SH_AUDIO_PACKAGE_MEDIA;
    }
    if(!ao_external_id(name,&ids[count])){free(locale);free(wide_locale);return -1;}
    kinds[count++]=SH_AUDIO_PACKAGE_EXTERNAL;
    for(size_t i=0;i<originals->count;i++) {
        ao_file *file=originals->files[i];const char *slash=strchr(file->path,'/');uint32_t language=0;
        if(!file->packed)continue;
        const char *selected=locale ? locale : originals->language;
        if(slash && ((size_t)(slash-file->path)!=strlen(selected) || strncmp(file->path,selected,(size_t)(slash-file->path))))continue;
        if(locale && !sh_audio_package_language_id(&file->package,wide_locale,&language))continue;
        for(size_t j=0;j<count;j++) {
            const sh_audio_package_entry *row=sh_audio_package_find(&file->package,kinds[j],ids[j],language);
            sh_package_file_identity candidate;
            if(!row)continue;
            if(!present) { span->file=file;span->offset=row->offset;span->length=row->length;present=1;continue; }
            if(!hashed) {
                if(!ao_digest(span->file,span->offset,span->length,identity)){present=-1;goto done;}
                hashed=1;
            }
            if(!ao_digest(file,row->offset,row->length,&candidate)){present=-1;goto done;}
            if(identity->length!=candidate.length || memcmp(identity->digest,candidate.digest,32)){present=-1;goto done;}
        }
    }
    if(present==1 && want_digest && !hashed &&
        !ao_digest(span->file,span->offset,span->length,identity))present=-1;
done:
    free(locale);free(wide_locale);return present;
}
int sh_audio_originals_identity(void *context,const char *path,sh_package_original_identity *out,
    char *error,size_t capacity)
{
    static const char prefix[]="sound/soundbanks/pc/";
    sh_audio_originals *originals=context;char *canonical;const char *relative,*name,*extension;
    sh_package_original_identity candidate={0};int result=1,present;ao_answer *grown;ao_span span={0};
    if(!out)return ao_fail(error,capacity,"missing installed audio identity output");
    memset(out,0,sizeof(*out));
    if(error && capacity)error[0]=0;
    if(!originals || !path)return 0;
    canonical=sh_package_engine_path(path);if(!canonical)return ao_fail(error,capacity,"invalid installed audio path");
    if(strncmp(canonical,prefix,sizeof(prefix)-1)){free(canonical);return 0;}
    relative=canonical+sizeof(prefix)-1;name=strrchr(relative,'/');name=name ? name+1 : relative;
    extension=strrchr(name,'.');
    if(!extension || (strcmp(extension,".bnk") && strcmp(extension,".wem") && strcmp(extension,".pck"))){free(canonical);return 0;}
    if(!originals->language){free(canonical);return ao_fail(error,capacity,"native audio language is still initializing");}
    AcquireSRWLockExclusive(&originals->lock);
    for(size_t i=0;i<originals->answer_count;i++)if(!strcmp(originals->answers[i].path,canonical)) {
        *out=originals->answers[i].original;goto done;
    }
    if(!originals->scanned) { originals->scanned=1;originals->scan_failed=ao_scan(originals,"",1)!=1; }
    if(originals->scan_failed){result=ao_fail(error,capacity,"installed audio package metadata is unreadable or invalid");goto done;}
    present=!strcmp(extension,".pck") ? 0 :
        ao_packed(originals,relative,name,extension,&candidate.file,&span,1);
    if(present<0){result=ao_fail(error,capacity,"installed packed audio is unreadable or ambiguous");goto done;}
    if(!present) {
        ao_file *file=NULL;present=ao_open(originals,relative,&file);
        if(present<0 || (present && !ao_digest(file,0,file->length,&candidate.file))) {
            result=ao_fail(error,capacity,"installed loose audio is unreadable");goto done;
        }
    }
    if(present) {
        candidate.scope=1;
        size_t locale_length=(size_t)(name-relative);
        if(locale_length && (locale_length-1!=strlen(originals->language) ||
            strncmp(relative,originals->language,locale_length-1)))candidate.scope=2;
        if(!strcmp(extension,".bnk") && (strstr(name,"_sp") ||
            (!strcmp(name,"init.bnk") && strcmp(relative,"init.bnk")) ||
            (strstr(name,"initial") && strcmp(relative,"doom_initial.bnk"))))candidate.scope=2;
        if(!strcmp(extension,".bnk") && candidate.scope==1 &&
            strcmp(relative,"init.bnk") && strcmp(relative,"doom_initial.bnk")) {
            /* Startup requests banks by enumerating loose bank filenames.
             * A bank found only inside a mounted PCK is available to install
             * checks but is not proof that SnapMap already loaded that bank. */
            ao_file *enumerated=NULL;int listed=ao_open(originals,relative,&enumerated);
            if(listed<0){result=ao_fail(error,capacity,"installed bank enumeration is unreadable");goto done;}
            if(!listed)candidate.scope=2;
        }
        if(!strcmp(extension,".pck") && !ao_pck_mounted(relative))candidate.scope=2;
    }
    if(originals->answer_count>=SIZE_MAX/sizeof(*grown)-1 ||
        !(grown=realloc(originals->answers,(originals->answer_count+1)*sizeof(*grown)))) {
        result=ao_fail(error,capacity,"cannot retain installed audio identity");goto done;
    }
    originals->answers=grown;grown[originals->answer_count++]=(ao_answer){canonical,candidate};canonical=NULL;*out=candidate;
done:
    ReleaseSRWLockExclusive(&originals->lock);free(canonical);return result;
}

int sh_audio_originals_read(void *context,const char *path,uint64_t offset,void *out,size_t span,
    uint64_t *length,char *error,size_t capacity)
{
    static const char prefix[]="sound/soundbanks/pc/";
    sh_audio_originals *originals=context;char *canonical;const char *relative,*name,*extension;
    sh_package_file_identity identity={0};ao_span located={0};int result=0,present;
    if(length)*length=0;
    if(error && capacity)error[0]=0;
    if(!originals || !path || (span && !out))return 0;
    canonical=sh_package_engine_path(path);if(!canonical)return ao_fail(error,capacity,"invalid installed audio path");
    if(strncmp(canonical,prefix,sizeof(prefix)-1)){free(canonical);return 0;}
    relative=canonical+sizeof(prefix)-1;name=strrchr(relative,'/');name=name ? name+1 : relative;
    extension=strrchr(name,'.');
    if(!extension || (strcmp(extension,".bnk") && strcmp(extension,".wem"))){free(canonical);return 0;}
    if(!originals->language){free(canonical);return ao_fail(error,capacity,"native audio language is still initializing");}
    AcquireSRWLockExclusive(&originals->lock);
    if(!originals->scanned) { originals->scanned=1;originals->scan_failed=ao_scan(originals,"",1)!=1; }
    if(originals->scan_failed){result=ao_fail(error,capacity,"installed audio package metadata is unreadable or invalid");goto done;}
    present=ao_packed(originals,relative,name,extension,&identity,&located,0);
    if(present<0){result=ao_fail(error,capacity,"installed packed audio is unreadable or ambiguous");goto done;}
    if(!present) {
        ao_file *file=NULL;present=ao_open(originals,relative,&file);
        if(present<0){result=ao_fail(error,capacity,"installed loose audio is unreadable");goto done;}
        if(present){located.file=file;located.offset=0;located.length=file->length;}
    }
    if(!present)goto done;
    if(length)*length=located.length;
    if(offset>located.length || span>located.length-offset) {
        result=ao_fail(error,capacity,"installed audio original read is out of range");goto done;
    }
    if(span && !ao_read(located.file,located.offset+offset,out,span)) {
        result=ao_fail(error,capacity,"installed audio original is unreadable");goto done;
    }
    result=1;
done:
    ReleaseSRWLockExclusive(&originals->lock);free(canonical);return result;
}

int sh_audio_originals_packaged_banks(void *context,sh_audio_originals_bank_visit visit,
    void *visitor,char *error,size_t capacity)
{
    sh_audio_originals *originals=context;int result=1;
    if(error && capacity)error[0]=0;
    if(!originals || !visit)return ao_fail(error,capacity,"installed audio package index is unavailable");
    if(!originals->language)return ao_fail(error,capacity,"native audio language is still initializing");
    AcquireSRWLockExclusive(&originals->lock);
    if(!originals->scanned) { originals->scanned=1;originals->scan_failed=ao_scan(originals,"",1)!=1; }
    if(originals->scan_failed){result=ao_fail(error,capacity,"installed audio package metadata is unreadable or invalid");goto done;}
    for(size_t i=0;i<originals->count && result==1;i++) {
        ao_file *file=originals->files[i];const char *slash=strchr(file->path,'/');
        if(!file->packed)continue;
        /* A package inside a language directory serves only that language. */
        if(slash && ((size_t)(slash-file->path)!=strlen(originals->language) ||
            strncmp(file->path,originals->language,(size_t)(slash-file->path))))continue;
        for(size_t j=0;j<file->package.counts[SH_AUDIO_PACKAGE_BANK] && result==1;j++) {
            const sh_audio_package_entry *row=file->package.entries[SH_AUDIO_PACKAGE_BANK]+j;
            if(row->id>UINT32_MAX)continue;
            if(!visit(visitor,(uint32_t)row->id,row->language))result=0;
        }
    }
done:
    ReleaseSRWLockExclusive(&originals->lock);return result;
}

int sh_audio_originals_ready(void *context)
{
    sh_audio_originals *originals=context;
    return originals && originals->language != NULL;
}
