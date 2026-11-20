# Install zevic from an unpacked release archive (Windows, PowerShell).
#   .\install.ps1 [-Prefix DIR]     default DIR: $env:LOCALAPPDATA\Zevic
#   .\install.ps1 [-WithZig | -NoZig] [-ZigVersion V]
#   .\install.ps1 -Uninstall
# -WithZig installs Zig (checked against ziglang.org's SHA-256) and the zevic-cc wrapper for
# cross-compiling: $env:ZEVIC_CLANG='zevic-cc'; zevic --target linux-x64 ... Asked when interactive.
# Copies the release to DIR and adds DIR\bin to the user PATH.
param([string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Zevic'), [switch]$Uninstall, [switch]$WithZig, [switch]$NoZig, [string]$ZigVersion = '0.17.1')
$ErrorActionPreference = 'Stop'
$bin = Join-Path $Prefix 'bin'
function Get-UserPath { [Environment]::GetEnvironmentVariable('Path', 'User') }
if ($Uninstall) {
    $parts = (Get-UserPath) -split ';' | Where-Object { $_ -and ($_ -ne $bin) }
    [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
    if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
    Write-Host "zevic removed from $Prefix"
    exit 0
}
$src = $PSScriptRoot
if (-not (Test-Path (Join-Path $src 'bin\zevic.exe')) -or -not (Test-Path (Join-Path $src 'runtime\zevic_rt.c'))) {
    throw 'Run install.ps1 from the unpacked release (bin and runtime must be next to it).'
}
if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
New-Item -ItemType Directory -Force $Prefix | Out-Null
foreach ($item in 'bin', 'packages', 'runtime', 'examples', 'LICENSE', 'README.md', 'CHANGELOG.md', 'VERSION') {
    $p = Join-Path $src $item
    if (Test-Path $p) { Copy-Item -Recurse -Force $p $Prefix }
}
$userPath = Get-UserPath
if (-not (($userPath -split ';') -contains $bin)) {
    [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $bin).TrimStart(';'), 'User')
    Write-Host "Added $bin to the user PATH. Open a new terminal to use it."
}
Write-Host ("zevic " + (Get-Content (Join-Path $Prefix 'VERSION') -Raw).Trim() + " installed in $Prefix")
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
    Write-Host 'note: clang 15 or newer is required to compile Zevic programs (winget install LLVM.LLVM)'
}

function Install-Zig {
    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'aarch64' } else { 'x86_64' }
    $key = "$arch-windows"
    Write-Host "zig $ZigVersion ($key): looking up checksum"
    $index = Invoke-RestMethod 'https://ziglang.org/download/index.json'
    $entry = $index.$ZigVersion.$key
    if (-not $entry) { throw "no build of Zig $ZigVersion for $key" }
    $zip = Join-Path ([IO.Path]::GetTempPath()) "zig-$ZigVersion.zip"
    Write-Host "zig: downloading $($entry.tarball)"
    Invoke-WebRequest $entry.tarball -OutFile $zip
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $entry.shasum) { Remove-Item $zip; throw "checksum mismatch (got $got, want $($entry.shasum))" }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "zig-$ZigVersion"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    tar -xf $zip -C $tmp   # Expand-Archive takes ~10 minutes on Zig's thousands of files
    if ($LASTEXITCODE -ne 0) { throw 'cannot unpack the Zig archive' }
    Remove-Item $zip
    $zdir = Join-Path $Prefix 'zig'
    if (Test-Path $zdir) { Remove-Item -Recurse -Force $zdir }
    Move-Item (Get-ChildItem $tmp | Select-Object -First 1).FullName $zdir
    if (-not (Test-Path (Join-Path $zdir 'lib\libc'))) { throw 'the unpacked Zig is incomplete (lib\libc is missing)' }
    Remove-Item -Recurse -Force $tmp
    $ps1 = @'
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
# zig cc as a C compiler for zevic: translates LLVM triples to zig targets.
$out = @()
foreach ($a in $Rest) {
    if ($a -like '--target=*') {
        $t = $a.Substring(9); $arch = ($t -split '-')[0]
        if ($arch -eq 'arm64') { $arch = 'aarch64' }
        if ($t -match 'windows|mingw') { $zt = "$arch-windows-gnu" }
        elseif ($t -match 'darwin|apple') { $zt = "$arch-macos" }
        elseif ($t -match 'linux') { $zt = "$arch-linux-gnu" }
        else { $zt = $t }
        $out += '-target', $zt
    } else { $out += $a }
}
& (Join-Path $PSScriptRoot '..\zig\zig.exe') cc @out
exit $LASTEXITCODE
'@
    Set-Content -Path (Join-Path $bin 'zevic-cc.ps1') -Value $ps1
    Set-Content -Path (Join-Path $bin 'zevic-cc.cmd') -Value "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0zevic-cc.ps1`" %*"
    Write-Host "zig $ZigVersion installed in $zdir; set ZEVIC_CLANG=zevic-cc to use it with --target"
}
if (-not $WithZig -and -not $NoZig -and [Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
    $ans = Read-Host 'Install Zig for cross-compiling (--target)? [y/N]'
    if ($ans -match '^(y|yes)$') { $WithZig = $true }
}
if ($WithZig) {
    try { Install-Zig } catch { Write-Host "note: zig was not installed: $_" }
}
