package main

import (
	"bufio"
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
	ui.Section("Checking earlier content")
	progress := startMigrationProgress()
	defer progress.stop()
	held := map[string]bool{}
	for {
		p, cleanup, err := planPlayerMigration(doom, data, explicit, held, progress)
		if err != nil {
			return p, cleanup, err
		}
		retry := false
		for folder := range p.needed {
			if p.converting[folder] && !held[folder] {
				held[folder] = true
				retry = true
			}
		}
		if !retry {
			p.reportDeferred()
			if !p.ready() && len(p.Deferred) == 0 {
				ui.OK("No earlier content needs migration.")
			}
			return p, cleanup, nil
		}
		cleanup()
	}
}

func planPlayerMigration(doom, data, explicit string, held map[string]bool, progress *migrationProgress) (*migrationPlan, func(), error) {
	if data == "" || !plainPath(data) {
		return nil, func() {}, fmt.Errorf("unavailable Snapmap+ data directory")
	}
	p := &migrationPlan{progress: progress, data: data, destinations: map[string]int{}, held: held, needed: map[string]bool{}, converting: map[string]bool{}}
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
	progress.step("Checking mod libraries and settings")
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
		progress.step("Checking selected export")
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
			progress.step("Checking maps, rawmaps and prefabs")
			err = p.documents(pins)
		}
		if err == nil && doom != "" {
			progress.step("Checking DOOM save slots")
			err = p.native(pins)
		}
	}
	if err == nil && explicit == "" {
		progress.step("Checking earlier mod builds")
		err = p.retireBuildHistory()
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

func showMigrationCount(label string, n int, names []string) {
	color := ansiGrey
	if n > 0 {
		color = ansiCyan
	}
	value := fmt.Sprint(n)
	if len(names) > 0 {
		value += "\n" + strings.Join(names, "\n")
	}
	ui.field(label, value, color)
}

var migrationInput = openMigrationInput

func (p *migrationPlan) consent(yes bool) bool {
	ui.Section("Migration summary")
	if samePath(p.work, migrationJournal(p.data)) {
		ui.Note("Resume the interrupted migration.")
	} else {
		preview := p.modPreview()
		showMigrationCount("Mods", len(preview.Names), preview.Names)
		if preview.Settings {
			ui.field("Settings", "Update config.json", ansiCyan)
		}
		if preview.Shared {
			ui.field("Shared mod data", "Update", ansiCyan)
		}
		showMigrationCount("Maps and rawmaps", p.Maps, nil)
		showMigrationCount("Prefabs", p.Prefabs, nil)
	}
	if len(p.VanillaMaps) > 0 {
		ui.field("Make vanilla", strings.Join(p.VanillaMaps, "\n"), ansiYellow)
		ui.Note("Earlier mod files are missing. These maps will lose their mod content and keep their stock content.")
	}
	if yes {
		ui.Note("Back up your data before migrating.")
		return true
	}
	input := migrationInput()
	if input == nil {
		return false
	}
	defer input.Close()
	ui.Ask("Back up your data before migrating. Continue?")
	line, err := bufio.NewReader(input).ReadString('\n')
	ui.Answered(err != nil)
	return err == nil && strings.EqualFold(cleanLine(line), "y")
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
	p.progress = startMigrationProgress()
	defer p.progress.stop()
	p.progress.step("Preparing migration")
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
		var missing *missingEarlierPins
		var prototype unavailablePrototype
		if errors.As(err, &missing) {
			return fmt.Errorf("restore this export's original mod files and try again")
		}
		if errors.As(err, &prototype) {
			return fmt.Errorf("this export uses an unrecognized earlier mod record")
		}
		return err
	}
	if !plan.ready() {
		return nil
	}
	if !plan.consent(yes) {
		ui.Skip("Migration cancelled.")
		return nil
	}
	return plan.commit()
}
