// SPDX-License-Identifier: MIT
// Stacks of every thread in the process, for diagnosing hangs where there is no debugger (DWM at boot). Each
// thread is suspended only while its frames are walked; nothing allocates meanwhile, since the suspended thread may
// hold the heap lock. Frames are module+offset, resolved offline (our DLLs and the ICD carry their symbols; system
// DLLs by their exports).
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace vgd::umd {

namespace {

constexpr unsigned kMaxThreads = 256;
constexpr unsigned kMaxFrames = 32;

struct ThreadStack {
    DWORD id;
    unsigned frames;
    DWORD64 pc[kMaxFrames];
    wchar_t name[32];
};

ThreadStack g_stacks[kMaxThreads];

// Walks by the unwind data in .pdata; a frame without it is a leaf only at the top, where the return address is at
// the stack pointer. Stops at the first frame it cannot unwind.
unsigned walk(CONTEXT ctx, DWORD64* pc)
{
    unsigned n = 0;
    while (n < kMaxFrames && ctx.Rip) {
        pc[n++] = ctx.Rip;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (fn) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
        } else if (n == 1) {
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            break;
        }
    }
    return n;
}

void format_frame(DWORD64 pc, char* out, size_t size)
{
    HMODULE mod = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &mod) &&
        GetModuleFileNameA(mod, path, sizeof(path))) {
        const char* base = std::strrchr(path, '\\');
        std::snprintf(out, size, "%s+0x%llx", base ? base + 1 : path,
                      static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(mod)));
    } else {
        std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(pc));
    }
}

// MinGW-w64 raises C++ throws as this SEH code (libgcc's STATUS_GCC_THROW).
constexpr DWORD kGccThrow = 0x20474343;

void log_frames(const char* what, const CONTEXT& ctx)
{
    DWORD64 pc[kMaxFrames];
    const unsigned n = walk(ctx, pc);
    char line[900];
    int len = std::snprintf(line, sizeof(line), "%s:", what);
    for (unsigned f = 0; f < n; f++) {
        char frame[96];
        format_frame(pc[f], frame, sizeof(frame));
        if (len + std::strlen(frame) + 2 >= sizeof(line)) {
            logf(LogLevel::warn, "  %s", line);
            len = std::snprintf(line, sizeof(line), "  ...:");
        }
        len += std::snprintf(line + len, sizeof(line) - len, " %s", frame);
    }
    logf(LogLevel::warn, "  %s", line);
}

// First-chance C++ throws and access violations, logged where they are raised, then left to their handlers.
// DXVK's threads catch everything and exit quietly (dxvk::thread::threadProc), so a throw on its CS thread
// otherwise leaves only a hang.
LONG CALLBACK log_exception(EXCEPTION_POINTERS* e)
{
    static std::atomic<unsigned> count{0};
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code != kGccThrow && code != EXCEPTION_ACCESS_VIOLATION) || count.fetch_add(1) >= 16)
        return EXCEPTION_CONTINUE_SEARCH;
    using GetThreadDescriptionProc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionProc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    wchar_t name[32] = L"";
    PWSTR desc = nullptr;
    if (get_description && SUCCEEDED(get_description(GetCurrentThread(), &desc)) && desc) {
        wcsncpy(name, desc, ARRAYSIZE(name) - 1);
        LocalFree(desc);
    }
    char what[128];
    std::snprintf(what, sizeof(what), "%s on thread %lu %ls",
                  code == kGccThrow ? "C++ exception thrown" : "access violation", GetCurrentThreadId(), name);
    logf(LogLevel::warn, "%s", what);
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        // What was accessed, and the registers that usually hold the pointer involved.
        const ULONG_PTR kind = e->ExceptionRecord->ExceptionInformation[0];
        const CONTEXT& c = *e->ContextRecord;
        logf(LogLevel::warn, "  %s 0x%llx; rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx",
             kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
             static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]),
             static_cast<unsigned long long>(c.Rax), static_cast<unsigned long long>(c.Rbx),
             static_cast<unsigned long long>(c.Rcx), static_cast<unsigned long long>(c.Rdx),
             static_cast<unsigned long long>(c.Rsi), static_cast<unsigned long long>(c.Rdi),
             static_cast<unsigned long long>(c.R8), static_cast<unsigned long long>(c.R9));
    }
    log_frames("at", *e->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace {
std::atomic<void*> g_handler{nullptr};
} // namespace

void log_exceptions()
{
    void* expected = nullptr;
    if (g_handler.load())
        return;
    void* h = AddVectoredExceptionHandler(0, log_exception);
    if (!g_handler.compare_exchange_strong(expected, h))
        RemoveVectoredExceptionHandler(h);
}

void log_thread_stacks(const char* why)
{
    using GetThreadDescriptionProc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionProc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));

    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    unsigned count = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok && count < kMaxThreads; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        ThreadStack& s = g_stacks[count];
        s = {};
        s.id = te.th32ThreadID;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (s.id == self) {
            RtlCaptureContext(&ctx);
            s.frames = walk(ctx, s.pc);
        } else {
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                  s.id);
            if (!t)
                continue;
            if (get_description) {
                PWSTR desc = nullptr;
                if (SUCCEEDED(get_description(t, &desc)) && desc) {
                    wcsncpy(s.name, desc, ARRAYSIZE(s.name) - 1);
                    LocalFree(desc);
                }
            }
            if (SuspendThread(t) != static_cast<DWORD>(-1)) {
                if (GetThreadContext(t, &ctx))
                    s.frames = walk(ctx, s.pc);
                ResumeThread(t);
            }
            CloseHandle(t);
        }
        count++;
    }
    CloseHandle(snap);

    logf(LogLevel::warn, "stacks (%s): %u threads", why, count);
    for (unsigned i = 0; i < count; i++) {
        const ThreadStack& s = g_stacks[i];
        char line[900];
        int len = std::snprintf(line, sizeof(line), "%lu%s %ls:", s.id, s.id == self ? " (this)" : "", s.name);
        for (unsigned f = 0; f < s.frames; f++) {
            char frame[96];
            format_frame(s.pc[f], frame, sizeof(frame));
            // Long stacks continue on further lines.
            if (len + std::strlen(frame) + 2 >= sizeof(line)) {
                logf(LogLevel::warn, "  %s", line);
                len = std::snprintf(line, sizeof(line), "%lu ...:", s.id);
            }
            len += std::snprintf(line + len, sizeof(line) - len, " %s", frame);
        }
        logf(LogLevel::warn, "  %s", line);
    }
}

} // namespace vgd::umd
