// SPDX-License-Identifier: MIT
// The page that still shows the design's mock data: Tools. It moves to a file of its own when it is wired to the
// driver.
#include "panel.h"


namespace panel {

using namespace theme;

namespace {

constexpr float kCardH = 68, kGap = 4;

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

std::unique_ptr<Page> makeTools() { return std::make_unique<Tools>(); }

}  // namespace panel
