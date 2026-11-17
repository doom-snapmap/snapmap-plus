// programs_rp_test.js -- the product's render programs have ONE source each (src/backend/programs/shaders/*.rp), and both
// renderers get the same rule from it: the OpenGL texts are the .rp sections as written, the Vulkan stage text and
// SPIR-V are tools/gen_programs.js's translation of the same functions. This checks (a) the translation compiles and
// its rules do what they say, (b) the call blocks load exactly what modprog.c's SPIR-V splice loads, in its order,
// (c) the generated files are what the sources give.
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
    assert.ok(p.glsl.includes('ml_blend(') || p.glsl.includes('ep_shade('), `${name}: the entry function is in the stage text`);
    assert.ok(!/\$|\bfloat[234]\b|\blerp\b|\bfrac\b|\bsaturate\b|\btex2D/.test(p.glsl), `${name}: the stage text is GLSL`);
  }
  if (!glslangPresent()) { t.skip('glslangValidator is not at hand (set GLSLANG_VALIDATOR)'); return; }
  for (const [name, p] of Object.entries(progs)) {
    const words = gen.compile(p.spirvSource);
    assert.ok(words.length > 1000, `${name}: SPIR-V words`);
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
});

test('the call blocks load what the splice loads, in its order', () => {
  // the multilayer blend: P is SH_MODPROG_ML_PARMS, E the program's emissive factors
  const ml = progs.layered.call;
  const mlP = arrayInit(ml, 'mlP');
  assert.deepEqual(parmNames(mlP.items), table('SH_MODPROG_ML_PARMS'));
  assert.equal(mlP.size, mlP.items.length);
  assert.ok(new RegExp(`ml_blend\\( [^;]*\\$time\\.x, mlP, mlE \\)`).test(ml.replace(/\s+/g, ' ')), 'ml_blend is called with $time.x, mlP, mlE');
  const mlE = arrayInit(ml, 'mlE');
  assert.deepEqual(parmNames(mlE.items), table('MP_EMISSIVE'));
  const header = progs.layered.header;
  assert.match(header, new RegExp(`float4 P\\[${mlP.size}\\]`));
  assert.match(header, new RegExp(`float4 E\\[${mlE.size}\\]`));
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
  assert.match(modprogC, /EP_BLOCK_PARMS\[\] = \{[^}]*"eternalparticle"/);   // the first four are the product's own
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
  // and neither reads a render parm (the call blocks pass them in)
  for (const p of Object.values(progs)) assert.ok(!/\$\w+\.\w/.test(p.header), 'a parm read in a header');
});

test('the generated files are what the sources give (gen_programs.js --check)', () => {
  const r = spawnSync(process.execPath, [path.join(root, 'tools', 'gen_programs.js'), '--check'], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stdout + r.stderr);
});
