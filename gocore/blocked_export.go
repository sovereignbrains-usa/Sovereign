package main

/*
#include <stdlib.h>
*/
import "C"

import "encoding/json"

// box_blocked: what each block category has refused, {"counts":{"ads":n,
// "trackers":n,"cookies":n,"malware":n},"cloaked":n,"apps":[{"app","path","count"}]}: the
// refusals behind a CNAME, and the programs that asked most. Freed with box_free.
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
	return map[string]any{"counts": blocked.blockedCounts(), "cloaked": cloaked, "apps": apps}
}
