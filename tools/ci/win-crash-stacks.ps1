# CI helper (Windows): after a failed ctest run, runs every failed test under the console
# debugger (cdb from the Windows SDK) and prints the stack where it crashes (access violation,
# second-chance C++ exception, fast fail).
#   pwsh tools/ci/win-crash-stacks.ps1 <build dir>
param([string]$Build = "build")
$failed = Join-Path $Build "Testing/Temporary/LastTestsFailed.log"
if (-not (Test-Path $failed)) { Write-Output "no failed tests"; exit 0 }
$cdb = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $cdb) { Write-Output "cdb not found"; exit 0 }
$env:_NT_SYMBOL_PATH = (Resolve-Path $Build).Path
foreach ($line in Get-Content $failed) {
  $name = ($line -split ":", 2)[1]
  $info = & ctest --test-dir $Build -N -V -R "^$name$" 2>&1 | Out-String
  if ($info -notmatch 'Test command: ([^\r\n]+)') { continue }
  $cmd = $Matches[1].Trim()
  $dir = if ($info -match 'Working Directory: ([^\r\n]+)') { $Matches[1].Trim() } else { $Build }
  $parts = @([regex]::Matches($cmd, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
  Write-Output "=== $name"
  Push-Location $dir
  $out = & $cdb.FullName -g -G -lines -c ".lastevent; .ecxr; kpn 40; q" @parts 2>&1 | Out-String
  Pop-Location
  $keep = $out -split "`n" | Where-Object { $_ -match 'Last event|exception|Exception|^\s*[0-9a-f]{2} [0-9a-f`]{8,}' } | Select-Object -First 50
  $keep | ForEach-Object { Write-Output $_.TrimEnd() }
}
exit 0
