package main

import (
	"net/netip"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/sagernet/sing/common/control"
)

func TestSmartNetKey(t *testing.T) {
	ifc := func(name string, addr string) *control.Interface {
		return &control.Interface{Name: name, Addresses: []netip.Prefix{netip.MustParsePrefix(addr)}}
	}
	home := smartNetKey(ifc("Wi-Fi", "192.168.1.23/24"))
	if home == "" || home != smartNetKey(ifc("Wi-Fi", "192.168.1.77/24")) {
		t.Fatal("a renewed lease is the same network")
	}
	if home != smartNetKey(ifc("Wi-Fi", "192.168.1.23/32")) {
		t.Fatal("a /32 is still its /24")
	}
	if home == smartNetKey(ifc("Wi-Fi", "192.168.43.5/24")) {
		t.Fatal("another subnet is another network")
	}
	if home == smartNetKey(ifc("Ethernet", "192.168.1.23/24")) {
		t.Fatal("another interface is another network")
	}
	if smartNetKey(nil) != "" {
		t.Fatal("no interface, no network")
	}
}

func TestSmartMemoryByNetwork(t *testing.T) {
	m := &smartMemory{items: map[string]smartVerdict{}}
	now := time.Now()
	m.remember("early.example", "proxy", "reset", now) // before the network is known
	m.switchNet("home", now)
	if way, ok := m.verdict("early.example", now); !ok || way != "proxy" {
		t.Fatal("what was learned before the network was known goes to the first network")
	}
	m.remember("site.example", "proxy", "cut", now)
	m.switchNet("hotspot", now)
	if _, ok := m.verdict("site.example", now); ok {
		t.Fatal("a hotspot must not inherit home's verdicts")
	}
	m.switchNet("home", now)
	if way, ok := m.verdict("site.example", now); !ok || way != "proxy" {
		t.Fatal("back home, home's verdicts are back")
	}
}

func TestSmartMemoryKeepsOnlyFailures(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sub", smartFileName)
	now := time.Now()
	m := &smartMemory{items: map[string]smartVerdict{}}
	m.open(path, now)
	m.switchNet("home", now)
	m.remember("blocked.example", "proxy", "reset", now)
	m.remember("fine.example", "direct", "", now)
	m.remember("old.example", "proxy", "cut", now.Add(-2*smartRemember))
	m.flush()

	back := &smartMemory{items: map[string]smartVerdict{}}
	back.open(path, now)
	back.switchNet("home", now)
	if way, ok := back.verdict("blocked.example", now); !ok || way != "proxy" {
		t.Fatal("a failed preferred way is kept across a restart")
	}
	if _, ok := back.verdict("fine.example", now); ok {
		t.Fatal("what the preferred way answered is not written down: the file would list the sites visited")
	}
	if _, ok := back.verdict("old.example", now); ok {
		t.Fatal("an expired verdict is not loaded")
	}
	if _, err := os.Stat(path + ".part"); err == nil {
		t.Fatal("the partial file is left behind")
	}
	// A second open must not replace the live network's map.
	back.open(path, now)
	back.remember("later.example", "proxy", "dial", now)
	if _, ok := back.verdict("later.example", now); !ok {
		t.Fatal("open after open lost the live memory")
	}
}

func TestSmartMemoryBrokenFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), smartFileName)
	if err := os.WriteFile(path, []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	m := &smartMemory{items: map[string]smartVerdict{}}
	m.open(path, time.Now()) // must not panic, must start empty
	m.switchNet("home", time.Now())
	if len(m.items) != 0 {
		t.Fatal("a broken file is an empty memory")
	}
}
