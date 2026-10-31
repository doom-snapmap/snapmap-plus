// Snapmap+ installer: discover DOOM, verify and deploy the bundle, and record
// backups for uninstall. Runtime dependencies are limited to the Go standard library.
package main

import (
	"errors"
	"os"
)

// version is set at build time: `go build -ldflags "-X main.version=v1.2.3" -o snapmap-plus.exe .`
var version = "dev"

func usage() {
	ui.Help(`snapmap-plus -- the Snapmap+ installer (`+version+`)

Usage:
  snapmap-plus install   [--doom <path>] [--local <dist-dir>] [--release <tag>] [--beta] [--force] [--yes]
  snapmap-plus update    [--doom <path>] [--release <tag>] [--beta] [--no-self] [--force] [--yes]
  snapmap-plus uninstall [--doom <path>] [--yes]
  snapmap-plus changelog [<version>|latest|all]
  snapmap-plus status
  snapmap-plus version
  snapmap-plus help

Options:
  --doom <path>       Your DOOM 2016 folder (the one with DOOMx64vk.exe and DOOMx64.exe).
                      Auto-detected from your Steam libraries if omitted.
  --local <dist-dir>  Install from a local dist/ tree (built by package.ps1) instead of
                      downloading a release.
  --release <tag>     Install a specific release version instead of the latest.
  --beta              Install the latest beta (pre-release) instead of the latest stable.
  --no-self           With "update": don't also update snapmap-plus.exe itself (overlay only).
  --force             Let this snapmap-plus.exe replace a newer copy kept in %LOCALAPPDATA%\snapmap-plus.
  --yes, -y           Skip the "are you sure?" confirmation (for scripts / automation).
  --plain             Print without colour. Colour is also off when NO_COLOR is set or the
                      output is not a console.

With "changelog", a bare version shows just that release (the leading v is optional):
  snapmap-plus changelog v0.2.1-beta.4    that release's notes
  snapmap-plus changelog latest           the newest stable release
  snapmap-plus changelog all              every release in full
With no argument it shows the newest release and lists the earlier ones.

With no --local, install/update download from GitHub. Uninstall restores any files it
replaced and leaves your Snapmap+ modding data untouched.

The installer never changes your mods folder or the package folders of earlier releases;
Snapmap+ converts them itself the first time DOOM starts with it.

Running snapmap-plus with no arguments (a double-click) opens an interactive prompt: it offers
to install (or to update when a newer version is out), and takes any command above.
`, "Usage:", "Options:")
}

func main() {
	args, plain := stripPlain(os.Args[1:])
	setupOutput(plain)
	cleanupSelfUpdateLeftovers() // remove any <exe>.old a prior self-update left behind
	if len(args) < 1 {
		// No arguments opens the interactive prompt used when double-clicking the executable.
		interactiveMain()
		restoreConsole()
		return
	}
	cmd := args[0]
	err := runCommand(cmd, args[1:])
	if errors.Is(err, errUnknownCommand) {
		ui.Fail("unknown command %q", cmd)
		ui.Blank()
		usage()
		restoreConsole()
		os.Exit(2)
	}
	if err != nil {
		ui.Fail("%v", err)
		restoreConsole()
		os.Exit(1)
	}
	restoreConsole()
}

// errUnknownCommand distinguishes "no such command" from a command that ran and failed.
var errUnknownCommand = errors.New("unknown command")

// runCommand dispatches one command; shared by the CLI entry point and the interactive prompt so both
// surfaces stay identical.
func runCommand(cmd string, args []string) error {
	args, plain := stripPlain(args)
	ui.setPlain(plain)
	for _, arg := range args {
		if arg == "--help" || arg == "-h" {
			usage()
			return nil
		}
	}
	f := parseFlags(args)
	switch cmd {
	case "install":
		return cmdInstall(f)
	case "update":
		return cmdUpdate(f)
	case "uninstall":
		return cmdUninstall(f)
	case "status":
		return cmdStatus(f)
	case "changelog", "info", "--info":
		return cmdChangelog(f, args)
	case "set-token":
		return cmdSetToken(args)
	case "version", "--version", "-v":
		cmdVersion()
		return nil
	case "help", "-h", "--help":
		usage()
		return nil
	}
	return errUnknownCommand
}

// flags holds the parsed command-line options.
type flags struct {
	doom    string
	local   string
	release string
	beta    bool
	token   string
	yes     bool
	noSelf  bool // with `update`: skip refreshing snapmap-plus.exe itself
	force   bool // replace a newer stable copy of snapmap-plus.exe with this older one
}

// parseFlags is a tiny "--key value" parser (the tool's option surface is small + fixed).
func parseFlags(a []string) flags {
	var f flags
	for i := 0; i < len(a); i++ {
		switch a[i] {
		case "--doom":
			if i+1 < len(a) {
				i++
				f.doom = a[i]
			}
		case "--local":
			if i+1 < len(a) {
				i++
				f.local = a[i]
			}
		case "--release":
			if i+1 < len(a) {
				i++
				f.release = a[i]
			}
		case "--beta":
			f.beta = true
		case "--no-self":
			f.noSelf = true
		case "--force":
			f.force = true
		case "--yes", "-y":
			f.yes = true
		case "--token":
			if i+1 < len(a) {
				i++
				f.token = a[i]
			}
		}
	}
	return f
}
