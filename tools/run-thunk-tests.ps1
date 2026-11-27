# Builds and runs the 32-bit test DLLs in 32-bit PowerShell:
#   proxy/tests/thunk_test.dll   netproxy.c (fixes core): thunks, lobby logging, telemetry
#   proxy/tests/update_test.dll  update.c: update targets, manifest, signatures, and an update end to end
#                                against a local HTTP server (python http.server on 127.0.0.1, started here)
#   powershell -ExecutionPolicy Bypass -File tools\run-thunk-tests.ps1
# Exit code = number of failed tests.
param([switch]$Inner, [string]$Dll, [string]$Fn = 'run_thunk_tests')
$ErrorActionPreference = 'Stop'
if ($Inner) {
	# runs inside 32-bit PowerShell
	Add-Type -Namespace T -Name D -MemberDefinition ('[DllImport(@"' + $Dll + '")] public static extern int ' + $Fn + '(System.Text.StringBuilder b, int n);')
	$sb = New-Object System.Text.StringBuilder 16384
	$f = [T.D]::$Fn($sb, 16384)
	$sb.ToString()
	exit $f
}
$root = Split-Path -Parent $PSScriptRoot
$tests = Join-Path $root 'proxy\tests'
$llvm = if ($env:LLVM_MINGW) { $env:LLVM_MINGW } else { Join-Path $env:USERPROFILE 'Tools\llvm-mingw' }  # CI sets LLVM_MINGW
$clang = Join-Path $llvm 'bin\i686-w64-mingw32-clang.exe'
$ps32 = "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
foreach ($t in 'thunk_test', 'update_test') {
	$src = if ($t -eq 'update_test') { 'update_tests.c' } else { 'thunk_test.c' }
	& $clang -O2 -Wall -shared -nostdlib '-Wl,--entry,_DllMain@12' '-Wl,--kill-at' -o "$tests\$t.dll" "$tests\$src" -lkernel32 -luser32 -lbcrypt
	if ($LASTEXITCODE) { throw "build of $t failed" }
}

$failed = 0
& $ps32 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Inner -Dll "$tests\thunk_test.dll" -Fn run_thunk_tests
$failed += $LASTEXITCODE

# updater: scratch feed + game folder, local web server
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("ronfix-update-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$serve = Join-Path $scratch 'feed'; $game = Join-Path $scratch 'game'
New-Item -ItemType Directory -Force $serve, $game | Out-Null
$port = Get-Random -Minimum 18000 -Maximum 19000
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
$srv = $null
if ($py) {
	$srv = Start-Process $py -ArgumentList '-I', '-m', 'http.server', $port, '--bind', '127.0.0.1', '--directory', "`"$serve`"" -WindowStyle Hidden -PassThru
	for ($i = 0; $i -lt 40; $i++) {
		try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $port); $c.Close(); break } catch { Start-Sleep -Milliseconds 150 }
	}
	$env:RONFIX_TEST_FEED = "http://127.0.0.1:$port/"
	$env:RONFIX_TEST_SERVE = $serve
	$env:RONFIX_TEST_GAME = $game
} else { 'python not found: the update test will fail' }
# log upload: the real log service code (logserver/src) with an in-memory database
$node = (Get-Command node -ErrorAction SilentlyContinue).Source
$logsrv = $null
if ($node) {
	$lport = $port + 1
	$dump = Join-Path $scratch 'logdump.json'
	$logsrv = Start-Process $node -ArgumentList "`"$root\logserver\test\serve.mjs`"", $lport, "`"$dump`"" -WindowStyle Hidden -PassThru
	for ($i = 0; $i -lt 40; $i++) {
		try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $lport); $c.Close(); break } catch { Start-Sleep -Milliseconds 150 }
	}
	$env:RONFIX_TEST_LOGS = "http://127.0.0.1:$lport/v1/upload"
	$env:RONFIX_TEST_LOGDUMP = $dump
} else { 'node not found: the log upload test will fail' }
try {
	& $ps32 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Inner -Dll "$tests\update_test.dll" -Fn run_update_tests
	$failed += $LASTEXITCODE
	if ($LASTEXITCODE -and (Test-Path "$game\ronfix-update.log")) { '--- ronfix-update.log'; Get-Content "$game\ronfix-update.log" }
} finally {
	if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
	if ($logsrv) { Stop-Process -Id $logsrv.Id -Force -ErrorAction SilentlyContinue }
	Remove-Item Env:RONFIX_TEST_FEED, Env:RONFIX_TEST_SERVE, Env:RONFIX_TEST_GAME, Env:RONFIX_TEST_LOGS, Env:RONFIX_TEST_LOGDUMP -ErrorAction SilentlyContinue
	Start-Sleep -Milliseconds 300
	Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"TOTAL failed: $failed"
exit $failed
