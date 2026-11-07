// SPDX-License-Identifier: MIT
// venera-setup.exe: installs, updates and uninstalls the driver package (pkg/venera.inf), which the build packs into
// the executable as a cabinet (setup/payload.cmake). See main.cpp for the flow.
#pragma once

#include <windows.h>

#include <setupapi.h>
#include <shlobj.h>

#include <functional>
#include <string>
#include <vector>

// The VirtIO GPU as pkg/venera.inf names it, and the part of it any virtio-gpu driver package names.
inline constexpr wchar_t kHardwareId[] = L"PCI\\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01";
inline constexpr wchar_t kDeviceIdPrefix[] = L"VEN_1AF4&DEV_1050";
// pkg/venera.inf's provider, which marks our packages in the driver store.
inline constexpr wchar_t kProvider[] = L"Venera project";
// The certificate that signs the package's catalog, created on each computer (install.cpp).
inline constexpr wchar_t kCertSubject[] = L"Venera test signing";
inline constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Venera";
inline constexpr wchar_t kProductName[] = L"Venera";
inline constexpr wchar_t kProjectUrl[] = L"https://github.com/Aleksanaa/venera";

// An error to show the user: what failed, with the system's message for the error code.
struct Failure {
    std::wstring message;
};
[[noreturn]] void fail(const std::wstring& what, DWORD error);
[[noreturn]] void fail(const std::wstring& what);
std::wstring error_text(DWORD error);

// Appends to %ProgramData%\Venera\setup.log (and stdout when it is redirected, as under the guest agent).
void log(const std::wstring& line);
std::wstring log_path();

std::wstring format(const wchar_t* fmt, ...);
std::wstring known_folder(const KNOWNFOLDERID& id);
std::wstring system_dir();
std::wstring self_path();
// Runs a program from System32 hidden, waits for it, and returns its exit code; its output goes to the log.
DWORD run(const std::wstring& exe, const std::wstring& args);
// Compares the four numbers of dotted versions ("0.1.0.85.1a2b3c4"); the commit hash after them does not rank.
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
// Whether a device has kHardwareId among its hardware IDs.
bool is_virtio_gpu(HDEVINFO set, SP_DEVINFO_DATA* dev);

// Reports a step: overall percentage and what is being done.
using Progress = std::function<void(int percent, const std::wstring& step)>;

// install.cpp. Both return whether Windows needs a restart; both throw Failure.
bool install(const SystemState& state, const std::wstring& version, const Progress& progress);
bool uninstall(const Progress& progress);
// At the boot after an install (`/cleanup`): removes the packages the install replaced, once the device runs on ours.
void cleanup();
// Turns test signing on for the next boot (after suspending BitLocker for that one restart); throws Failure.
void enable_test_signing();

// Every line logged so far, and a function that gets each new one (the window's log view).
std::vector<std::wstring> log_history();
void set_log_sink(std::function<void(const std::wstring&)> sink);

// The installer's window (ui.cpp): a wizard showing one page at a time.
struct Page {
    std::wstring heading, subheading;
    std::wstring text;    // written to the log, which every page shows
    std::wstring status;  // above the progress bar
    std::vector<std::pair<int, std::wstring>> buttons;  // right-aligned; the first is the default
    size_t default_index = 0;  // the button Enter presses
};

class Wizard {
public:
    explicit Wizard(const std::wstring& version);
    ~Wizard();
    // Shows `page` until a button is pressed; returns its id (IDCANCEL for the window's close box).
    int ask(const Page& page);
    // Shows the progress page and runs `work` on a thread meanwhile; returns its result, or sets `error`.
    bool run(const std::wstring& heading, const std::wstring& subheading,
             const std::function<bool(const Progress&)>& work, std::wstring& error);

private:
    struct Impl;
    Impl* impl;
};
