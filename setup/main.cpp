// SPDX-License-Identifier: MIT
// vgd-setup.exe: the installer. It checks the system (Windows 11 x64, a VirtIO GPU, Secure Boot off, test signing
// on; it offers to turn test signing on), says what it will replace, and installs, updates or reinstalls the driver
// package (install.cpp). The copy it leaves in Program Files uninstalls.
//   vgd-setup.exe [/S] [/testsigning]     install or update; /S: silently; /testsigning: with /S, turn test signing
//                                         on if it is off (then restart and run it again)
//   vgd-setup.exe /uninstall [/S]
// Exit codes as MSI's: 0 done, 3010 done but restart, 1602 cancelled, 1603 failed, 1633 this system cannot take it
// (yet), 1618 another instance is running. The log is %ProgramData%\vgd\setup.log.
#include "setup.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdarg>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace {

constexpr int kDone = 0, kRestart = ERROR_SUCCESS_REBOOT_REQUIRED, kCancelled = ERROR_INSTALL_USEREXIT,
              kFailed = ERROR_INSTALL_FAILURE, kUnsupported = ERROR_INSTALL_PLATFORM_UNSUPPORTED,
              kRunning = ERROR_INSTALL_ALREADY_RUNNING;
constexpr wchar_t kTitle[] = L"vgd Setup";
constexpr int kButtonPrimary = 100, kButtonSecondary = 101;

struct Options {
    bool silent = false, uninstall = false, test_signing = false;
};

std::wstring version, build_id;

std::wstring resource_string(UINT id)
{
    const wchar_t* s = nullptr;
    int n = LoadStringW(nullptr, id, reinterpret_cast<LPWSTR>(&s), 0);
    return n > 0 ? std::wstring(s, n) : L"?";
}

// --- Dialogs

HRESULT CALLBACK on_dialog(HWND, UINT msg, WPARAM, LPARAM lparam, LONG_PTR)
{
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lparam), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

struct Dialog {
    PCWSTR icon = TD_INFORMATION_ICON;
    std::wstring main, content, expanded, footer;
    std::vector<std::pair<int, std::wstring>> buttons;
    TASKDIALOG_COMMON_BUTTON_FLAGS common = 0;
    bool command_links = false;

    int show() const
    {
        std::vector<TASKDIALOG_BUTTON> b;
        for (const auto& [id, text] : buttons)
            b.push_back({id, text.c_str()});
        TASKDIALOGCONFIG c = {sizeof(c)};
        c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT | (command_links ? TDF_USE_COMMAND_LINKS : 0);
        c.pszWindowTitle = kTitle;
        c.pszMainIcon = icon;
        c.pszMainInstruction = main.c_str();
        c.pszContent = content.c_str();
        c.pButtons = b.data();
        c.cButtons = b.size();
        c.dwCommonButtons = common;
        if (!expanded.empty()) {
            c.pszExpandedInformation = expanded.c_str();
            c.pszExpandedControlText = L"Hide details";
            c.pszCollapsedControlText = L"Show details";
            c.dwFlags |= TDF_EXPAND_FOOTER_AREA;
        }
        if (!footer.empty()) {
            c.pszFooter = footer.c_str();
            c.pszFooterIcon = TD_WARNING_ICON;
        }
        c.pfCallback = on_dialog;
        int pressed = IDCANCEL;
        TaskDialogIndirect(&c, &pressed, nullptr, nullptr);
        return pressed;
    }
};

// Runs `work` with a progress dialog (or, silent, just the log); returns whether a restart is needed.
struct ProgressRun {
    std::function<bool(const Progress&)> work;
    std::wstring title;
    bool restart = false;
    std::optional<std::wstring> error;
    std::atomic<bool> done{false};
    std::thread thread;
};

void work(ProgressRun* run, const Progress& progress)
{
    try {
        run->restart = run->work(progress);
    } catch (const Failure& f) {
        run->error = f.message;
    } catch (const std::exception& e) {
        run->error = format(L"%hs", e.what());
    }
    if (run->error)
        log(L"Failed: " + *run->error);
}

HRESULT CALLBACK on_progress(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref)
{
    auto run = reinterpret_cast<ProgressRun*>(ref);
    if (msg == TDN_CREATED) {
        SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
        run->thread = std::thread([run, hwnd] {
            work(run, [hwnd](int percent, const std::wstring& step) {
                log(step);
                SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, percent, 0);
                SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(step.c_str()));
            });
            run->done = true;
            SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, TRUE);
            PostMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
        });
    } else if (msg == TDN_BUTTON_CLICKED && !run->done) {
        return S_FALSE;  // no cancelling half way
    }
    return S_OK;
}

bool run_with_progress(ProgressRun& run, const Options& opts)
{
    if (opts.silent) {
        work(&run, [](int, const std::wstring& step) { log(step); });
    } else {
        TASKDIALOGCONFIG c = {sizeof(c)};
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_SIZE_TO_CONTENT;
        c.pszWindowTitle = kTitle;
        c.pszMainInstruction = run.title.c_str();
        c.pszContent = L"Starting";
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pfCallback = on_progress;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&run);
        c.cxWidth = 260;
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
        if (run.thread.joinable())
            run.thread.join();
    }
    return !run.error;
}

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

// The result of an install or uninstall: the error, or an offer to restart.
int finish(const ProgressRun& run, const Options& opts, const std::wstring& done_text)
{
    if (run.error) {
        if (!opts.silent)
            Dialog{TD_ERROR_ICON, run.title + L" failed", *run.error,
                   L"The log is " + log_path() + L".", {}, {}, TDCBF_CLOSE_BUTTON}.show();
        return kFailed;
    }
    log(done_text);
    if (!run.restart)
        return kDone;
    if (!opts.silent) {
        Dialog d{TD_INFORMATION_ICON, done_text, L"Restart Windows to finish."};
        d.buttons = {{kButtonPrimary, L"Restart now"}, {kButtonSecondary, L"Restart later"}};
        if (d.show() == kButtonPrimary)
            restart_windows();
    }
    return kRestart;
}

// --- Install

constexpr wchar_t kDocs[] = L"https://github.com/Aleksanaa/virtio-win-dx12#required-configuration";

int blocked(const Options& opts, const SystemState& s, const std::wstring& main, const std::wstring& content)
{
    log(L"Cannot install: " + main);
    if (!opts.silent)
        Dialog{TD_ERROR_ICON, main, content, s.summary(), {}, {}, TDCBF_CLOSE_BUTTON}.show();
    return kUnsupported;
}

int offer_test_signing(const Options& opts, const SystemState& s)
{
    int choice = kButtonSecondary;
    if (opts.silent) {
        if (!opts.test_signing)
            return blocked(opts, s, L"Test signing is off", {});
    } else {
        Dialog d{TD_WARNING_ICON, L"Test signing must be on",
                 L"vgd's drivers are test-signed: Windows loads them only with test signing on, which shows "
                 L"\"Test Mode\" in a corner of the desktop. It takes a restart; run this installer again after it.\n\n"
                 L"If BitLocker is on, its protection is suspended for that one restart, so that it does not ask "
                 L"for the recovery key.",
                 s.summary()};
        d.buttons = {{kButtonPrimary, L"Turn on test signing and restart now"},
                     {kButtonSecondary, L"Turn on test signing, restart later"},
                     {IDCANCEL, L"Cancel"}};
        d.command_links = true;
        choice = d.show();
        if (choice == IDCANCEL)
            return kCancelled;
    }
    try {
        enable_test_signing();
    } catch (const Failure& f) {
        log(L"Failed: " + f.message);
        if (!opts.silent)
            Dialog{TD_ERROR_ICON, L"Turning on test signing failed", f.message, {}, {}, {}, TDCBF_CLOSE_BUTTON}.show();
        return kFailed;
    }
    log(L"Test signing is on from the next restart");
    if (choice == kButtonPrimary)
        restart_windows();
    else if (!opts.silent)
        Dialog{TD_INFORMATION_ICON, L"Test signing is on from the next restart",
               L"Restart Windows, then run this installer again.", {}, {}, {}, TDCBF_OK_BUTTON}.show();
    return kRestart;
}

std::wstring warnings(const SystemState& s)
{
    std::wstring w;
    auto add = [&w](const std::wstring& line) { w += (w.empty() ? L"" : L"\n\n") + line; };
    if (s.smart_app_control == 1 || s.smart_app_control == 2)
        add(L"Smart App Control is on or evaluating. It can block the driver's DLLs, which have no signature it "
            L"trusts: turn it off in Windows Security > App & browser control.");
    if (s.hvci)
        add(L"Memory integrity is on. If the display driver does not start, turn it off in Windows Security > "
            L"Device security > Core isolation.");
    if (!s.vulkan_runtime)
        add(L"OpenGL needs the Vulkan Runtime (vulkan-1.dll), which Windows does not include: install it from "
            L"<a href=\"https://vulkan.lunarg.com/sdk/home#windows\">LunarG</a>. Direct3D and Vulkan "
            L"applications that bring their own vulkan-1.dll work without it.");
    return w;
}

bool confirm_install(const SystemState& s)
{
    std::wstring main, action = L"Install";
    if (s.installed_version.empty()) {
        main = L"Install vgd " + version;
    } else {
        int c = compare_versions(version, s.installed_version);
        action = c > 0 ? L"Update" : c == 0 ? L"Reinstall" : L"Downgrade";
        main = c > 0  ? L"Update vgd " + s.installed_version + L" to " + version
             : c == 0 ? L"Reinstall vgd " + version
                      : L"Replace vgd " + s.installed_version + L" with the older " + version;
    }
    std::wstring content = L"Direct3D 9 to 12, Vulkan and OpenGL for the VirtIO GPU, rendered on the host's GPU "
                           L"through Venus.";
    if (_wcsicmp(s.device_provider.c_str(), kProvider) != 0)
        content += format(L"\n\nThe VirtIO GPU runs on %ls (%ls %ls) now.", s.device_driver.c_str(),
                          s.device_provider.c_str(), s.device_version.c_str());
    std::wstring others;
    for (const auto& p : s.packages)
        if (!p.ours())
            others += format(L"\n  •  %ls %ls (%ls)", p.provider.c_str(), p.version.c_str(), p.inf.c_str());
    if (!others.empty())
        content += L"\n\nThese drivers for it are removed from the driver store, so that Windows does not pick "
                   L"them over vgd again:" + others;
    if (!s.cert_exists)
        content += std::wstring(L"\n\nThe installer signs the driver package with a certificate it creates on "
                                L"this computer, \"") + kCertSubject + L"\", and trusts it, so that Windows "
                   L"installs the package without asking. Its private key cannot leave this computer. "
                   L"Uninstalling removes it.";
    Dialog d{TD_SHIELD_ICON, main, content, s.summary(), warnings(s)};
    d.buttons = {{kButtonPrimary, action}};
    d.common = TDCBF_CANCEL_BUTTON;
    return d.show() == kButtonPrimary;
}

int run_install(const Options& opts)
{
    SystemState s = inspect();
    log(s.summary());
    if (!s.x64 || s.build < 22000)
        return blocked(opts, s, L"vgd needs 64-bit Windows 11",
                       format(L"This is Windows build %lu%ls.", s.build, s.x64 ? L"" : L", not x64"));
    if (s.devices == 0)
        return blocked(opts, s, L"There is no VirtIO GPU",
                       std::wstring(L"The VM needs a virtio-vga or virtio-gpu device with Venus (QEMU: blob=true,"
                       L"venus=true, hostmem set). See <a href=\"") + kDocs + L"\">the required configuration</a>.");
    if (s.secure_boot)
        return blocked(opts, s, L"Secure Boot is on",
                       std::wstring(L"vgd's drivers are test-signed, which takes test signing, which Secure Boot "
                       L"does not allow. Turn Secure Boot off in the VM's firmware: with libvirt, use the UEFI "
                       L"firmware without Secure Boot (<os firmware=\"efi\"> with <feature enabled=\"no\" "
                       L"name=\"secure-boot\"/>); with QEMU, OVMF_CODE.fd instead of OVMF_CODE.secboot.fd, or "
                       L"untick Device Manager > Secure Boot Configuration > Attempt Secure Boot in its setup "
                       L"(Esc at boot). Then run this installer again.\n\nSee <a href=\"") + kDocs +
                       L"\">the required configuration</a>.");
    if (!s.test_signing)
        return offer_test_signing(opts, s);
    std::wstring w = warnings(s);
    if (!w.empty())
        log(L"Warnings: " + w);
    if (!opts.silent && !confirm_install(s))
        return kCancelled;

    ProgressRun run;
    run.title = L"Installing vgd " + version;
    run.work = [&s](const Progress& p) { return install(s, version, p); };
    run_with_progress(run, opts);
    return finish(run, opts, L"vgd " + version + L" is installed");
}

int run_uninstall(const Options& opts)
{
    SystemState s = inspect();
    log(s.summary());
    if (!opts.silent) {
        std::wstring installed = s.installed_version.empty() ? L"" : L" " + s.installed_version;
        Dialog d{TD_SHIELD_ICON, L"Uninstall vgd" + installed + L"?",
                 L"The VirtIO GPU goes back to the Microsoft Basic Display Adapter (no 3D) after a restart, and "
                 L"the signing certificate is removed.\n\nTest signing stays on. If nothing else needs it, turn it "
                 L"off from an elevated prompt with  bcdedit /set testsigning off  and restart.",
                 s.summary()};
        d.buttons = {{kButtonPrimary, L"Uninstall"}};
        d.common = TDCBF_CANCEL_BUTTON;
        if (d.show() != kButtonPrimary)
            return kCancelled;
    }
    ProgressRun run;
    run.title = L"Uninstalling vgd";
    run.work = [](const Progress& p) { return uninstall(p); };
    run_with_progress(run, opts);
    return finish(run, opts, L"vgd is uninstalled");
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

void log(const std::wstring& line)
{
    static std::mutex lock;
    std::lock_guard g(lock);
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
    int code = kFailed;
    try {
        code = opts.uninstall ? run_uninstall(opts) : run_install(opts);
    } catch (const Failure& f) {
        log(L"Failed: " + f.message);
        if (!opts.silent)
            MessageBoxW(nullptr, f.message.c_str(), kTitle, MB_ICONERROR);
    }
    log(format(L"Exit code %d", code));
    CloseHandle(instance);
    return code;
}
