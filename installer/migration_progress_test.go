package main

import (
	"bufio"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestMigrationShowsCurrentWorkAndStopsBeforeConsent(t *testing.T) {
	oldUI, oldInput, oldInteractive := ui, stdin, isInteractive
	t.Cleanup(func() { ui, stdin, isInteractive = oldUI, oldInput, oldInteractive })
	c, output := testConsole(false)
	ui = c
	progress := startMigrationProgress()
	progress.step("Checking DOOM save slots")
	progress.detail("Map\nTitle")
	progress.report(time.Now().Add(8 * time.Second))
	progress.stop()
	if !strings.Contains(output.String(), "Checking DOOM save slots  Map Title  (8s)") {
		t.Fatal(output.String())
	}

	p := &migrationPlan{data: t.TempDir(), work: t.TempDir(), Maps: 1}
	p.keepMissingPins(filepath.Join(t.TempDir(), "Room.json"), &missingEarlierPins{items: []missingEarlierPin{
		{mod: "doors", build: strings.Repeat("a", 64)}, {mod: "doors", build: strings.Repeat("b", 64)},
	}})
	p.reportDeferred()
	stdin = bufio.NewReader(strings.NewReader("\n"))
	isInteractive = func() bool { return true }
	ok, err := p.consent(false)
	if ok || err != nil {
		t.Fatalf("default answer: %v, %v", ok, err)
	}
	screen := output.String()
	if !strings.Contains(screen, fieldLabel("Maps and rawmaps")+"1") ||
		!strings.Contains(screen, "Skipped Room.json.") ||
		!strings.Contains(screen, "Missing versions: doors\n") ||
		!strings.HasSuffix(strings.TrimSpace(screen), "Back up your data before migrating. Continue? [Y/N]") ||
		strings.Contains(screen, "file change") || strings.Contains(screen, p.data) ||
		strings.Contains(screen, strings.Repeat("a", 64)) || strings.ContainsAny(screen, "\r\x1b") {
		t.Fatal(screen)
	}
}

func TestTerminalClearsActivityBeforeWritingResult(t *testing.T) {
	c, b := testConsole(true)
	c.activity("Reading a map")
	c.OK("Done")
	if !strings.Contains(b.String(), "\r\x1b[2K") || c.activityOn {
		t.Fatal(b.String())
	}
	plain, output := testConsole(true)
	plain.setPlain(true)
	plain.activity("Reading a map")
	if output.String() != indent+"Reading a map\n" {
		t.Fatal(output.String())
	}
}

func TestMigrationCountColorsAndListsPlannedMods(t *testing.T) {
	previous := ui
	t.Cleanup(func() { ui = previous })
	c, output := testConsole(true)
	ui = c
	showMigrationCount("Mods", 2, []string{"Airlock", "Doors"})
	showMigrationCount("Prefabs", 0, nil)
	want := fieldLabel("Mods") + ansiCyan + "2\n                    Airlock\n                    Doors" + ansiReset + "\n" +
		fieldLabel("Prefabs") + ansiGrey + "0" + ansiReset + "\n"
	if output.String() != want {
		t.Fatalf("migration counts = %q", output.String())
	}
	output.Reset()
	c.setPlain(true)
	showMigrationCount("Mods", 1, []string{"Doors"})
	if output.String() != fieldLabel("Mods")+"1\n                    Doors\n" {
		t.Fatalf("plain migration count = %q", output.String())
	}
}
