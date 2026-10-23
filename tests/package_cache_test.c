#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_sources.h"
#include "audio_bank_codec.h"

static int failures;
static char root[MAX_PATH], paths[32][4096];
static size_t path_count;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)
static void remember(const char *path)
{
    CHECK(path && !strncmp(path,root,strlen(root)) && path[strlen(root)]=='/');
    if(!path)return;
    for(size_t i=0;i<path_count;i++)if(!strcmp(paths[i],path))return;
    CHECK(path_count<32); if(path_count<32)strcpy_s(paths[path_count++],4096,path);
}
static size_t files(void)
{
    char pattern[MAX_PATH]; WIN32_FIND_DATAA data; size_t count=0;
    snprintf(pattern,sizeof(pattern),"%s/*",root);
    HANDLE find=FindFirstFileA(pattern,&data);
    if(find!=INVALID_HANDLE_VALUE) {
        do { if(strcmp(data.cFileName,".") && strcmp(data.cFileName,".."))++count; } while(FindNextFileA(find,&data));
        FindClose(find);
    }
    return count;
}
typedef struct content { const unsigned char *bytes; size_t length; int failure; } content;
static int produce(void *context,int (*write)(void *,const void *,size_t),void *sink,char *error,size_t capacity)
{
    content *input=context; size_t at=0;
    while(at<input->length) {
        size_t n=input->length-at>513?513:input->length-at;
        int result=write(sink,input->bytes+at,n); at+=n;
        if(input->failure==2) { (void)result; return 1; } /* Even an ignored sink failure must abort. */
        if(!result)return 0;
        if(input->failure==1) { snprintf(error,capacity,"interrupted producer"); return 0; }
    }
    return 1;
}
static void check_stream(sh_package_file *file,const void *bytes,size_t length)
{
    char error[256]; size_t size=0;
    unsigned char *body=sh_package_file_read(file,length,&size,error,sizeof(error));
    CHECK(body && size==length && !memcmp(body,bytes,length)); free(body);
}
static void test_cache(void)
{
    unsigned char data[131073]; char error[256], *first=NULL,*second=NULL;
    for(size_t i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i*7);
    content input={data,sizeof(data),0}; sh_package_file_identity identity;
    CHECK(sh_package_bytes_identity(data,sizeof(data),&identity));
    sh_package_file *a=sh_package_file_generate(root,sizeof(data),NULL,produce,&input,&first,error,sizeof(error));
    CHECK(a && first && !error[0]); remember(first); check_stream(a,data,sizeof(data));
    sh_package_file *b=sh_package_file_generate(root,sizeof(data),identity.digest,produce,&input,&second,error,sizeof(error));
    CHECK(b && second && !strcmp(first,second) && files()==1); check_stream(b,data,sizeof(data));
    CHECK(sh_package_file_equal(a,b)==1 && sh_package_file_equal(a,NULL)==-1);
    sh_package_file_release(b); free(second); second=NULL;
    uint64_t n=0; FILE *reader=sh_package_file_open(a,&n,error,sizeof(error)); CHECK(reader && n==sizeof(data));
    if(reader)CHECK(fgetc(reader)==data[0]);
    CHECK(sh_package_file_equal(a,a)==1);
    sh_package_file_release(a); a=NULL;
    CHECK(!DeleteFileA(first) && GetLastError()==ERROR_SHARING_VIOLATION);
    if(reader) { CHECK(fgetc(reader)==data[1]); fclose(reader); }
    input.failure=1;
    CHECK(!sh_package_file_generate(root,sizeof(data),NULL,produce,&input,&second,error,sizeof(error)));
    CHECK(!second && strstr(error,"interrupted") && files()==1);
    input.failure=2;
    CHECK(!sh_package_file_generate(root,1,NULL,produce,&input,&second,error,sizeof(error)) && !second && files()==1);
    input.failure=0;
    CHECK(!sh_package_file_generate(root,sizeof(data)+1,NULL,produce,&input,&second,error,sizeof(error)) && files()==1);
    unsigned char wrong[32]={0};
    CHECK(!sh_package_file_generate(root,sizeof(data),wrong,produce,&input,&second,error,sizeof(error)) && files()==1);
    /* A damaged cached object must not poison valid packages or be overwritten
     * underneath a retained reader. It remains untouched; healthy output gets
     * its own cached file and subsequent builds dedupe that alternate. */
    FILE *broken=fopen(first,"wb"); CHECK(broken); if(broken) { CHECK(fwrite("bad",1,3,broken)==3); fclose(broken); }
    sh_package_file *bad=sh_package_file_pin(first,error,sizeof(error)); CHECK(bad);
    a=sh_package_file_generate(root,sizeof(data),NULL,produce,&input,&second,error,sizeof(error));
    CHECK(a && second && strcmp(first,second) && files()==2); remember(second); check_stream(a,data,sizeof(data));
    CHECK(sh_package_file_equal(a,bad)==0);
    char *third=NULL;
    b=sh_package_file_generate(root,sizeof(data),NULL,produce,&input,&third,error,sizeof(error));
    CHECK(b && third && !strcmp(second,third) && files()==2); check_stream(bad,"bad",3);
    sh_package_file_release(a); sh_package_file_release(b); sh_package_file_release(bad);
    free(first); free(second); free(third);
    /* A preverified cached source stays available after the authored file was
     * removed. The runtime captures this known snapshot without recopying it. */
    content small={(const unsigned char *)"snapshot",8,0};
    a=sh_package_file_generate(root,8,NULL,produce,&small,&first,error,sizeof(error)); CHECK(a); remember(first);
    sh_package_source_file source={0}; source.absolute="missing-author-file"; source.length=8;
    CHECK(sh_package_bytes_identity("snapshot",8,&identity)); memcpy(source.digest,identity.digest,32);
    b=sh_package_source_cache(&source,root,&second,error,sizeof(error));
    CHECK(b && !strcmp(first,second)); check_stream(b,"snapshot",8);
    sh_package_file_release(a); sh_package_file_release(b); free(first); free(second);
}

static void u32(unsigned char *p,uint32_t value)
{ for(int i=0;i<4;i++)p[i]=(unsigned char)(value>>(i*8)); }
static int bank_read(void *context,uint64_t offset,void *out,size_t length)
{
    content *input=context;
    if(offset>input->length || length>input->length-offset)return 0;
    memcpy(out,input->bytes+(size_t)offset,length); return 1;
}
static int relocate(void *context,const sh_audio_identity_span *field,uint32_t *id,char *error,size_t capacity)
{ (void)context; (void)error; (void)capacity; *id=field->value+1000; return 1; }
typedef struct bound_bank { sh_audio_bank_source source; sh_audio_bank_binding binding; } bound_bank;
static int produce_bank(void *context,int (*write)(void *,const void *,size_t),void *sink,char *error,size_t capacity)
{
    bound_bank *bank=context;
    return sh_audio_bank_binding_write(bank->source,&bank->binding,write,sink,error,capacity);
}
static void test_bound_bank(void)
{
    unsigned char bytes[49]={0}, before[49]; char error[256],*path=NULL;
    memcpy(bytes,"BKHD",4); u32(bytes+4,16); u32(bytes+8,113); u32(bytes+12,42);
    memcpy(bytes+24,"HIRC",4); u32(bytes+28,17); u32(bytes+32,1);
    bytes[36]=4; u32(bytes+37,8); u32(bytes+41,50); u32(bytes+45,0);
    memcpy(before,bytes,sizeof(bytes)); content data={bytes,sizeof(bytes),0};
    bound_bank bound={{&data,sizeof(bytes),bank_read},{0}};
    sh_audio_bank_codec codec={0};
    CHECK(sh_audio_bank_codec_read(bound.source,&codec,error,sizeof(error))==1);
    CHECK(sh_audio_bank_codec_bind(bound.source,&codec,relocate,NULL,&bound.binding,error,sizeof(error)));
    sh_package_file *file=sh_package_file_generate(root,bound.binding.output_length,NULL,
        produce_bank,&bound,&path,error,sizeof(error));
    CHECK(file && path); remember(path);
    uint64_t length=0; FILE *stream=sh_package_file_open(file,&length,error,sizeof(error)); CHECK(stream);
    sh_audio_bank actual={0};
    if(stream) { CHECK(sh_audio_bank_read(stream,&actual,error,sizeof(error))==1); fclose(stream); }
    CHECK(actual.id==1042 && actual.event_count==1 && actual.events[0]==1050);
    CHECK(!memcmp(bytes,before,sizeof(bytes))); /* Authored bank remains exact. */
    sh_audio_bank_free(&actual); sh_package_file_release(file); free(path);
    sh_audio_bank_binding_free(&bound.binding); sh_audio_bank_codec_free(&codec);
}
typedef struct simultaneous {
    const unsigned char *bytes;
    HANDLE barrier;
    volatile LONG ready;
} simultaneous;
typedef struct worker {
    simultaneous *group;
    sh_package_file *file;
    char *path, error[256];
} worker;
static int simultaneous_produce(void *context,int (*write)(void *,const void *,size_t),void *sink,char *error,size_t capacity)
{
    simultaneous *group=context; (void)error; (void)capacity;
    int ok=write(sink,group->bytes,257);
    if(InterlockedIncrement(&group->ready)==8)SetEvent(group->barrier);
    return WaitForSingleObject(group->barrier,10000)==WAIT_OBJECT_0 && ok;
}
static DWORD WINAPI generate_worker(void *context)
{
    worker *w=context;
    w->file=sh_package_file_generate(root,257,NULL,simultaneous_produce,w->group,&w->path,w->error,sizeof(w->error));
    return 0;
}
static void test_simultaneous(void)
{
    for(unsigned round=0;round<12;round++) {
        unsigned char bytes[257]; for(size_t i=0;i<sizeof(bytes);i++)bytes[i]=(unsigned char)(i+round);
        simultaneous group={bytes,CreateEventA(NULL,TRUE,FALSE,NULL),0};
        worker workers[8]={0}; HANDLE threads[8]={0}; CHECK(group.barrier);
        if(!group.barrier)return;
        size_t count=files();
        for(size_t i=0;i<8;i++) {
            workers[i].group=&group;
            threads[i]=CreateThread(NULL,0,generate_worker,workers+i,0,NULL); CHECK(threads[i]);
        }
        for(size_t i=0;i<8;i++)if(threads[i]) {
            CHECK(WaitForSingleObject(threads[i],20000)==WAIT_OBJECT_0); CloseHandle(threads[i]);
        }
        for(size_t i=0;i<8;i++) {
            if(!workers[i].file)fprintf(stderr,"cache worker failed: %s\n",workers[i].error);
            CHECK(workers[i].file && workers[i].path);
            if(workers[i].path) {
                remember(workers[i].path); check_stream(workers[i].file,bytes,sizeof(bytes));
                CHECK(workers[0].path && !strcmp(workers[i].path,workers[0].path));
            }
        }
        CHECK(files()==count+1); /* One published file, no temporary remnants. */
        for(size_t i=0;i<8;i++) { sh_package_file_release(workers[i].file); free(workers[i].path); }
        CloseHandle(group.barrier);
    }
}
static void test_changed_source(void)
{
    char path[4096],error[256],*cached=NULL;
    snprintf(path,sizeof(path),"%s/author-input",root);
    FILE *author=fopen(path,"wb"); CHECK(author); if(!author)return;
    CHECK(fwrite("after!",1,6,author)==6); fclose(author); remember(path);
    sh_package_source_file source={0}; sh_package_file_identity before;
    source.absolute=path; source.length=6;
    CHECK(sh_package_bytes_identity("before",6,&before)); memcpy(source.digest,before.digest,32);
    size_t count=files();
    CHECK(!sh_package_source_cache(&source,root,&cached,error,sizeof(error)) && !cached);
    CHECK(files()==count); /* Changed authored bytes never enter the snapshot. */
    CHECK(sh_package_bytes_identity("after!",6,&before)); memcpy(source.digest,before.digest,32);
    sh_package_file *file=sh_package_source_cache(&source,root,&cached,error,sizeof(error)); CHECK(file && cached);
    if(cached)remember(cached); check_stream(file,"after!",6); sh_package_file_release(file); free(cached);
    author=fopen(path,"rb"); CHECK(author); if(author) { char bytes[6]; CHECK(fread(bytes,1,6,author)==6 && !memcmp(bytes,"after!",6)); fclose(author); }
}
int main(void)
{
    char temp[MAX_PATH]; CHECK(GetTempPathA(sizeof(temp),temp));
    CHECK(GetTempFileNameA(temp,"pcm",0,root)); CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root,NULL));
    test_cache(); test_bound_bank(); test_simultaneous(); test_changed_source();
    CHECK(files()==path_count);
    for(size_t i=0;i<path_count;i++) {
        CHECK(!strncmp(paths[i],root,strlen(root)) && paths[i][strlen(root)]=='/');
        CHECK(DeleteFileA(paths[i]));
    }
    CHECK(RemoveDirectoryA(root));
    if(failures)return 1;
    puts("streamed immutable cache and bound audio bank tests passed"); return 0;
}
