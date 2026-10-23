#ifndef SH_MATERIAL_DEFAULT_FIXTURE_ONLY
#define main sh_material_common_test_main
#include "decl_default_reads_test.c"
#undef main
#endif
#include "../src/backend/native_resource_id.c"
#include "../src/backend/decl_material_default_reads.c"
#ifndef SH_MATERIAL_DEFAULT_FIXTURE_ONLY
static int material_ready=1,model_ready=1,parm_ready=1,diag1_ready=1,diag4_ready=1;
int sh_resource_material_reads_ready(void){return material_ready;}
int sh_resource_model_reads_default_ready(const char *family){CHECK(!strcmp(family,"material"));return model_ready;}
int sh_resource_parm_reads_ready(void){return parm_ready;}
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,unsigned severity)
{
    CHECK(in && !r->complete && (severity==1 || severity==4));
    int ready=severity==1?diag1_ready:diag4_ready;
    if(!ready)r->reason=SH_DEFAULT_READ_CALLBACK;return ready;
}
#endif
static uintptr_t material_address(fixture *f,unsigned i)
{return (uintptr_t)f->image+mdr_pins[f->input.renderer][i].rva;}
static unsigned material_key(const char *name)
{for(unsigned i=0;i<MDR_PARAMETERS;i++)if(!strcmp(mdr_parameters[i].name,name))return i;CHECK(0);return 0;}
static uintptr_t material_object(fixture *f,unsigned i){return (uintptr_t)f->image+0x13020000+i*0x100;}
static uintptr_t material_bind(fixture *f,int renderer)
{
    for(unsigned i=0;i<MDR_PINS;i++)memcpy((void *)material_address(f,i),mdr_pins[renderer][i].bytes,mdr_pins[renderer][i].length);
    ptr(f->table,0x98,material_address(f,MDR_PARSE));ptr(f->table,0xa0,material_address(f,MDR_RELEASE));
    ptr(f->table,0x88,material_address(f,MDR_TEXT));ptr(f->table,0x70,material_address(f,MDR_MANAGER));
    int32_t relative;memcpy(&relative,(void *)(material_address(f,MDR_TEXT)+3),4);
    uintptr_t text=material_address(f,MDR_TEXT)+7+relative;memcpy((void *)text,mdr_definition,sizeof mdr_definition);
    uintptr_t manager=(uintptr_t)f->image+0x13050000,list=(uintptr_t)f->image+mdr_lists[renderer];
    uintptr_t array=(uintptr_t)f->image+0x13080000,buckets=array+0x1000,next=array+0x2000,type=array+0x3000;
    strcpy((char *)type,"renderParm");ptr((void *)list,8,type);ptr((void *)list,0x20,array);number((void *)list,0x28,MDR_PARAMETERS);
    ptr((void *)list,0x38,buckets);ptr((void *)list,0x40,next);number((void *)list,0x54,255);number((void *)list,0x58,65535);
    memset((void *)buckets,255,256*4);memset((void *)next,255,MDR_PARAMETERS*4);ptr(f->image,mdr_holders[renderer],manager);
    int vector=0,texture=0x400,program=0x680,string=0x800;
    for(unsigned i=0;i<MDR_PARAMETERS;i++) {
        const mdr_parameter *k=mdr_parameters+i;int kind=k->kind<0?(k->kind==-2?1:0):k->kind;
        int index=kind==0?vector++:kind==1?texture++:kind==11?program++:string++;
        uintptr_t object=material_object(f,i),name=object+0xa0;
        ptr(f->image,k->slot[renderer],object);ptr((void *)object,0,(uintptr_t)f->image+mdr_tables[renderer]);
        ptr((void *)object,8,name);ptr((void *)object,0x10,list);strcpy((char *)name,k->name);
        number((void *)object,0x68,index);number((void *)object,0x6c,kind);ptr((void *)manager,(size_t)index*8,object);
        uint32_t id,head;CHECK(sh_native_resource_id("renderParm",k->name,&id));number((void *)object,0x20,(int)id);
        memcpy(&head,(void *)(buckets+(id&255)*4),4);number((void *)next,i*4,(int)head);number((void *)buckets,(id&255)*4,(int)i);
        ptr((void *)array,i*8,object);
    }
    return text;
}
static uintptr_t material_setup(fixture *f,int renderer)
{setup(f,renderer);return material_bind(f,renderer);}
#ifndef SH_MATERIAL_DEFAULT_FIXTURE_ONLY
static int material_start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    CHECK(!sh_decl_default_read_begin(r,&f->input));
    return sh_decl_material_default_read_begin(r,&f->input);
}
static void material_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;uintptr_t text=material_setup(&f,renderer);
    CHECK(material_start(&f,&r) && r.complete && r.routine==SH_DEFAULT_READ_ROUTINE_DECLARATION);
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));CHECK(!sh_decl_material_default_read_begin(&r,&f.input));
    CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    /* Own arrays and optional buffer may be populated, including native
     * borrowed flags. Their pointees are never external resource inputs. */
    ptr(f.object,0x70,(uintptr_t)f.image+0x13100000);number(f.object,0x78,7);number(f.object,0x7c,8);
    ptr(f.object,0x88,(uintptr_t)f.image+0x13110000);number(f.object,0x90,7);number(f.object,0x94,8);
    ptr(f.object,0xa0,(uintptr_t)f.image+0x13120000);number(f.object,0xa8,9);number(f.object,0xac,10);
    ptr(f.object,0x168,(uintptr_t)f.image+0x13130000);
    for(unsigned flag=0;flag<256;flag++){f.object[0x83]=(unsigned char)flag;f.object[0x9b]=(unsigned char)flag;f.image[0x13130053]=(unsigned char)flag;CHECK(material_start(&f,&r));}
    for(unsigned i=0;i<MDR_PINS;i++)if(i!=MDR_STRING)for(unsigned j=0;j<mdr_pins[renderer][i].length;j++){
        *(unsigned char *)(material_address(&f,i)+j)^=1;CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(unsigned char *)(material_address(&f,i)+j)^=1;
    }
    *(char *)(text+9)^=1;CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*(char *)(text+9)^=1;
    uintptr_t trans=material_object(&f,material_key("transMap"));
    for(int kind=1;kind<=9;kind++){number((void *)trans,0x6c,kind);CHECK(material_start(&f,&r));}
    number((void *)trans,0x6c,1);
    uintptr_t node=(uintptr_t)f.image+0x13140000,name=node+0x100;
    ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);
    const char *keys[]={"transMap","_default","stageProgram","guiBlend"};
    for(unsigned i=0;i<4;i++){strcpy((char *)name,keys[i]);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);}
    strcpy((char *)name,"TRANSMAP");CHECK(material_start(&f,&r));ptr(f.image,ddp_define_heads[renderer],0);
    uintptr_t list_at=(uintptr_t)f.image+mdr_lists[renderer],buckets=(uintptr_t)f.image+0x13081000,next=(uintptr_t)f.image+0x13082000;
    uintptr_t first=material_object(&f,0);uint32_t first_id,head;
    memcpy(&first_id,(void *)(first+0x20),4);memcpy(&head,(void *)(buckets+(first_id&255)*4),4);
    number((void *)buckets,(first_id&255)*4,-1);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number((void *)buckets,(first_id&255)*4,(int)head);
    number((void *)list_at,0x28,0);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number((void *)list_at,0x28,MDR_PARAMETERS);
    /* A self-cycle with a different ID cannot establish cached membership,
     * even though the desired object remains in the resource array. */
    uint32_t other_id,old_next;uintptr_t other=material_object(&f,1);
    memcpy(&other_id,(void *)(other+0x20),4);CHECK(other_id!=first_id);memcpy(&old_next,(void *)(next+4),4);
    number((void *)buckets,(first_id&255)*4,1);number((void *)next,4,1);
    CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number((void *)next,4,(int)old_next);number((void *)buckets,(first_id&255)*4,(int)head);
    uintptr_t type=(uintptr_t)f.image+0x13083000;*(char *)type='R';CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_TYPE);*(char *)type='r';
    uintptr_t old;
    for(unsigned i=0;i<MDR_PARAMETERS;i++){
        uintptr_t object=material_object(&f,i);int kind;memcpy(&kind,(void *)(object+0x6c),4);
        number((void *)object,0x6c,99);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_TYPE);number((void *)object,0x6c,kind);
        memcpy(&old,(void *)(object+0x10),8);ptr((void *)object,0x10,old+1);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr((void *)object,0x10,old);
        uint32_t id;memcpy(&id,(void *)(object+0x20),4);number((void *)object,0x20,(int)(id+1));CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number((void *)object,0x20,(int)id);
        f.denied=object+0x68;CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    }
    uintptr_t video=material_object(&f,material_key("videoMap")),value=(uintptr_t)f.image+0x13150000;
    ptr((void *)video,0x58,value);CHECK(material_start(&f,&r));
    strcpy((char *)value,"video.bik");CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    *(char *)value=0;CHECK(material_start(&f,&r));ptr((void *)video,0x58,0);
    ptr(f.object,0x198,value);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_UNSUPPORTED);ptr(f.object,0x198,0);
    /* Other string defaults remain actual native Find/platform inputs. */
    ptr((void *)material_object(&f,material_key("lightColorAnimation_Table")),0x58,value);strcpy((char *)value,"tables/any/name");CHECK(material_start(&f,&r));
    ptr((void *)material_object(&f,material_key("landPageFile")),0x58,value);CHECK(material_start(&f,&r));
    /* The native temporary count is128; no header count or max-index guess. */
    uintptr_t manager=(uintptr_t)f.image+0x13050000,list=(uintptr_t)f.image+mdr_lists[renderer];
    for(unsigned i=0;i<128;i++){
        uintptr_t object=(uintptr_t)f.image+0x13160000+i*0x100;int index=0x200+i;
        ptr(f.image,mdr_temporaries[renderer]+i*8,object);ptr((void *)object,0,(uintptr_t)f.image+mdr_tables[renderer]);
        ptr((void *)object,0x10,list);number((void *)object,0x68,index);number((void *)object,0x6c,0);number((void *)object,0x74,2);ptr((void *)manager,index*8,object);
    }
    CHECK(material_start(&f,&r));uintptr_t last=(uintptr_t)f.image+0x13160000+127*0x100;
    number((void *)last,0x74,1);CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number((void *)last,0x74,2);
    ptr(f.image,mdr_temporaries[renderer]+127*8,0);CHECK(!material_start(&f,&r));ptr(f.image,mdr_temporaries[renderer]+127*8,last);
    int *ready[]={&material_ready,&model_ready,&parm_ready,&diag1_ready,&diag4_ready};
    for(unsigned i=0;i<5;i++){*ready[i]=0;CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);*ready[i]=1;}
    CHECK(material_start(&f,&r));size_t work=r.work;f.input.work_limit=work-1;CHECK(!material_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(material_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(material_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(!sh_resource_default_read_begin(&r,&f.input));f.input.read_context=NULL;CHECK(!sh_decl_material_default_read_begin(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.read_context=&f;
    VirtualFree(f.image,0,MEM_RELEASE);
}
static const unsigned char *material_raw(const unsigned char *raw,size_t size,uint32_t rva,size_t length)
{
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++)if(rva>=s[i].VirtualAddress && rva-s[i].VirtualAddress<=s[i].SizeOfRawData && length<=s[i].SizeOfRawData-(rva-s[i].VirtualAddress)){
        size_t off=s[i].PointerToRawData+rva-s[i].VirtualAddress;if(off<=size && length<=size-off)return raw+off;
    }CHECK(0);return NULL;
}
static void material_image(const char *path,int renderer)
{
    image_case(path,renderer);FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *raw=malloc(size);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,size,f)==(size_t)size);fclose(f);
    for(unsigned i=0;i<MDR_PINS;i++){const mdr_pin *p=mdr_pins[renderer]+i;const unsigned char *data=material_raw(raw,size,p->rva,p->length);CHECK(data && !memcmp(data,p->bytes,p->length));}
    int32_t rel;memcpy(&rel,mdr_pins[renderer][MDR_TEXT].bytes+3,4);
    const unsigned char *data=material_raw(raw,size,mdr_pins[renderer][MDR_TEXT].rva+7+rel,sizeof mdr_definition);CHECK(data && !memcmp(data,mdr_definition,sizeof mdr_definition));
    free(raw);
}
int main(int argc,char **argv)
{if(argc==3)material_image(argv[1],atoi(argv[2]));else{material_cases(0);material_cases(1);}if(!failures)puts("decl_material_default_reads_test: PASS");return failures?1:0;}

#endif
