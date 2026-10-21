# SPDX-License-Identifier: MIT
# Test-only switch between yttrium's installed viogpu3d.sys and our build (kmd/, test-signed with yttrium's own
# certificate). The DriverStore copy is never touched: the service's ImagePath is pointed at our file instead.
#   Swap    : copy kmd\viogpu3d.sys to System32\drivers under a name unique to its hash, point ImagePath at it
#   Restore : put the saved original ImagePath back
#   Status  : show ImagePath, the file it names and the display device state
# Reboot after Swap or Restore. Run from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\kmd-swap.ps1 Swap
# Needs test signing on (bcdedit /set testsigning on), as yttrium's experimental builds already do.
# If Windows does not come back, boot to Safe Mode (the driver is not loaded there) and run Restore.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('Swap', 'Restore', 'Status')]
    [string]$Action
)

$ErrorActionPreference = 'Stop'
$Work = 'C:\vgd'
$Saved = Join-Path $Work 'viogpu3d-ImagePath.txt'
$Service = 'HKLM:\SYSTEM\CurrentControlSet\Services\viogpu3d'
$Drivers = Join-Path $env:SystemRoot 'System32\drivers'

function Assert-Admin {
    $id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an elevated (Administrator) PowerShell.'
    }
}

function Get-ImagePath { (Get-ItemProperty $Service -ErrorAction Stop).ImagePath }

function Resolve-ImagePath([string]$p) {
    $p = $p -replace '^\\SystemRoot', $env:SystemRoot
    if ($p -match '^System32') { $p = Join-Path $env:SystemRoot $p }
    return $p
}

function Find-New {
    foreach ($base in @($PSScriptRoot, (Join-Path $PSScriptRoot '..\..'))) {
        $p = Join-Path $base 'kmd\viogpu3d.sys'
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    throw "Cannot find kmd\viogpu3d.sys next to $PSScriptRoot"
}

function Hash([string]$p) { if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() } else { '(none)' } }

# Our earlier copies; the loaded one is locked and stays until the next run after a reboot.
function Remove-Stale([string]$keep) {
    Get-ChildItem $Drivers -Filter 'vgd_viogpu3d_*.sys' | Where-Object { $_.Name -ne $keep } | ForEach-Object {
        Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
    }
}

Assert-Admin
New-Item -ItemType Directory -Force $Work | Out-Null
if (-not (Test-Path $Service)) { throw 'Service viogpu3d not found: is yttrium installed?' }

switch ($Action) {
    'Swap' {
        $new = Find-New
        $current = Get-ImagePath
        if (-not (Test-Path $Saved)) {
            if ($current -match 'vgd_viogpu3d_') { throw "ImagePath already points at our build and no original is saved: $current" }
            Set-Content -Path $Saved -Value $current -Encoding ASCII
            Write-Host "Saved the original ImagePath to ${Saved}: $current"
        }
        $h = Hash $new
        $name = "vgd_viogpu3d_$($h.Substring(0, 12)).sys"
        $dest = Join-Path $Drivers $name
        if (-not (Test-Path $dest)) { Copy-Item $new $dest }
        Set-ItemProperty $Service -Name ImagePath -Value "\SystemRoot\System32\drivers\$name" -Type ExpandString
        Remove-Stale $name
        Write-Host "ImagePath -> \SystemRoot\System32\drivers\$name"
        Write-Host "sha256 $h"
        Write-Host 'Reboot to load it.'
    }
    'Restore' {
        if (-not (Test-Path $Saved)) { throw "No saved ImagePath at $Saved" }
        $orig = (Get-Content $Saved -Raw).Trim()
        Set-ItemProperty $Service -Name ImagePath -Value $orig -Type ExpandString
        Remove-Item $Saved
        Write-Host "ImagePath -> $orig"
        Write-Host 'Reboot to load it; our copies in System32\drivers are removed by the next Swap or can be deleted then.'
    }
    'Status' {
        $ip = Get-ImagePath
        Write-Host "ImagePath: $ip"
        Write-Host "  sha256 $(Hash (Resolve-ImagePath $ip))"
        if (Test-Path $Saved) { Write-Host "saved original: $((Get-Content $Saved -Raw).Trim())" }
        try { $n = Find-New; Write-Host "new: $n"; Write-Host "  sha256 $(Hash $n)" } catch { Write-Host 'new: (not found)' }
        Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "device: $($_.FriendlyName) status $($_.Status) problem $($_.Problem)"
        }
    }
}
