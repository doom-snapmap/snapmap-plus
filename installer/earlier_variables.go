package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

var earlierNavName = regexp.MustCompile(`^smnav1\.([A-Za-z0-9_./-]+)/?\.([A-Za-z0-9_-]+)\.([0-9]{1,7})\.([0-9]{1,7})\.([a-f0-9]{16})\.([a-f0-9]{16})$`)
var earlierRowName = regexp.MustCompile(`^smpmod1\.([0-9]{1,7})\.([0-9]{1,7})$`)

type earlierNav struct {
	Module string   `json:"module"`
	Class  string   `json:"class"`
	Digest string   `json:"digest"`
	Geom   string   `json:"geom"`
	Data   []string `json:"data"`
}

func (set *earlierNav) check() error {
	text := strings.Join(set.Data, "")
	if base64.StdEncoding.DecodedLen(len(text)) > 8*1024*1024+2 {
		return fmt.Errorf("navigation for %s is too large", set.Module)
	}
	for _, s := range set.Data {
		if s == "" {
			return fmt.Errorf("navigation for %s is incomplete", set.Module)
		}
	}
	b, err := base64.StdEncoding.DecodeString(text)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(b)
	if hex.EncodeToString(sum[:8]) != set.Digest {
		return fmt.Errorf("navigation for %s differs from its saved hash", set.Module)
	}
	return nil
}

func earlierVariables(src []byte, root earlierObject, resolve earlierCarriedResolver, ids map[string]string) ([]byte, error) {
	raw := root["variables"]
	if raw == nil || !strings.Contains(string(raw), "snapVarInfo_t") {
		return src, nil
	}
	var variables earlierObject
	if err := json.Unmarshal(raw, &variables); err != nil {
		return src, nil
	}
	var rows []json.RawMessage
	if err := json.Unmarshal(variables["string"], &rows); err != nil {
		return src, nil
	}
	var block earlierObject
	if b := root["snapmap-plus"]; b != nil {
		if err := json.Unmarshal(b, &block); err != nil || block == nil {
			return nil, fmt.Errorf("unreadable map member")
		}
	} else {
		block = earlierObject{"format": json.RawMessage("1")}
	}
	settings := map[string]string{}
	mods := []json.RawMessage{}
	sets := []*earlierNav{}
	nav := map[string]*earlierNav{}
	carriers := map[string]*earlierCarrier{}
	used := map[string]bool{}
	old := make([]bool, len(rows))
	lastAuthor := -1
	found := false
	for i, raw := range rows {
		var row struct {
			Info struct {
				Name string
				Type string `json:"~type"`
			}
			InitialValue string
		}
		if err := json.Unmarshal(raw, &row); err != nil || row.Info.Type != "snapVarInfo_t" {
			lastAuthor = i
			continue
		}
		name := row.Info.Name
		switch {
		case name == "smp.render.v1" || name == "smp.bounds.v1":
			key := strings.Split(name, ".")[1]
			if block["settings"] == nil {
				if prior, ok := settings[key]; ok && prior != row.InitialValue {
					return nil, fmt.Errorf("conflicting earlier %s settings", key)
				}
				settings[key] = row.InitialValue
			}
		case strings.HasPrefix(name, "smpprod1."):
		case strings.HasPrefix(name, "smpmod1."):
			m := earlierRowName.FindStringSubmatch(name)
			if m == nil {
				return nil, fmt.Errorf("invalid earlier mod row %s", name)
			}
			idx, _ := strconv.Atoi(m[1])
			total, _ := strconv.Atoi(m[2])
			if idx >= total {
				return nil, fmt.Errorf("invalid earlier mod row %s", name)
			}
			if block["mods"] == nil {
				if !json.Valid([]byte(row.InitialValue)) {
					return nil, fmt.Errorf("unreadable earlier mod row %s", name)
				}
				mods = append(mods, json.RawMessage(row.InitialValue))
			}
		case strings.HasPrefix(name, "smnav1."):
			if block["nav"] != nil {
				break
			}
			m := earlierNavName.FindStringSubmatch(name)
			if m == nil || !strings.Contains(m[1], "/") || strings.Contains(m[1], "//") || strings.HasPrefix(m[1], "/") || strings.HasSuffix(m[1], "/") {
				return nil, fmt.Errorf("invalid earlier navigation %s", name)
			}
			idx, _ := strconv.Atoi(m[3])
			total, _ := strconv.Atoi(m[4])
			if idx >= total || total > 2048 {
				return nil, fmt.Errorf("invalid navigation chunk count in %s", name)
			}
			key := m[1] + "\t" + m[2]
			s := nav[key]
			if s == nil {
				s = &earlierNav{Module: m[1], Class: m[2], Digest: m[5], Geom: m[6], Data: make([]string, total)}
				nav[key] = s
				sets = append(sets, s)
			}
			if len(s.Data) != total || s.Digest != m[5] || s.Geom != m[6] || s.Data[idx] != "" {
				return nil, fmt.Errorf("conflicting navigation chunks for %s", m[1])
			}
			s.Data[idx] = row.InitialValue
		case strings.HasPrefix(name, "smpkg."):
			if err := addEarlierCarrier(carriers, name, row.InitialValue); err != nil {
				return nil, err
			}
		case strings.HasPrefix(name, "smpbind.") || strings.HasPrefix(name, "smpref."):
			if !earlierRetiredName.MatchString(name) {
				return nil, fmt.Errorf("unrecognized earlier binding %s", name)
			}
		default:
			used[strings.ToLower(name)] = true
			lastAuthor = i
			continue
		}
		old[i] = true
		found = true
	}
	if !found {
		return src, nil
	}
	var err error
	if len(carriers) > 0 {
		if existing := block["mods"]; existing != nil {
			if err = json.Unmarshal(existing, &mods); err != nil {
				return nil, err
			}
		}
		keys := make([]string, 0, len(carriers))
		for k := range carriers {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, key := range keys {
			row, err := carriers[key].pin(root, resolve, ids)
			if err != nil {
				return nil, err
			}
			raw, err := json.Marshal(row)
			if err != nil {
				return nil, err
			}
			mods = append(mods, raw)
		}
	}
	for _, set := range sets {
		if err = set.check(); err != nil {
			return nil, err
		}
	}
	if len(sets) > 0 {
		block["nav"], _ = json.Marshal(sets)
	}
	if len(mods) > 0 {
		block["mods"], _ = json.Marshal(mods)
	}
	if len(settings) > 0 {
		block["settings"], _ = json.Marshal(settings)
	}
	// Trim trailing delivery slots. Keep a string at each earlier index an author's later variable depends on.
	for i, isOld := range old[:lastAuthor+1] {
		if !isOld {
			continue
		}
		var row, info earlierObject
		if err = json.Unmarshal(rows[i], &row); err != nil {
			return nil, err
		}
		if err = json.Unmarshal(row["info"], &info); err != nil {
			return nil, err
		}
		name := fmt.Sprintf("String %d", i)
		for used[strings.ToLower(name)] {
			name += " (restored)"
		}
		used[strings.ToLower(name)] = true
		info["name"], _ = json.Marshal(name)
		row["info"], _ = json.Marshal(info)
		row["initialValue"] = json.RawMessage(`""`)
		rows[i], _ = json.Marshal(row)
	}
	rows = rows[:lastAuthor+1]
	variables["string"], _ = json.Marshal(rows)
	if a := variables["allocCount"]; a != nil {
		var counts []int
		if err = json.Unmarshal(a, &counts); err != nil || len(counts) < 5 {
			return nil, fmt.Errorf("unreadable variable counts")
		}
		counts[4] = len(rows)
		variables["allocCount"], _ = json.Marshal(counts)
	}
	v, _ := json.Marshal(variables)
	out, err := earlierSetMember(src, "variables", v)
	if err != nil {
		return nil, err
	}
	root["snapmap-plus"], _ = json.Marshal(block)
	return earlierSetMember(out, "snapmap-plus", root["snapmap-plus"])
}
