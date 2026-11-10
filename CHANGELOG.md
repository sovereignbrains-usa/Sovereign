# Changelog

What changed for someone using Sovereign, release by release. The release workflow puts a version's
section into its GitHub release. Releases up to 0.5.130 are at the project's first address,
[github.com/sovereignbrains/Sovereign](https://github.com/sovereignbrains/Sovereign/releases).

## 0.6.0 — 11.11.2026

**A new home.** The project lives at [github.com/sovereignbrains-usa/Sovereign](https://github.com/sovereignbrains-usa/Sovereign)
now. Updates, the problem report and the installer's links all point here.

**If you have 0.5.x, install this one by hand once.** 0.5.130 and older look for updates at the old
address and won't find this one. From 0.6.0 on, updates come in place as before. Your settings and
configurations stay.

### Protection

- **The kill switch lets DHCP through to the local network only**, from the client's own port
  (68/546). Before, UDP to port 67/547 of any address was a way past the tunnel for any program,
  no admin rights needed. IPv6 neighbour discovery is let through under the kill switch too (before
  it worked only with the local network closed).
- **A closed local network lets DHCP in only from a server's port** (67/547). While the network is
  closed the router's DNS is shut for the core as well, so a "system" local DNS and the bootstrap
  one are encrypted instead.
- **A restarted service remembers the filters.** After a crash or an update, the service used to
  take the filters in place for the kill switch alone. The closed local network and the devices let
  into it were lost, and the next connection applied that, until the tray next sent them. Now the filters carry their settings, and the service reads
  them back.

### Routing

- **Addresses in rules are checked strictly.** A bad one (`999.1.1.1`, `10.0.0.0/33`, `1::2::3`)
  is left out instead of the whole config being refused. `1.2.3.4:443` and `[2001:db8::1]:443`
  lose the port.
- **The per-app rule comes after the blocks**: the local network, blocked programs, your sites, the
  lists, DNS over HTTPS. In "only these programs" mode, those programs go through the VPN itself.
- **The public DNS-over-HTTPS names are always closed while the block is on.** Your own DNS
  servers' names among them are resolved first through the local DNS. Before, your server
  `security.cloudflare-dns.com` opened all of `cloudflare-dns.com`.
- **A blocked program stays blocked even toward the local network**, and a program sent directly
  keeps QUIC.
- **A rule list the core can't read no longer leaves you without the VPN.** It is deleted and
  downloaded again, and left out if it fails a second time. The first download fetches the missing
  lists faster and shows how many have come in: «Списки правил скачиваются: N из M».

### Service and tray

- **The core's DLL is tried again at the start**, 5 times 2 s apart. One failed load used to mean
  no VPN, at boot or from the tray, until the service restarted.
- **One tray per computer, not per session.** A second user's tray used to turn off the first one's
  kill switch, replace its boot config and restart the box with its own config. Started in another
  session, it now says so instead.
- **Autostart follows the copy that's running.** A Run entry pointing to a moved or deleted folder
  started an old tray, or nothing, at every sign-in. Now it is rewritten.
- **The uninstaller cleans up every user signed in**: their autostart entry and its Task Manager
  mark go, not only those of the user who uninstalls. A value of the same name that belongs to
  another program stays.
