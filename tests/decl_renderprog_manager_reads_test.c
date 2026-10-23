#define main sh_rpm_common_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_renderprog_manager_reads.c"
#include "native_pin_fixture.h"
static uintptr_t rpm_at(fixture *f,size_t off){return (uintptr_t)f->image+off;}
static int rpm_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_renderprog_manager_read_check(r,&f->input);}
static void rpm_rows(fixture *f,size_t at,size_t data,int count,int capacity)
{ptr(f->image,at,data?rpm_at(f,data):0);number(f->image,at+8,count);number(f->image,at+12,capacity);}
static void rpm_empty_hash(fixture *f,size_t at)
{
    ptr(f->image,at,rpm_at(f,0x150000));ptr(f->image,at+8,rpm_at(f,0x150000));
    number(f->image,at+0x10,1);number(f->image,at+0x14,0);number(f->image,at+0x1c,0);number(f->image,at+0x20,0);
    number(f->image,0x150000,-1);
}
static void rpm_type_setup(fixture *f,size_t at)
{
    const rpm_profile *p=rpm_profiles+f->input.renderer;
    ptr(f->image,at,rpm_at(f,p->typed_table));ptr(f->image,at+8,rpm_at(f,0x151000));ptr(f->image,at+0x68,rpm_at(f,0x151000));
    strcpy((char *)f->image+0x151000,"renderProg");
    rpm_empty_hash(f,at+0x38);rpm_empty_hash(f,at+0xb0);
}
static void rpm_setup(fixture *f,int renderer)
{
    setup(f,renderer);const rpm_profile *p=rpm_profiles+renderer;
    ptr(f->image,p->holder,rpm_at(f,p->object));ptr(f->image,p->object,rpm_at(f,p->table));
    static const unsigned slots[]={0x50,0x60,0x70,0x90};
    for(unsigned i=0;i<4;i++)ptr(f->image,p->table+slots[i],rpm_at(f,p->slots[i]));
    ptr(f->image,p->typed_table+0x30,rpm_at(f,p->factory));rpm_type_setup(f,p->typed);
    ptr(f->image,p->typed_table+0x18,rpm_at(f,p->load_factory));ptr(f->image,p->resource_table,rpm_at(f,p->resource_delete));
    ptr(f->image,p->typed_table+0x10,rpm_at(f,p->find_id));
    ptr(f->image,p->sampler,rpm_at(f,p->sampler_object));ptr(f->image,p->sampler_object,rpm_at(f,p->sampler_table));
    ptr(f->image,p->sampler_table+0x18,rpm_at(f,p->sampler_lookup));
    for(unsigned i=0;i<rpm_pin_counts[renderer];i++){const rpm_pin *pin=rpm_pins[renderer]+i;CHECK(sh_native_pin_fixture_copy(renderer,pin->rva,f->image+pin->rva,pin->length));}
}
static void rpm_populated(fixture *f)
{
    const rpm_profile *p=rpm_profiles+f->input.renderer;size_t dm=p->object;
    rpm_rows(f,dm+8,0x160000,3,4);
    ptr(f->image,0x160000,rpm_at(f,p->typed));ptr(f->image,0x160008,rpm_at(f,0x161000));ptr(f->image,0x160010,rpm_at(f,0x162000));
    /* An unrelated registry type is followed only for +88 ancestry. Its
     * arbitrary vtable and stored handles are never dispatched by this probe. */
    ptr(f->image,0x161000,1);rpm_type_setup(f,0x162000);ptr(f->image,0x162088,rpm_at(f,p->typed));
    rpm_rows(f,p->typed+0x20,0x163000,2,3);
    ptr(f->image,0x163000,rpm_at(f,0x164000));ptr(f->image,0x163008,rpm_at(f,0x165000));
    ptr(f->image,0x164008,rpm_at(f,0x164100));strcpy((char *)f->image+0x164100,"first");
    ptr(f->image,0x165008,rpm_at(f,0x165100));strcpy((char *)f->image+0x165100,"second");
    /* A populated collision chain is valid; cycles and out-of-array indices
     * must be rejected from actual native count/capacity bounds. */
    ptr(f->image,p->typed+0x38,rpm_at(f,0x166000));ptr(f->image,p->typed+0x40,rpm_at(f,0x166100));
    number(f->image,p->typed+0x48,1);number(f->image,p->typed+0x4c,3);number(f->image,p->typed+0x58,-1);
    number(f->image,0x166000,0);number(f->image,0x166100,1);number(f->image,0x166104,-1);
    rpm_rows(f,dm+0x20,0x167000,1,2);ptr(f->image,0x167000,rpm_at(f,0x168000));
    ptr(f->image,0x168008,rpm_at(f,0x168100));strcpy((char *)f->image+0x168100,"catalog.decl");
    ptr(f->image,0x168030,rpm_at(f,0x169000));ptr(f->image,0x169000,rpm_at(f,0x169100));
    strcpy((char *)f->image+0x169100,"program");number(f->image,0x169024,5);number(f->image,0x169028,7);
    rpm_rows(f,0x168038,0x169200,1,2);number(f->image,0x169200,0);
    rpm_rows(f,dm+0x38,0x16a000,1,2);ptr(f->image,0x16a000,rpm_at(f,0x16a100));
    strcpy((char *)f->image+0x16a100,"include.inc");rpm_rows(f,0x16a008,0x16a200,2,3);
    ptr(f->image,0x16a200,1);ptr(f->image,0x16a208,UINT64_C(0xabcdef1234567890));
    rpm_rows(f,p->sampler_object+0x90,0x16b000,2,3);
    ptr(f->image,0x16b010,rpm_at(f,0x16c000));strcpy((char *)f->image+0x16c000,"linearSampler");
    ptr(f->image,0x16b060,UINT64_C(0x123456789abcdef0));
    ptr(f->image,0x16b078,rpm_at(f,0x16c100));f->image[0x16c100]=0;
    /* Unused idStr vtable/length and a null stored sampler payload are native
     * lookup shapes. This helper never fabricates an origin from either row. */
    ptr(f->image,0x16b000,1);number(f->image,0x16b008,-1);
}
static void rpm_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rpm_setup(&f,renderer);const rpm_profile *p=rpm_profiles+renderer;
    CHECK(rpm_start(&f,&r) && r.state==1 && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    rpm_populated(&f);CHECK(rpm_start(&f,&r));
    /* Both native lock shapes are legal. Its epoch is CRT state, not a
     * synthetic boolean proving that initialization has run. */
    ptr(f.image,p->lock,rpm_at(&f,0x16d000));number(f.image,p->lock_epoch,-27);CHECK(rpm_start(&f,&r));
    f.denied=rpm_at(&f,0x16d000);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    ptr(f.image,p->lock,0);number(f.image,p->lock_epoch,0);CHECK(rpm_start(&f,&r));
    ptr(f.image,p->postload,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->postload,0);
    ptr(f.image,0x164008,0);CHECK(rpm_start(&f,&r));ptr(f.image,0x164008,rpm_at(&f,0x164100));
    ptr(f.image,0x164000,rpm_at(&f,p->resource_table));ptr(f.image,0x164018,1);CHECK(rpm_start(&f,&r));
    ptr(f.image,p->resource_table,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    ptr(f.image,p->resource_table,rpm_at(&f,p->resource_delete));ptr(f.image,0x164000,1);
    CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);
    number(f.image,0x16402c,4);CHECK(rpm_start(&f,&r));number(f.image,0x16402c,0);ptr(f.image,0x164018,0);
    f.denied=UINT64_C(0xabcdef1234567890);CHECK(rpm_start(&f,&r));f.denied=0;
    f.denied=rpm_at(&f,0x16c000);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    ptr(f.image,0x161088,rpm_at(&f,0x161000));CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr(f.image,0x161088,0);
    ptr(f.image,0x169018,rpm_at(&f,0x169000));CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr(f.image,0x169018,0);
    number(f.image,0x169024,INT_MAX);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);number(f.image,0x169024,5);
    number(f.image,0x166104,0);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,0x166104,-1);
    number(f.image,0x166104,2);number(f.image,0x166108,3);CHECK(!rpm_start(&f,&r));number(f.image,0x166104,-1);
    number(f.image,p->typed+0x2c,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,p->typed+0x2c,3);
    ptr(f.image,p->sampler_object+0x90,UINTPTR_MAX-3);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);ptr(f.image,p->sampler_object+0x90,rpm_at(&f,0x16b000));
    f.change_at=rpm_at(&f,0x162088);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_INVOCATION);f.change_at=0;ptr(f.image,0x162088,rpm_at(&f,p->typed));
    f.change_at=rpm_at(&f,p->holder);CHECK(!rpm_start(&f,&r));f.change_at=0;ptr(f.image,p->holder,rpm_at(&f,p->object));
    static const unsigned slots[]={0x50,0x60,0x70,0x90};
    for(unsigned i=0;i<4;i++){ptr(f.image,p->table+slots[i],1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->table+slots[i],rpm_at(&f,p->slots[i]));}
    ptr(f.image,p->typed_table+0x30,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->typed_table+0x30,rpm_at(&f,p->factory));
    ptr(f.image,p->typed_table+0x18,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->typed_table+0x18,rpm_at(&f,p->load_factory));
    ptr(f.image,p->typed_table+0x10,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->typed_table+0x10,rpm_at(&f,p->find_id));
    ptr(f.image,p->sampler_table+0x18,1);CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->sampler_table+0x18,rpm_at(&f,p->sampler_lookup));
    for(unsigned i=0;i<rpm_pin_counts[renderer];i++) {
        const rpm_pin *pin=rpm_pins[renderer]+i;size_t offsets[]={0,pin->length/2,pin->length-1};
        for(unsigned j=0;j<3;j++){unsigned char *at=f.image+pin->rva+offsets[j];*at^=1;CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*at^=1;}
    }
    CHECK(rpm_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!rpm_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(rpm_start(&f,&r));f.input.resource++;CHECK(!sh_decl_renderprog_manager_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
    CHECK(rpm_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static const unsigned char *rpm_raw(const unsigned char *raw,size_t length,uint32_t rva,size_t size)
{
    const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)raw;const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
        const IMAGE_SECTION_HEADER *s=sections+j;
        if(rva>=s->VirtualAddress && rva-s->VirtualAddress<=s->SizeOfRawData && size<=s->SizeOfRawData-(rva-s->VirtualAddress)) {
            size_t off=s->PointerToRawData+rva-s->VirtualAddress;CHECK(off<=length && size<=length-off);return raw+off;
        }
    }
    CHECK(0);return NULL;
}
static void rpm_image_case(const char *path,int renderer)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);
    unsigned char *raw=malloc(length);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,length,f)==(size_t)length);fclose(f);
    const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(raw+((const IMAGE_DOS_HEADER *)raw)->e_lfanew);
    for(unsigned i=0;i<rpm_pin_counts[renderer];i++){
        const rpm_pin *pin=rpm_pins[renderer]+i;const unsigned char *at=rpm_raw(raw,length,pin->rva,pin->length);
        CHECK(at && sh_native_pin_fixture_digest(at,pin->length,pin->digest));
    }
    const rpm_profile *p=rpm_profiles+renderer;uint32_t cells[]={p->holder,p->table+0x50,p->table+0x60,p->table+0x70,p->table+0x90,p->typed_table+0x30,p->sampler,p->sampler_table+0x18};
    uint32_t targets[]={p->object,p->slots[0],p->slots[1],p->slots[2],p->slots[3],p->factory,p->sampler_object,p->sampler_lookup};
    for(unsigned i=0;i<8;i++){uintptr_t actual=0;const unsigned char *at=rpm_raw(raw,length,cells[i],8);if(at)memcpy(&actual,at,8);CHECK(actual==nt->OptionalHeader.ImageBase+targets[i]);}
    uint32_t extra_cells[]={p->typed_table+0x18,p->resource_table,p->typed_table+0x10};uint32_t extra_targets[]={p->load_factory,p->resource_delete,p->find_id};
    for(unsigned i=0;i<3;i++){uintptr_t actual=0;const unsigned char *at=rpm_raw(raw,length,extra_cells[i],8);if(at)memcpy(&actual,at,8);CHECK(actual==nt->OptionalHeader.ImageBase+extra_targets[i]);}
    free(raw);
}
static void rpm_hash_vector(const unsigned char *text,size_t size,const char *expected)
{
    unsigned char digest[32],segmented[32];char hex[65];sh_native_pin_hash h;
    sh_native_pin_hash_init(&h);sh_native_pin_hash_update(&h,text,size);sh_native_pin_hash_finish(&h,digest);
    for(unsigned i=0;i<32;i++)sprintf(hex+i*2,"%02x",digest[i]);CHECK(!strcmp(hex,expected));
    for(size_t step=1;step<=73;step+=8){
        sh_native_pin_hash_init(&h);for(size_t off=0;off<size;){size_t n=size-off;if(n>step)n=step;sh_native_pin_hash_update(&h,text+off,n);off+=n;}
        sh_native_pin_hash_finish(&h,segmented);CHECK(!memcmp(digest,segmented,32));
    }
}
static void rpm_hash_vectors(void)
{
    rpm_hash_vector((const unsigned char *)"",0,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    rpm_hash_vector((const unsigned char *)"abc",3,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    static const unsigned char multi[]="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rpm_hash_vector(multi,sizeof multi-1,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    unsigned char block[1000],digest[32];memset(block,'a',sizeof block);sh_native_pin_hash h;
    sh_native_pin_hash_init(&h);for(unsigned i=0;i<1000;i++)sh_native_pin_hash_update(&h,block,sizeof block);sh_native_pin_hash_finish(&h,digest);
    char hex[65];for(unsigned i=0;i<32;i++)sprintf(hex+i*2,"%02x",digest[i]);
    CHECK(!strcmp(hex,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    /* No mutation in the first, middle, or last input byte may preserve the
     * original full digest. Native span mutation controls run below too. */
    unsigned char original[32];sh_native_pin_hash_init(&h);sh_native_pin_hash_update(&h,block,sizeof block);sh_native_pin_hash_finish(&h,original);
    const size_t points[]={0,55,56,63,64,500,999};
    for(unsigned i=0;i<sizeof points/sizeof points[0];i++){block[points[i]]^=1;sh_native_pin_hash_init(&h);sh_native_pin_hash_update(&h,block,sizeof block);sh_native_pin_hash_finish(&h,digest);CHECK(memcmp(digest,original,32));block[points[i]]^=1;}
}
int main(int argc,char **argv)
{rpm_hash_vectors();if(argc==3)rpm_image_case(argv[1],atoi(argv[2])!=0);else if(getenv("SNAPMAPPLUS_NATIVE_TEST_GL") && getenv("SNAPMAPPLUS_NATIVE_TEST_VK")){rpm_cases(0);rpm_cases(1);}else puts("SKIP manager semantic fixtures: explicit SNAPMAPPLUS_NATIVE_TEST_GL and SNAPMAPPLUS_NATIVE_TEST_VK images required");printf("RenderProg manager reads: %d failure(s)\n",failures);return failures?1:0;}

