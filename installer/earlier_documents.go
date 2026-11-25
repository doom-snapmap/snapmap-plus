package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
)

type earlierObject = map[string]json.RawMessage
type earlierRowResolver func(earlierObject) (earlierObject, error)

var earlierJSONString = regexp.MustCompile(`"(?:[^"\\]|\\.)*"`)

func earlierMapNamespaces(src []byte, ids map[string]string) []byte {
	if len(ids) == 0 {
		return src
	}
	view := earlierJSONText(src)
	var out bytes.Buffer
	last := 0
	for at := 0; at < len(view); {
		span := earlierJSONString.FindIndex(view[at:])
		if span == nil {
			break
		}
		span[0] += at
		span[1] += at
		at = span[1]
		raw := src[span[0]:span[1]]
		if !bytes.Contains(bytes.ToLower(raw), []byte("mods")) {
			continue
		}
		var name string
		if json.Unmarshal(raw, &name) != nil {
			continue
		}
		before := name
		for id, salt := range ids {
			prefix := "mods/" + strings.ToLower(id) + "/"
			for pos := 0; pos < len(name); {
				hit := strings.Index(strings.ToLower(name[pos:]), prefix)
				if hit < 0 {
					break
				}
				start := pos + hit
				if start == 0 || strings.ContainsRune("/=\"' \t\r\n", rune(name[start-1])) {
					name = name[:start] + "mods/" + salt + "/" + name[start+len(prefix):]
					pos = start + len(salt) + 6
				} else {
					pos = start + len(prefix)
				}
			}
		}
		if name == before {
			continue
		}
		out.Write(src[last:span[0]])
		replacement, _ := json.Marshal(name)
		out.Write(replacement)
		last = span[1]
	}
	if last == 0 {
		return src
	}
	out.Write(src[last:])
	return out.Bytes()
}

// The engine serializes some maps with comments. Spaces keep the JSON decoder's byte offsets valid.
func earlierJSONText(src []byte) []byte {
	if !bytes.Contains(src, []byte("//")) && !bytes.Contains(src, []byte("/*")) {
		return src
	}
	out := bytes.Clone(src)
	quoted := false
	for i := 0; i < len(out); i++ {
		if quoted {
			if out[i] == '\\' {
				i++
			} else if out[i] == '"' {
				quoted = false
			}
			continue
		}
		if out[i] == '"' {
			quoted = true
			continue
		}
		if out[i] != '/' || i+1 >= len(out) {
			continue
		}
		last := i
		switch out[i+1] {
		case '/':
			for last < len(out) && out[last] != '\n' && out[last] != '\r' {
				last++
			}
		case '*':
			end := bytes.Index(out[i+2:], []byte("*/"))
			if end < 0 {
				return src
			}
			last = i + end + 4
		default:
			continue
		}
		for j := i; j < last; j++ {
			out[j] = ' '
		}
		i = last - 1
	}
	return out
}

func earlierSetting(key, value string) (string, error) {
	bad := func() (string, error) { return "", fmt.Errorf("unrecognized %s setting %q", key, value) }
	parts := strings.Split(value, ";")
	var offset int
	switch {
	case key == "bounds" && len(parts) == 2 && parts[0] == "1":
		offset = 1
	case key == "render" && len(parts) == 2 && parts[0] == "4":
		offset = 1
	case key == "render" && len(parts) == 8 && (parts[0] == "1" || parts[0] == "3"):
		offset = 1
	case key == "render" && len(parts) == 9 && parts[0] == "2" && (parts[1] == "0" || parts[1] == "1"):
		offset = 2
	default:
		return bad()
	}
	var distance float64
	for i := offset; i < len(parts); i++ {
		f, err := strconv.ParseFloat(strings.TrimSpace(parts[i]), 32)
		if err != nil || math.IsInf(f, 0) || math.IsNaN(f) {
			return bad()
		}
		if i == offset {
			distance = f
		}
	}
	if key == "bounds" {
		if distance < 0 {
			return bad()
		}
		if math.Floor(distance) > 16000 {
			return "1;32671", nil
		}
		return "", nil
	}
	if distance != 0 && (distance < 256 || distance > 200000) {
		return bad()
	}
	if distance == 8192 || (offset == 2 && parts[1] == "0") {
		return "", nil
	}
	return "4;0", nil
}

// Replace one root member, preserving the bytes and order of the engine's own fields.
func earlierSetMember(text []byte, name string, value json.RawMessage) ([]byte, error) {
	start := len(text) - len(bytes.TrimPrefix(text, []byte{0xef, 0xbb, 0xbf}))
	d := json.NewDecoder(bytes.NewReader(earlierJSONText(text[start:])))
	if token, err := d.Token(); err != nil || token != json.Delim('{') {
		return nil, fmt.Errorf("expected an object")
	}
	pairs, begin, end := 0, -1, -1
	for d.More() {
		k, err := d.Token()
		if err != nil {
			return nil, err
		}
		var raw json.RawMessage
		if err = d.Decode(&raw); err != nil {
			return nil, err
		}
		if k == name {
			if begin >= 0 {
				return nil, fmt.Errorf("duplicate member %q", name)
			}
			end = start + int(d.InputOffset())
			begin = end - len(raw)
		}
		pairs++
	}
	if _, err := d.Token(); err != nil {
		return nil, err
	}
	var replacement []byte
	if begin >= 0 {
		replacement = value
	} else {
		begin = start + int(d.InputOffset()) - 1
		end = begin
		if pairs > 0 {
			replacement = append(replacement, ',')
		}
		quoted, _ := json.Marshal(name)
		replacement = append(replacement, quoted...)
		replacement = append(replacement, ':')
		replacement = append(replacement, value...)
	}
	out := make([]byte, 0, len(text)+len(replacement))
	out = append(out, text[:begin]...)
	out = append(out, replacement...)
	return append(out, text[end:]...), nil
}

func earlierPrefabBlock(side earlierObject) (earlierObject, error) {
	block := earlierObject{"format": json.RawMessage("1"), "description": json.RawMessage(`""`),
		"tags": json.RawMessage("[]"), "mods": json.RawMessage("[]")}
	summary := earlierObject{}
	if raw, ok := side["summary"]; ok {
		if err := json.Unmarshal(raw, &summary); err != nil || summary == nil {
			return nil, fmt.Errorf("unreadable prefab summary")
		}
	}
	for key, raw := range side {
		switch key {
		case "format", "summary":
		case "desc", "description":
			if prior, ok := side["description"]; key == "desc" && ok && !bytes.Equal(prior, raw) {
				return nil, fmt.Errorf("prefab descriptions disagree")
			}
			block["description"] = raw
		case "tags", "mods":
			block[key] = raw
		default:
			if prior, ok := summary[key]; ok && !bytes.Equal(prior, raw) {
				return nil, fmt.Errorf("prefab summary already has a different %q", key)
			}
			summary[key] = raw
		}
	}
	if len(summary) > 0 {
		raw, err := json.Marshal(summary)
		if err != nil {
			return nil, err
		}
		block["summary"] = raw
	}
	return block, nil
}

func earlierRows(block earlierObject, resolve earlierRowResolver) (bool, error) {
	data := block["mods"]
	if data == nil {
		return false, nil
	}
	var rows []earlierObject
	if err := json.Unmarshal(data, &rows); err != nil {
		return false, err
	}
	changed := false
	for i, row := range rows {
		var mod string
		_ = json.Unmarshal(row["mod"], &mod)
		salt, hash, old := strings.Cut(mod, "@")
		preSalt := row["id"] != nil || row["bundle"] != nil
		if !old && !preSalt {
			continue
		}
		if preSalt || !modSalt.MatchString(salt) || hash != strings.Repeat("0", 64) {
			if resolve == nil {
				return false, fmt.Errorf("the original mod files are needed to keep this map's pinned content")
			}
			next, err := resolve(row)
			if err != nil {
				return false, err
			}
			rows[i] = next
			changed = true
			continue
		}
		row["mod"], _ = json.Marshal(salt)
		row["local"] = json.RawMessage("true")
		if row["names"] == nil {
			row["names"] = json.RawMessage("[]")
		}
		if row["size"] == nil {
			row["size"] = json.RawMessage("0")
		}
		changed = true
	}
	if changed {
		block["mods"], _ = json.Marshal(rows)
	}
	return changed, nil
}

func earlierMapBlock(raw json.RawMessage, resolve earlierRowResolver) (json.RawMessage, bool, error) {
	var block earlierObject
	if err := json.Unmarshal(raw, &block); err != nil || block == nil {
		return nil, false, fmt.Errorf("unreadable Snapmap+ map member")
	}
	var format int
	if err := json.Unmarshal(block["format"], &format); err != nil || (format != 1 && format != 2) {
		return nil, false, nil
	}
	oldRows, err := earlierRows(block, resolve)
	if err != nil {
		return nil, false, err
	}
	changed := format == 1 || oldRows
	if changed {
		block["format"] = json.RawMessage("2")
	}
	if settings, ok := block["settings"]; ok {
		var values earlierObject
		if err := json.Unmarshal(settings, &values); err != nil || values == nil {
			return nil, false, fmt.Errorf("unreadable Snapmap+ map settings")
		}
		var settingsChanged bool
		for _, key := range []string{"render", "bounds"} {
			entry, ok := values[key]
			if !ok {
				continue
			}
			var old string
			if err := json.Unmarshal(entry, &old); err != nil {
				return nil, false, err
			}
			next, err := earlierSetting(key, old)
			if err != nil {
				return nil, false, err
			}
			if next == old {
				continue
			}
			if next == "" {
				delete(values, key)
			} else {
				values[key], _ = json.Marshal(next)
			}
			settingsChanged = true
		}
		if settingsChanged {
			if len(values) == 0 {
				delete(block, "settings")
			} else {
				block["settings"], _ = json.Marshal(values)
			}
			changed = true
		}
	}
	if !changed {
		return nil, false, nil
	}
	out, err := json.Marshal(block)
	return out, true, err
}
