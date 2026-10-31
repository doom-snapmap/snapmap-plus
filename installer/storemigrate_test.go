package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The stub store tool is this test binary itself: copied into a bundle as snapmap-plus-store.exe and
// run with storeStubEnv set, it answers check and migrate the way that mode says and exits before any
// test runs. Each run appends its arguments to the file storeStubLogEnv names.
const (
	storeStubEnv    = "SNAPMAP_PLUS_STORE_STUB"
	storeStubLogEnv = "SNAPMAP_PLUS_STORE_STUB_LOG"
)

func init() {
	mode := os.Getenv(storeStubEnv)
	if mode == "" {
		return
	}
	if log := os.Getenv(storeStubLogEnv); log != "" {
		if f, err := os.OpenFile(log, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644); err == nil {
			fmt.Fprintln(f, strings.Join(os.Args[1:], "|"))
			f.Close()
		}
	}
	cmd, store := "", ""
	if len(os.Args) > 2 {
		cmd, store = os.Args[1], os.Args[2]
	}
	switch cmd {
	case "check":
		fmt.Println("Checking " + store)
		switch mode {
		case "clean":
			fmt.Println("Nothing to migrate.")
			os.Exit(0)
		case "error":
			fmt.Fprintln(os.Stderr, "error: the store folder "+store+" cannot be read")
			os.Exit(2)
		}
		fmt.Println(`  mods\: 1 folder(s) in the retired layout (bundle.json and .state.json beside assets\)`)
		fmt.Println(`1 kind(s) of work found. Run: snapmap-plus-store migrate "` + store + `"`)
		os.Exit(1)
	case "migrate":
		fmt.Println("Migrating " + store)
		fmt.Println("  store: alex.tiny@1.0.0+1 -> alex.tiny")
		if mode == "fail" {
			fmt.Println("Partly done: the store layout upgraded. 1 kind(s) of work are left")
			os.Exit(1)
		}
		fmt.Println("Done: the store layout upgraded, 3 item(s) moved to the Recycle Bin.")
		os.Exit(0)
	}
	os.Exit(3)
}

// storeBundle makes a bundle folder holding the stub tool and its TOOLS.sha256.
func storeBundle(t *testing.T, withTool bool, digest string) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "dist")
	mkdirAll(t, dir)
	if !withTool {
		return dir
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	if err := copyFile(self, filepath.Join(dir, storeToolName)); err != nil {
		t.Fatal(err)
	}
	if digest == "" {
		if digest, err = fileSHA256(filepath.Join(dir, storeToolName)); err != nil {
			t.Fatal(err)
		}
	}
	writeF(t, filepath.Join(dir, storeToolManifest), strings.ToUpper(digest)+"  "+storeToolName+"\r\n")
	return dir
}

// runMigrateStore runs migrateStore with the stub in `mode` and returns what the installer printed,
// the tool's indented lines, and the stub's argument log.
func runMigrateStore(t *testing.T, bundle, mode string) (printed, tool string, calls []string) {
	t.Helper()
	appdata := filepath.Join(t.TempDir(), "appdata")
	t.Setenv("LOCALAPPDATA", appdata)
	log := filepath.Join(t.TempDir(), "calls.txt")
	t.Setenv(storeStubEnv, mode)
	t.Setenv(storeStubLogEnv, log)
	var out bytes.Buffer
	saved := storeToolOut
	storeToolOut = &out
	defer func() { storeToolOut = saved }()
	printed = captureStdout(t, func() { migrateStore(bundle, `D:\Games\DOOM`) })
	if data, err := os.ReadFile(log); err == nil {
		calls = strings.Split(strings.TrimSpace(string(data)), "\n")
	}
	return printed, out.String(), calls
}

func TestMigrateStoreRunsMigrateWhenCheckFindsWork(t *testing.T) {
	bundle := storeBundle(t, true, "")
	printed, tool, calls := runMigrateStore(t, bundle, "work")
	store := appDataDir()
	if len(calls) != 2 || calls[0] != "check|"+store || calls[1] != `migrate|`+store+`|--doom|D:\Games\DOOM` {
		t.Fatalf("calls = %q", calls)
	}
	for _, want := range []string{"  Checking " + store, "  1 kind(s) of work found.", "  Migrating " + store,
		"  store: alex.tiny@1.0.0+1 -> alex.tiny", "  Done: the store layout upgraded, 3 item(s) moved to the Recycle Bin."} {
		if !strings.Contains(tool, want+"\n") && !strings.Contains(tool, want) {
			t.Errorf("tool output lacks %q:\n%s", want, tool)
		}
	}
	if strings.Contains(printed, "Warning") {
		t.Errorf("unexpected warning: %s", printed)
	}
	// The tool is kept beside the installer copy and recorded like one, so uninstall removes it.
	placed := filepath.Join(store, storeToolName)
	if !identicalFiles(placed, filepath.Join(bundle, storeToolName)) {
		t.Error("the tool was not placed in the app-data folder")
	}
	rec, err := loadExecutableRecord(store)
	if err != nil || rec[storeToolName] == "" {
		t.Errorf("the placed tool is not recorded in %s: %v %v", executableRecordName, rec, err)
	}
	cleanupOwnedExecutables(store, filepath.Join(store, "snapmap-plus.exe"))
	if exists(placed) {
		t.Error("uninstall's cleanup kept the recorded tool")
	}
}

func TestMigrateStoreStopsWhenCheckIsClean(t *testing.T) {
	printed, tool, calls := runMigrateStore(t, storeBundle(t, true, ""), "clean")
	if len(calls) != 1 || !strings.HasPrefix(calls[0], "check|") {
		t.Fatalf("calls = %q", calls)
	}
	if !strings.Contains(tool, "  Nothing to migrate.") || strings.Contains(printed, "Warning") {
		t.Errorf("tool %q printed %q", tool, printed)
	}
}

func TestMigrateStoreWarnsWithoutTheTool(t *testing.T) {
	printed, tool, calls := runMigrateStore(t, storeBundle(t, false, ""), "work")
	if len(calls) != 0 || tool != "" {
		t.Fatalf("a missing tool ran: %q %q", calls, tool)
	}
	if !strings.Contains(printed, "Warning: this Snapmap+ bundle has no snapmap-plus-store.exe") ||
		!strings.Contains(printed, "Snapmap+ itself is installed.") {
		t.Errorf("printed %q", printed)
	}
}

func TestMigrateStoreWarnsWhenMigrateFails(t *testing.T) {
	printed, tool, calls := runMigrateStore(t, storeBundle(t, true, ""), "fail")
	store := appDataDir()
	if len(calls) != 2 {
		t.Fatalf("calls = %q", calls)
	}
	want := `Warning: snapmap-plus-store.exe did not finish migrating your mods folder (exit code 1); Snapmap+ itself is installed. ` +
		"To finish, close DOOM and run:\n" + `  "` + filepath.Join(store, storeToolName) + `" migrate "` + store + `" --doom "D:\Games\DOOM"`
	if !strings.Contains(printed, want) {
		t.Errorf("printed %q, want %q", printed, want)
	}
	if !strings.Contains(tool, "  Partly done") {
		t.Errorf("tool output %q", tool)
	}
}

func TestMigrateStoreWarnsWhenCheckFails(t *testing.T) {
	printed, tool, calls := runMigrateStore(t, storeBundle(t, true, ""), "error")
	if len(calls) != 1 {
		t.Fatalf("calls = %q", calls)
	}
	if !strings.Contains(printed, "Warning: snapmap-plus-store.exe could not check your mods folder (exit code 2)") {
		t.Errorf("printed %q", printed)
	}
	if !strings.Contains(tool, "cannot be read") {
		t.Errorf("the tool's error line was not shown: %q", tool)
	}
}

func TestMigrateStoreRefusesAnAlteredTool(t *testing.T) {
	printed, _, calls := runMigrateStore(t, storeBundle(t, true, strings.Repeat("ab", 32)), "work")
	if len(calls) != 0 {
		t.Fatalf("an altered tool ran: %q", calls)
	}
	if !strings.Contains(printed, "Warning: the bundle's snapmap-plus-store.exe was not run (its SHA-256 does not match TOOLS.sha256)") {
		t.Errorf("printed %q", printed)
	}
	if exists(filepath.Join(appDataDir(), storeToolName)) {
		t.Error("an altered tool was placed")
	}
}

// A copy the player changed stays; the bundle's verified copy runs instead.
func TestMigrateStoreKeepsAChangedCopy(t *testing.T) {
	bundle := storeBundle(t, true, "")
	appdata := filepath.Join(t.TempDir(), "appdata")
	store := filepath.Join(appdata, "snapmap-plus")
	writeF(t, filepath.Join(store, storeToolName), "the player's own file")
	t.Setenv("LOCALAPPDATA", appdata)
	t.Setenv(storeStubEnv, "clean")
	saved := storeToolOut
	storeToolOut = io.Discard
	defer func() { storeToolOut = saved }()
	printed := captureStdout(t, func() { migrateStore(bundle, `D:\Games\DOOM`) })
	if got := readF(t, filepath.Join(store, storeToolName)); got != "the player's own file" {
		t.Errorf("the player's file was replaced: %q", got)
	}
	if !strings.Contains(printed, "the bundle's copy runs this time") {
		t.Errorf("printed %q", printed)
	}
}

func TestStoreToolDigestReadsTheList(t *testing.T) {
	dir := t.TempDir()
	writeF(t, filepath.Join(dir, storeToolManifest), "ABCDEF  other.exe\nC0FFEE  snapmap-plus-store.exe\n")
	if got, err := storeToolDigest(dir); err != nil || got != "c0ffee" {
		t.Errorf("digest = %q, %v", got, err)
	}
	if _, err := storeToolDigest(t.TempDir()); err == nil {
		t.Error("a bundle without TOOLS.sha256 gave a digest")
	}
}
