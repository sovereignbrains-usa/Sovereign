# updates.packetlab.tech

A pull mirror of the latest Sovereign release. The client asks it for `update.json` after GitHub (`src/tray/update.h`, `kUpdateSources`). It trusts the signature, so this host only has to serve the bytes.

The timer fetches `update.json` from GitHub's latest release. When it changes, the script reads the version, size and sha256 out of the signed description, downloads that installer, checks both, and publishes `v<version>/Sovereign-Setup-<version>.exe` and then `update.json`. A failed run leaves the files already published alone. The three newest version directories are kept.

Until a release that contains `update.json` exists, the fetch ends in 404 and the web root stays empty.

## On the server

Debian, alongside the site that is already there. This block is its own server name; certbot adds TLS to it.

```
apt-get install -y jq nginx certbot python3-certbot-nginx
install -d -m 755 /var/www/sovereign-updates
install -m 755 tools/mirror/sync-updates.sh /usr/local/sbin/sovereign-updates-sync
install -m 644 tools/mirror/sovereign-updates.service /etc/systemd/system/sovereign-updates.service
install -m 644 tools/mirror/sovereign-updates.timer /etc/systemd/system/sovereign-updates.timer
install -m 644 tools/mirror/nginx-updates.conf /etc/nginx/sites-available/sovereign-updates
ln -sf /etc/nginx/sites-available/sovereign-updates /etc/nginx/sites-enabled/sovereign-updates
nginx -t && systemctl reload nginx
certbot --nginx -d updates.packetlab.tech --non-interactive --agree-tos --register-unsafely-without-email --redirect
systemctl enable --now sovereign-updates.timer
```

DNS, before certbot: an A record for `updates.packetlab.tech` to this machine, not proxied. The first sync can wait until the release exists; `systemctl start sovereign-updates.service` runs it once.
