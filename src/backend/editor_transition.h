/* Package activation within one explicitly requested native editor reload. */
#ifndef SH_EDITOR_TRANSITION_H
#define SH_EDITOR_TRANSITION_H
#include "signatures.h"

typedef int (*sh_editor_transition_load_fn)(void *editor, const void *name);
typedef struct sh_editor_transition_callbacks {
    void *context;
    int (*activate)(void *context);
    int (*commit)(void *context);
} sh_editor_transition_callbacks;

int sh_editor_transition_bind(const sig_result *results, size_t count, const uint8_t *base);
int sh_editor_transition_install(const sig_result *results, size_t count, const uint8_t *base);
/* Non-NULL identifies the current map in a reloadable editor. Capture before
 * consent, then require the same value and rawmap generation when continuing. */
void *sh_editor_transition_capture(const void *editor);
/* Only the editor-frame detour may bracket the point before native Think has
 * produced frame output. Enter never changes the editor or loads a map. */
int sh_editor_transition_frame_enter(void *editor);
void sh_editor_transition_frame_leave(void *editor);
/* The retained request still names the same map and owns the pre-Think frame. */
int sh_editor_transition_frame_ready(void *editor, void *previous_map);
/* Same return convention as native LoadMap: zero means completion. Activates
 * after native old-map destruction, releases renderer ownership after the full
 * native load, then commits installation last. Both callbacks run synchronously. */
int sh_editor_transition_load(void *editor, void *previous_map, const void *name,
    sh_editor_transition_load_fn load, const sh_editor_transition_callbacks *callbacks,
    char *error, size_t capacity);
/* True only inside the activation callback after verified native teardown. */
int sh_editor_transition_at_boundary(void);
#endif
