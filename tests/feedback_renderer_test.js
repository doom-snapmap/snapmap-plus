/* The feedback relay's renderer classifier + the two body renderers that consume it, lifted from the
 * deployed Worker. Pure ASCII.
 *
 * DOOM 2016 ships one executable per renderer and relaunches itself when r_renderAPI changes, so a
 * report has to say which one the player was in -- an issue that only reproduces under OpenGL is
 * otherwise indistinguishable on the tracker from one that happens under both. The relay must not
 * echo whatever the POST contained into a public issue, so an unrecognized token has to degrade to
 * "unknown" (no line, no label) rather than reach the tracker.
 *
 * worker.js is an ES module that runs on Cloudflare, so this extracts the pieces from the source
 * rather than importing it, the same way feedback_channel_test.js does.
 */
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
  extract('function rendererOf(renderer) {', '\n}'),
  extract('function issueBody(', '\n}'),
  extract('function commentBody(', '\n}'),
  'exports.rendererOf = rendererOf; exports.issueBody = issueBody; exports.commentBody = commentBody;',
].join('\n'))(sandbox);
const rendererOf = sandbox.rendererOf, issueBody = sandbox.issueBody, commentBody = sandbox.commentBody;

let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}
function expect(renderer, want) {
  const got = rendererOf(renderer);
  check(got === want, 'rendererOf(' + JSON.stringify(renderer) + ') is ' + JSON.stringify(got) +
    ', expected ' + JSON.stringify(want));
}

/* what the app actually sends */
expect('vulkan', 'vulkan');
expect('opengl', 'opengl');

/* unknown, absent, or hostile -> null: nothing lands in the issue body or the label set */
expect('', null);
expect('unknown', null);
expect('directx', null);
expect('Vulkan', null);                 /* the relay lowercases before classifying */
expect('__proto__', null);              /* an inherited property is not a renderer */
expect('constructor', null);
expect('toString', null);

/* the body renderers spell it out for a human, and omit the line entirely when it is unknown */
const withVk = issueBody('it broke', 'v0.2.1', 'beta', '', 'vulkan', 'abc123');
check(withVk.indexOf('- Renderer: Vulkan') >= 0, 'issueBody should carry the renderer line: ' + withVk);
check(withVk.indexOf('- Version: v0.2.1 (beta)') >= 0, 'issueBody should still carry the version line');
check(withVk.indexOf('report-sig:abc123') >= 0, 'issueBody should still carry the dedup signature');

const withGl = issueBody('it broke', 'v0.2.1', 'beta', 'me@example.com', 'opengl', 'abc123');
check(withGl.indexOf('- Renderer: OpenGL') >= 0, 'issueBody should name OpenGL: ' + withGl);
check(withGl.indexOf('- Contact: me@example.com') >= 0, 'issueBody should still carry the contact line');

const none = issueBody('it broke', 'dev', null, '', null, 'abc123');
check(none.indexOf('Renderer') < 0, 'issueBody should omit the renderer line when unknown: ' + none);

/* the dedup comment names the renderer of THIS occurrence -- the signature is category+title only,
 * so the same crash under both renderers stays one issue whose comments show it hits both */
const c = commentBody('again', 'v0.2.1', 'beta', '', 'opengl', '');
check(c.indexOf('on version v0.2.1 (beta), OpenGL:') >= 0, 'commentBody should name the renderer: ' + c);
const cNone = commentBody('again', 'dev', null, '', null, '');
check(cNone.indexOf('on version dev:') >= 0, 'commentBody should read cleanly with no renderer: ' + cNone);

if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
console.log('feedback renderer: ok');
