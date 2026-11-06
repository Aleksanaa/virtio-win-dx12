// SPDX-License-Identifier: MIT
// Reads the system state the installer decides on: Windows, the VirtIO GPU and its driver, Secure Boot, test signing
// and the other code integrity policies that affect test-signed drivers, and what is installed already.
#include "setup.h"

#include <setupapi.h>
#include <wincrypt.h>
#include <winternl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::wstring upper(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towupper(c); });
    return s;
}

std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

DWORD reg_dword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD fallback)
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return fallback;
    return v;
}

DWORD os_build()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW info = {sizeof(info)};
    if (!get || get(&info) != 0)
        return 0;
    return info.dwBuildNumber;
}

bool native_x64()
{
    USHORT process = 0, native = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &process, &native))
        return false;
    return native == IMAGE_FILE_MACHINE_AMD64;
}

ULONG code_integrity_options()
{
    SYSTEM_CODEINTEGRITY_INFORMATION info = {sizeof(info), 0};
    if (NtQuerySystemInformation(SystemCodeIntegrityInformation, &info, sizeof(info), nullptr) != 0)
        return 0;
    return info.CodeIntegrityOptions;
}

std::vector<std::wstring> multi_sz(const std::vector<BYTE>& buf)
{
    std::vector<std::wstring> out;
    auto p = reinterpret_cast<const wchar_t*>(buf.data());
    auto end = p + buf.size() / sizeof(wchar_t);
    while (p < end && *p) {
        std::wstring s(p);
        p += s.size() + 1;
        out.push_back(std::move(s));
    }
    return out;
}

// The present devices with kHardwareId among their hardware IDs, and the driver of the first.
void find_devices(SystemState& s)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        fail(L"Listing the PCI devices failed");
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        std::vector<BYTE> buf(4096);
        DWORD size = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, buf.data(), buf.size() - 4, &size))
            continue;
        buf.resize(size + 4);
        auto ids = multi_sz(buf);
        bool match = std::any_of(ids.begin(), ids.end(), [](const std::wstring& id) {
            return upper(id) == kHardwareId;
        });
        if (!match)
            continue;
        if (s.devices++ > 0)
            continue;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) {
            s.device_driver = L"(no driver)";
            continue;
        }
        s.device_driver = reg_string(key, nullptr, L"DriverDesc");
        s.device_provider = reg_string(key, nullptr, L"ProviderName");
        s.device_version = reg_string(key, nullptr, L"DriverVersion");
        s.device_inf = reg_string(key, nullptr, L"InfPath");
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
}

std::wstring inf_line(HINF inf, const wchar_t* section, const wchar_t* key)
{
    wchar_t buf[512];
    if (!SetupGetLineTextW(nullptr, inf, section, key, buf, ARRAYSIZE(buf), nullptr))
        return {};
    return buf;
}

// An INF's text, whichever encoding it is in (UTF-16 with a byte order mark, or ANSI).
std::wstring read_inf_text(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / sizeof(wchar_t));
    return std::wstring(raw.begin(), raw.end());
}

bool has_cert()
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG, L"My");
    if (!store)
        return false;
    PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W,
        kCertSubject, nullptr);
    if (cert)
        CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);
    return cert != nullptr;
}

}  // namespace

bool DriverPackage::ours() const
{
    return provider == kProvider;
}

std::vector<DriverPackage> gpu_packages()
{
    std::vector<DriverPackage> out;
    std::filesystem::path dir = known_folder(FOLDERID_Windows) + L"\\INF";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::wstring name = entry.path().filename().wstring();
        if (upper(name).rfind(L"OEM", 0) != 0 || upper(entry.path().extension().wstring()) != L".INF")
            continue;
        if (upper(read_inf_text(entry.path())).find(kDeviceIdPrefix) == std::wstring::npos)
            continue;
        HINF inf = SetupOpenInfFileW(entry.path().c_str(), nullptr, INF_STYLE_WIN4, nullptr);
        if (inf == INVALID_HANDLE_VALUE)
            continue;
        DriverPackage p = {name, inf_line(inf, L"Version", L"Provider"), inf_line(inf, L"Version", L"DriverVer"),
                           inf_line(inf, L"Version", L"Class")};
        SetupCloseInfFile(inf);
        if (_wcsicmp(p.cls.c_str(), L"Display") == 0 || _wcsicmp(p.cls.c_str(), L"Extension") == 0)
            out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const DriverPackage& a, const DriverPackage& b) { return a.inf < b.inf; });
    return out;
}

SystemState inspect()
{
    SystemState s;
    s.build = os_build();
    s.x64 = native_x64();
    find_devices(s);

    FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
    s.uefi = GetFirmwareType(&firmware) && firmware == FirmwareTypeUefi;
    s.secure_boot = s.uefi && reg_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                                        L"UEFISecureBootEnabled", 0) != 0;
    ULONG ci = code_integrity_options();
    s.test_signing = ci & CODEINTEGRITY_OPTION_TESTSIGN;
    s.hvci = ci & CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED;
    s.smart_app_control = reg_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy",
                                    L"VerifiedAndReputablePolicyState", 0);
    s.vulkan_runtime = GetFileAttributesW((system_dir() + L"\\vulkan-1.dll").c_str()) != INVALID_FILE_ATTRIBUTES;

    s.installed_version = reg_string(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion");
    s.packages = gpu_packages();
    s.cert_exists = has_cert();
    return s;
}

std::wstring SystemState::summary() const
{
    std::wstring out = format(L"Windows build %lu, %ls\n", build, x64 ? L"x64" : L"not x64");
    out += format(L"VirtIO GPU: %d present\n", devices);
    if (devices)
        out += format(L"  driver: %ls (%ls, %ls, %ls)\n", device_driver.c_str(), device_provider.c_str(),
                      device_version.c_str(), device_inf.c_str());
    out += format(L"Firmware: %ls, Secure Boot %ls\n", uefi ? L"UEFI" : L"BIOS", secure_boot ? L"on" : L"off");
    out += format(L"Test signing: %ls\n", test_signing ? L"on" : L"off");
    out += format(L"Memory integrity (HVCI): %ls\n", hvci ? L"on" : L"off");
    static const wchar_t* sac[] = {L"off", L"on", L"evaluation"};
    out += format(L"Smart App Control: %ls\n", smart_app_control < 3 ? sac[smart_app_control] : L"unknown");
    out += format(L"Vulkan Runtime (vulkan-1.dll): %ls\n", vulkan_runtime ? L"installed" : L"missing");
    out += format(L"Installed: %ls\n", installed_version.empty() ? L"no" : installed_version.c_str());
    for (const auto& p : packages)
        out += format(L"Driver store: %ls  %ls, %ls, %ls\n", p.inf.c_str(), p.provider.c_str(), p.cls.c_str(),
                      p.version.c_str());
    out += format(L"Signing certificate: %ls", cert_exists ? L"exists" : L"not created yet");
    return out;
}
