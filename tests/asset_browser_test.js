/* Asset-browser logic lifted from the shipped HTML: pins, trees, and carrier gates. */
'use strict';
const fs = require('fs');
const path = require('path');

const HTML = path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html');
const src = fs.readFileSync(HTML, 'utf8').replace(/\r\n/g, '\n');

function grab(startRe, endMarker) {
  const i = src.search(startRe);
  if (i < 0) throw new Error('could not find ' + startRe + ' in mockup.html');
  const j = src.indexOf(endMarker, i);
  if (j < 0) throw new Error('could not find end marker after ' + startRe);
  return src.slice(i, j + endMarker.length);
}

const pinBasics = grab(/function abPinKey\(name\)/,
  "function abIsPinned(name) { return !!abPinIx[abPinKey(name)]; }");
const pinRebuild = grab(/function abPinsRebuild\(\)/, "\n  }");
const pinLoad = grab(/function abPinsLoad\(doc\)/, "\n  }");
const bankFns = grab(/function abBankOf\(name\)/, "\n  }") + '\n' +
                grab(/function abBankPlace\(name\)/, "\n");
const buildTree = grab(/function abBuildTree\(names, place\)/, "\n    return root;\n  }");
const buildFlat = grab(/function abBuildFlat\(names\)/, "\n  }");

const assetKinds = grab(/var AB_KIND_VTONLY =/, "\n  ];");
const catalogCache = grab(/var abCounts = \{\};/, "\n  }") + '\n' +
                     grab(/function abCatalogResident\(type\)/, "\n  }");
const requestKind = grab(/var abPendingKinds = \{\};/, "\n  }");
const fetchOne = grab(/function abFetch\(m\)/, "\n  }");
const openCatalog = grab(/function abOpenCatalog\(m\)/, "\n  }");
const receiveCatalog = grab(/function onAssetList\(d\)/, "\n  }");
const forgetPreview = grab(/function abForgetPreview\(m\)/, "\n  }");
const previewKind = grab(/function abPreviewKind\(m\)/, "\n  }");
const requestPreview = grab(/function abRequestPreview\(m, path\)/, "\n  }");

const renderTarget = grab(/function abRenderTargetOk\(cls\)/, "\n  }");
const noApply = grab(/function abNoApplyEver\(cls\)/, "\n");
const modelTarget = grab(/var AB_MODEL_DENY =/, "\n  }");
const singlePurpose = grab(/function abIsSpeaker\(cls\)/, "\n  }");
const applyDenied = grab(/function abApplyDenied\(carrier, cls\)/, "\n  }");

const sandbox = {};
new Function('exports', `
  var abNames = {}, abTrees = {}, abPins = [], abPinIx = {}, abMounts = [];
  var abSndBank = null, AB_NO_BANK = '(no bank)';
  function abRenderRail() {} function abRenderTree() {}
  ${pinBasics}
  ${pinRebuild}
  ${pinLoad}
  ${bankFns}
  ${buildTree}
  ${buildFlat}
  ${renderTarget}
  ${noApply}
  ${modelTarget}
  ${singlePurpose}
  ${applyDenied}
  exports.loadPins = abPinsLoad;
  exports.pinState = function () { return {pins: abPins, names: abNames.pinned, index: abPinIx}; };
  exports.setBanks = function (value) { abSndBank = value; };
  exports.abBankPlace = abBankPlace;
  exports.abBuildTree = abBuildTree;
  exports.abBuildFlat = abBuildFlat;
  exports.abRenderTargetOk = abRenderTargetOk;
  exports.abNoApplyEver = abNoApplyEver;
  exports.abModelTargetOk = abModelTargetOk;
  exports.abIsSpeaker = abIsSpeaker;
  exports.abIsLight = abIsLight;
  exports.abIsEmitter = abIsEmitter;
  exports.abIsFxEntity = abIsFxEntity;
  exports.abApplyDenied = abApplyDenied;
`)(sandbox);

const fetchSandbox = {};
new Function('exports', `
  ${assetKinds}
  var abNames = {}, abTrees = {}, abMounts = [];
  ${catalogCache}
  var abVtOnly = null, abSndBank = null, abPinsFetched = 0;
  var sent = [];
  function post(message) { sent.push(message); }
  function abType(id) {
    for (var i = 0; i < AB_TYPES.length; i++) if (AB_TYPES[i].id === id) return AB_TYPES[i];
    return null;
  }
  function abRenderRail() {} function abRenderTree() {} function abRenderInsp() {}
  function toast() {}
  ${requestKind}
  ${fetchOne}
  ${openCatalog}
  ${receiveCatalog}
  exports.open = abOpenCatalog;
  exports.receive = onAssetList;
  exports.drain = function () { var out = sent; sent = []; return out; };
  exports.cache = function () { return {names:abNames, counts:abCounts, lru:abCatalogLru.slice()}; };
`)(fetchSandbox);

const previewSandbox = {};
new Function('exports', `
  var sent = [];
  function post(m) { sent.push(m); }
  function abStopPoll(m) { m.stopped = true; m.pvTimer = null; m.pvTries = 0; }
  ${forgetPreview}
  exports.forget = abForgetPreview;
  exports.drain = function () { return sent.splice(0); };
`)(previewSandbox);

const previewRouteSandbox = {};
new Function('exports', `
  ${assetKinds}
  var sent = [];
  function post(m) { sent.push(m); }
  function abType(id) {
    for (var i = 0; i < AB_TYPES.length; i++) if (AB_TYPES[i].id === id) return AB_TYPES[i];
    return null;
  }
  function abSelType(m) { return m.selectedType; }
  ${previewKind}
  ${requestPreview}
  exports.request = abRequestPreview;
  exports.drain = function () { return sent.splice(0); };
`)(previewRouteSandbox);

let failures = 0;
function check(condition, message) {
  if (condition) return;
  failures++;
  console.error('FAIL: ' + message);
}

sandbox.loadPins(JSON.stringify({version: 1, pins: [
  {type: 'material', name: 'Materials/One'},
  {type: 'sound', name: 'materials/one'},
  {type: 'sound', name: 'Play_Two'},
  {type: 7, name: 'bad'},
  null
]}));
let pins = sandbox.pinState();
check(pins.pins.length === 2, 'loaded pins were not deduplicated case-insensitively');
check(pins.names.length === 2, 'pinned pseudo-type did not mirror the clean list');
check(pins.index['materials/one'] === 'material', 'the first pin did not keep its asset type');
check(pins.index['play_two'] === 'sound', 'cross-type pin identity was lost');

sandbox.loadPins('{broken');
pins = sandbox.pinState();
check(pins.pins.length === 0 && pins.names.length === 0, 'malformed pin JSON did not degrade to empty');

const flat = sandbox.abBuildFlat(['z/path', 'a/path']);
check(flat.n === 2 && flat.dirKeys.length === 0, 'Pinned was not built as a flat list');
check(flat.files[0].name === 'a/path' && flat.files[0].path === 'a/path',
  'Pinned rows did not retain and sort their full names');

sandbox.setBanks({'play_one': 'doom_snapmaps'});
const tree = sandbox.abBuildTree(['Play_One', 'folder/raw'], sandbox.abBankPlace);
check(tree.n === 2, 'sound tree count was wrong');
check(tree.dirs.doom_snapmaps.files[0].path === 'Play_One',
  'soundbank placement leaked into the real asset name');
check(tree.dirs['(no bank)'].dirs.folder.files[0].path === 'folder/raw',
  'unmapped path-form sound was not filed under the no-bank group');

check(sandbox.abRenderTargetOk('idProp_Physics'), 'prop was not accepted as a render target');
check(sandbox.abRenderTargetOk('idVolume_Trigger_Editable'), 'trigger volume was not accepted');
check(sandbox.abRenderTargetOk('idMover_Platform'), 'mover was not accepted');
check(!sandbox.abRenderTargetOk('idSnapMapAction_Mover_Start'), 'logic action matched the mover rule');
check(!sandbox.abRenderTargetOk('idSnapMapGameEntity_Speaker'), 'speaker was accepted as geometry');
check(sandbox.abNoApplyEver('idSnapMapGameEntity_ComboStart_Coop'), 'player spawn was not globally denied');

check(sandbox.abModelTargetOk('idInteractable_LootCrate'), 'normal interactable could not wear a model');
check(!sandbox.abModelTargetOk('idInteractable_WorldCache'), 'world cache model exception was ignored');
check(sandbox.abModelTargetOk('idInteractable_WorldCache_Child'), 'model exception was not exact-match');
check(sandbox.abIsSpeaker('idSpeaker_Local'), 'speaker subclass was not recognized');
check(sandbox.abIsLight('idSnapMapGameEntity_Light_Point'), 'light subclass was not recognized');
check(sandbox.abIsEmitter('idParticleEmitter_Local'), 'particle emitter was not recognized');
check(sandbox.abIsFxEntity('idLaserHazard'), 'laser FX entity was not recognized');
check(!sandbox.abIsFxEntity('idProp_LaserDecoration'), 'unrelated laser prop was recognized as FX');

check(sandbox.abApplyDenied('m', 'idProp_Physics') === null, 'model was denied on a prop');
check(sandbox.abApplyDenied('cm', 'idProp_Physics') === null, 'material was denied on a prop');
check(sandbox.abApplyDenied('s', 'idSnapMapGameEntity_Speaker') === null, 'sound was denied on a speaker');
check(sandbox.abApplyDenied('s', 'idProp_Physics') !== null, 'sound was accepted on a prop');
check(sandbox.abApplyDenied('li', 'idLight') === null, 'light material was denied on a light');
check(sandbox.abApplyDenied('p', 'idParticleEmitter') === null, 'particle was denied on an emitter');
check(sandbox.abApplyDenied('f', 'idDynamicStampEntity') === null, 'FX was denied on an FX entity');
check(sandbox.abApplyDenied('unknown', 'idSnapMapGameEntity_ComboStart') !== null,
  'global player-spawn denial was bypassed by an unknown carrier');

const countBadge = {textContent: ''};
const fetchMount = {type: 'material', el: function () { return countBadge; }};
fetchSandbox.open(fetchMount);
let sent = fetchSandbox.drain();
let listKinds = sent.filter(function (m) { return m.cmd === 'listAssets'; })
                    .map(function (m) { return m.assetKind; });
check(sent.length === 3, 'first open did more than the active catalog, its qualifier, and pins');
check(listKinds.length === 2 && listKinds.indexOf(0) >= 0 && listKinds.indexOf(12) >= 0,
  'material open did not request exactly Materials plus its atlas-only qualifier');
check(sent.filter(function (m) { return m.cmd === 'pinsLoad'; }).length === 1,
  'pins were not requested exactly once on first open');

fetchSandbox.open(fetchMount);
check(fetchSandbox.drain().length === 0, 'an in-flight catalog was requested twice');
fetchSandbox.receive({assetKind: 0, names: 'materials/one\nmaterials/two\n'});
fetchSandbox.open(fetchMount);
check(fetchSandbox.drain().length === 0, 'a cached catalog was requested again');

fetchMount.type = 'image';
fetchSandbox.open(fetchMount);
sent = fetchSandbox.drain();
listKinds = sent.filter(function (m) { return m.cmd === 'listAssets'; })
                .map(function (m) { return m.assetKind; });
check(sent.length === 1 && listKinds[0] === 1,
  'an unrelated reference catalog pulled qualifiers or other asset types');

fetchMount.type = 'sound';
fetchSandbox.open(fetchMount);
sent = fetchSandbox.drain();
listKinds = sent.filter(function (m) { return m.cmd === 'listAssets'; })
                .map(function (m) { return m.assetKind; });
check(sent.length === 2 && listKinds.indexOf(3) >= 0 && listKinds.indexOf(13) >= 0,
  'sound open did not request exactly Sounds plus the soundbank qualifier');

fetchSandbox.receive({assetKind: 1, names: 'images/one\n'});
fetchSandbox.receive({assetKind: 3, names: 'play_one\n'});
let cache = fetchSandbox.cache();
check(cache.lru.length === 2 && !cache.names.material && cache.names.image && cache.names.sound,
  'catalog cache did not evict the least-recently-used third type');
check(cache.counts.material === 2,
  'catalog eviction discarded the scalar rail count');
fetchMount.type = 'material';
fetchSandbox.open(fetchMount);
sent = fetchSandbox.drain();
listKinds = sent.filter(function (m) { return m.cmd === 'listAssets'; })
                .map(function (m) { return m.assetKind; });
check(listKinds.length === 1 && listKinds[0] === 0,
  'reopening an evicted catalog did not fetch only that catalog');

const previewMount = {sel:'images/one', pvUri:'data:image/png;base64,AAAA', pvNote:'ready',
                      pvTimer:1, pvTries:4, stopped:false};
previewSandbox.forget(previewMount);
check(previewMount.sel === null && previewMount.pvUri === null && previewMount.pvNote === null &&
      previewMount.pvTimer === null && previewMount.pvTries === 0 && previewMount.stopped,
  'leaving a browser view retained its selected preview payload');
sent = previewSandbox.drain();
check(sent.length === 1 && sent[0].cmd === 'cancelPreview',
  'leaving a browser view did not cancel its backend preview generation');

previewRouteSandbox.request({selectedType:'image'}, 'textures/overlap');
previewRouteSandbox.request({selectedType:'material'}, 'materials/overlap');
sent = previewRouteSandbox.drain();
check(sent.length === 2 && sent[0].cmd === 'requestPreview' &&
      sent[0].name === 'textures/overlap' && sent[0].assetKind === 1,
  'an Image selection did not carry the direct-image kind to the host');
check(sent[1].name === 'materials/overlap' && sent[1].assetKind === 0,
  'a Material selection did not preserve the material kind');

const editorIds = new Function('mount', 'response',
  "var abIdsPending = {mount:mount,tile:'volume/blocking'}; function abRenderInsp() {}\n" +
  grab(/function abEsc\(s\)/, '\n') +
  grab(/function abEditorIdsResult\(msg\)/, '\n  }') + '\n' +
  grab(/function abEditorBindings\(m, path\)/, '\n  }') +
  '\nabEditorIdsResult(response); return abEditorBindings(mount, mount.sel);');
const editorReply = {data:{tile:'volume/blocking',sheets:[{id:'native/displayName',properties:['blockPlayers','blockDemons']}]}};
const bindingButtons = editorIds({sel:'volume/blocking'}, editorReply);
check(bindingButtons.includes('data-copy="native/displayName"') && bindingButtons.includes('data-copy="blockPlayers"'),
  'the asset browser must expose the stable sheet and property IDs');
check(!editorIds({sel:'weapons/chainsaw_mp_base'}, editorReply).includes('blockDemons'),
  'a late property lookup must not replace the selected tile');

if (failures) process.exit(1);
console.log('asset browser tests passed');
