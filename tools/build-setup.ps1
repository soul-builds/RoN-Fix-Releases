<#
.SYNOPSIS
  Builds an Accord release (the project and its files are still named RoN-Fix / ronfix) for the one-click setup and the updater:
    proxy\build\CrossplayNetLib.dll   loader (loader.c)
    proxy\build\ronfix-core.dll       fixes (netproxy.c)
    proxy\build\ronfix-update.dll     updater (update.c)
    dist\release-<n>\                 Accord-Setup.exe + every file the updater fetches + manifest.txt(.sig)
  Every file carries version information (product name Accord, version = -Name), as code signing requires; only the
  loader's is pinned, with the product name it was built with, RoN-Fix (see $LoaderRelease below). Since 0.15 there is no dssl.dll stand-in: the core fixes the game's own
  dssl.dll in memory (proxy/soundfix.c).
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\build-setup.ps1 -Release 7 -Name 0.7
  powershell -ExecutionPolicy Bypass -File tools\build-setup.ps1 -Release 7 -Name 0.7 -NoSign
.NOTES
  -Stage lets a signing step go in between (the GitHub release workflow does this):
    dlls      the three DLLs into proxy\build
    setup     Accord-Setup.exe around the DLLs in proxy\build (signed ones, if they were signed), and copies of the
              DLLs, into dist\release-<n>
    manifest  manifest.txt over the files in dist\release-<n>, signed with our update key unless -NoSign
    all       the three in a row (default)
  The release folder's files are what goes up as the assets of a GitHub release on soul-builds/RoN-Fix
  (tools\publish-release.ps1). The loader asks for releases/latest/download/manifest.txt, so the newest
  release on GitHub is the one players get.
#>
param([Parameter(Mandatory)][int]$Release, [Parameter(Mandatory)][ValidatePattern('^[0-9A-Za-z._-]{1,30}$')][string]$Name, [switch]$NoSign,
      [ValidateSet('all', 'dlls', 'setup', 'manifest')][string]$Stage = 'all')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$llvm = if ($env:LLVM_MINGW) { $env:LLVM_MINGW } else { Join-Path $env:USERPROFILE 'Tools\llvm-mingw' }  # CI sets LLVM_MINGW
$bin = Join-Path $llvm 'bin'
$cc = "$bin\i686-w64-mingw32-clang.exe"; $rc = "$bin\i686-w64-mingw32-windres.exe"
$proxy = Join-Path $root 'proxy'; $build = Join-Path $proxy 'build'; $setup = Join-Path $root 'setup'
$out = Join-Path $root "dist\release-$Release"
New-Item -ItemType Directory -Force $build | Out-Null

# The loader (CrossplayNetLib.dll) is the one file the game cannot start without, and Windows' Smart App Control judges
# every new build of an unsigned file afresh: a refused loader means the game does not start. So its version
# information stays pinned to the release that last changed loader.c / loader.def: the same source then builds the same
# bytes, and an update leaves the loader alone. Raise both values together with any change to loader.c or loader.def.
# Its product name and description stay as they were built in 0.14 too ("RoN-Fix", the name before Accord): changing
# them would change the file.
$LoaderRelease = 14; $LoaderName = '0.14'; $LoaderProduct = 'RoN-Fix'

function Invoke-Cc { & $cc @args; if ($LASTEXITCODE) { throw "compile failed: $args" } }
function Invoke-Rc([string]$from, [string]$to) { & $rc -O coff -o $to $from; if ($LASTEXITCODE) { throw "windres failed: $from" } }

# version information: product name and version are the same in every file of a release
function Write-VersionRc([string]$path, [string]$file, [string]$desc, [switch]$App, [int]$Rel = $Release, [string]$Nm = $Name,
                         [string]$Product = 'Accord') {
	$type = if ($App) { '0x1L' } else { '0x2L' }  # VFT_APP / VFT_DLL
	@"
1 VERSIONINFO
FILEVERSION 0,$Rel,0,0
PRODUCTVERSION 0,$Rel,0,0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE $type
FILESUBTYPE 0x0L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "$Product"
      VALUE "FileDescription", "$desc"
      VALUE "FileVersion", "$Nm"
      VALUE "InternalName", "$file"
      VALUE "LegalCopyright", "Copyright (c) 2026 Soul (soul-builds), MIT licence"
      VALUE "OriginalFilename", "$file"
      VALUE "ProductName", "$Product"
      VALUE "ProductVersion", "$Nm"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@ | Set-Content -Encoding ascii $path
}

if ($Stage -in 'all', 'dlls') {
	$dll = @('-O2', '-Wall', '-shared', '-nostdlib', '-Wl,--entry,_DllMain@12', '-Wl,--no-insert-timestamp')  # reproducible: same source, same file
	$dlls = @(
		@('CrossplayNetLib.dll', 'loader.c', 'loader.def', 'RoN-Fix loader', @('-lkernel32', '-luser32', '-lbcrypt')),
		@('ronfix-core.dll', 'netproxy.c', 'core.def', 'Accord fixes', @('-lkernel32', '-luser32', '-lbcrypt')),
		@('ronfix-update.dll', 'update.c', 'update.def', 'Accord updater', @('-lkernel32', '-luser32', '-lbcrypt')))
	foreach ($d in $dlls) {
		$base = [IO.Path]::GetFileNameWithoutExtension($d[0])
		if ($d[0] -eq 'CrossplayNetLib.dll') { Write-VersionRc "$build\$base.rc" $d[0] $d[3] -Rel $LoaderRelease -Nm $LoaderName -Product $LoaderProduct }
		else { Write-VersionRc "$build\$base.rc" $d[0] $d[3] }
		Invoke-Rc "$build\$base.rc" "$build\$base.res"
		Invoke-Cc @dll -o "$build\$($d[0])" "$proxy\$($d[1])" "$proxy\$($d[2])" "$build\$base.res" @($d[4])
	}
}

if ($Stage -in 'all', 'setup') {
	foreach ($f in 'CrossplayNetLib.dll', 'ronfix-core.dll', 'ronfix-update.dll') {
		if (-not (Test-Path "$build\$f")) { throw "$f is not built yet (-Stage dlls)" }
	}
	Set-Content -Encoding ascii "$setup\version.h" @("#define RONFIX_RELEASE $Release", "#define RONFIX_NAME L`"$Name`"")
	Write-VersionRc "$setup\verinfo.rc" 'Accord-Setup.exe' 'Accord setup' -App
	Push-Location $setup
	try { Invoke-Rc setup.rc setup.res } finally { Pop-Location }
	if (Test-Path $out) { Remove-Item -Recurse -Force $out }
	New-Item -ItemType Directory -Force $out | Out-Null
	Invoke-Cc -O2 -Wall -municode -mwindows '-Wl,--no-insert-timestamp' -o "$out\Accord-Setup.exe" "$setup\setup.c" "$setup\setup.res" -lcomctl32 -lole32 -luuid -lshell32 -limagehlp -ladvapi32 -lbcrypt -lurlmon
	foreach ($f in 'CrossplayNetLib.dll', 'ronfix-core.dll', 'ronfix-update.dll') { Copy-Item "$build\$f" "$out\$f" }
}

if ($Stage -in 'all', 'manifest') {
	# what the updater fetches: target in the game folder -> asset name in the release. The download is called
	# Accord-Setup.exe; installed, the app keeps its file name ronfix\RoN-Fix.exe (shortcuts and older updaters know it).
	$files = [ordered]@{
		'ronfix-core.dll'     = 'ronfix-core.dll'
		'ronfix-update.dll'   = 'ronfix-update.dll'
		'ronfix\RoN-Fix.exe'  = 'Accord-Setup.exe'
		'CrossplayNetLib.dll' = 'CrossplayNetLib.dll'
	}
	$lines = @('ronfix-manifest 1', "version $Release", "name $Name", 'game 6674863f bb4000')
	foreach ($t in $files.Keys) {
		$asset = "$out\$($files[$t])"
		if (-not (Test-Path $asset)) { throw "$($files[$t]) is missing in $out (-Stage setup)" }
		$h = (Get-FileHash $asset -Algorithm SHA256).Hash.ToLowerInvariant()
		$lines += "file $t $($files[$t]) $h $((Get-Item $asset).Length)"
	}
	# LF only, no BOM: the signature covers these exact bytes
	[IO.File]::WriteAllText("$out\manifest.txt", (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
	if (-not $NoSign) { & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\sign-manifest.ps1" "$out\manifest.txt"; if ($LASTEXITCODE) { throw 'signing failed' } }
	Get-ChildItem $out | ForEach-Object { "{0,-22} {1,9:N0} bytes" -f $_.Name, $_.Length }
}
