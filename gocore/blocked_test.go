package main

import (
	"strings"
	"testing"
	"time"
)

// A name asked, then the ads list matching that request: said once, again
// only after blockedRepeat; another rule, or another request, says nothing.
func TestBlockedNotice(t *testing.T) {
	w := newBlockedWatch()
	at := time.Unix(1_800_000_000, 0)
	feed := func(line string) string { return w.notice(line, at) }

	if got := feed("+0300 2026-10-09 12:00:00 DEBUG [3712645 0ms] dns: exchange Ads.Example.com. IN A\n"); got != "" {
		t.Fatalf("an ask said %q", got)
	}
	if got := feed("+0300 2026-10-09 12:00:00 DEBUG [3712645 1ms] dns: match[9] rule_set=[sov-ads sov-ads-adguard] => predefined(rcode=NOERROR)\n"); got != "ads: blocked ads.example.com" {
		t.Fatalf("blocked: %q", got)
	}
	// The same name again, from another request: not yet.
	feed("DEBUG [99 0ms] dns: exchange ads.example.com. IN AAAA")
	if got := feed("DEBUG [99 0ms] dns: match[9] rule_set=sov-ads => predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("said twice: %q", got)
	}
	at = at.Add(blockedRepeat)
	feed("DEBUG [100 0ms] dns: exchange ads.example.com. IN A")
	if got := feed("DEBUG [100 0ms] dns: match[9] rule_set=sov-ads => predefined(rcode=NOERROR)"); got != "ads: blocked ads.example.com" {
		t.Fatalf("after the pause: %q", got)
	}
	// A connection by its sniffed name, rejected by the route's rule.
	feed("DEBUG [7 0ms] router: sniffed protocol: tls, domain: tracker.example.net, client: chrome")
	if got := feed("DEBUG [7 0ms] router: match[4] rule_set=sov-ads => reject"); got != "ads: blocked tracker.example.net" {
		t.Fatalf("sniffed: %q", got)
	}
	// As the platform writer gets it: coloured, the ID too - read without colours.
	coloured := "\x1b[36mDEBUG\x1b[0m[0003] [\x1b[38;5;27m42\x1b[0m 15ms] dns: exchange ads.example.org. IN A"
	feed(ansiEscape.ReplaceAllString(coloured, ""))
	if got := feed(ansiEscape.ReplaceAllString("\x1b[36mDEBUG\x1b[0m[0003] [\x1b[38;5;27m42\x1b[0m 15ms] dns: match[0] rule_set=[sov-ads sov-ads-adguard] => predefined(NOERROR)", "")); got != "ads: blocked ads.example.org" {
		t.Fatalf("coloured: %q", got)
	}
	// The user's own block, and a match with no name known: nothing.
	feed("DEBUG [8 0ms] dns: exchange yandex.com. IN A")
	if got := feed("DEBUG [8 0ms] dns: match[2] domain_suffix=yandex.com => predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("the user's rule: %q", got)
	}
	if got := feed("DEBUG [9 0ms] dns: match[9] rule_set=sov-ads => predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("no name: %q", got)
	}
}

// Every match is a count of its category, by the first list of its rule; the A and the AAAA of
// one name together are one; the user's own rule counts nothing.
func TestBlockedCounts(t *testing.T) {
	w := newBlockedWatch()
	at := time.Unix(1_800_000_000, 0)
	hit := func(id, name, rule string) {
		w.notice("DEBUG ["+id+" 0ms] dns: exchange "+name+". IN A", at)
		w.notice("DEBUG ["+id+" 1ms] dns: match[3] rule_set="+rule+" => predefined(rcode=NOERROR)", at)
	}
	hit("1", "ads.example.com", "[sov-ads sov-ads-adguard]")
	hit("2", "ads.example.com", "[sov-ads sov-ads-adguard]") // the AAAA, at once
	hit("3", "pixel.example.net", "[sov-ads-trackers sov-ads-trackers-lowe]")
	hit("4", "evil.example.org", "[sov-ads-malware sov-ads-malware-dandelion sov-ads-phishing sov-ads-scams]")
	hit("5", "consent.example.org", "sov-ads-cookies")
	hit("6", "yandex.com", "domain_suffix=yandex.com")
	at = at.Add(countWindow)
	hit("7", "ads.example.com", "[sov-ads sov-ads-adguard]") // later: another block
	got := w.blockedCounts()
	want := map[string]uint64{"ads": 2, "trackers": 1, "malware": 1, "cookies": 1}
	if len(got) != len(want) {
		t.Fatalf("counts %v, want %v", got, want)
	}
	for cat, n := range want {
		if got[cat] != n {
			t.Fatalf("counts %v, want %v", got, want)
		}
	}
}

// The Russian sites' block (14.11.2026: funpay.com and the Google sign-in were cut and nothing said
// so): its match lines - the zones, the geosite and the geoip lists, each but for google.ru - are
// counted "russia" and kept apart, and say nothing to the ads' list; an allowed one (directly) or
// the user's own rule isn't theirs.
func TestBlockedRussia(t *testing.T) {
	w := newBlockedWatch()
	at := time.Unix(1_800_000_000, 0)
	hit := func(id, name, rule, action string) string {
		w.notice("DEBUG ["+id+" 0ms] dns: exchange "+name+". IN A", at)
		return w.notice("DEBUG ["+id+" 1ms] dns: match[2] "+rule+" => "+action, at)
	}
	const but = " && !(domain_suffix=google.ru)"
	if got := hit("1", "funpay.com", "rule_set=[sov-geosite-ru sov-geosite-gov-ru]"+but, "predefined(rcode=NOERROR)"); got != "" {
		t.Fatalf("a Russian block said %q", got)
	}
	hit("2", "ya.ru", "domain_suffix=[ru su xn--p1ai...]"+but, "predefined(rcode=NOERROR)")
	hit("3", "ya.ru", "domain_suffix=[ru su xn--p1ai...]"+but, "predefined(rcode=NOERROR)") // the AAAA
	w.notice("DEBUG [4 0ms] router: sniffed protocol: tls, domain: vk.com, client: chrome", at)
	w.notice("DEBUG [4 1ms] router: match[5] rule_set=sov-geoip-ru"+but+" => reject", at)
	hit("5", "ads.example.com", "rule_set=[sov-ads sov-ads-adguard]", "predefined(rcode=NOERROR)")
	hit("6", "mail.ru", "domain_suffix=[ru su xn--p1ai...]"+but, "route(sov-local)") // let through: no block
	hit("7", "x.ru", "domain_suffix=yandex.ru", "predefined(rcode=NOERROR)")          // the user's own
	got := w.blockedCounts()
	if got["russia"] != 3 || got["ads"] != 1 || len(got) != 2 {
		t.Fatalf("counts %v", got)
	}
	now := at.Add(time.Second)
	// All at the same moment: by name.
	if names := w.recentRussia(now, 10); strings.Join(names, ",") != "funpay.com,vk.com,ya.ru" {
		t.Fatalf("Russian recent %v", names)
	}
	if names := w.recentNames(now, 10); len(names) != 1 || names[0] != "ads.example.com" {
		t.Fatalf("recent %v: the Russian names don't belong to it", names)
	}
	if got := blockedAnswer()["recent_ru"]; got == nil {
		t.Fatalf("box_blocked without recent_ru")
	}
}

// "Just now": every refusal of a name renews it (the notice says it once in ten minutes), the
// newest first, what's older than recentWindow out, at most `most`.
func TestBlockedRecent(t *testing.T) {
	w := newBlockedWatch()
	at := time.Unix(1_800_000_000, 0)
	w.mu.Lock()
	w.count("ads", "old.example.com", "", false, at)
	w.count("ads", "ads.example.com", "", false, at.Add(time.Minute))
	w.count("trackers", "pixel.example.net", "", false, at.Add(time.Minute+time.Second))
	w.count("ads", "ads.example.com", "", false, at.Add(2*time.Minute))      // again: the newest now
	w.count("ads", "", "", false, at.Add(2*time.Minute))                     // no name: nothing to list
	w.count("malware", "bank.example.org", "", false, at.Add(2*time.Minute)) // never in a bunch
	w.mu.Unlock()
	now := at.Add(2*time.Minute + time.Second)
	got := w.recentNames(now, 10)
	want := []string{"ads.example.com", "pixel.example.net"} // old.example.com: over two minutes
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("recent %v, want %v", got, want)
	}
	if got := w.recentNames(now, 1); len(got) != 1 || got[0] != "ads.example.com" {
		t.Fatalf("recent at most 1: %v", got)
	}
	if got := blockedAnswer()["recent"]; got == nil {
		t.Fatalf("box_blocked without recent")
	}
}
