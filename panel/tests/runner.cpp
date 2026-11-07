// SPDX-License-Identifier: MIT
// Running the tests: each in a child process, `vgd-panel.exe /run-test <group>.<test>`, which prints what the test
// does and exits with 0 when it passed. A crash (the child reports where first) or a hang fails only that test, and
// the panel and the command line carry on with the next.
#include "../panel.h"

#include <cstdio>
#include <thread>

namespace panel {

bool testDebugLayer = false;

namespace {

constexpr DWORD kTestTimeoutMs = 120000;

std::string module(void* address, uintptr_t* offset)
{
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &mod))
        GetModuleFileNameA(mod, name, sizeof(name));
    *offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mod);
    return name;
}

// Prints where an access violation happened (module + offset, address accessed, thread) and a DirectX exception's
// stack before the process dies.
LONG CALLBACK reportCrash(EXCEPTION_POINTERS* e)
{
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    uintptr_t offset;
    if (r->ExceptionCode == 0x87a) {
        std::printf("DirectX exception, %lu parameters:\n", r->NumberParameters);
        for (DWORD i = 0; i < r->NumberParameters; i++)
            std::printf("  0x%llx\n", static_cast<unsigned long long>(r->ExceptionInformation[i]));
        void* frames[32];
        USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; i++) {
            std::string name = module(frames[i], &offset);
            std::printf("  %s+0x%llx\n", name.c_str(), static_cast<unsigned long long>(offset));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    ULONG_PTR kind = r->ExceptionInformation[0];
    std::string name = module(r->ExceptionAddress, &offset);
    std::printf("ACCESS VIOLATION in %s+0x%llx (ip %p), %s address 0x%llx, thread %lu\n", name.c_str(),
                static_cast<unsigned long long>(offset), r->ExceptionAddress,
                kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
                static_cast<unsigned long long>(r->ExceptionInformation[1]), GetCurrentThreadId());
    // A call through a null pointer leaves the caller's return address on top of the stack.
    void* ret = *reinterpret_cast<void**>(e->ContextRecord->Rsp);
    name = module(ret, &offset);
    std::printf("  return address on stack: %s+0x%llx (%p)\n", name.c_str(), static_cast<unsigned long long>(offset),
                ret);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Why a test failed, in a line: the last line of its output that says so.
std::wstring failure(const std::string& output)
{
    std::string why;
    for (size_t start = 0; start < output.size();) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();
        std::string line = output.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t lead = line.find_first_not_of(' ');
        if ((line.find("FAIL") != std::string::npos || line.find("failed") != std::string::npos ||
             line.find("ACCESS VIOLATION") != std::string::npos) &&
            line.compare(lead == std::string::npos ? 0 : lead, 7, "result:") != 0)
            why = line.substr(lead == std::string::npos ? 0 : lead);
        start = end + 1;
    }
    return widen(why);
}

}  // namespace

const std::vector<TestGroup>& testGroups()
{
    static const std::vector<TestGroup> groups = {d3d12Tests(), d3d11Tests(), d3d9Tests(), vulkanTests()};
    return groups;
}

const Test* findTest(const std::string& id)
{
    for (const TestGroup& g : testGroups())
        for (const Test& t : g.tests)
            if (id == std::string(g.id) + "." + t.id)
                return &t;
    return nullptr;
}

bool selectTests(const std::vector<std::string>& names, std::vector<std::string>* ids, std::string* unknown)
{
    std::vector<bool> chosen;
    for (const std::string& name : names) {
        bool known = false;
        size_t i = 0;
        for (const TestGroup& g : testGroups())
            for (const Test& t : g.tests) {
                if (name == "all" || name == g.id || name == std::string(g.id) + "." + t.id) {
                    if (chosen.size() <= i)
                        chosen.resize(i + 1);
                    chosen[i] = known = true;
                }
                i++;
            }
        if (!known) {
            *unknown = name;
            return false;
        }
    }
    size_t i = 0;
    for (const TestGroup& g : testGroups())
        for (const Test& t : g.tests) {
            if (i < chosen.size() && chosen[i])
                ids->push_back(std::string(g.id) + "." + t.id);
            i++;
        }
    return true;
}

ChildResult runSelf(const std::wstring& args, DWORD timeoutMs, HANDLE cancel)
{
    ChildResult r;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 1 << 16))
        return r;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi = {};
    // A job, so ending the child ends what it started too (d3d11's shared tests start another process).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    r.started = CreateProcessW(self, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                               nullptr, &si, &pi);
    CloseHandle(write);
    if (r.started) {
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        // The output can outgrow the pipe's buffer: read it while the child runs.
        std::thread reader([&] {
            char buf[4096];
            DWORD got;
            while (ReadFile(read, buf, sizeof(buf), &got, nullptr) && got)
                r.output.append(buf, got);
        });
        HANDLE waits[2] = {pi.hProcess, cancel};
        DWORD w = WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, timeoutMs);
        if (w != WAIT_OBJECT_0) {
            r.timedOut = w == WAIT_TIMEOUT;
            r.cancelled = !r.timedOut;
            TerminateJobObject(job, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        GetExitCodeProcess(pi.hProcess, &r.exitCode);
        TerminateJobObject(job, 1);  // what the child started may still hold the pipe open
        reader.join();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(job);
    CloseHandle(read);
    return r;
}

TestResult runTest(const std::string& id, HANDLE cancel)
{
    LARGE_INTEGER start, end, freq;
    QueryPerformanceCounter(&start);
    ChildResult c = runSelf(L"/run-test " + widen(id) + (testDebugLayer ? L" /debug" : L""), kTestTimeoutMs, cancel);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&freq);
    TestResult r;
    r.seconds = float(end.QuadPart - start.QuadPart) / freq.QuadPart;
    r.output = widen(c.output);
    r.ok = c.started && !c.timedOut && !c.cancelled && c.exitCode == 0;
    if (r.ok)
        return r;
    wchar_t buf[64];
    if (!c.started) {
        r.summary = L"Could not start the test";
    } else if (c.cancelled) {
        r.stopped = true;
        r.summary = L"Stopped";
    } else if (c.timedOut) {
        r.summary = L"Stopped responding after " + std::to_wstring(kTestTimeoutMs / 1000) + L" s";
    } else if (c.exitCode != 1) {
        swprintf(buf, 64, L"Crashed (0x%08lx)", c.exitCode);
        r.summary = buf;
        std::wstring where = failure(c.output);
        if (where.find(L"ACCESS VIOLATION") == 0)
            r.summary += L": " + where;
    } else {
        r.summary = failure(c.output);
        if (r.summary.empty())
            r.summary = L"Failed";
    }
    return r;
}

int runTestChild(const std::string& id)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow the last lines
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // and no error dialog keeps it alive
    AddVectoredExceptionHandler(1, reportCrash);
    const Test* test = findTest(id);
    if (!test) {
        std::printf("no test %s\n", id.c_str());
        return 2;
    }
    bool ok = test->run();
    std::printf("result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace panel
