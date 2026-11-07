# Install zevic from an unpacked release archive (Windows, PowerShell).
#   .\install.ps1 [-Prefix DIR]     default DIR: $env:LOCALAPPDATA\Zevic
#   .\install.ps1 -Uninstall
# Copies the release to DIR and adds DIR\bin to the user PATH.
param([string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Zevic'), [switch]$Uninstall)
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
