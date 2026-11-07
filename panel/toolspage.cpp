// SPDX-License-Identifier: MIT
// Tools. The frame rate overlay runs as venera-panel /fps (fpsoverlay.cpp), a process of its own with administrator
// rights, which outlives the panel; the toggle follows its window, also when Ctrl+Alt+F closed it. Troubleshooting:
// opening an issue on GitHub, and restarting the Desktop Window Manager (through venera-panel /restart-dwm and UAC when
// this panel runs without administrator rights).
#include "panel.h"

#include <shellapi.h>

namespace panel {

using namespace theme;

namespace {

constexpr float kCardH = 68, kGap = 4;
constexpr UINT_PTR kWatchTimer = kPageTimer + 1;

constexpr wchar_t kNewIssue[] = L"https://github.com/Aleksanaa/venera/issues/new";

class Tools : public Page {
public:
    explicit Tools(HWND window) : window_(window) { SetTimer(window_, kWatchTimer, 500, nullptr); }
    ~Tools() override
    {
        if (starting_)
            CloseHandle(starting_);
    }
    const wchar_t* title() const override { return L"Tools"; }
    const wchar_t* icon() const override { return L""; }
    void draw(Ui& ui, float x, float w, float& y) override;
    void click(int id) override;
    bool message(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    enum : int { kFpsId = kPageId, kIssueId, kDwmId };

    bool overlayOn() const { return starting_ || findFpsOverlay(); }
    void startOverlay();
    HANDLE runSelf(const wchar_t* args);

    HWND window_;
    HANDLE starting_ = nullptr;  // the overlay's process until its window shows (or it exits, when it could not start)
    bool shown_ = overlayOn();   // what the toggle showed last
};

void Tools::draw(Ui& ui, float x, float w, float& y)
{
    shown_ = overlayOn();
    float cy = ui.settingsCard(x, y, w, kCardH, L"", L"Frame rate overlay",
                               L"Frames the screen shows, the longest gap between them, mouse reports and each "
                               L"program's frame rate, over everything. Ctrl+Alt+F closes it.");
    ui.toggle(x + w - 16, cy, kFpsId, shown_);
    y += kCardH + kGap;
    ui.sectionHeader(L"Troubleshooting", x, y);
    cy = ui.settingsCard(x, y, w, kCardH, L"", L"Report an issue",
                         L"On GitHub, with what went wrong. Copy the Overview's report into it.");
    ui.button(L"Open", x + w - 16 - ui.buttonWidth(L"Open"), cy - 16, kIssueId, false);
    y += kCardH + kGap;
    cy = ui.settingsCard(x, y, w, kCardH, L"", L"Restart the Desktop Window Manager",
                         L"It loads the drivers afresh, with their current settings. The screen flickers.");
    ui.button(L"Restart", x + w - 16 - ui.buttonWidth(L"Restart"), cy - 16, kDwmId, false);
    y += kCardH + kGap;
}

void Tools::click(int id)
{
    if (id == kIssueId) {
        ShellExecuteW(window_, nullptr, kNewIssue, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == kDwmId) {
        if (elevated())
            restartDwm();
        else if (HANDLE p = runSelf(L"/restart-dwm"))
            CloseHandle(p);
    } else if (id == kFpsId) {
        if (HWND overlay = findFpsOverlay())
            PostMessageW(overlay, WM_CLOSE, 0, 0);
        else if (!starting_)
            startOverlay();
    }
}

// venera-panel with `args`, through UAC when this panel runs without administrator rights; its process, or null.
HANDLE Tools::runSelf(const wchar_t* args)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = elevated() ? nullptr : L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.hwnd = window_;
    sei.nShow = SW_SHOWNOACTIVATE;
    return ShellExecuteExW(&sei) ? sei.hProcess : nullptr;
}

// It reports its own errors.
void Tools::startOverlay() { starting_ = runSelf(L"/fps"); }

// The overlay's window comes and goes on its own: a repaint when the toggle would change.
bool Tools::message(UINT msg, WPARAM wp, LPARAM)
{
    if (msg != WM_TIMER || wp != kWatchTimer)
        return false;
    if (starting_ && (findFpsOverlay() || WaitForSingleObject(starting_, 0) == WAIT_OBJECT_0)) {
        CloseHandle(starting_);
        starting_ = nullptr;
    }
    return overlayOn() != shown_;
}

}  // namespace

std::unique_ptr<Page> makeTools(HWND window) { return std::make_unique<Tools>(window); }

}  // namespace panel
