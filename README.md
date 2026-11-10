# Sovereign

[![CI](https://github.com/sovereignbrains-usa/Sovereign/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sovereignbrains-usa/Sovereign/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/sovereignbrains-usa/Sovereign)](https://github.com/sovereignbrains-usa/Sovereign/releases/latest)
[![sing-box](https://img.shields.io/badge/sing--box-1.14.4-blue)](https://github.com/SagerNet/sing-box/releases/tag/v1.14.4)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

> **Experimental, still being finished.** Sovereign is under active development; the releases are not recommended for anyone else to download yet. Expect rough edges and changes without notice.

> **New address.** Since 10.11.2026 the project lives here, with its whole history; the releases up to 0.5.130 stayed at [sovereignbrains/Sovereign](https://github.com/sovereignbrains/Sovereign/releases). A 0.5.x install looks for updates there: install [0.6.0](https://github.com/sovereignbrains-usa/Sovereign/releases/latest) by hand once, and it updates itself from here on.

A native Windows client for [sing-box](https://github.com/SagerNet/sing-box), written in C++23 — and built so that its quality can be checked rather than claimed.

Third-party sing-box clients have a reputation for being thin, sloppy wrappers. Sovereign's answer is evidence in CI:

- **Servers are written from generated types, not by hand.** Every outbound and WireGuard endpoint the client writes (share links, subscriptions, WARP) is a C++ option struct generated from the pinned sing-box sources by `tools/codegen` (real Go type-checking, not regex; sing-box's discriminated unions read from its own schema). CI regenerates them and fails on any drift from upstream, and the import tests require our servers to come out of `sing-box format` unchanged - and to read back through the generated structs the same. The routing frame around them (DNS, rules, TUN) is still assembled by hand.
- **What goes on the wire is compared with the reference, byte for byte.** A conformance harness captures the TLS ClientHello Sovereign's core sends and the one the official sing-box release sends, and diffs them outside the fields that are random by design.
- **Clean C++.** RAII only, no `new`/`delete`, warnings as errors, MSVC `/analyze`, clang-tidy, AddressSanitizer, ThreadSanitizer, fuzzing.

## Status

The client works and is in daily use, released as an experimental build (0.6.0, 11.11.2026; what changed: [CHANGELOG](CHANGELOG.md)). The hybrid core and the conformance harness are done; now the work is the layers for the three kinds of users. Plan, gates and how each phase was accepted: [docs/roadmap.md](docs/roadmap.md).

| Phase | What | State |
|---|---|---|
| P0 | CMake + vcpkg, Windows service, named pipe, CI | done |
| P1 | Option structs generated from sing-box, golden tests | done (G1) |
| P2 | Hybrid MVP: sing-box as a DLL inside the service, TUN, tray | done: accepted on a clean Windows 11, 02.10.2026 |
| P2.5 | Per-UA subscription, ETW logging, system DNS left alone | done |
| P3 | Conformance harness against the pinned reference | done (G3): byte-identical ClientHello for AnyTLS + REALITY, AnyTLS + TLS and VLESS + REALITY in CI; fuzzing and ThreadSanitizer in CI |
| P6 | Subscribers, the provider's support, self-hosted, God Mode | in progress: the first three are in, God Mode is not |
| P4 | NativeCore (own TLS/transport), optional | not started |
| P5 | The harness offered upstream | not started |

## What it does today

The interface is in Russian. Paste a subscription, press the switch, forget about it — and everything past that is a click away when it's wanted.

**The main screen** — a cube for a switch (its colour and pose are the connection's state), the traffic drawn live around it (through the VPN, directly, through WARP, ads blocked on its face), the server in use with its country's flag (a click opens the servers), and a card per configuration: its name and till when it's paid, with "+" to add another (the clipboard, a QR code on the screen, a file). The title bar holds the tools (the speed test, the checks, where an address goes, the log) and the settings. A click on the notification-area icon opens the window by it; a right click gives on/off, open, the log, quit.

**Configurations** — subscriptions, sets of keys and config files, as many as you add, every one that's on running together: their servers go into one list with `auto`. Anything a provider or another client hands out is taken: an `https://` link (bare or inside an app's deep link), a whole sing-box config, proxy keys (`vless`, `vmess`, `trojan`, `ss`, `hysteria2`, `hysteria`, `tuic`, `anytls`, `socks`, `naive`, `wireguard`, `ssh`, `snell` — on lines, glued together, percent-encoded or base64), Clash/Mihomo YAML, Xray/v2rayN JSON, SIP008, or a picture of QR codes. Every config built that way passes the pinned sing-box's `check` in CI. A subscription server sees as little as can be: a `sing-box 1.x.y` User-Agent, an `x-hwid` that is random per configuration (never the machine's), and the VPN's address rather than yours; with no connection up, an optional relay (your own Cloudflare Worker, `tools/relay`) fetches it instead. A provider's "auto" balancer servers are hidden — the tray picks. Servers the core can't take (Xray's `xhttp`, which sing-box lacks) are said, not dropped silently: "Взято серверов: 3 из 5" and why. Our own extension of `hysteria2://` links pins the server's key (`pubkey-sha256`) and sets the port-hop pace. **A server of your own** («Свой сервер — поставить на него VPN», under the configurations and among the add ways on the main screen): type its address (`[user@]host[:port]`), and the client looks at the machine over ssh (the system's `ssh.exe`, by key, never a password prompt): root, Debian 12 or newer on x86_64, which of ports 80 and 443 are taken. «Поставить VPN» then installs [packetlab](https://github.com/sovereignbrains-usa/packetlab) on it from a pinned commit, step by step on the page («Шаг 2 из 6 · …»), and keeps the ways a client wants beside each other: Hysteria 2 with salamander (and its port-hopping twin) for UDP, REALITY and AnyTLS + REALITY over TCP for a network that filters UDP. The installer's log stays in the configuration's folder with the keys cut out. When a subscription is in trouble (unpaid, out of traffic, its servers silent), the main screen says so and offers a report for the provider's support: only what concerns their service, nothing about you.

**Servers** — a machine is one card with its ping, fastest first; its ways (the protocols it serves) lie under the one in use, each with a dot (works, in trouble, fails here, not known), «сейчас» or «закреплён», and three columns: **ОТКРЫТИЕ**, how long a new site waits for its first byte through it (the median of three fresh connections); **ИГРЫ**, UDP through it as a game feels it — STUN round trips, the step between consecutive ones and the loss, or «нет UDP» when the protocol carries none; **СКОРОСТЬ**, what it really carried in the last hour (the core's own connection tracker), or a run of its own on «замерить» (an 8 s download through that way, its best 3 s). Beside them, which of YouTube, Telegram, ChatGPT and Instagram open through it (ChatGPT's "unsupported country" counted as closed). The ping itself is measured so that it means something: one connection, then requests over it until the round trips settle, shown as the median ± half their spread with losses (sing-box's own URL test is one cold request); the digits the spread doesn't prove are dimmed. A folded card says how many of its ways work and the fastest opening.

**`auto` runs in the core.** The tray picks the server from its measurements and moves only once another has been clearly better twice in a row (on one machine, to another protocol only for a clear win); the core's `sov-auto` does what a tray polling every few seconds can't: a connection the pick doesn't open goes on to the next way at once (the same machine's next protocol first, then the other machines), the next one is started when the first takes twice its usual time (whichever opens first carries the connection), a way that failed goes last for a while — per network (a way dead on the phone's hotspot is tried first again at home), and kept on disk once it failed twice in a row, so a restart doesn't send the first connection into a way known dead — and UDP has a choice of its own. Switching never restarts the connection.

**Routing and DNS are the client's** — a configuration gives servers; the TUN, the DNS and the rules are Sovereign's. The routing page is a metro scheme: on a spine of four ways — through the VPN, directly, through WARP, blocked — sit services (30 of them: Netflix, YouTube, ChatGPT, Telegram — its desktop app by its IP ranges too —, Steam and Epic games, torrent clients by their programs…; a service can also go through a country — the fastest server whose exit is there), programs, sites, Russian sites, ads, cookie banners and the local network. Three modes: everything through the VPN but the exceptions; only what's chosen; or **«в обход блокировок»** — everything directly, and what is cut there through the VPN, the core deciding per site. That last one is an outbound of the core's own, `sov-smart`: it sends a connection's TLS hello down both ways, keeps the first that answers and remembers the verdict per site and per network (on disk, never the sites that opened directly); it catches the cut after the first 16 KB and slowed connections too, sends what a race can't see (Telegram, TikTok, the AI services) through the VPN by itself, and the page lists what the client decided on its own.

**Block** — a catalog of categories (ads, trackers, cookie banners, malicious sites), each counting what it refused. The core's own DNS guard follows CNAME chains, so a tracker hiding behind a site's own name is refused too, and it tells which program asked for a blocked name ("Кто обращается"; a click shows the file). While the block is on, browsers' own DNS over HTTPS/TLS/QUIC to the public resolvers is closed, so they fall back to the system's DNS, which is ours; "Сломалось только что" lets everything through for two minutes. WARP is a Cloudflare WireGuard endpoint the client registers itself, over the VPN or directly. Fine settings, folded: QUIC (blocked automatically when the servers can't carry it as UDP), Hysteria 2's Brutal speed, IPv4 only, the DNS for sites through the VPN and for direct ones (encrypted, auto by default, or your own: plain, TLS, HTTPS, QUIC). The lists are SagerNet's `sing-geosite`/`sing-geoip` and AdGuard's, fetched by the tray, refreshed daily and given to the core as local files; every combination is checked against the pinned sing-box in CI. The config stays yours to edit: over your edits a new version of the subscription waits, and you take it, keep yours or carry your edits over (a three-way merge).

**Protection** — a kill switch (on by default): while the connection is meant to be on nothing leaves the machine but through the VPN, and if the service falls over or the machine reboots, the internet stays shut until it's back. While Sovereign updates itself with the VPN on, the kill switch holds the internet shut even when it's off, till the new version's VPN is up (or three minutes). If the connection can't come up while the kill switch holds the internet shut, the tray looks once for another VPN in the way — a VPN program running, or its adapter up — and names it, so you turn it off instead of blaming the provider; it never fights one (a work VPN is wanted). The local network can be closed both ways (the shared 100.64.0.0/10 range too), the router included, with the devices you choose let through: IPv4 addresses only (the local network over IPv6 stays shut whole), and only past Sovereign's filters — what Windows' own firewall refuses in, a connection nothing on the PC asked for, it still refuses. Closed, the router's DNS is shut for the core as well (WFP's hard block holds over sing-tun's hard permit for it, tried), so a "system" local DNS gives way to an encrypted one. The window's edge takes the colour of the main way: blue for the VPN, orange when WARP carries more, red while the kill switch holds. With "start and connect" the service brings the tunnel up at boot, before anyone signs in — no browser in the Startup folder gets there first.

**Checks** — the tray's own requests go through the TUN and the rules like any program's, so they meet what your programs meet: the VPN's exit, what Russian sites see, a DNS leak test (bash.ws), WebRTC (a STUN request, as a browser's), IPv6, the local network, DNS latency, where the Russian, European and American sites go (asked of the core's own connection tracker), and what the computer shows to the local network (the firewall's profiles, sharing services, listening ports). They know the mode: with only chosen things through the VPN, a site going directly is as meant, not a leak. A failed one offers its way out — turn the kill switch on, reconnect, open the routing or the DNS. The speed test streams each way for a few seconds against Cloudflare and draws a graph as it goes. `diagnose-probe` runs the same checks from a console.

**Log** — the core's lines in colour, filters by level, pause, save, a line in full on a click, scrolled with the middle button as in a browser; a line repeating the one before folded into a count; noise the core reports as errors (a stream closed without one) shown as what it is.

**Updates and uninstalling** — the tray checks for a new release and updates in place (the installer checked against the release's `.sha256`), with percent and time left while it downloads; unasked, it installs only after the computer has been quiet for a while you choose (2 minutes by default), and the installer the service runs leaves its log in `Update\setup.log`. Uninstalling leaves nothing behind: the service, the filters, the logs, the cache, the boot config and the downloaded updates always go; the subscriptions and settings (your servers' keys) too, unless you say keep them.

Drawn with Direct2D, keyboard-navigable (Tab, Enter, Esc, Ctrl+1..5 for the main screen, servers, configurations, log and settings). Started by hand the tray opens the window; from the Run key (`--background`) it stays in the notification area. Closing the window only hides it. There is one tray per computer: started in another user's session it says so, and leaves the first one's connection, kill switch and boot config alone.

**Service** (`sovereign-core.exe`, runs as SYSTEM):

- hosts sing-box (`sovereign-gocore.dll`, the pinned version built with upstream's Windows release tags) behind an `ICore` interface, with the core's own parts beside it: the `sov-auto` and `sov-smart` outbounds, the DNS guard and the block's counters, the latency, UDP, throughput and speed measurements, the per-server check of sites; the config sing-box sees stays standard, the Go side adds its parts before parsing;
- a panic on the Go side ends that one call (logged with its stack), not the SYSTEM service and the connection with it; a DLL that fails to load at the start is tried again, five times two seconds apart;
- TUN through wintun; system DNS settings are never touched — DNS is hijacked inside the route;
- enforces the kill switch and the closed local network with Windows Filtering Platform filters of its own (persistent, in their own sublayer);
- starts the config kept for boot by itself (DPAPI-encrypted, readable by SYSTEM and Administrators only), restarts the box after sleep, keeps a cache file so remote rule sets don't block a start without network;
- answers the checks from the core's traffic manager — where the newest connection to an address went, by which rule;
- logs through ETW (TraceLogging provider `Sovereign.Core`) with an always-on flight recorder.

## How it fits together

```
sovereign-tray.exe  (user session)                sovereign-core.exe  (Windows service, SYSTEM)
  window, configurations, routing,
  builds the effective config         ──JSON over \\.\pipe\sovereign-control──►  ControlHandler
                                                                                   │ ICore
                                                                                   ▼
                                                                          GoCore → sovereign-gocore.dll
                                                                                   (sing-box, c-shared)
                                                                                   │
                                                                          TUN (wintun) → outbounds
```

The tray owns the state (settings, configurations, routing) and sends the service a complete config; the service keeps only the one it's asked to start at boot. The pipe is the only link between them, and there is no pipe into the core — `ICore` is an in-process interface, so a native core (P4) can replace the Go one without changing anything around it.

## Repository layout

```
src/service/      the service: pipe server, control protocol, GoCore bridge, kill switch (WFP), boot config, ETW
src/tray/         the tray: its window (Direct2D), worker, configurations, routing, checks
src/common/       shared headers and the handwritten JSON adapters (Duration, Listable, ...)
src/generated/    C++ option structs generated from sing-box — do not edit, regenerate
src/conformance/  ClientHello parser and canonical form for the harness
gocore/           the Go side: sing-box built as a c-shared DLL with a C ABI
installer/        Inno Setup script and its build and smoke scripts
assets/           icon, fonts, flags and service logos the window draws
tools/codegen/    Go -> C++ option-struct generator; pinned versions (sing-box, wintun)
tools/goldengen/  emits golden JSON through sing-box's own marshaler
tools/relay/      the subscription relay: a Cloudflare Worker
tools/trace/      sovtrace.ps1 — start/stop/dump the ETW recording
tools/fuzz/       run.ps1 — the fuzz targets locally, with the corpora and the dictionary
tools/boxrun/, gencert/, httptarget/   local sing-box, certificate and target for manual checks
tools/bump-singbox.ps1   moves the pinned sing-box to a new tag (checked daily by CI)
tests/unit/       unit tests (control protocol, tray logic, adapters)
tests/golden/     generated structs and adapters against golden JSON
tests/conformance/  the harness, golden ClientHellos, captured fixtures
tests/fuzz/       libFuzzer targets, seed corpora, the JSON dictionary (run with tools/fuzz/run.ps1)
tests/tsan/       the control plane's and the tray's threads raced under ThreadSanitizer
tests/ui/         ui-snapshot: the window's pages drawn into PNGs and laid out as text, no desktop needed
tests/smoke/      service smoke tests run by CI (GoCore, ETW, flight recorder)
bench/            comparison with other clients: the scripts, the server setup, the results
docs/             roadmap.md (phases, gates, decisions), design/ (window and server notes)
CHANGELOG.md      what changed per release; the release notes are taken from it
SECURITY.md       how to report a vulnerability privately
```

## How quality is checked

Every push (and a nightly run) goes through eight jobs:

- **build** — Debug build with warnings as errors, all tests, the real GoCore smoke test, the ETW flight-recorder test under the real SCM, the conformance harness against the official sing-box release (downloaded and sha256-pinned), the window's pages drawn to PNG (the `ui-snapshots` artifact) and laid out as text (clickable things overlapping or past the edge fail), and typing in the real window driven by keys;
- **analyze** — the whole tree under MSVC `/analyze`;
- **asan** — everything under AddressSanitizer (the Go runtime can't share an ASan process, so GoCore's C++ side is exercised against a stub DLL with the same C ABI — [why](docs/roadmap.md#why-the-asan-job-runs-a-stub-core));
- **tsan** — the service's control plane and the tray's logic, built on Linux with clang, raced by threads under ThreadSanitizer;
- **clang-tidy** — with the clang-tidy bundled with the runner's Visual Studio;
- **fuzz** — four libFuzzer targets (MSVC `/fsanitize=fuzzer` with ASan), a minute each per push and twenty minutes each nightly: the ClientHello parser, the generated config structs and adapters, everything the tray reads that it didn't write (subscriptions, service answers), and the service's control protocol — what any local user can send a SYSTEM service. Besides "no crash" each checks properties: exact round trips, exactly one JSON answer per request. Seeds live in `tests/fuzz/corpus`, every past finding among them;
- **codegen-drift** — the option structs regenerated from the pinned sing-box: any difference from what's committed fails;
- **installer** — the installer built, then installed, updated and uninstalled on the runner.

The conformance harness in short: five connections of the same client differ on the wire (randoms, GREASE, key shares, Chrome's extension order, ECH GREASE), so the harness first proves that the reference agrees with itself after masking exactly those fields — a mutation test checks that every other byte is compared — and only then compares Sovereign's core with it. Details: the plan and measurements in [the roadmap](docs/roadmap.md#p3--conformance-harness).

Stubs are allowed only with a name, a reason and a destination: generated fields that still need a handwritten adapter say so in `src/generated/*.gen.h`.

## Compared with other clients

A first, rough run (17.10.2026, one measurement per client, details and scripts in [bench/](bench/README.md)): Sovereign 0.5.22 against Hiddify 4.1.1, v2rayN 7.25.6, Happ 4.5.4 and INCY 3.8.10, all in TUN mode on the same VLESS + REALITY server.

| | Sovereign | Hiddify | v2rayN | Happ |
|---|---|---|---|---|
| Memory while connected, MB | **186** | 240 | 274 | 231 |
| Idle CPU, % of one core | **0.73** | 0.86 | 1.51 | 1.09 |
| Core killed: requests past the VPN | **0 of 27** | 50 of 50 | 77 of 77 | 76 of 76 |
| Core killed: came back by itself | **yes, in 7 s** | no | no | no |
| Server gone: requests past the VPN | 0 | 0 | 0 | 0 |

The killed core is the difference that matters: Sovereign's kill switch keeps traffic shut until the service brings the core back, while the others send everything straight from the home address within two seconds and stay in the tray as if connected. Speed and latency from a single run on a noisy line are not compared; INCY didn't open sites in TUN mode (a `fakedns` pool in class E addresses), see the bench notes.

## Building

Needs Windows 10/11 x64, and:

- Visual Studio 2022 Build Tools (C++ workload; the Clang tools component for clang-tidy), CMake ≥ 3.28, Ninja;
- vcpkg at `C:\vcpkg`;
- Go (see `gocore/go.mod`) and a MinGW-w64 `gcc` on `PATH` — cgo needs it for the DLL;
- the pinned sources: `git clone --depth 1 --branch v1.14.4 https://github.com/SagerNet/sing-box vendor/sing-box` (tag from `tools/codegen/sing-box.version`), and `vendor/wintun/amd64/wintun.dll` from the zip pinned in `tools/codegen/wintun.version`.

From a Developer Command Prompt:

```
cmake --preset ci
cmake --build --preset ci
ctest --test-dir build/ci --output-on-failure
```

Presets: `ci` (Debug), `ci-asan`, `ci-analyze`, `ci-fuzz` (then `pwsh tools/fuzz/run.ps1`), `release`. To also run the conformance reference locally, configure with `-DSOVEREIGN_REFERENCE_SINGBOX=<path to the official sing-box.exe of the pinned version>`. The desktop test (typing in the real window) runs in CI by itself; locally set `SOVEREIGN_DESKTOP_TESTS=1`.

## Installing

Download `Sovereign-Setup-<version>.exe` from [Releases](https://github.com/sovereignbrains-usa/Sovereign/releases/latest) and run it (on 0.5.x, do it once by hand: those versions look for updates at the old address). It installs into Program Files, registers the service (starts with Windows, restarts itself after a crash), adds Sovereign to the Start menu and starts it; uninstall from Settings → Apps. The tray checks for a newer release a minute after start and every 12 hours, and updates in place when asked: it downloads the new installer, checks it against the release's `.sha256` and runs it (Windows asks for administrator rights). Your settings in `%LOCALAPPDATA%\Sovereign` survive updates; uninstalling asks whether to keep them.

Releases are built by `.github/workflows/release.yml`: push a tag `vX.Y.Z` (or run the workflow with the version), and it builds the installer (`installer/build.ps1`: Release, the CRT linked in, Inno Setup), installs, updates and uninstalls it on the runner (`installer/smoke.ps1`) and publishes it with its checksum.

## Running from a build

```
sovereign-core.exe --install     (admin) register and start the service and its ETW recorder
sovereign-core.exe --uninstall   (admin) remove both, the kill switch and the boot config
sovereign-core.exe --unblock     (admin) lift the kill switch by hand, if the service can't
sovereign-core.exe --run         run in a console instead, for debugging
sovereign-tray.exe               the tray and its window; copy a subscription link and use "+"
```

Settings and the configurations live in `%LOCALAPPDATA%\Sovereign`; the service's logs, cache, rule sets and boot config in `%ProgramData%\Sovereign`. `tools/trace/sovtrace.ps1 dump` decodes the flight recorder.

## License

MIT — see [LICENSE](LICENSE).
