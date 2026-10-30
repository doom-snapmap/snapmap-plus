package main

import (
	"bufio"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCompareVersions(t *testing.T) {
	for _, c := range []struct {
		a, b string
		want int
		ok   bool
	}{
		{"v0.2.1-beta.9", "v0.2.1-beta.10", -1, true},
		{"v0.2.1-beta.20", "v0.2.1-beta.19", 1, true},
		{"v0.2.1", "v0.2.1-beta.20", 1, true},
		{"v0.3.0-beta.1", "v0.2.1", 1, true},
		{"v0.2.1-beta.19.1", "v0.2.1-beta.20", -1, true},
		{"v1.0.0-alpha", "v1.0.0-alpha.1", -1, true},
		{"v1.0.0-1", "v1.0.0-alpha", -1, true},
		{"v0.2.1-beta.20", "v0.2.1-beta.20", 0, true},
		{"dev", "v0.2.1", 0, false},
	} {
		got, ok := compareVersions(c.a, c.b)
		if got != c.want || ok != c.ok {
			t.Errorf("compareVersions(%q, %q) = %d, %v; want %d, %v", c.a, c.b, got, ok, c.want, c.ok)
		}
	}
}

// An older downloaded installer refuses to replace a newer stable copy unless --force is given;
// a newer one replaces an older copy as before.
func TestOlderInstallerKeepsANewerStableCopy(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("install with beta.2: %v", err)
	}
	stable := filepath.Join(r.data, selfExeAsset)
	if copyVersionOf(stable) != "v1.0.0-beta.2" {
		t.Fatalf("stable copy version = %q", copyVersionOf(stable))
	}

	writeF(t, r.exe, string(installerBytes("v1.0.0-beta.1")))
	version = "v1.0.0-beta.1"
	err := cmdInstall(flags{yes: true, doom: r.doom})
	if err == nil || !strings.Contains(err.Error(), "v1.0.0-beta.1, older than the v1.0.0-beta.2") || !strings.Contains(err.Error(), "--force") {
		t.Fatalf("older installer: %v", err)
	}
	if readF(t, stable) != string(installerBytes("v1.0.0-beta.2")) {
		t.Fatal("an older installer replaced the newer stable copy")
	}
	if rec, _ := loadRecord(); rec == nil || readF(t, filepath.Join(r.doom, "XINPUT1_4.dll")) != "backend-v1.0.0-beta.2" {
		t.Fatal("the refused install changed the DOOM folder")
	}

	if err := cmdInstall(flags{yes: true, doom: r.doom, force: true}); err != nil {
		t.Fatalf("--force: %v", err)
	}
	if readF(t, stable) != string(installerBytes("v1.0.0-beta.1")) || copyVersionOf(stable) != "v1.0.0-beta.1" {
		t.Fatal("--force did not replace the stable copy")
	}

	writeF(t, r.exe, string(installerBytes("v1.0.0-beta.3")))
	version = "v1.0.0-beta.3"
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("newer installer: %v", err)
	}
	if copyVersionOf(stable) != "v1.0.0-beta.3" {
		t.Fatal("a newer installer did not replace the older stable copy")
	}
	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatal(err)
	}
	if exists(filepath.Join(r.data, executableVersionsName)) {
		t.Error("uninstall left installer-versions.json for a removed copy")
	}
}

// A copy written by an earlier installer has no recorded version and is replaced as before.
func TestUnrecordedStableCopyIsReplaced(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	stable := filepath.Join(r.data, selfExeAsset)
	writeF(t, filepath.Join(r.tmp, "earlier.exe"), "an earlier installer")
	if err := installSelfCopy(filepath.Join(r.tmp, "earlier.exe"), stable); err != nil {
		t.Fatal(err)
	}
	if err := selfInstall(false); err != nil {
		t.Fatalf("selfInstall: %v", err)
	}
	if readF(t, stable) != string(installerBytes("v1.0.0-beta.1")) {
		t.Fatal("an unrecorded owned copy was not replaced")
	}
}

func withStdin(t *testing.T, input string) {
	t.Helper()
	saved := stdin
	stdin = bufio.NewReader(strings.NewReader(input))
	t.Cleanup(func() { stdin = saved })
}

// Input that ends before an answer is a no; Enter, y and yes are a yes.
func TestConfirmTreatsEndOfInputAsNo(t *testing.T) {
	for input, want := range map[string]bool{"": false, "\n": true, "y\n": true, "yes": true, "n\n": false, "no": false} {
		withStdin(t, input)
		if got := confirm("Continue?"); got != want {
			t.Errorf("confirm with input %q = %v, want %v", input, got, want)
		}
	}
}

func captureStdout(t *testing.T, fn func()) string {
	t.Helper()
	rd, wr, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	saved := os.Stdout
	os.Stdout = wr
	done := make(chan string)
	go func() { b, _ := io.ReadAll(rd); done <- string(b) }()
	fn()
	os.Stdout = saved
	wr.Close()
	return <-done
}

// An update resolves the release twice; the newest-beta fallback is announced once.
func TestBetaFallbackNoticePrintsOnce(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatal(err)
	}
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	betaFallbackNoted = false
	t.Cleanup(func() { betaFallbackNoted = false })
	out := captureStdout(t, func() {
		if err := cmdUpdate(flags{yes: true}); err != nil {
			t.Errorf("update: %v", err)
		}
	})
	if n := strings.Count(out, "No stable release has been published yet"); n != 1 {
		t.Errorf("fallback notice printed %d times:\n%s", n, out)
	}
	if !strings.Contains(out, "Updated snapmap-plus.exe to v1.0.0-beta.2") {
		t.Errorf("update did not self-update:\n%s", out)
	}
}
