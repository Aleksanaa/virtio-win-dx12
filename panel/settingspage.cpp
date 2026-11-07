// SPDX-License-Identifier: MIT
// Settings, which need administrator rights to change:
//   Pointer: the KMD's hardware cursor settings in the VirtIO GPU's driver key, which it reads before each pointer
//   shape (viogpu_adapter.cpp); reloading the system cursors sends one, so a change shows at once.
//   Logging: the drivers' settings in HKLM\SOFTWARE\Venera (umd/common/log.h), which a program reads when it loads a
//   driver, so they apply to programs started afterwards.
// The command line's /set changes them too (cli.cpp), through listSettings and changeSetting.
#include "panel.h"

#include <tlhelp32.h>

#include <algorithm>

namespace panel {

using namespace theme;

namespace {

constexpr float kCardH = 68, kGap = 4;
constexpr wchar_t kKey[] = L"SOFTWARE\\Venera";
constexpr wchar_t kLogFile[] = L"C:\\venera\\venera.log";

std::wstring readSetting(const wchar_t* name)
{
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    DWORD v;
    size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return std::to_wstring(v);
    return L"";
}

// Empty removes the setting: the driver's default.
void writeSetting(const wchar_t* name, const std::wstring& value)
{
    if (value.empty())
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kKey, name);
    else
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kKey, name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * 2));
}

// The hardware cursor's settings, as pkg/venera.inf sets them by default; `cli` is the name /set knows.
struct PointerSetting {
    const wchar_t *cli, *name, *icon, *title, *desc;
    DWORD fallback;
};
const PointerSetting kPointer[] = {
    {L"cursor", L"HWCursor", L"\uE962", L"Hardware cursor", L"The host draws the pointer, so it keeps up with the mouse", 1},
    {L"cursor-swap-rb", L"HWCursorSwapRB", L"\uE790", L"Swap red and blue",
     L"For QEMU's GTK display, which shows the pointer's red as blue unless its virglrenderer is patched", 0},
};
constexpr int kPointerCount = int(std::size(kPointer));

// A level setting of a library the drivers use: the values it takes, by label; "" leaves its default.
struct Level {
    const wchar_t *cli, *name, *icon, *title, *desc;
    std::vector<std::pair<const wchar_t*, const wchar_t*>> choices;
};
const Level kLevels[] = {
    {L"mesa-log", L"MESA_LOG_LEVEL", L"\uE7F8", L"Vulkan driver messages", L"What Mesa and Venus print, shown in Logs",
     {{L"Default", L""}, {L"Errors", L"error"}, {L"Warnings", L"warning"}, {L"Information", L"info"},
      {L"Debug", L"debug"}}},
    {L"dxvk-log", L"DXVK_LOG_LEVEL", L"\uE7F4", L"D3D11 messages", L"",  // the description follows the log file
     {{L"Default", L""}, {L"None", L"none"}, {L"Errors", L"error"}, {L"Warnings", L"warn"}, {L"Information", L"info"},
      {L"Debug", L"debug"}}},
    {L"vkd3d-log", L"VKD3D_DEBUG", L"\uE7F4", L"D3D12 messages", L"What vkd3d-proton prints, on a program's console (the tests show it)",
     {{L"Default", L""}, {L"None", L"none"}, {L"Errors", L"err"}, {L"Fixmes", L"fixme"}, {L"Warnings", L"warn"},
      {L"Information", L"info"}, {L"Trace", L"trace"}}},
};
constexpr int kLevelCount = int(std::size(kLevels));

// The pointer settings into `values`; false when the VirtIO GPU does not run Venera's driver (they keep their
// defaults).
bool readPointer(DWORD* values)
{
    HKEY key = openDriverKey(KEY_READ);
    for (int i = 0; i < kPointerCount; i++) {
        DWORD size = sizeof(DWORD);
        if (!key || RegGetValueW(key, nullptr, kPointer[i].name, RRF_RT_REG_DWORD, nullptr, &values[i], &size) !=
                        ERROR_SUCCESS)
            values[i] = kPointer[i].fallback;
    }
    if (!key)
        return false;
    RegCloseKey(key);
    return true;
}

bool writePointer(int i, DWORD v)
{
    HKEY key = openDriverKey(KEY_SET_VALUE);
    if (!key)
        return false;
    bool ok = RegSetValueExW(key, kPointer[i].name, 0, REG_DWORD, reinterpret_cast<BYTE*>(&v), sizeof(v)) ==
              ERROR_SUCCESS;
    RegCloseKey(key);
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);  // a new shape, which the driver takes with the change
    return ok;
}

// The choice a level setting has now.
int readLevel(int i)
{
    std::wstring v = readSetting(kLevels[i].name);
    for (size_t c = 0; c < kLevels[i].choices.size(); c++)
        if (v == kLevels[i].choices[c].second)
            return int(c);
    return 0;
}

// The log file on (kLogFile) or off; DXVK's files, one per program, go to its folder.
void writeLogFile(bool on)
{
    if (on)
        CreateDirectoryW(L"C:\\venera", nullptr);
    writeSetting(L"VENERA_LOG", on ? kLogFile : L"");
    writeSetting(L"DXVK_LOG_PATH", on ? L"C:\\venera\\" : L"");
}

class Settings : public Page {
public:
    explicit Settings(HWND window) : window_(window) { load(); }
    const wchar_t* title() const override { return L"Settings"; }
    const wchar_t* icon() const override { return L"\uE713"; }
    void draw(Ui& ui, float x, float w, float& y) override;
    void click(int id) override;

private:
    enum : int { kElevateId = kPageId, kDwmId, kLogFileId, kVerboseId, kPointerId = kPageId + 20,
                 kLevelId = kPageId + 50 };

    void load();
    void infoBar(Ui& ui, float x, float w, float& y, const std::wstring& text, const wchar_t* action, int id);

    HWND window_;
    bool admin_ = elevated();
    bool driver_ = false;  // the VirtIO GPU runs Venera's driver: the Pointer settings are there
    DWORD pointer_[kPointerCount] = {};
    bool changed_ = false;  // logging, since this panel started: running programs still have the old settings
    std::wstring logFile_;  // VENERA_LOG, empty when off
    bool verbose_ = false;
    int levels_[kLevelCount] = {};
};

void Settings::load()
{
    driver_ = readPointer(pointer_);
    logFile_ = readSetting(L"VENERA_LOG");
    verbose_ = readSetting(L"VENERA_VERBOSE") == L"1";
    for (int i = 0; i < kLevelCount; i++)
        levels_[i] = readLevel(i);
}

// An InfoBar of informational severity: a sentence, and a button.
void Settings::infoBar(Ui& ui, float x, float w, float& y, const std::wstring& text, const wchar_t* action, int id)
{
    D2D1_RECT_F r = {x, y, x + w, y + 48};
    ui.fill(r, 4, kWhite, 0.0326f);
    ui.stroke(r, 4, kBlack, 0.1f);
    ui.text(L"\uE946", Ui::kIcon, {x + 16, y, x + 36, y + 48}, accent, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    float right = x + w - 16 - ui.buttonWidth(action) - 12;
    ui.button(action, right + 12, y + 8, id, false);
    ui.text(text, Ui::kBody, {x + 48, y, right, y + 48});
    y += 48 + 4;
}

void Settings::draw(Ui& ui, float x, float w, float& y)
{
    if (!admin_)
        infoBar(ui, x, w, y, L"Restart as administrator to change these.", L"Restart as administrator", kElevateId);

    float cy;
    if (driver_) {
        ui.sectionHeader(L"Pointer", x, y);
        for (int i = 0; i < kPointerCount; i++) {
            const PointerSetting& p = kPointer[i];
            cy = ui.settingsCard(x, y, w, kCardH, p.icon, p.title, p.desc);
            ui.toggle(x + w - 16, cy, kPointerId + i, pointer_[i] != 0, admin_ && (i == 0 || pointer_[0]));
            y += kCardH + kGap;
        }
    }

    ui.sectionHeader(L"Logging", x, y);
    if (admin_ && changed_)
        infoBar(ui, x, w, y,
                L"Programs started from now on use these. Restart the Desktop Window Manager for the desktop's own.",
                L"Restart it", kDwmId);
    cy = ui.settingsCard(x, y, w, kCardH, L"\uE8A5", L"Log file",
                         logFile_.empty() ? L"The drivers also write what they print to a file"
                                          : L"The drivers also write what they print to " + logFile_);
    ui.toggle(x + w - 16, cy, kLogFileId, !logFile_.empty(), admin_);
    y += kCardH + kGap;
    cy = ui.settingsCard(x, y, w, kCardH, L"\uE9D9", L"Verbose",
                         L"Also every answer about format support and multisampling. Slows programs down.");
    ui.toggle(x + w - 16, cy, kVerboseId, verbose_, admin_);
    y += kCardH + kGap;
    for (int i = 0; i < kLevelCount; i++) {
        const Level& l = kLevels[i];
        std::wstring desc = l.desc;
        if (desc.empty())  // DXVK writes a file per program, beside the driver's log file or in the program's folder
            desc = L"What DXVK prints, to <program>_d3d11.log in " +
                   (logFile_.empty() ? std::wstring(L"the program's folder")
                                     : logFile_.substr(0, logFile_.find_last_of(L'\\')));
        std::vector<std::wstring> labels;
        for (const auto& c : l.choices)
            labels.push_back(c.first);
        cy = ui.settingsCard(x, y, w, kCardH, l.icon, l.title, desc);
        ui.comboBox(x + w - 16, cy, kLevelId + i, labels, levels_[i], admin_);
        y += kCardH + kGap;
    }
}

void Settings::click(int id)
{
    if (id == kElevateId) {
        restartElevated(window_, 1);
    } else if (id >= kPointerId && id < kPointerId + kPointerCount) {
        int i = id - kPointerId;
        if (writePointer(i, !pointer_[i]))
            pointer_[i] = !pointer_[i];
    } else if (id == kDwmId) {
        restartDwm();
        changed_ = false;
    } else if (id == kLogFileId) {
        logFile_ = logFile_.empty() ? kLogFile : L"";
        writeLogFile(!logFile_.empty());
        changed_ = true;
    } else if (id == kVerboseId) {
        verbose_ = !verbose_;
        writeSetting(L"VENERA_VERBOSE", verbose_ ? L"1" : L"");
        changed_ = true;
    } else if (id >= kLevelId && id < kLevelId + kLevelCount) {
        // The choices in a menu, as a ComboBox's dropdown, at the pointer.
        int i = id - kLevelId;
        HMENU menu = CreatePopupMenu();
        for (size_t c = 0; c < kLevels[i].choices.size(); c++)
            AppendMenuW(menu, MF_STRING | (int(c) == levels_[i] ? MF_CHECKED : 0), c + 1, kLevels[i].choices[c].first);
        POINT pt;
        GetCursorPos(&pt);
        int c = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (c > 0 && c - 1 != levels_[i]) {
            levels_[i] = c - 1;
            writeSetting(kLevels[i].name, kLevels[i].choices[c - 1].second);
            changed_ = true;
        }
    }
}

}  // namespace

std::unique_ptr<Page> makeSettings(HWND window) { return std::make_unique<Settings>(window); }

std::vector<SettingLine> listSettings()
{
    std::vector<SettingLine> out;
    DWORD pointer[kPointerCount];
    if (readPointer(pointer))
        for (int i = 0; i < kPointerCount; i++)
            out.push_back({kPointer[i].cli, pointer[i] ? L"on" : L"off", L"on, off", kPointer[i].title});
    std::wstring logFile = readSetting(L"VENERA_LOG");
    out.push_back({L"log-file", logFile.empty() ? L"off" : L"on", L"on, off",
                   L"Log file" + (logFile.empty() ? std::wstring() : L", " + logFile)});
    out.push_back({L"verbose", readSetting(L"VENERA_VERBOSE") == L"1" ? L"on" : L"off", L"on, off", L"Verbose"});
    for (int i = 0; i < kLevelCount; i++) {
        std::wstring values;
        for (const auto& c : kLevels[i].choices)
            values += (values.empty() ? L"" : L", ") + std::wstring(*c.second ? c.second : L"default");
        const wchar_t* v = kLevels[i].choices[readLevel(i)].second;
        out.push_back({kLevels[i].cli, *v ? v : L"default", values, kLevels[i].title});
    }
    return out;
}

std::wstring changeSetting(const std::wstring& name, const std::wstring& value, bool* logging)
{
    std::vector<SettingLine> all = listSettings();
    auto s = std::find_if(all.begin(), all.end(), [&](const SettingLine& l) { return l.name == name; });
    if (s == all.end())
        return L"No setting named " + name;
    if (value.empty() || value.find(L',') != std::wstring::npos ||
        (L", " + s->values + L",").find(L", " + value + L",") == std::wstring::npos)
        return name + L" takes " + s->values;
    if (!elevated())
        return L"Changing settings needs administrator rights";
    bool on = value == L"on";
    for (int i = 0; i < kPointerCount; i++)
        if (name == kPointer[i].cli)
            return writePointer(i, on) ? L"" : L"Could not change " + name;
    *logging = true;
    if (name == L"log-file")
        writeLogFile(on);
    else if (name == L"verbose")
        writeSetting(L"VENERA_VERBOSE", on ? L"1" : L"");
    for (int i = 0; i < kLevelCount; i++)
        if (name == kLevels[i].cli)
            writeSetting(kLevels[i].name, value == L"default" ? L"" : value);
    return L"";
}

// Windows starts DWM again at once, and it loads the drivers afresh. DWM runs as its own user (DWM-n), whose process
// administrators may end only with the debug privilege, as taskkill does.
void restartDwm()
{
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
        TOKEN_PRIVILEGES tp = {1};
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(token);
    }
    DWORD session = 0, s;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"dwm.exe") || !ProcessIdToSessionId(e.th32ProcessID, &s) || s != session)
            continue;
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, e.th32ProcessID)) {
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
}

}  // namespace panel
