package main

// The smart outbound's memory by network and on disk.
//
// A verdict is about a path: "this site is cut here". Home and a phone's
// hotspot cut different things, so verdicts are kept per network (the
// default interface and its subnet, hashed) and the ones of the network the
// machine is on are the ones asked. What the preferred way failed is also
// kept on disk, so a restart of the service (an update, a reboot) doesn't
// cost every blocked site its first slow visit again. What the preferred
// way answered is not kept: it costs nothing to learn again, and the file
// would list every site visited.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/netip"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/sagernet/sing/common/control"
)

const (
	smartNets      = 8               // networks remembered, the least recently used go
	smartSaveAfter = 3 * time.Second // a change is written this long after the first
	smartFileName  = "smart-memory.json"
)

// smartNet: one network's verdicts. Items is the map smartMemory.items points
// at while the machine is on this network.
type smartNet struct {
	Items map[string]smartVerdict `json:"items"`
	Used  time.Time               `json:"used"`
}

type smartFile struct {
	Version int                  `json:"v"`
	Nets    map[string]*smartNet `json:"nets"`
}

// smartNetKey: the network an interface is on - its name and the IPv4 subnets
// (at most /24, so a renewed lease doesn't make a new network), hashed so the
// file doesn't carry addresses. "" for no interface.
func smartNetKey(ifc *control.Interface) string {
	if ifc == nil {
		return ""
	}
	var subnets []string
	for _, prefix := range ifc.Addresses {
		addr := prefix.Addr().Unmap()
		if !addr.Is4() || addr.IsLinkLocalUnicast() {
			continue
		}
		subnets = append(subnets, netip.PrefixFrom(addr, min(prefix.Bits(), 24)).Masked().String())
	}
	sort.Strings(subnets)
	sum := sha256.Sum256([]byte(ifc.Name + "|" + strings.Join(subnets, ",")))
	return hex.EncodeToString(sum[:8])
}

// switchNet: the machine is on network `key` now. Verdicts learned before
// the network was known (the first moments) go to it.
func (m *smartMemory) switchNet(key string, now time.Time) {
	if key == "" {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if key == m.cur {
		return
	}
	if m.nets == nil {
		m.nets = map[string]*smartNet{}
	}
	net := m.nets[key]
	if net == nil {
		net = &smartNet{Items: map[string]smartVerdict{}}
		m.nets[key] = net
	}
	if m.cur == "" {
		for k, v := range m.items {
			if _, have := net.Items[k]; !have {
				net.Items[k] = v
			}
		}
	}
	net.Used = now
	m.cur = key
	m.items = net.Items
	if len(m.nets) > smartNets {
		keys := make([]string, 0, len(m.nets))
		for k := range m.nets {
			keys = append(keys, k)
		}
		sort.Slice(keys, func(a, b int) bool { return m.nets[keys[a]].Used.Before(m.nets[keys[b]].Used) })
		for _, k := range keys[:len(keys)-smartNets] {
			delete(m.nets, k)
		}
	}
	m.touchLocked()
}

// open: the file the memory is kept in; what it holds is loaded. Without it
// (tests) nothing touches the disk.
func (m *smartMemory) open(path string, now time.Time) {
	m.mu.Lock()
	opened := m.path != ""
	m.mu.Unlock()
	if opened {
		return // the memory outlives a restart of the core: loaded once
	}
	var file smartFile
	data, err := os.ReadFile(path)
	if err == nil && json.Unmarshal(data, &file) != nil {
		file = smartFile{}
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	m.path = path
	if m.nets == nil {
		m.nets = map[string]*smartNet{}
	}
	for key, net := range file.Nets {
		if net == nil || key == m.cur {
			continue
		}
		kept := map[string]smartVerdict{}
		for name, v := range net.Items {
			if v.Why != "" && v.Way != "" && now.Sub(v.At) < smartRemember {
				kept[name] = v
			}
		}
		if len(kept) > 0 {
			m.nets[key] = &smartNet{Items: kept, Used: net.Used}
		}
	}
}

// touchLocked: a change to keep; one write, smartSaveAfter later, for all
// the changes until then. The lock is held.
func (m *smartMemory) touchLocked() {
	if m.path == "" || m.pending {
		return
	}
	m.pending = true
	time.AfterFunc(smartSaveAfter, m.flush)
}

func (m *smartMemory) flush() {
	m.mu.Lock()
	m.pending = false
	path := m.path
	file := smartFile{Version: 1, Nets: map[string]*smartNet{}}
	now := time.Now()
	for key, net := range m.nets {
		kept := map[string]smartVerdict{}
		for name, v := range net.Items {
			if v.Why != "" && now.Sub(v.At) < smartRemember {
				kept[name] = v
			}
		}
		if len(kept) > 0 {
			file.Nets[key] = &smartNet{Items: kept, Used: net.Used}
		}
	}
	m.mu.Unlock()
	if path == "" {
		return
	}
	data, err := json.Marshal(file)
	if err != nil {
		return
	}
	if os.MkdirAll(filepath.Dir(path), 0o755) != nil {
		return
	}
	partial := path + ".part"
	if os.WriteFile(partial, data, 0o600) != nil {
		return
	}
	if os.Rename(partial, path) != nil {
		_ = os.Remove(partial)
	}
}

// smartStorePath: %ProgramData%\Sovereign\smart-memory.json (the service runs
// as SYSTEM; the rule-set cache lives beside it).
func smartStorePath() string {
	dir := os.Getenv("ProgramData")
	if dir == "" {
		return ""
	}
	return filepath.Join(dir, "Sovereign", smartFileName)
}
