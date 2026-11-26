api = 1
do
  local on = on.enemy("actors/placed")
  on.spawn("actors/placed", function(self)
    assert(self:wound_effect("wounds/shell", "FX_CINEMATIC_2"))
  end)
end
local brain = {}
function brain.spawn(self) self.state.live = true end
function brain.think(self) assert(self.state.live) end
function brain.damage(self, hit) assert(hit.amount == 37) end
function brain.remove(self) self.state.live = nil end
require("enemy_sdk").bind("actors/placed", brain)
