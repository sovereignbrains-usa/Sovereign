package main

// The hosters Russia's "16 KB" cut takes by their whole address space, not
// by one server: a cut seen on one Cloudflare address is there on every
// other, so the smart outbound remembers it for the hoster, and the next
// site behind it goes through the VPN at once instead of losing its first
// connection too (15.11.2026). Only a cut or a slowdown on the direct way
// counts: the address-wide filter is the country's; a refused dial or a
// VPN server's trouble says nothing about the hoster.
//
// Cloudflare's ranges are its published ones, www.cloudflare.com/ips-v4 and
// ips-v6 (and api.cloudflare.com/client/v4/ips, the same), as of 15.11.2026.

import (
	"net/netip"

	"github.com/sagernet/sing-box/adapter"
)

type smartHoster struct {
	name     string // the memory key's tail and what the tray shows by it
	prefixes []netip.Prefix
}

var smartHosters = []smartHoster{
	{name: "cloudflare", prefixes: mustPrefixes(
		"173.245.48.0/20", "103.21.244.0/22", "103.22.200.0/22", "103.31.4.0/22",
		"141.101.64.0/18", "108.162.192.0/18", "190.93.240.0/20", "188.114.96.0/20",
		"197.234.240.0/22", "198.41.128.0/17", "162.158.0.0/15", "104.16.0.0/13",
		"104.24.0.0/14", "172.64.0.0/13", "131.0.72.0/22",
		"2400:cb00::/32", "2606:4700::/32", "2803:f800::/32", "2405:b500::/32",
		"2405:8100::/32", "2a06:98c0::/29", "2c0f:f248::/32",
	)},
}

func mustPrefixes(list ...string) []netip.Prefix {
	out := make([]netip.Prefix, 0, len(list))
	for _, s := range list {
		out = append(out, netip.MustParsePrefix(s))
	}
	return out
}

// hosterOf: the name of the hoster `addr` belongs to, or "".
func hosterOf(addr netip.Addr) string {
	addr = addr.Unmap()
	for _, h := range smartHosters {
		for _, p := range h.prefixes {
			if p.Contains(addr) {
				return h.name
			}
		}
	}
	return ""
}

// smartHosterKey: the connection's hoster as a memory key, "hoster:cloudflare",
// or "" (no address, or one of no hoster in the table).
func smartHosterKey(metadata *adapter.InboundContext) string {
	if !metadata.Destination.IsIP() {
		return ""
	}
	if name := hosterOf(metadata.Destination.Addr); name != "" {
		return "hoster:" + name
	}
	return ""
}
