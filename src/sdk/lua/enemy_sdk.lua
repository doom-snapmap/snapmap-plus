-- enemy_sdk.lua: bind a brain (enemy_brain.lua) to a demon's entityDef. A demon's entry script is
-- three lines:
--
--   api = 1
--   local sdk = require("enemy_sdk")
--   sdk.bind("ai/demon/tyrant", require("tyrant_brain").new())
--
-- Every demon placed or spawned from the entityDef gets its own machine; pain, stagger and death
-- clips stay the engine's.

local S = {}

-- the animation layer's ae_notify rows a brain hears (doomforge.animlayer, the TY5 scheme)
S.FRAME_EVENTS = { "fsm_interrupt_window", "blackboard", "blackboard_remove", "end_spline", "laser_state",
                   "pull_trigger", "equip", "laser_autostop", "eternal" }

function S.bind(def, brain)
  on.spawn(def, function(self) brain.spawn(self) end)
  on.think(def, function(self, dt) brain.think(self, dt) end)
  on.damage(def, function(self, hit) brain.damage(self, hit) end)
  on.death(def, function(self) brain.death(self) end)
  on.remove(def, function(self) brain.remove(self) end)
  on.pain(def, function(self, reaction, damage, attacker) brain.pain(self, reaction, damage, attacker) end)
  on.projectile(def, function(self, p, decl, origin, dir) brain.projectile(self, p, decl, origin, dir) end)
  for _, command in ipairs(S.FRAME_EVENTS) do
    on.frame_event(def, command, function(self, args) brain.frame(self, command, args) end)
  end
  return brain
end

return S
