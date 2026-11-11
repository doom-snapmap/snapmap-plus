package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func cmdInstall(f flags) error {
	doom, err := resolveDoom(f.doom)
	if err != nil {
		return err
	}
	// Identify the DOOM build before downloading anything: the supported build takes the
	// requested release, the 2018 build takes the final release made for it.
	build, err := detectDoomBuild(doom)
	if err != nil {
		return err
	}
	ui.Section("Detecting DOOM")
	ui.PathField("Folder", doom)
	ui.Field("Build", build.label)
	for _, line := range describeDoomGames(doom) {
		ui.Field("Game", line)
	}
	if f, err = routeRelease(f, build); err != nil {
		return err
	}
	// A release install keeps a stable copy of snapmap-plus.exe; a refused one changes nothing,
	// and a dev --local build is never copied.
	if f.local == "" {
		if err := selfInstall(f.force); err != nil {
			return err
		}
	}
	if checkDoomRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (Snapmap+'s files can't be replaced while the game has them open)")
	}
	if build.build == build2018 {
		return installLegacyRelease(f, doom)
	}
	f.doom = doom // the installation a release choice is compared with
	b, cleanup, err := acquireBundle(f)
	var newer *newerInstalled
	if errors.As(err, &newer) {
		ui.OK("%v", err)
		return nil
	}
	if err != nil {
		return err
	}
	defer cleanup()
	if err := checkBundleForBuild(b.files, build); err != nil {
		return err
	}
	// Only the detected build's pair is installed, whatever else the bundle carries (bundles of
	// earlier releases also carried the 2018 release's files).
	pair := buildPair(build.build)
	files, skipped := splitBundleForBuild(b.files, pair)

	prior, _ := loadRecord()
	legacy := detectLegacy(doom)
	own := newOwnership(doom, prior, b.files)
	earlier := describeEarlier(doom, build.build, prior, legacy, own)
	ui.Section("Earlier install")
	ui.Field("Found", earlier.found)
	ui.Note("%s", earlier.plan)

	confirmMsg := fmt.Sprintf("About to install Snapmap+ %s for the %s DOOM build into\n  %s\nContinue?",
		b.version, build.label, doom)
	if len(legacy) > 0 {
		ui.Warn("Found the original SnapHak in this DOOM folder. Snapmap+ replaces it, so its\n"+
			"files (%d) will be removed. Your maps and prefabs under\n"+
			"%%USERPROFILE%%\\snaphak are yours and are kept (they carry over).", len(legacy))
		confirmMsg = fmt.Sprintf("About to remove the original SnapHak and install Snapmap+ %s for the %s DOOM build into\n  %s\nContinue?",
			b.version, build.label, doom)
	}
	// FINAL gate -- runs only after every check passed (DOOM found, DOOM closed, bundle downloaded + verified).
	if !f.yes && isInteractive() {
		if !confirm(confirmMsg) {
			ui.Skip("Cancelled -- nothing was changed.")
			return nil
		}
	}
	tidyRetiredFolders()
	reportStartupTidy()
	ui.LazySection("Migrating player data")
	// Bring player content forward from earlier install locations.
	migrateUserData()

	rec := &installRecord{}
	if prior != nil {
		if samePath(prior.DoomPath, doom) {
			rec.Files = append(rec.Files, prior.Files...) // files in another DOOM folder are not here
		}
		rec.Backups = append(rec.Backups, prior.Backups...)
		rec.LegacyRemoved = append(rec.LegacyRemoved, prior.LegacyRemoved...)
	}
	if len(legacy) > 0 {
		ui.Section("Removing the original SnapHak")
		rec.Backups = dropLegacyBackups(doom, rec.Backups)
		rec.LegacyRemoved = append(rec.LegacyRemoved, removeLegacy(doom, legacy)...)
	}
	ui.LazySection("Cleaning the DOOM folder")
	printDoomTidy(cleanDoomFolder(doom, pair, own, rec), appDataDir())

	ui.Section("Installing files")
	ui.Field("Version", b.version)
	ui.PathField("Folder", doom)
	// Both files are written and checked beside their targets before either target changes.
	staged, err := stagePair(doom, b.root, files)
	if err != nil {
		return err
	}
	defer discardStaged(staged)
	// A file at a pair path that is not ours is kept in the data folder for uninstall to restore.
	var keptAside []backup
	for _, e := range files {
		target := filepath.Join(doom, e.rel)
		if isRegular(target) && !own.ours(doom, e.rel) && !hasBackupFor(rec, e.rel) {
			if got, herr := fileSHA256(target); herr != nil || got != e.sha256 {
				dst, err := keepBackup(target, e.rel)
				if err != nil {
					restoreKept(doom, keptAside)
					return fmt.Errorf("couldn't move the existing %s aside (%v) -- check you can write to your DOOM folder (try running as administrator)", e.rel, err)
				}
				keptAside = append(keptAside, backup{Rel: e.rel, Backup: dst})
				ui.Note("%s belonged to another program: it is kept at %s, and uninstall puts it back.", e.rel, dst)
			}
		}
	}
	if err := swapPair(staged); err != nil {
		restoreKept(doom, keptAside)
		return fmt.Errorf("couldn't write Snapmap+'s files into your DOOM folder (%v); the folder is as it was -- check you have permission (try running as administrator)", err)
	}
	rec.Backups = append(rec.Backups, keptAside...)
	placed := ui.List("Installed files")
	for _, e := range files {
		placed.Add(e.rel)
	}
	placed.Close()
	for _, e := range skipped {
		ui.Skip("not installed: %s (the bundle carries it; the %s does not use it)", e.rel, build.label)
	}
	rec.Files = nil
	for _, e := range files {
		rec.Files = append(rec.Files, e.rel)
	}
	if err := saveInstall(b.version, doom, build.build, rec); err != nil {
		return fmt.Errorf("installed the files, but couldn't write the install record (%v) -- run: snapmap-plus update", err)
	}
	migrateLegacyLogs(doom) // runtime logs live in snapmap-plus\logs\ -- fold older locations (snaphak_logs\, snaphak\logs\) in (best-effort)
	// The bundle's snapmap-plus-store converts a mods folder of earlier releases (never fails the install).
	migrateStore(b.root, doom)
	ui.Blank()
	ui.OK("Snapmap+ %s installed.", b.version)
	ensureWebView2Runtime(f) // the HTML UI renders in the WebView2 runtime -- ensure it's present (never fails the install)
	ui.Blank()
	ui.Note("Launch DOOM and open the SnapMap editor.")
	return nil
}

// saveInstall writes the installation into install.json, keeping the installer's own state there.
func saveInstall(ver, doom, build string, rec *installRecord) error {
	dir := appDataDir()
	if dir == "" {
		return fmt.Errorf("couldn't find your local app-data folder (the LOCALAPPDATA environment variable is not set)")
	}
	return updateStateAt(dir, func(r *installRecord) {
		r.Version = ver
		r.DoomPath = doom
		r.DoomBuild = build
		r.InstalledAt = time.Now().UTC().Format(time.RFC3339)
		r.Files = rec.Files
		r.Backups = rec.Backups
		r.LegacyRemoved = rec.LegacyRemoved
		if r.Files == nil {
			r.Files = []string{}
		}
		if r.Backups == nil {
			r.Backups = []backup{}
		}
	})
}

// earlierInstall names what an install found before it and what it will do about it.
type earlierInstall struct{ found, plan string }

// describeEarlier recognises the migration path: the original SnapHak, a release that left the other
// build's files or its own backups and records, the current layout, or nothing.
func describeEarlier(doom, build string, prior *installRecord, legacy []string, own doomOwnership) earlierInstall {
	ver := "an earlier release"
	if prior != nil && prior.Version != "" {
		ver = prior.Version
	}
	pair := buildPair(build)
	other := false
	for _, rel := range ourDoomNames {
		if !containsRel(pair, rel) && isRegular(filepath.Join(doom, rel)) && own.ours(doom, rel) {
			other = true
		}
	}
	baks := false
	for _, dir := range []string{"", "snapmap-plus"} {
		entries, _ := os.ReadDir(filepath.Join(doom, dir))
		for _, e := range entries {
			for _, suf := range ourBackupSuffixes {
				baks = baks || strings.HasSuffix(strings.ToLower(e.Name()), suf)
			}
		}
	}
	present := false
	for _, rel := range pair {
		present = present || isRegular(filepath.Join(doom, rel))
	}
	retired := len(startupTidy) > 0
	if len(legacy) > 0 {
		return earlierInstall{"the original SnapHak",
			"Its files are removed from the DOOM folder, your prefabs, strings and rawmaps under\n" +
				"%USERPROFILE%\\snaphak are copied to %LOCALAPPDATA%\\snapmap-plus, and Snapmap+'s two files are installed."}
	}
	if prior == nil && !other && !baks && !retired {
		if present {
			return earlierInstall{"Snapmap+ files with no install record", "They are replaced by this release's files."}
		}
		return earlierInstall{"nothing: this is a fresh install", "Snapmap+'s two files are installed."}
	}
	var found, gone []string
	switch {
	case other && present:
		found, gone = append(found, "both DOOM builds' files"), append(gone, "the other build's files")
	case other:
		found, gone = append(found, "the other DOOM build's files"), append(gone, "the other build's files")
	}
	if baks {
		found, gone = append(found, "backups in the DOOM folder"), append(gone, "the backups")
	}
	if retired {
		found, gone = append(found, "retired installer records"), append(gone, "the retired records")
	}
	if len(found) == 0 {
		return earlierInstall{"Snapmap+ " + ver, "An update: Snapmap+'s two files are replaced."}
	}
	plan := upperFirst(joinAnd(gone)) + " are removed, then Snapmap+'s two files for this build are installed."
	if other && !present {
		plan += "\nThe store tool converts the overrides\\ packages of that release." // a retired package folder
	}
	return earlierInstall{"Snapmap+ " + ver + " with " + joinAnd(found), plan}
}

// joinAnd joins phrases as "a", "a and b" or "a, b and c".
func joinAnd(parts []string) string {
	if len(parts) <= 1 {
		return strings.Join(parts, "")
	}
	return strings.Join(parts[:len(parts)-1], ", ") + " and " + parts[len(parts)-1]
}

func upperFirst(s string) string {
	if s == "" {
		return s
	}
	return strings.ToUpper(s[:1]) + s[1:]
}

// tidyRetiredFolders removes the retired package folders of earlier releases while they are empty;
// reportStartupTidy prints what it did. One that still holds files is the player's and stays.
func tidyRetiredFolders() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	for _, name := range []string{"override-backups", "package-cache", "package-staging"} { // each a retired package folder
		p := filepath.Join(dir, name)
		if st, err := os.Lstat(p); err != nil || !st.IsDir() {
			continue
		}
		if entries, err := os.ReadDir(p); err == nil && len(entries) == 0 && os.Remove(p) == nil {
			startupTidy = append(startupTidy, name+"\\ (an empty folder of an earlier release)")
		} else {
			startupKept = append(startupKept, name+"\\ (it holds files of an earlier release; Snapmap+ no longer reads it)")
		}
	}
}

// migrateLegacyLogs moves old runtime logs into snapmap-plus/logs. Locked files
// and name collisions stay at the old location; migration failure is nonfatal.
func migrateLegacyLogs(doom string) {
	newDir := filepath.Join(doom, "snapmap-plus", "logs")
	foldLogsForward(filepath.Join(doom, "snaphak_logs"), newDir)
	foldLogsForward(filepath.Join(doom, "snaphak", "logs"), newDir)
	removeIfEmpty(filepath.Join(doom, "snaphak")) // the previous-name overlay dir, once its DLL + logs are gone
}

// foldLogsForward moves every file in old into newDir (keeping any that already exist there), then removes old
// if it ends up empty. Best-effort.
func foldLogsForward(old, newDir string) {
	entries, err := os.ReadDir(old)
	if err != nil {
		return // nothing to migrate
	}
	if err := os.MkdirAll(newDir, 0o755); err != nil {
		return
	}
	moved := false
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		dst := filepath.Join(newDir, e.Name())
		if _, err := os.Stat(dst); err == nil {
			continue // already exists at the new location -- keep it, leave the old copy
		}
		if os.Rename(filepath.Join(old, e.Name()), dst) == nil {
			moved = true
		}
	}
	if rest, err := os.ReadDir(old); err == nil && len(rest) == 0 {
		os.Remove(old)
		if moved {
			ui.OK("moved runtime logs into snapmap-plus\\logs\\")
		}
	}
}

func cmdUpdate(f flags) error {
	rec, err := loadRecord()
	ui.Section("Updating")
	if err != nil {
		ui.Note("No existing install found -- doing a fresh install.")
		return cmdInstall(f)
	}
	if f.doom == "" {
		f.doom = rec.DoomPath
	}
	ui.Field("Current version", rec.Version)
	ui.PathField("Folder", rec.DoomPath)
	if err := cmdInstall(f); err != nil {
		return err
	}
	// The overlay is updated; now refresh snapmap-plus.exe itself (best-effort -- never fails the overlay update).
	if !f.noSelf {
		ui.LazySection("Updating snapmap-plus.exe")
		selfUpdate(f, resolveToken(f))
	}
	return nil
}

func cmdUninstall(f flags) error {
	rec, err := loadRecord()
	if err != nil {
		// Without a record the DOOM folder named by --doom is cleaned by content: a file is
		// removed only when its bytes show it is a Snapmap+ file.
		if f.doom == "" {
			return fmt.Errorf("Snapmap+ has no install record on this PC -- if its files are still in your DOOM folder, run: snapmap-plus uninstall --doom \"C:\\path\\to\\DOOM\"")
		}
		if !holdsOurFiles(f.doom) {
			return fmt.Errorf("no Snapmap+ files were found in %s -- nothing to uninstall", f.doom)
		}
		rec = &installRecord{DoomPath: f.doom}
	}
	doom := rec.DoomPath
	if f.doom != "" {
		doom = f.doom
	}
	if checkDoomRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (its files are in use)")
	}
	ui.Section("Uninstalling")
	if rec.Version != "" {
		ui.Field("Version", rec.Version)
	} else {
		ui.Field("Version", "not recorded")
	}
	ui.PathField("Folder", doom)
	if !f.yes && isInteractive() {
		if !confirm(fmt.Sprintf("About to remove Snapmap+ from\n  %s\nand restore vanilla. Continue?", doom)) {
			ui.Skip("Cancelled -- nothing was changed.")
			return nil
		}
	}
	tidyRetiredFolders()
	reportStartupTidy()
	ui.Section("Removing Snapmap+ from the DOOM folder")

	// Remove every file of ours, wherever a release put it. Backups of files that were not ours move
	// out of DOOM first, then go back where they were.
	t := cleanDoomFolder(doom, nil, newOwnership(doom, rec, nil), rec)
	printDoomTidy(doomTidy{removed: t.removed}, appDataDir())
	for _, bk := range rec.Backups {
		target := filepath.Join(doom, bk.Rel)
		if isRegular(target) {
			ui.Warn("kept the saved %s at %s because another file is already in its place; copy it over that file if you want the original back", bk.Rel, bk.Backup)
			continue
		}
		os.MkdirAll(filepath.Dir(target), 0o755)
		if err := moveFile(bk.Backup, target); err != nil {
			ui.Warn("couldn't put back %s (%v); it is kept at %s -- copy it into your DOOM folder", bk.Rel, err, bk.Backup)
			continue
		}
		ui.OK("restored %s, which was there before Snapmap+", bk.Rel)
	}
	printForeign(t.foreign)
	// Remove the runtime log and crash trees. Remove overlay and plugin directories only if empty.
	os.RemoveAll(filepath.Join(doom, "snapmap-plus", "logs"))
	os.RemoveAll(filepath.Join(doom, "snapmap-plus", "crash"))
	os.RemoveAll(filepath.Join(doom, "snaphak", "logs"))
	os.RemoveAll(filepath.Join(doom, "snaphak_logs"))
	removeIfEmpty(filepath.Join(doom, "snapmap-plus"))
	removeIfEmpty(filepath.Join(doom, "snaphak"))
	// Remove the installer's record, the store tool and unused executable copies; preserve runtime
	// configuration and the player's content.
	cleanupAppData()
	ui.OK("DOOM restored to vanilla. (Your Snapmap+ modding data was left untouched.)")
	return nil
}

// holdsOurFiles reports whether doom holds a Snapmap+ file at a name any release used.
func holdsOurFiles(doom string) bool {
	own := newOwnership(doom, nil, nil)
	for _, rel := range ourDoomNames {
		if isRegular(filepath.Join(doom, rel)) && own.ours(doom, rel) {
			return true
		}
	}
	return false
}

func cmdStatus(f flags) error {
	rec, err := loadRecord()
	ui.Section("Status")
	if err != nil {
		ui.Skip("Snapmap+ is not installed (no install record).")
		return nil
	}
	ui.Field("Version", rec.Version)
	ui.PathField("Folder", rec.DoomPath)
	ui.Field("Installed", rec.InstalledAt)
	ui.Field("Installed files", fmt.Sprint(len(rec.Files)))
	ui.Field("Backups", fmt.Sprint(len(rec.Backups)))
	games := describeDoomGames(rec.DoomPath)
	if len(games) == 0 {
		ui.Field("Game", "no DOOM 2016 executable found")
	}
	for _, line := range games {
		ui.Field("Game", line)
	}
	missing := 0
	for _, rel := range rec.Files {
		if _, err := os.Stat(filepath.Join(rec.DoomPath, rel)); err != nil {
			missing++
		}
	}
	if missing > 0 {
		ui.Warn("%d Snapmap+ file(s) are missing from the DOOM folder -- run: snapmap-plus update", missing)
	}
	return nil
}

// cmdVersion prints the installer's own version and, if a mod is installed, the installed mod version.
func cmdVersion() {
	ui.Raw("snapmap-plus " + version + "\n")
	if rec, err := loadRecord(); err == nil {
		ui.Field("Installed mod", rec.Version)
		ui.PathField("Folder", rec.DoomPath)
		for _, line := range describeDoomGames(rec.DoomPath) {
			ui.Field("Game", line)
		}
	}
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(out, in)
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}

func removeIfEmpty(dir string) {
	entries, err := os.ReadDir(dir)
	if err == nil && len(entries) == 0 {
		os.Remove(dir)
	}
}
