package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

func looseGameRequest(rel string) bool {
	rel = strings.ToLower(filepath.ToSlash(rel))
	if strings.HasPrefix(rel, "generated/decls/") && strings.HasSuffix(rel, ".decl") && strings.Count(rel, "/") >= 3 {
		return true
	}
	if strings.HasPrefix(rel, "shader_includes/") && strings.HasSuffix(rel, ".inc") {
		return true
	}
	if strings.HasPrefix(rel, "sound/soundbanks/pc/") && strings.HasSuffix(rel, ".bnk") {
		return true
	}
	for _, a := range earlierAssetTypes {
		if strings.HasPrefix(rel, a.root) && strings.HasSuffix(rel, a.extension) {
			return true
		}
	}
	return false
}

// Beta 4 treated generated as a package. Later transitional layouts also used assets/.
func loosePackageRequest(rel string) string {
	lower := strings.ToLower(rel)
	if inner, ok := strings.CutPrefix(lower, "assets/"); ok && looseGameRequest(inner) && !strings.HasPrefix(inner, "shader_includes/") {
		return inner
	}
	for _, layout := range []struct{ old, current string }{
		{"generated/images/", "generated/image/"},
		{"generated/shaders/generated/spirv/", "generated/spirv/"},
		{"generated/shaders/generated/renderprogs/", "generated/renderprogs/"},
	} {
		if rest, ok := strings.CutPrefix(lower, layout.old); ok && looseGameRequest(layout.current+rest) {
			return layout.current + rest
		}
	}
	return ""
}

// Only typed manifests and requirements at the historical package root establish policy.
func loosePolicy(root, name string) (bool, error) {
	rel, err := filepath.Rel(root, name)
	if err != nil {
		return false, err
	}
	parts := strings.Split(filepath.ToSlash(rel), "/")
	if len(parts) != 3 || !strings.EqualFold(parts[0], "generated") {
		return false, nil
	}
	bank := strings.ToLower(parts[1])
	ext := strings.ToLower(filepath.Ext(parts[2]))
	if !(bank == "resources" && ext == ".manifest" || bank == "requirements" && ext == ".requirements") {
		return false, nil
	}
	data, err := os.ReadFile(name)
	if err != nil {
		return false, err
	}
	seen, bad := false, false
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSuffix(line, "\r")
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		v := strings.Split(line, "\t")
		good := false
		if len(v) == 3 {
			if bank == "resources" {
				provider := v[2]
				if provider == "" {
					provider = v[1]
				}
				good = (earlierResource{v[0], v[1], provider, v[2] == ""}).valid()
			} else {
				good = v[0] == "cvar" && (v[1] == "g_useResourceBlackList" || v[1] == "g_useImageBlackList") && v[2] == "0"
			}
		}
		if good {
			seen = true
		} else {
			bad = true
		}
	}
	if seen && bad {
		return false, fmt.Errorf("earlier policy contains unrecognized rows in %s", name)
	}
	return seen, nil
}

func (p *migrationPlan) loose(home string) error {
	start := len(p.Changes)
	root := filepath.Join(home, "overrides")
	var files, policies, candidates, wrapped []string
	var inactive []string
	requests := map[string][]string{}
	err := walkLibrary(root, func(path string, e fs.DirEntry, _ error) error {
		if e.IsDir() {
			if path != root {
				for _, name := range []string{"package.json", "mod.json", "bundle.json"} {
					if isRegular(filepath.Join(path, name)) {
						return filepath.SkipDir
					}
				}
			}
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if looseGameRequest(rel) && !strings.HasPrefix(strings.ToLower(rel), "shader_includes/") {
			requests[path] = []string{rel}
			files = append(files, path)
		} else if request := loosePackageRequest(rel); request != "" {
			requests[path] = []string{request}
			wrapped = append(wrapped, path)
		} else if policy, err := loosePolicy(root, path); err != nil {
			return err
		} else if policy {
			policies = append(policies, path)
		} else if pinRelative(rel) {
			candidates = append(candidates, rel)
		}
		return nil
	})
	if err != nil {
		return err
	}
	if len(candidates) > 0 {
		exact, err := earlierLooseCatalog(p.pins.doom, candidates)
		if err != nil {
			return err
		}
		for _, rel := range candidates {
			if names := exact[rel]; len(names) > 0 {
				file := filepath.Join(root, filepath.FromSlash(rel))
				requests[file] = names
				files = append(files, file)
			}
		}
	}
	// A direct file shadow had priority over its packaged copy.
	direct := map[string]bool{}
	for _, file := range files {
		for _, request := range requests[file] {
			direct[strings.ToLower(request)] = true
		}
	}
	for _, file := range wrapped {
		if direct[strings.ToLower(requests[file][0])] {
			inactive = append(inactive, file)
		} else {
			files = append(files, file)
		}
	}
	stringsFile := filepath.Join(home, "strings", "strids.json")
	var pairs map[string]string
	if plainPath(stringsFile) && isRegular(stringsFile) {
		raw, err := os.ReadFile(stringsFile)
		if err != nil {
			return err
		}
		if json.Unmarshal(bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf}), &pairs) != nil {
			pairs = nil
		}
	}
	if len(files) == 0 && len(policies) == 0 && len(pairs) == 0 {
		return nil
	}
	temp, err := os.MkdirTemp(p.work, "overrides-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(temp)
	input := filepath.Join(temp, "source")
	if err = os.MkdirAll(filepath.Join(input, "assets"), 0755); err != nil {
		return err
	}
	desc := map[string]any{"id": "my-overrides", "name": "My overrides"}
	if len(pairs) > 0 {
		desc["strings"] = map[string]any{"en": pairs}
	}
	encoded, err := json.Marshal(desc)
	if err != nil {
		return err
	}
	marker := "mod.json"
	if len(policies) > 0 {
		marker = "package.json"
	}
	if err = os.WriteFile(filepath.Join(input, marker), encoded, 0600); err != nil {
		return err
	}
	for _, file := range files {
		for _, request := range requests[file] {
			dst := filepath.Join(input, "assets", filepath.FromSlash(request))
			if earlierPathExists(dst) {
				if !identicalFiles(file, dst) {
					return fmt.Errorf("different loose overrides provide %s", request)
				}
				continue
			}
			if err = os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
				return err
			}
			if err = copyFile(file, dst); err != nil {
				return err
			}
		}
	}
	for _, file := range policies {
		rel, _ := filepath.Rel(filepath.Join(root, "generated"), file)
		dst := filepath.Join(input, rel)
		if err = os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
			return err
		}
		if err = copyFile(file, dst); err != nil {
			return err
		}
	}
	converted := filepath.Join(temp, "converted")
	target := filepath.Join(p.data, "mods", "my-overrides")
	if _, err = p.translateLocalAuthor(input, converted, target, nil, nil, nil); err != nil {
		return err
	}
	if err = stageEditorTiles(converted); err != nil {
		return err
	}
	for _, file := range append(inactive, policies...) {
		rel, _ := filepath.Rel(root, file)
		dst := filepath.Join(converted, rel)
		if err = os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
			return err
		}
		if err = copyFile(file, dst); err != nil {
			return err
		}
	}
	descriptor := filepath.Join(target, "mod.json")
	source, err := p.plannedAuthorFile(descriptor)
	if err != nil {
		return err
	}
	if !isRegular(source) {
		if entries, e := os.ReadDir(target); e == nil && len(entries) > 0 {
			return fmt.Errorf("an existing folder already uses %s", target)
		} else if e != nil && !os.IsNotExist(e) {
			return e
		}
	} else {
		old, err := os.ReadFile(source)
		if err != nil {
			return err
		}
		var original earlierObject
		if json.Unmarshal(bytes.TrimPrefix(old, []byte{0xef, 0xbb, 0xbf}), &original) != nil {
			return fmt.Errorf("could not read %s", descriptor)
		}
		block := earlierObject{}
		json.Unmarshal(original["snapmap-plus"], &block)
		if salt := pinString(block, "salt"); salt != earlierSalt("my-overrides") {
			return fmt.Errorf("another mod occupies %s", target)
		}
		if len(pairs) > 0 || len(policies) > 0 {
			var normalized earlierObject
			raw, err := os.ReadFile(filepath.Join(converted, "mod.json"))
			if err != nil {
				return err
			}
			if err = json.Unmarshal(raw, &normalized); err != nil {
				return err
			}
			newRaw := old
			if normalized["requirements"] != nil {
				var a, b map[string]any
				a = map[string]any{}
				if v := original["requirements"]; v != nil {
					if err = json.Unmarshal(v, &a); err != nil || a == nil {
						return fmt.Errorf("invalid requirements in %s", descriptor)
					}
				}
				if err = json.Unmarshal(normalized["requirements"], &b); err != nil {
					return err
				}
				dst, err := earlierSection(a, "cvars")
				if err != nil {
					return err
				}
				src, _ := b["cvars"].(map[string]any)
				for key, value := range src {
					if err = earlierAddSetting(dst, key, value); err != nil {
						return err
					}
				}
				newRaw, err = earlierSetMember(newRaw, "requirements", pinJSON(a))
				if err != nil {
					return err
				}
			}
			if len(pairs) > 0 {
				var languages map[string]map[string]string
				if raw := original["strings"]; raw != nil && json.Unmarshal(raw, &languages) != nil {
					return fmt.Errorf("invalid strings in %s", descriptor)
				}
				if languages == nil {
					languages = map[string]map[string]string{}
				}
				var additions map[string]map[string]string
				if err = json.Unmarshal(normalized["strings"], &additions); err != nil {
					return err
				}
				for lang, words := range additions {
					if languages[lang] == nil {
						languages[lang] = map[string]string{}
					}
					for id, val := range words {
						if current, ok := languages[lang][id]; ok && current != val {
							return fmt.Errorf("string %s is already different in %s", id, descriptor)
						}
						languages[lang][id] = val
					}
				}
				val, _ := json.Marshal(languages)
				newRaw, err = earlierSetMember(newRaw, "strings", val)
				if err != nil {
					return err
				}
			}
			if !bytes.Equal(newRaw, old) {
				if err = p.updateAuthorFile(descriptor, source, newRaw); err != nil {
					return err
				}
			}
		}
		if err = os.Remove(filepath.Join(converted, "mod.json")); err != nil {
			return err
		}
	}
	if err = p.transferTree(converted, target, "Mods", false); err != nil {
		return err
	}
	for _, file := range append(append(files, policies...), inactive...) {
		if err = p.remove(file, "Mods"); err != nil {
			return err
		}
		for d := filepath.Dir(file); under(root, d); d = filepath.Dir(d) {
			p.empty(d)
		}
	}
	p.empty(root)
	if len(pairs) > 0 {
		if err = p.remove(stringsFile, "Mods"); err != nil {
			return err
		}
		p.empty(filepath.Dir(stringsFile))
	}
	p.noteModChanges(target, start)
	p.editorAuthors = append(p.editorAuthors, target)
	return nil
}
