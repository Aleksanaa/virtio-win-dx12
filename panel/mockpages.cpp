// SPDX-License-Identifier: MIT
// The pages that still show the design's mock data: Settings, Logs, Tests and Tools. Each moves to a file of its own
// when it is wired to the driver.
#include "panel.h"

#include <cstdio>

namespace panel {

using namespace theme;

namespace {

constexpr float kCardH = 68, kGap = 4;

// ---- Settings -------------------------------------------------------------------------------------------------------

struct Setting {
    const wchar_t *icon, *title, *desc;
    std::vector<std::wstring> options;  // empty: a toggle
};
struct Group {
    const wchar_t* title;
    std::vector<Setting> rows;
};
const std::vector<Group> kSettings = {
    {L"Display", {{L"\uE962", L"Hardware cursor", L"The host draws the pointer. Changing this restarts the adapter.", {}},
                  {L"\uE740", L"Follow Windows scaling", L"Resize the guest desktop with the display scale", {}},
                  {L"\uE790", L"Invert colours", L"Show the guest desktop with inverted colours", {}}}},
    {L"Rendering", {{L"\uE7F4", L"Present mode", L"How windows reach the screen", {L"Automatic", L"Flip", L"Blt"}},
                    {L"\uE8B7", L"Shader cache", L"Keep compiled shaders between runs", {}}}},
    {L"Diagnostics", {{L"\uE8A5", L"Driver logging", L"Written to C:\\vgd. Verbose logging slows applications down.",
                       {L"Off", L"Errors only", L"Verbose"}}}},
};

class Settings : public Page {
public:
    const wchar_t* title() const override { return L"Settings"; }
    const wchar_t* icon() const override { return L"\uE713"; }

    void draw(Ui& ui, float x, float w, float& y) override
    {
        if (toggles_[0] != applied_[0]) {  // InfoBar, warning severity
            D2D1_RECT_F r = {x, y, x + w, y + 48};
            ui.fill(r, 4, kCautionBg);
            ui.stroke(r, 4, kBlack, 0.1f);
            ui.fill({x + 18, y + 16, x + 34, y + 32}, 8, kCaution);
            ui.text(L"!", Ui::kBodyStrong, {x + 18, y + 14, x + 34, y + 34}, kBlack, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            float tw = ui.measure(L"Restart required", Ui::kBodyStrong);
            ui.text(L"Restart required", Ui::kBodyStrong, {x + 48, y, x + 48 + tw, y + 48});
            ui.text(L"Restart the adapter to apply the cursor change.", Ui::kBody, {x + 60 + tw, y, x + w - 200, y + 48});
            float bw = ui.buttonWidth(L"Restart adapter");
            ui.button(L"Restart adapter", x + w - 56 - bw, y + 8, kRestartId, false);
            ui.text(L"\uE711", Ui::kIconSmall, {x + w - 44, y, x + w - 12, y + 48}, kWhite, kTextPrimary,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 48 + 8;
        }
        int index = 0;
        for (const Group& g : kSettings) {
            ui.sectionHeader(g.title, x, y);
            for (const Setting& s : g.rows) {
                float cy = ui.settingsCard(x, y, w, kCardH, s.icon, s.title, s.desc);
                if (s.options.empty())
                    ui.toggle(x + w - 16, cy, kToggleId + index, toggles_[index]);
                else
                    ui.comboBox(x + w - 16, cy, kComboId + index, s.options, combo_[index]);
                y += kCardH + kGap;
                index++;
            }
        }
    }

    void click(int id) override
    {
        if (id >= kToggleId && id < kToggleId + 16)
            toggles_[id - kToggleId] = !toggles_[id - kToggleId];
        else if (id >= kComboId && id < kComboId + 16) {
            int i = id - kComboId, n = 0;
            for (const Group& g : kSettings)
                for (const Setting& s : g.rows)
                    if (n++ == i)
                        combo_[i] = (combo_[i] + 1) % int(s.options.size());
        } else if (id == kRestartId)
            applied_[0] = toggles_[0];
    }

private:
    enum : int { kRestartId = kPageId, kToggleId = kPageId + 100, kComboId = kPageId + 200 };
    bool toggles_[16] = {true, true, false, false, true, false};
    bool applied_[16] = {true, true, false, false, true, false};
    int combo_[16] = {0, 0, 0, 0, 0, 1};
};

// ---- Logs -----------------------------------------------------------------------------------------------------------

struct LogLine {
    const wchar_t *time, *src;
    int level;  // 0 info 1 warning 2 error
    const wchar_t* msg;
};
const std::vector<LogLine> kLog = {
    {L"11:32:01.204", L"vgd3d12", 0, L"adapter: Venus on AMD Radeon 8060S Graphics (RADV GFX1151)"},
    {L"11:32:01.219", L"vgd3d12", 0, L"device 0x1a2b: feature level 12_1, shader model 6.6"},
    {L"11:32:01.388", L"vgd3d11", 0, L"dxvk: Venus device, 32 GiB host-visible heap"},
    {L"11:32:01.402", L"vgd3d11", 1, L"present: blt back buffer 1280×720 B8G8R8A8 in host-visible memory"},
    {L"11:32:01.733", L"viogpuap", 0, L"resolution: host asks 2560×1600, set"},
    {L"11:32:02.010", L"viogpu3d", 2, L"fence 0x4512 timed out after 2000 ms on context 3"},
    {L"11:32:02.011", L"viogpu3d", 1, L"context 3 reset, device removed for pid 4120 (Game.exe)"},
    {L"11:32:02.311", L"setup", 0, L"已安装 vgd 0.1.0.98.c2f7fde (日本語テスト, 한국어)"},
    {L"11:32:02.312", L"setup", 0, L"C:\\Users\\用户\\Downloads\\Office Tool\\Office Tool Plus.exe"},
    {L"11:32:03.120", L"vgd3d12", 0, L"pipeline cache: 1204 entries loaded (18.2 MiB)"},
    {L"11:32:04.508", L"vgd3d12", 1, L"descriptor heap 0x9: 1000000 descriptors requested, capped at 500000"},
    {L"11:32:05.002", L"vgpusrv", 0, L"capture: DxgKrnl and DWM providers on"},
    {L"11:32:06.774", L"vgd3d9", 0, L"D3D9On12 windowed present 640×480, copy host-visible"},
    {L"11:32:07.100", L"viogpu3d", 0, L"vsync: 60.00 Hz, 0 missed in the last 10 s"},
};

class Logs : public Page {
public:
    explicit Logs(HWND window) : window_(window) {}
    const wchar_t* title() const override { return L"Logs"; }
    const wchar_t* icon() const override { return L"\uE8A5"; }

    void draw(Ui& ui, float x, float w, float& y) override
    {
        if (capturing_) {
            DWORD t = (GetTickCount() - captureStart_) / 1000;
            wchar_t desc[96];
            swprintf(desc, 96, L"Capturing for %lu:%02lu. Reproduce the problem, then stop.", t / 60, t % 60);
            float cy = ui.settingsCard(x, y, w, kCardH, L"\uE7C8", L"Capture", desc, kNoId, kCritical);
            ui.button(L"Stop", x + w - 16 - ui.buttonWidth(L"Stop"), cy - 16, kCaptureId, false);
        } else {
            float cy = ui.settingsCard(x, y, w, kCardH, L"\uE7C8", L"Capture",
                                       L"Records driver, kernel and DWM events while you reproduce a problem");
            ui.button(L"Start capture", x + w - 16 - ui.buttonWidth(L"Start capture"), cy - 16, kCaptureId, true);
        }
        y += kCardH + 20;

        // SelectorBar for the level, actions on the right
        const wchar_t* levels[] = {L"All", L"Warnings", L"Errors"};
        float fx = x;
        for (int i = 0; i < 3; i++) {
            float tw = ui.measure(levels[i], Ui::kBody) + 24;
            D2D1_RECT_F r = {fx, y, fx + tw, y + 40};
            bool sel = filter_ == i, h = ui.hover == kFilterId + i;
            if (h && !sel)
                ui.fill({r.left, r.top + 4, r.right, r.bottom - 4}, 4, kWhite, kControl);
            ui.text(levels[i], Ui::kBody, r, kWhite, sel || h ? kTextPrimary : kTextSecondary,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            if (sel)
                ui.fill({fx + tw / 2 - 8, y + 36, fx + tw / 2 + 8, y + 39}, 1.5f, accent);
            ui.hot(r, kFilterId + i);
            fx += tw;
        }
        float bx = x + w - ui.buttonWidth(L"Export");
        ui.button(L"Export", bx, y + 4, kExportId, false);
        bx -= 8 + ui.buttonWidth(L"Open folder");
        ui.button(L"Open folder", bx, y + 4, kFolderId, false);
        y += 40 + 12;

        std::vector<const LogLine*> shown;
        for (const LogLine& l : kLog)
            if (l.level >= filter_)
                shown.push_back(&l);
        float rowH = 24, h = shown.size() * rowH + 24;
        ui.card({x, y, x + w, y + h});
        float ry = y + 12;
        for (const LogLine* l : shown) {
            if (l->level)
                ui.text(l->level == 2 ? L"\uEB90" : L"\uE7BA", Ui::kIconSmall, {x + 12, ry, x + 30, ry + rowH},
                        l->level == 2 ? kCritical : kCaution, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            ui.text(l->time, Ui::kMono, {x + 38, ry, x + 150, ry + rowH}, kWhite, kTextTertiary);
            ui.text(l->src, Ui::kMono, {x + 150, ry, x + 234, ry + rowH}, kWhite, kTextSecondary);
            ui.text(l->msg, Ui::kMono, {x + 238, ry, x + w - 16, ry + rowH});
            ry += rowH;
        }
        y += h;
    }

    void click(int id) override
    {
        if (id >= kFilterId && id < kFilterId + 3)
            filter_ = id - kFilterId;
        else if (id == kCaptureId) {
            capturing_ = !capturing_;
            captureStart_ = GetTickCount();
            if (capturing_)
                SetTimer(window_, kTickTimer, 500, nullptr);
            else
                KillTimer(window_, kTickTimer);
        }
    }

    bool message(UINT msg, WPARAM wp, LPARAM) override { return msg == WM_TIMER && wp == kTickTimer; }

private:
    enum : int { kCaptureId = kPageId, kExportId, kFolderId, kFilterId };
    static constexpr UINT_PTR kTickTimer = kPageTimer + 1;
    HWND window_;
    int filter_ = 0;
    bool capturing_ = false;
    DWORD captureStart_ = 0;
};

// ---- Tests ----------------------------------------------------------------------------------------------------------

struct Test {
    const wchar_t *name, *desc;
    int result;  // 0 pass 1 fail 2 not run 3 measured
    const wchar_t* detail;
};
const std::vector<Test> kTests = {
    {L"Direct3D 12 basics", L"Device, copies, a triangle, textures, depth, compute and queries", 0, L"Passed, 2.4 s"},
    {L"Direct3D 11 basics", L"Device creation, formats and a rendered frame", 0, L"Passed, 1.1 s"},
    {L"Direct3D 9 window", L"Draws a red square in a window; you confirm it", 2, L"Not run"},
    {L"Vulkan device", L"Instance, device, memory types and a submit", 0, L"Passed, 0.6 s"},
    {L"Vulkan swapchain", L"Presents 300 frames to a window; you confirm them", 1, L"Frame 12 lost"},
    {L"Desktop smoothness", L"Window drags and scrolling, measured under DWM", 3, L"41.2 ms at p99"},
};

class Tests : public Page {
public:
    const wchar_t* title() const override { return L"Tests"; }
    const wchar_t* icon() const override { return L"\uE9D5"; }

    void draw(Ui& ui, float x, float w, float& y) override
    {
        float cy = ui.settingsCard(x, y, w, kCardH, L"\uE9D5", L"4 passed, 1 failed, 1 not run",
                                   L"Last run today at 11:32");
        ui.button(L"Run all", x + w - 16 - ui.buttonWidth(L"Run all"), cy - 16, kPageId, true);
        y += kCardH;
        ui.sectionHeader(L"Tests", x, y);
        for (size_t i = 0; i < kTests.size(); i++) {
            const Test& t = kTests[i];
            const wchar_t* glyph = t.result == 0 ? L"\uEC61" : t.result == 1 ? L"\uEB90" : t.result == 3 ? L"\uE9D9"
                                                                                                          : L"\uE9CE";
            UINT32 c = t.result == 0 ? kSuccess : t.result == 1 ? kCritical : kWhite;
            float cy2 = ui.settingsCard(x, y, w, kCardH, glyph, t.name, t.desc, kNoId, c,
                                        t.result == 2 ? kTextTertiary : 1);
            float bw = ui.buttonWidth(L"Run");
            ui.button(L"Run", x + w - 16 - bw, cy2 - 16, kPageId + 1 + int(i), false);
            ui.text(t.detail, Ui::kBody, {x + w - 16 - bw - 240, cy2 - 12, x + w - 16 - bw - 16, cy2 + 12}, kWhite,
                    kTextSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
            y += kCardH + kGap;
        }
    }
};

// ---- Tools ----------------------------------------------------------------------------------------------------------

struct Tool {
    const wchar_t *icon, *title, *desc, *action;  // action null: it opens (chevron)
};
const std::vector<Tool> kTools = {
    {L"\uE962", L"Input monitor", L"Pointer position, buttons, wheel and keys as the guest receives them", nullptr},
    {L"\uEC4A", L"Frame rate overlay", L"Frame rate and frame times on top of any application", nullptr},
    {L"\uE790", L"Test pattern", L"Colour bars, gradients and a pixel grid for checking scaling and colours", nullptr},
    {L"\uEBE8", L"Bug report", L"Logs, a short capture and system information in one zip file", L"Create"},
    {L"\uE72C", L"Restart adapter", L"The screen goes black for a few seconds", L"Restart"},
};

class Tools : public Page {
public:
    const wchar_t* title() const override { return L"Tools"; }
    const wchar_t* icon() const override { return L"\uE90F"; }

    void draw(Ui& ui, float x, float w, float& y) override
    {
        for (size_t i = 0; i < kTools.size(); i++) {
            const Tool& t = kTools[i];
            if (i == 3)
                ui.sectionHeader(L"Troubleshooting", x, y);
            int id = kPageId + int(i);
            float cy = ui.settingsCard(x, y, w, kCardH, t.icon, t.title, t.desc, t.action ? kNoId : id);
            if (t.action)
                ui.button(t.action, x + w - 16 - ui.buttonWidth(t.action), cy - 16, kPageId + 100 + int(i), false);
            else
                ui.text(L"\uE76C", Ui::kIconSmall, {x + w - 36, cy - 10, x + w - 16, cy + 10}, kWhite, kTextPrimary,
                        DWRITE_TEXT_ALIGNMENT_CENTER);
            y += kCardH + kGap;
        }
    }
};

}  // namespace

std::unique_ptr<Page> makeSettings() { return std::make_unique<Settings>(); }
std::unique_ptr<Page> makeLogs(HWND window) { return std::make_unique<Logs>(window); }
std::unique_ptr<Page> makeTests() { return std::make_unique<Tests>(); }
std::unique_ptr<Page> makeTools() { return std::make_unique<Tools>(); }

}  // namespace panel
