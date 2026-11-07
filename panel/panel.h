// SPDX-License-Identifier: MIT
// vgd-panel.exe, the control panel: a Win32 window painted with Direct2D on the CPU (a software render target, so it
// never needs our driver) and DirectWrite text, in WinUI 3's dark theme: its colour tokens, a left NavigationView and
// Settings-style cards. Everything is laid out in 96-DPI units and scaled to the window's DPI.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tests/tests.h"

struct _EVENT_RECORD;

namespace panel {

// ---- WinUI 3 dark theme tokens (alpha fills are drawn as alpha) ---------------------------------------------------
namespace theme {
constexpr UINT32 kWhite = 0xffffff, kBlack = 0x000000;
constexpr UINT32 kBase = 0x202020;   // SolidBackgroundFillColorBase (Mica's fallback)
constexpr UINT32 kLayer = 0x282828;  // LayerFillColorDefault (#3A3A3A at 30 %) over the base
constexpr float kTextPrimary = 1, kTextSecondary = 0.786f, kTextTertiary = 0.544f;
constexpr float kCardFill = 0.0512f, kCardHover = 0.0837f;  // CardBackgroundFillColorDefault, and pointer over
constexpr float kControl = 0.0605f, kControlHover = 0.0837f, kControlPressed = 0.0326f, kControlStroke = 0.0698f;
constexpr float kStrongStroke = 0.544f;  // ControlStrongStrokeColorDefault
constexpr float kTextDisabled = 0.3628f, kControlDisabled = 0.0419f, kStrokeDisabled = 0.2465f;
constexpr UINT32 kSuccess = 0x6ccb5f, kCaution = 0xfce100, kCritical = 0xff99a4;
constexpr UINT32 kCautionBg = 0x433519, kCriticalBg = 0x442726;
extern UINT32 accent;      // SystemAccentColorLight2, from Windows' accent palette
extern UINT32 accentBase;  // SystemAccentColor: the text selection highlight
}  // namespace theme

// A clickable thing's id: the window owns kNavId..kPageId-1, pages number theirs from kPageId.
constexpr int kNoId = 0, kNavId = 100, kPageId = 1000;

// The painter and the controls drawn with it. Pages draw in DIPs through it and register what can be clicked.
class Ui {
public:
    enum Font { kTitle, kSubtitle, kBody, kBodyStrong, kCaption, kMono, kIcon, kIconLarge, kIconSmall, kGlyph, kArrow,
                kFontCount };

    void init();
    void begin(ID2D1RenderTarget* target)
    {
        rt_ = target;
        hots_.clear();
        selectables_.clear();
        runs_.clear();
        scrollArea = {};
        clip_ = kNoClip;
    }
    ID2D1RenderTarget* rt() const { return rt_; }

    // Drawing; colours are 0xRRGGBB with an alpha.
    void color(UINT32 rgb, float a = 1);
    void fill(D2D1_RECT_F r, float radius, UINT32 c, float a = 1);
    void stroke(D2D1_RECT_F r, float radius, UINT32 c, float a = 1);
    void text(const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c = theme::kWhite, float a = theme::kTextPrimary,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float measure(const std::wstring& s, Font f);

    // Clickable areas, in window DIPs; `offset` is the scroll offset of what is being drawn.
    void hot(D2D1_RECT_F r, int id);
    int hit(float x, float y) const;
    float offset = 0, view = 0;  // view: the height of what is shown, from offset
    int hover = kNoId, pressed = kNoId;

    // A part of a page that fills the window (Page::fills) and scrolls by itself, like a log's lines: what is drawn
    // between beginScroll and endScroll is clipped to r, and moved up by `scroll`, which the window keeps; `height` is
    // all of it. Clicks and text there are only where it shows.
    void beginScroll(D2D1_RECT_F r, float height);
    void endScroll();
    float scroll = 0;
    D2D1_RECT_F scrollArea = {};  // what the last frame scrolled, in window DIPs, and its height
    float scrollHeight = 0;

    // WinUI controls. Buttons are 32 high; the toggle and the combo box are placed by their right edge, and are greyed
    // out and not clickable when not `enabled`.
    float button(const std::wstring& s, float x, float y, int id, bool accent);
    float buttonWidth(const std::wstring& s);
    void toggle(float right, float cy, int id, bool on, bool enabled = true);
    void comboBox(float right, float cy, int id, const std::vector<std::wstring>& options, int selected,
                  bool enabled = true);
    // A Settings card (68 high): icon, title, description; returns the vertical centre for the control on its right.
    float settingsCard(float x, float y, float w, float h, const wchar_t* icon, const std::wstring& title,
                       const std::wstring& desc, int clickId = kNoId, UINT32 iconColor = theme::kWhite,
                       float iconAlpha = 1);
    void card(D2D1_RECT_F r, bool hover = false);
    void sectionHeader(const std::wstring& s, float x, float& y);
    // A 32 x 32 subtle button with a glyph: no fill until the pointer is over it.
    void iconButton(const wchar_t* glyph, float x, float y, int id);
    // A ProgressRing `size` across: `value` from 0 to 1, or below 0 for the indeterminate one (keep painting frames).
    void progressRing(float cx, float cy, float size, float value);

    // Selectable text (a TextBlock with IsTextSelectionEnabled): drawn like text(), and the window lets the user select
    // in it with the mouse and copy. One selection at a time; `id` names the text across frames. Texts with the same
    // `run` (not 0) are lines of one text, like a log's: a selection can go from one to another, in the order of their
    // ids, which grow down the page.
    void selectable(int id, const std::wstring& s, Font f, D2D1_RECT_F r, UINT32 c = theme::kWhite,
                    float a = theme::kTextPrimary, int run = 0);
    // A run's lines by id, also those not drawn (scrolled away), for copying; false for an id that is not a line.
    void runText(int run, std::function<bool(int id, std::wstring* s)> text) { runs_[run] = std::move(text); }
    // The selectable text under a point (window DIPs) and the character position there.
    bool textAt(float x, float y, int* id, UINT32* pos) const;
    void selectStart(int id, UINT32 pos);
    void selectTo(float x, float y);  // drag: moves the caret of the current selection towards the point
    void selectWord(int id, UINT32 pos);
    void selectAll(int id);
    void clearSelection() { selId_ = caretId_ = kNoId; }
    // `id` is in the selection, which is not empty.
    bool selected(int id) const;
    int selectionId() const { return selId_; }
    std::wstring selectedText() const;

    // The render target and its brush, made per window (they go when the target has to be recreated).
    void setBrush(ID2D1SolidColorBrush* b) { brush_ = b; }

private:
    IDWriteTextFormat* format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight);
    bool hasFont(const wchar_t* family);
    IDWriteTextLayout* layout(const std::wstring& s, Font f, D2D1_RECT_F r) const;
    struct Selectable { D2D1_RECT_F r; int id; std::wstring s; Font f; int run; D2D1_RECT_F clip; };
    const Selectable* findSelectable(int id) const;
    void select(int id, UINT32 anchor, UINT32 caret);
    void ends(int* firstId, UINT32* firstPos, int* lastId, UINT32* lastPos) const;
    bool part(int id, int run, UINT32 length, UINT32* start, UINT32* end) const;
    void arc(float cx, float cy, float r, float from, float to, float thickness);
    ID2D1StrokeStyle* round_ = nullptr;  // round caps, for arcs

    struct Hot { D2D1_RECT_F r; int id; };
    ID2D1RenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    IDWriteTextFormat* fonts_[kFontCount] = {};
    static constexpr D2D1_RECT_F kNoClip = {-1e9f, -1e9f, 1e9f, 1e9f};
    D2D1_RECT_F clip_ = kNoClip;  // where clicks and text are, in window DIPs
    D2D1_MATRIX_3X2_F unscrolled_ = {};
    std::vector<Hot> hots_;
    std::vector<Selectable> selectables_;  // as drawn in the last frame
    // The selection: from the anchor (where it started) to the caret, each a text and a position in it.
    int selId_ = kNoId, caretId_ = kNoId, selRun_ = 0;
    UINT32 selAnchor_ = 0, selCaret_ = 0;
    std::map<int, std::function<bool(int, std::wstring*)>> runs_;
};

// Puts text on the clipboard.
void setClipboard(HWND owner, const std::wstring& text);

// A page of the panel. The window scrolls it and tells it about clicks; it repaints after each.
class Page {
public:
    virtual ~Page() = default;
    virtual const wchar_t* title() const = 0;
    virtual const wchar_t* icon() const = 0;
    // Draws from the top of the page's content at y, w wide; leaves y below what it drew.
    virtual void draw(Ui& ui, float x, float w, float& y) = 0;
    virtual void click(int id) {}
    // The display configuration or a system setting changed.
    virtual void refresh() {}
    // A message the window does not handle itself (kPageMessage.., timers from kPageTimer); true when it was this
    // page's. The window repaints after it.
    virtual bool message(UINT msg, WPARAM wp, LPARAM lp) { return false; }
    // The page fills the window, to its bottom, and scrolls a part of itself (Ui::beginScroll) instead of all of it.
    virtual bool fills() const { return false; }
    // The page grows at its end (a log): while the view is at the end, it stays there.
    virtual bool followsEnd() const { return false; }
    // Something on the page moves (a ProgressRing), so the window paints frame after frame while it is shown.
    virtual bool animating() const { return false; }
};

// Messages and timer ids the window passes on to its pages.
constexpr UINT kPageMessage = WM_APP + 1;
constexpr UINT_PTR kPageTimer = 100;

std::unique_ptr<Page> makeOverview(HWND window);
std::unique_ptr<Page> makeSettings(HWND window);
std::unique_ptr<Page> makeLogs(HWND window);
std::unique_ptr<Page> makeTests(HWND window);
std::unique_ptr<Page> makeTools();

// ---- what the panel reads about the system (sysinfo.cpp) ----------------------------------------------------------

struct Display {
    DWORD width = 0, height = 0, hz = 0;
    UINT scale = 100;  // percent
};

// Read in the panel's own process: the registry, SetupAPI, the displays and the firmware's SMBIOS tables. Nothing
// here loads a graphics driver, so it works whatever state the driver is in.
struct SystemInfo {
    // The VirtIO GPU and the driver Windows runs it with.
    bool deviceFound = false;
    std::wstring driverDesc, provider, driverVersion, driverDate;
    bool ours = false;            // the driver is vgd's package
    ULONG problem = 0;            // Device Manager's problem code, 0 for none
    // vgd as Apps & features lists it.
    std::wstring productVersion, installDate;
    bool testSigning = false;
    // The guest. SMBIOS names the virtual machine (manufacturer and product) and its machine type, e.g. QEMU's
    // pc-q35-11.1: QEMU's own version is not visible to a guest, the machine type's is the nearest.
    std::wstring machine, machineType;
    std::wstring windows;  // edition, release and build
    std::wstring cpu;
    DWORD sockets = 0, cores = 0, threads = 0;
    ULONGLONG memory = 0, usableMemory = 0, swap = 0;  // bytes
    std::vector<std::wstring> mice, keyboards;
    std::vector<Display> displays;
};
SystemInfo readSystemInfo();
std::vector<Display> readDisplays(const std::wstring& adapterName);
// The VirtIO GPU's driver key (where pkg/vgd.inf puts the KMD's settings) when it runs vgd's driver, else null.
HKEY openDriverKey(REGSAM access);

// ---- the graphics probe (probe.cpp) -------------------------------------------------------------------------------
// `vgd-panel.exe /probe` creates a Vulkan instance and D3D12, D3D11 and OpenGL devices on the VirtIO GPU and prints
// what it found as key=value lines. It runs as a child process, so a driver that crashes or hangs takes only the
// probe down. Each API's lines are written before the next is tried.
int runProbe();
using ProbeResult = std::map<std::string, std::wstring>;
// Runs the probe and waits for it. A probe that does not finish in `timeoutMs` is ended, and the result has
// "timeout".
ProbeResult probeNow(DWORD timeoutMs);
// The same on a thread; posts `msg` to `window` with a ProbeResult* (the receiver deletes it) when it is done.
void startProbe(HWND window, UINT msg, DWORD timeoutMs);

// Runs this exe again with `args` and collects what it prints. One that runs longer than `timeoutMs`, or until
// `cancel` is set, is ended, with whatever it started itself.
struct ChildResult {
    bool started = false, timedOut = false, cancelled = false;
    DWORD exitCode = 0;
    std::string output;
};
ChildResult runSelf(const std::wstring& args, DWORD timeoutMs, HANDLE cancel = nullptr);

// ---- the tests (tests/) ------------------------------------------------------------------------------------------

struct TestResult {
    bool ok = false, stopped = false;
    float seconds = 0;
    std::wstring summary;  // why it failed
    std::wstring output;
};
// A test by its full id, "<group>.<test>".
const Test* findTest(const std::string& id);
// Expands "all", group ids and full ids into full ids, in the panel's order; false with the first name it does not
// know in *unknown.
bool selectTests(const std::vector<std::string>& names, std::vector<std::string>* ids, std::string* unknown);
// Runs a test in a child process and waits for it.
TestResult runTest(const std::string& id, HANDLE cancel);
// The child: `/run-test <id>`.
int runTestChild(const std::string& id);

// ---- what the drivers log (logcapture.cpp) -------------------------------------------------------------------------

struct LogLine {
    SYSTEMTIME time;  // local
    DWORD pid = 0;
    std::wstring process;    // the program without .exe, or "kernel"
    std::wstring component;  // what logged it, for the drivers' lines: a UMD (vgd3d12), "mesa" or "kernel"
    int level = 0;           // 0 information, 1 warning, 2 error
    std::wstring text;
};

// Captures debug output (OutputDebugString) from this session, and with administrator rights from services and
// the kernel's (DbgPrint) too, as DebugView does. Lines reach the sink on the capture's threads, one at a time.
class LogCapture {
public:
    ~LogCapture();
    void start(std::function<void(LogLine&&)> sink);
    void stop();
    bool running() const { return running_; }
    // Why a source did not start, after start(); empty when it did.
    std::wstring userError, kernelError;

private:
    struct Dbwin {
        HANDLE bufferReady = nullptr, dataReady = nullptr, section = nullptr;
        const char* view = nullptr;
        std::thread thread;
    };
    bool openDbwin(Dbwin& d, const wchar_t* prefix, std::wstring* error);
    void closeDbwin(Dbwin& d);
    void readDbwin(Dbwin& d);
    bool startKernel(std::wstring* error);
    void stopKernel();
    void kernelEvent(_EVENT_RECORD* r);
    std::wstring processName(DWORD pid);
    void emit(LogLine&& l);

    bool running_ = false;
    HANDLE stop_ = nullptr;
    Dbwin local_, global_;
    ULONG64 session_ = 0, trace_ = ~0ull;
    std::thread kernelThread_;
    std::function<void(LogLine&&)> sink_;
    std::mutex sinkMutex_, namesMutex_;
    std::map<DWORD, std::wstring> names_;
};
// A line as /log prints it and the Logs page exports it: time, program[pid], component, level (I/W/E), text.
std::wstring formatLogLine(const LogLine& l);
// The process runs as administrator.
bool elevated();
// Starts the panel again as administrator, on page `page`, and closes this window; false when that did not start.
bool restartElevated(HWND window, int page);

// The settings (settingspage.cpp), also for the command line: by its name there, its value and the values it takes.
struct SettingLine {
    std::wstring name, value, values, title;
};
std::vector<SettingLine> listSettings();
// Changes a setting; returns why it could not, else empty. *logging is set when the change is for programs started
// afterwards.
std::wstring changeSetting(const std::wstring& name, const std::wstring& value, bool* logging);
// Ends this session's Desktop Window Manager, which Windows starts again with the drivers' current settings.
void restartDwm();

// ---- the system report (report.cpp): what the Overview shows and /info prints ---------------------------------------

struct InfoRow {
    std::wstring key, value;
    bool bad = false;  // something that does not work
};
struct InfoSection {
    const wchar_t* icon;
    std::wstring title;
    std::vector<InfoRow> rows;
};
// `probe` null: the probe is still running.
std::vector<InfoSection> describe(const SystemInfo& info, const ProbeResult* probe);
std::wstring reportText(const std::vector<InfoSection>& sections);
bool anyProblem(const std::vector<InfoSection>& sections);
// The host GPU and its Vulkan driver from Venus' device name, "Virtio-GPU Venus (<GPU> (<driver>))".
void hostGpu(const ProbeResult* probe, std::wstring& gpu, std::wstring& driver);

// ---- the command line (cli.cpp) -------------------------------------------------------------------------------------
// Runs the command in argv when there is one, and returns its exit code; returns -1 when there is none (the window),
// with *page set from /page.
int runCommandLine(int argc, wchar_t** argv, int* page);

// UTF-8 / UTF-16.
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

}  // namespace panel
