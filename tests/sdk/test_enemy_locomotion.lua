-- test_enemy_locomotion.lua: the Enemy SDK's locomotion modes (enemy_locomotion.lua) in a box room:
-- floor z = 0, ceiling z = 600, walls at x = +-1000 and y = +-1000.
--   lua54 tests/sdk/test_enemy_locomotion.lua     (run from the repository root; tests/sdk/run.ps1 does)
-- The library under test is src/sdk/lua, or the folder SNAPMAP_SDK_LUA names.
local here = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]*$") or "."
package.path = (os.getenv("SNAPMAP_SDK_LUA") or (here .. "/../../src/sdk/lua")) .. "/?.lua;" .. package.path
local L = require("enemy_locomotion")
local V = L.vec

local failed, cases = 0, 0
local function check(name, ok, detail)
  cases = cases + 1
  if ok then print("PASS " .. name) else failed = failed + 1; print("FAIL " .. name .. (detail and (": " .. tostring(detail)) or "")) end
end
local function near(a, b, eps) return math.abs(a - b) <= (eps or 0.01) end

-- ---- the room and the adapter ----
local PLANES = {
  { n = V.new(0, 0, 1), d = 0 }, { n = V.new(0, 0, -1), d = -600 },
  { n = V.new(1, 0, 0), d = -1000 }, { n = V.new(-1, 0, 0), d = -1000 },
  { n = V.new(0, 1, 0), d = -1000 }, { n = V.new(0, -1, 0), d = -1000 },
}
local blockers = {}          -- extra planes (a deck, a pillar face) that stop sight lines
local demon_boxes = {}       -- planes only a demon trace sees (a Blocking Box with Block Demons)
local unreachable, detour = false, 1
local dead = {}              -- demons the adapter calls dead
local move_calls = 0
local function trace(from, to, also)
  local best
  local dir = V.sub(to, from)
  for _, list in ipairs({ PLANES, blockers, also or {} }) do
    for _, pl in ipairs(list) do
      local a, b = V.dot(pl.n, from) - pl.d, V.dot(pl.n, to) - pl.d
      if a > 0.001 and b < 0 then
        local t = a / (a - b)
        if not best or t < best.t then best = { t = t, pos = V.add(from, V.mul(dir, t)), normal = pl.n } end
      end
    end
  end
  return best
end

local clock, moving, placed, held, things = 0, {}, {}, {}, {}
local function adapter()
  local A = {}
  A.now = function() return clock end
  A.pos = function(e) return e.pos end
  A.forward = function(e) return e.forward end
  A.def = function(e) return e.def end
  A.alive = function(e) return not dead[e] end
  A.trace = function(from, to) return trace(from, to) end
  A.nav_ground = function(p) return V.new(p.x, p.y, 0) end
  A.entities_near = function() return things end
  A.move_to = function(e, pos) move_calls = move_calls + 1; moving[e] = pos end
  A.move_done = function(e) return moving[e] == nil end
  A.stop = function(e) moving[e] = nil end
  A.place = function(e, pos, angles) e.pos = pos; e.angles = angles; placed[#placed + 1] = { e = e, pos = pos, angles = angles } end
  A.surface_hold = function(e, on) held[e] = on end
  A.path_to = function(e, point) if unreachable then return nil end return V.len(V.sub(point, e.pos)) * detour end
  A.trace_demon = function(from, to) return trace(from, to, demon_boxes) end
  return A
end
local function demon(x, y) return { pos = V.new(x, y, 0) } end
local function reset()
  clock, moving, placed, held, things, blockers = 0, {}, {}, {}, {}, {}
  demon_boxes, unreachable, detour, dead, move_calls = {}, false, 1, {}, 0
end
local function run(loco, e, seconds)
  for _ = 1, math.floor(seconds * 10 + 0.5) do clock = clock + 0.1; loco:tick(e, 0.1) end
end
local function perch_loco(perch) return L.new(adapter(), { modes = { "ground", "perch" }, perch = perch }) end

-- ---- surfaces and angles ----
check("a normal pointing up is a floor, down a ceiling, sideways a wall",
      L.kind_of(V.new(0, 0, 1)) == "floor" and L.kind_of(V.new(0, 0, -1)) == "ceiling" and L.kind_of(V.new(-1, 0, 0)) == "wall")
do
  local a = L.surface_angles(V.new(0, 0, 1), V.new(0, 1, 0))
  check("on the floor facing +y: yaw 90, no pitch, no roll", near(a.yaw, 90) and near(a.pitch, 0) and near(a.roll, 0))
  a = L.surface_angles(V.new(0, 0, -1), V.new(1, 0, 0))
  check("on the ceiling: upside down (roll 180)", near(math.abs(a.roll), 180) and near(a.pitch, 0))
  a = L.surface_angles(V.new(-1, 0, 0), V.new(0, 0, 1))
  check("on a wall facing up it: pitched 90 up", near(a.pitch, -90))
  a = L.surface_angles(V.new(-1, 0, 0), V.new(-1, 0, 0))
  check("facing straight out of the surface still gives angles", a.yaw == a.yaw and a.pitch == a.pitch)
end

-- ---- modes ----
do
  reset()
  local ok = pcall(L.new, adapter(), { modes = { "ground", "swim" } })
  check("a mode nobody provides is refused by name", not ok)
  local loco = L.new(adapter(), { modes = { "ground" } })
  local e, t = demon(0, 0), demon(500, 0)
  check("a demon starts on the floor in ground mode", loco:mode(e) == "ground" and loco:support(e).kind == "floor")
  check("a brain without the perch mode finds no perch", loco:find("perch", e, t) == nil and #loco:find_all("perch", e, t) == 0)
  loco:go(e, loco:find("ground", e, t))
  check("ground: the engine's move_to, done when the engine says so", moving[e] == t.pos and not loco:arrived(e))
  moving[e] = nil
  check("ground: arrived", loco:arrived(e))
  check("ground: grounded", loco:grounded(e))
end

-- ---- finding spots ----
do
  reset()
  local loco = perch_loco()
  local e, t = demon(600, 0), demon(-200, 0)
  local spots = L.perch_spots(loco, e, t)
  local kinds = {}
  for _, s in ipairs(spots) do kinds[s.kind] = (kinds[s.kind] or 0) + 1 end
  check("walls and the ceiling around the demon are found", (kinds.wall or 0) >= 1 and (kinds.ceiling or 0) >= 1, #spots)
  local ok = true
  for _, s in ipairs(spots) do
    if s.kind == "wall" and not near(math.max(math.abs(s.pos.x), math.abs(s.pos.y)), 1000 - 96, 0.5) then ok = false end
    if s.kind == "ceiling" and not near(s.pos.z, 600 - 96, 0.5) then ok = false end
    if s.base.z ~= 0 then ok = false end
  end
  check("a spot is a body radius off its surface, with a floor point to mount from", ok)
  check("the best spot is the one nearest the enemy", spots[1].score <= spots[#spots].score)
  local other = demon(0, 0)
  loco.claims[spots[1].id] = other
  local again = L.perch_spots(loco, e, t)
  check("a spot another demon holds is left out (one demon per spot)", again[1].id ~= spots[1].id)
  loco.claims[spots[1].id] = nil
  blockers = { { n = V.new(0, 0, 1), d = 300 } }                                      -- a deck at z = 300 over the enemy
  t.pos = V.new(0, 0, 50)
  local blocked = L.perch_spots(loco, e, t)
  local seen_ceiling = false
  for _, s in ipairs(blocked) do if s.kind == "ceiling" then seen_ceiling = true end end
  check("a spot without a clear line to the enemy is left out", not seen_ceiling)
end

-- ---- navigation and blocking volumes decide ----
do
  reset()
  local loco = perch_loco()
  local e, t = demon(600, 0), demon(-200, 0)
  local open = #L.perch_spots(loco, e, t)
  unreachable = true
  check("a spot the engine's navigation cannot walk the demon to is left out", #L.perch_spots(loco, e, t) == 0 and open > 0)
  unreachable, detour = false, 5
  check("a spot whose walk is a long way round is left out", #L.perch_spots(loco, e, t) == 0)
  detour = 1
  demon_boxes = { { n = V.new(0, 0, -1), d = -150 } }       -- a demon-blocking box over the whole room at z = 150
  local under = L.perch_spots(loco, e, t)
  local high = false
  for _, s in ipairs(under) do if s.pos.z > 150 then high = true end end
  check("nothing above a box that blocks demons is a spot, though a shot passes it", not high)
end

do
  reset()
  local loco = perch_loco()
  local e, t = demon(600, 0), demon(-200, 0)
  local dest = loco:find("perch", e, t)
  loco:go(e, dest)
  moving[e] = nil
  e.pos = V.new(dest.base.x + 400, dest.base.y, 0)            -- the engine's move ended short of the floor point
  loco:tick(e, 0.1)
  check("a move that stops short gives the perch up: no jump from the wrong place",
        loco:mode(e) == "ground" and #placed == 0 and loco.claims[dest.id] == nil and loco:find("perch", e, t) == nil)
end

-- ---- a Perch object the map's author placed ----
do
  reset()
  local loco = perch_loco()
  local e, t = demon(0, 0), demon(-300, -300)
  things = { { def = "mods/abcd/info/perch", pos = V.new(-1000, -300, 300), forward = V.new(1, 0, 0) } }
  local found = false
  for _, s in ipairs(L.perch_spots(loco, e, t)) do if s.id:sub(1, 7) == "placed:" then found = s end end
  check("a placed Perch object is a spot, facing out of its surface", found and found.kind == "wall" and near(found.pos.x, -1000 + 96))
end

-- ---- request 1: the caller's parameters are honoured ----
do
  reset()
  local loco = perch_loco()
  local e, t = demon(600, 0), demon(-200, 0)
  local all = loco:find_all("perch", e, t)
  local far = loco:find_all("perch", e, t, { minDistanceFromEnemy = 800 })
  local ok = #far > 0 and #far < #all
  for _, s in ipairs(far) do if s.distance < 800 then ok = false end end
  check("minDistanceFromEnemy keeps only spots that far from the target", ok, #far .. " of " .. #all)
  local near_ = loco:find_all("perch", e, t, { maxDistanceFromEnemy = 900 })
  ok = #near_ > 0 and #near_ < #all
  for _, s in ipairs(near_) do if s.distance > 900 then ok = false end end
  check("maxDistanceFromEnemy keeps only spots that near the target", ok)
  check("a distance band nothing fits gives an empty list", #loco:find_all("perch", e, t, { minDistanceFromEnemy = 5000 }) == 0)

  -- a deck at z = 60 hides the target's feet and chest from every spot above it: one of three samples is clear
  blockers = { { n = V.new(0, 0, 1), d = 60 } }
  local vis = loco:find_all("perch", e, t)
  check("a spot that sees a third of the target is a candidate, with its visibility measured",
        #vis > 0 and near(vis[1].visibility, 1 / 3, 0.001))
  check("minVisibility 0.4 refuses a spot that sees a third", #loco:find_all("perch", e, t, { minVisibility = 0.4 }) == 0)
  check("minVisibility 0.3 accepts it", #loco:find_all("perch", e, t, { minVisibility = 0.3 }) == #vis)
  blockers = { { n = V.new(0, 0, 1), d = 200 } }               -- a deck above every sample: nothing seen
  check("a target no sample of which is seen leaves no spot", #loco:find_all("perch", e, t) == 0)
  check("minVisibility 0 asks for no line of sight", #loco:find_all("perch", e, t, { minVisibility = 0 }) > 0)
  blockers = {}

  -- travel: path length over a walk speed
  local spots = loco:find_all("perch", e, t, { walkSpeed = 100 })
  check("travel is the walk's length over the walk speed", near(spots[1].travel, spots[1].path / 100, 0.001) and spots[1].path > 0)
  check("without a walk speed the travel is not known", loco:find_all("perch", e, t)[1].travel == nil)
  local longest = 0
  for _, s in ipairs(spots) do if s.travel > longest then longest = s.travel end end
  local cut = loco:find_all("perch", e, t, { walkSpeed = 100, maxTravelTime = longest - 0.01 })
  check("maxTravelTime drops the spots whose walk takes longer", #cut > 0 and #cut < #spots)
  check("maxTravelTime is not applied when no walk speed is known", #loco:find_all("perch", e, t, { maxTravelTime = 0.001 }) == #all)
  detour = 2
  check("maxDetour is the caller's limit on a winding walk", #loco:find_all("perch", e, t, { maxDetour = 1.5 }) == 0 and #loco:find_all("perch", e, t) > 0)
  detour = 1

  -- clearance, search range, statistics
  check("searchRadius bounds the search round the demon", #loco:find_all("perch", e, t, { searchRadius = 50 }) == 0)
  local wide = loco:find_all("perch", e, t, { bodyRadius = 150 })
  ok = #wide > 0
  for _, s in ipairs(wide) do
    if s.kind == "ceiling" and not near(s.pos.z, 600 - 150, 0.5) then ok = false end
  end
  check("bodyRadius is the clearance the spot keeps off its surface", ok)
  demon_boxes = { { n = V.new(0, 0, -1), d = -150 } }
  local stats = {}
  loco:find_all("perch", e, t, { stats = stats })
  check("stats counts what was rejected and why", (stats["no room"] or 0) + (stats["jump blocked"] or 0) > 0)
  demon_boxes = {}
end

-- ---- request 2: all candidates, each with the data a scorer needs ----
do
  reset()
  local loco = perch_loco()
  local e, t = demon(600, 0), demon(-200, 0)
  local all = loco:find_all("perch", e, t)
  local ids, ok = {}, true
  for _, s in ipairs(all) do
    if ids[s.id] then ok = false end
    ids[s.id] = true
    if not (s.pos and s.normal and s.kind and s.node and s.id and s.path and s.visibility and s.distance and s.score) then ok = false end
  end
  check("every valid candidate is returned once, with the data a scorer needs", #all > 8 and ok, #all)
  check("the walk target is named: node is the floor point", near(all[1].node.z, 0) and all[1].node == all[1].base)
  local sorted = true
  for i = 2, #all do if all[i - 1].score > all[i].score then sorted = false end end
  check("the list is best first by the library's own score", sorted and loco:find("perch", e, t).id == all[1].id)
  check("a find_all changes nothing: no claim, no move", next(loco.claims) == nil and move_calls == 0)
end

-- ---- request 3: reserving at choice time ----
do
  reset()
  local loco = perch_loco()
  local e, f, t = demon(600, 0), demon(0, 600), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  check("a demon reserves the spot it chose", loco:reserve(e, spot) == true and loco:holder(spot) == e and loco:reservation(e) == spot)
  check("nothing has moved by the reservation alone", move_calls == 0 and loco:mode(e) == "ground")
  local ok, why = loco:reserve(f, spot)
  check("another demon cannot reserve it", ok == false and why ~= nil)
  local ids = {}
  for _, s in ipairs(loco:find_all("perch", f, t)) do ids[s.id] = true end
  check("another demon's search does not return it", not ids[spot.id])
  local other = loco:find_all("perch", e, t)
  local second
  for _, s in ipairs(other) do if s.id ~= spot.id then second = s break end end
  check("the holder still sees its own spot", (function() for _, s in ipairs(other) do if s.id == spot.id then return true end end end)())
  loco:reserve(e, second)
  check("reserving another spot drops the first", loco:holder(spot) == nil and loco:holder(second) == e)
  loco:release(e)
  check("release frees the spot and the demon", loco:holder(second) == nil and loco:reservation(e) == nil)
  loco:reserve(e, spot)
  dead[e] = true
  check("a holder the adapter calls dead holds nothing", loco:holder(spot) == nil and loco:reserve(f, spot) == true)
  dead[e] = nil
  loco:release(f)
  loco:reserve(e, spot)
  loco:forget(e)
  check("forget frees a removed demon's spot", loco:holder(spot) == nil and loco:reservation(e) == nil)
end

-- ---- request 4: the walk target and the arrival test ----
do
  reset()
  local loco = perch_loco()
  local e, t = demon(600, 0), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  loco:reserve(e, spot)
  moving[e] = spot.node                                        -- the brain's ordinary move to the node
  check("not at the node while the move runs", loco:at_node(e) == false)
  moving[e] = nil
  check("not at the node while the demon is far from it", loco:at_node(e) == false)
  e.pos = V.new(spot.node.x + 23, spot.node.y, spot.node.z)
  check("at the node within the radius (23 of 24 in)", loco:at_node(e) == true)
  e.pos = V.new(spot.node.x + 30, spot.node.y, spot.node.z)
  local at, d = loco:at_node(e)
  check("not at the node beyond it (30 in), and the distance is answered", at == false and near(d, 30, 0.001))
  check("a radius the caller gives is used", loco:at_node(e, 31) == true)
  e.pos = spot.node
  loco.claims[spot.id] = demon(0, 0)
  check("not at the node when another demon holds the spot", loco:at_node(e) == false)
  loco.claims[spot.id] = e
  check("at the node again once the demon holds it", loco:at_node(e) == true)
end

-- ---- request 8: the spot's validity ----
do
  reset()
  local loco = perch_loco()
  local e, f, t = demon(600, 0), demon(0, 600), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  loco:reserve(e, spot)
  check("a free spot with room, a free jump and a view is valid", loco:spot_valid(e, spot, t) == true)
  loco.claims[spot.id] = f
  local ok, why = loco:spot_valid(e, spot, t)
  check("a spot another demon took is not valid", ok == false and why == "held by another demon")
  loco.claims[spot.id] = e
  demon_boxes = { { n = V.new(0, 0, -1), d = -(spot.node.z + 32 + 1) } }   -- a demon-blocking box over the node
  ok, why = loco:spot_valid(e, spot, t)
  check("a blocking volume put in the jump makes it invalid", ok == false and (why == "jump blocked" or why == "no room"), why)
  demon_boxes = {}
  blockers = { { n = V.new(0, 0, 1), d = 200 } }
  ok, why = loco:spot_valid(e, spot, t)
  check("a spot that lost its view of the target is not valid", ok == false and why == "not seen")
  check("without a target the view is not asked", loco:spot_valid(e, spot) == true)
  blockers = {}
end

-- ---- requests 5, 7: mounting from the node with no second walk; the grounded signal ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 1 })
  local e, t = demon(600, 0), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  check("a demon far from the node cannot mount", loco:mount(e, spot) == false and loco:mode(e) == "ground")
  e.pos = V.new(spot.node.x + 10, spot.node.y, 0)
  local before = move_calls
  local ok = loco:mount(e, spot, t.pos)
  check("a demon at the node mounts without a second walk", ok == true and move_calls == before and moving[e] == nil)
  check("mounting reserves the spot", loco:holder(spot) == e and loco:mode(e) == "perch")
  check("while mounting the library places the demon: not grounded, traversing", loco:grounded(e) == false and held[e] == true and loco:traversing(e) == true)
  run(loco, e, 0.5)
  check("half way up it is placed off the floor", #placed > 0 and e.pos.z > 1)
  run(loco, e, 0.6)
  check("held on the surface when the mount ends", loco:arrived(e) and near(V.len(V.sub(e.pos, spot.pos)), 0) and loco:support(e).kind == spot.kind)
  check("holding is not grounded, and no jump is under way", loco:grounded(e) == false and loco:traversing(e) == false)
  local f = demon(0, 600)
  check("a second demon cannot mount a held spot", loco:mount(f, spot) == false)
  local count = 0
  loco:leave(e, function(who) count = count + 1; check("the callback names the demon", who == e) end)
  check("during the dismount: not grounded, traversing, callback not yet called", loco:grounded(e) == false and loco:traversing(e) == true and count == 0 and loco:mode(e) == "perch")
  run(loco, e, 0.95)
  check("not grounded a moment before the dismount ends", count == 0 and loco:grounded(e) == false)
  run(loco, e, 0.2)
  check("grounded the frame the dismount ends, the callback called once",
        loco:grounded(e) == true and count == 1 and loco:mode(e) == "ground" and held[e] == false and near(e.pos.z, 0))
  run(loco, e, 0.5)
  check("the callback is not called again", count == 1)
  count = 0
  loco:leave(e, function() count = count + 1 end)
  check("leaving from the ground answers at once", count == 1)
end

-- ---- request 6: the facing indices ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 0.2 })
  local e, t = demon(600, 0), demon(-200, 0)
  local wall
  for _, s in ipairs(loco:find_all("perch", e, t)) do if s.kind == "wall" and s.normal.x == -1 then wall = s break end end
  local function at(dx, dy, dz) return V.add(wall.pos, V.new(dx, dy, dz)) end
  check("the facing count is the spec's (8 by default)", loco:facing_count() == 8 and perch_loco({ facings = 12 }):facing_count() == 12)
  loco:reserve(e, wall)
  check("a reserved spot answers a facing index: 0 is up a wall", loco:facing_index(e, at(0, 0, 500)) == 0)
  check("indices grow counter-clockwise seen from outside the wall", loco:facing_index(e, at(0, 500, 0)) == 2 and loco:facing_index(e, at(0, -500, 0)) == 6)
  check("a direction between two facings rounds to the nearer", loco:facing_index(e, at(0, -100, 500)) == 0 and loco:facing_index(e, at(0, 500, 300)) == 1)
  check("a count the caller gives is used", loco:facing_index(e, at(0, 500, 0), 4) == 1)
  check("straight out of the surface has no facing", loco:facing_index(e, at(500, 0, 0)) == nil)
  check("with no spot there is no facing index", loco:facing_index(demon(0, 0), at(0, 0, 500)) == nil)
  e.pos = V.new(wall.node.x, wall.node.y, 0)
  loco:mount(e, wall, 2)
  check("a mount facing by index holds that facing", loco:facing(e) == 2)
  check("set_facing_index turns the held facing", loco:set_facing_index(e, 5) == true and loco:facing(e) == 5)
  run(loco, e, 0.5)
  local a = placed[#placed].angles
  loco:set_facing_index(e, 0)
  run(loco, e, 0.1)
  local up = placed[#placed].angles
  check("the held pose follows the facing (index 0 faces up the wall)", near(up.pitch, -90, 0.5) and not near(a.pitch, -90, 1))
  loco:face(e, at(0, -500, 0))
  check("face(point) holds a point exactly; its index is read back", loco:facing(e) == 6)
  local ceil
  for _, s in ipairs(loco:find_all("perch", demon(0, 0), t, { ignoreCooldown = true })) do if s.kind == "ceiling" then ceil = s break end end
  local cp = V.add(ceil.pos, V.new(500, 0, 0))
  check("on a ceiling index 0 is along +x", L.facing_index_of(ceil, cp, 8) == 0 and L.facing_index_of(ceil, V.add(ceil.pos, V.new(0, -500, 0)), 8) == 2)
  local d = L.facing_direction(ceil, 2, 8)
  check("facing_direction is the unit vector of an index", near(V.len(d), 1, 1e-6) and near(d.y, -1, 1e-6))
end

-- ---- request 9: the library never ends a perch by itself ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 0.5 })
  local e, t = demon(600, 0), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  e.pos = spot.node
  loco:mount(e, spot, t.pos)
  run(loco, e, 1)
  check("the demon holds the surface", loco:arrived(e))
  check("seconds at the spot is a query", near(loco:seconds_at_spot(e), 0.5, 0.11))
  run(loco, e, 120)
  check("two minutes later it still holds: nothing expires", loco:arrived(e) and loco:grounded(e) == false and loco:holder(spot) == e)
  check("the library has no expiry call", loco.perch_expired == nil)
  check("seconds at the spot keeps counting", loco:seconds_at_spot(e) > 120)
  check("the held demon is on the ground again only when told", loco:mode(e) == "perch")
end

-- ---- request 10: one cooldown owner ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 1, cooldown_seconds = 15 })
  local e, t = demon(600, 0), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  check("no cooldown before the first perch", loco:cooldown_left(e) == 0)
  e.pos = spot.node
  loco:mount(e, spot, t.pos)
  check("the cooldown starts at the mount", near(loco:cooldown_left(e), 15))
  run(loco, e, 5)
  check("it runs down", near(loco:cooldown_left(e), 10, 0.2))
  loco:leave(e)
  check("leaving restarts it (the engine sets lastHangoutTime on exit too)", near(loco:cooldown_left(e), 15))
  run(loco, e, 1.2)
  check("find_all gives nothing while it runs", loco:mode(e) == "ground" and #loco:find_all("perch", e, t) == 0 and loco:find("perch", e, t) == nil)
  check("ignoreCooldown asks past it", #loco:find_all("perch", e, t, { ignoreCooldown = true }) > 0)
  clock = clock + 13.7
  check("a moment short of 15 it is still cooling", loco:cooldown_left(e) > 0 and #loco:find_all("perch", e, t) == 0)
  clock = clock + 0.4
  check("then the spots come back", loco:cooldown_left(e) == 0 and #loco:find_all("perch", e, t) > 0)
  check("a second demon has its own cooldown", loco:cooldown_left(demon(0, 0)) == 0)
end

-- ---- a demon that dies on its spot ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 0.2 })
  local e, t = demon(600, 0), demon(-200, 0)
  local spot = loco:find("perch", e, t)
  e.pos = spot.node
  loco:mount(e, spot, t.pos)
  run(loco, e, 0.5)
  dead[e] = true
  loco:tick(e, 0.1)
  check("a dead demon's tick releases its spot", loco:holder(spot) == nil and loco.claims[spot.id] == nil)
  dead[e] = nil
  check("and the spot can be taken", loco:reserve(demon(0, 0), spot) == true)
end

-- ---- the whole sequence ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 1, cooldown_seconds = 15 })
  local e, t = demon(600, 0), demon(-200, 0)
  local parms = { minDistanceFromEnemy = 100, maxDistanceFromEnemy = 3000, minVisibility = 0.4, walkSpeed = 300, maxTravelTime = 7 }
  local list = loco:find_all("perch", e, t, parms)
  check("choose: candidates within the parameters", #list > 0)
  local spot = list[1]
  check("reserve: the demon holds the choice", loco:reserve(e, spot) and loco:holder(spot) == e)
  moving[e] = spot.node                                                -- the brain walks with the ordinary move
  for _ = 1, 20 do
    clock = clock + 0.1
    local p = V.sub(spot.node, e.pos)
    e.pos = V.add(e.pos, V.mul(p, 0.3))
    loco:tick(e, 0.1)
    if V.len(p) < 5 then moving[e] = nil end
  end
  check("walk: the library places nothing while the demon walks", #placed == 0 and loco:grounded(e))
  e.pos = V.new(spot.node.x + 5, spot.node.y, spot.node.z)
  check("arrive: at the node, the move finished", loco:at_node(e) == true)
  local before = move_calls
  check("mount: with a facing, no second walk", loco:mount(e, spot, loco:facing_index(e, t.pos)) == true and move_calls == before)
  run(loco, e, 1.1)
  check("hold: on the surface", loco:arrived(e) and loco:support(e).kind == spot.kind)
  local was = loco:facing(e)
  loco:set_facing_index(e, (was + 2) % 8)
  run(loco, e, 0.2)
  check("turn: the held facing changed by two indices", was ~= nil and loco:facing(e) == (was + 2) % 8)
  local grounded_at
  loco:leave(e, function() grounded_at = clock end)
  run(loco, e, 1.2)
  check("leave: grounded by the callback, spot free, off the surface", grounded_at ~= nil and loco:holder(spot) == nil and near(e.pos.z, 0) and loco:support(e).kind == "floor")
  check("cooldown: no new spot for 15 s from the leave", #loco:find_all("perch", e, t, parms) == 0 and near(loco:cooldown_left(e), 15 - 1.2, 0.3))
  clock = clock + 15
  check("then again", #loco:find_all("perch", e, t, parms) > 0)
end

-- ---- the older all-in-one go still works ----
do
  reset()
  local loco = perch_loco({ mount_seconds = 1 })
  local e, t = demon(600, 0), demon(-200, 0)
  local dest = loco:find("perch", e, t)
  loco:go(e, dest)
  check("go: the demon first walks to the floor point under its spot", moving[e] == dest.node and loco:mode(e) == "perch" and loco:grounded(e))
  loco:tick(e, 0.1)
  check("go: nothing is placed while it walks", #placed == 0 and not loco:arrived(e))
  moving[e] = nil; e.pos = dest.base
  loco:face(e, t.pos)
  run(loco, e, 1.2)
  check("go: mounted, the script holds the demon on the surface", loco:arrived(e) and held[e] == true and
        near(V.len(V.sub(e.pos, dest.pos)), 0) and loco:support(e).kind == dest.kind)
  local before = #placed
  loco:tick(e, 0.1)
  check("go: held in place every frame", #placed == before + 1 and near(V.len(V.sub(placed[#placed].pos, dest.pos)), 0))
  loco:leave(e)
  run(loco, e, 1.2)
  check("go: back on the floor, released, its spot free", loco:mode(e) == "ground" and held[e] == false and
        loco.claims[dest.id] == nil and near(e.pos.z, 0) and loco:support(e).kind == "floor")
  local g = demon(0, 600)
  local other = loco:find("perch", g, t, { ignoreCooldown = true })
  loco:reserve(demon(1, 1), other)
  check("go: a spot another demon holds is not walked to", loco:go(g, other) == false and loco:mode(g) == "ground" and moving[g] == nil)
end

-- ---- a new mode is one provider ----
do
  reset()
  L.register("fly", { find = function(self, e, target) return { mode = "fly", pos = target.pos } end,
                      go = function(self, e, dest) e.flying_to = dest.pos end,
                      arrived = function() return true end, leave = function(self, e) e.flying_to = nil end })
  local loco = L.new(adapter(), { modes = { "ground", "fly" } })
  local e, t = demon(0, 0), demon(100, 100)
  loco:go(e, loco:find("fly", e, t))
  check("a registered mode is used like the built-in ones", loco:mode(e) == "fly" and e.flying_to == t.pos and loco:arrived(e))
  check("find_all wraps a provider's single find", #loco:find_all("fly", e, t) == 1)
  loco:leave(e)
  check("leaving it gives the demon back to the ground", loco:mode(e) == "ground" and e.flying_to == nil and loco:grounded(e))
  L.providers.fly = nil
end

print(string.format("%d cases, %d passed, %d failed", cases, cases - failed, failed))
os.exit(failed == 0 and 0 or 1)
