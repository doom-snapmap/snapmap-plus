package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

var editorDeclTokens = regexp.MustCompile(`(?s)/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|[{}=;]|[^\s{}=;"]+`)
var editorNativeID = regexp.MustCompile(`^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$`)

// Read direct edit scalars only. The original declaration remains the tile's native template.
func legacyTileFields(raw []byte) map[string]string {
	var t []string
	for _, token := range editorDeclTokens.FindAllString(string(raw), -1) {
		if !strings.HasPrefix(token, "//") && !strings.HasPrefix(token, "/*") {
			t = append(t, token)
		}
	}
	fields := map[string]string{}
	depth, edit := 0, false
	for i, token := range t {
		switch token {
		case "{":
			if depth == 1 {
				edit = i >= 2 && t[i-1] == "=" && t[i-2] == "edit"
			}
			depth++
		case "}":
			depth--
			if depth == 1 {
				edit = false
			}
		default:
			if !edit || depth != 2 || i+3 >= len(t) || t[i+1] != "=" || t[i+3] != ";" {
				continue
			}
			v := t[i+2]
			if len(v) > 2 && v[0] == '"' && v[len(v)-1] == '"' {
				fields[token] = v[1 : len(v)-1]
			}
		}
	}
	return fields
}

func campaignTabOwner(salt string) bool {
	for _, id := range []string{"campaign-interactables", "campaign-interactables-2", "campaign-interactables-doors",
		"campaign-interactables-panels", "campaign-interactables-progression", "campaign-interactables-weapons"} {
		if salt == earlierSalt(id) {
			return true
		}
	}
	return false
}

func editorTiles(dir string, put func(string, []byte) error, remove func(string) error) error {
	if err := editorNativeDecls(dir, put, remove); err != nil {
		return err
	}
	var doc struct {
		Build modSourceIdentity `json:"snapmap-plus"`
	}
	raw, err := os.ReadFile(filepath.Join(dir, "mod.json"))
	if err != nil {
		return err
	}
	if err = json.Unmarshal(bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf}), &doc); err != nil {
		return err
	}
	shared := campaignTabOwner(doc.Build.Salt)
	tiles := map[string]map[string]string{}
	root := filepath.Join(dir, "decls", "snapeditorentitydef")
	out := map[string]any{"tiles": tiles}
	err = walkLibrary(root, func(path string, entry fs.DirEntry, _ error) error {
		if entry.IsDir() || !strings.EqualFold(filepath.Ext(path), ".decl") {
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		name := filepath.ToSlash(strings.TrimSuffix(rel, filepath.Ext(rel)))
		raw, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		fields := legacyTileFields(raw)
		entity := fields["entityDef"]
		if !strings.HasPrefix(entity, "entities/") || !editorNativeID.MatchString(entity) ||
			!editorNativeID.MatchString(name) || strings.Contains(name, "..") {
			return nil
		}
		template := "editor/templates/" + name + ".decl"
		tile := map[string]string{"entity": entity, "template": template}
		for native, key := range map[string]string{"displayNameTag": "title", "displayDescriptionTag": "description"} {
			if tag, ok := strings.CutPrefix(fields[native], "#str_"); ok && editorNativeID.MatchString(tag) {
				tile[key] = tag
			}
		}
		if icon := fields["previewImage"]; editorNativeID.MatchString(icon) {
			tile["icon"] = icon
		}
		if shared && fields["categoryNameTag"] == "#str_ci_tab" {
			const id = "shared/campaign-interactables"
			tile["tab"] = id
			out["tabs"] = map[string]any{id: map[string]string{"title": "ci_tab"}}
		}
		tiles[name] = tile
		if err = put(filepath.Join(dir, filepath.FromSlash(template)), raw); err != nil {
			return err
		}
		return remove(path)
	})
	if err != nil || len(tiles) == 0 {
		return err
	}
	data, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return err
	}
	return put(filepath.Join(dir, "editor", "imported-tiles.json"), append(data, '\n'))
}

func editorNativeDecls(dir string, put func(string, []byte) error, remove func(string) error) error {
	native := filepath.Join(dir, "native")
	decls := filepath.Join(native, "generated", "decls")
	if !plainPath(decls) {
		return fmt.Errorf("linked native editor source %s", decls)
	}
	kinds, err := os.ReadDir(decls)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, kind := range kinds {
		name := strings.ToLower(kind.Name())
		if !kind.IsDir() || !(strings.HasPrefix(name, "snapeditor") || strings.HasPrefix(name, "snappropertyinspector") || strings.HasPrefix(name, "snappalette")) {
			continue
		}
		err := walkLibrary(filepath.Join(decls, kind.Name()), func(path string, e fs.DirEntry, _ error) error {
			if e.IsDir() {
				if strings.EqualFold(e.Name(), "mods") {
					return filepath.SkipDir
				}
				return nil
			}
			if !strings.EqualFold(filepath.Ext(path), ".decl") {
				return nil
			}
			rel, err := filepath.Rel(native, path)
			if err != nil {
				return err
			}
			raw, err := os.ReadFile(path)
			if err != nil {
				return err
			}
			if err = put(filepath.Join(dir, "editor", "native", rel), raw); err != nil {
				return err
			}
			return remove(path)
		})
		if err != nil {
			return err
		}
	}
	return nil
}

// This folder is private installer staging. The journal handles all writes to the player folder.
func stageEditorTiles(dir string) error {
	return editorTiles(dir, func(path string, body []byte) error {
		if !plainPath(path) {
			return fmt.Errorf("linked editor file %s", path)
		}
		current, err := os.ReadFile(path)
		if err == nil {
			if bytes.Equal(current, body) {
				return nil
			}
			return fmt.Errorf("different editor content at %s", path)
		}
		if !os.IsNotExist(err) {
			return err
		}
		if err = os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			return err
		}
		return os.WriteFile(path, body, 0644)
	}, os.Remove)
}
