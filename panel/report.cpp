// SPDX-License-Identifier: MIT
// The system report: what the Overview shows and `/info` prints, from the system information and the probe's result.
#include "panel.h"

#include <cwchar>

namespace panel {

namespace {

std::wstring probed(const ProbeResult* probe, const char* key)
{
    if (!probe)
        return {};
    auto it = probe->find(key);
    return it == probe->end() ? std::wstring() : it->second;
}

bool apiOk(const ProbeResult* probe, const char* api)
{
    return probed(probe, (std::string(api) + ".status").c_str()) == L"ok";
}

InfoRow api(const ProbeResult* probe, const wchar_t* name, const char* prefix, const std::wstring& okText)
{
    if (!probe)
        return {name, L"Checking…"};
    std::wstring s = probed(probe, (std::string(prefix) + ".status").c_str());
    if (s == L"ok")
        return {name, okText};
    if (s == L"checking")  // started but never finished
        return {name, probed(probe, "timeout").empty() ? L"The check crashed" : L"The check stopped responding", true};
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

}  // namespace

void hostGpu(const ProbeResult* probe, std::wstring& gpu, std::wstring& driver)
{
    std::wstring name = probed(probe, "vk.device");
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

std::vector<InfoSection> describe(const SystemInfo& info, const ProbeResult* probe)
{
    std::vector<InfoSection> out;

    InfoSection driver{L"", L"Driver"};
    driver.rows.push_back({L"Version", info.productVersion.empty() ? L"Not installed" : info.productVersion,
                           info.productVersion.empty()});
    if (!info.installDate.empty())
        driver.rows.push_back({L"Installed", info.installDate});
    if (!info.deviceFound) {
        driver.rows.push_back({L"Device", L"No VirtIO GPU found", true});
    } else {
        if (info.problem)
            driver.rows.push_back(
                {L"Device", L"Device Manager reports a problem, code " + std::to_wstring(info.problem), true});
        driver.rows.push_back({L"Device driver", info.driverDesc.empty() ? L"None" : info.driverDesc, !info.ours});
        if (!info.driverVersion.empty())
            driver.rows.push_back(
                {L"Driver version", info.driverVersion + (info.driverDate.empty() ? L"" : L", " + info.driverDate)});
    }
    driver.rows.push_back({L"Test signing", info.testSigning ? L"On" : L"Off, so Windows does not load the driver",
                           !info.testSigning});
    out.push_back(driver);

    InfoSection host{L"", L"Host"};
    std::wstring gpu, vkDriver;
    hostGpu(probe, gpu, vkDriver);
    if (!probe)
        host.rows.push_back({L"GPU", L"Checking…"});
    else if (!gpu.empty()) {
        host.rows.push_back({L"GPU", gpu});
        if (!vkDriver.empty())
            host.rows.push_back({L"Vulkan driver", vkDriver});
    } else
        host.rows.push_back({L"GPU", L"Unknown: Venus did not answer", true});
    if (!info.machine.empty())
        host.rows.push_back({L"Virtual machine", info.machine});
    out.push_back(host);

    InfoSection apis{L"", L"Graphics APIs"};
    apis.rows.push_back(api(probe, L"Direct3D 12", "d3d12",
                            L"Feature level " + probed(probe, "d3d12.level") + L", shader model " +
                                probed(probe, "d3d12.shader_model")));
    apis.rows.push_back(api(probe, L"Direct3D 11", "d3d11", L"Feature level " + probed(probe, "d3d11.level")));
    if (!probe)
        apis.rows.push_back({L"Direct3D 9", L"Checking…"});
    else
        apis.rows.push_back(apiOk(probe, "d3d12") ? InfoRow{L"Direct3D 9", L"Through D3D9On12"}
                                                  : InfoRow{L"Direct3D 9", L"Needs Direct3D 12", true});
    apis.rows.push_back(api(probe, L"Vulkan", "vk",
                            probed(probe, "vk.api") + L" (" + probed(probe, "vk.driver") + L", " +
                                probed(probe, "vk.driver_info") + L")"));
    // OpenGL: "4.6 (Compatibility Profile) Mesa 26.1.0" from zink; Windows' own renderer is "GDI Generic".
    std::wstring glVersion = probed(probe, "gl.version"), renderer = probed(probe, "gl.renderer");
    InfoRow gl = api(probe, L"OpenGL", "gl", L"");
    if (apiOk(probe, "gl")) {
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

    InfoSection displays{L"", L"Displays"};
    for (size_t i = 0; i < info.displays.size(); i++) {
        const Display& d = info.displays[i];
        wchar_t buf[96];
        swprintf(buf, 96, L"%lu × %lu at %lu Hz, %u%% scale", d.width, d.height, d.hz, d.scale);
        displays.rows.push_back({info.displays.size() > 1 ? L"Display " + std::to_wstring(i + 1) : L"Display", buf});
    }
    if (displays.rows.empty())
        displays.rows.push_back({L"Display", L"None attached"});
    out.push_back(displays);
    return out;
}

std::wstring reportText(const std::vector<InfoSection>& sections)
{
    std::wstring text = L"vgd system information\r\n";
    for (const InfoSection& s : sections) {
        text += L"\r\n" + s.title + L"\r\n";
        for (const InfoRow& r : s.rows)
            text += L"  " + r.key + L": " + r.value + L"\r\n";
    }
    return text;
}

bool anyProblem(const std::vector<InfoSection>& sections)
{
    for (const InfoSection& s : sections)
        for (const InfoRow& r : s.rows)
            if (r.bad)
                return true;
    return false;
}

}  // namespace panel
