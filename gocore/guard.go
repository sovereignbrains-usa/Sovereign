package main

import (
	"context"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/dns"
	"github.com/sagernet/sing-box/log"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/logger"
	"github.com/sagernet/sing/service"

	mDNS "github.com/miekg/dns"
)

// The block, done by the core itself rather than read back from its log.
//
// Two DNS transports of ours, added to the config by prepareBlocker (blocker.go) only when
// the block's lists are in it:
//
//   - "sov-block" is the server of the DNS rule that used to answer "no addresses" by itself
//     (predefined). It answers the same - NOERROR, no records - and now knows who asked: the
//     program of the connection the query came in on, and which category's list the name is in.
//
//   - "sov-guard" stands in the place of each real DNS server (same tag; the real one keeps a tag
//     of its own) and looks at what comes back. A site that hides its tracker behind its own
//     name - metrics.shop.example CNAME t.tracker.example - passes a list of names, the
//     answer's chain does not: any name of it in a block list and the answer is "no addresses".
//     A site the user let through by name is left alone (skip).

const (
	guardType = "sov-guard"
	blockType = "sov-block"
)

// blockList: a block rule-set (its tag in the router) and the category it counts for.
type blockList struct {
	Tag      string `json:"tag"`
	Category string `json:"category"`
}

type guardOptions struct {
	Inner string      `json:"inner"`
	Lists []blockList `json:"lists"`
	Skip  []string    `json:"skip,omitempty"` // names (and what's under them) the user let through
}

type blockOptions struct {
	Lists []blockList `json:"lists"`
}

func registerGuard(registry *dns.TransportRegistry) {
	dns.RegisterTransport[guardOptions](registry, guardType, newGuard)
	dns.RegisterTransport[blockOptions](registry, blockType, newBlock)
}

// matcher: which of the lists a name is in, through the router's own rule-sets.
type matcher struct {
	ctx   context.Context
	lists []blockList
	once  sync.Once
	sets  []adapter.RuleSet // parallel to lists; nil where the router has none
}

func (m *matcher) load() {
	m.once.Do(func() {
		router := service.FromContext[adapter.Router](m.ctx)
		m.sets = make([]adapter.RuleSet, len(m.lists))
		if router == nil {
			return
		}
		for i, list := range m.lists {
			if set, ok := router.RuleSet(list.Tag); ok {
				m.sets[i] = set
			}
		}
	})
}

// category: the category of the first list `name` is in, or "".
func (m *matcher) category(name string) string {
	m.load()
	name = strings.TrimSuffix(strings.ToLower(name), ".")
	if name == "" {
		return ""
	}
	for i, set := range m.sets {
		if set == nil {
			continue
		}
		query := adapter.InboundContext{Domain: name}
		if set.Match(&query) {
			return m.lists[i].Category
		}
	}
	return ""
}

// programOf: the path of the program a DNS query came from, when the router found it (Windows'
// own resolver asks for most programs: they are then svchost.exe).
func programOf(ctx context.Context) string {
	if metadata := adapter.ContextFrom(ctx); metadata != nil && metadata.ProcessInfo != nil {
		return metadata.ProcessInfo.ProcessPath
	}
	return ""
}

// refusal: the answer "no addresses" (NOERROR, no records) to `message`: what predefined(NOERROR)
// said, and what Windows takes at once where it waits ~11 s on a negative one (routing.cpp).
func refusal(message *mDNS.Msg) *mDNS.Msg {
	response := new(mDNS.Msg)
	response.SetReply(message)
	response.Rcode = mDNS.RcodeSuccess
	response.RecursionAvailable = true
	return response
}

func questionName(message *mDNS.Msg) string {
	if len(message.Question) == 0 {
		return ""
	}
	return strings.TrimSuffix(strings.ToLower(message.Question[0].Name), ".")
}

// refuse: one refusal counted, and said for the tray's list when it is new.
func refuse(category, name, program string, cloaked bool) {
	now := time.Now()
	blocked.mu.Lock()
	blocked.count(category, name, program, cloaked, now)
	notice := blocked.say(name, now)
	blocked.mu.Unlock()
	if notice != "" {
		sayToHost(log.LevelInfo, notice)
	}
}

// ---- sov-block

type blockTransport struct {
	dns.TransportAdapter
	match matcher
}

func newBlock(ctx context.Context, _ log.ContextLogger, tag string, options blockOptions) (adapter.DNSTransport, error) {
	return &blockTransport{
		TransportAdapter: dns.NewTransportAdapter(blockType, tag, nil),
		match:            matcher{ctx: ctx, lists: options.Lists},
	}, nil
}

func (t *blockTransport) Start(adapter.StartStage, *adapter.Scope) error { return nil }
func (t *blockTransport) Reset()                                         {}

func (t *blockTransport) Exchange(ctx context.Context, message *mDNS.Msg) (*mDNS.Msg, error) {
	name := questionName(message)
	category := t.match.category(name)
	if category == "" {
		category = "ads" // the rule matched, the lists say nothing: a list since changed
	}
	refuse(category, name, programOf(ctx), false)
	return refusal(message), nil
}

func (t *blockTransport) ExchangeAsync(ctx context.Context, message *mDNS.Msg, callback func(*mDNS.Msg, error)) {
	callback(t.Exchange(ctx, message))
}

// ---- sov-guard

type guardTransport struct {
	dns.TransportAdapter
	ctx     context.Context
	logger  logger.ContextLogger
	inner   string
	match   matcher
	skip    []string
	manager adapter.DNSTransportManager
}

func newGuard(ctx context.Context, logger log.ContextLogger, tag string, options guardOptions) (adapter.DNSTransport, error) {
	if options.Inner == "" {
		return nil, E.New("missing inner")
	}
	skip := make([]string, 0, len(options.Skip))
	for _, name := range options.Skip {
		skip = append(skip, strings.TrimSuffix(strings.ToLower(name), "."))
	}
	return &guardTransport{
		TransportAdapter: dns.NewTransportAdapter(guardType, tag, []string{options.Inner}),
		ctx:              ctx,
		logger:           logger,
		inner:            options.Inner,
		match:            matcher{ctx: ctx, lists: options.Lists},
		skip:             skip,
	}, nil
}

func (t *guardTransport) Start(adapter.StartStage, *adapter.Scope) error {
	t.manager = service.FromContext[adapter.DNSTransportManager](t.ctx)
	if t.manager == nil {
		return E.New("no DNS transport manager")
	}
	return nil
}

func (t *guardTransport) Reset() {
	if inner := t.innerTransport(); inner != nil {
		inner.Reset()
	}
}

func (t *guardTransport) innerTransport() adapter.DNSTransport {
	if t.manager == nil {
		return nil
	}
	inner, _ := t.manager.Transport(t.inner)
	return inner
}

// letThrough: a name the user let through, or one under it.
func (t *guardTransport) letThrough(name string) bool {
	for _, skip := range t.skip {
		if name == skip || strings.HasSuffix(name, "."+skip) {
			return true
		}
	}
	return false
}

// cloaked: the category of the first name of the answer's CNAME chain that a list holds, "" if none.
// Never the asked name itself: the router's own rule has had it before the question came here.
func (t *guardTransport) cloaked(response *mDNS.Msg) (category string) {
	for _, record := range response.Answer {
		cname, ok := record.(*mDNS.CNAME)
		if !ok {
			continue
		}
		if category = t.match.category(cname.Target); category != "" {
			return category
		}
	}
	return ""
}

func (t *guardTransport) Exchange(ctx context.Context, message *mDNS.Msg) (*mDNS.Msg, error) {
	inner := t.innerTransport()
	if inner == nil {
		return nil, E.New("DNS server ", t.inner, " is not there")
	}
	response, err := inner.Exchange(ctx, message)
	if err != nil || response == nil || len(response.Answer) == 0 {
		return response, err
	}
	name := questionName(message)
	if name == "" || t.letThrough(name) {
		return response, nil
	}
	if category := t.cloaked(response); category != "" {
		refuse(category, name, programOf(ctx), true)
		return refusal(message), nil
	}
	return response, nil
}

func (t *guardTransport) ExchangeAsync(ctx context.Context, message *mDNS.Msg, callback func(*mDNS.Msg, error)) {
	callback(t.Exchange(ctx, message))
}

// programName: what the tray shows for a program's path.
func programName(path string) string {
	if path == "" {
		return ""
	}
	return filepath.Base(strings.ReplaceAll(path, "\\", "/"))
}
