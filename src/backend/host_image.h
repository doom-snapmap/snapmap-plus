/* Resolve the host DOOM image for either supported renderer.
 * GetModuleHandle(NULL) finds the process image; accepted basenames are
 * DOOMx64vk.exe and DOOMx64.exe. Engine functions are resolved by signature. */
#ifndef BACKEND_HOST_IMAGE_H
#define BACKEND_HOST_IMAGE_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* Also compiled into the C++ frontend for renderer reporting. */
#ifdef __cplusplus
extern "C" {
#endif

/* Return the accepted host image base, or NULL. Results are cached after startup. */
const uint8_t *sh_host_image_base(void);

/* SizeOfImage from the host's PE headers, or 0 if unresolved. */
size_t sh_host_image_size(void);

/* Basename of the host executable ("DOOMx64.exe" / "DOOMx64vk.exe"), or "" if unresolved.
 * Use this for log lines and crash records so a report names the build it came from. */
const char *sh_host_image_name(void);

/* Infer Vulkan (1), OpenGL (0), or unresolved (-1) from loaded renderer libraries. */
int sh_host_is_vulkan(void);

/* Return "vulkan", "opengl", or "". Cache only a definite renderer so startup
 * calls can retry; later reporting calls use the cached string without loader access. */
const char *sh_host_renderer_name(void);

/* Permit raw RVAs only when the backing executable has a known extraction-build
 * SHA-256. Both its original Steam wrapper and verified unpacked copy are known. */
int sh_host_is_pinned_rva_build(void);

/* The signature resolver must also bind that identity to the image it scans. */
int sh_host_is_pinned_rva_image(const uint8_t *base);

#ifdef SH_HOST_IMAGE_TESTING
/* Bind an offline mapped image only after checking its source file's SHA-256. */
int sh_host_test_bind_pinned_image(const uint8_t *base, const wchar_t *path);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_HOST_IMAGE_H */
