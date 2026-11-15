#version 450 core
// multilayer.frag -- the multilayer blend of the product's render programs (modprog.h), as a Vulkan module.
//
// DOOM Eternal draws a MATERIAL_IS_MULTILAYER character material (nearly every demon's wounds, gore and head) as
// its own layer blended with up to four tiled layers, each selected by one channel of a mask:
//   M = min(mask(uv) * multiblendlayerscale, 1); hb = (H0 - 127/255) * blendheightmapscale (layer 0's height);
//   for layer i (mask channel i) where m = M[i] != 0: hi = (Hi(uv * tiling[i]) - 127/255) * heightmapscale[i],
//   t0 = clamp(((hi + m) - (hb + 1 - m)) / softness[i], -1, 1) / 2 + 0.5, and within 0.1 of either end of the mask
//   t moves toward m by saturate((|0.5 - m| - 0.4) / 0.1); then albedo, specular, smoothness, normal and emissive
//   each move toward the layer by t times their own scale (a negative scale extrapolates);
//   the blended normal d is laid onto layer 0's normal n0 by reoriented normal mapping.
// Layer i is read at uv * tiling[i] * its surface tiling + time * its scroll (its height at uv * tiling[i]).
//
// ml_blend is spliced into a COMPUTE_SHADING program where CUSTOM_TEXTURE_INPUTS begins: after the material's own
// layer has been read from its virtual-texture pages, before the wound block and lighting. It reads layer 0 back
// from the inputs and writes them as the pages would have held the blend:
//   albedo      linear rgb (the pages are sRGB-decoded by the sampler)
//   specular    the luma of the sRGB of the blended rgb, sRGB-decoded (the grey every virtual-texture material of
//               a port draws); layer 0's rgb comes unpacked from the pages (the material sets packedspec 0)
//   thickness   the subsurface mask, what the packed pages would have given
//   smoothness  SRGBlinear of the blended smoothness (the pages hold it gamma-encoded in alpha)
//   normal.xy   the page encoding of the reoriented normal, (n * 128 + 128) / 255
//   lightmap.x  the emissive mask: the square root of the brightest blended emissive channel divided by 1 +
//               colorscale x color x albedo in the bloom colour's brightest channel (below); the base program
//               lights bloommaskscale x mask^2, and reads the pages' mask already decoded from sRGB
//   emissive    the program's own emissive term recomputed from that mask
// The base program adds the emissive twice: once into the ambient light the albedo multiplies (emissive x colorscale
// x color) and once in its composite (bloommaskscale x mask^2 x bloomcolor), so a mask m leaves the surface as
// bloommaskscale m^2 bloomcolor (1 + colorscale color albedo). DOOM Eternal writes its emissive once; the mask is
// divided so that the bloom colour's brightest channel leaves exactly as Eternal's (the others within the ratio of
// their 1 + albedo terms).
// Each layer image is sampled only where the mask selects its layer, with the screen-space footprint of its tiled
// uv (the derivatives are taken once, in uniform control flow).
//
// main() and the bindings below exist only so the module compiles on its own: the splice keeps ml_blend and what
// it calls, binds the samplers after the program's own textures, and loads the parameters from the program's
// material uniforms (tools/gen_multilayer.js builds modprog_multilayer.c from this file).

layout(binding = 0) uniform sampler2D ml_mask;          // layerblendmask
layout(binding = 1) uniform sampler2D ml_height0;       // blendlayer0heightmap: layer 0's heightmap
layout(binding = 2) uniform sampler2D ml_albedo1;       // blendlayer1albedomap
layout(binding = 3) uniform sampler2D ml_normal1;       // blendlayer1normalmap
layout(binding = 4) uniform sampler2D ml_smoothness1;   // blendlayer1smoothnessmap
layout(binding = 5) uniform sampler2D ml_height1;       // blendlayer1heightmap
layout(binding = 6) uniform sampler2D ml_albedo2;       // blendlayer2albedomap
layout(binding = 7) uniform sampler2D ml_normal2;       // blendlayer2normalmap
layout(binding = 8) uniform sampler2D ml_smoothness2;   // blendlayer2smoothnessmap
layout(binding = 9) uniform sampler2D ml_height2;       // blendlayer2heightmap
layout(binding = 10) uniform sampler2D ml_albedo3;      // blendlayer3albedomap
layout(binding = 11) uniform sampler2D ml_normal3;      // blendlayer3normalmap
layout(binding = 12) uniform sampler2D ml_smoothness3;  // blendlayer3smoothnessmap
layout(binding = 13) uniform sampler2D ml_height3;      // blendlayer3heightmap
layout(binding = 14) uniform sampler2D ml_albedo4;      // blendlayer4albedomap
layout(binding = 15) uniform sampler2D ml_normal4;      // blendlayer4normalmap
layout(binding = 16) uniform sampler2D ml_smoothness4;  // blendlayer4smoothnessmap
layout(binding = 17) uniform sampler2D ml_height4;      // blendlayer4heightmap
layout(binding = 18) uniform sampler2D ml_extra0;       // multilayerextramap0: a layer's specular or emissive image
layout(binding = 19) uniform sampler2D ml_extra1;       // multilayerextramap1
layout(binding = 20) uniform sampler2D ml_noise;        // the program's own magnificationclouds (the splice binds
                                                        // the base program's sampler, adding no texture)

layout(location = 0) in vec4 ml_in_uv;
layout(location = 0) out vec4 ml_out;

const float ML_HEIGHT_MID = 0.49803921580314636;   // 127 / 255
const float ML_EDGE = 0.1;

float ml_srgb_dec(float v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
float ml_srgb_enc(float v) { v = clamp(v, 0.0, 1.0); return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
vec3 ml_srgb_enc3(vec3 v) { return vec3(ml_srgb_enc(v.x), ml_srgb_enc(v.y), ml_srgb_enc(v.z)); }

vec3 ml_decode_normal(vec2 v)
{
    vec2 n = v * (255.0 / 128.0) - 1.0;
    return vec3(n, sqrt(clamp(1.0 - n.x * n.x - n.y * n.y, 0.0, 1.0)));
}

// One blend layer i (0..3: mask channel R, G, B, A selects layer i + 1), moving the running layer toward it.
void ml_layer(int i, float m, float hb, vec2 uv, vec2 dx, vec2 dy, float time, vec4 P[28],
              sampler2D s_albedo, sampler2D s_normal, sampler2D s_smoothness, sampler2D s_height,
              inout vec4 f_alb, inout vec4 f_spec, inout float f_smooth, inout vec4 f_n, inout vec4 f_em)
{
    if (m != 0.0) {
        float tl = P[0][i];
        vec4 lt = P[12 + i];
        float hi = (textureGrad(s_height, uv * tl, dx * tl, dy * tl).x - ML_HEIGHT_MID) * P[2][i];
        float a = (hb + 1.0) - m;
        float b = (hi + 0.0) + m;
        float t0 = clamp((b - a) / P[3][i], -1.0, 1.0) / 2.0 + 0.5;
        float f = clamp((abs(0.5 - m) - (0.5 - ML_EDGE)) / ML_EDGE, 0.0, 1.0);
        float t;
        if (m < ML_EDGE && t0 > m) {
            t = mix(t0, m, f);
        } else if (m > 1.0 - ML_EDGE && t0 < m) {
            t = mix(t0, m, f);
        } else {
            t = t0;
        }
        vec2 lsc = lt.xy;
        vec2 lu = (uv * tl) * lsc + time * lt.zw;
        vec2 ldx = dx * tl * lsc;
        vec2 ldy = dy * tl * lsc;
        if (P[4][i] != 0.0) {
            f_alb = mix(f_alb, vec4(textureGrad(s_albedo, lu, ldx, ldy).xyz, 1.0), vec4(t * P[4][i]));
        }
        if (P[6][i] != 0.0) {
            vec4 ls = P[16 + i];
            vec3 l_spec = ls.w == 1.0 ? textureGrad(ml_extra0, lu, ldx, ldy).xyz :
                          ls.w == 2.0 ? textureGrad(ml_extra1, lu, ldx, ldy).xyz : ls.xyz;
            f_spec = mix(f_spec, vec4(l_spec, 1.0), vec4(t * P[6][i]));
        }
        if (P[7][i] != 0.0) {
            f_smooth = mix(f_smooth, textureGrad(s_smoothness, lu, ldx, ldy).x, t * P[7][i]);
        }
        if (P[5][i] != 0.0) {
            f_n = mix(f_n, vec4(textureGrad(s_normal, lu, ldx, ldy).xy, 0.0, 0.0), vec4(t * P[5][i]));
        }
        if (P[8][i] != 0.0) {
            vec4 le = P[20 + i];
            vec3 l_em = le.w == 1.0 ? textureGrad(ml_extra0, lu, ldx, ldy).xyz * le.xyz :
                        le.w == 2.0 ? textureGrad(ml_extra1, lu, ldx, ldy).xyz * le.xyz : le.xyz;
            f_em = mix(f_em, vec4(l_em, 0.0), vec4(t * P[8][i]));
        }
    }
}

// DOOM Eternal's ember: characteropaque's wound block for an isember material, after the layers (R-G9's reading,
// tools/vt/multilayer.py Blend._ember): g is the wound cap's blood level (its image's green; 1 with no cap image),
// the blood takes the specular toward 0.02 and the smoothness toward 0.75 by its coverage squared, 1 - g burns
// the surface (normal toward the burnt tile's, charcoal albedo, smoothness and specular), and the emissive gains the
// embers (1, 0.07, 0) x 25 N.x N.y, flickered by the view's scrolled noise (magnificationclouds), animated as Eternal
// draws it. Emissive values are relative to the board's emissive scale; Pe[3].xyz is the ember colour on that scale.
void ml_ember(vec2 uv, vec2 dx, vec2 dy, float time, vec4 Pe[4], inout vec4 f_alb, inout vec4 f_spec,
              inout float f_smooth, inout vec3 r, inout vec4 f_em)
{
    vec2 bt = Pe[1].xy;
    vec4 B = Pe[0].z == 1.0 ? textureGrad(ml_extra0, uv * bt, dx * bt, dy * bt) :
             Pe[0].z == 2.0 ? textureGrad(ml_extra1, uv * bt, dx * bt, dy * bt) : vec4(0.5, 0.5, 0.0, 0.0);
    float g = Pe[0].w == 1.0 ? textureGrad(ml_extra0, uv, dx, dy).y :
              Pe[0].w == 2.0 ? textureGrad(ml_extra1, uv, dx, dy).y : 1.0;
    float ig = 1.0 - g;
    float bb = B.z;
    float w = clamp(1.0 - g * g, 0.0, 1.0);
    float w2 = w * w;
    float w3 = w2 * w;
    float m = clamp(w3 * bb * (2.0 - w) - (1.0 - w3) + clamp(8.0 * w2 - 7.0, 0.0, 1.0), 0.0, 1.0);
    m = clamp(16.0 * m, 0.0, 1.0);
    float m2 = m * m;
    f_spec.xyz = f_spec.xyz + (vec3(0.02) - f_spec.xyz) * m2;
    f_smooth = f_smooth + (0.75 - f_smooth) * m2;
    vec2 N = (r.xy + 1.0) * 0.5;
    vec2 nxy = N + (B.xy - N) * ig;
    vec3 charcoal = vec3(0.01) + (vec3(0.12, 0.11, 0.10) - vec3(0.01)) * bb;
    f_alb.xyz = f_alb.xyz + (charcoal - f_alb.xyz) * ig;
    f_smooth = f_smooth + ((0.1 + (0.7 - 0.1) * bb) - f_smooth) * ig;
    f_spec.xyz = f_spec.xyz + (vec3(0.048) - f_spec.xyz) * ig;
    vec3 hot = (bb * bb) * Pe[3].xyz;
    vec3 e1 = hot + (f_em.xyz - hot) * g;
    float s = (B.w + (bb - B.w) * ig) * ig;
    vec3 k = (vec3(s) + (e1 - vec3(s)) * g) * vec3(1.0, 0.07, 0.0) * 25.0 * (nxy.x * nxy.y);
    vec4 nt = Pe[2];
    vec2 u1 = vec2(uv.x * nt.x, uv.y * nt.x + time * nt.z);
    vec2 u2 = vec2(uv.x * nt.y, uv.y * nt.y - time * nt.w);
    float n1 = textureGrad(ml_noise, u1, dx * nt.x, dy * nt.x).x;
    float n2 = textureGrad(ml_noise, u2, dx * nt.y, dy * nt.y).x;
    float fl = clamp(n1 * n1 * n2 * n2 + 0.12, 0.0, 1.0);
    f_em.xyz = e1 + k * fl;
    vec2 x = nxy * 2.0 - 1.0;
    r = vec3(x, sqrt(clamp(1.0 - x.x * x.x - x.y * x.y, 0.0, 1.0)));
}

// P: the material's multilayer parameters, in the order the program declares them (renderparm names):
//   0 layerstilingfactor  1 multiblendlayerscale  2 multiblendheightmapscale  3 multiblendsoftness
//   4 multiblendalbedoscale  5 multiblendnormalscale  6 multiblendspecularscale  7 multiblendsmoothnessscale
//   8 multiblendemissivescale  9 blendheightmapscale (x)  10 blendlayer0tiling  11 blendlayer0emissive (rgb: layer
//   0's emissive colour, brightest channel 1)  12..15 blendlayer1..4tiling (surface tiling xy, scroll xy)
//   16..19 blendlayer1..4specular (rgb: the layer's specular when it is one colour; w: 0 that colour, 1 or 2 the
//   image in extra map 0 or 1)  20..23 blendlayer1..4emissive (rgb: the layer's emissive, relative to the board's
//   emissive scale, or the factor of its emissive image; w: 0 that colour, 1 or 2 the image in extra map 0 or 1)
//   24 multilayerember (x: 1 for an isember material; y: layer 0's emissive image in extra map 0 or 1, else the
//   pages' mask (11's w then scales 11's rgb to the image's emissive on the board's scale); z: the burnt tile in extra map 0 or 1, else none; w: the
//   wound cap's blood image in extra map 0 or 1, else none)  25 multilayerembertiling (xy: the burnt tile's tiling)
//   26 multilayerembernoise (Eternal's emissivenoisetiling)  27 multilayeremberhot (rgb: the ember colour on the
//   board's emissive scale)
// E: bloommaskscale, colorscale, color, bloomcolor (the program's emissive factors)
void ml_blend(inout vec3 albedo, inout vec3 specular, inout float smoothness, inout vec3 normal,
              inout vec3 lightmap, inout vec3 emissive, inout float thickness, vec4 texcoords, float time, vec4 P[28],
              vec4 E[4])
{
    vec2 uv = texcoords.xy;
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);

    // layer 0, read back from the inputs the pages gave
    vec4 f_alb = vec4(albedo, 1.0);
    vec4 f_spec = vec4(specular, 1.0);
    float f_smooth = ml_srgb_enc(smoothness);
    vec2 base_n = normal.xy;
    vec4 f_n = vec4(0.5, 0.5, 0.0, 0.0);
    // the pages' mask is read from the sRGB plane already decoded; its square is layer 0's emissive intensity (the
    // converter writes the sRGB encoding of its square root)
    float raw0 = lightmap.x;
    vec4 f_em = vec4(P[11].xyz * (raw0 * raw0), 0.0);

    vec2 uv0 = uv * P[10].xy + time * P[10].zw;
    if (P[24].y != 0.0) {
        // an ember material reads layer 0's emissive image itself (its colour, not only the pages' mask)
        vec2 d0x = dx * P[10].xy, d0y = dy * P[10].xy;
        vec3 e0 = P[24].y == 1.0 ? textureGrad(ml_extra0, uv0, d0x, d0y).xyz : textureGrad(ml_extra1, uv0, d0x, d0y).xyz;
        f_em = vec4(e0 * P[11].xyz * P[11].w, 0.0);
    }
    float h0 = textureGrad(ml_height0, uv0, dx * P[10].xy, dy * P[10].xy).x;
    vec4 M = min(textureGrad(ml_mask, uv, dx, dy) * P[1], vec4(1.0));
    float hb = (h0 - ML_HEIGHT_MID) * P[9].x;

    ml_layer(0, M[0], hb, uv, dx, dy, time, P, ml_albedo1, ml_normal1, ml_smoothness1, ml_height1,
             f_alb, f_spec, f_smooth, f_n, f_em);
    ml_layer(1, M[1], hb, uv, dx, dy, time, P, ml_albedo2, ml_normal2, ml_smoothness2, ml_height2,
             f_alb, f_spec, f_smooth, f_n, f_em);
    ml_layer(2, M[2], hb, uv, dx, dy, time, P, ml_albedo3, ml_normal3, ml_smoothness3, ml_height3,
             f_alb, f_spec, f_smooth, f_n, f_em);
    ml_layer(3, M[3], hb, uv, dx, dy, time, P, ml_albedo4, ml_normal4, ml_smoothness4, ml_height4,
             f_alb, f_spec, f_smooth, f_n, f_em);

    // reoriented normal mapping onto layer 0's normal
    vec3 n0 = ml_decode_normal(base_n);
    vec3 d = ml_decode_normal(f_n.xy);
    vec3 tt = n0 + vec3(0.0, 0.0, 1.0);
    vec3 uu = vec3(-d.x, -d.y, d.z);
    vec3 r = normalize(tt * dot(tt, uu) - uu * tt.z);
    if (P[24].x != 0.0) {
        vec4 Pe[4] = vec4[4](P[24], P[25], P[26], P[27]);
        ml_ember(uv, dx, dy, time, Pe, f_alb, f_spec, f_smooth, r, f_em);
    }

    albedo = f_alb.xyz;
    vec3 se = ml_srgb_enc3(f_spec.xyz);
    float grey = dot(se, vec3(0.2126, 0.7152, 0.0722));
    specular = vec3(ml_srgb_dec(grey));
    smoothness = ml_srgb_dec(clamp(f_smooth, 0.0, 1.0));
    normal.xy = clamp((r.xy * 128.0 + 128.0) / 255.0, 0.0, 1.0);
    float em = max(f_em.x, max(f_em.y, f_em.z));
    vec3 ka = E[1].x * E[2].xyz * f_alb.xyz;
    float kj = E[3].x >= max(E[3].y, E[3].z) ? ka.x : (E[3].y >= E[3].z ? ka.y : ka.z);
    em = em / (1.0 + max(kj, 0.0));
    float lmx = sqrt(clamp(em, 0.0, 1.0));
    lightmap.x = lmx;
    emissive = vec3(E[0].x * lmx * lmx * E[1].x) * E[2].xyz * E[3].xyz;
    thickness = lightmap.y;
}

layout(binding = 30) uniform ml_parms_ubo { vec4 P[28]; vec4 E[4]; vec4 time; } ml_parms;

void main()
{
    vec3 albedo = vec3(0.5), specular = vec3(0.04), normal = vec3(0.5, 0.5, 0.0), lightmap = vec3(0.0);
    vec3 emissive = vec3(0.0);
    float smoothness = 0.5, thickness = 0.0;
    ml_blend(albedo, specular, smoothness, normal, lightmap, emissive, thickness, ml_in_uv, ml_parms.time.x, ml_parms.P,
             ml_parms.E);
    ml_out = vec4(albedo + specular + emissive + lightmap + normal, smoothness + thickness);
}
