// gen_multilayer.js -- embed the shaders of the product's render programs (modprog.h) in the backend.
//   node tools/gen_multilayer.js           write src/backend/eternal/modprog_multilayer.c, src/backend/eternal/modprog_particles.c
//                                          and tests/modprog_fixture.c
//   node tools/gen_multilayer.js --check   exit 1 when they are not what the sources give
// Sources: src/backend/eternal/shaders/multilayer.frag (the Vulkan module, compiled here to SPIR-V 1.0 with
// glslangValidator: GLSLANG_VALIDATOR names it, else it is looked up on PATH) and
// src/backend/eternal/shaders/multilayer.decl.inc (the OpenGL text). The generated file holds the module's words, the
// module's functions as GLSL text (the stage text a cooked program carries beside its SPIR-V, samplers named as
// the engine names a material's textures) and the OpenGL text, so a build needs no shader compiler.
// src/backend/eternal/modprog_particles.c is the same for the particle program (eternal/shaders/eternalparticles.frag and
// eternalparticles.decl.inc). tests/modprog_fixture.c is modprog_test's stand-in program (tests/fixtures/modprog/lit.frag).
// --check compares the texts always and the SPIR-V only where a compiler is at hand (CI has none).
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const root = path.resolve(__dirname, '..');
const edir = path.join(root, 'src', 'backend', 'eternal', 'shaders');   // DOOM Eternal's shading, isolated
const out = path.join(root, 'src', 'backend', 'eternal', 'modprog_multilayer.c');
const fixtureSrc = path.join(root, 'tests', 'fixtures', 'modprog', 'lit.frag');
const fixtureOut = path.join(root, 'tests', 'modprog_fixture.c');
const frag = fs.readFileSync(path.join(edir, 'multilayer.frag'), 'utf8').replace(/\r\n/g, '\n');
const decl = fs.readFileSync(path.join(edir, 'multilayer.decl.inc'), 'utf8').replace(/\r\n/g, '\n');
const fixture = fs.readFileSync(fixtureSrc, 'utf8').replace(/\r\n/g, '\n');
const epFixture = fs.readFileSync(path.join(root, 'tests', 'fixtures', 'modprog', 'particle.frag'), 'utf8')
  .replace(/\r\n/g, '\n');
const check = process.argv.includes('--check');
const epOut = path.join(root, 'src', 'backend', 'eternal', 'modprog_particles.c');
const epFrag = fs.readFileSync(path.join(edir, 'eternalparticles.frag'), 'utf8').replace(/\r\n/g, '\n');
const epDecl = fs.readFileSync(path.join(edir, 'eternalparticles.decl.inc'), 'utf8').replace(/\r\n/g, '\n');
// the particle shading's samplers and the material textures they are (the first three the program's own)
const epSamplers = [['ep_trans', 'transmap'], ['ep_depth', 'viewdepthmap'], ['ep_atlas', 'particleslightatlasmap'],
  ['ep_emissive', 'eternalparticleemissivemap']];
const epStart = epFrag.indexOf('// The sRGB curve');
const epEnd = epFrag.indexOf('\nlayout(binding = 30) uniform ep_parms_ubo');
if (epStart < 0 || epEnd < 0) throw new Error('eternalparticles.frag: the module functions cannot be found');
let epGlsl = epFrag.slice(epStart, epEnd).replace(/\s+$/, '') + '\n';
for (const [v, parm] of epSamplers) epGlsl = epGlsl.replace(new RegExp(`\\b${v}\\b`, 'g'), `samp_${parm}`);

// The samplers, in binding order, and the names of the material textures they become.
const samplers = [['ml_mask', 'layerblendmask'], ['ml_height0', 'blendlayer0heightmap']];
for (let i = 1; i <= 4; i++)
  for (const k of ['albedo', 'normal', 'smoothness', 'height']) samplers.push([`ml_${k}${i}`, `blendlayer${i}${k}map`]);
samplers.push(['ml_extra0', 'multilayerextramap0'], ['ml_extra1', 'multilayerextramap1']);
// the ember's noise is the program's own magnificationclouds sampler (no texture added)
samplers.push(['ml_noise', 'magnificationclouds']);

// The stage text: from the first constant to the end of ml_blend, each sampler under its texture's name.
const start = frag.indexOf('const float ML_HEIGHT_MID');
const end = frag.indexOf('\nlayout(binding = 30) uniform ml_parms_ubo');
if (start < 0 || end < 0) throw new Error('multilayer.frag: the module functions cannot be found');
let glsl = frag.slice(start, end).replace(/\s+$/, '') + '\n';
for (const [v, parm] of samplers) glsl = glsl.replace(new RegExp(`\\b${v}\\b`, 'g'), `samp_${parm}`);

function compile(text) {
  const exe = process.env.GLSLANG_VALIDATOR || 'glslangValidator';
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'multilayer-'));
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

function render(words) {
  return '/* modprog_multilayer.c -- GENERATED by tools/gen_multilayer.js from src/backend/eternal/shaders/; do not edit.\n' +
         ' * The multilayer blend of the product\'s render programs (modprog.h): the Vulkan module, its functions as\n' +
         ' * stage text, and the OpenGL text. */\n#include "../modprog.h"\n\n' +
         wordsC('SH_MODPROG_ML_SPIRV', words) + cString('SH_MODPROG_ML_GLSL', glsl) + '\n' +
         cString('SH_MODPROG_ML_DECL', decl);
}

function renderParticles(words) {
  return '/* modprog_particles.c -- GENERATED by tools/gen_multilayer.js from src/backend/eternal/shaders/; do not edit.\n' +
         ' * DOOM Eternal\'s particle shading of the product\'s particle program (modprog.h): the Vulkan module, its\n' +
         ' * functions as stage text, and the OpenGL text. */\n#include "../modprog.h"\n\n' +
         wordsC('SH_MODPROG_EP_SPIRV', words) + cString('SH_MODPROG_EP_GLSL', epGlsl) + '\n' +
         cString('SH_MODPROG_EP_DECL', epDecl);
}

function renderFixture(words, epWords) {
  return '/* modprog_fixture.c -- GENERATED by tools/gen_multilayer.js from tests/fixtures/modprog/lit.frag and\n' +
         ' * particle.frag; do not edit. modprog_test\'s stand-in lit and particle programs: their SPIR-V and text. */\n\n' +
         wordsC('MODPROG_FIXTURE_SPIRV', words) + cString('MODPROG_FIXTURE_GLSL', fixture) + '\n' +
         wordsC('MODPROG_FIXTURE_EP_SPIRV', epWords) + cString('MODPROG_FIXTURE_EP_GLSL', epFixture);
}

function wordsOf(have, name) {
  const m = new RegExp(`const unsigned ${name}\\[\\] = \\{([^}]*)\\};`).exec(have);
  return m ? m[1].split(',').map((x) => x.trim()).filter(Boolean).map((x) => parseInt(x, 16)) : [];
}

// each output: its file, the modules it embeds (source text and the C name of its words), its renderer
const outputs = [[out, [[frag, 'SH_MODPROG_ML_SPIRV']], render],
  [epOut, [[epFrag, 'SH_MODPROG_EP_SPIRV']], renderParticles],
  [fixtureOut, [[fixture, 'MODPROG_FIXTURE_SPIRV'], [epFixture, 'MODPROG_FIXTURE_EP_SPIRV']], renderFixture]];
if (check) {
  for (const [file, mods, fn] of outputs) {
    const have = fs.existsSync(file) ? fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') : '';
    let want = fn(...mods.map(([, name]) => wordsOf(have, name)));
    try { want = fn(...mods.map(([text]) => compile(text))); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    if (have !== want) { console.error(`${path.basename(file)} is out of date: run node tools/gen_multilayer.js`); process.exit(1); }
  }
  console.log('modprog_multilayer.c, modprog_particles.c and modprog_fixture.c match their sources');
} else {
  for (const [file, mods, fn] of outputs) {
    const words = mods.map(([text]) => compile(text));
    fs.writeFileSync(file, fn(...words));
    console.log(`wrote ${path.relative(root, file)} (${words.map((w) => w.length).join(' + ')} SPIR-V words)`);
  }
}
