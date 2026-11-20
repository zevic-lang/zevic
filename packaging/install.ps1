# Install zevic from an unpacked release archive (Windows, PowerShell).
#   .\install.ps1 [-Prefix DIR]     default DIR: $env:LOCALAPPDATA\Zevic
#   .\install.ps1 [-WithZig | -NoZig] [-ZigVersion V]
#   .\install.ps1 -ZigOnly [-Prefix DIR]   only add Zig to an existing install
#   .\install.ps1 -Uninstall
# -WithZig installs Zig (checked against ziglang.org's SHA-256) next to zevic; `zevic --target ...`
# then cross-compiles with it. Asked when interactive.
# Copies the release to DIR and adds DIR\bin to the user PATH.
param([string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Zevic'), [switch]$Uninstall, [switch]$ZigOnly, [switch]$WithZig, [switch]$NoZig, [string]$ZigVersion = '0.17.1')
$ErrorActionPreference = 'Stop'
$bin = Join-Path $Prefix 'bin'
function Install-Zig {
    $ProgressPreference = 'SilentlyContinue'   # the progress bar makes downloads many times slower
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
    # Windows' own bsdtar (Git's GNU tar cannot read C:\ paths); Expand-Archive takes ~10 minutes on Zig's files
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $zip -C $tmp
    if ($LASTEXITCODE -ne 0) { throw 'cannot unpack the Zig archive' }
    Remove-Item $zip
    $zdir = Join-Path $Prefix 'zig'
    if (Test-Path $zdir) { Remove-Item -Recurse -Force $zdir }
    Move-Item (Get-ChildItem $tmp | Select-Object -First 1).FullName $zdir
    if (-not (Test-Path (Join-Path $zdir 'lib\libc'))) { throw 'the unpacked Zig is incomplete (lib\libc is missing)' }
    Remove-Item -Recurse -Force $tmp
    Write-Host "zig $ZigVersion installed in $zdir; zevic uses it for --target"
}
if ($ZigOnly) {   # used by the Inno Setup installer's optional Zig task
    Install-Zig
    exit 0
}
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

if (-not $WithZig -and -not $NoZig -and [Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
    $ans = Read-Host 'Install Zig for cross-compiling (--target)? [y/N]'
    if ($ans -match '^(y|yes)$') { $WithZig = $true }
}
if ($WithZig) {
    try { Install-Zig } catch { Write-Host "note: zig was not installed: $_" }
}
