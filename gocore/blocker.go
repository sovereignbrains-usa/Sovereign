package main

import (
	"bytes"
	"encoding/json"
	"strings"
)

// prepareBlocker puts the block's two transports (guard.go) into the config the tray made:
// a config of standard sing-box that the official binary checks, which stays so - the tray never
// writes "sov-guard" or "sov-block"; the core does, here, right before it parses.
//
//   - Every DNS rule that answered "no addresses" (predefined NOERROR) for the block's lists only
//     becomes a route to "sov-block", cache off: a cached answer would hide the repeats from
//     the count.
//   - Every real DNS server keeps its options under a new tag, and "sov-guard" takes its old tag, so
//     each rule, the final and every domain_resolver still name the server they named.
//
// A config without the block's lists comes back as it was; so does any config that doesn't
// read as expected (the box then runs exactly what the tray wrote).
func prepareBlocker(config []byte) []byte {
	var root map[string]any
	decoder := json.NewDecoder(bytes.NewReader(config))
	decoder.UseNumber()
	if decoder.Decode(&root) != nil {
		return config
	}
	lists := blockLists(root)
	dns, _ := root["dns"].(map[string]any)
	if len(lists) == 0 || dns == nil {
		return config
	}
	servers, _ := dns["servers"].([]any)
	rules, _ := dns["rules"].([]any)
	skip := letThroughNames(rules)
	changed := false

	// The real servers behind a guard each.
	for i, item := range servers {
		server, _ := item.(map[string]any)
		kind, _ := server["type"].(string)
		tag, _ := server["tag"].(string)
		if server == nil || tag == "" || !guarded[kind] {
			continue
		}
		behind := tag + guardSuffix
		inner := map[string]any{}
		for key, value := range server {
			inner[key] = value
		}
		inner["tag"] = behind
		servers[i] = map[string]any{"type": guardType, "tag": tag, "inner": behind, "lists": lists, "skip": skip}
		servers = append(servers, inner)
		changed = true
	}

	// The refusing rules' server.
	refusing := false
	for _, item := range rules {
		rule, _ := item.(map[string]any)
		if rule == nil || !refusesBlock(rule) {
			continue
		}
		delete(rule, "rcode")
		rule["action"] = "route"
		rule["server"] = blockTag
		rule["disable_cache"] = true
		refusing = true
	}
	if refusing {
		servers = append(servers, map[string]any{"type": blockType, "tag": blockTag, "lists": lists})
		changed = true
	}
	if !changed {
		return config
	}
	dns["servers"] = servers
	out, err := json.Marshal(root)
	if err != nil {
		return config
	}
	return out
}

const (
	blockTag    = "sov-block"
	guardSuffix = "~real"
	adsPrefix   = "sov-ads" // routing.cpp: every tag of the block's lists starts so
)

// guarded: the DNS server types that ask a resolver for answers. The others (hosts, fakeip, dhcp,
// the answers made up in the config) answer by themselves or for the system: no CNAME to look at.
var guarded = map[string]bool{
	"udp": true, "tcp": true, "tls": true, "quic": true, "https": true, "h3": true, "local": true,
}

// blockLists: the block's rule-sets of the route, and the category each counts for.
func blockLists(root map[string]any) []blockList {
	route, _ := root["route"].(map[string]any)
	sets, _ := route["rule_set"].([]any)
	var lists []blockList
	for _, item := range sets {
		set, _ := item.(map[string]any)
		tag, _ := set["tag"].(string)
		if strings.HasPrefix(tag, adsPrefix) {
			lists = append(lists, blockList{Tag: tag, Category: categoryOfTag(tag)})
		}
	}
	return lists
}

// refusesBlock: a DNS rule that says "no addresses" for the block's lists and nothing else.
func refusesBlock(rule map[string]any) bool {
	if action, _ := rule["action"].(string); action != "predefined" {
		return false
	}
	if rcode, _ := rule["rcode"].(string); rcode != "NOERROR" {
		return false
	}
	if len(rule) != 3 { // rule_set, action, rcode - not a rule that also holds a name or a type
		return false
	}
	var tags []string
	switch sets := rule["rule_set"].(type) {
	case string:
		tags = []string{sets}
	case []any:
		for _, item := range sets {
			tag, _ := item.(string)
			tags = append(tags, tag)
		}
	}
	if len(tags) == 0 {
		return false
	}
	for _, tag := range tags {
		if !strings.HasPrefix(tag, adsPrefix) {
			return false
		}
	}
	return true
}

// letThroughNames: the names the config sends to a DNS server by hand - the user's "let this
// one through" (and the servers' own names) - before the block's rules. A name there is not
// refused behind a CNAME either, so the way out of a false block stays one switch.
func letThroughNames(rules []any) []string {
	var names []string
	for _, item := range rules {
		rule, _ := item.(map[string]any)
		if rule == nil {
			continue
		}
		if refusesBlock(rule) {
			break
		}
		if server, _ := rule["server"].(string); server == "" {
			continue
		}
		for _, key := range []string{"domain", "domain_suffix"} {
			switch values := rule[key].(type) {
			case string:
				names = append(names, values)
			case []any:
				for _, value := range values {
					if name, _ := value.(string); name != "" {
						names = append(names, name)
					}
				}
			}
		}
	}
	return names
}

// categoryOfTag: the block category of a list's tag - the same split blocked.go's category()
// reads from a log line.
func categoryOfTag(tag string) string {
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
