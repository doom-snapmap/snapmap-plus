/* Parser, schema, highlighting and completion work without a document or host. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const modulePath = path.join(__dirname, '../src/ui/webview/decl_language.js');
const languageModule = require(modulePath);
const schema = {
  structs: {
    idEntity: {f: {health: 'n:int', enabled: 'b'}},
    Actor: {p: 'idEntity', f: {mode: 'e:Mode', label: 's', nested: 'S:Position'}},
    Position: {f: {x: 'n:float'}}
  },
  enums: {Mode: ['ONE', 'TWO']}
};
const language = languageModule.create(schema);
function check(text, className) {
  const tokens = language.tokenize(text);
  const parsed = language.parse(tokens);
  const mode = language.checkSchema(parsed.rootEntries, className, parsed.diags);
  return {tokens, parsed, mode};
}
const valid = check('edit = { health = 3; enabled = true; mode = "ONE"; nested = { x = 1.5; } }', 'Actor');
assert.equal(valid.mode, 'exact');
assert.equal(valid.parsed.diags.length, 0);
assert(check('edit = { mode = "INVALID"; }', 'Actor').parsed.diags.some(d => d.cat === 'enum-value'));
assert(check('edit = { health = "many"; }', 'Actor').parsed.diags.some(d => d.cat === 'type'));
assert.equal(check('edit = { health = 4; unknown = 1; }', 'Unlisted').mode, 'base');
assert.equal(check('edit = { health = 4; unknown = 1; }', 'Unlisted').parsed.diags.length, 0);
assert(check('edit = { health = ', 'Actor').parsed.diags.length > 0);
function complete(text, forced = false, instance = language) {
  return instance.complete(instance.tokenize(text), text.length, 'Actor', forced);
}
assert(complete('edit = { hea').items.some(item => item.label === 'health'));
assert(complete('edit = { enabled = t').items.some(item => item.label === 'true'));
assert(complete('edit = { mode = ', true).items.some(item => item.label === '"TWO"'));
assert(complete('edit = { nested = { ', true).items.some(item => item.label === 'x'));
assert.equal(complete('edit = { ', false).items.length, 0);
const other = languageModule.create({structs: {Actor: {f: {different: 'b'}}}, enums: {}});
assert.deepEqual(complete('edit = { ', true, other).items.map(item => item.label), ['different']);
assert(!complete('edit = { ', true).items.some(item => item.label === 'different'));
const structural = languageModule.create(null);
const parsed = structural.parse(structural.tokenize('edit = { health = 3; }'));
assert.equal(structural.checkSchema(parsed.rootEntries, 'Actor', parsed.diags), false);
assert.equal(parsed.diags.length, 0);
assert.equal(complete('edit = { ', true, structural).items.length, 0);
const browser = {};
vm.runInNewContext(fs.readFileSync(modulePath, 'utf8'), browser);
assert.equal(typeof browser.SnapmapDecl.create, 'function');
assert.equal(JSON.stringify(browser.SnapmapDecl.create(null).tokenize('x = 3;')),
             JSON.stringify(structural.tokenize('x = 3;')));
console.log('decl_language_test: parser, schema isolation, completion and browser export passed');
