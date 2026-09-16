#include <stdio.h>
#include <string.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
static int protect_calls, protect_failure;
static BOOL protect_probe(LPVOID address, SIZE_T size, DWORD protection, PDWORD old)
{
    if (++protect_calls == protect_failure) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return VirtualProtect(address,size,protection,old);
}
#define VirtualProtect protect_probe
#include "../src/backend/audio_files_native.c"
#undef VirtualProtect

static int failures, provider_calls, native_calls, graph_calls;
static int provider_result = 1, wrong_length;
static uint64_t large_length;
static const char *fixture = "first snapshot";
static char requested[4096];
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)

void backend_log(const char *message) { (void)message; }
void sh_resource_graph_file(const char *path) { CHECK(path && path[0]); ++graph_calls; }
int sh_package_runtime_open_file(const char *path, FILE **stream, uint64_t *length)
{
    ++provider_calls; strcpy_s(requested, sizeof(requested), path);
    *stream = NULL; *length = 0;
    if (provider_result != 1) return provider_result;
    CHECK(!tmpfile_s(stream) && *stream);
    if (!*stream) return -1;
    *length = strlen(fixture);
    CHECK(fwrite(fixture, 1, (size_t)*length, *stream) == *length);
    CHECK(!fflush(*stream));
    if (large_length) {
        HANDLE handle=(HANDLE)_get_osfhandle(_fileno(*stream));
        DWORD done; LARGE_INTEGER position;
        int sparse=DeviceIoControl(handle,FSCTL_SET_SPARSE,NULL,0,NULL,0,&done,NULL)!=0;
        CHECK(sparse);
        if(!sparse){fclose(*stream);*stream=NULL;return -1;}
        position.QuadPart=(LONGLONG)large_length-1;
        CHECK(SetFilePointerEx(handle,position,NULL,FILE_BEGIN));
        CHECK(WriteFile(handle,"Z",1,&done,NULL) && done==1);
        *length=large_length;
    }
    if (wrong_length) ++*length;
    return 1;
}
static int provider(void *context, const char *path, FILE **stream, uint64_t *length)
{ CHECK(context == (void *)7); return sh_package_runtime_open_file(path, stream, length); }
static int original_id(void *self, uint32_t id, int mode, const void *flags,
    unsigned char *sync, sh_audio_file_descriptor *out)
{ CHECK(self && id == 17 && mode >= 0 && flags && sync && out); ++native_calls; return 42; }
static int original_name(void *self, const wchar_t *name, int mode, const void *flags,
    unsigned char *sync, sh_audio_file_descriptor *out)
{ CHECK(self && name && mode >= 0 && flags && sync && out); ++native_calls; return 43; }
static void expect_path(const wchar_t *name, uint32_t id, int by_id, const void *flags,
    const wchar_t *bank, const wchar_t *media, const wchar_t *language, const char *expected)
{
    char *path = NULL;
    int result = sh_audio_file_path(name,id,by_id,flags,bank,media,language,&path);
    CHECK(result == (expected ? 1 : 0));
    if (expected) CHECK(path && !strcmp(path,expected));
    else CHECK(!path);
    free(path);
}
static void paths(void)
{
    uint32_t flags[7] = {0};
    char *path = NULL;
    expect_path(L"Boss.BNK",0,0,flags,L"banks/",L"media/",L"English(US)","sound/soundbanks/pc/banks/boss.bnk");
    expect_path(NULL,UINT32_MAX,1,flags,L"banks/",L"media/",L"", "sound/soundbanks/pc/banks/4294967295.bnk");
    flags[1]=4; flags[6]=1;
    expect_path(NULL,17,1,flags,L"banks/",L"media/",L"English(US)","sound/soundbanks/pc/media/english(us)/17.wem");
    expect_path(L"folder\\17.wem",0,0,flags,L"banks/",L"media/",L"English(US)","sound/soundbanks/pc/english(us)/folder/17.wem");
    expect_path(L"initial.pck",0,0,NULL,L"banks/",L"media/",L"English(US)","sound/soundbanks/pc/initial.pck");
    expect_path(L"initial.pck",0,0,flags,L"",L"",L"", "sound/soundbanks/pc/initial.pck");
    flags[1]=0; flags[0]=1;
    expect_path(NULL,17,1,flags,L"banks/",L"media/",L"", "sound/soundbanks/pc/banks/17.bnk");
    flags[0]=2;
    expect_path(NULL,17,1,flags,L"",L"",L"",NULL);
    expect_path(NULL,17,1,NULL,L"",L"",L"",NULL);
    /* An external source names its own complete location: the native location
     * base writes no base path and no bank directory for company 0 codec 201,
     * so a legitimate relative request is answered as it stands and a request
     * that leaves the resource namespace is left native. */
    flags[0]=0; flags[1]=201; flags[6]=0;
    expect_path(L"external.wav",0,0,flags,L"banks/",L"media/",L"English(US)","external.wav");
    expect_path(L"streamed/external.wav",0,0,flags,L"banks/",L"media/",L"English(US)","streamed/external.wav");
    expect_path(L"..\\outside.wav",0,0,flags,L"",L"",L"",NULL);
    expect_path(L"streamed/../../outside.wav",0,0,flags,L"",L"",L"",NULL);
    expect_path(L"C:\\outside.wav",0,0,flags,L"",L"",L"",NULL);
    expect_path(L"/outside.wav",0,0,flags,L"",L"",L"",NULL);
    flags[6]=1;
    expect_path(L"external.wav",0,0,flags,L"banks/",L"media/",L"English(US)","english(us)/external.wav");
    flags[1]=0;
    expect_path(L"C:\\external.bnk",0,0,flags,L"",L"",L"",NULL);
    expect_path(L"/external.bnk",0,0,flags,L"",L"",L"",NULL);
    expect_path(L"../outside.bnk",0,0,flags,L"",L"",L"",NULL);
    expect_path(L"ok.bnk",0,0,flags,L"../",L"",L"",NULL);
    expect_path(L"caf\x00e9.bnk",0,0,flags,L"",L"",L"", "sound/soundbanks/pc/caf\xc3\xa9.bnk");
    CHECK(sh_audio_file_path(L"\xd800.bnk",0,0,flags,L"",L"",L"",&path)==-1 && !path);
    {
        wchar_t long_name[1024];
        for(size_t i=0;i<sizeof(long_name)/sizeof(long_name[0])-1;++i) long_name[i]=L'a';
        long_name[1023]=0;
        CHECK(sh_audio_file_path(long_name,0,0,flags,L"",L"",L"",&path)==1);
        CHECK(path && strlen(path)==1023+strlen("sound/soundbanks/pc/")); free(path);
    }
}
static void read_at(sh_audio_file_descriptor *file, uint64_t offset, const char *expected)
{
    OVERLAPPED where = {0}; char bytes[64] = {0}; DWORD got=0;
    size_t length=strlen(expected);
    where.Offset=(DWORD)offset; where.OffsetHigh=(DWORD)(offset>>32);
    CHECK(ReadFile(file->handle,bytes,(DWORD)length,&got,&where));
    CHECK(got==length && !memcmp(bytes,expected,length));
}
static void handles(void)
{
    sh_audio_file_descriptor first, second, untouched;
    unsigned char sync=0;
    CHECK(sh_audio_file_open("sound/soundbanks/pc/a.bnk",9,provider,(void *)7,&sync,&first)==1);
    CHECK(sync==1 && first.length==strlen(fixture) && first.device==9 && !first.block && !first.sector && !first.custom);
    fixture="second snapshot";
    CHECK(sh_audio_file_open("sound/soundbanks/pc/a.bnk",9,provider,(void *)7,&sync,&second)==1);
    read_at(&first,6,"snapshot"); read_at(&second,0,"second"); read_at(&first,0,"first");
    CHECK(CloseHandle(first.handle)); CHECK(CloseHandle(second.handle));
    memset(&untouched,0xa5,sizeof(untouched)); first=untouched; sync=7;
    provider_result=0;
    CHECK(sh_audio_file_open("a",9,provider,(void *)7,&sync,&first)==0);
    CHECK(sync==7 && !memcmp(&first,&untouched,sizeof(first)));
    provider_result=-1;
    CHECK(sh_audio_file_open("a",9,provider,(void *)7,&sync,&first)==-1);
    CHECK(sync==7 && !memcmp(&first,&untouched,sizeof(first)));
    provider_result=1; wrong_length=1;
    CHECK(sh_audio_file_open("a",9,provider,(void *)7,&sync,&first)==-1);
    CHECK(sync==7 && !memcmp(&first,&untouched,sizeof(first)));
    wrong_length=0;
    CHECK(sh_audio_file_open("a",UINT32_MAX,provider,(void *)7,&sync,&first)==-1);
    large_length=UINT64_C(0x100000025);
    CHECK(sh_audio_file_open("a",9,provider,(void *)7,&sync,&first)==1);
    CHECK(first.length==large_length);
    read_at(&first,large_length-1,"Z");CHECK(CloseHandle(first.handle));large_length=0;
}
static void lea(unsigned char *at, unsigned char reg, void *target)
{
    int32_t relative=(int32_t)((unsigned char *)target-at-7);
    at[0]=0x48;at[1]=0x8d;at[2]=reg;memcpy(at+3,&relative,4);
}
static unsigned char *fake_image(sig_result results[4])
{
    unsigned char *image=(unsigned char *)VirtualAlloc(NULL,0x6000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    IMAGE_DOS_HEADER *dos; IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *s;
    void **table;
    CHECK(image); if(!image)return NULL;
    dos=(IMAGE_DOS_HEADER *)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
    nt=(IMAGE_NT_HEADERS64 *)(image+0x80);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.SizeOfOptionalHeader=sizeof(nt->OptionalHeader);nt->FileHeader.NumberOfSections=3;
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x6000;
    s=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<3;++i){s[i].VirtualAddress=(i+1)*0x1000;s[i].Misc.VirtualSize=i==2?0x2000:0x1000;s[i].Characteristics=IMAGE_SCN_MEM_READ;}
    s[0].Characteristics|=IMAGE_SCN_MEM_EXECUTE;s[2].Characteristics|=IMAGE_SCN_MEM_WRITE;
    lea(image+0x1004,0x0d,image+0x3000);lea(image+0x1010,0x05,image+0x2000);lea(image+0x1300,0x05,image+0x4000);
    table=(void **)(image+0x2000);table[1]=image+0x1100;table[2]=image+0x1200;
    results[0]=(sig_result){"AudioFileResolverInit",SIG_OK,(uintptr_t)(image+0x1000),0};
    results[1]=(sig_result){"AudioFileOpenId",SIG_OK,(uintptr_t)(image+0x1100),0};
    results[2]=(sig_result){"AudioFileOpenName",SIG_OK,(uintptr_t)(image+0x1200),0};
    results[3]=(sig_result){"AudioFileLanguage",SIG_OK,(uintptr_t)(image+0x1300),0};
    return image;
}
static void native_adapter(void)
{
    sig_result results[4]; unsigned char *image=fake_image(results), sync=0;
    sh_audio_file_descriptor file;uint32_t flags[7]={0};void **table;int calls;
    if(!image)return;
    table=(void **)(image+0x2000);
    results[1].status=SIG_AMBIGUOUS;
    CHECK(!sh_audio_files_native_install(results,4,image));CHECK(table[1]==image+0x1100);
    g_afn_attempted=0;results[1].status=SIG_OK;table[2]=image+0x1110;
    CHECK(!sh_audio_files_native_install(results,4,image));CHECK(table[1]==image+0x1100);
    g_afn_attempted=0;table[2]=image+0x1200;
    protect_calls=0;protect_failure=1;
    CHECK(!sh_audio_files_native_install(results,4,image));
    CHECK(table[1]==image+0x1100 && table[2]==image+0x1200 && !sh_audio_files_native_ready());
    g_afn_attempted=0;protect_calls=0;protect_failure=2;
    CHECK(!sh_audio_files_native_install(results,4,image));
    CHECK(table[1]==afn_open_id && table[2]==afn_open_name && !sh_audio_files_native_ready());
    g_afn_id=original_id;
    CHECK(afn_open_id(image+0x3000,17,0,flags,&sync,&file)==42);
    CHECK(!graph_calls);native_calls=0;
    g_afn_attempted=0;protect_calls=0;protect_failure=0;
    table[1]=image+0x1100;table[2]=image+0x1200;
    CHECK(sh_audio_files_native_install(results,4,image));
    CHECK(sh_audio_files_native_ready() && table[1]==afn_open_id && table[2]==afn_open_name);
    CHECK(sh_audio_files_native_install(results,4,image));
    g_afn_id=original_id;g_afn_name=original_name;
    *(uint32_t *)(image+0x3630)=6;
    wcscpy_s((wchar_t *)(image+0x4000),260,L"English(US)");
    {
        wchar_t language[260];
        CHECK(sh_audio_files_native_language(language,260) && !wcscmp(language,L"English(US)"));
        CHECK(!sh_audio_files_native_language(language,2) && !language[0]);
        CHECK(!sh_audio_files_native_language(NULL,260));
        char *localized=NULL, *fallback=NULL;
        CHECK(sh_audio_files_native_bank_paths("Boss.BNK",&localized,&fallback));
        CHECK(localized && !strcmp(localized,"sound/soundbanks/pc/english(us)/boss.bnk"));
        CHECK(fallback && !strcmp(fallback,"sound/soundbanks/pc/boss.bnk"));
        free(localized);free(fallback);
        wcscpy_s((wchar_t *)(image+0x3220),260,L"banks/");
        CHECK(sh_audio_files_native_bank_paths("Boss.BNK",&localized,&fallback));
        CHECK(localized && !strcmp(localized,"sound/soundbanks/pc/banks/english(us)/boss.bnk"));
        CHECK(fallback && !strcmp(fallback,"sound/soundbanks/pc/banks/boss.bnk"));
        free(localized);free(fallback);*(wchar_t *)(image+0x3220)=0;
    }
    fixture="native descriptor";provider_result=1;
    CHECK(afn_open_id(image+0x3000,17,0,flags,&sync,&file)==1);
    CHECK(!strcmp(requested,"sound/soundbanks/pc/17.bnk") && graph_calls==1);
    read_at(&file,0,"native descriptor"); CHECK(CloseHandle(file.handle));
    flags[1]=4;flags[6]=1;
    CHECK(afn_open_name(image+0x3000,L"17.wem",0,flags,&sync,&file)==1);
    CHECK(!strcmp(requested,"sound/soundbanks/pc/english(us)/17.wem") && graph_calls==2);
    CHECK(CloseHandle(file.handle));
    calls=provider_calls;
    CHECK(afn_open_id(image+0x3000,17,1,flags,&sync,&file)==42);
    CHECK(provider_calls==calls && native_calls==1);
    provider_result=0;
    CHECK(afn_open_name(image+0x3000,L"17.wem",0,flags,&sync,&file)==43);
    CHECK(native_calls==2);
    provider_result=-1;
    CHECK(afn_open_id(image+0x3000,17,0,flags,&sync,&file)==2);
    CHECK(native_calls==2); /* An owned failure cannot fall through to old bytes. */
    g_afn_ready=0;
    { char *a=(char *)1,*b=(char *)1; CHECK(!sh_audio_files_native_bank_paths("a.bnk",&a,&b) && !a && !b); }
    { wchar_t language[260]; CHECK(!sh_audio_files_native_language(language,260) && !language[0]); }
    CHECK(afn_open_id(image+0x3000,17,0,flags,&sync,&file)==42);
    CHECK(VirtualFree(image,0,MEM_RELEASE));g_afn_attempted=0;
}
int main(void)
{
    paths();handles();native_adapter();
    if(failures)return 1;
    puts("audio file paths, immutable handles and native resolver adapter tests passed");return 0;
}
