-- enemy_snapmap.lua: SnapMap's rules for a demon its own brain drives (the Enemy SDK's generic tier). The engine
-- applies the encounter's modifiers, team and teleport-in whatever drives the demon; this module supplies what the
-- stock AI's state machine does itself: Spawn Behavior, Follow orders and the choice of an enemy.
--
-- Spawn Behavior (DOOM 2016 0x140737180): default, ambush (768-unit perception radius) and hunt act through the
-- engine's perception; defend also holds a position within 512 units of the placement (ROLE_DEFEND), done here.
-- The Enemy SDK leaves path-point orders to SnapMap's own state. Other Follow orders redirect the brain's movement
-- requests because an animation requested during a move the brain did not start cancels that move. Team and
-- position are read from the engine every think; a jump no walk covers ends the order and the current move.
-- Every read goes through the script API's general calls; without them the demon hears no order and finds no enemy.

local target = require("enemy_target")

local P = {}

P.DEFEND_RADIUS = 512
P.LINK_WAIT = 3         -- seconds a new demon is looked for among the placed objects
P.ORDER_ARRIVE = 64     -- a path or position order is done this close to its goal
P.JUMP = 256            -- a move past this in one think is the engine's (a teleport): no demon walks it

local records = setmetatable({}, { __mode = "k" })

local function field(e, path)
  local ok, v = pcall(function() return e:get(path) end)
  if ok then return v end
  return nil
end

local function word(v, prefix)
  if type(v) ~= "string" then return nil end
  return (v:gsub("^" .. prefix, "")):lower()
end

local function dist2d(a, b)
  local dx, dy = a.x - b.x, a.y - b.y
  return math.sqrt(dx * dx + dy * dy)
end

--- The placed object that spawned demon e, or nil (spawned by a script or a Spawn Encounter node).
function P.placeable(e)
  local list = rawget(_G, "entities")
  if not list then return nil end
  for _, x in ipairs(list()) do
    local ok, cls = pcall(function() return x:class() end)
    if ok and cls == "idPlaceableSnapAIEncounter" and field(x, "entity") == e then return x end
  end
  return nil
end

--- What the editor says about demon e. The engine links a placed object to its demon just after
-- the demon spawns (and sets its team then), so a demon without one is looked for again, four
-- times a second, for LINK_WAIT seconds; after that it is a demon no placed object spawned.
function P.of(e)
  local s = records[e]
  local clock = rawget(_G, "now")
  local t = clock and clock() or 0
  if s and (s.placeable or s.settled or t < s.look_at) then return s end
  if not s then
    s = { behavior = "default", home = e:position(), health_mod = 1, damage_mod = 1, first = t, look_at = 0 }
    records[e] = s
  end
  local p = P.placeable(e)
  if p then
    s.placeable = p
    s.behavior = word(field(p, "spawnBehavior"), "SNAP_AI_SPAWN_BEHAVIOR_") or "default"
    s.health_mod = field(p, "encounterMods.healthMod") or 1
    s.damage_mod = field(p, "encounterMods.damageMod") or 1
    s.stagger = field(p, "staggerEnabled")
    s.stagger_at = field(p, "staggerPercentageOverride")
    s.glory_kill_only = field(p, "onlyDieFromGK") == true
    s.teleport = field(p, "teleportSpawn") == true
  elseif not clock or t - s.first >= P.LINK_WAIT then
    s.settled = true
  else
    s.look_at = t + 0.25
  end
  return s
end

--- Give demon e 2016's enemy when it holds none in 2016's combat band (the engine gives a script-driven demon none:
-- its state machines pick, and a scripted demon runs none of them). An enemy it has lost sight of is kept.
function P.acquire(A, e)
  local t = A.enemy(e)
  if target.holds(A, e, t, target.COMBAT) then return end
  local _, pick = target.available(A, e, target.FIND)
  if pick ~= nil and pick ~= t then A.set_enemy(e, pick) end
end

--- pos, pulled back to within `radius` of `home` on the ground plane.
function P.clamp(pos, home, radius)
  local d = dist2d(pos, home)
  if d <= radius then return pos end
  local k = radius / d
  return { x = home.x + (pos.x - home.x) * k, y = home.y + (pos.y - home.y) * k, z = pos.z }
end

--- The goal of demon e's Follow order now (a position), or nil when none holds. An order whose target is gone
-- ends here.
function P.order_goal(A, e)
  local s = P.of(e)
  local o = s.order
  if not o then return nil end
  if o.kind == "position" then return o.pos end
  if o.target == nil or not A.alive(o.target) and o.kind == "entity" then s.order = nil return nil end
  local ok, pos = pcall(A.position, o.target)
  if not ok or pos == nil then s.order = nil return nil end
  return pos
end

--- One think of SnapMap's rules for demon e (call it each think before the brain decides): a new Follow order is
-- taken, a teleport noticed, an order that arrived ended, and 2016's enemy given when it holds none (P.acquire).
function P.think(A, e)
  local s = P.of(e)
  local pos = A.position(e)
  -- a teleport: the engine moved the demon farther than it walked
  if s.last and pos and dist2d(pos, s.last) > P.JUMP then
    s.order = nil
    s.home = pos
    if A.stop then A.stop(e) end
  end
  s.last = pos
  if type(e.snapmap_order) == "function" then
    local o = e:snapmap_order()
    if o then s.order = o end
  end
  local goal = P.order_goal(A, e)
  if goal and pos and s.order.kind ~= "entity" and dist2d(pos, goal) <= P.ORDER_ARRIVE then s.order = nil end
  P.acquire(A, e)
end

--- Make the adapter honour SnapMap: a Follow order's goal takes every move the brain asks for, a defending demon's
-- moves stay within its radius, and the FSM reads the role the engine gave the demon. Once per adapter.
function P.install(A)
  if A.snapmap_installed then return A end
  A.snapmap_installed = true
  local move_to, move_to_entity = A.move_to, A.move_to_entity
  A.move_to = function(e, pos, flags)
    local s = P.of(e)
    local goal = P.order_goal(A, e)
    if goal then pos = goal
    elseif s.behavior == "defend" and s.home and pos then pos = P.clamp(pos, s.home, P.DEFEND_RADIUS) end
    return move_to(e, pos, flags)
  end
  A.move_to_entity = function(e, t, radius, flags)
    local s = P.of(e)
    local goal = P.order_goal(A, e)
    if goal then return move_to(e, goal, flags) end
    if s.behavior == "defend" and s.home then
      local tp = A.position(t)
      if tp then
        local c = P.clamp(tp, s.home, P.DEFEND_RADIUS)
        if c ~= tp then return move_to(e, c, flags) end
      end
    end
    return move_to_entity(e, t, radius, flags)
  end
  A.encounter_role = function(e)
    if e == nil then return "ROLE_NONE" end
    local role = field(e, "aiEditable.spawnSettings.initialRole")
    if type(role) == "string" then return role end
    return P.of(e).behavior == "defend" and "ROLE_DEFEND" or "ROLE_NONE"
  end
  return A
end

return P
