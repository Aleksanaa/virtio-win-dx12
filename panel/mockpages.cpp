// SPDX-License-Identifier: MIT
// The pages that still show the design's mock data: Settings and Tools. Each moves to a file of its own
// when it is wired to the driver.
#include "panel.h"


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
std::unique_ptr<Page> makeTools() { return std::make_unique<Tools>(); }

}  // namespace panel
