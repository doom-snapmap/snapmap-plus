/* Tests the release-retest workflow's version and game-build rules by
 * extracting its helper functions from the workflow script: a release after
 * v0.2.1-beta.19 prompts only reports from build 13954591, and a report with
 * no "- Game:" line from v0.2.1-beta.19 or earlier counts as the 2018 build. */
'use strict';
const fs = require('fs');
const path = require('path');

const WORKFLOW = path.join(__dirname, '..', '.github', 'workflows', 'issues-retest.yml');
const src = fs.readFileSync(WORKFLOW, 'utf8').replace(/\r\n/g, '\n');
const begin = src.indexOf('function parseV(v) {');
const end = src.indexOf('const issues = await');
if (begin < 0 || end < 0) throw new Error('could not find the helper block in issues-retest.yml');
const sandbox = {};
new Function('exports', src.slice(begin, end) +
  '\nexports.lt = lt; exports.reportBuild = reportBuild; exports.releaseSupports = releaseSupports;')(sandbox);
const { lt, reportBuild, releaseSupports } = sandbox;

let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }

const game = id => '- Version: v0.2.2 (beta)\n- Renderer: Vulkan\n- Game: DOOM 2016 build ' + id +
  ' (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)\n';

check(lt('0.2.1-beta.19', '0.2.2-beta.1') && !lt('0.2.1-beta.19', '0.2.1-beta.19'), 'version ordering');

check(reportBuild(game('13954591'), 'v0.2.2-beta.1') === '13954591', 'Game line names the build');
check(reportBuild('- Version: v0.2.2\n- Game: not attached\n', 'v0.2.2') === null, 'not attached is unknown');
check(reportBuild('- Version: v0.2.2\n- Game: unrecognized (not shown)\n', 'v0.2.2') === null, 'unrecognized is unknown');
check(reportBuild('- Version: 0.2.1-beta.19 (beta)\n', '0.2.1-beta.19') === '2642713', 'pre-Game report on beta.19 is 2018');
check(reportBuild('- Version: 0.2.0\n', '0.2.0') === '2642713', 'older pre-Game report is 2018');
check(reportBuild('- Version: 0.2.2\n', '0.2.2') === null, 'a newer report without the line is unknown');

check(releaseSupports('0.2.2-beta.1', '13954591'), 'new release prompts 13954591 reports');
check(!releaseSupports('0.2.2-beta.1', '2642713'), 'new release skips 2018 reports');
check(!releaseSupports('0.2.2-beta.1', null), 'new release skips reports of unknown build');
check(releaseSupports('0.2.1-beta.19', '2642713'), 'beta.19 itself still serves 2018 reports');
check(!releaseSupports('0.2.1-beta.19', '13954591'), 'beta.19 does not run on 13954591');

if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
console.log('issues retest: ok');
