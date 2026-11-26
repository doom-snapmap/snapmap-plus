//go:build windows

package main

import (
	"io"
	"os"
	"syscall"
)

// enableVirtualTerminalProcessing is ENABLE_VIRTUAL_TERMINAL_PROCESSING: the console interprets ANSI
// escape sequences.
const enableVirtualTerminalProcessing = 0x0004

var procSetConsoleMode = syscall.NewLazyDLL("kernel32.dll").NewProc("SetConsoleMode")
var procFlushConsoleInput = syscall.NewLazyDLL("kernel32.dll").NewProc("FlushConsoleInputBuffer")

func openConfirmationInput() io.ReadCloser {
	fi, err := os.Stdin.Stat()
	if err != nil || fi.Mode()&os.ModeCharDevice == 0 || !(isConsoleFile(os.Stdin) || isConsoleFile(os.Stdout) || isConsoleFile(os.Stderr)) {
		return nil
	}
	// Read the attached keyboard and discard keys queued before the question.
	in, err := os.OpenFile("CONIN$", os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	if ok, _, _ := procFlushConsoleInput.Call(in.Fd()); ok == 0 {
		in.Close()
		return nil
	}
	stdin.Reset(os.Stdin)
	return in
}

// savedConsoleModes holds the modes enableVirtualTerminal changed, for restoreConsole.
var savedConsoleModes = map[syscall.Handle]uint32{}

// isConsoleFile reports whether f is a console; GetConsoleMode fails for pipes, files and the null device.
func isConsoleFile(f *os.File) bool {
	var mode uint32
	return syscall.GetConsoleMode(syscall.Handle(f.Fd()), &mode) == nil
}

// enableVirtualTerminal sets ENABLE_VIRTUAL_TERMINAL_PROCESSING on f's console and reports whether the
// console has it afterwards. A console that refuses it (Windows before 10) gets no escape codes.
func enableVirtualTerminal(f *os.File) bool {
	h := syscall.Handle(f.Fd())
	var mode uint32
	if syscall.GetConsoleMode(h, &mode) != nil {
		return false
	}
	if mode&enableVirtualTerminalProcessing != 0 {
		return true
	}
	if r, _, _ := procSetConsoleMode.Call(uintptr(h), uintptr(mode|enableVirtualTerminalProcessing)); r == 0 {
		return false
	}
	if _, seen := savedConsoleModes[h]; !seen {
		savedConsoleModes[h] = mode
	}
	var now uint32
	return syscall.GetConsoleMode(h, &now) == nil && now&enableVirtualTerminalProcessing != 0
}

// restoreConsole puts back the console modes enableVirtualTerminal changed.
func restoreConsole() {
	for h, mode := range savedConsoleModes {
		procSetConsoleMode.Call(uintptr(h), uintptr(mode))
	}
}
