#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "image_exclusion.h"
#ifndef SH_IMAGE_EXCLUSION_TESTING
#include "backend_log.h"
#include "engine_globals.h"
#include "host_image.h"
#endif
#include "prepare_worker.h"

/* Resource list (the purge driver walks +0x18): type name +0x08, next +0x18, items +0x20, count
 * +0x28. idImage, both executables (R-309): name +0x08, GenericLoad bit +0x24 bit 0, default flag
 * +0x2c bit 2, sentinel/header +0x38/+0x3c, option block (copied to +0x54) state +0x58, width +0x60,
 * height +0x64. The block's later words are renderer-specific (sh_image_exclusion_layout, R-389). */
enum { IE_LIST_TYPE = 0x08, IE_LIST_NEXT = 0x18, IE_LIST_ITEMS = 0x20, IE_LIST_COUNT = 0x28,
    IE_NAME = 0x08, IE_SOURCE = 0x24, IE_FLAGS = 0x2c, IE_BINARY = 0x38, IE_STATE = 0x58,
    IE_WIDTH = 0x60, IE_HEIGHT = 0x64 };
#define IE_LIST_LIMIT 4096u
#define IE_ENTRY_LIMIT 262144

typedef void *(*ie_lock_fn)(void);
typedef unsigned (*ie_acquire_fn)(void *lock, char blocking);
typedef void (*ie_release_fn)(void *lock);
static struct {
    void *const *head;
    ie_lock_fn lock;
    ie_acquire_fn acquire;
    ie_release_fn release;
    const volatile DWORD *main_thread;
    sh_image_exclusion_layout layout;
    int installed, reported_thread, reported_fault, test_other_thread;
} g_ie;

int sh_image_exclusion_layout_of(int vulkan, sh_image_exclusion_layout *out)
{
    /* Vulkan's block keeps the format at +0x3c, OpenGL's at +0x38, and the bias 13.0f follows 0x20
     * bytes later on both (VK 0x140d9d58d / GL 0x140d9444d store the block at rsp+0x40; R-389). */
    static const sh_image_exclusion_layout vk = {0x90, 0x94, 0xb0}, gl = {0x8c, 0x90, 0xac};
    if (!out || (vulkan != 0 && vulkan != 1)) return 0;
    *out = vulkan ? vk : gl;
    return 1;
}

static void ie_log(const char *line, int *once)
{
    if (*once) return;
    *once = 1;
#ifndef SH_IMAGE_EXCLUSION_TESTING
    backend_log(line);
#else
    (void)line;
#endif
}
static int ie_main_thread(void)
{
#ifdef SH_IMAGE_EXCLUSION_TESTING
    return !g_ie.test_other_thread;
#else
    return g_ie.main_thread && *g_ie.main_thread == GetCurrentThreadId();
#endif
}
/* The state only idImage Load's exclusion branch leaves on an image whose name does not start with
 * '_' (see image_exclusion.h for why no installed binary and no other path produces it). */
static int ie_exclusion_state(const unsigned char *image)
{
    const char *name = *(const char *const *)(image + IE_NAME);
    return name && name[0] && name[0] != '_' && (image[IE_SOURCE] & 1u) && !(image[IE_FLAGS] & 4u) &&
        *(const uint32_t *)(image + IE_BINARY) == 0xffffffffu &&
        *(const uint32_t *)(image + IE_BINARY + 4) == 0xffffffffu &&
        *(const int *)(image + IE_STATE) == 3 && *(const int *)(image + IE_WIDTH) == 8 &&
        *(const int *)(image + IE_HEIGHT) == 8 && *(const int *)(image + g_ie.layout.format) == 5 &&
        *(const int *)(image + g_ie.layout.usage) == 0 &&
        *(const uint32_t *)(image + g_ie.layout.bias) == 0x41500000u;
}
/* 1 excluded, 0 absent or not excluded, -1 implausible native list. Native lock held. */
static int ie_lookup(const char *name)
{
    const unsigned char *list = (const unsigned char *)*g_ie.head;
    unsigned lists = 0;
    while (list && lists++ < IE_LIST_LIMIT) {
        const char *type = *(const char *const *)(list + IE_LIST_TYPE);
        if (type && !_stricmp(type, "image")) break;
        list = *(const unsigned char *const *)(list + IE_LIST_NEXT);
    }
    if (!list) return 0;               /* no image list yet: nothing is registered */
    if (lists > IE_LIST_LIMIT) return -1;
    void *const *items = *(void *const *const *)(list + IE_LIST_ITEMS);
    int count = *(const int *)(list + IE_LIST_COUNT);
    if (count < 0 || count >= IE_ENTRY_LIMIT || (count && !items)) return -1;
    /* Native add replaces a same-name entry, so a name names at most one registered image. */
    for (int i = 0; i < count; i++) {
        const unsigned char *image = items[i];
        const char *current = image ? *(const char *const *)(image + IE_NAME) : NULL;
        if (current && !_stricmp(current, name)) return ie_exclusion_state(image);
    }
    return 0;
}

/* The target of a rel32 at `at` whose instruction ends `next` bytes later. */
static const unsigned char *ie_relative(const unsigned char *at, size_t next)
{ int32_t displacement; memcpy(&displacement, at, 4); return at + (ptrdiff_t)next + displacement; }
int sh_image_exclusion_decode(const unsigned char *purge, sh_image_exclusion_native *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    /* call <lock> at +0x1b; mov dl,1; mov rcx,rax; call <acquire> at +0x28; mov rbx,[rip+<head>]
     * at +0x37; the walk mov rbx,[rbx+0x18] at +0x4d; mov rcx,rsi; call <release> at +0x56. */
    if (!purge || purge[0x1b] != 0xE8 || memcmp(purge + 0x28, "\xB2\x01\x48\x8B\xC8\xE8", 6) ||
        memcmp(purge + 0x37, "\x48\x8B\x1D", 3) || memcmp(purge + 0x4d, "\x48\x8B\x5B\x18", 4) ||
        memcmp(purge + 0x56, "\x48\x8B\xCE\xE8", 4)) return 0;
    out->lock = ie_relative(purge + 0x1c, 4);
    out->acquire = ie_relative(purge + 0x2e, 4);
    out->head = ie_relative(purge + 0x3a, 4);
    out->release = ie_relative(purge + 0x5a, 4);
    return 1;
}

#ifndef SH_IMAGE_EXCLUSION_TESTING
/* The renderer from the host executable itself (host_image.h accepts only these two basenames), known from
 * the first call, unlike the renderer library, which may load later than the backend. */
static int ie_host_vulkan(void)
{
    const char *name = sh_host_image_name();
    return name && !_stricmp(name, "DOOMx64vk.exe") ? 1 : name && !_stricmp(name, "DOOMx64.exe") ? 0 : -1;
}
#endif
int sh_image_exclusion_install(const sig_result *results, size_t count)
{
#ifndef SH_IMAGE_EXCLUSION_TESTING
    const unsigned char *purge = NULL;
    sh_image_exclusion_native native;
    int once = 0;
    if (g_ie.installed) return 1;
    for (size_t i = 0; results && i < count; i++)
        if (results[i].name && !strcmp(results[i].name, "ResourcePurgeLevel"))
            purge = results[i].status == SIG_OK ? (const unsigned char *)results[i].addr : NULL;
    const uint8_t *base = sh_host_image_base();
    void *const *table_head = (void *const *)glb_resolve(base, "decl_resource_head", NULL);
    const volatile DWORD *main_thread = (const volatile DWORD *)glb_resolve(base, "main_thread_id", NULL);
    /* The purge driver walks the same list-of-lists the globals table names, and the option block's
     * layout is the host executable's renderer's. */
    if (!sh_image_exclusion_decode(purge, &native) || !main_thread || !table_head ||
        table_head != (void *const *)native.head || !sh_image_exclusion_layout_of(ie_host_vulkan(), &g_ie.layout)) {
        ie_log("image availability: the native resource-list lock or the renderer's image layout did not verify; availability is unknown", &once);
        return 0;
    }
    g_ie.lock = (ie_lock_fn)native.lock;
    g_ie.acquire = (ie_acquire_fn)native.acquire;
    g_ie.release = (ie_release_fn)native.release;
    g_ie.head = (void *const *)native.head;
    g_ie.main_thread = main_thread;
    g_ie.installed = 1;
    return 1;
#else
    (void)results; (void)count;
    return g_ie.installed;
#endif
}

static uint32_t ie_be32(const unsigned char *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
int sh_image_exclusion_header_ambiguous(const unsigned char *installed, size_t length)
{
    /* The binary reader stores the first dword at +0x38 and the width and height from bytes 12-19
     * (VK 0x140d6c700, copied by 0x140d9d140). A shorter file cannot be read as an image. */
    return installed && length >= 20 && ie_be32(installed) == 0xffffffffu &&
        ie_be32(installed + 12) == 8 && ie_be32(installed + 16) == 8;
}

static int ie_query_engine(const char *name, const unsigned char *installed, size_t length);
typedef struct ie_query_call { const char *name; const unsigned char *installed; size_t length; } ie_query_call;
static intptr_t ie_query_thunk(void *p) { ie_query_call *c = p; return ie_query_engine(c->name, c->installed, c->length); }
int sh_image_exclusion_query(const char *name, const unsigned char *installed, size_t length)
{
    ie_query_call call = {name, installed, length};
    if (sh_prepare_on_worker()) return (int)sh_prepare_engine(ie_query_thunk, &call);
    return ie_query_engine(name, installed, length);
}
static int ie_query_engine(const char *name, const unsigned char *installed, size_t length)
{
    int result = -1;
    if (!name || !*name) return 0;
    if (!g_ie.installed) return -1;
    if (!ie_main_thread()) {
        ie_log("image availability: queried off the engine main thread; availability is unknown", &g_ie.reported_thread);
        return -1;
    }
    void *lock = NULL; int held = 0;
    __try {
        lock = g_ie.lock();
        held = lock && g_ie.acquire(lock, 1);
        if (held) result = ie_lookup(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; }
    if (held) __try { g_ie.release(lock); } __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; }
    if (result < 0) ie_log("image availability: the native image registry could not be read; availability is unknown",
        &g_ie.reported_fault);
    /* A binary this header describes, read from a source whose Timestamp() is all ones, would leave
     * exactly this state, so the evidence cannot tell the two histories apart: unknown. (A binary read
     * from any other source writes that timestamp at +0x3c and is already reported 0, usable.) */
    if (result == 1 && sh_image_exclusion_header_ambiguous(installed, length)) result = -1;
    return result;
}

#ifdef SH_IMAGE_EXCLUSION_TESTING
void sh_image_exclusion_test_bind(const sh_image_exclusion_test_native *native)
{
    memset(&g_ie, 0, sizeof(g_ie));
    if (!native) return;
    g_ie.head = native->head; g_ie.lock = native->lock; g_ie.acquire = native->acquire;
    g_ie.release = native->release; g_ie.test_other_thread = !native->main_thread;
    g_ie.installed = g_ie.head && g_ie.lock && g_ie.acquire && g_ie.release &&
        sh_image_exclusion_layout_of(!native->opengl, &g_ie.layout);
}
#endif
