// SPDX-License-Identifier: MIT
// What the panel reads in its own process: the VirtIO GPU and its driver (SetupAPI, the driver's registry key, the
// device's problem code), vgd as Apps & features lists it, test signing, the displays on the adapter, and the guest:
// the virtual machine from the firmware's SMBIOS tables, Windows, the processors, memory, page files, mice and keyboards.
#include "panel.h"

#include <initguid.h>  // defines the class GUIDs and property keys below

#include <devguid.h>
#include <devpkey.h>

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

// SMBIOS type 1 (System Information): manufacturer and product name, and the version, which is QEMU's machine type.
void readMachine(SystemInfo& s)
{
    constexpr DWORD kRsmb = 0x52534d42;  // 'RSMB'
    UINT size = GetSystemFirmwareTable(kRsmb, 0, nullptr, 0);
    if (!size)
        return;
    std::vector<BYTE> buf(size);
    GetSystemFirmwareTable(kRsmb, 0, buf.data(), size);
    const BYTE* p = buf.data() + 8;  // RawSMBIOSData: 4 bytes of versions, a DWORD length, then the table
    const BYTE* end = buf.data() + std::min<size_t>(size, 8 + *reinterpret_cast<const DWORD*>(buf.data() + 4));
    while (p + 4 <= end) {
        BYTE type = p[0], length = p[1];
        const char* strings = reinterpret_cast<const char*>(p + length);
        auto string = [&](BYTE index) -> std::wstring {
            const char* str = strings;
            for (BYTE i = 1; index && i < index && str < reinterpret_cast<const char*>(end); i++)
                str += strlen(str) + 1;
            return index && str < reinterpret_cast<const char*>(end) ? widen(str) : std::wstring();
        };
        if (type == 1 && length >= 8) {
            s.machine = string(p[4]);
            std::wstring product = string(p[5]);
            if (!product.empty())
                s.machine += (s.machine.empty() ? L"" : L" ") + product;
            s.machineType = string(p[6]);
            return;
        }
        // the next structure follows the strings' double NUL
        const BYTE* q = p + length;
        while (q + 1 < end && (q[0] || q[1]))
            q++;
        p = q + 2;
    }
}

// "Windows 11 Pro 24H2, build 26100.2314". ProductName still says Windows 10 on 11, which starts at build 22000.
std::wstring windowsVersion()
{
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring name = regString(HKEY_LOCAL_MACHINE, key, L"ProductName");
    std::wstring build = regString(HKEY_LOCAL_MACHINE, key, L"CurrentBuild");
    std::wstring release = regString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    if (_wtoi(build.c_str()) >= 22000 && name.compare(0, 10, L"Windows 10") == 0)
        name.replace(8, 2, L"11");
    DWORD ubr = 0, size = sizeof(ubr);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size) == ERROR_SUCCESS)
        build += L"." + std::to_wstring(ubr);
    if (!release.empty())
        name += L" " + release;
    return name + L", build " + build;
}

void readProcessors(SystemInfo& s)
{
    s.cpu = regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      L"ProcessorNameString");
    while (!s.cpu.empty() && s.cpu.back() == L' ')
        s.cpu.pop_back();
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &size);
    std::vector<BYTE> buf(size);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!size || !GetLogicalProcessorInformationEx(RelationAll, info, &size))
        return;
    for (DWORD at = 0; at < size; at += info->Size) {
        info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
        if (info->Relationship == RelationProcessorPackage) {
            s.sockets++;
        } else if (info->Relationship == RelationProcessorCore) {
            s.cores++;
            for (WORD g = 0; g < info->Processor.GroupCount; g++)
                for (KAFFINITY m = info->Processor.GroupMask[g].Mask; m; m &= m - 1)
                    s.threads++;
        }
    }
}

// The page files' total size, from SystemPageFileInformation.
ULONGLONG swapSize()
{
    struct PageFile {
        ULONG next, totalSize, totalInUse, peakUsage;  // sizes in pages
        UNICODE_STRING name;
    };
    std::vector<BYTE> buf(4096);
    ULONG length = 0;
    if (NtQuerySystemInformation(SYSTEM_INFORMATION_CLASS(18), buf.data(), buf.size(), &length) != 0 || !length)
        return 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONGLONG pages = 0;
    for (size_t at = 0;;) {
        const auto* f = reinterpret_cast<const PageFile*>(buf.data() + at);
        pages += f->totalSize;
        if (!f->next)
            break;
        at += f->next;
    }
    return pages * si.dwPageSize;
}

void readMemory(SystemInfo& s)
{
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb))
        s.memory = kb * 1024;
    MEMORYSTATUSEX m = {sizeof(m)};
    if (GlobalMemoryStatusEx(&m)) {
        s.usableMemory = m.ullTotalPhys;
        if (!s.memory)
            s.memory = m.ullTotalPhys;
    }
    s.swap = swapSize();
}

std::wstring deviceString(DEVINST inst, const DEVPROPKEY& key)
{
    wchar_t buf[256];
    ULONG size = sizeof(buf);
    DEVPROPTYPE type;
    if (CM_Get_DevNode_PropertyW(inst, &key, &type, reinterpret_cast<BYTE*>(buf), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return {};
    return buf;
}

// The present devices of a class, as Device Manager names them, with what the bus calls the device (QEMU's "QEMU USB
// Tablet", for one) where that says more. A HID mouse or keyboard has it on its parent, the HID device.
std::vector<std::wstring> devices(const GUID& cls)
{
    std::vector<std::wstring> out;
    HDEVINFO set = SetupDiGetClassDevsW(&cls, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return out;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        std::wstring name = deviceString(dev.DevInst, DEVPKEY_Device_FriendlyName);
        if (name.empty())
            name = deviceString(dev.DevInst, DEVPKEY_Device_DeviceDesc);
        std::wstring bus = deviceString(dev.DevInst, DEVPKEY_Device_BusReportedDeviceDesc);
        DEVINST parent;
        if (bus.empty() && CM_Get_Parent(&parent, dev.DevInst, 0) == CR_SUCCESS)
            bus = deviceString(parent, DEVPKEY_Device_BusReportedDeviceDesc);
        if (!bus.empty() && bus != name)
            name += L" (" + bus + L")";
        out.push_back(name);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

}  // namespace

HKEY openDriverKey(REGSAM access)
{
    HKEY out = nullptr;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return nullptr;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; !out && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (!isVirtioGpu(set, &dev))
            continue;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, access | KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        if (regString(key, nullptr, L"ProviderName") == kProvider)
            out = key;
        else
            RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

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
    readMachine(s);
    s.windows = windowsVersion();
    readProcessors(s);
    readMemory(s);
    s.mice = devices(GUID_DEVCLASS_MOUSE);
    s.keyboards = devices(GUID_DEVCLASS_KEYBOARD);
    s.displays = readDisplays(s.ours ? s.driverDesc : std::wstring());
    return s;
}

}  // namespace panel
