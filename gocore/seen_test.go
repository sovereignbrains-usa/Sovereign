package main

import (
	"testing"
	"time"

	"github.com/gofrs/uuid/v5"
	"github.com/sagernet/sing-box/common/trafficcontrol"
)

// tracked (flows_test.go) with an id of its own.
func identified(up, down int64, chain ...string) *trafficcontrol.TrackerMetadata {
	m := tracked(up, down, chain...)
	m.ID, _ = uuid.NewV4()
	return m
}

func TestSeenDeltas(t *testing.T) {
	a := identified(100, 1000, "nl-hy2", "proxy")
	b := identified(10, 500, "nl-hy2", "proxy")
	c := identified(1, 2, "direct")
	deltas, last := SeenDeltas(nil, []*trafficcontrol.TrackerMetadata{a, b}, []*trafficcontrol.TrackerMetadata{c})
	if deltas["nl-hy2"] != [2]int64{110, 1500} || deltas["direct"] != [2]int64{1, 2} || len(last) != 3 {
		t.Fatalf("first: %v %v", deltas, last)
	}
	// a goes on, b closes having carried more, c is gone from the lists.
	a.Download.Store(3000)
	b.Download.Store(600)
	deltas, last = SeenDeltas(last, []*trafficcontrol.TrackerMetadata{a}, []*trafficcontrol.TrackerMetadata{b})
	if deltas["nl-hy2"] != [2]int64{0, 2100} || len(deltas) != 1 || len(last) != 2 {
		t.Fatalf("second: %v %v", deltas, last)
	}
	// Listed open and closed both, between two reads: counted once.
	b.Download.Store(700)
	deltas, _ = SeenDeltas(last, []*trafficcontrol.TrackerMetadata{b}, []*trafficcontrol.TrackerMetadata{b})
	if deltas["nl-hy2"] != [2]int64{0, 100} {
		t.Fatalf("twice: %v", deltas)
	}
}

func TestSeenRate(t *testing.T) {
	seenMu.Lock()
	defer seenMu.Unlock()
	seenTags = map[string]*seenTagState{}
	defer func() { seenTags = map[string]*seenTagState{} }()
	rate := func(tag string, at time.Time) (uint32, uint32) {
		seenMu.Unlock()
		defer seenMu.Lock()
		return seenRate(tag, at)
	}
	start := time.Unix(1_800_000_000, 0)
	at := start
	tick := func(deltas map[string][2]int64) {
		at = at.Add(time.Second)
		addSeen(deltas, time.Second, at)
	}
	// 2.5 MB a second down (20 Mbit/s), 50 KB up (below the floor).
	tick(map[string][2]int64{"nl-hy2": {50_000, 2_500_000}})
	tick(map[string][2]int64{"nl-hy2": {50_000, 2_500_000}})
	if down, _ := rate("nl-hy2", at); down != 0 {
		t.Fatal("two seconds are no window yet")
	}
	tick(map[string][2]int64{"nl-hy2": {50_000, 2_500_000}})
	if down, up := rate("nl-hy2", at); down != 20_000 || up != 0 {
		t.Fatalf("peak: %d %d", down, up)
	}
	// Quiet after: the peak stays for the hour, then goes.
	for range 5 {
		tick(nil)
	}
	if down, _ := rate("nl-hy2", at); down != 20_000 {
		t.Fatalf("kept: %d", down)
	}
	if down, _ := rate("nl-hy2", at.Add(61*time.Minute)); down != 0 {
		t.Fatal("an hour on, the peak is gone")
	}
	if down, _ := rate("other", at); down != 0 {
		t.Fatal("a server nothing went through")
	}
}
