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
	"testing"
	"time"

	box "github.com/sagernet/sing-box"
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
	r, took := race(t, 300*time.Millisecond, fakeWay{behave: "silent"}, fakeWay{behave: "answer"})
	if r.index != 1 || r.why[0] != "silent" {
		t.Fatalf("silent preferred: kept %d, why %v", r.index, r.why)
	}
	if took < 250*time.Millisecond || took > 2*time.Second {
		t.Fatalf("the choice waits the preferred's patience out, no more: %v", took)
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

// dpiSocks is a SOCKS5 server that accepts a CONNECT and closes the
// connection on the first bytes after it - DPI cutting on the hello.
func dpiSocks(t *testing.T) string {
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
				skip := 0
				switch request[3] {
				case 1:
					skip = 4
				case 4:
					skip = 16
				case 3:
					size := make([]byte, 1)
					if _, err := io.ReadFull(conn, size); err != nil {
						return
					}
					skip = int(size[0])
				}
				if _, err := io.ReadFull(conn, make([]byte, skip+2)); err != nil {
					return
				}
				_, _ = conn.Write([]byte{5, 0, 0, 1, 0, 0, 0, 0, 0, 0})
				_, _ = conn.Read(make([]byte, 1)) // the hello: cut
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
	if err := instance.Start(); err != nil {
		t.Fatal(err)
	}
	defer instance.Close()

	const name = "smart-dpi.test"
	smartMemo.mu.Lock()
	delete(smartMemo.items, name)
	smartMemo.mu.Unlock()

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
}
