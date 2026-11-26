package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

func preparePlayerMigration(doom, data, explicit string) (*migrationPlan, func(), error) {
	if data == "" || !plainPath(data) {
		return nil, func() {}, fmt.Errorf("unavailable Snapmap+ data directory")
	}
	p := &migrationPlan{data: data, destinations: map[string]int{}}
	for _, home := range p.legacyHomes() {
		p.Roots = append(p.Roots, home)
	}
	if explicit != "" {
		abs, err := filepath.Abs(explicit)
		if err != nil {
			return nil, func() {}, err
		}
		explicit = abs
		p.Export = explicit
	}
	pending := migrationJournal(data)
	if !plainPath(pending) {
		return nil, func() {}, fmt.Errorf("linked migration journal %s", pending)
	}
	if earlierPathExists(pending) && !emptyMigrationJournal(pending) {
		file := filepath.Join(pending, "plan.json")
		if !plainPath(file) {
			return nil, func() {}, fmt.Errorf("linked migration plan %s", file)
		}
		body, err := os.ReadFile(file)
		if err != nil {
			return nil, func() {}, fmt.Errorf("could not read the previous migration: %w", err)
		}
		var existing migrationPlan
		if json.Unmarshal(body, &existing) != nil || !slices.Equal(existing.Roots, p.Roots) || existing.Export != p.Export {
			return nil, func() {}, fmt.Errorf("another migration is pending; run its original command to finish")
		}
		existing.data, existing.work = data, pending
		existing.journalHash = journalHash(body)
		return &existing, func() {}, nil
	}
	tmp, err := os.MkdirTemp("", "snapmap-migration-*")
	if err != nil {
		return nil, func() {}, err
	}
	cleanup := func() { os.RemoveAll(tmp) }
	p.work = tmp
	pins := newEarlierPins(data, doom, nil)
	p.pins = pins
	if explicit == "" {
		pins.keepMissing = p.keepMissingPins
	}
	pins.placements, err = p.readPlacements()
	if err != nil {
		cleanup()
		return nil, func() {}, err
	}
	defer pins.close()
	pins.output = filepath.Join(tmp, "pins")
	for _, home := range p.legacyHomes() {
		if !samePath(home, data) {
			for _, sub := range []string{"mods", "overrides"} {
				pins.libraries = append(pins.libraries, filepath.Join(home, sub))
			}
		}
	}
	if explicit != "" {
		if !plainPath(explicit) || !isRegular(explicit) {
			err = fmt.Errorf("cannot read export %s", explicit)
		} else if err = p.previewPlacementOwners(explicit); err == nil {
			switch strings.ToLower(filepath.Ext(explicit)) {
			case ".zip":
				err = p.document(explicit, explicit, "", true, pins)
			case ".json":
				if strings.HasSuffix(strings.ToLower(explicit), ".meta.json") {
					err = fmt.Errorf("choose the prefab file beside %s", explicit)
				} else {
					err = p.document(explicit, explicit, "", false, pins)
				}
			default:
				err = fmt.Errorf("choose a Snapmap+ JSON or ZIP file")
			}
		}
	} else {
		if err = p.preferences(); err == nil {
			err = p.mods()
		}
		if err == nil {
			err = p.pins.placements.save()
		}
		if err == nil {
			err = p.documents(pins)
		}
		if err == nil && doom != "" {
			err = p.native(pins)
		}
	}
	if err == nil && earlierPathExists(pins.output) {
		err = p.transferTree(pins.output, data, "Mods", false)
	}
	if err != nil {
		cleanup()
		return nil, func() {}, err
	}
	return p, cleanup, nil
}

func (p *migrationPlan) preferences() error {
	file := filepath.Join(p.data, "config.json")
	if !plainPath(file) {
		p.keepOverrides = true
		ui.Warn("Skipped linked config.json and its loose overrides.")
		return nil
	}
	f, err := os.Open(file)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	var body []byte
	if err == nil {
		defer f.Close()
		body, err = io.ReadAll(io.LimitReader(f, maxConfigBytes+1))
	}
	if err != nil {
		p.keepOverrides = true
		ui.Warn("Could not read config.json. Kept it and its loose overrides: %v.", err)
		return nil
	}
	hasLoose := false
	for _, home := range p.legacyHomes() {
		if earlierPathExists(filepath.Join(home, "overrides")) || earlierPathExists(filepath.Join(home, "strings", "strids.json")) {
			hasLoose = true
			break
		}
	}
	next, keepLoose, err := migratePlayerConfig(body, hasLoose)
	p.keepOverrides = keepLoose
	if err != nil {
		p.keepOverrides = true
		ui.Warn("Kept config.json and its loose overrides: %v.", err)
		return nil
	}
	if keepLoose {
		ui.Note("Kept loose overrides in place with their existing preferences.")
	}
	if !bytes.Equal(body, next) {
		return p.putBytes(file, next, "Mods", true)
	}
	return nil
}

func (p *migrationPlan) ready() bool {
	return len(p.Changes) > 0 || samePath(p.work, migrationJournal(p.data))
}

func (p *migrationPlan) consent(yes bool) (bool, error) {
	counts := p.counts()
	ui.Section("Update earlier Snapmap+ content")
	if samePath(p.work, migrationJournal(p.data)) {
		ui.Note("An earlier update was interrupted. Continue to finish the verified changes.")
	}
	for _, stage := range []string{"Mods", "Maps", "Prefabs"} {
		label := "file changes"
		if counts[stage] == 1 {
			label = "file change"
		}
		ui.Field(stage, fmt.Sprintf("%d %s", counts[stage], label))
	}
	ui.Field("Snapmap+ data", p.data)
	if p.Export != "" {
		ui.Field("Selected export", p.Export)
		ui.Note("To keep the earlier version, make a manual copy of the selected export and any matching prefab metadata file.")
	} else {
		ui.Note("For a backup of your mods, maps, rawmaps, prefabs and settings, copy the listed data folders before continuing.")
		for _, root := range p.Roots[1:] {
			if earlierPathExists(root) {
				ui.Field("Earlier data", root)
			}
		}
	}
	if yes {
		return true, nil
	}
	if !isInteractive() {
		return false, fmt.Errorf("earlier content needs your consent; run in a terminal or use --yes after reviewing the listed changes")
	}
	ui.Ask("Apply these changes?", "[y/N]")
	answer, open := readLineEOF()
	ui.Answered(!open)
	return open && (strings.EqualFold(answer, "yes") || strings.EqualFold(answer, "y")), nil
}

func (p *migrationPlan) commit() error {
	if !p.ready() {
		return nil
	}
	unlock, err := lockMigration(p.data)
	if err != nil {
		return err
	}
	defer unlock()
	if samePath(p.work, migrationJournal(p.data)) {
		hash, err := migrationFileHash(filepath.Join(p.work, "plan.json"))
		if err != nil || hash != p.journalHash {
			return fmt.Errorf("the pending migration changed before confirmation; run the command again")
		}
	} else if err := p.persist(); err != nil {
		return err
	}
	return p.apply()
}

func convertWithConsent(data, file string, yes bool) error {
	plan, done, err := preparePlayerMigration(earlierInstallDir(data), data, file)
	defer done()
	if err != nil {
		return err
	}
	if !plan.ready() {
		ui.OK("No earlier modded content needs updating.")
		return nil
	}
	accepted, err := plan.consent(yes)
	if err != nil {
		return err
	}
	if !accepted {
		ui.Skip("Migration cancelled.")
		return nil
	}
	return plan.commit()
}
