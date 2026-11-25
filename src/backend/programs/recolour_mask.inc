/* The installed customization shader reads its region mask from a baked virtual-texture plane.
 * An authored mask uses the model UVs and preserves the stock colour, light and wound operations. */
static const char *const RC_TEXTURES[] = { "recolourmask" };
static const unsigned RC_TEXTURE_COUNT = 1;

static char *rc_replace(const char *text, size_t len, const char *from, const char *to, size_t *n)
{
    const char *at = mp_find(text, len, from), *end = text + len;
    mp_buf b = { 0 };
    if (!at || mp_find(at + 1, (size_t)(end - at - 1), from)) return NULL;
    mp_put(&b, text, (size_t)(at - text));
    mp_puts(&b, to);
    at += strlen(from);
    mp_put(&b, at, (size_t)(end - at));
    return mp_take(&b, n);
}

static char *rc_decl(const char *text, size_t len, size_t *n)
{
    return rc_replace(text, len, "CUSTOMIZATION_EFFECT_CHARACTER( inputs, output_color );",
                      "inputs.colorMask.xyz = tex2D( $recolourMask, fragment.texcoord0.xy ).xyz;\n"
                      "\t\t\tCUSTOMIZATION_EFFECT_CHARACTER( inputs, output_color );", n);
}

static char *rc_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len)
{
    static const char end_sampler[] = "uniform sampler2D samp_vmtrphysicalpagesmap3;";
    char sampler[160], *shifted, *declared, *out;
    size_t n;
    if (!(shifted = mp_glsl_bindings(text, len, tex_end, shift, &n))) return NULL;
    snprintf(sampler, sizeof sampler, "%s\nlayout( binding = %u ) uniform sampler2D samp_recolourmask;", end_sampler, tex_end);
    declared = rc_replace(shifted, n, end_sampler, sampler, &n);
    free(shifted);
    if (!declared) return NULL;
    out = rc_replace(declared, n, "inputs.colorMask.xyz= sampleColorMask.xyz;",
                     "inputs.colorMask.xyz = texture( samp_recolourmask, vofi_TexCoord0.xy ).xyz;", out_len);
    free(declared);
    return out;
}

static uint32_t *rc_splice(const uint32_t *stock, size_t words, unsigned tex_end, unsigned shift,
                           size_t *out_words, const char **why)
{
    mp_ctx c = { 0 };
    mp_list code = { 0 }, empty = { 0 };
    uint32_t inputs, uv, native, dest = 0, *out = NULL;
    const mp_ins *invar, *inptr, *vary, *vp, *v4, *sampler, *sptr;
    size_t first, last, anchor = 0;
    if (!mp_parse(&c.S, stock, words) || !mp_main(&c, &first, &last)) MP_FAIL(&c, "its fragment has no main");
    inputs = mp_named(&c.S, "inputs", 0);
    uv = mp_named(&c.S, "vofi_TexCoord0", 0);
    native = mp_named(&c.S, "samp_vmtrphysicalpagesmap3", 0);
    invar = inputs ? mp_def(&c.S, inputs) : NULL;
    inptr = invar ? mp_def(&c.S, invar->w[1]) : NULL;
    vary = uv ? mp_def(&c.S, uv) : NULL;
    vp = vary ? mp_def(&c.S, vary->w[1]) : NULL;
    v4 = vp && MP_OP(vp) == OpTypePointer ? mp_def(&c.S, vp->w[3]) : NULL;
    sampler = native ? mp_def(&c.S, native) : NULL;
    sptr = sampler ? mp_def(&c.S, sampler->w[1]) : NULL;
    if (!inptr || MP_OP(inptr) != OpTypePointer || !v4 || MP_OP(v4) != OpTypeVector || v4->w[3] != 4 ||
        !sptr || MP_OP(sptr) != OpTypePointer || sptr->w[2] != 0) MP_FAIL(&c, "its mask inputs have changed");
    int member = mp_member(&c.S, inptr->w[3], "colorMask");
    if (member < 0) MP_FAIL(&c, "its lighting inputs have no colour mask");
    for (size_t i = first; i < last; i++) {
        const mp_ins *store = &c.S.ins[i];
        if (MP_OP(store) != OpStore) continue;
        const mp_ins *chain = mp_def(&c.S, store->w[1]);
        if (!chain || MP_OP(chain) != OpAccessChain || MP_N(chain) != 5 || chain->w[3] != inputs) continue;
        const mp_ins *index = mp_def(&c.S, chain->w[4]);
        if (!index || MP_OP(index) != OpConstant || index->w[3] != (unsigned)member) continue;
        if (anchor) MP_FAIL(&c, "its fragment sets the lighting colour mask twice");
        anchor = i;
        dest = store->w[1];
    }
    if (!anchor) MP_FAIL(&c, "its fragment does not assign the lighting colour mask");
    if (!mp_shift_bindings(&c, tex_end, shift)) goto done;
    {
        uint32_t scalar = v4->w[2], vec4 = v4->w[1], vtype[3] = { 0, scalar, 2 };
        uint32_t vec2 = mp_find_or_add(&c, OpTypeVector, vtype, 3, 1);
        vtype[2] = 3;
        uint32_t vec3 = mp_find_or_add(&c, OpTypeVector, vtype, 3, 1);
        uint32_t tex = c.S.bound++, coord = c.S.bound++, xy = c.S.bound++, image = c.S.bound++;
        uint32_t sample = c.S.bound++, rgb = c.S.bound++;
        if (!vec2 || !vec3) MP_FAIL(&c, "out of memory");
#define RC_EMIT(list, op, ...) do { const uint32_t a[] = { __VA_ARGS__ }; \
    if (!mp_emit(&c.S, list, op, a, sizeof a / sizeof a[0])) MP_FAIL(&c, "out of memory"); } while (0)
        RC_EMIT(&c.globals, OpVariable, sptr->w[1], tex, 0);
        RC_EMIT(&c.decos, OpDecorate, tex, 34, 0);
        RC_EMIT(&c.decos, OpDecorate, tex, 33, tex_end);
        RC_EMIT(&code, OpLoad, vec4, coord, uv);
        RC_EMIT(&code, OpVectorShuffle, vec2, xy, coord, coord, 0, 1);
        RC_EMIT(&code, OpLoad, sptr->w[3], image, tex);
        RC_EMIT(&code, OpImageSampleImplicitLod, vec4, sample, image, xy);
        RC_EMIT(&code, OpVectorShuffle, vec3, rgb, sample, sample, 0, 1, 2);
        RC_EMIT(&code, OpStore, dest, rgb);
#undef RC_EMIT
    }
    out = mp_reassemble(&c, (size_t)-1, &empty, anchor, &code, &empty, out_words);
done:
    if (why) *why = c.why;
    free(code.v);
    mp_ctx_free(&c);
    return out;
}
