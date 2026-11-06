package main

// A server's UDP, for games and calls (06.11.2026, his: 'что на счет
// джитера мб кто с ху2 играть будет'). The latency test times web requests
// over TCP; a game sends small packets over UDP and feels how evenly they
// come back, and there the protocols differ for real: Hysteria 2 and TUIC
// carry each packet as a QUIC datagram of its own, a lost one holds nothing
// up; AnyTLS, VLESS and Shadowsocks over TCP carry them inside a stream,
// where one lost segment holds back everything behind it; NaïveProxy without
// udp_over_tcp carries none.
//
// The probe: STUN binding requests (20 bytes each way) through the outbound
// to a public STUN server, one every udpSpacing, udpProbes of them, after
// one that's answered first - the outbound's first packet may wait for a
// connection to its server (VLESS dials one, TUIC may), and that is no
// round trip. Reported: the median round trip, the jitter - how far one
// round trip lies from the one before, on average (what RFC 3550 smooths
// into its interarrival jitter) - and the share lost.

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"net"
	"slices"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
)

const (
	udpProbes   = 20
	udpSpacing  = 50 * time.Millisecond
	udpTail     = time.Second     // how long the last request may take to come back
	udpFirstTry = time.Second     // the opening request: resent after this
	udpOpening  = 3               // that many times before UDP counts as not getting through
	udpDial     = 3 * time.Second // the outbound's packet connection, its server's handshake
	udpBudget   = 8 * time.Second // all of it, both STUN servers: it runs beside the TCP test, not after it
)

// errNoUDP: the outbound carries no UDP at all (its Network() lacks it).
const errNoUDP = "no udp"

// The STUN servers asked, by name - the server end resolves it, nearest
// to the exit. The second when the first gives no answer.
var stunTargets = []M.Socksaddr{
	M.ParseSocksaddrHostPort("stun.l.google.com", 19302),
	M.ParseSocksaddrHostPort("stun.cloudflare.com", 3478),
}

type udpResult struct {
	Delay  time.Duration   // the median round trip
	Jitter time.Duration   // the mean difference of consecutive round trips
	Loss   int             // percent of the requests lost
	Error  string          // errNoUDP, or why no request came back; Delay is 0 then
	rtts   []time.Duration // the round trips, in the order sent: for the live test's log
}

// probeUDP measures `outbound`'s UDP against the STUN servers.
func probeUDP(ctx context.Context, outbound N.Dialer, networks []string) udpResult {
	if !slices.Contains(networks, N.NetworkUDP) {
		return udpResult{Error: errNoUDP}
	}
	ctx, cancel := context.WithTimeout(ctx, udpBudget)
	defer cancel()
	var result udpResult
	for _, target := range stunTargets {
		result = probeUDPTo(ctx, outbound, target)
		if result.Error == "" || ctx.Err() != nil {
			break
		}
	}
	return result
}

// probeUDPTo: the probe against one STUN server.
func probeUDPTo(ctx context.Context, outbound N.Dialer, target M.Socksaddr) udpResult {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	dialCtx, dialCancel := context.WithTimeout(ctx, udpDial)
	conn, err := outbound.ListenPacket(dialCtx, target)
	dialCancel()
	if err != nil {
		return udpResult{Error: err.Error()}
	}
	session := newStunSession(conn, target)
	defer session.close()
	go func() {
		<-ctx.Done()
		session.close() // unblocks the reader
	}()
	go session.read()

	// The opening request, until one comes back.
	opened := false
	for try := 0; try < udpOpening && !opened && ctx.Err() == nil; try++ {
		index := udpProbes + try // past the measured ones
		if err := session.send(index); err != nil {
			return udpResult{Error: err.Error()}
		}
		opened = session.wait(ctx, func() bool { return session.answered(index) }, udpFirstTry)
	}
	if !opened {
		if err := session.failure(); err != nil {
			return udpResult{Error: err.Error()}
		}
		return udpResult{Error: "no answer"}
	}

	asked := 0
	for asked < udpProbes && ctx.Err() == nil && session.failure() == nil {
		if err := session.send(asked); err != nil {
			break
		}
		asked++
		session.wait(ctx, func() bool { return false }, udpSpacing)
	}
	if asked == 0 {
		return udpResult{Error: "no answer"}
	}
	session.wait(ctx, func() bool { return session.answeredAll(asked) }, udpTail)
	return session.result(asked)
}

// One packet connection's requests and answers. A request's transaction id
// is a random 8 bytes of the session and the request's index.
type stunSession struct {
	conn    net.PacketConn
	target  M.Socksaddr
	id      [8]byte
	mu      sync.Mutex
	changed chan struct{} // a request answered
	sent    map[int]time.Time
	rtt     map[int]time.Duration
	readErr error
	closed  sync.Once
}

func newStunSession(conn net.PacketConn, target M.Socksaddr) *stunSession {
	s := &stunSession{conn: conn, target: target, changed: make(chan struct{}, 1),
		sent: map[int]time.Time{}, rtt: map[int]time.Duration{}}
	_, _ = rand.Read(s.id[:])
	return s
}

func (s *stunSession) close() { s.closed.Do(func() { _ = s.conn.Close() }) }

func (s *stunSession) send(index int) error {
	s.mu.Lock()
	s.sent[index] = time.Now()
	s.mu.Unlock()
	_, err := s.conn.WriteTo(stunRequest(s.id, index), s.target)
	return err
}

func (s *stunSession) read() {
	// The outbound's packet code runs here, on a goroutine of ours: a panic
	// in it ends the reading as an error would, not the service.
	defer catchPanic("udp test: read", func(message string) {
		s.mu.Lock()
		s.readErr = errors.New(message)
		s.mu.Unlock()
		s.signal()
	})
	buffer := make([]byte, 1500)
	for {
		n, _, err := s.conn.ReadFrom(buffer)
		if err != nil {
			s.mu.Lock()
			s.readErr = err
			s.mu.Unlock()
			s.signal()
			return
		}
		now := time.Now()
		index, ok := stunAnswer(buffer[:n], s.id)
		if !ok {
			continue
		}
		s.mu.Lock()
		at, asked := s.sent[index]
		if _, seen := s.rtt[index]; asked && !seen {
			s.rtt[index] = now.Sub(at)
		}
		s.mu.Unlock()
		s.signal()
	}
}

func (s *stunSession) signal() {
	select {
	case s.changed <- struct{}{}:
	default:
	}
}

// wait: till done() holds, for at most `d`. Whether it holds.
func (s *stunSession) wait(ctx context.Context, done func() bool, d time.Duration) bool {
	timer := time.NewTimer(d)
	defer timer.Stop()
	for !done() {
		select {
		case <-ctx.Done():
			return done()
		case <-timer.C:
			return done()
		case <-s.changed:
			s.mu.Lock()
			failed := s.readErr != nil
			s.mu.Unlock()
			if failed {
				return done()
			}
		}
	}
	return true
}

func (s *stunSession) answered(index int) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.rtt[index]
	return ok
}

func (s *stunSession) answeredAll(count int) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for index := range count {
		if _, ok := s.rtt[index]; !ok {
			return false
		}
	}
	return true
}

// failure: why reading stopped, unless the session was closed by us.
func (s *stunSession) failure() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.readErr == nil || errors.Is(s.readErr, net.ErrClosed) {
		return nil
	}
	return s.readErr
}

// result: the figures of requests 0..count-1 in the order they were sent.
func (s *stunSession) result(count int) udpResult {
	s.mu.Lock()
	defer s.mu.Unlock()
	rtts := make([]time.Duration, 0, count)
	for index := range count {
		if rtt, ok := s.rtt[index]; ok {
			rtts = append(rtts, rtt)
		}
	}
	return udpFigures(rtts, count)
}

// udpFigures: the median, the jitter and the loss of the round trips that
// came back, in the order sent, of `asked` requests.
func udpFigures(rtts []time.Duration, asked int) udpResult {
	if len(rtts) == 0 {
		return udpResult{Error: "no answer", Loss: 100}
	}
	var jitter time.Duration
	if len(rtts) > 1 {
		var sum time.Duration
		for i := 1; i < len(rtts); i++ {
			sum += (rtts[i] - rtts[i-1]).Abs()
		}
		jitter = sum / time.Duration(len(rtts)-1)
	}
	return udpResult{
		Delay:  max(median(rtts), time.Millisecond),
		Jitter: jitter,
		Loss:   (asked - len(rtts)) * 100 / asked,
		rtts:   rtts,
	}
}

// RFC 8489: a binding request, no attributes.
const (
	stunBindingRequest = 0x0001
	stunBindingSuccess = 0x0101
	stunMagicCookie    = 0x2112A442
)

func stunRequest(id [8]byte, index int) []byte {
	packet := make([]byte, 20)
	binary.BigEndian.PutUint16(packet[0:], stunBindingRequest)
	binary.BigEndian.PutUint16(packet[2:], 0)
	binary.BigEndian.PutUint32(packet[4:], stunMagicCookie)
	copy(packet[8:16], id[:])
	binary.BigEndian.PutUint32(packet[16:], uint32(index))
	return packet
}

// stunAnswer: the index of the request `packet` answers, if it's a binding
// success response to one of this session's.
func stunAnswer(packet []byte, id [8]byte) (int, bool) {
	if len(packet) < 20 ||
		binary.BigEndian.Uint16(packet[0:]) != stunBindingSuccess ||
		binary.BigEndian.Uint32(packet[4:]) != stunMagicCookie ||
		[8]byte(packet[8:16]) != id {
		return 0, false
	}
	return int(binary.BigEndian.Uint32(packet[16:])), true
}

// The UDP probe of a tested outbound: networks from its adapter.
func probeOutboundUDP(ctx context.Context, outbound adapter.Outbound) udpResult {
	return probeUDP(ctx, outbound, outbound.Network())
}
