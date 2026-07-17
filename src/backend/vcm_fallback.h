/* vcm_fallback.h -- the static corpus-snapshot fallback behind the class/inherit dropdowns (see
 * vcm_fallback.c). Serves valid_class_map.h when the live type registry / entityDef manager is
 * unreachable on the running build. Pure logic -- unit-tested by tests\vcm_fallback_test.c. */
#ifndef B2_VCM_FALLBACK_H
#define B2_VCM_FALLBACK_H

/* pack a NUL string into out_buf at *pw (double-NUL-able); bumps *pw + returns 1 if it fit, else 0. */
int sh_vcm_pack(char *out_buf, int cap, int *pw, const char *s);

/* Valid classes for `inherit` from the snapshot (inherit -> Y -> classes). 1 = inherit known. */
int sh_vcm_fallback_valid_classes(const char *inherit, char *out_buf, int cap, int *written, int *names);

/* The full snapshot inherit set, packed (double-NUL end). 1 = >=1 name. */
int sh_vcm_fallback_inherits(char *out_buf, int cap, int *out_count);

#endif
