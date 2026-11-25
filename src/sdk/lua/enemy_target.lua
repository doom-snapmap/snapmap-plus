-- enemy_target.lua: who a demon fights, chosen as DOOM 2016's own demons choose (the Enemy SDK's generic tier). It
-- reproduces the condition Shared_SocialRelationAvailable (VK 0x1405F4930) that the engine's state machines use and a
-- scripted demon never runs: rows from ai:relations() (senses' GetRelations VK 0x1404AAA30 / GL 0x1404A91B0) whose
-- respect, emotion and awareness lie in the bands and within 10,000 units, then playersOnly / noPlayers filters and
-- the nearest of the most hostile emotion (VK 0x1404AAD10 / GL 0x1404A9490, distance squared in single precision).
-- Band defaults are the class's constructor (VK 0x141302930); a source game's condition may narrow range and flat.

local T = {}

T.RANGE = 10000          -- GetRelations' distance, units (Eternal's class: 190.48 m, the same)

-- 2016's enums (enumvals: socialRespect_t, socialEmotion_t, aiAwareness_t), by the words ai:relations() answers
T.RESPECT = { min = 0, peon = 1, minion = 2, peer = 3, leader = 4, worship = 5, max = 6 }
T.EMOTION = { min = 0, terrified = 1, fear = 2, wary = 3, destroy_at_all_costs = 4, destroy = 5, hate = 6,
              anger = 7, dislike = 8, annoyed = 9, neutral = 10, friendly = 11, squadmate = 12, partner = 13,
              max = 14 }
T.AWARE = { unaware = 0, lost = 1, suspected = 2, confirmed = 3, max = 4 }

-- The band a demon fights in: 2016's own entry into combat (behavior_shared, shared_combat_check1: any respect,
-- EMOTION_DESTROY_AT_ALL_COSTS..EMOTION_DISLIKE, at AIAWARE_CONFIRMED).
T.COMBAT = { minEmotion = "EMOTION_DESTROY_AT_ALL_COSTS", maxEmotion = "EMOTION_DISLIKE",
             awareness_min = "AIAWARE_CONFIRMED" }
-- The band the generic tier gives a demon its first enemy in: the combat band, where an entity the demon sees now
-- (A.can_see: sight state, seen within the last second, a clear line) counts as confirmed. Relation rows and
-- awareness are refreshed only in turns and by events (senses update VK 0x1404AE3C0) and nothing forces a scripted
-- demon's awareness in single player, so a hostile it sees is engaged at once.
T.FIND = { minEmotion = "EMOTION_DESTROY_AT_ALL_COSTS", maxEmotion = "EMOTION_DISLIKE",
           awareness_min = "AIAWARE_CONFIRMED", sight_confirms = true }

local function word(name, prefix)
  if type(name) ~= "string" then return nil end
  return (name:gsub("^" .. prefix, "")):lower()
end

local function rank(tbl, name, prefix, default)
  local w = word(name, prefix)
  local v = w and tbl[w]
  if v == nil then return default end
  return v
end

--- The condition's bands as numbers: { r0, r1, e0, e1, a0, a1 }.
function T.band(p)
  p = p or {}
  return {
    rank(T.RESPECT, p.minRespect, "RESPECT_", T.RESPECT.min), rank(T.RESPECT, p.maxRespect, "RESPECT_", T.RESPECT.max),
    rank(T.EMOTION, p.minEmotion, "EMOTION_", T.EMOTION.min), rank(T.EMOTION, p.maxEmotion, "EMOTION_", T.EMOTION.max),
    rank(T.AWARE, p.awareness_min, "AIAWARE_", T.AWARE.unaware), rank(T.AWARE, p.awareness_max, "AIAWARE_", T.AWARE.max),
  }
end

local function is_player(x)
  local ok, v = pcall(function() return x:is_player() end)
  return ok and v == true
end

--- The engine's relation rows of demon e, from the adapter's A.relations, else from e:relations(); nil when neither
-- can read them.
local function rows_of(A, e)
  if A.relations then return A.relations(e) end
  local ok, has = pcall(function() return e.relations end)
  if ok and has ~= nil then return e:relations() end
  return nil
end
T.rows_of = rows_of

--- The relations of demon e that condition p admits: { { entity, respect, emotion, awareness, distance }, ... } with
-- numbers for the words.
function T.relations(A, e, p)
  p = p or {}
  local b = T.band(p)
  local out = {}
  if p.ambientOnly then return out end     -- 2016: only a demon whose behaviour runs its ambient social behaviour
  for _, r in ipairs(rows_of(A, e) or {}) do
    local x = r.entity
    local respect, emotion = T.RESPECT[r.respect], T.EMOTION[r.emotion]
    local aware = T.AWARE[r.awareness or "unaware"] or 0
    if x ~= nil and x ~= e and respect and emotion and respect >= b[1] and respect <= b[2]
        and emotion >= b[3] and emotion <= b[4] and A.alive(x) then
      local d = p.flat and A.flat_distance and A.flat_distance(e, x) or A.distance(e, x)
      local in_range = r.unlimited or (type(d) == "number" and d < (p.range or T.RANGE))
      local player = is_player(x)
      if in_range and not (p.playersOnly and not player) and not (p.noPlayers and player)
          and ((aware >= b[5] and aware <= b[6]) or (p.sight_confirms and A.can_see and A.can_see(e, x)))
          and not (p.filterOutOffLimits and A.off_limits and A.off_limits(e, x)) then
        out[#out + 1] = { entity = x, respect = respect, emotion = emotion, awareness = aware, distance = d }
      end
    end
  end
  return out
end

-- 2016 starts the pick's best at FLT_MAX (VK 0x1404AAD10) and only a smaller squared distance replaces it, so a row
-- whose entity is at no measurable point (NaN, infinite, or a square past a float) is never picked but still counts
-- as a relation. Only a row with no range (r.unlimited) can be unmeasurable: a ranged row fails the range first.
local FLT_MAX = 3.4028234663852886e38
local function measurable(d) return type(d) == "number" and d == d and d * d < FLT_MAX end
T.measurable = measurable

--- 2016's pick among admitted relations: the nearest measurable row of the most hostile emotion present, or nil.
-- The most hostile emotion counts whether or not its rows are measurable, so a bucket of only unmeasurable rows gives
-- no pick (it does not give way to a less hostile one).
function T.pick(list)
  local worst
  for _, r in ipairs(list) do
    if worst == nil or r.emotion < worst then worst = r.emotion end
  end
  local best
  for _, r in ipairs(list) do
    if r.emotion == worst and measurable(r.distance) and (best == nil or r.distance < best.distance) then best = r end
  end
  return best and best.entity or nil
end

--- The condition evaluated: the admitted relations and the pick.
function T.available(A, e, p)
  local list = T.relations(A, e, p)
  return list, T.pick(list)
end

--- true when demon e still holds t in condition p's respect and emotion bands (awareness aside: a demon keeps an
-- enemy it has lost sight of, and DOOM 2016's search takes over from there).
function T.holds(A, e, t, p)
  if t == nil or not A.alive(t) then return false end
  local q = {}
  for k, v in pairs(p or {}) do q[k] = v end
  q.awareness_min, q.awareness_max, q.filterOutOffLimits = nil, nil, nil
  for _, r in ipairs(T.relations(A, e, q)) do
    if r.entity == t then return true end
  end
  return false
end

return T
