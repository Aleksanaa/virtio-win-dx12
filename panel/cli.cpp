// SPDX-License-Identifier: MIT
// The command line: vgd-panel.exe with a command prints instead of opening the window. Options start with /, - or --.
// The exe is a GUI program, which consoles do not wait for; vgd-panel.com (shim.c) is the console program a shell
// finds first, and runs the exe with its own console and handles.
#include "panel.h"

#include <algorithm>
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
    L"  /info      Print the system report: the driver, the host, the guest with its displays, and\n"
    L"             what each graphics API offers. The exit code is 1 when something in it does not work.\n"
    L"  /probe     Try each graphics API on the VirtIO GPU and print what it found as key=value lines\n"
    L"             (the check behind /info).\n"
    L"  /test      List the tests.\n"
    L"  /test NAME...\n"
    L"             Run tests and print each one's result, and the output of those that fail. A NAME is a\n"
    L"             group (d3d12), a test in one (d3d12.copy) or all. The exit code is 1 when one fails.\n"
    L"      /verbose   also print the output of those that pass\n"
    L"      /debug     turn the D3D12 debug layer on (needs the Graphics Tools optional feature)\n"
    L"  /log       Print what the drivers log, as they log it, until Ctrl+C. Run as administrator for the\n"
    L"             kernel driver's messages and services' output too.\n"
    L"      /all       every program's debug output, not only the drivers'\n"
    L"  /set       List the settings, as on the Settings page, with their values and the values they take.\n"
    L"  /set NAME VALUE...\n"
    L"             Change settings (as administrator). Drivers loaded already keep their logging settings:\n"
    L"             programs started afterwards get them.\n"
    L"  /restart-dwm\n"
    L"             Restart the Desktop Window Manager (as administrator), for the desktop's own drivers to\n"
    L"             take the logging settings.\n"
    L"  /fps       Show the frame rate overlay (as administrator): frames the screen shows, VSyncs,\n"
    L"             the longest gap between frames and mouse reports, and below them each program's frame\n"
    L"             rate, in the top-left corner. Ctrl+Alt+F closes it.\n"
    L"      off        close it\n"
    L"  /page N    Open the control panel on page N: 0 Overview, 1 Settings, 2 Logs, 3 Tests, 4 Tools.\n"
    L"  /help      Show this help.\n"
    L"\n"
    L"Options may also start with - or --.\n";

std::wstring indent(const std::wstring& text, const wchar_t* prefix)
{
    std::wstring out;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        out += prefix + line + L"\n";
        start = end + 1;
    }
    return out;
}

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

void listTests(const Output& out)
{
    std::wstring text;
    for (const TestGroup& g : testGroups()) {
        text += std::wstring(g.name) + L" (" + widen(g.id) + L"): " + g.desc + L"\n";
        for (const Test& t : g.tests) {
            std::wstring id = widen(g.id) + L"." + widen(t.id);
            id.resize(std::max<size_t>(id.size() + 1, 22), L' ');
            text += L"  " + id + t.name + L": " + t.desc + L"\n";
        }
        text += L"\n";
    }
    out.print(text + L"Run them with /test NAME..., where a NAME is a group, a test or all.\n");
}

int runTests(const std::vector<std::string>& names, bool verbose)
{
    Output out(STD_OUTPUT_HANDLE);
    std::vector<std::string> ids;
    std::string unknown;
    if (!selectTests(names, &ids, &unknown)) {
        Output(STD_ERROR_HANDLE).print(L"No test or group named " + widen(unknown) + L"; /test lists them.\n");
        return 2;
    }
    int passed = 0, failed = 0;
    for (const std::string& id : ids) {
        std::wstring name = widen(id);
        name.resize(std::max<size_t>(name.size() + 1, 26), L' ');
        out.print(name);
        TestResult r = runTest(id, nullptr);
        wchar_t time[32];
        swprintf(time, 32, L"%6.1f s", r.seconds);
        out.print(std::wstring(r.ok ? L"passed " : L"FAILED ") + time + (r.ok ? L"" : L"  " + r.summary) + L"\n");
        if (!r.ok || verbose)
            out.print(indent(r.output, L"    "));
        (r.ok ? passed : failed)++;
    }
    out.print(L"\n" + std::to_wstring(passed) + L" passed, " + std::to_wstring(failed) + L" failed\n");
    return failed ? 1 : 0;
}

// /set: without arguments the list, else NAME VALUE pairs to change.
int runSet(const std::vector<std::wstring>& args)
{
    Output out(STD_OUTPUT_HANDLE), err(STD_ERROR_HANDLE);
    if (args.empty()) {
        std::wstring text;
        for (const SettingLine& s : listSettings()) {
            std::wstring name = s.name, value = s.value;
            name.resize(std::max<size_t>(name.size() + 1, 16), L' ');
            value.resize(std::max<size_t>(value.size() + 1, 9), L' ');
            text += name + value + s.title + L" (" + s.values + L")\n";
        }
        out.print(text + L"Change them with /set NAME VALUE, also several at once.\n");
        return 0;
    }
    if (args.size() % 2) {
        err.print(L"No value for " + args.back() + L".\n");
        return 2;
    }
    bool logging = false;
    for (size_t i = 0; i < args.size(); i += 2) {
        std::wstring e = changeSetting(args[i], args[i + 1], &logging);
        if (!e.empty()) {
            err.print(e + L".\n");
            return 1;
        }
        out.print(args[i] + L" = " + args[i + 1] + L"\n");
    }
    if (logging)
        out.print(L"Programs started from now on use these; /restart-dwm for the desktop's own.\n");
    return 0;
}

HANDLE g_quit;

int followLog(bool all)
{
    AttachConsole(ATTACH_PARENT_PROCESS);  // for Ctrl+C, also when the output goes to a file
    Output out(STD_OUTPUT_HANDLE), err(STD_ERROR_HANDLE);
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler([](DWORD) -> BOOL { return SetEvent(g_quit); }, TRUE);
    LogCapture capture;
    capture.start([&](LogLine&& l) {
        if (all || !l.component.empty())
            out.print(formatLogLine(l) + L"\n");
    });
    for (const std::wstring& e : {capture.userError, capture.kernelError})
        if (!e.empty())
            err.print(e + L".\n");
    if (!capture.userError.empty() && !capture.kernelError.empty())
        return 1;
    WaitForSingleObject(g_quit, INFINITE);
    capture.stop();
    return 0;
}

}  // namespace

int runCommandLine(int argc, wchar_t** argv, int* page)
{
    for (int i = 1; i < argc; i++)
        if (option(argv[i]) == L"debug")
            testDebugLayer = true;
    for (int i = 1; i < argc; i++) {
        std::wstring o = option(argv[i]);
        if (o == L"probe")
            return runProbe();
        if (o == L"run-test" && i + 1 < argc)  // the child runTest starts
            return runTestChild(narrow(argv[i + 1]));
        if (o == L"shared-child" && i + 1 < argc)  // the process d3d11's shared tests start
            return runD3D11SharedChild(argv[i + 1]);
        if (o == L"test") {
            std::vector<std::string> names;
            bool verbose = false;
            for (int j = i + 1; j < argc; j++) {
                std::wstring n = option(argv[j]);
                if (n == L"verbose")
                    verbose = true;
                else if (n.empty()) {
                    std::wstring name = argv[j];
                    for (wchar_t& c : name)
                        c = std::towlower(c);
                    names.push_back(narrow(name));
                } else if (n != L"debug") {
                    Output(STD_ERROR_HANDLE).print(L"Unknown option: " + std::wstring(argv[j]) + L"\n\n" + kHelp);
                    return 2;
                }
            }
            if (names.empty()) {
                listTests(Output(STD_OUTPUT_HANDLE));
                return 0;
            }
            return runTests(names, verbose);
        }
        if (o == L"log") {
            bool all = false;
            for (int j = i + 1; j < argc; j++) {
                if (option(argv[j]) != L"all") {
                    Output(STD_ERROR_HANDLE).print(L"Unknown option: " + std::wstring(argv[j]) + L"\n\n" + kHelp);
                    return 2;
                }
                all = true;
            }
            return followLog(all);
        }
        if (o == L"set") {
            std::vector<std::wstring> args;
            for (int j = i + 1; j < argc; j++) {
                std::wstring a = argv[j];
                for (wchar_t& c : a)
                    c = std::towlower(c);
                args.push_back(a);
            }
            return runSet(args);
        }
        if (o == L"fps") {
            if (i + 1 < argc) {
                std::wstring a = argv[i + 1];
                if (_wcsicmp(a.c_str(), L"off")) {
                    Output(STD_ERROR_HANDLE).print(L"Unknown option: " + a + L"\n\n" + kHelp);
                    return 2;
                }
                if (HWND overlay = findFpsOverlay())
                    PostMessageW(overlay, WM_CLOSE, 0, 0);
                return 0;
            }
            std::wstring e = runFpsOverlay();
            if (e.empty())
                return 0;
            // Started from the Tools page there is no console to print to.
            if (AttachConsole(ATTACH_PARENT_PROCESS) || GetLastError() == ERROR_ACCESS_DENIED)
                Output(STD_ERROR_HANDLE).print(e + L".\n");
            else
                MessageBoxW(nullptr, (e + L".").c_str(), L"Frame rate overlay", MB_ICONERROR);
            return 1;
        }
        if (o == L"restart-dwm") {
            if (!elevated()) {
                Output(STD_ERROR_HANDLE).print(L"Restarting the Desktop Window Manager needs administrator rights.\n");
                return 1;
            }
            restartDwm();
            return 0;
        }
        if (o == L"debug")
            continue;
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
