<#
.SYNOPSIS
  Finishes a release that the GitHub release workflow (.github/workflows/release.yml) left as a draft:
    1. downloads the draft's files into dist\release-<n>\
    2. checks every file against the draft's manifest.txt (name, size, SHA-256) and that nothing else is attached
    3. with -Rebuild: builds the same release locally (tools\build-setup.ps1) and compares byte for byte; a
       code-signed file cannot match an unsigned local build, so for those it only reports that they are signed
    4. signs manifest.txt with the update key (tools\sign-manifest.ps1; the key never leaves this PC)
    5. adds manifest.txt.sig to the draft and publishes it as the latest release
  From then on every installed copy finds the new version at its next game start.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\finish-release.ps1 -Name 0.14
  powershell -ExecutionPolicy Bypass -File tools\finish-release.ps1 -Name 0.14 -Rebuild
#>
param([Parameter(Mandatory)][ValidatePattern('^[0-9A-Za-z._-]{1,30}$')][string]$Name, [switch]$Rebuild, [string]$Repo = 'soul-builds/RoN-Fix')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tag = "v$Name"
function Invoke-Gh { $o = & gh @args; if ($LASTEXITCODE) { throw "gh $($args -join ' ') failed" }; $o }
$owner = $Repo.Split('/')[0]
$env:GH_TOKEN = (& gh auth token --user $owner)  # always as the repository's owner account, whatever gh's default is
if (-not $env:GH_TOKEN) { throw "no gh login for $owner (gh auth login)" }
if ((Invoke-Gh api user --jq .login) -ne $owner) { throw "the gh token is not $owner's" }

$draft = Invoke-Gh release view $tag -R $Repo --json isDraft --jq .isDraft
if ($draft -ne 'true') { throw "$tag on $Repo is not a draft release (already published, or not there)" }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "ronfix-finish-$tag"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Invoke-Gh release download $tag -R $Repo -D $tmp | Out-Null

# 2. the manifest decides what belongs to the release
$m = Get-Content "$tmp\manifest.txt"
if ($m[0] -ne 'ronfix-manifest 1' -or $m[2] -ne "name $Name") { throw "manifest.txt is not for $Name" }
$release = [int]($m[1] -replace '^version ', '')
$listed = @('manifest.txt')
foreach ($l in $m | Where-Object { $_ -like 'file *' }) {
	$w = $l -split ' '
	$f = Join-Path $tmp $w[2]
	if (-not (Test-Path $f)) { throw "$($w[2]) is listed in the manifest but not attached" }
	if ((Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant() -ne $w[3] -or (Get-Item $f).Length -ne [long]$w[4]) { throw "$($w[2]) does not match the manifest" }
	$sig = (Get-AuthenticodeSignature $f).Status
	"  ok  {0,-20} {1}  code signature: {2}" -f $w[2], $w[3].Substring(0, 16), $sig
	$listed += $w[2]
}
$extra = Get-ChildItem $tmp -File | Where-Object { $listed -notcontains $_.Name }
if ($extra) { throw "attached but not in the manifest: $($extra.Name -join ', ')" }

# 3. optional: the same release built here must give the same files (code-signed files cannot be compared)
if ($Rebuild) {
	& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\build-setup.ps1" -Release $release -Name $Name -NoSign | Out-Null
	if ($LASTEXITCODE) { throw 'local build failed' }
	foreach ($f in $listed | Where-Object { $_ -ne 'manifest.txt' }) {
		$signed = (Get-AuthenticodeSignature "$tmp\$f").Status -ne 'NotSigned'
		$same = (Get-FileHash "$tmp\$f").Hash -eq (Get-FileHash "$root\dist\release-$release\$f").Hash
		"  rebuild  {0,-20} {1}" -f $f, $(if ($same) { 'identical' } elseif ($signed) { 'code-signed: not comparable' } else { throw "$f differs from the local build" })
	}
}

# 4. + 5.
$out = Join-Path $root "dist\release-$release"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$tmp\*" $out
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\sign-manifest.ps1" "$out\manifest.txt"
if ($LASTEXITCODE) { throw 'signing failed' }
Invoke-Gh release upload $tag "$out\manifest.txt.sig" -R $Repo | Out-Null
Invoke-Gh release edit $tag -R $Repo --draft=false --latest | Out-Null
Remove-Item -Recurse -Force $tmp
Invoke-Gh release view $tag -R $Repo --json url --jq .url
