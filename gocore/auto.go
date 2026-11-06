package main

// Auto, in the core (06.11.2026): the outbound "sov-auto" takes the place of
// the subscription's urltest group the tray steers. The tray still picks the
// server (its own measurements, delays.h JudgeAuto) and hands it over with
// box_select, as to a selector; what the core adds is what the tray, polling
// every few seconds, can't:
//
//   - a connection the picked server doesn't open goes on to the next one at
//     once - the next protocol of the same machine first (a protocol blocked,
//     the machine alive), then the other machines, best measured first;
//   - the next one doesn't wait for the first to give up: it starts when the
//     first takes twice its usual time to connect (a race; whichever opens
//     first carries the connection, the other is closed);
//   - a server that failed a connection goes last for a while - on this
//     network: the memory is one per network (the phone's mobile internet
//     with its white lists, the home Wi-Fi), so a way dead on one is tried
//     first again on the other;
//   - UDP (games, calls) has a choice of its own: the picked server's, unless
//     it carries none ("no udp") or another one's is clearly better.
//
// It is no OutboundGroup on purpose: the router walks a group down to its
// member and dials that itself (route.go resolveOutbound), past any choice
// the group would make per connection.

import (
	"context"
	"errors"
	"io"
	"net"
	"sort"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/adapter/outbound"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing/common"
	"github.com/sagernet/sing/common/control"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/logger"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
	"github.com/sagernet/sing/service"
)

const (
	autoType        = "sov-auto"
	autoStagger     = 1500 * time.Millisecond // the next way's head start, the first's connect time unknown
	autoStaggerMin  = 600 * time.Millisecond
	autoStaggerMax  = 2500 * time.Millisecond
	autoDialMax     = 15 * time.Second // a way's dial, a race's loser included
	autoDownFor     = 2 * time.Minute  // a way that failed a connection goes last this long, on this network
	autoUDPSticky   = 15               // ms: UDP leaves the picked server only for one this much better, and by a fifth
	autoUnmeasured  = 1 << 20          // a score: after every measured way, in the config's order
	autoFailedScore = 1 << 21          // a score: the last test failed
)

// autoOptions: the servers; where each connects (tag -> host: one machine's
// protocols are tried first when one of them fails).
type autoOptions struct {
	Outbounds []string          `json:"outbounds"`
	Hosts     map[string]string `json:"hosts,omitempty"`
}

func registerAuto(registry *outbound.Registry) {
	outbound.Register[autoOptions](registry, autoType, newAuto)
}

type autoDown struct {
	at      time.Time
	why     string // "failure text"
	strikes int    // failures in a row (autoStrikeGap apart) since the last success: the hold doubles with each (autoHold)
}

type autoOutbound struct {
	outbound.Adapter
	ctx     context.Context
	logger  logger.ContextLogger
	manager adapter.OutboundManager
	tags    []string
	hosts   map[string]string
	ways    []adapter.Outbound

	mu      sync.Mutex
	primary string                         // the tray's pick (SelectOutbound); "" until it makes one
	network string                         // the network the machine is on (smartNetKey)
	down    map[string]map[string]autoDown // by network, by tag
	lastTCP string                         // the way the last connection went
	lastUDP string
}

var _ adapter.Lifecycle = (*autoOutbound)(nil)

func newAuto(ctx context.Context, _ adapter.Router, logger log.ContextLogger, tag string, options autoOptions) (adapter.Outbound, error) {
	if len(options.Outbounds) == 0 {
		return nil, E.New("missing outbounds")
	}
	return &autoOutbound{
		Adapter: outbound.NewAdapter(autoType, tag, []string{N.NetworkTCP, N.NetworkUDP}, options.Outbounds),
		ctx:     ctx,
		logger:  logger,
		manager: service.FromContext[adapter.OutboundManager](ctx),
		tags:    options.Outbounds,
		hosts:   options.Hosts,
		down:    map[string]map[string]autoDown{},
	}, nil
}

func (a *autoOutbound) Start(stage adapter.StartStage, _ *adapter.Scope) error {
	if stage != adapter.StartStateStart {
		return nil
	}
	for i, tag := range a.tags {
		way, loaded := a.manager.Outbound(tag)
		if !loaded {
			return E.New("outbound ", i, " not found: ", tag)
		}
		a.ways = append(a.ways, way)
	}
	autoLiveMu.Lock()
	autoLive[a.Tag()] = a
	autoLiveMu.Unlock()
	a.followNetwork()
	return nil
}

// followNetwork: the failures remembered are the network's the machine is on.
func (a *autoOutbound) followNetwork() {
	manager := service.FromContext[adapter.NetworkManager](a.ctx)
	if manager == nil {
		return
	}
	monitor := manager.InterfaceMonitor()
	if monitor == nil {
		return
	}
	a.switchNetwork(smartNetKey(monitor.DefaultInterface()))
	monitor.RegisterCallback(func(ifc *control.Interface, _ int) {
		a.switchNetwork(smartNetKey(ifc))
	})
}

func (a *autoOutbound) switchNetwork(key string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.network = key
	// What a run before (or an earlier stay) learned of this network's ways (auto_store.go).
	if recalled := autoMemo.recall(key); len(recalled) > 0 {
		down := a.down[key]
		if down == nil {
			down = map[string]autoDown{}
			a.down[key] = down
		}
		for _, tag := range a.tags {
			if d, ok := recalled[tag]; ok {
				if have, exists := down[tag]; !exists || d.at.After(have.at) {
					down[tag] = d
				}
			}
		}
	}
}

// The running box's auto outbounds by tag: what box_delays, box_route and the
// traffic seen per server read of them.
var (
	autoLiveMu sync.Mutex
	autoLive   = map[string]*autoOutbound{}
)

func liveAuto(tag string) *autoOutbound {
	autoLiveMu.Lock()
	defer autoLiveMu.Unlock()
	return autoLive[tag]
}

// autoWay: for `tag`, a sov-auto's, the server it carries `network` by now -
// the one the last connection went by, else the one the next would try
// first; any other tag as it is.
func autoWay(tag string, network string) string {
	a := liveAuto(tag)
	if a == nil {
		return tag
	}
	a.mu.Lock()
	last := a.lastTCP
	if network == N.NetworkUDP {
		last = a.lastUDP
	}
	a.mu.Unlock()
	if last != "" {
		return last
	}
	if ways := a.order(network, time.Now()); len(ways) > 0 {
		return ways[0].Tag()
	}
	return tag
}

// byTag: the way 	ag, or nil.
func (a *autoOutbound) byTag(tag string) adapter.Outbound {
	for _, way := range a.ways {
		if way.Tag() == tag {
			return way
		}
	}
	return nil
}

// SelectOutbound: the tray's pick, as box_select hands it to a selector.
func (a *autoOutbound) SelectOutbound(tag string) bool {
	if !common.Contains(a.tags, tag) {
		return false
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	a.primary = tag
	return true
}

// measuredDelays: the last test's results of `tags` (urltest.go).
func measuredDelays(tags []string) map[string]delayResult {
	delayMu.Lock()
	defer delayMu.Unlock()
	out := make(map[string]delayResult, len(tags))
	for _, tag := range tags {
		if d := delays[tag]; d != nil {
			out[tag] = *d
		}
	}
	return out
}

// tcpScore: what a way costs, ms, by its last test - the median, its
// spread and its losses, as the tray's DelayScore (delays.cpp).
func tcpScore(d delayResult, ok bool) int {
	switch {
	case !ok || d.Pending:
		return autoUnmeasured
	case d.Error != "":
		return autoFailedScore
	}
	return int(d.Delay) + 2*int(d.Jitter) + 10*int(d.Loss)
}

// udpScore: the same for UDP; false for a way that carries none.
func udpScore(d delayResult, ok bool) (int, bool) {
	switch {
	case !ok || d.Pending:
		return autoUnmeasured, true
	case d.Error != "":
		return autoFailedScore, true
	case d.UDPError == "no udp":
		return 0, false
	case d.UDPError != "":
		return autoFailedScore, true
	case d.UDPDelay == 0:
		return autoUnmeasured, true
	}
	return int(d.UDPDelay) + 2*int(d.UDPJitter) + 10*int(d.UDPLoss), true
}

// order: the ways for a connection by `network`, the one to try first first.
// First the tray's pick (none yet: the best measured; for UDP another's when
// clearly better, or the pick carries none), then the other protocols of its
// machine, then the rest - each best measured first. A way that failed a
// connection on this network in the last autoHold goes after the others.
func (a *autoOutbound) order(network string, now time.Time) []adapter.Outbound {
	measured := measuredDelays(a.tags)
	a.mu.Lock()
	primary := a.primary
	down := a.down[a.network]
	isDown := make(map[string]bool, len(down))
	for tag, d := range down {
		isDown[tag] = now.Sub(d.at) < autoHold(d.strikes)
	}
	a.mu.Unlock()

	type candidate struct {
		way   adapter.Outbound
		score int
	}
	var candidates, carryNone []candidate
	for _, way := range a.ways {
		if !common.Contains(way.Network(), network) {
			continue
		}
		d, ok := measured[way.Tag()]
		if network == N.NetworkUDP {
			score, carries := udpScore(d, ok)
			if !carries {
				carryNone = append(carryNone, candidate{way: way, score: autoUnmeasured})
				continue
			}
			candidates = append(candidates, candidate{way: way, score: score})
			continue
		}
		candidates = append(candidates, candidate{way: way, score: tcpScore(d, ok)})
	}
	if len(candidates) == 0 {
		candidates = carryNone // its test may be wrong; better than no way at all
	}
	if len(candidates) == 0 {
		return nil
	}
	sort.SliceStable(candidates, func(i, j int) bool { return candidates[i].score < candidates[j].score })

	first := 0
	for i, c := range candidates {
		if c.way.Tag() != primary {
			continue
		}
		first = i
		if network == N.NetworkUDP && i > 0 {
			best, own := candidates[0].score, c.score
			if (own >= autoUnmeasured && best < autoUnmeasured) || (own > best+autoUDPSticky && own*5 > best*6) {
				first = 0 // clearly worse for UDP: the best one's
			}
		}
		break
	}
	head := candidates[first]
	rest := make([]candidate, 0, len(candidates)-1)
	rest = append(rest, candidates[:first]...)
	rest = append(rest, candidates[first+1:]...)
	host := a.hosts[head.way.Tag()]
	sort.SliceStable(rest, func(i, j int) bool {
		si := host != "" && a.hosts[rest[i].way.Tag()] == host
		sj := host != "" && a.hosts[rest[j].way.Tag()] == host
		return si && !sj
	})
	ways := make([]adapter.Outbound, 0, len(candidates))
	ways = append(ways, head.way)
	for _, c := range rest {
		ways = append(ways, c.way)
	}
	sort.SliceStable(ways, func(i, j int) bool { return !isDown[ways[i].Tag()] && isDown[ways[j].Tag()] })
	return ways
}

// stagger: how long the first way is given before the next starts beside it
// - twice its usual connect time (urltest.go "connect"), within bounds.
func staggerOf(way adapter.Outbound) time.Duration {
	d, ok := measuredDelays([]string{way.Tag()})[way.Tag()]
	if !ok || d.Connect == 0 {
		return autoStagger
	}
	return min(max(2*time.Duration(d.Connect)*time.Millisecond, autoStaggerMin), autoStaggerMax)
}

// reported: what a way's dial says of it - a failure remembered for this
// network (and logged when it is new), an opened connection clears it.
func (a *autoOutbound) reported(network string) func(adapter.Outbound, error) {
	return func(way adapter.Outbound, err error) {
		now := time.Now()
		a.mu.Lock()
		down := a.down[a.network]
		if down == nil {
			down = map[string]autoDown{}
			a.down[a.network] = down
		}
		net := a.network
		if err == nil {
			_, had := down[way.Tag()]
			delete(down, way.Tag())
			a.mu.Unlock()
			if had {
				autoMemo.note(net, way.Tag(), autoDown{}, true)
			}
			return
		}
		was, known := down[way.Tag()]
		why := failure(err)
		strikes := 1
		if known {
			strikes = was.strikes
			if now.Sub(was.at) >= autoStrikeGap {
				strikes++
			}
		}
		record := autoDown{at: now, why: why, strikes: strikes}
		down[way.Tag()] = record
		a.mu.Unlock()
		autoMemo.note(net, way.Tag(), record, false)
		if !known || now.Sub(was.at) >= autoHold(was.strikes) {
			a.logger.Info("auto: ", way.Tag(), " did not open a ", network, " connection (", why, ": ", err, "), the next way takes them")
		}
	}
}

func (a *autoOutbound) DialContext(ctx context.Context, network string, destination M.Socksaddr) (net.Conn, error) {
	network = N.NetworkName(network)
	ways := a.order(network, time.Now())
	if len(ways) == 0 {
		return nil, E.New("no way for ", network)
	}
	conn, way, err := autoRace(ctx, ways, staggerOf(ways[0]), a.reported(network),
		func(ctx context.Context, way adapter.Outbound) (net.Conn, error) {
			return way.DialContext(ctx, network, destination)
		})
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	a.lastTCP = way
	a.mu.Unlock()
	return conn, nil
}

func (a *autoOutbound) ListenPacket(ctx context.Context, destination M.Socksaddr) (net.PacketConn, error) {
	ways := a.order(N.NetworkUDP, time.Now())
	if len(ways) == 0 {
		return nil, E.New("no way for udp")
	}
	conn, way, err := autoRace(ctx, ways, staggerOf(ways[0]), a.reported(N.NetworkUDP),
		func(ctx context.Context, way adapter.Outbound) (net.PacketConn, error) {
			return way.ListenPacket(ctx, destination)
		})
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	a.lastUDP = way
	a.mu.Unlock()
	return conn, nil
}

type autoAttempt[T io.Closer] struct {
	index int
	value T
	err   error
}

// autoRace opens `ways` in turn, the next one `stagger` after the one before
// or as soon as it failed, and returns the first that opened (and its tag).
// The others go on by themselves - what they open is closed, what they say
// is reported - up to autoDialMax; the caller giving up stops them all.
func autoRace[T io.Closer](ctx context.Context, ways []adapter.Outbound, stagger time.Duration,
	report func(adapter.Outbound, error), open func(context.Context, adapter.Outbound) (T, error)) (T, string, error) {
	var zero T
	dialCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), autoDialMax)
	answers := make(chan autoAttempt[T], len(ways))
	started, pending := 0, 0
	next := func() {
		if started == len(ways) {
			return
		}
		index := started
		started++
		pending++
		go func() {
			answer := autoAttempt[T]{index: index}
			func() {
				defer catchPanic("auto: "+ways[index].Tag(), func(message string) { answer.err = errors.New(message) })
				answer.value, answer.err = open(dialCtx, ways[index])
			}()
			answers <- answer
		}()
	}
	// What a way said, unless it was the cancel: the caller gone is no
	// failure of the way.
	tell := func(answer autoAttempt[T]) {
		if answer.err != nil && errors.Is(dialCtx.Err(), context.Canceled) {
			return
		}
		report(ways[answer.index], answer.err)
	}
	next()
	timer := time.NewTimer(stagger)
	defer timer.Stop()
	var last error
	for pending > 0 {
		select {
		case answer := <-answers:
			pending--
			tell(answer)
			if answer.err == nil {
				go autoDrain(answers, pending, tell, cancel)
				return answer.value, ways[answer.index].Tag(), nil
			}
			last = answer.err
			next()
			timer.Reset(stagger)
		case <-timer.C:
			next()
			timer.Reset(stagger)
		case <-ctx.Done():
			cancel()
			go autoDrain(answers, pending, tell, func() {})
			return zero, "", ctx.Err()
		}
	}
	cancel()
	return zero, "", last
}

// autoDrain waits for a race's last `pending` ways: an opened one is closed.
func autoDrain[T io.Closer](answers <-chan autoAttempt[T], pending int, tell func(autoAttempt[T]), done func()) {
	defer done()
	for ; pending > 0; pending-- {
		answer := <-answers
		tell(answer)
		if answer.err == nil {
			_ = answer.value.Close()
		}
	}
}

// autoState: what box_delays says of a server in a sov-auto - a connection it
// didn't open on this network lately, and whether auto's UDP goes by it.
type autoState struct {
	failedAt  int64
	failedWhy string
	udpWay    bool
}

func autoStates(now time.Time) map[string]autoState {
	autoLiveMu.Lock()
	live := make([]*autoOutbound, 0, len(autoLive))
	for _, a := range autoLive {
		live = append(live, a)
	}
	autoLiveMu.Unlock()
	states := map[string]autoState{}
	for _, a := range live {
		a.mu.Lock()
		for tag, d := range a.down[a.network] {
			if now.Sub(d.at) < autoHold(d.strikes) {
				s := states[tag]
				s.failedAt, s.failedWhy = d.at.Unix(), d.why
				states[tag] = s
			}
		}
		udp, tcp := a.lastUDP, a.lastTCP
		a.mu.Unlock()
		if udp != "" && udp != tcp {
			s := states[udp]
			s.udpWay = true
			states[udp] = s
		}
	}
	return states
}
