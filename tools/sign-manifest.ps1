<#
.SYNOPSIS
  Signs an update manifest for the RoN-Fix loader: writes <manifest>.sig = ECDSA P-256 over SHA-256 of the
  manifest's exact bytes, 64 bytes r||s (the format BCryptVerifySignature takes).
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\sign-manifest.ps1 dist\release-7\manifest.txt
  powershell -ExecutionPolicy Bypass -File tools\sign-manifest.ps1 manifest.txt -Key backup
  powershell -ExecutionPolicy Bypass -File tools\sign-manifest.ps1 manifest.txt -KeyFile C:\path\test.key
.NOTES
  The private keys live in %USERPROFILE%\.ronfix-keys (never in a repo). Lose both and installed copies can
  only be moved to a new key by running a new Accord-Setup.exe.
#>
param([Parameter(Mandatory)][string]$Manifest, [ValidateSet('primary', 'backup')][string]$Key = 'primary', [string]$KeyFile)
$ErrorActionPreference = 'Stop'
if (-not $KeyFile) { $KeyFile = Join-Path $env:USERPROFILE ".ronfix-keys\update-$Key.key" }
$k = [System.Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($KeyFile), [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$ecdsa = New-Object System.Security.Cryptography.ECDsaCng $k
$ecdsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Manifest))
$sig = $ecdsa.SignData($bytes)
if ($sig.Length -ne 64) { throw "unexpected signature length $($sig.Length)" }
if (-not $ecdsa.VerifyData($bytes, $sig)) { throw 'signature does not verify' }
[IO.File]::WriteAllBytes("$((Resolve-Path $Manifest).Path).sig", $sig)
"signed $Manifest with the $Key key ($($bytes.Length) bytes)"
