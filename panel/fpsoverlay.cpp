// SPDX-License-Identifier: MIT
// The frame rate overlay (vgd-panel /fps): an always-on-top, click-through card in the screen's top-left corner with
// what the screen really shows: flips per second (frames that reached the display), VSyncs per second, the longest
// gap between flips in the last second (which catches the hitches an average hides), and mouse reports per second (a
// dragged window moves only as often as they arrive). Flips and VSyncs are dxgkrnl's MMIOFlip and VSyncDPC events
// from a live ETW session (keyword Present), so they count whatever presents, DWM included, and need administrator
// rights; mouse reports come from Raw Input. Ctrl+Alt+F closes it, as does WM_CLOSE from any process (/fps off, the
// Tools page).
// Below them, each program that presents with its frame rate: its presents per second, from the Present_Start events
// dxgi.dll and d3d9.dll log in its process. Every API ends in one of these here, Vulkan too (Mesa's WSI presents
// through a DXGI swap chain); in-game counters and PresentMon count the same.
// The numbers lag by up to a second: ETW hands real-time events over in buffers, flushed at least once a second.
#include "panel.h"

#include <evntcons.h>
#include <evntrace.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

namespace panel {

using namespace theme;

namespace {

// Microsoft-Windows-DxgKrnl
const GUID kDxgKrnl = {0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
constexpr USHORT kVSyncDpc = 17, kMmioFlip = 116;
constexpr ULONGLONG kPresentKeyword = 0x8000000;
// Microsoft-Windows-DXGI and Microsoft-Windows-D3D9, and their Present_Start events
const GUID kDxgi = {0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}};
const GUID kD3d9 = {0x783aca0a, 0x790e, 0x4d7f, {0x84, 0x51, 0xaa, 0x85, 0x05, 0x11, 0xc6, 0xb9}};
constexpr USHORT kDxgiPresent = 42, kD3d9Present = 1;
// evntprov.h's event id filter, which mingw's lacks
constexpr ULONG kFilterEventId = 0x80000200;  // EVENT_FILTER_TYPE_EVENT_ID
struct EventIdFilter {
    BOOLEAN filterIn;
    UCHAR reserved;
    USHORT count;
    USHORT events[1];
};
constexpr wchar_t kSession[] = L"vgd-fps-overlay", kClass[] = L"vgd-fps-overlay";
constexpr int kHotkey = 1;
// The card, in DIPs: columns of a value over its label, each as wide as the widest it shows, the same space between
// them and at the sides; below a line, a row for each program that presents, the busiest first.
constexpr float kGap = 24, kH = 60, kRowH = 22, kMargin = 8;
constexpr size_t kMaxRows = 10;
const wchar_t* const kLabels[] = {L"FPS", L"VSync", L"Worst gap", L"Mouse/s"};
const wchar_t* const kWidest[] = {L"000", L"000", L"000 ms", L"0000"};

std::mutex lock;
std::deque<LONGLONG> flips, vsyncs;  // event times, 100 ns units
std::deque<ULONGLONG> inputs;        // mouse reports' arrival, GetTickCount64 ms
std::map<DWORD, std::deque<LONGLONG>> presents;  // by process
LONGLONG newest = 0;

void WINAPI onEvent(EVENT_RECORD* record)
{
    const GUID& provider = record->EventHeader.ProviderId;
    USHORT id = record->EventHeader.EventDescriptor.Id;
    bool present =
        (IsEqualGUID(provider, kDxgi) && id == kDxgiPresent) || (IsEqualGUID(provider, kD3d9) && id == kD3d9Present);
    if (!present && (!IsEqualGUID(provider, kDxgKrnl) || (id != kMmioFlip && id != kVSyncDpc)))
        return;
    LONGLONG t = record->EventHeader.TimeStamp.QuadPart;
    std::lock_guard g(lock);
    if (present)
        presents[record->EventHeader.ProcessId].push_back(t);
    else
        (id == kMmioFlip ? flips : vsyncs).push_back(t);
    newest = std::max(newest, t);
}

// The trace properties followed by room for the session name, as StartTrace wants them.
struct Properties {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[64];
};

Properties makeProperties()
{
    Properties props = {};
    props.p.Wnode.BufferSize = sizeof(props);
    props.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props.p.Wnode.ClientContext = 1;  // QueryPerformanceCounter, which ProcessTrace hands over as 100 ns system time
    props.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props.p.BufferSize = 4;  // KB: small buffers fill and reach us sooner
    props.p.MinimumBuffers = 4;
    props.p.FlushTimer = 1;  // seconds
    props.p.LoggerNameOffset = offsetof(Properties, name);
    return props;
}

TRACEHANDLE session = 0, consumer = INVALID_PROCESSTRACE_HANDLE;

DWORD startTrace()
{
    Properties stop = makeProperties();
    ControlTraceW(0, kSession, &stop.p, EVENT_TRACE_CONTROL_STOP);  // one an overlay that crashed left running
    Properties props = makeProperties();
    ULONG err = StartTraceW(&session, kSession, &props.p);
    if (err != ERROR_SUCCESS)
        return err;
    err = EnableTraceEx2(session, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, kPresentKeyword,
                         0, 0, nullptr);
    if (err != ERROR_SUCCESS)
        return err;
    // Of these two, only the presents: they log much more. Without them there is no list, but the rest works.
    for (auto [provider, id] : {std::pair{&kDxgi, kDxgiPresent}, std::pair{&kD3d9, kD3d9Present}}) {
        EventIdFilter filter = {TRUE, 0, 1, {id}};
        EVENT_FILTER_DESCRIPTOR desc = {ULONGLONG(&filter), sizeof(filter), kFilterEventId};
        ENABLE_TRACE_PARAMETERS params = {ENABLE_TRACE_PARAMETERS_VERSION_2};
        params.EnableFilterDesc = &desc;
        params.FilterDescCount = 1;
        EnableTraceEx2(session, provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, ~0ull, 0, 0, &params);
    }
    EVENT_TRACE_LOGFILEW log = {};
    log.LoggerName = const_cast<wchar_t*>(kSession);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = onEvent;
    consumer = OpenTraceW(&log);
    if (consumer == INVALID_PROCESSTRACE_HANDLE)
        return GetLastError();
    std::thread([] { ProcessTrace(&consumer, 1, nullptr, nullptr); }).detach();
    return ERROR_SUCCESS;
}

void stopTrace()
{
    if (consumer != INVALID_PROCESSTRACE_HANDLE)
        CloseTrace(consumer);
    Properties props = makeProperties();
    ControlTraceW(session, nullptr, &props.p, EVENT_TRACE_CONTROL_STOP);
}

// The last second, ending at the newest event: flips, VSyncs, the longest gap between flips in ms, and the presents
// of each process that presented, the most first.
struct Stats {
    int flips, vsyncs, inputs;
    std::vector<std::pair<int, DWORD>> apps;  // presents, process
    double worstMs;
    bool any;  // an event came: until then there is nothing to show
};

Stats sample()
{
    std::lock_guard g(lock);
    LONGLONG from = newest - 10000000;
    for (auto p = presents.begin(); p != presents.end();) {
        while (!p->second.empty() && p->second.front() < from)
            p->second.pop_front();
        p = p->second.empty() ? presents.erase(p) : std::next(p);
    }
    while (!flips.empty() && flips.front() < from - 10000000)
        flips.pop_front();
    while (!vsyncs.empty() && vsyncs.front() < from)
        vsyncs.pop_front();
    ULONGLONG now = GetTickCount64();
    while (!inputs.empty() && inputs.front() < now - 1000)
        inputs.pop_front();
    Stats s = {0, int(vsyncs.size()), int(inputs.size()), {}, 0, newest != 0};
    for (const auto& [pid, times] : presents)
        s.apps.push_back({int(times.size()), pid});
    std::sort(s.apps.begin(), s.apps.end(), std::greater<>());
    if (s.apps.size() > kMaxRows)
        s.apps.resize(kMaxRows);
    LONGLONG prev = 0;
    for (LONGLONG t : flips) {
        if (t >= from) {
            s.flips++;
            if (prev)
                s.worstMs = std::max(s.worstMs, (t - prev) / 1e4);
        }
        prev = t;
    }
    return s;
}

// The window, drawn into a DIB with Direct2D and handed to UpdateLayeredWindow, for the card's rounded corners and
// translucency.
class Overlay {
public:
    int run();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void place();
    void paint();
    std::wstring appName(DWORD pid);

    HWND hwnd_ = nullptr;
    Ui ui_;
    ID2D1Factory* d2d_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    float scale_ = 1;
    POINT pos_ = {};
    SIZE size_ = {};
    Stats stats_ = {};
    float h_ = kH;
    std::map<DWORD, std::wstring> names_;
    float columnW_[4] = {}, w_ = 0;
};

Overlay* g_overlay;

// A process's file name without .exe, shortened with an ellipsis to fit beside its frame rate.
std::wstring Overlay::appName(DWORD pid)
{
    auto known = names_.find(pid);
    if (known != names_.end())
        return known->second;
    // From the process list: some system processes do not let even administrators open them.
    std::wstring name;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok && name.empty(); ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == pid)
            name = e.szExeFile;
    CloseHandle(snap);
    if (name.empty())  // gone: its id, and no name kept
        return L"Process " + std::to_wstring(pid);
    if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".exe"))
        name.resize(name.size() - 4);
    float room = w_ - 2 * kGap - ui_.measure(L"0000", Ui::kBodyStrong) - 12;
    if (ui_.measure(name, Ui::kBody) > room) {
        while (!name.empty() && ui_.measure(name + L"…", Ui::kBody) > room)
            name.pop_back();
        name += L"…";
    }
    if (names_.size() > 256)  // ids are reused: forget them now and then
        names_.clear();
    return names_[pid] = name;
}

// In the primary monitor's work area, so the taskbar does not cover it, at its DPI.
void Overlay::place()
{
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    scale_ = GetDpiForWindow(hwnd_) / 96.f;
    pos_ = {work.left + LONG(kMargin * scale_), work.top + LONG(kMargin * scale_)};
    size_ = {LONG(std::ceil(w_ * scale_)), LONG(std::ceil(h_ * scale_))};
    if (bitmap_)
        DeleteObject(bitmap_);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), size_.cx, -size_.cy, 1, 32, BI_RGB}};
    void* bits;
    bitmap_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(dc_, bitmap_);
}

void Overlay::paint()
{
    RECT rc = {0, 0, size_.cx, size_.cy};
    rt_->BindDC(dc_, &rc);
    ui_.begin(rt_);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    D2D1_RECT_F card = {0.5f, 0.5f, w_ - 0.5f, h_ - 0.5f};
    ui_.fill(card, 8, kBase, 0.88f);
    ui_.stroke(card, 8, kWhite, kControlStroke);
    // A gap over 1.5 frames at the VSync rate is a visible hitch.
    bool hitch = stats_.vsyncs > 0 && stats_.worstMs > 1500.0 / stats_.vsyncs;
    std::wstring values[] = {std::to_wstring(stats_.flips), std::to_wstring(stats_.vsyncs),
                             std::to_wstring(int(stats_.worstMs + 0.5)) + L" ms", std::to_wstring(stats_.inputs)};
    float x = kGap;
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F value = {x, 8, x + columnW_[i], 34}, label = {x, 34, x + columnW_[i], 52};
        ui_.text(stats_.any ? values[i] : L"–", Ui::kSubtitle, value, i == 2 && hitch ? kCritical : kWhite, 1,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        ui_.text(kLabels[i], Ui::kCaption, label, kWhite, kTextSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        x += columnW_[i] + kGap;
    }
    float y = kH;
    if (!stats_.apps.empty()) {
        ui_.fill({kGap, y - 0.5f, w_ - kGap, y + 0.5f}, 0, kWhite, kControlStroke);
        y += 6;
    }
    for (const auto& [count, pid] : stats_.apps) {
        ui_.text(appName(pid), Ui::kBody, {kGap, y, w_ - kGap, y + kRowH}, kWhite, kTextSecondary);
        ui_.text(std::to_wstring(count), Ui::kBodyStrong, {kGap, y, w_ - kGap, y + kRowH}, kWhite, 1,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += kRowH;
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        brush_->Release();
        rt_->Release();
        rt_ = nullptr;
        return;
    }
    POINT zero = {};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &pos_, &size_, dc_, &zero, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK Overlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay* o = g_overlay;
    switch (msg) {
    case WM_TIMER:
        o->stats_ = sample();
        if (float h = kH + (o->stats_.apps.empty() ? 0 : 6 + o->stats_.apps.size() * kRowH + 8); h != o->h_) {
            o->h_ = h;
            o->place();
        }
        if (!o->rt_) {
            auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            o->d2d_->CreateDCRenderTarget(&props, &o->rt_);
            o->rt_->CreateSolidColorBrush(D2D1::ColorF(kWhite), &o->brush_);
            o->rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs an opaque target
            o->ui_.setBrush(o->brush_);
        }
        o->paint();
        return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:  // the work area
        o->place();
        o->paint();
        return 0;
    case WM_INPUT: {
        // Mouse reports with a position (every report of an absolute tablet).
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

int Overlay::run()
{
    g_overlay = this;
    ui_.init();
    w_ = kGap;
    for (int i = 0; i < 4; i++) {
        columnW_[i] = std::max(ui_.measure(kLabels[i], Ui::kCaption), ui_.measure(kWidest[i], Ui::kSubtitle));
        w_ += columnW_[i] + kGap;
    }
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_);
    dc_ = CreateCompatibleDC(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kClass, L"Frame rate overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance,
                            nullptr);
    // The panel that turns it off may run without administrator rights, and UIPI drops its messages otherwise.
    ChangeWindowMessageFilterEx(hwnd_, WM_CLOSE, MSGFLT_ALLOW, nullptr);
    place();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    RAWINPUTDEVICE mouse = {0x01, 0x02, RIDEV_INPUTSINK, hwnd_};  // generic desktop, mouse; also in the background
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    RegisterHotKey(hwnd_, kHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    SetTimer(hwnd_, 1, 250, nullptr);
    SendMessageW(hwnd_, WM_TIMER, 1, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_overlay = nullptr;
    return 0;
}

}  // namespace

std::wstring runFpsOverlay()
{
    if (findFpsOverlay())
        return L"The frame rate overlay is already on";
    DWORD err = startTrace();
    if (err != ERROR_SUCCESS) {
        stopTrace();
        return L"Cannot start the ETW session (error " + std::to_wstring(err) + L")" +
               (err == ERROR_ACCESS_DENIED ? L"; it needs administrator rights" : L"");
    }
    Overlay overlay;
    overlay.run();
    stopTrace();
    return L"";
}

HWND findFpsOverlay() { return FindWindowW(kClass, nullptr); }

}  // namespace panel
