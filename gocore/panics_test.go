package main

import (
	"strings"
	"testing"
)

func panicking(where string) (answer string) {
	defer catchPanic(where, func(message string) { answer = "error: " + message })
	var nothing map[string]int
	nothing["x"] = 1 // a runtime panic, the kind a nil somewhere gives
	return "unreachable"
}

func TestCatchPanicAnswers(t *testing.T) {
	got := panicking("box_test")
	if !strings.HasPrefix(got, "error: core panic in box_test: ") || !strings.Contains(got, "nil map") {
		t.Fatalf("answer %q", got)
	}
}

func TestCatchPanicLeavesNormalReturns(t *testing.T) {
	calm := func() (answer string) {
		defer catchPanic("calm", func(string) { answer = "panicked" })
		return "fine"
	}
	if got := calm(); got != "fine" {
		t.Fatalf("got %q", got)
	}
}

func TestCatchPanicWithoutAnswer(t *testing.T) {
	func() {
		defer catchPanic("no answer", nil)
		panic("boom")
	}()
	// Reaching here is the test: the panic stopped in the deferred call.
}

// A panic on a goroutine of ours ends that goroutine, not the process.
func TestCatchPanicOnGoroutine(t *testing.T) {
	result := make(chan string, 1)
	go func() {
		defer catchPanic("goroutine", func(message string) { result <- message })
		panic("boom")
	}()
	if got := <-result; got != "core panic in goroutine: boom" {
		t.Fatalf("got %q", got)
	}
}
