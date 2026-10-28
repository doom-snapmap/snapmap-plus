/* The unrestricted placement patch on scratch code, never game bytes. */
#include "../src/backend/place_anywhere.c"
#include <assert.h>

static char last_log[512];
void backend_log(const char *message){strncpy_s(last_log,sizeof last_log,message,_TRUNCATE);}

/* The native refusal both gates share: blocked, outside every module, or no
 * accepted surface and no air flag. With the branch turned into a JMP the
 * last clause never runs. */
static int native_valid(int blocked,int module,int surface,unsigned flags)
{
    if(blocked||module==-1)return 0;
    return surface!=0||(flags&0x30)!=0;
}
static int patched_valid(int blocked,int module,int surface,unsigned flags)
{
    (void)surface;(void)flags;
    return !(blocked||module==-1);
}
static unsigned char *scratch(void)
{
    unsigned char *p=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(p);memset(p,0xcc,4096);
    memcpy(p,k_commit,sizeof k_commit);memcpy(p+0x100,k_element,sizeof k_element);
    return p;
}
int main(void)
{
    unsigned char *p=scratch();
    sig_result r[2]={{"PlaceGateCommit",SIG_OK,0,0},{"PlaceGateElement",SIG_OK,0,0}};
    unsigned flags;int s;

    /* Site recognition: exact bytes only, the branch at its documented offset. */
    assert(sh_place_gate_branch(p,SH_PLACE_GATE_COMMIT)==6&&p[6]==SH_PLACE_GATE_JNZ);
    assert(sh_place_gate_branch(p+0x100,SH_PLACE_GATE_ELEMENT)==0x23&&p[0x123]==SH_PLACE_GATE_JNZ);
    assert(sh_place_gate_branch(p,SH_PLACE_GATE_ELEMENT)<0&&sh_place_gate_branch(p+0x100,SH_PLACE_GATE_COMMIT)<0);
    assert(sh_place_gate_branch(NULL,0)<0&&sh_place_gate_branch(p,2)<0&&sh_place_gate_branch(p,-1)<0);
    p[40]^=0x10;assert(sh_place_gate_branch(p,SH_PLACE_GATE_COMMIT)<0);p[40]^=0x10;
    /* The tested mask (0x30 = AIR | AIR_UNALIGNED) is part of each match. */
    assert(k_commit[40]==0x30&&k_element[0x44]==0x30);

    /* The rule: only the no-surface-and-no-air refusal goes away. */
    for(flags=0;flags<0x40;flags++)for(s=0;s<=4;s++) {
        assert(patched_valid(1,0,s,flags)==0&&patched_valid(0,-1,s,flags)==0);
        assert(patched_valid(0,0,s,flags)==1);
        if(s||(flags&0x30))assert(native_valid(0,0,s,flags)==patched_valid(0,0,s,flags));
    }
    assert(native_valid(0,0,0,0x01)==0&&patched_valid(0,0,0,0x01)==1);   /* FLOOR demon in the air */

    /* Unavailable: either binding missing or wrong. Neither site is touched. */
    r[0].addr=(uintptr_t)p;r[1].addr=(uintptr_t)(p+0x100);
    assert(!sh_place_anywhere_install(r,1)&&strstr(last_log,"ghost colour test was not found"));
    assert(p[6]==0x75&&p[0x123]==0x75&&!g_live);
    r[1].status=SIG_OK_HOOKED;
    assert(!sh_place_anywhere_install(r,2)&&strstr(last_log,"unavailable")&&p[6]==0x75);
    r[1].status=SIG_OK;p[0x101]^=1;
    assert(!sh_place_anywhere_install(r,2)&&strstr(last_log,"unexpected bytes")&&p[6]==0x75&&p[0x123]==0x75);
    p[0x101]^=1;r[0].addr=0;
    assert(!sh_place_anywhere_install(r,2)&&strstr(last_log,"Place test has unexpected bytes")&&p[0x123]==0x75);

    /* A failed second write rolls the first back. */
    r[0].addr=(uintptr_t)p;
    sh_patch_test_faults(0,2,0);
    assert(!sh_place_anywhere_install(r,2)&&strstr(last_log,"could not be patched"));
    assert(p[6]==0x75&&p[0x123]==0x75&&!g_patch[0].live&&!g_patch[1].live);
    sh_patch_test_faults(0,0,0);

    /* Installed: both JNZ become JMP with their displacements kept. */
    assert(sh_place_anywhere_install(r,2)&&sh_place_anywhere_live()&&strstr(last_log,"open air"));
    assert(p[6]==SH_PLACE_GATE_JMP&&p[7]==0x27&&p[0x123]==SH_PLACE_GATE_JMP&&p[0x124]==0x2e);
    assert(sh_place_anywhere_install(r,2));
    assert(code_unpatch(&g_patch[0])==B2_PATCH_OK&&code_unpatch(&g_patch[1])==B2_PATCH_OK);
    assert(p[6]==0x75&&p[0x123]==0x75);
    puts("place_anywhere_test: ok");
    return 0;
}
