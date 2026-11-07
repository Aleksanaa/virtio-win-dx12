// SPDX-License-Identifier: MIT
// The command line: vgd-panel.exe with a command prints instead of opening the window. Options start with /, - or --.
// The exe is a GUI program, which consoles do not wait for; vgd-panel.com (shim.c) is the console program a shell
// finds first, and runs the exe with its own console and handles.
#include "panel.h"

#include <cwctype>

namespace panel {

namespace {

constexpr DWORD kProbeTimeoutMs = 20000;

const wchar_t kHelp[] =
    L"vgd Control Panel\n"
    L"\n"
    L"Usage: vgd-panel [command]\n"
    L"\n"
    L"  (none)     Open the control panel.\n"
    L"  /info      Print the system report: the driver, the host, what each graphics API offers and the\n"
    L"             displays. The exit code is 1 when something in it does not work.\n"
    L"  /probe     Try each graphics API on the VirtIO GPU and print what it found as key=value lines\n"
    L"             (the check behind /info).\n"
    L"  /page N    Open the control panel on page N: 0 Overview, 1 Settings, 2 Logs, 3 Tests, 4 Tools.\n"
    L"  /help      Show this help.\n"
    L"\n"
    L"Options may also start with - or --.\n";

// Where output goes: a file or pipe it was redirected to (UTF-8), or else the console it was started from.
class Output {
public:
    explicit Output(DWORD which)
    {
        handle_ = GetStdHandle(which);
        DWORD type = handle_ && handle_ != INVALID_HANDLE_VALUE ? GetFileType(handle_) : FILE_TYPE_UNKNOWN;
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)
            return;
        // A console: the one of the program that started this one (vgd-panel.com, or a shell when run as the .exe).
        if (AttachConsole(ATTACH_PARENT_PROCESS) || GetLastError() == ERROR_ACCESS_DENIED /* already attached */)
            handle_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  0, nullptr);
        console_ = true;
    }
    void print(const std::wstring& s) const
    {
        if (!handle_ || handle_ == INVALID_HANDLE_VALUE)
            return;
        DWORD written;
        if (console_ && WriteConsoleW(handle_, s.c_str(), s.size(), &written, nullptr))
            return;
        std::string utf8 = narrow(s);
        WriteFile(handle_, utf8.data(), utf8.size(), &written, nullptr);
    }

private:
    HANDLE handle_ = nullptr;
    bool console_ = false;
};

// "/info", "-info" and "--info" all as "info".
std::wstring option(const wchar_t* arg)
{
    std::wstring s = arg;
    size_t skip = s.compare(0, 2, L"--") == 0 ? 2 : (!s.empty() && (s[0] == L'/' || s[0] == L'-')) ? 1 : 0;
    if (!skip)
        return {};
    s.erase(0, skip);
    for (wchar_t& c : s)
        c = std::towlower(c);
    return s;
}

}  // namespace

int runCommandLine(int argc, wchar_t** argv, int* page)
{
    for (int i = 1; i < argc; i++) {
        std::wstring o = option(argv[i]);
        if (o == L"probe")
            return runProbe();
        if (o == L"help" || o == L"?" || o == L"h") {
            Output(STD_OUTPUT_HANDLE).print(kHelp);
            return 0;
        }
        if (o == L"info") {
            Output out(STD_OUTPUT_HANDLE);
            SystemInfo info = readSystemInfo();
            ProbeResult probe = probeNow(kProbeTimeoutMs);
            std::vector<InfoSection> report = describe(info, &probe);
            std::wstring text = reportText(report);
            for (size_t p; (p = text.find(L"\r\n")) != std::wstring::npos;)
                text.replace(p, 2, L"\n");
            out.print(text);
            return anyProblem(report) ? 1 : 0;
        }
        if (o == L"page" && i + 1 < argc) {  // the window's
            *page = _wtoi(argv[++i]);
            continue;
        }
        Output(STD_ERROR_HANDLE).print(L"Unknown command: " + std::wstring(argv[i]) + L"\n\n" + kHelp);
        return 2;
    }
    return -1;
}

}  // namespace panel
