api = 1

-- A think that never returns. The game stops this mod's scripts in the
-- frame it starts, says where, and keeps running every other mod.

on.think("runaway/block", function(self)
  while true do
  end
end)
