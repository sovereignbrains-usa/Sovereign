package main

// The speed of a way, by the user's click ("замерить" on the servers page):
// eight seconds of a download through the outbound itself from Cloudflare's
// speed test - three connections at once, each reading pieces of it one
// after another (a few hundred MB on a fast line, so only on a click). The
// figure is the average of the run after its first second of data (the
// window still growing; counted from the first byte, so a slow handshake
// is not held against the way): what a long download would get, not what a
// small page shows. Not the best window of the run: that figure is the
// burst's, and on 18.11.2026 one flow and the best 3 s put the same line
// at 122 to 329 Mbit/s from one way to the next. Like box_exitip it only
// starts the run and reports what it knows, one run at a time.

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
	"sync"
	"sync/atomic"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/constant"
	M "github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/common/ntp"
)

const (
	// Cloudflare's test file refuses more than 50 MB in one request with a
	// 403: a single 100 MB one worked until 18.11.2026, then every run on
	// the page said "замер не вышел: HTTP 403". So a run reads pieces of
	// speedPiece one after another until the time is up; a 403 on the first
	// one (the limit moved again) halves the piece, down to speedPieceLeast.
	speedURL        = "https://speed.cloudflare.com/__down?bytes=%d"
	speedPiece      = 40_000_000
	speedPieceLeast = 5_000_000
	speedDuration   = 8 * time.Second
	speedWarmup     = time.Second     // from the first byte of the run
	speedLeast      = 2 * time.Second // the shortest stretch after it that says a figure
	speedFlows      = 3               // one flow's speed is its congestion control's mood (150 of 250 Mbit/s on his line)
	speedSample     = 100 * time.Millisecond
	speedMaxTags    = 512
)

// nextPiece: what to ask after the answer `status` to a piece of `size`
// when `total` bytes have come in the run so far: the piece halved for a
// refusal (403, 429) of the run's very first request - the size is what
// they dislike - or false: a refusal later on (the lately-asked-too-much
// kind) ends the run with what it has, not a smaller piece.
func nextPiece(size int64, status int, total int64) (int64, bool) {
	if total > 0 || (status != http.StatusForbidden && status != http.StatusTooManyRequests) || size/2 < speedPieceLeast {
		return size, false
	}
	return size / 2, true
}

// The tray's name for the line without the VPN ("Напрямую" on the servers
// page): the config's direct outbound, whatever its tag.
const speedDirectTag = "sov-direct"

type speedState struct {
	Tag     string `json:"tag"`
	Pending bool   `json:"pending,omitempty"`
	Kbps    int64  `json:"kbps,omitempty"` // kbit/s; 0 with no error: not measured yet
	Error   string `json:"error,omitempty"`
}

type speedEntry struct {
	state speedState
	round int64
}

var (
	speedMu     sync.Mutex
	speedStates = map[string]*speedEntry{}
	speedRound  int64
	speedBusy   bool // a run goes: one at a time, each takes the link
)

func resetSpeed() {
	speedMu.Lock()
	defer speedMu.Unlock()
	speedStates = map[string]*speedEntry{}
	speedRound++
	speedBusy = false
}

// speedFigure: the average speed of a run given as byte counts (all its
// flows together) at moments since its start, in kbit/s: from `speedWarmup`
// after the first byte to the last sample. 0 when less than `speedLeast`
// of it is left to say.
func speedFigure(at []time.Duration, bytes []int64) int64 {
	first := -1
	for i := range at {
		if bytes[i] > 0 {
			first = i
			break
		}
	}
	if first < 0 {
		return 0
	}
	from := -1
	for i := first; i < len(at); i++ {
		if at[i]-at[first] >= speedWarmup {
			from = i
			break
		}
	}
	if from < 0 {
		return 0
	}
	last := len(at) - 1
	span := at[last] - at[from]
	if span < speedLeast {
		return 0
	}
	return (bytes[last] - bytes[from]) * 8 / int64(span/time.Millisecond)
}

type speedRequest struct {
	Tag   string `json:"tag"`
	Start bool   `json:"start"`
}

// box_speed reports the run through the outbound `tag` as JSON ({"tag",
// "kbps", "error", "pending"}); with "start" (the click) it starts one first,
// unless one runs. A poll without it never starts a run: the tray polls a
// running one until it's done, and its last poll comes after the end - which
// started the run again, round after round, a hundred megabytes each, the
// page saying "меряю…" for good (11.11.2026). A finished run's figure stays
// until the box restarts or a new run replaces it.
//
//export box_speed
func box_speed(requestJSON *C.char) (reply *C.char) {
	var request speedRequest
	defer catchPanic("box_speed", func(message string) { reply = marshalSpeed(speedState{Tag: request.Tag, Error: message}) })
	if err := json.Unmarshal([]byte(C.GoString(requestJSON)), &request); err != nil {
		return marshalSpeed(speedState{Error: fmt.Sprintf("parse request: %s", err)})
	}
	boxMu.Lock()
	instance, ctx := boxInstance, boxContext
	boxMu.Unlock()
	if instance == nil {
		return marshalSpeed(speedState{Tag: request.Tag, Error: "box not running"})
	}
	outbound, found := speedOutbound(instance, request.Tag)
	if !found {
		return marshalSpeed(speedState{Tag: request.Tag, Error: "no such outbound"})
	}

	speedMu.Lock()
	defer speedMu.Unlock()
	if entry, known := speedStates[request.Tag]; known && (entry.state.Pending || !request.Start) {
		return marshalSpeed(entry.state)
	}
	if !request.Start {
		return marshalSpeed(speedState{Tag: request.Tag}) // not measured
	}
	if speedBusy {
		return marshalSpeed(speedState{Tag: request.Tag, Error: "another run is going"})
	}
	entry, known := speedStates[request.Tag]
	if !known {
		if len(speedStates) >= speedMaxTags {
			return marshalSpeed(speedState{Tag: request.Tag, Error: "too many outbounds asked"})
		}
		entry = &speedEntry{}
		speedStates[request.Tag] = entry
	}
	speedRound++
	round := speedRound
	entry.round = round
	entry.state = speedState{Tag: request.Tag, Pending: true}
	speedBusy = true
	go func() {
		var result speedState
		func() {
			defer catchPanic("speed run", func(message string) { result = speedState{Error: message} })
			result = runSpeed(ctx, outbound, func(kbps int64) {
				speedMu.Lock()
				defer speedMu.Unlock()
				if current, ok := speedStates[request.Tag]; ok && current.round == round && current.state.Pending {
					current.state.Kbps = kbps
				}
			})
		}()
		result.Tag = request.Tag
		speedMu.Lock()
		defer speedMu.Unlock()
		if current, ok := speedStates[request.Tag]; ok && current.round == round {
			current.state = result
			speedBusy = false
		}
	}()
	return marshalSpeed(entry.state)
}

func marshalSpeed(state speedState) *C.char {
	out, err := json.Marshal(state)
	if err != nil {
		return C.CString(`{"error":"marshal"}`)
	}
	return C.CString(string(out))
}

// speedOutbound: the outbound `tag` names, or for speedDirectTag the config's
// first direct one - its tag is the user's or ours ("direct", "direct 2":
// routing.cpp), the tray only knows it wants the line itself.
func speedOutbound(instance *box.Box, tag string) (adapter.Outbound, bool) {
	if tag != speedDirectTag {
		return instance.Outbound().Outbound(tag)
	}
	for _, outbound := range instance.Outbound().Outbounds() {
		if outbound.Type() == constant.TypeDirect {
			return outbound, true
		}
	}
	return nil, false
}

// runSpeed: the run itself; `progress` hears the last second's speed, a few
// times a second, for the figure that grows on the page while it goes.
func runSpeed(ctx context.Context, outbound adapter.Outbound, progress func(kbps int64)) speedState {
	ctx, cancel := context.WithTimeout(ctx, speedDuration)
	defer cancel()

	var total atomic.Int64 // what all the flows have read
	var failMu sync.Mutex
	var failed string // the first flow's reason to stop, if it was not the clock
	fail := func(reason string) {
		failMu.Lock()
		defer failMu.Unlock()
		if failed == "" {
			failed = reason
		}
	}
	var flows sync.WaitGroup
	for range speedFlows {
		flows.Add(1)
		go func() {
			defer flows.Done()
			defer catchPanic("speed flow", fail)
			if reason := speedFlow(ctx, outbound, &total); reason != "" {
				fail(reason)
			}
		}()
	}
	finished := make(chan struct{})
	go func() {
		flows.Wait()
		close(finished)
	}()

	// The flows only count; the run looks at the count every speedSample.
	start := time.Now()
	ticker := time.NewTicker(speedSample)
	defer ticker.Stop()
	hard := time.NewTimer(speedDuration + 3*time.Second) // a flow that won't let go is left behind
	defer hard.Stop()
	var at []time.Duration
	var counts []int64
	var told time.Duration
	back := 0 // the earliest sample within the last second
	for done := false; !done; {
		select {
		case <-ticker.C:
		case <-finished:
			done = true
		case <-hard.C:
			done = true
		}
		now := time.Since(start)
		at = append(at, now)
		counts = append(counts, total.Load())
		for back < len(at)-1 && now-at[back+1] >= time.Second {
			back++
		}
		if now-told >= 250*time.Millisecond && now-at[back] >= 500*time.Millisecond {
			told = now
			progress((counts[len(counts)-1] - counts[back]) * 8 / int64((now-at[back])/time.Millisecond))
		}
	}
	if kbps := speedFigure(at, counts); kbps > 0 {
		return speedState{Kbps: kbps}
	}
	failMu.Lock()
	defer failMu.Unlock()
	if failed != "" {
		return speedState{Error: failed}
	}
	return speedState{Error: "too little came through to say"}
}

// speedFlow: one connection's share of a run - pieces of the test file one
// after another over one keep-alive connection (the next piece goes over
// the connection the last one used, not through a new handshake with the
// outbound), every byte added to `total`, until the context ends. It
// returns why it stopped when that was not the end of the run, else "".
func speedFlow(ctx context.Context, outbound adapter.Outbound, total *atomic.Int64) string {
	transport := &http.Transport{
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
	}
	defer transport.CloseIdleConnections()
	client := http.Client{Transport: transport}

	buffer := make([]byte, 64<<10)
	size := int64(speedPiece)
	var got int64 // this flow's
	for ctx.Err() == nil {
		request, err := http.NewRequestWithContext(ctx, http.MethodGet, fmt.Sprintf(speedURL, size), nil)
		if err != nil {
			return err.Error()
		}
		request.Header.Set("User-Agent", "sing-box "+constant.Version)
		response, err := client.Do(request)
		if err != nil {
			if ctx.Err() != nil {
				return ""
			}
			return shortSiteError(err)
		}
		if response.StatusCode != http.StatusOK {
			response.Body.Close()
			if smaller, again := nextPiece(size, response.StatusCode, got); again {
				size = smaller
				continue
			}
			return fmt.Sprintf("HTTP %d", response.StatusCode)
		}
		for {
			n, err := response.Body.Read(buffer)
			total.Add(int64(n))
			got += int64(n)
			if err == io.EOF {
				break // the piece came whole: the next one
			}
			if err != nil {
				response.Body.Close()
				if ctx.Err() != nil {
					return ""
				}
				return shortSiteError(err)
			}
		}
		response.Body.Close()
	}
	return ""
}
