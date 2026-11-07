// SPDX-License-Identifier: MIT
// Overview: the adapter at a glance, as Settings > System > About shows the PC. A card with the host GPU and a button
// that copies the whole page as text, then expanders for the driver, the host, the graphics APIs and the displays. What needs a
// graphics driver comes from the probe (probe.cpp), run in the background when the page is made and on "Check again".
#include "panel.h"

#include <cwchar>

namespace panel {

using namespace theme;

namespace {

constexpr UINT kProbeDone = kPageMessage;
constexpr UINT_PTR kCopiedTimer = kPageTimer;
constexpr DWORD kProbeTimeoutMs = 20000;
enum : int { kCopyId = kPageId, kCheckId, kExpanderId, kNameText = kPageId + 100, kValueText };

struct Row {
    std::wstring key, value;
    bool bad = false;  // shown in the critical colour
};
struct Section {
    const wchar_t* icon;
    std::wstring title;
    std::vector<Row> rows;
};

class Overview : public Page {
public:
    explicit Overview(HWND window) : window_(window), info_(readSystemInfo()) { check(); }

    const wchar_t* title() const override { return L"Overview"; }
    const wchar_t* icon() const override { return L""; }
    void draw(Ui& ui, float x, float w, float& y) override;
    void click(int id) override;
    void refresh() override { info_.displays = readDisplays(info_.ours ? info_.driverDesc : std::wstring()); }
    bool message(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void check()
    {
        probing_ = true;
        startProbe(window_, kProbeDone, kProbeTimeoutMs);
    }
    std::wstring probed(const char* key) const
    {
        auto it = probe_.find(key);
        return it == probe_.end() ? std::wstring() : it->second;
    }
    bool apiOk(const char* api) const { return !probing_ && probed((std::string(api) + ".status").c_str()) == L"ok"; }
    Row api(const wchar_t* name, const char* prefix, const std::wstring& okText) const;
    // The host GPU and its Vulkan driver from Venus' device name, "Virtio-GPU Venus (<GPU> (<driver>))".
    void host(std::wstring& gpu, std::wstring& driver) const;
    std::vector<Section> sections() const;
    void copy() const;

    HWND window_;
    SystemInfo info_;
    ProbeResult probe_;
    bool probing_ = false, copied_ = false;
    bool collapsed_[8] = {};
};

Row Overview::api(const wchar_t* name, const char* prefix, const std::wstring& okText) const
{
    if (probing_)
        return {name, L"Checking…"};
    std::wstring s = probed((std::string(prefix) + ".status").c_str());
    if (s == L"ok")
        return {name, okText};
    if (s == L"checking")  // started but never finished
        return {name, probed("timeout").empty() ? L"The check crashed" : L"The check stopped responding", true};
    if (s.empty())
        return {name, L"Not checked", true};
    if (s == L"no adapter")
        return {name, L"No VirtIO GPU adapter", true};
    if (s == L"no loader")
        return {name, L"The Vulkan Runtime is not installed", true};
    if (s == L"no device")
        return {name, L"No Venus device", true};
    return {name, L"Failed (" + s + L")", true};
}

void Overview::host(std::wstring& gpu, std::wstring& driver) const
{
    std::wstring name = probed("vk.device");
    const std::wstring prefix = L"Virtio-GPU Venus (";
    if (name.compare(0, prefix.size(), prefix) == 0 && name.back() == L')')
        name = name.substr(prefix.size(), name.size() - prefix.size() - 1);
    size_t open = name.rfind(L" (");
    if (!name.empty() && name.back() == L')' && open != std::wstring::npos) {
        driver = name.substr(open + 2, name.size() - open - 3);
        name.resize(open);
    }
    gpu = name;
}

std::vector<Section> Overview::sections() const
{
    std::vector<Section> out;

    Section driver{L"", L"Driver"};
    driver.rows.push_back({L"Version", info_.productVersion.empty() ? L"Not installed" : info_.productVersion,
                           info_.productVersion.empty()});
    if (!info_.installDate.empty())
        driver.rows.push_back({L"Installed", info_.installDate});
    if (info_.deviceFound) {
        std::wstring device = info_.driverDesc.empty() ? L"None" : info_.driverDesc;
        driver.rows.push_back({L"Device driver", device, !info_.ours});
        if (!info_.driverVersion.empty())
            driver.rows.push_back({L"Driver version", info_.driverVersion +
                                                          (info_.driverDate.empty() ? L"" : L", " + info_.driverDate)});
    }
    driver.rows.push_back({L"Test signing", info_.testSigning ? L"On" : L"Off, so Windows does not load the driver",
                           !info_.testSigning});
    out.push_back(driver);

    Section hostSection{L"", L"Host"};
    std::wstring gpu, vkDriver;
    host(gpu, vkDriver);
    if (probing_)
        hostSection.rows.push_back({L"GPU", L"Checking…"});
    else if (!gpu.empty()) {
        hostSection.rows.push_back({L"GPU", gpu});
        if (!vkDriver.empty())
            hostSection.rows.push_back({L"Vulkan driver", vkDriver});
    } else
        hostSection.rows.push_back({L"GPU", L"Unknown: Venus did not answer", true});
    if (!info_.machine.empty())
        hostSection.rows.push_back({L"Virtual machine", info_.machine});
    out.push_back(hostSection);

    Section apis{L"", L"Graphics APIs"};
    apis.rows.push_back(api(L"Direct3D 12", "d3d12",
                            L"Feature level " + probed("d3d12.level") + L", shader model " +
                                probed("d3d12.shader_model")));
    apis.rows.push_back(api(L"Direct3D 11", "d3d11", L"Feature level " + probed("d3d11.level")));
    if (probing_)
        apis.rows.push_back({L"Direct3D 9", L"Checking…"});
    else
        apis.rows.push_back(apiOk("d3d12") ? Row{L"Direct3D 9", L"Through D3D9On12"}
                                           : Row{L"Direct3D 9", L"Needs Direct3D 12", true});
    std::wstring vk = probed("vk.api") + L" (" + probed("vk.driver") + L", " + probed("vk.driver_info") + L")";
    apis.rows.push_back(api(L"Vulkan", "vk", vk));
    // OpenGL: "4.6 (Compatibility Profile) Mesa 26.1.0" from zink; Windows' own renderer is "GDI Generic".
    std::wstring glVersion = probed("gl.version"), renderer = probed("gl.renderer");
    Row gl = api(L"OpenGL", "gl", L"");
    if (apiOk("gl")) {
        if (renderer.find(L"GDI Generic") != std::wstring::npos) {
            gl = {L"OpenGL", L"Windows' software renderer, not the GPU", true};
        } else {
            std::wstring v = glVersion.substr(0, glVersion.find(L' '));
            size_t mesa = glVersion.find(L"Mesa ");
            std::wstring by = renderer.find(L"zink") != std::wstring::npos ? L"zink" : renderer;
            gl.value = v + L" (" + by + (mesa != std::wstring::npos ? L", " + glVersion.substr(mesa) : L"") + L")";
        }
    }
    apis.rows.push_back(gl);
    out.push_back(apis);

    Section displays{L"", L"Displays"};
    for (size_t i = 0; i < info_.displays.size(); i++) {
        const Display& d = info_.displays[i];
        wchar_t buf[96];
        swprintf(buf, 96, L"%lu × %lu at %lu Hz, %u%% scale", d.width, d.height, d.hz, d.scale);
        displays.rows.push_back({info_.displays.size() > 1 ? L"Display " + std::to_wstring(i + 1) : L"Display", buf});
    }
    if (displays.rows.empty())
        displays.rows.push_back({L"Display", L"None attached"});
    out.push_back(displays);
    return out;
}

void Overview::copy() const
{
    std::wstring text = L"vgd system information\r\n";
    for (const Section& s : sections()) {
        text += L"\r\n" + s.title + L"\r\n";
        for (const Row& r : s.rows)
            text += L"  " + r.key + L": " + r.value + L"\r\n";
    }
    setClipboard(window_, text);
}

void Overview::draw(Ui& ui, float x, float w, float& y)
{
    // the adapter
    ui.card({x, y, x + w, y + 104});
    std::wstring gpu, vkDriver;
    host(gpu, vkDriver);
    std::wstring name = !gpu.empty() ? gpu : info_.ours ? L"VirtIO GPU" : info_.driverDesc;
    std::wstring sub = info_.ours ? L"VirtIO GPU through Venus" : info_.deviceFound ? L"VirtIO GPU" : L"";
    ui.text(L"", Ui::kGlyph, {x + 24, y, x + 72, y + 104}, accent, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    const wchar_t* label = copied_ ? L"Copied" : L"Copy";
    float bw = ui.buttonWidth(label);
    ui.selectable(kNameText, name, Ui::kSubtitle, {x + 96, y + 24, x + w - bw - 40, y + 54});
    ui.text(sub, Ui::kBody, {x + 96, y + 56, x + w - bw - 40, y + 78}, kWhite, kTextSecondary);
    ui.button(label, x + w - 24 - bw, y + 36, kCopyId, false);
    y += 104 + 24;

    // expanders: header row, then (unless collapsed) a divider and the rows
    std::vector<Section> all = sections();
    int text = kValueText;  // the values are selectable, numbered in order
    for (size_t i = 0; i < all.size(); i++) {
        const Section& s = all[i];
        bool open = !collapsed_[i];
        int headerId = kExpanderId + int(i);
        float h = 64 + (open ? 1 + 12 + s.rows.size() * 32 + 12 : 0);
        ui.card({x, y, x + w, y + h});
        if (ui.hover == headerId)
            ui.fill({x + 1, y + 1, x + w - 1, y + 64}, 4, kWhite, kCardHover - kCardFill);
        ui.hot({x, y, x + w, y + 64}, headerId);
        ui.text(s.icon, Ui::kIconLarge, {x + 16, y, x + 36, y + 64}, kWhite, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        ui.text(s.title, Ui::kBody, {x + 56, y, x + w - 260, y + 64});
        float bx = x + w - 52;
        if (i == 2) {
            bx -= ui.buttonWidth(L"Check again");
            if (!probing_)
                ui.button(L"Check again", bx, y + 16, kCheckId, false);
        }
        ui.text(open ? L"" : L"", Ui::kIconSmall, {x + w - 44, y, x + w - 16, y + 64}, kWhite,
                kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (open) {
            ui.fill({x + 1, y + 64, x + w - 1, y + 65}, 0, kBlack, 0.25f);
            float ry = y + 65 + 12;
            for (const Row& r : s.rows) {
                ui.text(r.key, Ui::kBody, {x + 56, ry, x + 56 + 180, ry + 32}, kWhite, kTextSecondary);
                ui.selectable(text++, r.value, Ui::kBody, {x + 56 + 180, ry, x + w - 16, ry + 32},
                              r.bad ? kCritical : kWhite);
                ry += 32;
            }
        }
        y += h + 4;
    }
}

void Overview::click(int id)
{
    if (id == kCopyId) {
        copy();
        copied_ = true;
        SetTimer(window_, kCopiedTimer, 2000, nullptr);
    } else if (id == kCheckId) {
        info_ = readSystemInfo();
        check();
    } else if (id >= kExpanderId && id < kExpanderId + 8) {
        collapsed_[id - kExpanderId] = !collapsed_[id - kExpanderId];
    }
}

bool Overview::message(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == kProbeDone) {
        std::unique_ptr<ProbeResult> result(reinterpret_cast<ProbeResult*>(lp));
        probe_ = std::move(*result);
        probing_ = false;
        return true;
    }
    if (msg == WM_TIMER && wp == kCopiedTimer) {
        KillTimer(window_, kCopiedTimer);
        copied_ = false;
        return true;
    }
    return false;
}

}  // namespace

std::unique_ptr<Page> makeOverview(HWND window) { return std::make_unique<Overview>(window); }

}  // namespace panel
