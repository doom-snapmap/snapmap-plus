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

if (failures) process.exit(1);
console.log('asset browser tests passed');
