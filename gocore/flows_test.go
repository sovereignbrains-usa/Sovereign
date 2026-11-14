package main

import (
	"sync/atomic"
	"testing"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/trafficcontrol"
	M "github.com/sagernet/sing/common/metadata"
)

func tracked(up, down int64, chain ...string) *trafficcontrol.TrackerMetadata {
	u := new(atomic.Int64)
	u.Store(up)
	d := new(atomic.Int64)
	d.Store(down)
	return &trafficcontrol.TrackerMetadata{Upload: u, Download: d, Chain: chain}
}

func TestFlowTotalsByTheRulesOutbound(t *testing.T) {
	open := []*trafficcontrol.TrackerMetadata{
		tracked(10, 100, "Hysteria 2", "auto", "proxy"), // the server, the group, the rule's selector
		tracked(1, 5, "direct"),
	}
	closed := []*trafficcontrol.TrackerMetadata{
		tracked(2, 20, "AnyTLS", "proxy"),
		tracked(3, 30, "warp"),
		{Chain: nil}, // no counters, no chain
	}
	flows := FlowTotals(open, closed)
	if got := flows["proxy"]; got != [2]int64{12, 120} {
		t.Fatalf("proxy = %v", got)
	}
	if got := flows["direct"]; got != [2]int64{1, 5} {
		t.Fatalf("direct = %v", got)
	}
	if got := flows["warp"]; got != [2]int64{3, 30} {
		t.Fatalf("warp = %v", got)
	}
	if got := flows[""]; got != [2]int64{0, 0} {
		t.Fatalf("no chain = %v", got)
	}
}

func site(host, fqdn string, at time.Time, chain ...string) *trafficcontrol.TrackerMetadata {
	m := tracked(0, 0, chain...)
	m.Metadata = adapter.InboundContext{Domain: host, Destination: M.Socksaddr{Fqdn: fqdn, Port: 443}}
	m.CreatedAt = at
	return m
}

func TestRecentSitesNewestFirstByNameAndWay(t *testing.T) {
	now := time.Now()
	open := []*trafficcontrol.TrackerMetadata{
		site("YA.RU.", "", now.Add(-1*time.Minute), "direct"),
		site("", "github.com", now.Add(-2*time.Minute), "Hysteria 2", "proxy"), // asked by name, not sniffed
		site("", "", now, "direct"),                                            // by address only
	}
	closed := []*trafficcontrol.TrackerMetadata{
		site("ya.ru", "", now.Add(-3*time.Minute), "direct"),        // the same name and way: once
		site("ya.ru", "", now.Add(-4*time.Minute), "proxy"),         // the same name another way: kept
		site("old.example", "", now.Add(-20*time.Minute), "direct"), // before the window
		site("nochain.example", "", now),                            // no way
	}
	got := RecentSites(now.Add(-10*time.Minute), 10, open, closed)
	want := [][2]string{{"ya.ru", "direct"}, {"github.com", "proxy"}, {"ya.ru", "proxy"}}
	if len(got) != len(want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got %v, want %v", got, want)
		}
	}
	if got := RecentSites(now.Add(-10*time.Minute), 1, open, closed); len(got) != 1 || got[0] != want[0] {
		t.Fatalf("limit 1: %v", got)
	}
}
