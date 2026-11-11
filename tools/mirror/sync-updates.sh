#!/bin/sh
# Pull the latest signed Sovereign release onto this mirror.
# A failure leaves whatever is already published where it is.
set -eu

ROOT=/var/www/sovereign-updates
SRC=https://github.com/sovereignbrains-usa/Sovereign/releases/latest/download/update.json
KEEP=3

mkdir -p "$ROOT/.incoming"
tmp="$ROOT/.incoming/update.json"
inst="$ROOT/.incoming/installer"
cleanup() {
  # Under set -e dash exits with the trap's last status: a false test here would
  # turn a published release into a failed run.
  if [ -n "${tmp:-}" ]; then rm -f "$tmp"; fi
  if [ -n "${inst:-}" ]; then rm -f "$inst"; fi
}
trap cleanup EXIT

if ! curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 20 -o "$tmp" "$SRC"; then
  echo "sovereign-updates: update.json not fetched" >&2
  exit 1
fi

if [ -f "$ROOT/update.json" ] && cmp -s "$tmp" "$ROOT/update.json"; then
  exit 0
fi

desc=$(jq -er '.signed' "$tmp")
version=$(printf '%s' "$desc" | jq -er '.version')
sha=$(printf '%s' "$desc" | jq -er '.sha256')
size=$(printf '%s' "$desc" | jq -er '.size')

if ! printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "sovereign-updates: bad version" >&2
  exit 1
fi
if ! printf '%s\n' "$sha" | grep -Eq '^[0-9a-f]{64}$'; then
  echo "sovereign-updates: bad sha256" >&2
  exit 1
fi
if ! printf '%s\n' "$size" | grep -Eq '^[0-9]+$'; then
  echo "sovereign-updates: bad size" >&2
  exit 1
fi

name="Sovereign-Setup-${version}.exe"
url="https://github.com/sovereignbrains-usa/Sovereign/releases/download/v${version}/${name}"
if ! curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 20 -o "$inst" "$url"; then
  echo "sovereign-updates: installer not fetched" >&2
  exit 1
fi

actual=$(sha256sum "$inst" | awk '{print $1}')
if [ "$actual" != "$sha" ]; then
  echo "sovereign-updates: sha256 mismatch" >&2
  exit 1
fi
got=$(wc -c < "$inst" | tr -d ' ')
if [ "$got" != "$size" ]; then
  echo "sovereign-updates: size mismatch" >&2
  exit 1
fi

# The installer first, then the description, so a client that just saw update.json
# can already fetch the file it names. Both moves stay on this filesystem.
dir="$ROOT/v${version}"
mkdir -p "$dir"
mv -f "$inst" "$dir/$name"
inst=
chmod 644 "$dir/$name"
mv -f "$tmp" "$ROOT/update.json"
tmp=
chmod 644 "$ROOT/update.json"

# The newest KEEP version directories, by the time the installer landed.
find "$ROOT" -mindepth 1 -maxdepth 1 -type d -name 'v[0-9]*' -printf '%T@ %p\n' |
  sort -nr |
  awk -v keep="$KEEP" 'NR > keep { print $2 }' |
  while IFS= read -r old; do
    case $old in
      "$ROOT"/v*) rm -rf "$old" ;;
    esac
  done
