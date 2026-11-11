api = 1

-- A door that opens when three switches are on.
-- In the map: each switch's On Used goes to a Script node named
-- "switch_used"; a second Script node named "open_door" has its On Fired
-- wired to the door's Open.

local on_count = 0

on.input("switch_used", function(switch_node, activator)
  on_count = on_count + 1
  if on_count == 3 then
    node("open_door"):fire(activator)
  end
end)
