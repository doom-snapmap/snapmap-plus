/* Blueprint zoom-out: a client-side Snapmap+ default (no map data changes).
 *
 * In Module mode (editor state 1) the editor uses the free orbit view
 * (camera mode 0). Its dolly clamps the camera Z to
 * +/-snapEdit_cameraDistanceLimit (16000) and its pan clamps X, Y and Z to
 * +/-snapEdit_environmentModuleBounds. The sky view (mode 1) clamps its height
 * to bounds * 0.5. This module repoints those three constant loads at its own
 * floats, so the camera can reach `scale` times the native height (default
 * 2.75, range 1..4; the native height follows unlocked map bounds). The
 * default fits the whole map square in the top-down view: the Blueprint camera
 * sees about 0.40 x its height to the top and bottom screen edges at 16:9
 * (measured live, R-009), so 2.75 x bounds shows +/-1.1 x bounds.
 *
 * Far clip: while the blueprint blend is active the editor camera update
 * forces r_zfar to 33000 every frame and the environment blend writes the
 * map distance back on job threads, so the renderer sees whichever ran last.
 * Both writers are raised to the same Blueprint far plane (33000 * the zoom
 * height ratio): the editor's two 33000 loads are repointed at the published
 * value, and sh_editor_zoom_far_plane() is called from the blend's
 * maxViewDistance read (map_render_native.c) under exactly the editor's own
 * condition. Above the editor environment volume (+/-33000) the blend stops
 * writing, so there the editor's write alone decides; without it the height
 * is capped at SH_ZOOM_BLEND_CEILING.
 *
 * Editor environment volume: the editor render-world setup (VK 0x123e210)
 * builds a _unitCube render entity scaled 32768 and an environment area of
 * +/-snapEdit_environmentFogBounds (33000) once. A camera outside them looks
 * through the cube's faces and sees a thick blue fog, and the environment
 * blend stops. The builder (EditorFogVolume) is patched at install to size
 * both for the highest reachable camera (SH_ZOOM_FOG_BOUNDS), so the camera
 * never leaves them on any map.
 *
 * Blueprint ground plane: the same setup scales a _unitSquare render entity
 * (editor+0x1a0) to +/-snapEdit_environmentModuleBounds once. When map bounds
 * change, sh_editor_zoom_update() rescales it and marks it for update.
 *
 * Module picking: the Blueprint hover pick (VK 0x11f45a0, called from
 * GridBlueprintUpdate through the editor's hover update) traces each module
 * from the cursor ray origin to origin + dir * snapEdit_modulePickingDistance
 * (50000), and the Module Mode focus ray (VK 0x1257870) uses the same length.
 * A camera more than 50000 units from a module could not hover or select it.
 * Both cvar loads are repointed at a published pick length that keeps the
 * native 50000:33000 ratio to the Blueprint far plane, so anything drawn can
 * be picked (R-011). */
#ifndef SH_EDITOR_ZOOM_H
#define SH_EDITOR_ZOOM_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_ZOOM_NATIVE_HEIGHT   16000.0f  /* snapEdit_cameraDistanceLimit default */
#define SH_ZOOM_NATIVE_BOUNDS   16000.0f  /* snapEdit_environmentModuleBounds default */
#define SH_ZOOM_SKY_FACTOR      0.5f      /* sky-view height = bounds * factor */
#define SH_ZOOM_EDITOR_FAR      33000.0f  /* r_zfar the editor forces in Blueprint */
#define SH_ZOOM_FAR_MAX         200000.0f /* r_zfar's registered maximum */
#define SH_ZOOM_SCALE_DEFAULT   2.75f
#define SH_ZOOM_SCALE_MAX       4.0f
/* Highest Blueprint height when only the environment blend can raise r_zfar:
 * the blend stops writing above snapEdit_environmentFogBounds (33000). */
#define SH_ZOOM_BLEND_CEILING   32000.0f
/* Editor environment volume (snapEdit_environmentFogBounds default) and the
 * cube render entity the builder scales to 32768 inside it. */
#define SH_ZOOM_NATIVE_FOG      33000.0f
#define SH_ZOOM_NATIVE_FOG_CUBE 32768.0f
/* Highest Blueprint bound (map bounds ceiling, as SH_BOUNDS_CEILING). */
#define SH_ZOOM_MAX_BOUNDS      32671.0f
/* snapEdit_modulePickingDistance default: module hover and select ray length. */
#define SH_ZOOM_NATIVE_PICK     50000.0f
/* idRenderWorldLocal vtable (build 13954591), checked before the ground
 * plane at editor+0x1a0 is touched. */
#define SH_ZOOM_VT_RENDER_WORLD SH_RVA(0x28BBF78u, 0x28D5738u)

/* idSnapView subclasses of the editor camera (vtable RVAs, build 13954591;
 * symbols idSnapFreeOrbitView / idSnapSkyView / idSnapFPSView::vftable). */
#define SH_ZOOM_VT_ORBIT  SH_RVA(0x235FD50u, 0x2379AA0u)
#define SH_ZOOM_VT_SKY    SH_RVA(0x235FDA8u, 0x2379AF8u)
#define SH_ZOOM_VT_FPS    SH_RVA(0x238DF20u, 0x23A7C70u)

/* Pure helpers (tests). bounds is the active module bound. */
float sh_editor_zoom_clamp_scale(float scale);
float sh_editor_zoom_orbit_height(float bounds, float scale);
float sh_editor_zoom_pan_limit(float bounds, float scale);
float sh_editor_zoom_sky_factor(float scale);
float sh_editor_zoom_blueprint_far(float bounds, float scale, float blended);
/* The orbit height the camera may reach: unchanged when the editor's own far
 * plane write is redirected, else at most SH_ZOOM_BLEND_CEILING. */
float sh_editor_zoom_capped_height(float height, int editor_far_plane);
/* Editor environment volume half-size for a camera that can reach `reach`
 * (orbit pan limit): the native 33000:32000 margin, never below 33000. */
float sh_editor_zoom_fog_bounds(float reach);
/* The volume Snapmap+ builds: room for the highest camera at the ceiling
 * bounds and the largest scale. */
float sh_editor_zoom_fog_volume(void);
/* Cube entity scale inside a volume of that size (native 32768 : 33000). */
float sh_editor_zoom_fog_cube(float fog_bounds);
/* Height the camera may reach inside an environment volume of fog_bounds. */
float sh_editor_zoom_volume_height(float height, float fog_bounds);
/* Module picking ray length for the Blueprint far plane at bounds and scale:
 * the native 50000:33000 pick-to-far ratio, never below 50000, and never
 * below `configured` (the cvar's value, when it was raised by hand). */
float sh_editor_zoom_pick_distance(float bounds, float scale, float configured);
/* Mirror of the editor's blueprint blend (VK FUN_14123c510): current and
 * previous views' blueprint flags, whether the current view is still in its
 * transition and its transition fraction. */
float sh_editor_zoom_blend(int current_blueprint, int previous_blueprint, int transitioning, float fraction);

int  sh_editor_zoom_install(const sig_result *results, size_t count, const uint8_t *base);
void sh_editor_zoom_set_scale(float scale);
/* The active module bound changed (map bounds). */
void sh_editor_zoom_update(float module_bounds);
/* Any thread. Returns blended unchanged unless the editor camera update would
 * force r_zfar this frame; then at least the Blueprint far plane. */
float sh_editor_zoom_far_plane(float blended);
#endif
