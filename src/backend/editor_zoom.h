/* Blueprint zoom-out, a client-side default: repoints the orbit dolly, pan and sky-view constant loads at its own
 * floats so the camera reaches `scale` times the native height (default 2.75, range 1..4), and raises the far plane,
 * environment volume, ground plane and module-pick length to match. The editor's far-clip condition calls view
 * objects, so it is sampled on the main thread (sh_editor_zoom_tick) and job threads read only the sample. Above the
 * environment volume (+/-33000) the blend stops writing r_zfar, so height without the editor-write redirect is capped
 * at SH_ZOOM_BLEND_CEILING; the pick length keeps the native 50000:33000 ratio to the far plane. */
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
/* Highest Blueprint height when only the environment blend raises r_zfar; it stops above
 * snapEdit_environmentFogBounds (33000). */
#define SH_ZOOM_BLEND_CEILING   32000.0f
/* snapEdit_environmentFogBounds default, and the cube render entity scale inside it. */
#define SH_ZOOM_NATIVE_FOG      33000.0f
#define SH_ZOOM_NATIVE_FOG_CUBE 32768.0f
/* Highest Blueprint bound (the map bounds ceiling, SH_BOUNDS_CEILING). */
#define SH_ZOOM_MAX_BOUNDS      32671.0f
/* snapEdit_modulePickingDistance default: module hover and select ray length. */
#define SH_ZOOM_NATIVE_PICK     50000.0f
/* idRenderWorldLocal vtable, checked before the ground plane at editor+0x1a0 is touched. */
#define SH_ZOOM_VT_RENDER_WORLD SH_RVA(0x28BBF78u, 0x28D5738u)

/* idSnapView subclasses of the editor camera (vtable RVAs, build 13954591;
 * symbols idSnapFreeOrbitView / idSnapSkyView / idSnapFPSView::vftable). */
#define SH_ZOOM_VT_ORBIT  SH_RVA(0x235FD50u, 0x2379AA0u)
#define SH_ZOOM_VT_SKY    SH_RVA(0x235FDA8u, 0x2379AF8u)
#define SH_ZOOM_VT_FPS    SH_RVA(0x238DF20u, 0x23A7C70u)

/* Pure helpers; bounds is the active module bound. */
float sh_editor_zoom_clamp_scale(float scale);
float sh_editor_zoom_orbit_height(float bounds, float scale);
float sh_editor_zoom_pan_limit(float bounds, float scale);
float sh_editor_zoom_sky_factor(float scale);
float sh_editor_zoom_blueprint_far(float bounds, float scale, float blended);
/* The Blueprint far plane raised to floor_value (at most SH_ZOOM_FAR_MAX);
 * a floor of 0 leaves it unchanged. */
float sh_editor_zoom_far_with_floor(float far_plane, float floor_value);
/* The orbit height the camera may reach: unchanged when the editor's far plane write is redirected, else at most
 * SH_ZOOM_BLEND_CEILING. */
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
/* Mirror of the editor's blueprint blend (VK FUN_14123c510) from the current and previous views' blueprint flags,
 * whether the current view is in transition and its transition fraction. */
float sh_editor_zoom_blend(int current_blueprint, int previous_blueprint, int transitioning, float fraction);

/* The install's byte checks over each site's address: 1 when the bytes are the expected ones; the decoded operand
 * address is returned for the install to compare against its cvar. */
int  sh_editor_zoom_check_core(uintptr_t dolly, uintptr_t pan, uintptr_t sky, uintptr_t *half);
int  sh_editor_zoom_check_far(uintptr_t a, uintptr_t *zfar_at);
int  sh_editor_zoom_check_pick(uintptr_t a, uintptr_t *value_at);
int  sh_editor_zoom_check_fog(uintptr_t a, uintptr_t *fog_at);
int  sh_editor_zoom_install(const sig_result *results, size_t count, const uint8_t *base);
void sh_editor_zoom_set_scale(float scale);
/* The active module bound changed (map bounds). */
void sh_editor_zoom_update(float module_bounds);
/* Any thread. The lowest Blueprint far plane the open map asks for: SH_ZOOM_FAR_MAX while it unlocks its view
 * distance, else 0. Both Blueprint r_zfar writers use at least this value. */
void sh_editor_zoom_set_far_floor(float floor_value);
/* Main thread, once per editor frame: samples whether the editor camera update forces r_zfar this frame. */
void sh_editor_zoom_tick(void);
/* Any thread. Returns blended unchanged unless the last editor-frame sample (at most a quarter second old) says the
 * editor camera update forces r_zfar; then at least the Blueprint far plane. Calls no engine code. */
float sh_editor_zoom_far_plane(float blended);
#endif
