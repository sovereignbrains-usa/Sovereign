package main

import (
	"context"
	"crypto/tls"
	"encoding/binary"
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

// fakeWay is a way whose server, given the hello, answers a TLS record,
// closes (DPI's reset) or says nothing.
type fakeWay struct {
	behave string // "answer", "reset", "silent", "dial"
	delay  time.Duration
}

func (f fakeWay) DialContext(ctx context.Context, _ string, _ M.Socksaddr) (net.Conn, error) {
	if f.behave == "dial" {
		return nil, fmt.Errorf("refused")
	}
	if f.behave == "hang" { // an address blocked by IP: the connect gets no answer
		<-ctx.Done()
		return nil, ctx.Err()
	}
	client, server := net.Pipe()
	go func() {
		defer server.Close()
		hello := make([]byte, 64)
		if _, err := server.Read(hello); err != nil {
			return
		}
		time.Sleep(f.delay)
		switch f.behave {
		case "answer":
			_, _ = server.Write([]byte{tlsHandshake, 3, 3, 0, 1, 2})
			_, _ = io.Copy(io.Discard, server)
		case "silent":
			<-ctx.Done()
			time.Sleep(50 * time.Millisecond)
		}
	}()
	return client, nil
}

func (f fakeWay) ListenPacket(context.Context, M.Socksaddr) (net.PacketConn, error) {
	return nil, fmt.Errorf("no packets")
}

func race(t *testing.T, wait time.Duration, ways ...fakeWay) (raceResult, time.Duration) {
	t.Helper()
	dialers := make([]N.Dialer, len(ways))
	for i, way := range ways {
		dialers[i] = way
	}
	started := time.Now()
	r := raceWays(context.Background(), dialers, M.ParseSocksaddr("example.com:443"), []byte{tlsHandshake, 3, 1}, wait)
	if r.conn != nil {
		first := make([]byte, 1)
		if _, err := r.conn.Read(first); err != nil || first[0] != tlsHandshake {
			t.Fatalf("the winner's first byte is not given back: %v %v", first, err)
		}
		r.conn.Close()
	}
	return r, time.Since(started)
}

func TestRacePreferredAnswers(t *testing.T) {
	r, took := race(t, 2*time.Second, fakeWay{behave: "answer"}, fakeWay{behave: "silent"})
	if r.index != 0 {
		t.Fatalf("the preferred way answered, kept %d (%v)", r.index, r.why)
	}
	if took > time.Second {
		t.Fatalf("a silent second way must not hold the choice: %v", took)
	}
}

func TestRacePreferredReset(t *testing.T) {
	r, _ := race(t, 2*time.Second, fakeWay{behave: "reset"}, fakeWay{behave: "answer", delay: 50 * time.Millisecond})
	if r.index != 1 || r.why[0] != "reset" {
		t.Fatalf("reset preferred: kept %d, why %v", r.index, r.why)
	}
}

func TestRacePreferredSilent(t *testing.T) {
	// The second way answered: the silent preferred gets the grace, not the
	// whole wait.
	r, took := race(t, 3*time.Second, fakeWay{behave: "silent"}, fakeWay{behave: "answer"})
	if r.index != 1 || r.why[0] != "silent" {
		t.Fatalf("silent preferred: kept %d, why %v", r.index, r.why)
	}
	if took < smartGraceWait-50*time.Millisecond || took > smartGraceWait+500*time.Millisecond {
		t.Fatalf("the choice waits the grace out, no more: %v", took)
	}
}

func TestRacePreferredSlowWithinGrace(t *testing.T) {
	// The preferred answers 400 ms after the second: still within the grace,
	// still kept.
	r, _ := race(t, 3*time.Second, fakeWay{behave: "answer", delay: 400 * time.Millisecond}, fakeWay{behave: "answer"})
	if r.index != 0 {
		t.Fatalf("a slow preferred within the grace: kept %d, why %v", r.index, r.why)
	}
}

func TestRaceAllSilentWaitsWhole(t *testing.T) {
	// Nobody answered: no grace starts, the whole wait is waited.
	r, took := race(t, 300*time.Millisecond, fakeWay{behave: "silent"}, fakeWay{behave: "silent"})
	if r.index != -1 || r.why[0] != "silent" || r.why[1] != "silent" {
		t.Fatalf("all silent: kept %d, why %v", r.index, r.why)
	}
	if took < 250*time.Millisecond {
		t.Fatalf("all silent ended early: %v", took)
	}
}

func TestRaceNoneAnswers(t *testing.T) {
	r, _ := race(t, 300*time.Millisecond, fakeWay{behave: "dial"}, fakeWay{behave: "reset"})
	if r.index != -1 || r.conn != nil || r.why[0] != "dial" || r.why[1] != "reset" {
		t.Fatalf("none answered: kept %d, why %v", r.index, r.why)
	}
}

func TestSmartMemory(t *testing.T) {
	m := &smartMemory{items: map[string]smartVerdict{}}
	now := time.Now()
	m.remember("a.example", "proxy", "reset", now)
	m.remember("b.example", "direct", "", now)
	if way, ok := m.verdict("a.example", now.Add(time.Hour)); !ok || way != "proxy" {
		t.Fatalf("verdict: %q %v", way, ok)
	}
	if _, ok := m.verdict("a.example", now.Add(smartRemember)); ok {
		t.Fatal("a verdict past smartRemember must be asked again")
	}
	learned := m.learned(now)
	if len(learned) != 1 || learned["a.example"].Way != "proxy" {
		t.Fatalf("learned holds only what the preferred way failed: %v", learned)
	}
}

// readCut reads `conn` (watched with `stall`) until it ends; the server
// side writes `send`, then stays silent until the test ends.
func readCut(t *testing.T, stall time.Duration, send []byte) (cut bool, took time.Duration) {
	t.Helper()
	client, server := net.Pipe()
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		defer server.Close()
		_, _ = server.Write(send)
		<-stop
	}()
	var cuts atomic.Int32
	conn := watchCut(client, stall, func() { cuts.Add(1) })
	defer conn.Close()
	started := time.Now()
	_ = client.SetReadDeadline(time.Now().Add(4 * stall))
	buffer := make([]byte, 64)
	for {
		if _, err := conn.Read(buffer); err != nil {
			break
		}
	}
	return cuts.Load() == 1, time.Since(started)
}

func record(size int) []byte {
	return append([]byte{0x17, 3, 3, byte(size >> 8), byte(size)}, make([]byte, size)...)
}

func TestCutMidRecord(t *testing.T) {
	cut, took := readCut(t, 200*time.Millisecond, append(record(300), record(1000)[:600]...))
	if !cut || took > 600*time.Millisecond {
		t.Fatalf("a record stopped half-way: cut %v after %v", cut, took)
	}
}

func TestNoCutBetweenRecords(t *testing.T) {
	if cut, _ := readCut(t, 200*time.Millisecond, append(record(300), record(1000)...)); cut {
		t.Fatal("a server idle between records is no cut")
	}
}

func TestNoCutNotTLS(t *testing.T) {
	if cut, _ := readCut(t, 200*time.Millisecond, []byte("HTTP/1.1 200 OK\r\nContent-Length: 9000\r\n\r\npart")); cut {
		t.Fatal("not TLS: nothing to tell a cut by")
	}
}

func TestNoCutPastWatch(t *testing.T) {
	var send []byte
	for len(send) <= smartWatchMax {
		send = append(send, record(16000)...)
	}
	if cut, _ := readCut(t, 200*time.Millisecond, append(send, record(1000)[:10]...)); cut {
		t.Fatal("past smartWatchMax a stall is not looked at")
	}
}

// cutSocks is a SOCKS5 server that connects where asked and passes the
// server's data only for its first `limit` bytes, then holds the rest - the
// "16 KB" cut.
func cutSocks(t *testing.T, limit int64) string {
	t.Helper()
	hold := make(chan struct{})
	t.Cleanup(func() { close(hold) })
	return socksServer(t, func(conn net.Conn, target string) {
		remote, err := net.DialTimeout("tcp", target, 5*time.Second)
		if err != nil {
			return
		}
		defer remote.Close()
		go func() { _, _ = io.Copy(remote, conn) }()
		_, _ = io.CopyN(conn, remote, limit)
		go func() { _, _ = io.Copy(io.Discard, remote) }()
		<-hold // the rest held, the connection open: DPI doesn't close it
	})
}

// dpiSocks is a SOCKS5 server that accepts a CONNECT and closes the
// connection on the first bytes after it - DPI cutting on the hello.
func dpiSocks(t *testing.T) string {
	t.Helper()
	return socksServer(t, func(conn net.Conn, _ string) {
		_, _ = conn.Read(make([]byte, 1)) // the hello: cut
	})
}

// socksServer accepts SOCKS5 CONNECTs and hands each, answered, to `serve`
// with the address asked for.
func socksServer(t *testing.T, serve func(conn net.Conn, target string)) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				head := make([]byte, 2)
				if _, err := io.ReadFull(conn, head); err != nil {
					return
				}
				if _, err := io.ReadFull(conn, make([]byte, head[1])); err != nil {
					return
				}
				_, _ = conn.Write([]byte{5, 0})
				request := make([]byte, 4)
				if _, err := io.ReadFull(conn, request); err != nil {
					return
				}
				size := 0
				switch request[3] {
				case 1:
					size = 4
				case 4:
					size = 16
				case 3:
					length := make([]byte, 1)
					if _, err := io.ReadFull(conn, length); err != nil {
						return
					}
					size = int(length[0])
				}
				address := make([]byte, size+2)
				if _, err := io.ReadFull(conn, address); err != nil {
					return
				}
				host := string(address[:size])
				if request[3] != 3 {
					host = net.IP(address[:size]).String()
				}
				port := int(binary.BigEndian.Uint16(address[size:]))
				_, _ = conn.Write([]byte{5, 0, 0, 1, 0, 0, 0, 0, 0, 0})
				serve(conn, net.JoinHostPort(host, strconv.Itoa(port)))
			}()
		}
	}()
	return listener.Addr().String()
}

// socksConnect opens a SOCKS5 CONNECT to `target` (an IP:port) through `proxy`.
func socksConnect(t *testing.T, proxy, target string) net.Conn {
	t.Helper()
	conn, err := net.DialTimeout("tcp", proxy, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
	host, portText, _ := net.SplitHostPort(target)
	port, _ := strconv.Atoi(portText)
	request := append([]byte{5, 1, 0, 5, 1, 0, 1}, net.ParseIP(host).To4()...)
	request = binary.BigEndian.AppendUint16(request, uint16(port))
	if _, err := conn.Write(request[:3]); err != nil {
		t.Fatal(err)
	}
	if _, err := io.ReadFull(conn, make([]byte, 2)); err != nil {
		t.Fatal(err)
	}
	if _, err := conn.Write(request[3:]); err != nil {
		t.Fatal(err)
	}
	if _, err := io.ReadFull(conn, make([]byte, 10)); err != nil {
		t.Fatal(err)
	}
	return conn
}

func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// A real sing-box with "sov-smart" as the route's final: the preferred way
// is a "DPI" that cuts on the hello, the other is direct. The client's TLS
// to a local server must go through, by direct, and the name be learned.
func TestSmartInRealBox(t *testing.T) {
	site := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "ok")
	}))
	defer site.Close()
	dpi := dpiSocks(t)
	dpiHost, dpiPortText, _ := net.SplitHostPort(dpi)
	dpiPort, _ := strconv.Atoi(dpiPortText)
	inbound := freePort(t)
	config := fmt.Sprintf(`{
	  "log": {"level": "info"},
	  "inbounds": [{"type": "mixed", "tag": "in", "listen": "127.0.0.1", "listen_port": %d}],
	  "outbounds": [
	    {"type": "socks", "tag": "dpi", "server": %q, "server_port": %d},
	    {"type": "direct", "tag": "direct"},
	    {"type": "sov-smart", "tag": "smart", "outbounds": ["dpi", "direct"], "race_ms": 1500}
	  ],
	  "route": {"rules": [{"action": "sniff"}], "final": "smart"}
	}`, inbound, dpiHost, dpiPort)
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

	const name = "smart-dpi.test"
	forget(name, "ip:127.0.0.1")

	for round := 1; round <= 2; round++ { // the second goes by the memory
		conn := socksConnect(t, "127.0.0.1:"+strconv.Itoa(inbound), site.Listener.Addr().String())
		client := tls.Client(conn, &tls.Config{ServerName: name, InsecureSkipVerify: true}) //nolint:gosec // a local test server
		if err := client.Handshake(); err != nil {
			t.Fatalf("round %d: TLS through sov-smart: %v", round, err)
		}
		if _, err := io.WriteString(client, "GET / HTTP/1.1\r\nHost: "+name+"\r\nConnection: close\r\n\r\n"); err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(client)
		client.Close()
		if len(body) == 0 || string(body[len(body)-2:]) != "ok" {
			t.Fatalf("round %d: the page: %q", round, body)
		}
	}
	v, ok := smartMemo.learned(time.Now())[name]
	if !ok || v.Way != "direct" || v.Why != "reset" {
		t.Fatalf("learned: %+v %v", v, ok)
	}
	// The tray sees the way taken, not "smart": the flows by it, the route to the server.
	flows := FlowTotals(traffic.Connections(), traffic.ClosedConnections())
	if _, bySmart := flows["smart"]; bySmart || flows["direct"][1] == 0 {
		t.Fatalf("flows: %v", flows)
	}
	seen := RouteOf([]string{name}, traffic.Connections(), traffic.ClosedConnections())
	if seen == nil || seen.Outbound != "direct" || seen.Type != "direct" || strings.Join(seen.Chain, ",") != "direct,smart" {
		t.Fatalf("route: %+v", seen)
	}
}

func forget(keys ...string) {
	smartMemo.mu.Lock()
	defer smartMemo.mu.Unlock()
	for _, key := range keys {
		delete(smartMemo.items, key)
	}
}

// A real sing-box, the preferred way passing the server's first 16 KB and
// holding the rest: the first page is lost, cut; the name and the address
// learned for direct; the second page comes whole.
func TestSmartCutInRealBox(t *testing.T) {
	page := strings.Repeat("x", 64*1024)
	site := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, page)
	}))
	defer site.Close()
	cutHost, cutPortText, _ := net.SplitHostPort(cutSocks(t, 16*1024))
	cutPort, _ := strconv.Atoi(cutPortText)
	inbound := freePort(t)
	config := fmt.Sprintf(`{
	  "log": {"level": "info"},
	  "inbounds": [{"type": "mixed", "tag": "in", "listen": "127.0.0.1", "listen_port": %d}],
	  "outbounds": [
	    {"type": "socks", "tag": "cut", "server": %q, "server_port": %d},
	    {"type": "direct", "tag": "direct"},
	    {"type": "sov-smart", "tag": "smart", "outbounds": ["cut", "direct"], "race_ms": 1000}
	  ],
	  "route": {"rules": [{"action": "sniff"}], "final": "smart"}
	}`, inbound, cutHost, cutPort)
	ctx := coreContext(context.Background())
	var options option.Options
	if err := singjson.UnmarshalContext(ctx, []byte(config), &options); err != nil {
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

	const name = "smart-cut.test"
	forget(name, "ip:127.0.0.1")
	defer forget(name, "ip:127.0.0.1")
	fetch := func() (int, time.Duration) {
		started := time.Now()
		conn := socksConnect(t, "127.0.0.1:"+strconv.Itoa(inbound), site.Listener.Addr().String())
		client := tls.Client(conn, &tls.Config{ServerName: name, InsecureSkipVerify: true}) //nolint:gosec // a local test server
		defer client.Close()
		if err := client.Handshake(); err != nil {
			t.Fatalf("TLS through sov-smart: %v", err)
		}
		_, _ = io.WriteString(client, "GET / HTTP/1.1\r\nHost: "+name+"\r\nConnection: close\r\n\r\n")
		body, _ := io.ReadAll(client)
		return len(body), time.Since(started)
	}
	if got, took := fetch(); got >= len(page) || took > 5*time.Second {
		t.Fatalf("the first page through the cut: %d bytes in %v", got, took)
	}
	learned := smartMemo.learned(time.Now())
	if learned[name].Way != "direct" || learned[name].Why != "cut" || learned["ip:127.0.0.1"].Way != "direct" {
		t.Fatalf("learned: %+v", learned)
	}
	if got, _ := fetch(); got < len(page) {
		t.Fatalf("the second page, direct: %d bytes", got)
	}
}

// What is not TLS has no hello to race: the dialing finds the way - a connect
// that gets no answer (blocked by IP) or is refused goes to the next way, the
// address remembered for it; a way that connects stays as it is.
func TestFallbackDial(t *testing.T) {
	ways := func(first, second string) []adapter.Outbound {
		return []adapter.Outbound{fallbackWay{fakeWay{behave: first}, "direct"}, fallbackWay{fakeWay{behave: second}, "vpn"}}
	}
	run := func(first, second string) (time.Duration, string, error) {
		forget("ip:203.0.113.9")
		defer forget("ip:203.0.113.9")
		d := &fallbackDialer{ways: ways(first, second), wait: 200 * time.Millisecond, ip: "ip:203.0.113.9", logger: log.NewNOPFactory().Logger()}
		started := time.Now()
		conn, err := d.DialContext(context.Background(), N.NetworkTCP, M.ParseSocksaddr("203.0.113.9:443"))
		if conn != nil {
			conn.Close()
		}
		v := smartMemo.learned(time.Now())["ip:203.0.113.9"]
		return time.Since(started), v.Way + "/" + v.Why, err
	}
	if took, mem, err := run("answer", "answer"); err != nil || mem != "/" {
		t.Fatalf("the preferred connects: %v %q %v", took, mem, err)
	}
	if took, mem, err := run("hang", "answer"); err != nil || mem != "vpn/dial" || took < 150*time.Millisecond || took > 2*time.Second {
		t.Fatalf("silent connect: %v %q %v", took, mem, err)
	}
	if _, mem, err := run("dial", "answer"); err != nil || mem != "vpn/dial" {
		t.Fatalf("refused connect: %q %v", mem, err)
	}
	if _, mem, err := run("hang", "dial"); err == nil || mem != "/" {
		t.Fatalf("no way connects: %q %v", mem, err)
	}
}

// fallbackWay: a fakeWay with a tag, to stand as an outbound.
type fallbackWay struct {
	fakeWay
	tag string
}

func (w fallbackWay) Tag() string            { return w.tag }
func (w fallbackWay) Type() string           { return "fake" }
func (w fallbackWay) Network() []string      { return []string{N.NetworkTCP} }
func (w fallbackWay) Dependencies() []string { return nil }
