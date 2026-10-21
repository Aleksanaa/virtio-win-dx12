# SPDX-License-Identifier: MIT
# Test-only manual install of vgd3d12.dll before the signed package exists.
#   Install   : copy the DLL to System32, point the adapter's D3D12 UserModeDriverName slot at it (reboot after)
#   Probe     : run d3dprobe with VGD_LOG set and show the log
#   Uninstall : restore the saved UserModeDriverName value and remove the DLL (reboot after)
#   Status    : show the current value
# Run from an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File .\vgd-test.ps1 Install
# Expects the build tree layout next to it: umd\vgd3d12.dll, tests\win\d3dprobe.exe.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('Install', 'Probe', 'Uninstall', 'Status')]
    [string]$Action,
    # Probe only: enable the D3D12 debug layer (needs Graphics Tools:
    #   Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0)
    [switch]$DebugLayer,
    # Probe only: VGD_VERBOSE=1, logging every format-support and MSAA answer
    [switch]$Trace,
    # Probe only: also run the buffer copy test (upload -> default -> readback, fence) on each device
    [switch]$Copy,
    # Probe only: also draw off-screen and check pixels (render target, shaders, root constants, vertex buffer)
    [switch]$Triangle,
    # Probe only: also sample a texture through descriptor heaps, tables, a heap and a static sampler
    [switch]$Textured,
    # Probe only: also draw with depth/stencil tests and 4x MSAA with a resolve
    [switch]$Depth,
    # Probe only: also clear UAVs and dispatch a compute shader on a compute queue
    [switch]$Compute,
    # Probe only: also run occlusion, pipeline statistics and timestamp queries
    [switch]$Queries,
    # Probe only: also run ExecuteIndirect draws and dispatches
    [switch]$Indirect
)

$ErrorActionPreference = 'Stop'
$Work = 'C:\vgd'
$SysDll = Join-Path $env:SystemRoot 'System32\vgd3d12.dll'
$Saved = Join-Path $Work 'original-UserModeDriverName.txt'
$DisplayClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'

function Find-BuildFile([string]$rel) {
    foreach ($base in @($PSScriptRoot, (Join-Path $PSScriptRoot '..\..'))) {
        $p = Join-Path $base $rel
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    throw "Cannot find $rel next to $PSScriptRoot"
}

function Assert-Admin {
    $id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an elevated (Administrator) PowerShell.'
    }
}

# The display adapter key written by yttrium's viogpu3d.inf.
function Get-AdapterKey {
    $keys = @(Get-ChildItem $DisplayClass -ErrorAction SilentlyContinue | Where-Object {
        $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        $p -and ($p.UserModeDriverName -match 'viogpu_d3d10')
    })
    if ($keys.Count -ne 1) { throw "Expected one viogpu adapter key, found $($keys.Count)." }
    return $keys[0]
}

function Show-Status($key) {
    $p = Get-ItemProperty $key.PSPath
    Write-Host "Adapter key : $($key.Name)"
    Write-Host "DriverDesc  : $($p.DriverDesc)"
    Write-Host 'UserModeDriverName:'
    $i = 0
    foreach ($e in $p.UserModeDriverName) { Write-Host ("  [{0}] {1}" -f $i++, $e) }
    Write-Host "System32 DLL present: $(Test-Path $SysDll)"
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null

switch ($Action) {
    'Status' {
        Show-Status (Get-AdapterKey)
    }

    'Install' {
        Assert-Admin
        $dll = Find-BuildFile 'umd\vgd3d12.dll'
        $probe = Find-BuildFile 'tests\win\d3dprobe.exe'
        $key = Get-AdapterKey
        $current = @((Get-ItemProperty $key.PSPath).UserModeDriverName)

        # Save the original value once; repeated installs keep the first (yttrium's) value.
        if (-not (Test-Path $Saved)) {
            $original = @($current | Where-Object { $_ -notmatch 'vgd3d12' })
            Set-Content -Path $Saved -Value $original -Encoding ASCII
            & reg.exe export ($key.Name) (Join-Path $Work 'adapter-key-backup.reg') /y | Out-Null
        }
        $original = @(Get-Content $Saved)

        Copy-Item $dll $SysDll -Force
        Copy-Item $probe (Join-Path $Work 'd3dprobe.exe') -Force

        # Slots: D3D9, D3D10, D3D11 stay yttrium's; the 4th (D3D12) is ours.
        $slots = @($original[0..2]) + $SysDll
        Set-ItemProperty -Path $key.PSPath -Name UserModeDriverName -Type MultiString -Value $slots
        Show-Status $key
        Write-Host ''
        Write-Host 'Installed. Reboot, then run:  .\vgd-test.ps1 Probe' -ForegroundColor Green
    }

    'Probe' {
        $log = Join-Path $Work 'vgd.log'
        Remove-Item $log -ErrorAction SilentlyContinue
        # Prefer the probe next to this script (the fresh build); C:\vgd holds the copy Install made.
        $exe = $null
        try { $exe = Find-BuildFile 'tests\win\d3dprobe.exe' } catch { $exe = Join-Path $Work 'd3dprobe.exe' }
        $env:VGD_LOG = $log
        $env:VGD_VERBOSE = if ($Trace) { '1' } else { '' }
        Write-Host "== d3dprobe ($exe)"
        $probeArgs = @()
        if ($DebugLayer) { $probeArgs += '--debug' }
        if ($Copy) { $probeArgs += '--copy' }
        if ($Triangle) { $probeArgs += '--triangle' }
        if ($Textured) { $probeArgs += '--textured' }
        if ($Depth) { $probeArgs += '--depth' }
        if ($Compute) { $probeArgs += '--compute' }
        if ($Queries) { $probeArgs += '--queries' }
        if ($Indirect) { $probeArgs += '--indirect' }
        & $exe @probeArgs
        Write-Host "== exit code $LASTEXITCODE"
        Write-Host "== $log"
        if (Test-Path $log) { Get-Content $log } else { Write-Host '(no log: vgd3d12.dll was not loaded)' }
    }

    'Uninstall' {
        Assert-Admin
        $key = Get-AdapterKey
        if (Test-Path $Saved) {
            $original = @(Get-Content $Saved)
            Set-ItemProperty -Path $key.PSPath -Name UserModeDriverName -Type MultiString -Value $original
            Remove-Item $Saved
        } else {
            Write-Host 'No saved value; leaving UserModeDriverName as is.' -ForegroundColor Yellow
        }
        # The DLL may be loaded by a running process; then it goes after the reboot.
        try { Remove-Item $SysDll -ErrorAction Stop }
        catch { if (Test-Path $SysDll) { Write-Host "Could not delete $SysDll now (in use); rerun Uninstall after reboot." -ForegroundColor Yellow } }
        Show-Status $key
        Write-Host ''
        Write-Host 'Uninstalled. Reboot to make the adapter use the original value.' -ForegroundColor Green
    }
}
