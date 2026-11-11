api = 1

-- A pickup that spins and heals: a player who comes within 40 units with
-- less than 100 health gets 25 back, and the pickup is gone.

on.think("pickups/spin_heal", function(self, dt)
  self:set_yaw(self:yaw() + 90 * dt)
  for _, p in ipairs(players()) do
    if p:distance(self) < 40 and p:health() < 100 then
      p:set_health(math.min(100, p:health() + 25))
      self:remove()
      return
    end
  end
end)
