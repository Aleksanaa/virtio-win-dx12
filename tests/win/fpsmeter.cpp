// SPDX-License-Identifier: MIT
// An always-on-top, click-through overlay with what the screen really shows: flips per second (frames that reached
// the display), VSyncs per second, the longest gap between flips in the last second (which catches the hitches an
// average hides), and mouse reports per second (a dragged window moves only as often as they arrive). Flips and
// VSyncs are dxgkrnl's MMIOFlip and VSyncDPC events from a live ETW session (keyword Present), so they count whatever
// presents (DWM included) and need admin rights (the embedded manifest asks for them); mouse reports come from Raw
// Input.
//   fpsmeter          start (Ctrl+Alt+F quits)
// The numbers lag by up to a second: ETW hands real-time events over in buffers, flushed at least once a second.
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// Microsoft-Windows-DxgKrnl
const GUID kDxgKrnl = {0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
const USHORT kVSyncDpc = 17;
const USHORT kMmioFlip = 116;
const ULONGLONG kPresentKeyword = 0x8000000;
const wchar_t kSession[] = L"vgd-fpsmeter";
const int kHotkey = 1;

std::mutex lock;
std::deque<LONGLONG> flips, vsyncs;  // event times, 100 ns units
std::deque<ULONGLONG> inputs;        // mouse input arrival, GetTickCount64 ms
LONGLONG newest = 0;

void WINAPI on_event(EVENT_RECORD* record)
{
    if (!IsEqualGUID(record->EventHeader.ProviderId, kDxgKrnl))
        return;
    USHORT id = record->EventHeader.EventDescriptor.Id;
    if (id != kMmioFlip && id != kVSyncDpc)
        return;
    LONGLONG t = record->EventHeader.TimeStamp.QuadPart;
    std::lock_guard g(lock);
    (id == kMmioFlip ? flips : vsyncs).push_back(t);
    newest = std::max(newest, t);
}

// The trace properties followed by room for the session name, as StartTrace wants them.
struct Properties {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[64];
};

Properties make_properties()
{
    Properties props = {};
    props.p.Wnode.BufferSize = sizeof(props);
    props.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props.p.Wnode.ClientContext = 1;  // QueryPerformanceCounter-free system time stamps
    props.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props.p.BufferSize = 4;           // KB: small buffers fill and reach us sooner
    props.p.MinimumBuffers = 4;
    props.p.FlushTimer = 1;           // seconds
    props.p.LoggerNameOffset = offsetof(Properties, name);
    return props;
}

TRACEHANDLE session = 0, consumer = INVALID_PROCESSTRACE_HANDLE;

DWORD start_trace()
{
    Properties stop = make_properties();
    ControlTraceW(0, kSession, &stop.p, EVENT_TRACE_CONTROL_STOP);  // a session left by an earlier run
    Properties props = make_properties();
    ULONG err = StartTraceW(&session, kSession, &props.p);
    if (err != ERROR_SUCCESS)
        return err;
    err = EnableTraceEx2(session, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                         kPresentKeyword, 0, 0, nullptr);
    if (err != ERROR_SUCCESS)
        return err;
    EVENT_TRACE_LOGFILEW log = {};
    log.LoggerName = const_cast<wchar_t*>(kSession);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = on_event;
    consumer = OpenTraceW(&log);
    if (consumer == INVALID_PROCESSTRACE_HANDLE)
        return GetLastError();
    std::thread([] { ProcessTrace(&consumer, 1, nullptr, nullptr); }).detach();
    return ERROR_SUCCESS;
}

void stop_trace()
{
    if (consumer != INVALID_PROCESSTRACE_HANDLE)
        CloseTrace(consumer);
    Properties props = make_properties();
    ControlTraceW(session, nullptr, &props.p, EVENT_TRACE_CONTROL_STOP);
}

// The last second, ending at the newest event: flips, VSyncs and the longest gap between flips, in ms.
struct Stats {
    int flips, vsyncs;
    int inputs;
    double worst_ms;
};

Stats sample()
{
    std::lock_guard g(lock);
    LONGLONG from = newest - 10000000;
    while (!flips.empty() && flips.front() < from - 10000000)
        flips.pop_front();
    while (!vsyncs.empty() && vsyncs.front() < from)
        vsyncs.pop_front();
    ULONGLONG now = GetTickCount64();
    while (!inputs.empty() && inputs.front() < now - 1000)
        inputs.pop_front();
    Stats s = {0, static_cast<int>(vsyncs.size()), static_cast<int>(inputs.size()), 0};
    LONGLONG prev = 0;
    for (LONGLONG t : flips) {
        if (t >= from) {
            s.flips++;
            if (prev)
                s.worst_ms = std::max(s.worst_ms, (t - prev) / 1e4);
        }
        prev = t;
    }
    return s;
}

wchar_t text[128] = L"fpsmeter: waiting for events";
bool bad = false;

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER: {
        Stats s = sample();
        std::swprintf(text, 128, L"%d fps   vsync %d   worst %.0f ms   input %d/s", s.flips, s.vsyncs, s.worst_ms,
                      s.inputs);
        // A gap over 1.5 frames at the VSync rate is a visible hitch.
        bad = s.vsyncs > 0 && s.worst_ms > 1500.0 / s.vsyncs;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH back = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &rc, back);
        DeleteObject(back);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, bad ? RGB(255, 96, 96) : RGB(120, 255, 120));
        HFONT font = CreateFontW(-MulDiv(12, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        HGDIOBJ old = SelectObject(dc, font);
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_INPUT: {
        // Mouse reports with a position (every report of an absolute tablet): a dragged window moves only as often as
        // these arrive.
        RAWINPUT raw;
        UINT size = sizeof(raw);
        UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
        if (got != UINT(-1) && raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.lLastX || raw.data.mouse.lLastY)) {
            std::lock_guard g(lock);
            inputs.push_back(GetTickCount64());
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_HOTKEY:
        if (wp == kHotkey)
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DWORD err = start_trace();
    if (err != ERROR_SUCCESS) {
        wchar_t msg[160];
        std::swprintf(msg, 160, L"Cannot start the ETW session (error %lu).%s", err,
                      err == ERROR_ACCESS_DENIED ? L" Run it as administrator." : L"");
        MessageBoxW(nullptr, msg, L"fpsmeter", MB_ICONERROR);
        stop_trace();
        return 1;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = L"vgd-fpsmeter";
    RegisterClassW(&wc);
    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                    WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"fpsmeter", WS_POPUP, MulDiv(8, dpi, 96), MulDiv(8, dpi, 96),
                                MulDiv(440, dpi, 96), MulDiv(28, dpi, 96), nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(hwnd, 0, 210, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RAWINPUTDEVICE mouse = {0x01, 0x02, RIDEV_INPUTSINK, hwnd};  // generic desktop, mouse; also in the background
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    RegisterHotKey(hwnd, kHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    SetTimer(hwnd, 1, 250, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stop_trace();
    return 0;
}
