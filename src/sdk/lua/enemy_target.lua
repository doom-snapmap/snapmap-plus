-- enemy_target.lua: who a demon fights, chosen as DOOM 2016's own demons choose (the Enemy SDK's generic tier).
--
-- DOOM 2016 keeps every demon's social relations in its senses and rebuilds them every think, whatever drives the
-- demon: one row per entity it perceives (players and bots alike, other demons), with a respect and an emotion that
-- its faction graph gives (a SnapMap team makes the faction "team_<n>_demon", players "team_<n>_player") and that
-- what it perceives raises (being hurt by someone makes them hated). ai:relations() reads those rows. The engine
-- never picks an enemy from them by itself: its state machines do, with the condition Shared_SocialRelationAvailable,
-- and a demon whose brain is a script runs none of them. This module is that condition (VK 0x1405F4930, read):
--
--   rows      the relations whose respect and emotion lie in the condition's bands, within 10,000 units (a row
--             with no entity of its own is not range-limited), as the senses' GetRelations gives them (VK
--             0x1404AAA30 / GL 0x1404A91B0);
--   filters   playersOnly / noPlayers (a bot is a player), the awareness band, filterOutOffLimits;
--   the pick  the nearest of the most hostile emotion present (VK 0x1404AAD10 / GL 0x1404A9490).
--
-- A source game whose condition narrows the range gives it: range (units, instead of the 10,000) and flat (distance on
-- the ground plane); DOOM Eternal's class has maxRange, rangeString and use2DChecks for them.
--
-- The bands' defaults are the class's own (its constructor, VK 0x141302930): respect RESPECT_MIN..RESPECT_MAX,
-- emotion EMOTION_MIN..EMOTION_MAX, awareness AIAWARE_UNAWARE..AIAWARE_MAX. A source game's brain names the same
-- condition with the same fields (DOOM Eternal's class has them, its emotion list one longer); a band is read by
-- its name, so a name this game lacks leaves that bound at its default.
--
-- The team rule, bots and infighting are therefore the engine's: nothing here reads a team or lists players.

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
-- The band the generic tier gives a demon its first enemy in: the combat band, where the demon's perception seeing the
-- entity now (A.can_see: its sight state, seen within the last second, and a clear line) counts as confirmed. 2016's
-- perception raises awareness from its sight stimulus (the record's stimulus update VK 0x140651360), but a demon's
-- relation rows and awareness words are refreshed in turns and by events (the senses update VK 0x1404AE3C0: at spawn,
-- on an encounter's spawn, on the AI manager's turn), and nothing forces a scripted demon's awareness in single player
-- without co-op tracking: a hostile the demon sees is engaged at once, as a stock demon's alert cycle does on sight.
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

--- The relations of demon e that condition p admits, in the engine's order: { { entity, respect, emotion,
-- awareness, distance }, ... } with numbers for the words. The rows are the engine's: the adapter's A.relations when
-- it has one, else Snapmap+'s own e:relations() (a brain library emitted before the adapter had the call still gets
-- the engine's rows); none when this Snapmap+ cannot read them.
local function rows_of(A, e)
  if A.relations then return A.relations(e) end
  local ok, has = pcall(function() return e.relations end)
  if ok and has ~= nil then return e:relations() end
  return nil
end
T.rows_of = rows_of

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
      local in_range = r.unlimited or d < (p.range or T.RANGE)
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

--- 2016's pick among admitted relations: the nearest of the most hostile emotion present, or nil.
function T.pick(list)
  local worst
  for _, r in ipairs(list) do
    if worst == nil or r.emotion < worst then worst = r.emotion end
  end
  local best
  for _, r in ipairs(list) do
    if r.emotion == worst and (best == nil or r.distance < best.distance) then best = r end
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
