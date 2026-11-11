api = 1

-- A two-attack enemy: it closes in, hits up close, and throws a rock when
-- it can see the player from further away, at most every three seconds.
-- Its animWeb has "melee" and "throw" nodes; the throw clip's release frame
-- has the md6 row ae_notify "throw", and both clips end with
-- ae_notify "attack_done".

on.think("ai/brute", function(self)
  local p = self:enemy() or players()[1]
  if not p or self.state.busy then return end
  self:set_enemy(p)
  self:face(p)
  local d = self:distance(p)
  if d < 120 then
    self.state.busy = true
    self:anim_state("melee")
  elseif self:can_see(p) and now() > (self.state.next_throw or 0) then
    self.state.busy = true
    self.state.next_throw = now() + 3
    self:anim_state("throw")
  else
    self:move_to_entity(p)
  end
end)

on.anim("ai/brute", "throw", function(self)
  self:fire("brute/rock", "hand_r", self:enemy())
end)

on.anim("ai/brute", "attack_done", function(self)
  self.state.busy = false
end)
