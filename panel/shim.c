// SPDX-License-Identifier: MIT
// venera-panel.com: the console face of venera-panel.exe. A shell looks for .com before .exe, so `venera-panel /info`
// runs this console program, which runs the exe beside it with the same arguments, its console and handles, waits, and
// returns its exit code; a GUI program alone would leave the shell's prompt back before its output. Without arguments
// (the window) it starts the exe and returns at once.
#include <windows.h>

#include <wchar.h>

int wmain(int argc, wchar_t** argv)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n < 4 || n >= MAX_PATH)
        return 2;
    wcscpy(exe + n - 4, L".exe");

    // The command line as given, with the program replaced by the exe.
    const wchar_t* args = GetCommandLineW();
    if (*args == L'"') {
        args = wcschr(args + 1, L'"');
        args = args ? args + 1 : L"";
    } else {
        while (*args && *args != L' ' && *args != L'\t')
            args++;
    }
    size_t size = wcslen(exe) + wcslen(args) + 3;
    wchar_t* cmd = HeapAlloc(GetProcessHeap(), 0, size * sizeof(wchar_t));
    swprintf(cmd, size, L"\"%ls\"%ls", exe, args);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fwprintf(stderr, L"Starting %ls failed: error %lu\n", exe, GetLastError());
        return 2;
    }
    DWORD code = 0;
    if (argc > 1) {
        SetConsoleCtrlHandler(NULL, TRUE);  // Ctrl+C reaches the exe, which shares this console
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
