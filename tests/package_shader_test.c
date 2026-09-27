#include "package_shader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytes { unsigned char data[16384]; size_t length; } bytes;
static void raw(bytes *b, const void *data, size_t length)
{ assert(length <= sizeof(b->data) - b->length); memcpy(b->data + b->length, data, length); b->length += length; }
static void fill(bytes *b, size_t length, unsigned char value)
{ assert(length <= sizeof(b->data) - b->length); memset(b->data + b->length, value, length); b->length += length; }
static void byte(bytes *b, unsigned char n) { raw(b, &n, 1); }
static void be(bytes *b, uint32_t n)
{ unsigned char p[4] = {(unsigned char)(n >> 24),(unsigned char)(n >> 16),(unsigned char)(n >> 8),(unsigned char)n}; raw(b, p, 4); }
static void le(bytes *b, uint32_t n)
{ unsigned char p[4] = {(unsigned char)n,(unsigned char)(n >> 8),(unsigned char)(n >> 16),(unsigned char)(n >> 24)}; raw(b, p, 4); }
static void big64(bytes *b, uint64_t n) { be(b, (uint32_t)(n >> 32)); be(b, (uint32_t)n); }
static void string(bytes *b, const char *s) { le(b, (uint32_t)strlen(s)); raw(b, s, strlen(s)); }
static const unsigned char declaration[] = "{ fixture }";
static const unsigned char linked_declaration[] = "{ linked fixture }";
static const char vertex[] = "#version 450 core\nvoid main(){ /* textures/source */ }";
static const char fragment[] = "#version 450 core\nvoid main(){}";
static const char *fixture_include = "decls/renderprogs/global.inc";
static void parameter(bytes *b, unsigned char operation, const char *name)
{ byte(b, operation); byte(b, 15); string(b, name); }
static bytes fixture(void)
{
    bytes b = {0};
    be(&b, 0xb68800ad); be(&b, sh_package_shader_declaration_checksum(declaration, sizeof(declaration) - 1));
    be(&b, 0x43e10000); string(&b, "parent/name"); big64(&b, 0x8048);
    byte(&b, 0); be(&b, 8);
    parameter(&b, 0, "color"); fill(&b, 16, 0x41);
    parameter(&b, 0, "diffuse"); string(&b, "textures/source");
    parameter(&b, 0, "filter"); string(&b, "linear");
    parameter(&b, 0, "stageprogram"); string(&b, "child");
    parameter(&b, 0, "stringparm"); string(&b, "textures/prose");
    parameter(&b, 20, "color"); string(&b, "curve"); string(&b, "factor");
    parameter(&b, 4, "color"); string(&b, "left"); fill(&b, 16, 0x42); string(&b, "right");
    parameter(&b, 1, "diffuse"); string(&b, "ignored/image"); string(&b, "factor");
    for (unsigned stage = 0; stage < 3; stage++) for (unsigned group = 0; group < 7; group++) {
        be(&b, group ? 0 : 2);
        if (!group) { string(&b, "color"); string(&b, stage == 2 ? "native_abi_slot" : "diffuse"); }
    }
    be(&b, 1); string(&b, fixture_include); be(&b, 0x12345678);
    be(&b, 1); string(&b, "FEATURE_TOKEN");
    big64(&b, 0); be(&b, 2); big64(&b, 1); big64(&b, UINT64_C(0xabcdef0123456789));
    fill(&b, 192, 0x63);
    fill(&b, 10, 0x21); string(&b, vertex);
    fill(&b, 10, 0x22); string(&b, fragment);
    fill(&b, 10, 0x23); string(&b, "");
    fill(&b, 36, 0x71); return b;
}
static int schema(void *context, const char *name, int *kind)
{
    if (context) return *(int *)context;
    if (!strcmp(name, "color") || !strcmp(name, "factor") || !strcmp(name, "left") || !strcmp(name, "right")) *kind = 0;
    else if (!strcmp(name, "diffuse")) *kind = 1;
    else if (!strcmp(name, "filter")) *kind = 10;
    else if (!strcmp(name, "stageprogram")) *kind = 11;
    else if (!strcmp(name, "stringparm")) *kind = 12;
    else return 0;
    return 1;
}
static const sh_package_shader_schema kinds = {NULL, schema};
static size_t find_reference(const sh_package_shader_program *p, const char *name)
{
    for (size_t i = 0; i < p->reference_count; i++) if (!strcmp(p->references[i].name, name)) return i;
    return SIZE_MAX;
}
static void test_checksums(void)
{
    assert(sh_package_shader_checksum(NULL, 0) == 0x3b75655e);
    assert(sh_package_shader_checksum((const unsigned char *)"a", 1) == 0x927c8494);
    assert(sh_package_shader_checksum((const unsigned char *)"abc", 3) == 0x275fa452);
    assert(sh_package_shader_checksum((const unsigned char *)"message digest", 14) == 0x11b4777d);
    unsigned char a[1000]; memset(a, 'a', sizeof(a));
    const size_t lengths[] = {55,56,63,64,65,1000};
    const uint32_t values[] = {0x645ebb4f,0x1fc94972,0x8097445d,0x994388a2,0x24d0cce1,0x1ef5fd73};
    for (size_t i = 0; i < sizeof(lengths)/sizeof(*lengths); i++) assert(sh_package_shader_checksum(a, lengths[i]) == values[i]);
    assert(sh_package_shader_declaration_checksum((const unsigned char *)"abc \r\n", 6) == 0x275fa452);
    assert(sh_package_shader_declaration_checksum((const unsigned char *)"abc\t", 4) != 0x275fa452);
}
static void test_decode(void)
{
    bytes b = fixture(); char error[256]; sh_package_shader_program p = {0};
    assert(sh_package_shader_decode(b.data, b.length, &kinds, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(p.permutation_count == 2 && p.permutations[0] == 1 && p.permutations[1] == UINT64_C(0xabcdef0123456789));
    assert(!p.permutation_mask && p.stages[0].length == strlen(vertex) && !p.stages[2].length);
    assert(!memcmp(b.data + p.stages[0].offset, vertex, strlen(vertex)));
    size_t i = find_reference(&p, "textures/source"); assert(i != SIZE_MAX);
    assert(p.references[i].kind == SH_PACKAGE_SHADER_RESOURCE && !strcmp(p.references[i].family, "image"));
    i = find_reference(&p, "curve"); assert(i != SIZE_MAX && !strcmp(p.references[i].family, "table"));
    i = find_reference(&p, "decls/renderprogs/global.inc"); assert(i != SIZE_MAX);
    assert(p.references[i].kind == SH_PACKAGE_SHADER_INCLUDE && !p.references[i].family && p.references[i].source_stamp == 0x12345678);
    assert(find_reference(&p, "textures/prose") == SIZE_MAX && find_reference(&p, "ignored/image") == SIZE_MAX);
    i = find_reference(&p, "native_abi_slot");
    assert(i != SIZE_MAX && p.references[i].kind == SH_PACKAGE_SHADER_PARAMETER);
    i = find_reference(&p, "FEATURE_TOKEN");
    assert(i != SIZE_MAX && p.references[i].kind == SH_PACKAGE_SHADER_FLAG && !strcmp(p.references[i].family,"renderprogflag"));
    for (size_t j = 1; j < p.reference_count; j++) assert(p.references[j-1].offset + p.references[j-1].length <= p.references[j].offset);
    sh_package_shader_program_clear(&p);
    for (size_t n = 0; n < b.length; n++) {
        assert(sh_package_shader_decode(b.data, n, &kinds, &p, error, sizeof(error)) != SH_PACKAGE_SHADER_OK);
        assert(!p.references && !p.permutations);
    }
    assert(sh_package_shader_decode(b.data, b.length, NULL, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    int unavailable = 0; sh_package_shader_schema unknown = {&unavailable,schema};
    assert(sh_package_shader_decode(b.data, b.length, &unknown, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    unavailable = -1;
    assert(sh_package_shader_decode(b.data, b.length, &unknown, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_ERROR);
    b.data[0] ^= 1;
    assert(sh_package_shader_decode(b.data, b.length, &kinds, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    b.data[0] ^= 1; byte(&b, 0);
    assert(sh_package_shader_decode(b.data, b.length, &kinds, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    b.length--; memset(b.data + 12, 0xff, 4);
    assert(sh_package_shader_decode(b.data, b.length, &kinds, &p, error, sizeof(error)) == SH_PACKAGE_SHADER_ERROR);
}
typedef struct relocation { int reject_kind, calls; } relocation;
static int relocate(void *context, const sh_package_shader_reference *ref, const char **replacement)
{
    relocation *state = context; static char name[128]; state->calls++;
    if (ref->kind != SH_PACKAGE_SHADER_RESOURCE && state->reject_kind != (int)ref->kind) return 0;
    snprintf(name, sizeof(name), "smp/%s/linked", ref->family ? ref->family : "include");
    *replacement = name; return 1;
}
static void test_rebind(void)
{
    bytes b = fixture(), retained = b; char error[256];
    sh_package_shader_source original = {declaration,sizeof(declaration)-1};
    sh_package_shader_source linked = {linked_declaration,sizeof(linked_declaration)-1};
    unsigned char *out = NULL; size_t size = 0, changed = 0;
    assert(sh_package_shader_rebind(b.data,b.length,&kinds,original,original,NULL,NULL,&out,&size,&changed,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(size == b.length && !changed && !memcmp(out,b.data,size)); free(out);
    relocation state = {-1,0};
    assert(sh_package_shader_rebind(b.data,b.length,&kinds,original,linked,relocate,&state,&out,&size,&changed,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(changed == 6 && state.calls > 6 && !memcmp(&b,&retained,sizeof(b)));
    sh_package_shader_program p = {0};
    assert(sh_package_shader_decode(out,size,&kinds,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(p.declaration_checksum == sh_package_shader_declaration_checksum(linked.body,linked.length));
    assert(find_reference(&p,"smp/image/linked") != SIZE_MAX && find_reference(&p,"smp/table/linked") != SIZE_MAX);
    assert(p.stages[0].length == strlen(vertex) && !memcmp(out+p.stages[0].offset,vertex,strlen(vertex)));
    assert(p.references[find_reference(&p,"decls/renderprogs/global.inc")].source_stamp == 0x12345678);
    sh_package_shader_program_clear(&p); free(out);
    for (int kind = SH_PACKAGE_SHADER_PARAMETER; kind <= SH_PACKAGE_SHADER_FLAG; kind++) {
        state = (relocation){kind,0};
        assert(sh_package_shader_rebind(b.data,b.length,&kinds,original,linked,relocate,&state,&out,&size,&changed,error,sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
        assert(!out && !size && !changed && !memcmp(&b,&retained,sizeof(b)));
    }
    assert(sh_package_shader_rebind(b.data,b.length,&kinds,linked,original,NULL,NULL,&out,&size,&changed,error,sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    assert(!out && !size && !changed);
}
static bytes module(void)
{
    bytes b = {0}; be(&b,4); be(&b,sh_package_shader_checksum((const unsigned char *)vertex,strlen(vertex)));
    be(&b,0x13579bdf); be(&b,24);
    le(&b,0x07230203); le(&b,0x00010000); le(&b,0x00080001); le(&b,2); le(&b,0);
    le(&b,0x00010000); return b; /* Framing fixture, not an executable GPU shader. */
}
static void test_modules(void)
{
    char error[256]; bytes b=module();
    assert(sh_package_shader_spirv(b.data,b.length,(const unsigned char *)vertex,strlen(vertex),error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(sh_package_shader_spirv(b.data,b.length,(const unsigned char *)fragment,strlen(fragment),error,sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    for(size_t n=0;n<b.length;n++) assert(sh_package_shader_spirv(b.data,n,(const unsigned char *)vertex,strlen(vertex),error,sizeof(error)) != SH_PACKAGE_SHADER_OK);
    b.data[38]=0;
    assert(sh_package_shader_spirv(b.data,b.length,(const unsigned char *)vertex,strlen(vertex),error,sizeof(error)) == SH_PACKAGE_SHADER_ERROR);
    b=module(); b.data[8]^=0xff;
    assert(sh_package_shader_spirv(b.data,b.length,(const unsigned char *)vertex,strlen(vertex),error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
}
typedef struct paths { char original[32][256], linked[32][256]; int kinds[32]; size_t count; } paths;
static int path(void *context, sh_package_shader_path_kind kind, const char *original, const char *linked)
{
    paths *p=context; assert(p->count<32); size_t i=p->count++;
    snprintf(p->original[i],256,"%s",original); snprintf(p->linked[i],256,"%s",linked); p->kinds[i]=kind; return 1;
}
static int has_path(const paths *p,const char *wanted,int linked)
{ for(size_t i=0;i<p->count;i++) if(!strcmp(linked?p->linked[i]:p->original[i],wanted)) return 1; return 0; }
static void test_paths(void)
{
    bytes b=fixture(); char error[256]; sh_package_shader_program program={0}; paths p={0};
    assert(sh_package_shader_decode(b.data,b.length,&kinds,&program,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,"original","smp/linked",&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(p.count==7 && has_path(&p,"generated/renderprogs/smp/linked_pc_vulkan.bin",1));
    assert(has_path(&p,"generated/renderprogs/permutations/smp/linked_abcdef0123456789.decl",1));
    assert(has_path(&p,"generated/spirv/smp/linked.vspv",1));
    assert(has_path(&p,"decls/renderprogs/global.inc",1));
    memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_OPENGL,"original","smp/linked",&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(p.count==5 && has_path(&p,"generated/renderprogs/smp/linked_pc.bin",1));
    size_t permutation_count=program.permutation_count, reference_count=program.reference_count;
    program.permutation_count=0; program.reference_count=0;
    const char *permutation="generated/renderprogs/permutations/cyberdemonshockwave_0000000000000001.decl";
    memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,permutation,permutation,&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(has_path(&p,"generated/spirv/generated/renderprogs/permutations/cyberdemonshockwave_0000000000/000001.vspv",0));
    assert(has_path(&p,"generated/renderprogs/permutations/cyberdemonshockwave_0000000000000001_pc_vulkan.bin",0));
    char short_name[40]; memset(short_name,'a',32); short_name[32]=0;
    memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,short_name,short_name,&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(has_path(&p,"generated/spirv/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.vspv",0));
    short_name[32]='a';short_name[33]=0; memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,short_name,short_name,&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(has_path(&p,"generated/spirv/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/aaa.vspv",0));
    char nested_name[120];
    snprintf(nested_name,sizeof(nested_name),"a/longer/directory/%s",short_name);
    memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,nested_name,nested_name,&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(has_path(&p,"generated/spirv/a/longer/directory/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/aaa.vspv",0));
    memset(short_name,'a',sizeof(short_name)-1); short_name[sizeof(short_name)-1]=0;
    snprintf(nested_name,sizeof(nested_name),"%s%s",short_name,short_name);
    memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,nested_name,nested_name,&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(has_path(&p,"generated/spirv/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/aaaaaaaaaaaaaaaaaa.vspv",0));
    program.stages[2]=program.stages[1]; memset(&p,0,sizeof(p));
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,"compute","compute",&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_OK);
    assert(has_path(&p,"generated/spirv/compute.cspv",0));
    char long_name[300];memset(long_name,'a',299);long_name[299]=0;
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,"original",long_name,&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_UNSUPPORTED);
    assert(sh_package_shader_paths(SH_PACKAGE_SHADER_VULKAN,"../bad","name",&program,path,&p,error,sizeof(error)) == SH_PACKAGE_SHADER_ERROR);
    /* Restore owned array lengths after using a projection for path fixtures. */
    program.permutation_count=permutation_count; program.reference_count=reference_count;
    sh_package_shader_program_clear(&program);
}
int main(void)
{
    test_checksums();test_decode();test_rebind();test_modules();test_paths();
    fixture_include = "/decls/renderprogs/global.inc";
    test_decode();test_rebind();test_paths();
    fixture_include = "\\Decls\\Renderprogs\\Global.inc";
    test_decode();test_rebind();test_paths();
    puts("package_shader_test: PASS");return 0;
}
