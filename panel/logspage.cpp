// SPDX-License-Identifier: MIT
// Logs: what the drivers print, live (LogCapture). Start begins a capture, which keeps going until Stop or the panel
// closes. Without administrator rights it sees only this session's programs, and an InfoBar offers to restart the
// panel elevated, for the kernel driver's messages and services' output.
#include "panel.h"

#include <commdlg.h>
#include <shlobj.h>

#include <deque>

namespace panel {

using namespace theme;

namespace {

constexpr UINT kLines = kPageMessage + 4;
constexpr float kCardH = 68, kRowH = 22;
constexpr size_t kMaxLines = 20000;
constexpr int kLineRun = 1;
enum : int { kCaptureId = kPageId, kClearId, kExportId, kElevateId, kFilterId, kLineText = kPageId + 100 };

// The SelectorBar narrows step by step: everything, the drivers' lines, their warnings and errors, their errors.
enum Filter { kAll, kDrivers, kWarnings, kErrors };
const wchar_t* const kFilters[] = {L"All", L"Drivers", L"Warnings", L"Errors"};

std::wstring clock(const SYSTEMTIME& t, bool ms)
{
    wchar_t buf[16];
    if (ms)
        swprintf(buf, 16, L"%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    else
        swprintf(buf, 16, L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return buf;
}

struct Line : LogLine {
    unsigned seq;
};

class Logs : public Page {
public:
    explicit Logs(HWND window) : window_(window) {}
    ~Logs() override { capture_.stop(); }
    const wchar_t* title() const override { return L"Logs"; }
    const wchar_t* icon() const override { return L""; }
    void draw(Ui& ui, float x, float w, float& y) override;
    void click(int id) override;
    bool message(UINT msg, WPARAM wp, LPARAM lp) override;
    bool fills() const override { return true; }
    bool followsEnd() const override { return true; }

private:
    void start();
    bool shown(const Line& l) const
    {
        return filter_ == kAll || (!l.component.empty() && l.level >= filter_ - kDrivers);
    }
    void exportLog();
    void elevate();

    HWND window_;
    LogCapture capture_;
    bool admin_ = elevated();
    SYSTEMTIME since_ = {};
    int filter_ = kDrivers;
    std::deque<Line> lines_;
    unsigned seq_ = 0;
    std::mutex mutex_;
    std::vector<LogLine> pending_;  // from the capture's threads
};

void Logs::start()
{
    GetLocalTime(&since_);
    capture_.start([this](LogLine&& l) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty())
            PostMessageW(window_, kLines, 0, 0);
        pending_.push_back(std::move(l));
    });
}

bool Logs::message(UINT msg, WPARAM, LPARAM)
{
    if (msg != kLines)
        return false;
    std::vector<LogLine> got;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        got.swap(pending_);
    }
    for (LogLine& l : got)
        lines_.push_back({std::move(l), seq_++});
    while (lines_.size() > kMaxLines)
        lines_.pop_front();
    return true;
}

void Logs::draw(Ui& ui, float x, float w, float& y)
{
    if (capture_.running()) {
        std::wstring desc = L"Since " + clock(since_, false) + L", from " +
                            (capture_.kernelError.empty() ? L"programs, services and the kernel"
                                                          : L"programs in this session");
        float cy = ui.settingsCard(x, y, w, kCardH, L"", L"Capturing debug output", desc, kNoId, kCritical);
        ui.button(L"Stop", x + w - 16 - ui.buttonWidth(L"Stop"), cy - 16, kCaptureId, false);
    } else {
        float cy = ui.settingsCard(x, y, w, kCardH, L"", L"Capture debug output",
                                   L"What the drivers print while programs use the GPU");
        ui.button(L"Start", x + w - 16 - ui.buttonWidth(L"Start"), cy - 16, kCaptureId, true);
    }
    y += kCardH + 4;

    // InfoBars, a sentence each, for what the capture cannot see.
    auto infoBar = [&](const wchar_t* glyph, UINT32 iconColor, UINT32 fill, float fillAlpha, const std::wstring& text,
                       const wchar_t* action) {
        D2D1_RECT_F r = {x, y, x + w, y + 48};
        ui.fill(r, 4, fill, fillAlpha);
        ui.stroke(r, 4, kBlack, 0.1f);
        ui.text(glyph, Ui::kIcon, {x + 16, y, x + 36, y + 48}, iconColor, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        float right = x + w - 16;
        if (action) {
            right -= ui.buttonWidth(action) + 12;
            ui.button(action, right + 12, y + 8, kElevateId, false);
        }
        ui.text(text, Ui::kBody, {x + 48, y, right, y + 48});
        y += 48 + 4;
    };
    if (!admin_)
        infoBar(L"", accent, kWhite, 0.0326f,
                L"Restart as administrator to also see the kernel driver's messages and services' output.",
                L"Restart as administrator");
    if (capture_.running() && !capture_.userError.empty())
        infoBar(L"", kCritical, kCriticalBg, 1, capture_.userError + L".", nullptr);
    if (capture_.running() && admin_ && !capture_.kernelError.empty())
        infoBar(L"", kCritical, kCriticalBg, 1, capture_.kernelError + L".", nullptr);
    y += 16;

    // SelectorBar for which lines, actions on the right
    float fx = x;
    for (int i = 0; i < 4; i++) {
        float tw = ui.measure(kFilters[i], Ui::kBody) + 24;
        D2D1_RECT_F r = {fx, y, fx + tw, y + 40};
        bool sel = filter_ == i, h = ui.hover == kFilterId + i;
        if (h && !sel)
            ui.fill({r.left, r.top + 4, r.right, r.bottom - 4}, 4, kWhite, kControl);
        ui.text(kFilters[i], Ui::kBody, r, kWhite, sel || h ? kTextPrimary : kTextSecondary,
                DWRITE_TEXT_ALIGNMENT_CENTER);
        if (sel)
            ui.fill({fx + tw / 2 - 8, y + 36, fx + tw / 2 + 8, y + 39}, 1.5f, accent);
        ui.hot(r, kFilterId + i);
        fx += tw;
    }
    float bx = x + w - ui.buttonWidth(L"Export");
    ui.button(L"Export", bx, y + 4, kExportId, false);
    bx -= 8 + ui.buttonWidth(L"Clear");
    ui.button(L"Clear", bx, y + 4, kClearId, false);
    y += 40 + 12;

    std::vector<const Line*> shownLines;
    for (const Line& l : lines_)
        if (shown(l))
            shownLines.push_back(&l);
    if (shownLines.empty()) {
        ui.card({x, y, x + w, y + 64});
        ui.text(!capture_.running() ? L"Start capturing to see what the drivers print."
                : filter_ == kAll   ? L"Nothing yet."
                                    : L"Nothing from the drivers yet. They print as programs use the GPU.",
                Ui::kBody, {x + 16, y, x + w - 16, y + 64}, kWhite, kTextSecondary);
        y += 64;
        return;
    }
    // A selection can go over lines, also past those in view.
    ui.runText(kLineRun, [this](int id, std::wstring* s) {
        if (lines_.empty() || id < kLineText)
            return false;
        size_t i = unsigned(id - kLineText) - lines_.front().seq % 100000000;
        if (i >= lines_.size() || !shown(lines_[i]))
            return false;
        *s = lines_[i].text;
        return true;
    });
    // The lines fill the card down to the window's bottom and scroll in it.
    D2D1_RECT_F card = {x, y, x + w, std::max(y + 120, ui.view - 40)};
    ui.card(card);
    ui.beginScroll(card, shownLines.size() * kRowH + 24);
    // Only the rows in view: the log can be long.
    size_t first = size_t(std::max(0.f, (ui.scroll - 12) / kRowH));
    float bottom = card.bottom + ui.scroll;
    for (size_t i = first; i < shownLines.size(); i++) {
        float ry = y + 12 + i * kRowH;
        if (ry > bottom)
            break;
        const Line* l = shownLines[i];
        if (l->level)
            ui.text(l->level == 2 ? L"" : L"", Ui::kIconSmall, {x + 12, ry, x + 30, ry + kRowH},
                    l->level == 2 ? kCritical : kCaution, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        ui.text(clock(l->time, true), Ui::kMono, {x + 38, ry, x + 142, ry + kRowH}, kWhite, kTextTertiary);
        ui.text(l->component.empty() ? l->process : l->component, Ui::kMono, {x + 146, ry, x + 230, ry + kRowH},
                kWhite, kTextSecondary);
        ui.selectable(kLineText + int(l->seq % 100000000), l->text, Ui::kMono, {x + 238, ry, x + w - 20, ry + kRowH},
                     kWhite, kTextPrimary, kLineRun);
    }
    ui.endScroll();
    y = card.bottom;
}

void Logs::click(int id)
{
    if (id >= kFilterId && id < kFilterId + 4) {
        filter_ = id - kFilterId;
    } else if (id == kCaptureId) {
        if (capture_.running())
            capture_.stop();
        else
            start();
    } else if (id == kClearId) {
        lines_.clear();
    } else if (id == kExportId) {
        exportLog();
    } else if (id == kElevateId) {
        elevate();
    }
}

void Logs::exportLog()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t file[MAX_PATH];
    swprintf(file, MAX_PATH, L"vgd-log-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    wchar_t docs[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = window_;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = docs;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn))
        return;
    std::wstring text;
    for (const Line& l : lines_)
        text += formatLogLine(l) + L"\r\n";
    std::string utf8 = narrow(text);
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

// The panel again as administrator, on this page. This one lets go of debug output first: only one program can
// read it.
void Logs::elevate()
{
    bool was = capture_.running();
    capture_.stop();
    if (!restartElevated(window_, 2) && was)
        start();
}

}  // namespace

std::unique_ptr<Page> makeLogs(HWND window) { return std::make_unique<Logs>(window); }

}  // namespace panel
