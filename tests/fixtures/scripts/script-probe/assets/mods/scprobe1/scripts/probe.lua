api = 1

-- One line when each marker spawns, then one line a second from each
-- marker while the map plays.

local spawned = 0

on.map_start(function()
  print("probe started")
end)

on.spawn("probe/marker", function(self)
  spawned = spawned + 1
  self.state.next = now() + 1
  print("marker " .. spawned .. " spawned")
end)

on.think("probe/marker", function(self, dt)
  if now() >= self.state.next then
    self.state.next = self.state.next + 1
    print("marker thinking at " .. string.format("%.1f", now()))
  end
end)
