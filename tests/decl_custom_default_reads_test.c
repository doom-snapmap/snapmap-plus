/* Reuse the paired common-body fixture and its actual binding setup. */
#define main sh_common_default_test_main
#include "decl_default_reads_test.c"
#undef main
/* RenderProg has its own complete parent/admission and image suites. */
int sh_decl_renderprog_default_read_begin(sh_resource_default_read_receipt *r,
                                         const sh_resource_default_read_input *in)
{ (void)r; (void)in; return 0; }
#define SH_MATERIAL_DEFAULT_FIXTURE_ONLY
#include "decl_material_default_reads_test.c"
#undef SH_MATERIAL_DEFAULT_FIXTURE_ONLY
#define SH_MD6_DEFAULT_FIXTURE_ONLY
#include "decl_md6_default_reads_test.c"
#undef SH_MD6_DEFAULT_FIXTURE_ONLY
#include "../src/backend/decl_string_default_reads.c"
#include "../src/backend/decl_numeric_default_reads.c"
#include "../src/backend/decl_animweb_default_reads.c"
#include "../src/backend/decl_particle_default_reads.c"
int sh_resource_parm_reads_ready(void) { return 1; }
/* These dependency seams test dispatch admission. Their native receiver and
 * callback contracts are qualified in the separate joint/diagnostic suites. */
static int custom_joint_ready=1,custom_diagnostic_ready=1;
static int custom_model_ready=1,custom_metadata_ready=1;
static int custom_material_ready=1;
static int custom_md6_ready=1;
static unsigned custom_diagnostic_severity;
int sh_resource_joint_reads_ready(void) { return custom_joint_ready; }
int sh_resource_model_reads_default_ready(const char *family) { CHECK(!strcmp(family,"particle") || !strcmp(family,"material"));return custom_model_ready; }
int sh_resource_material_reads_ready(void) {return custom_material_ready;}
int sh_resource_md6_reads_ready(void) {return custom_md6_ready;}
int sh_resource_metadata_reads_ready(void) { return custom_metadata_ready; }
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,
                                  const sh_resource_default_read_input *in,unsigned severity)
{
    (void)in; custom_diagnostic_severity=severity;
    if (!custom_diagnostic_ready) {r->reason=SH_DEFAULT_READ_CALLBACK;return 0;}
    return 1;
}
#include "../src/backend/decl_custom_default_reads.c"

static uintptr_t custom_address(fixture *f,unsigned which)
{ return (uintptr_t)f->image+cdr_pins[f->input.renderer][which].rva; }
static int custom_start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    CHECK(!sh_decl_default_read_begin(r,&f->input));
    if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
    return sh_decl_custom_default_read_begin(r,&f->input);
}
static void custom_select(fixture *f,unsigned release)
{
    ptr(f->table,0x98,custom_address(f,release==CDR_FX_RELEASE?CDR_FX_PARSER:
        release==CDR_SKINS_RELEASE?CDR_SKINS_PARSER:
        release==CDR_ENTITY_RELEASE?CDR_ENTITY_PARSER:
        release==CDR_DETAIL_PARSER?CDR_DETAIL_PARSER:
        release==CDR_BREAKABLE_PARSER?CDR_BREAKABLE_PARSER:
        release==CDR_CLOTH_RELEASE?CDR_CLOTH_PARSER:
        release==CDR_FOLIAGE_PARSER?CDR_FOLIAGE_PARSER:
        release==CDR_AF_RELEASE?CDR_AF_PARSER:
        release==CDR_FLAG_RELEASE?CDR_FLAG_PARSER:CDR_COMMON));
    ptr(f->table,0xa0,custom_address(f,release==CDR_DETAIL_PARSER || release==CDR_BREAKABLE_PARSER ||
        release==CDR_FOLIAGE_PARSER?CDR_FLAG_RELEASE:release));
    ptr(f->table,0x88,release==CDR_SKINS_RELEASE?custom_address(f,CDR_SKINS_TEXT):
        release==CDR_ENTITY_RELEASE?custom_address(f,CDR_ENTITY_TEXT):
        release==CDR_DETAIL_PARSER?custom_address(f,CDR_DETAIL_TEXT):
        release==CDR_BREAKABLE_PARSER?custom_address(f,CDR_BREAKABLE_TEXT):
        release==CDR_CLOTH_RELEASE?custom_address(f,CDR_CLOTH_TEXT):
        release==CDR_FOLIAGE_PARSER?custom_address(f,CDR_FOLIAGE_TEXT):
        release==CDR_AF_RELEASE?custom_address(f,CDR_AF_TEXT):
        release==CDR_FLAG_RELEASE?custom_address(f,CDR_FLAG_TEXT):address(f,DDP_TEXT));
}
static void fallback_cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r; int32_t relative; uintptr_t text;
    setup(&f,renderer);
    for(unsigned i=0;i<SDR_PIN_COUNT;i++)
        memcpy(f.image+sdr_pins[renderer][i].rva,sdr_pins[renderer][i].bytes,sdr_pins[renderer][i].length);
    ptr(f.object,8,(uintptr_t)f.image+0x4000); memcpy(f.image+0x4000,"play_control",13);
    ptr(f.table,0x98,(uintptr_t)f.image+sdr_pins[renderer][SDR_PARSER].rva);
    ptr(f.table,0xa0,(uintptr_t)f.image+sdr_pins[renderer][SDR_RELEASE].rva);
    CHECK(custom_start(&f,&r) && r.complete);
    f.denied=(uintptr_t)f.image+0x4000;
    CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY && !r.complete); f.denied=0;
    f.image[sdr_pins[renderer][SDR_POST].rva]^=1;
    CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN && !r.complete);
    f.image[sdr_pins[renderer][SDR_POST].rva]^=1;
    CHECK(custom_start(&f,&r)); CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_custom_default_read_begin(&r,&f.input));
    for(unsigned i=0;i<NDP_PINS;i++)
        memcpy(f.image+ndp_pins[renderer][i].rva,ndp_pins[renderer][i].bytes,ndp_pins[renderer][i].length);
    memcpy(&relative,f.image+ndp_pins[renderer][NDP_TABLE_TEXT].rva+3,4);
    text=(uintptr_t)f.image+ndp_pins[renderer][NDP_TABLE_TEXT].rva+7+relative;
    memcpy((void *)text,ndp_table_definition,sizeof ndp_table_definition);
    ptr(f.table,0x98,(uintptr_t)f.image+ndp_pins[renderer][NDP_TABLE].rva);
    ptr(f.table,0xa0,(uintptr_t)f.image+ndp_pins[renderer][NDP_TABLE_RELEASE].rva);
    ptr(f.table,0x88,(uintptr_t)f.image+ndp_pins[renderer][NDP_TABLE_TEXT].rva);
    ptr(f.object,0x60,(uintptr_t)f.image+0x3000);
    CHECK(custom_start(&f,&r) && r.complete);
    f.denied=(uintptr_t)f.image+0x3000;
    CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY && !r.complete); f.denied=0;
    ptr(f.table,0x98,0x1234);
    CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_UNSUPPORTED && !r.complete);
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void custom_cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r;
    const unsigned releases[]={CDR_FLARE_RELEASE,CDR_VOICE_RELEASE,CDR_WALK_RELEASE,CDR_FX_RELEASE,
        CDR_SKINS_RELEASE,CDR_ENTITY_RELEASE,CDR_FLAG_RELEASE,CDR_DETAIL_PARSER,CDR_BREAKABLE_PARSER,
        CDR_CLOTH_RELEASE,CDR_FOLIAGE_PARSER};
    setup(&f,renderer);
    for (unsigned i=0;i<CDR_PIN_COUNT;i++)
        memcpy((void *)custom_address(&f,i),cdr_pins[renderer][i].bytes,cdr_pins[renderer][i].length);
    {
        int32_t relative;
        memcpy(&relative,(void *)(custom_address(&f,CDR_SKINS_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_SKINS_TEXT)+7+relative),"{\n}\n",5);
        memcpy(&relative,(void *)(custom_address(&f,CDR_ENTITY_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_ENTITY_TEXT)+7+relative),cdr_entity_definition,sizeof cdr_entity_definition);
        memcpy(&relative,(void *)(custom_address(&f,CDR_FLAG_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_FLAG_TEXT)+7+relative),cdr_flag_definition,sizeof cdr_flag_definition);
        memcpy(&relative,(void *)(custom_address(&f,CDR_DETAIL_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_DETAIL_TEXT)+7+relative),cdr_detail_definition,sizeof cdr_detail_definition);
        memcpy(&relative,(void *)(custom_address(&f,CDR_BREAKABLE_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_BREAKABLE_TEXT)+7+relative),cdr_breakable_definition,sizeof cdr_breakable_definition);
        memcpy(&relative,(void *)(custom_address(&f,CDR_CLOTH_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_CLOTH_TEXT)+7+relative),cdr_cloth_definition,sizeof cdr_cloth_definition);
        memcpy(&relative,(void *)(custom_address(&f,CDR_FOLIAGE_TEXT)+3),4);
        memcpy((void *)(custom_address(&f,CDR_FOLIAGE_TEXT)+7+relative),cdr_foliage_definition,sizeof cdr_foliage_definition);
        ptr(f.image,cdr_shape_rows[renderer],(uintptr_t)f.image+cdr_shape_text[renderer]);
        number(f.image,cdr_shape_rows[renderer]+8,0);
        memcpy(f.image+cdr_shape_text[renderer],"autosprites",sizeof "autosprites");
        ptr(f.image,cdr_brace_rows[renderer],(uintptr_t)f.image+cdr_brace_text[renderer]);
        number(f.image,cdr_brace_rows[renderer]+8,47);
        memcpy(f.image+cdr_brace_text[renderer],"}",2);
        ptr(f.image,cdr_token_tables[renderer],custom_address(&f,CDR_TOKEN_DELETE));
        ptr(f.image,cdr_file_tables[renderer],custom_address(&f,CDR_FILE_DELETE));
        ptr(f.image,cdr_file_tables[renderer]+0x30,custom_address(&f,CDR_FILE_WRITE));
        ptr(f.image,cdr_file_tables[renderer]+0x88,custom_address(&f,CDR_FILE_PRINTF));
    }
    CHECK(!sh_resource_default_read_begin(&r,&f.input));
    CHECK(sh_decl_custom_default_token_read_check(&r,&f.input) && !r.complete);
    r.state=2; CHECK(!sh_decl_custom_default_token_read_check(&r,&f.input));
    for (unsigned i=0;i<sizeof releases/sizeof releases[0];i++) {
        size_t field=releases[i]==CDR_VOICE_RELEASE?0x70:releases[i]==CDR_SKINS_RELEASE?0x58:
            releases[i]==CDR_ENTITY_RELEASE || releases[i]==CDR_FLAG_RELEASE ||
            releases[i]==CDR_DETAIL_PARSER || releases[i]==CDR_BREAKABLE_PARSER ||
            releases[i]==CDR_CLOTH_RELEASE || releases[i]==CDR_FOLIAGE_PARSER?0:0x68;
        custom_select(&f,releases[i]);
        f.object[0x128]=1; /* Actual native EntityDef constructor setting. */
        CHECK(custom_start(&f,&r) && r.complete && r.routine==SH_DEFAULT_READ_ROUTINE_DECLARATION);
        CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
        CHECK(!sh_decl_custom_default_read_begin(&r,&f.input));
        CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
        if(field) {
            ptr(f.object,field,0x12345678);
            CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_OWNERSHIP);
            CHECK(r.offending_address==(uintptr_t)f.object+field); ptr(f.object,field,0);
            f.denied=(uintptr_t)f.object+field;
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
        }
        f.object[0x60]=1;
        if(releases[i]==CDR_SKINS_RELEASE || !field) CHECK(custom_start(&f,&r));
        else CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        f.object[0x60]=0;
        ptr(f.table,0xa0,custom_address(&f,releases[i])+1); CHECK(!custom_start(&f,&r));
        custom_select(&f,releases[i]);
        CHECK(custom_start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
        CHECK(custom_start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    }
    {
        const unsigned choices[]={CDR_CLOTH_RELEASE,CDR_FOLIAGE_PARSER};
        const char *const cloth[]={"material","_default","width","height","hspacing","vspacing"};
        const char *const foliage[]={"material","foliage/default","quadWidth","quadHeight","shape","autosprites"};
        uintptr_t node=(uintptr_t)f.image+0x2000,name=(uintptr_t)f.image+0x2200;
        for(unsigned k=0;k<2;k++) {
            custom_select(&f,choices[k]);
            for(unsigned flag=0;flag<6;flag++) {
                ptr(f.object,k?0x88:0xa8,(uintptr_t)f.image+0x5000);
                f.object[k?0x9b:0xbb]=(unsigned char)flag;
                number(f.object,k?0x90:0xb0,23);
                if(!k) {ptr(f.object,0xc0,(uintptr_t)f.image+0x5100);f.object[0xd3]=(unsigned char)flag;number(f.object,0xc8,17);}
                CHECK(custom_start(&f,&r));
            }
            memset(f.object+0x88,0,sizeof f.object-0x88);
            ptr(f.image,ddp_define_heads[renderer],node);
            ptr((void *)node,0x10,name); ptr((void *)node,0x50,0);
            for(unsigned i=0;i<6;i++) {
                const char *blocked=(k?foliage:cloth)[i]; memcpy((void *)name,blocked,strlen(blocked)+1);
                CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP && !r.complete);
            }
            memcpy((void *)name,"MATERIAL",9);CHECK(custom_start(&f,&r));
            ptr(f.image,ddp_define_heads[renderer],0);
        }
        custom_select(&f,CDR_FOLIAGE_PARSER);
        number(f.image,cdr_shape_rows[renderer]+8,1);
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        number(f.image,cdr_shape_rows[renderer]+8,0);
        f.image[cdr_shape_text[renderer]]='x';CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        f.image[cdr_shape_text[renderer]]='a';
        for(unsigned i=CDR_CLOTH_PARSER;i<CDR_AF_PARSER;i++) {
            custom_select(&f,i==CDR_READ_FLOAT || i==CDR_CLOTH_PARSER || i==CDR_CLOTH_RELEASE || i==CDR_CLOTH_TEXT?
                CDR_CLOTH_RELEASE:CDR_FOLIAGE_PARSER);
            *(uint8_t *)custom_address(&f,i)^=1;
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            *(uint8_t *)custom_address(&f,i)^=1;
        }
    }
    custom_select(&f,CDR_BREAKABLE_PARSER);
    {
        uintptr_t brace=(uintptr_t)f.image+cdr_brace_text[renderer];
        const size_t unreachable[]={0x58,0xc8,0xe0,0x128,0x158,0x140,0x188,0x170,0x1a0};
        for(unsigned i=0;i<sizeof unreachable/sizeof unreachable[0];i++) ptr(f.object,unreachable[i],0x12345678);
        CHECK(custom_start(&f,&r)); /* Even reset's populated fields are not reached. */
        for(unsigned i=0;i<sizeof unreachable/sizeof unreachable[0];i++) ptr(f.object,unreachable[i],0);
        number(f.image,cdr_brace_rows[renderer]+8,46);
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        number(f.image,cdr_brace_rows[renderer]+8,47);
        ptr(f.image,cdr_brace_rows[renderer],brace+1);
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,cdr_brace_rows[renderer],brace);
        *(char *)brace='x'; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN); *(char *)brace='}';
        f.denied=brace; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
        for(unsigned i=CDR_BREAKABLE_PARSER;i<=CDR_BREAKABLE_TEXT;i++) {
            *(uint8_t *)custom_address(&f,i)^=1;
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            *(uint8_t *)custom_address(&f,i)^=1;
        }
        {
            int32_t relative; uintptr_t text;
            memcpy(&relative,(void *)(custom_address(&f,CDR_BREAKABLE_TEXT)+3),4);
            text=custom_address(&f,CDR_BREAKABLE_TEXT)+7+relative;
            *(char *)text='x'; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            *(char *)text='{';
        }
    }
    custom_select(&f,CDR_DETAIL_PARSER);
    {
        uintptr_t node=(uintptr_t)f.image+0x2000,name=(uintptr_t)f.image+0x2200;
        const char *blocked[]={"model","_default"}; int32_t relative; uintptr_t text;
        ptr(f.image,ddp_define_heads[renderer],node);
        ptr((void *)node,0x10,name); ptr((void *)node,0x50,0);
        memcpy((void *)name,"MODEL",6); CHECK(custom_start(&f,&r));
        for(unsigned i=0;i<2;i++) {
            memcpy((void *)name,blocked[i],strlen(blocked[i])+1);
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP && !r.complete);
        }
        ptr(f.image,ddp_define_heads[renderer],0);
        for(unsigned i=CDR_DETAIL_PARSER;i<=CDR_DETAIL_TEXT;i++) {
            *(uint8_t *)custom_address(&f,i)^=1;
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            *(uint8_t *)custom_address(&f,i)^=1;
        }
        memcpy(&relative,(void *)(custom_address(&f,CDR_DETAIL_TEXT)+3),4);
        text=custom_address(&f,CDR_DETAIL_TEXT)+7+relative;
        *(char *)text='x'; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(char *)text='{';
    }
    custom_select(&f,CDR_ENTITY_RELEASE);
    f.object[0x128]=0; CHECK(custom_start(&f,&r)); f.object[0x128]=1;
    {
        size_t empty_work; uintptr_t node=(uintptr_t)f.image+0x2000;
        uintptr_t other=(uintptr_t)f.image+0x2100, name=(uintptr_t)f.image+0x2200;
        const char *blocked[]={"inherit","class","edit"};
        CHECK(custom_start(&f,&r)); empty_work=r.work;
        ptr(f.image,ddp_define_heads[renderer],node);
        ptr((void *)node,0x10,name); ptr((void *)node,0x50,other);
        ptr((void *)other,0x10,(uintptr_t)f.image+0x2240); ptr((void *)other,0x50,0);
        memcpy(f.image+0x2240,"inherit_extra",14);
        memcpy((void *)name,"UNRELATED",10); CHECK(custom_start(&f,&r));
        memcpy((void *)name,"INHERIT",8); CHECK(custom_start(&f,&r));
        for(unsigned i=0;i<3;i++) {
            memcpy((void *)name,blocked[i],strlen(blocked[i])+1);
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP && !r.complete);
        }
        memcpy((void *)name,"UNRELATED",10);
        ptr((void *)other,0x50,node); CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        ptr((void *)other,0x50,0);
        f.denied=name; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
        f.input.work_limit=empty_work+2; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);
        f.input.work_limit=0;
        ptr(f.image,ddp_define_heads[renderer],0);
    }
    {
        const uint32_t tables[]={cdr_token_tables[renderer],cdr_file_tables[renderer],
            cdr_file_tables[renderer]+0x30,cdr_file_tables[renderer]+0x88};
        const unsigned functions[]={CDR_TOKEN_DELETE,CDR_FILE_DELETE,CDR_FILE_WRITE,CDR_FILE_PRINTF};
        for(unsigned i=0;i<4;i++) {
            ptr(f.image,tables[i],custom_address(&f,functions[i])+1);
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
            ptr(f.image,tables[i],custom_address(&f,functions[i]));
            f.denied=(uintptr_t)f.image+tables[i];
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
            *(uint8_t *)custom_address(&f,functions[i])^=1;
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            *(uint8_t *)custom_address(&f,functions[i])^=1;
        }
    }
    for(unsigned release=CDR_ENTITY_RELEASE;release<=CDR_FLAG_RELEASE;release+=3) {
        int32_t relative; uintptr_t text;
        custom_select(&f,release);
        for(unsigned i=release-1;i<=release+1;i++) {
            *(uint8_t *)custom_address(&f,i)^=1;
            CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            *(uint8_t *)custom_address(&f,i)^=1;
        }
        memcpy(&relative,(void *)(custom_address(&f,release+1)+3),4);
        text=custom_address(&f,release+1)+7+relative;
        *(char *)text='x'; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(char *)text='{';
    }
    custom_select(&f,CDR_SKINS_RELEASE);
    for (unsigned i=CDR_SKINS_PARSER;i<=CDR_SKINS_TEXT;i++) {
        *(uint8_t *)custom_address(&f,i)^=1;
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(uint8_t *)custom_address(&f,i)^=1;
    }
    {
        int32_t relative; uintptr_t text;
        memcpy(&relative,(void *)(custom_address(&f,CDR_SKINS_TEXT)+3),4);
        text=custom_address(&f,CDR_SKINS_TEXT)+7+relative;
        *(char *)text='x'; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(char *)text='{';
    }
    ptr(f.table,0x88,address(&f,DDP_TEXT)); CHECK(!custom_start(&f,&r));
    custom_select(&f,CDR_FX_RELEASE);
    ptr(f.object,0x80,0x1234); CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.object,0x80,0);
    /* Both native counts are reset before their post loops. They are not
     * input-emptiness predicates when the two cleanup pointers are NULL. */
    number(f.object,0x70,7); number(f.object,0x88,9); CHECK(custom_start(&f,&r));
    number(f.object,0x70,0); number(f.object,0x88,0);
    ptr(f.table,0x98,custom_address(&f,CDR_COMMON)); CHECK(!custom_start(&f,&r));
    ptr(f.table,0x98,custom_address(&f,CDR_FX_PARSER));
    ptr(f.table,0xa0,custom_address(&f,CDR_FLARE_RELEASE)); CHECK(!custom_start(&f,&r));
    custom_select(&f,CDR_FX_RELEASE);
    for (unsigned i=CDR_FX_PARSER;i<=CDR_FX_POST;i++) {
        *(uint8_t *)custom_address(&f,i)^=1;
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(uint8_t *)custom_address(&f,i)^=1;
    }
    custom_select(&f,CDR_FLARE_RELEASE);
    for (unsigned i=CDR_FLARE_RELEASE;i<=CDR_WALK_RELEASE;i++) {
        custom_select(&f,i); *(uint8_t *)custom_address(&f,i)^=1;
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(uint8_t *)custom_address(&f,i)^=1;
    }
    custom_select(&f,CDR_FLARE_RELEASE);
    f.input.work_limit=1; CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);
    f.input.work_limit=0;
    {
        const ddp_context_binding *b=ddp_context_profiles+renderer;
        for(int cold=-1;cold<=0;cold++)for(int event=0;event<=1;event++) {
            ddp_test_engine_state(&f,cold,event);
            CHECK(custom_start(&f,&r) && r.complete);
        }
        ptr(f.image,der_profiles[renderer].sys,0);
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ddp_test_engine_state(&f,-2,0);
        f.image[b->functions[1].rva]^=1;
        CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        f.image[b->functions[1].rva]^=1;
    }
    CHECK(!sh_resource_default_read_begin(&r,&f.input));
    CHECK(sh_decl_default_common_body_read_check(&r,&f.input) && !r.complete);
    f.input.read_context=NULL;
    CHECK(!sh_decl_custom_default_read_begin(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);
    f.input.read_context=&f;
    f.input.pinned_image=0; CHECK(!custom_start(&f,&r)); f.input.pinned_image=1;
    f.input.renderer=1-renderer; CHECK(!custom_start(&f,&r)); f.input.renderer=renderer;
    f.input.image_size=0x1000; CHECK(!custom_start(&f,&r)); f.input.image_size=f.size;
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void af_cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r; int32_t relative; uintptr_t text;
    uintptr_t node,name; const size_t mutations[]={0,127,128,sizeof cdr_af_definition-2,sizeof cdr_af_definition-1};
    setup(&f,renderer);
    for(unsigned i=0;i<CDR_PIN_COUNT;i++)
        memcpy((void *)custom_address(&f,i),cdr_pins[renderer][i].bytes,cdr_pins[renderer][i].length);
    memcpy(&relative,(void *)(custom_address(&f,CDR_AF_TEXT)+3),4);
    text=custom_address(&f,CDR_AF_TEXT)+7+relative;
    memcpy((void *)text,cdr_af_definition,sizeof cdr_af_definition);
    custom_select(&f,CDR_AF_RELEASE);
    custom_joint_ready=custom_diagnostic_ready=1;custom_diagnostic_severity=0;
    CHECK(custom_start(&f,&r) && r.complete && custom_diagnostic_severity==4);
    custom_joint_ready=0;
    CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
    custom_joint_ready=1;custom_diagnostic_ready=0;
    CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
    custom_diagnostic_ready=1;
    for(unsigned flag=0;flag<6;flag++) {
        ptr(f.object,0x140,(uintptr_t)f.image+0x5000);number(f.object,0x148,1);number(f.object,0x14c,1);
        f.object[0x153]=(unsigned char)flag;ptr(f.image,0x5000,(uintptr_t)f.image+0x6000);
        ptr(f.image,0x6150,(uintptr_t)f.image+0x6800);number(f.image,0x6158,3);f.image[0x6163]=(unsigned char)flag;
        ptr(f.object,0x158,(uintptr_t)f.image+0x5100);number(f.object,0x160,1);number(f.object,0x164,1);
        f.object[0x16b]=(unsigned char)flag;ptr(f.image,0x5100,(uintptr_t)f.image+0x7000);
        CHECK(custom_start(&f,&r) && r.complete);
    }
    node=(uintptr_t)f.image+0x2000;name=(uintptr_t)f.image+0x2200;
    ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);ptr((void *)node,0x50,0);
    for(unsigned i=0;i<sizeof cdr_af_names/sizeof cdr_af_names[0];i++) {
        memcpy((void *)name,cdr_af_names[i],strlen(cdr_af_names[i])+1);
        CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    }
    memcpy((void *)name,"ORIGIN",7);CHECK(custom_start(&f,&r));
    memcpy((void *)name,"*origin",8);CHECK(custom_start(&f,&r)); /* Quoted; raw Script has no defines. */
    ptr(f.image,ddp_define_heads[renderer],0);
    for(unsigned i=CDR_AF_PARSER;i<CDR_PIN_COUNT;i++) {
        unsigned char *at=(unsigned char *)custom_address(&f,i);*at^=1;
        CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_PIN);*at^=1;
    }
    for(unsigned i=0;i<sizeof mutations/sizeof mutations[0];i++) {
        unsigned char *at=(unsigned char *)text+mutations[i];*at^=1;
        CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_PIN);*at^=1;
    }
    f.denied=text+128;
    CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    CHECK(custom_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(custom_start(&f,&r));CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_custom_default_read_begin(&r,&f.input));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void custom_image_case(const char *path,int renderer)
{
    FILE *file=fopen(path,"rb"); uint8_t *raw; long length;
    IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *section;
    image_case(path,renderer);
    CHECK(file); if(!file)return;
    fseek(file,0,SEEK_END); length=ftell(file); rewind(file);
    raw=malloc(length); CHECK(raw); if(!raw){fclose(file);return;}
    CHECK(fread(raw,1,length,file)==(size_t)length); fclose(file);
    nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    section=IMAGE_FIRST_SECTION(nt);
    {
        const size_t locations[]={cdr_brace_rows[renderer],cdr_brace_text[renderer],cdr_shape_rows[renderer],cdr_shape_text[renderer]},sizes[]={12,2,12,sizeof "autosprites"};
        for(unsigned i=0;i<4;i++) {
            int found=0;
            for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
                size_t rva=section[j].VirtualAddress,size=section[j].SizeOfRawData;
                if(locations[i]>=rva && locations[i]-rva<=size && sizes[i]<=size-(locations[i]-rva)) {
                    size_t offset=section[j].PointerToRawData+locations[i]-rva;
                    CHECK(offset<=(size_t)length && sizes[i]<=(size_t)length-offset);
                    if(!(i&1)) {
                        uintptr_t actual; unsigned subtype;
                        memcpy(&actual,raw+offset,8); memcpy(&subtype,raw+offset+8,4);
                        CHECK(actual==nt->OptionalHeader.ImageBase+(i?cdr_shape_text[renderer]:cdr_brace_text[renderer]) && subtype==(i?0:47));
                    } else CHECK(!memcmp(raw+offset,i==1?"}":"autosprites",sizes[i]));
                    found=1;break;
                }
            }
            CHECK(found);
        }
    }
    for (unsigned i=0;i<CDR_PIN_COUNT;i++) {
        const cdr_pin *pin=&cdr_pins[renderer][i]; int found=0;
        for (unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            size_t rva=section[j].VirtualAddress,size=section[j].SizeOfRawData;
            if(pin->rva>=rva && pin->rva-rva<=size && pin->length<=size-(pin->rva-rva)) {
                size_t offset=section[j].PointerToRawData+(pin->rva-rva);
                CHECK(offset<=(size_t)length && pin->length<=(size_t)length-offset);
                CHECK(!memcmp(raw+offset,pin->bytes,pin->length)); found=1; break;
            }
        }
        CHECK(found);
    }
    {
        const uint32_t tables[]={cdr_token_tables[renderer],cdr_file_tables[renderer],
            cdr_file_tables[renderer]+0x30,cdr_file_tables[renderer]+0x88};
        const unsigned functions[]={CDR_TOKEN_DELETE,CDR_FILE_DELETE,CDR_FILE_WRITE,CDR_FILE_PRINTF};
        for(unsigned i=0;i<4;i++) {
            int found=0;
            for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
                size_t rva=section[j].VirtualAddress,size=section[j].SizeOfRawData;
                if(tables[i]>=rva && tables[i]-rva<=size && 8<=size-(tables[i]-rva)) {
                    size_t offset=section[j].PointerToRawData+(tables[i]-rva); uintptr_t actual;
                    CHECK(offset<=(size_t)length && 8<=(size_t)length-offset);
                    memcpy(&actual,raw+offset,8);
                    CHECK(actual==nt->OptionalHeader.ImageBase+cdr_pins[renderer][functions[i]].rva);
                    found=1; break;
                }
            }
            CHECK(found);
        }
    }
    {
        const unsigned getters[]={CDR_SKINS_TEXT,CDR_ENTITY_TEXT,CDR_FLAG_TEXT,CDR_DETAIL_TEXT,CDR_BREAKABLE_TEXT,CDR_CLOTH_TEXT,CDR_FOLIAGE_TEXT,CDR_AF_TEXT};
        const char *definitions[]={"{\n}\n",cdr_entity_definition,cdr_flag_definition,cdr_detail_definition,cdr_breakable_definition,cdr_cloth_definition,cdr_foliage_definition,cdr_af_definition};
        const size_t lengths[]={5,sizeof cdr_entity_definition,sizeof cdr_flag_definition,sizeof cdr_detail_definition,sizeof cdr_breakable_definition,sizeof cdr_cloth_definition,sizeof cdr_foliage_definition,sizeof cdr_af_definition};
        for(unsigned i=0;i<sizeof getters/sizeof getters[0];i++) {
            const cdr_pin *pin=&cdr_pins[renderer][getters[i]]; int32_t relative; int found=0;
            int64_t target;
            memcpy(&relative,pin->bytes+3,4); target=(int64_t)pin->rva+7+relative;
            CHECK(target>=0 && (uint64_t)target<nt->OptionalHeader.SizeOfImage);
            for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
                size_t rva=section[j].VirtualAddress,size=section[j].SizeOfRawData;
                if((uint64_t)target>=rva && (uint64_t)target-rva<=size && lengths[i]<=size-((uint64_t)target-rva)) {
                    size_t offset=section[j].PointerToRawData+((size_t)target-rva);
                    CHECK(offset<=(size_t)length && lengths[i]<=(size_t)length-offset);
                    CHECK(!memcmp(raw+offset,definitions[i],lengths[i])); found=1; break;
                }
            }
            CHECK(found);
        }
    }
    free(raw);
    printf("custom declaration defaults: %s native image pins verified\n",renderer?"Vulkan":"OpenGL");
}
static void connected_sibling_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;int32_t relative;uintptr_t getter,text;
    setup(&f,renderer);
    for(unsigned i=0;i<AW_PINS;i++)memcpy(f.image+aw_pins[renderer][i].rva,aw_pins[renderer][i].bytes,aw_pins[renderer][i].length);
    getter=(uintptr_t)f.image+aw_pins[renderer][AW_TEXT].rva;memcpy(&relative,(void *)(getter+3),4);text=getter+7+relative;
    memcpy((void *)text,aw_definition,sizeof aw_definition);
    ptr(f.table,0x98,(uintptr_t)f.image+aw_pins[renderer][AW_PARSE].rva);
    ptr(f.table,0xa0,(uintptr_t)f.image+aw_pins[renderer][AW_RELEASE].rva);ptr(f.table,0x88,getter);
    CHECK(custom_start(&f,&r) && r.complete);CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    f.object[0x15a]=1;custom_diagnostic_severity=0;
    CHECK(custom_start(&f,&r) && custom_diagnostic_severity==1);
    custom_diagnostic_ready=0;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);custom_diagnostic_ready=1;
    number(f.object,0x60,1);CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.object,0x60,0);
    CHECK(custom_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    VirtualFree(f.image,0,MEM_RELEASE);

    setup(&f,renderer);
    for(unsigned i=0;i<PDR_PIN_COUNT;i++)memcpy(f.image+pdr_pins[renderer][i].rva,pdr_pins[renderer][i].bytes,pdr_pins[renderer][i].length);
    getter=(uintptr_t)f.image+pdr_pins[renderer][PDR_TEXT].rva;memcpy(&relative,(void *)(getter+3),4);text=getter+7+relative;
    memcpy((void *)text,pdr_definition,sizeof pdr_definition);
    ptr(f.table,0x98,(uintptr_t)f.image+pdr_pins[renderer][PDR_PARSER].rva);
    ptr(f.table,0xa0,(uintptr_t)f.image+pdr_pins[renderer][PDR_RELEASE].rva);ptr(f.table,0x88,getter);
    static const char *const names[]={"none","constant","minmax","curve","curve_scale_bias","curve_use_variance_mod_constant","curve_mod_curve","curve_add_curve","parametricEval","parametricIntegrate","parametricIntegrateMinMax"};
    const uint32_t rows[]={0x36f9190,0x4df6bf0},holder[]={0x36fb530,0x4df9100};
    const uint32_t dependency[]={0x17a4f70,0x17b24b0},parent[]={0x17a5190,0x17b26d0};
    for(unsigned i=0;i<11;i++){strcpy((char *)f.image+0x3000+i*64,names[i]);ptr(f.image,rows[renderer]+i*8,(uintptr_t)f.image+0x3000+i*64);}
    ptr(f.image,holder[renderer],(uintptr_t)f.image+0x5000);ptr(f.image,0x5000,(uintptr_t)f.image+0x5100);
    ptr(f.image,0x5190,(uintptr_t)f.image+dependency[renderer]);ptr(f.image,0x5198,(uintptr_t)f.image+parent[renderer]);
    custom_diagnostic_severity=0;CHECK(custom_start(&f,&r) && r.complete && custom_diagnostic_severity==4);
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));CHECK(!sh_decl_custom_default_read_begin(&r,&f.input));
    custom_model_ready=0;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);custom_model_ready=1;
    custom_metadata_ready=0;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);custom_metadata_ready=1;
    custom_diagnostic_ready=0;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);custom_diagnostic_ready=1;
    f.denied=text;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    CHECK(custom_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    VirtualFree(f.image,0,MEM_RELEASE);

    material_setup(&f,renderer);
    custom_diagnostic_severity=0;CHECK(custom_start(&f,&r) && r.complete && custom_diagnostic_severity==4);
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    custom_material_ready=0;CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);custom_material_ready=1;
    custom_model_ready=0;CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);custom_model_ready=1;
    custom_diagnostic_ready=0;CHECK(!custom_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);custom_diagnostic_ready=1;
    uintptr_t video=material_object(&f,material_key("videoMap")),value=(uintptr_t)f.image+0x13180000;
    ptr((void *)video,0x58,value);strcpy((char *)value,"video.bik");
    CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    *(char *)value=0;CHECK(custom_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    VirtualFree(f.image,0,MEM_RELEASE);
}

static void connected_md6_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;uintptr_t text=mdp_setup(&f,renderer);
    custom_diagnostic_severity=0;
    CHECK(custom_start(&f,&r) && r.complete && custom_diagnostic_severity==4);
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_custom_default_read_begin(&r,&f.input));
    md6_populated(&f);CHECK(custom_start(&f,&r) && r.complete);
    custom_md6_ready=0;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);custom_md6_ready=1;
    custom_diagnostic_ready=0;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);custom_diagnostic_ready=1;
    f.denied=text;CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    f.image[mdp_pins[renderer][MDP_PARSER].rva]^=1;
    CHECK(!custom_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_PIN);
    f.image[mdp_pins[renderer][MDP_PARSER].rva]^=1;
    CHECK(custom_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(custom_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    VirtualFree(f.image,0,MEM_RELEASE);
}
int main(int argc,char **argv)
{
    if(argc>2)custom_image_case(argv[1],atoi(argv[2])!=0);
    else {custom_cases(0);custom_cases(1);fallback_cases(0);fallback_cases(1);af_cases(0);af_cases(1);connected_sibling_cases(0);connected_sibling_cases(1);connected_md6_cases(0);connected_md6_cases(1);}
    printf("custom declaration defaults: %d failure(s)\n",failures); return failures?1:0;
}
