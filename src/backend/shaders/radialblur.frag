#version 450 core
// radialblur.frag -- Snapmap+'s radial blur in the game's post process (modprog.h), as a Vulkan module.
//
// DOOM 2016's fx has a radial blur item (FX_BLUR_RADIAL) whose centre and radius reach the view and nothing draws.
// Snapmap+ draws it in the game's own post process, before tone mapping: every read of the scene image ($tex0) in
// `postprocess` and `computepostprocess` becomes rb_sample, which blurs the scene along a per-pixel vector toward the
// centres of the radial blurs that are on. The blur's shape is data the effect's author gives (backend/postfx.c
// reads it from the fx item and writes it per frame): the kernel rises from a ring at the peak radius r (screen
// heights) to its peak by `falloff`, over a zone `ring` r wide on each side; its length is `length` screen heights at
// full strength. Per pixel uv (view texture coordinates) and per blur b with centre c, peak radius r, strength s:
//   d = c - uv, d.x *= aspect, L = |d|
//   k_b = saturate(falloff_b (1 - |L - r_b| / (ring_b r_b))) s_b visible_b
//   v = sum(d / L k_b^2) / (1e-6 + sum k_b), quantized as an 8-bit signed unit vector component, times length
// then two passes of seven taps along v (x divided by the aspect): the coarse step v / 7 from uv, the fine step v / 49
// from each coarse tap with that tap's own v:
//   out(uv) = 1/49 sum_j sum_i S(q_j + i v(q_j) / 49),  q_j = uv + j v(uv) / 7,  i, j = 0..6.
// Per blur, from the world centre and the author's parameters: w the view depth (clip w), c the projected centre,
// r = proj_y R / (2 w) for the world radius R, s = strength(t) min(1, r / screenFade) saturate((w - near) / nearFade),
// capped at `cap`; when the fade zone ring r / falloff is wider than `zoneCap`, a falloff below 1 scales s and the
// falloff becomes ring r / zoneCap; a blur whose box c +- (1 + ring) r (x divided by the aspect) is off screen is
// off; visible = the share of eight depth samples in that box where the centre is in front of the scene.
//
// The post process reads the scene through tex2DlodClamped(tex0, texcoord, (resolutionScale.xy,
// renderPositionToViewTexture.zw)) five times (its chromatic aberration); the splice turns each of those calls into
// rb_clamped with the same arguments and P. main() and the bindings below exist only so the module compiles on its
// own: the splice keeps rb_clamped and what it calls, binds rb_depth to the program's own viewdepthmap sampler and
// loads P from the program's uniforms (the product's parameters, then viewprojectionmatrixx..w, projectionmatrixz and
// renderviewsize).
// tools/gen_multilayer.js builds modprog_post.c from this file.

layout(binding = 0) uniform sampler2D rb_scene;   // tex0: the scene's HDR image
layout(binding = 1) uniform sampler2D rb_depth;   // viewdepthmap

layout(location = 0) in vec4 rb_in;
layout(location = 0) out vec4 rb_out;

// P: 0 radialblur0 (x the image y sign: 1 Vulkan, -1 OpenGL; y the blurs on; z the length), then per blur b (0..6)
// two vectors radialblur1 + 2b.. : (centre x, y, z, strength now), (world radius, falloff, ring, cap); 15 radialblur15
// (screenFade, nearFade, zoneCap: the newest blur's, shared by those on, as the length is); 16..19
// viewprojectionmatrixx..w, 20 projectionmatrixz, 21 renderviewsize, info: (resolutionscale.xy, the view texture's
// pixel size). Every render parm takes one of the game's 1024 vector parm slots (idParmState::AllocParmState), which
// the stock decls nearly fill, so the blur spends as few as its terms allow.
#define RB_N 7
#define RB_SHAPE 15
#define RB_VP 16
#define RB_PZ 20
#define RB_SIZE 21
#define RB_NP 22

// The eight depth samples of a blur's box (unit square), a 3 x 3 grid without its centre.
const vec2 RB_TAPS[8] = vec2[8](vec2(0.1667, 0.1667), vec2(0.5, 0.1667), vec2(0.8333, 0.1667), vec2(0.1667, 0.5),
                                vec2(0.8333, 0.5), vec2(0.1667, 0.8333), vec2(0.5, 0.8333), vec2(0.8333, 0.8333));

struct rb_blur { vec2 c; float r; float falloff; float ring; float s; };

// One blur's screen terms; s 0 when it is off.
rb_blur rb_setup(int b, vec4 P[RB_NP], vec4 info, float aspect)
{
    rb_blur o;
    vec4 a = P[1 + 2 * b], q = P[2 + 2 * b], e = vec4(q.w, P[RB_SHAPE].xyz);
    vec4 p = vec4(a.xyz, 1.0);
    float cx = dot(P[RB_VP], p), cy = dot(P[RB_VP + 1], p), w = dot(P[RB_VP + 3], p);
    // the near plane: the view depth where the stored depth is 0 (the scene's depth is P.w / (d + P.z), as the
    // engine's own soft particles read it; the engine's projection gives P.z = -1, P.w = -2 znear)
    float pz = P[RB_PZ].z != 0.0 ? P[RB_PZ].w / P[RB_PZ].z : 0.0;
    o.s = 0.0; o.c = vec2(0.5); o.r = 1.0; o.falloff = q.y; o.ring = q.z;
    if (a.w <= 0.0 || w < pz || q.x <= 0.0) return o;
    o.c = vec2(cx / w * 0.5 + 0.5, P[0].x * -cy / w * 0.5 + 0.5);
    o.r = length(P[RB_VP + 1].xyz) * q.x / w * 0.5;
    vec2 half_ = vec2((1.0 + o.ring) * o.r / aspect, (1.0 + o.ring) * o.r);
    if (o.c.x + half_.x < 0.0 || o.c.x - half_.x > 1.0 || o.c.y + half_.y < 0.0 || o.c.y - half_.y > 1.0) return o;
    float s = a.w * min(1.0, o.r / e.y) * clamp((w - pz) / e.z, 0.0, 1.0);
    s = min(s, e.x);
    float zone = o.ring * o.r;
    if (min(zone / o.falloff, zone) > e.w) {
        if (o.falloff < 1.0) s *= o.falloff;
        o.falloff = zone / e.w;
    }
    // visibility: the centre in front of the scene at the box's eight samples
    vec2 lo = max(vec2(0.0), o.c - half_), hi = min(vec2(1.0), o.c + half_);
    int seen = 0;
    for (int k = 0; k < 8; k++) {
        vec2 uv = lo + RB_TAPS[k] * (hi - lo);
        float zs = P[RB_PZ].w / (textureLod(rb_depth, uv * info.xy, 0.0).x + P[RB_PZ].z);
        if (w < zs || !(zs > 0.0)) seen++;                         // depth 1 (the sky) is infinitely far
    }
    o.s = s * min(1.0, float(seen) * 0.125);
    return o;
}

// The blur vector at uv (aspect-corrected screen heights, before the x correction of the taps).
vec2 rb_vector(vec2 uv, rb_blur B[RB_N], float aspect, float len)
{
    vec2 sum = vec2(0.0);
    float wsum = 1e-6;
    for (int b = 0; b < RB_N; b++) {
        if (B[b].s <= 0.0) continue;
        vec2 d = B[b].c - uv;
        d.x *= aspect;
        float L = max(length(d), 1e-6);
        float k = clamp(B[b].falloff * (1.0 - abs(L - B[b].r) / (B[b].ring * B[b].r)), 0.0, 1.0) * B[b].s;
        sum += d / L * k * k;
        wsum += k;
    }
    vec2 v = sum / wsum;
    // the kernel image's 8-bit signed unit encoding
    v = (floor(clamp(v, -1.0, 1.0) * 127.0 + 128.0 + 0.5) - 128.0) / 127.0;
    return v * len;
}

vec4 rb_tap(sampler2D scene, vec2 uv, vec4 info)
{
    return textureLod(scene, clamp(uv * info.xy, vec2(0.0), info.xy - info.zw * 0.5), 0.0);
}

// The scene read at tc (the program's own texture coordinate, in the scaled image) with the radial blurs applied.
vec4 rb_sample(sampler2D scene, vec2 tc, float lod, vec4 info, vec4 P[RB_NP])
{
    if (P[0].y <= 0.0) return textureLod(scene, tc, lod);
    float aspect = P[RB_SIZE].x / P[RB_SIZE].y;
    rb_blur B[RB_N];
    bool any_ = false;
    for (int b = 0; b < RB_N; b++) {
        B[b] = rb_setup(b, P, info, aspect);
        if (B[b].s > 0.0) any_ = true;
    }
    if (!any_) return textureLod(scene, tc, lod);
    vec2 uv = tc / info.xy;
    vec2 v = rb_vector(uv, B, aspect, P[0].z);
    if (dot(v, v) <= 0.0) return textureLod(scene, tc, lod);
    vec2 coarse = v / 7.0;
    coarse.x /= aspect;
    vec4 acc = vec4(0.0);
    for (int j = 0; j < 7; j++) {
        vec2 q = uv + float(j) * coarse;
        vec2 fine = rb_vector(q, B, aspect, P[0].z) / 49.0;
        fine.x /= aspect;
        for (int i = 0; i < 7; i++) acc += rb_tap(scene, q + float(i) * fine, info);
    }
    return acc / 49.0;
}

// The post process's tex2DlodClamped (global.inc) with the radial blurs applied.
vec4 rb_clamped(sampler2D scene, vec4 texcoord, vec4 info, vec4 P[RB_NP])
{
    vec2 tc = clamp(texcoord.xy, vec2(0.0), info.xy - info.zw * 0.5);
    return rb_sample(scene, tc, texcoord.w, info, P);
}

layout(binding = 30) uniform rb_parms_ubo { vec4 P[RB_NP]; } rb_parms;

void main()
{
    rb_out = rb_clamped(rb_scene, rb_in, vec4(1.0, 1.0, 0.0, 0.0), rb_parms.P);
}
