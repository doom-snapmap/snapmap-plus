// snaphak -- the open-snaphak installer.
//
// A single static Windows CLI that installs / updates / removes the SnapHak clone overlay in a DOOM 2016
// install, with backup and an uninstall that restores vanilla. It is NOT the dev differential tool -- it does
// the clone only: detect DOOM, deploy the 6-file overlay (or download the latest release), and keep a record
// so uninstall reverses exactly what it placed. Stdlib only, no external dependencies.
package main

import (
	"fmt"
	"os"
)

// version is set at build time: `go build -ldflags "-X main.version=v1.2.3" -o snaphak.exe .`
var version = "dev"

func usage() {
	fmt.Print(`snaphak -- open-snaphak installer (` + version + `)

Usage:
  snaphak install   [--doom <path>] [--local <dist-dir>] [--release <tag>]
  snaphak update    [--doom <path>] [--release <tag>]
  snaphak uninstall [--doom <path>]
  snaphak status
  snaphak version

Options:
  --doom <path>       The DOOM install dir (the folder with DOOMx64vk.exe).
                      Auto-detected from your Steam libraries if omitted.
  --local <dist-dir>  Install from a local dist/ tree (built by package.ps1) instead of
                      downloading a GitHub release.
  --release <tag>     Install a specific release tag instead of the latest.

With no --local, install/update download the latest release bundle from GitHub.
Uninstall restores any files it replaced and leaves your %USERPROFILE%\snaphak data untouched.
`)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	cmd := os.Args[1]
	f := parseFlags(os.Args[2:])

	var err error
	switch cmd {
	case "install":
		err = cmdInstall(f)
	case "update":
		err = cmdUpdate(f)
	case "uninstall":
		err = cmdUninstall(f)
	case "status":
		err = cmdStatus(f)
	case "version", "--version", "-v":
		fmt.Println("snaphak", version)
	case "help", "-h", "--help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n", cmd)
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

// flags holds the parsed command-line options.
type flags struct {
	doom    string
	local   string
	release string
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
		}
	}
	return f
}
