package main

import (
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"syscall"
	"time"
)

type migrationChange struct {
	Path   string
	Before string
	After  string
	Stage  string
	Move   string `json:",omitempty"`
}

type migrationPlan struct {
	Roots         []string
	Export        string
	Deferred      []string `json:",omitempty"`
	Changes       []migrationChange
	Empty         []string
	data, work    string
	journalHash   string
	pins          *earlierPins
	destinations  map[string]int
	keepOverrides bool
}

func migrationFileHash(path string) (string, error) {
	if !plainPath(path) {
		return "", fmt.Errorf("linked content path %s", path)
	}
	stat, err := os.Lstat(path)
	if errors.Is(err, fs.ErrNotExist) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	if !stat.Mode().IsRegular() {
		return "", fmt.Errorf("expected a regular file at %s", path)
	}
	return fileSHA256(path)
}

func under(root, path string) bool {
	rel, err := filepath.Rel(root, path)
	return err == nil && rel != "." && filepath.IsLocal(rel)
}

func (p *migrationPlan) allows(path string) bool {
	if !filepath.IsAbs(path) || filepath.Clean(path) != path {
		return false
	}
	if p.Export != "" {
		if samePath(path, p.Export) || samePath(path, strings.TrimSuffix(p.Export, filepath.Ext(p.Export))+".meta.json") {
			return true
		}
		return under(filepath.Join(p.data, "cache", "content"), path)
	}
	if samePath(path, filepath.Join(p.data, "cache", "legacy-placements.json")) {
		return true
	}
	for _, root := range p.Roots {
		for _, dir := range []string{"mods", "maps", "prefabs", "rawmaps", "overrides"} {
			base := filepath.Join(root, dir)
			if under(base, path) || samePath(base, path) {
				return true
			}
		}
		for _, file := range []string{"rawmap.json", "rawmap.zip", "strings/strids.json"} {
			if samePath(path, filepath.Join(root, file)) {
				return true
			}
		}
	}
	for _, dir := range []string{"content", "served", "legacy-builds"} {
		if under(filepath.Join(p.data, "cache", dir), path) {
			return true
		}
	}
	if samePath(path, filepath.Join(p.data, "config.json")) {
		return true
	}
	return false
}

func (p *migrationPlan) cacheMove(from, to string) bool {
	for _, group := range []struct{ from, to string }{{".content", "content"}, {".served", "served"}, {".builds", "legacy-builds"}} {
		src := filepath.Join(p.data, "mods", group.from)
		rel, err := filepath.Rel(src, from)
		if err == nil && filepath.IsLocal(rel) && rel != "." && samePath(filepath.Join(p.data, "cache", group.to, rel), to) {
			return true
		}
	}
	return false
}

func (p *migrationPlan) record(path, before, after, source, stage string) error {
	path = filepath.Clean(path)
	if !p.allows(path) || !plainPath(path) {
		return fmt.Errorf("unsafe migration path %s", path)
	}
	if before == after {
		return nil
	}
	key := strings.ToLower(path)
	if i, ok := p.destinations[key]; ok {
		op := p.Changes[i]
		if op.Before == before && op.After == after {
			return nil
		}
		return fmt.Errorf("different content would use %s", path)
	}
	idx := len(p.Changes)
	if after != "" {
		dst := filepath.Join(p.work, fmt.Sprintf("new-%d", idx))
		if err := copyFile(source, dst); err != nil {
			return err
		}
		h, err := fileSHA256(dst)
		if err != nil || h != after {
			return fmt.Errorf("content changed while preparing %s", path)
		}
	}
	p.destinations[key] = idx
	p.Changes = append(p.Changes, migrationChange{Path: path, Before: before, After: after, Stage: stage})
	return nil
}

func (p *migrationPlan) put(path, source, stage string, replace bool) error {
	old, err := migrationFileHash(path)
	if err != nil {
		return err
	}
	next, err := migrationFileHash(source)
	if err != nil {
		return err
	}
	if next == "" {
		return fmt.Errorf("missing staged content %s", source)
	}
	if old != "" && old != next && !replace {
		return fmt.Errorf("the existing file at %s differs; choose which file to keep and retry", path)
	}
	return p.record(path, old, next, source, stage)
}

func (p *migrationPlan) remove(path, stage string) error {
	before, err := migrationFileHash(path)
	if err != nil {
		return err
	}
	return p.record(path, before, "", "", stage)
}

func (p *migrationPlan) empty(dir string) {
	if p.allows(dir) && !slices.Contains(p.Empty, dir) {
		p.Empty = append(p.Empty, dir)
	}
}

// Windows scanners can briefly hold newly written exports. Each retry checks the original identity again.
func retryMigrationAccess(run func() error) error {
	for attempt := 0; ; attempt++ {
		err := run()
		if err == nil || runtime.GOOS != "windows" || attempt == 8 || (!errors.Is(err, fs.ErrPermission) && !errors.Is(err, syscall.Errno(32))) {
			return err
		}
		time.Sleep(25 * time.Millisecond << attempt)
	}
}

func copyMigrationFile(src, dst string, before string) error {
	if !plainPath(src) || !plainPath(dst) {
		return fmt.Errorf("linked migration path %s", dst)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	input, err := os.Open(src)
	if err != nil {
		return err
	}
	defer input.Close()
	out, err := os.CreateTemp(filepath.Dir(dst), ".snapmap-install-*")
	if err != nil {
		return err
	}
	defer os.Remove(out.Name())
	_, e := io.Copy(out, input)
	err = errors.Join(e, out.Sync(), out.Close())
	if err != nil {
		return err
	}
	return retryMigrationAccess(func() error {
		h, err := migrationFileHash(dst)
		if err != nil {
			return err
		}
		if h != before {
			return fmt.Errorf("%s changed before replacement", dst)
		}
		if before == "" {
			return publishNativeCopy(out.Name(), dst)
		}
		return os.Rename(out.Name(), dst)
	})
}

func (p *migrationPlan) counts() map[string]int {
	counts := map[string]int{}
	for _, c := range p.Changes {
		counts[c.Stage]++
	}
	return counts
}

func (p *migrationPlan) inspect(applied bool) error {
	for i, c := range p.Changes {
		if !p.allows(c.Path) || !plainPath(c.Path) || (!pinHash(c.Before) && c.Before != "") ||
			(!pinHash(c.After) && c.After != "") || c.Before == c.After ||
			(c.Stage != "Mods" && c.Stage != "Maps" && c.Stage != "Prefabs") {
			return fmt.Errorf("invalid migration item %d", i)
		}
		current, err := migrationFileHash(c.Path)
		if err != nil {
			return err
		}
		if current != c.Before && (!applied || current != c.After) {
			return fmt.Errorf("%s changed since the migration was prepared", c.Path)
		}
		if c.Move != "" {
			if !p.allows(c.Move) || !plainPath(c.Move) || c.Before != "" || !p.cacheMove(c.Move, c.Path) {
				return fmt.Errorf("invalid cache relocation %s", c.Path)
			}
			h, err := migrationFileHash(c.Move)
			if err != nil || (current == c.After && h != "") || (current != c.After && h != c.After) {
				return fmt.Errorf("relocating cache file changed: %s", c.Move)
			}
		} else if c.After != "" && (!applied || current != c.After) {
			h, err := migrationFileHash(filepath.Join(p.work, fmt.Sprintf("new-%d", i)))
			if err != nil || h != c.After {
				return fmt.Errorf("prepared file for %s differs", c.Path)
			}
		}
	}
	for _, dir := range p.Empty {
		if !p.allows(dir) || !plainPath(dir) {
			return fmt.Errorf("invalid empty directory %s", dir)
		}
	}
	return nil
}

func migrationJournal(data string) string {
	return filepath.Join(installerCacheDir(data), "content-migration")
}

func journalHash(body []byte) string { return fmt.Sprintf("%x", sha256.Sum256(body)) }

func emptyMigrationJournal(path string) bool {
	entries, err := os.ReadDir(path)
	return err == nil && len(entries) == 0
}

func (p *migrationPlan) persist() error {
	dest := migrationJournal(p.data)
	if !plainPath(dest) {
		return fmt.Errorf("linked transaction directory %s", dest)
	}
	if err := os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
		return err
	}
	scratch := os.TempDir()
	if !strings.EqualFold(filepath.VolumeName(scratch), filepath.VolumeName(dest)) {
		scratch = filepath.Join(filepath.Dir(p.data), "Temp")
		if !plainPath(scratch) {
			return fmt.Errorf("linked temporary directory %s", scratch)
		}
		if err := os.MkdirAll(scratch, 0700); err != nil {
			return err
		}
	}
	temp, err := os.MkdirTemp(scratch, "snapmap-content-preflight-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(temp)
	for i, c := range p.Changes {
		if c.After != "" && c.Move == "" {
			leaf := fmt.Sprintf("new-%d", i)
			if err := copyMigrationFile(filepath.Join(p.work, leaf), filepath.Join(temp, leaf), ""); err != nil {
				return err
			}
		}
	}
	if err := p.inspect(false); err != nil {
		return err
	}
	b, err := json.Marshal(p)
	if err != nil {
		return err
	}
	if err = writeAtomicFile(filepath.Join(temp, "plan.json"), b); err != nil {
		return err
	}
	// A completed update can leave its empty journal behind if interrupted during cleanup.
	if emptyMigrationJournal(dest) {
		if err := os.Remove(dest); err != nil {
			return err
		}
	}
	if err = renameEarlierFolder(temp, dest); err != nil {
		return err
	}
	p.work, p.journalHash = dest, journalHash(b)
	return nil
}

func (p *migrationPlan) apply() error {
	if err := p.inspect(true); err != nil {
		return err
	}
	for _, stage := range []string{"Mods", "Maps", "Prefabs"} {
		ui.Section(stage)
		changed := 0
		for i, c := range p.Changes {
			if c.Stage != stage {
				continue
			}
			current, err := migrationFileHash(c.Path)
			if err != nil {
				return err
			}
			if current == c.After {
				continue
			}
			if current != c.Before {
				return fmt.Errorf("%s was edited during migration", c.Path)
			}
			if c.Move != "" {
				if err = os.MkdirAll(filepath.Dir(c.Path), 0755); err == nil {
					err = moveMigrationFile(c.Move, c.Path)
				}
			} else if c.After == "" {
				err = removeMigrationFile(c.Path)
			} else {
				err = copyMigrationFile(filepath.Join(p.work, fmt.Sprintf("new-%d", i)), c.Path, c.Before)
			}
			if err != nil {
				return err
			}
			if stage == "Maps" && under(filepath.Join(p.data, "rawmaps", "Updated saves"), c.Path) {
				ui.PathField("Open rawmap", c.Path)
			}
			changed++
		}
		ui.Field("Updated files", fmt.Sprint(changed))
	}
	for _, c := range p.Changes {
		h, err := migrationFileHash(c.Path)
		if err != nil || h != c.After {
			return fmt.Errorf("could not verify %s", c.Path)
		}
	}
	slices.SortFunc(p.Empty, func(a, b string) int { return len(b) - len(a) })
	for _, dir := range p.Empty {
		entries, err := os.ReadDir(dir)
		if errors.Is(err, fs.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		if len(entries) == 0 {
			if err = os.Remove(dir); err != nil {
				return err
			}
		}
	}
	if !samePath(p.work, migrationJournal(p.data)) {
		return fmt.Errorf("unexpected transaction directory")
	}
	return p.finish()
}

func (p *migrationPlan) finish() error {
	// Remove only transaction files named by this plan. An unrelated file is never swept.
	for i, c := range p.Changes {
		for _, x := range []struct{ prefix, hash string }{{"old", c.Before}, {"new", c.After}} {
			if x.hash == "" || (x.prefix == "new" && c.Move != "") {
				continue
			}
			path := filepath.Join(p.work, fmt.Sprintf("%s-%d", x.prefix, i))
			h, err := migrationFileHash(path)
			if err != nil {
				return err
			}
			if h == "" {
				continue
			}
			if h != x.hash {
				return fmt.Errorf("transaction file %s was edited", path)
			}
			if err = os.Remove(path); err != nil {
				return err
			}
		}
	}
	entries, err := os.ReadDir(p.work)
	if err != nil {
		return err
	}
	if len(entries) != 1 || entries[0].Name() != "plan.json" {
		return fmt.Errorf("unexpected file in migration journal %s", p.work)
	}
	if err = os.Remove(filepath.Join(p.work, "plan.json")); err != nil {
		return err
	}
	return os.Remove(p.work)
}
