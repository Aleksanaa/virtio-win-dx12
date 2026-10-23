// SPDX-License-Identifier: MIT
// Replays the same desktop interactions on every build, so UI smoothness can be compared between builds instead of
// judged by feel. Each scenario runs for a fixed time; for each one it writes its start and end (system time, the
// clock ETW timestamps use, so a trace taken meanwhile can be cut into scenarios), the frames DWM composed, and the
// CPU time used by DWM and by the whole system.
//   uibench [--out FILE] [scenario...]
// Scenarios (all of them, in this order, by default):
//   idle    nothing for 5 s: what DWM costs on a still desktop
//   cursor  the cursor circles at 500 Hz for 10 s, through SendInput like a real mouse
//   drag    a window moves back and forth for 10 s
//   anim    a window is minimized and restored, then windows are opened and closed, 5 times each
//   start   the Start menu is opened and closed 3 times
// Run it in the interactive user's session (input and DWM animations need the user's desktop).
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

FILE* out = stdout;
const wchar_t kClass[] = L"vgd-uibench";

uint64_t now_filetime()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t to_u64(FILETIME ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

double seconds_since(LARGE_INTEGER start)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
}

// Handles messages for up to ms milliseconds, so our windows paint and animate while a scenario waits.
void pump_for(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DWORD t = GetTickCount();
        if (static_cast<int32_t>(end - t) <= 0)
            return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, end - t, QS_ALLINPUT);
    }
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(40, 90, 160));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, L"vgd uibench", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND make_window(int x, int y)
{
    HWND hwnd = CreateWindowExW(0, kClass, L"vgd uibench", WS_OVERLAPPEDWINDOW, x, y, 800, 500, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    return hwnd;
}

DWORD dwm_pid()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0) {
            pid = pe.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

// What a scenario is measured by, taken before and after it.
struct Sample {
    uint64_t time;
    uint64_t frames, refreshes;
    uint64_t sys_idle, sys_kernel, sys_user;  // kernel includes idle
    uint64_t dwm_cpu;                         // 0 when DWM's times cannot be read
};

HANDLE dwm = nullptr;

Sample sample()
{
    Sample s = {};
    s.time = now_filetime();
    DWM_TIMING_INFO info = {};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info))) {
        s.frames = info.cFrame;
        s.refreshes = info.cRefresh;
    }
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    s.sys_idle = to_u64(idle);
    s.sys_kernel = to_u64(kernel);
    s.sys_user = to_u64(user);
    FILETIME created, exited;
    if (dwm && GetProcessTimes(dwm, &created, &exited, &kernel, &user))
        s.dwm_cpu = to_u64(kernel) + to_u64(user);
    return s;
}

void report(const char* name, const Sample& a, const Sample& b)
{
    double secs = (b.time - a.time) / 1e7;
    uint64_t busy = (b.sys_kernel - a.sys_kernel) + (b.sys_user - a.sys_user) - (b.sys_idle - a.sys_idle);
    uint64_t total = (b.sys_kernel - a.sys_kernel) + (b.sys_user - a.sys_user);
    std::fprintf(out,
                 "scenario %s start=%llu end=%llu secs=%.2f dwm_frames=%llu dwm_fps=%.1f refreshes=%llu "
                 "dwm_cpu_ms=%.0f sys_busy_pct=%.1f\n",
                 name, static_cast<unsigned long long>(a.time), static_cast<unsigned long long>(b.time), secs,
                 static_cast<unsigned long long>(b.frames - a.frames), (b.frames - a.frames) / secs,
                 static_cast<unsigned long long>(b.refreshes - a.refreshes),
                 dwm ? (b.dwm_cpu - a.dwm_cpu) / 1e4 : -1.0, total ? 100.0 * busy / total : 0.0);
    std::fflush(out);
}

void send_key(WORD vk, bool up)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

void move_cursor(int x, int y)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = MulDiv(x, 65535, GetSystemMetrics(SM_CXSCREEN) - 1);
    in.mi.dy = MulDiv(y, 65535, GetSystemMetrics(SM_CYSCREEN) - 1);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(in));
}

void idle() { pump_for(5000); }

void cursor()
{
    POINT saved;
    GetCursorPos(&saved);
    int cx = GetSystemMetrics(SM_CXSCREEN) / 2, cy = GetSystemMetrics(SM_CYSCREEN) / 2;
    int r = GetSystemMetrics(SM_CYSCREEN) / 4;
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    for (double t; (t = seconds_since(start)) < 10.0;) {
        move_cursor(cx + static_cast<int>(r * std::cos(t * 4)), cy + static_cast<int>(r * std::sin(t * 4)));
        pump_for(2);
    }
    SetCursorPos(saved.x, saved.y);
}

void drag()
{
    HWND hwnd = make_window(100, 200);
    pump_for(500);
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    for (double t; (t = seconds_since(start)) < 10.0;) {
        int x = 100 + static_cast<int>(400 * (1 + std::sin(t * 2)));
        SetWindowPos(hwnd, nullptr, x, 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump_for(4);
    }
    DestroyWindow(hwnd);
}

void anim()
{
    HWND hwnd = make_window(300, 200);
    pump_for(500);
    for (int i = 0; i < 5; i++) {
        ShowWindow(hwnd, SW_MINIMIZE);
        pump_for(600);
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        pump_for(600);
    }
    DestroyWindow(hwnd);
    for (int i = 0; i < 5; i++) {
        HWND w = make_window(300 + 20 * i, 200);
        pump_for(600);
        DestroyWindow(w);
        pump_for(600);
    }
}

void start_menu()
{
    for (int i = 0; i < 3; i++) {
        send_key(VK_LWIN, false);
        send_key(VK_LWIN, true);
        pump_for(1500);
        send_key(VK_ESCAPE, false);
        send_key(VK_ESCAPE, true);
        pump_for(1000);
    }
}

struct Scenario {
    const char* name;
    void (*run)();
};

const Scenario scenarios[] = {
    {"idle", idle}, {"cursor", cursor}, {"drag", drag}, {"anim", anim}, {"start", start_menu},
};

}  // namespace

int main(int argc, char** argv)
{
    std::vector<const Scenario*> chosen;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            FILE* f = std::fopen(argv[++i], "w");
            if (!f) {
                std::fprintf(stderr, "cannot open %s\n", argv[i]);
                return 2;
            }
            out = f;
            continue;
        }
        const Scenario* found = nullptr;
        for (const Scenario& s : scenarios)
            if (!std::strcmp(argv[i], s.name))
                found = &s;
        if (!found) {
            std::fprintf(stderr, "unknown scenario %s\n", argv[i]);
            return 2;
        }
        chosen.push_back(found);
    }
    if (chosen.empty())
        for (const Scenario& s : scenarios)
            chosen.push_back(&s);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);

    if (DWORD pid = dwm_pid())
        dwm = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    DWM_TIMING_INFO info = {};
    info.cbSize = sizeof(info);
    HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &info);
    std::fprintf(out, "uibench screen=%dx%d dwm=%s timing=0x%08lx refresh_period_qpc=%llu\n",
                 GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), dwm ? "yes" : "no",
                 static_cast<unsigned long>(hr), static_cast<unsigned long long>(info.qpcRefreshPeriod));

    for (const Scenario* s : chosen) {
        pump_for(1000);  // let the previous scenario's frames settle
        Sample a = sample();
        s->run();
        Sample b = sample();
        report(s->name, a, b);
    }
    timeEndPeriod(1);
    return 0;
}
