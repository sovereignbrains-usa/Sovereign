package main

// Smart routing, its first step (04.11.2026): the client decides the way of
// a site itself instead of the user's lists. An outbound of our own,
// "sov-smart", sits where a rule would name "direct" or the proxy; for a TLS
// connection to a name it knows nothing about it sends the client's hello
// down every way at once and keeps the first way, in the order given, whose
// server answered with a TLS handshake. What the race decided is remembered
// for the name, so the next connection goes straight there.
//
// Why the hello and not a plain dial: Russia's DPI lets the TCP handshake
// through and cuts the connection on the ClientHello's SNI (a reset, or
// silence), so a connect that succeeds says nothing; the server's first
// record does. The router hands an outbound that is an
// adapter.ConnectionHandler the whole connection (route.go), the sniffed
// hello still cached in it - that is what lets the race live in an outbound
// and not in a fork of sing-box.
//
// The "16 KB" cut (a handshake let through, the server's data dropped after
// the first ~16 KB - by the server's address, its hosting) a race can't see:
// a way with a way after it is watched (cutConn) - the server's data stopped
// in the middle of a TLS record while the client waits for it - and the name
// and the address go to the next way; that connection is lost, the next goes
// there (04.11.2026).
//
// Not yet: a slowdown, a site refusing the server's country; and the memory
// is one for every network.

/*
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"os"
	"sort"
	"strings"
	"sync"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/adapter/outbound"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing/common/buf"
	"github.com/sagernet/sing/common/bufio"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/control"
	"github.com/sagernet/sing/common/logger"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
	"github.com/sagernet/sing/service"
)

const (
	smartType      = "sov-smart"
	smartRaceWait  = 3 * time.Second // how long a way may stay silent after the hello
	smartHelloWait = 2 * time.Second // the client's hello, if the sniffer hasn't kept it
	smartGraceWait = time.Second     // a silent way's wait once a later way answered
	smartHelloMax  = 16 * 1024       // a ClientHello with a post-quantum share is ~1.8 KB
	smartRemember  = 6 * time.Hour   // a verdict asked again after this
	smartKeep      = 4096            // names remembered
	smartWatchMax  = 64 * 1024       // the cut is looked for in the server's first bytes, this many
	tlsRecordMax   = 18432           // 2^14 and the most a cipher adds
	tlsHandshake   = 0x16            // TLS record type of a ServerHello
	tlsAlert       = 0x15
)

// smartOptions: the ways, the preferred first ("direct" first: only what
// direct can't open goes through the server; the proxy first: only what the
// server can't open goes direct), and the race's patience in milliseconds.
type smartOptions struct {
	Outbounds []string `json:"outbounds"`
	RaceMS    int      `json:"race_ms,omitempty"`
}

func registerSmart(registry *outbound.Registry) {
	outbound.Register[smartOptions](registry, smartType, newSmart)
}

// coreContext is include.Context with one outbound type more, "sov-smart":
// a config without it parses and runs exactly as `sing-box run` would.
func coreContext(ctx context.Context) context.Context {
	smartLiveMu.Lock()
	smartLive = map[string]*smartOutbound{} // a new box: the old one's are gone
	smartLiveMu.Unlock()
	outbounds := include.OutboundRegistry()
	registerSmart(outbounds)
	return box.Context(ctx, include.InboundRegistry(), outbounds, include.EndpointRegistry(),
		include.DNSTransportRegistry(), include.ServiceRegistry(), include.CertificateProviderRegistry())
}

type smartOutbound struct {
	outbound.Adapter
	ctx         context.Context
	logger      logger.ContextLogger
	manager     adapter.OutboundManager
	connections adapter.ConnectionManager
	tags        []string
	ways        []adapter.Outbound
	wait        time.Duration
}

var (
	_ adapter.ConnectionHandler = (*smartOutbound)(nil)
	_ adapter.Lifecycle         = (*smartOutbound)(nil)
)

func newSmart(ctx context.Context, _ adapter.Router, logger log.ContextLogger, tag string, options smartOptions) (adapter.Outbound, error) {
	if len(options.Outbounds) == 0 {
		return nil, E.New("missing outbounds")
	}
	wait := smartRaceWait
	if options.RaceMS > 0 {
		wait = time.Duration(options.RaceMS) * time.Millisecond
	}
	return &smartOutbound{
		Adapter:     outbound.NewAdapter(smartType, tag, []string{N.NetworkTCP, N.NetworkUDP}, options.Outbounds),
		ctx:         ctx,
		logger:      logger,
		manager:     service.FromContext[adapter.OutboundManager](ctx),
		connections: service.FromContext[adapter.ConnectionManager](ctx),
		tags:        options.Outbounds,
		wait:        wait,
	}, nil
}

func (s *smartOutbound) Start(stage adapter.StartStage, _ *adapter.Scope) error {
	if stage != adapter.StartStateStart {
		return nil
	}
	for i, tag := range s.tags {
		way, loaded := s.manager.Outbound(tag)
		if !loaded {
			return E.New("outbound ", i, " not found: ", tag)
		}
		s.ways = append(s.ways, way)
	}
	smartLiveMu.Lock()
	smartLive[s.Tag()] = s
	smartLiveMu.Unlock()
	s.followNetwork()
	return nil
}

// followNetwork: the memory is the one of the network the machine is on,
// and changes with it (home, a hotspot).
func (s *smartOutbound) followNetwork() {
	manager := service.FromContext[adapter.NetworkManager](s.ctx)
	if manager == nil {
		return
	}
	monitor := manager.InterfaceMonitor()
	if monitor == nil {
		return
	}
	smartMemo.switchNet(smartNetKey(monitor.DefaultInterface()), time.Now())
	monitor.RegisterCallback(func(ifc *control.Interface, _ int) {
		smartMemo.switchNet(smartNetKey(ifc), time.Now())
	})
}

// The running box's smart outbounds by tag: box_flows and box_route tell a
// connection's real way by them, not "sov-smart".
var (
	smartLiveMu sync.Mutex
	smartLive   = map[string]*smartOutbound{}
)

// smartWay: for a connection a rule sent to the smart outbound `tag`, the
// way it goes - the one remembered for its name, else the preferred.
// False: `tag` is no smart outbound.
func smartWay(tag string, metadata *adapter.InboundContext) (adapter.Outbound, bool) {
	smartLiveMu.Lock()
	s := smartLive[tag]
	smartLiveMu.Unlock()
	if s == nil || len(s.ways) == 0 {
		return nil, false
	}
	return s.wayOf(metadata), true
}

// verdictOf: the way remembered for the connection's address (a cut: every
// name there is cut), else for its name.
func verdictOf(metadata *adapter.InboundContext, now time.Time) (string, bool) {
	if tag, ok := smartMemo.verdict(smartIPKey(metadata), now); ok {
		return tag, true
	}
	return smartMemo.verdict(smartKey(metadata), now)
}

// wayOf: the remembered way for the connection, else the preferred.
func (s *smartOutbound) wayOf(metadata *adapter.InboundContext) adapter.Outbound {
	if tag, ok := verdictOf(metadata, time.Now()); ok {
		return s.byTag(tag)
	}
	return s.ways[0]
}

// way: wayOf the connection in ctx.
func (s *smartOutbound) way(ctx context.Context) adapter.Outbound {
	if metadata := adapter.ContextFrom(ctx); metadata != nil {
		return s.wayOf(metadata)
	}
	return s.ways[0]
}

// smartIPKey: the connection's address as a memory key, "ip:1.2.3.4", or "".
func smartIPKey(metadata *adapter.InboundContext) string {
	if !metadata.Destination.IsIP() {
		return ""
	}
	return "ip:" + metadata.Destination.Addr.Unmap().String()
}

// watched: `way` (the index-th) with its connections watched for the cut,
// when a way after it is left to fall to; else `way` itself.
func (s *smartOutbound) watched(index int, key string, metadata *adapter.InboundContext) N.Dialer {
	if index < 0 || index+1 >= len(s.ways) {
		return s.ways[max(index, 0)]
	}
	return &cutWatchDialer{Dialer: s.ways[index], stall: s.wait, onCut: s.onCut(index, key, metadata)}
}

// onCut: what a cut on the index-th way does - the name and the address
// remembered for the next way.
func (s *smartOutbound) onCut(index int, key string, metadata *adapter.InboundContext) func() {
	from, next := s.ways[index].Tag(), s.ways[index+1].Tag()
	ip := smartIPKey(metadata)
	return func() {
		now := time.Now()
		smartMemo.remember(key, next, "cut", now)
		smartMemo.remember(ip, next, "cut", now)
		s.logger.Info("smart: ", key, " ", ip, " cut after the handshake on ", from, ", now via ", next)
	}
}

func (s *smartOutbound) indexOf(tag string) int {
	for i, way := range s.ways {
		if way.Tag() == tag {
			return i
		}
	}
	return -1
}

func (s *smartOutbound) DialContext(ctx context.Context, network string, destination M.Socksaddr) (net.Conn, error) {
	return s.way(ctx).DialContext(ctx, network, destination)
}

func (s *smartOutbound) ListenPacket(ctx context.Context, destination M.Socksaddr) (net.PacketConn, error) {
	return s.way(ctx).ListenPacket(ctx, destination)
}

// smartKey: the name a connection is for, lowercased, or "".
func smartKey(metadata *adapter.InboundContext) string {
	name := metadata.Domain
	if name == "" {
		name = metadata.Destination.Fqdn
	}
	return strings.ToLower(strings.TrimSuffix(name, "."))
}

func (s *smartOutbound) NewConnection(ctx context.Context, conn net.Conn, metadata adapter.InboundContext, onClose N.CloseHandlerFunc) {
	key := smartKey(&metadata)
	now := time.Now()
	if tag, ok := verdictOf(&metadata, now); ok {
		var dialer N.Dialer = s.byTag(tag)
		if metadata.Protocol == constant.ProtocolTLS {
			dialer = s.watched(s.indexOf(tag), key, &metadata)
		}
		s.connections.NewConnection(ctx, dialer, conn, metadata, onClose)
		return
	}
	// Only what the sniffer saw as TLS races: a protocol whose server speaks
	// first (SSH, SMTP) would wait for a hello that never comes.
	if key == "" || metadata.Protocol != constant.ProtocolTLS || len(s.ways) < 2 {
		s.connections.NewConnection(ctx, s.dialFallback(&metadata), conn, metadata, onClose)
		return
	}
	hello := buf.NewSize(smartHelloMax)
	_ = conn.SetReadDeadline(now.Add(smartHelloWait))
	_, err := hello.ReadOnceFrom(conn)
	_ = conn.SetReadDeadline(time.Time{})
	if err != nil || hello.IsEmpty() || hello.Byte(0) != tlsHandshake {
		// Not TLS (or the client said nothing): the preferred way, the bytes
		// read given back.
		s.connections.NewConnection(ctx, s.dialFallback(&metadata), bufio.NewCachedConn(conn, hello), metadata, onClose)
		return
	}
	dialers := make([]N.Dialer, len(s.ways))
	for i, way := range s.ways {
		dialers[i] = way
	}
	result := raceWays(ctx, dialers, metadata.Destination, hello.Bytes(), s.wait)
	if result.index < 0 {
		// Nothing answered: the preferred way as usual, its error the one
		// the user sees; nothing remembered, the next connection races again.
		s.logger.InfoContext(ctx, "smart: ", key, " answered by no way (", strings.Join(result.why, ", "), ")")
		s.connections.NewConnection(ctx, s.ways[0], bufio.NewCachedConn(conn, hello), metadata, onClose)
		return
	}
	hello.Release()
	tag := s.ways[result.index].Tag()
	why := ""
	if result.index > 0 {
		why = strings.Join(result.why[:result.index], ", ")
	}
	smartMemo.remember(key, tag, why, now)
	if why != "" {
		s.logger.InfoContext(ctx, "smart: ", key, " via ", tag, ": ", why)
	}
	won := result.conn
	if result.index+1 < len(s.ways) {
		won = watchCut(won, s.wait, s.onCut(result.index, key, &metadata))
	}
	s.connections.NewConnection(ctx, &preDialed{conn: won}, conn, metadata, onClose)
}

// dialFallback: the dialer for what is not TLS (Telegram's own protocol, SSH,
// mail): no hello to race, so the way is found by the dialing itself.
func (s *smartOutbound) dialFallback(metadata *adapter.InboundContext) N.Dialer {
	if len(s.ways) < 2 {
		return s.ways[0]
	}
	return &fallbackDialer{ways: s.ways, wait: s.wait, ip: smartIPKey(metadata), logger: s.logger}
}

// fallbackDialer: each way in turn, the connect given `wait` (the last, the
// whole of the caller's patience). An address blocked by IP gives no answer
// to the connect at all - the way after it takes the connection, and the
// address is remembered for it ("dial"), so the next connection there goes
// straight. Nothing is learned of a name: what answered on the way is not
// known to be the same site, only the address.
type fallbackDialer struct {
	ways   []adapter.Outbound
	wait   time.Duration
	ip     string
	logger log.ContextLogger
}

func (d *fallbackDialer) DialContext(ctx context.Context, network string, destination M.Socksaddr) (net.Conn, error) {
	var last error
	for i, way := range d.ways {
		attempt, cancel := ctx, context.CancelFunc(func() {})
		if i+1 < len(d.ways) && N.NetworkName(network) == N.NetworkTCP {
			attempt, cancel = context.WithTimeout(ctx, d.wait)
		}
		conn, err := way.DialContext(attempt, network, destination)
		cancel()
		if err == nil {
			if i > 0 && d.ip != "" {
				smartMemo.remember(d.ip, way.Tag(), "dial", time.Now())
				d.logger.InfoContext(ctx, "smart: ", d.ip, " did not connect on ", d.ways[0].Tag(), " (", failure(last), "), now via ", way.Tag())
			}
			return conn, nil
		}
		last = err
		if ctx.Err() != nil {
			break
		}
	}
	return nil, last
}

func (d *fallbackDialer) ListenPacket(ctx context.Context, destination M.Socksaddr) (net.PacketConn, error) {
	return d.ways[0].ListenPacket(ctx, destination)
}

// cutWatchDialer: a way whose connections are watched for the cut.
type cutWatchDialer struct {
	N.Dialer
	stall time.Duration
	onCut func()
}

func (d *cutWatchDialer) DialContext(ctx context.Context, network string, destination M.Socksaddr) (net.Conn, error) {
	conn, err := d.Dialer.DialContext(ctx, network, destination)
	if err != nil || N.NetworkName(network) != N.NetworkTCP {
		return conn, err
	}
	return watchCut(conn, d.stall, d.onCut), nil
}

// cutConn: a server's connection whose first smartWatchMax bytes are read as
// TLS records. A Read waiting `stall` with a record begun and not finished
// is the cut: an idle server stops between records, a client not reading
// (its own Read not called) is no stall. onCut is called once and the
// connection closed - the client sees it end, and its next one goes the
// next way.
type cutConn struct {
	net.Conn
	stall time.Duration
	onCut func()

	mu      sync.Mutex
	timer   *time.Timer
	done    bool // not watched any more: past smartWatchMax, not TLS, closed or cut
	total   int
	head    [5]byte // the record header being read
	headN   int
	left    int // the current record's bytes still to come
	reading bool
	since   time.Time // the Read waiting began
}

func watchCut(conn net.Conn, stall time.Duration, onCut func()) net.Conn {
	c := &cutConn{Conn: conn, stall: stall, onCut: onCut}
	c.timer = time.AfterFunc(time.Hour, c.check)
	c.timer.Stop()
	return c
}

func (c *cutConn) Read(p []byte) (int, error) {
	c.mu.Lock()
	watching := !c.done
	if watching {
		c.reading = true
		c.since = time.Now()
		c.timer.Reset(c.stall)
	}
	c.mu.Unlock()
	n, err := c.Conn.Read(p)
	if !watching {
		return n, err
	}
	c.mu.Lock()
	c.reading = false
	c.timer.Stop()
	if !c.done {
		c.feed(p[:n])
		if err != nil || c.total > smartWatchMax {
			c.done = true
		}
	}
	c.mu.Unlock()
	return n, err
}

// feed: the record state after `b` from the server.
func (c *cutConn) feed(b []byte) {
	c.total += len(b)
	for len(b) > 0 {
		if c.left > 0 {
			k := min(c.left, len(b))
			c.left -= k
			b = b[k:]
			continue
		}
		k := copy(c.head[c.headN:], b)
		c.headN += k
		b = b[k:]
		if c.headN < len(c.head) {
			return
		}
		c.headN = 0
		c.left = int(c.head[3])<<8 | int(c.head[4])
		if c.head[0] < 20 || c.head[0] > 23 || c.left > tlsRecordMax {
			c.done = true // not TLS: nothing to tell a cut by
			return
		}
	}
}

func (c *cutConn) check() {
	c.mu.Lock()
	cut := !c.done && c.reading && (c.left > 0 || c.headN > 0) && time.Since(c.since) >= c.stall-50*time.Millisecond
	if cut {
		c.done = true
	}
	c.mu.Unlock()
	if cut {
		c.onCut()
		_ = c.Conn.Close()
	}
}

func (c *cutConn) Close() error {
	c.mu.Lock()
	c.done = true
	c.timer.Stop()
	c.mu.Unlock()
	return c.Conn.Close()
}

func (s *smartOutbound) byTag(tag string) adapter.Outbound {
	for _, way := range s.ways {
		if way.Tag() == tag {
			return way
		}
	}
	return s.ways[0]
}

// raceResult: the way kept (-1: none), its connection with the server's
// first bytes given back, and for each way why it lost ("" - it answered or
// wasn't waited for).
type raceResult struct {
	index int
	conn  net.Conn
	why   []string
}

type wayAnswer struct {
	index int
	conn  net.Conn
	first *buf.Buffer
	why   string
}

// raceWays sends `hello` down every dialer at once and keeps the first in
// their order whose server answered with a TLS handshake record within
// `wait`: a way is chosen once every way before it has failed - or, a later
// way having answered, stayed silent for the grace more. The others are
// closed as they come.
func raceWays(ctx context.Context, dialers []N.Dialer, destination M.Socksaddr, hello []byte, wait time.Duration) raceResult {
	ctx, cancel := context.WithTimeout(ctx, wait)
	defer cancel()
	answers := make(chan wayAnswer, len(dialers))
	for i, dialer := range dialers {
		go func() { answers <- tryWay(ctx, i, dialer, destination, hello, wait) }()
	}
	result := raceResult{index: -1, why: make([]string, len(dialers))}
	got := make([]*wayAnswer, len(dialers))
	failed := make([]bool, len(dialers))
	next := 0 // the way the choice waits for: all before it failed
	// A later way answered while an earlier one is silent: the earlier gets
	// `grace` more, not the whole wait. DPI that drops the hello stays silent
	// to the end, and a server reached directly answers sooner than through
	// a server abroad - on his line every blocked site's first visit took the
	// full 3 s (04.11.2026).
	var graceOver <-chan time.Time
	choose := func() {
		for next < len(dialers) && failed[next] {
			next++
		}
		if result.index < 0 && next < len(dialers) && got[next] != nil && !failed[next] {
			result.index = next
			result.conn = bufio.NewCachedConn(got[next].conn, got[next].first)
			cancel() // the slower ways stop waiting
		}
	}
	for left := len(dialers); left > 0; {
		select {
		case a := <-answers:
			left--
			got[a.index] = &a
			switch {
			case failed[a.index]:
				// Lost by the grace already: its late word (an answer, or
				// "silent" from the cancel) doesn't replace the grace's.
			case a.why != "":
				failed[a.index] = true
				result.why[a.index] = a.why
			case graceOver == nil && a.index > next:
				graceOver = time.After(smartGrace(wait))
			}
			choose()
		case <-graceOver:
			graceOver = nil
			// The silent ones before the later way that answered lose; their
			// goroutines still report (closed by the cancel) and are counted.
			for i := next; i < len(dialers) && got[i] == nil; i++ {
				failed[i] = true
				result.why[i] = "silent"
			}
			choose()
		}
	}
	for i, a := range got {
		if i != result.index && a != nil && a.conn != nil {
			_ = a.conn.Close()
			a.first.Release()
		}
	}
	return result
}

// smartGrace: how much longer a silent way is waited for once a later way
// answered - a direct handshake takes ~0.2 s, a second is plenty; never more
// than a third of the whole wait.
func smartGrace(wait time.Duration) time.Duration {
	return min(smartGraceWait, wait/3)
}

func tryWay(ctx context.Context, index int, dialer N.Dialer, destination M.Socksaddr, hello []byte, wait time.Duration) wayAnswer {
	conn, err := dialer.DialContext(ctx, N.NetworkTCP, destination)
	if err != nil {
		return wayAnswer{index: index, why: "dial"}
	}
	// The deadline before the cancel hook: a hook run at once (the race
	// already decided) must not be overwritten by it.
	_ = conn.SetDeadline(time.Now().Add(wait))
	stop := context.AfterFunc(ctx, func() { _ = conn.SetDeadline(time.Now()) })
	first := buf.NewSize(smartHelloMax)
	why := ""
	if _, err = conn.Write(hello); err != nil {
		why = failure(err)
	} else if _, err = first.ReadOnceFrom(conn); err != nil {
		why = failure(err)
	} else if first.IsEmpty() || first.Byte(0) == tlsAlert {
		why = "alert"
	} else if first.Byte(0) != tlsHandshake {
		why = "not tls"
	}
	// The hook stopped before the deadline is lifted; if it already ran, the
	// connection's deadline is spent - the answer came too late to keep.
	if !stop() && why == "" {
		why = "silent"
	}
	if why != "" {
		_ = conn.Close()
		first.Release()
		return wayAnswer{index: index, why: why}
	}
	_ = conn.SetDeadline(time.Time{})
	return wayAnswer{index: index, conn: conn, first: first}
}

// failure names how a way failed: "silent" (no answer in time), else
// "reset" (closed or reset after the hello - DPI's usual answer).
func failure(err error) string {
	var timeout interface{ Timeout() bool }
	if errors.Is(err, os.ErrDeadlineExceeded) || (errors.As(err, &timeout) && timeout.Timeout()) {
		return "silent"
	}
	return "reset"
}

// preDialed hands the connection manager a connection already made: the
// race's winner, the server's first bytes cached in it.
type preDialed struct {
	mu   sync.Mutex
	conn net.Conn
}

func (d *preDialed) DialContext(context.Context, string, M.Socksaddr) (net.Conn, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.conn == nil {
		return nil, E.New("smart: the race's connection is already used")
	}
	conn := d.conn
	d.conn = nil
	return conn, nil
}

func (d *preDialed) ListenPacket(context.Context, M.Socksaddr) (net.PacketConn, error) {
	return nil, E.New("smart: no packets on a race's connection")
}

// smartMemory: what the races decided, by name. It outlives a restart of the
// core (a reconnect shouldn't forget), not the service.
type smartMemory struct {
	mu    sync.Mutex
	items map[string]smartVerdict // the current network's (smart_store.go)

	nets    map[string]*smartNet // by network; nets[cur].Items is items
	cur     string               // "": the network isn't known yet
	path    string               // "": nothing is kept on disk
	pending bool                 // a write is scheduled
}

type smartVerdict struct {
	Way string    `json:"way"`
	Why string    `json:"why,omitempty"` // how the ways before it failed; "" - the preferred answered
	At  time.Time `json:"at"`
}

var smartMemo = &smartMemory{items: map[string]smartVerdict{}}

func (m *smartMemory) verdict(key string, now time.Time) (string, bool) {
	if key == "" {
		return "", false
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	v, ok := m.items[key]
	if !ok || now.Sub(v.At) >= smartRemember {
		return "", false
	}
	return v.Way, true
}

func (m *smartMemory) remember(key, way, why string, now time.Time) {
	if key == "" {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if why != "" || m.items[key].Why != "" {
		m.touchLocked() // only what the preferred way failed is kept on disk
	}
	m.items[key] = smartVerdict{Way: way, Why: why, At: now}
	if len(m.items) <= smartKeep {
		return
	}
	// Over the cap: the oldest go.
	keys := make([]string, 0, len(m.items))
	for k := range m.items {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(a, b int) bool { return m.items[keys[a]].At.Before(m.items[keys[b]].At) })
	for _, k := range keys[:len(keys)-smartKeep] {
		delete(m.items, k)
	}
}

// learned: the names whose preferred way failed - what the tray shows as
// "the client decided itself" - newest first.
func (m *smartMemory) learned(now time.Time) map[string]smartVerdict {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := map[string]smartVerdict{}
	for k, v := range m.items {
		if v.Why != "" && now.Sub(v.At) < smartRemember {
			out[k] = v
		}
	}
	return out
}

// smartLine: one thing the client decided for itself.
type smartLine struct {
	Name string `json:"name"` // a site, or "ip:1.2.3.4" (a whole address cut)
	Way  string `json:"way"`  // the outbound tag it goes by
	Why  string `json:"why"`  // reset, silent, cut, ...
	At   int64  `json:"at"`   // unix seconds
}

// smartReport: what the preferred way failed, newest first.
func smartReport(now time.Time) []smartLine {
	learned := smartMemo.learned(now)
	lines := make([]smartLine, 0, len(learned))
	for name, v := range learned {
		lines = append(lines, smartLine{Name: name, Way: v.Way, Why: v.Why, At: v.At.Unix()})
	}
	sort.Slice(lines, func(a, b int) bool {
		if lines[a].At != lines[b].At {
			return lines[a].At > lines[b].At
		}
		return lines[a].Name < lines[b].Name
	})
	return lines
}

// box_smart: what the smart outbound decided itself, {"learned":[{"name",
// "way","why","at"}]}, newest first. Freed with box_free.
//
//export box_smart
func box_smart() *C.char {
	out, err := json.Marshal(map[string]any{"learned": smartReport(time.Now())})
	if err != nil {
		return C.CString(`{"learned":[]}`)
	}
	return C.CString(string(out))
}
