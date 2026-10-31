package main

import (
	"io"
	"os"
	"strings"
	"testing"
)

// captureStderr mirrors captureStdout for the warnings the output layer sends to stderr.
func captureStderr(t *testing.T, fn func()) string {
	t.Helper()
	rd, wr, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	saved := os.Stderr
	os.Stderr = wr
	done := make(chan string)
	go func() { b, _ := io.ReadAll(rd); done <- string(b) }()
	fn()
	os.Stderr = saved
	wr.Close()
	return <-done
}

// stubRelaunch records the relaunch instead of starting a process.
func stubRelaunch(t *testing.T) *struct {
	calls int
	exe   string
	args  []string
} {
	t.Helper()
	rec := &struct {
		calls int
		exe   string
		args  []string
	}{}
	saved := relaunchSelf
	relaunchSelf = func(exe string, args []string) (int, error) {
		rec.calls++
		rec.exe, rec.args = exe, args
		return 7, nil
	}
	t.Cleanup(func() { relaunchSelf = saved })
	return rec
}

// A stale installer replaces itself before any other work and relaunches the new image with the
// original arguments; the caller exits with the relaunched process's code.
func TestSelfUpdateFirstReplacesAndRelaunches(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	rec := stubRelaunch(t)
	out := captureStdout(t, func() {
		relaunched, code := selfUpdateFirst([]string{"update", "--yes"})
		if !relaunched || code != 7 {
			t.Fatalf("relaunched=%v code=%d, want true 7", relaunched, code)
		}
	})
	if rec.calls != 1 || rec.exe != r.exe || strings.Join(rec.args, " ") != "update --yes" {
		t.Fatalf("relaunch calls=%d exe=%q args=%q", rec.calls, rec.exe, rec.args)
	}
	got, err := os.ReadFile(r.exe)
	if err != nil || string(got) != string(installerBytes("v1.0.0-beta.2")) {
		t.Fatalf("exe on disk = %q, %v; want the beta.2 bytes", got, err)
	}
	if !strings.Contains(out, "Updated snapmap-plus.exe to v1.0.0-beta.2; starting it.") {
		t.Fatalf("output lacks the update line:\n%s", out)
	}
}

// Nothing newer, a dev build, a relaunched process, an offline run and the release-free commands all
// stay in this process without touching the exe.
func TestSelfUpdateFirstStaysPut(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	rec := stubRelaunch(t)
	check := func(name string, args []string) {
		t.Helper()
		captureStdout(t, func() {
			if relaunched, _ := selfUpdateFirst(args); relaunched {
				t.Fatalf("%s: relaunched", name)
			}
		})
		if rec.calls != 0 {
			t.Fatalf("%s: relaunch called", name)
		}
		got, _ := os.ReadFile(r.exe)
		if string(got) != string(installerBytes("v1.0.0-beta.1")) {
			t.Fatalf("%s: exe changed", name)
		}
	}
	check("current release", []string{"update"})
	check("interactive, current release", nil)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	check("version command", []string{"version"})
	check("help", []string{"--help"})
	check("set-token", []string{"set-token", "x"})
	check("uninstall", []string{"uninstall"})
	check("offline install", []string{"install", "--local", r.tmp})
	check("--no-self", []string{"update", "--no-self"})
	t.Setenv(relaunchEnv, "1")
	check("relaunched process", []string{"update"})
	t.Setenv(relaunchEnv, "")
	savedVersion := version
	version = "dev"
	check("dev build", []string{"update"})
	version = savedVersion
}

// A release server that cannot be reached is a warning; the run goes on with this installer.
func TestSelfUpdateFirstWarnsWhenOffline(t *testing.T) {
	newUpdateRig(t, "v1.0.0-beta.1")
	t.Setenv(apiBaseEnv, "http://127.0.0.1:1")
	rec := stubRelaunch(t)
	out := captureStderr(t, func() {
		if relaunched, _ := selfUpdateFirst([]string{"update"}); relaunched {
			t.Fatal("relaunched")
		}
	})
	if rec.calls != 0 || !strings.Contains(out, "couldn't check for a newer snapmap-plus.exe") {
		t.Fatalf("calls=%d output:\n%s", rec.calls, out)
	}
}
