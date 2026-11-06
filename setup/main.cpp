// SPDX-License-Identifier: MIT
// vgd-setup.exe: the installer, a wizard window (ui.cpp) with the log in view while it works. It checks the system (Windows 11 x64, a VirtIO GPU, Secure Boot off, test signing
// on; it offers to turn test signing on), says what it will replace, and installs, updates or reinstalls the driver
// package (install.cpp). The copy it leaves in Program Files uninstalls.
//   vgd-setup.exe [/S] [/testsigning]     install or update; /S: silently; /testsigning: with /S, turn test signing
//                                         on if it is off (then restart and run it again)
//   vgd-setup.exe /uninstall [/S]
//   vgd-setup.exe /cleanup              run by a one-shot task at the boot after an install (install.cpp)
// Exit codes as MSI's: 0 done, 3010 done but restart, 1602 cancelled, 1603 failed, 1633 this system cannot take it
// (yet), 1618 another instance is running. The log is %ProgramData%\vgd\setup.log.
#include "setup.h"

#include <shellapi.h>

#include <cstdarg>
#include <cwchar>
#include <memory>
#include <mutex>

namespace {

constexpr int kDone = 0, kRestart = ERROR_SUCCESS_REBOOT_REQUIRED, kCancelled = ERROR_INSTALL_USEREXIT,
              kFailed = ERROR_INSTALL_FAILURE, kUnsupported = ERROR_INSTALL_PLATFORM_UNSUPPORTED,
              kRunning = ERROR_INSTALL_ALREADY_RUNNING;
constexpr wchar_t kTitle[] = L"vgd Setup";
constexpr int kButtonPrimary = 100, kButtonReboot = 102;

struct Options {
    bool silent = false, uninstall = false, test_signing = false, cleanup = false;
};

std::wstring version, build_id;

std::wstring resource_string(UINT id)
{
    const wchar_t* s = nullptr;
    int n = LoadStringW(nullptr, id, reinterpret_cast<LPWSTR>(&s), 0);
    return n > 0 ? std::wstring(s, n) : L"?";
}

// --- Pages

// The wizard, unless silent.
std::unique_ptr<Wizard> wizard;

void restart_windows()
{
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES p = {1};
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &p.Privileges[0].Luid);
        p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &p, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    DWORD r = InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_RESTART,
        SHTDN_REASON_MAJOR_SOFTWARE | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
    if (r != ERROR_SUCCESS)
        log(L"Restarting failed: " + error_text(r));
}

// Runs `work` on the progress page (or, silent, with the log only), then shows how it went, with the log still in
// view, and restarts if asked to. Returns the exit code.
int run_work(const std::wstring& heading, const std::function<bool(const Progress&)>& work,
             const std::wstring& done_text)
{
    std::wstring error;
    bool restart = false;
    if (wizard) {
        restart = wizard->run(heading, {}, work, error);
    } else {
        try {
            restart = work([](int, const std::wstring& step) { log(step); });
        } catch (const Failure& f) {
            error = f.message;
            log(L"Failed: " + error);
        }
    }
    if (!error.empty()) {
        if (wizard)
            wizard->ask({heading, L"Log: " + log_path(), {}, L"Failed: " + error, {{IDCANCEL, L"Close"}}});
        return kFailed;
    }
    log(done_text);
    if (wizard) {
        Page page = {done_text, L"Log: " + log_path(), {},
                     restart ? L"Completed. Takes effect after a reboot." : L"Completed.", {{IDOK, L"Done"}}};
        if (restart) {
            page.buttons.insert(page.buttons.begin(), {kButtonReboot, L"Reboot now"});
            page.default_index = 1;
        }
        if (wizard->ask(page) == kButtonReboot)
            restart_windows();
    }
    return restart ? kRestart : kDone;
}

// --- Install

constexpr wchar_t kDocs[] = L"https://github.com/Aleksanaa/virtio-win-dx12#required-configuration";

int blocked(const SystemState& s, const std::wstring& main, const std::wstring& content)
{
    log(L"Cannot install: " + main);
    if (wizard)
        wizard->ask({main, {}, content, L"Cannot install.", {{IDCANCEL, L"Close"}}});
    return kUnsupported;
}

int offer_test_signing(const Options& opts, const SystemState& s)
{
    if (wizard) {
        Page page = {L"Test signing must be on", {},
                     L"vgd's drivers are test-signed: Windows loads them only with test signing on, which shows "
                     L"\"Test Mode\" in a corner of the desktop. Turning it on takes a reboot; run this installer "
                     L"again after it.\n\nIf BitLocker is on, its protection is suspended for that one reboot, so "
                     L"that it does not ask for the recovery key.",
                     L"Test signing is off.", {{kButtonPrimary, L"Turn on"}, {IDCANCEL, L"Cancel"}}};
        if (wizard->ask(page) != kButtonPrimary)
            return kCancelled;
    } else if (!opts.test_signing) {
        return blocked(s, L"Test signing is off", {});
    }
    return run_work(L"Turning on test signing",
                    [](const Progress& p) {
                        p(30, L"Turning on test signing");
                        enable_test_signing();
                        p(100, L"Done");
                        return true;
                    },
                    L"Test signing is on from the next reboot");
}

std::wstring warnings(const SystemState& s)
{
    std::wstring w;
    if (s.smart_app_control == 1 || s.smart_app_control == 2)
        w += L"\n  •  Smart App Control is on or evaluating. It can block the driver's DLLs, which have no "
             L"signature it trusts: turn it off in Windows Security > App & browser control.";
    if (s.hvci)
        w += L"\n  •  Memory integrity is on. If the display driver does not start, turn it off in Windows "
             L"Security > Device security > Core isolation.";
    if (!s.vulkan_runtime)
        w += L"\n  •  OpenGL needs the Vulkan Runtime (vulkan-1.dll), which Windows does not include: install it "
             L"from https://vulkan.lunarg.com/sdk/home#windows. Direct3D, and Vulkan applications that bring their "
             L"own vulkan-1.dll, work without it.";
    return w;
}

bool confirm_install(const SystemState& s)
{
    std::wstring heading, action = L"Install";
    if (s.installed_version.empty()) {
        heading = L"Install vgd " + version;
    } else {
        int c = compare_versions(version, s.installed_version);
        action = c > 0 ? L"Update" : c == 0 ? L"Reinstall" : L"Downgrade";
        heading = c > 0  ? L"Update vgd " + s.installed_version + L" to " + version
                : c == 0 ? L"Reinstall vgd " + version
                         : L"Replace vgd " + s.installed_version + L" with the older " + version;
    }
    std::wstring text = L"vgd brings Direct3D 9 to 12, Vulkan and OpenGL to the VirtIO GPU, rendered on the host's "
                        L"GPU through Venus.";
    if (_wcsicmp(s.device_provider.c_str(), kProvider) != 0)
        text += format(L"\n\nThe VirtIO GPU runs on %ls (%ls %ls) now; vgd replaces it.", s.device_driver.c_str(),
                       s.device_provider.c_str(), s.device_version.c_str());
    std::wstring others;
    for (const auto& p : s.packages)
        if (!p.ours())
            others += format(L"\n  •  %ls %ls (%ls)", p.provider.c_str(), p.version.c_str(), p.inf.c_str());
    if (!others.empty())
        text += L"\n\nThese drivers for it are removed from the driver store, so that Windows does not pick them "
                L"over vgd again:" + others;
    if (!s.cert_exists)
        text += std::wstring(L"\n\nThe installer signs the driver package with a certificate it creates on this "
                             L"computer, \"") + kCertSubject + L"\", and trusts it, so that Windows installs the "
                L"package without asking. Its private key cannot leave this computer. Uninstalling removes it.";
    std::wstring w = warnings(s);
    if (!w.empty())
        text += L"\n\nWarnings:" + w;
    text += L"\n\nThe new driver starts at the next reboot; the drivers it replaces are removed then.";
    return wizard->ask({heading, {}, text, L"Ready.",
                        {{kButtonPrimary, action}, {IDCANCEL, L"Cancel"}}}) == kButtonPrimary;
}

int run_install(const Options& opts)
{
    SystemState s = inspect();
    log(s.summary());
    if (!s.x64 || s.build < 22000)
        return blocked(s, L"vgd needs 64-bit Windows 11",
                       format(L"This is Windows build %lu%ls.", s.build, s.x64 ? L"" : L", not x64"));
    if (s.devices == 0)
        return blocked(s, L"There is no VirtIO GPU",
                       std::wstring(L"The VM needs a virtio-vga or virtio-gpu device with Venus (QEMU: blob=true, "
                       L"venus=true, hostmem set). See ") + kDocs);
    if (s.secure_boot)
        return blocked(s, L"Secure Boot is on",
                       std::wstring(L"vgd's drivers are test-signed, which takes test signing, which Secure Boot "
                       L"does not allow. Turn Secure Boot off in the VM's firmware:\n\n  •  libvirt: use the UEFI "
                       L"firmware without Secure Boot (<os firmware=\"efi\"> with <feature enabled=\"no\" "
                       L"name=\"secure-boot\"/>).\n  •  QEMU: use OVMF_CODE.fd instead of OVMF_CODE.secboot.fd, or "
                       L"untick Device Manager > Secure Boot Configuration > Attempt Secure Boot in its setup (Esc "
                       L"at boot).\n\nThen run this installer again. See ") + kDocs);
    if (!s.test_signing)
        return offer_test_signing(opts, s);
    std::wstring w = warnings(s);
    if (!w.empty())
        log(L"Warnings:" + w);
    if (wizard && !confirm_install(s))
        return kCancelled;
    return run_work(L"Installing vgd " + version, [&s](const Progress& p) { return install(s, version, p); },
                    L"vgd " + version + L" is installed");
}

int run_uninstall()
{
    SystemState s = inspect();
    log(s.summary());
    if (wizard) {
        std::wstring installed = s.installed_version.empty() ? L"" : L" " + s.installed_version;
        Page page = {L"Uninstall vgd" + installed, {},
                     L"The VirtIO GPU goes back to the Microsoft Basic Display Adapter (no 3D) after a reboot, and "
                     L"the signing certificate is removed.\n\nTest signing stays on. If nothing else needs it, turn "
                     L"it off from an elevated prompt with  bcdedit /set testsigning off  and reboot.",
                     L"Ready.", {{kButtonPrimary, L"Uninstall"}, {IDCANCEL, L"Cancel"}}};
        if (wizard->ask(page) != kButtonPrimary)
            return kCancelled;
    }
    return run_work(L"Uninstalling vgd", [](const Progress& p) { return uninstall(p); }, L"vgd is uninstalled");
}

}  // namespace

// --- Utilities (setup.h)

std::wstring format(const wchar_t* fmt, ...)
{
    std::vector<wchar_t> buf(1024);
    for (;;) {
        va_list args;
        va_start(args, fmt);
        int n = vswprintf(buf.data(), buf.size(), fmt, args);
        va_end(args);
        if (n >= 0 && static_cast<size_t>(n) < buf.size())
            return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 4);
    }
}

std::wstring error_text(DWORD error)
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring text = msg ? msg : L"unknown error";
    LocalFree(msg);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.'))
        text.pop_back();
    return format(L"%ls (0x%08lX)", text.c_str(), error);
}

void fail(const std::wstring& what, DWORD error)
{
    throw Failure{error == ERROR_SUCCESS ? what : what + L": " + error_text(error)};
}

void fail(const std::wstring& what)
{
    fail(what, GetLastError());
}

std::wstring known_folder(const KNOWNFOLDERID& id)
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SHGetKnownFolderPath(id, 0, nullptr, &path) == S_OK)
        out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring system_dir()
{
    return known_folder(FOLDERID_System);
}

std::wstring self_path()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring log_path()
{
    return known_folder(FOLDERID_ProgramData) + L"\\vgd\\setup.log";
}

namespace {
std::mutex log_lock;
std::vector<std::wstring> history;
std::function<void(const std::wstring&)> sink;
}  // namespace

std::vector<std::wstring> log_history()
{
    std::lock_guard g(log_lock);
    return history;
}

void set_log_sink(std::function<void(const std::wstring&)> s)
{
    std::lock_guard g(log_lock);
    sink = std::move(s);
}

void log(const std::wstring& line)
{
    std::lock_guard g(log_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring text = format(L"%04u-%02u-%02u %02u:%02u:%02u  %ls\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                               t.wMinute, t.wSecond, line.c_str());
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), text.size(), utf8.data(), size, nullptr, nullptr);

    std::wstring path = log_path();
    CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written;
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, utf8.data(), utf8.size(), &written, nullptr);
        CloseHandle(f);
    }
    // The window shows the time of day only.
    std::wstring shown = text.substr(11, text.size() - 13);
    history.push_back(shown);
    if (sink)
        sink(shown);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE)
        WriteFile(out, utf8.data(), utf8.size(), &written, nullptr);
}

DWORD run(const std::wstring& exe, const std::wstring& args)
{
    std::wstring cmd = L"\"" + system_dir() + L"\\" + exe + L"\" " + args;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read_end, write_end;
    if (!CreatePipe(&read_end, &write_end, &sa, 0))
        fail(L"Running " + exe + L" failed");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write_end;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD error = GetLastError();
    CloseHandle(write_end);
    if (!ok) {
        CloseHandle(read_end);
        fail(L"Running " + exe + L" failed", error);
    }
    std::string output;
    char buf[4096];
    DWORD n;
    while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n)
        output.append(buf, n);
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::wstring wide(MultiByteToWideChar(CP_OEMCP, 0, output.data(), output.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, output.data(), output.size(), wide.data(), wide.size());
    log(format(L"%ls: exit code %lu\n%ls", cmd.c_str(), code, wide.c_str()));
    return code;
}

int compare_versions(const std::wstring& a, const std::wstring& b)
{
    const wchar_t *pa = a.c_str(), *pb = b.c_str();
    while (*pa || *pb) {
        wchar_t *ea, *eb;
        unsigned long va = wcstoul(pa, &ea, 10), vb = wcstoul(pb, &eb, 10);
        if (va != vb)
            return va < vb ? -1 : 1;
        pa = *ea == L'.' ? ea + 1 : ea;
        pb = *eb == L'.' ? eb + 1 : eb;
        if (ea == pa && eb == pb)
            break;
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    version = resource_string(1);
    build_id = resource_string(2);
    Options opts;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (_wcsicmp(a.c_str(), L"/S") == 0 || _wcsicmp(a.c_str(), L"/quiet") == 0)
            opts.silent = true;
        else if (_wcsicmp(a.c_str(), L"/uninstall") == 0)
            opts.uninstall = true;
        else if (_wcsicmp(a.c_str(), L"/testsigning") == 0)
            opts.test_signing = true;
        else if (_wcsicmp(a.c_str(), L"/cleanup") == 0)
            opts.cleanup = opts.silent = true;
        else {
            MessageBoxW(nullptr,
                        L"vgd-setup.exe [/S] [/testsigning]\n    Install or update; /S silently; /testsigning with "
                        L"/S: turn test signing on if it is off.\n\nvgd-setup.exe /uninstall [/S]",
                        kTitle, MB_ICONINFORMATION);
            return a == L"/?" ? kDone : ERROR_INVALID_PARAMETER;
        }
    }
    LocalFree(argv);
    log(format(L"vgd-setup %ls (%ls): %ls", version.c_str(), build_id.c_str(), GetCommandLineW()));

    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Global\\vgd-setup");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        log(L"Another vgd-setup is running");
        if (!opts.silent)
            MessageBoxW(nullptr, L"vgd Setup is running already.", kTitle, MB_ICONWARNING);
        return kRunning;
    }
    if (!opts.silent)
        wizard = std::make_unique<Wizard>(version);
    int code = kFailed;
    try {
        if (opts.cleanup) {
            cleanup();
            code = kDone;
        } else {
            code = opts.uninstall ? run_uninstall() : run_install(opts);
        }
    } catch (const Failure& f) {
        log(L"Failed: " + f.message);
        if (!opts.silent)
            MessageBoxW(nullptr, f.message.c_str(), kTitle, MB_ICONERROR);
    }
    log(format(L"Exit code %d", code));
    wizard.reset();
    CloseHandle(instance);
    return code;
}
