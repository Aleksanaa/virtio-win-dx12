// SPDX-License-Identifier: MIT
// Installs and uninstalls the driver package.
//
// Install: unpack the package (the cabinet in our resources) into %ProgramData%\Venera\staging; catalog it and sign the
// catalog with a certificate made on this computer and trusted there (so PnP installs it without asking); install it
// on the VirtIO GPU from the next boot, in place of whatever drives it now (install_on_devices); install the control
// panel in Program Files, with a Start menu shortcut and a desktop context-menu entry; and register in Apps & features
// with a copy of this executable as the uninstaller. At that boot, the copy (`/cleanup`, a one-shot task) removes the
// other packages for the device from the driver store and what yttrium's package left in System32.
//
// Uninstall: switch the VirtIO GPU to Microsoft Basic Display from the next boot, the same way, and remove the
// resolution service, the certificate, the control panel's shortcuts and the Apps & features entry. At that boot the
// cleanup removes our packages, the control panel and the uninstaller.
//
// An update from the earlier vgd builds (the product's name before Venera) removes what they left: Program Files\vgd,
// ProgramData\vgd, their Apps & features entry and settings, their certificate and their scheduled cleanup. Their
// driver package goes with the other packages the cleanup removes at the next boot.
//
// The kernel loads viogpu3d.sys on its own test signature (test signing on), whoever made it; the catalog's signature
// only serves PnP, which wants a publisher it trusts. That is why the certificate can be local: its private key is
// created on this computer, cannot be exported, and nobody else holds one that Windows here trusts.
#include "setup.h"

#include <cryptuiapi.h>
#include <mscat.h>
#include <ncrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <setupapi.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// setupapi.dll; MinGW's setupapi.h lacks it.
extern "C" BOOL WINAPI SetupGetInfDriverStoreLocationW(PCWSTR FileName, PSP_ALTPLATFORM_INFO_V2 AlternatePlatformInfo,
                                                       PCWSTR LocaleName, PWSTR ReturnBuffer, DWORD ReturnBufferSize,
                                                       PDWORD RequiredSize);

namespace {

constexpr wchar_t kKeyName[] = L"venera-test-signing";
constexpr wchar_t kSetupName[] = L"venera-setup.exe";
// Earlier builds called the product vgd; an update from one removes what it left (remove_old_vgd).
constexpr wchar_t kOldCertSubject[] = L"vgd test signing";
constexpr wchar_t kOldCleanupTask[] = L"vgd cleanup";

std::wstring staging_dir()
{
    return known_folder(FOLDERID_ProgramData) + L"\\Venera\\staging";
}

std::wstring install_dir()
{
    return known_folder(FOLDERID_ProgramFiles) + L"\\Venera";
}

// --- The payload

UINT CALLBACK on_cab_file(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR)
{
    if (notification != SPFILENOTIFY_FILEINCABINET)
        return NO_ERROR;
    auto dir = static_cast<const std::wstring*>(context);
    auto info = reinterpret_cast<FILE_IN_CABINET_INFO_W*>(param1);
    std::wstring target = *dir + L"\\" + info->NameInCabinet;
    if (target.size() >= MAX_PATH)
        return FILEOP_ABORT;
    wcscpy(info->FullTargetName, target.c_str());
    return FILEOP_DOIT;
}

// Unpacks the package into a fresh `dir` and returns the names of its files.
std::vector<std::wstring> unpack(const std::wstring& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec)
        fail(L"Creating " + dir + L" failed", ec.value());

    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data)
        fail(L"This installer carries no driver package");
    std::wstring cab = dir + L"\\payload.cab";
    {
        std::ofstream f(fs::path(cab), std::ios::binary);
        f.write(static_cast<const char*>(LockResource(data)), SizeofResource(nullptr, res));
        if (!f)
            fail(L"Writing " + cab + L" failed", ERROR_WRITE_FAULT);
    }
    if (!SetupIterateCabinetW(cab.c_str(), 0, on_cab_file, const_cast<std::wstring*>(&dir)))
        fail(L"Unpacking the driver package failed");
    fs::remove(cab, ec);

    std::vector<std::wstring> files;
    for (const auto& entry : fs::directory_iterator(dir))
        files.push_back(entry.path().filename().wstring());
    log(format(L"Unpacked %zu files into %ls", files.size(), dir.c_str()));
    return files;
}

// --- The signing certificate

HCERTSTORE open_store(const wchar_t* name)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, name);
    if (!store)
        fail(format(L"Opening the %ls certificate store failed", name));
    return store;
}

// The certificate with `subject` in `store` (any one), or null.
PCCERT_CONTEXT find_cert(HCERTSTORE store, const wchar_t* subject, PCCERT_CONTEXT prev = nullptr)
{
    return CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, subject, prev);
}

bool usable(PCCERT_CONTEXT cert)
{
    if (CertVerifyTimeValidity(nullptr, cert->pCertInfo) != 0)
        return false;
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
    DWORD spec = 0;
    BOOL free_key = FALSE;
    if (!CryptAcquireCertificatePrivateKey(cert, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
                                           nullptr, &key, &spec, &free_key))
        return false;
    if (free_key) {
        if (spec == CERT_NCRYPT_KEY_SPEC)
            NCryptFreeObject(key);
        else
            CryptReleaseContext(key, 0);
    }
    return true;
}

void check(SECURITY_STATUS status, const wchar_t* what)
{
    if (status != ERROR_SUCCESS)
        fail(what, status);
}

// A self-signed code signing certificate with an RSA key in the machine's key store, valid for 20 years.
PCCERT_CONTEXT create_cert()
{
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE key = 0;
    check(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0), L"Opening the key storage provider failed");
    check(NCryptCreatePersistedKey(provider, &key, BCRYPT_RSA_ALGORITHM, kKeyName, 0,
                                   NCRYPT_MACHINE_KEY_FLAG | NCRYPT_OVERWRITE_KEY_FLAG),
          L"Creating the signing key failed");
    DWORD bits = 2048;
    check(NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof(bits), 0),
          L"Setting the signing key's length failed");
    check(NCryptFinalizeKey(key, 0), L"Creating the signing key failed");

    std::wstring x500 = std::wstring(L"CN=") + kCertSubject;
    DWORD size = 0;
    CertStrToNameW(X509_ASN_ENCODING, x500.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &size, nullptr);
    std::vector<BYTE> name(size);
    if (!CertStrToNameW(X509_ASN_ENCODING, x500.c_str(), CERT_X500_NAME_STR, nullptr, name.data(), &size, nullptr))
        fail(L"Encoding the certificate's subject failed");
    CERT_NAME_BLOB subject = {size, name.data()};

    LPSTR usage = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
    CERT_ENHKEY_USAGE eku = {1, &usage};
    BYTE* eku_data = nullptr;
    DWORD eku_size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                             &eku_data, &eku_size))
        fail(L"Encoding the certificate's key usage failed");
    CERT_EXTENSION extension = {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, {eku_size, eku_data}};
    CERT_EXTENSIONS extensions = {1, &extension};

    CRYPT_KEY_PROV_INFO key_info = {const_cast<LPWSTR>(kKeyName), const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER), 0,
                                    NCRYPT_MACHINE_KEY_FLAG, 0, nullptr, 0};
    CRYPT_ALGORITHM_IDENTIFIER algorithm = {const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
    SYSTEMTIME start, end;
    GetSystemTime(&start);
    end = start;
    end.wYear += 20;
    if (end.wMonth == 2 && end.wDay == 29)
        end.wDay = 28;
    PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(key, &subject, 0, &key_info, &algorithm, &start, &end,
                                                        &extensions);
    DWORD error = GetLastError();
    LocalFree(eku_data);
    NCryptFreeObject(key);
    NCryptFreeObject(provider);
    if (!cert)
        fail(L"Creating the signing certificate failed", error);
    return cert;
}

// Our usable certificate from the machine's personal store, created first if there is none; trusted as a root and a
// publisher either way.
PCCERT_CONTEXT signing_cert()
{
    HCERTSTORE my = open_store(L"My");
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = find_cert(my, kCertSubject, cert)) && !usable(cert)) {
    }
    if (!cert) {
        PCCERT_CONTEXT created = create_cert();
        if (!CertAddCertificateContextToStore(my, created, CERT_STORE_ADD_REPLACE_EXISTING, &cert))
            fail(L"Storing the signing certificate failed");
        CertFreeCertificateContext(created);
        log(L"Created the signing certificate CN=" + std::wstring(kCertSubject));
    }
    CertCloseStore(my, 0);
    for (const wchar_t* name : {L"Root", L"TrustedPublisher"}) {
        HCERTSTORE store = open_store(name);
        if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, cert->pbCertEncoded, cert->cbCertEncoded,
                                              CERT_STORE_ADD_USE_EXISTING, nullptr))
            fail(format(L"Trusting the signing certificate (%ls) failed", name));
        CertCloseStore(store, 0);
    }
    return cert;
}

void remove_certs(const wchar_t* subject)
{
    for (const wchar_t* name : {L"My", L"Root", L"TrustedPublisher"}) {
        HCERTSTORE store = open_store(name);
        while (PCCERT_CONTEXT cert = find_cert(store, subject)) {
            // The private key goes with the certificate.
            HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
            DWORD spec = 0;
            BOOL free_key = FALSE;
            if (CryptAcquireCertificatePrivateKey(cert, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG,
                                                  nullptr, &key, &spec, &free_key)) {
                if (NCryptDeleteKey(key, 0) != ERROR_SUCCESS && free_key)
                    NCryptFreeObject(key);
            }
            CertDeleteCertificateFromStore(cert);  // frees cert
            log(format(L"Removed the certificate %ls from %ls", subject, name));
        }
        CertCloseStore(store, 0);
    }
}

// --- The catalog

std::wstring cdf_errors;

void WINAPI on_cdf_error(DWORD area, DWORD error, WCHAR* line)
{
    cdf_errors += format(L"%ls: area %lu, error 0x%lx\n", line ? line : L"(header)", area, error);
}

std::string narrow(const std::wstring& s)
{
    int size = WideCharToMultiByte(CP_ACP, 0, s.c_str(), s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), s.size(), out.data(), size, nullptr, nullptr);
    return out;
}

// venera.cat in `dir`, listing `files` by their SHA-256 hashes, as MakeCat would make it from a catalog definition.
void make_catalog(const std::wstring& dir, const std::vector<std::wstring>& files)
{
    std::string cdf = "[CatalogHeader]\r\nName=venera.cat\r\nResultDir=" + narrow(dir) +
                      "\r\nPublicVersion=0x00000001\r\nCatalogVersion=2\r\nHashAlgorithms=SHA256\r\n"
                      "EncodingType=0x00010001\r\nCATATTR1=0x10010001:OSAttr:2:10.0\r\n\r\n[CatalogFiles]\r\n";
    for (const auto& f : files)
        cdf += "<hash>" + narrow(f) + "=" + narrow(dir + L"\\" + f) + "\r\n";
    std::wstring path = dir + L"\\venera.cdf";
    {
        std::ofstream out(fs::path(path), std::ios::binary);
        out << cdf;
    }

    cdf_errors.clear();
    CRYPTCATCDF* catalog = CryptCATCDFOpen(path.data(), on_cdf_error);
    if (!catalog)
        fail(L"Creating the catalog failed: " + cdf_errors);
    CRYPTCATMEMBER* member = nullptr;
    LPWSTR tag = nullptr;
    while ((tag = CryptCATCDFEnumMembersByCDFTagEx(catalog, tag, on_cdf_error, &member, TRUE, nullptr))) {
    }
    if (!CryptCATCDFClose(catalog) || !cdf_errors.empty())
        fail(L"Creating the catalog failed: " + cdf_errors);
    DeleteFileW(path.c_str());
}

void sign(const std::wstring& file, PCCERT_CONTEXT cert)
{
    CRYPTUI_WIZ_DIGITAL_SIGN_EXTENDED_INFO extended = {sizeof(extended)};
    extended.pszHashAlg = szOID_NIST_sha256;
    CRYPTUI_WIZ_DIGITAL_SIGN_INFO info = {sizeof(info)};
    info.dwSubjectChoice = CRYPTUI_WIZ_DIGITAL_SIGN_SUBJECT_FILE;
    info.pwszFileName = file.c_str();
    info.dwSigningCertChoice = CRYPTUI_WIZ_DIGITAL_SIGN_CERT;
    info.pSigningCertContext = cert;
    info.dwAdditionalCertChoice = 0;
    info.pSignExtInfo = &extended;
    if (!CryptUIWizDigitalSign(CRYPTUI_WIZ_NO_UI, nullptr, nullptr, &info, nullptr))
        fail(L"Signing " + file + L" failed");
}

// --- Packages and leftovers

std::wstring inf_dir()
{
    return known_folder(FOLDERID_Windows) + L"\\INF";
}

// Removes a package from the driver store; forced when a device still names it (such as the VirtIO GPU at a PCI
// slot it no longer sits in), which then gets the best package left when it comes back.
void remove_package(const DriverPackage& p)
{
    if (SetupUninstallOEMInfW(p.inf.c_str(), 0, nullptr) ||
        (GetLastError() == ERROR_INF_IN_USE_BY_DEVICES && SetupUninstallOEMInfW(p.inf.c_str(), SUOI_FORCEDELETE, nullptr))) {
        log(format(L"Removed %ls (%ls, %ls) from the driver store", p.inf.c_str(), p.provider.c_str(), p.version.c_str()));
        return;
    }
    log(format(L"Could not remove %ls: %ls", p.inf.c_str(), error_text(GetLastError()).c_str()));
}

// Deletes a file now or, if it is loaded, at the next restart.
void delete_file(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    if (DeleteFileW(path.c_str()))
        log(L"Deleted " + path);
    else if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
        log(L"Deleting " + path + L" at the next restart (in use)");
    else
        log(L"Could not delete " + path + L": " + error_text(GetLastError()));
}

// Deletes a directory and what is in it now, or at the next restart what is in use.
void remove_dir(const std::wstring& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec))
            remove_dir(entry.path().wstring());
        else
            delete_file(entry.path().wstring());
    }
    if (RemoveDirectoryW(dir.c_str())) {
        log(L"Removed " + dir);
    } else {
        MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        log(L"Removing " + dir + L" at the next restart");
    }
}

// What yttrium's package leaves once it is gone: the user-mode drivers its INF copied to System32 and SysWOW64 (our
// package has its own viogpu_wgl.dll in the DriverStore; its opengl32.dll is left alone, that name is Windows' own),
// and its Vulkan ICD registrations.
void remove_yttrium_leftovers()
{
    static const wchar_t* names[] = {
        L"viogpu_d3d10.dll", L"yttrium12.dll", L"viogpu_wgl.dll", L"libEGL.dll", L"libGLESv1_CM.dll",
        L"libGLESv2.dll", L"libvulkan_virtio.dll", L"vulkan_lvp.dll", L"lvp_icd.x86_64.json",
        L"virtio_icd.x86_64.json", L"lvp_icd.x86.json", L"virtio_icd.x86.json"};
    std::wstring windows = known_folder(FOLDERID_Windows);
    for (const wchar_t* dir : {L"\\System32\\", L"\\SysWOW64\\"})
        for (const wchar_t* name : names)
            delete_file(windows + dir + name);

    for (const wchar_t* path : {L"SOFTWARE\\Khronos\\Vulkan\\Drivers", L"SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers"}) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
            continue;
        std::vector<std::wstring> doomed;
        wchar_t name[1024];
        for (DWORD i = 0, size = ARRAYSIZE(name);
             RegEnumValueW(key, i, name, &size, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             i++, size = ARRAYSIZE(name)) {
            const wchar_t* file = wcsrchr(name, L'\\');
            file = file ? file + 1 : name;
            if (_wcsnicmp(file, L"lvp_icd.", 8) == 0 || _wcsnicmp(file, L"virtio_icd.", 11) == 0)
                doomed.push_back(name);
        }
        for (const auto& v : doomed) {
            RegDeleteValueW(key, v.c_str());
            log(L"Removed the Vulkan ICD registration " + v);
        }
        RegCloseKey(key);
    }
}

// Stops and deletes a service that runs from one of our packages (its binary in a venera.inf_* DriverStore folder).
void delete_our_service(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    SC_HANDLE service = scm ? OpenServiceW(scm, name, SERVICE_ALL_ACCESS) : nullptr;
    if (service) {
        std::vector<BYTE> buf(8192);
        DWORD size = 0;
        auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        if (QueryServiceConfigW(service, config, buf.size(), &size) && config->lpBinaryPathName &&
            StrStrIW(config->lpBinaryPathName, L"\\venera.inf_")) {
            SERVICE_STATUS status;
            ControlService(service, SERVICE_CONTROL_STOP, &status);
            if (DeleteService(service))
                log(format(L"Deleted the %ls service", name));
            else
                log(format(L"Could not delete the %ls service: %ls", name, error_text(GetLastError()).c_str()));
        }
        CloseServiceHandle(service);
    }
    if (scm)
        CloseServiceHandle(scm);
}

// Earlier builds called the product vgd (and its settings key vgd3d12). Installing over one removes its uninstaller,
// its Apps & features entry, its settings, log and staging directory, its certificate, and a cleanup it scheduled, so
// that only Venera is left. Its driver package goes with the other packages the cleanup removes at the next boot.
void remove_old_vgd()
{
    std::wstring old_install = known_folder(FOLDERID_ProgramFiles) + L"\\vgd";
    std::wstring old_data = known_folder(FOLDERID_ProgramData) + L"\\vgd";
    std::error_code ec;
    bool present = fs::exists(old_install, ec) || fs::exists(old_data, ec);
    for (const wchar_t* key : {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\vgd", L"SOFTWARE\\vgd3d12"}) {
        HKEY probe;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &probe) == ERROR_SUCCESS) {
            present = true;
            RegCloseKey(probe);
        }
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    }
    // The old certificate goes even if the rest was deleted by hand: nothing can use it any more.
    remove_certs(kOldCertSubject);
    if (!present)
        return;
    // A cleanup an old install scheduled would run its uninstaller from the directory removed below.
    run(L"schtasks.exe", format(L"/delete /f /tn \"%ls\"", kOldCleanupTask));
    remove_dir(old_install);
    remove_dir(old_data);
    log(L"Removed what vgd left: its uninstaller, Apps & features entry, settings, log and certificate");
}

// --- Apps & features

void set_string(HKEY key, const wchar_t* name, const std::wstring& value)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   (value.size() + 1) * sizeof(wchar_t));
}

void set_dword(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

// Copies this executable to Program Files as the uninstaller, and lists the product in Apps & features.
void register_product(const std::wstring& version, DWORD size_kb)
{
    std::wstring dir = install_dir();
    std::wstring exe = dir + L"\\" + kSetupName;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::equivalent(self_path(), exe, ec) && !CopyFileW(self_path().c_str(), exe.c_str(), FALSE))
        fail(L"Copying the uninstaller to " + exe + L" failed");

    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        fail(L"Registering in Apps & features failed");
    SYSTEMTIME now;
    GetLocalTime(&now);
    set_string(key, L"DisplayName", L"venera: Direct3D, Vulkan and OpenGL for the VirtIO GPU");
    set_string(key, L"DisplayVersion", version);
    set_string(key, L"Publisher", kProvider);
    set_string(key, L"URLInfoAbout", kProjectUrl);
    set_string(key, L"InstallLocation", dir);
    set_string(key, L"DisplayIcon", exe);
    set_string(key, L"InstallDate", format(L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay));
    set_string(key, L"UninstallString", L"\"" + exe + L"\" /uninstall");
    set_string(key, L"QuietUninstallString", L"\"" + exe + L"\" /uninstall /S");
    set_dword(key, L"NoModify", 1);
    set_dword(key, L"NoRepair", 1);
    set_dword(key, L"EstimatedSize", size_kb);
    RegCloseKey(key);
    log(L"Registered " + version + L" in Apps & features, uninstaller " + exe);
}

void unregister_product()
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    // The UMDs' log settings (tests/win/venera-test.ps1 Install sets them).
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Venera");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Venera");
}

// --- The control panel

std::wstring start_menu_dir()
{
    return known_folder(FOLDERID_CommonPrograms) + L"\\Venera";
}

// Copies the control panel out of the unpacked package. A running panel holds its executable open, in which case the
// new one replaces it at the next boot.
void install_panel(const std::wstring& staging)
{
    std::wstring dir = install_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        fail(L"Creating " + dir + L" failed", ec.value());
    for (const wchar_t* name : {kPanelExe, kPanelCom}) {
        std::wstring from = staging + L"\\" + name;
        std::wstring to = dir + L"\\" + name;
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            log(L"Installed " + to);
            continue;
        }
        DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
            fail(L"Installing " + to + L" failed", error);
        std::wstring pending = to + L".new";
        if (!CopyFileW(from.c_str(), pending.c_str(), FALSE) ||
            !MoveFileExW(pending.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT))
            fail(L"Installing " + to + L" at the next reboot failed", GetLastError());
        log(L"Installing " + to + L" at the next reboot: the running control panel holds it");
    }
}

// Joins or leaves the current thread's COM apartment (install runs on the wizard's worker thread).
struct Apartment {
    bool joined = false;
    Apartment()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        joined = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)  // already in another apartment: its owner leaves it
            fail(L"Starting COM failed", static_cast<DWORD>(hr));
    }
    ~Apartment()
    {
        if (joined)
            CoUninitialize();
    }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

void create_shortcut(const std::wstring& link, const std::wstring& target)
{
    IShellLinkW* shell_link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&shell_link));
    if (SUCCEEDED(hr)) {
        shell_link->SetPath(target.c_str());
        shell_link->SetWorkingDirectory(install_dir().c_str());
        shell_link->SetDescription(L"Venera's Direct3D, Vulkan and OpenGL settings and tests");
        shell_link->SetIconLocation(target.c_str(), 0);
        IPersistFile* file = nullptr;
        hr = shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
        if (SUCCEEDED(hr)) {
            hr = file->Save(link.c_str(), TRUE);
            file->Release();
        }
        shell_link->Release();
    }
    if (FAILED(hr))
        fail(L"Creating the shortcut " + link + L" failed", static_cast<DWORD>(hr));
}

// A Start menu shortcut and an entry in the desktop's context menu (right-click on the desktop), for every user.
void create_shortcuts()
{
    std::wstring panel = install_dir() + L"\\" + kPanelExe;
    std::wstring menu = start_menu_dir();
    std::error_code ec;
    fs::create_directories(menu, ec);
    if (ec)
        fail(L"Creating " + menu + L" failed", ec.value());

    {
        Apartment apartment;
        create_shortcut(menu + L"\\" + kPanelName + L".lnk", panel);
    }

    HKEY key;
    DWORD error = RegCreateKeyExW(HKEY_CLASSES_ROOT, L"DesktopBackground\\Shell\\Venera", 0, nullptr, 0, KEY_WRITE,
                                  nullptr, &key, nullptr);
    if (error != ERROR_SUCCESS)
        fail(std::wstring(L"Adding ") + kPanelName + L" to the desktop's context menu failed", error);
    set_string(key, nullptr, kPanelName);
    set_string(key, L"Icon", panel);
    HKEY command = nullptr;
    error = RegCreateKeyExW(key, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &command, nullptr);
    RegCloseKey(key);
    if (error != ERROR_SUCCESS)
        fail(std::wstring(L"Adding ") + kPanelName + L" to the desktop's context menu failed", error);
    set_string(command, nullptr, L"\"" + panel + L"\"");
    RegCloseKey(command);
    log(std::wstring(L"Added ") + kPanelName + L" to the Start menu and the desktop's context menu");
}

void remove_shortcuts()
{
    std::wstring menu = start_menu_dir();
    std::error_code ec;
    if (fs::remove(menu + L"\\" + kPanelName + L".lnk", ec))
        log(L"Removed the control panel's Start menu shortcut");
    fs::remove(menu, ec);  // only when it is empty
    if (RegDeleteTreeW(HKEY_CLASSES_ROOT, L"DesktopBackground\\Shell\\Venera") == ERROR_SUCCESS)
        log(L"Removed the control panel from the desktop's context menu");
}

void remove_install_dir()
{
    remove_dir(install_dir());  // the running uninstaller goes at the next restart
}

// --- The device and the boot after

constexpr wchar_t kCleanupTask[] = L"Venera cleanup";

bool runs_ours(HDEVINFO set, SP_DEVINFO_DATA* dev)
{
    HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE)
        return false;
    wchar_t provider[256];
    DWORD size = sizeof(provider);
    bool ours = RegGetValueW(key, nullptr, L"ProviderName", RRF_RT_REG_SZ, nullptr, provider, &size) == ERROR_SUCCESS &&
                _wcsicmp(provider, kProvider) == 0;
    RegCloseKey(key);
    return ours;
}

// Installs the driver from `inf` (in the driver store) on each present VirtIO GPU (with `only_ours`, those running on
// Venera) as its driver from the next boot: the class installer writes the device's configuration but does not restart
// the device (DI_DONOTCALLCONFIGMG). Restarting a display device under the running desktop crashes the processes
// that have the old user-mode drivers loaded (DWM in the old Venus ICD), and the display did not always come back.
// Selecting the driver from that one INF also makes it win over a better-signed one (Red Hat's WHQL-signed viogpudo).
int install_on_devices(const std::wstring& inf, bool only_ours)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        fail(L"Listing the PCI devices failed");
    int installed = 0;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (!is_virtio_gpu(set, &dev) || (only_ours && !runs_ours(set, &dev)))
            continue;
        SP_DEVINSTALL_PARAMS_W params = {sizeof(params)};
        SetupDiGetDeviceInstallParamsW(set, &dev, &params);
        wcsncpy(params.DriverPath, inf.c_str(), MAX_PATH - 1);
        params.Flags |= DI_ENUMSINGLEINF | DI_DONOTCALLCONFIGMG | DI_QUIETINSTALL;
        if (!SetupDiSetDeviceInstallParamsW(set, &dev, &params) ||
            !SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER) ||
            !SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, set, &dev) ||
            !SetupDiCallClassInstaller(DIF_INSTALLDEVICE, set, &dev)) {
            DWORD error = GetLastError();
            SetupDiDestroyDeviceInfoList(set);
            // A failure half way can leave the device with no driver configured.
            fail(L"Installing " + inf + L" on the VirtIO GPU failed; it may have no driver at the next boot, so "
                 L"run the installer again before rebooting. The error", error);
        }
        installed++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return installed;
}

// Runs `venera-setup.exe /cleanup` once at the next boot, as SYSTEM: the packages an install replaces, or an uninstall
// removes, are in use until then.
void schedule_cleanup()
{
    std::wstring exe = install_dir() + L"\\" + kSetupName;
    DWORD code = run(L"schtasks.exe", format(L"/create /f /tn \"%ls\" /sc onstart /ru SYSTEM /rl highest "
                                             L"/tr \"\\\"%ls\\\" /cleanup\"", kCleanupTask, exe.c_str()));
    if (code != 0)
        fail(format(L"Scheduling the cleanup at the next boot failed (schtasks exit code %lu)", code), ERROR_SUCCESS);
}

}  // namespace

bool install(const SystemState& state, const std::wstring& version, const Progress& progress)
{
    std::wstring dir = staging_dir();
    progress(5, L"Unpacking the driver package");
    std::vector<std::wstring> files = unpack(dir);
    DWORD size_kb = 0;
    for (const auto& f : files)
        size_kb += fs::file_size(dir + L"\\" + f) / 1024;

    progress(25, state.cert_exists ? L"Checking the signing certificate" : L"Creating the signing certificate");
    PCCERT_CONTEXT cert = signing_cert();
    progress(35, L"Signing the driver package");
    make_catalog(dir, files);
    sign(dir + L"\\venera.cat", cert);
    CertFreeCertificateContext(cert);

    progress(45, L"Adding the driver package to the driver store");
    std::wstring inf = dir + L"\\venera.inf";
    wchar_t published[MAX_PATH];
    PWSTR published_name = nullptr;
    if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_PATH, 0, published, ARRAYSIZE(published), nullptr,
                          &published_name))
        fail(L"Adding the driver package to the driver store failed");
    std::wstring oem = published_name;
    // The device installs from the driver store's copy: %13% (where the INF puts its files) is that folder.
    wchar_t stored[MAX_PATH];
    if (!SetupGetInfDriverStoreLocationW(published, nullptr, nullptr, stored, ARRAYSIZE(stored), nullptr))
        fail(L"Finding " + oem + L" in the driver store failed");
    log(L"Driver store: " + oem + L", " + stored);

    progress(55, L"Installing the driver on the VirtIO GPU, from the next boot");
    int devices = install_on_devices(stored, false);
    if (devices == 0)
        fail(L"There is no VirtIO GPU to install the driver on", ERROR_SUCCESS);
    log(format(L"Installed on %d VirtIO GPU(s); it starts at the next boot", devices));

    progress(80, L"Registering in Apps & features");
    register_product(version, size_kb);
    progress(82, L"Removing what earlier vgd builds left");
    remove_old_vgd();
    progress(85, L"Installing the control panel");
    install_panel(dir);
    progress(90, L"Adding the control panel to the Start menu and the desktop's menu");
    create_shortcuts();
    progress(95, L"Scheduling the removal of the drivers it replaces at the next boot");
    schedule_cleanup();
    std::error_code ec;
    fs::remove_all(dir, ec);
    progress(100, L"Done");
    return true;
}

void cleanup()
{
    SystemState s = inspect();
    log(s.summary());
    if (s.devices > 0 && _wcsicmp(s.device_provider.c_str(), kProvider) == 0) {
        // After an install, once the VirtIO GPU runs on our package: its other packages, ours included, are not in
        // use any more.
        for (const auto& p : s.packages)
            if (_wcsicmp(p.inf.c_str(), s.device_inf.c_str()) != 0)
                remove_package(p);
        remove_yttrium_leftovers();
    } else if (s.installed_version.empty()) {
        // After an uninstall, once the VirtIO GPU runs on Basic Display: our packages, and this uninstaller.
        for (const auto& p : s.packages)
            if (p.ours())
                remove_package(p);
        remove_install_dir();
    } else {
        log(L"The VirtIO GPU does not run on venera: nothing to clean up");
    }
    run(L"schtasks.exe", format(L"/delete /f /tn \"%ls\"", kCleanupTask));
}

bool uninstall(const Progress& progress)
{
    progress(10, L"Switching the VirtIO GPU to Microsoft Basic Display, from the next boot");
    int devices = install_on_devices(inf_dir() + L"\\display.inf", true);
    log(format(L"Switched %d VirtIO GPU(s) to Microsoft Basic Display; it starts at the next boot", devices));
    progress(40, L"Removing the resolution service");
    delete_our_service(L"vgpusrv");
    progress(55, L"Removing the signing certificate");
    remove_certs(kCertSubject);
    progress(70, L"Removing the Apps & features entry");
    unregister_product();
    progress(75, L"Removing the control panel's shortcuts");
    remove_shortcuts();
    // Our packages stay in use until the next boot, and the cleanup then runs from the uninstaller in Program Files.
    progress(85, L"Scheduling the removal of the driver packages at the next boot");
    schedule_cleanup();
    progress(100, L"Done");
    return true;
}

void enable_test_signing()
{
    // A BCD change makes BitLocker ask for the recovery key at the next boot; suspend it for that one restart. This
    // fails harmlessly when BitLocker is off or not there.
    wchar_t drive[MAX_PATH] = L"C:";
    GetEnvironmentVariableW(L"SystemDrive", drive, ARRAYSIZE(drive));
    run(L"manage-bde.exe", format(L"-protectors -disable %ls -RebootCount 1", drive));
    DWORD code = run(L"bcdedit.exe", L"/set {current} testsigning on");
    if (code != 0)
        fail(format(L"bcdedit /set testsigning on failed with exit code %lu; %ls has its output", code,
                    log_path().c_str()), ERROR_SUCCESS);
}
