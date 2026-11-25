package main

import (
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

var earlierParticleProgram = regexp.MustCompile(`(?im)^([ \t]*stageprogram[ \t]+"?)snapmapplus/eternal/(uberparticles)([ \t\r]*$|"|[ \t]+//)`)
var earlierParticleKey = regexp.MustCompile(`(?im)^[ \t]*eternalparticle(?:fade|color2?|emissivemap)?[ \t]+`)

func earlierVector(text string) ([4]float64, error) {
	var out [4]float64
	fields := strings.FieldsFunc(text, func(r rune) bool { return strings.ContainsRune("{}, \t", r) })
	if len(fields) < 1 || len(fields) > 4 {
		return out, fmt.Errorf("expected a vector of up to four numbers")
	}
	for i, s := range fields {
		n, err := strconv.ParseFloat(s, 32)
		if err != nil || math.IsInf(n, 0) || math.IsNaN(n) {
			return out, fmt.Errorf("invalid component %q", s)
		}
		out[i] = n
	}
	if len(fields) == 1 && !strings.HasPrefix(text, "{") {
		out = [4]float64{out[0], out[0], out[0], out[0]}
	}
	return out, nil
}

// Preserve the native material and translate only the shader parameters that changed.
func upgradeParticleMaterial(original []byte) ([]byte, error) {
	text := earlierLookKey.ReplaceAllString(string(original), "${1}type${2}")
	text = earlierParticleProgram.ReplaceAllString(text, "${1}snapmapplus/particles/${2}${3}")
	if !earlierParticleKey.MatchString(text) {
		return []byte(text), nil
	}
	eol := "\n"
	if strings.Contains(text, "\r\n") {
		eol = "\r\n"
	}
	lines := strings.Split(text, eol)
	ones := [4]float64{1, 1, 1, 1}
	vectors := map[string][4]float64{"eternalparticle": {}, "eternalparticlefade": {},
		"eternalparticlecolor": ones, "eternalparticlecolor2": ones, "emissive": ones}
	drop := map[int]bool{}
	first := -1
	for i, line := range lines {
		code, _, _ := strings.Cut(line, "//")
		fields := strings.Fields(code)
		if len(fields) < 2 {
			continue
		}
		key := strings.ToLower(fields[0])
		if key == "eternalparticleemissivemap" {
			at := strings.Index(strings.ToLower(line), key)
			lines[i] = line[:at] + "particleemissivemap" + line[at+len(key):]
			if first < 0 {
				first = i
			}
			continue
		}
		if _, ok := vectors[key]; ok {
			v, err := earlierVector(strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(code), fields[0])))
			if err != nil {
				return nil, fmt.Errorf("line %d, %s: %w", i+1, key, err)
			}
			vectors[key] = v
			if strings.HasPrefix(key, "eternalparticle") && first < 0 {
				first = i
			}
			drop[i] = true
		}
		switch key {
		case "particleuberparms1", "particleuberparms2", "particleuberparms3", "smokebrightness":
			drop[i] = true
		}
	}
	if first < 0 {
		return []byte(text), nil
	}
	mode, fade := vectors["eternalparticle"], vectors["eternalparticlefade"]
	col, col2 := vectors["eternalparticlecolor"], vectors["eternalparticlecolor2"]
	m := mode[0]
	if fade[3] != 0 {
		m += 16
	}
	parms := []struct {
		key string
		v   [4]float64
	}{
		{"particleuberparms1", [4]float64{0, 0, m, mode[3]}},
		{"particleuberparms2", [4]float64{col[0], col[1], col[2], fade[0]}},
		{"particleuberparms3", [4]float64{col2[0], col2[1], col2[2], mode[1]}},
		{"emissive", [4]float64{vectors["emissive"][0], fade[1], fade[2], col2[3]}},
		{"smokebrightness", [4]float64{mode[2], 0, 0, 0}},
	}
	var out []string
	for i, line := range lines {
		if i == first {
			for _, parm := range parms {
				var parts [4]string
				for j, f := range parm.v {
					parts[j] = strconv.FormatFloat(f, 'g', -1, 32)
				}
				out = append(out, parm.key+"\t{ "+strings.Join(parts[:], ", ")+" }")
			}
		}
		if !drop[i] {
			out = append(out, line)
		}
	}
	return []byte(strings.Join(out, eol)), nil
}

func upgradeGraphicsFolders(data string) error {
	root := filepath.Join(data, "mods")
	if !plainPath(root) {
		return fmt.Errorf("invalid mod directory %s", root)
	}
	if _, err := os.Stat(root); errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	return filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !entry.IsDir() || path == root {
			return nil
		}
		if !plainPath(path) || strings.HasPrefix(entry.Name(), ".") || modWorkFolder.MatchString(entry.Name()) {
			return filepath.SkipDir
		}
		if !isRegular(filepath.Join(path, "mod.json")) {
			return nil
		}
		if err := upgradeGraphicsFolder(data, path); err != nil {
			return err
		}
		return filepath.SkipDir
	})
}

func upgradeGraphicsFolder(data, folder string) error {
	dir := filepath.Join(folder, "materials")
	if !plainPath(dir) {
		return fmt.Errorf("invalid material folder %s", dir)
	}
	if _, err := os.Stat(dir); errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	return filepath.WalkDir(dir, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(path) {
			return fmt.Errorf("linked material path %s", path)
		}
		if entry.IsDir() || !strings.EqualFold(filepath.Ext(path), ".decl") {
			return nil
		}
		b, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		out, err := upgradeParticleMaterial(b)
		if err != nil {
			return fmt.Errorf("%s: %w", path, err)
		}
		if bytes.Equal(b, out) {
			return nil
		}
		if err := keepEarlierFile(data, path, b); err != nil {
			return err
		}
		// A crash before the new source is installed must leave no ready manifest for stale bytes.
		side := filepath.Join(folder, modBuildManifest)
		if !plainPath(side) {
			return fmt.Errorf("invalid manifest path %s", side)
		}
		if _, err := os.Stat(side); err == nil {
			backup, err := os.CreateTemp(filepath.Dir(side), "before-graphics-*.json")
			if err != nil {
				return err
			}
			if err := backup.Close(); err != nil {
				return err
			}
			if err := os.Rename(side, backup.Name()); err != nil {
				return err
			}
		} else if !errors.Is(err, fs.ErrNotExist) {
			return err
		}
		now, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if !bytes.Equal(now, b) {
			return fmt.Errorf("%s changed during conversion", path)
		}
		return writeAtomicFile(path, out)
	})
}
