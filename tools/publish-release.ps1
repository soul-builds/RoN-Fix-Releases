<#
.SYNOPSIS
  Publishes dist\release-<n> (tools\build-setup.ps1) as a GitHub release on soul-builds/RoN-Fix,
  always as the soul-builds account. The loader's updater reads releases/latest/download/manifest.txt, so the
  newest release reaches every installed copy at its next game start.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\publish-release.ps1 -Release 7 -Name 0.7 -Notes notes.md
#>
param([Parameter(Mandatory)][int]$Release, [Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Notes)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "dist\release-$Release"
$repo = 'soul-builds/RoN-Fix'
if (-not (Test-Path "$dir\manifest.txt") -or -not (Test-Path "$dir\manifest.txt.sig")) { throw "no signed manifest in $dir" }
$m = Get-Content "$dir\manifest.txt"
if ($m[1] -ne "version $Release" -or $m[2] -ne "name $Name") { throw "manifest is for '$($m[1]) / $($m[2])', not release $Release $Name" }
$assets = @("$dir\manifest.txt", "$dir\manifest.txt.sig")
foreach ($l in $m | Where-Object { $_ -like 'file *' }) {
	$w = $l -split ' '
	$f = Join-Path $dir $w[2]
	if (-not (Test-Path $f)) { throw "missing asset $($w[2])" }
	if ((Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant() -ne $w[3]) { throw "$($w[2]) does not match the manifest" }
	if ($assets -notcontains $f) { $assets += $f }
}
$env:GH_TOKEN = (& gh auth token --user soul-builds)
if (-not $env:GH_TOKEN) { throw 'no gh login for soul-builds' }
try {
	$who = (& gh api user --jq .login)
	if ($who -ne 'soul-builds') { throw "gh token belongs to '$who', not soul-builds" }
	& gh release create "v$Name" -R $repo --title "RoN-Fix $Name" --notes-file $Notes --latest @assets
	if ($LASTEXITCODE) { throw 'gh release create failed' }
	# (no spaces or quotes in --jq: Windows PowerShell 5.1 mangles them for native programs)
	& gh release view "v$Name" -R $repo --json url --jq .url
	& gh release view "v$Name" -R $repo --json assets --jq '.assets[].name'
} finally { Remove-Item Env:GH_TOKEN -ErrorAction SilentlyContinue }
