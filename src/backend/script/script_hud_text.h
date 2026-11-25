#ifndef SH_SCRIPT_HUD_TEXT_H
#define SH_SCRIPT_HUD_TEXT_H

#include "../signatures.h"

void sh_script_hud_bind(const sig_result *results, size_t count);
int sh_script_hud_text(void *player, const char *name, int slot, const char *text, const float rgba[4]);

#endif
