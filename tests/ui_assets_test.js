/* Bundle browser assets and exercise the explicit avatar updater without a network. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const os = require('os');
const crypto = require('crypto');
const {spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..');
const web = path.join(root, 'src/ui/webview');
const embed = path.join(root, 'src/ui/embed-page.ps1');
const updater = path.join(root, 'tools/update-avatar.ps1');
const names = ['mockup.html', 'studio.css', 'decl_language.js', 'schema_slice.js', 'prefab_transform.js', 'prefab_viewport.js'];
const hash = name => crypto.createHash('sha256').update(fs.readFileSync(path.join(web, name))).digest('hex');
const before = names.map(hash);
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'snapmap-ui-assets-'));
assert.equal(path.dirname(path.resolve(temporary)), path.resolve(os.tmpdir()));
function run(script, args) {
  return spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, ...args], {encoding:'utf8'});
}
function fixture() {
  for (const name of names) fs.copyFileSync(path.join(web, name), path.join(temporary, name));
}
function bundle() { return run(embed, ['-PageDirectory', temporary, '-Output', path.join(temporary, 'embedded.html')]); }
try {
  fixture();
  let result = bundle();
  assert.equal(result.status, 0, result.stderr);
  const embedded = fs.readFileSync(path.join(temporary, 'embedded.html'), 'utf8');
  assert(!/<script\b[^>]*\bsrc=|<link\b[^>]*stylesheet/.test(embedded));
  assert(embedded.includes(fs.readFileSync(path.join(web, 'studio.css'), 'utf8')));
  assert(embedded.includes(fs.readFileSync(path.join(web, 'decl_language.js'), 'utf8')));
  assert(embedded.indexOf('var SnapmapDecl') < embedded.indexOf('var declLanguage = SnapmapDecl.create'));
  for (const match of embedded.matchAll(/<script>([\s\S]*?)<\/script>/g)) new Function(match[1]);
  fs.unlinkSync(path.join(temporary, 'decl_language.js'));
  assert.notEqual(bundle().status, 0, 'missing module must fail');
  fixture();
  fs.appendFileSync(path.join(temporary, 'mockup.html'), '<script src="decl_language.js"></script>');
  assert.notEqual(bundle().status, 0, 'duplicate module tag must fail');
  fixture();
  fs.appendFileSync(path.join(temporary, 'studio.css'), '\n</style>');
  assert.notEqual(bundle().status, 0, 'closing style tag must fail');
  fixture();
  fs.appendFileSync(path.join(temporary, 'decl_language.js'), '\n/* )SNAPMAPPLUS */');
  assert.notEqual(bundle().status, 0, 'raw-literal delimiter must fail');
  fixture();
  const page = path.join(temporary, 'mockup.html');
  const original = fs.readFileSync(page, 'utf8');
  const avatar = original.match(/data:image\/(?:jpeg|png);base64,([A-Za-z0-9+/=]+)/);
  assert(avatar);
  const image = path.join(temporary, 'avatar.bin');
  fs.writeFileSync(image, Buffer.from(avatar[1], 'base64'));
  result = run(updater, ['-ImagePath', image, '-PagePath', page]);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(page, 'utf8'), original, 'same avatar must leave source unchanged');
  fs.writeFileSync(image, 'not an image');
  assert.notEqual(run(updater, ['-ImagePath', image, '-PagePath', page]).status, 0);
  assert.equal(fs.readFileSync(page, 'utf8'), original, 'invalid avatar must leave source unchanged');
  const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aSAAAAABJRU5ErkJggg==', 'base64');
  fs.writeFileSync(image, png);
  result = run(updater, ['-ImagePath', image, '-PagePath', page]);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(page, 'utf8'), original.replace(avatar[0], 'data:image/png;base64,' + png.toString('base64')));
  assert.deepEqual(names.map(hash), before, 'assembly and updater tests must not alter product sources');
  const build = fs.readFileSync(path.join(root, 'src/ui/build.ps1'), 'utf8');
  assert(!/avatar|doom-snapmap\.png|curl\.exe/.test(build));
  console.log('ui_assets_test: assembly, refusal cases, source preservation and offline avatar update passed');
} finally {
  fs.rmSync(temporary, {recursive:true, force:true});
}
