-- enemy_weapon.lua: how a demon of the Enemy SDK holds and fires its weapon.
--
--   local W = require("enemy_weapon")
--   W.equip(e, "weapon/ai/tyrant/cannon")      -- by decl name or inventory index; answers the hand
--   W.fire_mode(e, "FIREWHENREADY")            -- the engine's fire control takes it from here
--   W.tick(e, dt)                              -- once a frame: keeps the aim focus on the enemy
--
-- This library keeps no fire state of its own. Every question it answers is read from the game's
-- fire control and every order it gives is one of the game's own events, so a demon built on it
-- fires exactly as a stock demon does. The game's fire control does all of this by itself, once a
-- demon holds a weapon, has an aim focus and is in a firing mode; nothing below is to be written in
-- Lua:
--
--   The pull. In FIREWHENREADY and SPRAYWHENREADY the fire control asks its fire gate (the game's
--   CheckPullTrigger) every frame the trigger is released, and pulls the trigger when the gate
--   passes. The gate checks: an attack token; the weapon's wait since its last burst; that the aim
--   focus has a target the demon is aware of and has seen lately; that the aim point is inside the
--   weapon's maxRange; that the aim tracker has caught up with the target (the barrel points within
--   about 25 degrees of it); and the muzzle test (below). SPRAYWHENREADY is the same with free ammo.
--
--   The muzzle gate. The engine casts a ray from the muzzle (the model's muzzlereference tag; the
--   weapon's own muzzle when the model has none) and asks whether it passes through a sphere at the
--   aim focus point. The sphere is the target's half width (aimFocusTargetRadius) times 3, halved at
--   the pull and one and a half times while a burst runs. With no valid aim focus the test fails,
--   so a demon with no focus never fires; W.focus gives it one.
--
--   The burst. The weapon decl's skillSettings (picked by difficulty) give the burst length (min and
--   max burst duration) and the pause after it (min and max repull interval); the demon's
--   aibehavior weaponUseBehaviorList gives the shortest hold by distance. The engine draws the
--   burst, keeps the trigger down while the gate still passes and the burst runs, then releases it
--   and starts the pause. A burst also ends when the enemy dies or the weapon must reload.
--
--   The shots. While the trigger is down and the weapon is ready the engine test-fires each frame
--   (the muzzle direction against the aim direction) and launches the weapon decl's projectile,
--   with the weapon's firingInterval between shots. Pull and release are the engine's; so is each
--   projectile (on.projectile hears them).
--
--   The aim. The aim tracker turns the barrel (the fkGraph chain and the trackingparms decl) toward
--   the aim focus point within its trackerRange, and the engine's idle turn in place turns the body
--   when the focus leaves that yaw range (W.hold orders it).
--
-- What stays with the decls, not with this file: the weapon decl's skillSettings, firingInterval
-- and maxRange; a weaponUseBehaviorList entry for the weapon; the model's muzzlereference tag; an
-- fkGraph and a trackingParmsList entry for the aim; a turnParms entry for the idle turn type the
-- demon holds with. W.hold says when the last one is missing.
--
-- Fields of the engine's state a script can read by name with e:get when it needs more than this
-- library gives: aiVolatile.fireControl.fireMode_state, .fireMode_anim, .requireMuzzleOnTarget,
-- .weaponStates[n].nextPullTriggerTime / .endBurstTime; aiVolatile.focus.foci[1].finalFocusPoint and
-- .finalFocusPointIsValid, aiVolatile.focus.aimFocusTargetRadius and .muzzleAimPrecision.

local W = {}

W.MODES = { "HOLDFIRE", "WEAPON_EXPLICIT_RELEASE", "WEAPON", "FIREWHENREADY", "SPRAYWHENREADY" }

local function memory(e)
  local s = e.state
  s.weapon = s.weapon or {}
  return s.weapon
end

--- Put an item in the demon's hand: its decl name (as its startingInventory names it) or its
-- inventory index. Answers the hand the game put it in, "left" or "right".
function W.equip(e, item)
  return e:equip(item)
end

--- The engine's fire mode. W.fire_mode(e, "FIREWHENREADY") sets it (a name with or without the
-- AIFIREMODE_ prefix, or 0 to 4); W.fire_mode(e) answers the mode the script events set and the
-- mode the animation events set, as the game's names.
function W.fire_mode(e, mode)
  if mode == nil then return e:fire_mode() end
  return e:fire_mode(mode)
end

--- The engine's fire control pulls the trigger and draws the burst from the weapon decl (the game's
-- ae_aIPullTrigger path; a bare pullTrigger event would skip the fire control and a demon in the WEAPON
-- mode would release at once without a shot). With check = true the game's fire gate runs first and the trigger
-- is pulled only when it passes. Answers whether the trigger is held afterwards.
function W.pull(e, check)
  return e:pull_trigger(check and true or false)
end

--- The engine releases the trigger.
function W.release(e)
  e:release_trigger()
end

--- Pull (on) or release (not on). The shape the adapter's trigger has.
function W.trigger(e, on, check)
  if on then return W.pull(e, check) end
  W.release(e)
end

--- The trigger is held now: the weapon's own trigger state.
function W.pulled(e)
  return e:trigger_pulled()
end

--- The weapon's wait since its last burst has passed: the engine's own next-pull time against the
-- game clock.
function W.ready(e)
  return e:weapon_ready()
end

--- The engine's muzzle test: whether a shot along the barrel would pass through the sphere at the
-- aim focus point. test is "pull" (default), "firing" or "plain". Answers the engine's answer
-- (nil when this session cannot ask it) and a table of what it read: focus_valid, point, radius,
-- precision, require_muzzle, tracking, body_rotation.
function W.on_target(e, test)
  return e:muzzle_on_target(test)
end

--- The engine's aim focus: an entity (tracked), a position (fixed) or nil (cleared). Without a
-- valid focus the engine's fire gate never passes.
function W.focus(e, target)
  e:focus(target)
  local m = memory(e)
  m.fixed = type(target) == "table"      -- a fixed point is the script's to move, not tick's
  m.focus = (not m.fixed) and target or nil
end

--- Stand the demon still the engine's way and let the engine turn it toward its focus: ends the
-- pending move, orders the engine's idle with a turn in place type ("RELAXED", "COMBAT" the
-- default, "INCOVER"). Answers true when the engine will turn it, else false and why.
function W.hold(e, turn)
  return e:hold(turn)
end

--- What the engine holds for this demon's weapon, as one table: fire mode, animation fire mode,
-- trigger, wait over, aim focus, the muzzle test and its inputs. For a probe line.
function W.state(e)
  local mode, anim = e:fire_mode()
  local on, info = e:muzzle_on_target()
  info = info or {}
  return { mode = mode, anim_mode = anim, pulled = e:trigger_pulled(), ready = e:weapon_ready(),
           focus_valid = info.focus_valid, on_target = on, radius = info.radius, precision = info.precision,
           require_muzzle = info.require_muzzle, point = info.point }
end

--- Once a frame. The engine's aim focus tracks the entity it was given and nothing else, so when the
-- demon's enemy changes the focus follows it (unless the script gave a fixed point). Everything else
-- a frame brings (the pull, the burst, the release, the shots) is the engine's.
function W.tick(e, dt)
  local ok, enemy = pcall(e.enemy, e)
  if not ok or not enemy then return end
  local m = memory(e)
  if not m.fixed and m.focus ~= enemy then
    e:focus(enemy)
    m.focus = enemy
  end
end

return W
