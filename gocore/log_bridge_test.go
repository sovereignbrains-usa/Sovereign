package main

import "testing"

// The two WARP lines that were not errors are told apart from the ones that are.
func TestHarmlessWarpNoise(t *testing.T) {
	yes := []string{
		"ERROR[0161] endpoint/wireguard[warp]: peer(bmXO…fgyo) - failed to derive keypair: invalid state for keypair derivation: handshakeZeroed",
		"ERROR[0000] endpoint/wireguard[warp]: unable to update bind: listen udp6 [::]:52157: An invalid argument was supplied.",
	}
	no := []string{
		"ERROR[0399] connection: connection download closed: dial tcp [2600:1900::]:24678: i/o timeout",
		"ERROR[0000] endpoint/wireguard[warp]: unable to update bind: listen udp4 0.0.0.0:52157: address already in use",
		"ERROR[0000] outbound/direct: failed to derive keypair: invalid state for keypair derivation: handshakeZeroed",
	}
	for _, m := range yes {
		if !harmlessWarpNoise(m) {
			t.Errorf("not recognised: %s", m)
		}
	}
	for _, m := range no {
		if harmlessWarpNoise(m) {
			t.Errorf("hidden but real: %s", m)
		}
	}
}
