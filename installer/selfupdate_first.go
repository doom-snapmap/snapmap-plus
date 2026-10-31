package main

import (
	"os"
	"os/exec"
)

// relaunchEnv marks a process the self-update started, so the new image never updates itself again
// in the same run.
const relaunchEnv = "SNAPMAP_PLUS_RELAUNCHED"

// relaunchSelf runs the replaced executable with the original arguments and the console inherited,
// and returns its exit code. Tests point it at a stand-in.
var relaunchSelf = func(exe string, args []string) (int, error) {
	cmd := exec.Command(exe, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	cmd.Env = append(os.Environ(), relaunchEnv+"=1")
	if err := cmd.Run(); err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return ee.ExitCode(), nil
		}
		return 1, err
	}
	return 0, nil
}

// selfUpdateSkipsCommand reports the commands that never need a current installer first: reading the
// version or help, storing a token, and removing the install (an old installer removes what it placed).
func selfUpdateSkipsCommand(cmd string) bool {
	switch cmd {
	case "", "version", "--version", "-v", "help", "--help", "-h", "set-token", "uninstall":
		return cmd != ""
	}
	return false
}

// selfUpdateFirst runs before any other work: when the resolved release ships a newer snapmap-plus.exe,
// the running image is replaced on disk and the new one is started with the same arguments, so the
// mod work is always done by the current installer. It returns true when the caller must exit with
// `code` because the relaunched process did the work. A dev build, an offline (--local) run, a
// relaunched process, --no-self and the commands that need no release stay put. Network trouble is a
// warning, never a stop.
func selfUpdateFirst(rawArgs []string) (relaunched bool, code int) {
	if os.Getenv(relaunchEnv) == "1" || version == "" || version == "dev" {
		return false, 0
	}
	args, _ := stripPlain(rawArgs)
	cmd := ""
	rest := args
	if len(args) > 0 {
		cmd, rest = args[0], args[1:]
	}
	if selfUpdateSkipsCommand(cmd) {
		return false, 0
	}
	f := parseFlags(rest)
	if f.local != "" || f.noSelf {
		return false, 0
	}
	token := resolveToken(f)
	rel, err := fetchRelease(f, token)
	if err != nil {
		ui.Warn("couldn't check for a newer snapmap-plus.exe: %v", err)
		return false, 0
	}
	noteReleaseChecked(rel.TagName)
	if !shouldSelfUpdate(version, rel.TagName) {
		return false, 0
	}
	exe, updated := selfUpdateTo(rel, token)
	if !updated {
		return false, 0
	}
	ui.OK("Updated snapmap-plus.exe to %s; starting it.", rel.TagName)
	ui.Blank()
	restoreConsole()
	code, err = relaunchSelf(exe, rawArgs)
	if err != nil {
		setupOutput(false)
		ui.Warn("the updated snapmap-plus.exe could not be started (%v); continuing with this one", err)
		return false, 0
	}
	return true, code
}
