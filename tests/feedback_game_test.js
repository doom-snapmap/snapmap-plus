/* Tests the relay's game-identity handling: only the identity strings of known
 * builds (13954591, and 2642713 for legacy reports) are accepted, the issue body
 * always carries a "- Game:" line, and an unrecognized value is never echoed.
 * Extract the functions from the Worker source without its runtime imports. */
'use strict';
const fs = require('fs');
const path = require('path');

const WORKER = path.join(__dirname, '..', 'feedback', 'worker.js');
const src = fs.readFileSync(WORKER, 'utf8').replace(/\r\n/g, '\n');

function extract(startMarker, endMarker) {
  const begin = src.indexOf(startMarker);
  if (begin < 0) throw new Error('could not find ' + JSON.stringify(startMarker) + ' in worker.js');
  const end = src.indexOf(endMarker, begin);
  if (end < 0) throw new Error('could not find the end of ' + JSON.stringify(startMarker));
  return src.slice(begin, end + endMarker.length);
}

const sandbox = {};
new Function('exports', [
  extract('const RENDERERS = {', '};'),
  extract('const GAME_BUILDS = {', '\n};'),
  extract('const GAME_PATTERN =', ';\n'),
  extract('function gameOf(game) {', '\n}'),
  extract('function gameLine(game) {', '\n}'),
  extract('function issueBody(', '\n}'),
  extract('function commentBody(', '\n}'),
  'exports.gameOf = gameOf; exports.issueBody = issueBody; exports.commentBody = commentBody;',
].join('\n'))(sandbox);
const { gameOf, issueBody, commentBody } = sandbox;

let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}

const VK = 'DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)';
const GL = 'DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, OpenGL, 1bddecaa)';
const OLD = 'DOOM 2016 build 2642713 (20180321-154133-liver-goldenrod, Vulkan, 139763e9)';

/* what the backend sends */
let g = gameOf(VK);
check(g && g.build === '13954591' && g.text === VK, 'Vulkan identity accepted: ' + JSON.stringify(g));
g = gameOf(GL);
check(g && g.build === '13954591' && g.text === GL, 'OpenGL identity accepted: ' + JSON.stringify(g));
g = gameOf(OLD);
check(g && g.build === '2642713', 'legacy 2018 identity accepted: ' + JSON.stringify(g));
check(gameOf('  ' + VK + '  ') && gameOf('  ' + VK + '  ').text === VK, 'surrounding whitespace is trimmed');

/* absent -> null ("not attached") */
check(gameOf('') === null, 'empty is not attached');
check(gameOf(undefined) === null, 'missing is not attached');
check(gameOf('not attached') === null, 'the literal not-attached value stays not attached');

/* present but unknown or forged -> false, never echoed */
const bad = [
  'DOOM 2016 build 99999999 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)',   // unknown build
  'DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 1bddecaa)',   // GL hash on Vulkan
  'DOOM 2016 build 13954591 (20180321-154133-liver-goldenrod, Vulkan, 3d8b4393)',  // wrong binary
  'DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, DirectX, 3d8b4393)',  // unknown renderer
  'DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)\n- Contact: x',
  'DOOM 2016 build __proto__ (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)',
  'DOOM 2016 build constructor (x, Vulkan, 3d8b4393)',
  '<script>alert(1)</script>',
  'Vulkan',
];
for (const b of bad) check(gameOf(b) === false, 'rejected: ' + JSON.stringify(b));

/* issue body: always a Game line, after Version and Renderer */
const body = issueBody('it broke', 'v0.2.2', 'beta', '', 'vulkan', 'abc123', gameOf(VK));
check(body.indexOf('- Game: ' + VK) >= 0, 'body carries the game line: ' + body);
check(body.indexOf('- Renderer: Vulkan') < body.indexOf('- Game: '), 'Game follows Renderer');
check(body.indexOf('- Version: v0.2.2 (beta)') < body.indexOf('- Renderer: '), 'Renderer follows Version');
const unattached = issueBody('it broke', 'v0.2.2', 'beta', '', 'vulkan', 'abc123', gameOf(''));
check(unattached.indexOf('- Game: not attached') >= 0, 'absent identity says not attached: ' + unattached);
const legacyCaller = issueBody('it broke', 'v0.2.2', 'beta', '', 'vulkan', 'abc123');
check(legacyCaller.indexOf('- Game: not attached') >= 0, 'an omitted argument says not attached');
const forged = issueBody('it broke', 'v0.2.2', 'beta', '', 'vulkan', 'abc123', gameOf(bad[0]));
check(forged.indexOf('- Game: unrecognized (not shown)') >= 0, 'unrecognized identity is not echoed: ' + forged);
check(forged.indexOf('99999999') < 0, 'the forged build id never reaches the body');

/* dedup comment names this occurrence's build */
const c = commentBody('again', 'v0.2.2', 'beta', '', 'opengl', '', gameOf(GL));
check(c.indexOf('on version v0.2.2 (beta), OpenGL, build 13954591:') >= 0, 'comment names the build: ' + c);
check(c.indexOf('- Game: ' + GL) >= 0, 'comment carries the game line');
const cNone = commentBody('again', 'v0.2.2', 'beta', '', 'opengl', '', null);
check(cNone.indexOf('on version v0.2.2 (beta), OpenGL:') >= 0 && cNone.indexOf('- Game: not attached') >= 0,
      'comment without identity: ' + cNone);

/* the label the relay adds is build-<id> for an accepted identity only */
const labelSrc = extract("if (game) labels.push(", ";");
check(labelSrc.indexOf("'build-' + game.build") >= 0, 'label is build-<id>: ' + labelSrc);

if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
console.log('feedback game identity: ok');
