package main

// Which sites open through an outbound: YouTube, Telegram, ChatGPT,
// Instagram - the tray's server list shows one mark each. Asked through the
// outbound itself, as exitip.go does, so it is the exit that is judged and
// not the route rules' choice for the tray's own traffic. A check takes a
// few seconds, so box_sites only starts it and reports what it knows - the
// sites that have answered so far, "pending" while the rest run.
//
// ok: the site answered the way a reachable one does. blocked: it answered,
// but refused this exit (ChatGPT's "unsupported country", a 403/451 from the
// front page). fail: no answer through the outbound at all, with the reason.

/*
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/constant"
	M "github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/common/ntp"
)

const (
	sitesTimeout  = 12 * time.Second // one site
	sitesMaxBody  = 4096
	sitesParallel = 3   // servers checked at a time
	sitesMaxTags  = 512 // outbounds with a state kept, per box
)

// The sites, in the order the tray shows them. judge reads the status and the
// start of the body.
var sitesList = []struct {
	id    string
	url   string
	judge func(status int, body string) (state, why string)
}{
	{"youtube", "https://www.youtube.com/generate_204", judgeReachable},
	{"telegram", "https://api.telegram.org/", judgeReachable},
	// Without a key api.openai.com says 401 where it works for the country
	// and 403 "unsupported_country_region_territory" where it does not.
	{"chatgpt", "https://api.openai.com/v1/models", judgeOpenAI},
	{"instagram", "https://www.instagram.com/", judgeReachable},
}

type siteState struct {
	Site  string `json:"site"`
	State string `json:"state"`
	Error string `json:"error,omitempty"`
}

type sitesReply struct {
	Tag     string      `json:"tag"`
	Pending bool        `json:"pending,omitempty"`
	Error   string      `json:"error,omitempty"`
	Results []siteState `json:"results"`
}

type sitesEntry struct {
	results []siteState
	pending bool
	round   int64 // which check may write it: a newer one (or a box restart) drops an older one's result
}

var (
	sitesMu     sync.Mutex
	sitesStates = map[string]*sitesEntry{}
	sitesRound  int64
	sitesSlots  = make(chan struct{}, sitesParallel)
)

func resetSites() {
	sitesMu.Lock()
	defer sitesMu.Unlock()
	sitesStates = map[string]*sitesEntry{}
	sitesRound++
}

func judgeReachable(status int, _ string) (string, string) {
	switch {
	case status == http.StatusForbidden, status == http.StatusUnavailableForLegalReasons:
		return "blocked", fmt.Sprintf("HTTP %d", status)
	case status >= 500:
		return "fail", fmt.Sprintf("HTTP %d", status)
	}
	return "ok", ""
}

func judgeOpenAI(status int, body string) (string, string) {
	if status == http.StatusForbidden && strings.Contains(body, "unsupported_country") {
		return "blocked", "unsupported country"
	}
	if status == http.StatusForbidden || status == http.StatusUnavailableForLegalReasons {
		return "blocked", fmt.Sprintf("HTTP %d", status)
	}
	if status >= 500 {
		return "fail", fmt.Sprintf("HTTP %d", status)
	}
	return "ok", ""
}

type sitesRequest struct {
	Tag     string `json:"tag"`
	Refresh bool   `json:"refresh"`
}

// box_sites reports which sites open through the outbound `tag` as JSON
// ({"tag", "results": [{"site", "state", "error"}], "pending", "error"}), and
// starts a check when there's none for that tag yet or `refresh` asks for a
// new one (not while one runs). requestJSON: {"tag": "...", "refresh": false}
// (validated by the service). Freed with box_free.
//
//export box_sites
func box_sites(requestJSON *C.char) (reply *C.char) {
	var request sitesRequest
	defer catchPanic("box_sites", func(message string) { reply = marshalSites(sitesReply{Tag: request.Tag, Error: message}) })
	if err := json.Unmarshal([]byte(C.GoString(requestJSON)), &request); err != nil {
		return marshalSites(sitesReply{Error: fmt.Sprintf("parse request: %s", err)})
	}
	boxMu.Lock()
	instance, ctx := boxInstance, boxContext
	boxMu.Unlock()
	if instance == nil {
		return marshalSites(sitesReply{Tag: request.Tag, Error: "box not running"})
	}

	sitesMu.Lock()
	defer sitesMu.Unlock()
	entry, known := sitesStates[request.Tag]
	if known && (!request.Refresh || entry.pending) {
		return marshalSites(replyOf(request.Tag, entry))
	}
	outbound, found := instance.Outbound().Outbound(request.Tag)
	if !found {
		return marshalSites(sitesReply{Tag: request.Tag, Error: "no such outbound"})
	}
	if !known {
		if len(sitesStates) >= sitesMaxTags {
			return marshalSites(sitesReply{Tag: request.Tag, Error: "too many outbounds asked"})
		}
		entry = &sitesEntry{}
		sitesStates[request.Tag] = entry
	}
	sitesRound++
	round := sitesRound
	entry.round = round
	entry.pending = true
	entry.results = nil
	go func() {
		sitesSlots <- struct{}{}
		defer func() { <-sitesSlots }()
		var wg sync.WaitGroup
		for _, site := range sitesList {
			wg.Add(1)
			go func() {
				defer wg.Done()
				var result siteState
				func() {
					defer catchPanic("site check", func(message string) { result = siteState{State: "fail", Error: message} })
					result = checkSite(ctx, outbound, site.url, site.judge)
				}()
				result.Site = site.id
				sitesMu.Lock()
				defer sitesMu.Unlock()
				if current, ok := sitesStates[request.Tag]; ok && current.round == round {
					current.results = append(current.results, result)
				}
			}()
		}
		wg.Wait()
		sitesMu.Lock()
		defer sitesMu.Unlock()
		if current, ok := sitesStates[request.Tag]; ok && current.round == round {
			current.pending = false
		}
	}()
	return marshalSites(replyOf(request.Tag, entry))
}

// replyOf lists the answers in the tray's order, whichever came first.
func replyOf(tag string, entry *sitesEntry) sitesReply {
	reply := sitesReply{Tag: tag, Pending: entry.pending, Results: []siteState{}}
	for _, site := range sitesList {
		for _, result := range entry.results {
			if result.Site == site.id {
				reply.Results = append(reply.Results, result)
			}
		}
	}
	return reply
}

func marshalSites(reply sitesReply) *C.char {
	if reply.Results == nil {
		reply.Results = []siteState{}
	}
	out, err := json.Marshal(reply)
	if err != nil {
		return C.CString(`{"error":"marshal"}`)
	}
	return C.CString(string(out))
}

// checkSite asks one address through the outbound and judges the answer.
func checkSite(ctx context.Context, outbound adapter.Outbound, url string, judge func(int, string) (string, string)) siteState {
	ctx, cancel := context.WithTimeout(ctx, sitesTimeout)
	defer cancel()
	client := http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
				conn, err := outbound.DialContext(ctx, "tcp", M.ParseSocksaddr(addr))
				if err != nil {
					return nil, err
				}
				return eofConn{conn}, nil
			},
			TLSClientConfig: &tls.Config{
				Time:    ntp.TimeFuncFromContext(ctx),
				RootCAs: adapter.RootPoolFromContext(ctx),
			},
			DisableKeepAlives: true,
		},
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return siteState{State: "fail", Error: err.Error()}
	}
	request.Header.Set("User-Agent", "sing-box "+constant.Version)
	response, err := client.Do(request)
	if err != nil {
		return siteState{State: "fail", Error: shortSiteError(err)}
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(response.Body, sitesMaxBody))
	state, why := judge(response.StatusCode, string(body))
	return siteState{State: state, Error: why}
}

// shortSiteError: the part of a Go error that says why ("Get \"https://...\": dial tcp: i/o timeout" -> "i/o timeout").
func shortSiteError(err error) string {
	text := err.Error()
	if i := strings.LastIndex(text, ": "); i >= 0 && i+2 < len(text) {
		text = text[i+2:]
	}
	if len(text) > 100 {
		text = text[:100]
	}
	return text
}
