package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// The store migration tool, snapmap-plus-store.exe (src/store), converts a mods folder of earlier
// releases, retires removed-mods\, moves overrides\ into mods\, and converts once every mod folder
// and saved map of an earlier shape (My Maps, rawmap.json, prefabs) to the one the game reads. It
// ships in the bundle beside
// MANIFEST.sha256 but not in it: every installer installs each manifest-listed file into DOOM, and
// the tool belongs next to snapmap-plus.exe in %LOCALAPPDATA%\snapmap-plus. Its digest is in
// TOOLS.sha256 instead.
const (
	storeToolName     = "snapmap-plus-store.exe"
	storeToolManifest = "TOOLS.sha256"
)

// storeToolOut receives the tool's lines, indented; nil prints them through the output layer, under
// the current section (tests replace it).
var storeToolOut io.Writer

// migrateStore places the bundle's store tool in the app-data folder and runs it on that folder:
// check, then, when check finds work, migrate with the DOOM folder this install went into. It never
// fails the install: a missing, altered or failing tool is a warning that says how to run it by hand.
func migrateStore(bundleRoot, doom string) {
	dir := appDataDir()
	if dir == "" {
		return
	}
	ui.LazySection("Converting mods")
	tool, note := placeStoreTool(bundleRoot, dir)
	if note != "" {
		ui.Warn("%s", note)
	}
	if tool == "" {
		return
	}
	runStoreMigration(tool, dir, doom)
}

// placeStoreTool checks the bundle's tool against TOOLS.sha256 and keeps a copy beside the installer's
// in dir (recorded like the installer copies, so uninstall's rules apply to it). It returns the copy to
// run ("" when there is none) and a warning, if any. A copy the player changed is kept, and the
// bundle's verified copy runs instead.
func placeStoreTool(bundleRoot, dir string) (string, string) {
	src := filepath.Join(bundleRoot, storeToolName)
	if _, err := os.Stat(src); err != nil {
		return "", "this download of Snapmap+ is missing " + storeToolName + ", so your mods were not checked for files " +
			"of earlier releases; Snapmap+ itself is installed, and running the Snapmap+ installer again fetches it."
	}
	want, err := storeToolDigest(bundleRoot)
	if err == nil {
		var got string
		if got, err = fileSHA256(src); err == nil && got != want {
			err = errors.New("its SHA-256 does not match " + storeToolManifest)
		}
	}
	if err != nil {
		return "", fmt.Sprintf("%s in this download of Snapmap+ was not run (%v), so your mods were not checked for "+
			"files of earlier releases; Snapmap+ itself is installed, and running the Snapmap+ installer again fetches it",
			storeToolName, err)
	}
	dst := filepath.Join(dir, storeToolName)
	if err := os.MkdirAll(dir, 0o755); err == nil {
		if err = installSelfCopy(src, dst); err == nil {
			return dst, ""
		}
		return src, fmt.Sprintf("%v; the downloaded copy runs this time", err)
	}
	return src, ""
}

// storeToolDigest reads the tool's lower-case SHA-256 from the bundle's TOOLS.sha256.
func storeToolDigest(bundleRoot string) (string, error) {
	data, err := os.ReadFile(filepath.Join(bundleRoot, storeToolManifest))
	if err != nil {
		return "", fmt.Errorf("this download is missing %s", storeToolManifest)
	}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 2 && strings.EqualFold(filepath.Base(filepath.FromSlash(fields[1])), storeToolName) {
			return strings.ToLower(fields[0]), nil
		}
	}
	return "", fmt.Errorf("%s does not list %s", storeToolManifest, storeToolName)
}

// runStoreTool runs the tool and copies each line it prints to storeToolOut, indented. It returns the
// tool's exit code, or an error when the tool could not be started.
func runStoreTool(tool string, args ...string) (int, error) {
	cmd := exec.Command(tool, args...)
	pr, pw := io.Pipe()
	cmd.Stdout = pw
	cmd.Stderr = pw
	done := make(chan struct{})
	go func() {
		sc := bufio.NewScanner(pr)
		sc.Buffer(make([]byte, 64*1024), 1024*1024)
		for sc.Scan() {
			line := "  " + strings.TrimRight(sc.Text(), "\r")
			if storeToolOut != nil {
				fmt.Fprintln(storeToolOut, line)
			} else {
				ui.Line(line)
			}
		}
		io.Copy(io.Discard, pr)
		close(done)
	}()
	err := cmd.Run()
	pw.Close()
	<-done
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return exit.ExitCode(), nil
	}
	if err != nil {
		return -1, err
	}
	return 0, nil
}
