// programs_rp_test.js -- each render program has one source (src/backend/programs/shaders/*.rp): OpenGL gets the .rp
// text, Vulkan the translation by tools/gen_programs.js. Checks the translation compiles and obeys its rules, the call
// blocks load what modprog.c's SPIR-V splice loads in its order, and the generated files match the sources.
const { test } = require('node:test');
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const gen = require(path.join(root, 'tools', 'gen_programs.js'));
const modprogC = fs.readFileSync(path.join(root, 'src', 'backend', 'modprog.c'), 'utf8').replace(/\r\n/g, '\n');
const progs = gen.buildPrograms();

// the strings of a C string-array table in modprog.c
function table(name) {
  const m = new RegExp(`\\b${name}\\[\\]\\s*=\\s*\\{([^}]*)\\};`).exec(modprogC);
  assert.ok(m, `modprog.c has no table ${name}`);
  return [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]);
}

// the entries of `float4 <array>[N] = { ... };` in a call block, in order, and N
function arrayInit(call, array) {
  const m = new RegExp(`float4 ${array}\\[(\\d+)\\]\\s*=\\s*\\{([^}]*)\\}\\s*;`).exec(call);
  assert.ok(m, `the call block has no array ${array}`);
  return { size: Number(m[1]), items: m[2].split(',').map((x) => x.trim()).filter(Boolean) };
}
const parmNames = (items) => items.map((x) => {
  assert.match(x, /^\$\w+$/, `'${x}' is not a render parm`);
  return x.slice(1).toLowerCase();
});

function glslangPresent() {
  const r = spawnSync(process.env.GLSLANG_VALIDATOR || 'glslangValidator', ['--version'], { encoding: 'utf8' });
  return !r.error;
}

test('each .rp header translates and compiles (glslangValidator)', (t) => {
  for (const [name, p] of Object.entries(progs)) {
    assert.ok(p.glsl.includes('ml_blend(') || p.glsl.includes('ep_shade(') || p.glsl.includes('hr_fade('), `${name}: the entry function is in the stage text`);
    assert.ok(!/\$|\bfloat[234]\b|\blerp\b|\bfrac\b|\bsaturate\b|\btex2D/.test(p.glsl), `${name}: the stage text is GLSL`);
  }
  if (!glslangPresent()) { t.skip('glslangValidator is not at hand (set GLSLANG_VALIDATOR)'); return; }
  for (const [name, p] of Object.entries(progs)) {
    const words = gen.compile(p.spirvSource);
    assert.ok(words.length > (name === 'hair' ? 200 : 1000), `${name}: SPIR-V words`);
  }
});

test('the translator rules', () => {
  const sampler = (lc) => `samp_${lc}`;
  const tr = (s) => gen.translate(s, { sampler }).replace(/\s+/g, ' ').trim();
  assert.equal(tr('float2 a = lerp( float3( 1.0f, 2.0, 3f ).xy, frac( b ), c );'), 'vec2 a = mix( vec3( 1.0, 2.0, 3.0 ).xy, fract( b ), c );');
  assert.equal(tr('static const float k = 0.5f;'), 'const float k = 0.5;');
  assert.equal(tr('BRANCH if ( m != 0.0 ) x = ddx( y ) + _float4( 1.0 ).x;'), 'if ( m != 0.0 ) x = dFdx( y ) + vec4( 1.0 ).x;');
  assert.equal(tr('float4x4 m; float3x3 n; float2x2 o;'), 'mat4 m; mat3 n; mat2 o;');
  assert.equal(tr('a = saturate( b * ( c + 1.0 ) );'), 'a = clamp( b * ( c + 1.0 ), 0.0, 1.0 );');
  assert.equal(tr('a = tex2D( $transMap, uv ).x + tex2D( $transMap, uv, dx, dy ).y;'),
               'a = texture( samp_transmap, uv ).x + textureGrad( samp_transmap, uv, dx, dy ).y;');
  assert.equal(tr('a = tex2Dlod( s, float4( ( uv - 0.5 ) / r, 0.0, 0.0 ) ).xyz;'), 'a = textureLod( s, ( uv - 0.5 ) / r, 0.0 ).xyz;');
  assert.equal(tr('clip( g - 1e-6 );'), 'if ( g < 1e-6 ) discard;');
  assert.equal(tr('clip( g * 2.0 );'), 'if ( g * 2.0 < 0.0 ) discard;');
  assert.equal(tr('clip( -g - h + 1.0 );'), 'if ( -g - h + 1.0 < 0.0 ) discard;');
  // refused, with the reason
  const refused = (s, re) => assert.throws(() => gen.translate(s, { sampler: (lc, raw) => { throw new Error(`${raw} is not a texture`); } }), re);
  refused('x = $projectionMatrixZ.w;', /\$projectionMatrixZ is not a texture/);
  refused('#define X 1', /# directive/);
  refused('x = mul( a, b );', /'mul' is the engine's own/);
  refused('half3 c;', /'half3'/);
  refused('x = tex2Dbias( s, v );', /'tex2Dbias'/);
  refused('x = tex2Dlod( s, float4( uv, 1.0, 0.0 ) );', /tex2Dlod is translated only as/);
  // texelFetch and int2 pass through (the engine's language has GLSL's own texelFetch; int2 is ivec2), with the integer
  // operators on the coordinate and a texture parm as the sampler
  assert.equal(tr('float4 p( int k ) { return texelFetch( $layerParms, int2( k % 8, k / 8 ), 0 ); }'),
               'vec4 p( int k ) { return texelFetch( samp_layerparms, ivec2( k % 8, k / 8 ), 0 ); }');
  assert.equal(tr('float4 v = texelFetch( s, int2( 3, 1 ), 0 ) * 2.0f;'), 'vec4 v = texelFetch( s, ivec2( 3, 1 ), 0 ) * 2.0;');
});

test('the call blocks load what the splice loads, in its order', () => {
  // the layered surface: its 28 vectors are not render parms (the call block passes none: the program reads the
  // parameter image), E is the program's emissive factors
  const ml = progs.layered.call;
  assert.ok(!/float4 mlP\b/.test(ml), 'the layered call passes no vector parms');
  assert.ok(new RegExp(`ml_blend\\( [^;]*\\$time\\.x, mlE \\)`).test(ml.replace(/\s+/g, ' ')), 'ml_blend is called with $time.x, mlE');
  const mlE = arrayInit(ml, 'mlE');
  assert.deepEqual(parmNames(mlE.items), table('MP_EMISSIVE'));
  const header = progs.layered.header;
  assert.match(header, new RegExp(`float4 E\\[${mlE.size}\\]`));
  assert.ok(!/float4 P\[/.test(header), 'the blend takes no parameter array');
  // the parameter image: every vector the header fetches is one of the table's 28, and all 28 are read
  const names = table('SH_MODPROG_ML_PARMS');
  assert.equal(names.length, 28);
  const fetched = new Set();
  for (const m of header.matchAll(/ml_p\( (\d+)( \+ i)? \)/g)) {
    const k = Number(m[1]);
    assert.ok(k + (m[2] ? 3 : 0) < 28, `ml_p( ${k} ) is past the image`);
    for (let i = 0; i <= (m[2] ? 3 : 0); i++) fetched.add(k + i);
  }
  assert.deepEqual([...fetched].sort((a, b) => a - b), names.map((_, k) => k), 'every vector is fetched');
  assert.match(header, /texelFetch\( \$layerParms, int2\( k % 8, k \/ 8 \), 0 \)/);
  // the splice passes the lighting inputs in this order
  const inout = table('MP_INOUT');
  const m = /ml_blend\(([^;]*)\);/.exec(ml.replace(/\s+/g, ' '));
  const args = m[1].split(',').map((x) => x.trim());
  assert.deepEqual(args.slice(0, 7), inout.map((x) => `inputs.${x}`));
  assert.equal(args[7], 'inputs.texCoords');

  // the particle shading: V the stage inputs, P the material uniforms, U the program's own
  const ep = progs.particles.call;
  const varyings = table('EP_VARYINGS').map((v) => (v === 'vofi_Color' ? 'fragment.color' : v === 'gl_FragCoord' ? 'fragment.position'
    : `fragment.texcoord${/^vofi_TexCoord(\d+)$/.exec(v)[1]}`));
  const epV = arrayInit(ep, 'epV');
  assert.deepEqual(epV.items, varyings);
  const epP = arrayInit(ep, 'epP');
  assert.deepEqual(parmNames(epP.items), table('EP_BLOCK_PARMS'));
  const epU = arrayInit(ep, 'epU');
  assert.deepEqual(parmNames(epU.items), table('EP_UNIFORMS').map((x) => x.slice(1)));
  assert.ok(table('EP_UNIFORMS').every((x) => 'hl'.includes(x[0])));
  assert.match(ep.replace(/\s+/g, ' '), /ep_shade\( result\.color, epV, epP, epU \);/);
  assert.match(progs.particles.header, new RegExp(`float4 V\\[${epV.size}\\], float4 P\\[${epP.size}\\], float4 U\\[${epU.size}\\]`));
  // the three appended parameters are the game's own vectors (smokebrightness, which nothing else names), and the
  // product's programs declare no vector render parm
  assert.deepEqual(table('EP_BLOCK_PARMS'), ['smokebrightness', 'projectionmatrixz', 'globalvieworigin']);
  assert.deepEqual(table('EP_UNIFORMS').slice(7), ['hparticleuberparms1', 'hparticleuberparms2', 'hparticleuberparms3', 'hemissive']);
  const decls = modprogC.slice(modprogC.indexOf('MP_PARM_DECLS[] = {'), modprogC.indexOf('#define MP_NPARM_DECLS'));
  assert.ok(!/\{ Vec\b/.test(decls), 'the product derives no vector render parm decl');
  assert.match(decls, /"layerparms"/);
  assert.match(decls, /"particleemissivemap"/);

  // the hair programs' death fade: V the card's uv, P the render parm the splice appends (the game's own
  // deathfadeopacity, which no hair program reads)
  const hr = progs.hair.call;
  const hrVaryings = table('HR_VARYINGS').map((v) => `fragment.texcoord${/^vofi_TexCoord(\d+)$/.exec(v)[1]}`);
  const hrV = arrayInit(hr, 'hrV');
  assert.deepEqual(hrV.items, hrVaryings);
  const hrP = arrayInit(hr, 'hrP');
  assert.deepEqual(parmNames(hrP.items), table('HR_BLOCK_PARMS'));
  assert.deepEqual(table('HR_BLOCK_PARMS'), ['deathfadeopacity']);
  assert.match(hr.replace(/\s+/g, ' '), /hr_fade\( hrV, hrP \);/);
  assert.match(progs.hair.header, new RegExp(`float4 V\\[${hrV.size}\\], float4 P\\[${hrP.size}\\]`));
  // the fade is 2016's own (includes/effects.inc, CALC_DEATH_FADE_CLIP_VAL): the opacity less half the noise, less 0.1
  const fade = progs.hair.header.replace(/\s+/g, ' ');
  assert.match(fade, /BRANCH if \( P\[0\]\.x < 1\.0 \)/);
  assert.match(fade, /clip\( P\[0\]\.x - dither \* 0\.5 - 0\.1 \);/);
  assert.ok(!/magnificationclouds/.test(decls), 'the noise is the game\'s own render parm, not a product decl');
});

test('the header reads exactly the program\'s textures', () => {
  const used = (h) => new Set([...h.matchAll(/\$(\w+)/g)].map((x) => x[1].toLowerCase()));
  // the multilayer blend: its textures in order, then the program's own noise
  const ml = used(progs.layered.header);
  assert.deepEqual([...ml].sort(), [...table('SH_MODPROG_ML_TEXTURES'), 'magnificationclouds'].sort());
  const spec = gen.layeredSpec();
  assert.deepEqual(spec.samplers.slice(0, -1).map((x) => x[1]), table('SH_MODPROG_ML_TEXTURES'));
  // the particle shading: the program's own three, then the emissive sheet it adds
  const ep = used(progs.particles.header);
  const stock = table('EP_SAMPLER_STOCK').filter(Boolean).map((x) => x.replace(/^samp_/, ''));
  assert.deepEqual([...ep].sort(), [...stock, ...table('SH_MODPROG_EP_TEXTURES')].sort());
  assert.deepEqual(gen.particlesSpec().samplers.map((x) => x[1]), [...stock, ...table('SH_MODPROG_EP_TEXTURES')]);
  // the hair fade: the noise it adds, and no other texture
  const hr = used(progs.hair.header);
  assert.deepEqual([...hr], table('SH_MODPROG_HR_TEXTURES'));
  assert.deepEqual(gen.hairSpec().samplers.map((x) => x[1]), table('SH_MODPROG_HR_TEXTURES'));
  // and none reads a render parm (the call blocks pass them in)
  for (const p of Object.values(progs)) assert.ok(!/\$\w+\.\w/.test(p.header), 'a parm read in a header');
});

test('the generated files are what the sources give (gen_programs.js --check)', () => {
  const r = spawnSync(process.execPath, [path.join(root, 'tools', 'gen_programs.js'), '--check'], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stdout + r.stderr);
});
