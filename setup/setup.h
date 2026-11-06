// SPDX-License-Identifier: MIT
// vgd-setup.exe: installs, updates and uninstalls the driver package (pkg/vgd.inf), which the build packs into the
// executable as a cabinet (setup/payload.cmake). See main.cpp for the flow.
#pragma once

#include <windows.h>

#include <shlobj.h>

#include <functional>
#include <string>
#include <vector>

// The VirtIO GPU as pkg/vgd.inf names it, and the part of it any virtio-gpu driver package names.
inline constexpr wchar_t kHardwareId[] = L"PCI\\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01";
inline constexpr wchar_t kDeviceIdPrefix[] = L"VEN_1AF4&DEV_1050";
// pkg/vgd.inf's provider, which marks our packages in the driver store.
inline constexpr wchar_t kProvider[] = L"vgd project";
// The certificate that signs the package's catalog, created on each computer (install.cpp).
inline constexpr wchar_t kCertSubject[] = L"vgd test signing";
inline constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\vgd";
inline constexpr wchar_t kProductName[] = L"vgd";
inline constexpr wchar_t kProjectUrl[] = L"https://github.com/Aleksanaa/virtio-win-dx12";

// An error to show the user: what failed, with the system's message for the error code.
struct Failure {
    std::wstring message;
};
[[noreturn]] void fail(const std::wstring& what, DWORD error);
[[noreturn]] void fail(const std::wstring& what);
std::wstring error_text(DWORD error);

// Appends to %ProgramData%\vgd\setup.log (and stdout when it is redirected, as under the guest agent).
void log(const std::wstring& line);
std::wstring log_path();

std::wstring format(const wchar_t* fmt, ...);
std::wstring known_folder(const KNOWNFOLDERID& id);
std::wstring system_dir();
std::wstring self_path();
// Runs a program from System32 hidden, waits for it, and returns its exit code; its output goes to the log.
DWORD run(const std::wstring& exe, const std::wstring& args);
// Compares dotted versions ("0.1.0.85") numerically.
int compare_versions(const std::wstring& a, const std::wstring& b);

// A third-party driver package in the driver store (%windir%\INF\oem*.inf).
struct DriverPackage {
    std::wstring inf;  // oemN.inf
    std::wstring provider, version, cls;
    bool ours() const;
};

// What the installer needs to know about the system before it changes anything (system.cpp).
struct SystemState {
    DWORD build = 0;
    bool x64 = false;
    // The VirtIO GPU (present devices with kHardwareId) and the driver it runs on now.
    int devices = 0;
    std::wstring device_driver, device_provider, device_version, device_inf;
    bool uefi = false, secure_boot = false, test_signing = false, hvci = false;
    DWORD smart_app_control = 0;  // 0 off, 1 on, 2 evaluation
    bool vulkan_runtime = false;
    // This product as Apps & features lists it; empty when it is not registered.
    std::wstring installed_version;
    // Display and extension packages for the VirtIO GPU in the driver store: ours (earlier versions) and others
    // (Red Hat's virtio-win viogpudo, yttrium's), which the install removes once ours drives the device.
    std::vector<DriverPackage> packages;
    bool cert_exists = false;

    std::wstring summary() const;
};
SystemState inspect();
std::vector<DriverPackage> gpu_packages();

// Reports a step: overall percentage and what is being done.
using Progress = std::function<void(int percent, const std::wstring& step)>;

// install.cpp. Both return whether Windows needs a restart; both throw Failure.
bool install(const SystemState& state, const std::wstring& version, const Progress& progress);
bool uninstall(const Progress& progress);
// Turns test signing on for the next boot (after suspending BitLocker for that one restart); throws Failure.
void enable_test_signing();
