package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// runStoreMigration runs the store tool's check on the data folder and, when check finds work, its
// migrate with the DOOM folder. Every failure is a warning that names the command to run by hand.
// It returns 1 when the tool migrated, 0 when there was nothing to do, and -1 when the tool could not
// run or did not finish.
func runStoreMigration(tool, dir, doom string) int {
	rerun := fmt.Sprintf(`"%s" migrate "%s" --doom "%s"`, filepath.Join(dir, storeToolName), dir, doom)
	code, err := runStoreTool(tool, "check", dir)
	switch {
	case err != nil:
		ui.Warn("%s could not be run (%v), so your mods folder was not checked for files of earlier releases. "+
			"To do it by hand, close DOOM and run:\n  %s", storeToolName, err, rerun)
		return -1
	case code == 0:
		return 0
	case code != 1:
		ui.Warn("%s could not check your mods folder (exit code %d). To try again, close DOOM and run:\n  %s",
			storeToolName, code, rerun)
		return -1
	}
	code, err = runStoreTool(tool, "migrate", dir, "--doom", doom)
	switch {
	case err != nil:
		ui.Warn("%s could not be run (%v), so your mods folder was not migrated. To do it by hand, close DOOM "+
			"and run:\n  %s", storeToolName, err, rerun)
		return -1
	case code != 0:
		ui.Warn("%s did not finish migrating your mods folder (exit code %d); Snapmap+ itself is installed. "+
			"To finish, close DOOM and run:\n  %s", storeToolName, code, rerun)
		return -1
	}
	return 1
}

// cmdMigrate converts the mods folder with the store tool the last install or update left beside this
// installer, without reinstalling anything: for an overrides\ entry added after the first migration,
// or a conversion the installer could not finish. The DOOM folder comes from --doom or the install
// record; the tool needs it to tell game files from mod files.
func cmdMigrate(f flags) error {
	dir := appDataDir()
	if dir == "" {
		return fmt.Errorf("couldn't find %%LOCALAPPDATA%%, where the Snapmap+ data folder lives")
	}
	tool := filepath.Join(dir, storeToolName)
	if _, err := os.Stat(tool); err != nil {
		return fmt.Errorf("%s is not in %s -- run:  snapmap-plus update  (it places the tool and converts the mods folder)",
			storeToolName, dir)
	}
	doom := f.doom
	if doom == "" {
		rec, err := loadRecord()
		if err != nil {
			return fmt.Errorf("no existing install found -- pass --doom <path>, or run:  snapmap-plus install")
		}
		doom = rec.DoomPath
	}
	if checkDoomRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (the mods folder is in use while it runs)")
	}
	ui.Section("Converting mods")
	ui.PathField("Data folder", dir)
	ui.PathField("DOOM folder", doom)
	switch runStoreMigration(tool, dir, doom) {
	case 1:
		ui.OK("Mods folder converted.")
	case 0:
		ui.OK("Nothing to convert.")
	default:
		return fmt.Errorf("the mods folder was not fully converted; the lines above say what to do")
	}
	return nil
}
