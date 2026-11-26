package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// Project only a recognized earlier map. The caller decides where to keep the separate copy.
func projectVanillaMap(original []byte, pins *earlierPins) ([]byte, error) {
	raw := bytes.TrimPrefix(original, []byte{0xef, 0xbb, 0xbf})
	view := earlierJSONText(raw)
	var root earlierObject
	if err := json.Unmarshal(view, &root); err != nil {
		return nil, err
	}
	kind := pinString(root, "~type")
	if kind != "" && kind != "idSnapMap" || kind == "" && (root["instances"] == nil || root["entities"] == nil || root["instanceEntities"] == nil || root["version"] == nil) {
		return nil, fmt.Errorf("expected a native SnapMap map")
	}
	if !hasHistoricalEvidence(raw, nil, pins) {
		return nil, fmt.Errorf("no recognized earlier map content")
	}
	owners := vanillaOwners{names: map[string]bool{}, ids: map[string]bool{}}
	if pins != nil {
		owners.doom = pins.doom
		if pins.placements != nil {
			for name := range pins.placements.byName {
				owners.names[name] = true
			}
		}
	}
	// Use the same released carrier, manifest count and navigation checks as the modded conversion.
	if _, err := earlierVariables(raw, root, owners.carried, nil, nil); err != nil {
		return nil, err
	}
	if member := root["snapmap-plus"]; member != nil {
		var block struct {
			Format int
			Mods   []earlierObject
		}
		if json.Unmarshal(member, &block) != nil || (block.Format != 1 && block.Format != 2) {
			return nil, fmt.Errorf("unsupported map mod metadata")
		}
		for _, r := range block.Mods {
			if err := owners.row(r); err != nil {
				return nil, err
			}
		}
	}
	g := vanillaLinks{removed: map[int]bool{}, modules: -1}
	changes := map[string][]byte{"snapmap-plus": nil}
	source := func(key string) ([]byte, error) {
		a, b, _, err := earlierMemberSpan(raw, key)
		if err != nil || a == b {
			return nil, err
		}
		return raw[a:b], nil
	}
	vars, err := source("variables")
	if err != nil {
		return nil, err
	}
	if vars != nil {
		v, err := vanillaVariables(vars, raw, &owners, &g)
		if err != nil {
			return nil, err
		}
		changes["variables"] = v
	}
	instances, err := source("instances")
	if err != nil {
		return nil, err
	}
	if instances != nil {
		next, err := g.instances(instances, root, &owners)
		if err != nil {
			return nil, err
		}
		changes["instances"] = next
	}
	entities, err := source("entities")
	if err != nil {
		return nil, err
	}
	if entities == nil && len(owners.sites) > 0 {
		return nil, fmt.Errorf("recorded placements are absent")
	}
	if entities != nil {
		seen := map[int]bool{}
		size := 0
		value, err := vanillaArray(entities, func(i int, row []byte) ([]byte, error) { size++; return owners.entity(i, row, &g, seen) })
		if err != nil {
			return nil, err
		}
		for i := range owners.sites {
			if i >= size {
				return nil, fmt.Errorf("recorded placement %d is absent", i)
			}
		}
		changes["entities"] = value
	}
	out, err := vanillaObject(raw, func(k string, v []byte) ([]byte, error) {
		if val, ok := changes[k]; ok {
			return val, nil
		}
		if len(g.removed) != 0 || g.variables != nil || len(g.removedModules) != 0 {
			return g.table(k, v)
		}
		return v, nil
	})
	if err != nil {
		return nil, err
	}
	if bytes.Equal(raw, out) {
		return nil, fmt.Errorf("map has no removable earlier content")
	}
	if len(raw) != len(original) {
		out = append([]byte{0xef, 0xbb, 0xbf}, out...)
	}
	return out, nil
}

// A prototype row is removable only if its payload authenticates the matching serialized identity.
func vanillaPrototype(vars []earlierVariable, original []byte, owners *vanillaOwners) (map[string]bool, error) {
	active := map[string]bool{}
	var chunks []string
	var binderNames []string
	var hash string
	seenBind, damaged := false, false
	for _, v := range vars {
		if v.Info.Type != "snapVarInfo_t" {
			continue
		}
		if m := prototypeReference.FindStringSubmatch(v.Info.Name); m != nil {
			var r struct {
				ID, Name, SHA256 string
				Packed           int64
			}
			if json.Unmarshal(prototypeBody(v.InitialValue, m[1]), &r) == nil && earlierID.MatchString(r.ID) && r.Name != "" && pinHash(r.SHA256) && r.Packed > 0 {
				owners.ids[strings.ToLower(r.ID)] = true
				active[v.Info.Name] = true
			}
		}
		if m := prototypeBinding.FindStringSubmatch(v.Info.Name); m != nil {
			seenBind = true
			i, ei := strconv.Atoi(m[1])
			n, en := strconv.Atoi(m[2])
			if ei != nil || en != nil || n <= 0 || n > len(vars) || i < 0 || i >= n {
				damaged = true
				continue
			}
			if chunks == nil {
				chunks = make([]string, n)
				hash = m[3]
			}
			if n != len(chunks) || hash != m[3] || chunks[i] != "" {
				damaged = true
				continue
			}
			chunks[i] = v.InitialValue
			binderNames = append(binderNames, v.Info.Name)
		}
	}
	if !seenBind {
		return active, nil
	}
	var body []byte
	if !damaged && len(chunks) > 0 && len(binderNames) == len(chunks) {
		body = prototypeBody(strings.Join(chunks, ""), hash)
	}
	var doc earlierObject
	if err := json.Unmarshal(earlierJSONText(original), &doc); err != nil {
		return nil, err
	}
	if len(body) < 8 || string(body[:7]) != "SMPBIND" || body[7] > 2 {
		if err := earlierPrototypeDependency(original, doc); err != nil {
			return nil, err
		}
		return active, nil
	}
	if err := owners.bindings(body); err != nil {
		return nil, err
	}
	for _, n := range binderNames {
		active[n] = true
	}
	return active, nil
}

func vanillaVariables(raw, original []byte, owners *vanillaOwners, g *vanillaLinks) ([]byte, error) {
	var variables earlierObject
	if err := json.Unmarshal(earlierJSONText(raw), &variables); err != nil {
		return nil, err
	}
	if variables["string"] == nil {
		return raw, nil
	}
	var values []json.RawMessage
	if err := json.Unmarshal(variables["string"], &values); err != nil {
		return nil, err
	}
	vars := make([]earlierVariable, len(values))
	for i, v := range values {
		_ = json.Unmarshal(v, &vars[i])
	}
	proto, err := vanillaPrototype(vars, original, owners)
	if err != nil {
		return nil, err
	}
	count := 0
	next := make([]int, len(vars))
	for i, v := range vars {
		drop := v.Info.Type == "snapVarInfo_t" && (historicalVariable(v.Info.Name, v.InitialValue) || earlierStampName.MatchString(v.Info.Name) && v.InitialValue == "" || proto[v.Info.Name])
		if drop {
			next[i] = -1
		} else {
			next[i] = count
			count++
		}
	}
	if count == len(vars) {
		return raw, nil
	}
	g.variables = next
	return vanillaObject(raw, func(k string, v []byte) ([]byte, error) {
		switch k {
		case "string":
			return vanillaArray(v, func(i int, item []byte) ([]byte, error) {
				if next[i] < 0 {
					return nil, nil
				}
				return item, nil
			})
		case "allocCount":
			counts, err := vanillaInts(v)
			if err != nil || len(counts) <= 4 {
				return nil, fmt.Errorf("invalid string-variable counts")
			}
			counts[4] = count
			return pinJSON(counts), nil
		}
		return v, nil
	})
}
