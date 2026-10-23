/* Actual table reads made by the native material parameter evaluator. */
#ifndef SH_RESOURCE_PARM_READS_H
#define SH_RESOURCE_PARM_READS_H
#include <stdint.h>
#include <stddef.h>

/* The observer copies the live resource identity into the current dependency
 * frame. NULL means an actual table read could not be identified and must make
 * that frame incomplete. No resource address is retained by this module.
 * Install only at the native resource-hook installation boundary. */
int sh_resource_parm_reads_install(const uint8_t *base, int (*observe)(void *));
int sh_resource_parm_reads_ready(void);
/* Exact actual expression temporary/operand reads. The install is composed by
 * the main parm installer; readiness includes current four owned detours and
 * untouched native bodies. Callback borrows objects and copies their current
 * origin into the caller's consumer; no native resource address is retained. */
int sh_resource_parm_expression_reads_install(const uint8_t *base,int (*observe)(void *));
int sh_resource_parm_expression_reads_ready(void);
#ifdef SH_RESOURCE_PARM_READS_TESTING
void sh_resource_parm_reads_test_observe(const void *operation, void *manager);
void sh_resource_parm_reads_test_reset(void);
#endif
#endif
