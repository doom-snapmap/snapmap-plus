/* Native image-exclusion state for availability under a declared requirement (R-309).
 *
 * idImage Load (VK 0x140d9d240 / GL 0x140d94100) is reached only through GenericLoad
 * (VK 0x1417ff5f0 / GL 0x1417f1f40), which first sets the resource's +0x24 bit 0. Load builds a
 * placeholder, and reads no binary, for a name starting with '_' or one the image blacklist matcher
 * (VK 0x14031cf20 / GL 0x14031c7d0) accepts: a fixed option block (state 3, 8 x 8, +0x90 5, +0x94 0,
 * +0xb0 13.0f), with the constructor's 0xffffffff sentinel left at +0x38/+0x3c. Otherwise it reads
 * the binary, and the binary paths (VK 0x140d9d140 / GL 0x140d94000) write +0x38 from the header's
 * first dword and the option block, including width and height, from the header; a failed read
 * makes GenericLoad set the default flag (+0x2c bit 2) before MakeDefault. Generated images
 * (idImageManager::AllocImage, find-or-create with options) never set bit 0.
 *
 * A binary read reaches the image through the open-into helper (VK 0x1417ffc70), which gives the
 * reader the source file's own Timestamp() (+0x98); the binary reader stores it, and the binary path
 * copies it to +0x3c. So +0x3c other than all ones proves a binary was read: such an image is usable.
 *
 * Stock image names load only their installed binaries (changed bytes take private names). The
 * native reader accepts any first header dword, so the query also takes that name's installed
 * binary. When its header cannot produce the state (first dword other than 0xffffffff, or a size
 * other than 8 x 8), the registered object of the name shows all of these properties only when the
 * matcher excluded it at its own load. When it can, a binary read from a source whose Timestamp() is
 * all ones would leave identical evidence, and the answer is unknown. The query reads the current
 * state; no history is kept and nothing is persisted.
 *
 * Every query walks the native image list under the native resource-list lock: the
 * CRITICAL_SECTION every native add (VK 0x141800700), remove (0x141801ae0) and purge driver
 * (VK 0x141800e10 / GL 0x1417f3760) holds, decoded from that purge driver. */
#ifndef SH_IMAGE_EXCLUSION_H
#define SH_IMAGE_EXCLUSION_H
#include <stddef.h>
#include "signatures.h"

/* Where the exclusion branch's fixed option words land in an idImage: format (5), the word after it
 * (0) and the bias (13.0f). Load copies its option block to +0x54 on both renderers, but the block
 * differs after its size words: Vulkan +0x90/+0x94/+0xb0, OpenGL +0x8c/+0x90/+0xac (R-389; read
 * from the exclusion branches VK 0x140d9d58d and GL 0x140d9444d and the creation routines VK
 * 0x140dadb70 and GL 0x140da4610). */
typedef struct sh_image_exclusion_layout { size_t format, usage, bias; } sh_image_exclusion_layout;
/* vulkan 1 (DOOMx64vk.exe) or 0 (DOOMx64.exe); returns 0 for anything else, such as an unresolved host. */
int sh_image_exclusion_layout_of(int vulkan, sh_image_exclusion_layout *out);
/* The verified native dependencies, as addresses in the running image. */
typedef struct sh_image_exclusion_native {
    const void *lock, *acquire, *release;  /* resource-list lock getter, blocking acquire, release */
    const void *head;                      /* list-of-lists head the purge driver walks */
} sh_image_exclusion_native;
/* Decode from the purge driver (signature ResourcePurgeLevel); 1 when every expected instruction
 * is present. No native call. */
int sh_image_exclusion_decode(const unsigned char *purge, sh_image_exclusion_native *out);
/* Resolve, decode, and require the decoded head to equal the globals table's decl_resource_head and
 * the running renderer to be known (its option block layout). Idempotent; 1 when every dependency
 * verified. */
int sh_image_exclusion_install(const sig_result *results, size_t count);
/* installed/length: the name's installed binary as the compile serves it (NULL when absent).
 * 1: the registered image of this name is a native exclusion placeholder. 0: no image of this name
 * is registered, or the registered one is not (usable, defaulted or generated). -1: unknown, because
 * the binding is unavailable, the caller is not the engine main thread, the native lock or image
 * registry cannot be used, or the installed header leaves the placeholder state ambiguous. Unknown is
 * never reported as usable. */
int sh_image_exclusion_query(const char *name, const unsigned char *installed, size_t length);
/* Whether a binary with this header, once read, would leave the same +0x38 and size as the
 * exclusion branch: first big-endian dword 0xffffffff, width and height (big-endian, bytes 12-19) 8. */
int sh_image_exclusion_header_ambiguous(const unsigned char *installed, size_t length);

#ifdef SH_IMAGE_EXCLUSION_TESTING
/* A fake native environment: list-of-lists head and the lock getter/acquire/release the native
 * code uses. NULL unbinds. main_thread 0 simulates a call from another thread. */
typedef struct sh_image_exclusion_test_native {
    void *const *head;
    void *(*lock)(void);
    unsigned (*acquire)(void *lock, char blocking);
    void (*release)(void *lock);
    int main_thread;
    int opengl; /* 1 reads OpenGL's option block layout; 0 (the default) Vulkan's */
} sh_image_exclusion_test_native;
void sh_image_exclusion_test_bind(const sh_image_exclusion_test_native *native);
#endif
#endif
