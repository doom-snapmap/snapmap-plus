/* Console toggle for hidden SnapMap palette entries and their wire visibility.
 * Sets or clears decl visibility bits, leaving idInfoPath visible when hiding. */
#ifndef BACKEND_TARGET_ANY_H
#define BACKEND_TARGET_ANY_H

struct idCmdArgs;

/* Cache GetDeclsOfType (resolved in dllmain) so the toggle can enumerate the decl registry. Call once. */
void sh_target_any_install(void *get_decls_of_type);

/* Registered console handler; alternate between revealing and hiding entries. */
void h_target_any(struct idCmdArgs *a);

/* Return the reveal state; the connect-tool detours use it to allow any target. */
int sh_target_any_is_shown(void);

#endif /* BACKEND_TARGET_ANY_H */
