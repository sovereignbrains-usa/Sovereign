<#
.SYNOPSIS
  Puts vcpkg at the commit pinned in tools/codegen/vcpkg.version and bootstraps it.

.DESCRIPTION
  The ports, and with them every dependency's version and build recipe, are the
  checked-out tree. HEAD of microsoft/vcpkg is whatever was merged last (or, on
  a runner, whatever its image happened to ship), so the build wasn't the same
  from one day to the next and a change in there reached the release unseen.
  Move the pin on purpose: put the new commit in the file, let CI build with it.

.EXAMPLE
  pwsh tools/ci/bootstrap-vcpkg.ps1
  pwsh tools/ci/bootstrap-vcpkg.ps1 -Root D:\vcpkg
#>
param([string]$Root = 'C:\vcpkg')
$ErrorActionPreference = 'Stop'

$pin = @(Get-Content (Join-Path $PSScriptRoot '../codegen/vcpkg.version'))[0].Trim()
if ($pin -notmatch '^[0-9a-f]{40}$') { throw "tools/codegen/vcpkg.version: line 1 must be a 40-digit commit, got '$pin'" }

# safe.directory: a vcpkg the runner image made belongs to another account than the job's.
function Invoke-Git {
  git -c safe.directory=* @args
  if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed ($LASTEXITCODE)" }
}

if (-not (Test-Path (Join-Path $Root '.git'))) {
  Invoke-Git init --quiet $Root
  Invoke-Git -C $Root remote add origin https://github.com/microsoft/vcpkg.git
}
# A fresh repository has no HEAD yet: that's an answer here, not an error.
$head = & { $ErrorActionPreference = 'Continue'; git -c safe.directory=* -C $Root rev-parse --verify --quiet HEAD 2>$null }
if ("$head".Trim() -ne $pin) {
  # By hash, shallow: no tag or branch to move under us, and not the whole history.
  Invoke-Git -C $Root fetch --quiet --depth 1 origin $pin
  Invoke-Git -C $Root checkout --quiet --detach --force FETCH_HEAD
}
$head = (git -c safe.directory=* -C $Root rev-parse HEAD).Trim()
if ($head -ne $pin) { throw "vcpkg is at $head, the pin is $pin" }
Write-Host "vcpkg at $pin"

& (Join-Path $Root 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE -ne 0) { throw "bootstrap-vcpkg failed ($LASTEXITCODE)" }
