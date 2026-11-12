-- test_enemy_placement.lua: the Enemy SDK's reading of a placed demon's editor properties and the
-- spawn behaviours a script-driven brain honours (enemy_placement.lua), over a fake map.
--   lua54 tests/sdk/test_enemy_placement.lua      (run from the repository root; tests/sdk/run.ps1 does)
-- The library under test is src/sdk/lua, or the folder SNAPMAP_SDK_LUA names.
local here = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]*$") or "."
package.path = (os.getenv("SNAPMAP_SDK_LUA") or (here .. "/../../src/sdk/lua")) .. "/?.lua;" .. package.path

local failed, cases = 0, 0
local function check(name, ok, detail)
  cases = cases + 1
  if ok then print("PASS " .. name) else failed = failed + 1; print("FAIL " .. name .. (detail and (": " .. tostring(detail)) or "")) end
end

-- ---- a fake map ----
local world = {}
local function ent(t)
  t.fields = t.fields or {}
  local e = {}
  function e:class() return t.class or "idEntity" end
  function e:get(path)
    local v = t.fields[path]
    if v == nil then error("has no field named " .. path) end
    return v
  end
  function e:pos() return t.pos end
  function e:is_player() return t.player == true end
  function e:monster_type() return t.demon and "heavy" or nil end
  function e:alive() return t.dead ~= true end
  function e:distance(o) local a, b = t.pos, o:pos(); return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end
  e.t = t
  world[#world + 1] = e
  return e
end
function entities() return world end
function players() local out = {} for _, e in ipairs(world) do if e:is_player() then out[#out + 1] = e end end return out end

local seen = {}          -- [a][b] = true: a can see b
local enemy = {}
local moves = {}
local function adapter()
  local A = {}
  A.enemy = function(e) return enemy[e] end
  A.set_enemy = function(e, t) enemy[e] = t end
  A.alive = function(t) return t ~= nil and t:alive() end
  A.pos = function(t) return t:pos() end
  A.distance = function(e, t) return e:distance(t) end
  A.can_see = function(e, t) return (seen[e] or {})[t] == true end
  A.entities_near = function(pos, r)
    local out = {}
    for _, x in ipairs(world) do
      local p = x:pos()
      if p and math.sqrt((p.x - pos.x) ^ 2 + (p.y - pos.y) ^ 2) <= r then out[#out + 1] = x end
    end
    return out
  end
  A.move_to = function(e, pos, flags) moves[#moves + 1] = { e = e, pos = pos } end
  A.move_to_entity = function(e, t, radius, flags) moves[#moves + 1] = { e = e, target = t } end
  A.encounter_role = function() return "ROLE_NONE" end
  return A
end

local function fresh()
  package.loaded["enemy_placement"] = nil
  world, seen, enemy, moves = {}, {}, {}, {}
  return require("enemy_placement")
end

local function placed(P, behavior, team, extra)
  local demon = ent({ class = "idAI2", demon = true, pos = { x = 0, y = 0, z = 0 },
                      fields = { team = team or "TEAM_NONE", ["aiEditable.spawnSettings.initialRole"] = extra and extra.role or "ROLE_NONE" } })
  ent({ class = "idPlaceableSnapAIEncounter", pos = { x = 0, y = 0, z = 0 }, fields = {
    entity = demon, spawnBehavior = "SNAP_AI_SPAWN_BEHAVIOR_" .. behavior, ["encounterMods.healthMod"] = 2.5,
    ["encounterMods.damageMod"] = 0.5, staggerEnabled = true, staggerPercentageOverride = 0.2, onlyDieFromGK = true,
    teleportSpawn = false, gameTeam = team or "TEAM_NONE" } })
  return demon
end

-- ---- what the editor says ----
do
  local P = fresh()
  local d = placed(P, "HUNT", "TEAM_TWO")
  local s = P.of(d)
  check("a placed demon's editor properties are read from its placeable",
        s.behavior == "hunt" and s.health_mod == 2.5 and s.damage_mod == 0.5 and s.stagger == true and
        s.stagger_at == 0.2 and s.glory_kill_only == true and s.teleport == false and s.placeable ~= nil)
  check("the record is read once per demon", P.of(d) == s)
  local lone = ent({ class = "idAI2", demon = true, pos = { x = 9, y = 9, z = 0 }, fields = { team = "TEAM_NONE" } })
  local l = P.of(lone)
  check("a demon no placeable spawned is default with modifiers of 1", l.behavior == "default" and l.health_mod == 1 and l.placeable == nil)
end

-- ---- the engine links the placed object after the spawn ----
do
  local P = fresh()
  local t = 10
  now = function() return t end
  local demon = ent({ class = "idAI2", demon = true, pos = { x = 0, y = 0, z = 0 }, fields = { team = "TEAM_NONE" } })
  local s = P.of(demon)
  check("at spawn, before the engine linked it, the demon reads as default", s.behavior == "default" and s.placeable == nil)
  ent({ class = "idPlaceableSnapAIEncounter", pos = { x = 0, y = 0, z = 0 }, fields = {
    entity = demon, spawnBehavior = "SNAP_AI_SPAWN_BEHAVIOR_AMBUSH" } })
  check("it is not looked for again within a quarter second", P.of(demon).behavior == "default")
  t = 10.3
  check("once linked, the next look finds its placed object", P.of(demon).behavior == "ambush" and P.of(demon) == s)
  local lone = ent({ class = "idAI2", demon = true, pos = { x = 5, y = 5, z = 0 }, fields = { team = "TEAM_NONE" } })
  local l = P.of(lone)
  t = 14
  check("a demon nothing placed is settled after three seconds", P.of(lone).settled == true and l.behavior == "default")
  now = nil
end

-- ---- who is an enemy ----
do
  local P = fresh()
  local d = placed(P, "DEFAULT", "TEAM_ONE")
  local player = ent({ player = true, pos = { x = 100, y = 0, z = 0 }, fields = { team = "TEAM_TWO" } })
  local mate = ent({ player = true, pos = { x = 100, y = 0, z = 0 }, fields = { team = "TEAM_ONE" } })
  local rival = ent({ class = "idAI2", demon = true, pos = { x = 50, y = 0, z = 0 }, fields = { team = "TEAM_TWO" } })
  local friend = ent({ class = "idAI2", demon = true, pos = { x = 50, y = 0, z = 0 }, fields = { team = "TEAM_ONE" } })
  local stray = ent({ class = "idAI2", demon = true, pos = { x = 50, y = 0, z = 0 }, fields = { team = "TEAM_NONE" } })
  check("a player of another team is an enemy", P.hostile(d, player))
  check("a player of the demon's own team is not", not P.hostile(d, mate))
  check("a demon of another team is an enemy", P.hostile(d, rival))
  check("a demon of the same team is not", not P.hostile(d, friend))
  check("a demon without a team is not", not P.hostile(d, stray))
  check("a demon without a team fights players only", P.hostile(stray, player) and not P.hostile(stray, rival))
  check("nothing fights itself", not P.hostile(d, d))
end

-- ---- spawn behaviours ----
local function scene(behavior, team)
  local P = fresh()
  local A = adapter()
  local d = placed(P, behavior, team)
  local near = ent({ player = true, pos = { x = 500, y = 0, z = 0 }, fields = { team = "TEAM_NONE" } })
  local far = ent({ player = true, pos = { x = 2000, y = 0, z = 0 }, fields = { team = "TEAM_NONE" } })
  return P, A, d, near, far
end

do
  local P, A, d, near, far = scene("DEFAULT")
  P.acquire(A, d, {})
  check("default: no enemy while it sees none", enemy[d] == nil)
  seen[d] = { [far] = true }
  P.acquire(A, d, {})
  check("default: the enemy it can see, however far", enemy[d] == far)
  enemy[d] = nil
  seen[d] = { [far] = true, [near] = true }
  P.acquire(A, d, {})
  check("default: the nearest of the enemies it sees", enemy[d] == near)
end

do
  local P, A, d, near, far = scene("AMBUSH")
  seen[d] = { [far] = true }
  P.acquire(A, d, {})
  check("ambush: an enemy beyond 768 units is left alone, seen or not", enemy[d] == nil)
  seen[d] = { [far] = true, [near] = true }
  P.acquire(A, d, {})
  check("ambush: an enemy within 768 units that it sees", enemy[d] == near)
  enemy[d] = nil
  seen[d] = {}
  P.acquire(A, d, { attacker = far })
  check("ambush: whoever hurt it, wherever they are", enemy[d] == far)
end

do
  local P, A, d, near, far = scene("HUNT")
  P.acquire(A, d, {})
  check("hunt: the nearest enemy without seeing it", enemy[d] == near)
  near.t.dead = true
  enemy[d] = nil
  P.acquire(A, d, {})
  check("hunt: the next one when that one is dead", enemy[d] == far)
end

do
  local P, A, d, near, far = scene("DEFEND")
  P.install(A)
  A.move_to(d, { x = 2000, y = 0, z = 5 })
  local m = moves[#moves].pos
  check("defend: a move beyond 512 units of its place is pulled back to 512", math.abs(m.x - 512) < 0.01 and m.y == 0 and m.z == 5)
  A.move_to(d, { x = 300, y = 400, z = 0 })
  m = moves[#moves].pos
  check("defend: a move inside its radius is left as it is", m.x == 300 and m.y == 400)
  A.move_to_entity(d, far, 100)
  m = moves[#moves]
  check("defend: chasing an enemy outside the radius stops at the radius", m.pos ~= nil and math.abs(m.pos.x - 512) < 0.01)
  A.move_to_entity(d, near, 100)
  check("defend: chasing an enemy inside the radius is the chase itself", moves[#moves].target == near)
  d.t.fields["aiEditable.spawnSettings.initialRole"] = "ROLE_DEFEND"
  check("the FSM reads the role the engine gave the demon", A.encounter_role(d) == "ROLE_DEFEND")
  seen[d] = { [far] = true }
  P.acquire(A, d, {})
  check("defend: it still takes the enemy it sees", enemy[d] == far)
end

do
  local P, A, d, near, far = scene("DEFAULT")
  P.install(A)
  A.move_to(d, { x = 2000, y = 0, z = 0 })
  check("default: moves are not held to a radius", moves[#moves].pos.x == 2000)
  check("default: no role", A.encounter_role(d) == "ROLE_NONE")
end

-- ---- teams: demons of two teams fight ----
do
  local P = fresh()
  local A = adapter()
  local d = placed(P, "DEFAULT", "TEAM_ONE")
  local rival = ent({ class = "idAI2", demon = true, pos = { x = 300, y = 0, z = 0 }, fields = { team = "TEAM_TWO" } })
  local friend = ent({ class = "idAI2", demon = true, pos = { x = 100, y = 0, z = 0 }, fields = { team = "TEAM_ONE" } })
  local player = ent({ player = true, pos = { x = 900, y = 0, z = 0 }, fields = { team = "TEAM_TWO" } })
  seen[d] = { [rival] = true, [friend] = true, [player] = true }
  P.acquire(A, d, {})
  check("a demon with a team takes the nearest enemy, demon or player, never its own team", enemy[d] == rival)
  enemy[d] = nil
  P.acquire(A, d, { attacker = friend })
  check("a hit from its own team does not make an enemy of it", enemy[d] == rival)
end

-- ---- a Snapmap+ without the general calls ----
do
  local P = fresh()
  local A = adapter()
  local save = entities
  entities = nil
  local d = ent({ demon = true, pos = { x = 0, y = 0, z = 0 } })
  d.get = nil; d.class = nil
  local player = ent({ player = true, pos = { x = 100, y = 0, z = 0 } })
  player.get = nil
  seen[d] = { [player] = true }
  local s = P.of(d)
  P.acquire(A, d, {})
  check("without e:get and entities() the demon is default against players", s.behavior == "default" and enemy[d] == player)
  entities = save
end

print(string.format("%d cases, %d passed, %d failed", cases, cases - failed, failed))
os.exit(failed == 0 and 0 or 1)
