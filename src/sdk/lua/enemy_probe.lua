-- enemy_probe.lua: measure what a demon's brain actually does, so a port is checked with numbers.
--
--   local probe = require("enemy_probe")
--   sdk.bind("ai/demon/tyrant", probe.wrap(require("tyrant_brain").new(), { tags = { "rocket", "fx_blade" } }))
--
-- Every line starts with "probe " and goes to the mod's script log (sh_scripts log). Positions are
-- given in the demon's own frame (forward, left, up from its origin, in game units), so a line reads
-- the same wherever the demon stands and whichever way it faces:
--
--   probe pose  t yaw enemy_bearing(deg off the demon's forward) enemy_dist anim fire(the engine's fire state)
--   probe tag   name fwd left up            (each tag in opts.tags, every opts.every seconds)
--   probe shot  decl fwd left up | nearest tag and distance | aim error (deg between the launch
--               direction and the enemy) | enemy_bearing at the launch
--   probe frame command args                (each animation-layer event the brain hears)
--   probe fire  mode anim_mode pulled ready focus muzzle radius precision require
--                                           (the engine's fire control at each shot, read from the
--                                           game: the fire mode the script events set and the one
--                                           the animation events set, whether the trigger is down,
--                                           whether the weapon's wait is over, whether the aim focus
--                                           is valid, the engine's own muzzle test, the muzzle
--                                           sphere's radius and precision, and whether the fire gate
--                                           asks the muzzle test at all)
--
-- A tag the model does not have is reported once as "probe tag <name> MISSING".
-- A shot with a fire state the game cannot be asked for is reported as "probe fire UNAVAILABLE".

local weapon = require("enemy_weapon")

local P = {}

local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function len(a) return math.sqrt(dot(a, a)) end
local function deg(c) return math.deg(math.acos(math.max(-1, math.min(1, c)))) end

local function axes(e)
  local a = e:axis()
  return a.forward, a.left, a.up
end

-- world position -> fwd, left, up in e's frame
local function rel(e, p)
  local f, l, u = axes(e)
  local d = sub(p, e:pos())
  return dot(d, f), dot(d, l), dot(d, u)
end

-- degrees between e's forward (flat) and the direction to p; signed, + to the left
local function bearing(e, p)
  local f, l = axes(e)
  local d = sub(p, e:pos())
  return math.deg(math.atan(dot(d, l), dot(d, f)))
end

local function tag_of(e, name)
  local ok, p, f = pcall(e.tag, e, name)
  if ok and p then return p, f end
end

function P.wrap(brain, opts)
  opts = opts or {}
  local tags, period = opts.tags or {}, opts.every or 0.5
  local out = opts.print or print
  local missing = {}
  local w = setmetatable({}, { __index = brain })

  local function enemy_of(e)
    local ok, en = pcall(e.enemy, e)
    if ok and en and en:alive() then return en end
  end

  w.spawn = function(e)
    e.state.probe_next = 0
    out(("probe spawn %s"):format(tostring(e:def())))
    return brain.spawn(e)
  end

  w.think = function(e, dt)
    local t = now()
    if t >= (e.state.probe_next or 0) then
      e.state.probe_next = t + period
      local en = enemy_of(e)
      local ok, anim = pcall(e.anim_current, e)
      -- the engine's fire state rides on the pose line, so a state that should fire and does not shows why
      local fok, fs = pcall(weapon.state, e)
      local fire = "?"
      if fok and fs then
        fire = ("%s/%s/pulled=%s/ready=%s/focus=%s/muzzle=%s"):format(tostring(fs.mode), tostring(fs.anim_mode),
          tostring(fs.pulled), tostring(fs.ready), tostring(fs.focus_valid), tostring(fs.on_target))
      end
      out(("probe pose t=%.2f yaw=%.1f enemy_bearing=%s enemy_dist=%s anim=%s fire=%s"):format(t, e:yaw(),
        en and ("%.1f"):format(bearing(e, en:pos())) or "none",
        en and ("%.0f"):format(e:distance(en)) or "none", ok and tostring(anim) or "?", fire))
      for _, name in ipairs(tags) do
        local p = tag_of(e, name)
        if p then
          out(("probe tag %s %.1f %.1f %.1f"):format(name, rel(e, p)))
        elseif not missing[name] then
          missing[name] = true
          out(("probe tag %s MISSING"):format(name))
        end
      end
    end
    return brain.think(e, dt)
  end

  w.projectile = function(e, p, decl, origin, dir)
    local o = origin or p:pos()
    local best, best_d
    for _, name in ipairs(tags) do
      local tp = tag_of(e, name)
      if tp then
        local d = len(sub(tp, o))
        if not best_d or d < best_d then best, best_d = name, d end
      end
    end
    local en = enemy_of(e)
    local aim = "none"
    if en and dir then
      local to = sub(en:pos(), o)
      local n = len(to) * len(dir)
      if n > 0 then aim = ("%.1f"):format(deg(dot(to, dir) / n)) end
    end
    local fok, fs = pcall(weapon.state, e)
    if fok then
      out(("probe fire %s %s pulled=%s ready=%s focus=%s muzzle=%s radius=%s precision=%s require=%s"):format(
        tostring(fs.mode), tostring(fs.anim_mode), tostring(fs.pulled), tostring(fs.ready),
        tostring(fs.focus_valid), tostring(fs.on_target), fs.radius and ("%.1f"):format(fs.radius) or "?",
        tostring(fs.precision), tostring(fs.require_muzzle)))
    else
      out("probe fire UNAVAILABLE")
    end
    local a, b, c = rel(e, o)
    out(("probe shot %s %.1f %.1f %.1f | nearest %s %.1f | aim_error=%s | enemy_bearing=%s"):format(
      tostring(decl), a, b, c, tostring(best), best_d or -1, aim,
      en and ("%.1f"):format(bearing(e, en:pos())) or "none"))
    return brain.projectile(e, p, decl, origin, dir)
  end

  w.frame = function(e, command, args)
    local parts = {}
    for i, v in ipairs(args or {}) do parts[i] = tostring(v) end
    out(("probe frame %s %s"):format(command, table.concat(parts, ":")))
    return brain.frame(e, command, args)
  end

  return w
end

return P
