package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

// Use the file name that the original file shadow would have opened for this request.
func earlierShadowPath(request string) string {
	first := strings.Index(request, ".inc")
	if first < 0 || first+4 != len(request) {
		return request
	}
	if !strings.HasPrefix(request, "includes") {
		if _, tail, found := strings.Cut(request, "/"); found {
			request = tail
		}
	}
	return "shader_includes/" + request
}

// Keep only requested loose files, including all retail requests that shared an include.
func earlierLooseCatalog(doom string, paths []string) (map[string][]string, error) {
	selected := make(map[string][]string)
	for _, name := range paths {
		if pinRelative(name) {
			selected[strings.ToLower(name)] = nil
		}
	}
	if len(selected) == 0 {
		return nil, nil
	}
	if doom == "" {
		var err error
		if doom, err = resolveDoom(""); err != nil {
			return nil, nil
		}
	}
	for _, name := range []string{"gameresources.pindex", "snap_gameresources.pindex"} {
		file := filepath.Join(doom, "base", name)
		if !plainPath(file) {
			return nil, fmt.Errorf("cannot check loose overrides through linked index %s", file)
		}
		err := findEarlierLooseRequests(file, selected)
		if errors.Is(err, os.ErrNotExist) {
			// A missing index could hide a second request using the same loose file.
			return nil, nil
		}
		if err != nil {
			return nil, fmt.Errorf("could not check loose overrides against %s: %w", file, err)
		}
	}
	found := make(map[string][]string)
	for key, requests := range selected {
		slices.Sort(requests)
		selected[key] = slices.Compact(requests)
	}
	for _, name := range paths {
		requests := selected[strings.ToLower(name)]
		if len(requests) > 0 {
			found[name] = requests
		}
	}
	return found, nil
}

func findEarlierLooseRequests(file string, selected map[string][]string) error {
	return visitEarlierResourceFile(file, func(kind, name, provider []byte, row earlierResourceSlice) error {
		if len(kind) == 0 || len(name) == 0 || row.stored == 0 || row.size == 0 {
			return nil
		}
		request := string(provider)
		key := strings.ToLower(earlierShadowPath(request))
		if previous, wanted := selected[key]; wanted && pinRelative(request) {
			if row.archive > 1 {
				return fmt.Errorf("unknown archive selector for %s", request)
			}
			selected[key] = append(previous, strings.ToLower(request))
		}
		return nil
	})
}
