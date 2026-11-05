package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"time"
)

// box_blocked: what each block category has refused, {"counts":{"ads":n,
// "trackers":n,"cookies":n,"malware":n},"cloaked":n,"apps":[{"app","path","count"}],
// "recent":[name]}: the refusals behind a CNAME, the programs that asked most, and the names
// refused in the last two minutes, newest first. Freed with box_free.
//
//export box_blocked
func box_blocked() *C.char {
	out, err := json.Marshal(blockedAnswer())
	if err != nil {
		return C.CString(`{"counts":{}}`)
	}
	return C.CString(string(out))
}

const appsShown = 12

func blockedAnswer() map[string]any {
	apps, cloaked := blocked.blockedApps(appsShown)
	if apps == nil {
		apps = []appCount{}
	}
	return map[string]any{"counts": blocked.blockedCounts(), "cloaked": cloaked, "apps": apps,
		"recent": blocked.recentNames(time.Now(), recentShown)}
}
