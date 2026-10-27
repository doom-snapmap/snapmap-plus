/* Pure Blueprint zoom limits. Compile editor_zoom.c with /DSH_EDITOR_ZOOM_PURE. */
#include "../src/backend/editor_zoom.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    /* Scale 1 is native: orbit height 16000, pan 16000, sky 0.5, far 33000. */
    assert(sh_editor_zoom_orbit_height(16000,1)==16000&&sh_editor_zoom_pan_limit(16000,1)==16000);
    assert(sh_editor_zoom_sky_factor(1)==0.5f&&sh_editor_zoom_blueprint_far(16000,1,8192)==33000);
    /* Scale 2 doubles the height on any map; pan and far plane follow. */
    assert(sh_editor_zoom_orbit_height(16000,2)==32000);
    /* The default 2.75 fits the whole square: 0.40 x height >= 1.1 x bounds. */
    assert(SH_ZOOM_SCALE_DEFAULT==2.75f);
    assert(sh_editor_zoom_orbit_height(16000,SH_ZOOM_SCALE_DEFAULT)==44000);
    assert(fabsf(sh_editor_zoom_orbit_height(32671,SH_ZOOM_SCALE_DEFAULT)-89845.25f)<1);
    assert(0.40f*sh_editor_zoom_orbit_height(32671,SH_ZOOM_SCALE_DEFAULT)>=1.09f*32671);
    assert(fabsf(sh_editor_zoom_blueprint_far(32671,SH_ZOOM_SCALE_DEFAULT,0)-185305.8f)<1);
    assert(sh_editor_zoom_blueprint_far(16000,SH_ZOOM_SCALE_DEFAULT,0)==90750);
    /* Editor environment volume: native 33000 for a camera up to 32000, the
     * same margin above; the built volume holds the highest camera. */
    assert(sh_editor_zoom_fog_bounds(0)==33000&&sh_editor_zoom_fog_bounds(32000)==33000);
    assert(sh_editor_zoom_fog_bounds(NAN)==33000);
    assert(sh_editor_zoom_fog_bounds(64000)==66000);
    assert(sh_editor_zoom_fog_volume()>=sh_editor_zoom_pan_limit(32671,SH_ZOOM_SCALE_MAX));
    assert(sh_editor_zoom_fog_volume()==134768);
    assert(sh_editor_zoom_fog_cube(33000)==32768&&sh_editor_zoom_fog_cube(0)==32768);
    assert(sh_editor_zoom_fog_cube(134768)<134768&&sh_editor_zoom_fog_cube(134768)>130684);
    /* The camera stays inside the volume it has: native 33000 keeps 32000. */
    assert(sh_editor_zoom_volume_height(89845,33000)==32000);
    assert(sh_editor_zoom_volume_height(89845,sh_editor_zoom_fog_volume())==89845);
    assert(sh_editor_zoom_volume_height(16000,0)==16000);
    assert(sh_editor_zoom_pan_limit(16000,2)==32000&&sh_editor_zoom_sky_factor(2)==1.0f);
    assert(sh_editor_zoom_blueprint_far(16000,2,8192)==66000);
    /* A larger blended or map distance is kept. */
    assert(sh_editor_zoom_blueprint_far(16000,2,100000)==100000);
    /* Unlocked bounds raise the native height; the far plane stops at r_zfar's maximum. */
    assert(sh_editor_zoom_orbit_height(32671,2)==65342&&sh_editor_zoom_pan_limit(32671,2)==65342);
    assert(fabsf(sh_editor_zoom_blueprint_far(32671,2,0)-134767.875f)<1);
    assert(sh_editor_zoom_blueprint_far(32671,4,0)==SH_ZOOM_FAR_MAX);
    /* Bounds below native and nonsense input fall back to native. */
    assert(sh_editor_zoom_orbit_height(0,1)==16000&&sh_editor_zoom_orbit_height(16000,NAN)==16000);
    assert(sh_editor_zoom_clamp_scale(0.25f)==1&&sh_editor_zoom_clamp_scale(9)==SH_ZOOM_SCALE_MAX);
    assert(sh_editor_zoom_blueprint_far(16000,2,NAN)==66000);
    /* Unlocked bounds 23936 at x2: height 47872, far plane 98736 (R-006 D7). */
    assert(sh_editor_zoom_orbit_height(23936,2)==47872&&sh_editor_zoom_blueprint_far(23936,2,0)==98736);
    /* With the editor's far-plane write redirected the height is free; without
     * it only the blend raises r_zfar and it stops above the 33000 volume. */
    assert(sh_editor_zoom_capped_height(47872,1)==47872);
    assert(sh_editor_zoom_capped_height(47872,0)==SH_ZOOM_BLEND_CEILING);
    assert(sh_editor_zoom_capped_height(32000,0)==32000&&sh_editor_zoom_capped_height(16000,0)==16000);
    assert(SH_ZOOM_BLEND_CEILING<SH_ZOOM_EDITOR_FAR);
    /* The editor's blend: steady views use their own flag; a transition
     * between unlike views interpolates from the previous one. */
    assert(sh_editor_zoom_blend(1,0,0,0.3f)==1&&sh_editor_zoom_blend(0,1,0,0.3f)==0);
    assert(sh_editor_zoom_blend(1,1,1,0.3f)==1);
    assert(fabsf(sh_editor_zoom_blend(1,0,1,0.3f)-0.3f)<1e-6f);
    assert(fabsf(sh_editor_zoom_blend(0,1,1,0.3f)-0.7f)<1e-6f);
    assert(sh_editor_zoom_blend(1,0,1,2.0f)==1&&sh_editor_zoom_blend(1,0,1,-1.0f)==0);
    /* Module picking (R-011): native 50000 at scale 1; at least the native
     * pick:far ratio above it, and long enough to reach the farthest module
     * corner (floors +/-52 x 192 at the ceiling) from the farthest camera
     * whenever the zoom is raised (scale 1 keeps the native 50000). */
    assert(sh_editor_zoom_pick_distance(16000,1,SH_ZOOM_NATIVE_PICK)==SH_ZOOM_NATIVE_PICK);
    assert(sh_editor_zoom_pick_distance(0,NAN,NAN)==SH_ZOOM_NATIVE_PICK);
    {
        float pick=sh_editor_zoom_pick_distance(16000,SH_ZOOM_SCALE_DEFAULT,SH_ZOOM_NATIVE_PICK);
        assert(pick>=137500&&pick<137502);
        pick=sh_editor_zoom_pick_distance(32671,SH_ZOOM_SCALE_DEFAULT,SH_ZOOM_NATIVE_PICK);
        assert(pick>=280766&&pick<280770);
        assert(sh_editor_zoom_pick_distance(32671,SH_ZOOM_SCALE_MAX,0)>=303030);
    }
    {
        static const float bounds[]={16000,23936,32671},scales[]={2,SH_ZOOM_SCALE_DEFAULT,3,SH_ZOOM_SCALE_MAX};
        int i,j;
        for(i=0;i<3;i++)for(j=0;j<4;j++) {
            float b=bounds[i],h=sh_editor_zoom_orbit_height(b,scales[j]),p=sh_editor_zoom_pan_limit(b,scales[j]);
            float reach=sqrtf(2*(p+b)*(p+b)+(h+52*192.0f)*(h+52*192.0f));
            assert(sh_editor_zoom_pick_distance(b,scales[j],SH_ZOOM_NATIVE_PICK)>=reach);
            assert(sh_editor_zoom_pick_distance(b,scales[j],0)>=sh_editor_zoom_blueprint_far(b,scales[j],0));
        }
    }
    /* A hand-raised cvar is kept; a lower or invalid one is not. */
    assert(sh_editor_zoom_pick_distance(16000,1,400000)==400000);
    assert(sh_editor_zoom_pick_distance(16000,2,1)>=100000);
    assert(isfinite(sh_editor_zoom_pick_distance(16000,2,INFINITY))&&sh_editor_zoom_pick_distance(16000,2,INFINITY)>=100000);
    printf("editor_zoom_test: all passed\n");
    return 0;
}
