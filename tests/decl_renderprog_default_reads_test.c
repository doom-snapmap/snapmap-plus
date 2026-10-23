#include "native_pin_fixture.h"
#define main rpd_common_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_renderprog_default_reads.c"
static unsigned rpd_failure,rpd_seen;
static int rpd_adjunct(unsigned bit,sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{CHECK(r && in && r->state==1 && !r->complete && r->reason==SH_DEFAULT_READ_UNSUPPORTED);rpd_seen|=bit;if(rpd_failure&bit){r->reason=SH_DEFAULT_READ_CALLBACK;return 0;}return 1;}
#define RPD_ADJUNCT(name,bit) int name(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in){return rpd_adjunct(bit,r,in);}
RPD_ADJUNCT(sh_decl_source_preprocess_read_check,1)
RPD_ADJUNCT(sh_decl_renderprog_file_read_check,2)
RPD_ADJUNCT(sh_decl_renderprog_context_read_check,4)
RPD_ADJUNCT(sh_decl_renderprog_cvar_read_check,8)
RPD_ADJUNCT(sh_decl_renderprog_release_read_check,16)
RPD_ADJUNCT(sh_decl_renderprog_backend_read_check,32)
RPD_ADJUNCT(sh_decl_renderprog_manager_read_check,64)
int sh_resource_renderprog_reads_ready(void){return !(rpd_failure&128);}
int sh_resource_parm_reads_ready(void){return !(rpd_failure&256);}
int sh_resource_graph_native_decl_flow_ready(uintptr_t parse,uintptr_t text,uintptr_t compile)
{CHECK(parse && text && compile);return !(rpd_failure&512);}
static uintptr_t rpd_setup(fixture *f,int renderer)
{
 setup(f,renderer);const rpd_profile *b=rpd_profiles+renderer;
 for(unsigned i=0;i<rpd_pin_counts[renderer];i++){const rpd_pin *pin=rpd_pins[renderer]+i;CHECK(sh_native_pin_fixture_copy(renderer,pin->rva,f->image+pin->rva,pin->length));}
 ptr(f->table,0x98,(uintptr_t)f->image+b->parse);ptr(f->table,0x88,(uintptr_t)f->image+b->getter);
 int32_t relative;memcpy(&relative,f->image+b->getter+3,4);uintptr_t text=(uintptr_t)f->image+b->getter+7+relative;
 CHECK(sh_native_pin_fixture_copy(renderer,renderer?0x222e090:0x2246310,(void *)text,RPD_DEFINITION_LENGTH));
 ptr(f->image,b->holder,(uintptr_t)f->image+b->object);ptr(f->image,b->object,(uintptr_t)f->image+b->table);
 ptr(f->image,b->table+0xb0,(uintptr_t)f->image+b->setter);return text;
}
static int rpd_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));rpd_seen=0;return sh_decl_renderprog_default_read_begin(r,&f->input);}
static void rpd_cases(int renderer)
{
 fixture f;sh_resource_default_read_receipt r;uintptr_t text=rpd_setup(&f,renderer);const rpd_profile *b=rpd_profiles+renderer;
 CHECK(rpd_start(&f,&r) && r.complete && rpd_seen==127);CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
 for(unsigned bit=1;bit<=512;bit<<=1){rpd_failure=bit;CHECK(!rpd_start(&f,&r) && !r.complete);}rpd_failure=0;
 for(unsigned i=0;i<rpd_pin_counts[renderer];i++){const rpd_pin *pin=rpd_pins[renderer]+i;size_t offsets[]={0,pin->length/2,pin->length-1};for(unsigned j=0;j<3;j++){size_t at=pin->rva+offsets[j];f.image[at]^=1;CHECK(!rpd_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);f.image[at]^=1;}}
 *(char *)(text+20)^=1;CHECK(!rpd_start(&f,&r));*(char *)(text+20)^=1;
 ptr(f.image,b->table+0xb0,(uintptr_t)f.image+b->setter+1);CHECK(!rpd_start(&f,&r));ptr(f.image,b->table+0xb0,(uintptr_t)f.image+b->setter);
 for(unsigned i=0;i<3;i++){number(f.image,b->object+0x2c34,i==2?-1:(int)i);CHECK(rpd_start(&f,&r));}
 f.denied=(uintptr_t)f.image+b->object+0x2c34;CHECK(!rpd_start(&f,&r));f.denied=0;
 f.change_at=(uintptr_t)f.image+b->holder;CHECK(!rpd_start(&f,&r));f.change_at=0;ptr(f.image,b->holder,(uintptr_t)f.image+b->object);
 CHECK(rpd_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!rpd_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
 CHECK(!sh_resource_default_read_begin(&r,&f.input));f.input.resource++;CHECK(!sh_decl_renderprog_default_read_begin(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
 CHECK(rpd_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
 CHECK(rpd_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
 CHECK(rpd_start(&f,&r));CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
 VirtualFree(f.image,0,MEM_RELEASE);
}
static void rpd_image(const char *path,int renderer)
{
 FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);
 unsigned char *raw=malloc(length);CHECK(raw);if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
 IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
 uint32_t literal_rva=renderer?0x222e090:0x2246310;int literal_found=0;
 for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
  IMAGE_SECTION_HEADER *s=sections+i;
  if(literal_rva>=s->VirtualAddress && literal_rva-s->VirtualAddress<=s->SizeOfRawData && RPD_DEFINITION_LENGTH<=s->SizeOfRawData-(literal_rva-s->VirtualAddress)) {
   size_t at=s->PointerToRawData+literal_rva-s->VirtualAddress;
   CHECK(at<=(size_t)length && RPD_DEFINITION_LENGTH<=(size_t)length-at);
   if(at<=(size_t)length && RPD_DEFINITION_LENGTH<=(size_t)length-at)CHECK(sh_native_pin_fixture_digest(raw+at,RPD_DEFINITION_LENGTH,rpd_definition_hash));
   literal_found=1;break;
  }
 }
 CHECK(literal_found);
 for(unsigned n=0;n<rpd_pin_counts[renderer];n++){
  const rpd_pin *pin=rpd_pins[renderer]+n;int found=0;
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++){IMAGE_SECTION_HEADER *s=sections+i;if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
   size_t at=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(at<=(size_t)length && pin->length<=(size_t)length-at);if(at<=(size_t)length && pin->length<=(size_t)length-at)CHECK(sh_native_pin_fixture_digest(raw+at,pin->length,pin->hash));found=1;break;}}
  CHECK(found);
 }
 free(raw);
}
int main(int argc,char **argv)
{if(argc>2)rpd_image(argv[1],atoi(argv[2]));else{rpd_cases(0);rpd_cases(1);}return failures?1:0;}
