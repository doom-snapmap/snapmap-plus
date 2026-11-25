#pragma once
#include <windows.h>
#include <stdint.h>

/* Returns zero on a fault. A partial copy may already have occurred. */
static __inline int sh_safe_read(const void *source, void *destination, size_t n)
{
    const uint8_t *src = (const uint8_t *)source;
    uint8_t *dst = (uint8_t *)destination;
    __try {
        for (size_t i = 0; i < n; ++i) dst[i] = src[i];
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
