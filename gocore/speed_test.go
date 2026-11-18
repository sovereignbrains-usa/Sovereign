package main

import (
	"testing"
	"time"
)

func TestSpeedFigure(t *testing.T) {
	series := func(from, to, step int, perStep func(ms int) int64) ([]time.Duration, []int64) {
		var at []time.Duration
		var counts []int64
		var total int64
		for ms := from; ms <= to; ms += step {
			total += perStep(ms)
			at = append(at, time.Duration(ms)*time.Millisecond)
			counts = append(counts, total)
		}
		return at, counts
	}
	// 1 MB a second for 8 s, after a slow start: the figure is the steady one.
	at, counts := series(100, 8000, 100, func(ms int) int64 {
		if ms <= 1000 {
			return 20_000
		}
		return 100_000
	})
	if got := speedFigure(at, counts); got < 7900 || got > 8100 { // 100 KB / 100 ms = 8000 kbit/s
		t.Fatalf("steady 8 Mbit/s read as %d kbit/s", got)
	}
	if speedFigure(at[:5], counts[:5]) != 0 {
		t.Fatal("half a second said a figure")
	}
	if speedFigure(nil, nil) != 0 {
		t.Fatal("nothing said a figure")
	}

	// A burst is not the speed: 10 MB/s for one second, the rest of the run
	// at 1 MB/s. The best 3 s said 32 Mbit/s of it (18.11.2026: the same line
	// at 122 to 329 Mbit/s from one way to the next); the run's average says
	// what a long download got.
	at, counts = series(100, 8000, 100, func(ms int) int64 {
		if ms > 4000 && ms <= 5000 {
			return 1_000_000
		}
		return 100_000
	})
	if got := speedFigure(at, counts); got < 17_500 || got > 19_500 { // 15.9 MB in 6.9 s = 18.4 Mbit/s
		t.Fatalf("a burst in a slow run read as %d kbit/s, want its average (~18.4 Mbit/s)", got)
	}

	// A slow handshake is not held against the way: nothing for the first
	// 3 s, then 1 MB/s - the warmup counts from the first byte, so what
	// is left (4 s after it) reads as 8 Mbit/s all the same.
	at, counts = series(100, 8000, 100, func(ms int) int64 {
		if ms <= 3000 {
			return 0
		}
		return 100_000
	})
	if got := speedFigure(at, counts); got < 7900 || got > 8100 {
		t.Fatalf("a run that began after 3 s read as %d kbit/s, want 8000", got)
	}

	// Data came, but too late to say anything: the first byte at 7 s.
	at, counts = series(100, 8000, 100, func(ms int) int64 {
		if ms < 7000 {
			return 0
		}
		return 100_000
	})
	if got := speedFigure(at, counts); got != 0 {
		t.Fatalf("one second of data said %d kbit/s", got)
	}
}

// Cloudflare's 403 for a piece over 50 MB (18.11.2026) made every run say
// "HTTP 403": a refusal of the first request halves the piece, a refusal
// after data came, or one at the smallest piece, ends the run.
func TestNextPiece(t *testing.T) {
	if speedPiece > 50_000_000 {
		t.Fatalf("a piece of %d bytes is over what Cloudflare's test file gives (50 MB)", speedPiece)
	}
	cases := []struct {
		name   string
		size   int64
		status int
		total  int64
		want   int64
		again  bool
	}{
		{"first piece refused: halved", 40_000_000, 403, 0, 20_000_000, true},
		{"too much asked lately, nothing yet: halved", 40_000_000, 429, 0, 20_000_000, true},
		{"refused after data came: the run ends with it", 40_000_000, 429, 9_000_000, 40_000_000, false},
		{"the smallest piece refused: error stands", 8_000_000, 403, 0, 8_000_000, false},
		{"not a refusal of size", 40_000_000, 500, 0, 40_000_000, false},
	}
	for _, c := range cases {
		got, again := nextPiece(c.size, c.status, c.total)
		if got != c.want || again != c.again {
			t.Errorf("%s: nextPiece(%d, %d, %d) = %d, %v; want %d, %v", c.name, c.size, c.status, c.total, got, again, c.want, c.again)
		}
	}
}

func TestSiteJudges(t *testing.T) {
	cases := []struct {
		judge  func(int, string) (string, string)
		status int
		body   string
		want   string
	}{
		{judgeReachable, 204, "", "ok"},
		{judgeReachable, 302, "", "ok"},
		{judgeReachable, 403, "", "blocked"},
		{judgeReachable, 451, "", "blocked"},
		{judgeReachable, 502, "", "fail"},
		{judgeOpenAI, 401, "", "ok"},
		{judgeOpenAI, 403, `{"error":{"code":"unsupported_country_region_territory"}}`, "blocked"},
		{judgeOpenAI, 403, "", "blocked"},
	}
	for _, c := range cases {
		if state, _ := c.judge(c.status, c.body); state != c.want {
			t.Errorf("status %d: %q, want %q", c.status, state, c.want)
		}
	}
}
