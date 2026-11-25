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

/* Scalar reads assign the output only after the complete engine value is readable. */
static __inline int sh_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __inline int sh_read_i32(const void *src, int *out)
{
    __try { *out = *(const int *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __inline int sh_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __inline int sh_read_u8(const void *src, uint8_t *out)
{
    __try { *out = *(const uint8_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __inline int sh_read_float(uintptr_t address, float *out)
{
    __try { *out = *(const volatile float *)address; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
