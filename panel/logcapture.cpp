// SPDX-License-Identifier: MIT
// Capturing what the drivers log, as DebugView does but without its driver:
//  - user mode: the UMDs (and Mesa's Venus ICD) log with OutputDebugString, which hands each string to whoever owns
//    the DBWIN_BUFFER section and its two events. This session's are open to any user; the Global\ ones (session 0:
//    services) need administrator rights.
//  - kernel: the KMD logs with DbgPrintEx. The kernel passes DbgPrint calls to ETW sessions that ask for them
//    (EVENT_TRACE_FLAG_DBGPRINT); a session of the system logger kind does not take the one NT Kernel Logger, so it
//    runs beside WPR or xperf. Starting one needs administrator rights. The KMD's prints carry the error bit in their
//    level mask, so the default Debug Print Filter lets them through.
#include "panel.h"

#include <evntcons.h>
#include <evntrace.h>
#include <sddl.h>

#include <cstring>
#include <mutex>

namespace panel {

namespace {

// wmiguid.h's DbgPrintGuid: the class of the kernel's DbgPrint events. Their data is ComponentId, Level (ULONGs),
// then the message (ANSI).
constexpr GUID kDbgPrintGuid = {0x13976d09, 0xa327, 0x438c, {0x95, 0x0b, 0x7f, 0x03, 0x19, 0x28, 0x15, 0xc7}};
constexpr wchar_t kSessionName[] = L"venera-panel kernel messages";

std::wstring fromAnsi(const char* s, size_t n)
{
    std::wstring w(MultiByteToWideChar(CP_ACP, 0, s, int(n), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, int(n), w.data(), int(w.size()));
    return w;
}

// "venera_d3d12[4120:4188] W body" after the UMDs' time stamp (umd/common/log.cpp), or Mesa's "MESA: warning: body":
// the component and the level, and the body left in `text`.
void parseDriverLine(LogLine& l)
{
    const std::wstring& s = l.text;
    if (s.size() > 13 && s[2] == L':' && s[5] == L':' && s[8] == L'.' && s[12] == L' ') {
        size_t open = s.find(L'[', 13), close = s.find(L"] ", 13);
        if (open != std::wstring::npos && close != std::wstring::npos && open < close && close + 3 < s.size() &&
            s[close + 3] == L' ') {
            const wchar_t* levels = L"WE";
            const wchar_t* lv = std::wcschr(levels, s[close + 2]);
            l.component = s.substr(13, open - 13);
            l.level = lv ? int(lv - levels) + 1 : 0;
            l.text = s.substr(close + 4);
            return;
        }
    }
    if (s.compare(0, 6, L"MESA: ") == 0) {
        l.component = L"mesa";
        l.text = s.substr(6);
        const wchar_t* levels[] = {L"warning: ", L"error: "};
        for (int i = 0; i < 2; i++)
            if (l.text.compare(0, wcslen(levels[i]), levels[i]) == 0) {
                l.level = i + 1;
                l.text.erase(0, wcslen(levels[i]));
            }
        for (const wchar_t* info : {L"info: ", L"debug: "})
            if (l.text.compare(0, wcslen(info), info) == 0)
                l.text.erase(0, wcslen(info));
    }
}

}  // namespace

std::wstring formatLogLine(const LogLine& l)
{
    wchar_t time[16];
    swprintf(time, 16, L"%02u:%02u:%02u.%03u", l.time.wHour, l.time.wMinute, l.time.wSecond, l.time.wMilliseconds);
    std::wstring s = time + std::wstring(L"  ") + l.process;
    if (l.pid)
        s += L"[" + std::to_wstring(l.pid) + L"]";
    if (!l.component.empty() && l.component != l.process)
        s += L" " + l.component;
    return s + L" " + L"IWE"[l.level] + L" " + l.text;
}

bool elevated()
{
    HANDLE token;
    TOKEN_ELEVATION e = {};
    DWORD size;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
    CloseHandle(token);
    return e.TokenIsElevated;
}

LogCapture::~LogCapture() { stop(); }

void LogCapture::emit(LogLine&& l)
{
    std::lock_guard<std::mutex> lock(sinkMutex_);
    sink_(std::move(l));
}

std::wstring LogCapture::processName(DWORD pid)
{
    std::lock_guard<std::mutex> lock(namesMutex_);
    auto it = names_.find(pid);
    if (it != names_.end())
        return it->second;
    std::wstring name = L"pid " + std::to_wstring(pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            name = path;
            name.erase(0, name.find_last_of(L'\\') + 1);
            if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
                name.resize(name.size() - 4);
        }
        CloseHandle(p);
    }
    return names_[pid] = name;
}

bool LogCapture::openDbwin(Dbwin& d, const wchar_t* prefix, std::wstring* error)
{
    // Writers run as any user and at any integrity level (DWM, browsers' sandboxed GPU processes).
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AN)S:(ML;;NW;;;S-1-16-0)",
                                                         SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    std::wstring p = prefix;
    d.bufferReady = CreateEventW(&sa, FALSE, FALSE, (p + L"DBWIN_BUFFER_READY").c_str());
    DWORD err = GetLastError();
    if (d.bufferReady && err != ERROR_ALREADY_EXISTS) {
        d.dataReady = CreateEventW(&sa, FALSE, FALSE, (p + L"DBWIN_DATA_READY").c_str());
        d.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, (p + L"DBWIN_BUFFER").c_str());
        if (d.section)
            d.view = static_cast<const char*>(MapViewOfFile(d.section, FILE_MAP_READ, 0, 0, 4096));
    }
    LocalFree(sd);
    if (d.view && d.dataReady)
        return true;
    if (error)
        *error = err == ERROR_ALREADY_EXISTS ? L"Another program, such as DebugView, is reading debug output"
                                             : L"Could not read debug output (error " + std::to_wstring(err) + L")";
    closeDbwin(d);
    return false;
}

void LogCapture::closeDbwin(Dbwin& d)
{
    if (d.thread.joinable())
        d.thread.join();
    if (d.view)
        UnmapViewOfFile(d.view);
    for (HANDLE h : {d.section, d.dataReady, d.bufferReady})
        if (h)
            CloseHandle(h);
    d = {};
}

void LogCapture::readDbwin(Dbwin& d)
{
    HANDLE waits[2] = {d.dataReady, stop_};
    for (;;) {
        SetEvent(d.bufferReady);
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
            return;
        DWORD pid;
        std::memcpy(&pid, d.view, 4);
        const char* s = d.view + 4;
        std::wstring text = fromAnsi(s, strnlen(s, 4092));
        SYSTEMTIME now;
        GetLocalTime(&now);
        std::wstring process = processName(pid);
        // One string may hold several lines.
        for (size_t start = 0; start < text.size();) {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos)
                end = text.size();
            LogLine l;
            l.time = now;
            l.pid = pid;
            l.process = process;
            l.text = text.substr(start, end - start);
            while (!l.text.empty() && (l.text.back() == L'\r' || l.text.back() == L' '))
                l.text.pop_back();
            start = end + 1;
            if (l.text.empty())
                continue;
            parseDriverLine(l);
            emit(std::move(l));
        }
    }
}

bool LogCapture::startKernel(std::wstring* error)
{
    std::vector<BYTE> buf(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName));
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    auto reset = [&] {
        std::fill(buf.begin(), buf.end(), 0);
        props->Wnode.BufferSize = ULONG(buf.size());
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    };
    reset();
    ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);  // one a panel that crashed left running
    reset();
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 2;  // system time
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_DBGPRINT;
    props->FlushTimer = 1;
    ULONG err = StartTraceW(&session_, kSessionName, props);
    if (err != ERROR_SUCCESS) {
        *error = err == ERROR_ACCESS_DENIED ? L"Kernel messages need administrator rights"
                                            : L"Could not start reading kernel messages (error " +
                                                  std::to_wstring(err) + L")";
        session_ = 0;
        return false;
    }
    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = const_cast<wchar_t*>(kSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = [](EVENT_RECORD* r) { static_cast<LogCapture*>(r->UserContext)->kernelEvent(r); };
    lf.Context = this;
    trace_ = OpenTraceW(&lf);
    if (trace_ == INVALID_PROCESSTRACE_HANDLE) {
        *error = L"Could not read kernel messages (error " + std::to_wstring(GetLastError()) + L")";
        stopKernel();
        return false;
    }
    kernelThread_ = std::thread([this] { ProcessTrace(&trace_, 1, nullptr, nullptr); });
    return true;
}

void LogCapture::stopKernel()
{
    if (session_) {
        std::vector<BYTE> buf(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName));
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = ULONG(buf.size());
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(session_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    if (trace_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(trace_);
        trace_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (kernelThread_.joinable())
        kernelThread_.join();
}

void LogCapture::kernelEvent(EVENT_RECORD* r)
{
    if (!IsEqualGUID(r->EventHeader.ProviderId, kDbgPrintGuid) || r->UserDataLength <= 8)
        return;
    const char* s = static_cast<const char*>(r->UserData) + 8;
    LogLine l;
    FILETIME utc = {r->EventHeader.TimeStamp.LowPart, DWORD(r->EventHeader.TimeStamp.HighPart)}, local;
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &l.time);
    l.process = L"kernel";
    l.component = L"kernel";
    l.text = fromAnsi(s, strnlen(s, r->UserDataLength - 8));
    while (!l.text.empty() && (l.text.back() == L'\n' || l.text.back() == L'\r' || l.text.back() == L' '))
        l.text.pop_back();
    if (!l.text.empty())
        emit(std::move(l));
}

void LogCapture::start(std::function<void(LogLine&&)> sink)
{
    stop();
    sink_ = std::move(sink);
    userError.clear();
    kernelError.clear();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (openDbwin(local_, L"", &userError))
        local_.thread = std::thread([this] { readDbwin(local_); });
    if (elevated() && openDbwin(global_, L"Global\\", nullptr))
        global_.thread = std::thread([this] { readDbwin(global_); });
    startKernel(&kernelError);
    running_ = true;
}

void LogCapture::stop()
{
    if (!running_)
        return;
    SetEvent(stop_);
    closeDbwin(local_);
    closeDbwin(global_);
    stopKernel();
    CloseHandle(stop_);
    stop_ = nullptr;
    running_ = false;
}

}  // namespace panel
