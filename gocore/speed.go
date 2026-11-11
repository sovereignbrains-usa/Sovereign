package main

// The speed of a way, by the user's click ("замерить" on the servers page):
// eight seconds of a download through the outbound itself from Cloudflare's
// speed test (up to ~100 MB, so only on a click). The figure is the best
// three seconds of the run, the first second (the window still growing)
// left out - what a long download would get, not what a small page shows.
// Like box_exitip it only starts the run and reports what it knows, one run
// at a time.

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
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/constant"
	M "github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/common/ntp"
)

const (
	speedURL      = "https://speed.cloudflare.com/__down?bytes=100000000"
	speedDuration = 8 * time.Second
	speedWarmup   = time.Second
	speedWindow   = 3 * time.Second
	speedMaxTags  = 512
)

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

// speedFigure: the best `window` of a run given as byte counts at moments
// since its start, in kbit/s; 0 when the run was too short to say.
func speedFigure(at []time.Duration, bytes []int64) int64 {
	var best int64
	for i := range at {
		if at[i] < speedWarmup {
			continue
		}
		// The earliest sample at least `window` before this one.
		for j := i - 1; j >= 0; j-- {
			if at[i]-at[j] >= speedWindow {
				kbps := (bytes[i] - bytes[j]) * 8 / int64((at[i]-at[j])/time.Millisecond)
				if kbps > best {
					best = kbps
				}
				break
			}
		}
	}
	return best
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
	ctx, cancel := context.WithTimeout(ctx, speedDuration+10*time.Second)
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
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, speedURL, nil)
	if err != nil {
		return speedState{Error: err.Error()}
	}
	request.Header.Set("User-Agent", "sing-box "+constant.Version)
	response, err := client.Do(request)
	if err != nil {
		return speedState{Error: shortSiteError(err)}
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return speedState{Error: fmt.Sprintf("HTTP %d", response.StatusCode)}
	}
	start := time.Now()
	var at []time.Duration
	var counts []int64
	var total int64
	buffer := make([]byte, 64<<10)
	var readErr error
	var told time.Duration
	back := 0 // the earliest sample within the last second
	for time.Since(start) < speedDuration {
		n, err := response.Body.Read(buffer)
		total += int64(n)
		now := time.Since(start)
		at = append(at, now)
		counts = append(counts, total)
		if err != nil {
			readErr = err
			break
		}
		for back < len(at)-1 && now-at[back+1] >= time.Second {
			back++
		}
		if now-told >= 250*time.Millisecond && now-at[back] >= 500*time.Millisecond {
			told = now
			progress((total - counts[back]) * 8 / int64((now-at[back])/time.Millisecond))
		}
	}
	kbps := speedFigure(at, counts)
	if kbps == 0 {
		if readErr != nil && readErr != io.EOF {
			return speedState{Error: shortSiteError(readErr)}
		}
		return speedState{Error: "too little came through to say"}
	}
	return speedState{Kbps: kbps}
}
