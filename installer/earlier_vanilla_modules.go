package main

import (
	"encoding/json"
	"fmt"
	"slices"
	"strconv"
	"strings"
)

// These are the intrinsic Grid Room names and dimensions accepted by sh_grid_parse_name.
func vanillaGridRoom(name string) (bool, error) {
	rest, found := strings.CutPrefix(name, "maps/modules/smpgrid/v1/")
	if !found {
		return false, nil
	}
	kind, size, ok := strings.Cut(rest, "/")
	values := strings.Split(strings.TrimSuffix(size, ".decl"), "_")
	if !ok || kind != "m" && kind != "c" || len(values) != 3 {
		return false, fmt.Errorf("unrecognized built-in Grid Room %q", name)
	}
	low, high := [3]int{864, 272, 432}, [3]int{65342, 62846, 32767}
	if kind == "c" {
		low, high = [3]int{416, 416, 304}, [3]int{65406, 65406, 32767}
	}
	for i, s := range values {
		n, e := strconv.Atoi(s)
		if e != nil || strconv.Itoa(n) != s || n < low[i] || n > high[i] {
			return false, fmt.Errorf("invalid built-in Grid Room %q", name)
		}
	}
	return true, nil
}

// The last membership group is global. Portal slots are attributed by their door's module, as in the prefab writer.
func (g *vanillaLinks) instances(raw []byte, root earlierObject, o *vanillaOwners) ([]byte, error) {
	var instances []struct{ ModuleName, EnvironmentName string }
	if err := json.Unmarshal(earlierJSONText(raw), &instances); err != nil || instances == nil {
		return nil, fmt.Errorf("invalid module instances")
	}
	g.modules = len(instances)
	for i, m := range instances {
		if missing, e := o.unavailable("env", m.EnvironmentName); e != nil {
			return nil, e
		} else if missing {
			return nil, fmt.Errorf("cannot restore the original environment for room %d", i)
		}
		if grid, err := vanillaGridRoom(m.ModuleName); err != nil {
			return nil, err
		} else if grid {
			continue
		}
		missing, e := o.unavailable("snapmodule", m.ModuleName)
		if e != nil {
			return nil, e
		}
		if missing {
			if g.removedModules == nil {
				g.removedModules = map[int]bool{}
			}
			g.removedModules[i] = true
		}
	}
	if len(g.removedModules) == 0 {
		return raw, nil
	}
	if len(g.removedModules) == g.modules {
		return nil, fmt.Errorf("the map has no surviving room")
	}
	var members struct{ KeyValues, Values []int }
	if json.Unmarshal(root["instanceEntities"], &members) != nil || len(members.KeyValues) != g.modules+2 || members.KeyValues[0] != 0 || members.KeyValues[g.modules+1] != len(members.Values) || !slices.IsSorted(members.KeyValues) {
		return nil, fmt.Errorf("cannot establish native room membership")
	}
	groups := map[int]int{}
	for i := 0; i <= g.modules; i++ {
		for _, id := range members.Values[members.KeyValues[i]:members.KeyValues[i+1]] {
			if _, ok := groups[id]; id < 0 || ok {
				return nil, fmt.Errorf("ambiguous room membership for %d", id)
			}
			groups[id] = i
			if g.removedModules[i] {
				g.removed[id] = true
			}
		}
	}
	var portals struct{ PortalDoors, PortalFrames []int }
	if json.Unmarshal(root["doorsAndCaps"], &portals) != nil || portals.PortalDoors == nil || portals.PortalFrames == nil || len(portals.PortalDoors) != len(portals.PortalFrames) {
		return nil, fmt.Errorf("cannot establish the native portal slots")
	}
	previous := -1
	g.removedPortals = map[int]bool{}
	for i, id := range portals.PortalDoors {
		group, exists := groups[id]
		if !exists || group == g.modules || group < previous {
			return nil, fmt.Errorf("cannot determine the owning room for portal %d", i)
		}
		previous = group
		if g.removedModules[group] {
			g.removedPortals[i] = true
		}
	}
	return vanillaArray(raw, func(i int, v []byte) ([]byte, error) {
		if g.removedModules[i] {
			return nil, nil
		}
		return v, nil
	})
}
