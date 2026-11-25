// gen_programs.js -- embeds the product's render-program shaders (modprog.h) in the backend: writes programs_layered.c,
// programs_particles.c and programs_hair.c under src/backend/programs and tests/modprog_fixture.c; --check exits 1 when stale.
// Each .rp source (shaders/layered.rp, particles.rp, hair.rp) has hlsl_fp_header and call sections; the OpenGL text is both as
// written, the Vulkan stage text is the header translated to GLSL (translate()), and the SPIR-V is that plus a stub built by
// glslangValidator (GLSLANG_VALIDATOR or PATH). --check compares texts always and SPIR-V only where a compiler is at hand.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const root = path.resolve(__dirname, '..');
const edir = path.join(root, 'src', 'backend', 'programs', 'shaders');   // the derived render programs' sources
const out = path.join(root, 'src', 'backend', 'programs', 'programs_layered.c');
const epOut = path.join(root, 'src', 'backend', 'programs', 'programs_particles.c');
const hrOut = path.join(root, 'src', 'backend', 'programs', 'programs_hair.c');
const fixtureOut = path.join(root, 'tests', 'modprog_fixture.c');

// Tokens: ws (with comments: they never matter to a rule), id, num, dollar (a $parm), pp (#), punct (one character).
function tokenize(text) {
  const toks = [];
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    let m;
    if (/\s/.test(c)) { m = /^\s+/.exec(text.slice(i)); toks.push({ k: 'ws', s: m[0] }); i += m[0].length; }
    else if (c === '/' && text[i + 1] === '/') { const e = text.indexOf('\n', i); const n = e < 0 ? text.length : e; toks.push({ k: 'ws', s: text.slice(i, n), comment: true }); i = n; }
    else if (c === '/' && text[i + 1] === '*') { const e = text.indexOf('*/', i + 2); if (e < 0) throw new Error('an unterminated /* comment'); toks.push({ k: 'ws', s: text.slice(i, e + 2), comment: true }); i = e + 2; }
    else if ((m = /^[A-Za-z_]\w*/.exec(text.slice(i)))) { toks.push({ k: 'id', s: m[0] }); i += m[0].length; }
    else if ((m = /^(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?[fF]?/.exec(text.slice(i)))) { toks.push({ k: 'num', s: m[0] }); i += m[0].length; }
    else if (c === '$') {
      m = /^\$([A-Za-z_]\w*)/.exec(text.slice(i));
      if (!m) throw new Error('a $ that names no parm');
      toks.push({ k: 'dollar', s: m[0], name: m[1] }); i += m[0].length;
    }
    else if (c === '#') { toks.push({ k: 'pp', s: c }); i++; }
    else { toks.push({ k: 'punct', s: c }); i++; }
  }
  return toks;
}

// The section `name { ... }` of an .rp file: its braced text, exactly (the file's other top-level text is comments).
function parseRp(text, file) {
  const toks = tokenize(text.replace(/\r\n/g, '\n'));
  const secs = {};
  let i = 0;
  const skipWs = () => { while (i < toks.length && toks[i].k === 'ws') i++; };
  for (skipWs(); i < toks.length; skipWs()) {
    if (toks[i].k !== 'id' || !['hlsl_fp_header', 'call'].includes(toks[i].s)) throw new Error(`${file}: only hlsl_fp_header and call sections are allowed at the top level, found '${toks[i].s}'`);
    const name = toks[i++].s;
    skipWs();
    if (!toks[i] || toks[i].s !== '{') throw new Error(`${file}: section ${name} has no {`);
    if (secs[name] !== undefined) throw new Error(`${file}: section ${name} twice`);
    let depth = 0;
    const body = [];
    for (; i < toks.length; i++) {
      if (toks[i].k !== 'ws' && toks[i].s === '{') depth++;
      if (toks[i].k !== 'ws' && toks[i].s === '}') depth--;
      if (depth === 0) break;
      if (!(depth === 1 && body.length === 0 && toks[i].s === '{')) body.push(toks[i].s);
    }
    if (depth !== 0) throw new Error(`${file}: section ${name} is not closed`);
    i++;
    secs[name] = dedent(body.join(''));
  }
  for (const n of ['hlsl_fp_header', 'call']) if (secs[n] === undefined) throw new Error(`${file}: no ${n} section`);
  return secs;
}

// Without its blank first and last lines and the indentation every line shares; tabs and spaces count as themselves.
function dedent(text) {
  const lines = text.replace(/[ \t]+$/gm, '').split('\n');
  while (lines.length && lines[0].trim() === '') lines.shift();
  while (lines.length && lines[lines.length - 1].trim() === '') lines.pop();
  let common = null;
  for (const l of lines) {
    if (l.trim() === '') continue;
    const ind = /^[ \t]*/.exec(l)[0];
    if (common === null) common = ind;
    else { let k = 0; while (k < common.length && k < ind.length && common[k] === ind[k]) k++; common = common.slice(0, k); }
  }
  return lines.map((l) => l.slice(common ? Math.min(common.length, /^[ \t]*/.exec(l)[0].length) : 0)).join('\n') + '\n';
}

// The token rules from the engine's HLSL-like language to GLSL: type, function and intrinsic renames, dropped qualifiers
// (static, BRANCH, LOOP, IFANY, UNROLL, FLATTEN), saturate, tex2D, tex2Dlod with a zero-lod float4 and clip rewritten, and
// $name -> the program's sampler (samp_<lowercase name> in stage text; a $ that is not a texture parm is refused). A # directive
// and the engine's own mul, half*, fixed, other tex* lookups and similar are refused; anything else passes through.
const TYPES = { float2: 'vec2', float3: 'vec3', float4: 'vec4', int2: 'ivec2', int3: 'ivec3', int4: 'ivec4',
  uint2: 'uvec2', uint3: 'uvec3', uint4: 'uvec4', bool2: 'bvec2', bool3: 'bvec3', bool4: 'bvec4',
  float2x2: 'mat2', float3x3: 'mat3', float4x4: 'mat4' };
const RENAMES = { lerp: 'mix', frac: 'fract', ddx: 'dFdx', ddy: 'dFdy', rsqrt: 'inversesqrt',
  _float2: 'vec2', _float3: 'vec3', _float4: 'vec4' };
const DROPPED = new Set(['static', 'BRANCH', 'LOOP', 'IFANY', 'UNROLL', 'FLATTEN']);
const REFUSED = /^(mul|half[234]?|fixed|SineCosine|anyEqual|anyNotEqual|anyNotEqualZero|register|groupshared|tex(1D|3D|CUBE|2DARRAY|2Dbias|2Dproj|2DGather)\w*)$/;

function nextSig(toks, i) { while (i < toks.length && toks[i].k === 'ws') i++; return i; }

// The index of the ) closing the ( at toks[open], and the top-level comma-separated argument token lists between.
function callArgs(toks, open) {
  const args = [[]];
  let depth = 0, i = open;
  for (; i < toks.length; i++) {
    const t = toks[i];
    if (t.k !== 'ws' && '([{'.includes(t.s)) { depth++; if (depth === 1) continue; }
    if (t.k !== 'ws' && ')]}'.includes(t.s)) { depth--; if (depth === 0) break; }
    if (depth === 1 && t.k === 'punct' && t.s === ',') { args.push([]); continue; }
    args[args.length - 1].push(t);
  }
  if (i >= toks.length) throw new Error('a call is not closed');
  return { args, close: i };
}

const sig = (toks) => toks.filter((t) => t.k !== 'ws');
const isZero = (toks) => { const s = sig(toks); return s.length === 1 && s[0].k === 'num' && Number(s[0].s.replace(/[fF]$/, '')) === 0; };

// opts.sampler(lowercaseName) -> the identifier for a $texture, or throws.
function translate(text, opts) {
  const toks = tokenize(text);
  return tr(toks, opts).replace(/[ \t]+$/gm, '');
}

function tr(toks, opts) {
  let o = '';
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.k === 'pp') throw new Error('a # directive in a header: only functions and constants are translated');
    if (t.k === 'dollar') { o += opts.sampler(t.name.toLowerCase(), t.s); continue; }
    if (t.k === 'num') {
      const m = /^(.*?)[fF]$/.exec(t.s);
      o += m ? (/[.eE]/.test(m[1]) ? m[1] : m[1] + '.0') : t.s;
      continue;
    }
    if (t.k !== 'id') { o += t.s; continue; }
    const name = t.s;
    if (DROPPED.has(name)) { if (toks[i + 1] && toks[i + 1].k === 'ws' && !toks[i + 1].comment && !toks[i + 1].s.includes('\n')) i++; continue; }
    if (REFUSED.test(name)) throw new Error(`'${name}' is the engine's own and is not translated`);
    if (/^float\d+x\d+$/.test(name) && !TYPES[name]) { o += 'mat' + name.slice(5); continue; }
    if (name === 'saturate' || name === 'tex2D' || name === 'tex2Dlod' || name === 'clip') {
      const p = nextSig(toks, i + 1);
      if (!toks[p] || toks[p].s !== '(') throw new Error(`'${name}' used without a call`);
      const { args, close } = callArgs(toks, p);
      const a = args.map((x) => tr(x, opts).trim());
      if (name === 'saturate') {
        if (a.length !== 1) throw new Error('saturate takes one argument');
        o += `clamp( ${a[0]}, 0.0, 1.0 )`;
      } else if (name === 'tex2D') {
        if (a.length === 2) o += `texture( ${a[0]}, ${a[1]} )`;
        else if (a.length === 4) o += `textureGrad( ${a[0]}, ${a[1]}, ${a[2]}, ${a[3]} )`;
        else throw new Error('tex2D takes (sampler, uv) or (sampler, uv, dx, dy)');
      } else if (name === 'tex2Dlod') {
        if (a.length !== 2) throw new Error('tex2Dlod takes (sampler, float4( uv, 0, 0 ))');
        const s = sig(args[1]);
        const q = s.length > 1 && (s[0].s === 'float4' || s[0].s === 'vec4') && s[1].s === '(' ? callArgs(args[1], args[1].indexOf(s[1])) : null;
        if (!q || q.args.length !== 3 || !isZero(q.args[1]) || !isZero(q.args[2]) || sig(args[1].slice(q.close + 1)).length)
          throw new Error('tex2Dlod is translated only as tex2Dlod( sampler, float4( uv, 0.0, 0.0 ) )');
        o += `textureLod( ${a[0]}, ${tr(q.args[0], opts).trim()}, 0.0 )`;
      } else {
        if (a.length !== 1) throw new Error('clip takes one argument');
        // the top-level binary + and - of the argument (an operator after an operand)
        const x = args[0];
        let depth = 0, split = -1, n = 0, prev = null;
        x.forEach((t2, k) => {
          if (t2.k === 'ws') return;
          if (t2.k === 'punct' && '([{'.includes(t2.s)) depth++;
          else if (t2.k === 'punct' && ')]}'.includes(t2.s)) depth--;
          else if (depth === 0 && t2.k === 'punct' && (t2.s === '+' || t2.s === '-') && prev &&
                   (prev.k === 'id' || prev.k === 'num' || prev.k === 'dollar' || (prev.k === 'punct' && ')]'.includes(prev.s)))) { n++; split = k; }
          prev = t2;
        });
        if (n === 1 && x[split].s === '-') o += `if ( ${tr(x.slice(0, split), opts).trim()} < ${tr(x.slice(split + 1), opts).trim()} ) discard`;
        else o += `if ( ${a[0]} < 0.0 ) discard`;
      }
      i = close;
      continue;
    }
    o += RENAMES[name] || TYPES[name] || name;
  }
  return o;
}

// A program: its source, the samplers its header names ([the stub's sampler, the texture parm]) and its stub.
function layeredSpec() {
  // the parameter image takes the place of layer 0's height image: the program's 20 textures stay 20
  const samplers = [['ml_mask', 'layerblendmask'], ['ml_params', 'layerparms']];
  for (let i = 1; i <= 4; i++)
    for (const k of ['albedo', 'normal', 'smoothness', 'height']) samplers.push([`ml_${k}${i}`, `blendlayer${i}${k}map`]);
  samplers.push(['ml_extra0', 'multilayerextramap0'], ['ml_extra1', 'multilayerextramap1']);
  // the ember's noise is the program's own magnificationclouds sampler (no texture added)
  samplers.push(['ml_noise', 'magnificationclouds']);
  return {
    file: 'layered.rp', samplers, name: 'ML',
    stubHead: 'layout(location = 0) in vec4 ml_in_uv;\nlayout(location = 0) out vec4 ml_out;\n',
    stubTail: 'layout(binding = 30) uniform ml_parms_ubo { vec4 E[4]; vec4 time; } ml_parms;\n\n' +
      'void main()\n{\n' +
      '    vec3 albedo = vec3(0.5), specular = vec3(0.04), normal = vec3(0.5, 0.5, 0.0), lightmap = vec3(0.0);\n' +
      '    vec3 emissive = vec3(0.0);\n' +
      '    float smoothness = 0.5, thickness = 0.0;\n' +
      '    ml_blend(albedo, specular, smoothness, normal, lightmap, emissive, thickness, ml_in_uv, ml_parms.time.x,\n' +
      '             ml_parms.E);\n' +
      '    ml_out = vec4(albedo + specular + emissive + lightmap + normal, smoothness + thickness);\n}\n',
  };
}

function particlesSpec(extended = true) {
  return {
    file: 'particles.rp', name: extended ? 'EP' : 'EP_BASE', extended,
    // the shading's samplers and the material textures they are (the first three the program's own)
    samplers: [['ep_trans', 'transmap'], ['ep_depth', 'viewdepthmap'], ['ep_atlas', 'particleslightatlasmap'],
      ['ep_emissive', 'particleemissivemap']],
    stubHead: 'layout(location = 0) in vec4 ep_in;\nlayout(location = 0) out vec4 ep_out;\n',
    stubTail: 'layout(binding = 30) uniform ep_parms_ubo { vec4 V[7]; vec4 P[3]; vec4 U[11]; } ep_parms;\n\n' +
      'void main()\n{\n    vec4 frag = ep_in;\n    ep_shade(frag, ep_parms.V, ep_parms.P, ep_parms.U);\n    ep_out = frag;\n}\n',
  };
}

function hairSpec() {
  return {
    file: 'hair.rp', name: 'HR',
    // the dissolve's noise: a texture the hair programs gain (they have no noise of their own)
    samplers: [['hr_noise', 'magnificationclouds']],
    stubHead: 'layout(location = 0) out vec4 hr_out;\n',
    stubTail: 'layout(binding = 30) uniform hr_parms_ubo { vec4 V[1]; vec4 P[1]; } hr_parms;\n\n' +
      'void main()\n{\n    hr_fade(hr_parms.V, hr_parms.P);\n    hr_out = vec4(0.0);\n}\n',
  };
}

// What one program's source gives: { header, call (the OpenGL texts), glsl (the stage text), spirvSource }.
function buildProgram(spec) {
  let text = fs.readFileSync(path.join(edir, spec.file), 'utf8').replace(/\r\n/g, '\n');
  if (spec.file === 'particles.rp') {
    // Earlier published units pin the base program's exact bytes.
    text = text.replace(/^#ifdef PARTICLE_DISTANCE_MASK\n([\s\S]*?)^#endif\n/gm, (_, body) => {
      const [extended, base = ''] = body.split(/^#else\n/m);
      return spec.extended ? extended : base;
    });
  }
  const secs = parseRp(text, spec.file);
  const byParm = new Map(spec.samplers.map(([v, parm]) => [parm, v]));
  const sampler = (use) => (lc, raw) => {
    if (!byParm.has(lc)) throw new Error(`${spec.file}: ${raw} is read in the header, and it is not one of the program's textures (${[...byParm.keys()].join(', ')}): pass a render parm in from the call block`);
    return use(lc, byParm.get(lc));
  };
  let glsl, body;
  try {
    glsl = translate(secs.hlsl_fp_header, { sampler: sampler((lc) => `samp_${lc}`) });
    body = translate(secs.hlsl_fp_header, { sampler: sampler((lc, v) => v) });
  } catch (e) { throw new Error(`${spec.file}: ${e.message}`); }
  const decls = spec.samplers.map(([v], k) => `layout(binding = ${k}) uniform sampler2D ${v};`).join('\n');
  const spirvSource = `#version 450 core\n${decls}\n\n${spec.stubHead}\n${body.replace(/\s+$/, '')}\n\n${spec.stubTail}`;
  return { header: secs.hlsl_fp_header, call: secs.call, glsl, spirvSource, name: spec.name, spec };
}

function buildPrograms() {
  return { layered: buildProgram(layeredSpec()), particles: buildProgram(particlesSpec()), particlesBase: buildProgram(particlesSpec(false)), hair: buildProgram(hairSpec()) };
}

function compile(text) {
  const exe = process.env.GLSLANG_VALIDATOR || 'glslangValidator';
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'programs-'));
  const src = path.join(tmp, 'module.frag');
  const spv = path.join(tmp, 'module.spv');
  try {
    fs.writeFileSync(src, text);
    execFileSync(exe, ['-V', '--target-env', 'vulkan1.0', '-o', spv, src], { stdio: 'pipe' });
    const b = fs.readFileSync(spv);
    if (b.length % 4 || b.readUInt32LE(0) !== 0x07230203) throw new Error('glslangValidator wrote no SPIR-V');
    const words = [];
    for (let k = 0; k < b.length; k += 4) words.push(b.readUInt32LE(k));
    return words;
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

function cString(name, text) {
  let s = `const char ${name}[] =\n`;
  const lines = text.split('\n');
  if (lines[lines.length - 1] === '') lines.pop();
  for (const line of lines) {
    if (/[^\x20-\x7e\t]/.test(line)) throw new Error(`${name}: a line is not printable ASCII`);
    s += '    "' + line.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\t/g, '\\t') + '\\n"\n';
  }
  return s + ';\n';
}

function wordsC(name, words) {
  let c = `const unsigned ${name}_WORDS = ${words.length};\n`;
  c += `const unsigned ${name}[] = {`;
  words.forEach((w, k) => { c += (k % 8 === 0 ? '\n    ' : ' ') + '0x' + w.toString(16).padStart(8, '0') + 'u,'; });
  return c + '\n};\n\n';
}

function renderProgram(p, what) {
  return `/* programs_${what.file} -- GENERATED by tools/gen_programs.js from src/backend/programs/shaders/${p.spec.file}; do not edit.\n` +
         ` * ${what.desc} of the product's render programs (modprog.h): the Vulkan module, its functions as\n` +
         ' * stage text, and the OpenGL header section and call text. */\n#include "../modprog.h"\n\n' +
         wordsC(`SH_MODPROG_${p.name}_SPIRV`, what.words) + cString(`SH_MODPROG_${p.name}_GLSL`, p.glsl) + '\n' +
         cString(`SH_MODPROG_${p.name}_HEADER`, p.header) + '\n' + cString(`SH_MODPROG_${p.name}_CALL`, p.call);
}

function renderFixture(fixture, epFixture, hrFixture, words, epWords, hrWords) {
  return '/* modprog_fixture.c -- GENERATED by tools/gen_programs.js from tests/fixtures/modprog/lit.frag,\n' +
         ' * particle.frag and hair.frag; do not edit. modprog_test\'s stand-in lit, particle and hair programs: their\n' +
         ' * SPIR-V and text. */\n\n' +
         wordsC('MODPROG_FIXTURE_SPIRV', words) + cString('MODPROG_FIXTURE_GLSL', fixture) + '\n' +
         wordsC('MODPROG_FIXTURE_EP_SPIRV', epWords) + cString('MODPROG_FIXTURE_EP_GLSL', epFixture) + '\n' +
         wordsC('MODPROG_FIXTURE_HR_SPIRV', hrWords) + cString('MODPROG_FIXTURE_HR_GLSL', hrFixture);
}

function wordsOf(have, name) {
  const m = new RegExp(`const unsigned ${name}\\[\\] = \\{([^}]*)\\};`).exec(have);
  return m ? m[1].split(',').map((x) => x.trim()).filter(Boolean).map((x) => parseInt(x, 16)) : [];
}

function outputs() {
  const progs = buildPrograms();
  const read = (f) => fs.readFileSync(f, 'utf8').replace(/\r\n/g, '\n');
  const fixture = read(path.join(root, 'tests', 'fixtures', 'modprog', 'lit.frag'));
  const epFixture = read(path.join(root, 'tests', 'fixtures', 'modprog', 'particle.frag'));
  const hrFixture = read(path.join(root, 'tests', 'fixtures', 'modprog', 'hair.frag'));
  // each output: its file, the modules it embeds (source text and the C name of its words), its renderer
  return [
    [out, [[progs.layered.spirvSource, 'SH_MODPROG_ML_SPIRV']], (w) => renderProgram(progs.layered, { file: 'layered.c', desc: 'The layered surface', words: w })],
    [epOut, [[progs.particles.spirvSource, 'SH_MODPROG_EP_SPIRV'], [progs.particlesBase.spirvSource, 'SH_MODPROG_EP_BASE_SPIRV']],
      (extended, base) => renderProgram(progs.particles, { file: 'particles.c', desc: 'The particle shading', words: extended }) +
        '\n' + renderProgram(progs.particlesBase, { file: 'particles.c', desc: 'The base particle shading', words: base })],
    [hrOut, [[progs.hair.spirvSource, 'SH_MODPROG_HR_SPIRV']], (w) => renderProgram(progs.hair, { file: 'hair.c', desc: 'The hair programs\' death fade', words: w })],
    [fixtureOut, [[fixture, 'MODPROG_FIXTURE_SPIRV'], [epFixture, 'MODPROG_FIXTURE_EP_SPIRV'], [hrFixture, 'MODPROG_FIXTURE_HR_SPIRV']],
      (w, e, h) => renderFixture(fixture, epFixture, hrFixture, w, e, h)],
  ];
}

function main() {
  const check = process.argv.includes('--check');
  const outs = outputs();
  if (check) {
    for (const [file, mods, fn] of outs) {
      const have = fs.existsSync(file) ? fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') : '';
      let want = fn(...mods.map(([, name]) => wordsOf(have, name)));
      try { want = fn(...mods.map(([text]) => compile(text))); } catch (e) { if (e.code !== 'ENOENT') throw e; }
      if (have !== want) { console.error(`${path.basename(file)} is out of date: run node tools/gen_programs.js`); process.exit(1); }
    }
    console.log('programs_layered.c, programs_particles.c, programs_hair.c and modprog_fixture.c match their sources');
  } else {
    for (const [file, mods, fn] of outs) {
      const words = mods.map(([text]) => compile(text));
      fs.writeFileSync(file, fn(...words));
      console.log(`wrote ${path.relative(root, file)} (${words.map((w) => w.length).join(' + ')} SPIR-V words)`);
    }
  }
}

if (require.main === module) main();
module.exports = { tokenize, parseRp, translate, buildPrograms, compile, layeredSpec, particlesSpec, hairSpec };
