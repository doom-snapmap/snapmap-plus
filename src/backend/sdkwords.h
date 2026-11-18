/* sdkwords.h -- every word a mod author writes or reads in the Graphics SDK's data and calls, in one place.
 *
 * The effect, particle, environment and value compilers (modeffect.c, modenv.c, sdkvalues.c), the script calls that
 * play them and the sentences they print name a word only through this table (SDK_W(...)), so renaming one here
 * renames it everywhere: in what the compiler accepts, in its mistakes and nearest-word suggestions, and in the
 * guide's examples kept by the tests. docs/graphics-sdk.md lists the same words.
 *
 * A row is X(ID, "word"). IDs are the code's names and never reach an author. */
#ifndef SNAPMAP_PLUS_SDKWORDS_H
#define SNAPMAP_PLUS_SDKWORDS_H

#include <math.h>

#define SDK_WORDS(X)                                                                                               \
    /* the first key of every SDK file, and its kinds */                                                           \
    X(TYPE, "type") X(KIND_EFFECT, "effect") X(KIND_ENVIRONMENT, "environment") X(KIND_WOUND, "wound")            \
    /* a wound */                                                                                                  \
    X(W_KIND, "kind") X(W_PARTS, "parts") X(W_OPENS, "opens") X(W_INONEHIT, "inonehit") X(W_SHOWS, "shows")         \
    X(W_CUTS, "cuts") X(W_BLOOD, "blood") X(W_THROWS, "throws") X(W_WHILEALIVE, "whilealive") X(W_RAGDOLL, "ragdoll") \
    X(V_SMALL, "small") X(V_BIG, "big") X(V_HEADSHOT, "headshot") X(V_LIMB, "limb") X(V_SPRAY, "spray")             \
    X(V_POOL, "pool") X(V_BOTH, "both")                                                                            \
    /* an effect's items */                                                                                        \
    X(IT_LIGHT, "light") X(IT_DECAL, "decal") X(IT_SHAKE, "shake") X(IT_FLASH, "flash") X(IT_SCREEN, "screen")     \
    X(IT_BLUR, "blur") X(IT_FOCUS, "focus") X(IT_FLARE, "flare") X(IT_TRAIL, "trail") X(IT_MODEL, "model")         \
    X(IT_SOUND, "sound") X(IT_GLOW, "glow") X(IT_ENVIRONMENT, "environment") X(IT_WIND, "wind")                    \
    X(IT_PARTICLES, "particles")                                                                                   \
    /* timing and placement, any item (and `lasts` on the effect) */                                               \
    X(F_STARTS, "starts") X(F_LASTS, "lasts") X(F_FADEIN, "fadein") X(F_FADEOUT, "fadeout") X(F_REPEATS, "repeats") \
    X(F_OFFSET, "offset") X(F_AT, "at") X(F_WHEN, "when") X(F_REACH, "reach") X(F_FADESFROM, "fadesfrom")          \
    X(F_TURN, "turn") X(F_FOLLOWS, "follows") X(F_FACING, "facing") X(F_RESTARTS, "restarts") X(F_UNTIL, "until") \
    X(F_EVERYTAG, "everytag")                                                                                      \
    X(V_FIXED, "fixed") X(V_PARENT, "parent")                                                                      \
    /* item fields */                                                                                              \
    X(F_COLOUR, "colour") X(F_BRIGHTNESS, "brightness") X(F_FLICKER, "flicker") X(F_SHAPE, "shape")                \
    X(F_SHINE, "shine") X(F_SHADOWS, "shadows") X(F_IMAGE, "image") X(F_MATERIAL, "material") X(F_SIZE, "size")    \
    X(F_DEPTH, "depth") X(F_ANGLE, "angle") X(F_GLOW, "glow") X(F_GLOWLASTS, "glowlasts") X(F_STAYS, "stays")      \
    X(F_STRENGTH, "strength") X(F_STYLE, "style") X(F_TILES, "tiles") X(F_MIRROR, "mirror") X(F_BOTTOM, "bottom")  \
    X(F_NEAR, "near") X(F_FOCUS, "focus") X(F_FAR, "far") X(F_USE, "use") X(F_MODEL, "model") X(F_SOUND, "sound")  \
    X(F_STOPS, "stops") X(F_VALUE, "value") X(F_SEENFROM, "seenfrom") X(F_ONSCREEN, "onscreen") X(F_ANGLES, "angles")   \
    X(F_MOVE, "move")                                                                        \
    /* light flicker presets, shake and blur styles */                                                             \
    X(V_NONE, "none") X(V_FIRE, "fire") X(V_RADIAL, "radial") X(V_SOFT, "soft") X(V_YES, "yes") X(V_NO, "no")      \
    /* particles */                                                                                                \
    X(F_COUNT, "count") X(F_BURST, "burst") X(F_LIFE, "life") X(F_FROM, "from") X(F_DIRECTION, "direction")        \
    X(F_SPREAD, "spread") X(F_SPEED, "speed") X(F_FALL, "fall") X(F_DRAG, "drag")                \
    X(F_FACES, "faces") X(F_LIGHTING, "lighting") X(F_BOUNCE, "bounce") X(F_FRAMES, "frames")  \
    X(F_FRAMERATE, "framerate") X(F_OPACITY, "opacity")                                                            \
    X(V_POINT, "point") X(V_SPHERE, "sphere") X(V_RING, "ring") X(V_BOX, "box") X(V_DISC, "disc")                  \
    X(V_SURFACE, "surface") X(V_UP, "up") X(V_OUTWARD, "outward") X(V_CAMERA, "camera") X(V_MOTION, "motion")      \
    X(V_GROUND, "ground") X(V_FREE, "free") X(V_UNLIT, "unlit")              \
    X(V_LIT, "lit") X(V_STICK, "stick")                                                                            \
    /* values that change over time */                                                                             \
    X(V_PULSE, "pulse") X(V_FLICKER, "flicker") X(V_FADE, "fade") X(V_CURVE, "curve") X(F_RATE, "rate")            \
    X(F_LOW, "low") X(F_HIGH, "high") X(F_FADEFROM, "from") X(F_FADETO, "to") X(F_OVER, "over") X(F_LOOP, "loop")  \
    /* an environment */                                                                                           \
    X(E_FOG, "fog") X(E_FOGDENSITY, "fogdensity") X(E_FOGSTART, "fogstart") X(E_FOGEND, "fogend")                  \
    X(E_BLOOM, "bloom") X(E_EXPOSURE, "exposure") X(E_SATURATION, "saturation") X(E_TINT, "tint")                  \
    X(E_VIGNETTE, "vignette") X(E_VIGNETTECOLOUR, "vignettecolour") X(E_GRAIN, "grain")  \
    X(E_FRINGE, "fringe") X(E_BLEND, "blend")                                                                      \
    /* recolouring */                                                                                              \
    X(R_PRIMARY, "primary") X(R_SECONDARY, "secondary") X(R_PATTERN, "pattern") X(R_SCRATCHES, "scratches")        \
    X(R_DIRT, "dirt") X(R_GLOW, "glow") X(R_SECONDS, "seconds")                                                    \
    /* script calls (R-NM1's names) */                       \
    X(C_PLAY_EFFECT, "play_effect") X(C_SET_ENVIRONMENT, "set_environment") X(C_CLEAR_ENVIRONMENT, "clear_environment")     \
    X(C_ENVIRONMENT_AREA, "environment_area") X(C_RECOLOUR, "recolour") X(C_GAME, "game")

typedef enum sdk_word_id {
#define SDK_WORD_ENUM(id, w) SDK_##id,
    SDK_WORDS(SDK_WORD_ENUM)
#undef SDK_WORD_ENUM
    SDK_WORD_COUNT
} sdk_word_id;

/* The word an author writes for `id`. Header only, so every caller (the compilers, the script calls) reads the one
 * table without linking anything. */
static __inline const char *sdk_word(sdk_word_id id)
{
    static const char *const words[SDK_WORD_COUNT] = {
#define SDK_WORD_TEXT(id_, w_) w_,
        SDK_WORDS(SDK_WORD_TEXT)
#undef SDK_WORD_TEXT
    };
    return (unsigned)id < SDK_WORD_COUNT ? words[id] : "";
}

/* The parser's sRGB decode of a colour parm's constant (VK 0x141a035e0 / GL 0x141a1f680): below 0 (or NaN) 0, from 1
 * up 1, else the sRGB curve. A colour the SDK writes where no parser decodes it (an fx value, a per-entity parm) is
 * decoded the same way, so it draws as the same constant in a material would. */
static __inline double sdk_srgb_decode(double x)
{
    if (!(x > 0)) return 0;
    if (x >= 1) return 1;
    if (x <= 0.04045) return x / 12.92;
    return pow((x + 0.055) / 1.055, 2.4);
}
#define SDK_W(id) sdk_word(SDK_##id)

#endif /* SNAPMAP_PLUS_SDKWORDS_H */
