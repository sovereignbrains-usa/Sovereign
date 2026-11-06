package main

import (
	"context"
	"encoding/binary"
	"net"
	"testing"
	"time"

	M "github.com/sagernet/sing/common/metadata"
)

func TestStunPackets(t *testing.T) {
	id := [8]byte{1, 2, 3, 4, 5, 6, 7, 8}
	request := stunRequest(id, 7)
	if len(request) != 20 || binary.BigEndian.Uint16(request) != stunBindingRequest ||
		binary.BigEndian.Uint32(request[4:]) != stunMagicCookie {
		t.Fatalf("request: % x", request)
	}
	answer := append([]byte(nil), request...)
	binary.BigEndian.PutUint16(answer, stunBindingSuccess)
	if index, ok := stunAnswer(answer, id); !ok || index != 7 {
		t.Fatalf("answer: %d %v", index, ok)
	}
	if _, ok := stunAnswer(request, id); ok {
		t.Fatal("a request is no answer")
	}
	if _, ok := stunAnswer(answer, [8]byte{9}); ok {
		t.Fatal("another session's answer")
	}
	if _, ok := stunAnswer(answer[:19], id); ok {
		t.Fatal("short")
	}
}

func TestUdpFigures(t *testing.T) {
	r := udpFigures(ms(50, 52, 48, 50), 5)
	if r.Error != "" || r.Delay != 50*time.Millisecond || r.Loss != 20 {
		t.Fatalf("%+v", r)
	}
	if r.Jitter != 8*time.Millisecond/3 { // |2|+|4|+|2| over 3
		t.Fatalf("jitter %v", r.Jitter)
	}
	if r := udpFigures(nil, 20); r.Error == "" || r.Loss != 100 {
		t.Fatalf("none back: %+v", r)
	}
	if r := udpFigures(ms(40), 20); r.Jitter != 0 || r.Loss != 95 {
		t.Fatalf("one back: %+v", r)
	}
}

// A dialer whose packets go to a local STUN responder, whatever the address.
type localStun struct {
	responder net.Addr
}

func (d localStun) DialContext(context.Context, string, M.Socksaddr) (net.Conn, error) {
	return nil, net.ErrClosed
}

func (d localStun) ListenPacket(context.Context, M.Socksaddr) (net.PacketConn, error) {
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	return redirected{conn, d.responder}, nil
}

type redirected struct {
	net.PacketConn
	to net.Addr
}

func (r redirected) WriteTo(p []byte, _ net.Addr) (int, error) { return r.PacketConn.WriteTo(p, r.to) }

// serveStun answers binding requests, dropping those `drop` says.
func serveStun(t *testing.T, drop func(index int) bool) net.Addr {
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	go func() {
		buffer := make([]byte, 1500)
		for {
			n, from, err := conn.ReadFrom(buffer)
			if err != nil {
				return
			}
			if n != 20 || binary.BigEndian.Uint16(buffer) != stunBindingRequest {
				continue
			}
			if drop(int(binary.BigEndian.Uint32(buffer[16:]))) {
				continue
			}
			binary.BigEndian.PutUint16(buffer, stunBindingSuccess)
			_, _ = conn.WriteTo(buffer[:20], from)
		}
	}()
	return conn.LocalAddr()
}

func TestProbeUDP(t *testing.T) {
	responder := serveStun(t, func(index int) bool { return index < udpProbes && index%4 == 3 })
	started := time.Now()
	r := probeUDP(context.Background(), localStun{responder}, []string{"tcp", "udp"})
	if r.Error != "" || r.Delay <= 0 || r.Loss != 25 {
		t.Fatalf("%+v", r)
	}
	if took := time.Since(started); took > 3*time.Second {
		t.Fatalf("took %v", took) // 20 × 50 ms, and the tail for the lost
	}
	if r := probeUDP(context.Background(), localStun{responder}, []string{"tcp"}); r.Error != errNoUDP {
		t.Fatalf("tcp only: %+v", r)
	}
}
