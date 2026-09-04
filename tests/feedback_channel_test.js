/* The feedback relay's release-channel classifier, lifted from the deployed Worker. Pure ASCII.
 *
 * The in-app dialog reports the release TAG, which carries a leading "v" -- install.json holds
 * "v0.2.1-beta.7", not "0.2.1-beta.7". channelOf's pattern used to demand a bare x.y.z, so it
 * returned null for every real report: no issue was ever labelled beta or stable, and
 * issues-retest.yml, which only prompts beta-labelled reports when the new release is a
 * pre-release, prompted nothing for four consecutive betas.
 *
 * worker.js is an ES module that runs on Cloudflare, so this extracts the one function from the
 * source rather than importing it, the same way the decl editor tests read mockup.html.
 */
'use strict';
const fs = require('fs');
const path = require('path');

const WORKER = path.join(__dirname, '..', 'feedback', 'worker.js');
const src = fs.readFileSync(WORKER, 'utf8').replace(/\r\n/g, '\n');

const begin = src.indexOf('function channelOf(version) {');
if (begin < 0) throw new Error('could not find channelOf in worker.js');
const end = src.indexOf('\n}', begin);
if (end < 0) throw new Error('could not find the end of channelOf');
const sandbox = {};
new Function('exports', src.slice(begin, end + 2) + '\nexports.channelOf = channelOf;')(sandbox);
const channelOf = sandbox.channelOf;

let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}
function expect(version, want) {
  const got = channelOf(version);
  check(got === want, 'channelOf(' + JSON.stringify(version) + ') is ' + JSON.stringify(got) +
    ', expected ' + JSON.stringify(want));
}

/* what the app actually sends: the tag out of install.json */
expect('v0.2.1-beta.7', 'beta');
expect('v0.2.1-beta.3', 'beta');
expect('v0.2.0', 'stable');

/* the bare form stays accepted -- older clients and hand-typed reports */
expect('0.2.1-beta.7', 'beta');
expect('0.2.0', 'stable');

/* a dev build still gets no channel, which is the point of the check */
expect('unknown', null);
expect('', null);
expect('dev', null);
expect('beta', null);
expect('vbeta', null);
expect('version 1', null);

if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
console.log('feedback channel: ok');
