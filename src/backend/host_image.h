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

/* Snapmap+ supports one DOOM build: the April 2024 update, Steam build 13954591.
 * The host is identified by the SHA-256 of its executable file, never by its
 * name, size or loaded bytes. Any other executable (the 2018 build included) is
 * unsupported: the backend logs that once and only forwards XInput. */
typedef struct sh_build_identity {
    unsigned char  sha256[32];
    int            vulkan;      /* 1 Vulkan executable, 0 OpenGL executable */
    const char    *label;       /* "13954591-vk" / "13954591-gl", for logs */
    const char    *steam_build; /* Steam build id: "13954591" */
    const char    *binary;      /* the console's Build Information binary name */
} sh_build_identity;

/* Supported executables, terminated by an entry with a NULL label. */
extern const sh_build_identity SH_KNOWN_BUILDS[];

/* Look a digest up in SH_KNOWN_BUILDS; NULL when it is not a supported executable. */
const sh_build_identity *sh_host_identity_for_digest(const unsigned char digest[32]);

/* SHA-256 of a file's bytes. Returns 1 on success. Opening without write
 * sharing pins the bytes being read. */
int sh_host_file_sha256(const wchar_t *path, unsigned char digest[32]);

/* The live host's identity (hashes the executable once), or NULL. */
const sh_build_identity *sh_host_identity(void);
int sh_host_is_supported_build(void);

/* The one game-identity string every report carries, e.g.
 *   DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)
 * (Steam build, binary name, renderer of the executable, first eight hex
 * digits of its SHA-256). Writes it and returns 1 for a supported executable;
 * otherwise writes "" and returns 0. */
#define SH_GAME_IDENTITY_MAX 96
int sh_build_game_identity(const sh_build_identity *identity, char *out, size_t cap);
int sh_host_game_identity(char *out, size_t cap);

/* First eight hex digits of the host executable's SHA-256 (also for an
 * unsupported executable), or "" when it could not be hashed. */
const char *sh_host_executable_hash8(void);

/* Identity of an image being scanned: the live host when base is its mapped
 * base, a test-bound image in test builds, otherwise NULL. Raw RVAs are only
 * ever applied to an image this returns non-NULL for. */
const sh_build_identity *sh_host_image_identity(const uint8_t *base);
int sh_host_image_is_supported(const uint8_t *base);

/* Pick the renderer's value from a Vulkan/OpenGL RVA pair: for an image
 * (sh_host_image_rva) or for the live host (sh_host_rva). 0 when the image or
 * host is not a supported executable, so callers must treat 0 as "none". */
uint32_t sh_host_image_rva(const uint8_t *base, uint32_t rva_vk, uint32_t rva_gl);
uint32_t sh_host_rva(uint32_t rva_vk, uint32_t rva_gl);

#ifdef SH_HOST_IMAGE_TESTING
/* Bind an offline mapped image to the identity of its source file's SHA-256.
 * Returns the identity (NULL binds nothing). NULL base unbinds. */
const sh_build_identity *sh_host_test_bind_image(const uint8_t *base, const wchar_t *path);
/* Bind a synthetic buffer to a known identity without a file (unit tests). */
void sh_host_test_bind_identity(const uint8_t *base, const sh_build_identity *identity);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_HOST_IMAGE_H */
