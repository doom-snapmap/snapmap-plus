package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"unicode"
)

type migrationMod struct {
	folder string
	name   string
}

type migrationModPreview struct {
	Names            []string
	Settings, Shared bool
}

// Count a destination author folder once, regardless of how many source files moved.
func (p *migrationPlan) noteModChanges(dir string, start int) {
	if start == len(p.Changes) {
		return
	}
	folder, err := filepath.Rel(filepath.Join(p.data, "mods"), dir)
	if err != nil || !filepath.IsLocal(folder) || folder == "." {
		return
	}
	file := filepath.Join(dir, "mod.json")
	if i, ok := p.destinations[strings.ToLower(file)]; ok && p.Changes[i].After != "" {
		file = filepath.Join(p.work, fmt.Sprintf("new-%d", i))
	}
	var doc struct{ Name string }
	if raw, err := os.ReadFile(file); err == nil {
		_ = json.Unmarshal(bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf}), &doc)
	}
	folder = filepath.ToSlash(folder)
	if p.modFolders == nil {
		p.modFolders = make(map[string]migrationMod)
	}
	name := modPreviewLabel(doc.Name)
	if name == "" {
		name = modFolderLabel(filepath.Base(folder))
	}
	p.modFolders[strings.ToLower(folder)] = migrationMod{folder: folder, name: name}
}

func modPreviewLabel(s string) string {
	return strings.TrimSpace(strings.Map(func(r rune) rune {
		if !unicode.IsPrint(r) {
			return ' '
		}
		return r
	}, s))
}

// Preserve distinct folder spellings even if their names contain invisible characters.
func modFolderLabel(folder string) string {
	s := strconv.QuoteToGraphic(folder)
	return s[1 : len(s)-1]
}

func (p *migrationPlan) modPreview() migrationModPreview {
	var result migrationModPreview
	names := make(map[string]int, len(p.modFolders))
	for _, mod := range p.modFolders {
		names[strings.ToLower(mod.name)]++
	}
	for _, mod := range p.modFolders {
		label := mod.name
		if names[strings.ToLower(label)] > 1 {
			label += " (" + modFolderLabel(mod.folder) + ")"
		}
		result.Names = append(result.Names, label)
	}
	slices.Sort(result.Names)
	config := filepath.Join(p.data, "config.json")
	shared := []string{filepath.Join(p.data, "cache")}
	for _, root := range p.Roots {
		for _, old := range []string{".builds", ".content", ".served"} {
			shared = append(shared, filepath.Join(root, "mods", old))
		}
	}
	for _, change := range p.Changes {
		if change.Stage != "Mods" {
			continue
		}
		if samePath(change.Path, config) {
			result.Settings = true
		} else if !result.Shared {
			result.Shared = slices.ContainsFunc(shared, func(root string) bool { return under(root, change.Path) })
		}
		if result.Settings && result.Shared {
			break
		}
	}
	return result
}
