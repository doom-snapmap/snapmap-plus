/* Real load, copy, assignment and deletion callbacks around the native view-filter contract. */
#define SH_SAVED_VIEWS_TEST
#include "../src/backend/saved_views.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uintptr_t refs; unsigned char data[0x170]; } test_slot;
static unsigned char view[SV_MODE + 4], normal[SV_MODE + 4];
static int a, b, c, d, e, calls;
static void *destination;
static void pass_bind(void *p, void *s, void *src, void *dst) { (void)p; (void)s; (void)src; (void)dst; }
static void *pass_destroy(void *p, unsigned mode) { (void)mode; return p; }
static void *copy_def(void **dst, void **src) { (void)src; *dst = destination; return dst; }
static void *assign_def(void **dst, void **src)
{
    if (*dst) sv_destroyed(*dst,1);
    return copy_def(dst,src);
}
static void slot(test_slot *s, void *row, void *def)
{
    memset(s,0,sizeof(*s));
    s->refs = 1;
    memcpy(s->data,&row,sizeof(row));
    memcpy(s->data + SV_DEFINITION,&def,sizeof(def));
}
static int modes[4];
static int raise_on_filter;
static void *retire_in_filter;
static void **relocate;
static void *nested;
static int retained;
static void filter(void *store)
{
    void **cells = *(void ***)((char *)store + 0xc0);
    for (int i = 0; i < 4; i++) {
        unsigned char *row = *(unsigned char **)((unsigned char *)cells[i] + 8);
        modes[i] = *(int *)(row + SV_MODE);
    }
    ++calls;
    if (nested) {
        void *inner = nested;
        nested = NULL;
        sv_filtered(inner);
    }
    if (relocate) {
        size_t count = *(int *)((char *)store + 0xc8);
        memcpy(relocate,cells,count * sizeof(*cells));
        *(void ***)((char *)store + 0xc0) = relocate;
        relocate = NULL;
    }
    if (retire_in_filter) {
        sv_destroyed(retire_in_filter,1);
        retire_in_filter=NULL;
    }
    if (raise_on_filter) RaiseException(0xe14ce110,0,0,NULL);
}

static void count_kept(void *store)
{
    void **cells = *(void ***)((char *)store + 0xc0);
    int n = *(int *)((char *)store + 0xc8);
    retained = 0;
    for (int i = 0; i < n; ++i) {
        unsigned char *row = *(unsigned char **)((char *)cells[i]+8);
        if (*(int *)(row+SV_MODE)==1) ++retained;
    }
}

int main(void)
{
    unsigned char store[0x170] = {0};
    int ids[] = {0,1,2,3};
    test_slot entries[4];
    void *cells[] = {&entries[0],&entries[1],&entries[2],&entries[3]};
    *(int *)(view + SV_MODE) = SV_VIEW;
    *(int *)(normal + SV_MODE) = 0;
    *(void ***)(store + 0xc0) = cells;
    *(int *)(store + 0xc8) = 4;
    *(int **)(store + 0x128) = ids;
    *(int *)(store + 0x130) = 4;
    InitializeCriticalSection(&sv_lock);
    sv_bind=pass_bind; sv_destroy=pass_destroy; sv_copy=copy_def; sv_assign=assign_def; sv_filter=filter;
    sv_ready=1;

    slot(&entries[0],view,&a);
    slot(&entries[1],view,&b);
    slot(&entries[2],normal,&c);
    slot(&entries[3],view,&d);
    sv_filtered(store);
    CHECK(modes[0]==3 && modes[1]==3 && modes[2]==0 && modes[3]==3 && calls==1);
    sv_bound(NULL,NULL,&a,entries[0].data);
    sv_bound(NULL,NULL,&c,entries[2].data);
    CHECK(sv_count==1 && !sv_registered(&c));
    sv_filtered(store);
    CHECK(modes[0]==1 && modes[1]==3 && modes[2]==0 && modes[3]==3);
    CHECK(*(int *)(view+SV_MODE)==3 && cells[0]==&entries[0] && cells[1]==&entries[1]);

    /* Distinct cells may keep the same deep definition or the very same shared native block. */
    slot(&entries[3],view,&a);
    sv_filtered(store);
    CHECK(modes[0]==1 && modes[3]==1 && cells[0]==&entries[0] && cells[3]==&entries[3]);
    cells[3]=cells[0];
    retire_in_filter=&a;
    sv_filtered(store);
    CHECK(!sv_count && modes[0]==1 && modes[3]==1);
    CHECK(cells[0]==&entries[0] && cells[3]==&entries[0]);
    sv_filtered(store);
    CHECK(modes[0]==3 && modes[3]==3);
    cells[3]=&entries[3];
    slot(&entries[3],view,&d);
    sv_bound(NULL,NULL,&a,entries[0].data);

    /* A copy-on-write edit, followed by undo to the original allocation. */
    void **saved = (void **)(entries[0].data+SV_DEFINITION);
    void *old = *saved;
    destination=&e;
    CHECK(sv_copied(saved,&old)==saved && sv_registered(&e));
    sv_filtered(store); CHECK(modes[0]==1 && modes[1]==3);
    *saved=old;
    sv_filtered(store); CHECK(modes[0]==1 && modes[1]==3);
    CHECK(sv_destroyed(&e,1)==&e && !sv_registered(&e));

    /* A room template takes over that same numeric ID and frees its prior definition. */
    destination=&e;
    sv_assigned(saved,(void **)(entries[1].data+SV_DEFINITION));
    CHECK(!sv_count);
    sv_filtered(store); CHECK(modes[0]==3 && modes[1]==3);
    /* The allocator can reuse an old declaration address in another map. */
    *saved=&a;
    sv_filtered(store); CHECK(modes[0]==3);
    sv_bound(NULL,NULL,&a,entries[0].data);
    destination=&d;
    sv_assigned((void **)(entries[3].data+SV_DEFINITION),saved);
    CHECK(sv_count==2 && sv_registered(&d));
    void *next_cells[4];
    nested=store;
    relocate=next_cells;
    sv_filtered(store);
    CHECK(next_cells[0]==&entries[0] && next_cells[3]==&entries[3] && calls>=2);
    memcpy(cells,next_cells,sizeof(cells));
    *(void ***)(store + 0xc0) = cells;
    raise_on_filter=1;
    __try { sv_filtered(store); }
    __except(GetExceptionCode()==0xe14ce110 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
    CHECK(cells[0]==&entries[0] && cells[3]==&entries[3] && modes[0]==1 && modes[3]==1 && modes[1]==3);
    sv_destroyed(&a,1); sv_destroyed(&d,1);
    CHECK(!sv_count && !sv_registered(&a) && !sv_registered(&d));
    /* Cross the inline capacity with many slots sharing the one tracked definition. */
    enum { many = 33 };
    void *shared[many];
    int members[many+1];
    for (int i=0; i<many; ++i) { shared[i]=&entries[0]; members[i]=i; }
    members[many]=0;
    *(void ***)(store+0xc0)=shared;
    *(int *)(store+0xc8)=many;
    *(int **)(store+0x128)=members;
    *(int *)(store+0x130)=many+1;
    sv_filter=count_kept;
    sv_bound(NULL,NULL,&a,entries[0].data);
    sv_filtered(store);
    CHECK(retained==many);
    for (int i=0; i<many; ++i) CHECK(shared[i]==&entries[0]);
    sv_destroyed(&a,1);
    CHECK(!sv_count);
    puts("saved_views_test: passed");
    return 0;
}
