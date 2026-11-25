package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

// Some releases kept this canonical bundle only implicitly, in mod.json's author fields and build block.
func preSaltCanonical(doc earlierObject, block earlierObject) ([]byte, error) {
	value := func(raw json.RawMessage) (any, error) {
		dec := json.NewDecoder(bytes.NewReader(raw))
		dec.UseNumber()
		var v any
		err := dec.Decode(&v)
		return v, err
	}
	source := map[string]any{"format": 1, "id": strings.ToLower(pinString(doc, "id")), "version": pinString(doc, "version")}
	if source["version"] == "" {
		source["version"] = "0.0.0"
	}
	var build uint64
	if err := json.Unmarshal(block["build"], &build); err != nil || build == 0 {
		return nil, fmt.Errorf("invalid earlier build number")
	}
	source["build"] = build
	extra := map[string]any{}
	for k, v := range doc {
		switch k {
		case "format", "id", "version", "snapmap-plus", "stock", "shadows":
			continue
		case "author", "name", "homepage":
			if text := strings.TrimSpace(pinString(doc, k)); text != "" {
				source[k] = text
			}
		case "requirements":
			var req struct {
				Cvars    earlierObject
				Commands map[string][]string
			}
			if err := json.Unmarshal(v, &req); err != nil {
				return nil, err
			}
			cvars := map[string]string{}
			for name, raw := range req.Cvars {
				v, e := value(raw)
				if e != nil {
					return nil, e
				}
				switch x := v.(type) {
				case string:
					cvars[name] = x
				case bool:
					if x {
						cvars[name] = "1"
					} else {
						cvars[name] = "0"
					}
				case json.Number:
					cvars[name] = x.String()
					if cvars[name] == "-0" {
						cvars[name] = "0"
					}
				default:
					return nil, fmt.Errorf("invalid earlier console variable")
				}
			}
			normalized := map[string]any{"cvars": cvars}
			commands := map[string][]string{}
			for _, action := range []string{"activate", "deactivate"} {
				for _, cmd := range req.Commands[action] {
					commands[action] = append(commands[action], strings.TrimSpace(cmd))
				}
			}
			if len(commands) > 0 {
				normalized["commands"] = commands
			}
			source[k] = normalized
		default:
			x, e := value(v)
			if e != nil {
				return nil, e
			}
			extra[k] = x
		}
	}
	if legacy := block["legacy"]; legacy != nil && extra["legacy"] == nil {
		x, e := value(legacy)
		if e != nil {
			return nil, e
		}
		extra["legacy"] = x
	}
	if len(extra) > 0 {
		source["extra"] = extra
	}
	var shadows []string
	if raw := block["shadows"]; raw != nil {
		if e := json.Unmarshal(raw, &shadows); e != nil {
			return nil, e
		}
	}
	if len(shadows) > 0 {
		source["shadows"] = shadows
	}
	files := []map[string]any{}
	for _, kind := range []string{"files", "stock"} {
		var list []earlierObject
		if raw := block[kind]; raw != nil {
			if e := json.Unmarshal(raw, &list); e != nil {
				return nil, e
			}
		}
		for _, f := range list {
			file := pinString(f, "path")
			if !strings.HasPrefix(file, "assets/") {
				return nil, fmt.Errorf("invalid earlier mod path")
			}
			needs, e := value(f["needs"])
			if e != nil {
				return nil, e
			}
			provides, e := value(f["provides"])
			if e != nil {
				return nil, e
			}
			row := map[string]any{"path": file, "request": file[7:], "needs": needs, "provides": provides}
			if kind == "files" {
				size, e := value(f["size"])
				if e != nil {
					return nil, e
				}
				row["blob"], row["size"] = pinString(f, "sha256"), size
			} else {
				row["stock"] = map[string]string{"sha256": pinString(f, "sha256"), "source": pinString(f, "source")}
			}
			files = append(files, row)
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i]["path"].(string) < files[j]["path"].(string) })
	source["files"] = files
	var out bytes.Buffer
	enc := json.NewEncoder(&out)
	enc.SetEscapeHTML(false)
	if e := enc.Encode(source); e != nil {
		return nil, e
	}
	canonical := bytes.TrimSuffix(out.Bytes(), []byte{'\n'})
	if fmt.Sprintf("%x", sha256.Sum256(canonical)) != pinString(block, "bundle") {
		return nil, fmt.Errorf("earlier mod.json no longer matches its recorded build")
	}
	return canonical, nil
}
