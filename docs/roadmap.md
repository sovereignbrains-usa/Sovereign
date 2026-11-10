# Roadmap

The plan Sovereign is built to, its gates and where it stands (11.11.2026). It lived in the
issues of the project's first home (`github.com/sovereignbrains/Sovereign`, #1–#9); since the move
it lives here, in the repository.

**Goal:** a native Windows sing-box client whose quality claim is provable, not asserted — a
generated config schema kept in sync with upstream by a drift check, a differential conformance
harness with byte-level golden diffs against the reference sing-box, and clean C++23 (RAII only, no
`new`/`delete`, ASan, TSan, fuzzing, no warnings under `/W4 /WX`).

| Phase | What | State |
|---|---|---|
| [P0](#p0--toolchain-and-ci) | CMake + vcpkg, Windows service, named pipe, CI | done, 22.09.2026 |
| [P1](#p1--generated-option-structs) | Option structs generated from sing-box, golden tests | done, 22.09.2026 (G1) |
| [P2](#p2--hybrid-mvp) | sing-box as a DLL inside the service, TUN, tray | done, 02.10.2026: accepted on a clean Windows 11 |
| [P2.5](#p25--subscriptions-and-observability) | Per-UA subscription, ETW logging, system DNS left alone | done, 27.09.2026 |
| [P3](#p3--conformance-harness) | Conformance harness against the pinned reference | done, 03.10.2026 (G3) |
| [P6](#p6--three-kinds-of-users) | One engine, a layer per kind of user | in progress |
| [P4](#p4-optional--nativecore) | NativeCore: own TLS and data path, optional | not started |
| [P5](#p5--upstream-and-release) | The harness offered upstream | not started; releases go out already |

## Gates

- **G1** (after P1): the custom-type adapters closed, golden tests green. A handwritten config is not acceptable.
- **G2** (after P2): a week of stable daily use.
- **G3** (after P3): harness green — native quality proven. Not green — staying hybrid for good is fine.
- **G4** (after P4's benchmark): output identical in the harness AND (throughput +20% or latency −15%), else frozen on a research branch.
- **G5** (after P5): the harness being public is the proof; upstream's reaction gates nothing.

## Architecture decisions

- **The pipe is between the service and the tray only** (control and stats; the stats aggregated
  into one snapshot per request, not a message per connection). There is no pipe into the core.
- **`ICore` is an in-process interface** the service swaps itself: GoCore (sing-box as a c-shared
  DLL) and NativeCore (P4) are two implementations behind the same vtable, not separate processes.

**Not doing:** forking v2rayN, Hiddify, Karing or nekoray; native TLS ahead of the P3 harness;
boost::asio or libuv instead of native IOCP/RIO.

## P0 — toolchain and CI

CMake presets (MSVC, clang-cl), a vcpkg manifest, the `SovereignCore` service
(`--install`/`--uninstall`/`--run`), the named-pipe control plane (overlapped I/O, a cancelable
accept loop), a tray stub, CI with build, `/analyze`, ASan and clang-tidy. Zero warnings under `/W4 /WX`.

## P1 — generated option structs

`tools/codegen` reads sing-box's option package with Go's own type checker and writes C++ structs
with their JSON; `tools/goldengen` emits reference JSON through sing-box's own marshaler, and the
golden tests require ours to read and write it back the same. Custom wire types got handwritten
adapters (`Duration`, `FwMark`, `Listable[T]`), and every field still without one says why in
`src/generated/*.gen.h`. The uTLS fingerprint tables moved to P4, which is the only thing that
needs them. Since then the generator covers every outbound and endpoint the client writes, and
**codegen-drift** in CI fails on any difference from the pinned sources.

## P2 — hybrid MVP

sing-box as a c-shared DLL with a C ABI (`box_start`, `box_stop`, `box_stats`, the log callback),
loaded by the service behind `ICore`; TUN and DNS entirely inside sing-tun (the system's DNS
settings are never touched — DNS is hijacked in the route); the box restarted after sleep.

- gocore is built exactly like upstream's Windows release of the pinned version — its build tags,
  linker flags, `-trimpath` (without the tags there was no REALITY, no gVisor, no QUIC).
- AnyTLS + REALITY traffic proven locally and against a live server, each run ending with a
  negative check: a wrong REALITY key must carry nothing.
- A real S3 sleep with TUN and auto-route: the adapter and the default route back, HTTPS through
  AnyTLS + REALITY 1.6 s after Windows had the network again.
- **Acceptance, 02.10.2026:** a freshly installed Windows 11 with nothing on it, Sovereign 0.4.17
  from the release installer — the service, TUN, the kill switch, traffic through AnyTLS + REALITY,
  and back by itself after a real sleep.

## P2.5 — subscriptions and observability

Subscriptions fetched over WinHTTP with a sing-box User-Agent and the provider's headers
(`Profile-Update-Interval`, `Subscription-Userinfo`, `Support-Url`); ETW logging (TraceLogging
provider `Sovereign.Core`) with an always-on flight recorder; the system's DNS checked unchanged
across on/off/on — every interface's servers, the DNS client settings, NRPT, the hosts file, the DoH table.

## P3 — conformance harness

Five connections of the same client differ on the wire (randoms, GREASE, key shares, Chrome's
extension order, ECH GREASE), so a raw byte diff can't be green even reference against reference.
The harness (`tests/conformance`, `src/conformance`) parses the ClientHello, masks exactly those
fields — each with its reason in the source — sorts the extensions, and compares every other byte.
It first proves that the pinned official release (sha256-pinned) agrees with itself after masking,
a mutation test checks that every byte left is compared, and only then compares Sovereign's core,
driven through the client's real path: the importer, the effective config, the service's
`ControlHandler`, the shipped DLL.

Cases in CI: `anytls-reality-chrome`, `anytls-tls-chrome`, `vless-reality-firefox` (Firefox on
purpose: an importer that dropped `fp` would still pass a Chrome case). The reference self-test
caught a mask missing at first: Firefox's ECH GREASE picks its AEAD per connection.

Also P3's: the fuzz job (libFuzzer, on every push and nightly, the corpus kept in the repository)
and the tsan job (the control plane and the tray's logic raced on Linux under ThreadSanitizer).
Left for later: Hysteria 2 and TUIC in the harness (QUIC needs a UDP listener).

## P6 — three kinds of users

Agreed 06.10.2026: one engine, three layers; each person sees only theirs, and every screen has one
main action with the rest on demand.

1. **Subscribers** — paste a subscription, switch on, forget it. *Done:* the subscription's rules
   and DNS give way to the client's own; routing by services (30 of them), each through the VPN,
   directly, through WARP or through a country.
2. **The provider's support** — *done:* a report on what concerns their service only: what's
   wrong with which server, in words support reads without asking back ("the server turns the key
   away: the subscription is likely out of date"), where each comes out, the traffic and the paid
   period. Never the subscription's link, a key or the user's address. Shown before it goes, with
   the link from `Support-Url`.
3. **Self-hosted** — *done:* the client checks a server of the user's own over ssh and installs
   [packetlab](https://github.com/sovereignbrains-usa/packetlab) on it (Hysteria 2, REALITY, AnyTLS + REALITY).
   *Not yet:* a report for an AI about the server.
4. **God Mode** — the effective config in full, debug logs, `sing-box check` in place, hidden from
   the other two. *Not started.*
5. **The interface** — the cube switch and the live traffic around it are in; the layout checked
   in CI (`tests/ui`: nothing clickable overlapping or past the edge). *Not yet:* a control budget
   per screen as a CI check.

## P4 (optional) — NativeCore

BoringSSL, a handwritten ClientHello from generated uTLS tables, a wintun + IOCP/RIO zero-copy data
path, NativeCore behind `ICore` with an A/B switch against GoCore, an iperf3 benchmark. Gated by G4.

## P5 — upstream and release

The conformance harness offered to sing-box's CI. Releases themselves go out already: a tag builds
the installer, installs, updates and uninstalls it on a runner and publishes it.

## Why the asan job runs a stub core

The real `sovereign-gocore.dll` crashed about one start in a hundred under AddressSanitizer
(26.09.2026). Caught under `cdb`: ASan hot-patches `CreateThread` process-wide, and its interceptor
always takes a stack trace with `RtlCaptureStackBackTrace`; the Go runtime calls `CreateThread`
from its own stacks, not the thread's TEB stack, so when ASLR put a Go stack below the TEB's limit
the trace's `_chkstk` probed the real stack's guard page from outside it —
`STATUS_GUARD_PAGE_VIOLATION`, not dispatchable, the process gone. Neither side is wrong on its own,
and nothing in Sovereign's C++ corrupts memory: an ASan process can't host the Go runtime reliably.

So the asan job drives `GoCore` against `gocore-stub.dll`, which exports the same C ABI with no Go
runtime (mutation-checked: freeing with the CRT instead of `box_free`, or a destructor that keeps
the log callback registered, fails it), and the real DLL's smoke tests run in the build job.
`tests/manual/gocore_start_loop.ps1` stays as the reproduction.
