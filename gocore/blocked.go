package main

import (
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// What the ads lists block, said for the tray: its "Реклама" chip lists the
// names, so a site the list breaks can be let through. sing-box says it only
// at debug level, in two lines tied by the request's ID - what's asked
//
//	[3712645 0ms] dns: exchange ads.example.com. IN A
//	[3712645 0ms] router: sniffed protocol: tls, domain: ads.example.com
//
// and then the rule it matched:
//
//	[3712645 1ms] dns: match[9] rule_set=[sov-ads sov-ads-adguard] => predefined(rcode=NOERROR)
//
// Platform writers get every level, so platformWriter shows every line here;
// a blocked name comes back as one info line of its own, "ads: blocked
// ads.example.com", at most once in blockedRepeat whatever log.level is.

const (
	adsListPrefix = "sov-ads"        // routing.cpp's ads lists: sov-ads, sov-ads-adguard
	blockedRepeat = 10 * time.Minute // a name said again after this
	askedKeep     = 4096             // requests remembered, their names
	appsKeep      = 256              // programs counted
	blockedNotice = "ads: blocked "  // what the tray looks for
)

var idLine = regexp.MustCompile(`\[(\d+) [^\]]+\] (.+)$`)

type blockedWatch struct {
	mu    sync.Mutex
	asked map[string]string // request ID -> the name it's about
	order []string          // the IDs in `asked`, oldest first
	said  map[string]time.Time
	// What each block category refused, every match counted (the notice above says a name
	// once in ten minutes): "ads", "trackers", "cookies", "malware". box_blocked says it.
	counts  map[string]uint64
	counted map[string]time.Time // category+name -> when last counted
	// Who asked: the program (its path) -> how many refusals; and how many of all were a
	// name behind a CNAME (guard.go). Only what the core itself refuses says a program.
	apps    map[string]uint64
	cloaked uint64
	// When each name was last refused, every refusal (not once in ten minutes as the notice):
	// what a page that just broke had cut - the tray's "Сломалось только что" lets it through.
	// Not the malware lists' names: those are let through one by one, if at all.
	last map[string]time.Time
}

const (
	recentWindow = 2 * time.Minute // "just now": what box_blocked's "recent" holds
	recentShown  = 40              // at most so many names in it
)

var blocked = newBlockedWatch()

func newBlockedWatch() *blockedWatch {
	return &blockedWatch{asked: map[string]string{}, said: map[string]time.Time{}, counts: map[string]uint64{},
		counted: map[string]time.Time{}, apps: map[string]uint64{}, last: map[string]time.Time{}}
}

// recentNames: the names refused within recentWindow of `now`, newest first, at most `most`.
func (w *blockedWatch) recentNames(now time.Time, most int) []string {
	w.mu.Lock()
	defer w.mu.Unlock()
	type seen struct {
		name string
		at   time.Time
	}
	var list []seen
	for name, at := range w.last {
		if now.Sub(at) < recentWindow {
			list = append(list, seen{name, at})
		}
	}
	sort.Slice(list, func(i, j int) bool {
		if !list[i].at.Equal(list[j].at) {
			return list[i].at.After(list[j].at)
		}
		return list[i].name < list[j].name
	})
	if len(list) > most {
		list = list[:most]
	}
	names := make([]string, 0, len(list))
	for _, s := range list {
		names = append(names, s.name)
	}
	return names
}

// askedName: the name a line says is asked or sniffed, or "".
func askedName(text string) string {
	switch {
	case strings.HasPrefix(text, "dns: exchange "):
		if fields := strings.Fields(strings.TrimPrefix(text, "dns: exchange ")); len(fields) > 0 {
			return strings.TrimSuffix(fields[0], ".")
		}
	case strings.HasPrefix(text, "dns: lookup domain "):
		return strings.TrimSpace(strings.TrimPrefix(text, "dns: lookup domain "))
	case strings.HasPrefix(text, "router: sniffed "):
		if at := strings.Index(text, ", domain: "); at >= 0 {
			name, _, _ := strings.Cut(text[at+len(", domain: "):], ",")
			return strings.TrimSpace(name)
		}
	}
	return ""
}

// category: which block category the rule of a match line belongs to, by the first list in its
// rule_set - routing.cpp makes one rule a category, so its lists are all one category.
func category(text string) string {
	at := strings.Index(text, "rule_set=")
	if at < 0 {
		return "ads"
	}
	tag := strings.TrimPrefix(text[at+len("rule_set="):], "[")
	if end := strings.IndexAny(tag, " ]"); end >= 0 {
		tag = tag[:end]
	}
	switch {
	case strings.HasPrefix(tag, "sov-ads-trackers"):
		return "trackers"
	case strings.HasPrefix(tag, "sov-ads-malware"), tag == "sov-ads-phishing", tag == "sov-ads-scams":
		return "malware"
	case tag == "sov-ads-cookies":
		return "cookies"
	}
	return "ads"
}

// countWindow: the A and the AAAA of one name, asked together, are one block.
const countWindow = 3 * time.Second

// count: one block of `name` by `cat`, asked by the program `app` (its path, or ""), a name
// behind a CNAME when `cloaked`; w.mu is held.
func (w *blockedWatch) count(cat, name, app string, cloaked bool, now time.Time) {
	if name != "" && cat != "malware" { // a phishing name isn't let through in a bunch
		w.last[name] = now
		if len(w.last) > askedKeep {
			for n, at := range w.last {
				if now.Sub(at) >= recentWindow {
					delete(w.last, n)
				}
			}
		}
	}
	if name != "" {
		key := cat + " " + name
		if last, ok := w.counted[key]; ok && now.Sub(last) < countWindow {
			return
		}
		w.counted[key] = now
		if len(w.counted) > askedKeep {
			for k, at := range w.counted {
				if now.Sub(at) >= countWindow {
					delete(w.counted, k)
				}
			}
		}
	}
	w.counts[cat]++
	if cloaked {
		w.cloaked++
	}
	if app != "" {
		if _, known := w.apps[app]; known || len(w.apps) < appsKeep {
			w.apps[app]++
		}
	}
}

// blockedCounts: what each category has refused since the engine loaded.
func (w *blockedWatch) blockedCounts() map[string]uint64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	out := make(map[string]uint64, len(w.counts))
	for cat, n := range w.counts {
		out[cat] = n
	}
	return out
}

// blockedApps: the programs that asked most for what was refused, most first (at most `most`),
// and how many of all refusals were behind a CNAME.
type appCount struct {
	App   string `json:"app"`  // the program's file name
	Path  string `json:"path"` // and where it is
	Count uint64 `json:"count"`
}

func (w *blockedWatch) blockedApps(most int) (apps []appCount, cloaked uint64) {
	w.mu.Lock()
	defer w.mu.Unlock()
	for path, n := range w.apps {
		apps = append(apps, appCount{App: programName(path), Path: path, Count: n})
	}
	sort.Slice(apps, func(i, j int) bool {
		if apps[i].Count != apps[j].Count {
			return apps[i].Count > apps[j].Count
		}
		return apps[i].Path < apps[j].Path
	})
	if len(apps) > most {
		apps = apps[:most]
	}
	return apps, w.cloaked
}

// notice: the line to say for a log `message` (sing-box's, formatted), or "".
func (w *blockedWatch) notice(message string, now time.Time) string {
	if !strings.Contains(message, "] dns: ") && !strings.Contains(message, "] router: ") {
		return ""
	}
	m := idLine.FindStringSubmatch(strings.TrimRight(message, "\r\n"))
	if m == nil {
		return ""
	}
	id, text := m[1], m[2]
	w.mu.Lock()
	defer w.mu.Unlock()
	if name := askedName(text); name != "" {
		if _, known := w.asked[id]; !known {
			w.order = append(w.order, id)
		}
		w.asked[id] = strings.ToLower(name)
		for len(w.order) > askedKeep {
			delete(w.asked, w.order[0])
			w.order = w.order[1:]
		}
		return ""
	}
	if !strings.Contains(text, "match[") || !strings.Contains(text, "rule_set=") ||
		!strings.Contains(text, adsListPrefix) ||
		(!strings.Contains(text, "=> predefined") && !strings.Contains(text, "=> reject")) {
		return ""
	}
	name := w.asked[id]
	w.count(category(text), name, "", false, now)
	return w.say(name, now)
}

// say: the notice for a blocked `name`, or "" when it was said lately; w.mu is held.
func (w *blockedWatch) say(name string, now time.Time) string {
	if name == "" {
		return ""
	}
	if last, ok := w.said[name]; ok && now.Sub(last) < blockedRepeat {
		return ""
	}
	w.said[name] = now
	if len(w.said) > askedKeep {
		for n, at := range w.said {
			if now.Sub(at) >= blockedRepeat {
				delete(w.said, n)
			}
		}
	}
	return blockedNotice + name
}
