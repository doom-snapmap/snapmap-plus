/* Guard editor wire-render node references. The legacy palette_guard name
 * remains, but this service sanitizes unreadable output/input node pointers
 * before the wire resolver walks them.
 */
#ifndef B2_PALETTE_GUARD_H
#define B2_PALETTE_GUARD_H

#include <stdint.h>

/* Install the render-node guard at the pinned Vulkan resolver RVA.
 * module_base must identify that supported image. One-shot; returns 1 when
 * armed, 0 on failure.
 */
int sh_palette_guard_install(const uint8_t *module_base);

#endif /* B2_PALETTE_GUARD_H */
