package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"reflect"
	"strings"
)

func earlierAddSetting(dst map[string]any, key string, val any) error {
	if old, ok := dst[key]; ok && !reflect.DeepEqual(old, val) {
		return fmt.Errorf("conflicting values for %s", key)
	}
	dst[key] = val
	return nil
}

func earlierSection(doc map[string]any, keys ...string) (map[string]any, error) {
	for _, key := range keys {
		child, ok := doc[key].(map[string]any)
		if !ok {
			if doc[key] != nil {
				return nil, fmt.Errorf("unreadable %s", key)
			}
			child = map[string]any{}
			doc[key] = child
		}
		doc = child
	}
	return doc, nil
}

func earlierPackageFiles(src string, doc map[string]any, stock *earlierResourceRoot) (map[string]string, error) {
	files := map[string]string{}
	listed := map[string]bool{}
	resources := map[string]earlierResource{}
	component := map[string]bool{}
	var paths []string
	err := filepath.WalkDir(src, func(filename string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(filename) {
			return fmt.Errorf("linked package file %s", filename)
		}
		if entry.IsDir() {
			return nil
		}
		if !entry.Type().IsRegular() {
			return fmt.Errorf("unreadable package file %s", filename)
		}
		rel, err := filepath.Rel(src, filename)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if strings.EqualFold(path.Base(rel), "package.json") {
			component[path.Dir(rel)] = true
		}
		paths = append(paths, rel)
		return nil
	})
	if err != nil {
		return nil, err
	}
	wrapped, err := earlierWrappedFiles(src, paths, component, stock)
	if err != nil {
		return nil, err
	}
	for pass := 0; pass < 2; pass++ {
		for _, rel := range paths {
			dir := path.Dir(rel)
			for dir != "." && !component[dir] {
				dir = path.Dir(dir)
			}
			inner := rel
			if dir != "." {
				inner = strings.TrimPrefix(rel, dir+"/")
			}
			filename := filepath.Join(src, filepath.FromSlash(rel))
			low := strings.ToLower(inner)
			folder, leaf, _ := strings.Cut(low, "/")
			if pass == 0 {
				switch {
				case folder == "resources" && strings.HasSuffix(leaf, ".manifest") && !strings.Contains(leaf, "/"):
					text, err := os.ReadFile(filename)
					if err != nil {
						return nil, err
					}
					for _, line := range strings.Split(string(text), "\n") {
						parts := strings.Split(strings.TrimSuffix(line, "\r"), "\t")
						if len(parts) < 2 || strings.HasPrefix(parts[0], "#") {
							continue
						}
						provider := parts[1]
						if len(parts) > 2 && parts[2] != "" {
							provider = parts[2]
						}
						if !filepath.IsLocal(filepath.FromSlash(provider)) {
							return nil, fmt.Errorf("invalid manifest file %s", provider)
						}
						key := path.Join(dir, provider)
						listed[key] = true
						if len(parts) == 3 {
							item := earlierResource{parts[0], parts[1], provider, parts[2] == ""}
							if prior, found := resources[key]; found && item != prior {
								return nil, fmt.Errorf("conflicting package resources for %s", provider)
							}
							resources[key] = item
						}
					}
				case folder == "requirements" && strings.HasSuffix(leaf, ".requirements") && !strings.Contains(leaf, "/"):
					text, err := os.ReadFile(filename)
					if err != nil {
						return nil, err
					}
					for _, line := range strings.Split(string(text), "\n") {
						parts := strings.Split(strings.TrimSuffix(line, "\r"), "\t")
						if len(parts) != 3 || parts[0] != "cvar" || (parts[1] != "g_useResourceBlackList" && parts[1] != "g_useImageBlackList") || parts[2] != "0" {
							continue
						}
						target, err := earlierSection(doc, "requirements", "cvars")
						if err != nil {
							return nil, err
						}
						if err = earlierAddSetting(target, parts[1], "0"); err != nil {
							return nil, err
						}
					}
				case folder == "strings" && strings.HasSuffix(leaf, ".json") && !strings.Contains(leaf, "/"), low == "hud/weapons.json", low == "package.json" && dir != ".":
					text, err := os.ReadFile(filename)
					if err != nil {
						return nil, err
					}
					var policy map[string]any
					if err = json.Unmarshal(bytes.TrimPrefix(text, []byte{0xef, 0xbb, 0xbf}), &policy); err != nil {
						return nil, err
					}
					var keys []string
					switch {
					case folder == "strings":
						keys = []string{"strings", "en"}
					case folder == "hud":
						keys = []string{"hud", "weapons"}
						policy, _ = policy["weapons"].(map[string]any)
					default:
						keys = []string{"requirements", "cvars"}
						req, _ := policy["requirements"].(map[string]any)
						policy, _ = req["cvars"].(map[string]any)
					}
					if len(policy) == 0 {
						continue
					}
					target, err := earlierSection(doc, keys...)
					if err != nil {
						return nil, err
					}
					for key, val := range policy {
						if err = earlierAddSetting(target, key, val); err != nil {
							return nil, err
						}
					}
				}
				continue
			}
			req := ""
			for _, prefix := range []struct{ old, next string }{{"assets/", ""}, {"decls/", "generated/decls/"}, {"images/", "generated/image/"}, {"shaders/generated/spirv/", "generated/spirv/"}, {"shaders/generated/renderprogs/", "generated/renderprogs/"}} {
				if strings.HasPrefix(low, prefix.old) {
					if prefix.old != "assets/" && !listed[rel] && !looseGameRequest(prefix.next+inner[len(prefix.old):]) {
						continue
					}
					req = prefix.next + inner[len(prefix.old):]
					break
				}
			}
			if replacement, ok := wrapped[rel]; ok {
				req = replacement
			}
			// Earlier model packages also carried material listings for their authors.
			if !listed[rel] && strings.HasPrefix(low, "assets/cooked/model/") && strings.HasSuffix(low, ".materials.txt") {
				continue
			}
			if req == "" && listed[rel] {
				req = inner
			}
			if req == "" {
				continue
			}
			key := strings.ToLower(req)
			if prev := files[key]; prev != "" && !identicalFiles(prev, filename) {
				return nil, fmt.Errorf("different package files provide %s", key)
			}
			files[key] = filename
		}
	}
	missing := map[string]earlierResource{}
	providers := map[string]earlierResource{}
	for file := range listed {
		if isRegular(filepath.Join(src, filepath.FromSlash(file))) {
			continue
		}
		item, ok := resources[file]
		if !ok || !item.valid() {
			return nil, fmt.Errorf("package manifest refers to missing file %s", file)
		}
		key := strings.ToLower(item.request)
		if prior, found := providers[key]; found && prior != item {
			return nil, fmt.Errorf("conflicting installed-game resources for %s", item.request)
		}
		providers[key] = item
		missing[item.request] = item
	}
	if len(missing) > 4096 {
		return nil, fmt.Errorf("package exceeds the earlier manifest resource limit")
	}
	if len(missing) == 0 {
		return files, nil
	}
	// Placement discovery needs only the names. Translation resolves the exact retail rows.
	if stock == nil || stock.namesOnly {
		for name := range missing {
			if files[strings.ToLower(name)] == "" {
				files[strings.ToLower(name)] = name
			}
		}
		return files, nil
	}
	if err := stock.read(missing, files); err != nil {
		return nil, err
	}
	return files, nil
}
