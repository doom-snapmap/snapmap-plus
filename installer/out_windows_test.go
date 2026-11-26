//go:build windows

package main

import (
	"bufio"
	"context"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestMigrationReadsOnlyFreshConsoleInput(t *testing.T) {
	const child = "SNAPMAP_TEST_PROMPT_CONSOLE"
	if os.Getenv(child) == "" {
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
		defer cancel()
		cmd := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestMigrationReadsOnlyFreshConsoleInput$")
		cmd.Env = append(os.Environ(), child+"=1")
		const createNewConsole = 0x10
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNewConsole, HideWindow: true}
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("private test console: %v\n%s", err, out)
		}
		return
	}
	console, err := os.OpenFile("CONIN$", os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer console.Close()
	os.Stdin = console
	isInteractive = func() bool { return true }
	type keyEvent struct {
		kind, pad             uint16
		down                  int32
		count, key, scan, chr uint16
		control               uint32
	}
	writeInput := syscall.NewLazyDLL("kernel32.dll").NewProc("WriteConsoleInputW")
	typeLine := func(keys []keyEvent) {
		t.Helper()
		var written uint32
		ok, _, err := writeInput.Call(console.Fd(), uintptr(unsafe.Pointer(&keys[0])), uintptr(len(keys)), uintptr(unsafe.Pointer(&written)))
		if ok == 0 || int(written) != len(keys) {
			t.Fatalf("writing console input: %v", err)
		}
	}
	enter := keyEvent{kind: 1, down: 1, count: 1, key: '\r', chr: '\r'}
	typeLine([]keyEvent{enter})
	in := openMigrationInput()
	if in == nil {
		t.Fatal("missing migration keyboard")
	}
	defer in.Close()
	typeLine([]keyEvent{{kind: 1, down: 1, count: 1, key: 'Y', chr: 'y'}, enter})
	if line, err := bufio.NewReader(in).ReadString('\n'); err != nil || cleanLine(line) != "y" {
		t.Fatalf("expected the fresh answer, got %q (%v)", line, err)
	}
}

// A real console accepts virtual-terminal processing and gets its mode back afterwards; a pipe is not a
// console and is never switched.
func TestConsoleVirtualTerminal(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	if isConsoleFile(w) || enableVirtualTerminal(w) {
		t.Error("a pipe was treated as a console")
	}

	con, err := os.OpenFile("CONOUT$", os.O_RDWR, 0)
	if err != nil {
		t.Skipf("no console attached: %v", err)
	}
	defer con.Close()
	h := syscall.Handle(con.Fd())
	var before uint32
	if err := syscall.GetConsoleMode(h, &before); err != nil {
		t.Skipf("console mode unreadable: %v", err)
	}
	if !isConsoleFile(con) {
		t.Fatal("the console was not recognized")
	}
	if !enableVirtualTerminal(con) {
		t.Skip("this console refuses virtual-terminal processing")
	}
	var now uint32
	if syscall.GetConsoleMode(h, &now) != nil || now&enableVirtualTerminalProcessing == 0 {
		t.Errorf("mode %#x lacks virtual-terminal processing", now)
	}
	restoreConsole()
	delete(savedConsoleModes, h)
	if syscall.GetConsoleMode(h, &now) != nil || now != before {
		t.Errorf("restored mode %#x, want %#x", now, before)
	}
}
