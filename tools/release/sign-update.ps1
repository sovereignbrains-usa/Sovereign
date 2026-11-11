<#
.SYNOPSIS
  Writes a release's update.json: what the release is, signed with the release key.

.DESCRIPTION
  The client takes a release only from an update.json signed by one of the keys it
  carries (src/tray/update.h, kUpdateKeys; ParseSignedRelease), whoever served the file.
  So the release can live anywhere - GitHub, our mirror (tools/mirror), a new place -
  and losing one of them, an account included, loses no client. update.json is

    {"signed":"<the description, JSON as text>","signature":"<hex: ECDSA P-256 over SHA-256, r then s>"}

  and the description

    {"version":"0.6.3","size":<bytes>,"sha256":"<hex>","page":"<the release's page>",
     "urls":[<where the installer is, tried in order>],"sources":[<where the next update.json is>]}

  The release workflow runs this with the key from the secret UPDATE_SIGNING_KEY. By hand
  (GitHub gone, a release put on the mirror alone): the key's file from the vault, through
  the environment (a JSON argument to `pwsh -File` loses its quotes), e.g.
    $env:UPDATE_SIGNING_KEY = Get-Content <vault>/sovereign-update-keys/primary.json -Raw
    pwsh tools/release/sign-update.ps1 -Version 0.7.0 -Installer out/Sovereign-Setup-0.7.0.exe -Out out/update.json
  It runs on Windows PowerShell 5.1 as well as PowerShell 7.

.PARAMETER Key
  The private key as JSON: {"curve":"P-256","x":"<hex>","y":"<hex>","d":"<hex>"}. Its public
  half must be one of kUpdateKeys in -Header, or nothing is written.

.PARAMETER Repo
  The GitHub repository: the first place of the installer and of update.json.

.PARAMETER Mirrors
  Our own mirrors (tools/mirror): <mirror>/v<version>/<installer> and <mirror>/update.json.
  The same addresses as kUpdateSources in update.h.
#>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$Installer,
  [Parameter(Mandatory = $true)][string]$Out,
  [string]$Key = $env:UPDATE_SIGNING_KEY,
  [string]$Repo = 'https://github.com/sovereignbrains-usa/Sovereign',
  [string[]]$Mirrors = @('https://updates.packetlab.tech'),
  [string]$Header = (Join-Path $PSScriptRoot '../../src/tray/update.h')
)

$ErrorActionPreference = 'Stop'

function FromHex([string]$hex, [int]$length) {
  if ($hex -cnotmatch "^[0-9a-f]{$($length * 2)}$") { throw "not $length bytes of lowercase hex" }
  $bytes = New-Object byte[] $length
  for ($i = 0; $i -lt $length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
  return , $bytes
}
function ToHex([byte[]]$bytes) { return -join ($bytes | ForEach-Object { $_.ToString('x2') }) }
function JsonList([string[]]$items) { return '[' + (($items | ForEach-Object { '"' + $_ + '"' }) -join ',') + ']' }

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "not a MAJOR.MINOR.PATCH version: '$Version'" }
if (-not $Key) { throw 'no signing key (UPDATE_SIGNING_KEY is empty)' }
$k = $Key | ConvertFrom-Json
if ($k.curve -ne 'P-256') { throw "not a P-256 key: '$($k.curve)'" }

# A key the client doesn't carry would sign releases nobody takes: refuse it here, not in the field.
$known = [regex]::Matches((Get-Content $Header -Raw), '"([0-9a-f]{128})"') | ForEach-Object { $_.Groups[1].Value }
if ("$($k.x)$($k.y)" -notin $known) { throw "the signing key is none of kUpdateKeys in $Header" }

$name = "Sovereign-Setup-$Version.exe"
$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Installer).Path)
$sha = ToHex ([Security.Cryptography.SHA256]::Create().ComputeHash($bytes))
$urls = @("$Repo/releases/download/v$Version/$name") + @($Mirrors | ForEach-Object { "$($_.TrimEnd('/'))/v$Version/$name" })
$sources = @("$Repo/releases/latest/download/update.json") + @($Mirrors | ForEach-Object { "$($_.TrimEnd('/'))/update.json" })
$page = "$Repo/releases/tag/v$Version"
foreach ($url in $urls + $sources + $page) {
  # Written into JSON by hand below: nothing that would need escaping.
  if ($url -notmatch '^https://[^"\\\s]+$') { throw "not a plain https address: '$url'" }
}
$signed = '{"version":"' + $Version + '","size":' + $bytes.Length + ',"sha256":"' + $sha + '","page":"' + $page +
  '","urls":' + (JsonList $urls) + ',"sources":' + (JsonList $sources) + '}'
$data = [Text.Encoding]::UTF8.GetBytes($signed)

$curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
$point = New-Object Security.Cryptography.ECPoint
$point.X = FromHex $k.x 32
$point.Y = FromHex $k.y 32
$private = New-Object Security.Cryptography.ECParameters
$private.Curve = $curve
$private.Q = $point
$private.D = FromHex $k.d 32
$signature = [Security.Cryptography.ECDsa]::Create($private).SignData($data, [Security.Cryptography.HashAlgorithmName]::SHA256)
if ($signature.Length -ne 64) { throw "a signature of $($signature.Length) bytes, not r and s of 32" }

# Checked as the client will: the public half alone.
$public = New-Object Security.Cryptography.ECParameters
$public.Curve = $curve
$public.Q = $point
if (-not [Security.Cryptography.ECDsa]::Create($public).VerifyData($data, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256)) {
  throw 'the signature does not verify'
}

$json = '{"signed":"' + $signed.Replace('"', '\"') + '","signature":"' + (ToHex $signature) + '"}' + "`n"
[IO.File]::WriteAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out), $json, (New-Object Text.UTF8Encoding $false))
Write-Host "update.json for $Version ($($bytes.Length) bytes, sha256 $sha) signed by key $($k.x.Substring(0, 16))..."
