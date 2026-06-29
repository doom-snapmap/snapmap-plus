/* veh.h -- the fault-shield's vectored exception handler (raw AV -> recoverable Error(6)). */
#ifndef SHIELD_VEH_H
#define SHIELD_VEH_H

int veh_install(void);   /* AddVectoredExceptionHandler(first=1); returns 1 on success. */

#endif /* SHIELD_VEH_H */
