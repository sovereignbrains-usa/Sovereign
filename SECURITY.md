# Security

Sovereign runs a Windows service as SYSTEM, holds the keys to your servers and decides what leaves
the machine, so a security problem in it matters more than a bug. Please report one privately:
**[Report a vulnerability](https://github.com/sovereignbrains-usa/Sovereign/security/advisories/new)**
(GitHub's private reporting), not in a public issue.

What counts, for example:

- traffic leaving past the tunnel while the kill switch should hold it, or the closed local network
  letting something through;
- anything a local user without admin rights can make the service do through its pipe
  (`\\.\pipe\sovereign-control`) beyond what the tray asks of it;
- keys, passwords or subscription links where they shouldn't be: logs, reports, the ETW recording,
  files readable by other users;
- the updater taking an installer that isn't the release's (it checks the release's `.sha256`);
- a subscription, a config or a share link that crashes the tray or the service, or makes them run
  something.

Only the latest release is fixed; the tray updates itself to it. Please say which version you saw it
on (settings, «О Sovereign») and how to reproduce it, and give a reasonable time for a fix before
publishing.
