package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Each changed outer package is one transaction below
// %LOCALAPPDATA%\snapmap-plus\override-backups\migration-<random>:
//
//	pending.json   journal written before any authored file moves
//	stage/<n>/     the complete converted tree for each published folder
//	original/...   the authored sources, moved out of overrides unchanged
//	verified.json  written after the moved originals are verified again
//	complete.json  written after publication or rollback
//
// Before verified.json, recovery restores the originals. After it, recovery
// finishes publication. Neither direction overwrites an existing path.

// Tests stop a transaction at its boundaries, as a crash would.
var overrideMigrationCheckpoint func(point string)

func migrationCheckpoint(point string) {
	if overrideMigrationCheckpoint != nil {
		overrideMigrationCheckpoint(point)
	}
}

type migrationJournal struct {
	Unit    string           `json:"unit"`
	Sources []string         `json:"sources"`
	Publish []journalPublish `json:"publish"`
}

type journalPublish struct {
	Stage       string `json:"stage"`
	Destination string `json:"destination"`
}

type convertedOverride struct {
	Package string
	Backup  string
}

type overrideMigration struct {
	Converted []convertedOverride
	Unchanged int
	Rejected  []string
	Notes     []string
}

func writeJournal(path string, value any) error {
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	_, err = f.Write(append(b, '\n'))
	return errors.Join(err, f.Sync(), f.Close())
}

func pathExists(path string) (bool, error) {
	_, err := os.Lstat(path)
	if err == nil {
		return true, nil
	}
	if os.IsNotExist(err) {
		return false, nil
	}
	return false, err
}

// Paths inside a journal must stay below their roots.
func journalRelValid(rel string) bool {
	if rel == "" || strings.ContainsAny(rel, `\:`) {
		return false
	}
	for _, part := range strings.Split(rel, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

func recoverOverrideTransactions(root, backups string) error {
	entries, err := os.ReadDir(backups)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() && strings.HasPrefix(entry.Name(), "migration-") {
			if err := recoverOverrideTransaction(root, filepath.Join(backups, entry.Name())); err != nil {
				return err
			}
		}
	}
	return nil
}

func recoverOverrideTransaction(root, transaction string) error {
	if !plainPath(transaction) {
		return fmt.Errorf("linked migration transaction: %s", transaction)
	}
	if done, err := pathExists(filepath.Join(transaction, "complete.json")); done || err != nil {
		return err
	}
	b, err := os.ReadFile(filepath.Join(transaction, "pending.json"))
	if os.IsNotExist(err) {
		// Interrupted while staging: no authored file had moved yet.
		if moved, err := pathExists(filepath.Join(transaction, "original")); moved || err != nil {
			if err == nil {
				err = fmt.Errorf("migration transaction without a journal still holds originals: %s", transaction)
			}
			return err
		}
		return removeTransactionData(transaction, filepath.Join(transaction, "stage"), true)
	}
	if err != nil {
		return err
	}
	var j migrationJournal
	if err := json.Unmarshal(b, &j); err != nil {
		return fmt.Errorf("unreadable migration journal %s: %w", transaction, err)
	}
	for _, rel := range j.Sources {
		if !journalRelValid(rel) {
			return fmt.Errorf("invalid migration journal: %s", transaction)
		}
	}
	for _, p := range j.Publish {
		if !journalRelValid(p.Destination) || !journalRelValid(p.Stage) {
			return fmt.Errorf("invalid migration journal: %s", transaction)
		}
	}
	verified, err := pathExists(filepath.Join(transaction, "verified.json"))
	if err != nil {
		return err
	}
	if verified {
		for _, p := range j.Publish {
			stage := filepath.Join(transaction, filepath.FromSlash(p.Stage))
			destination := filepath.Join(root, filepath.FromSlash(p.Destination))
			staged, err := pathExists(stage)
			if err != nil || !staged {
				if err != nil {
					return err
				}
				continue
			}
			if taken, err := pathExists(destination); err != nil || taken {
				if err == nil {
					err = fmt.Errorf("cannot finish converting %s: that folder exists again; move it aside and run migrate-overrides again (the converted copy is %s)", destination, stage)
				}
				return err
			}
			if err := renameNoReplace(stage, destination); err != nil {
				return err
			}
		}
		if err := removeEmptyFolders(filepath.Join(transaction, "stage")); err != nil {
			return err
		}
		return writeJournal(filepath.Join(transaction, "complete.json"), map[string]string{"result": "published"})
	}
	if err := rollbackOverrideTransaction(root, transaction, j); err != nil {
		return err
	}
	return writeJournal(filepath.Join(transaction, "complete.json"), map[string]string{"result": "rolled back"})
}

// Restore moved originals, then discard the converted copies. Originals are
// never placed over a path that exists again.
func rollbackOverrideTransaction(root, transaction string, j migrationJournal) error {
	for i := len(j.Sources) - 1; i >= 0; i-- {
		backup := filepath.Join(transaction, "original", filepath.FromSlash(j.Sources[i]))
		source := filepath.Join(root, filepath.FromSlash(j.Sources[i]))
		moved, err := pathExists(backup)
		if err != nil {
			return err
		}
		if !moved {
			continue
		}
		if taken, err := pathExists(source); err != nil || taken {
			if err == nil {
				err = fmt.Errorf("cannot restore %s: that path exists again; nothing was overwritten, and the original is kept at %s", source, backup)
			}
			return err
		}
		if err := os.MkdirAll(filepath.Dir(source), 0o755); err != nil {
			return err
		}
		if err := renameNoReplace(backup, source); err != nil {
			return err
		}
	}
	if err := removeEmptyFolders(filepath.Join(transaction, "original")); err != nil {
		return err
	}
	return removeTransactionData(transaction, filepath.Join(transaction, "stage"), false)
}

// Remove folders left empty after originals moved back. A folder that still
// holds anything is kept.
func removeEmptyFolders(path string) error {
	entries, err := os.ReadDir(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, e := range entries {
		if e.IsDir() && e.Type()&os.ModeSymlink == 0 {
			if err := removeEmptyFolders(filepath.Join(path, e.Name())); err != nil {
				return err
			}
		}
	}
	if entries, err = os.ReadDir(path); err == nil && len(entries) == 0 {
		return os.Remove(path)
	}
	return err
}

// Remove converted copies, which exist only inside this transaction.
func removeTransactionData(transaction, path string, whole bool) error {
	rel, err := filepath.Rel(transaction, path)
	if err != nil || rel == "." || strings.HasPrefix(rel, "..") || filepath.IsAbs(rel) || !plainPath(path) {
		return fmt.Errorf("unsafe migration cleanup: %s", path)
	}
	if err := os.RemoveAll(path); err != nil {
		return err
	}
	if whole {
		return os.Remove(transaction)
	}
	return nil
}

func newTransaction(backups string) (string, error) {
	var random [8]byte
	if _, err := rand.Read(random[:]); err != nil {
		return "", err
	}
	path := filepath.Join(backups, "migration-"+hex.EncodeToString(random[:]))
	return path, os.Mkdir(path, 0o755)
}

// stage writes one planned tree without replacing anything, verifying each
// authored byte against the planning snapshot while it is copied.
func (p *unitPlan) stage(directory string, outputs *outputSet) error {
	if err := os.Mkdir(directory, 0o755); err != nil {
		return err
	}
	for _, item := range outputs.sorted() {
		target := filepath.Join(directory, filepath.FromSlash(item.rel))
		if item.dir {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err != nil {
			return err
		}
		err = p.writeOutput(f, item)
		if err := errors.Join(err, f.Sync(), f.Close()); err != nil {
			return err
		}
	}
	return nil
}

func (p *unitPlan) writeOutput(w io.Writer, item *plannedOutput) error {
	switch {
	case item.data != nil:
		_, err := w.Write(item.data)
		return err
	case item.record != nil:
		return p.ctx.catalog.decode(item.record, w)
	}
	want := p.snapshot[item.source]
	f, err := os.Open(p.abs(item.source))
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(w, h), f)
	if err != nil {
		return err
	}
	var sum [32]byte
	copy(sum[:], h.Sum(nil))
	if n != want.size || sum != want.sum {
		return fmt.Errorf("%s changed while it was being converted; nothing was published, run the command again", item.source)
	}
	return nil
}

func (p *unitPlan) verifyMoved(transaction string) error {
	moved := treeSnapshot{}
	for _, source := range p.sources {
		abs := filepath.Join(transaction, "original", filepath.FromSlash(source))
		info, err := os.Lstat(abs)
		if err != nil {
			return err
		}
		node := &libraryNode{rel: source, name: filepath.Base(abs), size: info.Size()}
		if info.IsDir() {
			if node, err = listLibrary(abs, source, false); err != nil {
				return err
			}
		} else if !info.Mode().IsRegular() {
			node.unsafe = true
		}
		if err := snapshotTree(abs, node, source, moved); err != nil {
			return err
		}
	}
	if !sameSnapshot(moved, p.snapshot) {
		return errors.New("an original changed during conversion; it was restored and nothing was published, run the command again")
	}
	return nil
}

// publish runs one transaction. Content refusals were found during planning;
// every error here is operational, after the unit has been restored.
func (p *unitPlan) publish(backups string) (string, error) {
	transaction, err := newTransaction(backups)
	if err != nil {
		return "", err
	}
	journal := migrationJournal{Unit: p.unit.rel, Sources: p.sources}
	for i, root := range p.roots {
		stage := fmt.Sprintf("stage/%d", i)
		if i == 0 {
			if err := os.Mkdir(filepath.Join(transaction, "stage"), 0o755); err != nil {
				return "", err
			}
		}
		if err := p.stage(filepath.Join(transaction, filepath.FromSlash(stage)), root.outputs); err != nil {
			return "", errors.Join(err, removeTransactionData(transaction, filepath.Join(transaction, "stage"), true))
		}
		journal.Publish = append(journal.Publish, journalPublish{Stage: stage, Destination: root.rel})
	}
	if err := writeJournal(filepath.Join(transaction, "pending.json"), journal); err != nil {
		return "", errors.Join(err, removeTransactionData(transaction, filepath.Join(transaction, "stage"), true))
	}
	migrationCheckpoint("journaled")
	restore := func(cause error) (string, error) {
		if err := rollbackOverrideTransaction(p.ctx.root, transaction, journal); err != nil {
			return transaction, fmt.Errorf("%v; restoring the originals also failed (%v); they are kept in %s", cause, err, transaction)
		}
		return "", errors.Join(cause, writeJournal(filepath.Join(transaction, "complete.json"), map[string]string{"result": "rolled back"}))
	}
	for _, source := range p.sources {
		target := filepath.Join(transaction, "original", filepath.FromSlash(source))
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return restore(err)
		}
		if err := renameNoReplace(p.abs(source), target); err != nil {
			return restore(fmt.Errorf("cannot move %s aside (close programs using it): %w", source, err))
		}
		migrationCheckpoint("moved:" + source)
	}
	if err := p.verifyMoved(transaction); err != nil {
		return restore(err)
	}
	if err := writeJournal(filepath.Join(transaction, "verified.json"), map[string]string{"sources": "verified"}); err != nil {
		return restore(err)
	}
	migrationCheckpoint("verified")
	for i, target := range journal.Publish {
		destination := p.abs(target.Destination)
		err := os.MkdirAll(filepath.Dir(destination), 0o755)
		if err == nil {
			err = renameNoReplace(filepath.Join(transaction, filepath.FromSlash(target.Stage)), destination)
		}
		if err != nil {
			// Undo this transaction completely before reporting the failure.
			for j := i - 1; j >= 0; j-- {
				back := journal.Publish[j]
				if undo := renameNoReplace(p.abs(back.Destination), filepath.Join(transaction, filepath.FromSlash(back.Stage))); undo != nil {
					return transaction, fmt.Errorf("publishing %s failed (%v) and could not be undone (%v); run migrate-overrides again to finish", target.Destination, err, undo)
				}
			}
			if undo := os.Remove(filepath.Join(transaction, "verified.json")); undo != nil {
				return transaction, fmt.Errorf("publishing %s failed (%v); run migrate-overrides again to finish", target.Destination, err)
			}
			return restore(err)
		}
		migrationCheckpoint("published:" + target.Destination)
	}
	if err := removeEmptyFolders(filepath.Join(transaction, "stage")); err != nil {
		return transaction, err
	}
	if err := writeJournal(filepath.Join(transaction, "complete.json"), map[string]string{"result": "published"}); err != nil {
		return transaction, err
	}
	return transaction, nil
}

func migrateOverrides(dataRoot, doom string) (overrideMigration, error) {
	var report overrideMigration
	if dataRoot == "" {
		return report, errors.New("the local app-data folder is unavailable")
	}
	if !plainPath(dataRoot) {
		return report, errors.New("the Snapmap+ app-data folder is a link; conversion refuses linked paths")
	}
	if err := os.MkdirAll(dataRoot, 0o755); err != nil {
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
			return report, fmt.Errorf("linked migration folder: %s", dir)
		}
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return report, err
		}
	}
	if err := recoverOverrideTransactions(root, backups); err != nil {
		return report, err
	}
	ctx := &migrationContext{root: root, catalog: newOverrideCatalog(doom), ids: map[string]string{}}
	discovery, err := discoverOverrides(root, ctx.catalog)
	if err != nil {
		return report, err
	}
	for _, skipped := range discovery.skipped {
		report.Notes = append(report.Notes, fmt.Sprintf("%s is a link or special file and was left unchanged", skipped))
	}
	var rejected *overrideRejected
	var plans []*unitPlan
	for _, unit := range discovery.units {
		plan, err := inspectUnit(ctx, unit)
		if errors.As(err, &rejected) {
			report.Rejected = append(report.Rejected, err.Error())
			continue
		}
		if err != nil {
			return report, fmt.Errorf("%s: %w", unit.rel, err)
		}
		plans = append(plans, plan)
	}
	var changed []*unitPlan
	for _, plan := range plans {
		if !plan.changed() {
			report.Unchanged++
			continue
		}
		if err := plan.build(); errors.As(err, &rejected) {
			report.Rejected = append(report.Rejected, err.Error())
			continue
		} else if err != nil {
			return report, fmt.Errorf("%s: %w", plan.unit.rel, err)
		}
		changed = append(changed, plan)
	}
	report.Notes = append(report.Notes, overlapNotes(plans, changed)...)
	for _, plan := range changed {
		transaction, err := plan.publish(backups)
		if err != nil {
			return report, fmt.Errorf("%s: %w", plan.unit.rel, err)
		}
		report.Converted = append(report.Converted, convertedOverride{plan.unit.rel, transaction})
		report.Notes = append(report.Notes, plan.notes...)
	}
	return report, nil
}

// Report engine paths that several packages will supply with different bytes.
// Legacy precedence no longer decides them; the compiler composes compatible
// declaration edits and reports incompatible ones.
func overlapNotes(plans, changed []*unitPlan) []string {
	type supply struct {
		unit string
		size int64
		sum  func() ([32]byte, error)
	}
	byPath := map[string][]supply{}
	converted := map[string]bool{}
	for _, plan := range changed {
		converted[plan.unit.rel] = true
		for _, root := range plan.roots {
			for _, item := range root.outputs.items {
				engine := assetEngine(item.rel)
				if engine == "" || item.dir {
					continue
				}
				item, outputs := item, root.outputs
				size := int64(-1)
				if item.source != "" {
					size = plan.snapshot[item.source].size
				}
				byPath[engine] = append(byPath[engine], supply{plan.unit.rel, size, func() ([32]byte, error) { return outputs.digest(item) }})
			}
		}
	}
	for _, plan := range plans {
		if converted[plan.unit.rel] || plan.outer == nil {
			continue
		}
		plan := plan
		plan.outer.node.walk(func(n *libraryNode) bool {
			engine := assetEngine(n.rel)
			if n.dir || n.unsafe || engine == "" || len(byPath[engine]) == 0 {
				return true
			}
			path := filepath.Join(plan.abs(plan.unit.rel), filepath.FromSlash(n.rel))
			byPath[engine] = append(byPath[engine], supply{plan.unit.rel, n.size, func() ([32]byte, error) {
				_, sum, err := hashFile(path)
				return sum, err
			}})
			return true
		})
	}
	var notes []string
	paths := make([]string, 0, len(byPath))
	for path := range byPath {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	for _, path := range paths {
		supplies := byPath[path]
		units := map[string]bool{}
		for _, s := range supplies {
			units[s.unit] = true
		}
		if len(units) < 2 {
			continue
		}
		differs := false
		first, err := supplies[0].sum()
		for _, s := range supplies[1:] {
			if err != nil {
				break
			}
			sum, e := s.sum()
			err = e
			differs = differs || sum != first
		}
		if err == nil && differs {
			names := make([]string, 0, len(units))
			for unit := range units {
				names = append(names, unit)
			}
			sort.Strings(names)
			notes = append(notes, fmt.Sprintf("%s is supplied with different contents by %s; loading composes compatible edits and reports conflicting ones", path, strings.Join(names, ", ")))
		}
	}
	const limit = 20
	if len(notes) > limit {
		notes = append(notes[:limit], fmt.Sprintf("%d more resources are supplied by more than one package", len(notes)-limit))
	}
	return notes
}

// Engine path of an output or authored file below any component's assets/.
func assetEngine(rel string) string {
	parts := strings.Split(rel, "/")
	for i, part := range parts[:len(parts)-1] {
		if strings.EqualFold(part, "assets") {
			return asciiLower(strings.Join(parts[i+1:], "/"))
		}
	}
	return ""
}

func printOverrideMigration(report overrideMigration) {
	if len(report.Converted) == 0 && len(report.Rejected) == 0 && len(report.Notes) == 0 {
		return
	}
	fmt.Printf("Overrides: %d converted, %d already current, %d need attention.\n",
		len(report.Converted), report.Unchanged, len(report.Rejected))
	for _, c := range report.Converted {
		fmt.Printf("  ~ converted %s (originals kept in %s)\n", c.Package, filepath.Join(c.Backup, "original"))
	}
	for _, reason := range report.Rejected {
		fmt.Printf("  ! left unchanged: %s\n", reason)
	}
	for _, note := range report.Notes {
		fmt.Printf("  - %s\n", note)
	}
}

func cmdMigrateOverrides(f flags) error {
	if checkDoomRunning() {
		return errors.New("DOOM is running -- close it, then run this again")
	}
	doom, err := resolveDoom(f.doom)
	if err != nil {
		// The catalog is needed only for loose files, manifests and includes.
		doom = ""
	}
	migrateUserData()
	report, err := migrateOverrides(appDataDir(), doom)
	printOverrideMigration(report)
	if err != nil {
		return err
	}
	if len(report.Rejected) > 0 {
		return fmt.Errorf("%d package(s) were left unchanged; resolve the reasons above, then run migrate-overrides again", len(report.Rejected))
	}
	if len(report.Converted) == 0 {
		fmt.Println("Overrides are already in the current package format.")
	}
	return nil
}
