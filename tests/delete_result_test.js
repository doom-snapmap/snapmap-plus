/* Window deletion reports only what the backend returned. Logic is lifted from the
 * shipped HTML and run with stubs; the host and ABI sources are checked as contracts.
 * Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const read = function (rel) { return fs.readFileSync(path.join(ROOT, rel), 'utf8').replace(/\r\n/g, '\n'); };
const page = read('src/ui/webview/mockup.html');
const host = read('src/ui/webview/snapmap_plus_ui_webview.cpp');
const abi = read('src/common/snapmap_plus_iface.h');
let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}

function slice(src, startMarker, endMarker) {
  const begin = src.indexOf(startMarker);
  const end = src.indexOf(endMarker, begin + startMarker.length);
  if (begin < 0 || end < 0) throw new Error('could not find ' + startMarker);
  return src.slice(begin, end);
}

/* Status codes come from the shared ABI header, not from this test. */
const codes = {};
abi.replace(/#define (SH_DELETE_[A-Z_]+)\s+\(?(-?[0-9]+)\)?/g, function (_, name, value) {
  codes[name] = Number(value); return _;
});
['DISPATCHED', 'UNAVAILABLE', 'NOT_MAIN', 'NOT_READY', 'NOT_ENTITY', 'BUSY', 'INVALID',
 'RESTRICTED', 'PORTAL', 'PROTECTED', 'REFERENCED', 'FAULT', 'INCOMPLETE'].forEach(function (n) {
  check(typeof codes['SH_DELETE_' + n] === 'number', 'ABI header lacks SH_DELETE_' + n);
});

const logic = slice(page, '  var DELETE_STATUS = {', '\n  // ---- context menu ----');
let posts = [], toasts = [], refreshes = [], domUpdates = 0;
/* A controllable clock: any time-based expiry in the page would see it move. */
let clockMs = 1000000;
const FakeDate = { now: function () { return clockMs; } };
const api = {};
new Function('exports', 'post', 'toast', 'updateSelDom', 'refreshEditorForSelection', 'Date',
  'var selectedEids = {};\n' + logic +
  '\nexports.requestDelete = requestDelete;' +
  '\nexports.applyDeleteResult = applyDeleteResult;' +
  '\nexports.deleteResultMessage = deleteResultMessage;' +
  '\nexports.status = DELETE_STATUS; exports.holding = DELETE_BUSY_HOLDING;' +
  '\nexports.setSel = function(v){ selectedEids = v; };' +
  '\nexports.getSel = function(){ return selectedEids; };')(
  api,
  function (o) { posts.push(o); },
  function (text, level) { toasts.push({text: text, level: level}); },
  function () { domUpdates++; },
  function (push) { refreshes.push(push); },
  FakeDate);

function reset(sel) { posts = []; toasts = []; refreshes = []; domUpdates = 0; api.setSel(sel); }

/* The page's codes are the ABI's codes. */
Object.keys(api.status).forEach(function (n) {
  check(api.status[n] === codes['SH_DELETE_' + n], 'page code ' + n + ' differs from the ABI header');
});
check(Object.keys(api.status).length === 13, 'page does not mirror every SH_DELETE_* status');
check(api.holding === codes.SH_DELETE_BUSY_MANIPULATION, 'page busy-holding detail differs from the ABI header');

/* A request changes nothing locally and announces nothing. */
reset({3: true, 4: true});
api.requestDelete([3, 4]);
check(posts.length === 1 && posts[0].cmd === 'delete' && JSON.stringify(posts[0].eids) === '[3,4]',
  'delete request was not posted exactly once');
check(toasts.length === 0, 'a toast was shown before the backend answered');
check(api.getSel()[3] && api.getSel()[4], 'selection changed before the backend answered');
api.requestDelete([3]);
check(posts.length === 1, 'a second delete was posted while one was in flight');
check(toasts.length === 1 && toasts[0].level === 'warn', 'the refused duplicate was not explained');

/* Success drops exactly the requested IDs and never pushes a selection back. */
api.applyDeleteResult({status: codes.SH_DELETE_DISPATCHED, released: 2, targets: 2});
check(!api.getSel()[3] && !api.getSel()[4], 'deleted entities stayed selected in the list');
check(refreshes.length === 1 && refreshes[0] === false, 'success pushed a selection to the editor');
check(toasts.length === 2 && toasts[1].level === 'ok' && /2 entities/.test(toasts[1].text),
  'success toast did not report the released count');
api.requestDelete([5]);
check(posts.length === 2, 'a finished request blocked the next one');
api.applyDeleteResult({status: codes.SH_DELETE_BUSY, busy: codes.SH_DELETE_BUSY_PHASE});
check(/busy finishing/.test(toasts[toasts.length - 1].text), 'a pending native action was not explained');
reset({});
api.requestDelete([6]);
api.applyDeleteResult({status: codes.SH_DELETE_BUSY, busy: codes.SH_DELETE_BUSY_MANIPULATION});
check(/holding/.test(toasts[0].text), 'a grab refusal was not explained');

/* Every refusal keeps the selection and explains itself. */
['BUSY', 'INVALID', 'RESTRICTED', 'PORTAL', 'PROTECTED', 'REFERENCED', 'NOT_READY', 'NOT_ENTITY',
 'NOT_MAIN', 'UNAVAILABLE', 'FAULT'].forEach(function (n) {
  reset({7: true});
  api.requestDelete([7]);
  api.applyDeleteResult({status: codes['SH_DELETE_' + n], released: 0, targets: 1});
  check(api.getSel()[7] === true, n + ' refusal dropped the selection');
  check(refreshes.length === 0, n + ' refusal pushed a selection');
  check(toasts.length === 1 && toasts[0].level !== 'ok', n + ' refusal was announced as success');
});
const specific = ['BUSY', 'INVALID', 'RESTRICTED', 'PORTAL', 'PROTECTED', 'REFERENCED', 'NOT_ENTITY'];
const texts = specific.map(function (n) { return api.deleteResultMessage({status: codes['SH_DELETE_' + n]}).text; });
check(new Set(texts).size === texts.length, 'refusal reasons share a message');
check(/did not respond/.test(api.deleteResultMessage({status: 'timeout'}).text),
  'a request the game never ran is not explained');

/* A partial native result keeps the rest for the list refresh to resolve. */
reset({8: true, 9: true});
api.requestDelete([8, 9]);
api.applyDeleteResult({status: codes.SH_DELETE_INCOMPLETE, released: 1, targets: 2});
check(api.getSel()[8] && api.getSel()[9], 'partial result cleared the selection itself');
check(toasts.length === 1 && toasts[0].level === 'err' && /1 of 2/.test(toasts[0].text),
  'partial result was not reported as such');

/* One request stays outstanding until its own reply, however late. A delayed
 * reply must apply to the IDs that were actually sent, never to a newer list. */
reset({10: true, 11: true, 12: true});
api.requestDelete([10, 11]);
check(posts.length === 1, 'first request was not posted');
clockMs += 60 * 60 * 1000;                     /* an hour without a reply */
api.requestDelete([12]);
check(posts.length === 1, 'a second request was posted while the first had no reply');
check(toasts.length === 1 && toasts[0].level === 'warn' && /still running/.test(toasts[0].text),
  'the refused second request was not explained');
api.applyDeleteResult({status: codes.SH_DELETE_DISPATCHED, released: 2, targets: 2});
check(!api.getSel()[10] && !api.getSel()[11], 'the late reply did not drop its own IDs');
check(api.getSel()[12] === true, 'the late reply cleared an ID that was never sent');
check(toasts.length === 2 && /2 entities/.test(toasts[1].text), 'the late reply was not reported');
/* A reply with nothing outstanding (stale or duplicated) changes nothing. */
api.applyDeleteResult({status: codes.SH_DELETE_DISPATCHED, released: 1, targets: 1});
check(api.getSel()[12] === true && toasts.length === 2 && refreshes.length === 1,
  'a reply with no outstanding request changed the page');
api.requestDelete([12]);
check(posts.length === 2 && JSON.stringify(posts[1].eids) === '[12]',
  'the next request was blocked after the late reply');
api.applyDeleteResult({status: codes.SH_DELETE_PROTECTED});
check(api.getSel()[12] === true, 'a refusal after a late reply dropped the selection');
/* The host's never-started reply also releases the request. */
reset({13: true});
api.requestDelete([13]);
api.applyDeleteResult({status: 'timeout'});
check(api.getSel()[13] === true && /did not respond/.test(toasts[0].text), 'a timeout reply was mishandled');
api.requestDelete([13]);
check(posts.length === 2, 'a timeout reply did not release the request');
api.applyDeleteResult({status: codes.SH_DELETE_BUSY, busy: codes.SH_DELETE_BUSY_PHASE});

/* The context menu only requests; it no longer edits selection or toasts itself. */
const menu = slice(page, "    addCtx('Delete'", "    addCtx('Push to stack 0'");
check(/requestDelete\(ids\)/.test(menu), 'context-menu Delete does not use requestDelete');
check(!/toast\(/.test(menu) && !/delete selectedEids/.test(menu),
  'context-menu Delete still edits selection or announces success');
check(/d\.kind === 'deleteResult'\) \{\s*applyDeleteResult\(d\);/.test(page),
  'page does not route deleteResult messages');

/* Host: one guarded batch call, never the per-ID slot, and a result after the main-thread call. */
const apply = slice(host, 'static void poc_apply_deletes()', '\n}\n');
check(/SH_IFACE_HAS_SLOT\(g_iface, delete_entities\)/.test(apply), 'host calls ext 29 without the vtable size guard');
check(!/selection_guard/.test(apply), 'host still deletes through the per-ID slot');
check(/kind\\":\\"deleteResult\\"/.test(host), 'host never posts deleteResult');
const loop = slice(host, 'static void poc_think_loop()', 'extern "C" __declspec(dllexport)');
const call = loop.indexOf('poc_on_main(poc_main_edits');
const result = loop.indexOf('poc_post_delete_result(');
check(call >= 0 && result > call, 'deleteResult is not posted after the main-thread call returns');

if (failures) { console.error('delete_result_test: ' + failures + ' failure(s)'); process.exit(1); }
console.log('delete_result_test: OK');
