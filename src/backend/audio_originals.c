#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "audio_originals.h"
#include "audio_packages.h"
#include "audio_bank_codec.h"
#pragma comment(lib,"bcrypt.lib")

typedef struct ao_file {
    char *path;
    HANDLE handle;
    uint64_t length;
    sh_audio_package package;
    int packed;
} ao_file;
typedef struct ao_answer { char *path; sh_package_original_identity original; } ao_answer;
typedef struct ao_located { char *path; ao_file *file; uint64_t offset,length; } ao_located;
struct sh_audio_originals {
    char *root, *language;
    /* "sound/soundbanks/pc/" plus the engine's configured bank prefix. The
     * engine requests every bank and media file below it, so discovery and the
     * identity reader must use it instead of assuming the plain root. */
    char *engine_prefix;
    ao_file **files;
    size_t count;
    ao_answer *answers;
    size_t answer_count;
    /* Successful read locations, open-addressed by exact engine path. */
    ao_located *located;
    size_t located_count,located_capacity;
    SRWLOCK lock;
    int scanned, scan_failed;
    char **bank_names;
    size_t bank_name_count;
    /* Root and current-language bank paths in the directory's own listing
     * order, which is the order the engine's unsorted ListFiles returns. */
    char **listed;
    size_t listed_count;
    SRWLOCK catalog_lock;
    sh_audio_original_bank *banks;
    size_t bank_count;
    int catalog_ready;
    uint32_t *ids, *loose_media;
    size_t id_count, loose_media_count, loose_media_capacity;
    int ids_ready;
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
    for(size_t i=0;i<originals->located_capacity;i++)free(originals->located[i].path);
    free(originals->located);
    for(size_t i=0;i<originals->bank_name_count;i++)free(originals->bank_names[i]);
    for(size_t i=0;i<originals->listed_count;i++)free(originals->listed[i]);
    free(originals->listed);
    for(size_t i=0;i<originals->bank_count;i++) {
        free((char *)originals->banks[i].path);sh_audio_bank_free(&originals->banks[i].metadata);
    }
    free(originals->bank_names);free(originals->banks);
    free(originals->ids);free(originals->loose_media);
    free(originals->files);free(originals->answers);free(originals->root);free(originals->language);
    free(originals->engine_prefix);free(originals);
}
sh_audio_originals *sh_audio_originals_open(const char *doom_base,const wchar_t *language,
    const char *bank_prefix)
{
    sh_audio_originals *out;size_t length;
    if(!doom_base || !*doom_base)return NULL;
    out=calloc(1,sizeof(*out));if(!out)return NULL;
    InitializeSRWLock(&out->lock);
    InitializeSRWLock(&out->catalog_lock);
    /* The native prefix is already lowercased with forward slashes and, when
     * present, ends in a separator. An empty prefix is the ordinary root. */
    length=sizeof("sound/soundbanks/pc/")+(bank_prefix ? strlen(bank_prefix) : 0);
    out->engine_prefix=malloc(length);
    if(out->engine_prefix)snprintf(out->engine_prefix,length,"sound/soundbanks/pc/%s",
        bank_prefix ? bank_prefix : "");
    out->root=out->engine_prefix ? ao_join(doom_base,out->engine_prefix) : NULL;
    out->language=language ? ao_utf8(language) : NULL;
    if(!out->root || !out->engine_prefix || (language && !out->language)){sh_audio_originals_close(out);return NULL;}
    if(out->root[0]){size_t used=strlen(out->root);while(used>1 && (out->root[used-1]=='/' || out->root[used-1]=='\\'))out->root[--used]=0;}
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
static int ao_bank_name(sh_audio_originals *originals,const char *name)
{
    for(size_t i=0;i<originals->bank_name_count;i++)
        if(!strcmp(originals->bank_names[i],name))return 1;
    if(originals->bank_name_count>=SIZE_MAX/sizeof(char *)-1)return 0;
    char **grown=realloc(originals->bank_names,(originals->bank_name_count+1)*sizeof(*grown));
    if(!grown)return 0;
    originals->bank_names=grown;grown[originals->bank_name_count]=_strdup(name);
    if(!grown[originals->bank_name_count])return 0;
    originals->bank_name_count++;return 1;
}
static int ao_listed_add(sh_audio_originals *originals,const char *path)
{
    if(originals->listed_count>=SIZE_MAX/sizeof(char *)-1)return 0;
    char **grown=realloc(originals->listed,(originals->listed_count+1)*sizeof(*grown));
    if(!grown)return 0;
    originals->listed=grown;grown[originals->listed_count]=_strdup(path);
    if(!grown[originals->listed_count])return 0;
    originals->listed_count++;return 1;
}
static int ao_id_append(uint32_t **ids,size_t *count,size_t *capacity,uint32_t id)
{
    if(!id)return 1;
    if(*count==*capacity) {
        size_t next=*capacity ? *capacity*2 : 256;
        if(next<=*capacity || next>SIZE_MAX/sizeof(**ids))return 0;
        uint32_t *grown=realloc(*ids,next*sizeof(**ids));if(!grown)return 0;
        *ids=grown;*capacity=next;
    }
    (*ids)[(*count)++]=id;return 1;
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
            if(dot && !strcmp(dot,".wem") &&
                (!*relative || (originals->language && !strcmp(relative,originals->language)))) {
                const char *filename=strrchr(path,'/');uint32_t id=0;
                filename=filename ? filename+1 : path;
                if(sh_audio_media_identity(filename,&id) &&
                    !ao_id_append(&originals->loose_media,&originals->loose_media_count,
                        &originals->loose_media_capacity,id))result=-1;
            }
            if(dot && !strcmp(dot,".bnk") &&
                (!*relative || (originals->language && !strcmp(relative,originals->language)))) {
                const char *filename=strrchr(path,'/');filename=filename ? filename+1 : path;
                if((found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !ao_bank_name(originals,filename) ||
                    !ao_listed_add(originals,path))result=-1;
            }
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
    /* Native mount order is initial.pck, then every .pck the platform directory
     * lists, then every .pck the language directory lists; each load is pushed
     * on the front of the package list and every lookup walks that list from its
     * head. A later mount stage therefore answers first. The engine lists a
     * directory unsorted, so two differing payloads inside one stage have no
     * native winner and stay ambiguous. */
    int stage_best=0;
    for(size_t i=0;i<originals->count;i++) {
        ao_file *file=originals->files[i];const char *slash=strchr(file->path,'/');uint32_t language=0;
        if(!file->packed)continue;
        const char *selected=locale ? locale : originals->language;
        if(slash && ((size_t)(slash-file->path)!=strlen(selected) || strncmp(file->path,selected,(size_t)(slash-file->path))))continue;
        if(locale && !sh_audio_package_language_id(&file->package,wide_locale,&language))continue;
        int stage=slash ? 2 : 1;
        for(size_t j=0;j<count;j++) {
            const sh_audio_package_entry *row=sh_audio_package_find(&file->package,kinds[j],ids[j],language);
            sh_package_file_identity candidate;
            if(!row)continue;
            if(!present) { span->file=file;span->offset=row->offset;span->length=row->length;present=1;stage_best=stage;continue; }
            if(stage!=stage_best) {
                if(stage>stage_best) {
                    span->file=file;span->offset=row->offset;span->length=row->length;
                    stage_best=stage;hashed=0;
                }
                continue;
            }
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
    sh_audio_originals *originals=context;char *canonical;const char *relative,*name,*extension;
    const char *prefix;size_t prefix_length;
    sh_package_original_identity candidate={0};int result=1,present;ao_answer *grown;ao_span span={0};
    if(!out)return ao_fail(error,capacity,"missing installed audio identity output");
    memset(out,0,sizeof(*out));
    if(error && capacity)error[0]=0;
    if(!originals || !path)return 0;
    canonical=sh_package_engine_path(path);if(!canonical)return ao_fail(error,capacity,"invalid installed audio path");
    prefix=originals->engine_prefix;prefix_length=prefix ? strlen(prefix) : 0;
    if(!prefix_length || strncmp(canonical,prefix,prefix_length)){free(canonical);return 0;}
    relative=canonical+prefix_length;name=strrchr(relative,'/');name=name ? name+1 : relative;
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

/* Sound init (VK FUN_1402b97d0, GL FUN_1402b87c0) loads Init.bnk, prepares
 * doom_initial.bnk, then requests every root bank the file system lists and
 * then every language bank, skipping names containing "initial" and, in
 * SnapMap mode, "_sp" (VK FUN_1402babc0, GL FUN_1402b9bb0). Its ListFiles
 * (VK FUN_141a56700, GL FUN_141a48be0) is unsorted: each search directory is
 * read with _findfirst64/_findnext64 (VK FUN_141a48280, GL FUN_141a3a7e0) and
 * kept in that order, so the position here is this directory's own listing
 * order, not a name order. The file reader serves a requested name from the
 * language directory first; a language file whose name the root also lists is
 * therefore registered by the root pass at that root position. */
static size_t ao_listed_position(sh_audio_originals *originals,const char *relative)
{
    size_t position=SIZE_MAX;
    AcquireSRWLockExclusive(&originals->lock);
    for(size_t i=0;i<originals->listed_count;i++)if(!strcmp(originals->listed[i],relative)){position=i;break;}
    ReleaseSRWLockExclusive(&originals->lock);
    return position;
}
int sh_audio_originals_registration(sh_audio_originals *originals,const char *path,
    int authored,int authored_root,sh_audio_native_rank *rank,char *error,size_t capacity)
{
    sh_package_original_identity identity={0};
    char *canonical,*root_path=NULL;const char *relative,*name,*extension;int result=0,present,root;
    size_t prefix;
    if(rank)*rank=(sh_audio_native_rank){0,SIZE_MAX};
    if(error && capacity)*error=0;
    if(!originals || !path || !rank)return ao_fail(error,capacity,"invalid native bank registration query");
    if(!originals->language)return ao_fail(error,capacity,"native audio language is still initializing");
    canonical=sh_package_engine_path(path);if(!canonical)return ao_fail(error,capacity,"invalid installed audio path");
    prefix=strlen(originals->engine_prefix);
    if(strncmp(canonical,originals->engine_prefix,prefix))goto done;
    relative=canonical+prefix;name=strrchr(relative,'/');name=name ? name+1 : relative;
    extension=strrchr(name,'.');
    if(!extension || strcmp(extension,".bnk"))goto done;
    root=name==relative;
    if(!root && ((size_t)(name-relative)-1!=strlen(originals->language) ||
        strncmp(relative,originals->language,(size_t)(name-relative)-1)))goto done;
    if(root && !strcmp(name,"init.bnk"))rank->phase=0;
    else if(root && !strcmp(name,"doom_initial.bnk"))rank->phase=1;
    else if(strstr(name,"initial") || !strcmp(name,"init.bnk") || strstr(name,"_sp"))goto done;
    else rank->phase=root ? 2 : 3;
    /* An installed bank must also be a loose file that the enumeration lists. */
    present=sh_audio_originals_identity(originals,canonical,&identity,error,capacity);
    if(present<0){result=-1;goto done;}
    if(!authored && (!present || identity.scope!=1))goto done;
    result=1;
    if(rank->phase>=2)rank->position=ao_listed_position(originals,relative);
    if(rank->phase==3) {
        size_t length=strlen(name),listed;
        if(prefix>SIZE_MAX-length-1 || !(root_path=malloc(prefix+length+1))){result=ao_fail(error,capacity,"cannot inspect a root audio bank");goto done;}
        memcpy(root_path,originals->engine_prefix,prefix);memcpy(root_path+prefix,name,length+1);
        present=sh_audio_originals_identity(originals,root_path,&identity,error,capacity);
        if(present<0){result=-1;goto done;}
        listed=present && identity.scope==1 ? ao_listed_position(originals,name) : SIZE_MAX;
        if(listed!=SIZE_MAX || authored_root){rank->phase=2;rank->position=listed;}
    }
done:
    free(root_path);free(canonical);return result;
}

#ifdef SH_AUDIO_ORIGINALS_TESTING
int sh_audio_originals_test_refuse_locations;
size_t sh_audio_originals_test_located(const sh_audio_originals *originals){return originals->located_count;}
#define AO_LOCATED_ROWS(count) (sh_audio_originals_test_refuse_locations ? NULL : calloc((count),sizeof(ao_located)))
#else
#define AO_LOCATED_ROWS(count) calloc((count),sizeof(ao_located))
#endif
static size_t ao_located_slot(const ao_located *rows,size_t capacity,const char *path)
{
    uint32_t hash=2166136261u;
    for(const unsigned char *at=(const unsigned char *)path;*at;at++)hash=(hash^*at)*16777619u;
    size_t slot=hash&(capacity-1);
    while(rows[slot].path && strcmp(rows[slot].path,path))slot=(slot+1)&(capacity-1);
    return slot;
}
/* A located original stays this exact path's answer until close: packages are
 * indexed once by the scan and held without write sharing, a loose file stays
 * open without write or delete sharing, and packages answer before loose
 * files. Absence, failure and ambiguity are never kept, so an absent path is
 * probed again and a newly created file is found. Keeping is only a saving;
 * when memory is refused the path is located again on the next request. */
static int ao_located_keep(sh_audio_originals *originals,char *path,const ao_span *span)
{
    if(originals->located_count>=originals->located_capacity/2) {
        size_t capacity=originals->located_capacity ? originals->located_capacity*2 : 256;
        ao_located *rows=capacity>originals->located_capacity ? AO_LOCATED_ROWS(capacity) : NULL;
        if(!rows)return 0;
        for(size_t i=0;i<originals->located_capacity;i++)if(originals->located[i].path)
            rows[ao_located_slot(rows,capacity,originals->located[i].path)]=originals->located[i];
        free(originals->located);originals->located=rows;originals->located_capacity=capacity;
    }
    originals->located[ao_located_slot(originals->located,originals->located_capacity,path)]=
        (ao_located){path,span->file,span->offset,span->length};
    originals->located_count++;return 1;
}
int sh_audio_originals_read(void *context,const char *path,uint64_t offset,void *out,size_t span,
    uint64_t *length,char *error,size_t capacity)
{
    const char *prefix;size_t prefix_length;
    sh_audio_originals *originals=context;char *canonical;const char *relative,*name,*extension;
    sh_package_file_identity identity={0};ao_span located={0};int result=0,present;
    if(length)*length=0;
    if(error && capacity)error[0]=0;
    if(!originals || !path || (span && !out))return 0;
    canonical=sh_package_engine_path(path);if(!canonical)return ao_fail(error,capacity,"invalid installed audio path");
    prefix=originals->engine_prefix;prefix_length=prefix ? strlen(prefix) : 0;
    if(!prefix_length || strncmp(canonical,prefix,prefix_length)){free(canonical);return 0;}
    relative=canonical+prefix_length;name=strrchr(relative,'/');name=name ? name+1 : relative;
    extension=strrchr(name,'.');
    if(!extension || (strcmp(extension,".bnk") && strcmp(extension,".wem"))){free(canonical);return 0;}
    if(!originals->language){free(canonical);return ao_fail(error,capacity,"native audio language is still initializing");}
    AcquireSRWLockExclusive(&originals->lock);
    if(!originals->scanned) { originals->scanned=1;originals->scan_failed=ao_scan(originals,"",1)!=1; }
    if(originals->scan_failed){result=ao_fail(error,capacity,"installed audio package metadata is unreadable or invalid");goto done;}
    if(originals->located_count) {
        const ao_located *known=originals->located+ao_located_slot(originals->located,originals->located_capacity,canonical);
        if(known->path){located.file=known->file;located.offset=known->offset;located.length=known->length;goto found;}
    }
    present=ao_packed(originals,relative,name,extension,&identity,&located,0);
    if(present<0){result=ao_fail(error,capacity,"installed packed audio is unreadable or ambiguous");goto done;}
    if(!present) {
        ao_file *file=NULL;present=ao_open(originals,relative,&file);
        if(present<0){result=ao_fail(error,capacity,"installed loose audio is unreadable");goto done;}
        if(present){located.file=file;located.offset=0;located.length=file->length;}
    }
    if(!present)goto done;
    if(ao_located_keep(originals,canonical,&located))canonical=NULL;
found:
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
    sh_audio_originals *originals=context;int result=1;wchar_t *language=NULL;
    if(error && capacity)error[0]=0;
    if(!originals || !visit)return ao_fail(error,capacity,"installed audio package index is unavailable");
    if(!originals->language)return ao_fail(error,capacity,"native audio language is still initializing");
    int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,originals->language,-1,NULL,0);
    language=units ? malloc((size_t)units*sizeof(*language)) : NULL;
    if(!language || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,originals->language,-1,language,units)) {
        free(language);return ao_fail(error,capacity,"cannot resolve captured audio language");
    }
    AcquireSRWLockExclusive(&originals->lock);
    if(!originals->scanned) { originals->scanned=1;originals->scan_failed=ao_scan(originals,"",1)!=1; }
    if(originals->scan_failed){result=ao_fail(error,capacity,"installed audio package metadata is unreadable or invalid");goto done;}
    for(size_t i=0;i<originals->count && result==1;i++) {
        ao_file *file=originals->files[i];const char *slash=strchr(file->path,'/');
        if(!file->packed)continue;
        /* A package inside a language directory serves only that language. */
        if(slash && ((size_t)(slash-file->path)!=strlen(originals->language) ||
            strncmp(file->path,originals->language,(size_t)(slash-file->path))))continue;
        uint32_t language_id=0;int localized=sh_audio_package_language_id(&file->package,language,&language_id);
        for(size_t j=0;j<file->package.counts[SH_AUDIO_PACKAGE_BANK] && result==1;j++) {
            const sh_audio_package_entry *row=file->package.entries[SH_AUDIO_PACKAGE_BANK]+j;
            if(row->language && (!localized || row->language!=language_id))continue;
            if(row->id>UINT32_MAX)continue;
            if(!visit(visitor,(uint32_t)row->id,row->language))result=0;
        }
    }
done:
    ReleaseSRWLockExclusive(&originals->lock);free(language);return result;
}

int sh_audio_originals_ready(void *context)
{
    sh_audio_originals *originals=context;
    return originals && originals->language != NULL;
}

int sh_audio_originals_matches(const sh_audio_originals *originals,
    const char *language,const char *bank_prefix)
{
    return originals && originals->language && language &&
        !_stricmp(originals->language,language) &&
        !strcmp(originals->engine_prefix+strlen("sound/soundbanks/pc/"),bank_prefix ? bank_prefix : "");
}

int sh_audio_originals_paths(sh_audio_originals *originals,const char *filename,
    char **localized,char **root)
{
    if(localized)*localized=NULL;if(root)*root=NULL;
    if(!originals || !originals->language || !filename || !*filename ||
        strchr(filename,'/') || strchr(filename,'\\') || !localized || !root)return 0;
    size_t prefix=strlen(originals->engine_prefix),language=strlen(originals->language),name=strlen(filename);
    if(prefix>SIZE_MAX-language-2 || name>SIZE_MAX-prefix-language-2)return 0;
    *root=malloc(prefix+name+1);*localized=malloc(prefix+language+name+2);
    if(!*root || !*localized){free(*root);free(*localized);*root=*localized=NULL;return 0;}
    snprintf(*root,prefix+name+1,"%s%s",originals->engine_prefix,filename);
    snprintf(*localized,prefix+language+name+2,"%s%s/%s",originals->engine_prefix,originals->language,filename);
    return 1;
}
typedef struct ao_bank_reader {
    sh_audio_originals *originals;
    const char *path;
    uint64_t length,at;
    size_t held;
    unsigned char window[65536];
    char *error;size_t capacity;
} ao_bank_reader;
static int ao_bank_read(void *context,uint64_t offset,void *out,size_t length)
{
    ao_bank_reader *reader=context;unsigned char *cursor=out;
    if(offset>reader->length || length>reader->length-offset)return 0;
    while(length) {
        if(offset<reader->at || offset-reader->at>=reader->held) {
            size_t span=reader->length-offset>sizeof(reader->window) ? sizeof(reader->window) : (size_t)(reader->length-offset);
            if(sh_audio_originals_read(reader->originals,reader->path,offset,reader->window,span,NULL,
                reader->error,reader->capacity)!=1)return 0;
            reader->at=offset;reader->held=span;
        }
        size_t span=reader->held-(size_t)(offset-reader->at);if(span>length)span=length;
        memcpy(cursor,reader->window+(size_t)(offset-reader->at),span);cursor+=span;offset+=span;length-=span;
    }
    return 1;
}
static int ao_bank_index(sh_audio_originals *originals,const char *name,int optional,
    char *error,size_t capacity)
{
    char *paths[2]={0};ao_bank_reader *reader=NULL;sh_audio_bank bank={0};int ok=0,found=0;
    if(!sh_audio_originals_paths(originals,name,paths,paths+1))goto done;
    reader=calloc(1,sizeof(*reader));if(!reader)goto done;
    reader->originals=originals;reader->error=error;reader->capacity=capacity;
    for(size_t i=0;i<2;i++) {
        int status=sh_audio_originals_read(originals,paths[i],0,NULL,0,&reader->length,error,capacity);
        if(status<0)goto done;
        if(status){reader->path=paths[i];found=1;break;}
    }
    if(!found){ok=optional;goto done;}
    if(sh_audio_bank_read_source((sh_audio_bank_source){reader,reader->length,ao_bank_read},&bank,error,capacity)!=1)goto done;
    if(!sh_audio_bank_names_identity(name,bank.id)) {
        ao_fail(error,capacity,"installed audio bank filename does not match its cooked identity");goto done;
    }
    for(size_t i=0;i<originals->bank_count;i++)if(originals->banks[i].metadata.id==bank.id) {
        sh_package_original_identity a,b;
        if(sh_audio_originals_identity(originals,reader->path,&a,error,capacity)!=1 ||
            sh_audio_originals_identity(originals,originals->banks[i].path,&b,error,capacity)!=1)goto done;
        if(!a.scope || !b.scope || a.file.length!=b.file.length || memcmp(a.file.digest,b.file.digest,32)) {
            ao_fail(error,capacity,"installed audio bank identity has different effective bytes");goto done;
        }
        ok=1;goto done;
    }
    if(originals->bank_count>=SIZE_MAX/sizeof(*originals->banks)-1)goto done;
    sh_audio_original_bank *grown=realloc(originals->banks,(originals->bank_count+1)*sizeof(*grown));
    if(!grown)goto done;
    originals->banks=grown;
    char *path=_strdup(reader->path);if(!path)goto done;
    grown[originals->bank_count++]=(sh_audio_original_bank){path,strrchr(path,'/')+1,bank};
    memset(&bank,0,sizeof(bank));ok=1;
done:
    sh_audio_bank_free(&bank);free(reader);free(paths[0]);free(paths[1]);
    if(!ok && error && capacity && !*error)ao_fail(error,capacity,"cannot capture installed audio bank metadata");
    return ok;
}
static int ao_bank_order(const void *a,const void *b)
{ return strcmp(*(const char *const *)a,*(const char *const *)b); }
typedef struct ao_bank_ids {uint32_t *ids;size_t count;} ao_bank_ids;
static int ao_bank_id(void *context,uint32_t id,uint32_t language)
{
    ao_bank_ids *list=context;(void)language;
    for(size_t i=0;i<list->count;i++)if(list->ids[i]==id)return 1;
    if(list->count>=SIZE_MAX/sizeof(*list->ids)-1)return 0;
    uint32_t *grown=realloc(list->ids,(list->count+1)*sizeof(*grown));if(!grown)return 0;
    list->ids=grown;grown[list->count++]=id;return 1;
}
int sh_audio_originals_banks(sh_audio_originals *originals,const sh_audio_original_bank **banks,
    size_t *count,char *error,size_t capacity)
{
    ao_bank_ids packed={0};int ok=0;
    if(banks)*banks=NULL;if(count)*count=0;if(error && capacity)*error=0;
    if(!banks || !count || !sh_audio_originals_ready(originals))
        return ao_fail(error,capacity,"installed audio metadata is unavailable");
    AcquireSRWLockExclusive(&originals->catalog_lock);
    if(originals->catalog_ready){ok=1;goto done;}
    /* This also freezes the one existing package and loose-name inventory.
     * Copy IDs under its lock, then parse through the ordinary pinned reader. */
    if(sh_audio_originals_packaged_banks(originals,ao_bank_id,&packed,error,capacity)!=1)goto done;
    if(originals->bank_name_count>1)qsort(originals->bank_names,originals->bank_name_count,sizeof(char *),ao_bank_order);
    for(size_t i=0;i<originals->bank_name_count;i++)
        if(!ao_bank_index(originals,originals->bank_names[i],0,error,capacity))goto done;
    for(size_t i=0;i<packed.count;i++) {
        int found=0;char name[32];
        for(size_t b=0;b<originals->bank_count;b++)if(originals->banks[b].metadata.id==packed.ids[i]){found=1;break;}
        if(found)continue;
        snprintf(name,sizeof(name),"%u.bnk",packed.ids[i]);
        if(!ao_bank_index(originals,name,1,error,capacity))goto done;
    }
    originals->catalog_ready=1;ok=1;
done:
    free(packed.ids);
    if(ok){*banks=originals->banks;*count=originals->bank_count;}
    else {
        for(size_t i=0;i<originals->bank_count;i++) {
            free((char *)originals->banks[i].path);sh_audio_bank_free(&originals->banks[i].metadata);
        }
        free(originals->banks);originals->banks=NULL;originals->bank_count=0;
    }
    ReleaseSRWLockExclusive(&originals->catalog_lock);return ok ? 1 : -1;
}

static int ao_id_compare(const void *a,const void *b)
{ uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return x<y ? -1 : x>y; }

int sh_audio_originals_ids(sh_audio_originals *originals,const uint32_t **ids,
    size_t *count,char *error,size_t capacity)
{
    const sh_audio_original_bank *banks=NULL;size_t bank_count=0,used=0,allocated=0;
    uint32_t *pending=NULL;ao_bank_reader *reader=NULL;int ok=0;
    if(ids)*ids=NULL;if(count)*count=0;if(error && capacity)*error=0;
    if(!ids || !count)return ao_fail(error,capacity,"original audio identity output is missing");
    if(sh_audio_originals_banks(originals,&banks,&bank_count,error,capacity)!=1)return -1;
    AcquireSRWLockExclusive(&originals->catalog_lock);
    if(originals->ids_ready){ok=1;goto done;}
    reader=calloc(1,sizeof(*reader));if(!reader)goto done;
    reader->originals=originals;reader->error=error;reader->capacity=capacity;
    for(size_t b=0;b<bank_count;b++) {
        sh_audio_bank_codec codec={0};reader->path=banks[b].path;reader->at=0;reader->held=0;
        if(sh_audio_originals_read(originals,reader->path,0,NULL,0,&reader->length,error,capacity)!=1 ||
            sh_audio_bank_codec_read((sh_audio_bank_source){reader,reader->length,ao_bank_read},&codec,error,capacity)!=1)goto done;
        int copied=1;
        for(size_t i=0;i<codec.span_count && copied;i++) {
            const sh_audio_identity_span *span=codec.spans+i;
            if(!(span->flags&SH_AUDIO_ID_LITERAL))copied=ao_id_append(&pending,&used,&allocated,span->value);
        }
        sh_audio_bank_codec_free(&codec);if(!copied)goto done;
    }
    /* Reserve even unused file identities. The engine can open media directly,
     * and a package's numeric inventory is authoritative without decoding DATA.
     * Including other languages here is conservative and permits later locale
     * changes without reusing a numeric file name from the installed packages. */
    int copied=1;
    AcquireSRWLockExclusive(&originals->lock);
    for(size_t f=0;f<originals->count && copied;f++) {
        const ao_file *file=originals->files[f];if(!file->packed)continue;
        for(unsigned kind=0;kind<2 && copied;kind++)for(size_t i=0;i<file->package.counts[kind] && copied;i++) {
            uint64_t id=file->package.entries[kind][i].id;
            if(id<=UINT32_MAX)copied=ao_id_append(&pending,&used,&allocated,(uint32_t)id);
        }
    }
    for(size_t i=0;i<originals->loose_media_count && copied;i++)
        copied=ao_id_append(&pending,&used,&allocated,originals->loose_media[i]);
    ReleaseSRWLockExclusive(&originals->lock);
    if(!copied)goto done;
    if(used>1)qsort(pending,used,sizeof(*pending),ao_id_compare);
    size_t unique=0;
    for(size_t i=0;i<used;i++)if(!i || pending[i]!=pending[i-1])pending[unique++]=pending[i];
    originals->ids=pending;originals->id_count=unique;originals->ids_ready=1;pending=NULL;ok=1;
done:
    free(pending);free(reader);
    if(ok){*ids=originals->ids;*count=originals->id_count;}
    else if(error && capacity && !*error)ao_fail(error,capacity,"cannot capture original audio identities");
    ReleaseSRWLockExclusive(&originals->catalog_lock);return ok ? 1 : -1;
}
