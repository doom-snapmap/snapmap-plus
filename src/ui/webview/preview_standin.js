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
// Generated from tests/fixtures/mods_tab_payload_v4.json (mods_tab_test checks they match).
var PREVIEW_MODS = {"v":4,"gen":12,"root":"C:\\Users\\player\\AppData\\Local\\snapmap-plus\\mods","storeReadable":true,"mods":[{"key":"alex.r2-imp","salt":"alexrimp","name":"R2 Imp copy","author":"alex","size":1843230,"files":13,"state":"ready","cause":"","editorRows":2,"folder":"alex.r2-imp","group":"","place":"","placeTitle":"","description":"A copy of the Imp with its own entityDef, two editor rows and a louder fireball.","requirements":{"cvars":{}},"editorRowNames":["Imp (R2)","Imp (R2, caged)"],"publishedMaps":1,"facts":{"adds":13,"borrows":2,"surfaces":0,"editor":0,"replaces":0,"editorOnly":false,"editorTweaks":[],"replacements":[],"edits":[{"file":"generated/decls/entitydef/player/coop.decl","ops":3,"set":3,"remove":0,"list":0,"keyed":0,"bag":0,"drop":0},{"file":"generated/decls/aifsmmanager/states/imp.decl","ops":1,"set":0,"remove":0,"list":0,"keyed":1,"bag":0,"drop":0}],"registries":{"conductor":["mods/alexrimp/single_imp"]}},"borrows":{"paths":["models/monsters/imp/imp.lwo","generated/decls/material/models/monsters/imp/imp.decl"],"count":2},"warnings":[]},{"key":"Arena Gates","salt":"gatesk2m","name":"Arena Gates","author":"jo","size":412000,"files":6,"state":"ready","cause":"","editorRows":2,"folder":"Arena Gates","group":"","place":"","placeTitle":"","description":"Turns the two blacklists off so the gate textures load.","requirements":{"commands":{"activate":["echo gates on"],"deactivate":["echo gates off"]},"cvars":{"g_useImageBlackList":"0","g_useResourceBlackList":"0"}},"editorRowNames":["Gate (heavy)","Gate (light)"],"publishedMaps":0,"facts":{"adds":6,"borrows":0,"surfaces":0,"editor":0,"replaces":0,"editorOnly":false,"editorTweaks":[],"replacements":[],"edits":[],"registries":{"banks":["sfx_vega"]},"stockBanks":[{"bank":"sfx_vega","state":"loaded","ms":41,"result":1}]},"borrows":{"paths":[],"count":0},"warnings":["Its description is longer than the Toybox shows; the first 200 characters are shown."],"sameFilesAs":{"name":"Arena Gates (old)","folder":"Arena Gates (old)","count":5}},{"key":"Arena Gates (old)","salt":"gatesq7p","name":"Arena Gates (old)","author":"jo","size":401000,"files":6,"state":"ready","cause":"","editorRows":2,"folder":"Arena Gates (old)","group":"","place":"","placeTitle":"","description":"","requirements":{"cvars":{}},"editorRowNames":["Gate (heavy)","Gate (light)"],"publishedMaps":0,"facts":{"adds":6,"borrows":0,"surfaces":0,"editor":0,"replaces":0,"editorOnly":false,"editorTweaks":[],"replacements":[],"edits":[],"registries":{}},"borrows":{"paths":[],"count":0},"warnings":[],"sameFilesAs":{"name":"Arena Gates","folder":"Arena Gates","count":5}},{"key":"Broken Barrels","salt":"brokenb1","name":"Broken Barrels","author":"","size":0,"files":0,"state":"attention","cause":"This mod could not be built: assets/generated/decls/entitydef/mods/<mod id>/barrel.decl is not a well-formed decl (unbalanced braces, quotes or comments, or a NUL byte); fix that in its folder and press Refresh.","editorRows":0,"folder":"Broken Barrels","group":"","place":"","placeTitle":"","description":"Barrels that break into pieces.","requirements":{"cvars":{}},"editorRowNames":[],"publishedMaps":0,"warnings":[]},{"key":"Editor Grid","salt":"edgrid4t","name":"Editor Grid","author":"alex","size":2100,"files":1,"state":"ready","cause":"","editorRows":0,"folder":"Editor Grid","group":"","place":"","placeTitle":"","description":"A finer grid in the editor.","requirements":{"cvars":{}},"editorRowNames":[],"publishedMaps":0,"facts":{"adds":0,"borrows":0,"surfaces":0,"editor":1,"replaces":0,"editorOnly":true,"editorTweaks":["generated/decls/snapeditorentitydef/demons/demon_base.decl"],"replacements":[],"edits":[],"registries":{}},"borrows":{"paths":[],"count":0},"warnings":[]},{"key":"campaign-interactables\\doors","salt":"doorsx8q","name":"Doors","author":"","size":3801224,"files":40,"state":"ready","cause":"","editorRows":5,"folder":"doors","group":"campaign-interactables","place":"","placeTitle":"","description":"The campaign's doors as Toybox rows.","requirements":{"cvars":{}},"editorRowNames":["Blast door","Lift door","Door (UAC)","Door (Hell)","Door (lab)"],"publishedMaps":1,"facts":{"adds":40,"borrows":12,"surfaces":2,"editor":0,"replaces":0,"editorOnly":false,"editorTweaks":[],"replacements":[],"edits":[],"registries":{}},"borrows":{"paths":["models/mapobjects/doors/blast_door.lwo"],"count":14},"warnings":[]},{"key":"maps\\Hell Guard Arena\\Hell Guards","salt":"hellgd3k","name":"Hell Guards","author":"sam","size":1210441,"files":220,"state":"ready","cause":"","editorRows":3,"folder":"Hell Guards","group":"","place":"maps\\Hell Guard Arena","placeTitle":"Hell Guard Arena","description":"Three Hell Guards with hammer, shield and fist.","requirements":{"cvars":{}},"editorRowNames":["Hell Guard (hammer)","Hell Guard (shield)","Hell Guard (fist)"],"facts":{"adds":220,"borrows":0,"surfaces":0,"editor":0,"replaces":1,"editorOnly":false,"editorTweaks":[],"replacements":[{"path":"generated/decls/entitydef/ai/demon/hellguard.decl","edits":false}],"edits":[],"registries":{}},"borrows":{"paths":[],"count":0},"warnings":[]},{"key":"maps\\Hell Guard Arena\\Lava Floor","salt":"lavafl2z","name":"Lava Floor","author":"jo","size":66000,"files":3,"state":"ready","cause":"","editorRows":2,"folder":"Lava Floor","group":"","place":"maps\\Hell Guard Arena","placeTitle":"Hell Guard Arena","description":"","requirements":{"cvars":{}},"editorRowNames":["Lava floor A","Lava floor B"],"facts":{"adds":3,"borrows":0,"surfaces":0,"editor":0,"replaces":0,"editorOnly":false,"editorTweaks":[],"replacements":[],"edits":[],"registries":{}},"borrows":{"paths":[],"count":0},"warnings":[]}],"map":{"entities":214,"names":96,"rows":[{"salt":"alexrimp","name":"R2 Imp copy","build":"0d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36","size":1843230,"state":"active","cause":"","key":"alex.r2-imp","nameCount":1,"names":["mods/alexrimp/imp_copy"],"attached":false},{"salt":"joguards","name":"Spiked Walls","build":"1d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36","size":52000,"state":"missing","cause":"This map uses Spiked Walls, which is not installed.","key":"","nameCount":1,"names":["mods/joguards/wall"],"attached":false}],"placed":[{"salt":"alexrimp","build":"0d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36","size":1843230,"key":"alex.r2-imp","viaNeeds":false,"inManifest":true,"nameCount":1,"names":["mods/alexrimp/imp_copy"]}],"name":"Hell Guard Arena","saved":1793380563,"published":{"state":"published","at":1793164563},"uploaded":{"claimed":true,"at":1793164663},"tagged":true,"mods":[{"key":"alex.r2-imp","salt":"alexrimp","name":"R2 Imp copy","state":"installed","placedEntities":["Imp (R2) 1","Imp (R2) 2","Imp (R2) 3"],"placedCount":3,"placedModules":0,"references":[{"kind":"timeline","name":"Wave 2: spawn the imps"},{"kind":"wire","name":"Relay: all imps dead"}],"size":1843230,"inManifest":true,"viaNeeds":false,"attached":false,"nothingPlaced":false,"build":"0d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36"},{"key":"campaign-interactables\\doors","salt":"doorsx8q","name":"Doors","state":"installed","placedEntities":[],"placedCount":0,"placedModules":0,"references":[],"size":3801224,"inManifest":true,"viaNeeds":false,"attached":true,"nothingPlaced":true,"build":"2d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36"},{"key":"","salt":"joguards","name":"Spiked Walls","state":"missing","placedEntities":["Spiked wall 1","Spiked wall 2"],"placedCount":2,"placedModules":0,"references":[],"size":52000,"inManifest":true,"viaNeeds":false,"attached":false,"nothingPlaced":false,"build":"1d9eb7f70e75562c77d3bdcbfbc944d6c0663e52cf60349f712db25b8bd96d36"}],"vanilla":false,"registryClashes":[{"slot":"entitydef/player/coop/edit/handsModelMPUseDoomHands","providers":[{"id":"alexrimp","name":"R2 Imp copy"},{"id":"doorsx8q","name":"Doors"}],"chosen":"","serving":"alexrimp"}]}};
// The Files view's preview data: alex.r2-imp's files as its folder holds them, the classes from its mod.json, the archives illustrative.
var PREVIEW_MOD_FILES = {"alex.r2-imp":{"files":[{"path":"mod.json","size":1840,"kind":"text","class":"descriptor","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/alexrimp/imp_copy.decl","size":2210,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/mods/alexrimp/single_imp.decl","size":640,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapeditorentitydef/mods/alexrimp/imp_copy.decl","size":880,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/snapeditorentitydef/mods/alexrimp/imp_copy_b.decl","size":902,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/md6def/mods/alexrimp/imp_copy.md6.decl","size":4120,"kind":"text","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/basemodel/md6/mods/alexrimp/imp_copy/assets/mesh/imp_copy.bmd6model","size":812400,"kind":"binary","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/cooked/anim/md6/mods/alexrimp/imp_copy/motion/idle.bmd6anim","size":41000,"kind":"binary","class":"add","game":false,"container":"","inFolder":true,"cause":""},{"path":"assets/generated/decls/entitydef/player/coop.decl","size":6100,"kind":"text","class":"edit","game":true,"container":"snap_gameresources","inFolder":true,"cause":""},{"path":"assets/generated/decls/aifsmmanager/states/imp.decl","size":3020,"kind":"text","class":"edit","game":true,"container":"gameresources","inFolder":true,"cause":""},{"path":"assets/models/monsters/imp/imp.lwo","size":0,"kind":"binary","class":"borrow","game":true,"container":"gameresources","inFolder":false,"cause":""},{"path":"assets/generated/decls/material/models/monsters/imp/imp.decl","size":0,"kind":"text","class":"borrow","game":true,"container":"gameresources","inFolder":false,"cause":""}],"texts":{}}};
var previewMods = modsTab.standIn(PREVIEW_MODS, PREVIEW_MOD_FILES);
function previewHandle(o) {
  if (previewConfigHandle(o)) return;
  if (o.cmd === 'modsList') { onMessage({kind:'mods', data: PREVIEW_MODS}); return; }
  if (o.cmd === 'modsAction') {
    // As the host: the action's result, then a fresh document, from the stand-in store in
    // mods_tab.js, which changes its copy as the backend would (Rebuild, New mod, the dry runs,
    // Remove from map and Uninstall all work here; Install does not).
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
