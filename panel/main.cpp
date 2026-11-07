// SPDX-License-Identifier: MIT
// The window: a NavigationView (pages on the left, the current page on a layer with a rounded top-left corner) with
// WinUI's ScrollBar, a dark title bar, and per-monitor DPI. With a command it runs that instead (cli.cpp).
//   vgd-panel.exe [/page N]
#include "panel.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace panel {

namespace {

using namespace theme;

constexpr float kPaneW = 280, kPad = 40, kMaxContent = 1000;
constexpr float kBarW = 16, kArrowH = 16, kLine = 48;
constexpr UINT_PTR kCollapseTimer = 1;

class Window {
public:
    int run(HINSTANCE inst, int show, int page);
    LRESULT proc(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum { kSbNone, kSbUp, kSbDown, kSbPageUp, kSbThumb, kSbPageDown };
    struct Bar { float top, bottom, thumbTop, thumbLen; };

    void paint(HDC dc);
    void drawPane();
    float areaH() const { return area_[page_].bottom - area_[page_].top; }
    float maxScroll() const { return std::max(0.f, contentH_[page_] - areaH()); }
    bool scrollable() const { return contentH_[page_] > areaH() + 0.5f; }
    Bar bar() const;
    int barPart(float x, float y) const;
    void drawScrollbar();
    void scrollBy(float d) { target_[page_] = std::clamp(target_[page_] + d, 0.f, maxScroll()); }
    void barAction(int part);
    void dragTo(float y);
    bool tick(float dt);
    void animate();
    void applyScale(UINT dpi);
    void makeTarget();
    void dropTarget();
    void contextMenu(int id, POINT screen);

    HWND hwnd_ = nullptr;
    Ui ui_;
    std::vector<std::unique_ptr<Page>> pages_;
    size_t page_ = 0;
    float scale_ = 1, width_ = 0, height_ = 0;  // width and height in DIPs
    ID2D1Factory* d2d_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;

    // The ScrollBar: scroll_ is what is shown, target_ where it is going (wheel and keys animate there). It scrolls
    // area_ (window DIPs): the page, or the part of a page that fills the window which scrolls by itself.
    std::vector<float> scroll_, target_, contentH_;
    std::vector<D2D1_RECT_F> area_;
    float expand_ = 0;  // 0 the collapsed indicator .. 1 expanded
    bool sbOver_ = false, dragging_ = false;
    int sbHot_ = kSbNone, sbDown_ = kSbNone;
    float sbY_ = 0, dragGrab_ = 0;
    DWORD leaveTick_ = 0, repeatAt_ = 0;
    bool animating_ = false;
    LARGE_INTEGER lastFrame_{};
    // A click in text selects the word, a double-click the whole text and a third click clears it; dragging past the
    // drag threshold selects from where it was pressed instead.
    bool selecting_ = false, selDragged_ = false;
    int selText_ = kNoId;
    UINT32 selPos_ = 0;
    POINT selFrom_{};
    DWORD selDouble_ = 0;  // when the last double-click was
};

Window* g_window;

void Window::makeTarget()
{
    if (rt_)
        return;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                              96, 96);
    d2d_->CreateDCRenderTarget(&props, &rt_);
    rt_->CreateSolidColorBrush(D2D1::ColorF(kWhite), &brush_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    ui_.setBrush(brush_);
}

void Window::dropTarget()
{
    if (brush_)
        brush_->Release();
    if (rt_)
        rt_->Release();
    brush_ = nullptr;
    rt_ = nullptr;
}

void Window::drawPane()
{
    float y = 8;
    for (size_t i = 0; i < pages_.size(); i++) {
        int id = kNavId + int(i);
        D2D1_RECT_F r = {4, y, kPaneW - 4, y + 36};
        bool sel = page_ == i, h = ui_.hover == id, p = h && ui_.pressed == id;
        if (sel || h)
            ui_.fill(r, 4, kWhite, p ? kControlPressed : (sel && h) ? kControlHover : kControl);
        if (sel)
            ui_.fill({4, y + 10, 7, y + 26}, 1.5f, accent);
        ui_.text(pages_[i]->icon(), Ui::kIcon, {16, y, 36, y + 36}, kWhite, kTextPrimary,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        ui_.text(pages_[i]->title(), Ui::kBody, {52, y, kPaneW - 12, y + 36});
        ui_.hot(r, id);
        y += 40;
    }
}

void Window::paint(HDC dc)
{
    makeTarget();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    rt_->BindDC(dc, &rc);
    if (animating_) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float dt = std::min(0.1f, float(now.QuadPart - lastFrame_.QuadPart) / freq.QuadPart);
        lastFrame_ = now;
        animating_ = tick(dt);
    }

    ui_.begin(rt_);
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    rt_->Clear(D2D1::ColorF(kBase));
    D2D1_RECT_F layer = {kPaneW, 0, width_ + 16, height_ + 16};
    ui_.fill(layer, 8, kLayer);
    ui_.stroke(layer, 8, kBlack, 0.1f);

    // the page is at most kMaxContent wide, centred in the space beside the pane
    float avail = width_ - kPaneW - 2 * kPad, w = std::min(avail, kMaxContent), x = kPaneW + kPad + (avail - w) / 2;
    bool fills = pages_[page_]->fills();
    scroll_[page_] = std::clamp(scroll_[page_], 0.f, maxScroll());
    ui_.scroll = scroll_[page_];
    ui_.offset = fills ? 0 : ui_.scroll;
    ui_.view = height_;
    bool atEnd = scroll_[page_] >= maxScroll() - 1 && target_[page_] >= maxScroll() - 1;
    rt_->PushAxisAlignedClip({kPaneW + 1, 1, width_, height_}, D2D1_ANTIALIAS_MODE_ALIASED);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, -ui_.offset) * D2D1::Matrix3x2F::Scale(scale_, scale_));
    float y = 28;
    ui_.text(pages_[page_]->title(), Ui::kTitle, {x, y, x + w, y + 40});
    y += 40 + 24;
    pages_[page_]->draw(ui_, x, w, y);
    if (fills) {
        area_[page_] = ui_.scrollArea;
        contentH_[page_] = ui_.scrollHeight;
    } else {
        area_[page_] = {kPaneW, 0, width_, height_};
        contentH_[page_] = y + 40;
    }
    bool grew = atEnd && pages_[page_]->followsEnd() && scroll_[page_] < maxScroll();
    rt_->PopAxisAlignedClip();
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    ui_.offset = 0;
    drawScrollbar();
    drawPane();
    if (grew) {  // show the new end in the next frame
        scroll_[page_] = target_[page_] = maxScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET)
        dropTarget();
    if (pages_[page_]->animating() && !animating_) {
        animating_ = true;
        QueryPerformanceCounter(&lastFrame_);
    }

    if (animating_) {
        // the next frame when the compositor takes this one; without DWM, about 120 Hz
        if (FAILED(DwmFlush()))
            Sleep(8);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---- ScrollBar: a 2 px indicator that expands, while the pointer is over it, into a track with a 6 px thumb and
// arrow buttons, and collapses a moment after the pointer leaves.

Window::Bar Window::bar() const
{
    Bar b;
    b.top = area_[page_].top + kArrowH;
    b.bottom = area_[page_].bottom - kArrowH;
    float track = b.bottom - b.top;
    b.thumbLen = std::max(24.f, track * areaH() / std::max(contentH_[page_], areaH()));
    float m = maxScroll();
    b.thumbTop = b.top + (m > 0 ? (track - b.thumbLen) * scroll_[page_] / m : 0);
    return b;
}

int Window::barPart(float x, float y) const
{
    const D2D1_RECT_F& a = area_[page_];
    if (!scrollable() || x < a.right - kBarW || x >= a.right || y < a.top || y >= a.bottom)
        return kSbNone;
    Bar b = bar();
    if (y < b.top)
        return kSbUp;
    if (y >= b.bottom)
        return kSbDown;
    if (y < b.thumbTop)
        return kSbPageUp;
    if (y < b.thumbTop + b.thumbLen)
        return kSbThumb;
    return kSbPageDown;
}

void Window::drawScrollbar()
{
    if (!scrollable())
        return;
    Bar b = bar();
    const D2D1_RECT_F& r = area_[page_];
    float e = expand_;
    if (e > 0.01f) {
        ui_.fill({r.right - kBarW + 2, r.top + 2, r.right - 2, r.bottom - 2}, 6, 0x2c2c2c, 0.96f * e);
        float up = sbHot_ == kSbUp ? kTextSecondary : kTextTertiary;
        float down = sbHot_ == kSbDown ? kTextSecondary : kTextTertiary;
        ui_.text(L"", Ui::kArrow, {r.right - kBarW, r.top + 2, r.right, r.top + kArrowH}, kWhite, up * e,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        ui_.text(L"", Ui::kArrow, {r.right - kBarW, r.bottom - kArrowH, r.right, r.bottom - 2}, kWhite, down * e,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    float w = 2 + 4 * e, cx = r.right - 4 - 4 * e;
    float a = dragging_ || sbHot_ == kSbThumb ? kTextSecondary : kTextTertiary;
    ui_.fill({cx - w / 2, b.thumbTop + 1, cx + w / 2, b.thumbTop + b.thumbLen - 1}, w / 2, kWhite, a);
}

// A press on an arrow or the track, and its repeats while held.
void Window::barAction(int part)
{
    Bar b = bar();
    switch (part) {
    case kSbUp: scrollBy(-kLine); break;
    case kSbDown: scrollBy(kLine); break;
    case kSbPageUp:
        if (sbY_ < b.thumbTop)
            scrollBy(-areaH() * 0.875f);
        break;
    case kSbPageDown:
        if (sbY_ >= b.thumbTop + b.thumbLen)
            scrollBy(areaH() * 0.875f);
        break;
    }
}

void Window::dragTo(float y)
{
    Bar b = bar();
    float range = (b.bottom - b.top) - b.thumbLen;
    float f = range > 0 ? std::clamp((y - dragGrab_ - b.top) / range, 0.f, 1.f) : 0;
    scroll_[page_] = target_[page_] = f * maxScroll();
}

// One frame, dt seconds after the last: scrolling towards the target, the bar expanding or collapsing, held buttons
// repeating. Time-based, so a late frame does not make the motion jump. Returns whether anything still moves.
bool Window::tick(float dt)
{
    bool busy = false;
    DWORD now = GetTickCount();
    target_[page_] = std::clamp(target_[page_], 0.f, maxScroll());
    float d = target_[page_] - scroll_[page_];
    if (std::abs(d) > 0.5f) {
        scroll_[page_] += d * (1 - std::exp(-dt / 0.06f));  // eases out with a 60 ms time constant
        busy = true;
    } else
        scroll_[page_] = target_[page_];
    bool held = sbOver_ || dragging_ || sbDown_;
    bool lingering = !held && expand_ > 0 && now - leaveTick_ < 600;
    float want = held || lingering ? 1 : 0;
    if (lingering)  // no frames while it waits: a timer starts the collapse
        SetTimer(hwnd_, kCollapseTimer, 600 - (now - leaveTick_), nullptr);
    if (expand_ != want) {
        float step = dt / 0.1f;  // 100 ms either way
        expand_ = want > expand_ ? std::min(1.f, expand_ + step) : std::max(0.f, expand_ - step);
        busy = true;
    }
    if (sbDown_ && sbDown_ != kSbThumb) {
        if (sbHot_ == sbDown_ && now >= repeatAt_) {
            barAction(sbDown_);
            repeatAt_ = now + 50;
        }
        busy = true;
    }
    return busy;
}

// Frames follow the compositor (DwmFlush in paint) until tick() says everything is still.
void Window::animate()
{
    if (!animating_)
        QueryPerformanceCounter(&lastFrame_);
    animating_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Right-click on selectable text: Copy (the selection, or the whole text when nothing of it is selected) and Select all.
void Window::contextMenu(int id, POINT screen)
{
    if (!ui_.selected(id))
        ui_.selectAll(id);
    InvalidateRect(hwnd_, nullptr, FALSE);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING, 2, L"Select all\tCtrl+A");
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 1)
        setClipboard(hwnd_, ui_.selectedText());
    else if (cmd == 2)
        ui_.selectAll(id);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::applyScale(UINT dpi)
{
    scale_ = dpi / 96.f;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = rc.right / scale_;
    height_ = rc.bottom / scale_;
}

LRESULT Window::proc(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        applyScale(GetDpiForWindow(hwnd_));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyScale(HIWORD(wp));
        for (auto& p : pages_)
            p->refresh();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        for (auto& p : pages_)
            p->refresh();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd_);
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {MulDiv(860, dpi, 96), MulDiv(560, dpi, 96)};
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        sbY_ = y;
        if (selecting_) {
            if (!selDragged_) {
                if (abs(GET_X_LPARAM(lp) - selFrom_.x) < GetSystemMetrics(SM_CXDRAG) &&
                    abs(GET_Y_LPARAM(lp) - selFrom_.y) < GetSystemMetrics(SM_CYDRAG))
                    return 0;
                selDragged_ = true;
                ui_.selectStart(selText_, selPos_);
            }
            ui_.selectTo(x, y);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (dragging_) {
            dragTo(y);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        int part = barPart(x, y);
        bool over = part != kSbNone;
        if (over != sbOver_) {
            sbOver_ = over;
            if (!over)
                leaveTick_ = GetTickCount();
            animate();
        }
        int h = over ? kNoId : ui_.hit(x, y);
        if (h != ui_.hover || part != sbHot_) {
            ui_.hover = h;
            sbHot_ = part;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        ui_.hover = kNoId;
        if (sbOver_ && !dragging_) {
            sbOver_ = false;
            sbHot_ = kSbNone;
            leaveTick_ = GetTickCount();
            animate();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetCapture(hwnd_);
        SetFocus(hwnd_);
        float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
        int textId;
        UINT32 pos;
        if (!sbHot_ && !ui_.hover && ui_.textAt(x, y, &textId, &pos)) {
            POINT at = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (msg == WM_LBUTTONDBLCLK) {
                ui_.selectAll(textId);
                selDouble_ = GetMessageTime();
                selFrom_ = at;
            } else if (DWORD(GetMessageTime()) - selDouble_ <= GetDoubleClickTime() &&
                       abs(at.x - selFrom_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                       abs(at.y - selFrom_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2) {
                ui_.clearSelection();  // the third click
                selDouble_ -= GetDoubleClickTime() + 1;
            } else {
                ui_.selectWord(textId, pos);
                selecting_ = true;
                selDragged_ = false;
                selText_ = textId;
                selPos_ = pos;
                selFrom_ = at;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        ui_.clearSelection();
        if (sbHot_) {
            sbDown_ = sbHot_;
            sbY_ = GET_Y_LPARAM(lp) / scale_;
            if (sbDown_ == kSbThumb) {
                dragging_ = true;
                dragGrab_ = sbY_ - bar().thumbTop;
            } else {
                barAction(sbDown_);
                repeatAt_ = GetTickCount() + 300;
            }
            animate();
        } else
            ui_.pressed = ui_.hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        selecting_ = false;
        if (sbDown_) {
            sbDown_ = kSbNone;
            dragging_ = false;
            leaveTick_ = GetTickCount();
            animate();
            return 0;
        }
        int p = ui_.pressed;
        ui_.pressed = kNoId;
        if (p && p == ui_.hover) {
            if (p >= kNavId && p < kNavId + int(pages_.size())) {
                page_ = p - kNavId;
                ui_.clearSelection();
            }
            else
                pages_[page_]->click(p);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        scrollBy(-GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 96);
        animate();
        return 0;
    case WM_RBUTTONUP: {
        int textId;
        UINT32 pos;
        if (ui_.textAt(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_, &textId, &pos)) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &pt);
            contextMenu(textId, pt);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            int textId;
            UINT32 pos;
            bool text = selecting_ || (!sbHot_ && !ui_.hover && ui_.textAt(pt.x / scale_, pt.y / scale_, &textId, &pos));
            SetCursor(LoadCursor(nullptr, text ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN: {
        if (GetKeyState(VK_CONTROL) < 0 && (wp == 'C' || wp == 'A')) {
            if (wp == 'C' && !ui_.selectedText().empty())
                setClipboard(hwnd_, ui_.selectedText());
            else if (wp == 'A')
                ui_.selectAll(ui_.selectionId());
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        float view = areaH() * 0.875f;
        switch (wp) {
        case VK_PRIOR: scrollBy(-view); break;
        case VK_NEXT: scrollBy(view); break;
        case VK_UP: scrollBy(-kLine); break;
        case VK_DOWN: scrollBy(kLine); break;
        case VK_HOME: scrollBy(-contentH_[page_]); break;
        case VK_END: scrollBy(contentH_[page_]); break;
        default: return 0;
        }
        animate();
        return 0;
    }
    case WM_TIMER:
        if (wp == kCollapseTimer) {
            KillTimer(hwnd_, kCollapseTimer);
            animate();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    if ((msg >= kPageMessage && msg < 0xC000) || (msg == WM_TIMER && wp >= kPageTimer)) {
        for (auto& p : pages_)
            if (p->message(msg, wp, lp)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return g_window ? g_window->proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

int Window::run(HINSTANCE inst, int show, int page)
{
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_);
    ui_.init();
    // Popup menus in the dark theme, as Explorer has them: uxtheme's SetPreferredAppMode(ForceDark) and FlushMenuThemes,
    // exported by ordinal only (135, 136). On builds before 1903, 135 is AllowDarkModeForApp, which takes the 2 as true.
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (auto set = reinterpret_cast<int(WINAPI*)(int)>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))
            set(2);
        if (auto flush = reinterpret_cast<void(WINAPI*)()>(GetProcAddress(ux, MAKEINTRESOURCEA(136))))
            flush();
    }
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"vgd-panel";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    RECT r = {0, 0, MulDiv(1180, dpi, 96), MulDiv(760, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"vgd Control Panel", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd_)
        return 1;
    pages_.push_back(makeOverview(hwnd_));
    pages_.push_back(makeSettings());
    pages_.push_back(makeLogs(hwnd_));
    pages_.push_back(makeTests(hwnd_));
    pages_.push_back(makeTools());
    page_ = std::clamp<size_t>(page, 0, pages_.size() - 1);
    scroll_.assign(pages_.size(), 0);
    target_.assign(pages_.size(), 0);
    contentH_.assign(pages_.size(), 0);
    area_.assign(pages_.size(), {});
    g_window = this;
    applyScale(GetDpiForWindow(hwnd_));
    // Windows 11: a dark title bar in the base colour and rounded corners; older systems ignore these.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = RGB(0x20, 0x20, 0x20), captionText = RGB(0xff, 0xff, 0xff);
    DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));
    int corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    ShowWindow(hwnd_, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_window = nullptr;
    return 0;
}

}  // namespace

}  // namespace panel

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int page = 0;
    int code = panel::runCommandLine(argc, argv, &page);
    if (code >= 0)
        return code;
    panel::Window window;
    return window.run(inst, show, page);
}
