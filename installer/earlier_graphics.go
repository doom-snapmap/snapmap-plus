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
		n, err := strconv.ParseFloat(s, 64)
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

// Turn retired material parameters into the same author look as a current emit.
func upgradeEarlierMaterial(original []byte) ([]byte, error) {
	if !earlierLookKey.Match(original) && !earlierParticleProgram.Match(original) && !earlierParticleKey.Match(original) && !earlierLayerKey.Match(original) {
		return original, nil
	}
	return migrateEarlierLook(string(original))
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
		out, err := upgradeEarlierMaterial(b)
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
