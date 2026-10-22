# SPDX-License-Identifier: MIT
# Test-only manual install of our UMDs before the signed package exists.
#   Install   : copy vgd3d12.dll and vgd3d11.dll to System32 and point the adapter's UserModeDriverName slots at them:
#               D3D10 and D3D11 at vgd3d11.dll (DWM too), D3D12 at vgd3d12.dll, D3D9 stays yttrium's (reboot after)
#   Probe     : run d3dprobe (or d3d11probe with -D3D11) with VGD_LOG set and show the log
#   Uninstall : restore the saved UserModeDriverName value and remove the DLLs (reboot after)
#   Status    : show the current value
# Run from an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File .\vgd-test.ps1 Install
# Expects the build tree layout next to it: umd\vgd3d12.dll, umd\vgd3d11.dll, icd\vgd_vulkan_virtio.dll,
# tests\win\d3dprobe.exe, tests\win\d3d11probe.exe.
#
# DWM loads vgd3d11.dll after the reboot. If the desktop does not come back: Safe Mode (which does not load it), then
# Uninstall; or roll the VM back to a snapshot taken before Install.

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
    [switch]$Indirect,
    # Probe only: also present a flip-model swapchain in phases (resizes, 10-bit, FP16, borderless fullscreen)
    [switch]$Present,
    # Probe only: also create heaps and placed resources, map and copy through them
    [switch]$Placed,
    # Probe only: run d3d11probe (every D3D11 test; with -Present also its swapchains) instead of d3dprobe
    [switch]$D3D11
)

$ErrorActionPreference = 'Stop'
$Work = 'C:\vgd'
$SysDll = Join-Path $env:SystemRoot 'System32\vgd3d12.dll'
$SysDll11 = Join-Path $env:SystemRoot 'System32\vgd3d11.dll'
# Our build of yttrium's Venus ICD (icd/), which vgd3d12.dll loads from its own directory.
$SysIcd = Join-Path $env:SystemRoot 'System32\vgd_vulkan_virtio.dll'
$Saved = Join-Path $Work 'original-UserModeDriverName.txt'
$DisplayClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'

function Find-BuildFile([string]$rel) {
    foreach ($base in @($PSScriptRoot, (Join-Path $PSScriptRoot '..\..'))) {
        $p = Join-Path $base $rel
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    throw "Cannot find $rel next to $PSScriptRoot"
}

# Copies a DLL into System32 unless the same file is already there. A DLL some process has loaded (DWM keeps
# vgd3d11.dll and the ICD loaded) cannot be overwritten, but it can be renamed: the old file moves aside as .old,
# the processes keep using it until they restart, and the next Install deletes it.
function Install-File([string]$src, [string]$dst) {
    $old = "$dst.old"
    if (Test-Path $old) { Remove-Item $old -ErrorAction SilentlyContinue }
    if ((Test-Path $dst) -and (Get-FileHash $src).Hash -eq (Get-FileHash $dst).Hash) {
        Write-Host "$dst is up to date"
        return
    }
    try { Copy-Item $src $dst -Force -ErrorAction Stop }
    catch {
        if (Test-Path $old) {
            Write-Host "$dst and $old are in use by:" -ForegroundColor Yellow
            & tasklist.exe /m (Split-Path $dst -Leaf)
            throw "Close those processes (or reboot) and run Install again."
        }
        Move-Item $dst $old -ErrorAction Stop
        Copy-Item $src $dst -ErrorAction Stop
        Write-Host "$dst was in use: the old one is $old until processes using it restart (reboot)" -ForegroundColor Yellow
    }
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
    Write-Host "System32 DLLs present: vgd3d12 $(Test-Path $SysDll), vgd3d11 $(Test-Path $SysDll11)"
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null

switch ($Action) {
    'Status' {
        Show-Status (Get-AdapterKey)
    }

    'Install' {
        Assert-Admin
        $dll = Find-BuildFile 'umd\vgd3d12.dll'
        $dll11 = Find-BuildFile 'umd\vgd3d11.dll'
        $probe = Find-BuildFile 'tests\win\d3dprobe.exe'
        $key = Get-AdapterKey
        $current = @((Get-ItemProperty $key.PSPath).UserModeDriverName)

        # Save the original value once; repeated installs keep the first (yttrium's) value.
        if (-not (Test-Path $Saved)) {
            $original = @($current | Where-Object { $_ -notmatch 'vgd3d1[12]' })
            Set-Content -Path $Saved -Value $original -Encoding ASCII
            & reg.exe export ($key.Name) (Join-Path $Work 'adapter-key-backup.reg') /y | Out-Null
        }
        $original = @(Get-Content $Saved)

        Install-File $dll $SysDll
        Install-File $dll11 $SysDll11
        $icd = $null
        try { $icd = Find-BuildFile 'icd\vgd_vulkan_virtio.dll' }
        catch { Write-Host "No icd\vgd_vulkan_virtio.dll: vgd3d12.dll will fall back to the registered ICD (no present)." -ForegroundColor Yellow }
        if ($icd) {
            Install-File $icd $SysIcd
            Write-Host "ICD: $SysIcd sha256 $((Get-FileHash $SysIcd -Algorithm SHA256).Hash.ToLower())"
        }
        Copy-Item $probe (Join-Path $Work 'd3dprobe.exe') -Force

        # Processes that do not inherit the environment log too, DWM included: the UMDs read these from the registry,
        # and C:\vgd must be writable by DWM's account. DXVK writes <exe>_vgd3d11.log there.
        & icacls.exe $Work /grant '*S-1-1-0:(OI)(CI)M' | Out-Null
        $settings = 'HKLM:\SOFTWARE\vgd3d12'
        if (-not (Test-Path $settings)) { New-Item -Path $settings | Out-Null }
        Set-ItemProperty -Path $settings -Name VGD_LOG -Value (Join-Path $Work 'vgd.log')
        Set-ItemProperty -Path $settings -Name DXVK_LOG_PATH -Value $Work

        # Slots: D3D9 stays yttrium's; D3D10 and D3D11 are vgd3d11, D3D12 is vgd3d12.
        $slots = @($original[0], $SysDll11, $SysDll11, $SysDll)
        Set-ItemProperty -Path $key.PSPath -Name UserModeDriverName -Type MultiString -Value $slots
        Show-Status $key
        Write-Host ''
        Write-Host 'Installed. Reboot (DWM then runs on vgd3d11.dll), then run:  .\vgd-test.ps1 Probe -D3D11' -ForegroundColor Green
    }

    'Probe' {
        $log = Join-Path $Work 'vgd.log'
        Remove-Item $log -ErrorAction SilentlyContinue
        # Prefer the probe next to this script (the fresh build); C:\vgd holds the copy Install made.
        $exe = $null
        $name = if ($D3D11) { 'd3d11probe.exe' } else { 'd3dprobe.exe' }
        try { $exe = Find-BuildFile "tests\win\$name" } catch { $exe = Join-Path $Work $name }
        $env:VGD_LOG = $log
        $env:VGD_VERBOSE = if ($Trace) { '1' } else { '' }
        Write-Host "== $name ($exe)"
        $probeArgs = @()
        if ($DebugLayer) { $probeArgs += '--debug' }
        if ($Copy) { $probeArgs += '--copy' }
        if ($Triangle) { $probeArgs += '--triangle' }
        if ($Textured) { $probeArgs += '--textured' }
        if ($Depth) { $probeArgs += '--depth' }
        if ($Compute) { $probeArgs += '--compute' }
        if ($Queries) { $probeArgs += '--queries' }
        if ($Indirect) { $probeArgs += '--indirect' }
        if ($Present) { $probeArgs += '--present' }
        if ($Placed) { $probeArgs += '--placed' }
        & $exe @probeArgs
        Write-Host "== exit code $LASTEXITCODE"
        Write-Host "== $log"
        if (Test-Path $log) { Get-Content $log } else { Write-Host '(no log: our UMDs were not loaded)' }
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
        # A DLL may be loaded by a running process (vgd3d11.dll by DWM); then it goes after the reboot.
        foreach ($f in @($SysDll, $SysDll11)) {
            try { Remove-Item $f -ErrorAction Stop }
            catch { if (Test-Path $f) { Write-Host "Could not delete $f now (in use); rerun Uninstall after reboot." -ForegroundColor Yellow } }
        }
        try { if (Test-Path $SysIcd) { Remove-Item $SysIcd -ErrorAction Stop } }
        catch { Write-Host "Could not delete $SysIcd now (in use); rerun Uninstall after reboot." -ForegroundColor Yellow }
        Show-Status $key
        Write-Host ''
        Write-Host 'Uninstalled. Reboot to make the adapter use the original value.' -ForegroundColor Green
    }
}
