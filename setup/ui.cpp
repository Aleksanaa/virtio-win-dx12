// SPDX-License-Identifier: MIT
// The installer's window: a classic wizard with a banner, a status line and progress bar, the log as it is written,
// and buttons at the bottom. Every page shows the same log; a page's text (what will happen) goes into it. It follows
// DPI changes.
#include "setup.h"

#include <commctrl.h>

#include <thread>

namespace {

constexpr wchar_t kClass[] = L"venera-setup";
constexpr UINT kLogLine = WM_APP, kStep = WM_APP + 1, kWorkDone = WM_APP + 2;
constexpr int kStatus = 1001, kBar = 1002, kLog = 1003, kTitle = 1004, kSubtitle = 1005, kVersion = 1006;
constexpr int kWidth = 640, kHeight = 500, kBanner = 64, kMargin = 16, kButtonW = 100, kButtonH = 28;
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, kExStyle = WS_EX_CONTROLPARENT;

}  // namespace

struct Wizard::Impl {
    HWND hwnd = nullptr;
    HWND title, subtitle, status, bar, log_view, version_label;
    std::vector<HWND> buttons;
    HFONT font = nullptr, bold = nullptr, mono = nullptr;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    int dpi = 96;
    int result = 0;
    bool working = false;
    int default_button = 0;

    int px(int v) const { return MulDiv(v, dpi, 96); }

    HWND child(const wchar_t* cls, DWORD style, DWORD ex, int id)
    {
        return CreateWindowExW(ex, cls, L"", WS_CHILD | style, 0, 0, 0, 0, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    }

    // The fonts for the current DPI, set on every control.
    void make_fonts()
    {
        for (HFONT f : {font, bold, mono})
            if (f)
                DeleteObject(f);
        NONCLIENTMETRICSW ncm = {sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfWeight = FW_BOLD;
        lf.lfHeight = lf.lfHeight * 4 / 3;
        bold = CreateFontIndirectW(&lf);
        mono = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           FIXED_PITCH, L"Consolas");
        for (HWND c : {subtitle, status, version_label})
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        for (HWND b : buttons)
            SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(bold), FALSE);
        SendMessageW(log_view, WM_SETFONT, reinterpret_cast<WPARAM>(mono), FALSE);
    }

    void append_log(const std::wstring& line)
    {
        std::wstring text;
        for (wchar_t c : line)
            if (c == L'\n')
                text += L"\r\n";
            else if (c != L'\r')
                text += c;
        text += L"\r\n";
        int len = GetWindowTextLengthW(log_view);
        SendMessageW(log_view, EM_SETSEL, len, len);
        SendMessageW(log_view, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    }

    // Places the controls in the client area as it is now.
    void layout()
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom, m = px(kMargin);
        MoveWindow(title, m, px(12), w - 2 * m, px(24), TRUE);
        MoveWindow(subtitle, m + px(8), px(38), w - 2 * m - px(8), px(20), TRUE);
        int top = px(kBanner) + m, bottom = h - px(52) - m / 2;
        MoveWindow(status, m, top, w - 2 * m, px(20), TRUE);
        MoveWindow(bar, m, top + px(24), w - 2 * m, px(18), TRUE);
        MoveWindow(log_view, m, top + px(50), w - 2 * m, bottom - top - px(50), TRUE);
        int y = h - px(10) - px(kButtonH);
        int x = w - m;
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
            x -= px(kButtonW);
            MoveWindow(*it, x, y, px(kButtonW), px(kButtonH), TRUE);
            x -= px(8);
        }
        MoveWindow(version_label, m, y + px(6), px(160), px(18), TRUE);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void show(const Page& page)
    {
        SetWindowTextW(title, page.heading.c_str());
        SetWindowTextW(subtitle, page.subheading.c_str());
        SetWindowTextW(status, page.status.c_str());
        if (!page.text.empty())
            log(page.text);
        for (HWND b : buttons)
            DestroyWindow(b);
        buttons.clear();
        default_button = page.buttons.empty() ? 0 : page.buttons[page.default_index].first;
        for (const auto& [id, label] : page.buttons) {
            HWND b = child(L"BUTTON", WS_VISIBLE | WS_TABSTOP | (id == default_button ? BS_DEFPUSHBUTTON : 0), 0, id);
            SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            SetWindowTextW(b, label.c_str());
            buttons.push_back(b);
        }
        layout();
        if (!buttons.empty())
            SetFocus(buttons[page.default_index]);
        ShowWindow(hwnd, SW_SHOW);
    }

    void pump()
    {
        MSG msg;
        while (!result && GetMessageW(&msg, nullptr, 0, 0)) {
            if (!IsDialogMessageW(hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (HIWORD(wparam) == BN_CLICKED && ((id >= 100 && id < 1000) || id == IDCANCEL || id == IDOK)) {
                if (!self->working)
                    self->result = id == IDOK ? self->default_button : id;
                return 0;
            }
            break;
        }
        case DM_GETDEFID:
            return MAKELONG(self->default_button, DC_HASDEFID);
        case WM_CLOSE:
            if (!self->working)
                self->result = IDCANCEL;
            return 0;
        case WM_SIZE:
            self->layout();
            return 0;
        case WM_DPICHANGED: {
            self->dpi = HIWORD(wparam);
            self->make_fonts();
            auto r = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            self->layout();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            RECT banner = {0, 0, rc.right, self->px(kBanner)};
            FillRect(dc, &banner, self->white);
            RECT line = {0, banner.bottom, rc.right, banner.bottom + 2};
            DrawEdge(dc, &line, EDGE_ETCHED, BF_TOP);
            RECT bottom = {0, rc.bottom - self->px(52), rc.right, rc.bottom - self->px(50)};
            DrawEdge(dc, &bottom, EDGE_ETCHED, BF_TOP);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HWND c = reinterpret_cast<HWND>(lparam);
            HDC dc = reinterpret_cast<HDC>(wparam);
            if (c == self->title || c == self->subtitle || c == self->log_view) {
                SetBkColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(self->white);
            }
            if (c == self->version_label) {
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        }
        case kLogLine: {
            auto line = reinterpret_cast<std::wstring*>(lparam);
            self->append_log(*line);
            delete line;
            return 0;
        }
        case kStep: {
            auto step = reinterpret_cast<std::wstring*>(lparam);
            SendMessageW(self->bar, PBM_SETPOS, wparam, 0);
            SetWindowTextW(self->status, step->c_str());
            delete step;
            return 0;
        }
        case kWorkDone:
            self->result = -1;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
};

Wizard::Wizard(const std::wstring& version) : impl(new Impl)
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Impl::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_DEFAULTCOLOR));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    Impl& d = *impl;
    d.hwnd = CreateWindowExW(kExStyle, kClass, L"Venera Setup", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                             nullptr, wc.hInstance, nullptr);
    d.title = d.child(L"STATIC", WS_VISIBLE | SS_NOPREFIX, 0, kTitle);
    d.subtitle = d.child(L"STATIC", WS_VISIBLE | SS_NOPREFIX, 0, kSubtitle);
    d.status = d.child(L"STATIC", WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, kStatus);
    d.bar = d.child(PROGRESS_CLASSW, WS_VISIBLE, 0, kBar);
    SendMessageW(d.bar, PBM_SETRANGE32, 0, 100);
    // No horizontal scrolling: the control wraps long lines at its width.
    d.log_view = d.child(L"EDIT", WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                         WS_EX_CLIENTEDGE, kLog);
    SendMessageW(d.log_view, EM_SETLIMITTEXT, 0, 0);
    d.version_label = d.child(L"STATIC", WS_VISIBLE | SS_NOPREFIX, 0, kVersion);
    SetWindowTextW(d.version_label, (L"Venera " + version).c_str());

    // Sized and centred for the DPI of the monitor it opened on.
    d.dpi = GetDpiForWindow(d.hwnd);
    d.make_fonts();
    RECT rc = {0, 0, d.px(kWidth), d.px(kHeight)};
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, kExStyle, d.dpi);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(d.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(d.hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));

    // Everything logged so far, then each line as it is logged.
    for (const auto& line : log_history())
        d.append_log(line);
    HWND hwnd = d.hwnd;
    set_log_sink([hwnd](const std::wstring& line) {
        PostMessageW(hwnd, kLogLine, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
    });
}

Wizard::~Wizard()
{
    set_log_sink(nullptr);
    DestroyWindow(impl->hwnd);
    for (HFONT f : {impl->font, impl->bold, impl->mono})
        DeleteObject(f);
    DeleteObject(impl->white);
    delete impl;
}

int Wizard::ask(const Page& page)
{
    impl->show(page);
    impl->result = 0;
    impl->pump();
    return impl->result ? impl->result : IDCANCEL;
}

bool Wizard::run(const std::wstring& heading, const std::wstring& subheading,
                 const std::function<bool(const Progress&)>& work, std::wstring& error)
{
    impl->show({heading, subheading, {}, L"Starting", {{IDCANCEL, L"Cancel"}}});
    EnableWindow(impl->buttons.front(), FALSE);
    SendMessageW(impl->bar, PBM_SETPOS, 0, 0);
    impl->working = true;
    impl->result = 0;
    bool restart = false;
    HWND hwnd = impl->hwnd;
    std::thread thread([&] {
        try {
            restart = work([hwnd](int percent, const std::wstring& step) {
                log(step);
                PostMessageW(hwnd, kStep, percent, reinterpret_cast<LPARAM>(new std::wstring(step)));
            });
        } catch (const Failure& f) {
            error = f.message;
        } catch (const std::exception& e) {
            error = format(L"%hs", e.what());
        }
        if (!error.empty())
            log(L"Failed: " + error);
        PostMessageW(hwnd, kWorkDone, 0, 0);
    });
    impl->pump();
    thread.join();
    impl->working = false;
    return restart;
}
