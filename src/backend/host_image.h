/* host_image.h -- resolve the DOOM image this DLL is loaded into, whichever build it is.
 *
 * DOOM 2016 ships two executables built from one source tree, linked one second apart:
 * DOOMx64vk.exe (imports vulkan-1.dll) and DOOMx64.exe (imports OPENGL32.dll). Their import
 * tables are otherwise identical, both import XINPUT1_3.dll, and the game switches between
 * them by relaunching itself when r_renderAPI changes -- so a user can land in either one
 * from the same Steam launch, and we get loaded into whichever it is.
 *
 * The backend used to find DOOM with GetModuleHandleA("DOOMx64vk.exe"), which returns NULL
 * under the OpenGL build and left the whole backend unarmed. It never needed a name: our DLL
 * is loaded BY DOOM, so the host process image IS DOOM. GetModuleHandleA(NULL) is the correct
 * and renderer-agnostic way to ask.
 *
 * The basename is still checked, but only to refuse a host that is not DOOM at all (this DLL
 * is named XINPUT1_3.dll and could be dropped beside any game). Both shipped names are accepted.
 * Real identity comes from the signature layer, which pins each engine function by its bytes.
 */
#ifndef BACKEND_HOST_IMAGE_H
#define BACKEND_HOST_IMAGE_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* Base of the host DOOM image, or NULL if the host is not a DOOM 2016 executable we ship for.
 * Idempotent and cheap after the first call; safe to call from any thread. */
const uint8_t *sh_host_image_base(void);

/* SizeOfImage from the host's PE headers, or 0 if unresolved. */
size_t sh_host_image_size(void);

/* Basename of the host executable ("DOOMx64.exe" / "DOOMx64vk.exe"), or "" if unresolved.
 * Use this for log lines and crash records so a report names the build it came from. */
const char *sh_host_image_name(void);

/* 1 when the host is running the Vulkan renderer, 0 when OpenGL, -1 when unresolved.
 * Decided by which renderer library is loaded, not by the executable name. Only the handful
 * of genuinely renderer-dependent probes should consult this -- everything else in the
 * backend is renderer-blind and must stay that way. */
int sh_host_is_vulkan(void);

/* Non-zero only when the host is the exact build every pinned `known_rva` in this product was
 * extracted from (the Vulkan image documented in signatures.c).
 *
 * A known_rva is a fact about one link output and nothing else. Using it as a fallback on any
 * other build publishes a pointer into unrelated code -- which is worse than publishing nothing,
 * because the caller cannot tell the difference. Every RVA backstop must be gated on this, so a
 * signature miss degrades to a clean refusal instead of a wild call. */
int sh_host_is_pinned_rva_build(void);

#endif /* BACKEND_HOST_IMAGE_H */
