package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strings"
)

var earlierID = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)
var earlierBadKey = regexp.MustCompile(`[^a-zA-Z0-9_]`)
var earlierCalls = regexp.MustCompile(`[.:]?[a-zA-Z_][a-zA-Z0-9_]*[ \t\r\n]*\(`)
var earlierAnimationCall = regexp.MustCompile(`(^|[^a-zA-Z0-9_.])on\.anim([ \t\r\n]*\()`)
var earlierGlobals = map[string]string{"fx": "play_effect", "fx_move": "move_effect", "fx_stop": "stop_effect", "fx_hard_stops": "end_effects_at_once", "cvar": "console_variable", "anim_valid": "animation_fits", "anim_force_via": "force_animation_via", "anim_current": "current_animation", "anim_event": "animation_event", "anim_finished": "animation_finished", "anim_force": "force_animation", "anim_state": "animation_state", "nav_ground": "ground_below", "set_pos": "set_position", "event_args": "event_arguments"}
var earlierMethods = map[string]string{"pos": "position", "def": "definition", "event": "run_event", "post": "queue_event"}

func earlierLua(text string) string {
	out := earlierCalls.ReplaceAllStringFunc(text, func(call string) string {
		if call[0] == '.' {
			return call
		}
		prefix := ""
		if call[0] == ':' {
			prefix = ":"
			call = call[1:]
		}
		end := strings.IndexAny(call, " \t\r\n(")
		name := call[:end]
		next := earlierGlobals[name]
		if prefix == ":" && earlierMethods[name] != "" {
			next = earlierMethods[name]
		}
		if next == "" {
			return prefix + call
		}
		return prefix + next + call[end:]
	})
	return earlierAnimationCall.ReplaceAllString(out, "${1}on.animation${2}")
}

func earlierDescriptor(src string) (map[string]any, string, error) {
	for _, name := range []string{"mod.json", "package.json", "bundle.json"} {
		file := filepath.Join(src, name)
		if !plainPath(file) {
			return nil, "", fmt.Errorf("linked file %s", file)
		}
		b, err := os.ReadFile(file)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return nil, "", err
		}
		var doc map[string]any
		r := json.NewDecoder(bytes.NewReader(bytes.TrimPrefix(b, []byte{0xef, 0xbb, 0xbf})))
		r.UseNumber()
		if err = r.Decode(&doc); err != nil || doc == nil {
			return nil, "", fmt.Errorf("unreadable %s", file)
		}
		return doc, name, nil
	}
	return nil, "", nil
}

func earlierModIdentity(doc map[string]any) (string, string, error) {
	id, _ := doc["id"].(string)
	block, _ := doc["snapmap-plus"].(map[string]any)
	salt, _ := block["salt"].(string)
	if salt != "" {
		if !modSalt.MatchString(salt) {
			return "", "", fmt.Errorf("invalid namespace %q", salt)
		}
		return salt, salt, nil
	}
	if !earlierID.MatchString(id) {
		return "", "", fmt.Errorf("the earlier mod's original id is missing")
	}
	return strings.ToLower(id), earlierSalt(id), nil
}

func earlierStringTables(doc map[string]any, n *earlierModNames) error {
	langs, _ := doc["strings"].(map[string]any)
	for _, raw := range langs {
		table, ok := raw.(map[string]any)
		if !ok {
			continue
		}
		keys := make([]string, 0, len(table))
		for key := range table {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for _, key := range keys {
			next := strings.TrimPrefix(key, n.id+"_")
			if !earlierKey.MatchString(next) {
				next = earlierBadKey.ReplaceAllString(strings.TrimSpace(next), "_")
			}
			if next == "" {
				return fmt.Errorf("empty earlier string key")
			}
			n.keys[key] = next
			if key == next {
				continue
			}
			if existing, ok := table[next]; ok {
				if !reflect.DeepEqual(existing, table[key]) {
					return fmt.Errorf("string keys %q and %q would coincide", key, next)
				}
				delete(table, key)
				continue
			}
			table[next] = table[key]
			delete(table, key)
		}
	}
	return nil
}

func earlierEvent(s string) uint32 {
	h := fnv.New32()
	_, _ = h.Write([]byte(strings.ToLower("play_" + s)))
	return h.Sum32()
}

func earlierBankEvents(b []byte, ids map[uint32]uint32) error {
	for offset := 0; offset < len(b); {
		if len(b)-offset < 8 {
			return fmt.Errorf("unreadable sound bank header")
		}
		length := int(binary.LittleEndian.Uint32(b[offset+4:]))
		if length > len(b)-offset-8 {
			return fmt.Errorf("truncated sound bank")
		}
		end := offset + 8 + length
		if string(b[offset:offset+4]) == "HIRC" {
			if length < 4 {
				return fmt.Errorf("unreadable HIRC section")
			}
			n := binary.LittleEndian.Uint32(b[offset+8:])
			pos := offset + 12
			for j := uint32(0); j < n; j++ {
				if end-pos < 5 {
					return fmt.Errorf("truncated HIRC entry")
				}
				size := int(binary.LittleEndian.Uint32(b[pos+1:]))
				if size > end-pos-5 {
					return fmt.Errorf("truncated HIRC object")
				}
				if b[pos] == 4 && size >= 4 {
					original := binary.LittleEndian.Uint32(b[pos+5:])
					if next, ok := ids[original]; ok {
						binary.LittleEndian.PutUint32(b[pos+5:], next)
					}
				}
				pos += 5 + size
			}
		}
		offset = end
	}
	return nil
}

func earlierBank(src []byte, n earlierModNames) ([]byte, error) {
	forward, back := map[uint32]uint32{}, map[uint32]uint32{}
	for _, s := range n.sounds {
		leaf := path.Base(s)
		a, b := earlierEvent(n.id+"_"+leaf), earlierEvent(leaf)
		forward[a] = b
		back[b] = a
	}
	if len(forward) == 0 {
		return src, nil
	}
	out := bytes.Clone(src)
	if err := earlierBankEvents(out, forward); err != nil {
		return nil, err
	}
	proof := bytes.Clone(out)
	if err := earlierBankEvents(proof, back); err != nil {
		return nil, err
	}
	if !bytes.Equal(proof, src) {
		return nil, fmt.Errorf("sound bank events collide in the new namespace")
	}
	return out, nil
}

func earlierModJSONValue(v any, n earlierModNames) any {
	switch a := v.(type) {
	case map[string]any:
		for k, v := range a {
			a[k] = earlierModJSONValue(v, n)
		}
	case []any:
		for i, v := range a {
			a[i] = earlierModJSONValue(v, n)
		}
	case string:
		return n.authorText(a)
	}
	return v
}

// Translate a private copy of an earlier folder or the verification source for a map's pinned build.
func translateEarlierMod(src, dst string) (string, error) {
	if !plainPath(src) || !plainPath(dst) {
		return "", fmt.Errorf("linked mod folder")
	}
	if _, err := os.Lstat(dst); !os.IsNotExist(err) {
		return "", fmt.Errorf("conversion destination already exists: %s", dst)
	}
	doc, descriptor, err := earlierDescriptor(src)
	if err != nil {
		return "", err
	}
	if descriptor == "package.json" && doc["id"] == nil {
		doc["id"] = filepath.Base(src)
	}
	id, salt, err := earlierModIdentity(doc)
	if err != nil {
		return "", err
	}
	n := earlierModNames{id: id, salt: salt, files: map[string]string{}, sounds: map[string]string{}, keys: map[string]string{}}
	var packages map[string]string
	if descriptor == "package.json" {
		packages, err = earlierPackageFiles(src, doc)
		if err != nil {
			return "", err
		}
		if len(packages) == 0 {
			return "", fmt.Errorf("the package contains no usable assets")
		}
	}
	if err = earlierStringTables(doc, &n); err != nil {
		return "", err
	}
	type asset struct{ src, author string }
	var items []asset
	assets := filepath.Join(src, "assets")
	used := map[string]bool{}
	addFile := func(file, rel string) error {
		author, err := earlierAuthorPath(id, rel)
		if err != nil {
			return err
		}
		if !filepath.IsLocal(filepath.FromSlash(author)) || used[author] {
			return fmt.Errorf("conflicting asset %s", rel)
		}
		used[author] = true
		n.add(author)
		items = append(items, asset{file, author})
		return nil
	}
	if descriptor == "package.json" {
		keys := make([]string, 0, len(packages))
		for rel := range packages {
			keys = append(keys, rel)
		}
		sort.Strings(keys)
		for _, rel := range keys {
			if err = addFile(packages[rel], rel); err != nil {
				return "", err
			}
		}
	} else {
		err = filepath.WalkDir(assets, func(file string, e fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if !plainPath(file) {
				return fmt.Errorf("linked mod asset %s", file)
			}
			if e.IsDir() {
				return nil
			}
			if !e.Type().IsRegular() {
				return fmt.Errorf("unreadable asset %s", file)
			}
			rel, err := filepath.Rel(assets, file)
			if err != nil {
				return err
			}
			return addFile(file, filepath.ToSlash(rel))
		})
	}
	if err != nil {
		return "", err
	}
	if err = os.Mkdir(dst, 0755); err != nil {
		return "", err
	}
	for _, item := range items {
		b, err := os.ReadFile(item.src)
		if err != nil {
			return "", err
		}
		switch {
		case strings.HasSuffix(item.author, ".decl"):
			text := n.authorText(string(b))
			b = []byte(text)
			if strings.HasPrefix(item.author, "materials/") {
				b, err = upgradeParticleMaterial(b)
				if err != nil {
					return "", fmt.Errorf("%s: %w", item.author, err)
				}
			}
		case strings.HasSuffix(item.author, ".lua"):
			b = []byte(earlierLua(strings.ReplaceAll(string(b), "mods/"+id+"/", "mods/"+salt+"/")))
		case strings.HasSuffix(item.author, ".bnk"):
			b, err = earlierBank(b, n)
			if err != nil {
				return "", fmt.Errorf("%s: %w", item.author, err)
			}
		case id != salt && bytes.Contains(b, []byte("mods/"+id+"/")):
			return "", fmt.Errorf("%s embeds its earlier namespace inside binary data", item.author)
		}
		filename := filepath.Join(dst, filepath.FromSlash(item.author))
		if err = os.MkdirAll(filepath.Dir(filename), 0755); err != nil {
			return "", err
		}
		if err = os.WriteFile(filename, b, 0644); err != nil {
			return "", err
		}
	}
	if descriptor == "bundle.json" {
		for _, k := range []string{"files", "bundle", "build"} {
			delete(doc, k)
		}
	}
	for _, k := range []string{"id", "scope", "stock", "format", "snapmap-plus"} {
		delete(doc, k)
	}
	for k, v := range doc {
		if k != "strings" {
			doc[k] = earlierModJSONValue(v, n)
		}
	}
	if events, ok := doc["clip_events"]; ok {
		if _, conflict := doc["animation_events"]; conflict {
			return "", fmt.Errorf("the mod defines both clip_events and animation_events")
		}
		doc["animation_events"] = events
		delete(doc, "clip_events")
	}
	doc["snapmap-plus"] = map[string]any{"format": 2, "salt": salt}
	data, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return "", err
	}
	if err = os.WriteFile(filepath.Join(dst, "mod.json"), append(data, '\n'), 0644); err != nil {
		return "", err
	}
	return salt, nil
}
