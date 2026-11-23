-- test_enemy_target.lua: DOOM 2016's Shared_SocialRelationAvailable as the Enemy SDK evaluates it on the engine's
-- relation rows (enemy_target.lua): the bands and their defaults, the players filters, the awareness band, the
-- 10,000-unit range, and the pick (the nearest of the most hostile emotion present).
--   lua54 tests/sdk/test_enemy_target.lua      (run from the repository root; tests/sdk/run.ps1 does)
local here = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]*$") or "."
package.path = (os.getenv("SNAPMAP_SDK_LUA") or (here .. "/../../src/sdk/lua")) .. "/?.lua;" .. package.path

local T = require("enemy_target")
local failed, cases = 0, 0
local function check(name, ok, detail)
  cases = cases + 1
  if ok then print("PASS " .. name) else failed = failed + 1; print("FAIL " .. name .. (detail and (": " .. tostring(detail)) or "")) end
end

local function ent(x, player) return { pos = { x = x, y = 0, z = 0 }, player = player, is_player = function(s) return s.player == true end } end
local rows = {}
local A = {
  relations = function() return rows end,
  alive = function(t) return t ~= nil and not t.dead end,
  distance = function(e, t) return math.abs(t.pos.x - e.pos.x) end,
  off_limits = function(e, t) return t.off == true end,
}
local me = ent(0)
local function row(x, emotion, awareness, respect, unlimited)
  return { entity = x, emotion = emotion, awareness = awareness or "confirmed", respect = respect or "minion", unlimited = unlimited }
end

do
  local b = T.band({})
  check("the condition's own defaults: every respect, every emotion, every awareness",
        b[1] == 0 and b[2] == 6 and b[3] == 0 and b[4] == 14 and b[5] == 0 and b[6] == 4)
  b = T.band({ minEmotion = "EMOTION_DESTROY_AT_ALL_COSTS", maxEmotion = "EMOTION_ANGER", awareness_min = "AIAWARE_SUSPECTED",
               maxRespect = "RESPECT_PEER" })
  check("bands are read by name", b[3] == 4 and b[4] == 7 and b[5] == 2 and b[2] == 3)
  b = T.band({ maxEmotion = "EMOTION_ONLY_IN_ANOTHER_GAME" })
  check("a name this game lacks leaves the bound at its default", b[4] == 14)
end

do
  local p1, p2, d1 = ent(300, true), ent(100, true), ent(50)
  rows = { row(d1, "destroy"), row(p1, "destroy"), row(p2, "anger") }
  local list, pick = T.available(A, me, { maxEmotion = "EMOTION_ANGER" })
  check("every hostile row in the band is admitted", #list == 3)
  check("the pick is the nearest of the most hostile emotion", pick == d1)
  local _, pp = T.available(A, me, { playersOnly = true })
  check("playersOnly: players and bots alone", pp == p1)
  local _, np = T.available(A, me, { noPlayers = true })
  check("noPlayers: no player and no bot", np == d1)
  local _, nb = T.available(A, me, { minEmotion = "EMOTION_HATE", maxEmotion = "EMOTION_ANGER" })
  check("the emotion band", nb == p2)
end

do
  local near, far, wide = ent(9000), ent(12000), ent(20000)
  rows = { row(far, "destroy"), row(near, "hate"), row(wide, "destroy", nil, nil, true) }
  local list, pick = T.available(A, me, {})
  check("beyond 10,000 units a row is out, unless it is not range-limited", #list == 2 and pick == wide)
end

do
  local a, b2 = ent(100), ent(200)
  rows = { row(a, "destroy", "suspected"), row(b2, "destroy", "confirmed") }
  local _, pick = T.available(A, me, { awareness_min = "AIAWARE_CONFIRMED" })
  check("the awareness band", pick == b2)
  b2.off = true
  local list = T.relations(A, me, { awareness_min = "AIAWARE_CONFIRMED", filterOutOffLimits = true })
  check("filterOutOffLimits drops a target the demon may not reach", #list == 0)
  check("ambientOnly admits nothing for a demon without ambient social behaviour", #T.relations(A, me, { ambientOnly = true }) == 0)
  b2.dead = true
  check("a dead entity is no relation", #T.relations(A, me, {}) == 1)
end

do
  local t = ent(100)
  rows = { row(t, "destroy", "lost") }
  check("holds: an enemy out of sight is still held", T.holds(A, me, t, T.COMBAT))
  rows = { row(t, "neutral") }
  check("holds: not once the relation is no longer hostile", not T.holds(A, me, t, T.COMBAT))
  check("holds: nothing held without an enemy", not T.holds(A, me, nil, T.COMBAT))
end

do
  rows = {}
  local none = {}
  for k, v in pairs(A) do none[k] = v end
  none.relations = nil
  local list, pick = T.available(none, me, {})
  check("a Snapmap+ that cannot read relations gives none", #list == 0 and pick == nil)
end

print(string.format("%d cases, %d passed, %d failed", cases, cases - failed, failed))
os.exit(failed == 0 and 0 or 1)
