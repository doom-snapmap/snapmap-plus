/* The resource manager's file-open method serves exactly the shadow's claims
 * (R-309): an engine name reaches the native method unchanged, a claimed name
 * becomes a native named memory file filled through the provider's open with
 * the claimed stream, and an owned name that cannot be supplied never reaches
 * the native method. Every stream and memory file is released exactly once:
 * a constructed file by its deleting destructor, and the raw block of a
 * constructor that raised by the sized delete the engine's unwind uses. */
#include <assert.h>
#include <stdlib.h>
#include "../src/backend/resource_file_visibility.c"

void backend_log(const char *message) { (void)message; }

/* --- the shadow (overrides.c) ------------------------------------------- */
typedef struct fake_stream { void **table; int id; } fake_stream;
static int claim_mode;            /* 0 engine name, 1 claimed, 2 owned but unavailable */
static int claims, closes;
static fake_stream *issued;
static const char *pending_name;
static void *pending_stream;
static void fake_stream_close(void *stream, int flags)
{
    assert(stream == issued && flags == 1);
    closes++; free(stream); issued = NULL;
}
static void *fake_stream_table[1] = {(void *)fake_stream_close};
int sh_overrides_claim_open(const char *name, void **stream)
{
    claims++;
    *stream = NULL;
    if (!name || !claim_mode) return 0;
    if (claim_mode == 2) return 1;
    issued = calloc(1, sizeof(*issued));
    assert(issued);
    issued->table = fake_stream_table; issued->id = 7;
    *stream = issued;
    return 1;
}
void sh_overrides_pending_open(const char *name, void *stream) { pending_name = name; pending_stream = stream; }
void *sh_overrides_pending_take(void)
{
    void *stream = pending_stream;
    pending_stream = NULL; pending_name = NULL;
    return stream;
}
void sh_overrides_stream_close(void *stream) { ((void (*)(void *, int))(*(void ***)stream)[0])(stream, 1); }

/* --- the engine ----------------------------------------------------------- */
typedef struct fake_memory_file { void **table; const char *name; int filled; } fake_memory_file;
static int native_opens, news, deletes, constructions, fills, releases, fill_mode, new_mode, memory_mode;
static void *native_result = (void *)0x5150;
static fake_memory_file *constructed;
static void *allocated;
static void *native_open(void *manager, const char *name)
{
    assert(manager == (void *)0x77);
    native_opens++; (void)name;
    return native_result;
}
/* new_mode 1 returns no block, 2 raises before allocating. */
static void *fake_new(size_t bytes)
{
    assert(bytes == 0x180 && !allocated);
    news++;
    if (new_mode == 2) RaiseException(0xe0004322, 0, 0, NULL);
    return new_mode ? NULL : (allocated = calloc(1, bytes));
}
/* The sized delete the engine's unwind uses for a block whose constructor raised. */
static void fake_delete(void *block, size_t bytes)
{
    assert(block == allocated && bytes == 0x180 && !constructed);
    deletes++; free(block); allocated = NULL;
}
static void fake_release(void *file, int flags)
{
    assert(file == constructed && file == allocated && flags == 1);
    releases++; free(file); constructed = NULL; allocated = NULL;
}
static void *fake_memory_table[1] = {(void *)fake_release};
/* memory_mode 1 raises part way through construction: no object exists. */
static void *fake_memory(void *file, const char *name)
{
    assert(file == allocated);
    constructions++;
    ((fake_memory_file *)file)->name = name;
    if (memory_mode) RaiseException(0xe0004323, 0, 0, NULL);
    constructed = file;
    constructed->table = fake_memory_table;
    return file;
}
/* The engine's open-into helper: open by name through the provider slot, where
 * the shadow hands back a pending stream only for exactly this name pointer. */
static unsigned char fake_fill(void *owner, void **file, const char *name, unsigned char required)
{
    void *stream = NULL;
    assert(!owner && file && *file == constructed && required == 1);
    fills++;
    if (fill_mode == 2) RaiseException(0xe0004321, 0, 0, NULL);
    if (fill_mode != 1 && pending_stream && name == pending_name) {
        stream = pending_stream; pending_stream = NULL; pending_name = NULL;
    }
    if (!stream) return 2;
    assert(((fake_stream *)stream)->id == 7);
    ((fake_memory_file *)*file)->filled = 1;
    sh_overrides_stream_close(stream);   /* the helper copies, then closes */
    return 0;
}

static void reset(int mode)
{
    claim_mode = mode; claims = closes = native_opens = news = deletes = constructions = fills = releases = 0;
    fill_mode = new_mode = memory_mode = 0;
    assert(!issued && !pending_stream && !constructed && !allocated);
}

int main(void)
{
    void *(*hook)(void *, const char *) = (void *(*)(void *, const char *))
        sh_resource_file_visibility_test_bind((void *)native_open, (void *)fake_new, (void *)fake_delete,
                                              (void *)fake_memory, (void *)fake_fill);
    const char *name = "generated/swf/smp/0123/4567/89ab.bswf";
    void *file;

    /* An engine name keeps the native method and its result. */
    reset(0);
    assert(hook((void *)0x77, "generated/swf/switches/automap.bswf") == native_result);
    assert(claims == 1 && native_opens == 1 && !news && !fills && !closes);
    reset(0);
    assert(hook((void *)0x77, NULL) == native_result && native_opens == 1);

    /* A claimed name is the engine's own named memory file, filled with the
     * claimed stream; nothing reaches the containers. */
    reset(1);
    file = hook((void *)0x77, name);
    assert(file && file == (void *)constructed && constructed->name == name && constructed->filled);
    assert(!native_opens && news == 1 && constructions == 1 && fills == 1 && closes == 1 && !releases);
    assert(!pending_stream && sh_resource_file_visibility_served() == 1);
    fake_release(file, 1);

    /* An owned name the shadow cannot supply stays unavailable. */
    reset(2);
    assert(!hook((void *)0x77, name));
    assert(!native_opens && !news && !fills && !closes);

    /* A fill that never opens the claimed stream: the stream and the memory
     * file are both released, and the name is not served from elsewhere. */
    reset(1); fill_mode = 1;
    assert(!hook((void *)0x77, name));
    assert(!native_opens && fills == 1 && closes == 1 && releases == 1 && !deletes && !pending_stream);

    /* No native memory file. */
    reset(1); new_mode = 1;
    assert(!hook((void *)0x77, name));
    assert(!native_opens && news == 1 && !constructions && !fills && closes == 1 && !deletes);

    /* A fault before any allocation exists releases nothing native. */
    reset(1); new_mode = 2;
    assert(!hook((void *)0x77, name));
    assert(!native_opens && news == 1 && !constructions && !fills && closes == 1 && !deletes && !releases);

    /* A constructor fault: the object never existed, so only the raw block is
     * released, once, through the sized delete; never its destructor. */
    reset(1); memory_mode = 1;
    assert(!hook((void *)0x77, name));
    assert(!native_opens && news == 1 && constructions == 1 && deletes == 1 && !releases && !fills && closes == 1);
    assert(!allocated && !pending_stream);

    /* A native fault in the fill: the constructed file is released by its
     * own deleting destructor, never the raw delete. */
    reset(1); fill_mode = 2;
    assert(!hook((void *)0x77, name));
    assert(!native_opens && fills == 1 && closes == 1 && releases == 1 && !deletes && !pending_stream);
    assert(sh_resource_file_visibility_served() == 1 && g_failures == 5);

    /* Decoding needs clean matches; hooked fallbacks are refused. */
    {
        sh_resource_file_visibility_native native;
        sig_result results[3] = {{"ResourceManagerConstruct", SIG_OK_HOOKED, 0x1000, 0x1000},
                                 {"ResourceManagerOpenFile", SIG_OK, 0x2000, 0x2000},
                                 {"FileResourceLoad", SIG_OK, 0x3000, 0x3000}};
        assert(!sh_resource_file_visibility_decode(results, 3, NULL, &native) && !native.slot);
        assert(!sh_resource_file_visibility_decode(results + 1, 2, NULL, &native));
    }
    printf("resource_file_visibility_test: claimed names are served as native memory files; engine names keep the native method\n");
    return 0;
}
