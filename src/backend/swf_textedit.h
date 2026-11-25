/* Clipboard shortcuts for focused SnapMap SWF text fields via an onKey::Call detour. Ctrl state comes from
 * GetAsyncKeyState because focus changes can drop modifier releases. */
#ifndef BACKEND_SWF_TEXTEDIT_H
#define BACKEND_SWF_TEXTEDIT_H

#include <stdint.h>

/* Install once after the handler and type resolve. Ctrl+C copies the selection or whole field; Ctrl+V
 * replaces the selection (a paste cut short by the field limit toasts); without idStr assignment only copy
 * works. Failures are logged. */
void sh_swf_textedit_install(const uint8_t *module_base);

/* Remove the detour if installed. */
void sh_swf_textedit_uninstall(void);

#endif /* BACKEND_SWF_TEXTEDIT_H */
