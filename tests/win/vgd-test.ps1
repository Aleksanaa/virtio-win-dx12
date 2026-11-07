# SPDX-License-Identifier: MIT
# Test install of our driver package through the installer the build makes (setup/vgd-setup.exe), plus what tests need.
#   Install   : run the installer silently (it installs or updates the package and removes the drivers it replaces),
#               then set the UMDs' logging up (reboot after)
#   Probe     : run d3dprobe (or d3d11probe with -D3D11) with VGD_LOG set and show the log
#   Uninstall : run the installer's uninstall; the device falls back to Microsoft's basic display driver (reboot after)
#   Status    : show the adapter, its driver and its UMD slots
#   Perf      : run uibench's desktop scenarios in the logged-on user's session under a DxgKrnl and DWM ETW trace;
#               writes the build installed, uibench's report and the trace (etl and csv) to -Out, which
#               tests/perf/summarize.py reads
# Run from an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File .\vgd-test.ps1 Install
# Run it from the build tree (build\win\tests\win): it finds setup\vgd-setup.exe and tests\win\*.exe in build\win.
# Needs Secure Boot off and test signing on (vgd-setup.exe /S /testsigning turns it on). The installer logs to
# C:\ProgramData\vgd\setup.log.
#
# DWM loads vgd3d11.dll once the package is installed. If the desktop does not come back: Safe Mode (basic display),
# then Uninstall; or roll the VM back to a snapshot taken before Install.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('Install', 'Probe', 'Uninstall', 'Status', 'Perf')]
    [string]$Action,
    # Probe only: enable the D3D12 debug layer (needs Graphics Tools:
    #   Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0)
    [switch]$DebugLayer,
    # Probe: VGD_VERBOSE=1, logging every format-support and MSAA answer. Install: the same, and DXVK_LOG_LEVEL=debug,
    # for every process (DWM included) through the registry; Install without it turns that off again.
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
    [switch]$D3D11,
    # Perf only: the uibench scenarios to run (all of them by default)
    [string[]]$Scenario = @(),
    # Perf only: where the results go (default C:\vgd\perf\<date-time>)
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$Work = 'C:\vgd'
$DisplayClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'

function Find-BuildFile([string]$rel) {
    foreach ($base in @($PSScriptRoot, (Join-Path $PSScriptRoot '..\..'), (Join-Path $PSScriptRoot '..\..\..'))) {
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

# The display adapter key of the VirtIO GPU present now, whichever package drives it. A guest also booted with the
# GPU at another PCI slot (libvirt's and run-win11-gtk.sh's) has a key for each.
function Get-AdapterKey {
    $gpus = @(Get-PnpDevice -PresentOnly -Class Display -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^PCI\\VEN_1AF4&DEV_1050' })
    if ($gpus.Count -ne 1) { throw "Expected one present VirtIO GPU, found $($gpus.Count)." }
    # "{class guid}\0001": the key under the display class.
    $driver = (Get-PnpDeviceProperty -InstanceId $gpus[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
    return Get-Item (Join-Path $DisplayClass (Split-Path $driver -Leaf))
}

function Show-Status {
    $p = Get-ItemProperty (Get-AdapterKey).PSPath
    Write-Host "DriverDesc  : $($p.DriverDesc)"
    Write-Host "Provider    : $($p.ProviderName), $($p.DriverDate) $($p.DriverVersion), $($p.InfPath)"
    Write-Host 'UserModeDriverName:'
    $i = 0
    foreach ($e in $p.UserModeDriverName) { Write-Host ("  [{0}] {1}" -f $i++, $e) }
    Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "Device      : $($_.FriendlyName), status $($_.Status), problem $($_.Problem)"
    }
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null

switch ($Action) {
    'Status' {
        Show-Status
    }

    'Install' {
        Assert-Admin
        $setup = Find-BuildFile 'setup\vgd-setup.exe'
        $setupLog = Join-Path $env:ProgramData 'vgd\setup.log'
        $logged = if (Test-Path $setupLog) { (Get-Content $setupLog).Count } else { 0 }
        $proc = Start-Process -FilePath $setup -ArgumentList '/S' -Wait -PassThru
        if (Test-Path $setupLog) { Get-Content $setupLog | Select-Object -Skip $logged }
        if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) { throw "vgd-setup.exe /S failed with $($proc.ExitCode)" }
        $ver = (Get-Item $setup).VersionInfo.FileVersion

        # Processes that do not inherit the environment log too, DWM included: the UMDs read these from the registry,
        # and C:\vgd must be writable by DWM's account. DXVK writes <exe>_vgd3d11.log there. Local, not on the share:
        # DWM starts before the share is mounted and cannot create files on it.
        & icacls.exe $Work /grant '*S-1-1-0:(OI)(CI)M' | Out-Null
        $settings = 'HKLM:\SOFTWARE\vgd3d12'
        if (-not (Test-Path $settings)) { New-Item -Path $settings | Out-Null }
        Set-ItemProperty -Path $settings -Name VGD_LOG -Value (Join-Path $Work 'vgd.log')
        Set-ItemProperty -Path $settings -Name DXVK_LOG_PATH -Value $Work
        Set-ItemProperty -Path $settings -Name VGD_VERBOSE -Value $(if ($Trace) { '1' } else { '0' })
        Set-ItemProperty -Path $settings -Name DXVK_LOG_LEVEL -Value $(if ($Trace) { 'debug' } else { 'info' })
        # With -Trace, every shader container the UMDs build, for offline compiles with dxbc-spirv's tools.
        $dump = Join-Path $Work 'shaders'
        if ($Trace) {
            if (-not (Test-Path $dump)) { New-Item -ItemType Directory -Path $dump | Out-Null }
            Set-ItemProperty -Path $settings -Name VGD_SHADER_DUMP -Value $dump
        } else { Remove-ItemProperty -Path $settings -Name VGD_SHADER_DUMP -ErrorAction SilentlyContinue }

        Copy-Item (Find-BuildFile 'tests\win\d3dprobe.exe') (Join-Path $Work 'd3dprobe.exe') -Force
        Show-Status
        Write-Host ''
        Write-Host "Installed $ver. Reboot, then run:  .\vgd-test.ps1 Probe -D3D11" -ForegroundColor Green
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

    'Perf' {
        Assert-Admin
        if (-not $Out) { $Out = Join-Path $Work ('perf\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
        New-Item -ItemType Directory -Force -Path $Out | Out-Null
        $user = (Get-CimInstance Win32_ComputerSystem).UserName
        if (-not $user) { throw 'Nobody is logged on: uibench needs a user session.' }

        # What is installed: the package, the files the device runs on (from the driver store), the log settings.
        $p = Get-ItemProperty (Get-AdapterKey).PSPath
        $info = @("driver $($p.DriverVersion) $($p.InfPath)", "user $user", "time $(Get-Date -Format o)")
        $umd = $p.UserModeDriverName | Where-Object { $_ -like '*\vgd3d11.dll' } | Select-Object -First 1
        if ($umd) {
            Get-ChildItem (Split-Path $umd) -Recurse -Include *.dll, *.sys | ForEach-Object {
                $info += 'file {0} {1}' -f $_.Name, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower().Substring(0, 16)
            }
        }
        $settings = Get-ItemProperty 'HKLM:\SOFTWARE\vgd3d12' -ErrorAction SilentlyContinue
        foreach ($n in @('VGD_LOG', 'VGD_VERBOSE', 'DXVK_LOG_LEVEL')) { $info += "setting $n=$($settings.$n)" }
        $info += "logical processors $([Environment]::ProcessorCount)"
        $info | Set-Content (Join-Path $Out 'info.txt') -Encoding utf8

        $bench = Join-Path $Work 'uibench.exe'
        Copy-Item (Find-BuildFile 'tests\win\uibench.exe') $bench -Force
        $etl = Join-Path $Work 'perf.etl'
        $providers = Join-Path $Work 'perf-providers.txt'
        @('"Microsoft-Windows-DxgKrnl" 0xFFFFFFFFFFFFFFFF 5', '"Microsoft-Windows-Dwm-Core" 0xFFFFFFFFFFFFFFFF 5') |
            Set-Content $providers -Encoding ascii
        & logman.exe stop vgdperf -ets 2>&1 | Out-Null
        Remove-Item $etl -ErrorAction SilentlyContinue
        & logman.exe create trace vgdperf -pf $providers -o $etl -bs 1024 -nb 64 512 -ets | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "logman could not start the trace ($LASTEXITCODE)" }
        $report = Join-Path $Out 'uibench.txt'
        $task = 'vgduibench'
        try {
            # In the user's session: SendInput and DWM's animations need the interactive desktop.
            $argList = (@('--out', "`"$report`"") + $Scenario) -join ' '
            $taskAction = New-ScheduledTaskAction -Execute $bench -Argument $argList
            $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive
            Register-ScheduledTask -TaskName $task -Action $taskAction -Principal $principal -Force | Out-Null
            Start-ScheduledTask -TaskName $task
            Start-Sleep -Seconds 1
            for ($i = 0; $i -lt 240 -and (Get-ScheduledTask -TaskName $task).State -eq 'Running'; $i++) {
                Start-Sleep -Milliseconds 500
            }
        } finally {
            Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
            & logman.exe stop vgdperf -ets | Out-Null
        }
        Copy-Item $etl (Join-Path $Out 'perf.etl') -Force
        & tracerpt.exe $etl -o (Join-Path $Out 'perf.csv') -of CSV -y | Out-Null
        # tracerpt leaves these providers' events unnamed: their names and fields, by id and version, for the summary.
        $events = foreach ($n in @('Microsoft-Windows-DxgKrnl', 'Microsoft-Windows-Dwm-Core')) {
            foreach ($e in (Get-WinEvent -ListProvider $n).Events) {
                $fields = @()
                if ($e.Template) { $fields = @(([xml]$e.Template).template.data | ForEach-Object { $_.name }) }
                [pscustomobject]@{ provider = $n; id = $e.Id; version = $e.Version; task = $e.Task.Name
                                   opcode = $e.Opcode.Name; fields = $fields }
            }
        }
        $events | ConvertTo-Json -Depth 3 -Compress | Set-Content (Join-Path $Out 'events.json') -Encoding utf8
        if (Test-Path $report) { Get-Content $report } else { Write-Host 'uibench wrote no report' -ForegroundColor Yellow }
        Write-Host "Results in $Out" -ForegroundColor Green
    }

    'Uninstall' {
        Assert-Admin
        $proc = Start-Process -FilePath (Find-BuildFile 'setup\vgd-setup.exe') -ArgumentList '/uninstall', '/S' -Wait -PassThru
        Get-Content (Join-Path $env:ProgramData 'vgd\setup.log') -Tail 20
        if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) { throw "vgd-setup.exe /uninstall failed with $($proc.ExitCode)" }
        Show-Status
        Write-Host ''
        Write-Host 'Uninstalled. Reboot; the device then runs on the basic display driver until a package is installed.' -ForegroundColor Green
    }
}
