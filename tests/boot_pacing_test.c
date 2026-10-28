/* Boot pacing redirects on scratch code, never game bytes. */
#include "../src/backend/boot_pacing.c"
#include <assert.h>
#include <stdio.h>

static char last_log[512];
void backend_log(const char *message){strncpy_s(last_log,sizeof last_log,message,_TRUNCATE);}

/* Byte copies of the build 13954591 Vulkan sites with their operands zeroed;
 * place() fills the operands for a scratch layout. */
static const unsigned char k_lock[SH_BOOT_LOCK_SITE_LEN]={0x48,0x8d,0x3d,0,0,0,0,0x48,0x89,0x7c,0x24,0x58,0xb2,0x01,
    0x48,0x8b,0xcf,0xe8,0,0,0,0,0xc6,0x44,0x24,0x60,0x01,0x48,0x83,0x3d,0,0,0,0,0x00,0x0f,0x84,0xdc,0,0,0,
    0x48,0x8b,0x0b,0xe8};
static const unsigned char k_sleep[SH_BOOT_SLEEP_SITE_LEN]={0x80,0x3d,0,0,0,0,0x00,0x74,0x2a,0x44,0x8b,0xce,0x44,0x8b,0xc5,
    0x48,0x8d,0x15,0,0,0,0,0x48,0x8b,0xcb,0xe8,0,0,0,0,0x44,0x8b,0xce,0x44,0x8b,0xc5,0x48,0x8d,0x15,0,0,0,0,
    0x48,0x8b,0xcb,0xe8,0,0,0,0,0xb9,0x0a,0,0,0,0xe8,0,0,0,0,0x48,0x83,0x3d,0,0,0,0,0x00};
static const unsigned char k_poll[SH_BOOT_POLL_SITE_LEN]={0x0f,0xb6,0x44,0x24,0x70,0x84,0xc0,0x75,0x1b,0x0f,0xb6,0x46,0x10,
    0x84,0xc0,0x75,0x13,0xb9,0x01,0,0,0,0xe8,0,0,0,0,0x0f,0xb6,0x44,0x24,0x70,0x84,0xc0,0x74,0xe5};

/* Scratch layout: sites, a Sleep thunk, the lock global and two handle globals. */
enum { LOCK=0x000, SLEEP=0x100, POLL=0x200, THUNK=0x400, LOCKG=0x500, H1=0x508, H2=0x510 };
static unsigned char *g_img;
static void put_rel(unsigned char *site,size_t at,size_t next,size_t dest)
{
    int32_t d=(int32_t)((intptr_t)(g_img+dest)-(intptr_t)(site+next));memcpy(site+at,&d,4);
}
static unsigned char *place(void)
{
    unsigned char *p=VirtualAlloc(NULL,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    static const unsigned char thunk[7]={0x48,0xff,0x25,0,0,0,0};
    assert(p);g_img=p;
    memcpy(p+LOCK,k_lock,sizeof k_lock);put_rel(p+LOCK,3,7,LOCKG);put_rel(p+LOCK,30,35,H1);
    memcpy(p+SLEEP,k_sleep,sizeof k_sleep);put_rel(p+SLEEP,0x12,0x16,H1);put_rel(p+SLEEP,0x27,0x2b,H2);
    put_rel(p+SLEEP,0x39,0x3d,THUNK);put_rel(p+SLEEP,0x40,0x45,H2);
    memcpy(p+POLL,k_poll,sizeof k_poll);put_rel(p+POLL,0x17,0x1b,THUNK);
    memcpy(p+THUNK,thunk,sizeof thunk);
    return p;
}

static volatile char g_done,g_abort;
static DWORD WINAPI set_done_later(LPVOID p){Sleep((DWORD)(ULONG_PTR)p);g_done=1;return 0;}
static CRITICAL_SECTION g_cs;
static CRITICAL_SECTION *g_cs_ptr=&g_cs;
static volatile LONG g_entered;
static DWORD WINAPI try_lock(LPVOID p){(void)p;if(TryEnterCriticalSection(&g_cs)){g_entered=1;LeaveCriticalSection(&g_cs);}return 0;}
static int other_thread_can_enter(void)
{
    HANDLE t;g_entered=0;t=CreateThread(NULL,0,try_lock,NULL,0,NULL);assert(t);
    WaitForSingleObject(t,INFINITE);CloseHandle(t);return g_entered;
}
static DWORD WINAPI probe_during_sleep(LPVOID p){(void)p;Sleep(3);try_lock(NULL);return 0;}

int main(void)
{
    unsigned char *img=place();uintptr_t a,b,c;int32_t disp;unsigned char stub[32];
    sig_result r[3]={{"BootVideoLock",SIG_OK,0,0},{"BootVideoSleep",SIG_OK,0,0},{"BootReadPoll",SIG_OK,0,0}};
    sh_boot_pacing_stats st;LARGE_INTEGER f,t0,t1;double ms;HANDLE th;

    /* Pure site checks. */
    assert(sh_boot_video_lock_site(img+LOCK,&a,&b)&&a==(uintptr_t)(img+LOCKG)&&b==(uintptr_t)(img+H1));
    img[LOCK+12]^=1;assert(!sh_boot_video_lock_site(img+LOCK,&a,&b));img[LOCK+12]^=1;
    assert(sh_boot_video_sleep_site(img+SLEEP,&a,&b,&c)&&a==(uintptr_t)(img+H1)&&b==(uintptr_t)(img+H2)&&c==(uintptr_t)(img+THUNK));
    img[SLEEP+0x34]=0x0b;assert(!sh_boot_video_sleep_site(img+SLEEP,&a,&b,&c));img[SLEEP+0x34]=0x0a;
    /* The CMP after the sleep must re-test the second handle. */
    put_rel(img+SLEEP,0x40,0x45,H1);assert(!sh_boot_video_sleep_site(img+SLEEP,&a,&b,&c));put_rel(img+SLEEP,0x40,0x45,H2);
    assert(sh_boot_read_poll_site(img+POLL,&a)&&a==(uintptr_t)(img+THUNK));
    img[POLL+4]=0x68;assert(!sh_boot_read_poll_site(img+POLL,&a));img[POLL+4]=0x70;
    img[POLL+12]=0x18;assert(!sh_boot_read_poll_site(img+POLL,&a));img[POLL+12]=0x10;
    assert(!sh_boot_video_lock_site(NULL,&a,&b)&&!sh_boot_read_poll_site(NULL,&a));
    assert(sh_boot_sleep_thunk(img+THUNK)&&!sh_boot_sleep_thunk(img+LOCK));
    assert(sh_boot_rel32(0x140000005u,0x140000105u,&disp)&&disp==0x100);
    assert(!sh_boot_rel32(0x140000000u,0x240000000u,&disp));

    /* Stubs: the poll stub hands [rsp+0x78] (the caller's [rsp+0x70]) and
     * [rsi+0x10] to the wait; the jump stub only transfers. */
    {
        static const unsigned char want[9]={0x48,0x8d,0x54,0x24,0x78,0x4c,0x8d,0x46,0x10};
        uint64_t fn;
        assert(sh_boot_poll_stub(stub,sizeof stub,0x1122334455667788ull)==21&&!memcmp(stub,want,9));
        memcpy(&fn,stub+11,8);assert(fn==0x1122334455667788ull&&stub[9]==0x48&&stub[10]==0xb8&&stub[19]==0xff&&stub[20]==0xe0);
        assert(!sh_boot_poll_stub(stub,20,1));
        assert(sh_boot_jump_stub(stub,sizeof stub,0x55)==12&&stub[0]==0x48&&stub[1]==0xb8&&stub[10]==0xff&&stub[11]==0xe0);
    }

    /* The poll wait: done at once (spin), done later (yield), never (sleep). */
    QueryPerformanceFrequency(&f);
    g_done=1;g_abort=0;sh_boot_read_wait(1,&g_done,&g_abort);
    sh_boot_pacing_get_stats(&st);assert(st.reads==1&&st.spun==1&&st.slept==0);
    g_done=0;th=CreateThread(NULL,0,set_done_later,(LPVOID)0,0,NULL);assert(th);
    QueryPerformanceCounter(&t0);
    while(!g_done)sh_boot_read_wait(1,&g_done,&g_abort);
    WaitForSingleObject(th,INFINITE);CloseHandle(th);
    g_done=0;QueryPerformanceCounter(&t0);sh_boot_read_wait(1,&g_done,&g_abort);QueryPerformanceCounter(&t1);
    ms=(double)(t1.QuadPart-t0.QuadPart)*1000.0/(double)f.QuadPart;
    sh_boot_pacing_get_stats(&st);assert(st.slept>=1&&ms>=1.0);
    g_abort=1;QueryPerformanceCounter(&t0);sh_boot_read_wait(1,&g_done,&g_abort);QueryPerformanceCounter(&t1);
    ms=(double)(t1.QuadPart-t0.QuadPart)*1000.0/(double)f.QuadPart;assert(ms<1.0);g_abort=0;
    /* Any other length is the engine's own sleep and is not counted. */
    {long before;sh_boot_pacing_get_stats(&st);before=st.reads;sh_boot_read_wait(2,&g_done,&g_abort);sh_boot_pacing_get_stats(&st);assert(st.reads==before);}

    /* The video sleep releases a lock held once by this thread, only for the sleep. */
    InitializeCriticalSection(&g_cs);
    g_video_lock=(void *volatile *)&g_cs_ptr;
    EnterCriticalSection(&g_cs);
    assert(!other_thread_can_enter());
    th=CreateThread(NULL,0,probe_during_sleep,NULL,0,NULL);assert(th);g_entered=0;
    sh_boot_video_sleep(30);
    WaitForSingleObject(th,INFINITE);CloseHandle(th);
    assert(g_entered&&g_cs.OwningThread==(HANDLE)(ULONG_PTR)GetCurrentThreadId()&&g_cs.RecursionCount==1);
    assert(!other_thread_can_enter());
    /* Held twice: the outer hold is kept. */
    EnterCriticalSection(&g_cs);
    th=CreateThread(NULL,0,probe_during_sleep,NULL,0,NULL);assert(th);g_entered=0;
    sh_boot_video_sleep(30);WaitForSingleObject(th,INFINITE);CloseHandle(th);
    assert(!g_entered&&g_cs.RecursionCount==2);
    LeaveCriticalSection(&g_cs);LeaveCriticalSection(&g_cs);
    /* Not held: just sleeps. */
    sh_boot_video_sleep(1);
    sh_boot_pacing_get_stats(&st);assert(st.video_sleeps==3&&st.video_released==1);
    g_video_lock=NULL;

    /* Late, missing or wrong sites leave the bytes untouched. */
    {
        unsigned char before[0x400];
        r[0].addr=(uintptr_t)(img+LOCK);r[1].addr=(uintptr_t)(img+SLEEP);
        r[2].addr=(uintptr_t)(img+POLL);
        memcpy(before,img,sizeof before);
        assert(sh_boot_pacing_install(r,3,3,SH_BOOT_PACING_LATE_MS+1)==0&&strstr(last_log,"too late"));
        assert(!memcmp(before,img,sizeof before)&&!g_page);
        assert(sh_boot_pacing_install(r,0,3,10)==0&&strstr(last_log,"was not found")&&!g_page);
        assert(sh_boot_pacing_install(r,3,0,10)==0&&strstr(last_log,"off")&&!g_page);
        r[1].status=SIG_OK_HOOKED;
        assert(sh_boot_pacing_install(r,3,SH_BOOT_PACING_VIDEO_LOCK,10)==0&&strstr(last_log,"video draw was not found"));
        r[1].status=SIG_OK;
        /* A lock site from another function: its handle is not the one drawn. */
        put_rel(img+LOCK,30,35,H2);
        assert(sh_boot_pacing_install(r,3,SH_BOOT_PACING_VIDEO_LOCK,10)==0&&strstr(last_log,"not one draw"));
        put_rel(img+LOCK,30,35,H1);
        assert(!memcmp(before,img,0x400)&&!g_page);
    }

    /* Installed: the poll calls its stub and the rest of the site is
     * unchanged. A bad video site does not stop the poll. */
    img[SLEEP+0x33]=0xba;
    assert(sh_boot_pacing_install(r,3,3,10)==SH_BOOT_PACING_READ_POLL);
    assert(strstr(last_log,"video lock unchanged -- the load-screen video draw has unexpected bytes"));
    assert(strstr(last_log,"read poll spins"));
    img[SLEEP+0x33]=0xb9;
    assert(sh_boot_read_poll_site(img+POLL,&a)&&a==(uintptr_t)(g_page+PAGE_POLL));
    assert(sh_boot_video_sleep_site(img+SLEEP,&a,&b,&c)&&c==(uintptr_t)(img+THUNK));
    {
        /* The page is executable and read-only, and the jump stub runs. */
        MEMORY_BASIC_INFORMATION mbi;
        assert(VirtualQuery(g_page,&mbi,sizeof mbi)&&mbi.Protect==PAGE_EXECUTE_READ);
        assert(!memcmp(g_page+PAGE_POLL,"\x48\x8d\x54\x24\x78",5));
    }
    assert(sh_boot_pacing_live()==SH_BOOT_PACING_READ_POLL);
    assert(sh_boot_pacing_install(r,3,3,10)==sh_boot_pacing_live());

    /* Removable: unpatching restores the native operand. */
    assert(code_unpatch(&g_patches[1])==B2_PATCH_OK);
    assert(sh_boot_read_poll_site(img+POLL,&a)&&a==(uintptr_t)(img+THUNK));

    /* Both parts on a fresh layout: the draw's sleep calls the video stub,
     * whose jump reaches the product's sleep. */
    g_live=0;g_page=NULL;memset(g_patches,0,sizeof g_patches);
    img=place();r[0].addr=(uintptr_t)(img+LOCK);r[1].addr=(uintptr_t)(img+SLEEP);r[2].addr=(uintptr_t)(img+POLL);
    assert(sh_boot_pacing_install(r,3,3,10)==3&&strstr(last_log,"video lock released"));
    assert(sh_boot_video_sleep_site(img+SLEEP,&a,&b,&c)&&c==(uintptr_t)(g_page+PAGE_VIDEO));
    {uint64_t fn;memcpy(&fn,g_page+PAGE_VIDEO+2,8);assert(fn==(uint64_t)(uintptr_t)sh_boot_video_sleep);}
    assert(g_video_lock==(void *volatile *)(img+LOCKG));
    g_video_lock=NULL;

    /* Matching at recorded RVAs refuses an image that is not a supported build. */
    {
        sig_result m[SH_BOOT_PACING_SIGS];
        assert(sh_boot_pacing_match((const uint8_t *)GetModuleHandleW(NULL),m,SH_BOOT_PACING_SIGS)==0);
        assert(m[0].status==SIG_NOT_FOUND&&!strcmp(m[2].name,"BootReadPoll"));
    }
    assert(sh_boot_pacing_process_age_ms()<600000u);
    puts("boot_pacing_test: passed");
    return 0;
}
