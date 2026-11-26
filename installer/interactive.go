package main

import (
	"bufio"
	"errors"
	"net/http"
	"os"
	"strings"
	"time"
)

var stdin = bufio.NewReader(os.Stdin)

// interactiveMain opens the no-argument prompt. Enter installs when absent or
// updates when a newer release is known; named commands remain available.
func interactiveMain() {
	ui.Section("snapmap-plus " + version + " (installer)")

	rec, err := loadRecord()
	installed := err == nil
	installedVersion := ""
	if installed {
		installedVersion = rec.Version
		ui.Field("Version", rec.Version)
		ui.PathField("Folder", rec.DoomPath)
	} else {
		ui.Note("Snapmap+ is not installed yet.")
		if doom, derr := resolveDoom(""); derr == nil {
			ui.PathField("Found DOOM", doom)
		} else {
			ui.Warn("couldn't find DOOM automatically -- you can type:  install --doom \"C:\\path\\to\\DOOM\"")
		}
	}

	latest := ""
	if installed {
		latest = latestDefaultTag()
	}
	enter := enterDefault(installed, installedVersion, latest)
	legacyFix := false
	if installed {
		if b, err := scanDoomExes(rec.DoomPath).identify(rec.DoomPath); err == nil && legacyNeedsUpdate(b, installedVersion) {
			enter, legacyFix = "update", true
		}
	}
	switch {
	case legacyFix:
		ui.Blank()
		ui.Note("Your DOOM is the %s, whose release is Snapmap+ %s; the install record says %s.",
			label2018, legacyReleaseTag, installedVersion)
		ui.Note("Press Enter to update now (this restores that release's files only), or type a command.")
		commandLoop(enter)
		return
	}
	switch enter {
	case "install":
		ui.Blank()
		ui.Note("Press Enter to install now, or type a command (help lists them).")
	case "update":
		ui.Blank()
		ui.Note("A new version of Snapmap+ is available: %s (you have %s).", latest, installedVersion)
		ui.Note("Press Enter to update now, or type a command (help lists them).")
	default:
		if installed && latest != "" {
			ui.OK("You're up to date.")
		}
		ui.Blank()
		ui.Note("Type a command (help lists them), or quit to close.")
	}
	commandLoop(enter)
}

// enterDefault selects install, update (only to a newer version), or no action for a bare Enter.
func enterDefault(installed bool, installedVersion, latestTag string) string {
	if !installed {
		return "install"
	}
	if c, ok := compareVersions(latestTag, installedVersion); ok && c > 0 {
		return "update"
	}
	return ""
}

// legacyNeedsUpdate reports a 2018 install whose record names a release other than legacyReleaseTag;
// the next update restores a clean legacyReleaseTag install.
func legacyNeedsUpdate(b doomBuild, installedVersion string) bool {
	return b.build == build2018 && installedVersion != legacyReleaseTag
}

// commandLoop reads and runs commands until quit / EOF. enterCmd is what the first bare Enter runs
// (the pre-selected obvious action); after firing once, a bare Enter just re-prompts.
func commandLoop(enterCmd string) {
	for {
		ui.CommandPrompt("snapmap-plus>")
		line, ok := readLineEOF()
		ui.Answered(!ok)
		if !ok {
			return // stdin closed (piped input ran out) -- never spin on EOF
		}
		fields := splitArgs(line)
		if len(fields) == 0 {
			if enterCmd == "" {
				continue
			}
			fields = []string{enterCmd}
		}
		enterCmd = ""
		cmd := fields[0]
		if cmd == "quit" || cmd == "exit" || cmd == "q" {
			return
		}
		if err := runCommand(cmd, fields[1:], false); err != nil {
			if errors.Is(err, errUnknownCommand) {
				ui.Warn("Unknown command %q -- type help for the list, or quit to close.", cmd)
				continue
			}
			ui.Fail("%v", err)
		}
	}
}

// splitArgs keeps double-quoted paths together and removes their delimiters.
func splitArgs(line string) []string {
	var out []string
	var cur strings.Builder
	inQuote := false
	flush := func() {
		if cur.Len() > 0 {
			out = append(out, cur.String())
			cur.Reset()
		}
	}
	for _, r := range line {
		switch {
		case r == '"':
			inQuote = !inQuote
		case !inQuote && (r == ' ' || r == '\t'):
			flush()
		default:
			cur.WriteRune(r)
		}
	}
	flush()
	return out
}

// noticeClient bounds the best-effort update check -- a double-click must never hang on a dead network.
var noticeClient = &http.Client{Timeout: 5 * time.Second}

// latestDefaultTag returns the default release tag, or an empty string when
// the bounded update check fails. Failure suppresses only the notice.
func latestDefaultTag() string {
	var list []ghRelease
	if apiGetWith(noticeClient, releasesURL()+"?per_page=100", resolveToken(flags{}), &list) != nil {
		return ""
	}
	sortReleases(list)
	if pick := newestRelease(list, false); pick != nil {
		return pick.TagName
	}
	return ""
}

// readLineEOF reads a line and additionally reports whether stdin is still open -- false means EOF with
// nothing read (the interactive loop must stop rather than treat EOF as an endless bare Enter).
func readLineEOF() (string, bool) {
	s, err := stdin.ReadString('\n')
	if err != nil && s == "" {
		return "", false
	}
	return cleanLine(s), true
}

// cleanLine trims whitespace and any UTF-8 BOM (U+FEFF) that pasted or piped input can carry.
func cleanLine(s string) string {
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(s, string(rune(0xFEFF)))
	return strings.TrimSpace(s)
}

var confirmationInput = openConfirmationInput

func confirm(yes bool, prompt string) bool {
	if yes {
		return true
	}
	input := confirmationInput()
	if input == nil {
		return false
	}
	defer input.Close()
	ui.Ask(prompt)
	line, err := bufio.NewReader(input).ReadString('\n')
	ui.Answered(err != nil)
	return err == nil && strings.EqualFold(cleanLine(line), "y")
}
