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

#define SDK_WORDS(X)                                                                                               \
    /* the first key of every SDK file, and its kinds */                                                           \
    X(TYPE, "type") X(KIND_EFFECT, "effect") X(KIND_ENVIRONMENT, "environment")                                    \
    /* an effect's items */                                                                                        \
    X(IT_LIGHT, "light") X(IT_DECAL, "decal") X(IT_SHAKE, "shake") X(IT_FLASH, "flash") X(IT_SCREEN, "screen")     \
    X(IT_BLUR, "blur") X(IT_FOCUS, "focus") X(IT_FLARE, "flare") X(IT_TRAIL, "trail") X(IT_MODEL, "model")         \
    X(IT_SOUND, "sound") X(IT_GLOW, "glow") X(IT_ENVIRONMENT, "environment") X(IT_WIND, "wind")                    \
    X(IT_PARTICLES, "particles")                                                                                   \
    /* timing and placement, any item (and `lasts` on the effect) */                                               \
    X(F_STARTS, "starts") X(F_LASTS, "lasts") X(F_FADEIN, "fadein") X(F_FADEOUT, "fadeout") X(F_REPEATS, "repeats") \
    X(F_OFFSET, "offset") X(F_AT, "at") X(F_WHEN, "when") X(F_REACH, "reach") X(F_FADESFROM, "fadesfrom")          \
    X(F_TURN, "turn")                                                                                              \
    /* item fields */                                                                                              \
    X(F_COLOUR, "colour") X(F_BRIGHTNESS, "brightness") X(F_FLICKER, "flicker") X(F_SHAPE, "shape")                \
    X(F_SHINE, "shine") X(F_SHADOWS, "shadows") X(F_IMAGE, "image") X(F_MATERIAL, "material") X(F_SIZE, "size")    \
    X(F_DEPTH, "depth") X(F_ANGLE, "angle") X(F_GLOW, "glow") X(F_GLOWLASTS, "glowlasts") X(F_STAYS, "stays")      \
    X(F_STRENGTH, "strength") X(F_STYLE, "style") X(F_TILES, "tiles") X(F_MIRROR, "mirror") X(F_BOTTOM, "bottom")  \
    X(F_NEAR, "near") X(F_FOCUS, "focus") X(F_FAR, "far") X(F_USE, "use") X(F_MODEL, "model") X(F_SOUND, "sound")  \
    X(F_STOPS, "stops") X(F_VALUE, "value")                                                                        \
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
    /* script calls (the playing calls are fx/fx_move/fx_stop until R-NM1's rename lands) */                       \
    X(C_PLAY_EFFECT, "fx") X(C_SET_ENVIRONMENT, "set_environment") X(C_CLEAR_ENVIRONMENT, "clear_environment")     \
    X(C_ENVIRONMENT_AREA, "environment_area") X(C_RECOLOUR, "recolour") X(C_GAME, "game")

typedef enum sdk_word_id {
#define SDK_WORD_ENUM(id, w) SDK_##id,
    SDK_WORDS(SDK_WORD_ENUM)
#undef SDK_WORD_ENUM
    SDK_WORD_COUNT
} sdk_word_id;

/* The word an author writes for `id`. */
const char *sdk_word(sdk_word_id id);
#define SDK_W(id) sdk_word(SDK_##id)

#endif /* SNAPMAP_PLUS_SDKWORDS_H */
