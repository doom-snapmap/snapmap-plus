//go:build windows

package main

import (
	"os"
	"syscall"
)

// enableVirtualTerminalProcessing is ENABLE_VIRTUAL_TERMINAL_PROCESSING: the console interprets ANSI
// escape sequences.
const enableVirtualTerminalProcessing = 0x0004

var procSetConsoleMode = syscall.NewLazyDLL("kernel32.dll").NewProc("SetConsoleMode")

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
