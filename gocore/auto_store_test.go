package main

import (
	"path/filepath"
	"testing"
	"time"
)

func TestAutoHold(t *testing.T) {
	if autoHold(0) != autoDownFor || autoHold(1) != autoDownFor {
		t.Fatal("one failure holds the base time")
	}
	if autoHold(2) != 2*autoDownFor || autoHold(3) != 4*autoDownFor {
		t.Fatal("each failure in a row doubles the hold")
	}
	if autoHold(50) != autoDownMax {
		t.Fatal("the hold has a cap")
	}
}

// A way that keeps failing on a network is remembered across a restart; a
// success, a single failure and an expired hold are not.
func TestAutoMemoryOnDisk(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sub", autoMemoFileName)
	now := time.Now()
	m := &autoMemory{}
	m.open(path, now)
	m.note("home", "hy2", autoDown{at: now, why: "reset", strikes: 3}, false)
	m.note("home", "tuic", autoDown{at: now, why: "reset", strikes: 1}, false) // once: not kept
	m.note("phone", "hy2", autoDown{at: now, why: "silent", strikes: 2}, false)
	m.note("phone", "naive", autoDown{at: now.Add(-time.Hour), why: "old", strikes: 2}, false) // hold over by the load
	m.flush()

	back := &autoMemory{}
	back.open(path, now.Add(time.Second))
	home := back.recall("home")
	if len(home) != 1 || home["hy2"].strikes != 3 || home["hy2"].why != "reset" {
		t.Fatalf("home: %+v", home)
	}
	phone := back.recall("phone")
	if len(phone) != 1 || phone["hy2"].why != "silent" {
		t.Fatalf("phone: %+v", phone)
	}
	if len(back.recall("elsewhere")) != 0 {
		t.Fatal("a network never seen has nothing")
	}

	// A success removes the record.
	m.note("home", "hy2", autoDown{}, true)
	m.flush()
	again := &autoMemory{}
	again.open(path, now.Add(time.Second))
	if len(again.recall("home")) != 0 {
		t.Fatal("a success left the record")
	}
	if len(again.recall("phone")) != 1 {
		t.Fatal("the other network lost its record")
	}
}

// A broken file starts empty and nothing panics.
func TestAutoMemoryBrokenFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), autoMemoFileName)
	m := &autoMemory{}
	m.open(path, time.Now())
	m.flush()
	if len(m.recall("x")) != 0 {
		t.Fatal("empty memory recalled something")
	}
}

// A restart of the box: the new sov-auto starts with the ways the network cut last.
func TestAutoRecallsOnNetwork(t *testing.T) {
	path := filepath.Join(t.TempDir(), autoMemoFileName)
	old := autoMemo
	autoMemo = &autoMemory{}
	t.Cleanup(func() { autoMemo = old })
	autoMemo.open(path, time.Now())
	autoMemo.note("home", "a", autoDown{at: time.Now(), why: "reset", strikes: 2}, false)

	a := testAuto(nil, newFake("a", "open", 0), newFake("b", "open", 0))
	a.switchNetwork("home")
	if got := a.down["home"]["a"]; got.strikes != 2 {
		t.Fatalf("not recalled: %+v", got)
	}
	if _, bad := a.down["home"]["b"]; bad {
		t.Fatal("a way nobody failed is down")
	}
}
