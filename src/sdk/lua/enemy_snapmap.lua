-- enemy_snapmap.lua: SnapMap's rules for a demon its own brain drives (the Enemy SDK's generic tier).
--
-- A placed demon is an idPlaceableSnapAIEncounter; the engine spawns the demon through its own encounter path, so
-- the health and damage modifiers, stagger, glory-kill-only, the team and the teleport-in reach the demon whatever
-- drives it, and every action that calls the engine on the demon (heal, hurt, kill, remove, hide, show, set team,
-- show health, teleport) acts on it. What the engine leaves to the stock AI's own state machine, which a
-- script-driven demon does not run, this module does (R-SM1, R-SM2):
--
-- Spawn Behavior (the engine writes it into the stock AI's settings, DOOM 2016 0x140737180):
--   default   the demon takes the nearest enemy it can see
--   ambush    the same, but only an enemy within 768 units (the engine's ambush perception radius)
--   hunt      the nearest enemy, seen or not (the engine forces awareness of the player)
--   defend    as default, and it holds a position within 512 units of where it was placed (the engine's defend
--             radius; its role is ROLE_DEFEND)
--
-- Follow orders (SnapMap's Follow action, idSnapMapAction_FollowPath, writes them into the demon's temp memory for
-- the stock FSM's snapmap state, which takes and removes them): e:snapmap_order() takes one the same way. While an
-- order holds, every move the brain asks for goes to the order's goal instead; its attacks stay the brain's own. The
-- tier never starts a move of its own: an animation the brain asks for during a move it did not start would cancel
-- that move (DOOM 2016 restarts the AI off locomotion, TY11), so the demon sets off at the brain's next move. A path
-- or position order ends on arrival (within ORDER_ARRIVE); an entity order holds while the entity lives; a new order
-- replaces it.
--
-- The engine's state is the truth: team and position are read from the engine every think, never kept. A jump of
-- the position no walk covers in one think (SnapMap's Teleport To, or the engine moving the demon) ends the order and
-- the current move, and a defender defends where it now stands.
--
-- An enemy is a player, or a demon of another team when the map's author gave this demon a team (the editor's Team
-- property: demons of two teams fight each other). The same team never fights itself, and the last attacker is taken
-- first.
--
-- Every read goes through the script API's general calls (e:get, e:class, entities, e:snapmap_order); on a Snapmap+
-- without them the demon behaves as "default" against players and hears no Follow order.

local P = {}

P.AMBUSH_RADIUS = 768
P.DEFEND_RADIUS = 512
P.SIGHT = 4096          -- how far a demon looks for demons of another team
P.LINK_WAIT = 3         -- seconds a new demon is looked for among the placed objects
P.ORDER_ARRIVE = 64     -- a path or position order is done this close to its goal
P.JUMP = 256            -- a move past this in one think is the engine's (a teleport): no demon walks it

local REAL = { TEAM_ONE = true, TEAM_TWO = true, TEAM_THREE = true, TEAM_FOUR = true }
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

--- The entity's team as the game names it (TEAM_ONE ... TEAM_NONE), or nil when unreadable.
function P.team(e) return field(e, "team") end

--- true when demon e fights `other`.
function P.hostile(e, other)
  if other == nil or other == e then return false end
  local mine, theirs = P.team(e), P.team(other)
  if REAL[mine] and mine == theirs then return false end
  if other:is_player() then return true end
  return (REAL[mine] and REAL[theirs]) and true or false
end

local function is_demon(x)
  local ok, t = pcall(function() return x:monster_type() end)
  return ok and t ~= nil
end

--- Give demon e an enemy when it has none (the engine gives a script-driven demon none).
-- b: the brain's record of e (b.attacker is the last entity that hurt it).
function P.acquire(A, e, b)
  if A.enemy(e) ~= nil then return end
  local s = P.of(e)
  local t = b and b.attacker or nil
  if t ~= nil and (not A.alive(t) or not P.hostile(e, t)) then t = nil end
  if t == nil then
    local best, best_d
    local function consider(x)
      if x == e or not A.alive(x) or not P.hostile(e, x) then return end
      local d = A.distance(e, x)
      if s.behavior == "ambush" and d > P.AMBUSH_RADIUS then return end
      if s.behavior ~= "hunt" and not A.can_see(e, x) then return end
      if best == nil or d < best_d then best, best_d = x, d end
    end
    local players = rawget(_G, "players")
    for _, p in ipairs(players and players() or {}) do consider(p) end
    if REAL[P.team(e)] then
      for _, x in ipairs(A.entities_near(A.position(e), P.SIGHT) or {}) do
        if not x:is_player() and is_demon(x) then consider(x) end
      end
    end
    t = best
  end
  if t ~= nil then A.set_enemy(e, t) end
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
-- taken, a teleport noticed, an order that arrived ended, and an enemy given when it has none (P.acquire). b: the
-- brain's record of e.
function P.think(A, e, b)
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
  P.acquire(A, e, b)
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
