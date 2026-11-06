package main

// What sov-auto learned of a network's ways, kept on disk.
//
// A way that fails a connection goes last for two minutes. One that keeps
// failing - the network cuts its protocol, as a phone's hotspot cuts QUIC -
// goes last for longer each time (autoHold), and that is worth keeping across
// a restart of the service: an update or a reboot on the same network would
// otherwise start by trying the cut way first again, and every first
// connection would wait for its stagger. Only ways with several failures in a
// row are written, only until their hold ends, by network (hashed, as
// smart-memory.json does) and tag; a success removes the record.

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const (
	autoDownMax      = 30 * time.Minute // the longest hold
	autoKeepStrikes  = 2                // failures in a row before a way is written to disk
	autoStrikeGap    = 10 * time.Second // failures closer than this are one (a burst of connections)
	autoMemoFileName = "auto-memory.json"
)

// autoHold: how long a way with `strikes` failures in a row goes last.
func autoHold(strikes int) time.Duration {
	if strikes < 1 {
		strikes = 1
	}
	hold := autoDownFor
	for i := 1; i < strikes && hold < autoDownMax; i++ {
		hold *= 2
	}
	return min(hold, autoDownMax)
}

type autoStored struct {
	At      int64  `json:"at"` // unix seconds
	Strikes int    `json:"n"`
	Why     string `json:"why,omitempty"`
}

type autoFile struct {
	Version int                              `json:"v"`
	Nets    map[string]map[string]autoStored `json:"nets"`
}

// autoMemory: the file's content in memory, kept by reported().
type autoMemory struct {
	mu      sync.Mutex
	path    string
	nets    map[string]map[string]autoStored
	pending bool
}

var autoMemo = &autoMemory{}

func autoMemoPath() string {
	dir := os.Getenv("ProgramData")
	if dir == "" {
		return ""
	}
	return filepath.Join(dir, "Sovereign", autoMemoFileName)
}

// open loads the file once; the memory outlives a restart of the core.
func (m *autoMemory) open(path string, now time.Time) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.path != "" {
		return
	}
	m.path = path
	m.nets = map[string]map[string]autoStored{}
	var file autoFile
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &file) != nil {
		return
	}
	for key, ways := range file.Nets {
		for tag, record := range ways {
			if record.Strikes >= autoKeepStrikes && now.Sub(time.Unix(record.At, 0)) < autoHold(record.Strikes) {
				if m.nets[key] == nil {
					m.nets[key] = map[string]autoStored{}
				}
				m.nets[key][tag] = record
			}
		}
	}
}

// recall: what was kept for this network, as the downs of a new box.
func (m *autoMemory) recall(network string) map[string]autoDown {
	m.mu.Lock()
	defer m.mu.Unlock()
	down := map[string]autoDown{}
	for tag, record := range m.nets[network] {
		down[tag] = autoDown{at: time.Unix(record.At, 0), why: record.Why, strikes: record.Strikes}
	}
	return down
}

// note records a way's state for a network: a failure with its strikes, or
// (ok) a success. Written a little later, all changes at once.
func (m *autoMemory) note(network, tag string, d autoDown, ok bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.path == "" {
		return
	}
	if ok || d.strikes < autoKeepStrikes {
		if _, had := m.nets[network][tag]; !had {
			return
		}
		delete(m.nets[network], tag)
	} else {
		if m.nets[network] == nil {
			m.nets[network] = map[string]autoStored{}
		}
		m.nets[network][tag] = autoStored{At: d.at.Unix(), Strikes: d.strikes, Why: d.why}
	}
	if !m.pending {
		m.pending = true
		time.AfterFunc(smartSaveAfter, m.flush)
	}
}

func (m *autoMemory) flush() {
	m.mu.Lock()
	m.pending = false
	file := autoFile{Version: 1, Nets: map[string]map[string]autoStored{}}
	now := time.Now()
	for key, ways := range m.nets {
		for tag, record := range ways {
			if now.Sub(time.Unix(record.At, 0)) < autoHold(record.Strikes) {
				if file.Nets[key] == nil {
					file.Nets[key] = map[string]autoStored{}
				}
				file.Nets[key][tag] = record
			}
		}
	}
	path := m.path
	m.mu.Unlock()
	data, err := json.Marshal(file)
	if err != nil || path == "" || os.MkdirAll(filepath.Dir(path), 0o755) != nil {
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
