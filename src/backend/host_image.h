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

/* Permit pinned-RVA fallbacks for a host named DOOMx64vk.exe.
 * This checks the basename only, not the extraction-build hash or version.
 * Every raw-RVA fallback must use this gate to exclude the OpenGL image. */
int sh_host_is_pinned_rva_build(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_HOST_IMAGE_H */
