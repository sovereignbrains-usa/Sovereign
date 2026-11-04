package main

/*
#include <stdlib.h>
*/
import "C"

import "encoding/json"

// box_blocked: what each block category has refused, {"counts":{"ads":n,
// "trackers":n,"cookies":n,"malware":n}}. Freed with box_free.
//
//export box_blocked
func box_blocked() *C.char {
	out, err := json.Marshal(map[string]any{"counts": blocked.blockedCounts()})
	if err != nil {
		return C.CString(`{"counts":{}}`)
	}
	return C.CString(string(out))
}
