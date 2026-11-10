#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Per-app routing: which programs go through the proxy, matched by sing-box's
// process_name - the exe name, not the path, which
// changes with every update for apps installed per version (app-1.12.3\...).
// Unit-tested in tests/unit/app_rules_test.cpp.

namespace sovereign::tray {

enum class AppsMode : std::uint8_t {
  Exclude,  // everything through the proxy except the listed apps (default)
  Include,  // only the listed apps through the proxy, the rest direct
};

std::string_view AppsModeName(AppsMode mode);             // "exclude" / "include"
AppsMode ParseAppsMode(std::string_view name);            // unknown -> Exclude

// The tray's own config gets the rule from the routing instead (routing.h
// RoutingSettings::apps: after the blocks too); this is for a config without
// the client's routing (the conformance harness).
//
// `config` with one rule for `apps` inserted right after the leading rules
// every connection must still meet - sniff, hijack-dns, resolve, a program's
// reject, the private addresses' route - before anything else that routes by
// domain or IP:
//   Exclude: {"process_name": apps, "outbound": <direct>}
//   Include: {"process_name": apps, "outbound": <route.final or the first
//            outbound>}, and route.final becomes <direct>.
// <direct> is the config's direct outbound, added if it has none. An empty
// list changes nothing. The result is nlohmann's dump (what the service
// hashes); text that isn't a JSON object comes back unchanged.
std::string ApplyAppRules(std::string_view config, AppsMode mode, const std::vector<std::string>& apps);

}  // namespace sovereign::tray
