package main

import (
	"fmt"
	"runtime/debug"
	"strings"

	"github.com/sagernet/sing-box/log"
)

// catchPanic keeps a panic in this DLL's code from taking the service down
// with it. A Go panic that crosses a cgo export, or ends a goroutine
// unrecovered, kills the whole host process - the SYSTEM service, and the
// user's connection with it. Deferred first thing in every export and in
// the goroutines this package starts around outbound code, it logs what
// panicked with its stack and hands the message to `answer`, which turns it
// into what that function returns on an error.
//
// It must be the deferred function itself (defer catchPanic(...)):
// recover only stops a panic when called directly by a deferred function.
func catchPanic(where string, answer func(message string)) {
	value := recover()
	if value == nil {
		return
	}
	message := fmt.Sprintf("core panic in %s: %v", where, value)
	stack := strings.TrimSpace(string(debug.Stack()))
	sayToHost(log.LevelError, message+"\n"+stack)
	if answer != nil {
		answer(message)
	}
}
