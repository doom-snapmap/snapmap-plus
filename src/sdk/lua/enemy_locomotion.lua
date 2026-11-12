-- enemy_locomotion.lua: how a demon of the Enemy SDK gets from one place to another.
--
-- A brain never moves a demon itself; it asks a locomotion mode. A mode is a provider:
--
--   find(L, e, target, parms) -> destination or nil    where this mode could take the demon
--   go(L, e, dest)                                      start going there
--   tick(L, e, dt)                                      each frame while the demon is in this mode
--   arrived(L, e) -> bool                               the demon is where go() sent it
--   leave(L, e)                                         give the demon back to the ground (a mode
--                                                       with comes_down = true does it over time)
--
-- and L.support(e) says what the demon stands on now: "floor", "wall" or "ceiling", with the
-- surface's normal. Modes:
--
--   ground   the engine's own navigation (every DOOM 2016 demon): the adapter's move_to.
--   perch    spots on walls and ceilings. This is how DOOM Eternal puts an Arachnotron on a wall:
--            not free walking, but a "hangout": the demon goes to a spot, mounts it with a
--            traversal clip, holds there for a few seconds turning in steps and firing, and
--            comes down again (arachnotron_hangout FSM, aipositioningparms default_hangout: one
--            demon per spot, 6 s at a spot, 15 s before the next). Eternal's levels carry those
--            spots; a SnapMap map has none, so a spot is either a Perch object the map's author
--            placed (the mod's own Toybox object, facing out of the surface) or one this file
--            finds by tracing from the demon's surroundings: a wall or ceiling with room for the
--            body and a clear line to the enemy. A spot counts only when the engine's own
--            navigation gives the demon a walk to the floor under it (so the map's blocking volumes
--            and the navigation Snapmap+ bakes for them decide) and nothing that stops a demon,
--            a Blocking Box with Block Demons included, is in the body's place or in the jump.
--            While perched the script holds the demon: the engine's physics knows no walls for
--            demons, so the provider places it every frame, with its up along the surface's
--            normal. The adapter supplies place(e, pos, angles) and surface_hold(e, on): the demon's
--            default movement mode goes to AIMOVEMODE_AIR, so the engine itself keeps its physics
--            flying (no gravity, no step, no drop to the ground), and its facing target is written
--            with the placement, all by name through the game's type tables (read from the engine's
--            code; work\M3\ghidra-wall-hold.md). Unproven in the game.
--
-- A new mode (flight, swimming) is one more provider: L.register("fly", { find =, go =, ... }).

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
-- perch = { seconds_at_spot =, cooldown_seconds =, body_radius =, reach =, mount_seconds =,
-- spot_def = "info/perch" } }.
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
function L:find(mode, e, target, parms) return self:has(mode) and L.providers[mode].find(self, e, target, parms or {}) or nil end
function L:go(e, dest)
  local s = self:st(e)
  if s.mode ~= dest.mode and s.mode ~= "ground" then L.providers[s.mode].leave(self, e) end
  s.mode = dest.mode
  return L.providers[dest.mode].go(self, e, dest)
end
function L:tick(e, dt) local p = L.providers[self:st(e).mode]; if p.tick then p.tick(self, e, dt) end end
function L:arrived(e) return L.providers[self:st(e).mode].arrived(self, e) end
function L:leave(e)
  local s = self:st(e)
  if s.mode == "ground" then return end
  local p = L.providers[s.mode]
  p.leave(self, e)
  if not p.comes_down then s.mode = "ground" end     -- a mode that comes down by itself says when it is done
end

-- ---------------------------------------------------------------------------------- ground
L.register("ground", {
  find = function(self, e, target, parms) return { mode = "ground", pos = parms.pos or self.A.pos(target) } end,
  go = function(self, e, dest) self.A.move_to(e, dest.pos, dest.flags) end,
  arrived = function(self, e) return self.A.move_done(e) end,
  leave = function() end,
})

-- ---------------------------------------------------------------------------------- perch
local PERCH = { seconds_at_spot = 6, cooldown_seconds = 15, body_radius = 96, reach = 1024, height = 220,
                mount_seconds = 1.0, spot_def = "info/perch" }
local function perch_parm(self, key) return (self.spec.perch or {})[key] or PERCH[key] end

-- A trace's answer as "what was hit, or nil": the adapter answers nil or { hit = false } for a
-- clear line. Demon traces (stopped by Blocking Boxes with Block Demons too) when the adapter
-- has them.
local function hit(tr) if tr and tr.hit ~= false and tr.normal then return tr end return nil end
local function solid(A, from, to, opts)
  if A.trace_demon then return hit(A.trace_demon(from, to, opts)) end
  return hit(A.trace(from, to, opts))
end
L.DETOUR = 3     -- a spot whose walk is more than this many times the straight line is behind something

local RING = {}
for i = 0, 7 do local a = i * math.pi / 4; RING[#RING + 1] = v(math.cos(a), math.sin(a), 0) end

--- Every spot the demon could perch on now, best first: { pos, normal, kind, base, id }.
-- pos is where the demon's origin goes (a body radius off the surface), base the floor point it
-- mounts from. A spot another demon holds, one without a clear line to the target and one the
-- body does not fit at are left out.
function L.perch_spots(self, e, target)
  local A, out = self.A, {}
  local from, radius, reach = A.pos(e), perch_parm(self, "body_radius"), perch_parm(self, "reach")
  local eye = target and A.pos(target) or nil
  local function consider(hit_pos, normal, id)
    local kind = L.kind_of(normal)
    if kind == "floor" then return end
    local pos = add(hit_pos, mul(normal, radius))
    if self.claims[id] and self.claims[id] ~= e then return end
    if solid(A, add(hit_pos, mul(normal, 1)), add(hit_pos, mul(normal, radius * 2))) then return end   -- no room for the body
    if eye and hit(A.trace(pos, eye, { ignore = target })) then return end            -- the enemy is not in view
    local base = A.nav_ground(kind == "ceiling" and pos or add(pos, mul(normal, radius)))
    if not base then return end
    -- the demon has to be able to walk to the floor point: the engine's own path, which knows the
    -- map's blocking volumes and the navigation baked for them
    if A.path_to then
      local walk = A.path_to(e, base)
      local straight = len(sub(base, from))
      if not walk or walk > math.max(straight, radius) * L.DETOUR then return end
    end
    -- and the jump from the floor point to the spot has to be free of anything that stops a demon
    if solid(A, add(base, v(0, 0, 32)), pos) then return end
    out[#out + 1] = { mode = "perch", pos = pos, normal = normal, kind = kind, base = base, id = id,
                      score = eye and len(sub(eye, pos)) or len(sub(pos, from)) }
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
  local h = perch_parm(self, "height")
  local head = add(from, v(0, 0, h))
  for i, dir in ipairs(RING) do
    local wall = solid(A, head, add(head, mul(dir, reach)))
    if wall then consider(wall.pos, wall.normal, string.format("wall:%d:%d:%d", wall.pos.x // 64, wall.pos.y // 64, wall.pos.z // 64)) end
    local mid = add(from, mul(dir, reach * 0.5))
    local top = solid(A, add(mid, v(0, 0, 32)), add(mid, v(0, 0, reach)))
    if top then consider(top.pos, top.normal, string.format("ceil:%d:%d", top.pos.x // 64, top.pos.y // 64)) end
  end
  table.sort(out, function(a, b) return a.score < b.score end)
  return out
end

L.register("perch", {
  comes_down = true,                 -- leaving plays the dismount; the demon is on the ground when it ends
  find = function(self, e, target, parms)
    local s = self:st(e)
    if s.perch_ready_at and self.A.now() < s.perch_ready_at then return nil end     -- cooling down
    return L.perch_spots(self, e, target)[1]
  end,
  go = function(self, e, dest)
    local s = self:st(e)
    self.claims[dest.id] = e
    s.perch = { dest = dest, phase = "approach", target = dest.target }
    self.A.move_to(e, dest.base)
  end,
  tick = function(self, e, dt)
    local A, s = self.A, self:st(e)
    local p = s.perch
    if not p then return end
    if p.phase == "approach" then
      if not A.move_done(e) then return end
      -- the engine's move ended: if it left the demon short of the floor point (a blocked way), the
      -- perch is given up rather than jumped to from the wrong place
      if len(sub(A.pos(e), p.dest.base)) > perch_parm(self, "body_radius") * 1.5 then
        self.claims[p.dest.id] = nil
        s.perch, s.mode = nil, "ground"
        s.perch_ready_at = A.now() + perch_parm(self, "cooldown_seconds")
        return
      end
      p.phase, p.t, p.from = "mount", 0, A.pos(e)
      A.stop(e)
      A.surface_hold(e, true)
    end
    if p.phase == "mount" or p.phase == "dismount" then
      p.t = p.t + dt
      local T = perch_parm(self, "mount_seconds")
      local k = math.min(1, p.t / T)
      local a, b = p.from, p.phase == "mount" and p.dest.pos or p.dest.base
      local pos = add(a, mul(sub(b, a), k))
      pos.z = pos.z + math.sin(k * math.pi) * 48                 -- the jump's arc
      local n = p.phase == "mount" and p.dest.normal or v(0, 0, 1)
      A.place(e, pos, L.surface_angles(k < 0.5 and v(0, 0, 1) or n, p.face))
      if k >= 1 then
        if p.phase == "mount" then
          p.phase, p.since = "hold", A.now()
          s.support = { kind = p.dest.kind, normal = p.dest.normal }
        else
          A.surface_hold(e, false)
          self.claims[p.dest.id] = nil
          s.support = { kind = "floor", normal = v(0, 0, 1) }
          s.perch_ready_at = A.now() + perch_parm(self, "cooldown_seconds")
          s.perch, s.mode = nil, "ground"
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
  leave = function(self, e)
    local p = self:st(e).perch
    if not p then return end
    if p.phase == "approach" then
      self.claims[p.dest.id] = nil
      self:st(e).perch, self:st(e).mode = nil, "ground"
    elseif p.phase ~= "dismount" then
      p.phase, p.t, p.from = "dismount", 0, self.A.pos(e)
    end
  end,
})

--- Face a point while perched (the direction is kept along the surface).
function L:face(e, point)
  local p = self:st(e).perch
  if p and point then p.face = sub(point, p.dest.pos) end
end

--- true once the demon has been at its spot as long as its package allows.
function L:perch_expired(e)
  local p = self:st(e).perch
  return p ~= nil and p.phase == "hold" and self.A.now() - p.since >= perch_parm(self, "seconds_at_spot")
end

--- Eternal's HANG_SelectTurnIndex: which of `steps` turn clips (1 = the smallest turn left ...)
-- brings a perched demon to face `point`; nil when it already does within half a step.
function L:turn_index(e, point, steps, step_degrees)
  local p = self:st(e).perch
  if not p then return nil end
  local n = p.dest.normal
  local want = sub(point, p.dest.pos); want = unit(sub(want, mul(n, dot(want, n))))
  local have = p.face and unit(sub(p.face, mul(n, dot(p.face, n)))) or nil
  if not want or not have then return nil end
  local angle = math.deg(math.atan(dot(cross(have, want), n), dot(have, want)))    -- signed, about the normal
  step_degrees = step_degrees or 45
  if math.abs(angle) < step_degrees / 2 then return nil end
  local index = math.min(steps or 4, math.floor(math.abs(angle) / step_degrees + 0.5))
  return index, angle > 0 and "left" or "right", angle
end

return L
