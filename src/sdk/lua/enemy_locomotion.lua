-- enemy_locomotion.lua: how a demon of the Enemy SDK gets from one place to another.
--
-- A brain never moves a demon itself; it asks a locomotion mode. The modes:
--
--   ground   the engine's own navigation (every DOOM 2016 demon): the adapter's move_to.
--   perch    holding on a wall or a ceiling, the way DOOM Eternal's Arachnotron "hangs out".
--   fly      not built in; one more provider (see the end of this header).
--
-- Perch is a sequence the brain drives and the library serves. Eternal's own is: the demon's
-- reposition step scores candidate spots with its positioning parameters and picks one; the demon
-- WALKS to the spot's floor point with the ordinary move system; "using the hangout" means having
-- arrived there (the move finished, the spot still valid, within a short distance); it holds on the
-- surface; and the brain's graph decides when it leaves. DOOM 2016 has no body physics for walls or
-- ceilings (aiMovementMode_t has no wall mode), so the library holds the demon itself: it places
-- the demon every frame with its up along the surface's normal, and the adapter's surface_hold
-- takes the demon off the engine's ground physics while it does. A SnapMap map carries no hangout
-- spots, so the library finds them by tracing (plus any Perch object the map's author placed).
--
-- THE CONTRACT (perch). Distances are the game's units (inches), times are seconds. Nothing here
-- caps the number of spots, demons or searches; the library never ends a perch on its own.
--
--   Choosing (a query: changes nothing)
--     L:find_all("perch", e, target, parms) -> { spot, ... }   every valid candidate, best first
--     L:find("perch", e, target, parms)     -> spot | nil      the first of those
--     L.perch_spots(L, e, target, parms)    -> the same list, without the cooldown
--     The list is empty while the demon's cooldown runs (parms.ignoreCooldown = true skips that),
--     while the demon is not on the ground, and for a brain built without the "perch" mode. A spot:
--       id        a stable name for the place (the same wall or ceiling cell gives the same id)
--       pos       where the demon's origin goes while it holds: a body radius off the surface
--       normal    the surface's normal, pointing out of it
--       kind      "wall" or "ceiling"
--       node      the ground point the demon WALKS to (the engine's navigation ground under the
--                 spot); `base` is the same point under its older name
--       path      the length of the engine's own path to the node (nil without a path query)
--       travel    path / walk speed, seconds (nil when no walk speed is known)
--       visibility the share of the target's sample points the spot sees (0..1; nil without a target)
--       distance  from pos to the target's position (nil without a target)
--       score     the library's own order: distance to the target (distance from the demon without
--                 one), nearest first. The caller's scorer replaces it; the data is what it needs.
--     parms, all optional, honoured when present:
--       minDistanceFromEnemy, maxDistanceFromEnemy   pos to the target
--       minVisibility           visible share required; absent means at least one sample clear,
--                               0 means no line of sight is required
--       visibilityHeights       the target-origin heights sampled (default 16, 48, 80)
--       maxTravelTime           path / walk speed; applied only when a walk speed is known
--       walkSpeed               inches per second (else spec.perch.walk_speed, else A.walk_speed(e))
--       maxDetour               a walk longer than this times the straight line is behind something
--                               (default L.DETOUR = 3)
--       bodyRadius, searchRadius, height   override the spec's body_radius, reach, height
--       ignoreCooldown, stats   stats is a table that receives the rejected count per reason
--     MaxAIAtPoint is not a parameter: a hangout is held by one demon (the engine says its
--     hangouts always are), so a spot another demon holds is never returned.
--
--   Reserving
--     L:reserve(e, spot) -> true | false, reason   the demon holds the spot from the choice on. One
--                         demon per spot; a demon holds one spot (reserving another drops the
--                         first unless it is mounted). Release is automatic when the demon dies (a
--                         holder the adapter's alive() calls dead is treated as gone), when the
--                         brain calls L:forget(e) (removed demon), and at the end of a dismount.
--     L:release(e)        give the spot up: a walk not yet mounted is dropped at once; a mounted
--                         demon begins its dismount (its spot is free when that ends).
--     L:forget(e)         the demon is gone: drop everything without any dismount.
--     L:reservation(e) -> spot | nil      L:holder(spot) -> demon | nil
--
--   The walk and the arrival (the brain's own, with the engine's move system)
--     the brain sends the ordinary move to spot.node. L:at_node(e, radius) -> bool, distance is
--     the engine's "using the hangout" test: the demon is on the ground, holds the reservation, its
--     move has finished and it is within radius (default spec.perch.arrive_radius, 24) of the node.
--     L:spot_valid(e, spot, target, parms) -> true | false, reason: the spot is still free (or the
--     demon's), still has room for the body and a free jump, and still sees the target by
--     parms.minVisibility. The stand-in for "the current cover is valid".
--
--   Mounting and holding
--     L:mount(e, spot, facing) -> true | false, reason   the jump from where the demon stands onto
--                         the surface, with no second walk. The demon must be within
--                         spec.perch.mount_reach (144) of the node. `facing` is a facing index
--                         (number) or a point to face (table); it reserves the spot when the demon
--                         does not hold it. Cooldown starts. Then the library places the demon every
--                         frame until it leaves.
--     L:go(e, spot)       the older all-in-one: reserve, walk to the node, mount on arrival.
--     L:tick(e, dt)       each frame; the mount, the hold and the dismount run in it.
--     L:arrived(e) -> bool    the demon is held on the surface.
--     L:seconds_at_spot(e) -> seconds held | nil     a query only: nothing ends because of it.
--
--   Facing (the wall idle's discrete facings)
--     L:facing_count()                         how many (spec.perch.facings, 8)
--     L:facing_index(e, point, count)          the index of the facing toward a point, 0..count-1
--     L:facing(e)                              the index the demon holds now (nil when none is set)
--     L:set_facing_index(e, i) -> bool         hold facing i; L:face(e, point) holds a point exactly
--     L.facing_index_of(spot, point, count), L.facing_direction(spot, i, count)   the same maths
--                                              for a spot not yet held
--     Index 0 faces up the surface on a wall and along +x on a ceiling; indices grow
--     counter-clockwise looking at the surface from outside.
--
--   Leaving and the ground
--     L:leave(e, on_grounded)   begin the dismount; on_grounded(e) is called the frame the demon
--                         stands on the ground again (at once when it never left it). Cooldown
--                         restarts.
--     L:grounded(e) -> bool     true when the library is not placing the demon (not mounting,
--                         holding or dismounting): the brain's own moves are the engine's again.
--     L:traversing(e) -> bool   true while the jump up or the jump down is under way (the stand-in
--                         for "a traversal clip is playing").
--     L:mode(e) / L:support(e)  "ground" or "perch"; "floor", "wall" or "ceiling" with its normal.
--
--   Cooldown (the library's alone)
--     L:cooldown_left(e) -> seconds. It starts at mount and again at leave (the engine sets its
--     lastHangoutTime on entering and leaving the state), lasts spec.perch.cooldown_seconds (15),
--     and find_all honours it. The brain keeps no cooldown clock of its own.
--
-- WHAT IS MEASURED AND WHAT IS ASSUMED. Measured, each call: a wall or ceiling by a trace from the
-- demon's surroundings (rays round it at head height, and up from each ray's mid point); room for
-- the body (a demon-blocking trace along the normal), a free jump (a demon-blocking trace from the
-- node to the spot, so a Blocking Box with Block Demons counts), the walk (the engine's path query,
-- which knows the map's blocking volumes and the navigation Snapmap+ bakes for them), the target's
-- visibility (traces from the spot to sample points above the target's origin) and its distance.
-- Assumed: the body radius (96) and the head height the rays start at (220), the sample heights, a
-- walk speed when the adapter has none, that a detour above L.DETOUR is a way blocked rather than
-- long, that the jump is a straight line with an arc (mount_seconds, 1), and that the wall pose is
-- the surface's normal. Not measured: the engine's silhouette visibility (the share of sample
-- points stands in), its dead-end test, and whether a held pose looks right: only a live game says.
--
-- Adapter calls used: pos, now, trace, nav_ground, entities_near, move_to, move_done, stop, place,
-- surface_hold; optional: trace_demon, path_to(e, point), walk_speed(e), alive(e), def, forward.
--
-- A new mode (flight, swimming) is one more provider:
--   L.register("fly", { find =, go =, tick =, arrived =, leave =, find_all = })
-- find_all is optional (find alone is wrapped into a list of one); comes_down = true says that
-- leave takes time and the demon is back on the ground when the provider sets it so.

local L = {}
L.__index = L

L.providers = {}
function L.register(mode, provider) L.providers[mode] = provider end

-- ---------------------------------------------------------------------------------- vectors
local function v(x, y, z) return { x = x, y = y, z = z } end
local function add(a, b) return v(a.x + b.x, a.y + b.y, a.z + b.z) end
local function sub(a, b) return v(a.x - b.x, a.y - b.y, a.z - b.z) end
local function mul(a, k) return v(a.x * k, a.y * k, a.z * k) end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function cross(a, b) return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x) end
local function len(a) return math.sqrt(dot(a, a)) end
local function unit(a) local n = len(a); if n < 1e-6 then return nil end return mul(a, 1 / n) end
L.vec = { new = v, add = add, sub = sub, mul = mul, dot = dot, cross = cross, len = len, unit = unit }

--- What a surface is, from its normal: floor (up), ceiling (down) or wall.
function L.kind_of(normal)
  if normal.z >= 0.7 then return "floor" end
  if normal.z <= -0.7 then return "ceiling" end
  return "wall"
end

--- The angles (pitch, yaw, roll in degrees, the engine's order) of a demon standing on a surface
-- with `normal`, facing along `toward` as far as the surface allows.
function L.surface_angles(normal, toward)
  local up = unit(normal)
  local f = toward and sub(toward, mul(up, dot(toward, up))) or nil
  f = f and unit(f)
  if not f then                      -- facing straight into or out of the surface: any tangent
    f = unit(cross(up, v(0, 0, 1))) or v(1, 0, 0)
  end
  local left = cross(up, f)
  local pitch = -math.deg(math.asin(math.max(-1, math.min(1, f.z))))
  local yaw = math.deg(math.atan(f.y, f.x))
  local roll = math.deg(math.atan(left.z, up.z))
  return { pitch = pitch, yaw = yaw, roll = roll }
end

-- ---------------------------------------------------------------------------------- the object
--- A locomotion object for one brain. A: the adapter. spec: { modes = { "ground", "perch" },
-- perch = { cooldown_seconds =, body_radius =, reach =, height =, rays =, mount_seconds =,
-- mount_reach =, arrive_radius =, facings =, walk_speed =, spot_def = "info/perch" } }.
function L.new(A, spec)
  spec = spec or {}
  local self = setmetatable({ A = A, spec = spec, modes = {}, state = setmetatable({}, { __mode = "k" }),
                              claims = {} }, L)
  for _, mode in ipairs(spec.modes or { "ground" }) do
    local p = L.providers[mode]
    if not p then error("enemy_locomotion: no locomotion mode named " .. tostring(mode)) end
    self.modes[#self.modes + 1] = mode
  end
  return self
end

function L:st(e)
  local s = self.state[e]
  if not s then s = { mode = "ground", support = { kind = "floor", normal = v(0, 0, 1) } }; self.state[e] = s end
  return s
end

function L:has(mode)
  for _, m in ipairs(self.modes) do if m == mode then return true end end
  return false
end

function L:mode(e) return self:st(e).mode end
function L:support(e) return self:st(e).support end

function L:find_all(mode, e, target, parms)
  if not self:has(mode) then return {} end
  local p = L.providers[mode]
  parms = parms or {}
  if p.find_all then return p.find_all(self, e, target, parms) or {} end
  local one = p.find(self, e, target, parms)
  return one and { one } or {}
end
function L:find(mode, e, target, parms) return self:find_all(mode, e, target, parms)[1] end

function L:go(e, dest)
  local s = self:st(e)
  if s.mode ~= dest.mode and s.mode ~= "ground" then L.providers[s.mode].leave(self, e) end
  s.mode = dest.mode
  return L.providers[dest.mode].go(self, e, dest)
end
function L:tick(e, dt)
  local A = self.A
  if A.alive and self.state[e] and self.state[e].perch and not A.alive(e) then self:forget(e); return end
  local p = L.providers[self:st(e).mode]
  if p.tick then p.tick(self, e, dt) end
end
function L:arrived(e) return L.providers[self:st(e).mode].arrived(self, e) end

--- Begin giving the demon back to the ground. on_grounded(e) is called the frame it stands there.
function L:leave(e, on_grounded)
  local s = self:st(e)
  if s.mode == "ground" then
    if on_grounded then on_grounded(e) end
    return
  end
  local p = L.providers[s.mode]
  p.leave(self, e, on_grounded)
  if not p.comes_down then
    s.mode = "ground"
    if on_grounded then on_grounded(e) end
  end
end

--- true while the jump onto a surface or down from it is under way.
function L:traversing(e)
  local p = self:st(e).perch
  return p ~= nil and (p.phase == "mount" or p.phase == "dismount")
end

--- true when the library is not placing the demon: the engine's moves are the demon's again.
function L:grounded(e)
  local s = self:st(e)
  local p = s.perch
  if not p then return true end
  return p.phase == "approach"
end

-- ---------------------------------------------------------------------------------- ground
L.register("ground", {
  find = function(self, e, target, parms) return { mode = "ground", pos = parms.pos or self.A.pos(target) } end,
  go = function(self, e, dest) self.A.move_to(e, dest.pos, dest.flags) end,
  arrived = function(self, e) return self.A.move_done(e) end,
  leave = function() end,
})

-- ---------------------------------------------------------------------------------- perch
local PERCH = { cooldown_seconds = 15, body_radius = 96, reach = 1024, height = 220, rays = 16,
                mount_seconds = 1.0, mount_reach = 144, arrive_radius = 24, facings = 8,
                spot_def = "info/perch" }
local function perch_parm(self, key) return (self.spec.perch or {})[key] or PERCH[key] end
L.DETOUR = 3     -- a spot whose walk is more than this many times the straight line is behind something
local SIGHT_HEIGHTS = { 16, 48, 80 }

-- A trace's answer as "what was hit, or nil": the adapter answers nil or { hit = false } for a
-- clear line. Demon traces (stopped by Blocking Boxes with Block Demons too) when the adapter
-- has them.
local function hit(tr) if tr and tr.hit ~= false and tr.normal then return tr end return nil end
local function solid(A, from, to, opts)
  if A.trace_demon then return hit(A.trace_demon(from, to, opts)) end
  return hit(A.trace(from, to, opts))
end

-- A holder that no longer lives holds nothing.
local function live(A, who)
  if who == nil then return false end
  if not A.alive then return true end
  local ok, alive = pcall(A.alive, who)
  return not ok or alive ~= false
end
local function holder_of(self, id)
  local h = self.claims[id]
  if h ~= nil and not live(self.A, h) then self.claims[id] = nil; return nil end
  return h
end
function L:holder(spot) return spot and holder_of(self, spot.id) or nil end
function L:reservation(e) return self:st(e).reserved end

local function walk_speed(self, e, parms)
  local s = parms.walkSpeed or (self.spec.perch or {}).walk_speed
  if not s and self.A.walk_speed then s = self.A.walk_speed(e) end
  if s and s > 0 then return s end
  return nil
end

-- the share of the target's sample points that a spot sees; nil without a target
local function visibility(self, from, target, parms)
  if not target then return nil end
  local A = self.A
  local base = A.pos(target)
  local heights = parms.visibilityHeights or SIGHT_HEIGHTS
  local seen = 0
  for _, h in ipairs(heights) do
    if not hit(A.trace(from, add(base, v(0, 0, h)), { ignore = target })) then seen = seen + 1 end
  end
  return seen / #heights
end
local function sight_ok(vis, parms)
  if vis == nil then return true end
  local need = parms.minVisibility
  if need == nil then return vis > 0 end
  if need <= 0 then return true end
  return vis + 1e-9 >= need
end
-- room for the body in front of the surface
local function room_ok(self, hit_pos, normal, radius)
  return not solid(self.A, add(hit_pos, mul(normal, 1)), add(hit_pos, mul(normal, radius * 2)))
end
-- the jump from the floor point to the spot is free of anything that stops a demon
local function jump_ok(self, node, pos)
  return not solid(self.A, add(node, v(0, 0, 32)), pos)
end
local function surface_point(spot, radius) return sub(spot.pos, mul(spot.normal, radius)) end

local function reject(parms, why)
  if parms.stats then parms.stats[why] = (parms.stats[why] or 0) + 1 end
  return nil
end

-- One candidate: the surface point and normal -> a spot, or nil with the reason counted.
local function make_spot(self, e, target, parms, hit_pos, normal, id)
  local A = self.A
  local radius = parms.bodyRadius or perch_parm(self, "body_radius")
  local kind = L.kind_of(normal)
  if kind == "floor" then return reject(parms, "floor") end
  local pos = add(hit_pos, mul(normal, radius))
  if holder_of(self, id) ~= nil and holder_of(self, id) ~= e then return reject(parms, "held") end
  if not room_ok(self, hit_pos, normal, radius) then return reject(parms, "no room") end
  local distance = target and len(sub(A.pos(target), pos)) or nil
  if distance then
    if parms.minDistanceFromEnemy and distance < parms.minDistanceFromEnemy then return reject(parms, "too near") end
    if parms.maxDistanceFromEnemy and distance > parms.maxDistanceFromEnemy then return reject(parms, "too far") end
  end
  local vis = visibility(self, pos, target, parms)
  if not sight_ok(vis, parms) then return reject(parms, "not seen") end
  local node = A.nav_ground(kind == "ceiling" and pos or add(pos, mul(normal, radius)))
  if not node then return reject(parms, "no ground") end
  local path, travel
  -- the demon has to be able to walk to the floor point: the engine's own path, which knows the
  -- map's blocking volumes and the navigation baked for them
  if A.path_to then
    path = A.path_to(e, node)
    if not path then return reject(parms, "unreachable") end
    local straight = len(sub(node, A.pos(e)))
    if path > math.max(straight, radius) * (parms.maxDetour or L.DETOUR) then return reject(parms, "detour") end
    local speed = walk_speed(self, e, parms)
    if speed then
      travel = path / speed
      if parms.maxTravelTime and travel > parms.maxTravelTime then return reject(parms, "too far to walk") end
    end
  end
  if not jump_ok(self, node, pos) then return reject(parms, "jump blocked") end
  return { mode = "perch", id = id, pos = pos, normal = normal, kind = kind, node = node, base = node,
           path = path, travel = travel, visibility = vis, distance = distance,
           score = distance or len(sub(pos, A.pos(e))) }
end

--- Every spot the demon could perch on now, best first (see the contract). The cooldown is not
-- looked at here: find_all does that.
function L.perch_spots(self, e, target, parms)
  parms = parms or {}
  local A, out, seen = self.A, {}, {}
  local from = A.pos(e)
  local reach = parms.searchRadius or perch_parm(self, "reach")
  local function consider(hit_pos, normal, id)
    if seen[id] then return end
    seen[id] = true
    local spot = make_spot(self, e, target, parms, hit_pos, normal, id)
    if spot then out[#out + 1] = spot end
  end
  -- the map author's own Perch objects
  local def = perch_parm(self, "spot_def")
  for _, x in ipairs(A.entities_near(from, reach) or {}) do
    local name = A.def and A.def(x) or nil
    if name and name:sub(-#def) == def then
      local p, fwd = A.pos(x), A.forward(x)
      if p and fwd then consider(p, fwd, "placed:" .. tostring(x)) end
    end
  end
  -- found by tracing: walls around the demon at perch height, and the ceiling above each of them
  local h = parms.height or perch_parm(self, "height")
  local head = add(from, v(0, 0, h))
  local rays = perch_parm(self, "rays")
  for i = 0, rays - 1 do
    local a = i * 2 * math.pi / rays
    local dir = v(math.cos(a), math.sin(a), 0)
    local wall = solid(A, head, add(head, mul(dir, reach)))
    if wall then consider(wall.pos, wall.normal, string.format("wall:%d:%d:%d", wall.pos.x // 64, wall.pos.y // 64, wall.pos.z // 64)) end
    local mid = add(from, mul(dir, reach * 0.5))
    local top = solid(A, add(mid, v(0, 0, 32)), add(mid, v(0, 0, reach)))
    if top then consider(top.pos, top.normal, string.format("ceil:%d:%d", top.pos.x // 64, top.pos.y // 64)) end
  end
  table.sort(out, function(a, b) if a.score ~= b.score then return a.score < b.score end return a.id < b.id end)
  return out
end

-- ---- reserving ------------------------------------------------------------------------
function L:reserve(e, spot)
  local s = self:st(e)
  local h = holder_of(self, spot.id)
  if h ~= nil and h ~= e then return false, "held by another demon" end
  local p = s.perch
  if s.reserved and s.reserved.id ~= spot.id then
    if p and p.phase ~= "approach" then return false, "mounted on another spot" end
    self.claims[s.reserved.id] = nil
    s.perch = nil
    if s.mode == "perch" then s.mode = "ground" end
  end
  s.reserved = spot
  self.claims[spot.id] = e
  return true
end

function L:release(e)
  local s = self:st(e)
  local p = s.perch
  if p and (p.phase == "mount" or p.phase == "hold") then self:leave(e); return end
  if p and p.phase == "dismount" then return end
  if s.reserved then self.claims[s.reserved.id] = nil end
  s.reserved, s.perch = nil, nil
  if s.mode == "perch" then s.mode = "ground" end
end

function L:forget(e)
  local s = self.state[e]
  if not s then return end
  if s.reserved then self.claims[s.reserved.id] = nil end
  if s.perch and s.perch.phase ~= "approach" and self.A.surface_hold then pcall(self.A.surface_hold, e, false) end
  self.state[e] = nil
end

function L:cooldown_left(e)
  local t = self:st(e).cooldown_until
  if not t then return 0 end
  return math.max(0, t - self.A.now())
end
local function start_cooldown(self, e) self:st(e).cooldown_until = self.A.now() + perch_parm(self, "cooldown_seconds") end

-- ---- the walk and the arrival ---------------------------------------------------------
function L:at_node(e, radius)
  local s, A = self:st(e), self.A
  local spot = s.reserved
  if not spot or self.claims[spot.id] ~= e then return false end
  local p = s.perch
  if p and p.phase ~= "approach" then return false end
  local d = len(sub(A.pos(e), spot.node))
  if not A.move_done(e) then return false, d end
  return d <= (radius or perch_parm(self, "arrive_radius")), d
end

function L:spot_valid(e, spot, target, parms)
  parms = parms or {}
  spot = spot or self:st(e).reserved
  if not spot then return false, "no spot" end
  local h = holder_of(self, spot.id)
  if h ~= nil and h ~= e then return false, "held by another demon" end
  local radius = parms.bodyRadius or perch_parm(self, "body_radius")
  local p = self:st(e).perch
  local mounted = p ~= nil and (p.phase == "hold" or p.phase == "mount")
  if not room_ok(self, surface_point(spot, radius), spot.normal, radius) then return false, "no room" end
  if not mounted and not jump_ok(self, spot.node, spot.pos) then return false, "jump blocked" end
  if target and not sight_ok(visibility(self, spot.pos, target, parms), parms) then return false, "not seen" end
  return true
end

-- ---- facings --------------------------------------------------------------------------
local function frame(spot)
  local n = unit(spot.normal)
  local up = v(0, 0, 1)
  local ref = unit(sub(up, mul(n, dot(up, n)))) or v(1, 0, 0)
  return n, ref
end

function L.facing_index_of(spot, point, count)
  if not spot or not point then return nil end
  local n, ref = frame(spot)
  local w = sub(point, spot.pos)
  w = unit(sub(w, mul(n, dot(w, n))))
  if not w then return nil end
  local ang = math.deg(math.atan(dot(cross(ref, w), n), dot(ref, w)))
  return math.floor(ang / (360 / count) + 0.5) % count
end

function L.facing_direction(spot, index, count)
  local n, ref = frame(spot)
  local a = math.rad(index * 360 / count)
  return add(mul(ref, math.cos(a)), mul(cross(n, ref), math.sin(a)))
end

function L:facing_count() return perch_parm(self, "facings") end

local function facing_spot(self, e)
  local s = self:st(e)
  return (s.perch and s.perch.dest) or s.reserved
end

function L:facing_index(e, point, count)
  return L.facing_index_of(facing_spot(self, e), point, count or self:facing_count())
end

function L:facing(e)
  local p = self:st(e).perch
  if not p or not p.face then return nil end
  local spot = p.dest
  return L.facing_index_of(spot, add(spot.pos, p.face), self:facing_count())
end

function L:set_facing_index(e, index)
  local p = self:st(e).perch
  if not p then return false end
  p.face = L.facing_direction(p.dest, index % self:facing_count(), self:facing_count())
  return true
end

--- Face a point while perched (the direction is kept along the surface).
function L:face(e, point)
  local p = self:st(e).perch
  if p and point then p.face = sub(point, p.dest.pos) end
end

-- ---- mounting, holding, dismounting ---------------------------------------------------
local function begin_mount(self, e, s, spot, facing)
  local A = self.A
  local p = { dest = spot, phase = "mount", t = 0, from = A.pos(e) }
  if type(facing) == "number" then p.face = L.facing_direction(spot, facing % self:facing_count(), self:facing_count())
  elseif type(facing) == "table" then p.face = sub(facing, spot.pos) end
  s.perch, s.mode = p, "perch"
  A.stop(e)
  A.surface_hold(e, true)
  start_cooldown(self, e)
end

function L:mount(e, spot, facing)
  local s, A = self:st(e), self.A
  if not self:has("perch") then return false, "no perch mode" end
  if s.perch and s.perch.phase ~= "approach" then return false, "already on a surface" end
  if len(sub(A.pos(e), spot.node)) > perch_parm(self, "mount_reach") then return false, "not at the node" end
  local ok, why = self:reserve(e, spot)
  if not ok then return false, why end
  begin_mount(self, e, s, spot, facing)
  return true
end

function L:seconds_at_spot(e)
  local p = self:st(e).perch
  if not p or p.phase ~= "hold" then return nil end
  return self.A.now() - p.since
end

L.register("perch", {
  comes_down = true,                 -- leaving plays the dismount; the demon is on the ground when it ends
  find_all = function(self, e, target, parms)
    if self:st(e).mode ~= "ground" then return {} end
    if not parms.ignoreCooldown and self:cooldown_left(e) > 0 then return {} end
    return L.perch_spots(self, e, target, parms)
  end,
  go = function(self, e, dest)
    local s = self:st(e)
    local ok = self:reserve(e, dest)
    if not ok then s.mode = "ground"; return false end
    s.perch = { dest = dest, phase = "approach" }
    self.A.move_to(e, dest.node)
    return true
  end,
  tick = function(self, e, dt)
    local A, s = self.A, self:st(e)
    local p = s.perch
    if not p then return end
    if p.phase == "approach" then
      if not A.move_done(e) then return end
      -- the engine's move ended: if it left the demon short of the floor point (a blocked way), the
      -- perch is given up rather than jumped to from the wrong place
      if len(sub(A.pos(e), p.dest.node)) > perch_parm(self, "mount_reach") then
        self.claims[p.dest.id] = nil
        s.reserved, s.perch, s.mode = nil, nil, "ground"
        start_cooldown(self, e)
        return
      end
      begin_mount(self, e, s, p.dest, nil)
      p = s.perch
    end
    if p.phase == "mount" or p.phase == "dismount" then
      p.t = p.t + dt
      local T = perch_parm(self, "mount_seconds")
      local k = math.min(1, p.t / T)
      local a, b = p.from, p.phase == "mount" and p.dest.pos or p.dest.node
      local pos = add(a, mul(sub(b, a), k))
      pos.z = pos.z + math.sin(k * math.pi) * 48                 -- the jump's arc
      local n = p.phase == "mount" and p.dest.normal or v(0, 0, 1)
      A.place(e, pos, L.surface_angles(k < 0.5 and v(0, 0, 1) or n, p.face))
      if k >= 1 then
        if p.phase == "mount" then
          p.phase, p.since = "hold", A.now()
          s.support = { kind = p.dest.kind, normal = p.dest.normal }
        else
          local done = p.on_grounded
          A.surface_hold(e, false)
          self.claims[p.dest.id] = nil
          s.support = { kind = "floor", normal = v(0, 0, 1) }
          s.reserved, s.perch, s.mode = nil, nil, "ground"
          if done then done(e) end
        end
      end
    elseif p.phase == "hold" then
      A.place(e, p.dest.pos, L.surface_angles(p.dest.normal, p.face))      -- the script holds it there
    end
  end,
  arrived = function(self, e)
    local p = self:st(e).perch
    return p ~= nil and p.phase == "hold"
  end,
  leave = function(self, e, on_grounded)
    local s = self:st(e)
    local p = s.perch
    if not p then
      if s.reserved then self.claims[s.reserved.id] = nil; s.reserved = nil end
      s.mode = "ground"
      if on_grounded then on_grounded(e) end
      return
    end
    if p.phase == "approach" then
      self.claims[p.dest.id] = nil
      s.reserved, s.perch, s.mode = nil, nil, "ground"
      if on_grounded then on_grounded(e) end
    elseif p.phase == "dismount" then
      if on_grounded then p.on_grounded = on_grounded end
    else
      p.phase, p.t, p.from, p.on_grounded = "dismount", 0, self.A.pos(e), on_grounded
      start_cooldown(self, e)
    end
  end,
})

return L
