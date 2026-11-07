// SPDX-License-Identifier: MIT
// Tests: the test groups (tests/) as expanders, collapsed until they run. A group's run button queues its tests and
// turns into a ProgressRing of how far the group is (click it to stop); the group opens while it runs and closes
// when it is done. Each test shows whether it waits, runs (a spinning ring), passed or failed, and why. The tests run
// one at a time on a worker thread, each in a child process (runTest).
#include "panel.h"

#include <deque>
#include <mutex>
#include <thread>

namespace panel {

using namespace theme;

namespace {

constexpr UINT kStarted = kPageMessage + 1, kFinished = kPageMessage + 2, kIdle = kPageMessage + 3;
constexpr float kCardH = 68, kHeaderH = 64, kRowH = 52;
enum : int {
    kRunAllId = kPageId, kCopyId,
    kHeaderId = kPageId + 100,   // + group
    kGroupRunId = kPageId + 200,  // + group
    kTestRunId = kPageId + 1000,  // + group * 100 + test
    kResultText = kPageId + 5000,  // + group * 100 + test
};

// kPartial is a group's alone: some of its tests passed, none failed, the rest did not run.
enum State { kNotRun, kWaiting, kRunning, kPassed, kFailed, kPartial };

// Segoe Fluent Icons
constexpr wchar_t kPlay[] = L"", kStop[] = L"", kNotRunGlyph[] = L"", kWaitGlyph[] = L"",
                  kPassGlyph[] = L"", kFailGlyph[] = L"";

struct Entry {
    State state = kNotRun;
    TestResult result;
};
struct Group {
    std::vector<Entry> tests;
    bool open = false;
    bool closeWhenDone = false;  // a run of the whole group: it closes again at the end
    int runTotal = 0, runDone = 0;
};

WPARAM pack(int g, int t) { return WPARAM(g) << 16 | WPARAM(t); }

class Tests : public Page {
public:
    explicit Tests(HWND window) : window_(window), cancel_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        for (const TestGroup& g : testGroups()) {
            groups_.emplace_back();
            groups_.back().tests.resize(g.tests.size());
        }
    }
    ~Tests() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            SetEvent(cancel_);
        }
        if (worker_.joinable())
            worker_.join();
        CloseHandle(cancel_);
    }

    const wchar_t* title() const override { return L"Tests"; }
    const wchar_t* icon() const override { return L""; }
    void draw(Ui& ui, float x, float w, float& y) override;
    void click(int id) override;
    bool message(UINT msg, WPARAM wp, LPARAM lp) override;
    bool animating() const override { return busy(); }

private:
    bool busy() const
    {
        for (const Group& g : groups_)
            if (active(g))
                return true;
        return false;
    }
    static bool active(const Group& g)
    {
        for (const Entry& e : g.tests)
            if (e.state == kWaiting || e.state == kRunning)
                return true;
        return false;
    }
    void queue(int g, int t);
    void queueGroup(int g);
    void stop(int g, int t);  // t < 0: the whole group
    void finishIfDone(int g);
    void work();
    void drawGroup(Ui& ui, int g, float x, float w, float& y);
    std::wstring summary(int* passed, int* failed) const;
    std::wstring results() const;

    HWND window_;
    std::vector<Group> groups_;
    SYSTEMTIME lastRun_ = {};
    bool copied_ = false;

    // The worker and its queue, shared with it under the mutex.
    std::mutex mutex_;
    std::deque<std::pair<int, int>> queue_;
    std::pair<int, int> running_ = {-1, -1};
    bool working_ = false;
    std::thread worker_;
    HANDLE cancel_;  // ends the test that runs
};

// ---- running --------------------------------------------------------------------------------------------------------

void Tests::work()
{
    for (;;) {
        std::pair<int, int> item;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                working_ = false;
                running_ = {-1, -1};
                PostMessageW(window_, kIdle, 0, 0);
                return;
            }
            item = running_ = queue_.front();
            queue_.pop_front();
            ResetEvent(cancel_);
        }
        const TestGroup& g = testGroups()[item.first];
        std::string id = std::string(g.id) + "." + g.tests[item.second].id;
        PostMessageW(window_, kStarted, pack(item.first, item.second), 0);
        auto* r = new TestResult(runTest(id, cancel_));
        if (!PostMessageW(window_, kFinished, pack(item.first, item.second), reinterpret_cast<LPARAM>(r)))
            delete r;
    }
}

void Tests::queue(int g, int t)
{
    Entry& e = groups_[g].tests[t];
    e.state = kWaiting;
    e.result = {};
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({g, t});
    if (!working_) {
        if (worker_.joinable())
            worker_.join();
        working_ = true;
        worker_ = std::thread(&Tests::work, this);
    }
}

void Tests::queueGroup(int g)
{
    Group& group = groups_[g];
    group.open = group.closeWhenDone = true;
    group.runTotal = int(group.tests.size());
    group.runDone = 0;
    for (size_t t = 0; t < group.tests.size(); t++)
        queue(g, int(t));
}

// After a group's last test: a group run closes it again.
void Tests::finishIfDone(int g)
{
    Group& group = groups_[g];
    if (active(group))
        return;
    if (group.closeWhenDone)
        group.open = false;
    group.closeWhenDone = false;
    group.runTotal = group.runDone = 0;
}

void Tests::stop(int g, int t)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->first == g && (t < 0 || it->second == t)) {
            groups_[g].tests[it->second].state = kNotRun;
            groups_[g].runDone++;
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    if (running_.first == g && (t < 0 || running_.second == t))
        SetEvent(cancel_);
}

bool Tests::message(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg != kStarted && msg != kFinished && msg != kIdle)
        return false;
    if (msg == kIdle) {
        GetLocalTime(&lastRun_);
        return true;
    }
    int g = int(wp >> 16), t = int(wp & 0xffff);
    Group& group = groups_[g];
    Entry& e = group.tests[t];
    if (msg == kStarted) {
        if (e.state == kWaiting)
            e.state = kRunning;
        return true;
    }
    std::unique_ptr<TestResult> r(reinterpret_cast<TestResult*>(lp));
    e.result = std::move(*r);
    e.state = e.result.ok ? kPassed : e.result.stopped ? kNotRun : kFailed;
    group.runDone++;
    finishIfDone(g);
    return true;
}

void Tests::click(int id)
{
    int n = int(groups_.size());
    if (id == kRunAllId) {
        if (busy()) {
            for (int g = 0; g < n; g++) {
                stop(g, -1);
                finishIfDone(g);
            }
        } else {
            for (int g = 0; g < n; g++)
                queueGroup(g);
        }
    } else if (id == kCopyId) {
        setClipboard(window_, results());
        copied_ = true;
    } else if (id >= kHeaderId && id < kHeaderId + n) {
        groups_[id - kHeaderId].open = !groups_[id - kHeaderId].open;
    } else if (id >= kGroupRunId && id < kGroupRunId + n) {
        int g = id - kGroupRunId;
        if (active(groups_[g])) {
            stop(g, -1);
            finishIfDone(g);
        }
        else
            queueGroup(g);
    } else if (id >= kTestRunId && id < kTestRunId + n * 100) {
        int g = (id - kTestRunId) / 100, t = (id - kTestRunId) % 100;
        Group& group = groups_[g];
        State s = group.tests[t].state;
        if (s == kWaiting || s == kRunning) {
            stop(g, t);
            finishIfDone(g);
        } else {
            if (!active(group))
                group.runTotal = group.runDone = 0;
            group.runTotal++;
            queue(g, t);
        }
    }
    if (id != kCopyId)
        copied_ = false;
}

// ---- drawing --------------------------------------------------------------------------------------------------------

// `s` cut with an ellipsis to fit `width`.
std::wstring fit(Ui& ui, const std::wstring& s, Ui::Font f, float width)
{
    if (ui.measure(s, f) <= width)
        return s;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (ui.measure(s.substr(0, mid) + L"…", f) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    return s.substr(0, lo) + L"…";
}

void stateIcon(Ui& ui, State s, float x, float cy)
{
    if (s == kPartial) {  // a caution-filled circle with a dash, as CompletedSolid is one with a check
        ui.text(L"\uEA3B", Ui::kIcon, {x, cy - 10, x + 20, cy + 10}, kCaution, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        ui.text(L"\uE738", Ui::kIconSmall, {x, cy - 10, x + 20, cy + 10}, kBlack, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (s == kRunning) {
        ui.progressRing(x + 10, cy, 16, -1);
        return;
    }
    const wchar_t* glyph = s == kWaiting ? kWaitGlyph : s == kPassed ? kPassGlyph : s == kFailed ? kFailGlyph
                                                                                                   : kNotRunGlyph;
    UINT32 c = s == kPassed ? kSuccess : s == kFailed ? kCritical : kWhite;
    float a = s == kNotRun ? kTextTertiary : s == kWaiting ? kTextSecondary : 1;
    ui.text(glyph, Ui::kIcon, {x, cy - 10, x + 20, cy + 10}, c, a, DWRITE_TEXT_ALIGNMENT_CENTER);
}

std::wstring seconds(float s)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f s", s);
    return buf;
}

std::wstring Tests::summary(int* passed, int* failed) const
{
    int notRun = 0;
    *passed = *failed = 0;
    for (const Group& g : groups_)
        for (const Entry& e : g.tests)
            (e.state == kPassed ? *passed : e.state == kFailed ? *failed : notRun)++;
    if (!*passed && !*failed)
        return L"Not run yet";
    std::wstring s = std::to_wstring(*passed) + L" passed, " + std::to_wstring(*failed) + L" failed";
    if (notRun)
        s += L", " + std::to_wstring(notRun) + L" not run";
    return s;
}

void Tests::draw(Ui& ui, float x, float w, float& y)
{
    // the summary, Run all and Copy results
    int passed, failed;
    std::wstring title = summary(&passed, &failed), desc = L"Each test runs on the VirtIO GPU in a process of its own";
    if (busy()) {
        for (size_t g = 0; g < groups_.size(); g++)
            for (size_t t = 0; t < groups_[g].tests.size(); t++)
                if (groups_[g].tests[t].state == kRunning)
                    title = std::wstring(L"Running ") + testGroups()[g].name + L": " + testGroups()[g].tests[t].name;
    } else if (lastRun_.wYear) {
        wchar_t time[64];
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &lastRun_, nullptr, time, 64);
        desc = std::wstring(L"Last run at ") + time;
    }
    float cy = ui.settingsCard(x, y, w, kCardH, L"", title, desc, kNoId, failed && !busy() ? kCritical : kWhite);
    const wchar_t* run = busy() ? L"Stop" : L"Run all";
    float right = x + w - 16 - ui.buttonWidth(run);
    ui.button(run, right, cy - 16, kRunAllId, !busy());
    if (passed || failed) {
        const wchar_t* copy = copied_ ? L"Copied" : L"Copy results";
        ui.button(copy, right - 8 - ui.buttonWidth(copy), cy - 16, kCopyId, false);
    }
    y += kCardH + 4;

    // an informational InfoBar: what the present tests do to the screen
    D2D1_RECT_F bar = {x, y, x + w, y + 68};
    ui.fill(bar, 4, kWhite, 0.0326f);  // SystemFillColorAttentionBackground
    ui.stroke(bar, 4, kBlack, 0.1f);
    ui.text(L"", Ui::kIcon, {x + 16, y, x + 36, y + 68}, accent, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    ui.text(L"Some tests show things on the screen", Ui::kBodyStrong, {x + 52, y + 14, x + w - 16, y + 34});
    ui.text(L"Swapchain and present tests open windows for a few seconds, and some fill the whole screen. They close "
            L"by themselves.",
            Ui::kBody, {x + 52, y + 34, x + w - 16, y + 54}, kWhite, kTextSecondary);
    y += 68 + 24;

    for (size_t g = 0; g < groups_.size(); g++)
        drawGroup(ui, int(g), x, w, y);
}

void Tests::drawGroup(Ui& ui, int g, float x, float w, float& y)
{
    const TestGroup& tg = testGroups()[g];
    const Group& group = groups_[g];
    bool running = active(group);
    float h = kHeaderH + (group.open ? 1 + 8 + group.tests.size() * kRowH + 8 : 0);
    ui.card({x, y, x + w, y + h});

    // the header: state, name and how it went, the run button (a ProgressRing while it runs) and the chevron
    int headerId = kHeaderId + g, runId = kGroupRunId + g;
    if (ui.hover == headerId)
        ui.fill({x + 1, y + 1, x + w - 1, y + kHeaderH}, 4, kWhite, kCardHover - kCardFill);
    ui.hot({x, y, x + w, y + kHeaderH}, headerId);
    int passed = 0, failed = 0, done = 0;
    for (const Entry& e : group.tests) {
        passed += e.state == kPassed;
        failed += e.state == kFailed;
    }
    done = passed + failed;
    State state = running ? kWaiting : failed ? kFailed : done == int(group.tests.size()) ? kPassed : done ? kPartial
                                                                                                            : kNotRun;
    stateIcon(ui, state, x + 16, y + kHeaderH / 2);
    std::wstring sub = tg.desc;
    if (running)
        sub = L"Test " + std::to_wstring(std::min(group.runDone + 1, group.runTotal)) + L" of " +
              std::to_wstring(group.runTotal);
    else if (done)
        sub = std::to_wstring(passed) + L" passed" + (failed ? L", " + std::to_wstring(failed) + L" failed" : L"") +
              (done < int(group.tests.size()) ? L", " + std::to_wstring(group.tests.size() - done) + L" not run" : L"");
    ui.text(tg.name, Ui::kBody, {x + 56, y + kHeaderH / 2 - 19, x + w - 120, y + kHeaderH / 2 + 1});
    ui.text(sub, Ui::kCaption, {x + 56, y + kHeaderH / 2 + 1, x + w - 120, y + kHeaderH / 2 + 19}, failed && !running
            ? kCritical : kWhite, failed && !running ? 1 : kTextSecondary);
    float bx = x + w - 52 - 32, by = y + kHeaderH / 2 - 16;
    if (running) {
        bool h2 = ui.hover == runId;
        if (h2)
            ui.fill({bx, by, bx + 32, by + 32}, 4, kWhite, ui.pressed == runId ? kControlPressed : kControlHover);
        ui.progressRing(bx + 16, by + 16, 20, group.runTotal ? float(group.runDone) / group.runTotal : 0);
        if (h2)
            ui.text(kStop, Ui::kIconSmall, {bx, by, bx + 32, by + 32}, kWhite, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        ui.hot({bx, by, bx + 32, by + 32}, runId);
    } else {
        ui.iconButton(kPlay, bx, by, runId);
    }
    ui.text(group.open ? L"" : L"", Ui::kIconSmall, {x + w - 44, y, x + w - 16, y + kHeaderH}, kWhite,
            kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    // the tests
    if (group.open) {
        ui.fill({x + 1, y + kHeaderH, x + w - 1, y + kHeaderH + 1}, 0, kBlack, 0.25f);
        float ry = y + kHeaderH + 1 + 8;
        for (size_t t = 0; t < group.tests.size(); t++) {
            const Test& test = tg.tests[t];
            const Entry& e = group.tests[t];
            float cy = ry + kRowH / 2;
            stateIcon(ui, e.state, x + 16, cy);
            float right = x + w - 52 - 32 - 16;  // left of the run button
            std::wstring time = e.state == kPassed || e.state == kFailed ? seconds(e.result.seconds) : L"";
            float tw = time.empty() ? 0 : ui.measure(time, Ui::kCaption) + 16;
            ui.text(test.name, Ui::kBody, {x + 56, cy - 19, right - tw, cy + 1});
            if (e.state == kFailed || (e.state == kNotRun && !e.result.summary.empty())) {
                std::wstring why = fit(ui, e.result.summary, Ui::kCaption, right - tw - x - 56);
                ui.selectable(kResultText + g * 100 + int(t), why, Ui::kCaption, {x + 56, cy + 1, right - tw, cy + 19},
                              e.state == kFailed ? kCritical : kWhite, e.state == kFailed ? 1 : kTextSecondary);
            } else {
                ui.text(test.desc, Ui::kCaption, {x + 56, cy + 1, right - tw, cy + 19}, kWhite, kTextSecondary);
            }
            if (!time.empty())
                ui.text(time, Ui::kCaption, {right - tw, cy - 10, right, cy + 10}, kWhite, kTextSecondary,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
            bool queued = e.state == kWaiting || e.state == kRunning;
            ui.iconButton(queued ? kStop : kPlay, x + w - 52 - 32, cy - 16, kTestRunId + g * 100 + int(t));
            ry += kRowH;
        }
    }
    y += h + 4;
}

// The results as text, with the output of each test that failed.
std::wstring Tests::results() const
{
    int passed, failed;
    std::wstring text = L"Venera tests: " + summary(&passed, &failed) + L"\r\n";
    for (size_t g = 0; g < groups_.size(); g++) {
        const TestGroup& tg = testGroups()[g];
        text += L"\r\n" + std::wstring(tg.name) + L"\r\n";
        for (size_t t = 0; t < groups_[g].tests.size(); t++) {
            const Entry& e = groups_[g].tests[t];
            text += L"  " + widen(tg.id) + L"." + widen(tg.tests[t].id) + L": ";
            if (e.state == kPassed)
                text += L"passed, " + seconds(e.result.seconds) + L"\r\n";
            else if (e.state == kFailed)
                text += L"FAILED, " + seconds(e.result.seconds) + L": " + e.result.summary + L"\r\n";
            else
                text += L"not run\r\n";
            if (e.state == kFailed)
                for (size_t start = 0; start < e.result.output.size();) {
                    size_t end = e.result.output.find(L'\n', start);
                    if (end == std::wstring::npos)
                        end = e.result.output.size();
                    std::wstring line = e.result.output.substr(start, end - start);
                    if (!line.empty() && line.back() == L'\r')
                        line.pop_back();
                    text += L"      " + line + L"\r\n";
                    start = end + 1;
                }
        }
    }
    return text;
}

}  // namespace

std::unique_ptr<Page> makeTests(HWND window) { return std::make_unique<Tests>(window); }

}  // namespace panel
