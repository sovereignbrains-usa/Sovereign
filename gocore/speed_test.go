package main

import (
	"testing"
	"time"
)

func TestSpeedFigure(t *testing.T) {
	// 1 MB a second for 8 s, after a slow start: the figure is the steady one.
	var at []time.Duration
	var counts []int64
	var total int64
	for ms := 100; ms <= 8000; ms += 100 {
		if ms <= 1000 {
			total += 20_000
		} else {
			total += 100_000
		}
		at = append(at, time.Duration(ms)*time.Millisecond)
		counts = append(counts, total)
	}
	got := speedFigure(at, counts)
	if got < 7900 || got > 8100 { // 100 KB / 100 ms = 8000 kbit/s
		t.Fatalf("steady 8 Mbit/s read as %d kbit/s", got)
	}
	if speedFigure(at[:5], counts[:5]) != 0 {
		t.Fatal("half a second said a figure")
	}
	if speedFigure(nil, nil) != 0 {
		t.Fatal("nothing said a figure")
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
