-- enemy_sdk.lua: binds a brain, a table of callbacks (spawn, think, damage, death, remove, pain, projectile and
-- frame; an optional bind(def) runs first), to a demon's entityDef. Every demon placed or spawned from it gets its
-- own machine; pain, stagger and death clips stay the engine's.

local S = {}

-- the animation-layer ae_notify rows a brain hears
S.FRAME_EVENTS = { "fsm_interrupt_window", "blackboard", "blackboard_remove", "end_spline", "laser_state",
                   "pull_trigger", "equip", "laser_autostop", "eternal" }

-- Binds brain's callbacks to the events of entityDef `def` (a name) and returns brain.
function S.bind(def, brain)
  -- brain.bind names what the game needs for the entityDef before any demon of it spawns (its breakables)
  if brain.bind then brain.bind(def) end
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
