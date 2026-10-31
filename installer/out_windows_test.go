//go:build windows

package main

import (
	"os"
	"syscall"
	"testing"
)

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
