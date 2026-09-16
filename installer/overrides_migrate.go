package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type overrideUnit struct {
	sources     []string
	destination string
	synthetic   bool
}
type overrideMigration struct {
	Converted, Unchanged int
	Rejected             []string
	Backups              []string
}
type overrideJournal struct {
	Sources     []string
	Destination string
}

func findOverrideUnits(root string) ([]overrideUnit, error) {
	var units []overrideUnit
	var visit func(string) error
	visit = func(dir string) error {
		if !plainPath(dir) {
			return rejectOverride("linked override directory: %s", dir)
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			return err
		}
		if dir != root {
			rel, _ := filepath.Rel(root, dir)
			rel = filepath.ToSlash(rel)
			for _, e := range entries {
				if strings.EqualFold(e.Name(), "package.json") {
					units = append(units, overrideUnit{[]string{rel}, rel, false})
					return nil
				}
			}
			for _, e := range entries {
				if oldOverrideRoot(e.Name()) {
					units = append(units, overrideUnit{[]string{rel}, rel, true})
					return nil
				}
			}
		}
		var loose []string
		for _, e := range entries {
			if oldOverrideRoot(e.Name()) && dir == root {
				loose = append(loose, e.Name())
				continue
			}
			if e.IsDir() {
				if err := visit(filepath.Join(dir, e.Name())); err != nil {
					return err
				}
			}
		}
		if len(loose) > 0 {
			target := "legacy-overrides"
			for i := 1; ; i++ {
				_, err := os.Lstat(filepath.Join(root, target))
				if os.IsNotExist(err) {
					break
				}
				if err != nil {
					return err
				}
				target = fmt.Sprintf("legacy-overrides-%d", i)
			}
			units = append(units, overrideUnit{loose, target, true})
		}
		return nil
	}
	if err := visit(root); err != nil {
		return nil, err
	}
	return units, nil
}

func writeOverrideJSON(path string, value any) error {
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, err = f.Write(append(b, '\n'))
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	return closeErr
}

// Recovery never guesses which user tree to overwrite. The journal is durable
// before the first rename; a present stage means publication never completed.
// A published stage is a complete package, and its original remains a backup.
func recoverOverrideMigration(root, transaction string) error {
	if !plainPath(transaction) {
		return fmt.Errorf("linked migration transaction: %s", transaction)
	}
	if _, err := os.Lstat(filepath.Join(transaction, "complete.json")); err == nil {
		return nil
	} else if !os.IsNotExist(err) {
		return err
	}
	b, err := os.ReadFile(filepath.Join(transaction, "pending.json"))
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	var j overrideJournal
	if err := json.Unmarshal(b, &j); err != nil {
		return err
	}
	if !overrideRel(j.Destination) || len(j.Sources) == 0 {
		return fmt.Errorf("invalid override recovery journal: %s", transaction)
	}
	for _, s := range j.Sources {
		if !overrideRel(s) {
			return fmt.Errorf("unsafe override recovery source: %s", s)
		}
	}
	stage := filepath.Join(transaction, "converted")
	dest := filepath.Join(root, filepath.FromSlash(j.Destination))
	if !plainPath(dest) || !plainPath(stage) {
		return fmt.Errorf("linked migration publication path")
	}
	if _, err := os.Lstat(stage); os.IsNotExist(err) {
		if info, err := os.Lstat(dest); err != nil || !info.IsDir() {
			return fmt.Errorf("published migration is missing: %s", dest)
		}
		for _, s := range j.Sources {
			if _, err := os.Lstat(filepath.Join(transaction, "original", filepath.FromSlash(s))); err != nil {
				return fmt.Errorf("migration backup missing: %s: %w", s, err)
			}
		}
		return writeOverrideJSON(filepath.Join(transaction, "complete.json"), j)
	} else if err != nil {
		return err
	}
	for i := len(j.Sources) - 1; i >= 0; i-- {
		s := j.Sources[i]
		backup := filepath.Join(transaction, "original", filepath.FromSlash(s))
		source := filepath.Join(root, filepath.FromSlash(s))
		if !plainPath(backup) || !plainPath(source) {
			return fmt.Errorf("linked override recovery path")
		}
		if _, err := os.Lstat(backup); os.IsNotExist(err) {
			continue
		} else if err != nil {
			return err
		}
		if _, err := os.Lstat(source); !os.IsNotExist(err) {
			return fmt.Errorf("recovery preserves both conflicting trees: %s and %s", source, backup)
		}
		if err := os.MkdirAll(filepath.Dir(source), 0755); err != nil {
			return err
		}
		if err := os.Rename(backup, source); err != nil {
			return err
		}
	}
	return writeOverrideJSON(filepath.Join(transaction, "complete.json"), map[string]string{"result": "rolled back before publication"})
}

// Only an exclusively created scratch directory can be removed. Original
// backups and completed transactions are retained for the user.
func removeOverrideScratch(parent, path string) error {
	absParent, err := filepath.Abs(parent)
	if err != nil {
		return err
	}
	absPath, err := filepath.Abs(path)
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(absParent, absPath)
	if err != nil || rel == "." || strings.ContainsAny(rel, `/\`) || strings.HasPrefix(rel, "..") {
		return fmt.Errorf("unsafe migration scratch cleanup: %s", path)
	}
	if !plainPath(absPath) {
		return fmt.Errorf("linked migration scratch: %s", path)
	}
	return os.RemoveAll(absPath)
}

// The injectable checkpoint exists only at transaction boundaries, so tests can
// model interruption after each move without racing or killing user processes.
func migrateOverrideUnit(root, backups string, u overrideUnit, catalog *overrideCatalog, checkpoint func(string) error) (changed bool, backup string, result error) {
	transaction, err := os.MkdirTemp(backups, "migration-")
	if err != nil {
		return false, "", err
	}
	journalWritten := false
	defer func() {
		if !journalWritten {
			if err := removeOverrideScratch(backups, transaction); err != nil {
				result = errors.Join(result, err)
			}
		}
	}()
	before := map[string]string{}
	for _, rel := range u.sources {
		if !overrideRel(rel) {
			return false, "", rejectOverride("unsafe source %s", rel)
		}
		hash, err := overrideSnapshot(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			return false, "", err
		}
		before[rel] = hash
	}
	input := filepath.Join(root, filepath.FromSlash(u.sources[0]))
	aggregate := len(u.sources) != 1 || u.sources[0] != u.destination
	if aggregate {
		input = filepath.Join(transaction, "input")
		if err := os.Mkdir(input, 0755); err != nil {
			return false, "", err
		}
		for _, rel := range u.sources {
			if _, err := copyTreeMissing(filepath.Join(root, filepath.FromSlash(rel)), filepath.Join(input, filepath.FromSlash(rel))); err != nil {
				return false, "", err
			}
		}
	}
	stage := filepath.Join(transaction, "converted")
	if err := os.Mkdir(stage, 0755); err != nil {
		return false, "", err
	}
	out := overrideOutput{root: stage, paths: map[string]string{}, catalog: catalog}
	if err := out.component(input, "", u.destination, u.synthetic); err != nil {
		return false, "", fmt.Errorf("%s: %w", u.destination, err)
	}
	if !out.changed {
		return false, "", nil
	}
	for _, rel := range u.sources {
		after, err := overrideSnapshot(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			return false, "", err
		}
		if after != before[rel] {
			return false, "", fmt.Errorf("override source changed during conversion: %s", rel)
		}
	}
	if err := writeOverrideJSON(filepath.Join(transaction, "pending.json"), overrideJournal{u.sources, u.destination}); err != nil {
		return false, "", err
	}
	journalWritten = true
	if checkpoint != nil {
		if err := checkpoint("staged"); err != nil {
			return false, transaction, err
		}
	}
	for _, rel := range u.sources {
		source := filepath.Join(root, filepath.FromSlash(rel))
		target := filepath.Join(transaction, "original", filepath.FromSlash(rel))
		if !plainPath(source) || !plainPath(target) {
			return false, transaction, fmt.Errorf("linked migration source")
		}
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return false, transaction, err
		}
		if err := os.Rename(source, target); err != nil {
			return false, transaction, err
		}
		if checkpoint != nil {
			if err := checkpoint("moved:" + rel); err != nil {
				return false, transaction, err
			}
		}
	}
	destination := filepath.Join(root, filepath.FromSlash(u.destination))
	if !plainPath(destination) {
		return false, transaction, fmt.Errorf("linked migration destination")
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return false, transaction, fmt.Errorf("migration destination already exists: %s", destination)
	}
	if err := os.Rename(stage, destination); err != nil {
		return false, transaction, err
	}
	if checkpoint != nil {
		if err := checkpoint("published"); err != nil {
			return false, transaction, err
		}
	}
	if err := recoverOverrideMigration(root, transaction); err != nil {
		return false, transaction, err
	}
	return true, transaction, nil
}

func migrateOverrides(dataRoot, doom string) (overrideMigration, error) {
	var report overrideMigration
	if dataRoot == "" {
		return report, fmt.Errorf("local app-data folder is unavailable")
	}
	if !plainPath(dataRoot) {
		return report, fmt.Errorf("linked app-data root")
	}
	if err := os.MkdirAll(dataRoot, 0755); err != nil {
		return report, err
	}
	unlock, err := lockOverrideMigration(dataRoot)
	if err != nil {
		return report, err
	}
	defer unlock()
	root := filepath.Join(dataRoot, "overrides")
	backups := filepath.Join(dataRoot, "override-backups")
	for _, dir := range []string{root, backups} {
		if !plainPath(dir) {
			return report, fmt.Errorf("linked migration directory: %s", dir)
		}
		if err := os.MkdirAll(dir, 0755); err != nil {
			return report, err
		}
	}
	transactions, err := os.ReadDir(backups)
	if err != nil {
		return report, err
	}
	for _, entry := range transactions {
		if entry.IsDir() && strings.HasPrefix(entry.Name(), "migration-") {
			if err := recoverOverrideMigration(root, filepath.Join(backups, entry.Name())); err != nil {
				return report, err
			}
		}
	}
	units, err := findOverrideUnits(root)
	if err != nil {
		return report, err
	}
	catalog := &overrideCatalog{base: filepath.Join(doom, "base")}
	for _, unit := range units {
		changed, backup, err := migrateOverrideUnit(root, backups, unit, catalog, nil)
		if err != nil {
			var rejected *overrideRejected
			if errors.As(err, &rejected) {
				report.Rejected = append(report.Rejected, fmt.Sprintf("%s: %s", unit.destination, err))
				continue
			}
			if backup != "" {
				err = errors.Join(err, recoverOverrideMigration(root, backup))
			}
			return report, err
		}
		if changed {
			report.Converted++
			report.Backups = append(report.Backups, backup)
		} else {
			report.Unchanged++
		}
	}
	return report, nil
}

func printOverrideMigration(report overrideMigration) {
	fmt.Printf("Overrides: %d converted, %d already current, %d left intact for repair.\n", report.Converted, report.Unchanged, len(report.Rejected))
	for _, backup := range report.Backups {
		fmt.Printf("  Original backup: %s\n", backup)
	}
	for _, reason := range report.Rejected {
		fmt.Printf("  Skipped: %s\n", reason)
	}
}

func cmdMigrateOverrides(f flags) error {
	if checkDoomRunning() {
		return fmt.Errorf("close DOOM before converting files in the authoring library; map installation does not require a restart")
	}
	doom, err := resolveDoom(f.doom)
	if err != nil {
		return err
	}
	migrateUserData()
	report, err := migrateOverrides(appDataDir(), doom)
	printOverrideMigration(report)
	return err
}
