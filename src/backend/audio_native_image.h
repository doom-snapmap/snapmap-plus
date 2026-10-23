#ifndef SH_AUDIO_NATIVE_IMAGE_H
#define SH_AUDIO_NATIVE_IMAGE_H
#include "signatures.h"
#include <string.h>

/* Native audio adapters resolve code and storage from independently scanned
 * anchors. This checks mapped PE extents without reading live engine objects. */
static int ani_section(const unsigned char *image,const void *address,size_t length,DWORD required,DWORD forbidden)
{
    if(!image || !address || (uintptr_t)address<(uintptr_t)image)return 0;
    uintptr_t rva=(uintptr_t)address-(uintptr_t)image;
    __try {
        const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)image;
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0)return 0;
        const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(image+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            rva>=nt->OptionalHeader.SizeOfImage || length>nt->OptionalHeader.SizeOfImage-rva)return 0;
        const IMAGE_SECTION_HEADER *section=IMAGE_FIRST_SECTION(nt);
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++,section++) {
            size_t span=section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
            if(rva<section->VirtualAddress || rva-section->VirtualAddress>=span ||
                length>span-(rva-section->VirtualAddress))continue;
            return (section->Characteristics&required)==required && !(section->Characteristics&forbidden);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
static void *ani_rip(const unsigned char *p,const unsigned char *prefix,size_t n)
{
    int32_t relative;if(memcmp(p,prefix,n))return NULL;memcpy(&relative,p+n,4);
    return (void *)((uintptr_t)(p+n+4)+(intptr_t)relative);
}
#endif
