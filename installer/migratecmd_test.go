package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// runCmdMigrate runs the migrate command with the stub tool kept in a fresh app-data folder (unless
// withTool is false) in `mode`, and returns what the installer printed, the tool's indented lines, the
// stub's argument log and the command's error.
func runCmdMigrate(t *testing.T, withTool bool, mode string, f flags) (printed, tool string, calls []string, err error) {
	return runCmdMigrateTool(t, withTool, true, mode, f)
}

// runCmdMigrateTool is runCmdMigrate with the tool recorded in install.json as the copy an install
// placed, or not.
func runCmdMigrateTool(t *testing.T, withTool, recorded bool, mode string, f flags) (printed, tool string, calls []string, err error) {
	t.Helper()
	appdata := filepath.Join(t.TempDir(), "appdata")
	t.Setenv("LOCALAPPDATA", appdata)
	if withTool {
		self, e := os.Executable()
		if e != nil {
			t.Fatal(e)
		}
		mkdirAll(t, appDataDir())
		placed := filepath.Join(appDataDir(), storeToolName)
		if e := copyFile(self, placed); e != nil {
			t.Fatal(e)
		}
		if recorded {
			sum, e := fileSHA256(placed)
			if e == nil {
				e = saveExecutableRecord(appDataDir(), executableRecord{storeToolName: sum})
			}
			if e != nil {
				t.Fatal(e)
			}
		}
	}
	log := filepath.Join(t.TempDir(), "calls.txt")
	t.Setenv(storeStubEnv, mode)
	t.Setenv(storeStubLogEnv, log)
	previous := checkDoomRunning
	checkDoomRunning = func() bool { return false }
	t.Cleanup(func() { checkDoomRunning = previous })
	var out bytes.Buffer
	saved := storeToolOut
	storeToolOut = &out
	defer func() { storeToolOut = saved }()
	var stdout string
	errOut := captureStderr(t, func() { stdout = captureStdout(t, func() { err = cmdMigrate(f) }) })
	printed = stdout + errOut
	if data, e := os.ReadFile(log); e == nil {
		calls = strings.Split(strings.TrimSpace(string(data)), "\n")
	}
	return printed, out.String(), calls, err
}

func TestCmdMigrateRunsTheKeptToolWithoutReinstalling(t *testing.T) {
	printed, tool, calls, err := runCmdMigrate(t, true, "work", flags{doom: `D:\Games\DOOM`})
	if err != nil {
		t.Fatalf("migrate failed: %v\n%s", err, printed)
	}
	store := appDataDir()
	if len(calls) != 2 || calls[0] != "check|"+store || calls[1] != `migrate|`+store+`|--doom|D:\Games\DOOM` {
		t.Fatalf("calls = %q", calls)
	}
	for _, want := range []string{"Converting mods", "Mods folder converted."} {
		if !strings.Contains(printed, want) {
			t.Errorf("output lacks %q:\n%s", want, printed)
		}
	}
	if !strings.Contains(tool, "  Done: the store layout upgraded") {
		t.Errorf("tool output lacks the migrate result:\n%s", tool)
	}
}

func TestCmdMigrateReportsNothingToConvert(t *testing.T) {
	printed, _, calls, err := runCmdMigrate(t, true, "clean", flags{doom: `D:\Games\DOOM`})
	if err != nil || len(calls) != 1 || !strings.HasPrefix(calls[0], "check|") {
		t.Fatalf("err %v calls %q", err, calls)
	}
	if !strings.Contains(printed, "Nothing to convert.") {
		t.Errorf("output lacks the clean result:\n%s", printed)
	}
}

func TestCmdMigrateFailsWhenTheToolDidNotFinish(t *testing.T) {
	printed, _, _, err := runCmdMigrate(t, true, "fail", flags{doom: `D:\Games\DOOM`})
	if err == nil {
		t.Fatalf("a partial migration passed:\n%s", printed)
	}
	if !strings.Contains(printed, "did not finish moving your mods") {
		t.Errorf("output lacks the warning naming the command:\n%s", printed)
	}
}

func TestCmdMigrateWithoutTheToolPointsAtUpdate(t *testing.T) {
	_, _, calls, err := runCmdMigrate(t, false, "work", flags{doom: `D:\Games\DOOM`})
	if err == nil || !strings.Contains(err.Error(), "snapmap-plus update") {
		t.Fatalf("err = %v", err)
	}
	if len(calls) != 0 {
		t.Errorf("the missing tool was run: %q", calls)
	}
}

func TestCmdMigrateWithoutAnInstallOrDoomFlagFails(t *testing.T) {
	_, _, calls, err := runCmdMigrate(t, true, "work", flags{})
	if err == nil || !strings.Contains(err.Error(), "--doom") {
		t.Fatalf("err = %v", err)
	}
	if len(calls) != 0 {
		t.Errorf("the tool ran without a DOOM folder: %q", calls)
	}
}

// A store tool the install record does not list as placed by Snapmap+ is never run.
func TestCmdMigrateRefusesAnUnrecordedTool(t *testing.T) {
	_, _, calls, err := runCmdMigrateTool(t, true, false, "work", flags{doom: `D:\Games\DOOM`})
	if err == nil || !strings.Contains(err.Error(), "is not the copy Snapmap+ placed") {
		t.Fatalf("err = %v", err)
	}
	if len(calls) != 0 {
		t.Errorf("an unrecorded tool was run: %q", calls)
	}
}
