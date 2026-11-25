#pragma once
#include <windows.h>
#include <stdint.h>

/* Only the 64-bit PE headers used by the mapped game image are accepted. */
static __inline const IMAGE_NT_HEADERS64 *sh_pe_headers(const uint8_t *base)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return NULL;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? nt : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static __inline uint32_t sh_pe_image_size(const uint8_t *base)
{
    const IMAGE_NT_HEADERS64 *nt = sh_pe_headers(base);
    __try { return nt ? nt->OptionalHeader.SizeOfImage : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __inline uint32_t sh_pe_section_span(const IMAGE_SECTION_HEADER *section)
{
    return section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
}
