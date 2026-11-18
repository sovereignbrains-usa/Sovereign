<#
.SYNOPSIS
  Fetches the pinned sing-box into vendor/sing-box, and checks it is the pinned commit.

.DESCRIPTION
  tools/codegen/sing-box.version names the tag on line 1 and the commit that tag
  pointed at when it was pinned (tools/bump-singbox.ps1) on line 2. A tag can be
  moved by whoever holds the upstream repository; the commit can't. What is built
  - and what the service runs as SYSTEM - is the commit.

.EXAMPLE
  pwsh tools/ci/fetch-singbox.ps1
#>
param([string]$Dest = 'vendor/sing-box')
$ErrorActionPreference = 'Stop'

$lines = @(Get-Content (Join-Path $PSScriptRoot '../codegen/sing-box.version'))
$tag = $lines[0].Trim()
$commit = if ($lines.Count -gt 1) { $lines[1].Trim() } else { '' }
if ($tag -notmatch '^v\d+\.\d+\.\d+$') { throw "tools/codegen/sing-box.version: line 1 must be a vX.Y.Z tag, got '$tag'" }
if ($commit -notmatch '^[0-9a-f]{40}$') { throw "tools/codegen/sing-box.version: line 2 must be the tag's 40-digit commit, got '$commit'" }

git -c advice.detachedHead=false clone --quiet --depth 1 --branch $tag https://github.com/SagerNet/sing-box.git $Dest
if ($LASTEXITCODE -ne 0) { throw "git clone of sing-box $tag failed ($LASTEXITCODE)" }
$head = (git -C $Dest rev-parse HEAD).Trim()
if ($head -ne $commit) { throw "sing-box $tag is at $head, sing-box.version pins $commit" }
Write-Host "sing-box $tag at $commit"
