#version 450 core
// eternalparticles.frag -- DOOM Eternal's particle shading in the product's particle program (modprog.h), as a
// Vulkan module.
//
// DOOM Eternal draws a CPU particle stage with its uberparticles program; the stage's render mode (the material's
// particleuberparms1.x when the stage names none) picks the shading, and per-stage constants (emissiveMultiplier,
// the emissive mask's multiplier and colour, the explosion gradients, the near fade, the soft-particle distance,
// depthOffsetScale) come with the stage. Per particle it reads the same things DOOM 2016's particle vertices carry:
// the flipbook's two frames and their blend, the colour and its alpha, and genericParm[0..3] (2016 encodes each in
// a byte: brightness 1 + 10 gp0, alpha scale 1 + 10 gp1, gp3 the opacity curve). With T the frame-blended texture
// (red and green linearized by the sRGB polynomial), c the particle colour, ca its alpha times the near fade, s the
// soft-particle term and E, Em, Mc, G2, G3 the stage constants:
//   UNLIT       rgb = T.r c (1 + 200 gp0) E ca^2        a = mix(sqrt T.g, T.g, gp3) ca (1 + 10 gp1) s
//   LIT         rgb = T.r c (L + 200 gp0) w + Mc Em m^2   a as UNLIT          (L the particle light atlas,
//                                                                            m the emissive map's red)
//   EXPLOSION   rgb = mix(L T.r c, mix(G2, G3, m) E, m)  o = mix(sqrt T.g, T.g, gp3) ca; a = mix(sqrt o, o, gp3) ca s
// near fade = saturate((d - end) / (begin - end)) on the distance d from the view (when begin > end); soft =
// saturate((zScene - zFrag - saturate(1 - T.g) depthOffsetScale) / distance). Then 2016's own fog, and the colour
// premultiplied by its alpha, as both games blend (ONE, ONE_MINUS_SRC_ALPHA).
// c and its alpha are the numbers the CPU wrote into the particle vertex, linear, as DOOM Eternal's vertex program
// passes them on: DOOM 2016's uberParticles vertex stage hands the fragment DeGamma(vertex.color) (its exact sRGB
// decode, on rgb and on alpha) and its CPU stores the alpha remapped past r_particlesMinAlpha t (a' = (a - t) /
// (1 - t), t = 16 / 255 by default); both are undone first (ep_srgb, EP_MIN_ALPHA).
//
// ep_shade is spliced into the game's uberparticles fragment stage at the end of main and replaces the colour it
// wrote (the stock shading runs as its unlit mode and is overwritten). main() and the bindings below exist only so
// the module compiles on its own: the splice keeps ep_shade and what it calls, binds ep_trans, ep_depth and
// ep_atlas to the program's own transmap, viewdepthmap and particleslightatlasmap samplers, binds ep_emissive after
// the program's textures, loads V from the stage inputs, P from the material uniforms (the product's parameters,
// then projectionmatrixz and globalvieworigin) and U from the program's own uniforms (tools/gen_multilayer.js builds
// modprog_particles.c from this file).

layout(binding = 0) uniform sampler2D ep_trans;     // transmap: the particle image (R luminance, G opacity)
layout(binding = 1) uniform sampler2D ep_depth;     // viewdepthmap
layout(binding = 2) uniform sampler2D ep_atlas;     // particleslightatlasmap
layout(binding = 3) uniform sampler2D ep_emissive;  // eternalparticleemissivemap: the stage's emissive mask sheet

layout(location = 0) in vec4 ep_in;
layout(location = 0) out vec4 ep_out;

// The sRGB curve DOOM Eternal's uberparticles applies to both texture channels (2016's SRGBlinearApprox).
float ep_lin(float x) { return x * (x * (x * 0.305306011 + 0.682171111) + 0.012522878); }

// The inverse of DOOM 2016's SRGBlinear (global.inc: x / 12.92 up to 0.04045, else pow(x / 1.055 + 0.0521327, 2.4)),
// which its particle vertex stage applies to the vertex colour; and the alpha floor its CPU remaps the alpha past
// (r_particlesMinAlpha, default 16 of 255).
float ep_srgb(float x) { return x <= 0.0031308 ? x * 12.92 : 1.055 * (pow(x, 1.0 / 2.4) - 0.0521327); }
const float EP_MIN_ALPHA = 16.0 / 255.0;

// A bicubic B-spline sample of the light atlas (four bilinear taps), as both games read it.
float ep_w0(float a) { return (1.0 / 6.0) * (a * (a * (-a + 3.0) - 3.0) + 1.0); }
float ep_w1(float a) { return (1.0 / 6.0) * (a * a * (3.0 * a - 6.0) + 4.0); }
float ep_w2(float a) { return (1.0 / 6.0) * (a * (a * (-3.0 * a + 3.0) + 3.0) + 1.0); }
float ep_w3(float a) { return (1.0 / 6.0) * (a * a * a); }
vec3 ep_bicubic(sampler2D image, vec2 uv, vec2 res)
{
    uv = uv * res + 0.5;
    vec2 iuv = floor(uv), fuv = fract(uv);
    float g0x = ep_w0(fuv.x) + ep_w1(fuv.x), g1x = ep_w2(fuv.x) + ep_w3(fuv.x);
    float g0y = ep_w0(fuv.y) + ep_w1(fuv.y), g1y = ep_w2(fuv.y) + ep_w3(fuv.y);
    float h0x = -1.0 + ep_w1(fuv.x) / g0x, h1x = 1.0 + ep_w3(fuv.x) / g1x;
    float h0y = -1.0 + ep_w1(fuv.y) / g0y, h1y = 1.0 + ep_w3(fuv.y) / g1y;
    vec3 t0 = textureLod(image, (vec2(iuv.x + h0x, iuv.y + h0y) - 0.5) / res, 0.0).xyz;
    vec3 t1 = textureLod(image, (vec2(iuv.x + h1x, iuv.y + h0y) - 0.5) / res, 0.0).xyz;
    vec3 t2 = textureLod(image, (vec2(iuv.x + h0x, iuv.y + h1y) - 0.5) / res, 0.0).xyz;
    vec3 t3 = textureLod(image, (vec2(iuv.x + h1x, iuv.y + h1y) - 0.5) / res, 0.0).xyz;
    return g0y * (g0x * t0 + g1x * t1) + g1y * (g0x * t2 + g1x * t3);
}

// V: 0 colour, 1 texcoord0 (current and next frame uv), 2 texcoord1 (x 1 + 10 gp0, z frame blend, w gp3),
//    3 texcoord2 (xyz world position), 4 texcoord4 (xy light atlas uv, z 1 + 10 gp1), 5 texcoord6 (fog), 6 the
//    fragment position
// P: 0 eternalparticle (x mode: 0 unlit, 1 lit, 4 explosion (a liquid stage
//    keeps the game's own blood shading: its material names liquidhighq, not this program); y emissiveMultiplier; z the emissive mask's
//    multiplier; w depthOffsetScale, units), 1 eternalparticlefade (x near fade begin, y near fade end, z the soft
//    distance, units; w 1 when the stage has an emissive mask sheet), 2 eternalparticlecolor (the emissive mask
//    colour, or the explosion's gradient2), 3 eternalparticlecolor2 (rgb the explosion's gradient3; w a lit stage's scale, 1 for a CPU stage:
//    a NEW-system stage Eternal lights per vertex draws (L + k)(1 + k)), 4 projectionmatrixz,
//    5 globalvieworigin
// U: 0 renderpositiontoviewtexture, 1 particleatlasparms, 2 foglightinscattercolor, 3 foglightinscatterintensity,
//    4 foglightoppositecolor, 5 fogcolor, 6 transparencylightmult
void ep_shade(inout vec4 frag, vec4 V[7], vec4 P[6], vec4 U[7])
{
    vec4 cur = texture(ep_trans, V[1].xy);
    vec4 nxt = texture(ep_trans, V[1].zw);
    float fb = V[2].z;
    float R = mix(ep_lin(cur.r), ep_lin(nxt.r), fb);
    float G = mix(ep_lin(cur.g), ep_lin(nxt.g), fb);
    if (G < 1e-6) discard;
    float mode = P[0].x;
    float gp0 = (V[2].x - 1.0) / 10.0;
    float gp1 = (V[4].z - 1.0) / 10.0;
    float gp3 = V[2].w;
    vec3 c = vec3(ep_srgb(V[0].r), ep_srgb(V[0].g), ep_srgb(V[0].b));

    // near fade, on the distance from the view
    float ca = ep_srgb(V[0].a) * (1.0 - EP_MIN_ALPHA) + EP_MIN_ALPHA;
    if (P[1].x > P[1].y) {
        float d = length(V[3].xyz - P[5].xyz);
        ca *= clamp((d - P[1].y) / (P[1].x - P[1].y), 0.0, 1.0);
    }
    // soft particle, on linear view depth
    vec2 tc = V[6].xy * U[0].zw + U[0].xy;
    float zs = P[4].w / (textureLod(ep_depth, tc, 0.0).x + P[4].z);
    float zf = P[4].w / (V[6].z + P[4].z);
    float soft = clamp((zs - zf - clamp(1.0 - G, 0.0, 1.0) * P[0].w) / max(P[1].z, 1e-3), 0.0, 1.0);

    float m = 1.0;
    if (P[1].w != 0.0) m = mix(texture(ep_emissive, V[1].xy).r, texture(ep_emissive, V[1].zw).r, fb);

    vec3 rgb;
    float a;
    float curve = mix(sqrt(G), G, gp3);
    if (mode == 1.0) {
        vec3 L = ep_bicubic(ep_atlas, V[4].xy, U[1].xy);
        rgb = R * c * (L + 200.0 * gp0) * P[3].w;
        if (P[0].z != 0.0) rgb += P[2].rgb * P[0].z * m * m;
        a = curve * ca * (1.0 + 10.0 * gp1);
    } else if (mode == 4.0) {
        vec3 L = ep_bicubic(ep_atlas, V[4].xy, U[1].xy);
        rgb = mix(L * R * c, mix(P[2].rgb, P[3].rgb, m) * P[0].y, m);
        float o = curve * ca;
        a = mix(sqrt(o), o, gp3) * ca;
    } else {
        rgb = R * c * (1.0 + 200.0 * gp0) * P[0].y * ca * ca;
        a = curve * ca * (1.0 + 10.0 * gp1);
    }
    a = clamp(a * soft, 0.0, 1.0);

    // 2016's fog (the stock program's, from its fog interpolator)
    vec3 heightFog = mix(U[2].xyz * U[3].x, U[4].xyz, V[5].z);
    float fogAmount = clamp(V[5].y + V[5].x, 0.0, 1.0);
    float fogSafe = max(fogAmount, 0.0001);
    vec3 fogColor = mix(U[5].xyz, heightFog, max(0.0, (fogSafe - V[5].x) / fogSafe));
    rgb = mix(fogColor * U[6].x, rgb, 1.0 - fogAmount);

    frag = vec4(rgb * a, a);
}

layout(binding = 30) uniform ep_parms_ubo { vec4 V[7]; vec4 P[6]; vec4 U[7]; } ep_parms;

void main()
{
    vec4 frag = ep_in;
    ep_shade(frag, ep_parms.V, ep_parms.P, ep_parms.U);
    ep_out = frag;
}
