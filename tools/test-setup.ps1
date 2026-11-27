<#
.SYNOPSIS
  Tests RoN-Fix-Setup.exe (command-line mode) on a scratch copy of the game made from the Steam install's
  ORIGINAL files: install, install again, Steam restoring a file, a player's own settings, the 4 GB flag,
  restore byte for byte, and refusing a different game build. Exit code = failures.
  With -Dll the same install/restore code runs from setup\tests\setup_test.dll inside a 32-bit PowerShell
  (used when Windows' Smart App Control will not run the unsigned exe on this PC).
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\test-setup.ps1 -Setup dist\release-7\RoN-Fix-Setup.exe
  powershell -ExecutionPolicy Bypass -File tools\test-setup.ps1 -Setup dist\release-7\RoN-Fix-Setup.exe -Dll setup\tests\setup_test.dll
#>
param([Parameter(Mandatory)][string]$Setup, [string]$Dll)
$ErrorActionPreference = 'Stop'
$Setup = (Resolve-Path $Setup).Path
if ($Dll -and [IntPtr]::Size -ne 4) {
	# the DLL is 32-bit: run this whole script again in 32-bit PowerShell
	& "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Setup $Setup -Dll (Resolve-Path $Dll).Path
	exit $LASTEXITCODE
}
if ($Dll) {
	Add-Type -Namespace S -Name T -MemberDefinition ('[DllImport(@"' + $Dll + '", CharSet = CharSet.Unicode)] public static extern int setup_test_run(int action, string dir, string self, System.Text.StringBuilder o, int cap);' +
		'[DllImport(@"' + $Dll + '", CharSet = CharSet.Unicode)] public static extern int setup_test_bans_list(string dir, System.Text.StringBuilder o, int cap);' +
		'[DllImport(@"' + $Dll + '", CharSet = CharSet.Unicode)] public static extern int setup_test_bans_unban(string dir, int row);')
}
$steam = 'C:\Program Files (x86)\Steam\steamapps\common\Rise of Nations'
$g = Join-Path ([IO.Path]::GetTempPath()) ('ronfix-setup-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $g | Out-Null
$script:fails = 0; $script:passes = 0
function Check($ok, $what) { if ($ok) { $script:passes++; "PASS $what" } else { $script:fails++; "FAIL $what" } }
function Get-Sha($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { $null } }
function Run([string[]]$a) {
	if ($Dll) {
		$sb = New-Object System.Text.StringBuilder 2048
		$action = if ($a[0] -eq '--restore') { 2 } elseif ($a -contains '--no-updates') { 1 } elseif ($a -contains 'yes') { 3 } elseif ($a -contains 'no') { 4 } else { 0 }
		$code = [S.T]::setup_test_run($action, $a[1].Trim('"'), $Setup, $sb, 2048)
		$script:lastmsg = $sb.ToString()
		return $code
	}
	$r = Join-Path $g 'result.txt'; Remove-Item $r -ErrorAction SilentlyContinue
	$p = Start-Process $Setup -ArgumentList ($a + @('--quiet', '--result', "`"$r`"")) -Wait -PassThru
	$script:lastmsg = if (Test-Path $r) { [IO.File]::ReadAllText($r, [Text.Encoding]::Unicode) } else { '' }
	$p.ExitCode
}
Add-Type -Namespace T -Name I -MemberDefinition @'
[DllImport("imagehlp.dll", CharSet = CharSet.Unicode)] public static extern uint MapFileAndCheckSumW(string file, out uint headerSum, out uint checkSum);
'@
function Laa($p) { $b = [IO.File]::ReadAllBytes($p); $e = [BitConverter]::ToInt32($b, 0x3C); ([BitConverter]::ToUInt16($b, $e + 22) -band 0x20) -ne 0 }
function SumOk($p) { $h = 0; $c = 0; [void][T.I]::MapFileAndCheckSumW($p, [ref]$h, [ref]$c); $h -eq $c }
function ClearLaa($p) {
	# makes the scratch exe look like a fresh Steam copy (Steam's has the flag from an earlier RoN-Fix)
	$b = [IO.File]::ReadAllBytes($p); $e = [BitConverter]::ToInt32($b, 0x3C)
	$v = [BitConverter]::GetBytes([uint16]([BitConverter]::ToUInt16($b, $e + 22) -band 0xFFDF)); $b[$e + 22] = $v[0]; $b[$e + 23] = $v[1]
	[IO.File]::WriteAllBytes($p, $b); $h = 0; $c = 0; [void][T.I]::MapFileAndCheckSumW($p, [ref]$h, [ref]$c)
	$v = [BitConverter]::GetBytes([uint32]$c); for ($i = 0; $i -lt 4; $i++) { $b[$e + 88 + $i] = $v[$i] }; [IO.File]::WriteAllBytes($p, $b)
}
try {
	Copy-Item "$steam\riseofnations.exe", "$steam\patriots.exe" $g
	Copy-Item "$steam\CrossplayNetLib_orig.dll" "$g\CrossplayNetLib.dll"
	Copy-Item "$steam\dssl_orig.dll" "$g\dssl.dll"
	ClearLaa "$g\riseofnations.exe"; ClearLaa "$g\patriots.exe"
	$orig = @{}; foreach ($f in 'riseofnations.exe', 'patriots.exe', 'CrossplayNetLib.dll', 'dssl.dll') { $orig[$f] = Get-Sha "$g\$f" }
	Check (-not (Laa "$g\riseofnations.exe") -and (SumOk "$g\riseofnations.exe")) 'scratch game: fresh exe without the 4 GB flag'
	# a player's own setting from an older RoN-Fix
	Set-Content "$g\ronfix.ini" "[netfix]`r`nWaitReportSeconds=30`r`n" -Encoding ascii

	$code = Run @('--install', "`"$g`"", '--no-shortcut')
	Check ($code -eq 0) "install: exit 0 ($lastmsg)"
	$rel = Join-Path (Split-Path -Parent $PSScriptRoot) 'proxy\build'
	Check ((Get-Sha "$g\CrossplayNetLib.dll") -eq (Get-Sha "$rel\CrossplayNetLib.dll") -and (Get-Sha "$g\CrossplayNetLib_orig.dll") -eq $orig['CrossplayNetLib.dll']) 'install: loader in, original kept as CrossplayNetLib_orig.dll'
	Check ((Get-Sha "$g\dssl_orig.dll") -eq $orig['dssl.dll'] -and (Test-Path "$g\ronfix-core.dll") -and (Test-Path "$g\ronfix-update.dll")) 'install: sound stand-in, core and updater in'
	Check ((Laa "$g\riseofnations.exe") -and (Laa "$g\patriots.exe") -and (SumOk "$g\riseofnations.exe") -and (SumOk "$g\patriots.exe")) 'install: 4 GB flag on both exes, header checksums valid'
	$ini = Get-Content "$g\ronfix.ini" -Raw
	Check ($ini -match 'WaitReportSeconds=30' -and $ini -match '\[update\][^\[]*Enabled=1' -and $ini -match '\[lobby\]' -and $ini -match 'Off=0') "install: the player's setting kept, new settings added"
	Check ((Get-Content "$g\ronfix\version.txt" -Raw) -match '^\d+ \S+' -and (Get-Sha "$g\ronfix\RoN-Fix.exe") -eq (Get-Sha $Setup)) 'install: version.txt and ronfix\RoN-Fix.exe'

	Check (-not ((Get-Content "$g\ronfix.ini" -Raw) -match 'Consent=')) 'install: no answer about logs yet = nothing is sent (the window asks before the first install)'
	$code = Run @('--install', "`"$g`"", '--no-shortcut', '--logs', 'yes')
	Check ($code -eq 0 -and (Get-Content "$g\ronfix.ini" -Raw) -match '\[upload\][^\[]*Consent=1') 'logs yes: ronfix.ini [upload] Consent=1'
	$code = Run @('--install', "`"$g`"", '--no-shortcut', '--logs', 'no')
	Check ($code -eq 0 -and (Get-Content "$g\ronfix.ini" -Raw) -match '\[upload\][^\[]*Consent=0') 'logs no: Consent=0'

	$code = Run @('--install', "`"$g`"", '--no-shortcut', '--no-updates')
	Check ($code -eq 0 -and (Get-Sha "$g\CrossplayNetLib_orig.dll") -eq $orig['CrossplayNetLib.dll'] -and ((Get-Content "$g\ronfix.ini" -Raw) -match '\[update\][^\[]*Enabled=0') -and (Get-Content "$g\ronfix.ini" -Raw) -match 'Consent=0') 'install again: fine, originals untouched, updates switched off, the logs answer kept'

	# Steam "Verify integrity" puts its own CrossplayNetLib.dll back over the loader
	Copy-Item "$g\CrossplayNetLib_orig.dll" "$g\CrossplayNetLib.dll" -Force
	$code = Run @('--install', "`"$g`"", '--no-shortcut')
	Check ($code -eq 0 -and (Get-Sha "$g\CrossplayNetLib.dll") -eq (Get-Sha "$rel\CrossplayNetLib.dll") -and (Get-Sha "$g\CrossplayNetLib_orig.dll") -eq $orig['CrossplayNetLib.dll']) 'install after Steam restored a file: loader back in'

	# files that are already right are not written again: Smart App Control judges a rewritten DLL afresh, even when
	# its bytes did not change (it blocked a rewritten dssl.dll on 2026-11-27). ronfix\RoN-Fix.exe is not measured:
	# CopyFile keeps the source's time stamp, so a rewrite would not show.
	$same = 'CrossplayNetLib.dll', 'ronfix-core.dll', 'ronfix-update.dll', 'dssl.dll', 'ronfix\version.txt'
	$before = @{}; foreach ($f in $same) { $before[$f] = (Get-Item "$g\$f").LastWriteTimeUtc.Ticks }
	Start-Sleep -Milliseconds 1100
	$code = Run @('--install', "`"$g`"", '--no-shortcut')
	$touched = @($same | Where-Object { (Get-Item "$g\$_").LastWriteTimeUtc.Ticks -ne $before[$_] })
	Check ($code -eq 0 -and $touched.Count -eq 0) "install again: identical files left untouched (written again: $($touched -join ', '))"

	# 0.13 dropped better graphics: every install removes what 0.12 put in, and nothing else
	# ReShade 6.8.0's own ReShade32.dll, as 0.12 installed it (RONFIX_RESHADE32 = its path, if it is somewhere else)
	$reshade = if ($env:RONFIX_RESHADE32) { $env:RONFIX_RESHADE32 } else { Join-Path $env:USERPROFILE 'Tools\downloads\reshade-6.8.0\extracted\ReShade32.dll' }
	if (Test-Path $reshade) {
		$gfx = 'reshade-shaders\Shaders\SweetFX\SMAA.fx', 'reshade-shaders\Shaders\SweetFX\SMAA.fxh', 'reshade-shaders\Shaders\SweetFX\CAS.fx',
			'reshade-shaders\Shaders\SweetFX\Vibrance.fx', 'reshade-shaders\Shaders\SweetFX\Curves.fx', 'reshade-shaders\Shaders\SweetFX\Levels.fx',
			'reshade-shaders\Shaders\ReShade.fxh', 'reshade-shaders\Shaders\ReShadeUI.fxh', 'ronfix-graphics-subtle.ini', 'ronfix-graphics-strong.ini',
			'ReShade.ini', 'ReShade.log', 'ronfix\graphics.txt'
		function Put012 {
			New-Item -ItemType Directory -Force "$g\reshade-shaders\Shaders\SweetFX" | Out-Null
			foreach ($f in $gfx) { Set-Content "$g\$f" 'from 0.12' }
			Copy-Item $reshade "$g\dxgi.dll"
		}
		Put012
		Set-Content "$g\reshade-shaders\Shaders\Mine.fx" 'the player''s own shader'
		$code = Run @('--install', "`"$g`"", '--no-shortcut', '--graphics', 'strong')
		$left = @($gfx + 'dxgi.dll' | Where-Object { Test-Path "$g\$_" })
		Check ($code -eq 0 -and $left.Count -eq 0 -and $lastmsg -match 'Better graphics \(ReShade\) from RoN-Fix 0.12 was removed') "graphics: 0.12's ReShade removed by the install, --graphics ignored (left: $($left -join ', '))"
		Check ((Get-Content "$g\reshade-shaders\Shaders\Mine.fx") -eq 'the player''s own shader' -and -not (Test-Path "$g\reshade-shaders\Shaders\SweetFX")) "graphics: the player's own shader (and its folders) kept, empty folders gone"
		Remove-Item -Recurse -Force "$g\reshade-shaders"
		$code = Run @('--install', "`"$g`"", '--no-shortcut')
		Check ($code -eq 0 -and $lastmsg -notmatch 'Better graphics') 'graphics: nothing to remove -> no word about it'
		Put012
		Set-Content "$g\dxgi.dll" 'somebody else''s add-on'
		$code = Run @('--install', "`"$g`"", '--no-shortcut')
		Check ($code -eq 0 -and (Get-Content "$g\dxgi.dll") -eq 'somebody else''s add-on' -and -not (Test-Path "$g\ReShade.ini")) "graphics: another add-on's dxgi.dll is never deleted"
		Remove-Item "$g\dxgi.dll"
		Put012
	} else { "SKIP graphics removal tests: $reshade not found" }

	if ($Dll) {
		# the "Banned players" window: list and unban (setup/bans.c), on the file format the core writes and reads
		$bf = "$g/ronfix-bans.txt"
		function Bans { $sb = New-Object System.Text.StringBuilder 8192; $n = [S.T]::setup_test_bans_list($g, $sb, 8192); $script:banlist = $sb.ToString(); $n }
		function BanBytes([string]$t) { [Text.Encoding]::UTF8.GetBytes($t) }
		Remove-Item $bf -ErrorAction SilentlyContinue
		Check ((Bans) -eq 0 -and $banlist -eq '') 'bans: no ronfix-bans.txt = nobody banned'
		# what the core writes (entity id + Steam ID, UTF-8 name), hand edits: LF only, Steam ID alone, a second line
		# for Bob, a note, a broken id, a last line without a line end
		$L = @("# my ban list`r`n", "3545F0E8065EBCDC 76561198000000001  # Bob, banned 2026-11-20`r`n",
			"AAAABBBBCCCCDDDD  # Zo$([char]0xEB) spams ready, banned 2026-11-21`r`n", "76561198000000003`n", "# note in the middle`r`n",
			"76561198000000001  # Bob again, by hand`r`n", "765611980000000  # 15 digits: no id`r`n", "eeeeffff00001111 # Carl, banned 2026-11-22")
		[IO.File]::WriteAllBytes($bf, (BanBytes ($L -join '')))
		$n = Bans; $rows = $banlist.TrimEnd("`n") -split "`n"
		Check ($n -eq 5 -and $rows[0] -eq "Bob`t2026-11-20`t3545F0E8065EBCDC 76561198000000001" -and $rows[1] -eq "Zo$([char]0xEB) spams ready`t2026-11-21`tAAAABBBBCCCCDDDD" -and $rows[2] -eq "76561198000000003`t`t76561198000000003" -and $rows[3] -eq "Bob again, by hand`t`t76561198000000001" -and $rows[4] -eq "Carl`t2026-11-22`teeeeffff00001111") "bans: 5 listed with name and date, the broken id is no ban ($($banlist -replace "`n", ' | '))"
		$code = [S.T]::setup_test_bans_unban($g, 0)
		$want = $L[0] + $L[2] + $L[3] + $L[4] + $L[6] + $L[7]
		Check ($code -eq 0 -and [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($bf)) -ceq $want -and (Bans) -eq 3) 'bans: unban Bob removes both of his lines (same Steam ID), every other byte stays'
		$code = [S.T]::setup_test_bans_unban($g, 2)
		$want = $L[0] + $L[2] + $L[3] + $L[4] + $L[6]
		Check ($code -eq 0 -and [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($bf)) -ceq $want -and (Bans) -eq 2) 'bans: unban the last line (no line end)'
		$h = [IO.File]::Open($bf, 'Open', 'Read', 'ReadWrite')  # how the game holds it while reading (no delete sharing)
		try { $code = [S.T]::setup_test_bans_unban($g, 0) } finally { $h.Close() }
		Check ($code -eq 2 -and [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($bf)) -ceq $want -and -not (Test-Path "$bf.new")) 'bans: file held open the whole time -> "not changed" message, file untouched'
		[void][S.T]::setup_test_bans_unban($g, 0); $code = [S.T]::setup_test_bans_unban($g, 0)
		Check ($code -eq 0 -and -not (Test-Path $bf) -and (Bans) -eq 0) 'bans: last ban gone -> the file is deleted (comments and the broken id with it)'
		[IO.File]::WriteAllBytes($bf, (BanBytes (('3545F0E8065EBCDC  # ' + ('x' * 80) + "`r`n") * 900)))
		$before = Get-Sha $bf
		Check ((Bans) -eq -1 -and [S.T]::setup_test_bans_unban($g, 0) -eq 1 -and (Get-Sha $bf) -eq $before) 'bans: a file over 64 KB is left alone (edit it in Notepad)'
		Remove-Item $bf
	}

	$code = Run @('--restore', "`"$g`"")
	Check ($code -eq 0) "restore: exit 0 ($lastmsg)"
	$same = $true; foreach ($f in $orig.Keys) { if ((Get-Sha "$g\$f") -ne $orig[$f]) { $same = $false; "  differs: $f" } }
	Check $same 'restore: riseofnations.exe, patriots.exe, CrossplayNetLib.dll, dssl.dll byte for byte the originals'
	Check (-not (Test-Path "$g\CrossplayNetLib_orig.dll") -and -not (Test-Path "$g\ronfix-core.dll") -and -not (Test-Path "$g\ronfix-update.dll") -and -not (Test-Path "$g\ronfix\version.txt") -and -not (Test-Path "$g\dxgi.dll") -and -not (Test-Path "$g\reshade-shaders")) 'restore: no RoN-Fix code (and no graphics) left in the game folder'

	# another game build: refused, nothing changed
	$b = [IO.File]::ReadAllBytes("$g\riseofnations.exe"); $e = [BitConverter]::ToInt32($b, 0x3C); $b[$e + 8] = $b[$e + 8] -bxor 1
	[IO.File]::WriteAllBytes("$g\riseofnations.exe", $b)
	$before = Get-Sha "$g\CrossplayNetLib.dll"
	$code = Run @('--install', "`"$g`"", '--no-shortcut')
	Check ($code -eq 1 -and (Get-Sha "$g\CrossplayNetLib.dll") -eq $before -and -not (Test-Path "$g\CrossplayNetLib_orig.dll")) "another game build: refused, nothing changed ($lastmsg)"
} finally {
	Remove-Item -Recurse -Force $g -ErrorAction SilentlyContinue
}
"setup: $passes passed, $fails failed"
exit $fails
