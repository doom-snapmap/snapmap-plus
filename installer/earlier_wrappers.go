package main

import (
	"fmt"
	"path"
	"path/filepath"
	"strings"
)

// Partially converted packages kept some early namespaces under assets/. The
// original converter preserved the first two components of an installed engine path.
func earlierWrappedFiles(src string, paths []string, components map[string]bool, stock *earlierResourceRoot) (map[string]string, error) {
	legacy := map[string]bool{}
	targets := map[string]string{}
	groups := map[string]string{}
	retail := map[string]bool{}
	for _, rel := range paths {
		dir := path.Dir(rel)
		for dir != "." && !components[dir] {
			dir = path.Dir(dir)
		}
		inner := strings.TrimPrefix(rel, dir+"/")
		parts := strings.SplitN(strings.ToLower(inner), "/", 4)
		if len(parts) < 3 || parts[0] != "assets" || !(parts[1] == "decls" || parts[1] == "images" || parts[1] == "shaders") {
			continue
		}
		if _, seen := legacy[dir]; !seen {
			doc, marker, err := earlierDescriptor(filepath.Join(src, filepath.FromSlash(dir)))
			if err != nil {
				return nil, err
			}
			legacy[dir] = marker == "package.json" && (doc["id"] == nil || doc["schema"] == "snapmap-plus.override-package.v1")
		}
		if !legacy[dir] {
			continue
		}
		relative := inner[len("assets/"):]
		lower := strings.ToLower(relative)
		var target string
		switch {
		case parts[1] == "decls":
			if len(parts) == 4 && strings.HasSuffix(lower, ".decl") {
				target = "generated/" + relative
			}
		case parts[1] == "images":
			target = "generated/image/" + relative[len("images/"):]
		case strings.HasPrefix(lower, "shaders/generated/spirv/"), strings.HasPrefix(lower, "shaders/generated/renderprogs/"):
			target = relative[len("shaders/"):]
		default:
			continue
		}
		targets[rel] = target
		group := parts[1] + "/" + parts[2]
		groups[rel] = group
		retail[group] = false
	}
	if len(targets) == 0 {
		return nil, nil
	}
	doom := ""
	if stock != nil {
		doom = stock.doom
	}
	if doom == "" {
		var err error
		if doom, err = resolveDoom(""); err != nil {
			return nil, fmt.Errorf("locate the game to identify partially converted package paths: %w", err)
		}
	}
	for _, name := range []string{"gameresources.pindex", "snap_gameresources.pindex"} {
		err := visitEarlierResourceFile(filepath.Join(doom, "base", name), func(_, _, provider []byte, _ earlierResourceSlice) error {
			parts := strings.SplitN(strings.ToLower(string(provider)), "/", 3)
			if len(parts) >= 2 {
				group := parts[0] + "/" + parts[1]
				if _, needed := retail[group]; needed {
					retail[group] = true
				}
			}
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	for rel, group := range groups {
		if retail[group] {
			delete(targets, rel)
		}
	}
	return targets, nil
}
