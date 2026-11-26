package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

var earlierNavName = regexp.MustCompile(`^smnav1\.([A-Za-z0-9_./-]+)/?\.([A-Za-z0-9_-]+)\.([0-9]{1,7})\.([0-9]{1,7})\.([a-f0-9]{16})\.([a-f0-9]{16})$`)
var earlierStampName = regexp.MustCompile(`^smpprod1\.v?[0-9]+(?:\.[0-9]+){1,2}(?:-[A-Za-z0-9.-]+)?$`)
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

type earlierVariable struct {
	Info struct {
		Name string
		Type string `json:"~type"`
	}
	InitialValue string
}

// Byte ranges refer to the native text even if the map uses comments.
func earlierVariableSpans(array []byte) ([][2]int, error) {
	d := json.NewDecoder(bytes.NewReader(earlierJSONText(array)))
	if token, err := d.Token(); err != nil || token != json.Delim('[') {
		return nil, fmt.Errorf("expected a string variable array")
	}
	var spans [][2]int
	for d.More() {
		var raw json.RawMessage
		if err := d.Decode(&raw); err != nil {
			return nil, err
		}
		end := int(d.InputOffset())
		spans = append(spans, [2]int{end - len(raw), end})
	}
	_, err := d.Token()
	return spans, err
}

func earlierOpaqueVariableSpans(view []byte) [][2]int {
	begin, end, _, err := earlierMemberSpan(view, "variables")
	if err != nil || begin == end {
		return nil
	}
	a, b, _, err := earlierMemberSpan(view[begin:end], "string")
	if err != nil || a == b {
		return nil
	}
	base := begin + a
	rows := view[base : begin+b]
	spans, err := earlierVariableSpans(rows)
	if err != nil {
		return nil
	}
	opaque := spans[:0]
	for _, s := range spans {
		var row struct{ Info struct{ Name string } }
		if json.Unmarshal(rows[s[0]:s[1]], &row) == nil &&
			(strings.HasPrefix(row.Info.Name, "smpbind.") || strings.HasPrefix(row.Info.Name, "smpref.")) {
			opaque = append(opaque, [2]int{base + s[0], base + s[1]})
		}
	}
	return opaque
}

func earlierVariables(src []byte, root earlierObject, resolve earlierCarriedResolver, ids map[string]string, check earlierRowCheck) ([]byte, error) {
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
	manifestTotal := 0
	manifestIndices := map[int]bool{}
	for i, raw := range rows {
		var row earlierVariable
		if err := json.Unmarshal(raw, &row); err != nil || row.Info.Type != "snapVarInfo_t" {
			lastAuthor = i
			continue
		}
		name := row.Info.Name
		stamp := earlierStampName.MatchString(name) && row.InitialValue == ""
		if !historicalVariable(name, row.InitialValue) && !stamp {
			used[strings.ToLower(name)] = true
			lastAuthor = i
			continue
		}
		switch {
		case name == "smp.render.v1" || name == "smp.bounds.v1":
			key := strings.Split(name, ".")[1]
			if block["settings"] == nil {
				if prior, ok := settings[key]; ok && prior != row.InitialValue {
					return nil, fmt.Errorf("conflicting earlier %s settings", key)
				}
				settings[key] = row.InitialValue
			}
		case stamp:
			// A producer version alone does not make a map modded.
		case strings.HasPrefix(name, "smpmod1."):
			m := earlierRowName.FindStringSubmatch(name)
			if m == nil {
				return nil, fmt.Errorf("invalid earlier mod row %s", name)
			}
			idx, _ := strconv.Atoi(m[1])
			total, _ := strconv.Atoi(m[2])
			if idx >= total || manifestIndices[idx] || manifestTotal != 0 && total != manifestTotal {
				return nil, fmt.Errorf("invalid earlier mod row %s", name)
			}
			manifestTotal = total
			manifestIndices[idx] = true
			if block["mods"] == nil {
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
		}
		old[i] = true
		if !stamp {
			found = true
		}
	}
	if !found {
		return src, nil
	}
	if manifestTotal != len(manifestIndices) {
		return nil, fmt.Errorf("earlier mod manifest is missing %d of %d rows", manifestTotal-len(manifestIndices), manifestTotal)
	}
	var err error
	if len(carriers) > 0 {
		if existing := block["mods"]; existing != nil {
			if err = json.Unmarshal(existing, &mods); err != nil {
				return nil, err
			}
		}
		// Verify pinned dependencies before extracting any embedded package from this map.
		if check != nil && len(mods) > 0 {
			var pinned []earlierObject
			for _, raw := range mods {
				var row earlierObject
				if err = json.Unmarshal(raw, &row); err != nil {
					return nil, err
				}
				pinned = append(pinned, row)
			}
			if err = check(pinned); err != nil {
				return nil, err
			}
		}
		if resolve == nil {
			return nil, fmt.Errorf("the installer must verify the mod carried by this map")
		}
		deliveryRows, err := resolve(root, carriers, ids)
		if err != nil {
			return nil, err
		}
		mods = append(mods, deliveryRows...)
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
	begin, end, _, err := earlierMemberSpan(src, "variables")
	if err != nil {
		return nil, err
	}
	v := src[begin:end]
	begin, end, _, err = earlierMemberSpan(v, "string")
	if err != nil {
		return nil, err
	}
	// Keep untouched variable rows byte-for-byte, including native comments.
	source := v[begin:end]
	spans, err := earlierVariableSpans(source)
	if err != nil || len(spans) != len(rows) {
		return nil, fmt.Errorf("unreadable string variable array")
	}
	var array bytes.Buffer
	pos := 0
	for i, span := range spans[:lastAuthor+1] {
		if old[i] {
			array.Write(source[pos:span[0]])
			array.Write(rows[i])
		} else {
			array.Write(source[pos:span[1]])
		}
		pos = span[1]
	}
	if lastAuthor+1 == len(rows) {
		array.Write(source[pos:])
	} else if lastAuthor < 0 {
		array.WriteString("[]")
	} else {
		array.WriteByte(']')
	}
	v, err = earlierSetMember(v, "string", array.Bytes())
	if err != nil {
		return nil, err
	}
	if a := variables["allocCount"]; a != nil {
		var counts []int
		if err = json.Unmarshal(a, &counts); err != nil || len(counts) < 5 {
			return nil, fmt.Errorf("unreadable variable counts")
		}
		counts[4] = lastAuthor + 1
		raw, _ := json.Marshal(counts)
		v, err = earlierSetMember(v, "allocCount", raw)
		if err != nil {
			return nil, err
		}
	}
	out, err := earlierSetMember(src, "variables", v)
	if err != nil {
		return nil, err
	}
	root["snapmap-plus"], _ = json.Marshal(block)
	return earlierSetMember(out, "snapmap-plus", root["snapmap-plus"])
}
