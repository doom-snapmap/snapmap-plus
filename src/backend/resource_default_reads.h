/* Per-invocation native default cleanup read contracts. */
#ifndef SH_RESOURCE_DEFAULT_READS_H
#define SH_RESOURCE_DEFAULT_READS_H
#include <stddef.h>
#include <stdint.h>

typedef int (*sh_resource_default_safe_read)(void *context, uintptr_t address, void *out, size_t size);
typedef enum sh_resource_default_read_reason {
    SH_DEFAULT_READ_OK = 0,
    SH_DEFAULT_READ_INPUT,
    SH_DEFAULT_READ_UNSUPPORTED,
    SH_DEFAULT_READ_IMAGE,
    SH_DEFAULT_READ_PIN,
    SH_DEFAULT_READ_MEMORY,
    SH_DEFAULT_READ_TYPE,
    SH_DEFAULT_READ_CALLBACK,
    SH_DEFAULT_READ_BOUNDS,
    SH_DEFAULT_READ_OWNERSHIP,
    SH_DEFAULT_READ_BUDGET,
    SH_DEFAULT_READ_ALLOCATION,
    SH_DEFAULT_READ_INVOCATION,
    SH_DEFAULT_READ_NATIVE_RETURN,
    SH_DEFAULT_READ_ORDER
} sh_resource_default_read_reason;

typedef enum sh_resource_default_read_routine {
    SH_DEFAULT_READ_ROUTINE_UNSUPPORTED = 0,
    SH_DEFAULT_READ_ROUTINE_FILE,
    SH_DEFAULT_READ_ROUTINE_JSON,
    SH_DEFAULT_READ_ROUTINE_ANIMPARA,
    SH_DEFAULT_READ_ROUTINE_DECLARATION
} sh_resource_default_read_routine;

typedef struct sh_resource_default_read_input {
    uintptr_t image_base;
    size_t image_size;
    int renderer;                 /* 0 OpenGL, 1 Vulkan; never inferred. */
    int pinned_image;             /* Established by the host image verifier. */
    uintptr_t resource;
    uintptr_t default_callback;
    sh_resource_default_safe_read read;
    void *read_context;
    size_t work_limit;            /* Zero: no artificial limit. Exhaustion is incomplete. */
} sh_resource_default_read_input;

typedef struct sh_resource_default_read_receipt {
    sh_resource_default_read_input invocation;
    sh_resource_default_read_reason reason;
    sh_resource_default_read_routine routine;
    uintptr_t offending_address;
    uint32_t offending_type;
    size_t work;
    int complete;
    unsigned state;              /* Per-invocation, consumed once by finish. */
} sh_resource_default_read_receipt;

/* A failed probe never authorizes skipping or changing the native default.
 * The caller owns nesting/order and must keep a separate stack receipt for
 * each invocation. Only the caller's pinned image and safe reader are used.
 * Known callbacks are qualified by implementation and reached ownership,
 * without resource-family, package, or telemetry-name special cases. */
int sh_resource_default_read_begin(sh_resource_default_read_receipt *receipt,
                                  const sh_resource_default_read_input *input);
/* Call after the native operation, while this same request frame is current.
 * The original input identity must still match. native_returned and in_order
 * describe the actual call, not whether the pre-probe happened to pass. */
int sh_resource_default_read_finish(sh_resource_default_read_receipt *receipt,
                                   const sh_resource_default_read_input *input,
                                   int native_returned, int in_order);
#endif

