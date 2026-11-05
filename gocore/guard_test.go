package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
	"github.com/sagernet/sing/service"

	mDNS "github.com/miekg/dns"
)

// The config the tray writes for the block: one reject rule of the lists, the servers as they are.
const blockerConfig = `{
  "dns": {
    "servers": [
      {"type": "udp", "tag": "up", "server": "127.0.0.1", "server_port": %d},
      {"type": "hosts", "tag": "own", "predefined": {"own.test": "10.0.0.1"}}
    ],
    "rules": [
      {"domain_suffix": ["letthrough.example"], "server": "up"},
      {"rule_set": ["sov-ads", "sov-ads-trackers"], "action": "predefined", "rcode": "NOERROR"},
      {"domain_suffix": ["x.test"], "action": "predefined", "rcode": "NOERROR"}
    ],
    "final": "up"
  },
  "route": {"rule_set": [
    {"type": "inline", "tag": "sov-ads", "rules": [{"domain_suffix": ["ad.test"]}]},
    {"type": "inline", "tag": "sov-ads-trackers", "rules": [{"domain_suffix": ["tracker.test"]}]}
  ]}
}`

func TestPrepareBlocker(t *testing.T) {
	config := fmt.Sprintf(blockerConfig, 5353)
	var root map[string]any
	if err := json.Unmarshal(prepareBlocker([]byte(config)), &root); err != nil {
		t.Fatal(err)
	}
	dns := root["dns"].(map[string]any)
	byTag := map[string]map[string]any{}
	for _, item := range dns["servers"].([]any) {
		server := item.(map[string]any)
		byTag[server["tag"].(string)] = server
	}
	// The udp server: a guard under its tag, the real one behind.
	if byTag["up"]["type"] != "sov-guard" || byTag["up"]["inner"] != "up~real" {
		t.Fatalf("up: %v", byTag["up"])
	}
	if real := byTag["up~real"]; real["type"] != "udp" || real["server"] != "127.0.0.1" || real["server_port"] != float64(5353) {
		t.Fatalf("up~real: %v", real)
	}
	if byTag["own"]["type"] != "hosts" {
		t.Fatalf("hosts answers by itself, no guard: %v", byTag["own"])
	}
	if !reflect.DeepEqual(byTag["up"]["skip"], []any{"letthrough.example"}) {
		t.Fatalf("let through: %v", byTag["up"]["skip"])
	}
	lists := byTag["up"]["lists"].([]any)
	if len(lists) != 2 || lists[1].(map[string]any)["category"] != "trackers" {
		t.Fatalf("lists: %v", lists)
	}
	// The block's rule goes to sov-block, uncached; a user's own "no addresses" stays.
	rules := dns["rules"].([]any)
	block := rules[1].(map[string]any)
	if block["server"] != "sov-block" || block["action"] != "route" || block["disable_cache"] != true || block["rcode"] != nil {
		t.Fatalf("block rule: %v", block)
	}
	if byTag["sov-block"]["type"] != "sov-block" {
		t.Fatalf("sov-block: %v", byTag["sov-block"])
	}
	if own := rules[2].(map[string]any); own["action"] != "predefined" {
		t.Fatalf("a user's refusal changed: %v", own)
	}

	// Without the block's lists, or without a DNS section: as it was.
	for _, plain := range []string{
		`{"dns": {"servers": [{"type": "udp", "tag": "up", "server": "1.1.1.1"}]}, "route": {}}`,
		`{"route": {"rule_set": [{"type": "inline", "tag": "sov-ads", "rules": []}]}}`,
		`not json`,
	} {
		if got := string(prepareBlocker([]byte(plain))); got != plain {
			t.Fatalf("changed %q to %q", plain, got)
		}
	}
}

// fakeResolver: an upstream that answers, over UDP on localhost, the names of the test.
func fakeResolver(t *testing.T) int {
	t.Helper()
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	server := &mDNS.Server{PacketConn: conn, Handler: mDNS.HandlerFunc(func(w mDNS.ResponseWriter, query *mDNS.Msg) {
		reply := new(mDNS.Msg)
		reply.SetReply(query)
		name := query.Question[0].Name
		address := func(owner string) mDNS.RR {
			return &mDNS.A{Hdr: mDNS.RR_Header{Name: owner, Rrtype: mDNS.TypeA, Class: mDNS.ClassINET, Ttl: 60}, A: net.ParseIP("93.184.216.34")}
		}
		switch {
		case strings.HasSuffix(name, "shop.example."), strings.HasSuffix(name, "letthrough.example."):
			// metrics.shop.example CNAME t.tracker.test CNAME edge.cdn.test, then an address
			reply.Answer = []mDNS.RR{
				&mDNS.CNAME{Hdr: mDNS.RR_Header{Name: name, Rrtype: mDNS.TypeCNAME, Class: mDNS.ClassINET, Ttl: 60}, Target: "t.tracker.test."},
				address("t.tracker.test."),
			}
		case strings.HasSuffix(name, "cdn.example."):
			// an honest CNAME: to a CDN no list holds
			reply.Answer = []mDNS.RR{
				&mDNS.CNAME{Hdr: mDNS.RR_Header{Name: name, Rrtype: mDNS.TypeCNAME, Class: mDNS.ClassINET, Ttl: 60}, Target: "edge.cdn.test."},
				address("edge.cdn.test."),
			}
		default:
			reply.Answer = []mDNS.RR{address(name)}
		}
		_ = w.WriteMsg(reply)
	})}
	go func() { _ = server.ActivateAndServe() }()
	t.Cleanup(func() { _ = server.Shutdown() })
	return conn.LocalAddr().(*net.UDPAddr).Port
}

// The block through a real box: sing-box's DNS router, the rules, the rule-sets, our two transports.
func TestBlockerThroughBox(t *testing.T) {
	blocked = newBlockedWatch()
	port := fakeResolver(t)
	ctx := coreContext(context.Background())
	var options option.Options
	if err := singjson.UnmarshalContext(ctx, prepareBlocker([]byte(fmt.Sprintf(blockerConfig, port))), &options); err != nil {
		t.Fatal(err)
	}
	instance, err := box.New(box.Options{Context: ctx, Options: options})
	if err != nil {
		t.Fatal(err)
	}
	if err := instance.Start(); err != nil {
		t.Fatal(err)
	}
	defer instance.Close()
	router := service.FromContext[adapter.DNSRouter](ctx)

	ask := func(name string) *mDNS.Msg {
		t.Helper()
		message := new(mDNS.Msg)
		message.SetQuestion(mDNS.Fqdn(name), mDNS.TypeA)
		response, err := router.Exchange(ctx, message, adapter.DNSQueryOptions{})
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		return response
	}
	answers := func(name string) int { return len(ask(name).Answer) }

	if n := answers("plain.example"); n != 1 {
		t.Fatalf("a plain name: %d answers", n)
	}
	if n := answers("shop.cdn.example"); n != 2 {
		t.Fatalf("an honest CNAME: %d answers (want the CNAME and the address)", n)
	}
	// The name on a list itself: the rule, sov-block.
	for i := 0; i < 3; i++ { // one name three times is one block (the A and the AAAA); three names, three
		if n := answers(fmt.Sprintf("ads%d.ad.test", i%3)); n != 0 {
			t.Fatalf("a listed name: %d answers", n)
		}
	}
	// The name that is not on a list but whose CNAME is: the guard.
	if response := ask("metrics.shop.example"); len(response.Answer) != 0 || response.Rcode != mDNS.RcodeSuccess {
		t.Fatalf("a cloaked name: %v", response)
	}
	// What the user let through by name stays through.
	if n := answers("a.letthrough.example"); n != 2 {
		t.Fatalf("a name let through: %d answers", n)
	}

	counts := blocked.blockedCounts()
	if counts["ads"] != 3 || counts["trackers"] != 1 {
		t.Fatalf("counts: %v", counts)
	}
	if _, cloaked := blocked.blockedApps(10); cloaked != 1 {
		t.Fatalf("cloaked: %d", cloaked)
	}
}

// The programs: counted by path, most first, named by the file.
func TestBlockedApps(t *testing.T) {
	w := newBlockedWatch()
	now := time.Unix(1_800_000_000, 0)
	w.mu.Lock()
	w.count("ads", "a.test", `C:\Program Files\Brave\brave.exe`, false, now)
	w.count("ads", "b.test", `C:\Program Files\Brave\brave.exe`, false, now)
	w.count("trackers", "c.test", `C:\Windows\System32\svchost.exe`, true, now)
	w.count("trackers", "d.test", "", false, now)
	w.mu.Unlock()
	apps, cloaked := w.blockedApps(5)
	if cloaked != 1 || len(apps) != 2 || apps[0].App != "brave.exe" || apps[0].Count != 2 || apps[1].App != "svchost.exe" {
		t.Fatalf("apps %+v cloaked %d", apps, cloaked)
	}
	if got, _ := w.blockedApps(1); len(got) != 1 {
		t.Fatalf("the most: %+v", got)
	}
	// What each asked: newest first, a name once.
	if names := apps[0].Names; len(names) != 2 || names[0] != "b.test" || names[1] != "a.test" {
		t.Fatalf("brave's names: %v", names)
	}
	w.mu.Lock()
	w.count("ads", "a.test", `C:\Program Files\Brave\brave.exe`, false, now.Add(time.Minute))
	for i := range appNamesKeep + 5 {
		w.count("ads", fmt.Sprintf("n%d.test", i), `C:\Windows\System32\svchost.exe`, false, now)
	}
	w.mu.Unlock()
	apps, _ = w.blockedApps(5)
	for _, a := range apps {
		switch {
		case a.App == "brave.exe" && (len(a.Names) != 2 || a.Names[0] != "a.test"):
			t.Fatalf("brave again: %v", a.Names)
		case a.App == "svchost.exe" && (len(a.Names) != appNamesKeep || a.Names[0] != "n14.test"):
			t.Fatalf("svchost kept: %v", a.Names)
		}
	}
}
