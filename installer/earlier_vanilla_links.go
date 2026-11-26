package main

import (
	"fmt"
	"slices"
)

type vanillaLinks struct {
	removed                        map[int]bool
	variables                      []int
	modules                        int
	removedModules, removedPortals map[int]bool
}

func (g vanillaLinks) multimap(raw []byte, kind string) ([]byte, error) {
	var offsets, vals []int
	var err error
	for _, field := range []string{"keyValues", "values"} {
		a, b, _, e := earlierMemberSpan(raw, field)
		if e != nil || a == b {
			return nil, fmt.Errorf("incomplete %s table", kind)
		}
		if field == "keyValues" {
			offsets, err = vanillaInts(raw[a:b])
		} else {
			vals, err = vanillaInts(raw[a:b])
		}
		if err != nil {
			return nil, err
		}
	}
	if len(offsets) == 0 && len(vals) == 0 && kind != "instanceEntities" {
		return raw, nil
	}
	if len(offsets) == 0 || offsets[0] != 0 || offsets[len(offsets)-1] != len(vals) || !slices.IsSorted(offsets) {
		return nil, fmt.Errorf("invalid %s offsets", kind)
	}
	if kind == "instanceEntities" && g.modules >= 0 && len(offsets) != g.modules+2 {
		return nil, fmt.Errorf("module membership does not match the instances")
	}
	newOffsets := []int{0}
	newVals := make([]int, 0, len(vals))
	for uid := 0; uid+1 < len(offsets); uid++ {
		if kind == "instanceEntities" && g.removedModules[uid] {
			continue
		}
		for _, v := range vals[offsets[uid]:offsets[uid+1]] {
			switch kind {
			case "instanceEntities":
				if g.removed[v] {
					continue
				}
			case "entityEntRefs":
				if g.removed[uid] {
					continue
				}
				if g.removed[v] {
					v = -1
				}
			case "entityVarRefs":
				if g.removed[uid] {
					continue
				}
				if len(g.variables) > 0 && v>>24 == 4 && v&0xffffff != 0xffffff {
					i := v & 0xffffff
					if i >= len(g.variables) {
						return nil, fmt.Errorf("invalid string variable index %d", i)
					}
					j := g.variables[i]
					if j < 0 {
						j = 0xffffff
					}
					v = 4<<24 | j
				}
			case "entitySources", "entityTargets":
				if g.removed[uid] || g.removed[v] {
					return nil, fmt.Errorf("cannot project an affected native %s cache", kind)
				}
			}
			newVals = append(newVals, v)
		}
		newOffsets = append(newOffsets, len(newVals))
	}
	if slices.Equal(vals, newVals) && slices.Equal(offsets, newOffsets) {
		return raw, nil
	}
	return vanillaObject(raw, func(k string, v []byte) ([]byte, error) {
		switch k {
		case "keyValues":
			return pinJSON(newOffsets), nil
		case "values":
			return pinJSON(newVals), nil
		}
		return v, nil
	})
}

func (g vanillaLinks) connections(raw []byte) ([]byte, error) {
	vals, err := vanillaInts(raw)
	if err != nil {
		return nil, err
	}
	next := make([]int, 0, len(vals))
	source := -1
	header := false
	for _, v := range vals {
		if v < 0 {
			source = -(v + 1)
			header = false
			continue
		}
		if source < 0 {
			return nil, fmt.Errorf("target before connection source")
		}
		if g.removed[source] || g.removed[v] {
			continue
		}
		if !header {
			next = append(next, -source-1)
			header = true
		}
		next = append(next, v)
	}
	if slices.Equal(vals, next) {
		return raw, nil
	}
	return pinJSON(next), nil
}

func (g vanillaLinks) table(k string, v []byte) ([]byte, error) {
	if len(g.removed) == 0 && len(g.removedModules) == 0 && k != "references" {
		return v, nil
	}
	switch k {
	case "instanceEntities":
		return g.multimap(v, k)
	case "references", "targets", "doorsAndCaps":
		return vanillaObject(v, func(name string, raw []byte) ([]byte, error) {
			switch {
			case k == "references" && (name == "entityEntRefs" || name == "entityVarRefs"), k == "targets" && (name == "entitySources" || name == "entityTargets"):
				return g.multimap(raw, name)
			case k == "targets" && name == "connections":
				return g.connections(raw)
			case k == "doorsAndCaps" && (name == "portalDoors" || name == "portalFrames"):
				values, err := vanillaInts(raw)
				if err != nil {
					return nil, err
				}
				changed := false
				next := make([]int, 0, len(values))
				for i, id := range values {
					if g.removedPortals[i] {
						changed = true
						continue
					}
					if g.removed[id] {
						id = -1
						changed = true
					}
					next = append(next, id)
				}
				if changed {
					return pinJSON(next), nil
				}
			}
			return raw, nil
		})
	}
	return v, nil
}
