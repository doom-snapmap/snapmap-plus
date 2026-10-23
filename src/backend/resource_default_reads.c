/* Native default cleanup contracts; paired evidence is R356/R358. */
#include "resource_default_reads.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct dr_pin { uint32_t rva[2]; unsigned char head[2][16]; } dr_pin;
enum {
    DR_P_FILE_DEFAULT,
    DR_P_MEMORY_CTOR,
    DR_P_MEMORY_DTOR,
    DR_P_MEMORY_BODY,
    DR_P_JSON_DEFAULT,
    DR_P_JSON_CLEAR,
    DR_P_JSON_RELEASE,
    DR_P_JSON_PAIRS,
    DR_P_JSON_MANAGER_DTOR,
    DR_P_JSON_COMMENT,
    DR_P_ARRAY48,
    DR_P_ARRAY78,
    DR_P_STRINGS2F0,
    DR_P_STRINGS_ARRAYS328,
    DR_P_STRING40,
    DR_P_RAW38A,
    DR_P_RAW38B,
    DR_P_RECORDS860,
    DR_P_BASE,
    DR_P_NESTED48,
    DR_P_NESTED78,
    DR_P_NESTED_D8,
    DR_P_NESTED28,
    DR_P_NESTED_LIST48,
    DR_P_JSON_ALLOCATOR_FREE,
    DR_P_JSON_ALLOCATOR_KEY_FREE,
    DR_P_STRING_BODY,
    DR_P_STRING_DTOR,
    DR_P_ANIMPARA_DEFAULT,
    DR_P_ANIMPARA_RELEASE,
    DR_P_STRING_ARRAY,
    DR_P_STRINGS2F0_BODY,
    DR_P_ARRAYS328_BODY,
    DR_P_RAW38A_BODY,
    DR_P_RAW38B_BODY,
    DR_P_RECORDS860_BODY,
    DR_P_RECORDS_B0,
    DR_P_RECORDS_ARRAY
};
static const dr_pin g_dr_pins[] = {
    {{0x17ea150u,0x17f7820u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* file_default */
    {{0x1a0c100u,0x1a19eb0u}, {{0x48,0x8b,0xc4,0x56,0x57,0x41,0x56,0x48,0x81,0xec,0x90,0x01,0x00,0x00,0x48,0xc7},{0x48,0x8b,0xc4,0x56,0x57,0x41,0x56,0x48,0x81,0xec,0x90,0x01,0x00,0x00,0x48,0xc7}}}, /* memory_ctor */
    {{0x2619e0u,0x2620d0u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8}}}, /* memory_dtor */
    {{0x1a0c3d0u,0x1a1a180u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* memory_body */
    {{0x17ea590u,0x17f7c60u}, {{0x48,0x83,0xc1,0x38,0xe9,0x27,0xa9,0x24,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},{0x48,0x83,0xc1,0x38,0xe9,0x47,0xad,0x24,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}}}, /* json_default */
    {{0x1a34ec0u,0x1a429b0u}, {{0x8b,0x41,0x08,0x83,0xc0,0xfa,0x83,0xf8,0x01,0x77,0x08,0x48,0x8b,0x09,0xe9,0x2d},{0x8b,0x41,0x08,0x83,0xc0,0xfa,0x83,0xf8,0x01,0x77,0x08,0x48,0x8b,0x09,0xe9,0x2d}}}, /* json_clear */
    {{0x1a33b50u,0x1a41640u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* json_release */
    {{0x1a34300u,0x1a41df0u}, {{0x41,0x56,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x41,0x56,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* json_pairs */
    {{0x1a34160u,0x1a41c50u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* json_manager_dtor */
    {{0x1a33b10u,0x1a41600u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* json_comment */
    {{0x702c30u,0x703f20u}, {{0x40,0x55,0x48,0x83,0xec,0x20,0x48,0x8b,0xe9,0x85,0xd2,0x7e,0x3f,0x48,0x89,0x5c},{0x40,0x55,0x48,0x83,0xec,0x20,0x48,0x8b,0xe9,0x85,0xd2,0x7e,0x3f,0x48,0x89,0x5c}}}, /* array48 */
    {{0x702bc0u,0x703eb0u}, {{0x40,0x55,0x48,0x83,0xec,0x20,0x48,0x8b,0xe9,0x85,0xd2,0x7e,0x3f,0x48,0x89,0x5c},{0x40,0x55,0x48,0x83,0xec,0x20,0x48,0x8b,0xe9,0x85,0xd2,0x7e,0x3f,0x48,0x89,0x5c}}}, /* array78 */
    {{0x7029e0u,0x703cd0u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8}}}, /* strings2f0 */
    {{0x702a20u,0x703d10u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8}}}, /* strings_arrays328 */
    {{0x702860u,0x703b50u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48}}}, /* string40 */
    {{0x702820u,0x703b10u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8}}}, /* raw38a */
    {{0x7027e0u,0x703ad0u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8}}}, /* raw38b */
    {{0x7029a0u,0x703c90u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8}}}, /* records860 */
    {{0x702a60u,0x703d50u}, {{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0x43,0x30,0x9a,0x01,0x48,0x8b,0xd9},{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0x63,0xa1,0x98,0x01,0x48,0x8b,0xd9}}}, /* base */
    {{0x702b10u,0x703e00u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48}}}, /* nested48 */
    {{0x702790u,0x703a80u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48}}}, /* nested78 */
    {{0x7028b0u,0x703ba0u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48}}}, /* nested_d8 */
    {{0x702910u,0x703c00u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* nested28 */
    {{0x702a90u,0x703d80u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* nested_list48 */
    {{0x1a35580u,0x1a43080u}, {{0x48,0x85,0xd2,0x74,0x14,0x48,0x8b,0x0d,0xac,0xf5,0xe5,0x03,0x41,0xb8,0x10,0x00},{0x48,0x85,0xd2,0x74,0x14,0x48,0x8b,0x0d,0x3c,0xb2,0x54,0x05,0x41,0xb8,0x10,0x00}}}, /* json_allocator_free */
    {{0xd97d60u,0x17ab490u}, {{0x48,0x8b,0x01,0x48,0xff,0x60,0x20,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},{0x48,0x8b,0x01,0x48,0xff,0x60,0x20,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}}}, /* json_allocator_key_free */
    {{0x19ef380u,0x19fd120u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* string_body */
    {{0x19eff00u,0x19fdca0u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* string_dtor */
    {{0x14a58e0u,0x14b22e0u}, {{0x48,0x83,0xc1,0x38,0xe9,0xc7,0xfc,0xff,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},{0x48,0x83,0xc1,0x38,0xe9,0xc7,0xfc,0xff,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}}}, /* animpara_default */
    {{0x14a55b0u,0x14b1fb0u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x0f,0xb6,0x41,0x23,0x33,0xff},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x0f,0xb6,0x41,0x23,0x33,0xff}}}, /* animpara_release */
    {{0x13e5130u,0x5057e0u}, {{0x40,0x55,0x48,0x83,0xec,0x20,0x48,0x8b,0xe9,0x85,0xd2,0x7e,0x3e,0x48,0x89,0x5c},{0x40,0x55,0x48,0x83,0xec,0x20,0x48,0x8b,0xe9,0x85,0xd2,0x7e,0x3e,0x48,0x89,0x5c}}}, /* string_array */
    {{0x701ed0u,0x7031c0u}, {{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x81,0xc1},{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x81,0xc1}}}, /* strings2f0_body */
    {{0x701f80u,0x703270u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* arrays328_body */
    {{0x701da0u,0x703090u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* raw38a_body */
    {{0x701d30u,0x703020u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* raw38b_body */
    {{0x701e50u,0x703140u}, {{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* records860_body */
    {{0x702180u,0x703470u}, {{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48},{0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}}}, /* records_b0 */
    {{0x702b60u,0x703e50u}, {{0x40,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf1,0x85,0xd2,0x7e,0x32,0x48,0x89,0x5c},{0x40,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf1,0x85,0xd2,0x7e,0x32,0x48,0x89,0x5c}}}, /* records_array */
};

enum { DR_VALUE, DR_MANAGER, DR_PAIR_ARRAY, DR_COMMENT_ARRAY, DR_CALLBACK, DR_STRING, DR_CALLBACK_ARRAY, DR_RECORD_ARRAY, DR_RECORD };
typedef struct dr_task { uintptr_t address; size_t count, stride; unsigned kind, slot, flag; } dr_task;
typedef struct dr_set { uintptr_t *items; size_t capacity, count; } dr_set;
typedef struct dr_probe {
    sh_resource_default_read_receipt *receipt;
    const sh_resource_default_read_input *input;
    dr_task *tasks;
    size_t count, capacity;
    dr_set owned, visited;
    unsigned char pins[sizeof(g_dr_pins)/sizeof(g_dr_pins[0])];
} dr_probe;

static int dr_fail(dr_probe *p, sh_resource_default_read_reason why, uintptr_t a, uint32_t type)
{
    if (p->receipt->reason == SH_DEFAULT_READ_OK) {
        p->receipt->reason = why; p->receipt->offending_address = a; p->receipt->offending_type = type;
    }
    return 0;
}
static int dr_span(dr_probe *p, uintptr_t a, size_t size)
{
    if (!a || size > UINTPTR_MAX - a) return dr_fail(p, SH_DEFAULT_READ_BOUNDS, a, 0);
    return 1;
}
static int dr_read(dr_probe *p, uintptr_t a, void *out, size_t size)
{
    return dr_span(p,a,size) && (p->input->read(p->input->read_context,a,out,size) ||
        dr_fail(p,SH_DEFAULT_READ_MEMORY,a,0));
}
static int dr_at(dr_probe *p, uintptr_t a, size_t offset, void *out, size_t size)
{
    if (offset > UINTPTR_MAX-a) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,a,0);
    return dr_read(p,a+offset,out,size);
}
static int dr_ptr(dr_probe *p, uintptr_t a, size_t off, uintptr_t *out)
{ uint64_t n=0; if (!dr_at(p,a,off,&n,8)) return 0; *out=(uintptr_t)n; return 1; }
static int dr_u32(dr_probe *p, uintptr_t a, size_t off, uint32_t *out)
{ return dr_at(p,a,off,out,4); }
static int dr_u8(dr_probe *p, uintptr_t a, size_t off, unsigned char *out)
{ return dr_at(p,a,off,out,1); }
static size_t dr_hash(uintptr_t a)
{
    a ^= a >> 30; a *= (uintptr_t)UINT64_C(0xbf58476d1ce4e5b9);
    a ^= a >> 27; return (size_t)(a ^ (a >> 31));
}
static int dr_set_insert(dr_probe *p, dr_set *s, uintptr_t a)
{
    size_t i;
    if (!a) return 1;
    if (!s->capacity || s->count >= s->capacity/2) {
        size_t cap=s->capacity ? s->capacity*2 : 64;
        uintptr_t *items;
        if (cap < s->capacity || cap > SIZE_MAX/sizeof(*items))
            return dr_fail(p,SH_DEFAULT_READ_ALLOCATION,a,0);
        items=(uintptr_t *)calloc(cap,sizeof(*items));
        if (!items) return dr_fail(p,SH_DEFAULT_READ_ALLOCATION,a,0);
        for(i=0;i<s->capacity;i++) if(s->items[i]) {
            size_t at=dr_hash(s->items[i])&(cap-1);
            while(items[at]) at=(at+1)&(cap-1);
            items[at]=s->items[i];
        }
        free(s->items); s->items=items; s->capacity=cap;
    }
    i=dr_hash(a)&(s->capacity-1);
    while(s->items[i]) {
        if(s->items[i]==a) return dr_fail(p,SH_DEFAULT_READ_OWNERSHIP,a,0);
        i=(i+1)&(s->capacity-1);
    }
    s->items[i]=a; s->count++; return 1;
}
static int dr_push(dr_probe *p, unsigned kind, uintptr_t a, size_t count, size_t stride, unsigned slot, unsigned flag)
{
    dr_task *tasks;
    if (p->input->work_limit && p->receipt->work+p->count >= p->input->work_limit)
        return dr_fail(p,SH_DEFAULT_READ_BUDGET,a,kind);
    if (p->count==p->capacity) {
        size_t cap=p->capacity ? p->capacity*2 : 32;
        if(cap<p->capacity || cap>SIZE_MAX/sizeof(*tasks)) return dr_fail(p,SH_DEFAULT_READ_ALLOCATION,a,kind);
        tasks=(dr_task *)realloc(p->tasks,cap*sizeof(*tasks));
        if(!tasks) return dr_fail(p,SH_DEFAULT_READ_ALLOCATION,a,kind);
        p->tasks=tasks; p->capacity=cap;
    }
    p->tasks[p->count++]=(dr_task){a,count,stride,kind,slot,flag}; return 1;
}
static uintptr_t dr_pin_address(dr_probe *p, unsigned id)
{
    uint32_t rva=g_dr_pins[id].rva[p->input->renderer];
    if(rva>p->input->image_size || 16>p->input->image_size-rva ||
       rva>UINTPTR_MAX-p->input->image_base) {
        dr_fail(p,SH_DEFAULT_READ_IMAGE,p->input->image_base,id); return 0;
    }
    return p->input->image_base+rva;
}
static int dr_pin_check(dr_probe *p, unsigned id)
{
    unsigned char head[16]; uintptr_t a;
    if(p->pins[id]) return 1;
    a=dr_pin_address(p,id);
    if(!a || !dr_read(p,a,head,sizeof(head))) return 0;
    if(memcmp(head,g_dr_pins[id].head[p->input->renderer],sizeof(head)))
        return dr_fail(p,SH_DEFAULT_READ_PIN,a,id);
    p->pins[id]=1; return 1;
}
static int dr_allocator(dr_probe *p, int key)
{
    static const uint32_t holder[2]={0x3eba5f8,0x55b7fd8};
    uintptr_t object,vt,callback,a;
    uint32_t rva=holder[p->input->renderer];
    if(rva>p->input->image_size || 8>p->input->image_size-rva ||
       rva>UINTPTR_MAX-p->input->image_base)
        return dr_fail(p,SH_DEFAULT_READ_IMAGE,p->input->image_base,0);
    a=p->input->image_base+rva;
    if(!dr_ptr(p,a,0,&object) || !object || !dr_ptr(p,object,0,&vt) || !vt ||
       !dr_ptr(p,vt,0x20,&callback)) return dr_fail(p,SH_DEFAULT_READ_CALLBACK,a,0);
    if(callback!=dr_pin_address(p,DR_P_JSON_ALLOCATOR_FREE) ||
       !dr_pin_check(p,DR_P_JSON_ALLOCATOR_FREE))
        return dr_fail(p,SH_DEFAULT_READ_CALLBACK,callback,0);
    if(key) {
        if(!dr_ptr(p,vt,0x10,&callback)) return 0;
        if(callback!=dr_pin_address(p,DR_P_JSON_ALLOCATOR_KEY_FREE) ||
           !dr_pin_check(p,DR_P_JSON_ALLOCATOR_KEY_FREE))
            return dr_fail(p,SH_DEFAULT_READ_CALLBACK,callback,0);
    }
    return 1;
}
static int dr_string(dr_probe *p, uintptr_t a, uintptr_t static_inline)
{
    uintptr_t bytes; uint32_t flags;
    if(!dr_pin_check(p,DR_P_STRING_BODY) || !dr_ptr(p,a,0x10,&bytes) || !dr_u32(p,a,0x18,&flags)) return 0;
    if(bytes && bytes!=static_inline && (flags&0x40000000u))
        return dr_set_insert(p,&p->owned,bytes);
    return 1;
}
static int dr_raw_list(dr_probe *p, uintptr_t a, size_t off, size_t stride, unsigned kind, unsigned slot)
{
    uintptr_t data; uint32_t num,cap; unsigned char flags;
    if(!dr_ptr(p,a,off,&data) || !dr_u32(p,a,off+8,&num) ||
       !dr_u32(p,a,off+12,&cap) || !dr_u8(p,a,off+19,&flags)) return 0;
    if((int32_t)num<0 || (int32_t)cap<0 || num>cap)
        return dr_fail(p,SH_DEFAULT_READ_BOUNDS,a+off,num);
    if(flags!=0 && flags!=3 && flags!=4) return 1;
    if(!data) return cap ? dr_fail(p,SH_DEFAULT_READ_BOUNDS,a+off,cap) : 1;
    if(!dr_set_insert(p,&p->owned,data)) return 0;
    if(!kind) return 1; /* Numerical storage is freed without reading its elements. */
    if(kind==DR_CALLBACK_ARRAY && !dr_pin_check(p,slot==0?DR_P_STRING_ARRAY:
        (stride==0x48?DR_P_ARRAY48:DR_P_ARRAY78))) return 0;
    if(kind==DR_RECORD_ARRAY && !dr_pin_check(p,DR_P_RECORDS_ARRAY)) return 0;
    if(cap>SIZE_MAX/stride) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,data,cap);
    if(!dr_span(p,data,(size_t)cap*stride)) return 0;
    return dr_push(p,kind,data,cap,stride,slot,0);
}
static int dr_value(dr_probe *p, uintptr_t a)
{
    uintptr_t data,comments; uint32_t type; unsigned char allocated; uint64_t count;
    if(!dr_set_insert(p,&p->visited,a) || !dr_pin_check(p,DR_P_JSON_RELEASE) ||
       !dr_ptr(p,a,0,&data) || !dr_u32(p,a,8,&type) || !dr_ptr(p,a,16,&comments)) return 0;
    if(type>8) return dr_fail(p,SH_DEFAULT_READ_TYPE,a,type);
    if(type==4) {
        if(!dr_u8(p,a,12,&allocated)) return 0;
        if(allocated && (!dr_allocator(p,0) || !dr_set_insert(p,&p->owned,data))) return 0;
    } else if((type==6 || type==7) && data) {
        if(!dr_pin_check(p,DR_P_JSON_MANAGER_DTOR) || !dr_push(p,DR_MANAGER,data,0,0,0,0)) return 0;
    } else if(type==8 && data) {
        if(!dr_push(p,DR_CALLBACK,data,0,0,0x10,1)) return 0;
    }
    if(comments) {
        if(comments<8) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,comments,0);
        if(!dr_read(p,comments-8,&count,8)) return 0;
        if(count>(SIZE_MAX-8)/8) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,comments,0);
        if(!dr_span(p,comments,(size_t)count*8) ||
           !dr_set_insert(p,&p->owned,comments-8) || !dr_pin_check(p,DR_P_JSON_COMMENT)) return 0;
        if(!dr_push(p,DR_COMMENT_ARRAY,comments,(size_t)count,8,0,0)) return 0;
    }
    return 1;
}
static int dr_manager(dr_probe *p, uintptr_t a)
{
    uintptr_t data; uint32_t num,cap; unsigned char flags;
    if(!a) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,a,0);
    if(!dr_set_insert(p,&p->owned,a) || !dr_pin_check(p,DR_P_JSON_PAIRS) ||
       !dr_ptr(p,a,0,&data) || !dr_u32(p,a,8,&num) || !dr_u32(p,a,12,&cap) ||
       !dr_u8(p,a,19,&flags)) return 0;
    if((int32_t)num<0 || (int32_t)cap<0 || num>cap || (num && !data))
        return dr_fail(p,SH_DEFAULT_READ_BOUNDS,a,num);
    if(data && (flags==0 || flags==3 || flags==4) && !dr_set_insert(p,&p->owned,data)) return 0;
    if(num>SIZE_MAX/8 || (num && !dr_span(p,data,(size_t)num*8))) return 0;
    return !num || dr_push(p,DR_PAIR_ARRAY,data,num,8,0,0);
}
static int dr_strings(dr_probe *p, uintptr_t a, const uint16_t *offsets, size_t n)
{
    size_t i;
    for(i=0;i<n;i++) {
        if(offsets[i]>UINTPTR_MAX-a) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,a,0);
        if(!dr_push(p,DR_STRING,a+offsets[i],0,0,0,0)) return 0;
    }
    return 1;
}
static int dr_callback(dr_probe *p, const dr_task *t)
{
    static const unsigned ids[]={DR_P_STRINGS2F0,DR_P_STRINGS_ARRAYS328,DR_P_STRING40,
        DR_P_RAW38A,DR_P_RAW38B,DR_P_RECORDS860,DR_P_BASE,DR_P_NESTED48,
        DR_P_NESTED78,DR_P_NESTED_D8,DR_P_NESTED28,DR_P_NESTED_LIST48};
    static const uint16_t many[]={0x2c0,0x270,0x240,0x1d8,0x198,0x168,0x100,0xc0,0x90,0x28};
    static const uint16_t arrays_strings[]={0x2d8,0x2a8,0x220,0x1d8,0x190,0x160,0xf8,0xb8,0x88,0x20};
    static const uint16_t two[]={0x40,0x10}, three[]={0xa8,0x78,0x10};
    uintptr_t vt,callback; size_t i; unsigned id=UINT_MAX;
    if(!dr_span(p,t->address,0x860)) return 0; /* Largest proven fixed payload span. */
    if(!dr_set_insert(p,&p->visited,t->address) || !dr_ptr(p,t->address,0,&vt) ||
       !vt || !dr_ptr(p,vt,t->slot,&callback)) return dr_fail(p,SH_DEFAULT_READ_CALLBACK,t->address,0);
    if(t->slot==0) {
        if(callback!=dr_pin_address(p,DR_P_STRING_DTOR)) return dr_fail(p,SH_DEFAULT_READ_CALLBACK,callback,0);
        if(!dr_pin_check(p,DR_P_STRING_DTOR)) return 0;
        return dr_string(p,t->address,0);
    }
    for(i=0;i<sizeof(ids)/sizeof(ids[0]);i++)
        if(callback==dr_pin_address(p,ids[i])) { id=ids[i]; break; }
    if(id==UINT_MAX) return dr_fail(p,SH_DEFAULT_READ_CALLBACK,callback,0);
    if(!dr_pin_check(p,id) || (t->flag && !dr_set_insert(p,&p->owned,t->address))) return 0;
    switch(id) {
    case DR_P_STRINGS2F0:
        return dr_pin_check(p,DR_P_STRINGS2F0_BODY) && dr_strings(p,t->address,many,sizeof(many)/sizeof(many[0]));
    case DR_P_STRINGS_ARRAYS328:
        return dr_pin_check(p,DR_P_ARRAYS328_BODY) &&
            dr_strings(p,t->address,arrays_strings,sizeof(arrays_strings)/sizeof(arrays_strings[0])) &&
            dr_raw_list(p,t->address,0x310,0x48,DR_CALLBACK_ARRAY,0x10) &&
            dr_raw_list(p,t->address,0x280,0x78,DR_CALLBACK_ARRAY,0x10);
    case DR_P_STRING40: return dr_push(p,DR_STRING,t->address+0x10,0,0,0,0);
    case DR_P_RAW38A: case DR_P_RAW38B:
        return dr_pin_check(p,id==DR_P_RAW38A?DR_P_RAW38A_BODY:DR_P_RAW38B_BODY) &&
            dr_raw_list(p,t->address,0x18,1,0,0);
    case DR_P_RECORDS860:
        return dr_pin_check(p,DR_P_RECORDS860_BODY) &&
            dr_pin_check(p,DR_P_RECORDS_B0) &&
            dr_push(p,DR_RECORD_ARRAY,t->address+0x20,12,0xb0,0,0) &&
            dr_raw_list(p,t->address,8,0xb0,DR_RECORD_ARRAY,0);
    case DR_P_BASE: return 1;
    case DR_P_NESTED48: return dr_push(p,DR_STRING,t->address+0x18,0,0,0,0);
    case DR_P_NESTED78: return dr_strings(p,t->address,two,2);
    case DR_P_NESTED_D8: return dr_strings(p,t->address,three,3);
    case DR_P_NESTED28: return dr_raw_list(p,t->address,8,1,0,0);
    case DR_P_NESTED_LIST48:
        return dr_raw_list(p,t->address,0x30,0x78,DR_CALLBACK_ARRAY,0x10);
    default: return dr_fail(p,SH_DEFAULT_READ_CALLBACK,callback,0);
    }
}
static int dr_array(dr_probe *p, dr_task *t)
{
    uintptr_t value,key; uint32_t key_type;
    if(!t->count) return 1;
    if(!t->stride || t->count>SIZE_MAX/t->stride) return dr_fail(p,SH_DEFAULT_READ_BOUNDS,t->address,0);
    if(!dr_span(p,t->address,t->count*t->stride)) return 0;
    if(t->count>1 && !dr_push(p,t->kind,t->address+t->stride,t->count-1,t->stride,t->slot,t->flag)) return 0;
    switch(t->kind) {
    case DR_PAIR_ARRAY:
        if(!dr_ptr(p,t->address,0,&value)) return 0;
        if(!value) return 1;
        if(!dr_set_insert(p,&p->owned,value) || !dr_ptr(p,value,0,&key) ||
           !dr_u32(p,value,8,&key_type)) return 0;
        if(key && key_type==1 && (!dr_allocator(p,1) || !dr_set_insert(p,&p->owned,key))) return 0;
        if(!dr_span(p,value,0x28)) return 0;
        return dr_push(p,DR_VALUE,value+0x10,0,0,0,0);
    case DR_COMMENT_ARRAY:
        if(!dr_ptr(p,t->address,0,&value)) return 0;
        return !value || (dr_allocator(p,0) && dr_set_insert(p,&p->owned,value));
    case DR_CALLBACK_ARRAY: return dr_push(p,DR_CALLBACK,t->address,0,0,t->slot,0);
    case DR_RECORD_ARRAY: return dr_push(p,DR_RECORD,t->address,0,0,0,0);
    default: return 0;
    }
}
static int dr_run(dr_probe *p)
{
    dr_task t;
    while(p->count && p->receipt->reason==SH_DEFAULT_READ_OK) {
        if(p->input->work_limit && p->receipt->work>=p->input->work_limit)
            return dr_fail(p,SH_DEFAULT_READ_BUDGET,p->tasks[p->count-1].address,0);
        t=p->tasks[--p->count]; p->receipt->work++;
        switch(t.kind) {
        case DR_VALUE: if(!dr_value(p,t.address)) return 0; break;
        case DR_MANAGER: if(!dr_manager(p,t.address)) return 0; break;
        case DR_CALLBACK: if(!dr_callback(p,&t)) return 0; break;
        case DR_STRING: if(!dr_string(p,t.address,0)) return 0; break;
        case DR_RECORD:
            if(!dr_pin_check(p,DR_P_RECORDS_B0) || !dr_raw_list(p,t.address,0x68,1,0,0) ||
               !dr_raw_list(p,t.address,0x10,1,0,0)) return 0;
            break;
        default: if(!dr_array(p,&t)) return 0; break;
        }
    }
    return p->receipt->reason==SH_DEFAULT_READ_OK;
}
int sh_resource_default_read_begin(sh_resource_default_read_receipt *r, const sh_resource_default_read_input *in)
{
    dr_probe p; uintptr_t old,vt,callback,root; uint32_t type; int ok=0;
    if(!r) return 0;
    memset(r,0,sizeof(*r)); r->state=1;
    if(!in) { r->reason=SH_DEFAULT_READ_INPUT; return 0; }
    r->invocation=*in;
    memset(&p,0,sizeof(p)); p.receipt=r; p.input=in;
    if(!in->read || !in->resource || !in->default_callback || !in->image_base ||
       sizeof(uintptr_t)!=8 || in->renderer<0 || in->renderer>1) {
        dr_fail(&p,SH_DEFAULT_READ_INPUT,in->resource,0); goto done;
    }
    if(!in->pinned_image || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base) {
        dr_fail(&p,SH_DEFAULT_READ_IMAGE,in->image_base,0); goto done;
    }
    if(!dr_ptr(&p,in->resource,0,&vt)) goto done;
    if(!vt) { dr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->resource,0); goto done; }
    if(!dr_ptr(&p,vt,0x50,&callback)) goto done;
    if(callback!=in->default_callback) { dr_fail(&p,SH_DEFAULT_READ_CALLBACK,callback,0); goto done; }
    if(in->resource>UINTPTR_MAX-0x38) { dr_fail(&p,SH_DEFAULT_READ_BOUNDS,in->resource,0); goto done; }
    root=in->resource+0x38;
    if(callback==dr_pin_address(&p,DR_P_JSON_DEFAULT)) {
        r->routine=SH_DEFAULT_READ_ROUTINE_JSON;
        if(!dr_pin_check(&p,DR_P_JSON_DEFAULT) || !dr_pin_check(&p,DR_P_JSON_CLEAR) ||
           !dr_u32(&p,root,8,&type)) goto done;
        if(type>8) { dr_fail(&p,SH_DEFAULT_READ_TYPE,root,type); goto done; }
        if(type==6 || type==7) {
            if(!dr_ptr(&p,root,0,&old) || !dr_push(&p,DR_MANAGER,old,0,0,0,0)) goto done;
        }
    } else if(callback==dr_pin_address(&p,DR_P_FILE_DEFAULT)) {
        r->routine=SH_DEFAULT_READ_ROUTINE_FILE;
        if(!dr_pin_check(&p,DR_P_FILE_DEFAULT) || !dr_pin_check(&p,DR_P_MEMORY_CTOR) ||
           !dr_pin_check(&p,DR_P_STRING_BODY) || !dr_ptr(&p,root,0,&old)) goto done;
        if(old) {
            if(!dr_span(&p,old,0x180) || !dr_ptr(&p,old,0,&vt)) goto done;
            if(!vt) { dr_fail(&p,SH_DEFAULT_READ_CALLBACK,old,0); goto done; }
            if(!dr_ptr(&p,vt,0,&callback)) goto done;
            if(callback!=dr_pin_address(&p,DR_P_MEMORY_DTOR)) { dr_fail(&p,SH_DEFAULT_READ_CALLBACK,callback,0); goto done; }
            if(!dr_pin_check(&p,DR_P_MEMORY_DTOR) || !dr_pin_check(&p,DR_P_MEMORY_BODY)) goto done;
            if(!dr_set_insert(&p,&p.owned,old)) goto done;
            {
                uintptr_t bytes; unsigned char owns;
                if(!dr_ptr(&p,old,0x168,&bytes) || !dr_u8(&p,old,0x178,&owns)) goto done;
                if(owns && !dr_set_insert(&p,&p.owned,bytes)) goto done;
                if(!dr_string(&p,old+8,old+0x38)) goto done;
            }
        }
    } else if(callback==dr_pin_address(&p,DR_P_ANIMPARA_DEFAULT)) {
        r->routine=SH_DEFAULT_READ_ROUTINE_ANIMPARA;
        if(!dr_pin_check(&p,DR_P_ANIMPARA_DEFAULT) || !dr_pin_check(&p,DR_P_ANIMPARA_RELEASE) ||
           !dr_raw_list(&p,root,0x10,1,0,0) ||
           !dr_raw_list(&p,root,0x80,0x30,DR_CALLBACK_ARRAY,0)) goto done;
    } else { dr_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,callback,0); goto done; }
    ok=dr_run(&p);
done:
    free(p.tasks); free(p.owned.items); free(p.visited.items);
    r->complete=ok && r->reason==SH_DEFAULT_READ_OK;
    return r->complete;
}
static int dr_same(const sh_resource_default_read_input *a, const sh_resource_default_read_input *b)
{
    return a && b && a->image_base==b->image_base && a->image_size==b->image_size &&
        a->renderer==b->renderer && a->pinned_image==b->pinned_image &&
        a->resource==b->resource && a->default_callback==b->default_callback &&
        a->read==b->read && a->read_context==b->read_context && a->work_limit==b->work_limit;
}
int sh_resource_default_read_finish(sh_resource_default_read_receipt *r,
    const sh_resource_default_read_input *in, int native_returned, int in_order)
{
    if(!r) return 0;
    if(r->state!=1 || !dr_same(&r->invocation,in)) {
        r->reason=SH_DEFAULT_READ_INVOCATION; r->complete=0;
    } else if(!native_returned) { r->reason=SH_DEFAULT_READ_NATIVE_RETURN; r->complete=0; }
    else if(!in_order) { r->reason=SH_DEFAULT_READ_ORDER; r->complete=0; }
    r->state=2;
    return r->complete;
}

