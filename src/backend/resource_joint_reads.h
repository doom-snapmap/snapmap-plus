#ifndef SH_RESOURCE_JOINT_READS_H
#define SH_RESOURCE_JOINT_READS_H
#include <stdint.h>
/* Observe the registered skeleton/model only at the native name-token read.
 * Empty or punctuation-only GetJointList calls consume neither resource here.
 * Eligible independent native scopes above source/presentation barriers remain
 * observable. Readiness verifies current owned detours and the pinned renderer;
 * reached inputs retain their exact observed lifetime throughout the call. */
int sh_resource_joint_reads_install(const uint8_t *base,int (*observe)(void *));
int sh_resource_joint_reads_ready(void);
#endif
