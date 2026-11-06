package main

// Where an address went: the newest connection the traffic manager keeps to
// it - by the name sniffed or asked for, or by one of its addresses - and the
// outbound its rule picked. The checks page's "where an address goes": the
// core's log said it too, but only at info level, and the user may keep it
// at error (16.10.2026).

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"strings"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/trafficcontrol"
	N "github.com/sagernet/sing/common/network"
)

// RouteSeen is one connection's way: the server (or "direct") it went out
// of, that outbound's type, the outbounds from the server back to the rule's
// one, and the rule ("" - none matched: the route's final).
type RouteSeen struct {
	Outbound string   `json:"outbound"`
	Type     string   `json:"type"`
	Chain    []string `json:"chain"`
	Rule     string   `json:"rule,omitempty"`
}

// RouteOf finds the newest connection to one of `targets` (a name, matched
// as sniffed or as dialled, or an address) in `connections`.
func RouteOf(targets []string, connections ...[]*trafficcontrol.TrackerMetadata) *RouteSeen {
	wanted := map[string]bool{}
	for _, t := range targets {
		if t = strings.ToLower(strings.TrimSuffix(strings.TrimSpace(t), ".")); t != "" {
			wanted[t] = true
		}
	}
	var newest *trafficcontrol.TrackerMetadata
	for _, list := range connections {
		for _, m := range list {
			if m == nil || (newest != nil && !m.CreatedAt.After(newest.CreatedAt)) {
				continue
			}
			names := []string{m.Metadata.Domain, m.Metadata.Destination.Fqdn}
			if m.Metadata.Destination.Addr.IsValid() {
				names = append(names, m.Metadata.Destination.Addr.Unmap().String())
			}
			for _, name := range names {
				if wanted[strings.ToLower(strings.TrimSuffix(name, "."))] {
					newest = m
					break
				}
			}
		}
	}
	if newest == nil {
		return nil
	}
	seen := &RouteSeen{Outbound: newest.Outbound, Type: newest.OutboundType, Chain: newest.Chain}
	if n := len(newest.Chain); n > 0 {
		if way, ok := smartWay(newest.Chain[n-1], &newest.Metadata); ok {
			// The smart outbound's: the way it took, down its groups to the server.
			path := []adapter.Outbound{way}
			for group, isGroup := way.(adapter.OutboundGroup); isGroup; group, isGroup = way.(adapter.OutboundGroup) {
				if way = group.Selected(N.NetworkTCP); way == nil {
					break
				}
				path = append(path, way)
			}
			last := path[len(path)-1]
			seen.Outbound, seen.Type = last.Tag(), last.Type()
			seen.Chain = nil
			for i := len(path) - 1; i >= 0; i-- {
				seen.Chain = append(seen.Chain, path[i].Tag())
			}
			seen.Chain = append(seen.Chain, newest.Chain[n-1])
		}
	}
	if a := liveAuto(seen.Outbound); a != nil {
		// The core's auto: the server it carries by, before it in the chain.
		network := N.NetworkTCP
		if newest.Metadata.Network == N.NetworkUDP {
			network = N.NetworkUDP
		}
		if way := a.byTag(autoWay(a.Tag(), network)); way != nil {
			seen.Outbound, seen.Type = way.Tag(), way.Type()
			seen.Chain = append([]string{way.Tag()}, seen.Chain...)
		}
	}
	if len(seen.Chain) > maxRouteChain {
		seen.Chain = seen.Chain[:maxRouteChain]
	}
	if newest.Rule != nil {
		seen.Rule = shortened(newest.Rule.String(), maxRouteRule)
	}
	return seen
}

// The answer goes back in one read of the tray's pipe buffer (4 KB): a rule
// with a user's hundred sites in it is cut, the chain is a few groups deep.
const (
	maxRouteRule  = 400
	maxRouteChain = 8
)

// shortened cuts `text` to at most `limit` bytes on a character's boundary.
func shortened(text string, limit int) string {
	if len(text) <= limit {
		return text
	}
	cut := 0
	for i := range text {
		if i > limit-3 {
			break
		}
		cut = i
	}
	return text[:cut] + "…"
}

// box_route answers where the newest connection to one of the addresses in
// `targets` (a JSON array of strings: a name and what it resolved to) went:
// {"route":{"outbound","type","chain","rule"}}, or {} if none is kept or
// nothing runs. Freed with box_free.
//
//export box_route
func box_route(targets *C.char) (reply *C.char) {
	defer catchPanic("box_route", func(string) { reply = C.CString(`{}`) })
	var list []string
	if targets == nil || json.Unmarshal([]byte(C.GoString(targets)), &list) != nil {
		return C.CString(`{}`)
	}
	boxMu.Lock()
	defer boxMu.Unlock()
	if boxTraffic == nil {
		return C.CString(`{}`)
	}
	seen := RouteOf(list, boxTraffic.Connections(), boxTraffic.ClosedConnections())
	if seen == nil {
		return C.CString(`{}`)
	}
	out, err := json.Marshal(map[string]any{"route": seen})
	if err != nil {
		return C.CString(`{}`)
	}
	return C.CString(string(out))
}
