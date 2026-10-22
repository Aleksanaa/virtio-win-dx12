// SPDX-License-Identifier: MIT
// Guest tool: prints every thread's stack of a running process, for hangs (run it through the QEMU guest agent while
// the desktop is black). Frames are module+offset, plus dbghelp's symbol where it has one (exports of system DLLs);
// our DLLs and the ICD are resolved offline from their own symbols.
//   stackdump <pid | exe name, e.g. dwm.exe>      all matching processes
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void enable_debug_privilege()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);
}

std::vector<DWORD> find_processes(const char* what)
{
    std::vector<DWORD> pids;
    char* end;
    const unsigned long pid = std::strtoul(what, &end, 10);
    if (*what && !*end) {
        pids.push_back(pid);
        return pids;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe{sizeof(pe)};
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (!_stricmp(pe.szExeFile, what))
            pids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return pids;
}

void print_frame(HANDLE process, DWORD64 pc)
{
    char module[MAX_PATH] = "?";
    DWORD64 base = SymGetModuleBase64(process, pc);
    if (base) {
        IMAGEHLP_MODULE64 mi{};
        mi.SizeOfStruct = sizeof(mi);
        if (SymGetModuleInfo64(process, base, &mi))
            std::snprintf(module, sizeof(module), "%s", mi.ModuleName);
    }
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(process, pc, &disp, sym))
        std::printf("    %s+0x%llx  %s+0x%llx\n", module, static_cast<unsigned long long>(pc - base), sym->Name,
                    static_cast<unsigned long long>(disp));
    else
        std::printf("    %s+0x%llx\n", module, static_cast<unsigned long long>(pc - base));
}

void dump(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::printf("pid %lu: OpenProcess failed: %lu\n", pid, GetLastError());
        return;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(process, nullptr, TRUE);
    using GetThreadDescriptionProc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetThreadDescriptionProc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));

    std::printf("== pid %lu\n", pid);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid)
            continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!t) {
            std::printf("  thread %lu: OpenThread failed: %lu\n", te.th32ThreadID, GetLastError());
            continue;
        }
        wchar_t name[64] = L"";
        if (get_description) {
            PWSTR desc = nullptr;
            if (SUCCEEDED(get_description(t, &desc)) && desc) {
                wcsncpy(name, desc, ARRAYSIZE(name) - 1);
                LocalFree(desc);
            }
        }
        std::printf("  thread %lu %ls\n", te.th32ThreadID, name);
        // Frames are collected while suspended and printed after: symbol lookups can be slow.
        std::vector<DWORD64> pcs;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t, &ctx)) {
                STACKFRAME64 f{};
                f.AddrPC.Offset = ctx.Rip;
                f.AddrPC.Mode = AddrModeFlat;
                f.AddrStack.Offset = ctx.Rsp;
                f.AddrStack.Mode = AddrModeFlat;
                f.AddrFrame.Offset = ctx.Rbp;
                f.AddrFrame.Mode = AddrModeFlat;
                while (pcs.size() < 64 &&
                       StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, t, &f, &ctx, nullptr, SymFunctionTableAccess64,
                                   SymGetModuleBase64, nullptr) &&
                       f.AddrPC.Offset)
                    pcs.push_back(f.AddrPC.Offset);
            }
            ResumeThread(t);
        }
        for (DWORD64 pc : pcs)
            print_frame(process, pc);
        CloseHandle(t);
    }
    CloseHandle(snap);
    SymCleanup(process);
    CloseHandle(process);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::printf("usage: stackdump <pid | exe name>\n");
        return 2;
    }
    enable_debug_privilege();
    const std::vector<DWORD> pids = find_processes(argv[1]);
    if (pids.empty()) {
        std::printf("no process %s\n", argv[1]);
        return 1;
    }
    for (DWORD pid : pids)
        dump(pid);
    std::fflush(stdout);
    return 0;
}
