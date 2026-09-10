/* Numeric-order regression checks for indexed fields in the Entity State editor.
 *
 * DOOM's canonical declaration text is keyed lexicographically. Once a list reaches
 * four digits that produces item[0], item[1000], ..., item[100], ..., item[1].
 * The editor normalizes those contiguous sibling runs for presentation without
 * changing any bytes outside the reordered entry bodies.
 */
'use strict';
const naturalize = require('../src/ui/webview/decl_language.js').create(null).naturalize;

let failures = 0;
function check(condition, message) {
  if (condition) return;
  failures++;
  console.error('FAIL: ' + message);
}

const boundary = [
  'events = {',
  '  num = 4;',
  '  item[1000] = { label = "thousand"; }',
  '  item[1001] = { label = "thousand-one"; }',
  '  item[998] = { label = "nine-nine-eight"; }',
  '  item[999] = { label = "nine-nine-nine"; }',
  '}'
].join('\n');
const boundaryExpected = [
  'events = {',
  '  num = 4;',
  '  item[998] = { label = "nine-nine-eight"; }',
  '  item[999] = { label = "nine-nine-nine"; }',
  '  item[1000] = { label = "thousand"; }',
  '  item[1001] = { label = "thousand-one"; }',
  '}'
].join('\n');
const boundaryActual = naturalize(boundary);
check(boundaryActual === boundaryExpected, 'the 999-to-1000 boundary was not displayed numerically');
check(boundaryActual.length === boundary.length, 'boundary normalization changed the declaration length');
check(naturalize(boundaryActual) === boundaryActual, 'normalization is not idempotent');

const nested = [
  'events = {',
  '  item[1] = {',
  '    args = {',
  '      item[10] = "ten";',
  '      item[2] = "two";',
  '    }',
  '  }',
  '  item[0] = { label = "zero"; }',
  '}'
].join('\n');
const nestedActual = naturalize(nested);
check(nestedActual.indexOf('item[0]') < nestedActual.indexOf('item[1]'),
  'outer indexed siblings were not sorted around a nested list');
check(nestedActual.indexOf('item[2]') < nestedActual.indexOf('item[10]'),
  'nested indexed siblings were not sorted');
check(nestedActual.length === nested.length, 'nested normalization changed the declaration length');

const quoted = [
  'edit = {',
  '  note = "item[7000] = { not syntax };";',
  '  "item[2]" = 2; // this comment stays in its separator slot',
  '  "item[1]" = 1;',
  '}'
].join('\n');
const quotedActual = naturalize(quoted);
check(quotedActual.indexOf('"item[1]"') < quotedActual.indexOf('"item[2]"'),
  'quoted indexed keys were not sorted');
check(quotedActual.indexOf('"item[7000] = { not syntax };"') >= 0,
  'indexed-looking text inside a string was changed');
check(quotedActual.length === quoted.length, 'quoted-key normalization changed the declaration length');

const count = 1500;
const lexical = Array.from({length: count}, (_, i) => i).sort((a, b) => {
  const aa = 'item[' + a + ']', bb = 'item[' + b + ']';
  return aa < bb ? -1 : aa > bb ? 1 : 0;
});
const padding = 'x'.repeat(64);
const large = 'events = {\n  num = ' + count + ';\n' +
  lexical.map(i => '  item[' + i + '] = { label = "' + padding + '"; }').join('\n') +
  '\n}';
check(large.length > 65535, 'large regression fixture did not cross the former 64 KiB limit');
const largeActual = naturalize(large);
const seen = Array.from(largeActual.matchAll(/^[ \t]*item\[([0-9]+)\]/gm), m => Number(m[1]));
check(seen.length === count, 'large declaration lost indexed entries');
check(seen.every((value, index) => value === index),
  'large lexicographic declaration was not normalized to complete numeric order');
check(largeActual.length === large.length, 'large normalization changed the declaration length');

if (failures) process.exit(1);
console.log('decl indexed-order tests passed');
