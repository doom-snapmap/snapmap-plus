/* Native source receipts keep byte equality separate from names and hashes. */
#include "package_shader_receipt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char object[0x90];
static const char source[] = "{ Tex2D textures/source }\r\n";
static const char path[] = "generated/decls/renderparm/tex.decl";
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    if (address != (uintptr_t)object || length > sizeof(object)) return 0;
    memcpy(out, object, length); return 1;
}
static int matches(const char *body, size_t length)
{
    return sh_package_shader_receipt_matches((uintptr_t)object, "renderparm", "tex",
        body, length, object, sizeof(object));
}
static void begin(sh_package_shader_receipt_frame *frame)
{ sh_package_shader_receipt_begin(frame, (uintptr_t)object, "renderParm", "tex"); }
static void bytes(size_t begin, size_t length)
{ sh_package_shader_receipt_bytes(path, strlen(source), begin, source + begin, length); }
static void exact_and_retired(void)
{
    sh_package_shader_receipt_frame frame;
    memset(object, 0, sizeof(object)); object[0x20] = 17; object[0x68] = 19;
    assert(!matches(source, strlen(source)));
    begin(&frame); assert(sh_package_shader_receipt_wants("generated\\decls\\renderParm\\TEX.decl"));
    bytes(7, strlen(source) - 7); bytes(0, 9);
    sh_package_shader_receipt_end(&frame, 1, read_memory, NULL);
    assert(matches(source, strlen(source)));
    /* Native retirement clears source pointer, length and checksum; native
     * lifetime promotion changes the common level fields. Neither is payload. */
    memset(object + 0x38, 0, 16); object[0x28] = 4;
    assert(matches(source, strlen(source)));
    object[0x58] ^= 1; assert(!matches(source, strlen(source))); object[0x58] ^= 1;
    object[0x68] ^= 1; assert(!matches(source, strlen(source))); object[0x68] ^= 1;
    char changed[sizeof(source)]; memcpy(changed, source, sizeof(source)); changed[10] ^= 1;
    assert(!matches(changed, strlen(changed))); /* Any freshness value is irrelevant. */
    sh_package_shader_receipt_invalidate((uintptr_t)object);
    assert(!matches(source, strlen(source)));
}
static void rejected_reads(void)
{
    for (int mode = 0; mode < 6; mode++) {
        sh_package_shader_receipt_frame frame; begin(&frame);
        if (mode != 0) bytes(0, strlen(source));
        if (mode == 0) bytes(0, strlen(source) - 1);
        if (mode == 1) sh_package_shader_receipt_bytes(path, strlen(source), 0, "X", 1);
        if (mode == 2) sh_package_shader_receipt_invalidate((uintptr_t)object);
        if (mode == 3) sh_package_shader_receipt_bytes(path, strlen(source) - 1, 0, source, 1);
        if (mode == 4) sh_package_shader_receipt_bytes(path, 0, 0, NULL, 0);
        sh_package_shader_receipt_end(&frame, mode != 5, read_memory, NULL);
        assert(!matches(source, strlen(source)));
    }
}
static void nesting_and_retained(void)
{
    sh_package_shader_receipt_frame outer, inner, pause;
    begin(&outer); bytes(0, 7); begin(&inner); bytes(7, strlen(source) - 7);
    sh_package_shader_receipt_end(&inner, 1, read_memory, NULL);
    sh_package_shader_receipt_begin(&pause, 1234, "material", "other");
    assert(!sh_package_shader_receipt_wants(path));
    sh_package_shader_receipt_bytes(path, strlen(source), 0, "wrong", 5);
    sh_package_shader_receipt_end(&pause, 0, read_memory, NULL);
    sh_package_shader_receipt_end(&outer, 1, read_memory, NULL);
    assert(matches(source, strlen(source)));
    begin(&outer); begin(&inner); bytes(0, strlen(source));
    sh_package_shader_receipt_end(&inner, 0, read_memory, NULL);
    sh_package_shader_receipt_end(&outer, 1, read_memory, NULL);
    assert(!matches(source, strlen(source)));
    begin(&outer); sh_package_shader_receipt_retained(source, strlen(source) - 2);
    sh_package_shader_receipt_end(&outer, 1, read_memory, NULL);
    assert(matches(source, strlen(source)));
    char with_tab[sizeof(source)]; memcpy(with_tab, source, sizeof(source)); with_tab[strlen(source) - 1] = '\t';
    assert(!matches(with_tab, strlen(source)));
}
static void setter_and_stream_agreement(void)
{
    for (int order = 0; order < 2; order++) {
        sh_package_shader_receipt_frame frame; begin(&frame);
        if (!order) bytes(0, strlen(source));
        sh_package_shader_receipt_text_begin((uintptr_t)object);
        sh_package_shader_receipt_text((uintptr_t)object, source, strlen(source) - 2);
        if (order) bytes(0, strlen(source));
        sh_package_shader_receipt_end(&frame, 1, read_memory, NULL);
        assert(matches(source, strlen(source)));
    }
    sh_package_shader_receipt_text_begin((uintptr_t)object);
    assert(!matches(source, strlen(source))); /* A setter outside parse is mutation, not proof. */
    sh_package_shader_receipt_frame frame; begin(&frame); bytes(0, strlen(source));
    sh_package_shader_receipt_text((uintptr_t)object, "different", 9);
    sh_package_shader_receipt_end(&frame, 1, read_memory, NULL); assert(!matches(source, strlen(source)));
    begin(&frame); sh_package_shader_receipt_text((uintptr_t)object + 1, source, strlen(source) - 2);
    sh_package_shader_receipt_end(&frame, 1, read_memory, NULL); assert(!matches(source, strlen(source)));
}
static void program_witness(void)
{
    const char include[] = "#define RETAINED 1\n"; unsigned char metadata[] = {1,2,3};
    sh_package_shader_capture capture = {0}; sh_package_shader_receipt_frame frame;
    memset(object, 0, sizeof(object));
    for (int mode = 0; mode < 5; mode++) {
        sh_package_shader_receipt_program_scope(1);
        sh_package_shader_receipt_begin(&frame,(uintptr_t)object,"renderProg","unit");
        sh_package_shader_receipt_retained(source,strlen(source)-2);
        assert(sh_package_shader_receipt_wants("decls/renderprogs/include.inc"));
        sh_package_shader_receipt_bytes("decls/renderprogs/include.inc",strlen(include),0,include,strlen(include));
        if (mode==2)sh_package_shader_receipt_bytes("decls/renderprogs/include.inc",strlen(include),0,"X",1);
        if (mode!=1)sh_package_shader_receipt_program_compiled((uintptr_t)object);
        assert(sh_package_shader_receipt_program_wrote((uintptr_t)object,metadata,sizeof(metadata))==(mode!=1));
        assert(!sh_package_shader_receipt_program_copy((uintptr_t)object,"unit",source,strlen(source),object,&capture));
        sh_package_shader_receipt_end(&frame,mode!=3,read_memory,NULL);
        sh_package_shader_receipt_program_scope(0);
        if (mode==4)sh_package_shader_receipt_invalidate((uintptr_t)object);
        int copied=sh_package_shader_receipt_program_copy((uintptr_t)object,"unit",source,strlen(source),object,&capture);
        assert(copied==(mode==0||mode==2));
        if (copied) {
            assert(capture.ticket && capture.metadata_length==sizeof(metadata) && !memcmp(capture.metadata,metadata,sizeof(metadata)));
            assert((capture.inputs!=NULL)==(mode==0));
            if(capture.inputs)assert(capture.inputs->length==strlen(include) && !memcmp(capture.inputs->body,include,strlen(include)));
            sh_package_shader_capture_clear(&capture);
        }
    }
}
static void isolated_cook_witness(void)
{
    sh_package_shader_receipt_frame outer,inner,pause;
    sh_package_shader_capture capture={0};
    const char parent_path[]="generated/decls/renderprog/base.decl";
    const char include_path[]="decls/renderprogs/shared.inc";
    const char parent[]="{ parent }",include[]="included";
    for(int mode=0;mode<6;mode++) {
        sh_package_shader_receipt_program_scope(1);
        sh_package_shader_cook_begin(&outer,11,0,"cooked");
        sh_package_shader_receipt_retained(source,strlen(source)-2);
        sh_package_shader_cook_begin(&inner,12,0,"base");
        sh_package_shader_receipt_bytes(parent_path,strlen(parent),0,parent,strlen(parent));
        /* A native fixed-resource parse does not hide compiler input files
         * from either the current cook or the enclosing inherited cook. */
        sh_package_shader_receipt_begin(&pause,99,"material","other");
        assert(sh_package_shader_receipt_wants(include_path));
        sh_package_shader_receipt_bytes(include_path,strlen(include),0,include,mode==1?1:strlen(include));
        if(mode==2)sh_package_shader_receipt_bytes(include_path,strlen(include),0,"X",1);
        sh_package_shader_receipt_end(&pause,1,read_memory,NULL);
        sh_package_shader_receipt_program_compiled(12);
        assert(sh_package_shader_receipt_program_wrote(12,"BASE",4));
        sh_package_shader_cook_end(&inner,1);
        if(mode!=3)sh_package_shader_receipt_program_compiled(11);
        assert(sh_package_shader_receipt_program_wrote(11,"META",4)==(mode!=3));
        assert(!sh_package_shader_cook_copy(0,"cooked",source,strlen(source),&capture));
        sh_package_shader_cook_end(&outer,mode!=4);
        if(mode==5)sh_package_shader_receipt_revoke_scope();
        sh_package_shader_receipt_program_scope(0);
        int copied=sh_package_shader_cook_copy(0,"cooked",source,strlen(source),&capture);
        assert(copied==(mode==0));
        if(copied) {
            int parents=0,includes=0;
            for(sh_package_shader_input *input=capture.inputs;input;input=input->next) {
                parents+=!strcmp(input->path,parent_path);includes+=!strcmp(input->path,include_path);
            }
            assert(parents==1 && includes==1 && capture.ticket);
            sh_package_shader_capture_clear(&capture);
            assert(!sh_package_shader_cook_copy(1,"cooked",source,strlen(source),&capture));
            assert(!sh_package_shader_cook_copy(0,"cooked","different",9,&capture));
            assert(!sh_package_shader_receipt_program_copy(11,"cooked",source,strlen(source),object,&capture));
        }
    }
    sh_package_shader_cook_begin(&outer,11,0,"no_admission");
    sh_package_shader_receipt_retained(source,strlen(source));
    sh_package_shader_receipt_program_compiled(11);
    assert(!sh_package_shader_receipt_program_wrote(11,"META",4));
    sh_package_shader_cook_end(&outer,1);
    assert(!sh_package_shader_cook_copy(0,"no_admission",source,strlen(source),&capture));
}
int main(void)
{
    exact_and_retired(); rejected_reads(); nesting_and_retained(); setter_and_stream_agreement();
    program_witness(); isolated_cook_witness();
    sh_package_shader_receipt_test_reset(); puts("package_shader_receipt_test: PASS"); return 0;
}
