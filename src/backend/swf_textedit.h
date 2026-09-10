/* Clipboard shortcuts for focused SnapMap SWF text fields. The onKey::Call
 * detour obtains the active field directly; Ctrl state comes from GetAsyncKeyState
 * because focus changes can drop modifier releases. Field offsets must be
 * rechecked against that handler when porting engine builds. */
#ifndef BACKEND_SWF_TEXTEDIT_H
#define BACKEND_SWF_TEXTEDIT_H

#include <stdint.h>

/* Install once after clean handler/type resolution. Ctrl+C copies the selection
 * or whole field; Ctrl+V replaces the selection. If idStr assignment does not
 * resolve, only copy is enabled. Failures are logged. */
void sh_swf_textedit_install(const uint8_t *module_base);

/* Remove the detour if installed. */
void sh_swf_textedit_uninstall(void);

#endif /* BACKEND_SWF_TEXTEDIT_H */
