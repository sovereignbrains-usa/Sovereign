package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/trafficcontrol"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
	singjson "github.com/sagernet/sing/common/json"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
)

// autoFake: a server that opens after `delay`, or fails ("refused"), or
// never answers ("hang": till the dial is given up).
type autoFake struct {
	tag    string
	udp    bool
	behave string // "open", "refused", "hang"
	delay  time.Duration
	dials  *atomic.Int32
	closes *atomic.Int32
}

func newFake(tag, behave string, delay time.Duration) *autoFake {
	return &autoFake{tag: tag, udp: true, behave: behave, delay: delay, dials: &atomic.Int32{}, closes: &atomic.Int32{}}
}

func (f *autoFake) Tag() string            { return f.tag }
func (f *autoFake) Type() string           { return "fake" }
func (f *autoFake) Dependencies() []string { return nil }
func (f *autoFake) Network() []string {
	if f.udp {
		return []string{N.NetworkTCP, N.NetworkUDP}
	}
	return []string{N.NetworkTCP}
}

func (f *autoFake) DialContext(ctx context.Context, _ string, _ M.Socksaddr) (net.Conn, error) {
	f.dials.Add(1)
	switch f.behave {
	case "refused":
		time.Sleep(f.delay)
		return nil, errors.New("connection refused")
	case "hang":
		<-ctx.Done()
		return nil, ctx.Err()
	}
	select {
	case <-time.After(f.delay):
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	client, server := net.Pipe()
	go func() { _, _ = io.Copy(io.Discard, server) }()
	return &countedConn{Conn: client, closes: f.closes, tag: f.tag}, nil
}

func (f *autoFake) ListenPacket(ctx context.Context, _ M.Socksaddr) (net.PacketConn, error) {
	conn, err := f.DialContext(ctx, N.NetworkUDP, M.Socksaddr{})
	if err != nil {
		return nil, err
	}
	return &fakePacket{countedConn: conn.(*countedConn)}, nil
}

type countedConn struct {
	net.Conn
	closes *atomic.Int32
	tag    string
}

func (c *countedConn) Close() error {
	c.closes.Add(1)
	return c.Conn.Close()
}

type fakePacket struct{ *countedConn }

func (p *fakePacket) ReadFrom([]byte) (int, net.Addr, error)    { return 0, nil, io.EOF }
func (p *fakePacket) WriteTo(b []byte, _ net.Addr) (int, error) { return len(b), nil }

func testAuto(hosts map[string]string, ways ...*autoFake) *autoOutbound {
	a := &autoOutbound{logger: log.NewNOPFactory().Logger(), hosts: hosts, down: map[string]map[string]autoDown{}}
	for _, w := range ways {
		a.tags = append(a.tags, w.tag)
		a.ways = append(a.ways, w)
	}
	return a
}

// measure sets the last test's results, as box_urltest would.
func measure(t *testing.T, results ...delayResult) {
	t.Helper()
	resetDelays()
	t.Cleanup(resetDelays)
	delayMu.Lock()
	defer delayMu.Unlock()
	for _, r := range results {
		r := r
		delayOrder = append(delayOrder, r.Tag)
		delays[r.Tag] = &r
	}
}

func tagsOf(ways []adapter.Outbound) string {
	tags := make([]string, len(ways))
	for i, w := range ways {
		tags[i] = w.Tag()
	}
	return strings.Join(tags, ",")
}

func TestAutoOrder(t *testing.T) {
	hosts := map[string]string{"nl-reality": "nl", "nl-hy2": "nl", "de-reality": "de", "de-hy2": "de"}
	a := testAuto(hosts, newFake("nl-reality", "open", 0), newFake("nl-hy2", "open", 0),
		newFake("de-reality", "open", 0), newFake("de-hy2", "open", 0))
	measure(t,
		delayResult{Tag: "nl-reality", Delay: 100, UDPError: "no udp"},
		delayResult{Tag: "nl-hy2", Delay: 120, UDPDelay: 60},
		delayResult{Tag: "de-reality", Delay: 50, UDPError: "no udp"},
		delayResult{Tag: "de-hy2", Delay: 80, UDPDelay: 90})
	now := time.Now()

	// No pick yet: the best measured, its machine's other protocol next.
	if got := tagsOf(a.order(N.NetworkTCP, now)); got != "de-reality,de-hy2,nl-reality,nl-hy2" {
		t.Fatalf("no pick: %s", got)
	}
	// The tray's pick first, its machine's other protocol next, then the rest by measure.
	a.SelectOutbound("nl-reality")
	if got := tagsOf(a.order(N.NetworkTCP, now)); got != "nl-reality,nl-hy2,de-reality,de-hy2" {
		t.Fatalf("picked: %s", got)
	}
	// UDP: the pick carries none - the best UDP; the ones carrying none left out.
	if got := tagsOf(a.order(N.NetworkUDP, now)); got != "nl-hy2,de-hy2" {
		t.Fatalf("udp: %s", got)
	}
	// A way that failed a connection on this network goes last, on this network only.
	a.reported(N.NetworkTCP)(a.ways[0], errors.New("connection reset"))
	if got := tagsOf(a.order(N.NetworkTCP, now)); got != "nl-hy2,de-reality,de-hy2,nl-reality" {
		t.Fatalf("pick down: %s", got)
	}
	if got := tagsOf(a.order(N.NetworkTCP, now.Add(autoDownFor+time.Second))); got != "nl-reality,nl-hy2,de-reality,de-hy2" {
		t.Fatalf("down for a while only: %s", got)
	}
	a.switchNetwork("phone")
	if got := tagsOf(a.order(N.NetworkTCP, now)); got != "nl-reality,nl-hy2,de-reality,de-hy2" {
		t.Fatalf("another network: %s", got)
	}
	a.switchNetwork("")
	a.reported(N.NetworkTCP)(a.ways[0], nil) // it opened one: no longer down
	if got := tagsOf(a.order(N.NetworkTCP, now)); got != "nl-reality,nl-hy2,de-reality,de-hy2" {
		t.Fatalf("opened again: %s", got)
	}
	states := autoStates(now)
	if len(states) != 0 {
		t.Fatalf("states of a box's not live: %v", states)
	}
}

func TestAutoUDPStaysWithPick(t *testing.T) {
	a := testAuto(nil, newFake("a", "open", 0), newFake("b", "open", 0), newFake("c", "open", 0))
	measure(t,
		delayResult{Tag: "a", Delay: 60, UDPDelay: 70},
		delayResult{Tag: "b", Delay: 50, UDPDelay: 60},
		delayResult{Tag: "c", Delay: 90, UDPDelay: 20})
	now := time.Now()
	a.SelectOutbound("b")
	// b's UDP is 40 ms worse than c's and three times: c's.
	if got := tagsOf(a.order(N.NetworkUDP, now))[:1]; got != "c" {
		t.Fatalf("clearly better: %s", got)
	}
	a.SelectOutbound("a")
	measure(t,
		delayResult{Tag: "a", Delay: 60, UDPDelay: 70},
		delayResult{Tag: "b", Delay: 50, UDPDelay: 60},
		delayResult{Tag: "c", Delay: 90, UDPDelay: 62})
	// 8 ms: not worth leaving the pick.
	if got := tagsOf(a.order(N.NetworkUDP, now))[:1]; got != "a" {
		t.Fatalf("close: %s", got)
	}
	if !a.SelectOutbound("c") || a.SelectOutbound("nope") {
		t.Fatal("SelectOutbound")
	}
}

func dialAuto(t *testing.T, a *autoOutbound) (string, time.Duration, error) {
	t.Helper()
	started := time.Now()
	conn, err := a.DialContext(context.Background(), N.NetworkTCP, M.ParseSocksaddr("example.org:443"))
	took := time.Since(started)
	if err != nil {
		return "", took, err
	}
	defer conn.Close()
	return conn.(*countedConn).tag, took, nil
}

func TestAutoFailsOver(t *testing.T) {
	measure(t, delayResult{Tag: "a", Delay: 50, Connect: 150}, delayResult{Tag: "b", Delay: 60, Connect: 150})
	// Refused: the next at once.
	a := testAuto(nil, newFake("a", "refused", 0), newFake("b", "open", 0))
	a.SelectOutbound("a")
	if tag, took, err := dialAuto(t, a); err != nil || tag != "b" || took > 300*time.Millisecond {
		t.Fatalf("refused: %s %v %v", tag, took, err)
	}
	// ... and it goes last now: the next connection goes straight to b.
	if tag, _, err := dialAuto(t, a); err != nil || tag != "b" || a.ways[0].(*autoFake).dials.Load() != 1 {
		t.Fatalf("down: %s %v %d", tag, err, a.ways[0].(*autoFake).dials.Load())
	}
	// Silent: the next after the stagger (twice the connect time, at least autoStaggerMin).
	a = testAuto(nil, newFake("a", "hang", 0), newFake("b", "open", 0))
	a.SelectOutbound("a")
	if tag, took, err := dialAuto(t, a); err != nil || tag != "b" || took < autoStaggerMin-50*time.Millisecond || took > autoStaggerMin+500*time.Millisecond {
		t.Fatalf("silent: %s %v %v", tag, took, err)
	}
	// The pick opens in time: the next one never dialled.
	a = testAuto(nil, newFake("a", "open", 100*time.Millisecond), newFake("b", "open", 0))
	a.SelectOutbound("a")
	if tag, _, err := dialAuto(t, a); err != nil || tag != "a" || a.ways[1].(*autoFake).dials.Load() != 0 {
		t.Fatalf("in time: %s %v", tag, err)
	}
	// Nothing opens: the last error.
	a = testAuto(nil, newFake("a", "refused", 0), newFake("b", "refused", 0))
	if _, _, err := dialAuto(t, a); err == nil || !strings.Contains(err.Error(), "refused") {
		t.Fatalf("none: %v", err)
	}
}

func TestAutoRaceLoserClosed(t *testing.T) {
	measure(t)
	slow, fast := newFake("slow", "open", autoStagger+400*time.Millisecond), newFake("fast", "open", 0)
	a := testAuto(nil, slow, fast)
	a.SelectOutbound("slow")
	tag, _, err := dialAuto(t, a)
	if err != nil || tag != "fast" {
		t.Fatalf("race: %s %v", tag, err)
	}
	deadline := time.Now().Add(3 * time.Second)
	for slow.closes.Load() == 0 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if slow.closes.Load() != 1 {
		t.Fatal("the loser that opened late was not closed")
	}
	// It opened: alive, not down.
	if got := tagsOf(a.order(N.NetworkTCP, time.Now())); got != "slow,fast" {
		t.Fatalf("loser alive: %s", got)
	}
}

func TestAutoCallerGivesUp(t *testing.T) {
	measure(t)
	a := testAuto(nil, newFake("a", "hang", 0))
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	if _, err := a.DialContext(ctx, N.NetworkTCP, M.ParseSocksaddr("example.org:443")); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("given up: %v", err)
	}
	time.Sleep(50 * time.Millisecond)
	if got := tagsOf(a.order(N.NetworkTCP, time.Now())); got != "a" || len(a.down[""]) != 0 {
		t.Fatalf("the caller giving up is no failure of the way: %v", a.down)
	}
}

func TestAutoListenPacket(t *testing.T) {
	measure(t, delayResult{Tag: "a", Delay: 50, UDPError: "no udp"}, delayResult{Tag: "b", Delay: 60, UDPDelay: 40})
	a := testAuto(nil, newFake("a", "open", 0), newFake("b", "open", 0))
	a.SelectOutbound("a")
	conn, err := a.ListenPacket(context.Background(), M.ParseSocksaddr("1.1.1.1:53"))
	if err != nil {
		t.Fatal(err)
	}
	conn.Close()
	if conn.(*fakePacket).tag != "b" || a.lastUDP != "b" {
		t.Fatalf("udp went by %s", conn.(*fakePacket).tag)
	}
}

// plainSocks: a SOCKS5 server that connects where asked.
func plainSocks(t *testing.T) string {
	t.Helper()
	return socksServer(t, func(conn net.Conn, target string) {
		remote, err := net.DialTimeout("tcp", target, 5*time.Second)
		if err != nil {
			return
		}
		defer remote.Close()
		go func() { _, _ = io.Copy(remote, conn) }()
		_, _ = io.Copy(conn, remote)
	})
}

func TestAutoInRealBox(t *testing.T) {
	site := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "ok")
	}))
	defer site.Close()
	alive := plainSocks(t)
	aliveHost, alivePortText, _ := net.SplitHostPort(alive)
	alivePort, _ := strconv.Atoi(alivePortText)
	dead := freePort(t) // nothing listens: refused
	inbound := freePort(t)
	config := fmt.Sprintf(`{
	  "log": {"level": "info"},
	  "inbounds": [{"type": "mixed", "tag": "in", "listen": "127.0.0.1", "listen_port": %d}],
	  "outbounds": [
	    {"type": "selector", "tag": "proxy", "outbounds": ["auto", "dead", "alive"], "default": "auto"},
	    {"type": "sov-auto", "tag": "auto", "outbounds": ["dead", "alive"], "hosts": {"dead": "127.0.0.1", "alive": "127.0.0.1"}},
	    {"type": "socks", "tag": "dead", "server": "127.0.0.1", "server_port": %d},
	    {"type": "socks", "tag": "alive", "server": %q, "server_port": %d},
	    {"type": "direct", "tag": "direct"}
	  ],
	  "route": {"final": "proxy"}
	}`, inbound, dead, aliveHost, alivePort)
	ctx := coreContext(context.Background())
	var options option.Options
	if err := singjson.UnmarshalContext(ctx, []byte(config), &options); err != nil {
		t.Fatal(err)
	}
	instance, err := box.New(box.Options{Context: ctx, Options: options})
	if err != nil {
		t.Fatal(err)
	}
	traffic := trafficcontrol.NewManager()
	instance.Router().AppendTracker(traffic)
	trafficScope := adapter.NewScope(ctx, instance.LogFactory().Logger())
	if err := traffic.Start(adapter.StartStateInitialize, trafficScope); err != nil {
		t.Fatal(err)
	}
	defer trafficScope.Close()
	if err := instance.Start(); err != nil {
		t.Fatal(err)
	}
	defer instance.Close()
	defer resetDelays()

	// What box_select does: the tray's pick, the dead one.
	selected, _ := instance.Outbound().Outbound("auto")
	if !selected.(interface{ SelectOutbound(string) bool }).SelectOutbound("dead") {
		t.Fatal("select")
	}
	for round := 1; round <= 2; round++ {
		conn := socksConnect(t, "127.0.0.1:"+strconv.Itoa(inbound), site.Listener.Addr().String())
		if _, err := io.WriteString(conn, "GET / HTTP/1.1\r\nHost: auto.test\r\nConnection: close\r\n\r\n"); err != nil {
			t.Fatal(err)
		}
		_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		body, _ := io.ReadAll(conn)
		conn.Close()
		if !strings.HasSuffix(string(body), "ok") {
			t.Fatalf("round %d through sov-auto: %q", round, body)
		}
	}
	states := autoStates(time.Now())
	if s := states["dead"]; s.failedAt == 0 || s.failedWhy != "reset" {
		t.Fatalf("the dead one's state: %+v", states)
	}
	seen := RouteOf([]string{site.Listener.Addr().(*net.TCPAddr).IP.String()}, traffic.Connections(), traffic.ClosedConnections())
	if seen == nil || seen.Outbound != "alive" || seen.Type != "socks" || seen.Chain[0] != "alive" {
		t.Fatalf("route: %+v", seen)
	}
}
