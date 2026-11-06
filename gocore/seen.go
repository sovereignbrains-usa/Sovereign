package main

// What each server carried for real (06.11.2026): the servers page shows a
// speed only measured, and a speed test costs the user a hundred megabytes
// and half a minute, so it's on a button; meanwhile the user's own traffic
// says how fast a server goes whenever something big comes down it - a
// video, an update, a download. Once a second the traffic manager's
// connections are read, each one's bytes since the second before added to
// the server it went through (the end of its chain; for the smart outbound,
// the way it takes, down its groups), and the rate over the last few
// seconds kept as the peak of its five minutes. box_delays reports the
// highest of the last hour (seen_down, seen_up) - a floor of what the server
// can do, never a guess above it: a page that came down at 2 Mbit/s says
// only that.
//
// Kept across box restarts: the tags are the servers', a restart doesn't
// change what they carried.

import (
	"sync"
	"time"

	"github.com/gofrs/uuid/v5"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/trafficcontrol"
	N "github.com/sagernet/sing/common/network"
)

const (
	seenTick    = time.Second
	seenWindow  = 3               // ticks a rate is averaged over
	seenBucket  = 5 * time.Minute // a peak kept for each
	seenBuckets = 12              // an hour of them
	seenFloor   = 125_000         // bytes/s, 1 Mbit/s: below it nothing is said
	seenGroups  = 8               // groups followed down from a smart outbound's way
)

type seenPeak struct {
	slot     int64   // the bucket's number, Unix time / seenBucket
	down, up float64 // bytes/s
}

type seenTagState struct {
	window [seenWindow][2]int64 // the last ticks' bytes, up and down
	at     int                  // where the next goes
	filled int
	peaks  [seenBuckets]seenPeak
}

var (
	seenOnce   sync.Once
	seenMu     sync.Mutex
	seenTags   = map[string]*seenTagState{}
	seenLast   = map[uuid.UUID][2]int64{} // each connection's bytes at the last tick
	seenOf     *trafficcontrol.Manager    // the manager seenLast belongs to
	seenTickAt time.Time
)

// startSeen starts the sampler, once for the process.
func startSeen() {
	seenOnce.Do(func() {
		go func() {
			ticker := time.NewTicker(seenTick)
			defer ticker.Stop()
			for now := range ticker.C {
				boxMu.Lock()
				traffic := boxTraffic
				boxMu.Unlock()
				func() {
					defer catchPanic("seen sampler", nil) // a tick lost, not the service
					sampleSeen(traffic, now)
				}()
			}
		}()
	})
}

// sampleSeen: one tick.
func sampleSeen(traffic *trafficcontrol.Manager, now time.Time) {
	seenMu.Lock()
	defer seenMu.Unlock()
	if traffic != seenOf {
		seenOf = traffic
		seenLast = map[uuid.UUID][2]int64{}
		seenTickAt = time.Time{}
	}
	if traffic == nil {
		return
	}
	var deltas map[string][2]int64
	deltas, seenLast = SeenDeltas(seenLast, traffic.Connections(), traffic.ClosedConnections())
	elapsed := now.Sub(seenTickAt)
	first := seenTickAt.IsZero()
	seenTickAt = now
	if first || elapsed <= 0 || elapsed > 3*seenTick {
		return // a baseline, or the ticks stalled: no rate from it
	}
	addSeen(deltas, elapsed, now)
}

// addSeen: a tick's bytes by server, `elapsed` after the one before; seenMu held.
func addSeen(deltas map[string][2]int64, elapsed time.Duration, now time.Time) {
	for tag := range deltas {
		if seenTags[tag] == nil {
			seenTags[tag] = &seenTagState{}
		}
	}
	slot := now.Unix() / int64(seenBucket/time.Second)
	for tag, state := range seenTags {
		state.window[state.at] = deltas[tag] // nothing this tick counts as nothing
		state.at = (state.at + 1) % seenWindow
		state.filled = min(state.filled+1, seenWindow)
		if state.filled < seenWindow {
			continue
		}
		var up, down int64
		for _, d := range state.window {
			up += d[0]
			down += d[1]
		}
		seconds := float64(seenWindow) * elapsed.Seconds()
		rateUp, rateDown := float64(up)/seconds, float64(down)/seconds
		peak := &state.peaks[slot%seenBuckets]
		if peak.slot != slot {
			*peak = seenPeak{slot: slot}
		}
		peak.up = max(peak.up, rateUp)
		peak.down = max(peak.down, rateDown)
	}
}

// SeenDeltas: each connection's bytes since `last`, summed by the server it
// went through; and the totals to compare with next time (connections no
// longer listed are forgotten).
func SeenDeltas(last map[uuid.UUID][2]int64, connections ...[]*trafficcontrol.TrackerMetadata) (map[string][2]int64, map[uuid.UUID][2]int64) {
	deltas := map[string][2]int64{}
	next := make(map[uuid.UUID][2]int64, len(last))
	for _, list := range connections {
		for _, m := range list {
			if m == nil {
				continue
			}
			var now [2]int64
			if m.Upload != nil {
				now[0] = m.Upload.Load()
			}
			if m.Download != nil {
				now[1] = m.Download.Load()
			}
			if _, listed := next[m.ID]; listed {
				continue // open and closed both, between two reads
			}
			next[m.ID] = now
			before := last[m.ID]
			delta := [2]int64{max(now[0]-before[0], 0), max(now[1]-before[1], 0)}
			if delta == [2]int64{} {
				continue
			}
			tag := seenServer(m)
			d := deltas[tag]
			d[0] += delta[0]
			d[1] += delta[1]
			deltas[tag] = d
		}
	}
	return deltas, next
}

// seenServer: the outbound a connection left through - the first of its
// chain (sing-box keeps it from the server back to the rule's outbound); for
// the smart outbound, the way it takes now, down its groups.
func seenServer(m *trafficcontrol.TrackerMetadata) string {
	if len(m.Chain) == 0 {
		return m.Outbound
	}
	way, smart := smartWay(m.Chain[len(m.Chain)-1], &m.Metadata)
	if !smart || way == nil {
		return m.Chain[0]
	}
	network := N.NetworkTCP
	if m.Metadata.Network == N.NetworkUDP {
		network = N.NetworkUDP
	}
	for range seenGroups {
		group, isGroup := way.(adapter.OutboundGroup)
		if !isGroup {
			break
		}
		next := group.Selected(network)
		if next == nil {
			break
		}
		way = next
	}
	return way.Tag()
}

// seenRate: the highest rate `tag` carried in the last hour, kbit/s, down
// and up; 0 below seenFloor.
func seenRate(tag string, now time.Time) (uint32, uint32) {
	seenMu.Lock()
	defer seenMu.Unlock()
	state := seenTags[tag]
	if state == nil {
		return 0, 0
	}
	slot := now.Unix() / int64(seenBucket/time.Second)
	var down, up float64
	for _, peak := range state.peaks {
		if peak.slot > slot-seenBuckets && peak.slot <= slot {
			down = max(down, peak.down)
			up = max(up, peak.up)
		}
	}
	kbps := func(rate float64) uint32 {
		if rate < seenFloor {
			return 0
		}
		return uint32(min(rate*8/1000, 4e9))
	}
	return kbps(down), kbps(up)
}
