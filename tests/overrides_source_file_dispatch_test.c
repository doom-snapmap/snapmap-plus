#define main overrides_existing_main
#include "overrides_internal_test.c"
#undef main

static void *reentrant_original;
static unsigned read_calls,destroy_calls;
static void *dispatch_delete(void *self,unsigned flags)
{(void)self;(void)flags;destroy_calls++;return NULL;}
static long long dispatch_read(void *self,void *body,uint64_t count)
{
    (void)body;(void)count;read_calls++;
    sh_overrides_file_dispatch d;CHECK(sh_overrides_file_dispatch_snapshot(self,&d));
    ((void *(*)(void *,unsigned))(*(void ***)self)[0])(self,0);
    CHECK(!sh_overrides_file_dispatch_snapshot(self,&d) || !d.receipt);
    SetLastError(991);return 0;
}
static uintptr_t opening_caller;static void *opening_provider,*observed_stream;static unsigned opening_calls,return_calls;
static uint64_t dispatch_guard(uintptr_t caller,void *provider)
{opening_caller=caller;opening_provider=provider;opening_calls++;SetLastError(981);return 2;}
static void dispatch_observer(void *stream,uintptr_t caller,void *provider,uint64_t stamp)
{CHECK(caller==opening_caller&&provider==opening_provider&&stamp==2);observed_stream=stream;return_calls++;SetLastError(982);}
static void other_observer(void *stream,uintptr_t caller,void *provider,uint64_t stamp)
{(void)stream;(void)caller;(void)provider;(void)stamp;destroy_calls++;}
int main(void)
{
    sh_overrides_file_dispatch d;void *stream=sh_overrides_test_memory_stream((const unsigned char *)"x",1);
    CHECK(stream);SetLastError(887);CHECK(sh_overrides_file_dispatch_snapshot(stream,&d));CHECK(d.product&&!d.receipt&&GetLastError()==887);
    void **original=*(void ***)stream;void *table[31];memcpy(table,original,sizeof table);
    void *saved_seek=original[14];original[14]=(void *)dispatch_read;CHECK(!sh_overrides_file_dispatch_snapshot(stream,&d));original[14]=saved_seek;
    source_active=1;sh_overrides_test_source_attach(stream,"generated/decls/renderparm/test.decl",NULL,1);
    void **wrapper=*(void ***)stream;CHECK(wrapper!=original);CHECK(sh_overrides_file_dispatch_snapshot(stream,&d)&&d.product&&d.receipt);
    void *forged[31];memcpy(forged,wrapper,sizeof forged);*(void ***)stream=forged;
    CHECK(!sh_overrides_file_dispatch_snapshot(stream,&d));*(void ***)stream=wrapper;
    void *saved=wrapper[3];wrapper[3]=(void *)dispatch_read;CHECK(!sh_overrides_file_dispatch_snapshot(stream,&d));wrapper[3]=saved;
    sh_overrides_test_stream_close(stream);source_active=0;
    /* A native Read may synchronously destroy its stream. The wrapper holds
     * its metadata until Read returns, and never resurrects active lineage. */
    stream=sh_overrides_test_memory_stream((const unsigned char *)"x",1);original=*(void ***)stream;
    memcpy(table,original,sizeof table);table[0]=(void *)dispatch_delete;table[5]=(void *)dispatch_read;*(void ***)stream=table;
    source_active=1;sh_overrides_test_source_attach(stream,"generated/decls/renderparm/test.decl",NULL,1);
    CHECK(sh_overrides_file_dispatch_snapshot(stream,&d)&&d.receipt&&!d.product);
    CHECK(sh_overrides_test_stream_read(stream,NULL,0)==0);CHECK(read_calls==1&&destroy_calls==1&&GetLastError()==991);
    CHECK(*(void ***)stream==table);*(void ***)stream=original;sh_overrides_test_stream_close(stream);source_active=0;
    CHECK(!sh_overrides_file_dispatch_snapshot(NULL,&d));CHECK(!sh_overrides_file_dispatch_snapshot((void *)1,&d));
    CHECK(sh_overrides_set_file_return_observer(dispatch_guard,dispatch_observer));CHECK(sh_overrides_set_file_return_observer(dispatch_guard,dispatch_observer));
    CHECK(!sh_overrides_set_file_return_observer(dispatch_guard,other_observer));CHECK(!sh_overrides_file_return_observer_ready(dispatch_guard,dispatch_observer,NULL,NULL));
    /* The real owned Open wrapper carries the exact result/caller/stamp and
     * restores LastError after both observer callbacks. */
    const char *pending="generated/decls/renderparm/dispatch.decl";
    stream=sh_overrides_test_memory_stream((const unsigned char *)"x",1);CHECK(stream);
    sh_overrides_pending_open(pending,stream);SetLastError(883);
    CHECK(sh_overrides_test_open(pending,0)==stream&&observed_stream==stream);
    CHECK(opening_calls==1&&return_calls==1&&opening_caller&&opening_provider==NULL&&GetLastError()==883);
    CHECK(!sh_overrides_pending_take());sh_overrides_test_stream_close(stream);
    if(g_failed)return 1;puts("override active receipt dispatch controls passed");return 0;
}
