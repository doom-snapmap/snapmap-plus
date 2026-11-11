/* The browser preview's stand-in for the WebView2 host and the backend.
 *
 * Loaded only when mockup.html is opened in a browser for development; the
 * window built into snapmap-plus-ui.dll never contains it (src/ui/embed-page.ps1
 * drops its script tag). It installs a window.chrome.webview that answers the
 * page's messages with sample data, so the page runs unchanged. Keep this file
 * ASCII. */
/* Settings the host keeps in config.json: kept here for the session (the theme in browser storage). */
var previewConfig = {};
try { var previewTheme = localStorage.getItem('sh_theme'); if (previewTheme) previewConfig.theme = JSON.stringify(previewTheme); } catch (e) {}

(function () {
  if (window.chrome && window.chrome.webview) return;   /* hosted: nothing to stand in for */
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage: function (o) { previewHandle(o); },
    addEventListener: function () {},   /* the stand-in calls onMessage itself */
    releaseBuffer: function () {}
  };
  /* The theme the preview last used, before the page reads it. */
  if (previewConfig.theme === '"dark"') document.documentElement.classList.add('dark');
  window.addEventListener('load', function () {
    var dot = document.getElementById('bridgeDot');
    dot.className = 'dot ok';
    dot.setAttribute('aria-label', 'Browser preview');
    dot.title = 'Browser preview';
    onMessage(previewList());
    onMessage({kind: 'selCount', count: 5});
    /* Auto-select the Block Box so the Entities tab shows a populated editor. */
    selectedEids = {}; selectedEids[65] = true; anchorEid = 65; updateSelDom();
    refreshEditorForSelection();
  });
})();

function previewConfigHandle(o) {
  if (o.cmd === 'configGet') {
    var have = Object.prototype.hasOwnProperty.call(previewConfig, o.key);
    onMessage({kind: 'configValue', key: o.key, valueJson: have ? previewConfig[o.key] : '', result: have ? 1 : 0});
    return true;
  }
  if (o.cmd === 'configSet') {
    previewConfig[o.key] = o.valueJson;
    if (o.key === 'theme') { try { localStorage.setItem('sh_theme', JSON.parse(o.valueJson)); } catch (e) {} }
    onMessage({kind: 'configSetResult', key: o.key, result: 1});
    return true;
  }
  return false;
}

/* browser-preview stand-in for the backend's rawmap path pair (see previewHandle) */
var PREVIEW_RAWMAP_DEFAULT = 'C:\\Users\\you\\AppData\\Local\\snapmap-plus\\rawmap.json';
var previewRawmap = {load: PREVIEW_RAWMAP_DEFAULT, save: PREVIEW_RAWMAP_DEFAULT, loads: 0};
var previewPinsDoc = '';      // session-only stand-in for %LOCALAPPDATA%\snapmap-plus\pinned.json
var previewStack0Count = 0;   // fake backend-owned stack 0 count, so Push/Clear feel connected in the demo
var previewReportSigs = {};   // fake dedup memory for the feedback dialog demo (category|title -> seen)
var previewEnts = [
  {eid:65, id:'0_ind_totally_blank_room_4x/snapmaps/volume/blocking', name:'Block Box', hidden:false},
  {eid:61, id:'0_ind_totally_blank_room_4x/player/coop/player_start', name:'My Player Start', hidden:false},
  {eid:62, id:'0_ind_totally_blank_room_4x/snapmaps/variable/string', name:'', hidden:false},
  {eid:63, id:'0_ind_totally_blank_room_4x/snapmaps/variable/integer', name:'', hidden:false},
  {eid:74, id:'0_ind_totally_blank_room_4x/snapmaps/ai/ai', name:'Heavy Gunner', hidden:false},
  {eid:78, id:'0_ind_totally_blank_room_4x/snapmaps/action/variable/integer/add', name:'Add (1)', hidden:false},
  {eid:0,  id:'0_ind_totally_blank_room_4x/snapmaps/filter/user_filter_entity_0', name:'#str_prefilter_any_player', hidden:true},
  {eid:1,  id:'0_ind_totally_blank_room_4x/snapmaps/filter/user_filter_entity_1', name:'#str_prefilter_any_ai', hidden:true},
  {eid:49, id:'0_ind_totally_blank_room_4x/snapmaps/filter/droppable_49', name:'', hidden:true}
];
var previewPrefabs = {
  root: ['Stairs 384x192', 'UAC VTOL (Static) - Dredge', 'Hangar Block'],
  folders: [
    {name: 'Doors', items: ['AirLock Door']},
    {name: 'Encounters', items: ['Corpse Dropper', 'Loadout Station 01']}
  ]
};
function previewFolderIndex(name) { for (var i = 0; i < previewPrefabs.folders.length; i++) if (previewPrefabs.folders[i].name === name) return i; return -1; }
function previewFolderList(folder) { if (!folder) return previewPrefabs.root; var idx = previewFolderIndex(folder); return idx === -1 ? null : previewPrefabs.folders[idx].items; }
var previewMetaBodies = {};   // preview stand-in for each prefab's details view (its snapmapPlus block), keyed "<folder>/<name>"
previewMetaBodies['Hangar Block'] = JSON.stringify({desc: 'Two hangar rooms and the hall between them', tags: ['industrial'], kind: 'modules',
  moduleCount: 3, entityCount: 14, doorCount: 2, variableCount: 1, droppedWires: 1, droppedRefs: 0, hasNavigationBoxes: true,
  environments: ['ind_lazarus'], bounds: {min: [-1024, -512, 0], max: [2048, 1536, 768]},
  modules: [
    {name: '0_ind_totally_blank_room_4x', label: 'Hangar A', origin: [0, 0, 0], rotation: 0, size: [1024, 1024, 768], bounds: {min: [-512, -512, 0], max: [512, 512, 768]}},
    {name: 'ind_hall_2x', label: 'Hall', origin: [1024, 0, 0], rotation: 1, size: [512, 1024, 384], bounds: {min: [512, -512, 0], max: [1024, 512, 384]}},
    {name: '0_ind_totally_blank_room_4x', label: 'Hangar B', origin: [1536, 512, 0], rotation: 2, size: [1024, 1024, 768], bounds: {min: [1024, 0, 0], max: [2048, 1536, 768]}}
  ]});
function previewKindsMap() {
  var m = {};
  for (var k in previewMetaBodies) { try { var p = JSON.parse(previewMetaBodies[k]); if (p && p.kind === 'modules') m[k] = 'modules'; } catch (e) {} }
  return m;
}
function previewMetaMap() {
  var m = {};
  for (var k in previewMetaBodies) {
    try { var p = JSON.parse(previewMetaBodies[k]); if (p && Array.isArray(p.tags) && p.tags.length) m[k] = p.tags; } catch (e) {}
  }
  return m;
}
function previewSendPrefabs() { onMessage({kind:'prefabs', root: previewPrefabs.root, folders: previewPrefabs.folders, meta: previewMetaMap(), kinds: previewKindsMap()}); }
var previewPrefabDetails = {
  'Hangar Block': {count: 14, scene: ''},
  'AirLock Door': {count: 3},
  'Corpse Dropper': {count: 2},
  'Loadout Station 01': {count: 45},
  'Stairs 384x192': {count: 1},
  'UAC VTOL (Static) - Dredge': {count: 4}
};
/* Browser-preview scenes are transform-only stand-ins: enough to exercise orbit, framing, splitter,
   ResizeObserver, and proxy lighting without embedding or inventing any game resource. */
function previewPrefabScene(items) {
  return JSON.stringify({entities:items.map(function(it){
    var a=it[3]||0,c=Math.cos(a),s=Math.sin(a);
    return {entityDef:{className:it[0],inherit:'',state:{edit:{
      spawnPosition:{x:it[1][0],y:it[1][1],z:it[1][2]},
      spawnOrientation:{mat:{'mat[0]':{x:c,y:-s},'mat[1]':{x:s,y:c}}},
      renderModelInfo:{scale:{x:it[2][0],y:it[2][1],z:it[2][2]}}
    }}}};
  })});
}
previewPrefabDetails['AirLock Door'].scene=previewPrefabScene([
  ['idInteractable_Obstacle_SnapDoor',[0,0,48],[96,20,96],0],
  ['idVolume_Trigger_Editable',[0,-48,32],[120,48,64],0],
  ['idProp_Coop',[0,0,104],[108,24,12],0]
]);
previewPrefabDetails['Corpse Dropper'].scene=previewPrefabScene([
  ['idAI_Snapmap',[0,0,32],[22,18,64],-.35],['idVolume_Trigger_Editable',[0,0,72],[96,96,24],0]
]);
previewPrefabDetails['Loadout Station 01'].scene=previewPrefabScene([
  ['idDynamicStampEntity_Coop',[-64,-48,16],[48,48,32],0],['idDynamicStampEntity_Coop',[0,-48,16],[48,48,32],0],
  ['idDynamicStampEntity_Coop',[64,-48,16],[48,48,32],0],['idVolume_Blocking',[0,32,8],[192,24,16],0],
  ['idProp_Coop',[0,24,48],[72,32,80],.15],['idSnapMapListener_Simple',[0,-96,8],[12,12,12],0]
]);
previewPrefabDetails['Stairs 384x192'].scene=previewPrefabScene([
  ['idVolume_Blocking',[-144,0,12],[96,192,24],0],['idVolume_Blocking',[-48,0,36],[96,192,72],0],
  ['idVolume_Blocking',[48,0,60],[96,192,120],0],['idVolume_Blocking',[144,0,84],[96,192,168],0]
]);
previewPrefabDetails['UAC VTOL (Static) - Dredge'].scene=previewPrefabScene([
  ['idProp_Coop',[0,0,48],[180,64,52],0],['idVolume_Blocking',[-72,0,16],[48,160,20],0],
  ['idVolume_Blocking',[72,0,16],[48,160,20],0],['idVolume_Blocking',[0,-72,32],[160,28,16],0]
]);
var previewDescs = {
  'snapmaps/volume/blocking': {summary:"A volume that blocks demon navigation/pathing through its bounds -- a placeable box, not a scriptable logic node."},
  'idVolume_Blocking': {summary:"Runtime class for a blocking volume -- occludes AI navmesh inside the box."},
  'player/coop': {summary:"A co-op player start point. Placed once per spawn location; no SnapMap wiring of its own."},
  'idPlayerStart_Coop': {summary:"Runtime class for a co-op player spawn point."}
};
var previewStates = {
  65: {decl:'edit = {\n\trenderModelInfo = {\n\t\tmodel = "models/mapobjects/snapmaps/box_trigger.lwo";\n\t\tscale = { x = 16; y = 128; z = 64; }\n\t}\n\tblockDemons = false;\n\tisVisible = true;\n\tisNetworkStatic = true;\n}', classname:'idVolume_Blocking', inherit:'snapmaps/volume/blocking', displayname:'Block Box'},
  61: {decl:'edit = {\n\tteam = "coop";\n\tisVisible = true;\n}', classname:'idPlayerStart_Coop', inherit:'player/coop', displayname:'My Player Start'},
  74: {decl:'edit = {\n\taiType = "heavy";\n\thealth = 300;\n}', classname:'idAI_Snapmap', inherit:'snapmaps/ai', displayname:'Heavy Gunner'}
};
var previewTimelines = [
  {eid:101, id:'0_ind_totally_blank_room_4x/snapmaps/unknown_101', name:'Intro Sequence'},
  {eid:102, id:'0_ind_totally_blank_room_4x/snapmaps/unknown_102', name:'Wave 1 Spawns'},
  {eid:103, id:'0_ind_totally_blank_room_4x/snapmaps/unknown_103', name:''},
  {eid:104, id:'0_ind_totally_blank_room_4x/snapmaps/unknown_104', name:'Arena/Wave 2 Spawns'},
  {eid:105, id:'0_ind_totally_blank_room_4x/snapmaps/unknown_105', name:'Arena/Boss'}
];
// sample serialized-entity JSON per preview timeline (the shape parseTimeline walks). 101 = populated,
// 102 = empty componentTimeLine, 103 = no timeline data at all.
var previewTimelineJson = {
  101: JSON.stringify({ entityDef: { state: { edit: { componentTimeLine: { entityEvents: {
    num: 2,
    'item[0]': { entity: '0_ind_totally_blank_room_4x/snapmaps/ai/ai', events: {
      num: 3,
      'item[0]': { eventTime: 0, eventCall: { eventDef: 'ai/spawn', args: { num: 0 } } },
      'item[1]': { eventTime: 2, eventCall: { eventDef: 'ai/set_target', args: { num: 1, 'item[0]': { entity: 'player_start_61' } } } },
      'item[2]': { eventTime: 5, eventCall: { eventDef: 'sound/play_shader', args: { num: 1, 'item[0]': { decl: { sound: 'alert_klaxon' } } } } }
    } },
    'item[1]': { entity: '0_ind_totally_blank_room_4x/snapmaps/action/door/close', events: {
      num: 1,
      'item[0]': { eventTime: 1, eventCall: { eventDef: 'door/open', args: { num: 0 } } }
    } }
  } } } } } }),
  102: JSON.stringify({ entityDef: { state: { edit: { componentTimeLine: { entityEvents: { num: 0 } } } } } }),
  103: JSON.stringify({ entityDef: { state: { edit: {} } } })
};
/* Sound-catalog sample for previewing bank grouping and search without DOOM. */
var PREVIEW_SOUNDS =
  'Play_wpn_HAR_dryFire\nPlay_wpn_sp_HAR_dryfire\n' +
  'Play_wpn_sp_HAR_fire\nPlay_wpn_sp_HAR_Zoom_02\n' +
  'Play_wpn_sp_HAR_Zoom_out_02\nPlay_wpn_sp_chainsaw_rev_high_rpm_decel_mid\n' +
  'Play_wpn_sp_chainsaw_start_up\nPlay_wpn_sp_fireball_summon\n' +
  'Play_wpn_sp_gauss_dryFire\nPlay_wpn_sp_HAR_MASTERY_fire\n' +
  'Play_wpn_sp_HAR_missile_ready\nPlay_wpn_sp_HAR_MissileCantFire\n' +
  'Play_wpn_sp_HAR_missileChamber_Close_S\nPlay_wpn_sp_HAR_missileChamber_mastered\n' +
  'Play_wpn_sp_HAR_missileChamber_S\nPlay_wpn_sp_HAR_StickyBomb\n' +
  'Play_wpn_sp_HAR_stickyshot_explode\nPlay_wpn_sp_plasma_hellified_soldier_charge\n' +
  'Play_wpn_sp_plasma_hellified_soldier_fire\nPlay_air_res_mid\n' +
  'Play_air_res_mid_01\nPlay_amb_3d_air_01\n' +
  'Play_amb_3d_air_02\nPlay_amb_3d_air_03\n' +
  'Play_amb_3d_air_04\nPlay_amb_3d_air_05\n' +
  'Play_amb_3d_alarm_01\nPlay_amb_3d_alarm_02\n' +
  'Play_amb_3d_alarm_03\nPlay_amb_3d_alarm_04\n' +
  'Play_amb_3d_alarm_05\nPlay_amb_3d_computer_01\n' +
  'Play_amb_3d_computer_02\nPlay_amb_3d_computer_03\n' +
  'Play_amb_3d_computer_04\nPlay_amb_3d_electrical_01\n' +
  'Play_amb_3d_electrical_02\nPlay_amb_3d_fire_01\n' +
  'Play_amb_3d_fire_02\nPlay_amb_3d_fire_03\n' +
  'Play__avoid_enemy_grenade\nPlay__behind_enemy\n' +
  'Play__breath\nPlay__breathe_in_out\n' +
  'Play__call_reinforcements\nPlay__chatter\n' +
  'Play__death\nPlay__death_glorykill\n' +
  'Play__enemy_behind\nPlay__enemy_charging\n' +
  'Play__enemy_down_there\nPlay__enemy_flanking\n' +
  'Play__enemy_lost\nPlay__enemy_on_left\n' +
  'Play__enemy_on_right\nPlay__enemy_over_here\n' +
  'Play__enemy_resighted\nPlay__enemy_retreating\n' +
  'Play__enemy_sighted\nPlay__enemy_surprise\n' +
  'Play__enemy_up_there\nPlay__exertion\n' +
  'Play_action_failed\nPlay_adjust_success\n' +
  'Play_alarm\nPlay_automap_on\n' +
  'Play_bottom_block_UI_on\nPlay_button_press\n' +
  'Play_button_rollover\nPlay_Challenge_Complete\n' +
  'Play_challenge_start_clock\nPlay_codex_entry\n' +
  'Play_empty_error\nPlay_endOfLevel_challenges_01\n' +
  'Play_endOfLevel_collectables\nPlay_endOfLevel_combat_score_01\n' +
  'Play_chainsaw_impact_metal_loop\nPlay_Gauss_charge_mechanism\n' +
  'Play_Gauss_charge_short_tonal\nPlay_gauss_spiny_Charge\n' +
  'Play_handgun_fire_charge_01\nplay_melee_impact_default_player_npc_glory\n' +
  'Play_pistol_pickup\nPlay_pistol_vent\n' +
  'Play_plasma_readyToFire\nPlay_plasma_stun_larger_impact\n' +
  'Play_pop_rocket_explode_01\nPlay_sfx_chaingun_intro\n' +
  'Play_sfx_chaingun_pickup\nPlay_sfx_wpn_sp_switchToSpin\n' +
  'Play_amb_buzzer_gritty_01\nPlay_amb_buzzer_gritty_lpf\n' +
  'Play_amb_dirt_debris_rand_looping\nPlay_amb_drySparks\n' +
  'Play_amb_drySparks_distant\nPlay_amb_fan_small\n' +
  'Play_amb_hor_influence_quad_lp_02\nPlay_amb_light_buzz\n' +
  'Play_amb_low_whistle_wind\nPlay_amb_metal_bending_3d\n' +
  'Play_amb_metal_creaking_3d\nPlay_amb_metal_taps_3d\n' +
  'Play_bossfight_cyberdemon_kill\nPlay_D4_MX_combat_UAC_03_140bpm_wip_GLORY_02\n' +
  'Play_D4_MX_combat_UAC_03_140bpm_wip_GLORY_02_softer\nPlay_D4_QuakeCon_imp_scare_v1\n' +
  'Play_D4_QuakeCon_Sting_TheHand\nPlay_e1m1_looped_temp_for_challenge\n' +
  'Play_enter_dos\nplay_EOL_01_intro\n' +
  'play_EOL_02_res\nPlay_leaving_dos\n' +
  'Play_Argent_tower_final_filter_explosion\nPlay_bot_wake_oneshot\n' +
  'Play_breath_bubbles\nPlay_chainsaw_hellknight\n' +
  'Play_chainsaw_mancubus_back\nPlay_chainsaw_mancubus_front\n' +
  'Play_chainsaw_mancubus_head\nPlay_chainsawGlory_long\n' +
  'Play_chainsawGlory_med\nPlay_chainsawGlory_short\n' +
  'Play_amb_gore_nest_falloff\nPlay_ambient_wind\n' +
  'Play_bloody_punch_switch\nPlay_boss_shield_off\n' +
  'Play_boss_shield_on\nPlay_buckshot_impact_concrete_local\n' +
  'Play_buckshot_impact_dirt_local\nPlay_buckshot_impact_flesh_local\n' +
  'Play_buckshot_impact_glass\nPlay_buckshot_impact_glass_local\n' +
  'Play_armor_s01\nPlay_Armored_Offensive\n' +
  'Play_berzerk\nPlay_bezerk_off\n' +
  'Play_bezerk_on\nPlay_bezerk_time_out_beep_01\n' +
  'Play_blood_fueled\nPlay_branch_hitbyplayer_standing02_finish01\n' +
  'Play_airlock_close_01\nPlay_airlock_open_01\n' +
  'Play_argent_top_shutters\nplay_bounce_01\n' +
  'Play_close\nPlay_door_slide_close\n' +
  'Play_door_slide_open\nPlay_dor_floodgate_slam_01\n' +
  'Play_8Bit_stinger_secret_lever\nPlay_Adrenaline_shot\n' +
  'Play_apophis_infusion\nPlay_bobblehead_01\n' +
  'Play_console_uac_beeps\nPlay_damage_weapon_ending\n' +
  'Play_demonic_scream_1\nPlay_electric_hand\n' +
  'Play_ability_on\nPlay_invuln_active_lp\n' +
  'Play_invulnerability_activate\nPlay_mp_pickup_quad_Switch\n' +
  'Play_pop_rocket_explode_large\nPlay_quad_active_lp\n' +
  'Play_quad_damage_activate_switch\nPlay_quad_fire_1Shot\n' +
  'Play_counter_tick_02\nPlay_counter_tick_03\n' +
  'Play_counter_tick_04\nPlay_counter_tick_05\n' +
  'Play_mp_vega_callout_overtime_01\nPlay_mp_vega_callout_overtime_02\n' +
  'Play_mp_vega_callout_overtime_03\nPlay_stop_vo_facilityvoice_002_020_powerofflineall\n' +
  'Play_UAC_Spokesperson_2022_FullyIndoctrinated_02\nPlay_vo_argent_intro\n' +
  'Play_vo_snapmaps_genericfemale1_absolutely_01\nPlay_vo_snapmaps_genericfemale1_accidentreported_01\n' +
  'Play_vo_snapmaps_genericfemale1_activatethatpanel_01\nPlay_vo_snapmaps_genericfemale1_activatethis_01\n' +
  'Play_vo_snapmaps_genericfemale1_ahsomeonenew_01\nPlay_vo_snapmaps_genericfemale1_allnewarrivals_01\n' +
  'Play_vo_snapmaps_genericfemale1_areaisoverrun_01\nPlay_vo_snapmaps_genericfemale1_argh1_01\n';

var PREVIEW_SNDBANKS =
  'Play_wpn_HAR_dryFire|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_dryfire|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_fire|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_Zoom_02|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_Zoom_out_02|doom_snapmaps\n' +
  'Play_wpn_sp_chainsaw_rev_high_rpm_decel_mid|doom_snapmaps\n' +
  'Play_wpn_sp_chainsaw_start_up|doom_snapmaps\n' +
  'Play_wpn_sp_fireball_summon|doom_snapmaps\n' +
  'Play_wpn_sp_gauss_dryFire|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_MASTERY_fire|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_missile_ready|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_MissileCantFire|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_missileChamber_Close_S|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_missileChamber_mastered|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_missileChamber_S|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_StickyBomb|doom_snapmaps\n' +
  'Play_wpn_sp_HAR_stickyshot_explode|doom_snapmaps\n' +
  'Play_wpn_sp_plasma_hellified_soldier_charge|doom_snapmaps\n' +
  'Play_wpn_sp_plasma_hellified_soldier_fire|doom_snapmaps\n' +
  'Play_air_res_mid|doom_snapmaps\n' +
  'Play_air_res_mid_01|doom_snapmaps\n' +
  'Play_amb_3d_air_01|doom_snapmaps\n' +
  'Play_amb_3d_air_02|doom_snapmaps\n' +
  'Play_amb_3d_air_03|doom_snapmaps\n' +
  'Play_amb_3d_air_04|doom_snapmaps\n' +
  'Play_amb_3d_air_05|doom_snapmaps\n' +
  'Play_amb_3d_alarm_01|doom_snapmaps\n' +
  'Play_amb_3d_alarm_02|doom_snapmaps\n' +
  'Play_amb_3d_alarm_03|doom_snapmaps\n' +
  'Play_amb_3d_alarm_04|doom_snapmaps\n' +
  'Play_amb_3d_alarm_05|doom_snapmaps\n' +
  'Play_amb_3d_computer_01|doom_snapmaps\n' +
  'Play_amb_3d_computer_02|doom_snapmaps\n' +
  'Play_amb_3d_computer_03|doom_snapmaps\n' +
  'Play_amb_3d_computer_04|doom_snapmaps\n' +
  'Play_amb_3d_electrical_01|doom_snapmaps\n' +
  'Play_amb_3d_electrical_02|doom_snapmaps\n' +
  'Play_amb_3d_fire_01|doom_snapmaps\n' +
  'Play_amb_3d_fire_02|doom_snapmaps\n' +
  'Play_amb_3d_fire_03|doom_snapmaps\n' +
  'Play__avoid_enemy_grenade|doom_monsters\n' +
  'Play__behind_enemy|doom_monsters\n' +
  'Play__breath|doom_monsters\n' +
  'Play__breathe_in_out|doom_monsters\n' +
  'Play__call_reinforcements|doom_monsters\n' +
  'Play__chatter|doom_monsters\n' +
  'Play__death|doom_monsters\n' +
  'Play__death_glorykill|doom_monsters\n' +
  'Play__enemy_behind|doom_monsters\n' +
  'Play__enemy_charging|doom_monsters\n' +
  'Play__enemy_down_there|doom_monsters\n' +
  'Play__enemy_flanking|doom_monsters\n' +
  'Play__enemy_lost|doom_monsters\n' +
  'Play__enemy_on_left|doom_monsters\n' +
  'Play__enemy_on_right|doom_monsters\n' +
  'Play__enemy_over_here|doom_monsters\n' +
  'Play__enemy_resighted|doom_monsters\n' +
  'Play__enemy_retreating|doom_monsters\n' +
  'Play__enemy_sighted|doom_monsters\n' +
  'Play__enemy_surprise|doom_monsters\n' +
  'Play__enemy_up_there|doom_monsters\n' +
  'Play__exertion|doom_monsters\n' +
  'Play_action_failed|doom_ui\n' +
  'Play_adjust_success|doom_ui\n' +
  'Play_alarm|doom_ui\n' +
  'Play_automap_on|doom_ui\n' +
  'Play_bottom_block_UI_on|doom_ui\n' +
  'Play_button_press|doom_ui\n' +
  'Play_button_rollover|doom_ui\n' +
  'Play_Challenge_Complete|doom_ui\n' +
  'Play_challenge_start_clock|doom_ui\n' +
  'Play_codex_entry|doom_ui\n' +
  'Play_empty_error|doom_ui\n' +
  'Play_endOfLevel_challenges_01|doom_ui\n' +
  'Play_endOfLevel_collectables|doom_ui\n' +
  'Play_endOfLevel_combat_score_01|doom_ui\n' +
  'Play_chainsaw_impact_metal_loop|doom_weapon_sp\n' +
  'Play_Gauss_charge_mechanism|doom_weapon_sp\n' +
  'Play_Gauss_charge_short_tonal|doom_weapon_sp\n' +
  'Play_gauss_spiny_Charge|doom_weapon_sp\n' +
  'Play_handgun_fire_charge_01|doom_weapon_sp\n' +
  'play_melee_impact_default_player_npc_glory|doom_weapon_sp\n' +
  'Play_pistol_pickup|doom_weapon_sp\n' +
  'Play_pistol_vent|doom_weapon_sp\n' +
  'Play_plasma_readyToFire|doom_weapon_sp\n' +
  'Play_plasma_stun_larger_impact|doom_weapon_sp\n' +
  'Play_pop_rocket_explode_01|doom_weapon_sp\n' +
  'Play_sfx_chaingun_intro|doom_weapon_sp\n' +
  'Play_sfx_chaingun_pickup|doom_weapon_sp\n' +
  'Play_sfx_wpn_sp_switchToSpin|doom_weapon_sp\n' +
  'Play_amb_buzzer_gritty_01|doom_ambience\n' +
  'Play_amb_buzzer_gritty_lpf|doom_ambience\n' +
  'Play_amb_dirt_debris_rand_looping|doom_ambience\n' +
  'Play_amb_drySparks|doom_ambience\n' +
  'Play_amb_drySparks_distant|doom_ambience\n' +
  'Play_amb_fan_small|doom_ambience\n' +
  'Play_amb_hor_influence_quad_lp_02|doom_ambience\n' +
  'Play_amb_light_buzz|doom_ambience\n' +
  'Play_amb_low_whistle_wind|doom_ambience\n' +
  'Play_amb_metal_bending_3d|doom_ambience\n' +
  'Play_amb_metal_creaking_3d|doom_ambience\n' +
  'Play_amb_metal_taps_3d|doom_ambience\n' +
  'Play_bossfight_cyberdemon_kill|doom_music\n' +
  'Play_D4_MX_combat_UAC_03_140bpm_wip_GLORY_02|doom_music\n' +
  'Play_D4_MX_combat_UAC_03_140bpm_wip_GLORY_02_softer|doom_music\n' +
  'Play_D4_QuakeCon_imp_scare_v1|doom_music\n' +
  'Play_D4_QuakeCon_Sting_TheHand|doom_music\n' +
  'Play_e1m1_looped_temp_for_challenge|doom_music\n' +
  'Play_enter_dos|doom_music\n' +
  'play_EOL_01_intro|doom_music\n' +
  'play_EOL_02_res|doom_music\n' +
  'Play_leaving_dos|doom_music\n' +
  'Play_Argent_tower_final_filter_explosion|doom_scripted_events\n' +
  'Play_bot_wake_oneshot|doom_scripted_events\n' +
  'Play_breath_bubbles|doom_scripted_events\n' +
  'Play_chainsaw_hellknight|doom_scripted_events\n' +
  'Play_chainsaw_mancubus_back|doom_scripted_events\n' +
  'Play_chainsaw_mancubus_front|doom_scripted_events\n' +
  'Play_chainsaw_mancubus_head|doom_scripted_events\n' +
  'Play_chainsawGlory_long|doom_scripted_events\n' +
  'Play_chainsawGlory_med|doom_scripted_events\n' +
  'Play_chainsawGlory_short|doom_scripted_events\n' +
  'Play_amb_gore_nest_falloff|doom_effects\n' +
  'Play_ambient_wind|doom_effects\n' +
  'Play_bloody_punch_switch|doom_effects\n' +
  'Play_boss_shield_off|doom_effects\n' +
  'Play_boss_shield_on|doom_effects\n' +
  'Play_buckshot_impact_concrete_local|doom_effects\n' +
  'Play_buckshot_impact_dirt_local|doom_effects\n' +
  'Play_buckshot_impact_flesh_local|doom_effects\n' +
  'Play_buckshot_impact_glass|doom_effects\n' +
  'Play_buckshot_impact_glass_local|doom_effects\n' +
  'Play_armor_s01|doom_player\n' +
  'Play_Armored_Offensive|doom_player\n' +
  'Play_berzerk|doom_player\n' +
  'Play_bezerk_off|doom_player\n' +
  'Play_bezerk_on|doom_player\n' +
  'Play_bezerk_time_out_beep_01|doom_player\n' +
  'Play_blood_fueled|doom_player\n' +
  'Play_branch_hitbyplayer_standing02_finish01|doom_player\n' +
  'Play_airlock_close_01|doom_movers\n' +
  'Play_airlock_open_01|doom_movers\n' +
  'Play_argent_top_shutters|doom_movers\n' +
  'play_bounce_01|doom_movers\n' +
  'Play_close|doom_movers\n' +
  'Play_door_slide_close|doom_movers\n' +
  'Play_door_slide_open|doom_movers\n' +
  'Play_dor_floodgate_slam_01|doom_movers\n' +
  'Play_8Bit_stinger_secret_lever|doom_items\n' +
  'Play_Adrenaline_shot|doom_items\n' +
  'Play_apophis_infusion|doom_items\n' +
  'Play_bobblehead_01|doom_items\n' +
  'Play_console_uac_beeps|doom_items\n' +
  'Play_damage_weapon_ending|doom_items\n' +
  'Play_demonic_scream_1|doom_items\n' +
  'Play_electric_hand|doom_items\n' +
  'Play_ability_on|doom_mp\n' +
  'Play_invuln_active_lp|doom_mp\n' +
  'Play_invulnerability_activate|doom_mp\n' +
  'Play_mp_pickup_quad_Switch|doom_mp\n' +
  'Play_pop_rocket_explode_large|doom_mp\n' +
  'Play_quad_active_lp|doom_mp\n' +
  'Play_quad_damage_activate_switch|doom_mp\n' +
  'Play_quad_fire_1Shot|doom_mp\n' +
  'Play_counter_tick_02|doom_vo\n' +
  'Play_counter_tick_03|doom_vo\n' +
  'Play_counter_tick_04|doom_vo\n' +
  'Play_counter_tick_05|doom_vo\n' +
  'Play_mp_vega_callout_overtime_01|doom_vo\n' +
  'Play_mp_vega_callout_overtime_02|doom_vo\n' +
  'Play_mp_vega_callout_overtime_03|doom_vo\n' +
  'Play_stop_vo_facilityvoice_002_020_powerofflineall|doom_vo\n' +
  'Play_UAC_Spokesperson_2022_FullyIndoctrinated_02|doom_vo\n' +
  'Play_vo_argent_intro|doom_vo\n' +
  'Play_vo_snapmaps_genericfemale1_absolutely_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_accidentreported_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_activatethatpanel_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_activatethis_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_ahsomeonenew_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_allnewarrivals_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_areaisoverrun_01|doom_dlc3_vo\n' +
  'Play_vo_snapmaps_genericfemale1_argh1_01|doom_dlc3_vo\n';

function previewList() { return {kind:'list', version:'local (preview)', renderer:'', game:'', editorReady:true, count:0, entities:previewEnts, timelines:previewTimelines}; }
var PREVIEW_MATERIALS =
  'default\nmaterial/snapmap/dynamic_block_textured_gore_floor\n' +
  'models/characters/doommarine/doommarine_eye\n' +
  'models/characters/doommarine_playerhands/doommarine_playerhands\n' +
  'models/characters/doommarine_playerhands/doommarine_playerhands_bloody\n' +
  'models/characters/male_scientist/male_scientist_wounds\n' +
  'models/characters/mp/doom_marine/doom_marine_helmet\nmodels/characters/olivia/olivia_eye_occ2\n' +
  'models/characters/olivia/olivia_head\nmodels/characters/player/hands\n' +
  'models/characters/spencer/spencer_uvset03\nmodels/mapobjects/crates/barrel_a\n' +
  'models/mapobjects/crates/ind_cratex128\nmodels/mapobjects/gore/hand\n' +
  'models/mapobjects/gore/head_mummy\nmodels/mapobjects/hell/hell_door_frame\n' +
  'models/mapobjects/hell_ext/pillars/1st_floor_pillar_med\n' +
  'models/mapobjects/hell_terrain/hell_rib_01\nmodels/mapobjects/hell_terrain/spire_kit_02\n' +
  'models/mapobjects/industrial/large_props/fan_large\nmodels/mapobjects/lights/light_covered\n' +
  'models/mapobjects/mp/ca_mp_graphite_specimentank/ca_mp_graphite_specimentank\n' +
  'models/mapobjects/pickups/ammodispenser/ammodispenser\n' +
  'models/mapobjects/polar_core/modules/train_rail\nmodels/mapobjects/vehicles/forklift_01_ext\n' +
  'models/mapobjects/vehicles/tram/tram_door\nmodels/mapobjects/white_collar/modularconsole_main\n' +
  'models/mapobjects/white_collar/modularconsole_short\n' +
  'models/monsters/hellknight_2/hellknight_fbg\nmodels/monsters/imp/imp\n' +
  'models/monsters/mancubus/mancubus_body_gibs\nmodels/monsters/mp_baron/mp_baron_gore\n' +
  'models/monsters/mp_mancubus/mp_cybermancubus\n' +
  'models/monsters/mp_mancubus/mp_cybermancubus_heart\n' +
  'models/monsters/mp_pinky/mp_pinky_armor_cloaked_allied\nmodels/monsters/mp_prowler/mp_prowler\n' +
  'models/monsters/mphands_prowler/mphands_prowler\nmodels/monsters/pinky/pinky_body_fbg\n' +
  'models/monsters/revenant/rev_body\nmodels/monsters/revenant/rev_body_gibs\n' +
  'models/monsters/welder_zombie/welder_zombie_torso\nmodels/pickups/mp_armor/mp_armor_medium\n' +
  'models/snapmaps/doors/door_horizontal\nmodels/weapons/ammo/combatshotgun_ammo\n' +
  'models/weapons/arc_canon/arc_3rdp\nmodels/weapons/autoturret/autoturret\n' +
  'models/weapons/boost_pack/boost_boot\nmodels/weapons/chaingun/chaingun_mod_gatling\n' +
  'models/weapons/chainsaw/chainsaw_blood1\nmodels/weapons/grenade/grenade\n' +
  'models/weapons/har/har_scope\nmodels/weapons/heavyassaultrifle/custom/har_base_3rd_custom\n' +
  'models/weapons/kinetic_mine/kinetic_mine_enemy\nmodels/weapons/mp_flare_rifle/mp_flare_rifle\n' +
  'models/weapons/mp_flare_rifle/mp_flare_rifle_3rdp\n' +
  'models/weapons/mp_static_rifle/custom/static_rifle_custom\n' +
  'models/weapons/mp_static_rifle/static_rifle_3rdp\n' +
  'models/weapons/plasmarifle/custom/plasmariflebase_3rd_custom\n' +
  'models/weapons/salvo_cannon/salvo_cannon_lod\nmodels/weapons/shotgun/mods/pop_rockets_mod\n' +
  'models/weapons/vortex_rifle/custom/vortex_rifle_3rd_custom\n' +
  'models/weapons/vortex_rifle/vortex_rifle_3rdp\nmodels/weapons/weapon_crate_01/weapon_crate_01\n' +
  'snap/decals/blood/blood_runes_05\nsnap/decals/damage/scratches_3\n' +
  'snap/decals/glass/shattered_02\nsnap/decals/markings/strips_med_c45\n' +
  'snap/decals/science/equation_2\nsnap/decals/science/equation_6\nsnap/decals/science/equation_8\n' +
  'snap/decals/signs/warning_flammable\nsteel\ntextures/biomechanic/tan_tile_01\n' +
  'textures/bluecollar/bc_floor_1b_192_320\ntextures/bluecollar/bc_tile_02\n' +
  'textures/bluecollar/walls/bc_panels_steel\ntextures/bluecollar/walls/bc_walls_detailed\n' +
  'textures/ca/pipes_mp/mp_pipe_simple_01_orange\ntextures/common/prototype\n' +
  'textures/effects/misc/backpack\ntextures/exterior/walls/ind_panels_exterior_orange\n' +
  'textures/gameplay/general_suitmod_container\ntextures/gore/gore_column_01\n' +
  'textures/guis/snapmaps/module_icons/b_ind_engineroom\ntextures/hell/brickpattern03\n' +
  'textures/hell/floor_tile01\ntextures/hell_terrain/ht_ground_sand_01\n' +
  'textures/industrial/catwalks/catwalk01_bottom_192\n' +
  'textures/industrial/catwalks/catwalk01_top_256\ntextures/industrial/floors/ind_lrg_floor_02\n' +
  'textures/industrial/greeble_panel_1\ntextures/industrial/greeble_panels_dark_grey_ind_drk\n' +
  'textures/industrial/ind_caps_1\ntextures/industrial/walls/panel_maint_1_green_iwk\n' +
  'textures/industrial_large/ceiling_lrg_ind\ntextures/industrial_large/elevator_trims_01_purple\n' +
  'textures/industrial_large/frame_large_tile_01_grey\ntextures/mp/glass/ca_mp_glass_dirty_01\n' +
  'textures/mp/mp_basin/ca_mp_basin_uac_trim_03\ntextures/mp/mp_cliffside/ca_mp_uac_tech_1b\n' +
  'textures/mp/mp_concise/ca_mp_concise_panel_03\n' +
  'textures/mp/mp_disposal/ca_mp_disposal_metal_tiles_lrg_sbs\n' +
  'textures/mp/tech/ca_mp_tech_servers_02\ntextures/mp/walls/ca_mp_ind_panels_painted\n' +
  'textures/mp/walls/ca_mp_metal_walls_02\ntextures/mp/wires/ca_mp_wires_01\n' +
  'textures/pipes/handrail_01_blue\ntextures/pipes/handrail_01_steel_clean\n' +
  'textures/pipes/ind_pipe_huge\ntextures/pipes/ind_pipe_huge_copper_s\n' +
  'textures/pipes/ind_pipe_huge_grey\ntextures/pipes/ind_pipe_huge_red\n' +
  'textures/pipes/ind_pipe_huge_steel\ntextures/pipes/pipe_detailed_red_bc\n' +
  'textures/pipes/pipe_simple_01_grey\ntextures/pipes/pipe_simple_01_wc_yellow\n' +
  'textures/pipes/wc_cooling_pipe\ntextures/props/cardboard_tiles_inner\n' +
  'textures/props/cardboard_tiles_outer_brown_b\n' +
  'textures/props/cardboard_tiles_outer_white_damaged\ntextures/sfx/glass_1_sided\n' +
  'textures/uac/floors/grate_rect2\ntextures/uac/panels/uac_metal_tile_01a\n' +
  'textures/whitecollar/wc_floor_red\ntextures/whitecollar/wc_panel_2b\n' +
  'textures/whitecollar/wc_panel_maint_1e\ntextures/whitecollar/wc_panel_maint_tall_1a\n' +
  'textures/whitecollar/wc_panel_tall_1d\nweapons/har/harbase\nweapons/har/harbase_bloody\n';
// Browser-preview stand-in for the Mods tab's document (backend/modui.c writes the real one).
// Generated from tests/fixtures/mods_tab_payload_v3.json (mods_tab_test checks they match).
var PREVIEW_MODS = {"v":3,"gen":12,"root":"C:\\Users\\player\\AppData\\Local\\snapmap-plus\\mods","storeReadable":true,"mods":[{"key":"acceptance.mixed-rows","id":"acceptance.mixed-rows","name":"","author":"","version":"","build":1,"size":41210,"files":6,"pointers":2,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"acceptance.mixed-rows","description":"","origin":"delivery","migratedFrom":"","servedBy":"","builtAt":1793344563,"bundle":"faefdcf61c29976b6947317bd2fc6c12aab4ad60fede0f9dfecd0ced4ffb6810","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/entitydef/acceptance/r137_row_01.decl","generated/decls/entitydef/acceptance/r137_row_02.bmodel","generated/decls/entitydef/acceptance/r137_row_03.bimage","generated/decls/entitydef/acceptance/r137_row_04.snd"],"publishedMaps":0,"newest":true},{"key":"airlock","id":"airlock","name":"Airlock","author":"","version":"0.0.0","build":1,"size":0,"files":1,"pointers":1,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"airlock","description":"The UAC airlock shell as a placeable module piece.","origin":"migration","migratedFrom":"overrides\\airlock","servedBy":"","builtAt":1793344623,"bundle":"14903ded50c7f993b1e19911945057483e9cf6391515e96ae98e8168e189c6ed","counts":{"shadows":1,"sounds":0,"movies":0,"modules":1,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Airlock"],"carriedPaths":[],"publishedMaps":0,"newest":true},{"key":"alex.mp-basin","id":"alex.mp-basin","name":"Basin (multiplayer)","author":"alex","version":"0.1.0","build":1,"size":2210331,"files":14,"pointers":6,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"alex.mp-basin","description":"Two module pieces cut from the multiplayer Basin map.","origin":"","migratedFrom":"","servedBy":"","builtAt":1793344683,"bundle":"c0a67577a9d439a94d404498ea67713e278b3e50961f09a6cb2ab84c48c25ece","counts":{"shadows":0,"sounds":0,"movies":0,"modules":2,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Basin floor","Basin rim"],"carriedPaths":["maps/modules/alex/basin_01.decl","maps/modules/alex/basin_02.bmodel","maps/modules/alex/basin_03.bimage","maps/modules/alex/basin_04.snd","maps/modules/alex/basin_05.decl","maps/modules/alex/basin_06.md6.decl","maps/modules/alex/basin_07.decl","maps/modules/alex/basin_08.bmodel"],"publishedMaps":0,"newest":true},{"key":"alex.r2-imp","id":"alex.r2-imp","name":"R2 Imp copy","author":"alex","version":"1.0.0","build":2,"size":1843230,"files":13,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"alex.r2-imp","description":"A copy of the Imp with its own entityDef, two editor rows and a louder fireball.","origin":"","migratedFrom":"","servedBy":"","builtAt":1793409543,"bundle":"0d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36","counts":{"shadows":0,"sounds":2,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Imp (R2)","Imp (R2, caged)"],"carriedPaths":["generated/decls/entitydef/mods/alex.r2-imp/imp_copy_01.decl","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_02.bmodel","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_03.bimage","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_04.snd","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_05.decl","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_06.md6.decl","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_07.decl","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_08.bmodel","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_09.bimage","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_10.snd","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_11.decl","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_12.md6.decl","generated/decls/entitydef/mods/alex.r2-imp/imp_copy_13.decl"],"publishedMaps":1,"newest":true,"facts":{"adds":13,"borrows":0,"surfaces":0,"editor":0,"replaces":0,"editorOnly":false,"editorTweaks":[],"replacements":[],"edits":[{"file":"generated/decls/entitydef/player/coop.decl","ops":3,"set":3,"remove":0,"list":0,"keyed":0,"bag":0,"drop":0},{"file":"generated/decls/aifsmmanager/states/imp.decl","ops":1,"set":0,"remove":0,"list":0,"keyed":1,"bag":0,"drop":0}],"registries":{"conductor":["mods/alex.r2-imp/single_imp"]}}},{"key":"campaign-models","id":"campaign-models","name":"Campaign models","author":"","version":"0.0.0","build":1,"size":912202,"files":18,"pointers":14,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":4,"folder":"campaign-models","description":"Props from the campaign maps that SnapMap does not list.","origin":"migration","migratedFrom":"overrides\\campaign-models","servedBy":"","builtAt":1793344803,"bundle":"62bd7af0afad6b1ab308f54a0c1325386a9a9cdbd63241d0c8bc41bda111732f","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Hover barge","Cargo lifter","Argent tank","Lab bench"],"carriedPaths":["models/mapobjects/campaign/prop_01.decl","models/mapobjects/campaign/prop_02.bmodel","models/mapobjects/campaign/prop_03.bimage","models/mapobjects/campaign/prop_04.snd"],"publishedMaps":0,"newest":true},{"key":"commdish","id":"commdish","name":"Satellites","author":"","version":"0.0.0","build":1,"size":301144,"files":9,"pointers":6,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"commdish","description":"Satellite dishes.","origin":"migration","migratedFrom":"overrides\\satellites","servedBy":"","builtAt":1793344863,"bundle":"289b3b44edbb51675d7b6b70a05bb69220fab49753927637f3b25c5c08313bcf","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Comm dish","Comm dish (broken)"],"carriedPaths":["models/mapobjects/commdish/dish_01.decl","models/mapobjects/commdish/dish_02.bmodel","models/mapobjects/commdish/dish_03.bimage"],"publishedMaps":0,"newest":true},{"key":"crucible","id":"crucible","name":"Crucible","author":"","version":"0.0.0","build":1,"size":802113,"files":16,"pointers":9,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"crucible","description":"The Crucible blade as a pickup-style prop.","origin":"migration","migratedFrom":"overrides\\crucible","servedBy":"","builtAt":1793344923,"bundle":"b25b9712e3b2e627dc2e6808b955cc6395adc309e7dca0f6dd2e97b1225173e1","counts":{"shadows":2,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["The Crucible (prop)"],"carriedPaths":["models/objects/props_char/crucible/crucible_01.decl","models/objects/props_char/crucible/crucible_02.bmodel","models/objects/props_char/crucible/crucible_03.bimage","models/objects/props_char/crucible/crucible_04.snd","models/objects/props_char/crucible/crucible_05.decl","models/objects/props_char/crucible/crucible_06.md6.decl","models/objects/props_char/crucible/crucible_07.decl"],"publishedMaps":0,"newest":true},{"key":"dlc","id":"dlc","name":"DLC modules","author":"","version":"0.0.0","build":1,"size":1311020,"files":135,"pointers":133,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":4,"folder":"dlc","description":"Module pieces from the Unto the Evil, Hell Followed and Bloodfall maps.","origin":"migration","migratedFrom":"overrides\\dlc modules","servedBy":"","builtAt":1793344983,"bundle":"c3468e607e4bfc89b83ce66ed26c3331b7bb6be45ef0b41f1dffeea364986c06","counts":{"shadows":0,"sounds":0,"movies":0,"modules":21,"vt":4},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Boneyard hall","Boneyard spine","Vor cliff","Vor bridge"],"carriedPaths":["maps/modules/dlc/piece_01.decl","maps/modules/dlc/piece_02.bmodel"],"publishedMaps":1,"newest":true},{"key":"doors","id":"doors","name":"All doors","author":"","version":"0.0.0","build":1,"size":3801224,"files":388,"pointers":271,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":4,"folder":"doors","description":"Every door the game ships, as editor rows, with their open and close sounds.","origin":"migration","migratedFrom":"overrides\\all doors","servedBy":"","builtAt":1793345043,"bundle":"faa17c4a4ef7c84041259dfc590fc67382754d534cea71db7c96ed31c932b16d","counts":{"shadows":117,"sounds":6,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Lift door","Blast door","Hell gate","Vent door"],"carriedPaths":["models/mapobjects/doors/door_01.decl","models/mapobjects/doors/door_02.bmodel","models/mapobjects/doors/door_03.bimage","models/mapobjects/doors/door_04.snd","models/mapobjects/doors/door_05.decl","models/mapobjects/doors/door_06.md6.decl","models/mapobjects/doors/door_07.decl","models/mapobjects/doors/door_08.bmodel","models/mapobjects/doors/door_09.bimage","models/mapobjects/doors/door_10.snd","models/mapobjects/doors/door_11.decl","models/mapobjects/doors/door_12.md6.decl","models/mapobjects/doors/door_13.decl","models/mapobjects/doors/door_14.bmodel","models/mapobjects/doors/door_15.bimage","models/mapobjects/doors/door_16.snd","models/mapobjects/doors/door_17.decl","models/mapobjects/doors/door_18.md6.decl","models/mapobjects/doors/door_19.decl","models/mapobjects/doors/door_20.bmodel","models/mapobjects/doors/door_21.bimage","models/mapobjects/doors/door_22.snd","models/mapobjects/doors/door_23.decl","models/mapobjects/doors/door_24.md6.decl"],"publishedMaps":3,"newest":true},{"key":"elevators","id":"elevators","name":"Elevators","author":"","version":"0.0.0","build":1,"size":1022113,"files":27,"pointers":20,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"elevators","description":"Working elevators.","origin":"migration","migratedFrom":"overrides\\elevators","servedBy":"","builtAt":1793345103,"bundle":"5a34dd5fc90dbfe5ed00290556eaa5ad1db96206a82be63034070f1f7b03cdef","counts":{"shadows":4,"sounds":2,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Freight elevator","Lift platform"],"carriedPaths":["models/mapobjects/elevators/elevator_01.decl","models/mapobjects/elevators/elevator_02.bmodel","models/mapobjects/elevators/elevator_03.bimage","models/mapobjects/elevators/elevator_04.snd","models/mapobjects/elevators/elevator_05.decl","models/mapobjects/elevators/elevator_06.md6.decl","models/mapobjects/elevators/elevator_07.decl"],"publishedMaps":0,"newest":true},{"key":"female.models","id":"female.models","name":"female.models","author":"","version":"","build":1,"size":6275377,"files":28,"pointers":21,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"female.models","description":"","origin":"delivery","migratedFrom":"","servedBy":"","builtAt":1793345163,"bundle":"021b882d0ed7485055bb2ca4a12df73ae4b1cf7d52ab67ad7bea8698449dbd4e","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Female marine"],"carriedPaths":["models/characters/female/body_01.decl","models/characters/female/body_02.bmodel","models/characters/female/body_03.bimage","models/characters/female/body_04.snd","models/characters/female/body_05.decl","models/characters/female/body_06.md6.decl","models/characters/female/body_07.decl"],"publishedMaps":0,"newest":true},{"key":"gauss.pickup","id":"gauss.pickup","name":"Gauss pickup","author":"","version":"0.0.0","build":1,"size":327899,"files":116,"pointers":110,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"gauss.pickup","description":"The single-player Gauss cannon as a weapon pickup, with its animations.","origin":"migration","migratedFrom":"overrides\\sp-gauss-anim","servedBy":"","builtAt":1793345223,"bundle":"0bd24603f87a761c07890f3dcbb6a4a5671ee632b676990ee1e371082c9688fd","counts":{"shadows":6,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Gauss cannon (pickup)"],"carriedPaths":["anim/weapons/gauss/gauss_01.decl","anim/weapons/gauss/gauss_02.bmodel","anim/weapons/gauss/gauss_03.bimage","anim/weapons/gauss/gauss_04.snd","anim/weapons/gauss/gauss_05.decl","anim/weapons/gauss/gauss_06.md6.decl"],"publishedMaps":0,"newest":true},{"key":"icon.sin","id":"icon.sin","name":"Icon of Sin head","author":"","version":"0.0.0","build":1,"size":113220,"files":8,"pointers":7,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"icon.sin","description":"The Boneyard demon head.","origin":"migration","migratedFrom":"overrides\\aqualung","servedBy":"","builtAt":1793345283,"bundle":"56d322519861a8de3de05f3917479b7a84babb2a9acd59fc0e1d61942238c5ea","counts":{"shadows":1,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Icon of Sin head"],"carriedPaths":["models/maps/game/dlc_3/boneyard_head_01.decl"],"publishedMaps":0,"newest":true},{"key":"local.cyberdemon@0.0.0","id":"local.cyberdemon","name":"Cyberdemon","author":"","version":"0.0.0","build":1,"size":4012230,"files":472,"pointers":450,"state":"attention","cause":"","otherBuilds":0,"duplicateId":true,"editorRows":1,"folder":"local.cyberdemon@0.0.0","description":"The Cyberdemon as a placeable demon.","origin":"migration","migratedFrom":"overrides\\cyberdemon","servedBy":"local.cyberdemon@0.0.0-05238b2e","builtAt":1793345343,"bundle":"a9661c08b153c87bf1c4faec3ff94144f4ae3bb377a0836189a84dee47afa711","counts":{"shadows":14,"sounds":12,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Cyberdemon"],"carriedPaths":["generated/decls/entitydef/ai/cyberdemon/cyberdemon_01.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_02.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_03.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_04.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_05.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_06.md6.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_07.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_08.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_09.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_10.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_11.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_12.md6.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_13.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_14.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_15.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_16.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_17.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_18.md6.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_19.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_20.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_21.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_22.snd"],"publishedMaps":1,"newest":false,"builds":[{"folder":"local.cyberdemon@0.0.0","bundle":"a9661c08b153c87bf1c4faec3ff94144f4ae3bb377a0836189a84dee47afa711","build":1,"version":"0.0.0","builtAt":1793345343,"newest":false,"failed":false},{"folder":"local.cyberdemon@0.0.0-05238b2e","bundle":"d7725049f46be3e3f13836a0cb978f3a47ddadddb02729e1dd31be1841d467a3","build":2,"version":"0.0.0","builtAt":1793345403,"newest":true,"failed":false}]},{"key":"local.cyberdemon@0.0.0-05238b2e","id":"local.cyberdemon","name":"Cyberdemon (Boss)","author":"","version":"0.0.0-05238b2e","build":2,"size":4051102,"files":473,"pointers":450,"state":"attention","cause":"","otherBuilds":0,"duplicateId":true,"editorRows":1,"folder":"local.cyberdemon@0.0.0-05238b2e","description":"The boss Cyberdemon with the arena health bar.","origin":"migration","migratedFrom":"overrides\\cyberdemon-boss","servedBy":"","builtAt":1793345403,"bundle":"d7725049f46be3e3f13836a0cb978f3a47ddadddb02729e1dd31be1841d467a3","counts":{"shadows":15,"sounds":12,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Cyberdemon (Boss)"],"carriedPaths":["generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_01.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_02.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_03.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_04.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_05.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_06.md6.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_07.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_08.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_09.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_10.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_11.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_12.md6.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_13.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_14.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_15.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_16.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_17.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_18.md6.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_19.decl","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_20.bmodel","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_21.bimage","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_22.snd","generated/decls/entitydef/ai/cyberdemon/cyberdemon_boss_23.decl"],"publishedMaps":0,"newest":true,"builds":[{"folder":"local.cyberdemon@0.0.0","bundle":"a9661c08b153c87bf1c4faec3ff94144f4ae3bb377a0836189a84dee47afa711","build":1,"version":"0.0.0","builtAt":1793345343,"newest":false,"failed":false},{"folder":"local.cyberdemon@0.0.0-05238b2e","bundle":"d7725049f46be3e3f13836a0cb978f3a47ddadddb02729e1dd31be1841d467a3","build":2,"version":"0.0.0","builtAt":1793345403,"newest":true,"failed":false}]},{"key":"local.hell-guard","id":"local.hell-guard","name":"Hell Guards","author":"","version":"0.0.0","build":2,"size":1210441,"files":500,"pointers":476,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":3,"folder":"local.hell-guard","description":"The three Hell Guard brothers from the campaign as placeable demons, with their melee loops, roar and death vignettes.","origin":"migration","migratedFrom":"overrides\\hell-guards","servedBy":"","builtAt":1793410263,"bundle":"c2b46c98ec7504221f170415fd8bc872257d75b5185522ba7da067813d221f05","counts":{"shadows":498,"sounds":9,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Hell Guard (hammer)","Hell Guard (shield)","Hell Guard (fist)"],"carriedPaths":["generated/decls/entitydef/ai/hellguard/hell_guard_hammer.decl","generated/decls/entitydef/ai/hellguard/hell_guard_shield.decl","generated/decls/entitydef/ai/hellguard/hell_guard_fist.decl","generated/decls/fsm/ai/hellguard/melee_loop.decl","generated/decls/fsm/ai/hellguard/melee_combo.decl","generated/decls/fsm/ai/hellguard/roar.decl","generated/decls/fsm/ai/hellguard/death_vignette.decl","generated/decls/fsm/ai/hellguard/pain.decl","generated/decls/fsm/ai/hellguard/taunt.decl","generated/decls/fsm/ai/hellguard/walk.decl","generated/decls/animweb/ai/hellguard/hellguard_base.decl","generated/decls/animweb/ai/hellguard/hellguard_melee.decl","generated/decls/attackgraph/ai/hellguard/hammer.decl","generated/decls/attackgraph/ai/hellguard/shield.decl","generated/decls/attackgraph/ai/hellguard/fist.decl","generated/decls/snapeditorentitydef/hellguard/hammer.decl","generated/decls/snapeditorentitydef/hellguard/shield.decl","generated/decls/snapeditorentitydef/hellguard/fist.decl","generated/decls/soundevent/vo/hellguard/roar.decl","generated/decls/soundevent/vo/hellguard/grunt.decl","generated/decls/soundevent/vo/hellguard/death.decl","generated/decls/strings/hellguard.decl","generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl","generated/decls/ai/hellguard_tuning.decl"],"publishedMaps":2,"newest":true,"facts":{"adds":400,"borrows":0,"surfaces":0,"editor":0,"replaces":498,"editorOnly":false,"editorTweaks":[],"replacements":[{"path":"generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl","edits":true},{"path":"generated/decls/aifsmmanager/states/hellguard.decl","edits":true}],"edits":[{"file":"generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl","ops":3,"set":0,"remove":0,"list":3,"keyed":0,"bag":0,"drop":0},{"file":"generated/decls/aifsmmanager/states/hellguard.decl","ops":2,"set":1,"remove":1,"list":0,"keyed":0,"bag":0,"drop":0}],"registries":{"conductor":["single_hell_guard_staff"]}}},{"key":"local.my-overrides","id":"local.my-overrides","name":"My overrides","author":"","version":"0.0.0","build":1,"size":329794,"files":63,"pointers":41,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"local.my-overrides","description":"Assorted tweaks to stock declarations.","origin":"migration","migratedFrom":"overrides\\my-overrides","servedBy":"","builtAt":1793345523,"bundle":"d31f236a3821f45d67cdfd57e29dfc17e9c585ca31303d500c3f878a99d2c324","counts":{"shadows":63,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/snappropertyinspector/inspector_01.decl","generated/decls/snappropertyinspector/inspector_02.bmodel","generated/decls/snappropertyinspector/inspector_03.bimage","generated/decls/snappropertyinspector/inspector_04.snd","generated/decls/snappropertyinspector/inspector_05.decl","generated/decls/snappropertyinspector/inspector_06.md6.decl","generated/decls/snappropertyinspector/inspector_07.decl","generated/decls/snappropertyinspector/inspector_08.bmodel","generated/decls/snappropertyinspector/inspector_09.bimage","generated/decls/snappropertyinspector/inspector_10.snd","generated/decls/snappropertyinspector/inspector_11.decl","generated/decls/snappropertyinspector/inspector_12.md6.decl","generated/decls/snappropertyinspector/inspector_13.decl","generated/decls/snappropertyinspector/inspector_14.bmodel","generated/decls/snappropertyinspector/inspector_15.bimage","generated/decls/snappropertyinspector/inspector_16.snd","generated/decls/snappropertyinspector/inspector_17.decl","generated/decls/snappropertyinspector/inspector_18.md6.decl","generated/decls/snappropertyinspector/inspector_19.decl","generated/decls/snappropertyinspector/inspector_20.bmodel","generated/decls/snappropertyinspector/inspector_21.bimage","generated/decls/snappropertyinspector/inspector_22.snd"],"publishedMaps":0,"newest":true,"facts":{"adds":0,"borrows":0,"surfaces":0,"editor":63,"replaces":0,"editorOnly":true,"editorTweaks":["generated/decls/snapeditorentitydef/demons/demon_base.decl","generated/decls/snappropertyinspector_door/door_inspector.decl"],"replacements":[],"edits":[],"registries":{}}},{"key":"local.rocks","id":"local.rocks","name":"Big rocks","author":"","version":"0.0.0","build":1,"size":865651,"files":79,"pointers":33,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":3,"folder":"local.rocks","description":"Hell boulders and rock spires.","origin":"migration","migratedFrom":"overrides\\big rocks","servedBy":"","builtAt":1793345583,"bundle":"2660c2e07b40c43d20e33b6099244a7fc6fc290786994184af60da3ad90f8a81","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Boulder A","Boulder B","Rock spire"],"carriedPaths":["models/mapobjects/hell_terrain/boulder_01.decl","models/mapobjects/hell_terrain/boulder_02.bmodel","models/mapobjects/hell_terrain/boulder_03.bimage","models/mapobjects/hell_terrain/boulder_04.snd","models/mapobjects/hell_terrain/boulder_05.decl","models/mapobjects/hell_terrain/boulder_06.md6.decl","models/mapobjects/hell_terrain/boulder_07.decl","models/mapobjects/hell_terrain/boulder_08.bmodel","models/mapobjects/hell_terrain/boulder_09.bimage","models/mapobjects/hell_terrain/boulder_10.snd","models/mapobjects/hell_terrain/boulder_11.decl","models/mapobjects/hell_terrain/boulder_12.md6.decl","models/mapobjects/hell_terrain/boulder_13.decl","models/mapobjects/hell_terrain/boulder_14.bmodel","models/mapobjects/hell_terrain/boulder_15.bimage","models/mapobjects/hell_terrain/boulder_16.snd","models/mapobjects/hell_terrain/boulder_17.decl","models/mapobjects/hell_terrain/boulder_18.md6.decl","models/mapobjects/hell_terrain/boulder_19.decl","models/mapobjects/hell_terrain/boulder_20.bmodel","models/mapobjects/hell_terrain/boulder_21.bimage","models/mapobjects/hell_terrain/boulder_22.snd","models/mapobjects/hell_terrain/boulder_23.decl","models/mapobjects/hell_terrain/boulder_24.md6.decl"],"publishedMaps":0,"newest":true},{"key":"local.sp-pistol","id":"local.sp-pistol","name":"Campaign pistol","author":"","version":"0.0.0","build":1,"size":1798800,"files":3,"pointers":1,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"local.sp-pistol","description":"","origin":"migration","migratedFrom":"overrides\\sp-pistol\\sp-pistol","servedBy":"","builtAt":1793345643,"bundle":"bd46b8cd3b00bf2028b3cd1095d12cd8786e2863238c4f0b3b2c9fb9afdabad0","counts":{"shadows":2,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Pistol (campaign)"],"carriedPaths":["generated/decls/weapon/pistol_01.decl","generated/decls/weapon/pistol_02.bmodel"],"publishedMaps":0,"newest":true},{"key":"local.spider-mastermind","id":"local.spider-mastermind","name":"Spider Mastermind","author":"","version":"0.0.0","build":1,"size":4924779,"files":509,"pointers":470,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"local.spider-mastermind","description":"The Spider Mastermind as a placeable boss.","origin":"migration","migratedFrom":"overrides\\spider-mastermind\\spider-mastermind","servedBy":"","builtAt":1793345703,"bundle":"d436c9cde8af7f4b1296fdb79cdc4dcc7782144a640e176d3422a81c287353f7","counts":{"shadows":19,"sounds":14,"movies":1,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Spider Mastermind"],"carriedPaths":["generated/decls/entitydef/ai/spider_mastermind/spider_01.decl","generated/decls/entitydef/ai/spider_mastermind/spider_02.bmodel","generated/decls/entitydef/ai/spider_mastermind/spider_03.bimage","generated/decls/entitydef/ai/spider_mastermind/spider_04.snd","generated/decls/entitydef/ai/spider_mastermind/spider_05.decl","generated/decls/entitydef/ai/spider_mastermind/spider_06.md6.decl","generated/decls/entitydef/ai/spider_mastermind/spider_07.decl","generated/decls/entitydef/ai/spider_mastermind/spider_08.bmodel","generated/decls/entitydef/ai/spider_mastermind/spider_09.bimage","generated/decls/entitydef/ai/spider_mastermind/spider_10.snd","generated/decls/entitydef/ai/spider_mastermind/spider_11.decl","generated/decls/entitydef/ai/spider_mastermind/spider_12.md6.decl","generated/decls/entitydef/ai/spider_mastermind/spider_13.decl","generated/decls/entitydef/ai/spider_mastermind/spider_14.bmodel","generated/decls/entitydef/ai/spider_mastermind/spider_15.bimage","generated/decls/entitydef/ai/spider_mastermind/spider_16.snd","generated/decls/entitydef/ai/spider_mastermind/spider_17.decl","generated/decls/entitydef/ai/spider_mastermind/spider_18.md6.decl","generated/decls/entitydef/ai/spider_mastermind/spider_19.decl","generated/decls/entitydef/ai/spider_mastermind/spider_20.bmodel","generated/decls/entitydef/ai/spider_mastermind/spider_21.bimage","generated/decls/entitydef/ai/spider_mastermind/spider_22.snd","generated/decls/entitydef/ai/spider_mastermind/spider_23.decl","generated/decls/entitydef/ai/spider_mastermind/spider_24.md6.decl"],"publishedMaps":0,"newest":true,"facts":{"adds":480,"borrows":0,"surfaces":0,"editor":0,"replaces":19,"editorOnly":false,"editorTweaks":[],"replacements":[{"path":"generated/decls/aifsmmanager/states/spidermastermind.decl","edits":true}],"edits":[{"file":"generated/decls/aifsmmanager/states/spidermastermind.decl","ops":2,"set":0,"remove":0,"list":0,"keyed":2,"bag":0,"drop":0}],"registries":{}}},{"key":"local.tree","id":"local.tree","name":"Dead tree","author":"","version":"0.0.0","build":1,"size":389035,"files":8,"pointers":5,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"local.tree","description":"","origin":"migration","migratedFrom":"overrides\\tree","servedBy":"","builtAt":1793345763,"bundle":"d3b7c265dbcf272f0bb43f84329a8c5a11827222d02ae39f537b269260ed2e6d","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":["Dead tree"],"carriedPaths":["models/mapobjects/hell/tree_01.decl","models/mapobjects/hell/tree_02.bmodel","models/mapobjects/hell/tree_03.bimage"],"publishedMaps":0,"newest":true},{"key":"local.vehicles","id":"local.vehicles","name":"Vehicles","author":"","version":"0.0.0","build":1,"size":925786,"files":11,"pointers":6,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"local.vehicles","description":"UAC ground vehicles.","origin":"migration","migratedFrom":"overrides\\vehicles","servedBy":"","builtAt":1793345823,"bundle":"11fa4f6bd161085cfc3976fa7c2e7df9d03d16dda2cf9c5abd6dc3ecb3f49d36","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Cargo truck","Forklift"],"carriedPaths":["models/mapobjects/vehicles/truck_01.decl","models/mapobjects/vehicles/truck_02.bmodel","models/mapobjects/vehicles/truck_03.bimage","models/mapobjects/vehicles/truck_04.snd","models/mapobjects/vehicles/truck_05.decl"],"publishedMaps":0,"newest":true},{"key":"mapobjects.prop","id":"mapobjects.prop","name":"Props","author":"","version":"0.0.0","build":1,"size":308676,"files":5,"pointers":2,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"mapobjects.prop","description":"","origin":"migration","migratedFrom":"overrides\\prop","servedBy":"","builtAt":1793345883,"bundle":"447faf9688ecec3520dcc6fce79400d899fe208f5372809c144945d210aa184a","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Crate stack"],"carriedPaths":["models/mapobjects/props/crate_01.decl","models/mapobjects/props/crate_02.bmodel","models/mapobjects/props/crate_03.bimage"],"publishedMaps":0,"newest":true},{"key":"misc.obj","id":"misc.obj","name":"Misc models","author":"","version":"0.0.0","build":1,"size":195644,"files":7,"pointers":3,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"misc.obj","description":"","origin":"migration","migratedFrom":"overrides\\misc models","servedBy":"","builtAt":1793345943,"bundle":"2c4b5d38fa5e6624e5cb13d68eec12b94a75c72d0b0fd9e11161366ba564e2cc","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Gore pile","Skull rack"],"carriedPaths":["models/mapobjects/misc/obj_01.decl","models/mapobjects/misc/obj_02.bmodel","models/mapobjects/misc/obj_03.bimage","models/mapobjects/misc/obj_04.snd"],"publishedMaps":0,"newest":true},{"key":"mod.grenade.launcher","id":"mod.grenade.launcher","name":"Beefy grenade launcher","author":"","version":"0.0.0","build":1,"size":3379,"files":3,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"mod.grenade.launcher","description":"Stronger grenade launcher damage in multiplayer loadouts.","origin":"migration","migratedFrom":"overrides\\beefy-grenade-launcher","servedBy":"","builtAt":1793346003,"bundle":"33e8f9cf5f851445a1945a7ca25b481f78d7d1da5fdc4c553a394afec09dbbdd","counts":{"shadows":3,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/damage/damage/player/mp/loadout/grenade_launcher_01.decl","generated/decls/damage/damage/player/mp/loadout/grenade_launcher_02.bmodel","generated/decls/damage/damage/player/mp/loadout/grenade_launcher_03.bimage"],"publishedMaps":0,"newest":true},{"key":"olivia-pierce-complete","id":"olivia-pierce-complete","name":"Olivia Pierce","author":"","version":"0.0.0","build":1,"size":5658103,"files":13,"pointers":6,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"olivia-pierce-complete","description":"Olivia Pierce with her campaign cinematics.","origin":"migration","migratedFrom":"overrides\\olivia-pierce-complete","servedBy":"","builtAt":1793346063,"bundle":"5a2f75e085aafe30b52759d1957c4bc54bc3406a0062b815c357747d4fde8b7f","counts":{"shadows":0,"sounds":3,"movies":2,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Olivia Pierce","Olivia Pierce (possessed)"],"carriedPaths":["models/characters/olivia/olivia_01.decl","models/characters/olivia/olivia_02.bmodel","models/characters/olivia/olivia_03.bimage","models/characters/olivia/olivia_04.snd","models/characters/olivia/olivia_05.decl","models/characters/olivia/olivia_06.md6.decl","models/characters/olivia/olivia_07.decl"],"publishedMaps":0,"newest":true},{"key":"p6probe","id":"p6probe","name":"p6probe","author":"","version":"1.0.0","build":1,"size":1404,"files":2,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"p6probe","description":"","origin":"install","migratedFrom":"","servedBy":"","builtAt":1793346123,"bundle":"1273bb6994b10847e6a7bce3281c66cc3a344e6445ddbf0ee05617ccdf573533","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/entitydef/p6probe/probe_01.decl","generated/decls/entitydef/p6probe/probe_02.bmodel"],"publishedMaps":0,"newest":true},{"key":"plasma.pickup","id":"plasma.pickup","name":"Plasma pickup","author":"","version":"0.0.0","build":1,"size":829622,"files":41,"pointers":30,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"plasma.pickup","description":"","origin":"migration","migratedFrom":"overrides\\pickup plasma","servedBy":"","builtAt":1793346183,"bundle":"e94c7fe129fe6980402876ba3a61397ad6e2c2813933afbb5c563930b6c3e74d","counts":{"shadows":3,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Plasma rifle (pickup)"],"carriedPaths":["anim/weapons/plasma/plasma_01.decl","anim/weapons/plasma/plasma_02.bmodel","anim/weapons/plasma/plasma_03.bimage","anim/weapons/plasma/plasma_04.snd","anim/weapons/plasma/plasma_05.decl","anim/weapons/plasma/plasma_06.md6.decl","anim/weapons/plasma/plasma_07.decl","anim/weapons/plasma/plasma_08.bmodel","anim/weapons/plasma/plasma_09.bimage","anim/weapons/plasma/plasma_10.snd","anim/weapons/plasma/plasma_11.decl"],"publishedMaps":0,"newest":true},{"key":"rctest","id":"rctest","name":"rctest","author":"","version":"1.0.0","build":1,"size":2210,"files":3,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"rctest","description":"","origin":"install","migratedFrom":"","servedBy":"","builtAt":1793346243,"bundle":"bc7c0da046eab6e4e37674a83fa8b90552a6344d1a7fd42057583bb5dc3bd439","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/entitydef/rctest/rc_01.decl","generated/decls/entitydef/rctest/rc_02.bmodel","generated/decls/entitydef/rctest/rc_03.bimage"],"publishedMaps":0,"newest":true},{"key":"space.station","id":"space.station","name":"Space station","author":"","version":"0.0.0","build":1,"size":8900760,"files":27,"pointers":12,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":3,"folder":"space.station","description":"Space station module pieces with a starfield.","origin":"migration","migratedFrom":"overrides\\space station","servedBy":"","builtAt":1793346303,"bundle":"2e01bff8bf1d3048d81a3b558f6a846b38efc6b5160399098a08bb9d49abd7b4","counts":{"shadows":0,"sounds":0,"movies":0,"modules":6,"vt":2},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Station hull","Station window","Station dock"],"carriedPaths":["maps/modules/space_station/hull_01.decl","maps/modules/space_station/hull_02.bmodel","maps/modules/space_station/hull_03.bimage","maps/modules/space_station/hull_04.snd","maps/modules/space_station/hull_05.decl","maps/modules/space_station/hull_06.md6.decl","maps/modules/space_station/hull_07.decl","maps/modules/space_station/hull_08.bmodel","maps/modules/space_station/hull_09.bimage","maps/modules/space_station/hull_10.snd","maps/modules/space_station/hull_11.decl","maps/modules/space_station/hull_12.md6.decl","maps/modules/space_station/hull_13.decl","maps/modules/space_station/hull_14.bmodel","maps/modules/space_station/hull_15.bimage"],"publishedMaps":0,"newest":true},{"key":"spencer","id":"spencer","name":"Robots","author":"","version":"0.0.0","build":1,"size":1641515,"files":10,"pointers":4,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":2,"folder":"spencer","description":"","origin":"migration","migratedFrom":"overrides\\robots","servedBy":"","builtAt":1793346363,"bundle":"4bbe4c19afe537effa358928c5129ceccd4f18200b2abcbeaa6762acf507b0e5","counts":{"shadows":0,"sounds":2,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Maintenance robot","Security robot"],"carriedPaths":["models/characters/robots/robot_01.decl","models/characters/robots/robot_02.bmodel","models/characters/robots/robot_03.bimage","models/characters/robots/robot_04.snd","models/characters/robots/robot_05.decl","models/characters/robots/robot_06.md6.decl"],"publishedMaps":0,"newest":true},{"key":"swf","id":"swf","name":"All swf","author":"","version":"0.0.0","build":1,"size":1204904,"files":229,"pointers":226,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"swf","description":"Interface screens for in-world displays.","origin":"migration","migratedFrom":"overrides\\all swf","servedBy":"","builtAt":1793346423,"bundle":"77be302b992e9365232282d88093750f75b4417d23401943ddbb14f5cf31f942","counts":{"shadows":3,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":[],"carriedPaths":["generated/swf/screen_01.decl","generated/swf/screen_02.bmodel","generated/swf/screen_03.bimage"],"publishedMaps":0,"newest":true},{"key":"tarps","id":"tarps","name":"Tarps","author":"","version":"0.0.0","build":1,"size":340261,"files":7,"pointers":4,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"tarps","description":"","origin":"migration","migratedFrom":"overrides\\tarps","servedBy":"","builtAt":1793346483,"bundle":"361d7574671ab6df788e79b0cf29a673754037e31896ad1c5c4a5efd920298b6","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Tarp (hanging)"],"carriedPaths":["models/mapobjects/tarps/tarp_01.decl","models/mapobjects/tarps/tarp_02.bmodel","models/mapobjects/tarps/tarp_03.bimage"],"publishedMaps":0,"newest":true},{"key":"test.shader-probe","id":"test.shader-probe","name":"Shader probe","author":"","version":"1.0.0","build":1,"size":3311,"files":2,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"test.shader-probe","description":"A test material for the shader work.","origin":"install","migratedFrom":"","servedBy":"","builtAt":1793346543,"bundle":"fa589b23d45da757de62f3a1bdaaa5de54ff9e4554656c1f4662e00c9f83cdad","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/material2/test/probe_01.decl","generated/decls/material2/test/probe_02.bmodel"],"publishedMaps":0,"newest":true},{"key":"test.shader-tint","id":"test.shader-tint","name":"Shader tint","author":"","version":"1.0.0","build":1,"size":3402,"files":2,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":0,"folder":"test.shader-tint","description":"A tinted test material.","origin":"install","migratedFrom":"","servedBy":"","builtAt":1793346603,"bundle":"0bfc66f294be8dcacddb87c5989083806fbac3971c251c51f62d6a4096863517","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{}},"editorRowNames":[],"carriedPaths":["generated/decls/material2/test/tint_01.decl","generated/decls/material2/test/tint_02.bmodel"],"publishedMaps":0,"newest":true},{"key":"waterfalls","id":"waterfalls","name":"Waterfalls","author":"","version":"0.0.0","build":1,"size":15655,"files":2,"pointers":0,"state":"installed","cause":"","otherBuilds":0,"duplicateId":false,"editorRows":1,"folder":"waterfalls","description":"","origin":"migration","migratedFrom":"overrides\\waterfalls","servedBy":"","builtAt":1793346663,"bundle":"0a65c7b8745b599150222d0df3082a666312402478528ba09ab246008a08d667","counts":{"shadows":0,"sounds":0,"movies":0,"modules":0,"vt":0},"requirements":{"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Waterfall"],"carriedPaths":["models/mapobjects/water/waterfall_01.decl","models/mapobjects/water/waterfall_02.bmodel"],"publishedMaps":0,"newest":true}],"migrate":{"entries":34,"pending":2,"running":false},"map":{"entities":214,"names":96,"rows":[],"placed":[],"name":"Hell Guard Arena","id":"e2c1f0a9b7d34c5813aa","saved":1793380563,"published":{"state":"published","at":1793164563},"uploaded":{"claimed":true,"at":1793164663},"tagged":true,"mods":[{"key":"local.hell-guard","id":"local.hell-guard","version":"0.0.0","state":"installed","placedEntities":["Hell Guard (hammer) 1","Hell Guard (hammer) 2","Hell Guard (hammer) 3","Hell Guard (hammer) 4","Hell Guard (hammer) 5","Hell Guard (hammer) 6","Hell Guard (shield) 1","Hell Guard (shield) 2","Hell Guard (shield) 3","Hell Guard (shield) 4","Hell Guard (fist) 1","Hell Guard (fist) 2"],"placedCount":12,"references":[{"kind":"timeline","name":"Wave 2: spawn the guards"},{"kind":"wire","name":"Relay: all guards dead"}],"size":1210441,"attached":false,"nothingPlaced":false},{"key":"doors","id":"doors","version":"0.0.0","state":"installed","placedEntities":["Blast door 1","Blast door 2","Blast door 3","Lift door 1","Lift door 2"],"placedCount":5,"references":[{"kind":"wire","name":"Arena gate: open on wave 3"}],"size":3801224,"attached":false,"nothingPlaced":false},{"key":"alex.r2-imp","id":"alex.r2-imp","version":"1.0.0","state":"installed","placedEntities":["Imp (R2) 1","Imp (R2) 2","Imp (R2) 3"],"placedCount":3,"references":[],"size":1843230,"attached":false,"nothingPlaced":false},{"key":"female.models","id":"female.models","version":"","state":"installed","placedEntities":["Female marine 1"],"placedCount":1,"references":[{"kind":"timeline","name":"Intro: marine walks to the gate"}],"size":6275377,"attached":false,"nothingPlaced":false},{"key":"","id":"jo.lava-floor","version":"1.2.0","state":"missing","placedEntities":["Lava floor A 1","Lava floor B 1"],"placedCount":2,"references":[],"size":66000,"attached":false,"nothingPlaced":false}],"vanilla":false,"registryClashes":[]}};  // migratedFrom names the retired package folder a mod was moved from
// The Files view's preview data: local.hell-guard's files as its folder holds them (12 of its 260 cooked animations), the classes from its mod.json, the archives illustrative.
var PREVIEW_MOD_FILES = {"local.hell-guard":{"files":[{"path":"mod.json","size":367805,"kind":"text","class":"descriptor","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aibehavior/mods/local.hell-guard/behaviors/zion/talismanguard/default.decl","size":1606,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aicomponent_oliviasguard/mods/local.hell-guard/aicomponent/oliviasguard.decl","size":3005,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aicomponent_oliviasguard/mods/local.hell-guard/aicomponent/oliviasguard_twin.decl","size":3048,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aicomponent_waveblast/mods/local.hell-guard/aicomponent/waveblast/oliviasguard/base.decl","size":3760,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard_beam.decl","size":318,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard_beam_spin.decl","size":318,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard_melee.decl","size":286,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard_radial.decl","size":318,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard_spin.decl","size":319,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard_spinsmash.decl","size":318,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/ammo/mods/local.hell-guard/ammo/zion/ai/oliviasguard/abaddon.decl","size":104,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/ammo/mods/local.hell-guard/ammo/zion/ai/oliviasguard/apollyon_beam.decl","size":230,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/ammo/mods/local.hell-guard/ammo/zion/ai/oliviasguard/apollyon_combo_beam.decl","size":162,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/mods/local.hell-guard/zion/characters/monsters/talismanguard.decl","size":123705,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/mods/local.hell-guard/zion/snapmap/demons/hell_guard_combo_snapeditor.decl","size":6593,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/mods/local.hell-guard/zion/snapmap/demons/hell_guard_hammer_snapeditor.decl","size":6599,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/mods/local.hell-guard/zion/snapmap/demons/hell_guard_staff_snapeditor.decl","size":6593,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/mods/local.hell-guard/zion/synccollab/oliviasguard/on_oliviasguard.decl","size":2380,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/mods/local.hell-guard/zion/syncmelee/talismanguard.decl","size":5674,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/attackgraph/mods/local.hell-guard/ai/talismanguard.decl","size":101504,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl","size":3695,"kind":"text","class":"edit","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/hellknight/attachments/death2_fullbody.decl","size":434,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/oliviasguard_base.decl","size":7841,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard/combo.decl","size":436,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard/hammer.decl","size":609,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard/misc/punch_deathcollision.decl","size":917,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard/misc/shield_collision.decl","size":1088,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard/misc/worm_deathcollision.decl","size":914,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard/staff.decl","size":434,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/talismanguard_base.decl","size":8250,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_heavy_assault_full_boss.decl","size":430,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_heavy_assault_half_boss.decl","size":294,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_plasma_rifle_full_boss.decl","size":432,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_plasma_rifle_half_boss.decl","size":294,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_rocket_full_boss.decl","size":435,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_rocket_half_boss.decl","size":291,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_shotgun_full_boss.decl","size":428,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/ammo/lootpinata/items/ammo_shotgun_half_boss.decl","size":284,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/lootpinata/items/health_100pct_boss.decl","size":442,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/lootpinata/items/health_10pct_boss.decl","size":211,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/lootpinata/items/health_25pct_boss.decl","size":211,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/lootpinata/items/health_50pct_boss.decl","size":254,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/prop/zion/lootpinata/items/health_5pct_boss.decl","size":210,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/snapmaps/placeablesnapaiencounter/hell_guard_combo.decl","size":537,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/snapmaps/placeablesnapaiencounter/hell_guard_hammer.decl","size":538,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/snapmaps/placeablesnapaiencounter/hell_guard_staff.decl","size":537,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/zion/ai/oliviasguard/waveblast/melee/tallwedge.decl","size":283,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/zion/ai/oliviasguard/waveblast/melee/wavefront.decl","size":155,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/zion/ai/oliviasguard/waveblast/melee/wavefront_large.decl","size":161,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/zion/synccollab/aionai/oliviasguard/on_talismanguard.decl","size":1874,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/zion/syncmelee/talismanguard.decl","size":2750,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/hammer_idle.decl","size":706,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/oliviasguard.decl","size":17503,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/staff_idle.decl","size":2168,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/swapattack.decl","size":1588,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/swapattack_large.decl","size":1640,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/wall.decl","size":638,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/goregraph/mods/local.hell-guard/ai/oliviasguard.decl","size":29341,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/goregraph/mods/local.hell-guard/ai/talismanguard.decl","size":22876,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/heavy_assault_rifle/ammo20_explicit_100pct_boss.decl","size":597,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/heavy_assault_rifle/ammo20_explicit_50pct_boss.decl","size":275,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/plasma_rifle/ammo30_explicit_100pct_boss.decl","size":597,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/plasma_rifle/ammo30_explicit_50pct_boss.decl","size":268,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/rocket_launcher/ammo5_explicit_100pct_boss.decl","size":594,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/rocket_launcher/ammo5_explicit_50pct_boss.decl","size":267,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/shotgun/ammo10_explicit_100pct_boss.decl","size":587,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/ammo/shotgun/ammo10_explicit_50pct_boss.decl","size":253,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/health/boss_100_force_drop.decl","size":578,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/health/boss_10_force_drop.decl","size":225,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/health/boss_25_force_drop.decl","size":225,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/health/boss_50_force_drop.decl","size":226,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/lootdrop/mods/local.hell-guard/zion/health/boss_5_force_drop.decl","size":223,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/material/mods/local.hell-guard/textures/guis/snapmaps/entity_icons/demons/hell_guard_enc.decl","size":648,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/characters/monsters/oliviasguard/base/oliviasguard.md6.decl","size":422593,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/characters/monsters/talisman_guard/base/talisman_guard.md6.decl","size":183788,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/characters/monsters/talisman_guard/base/talisman_guard_snapeditor.md6.decl","size":264,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/objects/weapons/melee/hammer.md6.decl","size":243,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/objects/weapons/melee/staff.md6.decl","size":461,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/objects/weapons/melee/staff_signature.md6.decl","size":459,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/projectile/mods/local.hell-guard/projectile/zion/ai/oliviasguard/abaddon.decl","size":148,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/projectile/mods/local.hell-guard/projectile/zion/ai/oliviasguard/apollyon_beam.decl","size":417,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/projectile/mods/local.hell-guard/projectile/zion/ai/oliviasguard/apollyon_combo_beam.decl","size":530,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/projectileimpacteffect/mods/local.hell-guard/footsteps/ai_oliviasguards.decl","size":4556,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/prophealth/mods/local.hell-guard/health/boss_10pct.decl","size":142,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/prophealth/mods/local.hell-guard/health/boss_25pct.decl","size":142,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/prophealth/mods/local.hell-guard/health/boss_50pct.decl","size":143,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/prophealth/mods/local.hell-guard/health/boss_5pct.decl","size":141,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/heavy_rifle_full.decl","size":148,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/heavy_rifle_half.decl","size":105,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/plasma_rifle_full.decl","size":150,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/plasma_rifle_half.decl","size":106,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/rocket_launcher_half.decl","size":109,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/shotgun_half.decl","size":101,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/ribbon/mods/local.hell-guard/apollyon_beam.decl","size":341,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapaiencounter/mods/local.hell-guard/single_hell_guard_combo.decl","size":325,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapaiencounter/mods/local.hell-guard/single_hell_guard_hammer.decl","size":327,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapaiencounter/mods/local.hell-guard/single_hell_guard_staff.decl","size":325,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapaiscore/mods/local.hell-guard/hell_guard_combo.decl","size":79,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapaiscore/mods/local.hell-guard/hell_guard_hammer.decl","size":79,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapaiscore/mods/local.hell-guard/hell_guard_staff.decl","size":79,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_combo_enc.decl","size":1284,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_hammer_enc.decl","size":1289,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_staff_enc.decl","size":1284,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/oliviasguard/melee_back.decl","size":302,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/oliviasguard/melee_front.decl","size":388,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/oliviasguard/abaddon.decl","size":610,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/oliviasguard/abaddon_combo.decl","size":617,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/oliviasguard/apollyon.decl","size":807,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/oliviasguard/apollyon_beam.decl","size":1529,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/oliviasguard/apollyon_combo.decl","size":889,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/oliviasguard/apollyon_combo_beam.decl","size":1603,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/talismanguard/hammer.decl","size":727,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/talismanguard/hammer_combo.decl","size":661,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/talismanguard/staff.decl","size":922,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/talismanguard/staff_beam.decl","size":1814,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/talismanguard/staff_combo.decl","size":1004,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/weapon/mods/local.hell-guard/weapon/zion/ai/talismanguard/staff_combo_beam.decl","size":1588,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/image/mods/local.hell_guard/textures/guis/snapmaps/entity_icons/demons/hell_guard_enc.bimage","size":65598,"kind":"binary","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/charge_attack.bmd6anim","size":23651,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/charge_attack_taunt.bmd6anim","size":29011,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/charge_leapattack.bmd6anim","size":26611,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/groundpound_miss_throw_lefthand.bmd6anim","size":29187,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/groundpound_miss_throw_righthand.bmd6anim","size":27811,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/meleeforward.bmd6anim","size":27699,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/meleeforward2.bmd6anim","size":66819,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/meleeforward2_1.bmd6anim","size":14931,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/meleeforward2_2.bmd6anim","size":15795,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/meleeforward2_3.bmd6anim","size":20547,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/throw.bmd6anim","size":20483,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/characters/monsters/hellknight/base/motion/combat/throw_double1_walking.bmd6anim","size":39059,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/model/mods/local.hell_guard/models/ai/tg_wall_max.bmodel","size":11664,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/cooked/model/mods/local.hell_guard/models/effects/misc/siphon_grenade_volume.bmodel","size":18908,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/characters/monsters/hellknight/gore/death/assets/mesh/death1_jaw.bmd6model","size":66044,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/characters/monsters/hellknight/gore/death/assets/mesh/death2_fullbody.bmd6model","size":885074,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/mods/local.hell_guard/characters/monsters/oliviasguard/base/assets/mesh/oliviasguard.bmd6model","size":734423,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/mods/local.hell_guard/objects/weapons/melee/hammer/assets/mesh/hammer.bmd6model","size":14138,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/mods/local.hell_guard/objects/weapons/melee/staff/assets/mesh/staff.bmd6model","size":11375,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/mods/local.hell_guard/snapmap_plus/hell_guard/talisman_guard.bmd6model","size":2684539,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/objects/weapons/melee/hammer/assets/mesh/tghammer.bmd6model","size":179706,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/objects/weapons/melee/staff/assets/mesh/tgstaff.bmd6model","size":200017,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/cm/md6/characters/monsters/talisman_guard/base/assets/collision/killfront_chest_punches.bcm","size":2389,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/cm/md6/characters/monsters/talisman_guard/base/assets/collision/killfront_chest_slug.bcm","size":2181,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aibehavior/mods/local.hell-guard/behaviors/zion/oliviasguard/default.decl","size":1515,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aicomponent_pathmanager/mods/local.hell-guard/aicomponent/pathmanager/oliviasguard.decl","size":116,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aicomponent_positionawareness/mods/local.hell-guard/aicomponent/positionawareness/oliviasguard/base.decl","size":1013,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aimovementgraph/mods/local.hell-guard/talismanguard.decl","size":3959,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aipositioningparms/mods/local.hell-guard/oliviasguard/default.decl","size":363,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aisensorysettings/default.decl","size":21,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aithreatmanagement/mods/local.hell-guard/threat/oliviasguard.decl","size":171,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aivignettegraph/mods/local.hell-guard/oliviasguard/weaponswap.decl","size":10622,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/ammo/mods/local.hell-guard/ammo/zion/ai/oliviasguard/apollyon.decl","size":83,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/zion/characters/monsters/oliviasguard.decl","size":314294,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/animweb/zion/characters/monsters/talismanguard.decl","size":121329,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/articulatedfigure/mods/local.hell-guard/zion/characters/monsters/attachments/hellknight/death2_fullbody_auto.decl","size":18340,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/articulatedfigure/mods/local.hell-guard/zion/characters/monsters/hellknight.decl","size":12752,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/attackgraph/mods/local.hell-guard/ai/oliviasguard.decl","size":81518,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/abaddon_nodamage.decl","size":225,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/apollyon_beam.decl","size":741,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/dual_columns.decl","size":591,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/dual_melee_blast.decl","size":446,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/duo_melee_blast.decl","size":445,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/melee_sync.decl","size":412,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/old_apollyon_combo_beam.decl","size":265,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/old_apollyon_rush.decl","size":367,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/old_dual_boomerang.decl","size":467,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/damage/mods/local.hell-guard/damage/zion/ai/oliviasguard/suicide.decl","size":519,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydamage/mods/local.hell-guard/zion/boss/talisman_guard.decl","size":870,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/local.hell-guard/ai/demon/hellknight/attachments/death2_fullbody1.decl","size":243,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/creatures/talisman/flamethrower.decl","size":1517,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/fx/mods/local.hell-guard/fx/gore/talisman_guard.decl","size":9049,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_back.decl","size":1144,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_chest_left.decl","size":1164,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_chest_right.decl","size":1168,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_left_arm.decl","size":1164,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_left_leg.decl","size":1160,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_left_shoulder.decl","size":1267,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_right_arm.decl","size":1169,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_right_leg.decl","size":1165,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/blood_right_shoulder.decl","size":1178,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_back.decl","size":1068,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_chest_left.decl","size":1075,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_chest_right.decl","size":1078,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_left_arm.decl","size":1076,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_left_leg.decl","size":1076,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_left_shoulder.decl","size":1087,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_right_arm.decl","size":1078,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_right_leg.decl","size":1078,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/gorewounds/mods/local.hell-guard/wounds/talismanguard/spray_right_shoulder.decl","size":1088,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/material/models/monsters/oliviasguard/oliviasguard_hammer.decl","size":1380,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/material/models/monsters/oliviasguard/oliviasguard_staff.decl","size":1374,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/material/mods/local.hell-guard/textures/common/clipping/clip_tg_shield.decl","size":784,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/material/mods/local.hell-guard/textures/ribbons/beam.decl","size":757,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/material/textures/effects/models/misc/tg_firewall.decl","size":939,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/characters/monsters/oliviasguard/gore/head.md6.decl","size":231,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/local.hell-guard/zion/characters/monsters/oliviasguard/gore/headremoved.md6.decl","size":245,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/zion/characters/monsters/hellknight/gore/death/death1_jaw.md6.decl","size":716,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/zion/characters/monsters/hellknight/gore/death/death2_fullbody.md6.decl","size":10238,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/zion/characters/monsters/hellknight/gore/death/death2_fullbody1.md6.decl","size":2542,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/zion/objects/weapons/melee/tghammer.md6.decl","size":436,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/zion/objects/weapons/melee/tgstaff.md6.decl","size":879,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/zion/objects/weapons/melee/tgstaff_signature.md6.decl","size":473,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/hellknight/smash_attack_hand.decl","size":7631,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/hammer_idle.decl","size":8749,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/hammer_slam_explosion_large.decl","size":17984,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/hammer_waveblast.decl","size":15941,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/hammer_waveblast_large.decl","size":15978,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/staff_idle_1.decl","size":9289,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/staff_idle_2.decl","size":9039,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/creature/talisman/wall_fire.decl","size":12661,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/particle/mods/local.hell-guard/impacts/talisman_footstep.decl","size":3152,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/prophealth/mods/local.hell-guard/health/boss_100pct.decl","size":408,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/heavy_rifle_5.decl","size":77,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/plasma_rifle_10.decl","size":79,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/rocket_launcher_full.decl","size":130,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/propitem/mods/local.hell-guard/ammo/sp/shotgun_full.decl","size":123,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/propmoveable/mods/local.hell-guard/pinata/boss_seek.decl","size":153,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/ribbon/mods/local.hell-guard/oliviasguard/connection.decl","size":231,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/mods/local.hell-guard/play_shield_off.decl","size":38,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/monster/hellknight/attacks/arm_flareup.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/monster/hellknight/hellknight_death_glory.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/mp/footsteps/ai_humanoid/mp_ai_wade_shallow.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_olivia_guard_shields_off.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_olivia_guard_shields_off_2.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_olivia_guard_shields_on.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_olivia_guard_shields_on_2.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_sfx_falter_impact.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_sfx_oliviasguard_bigshockwave.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_sfx_oliviasguard_boulder_summon_01.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_sfx_rockscoop.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_twins_footstep_base_01.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/sound/play_twins_footstep_metal_01.decl","size":38,"kind":"text","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/back.decl","size":165,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/death_front.decl","size":284,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/death_front_combo.decl","size":552,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/death_front_crushchest.decl","size":524,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/death_front_openchest.decl","size":522,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/death_left.decl","size":171,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/syncinteractions/mods/local.hell-guard/syncdeath/playervsai/talismanguard/death_right.decl","size":172,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/targeting/mods/local.hell-guard/characters/oliviasguard.decl","size":1792,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/targeting/mods/local.hell-guard/characters/talismanguard.decl","size":1971,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/twitchpain/mods/local.hell-guard/twitch/zion/oliviasguard.decl","size":17487,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/image/textures/effects/misc/beam_01.bimage","size":43790,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/image/textures/effects/misc/beam_02.bimage","size":43790,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/image/textures/effects/particles/liquid/blood_fall.bimage","size":87558,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/model/md6/mods/local.hell_guard/characters/monsters/talisman_guard/base/assets/collision/killfront_chest_punches.bmodel","size":3894,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/model/md6/mods/local.hell_guard/characters/monsters/talisman_guard/base/assets/collision/killfront_chest_slug.bmodel","size":3750,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/characters/monsters/hellknight/gore/death/assets/mesh/death1_jaw.bmd6skl","size":11409,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/characters/monsters/hellknight/gore/death/assets/mesh/death2_fullbody.bmd6skl","size":10546,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/characters/monsters/oliviasguard/base/assets/mesh/oliviasguard.bmd6skl","size":11470,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/objects/weapons/melee/hammer/assets/mesh/hammer.bmd6skl","size":878,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/objects/weapons/melee/hammer/assets/mesh/tghammer.bmd6skl","size":878,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/objects/weapons/melee/staff/assets/mesh/staff.bmd6skl","size":878,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/generated/skeleton/md6/objects/weapons/melee/staff/assets/mesh/tgstaff.bmd6skl","size":892,"kind":"binary","class":"surface","game":true,"container":"gameresources","inFolder":true,"cause":""}],"texts":{"mod.json":"{\r\n  \"id\": \"local.hell-guard\",\r\n  \"name\": \"Hell Guards\",\r\n  \"version\": \"0.0.0\",\r\n  \"requirements\": {\r\n    \"cvars\": {\r\n      \"g_useImageBlackList\": \"0\",\r\n      \"g_useResourceBlackList\": \"0\"\r\n    }\r\n  },\r\n  \"description\": \"Campaign staff, hammer and combined Hell Guard resources.\",\r\n  \"strings\": {\r\n    \"en\": {\r\n      \"snapmapplus_hell_guard_combo_name\": \"Hell Guard (Combined)\",\r\n      \"snapmapplus_hell_guard_combo_toybox_desc\": \"Combined-weapon Hell Guard. Wields both staff and hammer in the campaign boss fight.\",\r\n      \"snapmapplus_hell_guard_hammer_name\": \"Hell Guard (Hammer)\",\r\n      \"snapmapplus_hell_guard_hammer_toybox_desc\": \"Hammer-wielding Hell Guard. Uses heavy melee strikes and leaping ground slams.\",\r\n      \"snapmapplus_hell_guard_staff_name\": \"Hell Guard (Staff)\",\r\n      \"snapmapplus_hell_guard_staff_toybox_desc\": \"Staff-wielding Hell Guard. Uses sweeping melee strikes and ranged energy attacks.\"\r\n    }\r\n  }\r\n}\r\n","assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_hammer_enc.decl":"{\n\tinherit = \"demons/demon_base\";\n\tedit = {\n\t\tnavMeshType = \"NAVMESH_TYPE_128\";\n\t\tpreviewImage = \"mods/local.hell-guard/textures/guis/snapmaps/entity_icons/demons/hell_guard_enc\";\n\t\tdisplayNameTag = \"#str_snapmapplus_hell_guard_hammer_name\";\n\t\tdisplayDescriptionTag = \"#str_snapmapplus_hell_guard_hammer_toybox_desc\";\n\t\tsortPriority = 294;\n\t\tentityDef = \"mods/local.hell-guard/snapmaps/placeablesnapaiencounter/hell_guard_hammer\";\n\t\trenderInfo = {\n\t\t\trenderModelName = \"mods/local.hell-guard/zion/characters/monsters/talisman_guard/base/talisman_guard_snapeditor.md6\";\n\t\t\talwaysDrawBounds = false;\n\t\t\talwaysDrawLabel = false;\n\t\t\tsize = {\n\t\t\t\tx = 185;\n\t\t\t\ty = 185;\n\t\t\t\tz = 256;\n\t\t\t}\n\t\t}\n\t\teventInfo = {\n\t\t\tanimWebDecl = \"mods/local.hell-guard/zion/snapmap/demons/hell_guard_hammer_snapeditor\";\n\t\t\tselectedEvent = {\n\t\t\t\tsound = \"Play_PickUp_Baron\";\n\t\t\t}\n\t\t\tplacedEvent = {\n\t\t\t\tsound = \"Play_PutDown_Baron\";\n\t\t\t}\n\t\t}\n\t\tfxInfo = {\n\t\t\tfxDecl = \"fx/creatures/baron\";\n\t\t}\n\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\titem[4] = {\n\t\t\t\t\t\tdescriptionTag = \"#str_snapproperty_snapAISpawnBehavior_melee_desc\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t\tflags = {\n\t\t\tplaceable = true;\n\t\t}\n\t\tbuildGameRefEntityDefs = {\n\t\t\tnum = 1;\n\t\t\titem[0] = \"mods/local.hell-guard/ai/demon/talismanguard/hammer\";\n\t\t}\n\t}\n}\n","assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_staff_enc.decl":"{\n\tinherit = \"demons/demon_base\";\n\tedit = {\n\t\tnavMeshType = \"NAVMESH_TYPE_128\";\n\t\tpreviewImage = \"mods/local.hell-guard/textures/guis/snapmaps/entity_icons/demons/hell_guard_enc\";\n\t\tdisplayNameTag = \"#str_snapmapplus_hell_guard_staff_name\";\n\t\tdisplayDescriptionTag = \"#str_snapmapplus_hell_guard_staff_toybox_desc\";\n\t\tsortPriority = 293;\n\t\tentityDef = \"mods/local.hell-guard/snapmaps/placeablesnapaiencounter/hell_guard_staff\";\n\t\trenderInfo = {\n\t\t\trenderModelName = \"mods/local.hell-guard/zion/characters/monsters/talisman_guard/base/talisman_guard_snapeditor.md6\";\n\t\t\talwaysDrawBounds = false;\n\t\t\talwaysDrawLabel = false;\n\t\t\tsize = {\n\t\t\t\tx = 185;\n\t\t\t\ty = 185;\n\t\t\t\tz = 256;\n\t\t\t}\n\t\t}\n\t\teventInfo = {\n\t\t\tanimWebDecl = \"mods/local.hell-guard/zion/snapmap/demons/hell_guard_staff_snapeditor\";\n\t\t\tselectedEvent = {\n\t\t\t\tsound = \"Play_PickUp_Baron\";\n\t\t\t}\n\t\t\tplacedEvent = {\n\t\t\t\tsound = \"Play_PutDown_Baron\";\n\t\t\t}\n\t\t}\n\t\tfxInfo = {\n\t\t\tfxDecl = \"fx/creatures/baron\";\n\t\t}\n\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\titem[4] = {\n\t\t\t\t\t\tdescriptionTag = \"#str_snapproperty_snapAISpawnBehavior_melee_desc\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t\tflags = {\n\t\t\tplaceable = true;\n\t\t}\n\t\tbuildGameRefEntityDefs = {\n\t\t\tnum = 1;\n\t\t\titem[0] = \"mods/local.hell-guard/ai/demon/talismanguard/staff\";\n\t\t}\n\t}\n}\n","assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_combo_enc.decl":"{\n\tinherit = \"demons/demon_base\";\n\tedit = {\n\t\tnavMeshType = \"NAVMESH_TYPE_128\";\n\t\tpreviewImage = \"mods/local.hell-guard/textures/guis/snapmaps/entity_icons/demons/hell_guard_enc\";\n\t\tdisplayNameTag = \"#str_snapmapplus_hell_guard_combo_name\";\n\t\tdisplayDescriptionTag = \"#str_snapmapplus_hell_guard_combo_toybox_desc\";\n\t\tsortPriority = 295;\n\t\tentityDef = \"mods/local.hell-guard/snapmaps/placeablesnapaiencounter/hell_guard_combo\";\n\t\trenderInfo = {\n\t\t\trenderModelName = \"mods/local.hell-guard/zion/characters/monsters/talisman_guard/base/talisman_guard_snapeditor.md6\";\n\t\t\talwaysDrawBounds = false;\n\t\t\talwaysDrawLabel = false;\n\t\t\tsize = {\n\t\t\t\tx = 185;\n\t\t\t\ty = 185;\n\t\t\t\tz = 256;\n\t\t\t}\n\t\t}\n\t\teventInfo = {\n\t\t\tanimWebDecl = \"mods/local.hell-guard/zion/snapmap/demons/hell_guard_combo_snapeditor\";\n\t\t\tselectedEvent = {\n\t\t\t\tsound = \"Play_PickUp_Baron\";\n\t\t\t}\n\t\t\tplacedEvent = {\n\t\t\t\tsound = \"Play_PutDown_Baron\";\n\t\t\t}\n\t\t}\n\t\tfxInfo = {\n\t\t\tfxDecl = \"fx/creatures/baron\";\n\t\t}\n\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\titem[4] = {\n\t\t\t\t\t\tdescriptionTag = \"#str_snapproperty_snapAISpawnBehavior_melee_desc\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t\tflags = {\n\t\t\tplaceable = true;\n\t\t}\n\t\tbuildGameRefEntityDefs = {\n\t\t\tnum = 1;\n\t\t\titem[0] = \"mods/local.hell-guard/ai/demon/talismanguard/combo\";\n\t\t}\n\t}\n}\n"}}};
var previewMods = modsTab.standIn(PREVIEW_MODS, PREVIEW_MOD_FILES);
function previewHandle(o) {
  if (previewConfigHandle(o)) return;
  if (o.cmd === 'modsList') { onMessage({kind:'mods', data: PREVIEW_MODS}); return; }
  if (o.cmd === 'modsAction') {
    // As the host: the action's result, then a fresh document, from the stand-in store in
    // mods_tab.js, which changes its copy as the backend would (Rebuild, Verify, the dry runs,
    // Remove from map and Uninstall all work here; Install and Migrate do not).
    previewMods(o).forEach(onMessage);
    return;
  }
  // The File menu in browser preview: no native picker and no backend, so answer with the default
  // pair the backend would report. Without this the menu's readout would sit on "Loading..." and
  // the layout could never be checked outside the game.
  if (o.cmd === 'rawmapStatus' || o.cmd === 'rawmapLoadPick' || o.cmd === 'rawmapSavePick' ||
      o.cmd === 'rawmapSaveNow' || o.cmd === 'rawmapKeepHere' || o.cmd === 'rawmapLoadNow') {
    // Ticking pins the path already shown, so the readout does not move on the click.
    if (o.cmd === 'rawmapKeepHere') previewRawmap.saveTarget = o.on ? previewRawmap.save : '';
    // Opening a rawmap makes "Open ... Again" live, as in the game.
    if (o.cmd === 'rawmapLoadNow') previewRawmap.loads = (previewRawmap.loads || 0) + 1;
    var msg = {kind:'rawmapStatus', ok:true, status:{
      load: previewRawmap.load,
      save: previewRawmap.save,
      saveTarget: previewRawmap.saveTarget || '',
      saves: 0, lastBytes: 0, loads: previewRawmap.loads || 0
    }, note: o.cmd === 'rawmapStatus' ? '' : 'Preview only - no backend attached.'};
    // A pick asks before opening, as the backend's checked file does.
    if (o.cmd === 'rawmapLoadPick') msg.confirmLoad = rawmapBaseName(previewRawmap.load);
    onMessage(msg);
    return;
  }
  if (o.cmd === 'refresh') {
    onMessage(previewList());
    onMessage({kind:'camera', x:797.638489, y:183.456985, z:-950.475159});
  }
  else if (o.cmd === 'openTimeline') {
    var tj = previewTimelineJson[o.eid];
    onMessage(tj ? {kind:'timelineData', eid:o.eid, ok:true, json:tj} : {kind:'timelineData', eid:o.eid, ok:false});
  }
  else if (o.cmd === 'reportSubmit') {
    // preview only: prove the payload is well-formed JSON, then fake the round-trip (created, then a
    // dedup "appended" on an identical re-send) so both green-toast paths are demoable in a browser.
    var rOk = false, rp = null;
    try { rp = JSON.parse(o.payload); rOk = !!(rp && rp.category && rp.title && rp.body); } catch (e) { rOk = false; }
    var rSig = rOk ? (rp.category + '|' + rp.title.toLowerCase()) : '';
    var rMode = (rOk && previewReportSigs[rSig]) ? 'appended' : 'created';
    if (rOk) previewReportSigs[rSig] = true;
    setTimeout(function(){ onMessage({kind:'reportResult', ok:rOk, mode:rMode, number:42}); }, 600);
  }
  else if (o.cmd === 'crashSubmit') {
    // preview only: prove the crash submit carries the required fields, then fake the round-trip
    // (same dedup memory as the feedback demo, keyed on the auto-title's crash signature).
    var cOk = !!(o.title && o.body);
    var cSig = cOk ? ('crash|' + o.title.toLowerCase()) : '';
    var cMode = (cOk && previewReportSigs[cSig]) ? 'appended' : 'created';
    if (cOk) previewReportSigs[cSig] = true;
    setTimeout(function(){ onMessage({kind:'reportResult', ok:cOk, mode:cMode, number:43}); }, 600);
  }
  // Browser catalog samples cover materials, sounds, and soundbank grouping.
  // Other types return empty because no sample is embedded for them.
  else if (o.cmd === 'listAssets') {
    var sample = '';
    if (o.assetKind === 0)       sample = PREVIEW_MATERIALS;   /* SH_ASSET_MATERIAL */
    else if (o.assetKind === 3)  sample = PREVIEW_SOUNDS;      /* SH_ASSET_SOUND    */
    else if (o.assetKind === 13) sample = PREVIEW_SNDBANKS;    /* SH_ASSET_SNDBANK  */
    onMessage({kind:'assetList', assetKind: o.assetKind, names: sample});
  }
  /* Browser-preview pins last for this session only. */
  else if (o.cmd === 'pinsLoad') { onMessage({kind:'pins', doc: previewPinsDoc}); }
  else if (o.cmd === 'pinsSave') { previewPinsDoc = o.doc || ''; }
  // No decoder outside the game, so the preview honestly reports that rather than faking pixels.
  else if (o.cmd === 'requestPreview') { onMessage({kind:'previewRequested', ok:false, name:o.name,
    note:'Previews only decode in-game - there is no codec on the browser side.'}); }
  else if (o.cmd === 'cancelPreview')  { /* preview: no producer or retained image to cancel */ }
  else if (o.cmd === 'getPreview')     { onMessage({kind:'previewImage', ok:false, name:o.name}); }
  else if (o.cmd === 'findMaterial')   { onMessage({kind:'materialResult', found:false, name:o.name}); }
  // A real rect, so the browser-preview build can exercise the virtualmapping patch end to end:
  // material/snapmap/dynamic_block_textured_hell_cave_facade's actual .vmtr row.
  else if (o.cmd === 'newEntity') {
    // preview only: no engine to stage into, so just prove the authored prefab parses.
    var neOk = false, neParsed = null; try { neParsed = JSON.parse(o.json); neOk = !!neParsed; } catch (e) { neOk = false; }
    onMessage({kind:'newEntityResult', result: neOk ? 1 : -1, label: o.label});
    if (neOk && o.label === 'Timeline') {   // the mock "places" it at once
      var neEid = 200 + previewTimelines.length, neName = (neParsed.entities && neParsed.entities[0] && neParsed.entities[0].displayName) || 'New Timeline';
      previewTimelines.push({eid: neEid, id: '0_ind_totally_blank_room_4x/snapmaps/unknown_' + neEid, name: neName});
      setTimeout(function(){ onMessage(previewList()); }, 50);
    }
  }
  else if (o.cmd === 'setDisplayNames') {
    for (var sdi = 0; sdi < (o.eids || []).length; sdi++) {
      for (var sdt = 0; sdt < previewTimelines.length; sdt++) if (previewTimelines[sdt].eid === o.eids[sdi]) previewTimelines[sdt].name = o.names[sdi];
      for (var sde = 0; sde < previewEnts.length; sde++) if (previewEnts[sde].eid === o.eids[sdi]) previewEnts[sde].name = o.names[sdi];
    }
    onMessage({kind:'setDisplayNamesResult', applied: (o.eids || []).length, failed: 0});
    setTimeout(function(){ onMessage(previewList()); }, 50);
  }
  else if (o.cmd === 'materialRect')   { onMessage({kind:'materialRect', name:o.name, ok:true,
                                                    x:88320, y:138240, w:3840, h:3840}); }
  else if (o.cmd === 'crashDismiss') { /* preview: nothing to clear */ }
  else if (o.cmd === 'saveTimeline') {
    // preview only: no real backend to commit to -- just prove the patched JSON is well-formed and
    // contains what we expect, then fake success. Real validation happens in-game against the real engine.
    var ok = false;
    try { var doc = JSON.parse(o.json); ok = !!(doc && doc.entityDef && doc.entityDef.state && doc.entityDef.state.edit); } catch (e) { ok = false; }
    onMessage({kind:'saveTimelineResult', eid:o.eid, result: ok ? 1 : 0});
  }
  else if (o.cmd === 'select') { var s = previewStates[o.eid] || {decl:'(no sample decl for this entity)', classname:'', inherit:'', displayname:''}; onMessage({kind:'state', auto:false, eid:o.eid, ok:true, decl:s.decl, classname:s.classname, inherit:s.inherit, displayname:s.displayname}); }
  else if (o.cmd === 'save') {
    // preview only: fake the backend's validation so all three toast colors are demoable.
    // empty classname -> failed (red); a non-"id" classname -> refused (amber); else saved (green).
    var res = !o.classname ? -2 : (o.classname.indexOf('id') !== 0 ? 0 : 1);
    if (res === 1) {
      previewStates[o.eid] = {decl:o.decl, classname:o.classname, inherit:o.inherit, displayname:o.displayname};
      for (var i=0;i<previewEnts.length;i++) if (previewEnts[i].eid===o.eid) previewEnts[i].name=o.displayname;
      onMessage({kind:'saveResult', result:1}); onMessage(previewList());
      onMessage({kind:'state', auto:false, eid:o.eid, ok:true, decl:o.decl, classname:o.classname, inherit:o.inherit, displayname:o.displayname});
    } else {
      onMessage({kind:'saveResult', result:res});   // refused/failed: leave the edit dirty so you can fix it
    }
  }
  else if (o.cmd === 'delete') { previewEnts = previewEnts.filter(function(e){ return o.eids.indexOf(e.eid) < 0; }); onMessage(previewList()); }
  else if (o.cmd === 'listPrefabs') { previewSendPrefabs(); onMessage({kind:'editorMode', entityMode: 1, blueprint: 0}); }
  else if (o.cmd === 'createModulePrefab') {
    if (previewPrefabs.root.indexOf(o.name) !== -1) { onMessage({kind:'createModulePrefabResult', result:0, name:o.name}); return; }
    previewPrefabs.root.push(o.name);
    var cmSummary = {kind: 'modules', moduleCount: 1, entityCount: 3, doorCount: 0, variableCount: 0, droppedWires: 0, droppedRefs: 0, hasNavigationBoxes: false, environments: [],
                     bounds: {min: [-512, -512, 0], max: [512, 512, 768]}, modules: [{name: '0_ind_totally_blank_room_4x', label: '', origin: [0,0,0], rotation: 0, size: [1024,1024,768], bounds: {min: [-512,-512,0], max: [512,512,768]}}]};
    previewMetaBodies[o.name] = JSON.stringify(cmSummary);
    previewPrefabDetails[o.name] = {count: 3, scene: ''};
    onMessage({kind:'createModulePrefabResult', result:1, name:o.name, summary: cmSummary});
    previewSendPrefabs();
  }
  else if (o.cmd === 'budgetTable' || o.cmd === 'budgetUsage') {
    // A two-row stand-in table: enough to lay out the Limits section in a browser.
    var pbLimits = [768,14284,10384,2038,672,1000,8192,8192,0,0,500000,64], pbUsage = [32,67,91,1,30,0,1,0,1552,12,2545,0];
    if (o.cmd === 'budgetUsage') onMessage({kind:'budgetUsage', data:{limits:pbLimits, usage:pbUsage, reached:0, gen:1}});
    else onMessage({kind:'budgetTable', data:{gen:1, limits:pbLimits, usage:pbUsage, reached:0,
      layers:{autoView:131072, sound:4194304, source:'decl'},
      sedefs:{'volume/blocking':{l:[0,1,1,0,1,0,0,0,0,0,10,0], auto:0, ns:1}, 'prop/new/crate01':{l:[1,1,1,0,1,0,0,0,0,0,25,0], auto:0, ns:0}},
      inherits:{'snapmaps/volume/blocking':'volume/blocking', 'snapmaps/props/new/crate01':'prop/new/crate01'},
      modules:{'maps/modules/ind_dlc/ind_totally_blank_room_4x.decl':{own:[0,0,21,0,0,0,0,0,1552,12,50,0], templ:[0,2,0,0,0,0,0,0,0,0,400,0], view:[0,3,6,1,1,0,0,0,0,0,150,0], mask:131073}},
      aliases:{'ind_totally_blank_room_4x':'maps/modules/ind_dlc/ind_totally_blank_room_4x.decl'}}});
  }
  else if (o.cmd === 'selectPrefab') {
    var d = previewPrefabDetails[o.name];
    var mk = (o.folder ? o.folder + '/' : '') + o.name;
    onMessage(d ? {kind:'prefabDetail', name:o.name, ok:true, meta: previewMetaBodies[mk] || '', scene:d.scene || '', count:d.count,
                   prefabKind: previewKindsMap()[mk] === 'modules' ? 'modules' : 'objects'}
                : {kind:'prefabDetail', name:o.name, ok:false});
  }
  else if (o.cmd === 'resolvePrefabModel') {
    onMessage({kind:'prefabModelResolved',generation:o.generation,inherit:o.inherit,model:'',scale:[1,1,1]});
  }
  else if (o.cmd === 'requestPrefabMesh') { /* preview scenes deliberately use transform proxies only */ }
  else if (o.cmd === 'savePrefabMeta') {
    var smk = (o.folder ? o.folder + '/' : '') + o.name;
    if (o.body) previewMetaBodies[smk] = o.body; else delete previewMetaBodies[smk];
    onMessage({kind:'savePrefabMetaResult', result:1, name:o.name});
    previewSendPrefabs();
  }
  else if (o.cmd === 'createPrefab') {
    if (editorSelCount <= 0) { onMessage({kind:'createPrefabResult', result:0, name:o.name}); return; }
    if (previewPrefabs.root.indexOf(o.name) === -1) previewPrefabs.root.push(o.name);
    previewPrefabDetails[o.name] = {count: editorSelCount, scene: previewPrefabScene([])};
    onMessage({kind:'createPrefabResult', result:1, name:o.name});
    previewSendPrefabs();
  }
  else if (o.cmd === 'exportPrefab') {
    onMessage({kind:'prefabFileResult', op:'export', ok:true, message:'Exported prefab ' + o.name + ' to ' + o.name + '.zip; it uses no mods.'});
  }
  else if (o.cmd === 'importPrefab') {
    onMessage({kind:'prefabFileResult', op:'import', ok:false, message:'Import needs the game: the file dialog opens there.'});
  }
  else if (o.cmd === 'deletePrefab') {
    var list = previewFolderList(o.folder || '');
    var di = list ? list.indexOf(o.name) : -1;
    if (di === -1) { onMessage({kind:'deletePrefabResult', result:0, name:o.name}); return; }
    list.splice(di, 1); delete previewPrefabDetails[o.name];
    onMessage({kind:'deletePrefabResult', result:1, name:o.name});
    previewSendPrefabs();
  }
  else if (o.cmd === 'renamePrefab') {
    var rlist = previewFolderList(o.folder || '');
    var ri = rlist ? rlist.indexOf(o.oldName) : -1;
    if (ri === -1 || rlist.indexOf(o.newName) !== -1) { onMessage({kind:'renamePrefabResult', result:0, oldName:o.oldName, newName:o.newName}); return; }
    rlist[ri] = o.newName;
    previewPrefabDetails[o.newName] = previewPrefabDetails[o.oldName]; delete previewPrefabDetails[o.oldName];
    onMessage({kind:'renamePrefabResult', result:1, oldName:o.oldName, newName:o.newName});
    previewSendPrefabs();
  }
  else if (o.cmd === 'createFolder') {
    if (previewFolderIndex(o.name) !== -1) { onMessage({kind:'createFolderResult', result:0, name:o.name}); return; }
    previewPrefabs.folders.push({name:o.name, items:[]});
    onMessage({kind:'createFolderResult', result:1, name:o.name});
    previewSendPrefabs();
  }
  else if (o.cmd === 'renameFolder') {
    var fidx = previewFolderIndex(o.oldName);
    if (fidx === -1 || previewFolderIndex(o.newName) !== -1) { onMessage({kind:'renameFolderResult', result:0, oldName:o.oldName, newName:o.newName}); return; }
    previewPrefabs.folders[fidx].name = o.newName;
    onMessage({kind:'renameFolderResult', result:1, oldName:o.oldName, newName:o.newName});
    previewSendPrefabs();
  }
  else if (o.cmd === 'deleteFolder') {
    var didx = previewFolderIndex(o.name);
    if (didx === -1) { onMessage({kind:'deleteFolderResult', result:0, name:o.name}); return; }
    var moved = previewPrefabs.folders[didx].items;
    for (var mi = 0; mi < moved.length; mi++) previewPrefabs.root.push(moved[mi]);
    previewPrefabs.folders.splice(didx, 1);
    onMessage({kind:'deleteFolderResult', result:1, name:o.name});
    previewSendPrefabs();
  }
  else if (o.cmd === 'movePrefabToFolder') {
    var fromList = previewFolderList(o.fromFolder || '');
    var toList = previewFolderList(o.toFolder || '');
    var fi = fromList ? fromList.indexOf(o.name) : -1;
    if (fi === -1 || !toList || toList.indexOf(o.name) !== -1) { onMessage({kind:'movePrefabResult', result:0, name:o.name, toFolder:o.toFolder}); return; }
    fromList.splice(fi, 1); toList.push(o.name);
    onMessage({kind:'movePrefabResult', result:1, name:o.name, toFolder:o.toFolder});
    previewSendPrefabs();
  }
  else if (o.cmd === 'movePrefabs') {
    var mvMoved = [], mvFailed = 0;
    for (var mvi = 0; mvi < (o.names || []).length; mvi++) {
      var mvFrom = previewFolderList(o.fromFolders[mvi] || ''), mvTo = previewFolderList(o.toFolder || '');
      var mvIdx = mvFrom ? mvFrom.indexOf(o.names[mvi]) : -1;
      if (mvIdx === -1 || !mvTo || mvTo.indexOf(o.names[mvi]) !== -1) { mvFailed++; continue; }
      mvFrom.splice(mvIdx, 1); mvTo.push(o.names[mvi]); mvMoved.push(o.names[mvi]);
    }
    onMessage({kind:'movePrefabsResult', moved: mvMoved, failed: mvFailed, toFolder: o.toFolder});
    previewSendPrefabs();
  }
  else if (o.cmd === 'deletePrefabs') {
    var dpDeleted = 0, dpFailed = 0;
    for (var dpi = 0; dpi < (o.names || []).length; dpi++) {
      var dpList = previewFolderList(o.folders[dpi] || ''), dpIdx = dpList ? dpList.indexOf(o.names[dpi]) : -1;
      if (dpIdx === -1) { dpFailed++; continue; }
      dpList.splice(dpIdx, 1); delete previewPrefabDetails[o.names[dpi]]; dpDeleted++;
    }
    onMessage({kind:'deletePrefabsResult', deleted: dpDeleted, failed: dpFailed});
    previewSendPrefabs();
  }
  else if (o.cmd === 'pushStack') {
    var n = (o.eids || []).length;
    previewStack0Count += n;   // rough demo count, not dedup-aware -- good enough to make Clear feel real
    onMessage({kind:'pushStackResult', result: n > 0 ? 1 : 0, count: n});
  }
  else if (o.cmd === 'clearStack') {
    var had = previewStack0Count; previewStack0Count = 0;
    onMessage({kind:'clearStackResult', result:1, count: had});
  }
  else if (o.cmd === 'tlUseSelection') {
    // preview only: fake exactly one entity selected (Block Box, eid 65) to demo the success path.
    onMessage({kind:'tlUseSelectionResult', ok:true, eid:65});
  }
  else if (o.cmd === 'enumInherits') { onMessage({kind:'inherits', items:['snapmaps/volume/blocking','snapmaps/volume/trigger','player/coop','snapmaps/ai','snapmaps/variable','snapmaps/action/variable/integer','snapmaps/light','light','snapmaps/listener/ai']}); }
  else if (o.cmd === 'enumClasses') { onMessage({kind:'classes', inherit:o.inherit, items:['idVolume_Blocking','idVolume_Trigger','idVolume_Damage','idVolume_Water']}); }
  else if (o.cmd === 'lookupDesc') {
    onMessage({kind:'desc', inherit: previewDescs[o.inherit] || null, class: previewDescs[o.classname] || null});
  }
  else if (o.cmd === 'enumEvents') {
    // varied arg kinds on purpose -- exercises every tlClassifyArg branch in the browser demo without
    // needing the real 1611-event catalog. Every kind is now a real editable widget.
    onMessage({kind:'events', items:[
      {name:'ai/spawn', args:[{name:'origin', type:'vec3'}]},
      {name:'ai/set_target', args:[{name:'target', type:'entity'}]},
      {name:'ai/kill', args:[{name:'silent', type:'bool'}]},
      {name:'ai/heal', args:[{name:'amount', type:'float'}]},
      {name:'sound/play_shader', args:[{name:'shader', type:'idSoundShader*'}]},
      {name:'sound/stop_shader', args:[{name:'channel', type:'int'}]},
      {name:'door/open', args:[{name:'instant', type:'bool'}]},
      {name:'door/close', args:[{name:'delay', type:'float'}]},
      {name:'light/on', args:[{name:'color', type:'color'}]},
      {name:'light/off', args:[]},
      {name:'fx/play', args:[{name:'condition', type:'fxCondition_t'}]},
      {name:'anim/play', args:[{name:'facing', type:'angles'}, {name:'anim', type:'md6Anim'}]},
      {name:'model/set_index', args:[{name:'modelIndex', type:'int'}]}
    ]});
  }
  else if (o.cmd === 'enumEventDocs') {
    // Preview description cases: full documentation, missing argument text, and no document.
    onMessage({kind:'eventDocs', items:[
      {name:'ai/spawn', summary:'Spawns the AI actor into the world at the given origin.', args:[{name:'origin', desc:'World-space spawn position.'}]},
      {name:'ai/set_target', summary:'Sets the AI actor\'s current target entity.', args:[]},
      {name:'sound/play_shader', summary:'Plays a sound shader on this entity.', args:[]}
    ]});
  }
  else if (o.cmd === 'enumArgResclass') {
    // Preview declaration and enum choices; unmatched resource classes remain free text.
    var sample = {
      sound: ['alert_klaxon', 'door_open_heavy', 'ui_menu_select', 'demon_growl_01'],
      fxCondition_t: ['FX_ALWAYS', 'FX_ON_DEATH', 'FX_ON_HIT', 'FX_ON_SPAWN']
    };
    onMessage({kind:'argResclass', resClass:o.resClass, items: sample[o.resClass] || []});
  }
  else if (o.cmd === 'resolveEntityInherit') {
    // preview only: fake a serialize_entity round-trip -- eid 74 ("Heavy Gunner") resolves to the class
    // slug previewEntityAssets has data for, so the modelIndex/md6Anim asset dropdowns are demoable.
    var slugs = { 74: 'snapmaps/ai' };
    var slug = slugs[o.eid];
    var doc = { entityDef: { inherit: slug || '' } };
    onMessage({kind:'entityInherit', eid:o.eid, ok:true, json: JSON.stringify(doc)});
  }
  else if (o.cmd === 'enumEntityAssets') {
    onMessage({kind:'entityAssets', items:[
      { slug:'snapmaps/ai',
        models:    [{display:'heavy_gunner_v1', value:'0'}, {display:'heavy_gunner_v2_battledamage', value:'1'}],
        animWeb:   [],
        md6Anim:   [{display:'salute_v1', value:'md6/characters/humans/mp_male/motion/taunts/salute_v1.md6anim'}, {display:'idle', value:'md6/characters/humans/male/base/motion/shared/rifle/attention/idle.md6anim'}],
        animAlias: [],
        tagName:   [{display:'fx_head', value:'fx_head'}, {display:'left_hand_weapon_attach', value:'left_hand_weapon_attach'}]
      }
    ]});
  }
  else if (o.cmd === 'selectInEditor') { /* no 3D editor in browser preview */ }
  else if (o.cmd === 'camLock' || o.cmd === 'camSet') { /* no 3D editor in browser preview */ }
  else if (o.cmd === 'flyTo') { onMessage({kind:'flyToResult', result:1, eid:o.eid}); }
  else if (o.cmd === 'winState') { onMessage({kind:'windowState', maximized:windowMaximized}); }
  else if (o.cmd === 'winMax') { onMessage({kind:'windowState', maximized:!windowMaximized}); }
  else if (o.cmd === 'winMin' || o.cmd === 'winDrag' || o.cmd === 'winResize') { /* no native window in preview */ }
}
