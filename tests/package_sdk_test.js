const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'smp-sdk-package-'));
const mod = path.join(work, 'mods', 'lamp');
const effect = path.join(mod, 'effects', 'lamp.decl');
const manifest = path.join(mod, '.build', 'manifest.json');
const game = process.argv.slice(2).find(value => value.startsWith('doom=')) || 'doom=' + work;

function helper(name, args, status) {
  const result = spawnSync(path.join(__dirname, 'obj', name + '.exe'), [...args, game],
    { cwd: work, encoding: 'utf8' });
  assert.equal(result.status, status, result.error || result.stdout + result.stderr);
  return result.stdout + result.stderr;
}

function unit(folder, extra = [], status = 0) {
  return helper('unit_cli', ['mod=mods/lamp', 'out=' + folder, 'names=*', ...extra], status);
}

function output(folder) {
  const dir = path.join(work, folder, 'units');
  const names = fs.readdirSync(dir).filter(name => name.endsWith('.json'));
  assert.equal(names.length, 1);
  return fs.readFileSync(path.join(dir, names[0]));
}

const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

try {
  fs.mkdirSync(path.dirname(effect), { recursive: true });
  fs.writeFileSync(path.join(mod, 'mod.json'), JSON.stringify({ name: 'Lamp', 'snapmap-plus': { format: 2, salt: 'sdktest0' } }));
  const source = '{\n type effect\n light {\n colour { 1, 0.45, 0.1 }\n brightness pulse { rate 3, low 2, high 6 }\n reach 300\n }\n}\n';
  fs.writeFileSync(effect, source);
  unit('built');
  const canonical = output('built');
  const published = JSON.parse(canonical);
  const generated = published.game.filter(row => row.source === 'snapmapplus');
  assert(generated.some(row => row.path.startsWith('generated/decls/table/snapmapplus_sdk_')));
  assert(generated.some(row => row.path === 'generated/decls/entitydef/snapmapplus/effect/mods/sdktest0/lamp.decl'));

  const saved = fs.readFileSync(manifest);
  unit('loaded', ['load=1']);
  assert.deepEqual(output('loaded'), canonical);
  assert.deepEqual(fs.readFileSync(manifest), saved);
  helper('served_cli', ['root=mods', 'out=served.tsv'], 0);
  const rows = new Map(fs.readFileSync(path.join(work, 'served.tsv'), 'utf8').trim().split(/\r?\n/).map(line => line.split('\t')));
  for (const row of generated) assert.equal(rows.get(row.path), row.sha256, row.path);
  const servedEffect = rows.get('generated/decls/fx/mods/sdktest0/lamp.decl');
  assert.match(servedEffect, /^[0-9a-f]{64}$/);
  assert.notEqual(servedEffect, hash(source));

  function refuse(label) {
    const prior = fs.readFileSync(manifest);
    unit(label, ['load=1'], 1);
    const units = path.join(work, label, 'units');
    assert.deepEqual(fs.readdirSync(units), []);
    assert.deepEqual(fs.readFileSync(manifest), prior);
  }
  fs.writeFileSync(effect, source.replace('low 2', 'low 3'));
  refuse('edited');
  assert.equal(fs.readFileSync(effect, 'utf8'), source.replace('low 2', 'low 3'));
  fs.unlinkSync(effect);
  refuse('missing');
  assert(!fs.existsSync(effect));
  fs.writeFileSync(effect, source);
  const altered = JSON.parse(saved);
  const ptr = altered.stock.find(row => row.source === generated[0].path);
  ptr.sha256 = '0'.repeat(64);
  fs.writeFileSync(manifest, JSON.stringify(altered));
  refuse('altered-pointer');
  const moved = JSON.parse(saved);
  moved.stock.find(row => row.source === generated[0].path).source = generated[0].path.replace('sdktest0/lamp', 'sdktest0/other');
  assert.notDeepEqual(moved, JSON.parse(saved));
  fs.writeFileSync(manifest, JSON.stringify(moved));
  refuse('moved-pointer');

  fs.copyFileSync(path.join(__dirname, 'fixtures/mods/sdk-bad/effects/blast.decl'), effect);
  assert.match(unit('invalid', [], 1), /brigthness/);
  helper('served_cli', ['root=mods', 'out=invalid.tsv'], 1);
  assert.match(fs.readFileSync(path.join(work, 'invalid.tsv'), 'utf8'), /brigthness/);
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
