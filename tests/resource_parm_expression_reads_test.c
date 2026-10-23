#include "../src/backend/resource_parm_expression_reads.c"
#include "native_pin_fixture.h"
#include <stdio.h>
#include <stdlib.h>
static int failures,renderer,pinned=1,observing=1,unknown,graph_failed,observer_throws,native_throws,mutation,wrong_result;
static unsigned calls[4],references[5];static uint64_t tokens[5]={11,12,13,14,15};
static unsigned char objects[5][0xa0];static unsigned char *image;static size_t image_size=0x6000000;
static unsigned char manager[0x10188];
static int prepare_failure=-1,commit_failure=-1,prepare_count,commit_count,installed[4];
static void *last_lexer,*last_value,*last_left,*last_right,*last_table;static uint16_t last_operation;static int last_level;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
int sh_host_is_vulkan(void){return renderer;}
int sh_host_is_pinned_renderer_build(int im){return pinned && renderer==im;}
int sh_resource_graph_native_observing(void){return observing;}
void sh_resource_graph_incomplete(void){graph_failed++;}
int sh_resource_graph_native_observe_resource(void *r){CHECK(!r);return 0;}
uint64_t sh_resource_instances_token(const void *r){for(unsigned i=0;i<5;i++)if(r==objects[i])return tokens[i];return 0;}
static int observe(void *r)
{
    if(!r){unknown++;return 1;}
    for(unsigned i=0;i<5;i++)if(r==objects[i]){references[i]++;if(observer_throws)RaiseException(0xe0317101,0,0,NULL);SetLastError(992);return 1;}
    CHECK(0);return 0;
}
static void *selected(int *p,unsigned kind)
{
    calls[kind]++;if(native_throws)RaiseException(0xe0317102,0,0,NULL);
    int index=(*p)++;void *r=((void **)(image+px_profiles[renderer].pool))[index];
    if(mutation==1)tokens[0]++;
    if(mutation==2)((void **)(image+px_profiles[renderer].pool))[index]=objects[4];
    if(mutation==3)*(int *)(objects[2]+0x68)^=1;
    if(mutation==4)*(void **)(objects[3]+0x60)=objects[2];
    SetLastError(771);return wrong_result?objects[4]:r;
}
static void *native_get(int *p){return selected(p,0);}
static void *native_constant(int *p,void *lexer,const void *value)
{last_lexer=lexer;last_value=(void *)value;return selected(p,1);}
static void *native_binary(int *p,void *lexer,void *left,void *right,void *table,uint16_t operation)
{last_lexer=lexer;last_left=left;last_right=right;last_table=table;last_operation=operation;return selected(p,2);}
static void *native_recursive(int *p,void *lexer,int level)
{last_lexer=lexer;last_level=level;return selected(p,3);}
static void *native[4]={(void *)native_get,(void *)native_constant,(void *)native_binary,(void *)native_recursive};
void *hook_prepare(void *target,void *detour,size_t stolen)
{
    const px_profile *p=&px_profiles[renderer];unsigned i;for(i=0;i<4;i++)if(target==image+p->targets[i])break;
    CHECK(i<4);if(i==4)return NULL;CHECK(detour==px_detours[i] && stolen==p->stolen[i]);prepare_count++;
    return prepare_failure==(int)i?NULL:native[i];
}
sh_patch_status hook_commit(void *trampoline)
{
    unsigned i;for(i=0;i<4;i++)if(trampoline==native[i])break;CHECK(i<4);if(i==4)return B2_PATCH_FAIL_PROTECT;
    commit_count++;if(commit_failure==(int)i)return B2_PATCH_FAIL_PROTECT;
    const px_profile *p=&px_profiles[renderer];unsigned char *entry=image+p->targets[i];
    memcpy(entry,"\xff\x25\0\0\0\0",6);memcpy(entry+6,px_detours+i,8);memset(entry+14,0x90,p->stolen[i]-14);installed[i]=1;return B2_PATCH_OK;
}
int hook_is_installed(void *trampoline){for(unsigned i=0;i<4;i++)if(trampoline==native[i])return installed[i];return 0;}
int hook_unpatch(void *trampoline)
{
    const px_profile *p=&px_profiles[renderer];for(unsigned i=0;i<4;i++)if(trampoline==native[i]){
        CHECK(sh_native_pin_fixture_copy(renderer,p->targets[i],image+p->targets[i],p->stolen[i]));installed[i]=0;return 1;
    }CHECK(0);return 0;
}
static int cmp_region(const void *a,const void *b)
{const RUNTIME_FUNCTION *x=a,*y=b;return x->BeginAddress<y->BeginAddress?-1:x->BeginAddress>y->BeginAddress;}
static void semantic(int im)
{
    renderer=im;const px_profile *p=&px_profiles[im];memset(image,0,image_size);memset(objects,0,sizeof objects);
    memset(references,0,sizeof references);memset(installed,0,sizeof installed);memset(calls,0,sizeof calls);
    for(unsigned i=0;i<5;i++){tokens[i]=11+i;*(int *)(objects[i]+0x68)=10+i;*(int *)(objects[i]+0x6c)=2;}
    *(void **)(objects[3]+0x60)=objects[4];
    for(size_t i=0;i<p->count;i++)CHECK(sh_native_pin_fixture_copy(im,p->pins[i].rva,image+p->pins[i].rva,p->pins[i].length));
    memset(manager,0,sizeof manager);*(void **)(image+p->pool)=objects[0];*(void **)(image+p->manager)=manager;
    RUNTIME_FUNCTION *regions=calloc(p->region_count,sizeof *regions);CHECK(regions);if(!regions)return;
    for(size_t i=0;i<p->region_count;i++)regions[i]=(RUNTIME_FUNCTION){p->regions[i].begin,p->regions[i].end,p->regions[i].unwind};
    qsort(regions,p->region_count,sizeof *regions,cmp_region);CHECK(RtlAddFunctionTable(regions,(DWORD)p->region_count,(DWORD64)(uintptr_t)image));
    px_base=NULL;px_renderer=-1;px_observe=NULL;memset(px_original,0,sizeof px_original);px_installed=0;
    prepare_failure=2;CHECK(!sh_resource_parm_expression_reads_install(image,observe));CHECK(!installed[0]&&!installed[1]&&!installed[2]&&!installed[3]);prepare_failure=-1;
    commit_failure=2;CHECK(!sh_resource_parm_expression_reads_install(image,observe));CHECK(!installed[0]&&!installed[1]&&!installed[2]&&!installed[3]);commit_failure=-1;
    CHECK(sh_resource_parm_expression_reads_install(image,observe));CHECK(sh_resource_parm_expression_reads_ready());
    int parser=0;unsigned prior=references[0];SetLastError(770);CHECK(px_get(&parser)==objects[0]&&parser==1&&references[0]==prior+1&&GetLastError()==771);
    parser=0;int v[4]={0};CHECK(px_constant(&parser,objects[4],v)==objects[0]&&last_lexer==objects[4]&&last_value==v);
    parser=0;prior=references[1];CHECK(px_binary(&parser,objects[4],objects[1],objects[2],NULL,0x14)==objects[0]);
    CHECK(references[1]==prior+1&&last_left==objects[1]&&last_right==objects[2]&&!last_table&&last_operation==0x14);
    /* A real Table branch does not consume the unused left pointer. */
    parser=0;prior=references[3];CHECK(px_binary(&parser,objects[4],(void *)1,objects[2],objects[3],0x14)==objects[0]&&references[3]==prior+1&&last_left==(void *)1);
    int bad_before=unknown;*(int *)(manager+0x10180)=0x93f;parser=0;
    CHECK(px_binary(&parser,NULL,NULL,objects[2],objects[3],0x14)==objects[0]&&unknown==bad_before+1);*(int *)(manager+0x10180)=0;
    parser=0;prior=references[0];CHECK(px_recursive(&parser,objects[4],4)==objects[0]&&references[0]==prior+1&&last_level==4);
    int before=unknown;observing=0;parser=0;prior=references[0];CHECK(px_get(&parser)==objects[0]&&references[0]==prior&&unknown==before);observing=1;
    mutation=1;parser=0;CHECK(px_get(&parser)==objects[0]&&unknown==before+1);mutation=0;before=unknown;
    mutation=2;parser=0;CHECK(px_constant(&parser,NULL,v)==objects[0]&&unknown==before+1);mutation=0;*(void **)(image+p->pool)=objects[0];before=unknown;
    mutation=3;parser=0;CHECK(px_binary(&parser,NULL,objects[1],objects[2],NULL,1)==objects[0]&&unknown==before+1);mutation=0;before=unknown;
    mutation=4;parser=0;CHECK(px_binary(&parser,NULL,NULL,objects[2],objects[3],1)==objects[0]&&unknown==before+1);mutation=0;before=unknown;
    wrong_result=1;parser=0;CHECK(px_get(&parser)==objects[4]&&unknown==before+1);wrong_result=0;before=unknown;
    observer_throws=1;parser=0;unsigned c=calls[0];CHECK(px_get(&parser)==objects[0]&&calls[0]==c+1&&unknown>before);observer_throws=0;before=unknown;
    native_throws=1;int caught=0;parser=0;__try {px_get(&parser);}__except(EXCEPTION_EXECUTE_HANDLER){caught=GetExceptionCode()==0xe0317102;}
    CHECK(caught&&unknown==before+1);native_throws=0;
    for(unsigned i=0;i<4;i++)for(size_t j=0;j<p->stolen[i];j++){image[p->targets[i]+j]^=1;CHECK(!sh_resource_parm_expression_reads_ready());image[p->targets[i]+j]^=1;}
    for(size_t i=0;i<p->count;i++){
        const px_pin *pin=p->pins+i;size_t offset=pin->skip;if(offset==pin->length)continue;
        image[pin->rva+offset]^=1;CHECK(!sh_resource_parm_expression_reads_ready());image[pin->rva+offset]^=1;
        image[pin->rva+pin->length-1]^=1;CHECK(!sh_resource_parm_expression_reads_ready());image[pin->rva+pin->length-1]^=1;
    }
    regions[0].UnwindData^=1;CHECK(!sh_resource_parm_expression_reads_ready());regions[0].UnwindData^=1;
    pinned=0;CHECK(!sh_resource_parm_expression_reads_ready());pinned=1;CHECK(sh_resource_parm_expression_reads_ready());
    CHECK(px_remove());CHECK(!sh_resource_parm_expression_reads_ready());CHECK(RtlDeleteFunctionTable(regions));free(regions);
}
static void actual_image(const char *path,int im)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);unsigned char *raw=malloc(n);CHECK(raw);if(!raw){fclose(f);return;}
    CHECK(fread(raw,1,n,f)==(size_t)n);fclose(f);IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);const px_profile *p=&px_profiles[im];
    for(size_t i=0;i<p->count;i++){const px_pin *pin=p->pins+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t at=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(at<=(size_t)n&&pin->length<=(size_t)n-at);
                if(at<=(size_t)n&&pin->length<=(size_t)n-at){sh_native_pin_hash hash;unsigned char digest[32];
                    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,raw+at,pin->length);sh_native_pin_hash_finish(&hash,digest);CHECK(!memcmp(digest,pin->digest[0],32));}found=1;break;}}
        CHECK(found);
    }free(raw);
}
int main(int argc,char **argv)
{
    image=VirtualAlloc(NULL,image_size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(image);if(!image)return 1;
    if(argc>2){int im=atoi(argv[2]);actual_image(argv[1],im);semantic(im);}else{semantic(0);semantic(1);}
    VirtualFree(image,0,MEM_RELEASE);printf("expression parm reads: %d failures\n",failures);return failures?1:0;
}
