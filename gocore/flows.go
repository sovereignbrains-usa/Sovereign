package main

// Where the traffic went: bytes up and down per outbound a rule sent it to -
// "direct", the WARP endpoint, a proxy selector - for the main window's live
// streams round the switch (each way drawn its own). The tray polls it and
// takes rates from the differences, as with box_stats.

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"sort"
	"strings"
	"time"

	"github.com/sagernet/sing-box/common/trafficcontrol"
)

// How far back and how many sites box_flows reports for the "Маршруты"
// page's "сейчас:" lines.
const (
	recentWindow = 10 * time.Minute
	recentLimit  = 300
)

// RecentSites lists the names connections went to since `since`, newest
// first, each with the way it took ("direct", "warp", a proxy selector; the
// smart outbound resolved to its real way), one entry per name and way, at
// most `limit`. Only routed connections are tracked, so the client's own
// probes (latency, exit IP, sites, speed) and the losing leg of a smart race
// are not here, as they are in the log. A connection by address alone (no
// sniffed or asked name) is left out.
func RecentSites(since time.Time, limit int, connections ...[]*trafficcontrol.TrackerMetadata) [][2]string {
	type seen struct {
		host, tag string
		at        time.Time
	}
	var all []seen
	for _, list := range connections {
		for _, m := range list {
			if m == nil || m.CreatedAt.Before(since) {
				continue
			}
			host := m.Metadata.Domain
			if host == "" && m.Metadata.Destination.IsFqdn() {
				host = m.Metadata.Destination.Fqdn
			}
			host = strings.TrimSuffix(strings.ToLower(host), ".")
			if host == "" {
				continue
			}
			tag := ""
			if n := len(m.Chain); n > 0 {
				tag = m.Chain[n-1]
			}
			if way, ok := smartWay(tag, &m.Metadata); ok {
				tag = way.Tag()
			}
			if tag == "" {
				continue
			}
			all = append(all, seen{host, tag, m.CreatedAt})
		}
	}
	sort.SliceStable(all, func(i, j int) bool { return all[i].at.After(all[j].at) })
	sites := [][2]string{}
	done := map[[2]string]bool{}
	for _, s := range all {
		key := [2]string{s.host, s.tag}
		if done[key] {
			continue
		}
		done[key] = true
		sites = append(sites, key)
		if len(sites) >= limit {
			break
		}
	}
	return sites
}

// FlowTotals sums connections by the outbound their rule picked: the last of
// the chain (sing-box keeps it from the server back to the rule's outbound).
// A connection with no chain counts under "".
func FlowTotals(connections ...[]*trafficcontrol.TrackerMetadata) map[string][2]int64 {
	flows := map[string][2]int64{}
	for _, list := range connections {
		for _, m := range list {
			tag := ""
			if n := len(m.Chain); n > 0 {
				tag = m.Chain[n-1]
			}
			if way, ok := smartWay(tag, &m.Metadata); ok {
				tag = way.Tag() // "direct" or the proxy, not "sov-smart"
			}
			f := flows[tag]
			if m.Upload != nil {
				f[0] += m.Upload.Load()
			}
			if m.Download != nil {
				f[1] += m.Download.Load()
			}
			flows[tag] = f
		}
	}
	return flows
}

// box_flows reports the running instance's traffic by outbound: open
// connections plus the closed ones the manager still keeps (the oldest fall
// off its list - a total may drop; the tray counts a drop as nothing).
// {"flows":{"<tag>":[up,down],...},"recent":[["<host>","<tag>"],...]} (the
// sites of the last 10 minutes, RecentSites), empty while nothing runs.
// Freed with box_free.
//
//export box_flows
func box_flows() (reply *C.char) {
	defer catchPanic("box_flows", func(string) { reply = C.CString(`{"flows":{},"recent":[]}`) })
	boxMu.Lock()
	defer boxMu.Unlock()
	flows := map[string][2]int64{}
	recent := [][2]string{}
	if boxTraffic != nil {
		open, closed := boxTraffic.Connections(), boxTraffic.ClosedConnections()
		flows = FlowTotals(open, closed)
		recent = RecentSites(time.Now().Add(-recentWindow), recentLimit, open, closed)
	}
	out, err := json.Marshal(map[string]any{"flows": flows, "recent": recent})
	if err != nil {
		return C.CString(`{"flows":{},"recent":[]}`)
	}
	return C.CString(string(out))
}
