package main

import (
	"net/netip"
	"testing"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/log"
	M "github.com/sagernet/sing/common/metadata"
)

func TestHosterOf(t *testing.T) {
	for addr, want := range map[string]string{
		"104.16.132.229":         "cloudflare",
		"172.67.1.1":             "cloudflare",
		"::ffff:104.21.5.5":      "cloudflare", // a mapped v4 address too
		"2606:4700:3033::6815:1": "cloudflare",
		"1.1.1.1":                "", // the resolver is not in the published ranges
		"77.88.55.242":           "",
	} {
		if got := hosterOf(netip.MustParseAddr(addr)); got != want {
			t.Errorf("hosterOf(%s) = %q, want %q", addr, got, want)
		}
	}
	if got := smartHosterKey(&adapter.InboundContext{Destination: M.ParseSocksaddr("cut.test:443")}); got != "" {
		t.Errorf("a name, no address: %q", got)
	}
}

// typedWay: a fallbackWay of a given outbound type.
type typedWay struct {
	fallbackWay
	typ string
}

func (w typedWay) Type() string { return w.typ }

// A cut on the direct way to a Cloudflare address sends every site there
// through the VPN; one on the VPN's way says nothing about the hoster.
func TestCutRemembersTheHoster(t *testing.T) {
	const name, other = "cut.hoster.test", "other.hoster.test"
	first := &adapter.InboundContext{Domain: name, Destination: M.ParseSocksaddr("104.16.1.1:443")}
	next := &adapter.InboundContext{Domain: other, Destination: M.ParseSocksaddr("172.64.2.2:443")}
	keys := []string{name, other, "ip:104.16.1.1", "ip:172.64.2.2", "hoster:cloudflare"}
	forget(keys...)
	defer forget(keys...)
	direct := typedWay{fallbackWay{tag: "direct"}, constant.TypeDirect}
	vpn := typedWay{fallbackWay{tag: "vpn"}, "vless"}
	nop := log.NewNOPFactory().Logger()

	s := &smartOutbound{ways: []adapter.Outbound{direct, vpn}, logger: nop}
	s.stopped(0, name, first, "cut")()
	if tag, ok := verdictOf(next, time.Now()); !ok || tag != "vpn" {
		t.Fatalf("another Cloudflare site after a cut: %q %v", tag, ok)
	}
	if v := smartMemo.learned(time.Now())["hoster:cloudflare"]; v.Way != "vpn" || v.Why != "cut" {
		t.Fatalf("the hoster learned: %+v", v)
	}

	forget(keys...)
	s = &smartOutbound{ways: []adapter.Outbound{vpn, direct}, logger: nop}
	s.stopped(0, name, first, "slow")()
	if _, ok := smartMemo.learned(time.Now())["hoster:cloudflare"]; ok {
		t.Fatal("a slowdown on the VPN's way was taken for the hoster's")
	}
	if _, ok := verdictOf(next, time.Now()); ok {
		t.Fatal("another Cloudflare site has a way after the VPN's slowdown")
	}
}
