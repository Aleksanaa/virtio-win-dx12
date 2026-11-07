// SPDX-License-Identifier: MIT
// What the panel reads in its own process: the VirtIO GPU and its driver (SetupAPI, the driver's registry key, the
// device's problem code), vgd as Apps & features lists it, test signing, the displays on the adapter, and the virtual
// machine from the firmware's SMBIOS tables.
#include "panel.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellscalingapi.h>
#include <winternl.h>

namespace panel {

namespace {

// As pkg/vgd.inf names the device and its provider.
constexpr wchar_t kHardwareId[] = L"PCI\\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01";
constexpr wchar_t kProvider[] = L"vgd project";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\vgd";

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

std::wstring longDate(WORD year, WORD month, WORD day)
{
    SYSTEMTIME t = {};
    t.wYear = year;
    t.wMonth = month;
    t.wDay = day;
    wchar_t buf[128];
    if (!year || !GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &t, nullptr, buf, 128, nullptr))
        return {};
    return buf;
}

bool isVirtioGpu(HDEVINFO set, SP_DEVINFO_DATA* dev)
{
    wchar_t ids[2048] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                           sizeof(ids) - 2 * sizeof(wchar_t), nullptr))
        return false;
    for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
        if (!_wcsicmp(p, kHardwareId))
            return true;
    return false;
}

void readDevice(SystemInfo& s)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (!isVirtioGpu(set, &dev))
            continue;
        s.deviceFound = true;
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) == CR_SUCCESS && (status & DN_HAS_PROBLEM))
            s.problem = problem;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            s.driverDesc = regString(key, nullptr, L"DriverDesc");
            s.provider = regString(key, nullptr, L"ProviderName");
            s.driverVersion = regString(key, nullptr, L"DriverVersion");
            unsigned m = 0, d = 0, y = 0;  // DriverDate is m-d-yyyy
            if (swscanf(regString(key, nullptr, L"DriverDate").c_str(), L"%u-%u-%u", &m, &d, &y) == 3)
                s.driverDate = longDate(y, m, d);
            RegCloseKey(key);
        }
        s.ours = s.provider == kProvider;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

bool testSigning()
{
    SYSTEM_CODEINTEGRITY_INFORMATION info = {sizeof(info), 0};
    if (NtQuerySystemInformation(SystemCodeIntegrityInformation, &info, sizeof(info), nullptr) != 0)
        return false;
    return info.CodeIntegrityOptions & CODEINTEGRITY_OPTION_TESTSIGN;
}

// SMBIOS type 1 (System Information): manufacturer, product name and version, e.g. QEMU's machine type.
std::wstring machine()
{
    constexpr DWORD kRsmb = 0x52534d42;  // 'RSMB'
    UINT size = GetSystemFirmwareTable(kRsmb, 0, nullptr, 0);
    if (!size)
        return {};
    std::vector<BYTE> buf(size);
    GetSystemFirmwareTable(kRsmb, 0, buf.data(), size);
    const BYTE* p = buf.data() + 8;  // RawSMBIOSData: 4 bytes of versions, a DWORD length, then the table
    const BYTE* end = buf.data() + std::min<size_t>(size, 8 + *reinterpret_cast<const DWORD*>(buf.data() + 4));
    while (p + 4 <= end) {
        BYTE type = p[0], length = p[1];
        const char* strings = reinterpret_cast<const char*>(p + length);
        auto string = [&](BYTE index) -> std::wstring {
            const char* s = strings;
            for (BYTE i = 1; index && i < index && s < reinterpret_cast<const char*>(end); i++)
                s += strlen(s) + 1;
            return index && s < reinterpret_cast<const char*>(end) ? widen(s) : std::wstring();
        };
        if (type == 1 && length >= 8) {
            std::wstring out = string(p[4]);
            std::wstring product = string(p[5]), version = string(p[6]);
            if (!product.empty())
                out += (out.empty() ? L"" : L" ") + product;
            if (!version.empty())
                out += L", " + version;
            return out;
        }
        // the next structure follows the strings' double NUL
        const BYTE* q = p + length;
        while (q + 1 < end && (q[0] || q[1]))
            q++;
        p = q + 2;
    }
    return {};
}

}  // namespace

// The current mode and scale of each display attached to the adapter Windows names `adapterName` (all displays when
// it is empty).
std::vector<Display> readDisplays(const std::wstring& adapterName)
{
    std::vector<Display> out;
    DISPLAY_DEVICEW dd = {sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof(dd)) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        if (!adapterName.empty() && adapterName != dd.DeviceString)
            continue;
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
            continue;
        Display d;
        d.width = mode.dmPelsWidth;
        d.height = mode.dmPelsHeight;
        d.hz = mode.dmDisplayFrequency;
        POINT center = {mode.dmPosition.x + LONG(d.width / 2), mode.dmPosition.y + LONG(d.height / 2)};
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX,
                                       &dpiY)))
            d.scale = MulDiv(dpiX, 100, 96);
        out.push_back(d);
    }
    if (out.empty() && !adapterName.empty())
        return readDisplays({});
    return out;
}

SystemInfo readSystemInfo()
{
    SystemInfo s;
    readDevice(s);
    s.productVersion = regString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion");
    unsigned y = 0, m = 0, d = 0;  // InstallDate is yyyymmdd
    if (swscanf(regString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallDate").c_str(), L"%4u%2u%2u", &y, &m, &d) == 3)
        s.installDate = longDate(y, m, d);
    s.testSigning = testSigning();
    s.machine = machine();
    s.displays = readDisplays(s.ours ? s.driverDesc : std::wstring());
    return s;
}

}  // namespace panel
