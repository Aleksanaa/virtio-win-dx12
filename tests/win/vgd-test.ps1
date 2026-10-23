# SPDX-License-Identifier: MIT
# Test install of our driver package (pkg/vgd.inf): viogpu3d.sys, vgd3d11.dll, vgd3d12.dll, and our builds of the
# Venus Vulkan and OpenGL drivers.
#   Install   : stage the package from the build, stamp, catalog and test-sign it, install it, then remove yttrium's
#               package and what it left in System32 (reboot after)
#   Probe     : run d3dprobe (or d3d11probe with -D3D11) with VGD_LOG set and show the log
#   Uninstall : remove our package; the device falls back to Microsoft's basic display driver (reboot after)
#   Status    : show the adapter, its driver and its UMD slots
# Run from an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File .\vgd-test.ps1 Install
# Run it from the build tree (build\win\tests\win): it finds umd\vgd3d1{1,2}.dll, pkg\ and tests\win\*.exe in build\win,
# and kmd\signed\viogpu3d.sys and icd\{vgd_vulkan_virtio,viogpu_wgl}.dll in build. Needs test signing on
# (bcdedit /set testsigning on): Install signs the package with a test certificate of its own, which it creates once
# and trusts (viogpu3d.sys keeps its own signature, from yttrium's test certificate).
#
# DWM loads vgd3d11.dll once the package is installed. If the desktop does not come back: Safe Mode (basic display),
# then Uninstall; or roll the VM back to a snapshot taken before Install.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('Install', 'Probe', 'Uninstall', 'Status')]
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
    [switch]$D3D11
)

$ErrorActionPreference = 'Stop'
$Work = 'C:\vgd'
$DisplayClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$Provider = 'vgd project'   # pkg/vgd.inf's provider, which identifies our packages in the driver store
$CertSubject = 'CN=vgd test signing'

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

# The VirtIO GPU's display adapter key, whichever package drives it.
function Get-AdapterKey {
    $keys = @(Get-ChildItem $DisplayClass -ErrorAction SilentlyContinue | Where-Object {
        $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        $p -and ($p.MatchingDeviceId -match '^pci\\ven_1af4&dev_1050')
    })
    if ($keys.Count -ne 1) { throw "Expected one VirtIO GPU adapter key, found $($keys.Count)." }
    return $keys[0]
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

# Third-party packages in the driver store (oem*.inf) whose INF matches `pattern`.
function Find-Packages([string]$pattern) {
    Get-ChildItem (Join-Path $env:SystemRoot 'INF') -Filter 'oem*.inf' |
        Where-Object { Select-String -Path $_.FullName -Pattern $pattern -SimpleMatch -Quiet } |
        ForEach-Object { $_.Name }
}

# Deletes a file, or renames it to .old when a process has it loaded; the next Install deletes the .old.
function Remove-Leftover([string]$path) {
    if (Test-Path "$path.old") { Remove-Item "$path.old" -Force -ErrorAction SilentlyContinue }
    if (-not (Test-Path $path)) { return }
    try { Remove-Item $path -Force -ErrorAction Stop; Write-Host "Removed $path" }
    catch {
        Move-Item $path "$path.old" -Force
        Write-Host "$path is in use: renamed to .old, deleted by the next Install after a reboot" -ForegroundColor Yellow
    }
}

# What yttrium's package and our earlier test installs leave behind once their packages are gone.
function Remove-YttriumLeftovers {
    # yttrium's resolution service, whose binary went with its package.
    if (Get-Service vgpusrv -ErrorAction SilentlyContinue) {
        Stop-Service vgpusrv -Force -ErrorAction SilentlyContinue
        & sc.exe delete vgpusrv | Out-Null
        Write-Host 'Removed the vgpusrv service'
    }
    # yttrium's user-mode files, which its INF copied to System32 and SysWOW64 (our package has its own build of
    # viogpu_wgl.dll in the DriverStore). Its opengl32.dll is left alone: that name is Windows' own.
    $names = @('viogpu_d3d10.dll', 'yttrium12.dll', 'viogpu_wgl.dll', 'libEGL.dll', 'libGLESv1_CM.dll',
               'libGLESv2.dll', 'libvulkan_virtio.dll', 'vulkan_lvp.dll',
               'lvp_icd.x86_64.json', 'virtio_icd.x86_64.json', 'lvp_icd.x86.json', 'virtio_icd.x86.json')
    foreach ($dir in @('System32', 'SysWOW64')) {
        foreach ($n in $names) { Remove-Leftover (Join-Path $env:SystemRoot "$dir\$n") }
    }
    # Its Vulkan ICD registrations (the manifests above).
    foreach ($key in @('HKLM:\SOFTWARE\Khronos\Vulkan\Drivers', 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers')) {
        if (-not (Test-Path $key)) { continue }
        (Get-Item $key).Property | Where-Object { $_ -match '\\(lvp|virtio)_icd\.[^\\]*\.json$' } | ForEach-Object {
            Remove-ItemProperty -Path $key -Name $_
            Write-Host "Removed the Vulkan ICD registration $_"
        }
    }
    # Earlier test installs: the UMDs and ICD in System32 (vgd-test.ps1 before the package), the KMD copies and
    # saved ImagePath of kmd-swap.ps1, and the saved UserModeDriverName.
    foreach ($n in @('vgd3d11.dll', 'vgd3d12.dll', 'vgd_vulkan_virtio.dll')) {
        Remove-Leftover (Join-Path $env:SystemRoot "System32\$n")
    }
    Get-ChildItem (Join-Path $env:SystemRoot 'System32\drivers') -Filter 'vgd_viogpu3d_*.sys' | ForEach-Object {
        Remove-Leftover $_.FullName
    }
    foreach ($n in @('viogpu3d-ImagePath.txt', 'original-UserModeDriverName.txt')) {
        Remove-Item (Join-Path $Work $n) -ErrorAction SilentlyContinue
    }
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null

switch ($Action) {
    'Status' {
        Show-Status
    }

    'Install' {
        Assert-Admin
        # Stage the package: the INF and the build outputs it lists.
        $pkg = Join-Path $Work 'pkg'
        if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
        New-Item -ItemType Directory -Path $pkg | Out-Null
        $files = [ordered]@{
            'viogpu3d.sys'           = 'kmd\signed\viogpu3d.sys'
            'vgd3d11.dll'            = 'umd\vgd3d11.dll'
            'vgd3d12.dll'            = 'umd\vgd3d12.dll'
            'vgd_vulkan_virtio.dll'  = 'icd\vgd_vulkan_virtio.dll'
            'vgd_vulkan_virtio.json' = 'pkg\vgd_vulkan_virtio.json'
            'viogpu_wgl.dll'         = 'icd\viogpu_wgl.dll'
        }
        foreach ($name in $files.Keys) {
            $src = Find-BuildFile $files[$name]
            Copy-Item $src (Join-Path $pkg $name)
            Write-Host ("{0,-23} {1}  {2}" -f $name, (Get-FileHash $src -Algorithm SHA256).Hash.ToLower().Substring(0, 16), $src)
        }
        # DriverVer: today, and a version that grows with every install (yyddd.hhmm), so PnP ranks the new package
        # above the one installed before it.
        $now = Get-Date
        $ver = '1.0.{0}.{1}' -f (($now.Year % 100) * 1000 + $now.DayOfYear), ($now.Hour * 100 + $now.Minute)
        $driverVer = 'DriverVer   = {0},{1}' -f $now.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture), $ver
        $inf = Join-Path $pkg 'vgd.inf'
        (Get-Content (Find-BuildFile 'pkg\vgd.inf')) -replace '^DriverVer\s*=.*$', $driverVer | Set-Content $inf -Encoding ASCII

        # Catalog and test-sign it. Not with yttrium's certificate: it is not marked for code signing, which
        # Set-AuthenticodeSignature requires. Ours is trusted as a root and a publisher, so PnP installs without asking.
        $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $CertSubject | Select-Object -First 1
        if (-not $cert) {
            $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\LocalMachine\My `
                -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(20)
            Write-Host "Created the test certificate $CertSubject ($($cert.Thumbprint))"
        }
        foreach ($name in @('Root', 'TrustedPublisher')) {
            $store = [Security.Cryptography.X509Certificates.X509Store]::new($name, 'LocalMachine')
            $store.Open('ReadWrite')
            $store.Add([Security.Cryptography.X509Certificates.X509Certificate2]::new($cert.RawData))
            $store.Close()
        }
        $cat = Join-Path $pkg 'vgd.cat'
        New-FileCatalog -Path $pkg -CatalogFilePath $cat -CatalogVersion 2.0 | Out-Null
        $sig = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
        if ($sig.Status -ne 'Valid') { throw "Signing vgd.cat failed: $($sig.Status) $($sig.StatusMessage)" }

        & pnputil.exe /add-driver $inf /install
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil /add-driver failed with $LASTEXITCODE" }

        # The packages this one replaces: yttrium's, our D3D12 extension (pkg/vgd3d12.inf), and our earlier installs.
        $old = @(Find-Packages 'viogpu_d3d10_x64.dll') + @(Find-Packages '{e6eb91c4-82d6-49e5-bb39-81ea7ddae195}') +
               @(Find-Packages "`"$Provider`"" | Where-Object {
                   -not (Select-String -Path (Join-Path $env:SystemRoot "INF\$_") -Pattern ",$ver" -SimpleMatch -Quiet) })
        foreach ($p in $old) {
            & pnputil.exe /delete-driver $p
            if ($LASTEXITCODE -ne 0) { Write-Host "Could not delete $p (still in use?); Status shows what the device uses." -ForegroundColor Yellow }
        }
        Remove-YttriumLeftovers

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
        # OpenGL runs on zink, which needs the Vulkan loader; Windows does not ship it.
        if (-not (Test-Path (Join-Path $env:SystemRoot 'System32\vulkan-1.dll'))) {
            Write-Host 'No System32\vulkan-1.dll: OpenGL (zink) needs the Vulkan Runtime installed.' -ForegroundColor Yellow
        }
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

    'Uninstall' {
        Assert-Admin
        $ours = @(Find-Packages "`"$Provider`"")
        if ($ours.Count -eq 0) { Write-Host 'No package of ours is installed.' -ForegroundColor Yellow }
        foreach ($p in $ours) {
            & pnputil.exe /delete-driver $p /uninstall /force
            if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil /delete-driver $p failed with $LASTEXITCODE" }
        }
        Show-Status
        Write-Host ''
        Write-Host 'Uninstalled. Reboot; the device then runs on the basic display driver until a package is installed.' -ForegroundColor Green
    }
}
