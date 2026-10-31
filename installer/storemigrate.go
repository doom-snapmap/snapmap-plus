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
// releases, retires removed-mods\ and moves overrides\ into mods\. It ships in the bundle beside
// MANIFEST.sha256 but not in it: every installer installs each manifest-listed file into DOOM, and
// the tool belongs next to snapmap-plus.exe in %LOCALAPPDATA%\snapmap-plus. Its digest is in
// TOOLS.sha256 instead.
const (
	storeToolName     = "snapmap-plus-store.exe"
	storeToolManifest = "TOOLS.sha256"
)

// storeToolOut receives the tool's lines, indented (tests replace it).
var storeToolOut io.Writer = os.Stdout

// migrateStore places the bundle's store tool in the app-data folder and runs it on that folder:
// check, then, when check finds work, migrate with the DOOM folder this install went into. It never
// fails the install: a missing, altered or failing tool is a warning that says how to run it by hand.
func migrateStore(bundleRoot, doom string) {
	dir := appDataDir()
	if dir == "" {
		return
	}
	tool, note := placeStoreTool(bundleRoot, dir)
	if note != "" {
		fmt.Println("Warning: " + note)
	}
	if tool == "" {
		return
	}
	rerun := fmt.Sprintf(`"%s" migrate "%s" --doom "%s"`, filepath.Join(dir, storeToolName), dir, doom)
	code, err := runStoreTool(tool, "check", dir)
	switch {
	case err != nil:
		fmt.Printf("Warning: %s could not be run (%v), so your mods folder was not checked for files of earlier releases. "+
			"To do it by hand, close DOOM and run:\n  %s\n", storeToolName, err, rerun)
		return
	case code == 0:
		return
	case code != 1:
		fmt.Printf("Warning: %s could not check your mods folder (exit code %d). To try again, close DOOM and run:\n  %s\n",
			storeToolName, code, rerun)
		return
	}
	code, err = runStoreTool(tool, "migrate", dir, "--doom", doom)
	switch {
	case err != nil:
		fmt.Printf("Warning: %s could not be run (%v), so your mods folder was not migrated. To do it by hand, close DOOM "+
			"and run:\n  %s\n", storeToolName, err, rerun)
	case code != 0:
		fmt.Printf("Warning: %s did not finish migrating your mods folder (exit code %d); Snapmap+ itself is installed. "+
			"To finish, close DOOM and run:\n  %s\n", storeToolName, code, rerun)
	}
}

// placeStoreTool checks the bundle's tool against TOOLS.sha256 and keeps a copy beside the installer's
// in dir (recorded like the installer copies, so uninstall's rules apply to it). It returns the copy to
// run ("" when there is none) and a warning, if any. A copy the player changed is kept, and the
// bundle's verified copy runs instead.
func placeStoreTool(bundleRoot, dir string) (string, string) {
	src := filepath.Join(bundleRoot, storeToolName)
	if _, err := os.Stat(src); err != nil {
		return "", "this Snapmap+ bundle has no " + storeToolName + ", so your mods folder was not checked for files of " +
			"earlier releases. Snapmap+ itself is installed."
	}
	want, err := storeToolDigest(bundleRoot)
	if err == nil {
		var got string
		if got, err = fileSHA256(src); err == nil && got != want {
			err = errors.New("its SHA-256 does not match " + storeToolManifest)
		}
	}
	if err != nil {
		return "", fmt.Sprintf("the bundle's %s was not run (%v), so your mods folder was not checked for files of "+
			"earlier releases. Snapmap+ itself is installed.", storeToolName, err)
	}
	dst := filepath.Join(dir, storeToolName)
	if err := os.MkdirAll(dir, 0o755); err == nil {
		if err = installSelfCopy(src, dst); err == nil {
			return dst, ""
		}
		return src, fmt.Sprintf("kept the existing %s (%v); the bundle's copy runs this time", dst, err)
	}
	return src, ""
}

// storeToolDigest reads the tool's lower-case SHA-256 from the bundle's TOOLS.sha256.
func storeToolDigest(bundleRoot string) (string, error) {
	data, err := os.ReadFile(filepath.Join(bundleRoot, storeToolManifest))
	if err != nil {
		return "", fmt.Errorf("the bundle has no %s", storeToolManifest)
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
			fmt.Fprintln(storeToolOut, "  "+strings.TrimRight(sc.Text(), "\r"))
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
