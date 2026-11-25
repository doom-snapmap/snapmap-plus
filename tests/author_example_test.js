const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const guide = fs.readFileSync(path.join(__dirname, '../docs/authoring.md'), 'utf8').replace(/\r\n/g, '\n');
const section = guide.match(/^## A small mod from an empty folder\n([\s\S]*?)(?=^## )/m)[1];
const files = [...section.matchAll(/^\d+\. `([\w./-]+)`[^\n]*(?:\n(?!\d+\. |   ```)[^\n]*)*?\n   ```[^\n]*\n([\s\S]*?)^   ```/gm)];
assert.deepEqual(files.map(file => file[1]), [
  'mod.json', 'entities/crate.decl', 'effects/crate_sparks.decl',
  'decls/snapeditorentitydef/crate.decl', 'scripts/crate.lua',
]);

const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'smp-author-example-'));
try {
  for (const [,name,body] of files) {
    const dest = path.join(folder, name);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.writeFileSync(dest, body.replace(/^   /gm, ''));
  }
  execFileSync(path.join(__dirname, 'obj/modlook_mod_test.exe'), ['--author-example', folder, ...process.argv.slice(2)], { stdio: 'inherit' });
} finally {
  fs.rmSync(folder, { recursive: true, force: true });
}
