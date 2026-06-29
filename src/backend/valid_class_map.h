/* valid_class_map.h -- inherit -> Y -> valid-classes, the THREAD-SAFE fallback for the dropdown
 * enumerator. The live engine type-lookup (sh_typeinfo_inherit_base / _class_derives) is GAME-THREAD-AFFINE
 * and returns null on the Qt UI thread; the dropdown enumerator runs UI-thread, so when the live lookup
 * fails it serves from this corpus snapshot instead. GENERATED from the reference entityDef corpus
 * (enumerator/inherit_y.json + valid_class_map.json + hierarchy.json). BUILD-SPECIFIC: regenerate on a DOOM
 * type-hierarchy change (the live apply-guard remains the source of truth; this is a UI hint). DO NOT hand-edit. */
#ifndef B2_VALID_CLASS_MAP_H
#define B2_VALID_CLASS_MAP_H

static const char *const VCM_0[] = {
    "idAI2",
};
static const char *const VCM_1[] = {
    "idAIConductor", "idAIConductor_Coop",
};
static const char *const VCM_2[] = {
    "idAlignedEntity",
};
static const char *const VCM_3[] = {
    "idDemonPlayer_Baron",
};
static const char *const VCM_4[] = {
    "idDemonPlayer_Mancubus",
};
static const char *const VCM_5[] = {
    "idDemonPlayer_Revenant",
};
static const char *const VCM_6[] = {
    "idDesignSystemsEntity", "idDesignSystems_Turret",
};
static const char *const VCM_7[] = {
    "idDesignSystems_Turret",
};
static const char *const VCM_8[] = {
    "idDynamicEntity", "idDynamicEntity_Damageable", "idDynamicSnapMapEntity", "idEntityFx", "idLensFlare",
    "idNetworkedEntityFx", "idSnapMapCapEntity", "idSnapMapLensFlare",
};
static const char *const VCM_9[] = {
    "idDynamicSnapMapEntity", "idSnapMapCapEntity",
};
static const char *const VCM_10[] = {
    "idDynamicStampEntity", "idDynamicStampEntity_Coop",
};
static const char *const VCM_11[] = {
    "idAASObstacle", "idAFEntity_Corpse", "idAI2", "idAIConductor", "idAIConductor_Coop", "idAlignedEntity",
    "idAnimated", "idAnimatedEntity", "idAnimated_AnimWeb", "idAnimated_AnimWeb_Coop", "idAnimated_ThreatSensor",
    "idAutoMapManager", "idBfgBurn", "idBreakable", "idCorpseManager", "idDemonPlayer_Baron",
    "idDemonPlayer_Cacodemon", "idDemonPlayer_Harvester", "idDemonPlayer_Mancubus", "idDemonPlayer_Pinky",
    "idDemonPlayer_Prowler", "idDemonPlayer_Revenant", "idDesignSystemsEntity", "idDesignSystems_Turret",
    "idDynamicEntity", "idDynamicEntity_Damageable", "idDynamicSnapMapEntity", "idDynamicStampEntity",
    "idDynamicStampEntity_Coop", "idEntity", "idEntityFx", "idEntityFxRandom", "idEnvArea",
    "idEnvironmentalDamage_Hurt_Trigger", "idEnvironmentalDamage_Point",
    "idEnvironmentalDamage_PointManager_Trigger", "idFlightVolumeTraversalSpline", "idFuncRotate", "idFuncSwing",
    "idGameChallenge", "idGameChallenge_CampaignSinglePlayer", "idGameChallenge_Coop",
    "idGameChallenge_Coop_HudInfo", "idGameChallenge_Coop_Variables", "idGameChallenge_PVP",
    "idGameSystems_Manager_ContainerEntity", "idGlobalDataComponent", "idGoreEntity", "idHologram",
    "idInfluenceSpawnSettings", "idInfo", "idInfoAmbient", "idInfoCloudShot", "idInfoCover", "idInfoCoverExposed",
    "idInfoDebugText", "idInfoPath", "idInfoTexLod", "idInfoTraversal", "idInfo_BounceDestination",
    "idInfo_TeleportDestination", "idInfo_TraversalChain", "idInfo_TraversalPoint", "idInfo_UniversalTraversal",
    "idInteractable", "idInteractable_EliteGuard", "idInteractable_EliteGuard_Coop", "idInteractable_LootCrate",
    "idInteractable_LootDrop", "idInteractable_NightmareMarker", "idInteractable_Obstacle",
    "idInteractable_Obstacle_SnapDoor", "idInteractable_PowerCoreReceptacle", "idInteractable_VegaTraining",
    "idInteractable_WeaponModBot", "idInteractable_WorldCache", "idLaserHazard", "idLensFlare", "idLight",
    "idMover", "idNetworkedEntityFx", "idParticleEmitter", "idPieceEmitter", "idPlaceableSnapAIEncounter",
    "idPlayer", "idPlayerStart", "idProjectile_AutoTurret", "idProjectile_BfgArc", "idProjectile_Chargeball",
    "idProjectile_DamageOverTime", "idProjectile_EMP", "idProjectile_GBF", "idProjectile_Grenade",
    "idProjectile_Grenade_DemonControl", "idProjectile_GuardBoulder", "idProjectile_HarvesterDetonateBall",
    "idProjectile_Nanobore", "idProjectile_PersonalTeleporter", "idProjectile_PlasmaPuddle", "idProjectile_Rocket",
    "idProjectile_RocketBounce", "idProjectile_SiphonGrenade", "idProjectile_Slicer",
    "idProjectile_StrikerGrenade", "idProjectile_SwarmMissile", "idProjectile_SwarmMissile_V2",
    "idProjectile_ThreatSensor", "idProp", "idProp2", "idProp_Coop", "idProp_Coop_Billboard",
    "idProp_WeaponPickup", "idReferenceMap", "idSnapAmmoPickup", "idSnapBackpackPickup", "idSnapDroppable",
    "idSnapEvent_MiniBoss", "idSnapEvent_Survive", "idSnapEvent_WaveBattle", "idSnapFilter_AI",
    "idSnapFilter_Bool", "idSnapFilter_CachedEntity", "idSnapFilter_Droppable", "idSnapFilter_Equipment",
    "idSnapFilter_Module", "idSnapFilter_Player", "idSnapFilter_Race", "idSnapFilter_Team",
    "idSnapFilter_UserFilter", "idSnapFilter_Volume", "idSnapFilter_Weapon", "idSnapGuiEntity",
    "idSnapInspectCameraHint", "idSnapItemHint", "idSnapMapAction", "idSnapMapAction_AIConductor_ClearSpawnQueue",
    "idSnapMapAction_AIDifficulty", "idSnapMapAction_AI_SetMod", "idSnapMapAction_AI_SetStaggerEnabled",
    "idSnapMapAction_AssignToTeam", "idSnapMapAction_BoolVar_Set", "idSnapMapAction_BoolVar_Test",
    "idSnapMapAction_BoolVar_Toggle", "idSnapMapAction_CachedEntityVar_Operation",
    "idSnapMapAction_Camera_ActivateCamera", "idSnapMapAction_Camera_DeactivateCamera",
    "idSnapMapAction_Camera_FadeCamera", "idSnapMapAction_Camera_FadeFromToCamera",
    "idSnapMapAction_Camera_SetEnvironment", "idSnapMapAction_Camera_Shake", "idSnapMapAction_ChangeRace",
    "idSnapMapAction_ColorVar_Channel_Operation", "idSnapMapAction_ColorVar_Operation",
    "idSnapMapAction_ComboStart_SetEnable", "idSnapMapAction_CompleteMainObjective",
    "idSnapMapAction_CompleteSubObjective", "idSnapMapAction_Counter_Modify", "idSnapMapAction_Counter_Reset",
    "idSnapMapAction_Counter_Set", "idSnapMapAction_CustomEventFire", "idSnapMapAction_Damage",
    "idSnapMapAction_Door_EndLockdown", "idSnapMapAction_Door_StartLockdown", "idSnapMapAction_Droppable_Remove",
    "idSnapMapAction_Droppable_Reset", "idSnapMapAction_Echo_Glitch", "idSnapMapAction_Echo_Opacity",
    "idSnapMapAction_Echo_Play", "idSnapMapAction_Encounter_ApplyBuff", "idSnapMapAction_Encounter_AssignTeam",
    "idSnapMapAction_EndGame", "idSnapMapAction_EndGame_Conditional", "idSnapMapAction_Explodable_Explode",
    "idSnapMapAction_FinishCountDown", "idSnapMapAction_FinishEncounter", "idSnapMapAction_FinishSnapEvent",
    "idSnapMapAction_FollowPath", "idSnapMapAction_Gate_SetA", "idSnapMapAction_Gate_SetB",
    "idSnapMapAction_Gate_Test", "idSnapMapAction_GivePlayerScore", "idSnapMapAction_GiveTeamScore",
    "idSnapMapAction_Hazard_SetDamage", "idSnapMapAction_Heal", "idSnapMapAction_HealAI",
    "idSnapMapAction_HideMainObjective", "idSnapMapAction_HideObjective", "idSnapMapAction_Incapacitate",
    "idSnapMapAction_IntTest", "idSnapMapAction_IntVar_Operation", "idSnapMapAction_IntVar_Random",
    "idSnapMapAction_Interactable_SetEnable", "idSnapMapAction_Interaction", "idSnapMapAction_Kill",
    "idSnapMapAction_KillAI", "idSnapMapAction_LaserHazard_SetOn", "idSnapMapAction_Light_SetIntensity",
    "idSnapMapAction_Light_SetOn", "idSnapMapAction_LoadSubObjectiveProgress", "idSnapMapAction_Logic_Fire",
    "idSnapMapAction_Logic_ResetRandomRelayUsedList", "idSnapMapAction_Logic_SetDelay",
    "idSnapMapAction_Logic_SetRandomRelayActivateCount", "idSnapMapAction_Logic_SwitchFire",
    "idSnapMapAction_ModifyAmmo", "idSnapMapAction_Module_AllowAicSpawn", "idSnapMapAction_Module_EndLockDown",
    "idSnapMapAction_Module_LockDown", "idSnapMapAction_Module_SetEnvironment", "idSnapMapAction_Mover_Start",
    "idSnapMapAction_NumTest", "idSnapMapAction_NumVar_Operation", "idSnapMapAction_NumVar_Random",
    "idSnapMapAction_PauseCountDown", "idSnapMapAction_PlayCallout", "idSnapMapAction_PlayMusic",
    "idSnapMapAction_PlaySpeaker", "idSnapMapAction_PlayerCamera_FadeCamera",
    "idSnapMapAction_PlayerCamera_FadeFromToCamera", "idSnapMapAction_PlayerCamera_SetEnvironment",
    "idSnapMapAction_PlayerCamera_Shake", "idSnapMapAction_PlayerInput_SetEnable",
    "idSnapMapAction_PlayerLoadout_GiveLoadout", "idSnapMapAction_PlayerLoadout_SetDemonSelection",
    "idSnapMapAction_PlayerLoadout_SetMaxWeaponSlots", "idSnapMapAction_PlayerResource_Operation",
    "idSnapMapAction_PlayerResource_Spend", "idSnapMapAction_Player_DropCurrentWeapon",
    "idSnapMapAction_Player_GiveDroppable", "idSnapMapAction_Player_GiveItem",
    "idSnapMapAction_Player_GivePowerup", "idSnapMapAction_Player_GiveTakeAbility",
    "idSnapMapAction_Player_Respawn", "idSnapMapAction_Player_SaveApplyRestorePoint",
    "idSnapMapAction_Player_TakeDroppable", "idSnapMapAction_Player_TakeItem", "idSnapMapAction_PostEvent",
    "idSnapMapAction_PowerCoreReceptacle_RespawnCell", "idSnapMapAction_PowerCoreReceptacle_SetCoreColor",
    "idSnapMapAction_Prop_EnableFX", "idSnapMapAction_RemoveAI", "idSnapMapAction_RemovePOI",
    "idSnapMapAction_Repeater_Stop", "idSnapMapAction_Repeater_Toggle", "idSnapMapAction_ResetAicModuleCooldown",
    "idSnapMapAction_ResetCountDown", "idSnapMapAction_ResourceTest", "idSnapMapAction_Respawn",
    "idSnapMapAction_Revive", "idSnapMapAction_SaveSubObjectiveProgress", "idSnapMapAction_Secret",
    "idSnapMapAction_Sequencer_Reset", "idSnapMapAction_Sequencer_SetSequenceNumber", "idSnapMapAction_SetColor",
    "idSnapMapAction_SetConductorSpawningPauseState", "idSnapMapAction_SetCountDown",
    "idSnapMapAction_SetLaunchDestination", "idSnapMapAction_SetLaunchSpeed", "idSnapMapAction_SetPOI",
    "idSnapMapAction_SetPlayerModifier", "idSnapMapAction_SetPlayerScore",
    "idSnapMapAction_SetSubObjectiveProgress", "idSnapMapAction_SetTeamScore",
    "idSnapMapAction_SetTriggerActivatable", "idSnapMapAction_SetTriggerVisible",
    "idSnapMapAction_Settings_SetActive", "idSnapMapAction_ShowMainObjective", "idSnapMapAction_ShowMessage",
    "idSnapMapAction_ShowObjective", "idSnapMapAction_ShowObjectiveCounter", "idSnapMapAction_ShowObjectiveHealth",
    "idSnapMapAction_ShowObjectiveResource", "idSnapMapAction_ShowObjectiveTimer",
    "idSnapMapAction_ShowSubObjective", "idSnapMapAction_SpawnEncounter", "idSnapMapAction_SpawnInhibit_SetEnable",
    "idSnapMapAction_SpawnItem", "idSnapMapAction_StartCountDown", "idSnapMapAction_StartIterator",
    "idSnapMapAction_StartSnapEvent", "idSnapMapAction_StopCountDown", "idSnapMapAction_StopMusic",
    "idSnapMapAction_StopSpeaker", "idSnapMapAction_StringTest", "idSnapMapAction_StringVar_Build",
    "idSnapMapAction_StringVar_Set", "idSnapMapAction_TeamResource_Operation",
    "idSnapMapAction_TeamResource_Spend", "idSnapMapAction_Teleport", "idSnapMapAction_Teleporter_SetEnable",
    "idSnapMapAction_UpdateSpeaker", "idSnapMapAction_Volume_EnableDisableTouch", "idSnapMapCapEntity",
    "idSnapMapCustomEvent", "idSnapMapEntity_IntCompare", "idSnapMapEntity_NumCompare",
    "idSnapMapEntity_ResourceCompare", "idSnapMapEntity_StringCompare", "idSnapMapGameEntityIterator_AI",
    "idSnapMapGameEntityIterator_Player", "idSnapMapGameEntityIterator_Team", "idSnapMapGameEntity_AI",
    "idSnapMapGameEntity_AIDirector", "idSnapMapGameEntity_BouncePad", "idSnapMapGameEntity_BouncePadDestination",
    "idSnapMapGameEntity_Callout", "idSnapMapGameEntity_Camera", "idSnapMapGameEntity_CodexMessage",
    "idSnapMapGameEntity_ComboStart", "idSnapMapGameEntity_ControlPoint", "idSnapMapGameEntity_Encounter",
    "idSnapMapGameEntity_EncounterList", "idSnapMapGameEntity_EndGame", "idSnapMapGameEntity_GameplaySettings",
    "idSnapMapGameEntity_HUDSettings", "idSnapMapGameEntity_ItemSpawnSettings", "idSnapMapGameEntity_Level",
    "idSnapMapGameEntity_Light", "idSnapMapGameEntity_Message", "idSnapMapGameEntity_Module",
    "idSnapMapGameEntity_Music", "idSnapMapGameEntity_NextMapSettings", "idSnapMapGameEntity_Objective",
    "idSnapMapGameEntity_ObjectiveSP", "idSnapMapGameEntity_ObjectiveSub", "idSnapMapGameEntity_Player",
    "idSnapMapGameEntity_PlayerCamera", "idSnapMapGameEntity_PlayerInput", "idSnapMapGameEntity_Player_Loadout",
    "idSnapMapGameEntity_ScoreSettings", "idSnapMapGameEntity_SnapPoiSettings", "idSnapMapGameEntity_Spawner",
    "idSnapMapGameEntity_Spawner_Ammo", "idSnapMapGameEntity_Spawner_Backpack",
    "idSnapMapGameEntity_Spawner_Droppable", "idSnapMapGameEntity_Spawner_Fx", "idSnapMapGameEntity_Speaker",
    "idSnapMapGameEntity_Team", "idSnapMapGameEntity_TeleporterDestination", "idSnapMapGameEntity_TeleporterPad",
    "idSnapMapGameEntity_TransmissionMessage", "idSnapMapGameEntity_WorldText", "idSnapMapLensFlare",
    "idSnapMapListener", "idSnapMapListener_AIKilled", "idSnapMapListener_Activator",
    "idSnapMapListener_CapturePointCaptured", "idSnapMapListener_DroppableActivator",
    "idSnapMapListener_IntCompare", "idSnapMapListener_IntensityChanged", "idSnapMapListener_NumCompare",
    "idSnapMapListener_OnEncounterPercentageReached", "idSnapMapListener_OnHealthPercentageReached",
    "idSnapMapListener_OnSnapScoreReached", "idSnapMapListener_PlayerEnteredModule",
    "idSnapMapListener_PlayerIncapacitated", "idSnapMapListener_PlayerInput", "idSnapMapListener_SequencerFired",
    "idSnapMapListener_Simple", "idSnapMapListener_SnapAnimEvent", "idSnapMapListener_StringCompare",
    "idSnapMapListener_TeamScoreReached", "idSnapMapLogic_Count", "idSnapMapLogic_CountDown",
    "idSnapMapLogic_Delay", "idSnapMapLogic_Gate", "idSnapMapLogic_RandomRelay", "idSnapMapLogic_Relay",
    "idSnapMapLogic_Repeater", "idSnapMapLogic_Sequencer", "idSnapMapLogic_Switch", "idSnapMapParticleEmitter",
    "idSnapMapStaticWaterEntity", "idSnapMapUserFilter", "idSnapMapVariable", "idSoundEnvironment", "idSpeaker",
    "idSpeaker_Beam", "idSplinePath", "idStaticEntity", "idStaticWaterEntity", "idSyncEntity", "idTarget_Secret",
    "idTarget_Snap_Objective", "idTarget_Spawn", "idTarget_Spawn_Coop", "idTrigger", "idTrigger_BouncePad",
    "idTrigger_CoopSafeZone", "idTrigger_Hurt", "idTrigger_Teleporter", "idUmbraVolume", "idVolume",
    "idVolume_Blocking", "idVolume_DamageOverTime", "idVolume_Flight", "idVolume_Inhibit_AIDirector_Spawning",
    "idVolume_MancubusSteam", "idVolume_MancubusSteamFX", "idVolume_MatterBallCore", "idVolume_PlasmaPuddle",
    "idVolume_PlasmaPuddleFX", "idVolume_PlayerEnvOverride", "idVolume_RevivePlayer", "idVolume_SecretHint",
    "idVolume_Siphon", "idVolume_SnapLockdownDoor", "idVolume_ToggleableDamageOverTime",
    "idVolume_Trigger_Editable", "idVolume_Trigger_Editable_Damageable", "idWeaponEntity", "idWorldspawn",
};
static const char *const VCM_12[] = {
    "idEntityFx", "idNetworkedEntityFx",
};
static const char *const VCM_13[] = {
    "idGameChallenge", "idGameChallenge_CampaignSinglePlayer", "idGameChallenge_Coop", "idGameChallenge_PVP",
};
static const char *const VCM_14[] = {
    "idGameChallenge_Coop", "idGameChallenge_PVP",
};
static const char *const VCM_15[] = {
    "idGoreEntity",
};
static const char *const VCM_16[] = {
    "idInfo", "idInfoAmbient", "idInfoCloudShot", "idInfoCover", "idInfoCoverExposed", "idInfoPath",
    "idInfoTexLod", "idInfoTraversal", "idInfo_BounceDestination", "idInfo_TeleportDestination",
    "idInfo_TraversalChain", "idInfo_TraversalPoint", "idInfo_UniversalTraversal",
};
static const char *const VCM_17[] = {
    "idInfoCoverExposed",
};
static const char *const VCM_18[] = {
    "idInfoPath",
};
static const char *const VCM_19[] = {
    "idInfoTraversal",
};
static const char *const VCM_20[] = {
    "idInteractable", "idInteractable_EliteGuard", "idInteractable_EliteGuard_Coop", "idInteractable_LootCrate",
    "idInteractable_LootDrop", "idInteractable_NightmareMarker", "idInteractable_Obstacle",
    "idInteractable_Obstacle_SnapDoor", "idInteractable_VegaTraining", "idInteractable_WeaponModBot",
    "idInteractable_WorldCache",
};
static const char *const VCM_21[] = {
    "idInteractable_LootCrate", "idInteractable_LootDrop", "idInteractable_WeaponModBot",
    "idInteractable_WorldCache",
};
static const char *const VCM_22[] = {
    "idInteractable_NightmareMarker",
};
static const char *const VCM_23[] = {
    "idInteractable_Obstacle", "idInteractable_Obstacle_SnapDoor",
};
static const char *const VCM_24[] = {
    "idInteractable_Obstacle_SnapDoor",
};
static const char *const VCM_25[] = {
    "idInteractable_PowerCoreReceptacle",
};
static const char *const VCM_26[] = {
    "idInteractable_VegaTraining",
};
static const char *const VCM_27[] = {
    "idInteractable_WorldCache",
};
static const char *const VCM_28[] = {
    "idLensFlare", "idSnapMapLensFlare",
};
static const char *const VCM_29[] = {
    "idLight", "idSnapMapGameEntity_Light",
};
static const char *const VCM_30[] = {
    "idMover",
};
static const char *const VCM_31[] = {
    "idParticleEmitter", "idSnapMapParticleEmitter",
};
static const char *const VCM_32[] = {
    "idPlayer",
};
static const char *const VCM_33[] = {
    "idProjectile_AutoTurret",
};
static const char *const VCM_34[] = {
    "idProjectile_BfgArc",
};
static const char *const VCM_35[] = {
    "idProjectile_DamageOverTime",
};
static const char *const VCM_36[] = {
    "idProjectile_EMP",
};
static const char *const VCM_37[] = {
    "idProjectile_GBF",
};
static const char *const VCM_38[] = {
    "idProjectile_Chargeball", "idProjectile_DamageOverTime", "idProjectile_Grenade",
    "idProjectile_Grenade_DemonControl", "idProjectile_PersonalTeleporter", "idProjectile_PlasmaPuddle",
    "idProjectile_SiphonGrenade", "idProjectile_StrikerGrenade",
};
static const char *const VCM_39[] = {
    "idProjectile_GuardBoulder",
};
static const char *const VCM_40[] = {
    "idProjectile_PlasmaPuddle",
};
static const char *const VCM_41[] = {
    "idProjectile_BfgArc", "idProjectile_EMP", "idProjectile_GBF", "idProjectile_GuardBoulder",
    "idProjectile_HarvesterDetonateBall", "idProjectile_Rocket", "idProjectile_SwarmMissile",
    "idProjectile_SwarmMissile_V2",
};
static const char *const VCM_42[] = {
    "idProjectile_SiphonGrenade",
};
static const char *const VCM_43[] = {
    "idProjectile_Slicer",
};
static const char *const VCM_44[] = {
    "idProjectile_SwarmMissile",
};
static const char *const VCM_45[] = {
    "idProp", "idProp_WeaponPickup",
};
static const char *const VCM_46[] = {
    "idProp2", "idSnapAmmoPickup", "idSnapBackpackPickup",
};
static const char *const VCM_47[] = {
    "idProp_Coop", "idProp_Coop_Billboard",
};
static const char *const VCM_48[] = {
    "idProp_Coop_Billboard",
};
static const char *const VCM_49[] = {
    "idProp_WeaponPickup",
};
static const char *const VCM_50[] = {
    "idSnapBackpackPickup",
};
static const char *const VCM_51[] = {
    "idSnapDroppable",
};
static const char *const VCM_52[] = {
    "idSnapEvent_WaveBattle",
};
static const char *const VCM_53[] = {
    "idSnapMapAction", "idSnapMapAction_AIConductor_ClearSpawnQueue", "idSnapMapAction_AIDifficulty",
    "idSnapMapAction_AI_SetMod", "idSnapMapAction_AssignToTeam", "idSnapMapAction_BoolVar_Set",
    "idSnapMapAction_BoolVar_Test", "idSnapMapAction_BoolVar_Toggle", "idSnapMapAction_CachedEntityVar_Operation",
    "idSnapMapAction_Camera_ActivateCamera", "idSnapMapAction_Camera_DeactivateCamera",
    "idSnapMapAction_Camera_FadeCamera", "idSnapMapAction_Camera_FadeFromToCamera",
    "idSnapMapAction_Camera_SetEnvironment", "idSnapMapAction_Camera_Shake", "idSnapMapAction_ChangeRace",
    "idSnapMapAction_ColorVar_Channel_Operation", "idSnapMapAction_ColorVar_Operation",
    "idSnapMapAction_ComboStart_SetEnable", "idSnapMapAction_CompleteMainObjective",
    "idSnapMapAction_CompleteSubObjective", "idSnapMapAction_Counter_Modify", "idSnapMapAction_Counter_Reset",
    "idSnapMapAction_Counter_Set", "idSnapMapAction_CustomEventFire", "idSnapMapAction_Damage",
    "idSnapMapAction_Door_EndLockdown", "idSnapMapAction_Door_StartLockdown", "idSnapMapAction_Droppable_Remove",
    "idSnapMapAction_Droppable_Reset", "idSnapMapAction_Echo_Glitch", "idSnapMapAction_Echo_Opacity",
    "idSnapMapAction_Echo_Play", "idSnapMapAction_Encounter_ApplyBuff", "idSnapMapAction_Encounter_AssignTeam",
    "idSnapMapAction_EndGame", "idSnapMapAction_EndGame_Conditional", "idSnapMapAction_Explodable_Explode",
    "idSnapMapAction_FinishCountDown", "idSnapMapAction_FinishEncounter", "idSnapMapAction_FinishSnapEvent",
    "idSnapMapAction_FollowPath", "idSnapMapAction_Gate_SetA", "idSnapMapAction_Gate_SetB",
    "idSnapMapAction_Gate_Test", "idSnapMapAction_GivePlayerScore", "idSnapMapAction_GiveTeamScore",
    "idSnapMapAction_Hazard_SetDamage", "idSnapMapAction_Heal", "idSnapMapAction_HideMainObjective",
    "idSnapMapAction_HideObjective", "idSnapMapAction_Incapacitate", "idSnapMapAction_IntTest",
    "idSnapMapAction_IntVar_Operation", "idSnapMapAction_IntVar_Random", "idSnapMapAction_Interactable_SetEnable",
    "idSnapMapAction_Interaction", "idSnapMapAction_Kill", "idSnapMapAction_KillAI",
    "idSnapMapAction_LaserHazard_SetOn", "idSnapMapAction_Light_SetIntensity", "idSnapMapAction_Light_SetOn",
    "idSnapMapAction_LoadSubObjectiveProgress", "idSnapMapAction_Logic_Fire",
    "idSnapMapAction_Logic_ResetRandomRelayUsedList", "idSnapMapAction_Logic_SetDelay",
    "idSnapMapAction_Logic_SetRandomRelayActivateCount", "idSnapMapAction_Logic_SwitchFire",
    "idSnapMapAction_Module_AllowAicSpawn", "idSnapMapAction_Module_EndLockDown",
    "idSnapMapAction_Module_LockDown", "idSnapMapAction_Module_SetEnvironment", "idSnapMapAction_Mover_Start",
    "idSnapMapAction_NumTest", "idSnapMapAction_NumVar_Operation", "idSnapMapAction_NumVar_Random",
    "idSnapMapAction_PauseCountDown", "idSnapMapAction_PlayCallout", "idSnapMapAction_PlayMusic",
    "idSnapMapAction_PlaySpeaker", "idSnapMapAction_PlayerCamera_FadeCamera",
    "idSnapMapAction_PlayerCamera_FadeFromToCamera", "idSnapMapAction_PlayerCamera_SetEnvironment",
    "idSnapMapAction_PlayerCamera_Shake", "idSnapMapAction_PlayerInput_SetEnable",
    "idSnapMapAction_PlayerLoadout_GiveLoadout", "idSnapMapAction_PlayerLoadout_SetDemonSelection",
    "idSnapMapAction_PlayerLoadout_SetMaxWeaponSlots", "idSnapMapAction_PlayerResource_Operation",
    "idSnapMapAction_PlayerResource_Spend", "idSnapMapAction_Player_DropCurrentWeapon",
    "idSnapMapAction_Player_GiveDroppable", "idSnapMapAction_Player_GiveItem",
    "idSnapMapAction_Player_GivePowerup", "idSnapMapAction_Player_Respawn", "idSnapMapAction_Player_TakeDroppable",
    "idSnapMapAction_Player_TakeItem", "idSnapMapAction_PostEvent",
    "idSnapMapAction_PowerCoreReceptacle_RespawnCell", "idSnapMapAction_PowerCoreReceptacle_SetCoreColor",
    "idSnapMapAction_RemoveAI", "idSnapMapAction_RemovePOI", "idSnapMapAction_Repeater_Stop",
    "idSnapMapAction_Repeater_Toggle", "idSnapMapAction_ResetAicModuleCooldown", "idSnapMapAction_ResetCountDown",
    "idSnapMapAction_ResourceTest", "idSnapMapAction_Respawn", "idSnapMapAction_Revive",
    "idSnapMapAction_SaveSubObjectiveProgress", "idSnapMapAction_Secret", "idSnapMapAction_Sequencer_Reset",
    "idSnapMapAction_Sequencer_SetSequenceNumber", "idSnapMapAction_SetColor",
    "idSnapMapAction_SetConductorSpawningPauseState", "idSnapMapAction_SetCountDown",
    "idSnapMapAction_SetLaunchDestination", "idSnapMapAction_SetLaunchSpeed", "idSnapMapAction_SetPOI",
    "idSnapMapAction_SetPlayerModifier", "idSnapMapAction_SetPlayerScore",
    "idSnapMapAction_SetSubObjectiveProgress", "idSnapMapAction_SetTeamScore",
    "idSnapMapAction_SetTriggerActivatable", "idSnapMapAction_SetTriggerVisible",
    "idSnapMapAction_Settings_SetActive", "idSnapMapAction_ShowMainObjective", "idSnapMapAction_ShowMessage",
    "idSnapMapAction_ShowObjective", "idSnapMapAction_ShowObjectiveCounter", "idSnapMapAction_ShowObjectiveHealth",
    "idSnapMapAction_ShowObjectiveResource", "idSnapMapAction_ShowObjectiveTimer",
    "idSnapMapAction_ShowSubObjective", "idSnapMapAction_SpawnEncounter", "idSnapMapAction_SpawnInhibit_SetEnable",
    "idSnapMapAction_SpawnItem", "idSnapMapAction_StartCountDown", "idSnapMapAction_StartIterator",
    "idSnapMapAction_StartSnapEvent", "idSnapMapAction_StopCountDown", "idSnapMapAction_StopMusic",
    "idSnapMapAction_StopSpeaker", "idSnapMapAction_StringTest", "idSnapMapAction_StringVar_Build",
    "idSnapMapAction_StringVar_Set", "idSnapMapAction_TeamResource_Operation",
    "idSnapMapAction_TeamResource_Spend", "idSnapMapAction_Teleport", "idSnapMapAction_Teleporter_SetEnable",
    "idSnapMapAction_UpdateSpeaker", "idSnapMapAction_Volume_EnableDisableTouch",
};
static const char *const VCM_54[] = {
    "idSnapMapCapEntity",
};
static const char *const VCM_55[] = {
    "idSnapMapGameEntity_AIDirector",
};
static const char *const VCM_56[] = {
    "idSnapMapGameEntity_Spawner",
};
static const char *const VCM_57[] = {
    "idSnapMapListener", "idSnapMapListener_AIKilled", "idSnapMapListener_Activator",
    "idSnapMapListener_CapturePointCaptured", "idSnapMapListener_DroppableActivator",
    "idSnapMapListener_IntCompare", "idSnapMapListener_IntensityChanged", "idSnapMapListener_NumCompare",
    "idSnapMapListener_OnEncounterPercentageReached", "idSnapMapListener_OnSnapScoreReached",
    "idSnapMapListener_PlayerEnteredModule", "idSnapMapListener_PlayerIncapacitated",
    "idSnapMapListener_PlayerInput", "idSnapMapListener_SequencerFired", "idSnapMapListener_Simple",
    "idSnapMapListener_SnapAnimEvent", "idSnapMapListener_StringCompare", "idSnapMapListener_TeamScoreReached",
};
static const char *const VCM_58[] = {
    "idSpeaker",
};
static const char *const VCM_59[] = {
    "idFlightVolumeTraversalSpline", "idSplinePath",
};
static const char *const VCM_60[] = {
    "idSnapMapStaticWaterEntity", "idStaticWaterEntity",
};
static const char *const VCM_61[] = {
    "idSyncEntity",
};
static const char *const VCM_62[] = {
    "idEnvironmentalDamage_Hurt_Trigger", "idEnvironmentalDamage_PointManager_Trigger",
    "idSnapMapStaticWaterEntity", "idStaticWaterEntity", "idTrigger", "idTrigger_BouncePad", "idTrigger_Hurt",
    "idTrigger_Teleporter",
};
static const char *const VCM_63[] = {
    "idVolume", "idVolume_DamageOverTime", "idVolume_Flight", "idVolume_MancubusSteam", "idVolume_MancubusSteamFX",
    "idVolume_PlasmaPuddle", "idVolume_PlasmaPuddleFX", "idVolume_PlayerEnvOverride", "idVolume_RevivePlayer",
    "idVolume_SecretHint", "idVolume_Siphon", "idVolume_ToggleableDamageOverTime",
};
static const char *const VCM_64[] = {
    "idVolume_MancubusSteam",
};
static const char *const VCM_65[] = {
    "idVolume_MatterBallCore",
};
static const char *const VCM_66[] = {
    "idVolume_PlasmaPuddle",
};
static const char *const VCM_67[] = {
    "idVolume_PlasmaPuddleFX",
};
static const char *const VCM_68[] = {
    "idVolume_Siphon",
};
static const char *const VCM_69[] = {
    "idWeaponEntity",
};

typedef struct { const char *y; const char *const *classes; int n; } vcm_yc;
static const vcm_yc SH_VCM_Y_CLASSES[] = {
    { "idAI2", VCM_0, 1 },
    { "idAIConductor", VCM_1, 2 },
    { "idAlignedEntity", VCM_2, 1 },
    { "idDemonPlayer_Baron", VCM_3, 1 },
    { "idDemonPlayer_Mancubus", VCM_4, 1 },
    { "idDemonPlayer_Revenant", VCM_5, 1 },
    { "idDesignSystemsEntity", VCM_6, 2 },
    { "idDesignSystems_Turret", VCM_7, 1 },
    { "idDynamicEntity", VCM_8, 8 },
    { "idDynamicSnapMapEntity", VCM_9, 2 },
    { "idDynamicStampEntity", VCM_10, 2 },
    { "idEntity", VCM_11, 412 },
    { "idEntityFx", VCM_12, 2 },
    { "idGameChallenge", VCM_13, 4 },
    { "idGameChallenge_PVP", VCM_14, 2 },
    { "idGoreEntity", VCM_15, 1 },
    { "idInfo", VCM_16, 13 },
    { "idInfoCoverExposed", VCM_17, 1 },
    { "idInfoPath", VCM_18, 1 },
    { "idInfoTraversal", VCM_19, 1 },
    { "idInteractable", VCM_20, 11 },
    { "idInteractable_LootDrop", VCM_21, 4 },
    { "idInteractable_NightmareMarker", VCM_22, 1 },
    { "idInteractable_Obstacle", VCM_23, 2 },
    { "idInteractable_Obstacle_SnapDoor", VCM_24, 1 },
    { "idInteractable_PowerCoreReceptacle", VCM_25, 1 },
    { "idInteractable_VegaTraining", VCM_26, 1 },
    { "idInteractable_WorldCache", VCM_27, 1 },
    { "idLensFlare", VCM_28, 2 },
    { "idLight", VCM_29, 2 },
    { "idMover", VCM_30, 1 },
    { "idParticleEmitter", VCM_31, 2 },
    { "idPlayer", VCM_32, 1 },
    { "idProjectile_AutoTurret", VCM_33, 1 },
    { "idProjectile_BfgArc", VCM_34, 1 },
    { "idProjectile_DamageOverTime", VCM_35, 1 },
    { "idProjectile_EMP", VCM_36, 1 },
    { "idProjectile_GBF", VCM_37, 1 },
    { "idProjectile_Grenade", VCM_38, 8 },
    { "idProjectile_GuardBoulder", VCM_39, 1 },
    { "idProjectile_PlasmaPuddle", VCM_40, 1 },
    { "idProjectile_Rocket", VCM_41, 8 },
    { "idProjectile_SiphonGrenade", VCM_42, 1 },
    { "idProjectile_Slicer", VCM_43, 1 },
    { "idProjectile_SwarmMissile", VCM_44, 1 },
    { "idProp", VCM_45, 2 },
    { "idProp2", VCM_46, 3 },
    { "idProp_Coop", VCM_47, 2 },
    { "idProp_Coop_Billboard", VCM_48, 1 },
    { "idProp_WeaponPickup", VCM_49, 1 },
    { "idSnapBackpackPickup", VCM_50, 1 },
    { "idSnapDroppable", VCM_51, 1 },
    { "idSnapEvent_WaveBattle", VCM_52, 1 },
    { "idSnapMapAction", VCM_53, 152 },
    { "idSnapMapCapEntity", VCM_54, 1 },
    { "idSnapMapGameEntity_AIDirector", VCM_55, 1 },
    { "idSnapMapGameEntity_Spawner", VCM_56, 1 },
    { "idSnapMapListener", VCM_57, 18 },
    { "idSpeaker", VCM_58, 1 },
    { "idSplinePath", VCM_59, 2 },
    { "idStaticWaterEntity", VCM_60, 2 },
    { "idSyncEntity", VCM_61, 1 },
    { "idTrigger", VCM_62, 8 },
    { "idVolume", VCM_63, 12 },
    { "idVolume_MancubusSteam", VCM_64, 1 },
    { "idVolume_MatterBallCore", VCM_65, 1 },
    { "idVolume_PlasmaPuddle", VCM_66, 1 },
    { "idVolume_PlasmaPuddleFX", VCM_67, 1 },
    { "idVolume_Siphon", VCM_68, 1 },
    { "idWeaponEntity", VCM_69, 1 },
};
#define SH_VCM_Y_CLASSES_N 70

typedef struct { const char *inherit; const char *y; } vcm_iy;
static const vcm_iy SH_VCM_INHERIT_Y[] = {
    { "ai/base/conductor", "idAIConductor" },
    { "ai/default", "idAI2" },
    { "ai/demon/archvile", "idAI2" },
    { "ai/demon/archvile/attachments/default", "idGoreEntity" },
    { "ai/demon/archvile_base", "idAI2" },
    { "ai/demon/baron", "idAI2" },
    { "ai/demon/baron/attachments/default", "idGoreEntity" },
    { "ai/demon/baron_base", "idAI2" },
    { "ai/demon/cacodemon", "idAI2" },
    { "ai/demon/cacodemon/attachments/default", "idGoreEntity" },
    { "ai/demon/cacodemon_base", "idAI2" },
    { "ai/demon/hellknight", "idAI2" },
    { "ai/demon/hellknight/attachments/default", "idGoreEntity" },
    { "ai/demon/hellknight_base", "idAI2" },
    { "ai/demon/imp", "idAI2" },
    { "ai/demon/imp/attachments/default", "idGoreEntity" },
    { "ai/demon/imp_base", "idAI2" },
    { "ai/demon/lostsoul", "idAI2" },
    { "ai/demon/lostsoul_base", "idAI2" },
    { "ai/demon/mancubus", "idAI2" },
    { "ai/demon/mancubus/attachments/default", "idGoreEntity" },
    { "ai/demon/mancubus_base", "idAI2" },
    { "ai/demon/mancubus_cyber", "idAI2" },
    { "ai/demon/mancubus_cyber/attachments/default", "idGoreEntity" },
    { "ai/demon/pinky", "idAI2" },
    { "ai/demon/pinky/attachments/default", "idGoreEntity" },
    { "ai/demon/pinky_base", "idAI2" },
    { "ai/demon/pinky_spectre", "idAI2" },
    { "ai/demon/revenant", "idAI2" },
    { "ai/demon/revenant/attachments/default", "idGoreEntity" },
    { "ai/demon/revenant_base", "idAI2" },
    { "ai/hellified/attachments/default", "idGoreEntity" },
    { "ai/hellified/hell_base", "idAI2" },
    { "ai/hellified/marine_base", "idAI2" },
    { "ai/hellified/marine_laser", "idAI2" },
    { "ai/hellified/marine_rifle", "idAI2" },
    { "ai/hellified/marine_shotgunner", "idAI2" },
    { "ai/zombie/attachments/base", "idGoreEntity" },
    { "ai/zombie/hell_base", "idAI2" },
    { "ai/zombie/scientist", "idAI2" },
    { "ai/zombie/uac_security", "idAI2" },
    { "ai/zombie/welder", "idAI2" },
    { "ai/zombie/zombie_base", "idAI2" },
    { "coop/ammo/ai_loot_ammo_universal_small", "idProp2" },
    { "coop/ammo/coop_ammo_ai_drop", "idProp2" },
    { "coop/ammo/coop_ammo_universal_medium", "idProp2" },
    { "coop/ammo/coop_ammo_universal_medium_drop", "idProp2" },
    { "coop/ammo/coop_ammo_universal_small", "idProp2" },
    { "coop/backpack/backpack", "idSnapBackpackPickup" },
    { "coop/health/coop_health_pickup10", "idProp2" },
    { "coop/health/coop_health_pickup25", "idProp2" },
    { "coop/health/coop_health_pickup50", "idProp2" },
    { "coop/health/coop_health_pickup50_one_use", "idProp2" },
    { "coop/health/coop_supercharge", "idProp2" },
    { "coop/pickup/bfg_mp_base", "idProp2" },
    { "coop/pickup/chaingun_mp_base", "idProp2" },
    { "coop/pickup/chaingun_mp_gatling", "idProp2" },
    { "coop/pickup/combat_shotgun_mp_base", "idProp2" },
    { "coop/pickup/combat_shotgun_mp_pop_rockets", "idProp2" },
    { "coop/pickup/combat_shotgun_sp_burst", "idProp2" },
    { "coop/pickup/default", "idProp2" },
    { "coop/pickup/default_drop", "idProp2" },
    { "coop/pickup/flare_rifle_mp_burn", "idProp2" },
    { "coop/pickup/gauss_rifle_mp_base", "idProp2" },
    { "coop/pickup/gauss_rifle_sp_charged_sniper", "idProp2" },
    { "coop/pickup/heavy_assault_rifle_mp_base", "idProp2" },
    { "coop/pickup/heavy_assault_rifle_mp_zoom", "idProp2" },
    { "coop/pickup/heavy_assault_rifle_sp_detonate", "idProp2" },
    { "coop/pickup/lightning_gun_mp_chain", "idProp2" },
    { "coop/pickup/plasma_rifle_mp_base", "idProp2" },
    { "coop/pickup/plasma_rifle_mp_field", "idProp2" },
    { "coop/pickup/plasma_rifle_sp_stun", "idProp2" },
    { "coop/pickup/repeater_mp_single_shot", "idProp2" },
    { "coop/pickup/rocket_launcher_mp_base", "idProp2" },
    { "coop/pickup/rocket_launcher_mp_detonate", "idProp2" },
    { "coop/pickup/super_shotgun_mp_base", "idProp2" },
    { "coop/pickup/vortex_rifle_mp_charge", "idProp2" },
    { "design_systems/turrets/turret_base", "idDesignSystems_Turret" },
    { "func/dynamic", "idDynamicEntity" },
    { "func/emitter", "idParticleEmitter" },
    { "func/flare", "idLensFlare" },
    { "func/fx", "idEntityFx" },
    { "func/mover", "idMover" },
    { "func/water", "idStaticWaterEntity" },
    { "gore/default", "idGoreEntity" },
    { "info/exposed_cover", "idInfoCoverExposed" },
    { "info/path", "idInfoPath" },
    { "info/spacial", "idInfo" },
    { "info/traversal", "idInfoTraversal" },
    { "interact/crate_switch", "idInteractable" },
    { "interact/doors/bc_double_door", "idInteractable_Obstacle" },
    { "interact/doors/industrial_armored_double_384", "idInteractable_Obstacle" },
    { "interact/gore_totem", "idInteractable" },
    { "interact/hell_tablet", "idInteractable" },
    { "interact/loot_box/ammo_crate_base", "idInteractable_LootDrop" },
    { "interact/loot_box/ammo_crate_sync", "idInteractable_LootDrop" },
    { "interact/nightmaremarker/base", "idInteractable_NightmareMarker" },
    { "interact/panels/tripod_panel_small_1touch", "idInteractable" },
    { "interact/player_progression/jump_boots", "idInteractable" },
    { "interact/player_progression/suitmods/vega_training_terminal", "idInteractable_VegaTraining" },
    { "interact/player_progression/universal_cache_base", "idInteractable_WorldCache" },
    { "interact/player_progression/weaponmod_cache_base", "idInteractable_LootDrop" },
    { "interact/power_core/power_core_dispenser_one", "idInteractable_PowerCoreReceptacle" },
    { "interact/power_core/power_core_recepticle", "idInteractable_PowerCoreReceptacle" },
    { "interact/resource_ops_tech_corpse", "idInteractable" },
    { "interact/skull_switch", "idInteractable" },
    { "interact/syncentity/ammo_crate", "idSyncEntity" },
    { "interact/syncentity/panels/wall_panel_small_1touch", "idSyncEntity" },
    { "interact/syncentity/use_panel", "idSyncEntity" },
    { "interact/use_panel", "idInteractable" },
    { "light/point", "idLight" },
    { "light/spot", "idLight" },
    { "mp/ammo/mp_ammo_universal_medium", "idProp2" },
    { "mp/armor/mp_armor_pickup10", "idProp2" },
    { "mp/armor/mp_armor_pickup_blue", "idProp2" },
    { "mp/armor/mp_armor_pickup_green", "idProp2" },
    { "mp/base/game_challenge", "idGameChallenge" },
    { "mp/demonpowers/groundpoundspike1", "idEntityFx" },
    { "mp/health/mp_health_pickup10", "idProp2" },
    { "mp/health/mp_health_supercharge", "idProp2" },
    { "mp/modes/pvp/game_challenge", "idGameChallenge_PVP" },
    { "mp/pickup/pickup_haste", "idProp2" },
    { "mp/pickup/pickup_invisibility", "idProp2" },
    { "mp/pickup/pickup_quad_damage", "idProp2" },
    { "mp/pickup/pickup_regeneration", "idProp2" },
    { "mp/powerup/mp_become_demon", "idProp2" },
    { "player", "idPlayer" },
    { "player/mp", "idPlayer" },
    { "player/mp/demonplayer_baron", "idDemonPlayer_Baron" },
    { "player/mp/demonplayer_mancubus", "idDemonPlayer_Mancubus" },
    { "player/mp/demonplayer_revenant", "idDemonPlayer_Revenant" },
    { "player/mp/harvester/sliced/default", "idGoreEntity" },
    { "player/mp/mancubus/sliced/default", "idGoreEntity" },
    { "player_tp_body", "idAlignedEntity" },
    { "projectile_ent/player/mp/loadout/flare_rifle", "idProjectile_Grenade" },
    { "projectile_ent/player/mp/loadout/flare_rifle_burn", "idProjectile_DamageOverTime" },
    { "projectile_ent/player/mp/loadout/plasma_rifle", "idProjectile_Rocket" },
    { "projectile_ent/player/mp/loadout/plasma_rifle_field", "idProjectile_Grenade" },
    { "projectile_ent/player/mp/loadout/rocket_launcher", "idProjectile_Rocket" },
    { "projectile_ent/player/mp/power/bfg", "idProjectile_GBF" },
    { "projectile_ent/throwable/grenade", "idProjectile_Grenade" },
    { "projectile_ent/throwable/grenade/clustersub_poprocket", "idProjectile_Grenade" },
    { "projectile_ent/throwable/grenade/clustersub_sp", "idProjectile_Grenade" },
    { "projectile_ent/throwable/mp_auto_turret", "idProjectile_AutoTurret" },
    { "projectile_ent/throwable/mp_grenade_base", "idProjectile_Grenade" },
    { "projectile_ent/throwable/mp_radiation_grenade", "idProjectile_PlasmaPuddle" },
    { "projectile_ent/throwable/mp_shield_wall", "idProjectile_Grenade" },
    { "projectile_ent/throwable/mp_siphon_grenade", "idProjectile_SiphonGrenade" },
    { "projectile_ent/throwable/player/sp/grenade", "idProjectile_Grenade" },
    { "projectile_ent/zion/ai/hellified_soldier/plasma", "idProjectile_Rocket" },
    { "projectile_ent/zion/ai/imp/fireball", "idProjectile_Rocket" },
    { "projectile_ent/zion/ai/oliviasguard/boulder", "idProjectile_GuardBoulder" },
    { "projectile_ent/zion/ai/revenant/rocket", "idProjectile_Rocket" },
    { "projectile_ent/zion/base/plasma", "idProjectile_Rocket" },
    { "projectile_ent/zion/base/rocket", "idProjectile_Rocket" },
    { "projectile_ent/zion/player/mp/demon/mancubus/primary", "idProjectile_Rocket" },
    { "projectile_ent/zion/player/mp/demon/revenant/rocket", "idProjectile_SwarmMissile" },
    { "projectile_ent/zion/player/mp/demon/revenant/rocket_weakened", "idProjectile_SwarmMissile" },
    { "projectile_ent/zion/player/mp/slicer", "idProjectile_Slicer" },
    { "projectile_ent/zion/player/mp/tesla_rocket_alt", "idProjectile_GBF" },
    { "projectile_ent/zion/player/sp/assault_rifle_assaultrifle_secondary", "idProjectile_Grenade" },
    { "projectile_ent/zion/player/sp/bfg_charge_sphere_arc", "idProjectile_BfgArc" },
    { "projectile_ent/zion/player/sp/heavy_rifle_burst_detonate", "idProjectile_Rocket" },
    { "projectile_ent/zion/player/sp/heavy_rifle_burst_detonate_mastery", "idProjectile_Rocket" },
    { "projectile_ent/zion/player/sp/plasma_rifle", "idProjectile_Rocket" },
    { "projectile_ent/zion/player/sp/plasma_rifle_secondary_stun", "idProjectile_EMP" },
    { "projectile_ent/zion/player/sp/plasma_rifle_secondary_stun_mastery", "idProjectile_EMP" },
    { "projectile_ent/zion/player/sp/plasma_stun_mastery_arc", "idProjectile_BfgArc" },
    { "projectile_ent/zion/player/sp/rocket_launcher", "idProjectile_Rocket" },
    { "projectile_ent/zion/player/sp/shotgun_pop_rocket", "idProjectile_Grenade" },
    { "projectile_ent/zion/player/sp/shotgun_pop_rocket_larger_explosion", "idProjectile_Grenade" },
    { "prop/default", "idProp" },
    { "prop/player_progression/health_upgrades/default", "idProp2" },
    { "prop/player_progression/suitmods/default", "idProp2" },
    { "prop/player_progression/weaponmods/default", "idProp2" },
    { "prop/weapon/base/base", "idProp_WeaponPickup" },
    { "prop/weapon/base/chaingun", "idProp_WeaponPickup" },
    { "prop/weapon/base/chainsaw", "idProp_WeaponPickup" },
    { "prop/weapon/base/double_barrel_shotgun", "idProp_WeaponPickup" },
    { "prop/weapon/base/plasma_rifle", "idProp_WeaponPickup" },
    { "prop/weapon/base/rocket_launcher", "idProp_WeaponPickup" },
    { "prop/zion/abilities/sp/default", "idProp2" },
    { "prop/zion/ammo/lootpinata/items/ammo_bfg_1", "idProp2" },
    { "prop/zion/ammo/sp/cache/items/base", "idProp2" },
    { "prop/zion/lootpinata/items/ammo", "idProp2" },
    { "prop/zion/lootpinata/items/default", "idProp2" },
    { "prop/zion/lootpinata/items/health_small", "idProp2" },
    { "prop/zion/statuseffects/base", "idProp2" },
    { "prop/zion/statuseffects/sp/base", "idProp2" },
    { "prop/zion/statuseffects/sp/berzerk/base", "idProp2" },
    { "snapmaps/action/input_base", "idSnapMapAction" },
    { "snapmaps/ai/director_base", "idSnapMapGameEntity_AIDirector" },
    { "snapmaps/doors/industrial_door", "idInteractable_Obstacle" },
    { "snapmaps/doors/snap_door_base", "idInteractable_Obstacle_SnapDoor" },
    { "snapmaps/droppables/flags/base_flag", "idSnapDroppable" },
    { "snapmaps/droppables/keycard/base_keycard", "idSnapDroppable" },
    { "snapmaps/droppables/power_cores/power_core_base", "idSnapDroppable" },
    { "snapmaps/droppables/skullkey/base_skullkey", "idSnapDroppable" },
    { "snapmaps/event/wave", "idSnapEvent_WaveBattle" },
    { "snapmaps/explodeable_props/barrel", "idProp2" },
    { "snapmaps/explodeable_props/barrel_uac", "idProp2" },
    { "snapmaps/func/snap_dynamic", "idDynamicSnapMapEntity" },
    { "snapmaps/func/snap_mover", "idMover" },
    { "snapmaps/fx/decal", "idDynamicStampEntity" },
    { "snapmaps/listener/output_base", "idSnapMapListener" },
    { "snapmaps/moveable_props/default/cardboard_box", "idProp" },
    { "snapmaps/moveable_props/default/metal_box", "idProp" },
    { "snapmaps/moveable_props/default/metal_can", "idProp" },
    { "snapmaps/moveable_props/default/plastic_bottle", "idProp" },
    { "snapmaps/moveable_props/default/plastic_box", "idProp" },
    { "snapmaps/moveable_props/generic", "idProp" },
    { "snapmaps/pickups/pickup_berserk", "idProp2" },
    { "snapmaps/pickups/pickup_haste", "idProp2" },
    { "snapmaps/pickups/pickup_invisibility", "idProp2" },
    { "snapmaps/pickups/pickup_quad_damage", "idProp2" },
    { "snapmaps/pickups/pickup_regeneration", "idProp2" },
    { "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_heavy_assault_rifle_sp_detonate", "idSnapMapGameEntity_Spawner" },
    { "snapmaps/pickups/weapons/coop_pickup_spawner_weapon_plasma_rifle_sp_stun", "idSnapMapGameEntity_Spawner" },
    { "snapmaps/props/classic/classic_props_base", "idProp_Coop_Billboard" },
    { "snapmaps/props/keys/base_keycard", "idProp_Coop" },
    { "snapmaps/props/keys/base_keycard_classic", "idProp_Coop" },
    { "snapmaps/props/keys/base_skullkey", "idProp_Coop" },
    { "snapmaps/props/keys/base_skullkey_classic", "idProp_Coop" },
    { "snapmaps/unknown", "idEntity" },
    { "snapmaps/visblockers/cap_base", "idSnapMapCapEntity" },
    { "speaker", "idSpeaker" },
    { "spline/path", "idSplinePath" },
    { "target/default", "idEntity" },
    { "triggers/interact/loot_box_custom_1", "idTrigger" },
    { "triggers/interact/use_panel_custom", "idTrigger" },
    { "triggers/prop/default", "idTrigger" },
    { "volume/default", "idVolume" },
    { "volume/hazard/mp_radiation_puddle", "idVolume_PlasmaPuddle" },
    { "volume/hazard/siphon_volume", "idVolume_Siphon" },
    { "volume/hazard/sp_siphon_volume", "idVolume_Siphon" },
    { "volume/mp/mp_mancubus_steam", "idVolume_MancubusSteam" },
    { "volume/mp_shield_wall_classic", "idVolume_MatterBallCore" },
    { "volume/player/mp/equipment/radiation_grenade_field", "idVolume_PlasmaPuddle" },
    { "volume/player/mp/equipment/radiation_grenade_field_dot", "idVolume_PlasmaPuddleFX" },
    { "volume/player/mp/loadout/plasma_rifle_field", "idVolume_PlasmaPuddle" },
    { "volume/player/mp/loadout/plasma_rifle_field_dot", "idVolume_PlasmaPuddleFX" },
    { "weapons/staticweaponent", "idWeaponEntity" },
    { "weapons/weaponent", "idWeaponEntity" },
    { "zion/cineractive/maps/argent_tower/jump_boots_pickup/boots_pickup/sync11_boots_pickup_idsync", "idSyncEntity" },
    { "zion/designsystems/default", "idDesignSystemsEntity" },
    { "zion/designsystems/turrets/ceiling/mp_turret", "idDesignSystems_Turret" },
    { "zion/prop/breakables/mp_barrel", "idProp2" },
    { "zion/prop/breakables/mp_barrel_classic", "idProp2" },
    { "zion/prop/default", "idProp2" },
    { "zion/prop/weapons/sp/default", "idProp2" },
    { "zion/syncanims/player/human/base/motion/mechanics/gore_totem/gore_totem/sync11_gore_totem_idsync", "idSyncEntity" },
    { "zion/syncanims/player/human/base/motion/mechanics/kiosks/hell_tablet/hell_tablet/sync11_hell_tablet_idsync", "idSyncEntity" },
    { "zion/syncanims/player/human/base/motion/mechanics/kiosks/skull_alter/skull_alter/sync11_idsyncanimation", "idSyncEntity" },
    { "zion/syncanims/player/human/base/motion/mechanics/panels/touchscreen/touchscreen_tripod/sync11_touchscreen_tripod_idsync", "idSyncEntity" },
    { "zion/syncanims/player/human/base/motion/mechanics/portal/hell_challenge/hell_shrine/sync11_hell_shrine_idsync", "idSyncEntity" },
    { "zion/syncmelee/archvile", "idSyncEntity" },
    { "zion/syncmelee/baron", "idSyncEntity" },
    { "zion/syncmelee/cacodemon", "idSyncEntity" },
    { "zion/syncmelee/doommarine_pvp", "idSyncEntity" },
    { "zion/syncmelee/hellified_soldier", "idSyncEntity" },
    { "zion/syncmelee/hellified_soldier_beam", "idSyncEntity" },
    { "zion/syncmelee/hellknight", "idSyncEntity" },
    { "zion/syncmelee/imp", "idSyncEntity" },
    { "zion/syncmelee/mancubus", "idSyncEntity" },
    { "zion/syncmelee/mancubus_cyber", "idSyncEntity" },
    { "zion/syncmelee/pinky", "idSyncEntity" },
    { "zion/syncmelee/player_player_pvp_chainsaw", "idSyncEntity" },
    { "zion/syncmelee/revenant", "idSyncEntity" },
    { "zion/syncmelee/zombie", "idSyncEntity" },
    { "zion/syncmelee/zombie_hell", "idSyncEntity" },
    { "zion/syncmelee/zombie_uac_security", "idSyncEntity" },
    { "zion/syncmelee/zombie_welder", "idSyncEntity" },
};
#define SH_VCM_INHERIT_Y_N 272

#endif /* B2_VALID_CLASS_MAP_H */
