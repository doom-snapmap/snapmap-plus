/* Prefab Details layout, resize, and zero-copy geometry transport contract. */
'use strict';
const fs=require('fs'), path=require('path');
const root=path.join(__dirname,'..');
const html=fs.readFileSync(path.join(root,'src','ui','webview','mockup.html'),'utf8');
const css=fs.readFileSync(path.join(root,'src','ui','webview','studio.css'),'utf8');
const transform=fs.readFileSync(path.join(root,'src','ui','webview','prefab_transform.js'),'utf8');
const js=fs.readFileSync(path.join(root,'src','ui','webview','prefab_viewport.js'),'utf8');
const host=fs.readFileSync(path.join(root,'src','ui','webview','snapmap_plus_ui_webview.cpp'),'utf8');
const backend=fs.readFileSync(path.join(root,'src','backend','prefabpreview.c'),'utf8');
const iface=fs.readFileSync(path.join(root,'src','backend','iface_engine.c'),'utf8');
const common=fs.readFileSync(path.join(root,'src','common','snapmap_plus_iface.h'),'utf8');
let failures=0;
function check(value,message){if(!value){console.error('[FAIL] '+message);failures++;}}

new Function(transform); new Function(js); /* syntax checks without constructing the DOM-backed viewport */
check(html.includes('id="pcPreview"'),'Prefab Details has the 3D viewport');
check(html.indexOf('src="prefab_transform.js"') < html.indexOf('src="prefab_viewport.js"'),
      'pure transform helpers load before the viewport');
check(html.indexOf('id="pcPreview"') < html.indexOf('for="pcName"'),'viewport appears above Name');
check(!html.includes('id="pcChips"') && !html.includes('class="pc-chip"'),'per-type metadata is absent');
check(html.indexOf('id="pcEntityCount"') > html.indexOf('id="pcCanvas"') &&
      html.indexOf('id="pcEntityCount"') < html.indexOf('for="pcName"'),
      'aggregate entity count appears inside the preview above Name');
check(html.includes("d.count + (d.count === 1 ? ' entity' : ' entities')"),'aggregate count uses singular/plural text');
check(/\.pc-preview-count\s*\{[^}]*right:\s*8px;[^}]*bottom:\s*7px;/s.test(css),
      'aggregate count is bottom-right preview metadata');
check(/\.pc-preview-help\s*\{[^}]*top:\s*7px;[^}]*right:\s*8px;/s.test(css),
      'orbit controls replace the count at top-right');
check(/\.pc-preview[^}]*background:\s*var\(--field\)/s.test(css),'viewport uses the Decl Text field background');
check(html.includes('class="pc-preview-legend"') && html.includes('Logic / I/O') && html.includes('Triggers'),
      'viewport explains semantic scene roles');
check(/\.pc-name\s*\{\s*width:\s*100%;\s*\}/.test(css),'Name has no typography override over shared field rules');
check(js.includes('new ResizeObserver('),'element resize is observed directly');
check(js.includes('requestAnimationFrame('),'resize/render work is coalesced to animation frames');
check(js.includes('canvas.width !== w || canvas.height !== h'),'drawing buffer changes only when dimensions change');
check(js.includes('reach = 14') && js.includes('function fadeAt(x,y)') && js.includes('uAlpha*vFade'),
      'extended Cartesian floor grid uses a circular outward alpha mask');
check(js.includes('gl.vertexAttrib4f(1,.5,.5,1,1)'),'grid constant attribute supplies XYZW before mesh drawing');
check(js.includes('uniform mat3 uNormalMatrix') && js.includes('var normal=normalMatrixScratch') &&
      js.includes('gl.uniformMatrix3fv(loc.normalMatrix,false,normal)'),
      'non-uniform prefab scales use a reusable inverse-transpose normal matrix');
check(js.includes("setStatus('3D preview render failed')"),'draw failures become visible instead of leaving a silent blank canvas');
check(js.includes('snapedit_logic_hexagon.lwo') && js.includes('snapedit_logic_circle.lwo') &&
      js.includes('snapedit_logic_diamond.lwo'),'logic, input/output, and filter nodes use installed editor glyphs');
check(js.includes('input:[.5,.5,.5]') && js.includes('output:[.5,.5,.5]') &&
      js.includes('filter:[.5,.5,.5]') && js.includes('logic:[1,1,1]'),
      'logic hexagons remain larger than half-scale I/O circles and filter diamonds');
check(js.includes('composeBottomAnchoredBox(entity.position,entity.axes,size)'),
      'block and trigger proxies preserve the installed meshes bottom-origin dimensions');
check(js.includes('vec3(entity.scalePatch, defaultScale)') &&
      js.includes("entity.role==='blocker' || entity.role==='trigger'") &&
      js.includes('if (entities[i].inherit && entities[i].resolveModel)'),
      'direct-model entities merge sparse saved scale over installed inherited defaults');
check(js.includes("return 'decal'") && js.includes("role!=='decal'&&!MARKER_MODELS[role]"),
      'texture-only decals use flat helper geometry instead of model-resolution cubes');
check(!js.includes("edit.isVisible === false") &&
      js.indexOf("h.indexOf('snapmaps/interactibles/')") < js.indexOf("c.indexOf('volume_trigger')") &&
      js.includes("e.role !== 'trigger'"),
      'semantic triggers are excluded from framing without misclassifying normal isVisible=false entities');
check(js.includes('drawHelperOutline(e') && js.includes("e.role==='trigger'?.10:.16"),
      'trigger geometry is distinguished with translucent fill and an outline');
check(js.includes("rmi.customMaterial && listed[2]") && js.includes("return listed[1] || listed[2] || listed[0]"),
      'blocking volumes prefer their visible shell over item[0] editor trigger geometry');
check(!js.includes('setInterval('),'viewport does not run a continuous polling/render loop');
check(html.includes("addEventListener('sharedbufferreceived'"),'page receives binary geometry by shared buffer');
check(html.includes('releaseBuffer(buffer)'),'page releases WebView shared buffers after GPU upload');
check(host.includes('PostSharedBufferToScript'),'native host posts geometry without base64 encoding');
check(host.includes('resolve_prefab_defaults') && host.includes('SH_PREFAB_DEFAULT_SCALE') &&
      host.includes('m += L"\\\",\\\"scale\\\":"'),
      'native host returns installed scale defaults with model resolution');
check(backend.includes('PP_MAX_VERTICES') && backend.includes('PP_MAX_INDICES'),'native decode has hard geometry budgets');
check(backend.includes('fixed 32-byte cooked metadata block') && backend.includes('len - r.pos >= 24u'),
      'multi-surface installed BMODEL geometry consumes per-surface metadata exactly');
check(backend.includes('sh_prefabpreview_resolve_model') && backend.includes('spawnerEntityPair') &&
      backend.includes('entityStatic'),'pickup spawners resolve through installed entityStatic inheritance');
check(backend.includes('pp_decl_render_scale') && backend.includes('scale_have') &&
      backend.includes('SH_PREFAB_DEFAULT_SCALE'),
      'installed entityDef resolver composes sparse render scale through inheritance');
check(iface.indexOf('strstr(inherit_name, "spawner")') < iface.indexOf('sh_typeinfo_inherit_model(inherit_name'),
      'pickup spawners prefer their semantic entityStatic model over a generic resolved editor model');
check(iface.includes('slots.resolve_prefab_defaults = slot_resolve_prefab_defaults') &&
      common.includes('offsetof(sh_iface_vtbl, resolve_prefab_defaults) == 0x320'),
      'model-and-scale resolution is append-only at interface extension 23');
check(html.includes("model:'',scale:[1,1,1]"),'browser preview completes model-and-scale requests');
check(js.includes('var frame = grabFrame(doc);') && js.includes('entityFromJson(list[i], frame)') &&
      js.includes('applyFrame(vec3(edit.spawnPosition,[0,0,0]), orientationAxes(edit.spawnOrientation)'),
      'the preview places stored transforms through the prefab grabAxis, as the paste does');
check(js.includes('color:nav.marked?NAVIGATION_COLOR:roleColor(role,className)') &&
      html.includes('<i class="pc-legend-dot navigation"></i>AI Navigation') &&
      /\.pc-legend-dot\.navigation\s*\{\s*background:\s*#5cb86b;/.test(css),
      'AI Navigation boxes are tinted and explained in the legend');
check(html.includes('prefabTransform.countNavigationBoxes(doc)') && html.includes("' AI Navigation boxes'"),
      'Prefab Details counts AI Navigation boxes beside the entity count');
check(js.includes("if (!info.scale && entity.role === 'blocker') updateEntityTransform(entity,BLOCKING_DEFAULT_SIZE,true)"),
      'an unresolved blocking decl keeps its 16/128/64 default instead of 1-unit slabs');
check(html.includes("d.grab === 'corrected'") && html.includes('d.result === 3') &&
      host.includes('sh_prefab_grab_json_check(buf') && host.includes('L",\\"grab\\":\\"corrected\\""'),
      'Create reports a corrected grab point and refuses only an unusable frame');

/* Demons and player starts: role, model source and status text, run against the real viewport code
   with no DOM (no canvas, so no GL; the pure helpers are exported as `testing`). */
{
  const posted=[];
  const viewport=new Function('prefabTransform','document','window','requestAnimationFrame',
    'MutationObserver','getComputedStyle','post',js+'\nreturn prefabViewport;')(
    require(path.join(root,'src','ui','webview','prefab_transform.js')),
    {getElementById:()=>null,documentElement:{classList:{contains:()=>false}}},
    {addEventListener:()=>{}},()=>0,function(){this.observe=()=>{};},()=>({getPropertyValue:()=>''}),
    m=>posted.push(m));
  const t=viewport.testing;
  check(t && typeof t.entityRole==='function','viewport exports its pure helpers for tests');
  check(t.entityRole('idPlaceableSnapAIEncounter','snapmaps/placeablesnapaiencounter/imp',{})==='demon',
        'an imp encounter is a demon');
  check(t.entityRole('idSnapMapGameEntity_ComboStart','player/coop/player_start',{})==='player',
        'a player start is a player, not a generic prop');
  check(t.entityRole('idSnapMapGameEntity_Light','snapmaps/lights/user_point',{})==='system' &&
        t.entityRole('idSnapMapLogic_Counter','snapmaps/logic/count',{})==='logic' &&
        t.entityRole('idDynamicStamp','snapmaps/fx/decal',{})==='decal' &&
        t.entityRole('idProp2','snapmaps/props/new/crate01',{})==='prop',
        'lights, logic, decals and props keep their roles');
  check(t.roleColor('demon','')[0]>0.7 && t.roleColor('player','')[2]>0.6,'demons are red, players pink');
  check(html.includes('pc-legend-dot demon') && html.includes('pc-legend-dot player') &&
        /\.pc-legend-dot\.demon\s*\{\s*background:\s*#c76357;/.test(css) &&
        /\.pc-legend-dot\.player\s*\{\s*background:\s*#e07ab3;/.test(css),
        'the legend explains the demon and player tints');
  const frame=[[1,0,0],[0,1,0],[0,0,1]];
  const imp=t.entityFromJson({entityDef:{className:'idPlaceableSnapAIEncounter',
    inherit:'snapmaps/placeablesnapaiencounter/imp',state:{edit:{spawnPosition:{x:10,y:0,z:0}}}}},frame);
  const start=t.entityFromJson({entityDef:{className:'idSnapMapGameEntity_ComboStart',
    inherit:'player/coop/player_start',state:{edit:{}}}},frame);
  check(imp.model==='' && imp.resolveModel && start.resolveModel,
        'demons and player starts ask the backend for their model (the def names none)');
  check(Math.abs(imp.proxyMatrix[10]-96)<1e-4 && Math.abs(imp.proxyMatrix[14]-48)<1e-4,
        'an unresolved demon proxy is a 48x48x96 feet-origin box, not a 32-unit cube');
  check(t.modelUnavailable(imp),'an entity with no model is reported unavailable');
  posted.length=0;
  t.resolve([imp,start],'player/coop/player_start',
    {model:'zion/characters/humans/mp_male/player_snapeditor.md6',scale:null,size:[32,32,96],editorModel:true});
  check(start.model==='zion/characters/humans/mp_male/player_snapeditor.md6' && imp.model==='',
        'the resolved editor model goes to the entities of that inherit only');
  check(posted.some(m=>m.cmd==='requestPrefabMesh' && m.model===start.model),'the md6 is requested as a mesh');
  check(Math.abs(start.proxyMatrix[0]-32)<1e-4 && Math.abs(start.proxyMatrix[10]-96)<1e-4,
        'the palette row size sizes the player start box');
  check(!t.modelUnavailable(start),'a requested mesh is pending, not unavailable');
  check(t.statusText(3,0,0,0)==='Interactive preview' && t.statusText(3,0,1,0)==='1 model unavailable' &&
        t.statusText(3,0,2,0)==='2 models unavailable' && t.statusText(3,1,2,0)==='Loading installed geometry...' &&
        t.statusText(0,0,0,0)==='No renderable entities' &&
        /over the triangle budget/.test(t.statusText(3,0,0,1)),
        'the status names unavailable models instead of claiming an interactive preview');
  check(host.includes('scale, 6);') && host.includes('SH_PREFAB_DEFAULT_EDITOR') &&
        host.includes('SH_PREFAB_DEFAULT_SIZE'),'the host asks for and forwards the palette row model and size');
  check(common.includes('#define SH_PREFAB_DEFAULT_EDITOR 0x4') && common.includes('#define SH_PREFAB_DEFAULT_SIZE  0x8'),
        'the editor-model and size bits are part of the shared ABI');
  check(iface.includes('sh_budget_table_row_model(') && iface.includes('sh_prefabpreview_editor_model('),
        'the backend reads the palette row model live, with the installed sedef as fallback');
}

if(failures){console.error('prefab_viewport_contract_test: '+failures+' failure(s)');process.exit(1);}
console.log('prefab_viewport_contract_test: OK');
