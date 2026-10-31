package main

import (
	"bufio"
	"encoding/json"
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
	ui.Section("Snapmap+ " + version)

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
		if b, err := detectDoomBuild(rec.DoomPath); err == nil && legacyNeedsUpdate(b, installedVersion) {
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

// enterDefault selects install, update, or no action for a bare Enter.
func enterDefault(installed bool, installedVersion, latestTag string) string {
	if !installed {
		return "install"
	}
	if latestTag != "" && latestTag != installedVersion {
		return "update"
	}
	return ""
}

// legacyNeedsUpdate reports a 2018 install whose record names another release, which an earlier
// installer's update leaves behind: the next update restores a clean legacyReleaseTag install.
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
		if err := runCommand(cmd, fields[1:]); err != nil {
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
	req, err := http.NewRequest("GET", releasesURL()+"?per_page=30", nil)
	if err != nil {
		return ""
	}
	req.Header.Set("User-Agent", "snapmap-plus-installer")
	req.Header.Set("Accept", "application/vnd.github+json")
	if t := resolveToken(flags{}); t != "" {
		req.Header.Set("Authorization", "Bearer "+t)
	}
	resp, err := noticeClient.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ""
	}
	var list []ghRelease
	if json.NewDecoder(resp.Body).Decode(&list) != nil {
		return ""
	}
	if pick, _ := pickDefaultRelease(list); pick != nil {
		return pick.TagName
	}
	return ""
}

func readLine() string {
	s, _ := stdin.ReadString('\n')
	return cleanLine(s)
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

func isYes(s string) bool {
	s = strings.ToLower(strings.TrimSpace(s))
	return s == "" || s == "y" || s == "yes"
}

func pause() {
	ui.Ask("Press Enter to close.", "")
	readLine()
	ui.Answered(false)
}

// isInteractive reports whether stdin is a real terminal -- so we only prompt when a human can answer.
// Scripts / CI / piped input skip the confirmation automatically; force-skip with --yes.
// Tests replace it so a console-attached test run never waits on a prompt.
var isInteractive = func() bool {
	fi, err := os.Stdin.Stat()
	return err == nil && (fi.Mode()&os.ModeCharDevice) != 0
}

// confirm prints a yes/no prompt and returns true on yes (Enter / "y" / "yes"). Input that ends
// before an answer counts as no.
func confirm(prompt string) bool {
	ui.Ask(prompt, "[Y/n]")
	answer, open := readLineEOF()
	ui.Answered(!open)
	if !open {
		return false
	}
	return isYes(answer)
}
